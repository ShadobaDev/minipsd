/**
 * \file minipsd.h
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
 * - Document from a file
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
 *     int rc = minipsd_write(&doc, NULL, file_out, f);   // file_out: fwrite to f, see minipsd_write_fn
 *     if (rc != MINIPSD_OK) puts(minipsd_error_string(rc));
 * \endcode
 *
 * \par Configuration
 * Define before the implementation include:
 * - \c MINIPSD_STATIC: make every function static (private to that file);
 * - \c MINIPSD_MALLOC / \c MINIPSD_FREE: replace malloc / free (define both);
 * - \c MINIPSD_NO_MALLOC: leave out every allocation: minipsd_write, minipsd_read and
 *   minipsd_doc_free go (and <stdlib.h>); the pull writer, the reader and minipsd_read_into, on
 *   the caller's memory, stay.
 *
 * minipsd does no I/O of its own and includes no <stdio.h>: files, pipes and sockets are the
 * caller's, fed by minipsd_write or minipsd_writer_fill (examples/ in the repository has each).
 * \c MINIPSD_NO_STDIO, which left out a file writer until 0.2.0, is still accepted and does
 * nothing.
 *
 * \par What it writes
 * An RGB document, as a PSD or (minipsd_doc.format) a PSB, 8 or 16 bits a channel, with a
 * transparency channel:
 * - one or more layers, bottom first, each a rectangle anywhere on or off the canvas (by default
 *   the full canvas) or empty, with a name, opacity, visibility, blend key, clipping and flags;
 *   names go out both as a legacy Pascal string (ASCII) and as Unicode ('luni'), followed by any
 *   blocks kept from a file (text, effects, groups, ...);
 * - the composite (merged) image, which is what a reader that ignores layers shows: the caller's
 *   own render (minipsd_doc.composite), the right way, since flattening layers is rendering; or,
 *   without one, a best effort with no guarantee: the visible layers blended with straight-alpha
 *   "over" at their rectangles, clipped to the canvas.  Its transparency rides as the first extra
 *   channel, flagged by a negative layer count, and its colour is matted against white
 *   (c * a + 255 * (1 - a)), as Photoshop stores it and readers expect to undo;
 * - the resolution, in pixels per inch (resource 1005);
 * - an optional ICC profile (resource 1039); without one, readers assume sRGB;
 * - resources and the global layer mask and blocks kept from a file, as they are.
 *
 * Channel data is PackBits RLE by default (small for flat art), or raw.
 *
 * Groups (minipsd_layer.kind) and layer masks (minipsd_layer.mask) are written; text and effects
 * only as blocks kept from a file.  Not written: 32-bit, other colour modes.
 * PSD caps each side at 30000 px, PSB at 300000; larger documents are refused (MINIPSD_ERR_SIZE).
 *
 * \par Output
 * Bytes come out in order, never seeking back, so a pipe or a socket works as well as a file:
 * pulled by the caller into its own buffer (minipsd_writer_fill), or pushed to a callback
 * (minipsd_write).  The pull writer allocates nothing; its workspace, from the caller, holds a
 * size table (layer_count * 6 numbers), the composite's row sizes and one composite row.
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
    MINIPSD_ERR_ARG     = -1, /*!< A NULL document, no layers nor composite, or a dpi outside
                                   (0, 65536).                                                  */
    MINIPSD_ERR_SIZE    = -2, /*!< A side is 0 or above the format's (MINIPSD_MAX_SIDE, or
                                   MINIPSD_MAX_SIDE_PSB), or the document is too large for
                                   PSD's 32-bit section lengths (about 4 GiB).                */
    MINIPSD_ERR_WRITE   = -3, /*!< The write callback reported a failure.                      */
    MINIPSD_ERR_ALLOC   = -4, /*!< MINIPSD_MALLOC returned NULL.                               */
    MINIPSD_ERR_OPEN    = -5, /*!< Not returned by minipsd; kept for the caller's own file
                                   code (examples/): a file not opened or closed.             */
    MINIPSD_ERR_FORMAT  = -6, /*!< Reader: the bytes are not a well-formed PSD.                */
    MINIPSD_ERR_UNSUPPORTED = -7, /*!< Valid, but outside what minipsd handles yet: reading,
                                       pixels other than 8- or 16-bit RGB, or ZIP-compressed;
                                       writing in the other format, kept content that cannot
                                       change format yet.                                         */
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

/** \brief The largest width or height PSB allows, in pixels. */
#define MINIPSD_MAX_SIDE_PSB 300000

/**
 * \brief The file format, as the version field of the header says.  PSB ("large document format")
 *        is PSD with wider lengths and row counts; what the file holds means the same in both.
 */
enum {
    MINIPSD_PSD = 1, /*!< Version 1: lengths of four bytes, row counts of two. */
    MINIPSD_PSB = 2  /*!< Version 2: section, channel and some block lengths of eight bytes, row
                          counts of four; sides up to MINIPSD_MAX_SIDE_PSB.                   */
};

/** \brief Channel compression.  Zero is RLE, so a zero-initialised document gets the compact one. */
enum {
    MINIPSD_RLE = 0, /*!< PackBits run-length encoding. */
    MINIPSD_RAW = 1  /*!< Uncompressed.                 */
};

/**
 * \brief A read-only run of bytes inside a buffer the caller owns.
 *
 * Every view the reader hands out points into the buffer given to minipsd_reader_init, and is valid
 * for as long as that buffer is unchanged.  Nothing is copied.  In a document, it is content kept
 * from a file and written back as it is, borrowed like every other pointer there.
 */
typedef struct minipsd_bytes {
    const uint8_t *data; /*!< The first byte; NULL for a view that does not exist.  */
    size_t         size; /*!< How many bytes.                                       */
} minipsd_bytes;

/** \brief One image resource: a numbered block of the file, such as 1036, the embedded thumbnail. */
typedef struct minipsd_resource {
    uint16_t      id;   /*!< The resource's number.                                    */
    minipsd_bytes data; /*!< Its bytes, without the padding.                           */
    minipsd_bytes name; /*!< Its name, raw bytes of a Pascal string; usually empty.     */
} minipsd_resource;

/** \brief One tagged block of a layer's additional information ('luni', 'lsct', 'tySh', ...). */
typedef struct minipsd_block {
    char          key[5]; /*!< The four-character key, then a NUL.  */
    minipsd_bytes data;   /*!< The block's bytes.                   */
} minipsd_block;

/**
 * \brief One "Blend If" range: where a slider's black and white points sit, 0 to 255 at any depth.
 *
 * Each point has two halves, split in Photoshop by Alt-dragging for a soft transition; unsplit,
 * both are equal.  Left alone, a range is black {0, 0}, white {255, 255}.
 */
typedef struct minipsd_blend_range {
    uint8_t black[2]; /*!< The black point: its low and high half.  */
    uint8_t white[2]; /*!< The white point: its low and high half.  */
} minipsd_blend_range;

/**
 * \brief A layer's blending ranges ("Blend If") for one channel, as the file stores them.
 *
 * A layer has one for the composite grey, then one per channel, in the file's order (Photoshop
 * writes five: grey, then four).  Where the layer's own pixels fall outside source, or those
 * beneath it outside destination, the layer does not show.
 */
typedef struct minipsd_blending_range {
    minipsd_blend_range source;      /*!< "This Layer".       */
    minipsd_blend_range destination; /*!< "Underlying Layer". */
} minipsd_blending_range;

/** \brief What a layer record is: a layer, or one end of a group (the type of its 'lsct' block). */
enum {
    MINIPSD_KIND_LAYER        = 0, /*!< A layer with pixels, or an empty one.                     */
    MINIPSD_KIND_GROUP        = 1, /*!< A group, shown open in the layers panel: its top record.  */
    MINIPSD_KIND_GROUP_CLOSED = 2, /*!< A group, shown closed: its top record.                    */
    MINIPSD_KIND_GROUP_END    = 3  /*!< Where a group starts, below its layers: the divider.      */
};

/**
 * \brief A layer mask: one 8-bit channel over its own rectangle, 255 shows the layer, 0 hides it.
 *
 * Pointers are borrowed, like the layer's.  Fields are only ever added at the end.
 */
typedef struct minipsd_mask {
    int32_t        top, left, bottom, right; /*!< The mask's rectangle on the canvas; may differ from
                                                  the layer's, and may have no area.                */
    uint8_t        color;                    /*!< Its value outside the rectangle: 0 or 255.        */
    uint8_t        flags;                    /*!< As stored: 0x01 position relative to the layer,
                                                  0x02 disabled, 0x08 made by rendering other data,
                                                  0x10 parameters follow (in rest).                 */
    const uint8_t *pixels;                   /*!< (right - left) * (bottom - top) samples, rows top
                                                  down, of the document's depth; NULL with stored, or
                                                  for a rectangle without area.                     */
    minipsd_bytes  stored;                   /*!< The channel as a file stores it (compression marker,
                                                  rows), copied instead of encoding pixels; size 0:
                                                  pixels are encoded.                               */
    minipsd_bytes  real;                     /*!< The "real" user mask (channel -3) a file stores
                                                  beside a vector mask, kept as stored; size 0: none. */
    minipsd_bytes  rest;                     /*!< The mask data after flags, kept from a file:
                                                  parameters, the real mask's flags and rectangle.
                                                  Size 0: none.                                     */
} minipsd_mask;

/**
 * \brief One layer of a document.
 *
 * Every pointer is borrowed: it must stay valid for the duration of the write call, and minipsd
 * never frees or copies what it points to.
 */
typedef struct minipsd_layer {  /* NOLINT(clang-analyzer-optin.performance.Padding): fields are only added at the end */
    const char    *name;        /*!< UTF-8; NULL means "".                                            */
    const uint8_t *rgba;        /*!< (right - left) * (bottom - top) * 4 samples, rows top down, straight
                                    (not premultiplied) alpha; NULL means an empty layer.  A sample is
                                    a byte, or with depth 16 a uint16_t in host order (two bytes).  */
    uint8_t        opacity;     /*!< 255 is opaque, 0 is fully transparent.                            */
    uint8_t        hidden;      /*!< Non-zero: the layer starts hidden (and stays out of the composite). */
    int32_t        top, left, bottom, right; /*!< The layer's rectangle on the canvas, as in
                                    minipsd_layer_view; all four zero, with rgba and no stored: the
                                    full canvas.  It may reach past the canvas, or lie wholly
                                    outside it.  An empty layer's has no area, or is inverted as
                                    a file stored it (Photoshop's fill layers: 0, 0, -1, 0).      */
    char           blend[4];    /*!< The blend mode's four-character key, such as "mul "; all zero:
                                    "norm".  Written as given; the composed composite draws every
                                    mode as normal.                                                */
    uint8_t        clipping;    /*!< Non-zero: clipped to the layer below.  Written as given.        */
    uint8_t        flags;       /*!< The record's other flag bits, written as given: 0x01 transparency
                                    locked, 0x08 and 0x10 pixel data irrelevant (groups' markers).
                                    Bit 0x02 is ignored: hidden says it.                           */
    minipsd_bytes  extra;       /*!< Additional-information blocks kept from a file (text, effects,
                                    groups, ...), as minipsd_layer_view.extra holds them: written
                                     after the layer's own name, except a 'luni' block, since name
                                    wins.                                                          */
    const minipsd_bytes *stored; /*!< NULL: rgba is encoded.  Otherwise four channels as a file
                                    stores them (R, G, B, transparency, as minipsd_layer_view.channel:
                                    the compression marker, then the rows), copied as they are
                                    instead of encoding rgba: a layer never decoded is never encoded
                                    again.  Each is checked against the rectangle; transparency of
                                    size 0 is none, and written as none.  rgba may then be NULL, as long
                                    as the document supplies its composite.                       */
    uint8_t        kind;        /*!< MINIPSD_KIND_*: a layer, or a group's top record or divider.
                                    Groups nest bottom first: a divider, the group's layers, then
                                    its top record, which carries the group's name, visibility and
                                    mode (blend; zero: pass through).  Written as an 'lsct' block,
                                    or a kept one of the same kind as it is.                       */
    const minipsd_mask *mask;   /*!< The layer mask; NULL: none.                                   */
    const minipsd_blending_range *blending_ranges; /*!< blending_range_count ranges, written as given:
                                    the composite grey's, then each channel's.  NULL with a count
                                    of 0: none (what a writer other than Photoshop writes).  The
                                    composed composite ignores them.                               */
    uint32_t       blending_range_count; /*!< How many; 0: none.                                  */
    minipsd_bytes  name_pascal; /*!< The legacy name kept from a file, raw bytes (at most 255, in the
                                    code page of the program that wrote it), as
                                    minipsd_layer_view.name_pascal.  While name still says what the
                                    file's names say, they are written as they are, this one and
                                    a kept 'luni'; once it changes, the writer's own.  Size 0 and
                                    no kept 'luni': the writer's own.                            */
} minipsd_layer;

/**
 * \brief A whole document: canvas, resolution, layers and options.
 *
 * Zero-initialise it and set the fields you need; zero compression selects RLE.  Pointers are
 * borrowed for the duration of the write call.
 */
typedef struct minipsd_doc {
    uint32_t             width, height;  /*!< 1 .. MINIPSD_MAX_SIDE, or MINIPSD_MAX_SIDE_PSB
                                              for a PSB.                                      */
    double               dpi;            /*!< Above 0, below 65536; recorded, not resampled. */
    const minipsd_layer *layers;         /*!< Bottom first.                                  */
    size_t               layer_count;    /*!< >= 1; 0 with a composite (supplied or stored):
                                              a flat file, its image the composite.           */
    const uint8_t       *icc;            /*!< Optional ICC profile; NULL means none (sRGB).  */
    size_t               icc_size;       /*!< Size of icc in bytes; 0 when icc is NULL.      */
    int                  compression;    /*!< MINIPSD_RLE or MINIPSD_RAW.                    */
    const uint8_t       *composite;      /*!< The composite (merged) image: width * height * 4
                                              samples, rows top down, straight alpha, written
                                              whatever the layers hold.  NULL: composite_stored
                                              is copied; without it too, composed from the
                                              layers, a best effort (see "What it writes").  */
    int                  composite_matted; /*!< Non-zero: composite is already matted against
                                              white, as the file stores it (what
                                              minipsd_decode_composite gives with keep_matte),
                                              and is written as it is.  Ignored without
                                              composite.                                     */
    const minipsd_resource *resources;   /*!< Image resources kept from a file (thumbnail, guides,
                                              ...), written in order; the writer's resolution
                                              and ICC profile take the place of the first 1005
                                              and 1039 (dpi and icc win), or come first.      */
    size_t               resource_count; /*!< How many resources.                            */
    minipsd_bytes        layer_tail;     /*!< The rest of the layer and mask section kept from a
                                              file (minipsd_info.layer_tail): the global layer
                                              mask and the global blocks, written as they are;
                                              an 'Lr16' (or 'Lr32') block among them says
                                              where the layers go, the layer info left empty,
                                              as Photoshop's 16-bit files have them.
                                              Empty: no global mask, no blocks (an empty global
                                              mask is written), but with data set and size 0:
                                              nothing at all, as some files end the section.  */
    uint16_t             depth;          /*!< Bits per channel: 0 or 8, or 16, where every sample
                                              (rgba, mask pixels, composite) is a uint16_t in
                                              host order, 0 .. 65535.                         */
    uint16_t             format;         /*!< MINIPSD_PSD (or 0) or MINIPSD_PSB: what the
                                              document is.  Everything kept from a file
                                              (stored channels, masks' stored and real, extra,
                                              layer_tail) is laid out as this format lays it
                                              out; minipsd_read sets it to the file's.  It is
                                              written as this format unless
                                              minipsd_write_opts.format says otherwise.       */
    minipsd_bytes        composite_stored; /*!< The composite as a file stores it, copied as it is
                                              when composite is NULL, never decoded: its
                                              compression marker, then (RLE) every row's byte
                                              count, channel by channel, then the rows, laid
                                              out as format lays it out.  minipsd_read sets it
                                              with minipsd_read_opts.stored.  Size 0: none.   */
    uint16_t             composite_channels; /*!< The channels composite_stored holds: 3 (red,
                                              green, blue), or 4, the fourth transparency (the
                                              layer count is then written negative).  Ignored
                                              without composite_stored.                       */
    uint8_t              layer_info_align; /*!< The layer info's length is padded to a multiple
                                              of this: 0 or 2, even (the default); 4, as
                                              Photoshop pads it; 1, not padded.  minipsd_read
                                              sets the file's.                                 */
} minipsd_doc;

/**
 * \brief How a document is written.  NULL options write it as it is.
 *
 * Fields are only ever added at the end, and zero keeps the previous behaviour.
 */
typedef struct minipsd_write_opts {
    uint16_t format; /*!< The file written: MINIPSD_PSD or MINIPSD_PSB; 0 is the document's own
                          format.  When it differs from the document's, what the document keeps
                          is translated: RLE row counts and long blocks' lengths widen or narrow.
                          Blocks whose data itself depends on the format ('Layr', and an
                          'Lr16' or 'Lr32' past the first, which holds the layers) and long
                          keys not yet verified ('Alph', 'FELS', 'FEid', 'FXid',
                          'Mt16', 'Mtrn', 'PxSD', 'lnk3') are refused (MINIPSD_ERR_UNSUPPORTED);
                          a PSB that does not fit a PSD is MINIPSD_ERR_SIZE.                  */
} minipsd_write_opts;

/**
 * \brief Checks that a document can be written, without writing anything.
 *
 * The writer runs the same check first; call it alone to validate a document early.  One limit
 * is left to the writer: a PSD's layer section must fit 4 GiB once compressed, which only the
 * sizing pass can tell (MINIPSD_ERR_SIZE from minipsd_writer_init or minipsd_write).
 *
 * \param doc  The document to check.
 * \param opts How it is to be written; NULL: as it is.
 * \return MINIPSD_OK or an error code.
 *
 * \retval MINIPSD_ERR_ARG  doc is NULL, a format is neither 0, MINIPSD_PSD nor MINIPSD_PSB (the
 *                          document's or opts'), there are no layers and no composite (or more than 32767 layers), dpi is outside
 *                          (0, 65536), depth is not 0, 8 or 16, a size is set without its data (icc, a resource, a name,
 *                          extra, layer_tail), a layer with rgba has an inverted rectangle, an empty layer's
 *                          has an area, extra is not a run of well-formed blocks, a stored
 *                          channel does not match its rectangle, a layer has only stored
 *                          channels while the composite is to be composed, groups do not nest
 *                          (every group's top record needs its divider below it), or a mask's
 *                          rectangle is inverted or has an area without pixels.
 * \retval MINIPSD_ERR_SIZE A side of the canvas, a layer or a mask is 0 or above the format's
 *                          (MINIPSD_MAX_SIDE, MINIPSD_MAX_SIDE_PSB; a layer's may be 0), a
 *                          resource name is longer than 255 bytes, or the resources or a layer's
 *                          extra data do not fit a 32-bit length, or a PSD's layers its 32-bit
 *                          section length; written as a PSD from a PSB, a stored RLE row count
 *                          above 65535 or a kept block above 4 GiB.
 * \retval MINIPSD_ERR_UNSUPPORTED Written in the other format than the document's, a kept block
 *                          that cannot change format yet (see minipsd_write_opts.format), a
 *                          layer tail that is not blocks to its end, or an RLE real mask whose
 *                          rectangle the mask data does not hold.
 */
MINIPSDDEF int minipsd_doc_check(const minipsd_doc *doc, const minipsd_write_opts *opts);

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
 * \brief Writes a document as PSD (or PSB) through a callback.
 *
 * The callback receives the whole file front to back, in pieces of up to 4 KiB, and is never
 * asked to seek.  After it returns 0 it is not called again.  The same document always produces
 * the same bytes.  This is the pull writer driven to the end on a workspace from MINIPSD_MALLOC.
 *
 * \param doc  The document to write.
 * \param opts How to write it; NULL: as it is, in its own format.
 * \param fn   Receives the bytes.
 * \param user Passed to fn unchanged.
 * \return MINIPSD_OK or an error code.
 *
 * \retval MINIPSD_OK        The whole file went to fn.
 * \retval MINIPSD_ERR_ARG   doc or fn is NULL, or the document fails minipsd_doc_check with it.
 *                           Nothing was written.
 * \retval MINIPSD_ERR_SIZE  As minipsd_doc_check, or a PSD's layers do not fit a 32-bit section
 *                           length.  Nothing was written.
 * \retval MINIPSD_ERR_UNSUPPORTED As minipsd_doc_check.  Nothing was written.
 * \retval MINIPSD_ERR_ALLOC An allocation failed.  Nothing was written.
 * \retval MINIPSD_ERR_WRITE fn returned 0; the output is incomplete.
 */
MINIPSDDEF int minipsd_write(const minipsd_doc *doc, const minipsd_write_opts *opts, minipsd_write_fn fn, void *user);
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
 * \param opts How it is to be written, as for minipsd_writer_init.
 * \param size Receives the size in bytes.
 * \return MINIPSD_OK or an error code: as minipsd_doc_check, or MINIPSD_ERR_ARG when size is NULL.
 */
MINIPSDDEF int minipsd_writer_workspace_size(const minipsd_doc *doc, const minipsd_write_opts *opts, size_t *size);

/**
 * \brief Prepares a pull writer: checks the document and runs the sizing pass.
 *
 * Every layer channel and every composite row is compressed once here to learn its size, so the
 * file's size is known before its first byte (minipsd_writer_total).  Nothing is allocated.
 *
 * \param w         Receives the writer, built at the start of workspace; NULL on failure.
 * \param doc       The document to write; borrowed for as long as the writer is used.
 * \param opts      How to write it; NULL: as it is, in its own format.  Read here only.
 * \param workspace At least minipsd_writer_workspace_size bytes, any alignment; borrowed likewise.
 * \param size      The size of workspace in bytes.
 * \return MINIPSD_OK or an error code.
 *
 * \retval MINIPSD_ERR_ARG  As minipsd_doc_check, or w or workspace is NULL, or size is too small.
 * \retval MINIPSD_ERR_SIZE As minipsd_doc_check, or a PSD's compressed layers do not fit a 32-bit
 *                          section length.
 * \retval MINIPSD_ERR_UNSUPPORTED As minipsd_doc_check.
 */
MINIPSDDEF int minipsd_writer_init(minipsd_writer **w, const minipsd_doc *doc, const minipsd_write_opts *opts,
                                   minipsd_workspace *workspace, size_t size);

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
 * \brief What the reader refuses to look at.  Zero fields keep the format's own limits.
 *
 * Fields are only ever added at the end, and zero keeps the previous behaviour.
 */
typedef struct minipsd_limits {
    uint32_t max_side;          /*!< Largest width or height of the canvas, checked by init, and of
                                     a layer, checked by next_layer; 0 is the format's own for the
                                     canvas (MINIPSD_MAX_SIDE, MINIPSD_MAX_SIDE_PSB for a PSB) and
                                     no limit for layers, which may reach past it.                */
    uint32_t max_layers;        /*!< Most layers in a file; 0 is 32767, the format's cap.          */
    uint64_t max_decoded_bytes; /*!< Most bytes one decode call may write, width * rows * planes;
                                     0 is no limit.                                              */
} minipsd_limits;

/**
 * \brief What a file says about itself, read by minipsd_reader_init.
 *
 * Any PSD or PSB is described, whatever its colour mode or depth; it is the decoding that is
 * limited to 8- and 16-bit RGB.  Fields are only ever added at the end.
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
    minipsd_bytes layer_tail;          /*!< The layer and mask section after the layer info: the global
                                            layer mask and the global blocks.  data is NULL with a size
                                            when the buffer ends before its end.  With an empty layer
                                            info, a block 'Lr16' or 'Lr32' (Photoshop's 16- and 32-bit
                                            files) holds the layers: next_layer reads them there.      */
    uint16_t      format;              /*!< MINIPSD_PSD or MINIPSD_PSB: the layout of every view of the
                                            file's bytes (channels with their row counts, blocks, the
                                            layer tail).                                               */
} minipsd_info;

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
    uint16_t      other_channels;           /*!< Channels the reader does not read (alpha, spot, ...). */
    uint8_t       clipping;                 /*!< Non-zero: clipped to the layer below.                 */
    uint8_t       flags;                    /*!< The record's flag bits as stored (0x02 is hidden).    */
    uint8_t       kind;                     /*!< MINIPSD_KIND_*, from the record's 'lsct' block; a
                                                 group's own mode is in that block (blend is the
                                                 record's).                                           */
    uint8_t       has_mask;                 /*!< Non-zero: mask holds the layer mask.                  */
    minipsd_mask  mask;                     /*!< The layer mask: its rectangle, colour and flags, its
                                                 channel in stored (data NULL with a size when the
                                                 buffer ends before it), real and rest as views;
                                                 pixels is NULL: minipsd_decode_mask decodes.          */
    uint64_t      mask_offset;              /*!< Where the mask's channel starts in the file (0: none). */
    uint8_t       format;                   /*!< MINIPSD_PSD or MINIPSD_PSB, the file's: how extra's
                                                 blocks and the channels' row counts are laid out.
                                                 0 in a view built by hand is MINIPSD_PSD.             */
    minipsd_bytes blending_ranges;          /*!< The blending ranges as stored, eight bytes each; size
                                                 0: none.  minipsd_layer_blending_range reads one.     */
    uint32_t      blending_range_count;     /*!< How many: blending_ranges.size / 8.                   */
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
 * thumbnail), and nothing after; for layers in a global block (see minipsd_info.layer_tail), up
 * to their count in that block.  A buffer that ends sooner gives MINIPSD_ERR_SHORT: read more and
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
 * \retval MINIPSD_ERR_UNSUPPORTED A PSB whose layer and mask section is larger than size_t holds
 *                                 (on a 32-bit platform).
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
 * \brief One of a layer's blending ranges ("Blend If").
 *
 * \param layer A record from minipsd_reader_next_layer.
 * \param i     Which: 0 is the composite grey's, then each channel's; below
 *              layer->blending_range_count.
 * \param range Receives the range.
 * \return MINIPSD_OK; or MINIPSD_ERR_ARG for a NULL argument or i past the layer's ranges.
 */
MINIPSDDEF int minipsd_layer_blending_range(const minipsd_layer_view *layer, uint32_t i, minipsd_blending_range *range);

/**
 * \brief Where one channel's samples go: the first sample, and the steps to the next one.
 *
 * The library cannot see the size of the memory behind data: it writes exactly the samples of the
 * rows it decodes, at data + y * row_step + x * pixel_step, and nothing else.  Steps count bytes,
 * whatever the depth; a 16-bit sample is two bytes, a uint16_t in host order.
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
 * Its row 0 receives the first decoded row.  Samples are of minipsd_decode_opts.depth; straight, not
 * premultiplied, alpha.
 */
typedef struct minipsd_target {
    minipsd_plane channel[4]; /*!< R, G, B, A. */
} minipsd_target;

/**
 * \brief Which rows to decode, and how.  NULL options decode the whole image into 8-bit samples.
 *
 * Fields are only ever added at the end, and zero keeps the previous behaviour.
 */
typedef struct minipsd_decode_opts {
    uint32_t y_first;    /*!< The first row, counted from the layer's top.                     */
    uint32_t y_count;    /*!< How many rows; 0: from y_first to the end.                        */
    int      keep_matte; /*!< Composite only: non-zero keeps the white matte, as stored.        */
    int      depth;      /*!< The target's bits a sample: 0 or 8, or 16 (a uint16_t in host
                              order).  It must be the file's (minipsd_info.depth).               */
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
 * \brief Points a target at interleaved 16-bit RGBA: four planes two bytes apart, a pixel step of 8.
 *
 * Decode into it with minipsd_decode_opts.depth 16.
 *
 * \param t          The target to fill; nothing happens when it is NULL.
 * \param rgba       The first pixel; NULL leaves every plane NULL.
 * \param row_stride Bytes from a row to the next: width * 8 for a tight image.
 */
MINIPSDDEF void minipsd_target_rgba16(minipsd_target *t, uint16_t *rgba, size_t row_stride);

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
 * \retval MINIPSD_ERR_ARG         r (never initialised), layer or target is NULL, the band lies
 *                                 outside the layer, or opts->depth is not 0, 8 or 16.
 * \retval MINIPSD_ERR_LIMIT       The call would write more than max_decoded_bytes.
 * \retval MINIPSD_ERR_UNSUPPORTED The file is not 8- or 16-bit RGB, its depth is not opts->depth,
 *                                 or a channel is ZIP-compressed.
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
 * undone, giving straight alpha, exact where alpha is full and within about max / (2 * alpha)
 * levels below (max is 255, or 65535 at 16 bits), since matting has already merged nearby colours
 * (0 where alpha is 0).
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
 *                                 the image, the matte is to be undone with no A plane, or
 *                                 opts->depth is not 0, 8 or 16.
 * \retval MINIPSD_ERR_LIMIT       The call would write more than max_decoded_bytes.
 * \retval MINIPSD_ERR_UNSUPPORTED The file is not 8- or 16-bit RGB, its depth is not opts->depth,
 *                                 or the image is ZIP-compressed.
 * \retval MINIPSD_ERR_SHORT       The rows of a channel the target asks for are not all in the
 *                                 buffer; minipsd_reader_needed says how far it must reach.
 * \retval MINIPSD_ERR_FORMAT      The image is malformed.
 */
MINIPSDDEF int minipsd_decode_composite(minipsd_reader *r, const minipsd_target *target, const minipsd_decode_opts *opts);

/**
 * \brief Decodes a layer's mask, or a band of its rows, into one plane.
 *
 * The mask is decoded as stored: its rectangle's width by the rows asked for, one sample per pixel
 * at the file's depth (opts->depth), full where the layer shows; its colour outside the rectangle
 * and its flags are in the view.  As in
 * minipsd_decode_layer, every check comes before the first sample is written, and opts.keep_matte
 * does not apply.
 *
 * \param r     The reader the layer came from.
 * \param layer A record from minipsd_reader_next_layer of that reader, with has_mask set.
 * \param plane Where the samples go.
 * \param opts  The rows; NULL for all of them.
 * \return MINIPSD_OK or an error code, as minipsd_decode_layer; MINIPSD_ERR_ARG also when the
 *         layer has no mask, plane is NULL, or its data is NULL while there are samples to write.
 */
MINIPSDDEF int minipsd_decode_mask(minipsd_reader *r, const minipsd_layer_view *layer, const minipsd_plane *plane,
                                   const minipsd_decode_opts *opts);

/* --- Document from a file ------------------------------------------------------------------ */

/**
 * \brief What a document read from a file could not carry, as bits of minipsd_read's lost.
 *
 * 0 is the expected value.  Anything else is a loud warning: writing the document back drops that
 * content.  Each bit is open work; please report a file that sets one (an issue with the file or a
 * description of it) at the project's address.
 */
enum {
    MINIPSD_LOST_BLENDING_RANGES = 2, /*!< No longer set: blending ranges are kept
                                           (minipsd_layer.blending_ranges).                     */
    MINIPSD_LOST_CHANNELS        = 4  /*!< Channels beyond R, G, B, transparency and the masks':
                                           alpha and spot channels, of a layer or of the composite. */
};

/**
 * \brief How minipsd_read builds the document.  NULL options decode every layer.
 *
 * Fields are only ever added at the end, and zero keeps the previous behaviour.
 */
typedef struct minipsd_read_opts {
    int stored; /*!< Non-zero: each layer keeps its channels as stored (minipsd_layer.stored), not
                     decoded, so writing it back copies them: a fraction of the memory, and no
                     encoding, and ZIP-compressed channels pass.  A layer without a transparency
                     channel is written without one.  The composite too
                     (minipsd_doc.composite_stored, composite NULL), when it holds red, green,
                     blue and at most transparency.                                              */
} minipsd_read_opts;

/**
 * \brief How many bytes minipsd_read_into needs for a file: the document and all it holds.
 *
 * \param data       The whole file.
 * \param size       How many bytes data holds.
 * \param limits     As for minipsd_reader_init; max_decoded_bytes bounds all the pixels decoded,
 *                   every layer and the composite together.  NULL keeps the format's limits.
 * \param opts       How to build the document; NULL decodes every layer.
 * \param block_size Receives the size in bytes, alignment slack included.
 * \return MINIPSD_OK or an error code: as minipsd_read.
 */
MINIPSDDEF int minipsd_read_size(const void *data, size_t size, const minipsd_limits *limits,
                                 const minipsd_read_opts *opts, size_t *block_size);

/**
 * \brief Reads a whole file into one block of the caller's: a document the writer accepts as it is.
 *
 * Everything is copied into the block (names, pixels, the composite, the ICC profile, kept
 * blocks, resources and the layer tail), so data may be freed as soon as the call returns; the
 * document lives as long as the block.  Each layer comes with its rectangle, opacity, visibility,
 * blend key, clipping, flags, decoded pixels (with opts->stored, its channels as stored) and its
 * blocks as they are; the composite as the file stores it, with composite_matted set, so writing
 * the document gives the same image back.  The document has the file's depth, 8 or 16 bits.  A
 * file without layers gets one, "Background", holding its image.  A file without a resolution
 * gets 72 dpi.  Layers a 16-bit file keeps in its 'Lr16' block become the document's layers; the
 * block stays in layer_tail, empty, where the file had it, and the writer puts the layers back
 * there.  With opts->stored, a file without layers stays without: no layer, the composite copied.
 *
 * \param data       The whole file.
 * \param size       How many bytes data holds.
 * \param limits     As for minipsd_read_size.
 * \param opts       As for minipsd_read_size; the same as given there.
 * \param block      At least minipsd_read_size bytes, any alignment.
 * \param block_size The size of block in bytes.
 * \param doc        Receives the document, inside block; NULL on failure.
 * \param lost       Receives MINIPSD_LOST_* bits, 0 when the document carries everything; may be
 *                   NULL.
 * \return MINIPSD_OK or an error code.
 *
 * \retval MINIPSD_ERR_ARG         doc or block is NULL, or block_size is too small.
 * \retval MINIPSD_ERR_UNSUPPORTED Not 8- or 16-bit RGB, ZIP-compressed pixels to decode (with
 *                                 opts->stored they are copied), or a layer wider or taller than
 *                                 the format allows the writer (MINIPSD_MAX_SIDE, _PSB).
 * \retval MINIPSD_ERR_SIZE        The document does not fit in memory (a block above SIZE_MAX).
 * \retval MINIPSD_ERR_FORMAT, MINIPSD_ERR_SHORT, MINIPSD_ERR_LIMIT  As the reader.
 */
MINIPSDDEF int minipsd_read_into(const void *data, size_t size, const minipsd_limits *limits,
                                 const minipsd_read_opts *opts, void *block, size_t block_size,
                                 minipsd_doc **doc, uint32_t *lost);

#ifndef MINIPSD_NO_MALLOC
/**
 * \brief Reads a whole file into a document in one MINIPSD_MALLOC block; minipsd_doc_free frees it.
 *
 * minipsd_read_size and minipsd_read_into on a block of its own: the same document, the same
 * return codes, and MINIPSD_ERR_ALLOC when the allocation fails.
 */
MINIPSDDEF int minipsd_read(const void *data, size_t size, const minipsd_limits *limits,
                            const minipsd_read_opts *opts, minipsd_doc **doc, uint32_t *lost);

/** \brief Frees a document from minipsd_read, through MINIPSD_FREE.  NULL does nothing. */
MINIPSDDEF void minipsd_doc_free(minipsd_doc *doc);
#endif

#endif /* MINIPSD_H */

/* ============================================================================================== */

#ifdef MINIPSD_IMPLEMENTATION
#include "minipsd/base.h"
#include "minipsd/writer.h"
#include "minipsd/reader.h"
#include "minipsd/read.h"
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
