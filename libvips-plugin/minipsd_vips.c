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
    (void)vips_foreign_load_psd_file_get_type();
    (void)vips_foreign_load_psd_buffer_get_type();
    (void)vips_foreign_load_psd_source_get_type();
    (void)vips_foreign_save_psd_file_get_type();
    (void)vips_foreign_save_psd_buffer_get_type();
    (void)vips_foreign_save_psd_target_get_type();
    (void)vips_foreign_save_psd_layers_file_get_type();
    (void)vips_foreign_save_psd_layers_buffer_get_type();
    (void)vips_foreign_save_psd_layers_target_get_type();
    return 0;
}
