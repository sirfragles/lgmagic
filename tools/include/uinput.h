/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * uinput.h - virtual input devices via /dev/uinput (Linux only).
 *
 * The v2 daemon creates one pair per discovered remote, named after
 * its identity:
 *   - "lgmagicd keyboard <MAC>": the full EV_KEY set + REL_WHEEL etc.,
 *   - "lgmagicd mouse <MAC>":    REL_X/REL_Y/REL_WHEEL_HI_RES + buttons.
 * The v1 `lgmagic imu --mouse` behaviour is kept bit-for-bit through
 * the uinput_open()/uinput_move() wrappers (same name, ids and event
 * set as v1).
 */
#ifndef LG_TOOLS_UINPUT_H
#define LG_TOOLS_UINPUT_H

#include <stddef.h>
#include <linux/input.h>
#include <linux/uinput.h>

/* Bit arrays sized for the kernel's key/rel/abs tables. */
#define UINPUT_KEYBITS ((KEY_CNT + 7) / 8)
#define UINPUT_RELBITS ((REL_CNT + 7) / 8)
#define UINPUT_ABSBITS ((ABS_CNT + 7) / 8)

/* Everything uinput_create needs: the device identity and its event
 * capability bits.  The corresponding EV_* event types are derived
 * automatically (a non-empty keybits enables EV_KEY etc.). */
struct uinput_spec {
	const char *name;
	unsigned short bustype;
	unsigned vendor;
	unsigned product;
	unsigned char keybits[UINPUT_KEYBITS];
	unsigned char relbits[UINPUT_RELBITS];
	unsigned char absbits[UINPUT_ABSBITS];
};

void uinput_spec_init(struct uinput_spec *s, const char *name);
void uinput_spec_key(struct uinput_spec *s, unsigned code);
void uinput_spec_rel(struct uinput_spec *s, unsigned code);
void uinput_spec_abs(struct uinput_spec *s, unsigned code);

/* Create the device described by spec.  Tries /dev/uinput first, then
 * /dev/input/uinput.  Returns the fd or -1 with err (ENOENT ->
 * "modprobe uinput", EACCES -> root/input group). */
int uinput_create(const struct uinput_spec *spec, char *err, size_t errsz);
void uinput_close(int fd);

/* Emit one EV_KEY event (value 0/1/2) + SYN_REPORT.  Returns 0 or -1. */
int uinput_key(int fd, int code, int value);

/* Emit a wheel scroll: EV_REL REL_WHEEL (clicks) + REL_WHEEL_HI_RES
 * (units, 120 per click) + SYN_REPORT.  Zero parts are skipped; all-zero
 * emits only the SYN.  Returns 0 or -1. */
int uinput_scroll(int fd, int wheel, int hi_res);

/* Emit a relative move (REL_X/REL_Y) + SYN_REPORT.  Returns 0 or -1. */
int uinput_move(int fd, int dx, int dy);

/* v1 compatibility: the `imu --mouse` virtual mouse ("lgmagic mouse",
 * REL_X/REL_Y + BTN_LEFT/BTN_RIGHT, BUS_USB 0x1:0x1 - unchanged from
 * v1).  Returns the fd or -1 with err. */
int uinput_open(char *err, size_t errsz);

#endif /* LG_TOOLS_UINPUT_H */
