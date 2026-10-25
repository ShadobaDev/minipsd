/**
 * \file
 * \brief minipsd's libvips plugin in a program: an RGB image with alpha saved as a PSD by psdsave,
 *        then read back by psdload, its size and bands as saved.
 *
 *   minipsd-vips-example out.psd
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#include <vips/vips.h>

#include "minipsd_vips.h"

#include <stdio.h>
#include <string.h>

#define SIDE  64   /* pixels a side */
#define BANDS 4    /* RGB and alpha: one layer and the composite */

int main(int argc, char **argv)
{
    VipsImage  *image = NULL, *back = NULL;
    const char *loader;
    int         ok;
    if (argc != 2) {
        fprintf(stderr, "usage: %s out.psd\n", argv[0]);
        return 2;
    }
    if (VIPS_INIT(argv[0])) vips_error_exit(NULL);
    minipsd_vips_init();   /* psdload and psdsave, after VIPS_INIT */

    if (vips_black(&image, SIDE, SIDE, "bands", BANDS, NULL) || vips_image_write_to_file(image, argv[1], NULL)
        || !(back = vips_image_new_from_file(argv[1], NULL)))
        vips_error_exit(NULL);
    loader = vips_foreign_find_load(argv[1]);
    ok     = loader && strcmp(loader, "MinipsdVipsPsdloadFile") == 0 && back->Xsize == SIDE && back->Ysize == SIDE
         && back->Bands == BANDS;
    printf("%s: %dx%d, %d bands, read by %s: %s\n", argv[1], back->Xsize, back->Ysize, back->Bands, loader ? loader : "nothing",
           ok ? "ok" : "FAILED");
    g_object_unref(back);
    g_object_unref(image);
    vips_shutdown();
    return ok ? 0 : 1;
}
