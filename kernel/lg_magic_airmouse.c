/*  This is part of lg_magic_dkms

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <linux/types.h>
#include "lg_magic_airmouse.h"

static inline float lgmagic_fabs(float val)
{
	if (val<0)
		return -val;
	return val;
}

int lgmagic_validate_calib(struct lg_magic_airmouse_calib *calib)
{
	for (size_t i = 0; i < 3; i++)
	{
		if (lgmagic_fabs(calib->gyro_bias[i])>100.0 || lgmagic_fabs(calib->gyro_scale[i])>10.0)
			return 1;
	}
	if (calib->alpha<0.0 || calib->alpha>1.0)
			return 1;

	if (calib->mouse_k<0.0 || calib->mouse_k>1.0)
		return 1;

	return 0;
}

static inline void lgmagic_lpf(float *acc, float alpha, float value)
{
	*acc = alpha * value + (1.0 - alpha) * (*acc);
}

int lgmagic_calc_mouse(struct lg_magic_airmouse_calib *calib, float *gyro_acc, u16 threshold, s16 *gyro, s16 *mouse)
{
	for (size_t i = 0; i < 3; i++)
	{
		float gyro_corr = ((float)gyro[i] - calib->gyro_bias[i]) * calib->gyro_scale[i];
		gyro_acc[i] = calib->alpha * gyro_corr + (1.0 - calib->alpha) * gyro_acc[i];
	}

	mouse[0] = gyro_acc[2] * calib->mouse_k;
	mouse[1] = gyro_acc[0] * calib->mouse_k;

	if (lgmagic_fabs(gyro_acc[0])>threshold || lgmagic_fabs(gyro_acc[2])>threshold)
		return 1;
	return 0;
}
