/* t/conform.c -- run NormalizationTest.txt from the UCD.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The file is the specification of what the four functions mean.  Each
 * non-comment line has five columns -- source; NFC; NFD; NFKC; NFKD -- and the
 * header states twenty equalities per line that a conformant implementation
 * must satisfy.  All twenty are checked, plus idempotence of all four forms on
 * all five columns, plus the file's second conformance requirement: every
 * codepoint NOT listed in Part 1 normalises to itself under all four forms.
 *
 * Usage: t/conform ucd/NormalizationTest.txt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../utf8n.h"

#define MAXCOL 64
#define MAXLINE 8192
#define MAXPART 8
#define MAXSHOW 12		/* failures printed in full before summarising */

static const char *fname[] = {"NFD", "NFC", "NFKD", "NFKC"};

static long pass[MAXPART], fail[MAXPART], lines[MAXPART], badlines[MAXPART];
static long shown;
static unsigned char part1[(0x110000 + 7) / 8];

struct str {			/* a column: UTF-8 bytes plus its codepoints */
	char b[MAXCOL * 4 + 1];
	size_t n;
	uint32_t cp[MAXCOL];
	size_t ncp;
};

static int parse_col(const char *s, const char *end, struct str *o)
{
	o->n = o->ncp = 0;
	while (s < end) {
		char *e;
		unsigned long v;
		while (s < end && *s == ' ')
			s++;
		if (s >= end)
			break;
		v = strtoul(s, &e, 16);
		if (e == s || o->ncp >= MAXCOL)
			return 0;
		o->cp[o->ncp++] = (uint32_t)v;
		o->n += utf8n_encode((uint32_t)v, (unsigned char *)o->b + o->n);
		s = e;
	}
	o->b[o->n] = 0;
	return o->ncp > 0;
}

static void show(int part, long lineno, const char *what, enum utf8n_form f,
		 const struct str *in, const struct str *want, const char *got,
		 size_t gotn)
{
	size_t i;

	if (shown++ >= MAXSHOW)
		return;
	printf("# FAIL Part%d line %ld: %s %s(", part, lineno, what, fname[f]);
	for (i = 0; i < in->ncp; i++)
		printf("%s%04X", i ? " " : "", in->cp[i]);
	printf(") = ");
	for (i = 0; i < gotn;) {
		uint32_t cp;
		i += utf8n_decode((const unsigned char *)got + i, gotn - i, &cp);
		printf("%04X ", cp);
	}
	printf(", want ");
	for (i = 0; i < want->ncp; i++)
		printf("%s%04X", i ? " " : "", want->cp[i]);
	putchar('\n');
}

/* Assert form(in) == want. */
static void check(int part, long lineno, const char *what, enum utf8n_form f,
		  const struct str *in, const struct str *want)
{
	char got[MAXCOL * 4 * 18 + 1];
	size_t n = utf8n_normalize(f, in->b, in->n, got, sizeof got - 1);

	if (n == (size_t)-1 || n >= sizeof got) {
		printf("# line %ld: normalize failed or overflowed\n", lineno);
		fail[part]++;
		return;
	}
	if (n == want->n && memcmp(got, want->b, n) == 0) {
		pass[part]++;
		return;
	}
	fail[part]++;
	show(part, lineno, what, f, in, want, got, n);
}

/* Assert form(form(x)) == form(x). */
static void check_idem(int part, long lineno, enum utf8n_form f,
		       const struct str *x)
{
	char once[MAXCOL * 4 * 18 + 1], twice[sizeof once];
	size_t a = utf8n_normalize(f, x->b, x->n, once, sizeof once - 1);
	size_t b;

	if (a == (size_t)-1 || a >= sizeof once - 1) {
		fail[part]++;
		return;
	}
	b = utf8n_normalize(f, once, a, twice, sizeof twice - 1);
	if (b == a && memcmp(once, twice, a) == 0) {
		pass[part]++;
		return;
	}
	fail[part]++;
	if (shown++ < MAXSHOW)
		printf("# FAIL Part%d line %ld: %s is not idempotent on "
		       "U+%04X...\n", part, lineno, fname[f], x->cp[0]);
}

int main(int argc, char **argv)
{
	char line[MAXLINE];
	FILE *fp;
	int part = 0, i, f;
	long lineno = 0, total_pass = 0, total_fail = 0, nlines = 0, nbad = 0;
	long inv2_pass = 0, inv2_fail = 0;

	if (argc < 2 || !(fp = fopen(argv[1], "r"))) {
		fprintf(stderr, "conform: cannot open %s\n",
			argc < 2 ? "(no file given)" : argv[1]);
		return 2;
	}

	while (fgets(line, sizeof line, fp)) {
		struct str c[5];
		const char *s = line, *semi;
		long before_fail;

		lineno++;
		if (line[0] == '@') {
			part = line[5] - '0';
			if (part < 0 || part >= MAXPART)
				part = 0;
			continue;
		}
		if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
			continue;

		for (i = 0; i < 5; i++) {
			semi = strchr(s, ';');
			if (!semi || !parse_col(s, semi, &c[i]))
				break;
			s = semi + 1;
		}
		if (i != 5) {
			printf("# line %ld: cannot parse\n", lineno);
			fail[part]++;
			continue;
		}
		lines[part]++;
		before_fail = fail[part];

		if (part == 1)
			part1[c[0].cp[0] >> 3] |= (unsigned char)(1 << (c[0].cp[0] & 7));

		/* NFC: c2 == NFC(c1) == NFC(c2) == NFC(c3); c4 == NFC(c4) == NFC(c5) */
		for (i = 0; i < 3; i++)
			check(part, lineno, "c2 ==", UTF8N_NFC, &c[i], &c[1]);
		for (i = 3; i < 5; i++)
			check(part, lineno, "c4 ==", UTF8N_NFC, &c[i], &c[3]);
		/* NFD: c3 == NFD(c1..c3); c5 == NFD(c4), NFD(c5) */
		for (i = 0; i < 3; i++)
			check(part, lineno, "c3 ==", UTF8N_NFD, &c[i], &c[2]);
		for (i = 3; i < 5; i++)
			check(part, lineno, "c5 ==", UTF8N_NFD, &c[i], &c[4]);
		/* NFKC: c4 == NFKC(c1..c5).  NFKD: c5 == NFKD(c1..c5). */
		for (i = 0; i < 5; i++)
			check(part, lineno, "c4 ==", UTF8N_NFKC, &c[i], &c[3]);
		for (i = 0; i < 5; i++)
			check(part, lineno, "c5 ==", UTF8N_NFKD, &c[i], &c[4]);

		for (i = 0; i < 5; i++)
			for (f = 0; f < 4; f++)
				check_idem(part, lineno, (enum utf8n_form)f, &c[i]);

		if (fail[part] != before_fail)
			badlines[part]++;
	}
	fclose(fp);

	for (i = 0; i < MAXPART; i++) {
		if (!lines[i] && !fail[i])
			continue;
		printf("%s - @Part%d: %ld lines, %ld/%ld assertions\n",
		       fail[i] ? "not ok" : "ok", i, lines[i], pass[i],
		       pass[i] + fail[i]);
		total_pass += pass[i];
		total_fail += fail[i];
		nlines += lines[i];
		nbad += badlines[i];
	}
	if (shown > MAXSHOW)
		printf("# ...and %ld more failures not shown\n", shown - MAXSHOW);

	/* Conformance requirement 2: every codepoint not listed in Part 1
	 * normalises to itself in all four forms.  Surrogates are excluded:
	 * they are not scalar values and cannot appear in UTF-8 at all. */
	for (i = 0; i <= 0x10FFFF; i++) {
		char in[4], got[64];
		size_t n;
		if ((i >= 0xD800 && i <= 0xDFFF) ||
		    (part1[i >> 3] & (1 << (i & 7))))
			continue;
		n = utf8n_encode((uint32_t)i, (unsigned char *)in);
		for (f = 0; f < 4; f++) {
			size_t m = utf8n_normalize((enum utf8n_form)f, in, n,
						   got, sizeof got);
			if (m == n && memcmp(got, in, n) == 0) {
				inv2_pass++;
			} else {
				inv2_fail++;
				if (shown++ < MAXSHOW)
					printf("# FAIL requirement 2: "
					       "%s(U+%04X) != U+%04X\n",
					       fname[f], (unsigned)i,
					       (unsigned)i);
			}
		}
	}
	printf("%s - requirement 2: %ld/%ld codepoints outside Part 1 are "
	       "unchanged by all four forms\n",
	       inv2_fail ? "not ok" : "ok", inv2_pass / 4,
	       (inv2_pass + inv2_fail) / 4);

	printf("\nconformance score: %ld/%ld assertions, %ld/%ld lines "
	       "(NormalizationTest.txt, UCD %s)\n",
	       total_pass, total_pass + total_fail, nlines - nbad, nlines,
	       utf8n_ucd_version());
	return total_fail || inv2_fail ? 1 : 0;
}
