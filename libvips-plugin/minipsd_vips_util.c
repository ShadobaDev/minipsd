/**
 * \file
 * \brief The helpers minipsd_vips_util.h declares.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#include "minipsd_vips_util.h"

#include <limits.h>
#include <stdint.h>

const char *minipsd_vips__suffs[] = { ".psd", ".psb", NULL };

void minipsd_vips__class_properties(void *class)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(class);
    gobject_class->set_property = vips_object_set_property;
    gobject_class->get_property = vips_object_get_property;
}

void minipsd_vips__invert_cmyk(void *pixels, size_t count, unsigned bands, int depth)
{
    size_t   i;
    unsigned c;
    for (i = 0; i < count; ++i)
        for (c = 0; c < PSD_CMYK; ++c) {
            if (depth == 32) {
                float *v = (float *)pixels + i * bands + c;
                *v       = 1.0f - *v;
            } else if (depth == 16) {
                uint16_t *v = (uint16_t *)pixels + i * bands + c;
                *v          = (uint16_t)(USHRT_MAX - *v);
            } else {
                uint8_t *v = (uint8_t *)pixels + i * bands + c;
                *v         = (uint8_t)(UCHAR_MAX - *v);
            }
        }
}
