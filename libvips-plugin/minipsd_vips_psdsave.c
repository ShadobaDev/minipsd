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
 * profile, EXIF, XMP and IPTC kept.  The image is held whole in memory while it is written.
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

typedef struct _MinipsdVipsPsdsave {
    VipsForeignSave parent_object;
    VipsTarget     *target;      /* set by the subclass's build */
    gboolean        psb;         /* the option; also a .psb file, or a side past PSD's */
    gboolean        zip;         /* the option: ZIP the layer's channels */
    char           *layer_name;  /* the option */
    gboolean        layered;     /* the option: a layer also for an image without alpha */
} MinipsdVipsPsdsave;

typedef VipsForeignSaveClass MinipsdVipsPsdsaveClass;

G_DEFINE_ABSTRACT_TYPE(MinipsdVipsPsdsave, minipsd_vips_psdsave, VIPS_TYPE_FOREIGN_SAVE)

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

static int minipsd_vips_psdsave_deflate(void *user, const uint8_t *src, size_t src_size, uint8_t *dst, size_t *dst_size)
{
    uLongf n = (uLongf)*dst_size;
    int    rc;
    (void)user;
    if ((uLong)src_size != src_size || n != *dst_size) return 1;   /* past zlib's uLong: 32 bits on Windows */
    rc        = compress2(dst, &n, src, (uLong)src_size, PSD_ZIP_LEVEL);
    *dst_size = n;
    return rc != Z_OK;
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

static int minipsd_vips_psdsave_build(VipsObject *object)
{
    VipsForeignSave    *save = (VipsForeignSave *)object;
    MinipsdVipsPsdsave *psd  = (MinipsdVipsPsdsave *)object;
    VipsImage          *in;
    minipsd_resource    resources[PSD_MAX_RESOURCES];
    minipsd_layer       layer;
    minipsd_doc         doc;
    minipsd_write_opts  opts;
    const void         *icc;
    const char         *filename;
    size_t              icc_size, bytes, colors, count = 0;
    double              ratio;
    void               *pixels;
    int                 rc, alpha;

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
    doc.depth = in->BandFmt == VIPS_FORMAT_FLOAT ? 32 : in->BandFmt == VIPS_FORMAT_USHORT ? 16 : 8;

    /* Transparency as the image has it: none, none added. */
    alpha = (size_t)in->Bands > colors;
    if (!(pixels = vips_image_write_to_memory(in, &bytes))) return -1;
    if (doc.color_mode == MINIPSD_MODE_CMYK) minipsd_vips__invert_cmyk(pixels, (size_t)in->Xsize * (size_t)in->Ysize, (unsigned)in->Bands, doc.depth);

    memset(&layer, 0, sizeof layer);
    layer.name     = psd->layer_name ? psd->layer_name : PSD_LAYER_NAME;
    layer.pixels   = (const uint8_t *)pixels;
    layer.opacity  = UCHAR_MAX;
    layer.no_alpha = (uint8_t)!alpha;

    doc.width       = (uint32_t)in->Xsize;
    doc.height      = (uint32_t)in->Ysize;
    doc.dpi         = in->Xres * MM_PER_INCH;
    if (!(doc.dpi > 0.0 && doc.dpi < PSD_MAX_DPI)) doc.dpi = PSD_DEFAULT_DPI;
    doc.layers      = alpha || psd->layered ? &layer : NULL;   /* without alpha: flat, unless asked */
    doc.layer_count = doc.layers ? 1 : 0;
    doc.composite   = (const uint8_t *)pixels;   /* one layer, at full opacity: the composite itself */
    doc.composite_alpha = (uint8_t)alpha;
    doc.compression = psd->zip ? MINIPSD_ZIP : MINIPSD_RLE;
    /* PSB by the option, a file named .psb (libvips writes files through a target too), or a side
       past PSD's. */
    filename   = vips_connection_filename(VIPS_CONNECTION(psd->target));
    doc.format = psd->psb || (filename && vips_iscasepostfix(filename, ".psb")) || in->Xsize > MINIPSD_MAX_SIDE
                      || in->Ysize > MINIPSD_MAX_SIDE
                   ? MINIPSD_PSB
                   : MINIPSD_PSD;
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

    memset(&opts, 0, sizeof opts);
    opts.deflate = minipsd_vips_psdsave_deflate;
    rc = minipsd_write(&doc, &opts, minipsd_vips_psdsave_to_target, psd->target);
    g_free(pixels);
    if (rc != MINIPSD_OK) {
        vips_error(VIPS_OBJECT_GET_CLASS(object)->nickname, "%s", minipsd_error_string(rc));
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
    VIPS_ARG_BOOL(class, "zip", 21, "ZIP", "Compress the layer with ZIP instead of RLE",
                  VIPS_ARGUMENT_OPTIONAL_INPUT, G_STRUCT_OFFSET(MinipsdVipsPsdsave, zip), FALSE);
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
