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
#include "detail/samples.h"
#include "detail/utf.h"
#include "detail/packbits.h"
#include "detail/composite.h"

/* --- the reader's state --- */

struct minipsd_reader {
    const uint8_t *data;          /* the window: the file's bytes [base, base + size)            */
    size_t         size;
    uint64_t       base;
    uint64_t       needed_from;   /* what the last MINIPSD_ERR_SHORT asked for, [from, needed)    */
    uint64_t       needed;
    minipsd_limits limits;        /* with zero fields replaced by the format's own limits        */
    uint32_t       layer_side;    /* the caller's max_side, for layers; 0: none                  */
    int            ready;         /* init succeeded                                              */
    int            rgb;           /* RGB: the composite is un-matted                             */
    unsigned       depth;         /* bits a sample: 1, 8, 16 or 32                               */
    unsigned       shift;         /* a sample in memory takes 1 << shift bytes (detail/samples.h) */
    unsigned       colors;        /* the mode's colour channels; transparency's slot comes next  */
    unsigned       psb;           /* PSB: lengths of eight bytes, row counts of four             */
    uint32_t       width, height; /* the composite: its size, channels, and where it starts      */
    uint32_t       channels;      /* the composite: how many channels, 1..MPSD__MAX_CHANNELS     */
    int            composite_alpha; /* the composite: non-zero if it has an alpha channel        */
    uint64_t       image_start;   /* the composite: where its channel data starts                */
    uint64_t       res_start;     /* the resources: where the first one starts, where they end    */
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

/* Does the window hold the file's bytes [from, to)? */
static int mpsd__reader_holds(const minipsd_reader *r, uint64_t from, uint64_t to)
{
    return from >= r->base && to - r->base <= r->size;
}

/* The byte at file offset `at`, which the window holds. */
static const uint8_t *mpsd__reader_ptr(const minipsd_reader *r, uint64_t at)
{
    return r->data + (size_t)(at - r->base);
}

/* Does the window hold [from, to)?  If not, remember the range, so the caller can read it: from is
   where the call that asks starts reading when repeated. */
static int mpsd__have(minipsd_reader *r, uint64_t from, uint64_t to)
{
    if (mpsd__reader_holds(r, from, to)) return MINIPSD_OK;
    r->needed_from = from;
    r->needed      = to;
    return MINIPSD_ERR_SHORT;
}

/* A length field at `at`: four bytes, or eight when wide.  Init's: it starts again from the
   header. */
static int mpsd__length(minipsd_reader *r, uint64_t at, unsigned wide, uint64_t *length)
{
    const int rc = mpsd__have(r, 0, at + mpsd__len_size(wide));
    if (rc == MINIPSD_OK) *length = mpsd__getlen(mpsd__reader_ptr(r, at), wide);
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
    uint64_t       pos, size;
    if (r->res_end - at < mpsd__resource_size(0, 0) || !mpsd__tag(mpsd__reader_ptr(r, at), "8BIM")) return MINIPSD_ERR_FORMAT;
    pos = at + MPSD__RES_NAME + mpsd__pad(1u + mpsd__reader_ptr(r, at)[MPSD__RES_NAME], 2);   /* after the name */
    if (pos > r->res_end || r->res_end - pos < MPSD__LEN) return MINIPSD_ERR_FORMAT;
    size = mpsd__get32(mpsd__reader_ptr(r, pos));
    pos += MPSD__LEN;
    if (size + (size & 1) > r->res_end - pos) return MINIPSD_ERR_FORMAT;
    res->id        = (uint16_t)mpsd__get16(mpsd__reader_ptr(r, at) + 4);
    res->name.data = mpsd__reader_ptr(r, at) + MPSD__RES_NAME + 1;
    res->name.size = mpsd__reader_ptr(r, at)[MPSD__RES_NAME];
    res->data.data = mpsd__reader_ptr(r, pos);
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
    i->format = i->color_channels = 0;
    i->color_mode_data.data = NULL;
    i->color_mode_data.size = 0;
}

/* Colour modes: bitmap, grey, indexed, RGB, CMYK, multichannel, duotone, Lab. */
static int mpsd__mode_ok(uint32_t mode) { return mode <= MINIPSD_MODE_CMYK || (mode >= MINIPSD_MODE_MULTICHANNEL && mode <= MINIPSD_MODE_LAB); }

/* The layer count at `at`, then the records and their channel data up to `end`: the layer info. */
static int mpsd__layers_at(minipsd_reader *r, uint64_t at, uint64_t end, minipsd_info *in)
{
    int32_t count;
    const int rc = mpsd__have(r, 0, at + MPSD__LAYER_COUNT);
    if (rc != MINIPSD_OK) return rc;
    count = (int16_t)(uint16_t)mpsd__get16(mpsd__reader_ptr(r, at));
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
   so it is no error.  Their layer count is positive: a block 'Mtrn', 'Mt16' or 'Mt32' before the
   layers' says instead that the composite's first extra channel is transparency. */
static int mpsd__deep_layers(minipsd_reader *r, uint64_t at, uint64_t end, minipsd_info *in)
{
    const uint8_t *p;
    uint64_t       length;
    size_t         head;
    int            rc, merged = 0;
    if (end - at < MPSD__LEN) return MINIPSD_OK;
    rc = mpsd__length(r, at, 0, &length);
    if (rc != MINIPSD_OK) return rc;
    for (at += MPSD__LEN + length; at < end && end - at >= MPSD__BLOCK_HEAD; at += head + mpsd__pad((size_t)length, 4)) {
        rc = mpsd__have(r, 0, at + MPSD__BLOCK_HEAD);
        if (rc != MINIPSD_OK) return rc;
        p = mpsd__reader_ptr(r, at);
        if (!(mpsd__tag(p, "8BIM") || mpsd__tag(p, "8B64"))) return MINIPSD_OK;
        head = mpsd__block_head(p, r->psb);
        if (end - at < head) return MINIPSD_OK;
        rc = mpsd__length(r, at + MPSD__BLOCK_LEN, head > MPSD__BLOCK_HEAD, &length);
        if (rc != MINIPSD_OK) return rc;
        if (length > end - at - head) return MINIPSD_OK;
        if (mpsd__tag(p + MPSD__BLOCK_KEY, "Mtrn") || mpsd__tag(p + MPSD__BLOCK_KEY, "Mt16") || mpsd__tag(p + MPSD__BLOCK_KEY, "Mt32"))
            in->composite_has_alpha = merged = 1;
        if (!mpsd__tag(p + MPSD__BLOCK_KEY, "Lr16") && !mpsd__tag(p + MPSD__BLOCK_KEY, "Lr32")) continue;
        if (length == 0) return MINIPSD_OK;
        if (length == 1) return MINIPSD_ERR_FORMAT;
        rc = mpsd__layers_at(r, at + head, at + head + length, in);
        in->composite_has_alpha |= merged;
        return rc;
    }
    return MINIPSD_OK;
}

/* Reads everything init promises, in file order; each step asks for the bytes it needs. */
static int mpsd__scan(minipsd_reader *r, minipsd_info *info)
{
    minipsd_info   in;
    uint64_t       at, lm_start, length;
    uint32_t       side;
    unsigned       psb;
    int            rc, has_dpi = 0, has_icc = 0;

    mpsd__info_clear(&in);
    /* The header: a stream that is not a PSD is refused as soon as four bytes show it. */
    if (!mpsd__starts(r, "8BPS")) return MINIPSD_ERR_FORMAT;
    rc = mpsd__have(r, 0, MPSD__HEADER_SIZE);                    /* its fields: detail/format.h */
    if (rc != MINIPSD_OK) return rc;
    in.format     = (uint16_t)mpsd__get16(mpsd__reader_ptr(r, 4));
    in.channels   = (uint16_t)mpsd__get16(mpsd__reader_ptr(r, 12));
    in.height     = mpsd__get32(mpsd__reader_ptr(r, 14));
    in.width      = mpsd__get32(mpsd__reader_ptr(r, 18));
    in.depth      = (uint16_t)mpsd__get16(mpsd__reader_ptr(r, 22));
    in.color_mode = (uint16_t)mpsd__get16(mpsd__reader_ptr(r, 24));
    psb           = in.format == MINIPSD_PSB;
    side          = mpsd__max_side(psb);
    if ((in.format != MINIPSD_PSD && !psb) || in.channels < 1 || in.channels > MPSD__MAX_CHANNELS || !mpsd__depth_ok(in.depth)
        || !mpsd__mode_ok(in.color_mode) || in.width < 1 || in.height < 1 || in.width > side || in.height > side)
        return MINIPSD_ERR_FORMAT;
    if (r->limits.max_side) side = r->limits.max_side;
    if (in.width > side || in.height > side) return MINIPSD_ERR_LIMIT;
    r->psb = psb;

    /* The colour mode data: a palette, a duotone's inks; only viewed (the resources follow it, so
       a buffer that has them has it). */
    rc = mpsd__length(r, MPSD__HEADER_SIZE, 0, &length);
    if (rc != MINIPSD_OK) return rc;
    in.color_mode_data.data = length ? mpsd__reader_ptr(r, MPSD__HEADER_SIZE + MPSD__LEN) : NULL;
    in.color_mode_data.size = (size_t)length;
    at = MPSD__HEADER_SIZE + MPSD__LEN + length;

    /* The image resources, all of them: the resolution and the ICC profile are among them. */
    rc = mpsd__length(r, at, 0, &length);
    if (rc != MINIPSD_OK) return rc;
    r->res_start = at + MPSD__LEN;
    r->res_end   = r->res_start + length;
    rc = mpsd__have(r, 0, r->res_end);
    if (rc != MINIPSD_OK) return rc;
    for (at = r->res_start; at < r->res_end;) {                 /* checks every block as well */
        minipsd_resource res;
        rc = mpsd__resource(r, at, &res, &at);
        if (rc != MINIPSD_OK) return rc;
        if (res.id == MINIPSD_RES_RESOLUTION_INFO && !has_dpi) {
            if (res.data.size < MPSD__RESOLUTION_SIZE) return MINIPSD_ERR_FORMAT;
            in.dpi  = (double)mpsd__get32(res.data.data) / MPSD__FIXED_ONE;
            has_dpi = 1;
        } else if (res.id == MINIPSD_RES_ICC_PROFILE && !has_icc) {
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
        if (mpsd__reader_holds(r, tail, lm_start + length)) in.layer_tail.data = mpsd__reader_ptr(r, tail);
        rc = info_length ? mpsd__layers_at(r, lm_start + mpsd__len_size(psb), tail, &in) : MINIPSD_OK;
        if (rc == MINIPSD_OK && !in.layer_count) rc = mpsd__deep_layers(r, tail, lm_start + length, &in);
        if (rc != MINIPSD_OK) return rc;
    }
    r->layer_count = in.layer_count;
    r->record_next = r->li_start;
    r->rgb         = in.color_mode == MINIPSD_MODE_RGB;
    r->depth       = in.depth;
    r->shift       = mpsd__shift(in.depth);
    r->colors      = mpsd__colors(in.color_mode, in.channels - (in.composite_has_alpha ? 1u : 0u));
    in.color_channels = (uint16_t)r->colors;
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
    r->base         = 0;
    r->needed_from  = r->needed = 0;
    r->limits.max_side   = limits ? limits->max_side : 0;   /* 0: the format's own, known from the header */
    r->limits.max_layers = limits && limits->max_layers ? limits->max_layers : MPSD__MAX_LAYERS;
    r->limits.max_decoded_bytes = limits ? limits->max_decoded_bytes : 0;
    r->layer_side        = limits ? limits->max_side : 0;
    r->ready        = 0;
    r->rgb          = 0;
    r->depth        = r->shift = r->colors = 0;
    r->psb          = 0;
    r->res_start    = r->res_end = 0;
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

MINIPSDDEF int minipsd_reader_next_resource(const minipsd_reader *r, size_t *pos, minipsd_resource *res)
{
    uint64_t next;
    int      rc;
    if (!r || !pos || !res || !r->ready) return MINIPSD_ERR_ARG;
    if (*pos >= r->res_end - r->res_start) return MINIPSD_OK;
    rc = mpsd__resource(r, r->res_start + *pos, res, &next);   /* a pos it did not give: checked again */
    if (rc != MINIPSD_OK) return rc;
    *pos = (size_t)(next - r->res_start);                      /* the section lies in the buffer */
    return MINIPSD_MORE;
}

MINIPSDDEF int minipsd_reader_find_resource(const minipsd_reader *r, uint16_t id, minipsd_resource *res)
{
    minipsd_resource found;
    size_t           pos = 0;
    if (!r || !res || !r->ready) return MINIPSD_ERR_ARG;
    while (minipsd_reader_next_resource(r, &pos, &found) == MINIPSD_MORE) {
        if (found.id == id) {
            *res = found;
            return MINIPSD_OK;
        }
    }
    return MINIPSD_ERR_NOT_FOUND;
}

/* --- layers: a record each, then every layer's channel data --- */

/* One layer record, parsed.  The channel data is not in the record: for the colour channels it
   keeps where each one starts in the layer's run of channel data, and how long it is. */
typedef struct mpsd__rec {
    minipsd_layer_view view;
    uint64_t           offset[MPSD__SLOTS];   /* in the layer's channel data, by slot (detail/format.h) */
    uint64_t           length[MPSD__SLOTS];   /* 0: the layer has no such channel           */
    uint64_t           total;       /* all of its channel data, other channels too  */
    uint64_t           next;        /* where the next record starts                 */
} mpsd__rec;

/* A range of the layer info, [from, end): past li_end it is malformed, past the window only the
   window is short. */
static int mpsd__room(minipsd_reader *r, uint64_t from, uint64_t end)
{
    return end > r->li_end ? MINIPSD_ERR_FORMAT : mpsd__have(r, from, end);
}

/* The slots a target and a layer view have in this file's mode: its colour channels, then
   transparency, as many as fit. */
static int mpsd__planes(const minipsd_reader *r)
{
    return r->colors < MINIPSD_PLANES ? (int)r->colors + 1 : MINIPSD_PLANES;
}

/* Where a PSD channel id goes: its slot, or -1 for a channel the reader does not read.  Colour
   channels come first, transparency after them (detail/format.h). */
static int mpsd__slot(const minipsd_reader *r, int id)
{
    if (id >= 0) return id < mpsd__planes(r) && (unsigned)id < r->colors ? id : -1;
    if (id == -1) return r->colors < MINIPSD_PLANES ? (int)r->colors : -1;
    return id == -2 ? MPSD__SLOT_MASK : id == -3 ? MPSD__SLOT_REAL : -1;
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
    minipsd_block  block;
    size_t         pos;
    uint32_t       skip;
    int            k, rc;
    for (k = 0; k < 2; ++k) {                                 /* layer mask data, blending ranges */
        if (end - at < MPSD__LEN) return MINIPSD_ERR_FORMAT;
        skip = mpsd__get32(mpsd__reader_ptr(r, at));
        at += MPSD__LEN;
        if (skip > end - at) return MINIPSD_ERR_FORMAT;
        if (k == 0 && skip >= MPSD__MASK_HEAD) {               /* rectangle, colour, flags, the rest */
            const uint8_t *m = mpsd__reader_ptr(r, at);
            minipsd_mask  *mask = &v->mask;
            mask->top    = (int32_t)mpsd__get32(m);     mask->left  = (int32_t)mpsd__get32(m + 4);
            mask->bottom = (int32_t)mpsd__get32(m + 8); mask->right = (int32_t)mpsd__get32(m + 12);
            if (mask->bottom < mask->top) mask->bottom = mask->top;   /* inverted: empty, as layers */
            if (mask->right < mask->left) mask->right = mask->left;
            mask->default_color = m[MPSD__RECT_SIZE];
            mask->flags     = m[MPSD__RECT_SIZE + 1];
            mask->rest.data = m + MPSD__MASK_HEAD;
            mask->rest.size = skip - MPSD__MASK_HEAD;
            v->has_mask     = 1;
        }
        if (k == 1) {
            if (skip % MPSD__RANGE_SIZE) return MINIPSD_ERR_FORMAT;
            if (skip) v->blending_ranges.data = mpsd__reader_ptr(r, at);
            v->blending_ranges.size = skip;
            v->blending_range_count = skip / MPSD__RANGE_SIZE;
        }
        at += skip;
    }
    if (end == at || mpsd__pad(1u + *mpsd__reader_ptr(r, at), 4) > end - at) return MINIPSD_ERR_FORMAT;
    v->name_pascal.data = mpsd__reader_ptr(r, at) + 1;
    v->name_pascal.size = *mpsd__reader_ptr(r, at);
    at += mpsd__pad(1u + *mpsd__reader_ptr(r, at), 4);
    v->additional_info.data = mpsd__reader_ptr(r, at);
    v->additional_info.size = (size_t)(end - at);
    for (pos = 0; pos < v->additional_info.size;) {
        rc = mpsd__block(v->additional_info, pos, r->psb, &block, &pos);
        if (rc != MINIPSD_OK) return rc;
        if (mpsd__tag((const uint8_t *)block.key, "luni") && !v->name_unicode.data) {
            rc = mpsd__luni(block.data, &v->name_unicode);
            if (rc != MINIPSD_OK) return rc;
        }
        if (mpsd__tag((const uint8_t *)block.key, "lsct") && block.data.size >= MPSD__LSCT_TYPE && !v->section_divider_type) {
            const uint32_t type = mpsd__get32(block.data.data);
            v->section_divider_type = (uint8_t)(type <= (uint32_t)MINIPSD_SECTION_GROUP_END ? type : 0u);   /* unknown: a layer */
        }
    }
    return MINIPSD_OK;
}

/* The record at `at`, which must lie in the layer info; laid out as detail/format.h shows.  The
   caller reads from `from` on, `at` or a record before it. */
static int mpsd__record(minipsd_reader *r, uint64_t from, uint64_t at, mpsd__rec *rec)
{
    minipsd_layer_view *v = &rec->view;
    uint64_t            pos, extra_end;
    const unsigned      entry = mpsd__entry_size(r->psb);    /* a channel: its id, its length */
    uint32_t            channels, k, extra;
    int                 rc;
    int32_t             rect[4];

    rc = mpsd__room(r, from, at + MPSD__RECORD_HEAD);                /* the rectangle and the channel count */
    if (rc != MINIPSD_OK) return rc;
    channels = mpsd__get16(mpsd__reader_ptr(r, at) + MPSD__RECT_SIZE);
    pos      = at + MPSD__RECORD_HEAD + (uint64_t)entry * channels;   /* after the channel table */
    rc = mpsd__room(r, from, pos + MPSD__RECORD_TAIL);               /* blend, flags, extra length */
    if (rc != MINIPSD_OK) return rc;

    for (k = 0; k < 4; ++k) rect[k] = (int32_t)mpsd__get32(mpsd__reader_ptr(r, at) + 4 * (size_t)k);
    for (k = 0; k < MINIPSD_PLANES; ++k) {
        v->channel[k].data = NULL;
        v->channel[k].size = 0;
    }
    for (k = 0; k < MPSD__SLOTS; ++k) rec->offset[k] = rec->length[k] = 0;
    memset(&v->mask, 0, sizeof v->mask);
    v->section_divider_type = v->has_mask = 0;
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
        const uint8_t *e      = mpsd__reader_ptr(r, at) + MPSD__RECORD_HEAD + (size_t)entry * k;
        const uint64_t length = mpsd__getlen(e + 2, r->psb);   /* after the id */
        const int      slot   = mpsd__slot(r, (int16_t)(uint16_t)mpsd__get16(e));
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

    if (!mpsd__tag(mpsd__reader_ptr(r, pos), "8BIM")) return MINIPSD_ERR_FORMAT;
    for (k = 0; k < 4; ++k) v->blend_mode[k] = (char)mpsd__reader_ptr(r, pos)[4 + k];
    v->blend_mode[4] = '\0';
    v->opacity  = mpsd__reader_ptr(r, pos)[8];
    v->clipping = mpsd__reader_ptr(r, pos)[9];
    v->flags    = mpsd__reader_ptr(r, pos)[10];
    v->hidden   = (v->flags & MPSD__FLAG_HIDDEN) != 0;
    extra       = mpsd__get32(mpsd__reader_ptr(r, pos) + 12);
    extra_end   = pos + MPSD__RECORD_TAIL + extra;
    rc = mpsd__room(r, from, extra_end);
    if (rc != MINIPSD_OK) return rc;
    v->name_pascal.data = v->name_unicode.data = v->additional_info.data = NULL;
    v->name_pascal.size = v->name_unicode.size = v->additional_info.size = 0;
    v->blending_ranges.data = NULL;
    v->blending_ranges.size = 0;
    v->blending_range_count = 0;
    rc = mpsd__extra(r, pos + MPSD__RECORD_TAIL, extra_end, v);
    rec->next = extra_end;
    if (!v->has_mask) {                                        /* mask channels with no mask data */
        v->other_channels = (uint16_t)(v->other_channels + (rec->length[MPSD__SLOT_MASK] != 0) + (rec->length[MPSD__SLOT_REAL] != 0));
        rec->length[MPSD__SLOT_MASK] = rec->length[MPSD__SLOT_REAL] = 0;
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
        const int rc = mpsd__record(r, r->li_start, at, &rec);
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
    (void)mpsd__record(r, r->record_next, r->record_next, &rec);             /* the walk checked every record */
    if (r->layer_side && ((uint64_t)((int64_t)rec.view.right - rec.view.left) > r->layer_side
                          || (uint64_t)((int64_t)rec.view.bottom - rec.view.top) > r->layer_side))
        return MINIPSD_ERR_LIMIT;
    for (k = 0; k < MPSD__SLOTS; ++k) {
        const uint64_t at   = r->data_next + rec.offset[k];
        minipsd_bytes *view = k < MINIPSD_PLANES ? &rec.view.channel[k] : k == MPSD__SLOT_MASK ? &rec.view.mask.stored : &rec.view.mask.real;
        view->size = (size_t)rec.length[k];                     /* within the section, which fits */
        view->data = rec.length[k] && mpsd__reader_holds(r, at, at + rec.length[k]) ? mpsd__reader_ptr(r, at) : NULL;
        if (k < MINIPSD_PLANES) rec.view.channel_offset[k] = rec.length[k] ? at : 0;
        if (k == MPSD__SLOT_MASK) rec.view.mask_offset = rec.length[k] ? at : 0;
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
    if (layer->name_unicode.data) return mpsd__utf16be_utf8(layer->name_unicode.data, layer->name_unicode.size, out);
    for (i = 0; i < layer->name_pascal.size; ++i)              /* Latin-1: each byte is a code point */
        n += mpsd__put_utf8(out ? out + n : NULL, layer->name_pascal.data[i]);
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
    if (*pos >= layer->additional_info.size) return MINIPSD_OK;
    rc = mpsd__block(layer->additional_info, *pos, layer->format == MINIPSD_PSB, block, &next);
    if (rc != MINIPSD_OK) return rc;
    *pos = next;
    return MINIPSD_MORE;
}

/* --- pixels: a layer's channels into the caller's planes --- */

/* Do the bytes one decode call writes, width * rows samples for each of the mode's planes asked
   for, stay within the limit? */
static int mpsd__within(const minipsd_reader *r, const minipsd_target *t, uint32_t width, uint32_t rows)
{
    const uint64_t max = r->limits.max_decoded_bytes;
    uint64_t       planes = 0;
    int            k;
    for (k = 0; k < mpsd__planes(r); ++k) planes += t->channel[k].data != NULL;
    return !max || !planes || !rows || width <= (max >> r->shift) / planes / rows;   /* no product to overflow */
}

/* Can the file's samples go into samples of the depth opts ask for (0 is 8)?  Only its own. */
static int mpsd__opts_depth(const minipsd_decode_opts *opts)
{
    const int depth = opts && opts->depth ? opts->depth : MPSD__CH_8BIT;
    return mpsd__depth_ok((uint32_t)depth) ? depth : -1;
}

/* Fills rows of a plane with full samples: the transparency of a file that stores none. */
static void mpsd__opaque(const minipsd_reader *r, const minipsd_plane *pl, uint32_t width, uint32_t rows)
{
    uint32_t x, y;
    for (y = 0; y < rows; ++y)
        for (x = 0; x < width; ++x)
            mpsd__set_sample(pl->data + (ptrdiff_t)y * pl->row_step + (ptrdiff_t)x * pl->pixel_step, r->shift, mpsd__full(r->depth));
}

/* planes samples a pixel, interleaved, of 1 << shift bytes; the target's other planes NULL. */
static void mpsd__target(minipsd_target *t, uint8_t *pixels, size_t row_stride, unsigned planes, unsigned shift)
{
    unsigned k;
    if (!t) return;
    for (k = 0; k < MINIPSD_PLANES; ++k) {
        t->channel[k].data       = pixels && k < planes ? pixels + ((size_t)k << shift) : NULL;
        t->channel[k].pixel_step = (ptrdiff_t)planes << shift;
        t->channel[k].row_step   = (ptrdiff_t)row_stride;
    }
}

MINIPSDDEF void minipsd_target_rgba(minipsd_target *t, uint8_t *rgba, size_t row_stride)
{
    mpsd__target(t, rgba, row_stride, MPSD__RGBA, MPSD__SHIFT_8);
}

MINIPSDDEF void minipsd_target_rgba16(minipsd_target *t, uint16_t *rgba, size_t row_stride)
{
    mpsd__target(t, (uint8_t *)rgba, row_stride, MPSD__RGBA, MPSD__SHIFT_16);
}

MINIPSDDEF void minipsd_target_interleaved(minipsd_target *t, void *pixels, size_t row_stride, unsigned planes, int depth)
{
    mpsd__target(t, (uint8_t *)pixels, row_stride, planes < MINIPSD_PLANES ? planes : MINIPSD_PLANES,
                 mpsd__shift(depth > 0 ? (unsigned)depth : MPSD__CH_8BIT));
}

/* One channel's stored data, checked whole before a sample is written: the compression marker,
   then raw rows, or (RLE) a byte count per row (two bytes, four in a PSB) and the packed rows, or
   (ZIP) a zlib stream of the raw rows, inflated by the caller when its turn comes.  A row is width
   samples of the file's depth, big-endian, packed at 1 bit (detail/samples.h). */
typedef struct mpsd__chan {
    const uint8_t *table;   /* RLE: the row byte counts; NULL for raw   */
    const uint8_t *rows;    /* the first row's data                     */
    const uint8_t *stream;  /* ZIP: the zlib stream; NULL once inflated */
    size_t         stream_size;
    unsigned       predicted; /* ZIP: each sample stored as its difference from the left one */
    unsigned       shuffled;  /* raw rows of a predicted 32-bit channel: shuffled (detail/samples.h) */
    unsigned       depth;   /* bits a sample                            */
    unsigned       shift;   /* a sample takes 1 << shift bytes in memory */
    unsigned       psb;     /* row counts of four bytes                 */
} mpsd__chan;

/* A channel of the reader's file, before it is opened. */
static void mpsd__chan_init(const minipsd_reader *r, mpsd__chan *ch)
{
    ch->table     = ch->rows = ch->stream = NULL;
    ch->predicted = ch->shuffled = 0;
    ch->depth     = r->depth;
    ch->shift     = r->shift;
    ch->psb       = r->psb;
}

static int mpsd__chan_open(const minipsd_reader *r, minipsd_bytes c, uint32_t width, uint32_t height,
                           const minipsd_decode_opts *opts, mpsd__chan *ch)
{
    const uint64_t counts = (uint64_t)height * mpsd__count_size(r->psb);   /* the table's bytes */
    const uint64_t plane  = mpsd__row_bytes(width, r->depth) * height;     /* the raw rows'     */
    uint64_t       left, sum = 0;
    uint32_t       y;
    left = c.size - MPSD__MARKER_SIZE;                         /* next_layer saw the marker */
    mpsd__chan_init(r, ch);
    switch (mpsd__get16(c.data)) {
    case MPSD__COMP_RAW:
        if (plane > left) return MINIPSD_ERR_FORMAT;
        ch->rows = c.data + MPSD__MARKER_SIZE;
        return MINIPSD_OK;
    case MPSD__COMP_RLE:                                       /* PackBits */
        if (counts > left) return MINIPSD_ERR_FORMAT;
        ch->table = c.data + MPSD__MARKER_SIZE;
        ch->rows  = ch->table + (size_t)counts;
        for (y = 0; y < height; ++y) sum += mpsd__count(ch->table, y, ch->psb);
        return sum > left - counts ? MINIPSD_ERR_FORMAT : MINIPSD_OK;
    case MPSD__COMP_ZIP: case MPSD__COMP_ZIP_PREDICTED:
        if (!opts || !opts->inflate) return MINIPSD_ERR_UNSUPPORTED;
        ch->predicted = mpsd__get16(c.data) == MPSD__COMP_ZIP_PREDICTED;
        if (ch->predicted && r->depth == MPSD__CH_1BIT) return MINIPSD_ERR_FORMAT;   /* bits have no neighbours to subtract */
        if (!opts->scratch || opts->scratch_size < plane) return MINIPSD_ERR_ARG;
        ch->stream      = c.data + MPSD__MARKER_SIZE;
        ch->stream_size = (size_t)left;
        return MINIPSD_OK;
    default:
        return MINIPSD_ERR_FORMAT;
    }
}

/* A ZIP channel inflated whole into its part of opts->scratch, at, its prediction undone (a 32-bit
   one's rows left shuffled), so its rows are raw ones from there on; with scratch_kept they are
   there already. */
static int mpsd__chan_inflate(mpsd__chan *ch, uint32_t width, uint32_t height, const minipsd_decode_opts *opts,
                              uint8_t *at)
{
    const size_t   row = (size_t)mpsd__row_bytes(width, ch->depth);   /* the caller checked the scratch holds them */
    const uint8_t *stream = ch->stream;
    uint32_t       y;
    if (!stream) return MINIPSD_OK;                            /* not ZIP: opts may be NULL */
    ch->rows     = at;
    ch->stream   = NULL;
    ch->shuffled = ch->predicted && ch->depth == MPSD__CH_32BIT;
    if (opts->scratch_kept) return MINIPSD_OK;
    if (opts->inflate(opts->inflate_user, stream, ch->stream_size, at, row * height)) return MINIPSD_ERR_FORMAT;
    for (y = 0; ch->predicted && y < height; ++y) mpsd__unpredict(at + (size_t)y * row, width, ch->depth);
    return MINIPSD_OK;
}

/* Rows [y0, y0 + rows) of a checked channel into a plane; 1-bit rows unpacked there. */
static int mpsd__chan_rows(const mpsd__chan *ch, uint32_t width, uint32_t y0, uint32_t rows, const minipsd_plane *pl)
{
    const uint8_t *src   = ch->rows;
    const uint32_t bytes = (uint32_t)mpsd__row_bytes(width, ch->depth);   /* a side fits 300000 */
    uint32_t       y, x;
    if (!ch->table) {
        src += (size_t)y0 * bytes;
        for (y = 0; y < rows; ++y, src += bytes) {
            uint8_t *out = pl->data + (ptrdiff_t)y * pl->row_step;
            for (x = 0; x < bytes; ++x) out[mpsd__at(x, pl->pixel_step, ch->shift)] = src[ch->shuffled ? mpsd__shuffled(x, width) : x];
            if (ch->depth == MPSD__CH_1BIT) mpsd__unpack_bits(out, pl->pixel_step, width);
        }
        return MINIPSD_OK;
    }
    for (y = 0; y < y0; ++y) src += mpsd__count(ch->table, y, ch->psb);   /* to the band */
    for (y = 0; y < rows; ++y) {
        uint8_t     *out = pl->data + (ptrdiff_t)y * pl->row_step;
        const size_t n   = mpsd__count(ch->table, y0 + y, ch->psb);
        if (!mpsd__unpackbits(src, n, out, pl->pixel_step, bytes, ch->shift)) return MINIPSD_ERR_FORMAT;
        if (ch->depth == MPSD__CH_1BIT) mpsd__unpack_bits(out, pl->pixel_step, width);
        src += n;
    }
    return MINIPSD_OK;
}

MINIPSDDEF int minipsd_decode_layer(minipsd_reader *r, const minipsd_layer_view *layer, const minipsd_target *target,
                                    const minipsd_decode_opts *opts)
{
    mpsd__chan ch[MINIPSD_PLANES];
    uint64_t   short_from = UINT64_MAX, short_end = 0, plane;
    uint32_t   width, height, y0, rows;
    uint8_t   *part = opts ? opts->scratch : NULL;            /* where the next ZIP channel goes */
    int        k, rc, zip, planes, zips = 0, apart = 0, depth = mpsd__opts_depth(opts);
    if (!r || !layer || !target || !r->ready || depth < 0) return MINIPSD_ERR_ARG;
    width  = (uint32_t)((int64_t)layer->right - layer->left);    /* next_layer checked the order */
    height = (uint32_t)((int64_t)layer->bottom - layer->top);
    y0     = opts ? opts->y_first : 0;
    if (y0 > height) return MINIPSD_ERR_ARG;
    rows   = opts && opts->y_count ? opts->y_count : height - y0;
    if (rows > height - y0) return MINIPSD_ERR_ARG;
    if (!mpsd__within(r, target, width, rows)) return MINIPSD_ERR_LIMIT;
    if ((unsigned)depth != r->depth) return MINIPSD_ERR_UNSUPPORTED;
    if (width == 0 || rows == 0) return MINIPSD_OK;
    planes = mpsd__planes(r);
    plane  = mpsd__row_bytes(width, r->depth) * height;       /* a channel's raw rows */

    /* Every channel the target asks for: present, in the buffer, well formed. */
    for (k = 0; k < planes; ++k) {
        const minipsd_bytes c = layer->channel[k];
        if (!target->channel[k].data || ((unsigned)k == r->colors && c.size == 0)) continue;
        if (c.size == 0) return MINIPSD_ERR_FORMAT;            /* a colour channel is missing */
        if (c.data) continue;
        if (layer->channel_offset[k] < short_from) short_from = layer->channel_offset[k];
        if (layer->channel_offset[k] + c.size > short_end) short_end = layer->channel_offset[k] + c.size;
    }
    if (short_end) {
        r->needed_from = short_from;
        r->needed      = short_end;
        return MINIPSD_ERR_SHORT;
    }
    for (k = 0; k < planes; ++k) {
        if (!target->channel[k].data || layer->channel[k].size == 0) continue;
        rc = mpsd__chan_open(r, layer->channel[k], width, height, opts, &ch[k]);
        if (rc != MINIPSD_OK) return rc;
        zips += ch[k].stream != NULL;
    }
    /* Each ZIP channel its own part of the scratch when it has room for all (one does: chan_open
       checked), else all in one; scratch_kept needs the parts the previous call filled. */
    if (zips) {
        apart = opts->scratch_size / plane >= (uint64_t)zips;
        if (opts->scratch_kept && !apart) return MINIPSD_ERR_ARG;
    }

    for (k = 0; k < planes; ++k) {
        const minipsd_plane *pl = &target->channel[k];
        if (!pl->data) continue;
        if (layer->channel[k].size == 0) {                     /* no transparency: opaque */
            mpsd__opaque(r, pl, width, rows);
            continue;
        }
        zip = ch[k].stream != NULL;
        rc  = mpsd__chan_inflate(&ch[k], width, height, opts, part);
        if (zip && apart) part += (size_t)plane;
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
    int            k, rc, depth = mpsd__opts_depth(opts);
    if (!r || !layer || !plane || !r->ready || !layer->has_mask || depth < 0) return MINIPSD_ERR_ARG;
    width  = (uint32_t)((int64_t)layer->mask.right - layer->mask.left);   /* next_layer made it not inverted */
    height = (uint32_t)((int64_t)layer->mask.bottom - layer->mask.top);
    y0     = opts ? opts->y_first : 0;
    if (y0 > height) return MINIPSD_ERR_ARG;
    rows   = opts && opts->y_count ? opts->y_count : height - y0;
    if (rows > height - y0) return MINIPSD_ERR_ARG;
    for (k = 0; k < MINIPSD_PLANES; ++k) t.channel[k].data = NULL;
    t.channel[0] = *plane;
    if (!mpsd__within(r, &t, width, rows)) return MINIPSD_ERR_LIMIT;
    if ((unsigned)depth != r->depth) return MINIPSD_ERR_UNSUPPORTED;
    if (width == 0 || rows == 0) return MINIPSD_OK;
    if (!plane->data) return MINIPSD_ERR_ARG;
    c = layer->mask.stored;
    if (c.size == 0) return MINIPSD_ERR_FORMAT;                 /* a mask with an area, no channel */
    if (!c.data) {
        r->needed_from = layer->mask_offset;
        r->needed      = layer->mask_offset + c.size;
        return MINIPSD_ERR_SHORT;
    }
    rc = mpsd__chan_open(r, c, width, height, opts, &ch);
    if (rc == MINIPSD_OK) rc = mpsd__chan_inflate(&ch, width, height, opts, opts ? opts->scratch : NULL);
    return rc != MINIPSD_OK ? rc : mpsd__chan_rows(&ch, width, y0, rows, plane);
}

/* --- pixels: the composite, every channel's rows after one compression marker --- */

/* Where the composite's channels 0 .. last are, checked whole in the buffer before a sample is
   written: raw planes back to back, or (RLE) every channel's row counts, then every row. */
static int mpsd__composite_open(minipsd_reader *r, int last, mpsd__chan *ch)
{
    const uint64_t at = r->image_start + MPSD__MARKER_SIZE, plane = mpsd__row_bytes(r->width, r->depth) * r->height;
    uint64_t       sum = 0, offset[MINIPSD_PLANES], rows_at;
    uint32_t       y, n;
    int            k, rc = mpsd__have(r, r->image_start, at);
    if (rc != MINIPSD_OK) return rc;
    for (k = 0; k <= last; ++k) mpsd__chan_init(r, &ch[k]);
    switch (mpsd__get16(mpsd__reader_ptr(r, r->image_start))) {
    case MPSD__COMP_RAW:
        rc = mpsd__have(r, r->image_start, at + plane * (uint64_t)(last + 1));
        for (k = 0; rc == MINIPSD_OK && k <= last; ++k) ch[k].rows = mpsd__reader_ptr(r, at + plane * (uint64_t)k);
        return rc;
    case MPSD__COMP_RLE:
        rows_at = at + (uint64_t)r->channels * r->height * mpsd__count_size(r->psb);   /* the counts of every channel come first */
        rc = mpsd__have(r, r->image_start, rows_at);
        if (rc != MINIPSD_OK) return rc;
        for (k = 0; k <= last; ++k) {
            offset[k] = sum;
            for (y = 0, n = r->height; y < n; ++y) sum += mpsd__count(mpsd__reader_ptr(r, at), (uint32_t)k * n + y, r->psb);
        }
        rc = mpsd__have(r, r->image_start, rows_at + sum);
        for (k = 0; rc == MINIPSD_OK && k <= last; ++k) {
            ch[k].table = mpsd__reader_ptr(r, at) + (size_t)k * r->height * mpsd__count_size(r->psb);
            ch[k].rows  = mpsd__reader_ptr(r, rows_at + offset[k]);
        }
        return rc;
    case MPSD__COMP_ZIP: case MPSD__COMP_ZIP_PREDICTED:
        return MINIPSD_ERR_UNSUPPORTED;
    default:
        return MINIPSD_ERR_FORMAT;
    }
}

/* Undoes the white matte of a row's colour sample s, at alpha a (detail/composite.h). */
static void mpsd__unmatte_at(const minipsd_reader *r, uint8_t *s, const uint8_t *a)
{
    if (r->depth == MPSD__CH_32BIT) {                          /* c = 1 - (1 - s) / a */
        const float alpha = mpsd__float(a);
        mpsd__set_float(s, alpha > 0.0f ? 1.0f - (1.0f - mpsd__float(s)) / alpha : 0.0f);
    } else {
        mpsd__set_sample(s, r->shift, mpsd__unmatte(mpsd__sample(s, r->shift), mpsd__sample(a, r->shift), mpsd__full(r->depth)));
    }
}

MINIPSDDEF int minipsd_decode_composite(minipsd_reader *r, const minipsd_target *target, const minipsd_decode_opts *opts)
{
    mpsd__chan ch[MINIPSD_PLANES];
    uint32_t   y0, rows, y, x;
    int        k, rc, planes, alpha, last = -1, unmatte, colour = 0, depth = mpsd__opts_depth(opts);
    if (!r || !target || !r->ready || depth < 0) return MINIPSD_ERR_ARG;
    y0 = opts ? opts->y_first : 0;
    if (y0 > r->height) return MINIPSD_ERR_ARG;
    rows = opts && opts->y_count ? opts->y_count : r->height - y0;
    if (rows > r->height - y0) return MINIPSD_ERR_ARG;
    planes = mpsd__planes(r);
    alpha  = r->colors < MINIPSD_PLANES ? (int)r->colors : -1;   /* transparency's slot, the composite's channel after the colours */
    for (k = 0; k < planes; ++k)
        if (target->channel[k].data && k != alpha) last = k, colour = 1;
    unmatte = r->rgb && r->composite_alpha && !(opts && opts->keep_matte) && colour;
    if (unmatte && !target->channel[alpha].data) return MINIPSD_ERR_ARG;   /* the alpha is read back */
    if (!mpsd__within(r, target, r->width, rows)) return MINIPSD_ERR_LIMIT;
    if ((unsigned)depth != r->depth) return MINIPSD_ERR_UNSUPPORTED;
    if (r->channels < r->colors + (r->composite_alpha ? 1u : 0u)) return MINIPSD_ERR_FORMAT;
    if (alpha >= 0 && target->channel[alpha].data && r->composite_alpha) last = alpha;
    if (rows == 0) return MINIPSD_OK;
    if (last >= 0) {
        rc = mpsd__composite_open(r, last, ch);
        if (rc != MINIPSD_OK) return rc;
    }

    /* Transparency first, the last slot: un-matting reads it back from the target. */
    for (k = planes - 1; k >= 0; --k) {
        const minipsd_plane *pl = &target->channel[k];
        if (!pl->data) continue;
        if (k == alpha && !r->composite_alpha) {               /* no transparency: opaque */
            mpsd__opaque(r, pl, r->width, rows);
            continue;
        }
        rc = mpsd__chan_rows(&ch[k], r->width, y0, rows, pl);
        if (rc != MINIPSD_OK) return rc;
        if (k == alpha || !unmatte) continue;
        for (y = 0; y < rows; ++y) {
            const minipsd_plane *a = &target->channel[alpha];
            for (x = 0; x < r->width; ++x)
                mpsd__unmatte_at(r, pl->data + (ptrdiff_t)y * pl->row_step + (ptrdiff_t)x * pl->pixel_step,
                                 a->data + (ptrdiff_t)y * a->row_step + (ptrdiff_t)x * a->pixel_step);
        }
    }
    return MINIPSD_OK;
}

#endif /* MPSD__READER_DONE */
