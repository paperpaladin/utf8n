/* utf8n -- Unicode normalisation (NFD, NFC, NFKD, NFKC) over UTF-8.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef UTF8N_H
#define UTF8N_H

#include <stddef.h>
#include <stdint.h>

enum utf8n_form { UTF8N_NFD, UTF8N_NFC, UTF8N_NFKD, UTF8N_NFKC };

/* Normalise in[0..inlen) to the given form, writing UTF-8 to out.  Returns the
 * number of bytes the result needs, as snprintf does -- call with outcap 0 to
 * size the buffer, then again to fill it.
 *
 * On truncation it writes the longest *whole-codepoint* prefix of the result
 * that fits in outcap, and leaves the remaining bytes of out untouched.  It
 * does not write a partial codepoint, because a caller handed back ill-formed
 * UTF-8 is worse off than one handed back a short string; the bytes between
 * that prefix and outcap are therefore not written at all, which is the one
 * place this deviates from snprintf, and it is deliberate.
 *
 * in and out may be the same buffer, or overlap in any way: the whole input is
 * consumed into an internal codepoint buffer before the first output byte is
 * written.  Normalising in place is therefore safe, which is worth knowing
 * because the result is never longer than 4 * UTF8N_MAX_EXPANSION per input
 * codepoint but is usually about the same size.
 *
 * Returns (size_t)-1, and writes nothing, if the internal codepoint buffer
 * cannot be allocated.  Ill-formed input is replaced per utf8n_decode(), so
 * the result is always well-formed UTF-8 whether truncated or not. */
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

/* Longest decomposition of one codepoint, in codepoints (UTF8N_MAX_EXPANSION
 * in the generated table header).  A caller with a fixed output buffer needs
 * 4 * UTF8N_MAX_EXPANSION bytes per input codepoint in the worst case. */
int utf8n_max_expansion(void);

/* Canonical_Combining_Class of cp, 0 for a starter. */
int utf8n_ccc(uint32_t cp);

/* Version of the Unicode Character Database the tables were generated from. */
const char *utf8n_ucd_version(void);

#endif /* UTF8N_H */
