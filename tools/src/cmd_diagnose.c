/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cmd_diagnose.c - `lg-magic diagnose` (read-only, exit 0 always).
 *
 * A report for bug reports: version, kernel, module parameters, the
 * hidraw/evdev devices behind the remote, config presence, daemon
 * status and polkit presence.  Everything is best effort - a missing
 * piece prints a "not found" line, never an error.
 */
#include "commands.h"

#include "cmd_bus.h"
#include "config.h"
#include "evdev.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>

static int exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

/* Read the first line of a file ("" when absent/unreadable). */
static void read_line(const char *path, char *out, size_t outsz)
{
	FILE *f = fopen(path, "r");

	out[0] = '\0';
	if (!f)
		return;
	if (!fgets(out, (int)outsz, f))
		out[0] = '\0';
	fclose(f);
	{
		size_t n = strlen(out);

		if (n && out[n - 1] == '\n')
			out[n - 1] = '\0';
	}
}

static void section(const char *title)
{
	printf("\n== %s ==\n", title);
}

static void diag_module(void)
{
	char buf[256], params[4096];
	DIR *d;
	struct dirent *e;

	section("kernel module");
	if (!exists("/sys/module/lgmagic")) {
		printf("lgmagic: not loaded (try: sudo modprobe lgmagic)\n");
	} else {
		read_line("/sys/module/lgmagic/version", buf, sizeof(buf));
		printf("lgmagic: loaded (version %s)\n", buf[0] ? buf : "?");
		/* raw_only matters for v2: 1 = the daemon processes the input */
		read_line("/sys/module/lgmagic/parameters/raw_only", buf,
			  sizeof(buf));
		printf("raw_only: %s\n", buf[0] ? buf : "?");
		read_line("/sys/module/lgmagic/parameters/airmouse", buf,
			  sizeof(buf));
		printf("airmouse: %s\n", buf[0] ? buf : "?");
		read_line("/sys/module/lgmagic/parameters/imu_evdev", buf,
			  sizeof(buf));
		printf("imu_evdev: %s\n", buf[0] ? buf : "?");
	}
	/* modprobe.d overrides */
	params[0] = '\0';
	d = opendir("/etc/modprobe.d");
	if (d) {
		size_t used = 0;

		while ((e = readdir(d)) != NULL) {
			FILE *f;
			char path[512], line[256];
			size_t n;

			if (e->d_name[0] == '.')
				continue;
			n = snprintf(path, sizeof(path), "/etc/modprobe.d/%s",
				     e->d_name);
			if (n >= sizeof(path))
				continue;
			f = fopen(path, "r");
			if (!f)
				continue;
			while (fgets(line, sizeof(line), f)) {
				char *p;

				if (!strstr(line, "lgmagic"))
					continue;
				p = strchr(line, '\n');
				if (p)
					*p = '\0';
				n = strlen(line);
				if (used + n + 1 >= sizeof(params))
					break;
				memcpy(params + used, line, n + 1);
				used += n + 1;
			}
			fclose(f);
		}
		closedir(d);
	}
	printf("modprobe.d: %s\n", params[0] ? params : "(no lgmagic options)");
}

static void diag_devices(void)
{
	char uniq[128];
	char err[128];
	DIR *d;
	struct dirent *e;

	section("devices");
	/* hidraw */
	d = opendir("/sys/class/hidraw");
	if (!d) {
		printf("hidraw: none\n");
	} else {
		int found = 0;

		while ((e = readdir(d)) != NULL) {
			FILE *f;
			char path[512], line[512], upath[512];
			size_t n;

			if (e->d_name[0] == '.')
				continue;
			n = snprintf(path, sizeof(path),
				     "/sys/class/hidraw/%s/device/uevent",
				     e->d_name);
			if (n >= sizeof(path))
				continue;
			f = fopen(path, "r");
			if (!f)
				continue;
			while (fgets(line, sizeof(line), f)) {
				if (!strstr(line, "HID_ID=") ||
				    !strstr(line, ":000F:3412"))
					continue;
				uniq[0] = '\0';
				n = snprintf(upath, sizeof(upath),
					     "/sys/class/hidraw/%s/device/uniq",
					     e->d_name);
				if (n < sizeof(upath))
					read_line(upath, uniq, sizeof(uniq));
				printf("hidraw: /dev/%s (uniq %s)\n",
				       e->d_name, uniq[0] ? uniq : "-");
				found = 1;
				break;
			}
			fclose(f);
		}
		closedir(d);
		if (!found)
			printf("hidraw: no LG device (000f:3412)\n");
	}
	/* evdev */
	d = opendir("/dev/input");
	if (!d) {
		printf("evdev: none\n");
		return;
	}
	while ((e = readdir(d)) != NULL) {
		char path[512], name[256];
		unsigned vendor = 0, product = 0;
		size_t n;

		if (strncmp(e->d_name, "event", 5) != 0)
			continue;
		n = snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
		if (n >= sizeof(path))
			continue;
		if (evdev_probe(path, name, sizeof(name), &vendor, &product) < 0)
			continue;
		if (strcmp(name, "LG Magic Remote") == 0 ||
		    strcmp(name, "LG Magic Remote IMU") == 0 ||
		    (vendor == 0x000f && product == 0x3412)) {
			if (evdev_read_uniq(path, uniq, sizeof(uniq), err,
					    sizeof(err)) < 0)
				uniq[0] = '\0';
			printf("evdev: %s (%s, %04x:%04x, uniq %s)\n", path,
			       name, vendor, product, uniq[0] ? uniq : "-");
		}
	}
	closedir(d);
}

static void diag_config(void)
{
	char path[512];
	size_t n;

	section("configuration");
	n = snprintf(path, sizeof(path), "%s/config.toml", cmd_config_root());
	printf("config: %s%s\n", n < sizeof(path) ? path : "?",
	       n < sizeof(path) && exists(path) ? " (present)" : " (missing)");
	n = snprintf(path, sizeof(path), "%s/devices.d", cmd_config_root());
	printf("devices.d: %s%s\n", n < sizeof(path) ? path : "?",
	       n < sizeof(path) && exists(path) ? " (present)" : " (missing)");
	n = snprintf(path, sizeof(path), "%s/state.toml", cmd_state_dir());
	printf("state: %s%s\n", n < sizeof(path) ? path : "?",
	       n < sizeof(path) && exists(path) ? " (present)" : " (missing)");
}

static void diag_daemon(void)
{
	struct dbus_client c;
	char *err_name, *err_msg;
	struct dbus_value *out;
	char err[256];
	size_t i;

	section("daemon");
	memset(&c, 0, sizeof(c));
	if (dbus_connect(&c, NULL, err, sizeof(err)) < 0) {
		printf("lg-magicd: not running (%s)\n", err);
		return;
	}
	if (dbus_call(&c, "ListDevices", "", NULL, &err_name, &err_msg, &out,
		      err, sizeof(err)) < 0) {
		printf("lg-magicd: bus error\n");
		dbus_disconnect(&c);
		return;
	}
	printf("lg-magicd: running (%zu device%s)\n", out->n,
	       out->n == 1 ? "" : "s");
	for (i = 0; i < out->n; i++) {
		struct dbus_value *st = NULL;

		if (dbus_call(&c, "GetStatus", "s",
			      &(struct dbus_arg){ 's', out->items[i], 0, 0 },
			      &err_name, &err_msg, &st, err, sizeof(err)) == 0) {
			printf("  %s\n", out->items[i]);
			{
				char *line, *save = NULL;

				for (line = strtok_r(st->str, "\n", &save);
				     line; line = strtok_r(NULL, "\n", &save))
					printf("    %s\n", line);
			}
			dbus_value_free(st);
		}
	}
	dbus_value_free(out);
	dbus_disconnect(&c);
}

static void diag_polkit(void)
{
	section("polkit");
	printf("polkitd: %s\n",
	       exists("/usr/lib/polkit-1/polkitd") ||
	       exists("/usr/libexec/polkitd") ? "installed" : "not installed");
	printf("actions: %s\n",
	       exists("/usr/share/polkit-1/actions/org.lgmagic.policy") ?
	       "org.lgmagic.policy installed" :
	       "org.lgmagic.policy missing");
	printf("dbus: %s\n",
	       exists("/usr/share/dbus-1/system.d/org.lgmagic.conf") ?
	       "org.lgmagic.conf installed" :
	       "org.lgmagic.conf missing");
}

int cmd_diagnose(int argc, char **argv)
{
	struct utsname u;

	(void)argc;
	(void)argv;
	printf("lg-magic %s\n", g_tool_version);
	if (uname(&u) == 0)
		printf("kernel: %s %s %s\n", u.sysname, u.release, u.machine);
	else
		printf("kernel: unknown\n");
	diag_module();
	diag_devices();
	diag_config();
	diag_daemon();
	diag_polkit();
	return 0;	/* the report is always a success */
}
