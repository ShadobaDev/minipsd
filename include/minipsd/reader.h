/**
 * \file
 * \brief Implementation: the reader, a PSD held in memory walked in place.  Included by minipsd.h
 *        under MINIPSD_IMPLEMENTATION; the declarations are in minipsd.h.
 *
 * Nothing is allocated or copied: the reader keeps a few offsets in the caller's workspace and
 * hands out views into the caller's buffer.  Sections follow the order of the file: header, colour
 * mode data, image resources, layer and mask information.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MPSD__READER_DONE
#define MPSD__READER_DONE

#include "detail/bytes.h"
#include "detail/utf.h"

/* --- the reader's state --- */

struct minipsd_reader {
    const uint8_t *data;
    size_t         size;
    uint64_t       needed;        /* what the last MINIPSD_ERR_SHORT asked for                   */
    minipsd_limits limits;        /* with zero fields replaced by the format's own limits        */
    int            ready;         /* init succeeded                                              */
    uint64_t       res_next;      /* the resources: where the next one starts, where they end    */
    uint64_t       res_end;
    uint64_t       li_start;      /* the layer info: its first record, and the end of its content */
    uint64_t       li_end;
    size_t         layer_count;
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

/* Four bytes equal to a four-character tag. */
static int mpsd__tag(const uint8_t *p, const char *tag)
{
    size_t i;
    for (i = 0; i < 4; ++i)
        if (p[i] != (uint8_t)tag[i]) return 0;
    return 1;
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
}

static int mpsd__depth_ok(uint32_t depth) { return depth == 1 || depth == 8 || depth == 16 || depth == 32; }

/* Colour modes: bitmap, grey, indexed, RGB, CMYK, multichannel, duotone, Lab. */
static int mpsd__mode_ok(uint32_t mode) { return mode <= 4 || (mode >= 7 && mode <= 9); }

/* Reads everything init promises, in file order; each step asks for the bytes it needs. */
static int mpsd__scan(minipsd_reader *r, minipsd_info *info)
{
    const uint8_t *d = r->data;
    minipsd_info   in;
    uint64_t       at, lm_start;
    uint32_t       length, version;
    int32_t        count;
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
        uint32_t info_length;
        if (length < 4) return MINIPSD_ERR_FORMAT;
        rc = mpsd__length(r, lm_start, &info_length);
        if (rc != MINIPSD_OK) return rc;
        if (info_length > length - 4u || info_length == 1) return MINIPSD_ERR_FORMAT;
        if (info_length != 0) {
            rc = mpsd__have(r, lm_start + 6);
            if (rc != MINIPSD_OK) return rc;
            count = (int16_t)(uint16_t)mpsd__get16(d + (size_t)lm_start + 4);
            in.composite_has_alpha = count < 0;
            if (count < 0) count = -count;
            if (count > MPSD__MAX_LAYERS) return MINIPSD_ERR_FORMAT;
            if ((uint32_t)count > r->limits.max_layers) return MINIPSD_ERR_LIMIT;
            in.layer_count = (size_t)count;
            r->li_start    = lm_start + 6;
            r->li_end      = lm_start + 4 + info_length;
        }
    }
    r->layer_count = in.layer_count;
    r->record_next = r->li_start;
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
    r->ready        = 0;
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

/* A layer's additional information is a run of blocks: '8BIM' (or '8B64'), a key, a length, the
   data.  The length already includes any padding. */
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

/* One layer record, parsed.  The channel data is not in the record: for the colour channels it
   keeps where each one starts in the layer's run of channel data, and how long it is. */
typedef struct mpsd__rec {
    minipsd_layer_view view;
    uint64_t           offset[4];   /* in the layer's channel data, by MINIPSD_CH_* */
    uint32_t           length[4];   /* 0: the layer has no such channel             */
    uint64_t           total;       /* all of its channel data, other channels too  */
    uint64_t           next;        /* where the next record starts                 */
} mpsd__rec;

/* A range of the layer info: past li_end it is malformed, past the buffer only the buffer is short. */
static int mpsd__room(minipsd_reader *r, uint64_t end)
{
    return end > r->li_end ? MINIPSD_ERR_FORMAT : mpsd__have(r, end);
}

/* Which MINIPSD_CH_* a PSD channel id is, or -1 for a channel the reader does not read. */
static int mpsd__slot(int id)
{
    return id >= 0 && id <= 2 ? id : id == -1 ? MINIPSD_CH_A : -1;
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
        rec->offset[k] = rec->length[k] = 0;
        v->channel[k].data = NULL;
        v->channel[k].size = 0;
    }
    if (rect[2] < rect[0] || rect[3] < rect[1]) return MINIPSD_ERR_FORMAT;
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
    v->hidden   = (d[(size_t)pos + 10] & 0x02) != 0;
    extra       = mpsd__get32(d + (size_t)pos + 12);
    extra_end   = pos + 16 + extra;
    rc = mpsd__room(r, extra_end);
    if (rc != MINIPSD_OK) return rc;
    v->name_pascal.data = v->name_unicode.data = v->extra.data = NULL;
    v->name_pascal.size = v->name_unicode.size = v->extra.size = 0;
    rc = mpsd__extra(r, pos + 16, extra_end, v);
    rec->next = extra_end;
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
    for (k = 0; k < 4; ++k) {
        const uint64_t at = r->data_next + rec.offset[k];
        rec.view.channel[k].size = rec.length[k];
        if (rec.length[k] && at + rec.length[k] <= r->size) rec.view.channel[k].data = r->data + (size_t)at;
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

#endif /* MPSD__READER_DONE */
