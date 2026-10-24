/**
 * \file
 * \brief The libvips plugin's test: documents minipsd writes, loaded through libvips.
 *
 * Usage: minipsd-vips-test out-dir [list].  Each document is written into out-dir, then loaded as a
 * file (randomly and sequentially), from a buffer and from a source; the image libvips gives must
 * say what the document does, and its pixels be what minipsd decodes.  Its layers, loaded with
 * `layer`, must be the pixels the document gave, at their place.  list names other files, one path
 * a line, whose composite and every layer are loaded and compared with minipsd's decode the same
 * way (or refused by both).  libvips runs on several threads, so bands are decoded at once.  Exits
 * with 1 when anything fails.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include "minipsd_vips.h"

#include <vips/vips.h>
#include <zlib.h>

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH          301    /* odd sizes, and rows enough for several bands */
#define HEIGHT         203
#define PIXELS         (WIDTH * HEIGHT)
#define RGBA           4
#define MAX_PLANES     5      /* CMYK and transparency */
#define DPI            300.0
#define ASPECT         0.9
#define MM_PER_INCH    25.4
#define EPSILON        1e-9
#define FLOAT_EPSILON  1e-6   /* a 32-bit composite's un-matting, 1 - (1 - c) / a, is off by rounding */
#define PATH_MAX_SIZE  1024
#define NAME_SIZE      (PATH_MAX_SIZE + 64)   /* a path and what it names in it */
#define WORKSPACE_SIZE 256
#define THREADS        4
#define PALETTE        256    /* entries; the palette is 256 reds, 256 greens, 256 blues */
#define TRANSPARENT    5      /* the indexed document's transparent index */
#define HALF_OPAQUE    128
#define ZIP_LEVEL      6
#define COMPOSITE      (-1)   /* decode_image's layer: the composite */
#define STORED_RLE     1      /* a channel's stored compression markers */
#define STORED_ZIP     2

/* The ZIP document's two layers: one reaching past the canvas's left edge, one inside. */
#define ZIP_W          200
#define ZIP_H          100
#define ZIP_PIXELS     (ZIP_W * ZIP_H)

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

static int deflate_zlib(void *user, const uint8_t *src, size_t src_size, uint8_t *dst, size_t *dst_size)
{
    uLongf n = (uLongf)*dst_size;
    int    rc;
    (void)user;
    rc        = compress2(dst, &n, src, (uLong)src_size, ZIP_LEVEL);
    *dst_size = n;
    return rc != Z_OK;
}

static int inflate_zlib(void *user, const uint8_t *src, size_t src_size, uint8_t *dst, size_t dst_size)
{
    uLongf n = (uLongf)dst_size;
    (void)user;
    return uncompress(dst, &n, src, (uLong)src_size) != Z_OK || n != dst_size;
}

/* The composite (layer -1) or a layer as minipsd decodes it, laid out as the loader gives it:
   interleaved, the colour channels and transparency (a layer's always) up to MINIPSD_PLANES,
   bitmap's black 0 and white 255, indexed through its palette.  NULL with the error in *rc when
   minipsd refuses it, or MINIPSD_ERR_SIZE for an empty layer, which the loader refuses too. */
static uint8_t *decode_image(const void *file, size_t size, int layer, size_t *bytes, int *rc)
{
    uint8_t             workspace[WORKSPACE_SIZE], *pixels, *scratch = NULL, *p;
    minipsd_reader     *r;
    minipsd_info        info;
    minipsd_layer_view  view;
    minipsd_resource    res;
    minipsd_target      target;
    minipsd_decode_opts opts;
    size_t              need, i, row;
    uint32_t            width, height;
    unsigned            planes, bands;
    uint16_t            index;
    int                 transparent = -1, n;
    if ((*rc = minipsd_reader_workspace_size(&need)) != MINIPSD_OK) return NULL;
    if (need > sizeof workspace) {
        *rc = MINIPSD_ERR_ARG;
        return NULL;
    }
    memset(&view, 0, sizeof view);
    if ((*rc = minipsd_reader_init(&r, file, size, workspace, sizeof workspace, NULL, &info)) != MINIPSD_OK) return NULL;
    width  = info.width;
    height = info.height;
    planes = info.color_channels + (info.composite_has_alpha ? 1u : 0u);
    if (layer >= 0) {
        for (n = 0; n <= layer; ++n)
            if ((*rc = minipsd_reader_next_layer(r, &view)) != MINIPSD_MORE) {
                if (*rc == MINIPSD_OK) *rc = MINIPSD_ERR_NOT_FOUND;
                return NULL;
            }
        if (view.right <= view.left || view.bottom <= view.top) {
            *rc = MINIPSD_ERR_SIZE;
            return NULL;
        }
        width  = (uint32_t)(view.right - view.left);
        height = (uint32_t)(view.bottom - view.top);
        planes = info.color_channels + 1u;
    }
    if (planes > MINIPSD_PLANES) planes = MINIPSD_PLANES;
    bands = planes;
    if (info.color_mode == MINIPSD_MODE_INDEXED) {
        if (minipsd_reader_find_resource(r, MINIPSD_RES_TRANSPARENCY_INDEX, &res) == MINIPSD_OK
            && minipsd_resource_transparency_index(&res, &index) == MINIPSD_OK && index < PALETTE)
            transparent = index;
        bands = 3 + (transparent >= 0 ? 1u : 0u);
    }
    row    = (size_t)width * bands * (info.depth == 32 ? sizeof(float) : info.depth == 16 ? sizeof(uint16_t) : 1);
    *bytes = row * height;
    if (!(pixels = (uint8_t *)malloc(*bytes ? *bytes : 1))) {
        *rc = MINIPSD_ERR_ALLOC;
        return NULL;
    }
    minipsd_target_interleaved(&target, pixels, row, planes, info.depth);
    if (info.color_mode == MINIPSD_MODE_INDEXED) target.channel[0].pixel_step = (ptrdiff_t)bands;
    memset(&opts, 0, sizeof opts);
    opts.depth = info.depth;
    if (layer < 0) {
        *rc = minipsd_decode_composite(r, &target, &opts);
    } else {
        opts.inflate      = inflate_zlib;
        opts.scratch_size = (size_t)height * (((size_t)width * info.depth + 7) / 8);
        opts.scratch = scratch = (uint8_t *)malloc(opts.scratch_size ? opts.scratch_size : 1);
        *rc = scratch ? minipsd_decode_layer(r, &view, &target, &opts) : MINIPSD_ERR_ALLOC;
        free(scratch);
    }
    if (*rc != MINIPSD_OK) {
        free(pixels);
        return NULL;
    }
    if (info.color_mode == MINIPSD_MODE_BITMAP)
        for (i = 0; i < *bytes; ++i) pixels[i] = pixels[i] ? 0 : UCHAR_MAX;
    if (info.color_mode == MINIPSD_MODE_INDEXED)
        for (p = pixels, i = 0; i < (size_t)width * height; ++i, p += bands) {
            const unsigned k = p[0];
            p[0] = info.color_mode_data.data[k];
            p[1] = info.color_mode_data.data[PALETTE + k];
            p[2] = info.color_mode_data.data[2 * PALETTE + k];
            if (transparent >= 0) p[3] = (int)k == transparent ? 0 : UCHAR_MAX;
        }
    return pixels;
}

/* The loaded image's pixels against minipsd's own decode of the file: the same bytes, or both
   refuse it.  Takes the image.  Returns 1 when both decoded, 0 when both refused. */
static int check_pixels(VipsImage *image, const void *file, size_t size, int layer, const char *name)
{
    size_t   bytes = 0, got_bytes = 0;
    int      rc, both = 0;
    uint8_t *want = decode_image(file, size, layer, &bytes, &rc);
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

typedef struct expected {
    const char        *name;
    int                bands;
    VipsBandFormat     format;
    VipsInterpretation interpretation;
    int                bits;
} expected;

/* What libvips says of one loaded image, against the document's; then its pixels. */
static void check_image(VipsImage *image, const minipsd_doc *doc, const expected *e, const char *how,
                        const out_buffer *file)
{
    char        name[PATH_MAX_SIZE];
    const void *icc;
    size_t      icc_size;
    double      ratio = 0.0;
    int         bits  = 0, layers = -1;
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
    check(vips_image_get_int(image, VIPS_META_BITS_PER_SAMPLE, &bits) == 0 && bits == e->bits, "bits per sample", name);
    check(vips_image_get_int(image, "psd-layer-count", &layers) == 0 && layers == (int)doc->layer_count,
          "layer count", name);
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
    check(check_pixels(image, file->data, file->size, COMPOSITE, name), "decoded", name);
}

/* Each layer of the document, loaded from path with `layer`: its place, its look, and its pixels
   byte for byte as the document gave them (layers are not matted).  sample is a sample's bytes. */
static void check_layers(const char *path, const minipsd_doc *doc, size_t sample, const char *doc_name)
{
    char        name[PATH_MAX_SIZE];
    const char *got_name;
    VipsImage  *image;
    void       *got;
    size_t      i, bytes, want_bytes;
    int         opacity = -1, hidden = -1;
    for (i = 0; i < doc->layer_count; ++i) {
        const minipsd_layer *l    = &doc->layers[i];
        const int            full = !l->top && !l->left && !l->bottom && !l->right;
        const int            w = full ? (int)doc->width : l->right - l->left, h = full ? (int)doc->height : l->bottom - l->top;
        snprintf(name, sizeof name, "%s layer %zu", doc_name, i);
        if (!(image = vips_image_new_from_file(path, "layer", (int)i, NULL))) {
            printf("FAIL %s: not loaded: %s\n", name, vips_error_buffer());
            vips_error_clear();
            ++failures;
            continue;
        }
        check(vips_image_get_width(image) == w && vips_image_get_height(image) == h, "size", name);
        check(vips_image_get_xoffset(image) == l->left && vips_image_get_yoffset(image) == l->top, "offset", name);
        check(vips_image_get_string(image, "psd-layer-name", &got_name) == 0 && strcmp(got_name, l->name) == 0, "name", name);
        check(vips_image_get_int(image, "psd-layer-opacity", &opacity) == 0 && opacity == l->opacity, "opacity", name);
        check(vips_image_get_int(image, "psd-layer-hidden", &hidden) == 0 && !hidden == !l->hidden, "hidden", name);
        want_bytes = (size_t)w * (size_t)h * sample
                   * (doc->color_mode == MINIPSD_MODE_CMYK ? MAX_PLANES : doc->color_mode == MINIPSD_MODE_GREY ? 2u : RGBA);
        got = vips_image_write_to_memory(image, &bytes);
        check(got && bytes == want_bytes && memcmp(got, l->pixels, bytes) == 0, "pixels as given", name);
        vips_error_clear();
        g_free(got);
        g_object_unref(image);
    }
    /* One past the last: no such layer. */
    image = vips_image_new_from_file(path, "layer", (int)doc->layer_count, NULL);
    got   = image ? vips_image_write_to_memory(image, &bytes) : NULL;
    check(!got, "no layer past the last", doc_name);
    vips_error_clear();
    g_free(got);
    if (image) g_object_unref(image);
}

/* Writes doc into dir, then loads it the four ways; path receives the file's, for more checks. */
static void check_document(const minipsd_doc *doc, const minipsd_write_opts *opts, const expected *e, const char *dir,
                           char *path, size_t path_size)
{
    out_buffer  b = { NULL, 0, 0 };
    FILE       *f;
    const char *loader;
    VipsSource *source;
    int         rc = minipsd_write(doc, opts, to_buffer, &b);
    if (rc != MINIPSD_OK) {
        printf("FAIL %s: not written: %s\n", e->name, minipsd_error_string(rc));
        ++failures;
        free(b.data);
        return;
    }
    snprintf(path, path_size, "%s/%s.%s", dir, e->name, doc->format == MINIPSD_PSB ? "psb" : "psd");
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

/* An indexed document's image against its palette, worked out here: each pixel the palette's RGB,
   alpha 0 at the transparent index when there is one. */
static void check_indexed(const char *path, const uint8_t *indices, const uint8_t *palette, int transparent)
{
    const unsigned bands = transparent >= 0 ? RGBA : 3;
    VipsImage     *image = vips_image_new_from_file(path, NULL);
    size_t         bytes = 0, i;
    uint8_t       *got   = image ? (uint8_t *)vips_image_write_to_memory(image, &bytes) : NULL;
    int            same  = got && bytes == (size_t)PIXELS * bands;
    for (i = 0; same && i < PIXELS; ++i) {
        const uint8_t *p = got + i * bands;
        same = p[0] == palette[indices[i]] && p[1] == palette[PALETTE + indices[i]] && p[2] == palette[2 * PALETTE + indices[i]]
            && (transparent < 0 || p[3] == (indices[i] == transparent ? 0 : UCHAR_MAX));
    }
    check(same, "the palette's colours", path);
    vips_error_clear();
    g_free(got);
    if (image) g_object_unref(image);
}

static uint8_t *read_all(const char *path, size_t *size);

/* The same samples: bytes, or floats within FLOAT_EPSILON. */
static int same_samples(const void *got, const void *want, size_t bytes, int floats)
{
    const float *g = (const float *)got, *w = (const float *)want;
    size_t       i;
    if (!floats) return memcmp(got, want, bytes) == 0;
    for (i = 0; i < bytes / sizeof(float); ++i)
        if ((double)fabsf(g[i] - w[i]) > FLOAT_EPSILON) return 0;
    return 1;
}

/* An image over memory, with its interpretation and the test's resolution. */
static VipsImage *make_image(const void *data, int bands, VipsBandFormat format, VipsInterpretation interpretation)
{
    const size_t bytes = (size_t)PIXELS * (size_t)bands * vips_format_sizeof(format);
    VipsImage   *memory = vips_image_new_from_memory(data, bytes, WIDTH, HEIGHT, bands, format), *image = NULL;
    if (memory) {
        vips_copy(memory, &image, "interpretation", interpretation, "xres", DPI / MM_PER_INCH, "yres", DPI / MM_PER_INCH,
                  NULL);
        g_object_unref(memory);
    }
    return image;
}

/* An image saved through psdsave as a file, into a buffer and into a target, the same bytes each
   time, then read back through psdload: the layer is the image (made opaque when it had no
   alpha), and so is the composite when alpha is full (the matte undone exactly).  options are a
   psdsave option and its value, or NULL. */
static void check_saved(VipsImage *image, const char *name, const char *dir, int full_alpha, const char *option,
                        int value)
{
    char        path[PATH_MAX_SIZE];
    VipsImage  *want_image = NULL, *back;
    VipsTarget *target;
    VipsBlob   *blob = NULL;
    void       *buffer = NULL, *want = NULL, *got;
    uint8_t    *file;
    size_t      file_size = 0, buffer_size = 0, want_bytes = 0, got_bytes = 0, target_size = 0;
    const void *target_data = NULL;
    const char *saver;
    int         colors, saved, layer;
    if (!image) {
        check(0, "image made", name);
        return;
    }
    snprintf(path, sizeof path, "%s/%s", dir, name);
    saved = option ? vips_image_write_to_file(image, path, option, value, NULL) : vips_image_write_to_file(image, path, NULL);
    if (saved) {
        printf("FAIL %s: not saved: %s\n", name, vips_error_buffer());
        vips_error_clear();
        ++failures;
        g_object_unref(image);
        return;
    }
    saver = vips_foreign_find_save(path);
    check(saver && strcmp(saver, "VipsForeignSavePsdFile") == 0, "found as psdsave", name);

    /* The same bytes three ways. */
    file   = read_all(path, &file_size);
    target = vips_target_new_to_memory();
    if (option) {
        vips_image_write_to_buffer(image, ".psd", &buffer, &buffer_size, option, value, NULL);
        if (target) vips_image_write_to_target(image, ".psd", target, option, value, NULL);
    } else {
        vips_image_write_to_buffer(image, ".psd", &buffer, &buffer_size, NULL);
        if (target) vips_image_write_to_target(image, ".psd", target, NULL);
    }
    if (target) g_object_get(target, "blob", &blob, NULL);
    if (blob) target_data = vips_blob_get(blob, &target_size);
    check(file && buffer && buffer_size == file_size && memcmp(buffer, file, file_size) == 0, "buffer as the file", name);
    check(file && target_data && target_size == file_size && memcmp(target_data, file, file_size) == 0, "target as the file",
          name);
    vips_error_clear();

    /* Read back: the layer, then the composite. */
    colors = image->Type == VIPS_INTERPRETATION_CMYK ? 4 : image->Bands >= 3 ? 3 : 1;
    if (image->Bands == colors) {
        vips_bandjoin_const1(image, &want_image, image->BandFmt == VIPS_FORMAT_FLOAT ? 1.0
                                                 : image->BandFmt == VIPS_FORMAT_USHORT ? USHRT_MAX : UCHAR_MAX, NULL);
    } else {
        want_image = image;
        g_object_ref(want_image);
    }
    if (want_image) want = vips_image_write_to_memory(want_image, &want_bytes);
    for (layer = 0; layer >= COMPOSITE; --layer) {
        if (layer == COMPOSITE && !full_alpha) break;
        back = vips_image_new_from_file(path, "layer", layer, NULL);
        got  = back ? vips_image_write_to_memory(back, &got_bytes) : NULL;
        check(want && got && got_bytes == want_bytes
                  && same_samples(got, want, want_bytes, layer == COMPOSITE && image->BandFmt == VIPS_FORMAT_FLOAT),
              layer == COMPOSITE ? "composite read back as the image" : "layer read back as the image", name);
        if (back && layer == 0)
            check(fabs(vips_image_get_xres(back) - DPI / MM_PER_INCH) < EPSILON, "resolution read back", name);
        vips_error_clear();
        g_free(got);
        if (back) g_object_unref(back);
    }
    g_free(want);
    if (want_image) g_object_unref(want_image);
    if (blob) vips_area_unref(VIPS_AREA(blob));
    if (target) g_object_unref(target);
    g_free(buffer);
    free(file);
    g_object_unref(image);
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

/* The file's layer count, 0 when minipsd does not read it. */
static size_t layer_count(const void *file, size_t size)
{
    uint8_t         workspace[WORKSPACE_SIZE];
    minipsd_reader *r;
    minipsd_info    info;
    return minipsd_reader_init(&r, file, size, workspace, sizeof workspace, NULL, &info) == MINIPSD_OK ? info.layer_count : 0;
}

/* The stored compression marker of the file's first layer's first channel; -1 without one. */
static int first_layer_compression(const char *path)
{
    uint8_t            workspace[WORKSPACE_SIZE], *data;
    minipsd_reader    *r;
    minipsd_info       info;
    minipsd_layer_view view;
    size_t             size = 0;
    int                marker = -1;
    if (!(data = read_all(path, &size))) return -1;
    if (minipsd_reader_init(&r, data, size, workspace, sizeof workspace, NULL, &info) == MINIPSD_OK
        && minipsd_reader_next_layer(r, &view) == MINIPSD_MORE && view.channel[0].data && view.channel[0].size >= 2)
        marker = view.channel[0].data[0] << 8 | view.channel[0].data[1];
    free(data);
    return marker;
}

/* Each file the list names, its composite and every layer, loaded through libvips and compared
   with minipsd's decode. */
static void check_list(const char *list)
{
    char     path[PATH_MAX_SIZE], name[NAME_SIZE];
    size_t   size, length, files = 0, decoded = 0, layers = 0, layers_decoded = 0, i, count;
    uint8_t *data;
    FILE    *f = fopen(list, "r");
    if (!f) {
        check(0, "list read", list);
        return;
    }
    while (fgets(path, sizeof path, f)) {
        length       = strcspn(path, "\r\n");
        path[length] = 0;
        if (!length) continue;
        if (!(data = read_all(path, &size))) {
            check(0, "file read", path);
            continue;
        }
        ++files;
        decoded += (size_t)check_pixels(vips_image_new_from_file(path, NULL), data, size, COMPOSITE, path);
        for (count = layer_count(data, size), i = 0; i < count; ++i, ++layers) {
            snprintf(name, sizeof name, "%s layer %zu", path, i);
            layers_decoded += (size_t)check_pixels(vips_image_new_from_file(path, "layer", (int)i, NULL), data, size,
                                                   (int)i, name);
        }
        free(data);
    }
    fclose(f);
    printf("%zu files: %zu composites and %zu of %zu layers decoded as minipsd decodes them, the rest refused by both\n",
           files, decoded, layers_decoded, layers);
}

int main(int argc, char **argv)
{
    static const uint8_t icc[] = "not a real ICC profile";
    static uint8_t       rgba[PIXELS * RGBA], cmyka[PIXELS * MAX_PLANES], bits[PIXELS], indices[PIXELS];
    static uint8_t       palette[3 * PALETTE], zip_a[ZIP_PIXELS * RGBA], zip_b[ZIP_PIXELS * RGBA];
    static uint16_t      grey16[PIXELS * 2], rgba16[PIXELS * RGBA];
    static uint8_t       rgb8[PIXELS * 3];
    static float         rgbf[PIXELS * 3];
    static const char    junk[] = "8BPX, not a PSD";
    char                 path[PATH_MAX_SIZE];
    uint8_t              transparency[MINIPSD_TRANSPARENCY_INDEX_SIZE];
    minipsd_layer        layer, zip_layers[2];
    minipsd_doc          doc;
    minipsd_write_opts   opts;
    minipsd_resource     res;
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
    for (i = 0; i < PIXELS * RGBA; ++i) rgba16[i] = (uint16_t)(i * 2731);
    for (i = 0; i < PIXELS; ++i) bits[i] = (uint8_t)(i % 3 == 0);
    for (i = 0; i < PIXELS; ++i) indices[i] = (uint8_t)(i * 13);
    for (i = 0; i < sizeof palette; ++i) palette[i] = (uint8_t)(i * 11 + i / PALETTE * 85 + 3);   /* R, G, B differ */
    for (i = 0; i < sizeof zip_a; ++i) zip_a[i] = (uint8_t)(i * 3);
    for (i = 0; i < sizeof zip_b; ++i) zip_b[i] = (uint8_t)(i / 97);
    for (i = 0; i < sizeof rgb8; ++i) rgb8[i] = (uint8_t)(i * 17 + i / 255);
    for (i = 0; i < PIXELS * 3; ++i) rgbf[i] = (float)(i % 1001) / 1000.0f;

    {   /* 8-bit RGB with a layer, a profile and non-square pixels; as PSD and as PSB */
        static const expected psd = { "rgb8", 4, VIPS_FORMAT_UCHAR, VIPS_INTERPRETATION_sRGB, 8 };
        static const expected psb = { "rgb8-psb", 4, VIPS_FORMAT_UCHAR, VIPS_INTERPRETATION_sRGB, 8 };
        memset(&layer, 0, sizeof layer);
        layer.name    = "Layer";
        layer.pixels  = rgba;
        layer.opacity = UCHAR_MAX;
        memset(&doc, 0, sizeof doc);
        doc.width = WIDTH, doc.height = HEIGHT, doc.dpi = DPI;
        doc.layers = &layer, doc.layer_count = 1;
        doc.icc = icc, doc.icc_size = sizeof icc;
        doc.pixel_aspect_ratio = ASPECT;
        check_document(&doc, NULL, &psd, argv[1], path, sizeof path);
        check_layers(path, &doc, 1, psd.name);
        doc.format = MINIPSD_PSB;
        check_document(&doc, NULL, &psb, argv[1], path, sizeof path);
        check_layers(path, &doc, 1, psb.name);
    }
    {   /* 16-bit RGB, its composite composed by the writer */
        static const expected e = { "rgb16", 4, VIPS_FORMAT_USHORT, VIPS_INTERPRETATION_RGB16, 16 };
        layer.pixels = (const uint8_t *)rgba16;
        memset(&doc, 0, sizeof doc);
        doc.width = WIDTH, doc.height = HEIGHT, doc.dpi = DPI;
        doc.layers = &layer, doc.layer_count = 1;
        doc.depth = 16;
        check_document(&doc, NULL, &e, argv[1], path, sizeof path);
        check_layers(path, &doc, sizeof(uint16_t), e.name);
    }
    /* Grey and CMYK: a layer and its composite (the writer composes RGB only); with a layer, the
       negative layer count says the composite's transparency is there. */
    {   /* 16-bit grey */
        static const expected e = { "grey16", 2, VIPS_FORMAT_USHORT, VIPS_INTERPRETATION_GREY16, 16 };
        layer.pixels = (const uint8_t *)grey16;
        memset(&doc, 0, sizeof doc);
        doc.width = WIDTH, doc.height = HEIGHT, doc.dpi = DPI;
        doc.layers = &layer, doc.layer_count = 1;
        doc.color_mode = MINIPSD_MODE_GREY, doc.depth = 16;
        doc.composite = (const uint8_t *)grey16;
        check_document(&doc, NULL, &e, argv[1], path, sizeof path);
        check_layers(path, &doc, sizeof(uint16_t), e.name);
    }
    {   /* 8-bit CMYK */
        static const expected e = { "cmyk8", 5, VIPS_FORMAT_UCHAR, VIPS_INTERPRETATION_CMYK, 8 };
        layer.pixels = cmyka;
        memset(&doc, 0, sizeof doc);
        doc.width = WIDTH, doc.height = HEIGHT, doc.dpi = DPI;
        doc.layers = &layer, doc.layer_count = 1;
        doc.color_mode = MINIPSD_MODE_CMYK;
        doc.composite  = cmyka;
        check_document(&doc, NULL, &e, argv[1], path, sizeof path);
        check_layers(path, &doc, 1, e.name);
    }
    {   /* bitmap: one band, no transparency, 1 bit a sample */
        static const expected e = { "bitmap", 1, VIPS_FORMAT_UCHAR, VIPS_INTERPRETATION_B_W, 1 };
        memset(&doc, 0, sizeof doc);
        doc.width = WIDTH, doc.height = HEIGHT, doc.dpi = DPI;
        doc.color_mode = MINIPSD_MODE_BITMAP, doc.depth = 1;
        doc.composite  = bits;
        check_document(&doc, NULL, &e, argv[1], path, sizeof path);
        check_layers(path, &doc, 1, e.name);
    }
    {   /* indexed: through the palette, RGB, then RGBA with a transparent index (resource 1047) */
        static const expected rgb = { "indexed", 3, VIPS_FORMAT_UCHAR, VIPS_INTERPRETATION_sRGB, 8 };
        static const expected alpha = { "indexed-1047", 4, VIPS_FORMAT_UCHAR, VIPS_INTERPRETATION_sRGB, 8 };
        memset(&doc, 0, sizeof doc);
        doc.width = WIDTH, doc.height = HEIGHT, doc.dpi = DPI;
        doc.color_mode = MINIPSD_MODE_INDEXED;
        doc.color_mode_data.data = palette, doc.color_mode_data.size = sizeof palette;
        doc.composite = indices;
        check_document(&doc, NULL, &rgb, argv[1], path, sizeof path);
        check_indexed(path, indices, palette, -1);
        check(minipsd_resource_make_transparency_index(TRANSPARENT, transparency, sizeof transparency, NULL) == MINIPSD_OK,
              "transparency index made", alpha.name);
        memset(&res, 0, sizeof res);
        res.id = MINIPSD_RES_TRANSPARENCY_INDEX, res.data.data = transparency, res.data.size = sizeof transparency;
        doc.resources = &res, doc.resource_count = 1;
        check_document(&doc, NULL, &alpha, argv[1], path, sizeof path);
        check_indexed(path, indices, palette, TRANSPARENT);
    }
    {   /* ZIP layers, with and without prediction: one past the canvas's left edge, half opaque,
           one hidden inside it */
        static const expected zip = { "zip", 4, VIPS_FORMAT_UCHAR, VIPS_INTERPRETATION_sRGB, 8 };
        static const expected predicted = { "zip-predicted", 4, VIPS_FORMAT_UCHAR, VIPS_INTERPRETATION_sRGB, 8 };
        memset(zip_layers, 0, sizeof zip_layers);
        zip_layers[0].name = "Past the edge", zip_layers[0].pixels = zip_a, zip_layers[0].opacity = HALF_OPAQUE;
        zip_layers[0].top = 10, zip_layers[0].left = -50;
        zip_layers[1].name = "Hidden", zip_layers[1].pixels = zip_b, zip_layers[1].opacity = UCHAR_MAX, zip_layers[1].hidden = 1;
        zip_layers[1].top = 60, zip_layers[1].left = 70;
        for (i = 0; i < 2; ++i) {
            zip_layers[i].bottom = zip_layers[i].top + ZIP_H;
            zip_layers[i].right  = zip_layers[i].left + ZIP_W;
        }
        memset(&doc, 0, sizeof doc);
        doc.width = WIDTH, doc.height = HEIGHT, doc.dpi = DPI;
        doc.layers = zip_layers, doc.layer_count = 2;
        doc.compression = MINIPSD_ZIP;
        memset(&opts, 0, sizeof opts);
        opts.deflate = deflate_zlib;
        check_document(&doc, &opts, &zip, argv[1], path, sizeof path);
        check_layers(path, &doc, 1, zip.name);
        doc.compression = MINIPSD_ZIP_PREDICTED;
        check_document(&doc, &opts, &predicted, argv[1], path, sizeof path);
        check_layers(path, &doc, 1, predicted.name);
    }

    {   /* The saver: grey, RGB and CMYK at 8 and 16 bits, RGB as floats; ZIP; PSB by option and by
           suffix; a layer name and a profile kept; what PSD cannot hold refused. */
        VipsImage *image, *back;
        uint8_t   *file;
        size_t     size = 0;
        const char *got_name;
        const void *got_icc, *want_icc = NULL;
        size_t      want_icc_size = 0;
        VipsBlob   *profile = NULL;
        check_saved(make_image(rgb8, 3, VIPS_FORMAT_UCHAR, VIPS_INTERPRETATION_sRGB), "save-rgb8.psd", argv[1], 1, NULL, 0);
        check_saved(make_image(rgba, RGBA, VIPS_FORMAT_UCHAR, VIPS_INTERPRETATION_sRGB), "save-rgba8.psd", argv[1], 0, NULL, 0);
        check_saved(make_image(rgba16, 3, VIPS_FORMAT_USHORT, VIPS_INTERPRETATION_RGB16), "save-rgb16.psd", argv[1], 1, NULL, 0);
        check_saved(make_image(indices, 1, VIPS_FORMAT_UCHAR, VIPS_INTERPRETATION_B_W), "save-grey8.psd", argv[1], 1, NULL, 0);
        check_saved(make_image(grey16, 2, VIPS_FORMAT_USHORT, VIPS_INTERPRETATION_GREY16), "save-greya16.psd", argv[1], 0, NULL, 0);
        check_saved(make_image(cmyka, 4, VIPS_FORMAT_UCHAR, VIPS_INTERPRETATION_CMYK), "save-cmyk8.psd", argv[1], 1, NULL, 0);
        check_saved(make_image(rgbf, 3, VIPS_FORMAT_FLOAT, VIPS_INTERPRETATION_scRGB), "save-float.psd", argv[1], 1, NULL, 0);
        check_saved(make_image(rgb8, 3, VIPS_FORMAT_UCHAR, VIPS_INTERPRETATION_sRGB), "save-zip.psd", argv[1], 1, "zip", TRUE);
        snprintf(path, sizeof path, "%s/save-zip.psd", argv[1]);
        check(first_layer_compression(path) == STORED_ZIP, "ZIP by option", path);
        snprintf(path, sizeof path, "%s/save-rgb8.psd", argv[1]);
        check(first_layer_compression(path) == STORED_RLE, "RLE by default", path);
        check_saved(make_image(rgb8, 3, VIPS_FORMAT_UCHAR, VIPS_INTERPRETATION_sRGB), "save-psb.psd", argv[1], 1, "psb", TRUE);
        snprintf(path, sizeof path, "%s/save-psb.psd", argv[1]);
        file = read_all(path, &size);
        check(file && size > 5 && file[5] == MINIPSD_PSB, "PSB by option", path);
        free(file);
        /* A file named .psb is a PSB; a buffer or a target in memory has no name: psb says it. */
        snprintf(path, sizeof path, "%s/save-suffix.psb", argv[1]);
        image = make_image(rgb8, 3, VIPS_FORMAT_UCHAR, VIPS_INTERPRETATION_sRGB);
        check(image && vips_image_write_to_file(image, path, NULL) == 0, "saved", path);
        if (image) g_object_unref(image);
        back = vips_image_new_from_file(path, "layer", 0, NULL);
        check(back != NULL, "read back", path);
        if (back) g_object_unref(back);
        file = read_all(path, &size);
        check(file && size > 5 && file[5] == MINIPSD_PSB, "PSB by suffix", path);
        free(file);

        snprintf(path, sizeof path, "%s/save-named.psd", argv[1]);
        image = make_image(rgb8, 3, VIPS_FORMAT_UCHAR, VIPS_INTERPRETATION_sRGB);
        /* A real profile: libvips drops one it cannot read when it saves. */
        if (vips_profile_load("srgb", &profile, NULL) == 0) {
            want_icc = vips_blob_get(profile, &want_icc_size);
            if (image) vips_image_set_blob_copy(image, VIPS_META_ICC_NAME, want_icc, want_icc_size);
        }
        check(image && vips_call("psdsave", image, path, "layer_name", "Ink", NULL) == 0, "saved by psdsave", path);
        back = vips_image_new_from_file(path, "layer", 0, NULL);
        check(back && vips_image_get_string(back, "psd-layer-name", &got_name) == 0 && strcmp(got_name, "Ink") == 0,
              "layer name kept", path);
        check(back && want_icc && vips_image_get_blob(back, VIPS_META_ICC_NAME, &got_icc, &size) == 0
                  && size == want_icc_size && memcmp(got_icc, want_icc, size) == 0,
              "ICC profile kept", path);
        if (profile) vips_area_unref(VIPS_AREA(profile));
        if (back) g_object_unref(back);
        if (image) g_object_unref(image);

        snprintf(path, sizeof path, "%s/save-refused.psd", argv[1]);
        image = make_image(rgba16, 6, VIPS_FORMAT_UCHAR, VIPS_INTERPRETATION_MULTIBAND);
        check(image && vips_image_write_to_file(image, path, NULL) != 0, "six bands refused", path);
        vips_error_clear();
        if (image) g_object_unref(image);
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
