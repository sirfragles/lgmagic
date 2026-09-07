/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cmd_analyze.c - `lg-magic analyze` subcommand (scripts/lg_magic.py).
 *
 * Decodes HIDRAW reports from the LG Magic Remote and prints them in the
 * same format as the Python analyzer. The device is auto-detected by
 * VID/PID 000f:3412 (the hardcoded /dev/hidraw7 is gone); --device and
 * the hidraw_device config key override detection.
 */
#include "config.h"
#include "hidraw.h"

#include <stdio.h>
#include <string.h>

static void usage(FILE *out)
{
	fputs("Usage: lg-magic analyze [--device /dev/hidrawN] [--list]\n"
	      "\n"
	      "  --device PATH   use this hidraw device instead of auto-detection\n"
	      "  --list          list the detected LG Magic Remote devices\n", out);
}

int cmd_analyze(int argc, char **argv)
{
	const char *device = NULL;
	char path[256], err[256];
	int i;

	if (argc >= 2 && (strcmp(argv[1], "--help") == 0 ||
			  strcmp(argv[1], "-h") == 0)) {
		usage(stdout);
		return 0;
	}
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--list") == 0) {
			hidraw_list_remotes();
			return 0;
		} else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
			device = argv[++i];
		} else if (strncmp(argv[i], "--device=", 9) == 0) {
			device = argv[i] + 9;
		} else {
			fprintf(stderr, "lg-magic analyze: unexpected "
				"argument '%s'\n", argv[i]);
			usage(stderr);
			return 1;
		}
	}

	/* CLI flag > config key > auto-detection. */
	if (!device)
		device = g_cfg->hidraw_device;
	if (device) {
		snprintf(path, sizeof(path), "%s", device);
	} else {
		if (hidraw_find_remote(path, sizeof(path), err, sizeof(err)) < 0) {
			fprintf(stderr, "lg-magic: %s\n", err);
			return 1;
		}
		printf("Using device: %s\n", path);
	}

	return hidraw_run(path) < 0 ? 1 : 0;
}
