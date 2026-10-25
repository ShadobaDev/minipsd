/**
 * \file
 * \brief Internal to the plugin: what the loader and the saver share.
 *
 * Constants, the PSD_ARGS_* pair, the few helpers both sides use (defined once in psd.c, prefixed
 * vips__foreign_psd_, as libvips prefixes what its parts share), and the operations' types, which
 * minipsd_vips_init registers.  As libvips keeps TIFF's shared code in foreign/tiff.c and tiff.h.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef VIPS_FOREIGN_PSD_H
#define VIPS_FOREIGN_PSD_H

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
extern const char *vips__foreign_psd_suffs[];

/* A record's psd-layer-section, by its MINIPSD_SECTION_* type: NULL for a layer, then "open",
   "closed" (a group's top record, shown open or closed) and "end" (the divider below a group's
   layers).  psdload sets it, psdsave_layers reads it. */
#define PSD_SECTIONS 4
extern const char *const vips__foreign_psd_sections[PSD_SECTIONS];

/* GObject resets set_property and get_property in every class (g_object_base_class_init), so
   each class with arguments of its own sets libvips' again. */
void vips__foreign_psd_class_properties(void *class);

/* CMYK the other way round, in place: PSD stores ink inverted (full: no ink), libvips holds it
   as TIFF does (0: no ink), as its own jpegload makes Adobe's inverted CMYK and ImageMagick
   makes PSD's.  The colour channels only, not transparency; both ways, loading and saving.
   count pixels of bands samples, each of depth bits (8, 16, or 32 as floats). */
void vips__foreign_psd_invert_cmyk(void *pixels, size_t count, unsigned bands, int depth);

/* The operations: each abstract base, then the three made from it (psdload.c, psdsave.c). */
GType vips_foreign_load_psd_get_type(void);
GType vips_foreign_load_psd_source_get_type(void);
GType vips_foreign_load_psd_file_get_type(void);
GType vips_foreign_load_psd_buffer_get_type(void);
GType vips_foreign_save_psd_get_type(void);
GType vips_foreign_save_psd_target_get_type(void);
GType vips_foreign_save_psd_file_get_type(void);
GType vips_foreign_save_psd_buffer_get_type(void);
GType vips_foreign_save_psd_layers_get_type(void);
GType vips_foreign_save_psd_layers_target_get_type(void);
GType vips_foreign_save_psd_layers_file_get_type(void);
GType vips_foreign_save_psd_layers_buffer_get_type(void);

#endif /* VIPS_FOREIGN_PSD_H */
