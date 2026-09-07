#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# daemon_e2e.sh - full bus + polkit e2e for lg-magicd (Phase 4 gate).
#
# Runs on a Linux CI runner as root with /dev/uinput, dbus-daemon and
# polkitd.  SKIPs (exit 0) when any of those is missing.  Starts a
# throwaway system bus, a fake remote (uinput), and lg-magicd, then
# drives the whole surface through the CLI:
#
#   - device list / status (unprivileged reads, polkit-free - also as
#     nobody)
#   - button map via the daemon (polkit modify-input): root OK,
#     nobody -> NotAuthorized; the mapping takes effect immediately
#     (no daemon restart)
#   - profile set: state.toml updated, the map/scroll follow the
#     active profile
#   - scroll speed / sensitivity / reset (persisted in devices.d)
#   - SetCalibPath + Reload via dbus-send: new calibration applies;
#     an invalid calibration file is rejected (old one kept, daemon
#     stays alive)
#   - profile/button list read the files directly (world-readable)
#   - standalone `lg-magic imu` in parallel (IMU not grabbed)
#   - reconnect + grab release after SIGKILL, SIGTERM and SIGINT
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TOOLS_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
BIN=$TOOLS_DIR/lg-magic
DAEMON=$TOOLS_DIR/lg-magicd
FAKE=$SCRIPT_DIR/fake_devices
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)

# 0. prerequisites
if [ ! -e /dev/uinput ]; then
	modprobe uinput 2>/dev/null || true
fi
[ -e /dev/uinput ] || { echo "SKIP: /dev/uinput not available"; exit 0; }
command -v dbus-daemon >/dev/null 2>&1 || { echo "SKIP: no dbus-daemon"; exit 0; }
POLKITD=$(command -v polkitd 2>/dev/null || true)
[ -n "$POLKITD" ] || POLKITD=/usr/lib/polkit-1/polkitd
[ -x "$POLKITD" ] || { echo "SKIP: no polkitd"; exit 0; }
command -v dbus-send >/dev/null 2>&1 || { echo "SKIP: no dbus-send"; exit 0; }
[ "`id -u`" = 0 ] || { echo "SKIP: not root (needs a system bus)"; exit 0; }
[ ! -e /run/dbus/system_bus_socket ] || {
	echo "SKIP: a system bus is already running (cannot install the policy)";
	exit 0;
}

TMP=$(mktemp -d /tmp/lgmagic-e2e.XXXXXX)
DAEMON_PID=
FAKE_PID=
DBUS_PID=
POLKIT_PID=
DAEMON_LOG=

cleanup()
{
	[ -n "$DAEMON_PID" ] && kill -9 "$DAEMON_PID" 2>/dev/null || true
	[ -n "$FAKE_PID" ] && kill "$FAKE_PID" 2>/dev/null || true
	[ -n "$POLKIT_PID" ] && kill "$POLKIT_PID" 2>/dev/null || true
	[ -n "$DBUS_PID" ] && kill "$DBUS_PID" 2>/dev/null || true
	rm -f /etc/dbus-1/system.d/org.lgmagic.conf
	rm -f /usr/share/polkit-1/actions/org.lgmagic.policy
	rm -rf "$TMP"
	wait 2>/dev/null
}
trap cleanup EXIT INT TERM

fail()
{
	echo "FAIL: $*"
	[ -n "$last_exp_file" ] && { echo "--- watcher output ---"; cat "$last_exp_file" 2>/dev/null || true; }
	[ -n "$DAEMON_LOG" ] && { echo "--- daemon log tail ---"; tail -40 "$DAEMON_LOG" 2>/dev/null || true; }
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
# devices carry the identity ("lg-magicd keyboard unknown"), while the
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

# watch OUT + emit + expect line; every check gets its OWN
# output file and its watcher is killed afterwards (a stale watcher
# writing into a later check's file would fake a pass)
n_exp=0
last_exp_file=
# call convention: expect_emit LABEL OUT <emit flags> EXPECT MS - the
# emit flags (e.g. --kbd PATH --key NAME) go verbatim to fake_devices,
# the LAST two arguments are the grep pattern and the timeout. All emit
# arguments are controlled literals (paths, KEY_*/REL_* names), so the
# re-evaluation below is safe.
expect_emit()
{
	label=$1; out=$2; shift 2

	emit_args=
	while [ $# -gt 2 ]; do
		emit_args="$emit_args $1"
		shift
	done
	expect=$1; ms=$2

	f=$TMP/exp.$n_exp.out
	n_exp=$((n_exp + 1))
	last_exp_file=$f
	( "$FAKE" watch "$out" --ms "$ms" > "$f" 2>&1 ) &
	wp=$!
	sleep 0.1
	eval "\"$FAKE\" emit$emit_args"
	sleep $((ms / 1000 + 1))
	kill "$wp" 2>/dev/null || true
	wait "$wp" 2>/dev/null || true
	grep -q "$expect" "$f" || \
		fail "$label: expected '$expect', got: $(cat "$f")"
	echo "OK $label"
}

# ------------------------------------------------------------------ #
# Throwaway system bus + polkitd + policy                            #
# ------------------------------------------------------------------ #

cp "$REPO_ROOT/data/org.lgmagic.policy" /usr/share/polkit-1/actions/
cp "$REPO_ROOT/data/org.lgmagic.conf" /etc/dbus-1/system.d/
mkdir -p /run/dbus
dbus-daemon --system --nofork --nopidfile &
DBUS_PID=$!
sleep 0.5
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/system_bus_socket
"$POLKITD" --no-debug &
POLKIT_PID=$!
sleep 1

# ------------------------------------------------------------------ #
# Config: two profiles with different maps + a calibration           #
# ------------------------------------------------------------------ #

CFG=$TMP/config
STATE=$TMP/state
mkdir -p "$CFG/devices.d" "$STATE/unknown"

cat > "$CFG/config.toml" <<'EOF'
lpf_alpha = 0.2
mouse_scale = 30.0
EOF

# Gyro scale [1,1,1] - g_corr = R_align * 100 * pi/180 = -1.745329 on z.
cat > "$STATE/unknown/calibration.json" <<'EOF'
{
    "gyro": {
        "bias": [0.0, 0.0, 0.0],
        "scale": [1.0, 1.0, 1.0]
    }
}
EOF

cat > "$CFG/devices.d/unknown.toml" <<'EOF'
airmouse = true

[profiles.default]
scroll_speed = 1.0

[profiles.default.button_map]
"KEY_UP" = "KEY_VOLUMEUP"

[profiles.tv]
scroll_speed = 3.0

[profiles.tv.button_map]
"KEY_UP" = "KEY_HOME"
EOF

export LG_MAGIC_CONFIG_ROOT=$CFG
export LG_MAGIC_STATE_DIR=$STATE

# ------------------------------------------------------------------ #
# Fake remote + daemon (bus + uinput)                                #
# ------------------------------------------------------------------ #

$FAKE create > "$TMP/fake.out" 2>&1 &
FAKE_PID=$!
wait_for "fake create" "imu=" "$TMP/fake.out"
KBD=$(sed -n 's/^keyboard=//p' "$TMP/fake.out")
IMU=$(sed -n 's/^imu=//p' "$TMP/fake.out")
[ -n "$KBD" ] && [ -n "$IMU" ] || fail "fake: no keyboard=/imu= lines: $(cat "$TMP/fake.out")"
echo "fake: kbd=$KBD imu=$IMU"

$DAEMON --keyboard "$KBD" --config-root "$CFG" --state-dir "$STATE" \
	--debug > "$TMP/daemon.log" 2>&1 &
DAEMON_PID=$!
DAEMON_LOG=$TMP/daemon.log
wait_for "daemon startup" "virtual devices ready" "$TMP/daemon.log"
wait_for "daemon bus" "bus name org.lgmagic acquired" "$TMP/daemon.log"
wait_for "daemon takeover" "remote unknown: keyboard" "$TMP/daemon.log"

OUTK=$(find_node_prefix "lg-magicd keyboard")
OUTM=$(find_node_prefix "lg-magicd mouse")
[ -n "$OUTK" ] || fail "'lg-magicd keyboard' output node not found"
[ -n "$OUTM" ] || fail "'lg-magicd mouse' output node not found"
echo "daemon: outk=$OUTK outm=$OUTM"

# ------------------------------------------------------------------ #
# 1. Unprivileged reads                                              #
# ------------------------------------------------------------------ #

[ "$("$BIN" device list)" = "unknown" ] || \
	fail "device list: expected 'unknown', got: $("$BIN" device list)"
echo "OK device list"

"$BIN" device status unknown > "$TMP/status.out"
grep -q "identity: unknown" "$TMP/status.out" || fail "status: $(cat "$TMP/status.out")"
grep -q "airmouse: on" "$TMP/status.out" || fail "status: $(cat "$TMP/status.out")"
grep -q "calib_loaded: yes" "$TMP/status.out" || fail "status: $(cat "$TMP/status.out")"
echo "OK device status"

"$BIN" profile list > "$TMP/pl.out"
grep -q "default (active)" "$TMP/pl.out" || fail "profile list: $(cat "$TMP/pl.out")"
grep -q "tv" "$TMP/pl.out" || fail "profile list: $(cat "$TMP/pl.out")"
echo "OK profile list (direct file read)"

"$BIN" button list unknown > "$TMP/bl.out"
grep -q "KEY_UP -> KEY_VOLUMEUP" "$TMP/bl.out" || fail "button list: $(cat "$TMP/bl.out")"
echo "OK button list (direct file read)"

# The same reads as nobody: read-only methods must stay polkit-free.
[ "$(setpriv --reuid=nobody --regid=nogroup --clear-groups \
	"$BIN" device list)" = "unknown" ] || \
	fail "device list as nobody: read must be polkit-free"
setpriv --reuid=nobody --regid=nogroup --clear-groups \
	"$BIN" device status unknown > "$TMP/status-nobody.out" || \
	fail "device status as nobody: read must be polkit-free"
grep -q "identity: unknown" "$TMP/status-nobody.out" || \
	fail "device status as nobody: $(cat "$TMP/status-nobody.out")"
echo "OK polkit-free reads as nobody"

# ------------------------------------------------------------------ #
# 1b. Harness self-test: injection through the evdev NODE must reach
#     a watcher (the same node/queue/watcher path the checks below
#     rely on - if this fails, the harness itself is broken).
# ------------------------------------------------------------------ #

expect_emit "harness self-test" "$OUTK" --kbd "$OUTK" --key KEY_ENTER \
	"KEY KEY_ENTER 1" 1200

# ------------------------------------------------------------------ #
# 2. The file's default profile is live at startup                   #
# ------------------------------------------------------------------ #

expect_emit "startup map" "$OUTK" --kbd "$KBD" --key KEY_UP \
	"KEY KEY_VOLUMEUP 1" 1200

# ------------------------------------------------------------------ #
# 3. button map (polkit modify-input): root OK, nobody denied          #
# ------------------------------------------------------------------ #

"$BIN" button map unknown KEY_DOWN KEY_BACK || fail "button map as root"
# the daemon's TOML writer emits bare-safe keys unquoted
grep -q 'KEY_DOWN' "$CFG/devices.d/unknown.toml" || \
	fail "button map: devices.d not updated: $(cat "$CFG/devices.d/unknown.toml")"
expect_emit "mapped KEY_DOWN" "$OUTK" --kbd "$KBD" --key KEY_DOWN \
	"KEY KEY_BACK 1" 1200

err=$(setpriv --reuid=nobody --regid=nogroup --clear-groups \
	"$BIN" button map unknown KEY_ENTER KEY_HOME 2>&1) && \
	fail "button map as nobody: expected rc 1"
echo "$err" | grep -q "permission denied (polkit)" || \
	fail "button map as nobody: expected NotAuthorized, got: $err"
echo "OK polkit denies nobody (modify-input)"

# ------------------------------------------------------------------ #
# 4. profile set: state.toml + the tv profile's map/scroll           #
# ------------------------------------------------------------------ #

"$BIN" profile set unknown tv || fail "profile set as root"
grep -q 'profile = "tv"' "$STATE/state.toml" || \
	fail "profile set: state.toml: $(cat "$STATE/state.toml")"
expect_emit "tv map" "$OUTK" --kbd "$KBD" --key KEY_UP "KEY KEY_HOME 1" 1200

# wheel 2 x tv scroll_speed 3.0 -> REL_WHEEL 6 (+720 hires)
expect_emit "tv scroll" "$OUTM" --kbd "$KBD" --wheel 2 \
	"REL_WHEEL 6$" 1200
grep -q "REL_WHEEL_HI_RES 720$" "$last_exp_file" || \
	fail "tv scroll: expected REL_WHEEL_HI_RES 720, got: $(cat "$last_exp_file")"
echo "OK tv scroll hires"

err=$(setpriv --reuid=nobody --regid=nogroup --clear-groups \
	"$BIN" profile set unknown default 2>&1) && \
	fail "profile set as nobody: expected rc 1"
echo "$err" | grep -q "permission denied (polkit)" || \
	fail "profile set as nobody: expected NotAuthorized, got: $err"
echo "OK polkit denies nobody (profile-set)"

"$BIN" profile set unknown default || fail "profile set back"
expect_emit "default map again" "$OUTK" --kbd "$KBD" --key KEY_UP \
	"KEY KEY_VOLUMEUP 1" 1200

# ------------------------------------------------------------------ #
# 4b. held key across a remap: the old virtual key must not stick     #
# ------------------------------------------------------------------ #
# Press KEY_UP (current map: KEY_VOLUMEUP), remap it to KEY_HOME while
# still held, then release.  The daemon must release KEY_VOLUMEUP when
# the map changes (pipeline_configure drops every held key), and the
# held press must not leak into the new map as a KEY_HOME press.
( "$FAKE" watch "$OUTK" --ms 4000 > "$TMP/wheld.out" 2>&1 ) &
wheld=$!
sleep 0.1
"$FAKE" emit --kbd "$KBD" --press KEY_UP
sleep 0.3
"$BIN" button map unknown KEY_UP KEY_HOME || fail "button map mid-press"
sleep 0.3
"$FAKE" emit --kbd "$KBD" --release KEY_UP
sleep 0.7
kill "$wheld" 2>/dev/null || true
wait "$wheld" 2>/dev/null || true
grep -q "KEY KEY_VOLUMEUP 1" "$TMP/wheld.out" || \
	fail "held key: no KEY_VOLUMEUP press: $(cat "$TMP/wheld.out")"
grep -q "KEY KEY_VOLUMEUP 0" "$TMP/wheld.out" || \
	fail "held key: KEY_VOLUMEUP never released (stuck): $(cat "$TMP/wheld.out")"
grep -q "KEY KEY_HOME 1" "$TMP/wheld.out" && \
	fail "held key: the held press leaked into the new map: $(cat "$TMP/wheld.out")"
expect_emit "remap after release" "$OUTK" --kbd "$KBD" --key KEY_UP \
	"KEY KEY_HOME 1" 1200
# restore KEY_UP -> KEY_VOLUMEUP: section 6's button reset must still
# find a map to clear (a reset on an already-empty map proves nothing)
"$BIN" button map unknown KEY_UP KEY_VOLUMEUP || \
	fail "button map restore after held-key test"
expect_emit "restore map after held-key" "$OUTK" --kbd "$KBD" --key KEY_UP \
	"KEY KEY_VOLUMEUP 1" 1200
echo "OK held key across remap"

# ------------------------------------------------------------------ #
# 5. scroll speed / sensitivity / reset                              #
# ------------------------------------------------------------------ #

"$BIN" scroll speed unknown 2.0 || fail "scroll speed"
expect_emit "scroll 2.0" "$OUTM" --kbd "$KBD" --wheel 2 "REL_WHEEL 4$" 1200

# sensitivity 60: dx = int(0.349066 * 60) = 20
"$BIN" scroll sensitivity unknown 60.0 || fail "scroll sensitivity"
expect_emit "sensitivity 60" "$OUTM" --imu "$IMU" --gyro 0,0,100 \
	"REL_X 20$" 1500

"$BIN" scroll reset unknown || fail "scroll reset"
expect_emit "scroll reset" "$OUTM" --kbd "$KBD" --wheel 2 "REL_WHEEL 2$" 1200
# sensitivity back to 30: dx = int(0.349066 * 30) = 10
expect_emit "sensitivity reset" "$OUTM" --imu "$IMU" --gyro 0,0,100 \
	"REL_X 10$" 1500

# ------------------------------------------------------------------ #
# 6. button reset: the default map is gone, KEY_UP passes through    #
# ------------------------------------------------------------------ #

"$BIN" button reset unknown || fail "button reset"
# only the active (default) profile is cleared - tv keeps its map
# (the daemon's TOML writer emits bare-safe keys unquoted)
grep -q 'KEY_UP = "KEY_VOLUMEUP"' "$CFG/devices.d/unknown.toml" && \
	fail "button reset: default profile still maps KEY_UP"
grep -q 'KEY_UP = "KEY_HOME"' "$CFG/devices.d/unknown.toml" || \
	fail "button reset: the tv profile map must survive: $(cat "$CFG/devices.d/unknown.toml")"
"$BIN" button list unknown > "$TMP/bl2.out"
grep -q "(no buttons mapped" "$TMP/bl2.out" || \
	fail "button reset: button list: $(cat "$TMP/bl2.out")"
expect_emit "reset passthrough" "$OUTK" --kbd "$KBD" --key KEY_UP \
	"KEY KEY_UP 1" 1200

# ------------------------------------------------------------------ #
# 6b. An invalid calibration file is rejected at reload              #
# ------------------------------------------------------------------ #

# scale [1,1,1] is live (dx = int(0.349066 * 30) = 10).  Point the device
# at a broken JSON, reload: the daemon must log the rejection, keep the
# previous calibration and stay alive.
BADCAL=$TMP/bad.json
printf '{"gyro": {' > "$BADCAL"
dbus-send --system --print-reply --dest=org.lgmagic /org/lgmagic/Manager \
	org.lgmagic.Manager.SetCalibPath string:unknown string:"$BADCAL" \
	>/dev/null || fail "SetCalibPath(bad) via dbus-send"
dbus-send --system --print-reply --dest=org.lgmagic /org/lgmagic/Manager \
	org.lgmagic.Manager.Reload >/dev/null || fail "Reload(bad) via dbus-send"
grep -q "calibration reload for unknown" "$TMP/daemon.log" || \
	fail "bad calib: no rejection logged: $(tail -5 "$TMP/daemon.log")"
[ "$("$BIN" device list)" = "unknown" ] || fail "bad calib: daemon not alive"
expect_emit "bad calib rejected" "$OUTM" --imu "$IMU" --gyro 0,0,100 \
	"REL_X 10$" 1500

# ------------------------------------------------------------------ #
# 7. SetCalibPath + Reload: a new calibration applies live           #
# ------------------------------------------------------------------ #

# scale 0.5 -> g_corr z = -0.872665 -> filt = -0.174533 ->
# dx = int(0.174533 * 30) = 5
CAL2=$TMP/calib2.json
cat > "$CAL2" <<'EOF'
{
    "gyro": {
        "bias": [0.0, 0.0, 0.0],
        "scale": [0.5, 0.5, 0.5]
    }
}
EOF

dbus-send --system --print-reply --dest=org.lgmagic /org/lgmagic/Manager \
	org.lgmagic.Manager.SetCalibPath string:unknown string:"$CAL2" \
	>/dev/null || fail "SetCalibPath via dbus-send"
grep -q "calib = \"$CAL2\"" "$CFG/devices.d/unknown.toml" || \
	fail "SetCalibPath: devices.d not updated"

dbus-send --system --print-reply --dest=org.lgmagic /org/lgmagic/Manager \
	org.lgmagic.Manager.Reload >/dev/null || fail "Reload via dbus-send"

expect_emit "new calibration" "$OUTM" --imu "$IMU" --gyro 0,0,100 \
	"REL_X 5$" 1500

# ------------------------------------------------------------------ #
# 8. Standalone `lg-magic imu` in parallel (IMU not grabbed)         #
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
grep -q "100" "$TMP/csv.out" || fail "standalone imu: raw gyro not in CSV"
echo "OK standalone imu"

# ------------------------------------------------------------------ #
# 9. Reconnect: the daemon re-adds the remote, config re-applies     #
# ------------------------------------------------------------------ #

kill "$FAKE_PID" 2>/dev/null || true
wait "$FAKE_PID" 2>/dev/null || true
FAKE_PID=
sleep 0.5

$FAKE create > "$TMP/fake2.out" 2>&1 &
FAKE_PID=$!
wait_for "fake restart" "imu=" "$TMP/fake2.out"
KBD2=$(sed -n 's/^keyboard=//p' "$TMP/fake2.out")
echo "fake restart: kbd=$KBD2"
[ "$KBD2" = "$KBD" ] || echo "NOTE: event numbers changed ($KBD -> $KBD2); the daemon is pinned to the old path - this check may fail"

i=0
while [ $i -lt 60 ]; do
	[ "$(grep -c "remote unknown: keyboard" "$TMP/daemon.log")" -ge 2 ] && break
	i=$((i + 1))
	sleep 0.1
done
[ "$(grep -c "remote unknown: keyboard" "$TMP/daemon.log")" -ge 2 ] || \
	fail "reconnect: daemon did not re-add the remote"
[ "$("$BIN" device list)" = "unknown" ] || fail "reconnect: device list"
expect_emit "reconnect passthrough" "$OUTK" --kbd "$KBD2" --key KEY_UP \
	"KEY KEY_UP 1" 1200
echo "OK reconnect"

# ------------------------------------------------------------------ #
# 10. Grab active / released after SIGKILL                           #
# ------------------------------------------------------------------ #

if [ "$KBD2" != "$KBD" ]; then
	echo "NOTE: event numbers changed on reconnect - skipping the grab"
	echo "checks (the daemon is pinned to $KBD via --keyboard)"
	echo "E2E PASS"
	exit 0
fi

( "$FAKE" watch "$KBD2" --ms 600 > "$TMP/wg1.out" 2>&1 ) &
w1=$!
sleep 0.1
"$FAKE" emit --kbd "$KBD2" --key KEY_ENTER
sleep 0.7
kill "$w1" 2>/dev/null || true
wait "$w1" 2>/dev/null || true
[ -s "$TMP/wg1.out" ] && fail "grab: raw events leaked while grabbed"
echo "OK grab active"

kill -9 "$DAEMON_PID" 2>/dev/null || true
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=
sleep 0.3

( "$FAKE" watch "$KBD2" --ms 600 > "$TMP/wg2.out" 2>&1 ) &
w2=$!
sleep 0.1
"$FAKE" emit --kbd "$KBD2" --key KEY_ENTER
sleep 0.7
kill "$w2" 2>/dev/null || true
wait "$w2" 2>/dev/null || true
grep -q "KEY KEY_ENTER 1" "$TMP/wg2.out" || \
	fail "grab release: no raw events after SIGKILL"
echo "OK grab release"

# ------------------------------------------------------------------ #
# 11. Grab released after SIGTERM (clean shutdown)                   #
# 12. Grab released after SIGINT (clean shutdown)                    #
# ------------------------------------------------------------------ #

start_daemon()
{
	$DAEMON --keyboard "$KBD2" --config-root "$CFG" --state-dir "$STATE" \
		--debug > "$TMP/daemon2.log" 2>&1 &
	DAEMON_PID=$!
	DAEMON_LOG=$TMP/daemon2.log
	wait_for "daemon restart" "virtual devices ready" "$TMP/daemon2.log"
	wait_for "daemon bus" "bus name org.lgmagic acquired" "$TMP/daemon2.log"
	wait_for "daemon takeover" "remote unknown: keyboard" "$TMP/daemon2.log"
}

check_grab_released()
{
	sig=$1
	kill "-$sig" "$DAEMON_PID" 2>/dev/null || true
	wait "$DAEMON_PID" 2>/dev/null || true
	DAEMON_PID=
	sleep 0.3
	( "$FAKE" watch "$KBD2" --ms 600 > "$TMP/wg3.out" 2>&1 ) &
	w3=$!
	sleep 0.1
	"$FAKE" emit --kbd "$KBD2" --key KEY_ENTER
	sleep 0.7
	kill "$w3" 2>/dev/null || true
	wait "$w3" 2>/dev/null || true
	grep -q "KEY KEY_ENTER 1" "$TMP/wg3.out" || \
		fail "grab release: no raw events after SIG$sig"
	echo "OK grab release after SIG$sig"
}

start_daemon
check_grab_released TERM
start_daemon
check_grab_released INT

echo "E2E PASS"
