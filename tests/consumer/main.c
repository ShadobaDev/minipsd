/**
 * \file
 * \brief Writes a one-pixel document and checks the header reports the expected version.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 ShadobaDev
 */

#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include <stdio.h>
#include <string.h>

static int count(void *user, const void *data, size_t size)
{
    (void)data;
    *(size_t *)user += size;
    return 1;
}

int main(void)
{
    static const uint8_t pixel[4] = { 255, 105, 180, 255 };
    minipsd_layer layer = { "Layer", pixel, 255, 0 };
    minipsd_doc   doc;
    size_t        size = 0;
    int           rc;

    if (strcmp(MINIPSD_VERSION_STRING, MINIPSD_EXPECTED_VERSION) != 0) {
        printf("header version %s, expected %s\n", MINIPSD_VERSION_STRING, MINIPSD_EXPECTED_VERSION);
        return 1;
    }
    memset(&doc, 0, sizeof doc);
    doc.width = 1; doc.height = 1; doc.dpi = 72.0; doc.layers = &layer; doc.layer_count = 1;
    rc = minipsd_write(&doc, count, &size);
    printf("minipsd %s: %s, %u bytes\n", MINIPSD_VERSION_STRING, minipsd_error_string(rc), (unsigned)size);
    return rc == MINIPSD_OK && size > 0 ? 0 : 2;
}
