/* t/unit.c -- assertions that do not need the UCD files on disk.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Prints TAP-ish "ok - "/"not ok - " lines; t/run.sh counts them.
 */
#include <stdio.h>
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
	return failures != 0;
}
