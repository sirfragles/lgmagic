/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hidraw.h - hidraw access to the LG Magic Remote (Linux only).
 */
#ifndef LG_TOOLS_HIDRAW_H
#define LG_TOOLS_HIDRAW_H

#include <stddef.h>

/* LG Magic Remote IDs. */
#define LG_MAGIC_VID 0x000f
#define LG_MAGIC_PID 0x3412

/* Find the first /dev/hidraw* with VID/PID 000f:3412 (auto-detection
 * replaces the hardcoded /dev/hidraw7 from lg_magic.py).
 * Returns 0 (path filled) or -1 with err. */
int hidraw_find_remote(char *path, size_t pathsz, char *err, size_t errsz);

/* List every candidate hidraw device with matching VID/PID (--list). */
void hidraw_list_remotes(void);

/* Fetch the HID uniq string (usually the BT MAC, "aa:bb:cc:dd:ee:ff").
 * Returns 0 (buf filled, empty on failure) or -1. */
int hidraw_get_uniq(const char *path, char *buf, size_t bufsz);

/* Read reports and print them exactly like scripts/lg_magic.py.
 * Returns 0 on clean end, -1 on error. */
int hidraw_run(const char *path);

#endif /* LG_TOOLS_HIDRAW_H */
