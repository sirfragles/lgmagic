/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * pairing.h - pairing the LG keyboard evdev with its IMU evdev (portable).
 *
 * The kernel module creates two input devices per remote: the keyboard
 * "LG Magic Remote" (EV_KEY + EV_REL) and the IMU "LG Magic Remote IMU"
 * (EV_ABS + EV_MSC).  They are paired back together by the Bluetooth MAC
 * (sysfs uniq); when the MAC is unavailable there is a fallback for the
 * single-remote case.  A genuinely ambiguous set is an error - the daemon
 * refuses to guess.
 *
 * This is pure matching over structs so it is testable on macOS.
 */
#ifndef LG_TOOLS_PAIRING_H
#define LG_TOOLS_PAIRING_H

#include <stddef.h>

/* One evdev device as seen by the daemon's /dev/input scan.  The
 * strings are borrowed (owned by the caller). */
struct pairing_input_dev {
	const char *path;	/* /dev/input/eventN */
	const char *name;	/* evdev name (EVIOCGNAME) */
	const char *uniq;	/* sysfs uniq, "" when absent */
	unsigned vendor;	/* id.vendor */
	unsigned product;	/* id.product */
};

/* One remote: its keyboard plus the paired IMU (NULL when the IMU is
 * missing or could not be identified). */
struct paired_remote {
	char uniq[64];			/* identity MAC, "" when unknown */
	const struct pairing_input_dev *keyboard;
	const struct pairing_input_dev *imu;
};

/* Device classification: name + id for the keyboard ("LG Magic Remote",
 * 000f:3412), name for the IMU ("LG Magic Remote IMU"). */
int pairing_is_keyboard(const struct pairing_input_dev *d);
int pairing_is_imu(const struct pairing_input_dev *d);

/* Group keyboards and IMUs into remotes, one entry per keyboard in input
 * order.  Pairing rules: an IMU matches the keyboard with the same
 * non-empty uniq (case-insensitive); without a MAC match, and only then,
 * an IMU pairs with the sole keyboard candidate.  Anything ambiguous
 * (several keyboards, no MAC to tell them apart) is an error.
 *
 * Returns 0 and fills out[0..outsz) (n_out receives the count); -1 with
 * err set on ambiguity or allocation failure. */
int pairing_match(const struct pairing_input_dev *devs, size_t ndevs,
		  struct paired_remote *out, size_t outsz, size_t *n_out,
		  char *err, size_t errsz);

#endif /* LG_TOOLS_PAIRING_H */
