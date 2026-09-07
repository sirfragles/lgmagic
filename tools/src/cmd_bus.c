/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cmd_bus.c - shared plumbing for the CLI subcommands.
 * See cmd_bus.h.
 */
#include "cmd_bus.h"

#include "toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_bus_connect(struct dbus_client *c)
{
	char err[256];

	if (dbus_connect(c, NULL, err, sizeof(err)) == 0)
		return 0;
	fprintf(stderr, "lgmagic: %s\n", err);
	fprintf(stderr, "lgmagic: lgmagicd is not running - try: "
		"sudo systemctl enable --now lgmagicd\n");
	return -1;
}

int cmd_bus_call(struct dbus_client *c, const char *method, const char *sig,
		 const struct dbus_arg *args, char **err_name, char **err_msg,
		 struct dbus_value **out)
{
	char err[256];

	switch (dbus_call(c, method, sig, args, err_name, err_msg, out,
			  err, sizeof(err))) {
	case 0:
		return 0;
	case 1:
		if (*err_name && strcmp(*err_name,
					 "org.lgmagic.Error.NotAuthorized") == 0)
			fprintf(stderr, "lgmagic: permission denied (polkit)\n");
		else if (*err_msg && (*err_msg)[0])
			fprintf(stderr, "lgmagic: %s\n", *err_msg);
		else
			fprintf(stderr, "lgmagic: %s\n",
				*err_name ? *err_name : "daemon error");
		return -1;
	default:
		fprintf(stderr, "lgmagic: %s\n", err);
		fprintf(stderr, "lgmagic: lgmagicd is not running - try: "
			"sudo systemctl enable --now lgmagicd\n");
		return -1;
	}
}

int cmd_bus_resolve_mac(struct dbus_client *c, const char *given,
			char *buf, size_t bufsz)
{
	char *err_name, *err_msg;
	struct dbus_value *out;
	size_t i;

	if (given) {
		snprintf(buf, bufsz, "%s", given);
		return 0;
	}
	if (cmd_bus_call(c, "ListDevices", "", NULL, &err_name, &err_msg,
			 &out) < 0)
		return -1;
	if (out->type == DBUS_VALUE_EMPTY || out->n == 0) {
		fprintf(stderr, "lgmagic: no devices (is the remote "
			"connected?)\n");
		dbus_value_free(out);
		return -1;
	}
	if (out->n > 1) {
		fprintf(stderr, "lgmagic: multiple devices, specify a MAC: ");
		for (i = 0; i < out->n; i++)
			fprintf(stderr, "%s%s", i ? ", " : "", out->items[i]);
		fprintf(stderr, "\n");
		dbus_value_free(out);
		return -1;
	}
	snprintf(buf, bufsz, "%s", out->items[0]);
	dbus_value_free(out);
	return 0;
}

const char *cmd_config_root(void)
{
	const char *r = getenv("LGMAGIC_CONFIG_ROOT");

	return r && r[0] ? r : "/etc/lgmagic";
}

const char *cmd_state_dir(void)
{
	const char *r = getenv("LGMAGIC_STATE_DIR");

	return r && r[0] ? r : "/var/lib/lgmagic";
}

/* memmove-based join (see daemon_config.c: the compiler cannot bound
 * two %s in snprintf and warns). */
static int jpath(char *out, size_t outsz, const char *a, const char *b)
{
	size_t alen = strlen(a);
	size_t blen = strlen(b);

	if (alen + blen + 1 > outsz) {
		out[0] = '\0';
		return -1;
	}
	memmove(out, a, alen);
	memmove(out + alen, b, blen + 1);
	return 0;
}

int cmd_load_device(const char *mac, struct device_config *dc,
		    char *err, size_t errsz)
{
	char path[4096];
	struct toml_value *root;
	const char *profile;
	int rc;

	device_config_init(dc);
	if (jpath(path, sizeof(path), cmd_config_root(), "/devices.d/") < 0 ||
	    jpath(path, sizeof(path), path, mac) < 0 ||
	    jpath(path, sizeof(path), path, ".toml") < 0) {
		snprintf(err, errsz, "device config path too long");
		return -1;
	}
	rc = device_config_load_file(path, dc, err, errsz);
	if (rc < 0)
		return -1;
	/* rc == 1: no device file - the defaults stay. */

	if (jpath(path, sizeof(path), cmd_state_dir(), "/state.toml") < 0) {
		snprintf(err, errsz, "state path too long");
		return -1;
	}
	root = toml_load_file(path, NULL, NULL);
	if (!root)
		return 0;	/* absent or unparseable - nothing to overlay */
	profile = NULL;
	{
		struct toml_value *devs = toml_table_get_short(root, "devices");
		struct toml_value *dev = devs && devs->type == TOML_TABLE ?
			toml_table_get_short(devs, mac) : NULL;

		if (dev)
			profile = toml_table_get_str(dev, "profile", NULL);
	}
	if (profile) {
		char *s = strdup(profile);

		if (!s) {
			toml_free(root);
			snprintf(err, errsz, "out of memory");
			return -1;
		}
		free(dc->profile);
		dc->profile = s;
	}
	toml_free(root);
	return 0;
}
