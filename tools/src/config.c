/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * config.c - lgmagic user configuration.
 *
 * Precedence: built-in defaults < /etc/lgmagic/config.toml <
 * ~/.config/lgmagic/config.toml < --config FILE < CLI flags.
 * Files are TOML, parsed with our own toml.c; unknown keys are ignored,
 * malformed files produce a warning and are skipped.  A v1 config.json
 * next to a missing config.toml produces a migrate hint.
 */
#include "config.h"

#include "build_version.h"
#include "toml.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define CFG_SYSTEM_DIR "/etc/lgmagic"
#define CFG_USER_DIR "/.config/lgmagic"

/* System config path: /etc/lgmagic/config.toml, or <root>/config.toml when
 * LGMAGIC_CONFIG_ROOT is set.  Same resolution as cmd_config_root() in
 * cmd_bus.c, duplicated locally because config.c also links into lgmagicd
 * and the unit tests, which do not link cmd_bus.c.  Test suites point the
 * root at a scratch dir so an installed host's system config never leaks
 * into them. */
static void config_system_path(char *buf, size_t bufsz)
{
	const char *root = getenv("LGMAGIC_CONFIG_ROOT");

	snprintf(buf, bufsz, "%s/config.toml",
		 (root && root[0]) ? root : CFG_SYSTEM_DIR);
}

struct config *g_cfg;
const char *g_tool_version = LGMAGIC_VERSION;

enum {
	X_IMU_DEVICE = 1 << 0,
	X_HIDRAW_DEVICE = 1 << 1,
	X_DEFAULT_CALIB = 1 << 2,
	X_LPF_ALPHA = 1 << 3,
	X_MOUSE_SCALE = 1 << 4,
	X_MADGWICK_BETA = 1 << 5,
	X_ALPHA = 1 << 6,
	X_MOUSE_K = 1 << 7,
	X_GYRO_SCALE = 1 << 8,
};

static unsigned explicit_mask;

/* ------------------------------------------------------------------ */
/* Key table                                                           */
/* ------------------------------------------------------------------ */

static int key_bit(const char *key)
{
	if (strcmp(key, "imu_device") == 0)
		return X_IMU_DEVICE;
	if (strcmp(key, "hidraw_device") == 0)
		return X_HIDRAW_DEVICE;
	if (strcmp(key, "default_calib") == 0)
		return X_DEFAULT_CALIB;
	if (strcmp(key, "lpf_alpha") == 0)
		return X_LPF_ALPHA;
	if (strcmp(key, "mouse_scale") == 0)
		return X_MOUSE_SCALE;
	if (strcmp(key, "madgwick_beta") == 0)
		return X_MADGWICK_BETA;
	if (strcmp(key, "alpha") == 0)
		return X_ALPHA;
	if (strcmp(key, "mouse_k") == 0)
		return X_MOUSE_K;
	if (strcmp(key, "gyro_scale_default") == 0)
		return X_GYRO_SCALE;
	return 0;
}

static void set_from_value(struct config *cfg, const char *key,
			   const struct toml_value *v)
{
	char *s;
	int bit = key_bit(key);

	if (!bit || !v)
		return;
	switch (bit) {
	case X_IMU_DEVICE:
	case X_HIDRAW_DEVICE:
	case X_DEFAULT_CALIB:
		if (v->type != TOML_STR)
			return;
		/* An empty string means "auto" - normalize to NULL. */
		if (v->str[0] == '\0')
			s = NULL;
		else {
			s = strdup(v->str);
			if (!s)
				return;
		}
		if (bit == X_IMU_DEVICE) {
			free(cfg->imu_device);
			cfg->imu_device = s;
		} else if (bit == X_HIDRAW_DEVICE) {
			free(cfg->hidraw_device);
			cfg->hidraw_device = s;
		} else {
			free(cfg->default_calib);
			cfg->default_calib = s;
		}
		break;
	case X_LPF_ALPHA:
		if (v->type != TOML_FLOAT && v->type != TOML_INT)
			return;
		cfg->lpf_alpha = v->type == TOML_FLOAT ? v->d : (double)v->i;
		break;
	case X_MOUSE_SCALE:
		if (v->type != TOML_FLOAT && v->type != TOML_INT)
			return;
		cfg->mouse_scale = v->type == TOML_FLOAT ? v->d : (double)v->i;
		break;
	case X_MADGWICK_BETA:
		if (v->type != TOML_FLOAT && v->type != TOML_INT)
			return;
		cfg->madgwick_beta = v->type == TOML_FLOAT ? v->d : (double)v->i;
		break;
	case X_ALPHA:
		if (v->type != TOML_FLOAT && v->type != TOML_INT)
			return;
		cfg->alpha = v->type == TOML_FLOAT ? v->d : (double)v->i;
		break;
	case X_MOUSE_K:
		if (v->type != TOML_FLOAT && v->type != TOML_INT)
			return;
		cfg->mouse_k = v->type == TOML_FLOAT ? v->d : (double)v->i;
		break;
	case X_GYRO_SCALE:
		if (v->type != TOML_FLOAT && v->type != TOML_INT)
			return;
		cfg->gyro_scale_default = v->type == TOML_FLOAT ?
			v->d : (double)v->i;
		break;
	}
	explicit_mask |= (unsigned)bit;
}

/* ------------------------------------------------------------------ */
/* Load                                                                */
/* ------------------------------------------------------------------ */

/* "<name>.toml" -> "<name>.json"; returns 0 when the path does not end
 * in ".toml" or does not fit. */
static int legacy_json_path(const char *path, char *out, size_t outsz)
{
	size_t len = strlen(path);

	if (len < 5 || len >= outsz || strcmp(path + len - 5, ".toml") != 0) {
		out[0] = '\0';
		return -1;
	}
	memcpy(out, path, len - 4);
	strcpy(out + len - 4, "json");
	return 0;
}

static void merge_file(struct config *cfg, const char *path)
{
	const char *err = NULL;
	size_t err_line = 0;
	struct toml_value *root;
	static const char *keys[] = {
		"imu_device", "hidraw_device", "default_calib",
		"lpf_alpha", "mouse_scale", "madgwick_beta",
		"alpha", "mouse_k", "gyro_scale_default",
	};
	size_t i;
	struct stat st;

	/* Missing config files are the normal case - skip silently; warn
	 * only when a file exists but cannot be parsed.  A leftover v1
	 * config.json next to a missing config.toml gets a hint. */
	if (stat(path, &st) != 0) {
		char legacy[4096];

		if (legacy_json_path(path, legacy, sizeof(legacy)) == 0 &&
		    stat(legacy, &st) == 0)
			fprintf(stderr, "warning: %s is v1 JSON and is no "
				"longer loaded; run 'lgmagic config migrate' "
				"to convert it\n", legacy);
		return;
	}

	root = toml_load_file(path, &err, &err_line);
	if (!root) {
		fprintf(stderr, "warning: ignoring config file %s: %s "
			"(line %zu)\n", path, err ? err : "parse error",
			err_line);
		return;
	}
	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		set_from_value(cfg, keys[i], toml_table_get_short(root, keys[i]));
	toml_free(root);
}

static void set_defaults(struct config *cfg)
{
	cfg->lpf_alpha = 0.2;
	cfg->mouse_scale = 30.0;
	cfg->madgwick_beta = 0.1;
	cfg->alpha = 0.2;
	cfg->mouse_k = 0.5;
	cfg->gyro_scale_default = 0.07;
}

struct config *config_load(const char *extra_path)
{
	struct config *cfg = calloc(1, sizeof(*cfg));
	const char *home;
	char path[4096];

	if (!cfg)
		return NULL;
	explicit_mask = 0;
	set_defaults(cfg);
	config_system_path(path, sizeof(path));
	merge_file(cfg, path);
	home = getenv("HOME");
	if (home) {
		snprintf(path, sizeof(path), "%s%s/config.toml", home,
			 CFG_USER_DIR);
		merge_file(cfg, path);
	}
	if (extra_path)
		merge_file(cfg, extra_path);
	return cfg;
}

struct config *config_load_daemon(const char *config_root)
{
	struct config *cfg = calloc(1, sizeof(*cfg));
	char path[4096];

	if (!cfg)
		return NULL;
	explicit_mask = 0;
	set_defaults(cfg);
	snprintf(path, sizeof(path), "%s/config.toml", config_root);
	merge_file(cfg, path);
	return cfg;
}

void config_free(struct config *cfg)
{
	if (!cfg)
		return;
	free(cfg->imu_device);
	free(cfg->hidraw_device);
	free(cfg->default_calib);
	free(cfg);
}

/* ------------------------------------------------------------------ */
/* Save / set / query                                                  */
/* ------------------------------------------------------------------ */

static int mkdir_p(const char *dir)
{
	char tmp[4096];
	char *p;
	size_t len;

	snprintf(tmp, sizeof(tmp), "%s", dir);
	len = strlen(tmp);
	if (len > 0 && tmp[len - 1] == '/')
		tmp[len - 1] = '\0';
	for (p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

int config_save_user(struct config *cfg, char *err, size_t errsz)
{
	const char *home = getenv("HOME");
	char dir[4096], path[sizeof(dir) + sizeof("/config.toml")];
	struct toml_value *root;
	char *text;
	FILE *f;

	if (!home) {
		snprintf(err, errsz, "HOME is not set; cannot save user config");
		return -1;
	}
	snprintf(dir, sizeof(dir), "%s%s", home, CFG_USER_DIR);
	snprintf(path, sizeof(path), "%s/config.toml", dir);
	if (mkdir_p(dir) < 0) {
		snprintf(err, errsz, "cannot create %s: %s", dir,
			 strerror(errno));
		return -1;
	}

	root = toml_new_table();
	if (!root)
		goto oom;
	{
		/* fixed key order, string keys first then numbers */
		struct {
			const char *key;
			int is_str;
			const void *val;
		} kvs[] = {
			{ "imu_device", 1, cfg->imu_device },
			{ "hidraw_device", 1, cfg->hidraw_device },
			{ "default_calib", 1, cfg->default_calib },
			{ "lpf_alpha", 0, &cfg->lpf_alpha },
			{ "mouse_scale", 0, &cfg->mouse_scale },
			{ "madgwick_beta", 0, &cfg->madgwick_beta },
			{ "alpha", 0, &cfg->alpha },
			{ "mouse_k", 0, &cfg->mouse_k },
			{ "gyro_scale_default", 0, &cfg->gyro_scale_default },
		};
		size_t i;

		for (i = 0; i < sizeof(kvs) / sizeof(kvs[0]); i++) {
			struct toml_value *v;

			if (kvs[i].is_str) {
				const char *s = kvs[i].val;

				v = toml_new_str(s ? s : "");
				if (!v)
					goto oom_free_root;
			} else {
				v = toml_new_float(*(const double *)kvs[i].val);
				if (!v)
					goto oom_free_root;
			}
			if (toml_table_add(root, kvs[i].key, v) < 0) {
				toml_free(v);
				goto oom_free_root;
			}
		}
	}
	text = toml_dumps(root);
	toml_free(root);
	if (!text)
		goto oom;
	f = fopen(path, "w");
	if (!f) {
		snprintf(err, errsz, "cannot open %s: %s", path,
			 strerror(errno));
		free(text);
		return -1;
	}
	if (fputs(text, f) < 0 || fclose(f) != 0) {
		snprintf(err, errsz, "write error on %s", path);
		free(text);
		return -1;
	}
	free(text);
	return 0;

oom_free_root:
	toml_free(root);
oom:
	snprintf(err, errsz, "out of memory");
	return -1;
}

int config_set_key(struct config *cfg, const char *key, const char *value,
		   char *err, size_t errsz)
{
	char *end;
	double d;

	switch (key_bit(key)) {
	case X_IMU_DEVICE:
		{
			char *s = value[0] ? strdup(value) : NULL;

			if (value[0] && !s)
				goto oom;
			free(cfg->imu_device);
			cfg->imu_device = s;
			break;
		}
	case X_HIDRAW_DEVICE:
		{
			char *s = value[0] ? strdup(value) : NULL;

			if (value[0] && !s)
				goto oom;
			free(cfg->hidraw_device);
			cfg->hidraw_device = s;
			break;
		}
	case X_DEFAULT_CALIB:
		{
			char *s = value[0] ? strdup(value) : NULL;

			if (value[0] && !s)
				goto oom;
			free(cfg->default_calib);
			cfg->default_calib = s;
			break;
		}
	case X_LPF_ALPHA:
	case X_MOUSE_SCALE:
	case X_MADGWICK_BETA:
	case X_ALPHA:
	case X_MOUSE_K:
	case X_GYRO_SCALE:
		errno = 0;
		d = strtod(value, &end);
		if (errno != 0 || end == value || *end != '\0') {
			snprintf(err, errsz, "invalid value for %s: '%s'",
				 key, value);
			return -1;
		}
		switch (key_bit(key)) {
		case X_LPF_ALPHA: cfg->lpf_alpha = d; break;
		case X_MOUSE_SCALE: cfg->mouse_scale = d; break;
		case X_MADGWICK_BETA: cfg->madgwick_beta = d; break;
		case X_ALPHA: cfg->alpha = d; break;
		case X_MOUSE_K: cfg->mouse_k = d; break;
		default: cfg->gyro_scale_default = d; break;
		}
		break;
	default:
		snprintf(err, errsz, "unknown config key '%s'", key);
		return -1;
	}
	explicit_mask |= (unsigned)key_bit(key);
	return 0;

oom:
	snprintf(err, errsz, "out of memory");
	return -1;
}

int config_is_explicit(const char *key)
{
	int bit = key_bit(key);

	return bit ? (int)(explicit_mask & (unsigned)bit) : 0;
}

/* ------------------------------------------------------------------ */
/* Print                                                               */
/* ------------------------------------------------------------------ */

static void print_num(const char *key, double v)
{
	printf("%-20s = %g%s\n", key, v,
	       config_is_explicit(key) ? " (from config)" : " (default)");
}

static void print_str(const char *key, const char *v)
{
	printf("%-20s = %s%s\n", key, v ? v : "(auto)",
	       config_is_explicit(key) ? " (from config)" : " (default)");
}

void config_print(const struct config *cfg)
{
	print_str("imu_device", cfg->imu_device);
	print_str("hidraw_device", cfg->hidraw_device);
	print_str("default_calib", cfg->default_calib);
	print_num("lpf_alpha", cfg->lpf_alpha);
	print_num("mouse_scale", cfg->mouse_scale);
	print_num("madgwick_beta", cfg->madgwick_beta);
	print_num("alpha", cfg->alpha);
	print_num("mouse_k", cfg->mouse_k);
	print_num("gyro_scale_default", cfg->gyro_scale_default);
}

void config_print_paths(void)
{
	const char *home = getenv("HOME");
	char syspath[4096];

	config_system_path(syspath, sizeof(syspath));
	printf("system: %s\n", syspath);
	printf("user:   %s%s/config.toml\n", home ? home : "$HOME",
	       CFG_USER_DIR);
}
