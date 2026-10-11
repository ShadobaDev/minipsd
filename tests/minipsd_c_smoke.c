/**
 * \file
 * \brief Compiles minipsd.h as C and writes a small document, so test_minipsd.cpp can check the
 *        C build produces the same bytes as the C++ one.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_STATIC
#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

typedef struct smoke_buf {
    unsigned char *data;
    size_t         size, capacity;
} smoke_buf;

static int smoke_out(void *user, const void *data, size_t size)
{
    smoke_buf *b = (smoke_buf *)user;
    if (b->size + size > b->capacity) return 0;
    memcpy(b->data + b->size, data, size);
    b->size += size;
    return 1;
}

int minipsd_c_smoke(unsigned char *out, size_t capacity, size_t *size);

int minipsd_c_smoke(unsigned char *out, size_t capacity, size_t *size)
{
    uint8_t       px[3 * 2 * 4];
    minipsd_layer layers[2];
    minipsd_doc   doc;
    smoke_buf     b;
    size_t        i;
    int           rc;

    for (i = 0; i < sizeof px; ++i) px[i] = (uint8_t)(i * 17);
    layers[0].name = "Template"; layers[0].rgba = px;   layers[0].opacity = 255; layers[0].hidden = 0;
    layers[1].name = "Layer";    layers[1].rgba = NULL; layers[1].opacity = 255; layers[1].hidden = 0;
    memset(&doc, 0, sizeof doc);
    doc.width = 3; doc.height = 2; doc.dpi = 300.0; doc.layers = layers; doc.layer_count = 2;

    /* Every function, so a static build has none unused. */
    if (minipsd_write_file(NULL, NULL) != MINIPSD_ERR_ARG || !minipsd_error_string(MINIPSD_OK)[0])
        return -100;

    b.data = out; b.size = 0; b.capacity = capacity;
    rc = minipsd_write(&doc, smoke_out, &b);
    *size = b.size;
    return rc;
}
