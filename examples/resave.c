/**
 * \file
 * \brief Example: a PSD read, one layer changed, written again; every other layer copied as stored.
 *
 * minipsd_read with stored keeps each layer's channels as the file has them.  The layer to change
 * is decoded through the reader, from the same bytes, and handed back as pixels: the writer
 * encodes it alone and copies the others.  Blocks (text, effects, groups), resources and the
 * composite come back as they were.  The composite is the file's own, so a viewer that ignores
 * layers still shows the old image: rendering a new one is the caller's (libvips does it).
 *
 * Usage: resave in.psd out.psd [layer].  Inverts the colours of that layer, 0 (the bottom one) by
 * default.  Exits with 1 when a file cannot be read or written, or the layer has no pixels.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int to_file(void *file, const void *data, size_t size)
{
    return fwrite(data, 1, size, (FILE *)file) == size;   /* 0 stops the writer */
}

/* Layer `index` of the file, decoded into new memory: its rectangle in RGBA; NULL on failure. */
static uint8_t *decode_layer(const uint8_t *data, size_t size, size_t index)
{
    uint8_t            workspace[256];
    minipsd_reader    *r;
    minipsd_info       info;
    minipsd_layer_view v;
    minipsd_target     t;
    uint8_t           *rgba;
    size_t             i, n;
    memset(&v, 0, sizeof v);
    if (minipsd_reader_init(&r, data, size, workspace, sizeof workspace, NULL, &info) != MINIPSD_OK) return NULL;
    for (i = 0; i <= index; ++i)
        if (minipsd_reader_next_layer(r, &v) != MINIPSD_MORE) return NULL;
    n    = (size_t)(v.right - v.left) * (size_t)(v.bottom - v.top) * 4;
    rgba = (uint8_t *)malloc(n ? n : 1);
    if (!rgba) return NULL;
    minipsd_target_rgba(&t, rgba, (size_t)(v.right - v.left) * 4);
    if (minipsd_decode_layer(r, &v, &t, NULL) != MINIPSD_OK) {
        free(rgba);
        return NULL;
    }
    return rgba;
}

int main(int argc, char **argv)
{
    minipsd_read_opts opts;
    minipsd_doc      *doc;
    minipsd_layer    *layers;
    uint8_t          *data, *pixels;
    size_t            size = 0, index, i, n, copied = 0;
    uint32_t          lost = 0;
    FILE             *f;
    int               rc;

    if (argc < 3) { fputs("usage: resave in.psd out.psd [layer]\n", stderr); return 2; }
    index = argc > 3 ? (size_t)strtoul(argv[3], NULL, 10) : 0;
    data  = read_all(argv[1], &size);
    if (!data) { fprintf(stderr, "resave: cannot read %s\n", argv[1]); return 1; }

    opts.stored = 1;   /* nothing decoded: each layer keeps its channels as stored */
    rc = minipsd_read(data, size, NULL, &opts, &doc, &lost);
    if (rc != MINIPSD_OK) { fprintf(stderr, "resave: %s\n", minipsd_error_string(rc)); free(data); return 1; }
    if (lost) printf("warning: this file holds content minipsd cannot carry yet (lost %lu); please report it\n",
                     (unsigned long)lost);
    if (index >= doc->layer_count || !doc->layers[index].stored) {
        fprintf(stderr, "resave: layer %lu has no stored pixels to change\n", (unsigned long)index);
        minipsd_doc_free(doc);
        free(data);
        return 1;
    }

    /* The one layer to change, decoded from the file; then the file's bytes can go, since the
       document holds copies of everything else. */
    pixels = decode_layer(data, size, index);
    free(data);
    layers = (minipsd_layer *)malloc(doc->layer_count * sizeof *layers);
    if (!pixels || !layers) { fputs("resave: out of memory or bad layer\n", stderr); free(pixels); free(layers); minipsd_doc_free(doc); return 1; }
    memcpy(layers, doc->layers, doc->layer_count * sizeof *layers);
    n = (size_t)(layers[index].right - layers[index].left) * (size_t)(layers[index].bottom - layers[index].top);
    for (i = 0; i < n * 4; ++i)
        if (i % 4 != 3) pixels[i] = (uint8_t)(255 - pixels[i]);   /* invert the colour, keep the alpha */
    layers[index].rgba   = pixels;
    layers[index].stored = NULL;                                  /* encode this one */
    doc->layers = layers;
    for (i = 0; i < doc->layer_count; ++i) copied += layers[i].stored != NULL;

    f = fopen(argv[2], "wb");
    if (!f) rc = MINIPSD_ERR_OPEN;
    else {
        rc = minipsd_write(doc, NULL, to_file, f);
        if (fclose(f) != 0 && rc == MINIPSD_OK) rc = MINIPSD_ERR_WRITE;
    }
    if (rc == MINIPSD_OK)
        printf("layer %lu '%s' inverted; %lu other layers copied as stored\n", (unsigned long)index,
               layers[index].name, (unsigned long)copied);
    else
        fprintf(stderr, "resave: %s: %s\n", argv[2], minipsd_error_string(rc));
    free(layers);
    free(pixels);
    minipsd_doc_free(doc);
    return rc == MINIPSD_OK ? 0 : 1;
}
