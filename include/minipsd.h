/**
 * \file
 * \brief A small, dependency-free Photoshop (PSD) writer.  Header-only, C99, also valid C++.
 *
 * \par Usage
 * In exactly one C or C++ file:
 * \code
 *     #define MINIPSD_IMPLEMENTATION
 *     #include "minipsd.h"
 * \endcode
 * Everywhere else include it plainly.
 *
 * \code
 *     minipsd_layer layers[2] = {
 *         { "Template", rgba, 255, 0 },     // bottom
 *         { "Layer",    NULL, 255, 0 },     // empty, above it
 *     };
 *     minipsd_doc doc = { width, height, 300.0, layers, 2, NULL, 0, MINIPSD_RLE };
 *     int rc = minipsd_write_file(&doc, "template.psd");
 *     if (rc != MINIPSD_OK) puts(minipsd_error_string(rc));
 * \endcode
 *
 * \par Configuration
 * Define before the implementation include:
 * - \c MINIPSD_STATIC: make every function static (private to that file);
 * - \c MINIPSD_MALLOC / \c MINIPSD_FREE: replace malloc / free (define both);
 * - \c MINIPSD_NO_STDIO: leave out minipsd_write_file (and <stdio.h>) when bytes only go to a
 *   callback.
 *
 * \par What it writes
 * An 8-bit RGB document (PSD version 1) with a transparency channel:
 * - one or more layers, bottom first, each the full canvas or empty, with a name, opacity and
 *   visibility; names go out both as a legacy Pascal string (ASCII) and as Unicode ('luni');
 * - the composite (merged) image, the layers blended with straight-alpha "over", which is what a
 *   reader that ignores layers shows; its transparency rides as the first extra channel, flagged by
 *   a negative layer count, and its colour is matted against white (c * a + 255 * (1 - a)), as
 *   Photoshop stores it and readers expect to undo;
 * - the resolution, in pixels per inch (resource 1005);
 * - an optional ICC profile (resource 1039); without one, readers assume sRGB.
 *
 * Channel data is PackBits RLE by default (small for flat art), or raw.
 *
 * Not written: 16/32-bit, other colour modes, masks, groups, blend modes other than normal, PSB.
 * PSD caps each side at 30000 px; larger documents are refused (MINIPSD_ERR_SIZE).
 *
 * \par Output
 * Bytes go to a callback in order, never seeking back, so a pipe or a socket works as well as a
 * file.  Allocations are small: each layer channel's compressed size (layer_count * 4 numbers),
 * one row of the composite (width * 4 bytes), and on Windows the UTF-16 copy of
 * minipsd_write_file's path.  Layer pixels are read in place, never copied.
 *
 * \par Files
 * This file includes every module; each also works included alone:
 * - minipsd/base.h: version, linkage, return codes, minipsd_error_string;
 * - minipsd/document.h: minipsd_doc and minipsd_layer;
 * - minipsd/writer.h: minipsd_write and minipsd_write_file.
 * Internal code lives in minipsd/detail/.  Copy the whole include/ directory.
 *
 * \par Project
 * https://github.com/ShadobaDev/minipsd
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#include "minipsd/base.h"
#include "minipsd/document.h"
#include "minipsd/writer.h"

/*
--------------------------------------------------------------------------------------------------
MIT License

Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
--------------------------------------------------------------------------------------------------
*/
