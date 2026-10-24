/**
 * \file minipsd/detail/floats.h
 * \brief Internal: floating-point values compared within a tolerance, never with == or !=, and
 *        checked to be finite; no libm (minipsd links nothing beyond the C library).
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MPSD__FLOATS_DONE
#define MPSD__FLOATS_DONE

#include <float.h>

/* Two ratios this close say the same: a pixel aspect ratio kept from a file and the document's. */
#define MPSD__RATIO_EPSILON 1e-9

/* Are a and b within epsilon of each other? */
MPSD__SHARED static int mpsd__floats_equal(double a, double b) { return a - b < MPSD__RATIO_EPSILON && b - a < MPSD__RATIO_EPSILON; }

/* Is v a finite value above 0, or at least 0?  NaN fails every comparison, so both. */
MPSD__SHARED static int mpsd__floats_positive(double v) { return v > 0.0 && v <= DBL_MAX; }
MPSD__SHARED static int mpsd__floats_not_negative(double v) { return v >= 0.0 && v <= DBL_MAX; }

#endif /* MPSD__FLOATS_DONE */
