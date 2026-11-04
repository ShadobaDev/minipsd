/**
 * \file
 * \brief Edge cases across the modules, aimed at the branches the other files leave untaken (C1):
 *        each NULL argument on its own, counts and sizes at the edge of overflow, empty inputs,
 *        cursors outside a list, buffers one byte short, and the float and slot helpers at their
 *        limits (infinity, NaN, DBL_MAX, channel ids past the colours).
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_STATIC
#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include "support.h"

#include <cfloat>
#include <cstring>
#include <limits>

namespace {

minipsd_resource resourceOf(std::uint16_t id, const Bytes &data)
{
    return minipsd_resource{id, {data.data(), data.size()}, {nullptr, 0}};
}

// ---------------------------------------------------------------------------------------------------
// the rows: encode, predict, matte
// ---------------------------------------------------------------------------------------------------

TEST(MinipsdEdgeTest, RowFunctionsRefuseEachNullAndBadDepth)
{
    const std::uint8_t row[4] = {1, 2, 3, 4};
    std::uint8_t       out[16];
    std::size_t        size = 0;

    EXPECT_EQ(minipsd_encode_row(MINIPSD_RAW, 8, nullptr, 1, 4, out, sizeof out, &size), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_encode_row(MINIPSD_RAW, 8, row, 1, 4, nullptr, sizeof out, &size), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_encode_row(MINIPSD_RAW, 8, row, 1, 4, out, sizeof out, nullptr), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_encode_row(99, 8, row, 1, 4, out, sizeof out, &size), MINIPSD_ERR_ARG) << "no such compression";
    EXPECT_EQ(minipsd_encode_row(MINIPSD_RAW, 7, row, 1, 4, out, sizeof out, &size), MINIPSD_ERR_ARG) << "no such depth";

    // Too small by one byte: refused, and the size it needs reported.
    EXPECT_EQ(minipsd_encode_row(MINIPSD_RAW, 8, row, 1, 4, out, 3, &size), MINIPSD_ERR_SIZE);
    EXPECT_EQ(size, 4u);
    EXPECT_EQ(minipsd_encode_row(MINIPSD_RAW, 8, row, 1, 4, out, 4, &size), MINIPSD_OK);
    EXPECT_EQ(size, 4u);
    // An empty row: nothing raw.
    EXPECT_EQ(minipsd_encode_row(MINIPSD_RAW, 8, row, 1, 0, out, sizeof out, &size), MINIPSD_OK);
    EXPECT_EQ(size, 0u);

    EXPECT_EQ(minipsd_predict_row(8, nullptr, 1, 4, out, sizeof out), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_predict_row(8, row, 1, 4, nullptr, sizeof out), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_predict_row(1, row, 1, 4, out, sizeof out), MINIPSD_ERR_ARG) << "1-bit has no prediction";
    EXPECT_EQ(minipsd_predict_row(7, row, 1, 4, out, sizeof out), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_predict_row(8, row, 1, 4, out, 3), MINIPSD_ERR_SIZE) << "one byte short";
    EXPECT_EQ(minipsd_predict_row(8, row, 1, 4, out, 4), MINIPSD_OK);
    EXPECT_EQ(Bytes(out, out + 4), (Bytes{1, 1, 1, 1})) << "the differences";

    std::uint8_t pixels[4] = {0, 0, 0, 0};
    EXPECT_EQ(minipsd_matte_row(MINIPSD_MODE_RGB, 8, nullptr, 1), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_matte_row(MINIPSD_MODE_RGB, 1, pixels, 1), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_matte_row(MINIPSD_MODE_RGB, 7, pixels, 1), MINIPSD_ERR_ARG);
}

// ---------------------------------------------------------------------------------------------------
// the resource setters (builders.h)
// ---------------------------------------------------------------------------------------------------

TEST(MinipsdEdgeTest, VersionInfoNamesWithoutDataAreRefused)
{
    minipsd_version_info vi{};
    std::size_t          needed = 0;
    // Both names empty, no data: valid, the head and two empty strings.
    EXPECT_EQ(minipsd_resource_make_version_info(&vi, nullptr, 0, &needed), MINIPSD_MORE);
    EXPECT_GT(needed, 0u);

    vi.writer_name = {nullptr, 2};   // a size without data
    EXPECT_EQ(minipsd_resource_make_version_info(&vi, nullptr, 0, &needed), MINIPSD_ERR_ARG);
    vi.writer_name = {nullptr, 0};
    vi.reader_name = {nullptr, 2};
    EXPECT_EQ(minipsd_resource_make_version_info(&vi, nullptr, 0, &needed), MINIPSD_ERR_ARG);

    const std::uint8_t unit[3] = {0, 'A', 0};
    vi.reader_name = {nullptr, 0};
    vi.writer_name = {unit, 3};      // half a UTF-16 unit, in the writer's name this time
    EXPECT_EQ(minipsd_resource_make_version_info(&vi, nullptr, 0, &needed), MINIPSD_ERR_ARG);
}

TEST(MinipsdEdgeTest, ResourceSettersTakeEmptyListsAndRefuseOverflowingCounts)
{
    const minipsd_grid_and_guides grid{1, 576, 576, 0};
    std::size_t                   needed = 0;

    // A count past what the file's four bytes hold: refused before a guide is read.
    if constexpr (sizeof(std::size_t) > sizeof(std::uint32_t)) {
        const minipsd_guide one{0, 0};
        const std::size_t   tooMany = static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
        EXPECT_EQ(minipsd_resource_make_grid_and_guides(&grid, &one, tooMany, nullptr, 0, &needed), MINIPSD_ERR_ARG);
    }

    // Alpha names: the Unicode id with a count and no names; no names at all is an empty list.
    EXPECT_EQ(minipsd_resource_make_alpha_names(MINIPSD_RES_UNICODE_ALPHA_NAMES, nullptr, 1, nullptr, 0, &needed),
              MINIPSD_ERR_ARG);
    needed = 99;
    EXPECT_EQ(minipsd_resource_make_alpha_names(MINIPSD_RES_UNICODE_ALPHA_NAMES, nullptr, 0, nullptr, 0, &needed), MINIPSD_OK);
    EXPECT_EQ(needed, 0u);
    needed = 99;
    EXPECT_EQ(minipsd_resource_make_alpha_names(MINIPSD_RES_ALPHA_NAMES, nullptr, 0, nullptr, 0, &needed), MINIPSD_OK);
    EXPECT_EQ(needed, 0u);

    // Alpha identifiers: none, with no array, is an empty list.
    needed = 99;
    EXPECT_EQ(minipsd_resource_make_alpha_identifiers(nullptr, 0, nullptr, 0, &needed), MINIPSD_OK);
    EXPECT_EQ(needed, 0u);
}

TEST(MinipsdEdgeTest, BlockSettingsTakeAnEmptyRunInEitherFormat)
{
    minipsd_block_settings set{};
    std::size_t            needed = 0;
    const minipsd_bytes    empty{nullptr, 0}, noData{nullptr, 4};

    // An empty run with no data: a fresh layer, in a PSD and in a PSB.
    EXPECT_EQ(minipsd_layer_make_block_settings(&empty, MINIPSD_PSD, &set, nullptr, 0, &needed), MINIPSD_OK);
    EXPECT_EQ(minipsd_layer_make_block_settings(&empty, MINIPSD_PSB, &set, nullptr, 0, &needed), MINIPSD_OK);
    EXPECT_EQ(minipsd_layer_make_block_settings(nullptr, MINIPSD_PSB, &set, nullptr, 0, &needed), MINIPSD_OK);

    minipsd_block_settings back{};
    EXPECT_EQ(minipsd_blocks_settings(&empty, MINIPSD_PSB, &back), MINIPSD_OK);
    EXPECT_EQ(back.present, 0u) << "nothing in an empty run";
    EXPECT_EQ(minipsd_blocks_settings(&empty, 0, &back), MINIPSD_OK) << "0: the PSD layout";
    EXPECT_EQ(minipsd_blocks_settings(&empty, MINIPSD_PSD, nullptr), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_blocks_settings(&noData, MINIPSD_PSD, &back), MINIPSD_ERR_ARG) << "a size without data";
}

// ---------------------------------------------------------------------------------------------------
// the resource views (views.h)
// ---------------------------------------------------------------------------------------------------

TEST(MinipsdEdgeTest, ResourceViewsRefuseEachNullOutput)
{
    const Bytes            resolution(16, 0);
    const minipsd_resource ri = resourceOf(MINIPSD_RES_RESOLUTION_INFO, resolution);
    EXPECT_EQ(minipsd_resource_resolution_info(&ri, nullptr), MINIPSD_ERR_ARG);

    const Bytes            header(16, 0);   // a grid, no guides
    const minipsd_resource g = resourceOf(MINIPSD_RES_GRID_AND_GUIDES, header);
    std::size_t            pos = 0;
    minipsd_guide          guide{};
    EXPECT_EQ(minipsd_resource_grid_and_guides(&g, nullptr), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_resource_next_guide(&g, &pos, nullptr), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_resource_next_guide(nullptr, &pos, &guide), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_resource_next_guide(&g, &pos, &guide), MINIPSD_OK) << "no guides: the end at once";

    const Bytes            ids(8, 0);
    const minipsd_resource ai = resourceOf(MINIPSD_RES_ALPHA_IDENTIFIERS, ids);
    std::uint32_t          id = 0;
    EXPECT_EQ(minipsd_resource_next_alpha_identifier(&ai, nullptr, &id), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_resource_next_alpha_identifier(nullptr, &pos, &id), MINIPSD_ERR_ARG);

    const Bytes            pascal{1, 'A'};
    const minipsd_resource an = resourceOf(MINIPSD_RES_ALPHA_NAMES, pascal);
    EXPECT_EQ(minipsd_resource_next_alpha_name(&an, &pos, nullptr), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_resource_next_alpha_name(&an, nullptr, nullptr), MINIPSD_ERR_ARG);
    const minipsd_resource noData{MINIPSD_RES_UNICODE_ALPHA_NAMES, {nullptr, 4}, {nullptr, 0}};
    minipsd_bytes          name{};
    pos = 0;
    EXPECT_EQ(minipsd_resource_next_alpha_name(&noData, &pos, &name), MINIPSD_ERR_ARG) << "a size without data";
}

TEST(MinipsdEdgeTest, ResourceViewsRefuseOverflowingCountsAndShortHeaders)
{
    minipsd_grid_and_guides grid{};
    minipsd_guide           guide{};
    std::size_t             pos = 0;

    // A header one byte short.
    const Bytes            shortHead(15, 0);
    const minipsd_resource s = resourceOf(MINIPSD_RES_GRID_AND_GUIDES, shortHead);
    EXPECT_EQ(minipsd_resource_grid_and_guides(&s, &grid), MINIPSD_ERR_FORMAT);
    EXPECT_EQ(minipsd_resource_next_guide(&s, &pos, &guide), MINIPSD_ERR_FORMAT);

    // 0xFFFFFFFF guides of five bytes: the product does not fit 32 bits; refused, not wrapped.
    const Bytes            huge{0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    const minipsd_resource h = resourceOf(MINIPSD_RES_GRID_AND_GUIDES, huge);
    grid.version = 77;
    EXPECT_EQ(minipsd_resource_grid_and_guides(&h, &grid), MINIPSD_ERR_FORMAT);
    EXPECT_EQ(grid.version, 77u) << "untouched on failure";

    // 0xFFFFFFFF UTF-16 units: twice that overflows 32 bits; refused, not wrapped.
    const Bytes            units{0xFF, 0xFF, 0xFF, 0xFF, 0, 'A'};
    const minipsd_resource u = resourceOf(MINIPSD_RES_UNICODE_ALPHA_NAMES, units);
    minipsd_bytes          name{};
    pos = 0;
    EXPECT_EQ(minipsd_resource_next_alpha_name(&u, &pos, &name), MINIPSD_ERR_FORMAT);
}

TEST(MinipsdEdgeTest, CursorsOutsideAListAreRefusedOrEndIt)
{
    // A guide: the header, then one guide at x = 1.
    const Bytes            one{0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 32, 0};
    const minipsd_resource g = resourceOf(MINIPSD_RES_GRID_AND_GUIDES, one);
    minipsd_guide          guide{};
    std::size_t            pos = 4;   // inside the header: not where a guide starts
    EXPECT_EQ(minipsd_resource_next_guide(&g, &pos, &guide), MINIPSD_ERR_ARG);
    pos = 16 + 5 * 200;               // aligned, past the last guide: the end
    EXPECT_EQ(minipsd_resource_next_guide(&g, &pos, &guide), MINIPSD_OK);

    const Bytes            ids(8, 0);
    const minipsd_resource ai = resourceOf(MINIPSD_RES_ALPHA_IDENTIFIERS, ids);
    std::uint32_t          id = 0;
    pos = 2;                          // inside an identifier
    EXPECT_EQ(minipsd_resource_next_alpha_identifier(&ai, &pos, &id), MINIPSD_ERR_ARG);
    pos = 8;                          // at the end
    EXPECT_EQ(minipsd_resource_next_alpha_identifier(&ai, &pos, &id), MINIPSD_OK);
    pos = 400;                        // past the end
    EXPECT_EQ(minipsd_resource_next_alpha_identifier(&ai, &pos, &id), MINIPSD_OK);

    const Bytes            pascal{1, 'A'};
    const minipsd_resource an = resourceOf(MINIPSD_RES_ALPHA_NAMES, pascal);
    minipsd_bytes          name{};
    pos = 400;
    EXPECT_EQ(minipsd_resource_next_alpha_name(&an, &pos, &name), MINIPSD_OK);
}

// ---------------------------------------------------------------------------------------------------
// the reader (reader.h)
// ---------------------------------------------------------------------------------------------------

TEST(MinipsdEdgeTest, DecodeMaskRefusesANullLayerAndAReaderNotReady)
{
    minipsd_reader r;
    std::memset(&r, 0, sizeof r);    // never initialised: not ready
    minipsd_layer_view layer{};
    layer.has_mask = 1;
    std::uint8_t  sample = 0;
    minipsd_plane plane{&sample, 1, 1};

    EXPECT_EQ(minipsd_decode_mask(&r, nullptr, &plane, nullptr), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_decode_mask(&r, &layer, nullptr, nullptr), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_decode_mask(&r, &layer, &plane, nullptr), MINIPSD_ERR_ARG) << "not ready";
}

TEST(MinipsdEdgeTest, ChannelIdsMapToSlotsAtTheLimits)
{
    minipsd_reader r;
    std::memset(&r, 0, sizeof r);

    r.colors = 3;   // RGB: three colours, then transparency
    EXPECT_EQ(mpsd__planes(&r), 4);
    EXPECT_EQ(mpsd__slot(&r, 0), 0);
    EXPECT_EQ(mpsd__slot(&r, 2), 2);
    EXPECT_EQ(mpsd__slot(&r, 3), -1) << "a spot channel: within the planes, past the colours";
    EXPECT_EQ(mpsd__slot(&r, 9), -1);
    EXPECT_EQ(mpsd__slot(&r, -1), 3) << "transparency after the colours";
    EXPECT_EQ(mpsd__slot(&r, -2), MPSD__SLOT_MASK);
    EXPECT_EQ(mpsd__slot(&r, -3), MPSD__SLOT_REAL);
    EXPECT_EQ(mpsd__slot(&r, -4), -1) << "no such id";
    EXPECT_EQ(mpsd__slot(&r, std::numeric_limits<int>::min()), -1);

    r.colors = MINIPSD_PLANES;   // as many colours as planes: no room for transparency
    EXPECT_EQ(mpsd__planes(&r), MINIPSD_PLANES);
    EXPECT_EQ(mpsd__slot(&r, -1), -1);
    EXPECT_EQ(mpsd__slot(&r, MINIPSD_PLANES - 1), MINIPSD_PLANES - 1);
    EXPECT_EQ(mpsd__slot(&r, MINIPSD_PLANES), -1);
}

// ---------------------------------------------------------------------------------------------------
// the internal helpers at their limits
// ---------------------------------------------------------------------------------------------------

TEST(MinipsdEdgeTest, FloatHelpersAtInfinityNanAndTheExtremes)
{
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();

    EXPECT_TRUE(mpsd__floats_equal(1.0, 1.0 + 1e-12));
    EXPECT_FALSE(mpsd__floats_equal(1.0, 2.0)) << "b above a";
    EXPECT_FALSE(mpsd__floats_equal(2.0, 1.0)) << "a above b";
    EXPECT_FALSE(mpsd__floats_equal(nan, nan));
    EXPECT_FALSE(mpsd__floats_equal(inf, inf)) << "inf - inf is NaN";

    EXPECT_TRUE(mpsd__floats_positive(DBL_MIN));
    EXPECT_TRUE(mpsd__floats_positive(DBL_MAX));
    EXPECT_FALSE(mpsd__floats_positive(0.0));
    EXPECT_FALSE(mpsd__floats_positive(-0.0));
    EXPECT_FALSE(mpsd__floats_positive(-1.0));
    EXPECT_FALSE(mpsd__floats_positive(inf)) << "past DBL_MAX";
    EXPECT_FALSE(mpsd__floats_positive(-inf));
    EXPECT_FALSE(mpsd__floats_positive(nan));
}

TEST(MinipsdEdgeTest, ZipScratchGrowsOnlyForZipChannelsWithData)
{
    mpsd__arena a{};
    const std::uint8_t zip[2] = {0, MPSD__COMP_ZIP}, rle[2] = {0, 1};

    mpsd__zip_room(&a, minipsd_bytes{nullptr, 2}, 100);
    EXPECT_EQ(a.zip, 0u) << "a size without data";
    mpsd__zip_room(&a, minipsd_bytes{zip, 1}, 100);
    EXPECT_EQ(a.zip, 0u) << "shorter than the marker";
    mpsd__zip_room(&a, minipsd_bytes{rle, 2}, 100);
    EXPECT_EQ(a.zip, 0u) << "not ZIP";
    mpsd__zip_room(&a, minipsd_bytes{zip, 2}, 100);
    EXPECT_EQ(a.zip, 100u);
    mpsd__zip_room(&a, minipsd_bytes{zip, 2}, 50);
    EXPECT_EQ(a.zip, 100u) << "the largest kept";
    mpsd__zip_room(&a, minipsd_bytes{zip, 2}, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(a.zip, std::numeric_limits<std::uint64_t>::max());
}

}  // namespace
