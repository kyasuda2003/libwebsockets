/*
 * libwebsockets - small server side websockets and web server implementation
 *
 * Copyright (C) 2010 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 */

#include "private-libwebsockets.h"

#ifdef WIN32
#include <tchar.h>
#include <io.h>
#else
#ifdef LWS_BUILTIN_GETIFADDRS
#include <getifaddrs.h>
#else
#include <ifaddrs.h>
#endif
#include <sys/un.h>
#include <sys/socket.h>
#include <netdb.h>
#endif

#ifdef LWS_OPENSSL_SUPPORT
int openssl_websocket_private_data_index;
#endif

#ifdef __MINGW32__
#include "../win32port/win32helpers/websock-w32.c"
#else
#ifdef __MINGW64__
#include "../win32port/win32helpers/websock-w32.c"
#endif
#endif

static int log_level = LLL_ERR | LLL_WARN;
static void lwsl_emit_stderr(const char *line);
static void (*lwsl_emit)(const char *line) = lwsl_emit_stderr;
static const char *log_level_names[] = {
	"ERR",
	"WARN",
	"INFO",
	"DEBUG",
	"PARSER",
	"HEADER",
	"EXTENSION",
	"CLIENT",
};

/* file descriptor hash management */

struct libwebsocket *
wsi_from_fd(struct libwebsocket_context *context, int fd)
{
	int h = LWS_FD_HASH(fd);
	int n = 0;

	for (n = 0; n < context->fd_hashtable[h].length; n++)
		if (context->fd_hashtable[h].wsi[n]->sock == fd)
			return context->fd_hashtable[h].wsi[n];

	return NULL;
}

int
insert_wsi(struct libwebsocket_context *context, struct libwebsocket *wsi)
{
	int h = LWS_FD_HASH(wsi->sock);

	if (context->fd_hashtable[h].length == MAX_CLIENTS - 1) {
		lwsl_err("hash table overflow\n");
		return 1;
	}

	context->fd_hashtable[h].wsi[context->fd_hashtable[h].length++] = wsi;

	return 0;
}

int
delete_from_fd(struct libwebsocket_context *context, int fd)
{
	int h = LWS_FD_HASH(fd);
	int n = 0;

	for (n = 0; n < context->fd_hashtable[h].length; n++)
		if (context->fd_hashtable[h].wsi[n]->sock == fd) {
			while (n < context->fd_hashtable[h].length) {
				context->fd_hashtable[h].wsi[n] =
					    context->fd_hashtable[h].wsi[n + 1];
				n++;
			}
			context->fd_hashtable[h].length--;

			return 0;
		}

	lwsl_err("Failed to find fd %d requested for "
						   "delete in hashtable\n", fd);
	return 1;
}

#ifdef LWS_OPENSSL_SUPPORT
static void
libwebsockets_decode_ssl_error(void)
{
	char buf[256];
	u_long err;

	while ((err = ERR_get_error()) != 0) {
		ERR_error_string_n(err, buf, sizeof(buf));
		lwsl_err("*** %s\n", buf);
	}
}
#endif


static int
interface_to_sa(const char *ifname, struct sockaddr_in *addr, size_t addrlen)
{
	int rc = -1;
#ifdef WIN32
	/* TODO */
#else
	struct ifaddrs *ifr;
	struct ifaddrs *ifc;
	struct sockaddr_in *sin;

	getifaddrs(&ifr);
	for (ifc = ifr; ifc != NULL; ifc = ifc->ifa_next) {
		if (strcmp(ifc->ifa_name, ifname))
			continue;
		if (ifc->ifa_addr == NULL)
			continue;
		sin = (struct sockaddr_in *)ifc->ifa_addr;
		if (sin->sin_family != AF_INET)
			continue;
		memcpy(addr, sin, addrlen);
		rc = 0;
	}

	freeifaddrs(ifr);
#endif
	return rc;
}

void
libwebsocket_close_and_free_session(struct libwebsocket_context *context,
			 struct libwebsocket *wsi, enum lws_close_status reason)
{
	int n;
	int old_state;
	unsigned char buf[LWS_SEND_BUFFER_PRE_PADDING + 2 +
						  LWS_SEND_BUFFER_POST_PADDING];
	int ret;
	int m;
	struct lws_tokens eff_buf;
	struct libwebsocket_extension *ext;

	if (!wsi)
		return;

	old_state = wsi->state;

	if (old_state == WSI_STATE_DEAD_SOCKET)
		return;

	wsi->close_reason = reason;

	/*
	 * are his extensions okay with him closing?  Eg he might be a mux
	 * parent and just his ch1 aspect is closing?
	 */


	for (n = 0; n < wsi->count_active_extensions; n++) {
		if (!wsi->active_extensions[n]->callback)
			continue;

		m = wsi->active_extensions[n]->callback(context,
			wsi->active_extensions[n], wsi,
			LWS_EXT_CALLBACK_CHECK_OK_TO_REALLY_CLOSE,
				       wsi->active_extensions_user[n], NULL, 0);

		/*
		 * if somebody vetoed actually closing him at this time....
		 * up to the extension to track the attempted close, let's
		 * just bail
		 */

		if (m) {
			lwsl_ext("extension vetoed close\n");
			return;
		}
	}



	/*
	 * flush any tx pending from extensions, since we may send close packet
	 * if there are problems with send, just nuke the connection
	 */

	ret = 1;
	while (ret == 1) {

		/* default to nobody has more to spill */

		ret = 0;
		eff_buf.token = NULL;
		eff_buf.token_len = 0;

		/* show every extension the new incoming data */

		for (n = 0; n < wsi->count_active_extensions; n++) {
			m = wsi->active_extensions[n]->callback(
					wsi->protocol->owning_server,
					wsi->active_extensions[n], wsi,
					LWS_EXT_CALLBACK_FLUSH_PENDING_TX,
				   wsi->active_extensions_user[n], &eff_buf, 0);
			if (m < 0) {
				lwsl_ext("Extension reports fatal error\n");
				goto just_kill_connection;
			}
			if (m)
				/*
				 * at least one extension told us he has more
				 * to spill, so we will go around again after
				 */
				ret = 1;
		}

		/* assuming they left us something to send, send it */

		if (eff_buf.token_len)
			if (lws_issue_raw(wsi, (unsigned char *)eff_buf.token,
							     eff_buf.token_len))
				goto just_kill_connection;
	}

	/*
	 * signal we are closing, libsocket_write will
	 * add any necessary version-specific stuff.  If the write fails,
	 * no worries we are closing anyway.  If we didn't initiate this
	 * close, then our state has been changed to
	 * WSI_STATE_RETURNED_CLOSE_ALREADY and we will skip this.
	 *
	 * Likewise if it's a second call to close this connection after we
	 * sent the close indication to the peer already, we are in state
	 * WSI_STATE_AWAITING_CLOSE_ACK and will skip doing this a second time.
	 */

	if (old_state == WSI_STATE_ESTABLISHED &&
					  reason != LWS_CLOSE_STATUS_NOSTATUS) {

		lwsl_debug("sending close indication...\n");

		n = libwebsocket_write(wsi, &buf[LWS_SEND_BUFFER_PRE_PADDING],
							    0, LWS_WRITE_CLOSE);
		if (!n) {
			/*
			 * we have sent a nice protocol level indication we
			 * now wish to close, we should not send anything more
			 */

			wsi->state = WSI_STATE_AWAITING_CLOSE_ACK;

			/* and we should wait for a reply for a bit */

			libwebsocket_set_timeout(wsi,
						  PENDING_TIMEOUT_CLOSE_ACK, AWAITING_TIMEOUT);

			lwsl_debug("sent close indication, awaiting ack\n");

			return;
		}

		/* else, the send failed and we should just hang up */
	}

just_kill_connection:

	lwsl_debug("libwebsocket_close_and_free_session: just_kill_connection\n");

	/*
	 * we won't be servicing or receiving anything further from this guy
	 * remove this fd from wsi mapping hashtable
	 */

	if (wsi->sock)
		delete_from_fd(context, wsi->sock);

	/* delete it from the internal poll list if still present */

	for (n = 0; n < context->fds_count; n++) {
		if (context->fds[n].fd != wsi->sock)
			continue;
		while (n < context->fds_count - 1) {
			context->fds[n] = context->fds[n + 1];
			n++;
		}
		context->fds_count--;
		/* we only have to deal with one */
		n = context->fds_count;
	}

	/* remove also from external POLL support via protocol 0 */
	if (wsi->sock)
		context->protocols[0].callback(context, wsi,
		    LWS_CALLBACK_DEL_POLL_FD, (void *)(long)wsi->sock, NULL, 0);

	wsi->state = WSI_STATE_DEAD_SOCKET;

	/* tell the user it's all over for this guy */

	if (wsi->protocol && wsi->protocol->callback &&
			((old_state == WSI_STATE_ESTABLISHED) ||
			 (old_state == WSI_STATE_RETURNED_CLOSE_ALREADY) ||
			 (old_state == WSI_STATE_AWAITING_CLOSE_ACK))) {
		lwsl_debug("calling back CLOSED\n");
		wsi->protocol->callback(context, wsi, LWS_CALLBACK_CLOSED,
						      wsi->user_space, NULL, 0);
	} else
		lwsl_debug("not calling back closed, old_state=%d\n", old_state);

	/* deallocate any active extension contexts */

	for (n = 0; n < wsi->count_active_extensions; n++) {
		if (!wsi->active_extensions[n]->callback)
			continue;

		wsi->active_extensions[n]->callback(context,
			wsi->active_extensions[n], wsi,
				LWS_EXT_CALLBACK_DESTROY,
				       wsi->active_extensions_user[n], NULL, 0);

		free(wsi->active_extensions_user[n]);
	}

	/*
	 * inform all extensions in case they tracked this guy out of band
	 * even though not active on him specifically
	 */

	ext = context->extensions;
	while (ext && ext->callback) {
		ext->callback(context, ext, wsi,
				LWS_EXT_CALLBACK_DESTROY_ANY_WSI_CLOSING,
				       NULL, NULL, 0);
		ext++;
	}

	/* free up his parsing allocations */

	for (n = 0; n < WSI_TOKEN_COUNT; n++)
		if (wsi->utf8_token[n].token)
			free(wsi->utf8_token[n].token);

	if (wsi->c_address)
		free(wsi->c_address);

/*	lwsl_info("closing fd=%d\n", wsi->sock); */

#ifdef LWS_OPENSSL_SUPPORT
	if (wsi->ssl) {
		n = SSL_get_fd(wsi->ssl);
		SSL_shutdown(wsi->ssl);
		compatible_close(n);
		SSL_free(wsi->ssl);
	} else {
#endif
		shutdown(wsi->sock, SHUT_RDWR);

		if (wsi->sock)
			compatible_close(wsi->sock);
#ifdef LWS_OPENSSL_SUPPORT
	}
#endif
	if (wsi->protocol && wsi->protocol->per_session_data_size && wsi->user_space) /* user code may own */
		free(wsi->user_space);

	free(wsi);
}

/**
 * libwebsockets_hangup_on_client() - Server calls to terminate client
 *					connection
 * @context:	libwebsockets context
 * @fd:		Connection socket descriptor
 */

void
libwebsockets_hangup_on_client(struct libwebsocket_context *context, int fd)
{
	struct libwebsocket *wsi = wsi_from_fd(context, fd);

	if (wsi == NULL)
		return;

	libwebsocket_close_and_free_session(context, wsi,
						     LWS_CLOSE_STATUS_NOSTATUS);
}


/**
 * libwebsockets_get_peer_addresses() - Get client address information
 * @fd:		Connection socket descriptor
 * @name:	Buffer to take client address name
 * @name_len:	Length of client address name buffer
 * @rip:	Buffer to take client address IP qotted quad
 * @rip_len:	Length of client address IP buffer
 *
 *	This function fills in @name and @rip with the name and IP of
 *	the client connected with socket descriptor @fd.  Names may be
 *	truncated if there is not enough room.  If either cannot be
 *	determined, they will be returned as valid zero-length strings.
 */

void
libwebsockets_get_peer_addresses(int fd, char *name, int name_len,
					char *rip, int rip_len)
{
	unsigned int len;
	struct sockaddr_in sin;
	struct hostent *host;
	struct hostent *host1;
	char ip[128];
	unsigned char *p;
	int n;
#ifdef AF_LOCAL
    struct sockaddr_un *un;
#endif

	rip[0] = '\0';
	name[0] = '\0';

	len = sizeof sin;
	if (getpeername(fd, (struct sockaddr *) &sin, &len) < 0) {
		perror("getpeername");
		return;
	}

	host = gethostbyaddr((char *) &sin.sin_addr, sizeof sin.sin_addr,
								       AF_INET);
	if (host == NULL) {
		perror("gethostbyaddr");
		return;
	}

	strncpy(name, host->h_name, name_len);
	name[name_len - 1] = '\0';

	host1 = gethostbyname(host->h_name);
	if (host1 == NULL)
		return;
	p = (unsigned char *)host1;
	n = 0;
	while (p != NULL) {
		p = (unsigned char *)host1->h_addr_list[n++];
		if (p == NULL)
			continue;
		if ((host1->h_addrtype != AF_INET)
#ifdef AF_LOCAL
			&& (host1->h_addrtype != AF_LOCAL)
#endif
			)
			continue;

		if (host1->h_addrtype == AF_INET)
			sprintf(ip, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
#ifdef AF_LOCAL
		else {
			un = (struct sockaddr_un *)p;
			strncpy(ip, un->sun_path, sizeof(ip) - 1);
			ip[sizeof(ip) - 1] = '\0';
		}
#endif
		p = NULL;
		strncpy(rip, ip, rip_len);
		rip[rip_len - 1] = '\0';
	}
}

int libwebsockets_get_random(struct libwebsocket_context *context,
							     void *buf, int len)
{
	int n;
	char *p = (char *)buf;

#ifdef WIN32
	for (n = 0; n < len; n++)
		p[n] = (unsigned char)rand();
#else
	n = read(context->fd_random, p, len);
#endif

	return n;
}

unsigned char *
libwebsockets_SHA1(const unsigned char *d, size_t n, unsigned char *md)
{
	return SHA1(d, n, md);
}

int lws_send_pipe_choked(struct libwebsocket *wsi)
{
	struct pollfd fds;

	fds.fd = wsi->sock;
	fds.events = POLLOUT;
	fds.revents = 0;

	if (poll(&fds, 1, 0) != 1)
		return 1;

	if ((fds.revents & POLLOUT) == 0)
		return 1;

	/* okay to send another packet without blocking */

	return 0;
}

int
lws_handle_POLLOUT_event(struct libwebsocket_context *context,
				struct libwebsocket *wsi, struct pollfd *pollfd)
{
	struct lws_tokens eff_buf;
	int n;
	int ret;
	int m;
	int handled = 0;

	for (n = 0; n < wsi->count_active_extensions; n++) {
		if (!wsi->active_extensions[n]->callback)
			continue;

		m = wsi->active_extensions[n]->callback(context,
			wsi->active_extensions[n], wsi,
			LWS_EXT_CALLBACK_IS_WRITEABLE,
				       wsi->active_extensions_user[n], NULL, 0);
		if (m > handled)
			handled = m;
	}

	if (handled == 1)
		goto notify_action;

	if (!wsi->extension_data_pending || handled == 2)
		goto user_service;

	/*
	 * check in on the active extensions, see if they
	 * had pending stuff to spill... they need to get the
	 * first look-in otherwise sequence will be disordered
	 *
	 * NULL, zero-length eff_buf means just spill pending
	 */

	ret = 1;
	while (ret == 1) {

		/* default to nobody has more to spill */

		ret = 0;
		eff_buf.token = NULL;
		eff_buf.token_len = 0;

		/* give every extension a chance to spill */

		for (n = 0; n < wsi->count_active_extensions; n++) {
			m = wsi->active_extensions[n]->callback(
				wsi->protocol->owning_server,
				wsi->active_extensions[n], wsi,
					LWS_EXT_CALLBACK_PACKET_TX_PRESEND,
				   wsi->active_extensions_user[n], &eff_buf, 0);
			if (m < 0) {
				lwsl_err("ext reports fatal error\n");
				return -1;
			}
			if (m)
				/*
				 * at least one extension told us he has more
				 * to spill, so we will go around again after
				 */
				ret = 1;
		}

		/* assuming they gave us something to send, send it */

		if (eff_buf.token_len) {
			if (lws_issue_raw(wsi, (unsigned char *)eff_buf.token,
							     eff_buf.token_len))
				return -1;
		} else
			continue;

		/* no extension has more to spill */

		if (!ret)
			continue;

		/*
		 * There's more to spill from an extension, but we just sent
		 * something... did that leave the pipe choked?
		 */

		if (!lws_send_pipe_choked(wsi))
			/* no we could add more */
			continue;

		lwsl_info("choked in POLLOUT service\n");

		/*
		 * Yes, he's choked.  Leave the POLLOUT masked on so we will
		 * come back here when he is unchoked.  Don't call the user
		 * callback to enforce ordering of spilling, he'll get called
		 * when we come back here and there's nothing more to spill.
		 */

		return 0;
	}

	wsi->extension_data_pending = 0;

user_service:
	/* one shot */

	if (pollfd) {
		pollfd->events &= ~POLLOUT;

		/* external POLL support via protocol 0 */
		context->protocols[0].callback(context, wsi,
			LWS_CALLBACK_CLEAR_MODE_POLL_FD,
			(void *)(long)wsi->sock, NULL, POLLOUT);
	}

notify_action:

	if (wsi->mode == LWS_CONNMODE_WS_CLIENT)
		n = LWS_CALLBACK_CLIENT_WRITEABLE;
	else
		n = LWS_CALLBACK_SERVER_WRITEABLE;

	wsi->protocol->callback(context, wsi, (enum libwebsocket_callback_reasons) n, wsi->user_space, NULL, 0);

	return 0;
}



void
libwebsocket_service_timeout_check(struct libwebsocket_context *context,
				     struct libwebsocket *wsi, unsigned int sec)
{
	int n;

	/*
	 * if extensions want in on it (eg, we are a mux parent)
	 * give them a chance to service child timeouts
	 */

	for (n = 0; n < wsi->count_active_extensions; n++)
		wsi->active_extensions[n]->callback(
				    context, wsi->active_extensions[n],
				    wsi, LWS_EXT_CALLBACK_1HZ,
				    wsi->active_extensions_user[n], NULL, sec);

	if (!wsi->pending_timeout)
		return;

	/*
	 * if we went beyond the allowed time, kill the
	 * connection
	 */

	if (sec > wsi->pending_timeout_limit) {
		lwsl_info("TIMEDOUT WAITING\n");
		libwebsocket_close_and_free_session(context,
				wsi, LWS_CLOSE_STATUS_NOSTATUS);
	}
}

struct libwebsocket *
libwebsocket_create_new_server_wsi(struct libwebsocket_context *context)
{
	struct libwebsocket *new_wsi;
	int n;

	new_wsi = (struct libwebsocket *)malloc(sizeof(struct libwebsocket));
	if (new_wsi == NULL) {
		lwsl_err("Out of memory for new connection\n");
		return NULL;
	}

	memset(new_wsi, 0, sizeof(struct libwebsocket));
	new_wsi->count_active_extensions = 0;
	new_wsi->pending_timeout = NO_PENDING_TIMEOUT;

	/* intialize the instance struct */

	new_wsi->state = WSI_STATE_HTTP;
	new_wsi->name_buffer_pos = 0;
	new_wsi->mode = LWS_CONNMODE_HTTP_SERVING;

	for (n = 0; n < WSI_TOKEN_COUNT; n++) {
		new_wsi->utf8_token[n].token = NULL;
		new_wsi->utf8_token[n].token_len = 0;
	}

	/*
	 * these can only be set once the protocol is known
	 * we set an unestablished connection's protocol pointer
	 * to the start of the supported list, so it can look
	 * for matching ones during the handshake
	 */
	new_wsi->protocol = context->protocols;
	new_wsi->user_space = NULL;

	/*
	 * Default protocol is 76 / 00
	 * After 76, there's a header specified to inform which
	 * draft the client wants, when that's seen we modify
	 * the individual connection's spec revision accordingly
	 */
	new_wsi->ietf_spec_revision = 0;

	return new_wsi;
}


/**
 * libwebsocket_service_fd() - Service polled socket with something waiting
 * @context:	Websocket context
 * @pollfd:	The pollfd entry describing the socket fd and which events
 *		happened.
 *
 *	This function closes any active connections and then frees the
 *	context.  After calling this, any further use of the context is
 *	undefined.
 */

int
libwebsocket_service_fd(struct libwebsocket_context *context,
							  struct pollfd *pollfd)
{
	unsigned char buf[LWS_SEND_BUFFER_PRE_PADDING + 1 +
			 MAX_BROADCAST_PAYLOAD + LWS_SEND_BUFFER_POST_PADDING];
	struct libwebsocket *wsi;
	struct libwebsocket *new_wsi;
	int n;
	int m;
	ssize_t len;
	int accept_fd;
	unsigned int clilen;
	struct sockaddr_in cli_addr;
	struct timeval tv;
	int more = 1;
	struct lws_tokens eff_buf;
	int opt = 1;
#ifndef LWS_NO_CLIENT
	extern int lws_client_socket_service(struct libwebsocket_context *context, struct libwebsocket *wsi, struct pollfd *pollfd);
#endif
	/*
	 * you can call us with pollfd = NULL to just allow the once-per-second
	 * global timeout checks; if less than a second since the last check
	 * it returns immediately then.
	 */

	gettimeofday(&tv, NULL);

	if (context->last_timeout_check_s != tv.tv_sec) {
		context->last_timeout_check_s = tv.tv_sec;

		/* global timeout check once per second */

		for (n = 0; n < context->fds_count; n++) {
			wsi = wsi_from_fd(context, context->fds[n].fd);

			libwebsocket_service_timeout_check(context, wsi,
								     tv.tv_sec);
		}
	}

	/* just here for timeout management? */

	if (pollfd == NULL)
		return 0;

	/* no, here to service a socket descriptor */

	/*
	 * deal with listen service piggybacking
	 * every listen_service_modulo services of other fds, we
	 * sneak one in to service the listen socket if there's anything waiting
	 *
	 * To handle connection storms, as found in ab, if we previously saw a
	 * pending connection here, it causes us to check again next time.
	 */

	if (context->listen_service_fd && pollfd->fd != context->listen_service_fd) {
		context->listen_service_count++;
		if (context->listen_service_extraseen ||
				context->listen_service_count == context->listen_service_modulo) {
			context->listen_service_count = 0;
			m = 1;
			if (context->listen_service_extraseen > 5)
				m = 2;
			while (m--) {
				/* even with extpoll, we prepared this internal fds for listen */
				n = poll(&context->fds[0], 1, 0);
				if (n > 0) { /* there's a connection waiting for us */
					libwebsocket_service_fd(context, &context->fds[0]);
					context->listen_service_extraseen++;
				} else {
					if (context->listen_service_extraseen)
						context->listen_service_extraseen--;
					break;
				}
			}
		}

	}

	/* okay, what we came here to do... */

	wsi = wsi_from_fd(context, pollfd->fd);

	if (wsi == NULL) {
		lwsl_debug("hm fd %d has NULL wsi\n", pollfd->fd);
		return 0;
	}

	switch (wsi->mode) {

	case LWS_CONNMODE_HTTP_SERVING:

		/* handle http headers coming in */

		/* any incoming data ready? */

		if (pollfd->revents & POLLIN) {

	#ifdef LWS_OPENSSL_SUPPORT
			if (wsi->ssl)
				len = SSL_read(wsi->ssl, buf, sizeof buf);
			else
	#endif
				len = recv(pollfd->fd, buf, sizeof buf, 0);

			if (len < 0) {
				lwsl_debug("Socket read returned %d\n", len);
				if (errno != EINTR && errno != EAGAIN)
					libwebsocket_close_and_free_session(context,
						       wsi, LWS_CLOSE_STATUS_NOSTATUS);
				return 0;
			}
			if (!len) {
				libwebsocket_close_and_free_session(context, wsi,
							    LWS_CLOSE_STATUS_NOSTATUS);
				return 0;
			}

			n = libwebsocket_read(context, wsi, buf, len);
			if (n < 0)
				/* we closed wsi */
				return 0;
		}

		/* this handles POLLOUT for http serving fragments */

		if (!(pollfd->revents & POLLOUT))
			break;

		/* one shot */
		pollfd->events &= ~POLLOUT;
		
		if (wsi->state != WSI_STATE_HTTP_ISSUING_FILE)
			break;

		if (libwebsockets_serve_http_file_fragment(context, wsi) < 0)
			libwebsocket_close_and_free_session(context, wsi,
					       LWS_CLOSE_STATUS_NOSTATUS);
		else
			if (wsi->state == WSI_STATE_HTTP && wsi->protocol->callback)
				if (wsi->protocol->callback(context, wsi, LWS_CALLBACK_HTTP_FILE_COMPLETION, wsi->user_space,
								wsi->filepath, wsi->filepos))
					libwebsocket_close_and_free_session(context, wsi, LWS_CLOSE_STATUS_NOSTATUS);
		break;

	case LWS_CONNMODE_SERVER_LISTENER:

		/* pollin means a client has connected to us then */

		if (!(pollfd->revents & POLLIN))
			break;

		if (context->fds_count >= MAX_CLIENTS) {
			lwsl_warn("too busy to accept new client\n");
			break;
		}

		/* listen socket got an unencrypted connection... */

		clilen = sizeof(cli_addr);
		accept_fd  = accept(pollfd->fd, (struct sockaddr *)&cli_addr,
								       &clilen);
		if (accept_fd < 0) {
			lwsl_warn("ERROR on accept: %s\n", strerror(errno));
			break;
		}

		/* Disable Nagle */
		opt = 1;
		setsockopt(accept_fd, IPPROTO_TCP, TCP_NODELAY,
					      (const void *)&opt, sizeof(opt));

		/*
		 * look at who we connected to and give user code a chance
		 * to reject based on client IP.  There's no protocol selected
		 * yet so we issue this to protocols[0]
		 */

		if ((context->protocols[0].callback)(context, wsi,
				LWS_CALLBACK_FILTER_NETWORK_CONNECTION,
					   (void *)(long)accept_fd, NULL, 0)) {
			lwsl_debug("Callback denied network connection\n");
			compatible_close(accept_fd);
			break;
		}

		new_wsi = libwebsocket_create_new_server_wsi(context);
		if (new_wsi == NULL) {
			compatible_close(accept_fd);
			break;
		}

		new_wsi->sock = accept_fd;


#ifdef LWS_OPENSSL_SUPPORT
		new_wsi->ssl = NULL;

		if (context->use_ssl) {

			new_wsi->ssl = SSL_new(context->ssl_ctx);
			if (new_wsi->ssl == NULL) {
				lwsl_err("SSL_new failed: %s\n",
				    ERR_error_string(SSL_get_error(
				    new_wsi->ssl, 0), NULL));
				    libwebsockets_decode_ssl_error();
				free(new_wsi);
				compatible_close(accept_fd);
				break;
			}

			SSL_set_ex_data(new_wsi->ssl,
				openssl_websocket_private_data_index, context);

			SSL_set_fd(new_wsi->ssl, accept_fd);

			n = SSL_accept(new_wsi->ssl);
			if (n != 1) {
				/*
				 * browsers seem to probe with various
				 * ssl params which fail then retry
				 * and succeed
				 */
				lwsl_debug("SSL_accept failed skt %u: %s\n",
				      pollfd->fd,
				      ERR_error_string(SSL_get_error(
				      new_wsi->ssl, n), NULL));
				SSL_free(
				       new_wsi->ssl);
				free(new_wsi);
				compatible_close(accept_fd);
				break;
			}

			lwsl_debug("accepted new SSL conn  "
			      "port %u on fd=%d SSL ver %s\n",
				ntohs(cli_addr.sin_port), accept_fd,
				  SSL_get_version(new_wsi->ssl));

		} else
#endif
			lwsl_debug("accepted new conn  port %u on fd=%d\n",
					  ntohs(cli_addr.sin_port), accept_fd);

		insert_wsi(context, new_wsi);

		/*
		 * make sure NO events are seen yet on this new socket
		 * (otherwise we inherit old fds[client].revents from
		 * previous socket there and die mysteriously! )
		 */
		context->fds[context->fds_count].revents = 0;

		context->fds[context->fds_count].events = POLLIN;
		context->fds[context->fds_count++].fd = accept_fd;

		/* external POLL support via protocol 0 */
		context->protocols[0].callback(context, new_wsi,
			LWS_CALLBACK_ADD_POLL_FD,
			(void *)(long)accept_fd, NULL, POLLIN);

		break;

	case LWS_CONNMODE_BROADCAST_PROXY_LISTENER:

		/* as we are listening, POLLIN means accept() is needed */

		if (!(pollfd->revents & POLLIN))
			break;

		/* listen socket got an unencrypted connection... */

		clilen = sizeof(cli_addr);
		accept_fd  = accept(pollfd->fd, (struct sockaddr *)&cli_addr,
								       &clilen);
		if (accept_fd < 0) {
			lwsl_warn("ERROR on accept %d\n", accept_fd);
			return 0;
		}

		if (context->fds_count >= MAX_CLIENTS) {
			lwsl_err("too busy to accept new broadcast "
							      "proxy client\n");
			goto bail_prox_listener;
		}

		/* create a dummy wsi for the connection and add it */

		new_wsi = (struct libwebsocket *)malloc(sizeof(struct libwebsocket));
		if (new_wsi == NULL) {
			lwsl_err("Out of mem\n");
			goto bail_prox_listener;
		}
		memset(new_wsi, 0, sizeof (struct libwebsocket));
		new_wsi->sock = accept_fd;
		new_wsi->mode = LWS_CONNMODE_BROADCAST_PROXY;
		new_wsi->state = WSI_STATE_ESTABLISHED;
		new_wsi->count_active_extensions = 0;
		/* note which protocol we are proxying */
		new_wsi->protocol_index_for_broadcast_proxy =
					wsi->protocol_index_for_broadcast_proxy;
		insert_wsi(context, new_wsi);

		/* add connected socket to internal poll array */

		context->fds[context->fds_count].revents = 0;
		context->fds[context->fds_count].events = POLLIN;
		context->fds[context->fds_count++].fd = accept_fd;

		/* external POLL support via protocol 0 */
		context->protocols[0].callback(context, new_wsi,
			LWS_CALLBACK_ADD_POLL_FD,
			(void *)(long)accept_fd, NULL, POLLIN);

		break;

bail_prox_listener:
		compatible_close(accept_fd);
		break;

	case LWS_CONNMODE_BROADCAST_PROXY:

		/* handle session socket closed */

		if (pollfd->revents & (POLLERR | POLLHUP)) {

			lwsl_debug("Session Socket %p (fd=%d) dead\n",
				(void *)wsi, pollfd->fd);

			libwebsocket_close_and_free_session(context, wsi,
						       LWS_CLOSE_STATUS_NORMAL);
			return 0;
		}

		/*
		 * either extension code with stuff to spill, or the user code,
		 * requested a callback when it was OK to write
		 */

		if (pollfd->revents & POLLOUT)
			if (lws_handle_POLLOUT_event(context, wsi,
								 pollfd) < 0) {
				libwebsocket_close_and_free_session(
					context, wsi, LWS_CLOSE_STATUS_NORMAL);
				return 0;
			}

		/* any incoming data ready? */

		if (!(pollfd->revents & POLLIN))
			break;

		/* get the issued broadcast payload from the socket */

		len = read(pollfd->fd, buf + LWS_SEND_BUFFER_PRE_PADDING,
							 MAX_BROADCAST_PAYLOAD);
		if (len < 0) {
			lwsl_err("Error reading broadcast payload\n");
			break;
		}

		/* broadcast it to all guys with this protocol index */

		for (n = 0; n < FD_HASHTABLE_MODULUS; n++) {

			for (m = 0; m < context->fd_hashtable[n].length; m++) {

				new_wsi = context->fd_hashtable[n].wsi[m];

				/* only to clients we are serving to */

				if (new_wsi->mode != LWS_CONNMODE_WS_SERVING)
					continue;

				/*
				 * never broadcast to non-established
				 * connection
				 */

				if (new_wsi->state != WSI_STATE_ESTABLISHED)
					continue;

				/*
				 * only broadcast to connections using
				 * the requested protocol
				 */

				if (new_wsi->protocol->protocol_index !=
					wsi->protocol_index_for_broadcast_proxy)
					continue;

				/* broadcast it to this connection */

				new_wsi->protocol->callback(context, new_wsi,
					LWS_CALLBACK_BROADCAST,
					new_wsi->user_space,
					buf + LWS_SEND_BUFFER_PRE_PADDING, len);
			}
		}
		break;


	case LWS_CONNMODE_WS_SERVING:
	case LWS_CONNMODE_WS_CLIENT:

		/* handle session socket closed */

		if (pollfd->revents & (POLLERR | POLLHUP)) {

			lwsl_debug("Session Socket %p (fd=%d) dead\n",
				(void *)wsi, pollfd->fd);

			libwebsocket_close_and_free_session(context, wsi,
						     LWS_CLOSE_STATUS_NOSTATUS);
			return 0;
		}

		/* the guy requested a callback when it was OK to write */

		if ((pollfd->revents & POLLOUT) &&
					    wsi->state == WSI_STATE_ESTABLISHED)
			if (lws_handle_POLLOUT_event(context, wsi,
								  pollfd) < 0) {
				libwebsocket_close_and_free_session(
					 context, wsi, LWS_CLOSE_STATUS_NORMAL);
				return 0;
			}


		/* any incoming data ready? */

		if (!(pollfd->revents & POLLIN))
			break;

#ifdef LWS_OPENSSL_SUPPORT
read_pending:
		if (wsi->ssl)
			eff_buf.token_len = SSL_read(wsi->ssl, buf, sizeof buf);
		else
#endif
			eff_buf.token_len =
					   recv(pollfd->fd, buf, sizeof buf, 0);

		if (eff_buf.token_len < 0) {
			lwsl_debug("Socket read returned %d\n",
							    eff_buf.token_len);
			if (errno != EINTR && errno != EAGAIN)
				libwebsocket_close_and_free_session(context,
					       wsi, LWS_CLOSE_STATUS_NOSTATUS);
			return 0;
		}
		if (!eff_buf.token_len) {
			libwebsocket_close_and_free_session(context, wsi,
						    LWS_CLOSE_STATUS_NOSTATUS);
			return 0;
		}

		/*
		 * give any active extensions a chance to munge the buffer
		 * before parse.  We pass in a pointer to an lws_tokens struct
		 * prepared with the default buffer and content length that's in
		 * there.  Rather than rewrite the default buffer, extensions
		 * that expect to grow the buffer can adapt .token to
		 * point to their own per-connection buffer in the extension
		 * user allocation.  By default with no extensions or no
		 * extension callback handling, just the normal input buffer is
		 * used then so it is efficient.
		 */

		eff_buf.token = (char *)buf;

		more = 1;
		while (more) {

			more = 0;

			for (n = 0; n < wsi->count_active_extensions; n++) {
				m = wsi->active_extensions[n]->callback(context,
					wsi->active_extensions[n], wsi,
					LWS_EXT_CALLBACK_PACKET_RX_PREPARSE,
					wsi->active_extensions_user[n],
								   &eff_buf, 0);
				if (m < 0) {
					lwsl_ext(
					    "Extension reports fatal error\n");
					libwebsocket_close_and_free_session(
						context, wsi,
						    LWS_CLOSE_STATUS_NOSTATUS);
					return 0;
				}
				if (m)
					more = 1;
			}

			/* service incoming data */

			if (eff_buf.token_len) {
				n = libwebsocket_read(context, wsi,
					(unsigned char *)eff_buf.token,
							    eff_buf.token_len);
				if (n < 0)
					/* we closed wsi */
					return 0;
			}

			eff_buf.token = NULL;
			eff_buf.token_len = 0;
		}

#ifdef LWS_OPENSSL_SUPPORT
		if (wsi->ssl && SSL_pending(wsi->ssl))
			goto read_pending;
#endif
		break;

	default:
#ifdef LWS_NO_CLIENT
		break;
#else
		return  lws_client_socket_service(context, wsi, pollfd);
#endif
	}

	return 0;
}


/**
 * libwebsocket_context_destroy() - Destroy the websocket context
 * @context:	Websocket context
 *
 *	This function closes any active connections and then frees the
 *	context.  After calling this, any further use of the context is
 *	undefined.
 */
void
libwebsocket_context_destroy(struct libwebsocket_context *context)
{
	int n;
	int m;
	struct libwebsocket *wsi;
	struct libwebsocket_extension *ext;

	for (n = 0; n < FD_HASHTABLE_MODULUS; n++)
		for (m = 0; m < context->fd_hashtable[n].length; m++) {
			wsi = context->fd_hashtable[n].wsi[m];
			libwebsocket_close_and_free_session(context, wsi,
						    LWS_CLOSE_STATUS_GOINGAWAY);
		}

	/*
	 * give all extensions a chance to clean up any per-context
	 * allocations they might have made
	 */

	ext = context->extensions;
	m = LWS_EXT_CALLBACK_CLIENT_CONTEXT_DESTRUCT;
	if (context->listen_port)
		m = LWS_EXT_CALLBACK_SERVER_CONTEXT_DESTRUCT;
	while (ext && ext->callback) {
		ext->callback(context, ext, NULL, (enum libwebsocket_extension_callback_reasons)m, NULL, NULL, 0);
		ext++;
	}

#ifdef WIN32
#else
	close(context->fd_random);
#endif

#ifdef LWS_OPENSSL_SUPPORT
	if (context->ssl_ctx)
		SSL_CTX_free(context->ssl_ctx);
	if (context->ssl_client_ctx)
		SSL_CTX_free(context->ssl_client_ctx);
#endif

	free(context);

#ifdef WIN32
	WSACleanup();
#endif
}

LWS_EXTERN void *
libwebsocket_context_user(struct libwebsocket_context *context)
{
    return context->user_space;
}

/**
 * libwebsocket_service() - Service any pending websocket activity
 * @context:	Websocket context
 * @timeout_ms:	Timeout for poll; 0 means return immediately if nothing needed
 *		service otherwise block and service immediately, returning
 *		after the timeout if nothing needed service.
 *
 *	This function deals with any pending websocket traffic, for three
 *	kinds of event.  It handles these events on both server and client
 *	types of connection the same.
 *
 *	1) Accept new connections to our context's server
 *
 *	2) Perform pending broadcast writes initiated from other forked
 *	   processes (effectively serializing asynchronous broadcasts)
 *
 *	3) Call the receive callback for incoming frame data received by
 *	    server or client connections.
 *
 *	You need to call this service function periodically to all the above
 *	functions to happen; if your application is single-threaded you can
 *	just call it in your main event loop.
 *
 *	Alternatively you can fork a new process that asynchronously handles
 *	calling this service in a loop.  In that case you are happy if this
 *	call blocks your thread until it needs to take care of something and
 *	would call it with a large nonzero timeout.  Your loop then takes no
 *	CPU while there is nothing happening.
 *
 *	If you are calling it in a single-threaded app, you don't want it to
 *	wait around blocking other things in your loop from happening, so you
 *	would call it with a timeout_ms of 0, so it returns immediately if
 *	nothing is pending, or as soon as it services whatever was pending.
 */


int
libwebsocket_service(struct libwebsocket_context *context, int timeout_ms)
{
	int n;

	/* stay dead once we are dead */

	if (context == NULL)
		return 1;

	/* wait for something to need service */

	n = poll(context->fds, context->fds_count, timeout_ms);
	if (n == 0) /* poll timeout */
		return 0;



	if (n < 0) {
		/*
		lwsl_err("Listen Socket dead\n");
		*/
		return -1;
	}

	/* handle accept on listening socket? */

	for (n = 0; n < context->fds_count; n++)
		if (context->fds[n].revents)
			if (libwebsocket_service_fd(context,
							&context->fds[n]) < 0)
				return -1;
	return 0;
}

int
lws_any_extension_handled(struct libwebsocket_context *context,
			  struct libwebsocket *wsi,
			  enum libwebsocket_extension_callback_reasons r,
						       void *v, size_t len)
{
	int n;
	int handled = 0;

	/* maybe an extension will take care of it for us */

	for (n = 0; n < wsi->count_active_extensions && !handled; n++) {
		if (!wsi->active_extensions[n]->callback)
			continue;

		handled |= wsi->active_extensions[n]->callback(context,
			wsi->active_extensions[n], wsi,
			r, wsi->active_extensions_user[n], v, len);
	}

	return handled;
}


void *
lws_get_extension_user_matching_ext(struct libwebsocket *wsi,
					   struct libwebsocket_extension *ext)
{
	int n = 0;

	if (wsi == NULL)
		return NULL;

	while (n < wsi->count_active_extensions) {
		if (wsi->active_extensions[n] != ext) {
			n++;
			continue;
		}
		return wsi->active_extensions_user[n];
	}

	return NULL;
}

/**
 * libwebsocket_callback_on_writable() - Request a callback when this socket
 *					 becomes able to be written to without
 *					 blocking
 *
 * @context:	libwebsockets context
 * @wsi:	Websocket connection instance to get callback for
 */

int
libwebsocket_callback_on_writable(struct libwebsocket_context *context,
						      struct libwebsocket *wsi)
{
	int n;
	int handled = 0;

	/* maybe an extension will take care of it for us */

	for (n = 0; n < wsi->count_active_extensions; n++) {
		if (!wsi->active_extensions[n]->callback)
			continue;

		handled |= wsi->active_extensions[n]->callback(context,
			wsi->active_extensions[n], wsi,
			LWS_EXT_CALLBACK_REQUEST_ON_WRITEABLE,
				       wsi->active_extensions_user[n], NULL, 0);
	}

	if (handled)
		return 1;

	for (n = 0; n < context->fds_count; n++)
		if (context->fds[n].fd == wsi->sock) {
			context->fds[n].events |= POLLOUT;
			n = context->fds_count + 1;
		}

	if (n == context->fds_count)
		lwsl_err("libwebsocket_callback_on_writable: "
				      "failed to find socket %d\n", wsi->sock);

	/* external POLL support via protocol 0 */
	context->protocols[0].callback(context, wsi,
		LWS_CALLBACK_SET_MODE_POLL_FD,
		(void *)(long)wsi->sock, NULL, POLLOUT);

	return 1;
}

/**
 * libwebsocket_callback_on_writable_all_protocol() - Request a callback for
 *			all connections using the given protocol when it
 *			becomes possible to write to each socket without
 *			blocking in turn.
 *
 * @protocol:	Protocol whose connections will get callbacks
 */

int
libwebsocket_callback_on_writable_all_protocol(
				  const struct libwebsocket_protocols *protocol)
{
	struct libwebsocket_context *context = protocol->owning_server;
	int n;
	int m;
	struct libwebsocket *wsi;

	for (n = 0; n < FD_HASHTABLE_MODULUS; n++) {

		for (m = 0; m < context->fd_hashtable[n].length; m++) {

			wsi = context->fd_hashtable[n].wsi[m];

			if (wsi->protocol == protocol)
				libwebsocket_callback_on_writable(context, wsi);
		}
	}

	return 0;
}

/**
 * libwebsocket_set_timeout() - marks the wsi as subject to a timeout
 *
 * You will not need this unless you are doing something special
 *
 * @wsi:	Websocket connection instance
 * @reason:	timeout reason
 * @secs:	how many seconds
 */

void
libwebsocket_set_timeout(struct libwebsocket *wsi,
					  enum pending_timeout reason, int secs)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);

	wsi->pending_timeout_limit = tv.tv_sec + secs;
	wsi->pending_timeout = reason;
}


/**
 * libwebsocket_get_socket_fd() - returns the socket file descriptor
 *
 * You will not need this unless you are doing something special
 *
 * @wsi:	Websocket connection instance
 */

int
libwebsocket_get_socket_fd(struct libwebsocket *wsi)
{
	return wsi->sock;
}

/**
 * libwebsocket_rx_flow_control() - Enable and disable socket servicing for
 *				receieved packets.
 *
 * If the output side of a server process becomes choked, this allows flow
 * control for the input side.
 *
 * @wsi:	Websocket connection instance to get callback for
 * @enable:	0 = disable read servicing for this connection, 1 = enable
 */

int
libwebsocket_rx_flow_control(struct libwebsocket *wsi, int enable)
{
	struct libwebsocket_context *context = wsi->protocol->owning_server;
	int n;

	for (n = 0; n < context->fds_count; n++)
		if (context->fds[n].fd == wsi->sock) {
			if (enable)
				context->fds[n].events |= POLLIN;
			else
				context->fds[n].events &= ~POLLIN;

			return 0;
		}

	if (enable)
		/* external POLL support via protocol 0 */
		context->protocols[0].callback(context, wsi,
			LWS_CALLBACK_SET_MODE_POLL_FD,
			(void *)(long)wsi->sock, NULL, POLLIN);
	else
		/* external POLL support via protocol 0 */
		context->protocols[0].callback(context, wsi,
			LWS_CALLBACK_CLEAR_MODE_POLL_FD,
			(void *)(long)wsi->sock, NULL, POLLIN);

#if 0
	lwsl_err("libwebsocket_rx_flow_control unable to find socket\n");
#endif
	return 1;
}

/**
 * libwebsocket_canonical_hostname() - returns this host's hostname
 *
 * This is typically used by client code to fill in the host parameter
 * when making a client connection.  You can only call it after the context
 * has been created.
 *
 * @context:	Websocket context
 */


extern const char *
libwebsocket_canonical_hostname(struct libwebsocket_context *context)
{
	return (const char *)context->canonical_hostname;
}


static void sigpipe_handler(int x)
{
}

#ifdef LWS_OPENSSL_SUPPORT
static int
OpenSSL_verify_callback(int preverify_ok, X509_STORE_CTX *x509_ctx)
{

	SSL *ssl;
	int n;
	struct libwebsocket_context *context;

	ssl = X509_STORE_CTX_get_ex_data(x509_ctx,
		SSL_get_ex_data_X509_STORE_CTX_idx());

	/*
	 * !!! nasty openssl requires the index to come as a library-scope
	 * static
	 */
	context = SSL_get_ex_data(ssl, openssl_websocket_private_data_index);

	n = context->protocols[0].callback(NULL, NULL,
		LWS_CALLBACK_OPENSSL_PERFORM_CLIENT_CERT_VERIFICATION,
						   x509_ctx, ssl, preverify_ok);

	/* convert return code from 0 = OK to 1 = OK */

	if (!n)
		n = 1;
	else
		n = 0;

	return n;
}
#endif


/**
 * libwebsocket_create_context() - Create the websocket handler
 * @port:	Port to listen on... you can use 0 to suppress listening on
 *		any port, that's what you want if you are not running a
 *		websocket server at all but just using it as a client
 * @interf:  NULL to bind the listen socket to all interfaces, or the
 *		interface name, eg, "eth2"
 * @protocols:	Array of structures listing supported protocols and a protocol-
 *		specific callback for each one.  The list is ended with an
 *		entry that has a NULL callback pointer.
 *		It's not const because we write the owning_server member
 * @extensions: NULL or array of libwebsocket_extension structs listing the
 *		extensions this context supports
 * @ssl_cert_filepath:	If libwebsockets was compiled to use ssl, and you want
 *			to listen using SSL, set to the filepath to fetch the
 *			server cert from, otherwise NULL for unencrypted
 * @ssl_private_key_filepath: filepath to private key if wanting SSL mode,
 *			else ignored
 * @ssl_ca_filepath: CA certificate filepath or NULL
 * @gid:	group id to change to after setting listen socket, or -1.
 * @uid:	user id to change to after setting listen socket, or -1.
 * @options:	0, or LWS_SERVER_OPTION_DEFEAT_CLIENT_MASK
 * @user:	optional user pointer that can be recovered via the context
 * 		pointer using libwebsocket_context_user 
 *
 *	This function creates the listening socket and takes care
 *	of all initialization in one step.
 *
 *	After initialization, it returns a struct libwebsocket_context * that
 *	represents this server.  After calling, user code needs to take care
 *	of calling libwebsocket_service() with the context pointer to get the
 *	server's sockets serviced.  This can be done in the same process context
 *	or a forked process, or another thread,
 *
 *	The protocol callback functions are called for a handful of events
 *	including http requests coming in, websocket connections becoming
 *	established, and data arriving; it's also called periodically to allow
 *	async transmission.
 *
 *	HTTP requests are sent always to the FIRST protocol in @protocol, since
 *	at that time websocket protocol has not been negotiated.  Other
 *	protocols after the first one never see any HTTP callack activity.
 *
 *	The server created is a simple http server by default; part of the
 *	websocket standard is upgrading this http connection to a websocket one.
 *
 *	This allows the same server to provide files like scripts and favicon /
 *	images or whatever over http and dynamic data over websockets all in
 *	one place; they're all handled in the user callback.
 */

struct libwebsocket_context *
libwebsocket_create_context(int port, const char *interf,
			       struct libwebsocket_protocols *protocols,
			       struct libwebsocket_extension *extensions,
			       const char *ssl_cert_filepath,
			       const char *ssl_private_key_filepath,
			       const char *ssl_ca_filepath,
			       int gid, int uid, unsigned int options,
			       void *user)
{
	int n;
	int m;
	int sockfd = 0;
	int fd;
	struct sockaddr_in serv_addr, cli_addr;
	int opt = 1;
	struct libwebsocket_context *context = NULL;
	unsigned int slen;
	char *p;
	char hostname[1024] = "";
//	struct hostent *he;
	struct libwebsocket *wsi;
	struct sockaddr sa;

#ifdef LWS_OPENSSL_SUPPORT
	SSL_METHOD *method;
	char ssl_err_buf[512];
#endif

	lwsl_info("Initial logging level %d\n", log_level);
	lwsl_info(" FD_HASHTABLE_MODULUS: %u\n", FD_HASHTABLE_MODULUS);
	lwsl_info(" MAX_CLIENTS: %u\n", MAX_CLIENTS);
	lwsl_info(" LWS_MAX_HEADER_NAME_LENGTH: %u\n", LWS_MAX_HEADER_NAME_LENGTH);
	lwsl_info(" LWS_MAX_HEADER_LEN: %u\n", LWS_MAX_HEADER_LEN);
	lwsl_info(" LWS_INITIAL_HDR_ALLOC: %u\n", LWS_INITIAL_HDR_ALLOC);
	lwsl_info(" LWS_ADDITIONAL_HDR_ALLOC: %u\n", LWS_ADDITIONAL_HDR_ALLOC);
	lwsl_info(" MAX_USER_RX_BUFFER: %u\n", MAX_USER_RX_BUFFER);
	lwsl_info(" MAX_BROADCAST_PAYLOAD: %u\n", MAX_BROADCAST_PAYLOAD);
	lwsl_info(" LWS_MAX_PROTOCOLS: %u\n", LWS_MAX_PROTOCOLS);
	lwsl_info(" LWS_MAX_EXTENSIONS_ACTIVE: %u\n", LWS_MAX_EXTENSIONS_ACTIVE);
	lwsl_info(" SPEC_LATEST_SUPPORTED: %u\n", SPEC_LATEST_SUPPORTED);
	lwsl_info(" AWAITING_TIMEOUT: %u\n", AWAITING_TIMEOUT);
	lwsl_info(" CIPHERS_LIST_STRING: '%s'\n", CIPHERS_LIST_STRING);
	lwsl_info(" SYSTEM_RANDOM_FILEPATH: '%s'\n", SYSTEM_RANDOM_FILEPATH);
	lwsl_info(" LWS_MAX_ZLIB_CONN_BUFFER: %u\n", LWS_MAX_ZLIB_CONN_BUFFER);

#ifdef _WIN32
	{
		WORD wVersionRequested;
		WSADATA wsaData;
		int err;
		HMODULE wsdll;

		/* Use the MAKEWORD(lowbyte, highbyte) macro from Windef.h */
		wVersionRequested = MAKEWORD(2, 2);

		err = WSAStartup(wVersionRequested, &wsaData);
		if (err != 0) {
			/* Tell the user that we could not find a usable */
			/* Winsock DLL.                                  */
			lwsl_err("WSAStartup failed with error: %d\n", err);
			return NULL;
		}

		/* default to a poll() made out of select() */
		poll = emulated_poll;

		/* if windows socket lib available, use his WSAPoll */
		wsdll = GetModuleHandle(_T("Ws2_32.dll"));
		if (wsdll)
			poll = (PFNWSAPOLL)GetProcAddress(wsdll, "WSAPoll");
	}
#endif


	context = (struct libwebsocket_context *) malloc(sizeof(struct libwebsocket_context));
	if (!context) {
		lwsl_err("No memory for websocket context\n");
		return NULL;
	}
	context->protocols = protocols;
	context->listen_port = port;
	context->http_proxy_port = 0;
	context->http_proxy_address[0] = '\0';
	context->options = options;
	context->fds_count = 0;
	context->extensions = extensions;
	context->last_timeout_check_s = 0;
    context->user_space = user;

#ifdef WIN32
	context->fd_random = 0;
#else
	context->fd_random = open(SYSTEM_RANDOM_FILEPATH, O_RDONLY);
	if (context->fd_random < 0) {
		lwsl_err("Unable to open random device %s %d\n",
				    SYSTEM_RANDOM_FILEPATH, context->fd_random);
		return NULL;
	}
#endif

#ifdef LWS_OPENSSL_SUPPORT
	context->use_ssl = 0;
	context->ssl_ctx = NULL;
	context->ssl_client_ctx = NULL;
	openssl_websocket_private_data_index = 0;
#endif

	if (options & LWS_SERVER_OPTION_SKIP_SERVER_CANONICAL_NAME) {

		strcpy(context->canonical_hostname, "unknown");

	} else {

		/* find canonical hostname */

		hostname[(sizeof hostname) - 1] = '\0';
		memset(&sa, 0, sizeof(sa));
		sa.sa_family = AF_INET;
		sa.sa_data[(sizeof sa.sa_data) - 1] = '\0';
		gethostname(hostname, (sizeof hostname) - 1);

		n = 0;

		if (strlen(hostname) < sizeof(sa.sa_data) - 1) {
			strcpy(sa.sa_data, hostname);
	//		lwsl_debug("my host name is %s\n", sa.sa_data);
			n = getnameinfo(&sa, sizeof(sa), hostname,
				(sizeof hostname) - 1, NULL, 0, 0);
		}

		if (!n) {
			strncpy(context->canonical_hostname, hostname,
						sizeof context->canonical_hostname - 1);
			context->canonical_hostname[
					sizeof context->canonical_hostname - 1] = '\0';
		} else
			strncpy(context->canonical_hostname, hostname,
						sizeof context->canonical_hostname - 1);

	//	lwsl_debug("context->canonical_hostname = %s\n",
	//						context->canonical_hostname);
	}

	/* split the proxy ads:port if given */

	p = getenv("http_proxy");
	if (p) {
		strncpy(context->http_proxy_address, p,
				       sizeof context->http_proxy_address - 1);
		context->http_proxy_address[
				 sizeof context->http_proxy_address - 1] = '\0';

		p = strchr(context->http_proxy_address, ':');
		if (p == NULL) {
			lwsl_err("http_proxy needs to be ads:port\n");
			return NULL;
		}
		*p = '\0';
		context->http_proxy_port = atoi(p + 1);

		lwsl_debug("Using proxy %s:%u\n",
				context->http_proxy_address,
						      context->http_proxy_port);
	}

	if (port) {

#ifdef LWS_OPENSSL_SUPPORT
		context->use_ssl = ssl_cert_filepath != NULL &&
					       ssl_private_key_filepath != NULL;
		if (context->use_ssl)
			lwsl_info(" Compiled with SSL support, using it\n");
		else
			lwsl_info(" Compiled with SSL support, not using it\n");

#else
		if (ssl_cert_filepath != NULL &&
					     ssl_private_key_filepath != NULL) {
			lwsl_info(" Not compiled for OpenSSl support!\n");
			return NULL;
		}
		lwsl_info(" Compiled without SSL support, "
						       "serving unencrypted\n");
#endif
	}

	/* ignore SIGPIPE */
#ifdef WIN32
#else
	signal(SIGPIPE, sigpipe_handler);
#endif


#ifdef LWS_OPENSSL_SUPPORT

	/* basic openssl init */

	SSL_library_init();

	OpenSSL_add_all_algorithms();
	SSL_load_error_strings();

	openssl_websocket_private_data_index =
		SSL_get_ex_new_index(0, "libwebsockets", NULL, NULL, NULL);

	/*
	 * Firefox insists on SSLv23 not SSLv3
	 * Konq disables SSLv2 by default now, SSLv23 works
	 */

	method = (SSL_METHOD *)SSLv23_server_method();
	if (!method) {
		lwsl_err("problem creating ssl method: %s\n",
			ERR_error_string(ERR_get_error(), ssl_err_buf));
		return NULL;
	}
	context->ssl_ctx = SSL_CTX_new(method);	/* create context */
	if (!context->ssl_ctx) {
		lwsl_err("problem creating ssl context: %s\n",
			ERR_error_string(ERR_get_error(), ssl_err_buf));
		return NULL;
	}

#ifdef SSL_OP_NO_COMPRESSION
	SSL_CTX_set_options(context->ssl_ctx, SSL_OP_NO_COMPRESSION);
#endif
	SSL_CTX_set_options(context->ssl_ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
	SSL_CTX_set_cipher_list(context->ssl_ctx, CIPHERS_LIST_STRING);

	/* client context */

	if (port == CONTEXT_PORT_NO_LISTEN) {
		method = (SSL_METHOD *)SSLv23_client_method();
		if (!method) {
			lwsl_err("problem creating ssl method: %s\n",
				ERR_error_string(ERR_get_error(), ssl_err_buf));
			return NULL;
		}
		/* create context */
		context->ssl_client_ctx = SSL_CTX_new(method);
		if (!context->ssl_client_ctx) {
			lwsl_err("problem creating ssl context: %s\n",
				ERR_error_string(ERR_get_error(), ssl_err_buf));
			return NULL;
		}

#ifdef SSL_OP_NO_COMPRESSION
		SSL_CTX_set_options(context->ssl_client_ctx, SSL_OP_NO_COMPRESSION);
#endif
		SSL_CTX_set_options(context->ssl_client_ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
		SSL_CTX_set_cipher_list(context->ssl_client_ctx, CIPHERS_LIST_STRING);

		/* openssl init for cert verification (for client sockets) */
		if (!ssl_ca_filepath) {
			if (!SSL_CTX_load_verify_locations(
				context->ssl_client_ctx, NULL,
						     LWS_OPENSSL_CLIENT_CERTS))
				lwsl_err(
					"Unable to load SSL Client certs from %s "
					"(set by --with-client-cert-dir= in configure) -- "
					" client ssl isn't going to work",
						     LWS_OPENSSL_CLIENT_CERTS);
		} else
			if (!SSL_CTX_load_verify_locations(
				context->ssl_client_ctx, ssl_ca_filepath,
								  NULL))
				lwsl_err(
					"Unable to load SSL Client certs "
					"file from %s -- client ssl isn't "
					"going to work", ssl_ca_filepath);

		/*
		 * callback allowing user code to load extra verification certs
		 * helping the client to verify server identity
		 */

		context->protocols[0].callback(context, NULL,
			LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS,
			context->ssl_client_ctx, NULL, 0);
	}

	/* as a server, are we requiring clients to identify themselves? */

	if (options & LWS_SERVER_OPTION_REQUIRE_VALID_OPENSSL_CLIENT_CERT) {

		/* absolutely require the client cert */

		SSL_CTX_set_verify(context->ssl_ctx,
		       SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
						       OpenSSL_verify_callback);

		/*
		 * give user code a chance to load certs into the server
		 * allowing it to verify incoming client certs
		 */

		context->protocols[0].callback(context, NULL,
			LWS_CALLBACK_OPENSSL_LOAD_EXTRA_SERVER_VERIFY_CERTS,
						     context->ssl_ctx, NULL, 0);
	}

	if (context->use_ssl) {

		/* openssl init for server sockets */

		/* set the local certificate from CertFile */
		n = SSL_CTX_use_certificate_chain_file(context->ssl_ctx,
					ssl_cert_filepath);
		if (n != 1) {
			lwsl_err("problem getting cert '%s': %s\n",
				ssl_cert_filepath,
				ERR_error_string(ERR_get_error(), ssl_err_buf));
			return NULL;
		}
		/* set the private key from KeyFile */
		if (SSL_CTX_use_PrivateKey_file(context->ssl_ctx,
			     ssl_private_key_filepath, SSL_FILETYPE_PEM) != 1) {
			lwsl_err("ssl problem getting key '%s': %s\n",
						ssl_private_key_filepath,
				ERR_error_string(ERR_get_error(), ssl_err_buf));
			return NULL;
		}
		/* verify private key */
		if (!SSL_CTX_check_private_key(context->ssl_ctx)) {
			lwsl_err("Private SSL key doesn't match cert\n");
			return NULL;
		}

		/* SSL is happy and has a cert it's content with */
	}
#endif

	/* selftest */

	if (lws_b64_selftest())
		return NULL;

	/* fd hashtable init */

	for (n = 0; n < FD_HASHTABLE_MODULUS; n++)
		context->fd_hashtable[n].length = 0;

	/* set up our external listening socket we serve on */

	if (port) {

		sockfd = socket(AF_INET, SOCK_STREAM, 0);
		if (sockfd < 0) {
			lwsl_err("ERROR opening socket\n");
			return NULL;
		}

		/* allow us to restart even if old sockets in TIME_WAIT */
		setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
					      (const void *)&opt, sizeof(opt));

		/* Disable Nagle */
		opt = 1;
		setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY,
					      (const void *)&opt, sizeof(opt));

		bzero((char *) &serv_addr, sizeof(serv_addr));
		serv_addr.sin_family = AF_INET;
		if (interf == NULL)
			serv_addr.sin_addr.s_addr = INADDR_ANY;
		else
			interface_to_sa(interf, &serv_addr,
						sizeof(serv_addr));
		serv_addr.sin_port = htons(port);

		n = bind(sockfd, (struct sockaddr *) &serv_addr,
							     sizeof(serv_addr));
		if (n < 0) {
			lwsl_err("ERROR on binding to port %d (%d %d)\n",
								port, n, errno);
			close(sockfd);
			return NULL;
		}

		wsi = (struct libwebsocket *)malloc(sizeof(struct libwebsocket));
		if (wsi == NULL) {
			lwsl_err("Out of mem\n");
			close(sockfd);
			return NULL;
		}
		memset(wsi, 0, sizeof (struct libwebsocket));
		wsi->sock = sockfd;
		wsi->count_active_extensions = 0;
		wsi->mode = LWS_CONNMODE_SERVER_LISTENER;
		insert_wsi(context, wsi);

		context->listen_service_modulo = LWS_LISTEN_SERVICE_MODULO;
		context->listen_service_count = 0;
		context->listen_service_fd = sockfd;

		listen(sockfd, LWS_SOMAXCONN);
		lwsl_info(" Listening on port %d\n", port);

		/* list in the internal poll array - we're always first */

		context->fds[context->fds_count].fd = sockfd;
		context->fds[context->fds_count++].events = POLLIN;

		/* external POLL support via protocol 0 */
		context->protocols[0].callback(context, wsi,
			LWS_CALLBACK_ADD_POLL_FD,
			(void *)(long)sockfd, NULL, POLLIN);

	}

	/*
	 * drop any root privs for this process
	 * to listen on port < 1023 we would have needed root, but now we are
	 * listening, we don't want the power for anything else
	 */
#ifdef WIN32
#else
	if (gid != -1)
		if (setgid(gid))
			lwsl_warn("setgid: %s\n", strerror(errno));
	if (uid != -1)
		if (setuid(uid))
			lwsl_warn("setuid: %s\n", strerror(errno));
#endif

	/* set up our internal broadcast trigger sockets per-protocol */

	for (context->count_protocols = 0;
			protocols[context->count_protocols].callback;
						   context->count_protocols++) {

		lwsl_parser("  Protocol: %s\n",
				protocols[context->count_protocols].name);

		protocols[context->count_protocols].owning_server = context;
		protocols[context->count_protocols].protocol_index =
						       context->count_protocols;

		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0) {
			lwsl_err("ERROR opening socket\n");
			return NULL;
		}

		/* allow us to restart even if old sockets in TIME_WAIT */
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&opt,
								  sizeof(opt));

		bzero((char *) &serv_addr, sizeof(serv_addr));
		serv_addr.sin_family = AF_INET;
		serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
		serv_addr.sin_port = 0; /* pick the port for us */

		n = bind(fd, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
		if (n < 0) {
			lwsl_err("ERROR on binding to port %d (%d %d)\n",
								port, n, errno);
			return NULL;
		}

		slen = sizeof cli_addr;
		n = getsockname(fd, (struct sockaddr *)&cli_addr, &slen);
		if (n < 0) {
			lwsl_err("getsockname failed\n");
			return NULL;
		}
		protocols[context->count_protocols].broadcast_socket_port =
						       ntohs(cli_addr.sin_port);
		listen(fd, 5);

		lwsl_debug("  Protocol %s broadcast socket %d\n",
				protocols[context->count_protocols].name,
						      ntohs(cli_addr.sin_port));

		/* dummy wsi per broadcast proxy socket */

		wsi = (struct libwebsocket *)malloc(sizeof(struct libwebsocket));
		if (wsi == NULL) {
			lwsl_err("Out of mem\n");
			close(fd);
			return NULL;
		}
		memset(wsi, 0, sizeof (struct libwebsocket));
		wsi->sock = fd;
		wsi->mode = LWS_CONNMODE_BROADCAST_PROXY_LISTENER;
		wsi->count_active_extensions = 0;
		/* note which protocol we are proxying */
		wsi->protocol_index_for_broadcast_proxy =
						       context->count_protocols;
		insert_wsi(context, wsi);

		/* list in internal poll array */

		context->fds[context->fds_count].fd = fd;
		context->fds[context->fds_count].events = POLLIN;
		context->fds[context->fds_count].revents = 0;
		context->fds_count++;

		/* external POLL support via protocol 0 */
		context->protocols[0].callback(context, wsi,
			LWS_CALLBACK_ADD_POLL_FD,
			(void *)(long)fd, NULL, POLLIN);
	}

	/*
	 * give all extensions a chance to create any per-context
	 * allocations they need
	 */

	m = LWS_EXT_CALLBACK_CLIENT_CONTEXT_CONSTRUCT;
	if (port)
		m = LWS_EXT_CALLBACK_SERVER_CONTEXT_CONSTRUCT;
	
	if (extensions) {
	    while (extensions->callback) {
		    lwsl_ext("  Extension: %s\n", extensions->name);
		    extensions->callback(context, extensions, NULL,
			(enum libwebsocket_extension_callback_reasons)m,
								NULL, NULL, 0);
		    extensions++;
	    }
	}

	return context;
}


#ifndef LWS_NO_FORK

/**
 * libwebsockets_fork_service_loop() - Optional helper function forks off
 *				  a process for the websocket server loop.
 *				You don't have to use this but if not, you
 *				have to make sure you are calling
 *				libwebsocket_service periodically to service
 *				the websocket traffic
 * @context:	server context returned by creation function
 */

int
libwebsockets_fork_service_loop(struct libwebsocket_context *context)
{
	int fd;
	struct sockaddr_in cli_addr;
	int n;
	int p;

	n = fork();
	if (n < 0)
		return n;

	if (!n) {

		/* main process context */

		/*
		 * set up the proxy sockets to allow broadcast from
		 * service process context
		 */

		for (p = 0; p < context->count_protocols; p++) {
			fd = socket(AF_INET, SOCK_STREAM, 0);
			if (fd < 0) {
				lwsl_err("Unable to create socket\n");
				return -1;
			}
			cli_addr.sin_family = AF_INET;
			cli_addr.sin_port = htons(
			     context->protocols[p].broadcast_socket_port);
			cli_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
			n = connect(fd, (struct sockaddr *)&cli_addr,
							       sizeof cli_addr);
			if (n < 0) {
				lwsl_err("Unable to connect to "
						"broadcast socket %d, %s\n",
						n, strerror(errno));
				return -1;
			}

			context->protocols[p].broadcast_socket_user_fd = fd;
		}

		return 0;
	}

#ifdef HAVE_SYS_PRCTL_H
	/* we want a SIGHUP when our parent goes down */
	prctl(PR_SET_PDEATHSIG, SIGHUP);
#endif

	/* in this forked process, sit and service websocket connections */

	while (1) {
		if (libwebsocket_service(context, 1000))
			break;
//#ifndef HAVE_SYS_PRCTL_H
/*
 * on systems without prctl() (i.e. anything but linux) we can notice that our
 * parent is dead if getppid() returns 1. FIXME apparently this is not true for
 * solaris, could remember ppid right after fork and wait for it to change.
 */

        if (getppid() == 1)
            break;
//#endif
    }


	return 1;
}

#endif

/**
 * libwebsockets_get_protocol() - Returns a protocol pointer from a websocket
 *				  connection.
 * @wsi:	pointer to struct websocket you want to know the protocol of
 *
 *
 *	This is useful to get the protocol to broadcast back to from inside
 * the callback.
 */

const struct libwebsocket_protocols *
libwebsockets_get_protocol(struct libwebsocket *wsi)
{
	return wsi->protocol;
}

/**
 * libwebsockets_broadcast() - Sends a buffer to the callback for all active
 *				  connections of the given protocol.
 * @protocol:	pointer to the protocol you will broadcast to all members of
 * @buf:  buffer containing the data to be broadcase.  NOTE: this has to be
 *		allocated with LWS_SEND_BUFFER_PRE_PADDING valid bytes before
 *		the pointer and LWS_SEND_BUFFER_POST_PADDING afterwards in the
 *		case you are calling this function from callback context.
 * @len:	length of payload data in buf, starting from buf.
 *
 *	This function allows bulk sending of a packet to every connection using
 * the given protocol.  It does not send the data directly; instead it calls
 * the callback with a reason type of LWS_CALLBACK_BROADCAST.  If the callback
 * wants to actually send the data for that connection, the callback itself
 * should call libwebsocket_write().
 *
 * libwebsockets_broadcast() can be called from another fork context without
 * having to take any care about data visibility between the processes, it'll
 * "just work".
 */


int
libwebsockets_broadcast(const struct libwebsocket_protocols *protocol,
						 unsigned char *buf, size_t len)
{
	struct libwebsocket_context *context = protocol->owning_server;
	int n;
	int m;
	struct libwebsocket *wsi;

	if (!protocol->broadcast_socket_user_fd) {
		/*
		 * We are either running unforked / flat, or we are being
		 * called from poll thread context
		 * eg, from a callback.  In that case don't use sockets for
		 * broadcast IPC (since we can't open a socket connection to
		 * a socket listening on our own thread) but directly do the
		 * send action.
		 *
		 * Locking is not needed because we are by definition being
		 * called in the poll thread context and are serialized.
		 */

		for (n = 0; n < FD_HASHTABLE_MODULUS; n++) {

			for (m = 0; m < context->fd_hashtable[n].length; m++) {

				wsi = context->fd_hashtable[n].wsi[m];

				if (wsi->mode != LWS_CONNMODE_WS_SERVING)
					continue;

				/*
				 * never broadcast to
				 * non-established connections
				 */
				if (wsi->state != WSI_STATE_ESTABLISHED)
					continue;

				/* only broadcast to guys using
				 * requested protocol
				 */
				if (wsi->protocol != protocol)
					continue;

				wsi->protocol->callback(context, wsi,
					 LWS_CALLBACK_BROADCAST,
					 wsi->user_space,
					 buf, len);
			}
		}

		return 0;
	}

	/*
	 * We're being called from a different process context than the server
	 * loop.  Instead of broadcasting directly, we send our
	 * payload on a socket to do the IPC; the server process will serialize
	 * the broadcast action in its main poll() loop.
	 *
	 * There's one broadcast socket listening for each protocol supported
	 * set up when the websocket server initializes
	 */

	n = send(protocol->broadcast_socket_user_fd, buf, len, MSG_NOSIGNAL);

	return n;
}

int
libwebsocket_is_final_fragment(struct libwebsocket *wsi)
{
	return wsi->final;
}

unsigned char
libwebsocket_get_reserved_bits(struct libwebsocket *wsi)
{
	return wsi->rsv;
}

void *
libwebsocket_ensure_user_space(struct libwebsocket *wsi)
{
	/* allocate the per-connection user memory (if any) */

	if (wsi->protocol->per_session_data_size && !wsi->user_space) {
		wsi->user_space = malloc(
				  wsi->protocol->per_session_data_size);
		if (wsi->user_space  == NULL) {
			lwsl_err("Out of memory for conn user space\n");
			return NULL;
		}
		memset(wsi->user_space, 0,
					 wsi->protocol->per_session_data_size);
	}
	return wsi->user_space;
}


static void lwsl_emit_stderr(const char *line)
{
	fprintf(stderr, "%s", line);
}

void _lws_log(int filter, const char *format, ...)
{
	char buf[256];
	va_list ap;
	int n;
	int pos = 0;
	struct timeval tv;

	if (!(log_level & filter))
		return;

	gettimeofday(&tv, NULL);

	for (n = 0; n < LLL_COUNT; n++)
		if (filter == (1 << n)) {
			pos = sprintf(buf, "[%ld:%04ld] %s: ", tv.tv_sec,
					tv.tv_usec / 100, log_level_names[n]);
			break;
		}

	va_start(ap, format);
	vsnprintf(buf + pos, (sizeof buf) - pos, format, ap);
	buf[(sizeof buf) - 1] = '\0';
	va_end(ap);

	lwsl_emit(buf);
}

/**
 * lws_set_log_level() - Set the logging bitfield
 * @level:	OR together the LLL_ debug contexts you want output from
 * @log_emit_function:	NULL to leave it as it is, or a user-supplied
 *			function to perform log string emission instead of
 *			the default stderr one.
 *
 *	log level defaults to "err" and "warn" contexts enabled only and
 *	emission on stderr.
 */

void lws_set_log_level(int level, void (*log_emit_function)(const char *line))
{
	log_level = level;
	if (log_emit_function)
		lwsl_emit = log_emit_function;
}
