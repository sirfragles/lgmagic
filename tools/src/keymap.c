/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * keymap.c - keycode names and the LG button decode map (portable).
 * See keymap.h; values below are the frozen Linux input uapi ABI.
 */
#include "keymap.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Keycode names (curated Linux uapi subset; see keymap.h)             */
/* ------------------------------------------------------------------ */

static const struct {
	int code;
	const char *name;
} key_names[] = {
	/* codes the kernel module can emit (lg_btn_map) */
	{ 28, "KEY_ENTER" },
	{ 11, "KEY_0" }, { 2, "KEY_1" }, { 3, "KEY_2" }, { 4, "KEY_3" },
	{ 5, "KEY_4" }, { 6, "KEY_5" }, { 7, "KEY_6" }, { 8, "KEY_7" },
	{ 9, "KEY_8" }, { 10, "KEY_9" },
	{ 103, "KEY_UP" }, { 108, "KEY_DOWN" },
	{ 105, "KEY_LEFT" }, { 106, "KEY_RIGHT" },
	{ 116, "KEY_POWER" }, { 142, "KEY_SLEEP" },
	{ 115, "KEY_VOLUMEUP" }, { 114, "KEY_VOLUMEDOWN" },
	{ 113, "KEY_MUTE" }, { 582, "KEY_VOICECOMMAND" },
	{ 102, "KEY_HOME" }, { 141, "KEY_SETUP" }, { 158, "KEY_BACK" },
	{ 139, "KEY_MENU" }, { 395, "KEY_LIST" },
	{ 362, "KEY_PROGRAM" }, { 226, "KEY_MEDIA" }, { 377, "KEY_TV" },
	{ 438, "KEY_CONTEXT_MENU" }, { 403, "KEY_CHANNELDOWN" },
	{ 398, "KEY_RED" }, { 399, "KEY_GREEN" }, { 400, "KEY_YELLOW" },
	{ 401, "KEY_BLUE" }, { 393, "KEY_VIDEO" },
	{ 207, "KEY_PLAY" }, { 119, "KEY_PAUSE" },
	/* common mapping targets (not emitted by the kernel) */
	{ 272, "BTN_LEFT" }, { 273, "BTN_RIGHT" }, { 274, "BTN_MIDDLE" },
	{ 1, "KEY_ESC" }, { 15, "KEY_TAB" }, { 57, "KEY_SPACE" },
	{ 128, "KEY_STOP" }, { 352, "KEY_OK" }, { 174, "KEY_EXIT" },
	{ 407, "KEY_NEXT" }, { 412, "KEY_PREVIOUS" },
	{ 104, "KEY_PAGEUP" }, { 109, "KEY_PAGEDOWN" },
};

int keymap_name_to_code(const char *name)
{
	size_t i;

	if (!name)
		return -1;
	for (i = 0; i < sizeof(key_names) / sizeof(key_names[0]); i++)
		if (strcmp(key_names[i].name, name) == 0)
			return key_names[i].code;
	return -1;
}

const char *keymap_code_to_name(int code)
{
	size_t i;

	for (i = 0; i < sizeof(key_names) / sizeof(key_names[0]); i++)
		if (key_names[i].code == code)
			return key_names[i].name;
	return NULL;
}

/* ------------------------------------------------------------------ */
/* LG button codes (lgmagic.py BUTTON_CODES, verbatim)                */
/* ------------------------------------------------------------------ */

static const struct {
	unsigned code;
	const char *name;
} lg_button_codes[] = {
	{ 0x0000, "None" },
	{ 0x8000, "POWER" },
	{ 0x8099, "POWER2" },
	{ 0x8010, "0" },
	{ 0x8011, "1" },
	{ 0x8012, "2" },
	{ 0x8013, "3" },
	{ 0x8014, "4" },
	{ 0x8015, "5" },
	{ 0x8016, "6" },
	{ 0x8017, "7" },
	{ 0x8018, "8" },
	{ 0x8019, "9" },
	{ 0x8044, "WHEEL_PRESS" },
	{ 0x8053, "LIST" },
	{ 0x8045, "..." },
	{ 0x8002, "VOL+" },
	{ 0x8003, "VOL-" },
	{ 0x8009, "MUTE" },
	{ 0x808B, "MIC" },
	{ 0x807C, "HOME" },
	{ 0x8043, "SETTINGS" },
	{ 0x8028, "BACK" },
	{ 0x80AB, "GUIDE" },
	{ 0x805D, "STREAMING" },
	{ 0x800B, "INPUT" },
	{ 0x8098, "STB MENU" },
	{ 0x8001, "CH-" },
	{ 0x8072, "RED" },
	{ 0x8071, "GREEN" },
	{ 0x8063, "YELLOW" },
	{ 0x8061, "BLUE" },
	{ 0x8081, "MOVIES" },
	{ 0x80B0, "PLAY" },
	{ 0x80BA, "PAUSE" },
	{ 0x8040, "UP" },
	{ 0x8041, "DOWN" },
	{ 0x8006, "RIGHT" },
	{ 0x8007, "LEFT" },
};

const char *keymap_lg_code_to_name(unsigned code)
{
	size_t i;

	for (i = 0; i < sizeof(lg_button_codes) / sizeof(lg_button_codes[0]); i++)
		if (lg_button_codes[i].code == code)
			return lg_button_codes[i].name;
	return NULL;
}

int keymap_lg_name_to_code(const char *name)
{
	size_t i;

	if (!name)
		return -1;
	for (i = 0; i < sizeof(lg_button_codes) / sizeof(lg_button_codes[0]); i++)
		if (strcmp(lg_button_codes[i].name, name) == 0)
			return (int)lg_button_codes[i].code;
	return -1;
}

/* ------------------------------------------------------------------ */
/* Kernel default decode (lg_btn_map in lgmagic_main.c)               */
/* ------------------------------------------------------------------ */

/* The kernel's lg_btn_map has two WHEEL_PRESS entries: KEY_ENTER and
 * BTN_LEFT.  The BTN_LEFT one only ever matches while the airmouse
 * mode is engaged (drvdata->mode); with raw_only=1 the mode never
 * engages, so the daemon always sees KEY_ENTER for the wheel press. */
static const struct keymap_default_entry default_map[] = {
	{ 0x8000, 116 },	/* POWER -> KEY_POWER */
	{ 0x8099, 142 },	/* POWER2 -> KEY_SLEEP */
	{ 0x8010, 11 }, { 0x8011, 2 }, { 0x8012, 3 },
	{ 0x8013, 4 }, { 0x8014, 5 }, { 0x8015, 6 },
	{ 0x8016, 7 }, { 0x8017, 8 }, { 0x8018, 9 }, { 0x8019, 10 },
	{ 0x8044, 28 },		/* WHEEL_PRESS -> KEY_ENTER (raw_only) */
	{ 0x8053, 395 },	/* LIST -> KEY_LIST */
	{ 0x8045, 139 },	/* ... -> KEY_MENU */
	{ 0x8002, 115 },	/* VOL+ -> KEY_VOLUMEUP */
	{ 0x8003, 114 },	/* VOL- -> KEY_VOLUMEDOWN */
	{ 0x8009, 113 },	/* MUTE -> KEY_MUTE */
	{ 0x808B, 582 },	/* MIC -> KEY_VOICECOMMAND */
	{ 0x807C, 102 },	/* HOME -> KEY_HOME */
	{ 0x8043, 141 },	/* SETTINGS -> KEY_SETUP */
	{ 0x8028, 158 },	/* BACK -> KEY_BACK */
	{ 0x80AB, 362 },	/* GUIDE -> KEY_PROGRAM */
	{ 0x805D, 226 },	/* STREAMING -> KEY_MEDIA */
	{ 0x800B, 377 },	/* INPUT -> KEY_TV */
	{ 0x8098, 438 },	/* STB MENU -> KEY_CONTEXT_MENU */
	{ 0x8001, 403 },	/* CH- -> KEY_CHANNELDOWN */
	{ 0x8072, 398 }, { 0x8071, 399 }, { 0x8063, 400 }, { 0x8061, 401 },
	{ 0x8081, 393 },	/* MOVIES -> KEY_VIDEO */
	{ 0x80B0, 207 },	/* PLAY -> KEY_PLAY */
	{ 0x80BA, 119 },	/* PAUSE -> KEY_PAUSE */
	{ 0x8040, 103 },	/* UP -> KEY_UP */
	{ 0x8041, 108 },	/* DOWN -> KEY_DOWN */
	{ 0x8006, 106 },	/* RIGHT -> KEY_RIGHT */
	{ 0x8007, 105 },	/* LEFT -> KEY_LEFT */
};

int keymap_default_keycode(unsigned lg_code)
{
	size_t i;

	for (i = 0; i < sizeof(default_map) / sizeof(default_map[0]); i++)
		if (default_map[i].lg_code == lg_code)
			return default_map[i].keycode;
	return -1;
}

const struct keymap_default_entry *keymap_default_map(size_t *n)
{
	if (n)
		*n = sizeof(default_map) / sizeof(default_map[0]);
	return default_map;
}
