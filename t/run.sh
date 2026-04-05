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

conf_state='SKIPPED -- UCD not fetched'
diff_state='SKIPPED -- UCD not fetched'

printf '=== UCD %s (ours) vs %s (CPython)\n' "$UCD_VER" "$PY_VER"
if fetch "$UCD_VER" ucd UnicodeData.txt CompositionExclusions.txt \
	DerivedNormalizationProps.txt NormalizationTest.txt; then
	HAVE_UCD=1
else
	HAVE_UCD=0
fi

printf '=== building (CC=%s)\n' "$CC"
build ./t/unit t/unit.c utf8n.c || exit 1
build ./t/conform t/conform.c utf8n.c || exit 1
build ./t/nfx t/nfx.c utf8n.c || exit 1

printf '\n--- unit assertions (UTF-8 policy, ordering, Hangul, exclusions)\n'
./t/unit 2>&1 | tee -a "$OUT"

# Oracle 1: the official conformance file.  20 assertions per line that we did
# not write, plus idempotence, plus its second requirement over every codepoint
# it does not list.
if [ "$HAVE_UCD" = 1 ]; then
	printf '\n--- oracle 1: NormalizationTest.txt (UCD %s)\n' "$UCD_VER"
	./t/conform ucd/NormalizationTest.txt 2>&1 | tee -a "$OUT"
	conf_state=$(grep '^conformance score' "$OUT" | tail -1)
fi

# Oracle 3 and 4: CPython.  Works with no network, which is why it is the
# oracle that always runs.  It needs the UCD of CPython's own Unicode version
# as well as ours, to tell version skew from a real disagreement.
if [ "$PY_VER" != none ] && fetch "$PY_VER" "ucd-$PY_VER" UnicodeData.txt \
	DerivedNormalizationProps.txt; then
	printf '\n--- oracle 3+4: CPython unicodedata and its UTF-8 decoder\n'
	python3 t/diff.py ./t/nfx ucd "ucd-$PY_VER" 2>&1 | tee -a "$OUT"
	diff_state="ran against CPython UCD $PY_VER"
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

# Untrusted bytes, straight in.  Under `make test-asan` this is the run that
# matters: 400kB of urandom is overwhelmingly ill-formed UTF-8, and every
# property asserted here (sizing contract, well-formed output, idempotence,
# clean truncation) has to hold for input nobody validated.
printf '\n--- hostile input: 400kB of /dev/urandom through all four forms\n'
head -c 400000 /dev/urandom > scratch/urandom.bin
./t/nfx -r < scratch/urandom.bin 2>&1 | tee -a "$OUT"
printf '' | ./t/nfx -r 2>&1 | tee -a "$OUT"

pass=$(grep -c '^ok - ' "$OUT")
fail=$(grep -c '^not ok - ' "$OUT")

# A skipped oracle must never look like a passing one.  Both external oracles
# need the network, so print what actually ran, every time, next to the score.
printf '\n=== oracles\n'
printf '  1 NormalizationTest.txt   %s\n' "$conf_state"
printf '  2 Full_Composition_Exclusion via tools/gen_tables.py  %s\n' \
	"$([ "$HAVE_UCD" = 1 ] && echo "ran, table diffed byte for byte" \
		|| echo "SKIPPED -- UCD not fetched")"
printf '  3 CPython unicodedata     %s\n' "$diff_state"
printf '  4 unit assertions and hostile input   ran (no data needed)\n'
printf '\n%s passed, %s failed\n' "$pass" "$fail"

case "$conf_state$diff_state" in
*SKIPPED*)
	printf 'WARNING: an external oracle did not run, so this score proves\n'
	printf 'much less than a full run does.  Set UTF8N_REQUIRE_ORACLES=1 to\n'
	printf 'make that a failure instead of a warning.\n'
	[ "${UTF8N_REQUIRE_ORACLES:-0}" = 1 ] && exit 1
	;;
esac
[ "$fail" -eq 0 ]
