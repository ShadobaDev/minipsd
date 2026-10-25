/**
 * \file
 * \brief psdsave, psdsave_buffer, psdsave_target: an image as a PSD or PSB, one layer or flat.
 *
 * Laid out as libvips' own savers are (a base class over a VipsTarget; the file and buffer
 * variants make one), named as libvips names its own, in foreign/ as libvips keeps its formats:
 * the directory moves into libvips/foreign/ as it is (minipsd.h into its own foreign/minipsd/,
 * as libvips keeps the libraries it carries, libnsgif and qoi).  An image with alpha is written as one layer that is also the
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
#include "psd.h"

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

typedef struct _VipsForeignSavePsd {
    VipsForeignSave parent_object;
    VipsTarget     *target;      /* set by the subclass's build */
    gboolean        psb;         /* the option; also a .psb file, or a side past PSD's */
    VipsForeignPsdCompression compression;   /* the option: the layer's channels; none: the composite's too */
    char           *layer_name;  /* the option */
    gboolean        layered;     /* the option: a layer also for an image without alpha */
} VipsForeignSavePsd;

typedef VipsForeignSaveClass VipsForeignSavePsdClass;

G_DEFINE_ABSTRACT_TYPE(VipsForeignSavePsd, vips_foreign_save_psd, VIPS_TYPE_FOREIGN_SAVE)

/* VipsForeignPsdCompression as a GEnum, so libvips takes it by its nick ("rle", "zip", "none",
   "zip-predicted"). */
static GType vips_foreign_psd_compression_get_type(void)
{
    static gsize type = 0;
    if (g_once_init_enter(&type)) {
        static const GEnumValue values[] = {
            { VIPS_FOREIGN_PSD_COMPRESSION_RLE, "VIPS_FOREIGN_PSD_COMPRESSION_RLE", "rle" },
            { VIPS_FOREIGN_PSD_COMPRESSION_ZIP, "VIPS_FOREIGN_PSD_COMPRESSION_ZIP", "zip" },
            { VIPS_FOREIGN_PSD_COMPRESSION_NONE, "VIPS_FOREIGN_PSD_COMPRESSION_NONE", "none" },
            { VIPS_FOREIGN_PSD_COMPRESSION_ZIP_PREDICTED, "VIPS_FOREIGN_PSD_COMPRESSION_ZIP_PREDICTED", "zip-predicted" },
            { 0, NULL, NULL },
        };
        g_once_init_leave(&type, g_enum_register_static("VipsForeignPsdCompression", values));
    }
    return (GType)type;
}

/* What libvips' band formats become: 8-bit, 16-bit and float samples, as PSD has them. */
static VipsBandFormat vips_foreign_save_psd_save_formats[] = {
    /* UCHAR, CHAR, USHORT, SHORT, UINT, INT, FLOAT, COMPLEX, DOUBLE, DPCOMPLEX */
    VIPS_FORMAT_UCHAR, VIPS_FORMAT_UCHAR, VIPS_FORMAT_USHORT, VIPS_FORMAT_USHORT, VIPS_FORMAT_USHORT,
    VIPS_FORMAT_USHORT, VIPS_FORMAT_FLOAT, VIPS_FORMAT_FLOAT, VIPS_FORMAT_FLOAT, VIPS_FORMAT_FLOAT
};

static void vips_foreign_save_psd_dispose(GObject *gobject)
{
    VipsForeignSavePsd *psd = (VipsForeignSavePsd *)gobject;
    VIPS_UNREF(psd->target);
    G_OBJECT_CLASS(vips_foreign_save_psd_parent_class)->dispose(gobject);
}

static int vips_foreign_save_psd_to_target(void *user, const void *data, size_t size)
{
    return vips_target_write((VipsTarget *)user, data, size) == 0;   /* 0 stops the writer */
}

/* A blob of the image as a resource to keep, when it has one. */
static void vips_foreign_save_psd_keep_blob(VipsImage *image, const char *name, uint16_t id, minipsd_resource *res,
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
typedef struct _VipsForeignSavePsdBytes {
    uint8_t *data;
    size_t   size, room;
} VipsForeignSavePsdBytes;

/* Room for extra bytes more: exactly, or at least doubled. */
static int vips_foreign_save_psd_grow(VipsForeignSavePsdBytes *b, size_t extra, int exact)
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

static int vips_foreign_save_psd_append(VipsForeignSavePsdBytes *b, const void *data, size_t size)
{
    if (vips_foreign_save_psd_grow(b, size, 0)) return -1;
    memcpy(b->data + b->size, data, size);
    b->size += size;
    return 0;
}

/* One channel: room ahead (head) for its marker and its rows' sizes, the most a PSB needs, then
   its rows encoded one after another; each one's size (RLE), or its deflate (ZIP).  The block a
   file stores is then written in front of the rows, where they lie: never copied. */
typedef struct _VipsForeignSavePsdChannel {
    VipsForeignSavePsdBytes rows;
    size_t                  head;
    size_t                 *sizes;
    z_stream                zip;
    int                     zipping;
} VipsForeignSavePsdChannel;

/* One image's channels as a file stores them: a layer's, each channel its own block, or the
   composite's, one block, its channels joined behind the first's head, its rows matted when the
   image has alpha (as minipsd mattes a composite it is given). */
typedef struct _VipsForeignSavePsdChannels {
    VipsForeignSavePsdChannel channel[PSD_MAX_PLANES];
    unsigned                  planes;        /* the image's bands: colour channels, then alpha */
    int                       compression;   /* MINIPSD_RLE, _RAW, _ZIP, _ZIP_PREDICTED */
    int                       composite, matte;
    uint32_t                  height;
    size_t                    longest;       /* the longest RLE row: a PSD counts to 65535 */
} VipsForeignSavePsdChannels;

/* One image read once, top to bottom, each row encoded into every target's channels. */
typedef struct _VipsForeignSavePsdPass {
    VipsForeignSavePsdChannels **targets;
    unsigned                     count, planes;
    int                          depth, mode;
    uint32_t                     width;
    uint8_t                     *row;        /* one encoded row, MINIPSD_ROW_BOUND */
    size_t                       row_size;
    uint8_t                     *ink;        /* one row of pixels, CMYK inverted to PSD's ink */
    uint8_t                     *matted;     /* one row of pixels, matted for a composite */
    int                          failed;     /* memory ran out, or deflate failed, while the bands came */
} VipsForeignSavePsdPass;

static void vips_foreign_save_psd_channel_free(VipsForeignSavePsdChannel *c)
{
    g_free(c->rows.data);
    g_free(c->sizes);
    if (c->zipping) (void)deflateEnd(&c->zip);
    memset(c, 0, sizeof *c);
}

static void vips_foreign_save_psd_channels_free(VipsForeignSavePsdChannels *c)
{
    unsigned k;
    for (k = 0; k < PSD_MAX_PLANES; ++k) vips_foreign_save_psd_channel_free(&c->channel[k]);
    memset(c, 0, sizeof *c);
}

/* A channel ready for rows: the head for the tables of channels channels (the composite's first
   holds them all, its others none), raw rows' room known ahead, ZIP's deflate started. */
static int vips_foreign_save_psd_channel_init(VipsForeignSavePsdChannel *c, int compression, unsigned channels, uint32_t height,
                                              size_t row_bytes)
{
    const int rle = compression == MINIPSD_RLE, raw = compression == MINIPSD_RAW;
    c->head = channels ? PSD_MARKER_SIZE + (rle ? (size_t)channels * height * PSD_COUNT_PSB : 0) : 0;
    if (rle && !(c->sizes = g_try_new(size_t, height))) return -1;
    if (!rle && !raw) {
        if (deflateInit(&c->zip, PSD_ZIP_LEVEL) != Z_OK) return -1;
        c->zipping = 1;
    }
    if (vips_foreign_save_psd_grow(&c->rows, c->head + (raw ? (size_t)(channels ? channels : 1u) * height * row_bytes : 0), 1))
        return -1;
    c->rows.size = c->head;
    return 0;
}

/* An image's channels ready for its rows: a layer's (composite 0) or the composite's. */
static int vips_foreign_save_psd_channels_init(VipsForeignSavePsdChannels *c, VipsImage *in, int depth, int compression,
                                               int composite, int matte)
{
    const size_t row_bytes = (size_t)MINIPSD_ROW_BYTES((uint32_t)in->Xsize, depth);
    unsigned     k;
    memset(c, 0, sizeof *c);
    c->planes      = (unsigned)in->Bands;
    c->compression = compression;
    c->composite   = composite;
    c->matte       = matte;
    c->height      = (uint32_t)in->Ysize;
    for (k = 0; k < c->planes; ++k)
        if (vips_foreign_save_psd_channel_init(&c->channel[k], compression, composite ? (k ? 0 : c->planes) : 1, c->height, row_bytes))
            return -1;
    return 0;
}

/* data deflated onto the channel's rows; with Z_FINISH, the stream ended. */
static int vips_foreign_save_psd_deflate(VipsForeignSavePsdChannel *c, const uint8_t *data, size_t size, int flush)
{
    int rc;
    c->zip.next_in  = data;
    c->zip.avail_in = (uInt)size;   /* a row: within uInt */
    do {
        if (vips_foreign_save_psd_grow(&c->rows, PSD_ZIP_ROOM, 0)) return -1;
        c->zip.next_out  = c->rows.data + c->rows.size;
        c->zip.avail_out = (uInt)VIPS_MIN(c->rows.room - c->rows.size, (size_t)UINT_MAX);
        rc               = deflate(&c->zip, flush);
        c->rows.size     = (size_t)(c->zip.next_out - c->rows.data);
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) return -1;
    } while (flush == Z_FINISH ? rc != Z_STREAM_END : c->zip.avail_out == 0);
    return 0;
}

/* Row y of channel k out of interleaved pixels, encoded into t's channel: PackBits, raw, or
   deflated (raw or predicted rows into the channel's stream). */
static int vips_foreign_save_psd_encode(VipsForeignSavePsdPass *p, VipsForeignSavePsdChannels *t, const uint8_t *pixels, unsigned k,
                                        uint32_t y)
{
    VipsForeignSavePsdChannel *c      = &t->channel[k];
    const size_t               sample = (size_t)(p->depth / CHAR_BIT);
    size_t                     n      = (size_t)MINIPSD_ROW_BYTES(p->width, p->depth);
    const int                  rc     = t->compression == MINIPSD_ZIP_PREDICTED
                                            ? minipsd_predict_row(p->depth, pixels + k * sample, p->planes * sample, p->width, p->row,
                                                                  p->row_size)
                                            : minipsd_encode_row(t->compression == MINIPSD_RLE ? MINIPSD_RLE : MINIPSD_RAW, p->depth,
                                                                 pixels + k * sample, p->planes * sample, p->width, p->row,
                                                                 p->row_size, &n);
    if (rc != MINIPSD_OK) return -1;
    if (c->zipping) return vips_foreign_save_psd_deflate(c, p->row, n, Z_NO_FLUSH);
    if (c->sizes) {
        c->sizes[y] = n;
        if (n > t->longest) t->longest = n;
    }
    return vips_foreign_save_psd_append(&c->rows, p->row, n);
}

/* count rows of interleaved pixels, from row y on, stride bytes apart: CMYK inverted to PSD's ink,
   then each target's channels encoded, a composite's rows matted when it asks (RGB with alpha). */
static int vips_foreign_save_psd_encode_rows(VipsForeignSavePsdPass *p, const uint8_t *rows, uint32_t y, uint32_t count,
                                             size_t stride)
{
    const size_t bytes = (size_t)p->width * p->planes * (size_t)(p->depth / CHAR_BIT);
    uint32_t     i;
    unsigned     t, k;
    for (i = 0; i < count; ++i, rows += stride) {
        const uint8_t *pixels = rows;
        int            matted = 0;
        if (p->mode == MINIPSD_MODE_CMYK) {
            memcpy(p->ink, rows, bytes);
            vips__foreign_psd_invert_cmyk(p->ink, p->width, p->planes, p->depth);
            pixels = p->ink;
        }
        for (t = 0; t < p->count; ++t) {
            VipsForeignSavePsdChannels *target = p->targets[t];
            if (target->matte && !matted) {   /* as minipsd mattes a composite it is given (RGB; the others as they are) */
                memcpy(p->matted, pixels, bytes);
                (void)minipsd_matte_row(p->mode, p->depth, p->matted, p->width);
                matted = 1;
            }
            for (k = 0; k < p->planes; ++k)
                if (vips_foreign_save_psd_encode(p, target, target->matte ? p->matted : pixels, k, y + i)) return -1;
        }
    }
    return 0;
}

/* A band of the image, as libvips hands it: once, top to bottom; the region is the band. */
static int vips_foreign_save_psd_band(VipsRegion *region, VipsRect *area, void *user)
{
    VipsForeignSavePsdPass *p = (VipsForeignSavePsdPass *)user;
    if (vips_foreign_save_psd_encode_rows(p, VIPS_REGION_ADDR_TOPLEFT(region), (uint32_t)area->top, (uint32_t)area->height,
                                          (size_t)VIPS_REGION_LSKIP(region))) {
        p->failed = 1;
        return -1;
    }
    return 0;
}

/* in read once, top to bottom, in the bands libvips hands a saver, its rows encoded into count
   targets' channels (each with in's bands).  -1 on failure, *memory set when memory ran out or a
   deflate failed; otherwise the error is the image's own, reported where it arose. */
static int vips_foreign_save_psd_pass(VipsImage *in, int depth, int mode, VipsForeignSavePsdChannels **targets, unsigned count,
                                      int *memory)
{
    VipsForeignSavePsdPass p;
    const size_t           pixels = (size_t)in->Xsize * (size_t)in->Bands * (size_t)(depth / CHAR_BIT);
    int                    rc;
    memset(&p, 0, sizeof p);
    p.targets  = targets;
    p.count    = count;
    p.planes   = (unsigned)in->Bands;
    p.depth    = depth;
    p.mode     = mode;
    p.width    = (uint32_t)in->Xsize;
    p.row_size = (size_t)MINIPSD_ROW_BOUND(p.width, depth);
    p.row      = (uint8_t *)g_try_malloc(p.row_size);
    p.ink      = (uint8_t *)g_try_malloc(pixels);
    p.matted   = (uint8_t *)g_try_malloc(pixels);
    rc         = !p.row || !p.ink || !p.matted ? -1 : vips_sink_disc(in, vips_foreign_save_psd_band, &p);
    *memory    = rc && (!p.row || !p.ink || !p.matted || p.failed);
    g_free(p.row);
    g_free(p.ink);
    g_free(p.matted);
    return rc ? -1 : 0;
}

/* Every row in: the deflates ended, a composite's channels joined behind the first's head (the
   file stores them as one block). */
static int vips_foreign_save_psd_finish(VipsForeignSavePsdChannels *c)
{
    VipsForeignSavePsdBytes *first = &c->channel[0].rows;
    size_t                   total = first->size;
    unsigned                 k;
    for (k = 0; k < c->planes; ++k)
        if (c->channel[k].zipping) {
            if (vips_foreign_save_psd_deflate(&c->channel[k], NULL, 0, Z_FINISH)) return -1;
            (void)deflateEnd(&c->channel[k].zip);
            c->channel[k].zipping = 0;
        }
    if (!c->composite) return 0;
    for (k = 1; k < c->planes; ++k) total += c->channel[k].rows.size;
    if (vips_foreign_save_psd_grow(first, total - first->size, 1)) return -1;
    for (k = 1; k < c->planes; ++k) {
        memcpy(first->data + first->size, c->channel[k].rows.data, c->channel[k].rows.size);
        first->size += c->channel[k].rows.size;
        g_free(c->channel[k].rows.data);   /* copied: the first holds it now */
        memset(&c->channel[k].rows, 0, sizeof c->channel[k].rows);
    }
    return 0;
}

/* Puts n, big-endian, in size bytes. */
static uint8_t *vips_foreign_save_psd_put(uint8_t *p, uint64_t n, unsigned size)
{
    unsigned i;
    for (i = 0; i < size; ++i) p[i] = (uint8_t)(n >> (CHAR_BIT * (size - 1 - i)));
    return p + size;
}

/* The block a file stores for channels c[0] .. c[channels - 1], whose rows c[0] holds: the
   marker, an RLE one's row sizes in count bytes each, every channel's in turn, then the rows.
   Written into c[0]'s head, just ahead of the rows. */
static minipsd_bytes vips_foreign_save_psd_block(VipsForeignSavePsdChannel *c, unsigned channels, uint32_t height, unsigned count,
                                                 int compression)
{
    const int     rle    = compression == MINIPSD_RLE;
    const size_t  table  = rle ? (size_t)channels * height * count : 0;
    const size_t  start  = c->head - PSD_MARKER_SIZE - table;
    const uint8_t marker = rle ? PSD_STORED_RLE : compression == MINIPSD_RAW ? PSD_STORED_RAW
                         : compression == MINIPSD_ZIP ? PSD_STORED_ZIP : PSD_STORED_ZIP_PREDICTED;
    minipsd_bytes block;
    uint8_t      *p = vips_foreign_save_psd_put(c->rows.data + start, marker, PSD_MARKER_SIZE);
    unsigned      k;
    uint32_t      y;
    for (k = 0; rle && k < channels; ++k)
        for (y = 0; y < height; ++y) p = vips_foreign_save_psd_put(p, c[k].sizes[y], count);
    block.data = c->rows.data + start;
    block.size = c->rows.size - start;
    return block;
}

/* A layer's channels as a PSD (count 2) or a PSB (4) stores them, into stored: the colour
   channels, then transparency. */
static void vips_foreign_save_psd_layer_blocks(VipsForeignSavePsdChannels *c, unsigned count, minipsd_bytes *stored)
{
    unsigned k;
    for (k = 0; k < c->planes; ++k) stored[k] = vips_foreign_save_psd_block(&c->channel[k], 1, c->height, count, c->compression);
}

/* The composite's channels as a PSD or a PSB stores them: one block. */
static minipsd_bytes vips_foreign_save_psd_composite_block(VipsForeignSavePsdChannels *c, unsigned count)
{
    return vips_foreign_save_psd_block(c->channel, c->planes, c->height, count, c->compression);
}

/* Whether a PSD holds the document: its layer section within the 4 GiB its length counts
   (minipsd refuses one past it, MINIPSD_ERR_SIZE), the file within PSD_MAX_FILE.  Any other
   refusal is minipsd_write's to report. */
static int vips_foreign_save_psd_fits_psd(const minipsd_doc *doc)
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

/* MINIPSD_* compression by the option: the layers'; the composite's is RLE, raw with none. */
static int vips_foreign_save_psd_layer_compression(VipsForeignPsdCompression compression)
{
    static const int by_option[] = { MINIPSD_RLE, MINIPSD_ZIP, MINIPSD_RAW, MINIPSD_ZIP_PREDICTED };
    return by_option[compression];
}

static int vips_foreign_save_psd_build(VipsObject *object)
{
    VipsForeignSave            *save = (VipsForeignSave *)object;
    VipsForeignSavePsd         *psd  = (VipsForeignSavePsd *)object;
    VipsImage                  *in;
    minipsd_resource            resources[PSD_MAX_RESOURCES];
    minipsd_bytes               stored[PSD_MAX_PLANES];
    minipsd_layer               layer;
    minipsd_doc                 doc;
    VipsForeignSavePsdChannels  layer_channels, composite_channels, *targets[2];
    const void                 *icc;
    const char                 *filename;
    size_t                      icc_size, colors, count = 0;
    double                      ratio;
    unsigned                    n = 0, table;
    int                         rc, alpha, layered, psb, memory = 0;

    if (VIPS_OBJECT_CLASS(vips_foreign_save_psd_parent_class)->build(object)) return -1;
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
    layered    = alpha || psd->layered;        /* without alpha: flat, unless asked */
    doc.width  = (uint32_t)in->Xsize;
    doc.height = (uint32_t)in->Ysize;
    doc.dpi    = in->Xres * MM_PER_INCH;
    if (!(doc.dpi > 0.0 && doc.dpi < PSD_MAX_DPI)) doc.dpi = PSD_DEFAULT_DPI;

    /* Every channel encoded once, as minipsd encodes it, while libvips hands the image over in
       bands, top to bottom: memory about the file's size, never the whole image's.  One pass
       feeds the layer and the composite. */
    memset(&layer_channels, 0, sizeof layer_channels);
    if (layered) {
        memory = vips_foreign_save_psd_channels_init(&layer_channels, in, doc.depth,
                                                     vips_foreign_save_psd_layer_compression(psd->compression), 0, 0);
        targets[n++] = &layer_channels;
    }
    if (!memory)
        memory = vips_foreign_save_psd_channels_init(&composite_channels, in, doc.depth,
                                                     psd->compression == VIPS_FOREIGN_PSD_COMPRESSION_NONE ? MINIPSD_RAW : MINIPSD_RLE,
                                                     1, alpha);
    else
        memset(&composite_channels, 0, sizeof composite_channels);
    targets[n++] = &composite_channels;
    if (!memory && vips_foreign_save_psd_pass(in, doc.depth, doc.color_mode, targets, n, &memory) && !memory) {
        vips_foreign_save_psd_channels_free(&layer_channels);   /* the image's own error, reported where it arose */
        vips_foreign_save_psd_channels_free(&composite_channels);
        return -1;
    }
    if (!memory) memory = vips_foreign_save_psd_finish(&layer_channels) || vips_foreign_save_psd_finish(&composite_channels);
    if (memory) {
        vips_foreign_save_psd_channels_free(&layer_channels);
        vips_foreign_save_psd_channels_free(&composite_channels);
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
    doc.layers             = layered ? &layer : NULL;
    doc.layer_count        = layered ? 1 : 0;
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
    vips_foreign_save_psd_keep_blob(in, VIPS_META_EXIF_NAME, MINIPSD_RES_EXIF_DATA_1, resources, &count);
    vips_foreign_save_psd_keep_blob(in, VIPS_META_XMP_NAME, MINIPSD_RES_XMP_METADATA, resources, &count);
    vips_foreign_save_psd_keep_blob(in, VIPS_META_IPTC_NAME, MINIPSD_RES_IPTC_NAA, resources, &count);
    doc.resources      = resources;
    doc.resource_count = count;

    /* PSB by the option, a file named .psb (libvips writes files through a target too), a side
       past PSD's; then, the channels known, a row a PSD cannot count, a layer section past its
       4 GiB, or a file past what Photoshop opens as a PSD. */
    filename = vips_connection_filename(VIPS_CONNECTION(psd->target));
    psb      = psd->psb || (filename && vips_iscasepostfix(filename, ".psb")) || in->Xsize > MINIPSD_MAX_SIDE
          || in->Ysize > MINIPSD_MAX_SIDE || layer_channels.longest > UINT16_MAX || composite_channels.longest > UINT16_MAX;
    for (;;) {   /* the blocks for the format, again for a PSB when a PSD cannot hold them */
        table      = psb ? PSD_COUNT_PSB : PSD_COUNT_PSD;
        doc.format = psb ? MINIPSD_PSB : MINIPSD_PSD;
        if (layered) vips_foreign_save_psd_layer_blocks(&layer_channels, table, stored);
        doc.composite_stored = vips_foreign_save_psd_composite_block(&composite_channels, table);
        if (psb || vips_foreign_save_psd_fits_psd(&doc)) break;
        psb = 1;
    }

    rc = minipsd_write(&doc, NULL, vips_foreign_save_psd_to_target, psd->target);
    vips_foreign_save_psd_channels_free(&layer_channels);
    vips_foreign_save_psd_channels_free(&composite_channels);
    if (rc != MINIPSD_OK) {
        vips_error(VIPS_OBJECT_GET_CLASS(object)->nickname, "%s", minipsd_error_string(rc));
        return -1;
    }
    return vips_target_end(psd->target);
}

static void vips_foreign_save_psd_class_init(VipsForeignSavePsdClass *class)
{
    GObjectClass         *gobject_class = G_OBJECT_CLASS(class);
    VipsObjectClass      *object_class  = (VipsObjectClass *)class;
    VipsForeignSaveClass *save_class    = (VipsForeignSaveClass *)class;

    gobject_class->dispose = vips_foreign_save_psd_dispose;
    vips__foreign_psd_class_properties(class);

    object_class->nickname    = "psdsave_base";
    object_class->description = "save PSD and PSB with minipsd";
    object_class->build       = vips_foreign_save_psd_build;

    /* Any bands: the build takes grey, RGB and CMYK, with or without alpha, and refuses the rest. */
#if VIPS_MAJOR_VERSION > 8 || (VIPS_MAJOR_VERSION == 8 && VIPS_MINOR_VERSION >= 17)
    save_class->saveable = VIPS_FOREIGN_SAVEABLE_ANY;
#else
    save_class->saveable = VIPS_SAVEABLE_ANY;
#endif
    save_class->format_table = vips_foreign_save_psd_save_formats;

    PSD_ARGS_BEGIN
    VIPS_ARG_BOOL(class, "psb", 20, "PSB", "Save as PSB (also for a .psb file, or a side past 30000)",
                  VIPS_ARGUMENT_OPTIONAL_INPUT, G_STRUCT_OFFSET(VipsForeignSavePsd, psb), FALSE);
    VIPS_ARG_ENUM(class, "compression", 21, "Compression", "The layer's compression: rle, zip, zip-predicted, or none (the composite too)",
                  VIPS_ARGUMENT_OPTIONAL_INPUT, G_STRUCT_OFFSET(VipsForeignSavePsd, compression),
                  vips_foreign_psd_compression_get_type(), VIPS_FOREIGN_PSD_COMPRESSION_RLE);
    VIPS_ARG_STRING(class, "layer_name", 22, "Layer name", "The layer's name, UTF-8",
                    VIPS_ARGUMENT_OPTIONAL_INPUT, G_STRUCT_OFFSET(VipsForeignSavePsd, layer_name), PSD_LAYER_NAME);
    VIPS_ARG_BOOL(class, "layered", 23, "Layered", "Without alpha, a layer too, without transparency (else flat)",
                  VIPS_ARGUMENT_OPTIONAL_INPUT, G_STRUCT_OFFSET(VipsForeignSavePsd, layered), FALSE);
    PSD_ARGS_END
}

static void vips_foreign_save_psd_init(VipsForeignSavePsd *psd)
{
    (void)psd;
}

/* --- psdsave_target ------------------------------------------------------------------------ */

typedef struct _VipsForeignSavePsdTarget {
    VipsForeignSavePsd parent_object;
    VipsTarget        *target;
} VipsForeignSavePsdTarget;

typedef VipsForeignSavePsdClass VipsForeignSavePsdTargetClass;

G_DEFINE_TYPE(VipsForeignSavePsdTarget, vips_foreign_save_psd_target, vips_foreign_save_psd_get_type())

static int vips_foreign_save_psd_target_build(VipsObject *object)
{
    VipsForeignSavePsd       *psd    = (VipsForeignSavePsd *)object;
    VipsForeignSavePsdTarget *target = (VipsForeignSavePsdTarget *)object;
    if (target->target) {
        psd->target = target->target;
        g_object_ref(psd->target);
    }
    return VIPS_OBJECT_CLASS(vips_foreign_save_psd_target_parent_class)->build(object);
}

static void vips_foreign_save_psd_target_class_init(VipsForeignSavePsdTargetClass *class)
{
    VipsObjectClass  *object_class  = (VipsObjectClass *)class;
    VipsForeignClass *foreign_class = (VipsForeignClass *)class;

    vips__foreign_psd_class_properties(class);
    object_class->nickname    = "psdsave_target";
    object_class->description = "save PSD and PSB to a target with minipsd";
    object_class->build       = vips_foreign_save_psd_target_build;
    foreign_class->suffs      = vips__foreign_psd_suffs;

    PSD_ARGS_BEGIN
    VIPS_ARG_OBJECT(class, "target", 1, "Target", "Target to save to", VIPS_ARGUMENT_REQUIRED_INPUT,
                    G_STRUCT_OFFSET(VipsForeignSavePsdTarget, target), VIPS_TYPE_TARGET);
    PSD_ARGS_END
}

static void vips_foreign_save_psd_target_init(VipsForeignSavePsdTarget *target)
{
    (void)target;
}

/* --- psdsave (a file) ---------------------------------------------------------------------- */

typedef struct _VipsForeignSavePsdFile {
    VipsForeignSavePsd parent_object;
    char              *filename;
} VipsForeignSavePsdFile;

typedef VipsForeignSavePsdClass VipsForeignSavePsdFileClass;

G_DEFINE_TYPE(VipsForeignSavePsdFile, vips_foreign_save_psd_file, vips_foreign_save_psd_get_type())

static int vips_foreign_save_psd_file_build(VipsObject *object)
{
    VipsForeignSavePsd     *psd  = (VipsForeignSavePsd *)object;
    VipsForeignSavePsdFile *file = (VipsForeignSavePsdFile *)object;
    if (file->filename && !(psd->target = vips_target_new_to_file(file->filename))) return -1;
    return VIPS_OBJECT_CLASS(vips_foreign_save_psd_file_parent_class)->build(object);
}

static void vips_foreign_save_psd_file_class_init(VipsForeignSavePsdFileClass *class)
{
    VipsObjectClass  *object_class  = (VipsObjectClass *)class;
    VipsForeignClass *foreign_class = (VipsForeignClass *)class;

    vips__foreign_psd_class_properties(class);
    object_class->nickname    = "psdsave";
    object_class->description = "save PSD and PSB to a file with minipsd";
    object_class->build       = vips_foreign_save_psd_file_build;
    foreign_class->suffs      = vips__foreign_psd_suffs;

    PSD_ARGS_BEGIN
    VIPS_ARG_STRING(class, "filename", 1, "Filename", "Filename to save to", VIPS_ARGUMENT_REQUIRED_INPUT,
                    G_STRUCT_OFFSET(VipsForeignSavePsdFile, filename), NULL);
    PSD_ARGS_END
}

static void vips_foreign_save_psd_file_init(VipsForeignSavePsdFile *file)
{
    (void)file;
}

/* --- psdsave_buffer ------------------------------------------------------------------------ */

typedef struct _VipsForeignSavePsdBuffer {
    VipsForeignSavePsd parent_object;
    VipsArea          *buf;
} VipsForeignSavePsdBuffer;

typedef VipsForeignSavePsdClass VipsForeignSavePsdBufferClass;

G_DEFINE_TYPE(VipsForeignSavePsdBuffer, vips_foreign_save_psd_buffer, vips_foreign_save_psd_get_type())

static int vips_foreign_save_psd_buffer_build(VipsObject *object)
{
    VipsForeignSavePsd *psd = (VipsForeignSavePsd *)object;
    VipsBlob           *blob;
    if (!(psd->target = vips_target_new_to_memory())) return -1;
    if (VIPS_OBJECT_CLASS(vips_foreign_save_psd_buffer_parent_class)->build(object)) return -1;
    g_object_get(psd->target, "blob", &blob, NULL);
    g_object_set(object, "buffer", blob, NULL);
    vips_area_unref(VIPS_AREA(blob));
    return 0;
}

static void vips_foreign_save_psd_buffer_class_init(VipsForeignSavePsdBufferClass *class)
{
    VipsObjectClass  *object_class  = (VipsObjectClass *)class;
    VipsForeignClass *foreign_class = (VipsForeignClass *)class;

    vips__foreign_psd_class_properties(class);
    object_class->nickname    = "psdsave_buffer";
    object_class->description = "save PSD and PSB to a buffer with minipsd";
    object_class->build       = vips_foreign_save_psd_buffer_build;
    foreign_class->suffs      = vips__foreign_psd_suffs;

    PSD_ARGS_BEGIN
    VIPS_ARG_BOXED(class, "buffer", 1, "Buffer", "Buffer to save to", VIPS_ARGUMENT_REQUIRED_OUTPUT,
                   G_STRUCT_OFFSET(VipsForeignSavePsdBuffer, buf), VIPS_TYPE_BLOB);
    PSD_ARGS_END
}

static void vips_foreign_save_psd_buffer_init(VipsForeignSavePsdBuffer *buffer)
{
    (void)buffer;
}
