#!/bin/sh
# t/run.sh -- build and run every oracle, print the conformance score and
# "N passed, M failed".
# SPDX-License-Identifier: GPL-3.0-or-later
#
# POSIX sh.  This script, not the Makefile, compiles the test programs, so
# `make test-asan` is the same script with different CFLAGS.
set -u

cd "$(dirname "$0")/.." || exit 1
CC=${CC:-gcc}
CFLAGS=${CFLAGS:--std=c11 -Wall -Wextra -Wpedantic -O2 -g}

mkdir -p scratch			# repo-local: /tmp is shared with other work
OUT=scratch/run.log
: > "$OUT"

say() { printf '%s\n' "$*" | tee -a "$OUT"; }

build() {
	out=$1
	shift
	# shellcheck disable=SC2086
	if ! $CC $CFLAGS -o "$out" "$@"; then
		say "not ok - compile $out"
		return 1
	fi
}

printf '=== building (CC=%s)\n' "$CC"
build ./t/unit t/unit.c utf8n.c || exit 1

printf '\n--- unit assertions (UTF-8 policy, ordering, Hangul, exclusions)\n'
./t/unit 2>&1 | tee -a "$OUT"

pass=$(grep -c '^ok - ' "$OUT")
fail=$(grep -c '^not ok - ' "$OUT")
printf '\n%s passed, %s failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
