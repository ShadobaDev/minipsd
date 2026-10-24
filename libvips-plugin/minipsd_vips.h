/**
 * \file
 * \brief minipsd as a libvips loader and saver of PSD and PSB: psdload, psdload_buffer,
 *        psdload_source; psdsave, psdsave_buffer, psdsave_target.
 *
 * libvips' prebuilt Windows builds load no modules, so a program registers them itself: call
 * minipsd_vips_init() once, after VIPS_INIT().  vips_image_new_from_file("x.psd"),
 * vips_image_write_to_file(image, "x.psd") and the other ways libvips finds a loader or a saver
 * then find these.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MINIPSD_VIPS_H
#define MINIPSD_VIPS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief Registers minipsd's operations with libvips.  Safe to call more than once.
 *
 * \return 0.
 */
int minipsd_vips_init(void);

#ifdef __cplusplus
}
#endif

#endif /* MINIPSD_VIPS_H */
