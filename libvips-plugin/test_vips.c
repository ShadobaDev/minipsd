/**
 * \file
 * \brief The libvips plugin's test: documents minipsd writes, loaded through libvips.
 *
 * Usage: minipsd-vips-test out-dir.  Each document is written into out-dir, then loaded as a file,
 * from a buffer and from a source; the image libvips gives must say what the document does.
 * Exits with 1 when anything fails.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include "minipsd_vips.h"

#include <vips/vips.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH         4
#define HEIGHT        3
#define PIXELS        (WIDTH * HEIGHT)
#define MAX_PLANES    5      /* CMYK and transparency */
#define DPI           300.0
#define ASPECT        0.9
#define MM_PER_INCH   25.4
#define EPSILON       1e-9
#define PATH_MAX_SIZE 1024

static int failures;

static void check(int ok, const char *what, const char *name)
{
    if (!ok) {
        printf("FAIL %s: %s\n", name, what);
        ++failures;
    }
}

/* A growing buffer the writer appends to. */
typedef struct out_buffer {
    uint8_t *data;
    size_t   size, cap;
} out_buffer;

static int to_buffer(void *user, const void *data, size_t size)
{
    out_buffer *b = (out_buffer *)user;
    if (b->size + size > b->cap) {
        size_t   cap  = (b->size + size) * 2;
        uint8_t *grow = (uint8_t *)realloc(b->data, cap);
        if (!grow) return 0;
        b->data = grow;
        b->cap  = cap;
    }
    memcpy(b->data + b->size, data, size);
    b->size += size;
    return 1;
}

typedef struct expected {
    const char        *name;
    int                bands;
    VipsBandFormat     format;
    VipsInterpretation interpretation;
} expected;

/* What libvips says of one loaded image, against the document's. */
static void check_image(VipsImage *image, const minipsd_doc *doc, const expected *e, const char *how)
{
    char         name[PATH_MAX_SIZE];
    const void  *icc;
    size_t       icc_size;
    double       ratio = 0.0;
    snprintf(name, sizeof name, "%s (%s)", e->name, how);
    if (!image) {
        printf("FAIL %s: not loaded: %s\n", name, vips_error_buffer());
        vips_error_clear();
        ++failures;
        return;
    }
    check(vips_image_get_width(image) == (int)doc->width && vips_image_get_height(image) == (int)doc->height,
          "size", name);
    check(vips_image_get_bands(image) == e->bands, "bands", name);
    check(vips_image_get_format(image) == e->format, "format", name);
    check(vips_image_get_interpretation(image) == e->interpretation, "interpretation", name);
    check(fabs(vips_image_get_xres(image) - doc->dpi / MM_PER_INCH) < EPSILON
              && fabs(vips_image_get_yres(image) - doc->dpi / MM_PER_INCH) < EPSILON,
          "resolution", name);
    if (doc->icc)
        check(vips_image_get_blob(image, VIPS_META_ICC_NAME, &icc, &icc_size) == 0 && icc_size == doc->icc_size
                  && memcmp(icc, doc->icc, icc_size) == 0,
              "ICC profile", name);
    check(vips_image_get_double(image, "psd-pixel-aspect-ratio", &ratio) == 0
              && fabs(ratio - (doc->pixel_aspect_ratio > 0.0 ? doc->pixel_aspect_ratio : 1.0)) < EPSILON,
          "pixel aspect ratio", name);
    g_object_unref(image);
}

/* Writes doc into dir, then loads it the three ways. */
static void check_document(const minipsd_doc *doc, const expected *e, const char *dir)
{
    out_buffer  b = { NULL, 0, 0 };
    char        path[PATH_MAX_SIZE];
    FILE       *f;
    const char *loader;
    VipsSource *source;
    int         rc = minipsd_write(doc, NULL, to_buffer, &b);
    if (rc != MINIPSD_OK) {
        printf("FAIL %s: not written: %s\n", e->name, minipsd_error_string(rc));
        ++failures;
        free(b.data);
        return;
    }
    snprintf(path, sizeof path, "%s/%s.%s", dir, e->name, doc->format == MINIPSD_PSB ? "psb" : "psd");
    f = fopen(path, "wb");
    check(f && fwrite(b.data, 1, b.size, f) == b.size, "file written", e->name);
    if (f) fclose(f);

    loader = vips_foreign_find_load(path);
    check(loader && strcmp(loader, "VipsForeignLoadPsdFile") == 0, "found as psdload", e->name);
    vips_error_clear();
    check_image(vips_image_new_from_file(path, NULL), doc, e, "file");
    check_image(vips_image_new_from_buffer(b.data, b.size, "", NULL), doc, e, "buffer");
    source = vips_source_new_from_memory(b.data, b.size);
    check_image(source ? vips_image_new_from_source(source, "", NULL) : NULL, doc, e, "source");
    if (source) g_object_unref(source);
    free(b.data);
}

int main(int argc, char **argv)
{
    static const uint8_t icc[] = "not a real ICC profile";
    static uint8_t       rgba[PIXELS * 4], cmyka[PIXELS * MAX_PLANES], bits[PIXELS];
    static uint16_t      grey16[PIXELS * 2];
    static const char    junk[] = "8BPX, not a PSD";
    minipsd_layer        layer;
    minipsd_doc          doc;
    const char          *loader;
    size_t               i;

    if (argc != 2) {
        fprintf(stderr, "usage: %s out-dir\n", argv[0]);
        return 2;
    }
    if (VIPS_INIT(argv[0])) vips_error_exit(NULL);
    minipsd_vips_init();
    minipsd_vips_init();   /* twice is harmless */

    for (i = 0; i < sizeof rgba; ++i) rgba[i] = (uint8_t)(i * 7);
    for (i = 0; i < sizeof cmyka; ++i) cmyka[i] = (uint8_t)(i * 5);
    for (i = 0; i < PIXELS * 2; ++i) grey16[i] = (uint16_t)(i * 4099);
    for (i = 0; i < PIXELS; ++i) bits[i] = (uint8_t)(i % 2);

    {   /* 8-bit RGB with a layer, a profile and non-square pixels; as PSD and as PSB */
        static const expected psd = { "rgb8", 4, VIPS_FORMAT_UCHAR, VIPS_INTERPRETATION_sRGB };
        static const expected psb = { "rgb8-psb", 4, VIPS_FORMAT_UCHAR, VIPS_INTERPRETATION_sRGB };
        memset(&layer, 0, sizeof layer);
        layer.name    = "Layer";
        layer.pixels  = rgba;
        layer.opacity = 255;
        memset(&doc, 0, sizeof doc);
        doc.width = WIDTH, doc.height = HEIGHT, doc.dpi = DPI;
        doc.layers = &layer, doc.layer_count = 1;
        doc.icc = icc, doc.icc_size = sizeof icc;
        doc.pixel_aspect_ratio = ASPECT;
        check_document(&doc, &psd, argv[1]);
        doc.format = MINIPSD_PSB;
        check_document(&doc, &psb, argv[1]);
    }
    /* Grey and CMYK: a layer and its composite (the writer composes RGB only); with a layer, the
       negative layer count says the composite's transparency is there. */
    {   /* 16-bit grey */
        static const expected e = { "grey16", 2, VIPS_FORMAT_USHORT, VIPS_INTERPRETATION_GREY16 };
        layer.pixels = (const uint8_t *)grey16;
        memset(&doc, 0, sizeof doc);
        doc.width = WIDTH, doc.height = HEIGHT, doc.dpi = DPI;
        doc.layers = &layer, doc.layer_count = 1;
        doc.color_mode = MINIPSD_MODE_GREY, doc.depth = 16;
        doc.composite = (const uint8_t *)grey16;
        check_document(&doc, &e, argv[1]);
    }
    {   /* 8-bit CMYK */
        static const expected e = { "cmyk8", 5, VIPS_FORMAT_UCHAR, VIPS_INTERPRETATION_CMYK };
        layer.pixels = cmyka;
        memset(&doc, 0, sizeof doc);
        doc.width = WIDTH, doc.height = HEIGHT, doc.dpi = DPI;
        doc.layers = &layer, doc.layer_count = 1;
        doc.color_mode = MINIPSD_MODE_CMYK;
        doc.composite  = cmyka;
        check_document(&doc, &e, argv[1]);
    }
    {   /* bitmap: one band, no transparency */
        static const expected e = { "bitmap", 1, VIPS_FORMAT_UCHAR, VIPS_INTERPRETATION_B_W };
        memset(&doc, 0, sizeof doc);
        doc.width = WIDTH, doc.height = HEIGHT, doc.dpi = DPI;
        doc.color_mode = MINIPSD_MODE_BITMAP, doc.depth = 1;
        doc.composite  = bits;
        check_document(&doc, &e, argv[1]);
    }

    /* Not a PSD: psdload does not claim it. */
    loader = vips_foreign_find_load_buffer(junk, sizeof junk);
    check(!loader || strcmp(loader, "VipsForeignLoadPsdBuffer") != 0, "junk not claimed", "junk");
    vips_error_clear();

    vips_shutdown();
    printf(failures ? "%d failed\n" : "vips plugin: all passed\n", failures);
    return failures ? 1 : 0;
}
