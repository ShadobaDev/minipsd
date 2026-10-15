/**
 * \file
 * \brief Internal: the composite, the layers blended "over" and matted against white, one row at a
 *        time.  Included by the writer only.
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

static void mpsd__composite_init(mpsd__composite *c, const minipsd_doc *d, uint8_t *row)
{
    c->doc = d;
    c->row = row;
}

static const uint8_t *mpsd__composite_row(mpsd__composite *c, uint32_t y)
{
    const minipsd_doc *d = c->doc;
    size_t i;
    uint32_t x;
    memset(c->row, 0, (size_t)d->width * 4);
    for (i = 0; i < d->layer_count; ++i) {
        const minipsd_layer *l = &d->layers[i];
        const uint8_t *src;
        if (!mpsd__contributes(l)) continue;
        src = l->rgba + (size_t)y * d->width * 4;
        for (x = 0; x < d->width; ++x) {
            uint8_t    *o  = c->row + (size_t)x * 4;
            const float sa = src[x * 4 + 3] / 255.0f * (l->opacity / 255.0f);
            const float da = o[3] / 255.0f;
            const float oa = sa + da * (1.0f - sa);
            unsigned k;
            if (oa <= 0.0f) continue;
            for (k = 0; k < 3; ++k)
                o[k] = (uint8_t)((src[x * 4 + k] * sa + o[k] * da * (1.0f - sa)) / oa + 0.5f);
            o[3] = (uint8_t)(oa * 255.0f + 0.5f);
        }
    }
    for (x = 0; x < d->width; ++x) {          /* the merged image is stored over white */
        uint8_t *o = c->row + (size_t)x * 4;
        const unsigned a = o[3];
        int k;
        for (k = 0; k < 3; ++k)
            o[k] = (uint8_t)((o[k] * a + 255u * (255u - a) + 127u) / 255u);
    }
    return c->row;
}

#endif /* MPSD__COMPOSITE_DONE */
