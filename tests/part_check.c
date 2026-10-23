/**
 * \file
 * \brief A program that uses part of minipsd: the implementation in this file, private to it
 *        under MINIPSD_STATIC, only writing (as it is, or MINIPSD_NO_READER) or only reading
 *        (MINIPSD_NO_WRITER).  Built, never run, as C and as C++17 (part_check.cpp), each variant
 *        set by tests/CMakeLists.txt: the check is that nothing it leaves uncalled warns.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include <string.h>

#ifdef MINIPSD_NO_WRITER

int main(void)
{
    size_t size = 0;
    return minipsd_reader_workspace_size(&size) == MINIPSD_OK ? 0 : 1;
}

#else

static int discard(void *user, const void *data, size_t size)
{
    (void)user;
    (void)data;
    (void)size;
    return 1;
}

int main(void)
{
    static const uint8_t pixel[4] = {255, 0, 0, 255};
    minipsd_layer        layer;
    minipsd_doc          doc;
    memset(&layer, 0, sizeof layer);
    memset(&doc, 0, sizeof doc);
    layer.name    = "Layer";
    layer.pixels  = pixel;
    layer.opacity = 255;
    doc.width       = 1;
    doc.height      = 1;
    doc.dpi         = 72.0;
    doc.layers      = &layer;
    doc.layer_count = 1;
    return minipsd_write(&doc, NULL, discard, NULL) == MINIPSD_OK ? 0 : 1;
}

#endif
