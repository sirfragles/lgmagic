/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * test_pairing.c - unit tests for keyboard/IMU pairing (pairing.h).
 *
 * Covers: device classification, MAC pairing, case-insensitive MAC
 * comparison, the single-candidate fallback without a MAC, ambiguity
 * rejection, remote ordering, and the identity uniq choice.
 */
#include <stdio.h>
#include <string.h>

#include "test_util.h"
#include "pairing.h"

static const struct pairing_input_dev kbd1 = {
	.path = "/dev/input/event1", .name = "LG Magic Remote",
	.uniq = "aa:bb:cc:dd:ee:01", .vendor = 0x000f, .product = 0x3412,
};
static const struct pairing_input_dev imu1 = {
	.path = "/dev/input/event2", .name = "LG Magic Remote IMU",
	.uniq = "aa:bb:cc:dd:ee:01", .vendor = 0x000f, .product = 0x3412,
};
static const struct pairing_input_dev kbd2 = {
	.path = "/dev/input/event3", .name = "LG Magic Remote",
	.uniq = "aa:bb:cc:dd:ee:02", .vendor = 0x000f, .product = 0x3412,
};
static const struct pairing_input_dev imu2 = {
	.path = "/dev/input/event4", .name = "LG Magic Remote IMU",
	.uniq = "aa:bb:cc:dd:ee:02", .vendor = 0x000f, .product = 0x3412,
};
static const struct pairing_input_dev noise = {
	.path = "/dev/input/event5", .name = "Some other keyboard",
	.uniq = "", .vendor = 0x046d, .product = 0xc534,
};

static void test_classification(void)
{
	CHECK(pairing_is_keyboard(&kbd1) == 1,
	      "LG keyboard classified by name + vid/pid");
	CHECK(pairing_is_keyboard(&imu1) == 0,
	      "the IMU is not a keyboard");
	CHECK(pairing_is_imu(&imu1) == 1,
	      "the IMU classified by name");
	CHECK(pairing_is_imu(&kbd1) == 0,
	      "the keyboard is not an IMU");
	CHECK(pairing_is_keyboard(NULL) == 0 && pairing_is_imu(NULL) == 0,
	      "NULL devices classify as neither");
}

static void test_mac_pairing(void)
{
	const struct pairing_input_dev devs[] = { noise, kbd2, imu1, kbd1, imu2 };
	struct paired_remote out[4];
	size_t n = 0;
	char err[256];

	CHECK(pairing_match(devs, 5, out, 4, &n, err, sizeof(err)) == 0,
	      "MAC pairing of two remotes succeeds");
	CHECK_INT_EQ((long long)n, 2, "two remotes found");
	if (n == 2) {
		/* the pointers must reference the caller's scan array */
		CHECK_PTR_EQ(out[0].keyboard, &devs[1],
			     "remotes follow keyboard discovery order");
		CHECK_PTR_EQ(out[0].imu, &devs[4],
			     "kbd2 paired with imu2 by MAC");
		CHECK_STR_EQ(out[0].uniq, "aa:bb:cc:dd:ee:02",
			     "remote identity = keyboard MAC");
		CHECK_PTR_EQ(out[1].keyboard, &devs[3],
			     "second remote is kbd1");
		CHECK_PTR_EQ(out[1].imu, &devs[2],
			     "kbd1 paired with imu1 by MAC");
	}
}

static void test_case_insensitive(void)
{
	const struct pairing_input_dev k_up = {
		.path = "/dev/input/event1", .name = "LG Magic Remote",
		.uniq = "AA:BB:CC:DD:EE:01", .vendor = 0x000f, .product = 0x3412,
	};
	const struct pairing_input_dev i_lo = {
		.path = "/dev/input/event2", .name = "LG Magic Remote IMU",
		.uniq = "aa:bb:cc:dd:ee:01", .vendor = 0x000f, .product = 0x3412,
	};
	const struct pairing_input_dev devs[] = { k_up, i_lo };
	struct paired_remote out[1];
	size_t n = 0;
	char err[256];

	CHECK(pairing_match(devs, 2, out, 1, &n, err, sizeof(err)) == 0,
	      "MAC matching is case-insensitive");
	CHECK_INT_EQ((long long)n, 1, "one remote");
	if (n == 1)
		CHECK_PTR_EQ(out[0].imu, &devs[1], "paired despite MAC case");
}

static void test_fallback(void)
{
	const struct pairing_input_dev k_no_mac = {
		.path = "/dev/input/event1", .name = "LG Magic Remote",
		.uniq = "", .vendor = 0x000f, .product = 0x3412,
	};
	const struct pairing_input_dev i_no_mac = {
		.path = "/dev/input/event2", .name = "LG Magic Remote IMU",
		.uniq = "", .vendor = 0x000f, .product = 0x3412,
	};
	const struct pairing_input_dev i_other_mac = {
		.path = "/dev/input/event2", .name = "LG Magic Remote IMU",
		.uniq = "ff:ff:ff:ff:ff:ff", .vendor = 0x000f, .product = 0x3412,
	};
	const struct pairing_input_dev devs[] = { k_no_mac, i_no_mac };
	struct paired_remote out[1];
	size_t n = 0;
	char err[256];

	CHECK(pairing_match(devs, 2, out, 1, &n, err, sizeof(err)) == 0,
	      "single-candidate fallback without any MAC succeeds");
	CHECK_INT_EQ((long long)n, 1, "one remote");
	if (n == 1) {
		CHECK_PTR_EQ(out[0].imu, &devs[1], "IMU paired by fallback");
		CHECK_STR_EQ(out[0].uniq, "", "no MAC -> empty identity");
	}

	/* MAC-less keyboard + IMU with a foreign MAC: still pairs (there is
	 * only one candidate), and the IMU MAC becomes the identity (the
	 * keyboard has no MAC of its own to prefer) */
	{
		const struct pairing_input_dev devs2[] = { k_no_mac,
							   i_other_mac };

		n = 0;
		CHECK(pairing_match(devs2, 2, out, 1, &n, err, sizeof(err)) == 0,
		      "single keyboard pairs with a foreign-MAC IMU");
		CHECK_INT_EQ((long long)n, 1, "one remote");
		if (n == 1) {
			CHECK_PTR_EQ(out[0].imu, &devs2[1],
				     "IMU paired by the fallback");
			CHECK_STR_EQ(out[0].uniq, "ff:ff:ff:ff:ff:ff",
				     "identity falls back to the IMU MAC");
		}
	}
}

static void test_ambiguity(void)
{
	const struct pairing_input_dev k_nomac = {
		.path = "/dev/input/event1", .name = "LG Magic Remote",
		.uniq = "", .vendor = 0x000f, .product = 0x3412,
	};
	const struct pairing_input_dev i_nomac = {
		.path = "/dev/input/event2", .name = "LG Magic Remote IMU",
		.uniq = "", .vendor = 0x000f, .product = 0x3412,
	};
	const struct pairing_input_dev devs[] = { k_nomac, k_nomac, i_nomac };
	struct paired_remote out[4];
	size_t n = 0;
	char err[256];

	CHECK(pairing_match(devs, 3, out, 4, &n, err, sizeof(err)) == -1,
	      "two MAC-less keyboards + one IMU is rejected");
	CHECK(err[0] != '\0', "ambiguity error message set");
}

static void test_no_imu(void)
{
	const struct pairing_input_dev devs[] = { kbd1, noise };
	struct paired_remote out[2];
	size_t n = 0;
	char err[256];

	CHECK(pairing_match(devs, 2, out, 2, &n, err, sizeof(err)) == 0,
	      "keyboard without an IMU still yields a remote");
	CHECK_INT_EQ((long long)n, 1, "one remote");
	if (n == 1)
		CHECK_PTR_EQ(out[0].imu, NULL, "imu slot is NULL");
}

static void test_imu_only(void)
{
	const struct pairing_input_dev devs[] = { imu1, noise };
	struct paired_remote out[2];
	size_t n = 0;
	char err[256];

	CHECK(pairing_match(devs, 2, out, 2, &n, err, sizeof(err)) == 0,
	      "an IMU without a keyboard is ignored");
	CHECK_INT_EQ((long long)n, 0, "no remotes");
}

int main(void)
{
	TEST_BEGIN();
	test_classification();
	test_mac_pairing();
	test_case_insensitive();
	test_fallback();
	test_ambiguity();
	test_no_imu();
	test_imu_only();
	TEST_SUMMARY("test_pairing");
	return tu_fail_count ? 1 : 0;
}
