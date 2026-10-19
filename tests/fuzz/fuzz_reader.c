/**
 * \file
 * \brief Fuzz target: any bytes through the whole reader, every call a file can reach.
 *
 * Built as a libFuzzer target by the linux-fuzz preset (MINIPSD_FUZZ), and as a plain function
 * with replay.c's main everywhere else, where the tests replay tests/fuzz/regressions/ through it.
 * Besides the sanitizers it checks what the API promises: MINIPSD_ERR_SHORT always asks for more
 * than the buffer holds, and a decode over the limit fails before it writes (its target is one
 * pixel, so a write past it is an overflow the sanitizer reports).
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Small enough to decode thousands of inputs a second; the limit itself is under test too. */
#define FUZZ_MAX_DECODED ((uint64_t)1 << 22)

static void check_short(const minipsd_reader *r, int rc, size_t size)
{
    if (rc == MINIPSD_ERR_SHORT && minipsd_reader_needed(r) <= size) abort();
}

/* Decodes a width x height image (a layer, or the composite when layer is NULL) whole, then its
   middle row with the matte kept; at 16 bits for a 16-bit file. */
static void decode(minipsd_reader *r, const minipsd_layer_view *layer, uint64_t width, uint64_t height, int depth,
                   size_t size)
{
    minipsd_target      t;
    minipsd_decode_opts all = {0, 0, 0, 0}, band = {0, 1, 1, 0};
    uint16_t            pixel[4];
    uint8_t            *rgba = (uint8_t *)pixel;
    const unsigned      wide = depth == 16;
    uint64_t            bytes = width * height * 4 << wide;
    int                 rc;
    all.depth = band.depth = wide ? 16 : 8;
    if (bytes > FUZZ_MAX_DECODED) {
        minipsd_target_rgba(&t, rgba, 0);
        rc = layer ? minipsd_decode_layer(r, layer, &t, &all) : minipsd_decode_composite(r, &t, &all);
        if (rc == MINIPSD_OK) abort();
        return;
    }
    if (bytes && !(rgba = (uint8_t *)malloc((size_t)bytes))) return;
    if (wide) minipsd_target_rgba16(&t, (uint16_t *)rgba, (size_t)width * 8);
    else      minipsd_target_rgba(&t, rgba, (size_t)width * 4);
    rc = layer ? minipsd_decode_layer(r, layer, &t, &all) : minipsd_decode_composite(r, &t, &all);
    check_short(r, rc, size);
    band.y_first = (uint32_t)(height / 2);
    rc = layer ? minipsd_decode_layer(r, layer, &t, &band) : minipsd_decode_composite(r, &t, &band);
    check_short(r, rc, size);
    if (rgba != (uint8_t *)pixel) free(rgba);
}

/* A layer's mask, as decode does for its pixels: whole, then a band; refused over the limit. */
static void decode_mask(minipsd_reader *r, const minipsd_layer_view *layer, int depth, size_t size)
{
    const uint64_t      width = (uint64_t)((int64_t)layer->mask.right - layer->mask.left);
    const uint64_t      height = (uint64_t)((int64_t)layer->mask.bottom - layer->mask.top);
    const unsigned      wide = depth == 16;
    minipsd_decode_opts all = {0, 0, 0, 0}, band = {0, 1, 0, 0};
    minipsd_plane       plane;
    uint16_t            one;
    int                 rc;
    if (layer->mask.right < layer->mask.left || layer->mask.bottom < layer->mask.top) abort();   /* never inverted */
    all.depth = band.depth = wide ? 16 : 8;
    plane.data = (uint8_t *)&one; plane.pixel_step = (ptrdiff_t)1 << wide; plane.row_step = 0;
    if ((width * height << wide) > FUZZ_MAX_DECODED) {
        if (minipsd_decode_mask(r, layer, &plane, &all) == MINIPSD_OK) abort();
        return;
    }
    if (width && height && !(plane.data = (uint8_t *)malloc((size_t)(width * height << wide)))) return;
    plane.row_step = (ptrdiff_t)(width << wide);
    rc = minipsd_decode_mask(r, layer, &plane, &all);
    check_short(r, rc, size);
    band.y_first = (uint32_t)(height / 2);
    rc = minipsd_decode_mask(r, layer, &plane, &band);
    check_short(r, rc, size);
    if (plane.data != (uint8_t *)&one) free(plane.data);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t            workspace[256];
    size_t             need = 0, pos;
    minipsd_reader    *r;
    minipsd_info       info;
    minipsd_resource   res;
    minipsd_layer_view layer;
    minipsd_block      block;
    minipsd_blending_range range;
    uint32_t           i;
    minipsd_limits     limits = {1u << 16, 0, FUZZ_MAX_DECODED};
    char               name[8];
    int                rc;

    if (minipsd_reader_workspace_size(&need) != MINIPSD_OK || need > sizeof workspace) abort();
    rc = minipsd_reader_init(&r, data, size, workspace, sizeof workspace, &limits, &info);
    check_short(r, rc, size);
    if (rc != MINIPSD_OK) return 0;

    while (minipsd_reader_next_resource(r, &res) == MINIPSD_MORE) {}
    while ((rc = minipsd_reader_next_layer(r, &layer)) == MINIPSD_MORE) {
        (void)minipsd_layer_name(&layer, name, sizeof name, &need);   /* often too small: truncation too */
        for (pos = 0; minipsd_layer_next_block(&layer, &pos, &block) == MINIPSD_MORE;) {}
        for (i = 0; i < layer.blending_range_count; ++i)   /* the count is what the bytes hold */
            if (minipsd_layer_blending_range(&layer, i, &range) != MINIPSD_OK) abort();
        if (layer.right < layer.left || layer.bottom < layer.top) abort();   /* never inverted */
        decode(r, &layer, (uint64_t)((int64_t)layer.right - layer.left), (uint64_t)((int64_t)layer.bottom - layer.top),
               info.depth, size);
        if (layer.has_mask) decode_mask(r, &layer, info.depth, size);
        if (layer.kind > MINIPSD_KIND_GROUP_END) abort();
    }
    check_short(r, rc, size);
    decode(r, NULL, info.width, info.height, info.depth, size);
    return 0;
}
