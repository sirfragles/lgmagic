/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * keymap.h - keycode names and the LG button decode map (portable).
 *
 * Two tables:
 *  - Linux uapi keycode names ("KEY_ENTER", "BTN_LEFT", ...) with their
 *    numeric values hardcoded (the input ABI is frozen, and this file
 *    builds on macOS where linux/input.h does not exist).  The table is
 *    curated: every code the kernel module can emit, plus common
 *    mapping targets (mouse buttons, media keys).
 *  - LG button codes with their lgmagic.py names ("WHEEL_PRESS",
 *    "VOL+", ...) - verbatim from hidraw.c button_codes - plus the
 *    kernel's default decode (lg_btn_map in lgmagic_main.c, with the
 *    raw_only semantics: the WHEEL button reports KEY_ENTER because the
 *    BTN_LEFT branch only ever engaged in the airmouse mode).
 */
#ifndef LG_TOOLS_KEYMAP_H
#define LG_TOOLS_KEYMAP_H

#include <stddef.h>

/* "KEY_ENTER"/"BTN_LEFT" -> code; -1 when unknown. */
int keymap_name_to_code(const char *name);

/* code -> "KEY_ENTER"; NULL when the code is not in the table. */
const char *keymap_code_to_name(int code);

/* LG button code (0x8044) -> name ("WHEEL_PRESS"); NULL when unknown. */
const char *keymap_lg_code_to_name(unsigned code);

/* LG button name -> code; -1 when unknown. */
int keymap_lg_name_to_code(const char *name);

/* The kernel's default decode of an LG button code under raw_only=1
 * (lg_btn_map first match); -1 when the code is not mapped. */
int keymap_default_keycode(unsigned lg_code);

/* The full default map, for `button list` and the parity test. */
struct keymap_default_entry {
	unsigned lg_code;
	int keycode;
};
const struct keymap_default_entry *keymap_default_map(size_t *n);

#endif /* LG_TOOLS_KEYMAP_H */
