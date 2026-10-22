/**
 * \file
 * \brief Example: what a PSD holds, printed; every layer and the composite decoded once; then
 *        what a document read from it (minipsd_read) could not carry.
 *
 * The file is read whole into memory, then walked in place: the reader allocates nothing, and its
 * workspace sits on the stack.  Pixels are decoded into one buffer per image, in the file's mode
 * and at its depth, to count how many are not fully transparent and where they lie.
 *
 * Usage: read_file path.  Exits with 1 when the file cannot be read or is refused.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The layer's "Blend If" ranges that are not left alone, as black and white points (0 is the
   composite grey, then each channel). */
static void show_blend_if(const minipsd_layer_view *layer)
{
    minipsd_blending_range br;
    uint32_t               i;
    for (i = 0; minipsd_layer_blending_range(layer, i, &br) == MINIPSD_OK; ++i) {
        const minipsd_blend_range *both[2] = { &br.source, &br.destination };
        int k;
        for (k = 0; k < 2; ++k) {
            const minipsd_blend_range *b = both[k];
            if (b->black[0] || b->black[1] || b->white[0] != 255 || b->white[1] != 255)
                printf("      blend if %u, %s: black %u/%u, white %u/%u\n", (unsigned)i, k ? "underlying" : "this layer",
                       (unsigned)b->black[0], (unsigned)b->black[1], (unsigned)b->white[0], (unsigned)b->white[1]);
        }
    }
}

/* The whole file in a new buffer; NULL when it cannot be read. */
static uint8_t *read_all(const char *path, size_t *size)
{
    uint8_t *data = NULL;
    long     n;
    FILE    *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        *size = (size_t)n;
        data  = (uint8_t *)malloc(*size ? *size : 1);
        if (data && fread(data, 1, *size, f) != *size) {
            free(data);
            data = NULL;
        }
    }
    fclose(f);
    return data;
}

/* Decodes width x height pixels through decode (a layer or the composite), the mode's colour
   channels and transparency interleaved at the file's depth, and prints what is visible: how many
   pixels are not fully transparent, and the box around them.  ZIP, which needs an inflate this
   example does not give (minipsd_decode_opts.inflate), is no failure: the pixels are only
   described. */
static int show_pixels(minipsd_reader *r, const minipsd_layer_view *layer, uint32_t width, uint32_t height,
                       const minipsd_info *info)
{
    const unsigned      planes = info->color_channels < MINIPSD_PLANES ? info->color_channels + 1u : MINIPSD_PLANES;
    const unsigned      sample = info->depth == 32 ? 4u : info->depth == 16 ? 2u : 1u;
    const unsigned      alpha  = info->color_channels;   /* transparency's plane, when there is room for it */
    minipsd_target      t;
    minipsd_decode_opts opts;
    uint8_t            *px;
    uint32_t            x, y, x0 = width, y0 = height, x1 = 0, y1 = 0;
    uint64_t            seen = 0;
    int                 rc;
    if (!width || !height) { printf("      empty\n"); return MINIPSD_OK; }
    px = (uint8_t *)malloc((size_t)width * height * planes * sample);
    if (!px) { printf("      out of memory\n"); return MINIPSD_ERR_ALLOC; }
    memset(&opts, 0, sizeof opts);
    opts.depth = info->depth;
    minipsd_target_interleaved(&t, px, (size_t)width * planes * sample, planes, info->depth);
    rc = layer ? minipsd_decode_layer(r, layer, &t, &opts) : minipsd_decode_composite(r, &t, &opts);
    if (rc == MINIPSD_OK) {
        for (y = 0; y < height; ++y)
            for (x = 0; x < width; ++x) {
                const uint8_t *a = px + (((size_t)y * width + x) * planes + alpha) * sample;
                unsigned       k, shown = alpha >= planes;   /* no transparency plane: every pixel shows */
                for (k = 0; k < sample && !shown; ++k) shown = a[k] != 0;
                if (!shown) continue;
                ++seen;
                if (x < x0) x0 = x;
                if (y < y0) y0 = y;
                if (x >= x1) x1 = x + 1;
                if (y >= y1) y1 = y + 1;
            }
        if (seen) printf("      %llu visible pixels, within (%u, %u)-(%u, %u)\n", (unsigned long long)seen, x0, y0, x1, y1);
        else      printf("      fully transparent\n");
    } else if (rc == MINIPSD_ERR_UNSUPPORTED) {
        printf("      ZIP-compressed: described, not decoded\n");
        rc = MINIPSD_OK;
    } else {
        printf("      not decoded: %s\n", minipsd_error_string(rc));
    }
    free(px);
    return rc;
}

static const char *mode_name(unsigned mode)
{
    static const char *const names[10] = {"bitmap", "grey", "indexed", "RGB", "CMYK", "?", "?", "multichannel", "duotone", "Lab"};
    return mode < 10 ? names[mode] : "?";
}

int main(int argc, char **argv)
{
    uint8_t            workspace[256], *data;
    size_t             size = 0, need = 0, pos;
    minipsd_reader    *r;
    minipsd_info       info;
    minipsd_resource   res;
    minipsd_layer_view layer;
    minipsd_block      block;
    char               name[256];
    int                rc, failed = 0, rgb;
    unsigned           i = 0;

    if (argc < 2) { fputs("usage: read_file path\n", stderr); return 2; }
    data = read_all(argv[1], &size);
    if (!data) { fprintf(stderr, "read_file: cannot read %s\n", argv[1]); return 1; }
    if (minipsd_reader_workspace_size(&need) != MINIPSD_OK || need > sizeof workspace) return 1;

    rc = minipsd_reader_init(&r, data, size, workspace, sizeof workspace, NULL, &info);
    if (rc != MINIPSD_OK) {
        fprintf(stderr, "read_file: %s\n", minipsd_error_string(rc));
        free(data);
        return 1;
    }
    printf("%u x %u, %u-bit %s, %u channels, %g dpi, ICC profile: %lu bytes\n", info.width, info.height,
           (unsigned)info.depth, mode_name(info.color_mode), (unsigned)info.channels, info.dpi, (unsigned long)info.icc.size);
    rgb = (info.depth == 8 || info.depth == 16) && info.color_mode == 3;   /* what minipsd_read takes */

    printf("resources:");
    for (pos = 0; minipsd_reader_next_resource(r, &pos, &res) == MINIPSD_MORE;) {
        const char *res_name = minipsd_resource_id_name(res.id);   /* Adobe's name; NULL for others */
        printf(" %u %s (%lu)", (unsigned)res.id, res_name ? res_name : "?", (unsigned long)res.data.size);
    }
    printf("\n%lu layers, bottom first:\n", (unsigned long)info.layer_count);

    while ((rc = minipsd_reader_next_layer(r, &layer)) == MINIPSD_MORE) {
        if (minipsd_layer_name(&layer, name, sizeof name, NULL) != MINIPSD_OK) snprintf(name, sizeof name, "(longer than %u bytes)", (unsigned)sizeof name);
        printf("  %u '%s': (%d, %d)-(%d, %d), opacity %u, %s, blend %s\n", i++, name, (int)layer.left, (int)layer.top,
               (int)layer.right, (int)layer.bottom, (unsigned)layer.opacity, layer.hidden ? "hidden" : "visible", layer.blend_mode);
        printf("      blocks:");
        for (pos = 0; minipsd_layer_next_block(&layer, &pos, &block) == MINIPSD_MORE;) {
            printf(" %s", block.key);
            if (strcmp(block.key, "lsct") == 0 && block.data.size >= 4)   /* 1, 2: a group opens; 3: it closes */
                printf("=%u", (unsigned)block.data.data[3]);
        }
        printf("%s%s\n", layer.has_mask ? ", mask" : "", layer.other_channels ? ", and channels not read" : "");
        show_blend_if(&layer);
        if (show_pixels(r, &layer, (uint32_t)(layer.right - layer.left), (uint32_t)(layer.bottom - layer.top), &info) != MINIPSD_OK)
            failed = 1;
    }
    if (rc != MINIPSD_OK) { printf("  layers: %s\n", minipsd_error_string(rc)); failed = 1; }

    printf("composite:\n");
    if (show_pixels(r, NULL, info.width, info.height, &info) != MINIPSD_OK) failed = 1;

    /* The same file as a document, which minipsd_write takes back: anything lost is worth an issue.
       ZIP channels are not decoded without an inflate; read as stored, they are copied. */
    if (rgb) {
        minipsd_doc      *doc;
        minipsd_read_opts stored;
        uint32_t          lost = 0;
        rc = minipsd_read(data, size, NULL, NULL, &doc, &lost);
        if (rc == MINIPSD_ERR_UNSUPPORTED) {
            memset(&stored, 0, sizeof stored);
            stored.stored = 1;
            rc = minipsd_read(data, size, NULL, &stored, &doc, &lost);
            if (rc == MINIPSD_OK) printf("as a document, ZIP channels as stored:\n");
        }
        if (rc != MINIPSD_OK) { printf("as a document: %s\n", minipsd_error_string(rc)); failed = 1; }
        else if (!lost)       printf("as a document: everything kept\n");
        else printf("as a document, lost:%s\n", lost & MINIPSD_LOST_CHANNELS ? " channels" : "");
        minipsd_doc_free(doc);
    }
    free(data);
    return failed;
}
