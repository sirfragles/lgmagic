#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# daemon_e2e_busless.sh - busless e2e for lgmagicd (Phase 3 gate).
#
# Runs on a Linux CI runner as root.  SKIPs (exit 0) when /dev/uinput is
# unavailable (e.g. the Apple container on macOS - the full run happens
# on the ubuntu-latest runner).
#
# Covered: takeover + passthrough, rest (no spurious movement), airmouse
# (calibrated gyro -> REL_X with v1 signs), standalone `lgmagic imu`
# in parallel (IMU is deliberately NOT grabbed), SIGHUP profile reload
# (button map + scroll_speed), hotplug reconnect, grab active and grab
# release after SIGKILL.
#
# `cond && cmd || true` in cleanup() and `cond && ... || fail ...`
# assertions are deliberate: under `set -e` the || true arms absorb the
# nonzero exit of reaping already-dead daemon processes, and fail() cannot
# be reached when the checked condition holds.  SC2015 (info) warns about
# the idiom generically; it is not a bug here.
# shellcheck disable=SC2015
set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
TOOLS_DIR=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
BIN=$TOOLS_DIR/lgmagic
DAEMON=$TOOLS_DIR/lgmagicd
FAKE=$SCRIPT_DIR/fake_devices

# 0. uinput available?
if [ ! -e /dev/uinput ]; then
	modprobe uinput 2>/dev/null || true
fi
if [ ! -e /dev/uinput ]; then
	echo "SKIP: /dev/uinput not available - cannot run the busless e2e"
	exit 0
fi

TMP=$(mktemp -d /tmp/lgmagic-e2e.XXXXXX)
DAEMON_PID=
FAKE_PID=

cleanup()
{
	[ -n "$DAEMON_PID" ] && kill -9 "$DAEMON_PID" 2>/dev/null || true
	[ -n "$FAKE_PID" ] && kill "$FAKE_PID" 2>/dev/null || true
	rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

fail()
{
	echo "FAIL: $*"
	exit 1
}

# find the /dev/input/eventN node of an input device by name,
# mknod'ing it when the node does not exist (container without udev)
find_node()
{
	name=$1
	for input in /sys/class/input/input*; do
		[ -f "$input/name" ] || continue
		[ "$(cat "$input/name" 2>/dev/null)" = "$name" ] || continue
		for ev in "$input"/event*; do
			[ -f "$ev/dev" ] || continue
			dev=$(cat "$ev/dev")
			node=/dev/input/$(basename "$ev")
			if [ ! -e "$node" ]; then
				mkdir -p /dev/input
				mknod "$node" c "${dev%%:*}" "${dev##*:}" \
					2>/dev/null || continue
			fi
			echo "$node"
			return 0
		done
	done
	return 1
}

# like find_node, but matches by NAME PREFIX - the daemon's virtual
# devices carry the identity ("lgmagicd keyboard unknown"), while the
# fake devices need the exact match (a prefix would collide
# "LG Magic Remote" with "LG Magic Remote IMU")
find_node_prefix()
{
	prefix=$1
	for input in /sys/class/input/input*; do
		[ -f "$input/name" ] || continue
		name=$(cat "$input/name" 2>/dev/null)
		case "$name" in
		"$prefix"*) ;;
		*) continue ;;
		esac
		for ev in "$input"/event*; do
			[ -f "$ev/dev" ] || continue
			dev=$(cat "$ev/dev")
			node=/dev/input/$(basename "$ev")
			if [ ! -e "$node" ]; then
				mkdir -p /dev/input
				mknod "$node" c "${dev%%:*}" "${dev##*:}" \
					2>/dev/null || continue
			fi
			echo "$node"
			return 0
		done
	done
	return 1
}

wait_for()
{
	label=$1; want=$2; file=$3
	i=0
	while [ $i -lt 100 ]; do
		grep -q "$want" "$file" 2>/dev/null && return 0
		i=$((i + 1))
		sleep 0.1
	done
	fail "$label: timeout waiting for '$want': $(cat "$file" 2>/dev/null)"
}

# ------------------------------------------------------------------ #
# Setup: config root + state dir + identity calibration               #
# ------------------------------------------------------------------ #

CFG=$TMP/config
STATE=$TMP/state
mkdir -p "$CFG/devices.d" "$STATE/unknown"

cat > "$CFG/config.toml" <<'EOF'
lpf_alpha = 0.2
mouse_scale = 30.0
EOF

# Gyro scale [1,1,1] - the airmouse assertion below computes with
# g_corr = R_align * 100 * pi/180 = -1.745329 rad/s on the z axis.
cat > "$STATE/unknown/calibration.json" <<'EOF'
{
    "gyro": {
        "bias": [0.0, 0.0, 0.0],
        "scale": [1.0, 1.0, 1.0]
    }
}
EOF

# ------------------------------------------------------------------ #
# Fake remote + daemon                                                #
# ------------------------------------------------------------------ #

$FAKE create > "$TMP/fake.out" 2>&1 &
FAKE_PID=$!
wait_for "fake create" "imu=" "$TMP/fake.out"
KBD=$(sed -n 's/^keyboard=//p' "$TMP/fake.out")
IMU=$(sed -n 's/^imu=//p' "$TMP/fake.out")
[ -n "$KBD" ] && [ -n "$IMU" ] || fail "fake: no keyboard=/$IMU lines: $(cat "$TMP/fake.out")"
echo "fake: kbd=$KBD imu=$IMU"

$DAEMON --keyboard "$KBD" --config-root "$CFG" --state-dir "$STATE" \
	--debug > "$TMP/daemon.log" 2>&1 &
DAEMON_PID=$!
wait_for "daemon startup" "virtual devices ready" "$TMP/daemon.log"
wait_for "daemon takeover" "remote unknown: keyboard" "$TMP/daemon.log"

OUTK=$(find_node_prefix "lgmagicd keyboard")
OUTM=$(find_node_prefix "lgmagicd mouse")
[ -n "$OUTK" ] || fail "daemon: 'lgmagicd keyboard' output node not found"
[ -n "$OUTM" ] || fail "daemon: 'lgmagicd mouse' output node not found"
echo "daemon: outk=$OUTK outm=$OUTM"

# ------------------------------------------------------------------ #
# 1. Passthrough: no devices.d yet, KEY_UP arrives unmapped           #
# ------------------------------------------------------------------ #

( "$FAKE" watch "$OUTK" --ms 1200 > "$TMP/w1.out" 2>&1 ) &
sleep 0.1
"$FAKE" emit --kbd "$KBD" --key KEY_UP
sleep 1.3
grep -q "KEY KEY_UP 1" "$TMP/w1.out" || fail "passthrough press: $(cat "$TMP/w1.out")"
grep -q "KEY KEY_UP 0" "$TMP/w1.out" || fail "passthrough release: $(cat "$TMP/w1.out")"
echo "OK passthrough"

# ------------------------------------------------------------------ #
# 2. Rest: idle IMU must not move the mouse                           #
# ------------------------------------------------------------------ #

"$FAKE" watch "$OUTM" --ms 400 > "$TMP/w2.out" 2>&1
[ -s "$TMP/w2.out" ] && fail "rest: unexpected movement: $(cat "$TMP/w2.out")"
echo "OK rest"

# ------------------------------------------------------------------ #
# 3. Airmouse: gyro (0,0,100) -> filt[2]=-0.349066 -> dx=10           #
# ------------------------------------------------------------------ #

( "$FAKE" watch "$OUTM" --ms 1500 > "$TMP/w3.out" 2>&1 ) &
sleep 0.1
"$FAKE" emit --imu "$IMU" --gyro 0,0,0
sleep 0.2
"$FAKE" emit --imu "$IMU" --gyro 0,0,100
sleep 1.5
grep -q "REL_X 10$" "$TMP/w3.out" || fail "airmouse: expected 'REL_X 10', got: $(cat "$TMP/w3.out")"
grep -q "REL_Y" "$TMP/w3.out" && fail "airmouse: unexpected REL_Y: $(cat "$TMP/w3.out")"
echo "OK airmouse"

# ------------------------------------------------------------------ #
# 4. Standalone `lgmagic imu` works in parallel (IMU not grabbed)    #
# ------------------------------------------------------------------ #

# The CSV is written when the CLI exits, so end it explicitly:
# two samples, then SIGINT + wait make the file state deterministic
# (no reliance on --duration timing on a quiet device).
( "$BIN" imu --csv "$TMP/csv.out" --device "$IMU" --duration 0.4 \
	> "$TMP/imu.out" 2>&1 ) &
imu_pid=$!
sleep 0.1	# let the CLI open the evdev node before the samples
"$FAKE" emit --imu "$IMU" --gyro 1,2,100
"$FAKE" emit --imu "$IMU" --gyro 1,2,100
kill -INT "$imu_pid" 2>/dev/null || true
wait "$imu_pid" 2>/dev/null || \
	fail "standalone imu command failed: $(cat "$TMP/imu.out")"
[ -s "$TMP/csv.out" ] || fail "standalone imu: no CSV data: $(cat "$TMP/imu.out")"
grep -q "100" "$TMP/csv.out" || fail "standalone imu: raw gyro not in CSV: $(cat "$TMP/csv.out")"
echo "OK standalone imu"

# ------------------------------------------------------------------ #
# 5. SIGHUP: devices.d profile (map + scroll_speed) goes live         #
# ------------------------------------------------------------------ #

cat > "$CFG/devices.d/unknown.toml" <<'EOF'
airmouse = true

[profiles.default]
scroll_speed = 2.0
sensitivity = 30.0

[profiles.default.button_map]
"KEY_UP" = "KEY_VOLUMEUP"
EOF

kill -HUP "$DAEMON_PID"
sleep 0.5
wait_for "reload" "config reloaded" "$TMP/daemon.log"

# 5a. mapping: KEY_UP -> KEY_VOLUMEUP
( "$FAKE" watch "$OUTK" --ms 1200 > "$TMP/w4.out" 2>&1 ) &
sleep 0.1
"$FAKE" emit --kbd "$KBD" --key KEY_UP
sleep 1.3
grep -q "KEY KEY_VOLUMEUP 1" "$TMP/w4.out" || fail "mapping: expected KEY_VOLUMEUP, got: $(cat "$TMP/w4.out")"
grep -q "KEY KEY_UP 1" "$TMP/w4.out" && fail "mapping: unmapped KEY_UP leaked: $(cat "$TMP/w4.out")"
echo "OK mapping"

# 5b. scroll: 2 detents x scroll_speed 2.0 -> REL_WHEEL 4 (+480 hires)
( "$FAKE" watch "$OUTM" --ms 1200 > "$TMP/w5.out" 2>&1 ) &
sleep 0.1
"$FAKE" emit --kbd "$KBD" --wheel 2
sleep 1.3
grep -q "REL_WHEEL 4$" "$TMP/w5.out" || fail "scroll: expected 'REL_WHEEL 4', got: $(cat "$TMP/w5.out")"
grep -q "REL_WHEEL_HI_RES 480$" "$TMP/w5.out" || fail "scroll: expected 'REL_WHEEL_HI_RES 480', got: $(cat "$TMP/w5.out")"
echo "OK scroll"

# 5c. held key across a SIGHUP remap: the old virtual key must not stick
# Press KEY_UP (maps to KEY_VOLUMEUP), remap it to KEY_HOME via devices.d
# + SIGHUP while still held, then release.  pipeline_configure must
# release KEY_VOLUMEUP when the map changes; the held press must not
# leak into the new map as a KEY_HOME press.
( "$FAKE" watch "$OUTK" --ms 4000 > "$TMP/w5c.out" 2>&1 ) &
w5c=$!
sleep 0.1
"$FAKE" emit --kbd "$KBD" --press KEY_UP
sleep 0.3
cat > "$CFG/devices.d/unknown.toml" <<'EOF'
airmouse = true

[profiles.default]
scroll_speed = 2.0
sensitivity = 30.0

[profiles.default.button_map]
"KEY_UP" = "KEY_HOME"
EOF
# the reload line from section 5 is already in the log - wait for a NEW one
n0=$(grep -c "config reloaded" "$TMP/daemon.log" 2>/dev/null || true)
kill -HUP "$DAEMON_PID"
i=0
while [ $i -lt 100 ]; do
	[ "$(grep -c "config reloaded" "$TMP/daemon.log" 2>/dev/null || true)" -gt "$n0" ] && break
	i=$((i + 1))
	sleep 0.1
done
[ $i -lt 100 ] || fail "held key: second reload never completed: $(cat "$TMP/daemon.log")"
sleep 0.3
"$FAKE" emit --kbd "$KBD" --release KEY_UP
sleep 0.7
kill "$w5c" 2>/dev/null || true
wait "$w5c" 2>/dev/null || true
grep -q "KEY KEY_VOLUMEUP 1" "$TMP/w5c.out" || \
	fail "held key: no KEY_VOLUMEUP press: $(cat "$TMP/w5c.out")"
grep -q "KEY KEY_VOLUMEUP 0" "$TMP/w5c.out" || \
	fail "held key: KEY_VOLUMEUP never released (stuck): $(cat "$TMP/w5c.out")"
grep -q "KEY KEY_HOME 1" "$TMP/w5c.out" && \
	fail "held key: the held press leaked into the new map: $(cat "$TMP/w5c.out")"
echo "OK held key across reload"

# restore the KEY_VOLUMEUP map for section 6 (reconnect re-applies
# devices.d from disk, so the file itself is the source of truth)
cat > "$CFG/devices.d/unknown.toml" <<'EOF'
airmouse = true

[profiles.default]
scroll_speed = 2.0
sensitivity = 30.0

[profiles.default.button_map]
"KEY_UP" = "KEY_VOLUMEUP"
EOF
kill -HUP "$DAEMON_PID"
sleep 0.5

# ------------------------------------------------------------------ #
# 6. Reconnect: kill the fake, restart it, the daemon re-adds it      #
# ------------------------------------------------------------------ #

kill "$FAKE_PID" 2>/dev/null || true
wait "$FAKE_PID" 2>/dev/null || true
FAKE_PID=
sleep 0.5

$FAKE create > "$TMP/fake2.out" 2>&1 &
FAKE_PID=$!
wait_for "fake restart" "imu=" "$TMP/fake2.out"
KBD2=$(sed -n 's/^keyboard=//p' "$TMP/fake2.out")
IMU2=$(sed -n 's/^imu=//p' "$TMP/fake2.out")
echo "fake restart: kbd=$KBD2 imu=$IMU2"
[ "$KBD2" = "$KBD" ] || echo "NOTE: event numbers changed ($KBD -> $KBD2); the daemon is pinned to the old path - this check may fail"

# The daemon logs a second takeover line for the re-added remote
# (inotify debounce or the 2 s polling fallback).
i=0
while [ $i -lt 60 ]; do
	[ "$(grep -c "remote unknown: keyboard" "$TMP/daemon.log")" -ge 2 ] && break
	i=$((i + 1))
	sleep 0.1
done
[ "$(grep -c "remote unknown: keyboard" "$TMP/daemon.log")" -ge 2 ] || \
	fail "reconnect: daemon did not re-add the remote: $(cat "$TMP/daemon.log")"

# The re-added remote re-applies devices.d: KEY_UP still maps.
( "$FAKE" watch "$OUTK" --ms 1200 > "$TMP/w6.out" 2>&1 ) &
sleep 0.1
"$FAKE" emit --kbd "$KBD2" --key KEY_UP
sleep 1.3
grep -q "KEY KEY_VOLUMEUP 1" "$TMP/w6.out" || fail "reconnect: mapped key after re-add, got: $(cat "$TMP/w6.out")"
echo "OK reconnect"

# ------------------------------------------------------------------ #
# 7. Grab: raw events hidden while the daemon lives, visible after    #
#    SIGKILL (the grab dies with the fd)                              #
# ------------------------------------------------------------------ #

( "$FAKE" watch "$KBD2" --ms 600 > "$TMP/wg1.out" 2>&1 ) &
sleep 0.1
"$FAKE" emit --kbd "$KBD2" --key KEY_ENTER
sleep 0.7
[ -s "$TMP/wg1.out" ] && fail "grab: raw events leaked while grabbed: $(cat "$TMP/wg1.out")"
echo "OK grab active"

kill -9 "$DAEMON_PID" 2>/dev/null || true
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=
sleep 0.3

( "$FAKE" watch "$KBD2" --ms 600 > "$TMP/wg2.out" 2>&1 ) &
sleep 0.1
"$FAKE" emit --kbd "$KBD2" --key KEY_ENTER
sleep 0.7
grep -q "KEY KEY_ENTER 1" "$TMP/wg2.out" || fail "grab release: no raw events after SIGKILL: $(cat "$TMP/wg2.out")"
echo "OK grab release"

echo "E2E PASS"
