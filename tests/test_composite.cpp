/**
 * \file
 * \brief Tests for minipsd/detail/composite.h: blending "over" with opacity, and the white matte.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_STATIC
#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include "support.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {

// The whole composite image, row by row, as the writer stores it.
Bytes composite(std::uint32_t w, std::uint32_t h, const minipsd_layer *layers, size_t n)
{
    const minipsd_doc d = docOf(w, h, layers, n);
    Bytes buffer(std::size_t(w) * 4);
    mpsd__composite c;
    mpsd__composite_init(&c, &d, buffer.data());
    Bytes out;
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t *row = mpsd__composite_row(&c, y);
        EXPECT_EQ(row, buffer.data()) << "the row is built in the caller's buffer";
        out.insert(out.end(), row, row + std::size_t(w) * 4);
    }
    return out;
}

} // namespace

TEST(MinipsdCompositeTest, OneOpaqueLayerIsItselfOverWhite)
{
    const Bytes px = testImage(37, 5);
    const minipsd_layer layer{"L", px.data(), 255, 0};
    EXPECT_EQ(composite(37, 5, &layer, 1), matted(px)) << "every row from its own line";
}

TEST(MinipsdCompositeTest, BlendsLayersOverWithOpacity)
{
    // Opaque red below; blue at alpha 255 with layer opacity 128 above: about half and half.
    const Bytes red  = {255, 0, 0, 255};
    const Bytes blue = {0, 0, 255, 255};
    const minipsd_layer layers[2] = {{"Red", red.data(), 255, 0}, {"Blue", blue.data(), 128, 0}};
    const Bytes c = composite(1, 1, layers, 2);
    EXPECT_NEAR(c[0], 127, 1);
    EXPECT_EQ(c[1], 0);
    EXPECT_NEAR(c[2], 128, 1);
    EXPECT_EQ(c[3], 255);
}

TEST(MinipsdCompositeTest, TwoTransparentLayers)
{
    // Half-transparent white over nothing, then half-transparent black: alpha 0.75, grey 1/3 white,
    // which the file stores over white.
    const Bytes white = {255, 255, 255, 128};
    const Bytes black = {0, 0, 0, 128};
    const minipsd_layer layers[2] = {{"W", white.data(), 255, 0}, {"B", black.data(), 255, 0}};
    const Bytes c = composite(1, 1, layers, 2);
    EXPECT_NEAR(c[3], 192, 1);
    EXPECT_NEAR(c[0], matted({85, 85, 85, 192})[0], 1);
}

TEST(MinipsdCompositeTest, IsMattedAgainstWhite)
{
    // The template's margin pink at half alpha, worked by hand: c * 128/255 + 255 * 127/255.
    // psd-tools and Photoshop undo this; stored unmatted, the pink read back as (255, 0, 105).
    const Bytes pink = {255, 105, 180, 128};
    const minipsd_layer layer{"Template", pink.data(), 255, 0};
    EXPECT_EQ(composite(1, 1, &layer, 1), (Bytes{255, 180, 217, 128}));

    const Bytes clear = {10, 20, 30, 0};
    const minipsd_layer empty{"Clear", clear.data(), 255, 0};
    EXPECT_EQ(composite(1, 1, &empty, 1), (Bytes{255, 255, 255, 0})) << "a fully transparent pixel is plain white";
}

TEST(MinipsdCompositeTest, HiddenZeroOpacityAndEmptyLayersAreLeftOut)
{
    const Bytes red  = {255, 0, 0, 255};
    const Bytes blue = {0, 0, 255, 255};
    const minipsd_layer layers[4] = {{"Red", red.data(), 255, 0}, {"Hidden", blue.data(), 255, 1},
                                     {"Clear", blue.data(), 0, 0}, {"Empty", nullptr, 255, 0}};
    EXPECT_EQ(composite(1, 1, layers, 4), red);
}

TEST(MinipsdCompositeTest, ALayerBlendsAtItsRectangleClippedToTheCanvas)
{
    // A 2 x 2 layer at x 3, y -1 on a 4 x 3 canvas: only its bottom-left pixel lands, at (3, 0).
    const Bytes px = {1, 2, 3, 255,  4, 5, 6, 255,
                      7, 8, 9, 255,  10, 11, 12, 255};
    const minipsd_layer edge{.name = "Edge", .rgba = px.data(), .opacity = 255, .top = -1, .left = 3, .bottom = 1, .right = 5};
    Bytes want(std::size_t(4) * 3 * 4, 0);
    std::copy(px.begin() + 8, px.begin() + 12, want.begin() + std::ptrdiff_t(3) * 4);
    EXPECT_EQ(composite(4, 3, &edge, 1), matted(want));

    const minipsd_layer outside{.name = "Out", .rgba = px.data(), .opacity = 255, .top = 3, .left = -2, .bottom = 5, .right = 0};
    EXPECT_EQ(composite(4, 3, &outside, 1), matted(Bytes(std::size_t(4) * 3 * 4, 0))) << "wholly off the canvas: nothing";
}

TEST(MinipsdCompositeTest, UnmattingUndoesTheMatteWithinItsRounding)
{
    for (unsigned a = 0; a <= 255; ++a)
        for (unsigned c = 0; c <= 255; ++c) {
            const unsigned s = (c * a + 255u * (255u - a) + 127u) / 255u;   // the writer's matte
            const unsigned back = mpsd__unmatte(s, a, 255);
            if (a == 0) { ASSERT_EQ(back, 0u) << "no colour where nothing is"; continue; }
            const int tolerance = a == 255 ? 0 : 1 + 255 / (2 * static_cast<int>(a));
            ASSERT_LE(std::abs(static_cast<int>(back) - static_cast<int>(c)), tolerance) << "c " << c << ", a " << a;
        }
    EXPECT_EQ(mpsd__unmatte(0, 1, 255), 0u) << "clamped, not wrapped";
    EXPECT_EQ(mpsd__unmatte(255, 1, 255), 255u);

    // 16 bits, sampled: the same bound, max / (2a) levels.
    for (std::uint32_t a = 0; a <= 65535; a += a < 512 ? 1 : 257)
        for (std::uint32_t c = 0; c <= 65535; c += 4369) {
            const std::uint32_t s = static_cast<std::uint32_t>((std::uint64_t(c) * a + 65535ull * (65535 - a) + 32767) / 65535);
            const std::uint32_t back = mpsd__unmatte(s, a, 65535);
            if (a == 0) { ASSERT_EQ(back, 0u); continue; }
            const std::int64_t tolerance = a == 65535 ? 0 : 1 + 65535 / (2 * std::int64_t(a));
            ASSERT_LE(std::llabs(std::int64_t(back) - std::int64_t(c)), tolerance) << "c " << c << ", a " << a;
        }
    EXPECT_EQ(mpsd__unmatte(0, 1, 65535), 0u) << "clamped, not wrapped";
}

// 16 bits: the same "over" and the same matte, against 65535.
TEST(MinipsdCompositeTest, SixteenBitLayersComposeAndMatte)
{
    const std::uint16_t red[4] = {65535, 0, 0, 65535}, blue[4] = {0, 0, 65535, 32768};
    const minipsd_layer layers[2] = {{.name = "Red", .rgba = reinterpret_cast<const std::uint8_t *>(red), .opacity = 255},
                                     {.name = "Blue", .rgba = reinterpret_cast<const std::uint8_t *>(blue), .opacity = 255}};
    minipsd_doc d = docOf(1, 1, layers, 2);
    d.depth = 16;
    std::uint16_t buffer[4];
    mpsd__composite c;
    mpsd__composite_init(&c, &d, reinterpret_cast<std::uint8_t *>(buffer));
    std::uint16_t got[4];
    std::memcpy(got, mpsd__composite_row(&c, 0), sizeof got);
    EXPECT_NEAR(got[0], 32767, 1) << "half blue over opaque red";
    EXPECT_EQ(got[1], 0);
    EXPECT_NEAR(got[2], 32768, 1);
    EXPECT_EQ(got[3], 65535);

    const std::uint16_t half[4] = {0, 65535, 0, 32768};   // over white: 65535 * (1 - a) + c * a
    const minipsd_layer one{.name = "Half", .rgba = reinterpret_cast<const std::uint8_t *>(half), .opacity = 255};
    d = docOf(1, 1, &one, 1);
    d.depth = 16;
    std::memcpy(got, mpsd__composite_row(&c, 0), sizeof got);
    EXPECT_EQ(got[0], 32767);
    EXPECT_EQ(got[1], 65535);
    EXPECT_EQ(got[3], 32768);
}
