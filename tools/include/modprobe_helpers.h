/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * modprobe_helpers.h - kernel module helpers for the setup wizard
 * (Linux only).
 */
#ifndef LG_TOOLS_MODPROBE_HELPERS_H
#define LG_TOOLS_MODPROBE_HELPERS_H

#include <stddef.h>

int module_is_loaded(const char *name);
/* modprobe / rmmod+modprobe; return 0 or -1 with err. */
int module_load(const char *name, char *err, size_t errsz);
int module_reload(const char *name, char *err, size_t errsz);

/* Write "options <name> <params>" to /etc/modprobe.d/lgmagic.conf. */
int module_write_conf(const char *name, const char *params, char *err, size_t errsz);

/* Check dmesg for a string (used to verify the calibration was loaded).
 * Returns 1 if found, 0 if not, -1 if dmesg is not readable. */
int dmesg_contains(const char *needle);

#endif /* LG_TOOLS_MODPROBE_HELPERS_H */
