/**
 * \file minipsd/detail/format.h
 * \brief Internal: what PSD and PSB differ in.  PSB (version 2) is PSD with wider fields: the
 *        layer and mask section's length, the layer info's, each layer channel's and the length
 *        of blocks with certain keys take eight bytes; RLE row counts take four.  Every width that
 *        depends on the format comes from here, for the reader and the writer alike.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MPSD__FORMAT_DONE
#define MPSD__FORMAT_DONE

#include "bytes.h"

/* No length in a file comes near this: anything at or past it is malformed, and offsets built
   from lengths below it cannot wrap. */
#define MPSD__FAR ((uint64_t)1 << 62)

/* --- lengths and row counts --- */

/* A length of four bytes, or (psb) of eight. */
static uint64_t mpsd__getlen(const uint8_t *p, unsigned psb) { return psb ? mpsd__get64(p) : mpsd__get32(p); }

static void mpsd__putlen(mpsd__sink *s, uint64_t v, unsigned psb)
{
    if (psb) mpsd__u32(s, (uint32_t)(v >> 32));
    mpsd__u32(s, (uint32_t)v);
}

/* Row y's byte count in an RLE channel's table: two bytes, or four in a PSB. */
static uint32_t mpsd__count(const uint8_t *table, uint32_t y, unsigned psb)
{
    return psb ? mpsd__get32(table + 4 * (size_t)y) : mpsd__get16(table + 2 * (size_t)y);
}

static void mpsd__putcount(mpsd__sink *s, uint32_t v, unsigned psb)
{
    if (psb) mpsd__u32(s, v);
    else     mpsd__u16(s, v);
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

/* --- writing a block in the other format --- */

/* Can a block of this key go into the other format, its length re-written in that format's width
   and its data copied?  A short key's can: its data never depends on the format (psd-tools parses
   none of them differently).  So can the long keys checked against psd-tools' files, the PSD and
   PSB twins: masks' and transparency's parameters, paths, linked files (whose inner lengths are
   eight bytes in both) and descriptors.  Not yet: the layer structures ('Lr16', 'Lr32', 'Layr',
   whose data is a layer info of the format), and the long keys no file has shown. */
static int mpsd__convertible(const uint8_t *key)
{
    static const char keys[] = "FMskLMskMt32pthslnk2lnkEcinfartdextdextn";
    size_t i;
    if (!mpsd__long_key(key)) return 1;
    for (i = 0; i + 4 < sizeof keys; i += 4)
        if (mpsd__tag(key, keys + i)) return 1;
    return 0;
}

/* The block at p, whose header takes head bytes and whose length is size, written as the out
   format lays it out: signature and key, the length in out's width, then the span bytes after the
   header (the data, and any padding that follows it). */
static void mpsd__put_block(mpsd__sink *s, const uint8_t *p, size_t head, uint64_t size, size_t span, unsigned out)
{
    mpsd__put(s, p, 8);
    mpsd__putlen(s, size, out && mpsd__long_key(p + 4));
    mpsd__put(s, p + head, span);
}

#endif /* MPSD__FORMAT_DONE */
