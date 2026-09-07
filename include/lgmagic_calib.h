/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * lgmagic_calib.h - calibration blob layout shared between the kernel
 * module and the lgmagic userspace tools.
 *
 * This is the single source of truth for the 32-byte firmware blob
 * consumed by the lgmagic kernel driver (request_firmware()). The kernel
 * module and the userspace tools both include this file literally - do
 * not change the field order, and keep it free of kernel-only types.
 */
#ifndef LGMAGIC_CALIB_H
#define LGMAGIC_CALIB_H

/*
 * Native little-endian layout, 32 bytes, no padding (floats only):
 *   gyro_bias[3]   @  0  (3 * 4 = 12 bytes)
 *   gyro_scale[3]  @ 12  (3 * 4 = 12 bytes)
 *   alpha          @ 24
 *   mouse_k        @ 28
 */
struct lgmagic_airmouse_calib {
	float gyro_bias[3];
	float gyro_scale[3];
	float alpha;
	float mouse_k;
};

#endif /* LGMAGIC_CALIB_H */
