/**
 * \file
 * \brief Tests for minipsd/detail/utf.h: UTF-8 in, UTF-16 out, malformed input replaced.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_STATIC
#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include "support.h"

#include <string>

namespace {

std::u16string utf16(const char *utf8)
{
    std::u16string out;
    const auto unit = [](void *ctx, unsigned u) { static_cast<std::u16string *>(ctx)->push_back(static_cast<char16_t>(u)); };
    const size_t n = mpsd__utf16(utf8, unit, &out);
    EXPECT_EQ(n, out.size()) << "the count returned is the count delivered";
    EXPECT_EQ(mpsd__utf16(utf8, nullptr, nullptr), n) << "counting alone gives the same";
    return out;
}

} // namespace

TEST(MinipsdUtfTest, OneToFourByteSequences)
{
    // "Szablon zolw" with Polish letters (2 bytes), a euro sign (3), then a turtle (4: a surrogate pair).
    EXPECT_EQ(utf16("Szablon \xC5\xBC\xC3\xB3\xC5\x82w \xE2\x82\xAC \xF0\x9F\x90\xA2"),
              u"Szablon \u017C\u00F3\u0142w \u20AC \U0001F422");
}

TEST(MinipsdUtfTest, MalformedInputBecomesTheReplacementCharacter)
{
    // A stray continuation byte, a code point above U+10FFFF, a cut sequence.
    EXPECT_EQ(utf16("a\x80" "b\xF4\x90\x80\x80" "c\xC5"), u"a\uFFFDb\uFFFDc\uFFFD");
}

TEST(MinipsdUtfTest, NullIsEmpty)
{
    EXPECT_EQ(utf16(nullptr), u"");
}

TEST(MinipsdUtfTest, Utf16BeDecodesPairsAndReplacesUnpairedSurrogates)
{
    // A, a euro sign, a turtle (a pair), then every kind of unpaired surrogate.
    const std::uint8_t bytes[] = {0x00, 0x41, 0x20, 0xAC, 0xD8, 0x3D, 0xDC, 0x22,
                                  0xD8, 0x3D, 0x00, 0x41,    // a high surrogate, then an ordinary unit
                                  0xDC, 0x22,                // a low surrogate alone
                                  0xD8, 0x3D, 0xE0, 0x00,    // a high surrogate, then a unit above the surrogates
                                  0xD8, 0x3D};               // a high surrogate at the end
    const std::uint8_t *p = bytes, *end = bytes + sizeof bytes;
    const std::uint32_t expected[] = {0x41, 0x20AC, 0x1F422, 0xFFFD, 0x41, 0xFFFD, 0xFFFD, 0xE000, 0xFFFD};
    for (std::uint32_t cp : expected) {
        ASSERT_LT(p, end);
        EXPECT_EQ(mpsd__utf16be(&p, end), cp);
    }
    EXPECT_EQ(p, end);
}

TEST(MinipsdUtfTest, Utf8IsEncodedOneToFourBytesOrOnlyCounted)
{
    const struct { std::uint32_t cp; std::string utf8; } cases[] = {
        {0x41, "A"}, {0x7F, "\x7F"}, {0x80, "\xC2\x80"}, {0x7FF, "\xDF\xBF"}, {0x800, "\xE0\xA0\x80"},
        {0xFFFF, "\xEF\xBF\xBF"}, {0x10000, "\xF0\x90\x80\x80"}, {0x10FFFF, "\xF4\x8F\xBF\xBF"}};
    for (const auto &c : cases) {
        std::uint8_t out[4] = {};
        EXPECT_EQ(mpsd__put_utf8(nullptr, c.cp), c.utf8.size()) << std::hex << c.cp;
        EXPECT_EQ(mpsd__put_utf8(out, c.cp), c.utf8.size());
        EXPECT_EQ(std::string(out, out + c.utf8.size()), c.utf8) << std::hex << c.cp;
    }
}
