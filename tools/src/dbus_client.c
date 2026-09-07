/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * dbus_client.c - minimal zero-dependency D-Bus client for the lgmagic
 * CLI (portable).
 *
 * Hand-implements the D-Bus wire protocol subset the CLI needs:
 *
 *   - AUTH EXTERNAL + BEGIN handshake over a unix socket (the uid goes
 *     as a decimal string, hex-encoded as the initial-response argument)
 *   - METHOD_CALL marshaling: header fields PATH/INTERFACE/MEMBER/
 *     DESTINATION/SIGNATURE, input types s (string), d (double), u (u32)
 *   - reply parsing: METHOD_RETURN with s / as / v(s) / empty body,
 *     ERROR replies (name + message), reply-serial matching
 *   - one call in flight, 25 s timeout, no signals/introspection
 *
 * Everything is little-endian (the native order of every supported host)
 * and the implementation is pure libc - `ldd lgmagic` stays clean.
 *
 * Wire format notes (D-Bus specification, Message Format):
 *   - fixed 16-byte header: 'l', type, flags, version, body len u32,
 *     serial u32, header fields len u32; header padded to 8
 *   - every value is aligned to its size from the start of the message,
 *     except header field variant values which are always 8-aligned
 *   - string: u32 byte length (no NUL), data, NUL, padded to 4
 *   - the body starts on an 8-byte boundary, so alignments relative to
 *     the body start equal the absolute ones
 */
#include "dbus_client.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define CALL_TIMEOUT_MS 25000
#define MAX_MESSAGE (1u << 20)	/* sanity cap for header+body */

enum {
	MSG_TYPE_CALL = 1,
	MSG_TYPE_RETURN = 2,
	MSG_TYPE_ERROR = 3,
	MSG_TYPE_SIGNAL = 4,
};

enum {
	FIELD_PATH = 1,
	FIELD_INTERFACE = 2,
	FIELD_MEMBER = 3,
	FIELD_ERROR_NAME = 4,
	FIELD_REPLY_SERIAL = 5,
	FIELD_DESTINATION = 6,
	FIELD_SENDER = 7,
	FIELD_SIGNATURE = 8,
};

/* ----------------------------------------------------------------------
 * little-endian byte writer
 * ---------------------------------------------------------------------- */

struct wb {
	unsigned char *b;
	size_t n, cap;
	size_t nodepad;		/* n before the most recent padding */
	int oom;
};

static void wb_put(struct wb *w, const void *p, size_t n)
{
	if (w->oom)
		return;
	if (w->n + n > w->cap) {
		size_t ncap = w->cap ? w->cap : 128;
		unsigned char *nb;

		while (ncap < w->n + n)
			ncap *= 2;
		nb = realloc(w->b, ncap);
		if (!nb) {
			w->oom = 1;
			return;
		}
		w->b = nb;
		w->cap = ncap;
	}
	memcpy(w->b + w->n, p, n);
	w->n += n;
	w->nodepad = w->n;
}

static void wb_u8(struct wb *w, unsigned v)
{
	unsigned char b = (unsigned char)v;

	wb_put(w, &b, 1);
}

static void wb_u32(struct wb *w, uint32_t v)
{
	unsigned char b[4] = {
		(unsigned char)v, (unsigned char)(v >> 8),
		(unsigned char)(v >> 16), (unsigned char)(v >> 24),
	};

	wb_put(w, b, sizeof b);
}

static void wb_u64(struct wb *w, uint64_t v)
{
	unsigned char b[8];
	size_t i;

	for (i = 0; i < 8; i++)
		b[i] = (unsigned char)(v >> (8 * i));
	wb_put(w, b, sizeof b);
}

static void wb_pad(struct wb *w, size_t align)
{
	size_t pre = w->n;

	while (w->n % align)
		wb_u8(w, 0);
	w->nodepad = pre;	/* wb_u8/wb_put moved nodepad; restore */
}

/* string: u32 byte length, data, NUL, padded to 4 */
static void wb_str(struct wb *w, const char *s)
{
	size_t len = strlen(s);

	if (len > 0x0fffffffu) {
		w->oom = 1;
		return;
	}
	wb_u32(w, (uint32_t)len);
	wb_put(w, s, len);
	wb_u8(w, 0);
	wb_pad(w, 4);
}

/* signature value ('g'): u8 byte length, data, NUL */
static void wb_sig(struct wb *w, const char *sig)
{
	size_t len = strlen(sig);

	if (len > 255) {
		w->oom = 1;
		return;
	}
	wb_u8(w, (unsigned)len);
	wb_put(w, sig, len);
	wb_u8(w, 0);
}

/* ----------------------------------------------------------------------
 * method call marshaling
 * ---------------------------------------------------------------------- */

static int body_add_arg(struct wb *w, const struct dbus_arg *a)
{
	switch (a->type) {
	case 's':
		wb_pad(w, 4);
		wb_str(w, a->s);
		break;
	case 'd': {
		uint64_t bits;

		/* both supported hosts are little-endian IEEE 754 */
		memcpy(&bits, &a->d, sizeof bits);
		wb_pad(w, 8);
		wb_u64(w, bits);
		break;
	}
	case 'u':
		wb_pad(w, 4);
		wb_u32(w, a->u);
		break;
	default:
		return -1;
	}
	return 0;
}

/* one header field: [code][variant signature as a 'g' value][value].
 * Each struct of the a(yv) array is 8-aligned; the variant itself aligns
 * to 1 and starts right after the code byte, carrying its signature as a
 * full SIGNATURE value (u8 length + chars + NUL).  The contained value is
 * then aligned per its type, globally from the message start. */
static void hdr_field(struct wb *w, unsigned code, unsigned char sig,
		      void (*val)(struct wb *, const char *), const char *data)
{
	wb_pad(w, 8);
	wb_u8(w, code);
	wb_u8(w, 1);		/* signature length (single char) */
	wb_u8(w, sig);
	wb_u8(w, 0);		/* signature NUL terminator */
	if (sig == 'o' || sig == 's' || sig == 'u')
		wb_pad(w, 4);	/* value aligned per its type */
	val(w, data);
}

static int build_call(struct wb *msg, uint32_t serial, const char *dest,
		      const char *path, const char *iface, const char *member,
		      const char *sig, const struct dbus_arg *args)
{
	struct wb hdr = { 0 };
	struct wb body = { 0 };
	size_t fields_pos, fields_len, i;

	for (i = 0; sig[i]; i++)
		if (body_add_arg(&body, &args[i]) < 0) {
			free(body.b);
			return -1;
		}

	wb_u8(&hdr, 'l');
	wb_u8(&hdr, MSG_TYPE_CALL);
	wb_u8(&hdr, 0);		/* flags */
	wb_u8(&hdr, 1);		/* protocol version */
	/* the body length excludes the trailing padding of the last value
	 * (the reference daemon sends body_len 9 for ":1.0", not 12) */
	wb_u32(&hdr, (uint32_t)body.nodepad);
	wb_u32(&hdr, serial);
	fields_pos = hdr.n;	/* remember where fields_len goes */
	wb_u32(&hdr, 0);

	hdr_field(&hdr, FIELD_PATH, 'o', wb_str, path);
	hdr_field(&hdr, FIELD_INTERFACE, 's', wb_str, iface);
	hdr_field(&hdr, FIELD_MEMBER, 's', wb_str, member);
	hdr_field(&hdr, FIELD_DESTINATION, 's', wb_str, dest);
	if (*sig)
		hdr_field(&hdr, FIELD_SIGNATURE, 'g', wb_sig, sig);

	/* patch the fields length: the a(yv) byte count excludes the
	 * padding after the last element (hdr.nodepad is the end of the
	 * last field value before its trailing alignment padding) */
	fields_len = hdr.nodepad - fields_pos - 4;
	hdr.b[fields_pos + 0] = (unsigned char)fields_len;
	hdr.b[fields_pos + 1] = (unsigned char)(fields_len >> 8);
	hdr.b[fields_pos + 2] = (unsigned char)(fields_len >> 16);
	hdr.b[fields_pos + 3] = (unsigned char)(fields_len >> 24);
	wb_pad(&hdr, 8);	/* header padded to 8 before the body */

	if (hdr.oom || body.oom || hdr.n + body.nodepad > MAX_MESSAGE) {
		free(hdr.b);
		free(body.b);
		return -1;
	}
	msg->b = malloc(hdr.n + body.nodepad);
	if (!msg->b) {
		free(hdr.b);
		free(body.b);
		return -1;
	}
	memcpy(msg->b, hdr.b, hdr.n);
	memcpy(msg->b + hdr.n, body.b, body.nodepad);
	msg->n = hdr.n + body.nodepad;
	free(hdr.b);
	free(body.b);
	return 0;
}

/* ----------------------------------------------------------------------
 * bounded reader (alignments relative to the start of the slice)
 * ---------------------------------------------------------------------- */

struct rd {
	const unsigned char *p, *end;
	size_t off;		/* bytes consumed (for alignment) */
	int err;
};

static unsigned rd_u8(struct rd *r)
{
	unsigned v;

	if (r->err || r->p + 1 > r->end) {
		r->err = 1;
		return 0;
	}
	v = *r->p++;
	r->off++;
	return v;
}

static uint32_t rd_u32(struct rd *r)
{
	unsigned char b[4];

	b[0] = rd_u8(r);
	b[1] = rd_u8(r);
	b[2] = rd_u8(r);
	b[3] = rd_u8(r);
	return (uint32_t)b[0] | (uint32_t)b[1] << 8 |
	       (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

/* pad from a base offset (absolute = base is 0) to the given alignment.
 * Stops at the end of the slice: the spec says the padding after the
 * LAST value in a container is not part of the container's length (the
 * reference daemon sends body_len 9 for ":1.0", not 12). */
static int rd_pad(struct rd *r, size_t base, size_t align)
{
	while ((r->off - base) % align && r->p < r->end) {
		rd_u8(r);
		if (r->err)
			return -1;
	}
	return 0;
}

static char *rd_str(struct rd *r)
{
	uint32_t len = rd_u32(r);
	char *s;

	if (r->err || len > (uint32_t)(r->end - r->p) - 1) {
		r->err = 1;
		return NULL;
	}
	if (r->p[len] != '\0') {
		r->err = 1;
		return NULL;
	}
	s = malloc((size_t)len + 1);
	if (!s) {
		r->err = 1;
		return NULL;
	}
	memcpy(s, r->p, len);
	s[len] = '\0';
	r->p += len + 1;
	r->off += len + 1;
	rd_pad(r, 0, 4);
	if (r->err) {
		free(s);
		return NULL;
	}
	return s;
}

/* ----------------------------------------------------------------------
 * reply body parsing (s / as / v(s))
 * ---------------------------------------------------------------------- */

static void value_free_inner(struct dbus_value *v)
{
	size_t i;

	switch (v->type) {
	case DBUS_VALUE_STRING:
		free(v->str);
		break;
	case DBUS_VALUE_STRING_ARRAY:
		for (i = 0; i < v->n; i++)
			free(v->items[i]);
		free(v->items);
		break;
	case DBUS_VALUE_VARIANT:
		free(v->var_sig);
		dbus_value_free(v->var);
		break;
	default:
		break;
	}
}

static struct dbus_value *rd_value(struct rd *r, const char *sig)
{
	struct dbus_value *v = calloc(1, sizeof *v);

	if (!v) {
		r->err = 1;
		return NULL;
	}
	if (!strcmp(sig, "s")) {
		rd_pad(r, 0, 4);
		v->str = rd_str(r);
		if (r->err)
			goto fail;
		v->type = DBUS_VALUE_STRING;
	} else if (!strcmp(sig, "as")) {
		uint32_t alen;
		struct rd ar;

		rd_pad(r, 0, 4);
		alen = rd_u32(r);
		if (r->err || alen > (uint32_t)(r->end - r->p)) {
			r->err = 1;
			goto fail;
		}
		ar.p = r->p;
		ar.end = r->p + alen;
		ar.off = 0;
		ar.err = 0;
		for (;;) {
			char *s;

			if (ar.p >= ar.end)
				break;
			s = rd_str(&ar);
			if (ar.err) {
				free(s);
				r->err = 1;
				goto fail;
			}
			v->items = realloc(v->items,
					    (v->n + 1) * sizeof(char *));
			if (!v->items) {
				free(s);
				r->err = 1;
				goto fail;
			}
			v->items[v->n++] = s;
		}
		r->p = ar.p;
		r->off += alen;
		v->type = DBUS_VALUE_STRING_ARRAY;
	} else if (!strcmp(sig, "v")) {
		unsigned slen;
		char inner[17];

		slen = rd_u8(r);
		if (r->err || slen == 0 || slen >= sizeof inner) {
			r->err = 1;
			goto fail;
		}
		if (r->p + slen + 1 > r->end) {
			r->err = 1;
			goto fail;
		}
		memcpy(inner, r->p, slen);
		inner[slen] = '\0';
		r->p += slen;
		r->off += slen;
		rd_u8(r);	/* the terminating NUL */
		/* only v(s) is used by the lgmagic interface */
		if (strcmp(inner, "s")) {
			r->err = 1;
			goto fail;
		}
		rd_pad(r, 0, 4);	/* value aligned globally per spec */
		v->var_sig = strdup(inner);
		v->var = calloc(1, sizeof *v->var);
		if (!v->var) {
			r->err = 1;
			goto fail;
		}
		v->var->str = rd_str(r);
		if (r->err || !v->var_sig)
			goto fail;
		v->var->type = DBUS_VALUE_STRING;
		v->type = DBUS_VALUE_VARIANT;
	} else {
		r->err = 1;
		goto fail;
	}
	return v;
fail:
	value_free_inner(v);
	free(v);
	return NULL;
}

void dbus_value_free(struct dbus_value *v)
{
	if (!v)
		return;
	value_free_inner(v);
	free(v);
}

/* ----------------------------------------------------------------------
 * socket I/O with timeout
 * ---------------------------------------------------------------------- */

static int wait_fd(int fd, short events, int timeout_ms)
{
	struct pollfd pfd = { fd, events, 0 };

	for (;;) {
		int rc = poll(&pfd, 1, timeout_ms);

		if (rc < 0 && errno == EINTR)
			continue;
		return rc;	/* 0 = timeout, 1 = ready, -1 = error */
	}
}

static int write_all(int fd, const void *buf, size_t n)
{
	const unsigned char *p = buf;

	while (n) {
		ssize_t w = write(fd, p, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

/* pull more bytes into the client read buffer */
static int refill(struct dbus_client *c, char *err, size_t errsz)
{
	unsigned char tmp[4096];
	ssize_t r;
	int rc;

	rc = wait_fd(c->fd, POLLIN, CALL_TIMEOUT_MS);
	if (rc <= 0) {
		snprintf(err, errsz, "cannot read from the bus: %s",
			 rc == 0 ? "timed out" : strerror(errno));
		return -1;
	}
	r = read(c->fd, tmp, sizeof tmp);
	if (r < 0 && errno == EINTR)
		return refill(c, err, errsz);
	if (r <= 0) {
		snprintf(err, errsz, "cannot read from the bus: %s",
			 r == 0 ? "connection closed" : strerror(errno));
		return -1;
	}
	if (c->rn + (size_t)r > c->rcap) {
		size_t ncap = c->rcap ? c->rcap : 1024;
		unsigned char *nb;

		while (ncap < c->rn + (size_t)r)
			ncap *= 2;
		nb = realloc(c->rbuf, ncap);
		if (!nb) {
			snprintf(err, errsz, "out of memory");
			return -1;
		}
		c->rbuf = nb;
		c->rcap = ncap;
	}
	memcpy(c->rbuf + c->rn, tmp, (size_t)r);
	c->rn += (size_t)r;
	return 0;
}

/* read exactly n bytes from the buffer, pulling from the socket as needed */
static int readn(struct dbus_client *c, void *buf, size_t n,
		 char *err, size_t errsz)
{
	size_t got = 0;

	while (got < n) {
		if (c->rn) {
			size_t take = n - got < c->rn ? n - got : c->rn;

			memcpy((unsigned char *)buf + got, c->rbuf, take);
			memmove(c->rbuf, c->rbuf + take, c->rn - take);
			c->rn -= take;
			got += take;
			continue;
		}
		if (refill(c, err, errsz) < 0)
			return -1;
	}
	return 0;
}

/* read one '\n'-terminated line into buf (without the line ending) */
static int read_line(struct dbus_client *c, char *buf, size_t bufsz,
		     char *err, size_t errsz)
{
	size_t n = 0;

	for (;;) {
		size_t i, take;

		if (!c->rn)
			if (refill(c, err, errsz) < 0)
				return -1;
		for (i = 0; i < c->rn && c->rbuf[i] != '\n'; i++)
			;
		take = i < c->rn ? i + 1 : i;	/* include the '\n' */
		if (i < c->rn) {
			/* found */
			if (n + i >= bufsz - 1) {
				snprintf(err, errsz,
					 "bus sent an overlong line");
				return -1;
			}
			memcpy(buf + n, c->rbuf, i);
			n += i;
			if (n && buf[n - 1] == '\r')
				n--;
			buf[n] = '\0';
			memmove(c->rbuf, c->rbuf + take, c->rn - take);
			c->rn -= take;
			return 0;
		}
		if (n + take >= bufsz - 1) {
			snprintf(err, errsz, "bus sent an overlong line");
			return -1;
		}
		memcpy(buf + n, c->rbuf, take);
		n += take;
		c->rn = 0;
	}
}

/* ----------------------------------------------------------------------
 * connection + AUTH EXTERNAL
 * ---------------------------------------------------------------------- */

/* find "unix:path=" or "unix:abstract=" in a ;-separated address list;
 * returns a pointer into address for path sockets, else NULL */
static int pick_unix_socket(const char *address, int *abstract,
			    const char **sock, size_t *socklen)
{
	const char *p = address;

	*abstract = 0;
	*sock = NULL;
	*socklen = 0;
	while (*p) {
		const char *end = strchr(p, ';');
		const char *val, *v;

		if (!end)
			end = p + strlen(p);
		val = NULL;
		if ((size_t)(end - p) > 10 && !strncmp(p, "unix:path=", 10))
			val = p + 10;
		else if ((size_t)(end - p) > 13 &&
			 !strncmp(p, "unix:abstract=", 13)) {
			val = p + 13;
			*abstract = 1;
		}
		if (val) {
			/* the parameter value runs to the next separator:
			 * ',' starts the next key=value pair, ';' the next
			 * address (a bare ',' cannot appear in a value) */
			for (v = val; v < end && *v != ','; v++)
				;
			*sock = val;
			*socklen = (size_t)(v - val);
			return 0;
		}
		if (*end)
			end++;
		p = end;
	}
	return -1;
}

static int say_hello(struct dbus_client *c, char *err, size_t errsz);

int dbus_connect(struct dbus_client *c, const char *address,
		 char *err, size_t errsz)
{
	char line[512], path[512];
	const char *sock = NULL;
	size_t socklen = 0;
	int abstract = 0;
	int fd = -1;

	memset(c, 0, sizeof *c);
	c->fd = -1;

	if (!address)
		address = getenv("DBUS_SYSTEM_BUS_ADDRESS");
	if (!address || !*address)
		address = "unix:path=/run/dbus/system_bus_socket";

	if (pick_unix_socket(address, &abstract, &sock, &socklen) < 0 ||
	    socklen == 0 || socklen >= sizeof path) {
		snprintf(err, errsz,
			 "unsupported bus address '%s' (only unix sockets)",
			 address);
		return -1;
	}
	memcpy(path, sock, socklen);
	path[socklen] = '\0';

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		snprintf(err, errsz, "cannot create a socket: %s",
			 strerror(errno));
		return -1;
	}
	{
		struct sockaddr_un a;

		memset(&a, 0, sizeof a);
		a.sun_family = AF_UNIX;
		if (abstract) {
			memcpy(a.sun_path + 1, path, socklen);
			if (connect(fd, (struct sockaddr *)&a,
				    (socklen_t)(sizeof(sa_family_t) + 1 +
						socklen)) < 0)
				goto fail_connect;
		} else {
			strcpy(a.sun_path, path);
			if (connect(fd, (struct sockaddr *)&a,
				    (socklen_t)sizeof a) < 0)
				goto fail_connect;
		}
	}

	c->fd = fd;

	/* AUTH EXTERNAL: the identity is the uid in decimal (the reference
	 * dbus-daemon parses it base 10), and the initial-response argument
	 * is HEX-ENCODED binary per the D-Bus spec - the daemon decodes the
	 * hex back to the decimal string before parsing the uid.  So uid 0
	 * goes on the wire as "AUTH EXTERNAL 30" (hex of ASCII "0"); a raw
	 * decimal string would hex-decode to garbage and get REJECTED.
	 * The leading NUL is protocol, so build the raw bytes by hand -
	 * snprintf would stop at it (the format ends there). */
	{
		static const char hex[] = "0123456789abcdef";
		unsigned char raw[64];
		char dec[24];
		int dlen = snprintf(dec, sizeof dec, "%lu",
				    (unsigned long)getuid());
		int ilen = snprintf((char *)raw + 1, sizeof raw - 1,
				    "AUTH EXTERNAL ");
		int i;

		if (dlen < 0 || dlen >= (int)sizeof dec)
			goto fail_io;
		if (ilen < 0 || 1 + ilen + 2 * dlen + 2 > (int)sizeof raw)
			goto fail_io;
		for (i = 0; i < dlen; i++) {
			raw[1 + ilen + 2 * i] =
				(unsigned char)hex[(dec[i] >> 4) & 0xf];
			raw[1 + ilen + 2 * i + 1] =
				(unsigned char)hex[dec[i] & 0xf];
		}
		ilen += 2 * dlen;
		raw[1 + ilen] = '\r';
		raw[1 + ilen + 1] = '\n';
		raw[0] = '\0';
		if (write_all(fd, raw, (size_t)1 + ilen + 2) < 0)
			goto fail_io;
	}
	if (read_line(c, line, sizeof line, err, errsz) < 0)
		goto fail;
	if (strncmp(line, "OK ", 3)) {
		if (!strncmp(line, "REJECTED", 8))
			snprintf(err, errsz,
				 "the bus rejected authentication (EXTERNAL)");
		else
			snprintf(err, errsz,
				 "unexpected bus authentication response: %s",
				 line);
		goto fail;
	}
	if (write_all(fd, "BEGIN\r\n", 7) < 0)
		goto fail_io;
	/* No reply to BEGIN: the dbus-daemon (verified against dbus 1.14 on
	 * Ubuntu 24.04 via strace of both directions) switches the connection
	 * to binary mode the moment it reads BEGIN and sends nothing until the
	 * first message - dbus-send sends its first call immediately after
	 * BEGIN, and so do we.  Waiting for an OK here would hang forever. */
	if (say_hello(c, err, errsz) < 0)
		goto fail;
	return 0;

fail_connect:
	snprintf(err, errsz, "cannot connect to the bus at %s: %s",
		 path, strerror(errno));
	close(fd);
	return -1;
fail_io:
	snprintf(err, errsz, "cannot talk to the bus: %s", strerror(errno));
fail:
	close(fd);
	c->fd = -1;
	return -1;
}

void dbus_disconnect(struct dbus_client *c)
{
	if (c->fd >= 0)
		close(c->fd);
	free(c->rbuf);
	free(c->err_name);
	free(c->err_msg);
	memset(c, 0, sizeof *c);
	c->fd = -1;
}

/* ----------------------------------------------------------------------
 * method call + reply
 * ---------------------------------------------------------------------- */

/* parse the header fields of a reply: extracts ERROR_NAME, SIGNATURE,
 * REPLY_SERIAL; returns 0 or -1 on malformed input */
static int parse_header_fields(const unsigned char *msg, size_t msglen,
			       const unsigned char *fields, size_t fields_len,
			       char **error_name, char **sig_out,
			       uint32_t *reply_serial)
{
	const unsigned char *fp = fields, *fend = fields + fields_len;

	*error_name = NULL;
	*sig_out = NULL;
	*reply_serial = 0;
	while (fp < fend) {
		unsigned code, fsig, slen;
		const unsigned char *abs;

		while ((size_t)(fp - msg) % 8 && fp < fend)
			fp++;		/* field struct aligned to 8 */
		if (fp + 4 > fend)	/* code + sig 'g' value (len, char, NUL) */
			return -1;
		code = *fp++;
		slen = *fp++;
		if (slen != 1)
			return -1;	/* only single-char field sigs supported */
		fsig = *fp++;
		if (*fp++ != '\0')
			return -1;	/* signature NUL terminator */
		if (fsig == 's' || fsig == 'o' || fsig == 'u') {
			while ((size_t)(fp - msg) % 4 && fp < fend)
				fp++;	/* value aligned per its type */
		}
		abs = fp;
		if (fsig == 's' || fsig == 'o') {
			uint32_t len;

			if (fp + 4 > fend)
				return -1;
			len = (uint32_t)fp[0] | (uint32_t)fp[1] << 8 |
			      (uint32_t)fp[2] << 16 | (uint32_t)fp[3] << 24;
			fp += 4;
			if ((size_t)len > (size_t)(fend - fp) - 1)
				return -1;
			if (fp[len] != '\0')
				return -1;
			fp += len + 1;
			while ((size_t)(fp - msg) % 4 && fp < fend)
				fp++;	/* string padded to 4 */
			if (code == FIELD_ERROR_NAME ||
			    code == FIELD_SIGNATURE) {
				char *s = malloc((size_t)len + 1);

				if (!s)
					return -1;
				memcpy(s, abs + 4, len);
				s[len] = '\0';
				if (code == FIELD_ERROR_NAME)
					*error_name = s;
				else
					*sig_out = s;
			}
		} else if (fsig == 'u') {
			if (fp + 4 > fend)
				return -1;
			if (code == FIELD_REPLY_SERIAL)
				*reply_serial =
					(uint32_t)fp[0] | (uint32_t)fp[1] << 8 |
					(uint32_t)fp[2] << 16 |
					(uint32_t)fp[3] << 24;
			fp += 4;
		} else if (fsig == 'g') {
			unsigned slen;

			if (fp + 1 > fend)
				return -1;
			slen = fp[0];
			fp += 1;
			if (fp + slen + 1 > fend)
				return -1;
			if (code == FIELD_SIGNATURE) {
				char *s = malloc((size_t)slen + 1);

				if (!s)
					return -1;
				memcpy(s, fp, slen);
				s[slen] = '\0';
				*sig_out = s;
			}
			fp += slen + 1;
		} else {
			return -1;
		}
	}
	/* the message must be long enough for the declared fields */
	if ((size_t)(fp - msg) > msglen)
		return -1;
	return 0;
}

static int bus_call(struct dbus_client *c, const char *dest, const char *path,
		    const char *iface, const char *method, const char *sig_in,
		    const struct dbus_arg *args, char **err_name, char **err_msg,
		    struct dbus_value **out, char *err, size_t errsz)
{
	struct wb msg = { 0 };
	unsigned char fixed[16];
	unsigned char *buf = NULL, *rest;
	size_t rest_len, hdr_pad, i;
	uint32_t body_len, reply_serial = 0, fields_len;
	unsigned type;
	char *reply_sig = NULL, *reply_errname = NULL;
	const unsigned char *body;
	int rc = -1;

	if (out)
		*out = NULL;
	if (err_name)
		*err_name = NULL;
	if (err_msg)
		*err_msg = NULL;
	free(c->err_name);
	free(c->err_msg);
	c->err_name = c->err_msg = NULL;

	if (c->fd < 0) {
		snprintf(err, errsz, "not connected to the bus");
		return -1;
	}
	for (i = 0; sig_in[i]; i++)
		if (sig_in[i] != 's' && sig_in[i] != 'd' && sig_in[i] != 'u') {
			snprintf(err, errsz,
				 "unsupported input signature '%s'", sig_in);
			return -1;
		}
	if (sig_in[0] && !args) {
		snprintf(err, errsz,
			 "method call needs arguments for signature '%s'",
			 sig_in);
		return -1;
	}

	c->serial++;
	if (build_call(&msg, c->serial, dest, path, iface, method, sig_in,
		       args) < 0) {
		snprintf(err, errsz, "cannot marshal the method call");
		return -1;
	}
	if (write_all(c->fd, msg.b, msg.n) < 0) {
		snprintf(err, errsz, "cannot write to the bus: %s",
			 strerror(errno));
		goto out;
	}

	for (;;) {
		/* fixed header */
		if (readn(c, fixed, sizeof fixed, err, errsz) < 0)
			goto out;
		if (fixed[0] != 'l') {
			snprintf(err, errsz, "not a D-Bus reply (bad endian byte)");
			goto out;
		}
		type = fixed[1];
		body_len = (uint32_t)fixed[4] | (uint32_t)fixed[5] << 8 |
			   (uint32_t)fixed[6] << 16 | (uint32_t)fixed[7] << 24;
		fields_len = (uint32_t)fixed[12] | (uint32_t)fixed[13] << 8 |
			     (uint32_t)fixed[14] << 16 | (uint32_t)fixed[15] << 24;
		if (fields_len > MAX_MESSAGE || body_len > MAX_MESSAGE ||
		    16 + fields_len + body_len > MAX_MESSAGE) {
			snprintf(err, errsz, "reply too large");
			goto out;
		}
		hdr_pad = (8 - ((16 + fields_len) % 8)) % 8;
		rest_len = fields_len + hdr_pad + body_len;
		/* one contiguous buffer: the header fields align relative to
		 * the start of the message, and the buffer starts at
		 * message byte 0 */
		buf = malloc(16 + rest_len);
		if (!buf) {
			snprintf(err, errsz, "out of memory");
			goto out;
		}
		memcpy(buf, fixed, sizeof fixed);
		if (readn(c, buf + 16, rest_len, err, errsz) < 0)
			goto out;
		rest = buf + 16;

		if (parse_header_fields(buf, 16 + rest_len, rest, fields_len,
					&reply_errname, &reply_sig,
					&reply_serial) < 0) {
			snprintf(err, errsz, "malformed reply header");
			goto out;
		}

		if (type == MSG_TYPE_SIGNAL) {
			/* signals are not replies: skip and keep waiting */
			free(reply_errname);
			free(reply_sig);
			free(buf);
			reply_errname = reply_sig = NULL;
			buf = NULL;
			continue;
		}

		body = rest + fields_len + hdr_pad;
		if (type == MSG_TYPE_ERROR) {
			/* body = (s) error message */
			struct rd r = { body, rest + rest_len, 0, 0 };

			rd_pad(&r, 0, 4);
			c->err_msg = rd_str(&r);
			if (r.err || r.p != r.end) {
				snprintf(err, errsz, "malformed error reply body");
				goto out;
			}
			c->err_name = reply_errname;
			reply_errname = NULL;
			if (!c->err_name)
				c->err_name = strdup("org.lgmagic.Error.Failed");
			if (err_name)
				*err_name = c->err_name;
			if (err_msg)
				*err_msg = c->err_msg;
			rc = 1;
			goto out;
		}
		if (type != MSG_TYPE_RETURN) {
			snprintf(err, errsz, "unexpected reply type %u", type);
			goto out;
		}
		if (reply_serial != c->serial) {
			snprintf(err, errsz,
				 "reply serial %u does not match request %u",
				 reply_serial, c->serial);
			goto out;
		}
		if (!reply_sig || !*reply_sig) {
			if (out) {
				*out = calloc(1, sizeof(struct dbus_value));
				if (!*out) {
					snprintf(err, errsz, "out of memory");
					goto out;
				}
				(*out)->type = DBUS_VALUE_EMPTY;
			}
			rc = 0;
			goto out;
		}
		{
			struct rd r = { body, rest + rest_len, 0, 0 };
			struct dbus_value *v = rd_value(&r, reply_sig);

			if (!v || r.err || r.p != r.end) {
				dbus_value_free(v);
				snprintf(err, errsz,
					 "cannot parse the reply body "
					 "(signature '%s')",
					 reply_sig);
				goto out;
			}
			if (out)
				*out = v;
			else
				dbus_value_free(v);
			rc = 0;
		}
		break;			/* a matching reply was parsed */
	}

out:
	free(msg.b);
	free(buf);
	free(reply_sig);
	free(reply_errname);
	return rc;
}

int dbus_call(struct dbus_client *c, const char *method, const char *sig_in,
	      const struct dbus_arg *args, char **err_name, char **err_msg,
	      struct dbus_value **out, char *err, size_t errsz)
{
	return bus_call(c, DBUS_CLIENT_DEST, DBUS_CLIENT_PATH,
			DBUS_CLIENT_IFACE, method, sig_in, args,
			err_name, err_msg, out, err, errsz);
}

/* The Hello exchange: the daemon rejects any message from a connection
 * that has not yet sent org.freedesktop.DBus.Hello (verified against
 * dbus 1.14 - the connection is closed without a reply), so every
 * connection must say hello first and claim its unique name. */
static int say_hello(struct dbus_client *c, char *err, size_t errsz)
{
	struct dbus_value *v = NULL;
	char *err_name = NULL, *err_msg = NULL;
	int rc = bus_call(c, "org.freedesktop.DBus", "/org/freedesktop/DBus",
			  "org.freedesktop.DBus", "Hello", "", NULL,
			  &err_name, &err_msg, &v, err, errsz);

	if (rc == 1)
		snprintf(err, errsz, "the bus rejected Hello: %s (%s)",
			 err_name ? err_name : "unknown error",
			 err_msg ? err_msg : "no message");
	if (v)
		dbus_value_free(v);
	return rc == 0 ? 0 : -1;
}
