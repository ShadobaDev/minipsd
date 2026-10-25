/**
 * \file
 * \brief minipsd as a libvips loader and saver of PSD and PSB: psdload, psdload_buffer,
 *        psdload_source; psdsave, psdsave_buffer, psdsave_target.
 *
 * libvips' prebuilt Windows builds load no modules, so a program registers them itself: call
 * minipsd_vips_init() once, after VIPS_INIT().  vips_image_new_from_file("x.psd"),
 * vips_image_write_to_file(image, "x.psd") and the other ways libvips finds a loader or a saver
 * then find these.  A saver writes one image; a layered document is psdsave_layers, an
 * operation over an array of images, one a layer (vips_psdsave_layers below).
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MINIPSD_VIPS_H
#define MINIPSD_VIPS_H

#include <vips/vips.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief psdsave's `compression`: of the layer's channels; the composite is RLE, or raw with none.
 *
 * From the command line or an option string, by its nick: `compression=zip`.
 */
typedef enum {
    VIPS_FOREIGN_PSD_COMPRESSION_RLE  = 0, /*!< PackBits, as Photoshop writes 8-bit layers: the default. */
    VIPS_FOREIGN_PSD_COMPRESSION_ZIP  = 1, /*!< zlib, without prediction.                               */
    VIPS_FOREIGN_PSD_COMPRESSION_NONE = 2, /*!< Raw: the largest file.                                   */
    VIPS_FOREIGN_PSD_COMPRESSION_ZIP_PREDICTED = 3 /*!< zlib of each sample's difference from its left
                                                    neighbour, as Photoshop writes 16-bit layers:
                                                    the smaller file at 16 and 32 bits.          */
} VipsForeignPsdCompression;

/**
 * \brief Registers minipsd's operations with libvips.  Safe to call more than once.
 *
 * \return 0.
 */
int minipsd_vips_init(void);

/**
 * \brief Saves images as the layers of one PSD or PSB (the operation psdsave_layers).
 *
 * in holds the layers, bottom first.  Each image's psd-layer-* fields, the ones psdload sets,
 * give its layer's name ("psd-layer-name", else "Layer N"), opacity ("psd-layer-opacity", 0 to
 * 255), visibility ("psd-layer-hidden"), blend mode ("psd-layer-blend-mode", a four-character
 * key such as "mul ") and clipping ("psd-layer-clipping"); its position is its xoffset and
 * yoffset.  "psd-layer-empty" set makes it an empty layer, no pixels, as Photoshop writes an
 * unpainted one (the image's pixels are ignored).  Every layer with pixels is of one mode and
 * depth (grey, RGB or CMYK, with or without alpha; uchar, ushort or float); none is converted.
 *
 * A mask is an image in "psd-layer-mask": one band of the layers' depth, its own rectangle in its
 * xoffset and yoffset, with "psd-layer-mask-default-color" (0 or 255, its value outside; 255
 * without it) and "psd-layer-mask-disabled".  A group is records in the list, as the file has
 * them: below its layers an image with "psd-layer-section" "end", above them one with "open" or
 * "closed" and the group's name, visibility, opacity and blend mode ("pass", or none, passes
 * through).  Records have no pixels.  psdload gives masks and records the same way.
 *
 * Options, as NULL-ended name and value pairs: "composite" (a VipsImage, the canvas's size: the
 * composite as given; else vips_composite of the visible layers, blend modes mapped as the spec's
 * table, masks and groups' visibility and opacity in the layers' alpha, flattened against white
 * when no layer has alpha; clipping, and a group's own mode and mask, are not drawn), "compression"
 * (VipsForeignPsdCompression, the layers'), "psb", "width" and "height" (the canvas; 0: the
 * composite's size, else every layer's rectangle from (left, top)), "left" and "top" (where the
 * canvas lies in the layers' coordinates).  Each image is read once for its layer, and once more
 * by vips_composite unless the composite is given.
 *
 * \return 0, or -1 with libvips' error set.
 */
int vips_psdsave_layers(VipsImage **in, int n, const char *filename, ...) G_GNUC_NULL_TERMINATED;

/** \brief vips_psdsave_layers into a target. */
int vips_psdsave_layers_target(VipsImage **in, int n, VipsTarget *target, ...) G_GNUC_NULL_TERMINATED;

/** \brief vips_psdsave_layers into a new buffer, *buf (g_free it) of *len bytes. */
int vips_psdsave_layers_buffer(VipsImage **in, int n, void **buf, size_t *len, ...) G_GNUC_NULL_TERMINATED;

#ifdef __cplusplus
}
#endif

#endif /* MINIPSD_VIPS_H */
