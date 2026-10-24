/**
 * \file
 * \brief minipsd_vips_init: registers psdload and psdsave with libvips.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#include "minipsd_vips.h"

#include "minipsd_vips_psdload.h"
#include "minipsd_vips_psdsave.h"

/* --- Registration -------------------------------------------------------------------------- */

int minipsd_vips_init(void)
{
    /* Registering a type is enough: libvips finds loaders and savers among VipsForeignLoad's and
       VipsForeignSave's subclasses. */
    (void)minipsd_vips_psdload_file_get_type();
    (void)minipsd_vips_psdload_buffer_get_type();
    (void)minipsd_vips_psdload_source_get_type();
    (void)minipsd_vips_psdsave_file_get_type();
    (void)minipsd_vips_psdsave_buffer_get_type();
    (void)minipsd_vips_psdsave_target_get_type();
    return 0;
}
