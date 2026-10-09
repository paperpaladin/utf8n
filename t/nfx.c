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
 * -r: read raw bytes from stdin, almost certainly not valid UTF-8, and assert
 * the properties that must hold for any input at all: the two-call sizing
 * contract, that the result is well-formed UTF-8, that the result is
 * idempotent, and that a one-byte-short buffer truncates instead of
 * overflowing.  This is the mode `make test-asan` points at /dev/urandom.
 *
 * One process for the whole corpus, because fork-per-string would dominate.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../utf8n.h"

#define MAXCP 4096
#define MAXBYTES (MAXCP * 4 * 18 + 4)

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

static int raw_mode(void)
{
	static const char *fname[] = {"NFD", "NFC", "NFKD", "NFKC"};
	char *in = NULL, *a = NULL, *b = NULL;
	size_t n = 0, cap = 0;
	int f, bad = 0;

	for (;;) {			/* slurp stdin */
		size_t got;
		if (n == cap) {
			cap = cap ? cap * 2 : 65536;
			in = realloc(in, cap);
			if (!in)
				return 2;
		}
		got = fread(in + n, 1, cap - n, stdin);
		n += got;
		if (got == 0)
			break;
	}

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
		printf("ok - %s of %zu bytes: sized, well-formed, idempotent, "
		       "truncates cleanly (%zu bytes out)\n", fname[f], n, need);
	}
	free(in);
	free(a);
	free(b);
	return bad;
}

int main(int argc, char **argv)
{
	static char line[MAXCP * 8], in[MAXBYTES], out[MAXBYTES];
	int bytes_mode = argc > 1 && strcmp(argv[1], "-b") == 0;

	if (argc > 1 && strcmp(argv[1], "-r") == 0)
		return raw_mode();
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
