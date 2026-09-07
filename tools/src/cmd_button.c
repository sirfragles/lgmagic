/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cmd_button.c - `lgmagic button list|map|reset`.
 *
 * list reads the world-readable device file directly (no daemon);
 * map/reset go through the daemon (polkit modify-input).  Keycodes are
 * KEY_* and BTN_* names; the daemon validates them.
 */
#include "commands.h"

#include "cmd_bus.h"
#include "keymap.h"
#include "profiles.h"

#include <stdio.h>
#include <string.h>

static int usage(FILE *out)
{
	fputs("Usage: lgmagic button list [MAC]\n"
	      "       lgmagic button map MAC KEY_FROM KEY_TO\n"
	      "       lgmagic button reset MAC\n", out);
	return out == stderr;
}

static int do_list(const char *mac)
{
	struct device_config dc;
	const struct profile *p;
	char err[256];
	size_t i;

	if (cmd_load_device(mac, &dc, err, sizeof(err)) < 0) {
		fprintf(stderr, "lgmagic: %s: %s\n", mac, err);
		return -1;
	}
	p = profile_find(&dc, dc.profile);
	if (!p || p->nmap == 0) {
		printf("(no buttons mapped in profile '%s')\n", dc.profile);
		device_config_free(&dc);
		return 0;
	}
	for (i = 0; i < p->nmap; i++) {
		const char *from = keymap_code_to_name(p->map[i].from);
		const char *to = keymap_code_to_name(p->map[i].to);

		printf("%s -> %s\n", from ? from : "?", to ? to : "?");
	}
	device_config_free(&dc);
	return 0;
}

int cmd_button(int argc, char **argv)
{
	struct dbus_client c;
	char *err_name, *err_msg;
	struct dbus_value *out;
	char mac[64];
	int rc = 1;

	if (argc < 2 || strcmp(argv[1], "--help") == 0 ||
	    strcmp(argv[1], "-h") == 0)
		return usage(argc < 2 ? stderr : stdout);

	memset(&c, 0, sizeof(c));
	if (cmd_bus_connect(&c) < 0)
		return 1;

	if (strcmp(argv[1], "list") == 0) {
		if (argc > 3)
			return usage(stderr);
		if (cmd_bus_resolve_mac(&c, argc == 3 ? argv[2] : NULL,
					mac, sizeof(mac)) < 0)
			goto out;
		rc = do_list(mac) < 0 ? 1 : 0;
	} else if (strcmp(argv[1], "map") == 0) {
		if (argc != 5)
			return usage(stderr);
		{
			struct dbus_arg args[3] = {
				{ 's', argv[2], 0, 0 },
				{ 's', argv[3], 0, 0 },
				{ 's', argv[4], 0, 0 },
			};

			rc = cmd_bus_call(&c, "MapButton", "sss", args,
					  &err_name, &err_msg, &out) < 0 ? 1 : 0;
			if (rc == 0)
				dbus_value_free(out);
		}
	} else if (strcmp(argv[1], "reset") == 0) {
		if (argc != 3)
			return usage(stderr);
		rc = cmd_bus_call(&c, "ResetButtons", "s",
				  &(struct dbus_arg){ 's', argv[2], 0, 0 },
				  &err_name, &err_msg, &out) < 0 ? 1 : 0;
		if (rc == 0)
			dbus_value_free(out);
	} else {
		usage(stderr);
		goto out;
	}
out:
	dbus_disconnect(&c);
	return rc;
}
