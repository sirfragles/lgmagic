# Userspace tools: a single multi-call binary `lgmagic` + unit tests.
#
# The binary itself is Linux-only (evdev/hidraw/uinput); the `check` target
# builds and runs the portable module tests, which also pass on macOS.

CC      ?= cc
CFLAGS  += -O2 -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -Iinclude -I../include
LDLIBS  += -lm

BIN := lgmagic

SRC := \
	src/main.c src/config.c \
	src/cmd_imu.c src/cmd_analyze.c src/cmd_calibrate.c \
	src/cmd_calib2bin.c src/cmd_config.c src/cmd_setup.c \
	src/cmd_device.c src/cmd_profile.c src/cmd_button.c \
	src/cmd_scroll.c src/cmd_diagnose.c src/cmd_bus.c \
	src/matrix.c src/json.c src/csv.c src/lm.c src/madgwick.c src/calib.c \
	src/evdev.c src/hidraw.c src/uinput.c src/modprobe_helpers.c src/cube.c \
	src/toml.c src/keymap.c src/airmouse.c src/profiles.c src/pairing.c \
	src/dbus_client.c

OBJ := $(SRC:.c=.o)

# Portable modules, shared with the unit tests (build on macOS too).
PORTABLE_SRC := src/matrix.c src/json.c src/csv.c src/calib.c \
	src/madgwick.c src/lm.c src/cube.c src/config.c \
	src/toml.c src/keymap.c src/airmouse.c src/profiles.c src/pairing.c \
	src/dbus_client.c

# Explicit rules (no pattern rules, to keep BSD make on macOS happy).
TEST_DIR := tests
TEST_BINS := $(TEST_DIR)/test_json $(TEST_DIR)/test_matrix $(TEST_DIR)/test_csv \
	$(TEST_DIR)/test_madgwick $(TEST_DIR)/test_lm $(TEST_DIR)/test_calib_blob \
	$(TEST_DIR)/test_config $(TEST_DIR)/test_cube_math \
	$(TEST_DIR)/test_toml $(TEST_DIR)/test_keymap $(TEST_DIR)/test_airmouse \
	$(TEST_DIR)/test_profiles $(TEST_DIR)/test_pairing \
	$(TEST_DIR)/test_dbus_client

# Linux-only: the system daemon and the fake-device e2e harness.
# (GNU make would use ifeq; a shell guard keeps BSD make on macOS happy.)
DAEMON_BIN := lgmagicd
FAKE_BIN := $(TEST_DIR)/fake_devices

# Only the daemon links libsystemd (sd-bus); `lgmagic` keeps libc/libm
# only (asserted by ldd in CI).  `!=` works in BSD make and GNU make 4+.
SYSTEMD_CFLAGS != pkg-config --cflags libsystemd 2>/dev/null || true
SYSTEMD_LIBS != pkg-config --libs libsystemd 2>/dev/null || echo -lsystemd

DAEMON_SRC := src/daemon_main.c src/daemon_bus.c src/daemon_config.c \
	src/daemon_pipeline.c src/daemon_devices.c src/evdev.c src/uinput.c \
	src/config.c src/toml.c src/profiles.c src/pairing.c src/keymap.c \
	src/calib.c src/matrix.c src/json.c src/airmouse.c

all: $(BIN) linux-extra

linux-extra:
	@if [ "`uname -s`" = Linux ]; then $(MAKE) $(DAEMON_BIN) $(FAKE_BIN); fi

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(DAEMON_BIN): $(DAEMON_SRC)
	$(CC) $(CFLAGS) $(SYSTEMD_CFLAGS) -o $@ $(DAEMON_SRC) $(LDLIBS) $(SYSTEMD_LIBS)

$(FAKE_BIN): $(TEST_DIR)/fake_devices.c src/keymap.c
	$(CC) $(CFLAGS) -o $@ $< src/keymap.c $(LDLIBS)

# Runs the busless e2e (SKIPs with exit 0 without /dev/uinput).
e2e: $(BIN) $(DAEMON_BIN) $(FAKE_BIN)
	sh $(TEST_DIR)/daemon_e2e_busless.sh

# Runs the full bus+polkit e2e (dbus-daemon + polkitd; CI only - the
# Apple container VM has no uinput).
e2e-full: $(BIN) $(DAEMON_BIN) $(FAKE_BIN)
	sh $(TEST_DIR)/daemon_e2e.sh

# Bus smoke without devices: needs only a system bus + polkitd (runs in
# the gate container, which has no uinput).
bus-smoke: $(BIN) $(DAEMON_BIN)
	sh $(TEST_DIR)/daemon_bus_smoke.sh

$(TEST_DIR)/test_json: $(TEST_DIR)/test_json.c $(PORTABLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(PORTABLE_SRC) $(LDLIBS)

$(TEST_DIR)/test_matrix: $(TEST_DIR)/test_matrix.c $(PORTABLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(PORTABLE_SRC) $(LDLIBS)

$(TEST_DIR)/test_csv: $(TEST_DIR)/test_csv.c $(PORTABLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(PORTABLE_SRC) $(LDLIBS)

$(TEST_DIR)/test_madgwick: $(TEST_DIR)/test_madgwick.c $(PORTABLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(PORTABLE_SRC) $(LDLIBS)

$(TEST_DIR)/test_lm: $(TEST_DIR)/test_lm.c $(PORTABLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(PORTABLE_SRC) $(LDLIBS)

$(TEST_DIR)/test_calib_blob: $(TEST_DIR)/test_calib_blob.c $(PORTABLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(PORTABLE_SRC) $(LDLIBS)

$(TEST_DIR)/test_config: $(TEST_DIR)/test_config.c $(PORTABLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(PORTABLE_SRC) $(LDLIBS)

$(TEST_DIR)/test_cube_math: $(TEST_DIR)/test_cube_math.c $(PORTABLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(PORTABLE_SRC) $(LDLIBS)

$(TEST_DIR)/test_toml: $(TEST_DIR)/test_toml.c $(PORTABLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(PORTABLE_SRC) $(LDLIBS)

$(TEST_DIR)/test_keymap: $(TEST_DIR)/test_keymap.c $(PORTABLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(PORTABLE_SRC) $(LDLIBS)

$(TEST_DIR)/test_airmouse: $(TEST_DIR)/test_airmouse.c $(PORTABLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(PORTABLE_SRC) $(LDLIBS)

$(TEST_DIR)/test_profiles: $(TEST_DIR)/test_profiles.c $(PORTABLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(PORTABLE_SRC) $(LDLIBS)

$(TEST_DIR)/test_pairing: $(TEST_DIR)/test_pairing.c $(PORTABLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(PORTABLE_SRC) $(LDLIBS)

$(TEST_DIR)/test_dbus_client: $(TEST_DIR)/test_dbus_client.c $(PORTABLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(PORTABLE_SRC) $(LDLIBS)

check: $(TEST_BINS)
	@fail=0; \
	for t in $(TEST_BINS); do \
		echo "== $$t"; \
		if ./$$t; then echo "PASS $$t"; else echo "FAIL $$t"; fail=1; fi; \
	done; \
	if [ -x $(BIN) ]; then sh tests/cli_smoke.sh ./$(BIN) || fail=1; fi; \
	sh tests/check_naming.sh || fail=1; \
	sh tests/check_version_sync.sh || fail=1; \
	exit $$fail

clean:
	rm -f $(BIN) $(DAEMON_BIN) $(FAKE_BIN) $(OBJ) $(TEST_BINS)

.PHONY: all linux-extra check e2e e2e-full bus-smoke clean
