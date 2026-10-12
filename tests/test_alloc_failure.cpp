/**
 * \file
 * \brief minipsd.h built with an allocator that fails on demand, so every MINIPSD_ERR_ALLOC path
 *        runs (its own translation unit: MINIPSD_MALLOC must be set before the implementation),
 *        and with MINIPSD_NO_STDIO.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#include <cstddef>
#include <cstdlib>

namespace {
int g_allocs_left = -1;   // allocations that still succeed; -1: all of them

void *failingMalloc(std::size_t size)
{
    if (g_allocs_left == 0) return nullptr;
    if (g_allocs_left > 0) --g_allocs_left;
    return std::malloc(size);
}
}   // namespace

#define MINIPSD_STATIC
#define MINIPSD_NO_STDIO   // also checks the build without minipsd_write_file
#define MINIPSD_MALLOC(size) failingMalloc(size)
#define MINIPSD_FREE(ptr)    std::free(ptr)
#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include <gtest/gtest.h>

TEST(MinipsdAllocTest, EachFailedAllocationIsReported)
{
    const unsigned char px[4] = {255, 0, 0, 255};
    const minipsd_layer layer{"L", px, 255, 0};
    minipsd_doc d{};
    d.width = 1; d.height = 1; d.dpi = 72.0; d.layers = &layer; d.layer_count = 1;
    std::size_t size = 0;
    const auto count = [](void *user, const void *, std::size_t n) -> int {
        *static_cast<std::size_t *>(user) += n;
        return 1;
    };

    // Two allocations: the channel sizes, then the composite row.  Fail each in turn.
    for (int succeed : {0, 1}) {
        g_allocs_left = succeed;
        size = 0;
        EXPECT_EQ(minipsd_write(&d, count, &size), MINIPSD_ERR_ALLOC) << succeed << " allocations succeed";
        EXPECT_EQ(size, 0u) << "nothing is written when memory runs out";
    }
    g_allocs_left = 2;
    const int rc = minipsd_write(&d, count, &size);
    EXPECT_EQ(rc, MINIPSD_OK) << "two allocations are all a write needs: " << minipsd_error_string(rc);
    g_allocs_left = -1;
}
