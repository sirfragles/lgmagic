#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# cli_smoke.sh - smoke tests for the lgmagic multi-call binary (Linux).
#
# Usage: sh cli_smoke.sh /path/to/lgmagic
#
# Covers the top-level argument handling of main.c (help / version /
# unknown subcommand / global --config), the documented contract that every
# subcommand accepts its own --help, and the `config` subcommand end to end
# against a scratch HOME (it must never touch the real user config).
#
# The binary is Linux-only (evdev/hidraw/uinput) and is built by
# `make -C tools`.  If it has not been built, or the build cannot link on
# this host (e.g. macOS), the script reports SKIP and exits 0 so that
# `make -C tools check` still succeeds.
#
# All temporary files live under ${TMPDIR:-/tmp} and are removed on exit.
# Exit status: 0 = all checks passed (or skipped), 1 = a check failed.

# The assertion idiom `cond && pass "$name" || fail "$name"` is
# deliberate: pass() and fail() only count and print (they cannot fail),
# so the || arm can never run while cond is true.  SC2015 (info) warns
# about the idiom generically; it is not a bug here.
# shellcheck disable=SC2015

BIN=$1
[ -n "$BIN" ] || BIN=./lgmagic

if [ ! -x "$BIN" ]; then
	echo "SKIP tests/cli_smoke.sh: $BIN not found or not executable"
	echo "  (the lgmagic binary is Linux-only; run make -C tools to build it)"
	exit 0
fi

# A leftover binary from an older checkout (e.g. a v1 build) would fail
# every check below.  Skip unless it really is a fresh lgmagic binary
# (any semver — never a hardcoded version literal).
if ! "$BIN" --version 2>/dev/null | grep -qE '^lgmagic [0-9]+\.[0-9]+\.[0-9]+'; then
	echo "SKIP tests/cli_smoke.sh: $BIN is not an lgmagic binary"
	echo "  (stale build? rebuild with make -C tools on Linux)"
	exit 0
fi

TMP=${TMPDIR:-/tmp}/lgmagic-cli-smoke-$$
mkdir -p "$TMP/home"
trap 'rm -rf "$TMP"' 0 1 2 3 15

ok=0
bad=0

pass()
{
	ok=$((ok + 1))
	echo "PASS $1"
}

fail()
{
	bad=$((bad + 1))
	echo "FAIL $1"
}

# check <name> <expected_exit> <actual_exit>
check_rc()
{
	[ "$3" -eq "$2" ] && pass "$1" || fail "$1 (exit $3, want $2)"
}

# run <name> <expected_exit> <glob-output-check> -- cmd args...
# runs the binary, records stdout/stderr, checks the exit code and, when the
# glob check is non-empty, that stdout matches it.
check()
{
	name=$1
	want_rc=$2
	want_out=$3
	shift 3
	[ "$1" = "--" ] && shift

	"$BIN" "$@" >"$TMP/out" 2>"$TMP/err"
	rc=$?
	if [ "$rc" -ne "$want_rc" ]; then
		fail "$name (exit $rc, want $want_rc)"
		return
	fi
	if [ -n "$want_out" ]; then
		if grep -q -e "$want_out" "$TMP/out"; then
			pass "$name"
		else
			fail "$name (stdout does not match '$want_out')"
		fi
	else
		pass "$name"
	fi
}

# check_err <name> <expected_exit> <stderr-check> -- cmd args...
check_err()
{
	name=$1
	want_rc=$2
	want_err=$3
	shift 3
	[ "$1" = "--" ] && shift

	"$BIN" "$@" >"$TMP/out" 2>"$TMP/err"
	rc=$?
	if [ "$rc" -ne "$want_rc" ]; then
		fail "$name (exit $rc, want $want_rc)"
		return
	fi
	if [ -n "$want_err" ]; then
		if grep -q -e "$want_err" "$TMP/err"; then
			pass "$name"
		else
			fail "$name (stderr does not match '$want_err')"
		fi
	else
		pass "$name"
	fi
}

# ----------------------------------------------------------------------
# Top level (main.c)
# ----------------------------------------------------------------------

check "--version prints the version and exits 0" 0 '^lgmagic ' -- --version
check "--help prints usage on stdout, exit 0" 0 'Usage: lgmagic' -- --help
check "-h is accepted like --help" 0 'Usage: lgmagic' -- -h

"$BIN" >"$TMP/out" 2>"$TMP/err"
check_rc "no arguments prints usage to stderr and exits 1" 1 $?
grep -q 'Usage: lgmagic' "$TMP/err" && pass "no-argument usage goes to stderr" ||
	fail "no-argument usage goes to stderr"

check_err "unknown subcommand is rejected" 1 "unknown subcommand 'frobnicate'" \
	-- frobnicate

check_err "--config without a file argument is rejected" 1 \
	"--config needs a file argument" -- --config

# every documented subcommand appears in the top-level help
for c in analyze imu calibrate calib2bin config setup; do
	"$BIN" --help | grep -q "^  $c " && pass "help lists subcommand $c" ||
		fail "help lists subcommand $c"
done

# ----------------------------------------------------------------------
# Subcommand --help contract: each command accepts its own --help,
# prints usage on stdout and exits 0 (see the subcommand help comment in
# main.c).  Unknown sub-subcommands must exit nonzero.
# ----------------------------------------------------------------------

for c in analyze imu calibrate calib2bin config setup; do
	"$BIN" "$c" --help >"$TMP/out" 2>"$TMP/err"
	rc=$?
	if [ "$rc" -eq 0 ] && [ -s "$TMP/out" ]; then
		pass "$c --help exits 0 with usage on stdout"
	else
		fail "$c --help exits 0 with usage on stdout (rc=$rc)"
	fi
done

# ----------------------------------------------------------------------
# config subcommand against a scratch HOME (never the real user config)
# ----------------------------------------------------------------------

# pristine: all defaults, nothing marked (from config)
HOME="$TMP/home" "$BIN" config >"$TMP/out" 2>"$TMP/err"
rc=$?
if [ "$rc" -eq 0 ] && grep -q 'lpf_alpha' "$TMP/out" &&
	grep -q '(default)' "$TMP/out"; then
	pass "config prints the effective defaults on a pristine HOME"
else
	fail "config prints the effective defaults on a pristine HOME (rc=$rc)"
fi

HOME="$TMP/home" "$BIN" config path >"$TMP/out" 2>"$TMP/err"
rc=$?
grep -q "user:   $TMP/home/.config/lgmagic/config.toml" "$TMP/out" &&
	pass "config path reports the scratch user file" ||
	fail "config path reports the scratch user file (rc=$rc)"

HOME="$TMP/home" "$BIN" config set lpf_alpha 0.35 >"$TMP/out" 2>"$TMP/err"
rc=$?
if [ "$rc" -eq 0 ] && grep -q '^lpf_alpha = 0.35$' "$TMP/out"; then
	pass "config set echoes 'lpf_alpha = 0.35'"
else
	fail "config set echoes 'lpf_alpha = 0.35' (rc=$rc)"
fi
[ -f "$TMP/home/.config/lgmagic/config.toml" ] &&
	pass "config set wrote ~/.config/lgmagic/config.toml" ||
	fail "config set wrote ~/.config/lgmagic/config.toml"

HOME="$TMP/home" "$BIN" config >"$TMP/out" 2>"$TMP/err"
rc=$?
grep -q '0.35' "$TMP/out" && grep -q '(from config)' "$TMP/out" &&
	pass "a fresh config run picks the saved value up" ||
	fail "a fresh config run picks the saved value up (rc=$rc)"

HOME="$TMP/home" "$BIN" config set no_such_key 1 >"$TMP/out" 2>"$TMP/err"
check_rc "config set with an unknown key exits 1" 1 $?
HOME="$TMP/home" "$BIN" config set lpf_alpha abc >"$TMP/out" 2>"$TMP/err"
check_rc "config set with a non-numeric value exits 1" 1 $?
HOME="$TMP/home" "$BIN" config set lpf_alpha >"$TMP/out" 2>"$TMP/err"
check_rc "config set with a missing VALUE exits 1" 1 $?
HOME="$TMP/home" "$BIN" config bogus >"$TMP/out" 2>"$TMP/err"
check_rc "config with an unknown argument exits 1" 1 $?
HOME="$TMP/home" "$BIN" config show extra >"$TMP/out" 2>"$TMP/err"
check_rc "config show with extra arguments exits 1" 1 $?

# ----------------------------------------------------------------------
# config migrate: v1 JSON -> TOML, the JSON source is kept
# ----------------------------------------------------------------------

printf '%s\n' '{"lpf_alpha": 0.4, "mouse_k": 0.9, "bogus": 1}' >"$TMP/mig.json"

HOME="$TMP/home" "$BIN" config migrate "$TMP/mig.json" >"$TMP/out" 2>"$TMP/err"
rc=$?
if [ "$rc" -eq 0 ] && grep -q "migrated $TMP/mig.json -> $TMP/mig.toml" "$TMP/out"; then
	pass "config migrate converts a v1 JSON file"
else
	fail "config migrate converts a v1 JSON file (rc=$rc)"
fi
if [ -f "$TMP/mig.toml" ] &&
	grep -q '^lpf_alpha = 0.4$' "$TMP/mig.toml" &&
	grep -q '^mouse_k = 0.9$' "$TMP/mig.toml" &&
	! grep -q 'bogus' "$TMP/mig.toml"; then
	pass "migrated TOML has the known keys and drops unknown ones"
else
	fail "migrated TOML has the known keys and drops unknown ones"
fi
[ -f "$TMP/mig.json" ] && pass "migrate keeps the JSON source" ||
	fail "migrate keeps the JSON source"

HOME="$TMP/home" "$BIN" config migrate "$TMP/mig.json" >"$TMP/out" 2>"$TMP/err"
rc=$?
grep -q 'already exists' "$TMP/out" && [ "$rc" -eq 0 ] &&
	pass "a second migrate skips the existing .toml" ||
	fail "a second migrate skips the existing .toml (rc=$rc)"

# bare 'config migrate': the user file is already TOML, nothing to do
HOME="$TMP/home" "$BIN" config migrate >"$TMP/out" 2>"$TMP/err"
check_rc "bare 'config migrate' is a no-op here, exit 0" 0 $?

# a leftover v1 JSON next to a MISSING .toml produces a migrate hint
mkdir -p "$TMP/home2/.config/lgmagic"
printf '%s\n' '{"lpf_alpha": 0.4}' >"$TMP/home2/.config/lgmagic/config.json"
HOME="$TMP/home2" "$BIN" config >"$TMP/out" 2>"$TMP/err"
grep -q 'config migrate' "$TMP/err" &&
	pass "a leftover v1 JSON produces a migrate hint on stderr" ||
	fail "a leftover v1 JSON produces a migrate hint on stderr"
rm -rf "$TMP/home2"

# ----------------------------------------------------------------------
# Global --config FILE: merged after the user file, in either position
# ----------------------------------------------------------------------

printf '%s\n' 'mouse_k = 0.9' >"$TMP/extra.toml"

"$BIN" --config "$TMP/extra.toml" config >"$TMP/out" 2>"$TMP/err"
rc=$?
grep -q 'mouse_k' "$TMP/out" && grep -q '0.9' "$TMP/out" &&
	pass "--config FILE (before the command) overrides mouse_k" ||
	fail "--config FILE (before the command) overrides mouse_k (rc=$rc)"

"$BIN" config --config "$TMP/extra.toml" >"$TMP/out" 2>"$TMP/err"
rc=$?
grep -q '0.9' "$TMP/out" && pass "--config FILE after the command works too" ||
	fail "--config FILE after the command works too (rc=$rc)"

"$BIN" --config "$TMP/does-not-exist.toml" config >"$TMP/out" 2>"$TMP/err"
check_rc "a missing --config file is skipped, not fatal" 0 $?

echo "cli_smoke: $ok passed, $bad failed"
[ "$bad" -eq 0 ]
