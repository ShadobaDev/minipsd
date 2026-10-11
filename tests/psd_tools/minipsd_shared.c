/**
 * \file
 * \brief minipsd built as a shared library, so roundtrip.py can call the real writer through ctypes.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"
