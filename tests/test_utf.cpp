/**
 * \file
 * \brief Tests for minipsd/detail/utf.h and minipsd_utf16be_to_utf8: UTF-8 to UTF-16 and back,
 *        malformed input replaced.
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

    // The public converter gives the same code points as UTF-8, measured first; a cap one short is
    // MINIPSD_MORE with the text empty.
    std::string utf8;
    for (std::uint32_t cp : expected) {
        std::uint8_t out[4];
        utf8.append(out, out + mpsd__put_utf8(out, cp));
    }
    const minipsd_bytes text{bytes, sizeof bytes};
    std::size_t needed = 0;
    EXPECT_EQ(minipsd_utf16be_to_utf8(&text, nullptr, 0, &needed), MINIPSD_MORE);
    ASSERT_EQ(needed, utf8.size() + 1);
    std::string got(needed, 'x');
    EXPECT_EQ(minipsd_utf16be_to_utf8(&text, got.data(), needed - 1, nullptr), MINIPSD_MORE);
    EXPECT_EQ(got[0], '\0');
    EXPECT_EQ(minipsd_utf16be_to_utf8(&text, got.data(), needed, nullptr), MINIPSD_OK);
    EXPECT_EQ(got, utf8 + '\0');

    // And back: the converse gives the units, measured first, a pair for the turtle.
    const std::uint8_t units[] = {0x00, 0x41, 0x20, 0xAC, 0xD8, 0x3D, 0xDC, 0x22};
    std::uint8_t made[sizeof units];
    EXPECT_EQ(minipsd_utf8_to_utf16be("A\xE2\x82\xAC\xF0\x9F\x90\xA2", nullptr, 0, &needed), MINIPSD_MORE);
    ASSERT_EQ(needed, sizeof units);
    EXPECT_EQ(minipsd_utf8_to_utf16be("A\xE2\x82\xAC\xF0\x9F\x90\xA2", made, sizeof made, nullptr), MINIPSD_OK);
    EXPECT_EQ(Bytes(made, made + sizeof made), Bytes(units, units + sizeof units));
    EXPECT_EQ(minipsd_utf8_to_utf16be(nullptr, made, sizeof made, &needed), MINIPSD_OK);
    EXPECT_EQ(needed, 0u) << "NULL is empty";
    EXPECT_EQ(minipsd_utf8_to_utf16be("A", nullptr, 1, nullptr), MINIPSD_ERR_ARG);

    // A U+0000 ends the text; an odd last byte is dropped; empty is empty.
    const std::uint8_t ended[] = {0x00, 0x41, 0x00, 0x00, 0x00, 0x42}, odd[] = {0x00, 0x41, 0x00};
    char small[4];
    const minipsd_bytes endedText{ended, sizeof ended}, oddText{odd, sizeof odd}, empty{nullptr, 0};
    EXPECT_EQ(minipsd_utf16be_to_utf8(&endedText, small, sizeof small, nullptr), MINIPSD_OK);
    EXPECT_STREQ(small, "A");
    EXPECT_EQ(minipsd_utf16be_to_utf8(&oddText, small, sizeof small, nullptr), MINIPSD_OK);
    EXPECT_STREQ(small, "A");
    EXPECT_EQ(minipsd_utf16be_to_utf8(&empty, small, sizeof small, &needed), MINIPSD_OK);
    EXPECT_EQ(needed, 1u);

    const minipsd_bytes noData{nullptr, 2};
    EXPECT_EQ(minipsd_utf16be_to_utf8(nullptr, small, sizeof small, nullptr), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_utf16be_to_utf8(&noData, small, sizeof small, nullptr), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_utf16be_to_utf8(&text, nullptr, 1, nullptr), MINIPSD_ERR_ARG);
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
