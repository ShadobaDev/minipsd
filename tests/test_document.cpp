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

    // A rectangle may lie anywhere, on the canvas or off it; an empty layer's has no area.
    const minipsd_layer anywhere[3] = {
        {.name = "Off", .rgba = px.data(), .opacity = 255, .top = -2147483647 - 1, .left = 9, .bottom = -2147483647 + 1, .right = 11},
        {.name = "Edge", .rgba = px.data(), .opacity = 255, .top = 1, .left = 1, .bottom = 3, .right = 3},
        {.name = "Empty", .rgba = nullptr, .opacity = 255, .top = 5, .left = 5, .bottom = 5, .right = 9}};
    const minipsd_doc r = docOf(2, 2, anywhere, 3);
    EXPECT_EQ(minipsd_doc_check(&r), MINIPSD_OK);
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

    // Rectangles: inverted, an area without pixels, a side PSD's 16-bit row counts cannot hold.
    struct Case { std::int32_t top, left, bottom, right; const std::uint8_t *rgba; int rc; };
    for (const Case &c : {Case{2, 0, 1, 2, px.data(), MINIPSD_ERR_ARG}, Case{0, 2, 2, 1, px.data(), MINIPSD_ERR_ARG},
                          Case{0, 0, 2, 2, nullptr, MINIPSD_ERR_ARG},
                          Case{0, 0, 1, MINIPSD_MAX_SIDE + 1, px.data(), MINIPSD_ERR_SIZE},
                          Case{-MINIPSD_MAX_SIDE, 0, 1, 1, px.data(), MINIPSD_ERR_SIZE},
                          Case{-2147483647 - 1, 0, 2147483647, 1, px.data(), MINIPSD_ERR_SIZE}}) {
        const minipsd_layer bad{.name = "R", .rgba = c.rgba, .opacity = 255,
                                .top = c.top, .left = c.left, .bottom = c.bottom, .right = c.right};
        d = docOf(2, 2, &bad, 1);
        EXPECT_EQ(minipsd_doc_check(&d), c.rc) << c.top << ", " << c.left << ", " << c.bottom << ", " << c.right;
    }

    // Kept content: a size without its data, a resource name longer than a Pascal string, blocks
    // that do not parse.
    const std::uint8_t one[1] = {0};
    const std::string longName(256, 'n');
    const minipsd_resource noData{1036, {nullptr, 4}, {}}, longNamed{1036, {}, {reinterpret_cast<const std::uint8_t *>(longName.data()), 256}};
    d = docOf(2, 2, &layer, 1); d.resource_count = 1;
    EXPECT_EQ(minipsd_doc_check(&d), MINIPSD_ERR_ARG) << "a resource count without resources";
    d.resources = &noData;
    EXPECT_EQ(minipsd_doc_check(&d), MINIPSD_ERR_ARG) << "a resource size without data";
    d.resources = &longNamed;
    EXPECT_EQ(minipsd_doc_check(&d), MINIPSD_ERR_SIZE) << "a resource name over 255 bytes";
    d = docOf(2, 2, &layer, 1); d.layer_tail = {nullptr, 4};
    EXPECT_EQ(minipsd_doc_check(&d), MINIPSD_ERR_ARG) << "a layer tail size without data";
    for (const minipsd_bytes extra : {minipsd_bytes{nullptr, 12}, minipsd_bytes{one, 1}}) {
        const minipsd_layer bad{.name = "L", .rgba = px.data(), .opacity = 255, .extra = extra};
        d = docOf(2, 2, &bad, 1);
        EXPECT_EQ(minipsd_doc_check(&d), MINIPSD_ERR_ARG) << "extra of " << extra.size << " bytes";
    }

    // Stored channels: each must fit the rectangle, and a layer with only them needs a composite.
    const Bytes raw4 = {0, 0, 1, 2, 3, 4}, raw3 = {0, 0, 1, 2, 3}, rle = {0, 1, 0, 3, 0, 3, 0x01, 7, 7, 0x01, 7, 7};
    const Bytes zip = {0, 2, 9}, bad = {0, 9, 0, 0, 0, 0};
    auto storedDoc = [&](const Bytes &ch, const std::uint8_t *rgba, const std::uint8_t *composite) {
        static minipsd_bytes four[4];
        static minipsd_layer l;
        for (minipsd_bytes &c : four) c = {ch.data(), ch.size()};
        l = {.name = "S", .rgba = rgba, .opacity = 255, .top = 0, .left = 0, .bottom = 2, .right = 2, .stored = four};
        minipsd_doc s = docOf(2, 2, &l, 1);
        s.composite = composite;
        return minipsd_doc_check(&s);
    };
    EXPECT_EQ(storedDoc(raw4, nullptr, px.data()), MINIPSD_OK);
    EXPECT_EQ(storedDoc(rle, nullptr, px.data()), MINIPSD_OK);
    EXPECT_EQ(storedDoc(zip, nullptr, px.data()), MINIPSD_OK) << "ZIP is copied, not looked into";
    EXPECT_EQ(storedDoc(raw4, px.data(), nullptr), MINIPSD_OK) << "the pixels compose it";
    EXPECT_EQ(storedDoc(raw4, nullptr, nullptr), MINIPSD_ERR_ARG) << "nothing to compose the composite from";
    EXPECT_EQ(storedDoc(raw3, nullptr, px.data()), MINIPSD_ERR_ARG) << "raw rows short of the rectangle";
    EXPECT_EQ(storedDoc(bad, nullptr, px.data()), MINIPSD_ERR_ARG) << "an unknown compression";

    // The resources' length is 32-bit; the ICC size alone decides it, so no ICC byte is read and a
    // 4-byte buffer stands in for 4 GiB.
    const std::uint8_t stand_in[4] = {};
    d = docOf(2, 2, &layer, 1); d.icc = stand_in; d.icc_size = std::numeric_limits<std::uint32_t>::max();
    EXPECT_EQ(minipsd_doc_check(&d), MINIPSD_ERR_SIZE) << "resource section above 4 GiB";
}
