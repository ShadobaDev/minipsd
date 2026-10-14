/**
 * \file
 * \brief The document model: a canvas, its resolution, its layers and the options.
 *
 * Part of minipsd; include minipsd.h for the whole library and its documentation.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#include "base.h"

#ifndef MINIPSD_DOCUMENT_H
#define MINIPSD_DOCUMENT_H

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

#endif /* MINIPSD_DOCUMENT_H */
