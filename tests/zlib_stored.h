/**
 * \file
 * \brief zlib streams of stored blocks (RFC 1950 around RFC 1951's BTYPE 00): enough for the tests
 *        and the fuzzer to make ZIP channels and to inflate them through minipsd_inflate_fn, with
 *        no zlib on any toolchain.  A stream that uses compressed blocks fails to inflate.
 *
 * C and C++, static inline so that a file may use only part of it; include after minipsd.h.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MINIPSD_TESTS_ZLIB_STORED_H
#define MINIPSD_TESTS_ZLIB_STORED_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline uint32_t zlib_stored_adler(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    size_t   i;
    for (i = 0; i < n; ++i) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return b << 16 | a;
}

/* The stream of n bytes at src into dst, or only its size when dst is NULL. */
static inline size_t zlib_stored_deflate(const uint8_t *src, size_t n, uint8_t *dst)
{
    size_t         out = 2, done = 0;
    const uint32_t adler = zlib_stored_adler(src, n);
    if (dst) { dst[0] = 0x78; dst[1] = 0x01; }                 /* deflate, 32K window; FCHECK */
    do {
        const size_t len = n - done < 65535u ? n - done : 65535u;
        if (dst) {
            dst[out]     = (uint8_t)(done + len == n);         /* BFINAL, BTYPE 00 */
            dst[out + 1] = (uint8_t)len;
            dst[out + 2] = (uint8_t)(len >> 8);
            dst[out + 3] = (uint8_t)~len;
            dst[out + 4] = (uint8_t)(~len >> 8);
            if (len) memcpy(dst + out + 5, src + done, len);
        }
        out += 5 + len;
        done += len;
    } while (done < n);
    if (dst) {
        dst[out] = (uint8_t)(adler >> 24); dst[out + 1] = (uint8_t)(adler >> 16);
        dst[out + 2] = (uint8_t)(adler >> 8); dst[out + 3] = (uint8_t)adler;
    }
    return out + 4;
}

/* A minipsd_inflate_fn: 0 when src is a stream of stored blocks of exactly dst_size bytes. */
static inline int zlib_stored_inflate(void *user, const uint8_t *src, size_t src_size, uint8_t *dst, size_t dst_size)
{
    size_t at = 2, out = 0;
    int    last = 0;
    (void)user;
    if (src_size < 2 || (src[0] & 0x0F) != 8 || (src[0] << 8 | src[1]) % 31 || (src[1] & 0x20)) return 1;
    while (!last) {
        size_t len;
        if (src_size - at < 5 || (src[at] & 0x06)) return 1;   /* not a stored block */
        last = src[at] & 1;
        len  = (size_t)src[at + 1] | (size_t)src[at + 2] << 8;
        if (((src[at + 3] | src[at + 4] << 8) ^ 0xFFFF) != (int)len || src_size - at - 5 < len || dst_size - out < len) return 1;
        if (len) memcpy(dst + out, src + at + 5, len);
        at  += 5 + len;
        out += len;
    }
    return out != dst_size || src_size - at != 4
        || ((uint32_t)src[at] << 24 | (uint32_t)src[at + 1] << 16 | (uint32_t)src[at + 2] << 8 | src[at + 3]) != zlib_stored_adler(dst, out);
}

#endif /* MINIPSD_TESTS_ZLIB_STORED_H */
