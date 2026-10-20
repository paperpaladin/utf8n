# PLAN

## Contract

`utf8n` is a single C11 file that normalises UTF-8 text to any of the four
Unicode normalisation forms.

- `utf8n_normalize(form, in, inlen, out, outcap)` where `form` is one of
  `UTF8N_NFD`, `UTF8N_NFC`, `UTF8N_NFKD`, `UTF8N_NFKC`.  It returns the number
  of bytes the result needs, writing at most `outcap` of them, so the snprintf
  calling convention: call once to size, once to fill.
- `utf8n_decode()` / `utf8n_encode()` are the UTF-8 layer, public because the
  replacement policy for ill-formed input is part of the contract, not an
  implementation detail.
- `utf8n_ccc()` exposes Canonical_Combining_Class, because the canonical
  ordering test has to be able to check it independently.
- `utf8n_quick_check(form, in, inlen)` answers whether text is already in a
  form without normalising it, with UAX #15's three answers.  YES promises
  that `utf8n_normalize()` returns the input unchanged, NO promises that it
  does not, MAYBE promises nothing.
- Tables come from `tools/gen_tables.py` reading the UCD.  The generated header
  is committed so that building needs gcc and nothing else.
- Not in the contract: collation, case folding, grapheme segmentation, a
  streaming API, locale anything, NFKC_Casefold.

## Hard invariant

For every one of the 20,171 non-comment lines of `NormalizationTest.txt`, all
twenty equalities that line asserts hold, and additionally every form is
idempotent on all five columns: `f(f(x)) == f(x)` for f in NFC, NFD, NFKC,
NFKD.  Conformance is not "most lines pass". The file is the specification of
what the four functions mean, and a failing line is a wrong answer.

Two consequences shape the code.  Idempotence over an external corpus is what
catches a canonical-ordering bug that a hand-written test would miss, because
reordering that is not stable shows up only when you normalise twice.  And
because the test file covers all five columns in both directions, a composition
table that forgets exclusions cannot pass by accident.

## Oracle

Ranked by how little we control them, strongest first.

1. `NormalizationTest.txt` from the UCD.  Official conformance data,
   19,000-odd lines, each asserting twenty equalities we did not choose.
   `t/conform.c` runs all of them plus idempotence, and prints a score.
2. `DerivedNormalizationProps.txt`, property `Full_Composition_Exclusion`.
   `tools/gen_tables.py` *derives* that set (listed exclusions, plus singleton
   decompositions, plus non-starter decompositions) and then asserts its derived
   set equals the UCD's published one, exactly, or refuses to emit tables.  This
   is the oracle for the composition-exclusion rules, and it is independent of
   the normalisation test data.
3. `DerivedNormalizationProps.txt`, properties `NFD_QC`, `NFC_QC`, `NFKD_QC`,
   `NFKC_QC`.  `tools/gen_tables.py` derives all four by normalising every
   codepoint with its own Python implementation and refuses to emit tables
   unless its No and Maybe sets equal the published ones; `t/diff.py` then
   compares the C `utf8n_quick_check` against the same file for every
   codepoint, with its own parser for those lines.  4,448,256 answers.
4. CPython's `unicodedata.normalize`.  Available offline, so it is the
   oracle when the UCD files cannot be fetched.  `t/diff.py` compares all four
   forms over the whole BMP plus a sample of the astral planes.  Python's UCD
   version differs from ours, so disagreements are classified against the two
   `UnicodeData.txt` files: a disagreement is tolerated only when the codepoint
   involved demonstrably changed between the two versions, and anything else
   fails the run.
5. CPython's UTF-8 decoder.  `bytes.decode('utf-8', errors='replace')`
   implements the same maximal-subpart policy, so `t/diff.py` checks ill-formed
   input against it, not against our own opinion.

## Non-goals

- Streaming / incremental normalisation.  `utf8n_normalize()` buffers the whole
  input as codepoints.
- NFKC_Casefold, collation keys, grapheme clusters, `wcwidth` (that one lives in
  `vtparse`, deliberately not duplicated here).
- ~~Quick-check fast paths~~.  This said "always decomposing is slower and
  much shorter, and the conformance file does not reward the cleverness",
  which contradicted the first item of the queue below for as long as both
  were written down.  The queue wins: the properties have a published oracle,
  so the cleverness is checkable, which was the real objection.  Both halves
  are in now, and the second sentence of the old non-goal was also wrong on
  the facts: the conformance file does reward it, once the harness counts
  bytes copied instead of only comparing answers.

## Commit sequence

- [x] UTF-8 decode and encode, with the maximal-subpart replacement policy
      pinned by table 3-7 of the standard
      why: everything downstream is defined on codepoints, and the policy for
      ill-formed input has to be decided before anything consumes it.
- [x] `tools/gen_tables.py`, generated `utf8n_table.h`
      why: the exclusion self-oracle lives in the generator, so it comes before
      any code that uses the tables.  Done in ea8bf72; these six boxes were
      left unticked by the sessions that did the work and are ticked here
      against the commits that did it.  `make test` regenerates the header
      into `scratch/` and `cmp`s it against the committed one, and that check
      has now been watched to fail: a formatting-only change to the generator
      (`%5d` -> `%6d` in one record) is caught by the byte-for-byte diff and by
      nothing else in the suite, 106 passed / 1 failed.
- [x] canonical decomposition (NFD), table-driven
      done in 1ba53cc.
- [x] canonical ordering by Canonical_Combining_Class, stable
      done in 59989b1.
- [x] Hangul decomposition and composition, algorithmic
      done in e0a9186.
- [x] canonical composition (NFC) with the blocked-starter rule
      done in 95fdd9f.
- [x] compatibility decomposition (NFKD, NFKC)
      done in 1ba53cc alongside NFD; all four forms are what the 806,840
      conformance assertions cover.
- [x] `t/conform.c`: the NormalizationTest.txt runner, idempotence, and the
      file's second requirement over every codepoint it does not list
      why: the oracle has to exist before it can be believed, and it scored
      100% on the first run, a reason to distrust the harness.
- [x] mutation testing: watch the harness fail
      why: 806,840 assertions passing on the first run proves nothing until a
      deliberately broken implementation is seen to fail them.  Nine of twelve
      injected defects are caught; two of the three survivors were real gaps
      (decode consuming more than n, the Hangul `<= SCOUNT` boundary) and are
      now pinned by hand.
- [x] `t/diff.py`: differential against CPython with version-skew attribution
      why: the only oracle that works offline, and the one that needs the most
      discipline: 6420 codepoints differ between UCD 15.1.0 and 18.0.0, so
      skew has to be proved per codepoint, not claimed.
- [x] hostile input: 400kB of /dev/urandom through all four forms under ASAN
      why: we decode untrusted bytes; the sizing contract, well-formedness,
      idempotence and clean truncation all have to hold for input nobody
      validated.  This is where the truncation contract turned out to be wrong.
- [x] a skipped oracle must not look like a passing one
      why: offline, `make test` printed 89 passed / 0 failed and exited 0 with
      the conformance file never opened.  There is now an oracle roster and
      UTF8N_REQUIRE_ORACLES=1.
- [x] README, PLAN, score published

### Next, in order

- [x] `NFC_QC`/`NFD_QC` quick-check tables, derived and checked
      why: it is the single largest performance item and the only one with a
      published oracle: the quick-check properties are in the UCD, so the
      fast path's decisions can be diffed against them for every
      codepoint, not trusted.  Nothing else in this list is measurable.
      Done: the generator derives all four properties by normalising every
      codepoint itself and refuses unless its sets equal the UCD's;
      `t/diff.py` checks the C against the same file for all 1,112,064
      codepoints; `t/conform` checks the YES and NO promises on all five
      columns of all 20,171 lines.  Ten mutants, one survivor, which was a
      harness gap and is now closed by 17 hand cases and a 50,000-window
      sweep of random bytes: see the README.
- [x] the skip-ahead fast path inside `utf8n_normalize()`
      why: the tables are dead weight inside the library until something
      consults them, and this is the half that has to be measured rather than
      argued: a copy-the-prefix path is a second code path through every
      caller, so the conformance corpus has to be replayed through it with the
      number of codepoints it skipped reported, or it can be silently unused
      and prove nothing.  Done, and that last sentence was right: with
      `fast_prefix()` returning 0 the entire suite still passes except the
      byte counter, which is why there is one.  ASCII already in the form
      goes 40 to 80 times faster (38.9 -> 1611 MB/s for NFD over 2.2MB) and
      nothing measured gets slower.  Eight mutants, three of them survivors
      first, all three because truncation, well-formedness and overlapping
      buffers had only ever been checked on input where the fast path does
      nothing.
- [x] the sanitizer must be watching a buffer the size of its data
      why: t/nfx -r is the hostile-input mode and its slurp buffer grew by
      doubling, so it was 512KB after reading 400KB and a planted read one
      byte past the data produced zero ASan reports and a green suite.  This
      is the eighth instance of the bug class METHOD.md records finding seven
      times.  One realloc to the exact length fixes it;
      -DUTF8N_PLANT_OVERREAD plus an exit-code-42 check under make test-asan
      is what keeps it fixed, and deleting the realloc turns that case red.
- [ ] resume the fast path after each slow segment
      why: it is one prefix today, so a single precomposed character a hundred
      bytes into a 2.2MB file costs the whole saving for the remaining 2.2MB
      (39.0 MB/s instead of 1611), and mark-dense text gains nothing even when
      it is fully normalised.  Not a one-line change: `in` and `out` may
      overlap arbitrarily, which is why nothing is written until the whole
      input has been read, and a loop that alternates copying and composing
      has to preserve that.  The test already exists in the shape of the
      counter, so the measurement is the deliverable.
- [ ] streaming API: normalise at starter boundaries so a caller can feed
      chunks
      why: the whole-input buffer is the one hard ceiling in the contract, and
      the conformance corpus can be replayed through it a byte at a time, which
      is the test that makes the segmentation argument honest.
- [ ] a two-stage trie keyed on the top 13 bits, replacing the binary searches
      why: only worth doing after the quick-check path, because that path
      removes most of the lookups this would speed up.
- [x] `utf8n_quick_check()` as public API
      why: callers who only need to know whether text is already normalised
      currently have to normalise it and compare, the expensive way to
      answer the cheap question.  Done in the same commit as the tables,
      because a table with no caller cannot be tested and an API with no
      table cannot exist.
