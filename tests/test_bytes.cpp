/**
 * \file
 * \brief Tests for minipsd/detail/bytes.h: the output sink and big-endian integers.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_STATIC
#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include "support.h"

TEST(MinipsdBytesTest, IntegersAreBigEndian)
{
    Bytes out;
    mpsd__sink s = {collect, &out, 0, 0};
    mpsd__u8(&s, 0xAB);
    mpsd__u16(&s, 0x1234);
    mpsd__u32(&s, 0x89ABCDEF);
    mpsd__zeros(&s, 2);
    mpsd__put(&s, "xy", 2);
    EXPECT_EQ(out, (Bytes{0xAB, 0x12, 0x34, 0x89, 0xAB, 0xCD, 0xEF, 0, 0, 'x', 'y'}));
    EXPECT_EQ(s.count, out.size());
    EXPECT_EQ(s.failed, 0);
}

TEST(MinipsdBytesTest, ACountingSinkOnlyCounts)
{
    mpsd__sink s = {nullptr, nullptr, 0, 0};   // the sizing pass
    mpsd__u32(&s, 1);
    mpsd__zeros(&s, 3);
    EXPECT_EQ(s.count, 7u);
}

TEST(MinipsdBytesTest, AFailedSinkStopsCallingButKeepsCounting)
{
    int calls = 0;
    const auto refuse = [](void *user, const void *, size_t) -> int { ++*static_cast<int *>(user); return 0; };
    mpsd__sink s = {refuse, &calls, 0, 0};
    mpsd__u16(&s, 1);
    mpsd__u16(&s, 2);
    EXPECT_EQ(calls, 1) << "no call after the first failure";
    EXPECT_EQ(s.failed, 1);
    EXPECT_EQ(s.count, 4u) << "sizes stay right, so the caller can still report how far it got";
}

TEST(MinipsdBytesTest, PadRoundsUpToAMultiple)
{
    EXPECT_EQ(mpsd__pad(0, 4), 0u);
    EXPECT_EQ(mpsd__pad(1, 4), 4u);
    EXPECT_EQ(mpsd__pad(4, 4), 4u);
    EXPECT_EQ(mpsd__pad(5, 2), 6u);
}

TEST(MinipsdBytesTest, IntegersAreReadBackBigEndian)
{
    const std::uint8_t b[] = {0x12, 0x34, 0x89, 0xAB, 0xCD, 0xEF};
    EXPECT_EQ(mpsd__get16(b), 0x1234u);
    EXPECT_EQ(mpsd__get32(b + 2), 0x89ABCDEFu);
    const std::uint8_t high[] = {0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_EQ(mpsd__get32(high), 0xFFFFFFFFu) << "no sign extension";
    EXPECT_EQ(mpsd__get16(high), 0xFFFFu);

    Bytes out;
    mpsd__sink s = {collect, &out, 0, 0};
    mpsd__u32(&s, 0xDEADBEEF);
    mpsd__u16(&s, 0xCAFE);
    EXPECT_EQ(mpsd__get32(out.data()), 0xDEADBEEFu) << "what the writer puts, the reader gets";
    EXPECT_EQ(mpsd__get16(out.data() + 4), 0xCAFEu);
}
