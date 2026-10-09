/* utf8n -- Unicode normalisation (NFD, NFC, NFKD, NFKC) over UTF-8.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One file, C11, no dependencies.  The tables in utf8n_table.h are generated
 * from the Unicode Character Database by tools/gen_tables.py.
 */
#include "utf8n.h"

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
