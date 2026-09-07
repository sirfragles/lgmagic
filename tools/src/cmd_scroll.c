/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cmd_scroll.c - `lg-magic scroll speed|sensitivity|reset`.
 *
 * All three go through the daemon (polkit profile-set).  reset restores
 * both the scroll speed and the sensitivity to the built-in defaults
 * (1.0 / 30.0 - the same values Set* would write).
 */
#include "commands.h"

#include "cmd_bus.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(FILE *out)
{
	fputs("Usage: lg-magic scroll speed MAC VALUE\n"
	      "       lg-magic scroll sensitivity MAC VALUE\n"
	      "       lg-magic scroll reset MAC\n", out);
	return out == stderr;
}

static int do_set(struct dbus_client *c, const char *method,
		  const char *mac, const char *text)
{
	char *err_name, *err_msg;
	struct dbus_value *out;
	struct dbus_arg args[2];
	char *end;
	double v;

	errno = 0;
	v = strtod(text, &end);
	if (errno != 0 || end == text || *end != '\0') {
		fprintf(stderr, "lg-magic: invalid number '%s'\n", text);
		return 1;
	}
	args[0].type = 's';
	args[0].s = mac;
	args[1].type = 'd';
	args[1].d = v;
	if (cmd_bus_call(c, method, "sd", args, &err_name, &err_msg, &out) < 0)
		return 1;
	dbus_value_free(out);
	return 0;
}

int cmd_scroll(int argc, char **argv)
{
	struct dbus_client c;
	int rc = 1;

	if (argc < 2 || strcmp(argv[1], "--help") == 0 ||
	    strcmp(argv[1], "-h") == 0)
		return usage(argc < 2 ? stderr : stdout);
	if (argc < 3 || strcmp(argv[2], "--help") == 0 ||
	    strcmp(argv[2], "-h") == 0)
		return usage(stderr);

	memset(&c, 0, sizeof(c));
	if (cmd_bus_connect(&c) < 0)
		return 1;

	if (strcmp(argv[1], "speed") == 0) {
		if (argc != 4)
			return usage(stderr);
		rc = do_set(&c, "SetScrollSpeed", argv[2], argv[3]);
	} else if (strcmp(argv[1], "sensitivity") == 0) {
		if (argc != 4)
			return usage(stderr);
		rc = do_set(&c, "SetSensitivity", argv[2], argv[3]);
	} else if (strcmp(argv[1], "reset") == 0) {
		if (argc != 3)
			return usage(stderr);
		rc = do_set(&c, "SetScrollSpeed", argv[2], "1.0");
		if (rc == 0)
			rc = do_set(&c, "SetSensitivity", argv[2], "30.0");
	} else {
		usage(stderr);
	}
	dbus_disconnect(&c);
	return rc;
}
