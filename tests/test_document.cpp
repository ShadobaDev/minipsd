/**
 * \file
 * \brief Tests for the document section of minipsd.h: minipsd_doc_check.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_STATIC
#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include "support.h"

#include <limits>
#include <utility>

TEST(MinipsdDocumentTest, AcceptsAPlainDocument)
{
    const Bytes px = testImage(2, 2);
    const minipsd_layer layer{"L", px.data(), 255, 0};
    const minipsd_doc d = docOf(2, 2, &layer, 1);
    EXPECT_EQ(minipsd_doc_check(&d), MINIPSD_OK);
}

TEST(MinipsdDocumentTest, RejectsBadDocuments)
{
    const Bytes px = testImage(2, 2);
    const minipsd_layer layer{"L", px.data(), 255, 0};

    EXPECT_EQ(minipsd_doc_check(nullptr), MINIPSD_ERR_ARG);
    minipsd_doc d = docOf(2, 2, &layer, 0);
    EXPECT_EQ(minipsd_doc_check(&d), MINIPSD_ERR_ARG) << "no layers";
    d = docOf(2, 2, nullptr, 1);
    EXPECT_EQ(minipsd_doc_check(&d), MINIPSD_ERR_ARG) << "a layer count without layers";
    d = docOf(2, 2, &layer, 32768);   // refused before a layer is read
    EXPECT_EQ(minipsd_doc_check(&d), MINIPSD_ERR_ARG) << "more layers than PSD counts";
    // The dpi goes out as 16.16 fixed point: it must round to 1 .. 2^32 - 1.
    for (double dpi : {0.0, -1.0, 1e-9, 65536.0, 1e300,
                       std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        d = docOf(2, 2, &layer, 1); d.dpi = dpi;
        EXPECT_EQ(minipsd_doc_check(&d), MINIPSD_ERR_ARG) << "dpi " << dpi;
    }
    d = docOf(2, 2, &layer, 1); d.icc_size = 4;
    EXPECT_EQ(minipsd_doc_check(&d), MINIPSD_ERR_ARG) << "ICC size without data";
    for (const auto &[w, h] : {std::pair<std::uint32_t, std::uint32_t>{0, 2}, {2, 0},
                               {MINIPSD_MAX_SIDE + 1, 2}, {2, MINIPSD_MAX_SIDE + 1}}) {
        d = docOf(w, h, &layer, 1);
        EXPECT_EQ(minipsd_doc_check(&d), MINIPSD_ERR_SIZE) << w << " x " << h;
    }

    // The resources' length is 32-bit; the ICC size alone decides it, so no ICC byte is read and a
    // 4-byte buffer stands in for 4 GiB.
    const std::uint8_t stand_in[4] = {};
    d = docOf(2, 2, &layer, 1); d.icc = stand_in; d.icc_size = std::numeric_limits<std::uint32_t>::max();
    EXPECT_EQ(minipsd_doc_check(&d), MINIPSD_ERR_SIZE) << "resource section above 4 GiB";
}
