/**
 * \file
 * \brief The libvips plugin's test: documents minipsd writes, loaded through libvips.
 *
 * Usage: minipsd-vips-test out-dir [list].  Each document is written into out-dir, then loaded as a
 * file (randomly and sequentially), from a buffer and from a source; the image libvips gives must
 * say what the document does, and its pixels be what minipsd decodes.  list names other files, one
 * path a line, each loaded as a file and compared with minipsd's decode the same way (or refused by
 * both).  libvips runs on several threads, so bands are decoded at once.  Exits with 1 when
 * anything fails.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include "minipsd_vips.h"

#include <vips/vips.h>

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH         301    /* odd sizes, and rows enough for several bands */
#define HEIGHT        203
#define PIXELS        (WIDTH * HEIGHT)
#define MAX_PLANES    5      /* CMYK and transparency */
#define DPI           300.0
#define ASPECT        0.9
#define MM_PER_INCH   25.4
#define EPSILON       1e-9
#define PATH_MAX_SIZE 1024
#define WORKSPACE_SIZE 256
#define THREADS       4

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

/* The composite as minipsd decodes it, laid out as the loader gives it: interleaved, the colour
   channels and transparency up to MINIPSD_PLANES, bitmap's black 0 and white 255.  NULL with the
   error in *rc when minipsd refuses the file. */
static uint8_t *decode_composite(const void *file, size_t size, size_t *bytes, int *rc)
{
    uint8_t             workspace[WORKSPACE_SIZE], *pixels;
    minipsd_reader     *r;
    minipsd_info        info;
    minipsd_target      target;
    minipsd_decode_opts opts;
    size_t              need, i, row;
    unsigned            bands;
    if ((*rc = minipsd_reader_workspace_size(&need)) != MINIPSD_OK) return NULL;
    if (need > sizeof workspace) {
        *rc = MINIPSD_ERR_ARG;
        return NULL;
    }
    if ((*rc = minipsd_reader_init(&r, file, size, workspace, sizeof workspace, NULL, &info)) != MINIPSD_OK) return NULL;
    bands = info.color_channels + (info.composite_has_alpha ? 1u : 0u);
    if (bands > MINIPSD_PLANES) bands = MINIPSD_PLANES;
    row    = (size_t)info.width * bands * (info.depth == 32 ? sizeof(float) : info.depth == 16 ? sizeof(uint16_t) : 1);
    *bytes = row * info.height;
    if (!(pixels = (uint8_t *)malloc(*bytes ? *bytes : 1))) {
        *rc = MINIPSD_ERR_ALLOC;
        return NULL;
    }
    minipsd_target_interleaved(&target, pixels, row, bands, info.depth);
    memset(&opts, 0, sizeof opts);
    opts.depth = info.depth;
    if ((*rc = minipsd_decode_composite(r, &target, &opts)) != MINIPSD_OK) {
        free(pixels);
        return NULL;
    }
    if (info.color_mode == MINIPSD_MODE_BITMAP)
        for (i = 0; i < *bytes; ++i) pixels[i] = pixels[i] ? 0 : UCHAR_MAX;
    return pixels;
}

/* The loaded image's pixels against minipsd's own decode of the file: the same bytes, or both
   refuse it.  Takes the image.  Returns 1 when both decoded, 0 when both refused. */
static int check_pixels(VipsImage *image, const void *file, size_t size, const char *name)
{
    size_t   bytes = 0, got_bytes = 0;
    int      rc, both = 0;
    uint8_t *want = decode_composite(file, size, &bytes, &rc);
    void    *got  = image ? vips_image_write_to_memory(image, &got_bytes) : NULL;
    if (want) {
        both = 1;
        if (!got) printf("FAIL %s: libvips refused it: %s\n", name, vips_error_buffer());
        check(got && got_bytes == bytes && memcmp(got, want, bytes) == 0, "pixels as minipsd decodes them", name);
    } else {
        check(!got, "refused as minipsd refuses it", name);
    }
    vips_error_clear();
    g_free(got);
    free(want);
    if (image) g_object_unref(image);
    return both;
}

/* What libvips says of one loaded image, against the document's; then its pixels. */
static void check_image(VipsImage *image, const minipsd_doc *doc, const expected *e, const char *how,
                        const out_buffer *file)
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
    check(check_pixels(image, file->data, file->size, name), "decoded", name);
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
    check_image(vips_image_new_from_file(path, NULL), doc, e, "file", &b);
    check_image(vips_image_new_from_file(path, "access", VIPS_ACCESS_SEQUENTIAL, NULL), doc, e, "file, sequential", &b);
    check_image(vips_image_new_from_buffer(b.data, b.size, "", NULL), doc, e, "buffer", &b);
    source = vips_source_new_from_memory(b.data, b.size);
    check_image(source ? vips_image_new_from_source(source, "", NULL) : NULL, doc, e, "source", &b);
    if (source) g_object_unref(source);
    free(b.data);
}

/* The whole file in a new buffer; NULL when it cannot be read. */
static uint8_t *read_all(const char *path, size_t *size)
{
    uint8_t *data = NULL;
    long     n;
    FILE    *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        *size = (size_t)n;
        data  = (uint8_t *)malloc(*size ? *size : 1);
        if (data && fread(data, 1, *size, f) != *size) {
            free(data);
            data = NULL;
        }
    }
    fclose(f);
    return data;
}

/* Each file the list names, loaded through libvips and compared with minipsd's decode. */
static void check_list(const char *list)
{
    char     path[PATH_MAX_SIZE];
    size_t   size, length, files = 0, decoded = 0;
    uint8_t *data;
    FILE    *f = fopen(list, "r");
    if (!f) {
        check(0, "list read", list);
        return;
    }
    while (fgets(path, sizeof path, f)) {
        length = strcspn(path, "\r\n");
        path[length] = 0;
        if (!length) continue;
        if (!(data = read_all(path, &size))) {
            check(0, "file read", path);
            continue;
        }
        ++files;
        decoded += (size_t)check_pixels(vips_image_new_from_file(path, NULL), data, size, path);
        free(data);
    }
    fclose(f);
    printf("%zu files: %zu decoded as minipsd decodes them, %zu refused by both\n", files, decoded, files - decoded);
}

int main(int argc, char **argv)
{
    static const uint8_t icc[] = "not a real ICC profile";
    static uint8_t       rgba[PIXELS * 4], cmyka[PIXELS * MAX_PLANES], bits[PIXELS];
    static uint16_t      grey16[PIXELS * 2], rgba16[PIXELS * 4];
    static const char    junk[] = "8BPX, not a PSD";
    minipsd_layer        layer;
    minipsd_doc          doc;
    const char          *loader;
    size_t               i;

    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: %s out-dir [list]\n", argv[0]);
        return 2;
    }
    if (VIPS_INIT(argv[0])) vips_error_exit(NULL);
    vips_concurrency_set(THREADS);
    minipsd_vips_init();
    minipsd_vips_init();   /* twice is harmless */

    for (i = 0; i < sizeof rgba; ++i) rgba[i] = (uint8_t)(i * 7);
    for (i = 0; i < sizeof cmyka; ++i) cmyka[i] = (uint8_t)(i * 5);
    for (i = 0; i < PIXELS * 2; ++i) grey16[i] = (uint16_t)(i * 4099);
    for (i = 0; i < PIXELS * 4; ++i) rgba16[i] = (uint16_t)(i * 2731);
    for (i = 0; i < PIXELS; ++i) bits[i] = (uint8_t)(i % 3 == 0);

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
    {   /* 16-bit RGB, its composite composed by the writer */
        static const expected e = { "rgb16", 4, VIPS_FORMAT_USHORT, VIPS_INTERPRETATION_RGB16 };
        layer.pixels = (const uint8_t *)rgba16;
        memset(&doc, 0, sizeof doc);
        doc.width = WIDTH, doc.height = HEIGHT, doc.dpi = DPI;
        doc.layers = &layer, doc.layer_count = 1;
        doc.depth = 16;
        check_document(&doc, &e, argv[1]);
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

    if (argc == 3) check_list(argv[2]);

    vips_shutdown();
    printf(failures ? "%d failed\n" : "vips plugin: all passed\n", failures);
    return failures ? 1 : 0;
}
