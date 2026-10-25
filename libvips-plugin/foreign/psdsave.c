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
#include <stdarg.h>
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

/* The PSD mode of an image's bands: grey, RGB or CMYK (by its interpretation), each with or
   without alpha, its colour channels in *colors; -1 for any other band count. */
static int vips_foreign_save_psd_mode(const VipsImage *in, size_t *colors)
{
    if (in->Type == VIPS_INTERPRETATION_CMYK && (in->Bands == PSD_CMYK || in->Bands == PSD_CMYK + 1)) {
        *colors = PSD_CMYK;
        return MINIPSD_MODE_CMYK;
    }
    if (in->Bands == PSD_RGB || in->Bands == PSD_RGB + 1) {
        *colors = PSD_RGB;
        return MINIPSD_MODE_RGB;
    }
    if (in->Bands == 1 || in->Bands == 2) {
        *colors = 1;
        return MINIPSD_MODE_GREY;
    }
    return -1;
}

/* The PSD depth of an image's samples: 8, 16, 32 (floats); 0 for another format. */
static int vips_foreign_save_psd_depth(const VipsImage *in)
{
    return in->BandFmt == VIPS_FORMAT_UCHAR ? 8 : in->BandFmt == VIPS_FORMAT_USHORT ? 16 : in->BandFmt == VIPS_FORMAT_FLOAT ? 32 : 0;
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

/* One image read once into c, encoded by compression; a composite's joined as the file stores it.
   -1 on failure, *memory set when memory ran out (else the image's own error, reported). */
static int vips_foreign_save_psd_encode_image(VipsForeignSavePsdChannels *c, VipsImage *in, int depth, int mode, int compression,
                                              int composite, int matte, int *memory)
{
    *memory = 1;
    if (vips_foreign_save_psd_channels_init(c, in, depth, compression, composite, matte)) return -1;
    if (vips_foreign_save_psd_pass(in, depth, mode, &c, 1, memory)) return -1;
    *memory = 1;
    if (vips_foreign_save_psd_finish(c)) return -1;
    *memory = 0;
    return 0;
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
    size_t                      icc_size, colors = 0, count = 0;
    double                      ratio;
    unsigned                    n = 0, table;
    int                         rc, mode, alpha, layered, psb, memory = 0;

    if (VIPS_OBJECT_CLASS(vips_foreign_save_psd_parent_class)->build(object)) return -1;
    in = save->ready;

    /* The mode from the bands: grey, RGB or CMYK, each with or without alpha. */
    memset(&doc, 0, sizeof doc);
    if ((mode = vips_foreign_save_psd_mode(in, &colors)) < 0) {
        vips_error(VIPS_OBJECT_GET_CLASS(object)->nickname, "%s", "PSD takes grey, RGB or CMYK, each with or without alpha");
        return -1;
    }
    doc.color_mode = (uint16_t)mode;
    doc.depth      = (uint16_t)vips_foreign_save_psd_depth(in);   /* format_table made it one of the three */
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

/* --- psdsave_layers: a layered document from an array of images ----------------------------- */

/* A saver takes one image; a layered document is several, so this is an operation over an array
   of them, as vips_composite is.  Each image is a layer, bottom first, its name, opacity,
   visibility, blend mode and clipping in the psd-layer-* fields psdload sets, its position in
   xoffset and yoffset; psd-layer-empty makes it an empty layer (no pixels), as Photoshop writes
   one.  The composite is given, or made by vips_composite from the visible layers. */

#define PSD_BLEND_KEY_SIZE 4   /* a blend mode's key: four characters, such as "mul " */
#define PSD_COMPOSE_STEPS  9   /* the images vips_foreign_save_psd_compose makes for a layer */
#define PSD_MASK_DISABLED  0x02   /* a mask's flag: disabled */
#define PSD_DIVIDER_NAME   "</Layer group>"   /* a divider's name, as Photoshop names it */

typedef struct _VipsForeignSavePsdLayers {
    VipsOperation             parent_object;
    VipsTarget               *target;      /* set by the subclass's build */
    VipsArrayImage           *in;          /* the layers, bottom first */
    VipsImage                *composite;   /* the option: the composite, as given */
    VipsForeignPsdCompression compression;
    gboolean                  psb;
    int                       width, height;   /* the canvas: 0, from the composite or the layers */
    int                       left, top;       /* where the canvas lies in the layers' coordinates */
} VipsForeignSavePsdLayers;

typedef VipsOperationClass VipsForeignSavePsdLayersClass;

G_DEFINE_ABSTRACT_TYPE(VipsForeignSavePsdLayers, vips_foreign_save_psd_layers, VIPS_TYPE_OPERATION)

static void vips_foreign_save_psd_layers_dispose(GObject *gobject)
{
    VipsForeignSavePsdLayers *psd = (VipsForeignSavePsdLayers *)gobject;
    VIPS_UNREF(psd->target);
    G_OBJECT_CLASS(vips_foreign_save_psd_layers_parent_class)->dispose(gobject);
}

/* An int field of an image, or fallback without it. */
static int vips_foreign_save_psd_field(VipsImage *image, const char *name, int fallback)
{
    int value;
    return vips_image_get_typeof(image, name) && !vips_image_get_int(image, name, &value) ? value : fallback;
}

/* A record's MINIPSD_SECTION_* from its psd-layer-section: a layer without one; -1 for another
   text. */
static int vips_foreign_save_psd_section(VipsImage *image)
{
    const char *text;
    int         k;
    if (!vips_image_get_typeof(image, "psd-layer-section") || vips_image_get_string(image, "psd-layer-section", &text))
        return MINIPSD_SECTION_LAYER;
    for (k = MINIPSD_SECTION_GROUP; k < PSD_SECTIONS; ++k)
        if (strcmp(text, vips__foreign_psd_sections[k]) == 0) return k;
    return -1;
}

/* No pixels: an empty layer, or a group's record. */
static int vips_foreign_save_psd_empty(VipsImage *image)
{
    return vips_foreign_save_psd_field(image, "psd-layer-empty", 0) != 0 || vips_foreign_save_psd_section(image) != MINIPSD_SECTION_LAYER;
}

/* The image's mask (a new reference), or NULL without one. */
static VipsImage *vips_foreign_save_psd_mask(VipsImage *image)
{
    VipsImage *mask;
    return vips_image_get_typeof(image, "psd-layer-mask") && !vips_image_get_image(image, "psd-layer-mask", &mask) ? mask : NULL;
}

/* A PSD blend key as a libvips blend mode, as the spec's table maps them (Working with libvips);
   a key libvips has no counterpart for is drawn "over", as minipsd's own composer draws it. */
static VipsBlendMode vips_foreign_save_psd_blend_mode(const char key[PSD_BLEND_KEY_SIZE])
{
    static const struct {
        char          key[PSD_BLEND_KEY_SIZE + 1];
        VipsBlendMode mode;
    } table[] = {
        { "norm", VIPS_BLEND_MODE_OVER },        { "mul ", VIPS_BLEND_MODE_MULTIPLY },
        { "scrn", VIPS_BLEND_MODE_SCREEN },      { "over", VIPS_BLEND_MODE_OVERLAY },
        { "dark", VIPS_BLEND_MODE_DARKEN },      { "lite", VIPS_BLEND_MODE_LIGHTEN },
        { "lddg", VIPS_BLEND_MODE_ADD },         { "div ", VIPS_BLEND_MODE_COLOUR_DODGE },
        { "idiv", VIPS_BLEND_MODE_COLOUR_BURN }, { "hLit", VIPS_BLEND_MODE_HARD_LIGHT },
        { "sLit", VIPS_BLEND_MODE_SOFT_LIGHT },  { "diff", VIPS_BLEND_MODE_DIFFERENCE },
        { "smud", VIPS_BLEND_MODE_EXCLUSION },
    };
    size_t k;
    for (k = 0; k < sizeof table / sizeof table[0]; ++k)
        if (memcmp(key, table[k].key, PSD_BLEND_KEY_SIZE) == 0) return table[k].mode;
    return VIPS_BLEND_MODE_OVER;
}

/* Each layer's visibility and opacity (0 to 1) with its groups': a hidden group hides its layers,
   its opacity multiplies theirs.  Walked top down: a group's top record comes before its layers,
   its divider after them. */
static void vips_foreign_save_psd_groups(VipsImage **in, int n, int *hidden, double *opacity)
{
    int     *group_hidden = g_new0(int, (gsize)n + 1), level = 0, i;
    double  *group_opacity = g_new(double, (gsize)n + 1);
    group_opacity[0] = 1.0;
    for (i = n - 1; i >= 0; --i) {
        const int    section = vips_foreign_save_psd_section(in[i]);
        const int    own     = vips_foreign_save_psd_field(in[i], "psd-layer-hidden", 0) != 0;
        const double alpha   = VIPS_CLIP(0, vips_foreign_save_psd_field(in[i], "psd-layer-opacity", UCHAR_MAX), UCHAR_MAX) / (double)UCHAR_MAX;
        hidden[i]  = group_hidden[level] || own;
        opacity[i] = group_opacity[level] * alpha;
        if (section == MINIPSD_SECTION_GROUP || section == MINIPSD_SECTION_GROUP_CLOSED) {
            ++level;
            group_hidden[level]  = hidden[i];
            group_opacity[level] = opacity[i];
        } else if (section == MINIPSD_SECTION_GROUP_END && level > 0) {
            --level;
        }
    }
    g_free(group_hidden);
    g_free(group_opacity);
}

/* The visible layers composed by vips_composite onto a transparent canvas of width x height, at
   their positions less (left, top), their opacity (with their groups') and their mask in their
   alpha, their blend modes mapped; with no layer's transparency, flattened against white (as
   minipsd mattes a composite without it).  Its bands and format are the layers'.  Clipping, a
   group's blend mode and mask, and a blend mode libvips lacks are drawn as normal (pass through
   for a group).  NULL with an error. */
static VipsImage *vips_foreign_save_psd_compose(VipsForeignSavePsdLayers *psd, VipsImage **in, int n, const VipsImage *ref,
                                                size_t colors, int mode, int depth, int width, int height, int alpha)
{
    const double   max   = depth == 32 ? 1.0 : depth == 16 ? (double)UINT16_MAX : (double)UCHAR_MAX;
    const int      bands = (int)colors + 1;
    VipsImage    **t     = (VipsImage **)vips_object_local_array(VIPS_OBJECT(psd), PSD_COMPOSE_STEPS * n + 3);
    VipsImage    **layers = (VipsImage **)vips_object_local_array(VIPS_OBJECT(psd), n + 1);   /* the canvas, then each */
    int           *modes = (int *)vips_malloc(VIPS_OBJECT(psd), (size_t)(n + 1) * sizeof(int));
    int           *xs    = (int *)vips_malloc(VIPS_OBJECT(psd), (size_t)(n + 1) * sizeof(int));
    int           *ys    = (int *)vips_malloc(VIPS_OBJECT(psd), (size_t)(n + 1) * sizeof(int));
    int           *hidden  = (int *)vips_malloc(VIPS_OBJECT(psd), (size_t)n * sizeof(int));
    double        *opacity = (double *)vips_malloc(VIPS_OBJECT(psd), (size_t)n * sizeof(double));
    double         white[PSD_MAX_PLANES];
    VipsImage     *out, *composed = NULL;
    int            count = 0, i, k, rc;

    if (!modes || !xs || !ys || !hidden || !opacity) return NULL;
    vips_foreign_save_psd_groups(in, n, hidden, opacity);
    if (mode == MINIPSD_MODE_CMYK && depth != 8) {   /* libvips takes CMYK's alpha as 8-bit */
        vips_error(VIPS_OBJECT_GET_CLASS(psd)->nickname, "%s", "a 16-bit CMYK document needs its composite given");
        return NULL;
    }
    /* The canvas: transparent, the document's size, as the layers are. */
    if (vips_black(&t[0], width, height, "bands", bands, NULL) || vips_cast(t[0], &t[1], ref->BandFmt, NULL)
        || vips_copy(t[1], &layers[0], "interpretation", ref->Type, NULL))
        return NULL;
    ++count;
    for (i = 0; i < n; ++i) {   /* bottom first, as minipsd and vips_composite both list them */
        VipsImage  *layer = in[i], *alpha_band = NULL, *mask;
        VipsImage **s     = t + 2 + PSD_COMPOSE_STEPS * i;   /* this layer's steps */
        char        key[PSD_BLEND_KEY_SIZE] = { 'n', 'o', 'r', 'm' };
        const char *given;
        if (vips_foreign_save_psd_empty(layer) || hidden[i]) continue;
        if (layer->Bands == (int)colors) {   /* without transparency: opaque */
            if (vips_bandjoin_const1(layer, &s[0], max, NULL)) return NULL;
            layer = s[0];
        }
        /* The mask over the layer's rectangle, its colour outside its own. */
        if ((mask = vips_foreign_save_psd_mask(in[i])) && !vips_foreign_save_psd_field(in[i], "psd-layer-mask-disabled", 0)) {
            const double outside = vips_foreign_save_psd_field(in[i], "psd-layer-mask-default-color", UCHAR_MAX) ? max : 0.0;
            if (vips_foreign_save_psd_empty(mask))
                rc = vips_black(&s[1], layer->Xsize, layer->Ysize, NULL) || vips_linear1(s[1], &s[2], 1.0, outside, NULL);
            else {
                VipsArrayDouble *background = vips_array_double_new(&outside, 1);
                rc = vips_embed(mask, &s[2], mask->Xoffset - in[i]->Xoffset, mask->Yoffset - in[i]->Yoffset, layer->Xsize, layer->Ysize,
                                "extend", VIPS_EXTEND_BACKGROUND, "background", background, NULL);
                vips_area_unref(VIPS_AREA(background));
            }
            if (!rc) rc = vips_extract_band(layer, &s[3], (int)colors, NULL) || vips_multiply(s[3], s[2], &s[4], NULL);
            g_object_unref(mask);
            if (rc) return NULL;
            alpha_band = s[4];
            opacity[i] /= max;   /* the mask's scale */
        } else {
            VIPS_UNREF(mask);
            if (opacity[i] < 1.0) {
                if (vips_extract_band(layer, &s[3], (int)colors, NULL)) return NULL;
                alpha_band = s[3];
            }
        }
        if (alpha_band) {   /* the opacity (and the mask) in the alpha_band */
            if (vips_linear1(alpha_band, &s[5], opacity[i], 0.0, NULL) || vips_extract_band(layer, &s[6], 0, "n", (int)colors, NULL)
                || vips_bandjoin2(s[6], s[5], &s[7], NULL))
                return NULL;
            layer = s[7];
        }
        if (vips_copy(layer, &s[8], "interpretation", ref->Type, NULL)) return NULL;
        layers[count] = s[8];
        g_object_ref(layers[count]);   /* both arrays drop theirs */
        if (vips_image_get_typeof(in[i], "psd-layer-blend-mode") && !vips_image_get_string(in[i], "psd-layer-blend-mode", &given)
            && strlen(given) == PSD_BLEND_KEY_SIZE)
            memcpy(key, given, PSD_BLEND_KEY_SIZE);
        modes[count - 1] = (int)vips_foreign_save_psd_blend_mode(key);
        xs[count - 1]    = in[i]->Xoffset - psd->left;   /* where the layer goes; it may reach past the canvas */
        ys[count - 1]    = in[i]->Yoffset - psd->top;
        ++count;
    }
    if (count == 1) {   /* nothing visible: the transparent canvas */
        composed = layers[0];
        g_object_ref(composed);
    } else {
        VipsArrayInt *x = vips_array_int_new(xs, count - 1), *y = vips_array_int_new(ys, count - 1);
        rc = vips_composite(layers, &composed, count, modes, "x", x, "y", y, "compositing_space", ref->Type, NULL);
        vips_area_unref(VIPS_AREA(x));
        vips_area_unref(VIPS_AREA(y));
        if (rc) return NULL;
    }
    if (!alpha) {   /* no transparency anywhere: against white, as minipsd mattes; CMYK's white is no ink */
        VipsArrayDouble *background;
        for (k = 0; k < (int)colors; ++k) white[k] = mode == MINIPSD_MODE_CMYK ? 0.0 : max;
        background = vips_array_double_new(white, (int)colors);
        rc = vips_flatten(composed, &out, "background", background, "max_alpha", max, NULL);
        vips_area_unref(VIPS_AREA(background));
        g_object_unref(composed);
        if (rc) return NULL;
        composed = out;
    }
    rc = vips_cast(composed, &t[2 + PSD_COMPOSE_STEPS * n], ref->BandFmt, NULL);   /* vips_composite may give a wider format */
    g_object_unref(composed);
    if (rc || vips_copy(t[2 + PSD_COMPOSE_STEPS * n], &out, "interpretation", ref->Type, NULL)) return NULL;
    return out;
}

static int vips_foreign_save_psd_layers_build(VipsObject *object)
{
    VipsForeignSavePsdLayers   *psd      = (VipsForeignSavePsdLayers *)object;
    const char                 *nickname = VIPS_OBJECT_GET_CLASS(object)->nickname;
    VipsImage                 **in, *ref = NULL, *composite = NULL;
    VipsForeignSavePsdChannels *channels = NULL, composite_channels;
    minipsd_layer              *layers   = NULL;
    minipsd_mask               *masks    = NULL;
    VipsImage                 **mask_images = NULL;
    minipsd_bytes              *stored   = NULL;
    char                      **names    = NULL;
    minipsd_resource            resources[PSD_MAX_RESOURCES];
    minipsd_doc                 doc;
    const void                 *icc;
    const char                 *filename, *error = NULL, *text;
    size_t                      colors = 0, icc_size, count = 0, longest = 0;
    unsigned                    table;
    int                         n, i, mode, depth, compression, section, width, height, alpha = 0, psb, memory = 0, rc = MINIPSD_OK,
                                failed = 0;

    if (VIPS_OBJECT_CLASS(vips_foreign_save_psd_layers_parent_class)->build(object)) return -1;
    in = vips_array_image_get(psd->in, &n);
    if (n < 1) {
        vips_error(nickname, "%s", "no layers");
        return -1;
    }

    /* The mode and depth: the first layer with pixels decides, else the composite, else the first
       layer; every other image must agree. */
    for (i = 0; i < n && !ref; ++i)
        if (!vips_foreign_save_psd_empty(in[i])) ref = in[i];
    if (!ref) ref = psd->composite ? psd->composite : in[0];
    mode  = vips_foreign_save_psd_mode(ref, &colors);
    depth = vips_foreign_save_psd_depth(ref);
    if (mode < 0 || !depth) {
        vips_error(nickname, "%s", "PSD takes grey, RGB or CMYK, each with or without alpha, as uchar, ushort or float");
        return -1;
    }
    for (i = 0; i < n; ++i) {
        if (vips_foreign_save_psd_section(in[i]) < 0) {
            vips_error(nickname, "layer %d: psd-layer-section is not \"open\", \"closed\" or \"end\"", i);
            return -1;
        }
        if (vips_foreign_save_psd_empty(in[i])) continue;
        if (vips_foreign_save_psd_depth(in[i]) != depth || (in[i]->Bands != (int)colors && in[i]->Bands != (int)colors + 1)
            || (mode == MINIPSD_MODE_CMYK) != (in[i]->Type == VIPS_INTERPRETATION_CMYK)) {
            vips_error(nickname, "layer %d: not the mode or depth of the others (no layer is converted)", i);
            return -1;
        }
        alpha |= in[i]->Bands > (int)colors;
    }

    /* The canvas: the options; else the composite's size; else from (left, top), every layer in it. */
    width  = psd->width;
    height = psd->height;
    if (width <= 0 || height <= 0) {
        if (psd->composite) {
            width  = psd->composite->Xsize;
            height = psd->composite->Ysize;
        } else {
            for (i = 0, width = height = 0; i < n; ++i)
                if (!vips_foreign_save_psd_empty(in[i])) {
                    width  = VIPS_MAX(width, in[i]->Xoffset + in[i]->Xsize - psd->left);
                    height = VIPS_MAX(height, in[i]->Yoffset + in[i]->Ysize - psd->top);
                }
        }
    }
    if (width <= 0 || height <= 0) {
        vips_error(nickname, "%s", "no canvas: give width and height, or the composite");
        return -1;
    }
    if (psd->composite
        && (psd->composite->Xsize != width || psd->composite->Ysize != height || vips_foreign_save_psd_depth(psd->composite) != depth
            || (psd->composite->Bands != (int)colors && psd->composite->Bands != (int)colors + 1))) {
        vips_error(nickname, "%s", "the composite is not the canvas's size, or not the layers' mode and depth");
        return -1;
    }

    /* Each layer read once, its channels encoded as its rows come, then its mask; then the
       composite.  channels holds the layers', then the masks'. */
    memset(&composite_channels, 0, sizeof composite_channels);
    compression = vips_foreign_save_psd_layer_compression(psd->compression);
    channels    = g_new0(VipsForeignSavePsdChannels, 2 * (gsize)n);
    layers      = g_new0(minipsd_layer, (gsize)n);
    masks       = g_new0(minipsd_mask, (gsize)n);
    mask_images = g_new0(VipsImage *, (gsize)n);
    stored   = g_new0(minipsd_bytes, (gsize)n * PSD_MAX_PLANES);
    names    = g_new0(char *, (gsize)n);
    for (i = 0; i < n && !failed; ++i) {
        minipsd_layer *l    = &layers[i];
        VipsImage     *mask = mask_images[i] = vips_foreign_save_psd_mask(in[i]);
        section             = vips_foreign_save_psd_section(in[i]);
        if (vips_image_get_typeof(in[i], "psd-layer-name") && !vips_image_get_string(in[i], "psd-layer-name", &text))
            l->name = text;
        else
            l->name = section == MINIPSD_SECTION_GROUP_END ? PSD_DIVIDER_NAME : (names[i] = g_strdup_printf("Layer %d", i + 1));
        l->section_divider_type = (uint8_t)section;
        l->opacity  = (uint8_t)VIPS_CLIP(0, vips_foreign_save_psd_field(in[i], "psd-layer-opacity", UCHAR_MAX), UCHAR_MAX);
        l->hidden   = (uint8_t)(vips_foreign_save_psd_field(in[i], "psd-layer-hidden", 0) != 0);
        l->clipping = (uint8_t)(vips_foreign_save_psd_field(in[i], "psd-layer-clipping", 0) != 0);
        if (vips_image_get_typeof(in[i], "psd-layer-blend-mode") && !vips_image_get_string(in[i], "psd-layer-blend-mode", &text)) {
            if (strlen(text) != PSD_BLEND_KEY_SIZE) {
                error  = "a psd-layer-blend-mode is not a four-character key, such as \"mul \"";
                failed = 1;
                break;
            }
            /* A group passing through: none given, which minipsd writes as Photoshop does. */
            if (section == MINIPSD_SECTION_LAYER || section == MINIPSD_SECTION_GROUP_END || memcmp(text, "pass", PSD_BLEND_KEY_SIZE) != 0)
                memcpy(l->blend_mode, text, PSD_BLEND_KEY_SIZE);
        }
        if (mask) {   /* one band of the layers' depth, its own rectangle; encoded as the layer is */
            if (mask->Bands != 1 || vips_foreign_save_psd_depth(mask) != depth) {
                error  = "a psd-layer-mask is not one band of the layers' depth";
                failed = 1;
                break;
            }
            masks[i].default_color = vips_foreign_save_psd_field(in[i], "psd-layer-mask-default-color", UCHAR_MAX) ? UCHAR_MAX : 0;
            masks[i].flags         = vips_foreign_save_psd_field(in[i], "psd-layer-mask-disabled", 0) ? PSD_MASK_DISABLED : 0;
            l->mask                = &masks[i];
            if (!vips_foreign_save_psd_empty(mask)) {
                masks[i].top    = mask->Yoffset - psd->top;
                masks[i].left   = mask->Xoffset - psd->left;
                masks[i].bottom = masks[i].top + mask->Ysize;
                masks[i].right  = masks[i].left + mask->Xsize;
                if (vips_foreign_save_psd_encode_image(&channels[n + i], mask, depth, MINIPSD_MODE_GREY, compression, 0, 0, &memory)) {
                    failed = 1;
                    break;
                }
                longest = VIPS_MAX(longest, channels[n + i].longest);
            }
        }
        if (vips_foreign_save_psd_empty(in[i])) continue;   /* no pixels, no rectangle: as Photoshop writes one */
        l->top      = in[i]->Yoffset - psd->top;
        l->left     = in[i]->Xoffset - psd->left;
        l->bottom   = l->top + in[i]->Ysize;
        l->right    = l->left + in[i]->Xsize;
        l->no_alpha = (uint8_t)(in[i]->Bands == (int)colors);
        l->stored   = &stored[(size_t)i * PSD_MAX_PLANES];
        if (vips_foreign_save_psd_encode_image(&channels[i], in[i], depth, mode, compression, 0, 0, &memory)) failed = 1;
        longest = VIPS_MAX(longest, channels[i].longest);
    }
    if (!failed) {
        composite = psd->composite ? psd->composite : vips_foreign_save_psd_compose(psd, in, n, ref, colors, mode, depth, width, height, alpha);
        if (!composite) failed = 1;
    }
    if (!failed) {
        if (vips_foreign_save_psd_encode_image(&composite_channels, composite, depth, mode,
                                               psd->compression == VIPS_FOREIGN_PSD_COMPRESSION_NONE ? MINIPSD_RAW : MINIPSD_RLE, 1,
                                               composite->Bands > (int)colors, &memory))
            failed = 1;
        longest = VIPS_MAX(longest, composite_channels.longest);
    }

    if (!failed) {
        memset(&doc, 0, sizeof doc);
        doc.color_mode         = (uint16_t)mode;
        doc.depth              = (uint16_t)depth;
        doc.width              = (uint32_t)width;
        doc.height             = (uint32_t)height;
        doc.dpi                = ref->Xres * MM_PER_INCH;
        if (!(doc.dpi > 0.0 && doc.dpi < PSD_MAX_DPI)) doc.dpi = PSD_DEFAULT_DPI;
        doc.layers             = layers;
        doc.layer_count        = (size_t)n;
        doc.composite_channels = (uint16_t)composite->Bands;
        doc.composite_alpha    = (uint8_t)(composite->Bands > (int)colors);
        doc.compression        = MINIPSD_RLE;
        if (vips_image_get_typeof(ref, VIPS_META_ICC_NAME) && !vips_image_get_blob(ref, VIPS_META_ICC_NAME, &icc, &icc_size)) {
            doc.icc      = (const uint8_t *)icc;
            doc.icc_size = icc_size;
        }
        vips_foreign_save_psd_keep_blob(ref, VIPS_META_EXIF_NAME, MINIPSD_RES_EXIF_DATA_1, resources, &count);
        vips_foreign_save_psd_keep_blob(ref, VIPS_META_XMP_NAME, MINIPSD_RES_XMP_METADATA, resources, &count);
        vips_foreign_save_psd_keep_blob(ref, VIPS_META_IPTC_NAME, MINIPSD_RES_IPTC_NAA, resources, &count);
        doc.resources      = resources;
        doc.resource_count = count;

        /* PSB as psdsave chooses it. */
        filename = vips_connection_filename(VIPS_CONNECTION(psd->target));
        psb      = psd->psb || (filename && vips_iscasepostfix(filename, ".psb")) || width > MINIPSD_MAX_SIDE
              || height > MINIPSD_MAX_SIDE || longest > UINT16_MAX;
        for (;;) {
            table      = psb ? PSD_COUNT_PSB : PSD_COUNT_PSD;
            doc.format = psb ? MINIPSD_PSB : MINIPSD_PSD;
            for (i = 0; i < n; ++i) {
                if (layers[i].stored) vips_foreign_save_psd_layer_blocks(&channels[i], table, stored + (size_t)i * PSD_MAX_PLANES);
                if (channels[n + i].planes) vips_foreign_save_psd_layer_blocks(&channels[n + i], table, &masks[i].stored);
            }
            doc.composite_stored = vips_foreign_save_psd_composite_block(&composite_channels, table);
            if (psb || vips_foreign_save_psd_fits_psd(&doc)) break;
            psb = 1;
        }
        rc = minipsd_write(&doc, NULL, vips_foreign_save_psd_to_target, psd->target);
    }

    for (i = 0; channels && i < 2 * n; ++i) vips_foreign_save_psd_channels_free(&channels[i]);
    for (i = 0; mask_images && i < n; ++i) VIPS_UNREF(mask_images[i]);
    g_free(mask_images);
    g_free(masks);
    vips_foreign_save_psd_channels_free(&composite_channels);
    for (i = 0; names && i < n; ++i) g_free(names[i]);
    g_free(names);
    g_free(stored);
    g_free(layers);
    g_free(channels);
    if (composite && composite != psd->composite) g_object_unref(composite);
    if (error) vips_error(nickname, "%s", error);
    else if (memory) vips_error(nickname, "%s", minipsd_error_string(MINIPSD_ERR_ALLOC));
    else if (rc != MINIPSD_OK) vips_error(nickname, "%s", minipsd_error_string(rc));
    if (failed || rc != MINIPSD_OK) return -1;   /* an image's own error is reported where it arose */
    return vips_target_end(psd->target);
}

static void vips_foreign_save_psd_layers_class_init(VipsForeignSavePsdLayersClass *class)
{
    GObjectClass       *gobject_class   = G_OBJECT_CLASS(class);
    VipsObjectClass    *object_class    = (VipsObjectClass *)class;
    VipsOperationClass *operation_class = (VipsOperationClass *)class;

    gobject_class->dispose = vips_foreign_save_psd_layers_dispose;
    vips__foreign_psd_class_properties(class);

    object_class->nickname    = "psdsave_layers_base";
    object_class->description = "save images as the layers of a PSD or PSB with minipsd";
    object_class->build       = vips_foreign_save_psd_layers_build;
    operation_class->flags |= VIPS_OPERATION_NOCACHE;   /* it writes: never answered from the cache */

    PSD_ARGS_BEGIN
    VIPS_ARG_BOXED(class, "in", 0, "Input", "The layers, bottom first", VIPS_ARGUMENT_REQUIRED_INPUT,
                   G_STRUCT_OFFSET(VipsForeignSavePsdLayers, in), VIPS_TYPE_ARRAY_IMAGE);
    VIPS_ARG_IMAGE(class, "composite", 20, "Composite", "The composite, as given; else composed by vips_composite",
                   VIPS_ARGUMENT_OPTIONAL_INPUT, G_STRUCT_OFFSET(VipsForeignSavePsdLayers, composite));
    VIPS_ARG_ENUM(class, "compression", 21, "Compression", "The layers' compression: rle, zip, zip-predicted, or none (the composite too)",
                  VIPS_ARGUMENT_OPTIONAL_INPUT, G_STRUCT_OFFSET(VipsForeignSavePsdLayers, compression),
                  vips_foreign_psd_compression_get_type(), VIPS_FOREIGN_PSD_COMPRESSION_RLE);
    VIPS_ARG_BOOL(class, "psb", 22, "PSB", "Save as PSB (also for a .psb file, or when a PSD cannot hold it)",
                  VIPS_ARGUMENT_OPTIONAL_INPUT, G_STRUCT_OFFSET(VipsForeignSavePsdLayers, psb), FALSE);
    VIPS_ARG_INT(class, "width", 23, "Width", "The canvas's width; 0: the composite's, or every layer's",
                 VIPS_ARGUMENT_OPTIONAL_INPUT, G_STRUCT_OFFSET(VipsForeignSavePsdLayers, width), 0, MINIPSD_MAX_SIDE_PSB, 0);
    VIPS_ARG_INT(class, "height", 24, "Height", "The canvas's height; 0: the composite's, or every layer's",
                 VIPS_ARGUMENT_OPTIONAL_INPUT, G_STRUCT_OFFSET(VipsForeignSavePsdLayers, height), 0, MINIPSD_MAX_SIDE_PSB, 0);
    VIPS_ARG_INT(class, "left", 25, "Left", "The canvas's left edge in the layers' coordinates",
                 VIPS_ARGUMENT_OPTIONAL_INPUT, G_STRUCT_OFFSET(VipsForeignSavePsdLayers, left), -MINIPSD_MAX_SIDE_PSB,
                 MINIPSD_MAX_SIDE_PSB, 0);
    VIPS_ARG_INT(class, "top", 26, "Top", "The canvas's top edge in the layers' coordinates",
                 VIPS_ARGUMENT_OPTIONAL_INPUT, G_STRUCT_OFFSET(VipsForeignSavePsdLayers, top), -MINIPSD_MAX_SIDE_PSB,
                 MINIPSD_MAX_SIDE_PSB, 0);
    PSD_ARGS_END
}

static void vips_foreign_save_psd_layers_init(VipsForeignSavePsdLayers *psd)
{
    (void)psd;
}

/* --- psdsave_layers_target, psdsave_layers (a file), psdsave_layers_buffer ------------------ */

typedef struct _VipsForeignSavePsdLayersTarget {
    VipsForeignSavePsdLayers parent_object;
    VipsTarget              *target;
} VipsForeignSavePsdLayersTarget;

typedef VipsForeignSavePsdLayersClass VipsForeignSavePsdLayersTargetClass;

G_DEFINE_TYPE(VipsForeignSavePsdLayersTarget, vips_foreign_save_psd_layers_target, vips_foreign_save_psd_layers_get_type())

static int vips_foreign_save_psd_layers_target_build(VipsObject *object)
{
    VipsForeignSavePsdLayers       *psd    = (VipsForeignSavePsdLayers *)object;
    VipsForeignSavePsdLayersTarget *target = (VipsForeignSavePsdLayersTarget *)object;
    if (target->target) {
        psd->target = target->target;
        g_object_ref(psd->target);
    }
    return VIPS_OBJECT_CLASS(vips_foreign_save_psd_layers_target_parent_class)->build(object);
}

static void vips_foreign_save_psd_layers_target_class_init(VipsForeignSavePsdLayersTargetClass *class)
{
    VipsObjectClass *object_class = (VipsObjectClass *)class;

    vips__foreign_psd_class_properties(class);
    object_class->nickname    = "psdsave_layers_target";
    object_class->description = "save images as the layers of a PSD or PSB to a target with minipsd";
    object_class->build       = vips_foreign_save_psd_layers_target_build;

    PSD_ARGS_BEGIN
    VIPS_ARG_OBJECT(class, "target", 1, "Target", "Target to save to", VIPS_ARGUMENT_REQUIRED_INPUT,
                    G_STRUCT_OFFSET(VipsForeignSavePsdLayersTarget, target), VIPS_TYPE_TARGET);
    PSD_ARGS_END
}

static void vips_foreign_save_psd_layers_target_init(VipsForeignSavePsdLayersTarget *target)
{
    (void)target;
}

typedef struct _VipsForeignSavePsdLayersFile {
    VipsForeignSavePsdLayers parent_object;
    char                    *filename;
} VipsForeignSavePsdLayersFile;

typedef VipsForeignSavePsdLayersClass VipsForeignSavePsdLayersFileClass;

G_DEFINE_TYPE(VipsForeignSavePsdLayersFile, vips_foreign_save_psd_layers_file, vips_foreign_save_psd_layers_get_type())

static int vips_foreign_save_psd_layers_file_build(VipsObject *object)
{
    VipsForeignSavePsdLayers     *psd  = (VipsForeignSavePsdLayers *)object;
    VipsForeignSavePsdLayersFile *file = (VipsForeignSavePsdLayersFile *)object;
    if (file->filename && !(psd->target = vips_target_new_to_file(file->filename))) return -1;
    return VIPS_OBJECT_CLASS(vips_foreign_save_psd_layers_file_parent_class)->build(object);
}

static void vips_foreign_save_psd_layers_file_class_init(VipsForeignSavePsdLayersFileClass *class)
{
    VipsObjectClass *object_class = (VipsObjectClass *)class;

    vips__foreign_psd_class_properties(class);
    object_class->nickname    = "psdsave_layers";
    object_class->description = "save images as the layers of a PSD or PSB to a file with minipsd";
    object_class->build       = vips_foreign_save_psd_layers_file_build;

    PSD_ARGS_BEGIN
    VIPS_ARG_STRING(class, "filename", 1, "Filename", "Filename to save to", VIPS_ARGUMENT_REQUIRED_INPUT,
                    G_STRUCT_OFFSET(VipsForeignSavePsdLayersFile, filename), NULL);
    PSD_ARGS_END
}

static void vips_foreign_save_psd_layers_file_init(VipsForeignSavePsdLayersFile *file)
{
    (void)file;
}

typedef struct _VipsForeignSavePsdLayersBuffer {
    VipsForeignSavePsdLayers parent_object;
    VipsArea                *buf;
} VipsForeignSavePsdLayersBuffer;

typedef VipsForeignSavePsdLayersClass VipsForeignSavePsdLayersBufferClass;

G_DEFINE_TYPE(VipsForeignSavePsdLayersBuffer, vips_foreign_save_psd_layers_buffer, vips_foreign_save_psd_layers_get_type())

static int vips_foreign_save_psd_layers_buffer_build(VipsObject *object)
{
    VipsForeignSavePsdLayers *psd = (VipsForeignSavePsdLayers *)object;
    VipsBlob                 *blob;
    if (!(psd->target = vips_target_new_to_memory())) return -1;
    if (VIPS_OBJECT_CLASS(vips_foreign_save_psd_layers_buffer_parent_class)->build(object)) return -1;
    g_object_get(psd->target, "blob", &blob, NULL);
    g_object_set(object, "buffer", blob, NULL);
    vips_area_unref(VIPS_AREA(blob));
    return 0;
}

static void vips_foreign_save_psd_layers_buffer_class_init(VipsForeignSavePsdLayersBufferClass *class)
{
    VipsObjectClass *object_class = (VipsObjectClass *)class;

    vips__foreign_psd_class_properties(class);
    object_class->nickname    = "psdsave_layers_buffer";
    object_class->description = "save images as the layers of a PSD or PSB to a buffer with minipsd";
    object_class->build       = vips_foreign_save_psd_layers_buffer_build;

    PSD_ARGS_BEGIN
    VIPS_ARG_BOXED(class, "buffer", 1, "Buffer", "Buffer to save to", VIPS_ARGUMENT_REQUIRED_OUTPUT,
                   G_STRUCT_OFFSET(VipsForeignSavePsdLayersBuffer, buf), VIPS_TYPE_BLOB);
    PSD_ARGS_END
}

static void vips_foreign_save_psd_layers_buffer_init(VipsForeignSavePsdLayersBuffer *buffer)
{
    (void)buffer;
}

/* --- The C calls, as libvips has one beside each operation ---------------------------------- */

int vips_psdsave_layers(VipsImage **in, int n, const char *filename, ...)
{
    VipsArrayImage *array = vips_array_image_new(in, n);
    va_list         ap;
    int             result;
    va_start(ap, filename);
    result = vips_call_split("psdsave_layers", ap, array, filename);
    va_end(ap);
    vips_area_unref(VIPS_AREA(array));
    return result;
}

int vips_psdsave_layers_target(VipsImage **in, int n, VipsTarget *target, ...)
{
    VipsArrayImage *array = vips_array_image_new(in, n);
    va_list         ap;
    int             result;
    va_start(ap, target);
    result = vips_call_split("psdsave_layers_target", ap, array, target);
    va_end(ap);
    vips_area_unref(VIPS_AREA(array));
    return result;
}

int vips_psdsave_layers_buffer(VipsImage **in, int n, void **buf, size_t *len, ...)
{
    VipsArrayImage *array = vips_array_image_new(in, n);
    VipsArea       *area  = NULL;
    va_list         ap;
    int             result;
    va_start(ap, len);
    result = vips_call_split("psdsave_layers_buffer", ap, array, &area);
    va_end(ap);
    vips_area_unref(VIPS_AREA(array));
    if (!result && area) {   /* the bytes handed over, as libvips' own _buffer calls do */
        if (buf) {
            *buf          = area->data;
            area->free_fn = NULL;
        }
        if (len) *len = area->length;
        vips_area_unref(area);
    }
    return result;
}
