/**
 * \file minipsd/builders.h
 * \brief Implementation: typed setters, a resource's bytes made from plain values: the mirror of
 *        the typed views (views.h).  Included by minipsd.h under MINIPSD_IMPLEMENTATION, with the
 *        writer; the declarations are in minipsd.h.
 *
 * Each setter describes its bytes once, to a sink: counted first, then, when the caller's buffer
 * holds them, written into it.  Nothing is allocated or kept.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MPSD__BUILDERS_DONE
#define MPSD__BUILDERS_DONE

#include "detail/bytes.h"
#include "detail/format.h"
#include "detail/utf.h"

#include <string.h>

/* --- the bytes, counted then written --- */

/* What a setter makes: its bytes described to a sink, from what. */
typedef void (*mpsd__make_fn)(mpsd__sink *s, const void *what);

/* Where the next bytes go in the caller's buffer. */
typedef struct mpsd__memory {
    uint8_t *at;
} mpsd__memory;

static int mpsd__to_memory(void *user, const void *data, size_t size)
{
    mpsd__memory *m = (mpsd__memory *)user;
    memcpy(m->at, data, size);
    m->at += size;
    return 1;
}

/* make's bytes into buf when cap holds them, their size into *needed either way. */
static int mpsd__made(mpsd__make_fn make, const void *what, uint8_t *buf, size_t cap, size_t *needed)
{
    mpsd__sink   count = { NULL, NULL, 0, 0 };
    mpsd__sink   out   = { mpsd__to_memory, NULL, 0, 0 };
    mpsd__memory to;
    if (!buf && cap) return MINIPSD_ERR_ARG;
    make(&count, what);
    if (needed) *needed = (size_t)count.count;
    if (cap < count.count) return MINIPSD_MORE;
    to.at    = buf;
    out.user = &to;
    make(&out, what);
    return MINIPSD_OK;
}

/* --- text --- */

static void mpsd__make_utf16be(mpsd__sink *s, const void *utf8)
{
    (void)mpsd__utf16((const char *)utf8, mpsd__unit_out, s);   /* writer.h */
}

MINIPSDDEF int minipsd_utf8_to_utf16be(const char *utf8, uint8_t *utf16, size_t cap, size_t *needed)
{
    return mpsd__made(mpsd__make_utf16be, utf8 ? utf8 : "", utf16, cap, needed);
}

/* --- image resources --- */

MINIPSDDEF int minipsd_resource_make_resolution_info(const minipsd_resolution_info *info, uint8_t *buf, size_t cap,
                                                     size_t *needed)
{
    (void)info;
    (void)buf;
    (void)cap;
    (void)needed;
    return MINIPSD_ERR_USE_DOC_FIELD;
}

/* A name of version info: its count of UTF-16 units, then the units, as the view reads it. */
static int mpsd__name_ok(const minipsd_bytes *name) { return name->size % 2 == 0 && (name->data || !name->size); }

static void mpsd__unicode_out(mpsd__sink *s, const minipsd_bytes *text)
{
    mpsd__u32(s, (uint32_t)(text->size / 2));
    mpsd__put(s, text->data, text->size);
}

static void mpsd__make_version_info(mpsd__sink *s, const void *what)
{
    const minipsd_version_info *v = (const minipsd_version_info *)what;
    mpsd__u32(s, v->version);                    /* MPSD__VERSION_INFO_HEAD: the version, the flag */
    mpsd__u8(s, v->has_real_merged_data);
    mpsd__unicode_out(s, &v->writer_name);
    mpsd__unicode_out(s, &v->reader_name);
    mpsd__u32(s, v->file_version);
}

MINIPSDDEF int minipsd_resource_make_version_info(const minipsd_version_info *info, uint8_t *buf, size_t cap,
                                                  size_t *needed)
{
    if (!info || !mpsd__name_ok(&info->writer_name) || !mpsd__name_ok(&info->reader_name)) return MINIPSD_ERR_ARG;
    return mpsd__made(mpsd__make_version_info, info, buf, cap, needed);
}

static void mpsd__make_transparency_index(mpsd__sink *s, const void *what)
{
    mpsd__u16(s, *(const uint16_t *)what);       /* MINIPSD_TRANSPARENCY_INDEX_SIZE */
}

MINIPSDDEF int minipsd_resource_make_transparency_index(uint16_t index, uint8_t *buf, size_t cap, size_t *needed)
{
    return mpsd__made(mpsd__make_transparency_index, &index, buf, cap, needed);
}

/* --- the lists: guides, alpha channels' names and identifiers --- */

typedef struct mpsd__guides_in {
    const minipsd_grid_and_guides *grid;
    const minipsd_guide           *guides;
    size_t                         count;
} mpsd__guides_in;

static void mpsd__make_grid_and_guides(mpsd__sink *s, const void *what)
{
    const mpsd__guides_in *in = (const mpsd__guides_in *)what;
    size_t i;
    mpsd__u32(s, in->grid->version);             /* MPSD__GUIDES_HEAD */
    mpsd__u32(s, in->grid->horizontal_grid);
    mpsd__u32(s, in->grid->vertical_grid);
    mpsd__u32(s, (uint32_t)in->count);
    for (i = 0; i < in->count; ++i) {            /* MPSD__GUIDE_SIZE each */
        mpsd__u32(s, (uint32_t)in->guides[i].location);
        mpsd__u8(s, in->guides[i].direction);
    }
}

MINIPSDDEF int minipsd_resource_make_grid_and_guides(const minipsd_grid_and_guides *grid, const minipsd_guide *guides,
                                                     size_t count, uint8_t *buf, size_t cap, size_t *needed)
{
    mpsd__guides_in in;
    if (!grid || (count && !guides) || (uint64_t)count > UINT32_MAX) return MINIPSD_ERR_ARG;
    in.grid   = grid;
    in.guides = guides;
    in.count  = count;
    return mpsd__made(mpsd__make_grid_and_guides, &in, buf, cap, needed);
}

typedef struct mpsd__names_in {
    unsigned           unicode;               /* 1045: Unicode strings; 1006: Pascal strings */
    const char *const *names;
    size_t             count;
} mpsd__names_in;

static void mpsd__make_alpha_names(mpsd__sink *s, const void *what)
{
    const mpsd__names_in *in = (const mpsd__names_in *)what;
    size_t i;
    for (i = 0; i < in->count; ++i) {
        if (in->unicode) {                       /* its count of UTF-16 units, then the units */
            mpsd__u32(s, (uint32_t)mpsd__utf16(in->names[i], NULL, NULL));
            (void)mpsd__utf16(in->names[i], mpsd__unit_out, s);
        } else {                                 /* a Pascal string, not padded */
            uint8_t pascal[MPSD__PASCAL_MAX + 1];
            mpsd__put(s, pascal, mpsd__legacy_name(in->names[i], pascal) + 1);   /* writer.h */
        }
    }
}

MINIPSDDEF int minipsd_resource_make_alpha_names(uint16_t id, const char *const *names, size_t count, uint8_t *buf,
                                                 size_t cap, size_t *needed)
{
    mpsd__names_in in;
    if ((id != MINIPSD_RES_UNICODE_ALPHA_NAMES && id != MINIPSD_RES_ALPHA_NAMES) || (count && !names))
        return MINIPSD_ERR_ARG;
    in.unicode = id == MINIPSD_RES_UNICODE_ALPHA_NAMES;
    in.names   = names;
    in.count   = count;
    return mpsd__made(mpsd__make_alpha_names, &in, buf, cap, needed);
}

typedef struct mpsd__ids_in {
    const uint32_t *ids;
    size_t          count;
} mpsd__ids_in;

static void mpsd__make_alpha_identifiers(mpsd__sink *s, const void *what)
{
    const mpsd__ids_in *in = (const mpsd__ids_in *)what;
    size_t i;
    for (i = 0; i < in->count; ++i) mpsd__u32(s, in->ids[i]);
}

MINIPSDDEF int minipsd_resource_make_alpha_identifiers(const uint32_t *ids, size_t count, uint8_t *buf, size_t cap,
                                                       size_t *needed)
{
    mpsd__ids_in in;
    if (count && !ids) return MINIPSD_ERR_ARG;
    in.ids   = ids;
    in.count = count;
    return mpsd__made(mpsd__make_alpha_identifiers, &in, buf, cap, needed);
}

/* --- a layer's fixed-layout blocks --- */

typedef struct mpsd__settings_in {
    minipsd_bytes                 blocks;
    unsigned                      psb;
    const minipsd_block_settings *settings;
} mpsd__settings_in;

/* Setting k's block: the signature given, its key, the length, the value, then zeros. */
static void mpsd__setting_out(mpsd__sink *s, const uint8_t *signature, const minipsd_block_settings *settings, size_t k)
{
    const uint32_t v = mpsd__setting_get(settings, k);
    mpsd__put(s, signature, MPSD__BLOCK_KEY);
    mpsd__put(s, mpsd__settings[k].key, MPSD__BLOCK_KEY);
    mpsd__u32(s, mpsd__settings[k].span);                 /* a short key's length: four bytes in a PSB too */
    if (mpsd__settings[k].size == 4)      mpsd__u32(s, v);
    else if (mpsd__settings[k].size == 2) mpsd__u16(s, v);
    else                                  mpsd__u8(s, v);
    mpsd__zeros(s, (size_t)(mpsd__settings[k].span - mpsd__settings[k].size));
}

/* The run: each block as it is, but the first of each key present asks for, made anew in its
   place; then those the run lacked, in the table's order.  mpsd__settings_ok walked it. */
static void mpsd__make_settings(mpsd__sink *s, const void *what)
{
    const mpsd__settings_in *in = (const mpsd__settings_in *)what;
    unsigned      done = 0;
    size_t        pos, next, k;
    minipsd_block block;
    for (pos = 0; pos < in->blocks.size; pos = next) {
        const uint8_t *p = in->blocks.data + pos;
        (void)mpsd__block(in->blocks, pos, in->psb, &block, &next);
        for (k = 0; k < MPSD__SETTINGS; ++k)
            if (mpsd__tag((const uint8_t *)block.key, mpsd__settings[k].key)) break;
        if (k < MPSD__SETTINGS && (in->settings->present & mpsd__settings[k].bit) && !(done & mpsd__settings[k].bit)) {
            mpsd__setting_out(s, p, in->settings, k);
            done |= mpsd__settings[k].bit;
        } else {
            mpsd__put(s, p, next - pos);
        }
    }
    for (k = 0; k < MPSD__SETTINGS; ++k)
        if ((in->settings->present & mpsd__settings[k].bit) && !(done & mpsd__settings[k].bit))
            mpsd__setting_out(s, (const uint8_t *)"8BIM", in->settings, k);
}

MINIPSDDEF int minipsd_layer_make_block_settings(const minipsd_bytes *blocks, int format, const minipsd_block_settings *settings,
                                                 uint8_t *buf, size_t cap, size_t *needed)
{
    static const minipsd_bytes none = { NULL, 0 };
    mpsd__settings_in          in;
    minipsd_block              block;
    size_t                     pos, next;
    if (!settings || (blocks && !blocks->data && blocks->size) || (format != 0 && format != MINIPSD_PSD && format != MINIPSD_PSB))
        return MINIPSD_ERR_ARG;
    in.blocks   = blocks ? *blocks : none;
    in.psb      = format == MINIPSD_PSB;
    in.settings = settings;
    for (pos = 0; pos < in.blocks.size; pos = next)        /* a run of blocks to its end */
        if (mpsd__block(in.blocks, pos, in.psb, &block, &next) != MINIPSD_OK) return MINIPSD_ERR_FORMAT;
    return mpsd__made(mpsd__make_settings, &in, buf, cap, needed);
}

#endif /* MPSD__BUILDERS_DONE */
