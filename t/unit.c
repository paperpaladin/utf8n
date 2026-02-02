/* t/unit.c -- assertions that do not need the UCD files on disk.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Prints TAP-ish "ok - "/"not ok - " lines; t/run.sh counts them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "../utf8n.h"

static int failures;

static void ok(int cond, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void ok(int cond, const char *fmt, ...)
{
	va_list ap;
	fputs(cond ? "ok - " : "not ok - ", stdout);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	if (!cond)
		failures++;
}

/* Decode a whole buffer into a codepoint list, for comparing against the
 * replacement policy case by case. */
static size_t decode_all(const char *bytes, size_t n, uint32_t *out, size_t cap)
{
	size_t i = 0, w = 0;
	while (i < n && w < cap) {
		uint32_t cp;
		i += utf8n_decode((const unsigned char *)bytes + i, n - i, &cp);
		out[w++] = cp;
	}
	return w;
}

static void t_utf8(void)
{
	static const struct {
		const char *name, *in;
		size_t inlen;
		size_t n;
		uint32_t cp[8];
	} cases[] = {
		/* Well-formed. */
		{"ASCII", "A", 1, 1, {0x41}},
		{"2-byte", "\xC3\xA9", 2, 1, {0xE9}},
		{"3-byte", "\xE3\x81\x82", 3, 1, {0x3042}},
		{"4-byte", "\xF0\x9F\x92\xA9", 4, 1, {0x1F4A9}},
		{"U+10FFFF", "\xF4\x8F\xBF\xBF", 4, 1, {0x10FFFF}},
		/* Unicode 16.0 table 3-7 ill-formed examples. */
		{"lone continuation", "\x80", 1, 1, {0xFFFD}},
		{"overlong 2-byte NUL", "\xC0\x80", 2, 2, {0xFFFD, 0xFFFD}},
		{"overlong C1", "\xC1\xBF", 2, 2, {0xFFFD, 0xFFFD}},
		{"overlong 3-byte", "\xE0\x80\xAF", 3, 3, {0xFFFD, 0xFFFD, 0xFFFD}},
		{"surrogate D800", "\xED\xA0\x80", 3, 3, {0xFFFD, 0xFFFD, 0xFFFD}},
		{"beyond 10FFFF", "\xF4\x90\x80\x80", 4, 4,
		 {0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD}},
		{"F5 lead", "\xF5\x80\x80\x80", 4, 4,
		 {0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD}},
		{"FF", "\xFF", 1, 1, {0xFFFD}},
		/* Maximal subpart: one U+FFFD for the truncated prefix, and the
		 * following lead byte is not consumed with it. */
		{"truncated 3-byte then A", "\xE3\x81" "A", 3, 2, {0xFFFD, 0x41}},
		{"truncated 4-byte then A", "\xF0\x9F\x92" "A", 4, 2, {0xFFFD, 0x41}},
		{"truncated at end", "\xE3\x81", 2, 1, {0xFFFD}},
		{"C2 then lead", "\xC2\xC3\xA9", 3, 2, {0xFFFD, 0xE9}},
		{"E1 80 then E9", "\xE1\x80\xC3\xA9", 4, 2, {0xFFFD, 0xE9}},
	};
	size_t i, k;

	for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		uint32_t got[8];
		size_t n = decode_all(cases[i].in, cases[i].inlen, got, 8);
		int same = n == cases[i].n;
		for (k = 0; same && k < n; k++)
			same = got[k] == cases[i].cp[k];
		ok(same, "decode %s", cases[i].name);
	}

	/* Round trip: every scalar value encodes and decodes back to itself. */
	{
		uint32_t cp, back;
		int bad = 0;
		for (cp = 0; cp <= 0x10FFFF; cp++) {
			unsigned char b[4];
			size_t n;
			if (cp >= 0xD800 && cp <= 0xDFFF)
				continue;
			n = utf8n_encode(cp, b);
			if (utf8n_decode(b, n, &back) != n || back != cp)
				bad++;
		}
		ok(bad == 0, "encode/decode round trip over all 1112064 scalars "
		   "(%d bad)", bad);
	}

	/* utf8n_decode() must never claim to have consumed more than it was
	 * given.  A truncated sequence at the end of the buffer is the case
	 * that gets this wrong -- returning the sequence's nominal length
	 * instead of what was actually there -- and no normalisation test can
	 * see it, because our own loop stops at inlen either way.  A caller
	 * that advances by the return value walks off the end.
	 * Exhaustive over every 1, 2 and 3 byte sequence. */
	{
		unsigned a, b2, c3;
		long bad = 0;
		uint32_t cp;
		unsigned char s[3];
		for (a = 0; a < 256; a++) {
			s[0] = (unsigned char)a;
			if (utf8n_decode(s, 1, &cp) != 1)
				bad++;
			for (b2 = 0; b2 < 256; b2++) {
				size_t r;
				s[1] = (unsigned char)b2;
				r = utf8n_decode(s, 2, &cp);
				if (r < 1 || r > 2)
					bad++;
				for (c3 = 0; c3 < 256; c3++) {
					s[2] = (unsigned char)c3;
					r = utf8n_decode(s, 3, &cp);
					if (r < 1 || r > 3)
						bad++;
				}
			}
		}
		ok(bad == 0, "decode consumes 1..n bytes for all 16.8M "
		   "sequences of length 1, 2 and 3 (%ld bad)", bad);
	}

	/* Surrogates and out-of-range encode as U+FFFD, never as invalid UTF-8. */
	{
		unsigned char b[4];
		uint32_t cp;
		ok(utf8n_encode(0xD800, b) == 3 &&
		   utf8n_decode(b, 3, &cp) == 3 && cp == 0xFFFD,
		   "encode(U+D800) is U+FFFD");
		ok(utf8n_encode(0x110000, b) == 3 &&
		   utf8n_decode(b, 3, &cp) == 3 && cp == 0xFFFD,
		   "encode(0x110000) is U+FFFD");
	}
}

static void t_ccc(void)
{
	static const struct { uint32_t cp; int ccc; } cases[] = {
		{0x0041, 0},	/* LATIN CAPITAL LETTER A, a starter */
		{0x0334, 1},	/* COMBINING TILDE OVERLAY */
		{0x093C, 7},	/* DEVANAGARI SIGN NUKTA */
		{0x3099, 8},	/* COMBINING KATAKANA-HIRAGANA VOICED SOUND MARK */
		{0x05B0, 10},	/* HEBREW POINT SHEVA */
		{0x064B, 27},	/* ARABIC FATHATAN */
		{0x0E48, 107},	/* THAI CHARACTER MAI EK */
		{0x0316, 220},	/* COMBINING GRAVE ACCENT BELOW */
		{0x0591, 220},	/* HEBREW ACCENT ETNAHTA */
		{0x302E, 224},	/* HANGUL SINGLE DOT TONE MARK */
		{0x05AE, 228},	/* HEBREW ACCENT ZINOR */
		{0x0301, 230},	/* COMBINING ACUTE ACCENT */
		{0x1D167, 1},	/* MUSICAL SYMBOL COMBINING TREMOLO-1, astral */
		{0x10FFFF, 0},
	};
	size_t i;
	int bad = 0;

	for (i = 0; i < sizeof cases / sizeof cases[0]; i++)
		if (utf8n_ccc(cases[i].cp) != cases[i].ccc) {
			printf("# ccc(U+%04X) = %d, want %d\n", cases[i].cp,
			       utf8n_ccc(cases[i].cp), cases[i].ccc);
			bad++;
		}
	ok(bad == 0, "Canonical_Combining_Class spot checks (%zu values)",
	   sizeof cases / sizeof cases[0]);
	/* The worst case a caller sizing a buffer has to allow for.  U+FDFA
	 * ARABIC LIGATURE SALLALLAHOU ALAYHE WASALLAM is the longest
	 * decomposition in the UCD, and a fixed-buffer caller that assumed 4
	 * or 8 would overflow on it. */
	ok(utf8n_max_expansion() == 18, "longest decomposition is 18 codepoints"
	   " (got %d)", utf8n_max_expansion());
	ok(utf8n_ucd_version()[0] != 0, "tables declare a UCD version (%s)",
	   utf8n_ucd_version());
}

/* Normalise a NUL-terminated UTF-8 string into a static buffer.  Only the
 * tests get to be this careless about size. */
static const char *nf(enum utf8n_form form, const char *s)
{
	static char buf[4][512];
	static int turn;
	char *out = buf[turn = (turn + 1) & 3];
	size_t n = utf8n_normalize(form, s, strlen(s), out, sizeof buf[0] - 1);

	if (n == (size_t)-1 || n >= sizeof buf[0])
		return "<overflow>";
	out[n] = 0;
	return out;
}

static void eq(enum utf8n_form form, const char *in, const char *want,
	       const char *name)
{
	const char *got = nf(form, in);
	static const char *fname[] = {"NFD", "NFC", "NFKD", "NFKC"};

	if (strcmp(got, want) != 0)
		printf("# %s(%s): got %s want %s\n", fname[form], in, got, want);
	ok(strcmp(got, want) == 0, "%s %s", fname[form], name);
}

#define NELEM(a) (sizeof(a) / sizeof(a)[0])

/* Codepoint sequences are written as hex strings: combining marks are
 * unreadable as UTF-8 escapes, and C macros cannot take brace lists. */
static size_t parse_cps(const char *s, uint32_t *out, size_t cap)
{
	size_t n = 0;
	char *end;

	while (n < cap) {
		unsigned long v = strtoul(s, &end, 16);
		if (end == s)
			break;
		out[n++] = (uint32_t)v;
		s = end;
	}
	return n;
}

static void eqcp(enum utf8n_form form, const char *name, const char *inhex,
		 const char *wanthex)
{
	static const char *fname[] = {"NFD", "NFC", "NFKD", "NFKC"};
	uint32_t in[64], want[64], got[128];
	char ibuf[256], obuf[1024];
	size_t ni, nw, i, w = 0, n, r = 0, k;
	int same;

	ni = parse_cps(inhex, in, NELEM(in));
	nw = parse_cps(wanthex, want, NELEM(want));
	for (i = 0; i < ni; i++)
		w += utf8n_encode(in[i], (unsigned char *)ibuf + w);
	n = utf8n_normalize(form, ibuf, w, obuf, sizeof obuf);
	same = n != (size_t)-1 && n <= sizeof obuf;
	if (same) {
		r = decode_all(obuf, n, got, NELEM(got));
		same = r == nw;
		for (k = 0; same && k < nw; k++)
			same = got[k] == want[k];
	}
	if (!same) {
		printf("# %s(%s) =", fname[form], inhex);
		for (k = 0; k < r; k++)
			printf(" %04X", got[k]);
		printf(", want %s\n", wanthex);
	}
	ok(same, "%s %s", fname[form], name);
}

static void t_order(void)
{
	/* Three marks, all out of order, must come out 1 < 220 < 230. */
	eqcp(UTF8N_NFD, "three marks reorder (230,220,1 -> 1,220,230)",
	     "0061 0301 0316 0334", "0061 0334 0316 0301");
	/* Four marks spanning four classes, reordered. */
	eqcp(UTF8N_NFD, "four marks reorder (228,220,230,240)",
	     "0061 05AE 0316 0301 0345", "0061 0316 05AE 0301 0345");
	/* Equal classes must NOT be reordered: these two strings stay distinct. */
	eqcp(UTF8N_NFD, "equal class keeps order (U+0301 U+0300)",
	     "0061 0301 0300", "0061 0301 0300");
	eqcp(UTF8N_NFD, "equal class keeps order (U+0300 U+0301)",
	     "0061 0300 0301", "0061 0300 0301");
	/* Ordering happens after decomposition, not before: U+1E0B is d with dot
	 * above (230) and the trailing dot below (220) has to sort in front of
	 * the mark that came out of the decomposition. */
	eqcp(UTF8N_NFD, "mark from a decomposition is reordered",
	     "1E0B 0323", "0064 0323 0307");
	/* A starter resets the run: the second group sorts independently. */
	eqcp(UTF8N_NFD, "a starter breaks the reordering run",
	     "0061 0301 0334 0062 0301 0334", "0061 0334 0301 0062 0334 0301");
}

static void t_hangul(void)
{
	/* LVT, LV, and the two ends of the block. */
	eqcp(UTF8N_NFD, "U+AC01 -> L V T", "AC01", "1100 1161 11A8");
	eqcp(UTF8N_NFD, "U+AC00 -> L V (no trailer)", "AC00", "1100 1161");
	eqcp(UTF8N_NFD, "U+D7A3 (last syllable) -> L V T", "D7A3",
	     "1112 1175 11C2");
	/* One below and one above the block must be left alone.  U+D7A4 is
	 * unassigned, which is exactly the off-by-one a <= test would eat. */
	eqcp(UTF8N_NFD, "U+ABFF is not a syllable", "ABFF", "ABFF");
	eqcp(UTF8N_NFD, "U+D7A4 is not a syllable", "D7A4", "D7A4");
	/* The composition side of the same boundary.  U+D7A4 is one past the
	 * block and (U+D7A4 - SBASE) % TCOUNT happens to be 0, so a range test
	 * written as <= SCOUNT composes it with a following trailing jamo into
	 * a codepoint outside the Hangul block entirely.  NormalizationTest.txt
	 * cannot catch that -- U+D7A4 is unassigned, so it lists no such
	 * sequence -- which is why this case is here by hand. */
	eqcp(UTF8N_NFC, "U+D7A4 does not absorb a trailing jamo", "D7A4 11A8",
	     "D7A4 11A8");
	eqcp(UTF8N_NFC, "U+ABFF does not absorb a trailing jamo", "ABFF 11A8",
	     "ABFF 11A8");
	eqcp(UTF8N_NFC, "U+D7A3 (last syllable) has a trailer already",
	     "D7A3 11A8", "D7A3 11A8");

	/* Jamo are starters, so a decomposed syllable survives reordering. */
	eqcp(UTF8N_NFKD, "NFKD decomposes syllables too", "AC01",
	     "1100 1161 11A8");
	/* A syllable with marks after it: the jamo must not be reordered into
	 * the marks, and the marks must still sort among themselves. */
	eqcp(UTF8N_NFD, "syllable followed by marks", "AC01 0301 0334",
	     "1100 1161 11A8 0334 0301");
}

static void t_compose(void)
{
	eqcp(UTF8N_NFC, "e + U+0301 -> U+00E9", "0065 0301", "00E9");
	eqcp(UTF8N_NFC, "already composed stays", "00E9", "00E9");
	eqcp(UTF8N_NFC, "recomposes through two levels", "0063 0327 0301",
	     "1E09");

	/* The blocked-starter rule.  Both strings decompose to the same three
	 * codepoints in the same order (equal classes are not reordered), and
	 * NFC must compose a different one of them each time. */
	eqcp(UTF8N_NFC, "U+0301 blocked by equal-class U+0300", "0061 0300 0301",
	     "00E0 0301");
	eqcp(UTF8N_NFC, "U+0300 blocked by equal-class U+0301", "0061 0301 0300",
	     "00E1 0300");
	/* Not blocked: the intervening mark has a lower class, so U+0301
	 * reaches back past it to the starter. */
	eqcp(UTF8N_NFC, "U+0301 composes past a lower-class mark",
	     "0061 0334 0301", "00E1 0334");
	eqcp(UTF8N_NFC, "composes past two lower-class marks",
	     "0061 0334 0316 0301", "00E1 0334 0316");
	/* Blocked by a starter. */
	eqcp(UTF8N_NFC, "a starter blocks composition", "0061 0062 0301",
	     "0061 0062 0301");
	/* A string that begins with a combining mark has no starter to compose
	 * into, and must not invent one. */
	eqcp(UTF8N_NFC, "leading mark has no starter", "0301 0061",
	     "0301 0061");
	eqcp(UTF8N_NFC, "two leading marks", "0308 0301", "0308 0301");

	/* Composition exclusions: U+0958 is in CompositionExclusions.txt, so
	 * its own decomposition must not recompose. */
	eqcp(UTF8N_NFC, "U+0915 U+093C does not become U+0958", "0915 093C",
	     "0915 093C");
	eqcp(UTF8N_NFC, "U+0958 decomposes and stays decomposed", "0958",
	     "0915 093C");
	eqcp(UTF8N_NFC, "U+FB1D does not recompose", "FB1D", "05D9 05B4");
	/* Singletons must not come back. */
	eqcp(UTF8N_NFC, "U+2126 OHM SIGN stays U+03A9", "2126", "03A9");
	eqcp(UTF8N_NFC, "U+212B ANGSTROM SIGN becomes U+00C5", "212B", "00C5");
	/* Non-starter decomposition: U+0344 decomposes to two marks, and a
	 * composition table that did not exclude it would turn any U+0308
	 * U+0301 back into it. */
	eqcp(UTF8N_NFC, "U+0344 does not come back", "0308 0301", "0308 0301");
	/* ...but a composite whose first character *is* a starter does come
	 * back, so the exclusion is not a blanket ban on odd-looking pairs. */
	eqcp(UTF8N_NFC, "U+03D2 U+0301 does become U+03D3", "03D2 0301",
	     "03D3");

	/* Hangul composition, both halves of the arithmetic. */
	eqcp(UTF8N_NFC, "L V T -> syllable", "1100 1161 11A8", "AC01");
	eqcp(UTF8N_NFC, "L V -> syllable", "1100 1161", "AC00");
	eqcp(UTF8N_NFC, "LV + T -> LVT", "AC00 11A8", "AC01");
	eqcp(UTF8N_NFC, "LVT + T does not compose", "AC01 11A8", "AC01 11A8");
	eqcp(UTF8N_NFC, "L + T does not compose", "1100 11A8", "1100 11A8");
	eqcp(UTF8N_NFC, "V + T alone does not compose", "1161 11A8",
	     "1161 11A8");
	eqcp(UTF8N_NFC, "U+11A7 is the filler, not a trailer", "AC00 11A7",
	     "AC00 11A7");
	eqcp(UTF8N_NFC, "round trip of the last syllable", "D7A3", "D7A3");

	/* NFKC is NFKD followed by the same composition. */
	eqcp(UTF8N_NFKC, "U+03D3 -> U+038E under NFKC", "03D3", "038E");
	eqcp(UTF8N_NFKC, "U+00A0 -> SPACE under NFKC", "00A0", "0020");
	eqcp(UTF8N_NFKC, "ligature U+FB01 -> f i", "FB01", "0066 0069");
}

static void t_decompose(void)
{
	/* Canonical decomposition, cases that need no reordering. */
	eq(UTF8N_NFD, "A", "A", "ASCII is unchanged");
	eq(UTF8N_NFD, "\xC3\xA9", "e\xCC\x81", "U+00E9 -> e + U+0301");
	/* U+1E09 c-cedilla-acute decomposes recursively through U+00E7. */
	eq(UTF8N_NFD, "\xE1\xB8\x89", "c\xCC\xA7\xCC\x81",
	   "U+1E09 decomposes recursively (two levels)");
	/* A singleton: U+212B ANGSTROM SIGN -> U+00C5 -> A + U+030A. */
	eq(UTF8N_NFD, "\xE2\x84\xAB", "A\xCC\x8A",
	   "U+212B singleton decomposes through U+00C5");
	/* Compatibility only: the no-break space has a <noBreak> decomposition,
	 * so NFD must leave it alone and NFKD must not. */
	eq(UTF8N_NFD, "\xC2\xA0", "\xC2\xA0",
	   "U+00A0 is unchanged (compatibility decomposition only)");
	eq(UTF8N_NFKD, "\xC2\xA0", " ", "U+00A0 -> SPACE");
	eqcp(UTF8N_NFKD, "U+FDFA expands to 18 codepoints", "FDFA",
	     "0635 0644 0649 0020 0627 0644 0644 0647 0020 0639 0644 064A 0647"
	     " 0020 0648 0633 0644 0645");
	eqcp(UTF8N_NFD, "U+FDFA is unchanged by NFD", "FDFA", "FDFA");
	/* Ill-formed input is replaced, then normalised. */
	eq(UTF8N_NFD, "\xC3\xA9\xFF\xC3\xA9", "e\xCC\x81\xEF\xBF\xBD" "e\xCC\x81",
	   "ill-formed byte becomes U+FFFD between two decompositions");

	/* The snprintf convention: outcap 0 sizes the result. */
	ok(utf8n_normalize(UTF8N_NFD, "\xC3\xA9", 2, NULL, 0) == 3,
	   "outcap 0 returns the needed length");
	{
		char small[2] = {1, 1};
		ok(utf8n_normalize(UTF8N_NFD, "\xC3\xA9", 2, small, 2) == 3 &&
		   small[0] == 'e',
		   "a short buffer is filled as far as it goes");
	}
	ok(utf8n_normalize(UTF8N_NFD, "", 0, NULL, 0) == 0,
	   "empty input gives empty output");
}

int main(void)
{
	t_utf8();
	t_ccc();
	t_decompose();
	t_order();
	t_hangul();
	t_compose();
	return failures != 0;
}
