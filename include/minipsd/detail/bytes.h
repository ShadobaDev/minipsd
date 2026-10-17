/**
 * \file
 * \brief Internal: the output sink, big-endian integers in both directions, and additional-
 *        information blocks.  Included by the writer and the reader.
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

/* The alignment the writer and the reader give their own state inside a caller's workspace. */
#define MPSD__ALIGN 8u

/* --- input: big-endian integers out of a buffer --- */

static uint32_t mpsd__get16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t mpsd__get32(const uint8_t *p) { return (mpsd__get16(p) << 16) | mpsd__get16(p + 2); }

/* Four bytes equal to a four-character tag. */
static int mpsd__tag(const uint8_t *p, const char *tag)
{
    size_t i;
    for (i = 0; i < 4; ++i)
        if (p[i] != (uint8_t)tag[i]) return 0;
    return 1;
}

/* --- additional information: a run of blocks, '8BIM' (or '8B64'), a key, a length, the data --- */

/* The block at pos of region; *next is where the one after it starts.  The length already
   includes any padding. */
static int mpsd__block(minipsd_bytes region, size_t pos, minipsd_block *block, size_t *next)
{
    const uint8_t *p    = region.data + pos;
    const size_t   left = region.size - pos;
    size_t         size, i;
    if (left < 12 || !(mpsd__tag(p, "8BIM") || mpsd__tag(p, "8B64"))) return MINIPSD_ERR_FORMAT;
    size = mpsd__get32(p + 8);
    if (size > left - 12) return MINIPSD_ERR_FORMAT;
    for (i = 0; i < 4; ++i) block->key[i] = (char)p[4 + i];
    block->key[4]    = '\0';
    block->data.data = p + 12;
    block->data.size = size;
    *next            = pos + 12 + size;
    return MINIPSD_OK;
}

#endif /* MPSD__BYTES_DONE */
