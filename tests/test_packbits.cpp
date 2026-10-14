/**
 * \file
 * \brief Tests for minipsd/detail/packbits.h: packet sizes, and decoding back to the input.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_STATIC
#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include "support.h"

namespace {

// n bytes from src, every stride-th one: one channel out of interleaved RGBA.
Bytes pack(const std::uint8_t *src, std::size_t n, std::size_t stride)
{
    Bytes out;
    mpsd__sink s = {collect, &out, 0, 0};
    mpsd__packbits(&s, src, n, stride);
    return out;
}

Bytes unpackAll(const Bytes &packed, std::size_t n)
{
    Reader r(packed);
    return unpack(r, packed.size(), n);
}

} // namespace

// PackBits splits at 128 both ways; the sizes prove it compresses rather than merely round-trips.
TEST(MinipsdPackbitsTest, SplitsLongRepeatsAndLiterals)
{
    const std::size_t w = 300;
    Bytes px(w * 4);
    for (std::size_t x = 0; x < w; ++x) {
        px[x * 4 + 0] = 7;                                    // one long repeat
        px[x * 4 + 1] = static_cast<std::uint8_t>(x);         // all literals
        px[x * 4 + 2] = static_cast<std::uint8_t>(x / 2);     // pairs: not worth a repeat packet
        px[x * 4 + 3] = 255;
    }
    const std::size_t expected[4] = {6, 303, 303, 6};   // 300 = 128 + 128 + 44: three packets each
    for (int k = 0; k < 4; ++k) {
        const Bytes packed = pack(px.data() + k, w, 4);
        EXPECT_EQ(packed.size(), expected[k]) << "channel " << k;
        const Bytes back = unpackAll(packed, w);
        for (std::size_t x = 0; x < w; ++x) ASSERT_EQ(back[x], px[x * 4 + k]) << "channel " << k << ", x " << x;
    }
}

TEST(MinipsdPackbitsTest, ShortRowsEncode)
{
    for (std::size_t w : {1u, 2u, 3u, 129u}) {
        Bytes row(w, 255);
        row[0] = 1;
        EXPECT_EQ(unpackAll(pack(row.data(), w, 1), w), row) << "width " << w;
    }
}
