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

#endif /* MPSD__BUILDERS_DONE */
