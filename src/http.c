/*
 * $Id$
 *
 * Copyright (c) 2002, Raphael Manfredi
 *
 * HTTP routines.
 *
 * The whole HTTP logic is not contained here.  Only generic supporting
 * routines are here.
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

#include "gnutella.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>

#include "http.h"
#include "sockets.h"
#include "bsched.h"
#include "header.h"
#include "walloc.h"

#define MAX_HOSTLEN		256		/* Max length for FQDN host */

/*
 * http_timer
 *
 * Called from main timer to expire HTTP requests that take too long.
 */
void http_timer(time_t now)
{
	// XXX handle list, put ha in them monitor last_update
}

/*
 * http_send_status
 *
 * Send HTTP status on socket, with code and reason.
 *
 * If `hev' is non-null, it points to a vector of http_extra_desc_t items,
 * containing `hevcnt' entries.  Each entry describes something to be
 * inserted in the header.
 *
 * The connection is NOT closed physically.
 *
 * At the HTTP level, the connection is closed if an error is returned
 * (either 4xx or 5xx) or a redirection occurs (3xx).
 *
 * Returns TRUE if we were able to send everything, FALSE otherwise.
 */
gboolean http_send_status(
	struct gnutella_socket *s, gint code,
	http_extra_desc_t *hev, gint hevcnt,
	gchar *reason, ...)
{
	gchar header[1536];			/* 1.5 K max */
	gchar status_msg[512];
	gint rw;
	gint mrw;
	gint sent;
	gint i;
	va_list args;
	gchar *conn_close = "Connection: close\r\n";

	va_start(args, reason);
	g_vsnprintf(status_msg, sizeof(status_msg)-1,  reason, args);
	va_end(args);

	if (code < 300)
		conn_close = "";		/* Keep HTTP connection */

	rw = g_snprintf(header, sizeof(header),
		"HTTP/1.1 %d %s\r\n"
		"Server: %s\r\n"
		"%s"
		"X-Live-Since: %s\r\n",
		code, status_msg, version_string, conn_close, start_rfc822_date);

	mrw = rw;		/* Minimal header length */

	/*
	 * Append extra information to the minimal header created above.
	 */

	for (i = 0; i < hevcnt && rw < sizeof(header); i++) {
		http_extra_desc_t *he = &hev[i];
		http_extra_type_t type = he->he_type;

		switch (type) {
		case HTTP_EXTRA_LINE:
			rw += g_snprintf(&header[rw], sizeof(header) - rw,
				"%s", he->he_msg);
			break;
		case HTTP_EXTRA_CALLBACK:
			{
				/* The -3 is there to leave room for "\r\n" + NUL */
				gint len = sizeof(header) - rw - 3;
				
				(*he->he_cb)(&header[rw], &len, he->he_arg);

				g_assert(len + rw <= sizeof(header));

				rw += len;
			}
			break;
		}
	}

	if (rw < sizeof(header))
		rw += g_snprintf(&header[rw], sizeof(header) - rw, "\r\n");

	if (rw >= sizeof(header) && hev) {
		g_warning("HTTP status %d (%s) too big, ignoring extra information",
			code, reason);

		rw = mrw + g_snprintf(&header[mrw], sizeof(header) - mrw, "\r\n");
		g_assert(rw < sizeof(header));
	}

	if (-1 == (sent = bws_write(bws.out, s->file_desc, header, rw))) {
		g_warning("Unable to send back HTTP status %d (%s) to %s: %s",
			code, reason, ip_to_gchar(s->ip), g_strerror(errno));
		return FALSE;
	} else if (sent < rw) {
		g_warning("Only sent %d out of %d bytes of status %d (%s) to %s: %s",
			sent, rw, code, reason, ip_to_gchar(s->ip), g_strerror(errno));
		return FALSE;
	} else if (dbg > 2) {
		printf("----Sent HTTP Status to %s:\n%.*s----\n",
			ip_to_gchar(s->ip), rw, header);
		fflush(stdout);
	}

	return TRUE;
}

/***
 *** HTTP parsing.
 ***/

/*
 * code_message_parse
 *
 * Parse status messages formed of leading digit numbers, then an optional
 * message.  The pointer to the start of the message is returned in `msg'
 * if it is non-null.
 *
 * Returns status code, -1 on error.
 */
static gint code_message_parse(gchar *line, gchar **msg)
{
	gchar *p;
	guchar code[4];
	gint c;
	gint i;
	gint status;

	/*
	 * We expect exactly 3 status digits.
	 */

	for (i = 0, p = line; i < 3; i++, p++) {
		c = *p;
		if (!isdigit(c))
			return -1;
		code[i] = c;
	}
	code[3] = '\0';

	status = atoi(code);

	/*
	 * Make sure we have at least a space after the code, or that we
	 * reached the end of the string.
	 */

	c = *p;

	if (c == '\0') {			/* 3 digits followed by a space */
		if (msg)
			*msg = p;			/* Points to the trailing NUL */
		return status;
	}

	if (!isspace(c))			/* 3 digits NOT followed by a space */
		return -1;

	if (!msg)
		return status;			/* No need to point to start of message */

	/*
	 * Now skip any further space.
	 */

	for (c = *(++p); c; c = *(++p)) {
		if (!isspace(c))
			break;
	}

	*msg = p;					/* This is the beginning of the message */

	return status;
}

/*
 * http_status_parse
 *
 * Parse protocol status line, and return the status code, and optionally a
 * pointer within the string where the status message starts (if `msg' is
 * a non-null pointer), and the protocol major/minor (if `major' and `minor'
 * are non-null).
 *
 * If `proto' is non-null, then when there is a leading protocol string in
 * the reply, it must be equal to `proto'.
 *
 * Returns -1 if it fails to parse the status line correctly, the status code
 * otherwise.
 *
 * We recognize the following status lines:
 *
 *     ZZZ 403 message                        (major=-1, minor=-1)
 *     ZZZ/2.3 403 message                    (major=2, minor=3)
 *     403 message                            (major=-1, minor=-1)
 *
 * We don't yet handle "SMTP-like continuations":
 *
 *     403-message line #1
 *     403-message line #2
 *     403 last message line
 *
 * There is no way to return the value of "ZZZ" via this routine.
 *
 * NB: this routine is also used to parse GNUTELLA status codes, since
 * they follow the same pattern as HTTP status codes.
 */
gint http_status_parse(gchar *line,
	gchar *proto, gchar **msg, gint *major, gint *minor)
{
	gint c;
	gchar *p;

	/*
	 * Skip leading spaces.
	 */

	for (p = line, c = *p; c; c = *(++p)) {
		if (!isspace(c))
			break;
	}

	/*
	 * If first character is a digit, then we have simply:
	 *
	 *   403 message
	 *
	 * There's no known protocol information.
	 */

	if (c == '\0')
		return -1;					/* Empty line */

	if (isdigit(c)) {
		if (major)
			*major = -1;
		if (minor)
			*minor = -1;
		return code_message_parse(p, msg);
	}

	/*
	 * Check protocol.
	 */

	if (proto) {
		gint plen = strlen(proto);
		if (0 == strncmp(proto, line, plen)) {
			/*
			 * Protocol string matches, make sure it ends with a space or
			 * a "/" delimiter.
			 */

			p = &line[plen];
			c = *p;					/* Can dereference, at worst it's a NUL */
			if (c == '\0')			/* Only "protocol" name in status */
				return -1;
			if (!isspace(c) && c != '/')
				return -1;
		} else
			return -1;
	} else {
		/*
		 * Move along the string until we find a space or a "/".
		 */

		for (/* empty */; c; c = *(++p)) {
			if (c == '/' || isspace(c))
				break;
		}
	}

	if (c == '\0')
		return -1;

	/*
	 * We've got a "/", parse protocol version number, then move past
	 * to the first space.
	 */

	if (c == '/') {
		gint maj, min;
		if (major || minor) {
			if (sscanf(p+1, "%d.%d", &maj, &min)) {
				if (major)
					*major = maj;
				if (minor)
					*minor = min;
			} else
				return -1;
		}

		for (c = *(++p); c; c = *(++p)) {
			if (isspace(c))
				break;
		}

		if (c == '\0')
			return -1;
	}

	g_assert(isspace(c));

	/*
	 * Now strip leading spaces.
	 */

	for (c = *(++p); c; c = *(++p)) {
		if (!isspace(c))
			break;
	}

	if (c == '\0')
		return -1;

	if (!isdigit(c))
		return -1;

	return code_message_parse(p, msg);
}

/*
 * http_url_parse
 *
 * Parse HTTP url and extract the IP/port we need to connect to.
 * Also identifies the start of the path to request on the server.
 *
 * Returns TRUE if the URL was correctly parsed, with `ip', `port' and `path'
 * filled, FALSE otherwise.
 */
gboolean http_url_parse(gchar *url, guint32 *ip, guint16 *port, gchar **path)
{
	gchar *host_start;
	gchar *port_start;
	gchar *p;
	gchar c;
	gboolean seen_upw = FALSE;
	gchar s;
	guint32 portnum;

	if (0 != strncasecmp(url, "http://", 7))
		return FALSE;

	url += 7;

	/*
	 * The general URL syntax is (RFC-1738):
	 *
	 *	//<user>:<password>@<host>:<port>/<url-path>
	 *
	 * Any special character in <user> or <password> (i.e. '/', ':' or '@')
	 * must be URL-encoded, naturally.
	 *
	 * In the code below, we don't care about the user/password and simply
	 * skip them if they are present.
	 */

	host_start = url;		/* Assume there's no <user>:<password> */
	port_start = NULL;		/* Port not seen yet */
	p = url + 1;

	while ((c = *p++)) {
		if (c == '@') {
			if (seen_upw)			/* There can be only ONE user/password */
				return FALSE;
			seen_upw = TRUE;
			host_start = p;			/* Right after the '@' */
			port_start = NULL;
		} else if (c == ':')
			port_start = p;			/* Right after the ':' */
		else if (c == '/')
			break;
	}

	p--;							/* Go back to trailing "/" */
	if (*p != '/')
		return FALSE;
	*path = p;						/* Start of path, at the "/" */

	/*
	 * Validate the port.
	 */

	if (port_start == NULL)
		portnum = HTTP_PORT;
	else if (2 != sscanf(port_start, "%u%c", &portnum, &s) || s != '/')
		return FALSE;

	if ((guint32) (guint16) portnum != portnum)
		return FALSE;

	*port = (guint16) portnum;

	/*
	 * Validate the host.
	 */

	if (isdigit(*host_start)) {
		guint lsb, b2, b3, msb;
		if (
			5 == sscanf(host_start, "%u.%u.%u.%u%c", &msb, &b3, &b2, &lsb, &s)
			&& (s == '/' || s == ':')
		)
			*ip = lsb + (b2 << 8) + (b3 << 16) + (msb << 24);
		else
			return FALSE;
	} else {
		gchar host[MAX_HOSTLEN + 1];
		gchar *q = host;
		gchar *end = host + sizeof(host);

		/*
		 * Extract hostname into host[].
		 */

		p = host_start;
		while ((c = *p++) && q < end) {
			if (c == '/' || c == ':') {
				*q++ = '\0';
				break;
			}
			*q++ = c;
		}
		host[MAX_HOSTLEN] = '\0';

		*ip = host_to_ip(host);
		if (*ip == 0)					/* Unable to resolve name */
			return FALSE;
	}

	if (dbg > 12)
		printf("URL \"%s\" -> host=%s, path=%s\n",
			url, ip_port_to_gchar(*ip, *port), *path);

	return TRUE;
}

/***
 *** Asynchronous HTTP error code management.
 ***/

static gchar *error_str[] = {
	"OK",									/* HTTP_ASYNC_OK */
	"Invalid HTTP URL",						/* HTTP_ASYNC_BAD_URL */
	"Connection failed",					/* HTTP_ASYNC_CONN_FAILED */
	"I/O error",							/* HTTP_ASYNC_IO_ERROR */
};

gint http_async_errno;		/* Used to return error codes during setup */

#define MAX_ERRNUM (sizeof(error_str) / sizeof(error_str[0]) - 1)

/*
 * http_async_strerror
 *
 * Return human-readable error string corresponding to error code `errnum'.
 */
gchar *http_async_strerror(gint errnum)
{
	if (errnum < 0 || errnum > MAX_ERRNUM)
		return "Invalid error code";

	return error_str[errnum];
}

/***
 *** Asynchronous HTTP transactions.
 ***/

enum http_reqtype {
	HTTP_GET,
	HTTP_POST,
};

struct http_async {					/* An asynchronous HTTP request */
	enum http_reqtype type;			/* Type of request */
	gchar *path;					/* Path to request (atom) */
	struct gnutella_socket *socket;	/* Attached socket */
	http_data_cb_t data_ind;		/* Callback for data */
	http_error_cb_t error_ind;		/* Callback for errors */
	header_t *header;				/* Parsed HTTP reply header */
	time_t last_update;				/* Time of last activity */
};

/*
 * http_async_get
 *
 * Starts an asynchronous HTTP GET request on the specified path.
 * Returns a handle on the request if OK, NULL on error with the
 * http_async_errno variable set before returning.
 *
 * When data is available, `data_ind' will be called.  When all data have been
 * read, a final call to `data_ind' is made with no data.
 *
 * On error, `error_ind' will be called, and upon return, the request will
 * be automatically cancelled.
 */
gpointer http_async_get(
	gchar *url,
	http_data_cb_t data_ind,
	http_error_cb_t error_ind)
{
	guint32 ip;
	guint16 port;
	gchar *path;
	struct gnutella_socket *s;
	struct http_async *ha;

	g_assert(url);
	g_assert(data_ind);
	g_assert(error_ind);

	/*
	 * Extract the necessary parameters for the connection.
	 */

	if (!http_url_parse(url, &ip, &port, &path)) {
		http_async_errno = HTTP_ASYNC_BAD_URL;
		return NULL;
	}

	/*
	 * Attempt asynchronous connection.
	 *
	 * When connection is established, http_async_connected() will be called
	 * from the socket layer.
	 */

	s = socket_connect(ip, port, SOCK_TYPE_HTTP);

	if (s == NULL) {
		http_async_errno = HTTP_ASYNC_CONN_FAILED;
		return NULL;
	}

	/*
	 * Connection started, build handle and return.
	 */

	ha = walloc(sizeof(*ha));

	s->resource.handle = ha;

	ha->type = HTTP_GET;
	ha->path = atom_str_get(path);
	ha->socket = s;
	ha->data_ind = data_ind;
	ha->error_ind = error_ind;
	ha->header = NULL;
	ha->last_update = time(NULL);

	return ha;
}

/*
 * http_async_connected
 *
 * Callback from the socket layer when the connection to the remote
 * server is made.
 */
void http_async_connected(gpointer handle)
{
	struct http_async *ha = (struct http_async *) handle;
	struct gnutella_socket *s = ha->socket;
	gchar req[2048];
	gint rw;
	gint sent;

	g_assert(s);
	g_assert(s->resource.handle == handle);

	/*
	 * Create the HTTP request.
	 */

	rw = g_snprintf(req, sizeof(req),
		"%s %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"User-Agent: %s\r\n"
		"Connection: close\r\n",
		ha->type == HTTP_POST ? "POST" : "GET", ha->path,
		ip_port_to_gchar(s->ip, s->port),
		version_string);

	if (rw >= sizeof(req)) {
		http_async_cancel(ha, HTTP_ASYNC_REQ2BIG);
		return;
	}

	/*
	 * Send the HTTP request.
	 */

	if (-1 == (sent = bws_write(bws.out, s->file_desc, req, rw))) {
		g_warning("HTTP request sending to %s failed: %s",
			ip_port_to_gchar(s->ip, s->port), g_strerror(errno));
		http_async_cancel(ha, HTTP_ASYNC_IO_ERROR);
		return;
	} else if (sent < rw) {
		g_warning("HTTP request sending to %s: only %d of %d bytes sent",
			ip_port_to_gchar(s->ip, s->port), sent, rw);
		http_async_cancel(ha, HTTP_ASYNC_IO_ERROR);
		return;
	} else if (dbg > 2) {
		printf("----Sent HTTP request to %s:\n%.*s----\n",
			ip_port_to_gchar(s->ip, s->port), (int) rw, req);
		fflush(stdout);
	}

	ha->last_update = time(NULL);
	
	/*
	 * Prepare to read back the status line and the headers.
	 */

	// XXXX
}

/*
 * http_async_free
 *
 * Free the HTTP asynchronous request handler, disposing of all its
 * attached resources.
 */
static void http_async_free(struct http_async *ha)
{
	socket_free(ha->socket);
	atom_str_free(ha->path);
	if (ha->header)
		header_free(ha->header);

	wfree(ha, sizeof(*ha));
}

/*
 * http_async_cancel
 *
 * Cancel request.
 */
void http_async_cancel(gpointer handle, gint code)
{
	struct http_async *ha = (struct http_async *) handle;

	(*ha->error_ind)(handle, HTTP_ASYNC_ERROR, (gpointer) code);
	http_async_free(ha);
}

