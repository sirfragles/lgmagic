/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * modprobe_helpers.c - kernel module helpers for the setup wizard.
 *
 * Everything here is best-effort: when a step fails (no root, no dmesg
 * access, Secure Boot blocking an unsigned module) the caller gets a
 * readable error and the wizard continues with advice.
 */
#include "modprobe_helpers.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int module_is_loaded(const char *name)
{
	char path[256];

	snprintf(path, sizeof(path), "/sys/module/%s", name);
	return access(path, F_OK) == 0;
}

/* Run a shell command, capturing its output for error messages. */
static int run_cmd(const char *cmd, char *err, size_t errsz, const char *what)
{
	FILE *p;
	char buf[512];
	size_t n;
	int rc;

	p = popen(cmd, "r");
	if (!p) {
		snprintf(err, errsz, "cannot run %s: %s", what, strerror(errno));
		return -1;
	}
	n = fread(buf, 1, sizeof(buf) - 1, p);
	buf[n] = '\0';
	rc = pclose(p);
	if (rc != 0) {
		while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
			buf[--n] = '\0';
		snprintf(err, errsz, "%s failed: %s", what,
			 n ? buf : "unknown error");
		return -1;
	}
	return 0;
}

int module_load(const char *name, char *err, size_t errsz)
{
	char cmd[512];

	snprintf(cmd, sizeof(cmd), "modprobe %s 2>&1", name);
	return run_cmd(cmd, err, errsz, "modprobe");
}

int module_reload(const char *name, char *err, size_t errsz)
{
	char cmd[512];

	/* rmmod fails with EBUSY while the remote is bound to the module;
	 * report that honestly so the caller can advise a reboot. */
	snprintf(cmd, sizeof(cmd), "rmmod %s 2>&1", name);
	if (run_cmd(cmd, err, errsz, "rmmod") < 0)
		return -1;
	return module_load(name, err, errsz);
}

int module_write_conf(const char *name, const char *params, char *err,
		      size_t errsz)
{
	const char *dir = "/etc/modprobe.d";
	FILE *f;

	if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
		snprintf(err, errsz, "cannot create %s: %s", dir, strerror(errno));
		return -1;
	}
	f = fopen("/etc/modprobe.d/lgmagic.conf", "w");
	if (!f) {
		snprintf(err, errsz, "cannot write /etc/modprobe.d/lgmagic.conf: "
			 "%s (are you root?)", strerror(errno));
		return -1;
	}
	if (fprintf(f, "options %s %s\n", name, params) < 0 ||
	    fclose(f) != 0) {
		snprintf(err, errsz, "write error on "
			 "/etc/modprobe.d/lgmagic.conf");
		return -1;
	}
	return 0;
}

int dmesg_contains(const char *needle)
{
	FILE *p;
	char buf[1024];
	int found = 0;

	p = popen("dmesg 2>/dev/null", "r");
	if (!p)
		return -1;
	while (fgets(buf, sizeof(buf), p))
		if (strstr(buf, needle)) {
			found = 1;
			break;
		}
	pclose(p);
	return found;
}
