# PLAN

## Contract

`utf8n` is a single C11 file that normalises UTF-8 text to any of the four
Unicode normalisation forms.

- `utf8n_normalize(form, in, inlen, out, outcap)` where `form` is one of
  `UTF8N_NFD`, `UTF8N_NFC`, `UTF8N_NFKD`, `UTF8N_NFKC`.  It returns the number
  of bytes the result needs, writing at most `outcap` of them, so the snprintf
  calling convention: call once to size, once to fill.
- `utf8n_decode()` / `utf8n_encode()` are the UTF-8 layer, public because the
  replacement policy for ill-formed input is part of the contract rather than an
  implementation detail.
- `utf8n_ccc()` exposes Canonical_Combining_Class, because the canonical
  ordering test has to be able to check it independently.
- Tables come from `tools/gen_tables.py` reading the UCD.  The generated header
  is committed so that building needs gcc and nothing else.
- Not in the contract: collation, case folding, grapheme segmentation, a
  streaming API, locale anything, NFKC_Casefold.

## Hard invariant

For every one of the 20,171 non-comment lines of `NormalizationTest.txt`, all
twelve relations that line asserts hold, and additionally every form is
idempotent on all five columns -- `f(f(x)) == f(x)` for f in NFC, NFD, NFKC,
NFKD.  Conformance is not "most lines pass"; the file is the specification of
what the four functions mean, and a failing line is a wrong answer.

Two consequences shape the code.  Idempotence over an external corpus is what
catches a canonical-ordering bug that a hand-written test would miss, because
reordering that is not stable shows up only when you normalise twice.  And
because the test file covers all five columns in both directions, a composition
table that forgets exclusions cannot pass by accident.

## Oracle

Ranked by how little we control them, strongest first.

1. **`NormalizationTest.txt` from the UCD.**  Official conformance data,
   19,000-odd lines, each asserting twelve equalities we did not choose.
   `t/conform.c` runs all of them plus idempotence, and prints a score.
2. **`DerivedNormalizationProps.txt`, property `Full_Composition_Exclusion`.**
   `tools/gen_tables.py` *derives* that set (listed exclusions, plus singleton
   decompositions, plus non-starter decompositions) and then asserts its derived
   set equals the UCD's published one, exactly, or refuses to emit tables.  This
   is the oracle for the composition-exclusion rules, and it is independent of
   the normalisation test data.
3. **CPython's `unicodedata.normalize`.**  Available offline, so it is the
   oracle when the UCD files cannot be fetched.  `t/diff.py` compares all four
   forms over the whole BMP plus a sample of the astral planes.  Python's UCD
   version differs from ours, so disagreements are classified against the two
   `UnicodeData.txt` files: a disagreement is tolerated only when the codepoint
   involved demonstrably changed between the two versions, and anything else
   fails the run.
4. **CPython's UTF-8 decoder.**  `bytes.decode('utf-8', errors='replace')`
   implements the same maximal-subpart policy, so `t/diff.py` checks ill-formed
   input against it rather than against our own opinion.

## Non-goals

- Streaming / incremental normalisation.  `utf8n_normalize()` buffers the whole
  input as codepoints.
- NFKC_Casefold, collation keys, grapheme clusters, `wcwidth` (that one lives in
  `vtparse`, deliberately not duplicated here).
- Quick-check fast paths.  Always decomposing is slower and much shorter, and
  the conformance file does not reward the cleverness.

## Commit sequence

- [x] UTF-8 decode and encode, with the maximal-subpart replacement policy
      pinned by table 3-7 of the standard
      why: everything downstream is defined on codepoints, and the policy for
      ill-formed input has to be decided before anything consumes it.
- [ ] `tools/gen_tables.py`, generated `utf8n_table.h`
      why: the exclusion self-oracle lives in the generator, so it comes before
      any code that uses the tables.
- [ ] canonical decomposition (NFD), table-driven
- [ ] canonical ordering by Canonical_Combining_Class, stable
- [ ] Hangul decomposition and composition, algorithmic
- [ ] canonical composition (NFC) with the blocked-starter rule
- [ ] compatibility decomposition (NFKD, NFKC)
- [ ] `t/conform.c`: the NormalizationTest.txt runner and its score
- [ ] idempotence over the whole conformance corpus
- [ ] `t/diff.py`: differential against CPython with version-skew attribution
- [ ] README, PLAN, score published
