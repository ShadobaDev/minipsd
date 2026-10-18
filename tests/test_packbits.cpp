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

#include <vector>

namespace {

// n bytes from src, every stride-th one: one channel out of interleaved RGBA.  Wide: n bytes of
// two-byte samples in host order, packed big-endian.
Bytes pack(const std::uint8_t *src, std::size_t n, std::size_t stride, unsigned wide = 0)
{
    Bytes out;
    mpsd__sink s = {collect, &out, 0, 0};
    mpsd__packbits(&s, src, n, stride, wide);
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

TEST(MinipsdPackbitsTest, TheDecoderUndoesTheEncoderIntoAStridedTarget)
{
    const std::size_t w = 300;
    Bytes px(w * 4);
    for (std::size_t x = 0; x < w; ++x) {
        px[x * 4 + 0] = 7;
        px[x * 4 + 1] = static_cast<std::uint8_t>(x);
        px[x * 4 + 2] = static_cast<std::uint8_t>(x / 2);
        px[x * 4 + 3] = static_cast<std::uint8_t>(x < 200 ? 255 : x);
    }
    Bytes back(w * 4, 0xEE);
    for (int k = 0; k < 4; ++k) {   // each channel back into its place in interleaved RGBA
        const Bytes packed = pack(px.data() + k, w, 4);
        ASSERT_EQ(mpsd__unpackbits(packed.data(), packed.size(), back.data() + k, 4, static_cast<std::uint32_t>(w), 0), 1) << k;
    }
    EXPECT_EQ(back, px);

    const Bytes noop = {0x80, 0x00, 9};   // 128 is no packet at all
    std::uint8_t one = 0;
    EXPECT_EQ(mpsd__unpackbits(noop.data(), noop.size(), &one, 1, 1, 0), 1);
    EXPECT_EQ(one, 9);
    EXPECT_EQ(mpsd__unpackbits(noop.data(), 2, &one, 1, 1, 0), 0) << "the row ends inside a packet";
}

// 16 bits: the row is packed as the file stores it, big-endian, from samples in host order, and
// decoded back into host order.  Equal high bytes make runs across samples.
TEST(MinipsdPackbitsTest, SixteenBitSamplesGoOutBigEndian)
{
    const std::size_t w = 200;
    std::vector<std::uint16_t> px(w * 4);
    for (std::size_t x = 0; x < w; ++x) {
        px[x * 4 + 0] = 0x1234;                                     // a run of 0x12 0x34 pairs: literals
        px[x * 4 + 1] = static_cast<std::uint16_t>(x * 300);
        px[x * 4 + 2] = 0;                                          // one long repeat
        px[x * 4 + 3] = 65535;
    }
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(px.data());
    std::vector<std::uint16_t> back(w * 4, 0xEEEE);
    auto *out = reinterpret_cast<std::uint8_t *>(back.data());
    for (int k = 0; k < 4; ++k) {
        const Bytes packed = pack(bytes + 2 * k, 2 * w, 8, 1);
        Bytes big;   // what the file holds, unpacked: each sample high byte first
        for (std::size_t x = 0; x < w; ++x) { big.push_back(px[x * 4 + k] >> 8); big.push_back(px[x * 4 + k] & 0xFF); }
        EXPECT_EQ(unpackAll(packed, 2 * w), big) << "channel " << k;
        ASSERT_EQ(mpsd__unpackbits(packed.data(), packed.size(), out + 2 * k, 8, static_cast<std::uint32_t>(2 * w), 1), 1) << k;
    }
    EXPECT_EQ(back, px);
    EXPECT_EQ(pack(bytes + 4, 2 * w, 8, 1).size(), 8u) << "400 zero bytes: four repeats";
}
