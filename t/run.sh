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

# The UCD files are not committed (3MB of them) but every table-derived answer
# in this repo is only correct for one version, so fetch exactly the version the
# committed header was generated from, not "latest".
UCD_VER=$(sed -n 's/.*UTF8N_UCD_VERSION "\([0-9.]*\)".*/\1/p' utf8n_table.h)
PY_VER=$(python3 -c 'import unicodedata; print(unicodedata.unidata_version)' \
	2>/dev/null || echo none)

fetch() { # fetch <version> <dir> <file>...
	ver=$1 dir=$2
	shift 2
	mkdir -p "$dir"
	for f in "$@"; do
		[ -s "$dir/$f" ] && continue
		if ! curl -fsS --max-time 120 -o "$dir/$f" \
			"https://www.unicode.org/Public/$ver/ucd/$f"; then
			rm -f "$dir/$f"
			return 1
		fi
	done
}

printf '=== UCD %s (ours) vs %s (CPython)\n' "$UCD_VER" "$PY_VER"
if fetch "$UCD_VER" ucd UnicodeData.txt CompositionExclusions.txt \
	DerivedNormalizationProps.txt NormalizationTest.txt; then
	HAVE_UCD=1
else
	HAVE_UCD=0
	say "# WARNING: could not fetch the UCD; conformance oracle SKIPPED"
fi

printf '=== building (CC=%s)\n' "$CC"
build ./t/unit t/unit.c utf8n.c || exit 1
build ./t/conform t/conform.c utf8n.c || exit 1

printf '\n--- unit assertions (UTF-8 policy, ordering, Hangul, exclusions)\n'
./t/unit 2>&1 | tee -a "$OUT"

# Oracle 1: the official conformance file.  20 assertions per line that we did
# not write, plus idempotence, plus its second requirement over every codepoint
# it does not list.
if [ "$HAVE_UCD" = 1 ]; then
	printf '\n--- oracle 1: NormalizationTest.txt (UCD %s)\n' "$UCD_VER"
	./t/conform ucd/NormalizationTest.txt 2>&1 | tee -a "$OUT"
fi

# The committed header must be exactly what the generator produces from this
# UCD version: proof that the tables are reproducible rather than hand-edited,
# and it is the generator that runs the Full_Composition_Exclusion self-oracle.
if [ "$HAVE_UCD" = 1 ]; then
	printf '\n--- generated tables are reproducible from UCD %s\n' "$UCD_VER"
	if python3 tools/gen_tables.py ucd scratch/utf8n_table.h &&
		cmp -s scratch/utf8n_table.h utf8n_table.h; then
		say "ok - utf8n_table.h regenerates byte for byte from the UCD"
	else
		say "not ok - utf8n_table.h regenerates byte for byte from the UCD"
	fi
fi

pass=$(grep -c '^ok - ' "$OUT")
fail=$(grep -c '^not ok - ' "$OUT")
printf '\n%s passed, %s failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
