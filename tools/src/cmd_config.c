/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cmd_config.c - `lg-magic config` subcommand.
 *
 *   lg-magic config                   print the effective configuration
 *   lg-magic config set KEY VALUE     set one key, save to the user file
 *   lg-magic config path              print the config file paths
 *   lg-magic config migrate [FILE]    convert a v1 config.json to TOML
 *
 * Keys: imu_device, hidraw_device, default_calib (strings) and
 * lpf_alpha, mouse_scale, madgwick_beta, alpha, mouse_k,
 * gyro_scale_default (numbers).
 */
#include "config.h"

#include "json.h"
#include "toml.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void usage(FILE *out)
{
	fputs("Usage: lg-magic config [show|set KEY VALUE|path|migrate [FILE]]\n"
	      "\n"
	      "  (no argument)    print the effective configuration\n"
	      "  set KEY VALUE    set one key and save it to\n"
	      "                   ~/.config/lg-magic/config.toml\n"
	      "  path             print the config file locations\n"
	      "  migrate [FILE]   convert a v1 config.json to TOML\n"
	      "                   (the JSON source is kept)\n"
	      "\n"
	      "Keys (strings): imu_device, hidraw_device, default_calib\n"
	      "Keys (numbers): lpf_alpha, mouse_scale, madgwick_beta,\n"
	      "                alpha, mouse_k, gyro_scale_default\n", out);
}

/* ------------------------------------------------------------------ */
/* migrate                                                             */
/* ------------------------------------------------------------------ */

/* Convert one v1 JSON config to "<same name>.toml".  Never overwrites an
 * existing .toml and never deletes the JSON.  Returns 0 when there was
 * nothing to do or the conversion succeeded, 1 on error. */
static int migrate_one(const char *json_path)
{
	static const char *keys[] = {
		"imu_device", "hidraw_device", "default_calib",
		"lpf_alpha", "mouse_scale", "madgwick_beta",
		"alpha", "mouse_k", "gyro_scale_default",
	};
	const char *err = NULL;
	size_t eoff = 0;
	struct json_value *root = NULL;
	struct toml_value *out = NULL;
	char *text = NULL;
	char toml_path[4096];
	struct stat st;
	FILE *f;
	size_t len = strlen(json_path);
	size_t i;
	int rc = 0;

	if (len < 5 || strcmp(json_path + len - 5, ".json") != 0) {
		fprintf(stderr, "lg-magic config migrate: '%s' does not end "
			"in .json\n", json_path);
		return 1;
	}
	if (len + 1 >= sizeof(toml_path)) {
		fprintf(stderr, "lg-magic config migrate: path too long: %s\n",
			json_path);
		return 1;
	}
	memcpy(toml_path, json_path, len - 4);
	strcpy(toml_path + len - 4, "toml");

	if (stat(json_path, &st) != 0) {
		if (errno == ENOENT) {
			printf("skip %s: no v1 config\n", json_path);
			return 0;
		}
		fprintf(stderr, "lg-magic config migrate: %s: %s\n", json_path,
			strerror(errno));
		return 1;
	}
	if (stat(toml_path, &st) == 0) {
		printf("skip %s: %s already exists\n", json_path, toml_path);
		return 0;
	}

	root = json_load_file(json_path, &err, &eoff);
	if (!root) {
		fprintf(stderr, "lg-magic config migrate: %s: %s (byte %zu)\n",
			json_path, err ? err : "parse error", eoff);
		return 1;
	}
	if (root->type != JSON_OBJ) {
		fprintf(stderr, "lg-magic config migrate: %s: root is not an "
			"object\n", json_path);
		rc = 1;
		goto done;
	}
	out = toml_new_table();
	if (!out) {
		fprintf(stderr, "lg-magic config migrate: out of memory\n");
		rc = 1;
		goto done;
	}
	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		struct json_value *v = json_obj_get(root, keys[i]);
		struct toml_value *tv = NULL;

		if (!v)
			continue;
		if (v->type == JSON_STR)
			tv = toml_new_str(v->str);
		else if (v->type == JSON_NUM)
			tv = toml_new_float(v->num);
		else {
			fprintf(stderr, "lg-magic config migrate: skipping "
				"'%s': unsupported JSON value\n", keys[i]);
			continue;
		}
		if (!tv || toml_table_add(out, keys[i], tv) < 0) {
			if (tv)
				toml_free(tv);
			fprintf(stderr, "lg-magic config migrate: out of "
				"memory\n");
			rc = 1;
			goto done;
		}
	}

	text = toml_dumps(out);
	if (!text) {
		fprintf(stderr, "lg-magic config migrate: out of memory\n");
		rc = 1;
		goto done;
	}
	f = fopen(toml_path, "w");
	if (!f) {
		fprintf(stderr, "lg-magic config migrate: cannot open %s: %s\n",
			toml_path, strerror(errno));
		rc = 1;
		goto done;
	}
	if (fputs(text, f) < 0 || fclose(f) != 0) {
		fprintf(stderr, "lg-magic config migrate: write error on %s\n",
			toml_path);
		rc = 1;
		goto done;
	}
	printf("migrated %s -> %s\n", json_path, toml_path);

done:
	free(text);
	if (root)
		json_free(root);
	toml_free(out);
	return rc;
}

int cmd_config(int argc, char **argv)
{
	char err[256];

	if (argc >= 2 && (strcmp(argv[1], "--help") == 0 ||
			  strcmp(argv[1], "-h") == 0)) {
		usage(stdout);
		return 0;
	}

	/* No arguments: show the effective configuration. */
	if (argc == 1) {
		config_print(g_cfg);
		return 0;
	}

	if (strcmp(argv[1], "show") == 0) {
		if (argc != 2) {
			usage(stderr);
			return 1;
		}
		config_print(g_cfg);
		return 0;
	}

	if (strcmp(argv[1], "path") == 0) {
		if (argc != 2) {
			usage(stderr);
			return 1;
		}
		config_print_paths();
		return 0;
	}

	if (strcmp(argv[1], "set") == 0) {
		if (argc != 4) {
			fprintf(stderr, "usage: lg-magic config set "
				"KEY VALUE\n");
			return 1;
		}
		if (config_set_key(g_cfg, argv[2], argv[3],
				   err, sizeof(err)) < 0) {
			fprintf(stderr, "lg-magic: %s\n", err);
			return 1;
		}
		if (config_save_user(g_cfg, err, sizeof(err)) < 0) {
			fprintf(stderr, "lg-magic: %s\n", err);
			return 1;
		}
		printf("%s = %s\n", argv[2], argv[3]);
		return 0;
	}

	if (strcmp(argv[1], "migrate") == 0) {
		const char *home;
		char path[4096];
		int rc;

		if (argc > 3) {
			fprintf(stderr, "usage: lg-magic config migrate "
				"[FILE]\n");
			return 1;
		}
		if (argc == 3)
			return migrate_one(argv[2]);
		/* Both standard locations; /etc first (may need root). */
		rc = migrate_one("/etc/lg-magic/config.json");
		home = getenv("HOME");
		if (!home) {
			printf("skip user config: HOME is not set\n");
			return rc;
		}
		if (snprintf(path, sizeof(path),
			     "%s/.config/lg-magic/config.json", home) >=
		    (int)sizeof(path)) {
			fprintf(stderr, "lg-magic config migrate: HOME path "
				"too long\n");
			return rc ? rc : 1;
		}
		return rc || migrate_one(path);
	}

	fprintf(stderr, "lg-magic config: unknown argument '%s'\n\n", argv[1]);
	usage(stderr);
	return 1;
}
