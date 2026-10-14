/**
 * \file
 * \brief What every minipsd module shares: the version, linkage, return codes and their texts.
 *
 * Part of minipsd; include minipsd.h for the whole library and its documentation.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MINIPSD_BASE_H
#define MINIPSD_BASE_H

#include <stddef.h>
#include <stdint.h>

/**
 * \brief The version of minipsd, MAJOR.MINOR.PATCH.
 *
 * The single source of the project version: CMakeLists.txt reads these three lines.
 */
#define MINIPSD_VERSION_MAJOR 0
#define MINIPSD_VERSION_MINOR 1
#define MINIPSD_VERSION_PATCH 0

#define MPSD__STR(x)  #x
#define MPSD__XSTR(x) MPSD__STR(x)
/** \brief The version as a string literal, e.g. "0.1.0". */
#define MINIPSD_VERSION_STRING \
    MPSD__XSTR(MINIPSD_VERSION_MAJOR) "." MPSD__XSTR(MINIPSD_VERSION_MINOR) "." MPSD__XSTR(MINIPSD_VERSION_PATCH)

/** \brief Linkage of every public function: static under MINIPSD_STATIC, otherwise extern (C). */
#ifdef MINIPSD_STATIC
#  define MINIPSDDEF static
#elif defined(__cplusplus)
#  define MINIPSDDEF extern "C"
#else
#  define MINIPSDDEF extern
#endif

/** \brief Return codes: MINIPSD_OK, or one of the negative errors. */
enum {
    MINIPSD_OK          =  0, /*!< Success.                                                   */
    MINIPSD_ERR_ARG     = -1, /*!< A NULL document, no layers, or a dpi outside (0, 65536).    */
    MINIPSD_ERR_SIZE    = -2, /*!< A side is 0 or above MINIPSD_MAX_SIDE, or the document is too
                                   large for PSD's 32-bit section lengths (about 4 GiB).      */
    MINIPSD_ERR_WRITE   = -3, /*!< The write callback reported a failure.                      */
    MINIPSD_ERR_ALLOC   = -4, /*!< MINIPSD_MALLOC returned NULL.                               */
    MINIPSD_ERR_OPEN    = -5  /*!< minipsd_write_file could not open (or close) the file.      */
};

/**
 * \brief A short English description of a return code.
 *
 * \param code A value returned by a minipsd function.
 * \return A static string; "unknown error" for codes minipsd does not return.
 */
MINIPSDDEF const char *minipsd_error_string(int code);

#endif /* MINIPSD_BASE_H */

/* ============================================================================================== */

#if defined(MINIPSD_IMPLEMENTATION) && !defined(MPSD__BASE_DONE)
#define MPSD__BASE_DONE

MINIPSDDEF const char *minipsd_error_string(int code)
{
    switch (code) {
    case MINIPSD_OK:        return "success";
    case MINIPSD_ERR_ARG:   return "invalid argument";
    case MINIPSD_ERR_SIZE:  return "too large for PSD: a side outside 1..30000, or a section above 4 GiB";
    case MINIPSD_ERR_WRITE: return "write failed";
    case MINIPSD_ERR_ALLOC: return "out of memory";
    case MINIPSD_ERR_OPEN:  return "cannot open or close the file";
    default:                return "unknown error";
    }
}

#endif /* MINIPSD_IMPLEMENTATION */
