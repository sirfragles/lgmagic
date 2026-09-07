/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cmd_device.c - `lg-magic device list|status` (read-only, no polkit).
 *
 * Both subcommands go through the daemon: list renders ListDevices,
 * status renders GetStatus for one device (the MAC argument is
 * optional when exactly one device is connected).
 */
#include "commands.h"

#include "cmd_bus.h"

#include <stdio.h>
#include <string.h>

static int usage(FILE *out)
{
	fputs("Usage: lg-magic device list\n"
	      "       lg-magic device status [MAC]\n", out);
	return out == stderr;
}

int cmd_device(int argc, char **argv)
{
	struct dbus_client c;
	char *err_name, *err_msg;
	struct dbus_value *out;
	char mac[64];
	size_t i;
	int rc = 1;

	if (argc < 2 || strcmp(argv[1], "--help") == 0 ||
	    strcmp(argv[1], "-h") == 0)
		return usage(argc < 2 ? stderr : stdout);

	if (strcmp(argv[1], "list") == 0) {
		if (argc != 2)
			return usage(stderr);
		memset(&c, 0, sizeof(c));
		if (cmd_bus_connect(&c) < 0)
			return 1;
		if (cmd_bus_call(&c, "ListDevices", "", NULL, &err_name,
				 &err_msg, &out) < 0) {
			dbus_disconnect(&c);
			return 1;
		}
		for (i = 0; i < out->n; i++)
			printf("%s\n", out->items[i]);
		dbus_value_free(out);
		dbus_disconnect(&c);
		return 0;
	}
	if (strcmp(argv[1], "status") == 0) {
		if (argc > 3)
			return usage(stderr);
		memset(&c, 0, sizeof(c));
		if (cmd_bus_connect(&c) < 0)
			return 1;
		if (cmd_bus_resolve_mac(&c, argc == 3 ? argv[2] : NULL,
					mac, sizeof(mac)) < 0) {
			dbus_disconnect(&c);
			return 1;
		}
		{
			struct dbus_arg arg = { 's', mac, 0, 0 };

			if (cmd_bus_call(&c, "GetStatus", "s", &arg, &err_name,
					 &err_msg, &out) < 0)
				rc = 1;
			else {
				printf("%s", out->str);
				rc = 0;
				dbus_value_free(out);
			}
		}
		dbus_disconnect(&c);
		return rc;
	}
	return usage(stderr);
}
