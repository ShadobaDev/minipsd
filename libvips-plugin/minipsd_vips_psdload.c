/**
 * \file
 * \brief psdload, psdload_buffer, psdload_source: a PSD or PSB as a libvips image.
 *
 * Laid out as libvips' own loaders are (a base class over a VipsSource; the file and buffer
 * variants make one), so the code can move into libvips/foreign/ (as psdload.c, the minipsd_vips_
 * names back to vips_foreign_).  The file is mapped (vips_source_map) and read in place by
 * minipsd's reader; nothing is copied.
 * Pixels are decoded in bands of rows as libvips asks for them, from its threads at once.
 *
 * The image is the composite, or with `layer` one layer (its position in xoffset and yoffset), as
 * the file stores it, no colour converted: the mode's colour channels, then transparency where it
 * has one (a layer's channel, the composite's).  What libvips has no interpretation for (Lab,
 * multichannel) is MULTIBAND; the colour mode data is kept as "psd-color-mode-data".  Indexed is
 * the exception: libvips has no indexed image, so it comes through its palette as sRGB, as
 * libvips' own loaders give PNG, GIF and TIFF palettes (a lookup, no colour converted).  CMYK
 * comes the other way round from PSD's inverted ink, and bitmap as B_W (black 0), as libvips
 * holds them.  ZIP layers are inflated by the zlib libvips links.
 *
 * minipsd's reader is compiled in here, private (MINIPSD_STATIC; the writer left out), so a
 * program that uses minipsd itself keeps its own.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_STATIC
#define MINIPSD_NO_WRITER
#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include "minipsd_vips_psdload.h"
#include "minipsd_vips_util.h"

#include <zlib.h>

#include <limits.h>
#include <string.h>

#define PSD_SIGNATURE        "8BPS"
#define PSD_SIGNATURE_SIZE   4
#define PSD_SNIFF_SIZE       6    /* the signature and the version (1 PSD, 2 PSB), big-endian */
#define PSD_UNIT_CM          2    /* resolution info's hResUnit: pixels per cm, shown in cm */
#define PSD_STORED_ZIP       2    /* a channel's stored compression marker: ZIP, and ... */
#define PSD_STORED_ZIP_PRED  3    /* ... ZIP with prediction */
#define PSD_PALETTE_ENTRIES  256  /* an indexed file's palette: 256 reds, 256 greens, 256 blues */
#define PSD_COMPOSITE        (-1) /* the `layer` option's default: the composite */

/* --- The base class: a PSD read from a source --------------------------------------------- */

typedef struct _MinipsdVipsPsdload {
    VipsForeignLoad    parent_object;
    int                layer;          /* the option: -1 the composite, else a layer, bottom first */
    VipsSource        *source;         /* set by the subclass's build */
    minipsd_reader    *reader;
    minipsd_info       info;
    minipsd_layer_view view;           /* the layer, with layer >= 0 */
    uint32_t           width, height;  /* the image: the canvas, or the layer's rectangle */
    unsigned           planes;         /* what minipsd decodes: colour channels and transparency,
                                          up to MINIPSD_PLANES */
    unsigned           bands;          /* what libvips gets: planes, or indexed's RGB(A) */
    int                indexed;        /* the palette makes the colour */
    int                transparent;    /* indexed: the transparent index (resource 1047), or -1 */
    size_t             scratch_size;   /* a ZIP layer's channels inflated whole; 0 without ZIP */
    double             xres, yres;     /* pixels per mm */
} MinipsdVipsPsdload;

typedef VipsForeignLoadClass MinipsdVipsPsdloadClass;

G_DEFINE_ABSTRACT_TYPE(MinipsdVipsPsdload, minipsd_vips_psdload, VIPS_TYPE_FOREIGN_LOAD)

/* Do these bytes start a PSD or a PSB? */
static int minipsd_vips_psdload_is_a_bytes(const unsigned char *data, size_t size)
{
    return size >= PSD_SNIFF_SIZE && memcmp(data, PSD_SIGNATURE, PSD_SIGNATURE_SIZE) == 0 && data[4] == 0
        && (data[5] == MINIPSD_PSD || data[5] == MINIPSD_PSB);
}

static gboolean minipsd_vips_psdload_is_a_source(VipsSource *source)
{
    const unsigned char *data = vips_source_sniff(source, PSD_SNIFF_SIZE);
    return data && minipsd_vips_psdload_is_a_bytes(data, PSD_SNIFF_SIZE);
}

static void minipsd_vips_psdload_dispose(GObject *gobject)
{
    MinipsdVipsPsdload *psd = (MinipsdVipsPsdload *)gobject;
    VIPS_UNREF(psd->source);
    G_OBJECT_CLASS(minipsd_vips_psdload_parent_class)->dispose(gobject);
}

static VipsForeignFlags minipsd_vips_psdload_get_flags(VipsForeignLoad *load)
{
    (void)load;
    return VIPS_FOREIGN_SEQUENTIAL;   /* bands of rows; libvips keeps the image for random access */
}

static void minipsd_vips_psdload_error(MinipsdVipsPsdload *psd, const char *message)
{
    vips_error(VIPS_OBJECT_GET_CLASS(psd)->nickname, "%s", message);
}

/* The bands' format: a sample of depth bits.  Bitmap's 1-bit samples come as bytes. */
static VipsBandFormat minipsd_vips_psdload_format(const minipsd_info *info)
{
    return info->depth == 32 ? VIPS_FORMAT_FLOAT : info->depth == 16 ? VIPS_FORMAT_USHORT : VIPS_FORMAT_UCHAR;
}

/* What the samples are, as libvips names it; MULTIBAND where it has no name for the mode. */
static VipsInterpretation minipsd_vips_psdload_interpretation(const minipsd_info *info)
{
    switch (info->color_mode) {
    case MINIPSD_MODE_RGB:
        return info->depth == 32 ? VIPS_INTERPRETATION_scRGB
             : info->depth == 16 ? VIPS_INTERPRETATION_RGB16 : VIPS_INTERPRETATION_sRGB;
    case MINIPSD_MODE_INDEXED:
        return VIPS_INTERPRETATION_sRGB;
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
static int minipsd_vips_psdload_fields(MinipsdVipsPsdload *psd, VipsImage *image)
{
    vips_image_init_fields(image, (int)psd->width, (int)psd->height, (int)psd->bands,
                           minipsd_vips_psdload_format(&psd->info), VIPS_CODING_NONE,
                           minipsd_vips_psdload_interpretation(&psd->info), psd->xres, psd->yres);
    if (psd->layer >= 0) {
        image->Xoffset = psd->view.left;
        image->Yoffset = psd->view.top;
    }
    return vips_image_pipelinev(image, VIPS_DEMAND_STYLE_THINSTRIP, NULL);
}

/* A resource's bytes as a blob of the image, when the file has it. */
static void minipsd_vips_psdload_set_blob(MinipsdVipsPsdload *psd, uint16_t id, const char *name)
{
    minipsd_resource res;
    if (minipsd_reader_find_resource(psd->reader, id, &res) == MINIPSD_OK && res.data.size)
        vips_image_set_blob_copy(((VipsForeignLoad *)psd)->out, name, res.data.data, res.data.size);
}

/* Bytes to inflate the layer's ZIP channels whole, each in its own part (scratch_kept); 0 when
   none of the planes is ZIP. */
static size_t minipsd_vips_psdload_scratch_size(const MinipsdVipsPsdload *psd)
{
    const size_t row = ((size_t)psd->width * psd->info.depth + 7) / 8;
    unsigned     i;
    int          zip = 0;
    for (i = 0; i < psd->planes; ++i)
        if (psd->view.channel[i].size >= 2 && psd->view.channel[i].data && psd->view.channel[i].data[0] == 0
            && (psd->view.channel[i].data[1] == PSD_STORED_ZIP || psd->view.channel[i].data[1] == PSD_STORED_ZIP_PRED))
            zip = 1;
    return zip ? psd->planes * psd->height * row : 0;
}

/* The layer the option names: its record, its size, its name and look as metadata. */
static int minipsd_vips_psdload_find_layer(MinipsdVipsPsdload *psd)
{
    VipsImage *out = ((VipsForeignLoad *)psd)->out;
    char      *name;
    size_t     needed = 0;
    int        rc, i;
    for (i = 0; i <= psd->layer; ++i)
        if ((rc = minipsd_reader_next_layer(psd->reader, &psd->view)) != MINIPSD_MORE) {
            minipsd_vips_psdload_error(psd, rc == MINIPSD_OK ? "no such layer" : minipsd_error_string(rc));
            return -1;
        }
    if (psd->view.right <= psd->view.left || psd->view.bottom <= psd->view.top) {
        minipsd_vips_psdload_error(psd, "the layer is empty");
        return -1;
    }
    psd->width  = (uint32_t)(psd->view.right - psd->view.left);
    psd->height = (uint32_t)(psd->view.bottom - psd->view.top);
    /* Transparency when the layer has its channel: a layer without one comes without alpha, as it
       was saved. */
    psd->planes = psd->info.color_channels;
    if (psd->planes < MINIPSD_PLANES && psd->view.channel[psd->planes].size) ++psd->planes;

    if (minipsd_layer_name(&psd->view, NULL, 0, &needed) == MINIPSD_MORE
        && (name = (char *)vips_malloc(VIPS_OBJECT(psd), needed))
        && minipsd_layer_name(&psd->view, name, needed, &needed) == MINIPSD_OK)
        vips_image_set_string(out, "psd-layer-name", name);
    vips_image_set_string(out, "psd-layer-blend-mode", psd->view.blend_mode);
    vips_image_set_int(out, "psd-layer-opacity", psd->view.opacity);
    vips_image_set_int(out, "psd-layer-hidden", psd->view.hidden);
    return 0;
}

static int minipsd_vips_psdload_header(VipsForeignLoad *load)
{
    MinipsdVipsPsdload     *psd = (MinipsdVipsPsdload *)load;
    minipsd_resource        res;
    minipsd_resolution_info resolution;
    minipsd_version_info    version;
    uint16_t                index;
    double                  xres, yres;
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
        minipsd_vips_psdload_error(psd, minipsd_error_string(rc));
        return -1;
    }

    psd->width  = psd->info.width;
    psd->height = psd->info.height;
    /* Channels past MINIPSD_PLANES (a multichannel file's sixth on) are not decoded. */
    psd->planes = VIPS_MIN(psd->info.color_channels + (psd->info.composite_has_alpha ? 1u : 0u), MINIPSD_PLANES);
    if (psd->layer >= 0 && minipsd_vips_psdload_find_layer(psd)) return -1;
    psd->scratch_size = psd->layer >= 0 ? minipsd_vips_psdload_scratch_size(psd) : 0;

    /* Indexed: the palette's RGB, and transparency where resource 1047 names an index. */
    psd->bands       = psd->planes;
    psd->transparent = -1;
    psd->indexed     = psd->info.color_mode == MINIPSD_MODE_INDEXED;
    if (psd->indexed) {
        if (psd->info.color_mode_data.size < PSD_RGB * PSD_PALETTE_ENTRIES || !psd->info.color_mode_data.data) {
            minipsd_vips_psdload_error(psd, "an indexed file without its palette");
            return -1;
        }
        if (minipsd_reader_find_resource(psd->reader, MINIPSD_RES_TRANSPARENCY_INDEX, &res) == MINIPSD_OK
            && minipsd_resource_transparency_index(&res, &index) == MINIPSD_OK && index < PSD_PALETTE_ENTRIES)
            psd->transparent = index;
        psd->bands = PSD_RGB + (psd->transparent >= 0 ? 1u : 0u);
    }

    /* The resolution: resource 1005 whole, or none (libvips' default). */
    xres = yres = psd->info.dpi;
    if (minipsd_reader_find_resource(psd->reader, MINIPSD_RES_RESOLUTION_INFO, &res) == MINIPSD_OK
        && minipsd_resource_resolution_info(&res, &resolution) == MINIPSD_OK) {
        xres = resolution.horizontal_resolution;
        yres = resolution.vertical_resolution;
        if (resolution.horizontal_resolution_unit == PSD_UNIT_CM) unit = "cm";
    }
    psd->xres = xres > 0.0 ? xres / MM_PER_INCH : 1.0;
    psd->yres = yres > 0.0 ? yres / MM_PER_INCH : 1.0;
    if (minipsd_vips_psdload_fields(psd, load->out)) return -1;
    if (xres > 0.0) vips_image_set_string(load->out, VIPS_META_RESOLUTION_UNIT, unit);

    vips_image_set_int(load->out, VIPS_META_BITS_PER_SAMPLE, psd->info.depth);
    if (psd->info.icc.size)
        vips_image_set_blob_copy(load->out, VIPS_META_ICC_NAME, psd->info.icc.data, psd->info.icc.size);
    minipsd_vips_psdload_set_blob(psd, MINIPSD_RES_EXIF_DATA_1, VIPS_META_EXIF_NAME);
    minipsd_vips_psdload_set_blob(psd, MINIPSD_RES_XMP_METADATA, VIPS_META_XMP_NAME);
    minipsd_vips_psdload_set_blob(psd, MINIPSD_RES_IPTC_NAA, VIPS_META_IPTC_NAME);
    if (psd->info.color_mode_data.size)
        vips_image_set_blob_copy(load->out, "psd-color-mode-data", psd->info.color_mode_data.data,
                                 psd->info.color_mode_data.size);
    vips_image_set_double(load->out, "psd-pixel-aspect-ratio", psd->info.pixel_aspect_ratio);
    vips_image_set_int(load->out, "psd-layer-count", (int)psd->info.layer_count);
    /* Without version info (before Photoshop 6) the composite is real. */
    vips_image_set_int(load->out, "psd-has-real-merged-data",
                       minipsd_reader_find_resource(psd->reader, MINIPSD_RES_VERSION_INFO, &res) != MINIPSD_OK
                           || minipsd_resource_version_info(&res, &version) != MINIPSD_OK
                           || version.has_real_merged_data);
    return 0;
}

/* zlib's uncompress, as minipsd_inflate_fn: the stream must fill dst exactly. */
static int minipsd_vips_psdload_inflate(void *user, const uint8_t *src, size_t src_size, uint8_t *dst, size_t dst_size)
{
    uLongf n = (uLongf)dst_size;
    (void)user;
    if ((uLong)src_size != src_size || n != dst_size) return 1;   /* past zlib's uLong: 32 bits on Windows */
    return uncompress(dst, &n, src, (uLong)src_size) != Z_OK || n != dst_size;
}

/* A thread's sequence: the scratch a ZIP layer's channels stay inflated in, between its bands. */
typedef struct minipsd_vips_psdload_sequence {
    uint8_t *scratch;
    int      kept;
} minipsd_vips_psdload_sequence;

static void *minipsd_vips_psdload_start(VipsImage *out, void *a, void *b)
{
    MinipsdVipsPsdload            *psd = (MinipsdVipsPsdload *)a;
    minipsd_vips_psdload_sequence *seq = g_new0(minipsd_vips_psdload_sequence, 1);
    (void)out, (void)b;
    /* ponytail: every thread inflates a ZIP layer whole into its own scratch; one shared scratch
       under a lock would bound memory at one copy, if layers that large turn up. */
    if (psd->scratch_size && !(seq->scratch = (uint8_t *)g_try_malloc(psd->scratch_size))) {
        g_free(seq);
        return NULL;
    }
    return seq;
}

static int minipsd_vips_psdload_stop(void *seq, void *a, void *b)
{
    (void)a, (void)b;
    g_free(((minipsd_vips_psdload_sequence *)seq)->scratch);
    g_free(seq);
    return 0;
}

/* Indexed, in place: each pixel's index (in its first band) becomes the palette's RGB, and alpha
   0 at the transparent index. */
static void minipsd_vips_psdload_expand_palette(const MinipsdVipsPsdload *psd, VipsPel *row, int width)
{
    const uint8_t *palette = psd->info.color_mode_data.data;
    int            x;
    for (x = 0; x < width; ++x, row += psd->bands) {
        const unsigned index = row[0];
        row[0] = palette[index];
        row[1] = palette[PSD_PALETTE_ENTRIES + index];
        row[2] = palette[2 * PSD_PALETTE_ENTRIES + index];
        if (psd->transparent >= 0) row[PSD_RGB] = (int)index == psd->transparent ? 0 : UCHAR_MAX;
    }
}

/* One band of rows, decoded straight into the region.  vips_sequential asks for whole rows; calls
   run at once from libvips' threads on the one reader, which minipsd allows on a whole file. */
static int minipsd_vips_psdload_generate(VipsRegion *region, void *seq, void *a, void *b, gboolean *stop)
{
    MinipsdVipsPsdload            *psd      = (MinipsdVipsPsdload *)a;
    minipsd_vips_psdload_sequence *sequence = (minipsd_vips_psdload_sequence *)seq;
    const VipsRect                *rect     = &region->valid;
    minipsd_decode_opts            opts;
    minipsd_target                 target;
    VipsPel                       *top  = VIPS_REGION_ADDR_TOPLEFT(region), *row;
    const size_t                   skip = VIPS_REGION_LSKIP(region);
    int                 rc, x, y;
    (void)b, (void)stop;

    if (rect->left != 0 || rect->width != region->im->Xsize) {
        minipsd_vips_psdload_error(psd, "only whole rows are decoded");
        return -1;
    }
    memset(&opts, 0, sizeof opts);
    opts.y_first = (uint32_t)rect->top;
    opts.y_count = (uint32_t)rect->height;
    opts.depth   = psd->info.depth;
    minipsd_target_interleaved(&target, top, skip, psd->planes, psd->info.depth);
    if (psd->indexed) target.channel[0].pixel_step = (ptrdiff_t)psd->bands;   /* the index, in R */
    if (psd->layer < 0) {
        rc = minipsd_decode_composite(psd->reader, &target, &opts);
    } else {
        opts.inflate      = minipsd_vips_psdload_inflate;
        opts.scratch      = sequence->scratch;
        opts.scratch_size = psd->scratch_size;
        opts.scratch_kept = sequence->kept;
        rc = minipsd_decode_layer(psd->reader, &psd->view, &target, &opts);
        sequence->kept = rc == MINIPSD_OK && sequence->scratch;
    }
    if (rc != MINIPSD_OK) {
        minipsd_vips_psdload_error(psd, minipsd_error_string(rc));
        return -1;
    }

    for (y = 0; y < rect->height; ++y) {
        row = top + (size_t)y * skip;
        if (psd->indexed) {
            minipsd_vips_psdload_expand_palette(psd, row, rect->width);
        } else if (psd->info.color_mode == MINIPSD_MODE_CMYK) {
            minipsd_vips__invert_cmyk(row, (size_t)rect->width, psd->bands, psd->info.depth);
        } else if (psd->info.color_mode == MINIPSD_MODE_BITMAP) {
            /* Bitmap decodes to 1 for black, 0 for white; libvips' B_W is 0 black, 255 white. */
            for (x = 0; x < rect->width; ++x) row[x] = row[x] ? 0 : UCHAR_MAX;
        }
    }
    return 0;
}

static int minipsd_vips_psdload_load(VipsForeignLoad *load)
{
    MinipsdVipsPsdload *psd = (MinipsdVipsPsdload *)load;
    VipsImage         **t   = (VipsImage **)vips_object_local_array(VIPS_OBJECT(load), 2);
    t[0] = vips_image_new();
    if (minipsd_vips_psdload_fields(psd, t[0])
        || vips_image_generate(t[0], minipsd_vips_psdload_start, minipsd_vips_psdload_generate,
                               minipsd_vips_psdload_stop, psd, NULL)
        || vips_sequential(t[0], &t[1], NULL) || vips_image_write(t[1], load->real))
        return -1;
    return 0;
}

static void minipsd_vips_psdload_class_init(MinipsdVipsPsdloadClass *class)
{
    GObjectClass         *gobject_class = G_OBJECT_CLASS(class);
    VipsObjectClass      *object_class  = (VipsObjectClass *)class;
    VipsForeignLoadClass *load_class    = (VipsForeignLoadClass *)class;

    gobject_class->dispose = minipsd_vips_psdload_dispose;
    minipsd_vips__class_properties(class);

    object_class->nickname    = "psdload_base";
    object_class->description = "load PSD and PSB with minipsd";

    load_class->get_flags = minipsd_vips_psdload_get_flags;
    load_class->header    = minipsd_vips_psdload_header;
    load_class->load      = minipsd_vips_psdload_load;

    PSD_ARGS_BEGIN
    VIPS_ARG_INT(class, "layer", 20, "Layer", "Load this layer (bottom first), -1 the composite",
                 VIPS_ARGUMENT_OPTIONAL_INPUT, G_STRUCT_OFFSET(MinipsdVipsPsdload, layer), PSD_COMPOSITE, INT_MAX,
                 PSD_COMPOSITE);
    PSD_ARGS_END
}

static void minipsd_vips_psdload_init(MinipsdVipsPsdload *psd)
{
    psd->layer = PSD_COMPOSITE;
}

/* --- psdload_source ----------------------------------------------------------------------- */

typedef struct _MinipsdVipsPsdloadSource {
    MinipsdVipsPsdload parent_object;
    VipsSource        *source;
} MinipsdVipsPsdloadSource;

typedef MinipsdVipsPsdloadClass MinipsdVipsPsdloadSourceClass;

G_DEFINE_TYPE(MinipsdVipsPsdloadSource, minipsd_vips_psdload_source, minipsd_vips_psdload_get_type())

static int minipsd_vips_psdload_source_build(VipsObject *object)
{
    MinipsdVipsPsdload       *psd    = (MinipsdVipsPsdload *)object;
    MinipsdVipsPsdloadSource *source = (MinipsdVipsPsdloadSource *)object;
    if (source->source) {
        psd->source = source->source;
        g_object_ref(psd->source);
    }
    return VIPS_OBJECT_CLASS(minipsd_vips_psdload_source_parent_class)->build(object);
}

static void minipsd_vips_psdload_source_class_init(MinipsdVipsPsdloadSourceClass *class)
{
    VipsObjectClass      *object_class    = (VipsObjectClass *)class;
    VipsOperationClass   *operation_class = VIPS_OPERATION_CLASS(class);
    VipsForeignLoadClass *load_class      = (VipsForeignLoadClass *)class;

    minipsd_vips__class_properties(class);
    object_class->nickname    = "psdload_source";
    object_class->description = "load PSD and PSB from a source with minipsd";
    object_class->build       = minipsd_vips_psdload_source_build;
    operation_class->flags |= VIPS_OPERATION_NOCACHE;
    load_class->is_a_source = minipsd_vips_psdload_is_a_source;

    PSD_ARGS_BEGIN
    VIPS_ARG_OBJECT(class, "source", 1, "Source", "Source to load from", VIPS_ARGUMENT_REQUIRED_INPUT,
                    G_STRUCT_OFFSET(MinipsdVipsPsdloadSource, source), VIPS_TYPE_SOURCE);
    PSD_ARGS_END
}

static void minipsd_vips_psdload_source_init(MinipsdVipsPsdloadSource *source)
{
    (void)source;
}

/* --- psdload (a file) ---------------------------------------------------------------------- */

typedef struct _MinipsdVipsPsdloadFile {
    MinipsdVipsPsdload parent_object;
    char              *filename;
} MinipsdVipsPsdloadFile;

typedef MinipsdVipsPsdloadClass MinipsdVipsPsdloadFileClass;

G_DEFINE_TYPE(MinipsdVipsPsdloadFile, minipsd_vips_psdload_file, minipsd_vips_psdload_get_type())

static gboolean minipsd_vips_psdload_is_a_file(const char *filename)
{
    VipsSource *source = vips_source_new_from_file(filename);
    gboolean    is_a;
    if (!source) return FALSE;
    is_a = minipsd_vips_psdload_is_a_source(source);
    g_object_unref(source);
    return is_a;
}

static int minipsd_vips_psdload_file_build(VipsObject *object)
{
    MinipsdVipsPsdload     *psd  = (MinipsdVipsPsdload *)object;
    MinipsdVipsPsdloadFile *file = (MinipsdVipsPsdloadFile *)object;
    if (file->filename && !(psd->source = vips_source_new_from_file(file->filename))) return -1;
    return VIPS_OBJECT_CLASS(minipsd_vips_psdload_file_parent_class)->build(object);
}

static void minipsd_vips_psdload_file_class_init(MinipsdVipsPsdloadFileClass *class)
{
    VipsObjectClass      *object_class  = (VipsObjectClass *)class;
    VipsForeignClass     *foreign_class = (VipsForeignClass *)class;
    VipsForeignLoadClass *load_class    = (VipsForeignLoadClass *)class;

    minipsd_vips__class_properties(class);
    object_class->nickname    = "psdload";
    object_class->description = "load PSD and PSB with minipsd";
    object_class->build       = minipsd_vips_psdload_file_build;
    foreign_class->suffs      = minipsd_vips__suffs;
    load_class->is_a          = minipsd_vips_psdload_is_a_file;

    PSD_ARGS_BEGIN
    VIPS_ARG_STRING(class, "filename", 1, "Filename", "Filename to load from", VIPS_ARGUMENT_REQUIRED_INPUT,
                    G_STRUCT_OFFSET(MinipsdVipsPsdloadFile, filename), NULL);
    PSD_ARGS_END
}

static void minipsd_vips_psdload_file_init(MinipsdVipsPsdloadFile *file)
{
    (void)file;
}

/* --- psdload_buffer ------------------------------------------------------------------------ */

typedef struct _MinipsdVipsPsdloadBuffer {
    MinipsdVipsPsdload parent_object;
    VipsBlob          *blob;
} MinipsdVipsPsdloadBuffer;

typedef MinipsdVipsPsdloadClass MinipsdVipsPsdloadBufferClass;

G_DEFINE_TYPE(MinipsdVipsPsdloadBuffer, minipsd_vips_psdload_buffer, minipsd_vips_psdload_get_type())

static gboolean minipsd_vips_psdload_is_a_buffer(const void *data, size_t size)
{
    return minipsd_vips_psdload_is_a_bytes((const unsigned char *)data, size);
}

static int minipsd_vips_psdload_buffer_build(VipsObject *object)
{
    MinipsdVipsPsdload       *psd    = (MinipsdVipsPsdload *)object;
    MinipsdVipsPsdloadBuffer *buffer = (MinipsdVipsPsdloadBuffer *)object;
    if (buffer->blob && !(psd->source = vips_source_new_from_blob(buffer->blob))) return -1;
    return VIPS_OBJECT_CLASS(minipsd_vips_psdload_buffer_parent_class)->build(object);
}

static void minipsd_vips_psdload_buffer_class_init(MinipsdVipsPsdloadBufferClass *class)
{
    VipsObjectClass      *object_class = (VipsObjectClass *)class;
    VipsForeignLoadClass *load_class   = (VipsForeignLoadClass *)class;

    minipsd_vips__class_properties(class);
    object_class->nickname    = "psdload_buffer";
    object_class->description = "load PSD and PSB from a buffer with minipsd";
    object_class->build       = minipsd_vips_psdload_buffer_build;
    load_class->is_a_buffer   = minipsd_vips_psdload_is_a_buffer;

    PSD_ARGS_BEGIN
    VIPS_ARG_BOXED(class, "buffer", 1, "Buffer", "Buffer to load from", VIPS_ARGUMENT_REQUIRED_INPUT,
                   G_STRUCT_OFFSET(MinipsdVipsPsdloadBuffer, blob), VIPS_TYPE_BLOB);
    PSD_ARGS_END
}

static void minipsd_vips_psdload_buffer_init(MinipsdVipsPsdloadBuffer *buffer)
{
    (void)buffer;
}
