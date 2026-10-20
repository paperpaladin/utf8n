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
gen_state='SKIPPED -- UCD not fetched'
qc_state='SKIPPED -- t/diff.py did not run'

printf '=== UCD %s (ours) vs %s (CPython)\n' "$UCD_VER" "$PY_VER"
if fetch "$UCD_VER" ucd UnicodeData.txt CompositionExclusions.txt \
	DerivedNormalizationProps.txt NormalizationTest.txt; then
	HAVE_UCD=1
else
	HAVE_UCD=0
fi

printf '=== building (CC=%s)\n' "$CC"
build ./t/unit -DUTF8N_STATS t/unit.c utf8n.c || exit 1
build ./t/conform -DUTF8N_STATS t/conform.c utf8n.c || exit 1
build ./t/nfx t/nfx.c utf8n.c || exit 1

# Every oracle below runs through this, because the score is counted by
# grepping for "ok - " lines and the exit status used to go nowhere:
# `./t/conform ... 2>&1 | tee -a "$OUT"` discards it twice over, once
# because a pipeline's status is tee's and once because nothing looked.
#
# A test binary that dies partway leaves its earlier ok lines in the file,
# emits no "not ok", and the suite prints "N passed, 0 failed" with a
# smaller N that nothing asserts -- so the 806,840 conformance assertions
# can become a few dozen silently. Under test-asan it is worse: ASan exits
# 1, and that was the status being thrown away.
#
# tzf/t/run.sh next door already does it this way, including the
# ASAN_OPTIONS=exitcode= that makes a sanitizer report distinguishable from
# an ordinary failure.
runq() { # runq <label> <command...>
	label=$1
	shift
	_out=$("$@" 2>&1)
	_rc=$?
	printf '%s\n' "$_out" | tee -a "$OUT"
	if [ "$_rc" -ne 0 ]; then
		printf 'not ok - %s exited %s (crash, abort or sanitizer)\n' \
			"$label" "$_rc" | tee -a "$OUT"
	fi
}

printf '\n--- unit assertions (UTF-8 policy, ordering, Hangul, exclusions)\n'
runq "t/unit" ./t/unit

# Oracle 1: the official conformance file.  20 assertions per line that we did
# not write, plus idempotence, plus its second requirement over every codepoint
# it does not list.
if [ "$HAVE_UCD" = 1 ]; then
	printf '\n--- oracle 1: NormalizationTest.txt (UCD %s)\n' "$UCD_VER"
	runq "t/conform" ./t/conform ucd/NormalizationTest.txt
	conf_state=$(grep '^conformance score' "$OUT" | tail -1)
fi

# Oracle 3 and 4: CPython.  Works with no network, which is why it is the
# oracle that always runs.  It needs the UCD of CPython's own Unicode version
# as well as ours, to tell version skew from a real disagreement.
if [ "$PY_VER" != none ] && fetch "$PY_VER" "ucd-$PY_VER" UnicodeData.txt \
	DerivedNormalizationProps.txt; then
	printf '\n--- oracle 3+4: CPython unicodedata and its UTF-8 decoder\n'
	runq "t/diff.py" python3 t/diff.py ./t/nfx ucd "ucd-$PY_VER"
	diff_state="ran against CPython UCD $PY_VER"
	# The quick-check sweep lives in the same script but is not a CPython
	# oracle: it compares every codepoint against the UCD's own NFx_QC.
	# Take the whole line, "ok" or "not ok", so a failure cannot read as a
	# skip -- and so an empty grep cannot read as a pass, which is the bug
	# conf_state had.
	if qc_line=$(grep 'quick check of every codepoint' "$OUT"); then
		qc_state=$qc_line
	fi
fi

# The committed header must be exactly what the generator produces from this
# UCD version: proof that the tables are reproducible rather than hand-edited,
# and it is the generator that runs the Full_Composition_Exclusion self-oracle.
#
# Two different failures live here and they used to share one "not ok" line.
# The generator REFUSING (its derived Full_Composition_Exclusion set does not
# equal the UCD's, so it exits non-zero without writing a table) is a refusal:
# the oracle disagreed and we declined to answer.  A table that differs from
# the regenerated one is a wrong answer sitting in the repository.  Merging
# them hid which of the two had happened, and the second is the one that ships.
if [ "$HAVE_UCD" = 1 ]; then
	printf '\n--- generated tables are reproducible from UCD %s\n' "$UCD_VER"
	if gen_out=$(python3 tools/gen_tables.py ucd scratch/utf8n_table.h 2>&1); then
		printf '%s\n' "$gen_out" | sed 's/^/# /' | tee -a "$OUT"
		if cmp -s scratch/utf8n_table.h utf8n_table.h; then
			say "ok - utf8n_table.h regenerates byte for byte from the UCD"
			gen_state='ran, header matches the generator byte for byte'
		else
			say "not ok - utf8n_table.h DIFFERS from what tools/gen_tables.py produces (stale header, or hand-edited)"
			gen_state='ran, HEADER DOES NOT MATCH the generator'
		fi
	else
		printf '%s\n' "$gen_out" | sed 's/^/# /' | tee -a "$OUT"
		say "not ok - tools/gen_tables.py REFUSED to emit tables; no byte-for-byte comparison was made"
		gen_state='REFUSED -- the generator self-oracle disagreed with the UCD'
	fi
fi

# Untrusted bytes, straight in.  Under `make test-asan` this is the run that
# matters: 400kB of urandom is overwhelmingly ill-formed UTF-8, and every
# property asserted here (sizing contract, well-formed output, idempotence,
# clean truncation) has to hold for input nobody validated.
printf '\n--- hostile input: 400kB of /dev/urandom through all four forms\n'
head -c 400000 /dev/urandom > scratch/urandom.bin
runq "nfx -r urandom" sh -c './t/nfx -r < scratch/urandom.bin 2>&1'
runq "nfx -r empty" sh -c 'printf "" | ./t/nfx -r 2>&1'

# The sanitizer is only as good as the buffer it is watching.  This case runs
# under `make test-asan` alone, because a planted read past the end of a
# malloc'd buffer is invisible without ASan -- and it was invisible WITH ASan
# until t/nfx.c trimmed that buffer to the exact length of the input it read.
#
# ASAN_OPTIONS=exitcode=42 is the idea taken from tzf/t/run.sh next door: it
# makes "the sanitizer reported something" distinguishable from "the program
# died for any other reason", so this case cannot pass because the binary
# segfaulted or failed to build.
case "$CFLAGS" in
*sanitize*)
	printf '\n--- the planted over-read the sanitizer has to catch\n'
	if build ./t/nfx-overread -DUTF8N_PLANT_OVERREAD t/nfx.c utf8n.c; then
		head -c 1000 /dev/urandom > scratch/overread.bin
		ASAN_OPTIONS=exitcode=42 ./t/nfx-overread -r \
			< scratch/overread.bin >/dev/null 2>&1
		if [ $? -eq 42 ]; then
			say "ok - ASan sees a read one byte past the slurped input buffer, so that buffer is exactly sized"
		else
			say "not ok - ASan did NOT see a read one byte past the slurped input buffer: it is rounded up, and every other sanitizer result in raw mode is worth less than it looks"
		fi
	fi
	;;
esac

# What the fast path is worth, measured every run rather than written down
# once.  Comment lines, never "ok": a timing is not a pass or a fail, and a
# suite that goes red because a machine was busy teaches nobody anything.
#
# Two corpora, because one number here would be a lie in both directions.
# UnicodeData.txt is 2.2MB of ASCII already in all four forms, which is the
# case the fast path is for; the other is the NFC column of
# NormalizationTest.txt as actual text, 76% non-ASCII and dense with combining
# marks, where the scan stops almost immediately and the fast path is worth
# nothing at all.
printf '\n--- throughput (comments, not assertions)\n'
if [ "$HAVE_UCD" = 1 ]; then
	printf '# 2.2MB of ASCII (ucd/UnicodeData.txt), already in all four forms\n' \
		| tee -a "$OUT"
	./t/nfx -t < ucd/UnicodeData.txt | tee -a "$OUT"
	python3 -c 'import sys
out = []
for line in open("ucd/NormalizationTest.txt", encoding="utf-8"):
    if line[0] in "#@":
        continue
    col = line.split(";")
    if len(col) >= 5:
        out.append("".join(chr(int(x, 16)) for x in col[1].split()))
sys.stdout.write(" ".join(out))' > scratch/mixed.txt
	printf '# 100kB of mark-dense NFC text (the NFC column of NormalizationTest.txt)\n' \
		| tee -a "$OUT"
	./t/nfx -t < scratch/mixed.txt | tee -a "$OUT"
fi

pass=$(grep -c '^ok - ' "$OUT")
fail=$(grep -c '^not ok - ' "$OUT")

# A skipped oracle must never look like a passing one.  Both external oracles
# need the network, so print what actually ran, every time, next to the score.
printf '\n=== oracles\n'
printf '  1 NormalizationTest.txt   %s\n' "$conf_state"
printf '  2 Full_Composition_Exclusion via tools/gen_tables.py  %s\n' "$gen_state"
printf '  3 CPython unicodedata     %s\n' "$diff_state"
printf '  4 NFx_QC in DerivedNormalizationProps.txt  %s\n' "$qc_state"
printf '  5 unit assertions and hostile input   ran (no data needed)\n'
printf '\n%s passed, %s failed\n' "$pass" "$fail"

case "$conf_state$diff_state$gen_state$qc_state" in
*SKIPPED*)
	printf 'WARNING: an external oracle did not run, so this score proves\n'
	printf 'much less than a full run does.  Set UTF8N_REQUIRE_ORACLES=1 to\n'
	printf 'make that a failure instead of a warning.\n'
	[ "${UTF8N_REQUIRE_ORACLES:-0}" = 1 ] && exit 1
	;;
esac
[ "$fail" -eq 0 ]
