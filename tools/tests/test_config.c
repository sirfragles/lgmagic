/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * test_config.c - unit tests for the user configuration (config.h).
 *
 * Precedence: built-in defaults < /etc/lg-magic/config.toml <
 * ~/.config/lg-magic/config.toml < --config FILE.  The test points HOME at
 * a scratch directory so it never touches (or depends on) the real user
 * config, and uses a scratch extra_path for the --config slot.  Only a
 * system-wide /etc/lg-magic/config.toml on the host would leak in (there
 * is no way to override that slot); the tests assume none is present.
 *
 * Covers:
 *  - built-in defaults and "not explicit" after config_load(NULL);
 *  - config_set_key: numeric/string keys, unknown key and bad value -> -1,
 *    explicit tracking;
 *  - config_save_user writes ~/.config/lg-magic/config.toml and a fresh
 *    config_load(NULL) picks it up and marks the keys explicit;
 *  - extra_path overrides the user file; unknown keys in any file are
 *    ignored; a malformed file is skipped with a warning (defaults kept);
 *  - round-trip through the saved TOML file preserves values.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_util.h"
#include "config.h"
#include "toml.h"

static char scratch[4096];	/* temp HOME directory */

static int make_scratch(void)
{
	const char *dir = getenv("TMPDIR");
	char tmpl[4096];

	if (!dir || !*dir)
		dir = "/tmp";
	snprintf(tmpl, sizeof(tmpl), "%s/lgmagic-cfg-XXXXXX", dir);
	if (!mkdtemp(tmpl))
		return -1;
	snprintf(scratch, sizeof(scratch), "%s", tmpl);
	return setenv("HOME", scratch, 1) == 0 ? 0 : -1;
}

static void test_defaults(void)
{
	struct config *cfg = config_load(NULL);
	int ok;

	CHECK(cfg != NULL, "config_load(NULL) succeeds");
	if (!cfg)
		return;
	ok = cfg->lpf_alpha == 0.2 && cfg->mouse_scale == 30.0 &&
	     cfg->madgwick_beta == 0.1 && cfg->alpha == 0.2 &&
	     cfg->mouse_k == 0.5 && cfg->gyro_scale_default == 0.07;
	CHECK(ok == 1, "numeric defaults are 0.2/30.0/0.1/0.2/0.5/0.07");
	CHECK(cfg->imu_device == NULL && cfg->hidraw_device == NULL &&
	      cfg->default_calib == NULL,
	      "device paths default to NULL (auto-detect)");
	CHECK(config_is_explicit("lpf_alpha") == 0,
	      "defaults are not marked explicit");
	CHECK(config_is_explicit("no_such_key") == 0,
	      "unknown keys are never explicit");
	config_free(cfg);
}

static void test_set_key(void)
{
	struct config *cfg = config_load(NULL);
	char err[128];

	CHECK(cfg != NULL, "config_load(NULL) succeeds for set-key tests");
	if (!cfg)
		return;

	CHECK(config_set_key(cfg, "lpf_alpha", "0.35", err, sizeof(err)) == 0 &&
	      cfg->lpf_alpha == 0.35,
	      "config_set_key(\"lpf_alpha\", \"0.35\") sets the value");
	CHECK(config_is_explicit("lpf_alpha") != 0,
	      "set keys are marked explicit");
	CHECK(config_set_key(cfg, "imu_device", "/dev/input/event9",
			     err, sizeof(err)) == 0 &&
	      strcmp(cfg->imu_device, "/dev/input/event9") == 0,
	      "config_set_key accepts device path keys");
	CHECK(config_set_key(cfg, "gyro_scale_default", "0.09",
			     err, sizeof(err)) == 0 &&
	      cfg->gyro_scale_default == 0.09,
	      "numeric key with a decimal exponent-free value is set");
	CHECK(config_set_key(cfg, "no_such_key", "1", err, sizeof(err)) == -1 &&
	      err[0] != '\0',
	      "unknown key returns -1 with an error message");
	CHECK(config_set_key(cfg, "lpf_alpha", "abc", err, sizeof(err)) == -1,
	      "non-numeric value for a numeric key returns -1");
	CHECK(config_set_key(cfg, "lpf_alpha", "0.5x", err, sizeof(err)) == -1,
	      "trailing garbage in a numeric value returns -1");
	CHECK(cfg->lpf_alpha == 0.35,
	      "failed set attempts leave the previous value intact");
	config_free(cfg);
}

static void test_save_load_user(void)
{
	struct config *cfg = config_load(NULL);
	struct config *cfg2;
	char err[256];
	char path[sizeof(scratch) + 64];
	struct toml_value *root;
	const char *terr = NULL;
	size_t line = 0;
	struct toml_value *v;

	CHECK(cfg != NULL, "config_load(NULL) succeeds for save tests");
	if (!cfg)
		return;
	CHECK(config_set_key(cfg, "lpf_alpha", "0.35", err, sizeof(err)) == 0 &&
	      config_set_key(cfg, "imu_device", "/dev/input/event9",
			     err, sizeof(err)) == 0 &&
	      config_set_key(cfg, "mouse_k", "0.75", err, sizeof(err)) == 0,
	      "preparing a modified config for saving");

	CHECK(config_save_user(cfg, err, sizeof(err)) == 0,
	      "config_save_user writes the user config");
	config_free(cfg);

	snprintf(path, sizeof(path), "%s/.config/lg-magic/config.toml",
		 scratch);
	CHECK(fopen(path, "rb") != NULL, "~/.config/lg-magic/config.toml "
	       "exists after saving");
	root = toml_load_file(path, &terr, &line);
	CHECK(root != NULL, "saved user config is valid TOML");
	if (root) {
		v = toml_table_get_short(root, "lpf_alpha");
		CHECK(v && v->type == TOML_FLOAT && v->d == 0.35,
		      "saved lpf_alpha == 0.35");
		v = toml_table_get_short(root, "imu_device");
		CHECK(v && v->type == TOML_STR &&
		      strcmp(v->str, "/dev/input/event9") == 0,
		      "saved imu_device string");
		v = toml_table_get_short(root, "mouse_scale");
		CHECK(v && v->type == TOML_FLOAT && v->d == 30.0,
		      "default numeric keys are saved too");
		toml_free(root);
	}

	/* a fresh load must now pick the file up (precedence: user file
	 * over the built-in defaults) */
	cfg2 = config_load(NULL);
	CHECK(cfg2 != NULL, "second config_load(NULL) succeeds");
	if (cfg2) {
		CHECK(cfg2->lpf_alpha == 0.35 &&
		      cfg2->mouse_k == 0.75 &&
		      strcmp(cfg2->imu_device, "/dev/input/event9") == 0,
		      "fresh load applies the saved user config");
		CHECK(config_is_explicit("lpf_alpha") != 0 &&
		      config_is_explicit("mouse_k") != 0,
		      "keys read from the user file are marked explicit");
		CHECK(config_is_explicit("mouse_scale") != 0 &&
		      cfg2->mouse_scale == 30.0,
		      "save writes every key (defaults included), so "
		      "mouse_scale comes back explicit with its default value");
		config_free(cfg2);
	}
}

static void test_extra_path(void)
{
	char epath[sizeof(scratch) + 64];
	struct config *cfg;
	int rc;

	/* --config file overrides the user file (0.35 < 0.7); unknown keys
	 * are ignored */
	snprintf(epath, sizeof(epath), "%s/extra.toml", scratch);
	rc = tu_write_file(epath,
			   "lpf_alpha = 0.7\nmouse_k = 0.9\nbogus_key = 12\n",
			   strlen("lpf_alpha = 0.7\nmouse_k = 0.9\n"
				  "bogus_key = 12\n"));
	CHECK(rc == 0, "can write the --config file");
	if (rc != 0)
		return;

	cfg = config_load(epath);
	CHECK(cfg != NULL, "config_load(extra_path) succeeds");
	if (!cfg)
		return;
	CHECK(cfg->lpf_alpha == 0.7 && cfg->mouse_k == 0.9,
	      "extra_path overrides the user config file");
	CHECK(strcmp(cfg->imu_device, "/dev/input/event9") == 0,
	      "keys absent from extra_path keep the user-file value");
	CHECK(config_is_explicit("lpf_alpha") != 0 &&
	      config_is_explicit("mouse_k") != 0,
	      "extra_path keys are marked explicit");
	config_free(cfg);

	/* a malformed --config file is skipped with a warning */
	{
		char bad[sizeof(scratch) + 64];
		struct config *cfg2;

		snprintf(bad, sizeof(bad), "%s/bad.toml", scratch);
		if (tu_write_file(bad, "alpha = \n", 8) == 0) {
			cfg2 = config_load(bad);
			CHECK(cfg2 != NULL && cfg2->alpha == 0.2,
			      "malformed config file is skipped, defaults kept");
			config_free(cfg2);
		} else {
			CHECK(0, "can write the malformed config file");
		}
	}

	/* a file with unknown keys only must load cleanly */
	{
		char onlybad[sizeof(scratch) + 64];
		struct config *cfg2;

		snprintf(onlybad, sizeof(onlybad), "%s/onlybad.toml", scratch);
		if (tu_write_file(onlybad, "totally_unknown = 1\n",
				  strlen("totally_unknown = 1\n")) == 0) {
			cfg2 = config_load(onlybad);
			CHECK(cfg2 != NULL && cfg2->alpha == 0.2,
			      "unknown keys in a config file are ignored");
			config_free(cfg2);
		} else {
			CHECK(0, "can write the unknown-key config file");
		}
	}
	remove(epath);
}

int main(void)
{
	TEST_BEGIN();
	if (make_scratch() != 0) {
		CHECK(0, "can create a scratch HOME directory");
		TEST_SUMMARY("test_config");
		return 1;
	}
	printf("  (HOME=%s)\n", scratch);
	test_defaults();
	test_set_key();
	test_save_load_user();
	test_extra_path();
	tu_rm_rf(scratch);
	TEST_SUMMARY("test_config");
	return tu_fail_count ? 1 : 0;
}
