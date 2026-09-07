/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * lg_magic_calib.h - calibration blob layout shared between the kernel
 * module and the lg-magic userspace tools.
 *
 * This is the single source of truth for the 32-byte firmware blob
 * consumed by the lg_magic kernel driver (request_firmware()). The kernel
 * module and the userspace tools both include this file literally - do
 * not change the field order, and keep it free of kernel-only types.
 */
#ifndef LG_MAGIC_CALIB_H
#define LG_MAGIC_CALIB_H

/*
 * Native little-endian layout, 32 bytes, no padding (floats only):
 *   gyro_bias[3]   @  0  (3 * 4 = 12 bytes)
 *   gyro_scale[3]  @ 12  (3 * 4 = 12 bytes)
 *   alpha          @ 24
 *   mouse_k        @ 28
 */
struct lg_magic_airmouse_calib {
	float gyro_bias[3];
	float gyro_scale[3];
	float alpha;
	float mouse_k;
};

#endif /* LG_MAGIC_CALIB_H */
