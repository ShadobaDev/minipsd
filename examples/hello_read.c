/**
 * \file
 * \brief Example: the shortest program that reads a PSD: its layer names, and its flattened image
 *        as RGBA.
 *
 * Usage: hello_read [path], hello.psd by default.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    uint8_t            workspace[256];   /* the reader's whole memory, the same for every file */
    uint8_t           *psd, *rgba = NULL;
    long               size;
    minipsd_reader    *r;
    minipsd_info       info;
    minipsd_layer_view layer;
    minipsd_target     target;
    char               name[256];
    int                rc;
    FILE              *f = fopen(argc > 1 ? argv[1] : "hello.psd", "rb");

    if (!f) return 1;   /* the whole file into memory */
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rewind(f);
    psd = size > 0 ? (uint8_t *)malloc((size_t)size) : NULL;
    if (!psd || fread(psd, 1, (size_t)size, f) != (size_t)size) return 1;
    fclose(f);

    rc = minipsd_reader_init(&r, psd, (size_t)size, workspace, sizeof workspace, NULL, &info);
    if (rc == MINIPSD_OK) {
        printf("%u x %u, %u layers\n", info.width, info.height, (unsigned)info.layer_count);
        while (minipsd_reader_next_layer(r, &layer) == MINIPSD_MORE)   /* bottom first */
            if (minipsd_layer_name(&layer, name, sizeof name, NULL) == MINIPSD_OK) printf("layer '%s'\n", name);

        rgba = (uint8_t *)malloc((size_t)info.width * info.height * 4);
        minipsd_target_rgba(&target, rgba, (size_t)info.width * 4);
        rc = rgba ? minipsd_decode_composite(r, &target, NULL) : MINIPSD_ERR_ALLOC;
    }
    if (rc == MINIPSD_OK)
        printf("first pixel: %u %u %u %u\n", rgba[0], rgba[1], rgba[2], rgba[3]);
    else
        fprintf(stderr, "hello_read: %s\n", minipsd_error_string(rc));
    free(rgba);
    free(psd);
    return rc != MINIPSD_OK;
}
