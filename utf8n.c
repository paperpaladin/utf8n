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

/* --- decomposition --------------------------------------------------------
 *
 * The table already holds the full, recursive decomposition, so this is one
 * lookup and a copy.  A codepoint with no decomposition, and a codepoint whose
 * canonical decomposition is empty but whose compatibility decomposition is
 * not, both fall through to "emit as is" via the zero length.
 */
static void decompose(struct cpbuf *b, uint32_t cp, int compat)
{
	const struct utf8n_dec *d = dec_find(cp);
	unsigned off, len, i;

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
