/**
 * \file minipsd/detail/samples.h
 * \brief Internal: samples as a file stores them and as a caller holds them, in every colour
 *        mode and depth: a mode's colour channels, a row's bytes, where a stored byte lies in
 *        memory, ZIP prediction both ways, 1-bit rows unpacked.  Included by the writer and the
 *        reader.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MPSD__SAMPLES_DONE
#define MPSD__SAMPLES_DONE

#include "bytes.h"

#include <string.h>

/* --- colour modes, as the header stores them --- */

#define MPSD__MODE_RGB          3
#define MPSD__MODE_CMYK         4
#define MPSD__MODE_MULTICHANNEL 7
#define MPSD__MODE_LAB          9

/* --- colour channels count --- */

#define MPSD__SINGLE_COLOR_CHAN     1
#define MPSD__TRIPLE_COLOR_CHAN     3
#define MPSD__QUADRUPLE_COLOR_CHAN  4

/* A mode's colour channels, before transparency, alpha and spot channels: three in RGB and Lab,
   four in CMYK, every one of `channels` in multichannel, one in the rest (bitmap, grey, indexed,
   duotone).  Channel ids 0 .. colours - 1 in a layer, the first channels of the composite. */
static unsigned mpsd__colors(unsigned mode, unsigned channels)
{
    switch (mode) {
    case MPSD__MODE_RGB: case MPSD__MODE_LAB: return MPSD__TRIPLE_COLOR_CHAN;
    case MPSD__MODE_CMYK:                     return MPSD__QUADRUPLE_COLOR_CHAN;
    case MPSD__MODE_MULTICHANNEL:             return channels;
    default:                                  return MPSD__SINGLE_COLOR_CHAN;
    }
}

/* --- depths: a sample in memory, a row in the file --- */

#define MPSD__CH_1BIT  1          /* the 1-bit channel size: bitmap, eight pixels a byte  */
#define MPSD__CH_8BIT  8          /* the 8-bit channel size                               */
#define MPSD__CH_16BIT 16         /* the 16-bit channel size: a uint16_t in memory        */
#define MPSD__CH_32BIT 32         /* the 32-bit channel size: a float in memory           */

#define MPSD__BYTE_BITS 8         /* bits a byte: 1-bit pixels a byte, 8-bit samples' size */

/* A sample in memory takes 1 << shift bytes. */
#define MPSD__SHIFT_8  0          /* a byte, at 1 and 8 bits */
#define MPSD__SHIFT_16 1          /* a uint16_t              */
#define MPSD__SHIFT_32 2          /* a float                 */

/* The full sample, opaque transparency. */
#define MPSD__FULL_1   1u
#define MPSD__FULL_8   255u
#define MPSD__FULL_16  65535u
#define MPSD__FULL_32  0x3F800000u   /* 1.0f's bits, IEEE 754 */

/* Is depth one a file may have? */
static int mpsd__depth_ok(uint32_t depth)
{
    return depth == MPSD__CH_1BIT || depth == MPSD__CH_8BIT || depth == MPSD__CH_16BIT || depth == MPSD__CH_32BIT;
}

static unsigned mpsd__shift(unsigned depth)
{
    return depth == MPSD__CH_32BIT ? MPSD__SHIFT_32 : depth == MPSD__CH_16BIT ? MPSD__SHIFT_16 : MPSD__SHIFT_8;
}

/* A row of width samples as the file stores it: big-endian samples, or at 1 bit eight pixels a
   byte, the last one padded. */
static uint64_t mpsd__row_bytes(uint32_t width, unsigned depth)
{
    return ((uint64_t)width * depth + MPSD__BYTE_BITS - 1) / MPSD__BYTE_BITS;
}

static uint32_t mpsd__full(unsigned depth)
{
    return depth == MPSD__CH_32BIT ? MPSD__FULL_32 : depth == MPSD__CH_16BIT ? MPSD__FULL_16
         : depth == MPSD__CH_1BIT ? MPSD__FULL_1 : MPSD__FULL_8;
}

/* Where byte i of a row as the file stores it (big-endian) lies in memory, the samples step bytes
   apart: a sample's high byte comes first in the file, last in memory on a little-endian host. */
static ptrdiff_t mpsd__at(size_t i, ptrdiff_t step, unsigned shift)
{
    static const uint16_t one = 1;
    const size_t byte = ((size_t)1 << shift) - 1;            /* the bits of i that pick a sample's byte */
    const size_t lane = *(const uint8_t *)&one ? byte : 0;   /* little-endian: reversed */
    return (ptrdiff_t)(i >> shift) * step + (ptrdiff_t)((i & byte) ^ lane);
}

/* A sample in memory: a byte, a uint16_t, or a float's bits. */
static uint32_t mpsd__sample(const uint8_t *p, unsigned shift)
{
    uint16_t h;
    uint32_t w;
    if (shift == MPSD__SHIFT_8) return *p;
    if (shift == MPSD__SHIFT_16) { memcpy(&h, p, sizeof h); return h; }
    memcpy(&w, p, sizeof w);
    return w;
}

static void mpsd__set_sample(uint8_t *p, unsigned shift, uint32_t v)
{
    const uint16_t h = (uint16_t)v;
    if (shift == MPSD__SHIFT_8)       *p = (uint8_t)v;
    else if (shift == MPSD__SHIFT_16) memcpy(p, &h, sizeof h);
    else                              memcpy(p, &v, sizeof v);
}

static float mpsd__float(const uint8_t *p)
{
    float f;
    memcpy(&f, p, sizeof f);
    return f;
}

static void mpsd__set_float(uint8_t *p, float f) { memcpy(p, &f, sizeof f); }

/* --- ZIP prediction: each sample stored as its difference from its left neighbour ---

   Bytes at 8 bits, big-endian samples at 16.  At 32 the row is shuffled first, every sample's
   first byte, then every second byte, and so on, and the differences are the shuffled row's
   bytes'.  1-bit rows are never predicted. */

/* Where byte i of a 32-bit row as the file stores it lies once the row is shuffled: its byte of
   the sample picks the run, the sample its place in the run. */
static size_t mpsd__shuffled(size_t i, uint32_t width)
{
    const size_t byte = ((size_t)1 << MPSD__SHIFT_32) - 1;
    return (i & byte) * width + (i >> MPSD__SHIFT_32);
}

/* What the differences of a row go over: its samples at 16 bits, its bytes otherwise. */
static size_t mpsd__units(uint32_t width, unsigned depth)
{
    return depth == MPSD__CH_16BIT ? width : (size_t)mpsd__row_bytes(width, depth);
}

/* Unit x of a row, from its left neighbour: the difference made (sum 0), or undone (sum 1). */
static void mpsd__delta(uint8_t *row, size_t x, unsigned depth, int sum)
{
    if (depth == MPSD__CH_16BIT) {                             /* big-endian, as the raw rows are */
        uint8_t       *s    = row + (x << MPSD__SHIFT_16);
        const uint32_t left = mpsd__get16(s - ((size_t)1 << MPSD__SHIFT_16));
        mpsd__set16(s, sum ? mpsd__get16(s) + left : mpsd__get16(s) - left);
    } else {
        row[x] = (uint8_t)(sum ? row[x] + row[x - 1] : row[x] - row[x - 1]);
    }
}

/* A row of the file's bytes made differences, in place: right to left. */
static void mpsd__predict(uint8_t *row, uint32_t width, unsigned depth)
{
    size_t x = mpsd__units(width, depth);
    while (x-- > 1) mpsd__delta(row, x, depth, 0);
}

/* The differences of a row summed back, in place: left to right.  A 32-bit row stays shuffled. */
static void mpsd__unpredict(uint8_t *row, uint32_t width, unsigned depth)
{
    const size_t n = mpsd__units(width, depth);
    size_t       x;
    for (x = 1; x < n; ++x) mpsd__delta(row, x, depth, 1);
}

/* --- 1 bit: eight pixels a byte, the first in the high bit; a set bit is black --- */

/* Pixel x of a packed row: 1 or 0. */
static uint8_t mpsd__bit(uint8_t packed, uint32_t x)
{
    return (uint8_t)((packed >> (MPSD__BYTE_BITS - 1 - x % MPSD__BYTE_BITS)) & 1u);
}

/* A packed row, held in the first (width + 7) / 8 samples of a row in memory, step bytes apart,
   spread over its width samples in place, a byte of 0 or 1 each.  Right to left: sample x is
   read from sample x / 8, never one already written. */
static void mpsd__unpack_bits(uint8_t *row, ptrdiff_t step, uint32_t width)
{
    uint32_t x = width;
    while (x-- > 0) row[(ptrdiff_t)x * step] = mpsd__bit(row[(ptrdiff_t)(x / MPSD__BYTE_BITS) * step], x);
}

#endif /* MPSD__SAMPLES_DONE */
