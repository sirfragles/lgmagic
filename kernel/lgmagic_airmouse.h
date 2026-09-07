#ifndef LGMAGIC_AIRMOUSE_H
#define LGMAGIC_AIRMOUSE_H

/* Shared with the userspace tools (single source of truth for the blob). */
#include "../include/lgmagic_calib.h"

int lgmagic_calc_mouse(struct lgmagic_airmouse_calib *calib, float *gyro_acc, u16 threshold, s16 *gyro, s16 *mouse);
int lgmagic_validate_calib(struct lgmagic_airmouse_calib *calib);

#endif
