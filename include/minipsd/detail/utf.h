/**
 * \file
 * \brief Internal: UTF-8 to UTF-16, for layer names and Windows paths.  Included by implementations
 *        only.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MPSD__UTF_DONE
#define MPSD__UTF_DONE

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

#endif /* MPSD__UTF_DONE */
