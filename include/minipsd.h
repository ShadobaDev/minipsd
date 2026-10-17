/**
 * \file
 * \brief A small, dependency-free Photoshop (PSD) writer and reader.  Header-only, C99, also valid C++.
 *
 * This file is the whole public API: every type, constant and function a caller can use.  The
 * implementation lives in minipsd/ and comes in only with MINIPSD_IMPLEMENTATION; copy the whole
 * include/ directory.  Sections:
 * - Version and linkage
 * - Return codes
 * - Workspace
 * - Document
 * - Writer
 * - Reader
 *
 * \par Usage
 * In exactly one C or C++ file:
 * \code
 *     #define MINIPSD_IMPLEMENTATION
 *     #include "minipsd.h"
 * \endcode
 * Everywhere else include it plainly.
 *
 * \code
 *     minipsd_layer layers[2] = {
 *         { .name = "Template", .rgba = rgba, .opacity = 255 },   // bottom, the full canvas
 *         { .name = "Layer", .opacity = 255 },                    // empty, above it
 *     };
 *     minipsd_doc doc = { .width = width, .height = height, .dpi = 300.0, .layers = layers, .layer_count = 2 };
 *     int rc = minipsd_write(&doc, file_out, f);   // file_out: fwrite to f, see minipsd_write_fn
 *     if (rc != MINIPSD_OK) puts(minipsd_error_string(rc));
 * \endcode
 *
 * \par Configuration
 * Define before the implementation include:
 * - \c MINIPSD_STATIC: make every function static (private to that file);
 * - \c MINIPSD_MALLOC / \c MINIPSD_FREE: replace malloc / free (define both);
 * - \c MINIPSD_NO_MALLOC: leave out every allocation: minipsd_write goes (and <stdlib.h>); the
 *   pull writer, on the caller's memory, stays.
 *
 * minipsd does no I/O of its own and includes no <stdio.h>: files, pipes and sockets are the
 * caller's, fed by minipsd_write or minipsd_writer_fill (examples/ in the repository has each).
 * \c MINIPSD_NO_STDIO, which left out a file writer until 0.2.0, is still accepted and does
 * nothing.
 *
 * \par What it writes
 * An 8-bit RGB document (PSD version 1) with a transparency channel:
 * - one or more layers, bottom first, each a rectangle anywhere on or off the canvas (by default
 *   the full canvas) or empty, with a name, opacity and visibility; names go out both as a legacy
 *   Pascal string (ASCII) and as Unicode ('luni');
 * - the composite (merged) image, which is what a reader that ignores layers shows: the caller's
 *   own render (minipsd_doc.composite), the right way, since flattening layers is rendering; or,
 *   without one, a best effort with no guarantee: the visible layers blended with straight-alpha
 *   "over" at their rectangles, clipped to the canvas.  Its transparency rides as the first extra
 *   channel, flagged by a negative layer count, and its colour is matted against white
 *   (c * a + 255 * (1 - a)), as Photoshop stores it and readers expect to undo;
 * - the resolution, in pixels per inch (resource 1005);
 * - an optional ICC profile (resource 1039); without one, readers assume sRGB.
 *
 * Channel data is PackBits RLE by default (small for flat art), or raw.
 *
 * Not written: 16/32-bit, other colour modes, masks, groups, blend modes other than normal, PSB.
 * PSD caps each side at 30000 px; larger documents are refused (MINIPSD_ERR_SIZE).
 *
 * \par Output
 * Bytes come out in order, never seeking back, so a pipe or a socket works as well as a file:
 * pulled by the caller into its own buffer (minipsd_writer_fill), or pushed to a callback
 * (minipsd_write).  The pull writer allocates nothing; its workspace, from the caller, holds a
 * size table (layer_count * 4 numbers), the composite's row sizes and one composite row.
 * minipsd_write allocates that workspace once.  Layer pixels and a supplied composite are read
 * in place, never copied whole.
 *
 * \par Project
 * https://github.com/ShadobaDev/minipsd
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MINIPSD_H
#define MINIPSD_H

#include <stddef.h>
#include <stdint.h>

/* --- Version and linkage ------------------------------------------------------------------- */

/**
 * \brief The version of minipsd, MAJOR.MINOR.PATCH.
 *
 * The single source of the project version: CMakeLists.txt reads these three lines.
 */
#define MINIPSD_VERSION_MAJOR 0
#define MINIPSD_VERSION_MINOR 2
#define MINIPSD_VERSION_PATCH 0

#define MPSD__STR(x)  #x
#define MPSD__XSTR(x) MPSD__STR(x)
/** \brief The version as a string literal, e.g. "0.2.0". */
#define MINIPSD_VERSION_STRING \
    MPSD__XSTR(MINIPSD_VERSION_MAJOR) "." MPSD__XSTR(MINIPSD_VERSION_MINOR) "." MPSD__XSTR(MINIPSD_VERSION_PATCH)

/** \brief Linkage of every public function: static under MINIPSD_STATIC, otherwise extern (C). */
#ifdef MINIPSD_STATIC
#  define MINIPSDDEF static
#elif defined(__cplusplus)
#  define MINIPSDDEF extern "C"
#else
#  define MINIPSDDEF extern
#endif

/* --- Return codes -------------------------------------------------------------------------- */

/**
 * \brief Return codes: MINIPSD_OK, MINIPSD_MORE, or one of the negative errors.
 *
 * Errors are negative and success is not, so a caller that also drives the pull writer tests
 * `rc < 0` for failure; `rc == MINIPSD_OK` stays right for every function that never returns
 * MINIPSD_MORE.
 */
enum {
    MINIPSD_OK          =  0, /*!< Success.                                                   */
    MINIPSD_MORE        =  1, /*!< Not an error: the work is not finished, call again.         */
    MINIPSD_ERR_ARG     = -1, /*!< A NULL document, no layers, or a dpi outside (0, 65536).    */
    MINIPSD_ERR_SIZE    = -2, /*!< A side is 0 or above MINIPSD_MAX_SIDE, or the document is too
                                   large for PSD's 32-bit section lengths (about 4 GiB).      */
    MINIPSD_ERR_WRITE   = -3, /*!< The write callback reported a failure.                      */
    MINIPSD_ERR_ALLOC   = -4, /*!< MINIPSD_MALLOC returned NULL.                               */
    MINIPSD_ERR_OPEN    = -5, /*!< Not returned by minipsd; kept for the caller's own file
                                   code (examples/): a file not opened or closed.             */
    MINIPSD_ERR_FORMAT  = -6, /*!< Reader: the bytes are not a well-formed PSD.                */
    MINIPSD_ERR_UNSUPPORTED = -7, /*!< Reader: a valid PSD outside what minipsd reads (so far a
                                       PSB; pixels other than 8-bit RGB once decoding lands).     */
    MINIPSD_ERR_SHORT   = -8, /*!< Reader: the buffer ends inside data the call needs; the
                                   file may be fine.  minipsd_reader_needed says how much more.  */
    MINIPSD_ERR_LIMIT   = -9  /*!< Reader: a value is over a limit (minipsd_limits).           */
};

/**
 * \brief A short English description of a return code.
 *
 * \param code A value returned by a minipsd function.
 * \return A static string; "unknown error" for codes minipsd does not return.
 */
MINIPSDDEF const char *minipsd_error_string(int code);

/* --- Workspace ----------------------------------------------------------------------------- */

/**
 * \brief Caller memory lent to a minipsd object (the writer, the reader).
 *
 * Its size depends on the document, so it has no layout of its own: the object's
 * *_workspace_size function says how many bytes it needs, and its init function lays the object
 * out inside.  Any alignment will do.  minipsd never allocates or frees it; it must stay valid for
 * as long as the object is used.  A void typedef, so any pointer converts to it without a cast.
 */
typedef void minipsd_workspace;

/* --- Document ------------------------------------------------------------------------------ */

/** \brief The largest width or height PSD allows, in pixels. */
#define MINIPSD_MAX_SIDE 30000

/** \brief Channel compression.  Zero is RLE, so a zero-initialised document gets the compact one. */
enum {
    MINIPSD_RLE = 0, /*!< PackBits run-length encoding. */
    MINIPSD_RAW = 1  /*!< Uncompressed.                 */
};

/**
 * \brief One layer of a document.
 *
 * Every pointer is borrowed: it must stay valid for the duration of the write call, and minipsd
 * never frees or copies what it points to.
 */
typedef struct minipsd_layer {
    const char    *name;    /*!< UTF-8; NULL means "".                                            */
    const uint8_t *rgba;    /*!< (right - left) * (bottom - top) * 4 bytes, rows top down, straight
                                 (not premultiplied) alpha; NULL means an empty layer.             */
    uint8_t        opacity; /*!< 255 is opaque, 0 is fully transparent.                            */
    uint8_t        hidden;  /*!< Non-zero: the layer starts hidden (and stays out of the composite). */
    int32_t        top, left, bottom, right; /*!< The layer's rectangle on the canvas, as in
                                 minipsd_layer_view; all four zero: the full canvas.  It may reach
                                 past the canvas, or lie wholly outside it.  An empty layer's has
                                 no area.                                                       */
} minipsd_layer;

/**
 * \brief A whole document: canvas, resolution, layers and options.
 *
 * Zero-initialise it and set the fields you need; zero compression selects RLE.  Pointers are
 * borrowed for the duration of the write call.
 */
typedef struct minipsd_doc {
    uint32_t             width, height;  /*!< 1 .. MINIPSD_MAX_SIDE.                        */
    double               dpi;            /*!< Above 0, below 65536; recorded, not resampled. */
    const minipsd_layer *layers;         /*!< Bottom first.                                  */
    size_t               layer_count;    /*!< >= 1.                                          */
    const uint8_t       *icc;            /*!< Optional ICC profile; NULL means none (sRGB).  */
    size_t               icc_size;       /*!< Size of icc in bytes; 0 when icc is NULL.      */
    int                  compression;    /*!< MINIPSD_RLE or MINIPSD_RAW.                    */
    const uint8_t       *composite;      /*!< The composite (merged) image: width * height * 4
                                              bytes, rows top down, straight alpha, written
                                              whatever the layers hold.  NULL: composed from the
                                              layers, a best effort (see "What it writes").  */
    int                  composite_matted; /*!< Non-zero: composite is already matted against
                                              white, as the file stores it (what
                                              minipsd_decode_composite gives with keep_matte),
                                              and is written as it is.  Ignored without
                                              composite.                                     */
} minipsd_doc;

/**
 * \brief Checks that a document can be written as PSD, without writing anything.
 *
 * The writer runs the same check first; call it alone to validate a document early.  One limit
 * is left to the writer: the layer section must fit 4 GiB once compressed, which only the sizing
 * pass can tell (MINIPSD_ERR_SIZE from minipsd_writer_init or minipsd_write).
 *
 * \param doc The document to check.
 * \return MINIPSD_OK or an error code.
 *
 * \retval MINIPSD_ERR_ARG  doc is NULL, there are no layers (or more than 32767), dpi is outside
 *                          (0, 65536), icc_size is set without icc, a layer's rectangle is
 *                          inverted, or an empty layer's has an area.
 * \retval MINIPSD_ERR_SIZE A side of the canvas or of a layer is 0 or above MINIPSD_MAX_SIDE (a
 *                          layer's may be 0), or the ICC profile does not fit a 32-bit section
 *                          length.
 */
MINIPSDDEF int minipsd_doc_check(const minipsd_doc *doc);

/* --- Writer -------------------------------------------------------------------------------- */

/**
 * \brief Receives the file's bytes, in order.
 *
 * \param user The pointer given to minipsd_write.
 * \param data The next bytes of the file.
 * \param size How many bytes data holds; never 0.
 * \return Non-zero on success, 0 to abort the write.
 */
typedef int (*minipsd_write_fn)(void *user, const void *data, size_t size);

#ifndef MINIPSD_NO_MALLOC
/**
 * \brief Writes a document as PSD through a callback.
 *
 * The callback receives the whole file front to back, in pieces of up to 4 KiB, and is never
 * asked to seek.  After it returns 0 it is not called again.  The same document always produces
 * the same bytes.  This is the pull writer driven to the end on a workspace from MINIPSD_MALLOC.
 *
 * \param doc  The document to write.
 * \param fn   Receives the bytes.
 * \param user Passed to fn unchanged.
 * \return MINIPSD_OK or an error code.
 *
 * \retval MINIPSD_OK        The whole file went to fn.
 * \retval MINIPSD_ERR_ARG   doc or fn is NULL, or the document fails minipsd_doc_check with it.
 *                           Nothing was written.
 * \retval MINIPSD_ERR_SIZE  As minipsd_doc_check, or the layers do not fit a 32-bit section
 *                           length.  Nothing was written.
 * \retval MINIPSD_ERR_ALLOC An allocation failed.  Nothing was written.
 * \retval MINIPSD_ERR_WRITE fn returned 0; the output is incomplete.
 */
MINIPSDDEF int minipsd_write(const minipsd_doc *doc, minipsd_write_fn fn, void *user);
#endif

/**
 * \brief The pull writer: produces the file into the caller's buffers, at the caller's pace.
 *
 * Opaque: minipsd_writer_init builds it inside the caller's workspace and hands back a pointer,
 * so nothing is allocated and the workspace is the writer's whole memory.  The workspace and the
 * document are borrowed: they must stay valid, and the document unchanged, for as long as the
 * writer is used.
 */
typedef struct minipsd_writer minipsd_writer;

/**
 * \brief How many bytes of workspace minipsd_writer_init needs for a document.
 *
 * The size covers the writer itself, the per-channel size table, the composite's row sizes and
 * one composite row, plus slack for aligning them: any pointer will do as the workspace.
 *
 * \param doc  The document to write.
 * \param size Receives the size in bytes.
 * \return MINIPSD_OK or an error code: as minipsd_doc_check, or MINIPSD_ERR_ARG when size is NULL.
 */
MINIPSDDEF int minipsd_writer_workspace_size(const minipsd_doc *doc, size_t *size);

/**
 * \brief Prepares a pull writer: checks the document and runs the sizing pass.
 *
 * Every layer channel and every composite row is compressed once here to learn its size, so the
 * file's size is known before its first byte (minipsd_writer_total).  Nothing is allocated.
 *
 * \param w         Receives the writer, built at the start of workspace; NULL on failure.
 * \param doc       The document to write; borrowed for as long as the writer is used.
 * \param workspace At least minipsd_writer_workspace_size bytes, any alignment; borrowed likewise.
 * \param size      The size of workspace in bytes.
 * \return MINIPSD_OK or an error code.
 *
 * \retval MINIPSD_ERR_ARG  As minipsd_doc_check, or w or workspace is NULL, or size is too small.
 * \retval MINIPSD_ERR_SIZE As minipsd_doc_check, or the compressed layers do not fit a 32-bit
 *                          section length.
 */
MINIPSDDEF int minipsd_writer_init(minipsd_writer **w, const minipsd_doc *doc, minipsd_workspace *workspace,
                                   size_t size);

/**
 * \brief The size of the whole file in bytes, known once minipsd_writer_init succeeded.
 *
 * \param w A writer from minipsd_writer_init.
 * \return The file size; 0 when w is NULL.
 */
MINIPSDDEF uint64_t minipsd_writer_total(const minipsd_writer *w);

/**
 * \brief Produces the next bytes of the file into buf.
 *
 * Call it until it returns MINIPSD_OK.  Each call fills buf with as many bytes as fit, in file
 * order, and returns at once: the writer never waits on anything, so it suits a non-blocking
 * target as well as a blocking one.  A target that took only part of the bytes is the caller's
 * business: keep the rest and send it before calling again.  Any cap works, 1 included; a call
 * re-encodes the row it stops in, so a buffer of a few rows (64 KiB is plenty) keeps that cost
 * negligible.  The bytes are those minipsd_write produces for the same document.
 *
 * \param w        A writer from minipsd_writer_init.
 * \param buf      Receives up to cap bytes.
 * \param cap      The size of buf in bytes; 0 produces nothing.
 * \param produced Receives how many bytes went into buf.
 * \return MINIPSD_MORE while bytes remain; MINIPSD_OK when the last byte has been produced (and on
 *         every call after that, producing nothing); or an error code.
 *
 * \retval MINIPSD_ERR_ARG w or produced is NULL, or buf is NULL with cap above 0.
 */
MINIPSDDEF int minipsd_writer_fill(minipsd_writer *w, void *buf, size_t cap, size_t *produced);

/* --- Reader -------------------------------------------------------------------------------- */

/**
 * \brief A read-only run of bytes inside a buffer the caller owns.
 *
 * Every view the reader hands out points into the buffer given to minipsd_reader_init, and is valid
 * for as long as that buffer is unchanged.  Nothing is copied.
 */
typedef struct minipsd_bytes {
    const uint8_t *data; /*!< The first byte; NULL for a view that does not exist.  */
    size_t         size; /*!< How many bytes.                                       */
} minipsd_bytes;

/**
 * \brief What the reader refuses to look at.  Zero fields keep the format's own limits.
 *
 * Fields are only ever added at the end, and zero keeps the previous behaviour.
 */
typedef struct minipsd_limits {
    uint32_t max_side;          /*!< Largest width or height of the canvas, checked by init, and of
                                     a layer, checked by next_layer; 0 is MINIPSD_MAX_SIDE for the
                                     canvas and no limit for layers, which may reach past it.     */
    uint32_t max_layers;        /*!< Most layers in a file; 0 is 32767, the format's cap.          */
    uint64_t max_decoded_bytes; /*!< Most bytes one decode call may write, width * rows * planes;
                                     0 is no limit.                                              */
} minipsd_limits;

/**
 * \brief What a file says about itself, read by minipsd_reader_init.
 *
 * Any PSD of version 1 is described, whatever its colour mode or depth; it is the decoding that
 * is limited to 8-bit RGB.
 */
typedef struct minipsd_info {
    uint32_t      width, height;       /*!< In pixels.                                                 */
    uint16_t      channels;            /*!< Channels of the composite, as stored.                      */
    uint16_t      depth;               /*!< Bits per channel: 1, 8, 16 or 32.                          */
    uint16_t      color_mode;          /*!< As stored: 0 bitmap, 1 grey, 2 indexed, 3 RGB, 4 CMYK,
                                            7 multichannel, 8 duotone, 9 Lab.                          */
    double        dpi;                 /*!< Horizontal pixels per inch; 0 without a resolution resource. */
    minipsd_bytes icc;                 /*!< The ICC profile; size 0 when the file has none.            */
    size_t        layer_count;         /*!< Layers in the file.                                        */
    int           composite_has_alpha; /*!< Non-zero: the composite's first extra channel is transparency
                                            (the file's layer count is negative).                      */
} minipsd_info;

/** \brief One image resource: a numbered block of the file, such as 1036, the embedded thumbnail. */
typedef struct minipsd_resource {
    uint16_t      id;   /*!< The resource's number.               */
    minipsd_bytes data; /*!< Its bytes, without the padding.      */
} minipsd_resource;

/** \brief One tagged block of a layer's additional information ('luni', 'lsct', 'tySh', ...). */
typedef struct minipsd_block {
    char          key[5]; /*!< The four-character key, then a NUL.  */
    minipsd_bytes data;   /*!< The block's bytes.                   */
} minipsd_block;

/** \brief The index of a colour channel in minipsd_layer_view.channel. */
enum {
    MINIPSD_CH_R = 0, /*!< Red.                                */
    MINIPSD_CH_G = 1, /*!< Green.                              */
    MINIPSD_CH_B = 2, /*!< Blue.                               */
    MINIPSD_CH_A = 3  /*!< Transparency (the channel with id -1). */
};

/**
 * \brief One layer record, as minipsd_reader_next_layer reads it.
 *
 * All views point into the buffer of the reader.
 */
typedef struct minipsd_layer_view {
    int32_t       top, left, bottom, right; /*!< The layer's rectangle on the canvas; all zero for an
                                                 empty layer.  Never inverted: one stored with bottom
                                                 above top or right left of left (Photoshop's fill
                                                 layers) comes back empty, bottom = top, right = left. */
    uint8_t       opacity;                  /*!< 255 is opaque.                                        */
    uint8_t       hidden;                   /*!< Non-zero: the layer is hidden.                        */
    char          blend[5];                 /*!< The blend mode's four-character key ("norm"), then a NUL. */
    minipsd_bytes name_pascal;              /*!< The legacy name, raw bytes (non-ASCII characters are
                                                 whatever code page the writer used).                  */
    minipsd_bytes name_unicode;             /*!< The 'luni' name, raw UTF-16BE; data is NULL when the
                                                 record has none.  minipsd_layer_name converts.        */
    minipsd_bytes extra;                    /*!< All the layer's additional-information blocks, walked
                                                 with minipsd_layer_next_block.                        */
    minipsd_bytes channel[4];               /*!< Red, green, blue, transparency (MINIPSD_CH_*): the
                                                 channel's stored data, its compression marker first.
                                                 size 0: the layer has no such channel.  data NULL with
                                                 a size: the channel exists but the buffer ends before
                                                 it (a prefix of the file).                            */
    uint64_t      channel_offset[4];        /*!< Where each channel's data starts in the file (0 for an
                                                 absent one): a buffer holds the channel once it reaches
                                                 channel_offset + size.                                */
    uint16_t      other_channels;           /*!< Channels the reader does not read (masks, ...).       */
} minipsd_layer_view;

/**
 * \brief The reader: walks a PSD held in memory and hands out views of it.
 *
 * Opaque, like the writer: minipsd_reader_init builds it inside the caller's workspace.  The buffer
 * and the workspace are borrowed for as long as the reader is used.
 */
typedef struct minipsd_reader minipsd_reader;

/**
 * \brief How many bytes of workspace minipsd_reader_init needs.
 *
 * The size does not depend on the file, so it can be asked before the first byte is read.
 *
 * \param size Receives the size in bytes.
 * \return MINIPSD_OK, or MINIPSD_ERR_ARG when size is NULL.
 */
MINIPSDDEF int minipsd_reader_workspace_size(size_t *size);

/**
 * \brief Starts reading a PSD: its header, its image resources and the number of layers.
 *
 * Needs the bytes up to the layer count, resources included (so a whole ICC profile and a
 * thumbnail), and nothing after.  A buffer that ends sooner gives MINIPSD_ERR_SHORT: read more and
 * call again, on the same workspace; minipsd_reader_needed says how much.  Nothing is allocated or
 * copied, and calling init again restarts every iteration.
 *
 * \param r              Receives the reader, built inside workspace; set on MINIPSD_OK and on
 *                       MINIPSD_ERR_SHORT, NULL otherwise.
 * \param data           The file, or a prefix of it; NULL only with size 0.
 * \param size           How many bytes data holds.
 * \param workspace      At least minipsd_reader_workspace_size bytes, any alignment.
 * \param workspace_size The size of workspace in bytes.
 * \param limits         What to refuse; NULL keeps the format's own limits.
 * \param info           Receives the file's description on MINIPSD_OK.
 * \return MINIPSD_OK or an error code.
 *
 * \retval MINIPSD_ERR_ARG         r, info or workspace is NULL, data is NULL with a size, or
 *                                 workspace_size is too small.
 * \retval MINIPSD_ERR_SHORT       The buffer ends too soon.
 * \retval MINIPSD_ERR_FORMAT      Not a PSD, or malformed.  Four wrong bytes at the start are enough.
 * \retval MINIPSD_ERR_UNSUPPORTED A PSB (version 2).
 * \retval MINIPSD_ERR_LIMIT       A side or the layer count is over limits.
 */
MINIPSDDEF int minipsd_reader_init(minipsd_reader **r, const void *data, size_t size, minipsd_workspace *workspace,
                                   size_t workspace_size, const minipsd_limits *limits, minipsd_info *info);

/**
 * \brief The least buffer size with which the call that returned MINIPSD_ERR_SHORT can go further.
 *
 * Exact for the next step only: a section's length is read before the section, so the answer can
 * grow as more of the file arrives.  Read at least that many bytes (from the start of the file) and
 * repeat the call: minipsd_reader_init, then the iteration up to where it stopped.  For a decode
 * it is where the last channel the target asks for ends.
 *
 * \param r A reader, or the one minipsd_reader_init set before returning MINIPSD_ERR_SHORT.
 * \return The size in bytes; 0 when r is NULL.
 */
MINIPSDDEF uint64_t minipsd_reader_needed(const minipsd_reader *r);

/**
 * \brief The next image resource, in file order.
 *
 * \param r   A reader from minipsd_reader_init.
 * \param res Receives the resource.
 * \return MINIPSD_MORE when a resource was written to res; MINIPSD_OK when there are no more
 *         (res is untouched); or MINIPSD_ERR_ARG when r (never initialised) or res is NULL.
 */
MINIPSDDEF int minipsd_reader_next_resource(minipsd_reader *r, minipsd_resource *res);

/**
 * \brief The next layer record, bottom layer first.
 *
 * The first call walks every record once to find where the channel data begins, so for the names
 * of a file read in parts it needs the whole record section.  A layer whose channel data lies past
 * the end of the buffer is still returned; its channel view says so (see minipsd_layer_view).
 *
 * \param r     A reader from minipsd_reader_init.
 * \param layer Receives the record.
 * \return MINIPSD_MORE when a layer was written to layer; MINIPSD_OK when there are no more;
 *         or an error code.
 *
 * \retval MINIPSD_ERR_ARG    r (never initialised) or layer is NULL.
 * \retval MINIPSD_ERR_SHORT  The buffer ends inside the records; the reader stays where it was.
 * \retval MINIPSD_ERR_FORMAT The records are malformed.
 * \retval MINIPSD_ERR_LIMIT  The layer is wider or taller than the max_side the caller set; the
 *                           reader stays where it was.
 */
MINIPSDDEF int minipsd_reader_next_layer(minipsd_reader *r, minipsd_layer_view *layer);

/**
 * \brief A layer's name as UTF-8.
 *
 * The 'luni' name when the record has one; otherwise the legacy name, read as Latin-1.  An
 * unpaired surrogate in 'luni' becomes U+FFFD: a name is for display, so it is not an error.  A
 * U+0000 ends the name, since some writers count a terminating NUL in 'luni'.
 *
 * \param layer  A record from minipsd_reader_next_layer.
 * \param utf8   Receives the name and a terminating NUL; may be NULL when cap is 0.
 * \param cap    The size of utf8 in bytes.
 * \param needed Receives the bytes the name takes with its NUL; may be NULL.
 * \return MINIPSD_OK when it fits; MINIPSD_MORE when cap is too small (utf8 is then empty);
 *         or MINIPSD_ERR_ARG when layer is NULL or utf8 is NULL with cap above 0.
 */
MINIPSDDEF int minipsd_layer_name(const minipsd_layer_view *layer, char *utf8, size_t cap, size_t *needed);

/**
 * \brief The next additional-information block of a layer, in record order.
 *
 * \param layer A record from minipsd_reader_next_layer.
 * \param pos   The caller's cursor: start at 0; moved past the block returned.
 * \param block Receives the block.
 * \return MINIPSD_MORE when a block was written to block; MINIPSD_OK at the end;
 *         MINIPSD_ERR_FORMAT for a malformed block; MINIPSD_ERR_ARG for a NULL argument.
 */
MINIPSDDEF int minipsd_layer_next_block(const minipsd_layer_view *layer, size_t *pos, minipsd_block *block);

/**
 * \brief Where one channel's samples go: the first sample, and the steps to the next one.
 *
 * The library cannot see the size of the memory behind data: it writes exactly the samples of the
 * rows it decodes, at data + y * row_step + x * pixel_step, and nothing else.
 */
typedef struct minipsd_plane {
    uint8_t  *data;       /*!< Sample (0, 0) of the channel; NULL: the channel is not written.      */
    ptrdiff_t pixel_step; /*!< Bytes from a sample to its right neighbour: 4 for interleaved RGBA. */
    ptrdiff_t row_step;   /*!< Bytes from a row to the next; negative for bottom-up rows.          */
} minipsd_plane;

/**
 * \brief Where decoded pixels go: one plane per channel, by MINIPSD_CH_* (R, G, B, A).
 *
 * One shape for every layout: interleaved RGBA (minipsd_target_rgba), BGRA (swap two planes),
 * planar, a sub-rectangle of a larger image (offset data), or only some channels (the rest NULL).
 * Its row 0 receives the first decoded row.  Samples are 8-bit; straight, not premultiplied, alpha.
 */
typedef struct minipsd_target {
    minipsd_plane channel[4]; /*!< R, G, B, A. */
} minipsd_target;

/**
 * \brief Which rows to decode.  NULL options decode the whole image.
 *
 * Fields are only ever added at the end, and zero keeps the previous behaviour.
 */
typedef struct minipsd_decode_opts {
    uint32_t y_first;    /*!< The first row, counted from the layer's top.                     */
    uint32_t y_count;    /*!< How many rows; 0: from y_first to the end.                        */
    int      keep_matte; /*!< Composite only: non-zero keeps the white matte, as stored.        */
} minipsd_decode_opts;

/**
 * \brief Points a target at interleaved RGBA: four planes one byte apart, a pixel step of 4.
 *
 * \param t          The target to fill; nothing happens when it is NULL.
 * \param rgba       The first pixel; NULL leaves every plane NULL.
 * \param row_stride Bytes from a row to the next: width * 4 for a tight image.
 */
MINIPSDDEF void minipsd_target_rgba(minipsd_target *t, uint8_t *rgba, size_t row_stride);

/**
 * \brief Decodes a layer's pixels, or a band of its rows, into the target.
 *
 * The layer is decoded as stored: its rectangle's width (right - left) by the rows asked for,
 * with its opacity and hidden flag left in the view, not applied.  A layer without a transparency
 * channel decodes as opaque (255 in the A plane).  Everything that can fail before decoding is
 * checked first, so after MINIPSD_ERR_ARG, MINIPSD_ERR_UNSUPPORTED or MINIPSD_ERR_SHORT the target
 * is untouched; after MINIPSD_ERR_FORMAT found inside a row it holds the rows decoded so far.
 * An empty layer, or a band of no rows, writes nothing.
 *
 * \param r      The reader the layer came from.
 * \param layer  A record from minipsd_reader_next_layer of that reader.
 * \param target Where the samples go; only its non-NULL planes are written.
 * \param opts   The rows; NULL for all of them.
 * \return MINIPSD_OK or an error code.
 *
 * \retval MINIPSD_ERR_ARG         r (never initialised), layer or target is NULL, or the band
 *                                 lies outside the layer.
 * \retval MINIPSD_ERR_LIMIT       The call would write more than max_decoded_bytes.
 * \retval MINIPSD_ERR_UNSUPPORTED The file is not 8-bit RGB, or a channel is ZIP-compressed.
 * \retval MINIPSD_ERR_SHORT       A channel the target asks for is not all in the buffer;
 *                                 minipsd_reader_needed says how far it must reach.
 * \retval MINIPSD_ERR_FORMAT      A channel the target asks for is missing or malformed.
 */
MINIPSDDEF int minipsd_decode_layer(minipsd_reader *r, const minipsd_layer_view *layer, const minipsd_target *target,
                                    const minipsd_decode_opts *opts);

/**
 * \brief Decodes the composite (merged) image, or a band of its rows, into the target.
 *
 * The composite is what a reader that ignores layers shows: the full canvas, info.width by the
 * rows asked for.  The file stores its colour matted against white; by default the matte is
 * undone, giving straight alpha, exact where alpha is 255 and within about 255 / (2 * alpha)
 * levels below, since 8-bit matting has already merged nearby colours (0 where alpha is 0).
 * Un-matting reads each pixel's alpha back from the target, so it then needs an A plane.  A file
 * whose composite has no transparency (info.composite_has_alpha is 0) decodes as opaque.  As in
 * minipsd_decode_layer, every check comes before the first sample is written.
 *
 * \param r      A reader from minipsd_reader_init.
 * \param target Where the samples go; only its non-NULL planes are written.
 * \param opts   The rows and keep_matte; NULL for every row, un-matted.
 * \return MINIPSD_OK or an error code.
 *
 * \retval MINIPSD_ERR_ARG         r (never initialised) or target is NULL, the band lies outside
 *                                 the image, or the matte is to be undone with no A plane.
 * \retval MINIPSD_ERR_LIMIT       The call would write more than max_decoded_bytes.
 * \retval MINIPSD_ERR_UNSUPPORTED The file is not 8-bit RGB, or the image is ZIP-compressed.
 * \retval MINIPSD_ERR_SHORT       The rows of a channel the target asks for are not all in the
 *                                 buffer; minipsd_reader_needed says how far it must reach.
 * \retval MINIPSD_ERR_FORMAT      The image is malformed.
 */
MINIPSDDEF int minipsd_decode_composite(minipsd_reader *r, const minipsd_target *target, const minipsd_decode_opts *opts);

#endif /* MINIPSD_H */

/* ============================================================================================== */

#ifdef MINIPSD_IMPLEMENTATION
#include "minipsd/base.h"
#include "minipsd/writer.h"
#include "minipsd/reader.h"
#endif

/*
--------------------------------------------------------------------------------------------------
MIT License

Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
--------------------------------------------------------------------------------------------------
*/
