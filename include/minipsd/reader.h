/**
 * \file minipsd/reader.h
 * \brief Implementation: the reader, a PSD held in memory walked in place.  Included by minipsd.h
 *        under MINIPSD_IMPLEMENTATION; the declarations are in minipsd.h.
 *
 * Nothing is allocated or copied: the reader keeps a few offsets in the caller's workspace and
 * hands out views into the caller's buffer.  Sections follow the order of the file: header, colour
 * mode data, image resources, layer and mask information, image data (the composite).
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MPSD__READER_DONE
#define MPSD__READER_DONE

#include "detail/bytes.h"
#include "detail/utf.h"
#include "detail/packbits.h"
#include "detail/composite.h"

/* --- the reader's state --- */

struct minipsd_reader {
    const uint8_t *data;          /* raw data, a binary PSD file                                 */
    size_t         size;          /* PSD file size                                               */
    uint64_t       needed;        /* what the last MINIPSD_ERR_SHORT asked for                   */
    minipsd_limits limits;        /* with zero fields replaced by the format's own limits        */
    uint32_t       layer_side;    /* the caller's max_side, for layers; 0: none                  */
    int            ready;         /* init succeeded                                              */
    int            rgb;           /* the file is 8- or 16-bit RGB: its pixels can be decoded     */
    unsigned       wide;          /* 16-bit: two bytes a sample                                  */
    uint32_t       width, height; /* the composite: its size, channels, and where it starts      */
    uint32_t       channels;      /* the composite: how many channels, 1..56; 0: not known yet   */
    int            composite_alpha; /* the composite: non-zero if it has an alpha channel        */
    uint64_t       image_start;   /* the composite: where its channel data starts                */
    uint64_t       res_next;      /* the resources: where the next one starts, where they end    */
    uint64_t       res_end;
    uint64_t       li_start;      /* the layer info: its first record, and the end of its content */
    uint64_t       li_end;
    size_t         layer_count;   /* the layer info: how many records, or 0 for a flat file      */
    size_t         layer_next;    /* the layers already returned                                 */
    uint64_t       record_next;   /* where the next layer record starts                          */
    uint64_t       data_next;     /* where the next layer's channel data starts                  */
    int            walked;        /* data_next is known (set by the first next_layer)            */
};

#define MPSD__MAX_LAYERS 32767   /* a layer count is a signed 16-bit number */

/* --- reading from a buffer that may end early --- */

/* Does the buffer reach `end`?  If not, remember how far it must, so the caller can read more. */
static int mpsd__have(minipsd_reader *r, uint64_t end)
{
    if (end <= r->size) return MINIPSD_OK;
    r->needed = end;
    return MINIPSD_ERR_SHORT;
}

/* A length field of four bytes at `at`. */
static int mpsd__length(minipsd_reader *r, uint64_t at, uint32_t *length)
{
    const int rc = mpsd__have(r, at + 4);
    if (rc == MINIPSD_OK) *length = mpsd__get32(r->data + (size_t)at);
    return rc;
}

/* The first bytes of the buffer, as many as there are, agree with the signature. */
static int mpsd__starts(const minipsd_reader *r, const char *signature)
{
    size_t i;
    for (i = 0; i < 4 && i < r->size; ++i)
        if (r->data[i] != (uint8_t)signature[i]) return 0;
    return 1;
}

/* --- image resources: '8BIM', a number, a name, a length, the data --- */

/* The resource at `at`, which lies in [at, res_end); *next is where the one after it starts.
   Padding of the name and of the data to even sizes is part of the block. */
static int mpsd__resource(const minipsd_reader *r, uint64_t at, minipsd_resource *res, uint64_t *next)
{
    const uint8_t *d = r->data;
    uint64_t       pos, size;
    if (r->res_end - at < 12 || !mpsd__tag(d + (size_t)at, "8BIM")) return MINIPSD_ERR_FORMAT;
    pos = at + 6 + mpsd__pad(1u + d[(size_t)at + 6], 2);      /* after the name */
    if (pos > r->res_end || r->res_end - pos < 4) return MINIPSD_ERR_FORMAT;
    size = mpsd__get32(d + (size_t)pos);
    pos += 4;
    if (size + (size & 1) > r->res_end - pos) return MINIPSD_ERR_FORMAT;
    res->id        = (uint16_t)mpsd__get16(d + (size_t)at + 4);
    res->name.data = d + (size_t)at + 7;
    res->name.size = d[(size_t)at + 6];
    res->data.data = d + (size_t)pos;
    res->data.size = (size_t)size;
    *next          = pos + size + (size & 1);
    return MINIPSD_OK;
}

/* Resolution info (1005): horizontal resolution as 16.16 fixed point, pixels per inch. */
#define MPSD__RES_RESOLUTION 1005
#define MPSD__RES_ICC        1039

/* --- the header and the sections before the layers --- */

/* All fields zero.  (Not "= {0}": a header that is also C++ cannot use it without a warning.) */
static void mpsd__info_clear(minipsd_info *i)
{
    i->width = i->height = 0;
    i->channels = i->depth = i->color_mode = 0;
    i->dpi = 0;
    i->icc.data = NULL;
    i->icc.size = 0;
    i->layer_count = 0;
    i->composite_has_alpha = 0;
    i->layer_tail.data = NULL;
    i->layer_tail.size = 0;
}

static int mpsd__depth_ok(uint32_t depth) { return depth == 1 || depth == 8 || depth == 16 || depth == 32; }

/* Colour modes: bitmap, grey, indexed, RGB, CMYK, multichannel, duotone, Lab. */
static int mpsd__mode_ok(uint32_t mode) { return mode <= 4 || (mode >= 7 && mode <= 9); }

/* The layer count at `at`, then the records and their channel data up to `end`: the layer info. */
static int mpsd__layers_at(minipsd_reader *r, uint64_t at, uint64_t end, minipsd_info *in)
{
    int32_t count;
    const int rc = mpsd__have(r, at + 2);
    if (rc != MINIPSD_OK) return rc;
    count = (int16_t)(uint16_t)mpsd__get16(r->data + (size_t)at);
    in->composite_has_alpha = count < 0;
    if (count < 0) count = -count;
    if (count > MPSD__MAX_LAYERS) return MINIPSD_ERR_FORMAT;
    if ((uint32_t)count > r->limits.max_layers) return MINIPSD_ERR_LIMIT;
    in->layer_count = (size_t)count;
    r->li_start     = at + 2;
    r->li_end       = end;
    return MINIPSD_OK;
}

/* Photoshop keeps the layers of a 16- or 32-bit file in a global block, 'Lr16' or 'Lr32', laid out
   as the layer info, and leaves the layer info empty.  Looks for one in the tail [at, end): the
   global layer mask, then blocks, each padded to four bytes past its length ('Lr16' 1218 long
   takes 1220).  A tail that is not blocks is only kept, so it is no error. */
static int mpsd__deep_layers(minipsd_reader *r, uint64_t at, uint64_t end, minipsd_info *in)
{
    const uint8_t *p;
    uint32_t       length;
    int            rc;
    if (end - at < 4) return MINIPSD_OK;
    rc = mpsd__length(r, at, &length);
    if (rc != MINIPSD_OK) return rc;
    for (at += 4 + (uint64_t)length; at < end && end - at >= 12; at += 12 + mpsd__pad(length, 4)) {
        rc = mpsd__have(r, at + 12);
        if (rc != MINIPSD_OK) return rc;
        p      = r->data + (size_t)at;
        length = mpsd__get32(p + 8);
        if (!(mpsd__tag(p, "8BIM") || mpsd__tag(p, "8B64")) || length > end - at - 12) return MINIPSD_OK;
        if (!mpsd__tag(p + 4, "Lr16") && !mpsd__tag(p + 4, "Lr32")) continue;
        if (length == 0) return MINIPSD_OK;
        return length == 1 ? MINIPSD_ERR_FORMAT : mpsd__layers_at(r, at + 12, at + 12 + length, in);
    }
    return MINIPSD_OK;
}

/* Reads everything init promises, in file order; each step asks for the bytes it needs. */
static int mpsd__scan(minipsd_reader *r, minipsd_info *info)
{
    const uint8_t *d = r->data;
    minipsd_info   in;
    uint64_t       at, lm_start;
    uint32_t       length, version;
    int            rc, has_dpi = 0, has_icc = 0;

    mpsd__info_clear(&in);
    /* The header: a stream that is not a PSD is refused as soon as four bytes show it. */
    if (!mpsd__starts(r, "8BPS")) return MINIPSD_ERR_FORMAT;
    rc = mpsd__have(r, 26);
    if (rc != MINIPSD_OK) return rc;
    version = mpsd__get16(d + 4);
    if (version == 2) return MINIPSD_ERR_UNSUPPORTED;
    in.channels   = (uint16_t)mpsd__get16(d + 12);
    in.height     = mpsd__get32(d + 14);
    in.width      = mpsd__get32(d + 18);
    in.depth      = (uint16_t)mpsd__get16(d + 22);
    in.color_mode = (uint16_t)mpsd__get16(d + 24);
    if (version != 1 || in.channels < 1 || in.channels > 56 || !mpsd__depth_ok(in.depth) || !mpsd__mode_ok(in.color_mode)
        || in.width < 1 || in.height < 1 || in.width > MINIPSD_MAX_SIDE || in.height > MINIPSD_MAX_SIDE)
        return MINIPSD_ERR_FORMAT;
    if (in.width > r->limits.max_side || in.height > r->limits.max_side) return MINIPSD_ERR_LIMIT;

    /* The colour mode data is not read: skip it. */
    rc = mpsd__length(r, 26, &length);
    if (rc != MINIPSD_OK) return rc;
    at = 30 + (uint64_t)length;

    /* The image resources, all of them: the resolution and the ICC profile are among them. */
    rc = mpsd__length(r, at, &length);
    if (rc != MINIPSD_OK) return rc;
    r->res_next = at + 4;
    r->res_end  = r->res_next + length;
    rc = mpsd__have(r, r->res_end);
    if (rc != MINIPSD_OK) return rc;
    for (at = r->res_next; at < r->res_end;) {                 /* checks every block as well */
        minipsd_resource res;
        rc = mpsd__resource(r, at, &res, &at);
        if (rc != MINIPSD_OK) return rc;
        if (res.id == MPSD__RES_RESOLUTION && !has_dpi) {
            if (res.data.size < 16) return MINIPSD_ERR_FORMAT;
            in.dpi  = (double)mpsd__get32(res.data.data) / 65536.0;
            has_dpi = 1;
        } else if (res.id == MPSD__RES_ICC && !has_icc) {
            in.icc  = res.data;
            has_icc = 1;
        }
    }

    /* The layer and mask information: its length, then the layer info's length and layer count. */
    rc = mpsd__length(r, r->res_end, &length);
    if (rc != MINIPSD_OK) return rc;
    lm_start = r->res_end + 4;
    if (length != 0) {
        uint32_t       info_length;
        uint64_t       tail;
        if (length < 4) return MINIPSD_ERR_FORMAT;
        rc = mpsd__length(r, lm_start, &info_length);
        if (rc != MINIPSD_OK) return rc;
        if (info_length > length - 4u || info_length == 1) return MINIPSD_ERR_FORMAT;
        tail = lm_start + 4 + info_length;                     /* the global layer mask, the global blocks */
        in.layer_tail.size = (size_t)(lm_start + length - tail);
        if (lm_start + length <= r->size) in.layer_tail.data = d + (size_t)tail;
        rc = info_length ? mpsd__layers_at(r, lm_start + 4, tail, &in) : MINIPSD_OK;
        if (rc == MINIPSD_OK && !in.layer_count) rc = mpsd__deep_layers(r, tail, lm_start + length, &in);
        if (rc != MINIPSD_OK) return rc;
    }
    r->layer_count = in.layer_count;
    r->record_next = r->li_start;
    r->rgb         = (in.depth == 8 || in.depth == 16) && in.color_mode == 3;
    r->wide        = in.depth == 16;
    r->width           = in.width;
    r->height          = in.height;
    r->channels        = in.channels;
    r->composite_alpha = in.composite_has_alpha;
    r->image_start     = lm_start + length;                   /* after the layer and mask section */
    *info = in;
    return MINIPSD_OK;
}

MINIPSDDEF int minipsd_reader_workspace_size(size_t *size)
{
    if (!size) return MINIPSD_ERR_ARG;
    *size = MPSD__ALIGN - 1 + mpsd__pad(sizeof(minipsd_reader), MPSD__ALIGN);
    return MINIPSD_OK;
}

MINIPSDDEF int minipsd_reader_init(minipsd_reader **out, const void *data, size_t size, minipsd_workspace *workspace,
                                   size_t workspace_size, const minipsd_limits *limits, minipsd_info *info)
{
    minipsd_reader *r;
    size_t          need = 0;
    uint8_t        *p;
    int             rc;
    if (!out) return MINIPSD_ERR_ARG;
    *out = NULL;
    (void)minipsd_reader_workspace_size(&need);
    if (!info || !workspace || workspace_size < need || (!data && size)) return MINIPSD_ERR_ARG;
    mpsd__info_clear(info);
    p = (uint8_t *)workspace;
    p += (MPSD__ALIGN - (uintptr_t)p % MPSD__ALIGN) % MPSD__ALIGN;
    r = (minipsd_reader *)p;

    r->data         = (const uint8_t *)data;
    r->size         = size;
    r->needed       = 0;
    r->limits.max_side   = limits && limits->max_side ? limits->max_side : MINIPSD_MAX_SIDE;
    r->limits.max_layers = limits && limits->max_layers ? limits->max_layers : MPSD__MAX_LAYERS;
    r->limits.max_decoded_bytes = limits ? limits->max_decoded_bytes : 0;
    r->layer_side        = limits ? limits->max_side : 0;
    r->ready        = 0;
    r->rgb          = 0;
    r->wide         = 0;
    r->res_next     = r->res_end = 0;
    r->li_start     = r->li_end = 0;
    r->layer_count  = r->layer_next = 0;
    r->record_next  = r->data_next = 0;
    r->walked       = 0;

    rc = mpsd__scan(r, info);
    if (rc == MINIPSD_OK) r->ready = 1;
    if (rc == MINIPSD_OK || rc == MINIPSD_ERR_SHORT) *out = r;
    return rc;
}

MINIPSDDEF uint64_t minipsd_reader_needed(const minipsd_reader *r)
{
    return r ? r->needed : 0;
}

MINIPSDDEF int minipsd_reader_next_resource(minipsd_reader *r, minipsd_resource *res)
{
    if (!r || !res || !r->ready) return MINIPSD_ERR_ARG;
    if (r->res_next >= r->res_end) return MINIPSD_OK;
    (void)mpsd__resource(r, r->res_next, res, &r->res_next);   /* init checked every block */
    return MINIPSD_MORE;
}

/* --- layers: a record each, then every layer's channel data --- */

/* One layer record, parsed.  The channel data is not in the record: for the colour channels it
   keeps where each one starts in the layer's run of channel data, and how long it is. */
typedef struct mpsd__rec {
    minipsd_layer_view view;
    uint64_t           offset[6];   /* in the layer's channel data, by MINIPSD_CH_*, then the mask, the real mask */
    uint32_t           length[6];   /* 0: the layer has no such channel             */
    uint64_t           total;       /* all of its channel data, other channels too  */
    uint64_t           next;        /* where the next record starts                 */
} mpsd__rec;

/* A range of the layer info: past li_end it is malformed, past the buffer only the buffer is short. */
static int mpsd__room(minipsd_reader *r, uint64_t end)
{
    return end > r->li_end ? MINIPSD_ERR_FORMAT : mpsd__have(r, end);
}

/* Where a PSD channel id goes: MINIPSD_CH_*, 4 the mask (-2), 5 the real mask (-3), or -1 for a
   channel the reader does not read. */
static int mpsd__slot(int id)
{
    return id >= 0 && id <= 2 ? id : id == -1 ? MINIPSD_CH_A : id == -2 ? 4 : id == -3 ? 5 : -1;
}

/* The name block 'luni': a count of UTF-16 units, then the units. */
static int mpsd__luni(minipsd_bytes data, minipsd_bytes *name)
{
    size_t units;
    if (data.size < 4) return MINIPSD_ERR_FORMAT;
    units = mpsd__get32(data.data);
    if (units > (data.size - 4) / 2) return MINIPSD_ERR_FORMAT;
    name->data = data.data + 4;
    name->size = units * 2;
    return MINIPSD_OK;
}

/* The extra data of a record, [at, end): masks and blending ranges skipped, the legacy name, then
   the blocks, which are walked here to check them and to find 'luni'. */
static int mpsd__extra(const minipsd_reader *r, uint64_t at, uint64_t end, minipsd_layer_view *v)
{
    const uint8_t *d = r->data;
    minipsd_block  block;
    size_t         pos;
    uint32_t       skip;
    int            k, rc;
    for (k = 0; k < 2; ++k) {                                 /* layer mask data, blending ranges */
        if (end - at < 4) return MINIPSD_ERR_FORMAT;
        skip = mpsd__get32(d + (size_t)at);
        at += 4;
        if (skip > end - at) return MINIPSD_ERR_FORMAT;
        if (k == 0 && skip >= 18) {                            /* rectangle, colour, flags, the rest */
            const uint8_t *m = d + (size_t)at;
            minipsd_mask  *mask = &v->mask;
            mask->top    = (int32_t)mpsd__get32(m);     mask->left  = (int32_t)mpsd__get32(m + 4);
            mask->bottom = (int32_t)mpsd__get32(m + 8); mask->right = (int32_t)mpsd__get32(m + 12);
            if (mask->bottom < mask->top) mask->bottom = mask->top;   /* inverted: empty, as layers */
            if (mask->right < mask->left) mask->right = mask->left;
            mask->color     = m[16];
            mask->flags     = m[17];
            mask->rest.data = m + 18;
            mask->rest.size = skip - 18;
            v->has_mask     = 1;
        }
        at += skip;
    }
    if (end == at || mpsd__pad(1u + d[(size_t)at], 4) > end - at) return MINIPSD_ERR_FORMAT;
    v->name_pascal.data = d + (size_t)at + 1;
    v->name_pascal.size = d[(size_t)at];
    at += mpsd__pad(1u + d[(size_t)at], 4);
    v->extra.data = d + (size_t)at;
    v->extra.size = (size_t)(end - at);
    for (pos = 0; pos < v->extra.size;) {
        rc = mpsd__block(v->extra, pos, &block, &pos);
        if (rc != MINIPSD_OK) return rc;
        if (mpsd__tag((const uint8_t *)block.key, "luni") && !v->name_unicode.data) {
            rc = mpsd__luni(block.data, &v->name_unicode);
            if (rc != MINIPSD_OK) return rc;
        }
        if (mpsd__tag((const uint8_t *)block.key, "lsct") && block.data.size >= 4 && !v->kind) {
            const uint32_t type = mpsd__get32(block.data.data);
            v->kind = (uint8_t)(type <= (uint32_t)MINIPSD_KIND_GROUP_END ? type : 0u);   /* unknown: a layer */
        }
    }
    return MINIPSD_OK;
}

/* The record at `at`, which must lie in the layer info. */
static int mpsd__record(minipsd_reader *r, uint64_t at, mpsd__rec *rec)
{
    const uint8_t      *d = r->data;
    minipsd_layer_view *v = &rec->view;
    uint64_t            pos, extra_end;
    uint32_t            channels, k, extra;
    int                 rc;
    int32_t             rect[4];

    rc = mpsd__room(r, at + 18);                               /* the rectangle and the channel count */
    if (rc != MINIPSD_OK) return rc;
    channels = mpsd__get16(d + (size_t)at + 16);
    pos      = at + 18 + 6 * (uint64_t)channels;               /* after the channel table */
    rc = mpsd__room(r, pos + 16);                              /* blend, flags, extra length */
    if (rc != MINIPSD_OK) return rc;

    for (k = 0; k < 4; ++k) {
        rect[k]        = (int32_t)mpsd__get32(d + (size_t)at + 4 * (size_t)k);
        v->channel[k].data = NULL;
        v->channel[k].size = 0;
    }
    for (k = 0; k < 6; ++k) rec->offset[k] = rec->length[k] = 0;
    memset(&v->mask, 0, sizeof v->mask);
    v->kind = v->has_mask = 0;
    v->mask_offset = 0;
    /* An inverted rectangle is an empty one: Photoshop writes (0, 0, -1, 0) for a fill layer
       that has no pixels of its own. */
    if (rect[2] < rect[0]) rect[2] = rect[0];
    if (rect[3] < rect[1]) rect[3] = rect[1];
    v->top = rect[0]; v->left = rect[1]; v->bottom = rect[2]; v->right = rect[3];
    v->other_channels = 0;
    rec->total        = 0;
    for (k = 0; k < channels; ++k) {
        const uint8_t *entry = d + (size_t)at + 18 + 6 * (size_t)k;
        const uint32_t length = mpsd__get32(entry + 2);
        const int      slot   = mpsd__slot((int16_t)(uint16_t)mpsd__get16(entry));
        if (length < 2 || (slot >= 0 && rec->length[slot])) return MINIPSD_ERR_FORMAT;   /* a marker, and no repeats */
        if (slot >= 0) {
            rec->offset[slot] = rec->total;
            rec->length[slot] = length;
        } else {
            ++v->other_channels;
        }
        rec->total += length;
    }

    if (!mpsd__tag(d + (size_t)pos, "8BIM")) return MINIPSD_ERR_FORMAT;
    for (k = 0; k < 4; ++k) v->blend[k] = (char)d[(size_t)pos + 4 + k];
    v->blend[4] = '\0';
    v->opacity  = d[(size_t)pos + 8];
    v->clipping = d[(size_t)pos + 9];
    v->flags    = d[(size_t)pos + 10];
    v->hidden   = (v->flags & 0x02) != 0;
    extra       = mpsd__get32(d + (size_t)pos + 12);
    extra_end   = pos + 16 + extra;
    rc = mpsd__room(r, extra_end);
    if (rc != MINIPSD_OK) return rc;
    v->name_pascal.data = v->name_unicode.data = v->extra.data = NULL;
    v->name_pascal.size = v->name_unicode.size = v->extra.size = 0;
    rc = mpsd__extra(r, pos + 16, extra_end, v);
    rec->next = extra_end;
    if (!v->has_mask) {                                        /* mask channels with no mask data */
        v->other_channels = (uint16_t)(v->other_channels + (rec->length[4] != 0) + (rec->length[5] != 0));
        rec->length[4] = rec->length[5] = 0;
    }
    return rc;
}

/* The channel data of all layers follows the last record.  Finds where, by walking the records
   once; also checks that the data they declare fits the layer info. */
static int mpsd__walk(minipsd_reader *r)
{
    mpsd__rec rec;
    uint64_t  at = r->li_start, total = 0;
    size_t    i;
    for (i = 0; i < r->layer_count; ++i) {
        const int rc = mpsd__record(r, at, &rec);
        if (rc != MINIPSD_OK) return rc;
        at = rec.next;
        total += rec.total;
    }
    if (total > r->li_end - at) return MINIPSD_ERR_FORMAT;
    r->data_next = at;
    r->walked    = 1;
    return MINIPSD_OK;
}

MINIPSDDEF int minipsd_reader_next_layer(minipsd_reader *r, minipsd_layer_view *layer)
{
    mpsd__rec rec;
    int       rc, k;
    if (!r || !layer || !r->ready) return MINIPSD_ERR_ARG;
    if (r->layer_next >= r->layer_count) return MINIPSD_OK;
    if (!r->walked) {
        rc = mpsd__walk(r);
        if (rc != MINIPSD_OK) return rc;
    }
    (void)mpsd__record(r, r->record_next, &rec);              /* the walk checked every record */
    if (r->layer_side && ((uint64_t)((int64_t)rec.view.right - rec.view.left) > r->layer_side
                          || (uint64_t)((int64_t)rec.view.bottom - rec.view.top) > r->layer_side))
        return MINIPSD_ERR_LIMIT;
    for (k = 0; k < 6; ++k) {
        const uint64_t at   = r->data_next + rec.offset[k];
        minipsd_bytes *view = k < 4 ? &rec.view.channel[k] : k == 4 ? &rec.view.mask.stored : &rec.view.mask.real;
        view->size = rec.length[k];
        view->data = rec.length[k] && at + rec.length[k] <= r->size ? r->data + (size_t)at : NULL;
        if (k < 4) rec.view.channel_offset[k] = rec.length[k] ? at : 0;
        if (k == 4) rec.view.mask_offset = rec.length[k] ? at : 0;
    }
    *layer         = rec.view;
    r->record_next = rec.next;
    r->data_next  += rec.total;
    ++r->layer_next;
    return MINIPSD_MORE;
}

/* The name as UTF-8 into out, or only its length when out is NULL. */
static size_t mpsd__name_utf8(const minipsd_layer_view *layer, uint8_t *out)
{
    size_t n = 0, i;
    if (layer->name_unicode.data) {
        const uint8_t *p   = layer->name_unicode.data;
        const uint8_t *end = p + layer->name_unicode.size;
        while (p < end) {
            const uint32_t cp = mpsd__utf16be(&p, end);
            if (cp == 0) break;                                /* some writers count a final NUL */
            n += mpsd__put_utf8(out ? out + n : NULL, cp);
        }
    } else {                                                   /* Latin-1: each byte is a code point */
        for (i = 0; i < layer->name_pascal.size; ++i)
            n += mpsd__put_utf8(out ? out + n : NULL, layer->name_pascal.data[i]);
    }
    return n;
}

MINIPSDDEF int minipsd_layer_name(const minipsd_layer_view *layer, char *utf8, size_t cap, size_t *needed)
{
    size_t n;
    if (!layer || (!utf8 && cap)) return MINIPSD_ERR_ARG;
    n = mpsd__name_utf8(layer, NULL) + 1;
    if (needed) *needed = n;
    if (cap < n) {
        if (cap) utf8[0] = '\0';
        return MINIPSD_MORE;
    }
    mpsd__name_utf8(layer, (uint8_t *)utf8);
    utf8[n - 1] = '\0';
    return MINIPSD_OK;
}

MINIPSDDEF int minipsd_layer_next_block(const minipsd_layer_view *layer, size_t *pos, minipsd_block *block)
{
    size_t next;
    int    rc;
    if (!layer || !pos || !block) return MINIPSD_ERR_ARG;
    if (*pos >= layer->extra.size) return MINIPSD_OK;
    rc = mpsd__block(layer->extra, *pos, block, &next);
    if (rc != MINIPSD_OK) return rc;
    *pos = next;
    return MINIPSD_MORE;
}

/* --- pixels: a layer's channels into the caller's planes --- */

/* Do the bytes one decode call writes, width * rows samples for each plane, stay within the limit? */
static int mpsd__within(const minipsd_reader *r, const minipsd_target *t, uint32_t width, uint32_t rows, unsigned wide)
{
    const uint64_t max = r->limits.max_decoded_bytes;
    uint64_t       planes = 0;
    int            k;
    for (k = 0; k < 4; ++k) planes += t->channel[k].data != NULL;
    return !max || !planes || !rows || width <= (max >> wide) / planes / rows;   /* no product to overflow */
}

/* The samples opts ask for: 0 one byte, 1 two (wide); -1 a depth minipsd does not decode into. */
static int mpsd__opts_wide(const minipsd_decode_opts *opts)
{
    const int depth = opts ? opts->depth : 0;
    return depth == 0 || depth == 8 ? 0 : depth == 16 ? 1 : -1;
}

/* Can the file's pixels be decoded into samples that wide? */
static int mpsd__decodes(const minipsd_reader *r, int wide)
{
    return r->rgb && (unsigned)wide == r->wide;
}

/* Fills rows of a plane with full samples: the transparency of a file that stores none. */
static void mpsd__opaque(const minipsd_plane *pl, uint32_t width, uint32_t rows, unsigned wide)
{
    uint32_t x, y;
    for (y = 0; y < rows; ++y)
        for (x = 0; x < width; ++x)
            mpsd__set_sample(pl->data + (ptrdiff_t)y * pl->row_step + (ptrdiff_t)x * pl->pixel_step, wide, wide ? 65535u : 255u);
}

/* Four planes interleaved, samples of one byte or (wide) two. */
static void mpsd__target(minipsd_target *t, uint8_t *rgba, size_t row_stride, unsigned wide)
{
    int k;
    if (!t) return;
    for (k = 0; k < 4; ++k) {
        t->channel[k].data       = rgba ? rgba + ((size_t)k << wide) : NULL;
        t->channel[k].pixel_step = (ptrdiff_t)4 << wide;
        t->channel[k].row_step   = (ptrdiff_t)row_stride;
    }
}

MINIPSDDEF void minipsd_target_rgba(minipsd_target *t, uint8_t *rgba, size_t row_stride)
{
    mpsd__target(t, rgba, row_stride, 0);
}

MINIPSDDEF void minipsd_target_rgba16(minipsd_target *t, uint16_t *rgba, size_t row_stride)
{
    mpsd__target(t, (uint8_t *)rgba, row_stride, 1);
}

/* One channel's stored data, checked whole before a sample is written: the compression marker,
   then raw rows, or (RLE) a byte count per row and the packed rows.  A row is width samples of the
   file's depth, big-endian. */
typedef struct mpsd__chan {
    const uint8_t *table;   /* RLE: the row byte counts; NULL for raw */
    const uint8_t *rows;    /* the first row's data                   */
    unsigned       wide;    /* two bytes a sample                     */
} mpsd__chan;

static int mpsd__chan_open(minipsd_bytes c, uint32_t width, uint32_t height, unsigned wide, mpsd__chan *ch)
{
    uint64_t left, sum = 0;
    uint32_t y;
    left     = c.size - 2;                                     /* next_layer saw the marker */
    ch->wide = wide;
    switch (mpsd__get16(c.data)) {
    case 0:                                                    /* raw */
        if (((uint64_t)width * height << wide) > left) return MINIPSD_ERR_FORMAT;
        ch->table = NULL;
        ch->rows  = c.data + 2;
        return MINIPSD_OK;
    case 1:                                                    /* PackBits */
        if (2 * (uint64_t)height > left) return MINIPSD_ERR_FORMAT;
        ch->table = c.data + 2;
        ch->rows  = ch->table + 2 * (size_t)height;
        for (y = 0; y < height; ++y) sum += mpsd__get16(ch->table + 2 * (size_t)y);
        return sum > left - 2 * (uint64_t)height ? MINIPSD_ERR_FORMAT : MINIPSD_OK;
    case 2: case 3:                                            /* ZIP, with or without prediction */
        return MINIPSD_ERR_UNSUPPORTED;
    default:
        return MINIPSD_ERR_FORMAT;
    }
}

/* Rows [y0, y0 + rows) of a checked channel into a plane. */
static int mpsd__chan_rows(const mpsd__chan *ch, uint32_t width, uint32_t y0, uint32_t rows, const minipsd_plane *pl)
{
    const uint8_t *src   = ch->rows;
    const uint32_t bytes = width << ch->wide;                  /* a row as stored; a side fits 30000 */
    uint32_t       y, x;
    if (!ch->table) {
        src += (size_t)y0 * bytes;
        for (y = 0; y < rows; ++y, src += bytes) {
            uint8_t *out = pl->data + (ptrdiff_t)y * pl->row_step;
            for (x = 0; x < bytes; ++x) out[mpsd__at(x, pl->pixel_step, ch->wide)] = src[x];
        }
        return MINIPSD_OK;
    }
    for (y = 0; y < y0; ++y) src += mpsd__get16(ch->table + 2 * (size_t)y);   /* to the band */
    for (y = 0; y < rows; ++y) {
        const size_t n = mpsd__get16(ch->table + 2 * ((size_t)y0 + y));
        if (!mpsd__unpackbits(src, n, pl->data + (ptrdiff_t)y * pl->row_step, pl->pixel_step, bytes, ch->wide))
            return MINIPSD_ERR_FORMAT;
        src += n;
    }
    return MINIPSD_OK;
}

MINIPSDDEF int minipsd_decode_layer(minipsd_reader *r, const minipsd_layer_view *layer, const minipsd_target *target,
                                    const minipsd_decode_opts *opts)
{
    mpsd__chan ch[4];
    uint64_t   short_end = 0;
    uint32_t   width, height, y0, rows;
    int        k, rc, wide = mpsd__opts_wide(opts);
    if (!r || !layer || !target || !r->ready || wide < 0) return MINIPSD_ERR_ARG;
    width  = (uint32_t)((int64_t)layer->right - layer->left);    /* next_layer checked the order */
    height = (uint32_t)((int64_t)layer->bottom - layer->top);
    y0     = opts ? opts->y_first : 0;
    if (y0 > height) return MINIPSD_ERR_ARG;
    rows   = opts && opts->y_count ? opts->y_count : height - y0;
    if (rows > height - y0) return MINIPSD_ERR_ARG;
    if (!mpsd__within(r, target, width, rows, (unsigned)wide)) return MINIPSD_ERR_LIMIT;
    if (!mpsd__decodes(r, wide)) return MINIPSD_ERR_UNSUPPORTED;
    if (width == 0 || rows == 0) return MINIPSD_OK;

    /* Every channel the target asks for: present, in the buffer, well formed. */
    for (k = 0; k < 4; ++k) {
        const minipsd_bytes c = layer->channel[k];
        if (!target->channel[k].data || (k == MINIPSD_CH_A && c.size == 0)) continue;
        if (c.size == 0) return MINIPSD_ERR_FORMAT;            /* a colour channel is missing */
        if (!c.data && layer->channel_offset[k] + c.size > short_end) short_end = layer->channel_offset[k] + c.size;
    }
    if (short_end) {
        r->needed = short_end;
        return MINIPSD_ERR_SHORT;
    }
    for (k = 0; k < 4; ++k) {
        if (!target->channel[k].data || layer->channel[k].size == 0) continue;
        rc = mpsd__chan_open(layer->channel[k], width, height, r->wide, &ch[k]);
        if (rc != MINIPSD_OK) return rc;
    }

    for (k = 0; k < 4; ++k) {
        const minipsd_plane *pl = &target->channel[k];
        if (!pl->data) continue;
        if (layer->channel[k].size == 0) {                     /* no transparency: opaque */
            mpsd__opaque(pl, width, rows, r->wide);
            continue;
        }
        rc = mpsd__chan_rows(&ch[k], width, y0, rows, pl);
        if (rc != MINIPSD_OK) return rc;
    }
    return MINIPSD_OK;
}

MINIPSDDEF int minipsd_decode_mask(minipsd_reader *r, const minipsd_layer_view *layer, const minipsd_plane *plane,
                                   const minipsd_decode_opts *opts)
{
    minipsd_target t;
    mpsd__chan     ch;
    minipsd_bytes  c;
    uint32_t       width, height, y0, rows;
    int            k, rc, wide = mpsd__opts_wide(opts);
    if (!r || !layer || !plane || !r->ready || !layer->has_mask || wide < 0) return MINIPSD_ERR_ARG;
    width  = (uint32_t)((int64_t)layer->mask.right - layer->mask.left);   /* next_layer made it not inverted */
    height = (uint32_t)((int64_t)layer->mask.bottom - layer->mask.top);
    y0     = opts ? opts->y_first : 0;
    if (y0 > height) return MINIPSD_ERR_ARG;
    rows   = opts && opts->y_count ? opts->y_count : height - y0;
    if (rows > height - y0) return MINIPSD_ERR_ARG;
    for (k = 0; k < 4; ++k) t.channel[k].data = NULL;
    t.channel[0] = *plane;
    if (!mpsd__within(r, &t, width, rows, (unsigned)wide)) return MINIPSD_ERR_LIMIT;
    if (!mpsd__decodes(r, wide)) return MINIPSD_ERR_UNSUPPORTED;
    if (width == 0 || rows == 0) return MINIPSD_OK;
    if (!plane->data) return MINIPSD_ERR_ARG;
    c = layer->mask.stored;
    if (c.size == 0) return MINIPSD_ERR_FORMAT;                 /* a mask with an area, no channel */
    if (!c.data) {
        r->needed = layer->mask_offset + c.size;
        return MINIPSD_ERR_SHORT;
    }
    rc = mpsd__chan_open(c, width, height, r->wide, &ch);
    return rc != MINIPSD_OK ? rc : mpsd__chan_rows(&ch, width, y0, rows, plane);
}

/* --- pixels: the composite, every channel's rows after one compression marker --- */

/* Where the composite's channels 0 .. last are, checked whole in the buffer before a sample is
   written: raw planes back to back, or (RLE) every channel's row counts, then every row. */
static int mpsd__composite_open(minipsd_reader *r, int last, mpsd__chan *ch)
{
    const uint8_t *d = r->data;
    const uint64_t at = r->image_start + 2, plane = (uint64_t)r->width * r->height << r->wide;
    uint64_t       sum = 0, offset[4], rows_at;
    uint32_t       y, n;
    int            k, rc = mpsd__have(r, at);
    if (rc != MINIPSD_OK) return rc;
    switch (mpsd__get16(d + (size_t)r->image_start)) {
    case 0:
        rc = mpsd__have(r, at + plane * (uint64_t)(last + 1));
        for (k = 0; rc == MINIPSD_OK && k <= last; ++k) {
            ch[k].table = NULL;
            ch[k].rows  = d + (size_t)(at + plane * (uint64_t)k);
            ch[k].wide  = r->wide;
        }
        return rc;
    case 1:
        rows_at = at + 2 * (uint64_t)r->channels * r->height;   /* the counts of every channel come first */
        rc = mpsd__have(r, rows_at);
        if (rc != MINIPSD_OK) return rc;
        for (k = 0; k <= last; ++k) {
            offset[k] = sum;
            for (y = 0, n = r->height; y < n; ++y) sum += mpsd__get16(d + (size_t)at + 2 * ((size_t)k * n + y));
        }
        rc = mpsd__have(r, rows_at + sum);
        for (k = 0; rc == MINIPSD_OK && k <= last; ++k) {
            ch[k].table = d + (size_t)at + 2 * (size_t)k * r->height;
            ch[k].rows  = d + (size_t)(rows_at + offset[k]);
            ch[k].wide  = r->wide;
        }
        return rc;
    case 2: case 3:
        return MINIPSD_ERR_UNSUPPORTED;
    default:
        return MINIPSD_ERR_FORMAT;
    }
}

MINIPSDDEF int minipsd_decode_composite(minipsd_reader *r, const minipsd_target *target, const minipsd_decode_opts *opts)
{
    mpsd__chan ch[4];
    uint32_t   y0, rows, y, x;
    int        k, rc, last = -1, unmatte, colour, wide = mpsd__opts_wide(opts);
    if (!r || !target || !r->ready || wide < 0) return MINIPSD_ERR_ARG;
    y0 = opts ? opts->y_first : 0;
    if (y0 > r->height) return MINIPSD_ERR_ARG;
    rows = opts && opts->y_count ? opts->y_count : r->height - y0;
    if (rows > r->height - y0) return MINIPSD_ERR_ARG;
    for (k = 0; k < 3; ++k)
        if (target->channel[k].data) last = k;
    colour  = last >= 0;
    unmatte = r->composite_alpha && !(opts && opts->keep_matte) && colour;
    if (unmatte && !target->channel[MINIPSD_CH_A].data) return MINIPSD_ERR_ARG;   /* the alpha is read back */
    if (!mpsd__within(r, target, r->width, rows, (unsigned)wide)) return MINIPSD_ERR_LIMIT;
    if (!mpsd__decodes(r, wide)) return MINIPSD_ERR_UNSUPPORTED;
    if (r->channels < 3u + (r->composite_alpha ? 1u : 0u)) return MINIPSD_ERR_FORMAT;
    if (target->channel[MINIPSD_CH_A].data && r->composite_alpha) last = MINIPSD_CH_A;
    if (rows == 0) return MINIPSD_OK;
    if (last >= 0) {
        rc = mpsd__composite_open(r, last, ch);
        if (rc != MINIPSD_OK) return rc;
    }

    /* Transparency first: un-matting reads it back from the target. */
    for (k = MINIPSD_CH_A; k >= 0; --k) {
        const minipsd_plane *pl = &target->channel[k];
        if (!pl->data) continue;
        if (k == MINIPSD_CH_A && !r->composite_alpha) {          /* no transparency: opaque */
            mpsd__opaque(pl, r->width, rows, r->wide);
            continue;
        }
        rc = mpsd__chan_rows(&ch[k], r->width, y0, rows, pl);
        if (rc != MINIPSD_OK) return rc;
        if (k == MINIPSD_CH_A || !unmatte) continue;
        for (y = 0; y < rows; ++y) {
            const minipsd_plane *a = &target->channel[MINIPSD_CH_A];
            for (x = 0; x < r->width; ++x) {
                uint8_t       *s     = pl->data + (ptrdiff_t)y * pl->row_step + (ptrdiff_t)x * pl->pixel_step;
                const uint8_t *alpha = a->data + (ptrdiff_t)y * a->row_step + (ptrdiff_t)x * a->pixel_step;
                mpsd__set_sample(s, r->wide, mpsd__unmatte(mpsd__sample(s, r->wide), mpsd__sample(alpha, r->wide), r->wide ? 65535u : 255u));
            }
        }
    }
    return MINIPSD_OK;
}

#endif /* MPSD__READER_DONE */
