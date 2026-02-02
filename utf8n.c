/* utf8n -- Unicode normalisation (NFD, NFC, NFKD, NFKC) over UTF-8.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One file, C11, no dependencies.  The tables in utf8n_table.h are generated
 * from the Unicode Character Database by tools/gen_tables.py.
 */
#include <stdlib.h>
#include <string.h>

#include "utf8n.h"
#include "utf8n_table.h"

/* --- UTF-8 ---------------------------------------------------------------
 *
 * Policy for ill-formed input, held to everywhere: one U+FFFD per maximal
 * ill-formed subpart, as recommended by Unicode TR#36 and required by the
 * WHATWG Encoding Standard.  That means a truncated three-byte sequence
 * becomes exactly one U+FFFD, not three, and the byte that follows is
 * re-examined as a fresh lead byte.  Overlong forms, surrogates encoded as
 * CESU-8, and anything above U+10FFFF are ill-formed and get the same
 * treatment.  Normalisation is then defined on the replaced text, so
 * utf8n_normalize() always returns well-formed UTF-8.
 */

/* Lowest and highest legal second byte for each lead byte that starts a
 * multi-byte sequence; this is what rejects overlongs (0xE0 0x80), surrogates
 * (0xED 0xA0) and out-of-range (0xF4 0x90) without a post-hoc range check. */
static void second_byte_range(unsigned lead, unsigned *lo, unsigned *hi)
{
	*lo = 0x80;
	*hi = 0xBF;
	if (lead == 0xE0)
		*lo = 0xA0;
	else if (lead == 0xED)
		*hi = 0x9F;
	else if (lead == 0xF0)
		*lo = 0x90;
	else if (lead == 0xF4)
		*hi = 0x8F;
}

size_t utf8n_decode(const unsigned char *s, size_t n, uint32_t *cp)
{
	unsigned lead, lo, hi;
	size_t len, i;
	uint32_t v;

	*cp = 0xFFFD;
	if (n == 0)
		return 0;
	lead = s[0];
	if (lead < 0x80) {
		*cp = lead;
		return 1;
	}
	if (lead < 0xC2 || lead > 0xF4)	/* continuation byte, overlong C0/C1,
					 * or a lead above the Unicode range */
		return 1;
	len = lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
	v = lead & (0x7Fu >> len);
	second_byte_range(lead, &lo, &hi);
	for (i = 1; i < len; i++) {
		if (i >= n)
			return i;	/* truncated: one U+FFFD for what we saw */
		if (s[i] < lo || s[i] > hi)
			return i;	/* ill-formed subpart ends before s[i] */
		v = (v << 6) | (s[i] & 0x3Fu);
		lo = 0x80;
		hi = 0xBF;
	}
	*cp = v;
	return len;
}

size_t utf8n_encode(uint32_t cp, unsigned char *out)
{
	if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
		cp = 0xFFFD;
	if (cp < 0x80) {
		out[0] = (unsigned char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (unsigned char)(0xC0 | (cp >> 6));
		out[1] = (unsigned char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (unsigned char)(0xE0 | (cp >> 12));
		out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (unsigned char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (unsigned char)(0xF0 | (cp >> 18));
	out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (unsigned char)(0x80 | (cp & 0x3F));
	return 4;
}

/* --- tables ---------------------------------------------------------------
 *
 * Three binary searches over generated, sorted arrays.  No hashing, no
 * two-stage trie: at 422 combining-class ranges and 5982 decomposition records
 * a search is 9 and 13 comparisons, which is not the bottleneck in a routine
 * that also walks the string four times.
 * shortcut: linear-memory whole-input buffering and binary-search lookups;
 * if this ever shows up in a profile, the upgrade is a two-stage trie keyed on
 * the top 13 bits plus a quick-check table to skip already-normalised runs.
 */

const char *utf8n_ucd_version(void)
{
	return UTF8N_UCD_VERSION;
}

int utf8n_max_expansion(void)
{
	return UTF8N_MAX_EXPANSION;
}

#define NELEM(a) (sizeof(a) / sizeof(a)[0])

int utf8n_ccc(uint32_t cp)
{
	size_t lo = 0, hi = NELEM(utf8n_ccc_tab);

	while (lo < hi) {
		size_t m = lo + (hi - lo) / 2;
		if (cp < utf8n_ccc_tab[m].lo)
			hi = m;
		else if (cp > utf8n_ccc_tab[m].hi)
			lo = m + 1;
		else
			return utf8n_ccc_tab[m].ccc;
	}
	return 0;
}
static const struct utf8n_dec *dec_find(uint32_t cp)
{
	size_t lo = 0, hi = NELEM(utf8n_dec_tab);

	while (lo < hi) {
		size_t m = lo + (hi - lo) / 2;
		if (cp < utf8n_dec_tab[m].cp)
			hi = m;
		else if (cp > utf8n_dec_tab[m].cp)
			lo = m + 1;
		else
			return &utf8n_dec_tab[m];
	}
	return NULL;
}

/* --- the codepoint buffer -------------------------------------------------
 *
 * Normalisation is three passes over a codepoint array, so there is an array.
 * It grows by doubling and lives for one call.  The worst-case expansion is
 * U+FDFA, which decomposes to 18 codepoints under NFKD, but reserving 18n up
 * front would mean a 72MB allocation for a 1MB input, so grow instead.
 */
struct cpbuf {
	uint32_t *p;
	size_t n, cap;
	int err;
};

static void cpbuf_push(struct cpbuf *b, uint32_t cp)
{
	if (b->err)
		return;
	if (b->n == b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 64;
		uint32_t *p = realloc(b->p, cap * sizeof *p);
		if (!p) {
			b->err = 1;
			return;
		}
		b->p = p;
		b->cap = cap;
	}
	b->p[b->n++] = cp;
}

/* --- Hangul --------------------------------------------------------------
 *
 * Hangul syllables are not in the decomposition table: UnicodeData.txt does not
 * list them, and 11,172 records would dwarf everything else.  UAX #15 section
 * 'Hangul Syllable Decomposition' gives the arithmetic instead, and these are
 * its constant names.
 */
#define SBASE 0xAC00u
#define LBASE 0x1100u
#define VBASE 0x1161u
#define TBASE 0x11A7u
#define LCOUNT 19u
#define VCOUNT 21u
#define TCOUNT 28u
#define NCOUNT (VCOUNT * TCOUNT)	/* 588 */
#define SCOUNT (LCOUNT * NCOUNT)	/* 11172 */

/* --- decomposition --------------------------------------------------------
 *
 * The table already holds the full, recursive decomposition, so this is one
 * lookup and a copy.  A codepoint with no decomposition, and a codepoint whose
 * canonical decomposition is empty but whose compatibility decomposition is
 * not, both fall through to "emit as is" via the zero length.
 */
static void decompose(struct cpbuf *b, uint32_t cp, int compat)
{
	const struct utf8n_dec *d;
	unsigned off, len, i;

	if (cp - SBASE < SCOUNT) {		/* unsigned wrap is the range test */
		uint32_t s = cp - SBASE, t = s % TCOUNT;

		cpbuf_push(b, LBASE + s / NCOUNT);
		cpbuf_push(b, VBASE + s % NCOUNT / TCOUNT);
		if (t)				/* TBASE itself means 'no trailer' */
			cpbuf_push(b, TBASE + t);
		return;
	}
	d = dec_find(cp);
	if (d) {
		off = compat && d->k_len ? d->k_off : d->d_off;
		len = compat && d->k_len ? d->k_len : d->d_len;
		if (len) {
			for (i = 0; i < len; i++)
				cpbuf_push(b, utf8n_pool[off + i]);
			return;
		}
	}
	cpbuf_push(b, cp);
}

/* --- canonical ordering ---------------------------------------------------
 *
 * UAX #15 defines this as "repeatedly swap any adjacent pair A B where
 * ccc(A) > ccc(B) > 0".  An insertion sort that moves each non-starter left
 * past strictly-greater classes is the same permutation and runs in one pass.
 *
 * The > is strict, which is the whole correctness argument: marks of equal
 * class must keep their original order, because a+U+0300+U+0301 and
 * a+U+0301+U+0300 are genuinely different text.  A sort with >= would still
 * pass a test for "classes are ascending" while corrupting that, which is why
 * the oracle to trust here is idempotence over a corpus and not a hand-written
 * ordering check.  Starters have class 0 and so are never crossed.
 */
static void canonical_order(uint32_t *p, size_t n)
{
	size_t i, j;

	for (i = 1; i < n; i++) {
		uint32_t ch = p[i];
		int cc = utf8n_ccc(ch);

		if (cc == 0)
			continue;
		for (j = i; j > 0 && utf8n_ccc(p[j - 1]) > cc; j--)
			p[j] = p[j - 1];
		p[j] = ch;
	}
}

/* --- composition ----------------------------------------------------------
 *
 * compose_pair() is the primary-composite lookup.  The table it searches was
 * built by inverting the canonical decompositions, minus Full_Composition_
 * Exclusion, so three classes of pair are absent from it by construction and
 * must stay absent: script-specific listed exclusions (U+0915 U+093C must not
 * become U+0958), singleton decompositions (U+2126 OHM SIGN decomposes to
 * U+03A9 and must never come back, which is what makes NFC lossy on purpose),
 * and decompositions whose first character is not a starter.
 *
 * Hangul is arithmetic again, in two halves: L + V -> LV, and LV + T -> LVT.
 * The second half only applies when the syllable has no trailer already, and
 * b must be strictly greater than TBASE because TBASE is the filler slot.
 */
static uint32_t compose_pair(uint32_t a, uint32_t b)
{
	size_t lo = 0, hi = NELEM(utf8n_comp_tab);

	if (a - LBASE < LCOUNT && b - VBASE < VCOUNT)
		return SBASE + ((a - LBASE) * VCOUNT + (b - VBASE)) * TCOUNT;
	if (a - SBASE < SCOUNT && (a - SBASE) % TCOUNT == 0 &&
	    b > TBASE && b < TBASE + TCOUNT)
		return a + (b - TBASE);

	while (lo < hi) {
		size_t m = lo + (hi - lo) / 2;
		if (a < utf8n_comp_tab[m].a ||
		    (a == utf8n_comp_tab[m].a && b < utf8n_comp_tab[m].b))
			hi = m;
		else if (a > utf8n_comp_tab[m].a ||
			 (a == utf8n_comp_tab[m].a && b > utf8n_comp_tab[m].b))
			lo = m + 1;
		else
			return utf8n_comp_tab[m].c;
	}
	return 0;
}

/* Canonical composition in place, over an already decomposed and ordered
 * array.  Returns the new length.
 *
 * This is UAX #15's algorithm: walk forward holding the last starter, and
 * compose the current character into it unless it is *blocked*.  Blocked means
 * some character between the starter and here has a combining class greater
 * than or equal to this one -- last_cc >= cc fails to compose -- which is why
 * a+U+0300+U+0301 is U+00E0 U+0301 and not U+00E1 U+0300.  last_cc == 0 is the
 * special case for "the starter is immediately before us", where nothing
 * intervenes and composition is always allowed.
 *
 * The 256 is the standard's own trick for a leading non-starter: a string that
 * begins with a combining mark has no starter to compose into, and setting
 * last_cc above every real class (254 is the maximum) blocks everything until
 * a real starter shows up.  Honesty about this line: removing it changes no
 * answer, and no oracle in this repo fails without it, because a pair whose
 * first character is a non-starter is a Full_Composition_Exclusion and is
 * therefore absent from the table anyway, so compose_pair() already returns 0.
 * It stays because it is the published formulation and because that argument
 * depends on the exclusion table being right, which is a different file's job.
 */
static size_t compose(uint32_t *p, size_t n)
{
	size_t starter = 0, w = 1, i;
	int last_cc;

	if (n == 0)
		return 0;
	last_cc = utf8n_ccc(p[0]);
	if (last_cc != 0)
		last_cc = 256;

	for (i = 1; i < n; i++) {
		uint32_t ch = p[i], comp;
		int cc = utf8n_ccc(ch);

		comp = compose_pair(p[starter], ch);
		if (comp && (last_cc < cc || last_cc == 0)) {
			p[starter] = comp;
			continue;
		}
		if (cc == 0)
			starter = w;
		last_cc = cc;
		p[w++] = ch;
	}
	return w;
}

size_t utf8n_normalize(enum utf8n_form form, const char *in, size_t inlen,
		       char *out, size_t outcap)
{
	int compat = form == UTF8N_NFKD || form == UTF8N_NFKC;
	struct cpbuf b = {NULL, 0, 0, 0};
	size_t i = 0, need = 0, k;

	while (i < inlen) {
		uint32_t cp;
		i += utf8n_decode((const unsigned char *)in + i, inlen - i, &cp);
		decompose(&b, cp, compat);
	}
	if (b.err) {
		free(b.p);
		return (size_t)-1;
	}
	canonical_order(b.p, b.n);
	if (form == UTF8N_NFC || form == UTF8N_NFKC)
		b.n = compose(b.p, b.n);

	/* A codepoint that would straddle outcap is not written at all, and
	 * neither is anything after it, since need only grows: what lands in
	 * out is the longest whole-codepoint prefix that fits.  See the
	 * contract in utf8n.h. */
	for (k = 0; k < b.n; k++) {
		unsigned char tmp[4];
		size_t w = utf8n_encode(b.p[k], tmp);
		if (need + w <= outcap)
			memcpy(out + need, tmp, w);
		need += w;
	}
	free(b.p);
	return need;
}
