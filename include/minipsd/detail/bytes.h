/**
 * \file minipsd/detail/bytes.h
 * \brief Internal: the output sink, big-endian integers in both directions, samples of one or two
 *        bytes, and additional-information blocks.  Included by the writer and the reader.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MPSD__BYTES_DONE
#define MPSD__BYTES_DONE

#include <string.h>

/* --- output: a sink either counts the bytes (the sizing pass) or sends them on --- */

typedef struct mpsd__sink {
    minipsd_write_fn fn;        /* NULL while counting */
    void            *user;      /* user-provided context for the write function */
    uint64_t         count;     /* how many bytes have been sent or counted */
    int              failed;    /* 1 if the write function returned 0, else 0 */
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
static uint64_t mpsd__get64(const uint8_t *p) { return ((uint64_t)mpsd__get32(p) << 32) | mpsd__get32(p + 4); }

/* A length of four bytes, or (psb) of eight. */
static uint64_t mpsd__getlen(const uint8_t *p, unsigned psb) { return psb ? mpsd__get64(p) : mpsd__get32(p); }

/* Four bytes equal to a four-character tag. */
static int mpsd__tag(const uint8_t *p, const char *tag)
{
    size_t i;
    for (i = 0; i < 4; ++i)
        if (p[i] != (uint8_t)tag[i]) return 0;
    return 1;
}

/* --- samples: one byte, or (wide) two in host order, a uint16_t --- */

/* Where byte i of a row as the file stores it (big-endian) lies in memory, the samples step bytes
   apart: a sample's high byte comes first in the file, second in memory on a little-endian host. */
static ptrdiff_t mpsd__at(size_t i, ptrdiff_t step, unsigned wide)
{
    static const uint16_t one = 1;
    const unsigned lane = wide & *(const uint8_t *)&one;
    return (ptrdiff_t)(i >> wide) * step + (ptrdiff_t)((i & wide) ^ lane);
}

static uint32_t mpsd__sample(const uint8_t *p, unsigned wide)
{
    uint16_t v;
    if (!wide) return *p;
    memcpy(&v, p, 2);
    return v;
}

static void mpsd__set_sample(uint8_t *p, unsigned wide, uint32_t v)
{
    const uint16_t s = (uint16_t)v;
    if (wide) memcpy(p, &s, 2);
    else      *p = (uint8_t)v;
}

/* --- additional information: a run of blocks, '8BIM' (or '8B64'), a key, a length, the data --- */

/* The keys whose block length is eight bytes in a PSB, four in a PSD: the key decides, not the
   signature.  Adobe's specification names 13; psd-tools found 8 more in files Photoshop wrote
   ('lnkE' and 'pths' are in its test files), and this is its list. */
static int mpsd__long_key(const uint8_t *key)
{
    static const char keys[] = "LMskLr16Lr32LayrMt16Mt32MtrnAlphFMsklnk2lnk3lnkEFEidFXidFELSPxSDpthsextdextncinfartd";
    size_t i;
    for (i = 0; i + 4 < sizeof keys; i += 4)
        if (mpsd__tag(key, keys + i)) return 1;
    return 0;
}

/* The bytes before the data of the block at p: signature, key and a length of four bytes, or of
   eight for a long key in a PSB. */
static size_t mpsd__block_head(const uint8_t *p, unsigned psb) { return psb && mpsd__long_key(p + 4) ? 16 : 12; }

/* The block at pos of region, laid out as a PSB's when psb; *next is where the one after it
   starts.  The length already includes any padding. */
static int mpsd__block(minipsd_bytes region, size_t pos, unsigned psb, minipsd_block *block, size_t *next)
{
    const uint8_t *p    = region.data + pos;
    const size_t   left = region.size - pos;
    size_t         head, i;
    uint64_t       size;
    if (left < 12 || !(mpsd__tag(p, "8BIM") || mpsd__tag(p, "8B64"))) return MINIPSD_ERR_FORMAT;
    head = mpsd__block_head(p, psb);
    if (left < head) return MINIPSD_ERR_FORMAT;
    size = mpsd__getlen(p + 8, head == 16);
    if (size > left - head) return MINIPSD_ERR_FORMAT;
    for (i = 0; i < 4; ++i) block->key[i] = (char)p[4 + i];
    block->key[4]    = '\0';
    block->data.data = p + head;
    block->data.size = (size_t)size;
    *next            = pos + head + (size_t)size;
    return MINIPSD_OK;
}

#endif /* MPSD__BYTES_DONE */
