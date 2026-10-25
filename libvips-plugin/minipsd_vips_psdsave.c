/**
 * \file
 * \brief psdsave, psdsave_buffer, psdsave_target: an image as a PSD or PSB, one layer or flat.
 *
 * Laid out as libvips' own savers are (a base class over a VipsTarget; the file and buffer
 * variants make one), so the code can move into libvips/foreign/ (as psdsave.c, the minipsd_vips_
 * names back to vips_foreign_).  An image with alpha is written as one layer that is also the
 * composite; one without, flat, as Photoshop saves an image without layers: the composite only,
 * without transparency (with `layered`, one layer without transparency too).  Grey, RGB or CMYK,
 * 8 or 16 bits, grey and RGB also 32, CMYK back to PSD's inverted ink; the resolution, the ICC
 * profile, EXIF, XMP and IPTC kept.  Each channel is encoded once, as minipsd encodes it, and
 * handed to minipsd as stored; the image is held whole in memory while it is encoded.
 *
 * minipsd's writer is compiled in here, private (MINIPSD_STATIC; the reader left out), so a
 * program that uses minipsd itself keeps its own.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_STATIC
#define MINIPSD_NO_READER
#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include "minipsd_vips.h"
#include "minipsd_vips_psdsave.h"
#include "minipsd_vips_util.h"

#include <zlib.h>

#include <limits.h>
#include <string.h>

/* --- The saver's base class: an image as a one-layer PSD into a target --------------------- */

#define PSD_ZIP_LEVEL      6           /* zlib's default */
#define PSD_DEFAULT_DPI    72.0        /* when the image's resolution is none PSD can hold */
#define PSD_MAX_DPI        65536.0     /* PSD's resolution is a 16.16 fixed-point number */
#define PSD_LAYER_NAME     "Layer 1"
#define PSD_MAX_RESOURCES  3           /* EXIF, XMP, IPTC */
#define PSD_MAX_PLANES     5           /* the most bands it takes: CMYK and alpha */

typedef struct _MinipsdVipsPsdsave {
    VipsForeignSave parent_object;
    VipsTarget     *target;      /* set by the subclass's build */
    gboolean        psb;         /* the option; also a .psb file, or a side past PSD's */
    MinipsdVipsCompression compression;   /* the option: the layer's channels; none: the composite's too */
    char           *layer_name;  /* the option */
    gboolean        layered;     /* the option: a layer also for an image without alpha */
} MinipsdVipsPsdsave;

typedef VipsForeignSaveClass MinipsdVipsPsdsaveClass;

G_DEFINE_ABSTRACT_TYPE(MinipsdVipsPsdsave, minipsd_vips_psdsave, VIPS_TYPE_FOREIGN_SAVE)

/* MinipsdVipsCompression as a GEnum, so libvips takes it by its nick ("rle", "zip", "none"). */
static GType minipsd_vips_psdsave_compression_get_type(void)
{
    static gsize type = 0;
    if (g_once_init_enter(&type)) {
        static const GEnumValue values[] = {
            { MINIPSD_VIPS_COMPRESSION_RLE, "MINIPSD_VIPS_COMPRESSION_RLE", "rle" },
            { MINIPSD_VIPS_COMPRESSION_ZIP, "MINIPSD_VIPS_COMPRESSION_ZIP", "zip" },
            { MINIPSD_VIPS_COMPRESSION_NONE, "MINIPSD_VIPS_COMPRESSION_NONE", "none" },
            { 0, NULL, NULL },
        };
        g_once_init_leave(&type, g_enum_register_static("MinipsdVipsCompression", values));
    }
    return (GType)type;
}

/* What libvips' band formats become: 8-bit, 16-bit and float samples, as PSD has them. */
static VipsBandFormat minipsd_vips_psdsave_save_formats[] = {
    /* UCHAR, CHAR, USHORT, SHORT, UINT, INT, FLOAT, COMPLEX, DOUBLE, DPCOMPLEX */
    VIPS_FORMAT_UCHAR, VIPS_FORMAT_UCHAR, VIPS_FORMAT_USHORT, VIPS_FORMAT_USHORT, VIPS_FORMAT_USHORT,
    VIPS_FORMAT_USHORT, VIPS_FORMAT_FLOAT, VIPS_FORMAT_FLOAT, VIPS_FORMAT_FLOAT, VIPS_FORMAT_FLOAT
};

static void minipsd_vips_psdsave_dispose(GObject *gobject)
{
    MinipsdVipsPsdsave *psd = (MinipsdVipsPsdsave *)gobject;
    VIPS_UNREF(psd->target);
    G_OBJECT_CLASS(minipsd_vips_psdsave_parent_class)->dispose(gobject);
}

static int minipsd_vips_psdsave_to_target(void *user, const void *data, size_t size)
{
    return vips_target_write((VipsTarget *)user, data, size) == 0;   /* 0 stops the writer */
}

/* A blob of the image as a resource to keep, when it has one. */
static void minipsd_vips_psdsave_keep_blob(VipsImage *image, const char *name, uint16_t id, minipsd_resource *res,
                                           size_t *count)
{
    const void *data;
    size_t      size;
    if (!vips_image_get_typeof(image, name) || vips_image_get_blob(image, name, &data, &size) || !size) return;
    memset(&res[*count], 0, sizeof res[*count]);
    res[*count].id        = id;
    res[*count].data.data = (const uint8_t *)data;
    res[*count].data.size = size;
    ++*count;
}

/* --- The channels, encoded once: rows as they come, kept compressed, written as stored ------- */

#define PSD_MARKER_SIZE    2           /* a stored channel's compression marker, big-endian */
#define PSD_STORED_RAW     0           /* ... raw */
#define PSD_STORED_RLE     1           /* ... PackBits, its rows' sizes ahead of them */
#define PSD_STORED_ZIP     2           /* ... zlib, without prediction */
#define PSD_COUNT_PSD      2           /* an RLE row's size: two bytes in a PSD */
#define PSD_COUNT_PSB      4           /* ... four in a PSB */

/* Bytes that grow, past what guint can count. */
typedef struct _MinipsdVipsPsdsaveBytes {
    uint8_t *data;
    size_t   size, room;
} MinipsdVipsPsdsaveBytes;

static int minipsd_vips_psdsave_append(MinipsdVipsPsdsaveBytes *b, const void *data, size_t size)
{
    if (size > b->room - b->size) {
        const size_t room = VIPS_MAX(b->room * 2, b->size + size);
        uint8_t     *grown;
        if (room < b->size || !(grown = (uint8_t *)g_try_realloc(b->data, room))) return -1;
        b->data = grown;
        b->room = room;
    }
    memcpy(b->data + b->size, data, size);
    b->size += size;
    return 0;
}

/* One channel: its rows, encoded one after another, and each one's size. */
typedef struct _MinipsdVipsPsdsaveChannel {
    MinipsdVipsPsdsaveBytes rows;
    size_t                 *sizes;
} MinipsdVipsPsdsaveChannel;

/* What the build encodes: the layer's channels (the colour ones, then transparency) when there is
   a layer, the composite's always (the colour ones, then transparency when the image has it). */
typedef struct _MinipsdVipsPsdsaveEncoding {
    MinipsdVipsPsdsaveChannel layer[PSD_MAX_PLANES], composite[PSD_MAX_PLANES];
    unsigned                  planes;      /* the image's bands: colour channels, then alpha */
    int                       layered, alpha, depth, mode;
    MinipsdVipsCompression    compression;
    uint32_t                  width, height;
    uint8_t                  *row;         /* one encoded row, MINIPSD_ROW_BOUND */
    size_t                    row_size;
    uint8_t                  *matted;      /* one row of pixels, matted for the composite */
} MinipsdVipsPsdsaveEncoding;

static void minipsd_vips_psdsave_encoding_free(MinipsdVipsPsdsaveEncoding *e)
{
    unsigned k;
    for (k = 0; k < PSD_MAX_PLANES; ++k) {
        g_free(e->layer[k].rows.data);
        g_free(e->layer[k].sizes);
        g_free(e->composite[k].rows.data);
        g_free(e->composite[k].sizes);
    }
    g_free(e->row);
    g_free(e->matted);
    memset(e, 0, sizeof *e);
}

static int minipsd_vips_psdsave_encoding_init(MinipsdVipsPsdsaveEncoding *e, VipsImage *in, int depth, int mode, int alpha,
                                              int layered, MinipsdVipsCompression compression)
{
    const size_t sample = (size_t)(depth / CHAR_BIT);
    unsigned     k;
    memset(e, 0, sizeof *e);
    e->planes   = (unsigned)in->Bands;
    e->layered  = layered;
    e->compression = compression;
    e->alpha    = alpha;
    e->depth    = depth;
    e->mode     = mode;
    e->width    = (uint32_t)in->Xsize;
    e->height   = (uint32_t)in->Ysize;
    e->row_size = (size_t)MINIPSD_ROW_BOUND(e->width, depth);
    e->row      = (uint8_t *)g_try_malloc(e->row_size);
    e->matted   = (uint8_t *)g_try_malloc((size_t)e->width * e->planes * sample);
    if (!e->row || !e->matted) return -1;
    for (k = 0; k < e->planes; ++k) {
        if (!(e->composite[k].sizes = g_try_new(size_t, e->height))) return -1;
        if (layered && !(e->layer[k].sizes = g_try_new(size_t, e->height))) return -1;
    }
    return 0;
}

/* Row y of channel k out of interleaved pixels, encoded into c: PackBits, or raw for a channel
   deflated whole at the end. */
static int minipsd_vips_psdsave_encode(MinipsdVipsPsdsaveEncoding *e, MinipsdVipsPsdsaveChannel *c, const uint8_t *pixels,
                                       unsigned k, uint32_t y, int raw)
{
    const size_t sample = (size_t)(e->depth / CHAR_BIT);
    size_t       n;
    if (minipsd_encode_row(raw ? MINIPSD_RAW : MINIPSD_RLE, e->depth, pixels + k * sample, e->planes * sample, e->width, e->row,
                           e->row_size, &n) != MINIPSD_OK
        || minipsd_vips_psdsave_append(&c->rows, e->row, n))
        return -1;
    c->sizes[y] = n;
    return 0;
}

/* count rows of interleaved pixels, from row y on, stride bytes apart: each channel's rows
   encoded, the layer's as they are, the composite's matted (RGB with alpha, as minipsd mattes a
   composite it is given). */
static int minipsd_vips_psdsave_encode_rows(MinipsdVipsPsdsaveEncoding *e, const uint8_t *rows, uint32_t y, uint32_t count,
                                            size_t stride)
{
    const size_t bytes = (size_t)e->width * e->planes * (size_t)(e->depth / CHAR_BIT);
    uint32_t     i;
    unsigned     k;
    for (i = 0; i < count; ++i, rows += stride) {
        const uint8_t *composite = rows;
        for (k = 0; e->layered && k < e->planes; ++k)
            if (minipsd_vips_psdsave_encode(e, &e->layer[k], rows, k, y + i, e->compression != MINIPSD_VIPS_COMPRESSION_RLE))
                return -1;
        if (e->alpha) {   /* matted as minipsd mattes a composite it is given (RGB; the others as they are) */
            memcpy(e->matted, rows, bytes);
            (void)minipsd_matte_row(e->mode, e->depth, e->matted, e->width);
            composite = e->matted;
        }
        for (k = 0; k < e->planes; ++k)
            if (minipsd_vips_psdsave_encode(e, &e->composite[k], composite, k, y + i, e->compression == MINIPSD_VIPS_COMPRESSION_NONE))
                return -1;
    }
    return 0;
}

/* Puts n, big-endian, in size bytes. */
static uint8_t *minipsd_vips_psdsave_put(uint8_t *p, uint64_t n, unsigned size)
{
    unsigned i;
    for (i = 0; i < size; ++i) p[i] = (uint8_t)(n >> (CHAR_BIT * (size - 1 - i)));
    return p + size;
}

/* Channels as a file stores them, one block: one marker, every channel's row sizes (RLE), then
   every channel's rows (the composite's layout; a layer channel is the same, one channel).  NULL
   when a size does not fit its count, or memory runs out. */
static uint8_t *minipsd_vips_psdsave_stored(MinipsdVipsPsdsaveChannel *c, unsigned channels, uint32_t height, unsigned count,
                                            int raw, size_t *size)
{
    const uint64_t max = count == PSD_COUNT_PSD ? UINT16_MAX : UINT32_MAX;
    uint8_t       *block, *p;
    unsigned       k;
    uint32_t       y;
    *size = PSD_MARKER_SIZE + (raw ? 0 : (size_t)channels * height * count);
    for (k = 0; k < channels; ++k) *size += c[k].rows.size;
    if (!(block = (uint8_t *)g_try_malloc(*size))) return NULL;
    p = minipsd_vips_psdsave_put(block, raw ? PSD_STORED_RAW : PSD_STORED_RLE, PSD_MARKER_SIZE);
    for (k = 0; !raw && k < channels; ++k)
        for (y = 0; y < height; ++y) {
            if (c[k].sizes[y] > max) {
                g_free(block);
                return NULL;
            }
            p = minipsd_vips_psdsave_put(p, c[k].sizes[y], count);
        }
    for (k = 0; k < channels; ++k) {
        memcpy(p, c[k].rows.data, c[k].rows.size);
        p += c[k].rows.size;
        g_free(c[k].rows.data);   /* copied: the block holds it now */
        memset(&c[k].rows, 0, sizeof c[k].rows);
    }
    return block;
}

/* A channel's raw rows deflated as a file stores them: the marker, then the zlib stream. */
static uint8_t *minipsd_vips_psdsave_stored_zip(MinipsdVipsPsdsaveChannel *c, size_t *size)
{
    uLongf   n = compressBound((uLong)c->rows.size);
    uint8_t *block;
    if ((uLong)c->rows.size != c->rows.size || !(block = (uint8_t *)g_try_malloc(PSD_MARKER_SIZE + (size_t)n))) return NULL;
    (void)minipsd_vips_psdsave_put(block, PSD_STORED_ZIP, PSD_MARKER_SIZE);
    if (compress2(block + PSD_MARKER_SIZE, &n, c->rows.data, (uLong)c->rows.size, PSD_ZIP_LEVEL) != Z_OK) {
        g_free(block);
        return NULL;
    }
    *size = PSD_MARKER_SIZE + (size_t)n;
    g_free(c->rows.data);
    memset(&c->rows, 0, sizeof c->rows);
    return block;
}

static int minipsd_vips_psdsave_build(VipsObject *object)
{
    VipsForeignSave           *save = (VipsForeignSave *)object;
    MinipsdVipsPsdsave        *psd  = (MinipsdVipsPsdsave *)object;
    VipsImage                 *in;
    minipsd_resource           resources[PSD_MAX_RESOURCES];
    minipsd_bytes              stored[PSD_MAX_PLANES];
    minipsd_layer              layer;
    minipsd_doc                doc;
    MinipsdVipsPsdsaveEncoding e;
    uint8_t                   *blocks[PSD_MAX_PLANES + 1];   /* the layer's channels, then the composite */
    const void                *icc;
    const char                *filename;
    size_t                     icc_size, bytes, colors, count = 0, size = 0;
    double                     ratio;
    void                      *pixels;
    unsigned                   k, table;
    int                        rc, alpha, failed;

    if (VIPS_OBJECT_CLASS(minipsd_vips_psdsave_parent_class)->build(object)) return -1;
    in = save->ready;

    /* The mode from the bands: grey, RGB or CMYK, each with or without alpha. */
    memset(&doc, 0, sizeof doc);
    if (in->Type == VIPS_INTERPRETATION_CMYK && (in->Bands == 4 || in->Bands == 5)) {
        doc.color_mode = MINIPSD_MODE_CMYK;
        colors         = 4;
    } else if (in->Bands == 3 || in->Bands == 4) {
        doc.color_mode = MINIPSD_MODE_RGB;
        colors         = PSD_RGB;
    } else if (in->Bands == 1 || in->Bands == 2) {
        doc.color_mode = MINIPSD_MODE_GREY;
        colors         = 1;
    } else {
        vips_error(VIPS_OBJECT_GET_CLASS(object)->nickname, "%s", "PSD takes grey, RGB or CMYK, each with or without alpha");
        return -1;
    }
    doc.depth  = in->BandFmt == VIPS_FORMAT_FLOAT ? 32 : in->BandFmt == VIPS_FORMAT_USHORT ? 16 : 8;
    alpha      = (size_t)in->Bands > colors;   /* transparency as the image has it: none, none added */
    doc.width  = (uint32_t)in->Xsize;
    doc.height = (uint32_t)in->Ysize;
    doc.dpi    = in->Xres * MM_PER_INCH;
    if (!(doc.dpi > 0.0 && doc.dpi < PSD_MAX_DPI)) doc.dpi = PSD_DEFAULT_DPI;
    /* PSB by the option, a file named .psb (libvips writes files through a target too), or a side
       past PSD's. */
    filename   = vips_connection_filename(VIPS_CONNECTION(psd->target));
    doc.format = psd->psb || (filename && vips_iscasepostfix(filename, ".psb")) || in->Xsize > MINIPSD_MAX_SIDE
                      || in->Ysize > MINIPSD_MAX_SIDE
                   ? MINIPSD_PSB
                   : MINIPSD_PSD;
    table      = doc.format == MINIPSD_PSB ? PSD_COUNT_PSB : PSD_COUNT_PSD;

    /* Every channel encoded once, as minipsd encodes it, and handed to it as stored: minipsd copies
       it, where it would encode each row again for its size. */
    if (!(pixels = vips_image_write_to_memory(in, &bytes))) return -1;
    if (doc.color_mode == MINIPSD_MODE_CMYK) minipsd_vips__invert_cmyk(pixels, (size_t)in->Xsize * (size_t)in->Ysize, (unsigned)in->Bands, doc.depth);
    memset(blocks, 0, sizeof blocks);
    memset(stored, 0, sizeof stored);
    failed = minipsd_vips_psdsave_encoding_init(&e, in, doc.depth, doc.color_mode, alpha, alpha || psd->layered, psd->compression)
             || minipsd_vips_psdsave_encode_rows(&e, (const uint8_t *)pixels, 0, e.height, bytes / e.height);
    g_free(pixels);
    for (k = 0; !failed && e.layered && k < e.planes; ++k) {   /* stored: the colour channels, then transparency */
        blocks[k] = e.compression == MINIPSD_VIPS_COMPRESSION_ZIP
                        ? minipsd_vips_psdsave_stored_zip(&e.layer[k], &size)
                        : minipsd_vips_psdsave_stored(&e.layer[k], 1, e.height, table, e.compression == MINIPSD_VIPS_COMPRESSION_NONE, &size);
        failed    = !blocks[k];
        stored[k].data = blocks[k];
        stored[k].size = size;
    }
    if (!failed) {
        blocks[PSD_MAX_PLANES] = minipsd_vips_psdsave_stored(e.composite, e.planes, e.height, table,
                                                             e.compression == MINIPSD_VIPS_COMPRESSION_NONE, &size);
        failed                 = !blocks[PSD_MAX_PLANES];
        doc.composite_stored.data = blocks[PSD_MAX_PLANES];
        doc.composite_stored.size = size;
    }
    minipsd_vips_psdsave_encoding_free(&e);

    memset(&layer, 0, sizeof layer);
    layer.name     = psd->layer_name ? psd->layer_name : PSD_LAYER_NAME;
    layer.opacity  = UCHAR_MAX;
    layer.no_alpha = (uint8_t)!alpha;
    layer.stored   = stored;               /* its own rectangle, the canvas: a stored layer gives it */
    layer.bottom   = (int32_t)doc.height;
    layer.right    = (int32_t)doc.width;
    doc.layers             = alpha || psd->layered ? &layer : NULL;   /* without alpha: flat, unless asked */
    doc.layer_count        = doc.layers ? 1 : 0;
    doc.composite_channels = (uint16_t)in->Bands;
    doc.composite_alpha    = (uint8_t)alpha;
    doc.compression        = MINIPSD_RLE;
    if (vips_image_get_typeof(in, VIPS_META_ICC_NAME) && !vips_image_get_blob(in, VIPS_META_ICC_NAME, &icc, &icc_size)) {
        doc.icc      = (const uint8_t *)icc;
        doc.icc_size = icc_size;
    }
    if (vips_image_get_typeof(in, "psd-pixel-aspect-ratio") && !vips_image_get_double(in, "psd-pixel-aspect-ratio", &ratio)
        && ratio > 0.0)
        doc.pixel_aspect_ratio = ratio;
    minipsd_vips_psdsave_keep_blob(in, VIPS_META_EXIF_NAME, MINIPSD_RES_EXIF_DATA_1, resources, &count);
    minipsd_vips_psdsave_keep_blob(in, VIPS_META_XMP_NAME, MINIPSD_RES_XMP_METADATA, resources, &count);
    minipsd_vips_psdsave_keep_blob(in, VIPS_META_IPTC_NAME, MINIPSD_RES_IPTC_NAA, resources, &count);
    doc.resources      = resources;
    doc.resource_count = count;

    rc = failed ? MINIPSD_ERR_ALLOC : minipsd_write(&doc, NULL, minipsd_vips_psdsave_to_target, psd->target);
    for (k = 0; k <= PSD_MAX_PLANES; ++k) g_free(blocks[k]);
    if (rc != MINIPSD_OK) {
        vips_error(VIPS_OBJECT_GET_CLASS(object)->nickname, "%s",
                   failed ? "out of memory, or a row too long for a PSD's counts (save as PSB)" : minipsd_error_string(rc));
        return -1;
    }
    return vips_target_end(psd->target);
}

static void minipsd_vips_psdsave_class_init(MinipsdVipsPsdsaveClass *class)
{
    GObjectClass         *gobject_class = G_OBJECT_CLASS(class);
    VipsObjectClass      *object_class  = (VipsObjectClass *)class;
    VipsForeignSaveClass *save_class    = (VipsForeignSaveClass *)class;

    gobject_class->dispose = minipsd_vips_psdsave_dispose;
    minipsd_vips__class_properties(class);

    object_class->nickname    = "psdsave_base";
    object_class->description = "save PSD and PSB with minipsd";
    object_class->build       = minipsd_vips_psdsave_build;

    /* Any bands: the build takes grey, RGB and CMYK, with or without alpha, and refuses the rest. */
#if VIPS_MAJOR_VERSION > 8 || (VIPS_MAJOR_VERSION == 8 && VIPS_MINOR_VERSION >= 17)
    save_class->saveable = VIPS_FOREIGN_SAVEABLE_ANY;
#else
    save_class->saveable = VIPS_SAVEABLE_ANY;
#endif
    save_class->format_table = minipsd_vips_psdsave_save_formats;

    PSD_ARGS_BEGIN
    VIPS_ARG_BOOL(class, "psb", 20, "PSB", "Save as PSB (also for a .psb file, or a side past 30000)",
                  VIPS_ARGUMENT_OPTIONAL_INPUT, G_STRUCT_OFFSET(MinipsdVipsPsdsave, psb), FALSE);
    VIPS_ARG_ENUM(class, "compression", 21, "Compression", "The layer's compression: rle, zip or none (the composite too)",
                  VIPS_ARGUMENT_OPTIONAL_INPUT, G_STRUCT_OFFSET(MinipsdVipsPsdsave, compression),
                  minipsd_vips_psdsave_compression_get_type(), MINIPSD_VIPS_COMPRESSION_RLE);
    VIPS_ARG_STRING(class, "layer_name", 22, "Layer name", "The layer's name, UTF-8",
                    VIPS_ARGUMENT_OPTIONAL_INPUT, G_STRUCT_OFFSET(MinipsdVipsPsdsave, layer_name), PSD_LAYER_NAME);
    VIPS_ARG_BOOL(class, "layered", 23, "Layered", "Without alpha, a layer too, without transparency (else flat)",
                  VIPS_ARGUMENT_OPTIONAL_INPUT, G_STRUCT_OFFSET(MinipsdVipsPsdsave, layered), FALSE);
    PSD_ARGS_END
}

static void minipsd_vips_psdsave_init(MinipsdVipsPsdsave *psd)
{
    (void)psd;
}

/* --- psdsave_target ------------------------------------------------------------------------ */

typedef struct _MinipsdVipsPsdsaveTarget {
    MinipsdVipsPsdsave parent_object;
    VipsTarget        *target;
} MinipsdVipsPsdsaveTarget;

typedef MinipsdVipsPsdsaveClass MinipsdVipsPsdsaveTargetClass;

G_DEFINE_TYPE(MinipsdVipsPsdsaveTarget, minipsd_vips_psdsave_target, minipsd_vips_psdsave_get_type())

static int minipsd_vips_psdsave_target_build(VipsObject *object)
{
    MinipsdVipsPsdsave       *psd    = (MinipsdVipsPsdsave *)object;
    MinipsdVipsPsdsaveTarget *target = (MinipsdVipsPsdsaveTarget *)object;
    if (target->target) {
        psd->target = target->target;
        g_object_ref(psd->target);
    }
    return VIPS_OBJECT_CLASS(minipsd_vips_psdsave_target_parent_class)->build(object);
}

static void minipsd_vips_psdsave_target_class_init(MinipsdVipsPsdsaveTargetClass *class)
{
    VipsObjectClass  *object_class  = (VipsObjectClass *)class;
    VipsForeignClass *foreign_class = (VipsForeignClass *)class;

    minipsd_vips__class_properties(class);
    object_class->nickname    = "psdsave_target";
    object_class->description = "save PSD and PSB to a target with minipsd";
    object_class->build       = minipsd_vips_psdsave_target_build;
    foreign_class->suffs      = minipsd_vips__suffs;

    PSD_ARGS_BEGIN
    VIPS_ARG_OBJECT(class, "target", 1, "Target", "Target to save to", VIPS_ARGUMENT_REQUIRED_INPUT,
                    G_STRUCT_OFFSET(MinipsdVipsPsdsaveTarget, target), VIPS_TYPE_TARGET);
    PSD_ARGS_END
}

static void minipsd_vips_psdsave_target_init(MinipsdVipsPsdsaveTarget *target)
{
    (void)target;
}

/* --- psdsave (a file) ---------------------------------------------------------------------- */

typedef struct _MinipsdVipsPsdsaveFile {
    MinipsdVipsPsdsave parent_object;
    char              *filename;
} MinipsdVipsPsdsaveFile;

typedef MinipsdVipsPsdsaveClass MinipsdVipsPsdsaveFileClass;

G_DEFINE_TYPE(MinipsdVipsPsdsaveFile, minipsd_vips_psdsave_file, minipsd_vips_psdsave_get_type())

static int minipsd_vips_psdsave_file_build(VipsObject *object)
{
    MinipsdVipsPsdsave     *psd  = (MinipsdVipsPsdsave *)object;
    MinipsdVipsPsdsaveFile *file = (MinipsdVipsPsdsaveFile *)object;
    if (file->filename && !(psd->target = vips_target_new_to_file(file->filename))) return -1;
    return VIPS_OBJECT_CLASS(minipsd_vips_psdsave_file_parent_class)->build(object);
}

static void minipsd_vips_psdsave_file_class_init(MinipsdVipsPsdsaveFileClass *class)
{
    VipsObjectClass  *object_class  = (VipsObjectClass *)class;
    VipsForeignClass *foreign_class = (VipsForeignClass *)class;

    minipsd_vips__class_properties(class);
    object_class->nickname    = "psdsave";
    object_class->description = "save PSD and PSB to a file with minipsd";
    object_class->build       = minipsd_vips_psdsave_file_build;
    foreign_class->suffs      = minipsd_vips__suffs;

    PSD_ARGS_BEGIN
    VIPS_ARG_STRING(class, "filename", 1, "Filename", "Filename to save to", VIPS_ARGUMENT_REQUIRED_INPUT,
                    G_STRUCT_OFFSET(MinipsdVipsPsdsaveFile, filename), NULL);
    PSD_ARGS_END
}

static void minipsd_vips_psdsave_file_init(MinipsdVipsPsdsaveFile *file)
{
    (void)file;
}

/* --- psdsave_buffer ------------------------------------------------------------------------ */

typedef struct _MinipsdVipsPsdsaveBuffer {
    MinipsdVipsPsdsave parent_object;
    VipsArea          *buf;
} MinipsdVipsPsdsaveBuffer;

typedef MinipsdVipsPsdsaveClass MinipsdVipsPsdsaveBufferClass;

G_DEFINE_TYPE(MinipsdVipsPsdsaveBuffer, minipsd_vips_psdsave_buffer, minipsd_vips_psdsave_get_type())

static int minipsd_vips_psdsave_buffer_build(VipsObject *object)
{
    MinipsdVipsPsdsave *psd = (MinipsdVipsPsdsave *)object;
    VipsBlob           *blob;
    if (!(psd->target = vips_target_new_to_memory())) return -1;
    if (VIPS_OBJECT_CLASS(minipsd_vips_psdsave_buffer_parent_class)->build(object)) return -1;
    g_object_get(psd->target, "blob", &blob, NULL);
    g_object_set(object, "buffer", blob, NULL);
    vips_area_unref(VIPS_AREA(blob));
    return 0;
}

static void minipsd_vips_psdsave_buffer_class_init(MinipsdVipsPsdsaveBufferClass *class)
{
    VipsObjectClass  *object_class  = (VipsObjectClass *)class;
    VipsForeignClass *foreign_class = (VipsForeignClass *)class;

    minipsd_vips__class_properties(class);
    object_class->nickname    = "psdsave_buffer";
    object_class->description = "save PSD and PSB to a buffer with minipsd";
    object_class->build       = minipsd_vips_psdsave_buffer_build;
    foreign_class->suffs      = minipsd_vips__suffs;

    PSD_ARGS_BEGIN
    VIPS_ARG_BOXED(class, "buffer", 1, "Buffer", "Buffer to save to", VIPS_ARGUMENT_REQUIRED_OUTPUT,
                   G_STRUCT_OFFSET(MinipsdVipsPsdsaveBuffer, buf), VIPS_TYPE_BLOB);
    PSD_ARGS_END
}

static void minipsd_vips_psdsave_buffer_init(MinipsdVipsPsdsaveBuffer *buffer)
{
    (void)buffer;
}
