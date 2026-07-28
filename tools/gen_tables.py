#!/usr/bin/env python3
"""Generate utf8n_table.h from the Unicode Character Database.

Reads UnicodeData.txt, CompositionExclusions.txt and DerivedNormalizationProps.txt
out of a UCD directory and writes the C tables utf8n.c binary-searches.

Usage: python3 tools/gen_tables.py ucd/ utf8n_table.h

Everything here is derived, not transcribed: the full (recursive) canonical and
compatibility decompositions are expanded at generation time so that utf8n.c
never recurses, and the composition table is inverted from the canonical
decompositions with the three exclusion rules applied.  The derived
Full_Composition_Exclusion set is checked against the UCD's own derived
property as a self-oracle -- see check_exclusions() -- and so are the four
quick-check properties, which this file derives by normalising every codepoint
with its own small implementation; see derive_quickcheck().
"""
import sys, os, re

SBASE, LBASE, VBASE, TBASE = 0xAC00, 0x1100, 0x1161, 0x11A7
LCOUNT, VCOUNT, TCOUNT = 19, 21, 28
NCOUNT, SCOUNT = VCOUNT * TCOUNT, LCOUNT * VCOUNT * TCOUNT


def parse_unicodedata(path):
    """-> (ccc{cp:int}, canon{cp:[cps]}, compat{cp:[cps]})"""
    ccc, canon, compat = {}, {}, {}
    for line in open(path, encoding='utf-8'):
        f = line.split(';')
        if len(f) < 6:
            continue
        cp = int(f[0], 16)
        if f[3] != '0':
            ccc[cp] = int(f[3])
        d = f[5].strip()
        if not d:
            continue
        if d.startswith('<'):
            compat[cp] = [int(x, 16) for x in d.split('>', 1)[1].split()]
        else:
            canon[cp] = [int(x, 16) for x in d.split()]
    return ccc, canon, compat


def ucd_version(path):
    """UnicodeData.txt carries no header, so take the version from a file
    that does."""
    first = open(path, encoding='utf-8').readline()
    m = re.search(r'-(\d+\.\d+\.\d+)\.txt', first)
    return m.group(1) if m else 'unknown'


def parse_exclusions(path):
    out = set()
    for line in open(path, encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        if line:
            out.add(int(line.split('.')[0], 16))
    return out


def parse_derived(path, prop):
    """Codepoints with the named binary property from DerivedNormalizationProps."""
    out = set()
    for line in open(path, encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        f = [x.strip() for x in line.split(';')]
        if len(f) < 2 or f[1] != prop:
            continue
        r = f[0].split('..')
        lo = int(r[0], 16)
        hi = int(r[-1], 16)
        out.update(range(lo, hi + 1))
    return out


def parse_qc(path, prop):
    """-> (no, maybe) codepoint sets for a quick-check property.

    These lines carry a third field, N or M; Yes is the @missing default and is
    not listed, so a codepoint absent from both sets is Yes.
    """
    no, maybe = set(), set()
    for line in open(path, encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        f = [x.strip() for x in line.split(';')]
        if len(f) < 3 or f[1] != prop:
            continue
        r = f[0].split('..')
        rng = range(int(r[0], 16), int(r[-1], 16) + 1)
        (no if f[2] == 'N' else maybe).update(rng)
    return no, maybe


def expand(cp, canon, compat, compatibility):
    """Full recursive decomposition of one codepoint, Hangul excluded."""
    src = canon.get(cp)
    if src is None and compatibility:
        src = compat.get(cp)
    if src is None:
        return [cp]
    out = []
    for c in src:
        out.extend(expand(c, canon, compat, compatibility))
    return out


def check_exclusions(canon, ccc, listed, derived):
    """Full_Composition_Exclusion = listed + singletons + non-starter decomps.

    The UCD publishes that set; we derive it.  If the two disagree the rest of
    the table is not to be trusted, so this is a hard error.
    """
    mine = set(listed)
    for cp, d in canon.items():
        if len(d) == 1 or ccc.get(cp, 0) != 0 or ccc.get(d[0], 0) != 0:
            mine.add(cp)
    if mine != derived:
        raise SystemExit('Full_Composition_Exclusion mismatch: '
                         'only ours %s, only UCD %s'
                         % (sorted(mine - derived)[:8], sorted(derived - mine)[:8]))
    return mine


# Quick-check values, two bits per form, in the order of enum utf8n_form.
QC_NO, QC_MAYBE = 1, 2
QC_FORMS = ('NFD', 'NFC', 'NFKD', 'NFKC')


def derive_quickcheck(ccc, canon, compat, excl, propspath):
    """-> {cp: packed bits}: two bits per form, enum utf8n_form order.

    NFx_QC answers "is this text already in form x" without normalising it.
    No means it never is, Yes means it always is, Maybe means it depends on
    what comes before.  Deriving the property means normalising every
    codepoint, so what follows is a second implementation of the algorithm
    that utf8n.c implements in C.  The duplication is the point: the two have
    never seen each other's answers, and each is compared against the UCD.

    The No half is the straightforward one: NFx_QC(cp) is No exactly when
    NFx(cp) != cp.  All four sets fall out of normalising 1,114,112
    codepoints four ways.

    Maybe is the half that is easy to get wrong.  A codepoint is Maybe when
    something in front of it can compose with it, which starts from the second
    elements of the primary composites plus the Hangul V and T jamo: 120
    codepoints.  That is not the whole set.  U+113C5 TULU-TIGALARI VOWEL SIGN
    AI decomposes to U+113C2 twice, and U+113C2 is itself a composition
    second, so <U+113C2, U+113C5> recomposes to <U+113C5, U+113C2> and U+113C5
    has to be Maybe too.  The closure over "my decomposition starts with
    something that is already Maybe" adds the twelve such codepoints, all in
    scripts added in Unicode 16 and 18, and the UCD's published set is what
    says so: 132, not 120.
    """
    comp = {(d[0], d[1]): cp for cp, d in canon.items()
            if len(d) == 2 and cp not in excl}

    def decompose(cp, compatibility):
        if 0 <= cp - SBASE < SCOUNT:
            s, t = cp - SBASE, (cp - SBASE) % TCOUNT
            r = [LBASE + s // NCOUNT, VBASE + s % NCOUNT // TCOUNT]
            return r + [TBASE + t] if t else r
        return expand(cp, canon, compat, compatibility)

    def order(p):
        p = list(p)
        for i in range(1, len(p)):
            ch, cc = p[i], ccc.get(p[i], 0)
            if cc == 0:
                continue
            j = i
            while j > 0 and ccc.get(p[j - 1], 0) > cc:
                p[j] = p[j - 1]
                j -= 1
            p[j] = ch
        return p

    def compose_pair(a, b):
        if 0 <= a - LBASE < LCOUNT and 0 <= b - VBASE < VCOUNT:
            return SBASE + ((a - LBASE) * VCOUNT + (b - VBASE)) * TCOUNT
        if (0 <= a - SBASE < SCOUNT and (a - SBASE) % TCOUNT == 0
                and TBASE < b < TBASE + TCOUNT):
            return a + (b - TBASE)
        return comp.get((a, b), 0)

    def compose(p):
        if not p:
            return p
        out, starter = [p[0]], 0
        last = 256 if ccc.get(p[0], 0) else 0
        for ch in p[1:]:
            cc = ccc.get(ch, 0)
            c = compose_pair(out[starter], ch)
            if c and (last < cc or last == 0):
                out[starter] = c
                continue
            if cc == 0:
                starter = len(out)
            last = cc
            out.append(ch)
        return out

    def nf(cp, form):
        p = order(decompose(cp, form in ('NFKD', 'NFKC')))
        return compose(p) if form in ('NFC', 'NFKC') else p

    maybe = set(b for _, b in comp)
    maybe |= set(range(VBASE, VBASE + VCOUNT))
    maybe |= set(range(TBASE + 1, TBASE + TCOUNT))
    while True:
        more = set(cp for cp in canon
                   if cp not in maybe and decompose(cp, False)[0] in maybe)
        if not more:
            break
        maybe |= more

    bits = {}
    for i, form in enumerate(QC_FORMS):
        no = set(cp for cp in range(0x110000) if nf(cp, form) != [cp])
        mb = (maybe - no) if form in ('NFC', 'NFKC') else set()
        ucd_no, ucd_mb = parse_qc(propspath, form + '_QC')
        if no != ucd_no or mb != ucd_mb:
            raise SystemExit(
                '%s_QC mismatch: No only ours %s only UCD %s; '
                'Maybe only ours %s only UCD %s'
                % (form, sorted(no - ucd_no)[:8], sorted(ucd_no - no)[:8],
                   sorted(mb - ucd_mb)[:8], sorted(ucd_mb - mb)[:8]))
        for cp in no:
            bits[cp] = bits.get(cp, 0) | (QC_NO << (2 * i))
        for cp in mb:
            bits[cp] = bits.get(cp, 0) | (QC_MAYBE << (2 * i))
    return bits


def main():
    ucd, outpath = sys.argv[1], sys.argv[2]
    p = lambda n: os.path.join(ucd, n)
    ver = ucd_version(p('DerivedNormalizationProps.txt'))
    ccc, canon, compat = parse_unicodedata(p('UnicodeData.txt'))
    listed = parse_exclusions(p('CompositionExclusions.txt'))
    derived = parse_derived(p('DerivedNormalizationProps.txt'),
                            'Full_Composition_Exclusion')
    excl = check_exclusions(canon, ccc, listed, derived)
    qcbits = derive_quickcheck(ccc, canon, compat, excl,
                               p('DerivedNormalizationProps.txt'))

    # Decomposition records, keyed by codepoint, sorted.  Sequences live in one
    # shared pool; identical sequences (and NFD == NFKD, the common case) are
    # stored once.
    pool, poolidx = [], {}

    def intern(seq):
        key = tuple(seq)
        if key not in poolidx:
            poolidx[key] = len(pool)
            pool.extend(seq)
        return poolidx[key]

    recs = []
    for cp in sorted(set(canon) | set(compat)):
        nfd = expand(cp, canon, compat, False)
        nfkd = expand(cp, canon, compat, True)
        nfd = [] if nfd == [cp] else nfd
        nfkd = [] if nfkd == [cp] else nfkd
        recs.append((cp, intern(nfd) if nfd else 0, len(nfd),
                     intern(nfkd) if nfkd else 0, len(nfkd)))

    maxexp = max(max(r[2], r[4]) for r in recs)
    if len(pool) > 0xFFFF:
        raise SystemExit('pool has %d words, offsets no longer fit in uint16; '
                         'widen struct utf8n_dec' % len(pool))
    if maxexp > 0xFF:
        raise SystemExit('longest decomposition is %d, no longer fits in uint8'
                         % maxexp)

    # CCC as ranges.
    cccr = []
    for cp in sorted(ccc):
        if cccr and cccr[-1][1] == cp - 1 and cccr[-1][2] == ccc[cp]:
            cccr[-1][1] = cp
        else:
            cccr.append([cp, cp, ccc[cp]])

    # Quick-check values as ranges, the same shape as the combining classes.
    # 529 ranges for 17,274 non-Yes codepoints: Hangul alone is one of them.
    qcr = []
    for cp in sorted(qcbits):
        if qcr and qcr[-1][1] == cp - 1 and qcr[-1][2] == qcbits[cp]:
            qcr[-1][1] = cp
        else:
            qcr.append([cp, cp, qcbits[cp]])

    # Primary composites: invert the canonical decompositions that are pairs and
    # are not excluded.  Sorted by (first, second) for binary search.
    comp = sorted((d[0], d[1], cp) for cp, d in canon.items()
                  if len(d) == 2 and cp not in excl)

    w = open(outpath, 'w', encoding='utf-8')
    e = w.write
    e('/* utf8n_table.h -- GENERATED by tools/gen_tables.py.  Do not edit.\n'
      ' * SPDX-License-Identifier: GPL-3.0-or-later\n'
      ' * Source: Unicode Character Database %s\n'
      ' */\n#ifndef UTF8N_TABLE_H\n#define UTF8N_TABLE_H\n#include <stdint.h>\n\n'
      '#define UTF8N_UCD_VERSION "%s"\n\n'
      '/* Longest decomposition of a single codepoint, in codepoints: the\n'
      ' * expansion factor a caller sizing a fixed buffer has to allow for.\n'
      ' * U+FDFA under NFKD is what sets it. */\n'
      '#define UTF8N_MAX_EXPANSION %d\n\n' % (ver, ver, maxexp))
    e('/* Fully expanded decomposition sequences, shared by NFD and NFKD. */\n'
      'static const uint32_t utf8n_pool[%d] = {' % len(pool))
    for i, c in enumerate(pool):
        e(('\n\t' if i % 8 == 0 else ' ') + '0x%04X,' % c)
    e('\n};\n\n')
    e('struct utf8n_dec { uint32_t cp; uint16_t d_off, k_off; uint8_t d_len, k_len; };\n'
      'static const struct utf8n_dec utf8n_dec_tab[%d] = {' % len(recs))
    for i, (cp, do, dl, ko, kl) in enumerate(recs):
        e(('\n\t' if i % 4 == 0 else ' ')
          + '{0x%05X,%5d,%5d,%2d,%2d},' % (cp, do, ko, dl, kl))
    e('\n};\n\n')
    e('struct utf8n_cccr { uint32_t lo, hi; uint8_t ccc; };\n'
      'static const struct utf8n_cccr utf8n_ccc_tab[%d] = {' % len(cccr))
    for i, (lo, hi, v) in enumerate(cccr):
        e(('\n\t' if i % 3 == 0 else ' ') + '{0x%05X,0x%05X,%3d},' % (lo, hi, v))
    e('\n};\n\n')
    e('/* NFx_QC: two bits per form in enum utf8n_form order (NFD, NFC, NFKD,\n'
      ' * NFKC), 0 Yes, 1 No, 2 Maybe.  A codepoint in none of these ranges is\n'
      ' * Yes for all four forms, which is all but %d of them.  Derived by\n'
      ' * normalising every codepoint and checked against the UCD\'s own\n'
      ' * NFD_QC/NFC_QC/NFKD_QC/NFKC_QC. */\n'
      'struct utf8n_qcr { uint32_t lo, hi; uint8_t qc; };\n'
      'static const struct utf8n_qcr utf8n_qc_tab[%d] = {' % (len(qcbits), len(qcr)))
    for i, (lo, hi, v) in enumerate(qcr):
        e(('\n\t' if i % 3 == 0 else ' ') + '{0x%05X,0x%05X,0x%02X},' % (lo, hi, v))
    e('\n};\n\n')
    e('/* Primary composites: (first, second) -> composite, exclusions removed. */\n'
      'struct utf8n_comp { uint32_t a, b, c; };\n'
      'static const struct utf8n_comp utf8n_comp_tab[%d] = {' % len(comp))
    for i, (a, b, c) in enumerate(comp):
        e(('\n\t' if i % 4 == 0 else ' ')
          + '{0x%05X,0x%05X,0x%05X},' % (a, b, c))
    e('\n};\n\n#endif /* UTF8N_TABLE_H */\n')
    w.close()
    sys.stderr.write('UCD %s: %d decomp records, %d pool words, %d ccc ranges, '
                     '%d composites, %d exclusions, %d quick-check ranges '
                     'over %d non-Yes codepoints\n'
                     % (ver, len(recs), len(pool), len(cccr), len(comp),
                        len(excl), len(qcr), len(qcbits)))


if __name__ == '__main__':
    main()
