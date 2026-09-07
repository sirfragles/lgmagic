#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# check_naming.sh - repo hygiene guard: the project rename is final.
#
# The project renamed its binary/daemon/module/macros to the prefix
# "lgmagic" (no separator).  This guard scans the whole repo tree for
# the three pre-rename token spellings (dash, underscore, and their
# uppercase form) with zero tolerance - there is no allowlist, so the
# scan covers packaging, docs and scripts alike.  The old package names
# may only live in the GitHub Release notes, never in a tracked file.
#
# ONE sanctioned exception: attribution of the upstream project this
# code is based on (the canonical URL github.com/brainrom/<old name>,
# composed at runtime below).  A hit line that also carries that URL is
# attribution, not a stale reference, and passes.  Anything else still
# fails, URL or not.
#
#   - content: every file under the repo root (excluding .git/,
#     testdata/ and receipts/) is grepped for the tokens;
#   - filenames: `git ls-files` must contain none of the tokens (a plain
#     find over the same exclusions covers a checkout without .git);
#   - every hit is printed as  path:linenr: text  and the exit status is
#     1; a clean tree prints a PASS line and exits 0.
#
# POSIX sh, no prerequisites, never silently SKIPs: an inability to
# scan is a FAIL, not a SKIP.  The scan covers this script itself, so
# the token spellings are composed at runtime and never appear in the
# source text.

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/../.." && pwd)

# Compose the tokens so this file cannot match itself: D is the dash
# spelling, U the underscore spelling, BIG the uppercase underscore
# spelling.
D=$(printf '%s-%s' lg magic)
U=$(printf '%s_%s' lg magic)
BIG=$(printf '%s' "$U" | tr '[:lower:]' '[:upper:]')
PAT=$(printf '%s|%s|%s' "$D" "$U" "$BIG")
# The upstream project's canonical URL: the one place the old spelling
# may legitimately appear (attribution), composed so this file cannot
# match itself.
UPSTR=$(printf '%s' "github.com/brainrom/$D")

[ -d "$ROOT" ] || { echo "FAIL tests/check_naming: repo root $ROOT missing"; exit 1; }

hits=0

# 1. content scan over the tree (binary files are skipped: their bytes
#    cannot be printed as path:linenr:text; the tracked-filename scan
#    below still covers binary names).
out=$(find "$ROOT" \
	\( -path "$ROOT/.git" -o -path "$ROOT/testdata" \
	   -o -path "$ROOT/receipts" \) -prune -o -type f -print 2>/dev/null | \
	while IFS= read -r f; do
		# Drop hit lines that also carry the upstream URL - they are
		# attribution of the project this code is based on (see the
		# header comment), not stale references to our own old names.
		grep -HInE "$PAT" "$f" 2>/dev/null | grep -vF "$UPSTR"
	done) || true
if [ -n "$out" ]; then
	printf '%s\n' "$out"
	hits=$((hits + $(printf '%s\n' "$out" | wc -l | tr -d ' ')))
fi

# 2. tracked-filename scan (`git ls-files`; plain find when no .git -
#    e.g. a source tarball checkout - so this never silently skips).
names=$(git ls-files 2>/dev/null | grep -E "$PAT") || \
	names=$(find "$ROOT" \
		\( -path "$ROOT/.git" -o -path "$ROOT/testdata" \
		   -o -path "$ROOT/receipts" \) -prune -o -type f -print 2>/dev/null | \
		grep -E "$PAT") || true
if [ -n "$names" ]; then
	printf 'file: %s\n' "$names" | sed "s|^file: $ROOT/|file: |"
	hits=$((hits + $(printf '%s\n' "$names" | wc -l | tr -d ' ')))
fi

if [ "$hits" -gt 0 ]; then
	echo "FAIL tests/check_naming: $hits stale token reference(s) found"
	exit 1
fi
echo "PASS tests/check_naming: no stale $D/$U/$BIG tokens in the tree or filenames"
exit 0
