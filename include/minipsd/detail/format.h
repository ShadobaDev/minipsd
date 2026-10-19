/**
 * \file minipsd/detail/format.h
 * \brief Internal: the layout of a PSD file, and what PSD and PSB differ in.  PSB (version 2) is PSD
 *        with wider fields: the layer and mask section's length, the layer info's, each layer
 *        channel's and the length of blocks with certain keys take eight bytes; RLE row counts
 *        take four.  Every structure's size, and every width that depends on the format, comes
 *        from here, for the reader and the writer alike.
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

/* A length of four bytes: the colour mode data's, the resources', a record's extra data's, the
   global layer mask's, in both formats.  A PSB widens some to eight (mpsd__len_size). */
#define MPSD__LEN 4u

/* A length that a PSB widens: four bytes, or (psb) eight. */
static unsigned mpsd__len_size(unsigned psb) { return MPSD__LEN << psb; }

static uint64_t mpsd__getlen(const uint8_t *p, unsigned psb) { return psb ? mpsd__get64(p) : mpsd__get32(p); }

static void mpsd__putlen(mpsd__sink *s, uint64_t v, unsigned psb)
{
    if (psb) mpsd__u32(s, (uint32_t)(v >> 32));
    mpsd__u32(s, (uint32_t)v);
}

/* An RLE row's byte count: two bytes, or four in a PSB. */
static unsigned mpsd__count_size(unsigned psb) { return 2u << psb; }

/* Row y's byte count in an RLE channel's table. */
static uint32_t mpsd__count(const uint8_t *table, uint32_t y, unsigned psb)
{
    const uint8_t *p = table + (size_t)y * mpsd__count_size(psb);
    return psb ? mpsd__get32(p) : mpsd__get16(p);
}

static void mpsd__putcount(mpsd__sink *s, uint32_t v, unsigned psb)
{
    if (psb) mpsd__u32(s, v);
    else     mpsd__u16(s, v);
}

/* --- the file: a header, then four sections ---

     the header                      offset  size
       signature '8BPS'                   0     4
       version: MINIPSD_PSD, _PSB         4     2
       reserved, zero                     6     6
       channels                          12     2
       height, width                     14   4+4
       depth: bits per channel           22     2
       colour mode                       24     2
                                         26
     colour mode data                     MPSD__LEN, then the data
     image resources                      MPSD__LEN, then resources
     layer and mask information           mpsd__len_size, then the layer info and the tail
     image data (the composite)           a compression marker, then every channel; no length */

#define MPSD__HEADER_SIZE  26
#define MPSD__MAX_CHANNELS 56   /* in the file, the composite's */
#define MPSD__MODE_RGB     3

/* The largest width or height a format allows. */
static uint32_t mpsd__max_side(unsigned psb) { return psb ? MINIPSD_MAX_SIDE_PSB : MINIPSD_MAX_SIDE; }

/* --- image resources ---

     a resource                      size
       signature '8BIM'                 4
       id                               2
       name: a Pascal string            1 + length, padded to even
       data length                      MPSD__LEN
       data                             padded to even */

#define MPSD__RES_NAME 6   /* where the name starts: after the signature and id */

#define MPSD__RES_RESOLUTION 1005
#define MPSD__RES_ICC        1039

/* Resolution info: horizontal resolution (16.16 fixed point, pixels per inch), its unit, the
   width's unit (4 + 2 + 2), then the same three for the vertical. */
#define MPSD__RESOLUTION_SIZE 16
#define MPSD__FIXED_ONE       65536.0   /* 1.0 in 16.16 fixed point */

static uint64_t mpsd__resource_size(size_t name, size_t data)
{
    return MPSD__RES_NAME + (uint64_t)mpsd__pad(1 + name, 2) + MPSD__LEN + (uint64_t)mpsd__pad(data, 2);
}

/* --- the layer info: a count, the records, then every layer's channel data --- */

#define MPSD__LAYER_COUNT 2       /* signed: negative when the composite has alpha */
#define MPSD__MAX_LAYERS  32767

/* A layer record                         PSD    PSB
     rectangle: top, left, bottom, right   16     16
     channel count                          2      2
     per channel: id, data length         2+4    2+8
     '8BIM', blend key                      8      8
     opacity, clipping, flags, filler       4      4
     extra data length                      4      4     MPSD__LEN in a PSB too
     extra data: the mask data, the blending ranges, the name (a Pascal string padded to four
     bytes), then blocks

   Blending ranges, after their length (MPSD__LEN; 0: none)      size
     per range, the composite grey's first, then each channel's:
       source: black low, black high, white low, white high       4
       destination: the same                                      4 */

#define MPSD__RECT_SIZE   16
#define MPSD__RECORD_HEAD 18      /* rectangle, channel count: the channel table starts here */
#define MPSD__RECORD_TAIL 16      /* after the channel table: '8BIM' .. the extra data's length */
#define MPSD__FLAG_HIDDEN 0x02u
#define MPSD__RANGE_SIZE  8       /* a blending range: source and destination */

/* The height of a rectangle laid out as above. */
static int64_t mpsd__rect_height(const uint8_t *rect)
{
    return (int64_t)(int32_t)mpsd__get32(rect + 8) - (int32_t)mpsd__get32(rect);
}

static unsigned mpsd__entry_size(unsigned psb) { return 2 + mpsd__len_size(psb); }

/* A record without its extra data. */
static uint64_t mpsd__record_size(uint32_t channels, unsigned psb)
{
    return MPSD__RECORD_HEAD + (uint64_t)channels * mpsd__entry_size(psb) + MPSD__RECORD_TAIL;
}

/* Layer mask data, after its length               size
     rectangle                                      16
     default colour                                  1
     flags                                           1
     the rest, kept as it is.  With a real mask, as psd-tools reads Photoshop's files (Adobe's
     specification puts the mask parameters first):
       real flags                                    1
       real background colour                        1
       real rectangle                               16
   A mask with no rest is padded with two bytes. */

#define MPSD__MASK_HEAD 18        /* rectangle, colour, flags: the rest starts here */
#define MPSD__MASK_PAD  2
#define MPSD__REAL_RECT 2         /* where the real rectangle starts in the rest */

/* A layer's channels by id: -1 transparency, 0 1 2 red green blue, -2 the mask, -3 the real mask;
   other ids are alpha and spot channels.  minipsd keeps them in slots: MINIPSD_CH_*, then the
   two masks. */
#define MPSD__RGBA     4          /* the colour channels and transparency; samples a pixel */
#define MPSD__CH_MASK  4
#define MPSD__CH_REAL  5
#define MPSD__CHANNELS 6          /* the most slots a layer has */

/* A channel's data, and the composite's: a compression marker, then the rows.  RLE rows follow a
   table of their byte counts, one per row (mpsd__count_size), every channel's in the composite. */
#define MPSD__MARKER_SIZE 2
#define MPSD__COMP_RAW    0
#define MPSD__COMP_RLE    1
#define MPSD__COMP_ZIP    2
#define MPSD__COMP_ZIP_PREDICTED 3

/* --- additional information: a run of blocks ---

     a block                         offset  size
       signature '8BIM' or '8B64'         0     4
       key                                4     4
       data length                        8     MPSD__LEN, or eight for a long key in a PSB
       data                                     as the reader walks a layer tail, padded to
                                                four bytes past its length */

#define MPSD__BLOCK_KEY  4
#define MPSD__BLOCK_LEN  8        /* the length: signature and key come before it */
#define MPSD__BLOCK_HEAD (MPSD__BLOCK_LEN + MPSD__LEN)   /* the shortest header */

/* An 'lsct' block: the section type, then, for a group's top record, '8BIM' and a blend key. */
#define MPSD__LSCT_TYPE  4
#define MPSD__LSCT_FULL 12

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

/* The bytes before the data of the block at p, laid out as a PSB's when psb. */
static size_t mpsd__block_head(const uint8_t *p, unsigned psb)
{
    return MPSD__BLOCK_LEN + mpsd__len_size(psb && mpsd__long_key(p + MPSD__BLOCK_KEY));
}

/* The block at pos of region, laid out as a PSB's when psb; *next is where the one after it
   starts.  The length already includes any padding. */
static int mpsd__block(minipsd_bytes region, size_t pos, unsigned psb, minipsd_block *block, size_t *next)
{
    const uint8_t *p    = region.data + pos;
    const size_t   left = region.size - pos;
    size_t         head, i;
    uint64_t       size;
    if (left < MPSD__BLOCK_HEAD || !(mpsd__tag(p, "8BIM") || mpsd__tag(p, "8B64"))) return MINIPSD_ERR_FORMAT;
    head = mpsd__block_head(p, psb);
    if (left < head) return MINIPSD_ERR_FORMAT;
    size = mpsd__getlen(p + MPSD__BLOCK_LEN, head > MPSD__BLOCK_HEAD);
    if (size > left - head) return MINIPSD_ERR_FORMAT;
    for (i = 0; i < 4; ++i) block->key[i] = (char)p[MPSD__BLOCK_KEY + i];
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
    mpsd__put(s, p, MPSD__BLOCK_LEN);
    mpsd__putlen(s, size, out && mpsd__long_key(p + MPSD__BLOCK_KEY));
    mpsd__put(s, p + head, span);
}

#endif /* MPSD__FORMAT_DONE */
