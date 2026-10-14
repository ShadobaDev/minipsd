/**
 * \file
 * \brief A small, dependency-free Photoshop (PSD) writer.  Header-only, C99, also valid C++.
 *
 * This file is the whole public API: every type, constant and function a caller can use.  The
 * implementation lives in minipsd/ and comes in only with MINIPSD_IMPLEMENTATION; copy the whole
 * include/ directory.  Sections:
 * - Version and linkage
 * - Return codes
 * - Document
 * - Writer
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
 *         { "Template", rgba, 255, 0 },     // bottom
 *         { "Layer",    NULL, 255, 0 },     // empty, above it
 *     };
 *     minipsd_doc doc = { width, height, 300.0, layers, 2, NULL, 0, MINIPSD_RLE };
 *     int rc = minipsd_write_file(&doc, "template.psd");
 *     if (rc != MINIPSD_OK) puts(minipsd_error_string(rc));
 * \endcode
 *
 * \par Configuration
 * Define before the implementation include:
 * - \c MINIPSD_STATIC: make every function static (private to that file);
 * - \c MINIPSD_MALLOC / \c MINIPSD_FREE: replace malloc / free (define both);
 * - \c MINIPSD_NO_STDIO: leave out minipsd_write_file (and <stdio.h>) when bytes only go to a
 *   callback.
 *
 * \par What it writes
 * An 8-bit RGB document (PSD version 1) with a transparency channel:
 * - one or more layers, bottom first, each the full canvas or empty, with a name, opacity and
 *   visibility; names go out both as a legacy Pascal string (ASCII) and as Unicode ('luni');
 * - the composite (merged) image, the layers blended with straight-alpha "over", which is what a
 *   reader that ignores layers shows; its transparency rides as the first extra channel, flagged by
 *   a negative layer count, and its colour is matted against white (c * a + 255 * (1 - a)), as
 *   Photoshop stores it and readers expect to undo;
 * - the resolution, in pixels per inch (resource 1005);
 * - an optional ICC profile (resource 1039); without one, readers assume sRGB.
 *
 * Channel data is PackBits RLE by default (small for flat art), or raw.
 *
 * Not written: 16/32-bit, other colour modes, masks, groups, blend modes other than normal, PSB.
 * PSD caps each side at 30000 px; larger documents are refused (MINIPSD_ERR_SIZE).
 *
 * \par Output
 * Bytes go to a callback in order, never seeking back, so a pipe or a socket works as well as a
 * file.  Allocations are small: each layer channel's compressed size (layer_count * 4 numbers),
 * one row of the composite (width * 4 bytes), and on Windows the UTF-16 copy of
 * minipsd_write_file's path.  Layer pixels are read in place, never copied.
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
#define MINIPSD_VERSION_MINOR 1
#define MINIPSD_VERSION_PATCH 0

#define MPSD__STR(x)  #x
#define MPSD__XSTR(x) MPSD__STR(x)
/** \brief The version as a string literal, e.g. "0.1.0". */
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
    MINIPSD_ERR_OPEN    = -5  /*!< minipsd_write_file could not open (or close) the file.      */
};

/**
 * \brief A short English description of a return code.
 *
 * \param code A value returned by a minipsd function.
 * \return A static string; "unknown error" for codes minipsd does not return.
 */
MINIPSDDEF const char *minipsd_error_string(int code);

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
    const uint8_t *rgba;    /*!< width * height * 4 bytes, rows top down, straight (not premultiplied)
                                 alpha; NULL means an empty layer.                                */
    uint8_t        opacity; /*!< 255 is opaque, 0 is fully transparent.                            */
    uint8_t        hidden;  /*!< Non-zero: the layer starts hidden (and stays out of the composite). */
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
 *                          (0, 65536), or icc_size is set without icc.
 * \retval MINIPSD_ERR_SIZE A side is 0 or above MINIPSD_MAX_SIDE, or the ICC profile does not fit
 *                          a 32-bit section length.
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

/**
 * \brief Writes a document as PSD through a callback.
 *
 * The callback receives the whole file front to back and is never asked to seek.  After it
 * returns 0 it is not called again.  The same document always produces the same bytes.
 *
 * \param doc  The document to write.
 * \param fn   Receives the bytes.
 * \param user Passed to fn unchanged.
 * \return MINIPSD_OK or an error code.
 *
 * \retval MINIPSD_OK        The whole file went to fn.
 * \retval MINIPSD_ERR_ARG   doc or fn is NULL, there are no layers, dpi is outside (0, 65536), or
 *                           icc_size is set without icc.  Nothing was written.
 * \retval MINIPSD_ERR_SIZE  A side is 0 or above MINIPSD_MAX_SIDE, or the layers or the ICC
 *                           profile do not fit a 32-bit section length.  Nothing was written.
 * \retval MINIPSD_ERR_ALLOC An allocation failed.  Nothing was written.
 * \retval MINIPSD_ERR_WRITE fn returned 0; the output is incomplete.
 */
MINIPSDDEF int minipsd_write(const minipsd_doc *doc, minipsd_write_fn fn, void *user);

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
MINIPSDDEF int minipsd_writer_init(minipsd_writer **w, const minipsd_doc *doc, void *workspace, size_t size);

/**
 * \brief The size of the whole file in bytes, known once minipsd_writer_init succeeded.
 *
 * \param w A writer from minipsd_writer_init.
 * \return The file size; 0 when w is NULL.
 */
MINIPSDDEF uint64_t minipsd_writer_total(const minipsd_writer *w);

#ifndef MINIPSD_NO_STDIO
/**
 * \brief Writes a document as PSD to a file, replacing it if it exists.
 *
 * \param doc  The document to write.
 * \param path The file path, UTF-8 on every platform (Windows included).
 * \return MINIPSD_OK or an error code; see minipsd_write.
 *
 * \retval MINIPSD_ERR_ARG  As for minipsd_write, or path is NULL.
 * \retval MINIPSD_ERR_OPEN The file could not be opened or closed.
 *
 * \note A failure after the file was opened leaves the partial file in place.
 */
MINIPSDDEF int minipsd_write_file(const minipsd_doc *doc, const char *path);
#endif

#endif /* MINIPSD_H */

/* ============================================================================================== */

#ifdef MINIPSD_IMPLEMENTATION
#include "minipsd/base.h"
#include "minipsd/writer.h"
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
