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
