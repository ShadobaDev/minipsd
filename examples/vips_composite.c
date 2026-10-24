/**
 * \file
 * \brief Example: layers rendered into a composite by libvips, then written with it by minipsd.
 *
 * minipsd writes and reads PSD; it does not render.  A program that holds layers as libvips
 * images lets libvips flatten them (vips_composite) and hands that image to minipsd as the
 * document's composite, which is what a viewer that ignores layers shows.  The two meet in memory
 * with no conversion: 8-bit RGBA, straight alpha, rows top down (the spec's "Working with libvips").
 *
 * Usage: vips_composite out.psd [in.psd].  Without in.psd, four layers made here (a gradient, a
 * disc multiplied, a panel screened at half opacity, a hidden layer); with it, the layers of an
 * 8-bit RGB file, read with minipsd_read.  Either way the composite is rendered by libvips and the
 * file written with it.  Then, as a check, the file is read back and its composite decoded by
 * minipsd: alpha must match the render exactly, colour too where alpha is full and within the
 * un-matting bound elsewhere.  Exits with 1 when anything fails.
 *
 * Rendered as libvips can: each layer at its rectangle, its opacity multiplied into alpha, hidden
 * layers and group markers left out, the blend modes with a libvips counterpart mapped, the others
 * drawn "over".  Masks, clipping, group modes and effects are left to the caller.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include <vips/vips.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RGBA 4     /* bands: 8-bit red, green, blue, alpha */
#define FULL 255   /* an 8-bit sample's full value */

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

static int to_file(void *file, const void *data, size_t size)
{
    return fwrite(data, 1, size, (FILE *)file) == size;   /* 0 stops the writer */
}

/* A PSD blend key as a libvips blend mode (the spec's table); *mapped is 0 for a key libvips has
   no counterpart for, which is then drawn "over". */
static VipsBlendMode blend_mode(const char key[4], int *mapped)
{
    static const struct { char key[5]; VipsBlendMode mode; } table[] = {
        { "norm", VIPS_BLEND_MODE_OVER },        { "mul ", VIPS_BLEND_MODE_MULTIPLY },
        { "scrn", VIPS_BLEND_MODE_SCREEN },      { "over", VIPS_BLEND_MODE_OVERLAY },
        { "dark", VIPS_BLEND_MODE_DARKEN },      { "lite", VIPS_BLEND_MODE_LIGHTEN },
        { "lddg", VIPS_BLEND_MODE_ADD },         { "div ", VIPS_BLEND_MODE_COLOUR_DODGE },
        { "idiv", VIPS_BLEND_MODE_COLOUR_BURN }, { "hLit", VIPS_BLEND_MODE_HARD_LIGHT },
        { "sLit", VIPS_BLEND_MODE_SOFT_LIGHT },  { "diff", VIPS_BLEND_MODE_DIFFERENCE },
        { "smud", VIPS_BLEND_MODE_EXCLUSION }
    };
    size_t k;
    *mapped = 1;
    if (!key[0] && !key[1] && !key[2] && !key[3]) return VIPS_BLEND_MODE_OVER;   /* zero: "norm" */
    for (k = 0; k < sizeof table / sizeof table[0]; ++k)
        if (memcmp(key, table[k].key, 4) == 0) return table[k].mode;
    *mapped = 0;
    return VIPS_BLEND_MODE_OVER;
}

/* A layer's pixels as a libvips image, wrapped without a copy, its opacity in its alpha. */
static VipsImage *layer_image(const minipsd_layer *l)
{
    const int  w = (int)(l->right - l->left), h = (int)(l->bottom - l->top);
    VipsImage *wrapped, *srgb, *out;
    wrapped = vips_image_new_from_memory(l->pixels, (size_t)w * (size_t)h * RGBA, w, h, RGBA, VIPS_FORMAT_UCHAR);
    if (!wrapped) return NULL;
    if (vips_copy(wrapped, &srgb, "interpretation", VIPS_INTERPRETATION_sRGB, NULL)) {   /* RGBA, not 4 bands of anything */
        g_object_unref(wrapped);
        return NULL;
    }
    g_object_unref(wrapped);
    if (l->opacity == FULL) return srgb;
    {
        double scale[RGBA] = { 1, 1, 1, 0 }, offset[RGBA] = { 0, 0, 0, 0 };
        scale[RGBA - 1] = l->opacity / (double)FULL;
        if (vips_linear(srgb, &out, scale, offset, RGBA, "uchar", TRUE, NULL)) out = NULL;
        g_object_unref(srgb);
        return out;
    }
}

/* The document's visible layers flattened by vips_composite onto a transparent canvas, as a new
   width * height * 4 buffer (g_free it); NULL on failure. */
static uint8_t *render(const minipsd_doc *d, size_t *unmapped)
{
    VipsImage   **in = (VipsImage **)calloc(d->layer_count + 1, sizeof *in), *canvas, *out = NULL, *flat = NULL;
    int          *modes = (int *)calloc(d->layer_count + 1, sizeof *modes);
    int          *x = (int *)calloc(d->layer_count + 1, sizeof *x), *y = (int *)calloc(d->layer_count + 1, sizeof *y);
    int           mapped, n = 0, k;
    uint8_t      *pixels = NULL;
    size_t        i, size = 0;

    *unmapped = 0;
    if (!in || !modes || !x || !y) goto done;
    /* The canvas, transparent, makes the composite the document's size; sRGB, as every layer. */
    if (vips_black(&canvas, (int)d->width, (int)d->height, "bands", RGBA, NULL)) goto done;
    k = vips_copy(canvas, &in[n], "interpretation", VIPS_INTERPRETATION_sRGB, NULL);
    g_object_unref(canvas);
    if (k) goto done;
    ++n;
    for (i = 0; i < d->layer_count; ++i) {   /* bottom first, as minipsd and vips_composite both list them */
        const minipsd_layer *l = &d->layers[i];
        if (l->hidden || l->section_divider_type != MINIPSD_SECTION_LAYER || !l->pixels
            || l->right <= l->left || l->bottom <= l->top)
            continue;
        if (!(in[n] = layer_image(l))) goto done;
        modes[n - 1] = (int)blend_mode(l->blend_mode, &mapped);
        *unmapped   += !mapped;
        x[n - 1]     = (int)l->left;   /* where the layer goes; it may reach past the canvas */
        y[n - 1]     = (int)l->top;
        ++n;
    }
    if (n == 1) {   /* nothing visible: the transparent canvas */
        out = in[0];
        g_object_ref(out);
    } else {
        VipsArrayInt *xs = vips_array_int_new(x, n - 1), *ys = vips_array_int_new(y, n - 1);
        k = vips_composite(in, &out, n, modes, "x", xs, "y", ys, NULL);
        vips_area_unref(VIPS_AREA(xs));
        vips_area_unref(VIPS_AREA(ys));
        if (k) goto done;
    }
    if (vips_cast_uchar(out, &flat, NULL) == 0) {   /* vips_composite may give a wider format */
        pixels = (uint8_t *)vips_image_write_to_memory(flat, &size);
        if (pixels && size != (size_t)d->width * d->height * RGBA) {
            g_free(pixels);
            pixels = NULL;
        }
    }
done:
    for (k = 0; in && k < n; ++k) g_object_unref(in[k]);
    if (out) g_object_unref(out);
    if (flat) g_object_unref(flat);
    free(in);
    free(modes);
    free(x);
    free(y);
    return pixels;
}

/* The written file's composite, decoded by minipsd, against the render: alpha exactly; colour
   exactly where alpha is full, elsewhere within the un-matting bound, max / (2 * alpha) levels
   (the spec's API, "Un-matting").  Returns the samples outside it, or -1 when it cannot decode. */
static long check(const uint8_t *file, size_t size, const uint8_t *render, uint32_t width, uint32_t height)
{
    uint8_t         workspace[256];
    minipsd_reader *r;
    minipsd_info    info;
    minipsd_target  t;
    uint8_t        *got;
    size_t          i, n = (size_t)width * height;
    long            off = 0;
    if (minipsd_reader_init(&r, file, size, workspace, sizeof workspace, NULL, &info) != MINIPSD_OK) return -1;
    got = (uint8_t *)malloc(n * RGBA);
    if (!got) return -1;
    minipsd_target_rgba(&t, got, (size_t)width * RGBA);
    if (minipsd_decode_composite(r, &t, NULL) != MINIPSD_OK) { free(got); return -1; }
    for (i = 0; i < n * RGBA; ++i) {
        const int a = render[i - i % RGBA + RGBA - 1], diff = abs((int)got[i] - (int)render[i]);
        if (i % RGBA == RGBA - 1 || a == FULL) off += diff != 0;
        else if (a) off += diff > FULL / (2 * a) + 1;   /* + 1: rounding on both sides */
    }
    free(got);
    return off;
}

/* --- the layers made here, for a run without an input file --- */

#define W 256
#define H 192

static uint8_t background[W * H * RGBA], disc[128 * 128 * RGBA], panel[120 * 80 * RGBA], hidden[W * H * RGBA];

static void make_layers(minipsd_layer layers[4])
{
    int x, y;
    for (y = 0; y < H; ++y)
        for (x = 0; x < W; ++x) {   /* a gradient: red across, green down, opaque */
            uint8_t *p = background + ((size_t)y * W + (size_t)x) * RGBA;
            p[0] = (uint8_t)(x * FULL / (W - 1)); p[1] = (uint8_t)(y * FULL / (H - 1)); p[2] = 160; p[3] = FULL;
        }
    for (y = 0; y < 128; ++y)
        for (x = 0; x < 128; ++x) {   /* an orange disc, transparent around it */
            uint8_t *p = disc + ((size_t)y * 128 + (size_t)x) * RGBA;
            const int dx = x - 64, dy = y - 64;
            p[0] = 255; p[1] = 150; p[2] = 40; p[3] = dx * dx + dy * dy < 60 * 60 ? FULL : 0;
        }
    for (x = 0; x < 120 * 80; ++x) {   /* a blue panel */
        panel[x * RGBA] = 40; panel[x * RGBA + 1] = 90; panel[x * RGBA + 2] = 220; panel[x * RGBA + 3] = FULL;
    }
    for (x = 0; x < W * H; ++x) hidden[x * RGBA] = hidden[x * RGBA + 3] = FULL;   /* red, never shown */

    memset(layers, 0, 4 * sizeof *layers);   /* the structs grow at the end: zero is every default */
    layers[0].name = "Background"; layers[0].pixels = background; layers[0].opacity = FULL;
    layers[0].right = W; layers[0].bottom = H;
    layers[1].name = "Disc"; layers[1].pixels = disc; layers[1].opacity = FULL;
    memcpy(layers[1].blend_mode, "mul ", 4);
    layers[1].left = -32; layers[1].top = 96; layers[1].right = 96; layers[1].bottom = 224;   /* part off the canvas */
    layers[2].name = "Panel"; layers[2].pixels = panel; layers[2].opacity = 128;
    memcpy(layers[2].blend_mode, "scrn", 4);
    layers[2].left = 100; layers[2].top = 40; layers[2].right = 220; layers[2].bottom = 120;
    layers[3].name = "Hidden"; layers[3].pixels = hidden; layers[3].opacity = FULL; layers[3].hidden = 1;
    layers[3].right = W; layers[3].bottom = H;
}

int main(int argc, char **argv)
{
    minipsd_layer layers[4];
    minipsd_doc   made, *doc = &made, *read = NULL;
    uint8_t      *input = NULL, *flat = NULL, *written = NULL;
    size_t        size = 0, unmapped = 0;
    uint32_t      lost = 0;
    long          off = -1;
    FILE         *f;
    int           rc = MINIPSD_OK, failed = 1;

    if (argc < 2) { fputs("usage: vips_composite out.psd [in.psd]\n", stderr); return 2; }
    if (VIPS_INIT(argv[0])) { fputs("vips_composite: libvips did not start\n", stderr); return 1; }

    if (argc > 2) {   /* the layers of a file */
        input = read_all(argv[2], &size);
        rc    = input ? minipsd_read(input, size, NULL, NULL, &read, &lost) : MINIPSD_ERR_OPEN;
        free(input);
        if (rc != MINIPSD_OK) { fprintf(stderr, "vips_composite: %s: %s\n", argv[2], minipsd_error_string(rc)); goto done; }
        if (read->depth == 16) { fputs("vips_composite: an 8-bit file, please\n", stderr); goto done; }
        if (lost) printf("warning: the file holds content minipsd cannot carry yet (lost %lu)\n", (unsigned long)lost);
        doc = read;
    } else {
        make_layers(layers);
        memset(&made, 0, sizeof made);
        made.width = W; made.height = H; made.dpi = 72.0; made.layers = layers; made.layer_count = 4;
    }

    flat = render(doc, &unmapped);   /* libvips flattens the layers ... */
    if (!flat) { fprintf(stderr, "vips_composite: libvips: %s\n", vips_error_buffer()); goto done; }
    doc->composite        = flat;    /* ... and minipsd writes them with that image */
    doc->composite_matted = 0;       /* straight alpha: the writer mattes it, as the format wants */

    f = fopen(argv[1], "wb");
    if (!f) rc = MINIPSD_ERR_OPEN;
    else {
        rc = minipsd_write(doc, NULL, to_file, f);
        if (fclose(f) != 0 && rc == MINIPSD_OK) rc = MINIPSD_ERR_OPEN;
    }
    if (rc != MINIPSD_OK) { fprintf(stderr, "vips_composite: %s: %s\n", argv[1], minipsd_error_string(rc)); goto done; }

    written = read_all(argv[1], &size);
    off     = written ? check(written, size, flat, doc->width, doc->height) : -1;
    printf("%lu layers, %lu blend modes drawn as normal; composite read back: %s (%ld samples off)\n",
           (unsigned long)doc->layer_count, (unsigned long)unmapped, off == 0 ? "as rendered" : "DIFFERENT", off);
    fflush(stdout);   /* now: with libvips' prebuilt Windows build, exit does not always flush it */
    failed = off != 0;
done:
    free(written);
    g_free(flat);
    if (read) minipsd_doc_free(read);
    vips_shutdown();
    return failed;
}
