/**
 * \file
 * \brief minipsd as a libvips loader of PSD and PSB: psdload, psdload_buffer, psdload_source.
 *
 * libvips' prebuilt Windows builds load no modules, so a program registers the loader itself: call
 * minipsd_vips_init() once, after VIPS_INIT().  vips_image_new_from_file("x.psd") and the other
 * ways libvips finds a loader then find this one.
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
