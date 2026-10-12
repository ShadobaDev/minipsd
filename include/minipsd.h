/**
 * \file
 * \brief A small, dependency-free Photoshop (PSD) writer.  Single header, C99, also valid C++.
 *
 * \par Usage
 * In exactly one C or C++ file:
 * \code
 *     #define MINIPSD_IMPLEMENTATION
 *     #include "minipsd.h"
 * \endcode
 * Everywhere else include it plainly.
 *
 * \code
 *     minipsd_layer layers[2] = {
 *         { "Template", rgba, 255, 0 },     // bottom
 *         { "Layer",    NULL, 255, 0 },     // empty, above it
 *     };
 *     minipsd_doc doc = { width, height, 300.0, layers, 2, NULL, 0, MINIPSD_RLE };
 *     int rc = minipsd_write_file(&doc, "template.psd");
 *     if (rc != MINIPSD_OK) puts(minipsd_error_string(rc));
 * \endcode
 *
 * \par Configuration
 * Define before the implementation include:
 * - \c MINIPSD_STATIC: make every function static (private to that file);
 * - \c MINIPSD_MALLOC / \c MINIPSD_FREE: replace malloc / free (define both);
 * - \c MINIPSD_NO_STDIO: leave out minipsd_write_file (and <stdio.h>) when bytes only go to a
 *   callback.
 *
 * \par What it writes
 * An 8-bit RGB document (PSD version 1) with a transparency channel:
 * - one or more layers, bottom first, each the full canvas or empty, with a name, opacity and
 *   visibility; names go out both as a legacy Pascal string (ASCII) and as Unicode ('luni');
 * - the composite (merged) image, the layers blended with straight-alpha "over", which is what a
 *   reader that ignores layers shows; its transparency rides as the first extra channel, flagged by
 *   a negative layer count, and its colour is matted against white (c * a + 255 * (1 - a)), as
 *   Photoshop stores it and readers expect to undo;
 * - the resolution, in pixels per inch (resource 1005);
 * - an optional ICC profile (resource 1039); without one, readers assume sRGB.
 *
 * Channel data is PackBits RLE by default (small for flat art), or raw.
 *
 * Not written: 16/32-bit, other colour modes, masks, groups, blend modes other than normal, PSB.
 * PSD caps each side at 30000 px; larger documents are refused (MINIPSD_ERR_SIZE).
 *
 * \par Output
 * Bytes go to a callback in order, never seeking back, so a pipe or a socket works as well as a
 * file.  Allocations are small: each layer channel's compressed size (layer_count * 4 numbers),
 * one row of the composite (width * 4 bytes), and on Windows the UTF-16 copy of
 * minipsd_write_file's path.  Layer pixels are read in place, never copied.
 *
 * \par Project
 * https://github.com/ShadobaDev/minipsd
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MINIPSD_H
#define MINIPSD_H

#include <stddef.h>
#include <stdint.h>

/**
 * \brief The version of this header, MAJOR.MINOR.PATCH.
 *
 * The single source of the project version: CMakeLists.txt reads these three lines.
 */
#define MINIPSD_VERSION_MAJOR 0
#define MINIPSD_VERSION_MINOR 1
#define MINIPSD_VERSION_PATCH 0

#define MPSD__STR(x)  #x
#define MPSD__XSTR(x) MPSD__STR(x)
/** \brief The version as a string literal, e.g. "0.1.0". */
#define MINIPSD_VERSION_STRING \
    MPSD__XSTR(MINIPSD_VERSION_MAJOR) "." MPSD__XSTR(MINIPSD_VERSION_MINOR) "." MPSD__XSTR(MINIPSD_VERSION_PATCH)

/** \brief Linkage of every public function: static under MINIPSD_STATIC, otherwise extern (C). */
#ifdef MINIPSD_STATIC
#  define MINIPSDDEF static
#elif defined(__cplusplus)
#  define MINIPSDDEF extern "C"
#else
#  define MINIPSDDEF extern
#endif

/** \brief The largest width or height PSD allows, in pixels. */
#define MINIPSD_MAX_SIDE 30000

/** \brief Return codes: MINIPSD_OK, or one of the negative errors. */
enum {
    MINIPSD_OK          =  0, /*!< Success.                                                   */
    MINIPSD_ERR_ARG     = -1, /*!< A NULL document, no layers, or a dpi outside (0, 65536).    */
    MINIPSD_ERR_SIZE    = -2, /*!< A side is 0 or above MINIPSD_MAX_SIDE, or the document is too
                                   large for PSD's 32-bit section lengths (about 4 GiB).      */
    MINIPSD_ERR_WRITE   = -3, /*!< The write callback reported a failure.                      */
    MINIPSD_ERR_ALLOC   = -4, /*!< MINIPSD_MALLOC returned NULL.                               */
    MINIPSD_ERR_OPEN    = -5  /*!< minipsd_write_file could not open (or close) the file.      */
};

/** \brief Channel compression.  Zero is RLE, so a zero-initialised document gets the compact one. */
enum {
    MINIPSD_RLE = 0, /*!< PackBits run-length encoding. */
    MINIPSD_RAW = 1  /*!< Uncompressed.                 */
};

/**
 * \brief One layer of a document.
 *
 * Every pointer is borrowed: it must stay valid for the duration of the write call, and minipsd
 * never frees or copies what it points to.
 */
typedef struct minipsd_layer {
    const char    *name;    /*!< UTF-8; NULL means "".                                            */
    const uint8_t *rgba;    /*!< width * height * 4 bytes, rows top down, straight (not premultiplied)
                                 alpha; NULL means an empty layer.                                */
    uint8_t        opacity; /*!< 255 is opaque, 0 is fully transparent.                            */
    uint8_t        hidden;  /*!< Non-zero: the layer starts hidden (and stays out of the composite). */
} minipsd_layer;

/**
 * \brief A whole document: canvas, resolution, layers and options.
 *
 * Zero-initialise it and set the fields you need; zero compression selects RLE.  Pointers are
 * borrowed for the duration of the write call.
 */
typedef struct minipsd_doc {
    uint32_t             width, height;  /*!< 1 .. MINIPSD_MAX_SIDE.                        */
    double               dpi;            /*!< Above 0, below 65536; recorded, not resampled. */
    const minipsd_layer *layers;         /*!< Bottom first.                                  */
    size_t               layer_count;    /*!< >= 1.                                          */
    const uint8_t       *icc;            /*!< Optional ICC profile; NULL means none (sRGB).  */
    size_t               icc_size;       /*!< Size of icc in bytes; 0 when icc is NULL.      */
    int                  compression;    /*!< MINIPSD_RLE or MINIPSD_RAW.                    */
} minipsd_doc;

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
/**
 * \brief A short English description of a return code.
 *
 * \param code A value returned by minipsd_write or minipsd_write_file.
 * \return A static string; "unknown error" for codes minipsd does not return.
 */
MINIPSDDEF const char *minipsd_error_string(int code);

#endif /* MINIPSD_H */

/* ============================================================================================== */

#ifdef MINIPSD_IMPLEMENTATION
#ifndef MINIPSD_IMPLEMENTATION_DONE
#define MINIPSD_IMPLEMENTATION_DONE

#include <string.h>
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

/* --- output: a sink either counts the bytes (the sizing pass) or sends them on --- */

typedef struct mpsd__sink {
    minipsd_write_fn fn;    /* NULL while counting */
    void            *user;
    uint64_t         count;
    int              failed;
} mpsd__sink;

static void mpsd__put(mpsd__sink *s, const void *data, size_t size)
{
    s->count += size;
    if (s->fn && !s->failed && size && !s->fn(s->user, data, size))
        s->failed = 1;
}

static void mpsd__u8(mpsd__sink *s, unsigned v)  { uint8_t b = (uint8_t)v; mpsd__put(s, &b, 1); }
static void mpsd__u16(mpsd__sink *s, unsigned v) { uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v }; mpsd__put(s, b, 2); }
static void mpsd__u32(mpsd__sink *s, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    mpsd__put(s, b, 4);
}
static void mpsd__zeros(mpsd__sink *s, size_t n) { while (n--) mpsd__u8(s, 0); }

/* --- PackBits over a strided source (one channel out of interleaved RGBA) --- */

static void mpsd__packbits(mpsd__sink *s, const uint8_t *src, size_t n, size_t stride)
{
    uint8_t packet[129];
    size_t  i = 0;
    while (i < n) {
        const uint8_t v = src[i * stride];
        size_t run = 1;
        while (i + run < n && run < 128 && src[(i + run) * stride] == v) ++run;
        if (run >= 3) {                       /* a repeat: 1 - run, then the byte */
            packet[0] = (uint8_t)(257 - run);
            packet[1] = v;
            mpsd__put(s, packet, 2);
            i += run;
        } else {                              /* literals, up to the next run of three */
            size_t len = 0;
            while (i < n && len < 128) {
                if (i + 2 < n && src[i * stride] == src[(i + 1) * stride]
                              && src[i * stride] == src[(i + 2) * stride])
                    break;
                packet[1 + len++] = src[i++ * stride];
            }
            packet[0] = (uint8_t)(len - 1);
            mpsd__put(s, packet, len + 1);
        }
    }
}

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

/* --- the composite: layers blended "over", then matted against white, one row at a time --- */

typedef struct mpsd__composite {
    const minipsd_doc *doc;
    uint8_t           *row;      /* width * 4 */
} mpsd__composite;

static int mpsd__contributes(const minipsd_layer *l) { return l->rgba && !l->hidden && l->opacity; }

static int mpsd__composite_init(mpsd__composite *c, const minipsd_doc *d)
{
    c->doc = d;
    c->row = (uint8_t *)MINIPSD_MALLOC((size_t)d->width * 4);
    return c->row ? MINIPSD_OK : MINIPSD_ERR_ALLOC;
}

static const uint8_t *mpsd__composite_row(mpsd__composite *c, uint32_t y)
{
    const minipsd_doc *d = c->doc;
    size_t i;
    uint32_t x;
    memset(c->row, 0, (size_t)d->width * 4);
    for (i = 0; i < d->layer_count; ++i) {
        const minipsd_layer *l = &d->layers[i];
        const uint8_t *src;
        if (!mpsd__contributes(l)) continue;
        src = l->rgba + (size_t)y * d->width * 4;
        for (x = 0; x < d->width; ++x) {
            uint8_t    *o  = c->row + (size_t)x * 4;
            const float sa = src[x * 4 + 3] / 255.0f * (l->opacity / 255.0f);
            const float da = o[3] / 255.0f;
            const float oa = sa + da * (1.0f - sa);
            int k;
            if (oa <= 0.0f) continue;
            for (k = 0; k < 3; ++k)
                o[k] = (uint8_t)((src[x * 4 + k] * sa + o[k] * da * (1.0f - sa)) / oa + 0.5f);
            o[3] = (uint8_t)(oa * 255.0f + 0.5f);
        }
    }
    for (x = 0; x < d->width; ++x) {          /* the merged image is stored over white */
        uint8_t *o = c->row + (size_t)x * 4;
        const unsigned a = o[3];
        int k;
        for (k = 0; k < 3; ++k)
            o[k] = (uint8_t)((o[k] * a + 255u * (255u - a) + 127u) / 255u);
    }
    return c->row;
}

static void mpsd__composite_free(mpsd__composite *c) { MINIPSD_FREE(c->row); }   /* only after a successful init */

/* --- names: a legacy ASCII Pascal string and the UTF-16 one --- */

/* Decodes one UTF-8 code point; malformed input yields U+FFFD and moves on by one byte. */
static uint32_t mpsd__utf8(const unsigned char **p)
{
    const unsigned char *s = *p;
    uint32_t cp;
    int extra, k;
    if      (s[0] < 0x80)           { *p = s + 1; return s[0]; }
    else if ((s[0] & 0xE0) == 0xC0) { cp = s[0] & 0x1F; extra = 1; }
    else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0F; extra = 2; }
    else if ((s[0] & 0xF8) == 0xF0) { cp = s[0] & 0x07; extra = 3; }
    else                            { *p = s + 1; return 0xFFFD; }
    for (k = 1; k <= extra; ++k) {
        if ((s[k] & 0xC0) != 0x80) { *p = s + 1; return 0xFFFD; }
        cp = (cp << 6) | (s[k] & 0x3F);
    }
    *p = s + extra + 1;
    return cp > 0x10FFFF ? 0xFFFD : cp;
}

/* Calls back with each UTF-16 unit of a UTF-8 string; returns how many there were. */
static size_t mpsd__utf16(const char *utf8, void (*unit)(void *, unsigned), void *ctx)
{
    const unsigned char *p = (const unsigned char *)(utf8 ? utf8 : "");
    size_t n = 0;
    while (*p) {
        const uint32_t cp = mpsd__utf8(&p);
        if (cp >= 0x10000) {
            if (unit) { unit(ctx, 0xD800 + ((cp - 0x10000) >> 10)); unit(ctx, 0xDC00 + ((cp - 0x10000) & 0x3FF)); }
            n += 2;
        } else {
            if (unit) unit(ctx, cp);
            ++n;
        }
    }
    return n;
}

static void mpsd__unit_out(void *ctx, unsigned u) { mpsd__u16((mpsd__sink *)ctx, u); }

static size_t mpsd__pad(size_t n, size_t to) { return (n + to - 1) / to * to; }

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

MINIPSDDEF const char *minipsd_error_string(int code)
{
    switch (code) {
    case MINIPSD_OK:        return "success";
    case MINIPSD_ERR_ARG:   return "invalid argument";
    case MINIPSD_ERR_SIZE:  return "too large for PSD: a side outside 1..30000, or a section above 4 GiB";
    case MINIPSD_ERR_WRITE: return "write failed";
    case MINIPSD_ERR_ALLOC: return "out of memory";
    case MINIPSD_ERR_OPEN:  return "cannot open or close the file";
    default:                return "unknown error";
    }
}

#endif /* MINIPSD_IMPLEMENTATION_DONE */
#endif /* MINIPSD_IMPLEMENTATION */

/*
--------------------------------------------------------------------------------------------------
MIT License

Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
--------------------------------------------------------------------------------------------------
*/
