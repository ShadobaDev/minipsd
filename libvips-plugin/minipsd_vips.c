/**
 * \file
 * \brief minipsd_vips_init: registers psdload and psdsave with libvips.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#include "minipsd_vips.h"

#include "foreign/psd.h"

/* --- Registration -------------------------------------------------------------------------- */

int minipsd_vips_init(void)
{
    /* Registering a type is enough: libvips finds loaders and savers among VipsForeignLoad's and
       VipsForeignSave's subclasses. */
    (void)minipsd_vips_load_file_get_type();
    (void)minipsd_vips_load_buffer_get_type();
    (void)minipsd_vips_load_source_get_type();
    (void)minipsd_vips_save_file_get_type();
    (void)minipsd_vips_save_buffer_get_type();
    (void)minipsd_vips_save_target_get_type();
    (void)minipsd_vips_save_layers_file_get_type();
    (void)minipsd_vips_save_layers_buffer_get_type();
    (void)minipsd_vips_save_layers_target_get_type();
    return 0;
}
