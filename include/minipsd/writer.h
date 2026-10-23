/**
 * \file minipsd/writer.h
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
#include "detail/format.h"
#include "detail/samples.h"
#include "detail/utf.h"
#include "detail/packbits.h"
#include "detail/composite.h"

/* The document's format: what its kept bytes are laid out as, 1 for a PSB (detail/format.h). */
static unsigned mpsd__doc_psb(const minipsd_doc *d) { return d->format == MINIPSD_PSB; }


/* The format written: opts->format, or else the document's own; 1 for a PSB. */
static unsigned mpsd__out_psb(const minipsd_doc *d, const minipsd_write_opts *opts)
{
    return opts && opts->format ? opts->format == MINIPSD_PSB : mpsd__doc_psb(d);
}

/* --- channel rows: PackBits or raw --- */

/* A row of n bytes as the file stores it, out of samples stride bytes apart (see mpsd__packbits). */
static void mpsd__raw(mpsd__sink *s, const uint8_t *src, size_t n, size_t stride, unsigned wide)
{
    uint8_t chunk[256];
    size_t  i = 0;
    while (i < n) {
        size_t len = 0;
        while (i < n && len < sizeof chunk) chunk[len++] = src[mpsd__at(i++, (ptrdiff_t)stride, wide)];
        mpsd__put(s, chunk, len);
    }
}

/* One row of one channel, width samples of the document's depth, step bytes apart; at 1 bit
   already packed, a byte apart. */
static void mpsd__row(mpsd__sink *s, const minipsd_doc *d, const uint8_t *row, size_t step, uint32_t width)
{
    const unsigned wide = mpsd__doc_shift(d);
    const size_t   n    = (size_t)mpsd__row_bytes(width, mpsd__doc_depth(d));
    if (d->compression == MINIPSD_RAW) mpsd__raw(s, row, n, step, wide);
    else                               mpsd__packbits(s, row, n, step, wide);
}

/* The compression marker a channel the writer encodes starts with. */
static unsigned mpsd__marker(const minipsd_doc *d) { return d->compression == MINIPSD_RAW ? MPSD__COMP_RAW : MPSD__COMP_RLE; }

static uint64_t mpsd__row_size(const minipsd_doc *d, const uint8_t *row, size_t step, uint32_t width)
{
    mpsd__sink c = { NULL, NULL, 0, 0 };
    mpsd__row(&c, d, row, step, width);
    return c.count;
}

/* A layer's channels in the order written, k: transparency (id -1), the colour channels (ids 0
   on), then the mask (-2) and the real mask (-3) when there are.  The mask's k follows the pixel's
   channels. */
static int mpsd__ch_mask(const minipsd_doc *d) { return (int)mpsd__doc_pixel(d); }

static int mpsd__channel_id(const minipsd_doc *d, int k)
{
    const int mask = mpsd__ch_mask(d);
    return k == 0 ? -1 : k < mask ? k - 1 : k == mask ? -2 : -3;
}

/* Where pixel channel k lies in a pixel, and in stored (as minipsd_layer_view.channel): the colour
   channels first, transparency last. */
static int mpsd__channel_of(const minipsd_doc *d, int k) { return k ? k - 1 : (int)mpsd__doc_colors(d); }

/* The channels written are [mpsd__first_channel, mpsd__channel_count) of that order: a stored
   layer without transparency, as its file had it, starts at the first colour channel. */
static int mpsd__channel_count(const minipsd_doc *d, const minipsd_layer *l)
{
    return mpsd__ch_mask(d) + (l->mask ? 1 + (l->mask->real.size != 0) : 0);
}

static int mpsd__first_channel(const minipsd_doc *d, const minipsd_layer *l)
{
    return l->stored && !l->stored[mpsd__doc_colors(d)].size;
}

/* Where channel k of a layer comes from: bytes to copy as they are, or samples to encode, step
   bytes apart, width by height, of the document's depth.  A stored channel's height is its
   rectangle's: the rows of an RLE count table. */
typedef struct mpsd__source {
    const minipsd_bytes *stored;
    const uint8_t       *samples;
    size_t               step;
    uint32_t             width, height;
} mpsd__source;

/* The real mask's height.  Its rectangle follows the real flags and background colour at the start
   of the rest of the mask data, as psd-tools reads Photoshop's files (Adobe's specification puts
   the mask parameters first); 0 when the rest is too short to hold one. */
static uint32_t mpsd__real_rows(const minipsd_mask *m)
{
    int64_t rows;
    if (m->rest.size < MPSD__REAL_RECT + MPSD__RECT_SIZE) return 0;
    rows = mpsd__rect_height(m->rest.data + MPSD__REAL_RECT);
    return rows > 0 ? (uint32_t)rows : 0;
}

static mpsd__source mpsd__channel_source(const minipsd_doc *d, const minipsd_layer *l, int k)
{
    const unsigned wide = mpsd__doc_shift(d);
    mpsd__source src;
    src.stored  = NULL;
    src.samples = NULL;
    src.step    = (size_t)1 << wide;
    src.width   = src.height = 0;
    if (k < mpsd__ch_mask(d)) {
        static const uint8_t full[2] = { MPSD__FULL_8, MPSD__FULL_8 };   /* 255, or 65535 at 16 bits */
        const mpsd__rect r  = mpsd__layer_rect(d, l);
        const int        at = mpsd__channel_of(d, k);
        src.height = r.height;
        if (l->stored && l->stored[at].size) { src.stored = &l->stored[at]; return src; }
        if (l->stored) {                                      /* no transparency stored: never written (mpsd__first_channel) */
            src.samples = r.height ? full : NULL;
            src.step    = 0;
        } else {
            src.samples = r.height ? l->pixels + ((size_t)at << wide) : NULL;
            src.step    = (size_t)mpsd__doc_pixel(d) << wide;
        }
        src.width   = r.width;
        src.height  = r.height;
    } else if (k == mpsd__ch_mask(d)) {
        const minipsd_mask *m = l->mask;
        const uint32_t      w = (uint32_t)((int64_t)m->right - m->left), h = (uint32_t)((int64_t)m->bottom - m->top);
        src.height = h;
        if (m->stored.size) { src.stored = &m->stored; return src; }
        if (m->pixels && w && h) { src.samples = m->pixels; src.width = w; }
        else                      src.height = 0;
    } else {
        src.stored = &l->mask->real;
        src.height = mpsd__real_rows(l->mask);
    }
    return src;
}

/* Is the source a stored RLE channel whose row count table changes width, the formats differing? */
static int mpsd__retable(const mpsd__source *src, unsigned from, unsigned out)
{
    return from != out && src->stored && src->stored->size >= MPSD__MARKER_SIZE
        && mpsd__get16(src->stored->data) == MPSD__COMP_RLE;
}

/* What a stored source takes in the out format: its bytes, its count table re-written when the
   formats differ. */
static uint64_t mpsd__stored_size(const mpsd__source *src, unsigned from, unsigned out)
{
    if (!mpsd__retable(src, from, out)) return src->stored->size;
    return src->stored->size - (uint64_t)src->height * mpsd__count_size(from) + (uint64_t)src->height * mpsd__count_size(out);
}

/* Does the document compress its layers' and masks' channels with the caller's deflate? */
static int mpsd__zip(const minipsd_doc *d) { return d->compression == MINIPSD_ZIP || d->compression == MINIPSD_ZIP_PREDICTED; }

/* The largest channel the writer deflates, raw: its bytes, 0 for none. */
static uint64_t mpsd__zip_plane(const minipsd_doc *d)
{
    uint64_t max = 0;
    size_t   i;
    int      k;
    for (i = 0; mpsd__zip(d) && i < d->layer_count; ++i)
        for (k = 0; k <= mpsd__ch_mask(d) && k < mpsd__channel_count(d, &d->layers[i]); ++k) {
            const mpsd__source src = mpsd__channel_source(d, &d->layers[i], k);
            const uint64_t     n   = src.samples ? (uint64_t)src.width * src.height << mpsd__doc_shift(d) : 0;
            if (n > max) max = n;
        }
    return max;
}

/* The room a deflate gets for n bytes: more than zlib's compressBound, and than stored blocks take. */
static uint64_t mpsd__deflate_room(uint64_t n) { return n + (n >> 11) + 16; }

/* Is the composite copied as the file stored it?  Only when the caller gives none of their own. */
static int mpsd__copies_composite(const minipsd_doc *d) { return !d->composite && d->composite_stored.size; }

/* The composite's channels in the file: the stored one's, or R, G, B and transparency. */
static unsigned mpsd__composite_channels(const minipsd_doc *d)
{
    return mpsd__copies_composite(d) ? d->composite_channels : mpsd__doc_pixel(d);
}

/* Is the composite's fourth channel transparency, the layer count negative?  Always for a
   supplied or composed one. */
static int mpsd__composite_alpha(const minipsd_doc *d) { return !mpsd__copies_composite(d) || d->composite_alpha; }

/* No source: the pieces outside a layer channel and the composite. */
static mpsd__source mpsd__no_source(void)
{
    mpsd__source src;
    src.stored  = NULL;
    src.samples = NULL;
    src.step    = 0;
    src.width   = src.height = 0;
    return src;
}

/* The stored composite as a source: one marker, then its channels' rows one after another, a
   single count table over all of them. */
static mpsd__source mpsd__composite_source(const minipsd_doc *d)
{
    mpsd__source src;
    src.stored  = &d->composite_stored;
    src.samples = NULL;
    src.step    = 0;
    src.width   = d->width;
    src.height  = (uint32_t)d->composite_channels * d->height;
    return src;
}

/* --- names: a legacy ASCII Pascal string and the UTF-16 one --- */

static void mpsd__unit_out(void *ctx, unsigned u) { mpsd__u16((mpsd__sink *)ctx, u); }

/* The section type an 'lsct' block holds; -1 for any other block, or one too short to say. */
static int mpsd__lsct_type(const minipsd_block *block)
{
    if (!mpsd__tag((const uint8_t *)block->key, "lsct") || block->data.size < MPSD__LSCT_TYPE) return -1;
    return (int)mpsd__get32(block->data.data);
}

/* Is there a kept 'lsct' of the layer's own type?  It is then written as it is, its group mode
   and sub-type with it, in place of the writer's own. */
static int mpsd__kept_section(minipsd_bytes extra, unsigned kind, unsigned psb)
{
    minipsd_block block;
    size_t        pos = 0, next;
    for (; pos < extra.size && mpsd__block(extra, pos, psb, &block, &next) == MINIPSD_OK; pos = next)
        if (mpsd__lsct_type(&block) == (int)kind) return 1;
    return 0;
}

/* Is there a kept 'luni'?  The first one's data in *data. */
static int mpsd__kept_luni(minipsd_bytes extra, unsigned psb, minipsd_bytes *data)
{
    minipsd_block block;
    size_t        pos = 0, next;
    for (; pos < extra.size && mpsd__block(extra, pos, psb, &block, &next) == MINIPSD_OK; pos = next)
        if (mpsd__tag((const uint8_t *)block.key, "luni")) { *data = block.data; return 1; }
    return 0;
}

/* The writer's own blocks, first: the name in 'luni' (the kept one, as it is, when the kept names
   say it), then the group marker unless a kept one says it. */
static void mpsd__own_blocks(mpsd__sink *s, const minipsd_layer *l, unsigned from, int named)
{
    const char   *name = l->name;
    minipsd_bytes kept;
    if (named && mpsd__kept_luni(l->additional_info, from, &kept)) {
        mpsd__put(s, "8BIMluni", 8);
        mpsd__u32(s, (uint32_t)kept.size);
        mpsd__put(s, kept.data, kept.size);
    } else if (!named) {
        const size_t units = mpsd__utf16(name, NULL, NULL);
        const size_t luni  = MPSD__LEN + units * 2;   /* the count of UTF-16 units, the units */
        mpsd__put(s, "8BIMluni", 8);
        mpsd__u32(s, (uint32_t)mpsd__pad(luni, 4));
        mpsd__u32(s, (uint32_t)units);
        mpsd__utf16(name, mpsd__unit_out, s);
        mpsd__zeros(s, mpsd__pad(luni, 4) - luni);
    }
    if (l->section_divider_type && !mpsd__kept_section(l->additional_info, l->section_divider_type, from)) {   /* the group marker, unless kept */
        const char *mode = l->blend_mode[0] || l->blend_mode[1] || l->blend_mode[2] || l->blend_mode[3] ? l->blend_mode : "pass";
        mpsd__put(s, "8BIMlsct", 8);
        mpsd__u32(s, l->section_divider_type == MINIPSD_SECTION_GROUP_END ? MPSD__LSCT_TYPE : MPSD__LSCT_FULL);
        mpsd__u32(s, l->section_divider_type);
        if (l->section_divider_type != MINIPSD_SECTION_GROUP_END) { mpsd__put(s, "8BIM", 4); mpsd__put(s, mode, 4); }
    }
}

/* Do the names kept from a file (a 'luni' in extra, else name_pascal read as Latin-1) say the
   layer's name, as minipsd_read made it?  Then they are written as they are. */
static int mpsd__kept_names(const minipsd_layer *l, unsigned psb)
{
    minipsd_bytes luni;
    if (mpsd__kept_luni(l->additional_info, psb, &luni)) {
        const size_t units = luni.size >= MPSD__LEN ? mpsd__get32(luni.data) : 0;
        return luni.size >= MPSD__LEN && units <= (luni.size - MPSD__LEN) / 2
            && mpsd__same_name(l->name, luni.data + MPSD__LEN, units * 2, 0);
    }
    return l->name_pascal.size && mpsd__same_name(l->name, l->name_pascal.data, l->name_pascal.size, 1);
}

/* Blocks kept from a file, laid out as the from format lays them out, written in the out format,
   after the writer's own, but where the layer's own fields win: 'luni' (mpsd__own_blocks wrote the
   name) and an 'lsct' of another type than the layer's go.  minipsd_doc_check has walked them, and
   checked they can change format. */
static void mpsd__kept_blocks(mpsd__sink *s, const minipsd_layer *l, unsigned from, unsigned out)
{
    const minipsd_bytes extra = l->additional_info;
    minipsd_block block;
    size_t        pos = 0, next;
    while (pos < extra.size && mpsd__block(extra, pos, from, &block, &next) == MINIPSD_OK) {
        const int type = mpsd__lsct_type(&block);
        if (!mpsd__tag((const uint8_t *)block.key, "luni") && (type < 0 || type == (int)l->section_divider_type)) {
            if (from == out) mpsd__put(s, extra.data + pos, next - pos);
            else mpsd__put_block(s, extra.data + pos, (size_t)(block.data.data - (extra.data + pos)), block.data.size, block.data.size, out);
        }
        pos = next;
    }
}

/* The layer mask data: the rectangle, colour and flags, then what was kept, or the padding a
   mask without more takes. */
static void mpsd__mask_data(mpsd__sink *s, const minipsd_mask *m)
{
    if (!m) { mpsd__u32(s, 0); return; }
    mpsd__u32(s, (uint32_t)(MPSD__MASK_HEAD + (m->rest.size ? m->rest.size : MPSD__MASK_PAD)));
    mpsd__u32(s, (uint32_t)m->top);    mpsd__u32(s, (uint32_t)m->left);
    mpsd__u32(s, (uint32_t)m->bottom); mpsd__u32(s, (uint32_t)m->right);
    mpsd__u8(s, m->default_color);
    mpsd__u8(s, m->flags);
    if (m->rest.size) mpsd__put(s, m->rest.data, m->rest.size);
    else              mpsd__zeros(s, MPSD__MASK_PAD);
}

/* The blending ranges, as given: their length, then each range as detail/format.h lays it out. */
static void mpsd__blending_ranges(mpsd__sink *s, const minipsd_layer *l)
{
    uint32_t i;
    mpsd__u32(s, l->blending_range_count * MPSD__RANGE_SIZE);   /* minipsd_doc_check bounds the count */
    for (i = 0; i < l->blending_range_count; ++i) {
        const minipsd_blending_range *r = &l->blending_ranges[i];
        mpsd__put(s, r->source.black, 2);      mpsd__put(s, r->source.white, 2);
        mpsd__put(s, r->destination.black, 2); mpsd__put(s, r->destination.white, 2);
    }
}

/* The extra-data block of a layer record: the mask, the blending ranges, the legacy name, then
   the blocks: the writer's own ('luni', the group marker), then the kept ones in their order (laid
   out as the from format lays them out, written in the out format). */
static void mpsd__layer_extra(mpsd__sink *s, const minipsd_layer *l, unsigned from, unsigned out)
{
    const char          *name = l->name;
    const unsigned char *p = (const unsigned char *)(name ? name : "");
    const int            named = mpsd__kept_names(l, from);
    size_t ascii = 0;
    uint8_t pascal[256];
    if (named) {                              /* the kept legacy name, as it is */
        ascii = l->name_pascal.size;
        if (ascii) memcpy(pascal + 1, l->name_pascal.data, ascii);
    } else {
        while (*p && ascii < 255) {           /* one '?' per non-ASCII character */
            const uint32_t cp = mpsd__utf8(&p);
            pascal[1 + ascii++] = (uint8_t)(cp < 0x80 ? cp : '?');
        }
    }
    pascal[0] = (uint8_t)ascii;
    mpsd__mask_data(s, l->mask);
    mpsd__blending_ranges(s, l);
    mpsd__put(s, pascal, ascii + 1);
    mpsd__zeros(s, mpsd__pad(ascii + 1, 4) - (ascii + 1));
    mpsd__own_blocks(s, l, from, named);
    mpsd__kept_blocks(s, l, from, out);
}


/* --- the layer section --- */

/* Tests count the layers whose pixels are encoded by defining this before the implementation. */
#ifndef MPSD__ENCODED
#  define MPSD__ENCODED(layer) ((void)0)
#endif

/* What layer channel k's data takes in the out format: its compression, then (RLE) the row byte
   counts, two bytes each or four in a PSB, then the rows; or the stored channel, its count table
   re-written when the formats differ. */
static uint64_t mpsd__layer_channel_size(const minipsd_doc *d, const minipsd_layer *l, int k, unsigned out)
{
    const mpsd__source src = mpsd__channel_source(d, l, k);
    const unsigned     from = mpsd__doc_psb(d);
    uint64_t size = MPSD__MARKER_SIZE;
    uint32_t y;
    if (src.stored) return mpsd__stored_size(&src, from, out);
    if (!src.samples) return size;
    MPSD__ENCODED(l);
    if (d->compression == MINIPSD_RAW) return size + ((uint64_t)src.width * src.height << mpsd__doc_shift(d));
    for (y = 0; y < src.height; ++y)
        size += mpsd__count_size(out) + mpsd__row_size(d, src.samples + (size_t)y * src.width * src.step, src.step, src.width);
    return size;
}

static uint64_t mpsd__layer_extra_size(const minipsd_layer *l, unsigned from, unsigned out)
{
    mpsd__sink c = { NULL, NULL, 0, 0 };
    mpsd__layer_extra(&c, l, from, out);
    return c.count;
}

/* One layer record.  sizes holds its channels' sizes (written order), from
   mpsd__layer_channel_size. */
static void mpsd__layer_record(mpsd__sink *s, const minipsd_doc *d, const minipsd_layer *l, const uint64_t *sizes, unsigned out)
{
    const mpsd__rect r = mpsd__layer_rect(d, l);
    const int        n = mpsd__channel_count(d, l);
    int k;
    mpsd__u32(s, (uint32_t)r.top);    mpsd__u32(s, (uint32_t)r.left);
    mpsd__u32(s, (uint32_t)r.bottom); mpsd__u32(s, (uint32_t)r.right);
    mpsd__u16(s, (unsigned)(n - mpsd__first_channel(d, l)));
    for (k = mpsd__first_channel(d, l); k < n; ++k) {
        mpsd__u16(s, (unsigned)(uint16_t)(int16_t)mpsd__channel_id(d, k));
        mpsd__putlen(s, sizes[k], out);
    }
    mpsd__put(s, "8BIM", 4);
    mpsd__put(s, l->blend_mode[0] || l->blend_mode[1] || l->blend_mode[2] || l->blend_mode[3] ? l->blend_mode : "norm", 4);
    mpsd__u8(s, l->opacity);
    mpsd__u8(s, l->clipping);
    mpsd__u8(s, (l->flags & ~MPSD__FLAG_HIDDEN) | (l->hidden ? MPSD__FLAG_HIDDEN : 0x00u));
    mpsd__u8(s, 0);                                               /* filler          */
    mpsd__u32(s, (uint32_t)mpsd__layer_extra_size(l, mpsd__doc_psb(d), out));   /* MPSD__LEN in a PSB too */
    mpsd__layer_extra(s, l, mpsd__doc_psb(d), out);
}

/* --- the file --- */

/* PSD stores section lengths in 32 bits, as both formats do a record's extra data and the
   resources; a PSB's layer and mask section takes 64. */
#define MPSD__MAX_SECTION 0xFFFFFFFFu

static uint64_t mpsd__max_layers_section(unsigned psb) { return psb ? MPSD__FAR - 1 : MPSD__MAX_SECTION; }

/* A kept resource is written unless the document's own field says the same: dpi, icc. */
static int mpsd__kept_resource(const minipsd_resource *r) { return r->id != MINIPSD_RES_RESOLUTION_INFO && r->id != MINIPSD_RES_ICC_PROFILE; }

/* The dpi as stored: 16.16 fixed point. */
static uint32_t mpsd__dpi_fixed(const minipsd_doc *d) { return (uint32_t)(d->dpi * MPSD__FIXED_ONE + 0.5); }

/* A kept resolution that says the document's dpi, with its units and vertical resolution:
   written in place of the writer's own, which shows inches.  NULL when there is none. */
static const uint8_t *mpsd__kept_resolution(const minipsd_doc *d)
{
    size_t i;
    for (i = 0; i < d->resource_count; ++i) {
        const minipsd_resource *r = &d->resources[i];
        if (r->id == MINIPSD_RES_RESOLUTION_INFO)
            return r->data.size == MPSD__RESOLUTION_SIZE && r->data.data && mpsd__get32(r->data.data) == mpsd__dpi_fixed(d) ? r->data.data : NULL;
    }
    return NULL;
}

/* The first kept resource with this id; NULL when there is none: its name goes on the writer's own. */
static const minipsd_resource *mpsd__find_resource(const minipsd_doc *d, unsigned id)
{
    size_t i;
    for (i = 0; i < d->resource_count; ++i)
        if (d->resources[i].id == id) return &d->resources[i];
    return NULL;
}

/* One resource, laid out as detail/format.h shows: a Pascal name padded to even, the data too. */
static void mpsd__put_resource(mpsd__sink *s, unsigned id, minipsd_bytes name, const void *data, size_t size)
{
    mpsd__put(s, "8BIM", 4); mpsd__u16(s, id);
    mpsd__u8(s, (unsigned)name.size);
    mpsd__put(s, name.data, name.size);
    mpsd__zeros(s, mpsd__pad(1 + name.size, 2) - (1 + name.size));
    mpsd__u32(s, (uint32_t)size);
    mpsd__put(s, data, size);
    mpsd__zeros(s, size & 1);
}

/* The resolution: a kept one that says the document's dpi, or the writer's own, in inches. */
static void mpsd__resolution(mpsd__sink *s, const minipsd_doc *d, minipsd_bytes name)
{
    const uint32_t res  = mpsd__dpi_fixed(d);
    const uint8_t *kept = mpsd__kept_resolution(d);
    uint8_t        own[MPSD__RESOLUTION_SIZE] = { 0 };
    uint8_t       *p;
    for (p = own; p < own + MPSD__RESOLUTION_SIZE; p += 8) {   /* horizontal, vertical: pixels per inch, shown in inches */
        p[0] = (uint8_t)(res >> 24); p[1] = (uint8_t)(res >> 16); p[2] = (uint8_t)(res >> 8); p[3] = (uint8_t)res;
        p[5] = p[7] = 1;
    }
    mpsd__put_resource(s, MINIPSD_RES_RESOLUTION_INFO, name, kept ? kept : own, MPSD__RESOLUTION_SIZE);
}

/* The image resources: the writer's own first (the resolution, the ICC profile if any), then the
   kept ones.  A kept resolution or profile goes: the document's field says it. */
static void mpsd__resources(mpsd__sink *s, const minipsd_doc *d)
{
    const minipsd_resource *res = mpsd__find_resource(d, MINIPSD_RES_RESOLUTION_INFO);
    const minipsd_resource *icc = mpsd__find_resource(d, MINIPSD_RES_ICC_PROFILE);
    const minipsd_bytes     none = { NULL, 0 };
    size_t                  i;
    mpsd__resolution(s, d, res ? res->name : none);
    if (d->icc) mpsd__put_resource(s, MINIPSD_RES_ICC_PROFILE, icc ? icc->name : none, d->icc, d->icc_size);
    for (i = 0; i < d->resource_count; ++i)
        if (mpsd__kept_resource(&d->resources[i]))
            mpsd__put_resource(s, d->resources[i].id, d->resources[i].name, d->resources[i].data.data, d->resources[i].data.size);
}

static uint64_t mpsd__resources_size(const minipsd_doc *d)
{
    mpsd__sink c = { NULL, NULL, 0, 0 };
    mpsd__resources(&c, d);
    return c.count;
}

/* A stored channel that fits a width x height rectangle of one-byte or (wide) two-byte samples:
   the compression marker, then raw rows, or (RLE) a byte count per row (four bytes in a PSB) and
   rows no longer than they add up to.  ZIP passes as it is. */
static int mpsd__stored_ok(minipsd_bytes c, uint32_t width, uint32_t height, unsigned wide, unsigned psb)
{
    const uint64_t counts = (uint64_t)height * mpsd__count_size(psb);
    uint64_t       left, sum = 0;
    uint32_t       y;
    if (!c.data || c.size < MPSD__MARKER_SIZE) return 0;
    left = c.size - MPSD__MARKER_SIZE;
    switch (mpsd__get16(c.data)) {
    case MPSD__COMP_RAW:
        return ((uint64_t)width * height << wide) <= left;
    case MPSD__COMP_RLE:
        if (counts > left) return 0;
        for (y = 0; y < height; ++y) sum += mpsd__count(c.data + MPSD__MARKER_SIZE, y, psb);
        return sum <= left - counts;
    case MPSD__COMP_ZIP: case MPSD__COMP_ZIP_PREDICTED:
        return 1;
    default:
        return 0;
    }
}

/* Where an 'Lr16' (or 'Lr32') block starts in the kept tail, and where it ends (padded); both the tail's size
   when there is none.  Such a block says where the layers go, as Photoshop keeps its 16-bit files'
   layers: the writer writes them there and leaves the layer info empty.  What it held goes: the
   document's layers win. */
static size_t mpsd__layers_block(const minipsd_doc *d, size_t *end)
{
    const minipsd_bytes t   = d->layer_tail;
    const unsigned      psb = mpsd__doc_psb(d);
    size_t              at;
    *end = t.size;
    if (t.size < MPSD__LEN || mpsd__get32(t.data) > t.size - MPSD__LEN) return t.size;
    for (at = MPSD__LEN + mpsd__get32(t.data); at < t.size;) {
        const uint8_t *p    = t.data + at;
        const size_t   left = t.size - at;
        size_t         head, span;
        uint64_t       size;
        if (left < MPSD__BLOCK_HEAD || !(mpsd__tag(p, "8BIM") || mpsd__tag(p, "8B64"))) return t.size;
        head = mpsd__block_head(p, psb);
        if (left < head) return t.size;
        size = mpsd__getlen(p + MPSD__BLOCK_LEN, head > MPSD__BLOCK_HEAD);
        if (size > left - head) return t.size;
        span = mpsd__pad((size_t)size, 4) < left - head ? mpsd__pad((size_t)size, 4) : left - head;
        if (mpsd__tag(p + MPSD__BLOCK_KEY, "Lr16") || mpsd__tag(p + MPSD__BLOCK_KEY, "Lr32")) { *end = at + head + span; return at; }
        at += head + span;
    }
    return t.size;
}

/* What follows the layer info in its section, [lo, hi) of the kept tail (lo and hi at block
   bounds), or an empty global layer mask.  A tail going into the other format is the global layer
   mask as it is, then blocks, each padded to four bytes past its length, with their lengths
   re-written (a tail too short for the mask's length is only bytes, copied as they are):
   MINIPSD_ERR_UNSUPPORTED when it is not blocks to its end or holds one that cannot change format,
   MINIPSD_ERR_SIZE when a PSB's block is too long for a PSD.  The last block may end without its
   padding. */
static int mpsd__tail_range(mpsd__sink *s, const minipsd_doc *d, size_t lo, size_t hi, unsigned out)
{
    const minipsd_bytes t    = d->layer_tail;
    const unsigned      from = mpsd__doc_psb(d);
    size_t              at;
    if (!t.size) { mpsd__u32(s, 0); return MINIPSD_OK; }   /* an empty global layer mask */
    if (from == out || t.size < MPSD__LEN) { mpsd__put(s, t.data + lo, hi - lo); return MINIPSD_OK; }   /* too short to hold a length */
    if (mpsd__get32(t.data) > t.size - MPSD__LEN) return MINIPSD_ERR_UNSUPPORTED;
    at = MPSD__LEN + mpsd__get32(t.data);
    if (lo) at = lo;
    else    mpsd__put(s, t.data, at);
    while (at < hi) {
        const uint8_t *p    = t.data + at;
        const size_t   left = t.size - at;
        size_t         head, span;
        uint64_t       size;
        if (left < MPSD__BLOCK_HEAD || !(mpsd__tag(p, "8BIM") || mpsd__tag(p, "8B64"))) return MINIPSD_ERR_UNSUPPORTED;
        head = mpsd__block_head(p, from);
        if (left < head || !mpsd__convertible(p + MPSD__BLOCK_KEY)) return MINIPSD_ERR_UNSUPPORTED;
        size = mpsd__getlen(p + MPSD__BLOCK_LEN, head > MPSD__BLOCK_HEAD);
        if (size > left - head) return MINIPSD_ERR_UNSUPPORTED;
        if (!out && size > MPSD__MAX_SECTION) return MINIPSD_ERR_SIZE;
        span = mpsd__pad((size_t)size, 4) < left - head ? mpsd__pad((size_t)size, 4) : left - head;
        mpsd__put_block(s, p, head, size, span, out);
        at += head + span;
    }
    return MINIPSD_OK;
}

/* Does the writer put the layers in a block of its own, 'Lr32', the layer info left empty, as
   Photoshop keeps a 32-bit file's?  For a 32-bit document whose kept tail has no layers' block: after
   the tail, with an empty 'Mt32' before it when the composite has transparency (the layer count in
   the block is then positive, as Photoshop writes it). */
static int mpsd__own_layers_block(const minipsd_doc *d)
{
    size_t end;
    return mpsd__doc_depth(d) == MPSD__CH_32BIT && d->layer_count && mpsd__layers_block(d, &end) == d->layer_tail.size;
}

/* A block of the writer's own, of no data or of size bytes that follow it: '8BIM', or in a PSB
   '8B64' as Photoshop marks the long keys these are, the key, the length. */
static void mpsd__own_block(mpsd__sink *s, const char *key, uint64_t size, unsigned out)
{
    mpsd__put(s, out ? "8B64" : "8BIM", 4);
    mpsd__put(s, key, 4);
    mpsd__putlen(s, size, out);
}

/* The kept tail whole, but for the layers' block. */
static int mpsd__tail_out(mpsd__sink *s, const minipsd_doc *d, unsigned out)
{
    size_t    end;
    const size_t at = mpsd__layers_block(d, &end);
    const int rc = mpsd__tail_range(s, d, 0, at, out);
    return rc != MINIPSD_OK || at == end ? rc : mpsd__tail_range(s, d, end, d->layer_tail.size, out);
}

/* The layer and mask section's length: the layer info's length, the layer info (info bytes, here
   or in the layers' block), the tail; the block's header and padding with it. */
static uint64_t mpsd__section_size(const minipsd_doc *d, uint64_t info, unsigned out)
{
    mpsd__sink c = { NULL, NULL, 0, 0 };
    size_t     end;
    (void)mpsd__tail_out(&c, d, out);
    if (mpsd__layers_block(d, &end) < d->layer_tail.size || mpsd__own_layers_block(d))
        c.count += MPSD__BLOCK_LEN + mpsd__len_size(out) + (mpsd__pad((size_t)info, 4) - info);   /* '8BIM', 'Lr16', length; padding */
    if (mpsd__own_layers_block(d) && mpsd__composite_alpha(d)) c.count += MPSD__BLOCK_LEN + mpsd__len_size(out);   /* 'Mt32' */
    return mpsd__len_size(out) + info + c.count;
}

/* Pointers with sizes, blending ranges their length can hold, and well-formed blocks: what the
   sizes below rely on.  Going into the other format (out), every kept block must be able to
   change format, and a PSB's must fit a PSD. */
static int mpsd__kept_ok(const minipsd_doc *d, unsigned out)
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
        const minipsd_layer *l     = &d->layers[i];
        const minipsd_bytes  extra = l->additional_info;
        if ((extra.size && !extra.data) || (l->blending_range_count && !l->blending_ranges)
            || (l->name_pascal.size && !l->name_pascal.data)) return MINIPSD_ERR_ARG;
        if (l->name_pascal.size > 255) return MINIPSD_ERR_SIZE;
        if (l->blending_range_count > MPSD__MAX_SECTION / MPSD__RANGE_SIZE) return MINIPSD_ERR_SIZE;
        for (pos = 0; pos < extra.size; pos = next) {
            if (mpsd__block(extra, pos, mpsd__doc_psb(d), &block, &next) != MINIPSD_OK) return MINIPSD_ERR_ARG;
            if (out == mpsd__doc_psb(d)) continue;
            if (!mpsd__convertible((const uint8_t *)block.key)) return MINIPSD_ERR_UNSUPPORTED;
            if (!out && block.data.size > MPSD__MAX_SECTION) return MINIPSD_ERR_SIZE;
        }
    }
    {
        mpsd__sink c = { NULL, NULL, 0, 0 };
        return mpsd__tail_out(&c, d, out);
    }
}

/* A stored channel going into the other format: an RLE one's rows must be known, its table must
   hold them, and they must fit a PSD's two-byte counts when it goes there. */
static int mpsd__table_ok(const mpsd__source *src, unsigned from, unsigned out)
{
    uint32_t y;
    if (!mpsd__retable(src, from, out)) return MINIPSD_OK;
    if (!mpsd__stored_ok(*src->stored, 0, src->height, 0, from))
        return MINIPSD_ERR_ARG;                                   /* a real mask's table: mpsd__mask_ok knew no rows */
    for (y = 0; !out && y < src->height; ++y)                     /* a PSD's counts: two bytes */
        if (mpsd__count(src->stored->data + MPSD__MARKER_SIZE, y, 1) > 0xFFFFu) return MINIPSD_ERR_SIZE;
    return MINIPSD_OK;
}

static int mpsd__retable_ok(const minipsd_doc *d, const minipsd_layer *l, int k, unsigned out)
{
    const mpsd__source src = mpsd__channel_source(d, l, k);
    if (k == mpsd__ch_mask(d) + 1 && mpsd__retable(&src, mpsd__doc_psb(d), out) && l->mask->rest.size < MPSD__REAL_RECT + MPSD__RECT_SIZE)
        return MINIPSD_ERR_UNSUPPORTED;                           /* no real rectangle to count rows by */
    return mpsd__table_ok(&src, mpsd__doc_psb(d), out);
}

/* A stored composite: the mode's colour channels and up to the format's limit, the one after them
   transparency only when there is one, fitting the canvas, its table the out format's (or one that
   can become it). */
static int mpsd__composite_ok(const minipsd_doc *d, unsigned out)
{
    const mpsd__source src = mpsd__composite_source(d);
    if (!mpsd__copies_composite(d)) return MINIPSD_OK;
    if (d->composite_channels < mpsd__doc_colors(d) || d->composite_channels > MPSD__MAX_CHANNELS
        || (d->composite_alpha && d->composite_channels < mpsd__doc_pixel(d))
        || !mpsd__stored_ok(d->composite_stored, d->width, src.height, mpsd__doc_shift(d), mpsd__doc_psb(d)))
        return MINIPSD_ERR_ARG;
    return mpsd__table_ok(&src, mpsd__doc_psb(d), out);
}

/* Groups nest: walking up from the bottom, a divider opens one, a top record closes the innermost. */
static int mpsd__groups_ok(const minipsd_doc *d)
{
    size_t i, open = 0;
    for (i = 0; i < d->layer_count; ++i) {
        const unsigned kind = d->layers[i].section_divider_type;
        if (kind == MINIPSD_SECTION_GROUP_END) ++open;
        else if (kind == MINIPSD_SECTION_GROUP || kind == MINIPSD_SECTION_GROUP_CLOSED) {
            if (!open) return 0;
            --open;
        } else if (kind != MINIPSD_SECTION_LAYER) {
            return 0;
        }
    }
    return open == 0;
}

/* A mask's rectangle, its pixels or stored channel, and what it keeps. */
static int mpsd__mask_ok(const minipsd_doc *d, const minipsd_mask *m, unsigned out)
{
    const int64_t  w = (int64_t)m->right - m->left, h = (int64_t)m->bottom - m->top;
    const unsigned wide = mpsd__doc_shift(d), psb = mpsd__doc_psb(d);
    if (w < 0 || h < 0 || (m->rest.size && !m->rest.data)) return MINIPSD_ERR_ARG;
    if (w > mpsd__max_side(out) || h > mpsd__max_side(out)) return MINIPSD_ERR_SIZE;
    if (m->stored.size ? !mpsd__stored_ok(m->stored, (uint32_t)w, (uint32_t)h, wide, psb) : (w && h && !m->pixels))
        return MINIPSD_ERR_ARG;
    if (m->real.size && !mpsd__stored_ok(m->real, 0, 0, 0, psb)) return MINIPSD_ERR_ARG;   /* its rectangle is in rest */
    return MINIPSD_OK;
}

/* The modes the writer takes, at the depths Photoshop has them: bitmap at 1 bit, indexed at 8,
   the others at 8 and 16; RGB and grey at 32 too.  Duotone brings its colour mode data (inks and
   curves), indexed its palette, the others none.  Every mode but RGB, and every 32-bit document,
   brings its composite, supplied or stored: the writer composes 8- and 16-bit RGB only.  The flat modes (bitmap, indexed, multichannel) have no layers and no
   transparency, and multichannel's composite is only copied: minipsd does not know its channels. */
static int mpsd__doc_mode_ok(const minipsd_doc *d)
{
    const unsigned mode  = mpsd__doc_mode(d), depth = mpsd__doc_depth(d);
    const int      byte  = mode == MINIPSD_MODE_BITMAP || mode == MINIPSD_MODE_INDEXED;   /* at most 8 bits */
    if (!mpsd__mode_ok(mode) || (mode == MINIPSD_MODE_BITMAP) != (depth == MPSD__CH_1BIT)) return MINIPSD_ERR_ARG;
    if ((depth == MPSD__CH_16BIT && byte)
        || (depth == MPSD__CH_32BIT && mode != MINIPSD_MODE_RGB && mode != MINIPSD_MODE_GREY))
        return MINIPSD_ERR_UNSUPPORTED;
    if (mode == MINIPSD_MODE_INDEXED ? d->color_mode_data.size != MPSD__PALETTE_SIZE
                                     : (mode == MINIPSD_MODE_DUOTONE) != (d->color_mode_data.size != 0))
        return MINIPSD_ERR_ARG;
    if ((mode != MINIPSD_MODE_RGB || depth == MPSD__CH_32BIT) && !d->composite && !d->composite_stored.size) return MINIPSD_ERR_ARG;
    if (mpsd__doc_flat(d) && (d->layer_count || (d->composite_stored.size && d->composite_alpha))) return MINIPSD_ERR_ARG;
    if (mode == MINIPSD_MODE_MULTICHANNEL && (d->composite || !d->composite_channels)) return MINIPSD_ERR_ARG;
    return MINIPSD_OK;
}

MINIPSDDEF int minipsd_doc_check(const minipsd_doc *d, const minipsd_write_opts *opts)
{
    double   res;
    size_t   i;
    int      rc;
    unsigned out;
    if (!d || (opts && opts->format > MINIPSD_PSB)) return MINIPSD_ERR_ARG;
    out = mpsd__out_psb(d, opts);
    /* The dpi is stored as 16.16 fixed point: it must round to 1 .. 2^32 - 1.  NaN fails both. */
    res = d->dpi * MPSD__FIXED_ONE + 0.5;
    if ((d->layer_count && !d->layers) || (!d->layer_count && !d->composite && !d->composite_stored.size) || d->layer_count > MPSD__MAX_LAYERS || !(res >= 1.0 && res < 4294967296.0)
        || (d->depth != 0 && d->depth != MPSD__CH_1BIT && d->depth != MPSD__CH_8BIT && d->depth != MPSD__CH_16BIT && d->depth != MPSD__CH_32BIT) || d->format > MINIPSD_PSB
        || d->compression < MINIPSD_RLE || d->compression > MINIPSD_ZIP_PREDICTED || (mpsd__zip(d) && !(opts && opts->deflate))
        || (d->color_mode_data.size && !d->color_mode_data.data))
        return MINIPSD_ERR_ARG;
    rc = mpsd__doc_mode_ok(d);
    if (rc != MINIPSD_OK) return rc;
    rc = mpsd__kept_ok(d, out);
    if (rc != MINIPSD_OK) return rc;
    if (!mpsd__groups_ok(d)) return MINIPSD_ERR_ARG;
    if (d->width < 1 || d->height < 1 || d->width > mpsd__max_side(out) || d->height > mpsd__max_side(out))
        return MINIPSD_ERR_SIZE;
    if (mpsd__resources_size(d) > MPSD__MAX_SECTION) return MINIPSD_ERR_SIZE;
    if (mpsd__zip_plane(d) > ((size_t)-1 >> 2)) return MINIPSD_ERR_SIZE;   /* the workspace holds it twice */
    rc = mpsd__composite_ok(d, out);
    if (rc != MINIPSD_OK) return rc;
    for (i = 0; i < d->layer_count; ++i) {    /* a side within the format's: so is any RLE row count */
        const minipsd_layer *l = &d->layers[i];
        const int64_t w = (int64_t)l->right - l->left, h = (int64_t)l->bottom - l->top;
        int k;
        if (w < 0 || h < 0 || (!l->pixels && !l->stored && w && h)) return MINIPSD_ERR_ARG;
        if (w > mpsd__max_side(out) || h > mpsd__max_side(out)) return MINIPSD_ERR_SIZE;
        rc = l->mask ? mpsd__mask_ok(d, l->mask, out) : MINIPSD_OK;
        for (k = mpsd__ch_mask(d); rc == MINIPSD_OK && k < mpsd__channel_count(d, l); ++k) rc = mpsd__retable_ok(d, l, k, out);
        if (rc != MINIPSD_OK) return rc;
        if (mpsd__layer_extra_size(l, mpsd__doc_psb(d), out) > MPSD__MAX_SECTION) return MINIPSD_ERR_SIZE;
        if (!l->stored) continue;
        if (!l->pixels && !d->composite && !d->composite_stored.size) return MINIPSD_ERR_ARG;   /* nothing to compose it from */
        for (k = 0; k < (int)mpsd__doc_pixel(d); ++k) {   /* transparency, the last, may be missing */
            const mpsd__rect r = mpsd__layer_rect(d, l);
            if ((k != (int)mpsd__doc_colors(d) || l->stored[k].size)
                && !mpsd__stored_ok(l->stored[k], r.width, r.height, mpsd__doc_shift(d), mpsd__doc_psb(d)))
                return MINIPSD_ERR_ARG;
        }
        for (k = 0; k < mpsd__ch_mask(d); ++k) {
            rc = mpsd__retable_ok(d, l, k, out);
            if (rc != MINIPSD_OK) return rc;
        }
    }
    return MINIPSD_OK;
}

/* Everything up to the first layer record: the file header, the colour mode data, the image
   resources (resolution, the ICC profile if any, the kept ones), the layer section's lengths and
   the layer count; with a layers' block, the tail before it and its header between the two. */
static void mpsd__head(mpsd__sink *s, const minipsd_doc *d, uint64_t info, unsigned out)
{
    mpsd__put(s, "8BPS", 4);
    mpsd__u16(s, out ? MINIPSD_PSB : MINIPSD_PSD);   /* the version */
    mpsd__zeros(s, 6);                        /* reserved            */
    mpsd__u16(s, mpsd__composite_channels(d));   /* R, G, B, alpha, ... */
    mpsd__u32(s, d->height);
    mpsd__u32(s, d->width);
    mpsd__u16(s, mpsd__doc_depth(d));         /* bits per channel    */
    mpsd__u16(s, mpsd__doc_mode(d));

    mpsd__u32(s, (uint32_t)d->color_mode_data.size);   /* colour mode data: a palette, inks */
    mpsd__put(s, d->color_mode_data.data, d->color_mode_data.size);

    mpsd__u32(s, (uint32_t)mpsd__resources_size(d));
    mpsd__resources(s, d);

    size_t         end;
    const size_t   at  = mpsd__layers_block(d, &end);
    const int      own = mpsd__own_layers_block(d);
    mpsd__putlen(s, mpsd__section_size(d, info, out), out);   /* layer and mask information */
    mpsd__putlen(s, at < d->layer_tail.size || own ? 0 : info, out);
    if (at < d->layer_tail.size) {
        (void)mpsd__tail_range(s, d, 0, at, out);
        mpsd__put(s, d->layer_tail.data + at, MPSD__BLOCK_LEN);   /* '8BIM', 'Lr16' */
        mpsd__putlen(s, info, out);
    } else if (own) {                                             /* the tail, then the writer's blocks */
        (void)mpsd__tail_out(s, d, out);
        if (mpsd__composite_alpha(d)) mpsd__own_block(s, "Mt32", 0, out);
        mpsd__own_block(s, "Lr32", info, out);
    }
    if (d->layer_count)                                           /* negative: the composite has transparency */
        mpsd__u16(s, (unsigned)(uint16_t)(mpsd__composite_alpha(d) && !own ? -(int16_t)d->layer_count : (int16_t)d->layer_count));
}

/* After the last layer channel: the layer info's padding, the kept tail or an empty global layer
   mask, and the composite's compression. */
static void mpsd__tail(mpsd__sink *s, const minipsd_doc *d, uint64_t content, uint64_t info, unsigned out)
{
    size_t       end;
    const size_t at = mpsd__layers_block(d, &end);
    mpsd__zeros(s, (size_t)(info - content));
    if (at < d->layer_tail.size) {            /* the block's padding, then the tail after it */
        mpsd__zeros(s, mpsd__pad((size_t)info, 4) - (size_t)info);
        (void)mpsd__tail_range(s, d, end, d->layer_tail.size, out);
    } else if (mpsd__own_layers_block(d)) {   /* the writer's block: its padding; the tail came before it */
        mpsd__zeros(s, mpsd__pad((size_t)info, 4) - (size_t)info);
    } else {
        (void)mpsd__tail_range(s, d, 0, d->layer_tail.size, out);   /* minipsd_doc_check has accepted it */
    }
    if (!mpsd__copies_composite(d)) mpsd__u16(s, mpsd__marker(d));   /* a stored composite brings its own */
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
    MPSD__STORED,            /* a stored layer channel, whole      (layer, k) */
    MPSD__STORED_HEAD,       /* a stored RLE channel going into the other format: its marker (layer, k) */
    MPSD__STORED_COUNT,      /* ... one row's count, re-written    (layer, k, row) */
    MPSD__STORED_ROWS,       /* ... its rows, whole                (layer, k) */
    MPSD__ZIP,               /* a deflated layer channel, whole    (layer, k) */
    MPSD__TAIL,              /* mpsd__tail                                    */
    MPSD__COMPOSITE_COUNT,   /* a composite row's byte count, RLE  (k, row)   */
    MPSD__COMPOSITE_ROW,     /* a composite row                    (k, row)   */
    MPSD__DONE
};

struct minipsd_writer {
    const minipsd_doc *doc;
    uint64_t          *channel_sizes;     /* layer_count * MPSD__CHANNELS     */
    uint32_t          *composite_sizes;   /* pixel * height, channel-major (RLE) */
    uint8_t           *composite_row;     /* width * pixel samples            */
    uint64_t           content, info;     /* the layer info's length, unpadded and padded */
    uint64_t           total;             /* the file size                    */
    /* the cursor */
    int                piece;             /* MPSD__HEAD .. MPSD__DONE         */
    size_t             layer;
    int                channel;           /* layers: written order (mpsd__channel_id); composite: RGBA offset */
    uint32_t           row;
    uint64_t           offset;            /* bytes of the current piece already handed out */
    uint32_t           composite_y;       /* the row composite_at holds; MPSD__NO_ROW for none */
    unsigned           out;               /* the format written: 1 for a PSB   */
    int                copying;           /* the composite, copied as stored  */
    const uint8_t     *composite_at;      /* that row: composite_row, or a matted supplied row */
    /* ZIP: the caller's deflate, and the one channel it last compressed */
    minipsd_deflate_fn deflate;
    void              *deflate_user;
    uint8_t           *zip_plane;         /* the channel's raw rows, predicted when asked */
    uint8_t           *zip_out;           /* its stream                       */
    size_t             zip_room, zip_size;
    size_t             zip_layer;         /* which channel: (size_t)-1 for none */
    int                zip_channel;
};

#define MPSD__NO_ROW 0xFFFFFFFFu

/* Lays the workspace out and returns the bytes it takes, alignment slack included: the writer,
   the size table, the composite row sizes (RLE only), one composite row; with ZIP, the largest
   deflated channel raw, and the room its stream gets.  With a workspace, the writer is built at
   its aligned start and returned through *w. */
static size_t mpsd__writer_layout(const minipsd_doc *d, minipsd_workspace *workspace, minipsd_writer **w)
{
    const size_t self  = mpsd__pad(sizeof(minipsd_writer), MPSD__ALIGN);
    const size_t sizes = d->layer_count * MPSD__CHANNELS * sizeof(uint64_t);
    const size_t rows  = d->compression == MINIPSD_RAW ? 0 : (size_t)d->height * mpsd__doc_pixel(d) * sizeof(uint32_t);
    const size_t row   = (size_t)d->width * mpsd__doc_pixel(d) << mpsd__doc_shift(d);
    const size_t plane = (size_t)mpsd__zip_plane(d), room = plane ? (size_t)mpsd__deflate_room(plane) : 0;   /* doc_check bounds it */
    if (workspace) {
        uint8_t *p = (uint8_t *)workspace;
        p += (MPSD__ALIGN - (uintptr_t)p % MPSD__ALIGN) % MPSD__ALIGN;
        *w = (minipsd_writer *)p;
        (*w)->channel_sizes   = (uint64_t *)(p + self);
        (*w)->composite_sizes = (uint32_t *)(p + self + sizes);
        (*w)->composite_row   = p + self + sizes + rows;
        (*w)->zip_plane       = (*w)->composite_row + row;
        (*w)->zip_out         = (*w)->zip_plane + plane;
        (*w)->zip_room        = room;
    }
    return MPSD__ALIGN - 1 + self + sizes + rows + row + plane + room;
}

/* Is layer i's channel k deflated?  With ZIP, every channel encoded from samples. */
static int mpsd__zips(const minipsd_doc *d, size_t i, int k)
{
    return mpsd__zip(d) && k <= mpsd__ch_mask(d) && mpsd__channel_source(d, &d->layers[i], k).samples;
}

/* Layer i's channel k deflated into zip_out, unless it is there already: its samples gathered as
   the file's raw rows (big-endian), each made its difference from its left neighbour when
   predicted, then handed to the caller's deflate. */
static int mpsd__zip_channel(minipsd_writer *w, size_t i, int k)
{
    const minipsd_doc *d    = w->doc;
    const mpsd__source src  = mpsd__channel_source(d, &d->layers[i], k);
    const unsigned     wide = mpsd__doc_shift(d);
    const size_t       bytes = (size_t)src.width << wide;
    size_t             x, y, size = w->zip_room;
    if (w->zip_layer == i && w->zip_channel == k) return MINIPSD_OK;
    w->zip_layer = (size_t)-1;
    for (y = 0; y < src.height; ++y) {
        const uint8_t *row = src.samples + y * src.width * src.step;
        uint8_t       *out = w->zip_plane + y * bytes;
        for (x = 0; x < bytes; ++x)               /* at 32 bits predicted: shuffled, as the format wants */
            out[d->compression == MINIPSD_ZIP_PREDICTED && wide == MPSD__SHIFT_32 ? mpsd__shuffled(x, src.width) : x]
                = row[mpsd__at(x, (ptrdiff_t)src.step, wide)];
        if (d->compression == MINIPSD_ZIP_PREDICTED) mpsd__predict(out, src.width, mpsd__doc_depth(d));
    }
    if (w->deflate(w->deflate_user, w->zip_plane, bytes * src.height, w->zip_out, &size) || size > w->zip_room)
        return MINIPSD_ERR_WRITE;
    w->zip_size    = size;
    w->zip_layer   = i;
    w->zip_channel = k;
    return MINIPSD_OK;
}

/* The sizing pass over the layers: every channel's size into channel_sizes (layer_count *
   MPSD__CHANNELS, written order), the layer info's length into content and, padded to even, info.
   Every layer channel's size goes in the records ahead of the data, so each is compressed once
   here to learn it (deflated too, with ZIP). */
static int mpsd__measure_layers(minipsd_writer *w)
{
    const minipsd_doc *d = w->doc;
    const unsigned     psb = w->out;
    uint64_t          *sizes = w->channel_sizes;
    size_t             i;
    int                k, rc;
    w->content = d->layer_count ? MPSD__LAYER_COUNT : 0;   /* no layers: no count either */
    for (i = 0; i < d->layer_count; ++i) {
        const int n = mpsd__channel_count(d, &d->layers[i]), first = mpsd__first_channel(d, &d->layers[i]);
        w->content += mpsd__record_size((uint32_t)(n - first), psb) + mpsd__layer_extra_size(&d->layers[i], mpsd__doc_psb(d), psb);
        for (k = first; k < n; ++k) {
            if (mpsd__zips(d, i, k)) {                 /* deflated once here, once more to write */
                MPSD__ENCODED(&d->layers[i]);
                rc = mpsd__zip_channel(w, i, k);
                if (rc != MINIPSD_OK) return rc;
                sizes[i * MPSD__CHANNELS + (size_t)k] = MPSD__MARKER_SIZE + (uint64_t)w->zip_size;
            } else {
                sizes[i * MPSD__CHANNELS + (size_t)k] = mpsd__layer_channel_size(d, &d->layers[i], k, psb);
            }
            w->content += sizes[i * MPSD__CHANNELS + (size_t)k];
        }
    }
    w->info = mpsd__pad((size_t)w->content, 2);   /* even, as the format has it */
    return mpsd__section_size(d, w->info, psb) > mpsd__max_layers_section(psb) ? MINIPSD_ERR_SIZE : MINIPSD_OK;
}

MINIPSDDEF int minipsd_writer_workspace_size(const minipsd_doc *d, const minipsd_write_opts *opts, size_t *size)
{
    const int rc = d ? minipsd_doc_check(d, opts) : MINIPSD_ERR_ARG;
    if (rc != MINIPSD_OK) return rc;
    if (!size) return MINIPSD_ERR_ARG;
    *size = mpsd__writer_layout(d, NULL, NULL);
    return MINIPSD_OK;
}

MINIPSDDEF int minipsd_writer_init(minipsd_writer **out, const minipsd_doc *d, const minipsd_write_opts *opts,
                                   minipsd_workspace *workspace, size_t size)
{
    minipsd_writer *w = NULL;
    mpsd__composite comp;
    uint64_t        total;
    uint32_t        y;
    unsigned        wide;
    int             k, rc;
    if (!out) return MINIPSD_ERR_ARG;
    *out = NULL;
    rc = d ? minipsd_doc_check(d, opts) : MINIPSD_ERR_ARG;
    if (rc != MINIPSD_OK) return rc;
    if (!workspace || size < mpsd__writer_layout(d, NULL, NULL)) return MINIPSD_ERR_ARG;
    mpsd__writer_layout(d, workspace, &w);
    w->doc = d;
    w->out = mpsd__out_psb(d, opts);
    wide   = mpsd__doc_shift(d);
    w->deflate      = opts ? opts->deflate : NULL;   /* copied: opts may be gone by the first fill */
    w->deflate_user = opts ? opts->deflate_user : NULL;
    w->zip_layer    = (size_t)-1;
    w->zip_channel  = 0;
    w->zip_size     = 0;
    rc = mpsd__measure_layers(w);
    if (rc != MINIPSD_OK) return rc;

    total = MPSD__HEADER_SIZE
          + MPSD__LEN + d->color_mode_data.size                      /* colour mode data */
          + MPSD__LEN + mpsd__resources_size(d)
          + mpsd__len_size(w->out) + mpsd__section_size(d, w->info, w->out)   /* the section's length, the section */
          + MPSD__MARKER_SIZE;                                       /* the composite's */
    if (mpsd__copies_composite(d)) {
        const mpsd__source src = mpsd__composite_source(d);
        total += mpsd__stored_size(&src, mpsd__doc_psb(d), w->out) - MPSD__MARKER_SIZE;
    } else if (d->compression == MINIPSD_RAW) {
        total += mpsd__row_bytes(d->width, mpsd__doc_depth(d)) * d->height * mpsd__doc_pixel(d);
    } else {                                  /* each row's count, then the row */
        mpsd__composite_init(&comp, d, w->composite_row);
        for (y = 0; y < d->height; ++y) {
            const uint8_t *row = mpsd__composite_row(&comp, y);
            for (k = 0; k < (int)mpsd__doc_pixel(d); ++k) {
                const uint64_t n = mpsd__row_size(d, row + ((size_t)k << wide), (size_t)mpsd__doc_pixel(d) << wide, d->width);
                w->composite_sizes[(size_t)k * d->height + y] = (uint32_t)n;
                total += mpsd__count_size(w->out) + n;
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
    w->copying     = 0;
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
    const int            channel = w->piece > MPSD__RECORD && w->piece < MPSD__TAIL;   /* a layer channel's piece */
    const minipsd_layer *l   = d->layer_count ? &d->layers[w->layer] : NULL;   /* 0 outside the layer section */
    const mpsd__source   src = w->copying ? mpsd__composite_source(d) : channel ? mpsd__channel_source(d, l, w->channel) : mpsd__no_source();
    const uint8_t       *row = src.samples ? src.samples + (size_t)w->row * src.width * src.step : NULL;
    switch (w->piece) {
    case MPSD__HEAD:            mpsd__head(s, d, w->info, w->out); break;
    case MPSD__RECORD:          mpsd__layer_record(s, d, l, w->channel_sizes + w->layer * MPSD__CHANNELS, w->out); break;
    case MPSD__COMPRESSION:     mpsd__u16(s, mpsd__marker(d)); break;
    case MPSD__COUNT:           mpsd__putcount(s, (uint32_t)mpsd__row_size(d, row, src.step, src.width), w->out); break;
    case MPSD__ROW:             mpsd__row(s, d, row, src.step, src.width); break;
    case MPSD__STORED:          mpsd__put(s, src.stored->data, src.stored->size); break;
    case MPSD__STORED_HEAD:     mpsd__put(s, src.stored->data, MPSD__MARKER_SIZE); break;
    case MPSD__STORED_COUNT:    mpsd__putcount(s, mpsd__count(src.stored->data + MPSD__MARKER_SIZE, w->row, mpsd__doc_psb(d)), w->out); break;
    case MPSD__STORED_ROWS: {
        const size_t table = MPSD__MARKER_SIZE + (size_t)src.height * mpsd__count_size(mpsd__doc_psb(d));
        mpsd__put(s, src.stored->data + table, src.stored->size - table);
        break;
    }
    case MPSD__ZIP:                                         /* minipsd_writer_fill deflated it */
        mpsd__u16(s, d->compression == MINIPSD_ZIP ? MPSD__COMP_ZIP : MPSD__COMP_ZIP_PREDICTED);
        mpsd__put(s, w->zip_out, w->zip_size);
        break;
    case MPSD__TAIL:            mpsd__tail(s, d, w->content, w->info, w->out); break;
    case MPSD__COMPOSITE_COUNT: mpsd__putcount(s, w->composite_sizes[(size_t)w->channel * d->height + w->row], w->out); break;
    default:
        mpsd__row(s, d, mpsd__writer_composite(w, w->row) + ((size_t)w->channel << mpsd__doc_shift(d)), (size_t)mpsd__doc_pixel(d) << mpsd__doc_shift(d), d->width);
        break;
    }
}

/* The first piece of a source: copied whole when stored (in parts when its count table changes
   width), else encoded. */
static int mpsd__source_piece(const mpsd__source *src, const minipsd_writer *w)
{
    if (mpsd__retable(src, mpsd__doc_psb(w->doc), w->out)) return MPSD__STORED_HEAD;
    return src->stored ? MPSD__STORED : MPSD__COMPRESSION;
}

/* The first piece of the current layer channel. */
static int mpsd__channel_piece(const minipsd_writer *w)
{
    const mpsd__source src = mpsd__channel_source(w->doc, &w->doc->layers[w->layer], w->channel);
    return mpsd__zips(w->doc, w->layer, w->channel) ? MPSD__ZIP : mpsd__source_piece(&src, w);
}

/* Moves the cursor to the piece after the current one, in file order. */
static void mpsd__writer_next(minipsd_writer *w)
{
    const minipsd_doc *d    = w->doc;
    const int          rle  = d->compression != MINIPSD_RAW;
    const uint32_t     rows = w->copying ? mpsd__composite_source(d).height
                            : w->piece > MPSD__RECORD && w->piece < MPSD__TAIL ? mpsd__channel_source(d, &d->layers[w->layer], w->channel).height : 0;
    w->offset = 0;
    switch (w->piece) {
    case MPSD__HEAD:
        w->piece = d->layer_count ? MPSD__RECORD : MPSD__TAIL;
        return;
    case MPSD__RECORD:
        if (++w->layer < d->layer_count) return;
        w->layer   = 0;
        w->channel = mpsd__first_channel(d, &d->layers[0]);
        w->piece   = mpsd__channel_piece(w);
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
    case MPSD__STORED:
    case MPSD__STORED_ROWS:
    case MPSD__ZIP:
        break;
    case MPSD__STORED_HEAD:                   /* the counts, unless there are no rows */
        w->piece = rows ? MPSD__STORED_COUNT : MPSD__STORED_ROWS;
        return;
    case MPSD__STORED_COUNT:
        if (++w->row < rows) return;
        w->row   = 0;
        w->piece = MPSD__STORED_ROWS;
        return;
    case MPSD__TAIL:
        if (mpsd__copies_composite(d)) {
            const mpsd__source src = mpsd__composite_source(d);
            w->copying = 1;
            w->piece   = mpsd__source_piece(&src, w);
            return;
        }
        w->piece = rle ? MPSD__COMPOSITE_COUNT : MPSD__COMPOSITE_ROW;
        return;
    default:                                  /* the composite: channel by channel, row by row */
        if (++w->row < d->height) return;
        w->row = 0;
        if (++w->channel < (int)mpsd__doc_pixel(d)) return;
        w->channel = 0;
        w->piece   = w->piece == MPSD__COMPOSITE_COUNT ? MPSD__COMPOSITE_ROW : MPSD__DONE;
        return;
    }
    if (w->copying) { w->piece = MPSD__DONE; return; }   /* the stored composite is complete */
    /* A layer channel is complete: the next channel, the next layer, or the tail. */
    if (++w->channel < mpsd__channel_count(d, &d->layers[w->layer])) { w->piece = mpsd__channel_piece(w); return; }
    w->channel = 0;
    if (++w->layer < d->layer_count) {
        w->channel = mpsd__first_channel(d, &d->layers[w->layer]);
        w->piece   = mpsd__channel_piece(w);
        return;
    }
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
        if (w->piece == MPSD__ZIP) {                  /* deflated again: the size in its record must hold */
            const int rc = mpsd__zip_channel(w, w->layer, w->channel);
            if (rc != MINIPSD_OK || MPSD__MARKER_SIZE + (uint64_t)w->zip_size != w->channel_sizes[w->layer * MPSD__CHANNELS + (size_t)w->channel]) {
                *produced = n;
                return MINIPSD_ERR_WRITE;
            }
        }
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

MINIPSDDEF int minipsd_write(const minipsd_doc *d, const minipsd_write_opts *opts, minipsd_write_fn fn, void *user)
{
    minipsd_writer *w;
    void           *workspace;
    uint8_t         chunk[MPSD__CHUNK];
    size_t          size = 0, n;
    int             rc = d ? minipsd_writer_workspace_size(d, opts, &size) : MINIPSD_ERR_ARG;   /* d spelled out for static analysers */
    if (rc != MINIPSD_OK) return rc;
    if (!fn) return MINIPSD_ERR_ARG;
    workspace = MINIPSD_MALLOC(size);
    if (!workspace) return MINIPSD_ERR_ALLOC;
    rc = minipsd_writer_init(&w, d, opts, workspace, size);
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
