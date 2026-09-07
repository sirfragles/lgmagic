/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cmd_profile.c - `lg-magic profile list|show|set`.
 *
 * list/show read the world-readable files directly (no daemon, no
 * polkit needed): devices.d/<MAC>.toml + the active-profile overlay in
 * state.toml.  set goes through the daemon (polkit profile-set).
 */
#include "commands.h"

#include "cmd_bus.h"
#include "keymap.h"
#include "profiles.h"

#include <stdio.h>
#include <string.h>

static int usage(FILE *out)
{
	fputs("Usage: lg-magic profile list\n"
	      "       lg-magic profile show [MAC]\n"
	      "       lg-magic profile set MAC PROFILE\n", out);
	return out == stderr;
}

static int do_list(struct dbus_client *c)
{
	char *err_name, *err_msg;
	struct dbus_value *out;
	size_t i;

	if (cmd_bus_call(c, "ListDevices", "", NULL, &err_name, &err_msg,
			 &out) < 0)
		return -1;
	for (i = 0; i < out->n; i++) {
		struct device_config dc;
		char err[256];
		size_t j;

		if (cmd_load_device(out->items[i], &dc, err, sizeof(err)) < 0) {
			fprintf(stderr, "lg-magic: %s: %s\n", out->items[i],
				err);
			continue;
		}
		printf("%s:", out->items[i]);
		if (dc.nprofiles == 0)
			printf(" default");
		for (j = 0; j < dc.nprofiles; j++)
			printf(" %s%s", dc.profiles[j].name,
			       strcmp(dc.profiles[j].name, dc.profile) == 0 ?
			       " (active)" : "");
		printf("\n");
		device_config_free(&dc);
	}
	dbus_value_free(out);
	return 0;
}

static void print_buttons(const struct profile *p, const char *indent)
{
	size_t i;

	if (p->nmap == 0) {
		printf("%s  buttons: (none)\n", indent);
		return;
	}
	for (i = 0; i < p->nmap; i++) {
		const char *from = keymap_code_to_name(p->map[i].from);
		const char *to = keymap_code_to_name(p->map[i].to);

		printf("%s  %s -> %s\n", indent, from ? from : "?",
		       to ? to : "?");
	}
}

static int do_show(struct dbus_client *c, const char *mac)
{
	struct device_config dc;
	char err[256];
	size_t i;

	(void)c;	/* status comes from the world-readable file */
	if (cmd_load_device(mac, &dc, err, sizeof(err)) < 0) {
		fprintf(stderr, "lg-magic: %s: %s\n", mac, err);
		return -1;
	}
	printf("device: %s\n", mac);
	printf("profile: %s\n", dc.profile);
	printf("airmouse: %s\n", dc.has_airmouse ?
	       (dc.airmouse ? "on" : "off") : "on (default)");
	printf("calib: %s\n", dc.calib ? dc.calib : "- (per-device default)");
	printf("profiles:\n");
	if (dc.nprofiles == 0)
		printf("  default (active)\n");
	for (i = 0; i < dc.nprofiles; i++) {
		const struct profile *p = &dc.profiles[i];
		int active = strcmp(p->name, dc.profile) == 0;

		printf("  %s%s\n", p->name, active ? " (active)" : "");
		printf("    scroll_speed: %g%s\n", p->scroll_speed,
		       p->has_scroll ? "" : " (default)");
		printf("    sensitivity: %g%s\n", p->sensitivity,
		       p->has_sens ? "" : " (default)");
		if (p->has_lpf)
			printf("    lpf_alpha: %g\n", p->lpf_alpha);
		else
			printf("    lpf_alpha: (global)\n");
		print_buttons(p, "   ");
	}
	device_config_free(&dc);
	return 0;
}

int cmd_profile(int argc, char **argv)
{
	struct dbus_client c;
	char mac[64];
	int rc = 1;

	if (argc < 2 || strcmp(argv[1], "--help") == 0 ||
	    strcmp(argv[1], "-h") == 0)
		return usage(argc < 2 ? stderr : stdout);

	memset(&c, 0, sizeof(c));
	if (cmd_bus_connect(&c) < 0)
		return 1;

	if (strcmp(argv[1], "list") == 0) {
		if (argc != 2)
			return usage(stderr);
		rc = do_list(&c) < 0 ? 1 : 0;
	} else if (strcmp(argv[1], "show") == 0) {
		if (argc > 3)
			return usage(stderr);
		if (cmd_bus_resolve_mac(&c, argc == 3 ? argv[2] : NULL,
					mac, sizeof(mac)) < 0)
			goto out;
		rc = do_show(&c, mac) < 0 ? 1 : 0;
	} else if (strcmp(argv[1], "set") == 0) {
		char *err_name, *err_msg;
		struct dbus_value *out;

		if (argc != 4)
			return usage(stderr);
		{
			struct dbus_arg args[2] = {
				{ 's', argv[2], 0, 0 },
				{ 's', argv[3], 0, 0 },
			};

			rc = cmd_bus_call(&c, "SetProfile", "ss", args,
					  &err_name, &err_msg, &out) < 0 ? 1 : 0;
			if (rc == 0)
				dbus_value_free(out);
		}
	} else {
		usage(stderr);
		goto out;
	}
out:
	dbus_disconnect(&c);
	return rc;
}
