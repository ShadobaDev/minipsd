/**
 * \file
 * \brief minipsd.h built with an allocator that fails on demand, so every MINIPSD_ERR_ALLOC path
 *        runs (its own translation unit: MINIPSD_MALLOC must be set before the implementation).
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <utility>

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

    // One allocation: the pull writer's workspace.  Fail it.
    g_allocs_left = 0;
    EXPECT_EQ(minipsd_write(&d, count, &size), MINIPSD_ERR_ALLOC);
    EXPECT_EQ(size, 0u) << "nothing is written when memory runs out";
    g_allocs_left = 1;
    const int rc = minipsd_write(&d, count, &size);
    EXPECT_EQ(rc, MINIPSD_OK) << "one allocation is all a write needs: " << minipsd_error_string(rc);

    // minipsd_read: one allocation, the document's block.  Fail it.
    unsigned char file[1024];
    std::size_t   n = 0;
    const auto into = [](void *user, const void *data, std::size_t len) -> int {
        auto *out = static_cast<std::pair<unsigned char *, std::size_t *> *>(user);
        std::memcpy(out->first + *out->second, data, len);
        *out->second += len;
        return 1;
    };
    std::pair<unsigned char *, std::size_t *> sink{file, &n};
    g_allocs_left = -1;
    ASSERT_EQ(minipsd_write(&d, into, &sink), MINIPSD_OK);
    minipsd_doc *doc = reinterpret_cast<minipsd_doc *>(1);
    g_allocs_left = 0;
    EXPECT_EQ(minipsd_read(file, n, nullptr, nullptr, &doc, nullptr), MINIPSD_ERR_ALLOC);
    EXPECT_EQ(doc, nullptr);
    g_allocs_left = 1;
    ASSERT_EQ(minipsd_read(file, n, nullptr, nullptr, &doc, nullptr), MINIPSD_OK) << "one allocation is all a read needs";
    minipsd_doc_free(doc);
    g_allocs_left = -1;
}
