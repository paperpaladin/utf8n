/* utf8n -- Unicode normalisation (NFD, NFC, NFKD, NFKC) over UTF-8.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef UTF8N_H
#define UTF8N_H

#include <stddef.h>
#include <stdint.h>

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
