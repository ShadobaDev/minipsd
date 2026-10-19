/**
 * \file
 * \brief Tests for minipsd/detail/format.h: lengths, row counts and blocks as PSD and PSB lay them
 *        out.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_STATIC
#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include "support.h"

#include <string>
#include <tuple>

TEST(MinipsdFormatTest, LengthsAndCountsWidenInAPsb)
{
    Bytes out;
    mpsd__sink s = {collect, &out, 0, 0};
    mpsd__putlen(&s, 0x0102030405ull, 0);       // a PSD keeps the low four bytes
    mpsd__putlen(&s, 0x0102030405ull, 1);
    mpsd__putcount(&s, 0x0607, 0);
    mpsd__putcount(&s, 0x08090A, 1);
    EXPECT_EQ(out, (Bytes{2, 3, 4, 5, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 0, 8, 9, 10}));
    EXPECT_EQ(mpsd__getlen(out.data(), 0), 0x02030405u);
    EXPECT_EQ(mpsd__getlen(out.data() + 4, 1), 0x0102030405ull);
    EXPECT_EQ(mpsd__count(out.data() + 12, 0, 0), 0x0607u);
    EXPECT_EQ(mpsd__count(out.data() + 10, 1, 1), 0x08090Au) << "row 1 of a table of four-byte counts";
}

TEST(MinipsdFormatTest, TheKeyDecidesALengthsWidthNotTheSignature)
{
    for (const char *key : {"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn", "Alph", "FMsk", "lnk2", "lnk3",
                            "lnkE", "FEid", "FXid", "FELS", "PxSD", "pths", "extd", "extn", "cinf", "artd"})
        EXPECT_TRUE(mpsd__long_key(reinterpret_cast<const std::uint8_t *>(key))) << key;
    for (const char *key : {"luni", "lsct", "Patt", "lnk1", "artb"})
        EXPECT_FALSE(mpsd__long_key(reinterpret_cast<const std::uint8_t *>(key))) << key;

    const Bytes wide = {'8', 'B', '6', '4', 'L', 'r', '1', '6', 0, 0, 0, 0, 0, 0, 0, 1, 9};
    const Bytes narrow = {'8', 'B', 'I', 'M', 'l', 'u', 'n', 'i', 0, 0, 0, 1, 9};
    minipsd_block block;
    std::size_t   next = 0;
    ASSERT_EQ(mpsd__block({wide.data(), wide.size()}, 0, 1, &block, &next), MINIPSD_OK);
    EXPECT_EQ(std::make_tuple(std::string(block.key), block.data.size, next), std::make_tuple(std::string("Lr16"), std::size_t(1), std::size_t(17)));
    ASSERT_EQ(mpsd__block({wide.data(), wide.size()}, 0, 0, &block, &next), MINIPSD_OK);
    EXPECT_EQ(next, 12u) << "read as a PSD's: four bytes of length, 0";
    EXPECT_EQ(mpsd__block({wide.data(), wide.size()}, next, 0, &block, &next), MINIPSD_ERR_FORMAT) << "and no block follows";
    for (unsigned psb : {0u, 1u}) {
        ASSERT_EQ(mpsd__block({narrow.data(), narrow.size()}, 0, psb, &block, &next), MINIPSD_OK) << psb;
        EXPECT_EQ(next, narrow.size());
    }
    EXPECT_EQ(mpsd__block({wide.data(), 14}, 0, 1, &block, &next), MINIPSD_ERR_FORMAT) << "a long key's header cut short";
}
