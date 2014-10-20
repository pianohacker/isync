/*
 * mbsync - mailbox synchronizer
 * Copyright (C) 2000-2002 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 2002-2006,2008,2010,2011, 2013 Oswald Buddenhagen <ossi@users.sf.net>
 * Copyright (C) 2004 Theodore Y. Ts'o <tytso@mit.edu>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, mbsync may be linked with the OpenSSL library,
 * despite that library's more restrictive license.
 */

#include "socket.h"

#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#ifdef HAVE_LIBSSL
# include <openssl/ssl.h>
# include <openssl/err.h>
# include <openssl/hmac.h>
# include <openssl/x509v3.h>
#endif

enum {
	SCK_CONNECTING,
#ifdef HAVE_LIBSSL
	SCK_STARTTLS,
#endif
	SCK_READY
};

static void
socket_fail( conn_t *conn )
{
	conn->bad_callback( conn->callback_aux );
}

#ifdef HAVE_LIBSSL
static int
ssl_return( const char *func, conn_t *conn, int ret )
{
	int err;

	switch ((err = SSL_get_error( conn->ssl, ret ))) {
	case SSL_ERROR_NONE:
		return ret;
	case SSL_ERROR_WANT_WRITE:
		conf_fd( conn->fd, POLLIN, POLLOUT );
		/* fallthrough */
	case SSL_ERROR_WANT_READ:
		return 0;
	case SSL_ERROR_SYSCALL:
	case SSL_ERROR_SSL:
		if (!(err = ERR_get_error())) {
			if (ret == 0)
				error( "Socket error: secure %s %s: unexpected EOF\n", func, conn->name );
			else
				sys_error( "Socket error: secure %s %s", func, conn->name );
		} else {
			error( "Socket error: secure %s %s: %s\n", func, conn->name, ERR_error_string( err, 0 ) );
		}
		break;
	default:
		error( "Socket error: secure %s %s: unhandled SSL error %d\n", func, conn->name, err );
		break;
	}
	if (conn->state == SCK_STARTTLS)
		conn->callbacks.starttls( 0, conn->callback_aux );
	else
		socket_fail( conn );
	return -1;
}

/* Some of this code is inspired by / lifted from mutt. */

static int
host_matches( const char *host, const char *pattern )
{
	if (pattern[0] == '*' && pattern[1] == '.') {
		pattern += 2;
		if (!(host = strchr( host, '.' )))
			return 0;
		host++;
	}

	return *host && *pattern && !strcasecmp( host, pattern );
}

static int
verify_hostname( X509 *cert, const char *hostname )
{
	int i, len, found;
	X509_NAME *subj;
	STACK_OF(GENERAL_NAME) *subj_alt_names;
	char cname[1000];

	/* try the DNS subjectAltNames */
	found = 0;
	if ((subj_alt_names = X509_get_ext_d2i( cert, NID_subject_alt_name, NULL, NULL ))) {
		int num_subj_alt_names = sk_GENERAL_NAME_num( subj_alt_names );
		for (i = 0; i < num_subj_alt_names; i++) {
			GENERAL_NAME *subj_alt_name = sk_GENERAL_NAME_value( subj_alt_names, i );
			if (subj_alt_name->type == GEN_DNS &&
			    strlen( (const char *)subj_alt_name->d.ia5->data ) == (size_t)subj_alt_name->d.ia5->length &&
			    host_matches( hostname, (const char *)(subj_alt_name->d.ia5->data) ))
			{
				found = 1;
				break;
			}
		}
		sk_GENERAL_NAME_pop_free( subj_alt_names, GENERAL_NAME_free );
	}
	if (found)
		return 0;

	/* try the common name */
	if (!(subj = X509_get_subject_name( cert ))) {
		error( "Error, cannot get certificate subject\n" );
		return -1;
	}
	if ((len = X509_NAME_get_text_by_NID( subj, NID_commonName, cname, sizeof(cname) )) < 0) {
		error( "Error, cannot get certificate common name\n" );
		return -1;
	}
	if (strlen( cname ) == (size_t)len && host_matches( hostname, cname ))
		return 0;

	error( "Error, certificate owner does not match hostname %s\n", hostname );
	return -1;
}

static int
verify_cert_host( const server_conf_t *conf, conn_t *sock )
{
	unsigned i;
	long err;
	X509 *cert;
	STACK_OF(X509_OBJECT) *trusted;

	cert = SSL_get_peer_certificate( sock->ssl );
	if (!cert) {
		error( "Error, no server certificate\n" );
		return -1;
	}

	trusted = SSL_CTX_get_cert_store( conf->SSLContext )->objs;
	for (i = 0; i < conf->num_trusted; i++) {
		if (!X509_cmp( cert, sk_X509_OBJECT_value( trusted, i )->data.x509 ))
			return 0;
	}

	err = SSL_get_verify_result( sock->ssl );
	if (err != X509_V_OK) {
		error( "SSL error connecting %s: %s\n", sock->name, ERR_error_string( err, NULL ) );
		return -1;
	}

	if (!conf->host) {
		error( "SSL error connecting %s: Neither host nor matching certificate specified\n", sock->name );
		return -1;
	}

	return verify_hostname( cert, conf->host );
}

static int
init_ssl_ctx( const server_conf_t *conf )
{
	server_conf_t *mconf = (server_conf_t *)conf;
	int options = 0;

	if (conf->SSLContext)
		return conf->ssl_ctx_valid;

	mconf->SSLContext = SSL_CTX_new( SSLv23_client_method() );

	if (!(conf->ssl_versions & SSLv2))
		options |= SSL_OP_NO_SSLv2;
	if (!(conf->ssl_versions & SSLv3))
		options |= SSL_OP_NO_SSLv3;
	if (!(conf->ssl_versions & TLSv1))
		options |= SSL_OP_NO_TLSv1;
#ifdef SSL_OP_NO_TLSv1_1
	if (!(conf->ssl_versions & TLSv1_1))
		options |= SSL_OP_NO_TLSv1_1;
#endif
#ifdef SSL_OP_NO_TLSv1_2
	if (!(conf->ssl_versions & TLSv1_2))
		options |= SSL_OP_NO_TLSv1_2;
#endif

	SSL_CTX_set_options( mconf->SSLContext, options );

	if (conf->cert_file && !SSL_CTX_load_verify_locations( mconf->SSLContext, conf->cert_file, 0 )) {
		error( "Error while loading certificate file '%s': %s\n",
		       conf->cert_file, ERR_error_string( ERR_get_error(), 0 ) );
		return 0;
	}
	mconf->num_trusted = sk_X509_OBJECT_num( SSL_CTX_get_cert_store( mconf->SSLContext )->objs );
	if (mconf->system_certs && !SSL_CTX_set_default_verify_paths( mconf->SSLContext ))
		warn( "Warning: Unable to load default certificate files: %s\n",
		      ERR_error_string( ERR_get_error(), 0 ) );

	SSL_CTX_set_verify( mconf->SSLContext, SSL_VERIFY_NONE, NULL );

	mconf->ssl_ctx_valid = 1;
	return 1;
}

static void start_tls_p2( conn_t * );
static void start_tls_p3( conn_t *, int );

void
socket_start_tls( conn_t *conn, void (*cb)( int ok, void *aux ) )
{
	static int ssl_inited;

	conn->callbacks.starttls = cb;

	if (!ssl_inited) {
		SSL_library_init();
		SSL_load_error_strings();
		ssl_inited = 1;
	}

	if (!init_ssl_ctx( conn->conf )) {
		start_tls_p3( conn, 0 );
		return;
	}

	conn->ssl = SSL_new( ((server_conf_t *)conn->conf)->SSLContext );
	SSL_set_fd( conn->ssl, conn->fd );
	SSL_set_mode( conn->ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER );
	conn->state = SCK_STARTTLS;
	start_tls_p2( conn );
}

static void
start_tls_p2( conn_t *conn )
{
	if (ssl_return( "connect to", conn, SSL_connect( conn->ssl ) ) > 0) {
		if (verify_cert_host( conn->conf, conn )) {
			start_tls_p3( conn, 0 );
		} else {
			info( "Connection is now encrypted\n" );
			start_tls_p3( conn, 1 );
		}
	}
}

static void start_tls_p3( conn_t *conn, int ok )
{
	conn->state = SCK_READY;
	conn->callbacks.starttls( ok, conn->callback_aux );
}

#endif /* HAVE_LIBSSL */

#ifdef HAVE_LIBZ
static voidpf _socket_zalloc( voidpf opaque ATTR_UNUSED, uInt items, uInt size )
{
	return nfmalloc( items * size );
}

static void _socket_zfree( voidpf opaque ATTR_UNUSED, voidpf address )
{
	free( address );
}

void socket_start_deflate( conn_t *conn )
{
	int result;

	if (conn->in_z) return;

	conn->in_z = nfcalloc( sizeof( *conn->in_z ) );
	conn->in_z->zalloc = _socket_zalloc;
	conn->in_z->zfree = _socket_zfree;
	result = inflateInit2(
		conn->in_z,
		-8 /* Use raw deflate */
	);

	if (result != Z_OK) {
		error( "IMAP error: Cannot start compression for server '%s': %s\n", conn->conf->host, conn->in_z->msg );
	}

	conn->out_z = nfcalloc( sizeof( *conn->out_z ) );
	conn->out_z->zalloc = _socket_zalloc;
	conn->out_z->zfree = _socket_zfree;
	result = deflateInit2(
		conn->out_z,
		Z_DEFAULT_COMPRESSION, /* Compression level */
		Z_DEFLATED, /* Only valid value */
		-8, /* Use raw deflate */
		8, /* Default memory usage */
		Z_DEFAULT_STRATEGY /* Don't try to do anything fancy */
	);

	if (result != Z_OK) {
		error( "IMAP error: Cannot start compression for server '%s': %s\n", conn->conf->host, conn->out_z->msg );
	}
}
#endif /* HAVE_LIBZ */

static void socket_fd_cb( int, void * );

static void socket_connect_one( conn_t * );
static void socket_connect_failed( conn_t * );
static void socket_connected( conn_t * );
static void socket_connect_bail( conn_t * );

static void
socket_close_internal( conn_t *sock )
{
	del_fd( sock->fd );
	close( sock->fd );
	sock->fd = -1;
}

void
socket_connect( conn_t *sock, void (*cb)( int ok, void *aux ) )
{
	const server_conf_t *conf = sock->conf;

	sock->callbacks.connect = cb;

	/* open connection to server */
	if (conf->tunnel) {
		int a[2];

		nfasprintf( &sock->name, "tunnel '%s'", conf->tunnel );
		infon( "Starting %s... ", sock->name );

		if (socketpair( PF_UNIX, SOCK_STREAM, 0, a )) {
			perror( "socketpair" );
			exit( 1 );
		}

		if (fork() == 0) {
			if (dup2( a[0], 0 ) == -1 || dup2( a[0], 1 ) == -1)
				_exit( 127 );
			close( a[0] );
			close( a[1] );
			execl( "/bin/sh", "sh", "-c", conf->tunnel, (char *)0 );
			_exit( 127 );
		}

		close( a[0] );
		sock->fd = a[1];

		fcntl( a[1], F_SETFL, O_NONBLOCK );
		add_fd( a[1], socket_fd_cb, sock );

		info( "\vok\n" );
		socket_connected( sock );
	} else {
#ifdef HAVE_IPV6
		int gaierr;
		struct addrinfo hints;

		memset( &hints, 0, sizeof(hints) );
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_ADDRCONFIG;
		infon( "Resolving %s... ", conf->host );
		if ((gaierr = getaddrinfo( conf->host, NULL, &hints, &sock->addrs ))) {
			error( "Error: Cannot resolve server '%s': %s\n", conf->host, gai_strerror( gaierr ) );
			socket_connect_bail( sock );
			return;
		}
		info( "\vok\n" );

		sock->curr_addr = sock->addrs;
#else
		struct hostent *he;

		infon( "Resolving %s... ", conf->host );
		he = gethostbyname( conf->host );
		if (!he) {
			error( "Error: Cannot resolve server '%s': %s\n", conf->host, hstrerror( h_errno ) );
			socket_connect_bail( sock );
			return;
		}
		info( "\vok\n" );

		sock->curr_addr = he->h_addr_list;
#endif
		socket_connect_one( sock );
	}
}

static void
socket_connect_one( conn_t *sock )
{
	int s;
#ifdef HAVE_IPV6
	struct addrinfo *ai;
#else
	struct {
		struct sockaddr_in ai_addr[1];
	} ai[1];
#endif

#ifdef HAVE_IPV6
	if (!(ai = sock->curr_addr)) {
#else
	if (!*sock->curr_addr) {
#endif
		error( "No working address found for %s\n", sock->conf->host );
		socket_connect_bail( sock );
		return;
	}

#ifdef HAVE_IPV6
	if (ai->ai_family == AF_INET6) {
		struct sockaddr_in6 *in6 = ((struct sockaddr_in6 *)ai->ai_addr);
		char sockname[64];
		in6->sin6_port = htons( sock->conf->port );
		nfasprintf( &sock->name, "%s ([%s]:%hu)",
		            sock->conf->host, inet_ntop( AF_INET6, &in6->sin6_addr, sockname, sizeof(sockname) ), sock->conf->port );
	} else
#endif
	{
		struct sockaddr_in *in = ((struct sockaddr_in *)ai->ai_addr);
#ifndef HAVE_IPV6
		memset( in, 0, sizeof(*in) );
		in->sin_family = AF_INET;
		in->sin_addr.s_addr = *((int *)*sock->curr_addr);
#endif
		in->sin_port = htons( sock->conf->port );
		nfasprintf( &sock->name, "%s (%s:%hu)",
		            sock->conf->host, inet_ntoa( in->sin_addr ), sock->conf->port );
	}

#ifdef HAVE_IPV6
	s = socket( ai->ai_family, SOCK_STREAM, 0 );
#else
	s = socket( PF_INET, SOCK_STREAM, 0 );
#endif
	if (s < 0) {
		perror( "socket" );
		exit( 1 );
	}
	sock->fd = s;
	fcntl( s, F_SETFL, O_NONBLOCK );
	add_fd( s, socket_fd_cb, sock );

	infon( "Connecting to %s... ", sock->name );
#ifdef HAVE_IPV6
	if (connect( s, ai->ai_addr, ai->ai_addrlen )) {
#else
	if (connect( s, ai->ai_addr, sizeof(*ai->ai_addr) )) {
#endif
		if (errno != EINPROGRESS) {
			socket_connect_failed( sock );
			return;
		}
		conf_fd( s, 0, POLLOUT );
		sock->state = SCK_CONNECTING;
		info( "\v\n" );
		return;
	}
	info( "\vok\n" );
	socket_connected( sock );
}

static void
socket_connect_failed( conn_t *conn )
{
	sys_error( "Cannot connect to %s", conn->name );
	socket_close_internal( conn );
	free( conn->name );
	conn->name = 0;
#ifdef HAVE_IPV6
	conn->curr_addr = conn->curr_addr->ai_next;
#else
	conn->curr_addr++;
#endif
	socket_connect_one( conn );
}

static void
socket_connected( conn_t *conn )
{
#ifdef HAVE_IPV6
	freeaddrinfo( conn->addrs );
#endif
	conf_fd( conn->fd, 0, POLLIN );
	conn->state = SCK_READY;
	conn->callbacks.connect( 1, conn->callback_aux );
}

static void
socket_connect_bail( conn_t *conn )
{
#ifdef HAVE_IPV6
	freeaddrinfo( conn->addrs );
#endif
	free( conn->name );
	conn->name = 0;
	conn->callbacks.connect( 0, conn->callback_aux );
}

static void dispose_chunk( conn_t *conn );

void
socket_close( conn_t *sock )
{
	if (sock->fd >= 0)
		socket_close_internal( sock );
	free( sock->name );
	sock->name = 0;
#ifdef HAVE_LIBSSL
	if (sock->ssl) {
		SSL_free( sock->ssl );
		sock->ssl = 0;
	}
#endif
	while (sock->write_buf)
		dispose_chunk( sock );
}

static void
socket_fill( conn_t *sock )
{
	char *buf;
	int n = sock->offset + sock->bytes;
	int len = sizeof(sock->buf) - n;
	if (!len) {
		error( "Socket error: receive buffer full. Probably protocol error.\n" );
		socket_fail( sock );
		return;
	}
	assert( sock->fd >= 0 );
	buf = sock->buf + n;
#ifdef HAVE_LIBSSL
	if (sock->ssl) {
		if ((n = ssl_return( "read from", sock, SSL_read( sock->ssl, buf, len ) )) <= 0)
			return;
		if (n == len && SSL_pending( sock->ssl ))
			fake_fd( sock->fd, POLLIN );
	} else
#endif
	{
		if ((n = read( sock->fd, buf, len )) < 0) {
			sys_error( "Socket error: read from %s", sock->name );
			socket_fail( sock );
			return;
		} else if (!n) {
			error( "Socket error: read from %s: unexpected EOF\n", sock->name );
			socket_fail( sock );
			return;
		}
	}
	sock->bytes += n;
	sock->read_callback( sock->callback_aux );
}

int
socket_read( conn_t *conn, char *buf, int len )
{
	int n = conn->bytes;
	if (n > len)
		n = len;
	memcpy( buf, conn->buf + conn->offset, n );
	if (!(conn->bytes -= n))
		conn->offset = 0;
	else
		conn->offset += n;
	return n;
}

char *
socket_read_line( conn_t *b )
{
	char *p, *s;
	int n;

	s = b->buf + b->offset;
	p = memchr( s + b->scanoff, '\n', b->bytes - b->scanoff );
	if (!p) {
		b->scanoff = b->bytes;
		if (b->offset + b->bytes == sizeof(b->buf)) {
			memmove( b->buf, b->buf + b->offset, b->bytes );
			b->offset = 0;
		}
		return 0;
	}
	n = p + 1 - s;
	b->offset += n;
	b->bytes -= n;
	b->scanoff = 0;
	if (p != s && p[-1] == '\r')
		p--;
	*p = 0;
	return s;
}

#ifdef HAVE_LIBZ
static int
do_write_inner( conn_t *sock, char *buf, int len )
#else
static int
do_write( conn_t *sock, char *buf, int len )
#endif
{
	int n;

	assert( sock->fd >= 0 );
#ifdef HAVE_LIBSSL
	if (sock->ssl)
		return ssl_return( "write to", sock, SSL_write( sock->ssl, buf, len ) );
#endif
	n = write( sock->fd, buf, len );
	if (n < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			sys_error( "Socket error: write to %s", sock->name );
			socket_fail( sock );
		} else {
			n = 0;
			conf_fd( sock->fd, POLLIN, POLLOUT );
		}
	} else if (n != len) {
		conf_fd( sock->fd, POLLIN, POLLOUT );
	}
	return n;
}

#ifdef HAVE_LIBZ
static int
do_write( conn_t *sock, char *buf, int len )
{
	int result, to_write;
	int progress = 0;
	int outlen;
	unsigned char *outbuf;

	if (sock->out_z == NULL)
		return do_write_inner( sock, buf, len );

	/* Make sure that we write out any leftover compression output before we try to output more */
	if (sock->in_z_leftover_len) {
		result = do_write_inner( sock, (char*) sock->in_z_leftover, sock->in_z_leftover_len );

		if (result < 0) {
			return result;
		} else if (result < sock->in_z_leftover_len) {
			sock->in_z_leftover_len -= result;
			sock->in_z_leftover += result;

			/*
			 * Don't return the result of the write; that reflects the amount of _compressed_ output
			 * that was successfully written, and only applies to the last write attempt anyway.
			 */
			return 0;
		}
	}

	sock->out_z->next_in = (unsigned char*) buf;
	sock->out_z->avail_in = len;

	outbuf = NULL;
	outlen = 32;

	do {
		/* Have to reassign next_out each time as outbuf may move during realloc */
		outbuf = nfrealloc( outbuf, outlen );
		sock->out_z->next_out = outbuf + progress;
		sock->out_z->avail_out = outlen - progress;

		if ( deflate( sock->out_z, Z_SYNC_FLUSH ) != Z_OK ) {
			error( "Outbound compression error: %s: %s\n", sock->name, sock->out_z->msg );
			socket_fail( sock );
			return -1;
		}

		to_write = outlen - sock->out_z->avail_out;
		progress = sock->out_z->next_out - outbuf;

		outlen *= 2; /* For next pass */
	} while (sock->out_z->avail_out == 0);

	result = do_write_inner( sock, (char*) outbuf, to_write );

	if ( result > 0 && result < to_write ) {
		sock->in_z_leftover = outbuf + result;
		sock->in_z_leftover_len = to_write - result;

		return len;
	}

	free( outbuf );

	if ( result < 0 ) {
		return result;
	} else {
		return len;
	}
}
#endif /* HAVE_LIBZ */

static void
dispose_chunk( conn_t *conn )
{
	buff_chunk_t *bc = conn->write_buf;
	if (!(conn->write_buf = bc->next))
		conn->write_buf_append = &conn->write_buf;
	if (bc->data != bc->buf)
		free( bc->data );
	free( bc );
}

static int
do_queued_write( conn_t *conn )
{
	buff_chunk_t *bc;

	if (!conn->write_buf)
		return 0;

	while ((bc = conn->write_buf)) {
		int n, len = bc->len - conn->write_offset;
		if ((n = do_write( conn, bc->data + conn->write_offset, len )) < 0)
			return -1;
		if (n != len) {
			conn->write_offset += n;
			return 0;
		}
		conn->write_offset = 0;
		dispose_chunk( conn );
	}
#ifdef HAVE_LIBSSL
	if (conn->ssl && SSL_pending( conn->ssl ))
		fake_fd( conn->fd, POLLIN );
#endif
	return conn->write_callback( conn->callback_aux );
}

static void
do_append( conn_t *conn, char *buf, int len, ownership_t takeOwn )
{
	buff_chunk_t *bc;

	if (takeOwn == GiveOwn) {
		bc = nfmalloc( offsetof(buff_chunk_t, buf) );
		bc->data = buf;
	} else {
		bc = nfmalloc( offsetof(buff_chunk_t, buf) + len );
		bc->data = bc->buf;
		memcpy( bc->data, buf, len );
	}
	bc->len = len;
	bc->next = 0;
	*conn->write_buf_append = bc;
	conn->write_buf_append = &bc->next;
}

int
socket_write( conn_t *conn, char *buf, int len, ownership_t takeOwn )
{
	if (conn->write_buf) {
		do_append( conn, buf, len, takeOwn );
		return len;
	} else {
		int n = do_write( conn, buf, len );
		if (n != len && n >= 0) {
			conn->write_offset = n;
			do_append( conn, buf, len, takeOwn );
		} else if (takeOwn) {
			free( buf );
		}
		return n;
	}
}

static void
socket_fd_cb( int events, void *aux )
{
	conn_t *conn = (conn_t *)aux;

	if ((events & POLLERR) || conn->state == SCK_CONNECTING) {
		int soerr;
		socklen_t selen = sizeof(soerr);
		if (getsockopt( conn->fd, SOL_SOCKET, SO_ERROR, &soerr, &selen )) {
			perror( "getsockopt" );
			exit( 1 );
		}
		errno = soerr;
		if (conn->state == SCK_CONNECTING) {
			if (errno)
				socket_connect_failed( conn );
			else
				socket_connected( conn );
			return;
		}
		sys_error( "Socket error from %s", conn->name );
		socket_fail( conn );
		return;
	}

	if (events & POLLOUT)
		conf_fd( conn->fd, POLLIN, 0 );

#ifdef HAVE_LIBSSL
	if (conn->state == SCK_STARTTLS) {
		start_tls_p2( conn );
		return;
	}
	if (conn->ssl) {
		if (do_queued_write( conn ) < 0)
			return;
		socket_fill( conn );
		return;
	}
#endif

	if ((events & POLLOUT) && do_queued_write( conn ) < 0)
		return;
	if (events & POLLIN)
		socket_fill( conn );
}
