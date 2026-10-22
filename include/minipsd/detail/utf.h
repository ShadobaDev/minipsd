/**
 * \file minipsd/detail/utf.h
 * \brief Internal: UTF-8 to UTF-16 (the writer) and UTF-16BE to UTF-8 (the reader, the typed
 *        views), for layer names and other text.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MPSD__UTF_DONE
#define MPSD__UTF_DONE

#include "bytes.h"

/* Decodes one UTF-8 code point; malformed input yields U+FFFD and moves on by one byte. */
static uint32_t mpsd__utf8(const unsigned char **p)
{
    const unsigned char *s = *p;
    uint32_t cp;
    int extra, k;
    if      (s[0] < 0x80)           { *p = s + 1; return s[0]; }
    else if ((s[0] & 0xE0) == 0xC0) { cp = s[0] & 0x1F; extra = 1; }
    else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0F; extra = 2; }
    else if ((s[0] & 0xF8) == 0xF0) { cp = s[0] & 0x07; extra = 3; }
    else                            { *p = s + 1; return 0xFFFD; }
    for (k = 1; k <= extra; ++k) {
        if ((s[k] & 0xC0) != 0x80) { *p = s + 1; return 0xFFFD; }
        cp = (cp << 6) | (s[k] & 0x3F);
    }
    *p = s + extra + 1;
    return cp > 0x10FFFF ? 0xFFFD : cp;
}

/* Calls back with each UTF-16 unit of a UTF-8 string; returns how many there were. */
static size_t mpsd__utf16(const char *utf8, void (*unit)(void *, unsigned), void *ctx)
{
    const unsigned char *p = (const unsigned char *)(utf8 ? utf8 : "");
    size_t n = 0;
    while (*p) {
        const uint32_t cp = mpsd__utf8(&p);
        if (cp >= 0x10000) {
            if (unit) { unit(ctx, 0xD800 + ((cp - 0x10000) >> 10)); unit(ctx, 0xDC00 + ((cp - 0x10000) & 0x3FF)); }
            n += 2;
        } else {
            if (unit) unit(ctx, cp);
            ++n;
        }
    }
    return n;
}

/* Decodes one code point of UTF-16BE from [*p, end), which holds at least two bytes; an unpaired
   surrogate yields U+FFFD. */
static uint32_t mpsd__utf16be(const uint8_t **p, const uint8_t *end)
{
    const uint32_t hi = mpsd__get16(*p);
    *p += 2;
    if (hi < 0xD800 || hi > 0xDFFF) return hi;
    if (hi < 0xDC00 && end - *p >= 2) {
        const uint32_t lo = mpsd__get16(*p);
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
            *p += 2;
            return 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00);
        }
    }
    return 0xFFFD;
}

/* Encodes one code point (at most 0x10FFFF, no surrogates) as UTF-8 into out, or only counts when
   out is NULL; returns the bytes it takes. */
static size_t mpsd__put_utf8(uint8_t *out, uint32_t cp)
{
    uint8_t b[4];
    size_t  n, i;
    if (cp < 0x80)         { b[0] = (uint8_t)cp; n = 1; }
    else if (cp < 0x800)   { b[0] = (uint8_t)(0xC0 | (cp >> 6));  b[1] = (uint8_t)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { b[0] = (uint8_t)(0xE0 | (cp >> 12)); b[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
                             b[2] = (uint8_t)(0x80 | (cp & 0x3F)); n = 3; }
    else                   { b[0] = (uint8_t)(0xF0 | (cp >> 18)); b[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
                             b[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (uint8_t)(0x80 | (cp & 0x3F)); n = 4; }
    if (out) for (i = 0; i < n; ++i) out[i] = b[i];
    return n;
}

/* UTF-16BE [p, p + size) as UTF-8 into out, or only its length when out is NULL: up to a U+0000,
   an odd last byte dropped. */
static size_t mpsd__utf16be_utf8(const uint8_t *p, size_t size, uint8_t *out)
{
    const uint8_t *end;
    size_t         n = 0;
    if (!size) return 0;
    end = p + size;
    while (end - p >= 2) {
        const uint32_t cp = mpsd__utf16be(&p, end);
        if (cp == 0) break;                                    /* some writers count a final NUL */
        n += mpsd__put_utf8(out ? out + n : NULL, cp);
    }
    return n;
}

/* Does a UTF-8 name say what a file's name bytes say, as the reader reads them: UTF-16BE units up
   to a U+0000, or Latin-1 bytes (latin1)?  Code point by code point, U+FFFD for what is broken. */
static int mpsd__same_name(const char *utf8, const uint8_t *bytes, size_t size, int latin1)
{
    const unsigned char *p   = (const unsigned char *)(utf8 ? utf8 : "");
    const uint8_t       *q   = bytes;
    const uint8_t       *end = bytes + size;
    for (;;) {
        uint32_t cp = 0;
        if (latin1 && q < end)         cp = *q++;
        else if (!latin1 && end - q >= 2) cp = mpsd__utf16be(&q, end);
        if (cp == 0) return *p == 0;
        if (*p == 0 || mpsd__utf8(&p) != cp) return 0;
    }
}

#endif /* MPSD__UTF_DONE */
