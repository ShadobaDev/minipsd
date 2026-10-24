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
    if (res->data.size < MINIPSD_TRANSPARENCY_INDEX_SIZE) return MINIPSD_ERR_FORMAT;
    *index = (uint16_t)mpsd__get16(res->data.data);
    return MINIPSD_OK;
}

/* Adobe's "Image Resource IDs" table: each entry covers the ids [first, last], in ascending order. */
static const struct { uint16_t first, last; const char *name; } mpsd__resource_names[] = {
    { 1000, 1000, "Channels, rows, columns, depth and mode (obsolete)" },
    { 1001, 1001, "Macintosh print manager print info" },
    { 1002, 1002, "Macintosh page format information (obsolete)" },
    { 1003, 1003, "Indexed colour table (obsolete)" },
    { 1005, 1005, "Resolution info" },
    { 1006, 1006, "Alpha channel names" },
    { 1007, 1007, "Display info (obsolete)" },
    { 1008, 1008, "Caption" },
    { 1009, 1009, "Border information" },
    { 1010, 1010, "Background colour" },
    { 1011, 1011, "Print flags" },
    { 1012, 1012, "Greyscale and multichannel halftoning information" },
    { 1013, 1013, "Colour halftoning information" },
    { 1014, 1014, "Duotone halftoning information" },
    { 1015, 1015, "Greyscale and multichannel transfer function" },
    { 1016, 1016, "Colour transfer functions" },
    { 1017, 1017, "Duotone transfer functions" },
    { 1018, 1018, "Duotone image information" },
    { 1019, 1019, "Effective black and white values for the dot range" },
    { 1020, 1020, "Obsolete" },
    { 1021, 1021, "EPS options" },
    { 1022, 1022, "Quick mask information" },
    { 1023, 1023, "Obsolete" },
    { 1024, 1024, "Layer state information" },
    { 1025, 1025, "Working path" },
    { 1026, 1026, "Layers group information" },
    { 1027, 1027, "Obsolete" },
    { 1028, 1028, "IPTC-NAA record" },
    { 1029, 1029, "Image mode for raw format files" },
    { 1030, 1030, "JPEG quality" },
    { 1032, 1032, "Grid and guides information" },
    { 1033, 1033, "Thumbnail (Photoshop 4.0, obsolete)" },
    { 1034, 1034, "Copyright flag" },
    { 1035, 1035, "URL" },
    { 1036, 1036, "Thumbnail" },
    { 1037, 1037, "Global angle" },
    { 1038, 1038, "Colour samplers (obsolete)" },
    { 1039, 1039, "ICC profile" },
    { 1040, 1040, "Watermark" },
    { 1041, 1041, "ICC untagged profile" },
    { 1042, 1042, "Effects visible" },
    { 1043, 1043, "Spot halftone" },
    { 1044, 1044, "Document-specific IDs seed number" },
    { 1045, 1045, "Unicode alpha names" },
    { 1046, 1046, "Indexed colour table count" },
    { 1047, 1047, "Transparency index" },
    { 1049, 1049, "Global altitude" },
    { 1050, 1050, "Slices" },
    { 1051, 1051, "Workflow URL" },
    { 1052, 1052, "Jump to XPEP" },
    { 1053, 1053, "Alpha identifiers" },
    { 1054, 1054, "URL list" },
    { 1057, 1057, "Version info" },
    { 1058, 1058, "EXIF data 1" },
    { 1059, 1059, "EXIF data 3" },
    { 1060, 1060, "XMP metadata" },
    { 1061, 1061, "Caption digest" },
    { 1062, 1062, "Print scale" },
    { 1064, 1064, "Pixel aspect ratio" },
    { 1065, 1065, "Layer comps" },
    { 1066, 1066, "Alternate duotone colours" },
    { 1067, 1067, "Alternate spot colours" },
    { 1069, 1069, "Layer selection IDs" },
    { 1070, 1070, "HDR toning information" },
    { 1071, 1071, "Print info" },
    { 1072, 1072, "Layer groups enabled IDs" },
    { 1073, 1073, "Colour samplers" },
    { 1074, 1074, "Measurement scale" },
    { 1075, 1075, "Timeline information" },
    { 1076, 1076, "Sheet disclosure" },
    { 1077, 1077, "Display info" },
    { 1078, 1078, "Onion skins" },
    { 1080, 1080, "Count information" },
    { 1082, 1082, "Print information" },
    { 1083, 1083, "Print style" },
    { 1084, 1084, "Macintosh NSPrintInfo" },
    { 1085, 1085, "Windows DEVMODE" },
    { 1086, 1086, "Auto save file path" },
    { 1087, 1087, "Auto save format" },
    { 1088, 1088, "Path selection state" },
    { 2000, 2997, "Path information" },
    { 2999, 2999, "Name of clipping path" },
    { 3000, 3000, "Origin path info" },
    { 4000, 4999, "Plug-in resource" },
    { 7000, 7000, "Image Ready variables" },
    { 7001, 7001, "Image Ready data sets" },
    { 7002, 7002, "Image Ready default selected state" },
    { 7003, 7003, "Image Ready 7 rollover expanded state" },
    { 7004, 7004, "Image Ready rollover expanded state" },
    { 7005, 7005, "Image Ready save layer settings" },
    { 7006, 7006, "Image Ready version" },
    { 8000, 8000, "Lightroom workflow" },
    { 10000, 10000, "Print flags information" }
};

/* Entries ascend and never share an id, so entry k starts at the first one's id + k or later: the
   entry that holds id lies at index id - first or below, and is found walking down from there. */
MINIPSDDEF const char *minipsd_resource_id_name(uint16_t id)
{
    const size_t count = sizeof mpsd__resource_names / sizeof mpsd__resource_names[0];
    size_t       k;
    if (id < mpsd__resource_names[0].first) return NULL;
    k = (size_t)(id - mpsd__resource_names[0].first);
    if (k >= count) k = count - 1;
    while (mpsd__resource_names[k].first > id) --k;            /* stops at entry 0 at the latest */
    return id <= mpsd__resource_names[k].last ? mpsd__resource_names[k].name : NULL;
}

/* --- a layer's fixed-layout blocks --- */

MINIPSDDEF int minipsd_blocks_settings(const minipsd_bytes *blocks, int format, minipsd_block_settings *out)
{
    minipsd_block_settings s;
    minipsd_block          block;
    size_t                 pos = 0, next, k;
    if (!blocks || !out || (!blocks->data && blocks->size) || (format != 0 && format != MINIPSD_PSD && format != MINIPSD_PSB))
        return MINIPSD_ERR_ARG;
    memset(&s, 0, sizeof s);
    s.fill_opacity = MPSD__FILL_OPAQUE;
    for (; pos < blocks->size; pos = next) {
        if (mpsd__block(*blocks, pos, format == MINIPSD_PSB, &block, &next) != MINIPSD_OK) return MINIPSD_ERR_FORMAT;
        for (k = 0; k < MPSD__SETTINGS; ++k) {          /* the first block of each key counts */
            if (!mpsd__tag((const uint8_t *)block.key, mpsd__settings[k].key) || (s.present & mpsd__settings[k].bit)) continue;
            if (block.data.size < mpsd__settings[k].size) return MINIPSD_ERR_FORMAT;
            mpsd__setting_set(&s, k, block.data.data);
        }
    }
    *out = s;
    return MINIPSD_OK;
}

MINIPSDDEF int minipsd_layer_block_settings(const minipsd_layer_view *layer, minipsd_block_settings *out)
{
    if (!layer) return MINIPSD_ERR_ARG;
    return minipsd_blocks_settings(&layer->additional_info, layer->format, out);
}

#endif /* MPSD__VIEWS_DONE */
