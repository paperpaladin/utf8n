/* t/nfx.c -- the filter t/diff.py drives.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Normal mode: each input line is a space-separated list of hex codepoints;
 * each output line is the four normalisations of it, hex, semicolon separated,
 * in the order NFD;NFC;NFKD;NFKC.
 *
 * -b: each input line is a space-separated list of hex *bytes*, which need not
 * be valid UTF-8; each output line is the codepoints our decoder produces.
 * That is the mode that gets compared against CPython's errors='replace'.
 *
 * -q: each input line is a space-separated list of hex codepoints; each output
 * line is utf8n_quick_check() on it, as four letters Y, N or M, semicolon
 * separated, in the same NFD;NFC;NFKD;NFKC order.  t/diff.py compares that
 * against the UCD's published NFD_QC/NFC_QC/NFKD_QC/NFKC_QC and against
 * CPython's unicodedata.is_normalized.
 *
 * -r: read raw bytes from stdin, almost certainly not valid UTF-8, and assert
 * the properties that must hold for any input at all: the two-call sizing
 * contract, that the result is well-formed UTF-8, that the result is
 * idempotent, and that a one-byte-short buffer truncates instead of
 * overflowing.  This is the mode `make test-asan` points at /dev/urandom.
 *
 * -t: read raw bytes from stdin and time all four forms over them, printing
 * comment lines.  A timing is not a pass or a fail, so it is never an "ok"
 * line; it is here so the fast path's claim is a measurement in the log of
 * every run rather than a number somebody typed into the README once.
 *
 * One process for the whole corpus, because fork-per-string would dominate.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../utf8n.h"
#include "../utf8n_table.h"

static const char *fname[] = {"NFD", "NFC", "NFKD", "NFKC"};

#define MAXCP 4096
#define MAXBYTES (MAXCP * 4 * UTF8N_MAX_EXPANSION + 4)

static size_t parse_hex(char *line, uint32_t *out, size_t cap)
{
	size_t n = 0;
	char *s = line, *e;

	while (n < cap) {
		unsigned long v = strtoul(s, &e, 16);
		if (e == s)
			break;
		out[n++] = (uint32_t)v;
		s = e;
	}
	return n;
}

static void print_cps(const char *b, size_t n)
{
	size_t i;

	for (i = 0; i < n;) {
		uint32_t cp;
		i += utf8n_decode((const unsigned char *)b + i, n - i, &cp);
		printf("%04X", cp);
		if (i < n)
			putchar(' ');
	}
}

/* Walk the buffer and re-encode: equal bytes out means well-formed UTF-8 in,
 * without a second decoder to disagree with. */
static int wellformed(const char *b, size_t n)
{
	size_t i = 0;

	while (i < n) {
		unsigned char tmp[4];
		uint32_t cp;
		size_t r = utf8n_decode((const unsigned char *)b + i, n - i, &cp);
		if (r != utf8n_encode(cp, tmp) || memcmp(tmp, b + i, r) != 0)
			return 0;
		i += r;
	}
	return 1;
}

/* The two promises utf8n_quick_check() makes, checked against what
 * normalising actually did: YES means the bytes do not change, NO means they
 * do.  Only MAYBE is allowed to be wrong in either direction.  Over 400kB of
 * urandom every answer is NO, so this is mostly the NO half; the YES half is
 * exercised by the empty input and, over real text, by t/conform. */
static int qc_agrees(const char *in, size_t n, const char *norm, size_t nn,
		     enum utf8n_form f)
{
	int same = nn == n && memcmp(in, norm, n) == 0;

	switch (utf8n_quick_check(f, in, n)) {
	case UTF8N_QC_YES:
		return same;
	case UTF8N_QC_NO:
		return !same;
	default:
		return 1;
	}
}

/* The same promise at a granularity where ill-formed bytes can still decide
 * the answer.
 *
 * Over the whole 400kB buffer every form answers NO, because somewhere in it
 * is certainly a codepoint with NFx_QC=No, so the check above holds even for
 * a quick check that never looks at whether the bytes were well-formed --
 * which is exactly how deleting that test survived the whole suite once.  An
 * 8-byte window is short enough that plenty of them contain nothing but
 * QC_Yes codepoints, and about one in 256 is pure ASCII and so a YES.
 *
 * The output buffer is sized by the first call and allocated to exactly that,
 * so a write past the end lands outside the allocation where the sanitizer
 * can see it, rather than inside a buffer rounded up to a power of two. */
static int qc_windows(const char *in, size_t n)
{
	long tally[3] = {0, 0, 0}, broken = 0;
	size_t i;

	/* Not an "ok" line and not a silent pass: a comment, because there is
	 * nothing here to check and saying so is the only honest option. */
	if (n < 8) {
		printf("# %zu bytes: too short for an 8-byte window, window "
		       "sweep not run\n", n);
		return 0;
	}
	for (i = 0; i + 8 <= n; i += 8) {
		int f;

		for (f = 0; f < 4; f++) {
			enum utf8n_form form = (enum utf8n_form)f;
			enum utf8n_qc q = utf8n_quick_check(form, in + i, 8);
			size_t need = utf8n_normalize(form, in + i, 8, NULL, 0);
			char *out;
			int same;

			if (need == (size_t)-1)
				return 2;
			out = malloc(need ? need : 1);
			if (!out)
				return 2;
			if (utf8n_normalize(form, in + i, 8, out, need)
			    != need) {
				free(out);
				printf("not ok - %s: sizing call disagrees "
				       "with the filling call on an 8-byte "
				       "window at %zu\n", fname[f], i);
				return 1;
			}
			same = need == 8 && memcmp(out, in + i, 8) == 0;
			free(out);
			tally[q]++;
			if ((q == UTF8N_QC_YES && !same) ||
			    (q == UTF8N_QC_NO && same)) {
				if (broken++ < 4)
					printf("# window at %zu: quick check "
					       "%c for %s but normalising %s "
					       "the bytes\n", i, "YNM"[q],
					       fname[f],
					       same ? "left" : "changed");
			}
		}
	}
	printf("%s - quick check kept its promises on %zu 8-byte windows of "
	       "random bytes: %ld Yes %ld No %ld Maybe, %ld broken\n",
	       broken || !tally[0] || !tally[1] ? "not ok" : "ok", n / 8,
	       tally[0], tally[1], tally[2], broken);
	if (!tally[0] || !tally[1])
		printf("# a quick-check answer that never occurred means the "
		       "promise above was not tested in both directions\n");
	return broken || !tally[0] || !tally[1] ? 1 : 0;
}

/* Slurp stdin.  Shared by -r and -t. */
static char *slurp(size_t *len)
{
	char *in = NULL;
	size_t n = 0, cap = 0;

	for (;;) {
		size_t got;
		if (n == cap) {
			cap = cap ? cap * 2 : 65536;
			in = realloc(in, cap);
			if (!in)
				return NULL;
		}
		got = fread(in + n, 1, cap - n, stdin);
		n += got;
		if (got == 0)
			break;
	}
	/* Trim to the exact length: a buffer rounded up to a power of two
	 * hides a read past the end of the data from the sanitizer, which is
	 * the whole point of running this mode under ASan. */
	in = realloc(in, n ? n : 1);
	*len = n;
	return in;
}

static int bench_mode(void)
{
	size_t n, i;
	char *in = slurp(&n), *out;
	size_t need;

	if (!in)
		return 2;
	if (n == 0) {
		free(in);
		printf("# nothing on stdin, nothing to time\n");
		return 0;
	}
	need = utf8n_normalize(UTF8N_NFC, in, n, NULL, 0);
	for (i = 0; i < 4; i++) {
		size_t m = utf8n_normalize((enum utf8n_form)i, in, n, NULL, 0);
		if (m > need)
			need = m;
	}
	out = malloc(need);
	if (!out) {
		free(in);
		return 2;
	}
	for (i = 0; i < 4; i++) {
		clock_t t0 = clock();
		long reps = 0;
		double secs;

		do {
			utf8n_normalize((enum utf8n_form)i, in, n, out, need);
			reps++;
			secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
		} while (secs < 0.2);
		printf("# %-4s %7.1f MB/s over %zu bytes (%ld reps, %.3fs)\n",
		       fname[i], (double)n * (double)reps / secs / 1e6, n,
		       reps, secs);
	}
	free(out);
	free(in);
	return 0;
}

static int raw_mode(void)
{
	char *a = NULL, *b = NULL;
	size_t n = 0;
	char *in = slurp(&n);
	int f, bad = 0;

	if (!in)
		return 2;

	for (f = 0; f < 4; f++) {
		size_t need = utf8n_normalize((enum utf8n_form)f, in, n, NULL, 0);
		size_t got, again;

		if (need == (size_t)-1)
			return 2;
		a = realloc(a, need + 1);
		b = realloc(b, need + 1);
		if (!a || !b)
			return 2;
		got = utf8n_normalize((enum utf8n_form)f, in, n, a, need);
		if (got != need) {
			printf("not ok - %s: sizing call disagrees with the "
			       "filling call (%zu vs %zu)\n", fname[f], need, got);
			bad = 1;
			continue;
		}
		if (!wellformed(a, need)) {
			printf("not ok - %s of %zu random bytes is well-formed "
			       "UTF-8\n", fname[f], n);
			bad = 1;
			continue;
		}
		again = utf8n_normalize((enum utf8n_form)f, a, need, b, need);
		if (again != need || memcmp(a, b, need) != 0) {
			printf("not ok - %s of %zu random bytes is idempotent\n",
			       fname[f], n);
			bad = 1;
			continue;
		}
		/* Truncation, which is where ASAN earns its keep.  The contract
		 * is a whole-codepoint prefix, so work out how long that prefix
		 * is rather than assuming outcap bytes get written: the rest of
		 * the buffer must be left exactly as it was. */
		if (need > 1) {
			size_t cap = need - 1, pre = 0, i;
			memset(b, 0x7F, need);
			for (i = 0; i < cap;) {
				uint32_t cp;
				size_t r = utf8n_decode((const unsigned char *)a
							+ i, need - i, &cp);
				if (i + r > cap)
					break;
				i += r;
				pre = i;
			}
			if (utf8n_normalize((enum utf8n_form)f, in, n, b, cap)
				!= need ||
			    memcmp(a, b, pre) != 0) {
				printf("not ok - %s truncates to a whole-"
				       "codepoint prefix (%zu of %zu bytes)\n",
				       fname[f], pre, need);
				bad = 1;
				continue;
			}
			for (i = pre; i < need; i++)
				if ((unsigned char)b[i] != 0x7F) {
					printf("not ok - %s wrote past its "
					       "whole-codepoint prefix at "
					       "byte %zu of %zu\n",
					       fname[f], i, cap);
					bad = 1;
					break;
				}
			if (bad)
				continue;
		}
		if (!qc_agrees(in, n, a, need, (enum utf8n_form)f)) {
			printf("not ok - %s: quick check %c on %zu random "
			       "bytes, but normalising %s them\n", fname[f],
			       "YNM"[utf8n_quick_check((enum utf8n_form)f,
						       in, n)],
			       n, need == n && memcmp(in, a, n) == 0
				       ? "did not change" : "changed");
			bad = 1;
			continue;
		}
		if (utf8n_quick_check((enum utf8n_form)f, a, need)
		    == UTF8N_QC_NO) {
			printf("not ok - %s: quick check calls its own output "
			       "not normalised\n", fname[f]);
			bad = 1;
			continue;
		}
		printf("ok - %s of %zu bytes: sized, well-formed, idempotent, "
		       "quick check %c and it kept its promise, truncates "
		       "cleanly (%zu bytes out)\n", fname[f], n,
		       "YNM"[utf8n_quick_check((enum utf8n_form)f, in, n)],
		       need);
	}
	if (qc_windows(in, n))
		bad = 1;
	free(in);
	free(a);
	free(b);
	return bad;
}

int main(int argc, char **argv)
{
	static char line[MAXCP * 8], in[MAXBYTES], out[MAXBYTES];
	int bytes_mode = argc > 1 && strcmp(argv[1], "-b") == 0;
	int qc_mode = argc > 1 && strcmp(argv[1], "-q") == 0;

	if (argc > 1 && strcmp(argv[1], "-r") == 0)
		return raw_mode();
	if (argc > 1 && strcmp(argv[1], "-t") == 0)
		return bench_mode();
	uint32_t v[MAXCP];
	static const enum utf8n_form order[4] = {
		UTF8N_NFD, UTF8N_NFC, UTF8N_NFKD, UTF8N_NFKC
	};

	while (fgets(line, sizeof line, stdin)) {
		size_t n = parse_hex(line, v, MAXCP), w = 0, i;

		if (bytes_mode) {
			for (i = 0; i < n; i++)
				in[w++] = (char)(v[i] & 0xFF);
			print_cps(in, w);
			putchar('\n');
			continue;
		}
		for (i = 0; i < n; i++)
			w += utf8n_encode(v[i], (unsigned char *)in + w);
		if (qc_mode) {
			for (i = 0; i < 4; i++)
				printf("%s%c", i ? ";" : "",
				       "YNM"[utf8n_quick_check(order[i], in, w)]);
			putchar('\n');
			continue;
		}
		for (i = 0; i < 4; i++) {
			size_t m = utf8n_normalize(order[i], in, w, out,
						   sizeof out);
			if (m == (size_t)-1 || m > sizeof out) {
				fprintf(stderr, "nfx: normalize failed\n");
				return 2;
			}
			if (i)
				putchar(';');
			print_cps(out, m);
		}
		putchar('\n');
	}
	return 0;
}
