#!/bin/sh
# daemon_bus_smoke.sh - bus + polkit smoke WITHOUT devices (Phase 4 gate).
#
# Starts a throwaway system bus + polkitd, runs lgmagicd with
# --no-uinput (no /dev/uinput is needed), and asserts:
#   1. device list               -> empty, rc 0
#   2. device status UNKNOWN     -> NotFound
#   3. profile set as root       -> NotFound (polkit passed first)
#   4. profile set as nobody     -> NotAuthorized (polkit denies)
#   5. button map, bad keycode   -> InvalidArguments
# Requires root (the system bus, the policy install) - SKIPs otherwise,
# and SKIPs when dbus-daemon/polkitd are not installed.
set -u

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT/tools" || exit 1

command -v dbus-daemon >/dev/null 2>&1 || { echo "SKIP: no dbus-daemon"; exit 0; }
POLKITD=$(command -v polkitd 2>/dev/null || true)
[ -n "$POLKITD" ] || POLKITD=/usr/lib/polkit-1/polkitd
[ -x "$POLKITD" ] || { echo "SKIP: no polkitd"; exit 0; }
[ "$(id -u)" = 0 ] || { echo "SKIP: not root (needs a system bus)"; exit 0; }

TMP=$(mktemp -d) || exit 1
CFG=$TMP/config
STATE=$TMP/state
mkdir -p "$CFG" "$STATE"
DBUS_PID=
POLKIT_PID=
DAEMON_PID=

# shellcheck disable=SC2317 # cleanup() is invoked only via the trap below
cleanup() {
	[ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null
	[ -n "$POLKIT_PID" ] && kill "$POLKIT_PID" 2>/dev/null
	[ -n "$DBUS_PID" ] && kill "$DBUS_PID" 2>/dev/null
	rm -f /etc/dbus-1/system.d/org.lgmagic.conf
	rm -f /usr/share/polkit-1/actions/org.lgmagic.policy
	rm -rf "$TMP"
	wait 2>/dev/null
}
trap cleanup EXIT INT TERM

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

# Install the policy + dbus conf (this box is disposable).
cp "$ROOT/data/org.lgmagic.policy" /usr/share/polkit-1/actions/ || exit 1
cp "$ROOT/data/org.lgmagic.conf" /etc/dbus-1/system.d/ || exit 1

# A throwaway system bus.
mkdir -p /run/dbus
dbus-daemon --system --nofork --nopidfile &
DBUS_PID=$!
sleep 0.5
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/system_bus_socket

# polkitd (the daemon fails closed while it is absent, so it must be
# present for the authorized paths below).
"$POLKITD" --no-debug &
POLKIT_PID=$!
sleep 1

# The daemon without devices (and without uinput).
./lgmagicd --no-uinput --config-root "$CFG" --state-dir "$STATE" \
	--debug >"$TMP/daemon.log" 2>&1 &
DAEMON_PID=$!
i=0
while ! grep -q "bus name org.lgmagic acquired" "$TMP/daemon.log"; do
	sleep 0.2
	i=$((i+1))
	[ "$i" -lt 50 ] || fail "daemon did not acquire the bus: $(tail -5 "$TMP/daemon.log")"
done

BIN=./lgmagic

# 1. empty device list
out=$("$BIN" device list) || fail "device list: rc=$?"
[ -z "$out" ] || fail "device list: expected empty output, got '$out'"

# 2. GetStatus of an unknown device -> NotFound
err=$("$BIN" device status AA_BB_CC_DD_EE_FF 2>&1) && fail "status: expected rc 1"
echo "$err" | grep -q "no device" || fail "status: expected NotFound, got: $err"

# 3. SetProfile as root -> NotFound (proves the polkit gate passed
# before the device lookup)
err=$("$BIN" profile set AA_BB_CC_DD_EE_FF default 2>&1) && fail "set as root: expected rc 1"
echo "$err" | grep -q "no device" || fail "set as root: expected NotFound (authz OK), got: $err"

# 4. SetProfile as nobody -> NotAuthorized (polkit fails closed for a
# user without an active session)
err=$(setpriv --reuid=nobody --regid=nogroup --clear-groups \
	"$BIN" profile set AA_BB_CC_DD_EE_FF default 2>&1) && fail "set as nobody: expected rc 1"
echo "$err" | grep -q "permission denied (polkit)" || \
	fail "set as nobody: expected NotAuthorized, got: $err"

# 5. MapButton with an unknown keycode -> InvalidArguments
err=$("$BIN" button map AA_BB_CC_DD_EE_FF KEY_UP NOPE 2>&1) && fail "map: expected rc 1"
echo "$err" | grep -q "unknown keycode" || \
	fail "map: expected InvalidArguments, got: $err"

echo "BUS SMOKE PASS"
exit 0
