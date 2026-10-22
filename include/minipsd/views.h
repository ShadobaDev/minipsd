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

#endif /* MPSD__VIEWS_DONE */
