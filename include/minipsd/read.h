/**
 * \file minipsd/read.h
 * \brief Implementation: minipsd_read, a PSD held in memory into one standalone minipsd_doc, built
 *        on the reader.  Included by minipsd.h under MINIPSD_IMPLEMENTATION; the declarations are
 *        in minipsd.h.
 *
 * One function lays the document out twice: measuring (no block), then filling the caller's
 * block.  Everything is copied, kept blocks, resources and the layer tail as they are, so nothing
 * points into the file.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MPSD__READ_DONE
#define MPSD__READ_DONE

#include "detail/bytes.h"
#include "detail/format.h"
#include "detail/floats.h"

#include <string.h>

/* The block being laid out: base NULL measures, otherwise the parts are placed from base.  After
   them, outside the document, the scratch a ZIP channel is inflated into: the largest one. */
typedef struct mpsd__arena {
    uint8_t *base;
    uint64_t used;
    uint8_t *scratch;
    uint64_t zip;
} mpsd__arena;

/* A channel to decode, n bytes inflated: when it is ZIP-compressed, the scratch must hold it. */
static void mpsd__zip_room(mpsd__arena *a, minipsd_bytes c, uint64_t n)
{
    if (c.size >= MPSD__MARKER_SIZE && c.data && mpsd__get16(c.data) >= MPSD__COMP_ZIP && n > a->zip) a->zip = n;
}

/* The next size bytes of the block, aligned for a struct when asked; NULL while measuring. */
static void *mpsd__take(mpsd__arena *a, uint64_t size, int aligned)
{
    void *p;
    if (aligned) a->used = mpsd__pad(a->used, MPSD__ALIGN);
    p = a->base ? a->base + (size_t)a->used : NULL;
    a->used += size;
    return p;
}

/* A copy of src in the block; its data NULL while measuring. */
static minipsd_bytes mpsd__copy(mpsd__arena *a, minipsd_bytes src)
{
    minipsd_bytes out;
    out.size = src.size;
    out.data = NULL;
    if (src.size) {
        uint8_t *p = (uint8_t *)mpsd__take(a, src.size, 0);
        if (p && src.data) memcpy(p, src.data, src.size);
        out.data = p;
    }
    return out;
}

/* The layer tail, copied without the 'Lr16' or 'Lr32' block the layers came from: they are the
   document's own now.  That block lies in the tail when the layer info starts inside it; it ends
   padded to four bytes past its length, as the reader walks it. */
static minipsd_bytes mpsd__copy_tail(mpsd__arena *a, const minipsd_reader *r, minipsd_bytes tail)
{
    uint64_t      at, end;
    minipsd_bytes before, key, after, out;
    uint8_t      *length;
    if (!tail.data || r->li_start <= (uint64_t)(tail.data - r->data)) return mpsd__copy(a, tail);
    at  = (uint64_t)(tail.data - r->data);
    end = r->li_start - MPSD__LAYER_COUNT + mpsd__pad((size_t)(r->li_end - (r->li_start - MPSD__LAYER_COUNT)), 4);   /* data: count, records, ... */
    if (end > at + tail.size) end = at + tail.size;
    before.data = tail.data;                                    /* the block: '8BIM', key, length (a long key's), count */
    before.size = (size_t)(r->li_start - MPSD__LAYER_COUNT - MPSD__BLOCK_LEN - mpsd__len_size(r->psb) - at);
    after.data  = r->data + (size_t)end;
    after.size  = (size_t)(at + tail.size - end);
    key.data = before.data + before.size;                       /* '8BIM' and the key, kept: where the layers go */
    key.size = MPSD__BLOCK_LEN;
    out    = mpsd__copy(a, before);                             /* the copies lie end to end */
    key    = mpsd__copy(a, key);
    length = (uint8_t *)mpsd__take(a, mpsd__len_size(r->psb), 0);   /* a length of 0: its layers are the document's */
    if (length) memset(length, 0, mpsd__len_size(r->psb));
    after  = mpsd__copy(a, after);
    if (!out.data) out.data = key.data;
    out.size += MPSD__BLOCK_LEN + mpsd__len_size(r->psb) + after.size;
    return out;
}

/* Every image resource, copied, in file order: counted on one pass, placed on a second. */
static void mpsd__read_resources(const minipsd_reader *r, mpsd__arena *a, minipsd_doc *d)
{
    minipsd_resource  res, *out;
    size_t            count = 0, pos = 0, i;
    while (minipsd_reader_next_resource(r, &pos, &res) == MINIPSD_MORE) ++count;
    if (!count) return;
    out = (minipsd_resource *)mpsd__take(a, (uint64_t)count * sizeof *out, 1);
    for (i = 0, pos = 0; minipsd_reader_next_resource(r, &pos, &res) == MINIPSD_MORE; ++i) {
        const minipsd_bytes copied = mpsd__copy(a, res.data), name = mpsd__copy(a, res.name);
        if (!out) continue;
        out[i].id   = res.id;
        out[i].data = copied;
        out[i].name = name;
    }
    if (d) {
        d->resources      = out;
        d->resource_count = count;
    }
}

/* The document in a, measured or placed; on MINIPSD_OK with a block, *out and *lost are set. */
static int mpsd__read(const void *data, size_t size, const minipsd_limits *limits, const minipsd_read_opts *opts,
                      mpsd__arena *a, minipsd_doc **out, uint32_t *lost)
{
    uint8_t             workspace[sizeof(minipsd_reader) + (size_t)2 * MPSD__ALIGN];
    minipsd_reader     *r;
    minipsd_info        info;
    minipsd_layer_view  v;
    minipsd_target      t;
    minipsd_decode_opts all, as_stored;
    minipsd_doc        *d;
    minipsd_layer      *layers;
    uint8_t            *pixels, *flat = NULL;
    minipsd_bytes       tail, composite = { NULL, 0 };
    int                 keep_composite;
    const uint64_t      max = limits ? limits->max_decoded_bytes : 0;
    uint64_t            decoded, canvas;
    uint32_t            loss = 0;
    size_t              i, count, n = 0;
    unsigned            wide, planes, cplanes;
    uint32_t            side;
    int                 rc, flat_mode;

    rc = minipsd_reader_init(&r, data, size, workspace, sizeof workspace, limits, &info);
    if (rc != MINIPSD_OK) return rc;
    {   /* the modes and depths the writer takes: a file it would refuse is refused here */
        minipsd_doc probe;
        memset(&probe, 0, sizeof probe);
        probe.depth                 = info.depth;
        probe.color_mode            = info.color_mode;
        probe.color_mode_data       = info.color_mode_data;
        probe.composite_stored.size = MPSD__MARKER_SIZE;       /* a composite it has */
        probe.composite_channels    = info.channels;
        flat_mode = mpsd__doc_flat(&probe);
        if ((info.color_mode == MINIPSD_MODE_BITMAP) != (info.depth == MPSD__CH_1BIT) || mpsd__doc_mode_ok(&probe) != MINIPSD_OK
            || (flat_mode && info.layer_count))
            return MINIPSD_ERR_UNSUPPORTED;
    }
    wide   = r->shift;                                         /* wider samples, aligned for their type */
    planes = r->colors + !flat_mode;                           /* a pixel: the colour channels, then transparency */
    cplanes = r->colors + (!flat_mode && info.composite_has_alpha && info.channels > r->colors);   /* the composite's, as the file has it */
    side   = mpsd__max_side(r->psb);
    memset(&all, 0, sizeof all);                               /* every row, un-matted; new fields zero */
    all.depth      = (int)r->depth;
    all.inflate      = opts ? opts->inflate : NULL;            /* ZIP channels: without it, unsupported */
    all.inflate_user = opts ? opts->inflate_user : NULL;
    all.scratch      = a->scratch;
    all.scratch_size = (size_t)a->zip;                         /* measured: the block holds it */
    as_stored = all;
    as_stored.keep_matte = 1;
    canvas  = (uint64_t)info.width * info.height * cplanes << wide;
    /* Stored: the composite copied as it is, every channel, never decoded; multichannel's always. */
    keep_composite = ((opts && opts->stored) || info.color_mode == MINIPSD_MODE_MULTICHANNEL) && info.channels >= r->colors;
    decoded = keep_composite ? 0 : canvas;                      /* the composite */
    /* A file without layers: its image as one layer; stored, as it is, no layer, the composite
       copied.  The flat modes have none. */
    count   = info.layer_count ? info.layer_count : keep_composite || flat_mode ? 0 : 1;
    d       = (minipsd_doc *)mpsd__take(a, sizeof *d, 1);
    layers  = (minipsd_layer *)mpsd__take(a, (uint64_t)count * sizeof *layers, 1);
    if (d) {
        memset(d, 0, sizeof *d);
        memset(layers, 0, count * sizeof *layers);
    }
    mpsd__read_resources(r, a, d);

    for (i = 0; i < info.layer_count; ++i) {
        uint64_t       w, h;
        char          *name;
        minipsd_bytes  extra, pascal, copies[MINIPSD_PLANES], *stored = NULL;
        minipsd_mask  *mask = NULL;
        minipsd_blending_range *ranges;
        uint8_t       *mask_pixels = NULL;
        uint32_t       j;
        int            k, keep;
        unsigned       lplanes;                                 /* decoded: transparency only when the file has it */
        rc = minipsd_reader_next_layer(r, &v);
        if (rc != MINIPSD_MORE) return rc == MINIPSD_OK ? MINIPSD_ERR_FORMAT : rc;
        if (v.other_channels) loss |= MINIPSD_LOST_CHANNELS;    /* TODO: alpha and spot channels (ROADMAP, Not scheduled) */
        w = (uint64_t)((int64_t)v.right - v.left);
        h = (uint64_t)((int64_t)v.bottom - v.top);
        if (w > side || h > side) return MINIPSD_ERR_UNSUPPORTED;   /* the writer's limit */
        (void)minipsd_layer_name(&v, NULL, 0, &n);
        name   = (char *)mpsd__take(a, n, 0);
        /* Stored: every channel copied as it is, never decoded; one without transparency is
           written without one, as the file has it.  An empty layer has nothing to copy. */
        keep = opts && opts->stored && w && h;
        for (k = 0; k < (int)r->colors; ++k) keep = keep && v.channel[k].size;   /* the colour channels */
        if (keep) {
            stored = (minipsd_bytes *)mpsd__take(a, planes * sizeof *stored, 1);
            for (k = 0; k < (int)planes; ++k) copies[k] = mpsd__copy(a, v.channel[k]);
        }
        lplanes = r->colors + (v.channel[r->colors].size != 0);
        pixels = w && h && !keep ? (uint8_t *)mpsd__take(a, w * h * lplanes << wide, (int)wide) : NULL;
        extra  = mpsd__copy(a, v.additional_info);              /* every block, 'luni' too */
        pascal = mpsd__copy(a, v.name_pascal);
        ranges = (minipsd_blending_range *)mpsd__take(a, (uint64_t)v.blending_range_count * sizeof *ranges, 1);
        if (pixels) decoded += w * h * lplanes << wide;
        for (k = 0; w && h && !keep && k < (int)lplanes; ++k)   /* decoded: pixels, NULL while measuring */
            mpsd__zip_room(a, v.channel[k], w * h << wide);
        if (v.has_mask) {                                       /* the mask: decoded, or stored with the layer */
            const uint64_t mw = (uint64_t)((int64_t)v.mask.right - v.mask.left), mh = (uint64_t)((int64_t)v.mask.bottom - v.mask.top);
            const int      copy = opts && opts->stored && v.mask.stored.size;
            minipsd_mask  *m = (minipsd_mask *)mpsd__take(a, sizeof *m, 1);
            minipsd_mask   kept = v.mask;
            kept.rest   = mpsd__copy(a, v.mask.rest);
            kept.real   = mpsd__copy(a, v.mask.real);
            kept.stored.data = NULL;
            kept.stored.size = 0;
            if (copy) kept.stored = mpsd__copy(a, v.mask.stored);
            if (!copy && mw && mh) {
                mask_pixels = (uint8_t *)mpsd__take(a, mw * mh << wide, (int)wide);   /* NULL while measuring */
                decoded    += mw * mh << wide;
                mpsd__zip_room(a, v.mask.stored, mw * mh << wide);
            }
            kept.pixels = mask_pixels;
            if (m) *m = kept;
            mask = m;
        }
        if (max && decoded > max) return MINIPSD_ERR_LIMIT;
        if (!d) continue;
        (void)minipsd_layer_name(&v, name, n, NULL);
        if (pixels) {
            mpsd__target(&t, pixels, (size_t)(w * lplanes << wide), lplanes, wide);
            rc = minipsd_decode_layer(r, &v, &t, &all);
            if (rc != MINIPSD_OK) return rc;
        }
        layers[i].name    = name;
        layers[i].pixels  = pixels;
        layers[i].no_alpha = (uint8_t)(lplanes == r->colors);   /* an empty one's too: its channels as the file's */
        layers[i].opacity = v.opacity;
        layers[i].hidden  = v.hidden;
        layers[i].top     = v.top;    layers[i].left  = v.left;
        layers[i].bottom  = v.bottom; layers[i].right = v.right;
        memcpy(layers[i].blend_mode, v.blend_mode, 4);
        layers[i].clipping = v.clipping;
        layers[i].flags    = (uint8_t)(v.flags & ~MPSD__FLAG_HIDDEN);
        layers[i].additional_info = extra;
        layers[i].name_pascal = pascal;
        layers[i].section_divider_type = v.section_divider_type;
        layers[i].mask     = mask;
        for (j = 0; j < v.blending_range_count; ++j) (void)minipsd_layer_blending_range(&v, j, &ranges[j]);
        layers[i].blending_ranges      = v.blending_range_count ? ranges : NULL;
        layers[i].blending_range_count = v.blending_range_count;
        if (mask_pixels) {
            minipsd_plane plane;
            plane.data       = mask_pixels;
            plane.pixel_step = (ptrdiff_t)1 << wide;
            plane.row_step   = (ptrdiff_t)((int64_t)v.mask.right - v.mask.left) << wide;
            rc = minipsd_decode_mask(r, &v, &plane, &all);
            if (rc != MINIPSD_OK) return rc;
        }
        if (stored) {
            for (k = 0; k < (int)planes; ++k) stored[k] = copies[k];
            layers[i].stored = stored;
        }
    }

    /* The composite as stored: its bytes, or its pixels matted, so it is written back exactly; a
       flat file's image also becomes its one layer, un-matted. */
    if (keep_composite) {
        if (r->image_start > size) return MINIPSD_ERR_SHORT;
        composite.data = r->data + (size_t)r->image_start;
        composite.size = size - (size_t)r->image_start;
        if (!mpsd__stored_ok(composite, info.width, info.channels * info.height, r->depth, r->psb))   /* as decoding says it */
            return composite.size >= MPSD__MARKER_SIZE && mpsd__get16(composite.data) > MPSD__COMP_ZIP_PREDICTED ? MINIPSD_ERR_FORMAT : MINIPSD_ERR_SHORT;
        composite = mpsd__copy(a, composite);
    }
    pixels = keep_composite ? NULL : (uint8_t *)mpsd__take(a, canvas, (int)wide);
    if (count && !info.layer_count) {
        flat     = (uint8_t *)mpsd__take(a, canvas, (int)wide);
        decoded += canvas;
        if (max && decoded > max) return MINIPSD_ERR_LIMIT;
    }
    if (!keep_composite && info.channels > r->colors + (info.composite_has_alpha ? 1u : 0u))
        loss |= MINIPSD_LOST_CHANNELS;                          /* decoded: the colour channels, transparency */
    tail = mpsd__copy_tail(a, r, info.layer_tail);
    if (info.icc.size) {
        uint8_t *icc = (uint8_t *)mpsd__take(a, info.icc.size, 0);
        if (icc) memcpy(icc, info.icc.data, info.icc.size);
        if (d) { d->icc = icc; d->icc_size = info.icc.size; }
    }
    {   /* the mode, and its data (a palette, a duotone's inks) as the file has it */
        const minipsd_bytes mode_data = mpsd__copy(a, info.color_mode_data);
        if (d) { d->color_mode = info.color_mode; d->color_mode_data = mode_data; }
    }
    /* reachable with a 32-bit size_t (wasm32, armv7) */
    if (a->zip > (uint64_t)(size_t)-1 - MPSD__ALIGN || a->used > (uint64_t)(size_t)-1 - MPSD__ALIGN - a->zip) return MINIPSD_ERR_SIZE;
    if (!d) return MINIPSD_OK;

    if (pixels) {
        mpsd__target(&t, pixels, (size_t)info.width * cplanes << wide, cplanes, wide);
        rc = minipsd_decode_composite(r, &t, &as_stored);
        if (rc != MINIPSD_OK) return rc;
    }
    if (flat) {   /* un-matted; without transparency there is no matte to undo */
        mpsd__target(&t, flat, (size_t)info.width * cplanes << wide, cplanes, wide);
        rc = minipsd_decode_composite(r, &t, cplanes > r->colors ? &all : &as_stored);
        if (rc != MINIPSD_OK) return rc;
        layers[0].name    = "Background";
        layers[0].pixels  = flat;
        layers[0].no_alpha = (uint8_t)(cplanes == r->colors);
        layers[0].opacity = 255;
        layers[0].bottom  = (int32_t)info.height;   /* the rectangle a reader then sees */
        layers[0].right   = (int32_t)info.width;
    }
    d->width            = info.width;
    d->height           = info.height;
    d->dpi              = info.dpi > 0 ? info.dpi : 72.0;      /* none stored: Photoshop's default */
    d->layers           = layers;
    d->layer_count      = count;
    d->compression      = r->image_start + MPSD__MARKER_SIZE <= size && mpsd__get16(r->data + (size_t)r->image_start) == MPSD__COMP_RAW
                              ? MINIPSD_RAW : MINIPSD_RLE;
    d->composite        = pixels;
    d->composite_matted = 1;
    d->composite_stored = composite;
    d->composite_channels = keep_composite ? (uint16_t)info.channels : 0;
    d->composite_alpha    = (uint8_t)(cplanes > r->colors);   /* stored or decoded, as the file has it */
    {   /* the file's pixel aspect ratio, kept when it is one the writer takes; else 0: none of its own */
        minipsd_resource aspect;
        const double     ratio = info.pixel_aspect_ratio;
        d->pixel_aspect_ratio = minipsd_reader_find_resource(r, MINIPSD_RES_PIXEL_ASPECT_RATIO, &aspect) == MINIPSD_OK
                                && mpsd__floats_positive(ratio) ? ratio : 0.0;
    }
    d->layer_tail       = tail;
    d->depth            = info.depth;
    d->format           = info.format;
    *out = d;
    if (lost) *lost = loss;
    return MINIPSD_OK;
}

/* The document measured: a->used its bytes, a->zip the scratch after them. */
static int mpsd__measure(const void *data, size_t size, const minipsd_limits *limits, const minipsd_read_opts *opts,
                         mpsd__arena *a)
{
    a->base    = NULL;
    a->used    = 0;
    a->scratch = NULL;
    a->zip     = 0;
    return mpsd__read(data, size, limits, opts, a, NULL, NULL);
}

MINIPSDDEF int minipsd_read_size(const void *data, size_t size, const minipsd_limits *limits,
                                 const minipsd_read_opts *opts, size_t *block_size)
{
    mpsd__arena a;
    int         rc;
    if (!block_size) return MINIPSD_ERR_ARG;
    *block_size = 0;
    rc = mpsd__measure(data, size, limits, opts, &a);
    if (rc == MINIPSD_OK) *block_size = (size_t)(a.used + a.zip) + MPSD__ALIGN - 1;   /* any alignment will do */
    return rc;
}

MINIPSDDEF int minipsd_read_into(const void *data, size_t size, const minipsd_limits *limits,
                                 const minipsd_read_opts *opts, void *block, size_t block_size,
                                 minipsd_doc **doc, uint32_t *lost)
{
    mpsd__arena a;
    int         rc;
    if (!doc) return MINIPSD_ERR_ARG;
    *doc = NULL;
    if (lost) *lost = 0;
    if (!block) return MINIPSD_ERR_ARG;
    rc = mpsd__measure(data, size, limits, opts, &a);
    if (rc != MINIPSD_OK) return rc;
    if (block_size < (size_t)(a.used + a.zip) + MPSD__ALIGN - 1) return MINIPSD_ERR_ARG;
    a.base    = (uint8_t *)block + (MPSD__ALIGN - (uintptr_t)block % MPSD__ALIGN) % MPSD__ALIGN;
    a.scratch = a.zip ? a.base + (size_t)a.used : NULL;        /* after the document */
    a.used    = 0;
    rc = mpsd__read(data, size, limits, opts, &a, doc, lost);
    if (rc != MINIPSD_OK) *doc = NULL;
    return rc;
}

#ifndef MINIPSD_NO_MALLOC

MINIPSDDEF int minipsd_read(const void *data, size_t size, const minipsd_limits *limits,
                            const minipsd_read_opts *opts, minipsd_doc **doc, uint32_t *lost)
{
    size_t need = 0;
    void  *block;
    int    rc;
    if (!doc) return MINIPSD_ERR_ARG;
    *doc = NULL;
    if (lost) *lost = 0;
    rc = minipsd_read_size(data, size, limits, opts, &need);
    if (rc != MINIPSD_OK) return rc;
    block = MINIPSD_MALLOC(need);
    if (!block) return MINIPSD_ERR_ALLOC;
    rc = minipsd_read_into(data, size, limits, opts, block, need, doc, lost);
    if (rc != MINIPSD_OK || (void *)*doc != block) {           /* malloc aligns: the document is at its start */
        MINIPSD_FREE(block);
        *doc = NULL;
        if (rc == MINIPSD_OK) rc = MINIPSD_ERR_ALLOC;
    }
    return rc;
}

MINIPSDDEF void minipsd_doc_free(minipsd_doc *doc)
{
    if (doc) MINIPSD_FREE(doc);
}

#endif /* MINIPSD_NO_MALLOC */

#endif /* MPSD__READ_DONE */
