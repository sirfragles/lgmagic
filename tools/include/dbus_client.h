/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * dbus_client.h - minimal zero-dependency D-Bus client for the lgmagic
 * CLI (portable).
 *
 * The CLI talks to lgmagicd over the system bus: one fixed destination,
 * one fixed interface, one call in flight, no signals, no introspection.
 * The wire protocol (marshaling, AUTH EXTERNAL) is implemented by hand in
 * dbus_client.c so that `lgmagic` keeps linking only libc/libm - no
 * libsystemd (asserted by ldd in CI).
 *
 * Supported input types:  s (string), d (double), u (uint32)
 * Supported reply types:  s (string), as (array of strings), v(s)
 *                         (variant of a string), and an empty body.
 * D-Bus ERROR replies are reported as rc 1 with err_name/err_msg.
 */
#ifndef LG_TOOLS_DBUS_CLIENT_H
#define LG_TOOLS_DBUS_CLIENT_H

#include <stddef.h>
#include <stdint.h>

/* The fixed destination of every call.  Tests may override the constants
 * with -D to talk to a real bus for smoke validation. */
#ifndef DBUS_CLIENT_DEST
#define DBUS_CLIENT_DEST "org.lgmagic"
#endif
#ifndef DBUS_CLIENT_PATH
#define DBUS_CLIENT_PATH "/org/lgmagic/Manager"
#endif
#ifndef DBUS_CLIENT_IFACE
#define DBUS_CLIENT_IFACE "org.lgmagic.Manager"
#endif

/* One input argument to dbus_call; `type` says which member is used. */
struct dbus_arg {
	char type;		/* 's', 'd' or 'u' */
	const char *s;		/* for 's' */
	double d;		/* for 'd' */
	uint32_t u;		/* for 'u' */
};

/* A returned value (one of the supported reply types). */
enum dbus_value_type {
	DBUS_VALUE_EMPTY = 0,
	DBUS_VALUE_STRING,
	DBUS_VALUE_STRING_ARRAY,
	DBUS_VALUE_VARIANT,	/* a v(s): var_sig + var (a string) */
	DBUS_VALUE_UINT32,
	DBUS_VALUE_DOUBLE,
};

struct dbus_value {
	int type;
	char *str;		/* DBUS_VALUE_STRING */
	char **items;		/* DBUS_VALUE_STRING_ARRAY */
	size_t n;
	char *var_sig;		/* DBUS_VALUE_VARIANT */
	struct dbus_value *var;	/* DBUS_VALUE_VARIANT: inner value (owned) */
	uint32_t u;		/* DBUS_VALUE_UINT32 */
	double d;		/* DBUS_VALUE_DOUBLE */
};

struct dbus_client {
	int fd;
	uint32_t serial;	/* serial of the next call (0 = none yet) */
	unsigned char *rbuf;	/* read buffer */
	size_t rn, rcap;
	char *err_name;		/* last ERROR reply name (owned) */
	char *err_msg;		/* last ERROR reply message (owned) */
};

/* Connect to the bus and run the AUTH EXTERNAL handshake.  address is
 * "unix:path=..."/"unix:abstract=..." or NULL for the default system bus
 * ($DBUS_SYSTEM_BUS_ADDRESS, then /run/dbus/system_bus_socket).
 * Returns 0 or -1 with err set. */
int dbus_connect(struct dbus_client *c, const char *address,
		 char *err, size_t errsz);

/* Close the connection and release the client state.  Safe to call on a
 * fresh/zeroed client. */
void dbus_disconnect(struct dbus_client *c);

/* Call `method` (signature sig_in, args[] one per signature character;
 * NULL args allowed for an empty signature) and wait up to 25 s for the
 * reply.
 *
 * Returns: 0 on METHOD_RETURN with *out set (caller frees it with
 * dbus_value_free; DBUS_VALUE_EMPTY for an empty body), 1 on a D-Bus
 * ERROR reply with *err_name / *err_msg pointing into the client (valid
 * until the next call), -1 on transport/protocol errors with err set.
 * out/err_name/err_msg are always reset first. */
int dbus_call(struct dbus_client *c, const char *method, const char *sig_in,
	      const struct dbus_arg *args, char **err_name, char **err_msg,
	      struct dbus_value **out, char *err, size_t errsz);

void dbus_value_free(struct dbus_value *v);

#endif /* LG_TOOLS_DBUS_CLIENT_H */
