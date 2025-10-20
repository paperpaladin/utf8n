/* utf8n -- Unicode normalisation (NFD, NFC, NFKD, NFKC) over UTF-8.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef UTF8N_H
#define UTF8N_H

#include <stddef.h>
#include <stdint.h>

enum utf8n_form { UTF8N_NFD, UTF8N_NFC, UTF8N_NFKD, UTF8N_NFKC };

/* Normalise in[0..inlen) to the given form, writing UTF-8 to out.  Returns the
 * number of bytes the result needs; at most outcap of them are written, so the
 * snprintf convention applies -- call with outcap 0 to size the buffer, then
 * again to fill it.  Returns (size_t)-1, and writes nothing, if the internal
 * codepoint buffer cannot be allocated.  Ill-formed input is replaced per
 * utf8n_decode(), so the result is always well-formed UTF-8. */
size_t utf8n_normalize(enum utf8n_form form, const char *in, size_t inlen,
		       char *out, size_t outcap);

/* Decode one codepoint from s[0..n).  Returns the number of bytes consumed,
 * which is never 0 for n > 0.  Ill-formed input yields U+FFFD and consumes the
 * maximal ill-formed subpart (Unicode 16.0 table 3-7 / TR#36), so a decoder
 * never gets stuck and never swallows a valid lead byte. */
size_t utf8n_decode(const unsigned char *s, size_t n, uint32_t *cp);

/* Encode cp as UTF-8 into out[0..4).  Returns bytes written.  Surrogates and
 * values above U+10FFFF are encoded as U+FFFD rather than as invalid UTF-8. */
size_t utf8n_encode(uint32_t cp, unsigned char *out);

/* Canonical_Combining_Class of cp, 0 for a starter. */
int utf8n_ccc(uint32_t cp);

/* Version of the Unicode Character Database the tables were generated from. */
const char *utf8n_ucd_version(void);

#endif /* UTF8N_H */
