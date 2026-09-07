/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * pairing.c - pairing the LG keyboard evdev with its IMU evdev (portable).
 * See pairing.h for the rules.
 */
#include "pairing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LG_VID	0x000f
#define LG_PID	0x3412
#define KBD_NAME "LG Magic Remote"
#define IMU_NAME "LG Magic Remote IMU"

int pairing_is_keyboard(const struct pairing_input_dev *d)
{
	return d && d->name && strcmp(d->name, KBD_NAME) == 0 &&
	       d->vendor == LG_VID && d->product == LG_PID;
}

int pairing_is_imu(const struct pairing_input_dev *d)
{
	return d && d->name && strcmp(d->name, IMU_NAME) == 0;
}

/* MAC comparison, case-insensitive, "" never matches "". */
static int uniq_eq(const char *a, const char *b)
{
	if (!a)
		a = "";
	if (!b)
		b = "";
	for (; *a && *b; a++, b++) {
		char ca = *a, cb = *b;

		if (ca >= 'A' && ca <= 'Z')
			ca += 'a' - 'A';
		if (cb >= 'A' && cb <= 'Z')
			cb += 'a' - 'A';
		if (ca != cb)
			return 0;
	}
	return *a == *b;
}

int pairing_match(const struct pairing_input_dev *devs, size_t ndevs,
		  struct paired_remote *out, size_t outsz, size_t *n_out,
		  char *err, size_t errsz)
{
	size_t *kbd_idx = NULL, nkbd = 0;
	int *imu_of_kbd = NULL;
	size_t i, emitted = 0;
	int ret = 0;

	if (n_out)
		*n_out = 0;
	for (i = 0; i < ndevs; i++) {
		if (pairing_is_keyboard(&devs[i])) {
			size_t *p = realloc(kbd_idx, (nkbd + 1) * sizeof(*p));

			if (!p) {
				snprintf(err, errsz, "out of memory");
				ret = -1;
				goto done;
			}
			kbd_idx = p;
			kbd_idx[nkbd++] = i;
		}
	}
	if (nkbd > 0) {
		imu_of_kbd = malloc(nkbd * sizeof(*imu_of_kbd));
		if (!imu_of_kbd) {
			snprintf(err, errsz, "out of memory");
			ret = -1;
			goto done;
		}
		for (i = 0; i < nkbd; i++)
			imu_of_kbd[i] = -1;

		for (i = 0; i < ndevs; i++) {
			const struct pairing_input_dev *imu = &devs[i];
			size_t match = (size_t)-1, nmatch = 0;
			size_t free_kbd = (size_t)-1, nfree = 0;
			size_t k;

			if (!pairing_is_imu(imu))
				continue;
			for (k = 0; k < nkbd; k++) {
				if (imu_of_kbd[k] >= 0)
					continue;	/* already paired */
				nfree++;
				free_kbd = k;
				if (imu->uniq && imu->uniq[0] &&
				    uniq_eq(imu->uniq, devs[kbd_idx[k]].uniq)) {
					nmatch++;
					match = k;
				}
			}
			if (nmatch > 1) {
				snprintf(err, errsz, "ambiguous IMU pairing: "
					 "MAC '%s' matches %zu keyboards",
					 imu->uniq, nmatch);
				ret = -1;
				goto done;
			}
			if (nmatch == 1) {
				imu_of_kbd[match] = (int)i;
				continue;
			}
			/* No MAC match: the fallback needs exactly one
			 * keyboard candidate, anything else is a guess. */
			if (nfree == 1) {
				imu_of_kbd[free_kbd] = (int)i;
				continue;
			}
			if (nfree == 0)
				continue;	/* spare IMU node, ignore */
			snprintf(err, errsz, "ambiguous IMU pairing: %zu "
				 "keyboard candidates, no MAC match", nfree);
			ret = -1;
			goto done;
		}
	}
	/* Emit one remote per keyboard, in discovery order. */
	for (i = 0; i < nkbd && emitted < outsz; i++) {
		struct paired_remote *r = &out[emitted];
		int imu_i = imu_of_kbd ? imu_of_kbd[i] : -1;
		const char *u = devs[kbd_idx[i]].uniq;

		memset(r, 0, sizeof(*r));
		r->keyboard = &devs[kbd_idx[i]];
		if (imu_i >= 0)
			r->imu = &devs[imu_i];
		if (!u || !u[0]) {
			if (imu_i >= 0 && devs[imu_i].uniq && devs[imu_i].uniq[0])
				u = devs[imu_i].uniq;
			else
				u = "";
		}
		snprintf(r->uniq, sizeof(r->uniq), "%s", u);
		emitted++;
	}
	if (n_out)
		*n_out = emitted;
done:
	free(kbd_idx);
	free(imu_of_kbd);
	return ret;
}
