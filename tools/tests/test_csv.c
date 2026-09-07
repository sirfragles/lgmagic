/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * test_csv.c - unit tests for csv.h (IMU sample CSV reader/writer).
 *
 * The writer is byte-compatible with scripts/display_imu.py, i.e. with
 * Python's csv module: CRLF line endings, the header
 * "counter,dt,ax,ay,az,gx,gy,gz", an empty dt field for the first frame
 * (Python wrote None there) and numbers rendered as Python repr does for
 * these values ("%.9g" gives the same bytes).
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "test_util.h"
#include "csv.h"

static const char EXPECTED_BYTES[] =
	"counter,dt,ax,ay,az,gx,gy,gz\r\n"
	"1,,10,11,12,20,21,22\r\n"
	"2,0.02,13,14,15,23,24,25\r\n"
	"3,0.04,16,17,18,26,27,28\r\n";

static void test_writer_byte_parity(void)
{
	struct imu_sample samples[3];
	char path[4096];
	char *bytes;
	size_t n = 0;
	char err[256];
	int rc;
	size_t i;

	samples[0].counter = 1;
	samples[0].dt = NAN;	/* first frame: Python wrote None */
	samples[0].accel[0] = 10.0; samples[0].accel[1] = 11.0;
	samples[0].accel[2] = 12.0;
	samples[0].gyro[0] = 20.0; samples[0].gyro[1] = 21.0;
	samples[0].gyro[2] = 22.0;

	samples[1].counter = 2;
	samples[1].dt = 0.02;
	samples[1].accel[0] = 13.0; samples[1].accel[1] = 14.0;
	samples[1].accel[2] = 15.0;
	samples[1].gyro[0] = 23.0; samples[1].gyro[1] = 24.0;
	samples[1].gyro[2] = 25.0;

	samples[2].counter = 3;
	samples[2].dt = 0.04;
	samples[2].accel[0] = 16.0; samples[2].accel[1] = 17.0;
	samples[2].accel[2] = 18.0;
	samples[2].gyro[0] = 26.0; samples[2].gyro[1] = 27.0;
	samples[2].gyro[2] = 28.0;

	if (tu_temp_path(path, sizeof(path), "csv") != 0) {
		CHECK(0, "can create a temp file for the writer test");
		return;
	}
	rc = csv_write(path, samples, 3, err, sizeof(err));
	CHECK(rc == 0, "csv_write succeeds");
	if (rc != 0)
		printf("  csv_write error: %s\n", err);

	bytes = tu_read_file(path, &n);
	CHECK(bytes != NULL, "written file can be read back");
	if (!bytes) {
		remove(path);
		return;
	}
	if (n == strlen(EXPECTED_BYTES) &&
	    memcmp(bytes, EXPECTED_BYTES, n) == 0) {
		CHECK(1, "writer output is byte-identical to the "
		      "display_imu.py/Python csv format");
	} else {
		CHECK(0, "writer output is byte-identical to the "
		      "display_imu.py/Python csv format");
		printf("  got %zu bytes, want %zu:\n", n,
		       strlen(EXPECTED_BYTES));
		for (i = 0; i < n; i++)
			printf("%02x ", (unsigned char)bytes[i]);
		printf("\n--- got text ---\n%.*s\n--- want ---\n%s\n",
		       (int)n, bytes, EXPECTED_BYTES);
	}
	free(bytes);
	remove(path);
}

static void test_write_read_roundtrip(void)
{
	static const double dtv[8] = { NAN, 0.02, 0.125, 0.03, 0.04, 0.05,
				       0.06, 0.07 };
	struct imu_sample in[8], *out = NULL;
	char path[4096];
	char err[256];
	long n;
	int i, rc;
	int ok;

	/* The writer emits raw counts with "%.0f" (Python wrote ints) and dt
	 * with "%.9g" (Python's repr).  All values are chosen so the written
	 * form parses back to the identical double: integer payloads, and dt
	 * values that are exact in binary or have a short exact decimal form
	 * (0.03, 0.02, ...). */
	if (tu_temp_path(path, sizeof(path), "csvrt") != 0) {
		CHECK(0, "can create a temp file for the round-trip test");
		return;
	}
	for (i = 0; i < 8; i++) {
		in[i].counter = (unsigned)(i + 1);
		in[i].dt = dtv[i];
		in[i].accel[0] = 10.0 + i;
		in[i].accel[1] = -(5.0 * i + 3.0);
		in[i].accel[2] = 13.0 + 8.0 * i;
		in[i].gyro[0] = 100.0 + 3.0 * i;
		in[i].gyro[1] = -(2.0 * i + 1.0);
		in[i].gyro[2] = 7.0 + 5.0 * i;
	}
	rc = csv_write(path, in, 8, err, sizeof(err));
	CHECK(rc == 0, "csv_write succeeds for the round-trip data");
	if (rc != 0) {
		remove(path);
		return;
	}
	n = csv_read(path, &out, err, sizeof(err));
	CHECK(n == 8, "csv_read returns all 8 written samples");
	ok = 1;
	if (n == 8) {
		for (i = 0; i < 8; i++) {
			int j;
			if (out[i].counter != in[i].counter)
				ok = 0;
			if (i == 0) {
				if (!isnan(out[i].dt))
					ok = 0;
			} else if (out[i].dt != in[i].dt) {
				ok = 0;
			}
			for (j = 0; j < 3; j++) {
				if (out[i].accel[j] != in[i].accel[j])
					ok = 0;
				if (out[i].gyro[j] != in[i].gyro[j])
					ok = 0;
			}
		}
	}
	CHECK(ok == 1,
	      "write->read round trip preserves every sample field "
	      "(empty dt reads back as NAN)");
	free(out);
	remove(path);
}

static void test_reader_robustness(void)
{
	const char *crafted =
		"counter,dt,ax,ay,az,gx,gy,gz\r\n"
		"1,,10,11,12,20,21,22\r\n"
		"2,0.02,13,14,15,23,24\r\n"		/* 7 fields: skipped */
		"3,x,16,17,18,26,27,28\r\n"		/* non-numeric dt */
		"this is not numeric at all\r\n"	/* garbage row */
		"4,0.04,1.5,-2.5,3.5,4.5,-5.5,6.5,99\r\n" /* 9 fields: valid */
		"5,9.5e-3,19,20,21,29,30,31\r\n"
		"5\r\n";				/* short row */
	char path[4096];
	struct imu_sample *out = NULL;
	char err[256];
	long n;

	if (tu_temp_path(path, sizeof(path), "csvrob") != 0) {
		CHECK(0, "can create a temp file for the robustness test");
		return;
	}
	if (tu_write_file(path, crafted, strlen(crafted)) != 0) {
		CHECK(0, "can write the crafted CSV file");
		remove(path);
		return;
	}
	n = csv_read(path, &out, err, sizeof(err));
	CHECK(n == 3, "reader skips short rows, garbage and non-numeric "
	      "fields (3 valid rows of 7)");
	if (n >= 1) {
		CHECK(out[0].counter == 1 && isnan(out[0].dt) &&
		      out[0].accel[0] == 10.0 && out[0].gyro[2] == 22.0,
		      "first valid row: empty dt field reads back as NAN");
	}
	if (n >= 3) {
		CHECK(out[2].counter == 5 && out[2].dt == 9.5e-3 &&
		      out[2].accel[2] == 21.0,
		      "rows with exponent numbers and trailing fields "
		      "parse correctly");
	}
	free(out);
	remove(path);
}

static void test_read_errors(void)
{
	char err[256];
	struct imu_sample *out = NULL;
	long n;

	n = csv_read("/nonexistent/lg-magic-no-such.csv", &out, err,
		     sizeof(err));
	CHECK(n == -1 && err[0] != '\0',
	      "csv_read of a missing file returns -1 with an error message");
	free(out);
}

int main(void)
{
	TEST_BEGIN();
	test_writer_byte_parity();
	test_write_read_roundtrip();
	test_reader_robustness();
	test_read_errors();
	TEST_SUMMARY("test_csv");
	return tu_fail_count ? 1 : 0;
}
