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
    case MINIPSD_OK:                return "success";
    case MINIPSD_MORE:              return "not finished: call again";
    case MINIPSD_ERR_ARG:           return "invalid argument";
    case MINIPSD_ERR_SIZE:          return "too large: a side above the format's (30000 PSD, 300000 PSB), or a section above its length field";
    case MINIPSD_ERR_WRITE:         return "write failed";
    case MINIPSD_ERR_ALLOC:         return "out of memory";
    case MINIPSD_ERR_OPEN:          return "cannot open or close the file";
    case MINIPSD_ERR_FORMAT:        return "not a well-formed PSD";
    case MINIPSD_ERR_UNSUPPORTED:   return "valid, but outside what minipsd handles yet";
    case MINIPSD_ERR_SHORT:         return "the buffer ends too soon: read more and call again";
    case MINIPSD_ERR_LIMIT:         return "over a limit set by the caller";
    case MINIPSD_ERR_NOT_FOUND:     return "not found";
    case MINIPSD_ERR_USE_DOC_FIELD: return "not made: a field of the document says it";
    default:                        return "unknown error";
    }
}

#endif /* MPSD__BASE_DONE */
