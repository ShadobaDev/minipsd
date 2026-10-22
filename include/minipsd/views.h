/**
 * \file minipsd/views.h
 * \brief Implementation: typed views, what a kept resource or block says parsed into plain values.
 *        Included by minipsd.h under MINIPSD_IMPLEMENTATION; the declarations are in minipsd.h.
 *
 * Each function reads only its arguments: no reader, nothing allocated, nothing kept.  A view in, a
 * small struct out; a view shorter than its layout is MINIPSD_ERR_FORMAT, never a partial struct.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MPSD__VIEWS_DONE
#define MPSD__VIEWS_DONE

#include "detail/bytes.h"
#include "detail/format.h"
#include "detail/utf.h"

/* --- text --- */

MINIPSDDEF int minipsd_utf16be_to_utf8(const minipsd_bytes *utf16, char *utf8, size_t cap, size_t *needed)
{
    size_t n;
    if (!utf16 || (!utf16->data && utf16->size) || (!utf8 && cap)) return MINIPSD_ERR_ARG;
    n = mpsd__utf16be_utf8(utf16->data, utf16->size, NULL) + 1;
    if (needed) *needed = n;
    if (cap < n) {
        if (cap) utf8[0] = '\0';
        return MINIPSD_MORE;
    }
    mpsd__utf16be_utf8(utf16->data, utf16->size, (uint8_t *)utf8);
    utf8[n - 1] = '\0';
    return MINIPSD_OK;
}

/* --- image resources --- */

/* Is res a resource of that id, its data readable? */
static int mpsd__resource_of(const minipsd_resource *res, unsigned id)
{
    return res && res->id == id && (res->data.data || !res->data.size);
}

MINIPSDDEF int minipsd_resource_resolution_info(const minipsd_resource *res, minipsd_resolution_info *out)
{
    const uint8_t *p;
    if (!out || !mpsd__resource_of(res, MINIPSD_RES_RESOLUTION_INFO)) return MINIPSD_ERR_ARG;
    if (res->data.size < MPSD__RESOLUTION_SIZE) return MINIPSD_ERR_FORMAT;
    p = res->data.data;                                        /* detail/format.h */
    out->horizontal_resolution      = (double)mpsd__get32(p) / MPSD__FIXED_ONE;
    out->horizontal_resolution_unit = (uint16_t)mpsd__get16(p + 4);
    out->width_unit                 = (uint16_t)mpsd__get16(p + 6);
    out->vertical_resolution        = (double)mpsd__get32(p + 8) / MPSD__FIXED_ONE;
    out->vertical_resolution_unit   = (uint16_t)mpsd__get16(p + 12);
    out->height_unit                = (uint16_t)mpsd__get16(p + 14);
    return MINIPSD_OK;
}

/* A Unicode string at *p, in [*p, end): a count of UTF-16 units (four bytes), then the units.
   *text receives the units; *p moves past them. */
static int mpsd__unicode_string(const uint8_t **p, const uint8_t *end, minipsd_bytes *text)
{
    uint64_t size;
    if ((size_t)(end - *p) < MPSD__LEN) return MINIPSD_ERR_FORMAT;
    size = (uint64_t)mpsd__get32(*p) * 2;
    *p  += MPSD__LEN;
    if (size > (uint64_t)(end - *p)) return MINIPSD_ERR_FORMAT;
    text->data = *p;
    text->size = (size_t)size;
    *p        += (size_t)size;
    return MINIPSD_OK;
}

MINIPSDDEF int minipsd_resource_version_info(const minipsd_resource *res, minipsd_version_info *out)
{
    const uint8_t       *p, *end;
    minipsd_version_info v;
    if (!out || !mpsd__resource_of(res, MINIPSD_RES_VERSION_INFO)) return MINIPSD_ERR_ARG;
    if (res->data.size < MPSD__VERSION_INFO_HEAD) return MINIPSD_ERR_FORMAT;
    p   = res->data.data;                                      /* detail/format.h */
    end = p + res->data.size;
    v.version              = mpsd__get32(p);
    v.has_real_merged_data = p[4];
    p += MPSD__VERSION_INFO_HEAD;
    if (mpsd__unicode_string(&p, end, &v.writer_name) != MINIPSD_OK
        || mpsd__unicode_string(&p, end, &v.reader_name) != MINIPSD_OK || (size_t)(end - p) < MPSD__LEN)
        return MINIPSD_ERR_FORMAT;
    v.file_version = mpsd__get32(p);
    *out = v;
    return MINIPSD_OK;
}

/* Grid and guides: the header (version, two grid values, the guide count), then each guide. */
static int mpsd__guides(const minipsd_resource *res, minipsd_grid_and_guides *out)
{
    const uint8_t *p = res->data.data;
    uint32_t       count;
    if (res->data.size < MPSD__GUIDES_HEAD) return MINIPSD_ERR_FORMAT;
    count = mpsd__get32(p + 12);
    if ((uint64_t)count * MPSD__GUIDE_SIZE > res->data.size - MPSD__GUIDES_HEAD) return MINIPSD_ERR_FORMAT;
    out->version         = mpsd__get32(p);
    out->horizontal_grid = mpsd__get32(p + 4);
    out->vertical_grid   = mpsd__get32(p + 8);
    out->guide_count     = count;
    return MINIPSD_OK;
}

MINIPSDDEF int minipsd_resource_grid_and_guides(const minipsd_resource *res, minipsd_grid_and_guides *out)
{
    minipsd_grid_and_guides g;
    int                     rc;
    if (!out || !mpsd__resource_of(res, MINIPSD_RES_GRID_AND_GUIDES)) return MINIPSD_ERR_ARG;
    rc = mpsd__guides(res, &g);
    if (rc == MINIPSD_OK) *out = g;
    return rc;
}

MINIPSDDEF int minipsd_resource_next_guide(const minipsd_resource *res, size_t *pos, minipsd_guide *guide)
{
    minipsd_grid_and_guides g;
    size_t                  at, end;
    int                     rc;
    if (!pos || !guide || !mpsd__resource_of(res, MINIPSD_RES_GRID_AND_GUIDES)) return MINIPSD_ERR_ARG;
    rc = mpsd__guides(res, &g);
    if (rc != MINIPSD_OK) return rc;
    at  = *pos ? *pos : MPSD__GUIDES_HEAD;
    end = MPSD__GUIDES_HEAD + (size_t)g.guide_count * MPSD__GUIDE_SIZE;
    if (at < MPSD__GUIDES_HEAD || (at - MPSD__GUIDES_HEAD) % MPSD__GUIDE_SIZE) return MINIPSD_ERR_ARG;
    if (at >= end) return MINIPSD_OK;
    guide->location  = (int32_t)mpsd__get32(res->data.data + at);
    guide->direction = res->data.data[at + 4];
    *pos = at + MPSD__GUIDE_SIZE;
    return MINIPSD_MORE;
}

MINIPSDDEF int minipsd_resource_next_alpha_name(const minipsd_resource *res, size_t *pos, minipsd_bytes *name)
{
    const uint8_t *p, *end;
    minipsd_bytes  text;
    if (!pos || !name || !res || (res->id != MINIPSD_RES_ALPHA_NAMES && res->id != MINIPSD_RES_UNICODE_ALPHA_NAMES)
        || !mpsd__resource_of(res, res->id))
        return MINIPSD_ERR_ARG;
    if (*pos >= res->data.size) return MINIPSD_OK;
    p   = res->data.data + *pos;
    end = res->data.data + res->data.size;
    if (res->id == MINIPSD_RES_ALPHA_NAMES) {                  /* a Pascal string, not padded */
        if ((size_t)(end - p) < 1u + p[0]) return MINIPSD_ERR_FORMAT;
        text.data = p + 1;
        text.size = p[0];
        p += 1u + p[0];
    } else if (mpsd__unicode_string(&p, end, &text) != MINIPSD_OK) {
        return MINIPSD_ERR_FORMAT;
    }
    *name = text;
    *pos  = (size_t)(p - res->data.data);
    return MINIPSD_MORE;
}

MINIPSDDEF int minipsd_resource_next_alpha_identifier(const minipsd_resource *res, size_t *pos, uint32_t *id)
{
    if (!pos || !id || !mpsd__resource_of(res, MINIPSD_RES_ALPHA_IDENTIFIERS)) return MINIPSD_ERR_ARG;
    if (res->data.size % MPSD__LEN) return MINIPSD_ERR_FORMAT;
    if (*pos % MPSD__LEN) return MINIPSD_ERR_ARG;
    if (*pos >= res->data.size) return MINIPSD_OK;
    *id   = mpsd__get32(res->data.data + *pos);
    *pos += MPSD__LEN;
    return MINIPSD_MORE;
}

MINIPSDDEF int minipsd_resource_transparency_index(const minipsd_resource *res, uint16_t *index)
{
    if (!index || !mpsd__resource_of(res, MINIPSD_RES_TRANSPARENCY_INDEX)) return MINIPSD_ERR_ARG;
    if (res->data.size < MPSD__TRANSPARENCY_INDEX_SIZE) return MINIPSD_ERR_FORMAT;
    *index = (uint16_t)mpsd__get16(res->data.data);
    return MINIPSD_OK;
}

/* --- a layer's fixed-layout blocks --- */

/* The blocks minipsd_layer_block_settings reads: key, the bytes its value needs, its present bit. */
static const struct { char key[5]; unsigned char size, bit; } mpsd__settings[] = {
    { "lyid", 4, MINIPSD_PRESENT_LAYER_ID },         { "lclr", 2, MINIPSD_PRESENT_SHEET_COLOR },
    { "iOpa", 1, MINIPSD_PRESENT_FILL_OPACITY },     { "lspf", 4, MINIPSD_PRESENT_PROTECTION_FLAGS },
    { "knko", 1, MINIPSD_PRESENT_KNOCKOUT },         { "clbl", 1, MINIPSD_PRESENT_BLEND_CLIPPED_ELEMENTS },
    { "infx", 1, MINIPSD_PRESENT_BLEND_INTERIOR_ELEMENTS }
};

MINIPSDDEF int minipsd_layer_block_settings(const minipsd_layer_view *layer, minipsd_block_settings *out)
{
    minipsd_block_settings s;
    minipsd_block          block;
    size_t                 pos = 0, k;
    int                    rc;
    if (!layer || !out) return MINIPSD_ERR_ARG;
    memset(&s, 0, sizeof s);
    s.fill_opacity = MPSD__FILL_OPAQUE;
    while ((rc = minipsd_layer_next_block(layer, &pos, &block)) == MINIPSD_MORE) {
        for (k = 0; k < sizeof mpsd__settings / sizeof mpsd__settings[0]; ++k) {
            const uint8_t *d = block.data.data;
            if (!mpsd__tag((const uint8_t *)block.key, mpsd__settings[k].key) || (s.present & mpsd__settings[k].bit)) continue;
            if (block.data.size < mpsd__settings[k].size) return MINIPSD_ERR_FORMAT;
            s.present |= mpsd__settings[k].bit;
            switch (mpsd__settings[k].bit) {
            case MINIPSD_PRESENT_LAYER_ID:                s.layer_id = mpsd__get32(d); break;
            case MINIPSD_PRESENT_SHEET_COLOR:             s.sheet_color = (uint16_t)mpsd__get16(d); break;
            case MINIPSD_PRESENT_FILL_OPACITY:            s.fill_opacity = d[0]; break;
            case MINIPSD_PRESENT_PROTECTION_FLAGS:        s.protection_flags = mpsd__get32(d); break;
            case MINIPSD_PRESENT_KNOCKOUT:                s.knockout = d[0]; break;
            case MINIPSD_PRESENT_BLEND_CLIPPED_ELEMENTS:  s.blend_clipped_elements = d[0]; break;
            default:                                      s.blend_interior_elements = d[0]; break;
            }
        }
    }
    if (rc != MINIPSD_OK) return rc;
    *out = s;
    return MINIPSD_OK;
}

#endif /* MPSD__VIEWS_DONE */
