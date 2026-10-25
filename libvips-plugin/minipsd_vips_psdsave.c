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
 * profile, EXIF, XMP and IPTC kept.  The image is read once, top to bottom, in bands; each
 * channel is encoded as its rows come, as minipsd encodes it, kept compressed and handed to
 * minipsd as stored: memory about the file's size, not the image's.  PSB when asked, for a .psb
 * file, a side past PSD's, or when a PSD cannot hold the file (a row its counts cannot hold, a
 * layer section past 4 GiB, a file past the 2 GB Photoshop opens as a PSD).
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

#define ZLIB_CONST   /* next_in const: rows are only read */
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

/* MinipsdVipsCompression as a GEnum, so libvips takes it by its nick ("rle", "zip", "none",
   "zip-predicted"). */
static GType minipsd_vips_psdsave_compression_get_type(void)
{
    static gsize type = 0;
    if (g_once_init_enter(&type)) {
        static const GEnumValue values[] = {
            { MINIPSD_VIPS_COMPRESSION_RLE, "MINIPSD_VIPS_COMPRESSION_RLE", "rle" },
            { MINIPSD_VIPS_COMPRESSION_ZIP, "MINIPSD_VIPS_COMPRESSION_ZIP", "zip" },
            { MINIPSD_VIPS_COMPRESSION_NONE, "MINIPSD_VIPS_COMPRESSION_NONE", "none" },
            { MINIPSD_VIPS_COMPRESSION_ZIP_PREDICTED, "MINIPSD_VIPS_COMPRESSION_ZIP_PREDICTED", "zip-predicted" },
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

#define PSD_MARKER_SIZE          2   /* a stored channel's compression marker, big-endian */
#define PSD_STORED_RAW           0   /* ... raw */
#define PSD_STORED_RLE           1   /* ... PackBits, its rows' sizes ahead of them */
#define PSD_STORED_ZIP           2   /* ... zlib, without prediction */
#define PSD_STORED_ZIP_PREDICTED 3   /* ... zlib of each sample's difference from its left neighbour */
#define PSD_COUNT_PSD            2   /* an RLE row's size: two bytes in a PSD */
#define PSD_COUNT_PSB            4   /* ... four in a PSB */
#define PSD_MAX_FILE             INT32_MAX   /* a PSD Photoshop opens: 2 GB at most, as Adobe documents it */
#define PSD_ZIP_ROOM             65536       /* deflate's output grows by at least this */

/* Bytes that grow, past what guint can count. */
typedef struct _MinipsdVipsPsdsaveBytes {
    uint8_t *data;
    size_t   size, room;
} MinipsdVipsPsdsaveBytes;

/* Room for extra bytes more: exactly, or at least doubled. */
static int minipsd_vips_psdsave_grow(MinipsdVipsPsdsaveBytes *b, size_t extra, int exact)
{
    if (extra > b->room - b->size) {
        const size_t room = exact ? b->size + extra : VIPS_MAX(b->room * 2, b->size + extra);
        uint8_t     *grown;
        if (room < b->size || !(grown = (uint8_t *)g_try_realloc(b->data, room))) return -1;
        b->data = grown;
        b->room = room;
    }
    return 0;
}

static int minipsd_vips_psdsave_append(MinipsdVipsPsdsaveBytes *b, const void *data, size_t size)
{
    if (minipsd_vips_psdsave_grow(b, size, 0)) return -1;
    memcpy(b->data + b->size, data, size);
    b->size += size;
    return 0;
}

/* One channel: room ahead (head) for its marker and its rows' sizes, the most a PSB needs, then
   its rows encoded one after another; each one's size (RLE), or its deflate (ZIP).  The block a
   file stores is then written in front of the rows, where they lie: never copied. */
typedef struct _MinipsdVipsPsdsaveChannel {
    MinipsdVipsPsdsaveBytes rows;
    size_t                  head;
    size_t                 *sizes;
    z_stream                zip;
    int                     zipping;
} MinipsdVipsPsdsaveChannel;

/* What the build encodes: the layer's channels (the colour ones, then transparency) when there is
   a layer, the composite's always (the colour ones, then transparency when the image has it). */
typedef struct _MinipsdVipsPsdsaveEncoding {
    MinipsdVipsPsdsaveChannel layer[PSD_MAX_PLANES], composite[PSD_MAX_PLANES];
    unsigned                  planes;      /* the image's bands: colour channels, then alpha */
    int                       layered, alpha, depth, mode;
    int                       layer_compression, composite_compression;   /* MINIPSD_RLE, _RAW, _ZIP, _ZIP_PREDICTED */
    uint32_t                  width, height;
    size_t                    longest;     /* the longest RLE row: a PSD counts to 65535 */
    uint8_t                  *row;         /* one encoded row, MINIPSD_ROW_BOUND */
    size_t                    row_size;
    uint8_t                  *ink;         /* one row of pixels, CMYK inverted to PSD's ink */
    uint8_t                  *matted;      /* one row of pixels, matted for the composite */
    int                       failed;      /* memory ran out, or deflate failed, while the bands came */
} MinipsdVipsPsdsaveEncoding;

static void minipsd_vips_psdsave_channel_free(MinipsdVipsPsdsaveChannel *c)
{
    g_free(c->rows.data);
    g_free(c->sizes);
    if (c->zipping) (void)deflateEnd(&c->zip);
    memset(c, 0, sizeof *c);
}

static void minipsd_vips_psdsave_encoding_free(MinipsdVipsPsdsaveEncoding *e)
{
    unsigned k;
    for (k = 0; k < PSD_MAX_PLANES; ++k) {
        minipsd_vips_psdsave_channel_free(&e->layer[k]);
        minipsd_vips_psdsave_channel_free(&e->composite[k]);
    }
    g_free(e->row);
    g_free(e->ink);
    g_free(e->matted);
    memset(e, 0, sizeof *e);
}

/* A channel ready for rows: the head for the tables of channels channels (the composite's first
   holds them all, the others none), raw rows' room known ahead, ZIP's deflate started. */
static int minipsd_vips_psdsave_channel_init(MinipsdVipsPsdsaveChannel *c, int compression, unsigned channels, uint32_t height,
                                             size_t row_bytes)
{
    const int rle = compression == MINIPSD_RLE, raw = compression == MINIPSD_RAW;
    c->head = channels ? PSD_MARKER_SIZE + (rle ? (size_t)channels * height * PSD_COUNT_PSB : 0) : 0;
    if (rle && !(c->sizes = g_try_new(size_t, height))) return -1;
    if (!rle && !raw) {
        if (deflateInit(&c->zip, PSD_ZIP_LEVEL) != Z_OK) return -1;
        c->zipping = 1;
    }
    if (minipsd_vips_psdsave_grow(&c->rows, c->head + (raw ? (size_t)(channels ? channels : 1u) * height * row_bytes : 0), 1)) return -1;
    c->rows.size = c->head;
    return 0;
}

static int minipsd_vips_psdsave_encoding_init(MinipsdVipsPsdsaveEncoding *e, VipsImage *in, int depth, int mode, int alpha,
                                              int layered, MinipsdVipsCompression compression)
{
    static const int layer_compression[] = { MINIPSD_RLE, MINIPSD_ZIP, MINIPSD_RAW, MINIPSD_ZIP_PREDICTED };   /* by option */
    const size_t sample = (size_t)(depth / CHAR_BIT);
    size_t       row_bytes, pixels;
    unsigned     k;
    memset(e, 0, sizeof *e);
    e->planes   = (unsigned)in->Bands;
    e->layered  = layered;
    e->alpha    = alpha;
    e->depth    = depth;
    e->mode     = mode;
    e->width    = (uint32_t)in->Xsize;
    e->height   = (uint32_t)in->Ysize;
    e->layer_compression     = layer_compression[compression];
    e->composite_compression = compression == MINIPSD_VIPS_COMPRESSION_NONE ? MINIPSD_RAW : MINIPSD_RLE;
    row_bytes   = (size_t)MINIPSD_ROW_BYTES(e->width, depth);
    pixels      = (size_t)e->width * e->planes * sample;
    e->row_size = (size_t)MINIPSD_ROW_BOUND(e->width, depth);
    e->row      = (uint8_t *)g_try_malloc(e->row_size);
    e->ink      = (uint8_t *)g_try_malloc(pixels);
    e->matted   = (uint8_t *)g_try_malloc(pixels);
    if (!e->row || !e->ink || !e->matted) return -1;
    for (k = 0; k < e->planes; ++k) {
        if (minipsd_vips_psdsave_channel_init(&e->composite[k], e->composite_compression, k ? 0 : e->planes, e->height, row_bytes))
            return -1;
        if (layered && minipsd_vips_psdsave_channel_init(&e->layer[k], e->layer_compression, 1, e->height, row_bytes)) return -1;
    }
    return 0;
}

/* data deflated onto the channel's rows; with Z_FINISH, the stream ended. */
static int minipsd_vips_psdsave_deflate(MinipsdVipsPsdsaveChannel *c, const uint8_t *data, size_t size, int flush)
{
    int rc;
    c->zip.next_in  = data;
    c->zip.avail_in = (uInt)size;   /* a row: within uInt */
    do {
        if (minipsd_vips_psdsave_grow(&c->rows, PSD_ZIP_ROOM, 0)) return -1;
        c->zip.next_out  = c->rows.data + c->rows.size;
        c->zip.avail_out = (uInt)VIPS_MIN(c->rows.room - c->rows.size, (size_t)UINT_MAX);
        rc               = deflate(&c->zip, flush);
        c->rows.size     = (size_t)(c->zip.next_out - c->rows.data);
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) return -1;
    } while (flush == Z_FINISH ? rc != Z_STREAM_END : c->zip.avail_out == 0);
    return 0;
}

/* Row y of channel k out of interleaved pixels, encoded into c: PackBits, raw, or deflated (raw
   or predicted rows into the channel's stream). */
static int minipsd_vips_psdsave_encode(MinipsdVipsPsdsaveEncoding *e, MinipsdVipsPsdsaveChannel *c, const uint8_t *pixels,
                                       unsigned k, uint32_t y, int compression)
{
    const size_t sample = (size_t)(e->depth / CHAR_BIT);
    size_t       n      = (size_t)MINIPSD_ROW_BYTES(e->width, e->depth);
    const int    rc     = compression == MINIPSD_ZIP_PREDICTED
                              ? minipsd_predict_row(e->depth, pixels + k * sample, e->planes * sample, e->width, e->row, e->row_size)
                              : minipsd_encode_row(compression == MINIPSD_RLE ? MINIPSD_RLE : MINIPSD_RAW, e->depth, pixels + k * sample,
                                                   e->planes * sample, e->width, e->row, e->row_size, &n);
    if (rc != MINIPSD_OK) return -1;
    if (c->zipping) return minipsd_vips_psdsave_deflate(c, e->row, n, Z_NO_FLUSH);
    if (c->sizes) {
        c->sizes[y] = n;
        if (n > e->longest) e->longest = n;
    }
    return minipsd_vips_psdsave_append(&c->rows, e->row, n);
}

/* count rows of interleaved pixels, from row y on, stride bytes apart: CMYK inverted to PSD's ink,
   each channel's rows encoded, the layer's as they are, the composite's matted (RGB with alpha, as
   minipsd mattes a composite it is given). */
static int minipsd_vips_psdsave_encode_rows(MinipsdVipsPsdsaveEncoding *e, const uint8_t *rows, uint32_t y, uint32_t count,
                                            size_t stride)
{
    const size_t bytes = (size_t)e->width * e->planes * (size_t)(e->depth / CHAR_BIT);
    uint32_t     i;
    unsigned     k;
    for (i = 0; i < count; ++i, rows += stride) {
        const uint8_t *pixels = rows, *composite;
        if (e->mode == MINIPSD_MODE_CMYK) {
            memcpy(e->ink, rows, bytes);
            minipsd_vips__invert_cmyk(e->ink, e->width, e->planes, e->depth);
            pixels = e->ink;
        }
        for (k = 0; e->layered && k < e->planes; ++k)
            if (minipsd_vips_psdsave_encode(e, &e->layer[k], pixels, k, y + i, e->layer_compression)) return -1;
        composite = pixels;
        if (e->alpha) {   /* matted as minipsd mattes a composite it is given (RGB; the others as they are) */
            memcpy(e->matted, pixels, bytes);
            (void)minipsd_matte_row(e->mode, e->depth, e->matted, e->width);
            composite = e->matted;
        }
        for (k = 0; k < e->planes; ++k)
            if (minipsd_vips_psdsave_encode(e, &e->composite[k], composite, k, y + i, e->composite_compression)) return -1;
    }
    return 0;
}

/* A band of the image, as libvips hands it: once, top to bottom; the region is the band. */
static int minipsd_vips_psdsave_band(VipsRegion *region, VipsRect *area, void *user)
{
    MinipsdVipsPsdsaveEncoding *e = (MinipsdVipsPsdsaveEncoding *)user;
    if (minipsd_vips_psdsave_encode_rows(e, VIPS_REGION_ADDR_TOPLEFT(region), (uint32_t)area->top, (uint32_t)area->height,
                                         (size_t)VIPS_REGION_LSKIP(region))) {
        e->failed = 1;
        return -1;
    }
    return 0;
}

/* Every row in: the deflates ended, the composite's channels joined behind the first's head (the
   file stores them as one block). */
static int minipsd_vips_psdsave_finish(MinipsdVipsPsdsaveEncoding *e)
{
    MinipsdVipsPsdsaveBytes *first = &e->composite[0].rows;
    size_t                   total = first->size;
    unsigned                 k;
    for (k = 0; k < e->planes; ++k)
        if (e->layer[k].zipping) {
            if (minipsd_vips_psdsave_deflate(&e->layer[k], NULL, 0, Z_FINISH)) return -1;
            (void)deflateEnd(&e->layer[k].zip);
            e->layer[k].zipping = 0;
        }
    for (k = 1; k < e->planes; ++k) total += e->composite[k].rows.size;
    if (minipsd_vips_psdsave_grow(first, total - first->size, 1)) return -1;
    for (k = 1; k < e->planes; ++k) {
        memcpy(first->data + first->size, e->composite[k].rows.data, e->composite[k].rows.size);
        first->size += e->composite[k].rows.size;
        g_free(e->composite[k].rows.data);   /* copied: the first holds it now */
        memset(&e->composite[k].rows, 0, sizeof e->composite[k].rows);
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

/* The block a file stores for channels c[0] .. c[channels - 1], whose rows c[0] holds: the
   marker, an RLE one's row sizes in count bytes each, every channel's in turn, then the rows.
   Written into c[0]'s head, just ahead of the rows. */
static minipsd_bytes minipsd_vips_psdsave_block(MinipsdVipsPsdsaveChannel *c, unsigned channels, uint32_t height, unsigned count,
                                               int compression)
{
    const int     rle   = compression == MINIPSD_RLE;
    const size_t  table = rle ? (size_t)channels * height * count : 0;
    const size_t  start = c->head - PSD_MARKER_SIZE - table;
    const uint8_t marker = rle ? PSD_STORED_RLE : compression == MINIPSD_RAW ? PSD_STORED_RAW
                         : compression == MINIPSD_ZIP ? PSD_STORED_ZIP : PSD_STORED_ZIP_PREDICTED;
    minipsd_bytes block;
    uint8_t      *p = minipsd_vips_psdsave_put(c->rows.data + start, marker, PSD_MARKER_SIZE);
    unsigned      k;
    uint32_t      y;
    for (k = 0; rle && k < channels; ++k)
        for (y = 0; y < height; ++y) p = minipsd_vips_psdsave_put(p, c[k].sizes[y], count);
    block.data = c->rows.data + start;
    block.size = c->rows.size - start;
    return block;
}

/* The document's channels as a PSD or a PSB stores them. */
static void minipsd_vips_psdsave_stored(MinipsdVipsPsdsaveEncoding *e, minipsd_doc *doc, minipsd_bytes *stored, int psb)
{
    const unsigned count = psb ? PSD_COUNT_PSB : PSD_COUNT_PSD;
    unsigned       k;
    doc->format = psb ? MINIPSD_PSB : MINIPSD_PSD;
    for (k = 0; e->layered && k < e->planes; ++k)   /* the colour channels, then transparency */
        stored[k] = minipsd_vips_psdsave_block(&e->layer[k], 1, e->height, count, e->layer_compression);
    doc->composite_stored = minipsd_vips_psdsave_block(e->composite, e->planes, e->height, count, e->composite_compression);
}

/* Whether a PSD holds the document: its layer section within the 4 GiB its length counts
   (minipsd refuses one past it, MINIPSD_ERR_SIZE), the file within PSD_MAX_FILE.  Any other
   refusal is minipsd_write's to report. */
static int minipsd_vips_psdsave_fits_psd(const minipsd_doc *doc)
{
    minipsd_writer *w;
    void           *workspace;
    size_t          size = 0;
    int             rc = minipsd_writer_workspace_size(doc, NULL, &size), fits;
    if (rc != MINIPSD_OK) return rc != MINIPSD_ERR_SIZE;
    if (!(workspace = g_try_malloc(size))) return 1;   /* minipsd_write will not find it either */
    rc   = minipsd_writer_init(&w, doc, NULL, workspace, size);
    fits = rc == MINIPSD_OK ? minipsd_writer_total(w) <= PSD_MAX_FILE : rc != MINIPSD_ERR_SIZE;
    g_free(workspace);
    return fits;
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
    const void                *icc;
    const char                *filename;
    size_t                     icc_size, colors, count = 0;
    double                     ratio;
    int                        rc, alpha, psb, memory;

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

    /* Every channel encoded once, as minipsd encodes it, while libvips hands the image over in
       bands, top to bottom: memory about the file's size, never the whole image's. */
    memory = minipsd_vips_psdsave_encoding_init(&e, in, doc.depth, doc.color_mode, alpha, alpha || psd->layered, psd->compression);
    if (!memory && vips_sink_disc(in, minipsd_vips_psdsave_band, &e)) {
        memory = e.failed;
        if (!memory) {   /* the image's own error, reported where it arose */
            minipsd_vips_psdsave_encoding_free(&e);
            return -1;
        }
    }
    if (!memory) memory = minipsd_vips_psdsave_finish(&e);
    if (memory) {
        minipsd_vips_psdsave_encoding_free(&e);
        vips_error(VIPS_OBJECT_GET_CLASS(object)->nickname, "%s", minipsd_error_string(MINIPSD_ERR_ALLOC));
        return -1;
    }

    memset(stored, 0, sizeof stored);   /* past the image's bands: none (transparency of size 0) */
    memset(&layer, 0, sizeof layer);
    layer.name     = psd->layer_name ? psd->layer_name : PSD_LAYER_NAME;
    layer.opacity  = UCHAR_MAX;
    layer.no_alpha = (uint8_t)!alpha;
    layer.stored   = stored;               /* its own rectangle, the canvas: a stored layer gives it */
    layer.bottom   = (int32_t)doc.height;
    layer.right    = (int32_t)doc.width;
    doc.layers             = e.layered ? &layer : NULL;   /* without alpha: flat, unless asked */
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

    /* PSB by the option, a file named .psb (libvips writes files through a target too), a side
       past PSD's; then, the channels known, a row a PSD cannot count, a layer section past its
       4 GiB, or a file past what Photoshop opens as a PSD. */
    filename = vips_connection_filename(VIPS_CONNECTION(psd->target));
    psb      = psd->psb || (filename && vips_iscasepostfix(filename, ".psb")) || in->Xsize > MINIPSD_MAX_SIDE
          || in->Ysize > MINIPSD_MAX_SIDE || e.longest > UINT16_MAX;
    minipsd_vips_psdsave_stored(&e, &doc, stored, psb);
    if (!psb && !minipsd_vips_psdsave_fits_psd(&doc)) minipsd_vips_psdsave_stored(&e, &doc, stored, 1);

    rc = minipsd_write(&doc, NULL, minipsd_vips_psdsave_to_target, psd->target);
    minipsd_vips_psdsave_encoding_free(&e);
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
    VIPS_ARG_ENUM(class, "compression", 21, "Compression", "The layer's compression: rle, zip, zip-predicted, or none (the composite too)",
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
