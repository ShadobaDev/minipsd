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
    minipsd_blending_range blend_if = { { { 0, 0 }, { 255, 255 } }, { { 59, 59 }, { 210, 210 } } };
    minipsd_doc     doc;
    minipsd_writer *w;
    uint8_t         workspace[384], again[1024];
    smoke_buf       b;
    size_t          i, need, n, pulled = 0;
    int             rc;

    for (i = 0; i < sizeof px; ++i) px[i] = (uint8_t)(i * 17);
    memset(layers, 0, sizeof layers);   /* the structs grow at the end: zero is every default */
    layers[0].name = "Template"; layers[0].rgba = px;   layers[0].opacity = 255; layers[0].hidden = 0;
    layers[1].name = "Layer";    layers[1].rgba = NULL; layers[1].opacity = 255; layers[1].hidden = 0;
    layers[1].blending_ranges = &blend_if; layers[1].blending_range_count = 1;
    memset(&doc, 0, sizeof doc);
    doc.width = 3; doc.height = 2; doc.dpi = 300.0; doc.layers = layers; doc.layer_count = 2;

    /* Every function, so a static build has none unused. */
    if (!minipsd_error_string(MINIPSD_OK)[0] || minipsd_doc_check(&doc, NULL) != MINIPSD_OK)
        return -100;
    layers[1].blending_range_count = 0xFFFFFFFFu;   /* more than a record holds */
    rc = minipsd_doc_check(&doc, NULL);
    layers[1].blending_ranges = NULL;               /* a count without ranges */
    if (rc != MINIPSD_ERR_SIZE || minipsd_doc_check(&doc, NULL) != MINIPSD_ERR_ARG)
        return -112;
    layers[1].blending_ranges = &blend_if; layers[1].blending_range_count = 1;

    b.data = out; b.size = 0; b.capacity = capacity;
    rc = minipsd_write(&doc, NULL, smoke_out, &b);
    *size = b.size;
    if (rc != MINIPSD_OK) return rc;

    /* The pull writer, from a stack workspace: it knows the size before writing, then gives the
       same bytes 7 at a time. */
    if (minipsd_writer_workspace_size(&doc, NULL, &need) != MINIPSD_OK || need > sizeof workspace
        || minipsd_writer_init(&w, &doc, NULL, workspace, need) != MINIPSD_OK
        || minipsd_writer_total(w) != b.size || b.size > sizeof again)
        return -101;
    do {
        rc = minipsd_writer_fill(w, again + pulled, 7 < sizeof again - pulled ? 7 : sizeof again - pulled, &n);
        pulled += n;
    } while (rc == MINIPSD_MORE);
    if (rc != MINIPSD_OK || pulled != b.size || memcmp(again, b.data, b.size) != 0)
        return -102;

    /* The reader, over the bytes just written: the file, its resources, its layers, their pixels and
       their blocks, the composite. */
    {
        minipsd_reader    *rd;
        minipsd_info       info;
        minipsd_resource   res;
        minipsd_layer_view lv;
        minipsd_block      blk;
        minipsd_blending_range br;
        minipsd_target     t;
        uint8_t            got[sizeof px];
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
        minipsd_target_rgba(&t, got, sizeof got / 2);   /* two rows */
        if (minipsd_decode_layer(rd, &lv, &t, NULL) != MINIPSD_OK || memcmp(got, px, sizeof px) != 0)
            return -107;
        if (minipsd_decode_composite(rd, &t, NULL) != MINIPSD_OK || got[3] != px[3])   /* one layer: its alpha */
            return -108;
        while (minipsd_layer_next_block(&lv, &pos, &blk) == MINIPSD_MORE) ++blocks;
        if (blocks != 1 || minipsd_reader_next_layer(rd, &lv) != MINIPSD_MORE
            || minipsd_layer_blending_range(&lv, 0, &br) != MINIPSD_OK || br.destination.white[0] != 210
            || minipsd_reader_next_layer(rd, &lv) != MINIPSD_OK)
            return -106;
    }

    /* The document read back from a copy of the file, whole and standalone: with the copy wiped,
       it writes the same bytes. */
    {
        minipsd_doc *rdoc;
        uint32_t     lost = 1;
        uint8_t      copy[1024], back[1024];
        smoke_buf    c;
        memcpy(copy, b.data, b.size);
        if (minipsd_read(copy, b.size, NULL, NULL, &rdoc, &lost) != MINIPSD_OK || lost != 0)
            return -109;
        memset(copy, 0, sizeof copy);
        c.data = back; c.size = 0; c.capacity = sizeof back;
        rc = minipsd_write(rdoc, NULL, smoke_out, &c);
        minipsd_doc_free(rdoc);
        if (rc != MINIPSD_OK || c.size != b.size || memcmp(back, b.data, b.size) != 0)
            return -110;

        /* As stored: nothing decoded, every byte copied, the same file again. */
        {
            minipsd_read_opts keep;
            memset(&keep, 0, sizeof keep);   /* fields grow at the end: zero is the old behaviour */
            keep.stored = 1;
            if (minipsd_read(b.data, b.size, NULL, &keep, &rdoc, &lost) != MINIPSD_OK || lost != 0)
                return -113;
            c.size = 0;
            rc = minipsd_write(rdoc, NULL, smoke_out, &c);
            minipsd_doc_free(rdoc);
            if (rc != MINIPSD_OK || c.size != b.size || memcmp(back, b.data, b.size) != 0)
                return -114;
        }
    }

    /* 16 bits: the same layer at 257 times each sample, written and decoded back. */
    {
        uint16_t            px16[3 * 2 * 4], got16[3 * 2 * 4];
        uint8_t             file[1024], rws[256];
        minipsd_reader     *rd;
        minipsd_info        info;
        minipsd_layer_view  lv;
        minipsd_target      t;
        minipsd_decode_opts deep;
        smoke_buf           c;
        for (i = 0; i < sizeof px16 / sizeof px16[0]; ++i) px16[i] = (uint16_t)(px[i] * 257u);
        layers[0].rgba = (const uint8_t *)px16;
        doc.depth      = 16;
        c.data = file; c.size = 0; c.capacity = sizeof file;
        memset(&deep, 0, sizeof deep);
        deep.depth = 16;
        minipsd_target_rgba16(&t, got16, (size_t)3 * 8);
        if (minipsd_write(&doc, NULL, smoke_out, &c) != MINIPSD_OK
            || minipsd_reader_init(&rd, file, c.size, rws, sizeof rws, NULL, &info) != MINIPSD_OK || info.depth != 16
            || minipsd_reader_next_layer(rd, &lv) != MINIPSD_MORE
            || minipsd_decode_layer(rd, &lv, &t, &deep) != MINIPSD_OK || memcmp(got16, px16, sizeof px16) != 0)
            return -111;
        memset(got16, 0, sizeof got16);   /* the same pixels, through the target of any mode */
        minipsd_target_interleaved(&t, got16, (size_t)3 * 8, 4, 16);
        if (minipsd_decode_layer(rd, &lv, &t, &deep) != MINIPSD_OK || memcmp(got16, px16, sizeof px16) != 0)
            return -115;
    }
    return MINIPSD_OK;
}
