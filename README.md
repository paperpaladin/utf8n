# utf8n

Unicode normalisation (NFD, NFC, NFKD, NFKC) over UTF-8, in one C11 file
with no dependencies beyond libc.  Tables are generated from the Unicode
Character Database by a Python script in the repo.

Tables are from UCD 18.0.0.  That is the version every answer below is
correct for, and `tools/gen_tables.py` reads it out of the data files rather
than anyone typing it in; `utf8n_ucd_version()` returns it at runtime.

## Score against the official conformance file

`NormalizationTest.txt` from the UCD is the specification of what the four
functions mean, and it is the point of this repo:

```
conformance score: 806840/806840 assertions, 20171/20171 lines
                   (NormalizationTest.txt, UCD 18.0.0)

ok - @Part0: 46 lines, 1840/1840 assertions          (specific cases)
ok - @Part1: 17154 lines, 686160/686160 assertions   (character by character)
ok - @Part2: 2004 lines, 80160/80160 assertions      (canonical order test)
ok - @Part3: 194 lines, 7760/7760 assertions         (PRI #29 test)
ok - @Part4: 735 lines, 29400/29400 assertions       (canonical closures)
ok - @Part5: 38 lines, 1520/1520 assertions          (chained primary composites)
ok - requirement 2: 1094910/1094910 codepoints outside Part 1 are unchanged
ok - quick check promises hold on every column: 403420/403420 column-form
     pairs, 221109 Yes 117585 No 64726 Maybe
ok - the fast path ran exactly where the quick check says it can: 403420/403420
     columns, 22872123 bytes copied verbatim over 5993320 calls
```

Nothing fails.  20 of those assertions per line are the equalities the
file's own header states (`c2 == toNFC(c1) == toNFC(c2) == toNFC(c3)` and so on
for all four forms). The other 20 per line are idempotence of all four forms on
all five columns, which the file does not ask for and which is where a
non-stable canonical sort shows up.  Requirement 2 is the file's second
conformance clause: every codepoint it does not list in Part 1 must be
unchanged by all four forms, which is 1,094,910 codepoints times four.

That it passed on the first run is not evidence of anything, so see
"Watching the harness fail" below.

## Build and test

```sh
make                # utf8n.o, gcc and nothing else needed
make test           # every oracle; prints the conformance score and MB/s
make test-asan      # same, under -fsanitize=address,undefined
make gen-tables     # regenerate utf8n_table.h from ucd/
make clean
```

`make test` needs the network the first time: the UCD files are 6.5MB and are
not committed, so `t/run.sh` fetches the four it needs into `ucd/` (pinned to
18.0.0, read out of the committed header: not `latest`, so a new Unicode
release cannot silently change the answers under the test suite).  Without the
network, and with nothing cached, every UCD-backed oracle is skipped, the run
says so in a roster at the end, and `UTF8N_REQUIRE_ORACLES=1` turns that skip
into a failure.  A skip is a skip and never a failure: a run with `ucd/`
missing and CPython's `ucd-15.1.0/` still cached used to print "100 passed, 1
failed", because `t/diff.py` ran and died on its first `open()`.  It is
skipped with the rest now.

## API

```c
enum utf8n_form { UTF8N_NFD, UTF8N_NFC, UTF8N_NFKD, UTF8N_NFKC };

size_t utf8n_normalize(enum utf8n_form form, const char *in, size_t inlen,
                       char *out, size_t outcap);
size_t utf8n_decode(const unsigned char *s, size_t n, uint32_t *cp);
size_t utf8n_encode(uint32_t cp, unsigned char *out);
int utf8n_ccc(uint32_t cp);
int utf8n_max_expansion(void);        /* 18, set by U+FDFA under NFKD */
const char *utf8n_ucd_version(void);  /* "18.0.0" */

enum utf8n_qc { UTF8N_QC_YES, UTF8N_QC_NO, UTF8N_QC_MAYBE };
enum utf8n_qc utf8n_quick_check(enum utf8n_form form, const char *in,
                                size_t inlen);
```

`utf8n_normalize` returns the number of bytes the result needs and writes at
most `outcap`, so the snprintf convention: call once with `outcap` 0 to size,
once to fill.  It deviates from snprintf in one way, deliberately. On
truncation it writes the longest *whole-codepoint* prefix that fits and leaves
the rest of the buffer untouched, instead of cutting a UTF-8 sequence in half.
`in` and `out` may be the same buffer.  The only failure return is `(size_t)-1`
for a failed `realloc`.

`utf8n_quick_check` answers "is this text already in that form" without
normalising it: one table lookup per codepoint, no allocation, and it stops at
the first codepoint that settles the question.  This is UAX #15's quick check
and it has three answers, not two.  `UTF8N_QC_YES` is a guarantee that
`utf8n_normalize` would return the input byte for byte; `UTF8N_QC_NO` is
equally definite the other way; `UTF8N_QC_MAYBE` means the cheap test cannot
tell, because the text contains a codepoint that something in front of it
might compose with, and the only way to find out is to normalise and compare.
Over the conformance corpus the three answers come up 221,109 / 117,585 /
64,726 times, so MAYBE is not a rare corner.

Ill-formed input is `UTF8N_QC_NO`, even when every codepoint it decodes to is
normalisation-stable: `utf8n_normalize` replaces the bad bytes with U+FFFD, so
what comes back is not what went in, and YES has to keep meaning unchanged.
`utf8n_normalize` uses the same table itself, which is the next section.

## The fast path, and what it is actually worth

`utf8n_normalize` starts by asking how many leading bytes are already in the
requested form and cannot be changed by anything that follows.  Those bytes
are copied straight through and only the rest is decomposed, reordered and
recomposed.

A byte offset k is a safe place to split when three things hold.  Everything
before k is well-formed, `QC_YES` and in canonical order, which is exactly the
promise `utf8n_quick_check` makes.  The codepoint AT k is a starter, so no mark
after k can reorder in front of anything before k, because canonical ordering
never crosses a class-0 codepoint.  And the codepoint at k is `QC_YES` rather
than `QC_MAYBE`, which is what stops it being the second half of a
composition: Hangul V and T jamo are starters that compose with the starter in
front of them, and so are twelve vowel signs in Tulu-Tigalari, Gurung Khema
and Kirat Rai.  Split in front of an L jamo and `<L, V>` quietly stops
composing; split in front of a V jamo and nothing is lost.  That is why the
Maybe set has to be exactly right and not approximately right.  The codepoint
at the split goes into the slow half, since it is free to be the FIRST half of
a composition with whatever comes next.

Bytes below 0x80 skip both binary searches: ASCII is its own encoding, has
combining class 0, and is `QC_YES` in all four forms.  That branch is where
nearly all of the speed comes from, so `t/unit` asserts all three facts for
all 128 of them against the generated table, which `t/diff.py` in turn checks
against the UCD.

Measured by `t/nfx -t`, which `make test` runs on two corpora every time so
the numbers below cannot go stale quietly.  CPU time via `clock()`, one
machine, repeated until 0.2s per form; runs vary by about 5%.

| corpus | NFD | NFC | NFKD | NFKC |
| --- | --- | --- | --- | --- |
| 2.2MB of ASCII (`UnicodeData.txt`), no fast path | 38.9 | 19.6 | 38.7 | 19.6 |
| the same, with it | **1611** | **1594** | **1562** | **1642** |
| the same plus one U+00E9 at byte 100, no fast path | 38.6 | 19.3 | 39.9 | 18.3 |
| the same, with it | 39.0 | **1570** | 38.0 | **1575** |
| 100kB of mark-dense NFC text, no fast path | 46.3 | 26.7 | 47.9 | 26.2 |
| the same, with it | 47.3 | 27.5 | 47.9 | 26.8 |
| 400kB of urandom, no fast path | 23.2 | 13.0 | 23.4 | 12.9 |
| the same, with it | 23.0 | 12.9 | 21.7 | 12.9 |

MB/s.  "No fast path" is `fast_prefix()` edited to return 0, which is also
mutant F1 below.

Three things in that table are worth saying out loud.  ASCII text already in
the form goes 40 to 80 times faster.  Nothing gets slower, including 400kB of
random bytes, where the scan stops at the first ill-formed byte and the whole
exercise costs a handful of comparisons.  And the mark-dense corpus gains
nothing at all: it is the NFC column of `NormalizationTest.txt` as actual
text, 76% non-ASCII, and the scan stops after 8 bytes, because a combining
mark is `QC_MAYBE` for the composing forms and most codepoints in it are
`NFD_QC=No` for the decomposing ones.

The second pair of rows is the honest limit.  One precomposed U+00E9 a hundred
bytes into a 2.2MB file leaves NFD at 39.0 MB/s, the no-fast-path speed, for
the remaining 2.2MB: the scan stops at the first codepoint that is not
`QC_YES` and never resumes.  NFC is unaffected by that same character, because
the file is already NFC.  Resuming after each slow segment is the next item in
`PLAN.md`, and it is not a one-line change: the contract allows `in` and `out`
to overlap arbitrarily, which is why nothing is written until the whole input
has been read, and a loop that alternates copying and composing has to keep
that property.

## Invalid UTF-8: the policy, stated and held to

One U+FFFD per maximal ill-formed subpart, per Unicode table 3-7 and TR#36,
which is also what the WHATWG Encoding Standard requires and what CPython's
`bytes.decode('utf-8', errors='replace')` does.  Concretely:

- `E3 81 41` is U+FFFD followed by `A`, two codepoints.  The truncated
  sequence does not swallow the ASCII byte after it.
- Overlongs (`C0 80`, `E0 80 AF`), surrogates encoded as CESU-8 (`ED A0 80`),
  and anything above U+10FFFF (`F4 90 80 80`) are ill-formed and replaced byte
  by byte, not treated as their nominal value.
- `utf8n_decode` always consumes between 1 and `n` bytes, so a caller advancing
  by the return value cannot walk off its own buffer.  Checked exhaustively
  over all 16.8M byte sequences of length 1, 2 and 3.
- Normalisation is then defined on the replaced text, so the output of
  `utf8n_normalize` is always well-formed UTF-8, truncated or not.
- `utf8n_encode` maps surrogates and out-of-range values to U+FFFD,
  not emitting invalid UTF-8.

185,799 byte strings (every 1 and 2 byte sequence, 120,000 random 3-6 byte
strings, and the standard's own worked examples) are compared against CPython's
decoder, codepoint for codepoint.

## Oracles

Ranked by how little of each one we control.

1. `NormalizationTest.txt`, UCD 18.0.0.  20,171 lines, 806,840 assertions.
   `t/conform.c`.
2. `DerivedNormalizationProps.txt`, `Full_Composition_Exclusion`.
   `tools/gen_tables.py` derives that set itself: the listed exclusions, plus
   singleton decompositions, plus decompositions whose first character is not a
   starter, and refuses to emit tables unless its derived set equals the
   UCD's published one exactly.  1120 codepoints.  This is an independent check
   on the exclusion rules, and `make test` additionally regenerates the
   committed header and diffs it byte for byte, so the tables cannot have been
   hand-edited.
3. `DerivedNormalizationProps.txt`, properties `NFD_QC`, `NFC_QC`, `NFKD_QC`
   and `NFKC_QC`.  Derived twice and checked twice.  `tools/gen_tables.py`
   derives all four by normalising every codepoint with its own small Python
   implementation and refuses to emit tables unless its No and Maybe sets
   equal the UCD's published ones exactly; then `t/diff.py` asks the C
   `utf8n_quick_check` for every codepoint and compares that against the same
   published property, with a second, independent parser for those lines.
   1,112,064 codepoints times four forms: 4,411,432 Yes, 36,560 No, 264 Maybe,
   0 wrong.  The two checks catch different things, which a mutation
   demonstrates: a generator that packs Maybe as No passes the byte-for-byte
   header diff, because the header and the generator agree with each other,
   and fails the sweep.
4. CPython `unicodedata.normalize` and `unicodedata.is_normalized`.  Needs no
   network once the UCD files are on disk, so it is the oracle that survives a
   broken connection; it does need both UCD directories, ours and CPython's
   own version, because a disagreement has to be attributed to a property
   change or reported.  All four forms over: every BMP codepoint (63,488), every
   astral codepoint with a decomposition or a combining class plus every 97th
   of the rest (13,132), 5580 starter-mark-mark sequences, all 11,172 Hangul
   jamo sequences, runs of 33 to 2000 combining marks, and strings that start
   with a combining mark.  `is_normalized` is the external check on
   `utf8n_quick_check` over multi-codepoint strings, which is where its
   canonical-ordering branch lives: deleting that branch is invisible to the
   per-codepoint sweep above and shows up here as 0061 0301 0316 answering YES
   where CPython says it is not normalised.
5. CPython's UTF-8 decoder, as above.

### Version skew, and not hiding behind it

Our tables are UCD 18.0.0.  This machine's CPython is 15.1.0, and **6420
codepoints differ between those two versions** in combining class,
decomposition or composition-exclusion status, so "version skew" is an excuse
that could absorb a real bug.  `t/diff.py` therefore refuses to make the excuse
in the abstract.  A disagreement is tolerated only when a codepoint in the
input, or in either answer, is in that differing set, and the script prints what
that codepoint's properties were in each version:

```
# every BMP codepoint: 7 codepoints disagree, every one of them because its
# own properties changed between 15.1.0 and 18.0.0:
#   U+0558  15.1.0 unassigned  ->  18.0.0 ccc=0 decomp=<super> 0567
#   U+209D  15.1.0 unassigned  ->  18.0.0 ccc=0 decomp=<sub> 0077
#   ...
# 149 codepoints in total disagree with CPython, all of them explained by a
# property change between UCD 15.1.0 and 18.0.0; 0 unexplained
```

All 149 are codepoints that were unassigned in 15.1.0 and have a compatibility
decomposition in 18.0.0: the Armenian superscripts U+0558/U+058B/U+058C, the
Latin subscripts U+209D..U+209F, the Garay block U+105C0..U+105F3, Tulu-Tigalari,
and a run of IPA superscript modifiers in U+107Bx.  **Zero unexplained.**  With
canonical ordering deliberately made unstable, 1500+ disagreements are reported
as unexplained, naming both answers, so the classifier does bite.

## Watching the harness fail

806,840 assertions passing on the first run is a reason to distrust the
harness, not to celebrate.  Twelve defects were injected into a correct
implementation one at a time and scored against the conformance file:

| injected defect | conformance score | lines |
| --- | --- | --- |
| blocking rule `last_cc <= cc` | 801380/806840 | 19625/20171 |
| blocking rule dropped | 799560/806840 | 19443 |
| canonical ordering unstable (`>=`) | 699520/806840 | 17485 |
| canonical ordering dropped | 795190/806840 | 17856 |
| Hangul filler U+11A7 always emitted | 795630/806840 | 19643 |
| Hangul `LV + U+11A7` composes | 806830/806840 | 20170 |
| NFD reads the NFKD column | 806769/806840 | 20155 |
| composition exclusions ignored | caught by the table diff, then ~804k | |
| UTF-8 ill-formed subpart off by one | caught by the unit assertions | |

Reproduce any row by making the edit and running `make test`.

Three survived everything, and two of them were real gaps that are now
closed:

1. `utf8n_decode()` returning a sequence's nominal length for a sequence
   truncated at the end of the buffer.  No normalisation test can see it, since
   our own loop stops at `inlen` either way, but a caller advancing by the
   return value walks off its buffer.  Now pinned exhaustively.
2. The Hangul range test written as `<= SCOUNT`.  U+D7A4 is one past the block
   and `(U+D7A4 - SBASE) % 28 == 0`, so a trailing jamo after it would compose
   into a codepoint outside Hangul.  `NormalizationTest.txt` lists no sequence
   beginning with U+D7A4 because it is unassigned, so the oracle is blind here
   by construction.  Three boundary cases added by hand.
3. The `last_cc = 256` guard for a string that begins with a combining mark.
   That one is genuinely dead code: a pair whose first character is a
   non-starter is a `Full_Composition_Exclusion`, so the lookup returns 0
   regardless.  Kept, with a comment saying so, because the argument for its
   deadness depends on the exclusion table being right and that is a different
   file's job.

The other defect the test suite found was in the suite itself: the first
truncation check asserted snprintf semantics that `utf8n_normalize` does not
implement and should not, which is how the whole-codepoint-prefix contract came
to be written down.

### Mutating the generated table, and the generator that writes it

A generated, committed table has a failure mode of its own: the generator and
the header drifting apart, so that `tools/gen_tables.py` describes one Unicode
version and `utf8n_table.h` encodes something else.  Three mutants, aimed at
that seam rather than at the algorithms:

| injected defect | caught by |
| --- | --- |
| one composite in `utf8n_table.h` edited by hand (`A`+`U+0300` -> `U+00C1`) | conformance (806830/806840, 20170/20171 lines), CPython diff (2 unexplained), and the table diff: 104 passed / 5 failed |
| generator field width changed, `%5d` -> `%6d`, header left alone | the table diff ALONE: 106 passed / 1 failed |
| the non-starter clause dropped from `check_exclusions()` | the generator refusing: `only ours [], only UCD [3955, 3957, 3969]`, which is U+0F73, U+0F75 and U+0F81 |

The middle row is the one that justifies the check.  A generator whose output
formatting has moved produces a table that is byte-for-byte different and
semantically identical, so every value oracle in the repository agrees with it
and only the regeneration diff notices.  Without that diff the generator would
have become decoration, and the next person to re-run it would get a diff of
thousands of lines with no way to tell drift from a Unicode update.

Writing those three found two defects in the reporting, both of the
"passes while saying nothing" kind:

- The oracle roster printed `2 Full_Composition_Exclusion via
  tools/gen_tables.py  ran, table diffed byte for byte` **whether or not the
  diff agreed**.  The roster exists to stop a skipped oracle looking like a
  passing one, and on this line it made a failing oracle look like a passing
  one.  It reports the outcome now: `header matches the generator byte for
  byte`, `HEADER DOES NOT MATCH the generator`, or `REFUSED`.
- A refusal and a wrong answer shared one `not ok` line.  The generator
  exiting non-zero because its derived `Full_Composition_Exclusion` set
  disagrees with the UCD's is a refusal: no table was written and no
  comparison was made.  A header that differs from the regenerated one is a
  wrong answer already sitting in the repository.  They are separate lines
  now, and the generator's own output, including the mismatch message, is
  logged with a `# ` prefix so it cannot be counted as a result.

### The quick check, and a survivor that was a harness gap

Ten more mutants, for the quick-check tables and `utf8n_quick_check`:

| injected defect | caught by |
| --- | --- |
| the canonical-ordering branch deleted | `t/conform`, 5164 of 403,420 promises broken, and CPython's `is_normalized`; NOT the per-codepoint sweep, which only ever sees one codepoint at a time |
| MAYBE reported as YES | the sweep (132 wrong) and `t/conform` (55,773 promises) |
| `utf8n_quick_check` always YES | the sweep (17,274 wrong) and `t/conform` (173,358) |
| `utf8n_quick_check` always MAYBE | `t/conform`'s requirement that all three answers occur at least once, and the sweep: 1,112,064 wrong |
| range search off by one, `cp >= hi` | the sweep, 529 wrong, exactly one per range |
| generator drops the Maybe closure | the generator refusing: `NFC_QC mismatch: Maybe only UCD [70597, ...]` |
| generator packs Maybe as No | the sweep (132 wrong) and `t/conform`; NOT the header diff, which still says the header matches the generator, because it does |
| generator shifts the No bits into the wrong form | the sweep, 16,034 wrong |
| the ill-formed byte comparison deleted | **nothing, at first** |
| the same check weakened to compare only the length | `t/unit`'s `F0 90 80 20` case, and 40 of 200,000 random windows |

The ninth row is the one worth the section.  `utf8n_quick_check` re-encodes
each codepoint and compares the bytes, which is how it notices that the input
was ill-formed; deleting that comparison passed all 109 assertions.  The
reason is granularity, not coverage.  The hostile-input oracle asks the
question once about the whole 400kB buffer, and a buffer of random bytes that
long certainly contains some codepoint with `NFx_QC=No`, so the answer is NO
whether or not the ill-formed bytes were noticed.  `NormalizationTest.txt` is
valid UTF-8 from end to end and the UCD's property is defined on codepoints,
so neither external oracle can see this at all.

Two things closed it.  Seventeen hand-written cases in `t/unit.c`, including
`F0 90 80 20`, which consumes three bytes and yields U+FFFD -- exactly what
three well-formed bytes of U+FFFD do, so a decoder that checks only the
consumed length calls it well-formed.  And `t/nfx -r` now asks the same
question of every 8-byte window of the random input, 50,000 of them, where
1622 windows are all-ASCII and answer YES.  With the comparison deleted,
193,060 of 200,000 window-form pairs break the promise.

The lesson is the one about granularity.  A property asserted over a single
large input can be true for a reason that has nothing to do with the code
under test, and the fix is not more input but shorter input.

### The fast path, and the two things that only a counter can see

A fast path is the shape METHOD warns about: when it is never taken, every
answer is still right.  So `utf8n.c` has two counters under `-DUTF8N_STATS`
and `t/conform` asserts, for all 403,420 column-form pairs, that the fast path
copied the whole column exactly when the quick check says YES and stopped
short of the end otherwise.  That assertion earned itself immediately: with
`fast_prefix()` edited to return 0, all 806,840 conformance assertions, all
403,420 quick-check promises and the whole CPython differential still pass,
and the counter is the only thing in the suite that goes red.

Eight mutants:

| injected defect | caught by |
| --- | --- |
| `fast_prefix()` returns 0, the path is never taken | the counter in `t/conform`, and nothing else: 182,311 of 403,420 columns |
| split allowed anywhere, not only at a starter | `NormalizationTest.txt` (ordering breaks: `05B8 05B9 05B1 ...`) and three `t/unit` ordering cases |
| `QC_MAYBE` accepted as safe at the split | nine `t/unit` composition cases and the conformance file: `<L, V>` stops composing |
| the fast path ignores whether the bytes were well-formed | the hostile-input oracle, which reports the output is not well-formed UTF-8 in all four forms, plus 192,995 broken promises over the random windows |
| the ASCII branch takes continuation bytes too, `< 0xC0` | `t/unit`'s ill-formed cases and 26,938 of the random windows |
| the ASCII branch forgets to reset the combining class | one `t/unit` case, counting bytes: 2 of 5 copied instead of 5 |
| truncation back-up deleted, so the prefix can be cut mid-codepoint | `t/unit`'s truncation cases |
| the prefix copied BEFORE the input is read | `t/unit`'s overlapping-buffer case, with `out` two bytes after `in` |

The last three were survivors first, and all three for the same reason: the
properties they break were only ever checked on input where the fast path does
nothing.  Truncation and well-formedness were checked against 400kB of random
bytes, which is ill-formed, so the fast prefix there is always empty; in-place
use was checked with `in == out`, where copying early writes the same bytes
back to the same place and nothing notices.  The three new cases are
already-normalised multi-byte strings, every `outcap` from 0 to the output
length, and `out` at a different offset from `in` in both directions.

Only the shifted-forward direction catches the early-copy mutant, and the
reason is worth keeping: with `out` before `in`, the prefix write lands
entirely in bytes the decode loop has already passed, so it cannot corrupt
anything.  The case where `out` is after `in` is the one that can.

## Every oracle's exit status is now checked

Each oracle used to run as `./t/conform ... 2>&1 | tee -a "$OUT"`, which
discards the status twice: a pipeline's status is `tee`'s, and nothing looked
at it either way. The score is counted by grepping the log for `ok - ` lines,
and nothing asserts how many there should be.

So a test binary that died partway through left its earlier `ok` lines in
the file, emitted no `not ok`, and the suite printed "N passed, 0 failed"
with a smaller N. **The 806,840 conformance assertions could have become a
few dozen with the suite green.** Under `make test-asan` the discarded
status was ASan's exit 1.

Worse, it defeated the roster, which is this repo's best idea. `conf_state`
is read with `grep '^conformance score' "$OUT" | tail -1`, so a `t/conform`
that crashed before printing its score left that variable empty, and an
empty string does not match the `*SKIPPED*` case. The "an external oracle
did not run" warning never fired. One mechanism built to stop a skipped oracle looking like a passing one was bypassed by the failure mode most
likely to occur.

All five oracles go through one `runq` helper now, which captures the status,
prints the output, and emits a `not ok` line naming the exit code when it is
non-zero. Measured on the helper, against a command that prints one `ok` line
and exits 134:

```
OLD  | tee :  ok=1  not-ok=0
NEW  runq   :  ok=1  not-ok=1
```

`tzf/t/run.sh` next door also sets `ASAN_OPTIONS=exitcode=`, which makes a
sanitizer report distinguishable from an ordinary failure. This repo now does,
in the one case that needs it: see the next section.

### The sanitizer was watching a buffer bigger than its data

`t/nfx -r` is the mode `make test-asan` points at 400kB of `/dev/urandom`, and
it is the most attacker-shaped thing here: untrusted bytes, straight into the
decoder.  Its input buffer grew by doubling, so after reading 400,000 bytes it
was 524,288 bytes long, and a read one byte past the end of the data landed
121KB inside a live allocation.  Planting exactly that read produced **zero
sanitizer reports and a green suite**.

This is the failure that `METHOD.md` records finding seven times in one
session across the other repositories, and this was an eighth.  One `realloc`
down to the exact length after the read loop fixes it, and the fix is only
worth as much as the check that it stays: `-DUTF8N_PLANT_OVERREAD` builds a
second binary with the over-read deliberately in it, and `make test-asan`
requires that run to be killed.

```
ok - ASan sees a read one byte past the slurped input buffer, so that buffer
     is exactly sized
```

With the `realloc` deleted that line reads `not ok` and says so in those
words.  The case runs under `make test-asan` only, since a planted over-read
is invisible without a sanitizer, and it checks for exit code 42 via
`ASAN_OPTIONS=exitcode=42` rather than for any non-zero status, so it cannot
pass because the binary happened to crash or fail to build.

## What does NOT work

Stated plainly, because a README that only lists successes is not finished.

- No streaming or incremental API.  `utf8n_normalize` buffers the entire
  input as codepoints: peak memory is about 4 bytes per input codepoint plus
  the output, and a 1GB input needs the lot at once.  There is no way to feed it
  a chunk at a time.  Normalisation is only segmentable at starter boundaries,
  so a streaming version is a real design, not a flag.
- `realloc` failure is reported, and the program stops there.  `(size_t)-1` and nothing
  written.  No fixed-buffer mode, no caller-supplied allocator.
- The fast path is one prefix, not a loop.  It stops at the first codepoint
  that is not `QC_YES` and does not resume, so a single precomposed character
  early in a large file costs the whole saving for everything after it: 39.0
  MB/s instead of 1611 for NFD over 2.2MB, measured above.  Mark-dense text
  gains nothing even when it is entirely normalised, because combining marks
  are `QC_MAYBE` for NFC and NFKC.  Both are the same missing feature, a loop
  that alternates fast and slow segments, and it has to be written around the
  overlapping-buffer contract rather than over it.
- O(n^2) canonical ordering within a run of combining marks.  Fine for the
  runs that occur in text (the longest in the UCD's own test data is 4) and
  tested up to 2000 marks, but a crafted input of a million marks on one base
  will take quadratic time.  An insertion sort is the right shape for runs of
  2-4 and the wrong shape for an adversary.
- Binary search, with no trie.  9 comparisons for a combining class, 13 for a
  decomposition, per codepoint, three passes.  Roughly 3x slower than a
  two-stage table and about 100 lines shorter.
- Not thread-hostile but not thread-aware either.  No global state, so
  concurrent calls are fine. There is no locking because there is nothing to
  lock.  The exception is deliberate and is not in a normal build:
  `-DUTF8N_STATS` adds two unsynchronised counters that exist so the test
  suite can tell whether the fast path ran, since a fast path that is never
  taken returns identical answers.  `make` does not define it; `t/run.sh`
  defines it for `t/unit` and `t/conform` only.
- Only the four normalisation forms.  No `NFKC_Casefold`, no case folding,
  no collation, no grapheme cluster segmentation, no bidi.  `wcwidth`-style
  display width lives in the sibling `vtparse` repo and is deliberately not
  duplicated here.
- Unicode version is baked in at generation time.  There is no runtime
  switch, and normalising with 18.0.0 tables text that a 15.1.0 system
  normalised will differ on the 149 codepoints listed above.  That is inherent
  to normalisation across versions, not a bug here, but it is a real
  interoperability hazard if two sides of a protocol use different versions.
- `utf8n_encode` silently substitutes U+FFFD for a surrogate or an
  out-of-range value instead of reporting the error.  A caller that wants to
  know its codepoint was invalid has to range-check first.
- The committed table header is **281KB of C** (119KiB of `.rodata`, 122,128
  bytes by `size -A`): 5982
  decomposition records, 6828 pool words, 422 combining-class ranges, 961
  primary composites, 529 quick-check ranges.  Justified because it is the whole UCD and no Latin-1-and-hope subset, and because the alternative, parsing
  `UnicodeData.txt` at startup, makes a C library depend on a 2.2MB data file
  being installed and correct.  It compresses to about 40KB in git, and it is
  regenerated and diffed byte for byte by `make test`, so it cannot drift from
  the generator.

## Files

```
utf8n.c            562 lines   the implementation
utf8n.h             79 lines   the contract, including the ill-formed policy
utf8n_table.h      281 KB      GENERATED -- do not edit
tools/gen_tables.py           reads the UCD, derives and checks the exclusions
                              and the four quick-check properties
t/run.sh                      builds and runs every oracle, prints the score
t/conform.c                   NormalizationTest.txt
t/unit.c                      assertions that need no downloaded data
t/diff.py                     CPython differential with skew attribution
t/nfx.c                       the filter diff.py drives, plus -r for raw bytes,
                              -t for throughput, and the planted over-read
```

Deliberate shortcuts are marked `shortcut:` in the source with the ceiling they
accept and the upgrade path.

## Licence

GPL-3.0-or-later.  `NormalizationTest.txt` and the other UCD files are fetched
at test time and are not redistributed here. They are covered by the Unicode
Terms of Use.
