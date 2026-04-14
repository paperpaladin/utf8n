# utf8n

Unicode normalisation -- NFD, NFC, NFKD, NFKC -- over UTF-8, in one C11 file
with no dependencies beyond libc.  Tables are generated from the Unicode
Character Database by a Python script in the repo.

**Tables are from UCD 18.0.0.**  That is the version every answer below is
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
```

**Nothing fails.**  20 of those assertions per line are the equalities the
file's own header states (`c2 == toNFC(c1) == toNFC(c2) == toNFC(c3)` and so on
for all four forms); the other 20 per line are idempotence of all four forms on
all five columns, which the file does not ask for and which is where a
non-stable canonical sort shows up.  Requirement 2 is the file's second
conformance clause: every codepoint it does not list in Part 1 must be
unchanged by all four forms, which is 1,094,910 codepoints times four.

That it passed on the first run is not evidence of anything, so see
"Watching the harness fail" below.

## Build and test

```sh
make                # utf8n.o, gcc and nothing else needed
make test           # all four oracles; prints the conformance score
make test-asan      # same, under -fsanitize=address,undefined
make gen-tables     # regenerate utf8n_table.h from ucd/
make clean
```

`make test` needs the network the first time: the UCD files are 6.5MB and are
not committed, so `t/run.sh` fetches the four it needs into `ucd/` (pinned to
18.0.0, read out of the committed header -- not `latest`, so a new Unicode
release cannot silently change the answers under the test suite).  Without the
network the two external oracles are skipped, the run says so in a roster at
the end, and `UTF8N_REQUIRE_ORACLES=1` turns that skip into a failure.

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
```

`utf8n_normalize` returns the number of bytes the result needs and writes at
most `outcap`, so the snprintf convention: call once with `outcap` 0 to size,
once to fill.  It deviates from snprintf in one way, deliberately: on
truncation it writes the longest *whole-codepoint* prefix that fits and leaves
the rest of the buffer untouched, rather than cutting a UTF-8 sequence in half.
`in` and `out` may be the same buffer.  The only failure return is `(size_t)-1`
for a failed `realloc`.

## Invalid UTF-8: the policy, stated and held to

**One U+FFFD per maximal ill-formed subpart**, per Unicode table 3-7 and TR#36,
which is also what the WHATWG Encoding Standard requires and what CPython's
`bytes.decode('utf-8', errors='replace')` does.  Concretely:

- `E3 81 41` is U+FFFD followed by `A` -- two codepoints.  The truncated
  sequence does not swallow the ASCII byte after it.
- Overlongs (`C0 80`, `E0 80 AF`), surrogates encoded as CESU-8 (`ED A0 80`),
  and anything above U+10FFFF (`F4 90 80 80`) are ill-formed and replaced byte
  by byte, not treated as their nominal value.
- `utf8n_decode` always consumes between 1 and `n` bytes, so a caller advancing
  by the return value cannot walk off its own buffer.  Checked exhaustively
  over all 16.8M byte sequences of length 1, 2 and 3.
- Normalisation is then defined on the replaced text, so the output of
  `utf8n_normalize` is always well-formed UTF-8, truncated or not.
- `utf8n_encode` maps surrogates and out-of-range values to U+FFFD rather than
  emitting invalid UTF-8.

185,799 byte strings (every 1 and 2 byte sequence, 120,000 random 3-6 byte
strings, and the standard's own worked examples) are compared against CPython's
decoder, codepoint for codepoint.

## Oracles

Ranked by how little of each one we control.

1. **`NormalizationTest.txt`, UCD 18.0.0.**  20,171 lines, 806,840 assertions.
   `t/conform.c`.
2. **`DerivedNormalizationProps.txt`, `Full_Composition_Exclusion`.**
   `tools/gen_tables.py` derives that set itself -- the listed exclusions, plus
   singleton decompositions, plus decompositions whose first character is not a
   starter -- and refuses to emit tables unless its derived set equals the
   UCD's published one exactly.  1120 codepoints.  This is an independent check
   on the exclusion rules, and `make test` additionally regenerates the
   committed header and diffs it byte for byte, so the tables cannot have been
   hand-edited.
3. **CPython `unicodedata.normalize`.**  Works offline, so it is the oracle
   that always runs.  All four forms over: every BMP codepoint (63,488), every
   astral codepoint with a decomposition or a combining class plus every 97th
   of the rest (13,132), 5580 starter-mark-mark sequences, all 11,172 Hangul
   jamo sequences, runs of 33 to 2000 combining marks, and strings that start
   with a combining mark.
4. **CPython's UTF-8 decoder**, as above.

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
as unexplained, naming both answers -- so the classifier does bite.

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

**Three survived everything**, and two of them were real gaps that are now
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

## What does NOT work

Stated plainly, because a README that only lists successes is not finished.

- **No streaming or incremental API.**  `utf8n_normalize` buffers the entire
  input as codepoints: peak memory is about 4 bytes per input codepoint plus
  the output, and a 1GB input needs the lot at once.  There is no way to feed it
  a chunk at a time.  Normalisation is only segmentable at starter boundaries,
  so a streaming version is a real design, not a flag.
- **`realloc` failure is reported, not survived.**  `(size_t)-1` and nothing
  written.  No fixed-buffer mode, no caller-supplied allocator.
- **No quick-check fast path.**  Already-NFC text is fully decomposed and
  recomposed anyway, which is the single largest performance item here and is
  deliberately absent: `NFC_QC`/`NFD_QC` from `DerivedNormalizationProps.txt`
  would skip most real-world text, at the cost of a second code path that the
  conformance file would mostly not exercise.
- **O(n^2) canonical ordering** within a run of combining marks.  Fine for the
  runs that occur in text (the longest in the UCD's own test data is 4) and
  tested up to 2000 marks, but a crafted input of a million marks on one base
  will take quadratic time.  An insertion sort is the right shape for runs of
  2-4 and the wrong shape for an adversary.
- **Binary search, not a trie.**  9 comparisons for a combining class, 13 for a
  decomposition, per codepoint, three passes.  Roughly 3x slower than a
  two-stage table and about 100 lines shorter.
- **Not thread-hostile but not thread-aware either.**  No global state, so
  concurrent calls are fine; there is no locking because there is nothing to
  lock.
- **Only the four normalisation forms.**  No `NFKC_Casefold`, no case folding,
  no collation, no grapheme cluster segmentation, no bidi.  `wcwidth`-style
  display width lives in the sibling `vtparse` repo and is deliberately not
  duplicated here.
- **Unicode version is baked in at generation time.**  There is no runtime
  switch, and normalising with 18.0.0 tables text that a 15.1.0 system
  normalised will differ on the 149 codepoints listed above.  That is inherent
  to normalisation across versions, not a bug here, but it is a real
  interoperability hazard if two sides of a protocol use different versions.
- **`utf8n_encode` silently substitutes U+FFFD** for a surrogate or an
  out-of-range value rather than reporting the error.  A caller that wants to
  know its codepoint was invalid has to range-check first.
- The committed table header is **261KB of C** (119KB of `.rodata`): 5982
  decomposition records, 6828 pool words, 422 combining-class ranges, 961
  primary composites.  Justified because it is the whole UCD rather than a
  Latin-1-and-hope subset, and because the alternative -- parsing
  `UnicodeData.txt` at startup -- makes a C library depend on a 2.2MB data file
  being installed and correct.  It compresses to about 40KB in git, and it is
  regenerated and diffed byte for byte by `make test`, so it cannot drift from
  the generator.

## Files

```
utf8n.c            389 lines   the implementation
utf8n.h             56 lines   the contract, including the ill-formed policy
utf8n_table.h      261 KB      GENERATED -- do not edit
tools/gen_tables.py           reads the UCD, derives and checks the exclusions
t/run.sh                      builds and runs all four oracles, prints the score
t/conform.c                   NormalizationTest.txt
t/unit.c                      assertions that need no downloaded data
t/diff.py                     CPython differential with skew attribution
t/nfx.c                       the filter diff.py drives, plus -r for raw bytes
```

Deliberate shortcuts are marked `shortcut:` in the source with the ceiling they
accept and the upgrade path.

## Licence

GPL-3.0-or-later.  `NormalizationTest.txt` and the other UCD files are fetched
at test time and are not redistributed here; they are covered by the Unicode
Terms of Use.
