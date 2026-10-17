/**
 * \file
 * \brief Internal: the composite, the layers blended "over" and matted against white, one row at a
 *        time (the writer); the matte undone again (the reader).  Also a layer's rectangle.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MPSD__COMPOSITE_DONE
#define MPSD__COMPOSITE_DONE

#include <string.h>

typedef struct mpsd__composite {
    const minipsd_doc *doc;
    uint8_t           *row;      /* width * 4, the caller's */
} mpsd__composite;

static int mpsd__contributes(const minipsd_layer *l) { return l->rgba && !l->hidden && l->opacity; }

/* A layer's rectangle as written, and the size of its pixels: 0 by 0 for a layer without pixels or
   without area.  All four sides zero with pixels is the full canvas.  Valid after
   minipsd_doc_check, which keeps width and height within MINIPSD_MAX_SIDE. */
typedef struct mpsd__rect {
    int32_t  top, left, bottom, right;
    uint32_t width, height;
} mpsd__rect;

static mpsd__rect mpsd__layer_rect(const minipsd_doc *d, const minipsd_layer *l)
{
    mpsd__rect r;
    r.top = l->top; r.left = l->left; r.bottom = l->bottom; r.right = l->right;
    if (l->rgba && !(r.top | r.left | r.bottom | r.right)) {
        r.bottom = (int32_t)d->height;
        r.right  = (int32_t)d->width;
    }
    r.width  = (uint32_t)((int64_t)r.right - r.left);
    r.height = (uint32_t)((int64_t)r.bottom - r.top);
    if (!l->rgba || !r.width || !r.height) r.width = r.height = 0;
    return r;
}

static void mpsd__composite_init(mpsd__composite *c, const minipsd_doc *d, uint8_t *row)
{
    c->doc = d;
    c->row = row;
}

static const uint8_t *mpsd__composite_row(mpsd__composite *c, uint32_t y)
{
    const minipsd_doc *d = c->doc;
    size_t i;
    int64_t x;
    memset(c->row, 0, (size_t)d->width * 4);
    for (i = 0; i < d->layer_count; ++i) {      /* each layer at its offset, clipped to the canvas */
        const minipsd_layer *l  = &d->layers[i];
        const mpsd__rect     r  = mpsd__layer_rect(d, l);
        const int64_t        ly = (int64_t)y - r.top;
        const int64_t        x0 = r.left > 0 ? r.left : 0;
        const int64_t        x1 = r.right < (int64_t)d->width ? r.right : (int64_t)d->width;
        const uint8_t *src;
        if (!mpsd__contributes(l) || ly < 0 || ly >= (int64_t)r.height) continue;
        src = l->rgba + (size_t)ly * r.width * 4;
        for (x = x0; x < x1; ++x) {
            uint8_t       *o  = c->row + (size_t)x * 4;
            const uint8_t *px = src + (size_t)(x - r.left) * 4;
            const float sa = px[3] / 255.0f * (l->opacity / 255.0f);
            const float da = o[3] / 255.0f;
            const float oa = sa + da * (1.0f - sa);
            unsigned k;
            if (oa <= 0.0f) continue;
            for (k = 0; k < 3; ++k)
                o[k] = (uint8_t)((px[k] * sa + o[k] * da * (1.0f - sa)) / oa + 0.5f);
            o[3] = (uint8_t)(oa * 255.0f + 0.5f);
        }
    }
    for (x = 0; x < (int64_t)d->width; ++x) {   /* the merged image is stored over white */
        uint8_t *o = c->row + (size_t)x * 4;
        const unsigned a = o[3];
        int k;
        for (k = 0; k < 3; ++k)
            o[k] = (uint8_t)((o[k] * a + 255u * (255u - a) + 127u) / 255u);
    }
    return c->row;
}

/* Undoes the white matte of one stored sample s at alpha a: c = 255 - (255 - s) * 255 / a, rounded
   and clamped; 0 where a is 0, since a transparent pixel has no colour.  Exact at a == 255; below,
   8-bit matting has already merged nearby colours, about 255 / (2a) levels apart. */
static uint8_t mpsd__unmatte(unsigned s, unsigned a)
{
    unsigned d;
    if (a == 0) return 0;
    d = ((255u - s) * 255u + a / 2) / a;
    return (uint8_t)(d >= 255u ? 0u : 255u - d);
}

#endif /* MPSD__COMPOSITE_DONE */
