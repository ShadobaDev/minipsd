/**
 * \file
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

#include <string.h>

/* The block being laid out: base NULL measures, otherwise the parts are placed from base. */
typedef struct mpsd__arena {
    uint8_t *base;
    uint64_t used;
} mpsd__arena;

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

/* What a layer record holds that the document cannot carry yet.  at is where the record starts;
   the reader has checked it whole. */
static uint32_t mpsd__record_loss(const minipsd_reader *r, uint64_t at)
{
    static const uint8_t no_range[8] = { 0, 0, 255, 255, 0, 0, 255, 255 };   /* "Blend If" left alone */
    const uint8_t *d        = r->data;
    const uint32_t channels = mpsd__get16(d + (size_t)at + 16);
    uint64_t       extra    = at + 18 + 6 * (uint64_t)channels + 16, ranges;
    uint32_t       loss = 0, k, length;
    for (k = 0; k < channels; ++k) {
        const int id = (int16_t)(uint16_t)mpsd__get16(d + (size_t)at + 18 + 6 * (size_t)k);
        /* TODO(S6.4): layer masks.  TODO(S6.8): channels beyond R, G, B, A. */
        if (id == -2 || id == -3) loss |= MINIPSD_LOST_MASK;
        else if (id < -1 || id > 2) loss |= MINIPSD_LOST_CHANNELS;
    }
    length = mpsd__get32(d + (size_t)extra);
    if (length) loss |= MINIPSD_LOST_MASK;                        /* TODO(S6.4) */
    extra += 4 + length;
    length = mpsd__get32(d + (size_t)extra);
    for (ranges = extra + 4; ranges + 8 <= extra + 4 + length; ranges += 8)
        if (memcmp(d + (size_t)ranges, no_range, 8) != 0) loss |= MINIPSD_LOST_BLENDING_RANGES;   /* TODO(S6.7) */
    return loss;
}

/* Every image resource, copied, in file order: counted on one pass, placed on a second, the reader
   started again in between. */
static int mpsd__read_resources(minipsd_reader **r, const void *data, size_t size, uint8_t *workspace,
                                size_t workspace_size, const minipsd_limits *limits, mpsd__arena *a,
                                minipsd_doc *d)
{
    minipsd_resource  res, *out;
    minipsd_info      info;
    size_t            count = 0, i;
    int               rc;
    while (minipsd_reader_next_resource(*r, &res) == MINIPSD_MORE) ++count;
    if (!count) return MINIPSD_OK;
    rc = minipsd_reader_init(r, data, size, workspace, workspace_size, limits, &info);
    if (rc != MINIPSD_OK) return rc;
    out = (minipsd_resource *)mpsd__take(a, (uint64_t)count * sizeof *out, 1);
    for (i = 0; minipsd_reader_next_resource(*r, &res) == MINIPSD_MORE; ++i) {
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
    return MINIPSD_OK;
}

/* The document in a, measured or placed; on MINIPSD_OK with a block, *out and *lost are set. */
static int mpsd__read(const void *data, size_t size, const minipsd_limits *limits, const minipsd_read_opts *opts,
                      mpsd__arena *a, minipsd_doc **out, uint32_t *lost)
{
    uint8_t             workspace[sizeof(minipsd_reader) + 2 * MPSD__ALIGN];
    minipsd_reader     *r;
    minipsd_info        info;
    minipsd_layer_view  v;
    minipsd_target      t;
    minipsd_decode_opts as_stored;
    minipsd_doc        *d;
    minipsd_layer      *layers;
    uint8_t            *pixels, *flat = NULL;
    minipsd_bytes       tail;
    const uint64_t      max = limits ? limits->max_decoded_bytes : 0;
    uint64_t            decoded, canvas;
    uint32_t            loss = 0;
    size_t              i, count, n = 0;
    int                 rc;

    rc = minipsd_reader_init(&r, data, size, workspace, sizeof workspace, limits, &info);
    if (rc != MINIPSD_OK) return rc;
    if (info.depth != 8 || info.color_mode != 3) return MINIPSD_ERR_UNSUPPORTED;
    canvas  = (uint64_t)info.width * info.height * 4;
    decoded = canvas;                                           /* the composite */
    count   = info.layer_count ? info.layer_count : 1;          /* a flat file: its image as one layer */
    d       = (minipsd_doc *)mpsd__take(a, sizeof *d, 1);
    layers  = (minipsd_layer *)mpsd__take(a, (uint64_t)count * sizeof *layers, 1);
    if (d) {
        memset(d, 0, sizeof *d);
        memset(layers, 0, count * sizeof *layers);
    }
    rc = mpsd__read_resources(&r, data, size, workspace, sizeof workspace, limits, a, d);
    if (rc != MINIPSD_OK) return rc;

    for (i = 0; i < info.layer_count; ++i) {
        const uint64_t at = r->record_next;
        uint64_t       w, h;
        char          *name;
        minipsd_bytes  extra, copies[4], *stored = NULL;
        int            k, keep;
        rc = minipsd_reader_next_layer(r, &v);
        if (rc != MINIPSD_MORE) return rc == MINIPSD_OK ? MINIPSD_ERR_FORMAT : rc;
        loss |= mpsd__record_loss(r, at);
        w = (uint64_t)((int64_t)v.right - v.left);
        h = (uint64_t)((int64_t)v.bottom - v.top);
        if (w > MINIPSD_MAX_SIDE || h > MINIPSD_MAX_SIDE) return MINIPSD_ERR_UNSUPPORTED;   /* the writer's limit */
        (void)minipsd_layer_name(&v, NULL, 0, &n);
        name   = (char *)mpsd__take(a, n, 0);
        /* Stored: every channel copied as it is, never decoded; a layer without transparency is
           decoded, since the writer writes four channels. */
        keep = opts && opts->stored && w && h;
        for (k = 0; k < 4; ++k) keep = keep && v.channel[k].size;
        if (keep) {
            stored = (minipsd_bytes *)mpsd__take(a, 4 * sizeof *stored, 1);
            for (k = 0; k < 4; ++k) copies[k] = mpsd__copy(a, v.channel[k]);
        }
        pixels = w && h && !keep ? (uint8_t *)mpsd__take(a, w * h * 4, 0) : NULL;
        extra  = mpsd__copy(a, v.extra);                        /* every block, 'luni' too */
        if (pixels) decoded += w * h * 4;
        if (max && decoded > max) return MINIPSD_ERR_LIMIT;
        if (!d) continue;
        (void)minipsd_layer_name(&v, name, n, NULL);
        if (pixels) {
            minipsd_target_rgba(&t, pixels, (size_t)w * 4);
            rc = minipsd_decode_layer(r, &v, &t, NULL);
            if (rc != MINIPSD_OK) return rc;
        }
        layers[i].name    = name;
        layers[i].rgba    = pixels;
        layers[i].opacity = v.opacity;
        layers[i].hidden  = v.hidden;
        layers[i].top     = v.top;    layers[i].left  = v.left;
        layers[i].bottom  = v.bottom; layers[i].right = v.right;
        memcpy(layers[i].blend, v.blend, 4);
        layers[i].clipping = v.clipping;
        layers[i].flags    = (uint8_t)(v.flags & ~0x02u);
        layers[i].extra    = extra;
        if (stored) {
            for (k = 0; k < 4; ++k) stored[k] = copies[k];
            layers[i].stored = stored;
        }
    }

    /* The composite as stored, matted, so it is written back exactly; a flat file's image also
       becomes its one layer, un-matted. */
    pixels = (uint8_t *)mpsd__take(a, canvas, 0);
    if (!info.layer_count) {
        flat     = (uint8_t *)mpsd__take(a, canvas, 0);
        decoded += canvas;
        if (max && decoded > max) return MINIPSD_ERR_LIMIT;
    }
    if (info.channels > 3u + (info.composite_has_alpha ? 1u : 0u))
        loss |= MINIPSD_LOST_CHANNELS;                          /* TODO(S6.8): alpha and spot channels */
    tail = mpsd__copy(a, info.layer_tail);
    if (info.icc.size) {
        uint8_t *icc = (uint8_t *)mpsd__take(a, info.icc.size, 0);
        if (icc) memcpy(icc, info.icc.data, info.icc.size);
        if (d) { d->icc = icc; d->icc_size = info.icc.size; }
    }
    if (a->used > (uint64_t)(size_t)-1 - MPSD__ALIGN) return MINIPSD_ERR_SIZE;
    if (!d) return MINIPSD_OK;

    minipsd_target_rgba(&t, pixels, (size_t)info.width * 4);
    as_stored.y_first = as_stored.y_count = 0;
    as_stored.keep_matte = 1;
    rc = minipsd_decode_composite(r, &t, &as_stored);
    if (rc != MINIPSD_OK) return rc;
    if (flat) {
        minipsd_target_rgba(&t, flat, (size_t)info.width * 4);
        rc = minipsd_decode_composite(r, &t, NULL);
        if (rc != MINIPSD_OK) return rc;
        layers[0].name    = "Background";
        layers[0].rgba    = flat;
        layers[0].opacity = 255;
        layers[0].bottom  = (int32_t)info.height;   /* the rectangle a reader then sees */
        layers[0].right   = (int32_t)info.width;
    }
    d->width            = info.width;
    d->height           = info.height;
    d->dpi              = info.dpi > 0 ? info.dpi : 72.0;      /* none stored: Photoshop's default */
    d->layers           = layers;
    d->layer_count      = count;
    d->compression      = r->image_start + 2 <= size && mpsd__get16(r->data + (size_t)r->image_start) == 0
                              ? MINIPSD_RAW : MINIPSD_RLE;
    d->composite        = pixels;
    d->composite_matted = 1;
    d->layer_tail       = tail;
    *out = d;
    if (lost) *lost = loss;
    return MINIPSD_OK;
}

MINIPSDDEF int minipsd_read_size(const void *data, size_t size, const minipsd_limits *limits,
                                 const minipsd_read_opts *opts, size_t *block_size)
{
    mpsd__arena a;
    int         rc;
    if (!block_size) return MINIPSD_ERR_ARG;
    *block_size = 0;
    a.base = NULL;
    a.used = 0;
    rc = mpsd__read(data, size, limits, opts, &a, NULL, NULL);
    if (rc == MINIPSD_OK) *block_size = (size_t)a.used + MPSD__ALIGN - 1;   /* any alignment will do */
    return rc;
}

MINIPSDDEF int minipsd_read_into(const void *data, size_t size, const minipsd_limits *limits,
                                 const minipsd_read_opts *opts, void *block, size_t block_size,
                                 minipsd_doc **doc, uint32_t *lost)
{
    mpsd__arena a;
    size_t      need = 0;
    int         rc;
    if (!doc) return MINIPSD_ERR_ARG;
    *doc = NULL;
    if (lost) *lost = 0;
    if (!block) return MINIPSD_ERR_ARG;
    rc = minipsd_read_size(data, size, limits, opts, &need);
    if (rc != MINIPSD_OK) return rc;
    if (block_size < need) return MINIPSD_ERR_ARG;
    a.base = (uint8_t *)block + (MPSD__ALIGN - (uintptr_t)block % MPSD__ALIGN) % MPSD__ALIGN;
    a.used = 0;
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
