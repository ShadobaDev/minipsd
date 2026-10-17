/**
 * \file
 * \brief Example: the shortest program that writes a PSD, hello.psd, of one layer.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include <stdio.h>

static int to_file(void *file, const void *data, size_t size)
{
    return fwrite(data, 1, size, (FILE *)file) == size;   /* 0 stops the writer */
}

int main(void)
{
    static uint8_t rgba[64 * 64 * 4];   /* straight alpha, rows top down */
    /* Fields left out are zero, the defaults: visible, the full canvas, RLE. */
    minipsd_layer  layer = {.name = "Hello", .rgba = rgba, .opacity = 255};
    minipsd_doc    doc   = {.width = 64, .height = 64, .dpi = 72.0, .layers = &layer, .layer_count = 1};
    FILE          *f;
    int            i, rc;

    for (i = 0; i < 64 * 64; ++i) {   /* a red-to-blue gradient */
        rgba[i * 4 + 0] = (uint8_t)(255 - i % 64 * 4);
        rgba[i * 4 + 2] = (uint8_t)(i % 64 * 4);
        rgba[i * 4 + 3] = 255;
    }

    f = fopen("hello.psd", "wb");
    if (!f) return 1;
    rc = minipsd_write(&doc, to_file, f);
    if (fclose(f) != 0 && rc == MINIPSD_OK) rc = MINIPSD_ERR_WRITE;
    if (rc != MINIPSD_OK) fprintf(stderr, "hello_write: %s\n", minipsd_error_string(rc));
    return rc != MINIPSD_OK;
}
