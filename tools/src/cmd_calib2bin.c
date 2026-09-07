/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cmd_calib2bin.c - `lgmagic calib2bin` subcommand (scripts/convert_calib.py).
 *
 *   lgmagic calib2bin JSON OUTPUT_BIN [--alpha F] [--mouse_k F]
 *
 * Converts the calibration JSON to the 32-byte little-endian firmware blob
 * (struct lgmagic_airmouse_calib: gyro_bias[3], gyro_scale[3], alpha,
 * mouse_k). alpha/mouse_k default to the configuration values (0.2/0.5).
 * The range checks of the kernel are mirrored as warnings; the blob is
 * still written (the kernel rejects it the same way).
 */
#include "calib.h"
#include "config.h"
#include "json.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out)
{
	fputs("Usage: lgmagic calib2bin JSON OUTPUT_BIN [--alpha F] [--mouse_k F]\n"
	      "\n"
	      "  JSON       calibration file (gyro.bias and gyro.scale, 3 values each)\n"
	      "  OUTPUT_BIN 32-byte firmware blob for /lib/firmware/\n"
	      "\n"
	      "  --alpha F     low-pass filter coefficient (default: config alpha)\n"
	      "  --mouse_k F   airmouse sensitivity (default: config mouse_k)\n", out);
}

/* Python convert_calib.py: "bias and scale must each have exactly 3 elements." */
static int check_len3(struct json_value *arr)
{
	size_t i;

	if (!arr || arr->type != JSON_ARR || arr->count != 3)
		return -1;
	for (i = 0; i < 3; i++)
		if (arr->items[i]->type != JSON_NUM)
			return -1;
	return 0;
}

int cmd_calib2bin(int argc, char **argv)
{
	const char *json_path, *out_path;
	double alpha = g_cfg->alpha, mouse_k = g_cfg->mouse_k;
	struct json_value *root, *gyro;
	const char *err = NULL;
	size_t eoff = 0;
	struct calib c;
	struct lgmagic_airmouse_calib blob;
	char cerr[256];
	FILE *f;
	int i;

	/* argv[0] is the subcommand name; flags may come before or after
	 * the two positional arguments. */
	if (argc >= 2 && (strcmp(argv[1], "--help") == 0 ||
			  strcmp(argv[1], "-h") == 0)) {
		usage(stdout);
		return 0;
	}
	{
		const char *pos[2];
		int npos = 0;

		for (i = 1; i < argc; i++) {
			if (strcmp(argv[i], "--alpha") == 0 && i + 1 < argc)
				alpha = strtod(argv[++i], NULL);
			else if (strncmp(argv[i], "--alpha=", 8) == 0)
				alpha = strtod(argv[i] + 8, NULL);
			else if (strcmp(argv[i], "--mouse_k") == 0 && i + 1 < argc)
				mouse_k = strtod(argv[++i], NULL);
			else if (strncmp(argv[i], "--mouse_k=", 10) == 0)
				mouse_k = strtod(argv[i] + 10, NULL);
			else if (npos < 2)
				pos[npos++] = argv[i];
			else {
				fprintf(stderr, "lgmagic calib2bin: unexpected "
					"argument '%s'\n", argv[i]);
				usage(stderr);
				return 1;
			}
		}
		if (npos != 2) {
			usage(stderr);
			return 1;
		}
		json_path = pos[0];
		out_path = pos[1];
	}

	/* Strict length check first, with the original Python message. */
	root = json_load_file(json_path, &err, &eoff);
	if (!root) {
		fprintf(stderr, "Error reading JSON: %s (byte %zu)\n", err, eoff);
		return 1;
	}
	gyro = root->type == JSON_OBJ ? json_obj_get(root, "gyro") : NULL;
	if (check_len3(gyro ? json_obj_get(gyro, "bias") : NULL) < 0 ||
	    check_len3(gyro ? json_obj_get(gyro, "scale") : NULL) < 0) {
		fprintf(stderr, "Error: bias and scale must each have exactly "
			"3 elements.\n");
		json_free(root);
		return 1;
	}
	json_free(root);

	if (calib_load(json_path, &c, cerr, sizeof(cerr)) < 0) {
		fprintf(stderr, "Error reading JSON: %s\n", cerr);
		return 1;
	}
	calib_to_blob(&c, (float)alpha, (float)mouse_k, &blob);
	calib_validate_blob(&blob);

	f = fopen(out_path, "wb");
	if (!f) {
		fprintf(stderr, "lgmagic: cannot open %s: %s\n", out_path,
			strerror(errno));
		return 1;
	}
	if (fwrite(&blob, 1, sizeof(blob), f) != sizeof(blob) ||
	    fclose(f) != 0) {
		fprintf(stderr, "lgmagic: write error on %s\n", out_path);
		return 1;
	}
	printf("Saved struct to %s\n", out_path);
	return 0;
}
