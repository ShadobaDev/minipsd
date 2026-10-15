/**
 * \file
 * \brief Compiles minipsd.h as C and writes a small document, so test_writer.cpp can check the
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
    uint8_t         px[3 * 2 * 4];
    minipsd_layer   layers[2];
    minipsd_doc     doc;
    minipsd_writer *w;
    uint8_t         workspace[256], again[1024];
    smoke_buf       b;
    size_t          i, need, n, pulled = 0;
    int             rc;

    for (i = 0; i < sizeof px; ++i) px[i] = (uint8_t)(i * 17);
    layers[0].name = "Template"; layers[0].rgba = px;   layers[0].opacity = 255; layers[0].hidden = 0;
    layers[1].name = "Layer";    layers[1].rgba = NULL; layers[1].opacity = 255; layers[1].hidden = 0;
    memset(&doc, 0, sizeof doc);
    doc.width = 3; doc.height = 2; doc.dpi = 300.0; doc.layers = layers; doc.layer_count = 2;

    /* Every function, so a static build has none unused. */
    if (!minipsd_error_string(MINIPSD_OK)[0] || minipsd_doc_check(&doc) != MINIPSD_OK)
        return -100;

    b.data = out; b.size = 0; b.capacity = capacity;
    rc = minipsd_write(&doc, smoke_out, &b);
    *size = b.size;
    if (rc != MINIPSD_OK) return rc;

    /* The pull writer, from a stack workspace: it knows the size before writing, then gives the
       same bytes 7 at a time. */
    if (minipsd_writer_workspace_size(&doc, &need) != MINIPSD_OK || need > sizeof workspace
        || minipsd_writer_init(&w, &doc, workspace, need) != MINIPSD_OK
        || minipsd_writer_total(w) != b.size || b.size > sizeof again)
        return -101;
    do {
        rc = minipsd_writer_fill(w, again + pulled, 7 < sizeof again - pulled ? 7 : sizeof again - pulled, &n);
        pulled += n;
    } while (rc == MINIPSD_MORE);
    if (rc != MINIPSD_OK || pulled != b.size || memcmp(again, b.data, b.size) != 0)
        return -102;

    /* The reader, over the bytes just written: the file, its resources, its layers and their blocks. */
    {
        minipsd_reader    *rd;
        minipsd_info       info;
        minipsd_resource   res;
        minipsd_layer_view lv;
        minipsd_block      blk;
        uint8_t            rws[256];
        char               name[16];
        size_t             rneed, pos = 0, resources = 0, blocks = 0;

        if (minipsd_reader_workspace_size(&rneed) != MINIPSD_OK || rneed > sizeof rws
            || minipsd_reader_init(&rd, b.data, b.size, rws, rneed, NULL, &info) != MINIPSD_OK
            || info.width != 3 || info.height != 2 || info.layer_count != 2 || minipsd_reader_needed(rd) != 0)
            return -103;
        while (minipsd_reader_next_resource(rd, &res) == MINIPSD_MORE) ++resources;
        if (resources != 1)
            return -104;
        if (minipsd_reader_next_layer(rd, &lv) != MINIPSD_MORE
            || minipsd_layer_name(&lv, name, sizeof name, &rneed) != MINIPSD_OK
            || rneed != 9 || memcmp(name, "Template", 9) != 0 || lv.channel[MINIPSD_CH_R].size == 0)
            return -105;
        while (minipsd_layer_next_block(&lv, &pos, &blk) == MINIPSD_MORE) ++blocks;
        if (blocks != 1 || minipsd_reader_next_layer(rd, &lv) != MINIPSD_MORE
            || minipsd_reader_next_layer(rd, &lv) != MINIPSD_OK)
            return -106;
    }
    return MINIPSD_OK;
}
