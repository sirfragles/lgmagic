#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# check_version_sync.sh - guard: every version stamp site must agree.
#
# The stamp sites are dkms.conf PACKAGE_VERSION,
# tools/include/build_version.h LGMAGIC_VERSION, the top line of
# debian/changelog ("lgmagic (X-Y)"), rpm/lgmagic.spec Version: and
# arch/PKGBUILD pkgver=; tools/include/daemon_bus.h LG_API_VERSION must
# equal MAJOR.MINOR of that shared version.
#
# This guard does four things:
#   (a) runs scripts/sync-version.sh --check (exit 0 = all sites agree
#       on a MAJOR.MINOR.PATCH version);
#   (b) re-verifies the sites INDEPENDENTLY with its own parsing (a
#       second implementation must agree with the script, so a bug in
#       either cannot pass unnoticed);
#   (c) hermetic self-tests in a temp copy of the stamped files plus
#       sync-version.sh (the real tree is never written to):
#         - idempotency: running the sync twice leaves identical files;
#         - --check detects a drifted site BEFORE any resync (exit 1);
#         - the sync heals the drift (all sites agree again);
#   (d) when a Linux ./tools/lgmagic binary is present, its --version
#       output must be exactly "lgmagic <version from dkms.conf>".
#
# POSIX sh, no prerequisites beyond bash (scripts/sync-version.sh is a
# bash script); a missing bash or a missing site file is a FAIL, never
# a silent skip.

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/../.." && pwd)

[ -d "$ROOT" ] || { echo "FAIL tests/check_version_sync: repo root $ROOT missing"; exit 1; }
command -v bash >/dev/null 2>&1 || {
	echo "FAIL tests/check_version_sync: bash not found (scripts/sync-version.sh is bash)"
	exit 1
}

SEMVER='^[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*$'

fail()
{
	echo "FAIL tests/check_version_sync: $*"
	exit 1
}

# read_ver FILE dkms|buildver|apiver|changelog|spec|pkgbuild - the
# version string found at that site ("" if absent).
read_ver()
{
	case "$2" in
	dkms)      sed -n 's/^PACKAGE_VERSION="\([^"]*\)".*/\1/p' "$1" | head -n1 ;;
	buildver)  sed -n 's/^#define LGMAGIC_VERSION "\([^"]*\)".*/\1/p' "$1" | head -n1 ;;
	apiver)    sed -n 's/^#define LG_API_VERSION "\([^"]*\)".*/\1/p' "$1" | head -n1 ;;
	changelog) sed -n '1s/^lgmagic (\([^)]*\)).*/\1/p' "$1" | sed 's/-[0-9][0-9]*$//' | head -n1 ;;
	spec)      sed -n 's/^Version:[[:space:]]*\(.*\)/\1/p' "$1" | head -n1 ;;
	pkgbuild)  sed -n 's/^pkgver=\(.*\)/\1/p' "$1" | head -n1 ;;
	*) fail "read_ver: unknown site $2" ;;
	esac
}

# ------------------------------------------------------------------ #
# (a) scripts/sync-version.sh --check must pass                        #
# ------------------------------------------------------------------ #

if ! bash "$ROOT/scripts/sync-version.sh" --check >/dev/null 2>&1; then
	echo "FAIL tests/check_version_sync: scripts/sync-version.sh --check reports drift:"
	bash "$ROOT/scripts/sync-version.sh" --check 2>&1 | sed 's/^/    /'
	exit 1
fi
echo "OK  scripts/sync-version.sh --check: stamp sites agree"

# ------------------------------------------------------------------ #
# (b) independent re-verification of every site                        #
# ------------------------------------------------------------------ #

DKMS=$ROOT/dkms.conf
BUILD_H=$ROOT/tools/include/build_version.h
DBUS_H=$ROOT/tools/include/daemon_bus.h
CHANGELOG=$ROOT/debian/changelog
SPEC=$ROOT/rpm/lgmagic.spec
PKGBUILD=$ROOT/arch/PKGBUILD

for f in "$DKMS" "$BUILD_H" "$DBUS_H" "$CHANGELOG" "$SPEC" "$PKGBUILD"; do
	[ -f "$f" ] || fail "missing stamp site $f"
done

v_dkms=$(read_ver "$DKMS" dkms)
v_build=$(read_ver "$BUILD_H" buildver)
v_deb=$(read_ver "$CHANGELOG" changelog)
v_spec=$(read_ver "$SPEC" spec)
v_arch=$(read_ver "$PKGBUILD" pkgbuild)
v_api=$(read_ver "$DBUS_H" apiver)

bad=""
for v in "$v_dkms" "$v_build" "$v_deb" "$v_spec" "$v_arch"; do
	printf '%s\n' "$v" | grep -q "$SEMVER" || bad=1
done
if [ -z "$bad" ] \
   && [ "$v_dkms" = "$v_build" ] \
   && [ "$v_build" = "$v_deb" ] \
   && [ "$v_deb" = "$v_spec" ] \
   && [ "$v_spec" = "$v_arch" ] \
   && [ "$v_api" = "${v_dkms%.*}" ]; then
	echo "OK  independent site check: all five stamps are $v_dkms, LG_API_VERSION $v_api"
else
	echo "FAIL tests/check_version_sync: independent site check found drift:"
	printf '    dkms.conf:                      %s\n' "$v_dkms"
	printf '    tools/include/build_version.h:  %s\n' "$v_build"
	printf '    debian/changelog (top entry):   %s\n' "$v_deb"
	printf '    rpm/lgmagic.spec (Version):     %s\n' "$v_spec"
	printf '    arch/PKGBUILD (pkgver):         %s\n' "$v_arch"
	printf '    LG_API_VERSION (want %s):       %s\n' "${v_dkms%.*}" "$v_api"
	exit 1
fi

# ------------------------------------------------------------------ #
# (c) hermetic self-tests in a temp copy (real tree is never written) #
# ------------------------------------------------------------------ #

TMP=
# shellcheck disable=SC2317 # cleanup() is invoked only via the trap below
cleanup()
{
	[ -n "$TMP" ] && rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

TMP=$(mktemp -d "${TMPDIR:-/tmp}/lgmagic-versync.XXXXXX") || \
	fail "cannot create a temp directory for the self-tests"
mkdir -p "$TMP/scripts" "$TMP/tools/include" "$TMP/debian" "$TMP/rpm" "$TMP/arch" || \
	fail "cannot populate the temp tree"
cp "$ROOT/dkms.conf" "$TMP/dkms.conf" || fail "cp dkms.conf"
cp "$ROOT/tools/include/build_version.h" "$TMP/tools/include/" || fail "cp build_version.h"
cp "$ROOT/tools/include/daemon_bus.h" "$TMP/tools/include/" || fail "cp daemon_bus.h"
cp "$ROOT/debian/changelog" "$TMP/debian/changelog" || fail "cp debian/changelog"
cp "$ROOT/rpm/lgmagic.spec" "$TMP/rpm/lgmagic.spec" || fail "cp rpm/lgmagic.spec"
cp "$ROOT/arch/PKGBUILD" "$TMP/arch/PKGBUILD" || fail "cp arch/PKGBUILD"
cp "$ROOT/scripts/sync-version.sh" "$TMP/scripts/sync-version.sh" || fail "cp sync-version.sh"

STAMPS="dkms.conf tools/include/build_version.h tools/include/daemon_bus.h \
debian/changelog rpm/lgmagic.spec arch/PKGBUILD"

sums()
{
	# shellcheck disable=SC2086 # word-splitting is intended (STAMPS)
	(cd "$TMP" && cksum $STAMPS)
}

# idempotency: two sync runs must leave every stamped file identical.
( cd "$TMP" && bash scripts/sync-version.sh ) >/dev/null 2>&1 || \
	fail "sync run #1 in the temp copy failed"
sums_a=$(sums)
( cd "$TMP" && bash scripts/sync-version.sh ) >/dev/null 2>&1 || \
	fail "sync run #2 in the temp copy failed"
sums_b=$(sums)
[ "$sums_a" = "$sums_b" ] || fail "sync-version.sh is not idempotent (run #1 vs #2)"
echo "OK  idempotency: two sync runs leave identical stamp files"

# drift: corrupt one site, then --check must fail BEFORE any resync...
( cd "$TMP" && sed 's/^Version:[[:space:]]*/Version:        9.9.9/' \
	rpm/lgmagic.spec > rpm/lgmagic.spec.tmp && mv rpm/lgmagic.spec.tmp \
	rpm/lgmagic.spec ) || fail "cannot inject the drift into the temp copy"
if ( cd "$TMP" && bash scripts/sync-version.sh --check ) >/dev/null 2>&1; then
	fail "--check did NOT detect the drifted rpm/lgmagic.spec"
fi
echo "OK  --check detects a drifted site (exit 1 before any resync)"

# ...and one sync run (version defaults to the temp dkms.conf) must
# restore full agreement.
( cd "$TMP" && bash scripts/sync-version.sh ) >/dev/null 2>&1 || \
	fail "drift-heal sync run in the temp copy failed"
( cd "$TMP" && bash scripts/sync-version.sh --check ) >/dev/null 2>&1 || \
	fail "drift-heal: the stamp sites do not agree after the resync"
v_healed=$(read_ver "$TMP/rpm/lgmagic.spec" spec)
[ "$v_healed" = "$v_dkms" ] || \
	fail "drift-heal: spec Version is $v_healed, want $v_dkms"
echo "OK  drift-heal: a sync run restores agreement (all sites $v_dkms)"

# --check must also catch a drifted debian/changelog top entry (the
# changelog reader and the other sites agree on the same shape).
( cd "$TMP" && sed '1s/^lgmagic ([0-9.]*/lgmagic (9.9.9/' \
	debian/changelog > debian/changelog.tmp && mv debian/changelog.tmp \
	debian/changelog ) || fail "cannot inject the changelog drift"
if ( cd "$TMP" && bash scripts/sync-version.sh --check ) >/dev/null 2>&1; then
	fail "--check did NOT detect the drifted debian/changelog"
fi
echo "OK  --check detects a drifted debian/changelog top entry"

# ------------------------------------------------------------------ #
# (d) cross-check the built binary's --version (Linux builds only)    #
# ------------------------------------------------------------------ #

if [ -x "$ROOT/tools/lgmagic" ]; then
	vout=$("$ROOT/tools/lgmagic" --version 2>/dev/null) || \
		fail "$ROOT/tools/lgmagic --version failed (rc $?)"
	[ "$vout" = "lgmagic $v_dkms" ] || \
		fail "$ROOT/tools/lgmagic --version printed \"$vout\", want \"lgmagic $v_dkms\""
	echo "OK  $ROOT/tools/lgmagic --version: lgmagic $v_dkms"
else
	echo "note: tools/lgmagic is absent (Linux-only binary) - --version cross-check skipped"
fi

echo "PASS tests/check_version_sync"
exit 0
