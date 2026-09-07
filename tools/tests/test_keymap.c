/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * test_keymap.c - unit tests for the keycode names and the kernel default
 * decode map (keymap.h).
 *
 * Covers: name<->code round-trips, the curated Linux uapi values, the LG
 * button-code names, and the consistency of keymap_default_map with the
 * kernel lg_btn_map (raw_only semantics: one WHEEL_PRESS entry).
 */
#include <stdio.h>

#include "test_util.h"
#include "keymap.h"

static void test_name_code(void)
{
	CHECK_INT_EQ(keymap_name_to_code("KEY_ENTER"), 28,
		     "KEY_ENTER -> 28");
	CHECK_INT_EQ(keymap_name_to_code("KEY_LIST"), 395,
		     "KEY_LIST -> 395");
	CHECK_INT_EQ(keymap_name_to_code("BTN_LEFT"), 272,
		     "BTN_LEFT -> 272");
	CHECK_INT_EQ(keymap_name_to_code("KEY_DOWN"), 108,
		     "KEY_DOWN -> 108");
	CHECK_INT_EQ(keymap_name_to_code(NULL), -1,
		     "NULL name -> -1");
	CHECK_INT_EQ(keymap_name_to_code("KEY_NOPE"), -1,
		     "unknown name -> -1");
	CHECK_STR_EQ(keymap_code_to_name(28), "KEY_ENTER",
		     "28 -> KEY_ENTER");
	CHECK_STR_EQ(keymap_code_to_name(395), "KEY_LIST",
		     "395 -> KEY_LIST");
	CHECK_PTR_EQ(keymap_code_to_name(9999), NULL,
		     "unknown code -> NULL");
}

static void test_default_map_consistency(void)
{
	const struct keymap_default_entry *map;
	size_t n, i;

	CHECK_STR_EQ(keymap_code_to_name(keymap_default_keycode(0x8044)),
		     "KEY_ENTER",
		     "WHEEL_PRESS defaults to KEY_ENTER under raw_only");
	CHECK_INT_EQ(keymap_default_keycode(0x8000), 116,
		     "POWER defaults to KEY_POWER");
	CHECK_INT_EQ(keymap_default_keycode(0x0000), -1,
		     "unknown LG code -> -1");

	map = keymap_default_map(&n);
	CHECK(map != NULL && n == 38,
	      "default map has 38 entries (one per unique kernel lg_btn_map "
	      "code; WHEEL_PRESS appears twice in the kernel)");
	for (i = 0; i < n; i++) {
		const char *name = keymap_code_to_name(map[i].keycode);

		CHECK(name != NULL &&
		      keymap_name_to_code(name) == map[i].keycode,
		      "default map keycode is a known round-trippable name");
		if (!name || keymap_name_to_code(name) != map[i].keycode) {
			printf("  (lg_code 0x%04x keycode %d name %s)\n",
			       map[i].lg_code, map[i].keycode,
			       name ? name : "(none)");
			break;
		}
	}
}

static void test_lg_names(void)
{
	CHECK_STR_EQ(keymap_lg_code_to_name(0x8044), "WHEEL_PRESS",
		     "LG code 0x8044 -> WHEEL_PRESS");
	CHECK_INT_EQ(keymap_lg_name_to_code("WHEEL_PRESS"), 0x8044,
		     "WHEEL_PRESS -> LG code 0x8044");
	CHECK_PTR_EQ(keymap_lg_code_to_name(0x0000), "None",
		     "LG code 0x0000 -> None");
	CHECK_PTR_EQ(keymap_lg_code_to_name(0xffff), NULL,
		     "unknown LG code -> NULL");
	CHECK_INT_EQ(keymap_lg_name_to_code("NO_SUCH_BUTTON"), -1,
		     "unknown LG name -> -1");
}

int main(void)
{
	TEST_BEGIN();
	test_name_code();
	test_default_map_consistency();
	test_lg_names();
	TEST_SUMMARY("test_keymap");
	return tu_fail_count ? 1 : 0;
}
