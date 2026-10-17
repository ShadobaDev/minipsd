/**
 * \file minipsd/base.h
 * \brief Implementation: the texts of the return codes.  Included by minipsd.h under
 *        MINIPSD_IMPLEMENTATION; the declarations are in minipsd.h.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MPSD__BASE_DONE
#define MPSD__BASE_DONE

MINIPSDDEF const char *minipsd_error_string(int code)
{
    switch (code) {
    case MINIPSD_OK:              return "success";
    case MINIPSD_MORE:            return "not finished: call again";
    case MINIPSD_ERR_ARG:         return "invalid argument";
    case MINIPSD_ERR_SIZE:        return "too large for PSD: a side outside 1..30000, or a section above 4 GiB";
    case MINIPSD_ERR_WRITE:       return "write failed";
    case MINIPSD_ERR_ALLOC:       return "out of memory";
    case MINIPSD_ERR_OPEN:        return "cannot open or close the file";
    case MINIPSD_ERR_FORMAT:      return "not a well-formed PSD";
    case MINIPSD_ERR_UNSUPPORTED: return "a valid PSD outside what minipsd reads";
    case MINIPSD_ERR_SHORT:       return "the buffer ends too soon: read more and call again";
    case MINIPSD_ERR_LIMIT:       return "over a limit set by the caller";
    default:                      return "unknown error";
    }
}

#endif /* MPSD__BASE_DONE */
