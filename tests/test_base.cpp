/**
 * \file
 * \brief Tests for minipsd/base.h: the version and the return-code texts.
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_STATIC
#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include <gtest/gtest.h>

#include <string>

TEST(MinipsdBaseTest, VersionStringSpellsTheThreeNumbers)
{
    EXPECT_EQ(std::string(MINIPSD_VERSION_STRING), std::to_string(MINIPSD_VERSION_MAJOR) + "." +
                                                   std::to_string(MINIPSD_VERSION_MINOR) + "." +
                                                   std::to_string(MINIPSD_VERSION_PATCH));
}

TEST(MinipsdBaseTest, OnlyErrorsAreNegative)
{
    EXPECT_EQ(MINIPSD_OK, 0);
    EXPECT_GT(MINIPSD_MORE, 0) << "rc < 0 is the failure test for every function";
    for (int code : {MINIPSD_ERR_ARG, MINIPSD_ERR_SIZE, MINIPSD_ERR_WRITE, MINIPSD_ERR_ALLOC, MINIPSD_ERR_OPEN})
        EXPECT_LT(code, 0);
}

TEST(MinipsdBaseTest, EveryReturnCodeHasItsOwnText)
{
    EXPECT_STREQ(minipsd_error_string(MINIPSD_ERR_SIZE),
                 "too large for PSD: a side outside 1..30000, or a section above 4 GiB");
    EXPECT_STREQ(minipsd_error_string(-100), "unknown error");
    for (int code : {MINIPSD_OK, MINIPSD_MORE, MINIPSD_ERR_ARG, MINIPSD_ERR_WRITE, MINIPSD_ERR_ALLOC, MINIPSD_ERR_OPEN})
        EXPECT_STRNE(minipsd_error_string(code), "unknown error") << "every return code has its own text: " << code;
}
