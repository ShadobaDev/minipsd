/**
 * \file
 * \brief Compiles minipsd.h alone, then again with its implementation in the same file, as C with
 *        extern linkage.  Built, never run: the check is that the public header needs nothing
 *        before it, that a second include adds the implementation, and that the implementation
 *        leaves no internal function unused.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#include "minipsd.h"

#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"
