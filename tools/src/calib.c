/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * calib.c - calibration JSON load/save, sample correction, blob conversion.
 *
 * Correction order matches scripts/display_imu.py exactly:
 *   a = R_align * M(a - b);  g = R_align * ((g - bias) * scale) * pi/180
 * i.e. the corrected gyro is already in rad/s.
 *
 * The JSON writer rounds to 6 decimal places first (Python did
 * json.dump(..., indent=4) on values already rounded via
 * f"{v:.6f}"), so the output shape matches the original scripts.
 */
#include "calib.h"

#include "json.h"
#include "matrix.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Sensor-to-body alignment from display_imu.py */
static const mat3 R_align = {
	{ { 0.0, -1.0, 0.0 },
	  { -1.0, 0.0, 0.0 },
	  { 0.0, 0.0, -1.0 } }
};

void calib_init_identity(struct calib *c)
{
	memset(c, 0, sizeof(*c));
	c->accel_matrix[0][0] = 1.0;
	c->accel_matrix[1][1] = 1.0;
	c->accel_matrix[2][2] = 1.0;
	c->gyro_scale[0] = 1.0;
	c->gyro_scale[1] = 1.0;
	c->gyro_scale[2] = 1.0;
}

int calib_load(const char *path, struct calib *c, char *err, size_t errsz)
{
	struct json_value *root, *accel, *gyro;
	const char *perr = NULL;
	size_t eoff = 0;

	calib_init_identity(c);
	root = json_load_file(path, &perr, &eoff);
	if (!root) {
		snprintf(err, errsz, "%s: %s (byte %zu)", path, perr, eoff);
		return -1;
	}
	if (root->type != JSON_OBJ) {
		snprintf(err, errsz, "%s: root is not a JSON object", path);
		json_free(root);
		return -1;
	}
	/* Missing keys keep the identity defaults - the original scripts
	 * crashed with KeyError instead, and a gyro-only calibration wrote
	 * empty accel arrays which broke --ahrs. */
	accel = json_obj_get(root, "accel");
	if (accel && accel->type == JSON_OBJ) {
		json_get_float3(accel, "bias", c->accel_bias);
		json_get_mat3(accel, "matrix", c->accel_matrix);
	}
	gyro = json_obj_get(root, "gyro");
	if (gyro && gyro->type == JSON_OBJ) {
		json_get_float3(gyro, "bias", c->gyro_bias);
		json_get_float3(gyro, "scale", c->gyro_scale);
	}
	json_free(root);
	return 0;
}

void calib_apply(const struct calib *c, const double a_raw[3],
		 const double g_raw[3], double a_out[3], double g_out[3])
{
	vec3 d = { a_raw[0] - c->accel_bias[0],
		   a_raw[1] - c->accel_bias[1],
		   a_raw[2] - c->accel_bias[2] };
	mat3 m = { { { c->accel_matrix[0][0], c->accel_matrix[0][1],
		       c->accel_matrix[0][2] },
		     { c->accel_matrix[1][0], c->accel_matrix[1][1],
		       c->accel_matrix[1][2] },
		     { c->accel_matrix[2][0], c->accel_matrix[2][1],
		       c->accel_matrix[2][2] } } };
	vec3 a = mat3_mul_vec3(&m, d);
	vec3 g = { (g_raw[0] - c->gyro_bias[0]) * c->gyro_scale[0],
		   (g_raw[1] - c->gyro_bias[1]) * c->gyro_scale[1],
		   (g_raw[2] - c->gyro_bias[2]) * c->gyro_scale[2] };

	a = mat3_mul_vec3(&R_align, a);
	g = mat3_mul_vec3(&R_align, g);
	g = vec3_scale(g, M_PI / 180.0);

	a_out[0] = a.x;
	a_out[1] = a.y;
	a_out[2] = a.z;
	g_out[0] = g.x;
	g_out[1] = g.y;
	g_out[2] = g.z;
}

/* Python: v = float(f"{v:.6f}") - round to 6 decimals before writing */
static double round6(double v)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "%.6f", v);
	return strtod(buf, NULL);
}

static struct json_value *arr3(double a, double b, double c)
{
	struct json_value *v = json_new(JSON_ARR);

	if (!v)
		return NULL;
	if (json_arr_add(v, json_new_num(round6(a))) < 0 ||
	    json_arr_add(v, json_new_num(round6(b))) < 0 ||
	    json_arr_add(v, json_new_num(round6(c))) < 0) {
		json_free(v);
		return NULL;
	}
	return v;
}

int calib_save_json(const struct calib *c, const char *path, char *err,
		    size_t errsz)
{
	struct json_value *root, *accel, *gyro, *mat;
	int accel_in_root = 0, gyro_in_root = 0, mat_in_accel = 0;
	char *text;
	FILE *f;
	int i, ok = 1;

	root = json_new(JSON_OBJ);
	accel = json_new(JSON_OBJ);
	gyro = json_new(JSON_OBJ);
	mat = json_new(JSON_ARR);
	if (!root || !accel || !gyro || !mat)
		goto oom;
	for (i = 0; i < 3; i++) {
		struct json_value *row = arr3(c->accel_matrix[i][0],
					      c->accel_matrix[i][1],
					      c->accel_matrix[i][2]);

		if (!row || json_arr_add(mat, row) < 0)
			goto oom;
	}
	/* Key order matches calibrate.py: accel{bias,matrix}, gyro{bias,scale}. */
	if (json_obj_add(accel, "bias",
			 arr3(c->accel_bias[0], c->accel_bias[1],
			      c->accel_bias[2])) < 0 ||
	    json_obj_add(accel, "matrix", mat) < 0)
		goto oom;
	mat_in_accel = 1;
	if (json_obj_add(gyro, "bias",
			 arr3(c->gyro_bias[0], c->gyro_bias[1],
			      c->gyro_bias[2])) < 0 ||
	    json_obj_add(gyro, "scale",
			 arr3(c->gyro_scale[0], c->gyro_scale[1],
			      c->gyro_scale[2])) < 0)
		goto oom;
	if (json_obj_add(root, "accel", accel) < 0)
		goto oom;
	accel_in_root = 1;
	if (json_obj_add(root, "gyro", gyro) < 0)
		goto oom;
	gyro_in_root = 1;

	text = json_dumps(root);
	/* By now every part is owned by root, so this frees the whole tree. */
	json_free(root);
	if (!text) {
		snprintf(err, errsz, "out of memory");
		return -1;
	}
	/* Python json.dump() writes no trailing newline - keep byte parity */
	f = fopen(path, "w");
	if (!f) {
		snprintf(err, errsz, "cannot open %s: %s", path,
			 strerror(errno));
		free(text);
		return -1;
	}
	ok = fputs(text, f) >= 0;
	free(text);
	if (!ok || fclose(f) != 0) {
		snprintf(err, errsz, "write error on %s", path);
		return -1;
	}
	return 0;

oom:
	/* At most one of accel/gyro can be owned by root here; the other
	 * (and mat, when not yet given to accel) is freed explicitly. */
	if (!mat_in_accel && mat)
		json_free(mat);
	if (!accel_in_root)
		json_free(accel);
	if (!gyro_in_root)
		json_free(gyro);
	json_free(root);
	snprintf(err, errsz, "out of memory");
	return -1;
}

void calib_to_blob(const struct calib *c, float alpha, float mouse_k,
		   struct lg_magic_airmouse_calib *blob)
{
	int i;

	for (i = 0; i < 3; i++) {
		blob->gyro_bias[i] = (float)c->gyro_bias[i];
		blob->gyro_scale[i] = (float)c->gyro_scale[i];
	}
	blob->alpha = alpha;
	blob->mouse_k = mouse_k;
}

int calib_validate_blob(const struct lg_magic_airmouse_calib *blob)
{
	int i, ret = 0;

	for (i = 0; i < 3; i++) {
		if (fabsf(blob->gyro_bias[i]) > 100.0f) {
			fprintf(stderr, "warning: gyro_bias[%d] out of range "
				"(%g)\n", i, (double)blob->gyro_bias[i]);
			ret = -1;
		}
		if (fabsf(blob->gyro_scale[i]) > 10.0f) {
			fprintf(stderr, "warning: gyro_scale[%d] out of range "
				"(%g)\n", i, (double)blob->gyro_scale[i]);
			ret = -1;
		}
	}
	if (blob->alpha < 0.0f || blob->alpha > 1.0f) {
		fprintf(stderr, "warning: alpha out of range (%g)\n",
			(double)blob->alpha);
		ret = -1;
	}
	if (blob->mouse_k < 0.0f || blob->mouse_k > 1.0f) {
		fprintf(stderr, "warning: mouse_k out of range (%g)\n",
			(double)blob->mouse_k);
		ret = -1;
	}
	return ret;
}
