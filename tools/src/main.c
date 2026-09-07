/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * main.c - entry point of the multi-call lg-magic binary.
 *
 * Usage: lg-magic <subcommand> [options]
 *
 * Global flags (accepted anywhere on the command line):
 *   --config FILE   extra config file, merged after the system/user files
 *   --version       print the version and exit
 *   --help          print this help and exit
 *
 * The subcommand receives the remaining arguments (its own --help is
 * handled by the subcommand itself).
 */
#include "commands.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *usage_text =
	"Usage: lg-magic <subcommand> [options]\n"
	"\n"
	"Tools for the LG Magic Remote MR20 airmouse driver.\n"
	"\n"
	"Subcommands:\n"
	"  analyze     decode HIDRAW reports from the remote (like lg_magic.py)\n"
	"  imu         read the IMU via evdev: CSV, airmouse, AHRS, cube\n"
	"  calibrate   fit accel/gyro calibration from an IMU CSV recording\n"
	"  calib2bin   convert a calibration JSON to the 32-byte firmware blob\n"
	"  config      show / change the configuration (TOML files, see below)\n"
	"  setup       interactive wizard: configure, calibrate, install\n"
	"  device      list connected remotes / show the daemon status\n"
	"  profile     list / show / switch the active profile\n"
	"  button      list / remap / reset the remote buttons\n"
	"  scroll      adjust the wheel speed / airmouse sensitivity\n"
	"  diagnose    print a report for bug reports (read-only)\n"
	"\n"
	"Global flags:\n"
	"  --config FILE   extra config file (merged last, below CLI flags)\n"
	"  --version       print version and exit\n"
	"  --help          print this help and exit\n"
	"\n"
	"Configuration precedence: built-in defaults < /etc/lg-magic/config.toml\n"
	"< ~/.config/lg-magic/config.toml < --config FILE < CLI flags.\n"
	"Run 'lg-magic <subcommand> --help' for subcommand options.\n";

struct command {
	const char *name;
	const char *summary;
	int (*fn)(int argc, char **argv);
};

static const struct command commands[] = {
	{ "analyze", "decode HIDRAW reports from the remote", cmd_analyze },
	{ "imu", "read IMU via evdev: CSV, airmouse, AHRS, cube", cmd_imu },
	{ "calibrate", "fit accel/gyro calibration from an IMU CSV", cmd_calibrate },
	{ "calib2bin", "convert calibration JSON to a firmware blob", cmd_calib2bin },
	{ "config", "show / change the configuration", cmd_config },
	{ "setup", "interactive configuration and calibration wizard", cmd_setup },
	{ "device", "list devices / show the daemon status", cmd_device },
	{ "profile", "list / show / switch profiles", cmd_profile },
	{ "button", "list / remap / reset buttons", cmd_button },
	{ "scroll", "wheel speed and airmouse sensitivity", cmd_scroll },
	{ "diagnose", "print a report for bug reports", cmd_diagnose },
};

int main(int argc, char **argv)
{
	const char *extra_config = NULL;
	char **rest;
	int nrest = 0;
	size_t i;
	int rc = 1;

	/* Pull out the global flags; everything else goes to the subcommand. */
	rest = malloc((size_t)argc * sizeof(*rest));
	if (!rest) {
		fprintf(stderr, "lg-magic: out of memory\n");
		return 1;
	}
	for (i = 1; i < (size_t)argc; i++) {
		if (strcmp(argv[i], "--config") == 0) {
			if (i + 1 >= (size_t)argc) {
				fprintf(stderr, "lg-magic: --config needs a "
					"file argument\n");
				free(rest);
				return 1;
			}
			extra_config = argv[++i];
		} else if (strncmp(argv[i], "--config=", 9) == 0) {
			extra_config = argv[i] + 9;
		} else {
			rest[nrest++] = argv[i];
		}
	}
	rest[nrest] = NULL;

	if (nrest == 0 || strcmp(rest[0], "--help") == 0 ||
	    strcmp(rest[0], "-h") == 0) {
		fputs(usage_text, nrest == 0 ? stderr : stdout);
		free(rest);
		return nrest == 0 ? 1 : 0;
	}
	if (strcmp(rest[0], "--version") == 0) {
		printf("lg-magic %s\n", g_tool_version);
		free(rest);
		return 0;
	}

	for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		if (strcmp(rest[0], commands[i].name) == 0) {
			g_cfg = config_load(extra_config);
			if (!g_cfg) {
				fprintf(stderr, "lg-magic: out of memory\n");
				free(rest);
				return 1;
			}
			/* argv[0] keeps the subcommand name, git style. */
			rc = commands[i].fn(nrest, rest);
			config_free(g_cfg);
			free(rest);
			return rc;
		}
	}

	fprintf(stderr, "lg-magic: unknown subcommand '%s'\n\n", rest[0]);
	fputs(usage_text, stderr);
	free(rest);
	return 1;
}
