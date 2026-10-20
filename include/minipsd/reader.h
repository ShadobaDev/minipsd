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
#include "detail/format.h"
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
    unsigned       psb;           /* PSB: lengths of eight bytes, row counts of four             */
    uint32_t       width, height; /* the composite: its size, channels, and where it starts      */
    uint32_t       channels;      /* the composite: how many channels, 1..MPSD__MAX_CHANNELS     */
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

/* --- reading from a buffer that may end early --- */

/* Does the buffer reach `end`?  If not, remember how far it must, so the caller can read more. */
static int mpsd__have(minipsd_reader *r, uint64_t end)
{
    if (end <= r->size) return MINIPSD_OK;
    r->needed = end;
    return MINIPSD_ERR_SHORT;
}

/* A length field at `at`: four bytes, or eight when wide. */
static int mpsd__length(minipsd_reader *r, uint64_t at, unsigned wide, uint64_t *length)
{
    const int rc = mpsd__have(r, at + mpsd__len_size(wide));
    if (rc == MINIPSD_OK) *length = mpsd__getlen(r->data + (size_t)at, wide);
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

/* --- image resources: '8BIM', a number, a name, a length, the data (detail/format.h) --- */

/* The resource at `at`, which lies in [at, res_end); *next is where the one after it starts.
   Padding of the name and of the data to even sizes is part of the block. */
static int mpsd__resource(const minipsd_reader *r, uint64_t at, minipsd_resource *res, uint64_t *next)
{
    const uint8_t *d = r->data;
    uint64_t       pos, size;
    if (r->res_end - at < mpsd__resource_size(0, 0) || !mpsd__tag(d + (size_t)at, "8BIM")) return MINIPSD_ERR_FORMAT;
    pos = at + MPSD__RES_NAME + mpsd__pad(1u + d[(size_t)at + MPSD__RES_NAME], 2);   /* after the name */
    if (pos > r->res_end || r->res_end - pos < MPSD__LEN) return MINIPSD_ERR_FORMAT;
    size = mpsd__get32(d + (size_t)pos);
    pos += MPSD__LEN;
    if (size + (size & 1) > r->res_end - pos) return MINIPSD_ERR_FORMAT;
    res->id        = (uint16_t)mpsd__get16(d + (size_t)at + 4);
    res->name.data = d + (size_t)at + MPSD__RES_NAME + 1;
    res->name.size = d[(size_t)at + MPSD__RES_NAME];
    res->data.data = d + (size_t)pos;
    res->data.size = (size_t)size;
    *next          = pos + size + (size & 1);
    return MINIPSD_OK;
}

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
    const int rc = mpsd__have(r, at + MPSD__LAYER_COUNT);
    if (rc != MINIPSD_OK) return rc;
    count = (int16_t)(uint16_t)mpsd__get16(r->data + (size_t)at);
    in->composite_has_alpha = count < 0;
    if (count < 0) count = -count;
    if (count > MPSD__MAX_LAYERS) return MINIPSD_ERR_FORMAT;
    if ((uint32_t)count > r->limits.max_layers) return MINIPSD_ERR_LIMIT;
    in->layer_count = (size_t)count;
    r->li_start     = at + MPSD__LAYER_COUNT;
    r->li_end       = end;
    return MINIPSD_OK;
}

/* Photoshop keeps the layers of a 16- or 32-bit file in a global block, 'Lr16' or 'Lr32', laid out
   as the layer info, and leaves the layer info empty.  Looks for one in the tail [at, end): the
   global layer mask, then blocks, each padded to four bytes past its length ('Lr16' 1218 long
   takes 1220), a long key's length eight bytes in a PSB.  A tail that is not blocks is only kept,
   so it is no error. */
static int mpsd__deep_layers(minipsd_reader *r, uint64_t at, uint64_t end, minipsd_info *in)
{
    const uint8_t *p;
    uint64_t       length;
    size_t         head;
    int            rc;
    if (end - at < MPSD__LEN) return MINIPSD_OK;
    rc = mpsd__length(r, at, 0, &length);
    if (rc != MINIPSD_OK) return rc;
    for (at += MPSD__LEN + length; at < end && end - at >= MPSD__BLOCK_HEAD; at += head + mpsd__pad((size_t)length, 4)) {
        rc = mpsd__have(r, at + MPSD__BLOCK_HEAD);
        if (rc != MINIPSD_OK) return rc;
        p = r->data + (size_t)at;
        if (!(mpsd__tag(p, "8BIM") || mpsd__tag(p, "8B64"))) return MINIPSD_OK;
        head = mpsd__block_head(p, r->psb);
        if (end - at < head) return MINIPSD_OK;
        rc = mpsd__length(r, at + MPSD__BLOCK_LEN, head > MPSD__BLOCK_HEAD, &length);
        if (rc != MINIPSD_OK) return rc;
        if (length > end - at - head) return MINIPSD_OK;
        if (!mpsd__tag(p + MPSD__BLOCK_KEY, "Lr16") && !mpsd__tag(p + MPSD__BLOCK_KEY, "Lr32")) continue;
        if (length == 0) return MINIPSD_OK;
        return length == 1 ? MINIPSD_ERR_FORMAT : mpsd__layers_at(r, at + head, at + head + length, in);
    }
    return MINIPSD_OK;
}

/* Reads everything init promises, in file order; each step asks for the bytes it needs. */
static int mpsd__scan(minipsd_reader *r, minipsd_info *info)
{
    const uint8_t *d = r->data;
    minipsd_info   in;
    uint64_t       at, lm_start, length;
    uint32_t       side;
    unsigned       psb;
    int            rc, has_dpi = 0, has_icc = 0;

    mpsd__info_clear(&in);
    /* The header: a stream that is not a PSD is refused as soon as four bytes show it. */
    if (!mpsd__starts(r, "8BPS")) return MINIPSD_ERR_FORMAT;
    rc = mpsd__have(r, MPSD__HEADER_SIZE);                     /* its fields: detail/format.h */
    if (rc != MINIPSD_OK) return rc;
    in.format     = (uint16_t)mpsd__get16(d + 4);
    in.channels   = (uint16_t)mpsd__get16(d + 12);
    in.height     = mpsd__get32(d + 14);
    in.width      = mpsd__get32(d + 18);
    in.depth      = (uint16_t)mpsd__get16(d + 22);
    in.color_mode = (uint16_t)mpsd__get16(d + 24);
    psb           = in.format == MINIPSD_PSB;
    side          = mpsd__max_side(psb);
    if ((in.format != MINIPSD_PSD && !psb) || in.channels < 1 || in.channels > MPSD__MAX_CHANNELS || !mpsd__depth_ok(in.depth)
        || !mpsd__mode_ok(in.color_mode) || in.width < 1 || in.height < 1 || in.width > side || in.height > side)
        return MINIPSD_ERR_FORMAT;
    if (r->limits.max_side) side = r->limits.max_side;
    if (in.width > side || in.height > side) return MINIPSD_ERR_LIMIT;
    r->psb = psb;

    /* The colour mode data is not read: skip it. */
    rc = mpsd__length(r, MPSD__HEADER_SIZE, 0, &length);
    if (rc != MINIPSD_OK) return rc;
    at = MPSD__HEADER_SIZE + MPSD__LEN + length;

    /* The image resources, all of them: the resolution and the ICC profile are among them. */
    rc = mpsd__length(r, at, 0, &length);
    if (rc != MINIPSD_OK) return rc;
    r->res_next = at + MPSD__LEN;
    r->res_end  = r->res_next + length;
    rc = mpsd__have(r, r->res_end);
    if (rc != MINIPSD_OK) return rc;
    for (at = r->res_next; at < r->res_end;) {                 /* checks every block as well */
        minipsd_resource res;
        rc = mpsd__resource(r, at, &res, &at);
        if (rc != MINIPSD_OK) return rc;
        if (res.id == MPSD__RES_RESOLUTION && !has_dpi) {
            if (res.data.size < MPSD__RESOLUTION_SIZE) return MINIPSD_ERR_FORMAT;
            in.dpi  = (double)mpsd__get32(res.data.data) / MPSD__FIXED_ONE;
            has_dpi = 1;
        } else if (res.id == MPSD__RES_ICC && !has_icc) {
            in.icc  = res.data;
            has_icc = 1;
        }
    }

    /* The layer and mask information: its length, then the layer info's length and layer count;
       both lengths are eight bytes in a PSB. */
    rc = mpsd__length(r, r->res_end, psb, &length);
    if (rc != MINIPSD_OK) return rc;
    if (length >= MPSD__FAR) return MINIPSD_ERR_FORMAT;
    if ((size_t)length != length) return MINIPSD_ERR_UNSUPPORTED;   /* views of it could not say their size */
    lm_start = r->res_end + mpsd__len_size(psb);
    if (length != 0) {
        uint64_t       info_length, tail;
        if (length < mpsd__len_size(psb)) return MINIPSD_ERR_FORMAT;
        rc = mpsd__length(r, lm_start, psb, &info_length);
        if (rc != MINIPSD_OK) return rc;
        if (info_length > length - mpsd__len_size(psb) || info_length == 1) return MINIPSD_ERR_FORMAT;
        tail = lm_start + mpsd__len_size(psb) + info_length;   /* the global layer mask, the global blocks */
        in.layer_tail.size = (size_t)(lm_start + length - tail);
        if (lm_start + length <= r->size) in.layer_tail.data = d + (size_t)tail;
        rc = info_length ? mpsd__layers_at(r, lm_start + mpsd__len_size(psb), tail, &in) : MINIPSD_OK;
        if (rc == MINIPSD_OK && !in.layer_count) rc = mpsd__deep_layers(r, tail, lm_start + length, &in);
        if (rc != MINIPSD_OK) return rc;
    }
    r->layer_count = in.layer_count;
    r->record_next = r->li_start;
    r->rgb         = (in.depth == 8 || in.depth == 16) && in.color_mode == MPSD__MODE_RGB;
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
    r->limits.max_side   = limits ? limits->max_side : 0;   /* 0: the format's own, known from the header */
    r->limits.max_layers = limits && limits->max_layers ? limits->max_layers : MPSD__MAX_LAYERS;
    r->limits.max_decoded_bytes = limits ? limits->max_decoded_bytes : 0;
    r->layer_side        = limits ? limits->max_side : 0;
    r->ready        = 0;
    r->rgb          = 0;
    r->wide         = 0;
    r->psb          = 0;
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
    uint64_t           offset[MPSD__CHANNELS];   /* in the layer's channel data, by slot (detail/format.h) */
    uint64_t           length[MPSD__CHANNELS];   /* 0: the layer has no such channel           */
    uint64_t           total;       /* all of its channel data, other channels too  */
    uint64_t           next;        /* where the next record starts                 */
} mpsd__rec;

/* A range of the layer info: past li_end it is malformed, past the buffer only the buffer is short. */
static int mpsd__room(minipsd_reader *r, uint64_t end)
{
    return end > r->li_end ? MINIPSD_ERR_FORMAT : mpsd__have(r, end);
}

/* Where a PSD channel id goes: its slot, or -1 for a channel the reader does not read. */
static int mpsd__slot(int id)
{
    return id >= 0 && id <= 2 ? id : id == -1 ? MINIPSD_CH_A : id == -2 ? MPSD__CH_MASK : id == -3 ? MPSD__CH_REAL : -1;
}

/* The name block 'luni': a count of UTF-16 units, then the units. */
static int mpsd__luni(minipsd_bytes data, minipsd_bytes *name)
{
    size_t units;
    if (data.size < MPSD__LEN) return MINIPSD_ERR_FORMAT;
    units = mpsd__get32(data.data);
    if (units > (data.size - MPSD__LEN) / 2) return MINIPSD_ERR_FORMAT;
    name->data = data.data + MPSD__LEN;
    name->size = units * 2;
    return MINIPSD_OK;
}

/* The extra data of a record, [at, end): the mask data, the blending ranges (whole ones only),
   the legacy name, then the blocks, which are walked here to check them and to find 'luni'. */
static int mpsd__extra(const minipsd_reader *r, uint64_t at, uint64_t end, minipsd_layer_view *v)
{
    const uint8_t *d = r->data;
    minipsd_block  block;
    size_t         pos;
    uint32_t       skip;
    int            k, rc;
    for (k = 0; k < 2; ++k) {                                 /* layer mask data, blending ranges */
        if (end - at < MPSD__LEN) return MINIPSD_ERR_FORMAT;
        skip = mpsd__get32(d + (size_t)at);
        at += MPSD__LEN;
        if (skip > end - at) return MINIPSD_ERR_FORMAT;
        if (k == 0 && skip >= MPSD__MASK_HEAD) {               /* rectangle, colour, flags, the rest */
            const uint8_t *m = d + (size_t)at;
            minipsd_mask  *mask = &v->mask;
            mask->top    = (int32_t)mpsd__get32(m);     mask->left  = (int32_t)mpsd__get32(m + 4);
            mask->bottom = (int32_t)mpsd__get32(m + 8); mask->right = (int32_t)mpsd__get32(m + 12);
            if (mask->bottom < mask->top) mask->bottom = mask->top;   /* inverted: empty, as layers */
            if (mask->right < mask->left) mask->right = mask->left;
            mask->color     = m[MPSD__RECT_SIZE];
            mask->flags     = m[MPSD__RECT_SIZE + 1];
            mask->rest.data = m + MPSD__MASK_HEAD;
            mask->rest.size = skip - MPSD__MASK_HEAD;
            v->has_mask     = 1;
        }
        if (k == 1) {
            if (skip % MPSD__RANGE_SIZE) return MINIPSD_ERR_FORMAT;
            if (skip) v->blending_ranges.data = d + (size_t)at;
            v->blending_ranges.size = skip;
            v->blending_range_count = skip / MPSD__RANGE_SIZE;
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
        rc = mpsd__block(v->extra, pos, r->psb, &block, &pos);
        if (rc != MINIPSD_OK) return rc;
        if (mpsd__tag((const uint8_t *)block.key, "luni") && !v->name_unicode.data) {
            rc = mpsd__luni(block.data, &v->name_unicode);
            if (rc != MINIPSD_OK) return rc;
        }
        if (mpsd__tag((const uint8_t *)block.key, "lsct") && block.data.size >= MPSD__LSCT_TYPE && !v->kind) {
            const uint32_t type = mpsd__get32(block.data.data);
            v->kind = (uint8_t)(type <= (uint32_t)MINIPSD_KIND_GROUP_END ? type : 0u);   /* unknown: a layer */
        }
    }
    return MINIPSD_OK;
}

/* The record at `at`, which must lie in the layer info; laid out as detail/format.h shows. */
static int mpsd__record(minipsd_reader *r, uint64_t at, mpsd__rec *rec)
{
    const uint8_t      *d = r->data;
    minipsd_layer_view *v = &rec->view;
    uint64_t            pos, extra_end;
    const unsigned      entry = mpsd__entry_size(r->psb);    /* a channel: its id, its length */
    uint32_t            channels, k, extra;
    int                 rc;
    int32_t             rect[4];

    rc = mpsd__room(r, at + MPSD__RECORD_HEAD);                /* the rectangle and the channel count */
    if (rc != MINIPSD_OK) return rc;
    channels = mpsd__get16(d + (size_t)at + MPSD__RECT_SIZE);
    pos      = at + MPSD__RECORD_HEAD + (uint64_t)entry * channels;   /* after the channel table */
    rc = mpsd__room(r, pos + MPSD__RECORD_TAIL);               /* blend, flags, extra length */
    if (rc != MINIPSD_OK) return rc;

    for (k = 0; k < 4; ++k) {
        rect[k]        = (int32_t)mpsd__get32(d + (size_t)at + 4 * (size_t)k);
        v->channel[k].data = NULL;
        v->channel[k].size = 0;
    }
    for (k = 0; k < MPSD__CHANNELS; ++k) rec->offset[k] = rec->length[k] = 0;
    memset(&v->mask, 0, sizeof v->mask);
    v->kind = v->has_mask = 0;
    v->mask_offset = 0;
    v->format      = (uint8_t)(r->psb ? MINIPSD_PSB : MINIPSD_PSD);
    /* An inverted rectangle is an empty one: Photoshop writes (0, 0, -1, 0) for a fill layer
       that has no pixels of its own. */
    if (rect[2] < rect[0]) rect[2] = rect[0];
    if (rect[3] < rect[1]) rect[3] = rect[1];
    v->top = rect[0]; v->left = rect[1]; v->bottom = rect[2]; v->right = rect[3];
    v->other_channels = 0;
    rec->total        = 0;
    for (k = 0; k < channels; ++k) {
        const uint8_t *e      = d + (size_t)at + MPSD__RECORD_HEAD + (size_t)entry * k;
        const uint64_t length = mpsd__getlen(e + 2, r->psb);   /* after the id */
        const int      slot   = mpsd__slot((int16_t)(uint16_t)mpsd__get16(e));
        /* A marker, no repeats, and no more data than the layer info could hold. */
        if (length < MPSD__MARKER_SIZE || (slot >= 0 && rec->length[slot]) || length > r->li_end - rec->total) return MINIPSD_ERR_FORMAT;
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
    v->hidden   = (v->flags & MPSD__FLAG_HIDDEN) != 0;
    extra       = mpsd__get32(d + (size_t)pos + 12);
    extra_end   = pos + MPSD__RECORD_TAIL + extra;
    rc = mpsd__room(r, extra_end);
    if (rc != MINIPSD_OK) return rc;
    v->name_pascal.data = v->name_unicode.data = v->extra.data = NULL;
    v->name_pascal.size = v->name_unicode.size = v->extra.size = 0;
    v->blending_ranges.data = NULL;
    v->blending_ranges.size = 0;
    v->blending_range_count = 0;
    rc = mpsd__extra(r, pos + MPSD__RECORD_TAIL, extra_end, v);
    rec->next = extra_end;
    if (!v->has_mask) {                                        /* mask channels with no mask data */
        v->other_channels = (uint16_t)(v->other_channels + (rec->length[MPSD__CH_MASK] != 0) + (rec->length[MPSD__CH_REAL] != 0));
        rec->length[MPSD__CH_MASK] = rec->length[MPSD__CH_REAL] = 0;
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
    for (k = 0; k < MPSD__CHANNELS; ++k) {
        const uint64_t at   = r->data_next + rec.offset[k];
        minipsd_bytes *view = k < MPSD__RGBA ? &rec.view.channel[k] : k == MPSD__CH_MASK ? &rec.view.mask.stored : &rec.view.mask.real;
        view->size = (size_t)rec.length[k];                     /* within the section, which fits */
        view->data = rec.length[k] && at + rec.length[k] <= r->size ? r->data + (size_t)at : NULL;
        if (k < MPSD__RGBA) rec.view.channel_offset[k] = rec.length[k] ? at : 0;
        if (k == MPSD__CH_MASK) rec.view.mask_offset = rec.length[k] ? at : 0;
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

MINIPSDDEF int minipsd_layer_blending_range(const minipsd_layer_view *layer, uint32_t i, minipsd_blending_range *range)
{
    const uint8_t *b;
    if (!layer || !range || i >= layer->blending_range_count || !layer->blending_ranges.data
        || ((uint64_t)i + 1) * MPSD__RANGE_SIZE > layer->blending_ranges.size)   /* a view built by hand */
        return MINIPSD_ERR_ARG;
    b = layer->blending_ranges.data + (size_t)i * MPSD__RANGE_SIZE;
    memcpy(range->source.black,      b,     2);
    memcpy(range->source.white,      b + 2, 2);
    memcpy(range->destination.black, b + 4, 2);
    memcpy(range->destination.white, b + 6, 2);
    return MINIPSD_OK;
}

MINIPSDDEF int minipsd_layer_next_block(const minipsd_layer_view *layer, size_t *pos, minipsd_block *block)
{
    size_t next;
    int    rc;
    if (!layer || !pos || !block) return MINIPSD_ERR_ARG;
    if (*pos >= layer->extra.size) return MINIPSD_OK;
    rc = mpsd__block(layer->extra, *pos, layer->format == MINIPSD_PSB, block, &next);
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
    for (k = 0; k < MPSD__RGBA; ++k) planes += t->channel[k].data != NULL;
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
    for (k = 0; k < MPSD__RGBA; ++k) {
        t->channel[k].data       = rgba ? rgba + ((size_t)k << wide) : NULL;
        t->channel[k].pixel_step = (ptrdiff_t)MPSD__RGBA << wide;
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
   then raw rows, or (RLE) a byte count per row (two bytes, four in a PSB) and the packed rows, or
   (ZIP) a zlib stream of the raw rows, inflated by the caller when its turn comes.  A row is width
   samples of the file's depth, big-endian. */
typedef struct mpsd__chan {
    const uint8_t *table;   /* RLE: the row byte counts; NULL for raw   */
    const uint8_t *rows;    /* the first row's data                     */
    const uint8_t *stream;  /* ZIP: the zlib stream; NULL once inflated */
    size_t         stream_size;
    unsigned       predicted; /* ZIP: each sample stored as its difference from the left one */
    unsigned       wide;    /* two bytes a sample                       */
    unsigned       psb;     /* row counts of four bytes                 */
} mpsd__chan;

static int mpsd__chan_open(const minipsd_reader *r, minipsd_bytes c, uint32_t width, uint32_t height,
                           const minipsd_decode_opts *opts, mpsd__chan *ch)
{
    const uint64_t counts = (uint64_t)height * mpsd__count_size(r->psb);   /* the table's bytes */
    uint64_t       left, sum = 0;
    uint32_t       y;
    left     = c.size - MPSD__MARKER_SIZE;                     /* next_layer saw the marker */
    ch->wide = r->wide;
    ch->psb  = r->psb;
    ch->stream = NULL;
    switch (mpsd__get16(c.data)) {
    case MPSD__COMP_RAW:
        if (((uint64_t)width * height << ch->wide) > left) return MINIPSD_ERR_FORMAT;
        ch->table = NULL;
        ch->rows  = c.data + MPSD__MARKER_SIZE;
        return MINIPSD_OK;
    case MPSD__COMP_RLE:                                       /* PackBits */
        if (counts > left) return MINIPSD_ERR_FORMAT;
        ch->table = c.data + MPSD__MARKER_SIZE;
        ch->rows  = ch->table + (size_t)counts;
        for (y = 0; y < height; ++y) sum += mpsd__count(ch->table, y, ch->psb);
        return sum > left - counts ? MINIPSD_ERR_FORMAT : MINIPSD_OK;
    case MPSD__COMP_ZIP: case MPSD__COMP_ZIP_PREDICTED:
        if (!opts || !opts->inflate) return MINIPSD_ERR_UNSUPPORTED;
        if (!opts->scratch || opts->scratch_size < ((uint64_t)width * height << ch->wide)) return MINIPSD_ERR_ARG;
        ch->table       = NULL;
        ch->stream      = c.data + MPSD__MARKER_SIZE;
        ch->stream_size = (size_t)left;
        ch->predicted   = mpsd__get16(c.data) == MPSD__COMP_ZIP_PREDICTED;
        return MINIPSD_OK;
    default:
        return MINIPSD_ERR_FORMAT;
    }
}

/* A ZIP channel inflated whole into opts->scratch, its prediction undone (byte sums at 8 bits,
   big-endian sample sums at 16), so its rows are raw ones from there on. */
static int mpsd__chan_inflate(mpsd__chan *ch, uint32_t width, uint32_t height, const minipsd_decode_opts *opts)
{
    const size_t size = (size_t)width * height << ch->wide;   /* chan_open checked the scratch holds it */
    uint8_t     *row;
    uint32_t     x, y;
    if (!ch->stream) return MINIPSD_OK;                        /* not ZIP: opts may be NULL */
    row = opts->scratch;
    if (opts->inflate(opts->inflate_user, ch->stream, ch->stream_size, opts->scratch, size)) return MINIPSD_ERR_FORMAT;
    for (y = 0; ch->predicted && y < height; ++y, row += (size_t)width << ch->wide)
        for (x = 1; x < width; ++x) {
            uint8_t *s = row + ((size_t)x << ch->wide);
            if (ch->wide) {                                    /* big-endian, as the raw rows are */
                const uint32_t v = mpsd__get16(s) + mpsd__get16(s - 2);
                s[0] = (uint8_t)(v >> 8);
                s[1] = (uint8_t)v;
            } else {
                s[0] = (uint8_t)(s[0] + s[-1]);
            }
        }
    ch->rows   = opts->scratch;
    ch->stream = NULL;
    return MINIPSD_OK;
}

/* Rows [y0, y0 + rows) of a checked channel into a plane. */
static int mpsd__chan_rows(const mpsd__chan *ch, uint32_t width, uint32_t y0, uint32_t rows, const minipsd_plane *pl)
{
    const uint8_t *src   = ch->rows;
    const uint32_t bytes = width << ch->wide;                  /* a row as stored; a side fits 300000 */
    uint32_t       y, x;
    if (!ch->table) {
        src += (size_t)y0 * bytes;
        for (y = 0; y < rows; ++y, src += bytes) {
            uint8_t *out = pl->data + (ptrdiff_t)y * pl->row_step;
            for (x = 0; x < bytes; ++x) out[mpsd__at(x, pl->pixel_step, ch->wide)] = src[x];
        }
        return MINIPSD_OK;
    }
    for (y = 0; y < y0; ++y) src += mpsd__count(ch->table, y, ch->psb);   /* to the band */
    for (y = 0; y < rows; ++y) {
        const size_t n = mpsd__count(ch->table, y0 + y, ch->psb);
        if (!mpsd__unpackbits(src, n, pl->data + (ptrdiff_t)y * pl->row_step, pl->pixel_step, bytes, ch->wide))
            return MINIPSD_ERR_FORMAT;
        src += n;
    }
    return MINIPSD_OK;
}

MINIPSDDEF int minipsd_decode_layer(minipsd_reader *r, const minipsd_layer_view *layer, const minipsd_target *target,
                                    const minipsd_decode_opts *opts)
{
    mpsd__chan ch[MPSD__RGBA];
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
    for (k = 0; k < MPSD__RGBA; ++k) {
        const minipsd_bytes c = layer->channel[k];
        if (!target->channel[k].data || (k == MINIPSD_CH_A && c.size == 0)) continue;
        if (c.size == 0) return MINIPSD_ERR_FORMAT;            /* a colour channel is missing */
        if (!c.data && layer->channel_offset[k] + c.size > short_end) short_end = layer->channel_offset[k] + c.size;
    }
    if (short_end) {
        r->needed = short_end;
        return MINIPSD_ERR_SHORT;
    }
    for (k = 0; k < MPSD__RGBA; ++k) {
        if (!target->channel[k].data || layer->channel[k].size == 0) continue;
        rc = mpsd__chan_open(r, layer->channel[k], width, height, opts, &ch[k]);
        if (rc != MINIPSD_OK) return rc;
    }

    for (k = 0; k < MPSD__RGBA; ++k) {
        const minipsd_plane *pl = &target->channel[k];
        if (!pl->data) continue;
        if (layer->channel[k].size == 0) {                     /* no transparency: opaque */
            mpsd__opaque(pl, width, rows, r->wide);
            continue;
        }
        rc = mpsd__chan_inflate(&ch[k], width, height, opts);
        if (rc == MINIPSD_OK) rc = mpsd__chan_rows(&ch[k], width, y0, rows, pl);
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
    for (k = 0; k < MPSD__RGBA; ++k) t.channel[k].data = NULL;
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
    rc = mpsd__chan_open(r, c, width, height, opts, &ch);
    if (rc == MINIPSD_OK) rc = mpsd__chan_inflate(&ch, width, height, opts);
    return rc != MINIPSD_OK ? rc : mpsd__chan_rows(&ch, width, y0, rows, plane);
}

/* --- pixels: the composite, every channel's rows after one compression marker --- */

/* Where the composite's channels 0 .. last are, checked whole in the buffer before a sample is
   written: raw planes back to back, or (RLE) every channel's row counts, then every row. */
static int mpsd__composite_open(minipsd_reader *r, int last, mpsd__chan *ch)
{
    const uint8_t *d = r->data;
    const uint64_t at = r->image_start + MPSD__MARKER_SIZE, plane = (uint64_t)r->width * r->height << r->wide;
    uint64_t       sum = 0, offset[MPSD__RGBA], rows_at;
    uint32_t       y, n;
    int            k, rc = mpsd__have(r, at);
    if (rc != MINIPSD_OK) return rc;
    switch (mpsd__get16(d + (size_t)r->image_start)) {
    case MPSD__COMP_RAW:
        rc = mpsd__have(r, at + plane * (uint64_t)(last + 1));
        for (k = 0; rc == MINIPSD_OK && k <= last; ++k) {
            ch[k].table = NULL;
            ch[k].rows  = d + (size_t)(at + plane * (uint64_t)k);
            ch[k].wide  = r->wide;
            ch[k].psb   = r->psb;
        }
        return rc;
    case MPSD__COMP_RLE:
        rows_at = at + (uint64_t)r->channels * r->height * mpsd__count_size(r->psb);   /* the counts of every channel come first */
        rc = mpsd__have(r, rows_at);
        if (rc != MINIPSD_OK) return rc;
        for (k = 0; k <= last; ++k) {
            offset[k] = sum;
            for (y = 0, n = r->height; y < n; ++y) sum += mpsd__count(d + (size_t)at, (uint32_t)k * n + y, r->psb);
        }
        rc = mpsd__have(r, rows_at + sum);
        for (k = 0; rc == MINIPSD_OK && k <= last; ++k) {
            ch[k].table = d + (size_t)at + (size_t)k * r->height * mpsd__count_size(r->psb);
            ch[k].rows  = d + (size_t)(rows_at + offset[k]);
            ch[k].wide  = r->wide;
            ch[k].psb   = r->psb;
        }
        return rc;
    case MPSD__COMP_ZIP: case MPSD__COMP_ZIP_PREDICTED:
        return MINIPSD_ERR_UNSUPPORTED;
    default:
        return MINIPSD_ERR_FORMAT;
    }
}

MINIPSDDEF int minipsd_decode_composite(minipsd_reader *r, const minipsd_target *target, const minipsd_decode_opts *opts)
{
    mpsd__chan ch[MPSD__RGBA];
    uint32_t   y0, rows, y, x;
    int        k, rc, last = -1, unmatte, colour, wide = mpsd__opts_wide(opts);
    if (!r || !target || !r->ready || wide < 0) return MINIPSD_ERR_ARG;
    y0 = opts ? opts->y_first : 0;
    if (y0 > r->height) return MINIPSD_ERR_ARG;
    rows = opts && opts->y_count ? opts->y_count : r->height - y0;
    if (rows > r->height - y0) return MINIPSD_ERR_ARG;
    for (k = 0; k < MINIPSD_CH_A; ++k)
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
