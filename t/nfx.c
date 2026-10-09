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

int main(int argc, char **argv)
{
	static char line[MAXCP * 8], in[MAXBYTES], out[MAXBYTES];
	int bytes_mode = argc > 1 && strcmp(argv[1], "-b") == 0;
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
