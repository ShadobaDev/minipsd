/**
 * \file
 * \brief Internal to the plugin: the psdsave operations' types (minipsd_vips_psdsave.c).
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MINIPSD_VIPS_PSDSAVE_H
#define MINIPSD_VIPS_PSDSAVE_H

#include <vips/vips.h>

/* The abstract base, then the three made from it; minipsd_vips_init registers the three. */
GType minipsd_vips_psdsave_get_type(void);
GType minipsd_vips_psdsave_target_get_type(void);
GType minipsd_vips_psdsave_file_get_type(void);
GType minipsd_vips_psdsave_buffer_get_type(void);

#endif /* MINIPSD_VIPS_PSDSAVE_H */
