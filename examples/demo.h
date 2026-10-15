/**
 * \file
 * \brief The document every example writes: Platemaker's template, a "Template" layer with an
 *        empty "Layer" above it.  It is the golden test's "template-rle" document
 *        (tests/test_golden.cpp), so ctest can check each example's bytes against its SHA-256.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef EXAMPLES_DEMO_H
#define EXAMPLES_DEMO_H

#include "minipsd.h"

#include <string.h>

#define DEMO_W 300
#define DEMO_H 7

typedef struct demo {
    uint8_t       pixels[DEMO_W * DEMO_H * 4];
    minipsd_layer layers[2];
    minipsd_doc   doc;
} demo;

/* Fills d; d->doc borrows the rest of d, so d must stay put while it is written. */
static void demo_init(demo *d)
{
    uint32_t state = 1, x, y;
    for (y = 0; y < DEMO_H; ++y)
        for (x = 0; x < DEMO_W; ++x) {   /* three bands: flat, gradient, noise */
            uint8_t *p = d->pixels + ((size_t)y * DEMO_W + x) * 4;
            const uint32_t band = x * 3 / DEMO_W;
            state = state * 1664525u + 1013904223u;
            p[0] = (uint8_t)(band == 0 ? 220 : band == 1 ? x + y : state >> 24);
            p[1] = (uint8_t)(band == 0 ? 40 : y * 9);
            p[2] = (uint8_t)(70 + (band == 2 ? state >> 16 : 0));
            p[3] = (uint8_t)(band == 0 ? 255 : band == 1 ? 255 - x : state >> 8);
        }
    d->layers[0].name = "Template"; d->layers[0].rgba = d->pixels; d->layers[0].opacity = 255; d->layers[0].hidden = 0;
    d->layers[1].name = "Layer";    d->layers[1].rgba = NULL;      d->layers[1].opacity = 255; d->layers[1].hidden = 0;
    memset(&d->doc, 0, sizeof d->doc);
    d->doc.width = DEMO_W; d->doc.height = DEMO_H; d->doc.dpi = 300.0;
    d->doc.layers = d->layers; d->doc.layer_count = 2; d->doc.compression = MINIPSD_RLE;
}

#endif /* EXAMPLES_DEMO_H */
