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

/* Result of utf8n_quick_check().  The numbers are also the encoding used by
 * the generated quick-check table, so they are not free to be reordered;
 * utf8n.c has the _Static_assert that says so. */
enum utf8n_qc { UTF8N_QC_YES = 0, UTF8N_QC_NO = 1, UTF8N_QC_MAYBE = 2 };

/* Is in[0..inlen) already in the given form?  This is UAX #15's quick check:
 * one table lookup per codepoint, no allocation, and it stops at the first
 * codepoint that settles the question.
 *
 * UTF8N_QC_YES is a guarantee: utf8n_normalize() on this input would return
 * these exact bytes.  UTF8N_QC_NO is equally definite the other way.
 * UTF8N_QC_MAYBE means the cheap test cannot tell -- the input ends with, or
 * contains, a codepoint that something in front of it might compose with --
 * and the only way to find out is to normalise and compare.
 *
 * Ill-formed input is UTF8N_QC_NO, including a byte sequence that decodes to
 * a codepoint that is itself normalisation-stable: utf8n_normalize() replaces
 * the ill-formed bytes with U+FFFD, so what comes back is not what went in,
 * and YES has to keep meaning "unchanged".  Well-formed U+FFFD is of course
 * fine. */
enum utf8n_qc utf8n_quick_check(enum utf8n_form form, const char *in,
				size_t inlen);

/* Version of the Unicode Character Database the tables were generated from. */
const char *utf8n_ucd_version(void);

#endif /* UTF8N_H */
