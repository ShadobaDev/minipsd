/**
 * \file
 * \brief Implementation: the writer, a document to PSD bytes, pulled into a buffer or pushed to a
 *        callback.  Included by minipsd.h under MINIPSD_IMPLEMENTATION; the declarations are in
 *        minipsd.h.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MPSD__WRITER_DONE
#define MPSD__WRITER_DONE

#ifndef MINIPSD_NO_MALLOC
#  if !defined(MINIPSD_MALLOC) || !defined(MINIPSD_FREE)
#    include <stdlib.h>
#    undef  MINIPSD_MALLOC
#    undef  MINIPSD_FREE
#    define MINIPSD_MALLOC(size) malloc(size)
#    define MINIPSD_FREE(ptr)    free(ptr)
#  endif
#endif

#include "detail/bytes.h"
#include "detail/utf.h"
#include "detail/packbits.h"
#include "detail/composite.h"

/* --- channel rows: PackBits or raw --- */

static void mpsd__raw(mpsd__sink *s, const uint8_t *src, size_t n, size_t stride)
{
    uint8_t chunk[256];
    size_t  i = 0;
    while (i < n) {
        size_t len = 0;
        while (i < n && len < sizeof chunk) chunk[len++] = src[i++ * stride];
        mpsd__put(s, chunk, len);
    }
}

/* One row of one channel, width pixels. */
static void mpsd__row(mpsd__sink *s, const minipsd_doc *d, const uint8_t *row, int channel, uint32_t width)
{
    if (d->compression == MINIPSD_RAW) mpsd__raw(s, row + channel, width, 4);
    else                               mpsd__packbits(s, row + channel, width, 4);
}

static uint64_t mpsd__row_size(const minipsd_doc *d, const uint8_t *row, int channel, uint32_t width)
{
    mpsd__sink c = { NULL, NULL, 0, 0 };
    mpsd__row(&c, d, row, channel, width);
    return c.count;
}

/* PSD channel order is R, G, B, then transparency (id -1); RGBA offsets 0, 1, 2, 3. */
static const int mpsd__channel_id[4] = { -1, 0, 1, 2 };
static const int mpsd__channel_of[4] = {  3, 0, 1, 2 };   /* RGBA offset for each of those ids */

/* --- names: a legacy ASCII Pascal string and the UTF-16 one --- */

static void mpsd__unit_out(void *ctx, unsigned u) { mpsd__u16((mpsd__sink *)ctx, u); }

/* Blocks kept from a file, as they are, but 'luni': the layer's own name wins.  minipsd_doc_check
   has walked them. */
static void mpsd__kept_blocks(mpsd__sink *s, minipsd_bytes extra)
{
    minipsd_block block;
    size_t        pos = 0, next;
    while (pos < extra.size && mpsd__block(extra, pos, &block, &next) == MINIPSD_OK) {
        if (!mpsd__tag((const uint8_t *)block.key, "luni")) mpsd__put(s, extra.data + pos, next - pos);
        pos = next;
    }
}

/* The extra-data block of a layer record: no mask, no blending ranges, the names, kept blocks. */
static void mpsd__layer_extra(mpsd__sink *s, const minipsd_layer *l)
{
    const char          *name = l->name;
    const unsigned char *p = (const unsigned char *)(name ? name : "");
    size_t ascii = 0, units, luni;
    uint8_t pascal[256];
    while (*p && ascii < 255) {               /* one '?' per non-ASCII character */
        const uint32_t cp = mpsd__utf8(&p);
        pascal[1 + ascii++] = (uint8_t)(cp < 0x80 ? cp : '?');
    }
    pascal[0] = (uint8_t)ascii;
    mpsd__u32(s, 0);                          /* layer mask data      */
    mpsd__u32(s, 0);                          /* blending ranges      */
    mpsd__put(s, pascal, ascii + 1);
    mpsd__zeros(s, mpsd__pad(ascii + 1, 4) - (ascii + 1));

    units = mpsd__utf16(name, NULL, NULL);
    luni  = 4 + units * 2;
    mpsd__put(s, "8BIMluni", 8);
    mpsd__u32(s, (uint32_t)mpsd__pad(luni, 4));
    mpsd__u32(s, (uint32_t)units);
    mpsd__utf16(name, mpsd__unit_out, s);
    mpsd__zeros(s, mpsd__pad(luni, 4) - luni);
    mpsd__kept_blocks(s, l->extra);
}

/* --- the layer section --- */

/* What a layer channel's data takes: its compression, then (RLE) the row byte counts, then the
   rows. */
static uint64_t mpsd__layer_channel_size(const minipsd_doc *d, const minipsd_layer *l, int channel)
{
    const mpsd__rect r = mpsd__layer_rect(d, l);
    uint64_t size = 2;
    uint32_t y;
    if (!r.height) return size;
    if (d->compression == MINIPSD_RAW) return size + (uint64_t)r.width * r.height;
    for (y = 0; y < r.height; ++y)
        size += 2 + mpsd__row_size(d, l->rgba + (size_t)y * r.width * 4, channel, r.width);
    return size;
}

static uint64_t mpsd__layer_extra_size(const minipsd_layer *l)
{
    mpsd__sink c = { NULL, NULL, 0, 0 };
    mpsd__layer_extra(&c, l);
    return c.count;
}

/* One layer record.  sizes holds its four channels' sizes (PSD order), from
   mpsd__layer_channel_size. */
static void mpsd__layer_record(mpsd__sink *s, const minipsd_doc *d, const minipsd_layer *l, const uint64_t *sizes)
{
    const mpsd__rect r = mpsd__layer_rect(d, l);
    int k;
    mpsd__u32(s, (uint32_t)r.top);    mpsd__u32(s, (uint32_t)r.left);
    mpsd__u32(s, (uint32_t)r.bottom); mpsd__u32(s, (uint32_t)r.right);
    mpsd__u16(s, 4);
    for (k = 0; k < 4; ++k) {
        mpsd__u16(s, (unsigned)(uint16_t)(int16_t)mpsd__channel_id[k]);
        mpsd__u32(s, (uint32_t)sizes[k]);
    }
    mpsd__put(s, "8BIM", 4);
    mpsd__put(s, l->blend[0] || l->blend[1] || l->blend[2] || l->blend[3] ? l->blend : "norm", 4);
    mpsd__u8(s, l->opacity);
    mpsd__u8(s, l->clipping);
    mpsd__u8(s, (l->flags & ~0x02u) | (l->hidden ? 0x02u : 0x00u));
    mpsd__u8(s, 0);                                               /* filler          */
    mpsd__u32(s, (uint32_t)mpsd__layer_extra_size(l));
    mpsd__layer_extra(s, l);
}

/* --- the file --- */

/* PSD (version 1) stores section lengths in 32 bits. */
#define MPSD__MAX_SECTION 0xFFFFFFFFu

/* A kept resource is written unless the document's own field says the same: 1005 is dpi, 1039 icc. */
static int mpsd__kept_resource(const minipsd_resource *r) { return r->id != 1005 && r->id != 1039; }

/* The image resources' length: the resolution, the ICC profile if any, the kept resources. */
static uint64_t mpsd__resources_size(const minipsd_doc *d)
{
    uint64_t size = 28 + (d->icc ? 12 + (uint64_t)mpsd__pad(d->icc_size, 2) : 0);
    size_t   i;
    for (i = 0; i < d->resource_count; ++i)
        if (mpsd__kept_resource(&d->resources[i]))
            size += 6 + mpsd__pad(1 + d->resources[i].name.size, 2) + 4 + (uint64_t)mpsd__pad(d->resources[i].data.size, 2);
    return size;
}

/* The dpi as stored: 16.16 fixed point. */
static uint32_t mpsd__dpi_fixed(const minipsd_doc *d) { return (uint32_t)(d->dpi * 65536.0 + 0.5); }

/* A kept resolution (1005) that says the document's dpi, with its units and vertical resolution:
   written in place of the writer's own, which shows inches.  NULL when there is none. */
static const uint8_t *mpsd__kept_resolution(const minipsd_doc *d)
{
    size_t i;
    for (i = 0; i < d->resource_count; ++i) {
        const minipsd_resource *r = &d->resources[i];
        if (r->id == 1005)
            return r->data.size == 16 && r->data.data && mpsd__get32(r->data.data) == mpsd__dpi_fixed(d) ? r->data.data : NULL;
    }
    return NULL;
}

/* What follows the layer info in its section: the kept tail, or an empty global layer mask. */
static uint64_t mpsd__tail_size(const minipsd_doc *d) { return d->layer_tail.size ? d->layer_tail.size : 4; }

/* Pointers with sizes, and well-formed blocks: what the sizes below rely on. */
static int mpsd__kept_ok(const minipsd_doc *d)
{
    size_t i, pos, next;
    minipsd_block block;
    if ((d->icc_size && !d->icc) || (d->resource_count && !d->resources) || (d->layer_tail.size && !d->layer_tail.data))
        return MINIPSD_ERR_ARG;
    for (i = 0; i < d->resource_count; ++i) {
        const minipsd_resource *r = &d->resources[i];
        if ((r->data.size && !r->data.data) || (r->name.size && !r->name.data)) return MINIPSD_ERR_ARG;
        if (r->name.size > 255) return MINIPSD_ERR_SIZE;
    }
    for (i = 0; i < d->layer_count; ++i) {
        const minipsd_bytes extra = d->layers[i].extra;
        if (extra.size && !extra.data) return MINIPSD_ERR_ARG;
        for (pos = 0; pos < extra.size; pos = next)
            if (mpsd__block(extra, pos, &block, &next) != MINIPSD_OK) return MINIPSD_ERR_ARG;
    }
    return MINIPSD_OK;
}

MINIPSDDEF int minipsd_doc_check(const minipsd_doc *d)
{
    double res;
    size_t i;
    int    rc;
    if (!d) return MINIPSD_ERR_ARG;
    /* The dpi is stored as 16.16 fixed point: it must round to 1 .. 2^32 - 1.  NaN fails both. */
    res = d->dpi * 65536.0 + 0.5;
    if (!d->layers || d->layer_count < 1 || d->layer_count > 32767 || !(res >= 1.0 && res < 4294967296.0))
        return MINIPSD_ERR_ARG;
    rc = mpsd__kept_ok(d);
    if (rc != MINIPSD_OK) return rc;
    if (d->width < 1 || d->height < 1 || d->width > MINIPSD_MAX_SIDE || d->height > MINIPSD_MAX_SIDE)
        return MINIPSD_ERR_SIZE;
    if (mpsd__resources_size(d) > MPSD__MAX_SECTION) return MINIPSD_ERR_SIZE;
    for (i = 0; i < d->layer_count; ++i) {    /* RLE row counts are 16 bits: a side fits 30000 */
        const minipsd_layer *l = &d->layers[i];
        const int64_t w = (int64_t)l->right - l->left, h = (int64_t)l->bottom - l->top;
        if (w < 0 || h < 0 || (!l->rgba && w && h)) return MINIPSD_ERR_ARG;
        if (w > MINIPSD_MAX_SIDE || h > MINIPSD_MAX_SIDE) return MINIPSD_ERR_SIZE;
        if (mpsd__layer_extra_size(l) > MPSD__MAX_SECTION) return MINIPSD_ERR_SIZE;
    }
    return MINIPSD_OK;
}

/* The sizing pass over the layers: every channel's size into sizes (layer_count * 4, PSD order),
   the layer info's length into *content and, padded to even, *info.  Every layer channel's size
   goes in the records ahead of the data, so each is compressed once here to learn it. */
static int mpsd__measure_layers(const minipsd_doc *d, uint64_t *sizes, uint64_t *content, uint64_t *info)
{
    size_t i, k;
    *content = 2;                                                  /* the layer count */
    for (i = 0; i < d->layer_count; ++i) {
        *content += 16 + 2 + 4 * 6 + 8 + 4 + 4 + mpsd__layer_extra_size(&d->layers[i]);
        for (k = 0; k < 4; ++k)
            *content += sizes[i * 4 + k] = mpsd__layer_channel_size(d, &d->layers[i], mpsd__channel_of[k]);
    }
    *info = (*content + 1) / 2 * 2;
    return 4 + *info + mpsd__tail_size(d) > MPSD__MAX_SECTION ? MINIPSD_ERR_SIZE : MINIPSD_OK;
}

/* Everything up to the first layer record: the file header, the colour mode data, the image
   resources (resolution, the ICC profile if any, the kept ones), the layer section's lengths and
   the layer count. */
static void mpsd__head(mpsd__sink *s, const minipsd_doc *d, uint64_t info)
{
    const uint32_t res  = mpsd__dpi_fixed(d);
    const uint8_t *kept = mpsd__kept_resolution(d);
    size_t         i;
    mpsd__put(s, "8BPS", 4);
    mpsd__u16(s, 1);                          /* version: PSD        */
    mpsd__zeros(s, 6);
    mpsd__u16(s, 4);                          /* R, G, B, alpha      */
    mpsd__u32(s, d->height);
    mpsd__u32(s, d->width);
    mpsd__u16(s, 8);                          /* bits per channel    */
    mpsd__u16(s, 3);                          /* colour mode: RGB    */

    mpsd__u32(s, 0);                          /* colour mode data    */

    mpsd__u32(s, (uint32_t)mpsd__resources_size(d));
    mpsd__put(s, "8BIM", 4); mpsd__u16(s, 1005); mpsd__u16(s, 0); mpsd__u32(s, 16);
    if (kept) {
        mpsd__put(s, kept, 16);
    } else {
        mpsd__u32(s, res); mpsd__u16(s, 1); mpsd__u16(s, 1);  /* pixels per inch, shown in inches */
        mpsd__u32(s, res); mpsd__u16(s, 1); mpsd__u16(s, 1);
    }
    if (d->icc) {
        mpsd__put(s, "8BIM", 4); mpsd__u16(s, 1039); mpsd__u16(s, 0);
        mpsd__u32(s, (uint32_t)d->icc_size);
        mpsd__put(s, d->icc, d->icc_size);
        mpsd__zeros(s, d->icc_size & 1);
    }
    for (i = 0; i < d->resource_count; ++i) {
        const minipsd_resource *r = &d->resources[i];
        if (!mpsd__kept_resource(r)) continue;
        mpsd__put(s, "8BIM", 4); mpsd__u16(s, r->id);
        mpsd__u8(s, (unsigned)r->name.size);
        mpsd__put(s, r->name.data, r->name.size);
        mpsd__zeros(s, mpsd__pad(1 + r->name.size, 2) - (1 + r->name.size));
        mpsd__u32(s, (uint32_t)r->data.size);
        mpsd__put(s, r->data.data, r->data.size);
        mpsd__zeros(s, r->data.size & 1);
    }

    mpsd__u32(s, (uint32_t)(4 + info + mpsd__tail_size(d)));   /* layer and mask information */
    mpsd__u32(s, (uint32_t)info);
    mpsd__u16(s, (unsigned)(uint16_t)-(int16_t)d->layer_count);   /* negative: composite has alpha */
}

/* After the last layer channel: the layer info's padding, the kept tail or an empty global layer
   mask, and the composite's compression. */
static void mpsd__tail(mpsd__sink *s, const minipsd_doc *d, uint64_t content, uint64_t info)
{
    mpsd__zeros(s, (size_t)(info - content));
    if (d->layer_tail.size) mpsd__put(s, d->layer_tail.data, d->layer_tail.size);
    else                    mpsd__u32(s, 0);
    mpsd__u16(s, d->compression == MINIPSD_RAW ? 0 : 1);
}

/* --- the pull writer: everything in the caller's workspace --- */

/* The file as a sequence of pieces, each small (one record, one row, one count) except the head
   with an ICC profile in it, which is copied, never re-encoded. */
enum {
    MPSD__HEAD,              /* mpsd__head                                    */
    MPSD__RECORD,            /* a layer record                     (layer)    */
    MPSD__COMPRESSION,       /* a layer channel's compression      (layer, k) */
    MPSD__COUNT,             /* a layer row's byte count, RLE      (layer, k, row) */
    MPSD__ROW,               /* a layer row                        (layer, k, row) */
    MPSD__TAIL,              /* mpsd__tail                                    */
    MPSD__COMPOSITE_COUNT,   /* a composite row's byte count, RLE  (k, row)   */
    MPSD__COMPOSITE_ROW,     /* a composite row                    (k, row)   */
    MPSD__DONE
};

struct minipsd_writer {
    const minipsd_doc *doc;
    uint64_t          *channel_sizes;     /* layer_count * 4, PSD order       */
    uint16_t          *composite_sizes;   /* 4 * height, channel-major (RLE)  */
    uint8_t           *composite_row;     /* width * 4                        */
    uint64_t           content, info;     /* the layer info's length, unpadded and padded */
    uint64_t           total;             /* the file size                    */
    /* the cursor */
    int                piece;             /* MPSD__HEAD .. MPSD__DONE         */
    size_t             layer;
    int                channel;           /* 0..3: PSD order in layers, RGBA offset in the composite */
    uint32_t           row;
    uint64_t           offset;            /* bytes of the current piece already handed out */
    uint32_t           composite_y;       /* the row composite_at holds; MPSD__NO_ROW for none */
    const uint8_t     *composite_at;      /* that row: composite_row, or a matted supplied row */
};

#define MPSD__NO_ROW 0xFFFFFFFFu

/* Lays the workspace out and returns the bytes it takes, alignment slack included: the writer,
   the size table, the composite row sizes (RLE only), one composite row.  With a workspace, the
   writer is built at its aligned start and returned through *w. */
static size_t mpsd__writer_layout(const minipsd_doc *d, minipsd_workspace *workspace, minipsd_writer **w)
{
    const size_t self  = mpsd__pad(sizeof(minipsd_writer), MPSD__ALIGN);
    const size_t sizes = d->layer_count * 4 * sizeof(uint64_t);
    const size_t rows  = d->compression == MINIPSD_RAW ? 0 : (size_t)d->height * 4 * sizeof(uint16_t);
    if (workspace) {
        uint8_t *p = (uint8_t *)workspace;
        p += (MPSD__ALIGN - (uintptr_t)p % MPSD__ALIGN) % MPSD__ALIGN;
        *w = (minipsd_writer *)p;
        (*w)->channel_sizes   = (uint64_t *)(p + self);
        (*w)->composite_sizes = (uint16_t *)(p + self + sizes);
        (*w)->composite_row   = p + self + sizes + rows;
    }
    return MPSD__ALIGN - 1 + self + sizes + rows + (size_t)d->width * 4;
}

MINIPSDDEF int minipsd_writer_workspace_size(const minipsd_doc *d, size_t *size)
{
    const int rc = d ? minipsd_doc_check(d) : MINIPSD_ERR_ARG;
    if (rc != MINIPSD_OK) return rc;
    if (!size) return MINIPSD_ERR_ARG;
    *size = mpsd__writer_layout(d, NULL, NULL);
    return MINIPSD_OK;
}

MINIPSDDEF int minipsd_writer_init(minipsd_writer **out, const minipsd_doc *d, minipsd_workspace *workspace,
                                   size_t size)
{
    minipsd_writer *w = NULL;
    mpsd__composite comp;
    uint64_t        total;
    uint32_t        y;
    int             k, rc;
    if (!out) return MINIPSD_ERR_ARG;
    *out = NULL;
    rc = d ? minipsd_doc_check(d) : MINIPSD_ERR_ARG;
    if (rc != MINIPSD_OK) return rc;
    if (!workspace || size < mpsd__writer_layout(d, NULL, NULL)) return MINIPSD_ERR_ARG;
    mpsd__writer_layout(d, workspace, &w);
    w->doc = d;
    rc = mpsd__measure_layers(d, w->channel_sizes, &w->content, &w->info);
    if (rc != MINIPSD_OK) return rc;

    /* Header, colour mode data, resources, layer and mask information, composite compression. */
    total = 26 + 4 + 4 + mpsd__resources_size(d) + 4 + 4 + w->info + mpsd__tail_size(d) + 2;
    if (d->compression == MINIPSD_RAW) {
        total += (uint64_t)d->width * d->height * 4;
    } else {                                  /* each row's count, then the row */
        mpsd__composite_init(&comp, d, w->composite_row);
        for (y = 0; y < d->height; ++y) {
            const uint8_t *row = mpsd__composite_row(&comp, y);
            for (k = 0; k < 4; ++k) {
                const uint64_t n = mpsd__row_size(d, row, k, d->width);
                w->composite_sizes[(size_t)k * d->height + y] = (uint16_t)n;
                total += 2 + n;
            }
        }
    }
    w->total       = total;
    w->piece       = MPSD__HEAD;
    w->layer       = 0;
    w->channel     = 0;
    w->row         = 0;
    w->offset      = 0;
    w->composite_y = MPSD__NO_ROW;
    *out = w;
    return MINIPSD_OK;
}

MINIPSDDEF uint64_t minipsd_writer_total(const minipsd_writer *w)
{
    return w ? w->total : 0;
}

/* A sink callback that keeps bytes [skip, skip + room) of what goes through it, so a piece can
   be produced again from its start and only the part not yet handed out is copied.
   ponytail: a call re-encodes the piece it stops in; a staging buffer would save that if tiny
   caps ever matter. */
typedef struct mpsd__window {
    uint8_t *out;
    uint64_t skip, seen;
    size_t   room, taken;
} mpsd__window;

static int mpsd__window_out(void *user, const void *data, size_t size)
{
    mpsd__window  *win = (mpsd__window *)user;
    const uint8_t *p   = (const uint8_t *)data;
    const uint64_t at  = win->seen;
    win->seen += size;
    if (win->seen <= win->skip) return 1;     /* still before the window */
    if (at < win->skip) { p += win->skip - at; size = (size_t)(win->seen - win->skip); }
    if (size > win->room - win->taken) size = win->room - win->taken;
    memcpy(win->out + win->taken, p, size);
    win->taken += size;
    return win->taken < win->room;            /* full: the rest is only counted */
}

/* The composite row y, built once and kept while pieces of it are produced. */
static const uint8_t *mpsd__writer_composite(minipsd_writer *w, uint32_t y)
{
    if (w->composite_y != y) {
        mpsd__composite c;
        mpsd__composite_init(&c, w->doc, w->composite_row);
        w->composite_at = mpsd__composite_row(&c, y);
        w->composite_y  = y;
    }
    return w->composite_at;
}

/* Produces the current piece, whole, into s. */
static void mpsd__writer_piece(minipsd_writer *w, mpsd__sink *s)
{
    const minipsd_doc   *d = w->doc;
    const minipsd_layer *l = &d->layers[w->layer];   /* always valid: 0 outside the layer section */
    const mpsd__rect     r = mpsd__layer_rect(d, l);
    const uint8_t       *row = r.height ? l->rgba + (size_t)w->row * r.width * 4 : NULL;
    switch (w->piece) {
    case MPSD__HEAD:            mpsd__head(s, d, w->info); break;
    case MPSD__RECORD:          mpsd__layer_record(s, d, l, w->channel_sizes + w->layer * 4); break;
    case MPSD__COMPRESSION:     mpsd__u16(s, d->compression == MINIPSD_RAW ? 0 : 1); break;
    case MPSD__COUNT:           mpsd__u16(s, (unsigned)mpsd__row_size(d, row, mpsd__channel_of[w->channel], r.width)); break;
    case MPSD__ROW:             mpsd__row(s, d, row, mpsd__channel_of[w->channel], r.width); break;
    case MPSD__TAIL:            mpsd__tail(s, d, w->content, w->info); break;
    case MPSD__COMPOSITE_COUNT: mpsd__u16(s, w->composite_sizes[(size_t)w->channel * d->height + w->row]); break;
    default:                    mpsd__row(s, d, mpsd__writer_composite(w, w->row), w->channel, d->width); break;
    }
}

/* Moves the cursor to the piece after the current one, in file order. */
static void mpsd__writer_next(minipsd_writer *w)
{
    const minipsd_doc *d    = w->doc;
    const int          rle  = d->compression != MINIPSD_RAW;
    const uint32_t     rows = mpsd__layer_rect(d, &d->layers[w->layer]).height;   /* the layer's */
    w->offset = 0;
    switch (w->piece) {
    case MPSD__HEAD:
        w->piece = MPSD__RECORD;
        return;
    case MPSD__RECORD:
        if (++w->layer < d->layer_count) return;
        w->layer = 0;
        w->piece = MPSD__COMPRESSION;
        return;
    case MPSD__COMPRESSION:                   /* rows follow, unless the layer is empty */
        if (rows) { w->piece = rle ? MPSD__COUNT : MPSD__ROW; return; }
        break;
    case MPSD__COUNT:
        if (++w->row < rows) return;
        w->row   = 0;
        w->piece = MPSD__ROW;
        return;
    case MPSD__ROW:
        if (++w->row < rows) return;
        w->row = 0;
        break;
    case MPSD__TAIL:
        w->piece = rle ? MPSD__COMPOSITE_COUNT : MPSD__COMPOSITE_ROW;
        return;
    default:                                  /* the composite: channel by channel, row by row */
        if (++w->row < d->height) return;
        w->row = 0;
        if (++w->channel < 4) return;
        w->channel = 0;
        w->piece   = w->piece == MPSD__COMPOSITE_COUNT ? MPSD__COMPOSITE_ROW : MPSD__DONE;
        return;
    }
    /* A layer channel is complete: the next channel, the next layer, or the tail. */
    w->piece = MPSD__COMPRESSION;
    if (++w->channel < 4) return;
    w->channel = 0;
    if (++w->layer < d->layer_count) return;
    w->layer = 0;
    w->piece = MPSD__TAIL;
}

MINIPSDDEF int minipsd_writer_fill(minipsd_writer *w, void *buf, size_t cap, size_t *produced)
{
    size_t n = 0;
    if (produced) *produced = 0;
    if (!w || !produced || (!buf && cap)) return MINIPSD_ERR_ARG;
    while (w->piece != MPSD__DONE && n < cap) {
        mpsd__window win;
        mpsd__sink   s;
        win.out = (uint8_t *)buf + n; win.skip = w->offset; win.seen = 0; win.room = cap - n; win.taken = 0;
        s.fn = mpsd__window_out; s.user = &win; s.count = 0; s.failed = 0;
        mpsd__writer_piece(w, &s);
        n         += win.taken;
        w->offset += win.taken;
        if (w->offset < s.count) break;       /* buf is full inside this piece */
        mpsd__writer_next(w);
    }
    *produced = n;
    return w->piece == MPSD__DONE ? MINIPSD_OK : MINIPSD_MORE;
}

#ifndef MINIPSD_NO_MALLOC

/* --- the push writer: the pull writer driven to the end on an allocated workspace --- */

#define MPSD__CHUNK 4096   /* bytes per callback; on the stack */

MINIPSDDEF int minipsd_write(const minipsd_doc *d, minipsd_write_fn fn, void *user)
{
    minipsd_writer *w;
    void           *workspace;
    uint8_t         chunk[MPSD__CHUNK];
    size_t          size = 0, n;
    int             rc = d ? minipsd_writer_workspace_size(d, &size) : MINIPSD_ERR_ARG;   /* d spelled out for static analysers */
    if (rc != MINIPSD_OK) return rc;
    if (!fn) return MINIPSD_ERR_ARG;
    workspace = MINIPSD_MALLOC(size);
    if (!workspace) return MINIPSD_ERR_ALLOC;
    rc = minipsd_writer_init(&w, d, workspace, size);
    if (rc == MINIPSD_OK) {
        do {   /* never n == 0: the call that produces the last byte already returns MINIPSD_OK */
            rc = minipsd_writer_fill(w, chunk, sizeof chunk, &n);
            if (!fn(user, chunk, n)) rc = MINIPSD_ERR_WRITE;
        } while (rc == MINIPSD_MORE);
    }
    MINIPSD_FREE(workspace);
    return rc;
}

#endif /* MINIPSD_NO_MALLOC */

#endif /* MPSD__WRITER_DONE */
