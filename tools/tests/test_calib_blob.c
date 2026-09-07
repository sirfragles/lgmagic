/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * test_calib_blob.c - unit tests for the kernel calibration blob
 * conversion (calib.h + include/lgmagic_calib.h).
 *
 *  - the struct layout is compile-time checked: 32 bytes total with
 *    gyro_bias[3] @ 0, gyro_scale[3] @ 12, alpha @ 24, mouse_k @ 28
 *    (little-endian float bytes in the file);
 *  - loading ../testdata/ref_calib.json (a calibration written by the
 *    reference Python scripts) and converting with alpha = 0.2 and
 *    mouse_k = 0.5 produces a blob byte-identical to the golden
 *    ../testdata/ref_calib.bin (which is what the kernel firmware would
 *    consume);
 *  - loading ../testdata/ref_gyro.json (a gyro-only calibration with empty
 *    accel arrays - the Python bug case) keeps the identity accel defaults
 *    instead of failing or leaving NaN;
 *  - calib_validate_blob() applies the kernel's range checks.
 *
 * Fixtures are committed on purpose; a missing fixture is a test FAILURE.
 */
#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "test_util.h"
#include "calib.h"

/* Layout single source of truth: kernel and tools include this file
 * literally, so the checks are compile-time. */
_Static_assert(sizeof(struct lgmagic_airmouse_calib) == 32,
	       "calibration blob must be exactly 32 bytes");
_Static_assert(offsetof(struct lgmagic_airmouse_calib, gyro_bias[0]) == 0 &&
	       offsetof(struct lgmagic_airmouse_calib, gyro_scale[0]) == 12 &&
	       offsetof(struct lgmagic_airmouse_calib, alpha) == 24 &&
	       offsetof(struct lgmagic_airmouse_calib, mouse_k) == 28,
	       "calibration blob field offsets must be 0/12/24/28");
_Static_assert(sizeof(struct lgmagic_airmouse_calib) /
		       sizeof(float) == 8,
	       "calibration blob is eight floats");

static void test_blob_from_ref_calib(void)
{
	char jpath[4096], bpath[4096];
	struct calib c;
	struct lgmagic_airmouse_calib blob;
	char *golden;
	size_t glen = 0;
	char err[256];
	int rc;

	/* Golden data must exist - never invent it in the test. */
	if (tu_fixture_path(jpath, sizeof(jpath), "ref_calib.json") != 0 ||
	    tu_fixture_path(bpath, sizeof(bpath), "ref_calib.bin") != 0) {
		CHECK(0, "fixtures ref_calib.json and ref_calib.bin exist "
		      "(fixtures must be committed)");
		return;
	}
	rc = calib_load(jpath, &c, err, sizeof(err));
	CHECK(rc == 0, "ref_calib.json loads");
	if (rc != 0) {
		printf("  calib_load error: %s\n", err);
		return;
	}
	CHECK(c.gyro_bias[0] == 0.5 && c.gyro_bias[1] == -0.25 &&
	      c.gyro_bias[2] == 0.125,
	      "ref_calib.json gyro bias parsed");
	CHECK(c.gyro_scale[0] == 0.9 && c.gyro_scale[1] == 1.1 &&
	      c.gyro_scale[2] == 1.0,
	      "ref_calib.json gyro scale parsed");
	CHECK(c.accel_bias[0] == 0.1 && c.accel_bias[1] == -0.2 &&
	      c.accel_bias[2] == 0.3,
	      "ref_calib.json accel bias parsed");

	calib_to_blob(&c, 0.2f, 0.5f, &blob);
	CHECK(blob.gyro_bias[0] == 0.5f && blob.gyro_bias[1] == -0.25f &&
	      blob.gyro_bias[2] == 0.125f &&
	      blob.gyro_scale[0] == 0.9f && blob.gyro_scale[1] == 1.1f &&
	      blob.gyro_scale[2] == 1.0f && blob.alpha == 0.2f &&
	      blob.mouse_k == 0.5f,
	      "blob fields hold the calibration values (float)");

	golden = tu_read_file(bpath, &glen);
	CHECK(golden != NULL, "ref_calib.bin is readable");
	if (!golden)
		return;
	CHECK(glen == 32,
	      "ref_calib.bin is 32 bytes");
	if (glen == 32) {
		int same = memcmp(&blob, golden, 32) == 0;

		CHECK(same, "calib_to_blob output is byte-identical to "
		      "ref_calib.bin");
		if (!same) {
			size_t i;

			printf("  got:  ");
			for (i = 0; i < 32; i++)
				printf("%02x", ((unsigned char *)&blob)[i]);
			printf("\n  want: ");
			for (i = 0; i < 32; i++)
				printf("%02x", (unsigned char)golden[i]);
			printf("\n");
		}
	}
	free(golden);
}

static void test_ref_gyro_defaults(void)
{
	char jpath[4096];
	struct calib c;
	char err[256];
	int rc;

	if (tu_fixture_path(jpath, sizeof(jpath), "ref_gyro.json") != 0) {
		CHECK(0, "fixture ref_gyro.json exists "
		      "(fixtures must be committed)");
		return;
	}
	rc = calib_load(jpath, &c, err, sizeof(err));
	CHECK(rc == 0, "ref_gyro.json loads (gyro-only calibration)");
	if (rc != 0)
		return;
	CHECK(c.accel_matrix[0][0] == 1.0 && c.accel_matrix[1][1] == 1.0 &&
	      c.accel_matrix[2][2] == 1.0 &&
	      c.accel_bias[0] == 0.0 && c.accel_bias[1] == 0.0 &&
	      c.accel_bias[2] == 0.0,
	      "empty accel arrays keep the identity defaults (no NaN, no "
	      "crash)");
	CHECK(c.gyro_bias[0] == 1.5115 && c.gyro_bias[1] == -2.199 &&
	      c.gyro_bias[2] == 0.798 && c.gyro_scale[0] == 1.0 &&
	      c.gyro_scale[1] == 1.0 && c.gyro_scale[2] == 1.0,
	      "gyro bias/scale parsed from the gyro-only file");
}

static void test_validate(void)
{
	struct lgmagic_airmouse_calib blob = {
		.gyro_bias = { 0.0f, 0.0f, 0.0f },
		.gyro_scale = { 1.0f, 1.0f, 1.0f },
		.alpha = 0.2f,
		.mouse_k = 0.5f
	};

	CHECK(calib_validate_blob(&blob) == 0,
	      "a valid blob passes validation");

	blob.gyro_bias[0] = 100.0f;
	CHECK(calib_validate_blob(&blob) == 0,
	      "|gyro_bias| == 100 is still valid (kernel rejects only > 100)");
	blob.gyro_bias[0] = 100.5f;
	CHECK(calib_validate_blob(&blob) == -1,
	      "|gyro_bias| > 100 is rejected");
	blob.gyro_bias[0] = 0.0f;

	blob.gyro_scale[1] = 10.0f;
	CHECK(calib_validate_blob(&blob) == 0,
	      "|gyro_scale| == 10 is still valid (kernel rejects only > 10)");
	blob.gyro_scale[1] = -10.5f;
	CHECK(calib_validate_blob(&blob) == -1,
	      "|gyro_scale| > 10 is rejected");
	blob.gyro_scale[1] = 1.0f;

	blob.alpha = -0.01f;
	CHECK(calib_validate_blob(&blob) == -1, "alpha < 0 is rejected");
	blob.alpha = 0.0f;
	CHECK(calib_validate_blob(&blob) == 0, "alpha == 0 is valid");
	blob.alpha = 1.0f;
	CHECK(calib_validate_blob(&blob) == 0, "alpha == 1 is valid");
	blob.alpha = 1.01f;
	CHECK(calib_validate_blob(&blob) == -1, "alpha > 1 is rejected");
	blob.alpha = 0.2f;

	blob.mouse_k = -0.1f;
	CHECK(calib_validate_blob(&blob) == -1, "mouse_k < 0 is rejected");
	blob.mouse_k = 0.0f;
	CHECK(calib_validate_blob(&blob) == 0, "mouse_k == 0 is valid");
	blob.mouse_k = 1.0f;
	CHECK(calib_validate_blob(&blob) == 0, "mouse_k == 1 is valid");
	blob.mouse_k = 1.1f;
	CHECK(calib_validate_blob(&blob) == -1, "mouse_k > 1 is rejected");
}

static void test_save_json_roundtrip(void)
{
	/* calib_save_json writes what calib_load reads back: exercised with
	 * the golden calibration through a temp file. */
	char jpath[4096], tpath[4096];
	struct calib c, c2;
	char err[256];
	int rc, ok;
	int i, j;

	if (tu_fixture_path(jpath, sizeof(jpath), "ref_calib.json") != 0) {
		CHECK(0, "fixture ref_calib.json exists");
		return;
	}
	rc = calib_load(jpath, &c, err, sizeof(err));
	if (rc != 0) {
		CHECK(0, "ref_calib.json loads for the save/load round trip");
		return;
	}
	if (tu_temp_path(tpath, sizeof(tpath), "calibjson") != 0) {
		CHECK(0, "can create a temp file for the save test");
		return;
	}
	rc = calib_save_json(&c, tpath, err, sizeof(err));
	CHECK(rc == 0, "calib_save_json writes the calibration");
	if (rc != 0) {
		remove(tpath);
		return;
	}
	rc = calib_load(tpath, &c2, err, sizeof(err));
	CHECK(rc == 0, "saved calibration loads back");
	ok = 1;
	if (rc == 0) {
		for (i = 0; i < 3; i++) {
			if (c2.gyro_bias[i] != c.gyro_bias[i] ||
			    c2.gyro_scale[i] != c.gyro_scale[i] ||
			    c2.accel_bias[i] != c.accel_bias[i])
				ok = 0;
			for (j = 0; j < 3; j++)
				if (c2.accel_matrix[i][j] != c.accel_matrix[i][j])
					ok = 0;
		}
	}
	CHECK(ok == 1, "save -> load round trip preserves all values "
	      "(6-decimal JSON)");
	remove(tpath);
}

int main(void)
{
	TEST_BEGIN();
	test_blob_from_ref_calib();
	test_ref_gyro_defaults();
	test_validate();
	test_save_json_roundtrip();
	TEST_SUMMARY("test_calib_blob");
	return tu_fail_count ? 1 : 0;
}
