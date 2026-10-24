/**
 * \file
 * \brief Internal to the plugin: the psdload operations' types (minipsd_vips_psdload.c).
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MINIPSD_VIPS_PSDLOAD_H
#define MINIPSD_VIPS_PSDLOAD_H

#include <vips/vips.h>

/* The abstract base, then the three made from it; minipsd_vips_init registers the three. */
GType minipsd_vips_psdload_get_type(void);
GType minipsd_vips_psdload_source_get_type(void);
GType minipsd_vips_psdload_file_get_type(void);
GType minipsd_vips_psdload_buffer_get_type(void);

#endif /* MINIPSD_VIPS_PSDLOAD_H */
