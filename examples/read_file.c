/**
 * \file
 * \brief Example: what a PSD holds, printed; every layer and the composite decoded once.
 *
 * The file is read whole into memory, then walked in place: the reader allocates nothing, and its
 * workspace sits on the stack.  Pixels are decoded into one buffer per image, to count how many
 * are not fully transparent and where they lie.
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

/* Decodes width x height pixels through decode (a layer or the composite) and prints what is
   visible: how many pixels are not fully transparent, and the box around them. */
static int show_pixels(minipsd_reader *r, const minipsd_layer_view *layer, uint32_t width, uint32_t height)
{
    minipsd_target t;
    uint8_t       *rgba;
    uint32_t       x, y, x0 = width, y0 = height, x1 = 0, y1 = 0;
    uint64_t       seen = 0;
    int            rc;
    if (!width || !height) { printf("      empty\n"); return MINIPSD_OK; }
    rgba = (uint8_t *)malloc((size_t)width * height * 4);
    if (!rgba) { printf("      out of memory\n"); return MINIPSD_ERR_ALLOC; }
    minipsd_target_rgba(&t, rgba, (size_t)width * 4);
    rc = layer ? minipsd_decode_layer(r, layer, &t, NULL) : minipsd_decode_composite(r, &t, NULL);
    if (rc == MINIPSD_OK) {
        for (y = 0; y < height; ++y)
            for (x = 0; x < width; ++x)
                if (rgba[((size_t)y * width + x) * 4 + 3]) {
                    ++seen;
                    if (x < x0) x0 = x;
                    if (y < y0) y0 = y;
                    if (x >= x1) x1 = x + 1;
                    if (y >= y1) y1 = y + 1;
                }
        if (seen) printf("      %llu visible pixels, within (%u, %u)-(%u, %u)\n", (unsigned long long)seen, x0, y0, x1, y1);
        else      printf("      fully transparent\n");
    } else {
        printf("      not decoded: %s\n", minipsd_error_string(rc));
    }
    free(rgba);
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
    int                rc, failed = 0;
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

    printf("resources:");
    while (minipsd_reader_next_resource(r, &res) == MINIPSD_MORE) printf(" %u (%lu)", (unsigned)res.id, (unsigned long)res.data.size);
    printf("\n%lu layers, bottom first:\n", (unsigned long)info.layer_count);

    while ((rc = minipsd_reader_next_layer(r, &layer)) == MINIPSD_MORE) {
        if (minipsd_layer_name(&layer, name, sizeof name, NULL) != MINIPSD_OK) snprintf(name, sizeof name, "(longer than %u bytes)", (unsigned)sizeof name);
        printf("  %u '%s': (%d, %d)-(%d, %d), opacity %u, %s, blend %s\n", i++, name, (int)layer.left, (int)layer.top,
               (int)layer.right, (int)layer.bottom, (unsigned)layer.opacity, layer.hidden ? "hidden" : "visible", layer.blend);
        printf("      blocks:");
        for (pos = 0; minipsd_layer_next_block(&layer, &pos, &block) == MINIPSD_MORE;) printf(" %s", block.key);
        printf("%s\n", layer.other_channels ? ", and channels not read (masks)" : "");
        if (info.depth == 8 && info.color_mode == 3
            && show_pixels(r, &layer, (uint32_t)(layer.right - layer.left), (uint32_t)(layer.bottom - layer.top)) != MINIPSD_OK)
            failed = 1;
    }
    if (rc != MINIPSD_OK) { printf("  layers: %s\n", minipsd_error_string(rc)); failed = 1; }

    printf("composite:\n");
    if (show_pixels(r, NULL, info.width, info.height) != MINIPSD_OK) failed = 1;
    free(data);
    return failed;
}
