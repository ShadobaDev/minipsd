/**
 * \file
 * \brief minipsd as a libvips loader: a PSD or PSB as a libvips image.
 *
 * Laid out as libvips' own loaders are (a base class over a VipsSource; the file and buffer
 * variants make one), so the file can move into libvips/foreign/ as it is.  The file is mapped
 * (vips_source_map) and read in place by minipsd's reader; nothing is copied.  Pixels are decoded
 * in bands of rows as libvips asks for them, from its threads at once.
 *
 * The image is the composite as the file stores it, no colour converted: the mode's colour
 * channels, then its transparency.  What libvips has no interpretation for (indexed, Lab,
 * multichannel) is MULTIBAND, the colour mode data as "psd-color-mode-data".
 *
 * minipsd is compiled in here, private (MINIPSD_STATIC), so a program that uses minipsd itself
 * keeps its own.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_STATIC
#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include "minipsd_vips.h"

#include <vips/vips.h>

#include <limits.h>
#include <string.h>

#define PSD_SIGNATURE      "8BPS"
#define PSD_SIGNATURE_SIZE 4
#define PSD_SNIFF_SIZE     6      /* the signature and the version (1 PSD, 2 PSB), big-endian */
#define PSD_UNIT_CM        2      /* resolution info's hResUnit: pixels per cm, shown in cm */
#define MM_PER_INCH        25.4

/* libvips' VIPS_ARG_* macros pass vips_argument_get_id()'s int as a guint property id, and
   G_STRUCT_OFFSET's glong as a guint offset, which -Wsign-conversion and -Wconversion report in
   our code; only around them are they off. */
#if defined(__GNUC__)
#    define PSD_ARGS_BEGIN                                                                     \
        _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wsign-conversion\"") \
            _Pragma("GCC diagnostic ignored \"-Wconversion\"")
#    define PSD_ARGS_END   _Pragma("GCC diagnostic pop")
#else
#    define PSD_ARGS_BEGIN
#    define PSD_ARGS_END
#endif

/* --- The base class: a PSD read from a source --------------------------------------------- */

typedef struct _VipsForeignLoadPsd {
    VipsForeignLoad parent_object;
    VipsSource     *source;  /* set by the subclass's build */
    minipsd_reader *reader;
    minipsd_info    info;
    unsigned        bands;       /* the colour channels and transparency, up to MINIPSD_PLANES */
    double          xres, yres;  /* pixels per mm */
} VipsForeignLoadPsd;

typedef VipsForeignLoadClass VipsForeignLoadPsdClass;

GType vips_foreign_load_psd_get_type(void);
G_DEFINE_ABSTRACT_TYPE(VipsForeignLoadPsd, vips_foreign_load_psd, VIPS_TYPE_FOREIGN_LOAD)

/* Do these bytes start a PSD or a PSB? */
static int psd_is_a_bytes(const unsigned char *data, size_t size)
{
    return size >= PSD_SNIFF_SIZE && memcmp(data, PSD_SIGNATURE, PSD_SIGNATURE_SIZE) == 0 && data[4] == 0
        && (data[5] == MINIPSD_PSD || data[5] == MINIPSD_PSB);
}

static gboolean psd_is_a_source(VipsSource *source)
{
    const unsigned char *data = vips_source_sniff(source, PSD_SNIFF_SIZE);
    return data && psd_is_a_bytes(data, PSD_SNIFF_SIZE);
}

/* GObject resets set_property and get_property in every class (g_object_base_class_init), so
   each class with arguments of its own sets libvips' again. */
static void psd_class_properties(void *class)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(class);
    gobject_class->set_property = vips_object_set_property;
    gobject_class->get_property = vips_object_get_property;
}

static void vips_foreign_load_psd_dispose(GObject *gobject)
{
    VipsForeignLoadPsd *psd = (VipsForeignLoadPsd *)gobject;
    VIPS_UNREF(psd->source);
    G_OBJECT_CLASS(vips_foreign_load_psd_parent_class)->dispose(gobject);
}

static VipsForeignFlags vips_foreign_load_psd_get_flags(VipsForeignLoad *load)
{
    (void)load;
    return VIPS_FOREIGN_SEQUENTIAL;   /* bands of rows; libvips keeps the image for random access */
}

/* The bands' format: a sample of depth bits.  Bitmap's 1-bit samples come as bytes. */
static VipsBandFormat psd_format(const minipsd_info *info)
{
    return info->depth == 32 ? VIPS_FORMAT_FLOAT : info->depth == 16 ? VIPS_FORMAT_USHORT : VIPS_FORMAT_UCHAR;
}

/* What the samples are, as libvips names it; MULTIBAND where it has no name for the mode. */
static VipsInterpretation psd_interpretation(const minipsd_info *info)
{
    switch (info->color_mode) {
    case MINIPSD_MODE_RGB:
        return info->depth == 32 ? VIPS_INTERPRETATION_scRGB
             : info->depth == 16 ? VIPS_INTERPRETATION_RGB16 : VIPS_INTERPRETATION_sRGB;
    case MINIPSD_MODE_BITMAP:
    case MINIPSD_MODE_GREY:
    case MINIPSD_MODE_DUOTONE:
        return info->depth == 16 ? VIPS_INTERPRETATION_GREY16 : VIPS_INTERPRETATION_B_W;
    case MINIPSD_MODE_CMYK:
        return VIPS_INTERPRETATION_CMYK;
    default:
        return VIPS_INTERPRETATION_MULTIBAND;
    }
}

/* The image's fields: on out, and on the image load() generates. */
static int psd_fields(VipsForeignLoadPsd *psd, VipsImage *image)
{
    vips_image_init_fields(image, (int)psd->info.width, (int)psd->info.height, (int)psd->bands, psd_format(&psd->info),
                           VIPS_CODING_NONE, psd_interpretation(&psd->info), psd->xres, psd->yres);
    return vips_image_pipelinev(image, VIPS_DEMAND_STYLE_THINSTRIP, NULL);
}

/* A resource's bytes as a blob of the image, when the file has it. */
static void psd_set_blob(VipsForeignLoadPsd *psd, uint16_t id, const char *name)
{
    minipsd_resource res;
    if (minipsd_reader_find_resource(psd->reader, id, &res) == MINIPSD_OK && res.data.size)
        vips_image_set_blob_copy(((VipsForeignLoad *)psd)->out, name, res.data.data, res.data.size);
}

static int vips_foreign_load_psd_header(VipsForeignLoad *load)
{
    VipsForeignLoadPsd     *psd   = (VipsForeignLoadPsd *)load;
    VipsObjectClass        *class = VIPS_OBJECT_GET_CLASS(load);
    minipsd_resource        res;
    minipsd_resolution_info resolution;
    minipsd_version_info    version;
    double                  xres = psd->info.dpi, yres;
    const char             *unit = "in";
    const void             *data;
    void                   *workspace;
    size_t                  size, workspace_size;
    int                     rc;

    if (!(data = vips_source_map(psd->source, &size))) return -1;
    if (minipsd_reader_workspace_size(&workspace_size) != MINIPSD_OK
        || !(workspace = vips_malloc(VIPS_OBJECT(load), workspace_size)))
        return -1;
    rc = minipsd_reader_init(&psd->reader, data, size, workspace, workspace_size, NULL, &psd->info);
    if (rc != MINIPSD_OK) {
        vips_error(class->nickname, "%s", minipsd_error_string(rc));
        return -1;
    }

    /* The resolution: resource 1005 whole, or none (libvips' default). */
    yres = xres;
    if (minipsd_reader_find_resource(psd->reader, MINIPSD_RES_RESOLUTION_INFO, &res) == MINIPSD_OK
        && minipsd_resource_resolution_info(&res, &resolution) == MINIPSD_OK) {
        xres = resolution.horizontal_resolution;
        yres = resolution.vertical_resolution;
        if (resolution.horizontal_resolution_unit == PSD_UNIT_CM) unit = "cm";
    }

    /* Channels past MINIPSD_PLANES (a multichannel file's sixth on) are not decoded. */
    psd->bands = VIPS_MIN(psd->info.color_channels + (psd->info.composite_has_alpha ? 1u : 0u), MINIPSD_PLANES);
    psd->xres  = xres > 0.0 ? xres / MM_PER_INCH : 1.0;
    psd->yres  = yres > 0.0 ? yres / MM_PER_INCH : 1.0;
    if (psd_fields(psd, load->out)) return -1;
    if (xres > 0.0) vips_image_set_string(load->out, VIPS_META_RESOLUTION_UNIT, unit);

    if (psd->info.icc.size)
        vips_image_set_blob_copy(load->out, VIPS_META_ICC_NAME, psd->info.icc.data, psd->info.icc.size);
    psd_set_blob(psd, MINIPSD_RES_EXIF_DATA_1, VIPS_META_EXIF_NAME);
    psd_set_blob(psd, MINIPSD_RES_XMP_METADATA, VIPS_META_XMP_NAME);
    psd_set_blob(psd, MINIPSD_RES_IPTC_NAA, VIPS_META_IPTC_NAME);
    if (psd->info.color_mode_data.size)
        vips_image_set_blob_copy(load->out, "psd-color-mode-data", psd->info.color_mode_data.data,
                                 psd->info.color_mode_data.size);
    vips_image_set_double(load->out, "psd-pixel-aspect-ratio", psd->info.pixel_aspect_ratio);
    /* Without version info (before Photoshop 6) the composite is real. */
    vips_image_set_int(load->out, "psd-has-real-merged-data",
                       minipsd_reader_find_resource(psd->reader, MINIPSD_RES_VERSION_INFO, &res) != MINIPSD_OK
                           || minipsd_resource_version_info(&res, &version) != MINIPSD_OK
                           || version.has_real_merged_data);
    return 0;
}

/* One band of rows, decoded straight into the region.  vips_sequential asks for whole rows; calls
   run at once from libvips' threads on the one reader, which minipsd allows on a whole file. */
static int psd_generate(VipsRegion *region, void *seq, void *a, void *b, gboolean *stop)
{
    VipsForeignLoadPsd *psd  = (VipsForeignLoadPsd *)a;
    const VipsRect     *rect = &region->valid;
    minipsd_decode_opts opts;
    minipsd_target      target;
    VipsPel            *top  = VIPS_REGION_ADDR_TOPLEFT(region), *row;
    const size_t        skip = VIPS_REGION_LSKIP(region);
    int                 rc, x, y;
    (void)seq, (void)b, (void)stop;

    if (rect->left != 0 || rect->width != region->im->Xsize) {
        vips_error(VIPS_OBJECT_GET_CLASS(psd)->nickname, "%s", "only whole rows are decoded");
        return -1;
    }
    memset(&opts, 0, sizeof opts);
    opts.y_first = (uint32_t)rect->top;
    opts.y_count = (uint32_t)rect->height;
    opts.depth   = psd->info.depth;
    minipsd_target_interleaved(&target, top, skip, psd->bands, psd->info.depth);
    rc = minipsd_decode_composite(psd->reader, &target, &opts);
    if (rc != MINIPSD_OK) {
        vips_error(VIPS_OBJECT_GET_CLASS(psd)->nickname, "%s", minipsd_error_string(rc));
        return -1;
    }

    /* Bitmap decodes to 1 for black, 0 for white; libvips' B_W is 0 black, 255 white. */
    if (psd->info.color_mode == MINIPSD_MODE_BITMAP)
        for (y = 0; y < rect->height; ++y)
            for (row = top + (size_t)y * skip, x = 0; x < rect->width; ++x)
                row[x] = row[x] ? 0 : UCHAR_MAX;
    return 0;
}

static int vips_foreign_load_psd_load(VipsForeignLoad *load)
{
    VipsForeignLoadPsd *psd = (VipsForeignLoadPsd *)load;
    VipsImage         **t   = (VipsImage **)vips_object_local_array(VIPS_OBJECT(load), 2);
    t[0] = vips_image_new();
    if (psd_fields(psd, t[0]) || vips_image_generate(t[0], NULL, psd_generate, NULL, psd, NULL)
        || vips_sequential(t[0], &t[1], NULL) || vips_image_write(t[1], load->real))
        return -1;
    return 0;
}

static void vips_foreign_load_psd_class_init(VipsForeignLoadPsdClass *class)
{
    GObjectClass         *gobject_class = G_OBJECT_CLASS(class);
    VipsObjectClass      *object_class  = (VipsObjectClass *)class;
    VipsForeignLoadClass *load_class    = (VipsForeignLoadClass *)class;

    gobject_class->dispose = vips_foreign_load_psd_dispose;
    psd_class_properties(class);

    object_class->nickname    = "psdload_base";
    object_class->description = "load PSD and PSB with minipsd";

    load_class->get_flags = vips_foreign_load_psd_get_flags;
    load_class->header    = vips_foreign_load_psd_header;
    load_class->load      = vips_foreign_load_psd_load;
}

static void vips_foreign_load_psd_init(VipsForeignLoadPsd *psd)
{
    (void)psd;
}

static const char *psd_suffs[] = { ".psd", ".psb", NULL };

/* --- psdload_source ----------------------------------------------------------------------- */

typedef struct _VipsForeignLoadPsdSource {
    VipsForeignLoadPsd parent_object;
    VipsSource        *source;
} VipsForeignLoadPsdSource;

typedef VipsForeignLoadPsdClass VipsForeignLoadPsdSourceClass;

GType vips_foreign_load_psd_source_get_type(void);
G_DEFINE_TYPE(VipsForeignLoadPsdSource, vips_foreign_load_psd_source, vips_foreign_load_psd_get_type())

static int vips_foreign_load_psd_source_build(VipsObject *object)
{
    VipsForeignLoadPsd       *psd    = (VipsForeignLoadPsd *)object;
    VipsForeignLoadPsdSource *source = (VipsForeignLoadPsdSource *)object;
    if (source->source) {
        psd->source = source->source;
        g_object_ref(psd->source);
    }
    return VIPS_OBJECT_CLASS(vips_foreign_load_psd_source_parent_class)->build(object);
}

static void vips_foreign_load_psd_source_class_init(VipsForeignLoadPsdSourceClass *class)
{
    VipsObjectClass      *object_class    = (VipsObjectClass *)class;
    VipsOperationClass   *operation_class = VIPS_OPERATION_CLASS(class);
    VipsForeignLoadClass *load_class      = (VipsForeignLoadClass *)class;

    psd_class_properties(class);
    object_class->nickname    = "psdload_source";
    object_class->description = "load PSD and PSB from a source with minipsd";
    object_class->build       = vips_foreign_load_psd_source_build;
    operation_class->flags |= VIPS_OPERATION_NOCACHE;
    load_class->is_a_source = psd_is_a_source;

    PSD_ARGS_BEGIN
    VIPS_ARG_OBJECT(class, "source", 1, "Source", "Source to load from", VIPS_ARGUMENT_REQUIRED_INPUT,
                    G_STRUCT_OFFSET(VipsForeignLoadPsdSource, source), VIPS_TYPE_SOURCE);
    PSD_ARGS_END
}

static void vips_foreign_load_psd_source_init(VipsForeignLoadPsdSource *source)
{
    (void)source;
}

/* --- psdload (a file) ---------------------------------------------------------------------- */

typedef struct _VipsForeignLoadPsdFile {
    VipsForeignLoadPsd parent_object;
    char              *filename;
} VipsForeignLoadPsdFile;

typedef VipsForeignLoadPsdClass VipsForeignLoadPsdFileClass;

GType vips_foreign_load_psd_file_get_type(void);
G_DEFINE_TYPE(VipsForeignLoadPsdFile, vips_foreign_load_psd_file, vips_foreign_load_psd_get_type())

static gboolean psd_is_a_file(const char *filename)
{
    VipsSource *source = vips_source_new_from_file(filename);
    gboolean    is_a;
    if (!source) return FALSE;
    is_a = psd_is_a_source(source);
    g_object_unref(source);
    return is_a;
}

static int vips_foreign_load_psd_file_build(VipsObject *object)
{
    VipsForeignLoadPsd     *psd  = (VipsForeignLoadPsd *)object;
    VipsForeignLoadPsdFile *file = (VipsForeignLoadPsdFile *)object;
    if (file->filename && !(psd->source = vips_source_new_from_file(file->filename))) return -1;
    return VIPS_OBJECT_CLASS(vips_foreign_load_psd_file_parent_class)->build(object);
}

static void vips_foreign_load_psd_file_class_init(VipsForeignLoadPsdFileClass *class)
{
    VipsObjectClass      *object_class  = (VipsObjectClass *)class;
    VipsForeignClass     *foreign_class = (VipsForeignClass *)class;
    VipsForeignLoadClass *load_class    = (VipsForeignLoadClass *)class;

    psd_class_properties(class);
    object_class->nickname    = "psdload";
    object_class->description = "load PSD and PSB with minipsd";
    object_class->build       = vips_foreign_load_psd_file_build;
    foreign_class->suffs      = psd_suffs;
    load_class->is_a          = psd_is_a_file;

    PSD_ARGS_BEGIN
    VIPS_ARG_STRING(class, "filename", 1, "Filename", "Filename to load from", VIPS_ARGUMENT_REQUIRED_INPUT,
                    G_STRUCT_OFFSET(VipsForeignLoadPsdFile, filename), NULL);
    PSD_ARGS_END
}

static void vips_foreign_load_psd_file_init(VipsForeignLoadPsdFile *file)
{
    (void)file;
}

/* --- psdload_buffer ------------------------------------------------------------------------ */

typedef struct _VipsForeignLoadPsdBuffer {
    VipsForeignLoadPsd parent_object;
    VipsBlob          *blob;
} VipsForeignLoadPsdBuffer;

typedef VipsForeignLoadPsdClass VipsForeignLoadPsdBufferClass;

GType vips_foreign_load_psd_buffer_get_type(void);
G_DEFINE_TYPE(VipsForeignLoadPsdBuffer, vips_foreign_load_psd_buffer, vips_foreign_load_psd_get_type())

static gboolean psd_is_a_buffer(const void *data, size_t size)
{
    return psd_is_a_bytes((const unsigned char *)data, size);
}

static int vips_foreign_load_psd_buffer_build(VipsObject *object)
{
    VipsForeignLoadPsd       *psd    = (VipsForeignLoadPsd *)object;
    VipsForeignLoadPsdBuffer *buffer = (VipsForeignLoadPsdBuffer *)object;
    if (buffer->blob && !(psd->source = vips_source_new_from_blob(buffer->blob))) return -1;
    return VIPS_OBJECT_CLASS(vips_foreign_load_psd_buffer_parent_class)->build(object);
}

static void vips_foreign_load_psd_buffer_class_init(VipsForeignLoadPsdBufferClass *class)
{
    VipsObjectClass      *object_class = (VipsObjectClass *)class;
    VipsForeignLoadClass *load_class   = (VipsForeignLoadClass *)class;

    psd_class_properties(class);
    object_class->nickname    = "psdload_buffer";
    object_class->description = "load PSD and PSB from a buffer with minipsd";
    object_class->build       = vips_foreign_load_psd_buffer_build;
    load_class->is_a_buffer   = psd_is_a_buffer;

    PSD_ARGS_BEGIN
    VIPS_ARG_BOXED(class, "buffer", 1, "Buffer", "Buffer to load from", VIPS_ARGUMENT_REQUIRED_INPUT,
                   G_STRUCT_OFFSET(VipsForeignLoadPsdBuffer, blob), VIPS_TYPE_BLOB);
    PSD_ARGS_END
}

static void vips_foreign_load_psd_buffer_init(VipsForeignLoadPsdBuffer *buffer)
{
    (void)buffer;
}

/* --- Registration -------------------------------------------------------------------------- */

int minipsd_vips_init(void)
{
    /* Registering a type is enough: libvips finds loaders among VipsForeignLoad's subclasses. */
    (void)vips_foreign_load_psd_file_get_type();
    (void)vips_foreign_load_psd_buffer_get_type();
    (void)vips_foreign_load_psd_source_get_type();
    return 0;
}
