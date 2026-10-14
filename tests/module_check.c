/**
 * \file
 * \brief Compiles one public module alone, with its implementation, as C (MINIPSD_CHECK_HEADER names
 *        it).  Built, never run: the check is that each header includes what it uses and that no
 *        internal function goes unused.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_IMPLEMENTATION
#include MINIPSD_CHECK_HEADER

typedef int minipsd_module_check;   /* ISO C: a translation unit declares something */
