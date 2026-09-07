/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * test_profiles.c - unit tests for per-device profiles and button maps
 * (profiles.h).
 *
 * Covers: built-in defaults, the has_* explicit-set flags, button map
 * add/overwrite semantics, validation ranges, merge precedence, and
 * device TOML parsing (profiles, calib, airmouse) including error paths.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_util.h"
#include "keymap.h"
#include "profiles.h"
#include "toml.h"

static char err[256];

static void test_profile_basics(void)
{
	struct profile p;

	CHECK(profile_init(&p, "default") == 0, "profile_init succeeds");
	CHECK_STR_EQ(p.name, "default", "profile name copied");
	CHECK_FLOAT_EQ(p.scroll_speed, 1.0, "default scroll_speed 1.0");
	CHECK_FLOAT_EQ(p.sensitivity, 30.0, "default sensitivity 30.0");
	CHECK_FLOAT_EQ(p.lpf_alpha, -1.0, "lpf_alpha unset (-1)");
	CHECK(p.has_scroll == 0 && p.has_sens == 0 && p.has_lpf == 0,
	      "no field is marked explicitly set");
	CHECK_INT_EQ((long long)p.nmap, 0, "empty button map");

	CHECK(profile_set_button(&p, 28, 272) == 0, "add a button mapping");
	CHECK_INT_EQ((long long)p.nmap, 1, "map grows to 1");
	CHECK(profile_set_button(&p, 28, 273) == 0, "overwrite the mapping");
	CHECK_INT_EQ((long long)p.nmap, 1, "overwrite does not grow the map");
	CHECK_INT_EQ(p.map[0].to, 273, "overwritten 'to' keycode");
	profile_clear_buttons(&p);
	CHECK_INT_EQ((long long)p.nmap, 0, "clear_buttons empties the map");
	profile_free(&p);
}

static void test_profile_validate(void)
{
	struct profile p;
	char err2[256];

	CHECK(profile_init(&p, "test") == 0, "profile_init for validation");
	CHECK(profile_validate(&p, err, sizeof(err)) == 0,
	      "default profile validates");

	p.scroll_speed = 0.0;
	CHECK(profile_validate(&p, err2, sizeof(err2)) == -1 && err2[0],
	      "scroll_speed 0 rejected");
	p.scroll_speed = 100.0;
	CHECK(profile_validate(&p, err2, sizeof(err2)) == 0,
	      "scroll_speed 100 accepted");
	p.scroll_speed = 100.01;
	CHECK(profile_validate(&p, err2, sizeof(err2)) == -1,
	      "scroll_speed above the range rejected");
	p.scroll_speed = 1.0;
	p.sensitivity = 10000.0;
	CHECK(profile_validate(&p, err2, sizeof(err2)) == 0,
	      "sensitivity 10000 accepted");
	p.sensitivity = 10000.1;
	CHECK(profile_validate(&p, err2, sizeof(err2)) == -1,
	      "sensitivity above the range rejected");
	p.sensitivity = 30.0;
	p.lpf_alpha = 1.5;
	p.has_lpf = 1;
	CHECK(profile_validate(&p, err2, sizeof(err2)) == -1,
	      "lpf_alpha > 1 rejected");
	p.lpf_alpha = -1.0;
	p.has_lpf = 0;
	CHECK(profile_validate(&p, err2, sizeof(err2)) == 0,
	      "unset lpf_alpha ignored by validation");
	profile_set_button(&p, 9999, 28);
	CHECK(profile_validate(&p, err2, sizeof(err2)) == -1,
	      "unknown 'from' keycode rejected");
	profile_clear_buttons(&p);
	profile_set_button(&p, 28, 9999);
	CHECK(profile_validate(&p, err2, sizeof(err2)) == -1,
	      "unknown 'to' keycode rejected");
	profile_free(&p);
}

static void test_merge(void)
{
	struct profile dst, src;

	profile_init(&dst, "dst");
	profile_init(&src, "src");
	dst.scroll_speed = 2.0;
	dst.sensitivity = 40.0;
	profile_set_button(&dst, 28, 272);
	profile_set_button(&dst, 103, 108);

	/* src with nothing set: a no-op merge */
	CHECK(profile_merge(&dst, &src) == 0, "merge of an empty src succeeds");
	CHECK_FLOAT_EQ(dst.scroll_speed, 2.0, "empty src keeps dst scroll");
	CHECK_FLOAT_EQ(dst.sensitivity, 40.0, "empty src keeps dst sensitivity");

	src.scroll_speed = 3.0;
	src.has_scroll = 1;
	src.lpf_alpha = 0.4;
	src.has_lpf = 1;
	profile_set_button(&src, 28, 273);	/* overwrites dst */
	profile_set_button(&src, 106, 105);	/* new entry */
	CHECK(profile_merge(&dst, &src) == 0, "merge of a filled src succeeds");
	CHECK_FLOAT_EQ(dst.scroll_speed, 3.0, "has_scroll overwrites");
	CHECK_FLOAT_EQ(dst.sensitivity, 40.0, "unset sensitivity keeps dst");
	CHECK_FLOAT_EQ(dst.lpf_alpha, 0.4, "has_lpf overwrites");
	CHECK_INT_EQ((long long)dst.nmap, 3, "button maps merged by 'from'");
	CHECK_INT_EQ(dst.map[0].to, 273, "existing entry overwritten");

	profile_free(&dst);
	profile_free(&src);
}

static void test_device_toml(void)
{
	static const char *doc =
		"profile = \"tv\"\n"
		"calib = \"/var/lib/lg-magic/x/calibration.json\"\n"
		"airmouse = false\n"
		"unknown_key = \"ignored\"\n"
		"\n"
		"[profiles.default]\n"
		"scroll_speed = 2.0\n"
		"sensitivity = 40.0\n"
		"lpf_alpha = 0.3\n"
		"\n"
		"[profiles.default.button_map]\n"
		"\"KEY_ENTER\" = \"BTN_LEFT\"\n"
		"\"KEY_VOLUMEUP\" = \"KEY_UP\"\n"
		"\n"
		"[profiles.tv]\n"
		"scroll_speed = 3.0\n";
	struct toml_value *root = toml_parse(doc, strlen(doc), NULL, NULL);
	struct device_config dc;
	const struct profile *p;

	CHECK(root != NULL, "device TOML parses");
	if (!root)
		return;
	device_config_init(&dc);
	CHECK(device_config_from_toml(root, &dc, err, sizeof(err)) == 0,
	      "device config from TOML succeeds");
	CHECK_STR_EQ(dc.profile, "tv", "active profile from the file");
	CHECK_STR_EQ(dc.calib, "/var/lib/lg-magic/x/calibration.json",
		     "calib path from the file");
	CHECK(dc.airmouse == 0 && dc.has_airmouse == 1,
	      "airmouse = false parsed with the explicit flag");
	CHECK_INT_EQ((long long)dc.nprofiles, 2, "two profiles parsed");

	p = profile_find(&dc, "default");
	CHECK(p != NULL, "profile 'default' found");
	if (p) {
		CHECK_FLOAT_EQ(p->scroll_speed, 2.0, "scroll_speed parsed");
		CHECK(p->has_scroll && p->has_sens && p->has_lpf,
		      "all three numeric fields marked explicit");
		CHECK_FLOAT_EQ(p->lpf_alpha, 0.3, "lpf_alpha parsed");
		CHECK_INT_EQ((long long)p->nmap, 2, "two button mappings");
		CHECK_INT_EQ(p->map[0].from, 28, "KEY_ENTER -> BTN_LEFT from");
		CHECK_INT_EQ(p->map[0].to, 272, "KEY_ENTER -> BTN_LEFT to");
		CHECK_INT_EQ(p->map[1].from, 115, "KEY_VOLUMEUP -> KEY_UP from");
		CHECK_INT_EQ(p->map[1].to, 103, "KEY_VOLUMEUP -> KEY_UP to");
	}
	p = profile_find(&dc, "tv");
	CHECK(p != NULL, "profile 'tv' found");
	if (p) {
		CHECK_FLOAT_EQ(p->scroll_speed, 3.0, "tv scroll_speed parsed");
		CHECK(p->has_scroll && !p->has_sens && !p->has_lpf,
		      "tv marks only scroll_speed explicit");
		CHECK_FLOAT_EQ(p->sensitivity, 30.0, "tv sensitivity default");
	}
	device_config_free(&dc);
	toml_free(root);
}

static void test_device_toml_errors(void)
{
	static const char *bad_airmouse =
		"airmouse = \"yes\"\n";
	static const char *bad_scroll =
		"[profiles.default]\nscroll_speed = \"fast\"\n";
	static const char *bad_keycode =
		"[profiles.default.button_map]\n\"KEY_ENTER\" = \"KEY_NOPE\"\n";
	static const char *bad_active =
		"profile = \"nonexistent\"\n";
	struct toml_value *root;
	struct device_config dc;

	root = toml_parse(bad_airmouse, strlen(bad_airmouse), NULL, NULL);
	CHECK(root != NULL, "bad-airmouse TOML parses");
	device_config_init(&dc);
	CHECK(root && device_config_from_toml(root, &dc, err, sizeof(err)) == -1,
	      "non-boolean airmouse rejected");
	toml_free(root);
	device_config_free(&dc);

	root = toml_parse(bad_scroll, strlen(bad_scroll), NULL, NULL);
	CHECK(root != NULL, "bad-scroll TOML parses");
	device_config_init(&dc);
	CHECK(root && device_config_from_toml(root, &dc, err, sizeof(err)) == -1,
	      "string scroll_speed rejected");
	toml_free(root);
	device_config_free(&dc);

	root = toml_parse(bad_keycode, strlen(bad_keycode), NULL, NULL);
	CHECK(root != NULL, "bad-keycode TOML parses");
	device_config_init(&dc);
	CHECK(root && device_config_from_toml(root, &dc, err, sizeof(err)) == -1,
	      "unknown keycode in the button map rejected");
	toml_free(root);
	device_config_free(&dc);

	root = toml_parse(bad_active, strlen(bad_active), NULL, NULL);
	CHECK(root != NULL, "bad-active TOML parses");
	device_config_init(&dc);
	CHECK(root && device_config_from_toml(root, &dc, err, sizeof(err)) == -1,
	      "active profile that is not defined rejected");
	toml_free(root);
	device_config_free(&dc);
}

static void test_load_file(void)
{
	char path[4096];
	struct device_config dc;

	if (tu_temp_path(path, sizeof(path), "profiles") != 0) {
		CHECK(0, "can create a temp path");
		return;
	}
	remove(path);
	device_config_init(&dc);
	CHECK_INT_EQ(device_config_load_file(path, &dc, err, sizeof(err)), 1,
		     "missing device file -> 1 (keep defaults)");
	device_config_free(&dc);

	if (tu_write_file(path, "profile = 5\n", strlen("profile = 5\n")) != 0) {
		CHECK(0, "can write the malformed device file");
		return;
	}
	device_config_init(&dc);
	CHECK_INT_EQ(device_config_load_file(path, &dc, err, sizeof(err)), -1,
		     "malformed device file -> -1 with err");
	CHECK(err[0] != '\0', "error message set for the malformed file");
	device_config_free(&dc);

	if (tu_write_file(path, "profile = \"tv\"\n\n[profiles.tv]\n",
			  strlen("profile = \"tv\"\n\n[profiles.tv]\n")) != 0) {
		CHECK(0, "can write the valid device file");
		return;
	}
	device_config_init(&dc);
	CHECK_INT_EQ(device_config_load_file(path, &dc, err, sizeof(err)), 0,
		     "valid device file -> 0");
	CHECK_STR_EQ(dc.profile, "tv", "profile read from the file");
	device_config_free(&dc);
	remove(path);
}

int main(void)
{
	TEST_BEGIN();
	test_profile_basics();
	test_profile_validate();
	test_merge();
	test_device_toml();
	test_device_toml_errors();
	test_load_file();
	TEST_SUMMARY("test_profiles");
	return tu_fail_count ? 1 : 0;
}
