/*  This is part of lgmagic_dkms

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
#include <linux/module.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/firmware.h>
#include <linux/version.h>

#include "lgmagic_airmouse.h"

/* lgmagic_airmouse.c does single-precision float math, compiled with
 * -msse (kernel/Makefile) so it builds against distro kernels that use
 * -mno-sse (Debian 12, RHEL-family); on arm64 the same file drops
 * -mgeneral-regs-only via CFLAGS_REMOVE. Any entry into that code must
 * therefore save/restore the kernel FPU state the standard way -
 * kernel_fpu_begin/end on x86, kernel_neon_begin/end on arm64. Since
 * kernel 7.0 the arm64 API takes a caller-owned buffer (Ard Biesheuvel's
 * on-stack FPSIMD rework) - kept in drvdata so begin and end hand in the
 * same one. Both call sites are process context (HID report handling,
 * probe-time firmware load) and do no sleeping in between. No-op
 * elsewhere. */
#ifdef CONFIG_X86
#include <asm/fpu/api.h>
#elif defined(CONFIG_ARM64)
#include <asm/neon.h>
#endif

static int debug = 1;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Debug message level (0=quiet, 1=normal, 2=verbose)");

#define lgmagic_dev_dbg(dev, fmt, ...) \
do { if (debug >= 2) dev_dbg(dev, fmt, ##__VA_ARGS__); } while (0)

#define lgmagic_dev_info(dev, fmt, ...) \
do { if (debug >= 1) dev_info(dev, fmt, ##__VA_ARGS__); } while (0)

#define lgmagic_dev_warn(dev, fmt, ...) \
do { if (debug >= 1) dev_warn(dev, fmt, ##__VA_ARGS__); } while (0)

#define lgmagic_dev_err(dev, fmt, ...) \
do { dev_err(dev, fmt, ##__VA_ARGS__); } while (0)

static int raw_only = 1;
module_param(raw_only, int, 0644);
MODULE_PARM_DESC(raw_only, "Raw decoder mode: buttons + REL_WHEEL only, no airmouse (default 1; lgmagicd processes the input)");

static int airmouse = 1;
module_param(airmouse, int, 0644);
MODULE_PARM_DESC(airmouse, "Report mouse events (raw_only=0 only)");

static int airmouse_threshold = 300;
module_param(airmouse_threshold, int, 0644);
MODULE_PARM_DESC(airmouse_threshold, "Airmouse enable threshold");

static int imu_evdev = 0;
module_param(imu_evdev, int, 0644);
MODULE_PARM_DESC(imu_evdev, "Expose raw IMU");

static int key_0x8000 = KEY_POWER;
module_param(key_0x8000, int, 0644);
MODULE_PARM_DESC(key_0x8000, "Keycode for HID code 0x8000, which is model-dependent: KEY_POWER on the MR20, but the channel-up key on the AN-MR19BA (its power key is IR-only and sends no BLE event - set this to KEY_CHANNELUP there)");

struct lgmagic_drvdata {
	struct input_dev *input_hid;
	struct input_dev *input_imu;

	u16 last_keycode;
	u16 last_btncode;
	float gyro_acc[3];
	int mode;
	struct lgmagic_airmouse_calib calib;
#if defined(CONFIG_ARM64) && LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
	struct user_fpsimd_state neon_state;
#endif
};

static inline void lgmagic_fpu_begin(struct lgmagic_drvdata *drvdata)
{
#ifdef CONFIG_X86
	kernel_fpu_begin();
#elif defined(CONFIG_ARM64)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
	kernel_neon_begin(&drvdata->neon_state);
#else
	kernel_neon_begin();
#endif
#endif
}

static inline void lgmagic_fpu_end(struct lgmagic_drvdata *drvdata)
{
#ifdef CONFIG_X86
	kernel_fpu_end();
#elif defined(CONFIG_ARM64)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
	kernel_neon_end(&drvdata->neon_state);
#else
	kernel_neon_end();
#endif
#endif
}

#define LGMAGIC_CODE_WHEEL 0x8044
#define LGMAGIC_CODE_MODELDEP 0x8000

/* 0x8000 is the model-dependent button: KEY_POWER on the MR20, but the
 * channel-up key on the AN-MR19BA (reported by a tester: its power key
 * transmits IR only, so no BLE event ever arrives). The key_0x8000
 * module parameter overrides the table default; anything out of the
 * valid keycode range falls back to the table value. */
static inline u16 lgmagic_btn_keycode(u16 code, u16 keycode)
{
	if (code == LGMAGIC_CODE_MODELDEP && key_0x8000 >= 0 &&
	    key_0x8000 <= KEY_MAX)
		return (u16)key_0x8000;
	return keycode;
}

static const struct {
	u16 code;
	u16 keycode;
} lg_btn_map[] = {
	{ LGMAGIC_CODE_MODELDEP, KEY_POWER },
	{ 0x8099, KEY_SLEEP },
	{ 0x8010, KEY_0 }, { 0x8011, KEY_1 }, { 0x8012, KEY_2 },
	{ 0x8013, KEY_3 }, { 0x8014, KEY_4 }, { 0x8015, KEY_5 },
	{ 0x8016, KEY_6 }, { 0x8017, KEY_7 }, { 0x8018, KEY_8 }, { 0x8019, KEY_9 },
	{ LGMAGIC_CODE_WHEEL, KEY_ENTER },
	{ LGMAGIC_CODE_WHEEL, BTN_LEFT },
	{ 0x8053, KEY_LIST },
	{ 0x8045, KEY_MENU }, // ... button
	{ 0x8002, KEY_VOLUMEUP },
	{ 0x8003, KEY_VOLUMEDOWN },
	{ 0x8009, KEY_MUTE },
	{ 0x808B, KEY_VOICECOMMAND },
	{ 0x807C, KEY_HOME },
	{ 0x8043, KEY_SETUP },
	{ 0x8028, KEY_BACK },
	{ 0x80AB, KEY_PROGRAM },
	{ 0x805D, KEY_MEDIA }, // IVI
	{ 0x800B, KEY_TV },
	{ 0x8098, KEY_CONTEXT_MENU }, // STB MENU
	{ 0x8001, KEY_CHANNELDOWN },
	{ 0x8072, KEY_RED },
	{ 0x8071, KEY_GREEN },
	{ 0x8063, KEY_YELLOW },
	{ 0x8061, KEY_BLUE },
	{ 0x8081, KEY_VIDEO }, // MOVIES
	{ 0x80B0, KEY_PLAY },
	{ 0x80BA, KEY_PAUSE },
	{ 0x8040, KEY_UP },
	{ 0x8041, KEY_DOWN },
	{ 0x8006, KEY_RIGHT },
	{ 0x8007, KEY_LEFT },
};

static int lgmagic_raw_event(struct hid_device *hdev, struct hid_report *report,
				u8 *data, int size)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(hdev);
	u8 reporting = 0;
	u16 counter;
	s16 imu[6];
	s16 mouse[2] = {0};

	int i;

	if (!drvdata || !drvdata->input_hid || !drvdata->input_imu)
	{
		lgmagic_dev_warn(&hdev->dev, "No drvdata or no input dev");
		return 0;
	}

	if (size != 20 || data[0] != 0xFD)
	{
		lgmagic_dev_warn(&hdev->dev, "Unknown descriptor with size %d and type %x", size, data[0]);
		return 0; // Not ours
	}

	// Button is last two bytes before wheel
	u16 btn_code = (data[17] << 8) | data[18];
	s8 wheel = (s8)data[19];

	/* Parse counter (little-endian) */
	counter = data[1] | (data[2] << 8);

	/* Parse 6 signed 16-bit values, big-endian */
	for (i = 0; i < 6; i++) {
		imu[i] = (data[5 + 2*i] << 8) | data[6 + 2*i];
	}

	if (btn_code != drvdata->last_btncode)
	{
		input_report_key(drvdata->input_hid, drvdata->last_keycode, 0);
		reporting = 1;
		drvdata->last_keycode = 0;
		drvdata->last_btncode = btn_code;
		if (btn_code != 0)
		{
			for (i = 0; i < ARRAY_SIZE(lg_btn_map); i++) {
				if (lg_btn_map[i].code == btn_code) {
					u16 report_keycode = lgmagic_btn_keycode(lg_btn_map[i].code, lg_btn_map[i].keycode);

					if (lg_btn_map[i].code==LGMAGIC_CODE_WHEEL && drvdata->mode && !raw_only)
						report_keycode = BTN_LEFT;
					else
						drvdata->mode = 0;

					input_report_key(drvdata->input_hid, report_keycode, 1);
					drvdata->last_keycode = report_keycode;
					break;
				}
			}
		}
	}

	if (wheel != 0)
	{
		/* raw_only: the wheel is always the native REL_WHEEL detent
		 * step - never key emulation (the v1 fallback path would make
		 * it indistinguishable from the physical UP/DOWN buttons). */
		if (raw_only || drvdata->mode)
			input_report_rel(drvdata->input_hid, REL_WHEEL, wheel);
		else
		{
			input_report_key(drvdata->input_hid, wheel>0 ? KEY_UP : KEY_DOWN, 1);
			input_report_key(drvdata->input_hid, wheel>0 ? KEY_UP : KEY_DOWN, 0);
		}
		reporting = 1;
	}

	if (airmouse && !raw_only)
	{
		int bigmove;

		lgmagic_fpu_begin(drvdata);
		bigmove = lgmagic_calc_mouse(&drvdata->calib, drvdata->gyro_acc, airmouse_threshold, imu, mouse);
		lgmagic_fpu_end(drvdata);

		if (bigmove)
			drvdata->mode = 1;

		if (drvdata->mode==1)
		{
			input_report_rel(drvdata->input_hid, REL_X, mouse[0]);
			input_report_rel(drvdata->input_hid, REL_Y, mouse[1]);
		}
	}

	if (mouse[0] || mouse[1])
		reporting = 1;

	if (reporting)
	{
		input_sync(drvdata->input_hid);
	}

	/* raw_only implies the IMU evdev - the daemon needs the raw IMU
	 * stream to do airmouse in userspace. */
	if (!imu_evdev && !raw_only)
		return 0;

	/* Report counter */
	input_event(drvdata->input_imu, EV_MSC, MSC_SERIAL, counter);

	/* Report IMU axes */
	input_report_abs(drvdata->input_imu, ABS_X,  imu[3]);
	input_report_abs(drvdata->input_imu, ABS_Y,  imu[4]);
	input_report_abs(drvdata->input_imu, ABS_Z,  imu[5]);
	input_report_abs(drvdata->input_imu, ABS_RX, imu[0]);
	input_report_abs(drvdata->input_imu, ABS_RY, imu[1]);
	input_report_abs(drvdata->input_imu, ABS_RZ, imu[2]);

	input_sync(drvdata->input_imu);

	return 0;
}

static void lgmagic_sanitize_mac(const char *uniq, char *out)
{
	size_t i = 0;
	for (i = 0; uniq[i] && i<17; i++) {
		if (uniq[i] == ':')
			out[i] = '_';  /* or just skip it */
			else
				out[i] = uniq[i];
	}
}

static int lgmagic_load_fw(const char *fwname, struct device *dev, struct lgmagic_drvdata *drvdata)
{
	int ret;
	int calib_ok;
	const struct firmware *fw;

	ret = request_firmware(&fw, fwname, dev);
	if (ret == 0 && fw->size>=sizeof(struct lgmagic_airmouse_calib)) {
		lgmagic_dev_info(dev, "Loading LG Magic calibration");
		memcpy(&drvdata->calib, fw->data, sizeof(struct lgmagic_airmouse_calib));
		release_firmware(fw);

		lgmagic_fpu_begin(drvdata);
		calib_ok = (lgmagic_validate_calib(&drvdata->calib) == 0);
		lgmagic_fpu_end(drvdata);

		if (!calib_ok)
		{
			lgmagic_dev_warn(dev, "Calibration table isn't valid. Airmouse disabled");
			memset(&drvdata->calib, 0, sizeof(struct lgmagic_airmouse_calib));
			return 0;
		}
	}
	return ret;
}

static int lgmagic_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret, i;
	struct lgmagic_drvdata *drvdata;

	drvdata = devm_kzalloc(&hdev->dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	hid_set_drvdata(hdev, drvdata);

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	if (strlen(hdev->uniq)==17)
	{
		char addr_fw_name[] = "lgmagic_calib_XX_XX_XX_XX_XX_XX.bin";
		lgmagic_sanitize_mac(hdev->uniq, addr_fw_name+sizeof("lgmagic_calib_")-1);
		if (lgmagic_load_fw(addr_fw_name, &hdev->dev, drvdata)==0)
			goto loaded;
	}
		lgmagic_load_fw("lgmagic_calib.bin", &hdev->dev, drvdata);
loaded:

	drvdata->input_hid = devm_input_allocate_device(&hdev->dev);
	if (!drvdata->input_hid)
		return -ENOMEM;

	drvdata->input_hid->name = "LG Magic Remote";
	drvdata->input_hid->id.bustype = hdev->bus;
	drvdata->input_hid->id.vendor = hdev->vendor;
	drvdata->input_hid->id.product = hdev->product;

	set_bit(EV_KEY, drvdata->input_hid->evbit);
	/* EV_REP: let the input core generate autorepeat for held keys
	 * (feedback from an AN-MR19BA tester). */
	set_bit(EV_REP, drvdata->input_hid->evbit);
	set_bit(EV_REL, drvdata->input_hid->evbit);
	set_bit(REL_WHEEL, drvdata->input_hid->relbit);
	set_bit(REL_X, drvdata->input_hid->relbit);
	set_bit(REL_Y, drvdata->input_hid->relbit);

	for (i = 0; i < ARRAY_SIZE(lg_btn_map); i++)
		set_bit(lgmagic_btn_keycode(lg_btn_map[i].code,
			lg_btn_map[i].keycode), drvdata->input_hid->keybit);

	ret = input_register_device(drvdata->input_hid);
	if (ret)
		return ret;

	drvdata->input_imu = devm_input_allocate_device(&hdev->dev);
	if (!drvdata->input_imu)
		return -ENOMEM;

	drvdata->input_imu->name = "LG Magic Remote IMU";
	drvdata->input_imu->id.bustype = hdev->bus;
	drvdata->input_imu->id.vendor = hdev->vendor;
	drvdata->input_imu->id.product = hdev->product;

	set_bit(EV_ABS, drvdata->input_imu->evbit);
	set_bit(EV_MSC, drvdata->input_imu->evbit);

	set_bit(MSC_SERIAL, drvdata->input_imu->mscbit);

	for (i = ABS_X; i <= ABS_RZ; i++)
	{
		set_bit(i, drvdata->input_imu->absbit);
		input_set_abs_params(drvdata->input_imu, i, -32768, 32767, 4, 4);
	}

	if (imu_evdev || raw_only)
	{
		ret = input_register_device(drvdata->input_imu);
		if (ret)
			return ret;
	}

	return 0;
}

static void lgmagic_remove(struct hid_device *hdev)
{
	hid_hw_stop(hdev);
}

static const struct hid_device_id lgmagic_devices[] = {
	{ HID_BLUETOOTH_DEVICE(0x000f, 0x3412) }, // LG Magic Remote
	{ }
};
MODULE_DEVICE_TABLE(hid, lgmagic_devices);

static struct hid_driver lgmagic_driver = {
	.name = "lgmagic",
	.id_table = lgmagic_devices,
	.raw_event = lgmagic_raw_event,
	.probe = lgmagic_probe,
	.remove = lgmagic_remove,
};

module_hid_driver(lgmagic_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ilya \"Kokokoshka\" Chelyadin <ilya77105@gmail.com>");
MODULE_DESCRIPTION("LG Magic Remote HID Driver");
