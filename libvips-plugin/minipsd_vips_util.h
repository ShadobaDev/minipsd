/**
 * \file
 * \brief Internal to the plugin: what the loader and the saver share.
 *
 * Constants, the PSD_ARGS_* pair, and the few helpers both sides use, defined once in
 * minipsd_vips_util.c (prefixed minipsd_vips__, since a static library's names meet the program's).
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MINIPSD_VIPS_UTIL_H
#define MINIPSD_VIPS_UTIL_H

#include <vips/vips.h>

#include <stddef.h>

#define PSD_RGB      3       /* RGB's colour channels */
#define PSD_CMYK     4       /* CMYK's colour channels */
#define MM_PER_INCH  25.4

/* libvips' VIPS_ARG_* macros pass vips_argument_get_id()'s int as a guint property id, and
   G_STRUCT_OFFSET's glong as a guint offset, which -Wsign-conversion and -Wconversion report in
   our code; only around them are they off. */
#if defined(__GNUC__)
#    define PSD_ARGS_BEGIN                                                                     \
        _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wsign-conversion\"") \
            _Pragma("GCC diagnostic ignored \"-Wconversion\"")
#    define PSD_ARGS_END   _Pragma("GCC diagnostic pop")
#else
#    define PSD_ARGS_BEGIN
#    define PSD_ARGS_END
#endif

/* The suffixes libvips matches files by: .psd and .psb. */
extern const char *minipsd_vips__suffs[];

/* GObject resets set_property and get_property in every class (g_object_base_class_init), so
   each class with arguments of its own sets libvips' again. */
void minipsd_vips__class_properties(void *class);

/* CMYK the other way round, in place: PSD stores ink inverted (full: no ink), libvips holds it
   as TIFF does (0: no ink), as its own jpegload makes Adobe's inverted CMYK and ImageMagick
   makes PSD's.  The colour channels only, not transparency; both ways, loading and saving.
   count pixels of bands samples, each of depth bits (8, 16, or 32 as floats). */
void minipsd_vips__invert_cmyk(void *pixels, size_t count, unsigned bands, int depth);

#endif /* MINIPSD_VIPS_UTIL_H */
