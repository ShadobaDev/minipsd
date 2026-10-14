/**
 * \file
 * \brief Internal: PackBits run-length encoding over a strided source.  Included by the writer
 *        only.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#ifndef MPSD__PACKBITS_DONE
#define MPSD__PACKBITS_DONE

#include "bytes.h"

/* Encodes n bytes, src[0], src[stride], ...: one channel out of interleaved RGBA. */
static void mpsd__packbits(mpsd__sink *s, const uint8_t *src, size_t n, size_t stride)
{
    uint8_t packet[129];
    size_t  i = 0;
    while (i < n) {
        const uint8_t v = src[i * stride];
        size_t run = 1;
        while (i + run < n && run < 128 && src[(i + run) * stride] == v) ++run;
        if (run >= 3) {                       /* a repeat: 1 - run, then the byte */
            packet[0] = (uint8_t)(257 - run);
            packet[1] = v;
            mpsd__put(s, packet, 2);
            i += run;
        } else {                              /* literals, up to the next run of three */
            size_t len = 0;
            while (i < n && len < 128) {
                if (i + 2 < n && src[i * stride] == src[(i + 1) * stride]
                              && src[i * stride] == src[(i + 2) * stride])
                    break;
                packet[1 + len++] = src[i++ * stride];
            }
            packet[0] = (uint8_t)(len - 1);
            mpsd__put(s, packet, len + 1);
        }
    }
}

#endif /* MPSD__PACKBITS_DONE */
