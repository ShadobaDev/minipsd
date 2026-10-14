/**
 * \file
 * \brief Internal: the output sink and big-endian integers.  Included by implementations only.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MPSD__BYTES_DONE
#define MPSD__BYTES_DONE

/* --- output: a sink either counts the bytes (the sizing pass) or sends them on --- */

typedef struct mpsd__sink {
    minipsd_write_fn fn;    /* NULL while counting */
    void            *user;
    uint64_t         count;
    int              failed;
} mpsd__sink;

static void mpsd__put(mpsd__sink *s, const void *data, size_t size)
{
    s->count += size;
    if (s->fn && !s->failed && size && !s->fn(s->user, data, size))
        s->failed = 1;
}

static void mpsd__u8(mpsd__sink *s, unsigned v)  { uint8_t b = (uint8_t)v; mpsd__put(s, &b, 1); }
static void mpsd__u16(mpsd__sink *s, unsigned v) { uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v }; mpsd__put(s, b, 2); }
static void mpsd__u32(mpsd__sink *s, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    mpsd__put(s, b, 4);
}
static void mpsd__zeros(mpsd__sink *s, size_t n) { while (n--) mpsd__u8(s, 0); }

static size_t mpsd__pad(size_t n, size_t to) { return (n + to - 1) / to * to; }

#endif /* MPSD__BYTES_DONE */
