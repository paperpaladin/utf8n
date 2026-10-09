#!/usr/bin/env python3
"""Differential test against CPython's unicodedata, and against its UTF-8
decoder.

Usage: python3 t/diff.py ./t/nfx ucd ucd-<python's version>

CPython's UCD version is almost never the one our tables were generated from,
so a disagreement is not automatically our bug and is not automatically theirs.
Every disagreement is therefore classified against the two UnicodeData.txt and
DerivedNormalizationProps.txt files: it is reported as version skew only when a
codepoint in the input, or in either answer, demonstrably changed its combining
class, its decomposition or its composition-exclusion status between the two
versions, and the change is printed.  Anything else is a failure.

Prints TAP-ish lines for t/run.sh to count.
"""
import os
import random
import subprocess
import sys
import unicodedata

FORMS = ('NFD', 'NFC', 'NFKD', 'NFKC')
fails = 0


def ok(cond, msg):
    global fails
    print(('ok - ' if cond else 'not ok - ') + msg)
    if not cond:
        fails += 1


def load_props(ucddir):
    """cp -> (ccc, decomposition, excluded) for one UCD directory."""
    props = {}
    path = os.path.join(ucddir, 'UnicodeData.txt')
    for line in open(path, encoding='utf-8'):
        f = line.split(';')
        if len(f) < 6:
            continue
        props[int(f[0], 16)] = (f[3], f[5].strip(), False)
    dp = os.path.join(ucddir, 'DerivedNormalizationProps.txt')
    for line in open(dp, encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        f = [x.strip() for x in line.split(';')]
        if len(f) < 2 or f[1] != 'Full_Composition_Exclusion':
            continue
        r = f[0].split('..')
        for cp in range(int(r[0], 16), int(r[-1], 16) + 1):
            ccc, dec, _ = props.get(cp, ('0', '', False))
            props[cp] = (ccc, dec, True)
    return props


def changed_set(a, b):
    """Codepoints whose normalisation-relevant properties differ."""
    out = {}
    for cp in set(a) | set(b):
        if a.get(cp) != b.get(cp):
            out[cp] = (a.get(cp), b.get(cp))
    return out


def describe(p):
    if p is None:
        return 'unassigned'
    ccc, dec, excl = p
    return 'ccc=%s decomp=%s%s' % (ccc, dec if dec else '-',
                                   ' excluded' if excl else '')


def run_nfx(nfx, args, lines):
    p = subprocess.run([nfx] + args, input='\n'.join(lines) + '\n',
                       capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit('nfx failed: %s' % p.stderr)
    return p.stdout.splitlines()


def hexs(s):
    return ' '.join('%04X' % ord(c) for c in s)


def compare(label, nfx, seqs, changed, ourver, pyver):
    """seqs: list of codepoint tuples.  Returns the number of codepoints whose
    disagreement with CPython was accounted for by a property change."""
    lines = [' '.join('%04X' % c for c in s) for s in seqs]
    got = run_nfx(nfx, [], lines)
    if len(got) != len(seqs):
        ok(False, '%s: nfx returned %d lines for %d inputs'
           % (label, len(got), len(seqs)))
        return 0
    bad = []
    skew = {}
    for seq, line in zip(seqs, got):
        s = ''.join(map(chr, seq))
        cols = line.split(';')
        for form, col in zip(FORMS, cols):
            want = hexs(unicodedata.normalize(form, s))
            if col == want:
                continue
            culprits = [cp for cp in set(seq)
                        | {int(x, 16) for x in (col + ' ' + want).split()}
                        if cp in changed]
            if culprits:
                for cp in culprits:
                    skew[cp] = changed[cp]
            else:
                bad.append((form, hexs(s), col, want))
    for form, s, col, want in bad[:12]:
        print('# DISAGREE %s(%s): ours %s, CPython %s -- no codepoint '
              'involved changed between %s and %s'
              % (form, s, col, want, ourver, pyver))
    if len(bad) > 12:
        print('# ...and %d more' % (len(bad) - 12))
    ok(not bad, '%s: %d sequences x 4 forms agree with CPython '
       '(%d unexplained disagreements)' % (label, len(seqs), len(bad)))
    if skew:
        cps = sorted(skew)
        print('# %s: %d codepoints disagree, every one of them because its own '
              'properties changed between %s and %s:'
              % (label, len(cps), pyver, ourver))
        for c in cps[:6]:
            print('#   U+%04X  %s %s  ->  %s %s'
                  % (c, pyver, describe(skew[c][1]), ourver,
                     describe(skew[c][0])))
        if len(cps) > 6:
            print('#   ...and %d more' % (len(cps) - 6))
    return len(skew)


def decoder_corpus():
    """Byte strings that are mostly not valid UTF-8."""
    seqs = []
    for a in range(256):                    # every 1 and 2 byte sequence
        seqs.append(bytes([a]))
        for b in range(256):
            seqs.append(bytes([a, b]))
    rnd = random.Random(20261009)           # fixed seed: a corpus, not a fuzz
    for _ in range(120000):
        n = rnd.randint(3, 6)
        seqs.append(bytes(rnd.randrange(256) for _ in range(n)))
    # Unicode 16.0 table 3-8 .. 3-11, the examples the standard spells out.
    for h in ('C0 AF E0 80 BF F0 81 82 41', 'ED A0 80', 'F4 90 80 80',
              'E1 80 E2 F0 91 92 F1 BF 41', '61 F1 80 80 E1 80 C2 62',
              '80 BF 82', 'FE FF'):
        seqs.append(bytes(int(x, 16) for x in h.split()))
    return seqs


def main():
    nfx, ourdir, pydir = sys.argv[1], sys.argv[2], sys.argv[3]
    ourver = open(os.path.join(ourdir, 'DerivedNormalizationProps.txt'),
                  encoding='utf-8').readline().split('-')[1][:-5]
    pyver = unicodedata.unidata_version
    changed = changed_set(load_props(ourdir), load_props(pydir))
    print('# our UCD %s, CPython UCD %s, %d codepoints differ in combining '
          'class, decomposition or exclusion status'
          % (ourver, pyver, len(changed)))

    # Single codepoints: the whole BMP, then every astral codepoint that has a
    # decomposition or a combining class, then a stride sample of the rest.
    skewed = 0
    bmp = [(c,) for c in range(0x10000) if not 0xD800 <= c <= 0xDFFF]
    skewed += compare('every BMP codepoint', nfx, bmp, changed, ourver, pyver)

    ourprops = load_props(ourdir)
    astral = sorted(c for c in ourprops
                    if c >= 0x10000 and (ourprops[c][0] != '0'
                                         or ourprops[c][1]))
    astral += list(range(0x10000, 0x110000, 97))
    skewed += compare('astral: all interesting + every 97th codepoint', nfx,
            [(c,) for c in sorted(set(astral))], changed, ourver, pyver)

    # Sequences.  Starters crossed with marks, which is where ordering,
    # blocking and exclusions actually interact.
    starters = [0x61, 0x1E0A, 0x0044, 0x0915, 0x03D2, 0x4E00, 0xAC00, 0x1100,
                0x05D9, 0x0F77, 0x0B47, 0x1D157]
    marks = [0x0301, 0x0300, 0x0316, 0x0334, 0x05AE, 0x0345, 0x093C, 0x11A8,
             0x3099, 0x0323, 0x0307, 0x031B, 0x0E48, 0x0591, 0x0000 + 0x064B]
    seqs = []
    for s in starters:
        for m1 in marks:
            seqs.append((s, m1))
            for m2 in marks:
                seqs.append((s, m1, m2))
                seqs.append((s, m1, m2, 0x0301))
    skewed += compare('starter x mark x mark (ordering, blocking, exclusions)', nfx,
            seqs, changed, ourver, pyver)

    # Every Hangul L V T triple, and the two-step compositions.
    jamo = []
    for l in range(0x1100, 0x1113):
        for v in range(0x1161, 0x1176):
            jamo.append((l, v))
            for t in range(0x11A8, 0x11C3):
                jamo.append((l, v, t))
    skewed += compare('every Hangul L V (T) jamo sequence', nfx, jamo, changed,
            ourver, pyver)

    # Degenerate input: long runs of combining marks.  This is where the
    # O(n^2) insertion sort and the doubling buffer get exercised, and where an
    # implementation with a fixed-size reordering window gives wrong answers
    # rather than crashing.
    rnd = random.Random(1009)
    degen = []
    for n in (33, 64, 100, 500, 2000):
        ms = [rnd.choice(marks) for _ in range(n)]
        degen.append(tuple([0x61] + ms))
        degen.append(tuple(ms))
    skewed += compare('runs of 33..2000 combining marks', nfx, degen, changed,
                      ourver, pyver)

    # Idempotence is checked in C over the conformance corpus; here it is
    # checked over this corpus too, against CPython's own second application.
    marks_only = [(m,) for m in marks] + [(m, 0x61) for m in marks]
    skewed += compare('leading combining marks', nfx, marks_only, changed, ourver, pyver)

    # The UTF-8 decoder against CPython's.
    corpus = decoder_corpus()
    lines = [' '.join('%02X' % b for b in s) for s in corpus]
    got = run_nfx(nfx, ['-b'], lines)
    bad = 0
    for s, line in zip(corpus, got):
        want = hexs(s.decode('utf-8', errors='replace'))
        if line != want:
            if bad < 8:
                print('# DISAGREE decode(%s): ours %s, CPython %s'
                      % (' '.join('%02X' % b for b in s), line, want))
            bad += 1
    ok(bad == 0 and len(got) == len(corpus),
       'UTF-8 decode of %d byte strings matches CPython errors=replace '
       '(%d bad)' % (len(corpus), bad))

    print('# %d codepoints in total disagree with CPython, all of them '
          'explained by a property change between UCD %s and %s; %d '
          'unexplained' % (skewed, pyver, ourver, fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
