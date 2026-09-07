#ifndef LG_MAGIC_AIRMOUSE_H
#define LG_MAGIC_AIRMOUSE_H

/* Shared with the userspace tools (single source of truth for the blob). */
#include "../include/lg_magic_calib.h"

int lgmagic_calc_mouse(struct lg_magic_airmouse_calib *calib, float *gyro_acc, u16 threshold, s16 *gyro, s16 *mouse);
int lgmagic_validate_calib(struct lg_magic_airmouse_calib *calib);

#endif
