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
#define MPSD__QUADRAPLE_COLOR_CHAN  4

/* A mode's colour channels, before transparency, alpha and spot channels: three in RGB and Lab,
   four in CMYK, every one of `channels` in multichannel, one in the rest (bitmap, grey, indexed,
   duotone).  Channel ids 0 .. colours - 1 in a layer, the first channels of the composite. */
static unsigned mpsd__colors(unsigned mode, unsigned channels)
{
    switch (mode) {
    case MPSD__MODE_RGB: case MPSD__MODE_LAB: return MPSD__TRIPLE_COLOR_CHAN;
    case MPSD__MODE_CMYK:                     return MPSD__QUADRAPLE_COLOR_CHAN;
    case MPSD__MODE_MULTICHANNEL:             return channels;
    default:                                  return MPSD__SINGLE_COLOR_CHAN;
    }
}

/* --- depths: a sample in memory, a row in the file --- */

/* A sample in memory takes 1 << shift bytes: one at 1 and 8 bits, two (a uint16_t) at 16, four (a
   float) at 32. */
static unsigned mpsd__shift(unsigned depth) { return depth == 32 ? 2u : depth == 16 ? 1u : 0u; }

/* A row of width samples as the file stores it: big-endian samples, or at 1 bit eight pixels a
   byte, the last one padded. */
static uint64_t mpsd__row_bytes(uint32_t width, unsigned depth) { return ((uint64_t)width * depth + 7) / 8; }

/* The full sample, opaque transparency: 1 at 1 bit, 255, 65535, and at 32 bits 1.0f's bits. */
static uint32_t mpsd__full(unsigned depth)
{
    return depth == 32 ? 0x3F800000u : depth == 16 ? 65535u : depth == 1 ? 1u : 255u;
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
    if (!shift) return *p;
    if (shift == 1) { memcpy(&h, p, 2); return h; }
    memcpy(&w, p, 4);
    return w;
}

static void mpsd__set_sample(uint8_t *p, unsigned shift, uint32_t v)
{
    const uint16_t h = (uint16_t)v;
    if (!shift)          *p = (uint8_t)v;
    else if (shift == 1) memcpy(p, &h, 2);
    else                 memcpy(p, &v, 4);
}

static float mpsd__float(const uint8_t *p)
{
    float f;
    memcpy(&f, p, 4);
    return f;
}

static void mpsd__set_float(uint8_t *p, float f) { memcpy(p, &f, 4); }

/* --- ZIP prediction: each sample stored as its difference from its left neighbour ---

   Bytes at 8 bits, big-endian samples at 16.  At 32 the row is shuffled first, every sample's
   first byte, then every second byte, and so on, and the differences are the shuffled row's
   bytes'.  1-bit rows are never predicted. */

/* Where byte i of a 32-bit row as the file stores it lies once the row is shuffled. */
static size_t mpsd__shuffled(size_t i, uint32_t width) { return (i & 3) * width + (i >> 2); }

/* A row of the file's bytes made differences, in place: right to left. */
static void mpsd__predict(uint8_t *row, uint32_t width, unsigned depth)
{
    size_t x = depth == 16 ? width : (size_t)mpsd__row_bytes(width, depth);
    while (x-- > 1) {
        if (depth == 16) {                                     /* big-endian, as the raw rows are */
            const uint32_t v = mpsd__get16(row + 2 * x) - mpsd__get16(row + 2 * x - 2);
            row[2 * x]     = (uint8_t)(v >> 8);
            row[2 * x + 1] = (uint8_t)v;
        } else {
            row[x] = (uint8_t)(row[x] - row[x - 1]);
        }
    }
}

/* The differences of a row summed back, in place: left to right.  A 32-bit row stays shuffled. */
static void mpsd__unpredict(uint8_t *row, uint32_t width, unsigned depth)
{
    const size_t n = depth == 16 ? width : (size_t)mpsd__row_bytes(width, depth);
    size_t       x;
    for (x = 1; x < n; ++x) {
        if (depth == 16) {
            const uint32_t v = mpsd__get16(row + 2 * x) + mpsd__get16(row + 2 * x - 2);
            row[2 * x]     = (uint8_t)(v >> 8);
            row[2 * x + 1] = (uint8_t)v;
        } else {
            row[x] = (uint8_t)(row[x] + row[x - 1]);
        }
    }
}

/* --- 1 bit: eight pixels a byte, the first in the high bit; a set bit is black --- */

/* A packed row, held in the first (width + 7) / 8 samples of a row in memory, step bytes apart,
   spread over its width samples in place, a byte of 0 or 1 each.  Right to left: sample x is
   read from sample x / 8, never one already written. */
static void mpsd__unpack_bits(uint8_t *row, ptrdiff_t step, uint32_t width)
{
    uint32_t x = width;
    while (x-- > 0) row[(ptrdiff_t)x * step] = (uint8_t)((row[(ptrdiff_t)(x >> 3) * step] >> (7 - (x & 7))) & 1);
}

#endif /* MPSD__SAMPLES_DONE */
