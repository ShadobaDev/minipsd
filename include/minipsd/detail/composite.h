/**
 * \file minipsd/detail/composite.h
 * \brief Internal: the composite one row at a time, supplied or the layers blended "over", matted
 *        against white (the writer); the matte undone again (the reader).  Also a layer's
 *        rectangle.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MPSD__COMPOSITE_DONE
#define MPSD__COMPOSITE_DONE

#include <string.h>

typedef struct mpsd__composite {
    const minipsd_doc *doc;     /* pointer to the PSD document */
    uint8_t           *row;     /* width * 4 samples, the caller's */
} mpsd__composite;

/* The document's bits a sample (0 is 8), and the bytes a sample takes in memory: 1 << shift. */
MPSD__SHARED static unsigned mpsd__doc_depth(const minipsd_doc *d) { return d->depth ? d->depth : MPSD__CH_8BIT; }
MPSD__SHARED static unsigned mpsd__doc_shift(const minipsd_doc *d) { return mpsd__shift(mpsd__doc_depth(d)); }

/* The full sample of the document's depth (1.0f's bits at 32). */
MPSD__SHARED static uint32_t mpsd__doc_full(const minipsd_doc *d) { return mpsd__full(mpsd__doc_depth(d)); }

/* The file's colour mode: color_mode, whose 0 is RGB unless depth is 1 (bitmap, the only 1-bit
   mode). */
MPSD__SHARED static unsigned mpsd__doc_mode(const minipsd_doc *d)
{
    return d->color_mode || d->depth == 1 ? d->color_mode : (unsigned)MINIPSD_MODE_RGB;
}

/* Is the mode flat: bitmap, indexed or multichannel, which have no layers and no transparency? */
MPSD__SHARED static int mpsd__doc_flat(const minipsd_doc *d)
{
    const unsigned mode = mpsd__doc_mode(d);
    return mode == MINIPSD_MODE_BITMAP || mode == MINIPSD_MODE_INDEXED || mode == MINIPSD_MODE_MULTICHANNEL;
}

/* The document's colour channels (three in RGB and Lab, four in CMYK, one in grey, duotone, bitmap
   and indexed, the stored composite's in multichannel), and the samples of a pixel: those, then
   transparency unless the mode is flat. */
MPSD__SHARED static unsigned mpsd__doc_colors(const minipsd_doc *d) { return mpsd__colors(mpsd__doc_mode(d), d->composite_channels); }
MPSD__SHARED static unsigned mpsd__doc_pixel(const minipsd_doc *d) { return mpsd__doc_colors(d) + !mpsd__doc_flat(d); }

/* The samples of a layer's pixel: the colour channels, then transparency unless it has none. */
MPSD__SHARED static unsigned mpsd__layer_pixel(const minipsd_doc *d, const minipsd_layer *l) { return mpsd__doc_colors(d) + !l->no_alpha; }

/* The samples of a supplied composite's pixel: the colour channels, then transparency when it has
   one (never in the flat modes). */
MPSD__SHARED static unsigned mpsd__supplied_pixel(const minipsd_doc *d)
{
    return mpsd__doc_colors(d) + (d->composite_alpha && !mpsd__doc_flat(d));
}

/* The samples a pixel of mpsd__composite_row's row takes: a supplied composite without
   transparency is handed out as it is; every other row is a full pixel. */
MPSD__SHARED static unsigned mpsd__composite_step(const minipsd_doc *d)
{
    return d->composite ? mpsd__supplied_pixel(d) : mpsd__doc_pixel(d);
}

MPSD__SHARED static int mpsd__contributes(const minipsd_layer *l) { return l->pixels && !l->hidden && l->opacity; }

/* A layer's rectangle as written, and the size of its pixels: 0 by 0 for a layer without pixels or
   without area.  All four sides zero with pixels (rgba or stored channels) is the full canvas.
   Valid after minipsd_doc_check, which keeps width and height within the format's side limit. */
typedef struct mpsd__rect {
    int32_t  top, left, bottom, right;
    uint32_t width, height;
} mpsd__rect;

MPSD__SHARED static mpsd__rect mpsd__layer_rect(const minipsd_doc *d, const minipsd_layer *l)
{
    mpsd__rect r;
    r.top = l->top; r.left = l->left; r.bottom = l->bottom; r.right = l->right;
    if (l->pixels && !l->stored && !(r.top | r.left | r.bottom | r.right)) {   /* stored: the file's own rectangle */
        r.bottom = (int32_t)d->height;
        r.right  = (int32_t)d->width;
    }
    r.width  = (uint32_t)((int64_t)r.right - r.left);
    r.height = (uint32_t)((int64_t)r.bottom - r.top);
    if ((!l->pixels && !l->stored) || r.right <= r.left || r.bottom <= r.top) r.width = r.height = 0;   /* inverted: empty */
    return r;
}

MPSD__SHARED static void mpsd__composite_init(mpsd__composite *c, const minipsd_doc *d, uint8_t *row)
{
    c->doc = d;
    c->row = row;
}

/* Tests count the rows composed from the layers by defining this before the implementation. */
#ifndef MPSD__COMPOSED
#  define MPSD__COMPOSED() ((void)0)
#endif

/* The visible layers' row y blended "over" into row, straight alpha: the best effort.  A layer
   without transparency counts as opaque. */
MPSD__SHARED static void mpsd__compose(const minipsd_doc *d, uint8_t *row, uint32_t y)
{
    const unsigned wide = mpsd__doc_shift(d), pixel = (unsigned)MPSD__RGBA << wide;
    const float    max  = (float)mpsd__doc_full(d);
    size_t i;
    int64_t x;
    MPSD__COMPOSED();
    memset(row, 0, (size_t)d->width * pixel);
    for (i = 0; i < d->layer_count; ++i) {      /* each layer at its offset, clipped to the canvas */
        const minipsd_layer *l  = &d->layers[i];
        const mpsd__rect     r  = mpsd__layer_rect(d, l);
        const int64_t        ly = (int64_t)y - r.top;
        const int64_t        x0 = r.left > 0 ? r.left : 0;
        const int64_t        x1 = r.right < (int64_t)d->width ? r.right : (int64_t)d->width;
        const unsigned       lp = mpsd__layer_pixel(d, l) << wide;
        const uint8_t *src;
        if (!mpsd__contributes(l) || ly < 0 || ly >= (int64_t)r.height) continue;
        src = l->pixels + (size_t)ly * r.width * lp;
        for (x = x0; x < x1; ++x) {
            uint8_t       *o  = row + (size_t)x * pixel;
            const uint8_t *px = src + (size_t)(x - r.left) * lp;
            const float    la = l->no_alpha ? max : (float)mpsd__sample(px + ((unsigned)MINIPSD_CH_A << wide), wide);
            const float sa = la / max * (l->opacity / (float)MPSD__FULL_8);
            const float da = (float)mpsd__sample(o + ((unsigned)MINIPSD_CH_A << wide), wide) / max;
            const float oa = sa + da * (1.0f - sa);
            unsigned k;
            if (oa <= 0.0f) continue;
            for (k = 0; k < MINIPSD_CH_A; ++k) {
                const float c = ((float)mpsd__sample(px + (k << wide), wide) * sa
                                 + (float)mpsd__sample(o + (k << wide), wide) * da * (1.0f - sa)) / oa;
                mpsd__set_sample(o + (k << wide), wide, (uint32_t)(c + 0.5f));
            }
            mpsd__set_sample(o + ((unsigned)MINIPSD_CH_A << wide), wide, (uint32_t)(oa * max + 0.5f));
        }
    }
}

/* The composite's row y as the file stores it, matted against white: a supplied row that is
   already matted, of another mode than RGB, or without transparency (nothing to matte), as it is
   (bitmap packed into the caller's row), mpsd__composite_step samples a pixel; otherwise the
   supplied row copied, or the layers composed, into the caller's row, then matted there, a full
   pixel each, of which a composite without transparency writes the colour. */
MPSD__SHARED static const uint8_t *mpsd__composite_row(mpsd__composite *c, uint32_t y)
{
    const minipsd_doc *d    = c->doc;
    const unsigned     wide = mpsd__doc_shift(d), pixel = mpsd__doc_pixel(d) << wide;
    const uint32_t     max  = mpsd__doc_full(d);
    const size_t       size = (size_t)d->width * pixel;
    uint32_t x;
    if (d->composite && mpsd__supplied_pixel(d) < mpsd__doc_pixel(d))   /* no transparency: colour as given */
        return d->composite + (size_t)y * d->width * (mpsd__supplied_pixel(d) << wide);
    if (mpsd__doc_depth(d) == MPSD__CH_1BIT) {   /* bitmap: eight pixels a byte, the first the high bit, 1 black */
        const uint8_t *in = d->composite + (size_t)y * d->width;
        memset(c->row, 0, (size_t)mpsd__row_bytes(d->width, MPSD__CH_1BIT));
        for (x = 0; x < d->width; ++x)
            if (in[x]) c->row[x / MPSD__BYTE_BITS] |= (uint8_t)(1u << (MPSD__BYTE_BITS - 1u - x % MPSD__BYTE_BITS));
        return c->row;
    }
    if (!d->composite)              mpsd__compose(d, c->row, y);   /* RGB only: minipsd_doc_check */
    else if (d->composite_matted || mpsd__doc_mode(d) != MINIPSD_MODE_RGB)   /* other modes: as given */
        return d->composite + (size_t)y * size;
    else                            memcpy(c->row, d->composite + (size_t)y * size, size);
    for (x = 0; x < d->width && wide == MPSD__SHIFT_32; ++x) {   /* floats: c * a + (1 - a) */
        uint8_t    *o = c->row + (size_t)x * pixel;
        const float a = mpsd__float(o + ((unsigned)MINIPSD_CH_A << wide));
        unsigned k;
        for (k = 0; k < MINIPSD_CH_A; ++k) mpsd__set_float(o + (k << wide), mpsd__float(o + (k << wide)) * a + (1.0f - a));
    }
    for (x = 0; x < d->width && wide != MPSD__SHIFT_32; ++x) {   /* the merged image is stored over white */
        uint8_t       *o = c->row + (size_t)x * pixel;
        const uint32_t a = mpsd__sample(o + ((unsigned)MINIPSD_CH_A << wide), wide);
        unsigned k;
        for (k = 0; k < MINIPSD_CH_A; ++k)               /* at most max * max + max / 2: fits 32 bits */
            mpsd__set_sample(o + (k << wide), wide, (mpsd__sample(o + (k << wide), wide) * a + max * (max - a) + max / 2) / max);
    }
    return c->row;
}

/* Undoes the white matte of one stored sample s at alpha a, both up to max (255, or 65535 for 16
   bits): c = max - (max - s) * max / a, rounded and clamped; 0 where a is 0, since a transparent
   pixel has no colour.  Exact at a == max; below, matting has already merged nearby colours, about
   max / (2a) levels apart. */
MPSD__SHARED static uint32_t mpsd__unmatte(uint32_t s, uint32_t a, uint32_t max)
{
    uint32_t d;
    if (a == 0) return 0;
    d = ((max - s) * max + a / 2) / a;
    return d >= max ? 0u : max - d;
}

#endif /* MPSD__COMPOSITE_DONE */
