/**
 * \file
 * \brief The writer: a document to PSD bytes, through a callback or into a file.
 *
 * Part of minipsd; include minipsd.h for the whole library and its documentation.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#include "base.h"
#include "document.h"

#ifndef MINIPSD_WRITER_H
#define MINIPSD_WRITER_H

/**
 * \brief Receives the file's bytes, in order.
 *
 * \param user The pointer given to minipsd_write.
 * \param data The next bytes of the file.
 * \param size How many bytes data holds; never 0.
 * \return Non-zero on success, 0 to abort the write.
 */
typedef int (*minipsd_write_fn)(void *user, const void *data, size_t size);

/**
 * \brief Writes a document as PSD through a callback.
 *
 * The callback receives the whole file front to back and is never asked to seek.  After it
 * returns 0 it is not called again.  The same document always produces the same bytes.
 *
 * \param doc  The document to write.
 * \param fn   Receives the bytes.
 * \param user Passed to fn unchanged.
 * \return MINIPSD_OK or an error code.
 *
 * \retval MINIPSD_OK        The whole file went to fn.
 * \retval MINIPSD_ERR_ARG   doc or fn is NULL, there are no layers, dpi is outside (0, 65536), or
 *                           icc_size is set without icc.  Nothing was written.
 * \retval MINIPSD_ERR_SIZE  A side is 0 or above MINIPSD_MAX_SIDE, or the layers or the ICC
 *                           profile do not fit a 32-bit section length.  Nothing was written.
 * \retval MINIPSD_ERR_ALLOC An allocation failed.  Nothing was written.
 * \retval MINIPSD_ERR_WRITE fn returned 0; the output is incomplete.
 */
MINIPSDDEF int minipsd_write(const minipsd_doc *doc, minipsd_write_fn fn, void *user);
#ifndef MINIPSD_NO_STDIO
/**
 * \brief Writes a document as PSD to a file, replacing it if it exists.
 *
 * \param doc  The document to write.
 * \param path The file path, UTF-8 on every platform (Windows included).
 * \return MINIPSD_OK or an error code; see minipsd_write.
 *
 * \retval MINIPSD_ERR_ARG  As for minipsd_write, or path is NULL.
 * \retval MINIPSD_ERR_OPEN The file could not be opened or closed.
 *
 * \note A failure after the file was opened leaves the partial file in place.
 */
MINIPSDDEF int minipsd_write_file(const minipsd_doc *doc, const char *path);
#endif

#endif /* MINIPSD_WRITER_H */

/* ============================================================================================== */

#if defined(MINIPSD_IMPLEMENTATION) && !defined(MPSD__WRITER_DONE)
#define MPSD__WRITER_DONE

#ifndef MINIPSD_NO_STDIO
#  include <stdio.h>
#  ifdef _WIN32
#    include <wchar.h>
#  endif
#endif

#if !defined(MINIPSD_MALLOC) || !defined(MINIPSD_FREE)
#  include <stdlib.h>
#  undef  MINIPSD_MALLOC
#  undef  MINIPSD_FREE
#  define MINIPSD_MALLOC(size) malloc(size)
#  define MINIPSD_FREE(ptr)    free(ptr)
#endif

#include "detail/bytes.h"
#include "detail/utf.h"
#include "detail/packbits.h"
#include "detail/composite.h"

/* --- channel rows: PackBits or raw --- */

static void mpsd__raw(mpsd__sink *s, const uint8_t *src, size_t n, size_t stride)
{
    uint8_t chunk[256];
    size_t  i = 0;
    while (i < n) {
        size_t len = 0;
        while (i < n && len < sizeof chunk) chunk[len++] = src[i++ * stride];
        mpsd__put(s, chunk, len);
    }
}

/* One row of one channel. */
static void mpsd__row(mpsd__sink *s, const minipsd_doc *d, const uint8_t *row, int channel)
{
    if (d->compression == MINIPSD_RAW) mpsd__raw(s, row + channel, d->width, 4);
    else                               mpsd__packbits(s, row + channel, d->width, 4);
}

static uint64_t mpsd__row_size(const minipsd_doc *d, const uint8_t *row, int channel)
{
    mpsd__sink c = { NULL, NULL, 0, 0 };
    mpsd__row(&c, d, row, channel);
    return c.count;
}

/* PSD channel order is R, G, B, then transparency (id -1); RGBA offsets 0, 1, 2, 3. */
static const int mpsd__channel_id[4] = { -1, 0, 1, 2 };
static const int mpsd__channel_of[4] = {  3, 0, 1, 2 };   /* RGBA offset for each of those ids */

/* --- names: a legacy ASCII Pascal string and the UTF-16 one --- */

static void mpsd__unit_out(void *ctx, unsigned u) { mpsd__u16((mpsd__sink *)ctx, u); }

/* The extra-data block of a layer record: no mask, no blending ranges, the names. */
static void mpsd__layer_extra(mpsd__sink *s, const char *name)
{
    const unsigned char *p = (const unsigned char *)(name ? name : "");
    size_t ascii = 0, units, luni;
    uint8_t pascal[256];
    while (*p && ascii < 255) {               /* one '?' per non-ASCII character */
        const uint32_t cp = mpsd__utf8(&p);
        pascal[1 + ascii++] = (uint8_t)(cp < 0x80 ? cp : '?');
    }
    pascal[0] = (uint8_t)ascii;
    mpsd__u32(s, 0);                          /* layer mask data      */
    mpsd__u32(s, 0);                          /* blending ranges      */
    mpsd__put(s, pascal, ascii + 1);
    mpsd__zeros(s, mpsd__pad(ascii + 1, 4) - (ascii + 1));

    units = mpsd__utf16(name, NULL, NULL);
    luni  = 4 + units * 2;
    mpsd__put(s, "8BIMluni", 8);
    mpsd__u32(s, (uint32_t)mpsd__pad(luni, 4));
    mpsd__u32(s, (uint32_t)units);
    mpsd__utf16(name, mpsd__unit_out, s);
    mpsd__zeros(s, mpsd__pad(luni, 4) - luni);
}

/* --- the layer section --- */

/* A layer channel's data: its compression, then (RLE) the row byte counts, then the rows. */
static void mpsd__layer_channel(mpsd__sink *s, const minipsd_doc *d, const minipsd_layer *l, int channel)
{
    uint32_t y;
    const int rle = d->compression != MINIPSD_RAW;
    mpsd__u16(s, rle ? 1 : 0);
    if (!l->rgba) return;                     /* an empty layer has no rows */
    if (rle)
        for (y = 0; y < d->height; ++y)
            mpsd__u16(s, (unsigned)mpsd__row_size(d, l->rgba + (size_t)y * d->width * 4, channel));
    for (y = 0; y < d->height; ++y)
        mpsd__row(s, d, l->rgba + (size_t)y * d->width * 4, channel);
}

/* What mpsd__layer_channel will write, without compressing anything twice. */
static uint64_t mpsd__layer_channel_size(const minipsd_doc *d, const minipsd_layer *l, int channel)
{
    uint64_t size = 2;
    uint32_t y;
    if (!l->rgba) return size;
    if (d->compression == MINIPSD_RAW) return size + (uint64_t)d->width * d->height;
    for (y = 0; y < d->height; ++y)
        size += 2 + mpsd__row_size(d, l->rgba + (size_t)y * d->width * 4, channel);
    return size;
}

static uint64_t mpsd__layer_extra_size(const char *name)
{
    mpsd__sink c = { NULL, NULL, 0, 0 };
    mpsd__layer_extra(&c, name);
    return c.count;
}

/* The layer info: count, records, then every layer's channel data.  sizes[i * 4 + k] is layer i's
   channel k (in PSD order), from mpsd__layer_channel_size. */
static void mpsd__layer_info(mpsd__sink *s, const minipsd_doc *d, const uint64_t *sizes)
{
    size_t i;
    int k;
    mpsd__u16(s, (unsigned)(uint16_t)-(int16_t)d->layer_count);   /* negative: composite has alpha */
    for (i = 0; i < d->layer_count; ++i) {
        const minipsd_layer *l = &d->layers[i];
        mpsd__u32(s, 0); mpsd__u32(s, 0);                         /* top, left       */
        mpsd__u32(s, l->rgba ? d->height : 0);                    /* bottom          */
        mpsd__u32(s, l->rgba ? d->width  : 0);                    /* right           */
        mpsd__u16(s, 4);
        for (k = 0; k < 4; ++k) {
            mpsd__u16(s, (unsigned)(uint16_t)(int16_t)mpsd__channel_id[k]);
            mpsd__u32(s, (uint32_t)sizes[i * 4 + k]);
        }
        mpsd__put(s, "8BIMnorm", 8);
        mpsd__u8(s, l->opacity);
        mpsd__u8(s, 0);                                           /* clipping: base  */
        mpsd__u8(s, l->hidden ? 0x02 : 0x00);                     /* flags           */
        mpsd__u8(s, 0);                                           /* filler          */
        mpsd__u32(s, (uint32_t)mpsd__layer_extra_size(l->name));
        mpsd__layer_extra(s, l->name);
    }
    for (i = 0; i < d->layer_count; ++i)
        for (k = 0; k < 4; ++k)
            mpsd__layer_channel(s, d, &d->layers[i], mpsd__channel_of[k]);
}

/* --- the file --- */

/* PSD (version 1) stores section lengths in 32 bits. */
#define MPSD__MAX_SECTION 0xFFFFFFFFu

static int mpsd__check(const minipsd_doc *d)
{
    double res;
    if (!d) return MINIPSD_ERR_ARG;
    /* The dpi is stored as 16.16 fixed point: it must round to 1 .. 2^32 - 1.  NaN fails both. */
    res = d->dpi * 65536.0 + 0.5;
    if (!d->layers || d->layer_count < 1 || d->layer_count > 32767 || !(res >= 1.0 && res < 4294967296.0)
        || (d->icc_size && !d->icc))
        return MINIPSD_ERR_ARG;
    if (d->width < 1 || d->height < 1 || d->width > MINIPSD_MAX_SIDE || d->height > MINIPSD_MAX_SIDE)
        return MINIPSD_ERR_SIZE;
    if ((uint64_t)d->icc_size + 28 + 12 + 1 > MPSD__MAX_SECTION)   /* resources: resolution + ICC, padded */
        return MINIPSD_ERR_SIZE;
    return MINIPSD_OK;
}

MINIPSDDEF int minipsd_write(const minipsd_doc *d, minipsd_write_fn fn, void *user)
{
    mpsd__sink      s = { fn, user, 0, 0 };
    mpsd__composite comp;
    uint64_t       *sizes;
    uint64_t        info, content;
    uint32_t        y, res;
    size_t          i;
    int             k, rc = d ? mpsd__check(d) : MINIPSD_ERR_ARG;   /* d spelled out for static analysers */
    if (rc != MINIPSD_OK) return rc;
    if (!fn) return MINIPSD_ERR_ARG;

    /* Every layer channel's size goes in the records ahead of the data: compress once to learn them. */
    sizes = (uint64_t *)MINIPSD_MALLOC(d->layer_count * 4 * sizeof(uint64_t));
    if (!sizes) return MINIPSD_ERR_ALLOC;
    content = 2;                                                   /* the layer count */
    for (i = 0; i < d->layer_count; ++i) {
        content += 16 + 2 + 4 * 6 + 8 + 4 + 4 + mpsd__layer_extra_size(d->layers[i].name);
        for (k = 0; k < 4; ++k)
            content += sizes[i * 4 + k] = mpsd__layer_channel_size(d, &d->layers[i], mpsd__channel_of[k]);
    }
    info = (content + 1) / 2 * 2;             /* the layer info is padded to an even length */
    if (4 + info + 4 > MPSD__MAX_SECTION) { MINIPSD_FREE(sizes); return MINIPSD_ERR_SIZE; }
    rc = mpsd__composite_init(&comp, d);
    if (rc != MINIPSD_OK) { MINIPSD_FREE(sizes); return rc; }

    /* Header. */
    mpsd__put(&s, "8BPS", 4);
    mpsd__u16(&s, 1);                         /* version: PSD        */
    mpsd__zeros(&s, 6);
    mpsd__u16(&s, 4);                         /* R, G, B, alpha      */
    mpsd__u32(&s, d->height);
    mpsd__u32(&s, d->width);
    mpsd__u16(&s, 8);                         /* bits per channel    */
    mpsd__u16(&s, 3);                         /* colour mode: RGB    */

    mpsd__u32(&s, 0);                         /* colour mode data    */

    /* Image resources: resolution, then the ICC profile if any. */
    mpsd__u32(&s, (uint32_t)(28 + (d->icc ? 12 + mpsd__pad(d->icc_size, 2) : 0)));
    res = (uint32_t)(d->dpi * 65536.0 + 0.5);  /* 16.16 fixed point */
    mpsd__put(&s, "8BIM", 4); mpsd__u16(&s, 1005); mpsd__u16(&s, 0); mpsd__u32(&s, 16);
    mpsd__u32(&s, res); mpsd__u16(&s, 1); mpsd__u16(&s, 1);   /* pixels per inch, shown in inches */
    mpsd__u32(&s, res); mpsd__u16(&s, 1); mpsd__u16(&s, 1);
    if (d->icc) {
        mpsd__put(&s, "8BIM", 4); mpsd__u16(&s, 1039); mpsd__u16(&s, 0);
        mpsd__u32(&s, (uint32_t)d->icc_size);
        mpsd__put(&s, d->icc, d->icc_size);
        mpsd__zeros(&s, d->icc_size & 1);
    }

    /* Layer and mask information. */
    mpsd__u32(&s, (uint32_t)(4 + info + 4));
    mpsd__u32(&s, (uint32_t)info);
    mpsd__layer_info(&s, d, sizes);
    mpsd__zeros(&s, (size_t)(info - content));
    mpsd__u32(&s, 0);                         /* global layer mask   */

    /* The composite: R, G, B, then its transparency (RGBA offsets 0..3), each channel's rows in turn. */
    mpsd__u16(&s, d->compression == MINIPSD_RAW ? 0 : 1);
    if (d->compression != MINIPSD_RAW)
        for (k = 0; k < 4; ++k)
            for (y = 0; y < d->height; ++y)
                mpsd__u16(&s, (unsigned)mpsd__row_size(d, mpsd__composite_row(&comp, y), k));
    for (k = 0; k < 4; ++k)
        for (y = 0; y < d->height && !s.failed; ++y)
            mpsd__row(&s, d, mpsd__composite_row(&comp, y), k);

    mpsd__composite_free(&comp);
    MINIPSD_FREE(sizes);
    return s.failed ? MINIPSD_ERR_WRITE : MINIPSD_OK;
}

/* --- to a file --- */

#ifndef MINIPSD_NO_STDIO
static int mpsd__file_out(void *user, const void *data, size_t size)
{
    return fwrite(data, 1, size, (FILE *)user) == size;
}

#ifdef _WIN32
static void mpsd__unit_wide(void *ctx, unsigned u) { *(*(wchar_t **)ctx)++ = (wchar_t)u; }
#endif

static FILE *mpsd__open(const char *path)
{
#ifdef _WIN32
    /* The narrow fopen reads the ANSI code page; convert UTF-8 so any path opens. */
    FILE    *f = NULL;
    wchar_t *wide = (wchar_t *)MINIPSD_MALLOC((mpsd__utf16(path, NULL, NULL) + 1) * sizeof(wchar_t));
    wchar_t *at = wide;
    if (!wide) return NULL;
    mpsd__utf16(path, mpsd__unit_wide, &at);
    *at = 0;
#  ifdef _MSC_VER
    if (_wfopen_s(&f, wide, L"wb") != 0) f = NULL;
#  else
    f = _wfopen(wide, L"wb");
#  endif
    MINIPSD_FREE(wide);
    return f;
#else
    return fopen(path, "wb");
#endif
}

MINIPSDDEF int minipsd_write_file(const minipsd_doc *doc, const char *path)
{
    FILE *f;
    int   rc = mpsd__check(doc);
    if (rc != MINIPSD_OK) return rc;
    if (!path) return MINIPSD_ERR_ARG;
    f = mpsd__open(path);
    if (!f) return MINIPSD_ERR_OPEN;
    rc = minipsd_write(doc, mpsd__file_out, f);
    if (fclose(f) != 0 && rc == MINIPSD_OK) rc = MINIPSD_ERR_OPEN;
    return rc;
}
#endif /* MINIPSD_NO_STDIO */

#endif /* MINIPSD_IMPLEMENTATION */
