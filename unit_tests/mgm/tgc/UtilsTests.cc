//------------------------------------------------------------------------------
// File: TgcUtilsTests.cc
// Author: Steven Murray <smurray at cern dot ch>
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2018 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

#include "mgm/tgc/Utils.hh"

#include <gtest/gtest.h>

class TgcUtilsTest : public ::testing::Test {
protected:

  virtual void SetUp() {
  }

  virtual void TearDown() {
  }
};

//------------------------------------------------------------------------------
// Test
//------------------------------------------------------------------------------
TEST_F(TgcUtilsTest, isValidUInt_unsigned_int) {
  using namespace eos::mgm::tgc;

  ASSERT_TRUE(Utils::isValidUInt("12345"));
}

//------------------------------------------------------------------------------
// Test
//------------------------------------------------------------------------------
TEST_F(TgcUtilsTest, isValidUInt_empty_string) {
  using namespace eos::mgm::tgc;

  ASSERT_FALSE(Utils::isValidUInt(""));
}

//------------------------------------------------------------------------------
// Test
//------------------------------------------------------------------------------
TEST_F(TgcUtilsTest, isValidUInt_signed_int) {
  using namespace eos::mgm::tgc;

  ASSERT_FALSE(Utils::isValidUInt("-12345"));
}

//------------------------------------------------------------------------------
// Test
//------------------------------------------------------------------------------
TEST_F(TgcUtilsTest, isValidUInt_not_a_number) {
  using namespace eos::mgm::tgc;

  ASSERT_FALSE(Utils::isValidUInt("one"));
}

TEST_F(TgcUtilsTest, toUint64_unsigned_int) {
  using namespace eos::mgm::tgc;

  ASSERT_EQ((uint64_t)12345, Utils::toUint64("12345"));
  ASSERT_EQ((uint64_t)18446744073709551615ULL, Utils::toUint64("18446744073709551615"));
}

TEST_F(TgcUtilsTest, toUint64_out_of_range) {
  using namespace eos::mgm::tgc;

  ASSERT_THROW(Utils::toUint64("18446744073709551616"), Utils::OutOfRangeUint64);
}

TEST_F(TgcUtilsTest, toUint64_empty_string) {
  using namespace eos::mgm::tgc;

  ASSERT_THROW(Utils::toUint64(""), Utils::InvalidUint64);
}

TEST_F(TgcUtilsTest, toUint64_max) {
  using namespace eos::mgm::tgc;

  ASSERT_EQ((uint64_t)18446744073709551615UL, Utils::toUint64("18446744073709551615"));
}

TEST_F(TgcUtilsTest, toUint64_not_a_number) {
  using namespace eos::mgm::tgc;

  ASSERT_THROW(Utils::toUint64("one"), Utils::InvalidUint64);
}

//------------------------------------------------------------------------------
// Test
//------------------------------------------------------------------------------
TEST_F(TgcUtilsTest, divideAndRoundToNearest) {
  using namespace eos::mgm::tgc;

  ASSERT_EQ(1, Utils::divideAndRoundToNearest( 1, 1));
  ASSERT_EQ(2, Utils::divideAndRoundToNearest( 2, 1));
  ASSERT_EQ(3, Utils::divideAndRoundToNearest( 3, 1));

  ASSERT_EQ(1, Utils::divideAndRoundToNearest( 1, 2));
  ASSERT_EQ(1, Utils::divideAndRoundToNearest( 2, 2));
  ASSERT_EQ(2, Utils::divideAndRoundToNearest( 3, 2));
  ASSERT_EQ(2, Utils::divideAndRoundToNearest( 4, 2));
  ASSERT_EQ(3, Utils::divideAndRoundToNearest( 5, 2));
  ASSERT_EQ(3, Utils::divideAndRoundToNearest( 6, 2));

  ASSERT_EQ(0, Utils::divideAndRoundToNearest( 1, 3));
  ASSERT_EQ(1, Utils::divideAndRoundToNearest( 2, 3));
  ASSERT_EQ(1, Utils::divideAndRoundToNearest( 3, 3));
  ASSERT_EQ(1, Utils::divideAndRoundToNearest( 4, 3));
  ASSERT_EQ(2, Utils::divideAndRoundToNearest( 5, 3));
  ASSERT_EQ(2, Utils::divideAndRoundToNearest( 6, 3));
  ASSERT_EQ(2, Utils::divideAndRoundToNearest( 7, 3));
  ASSERT_EQ(3, Utils::divideAndRoundToNearest( 8, 3));
  ASSERT_EQ(3, Utils::divideAndRoundToNearest( 9, 3));
  ASSERT_EQ(3, Utils::divideAndRoundToNearest(10, 3));

  ASSERT_EQ(0, Utils::divideAndRoundToNearest( 1, 4));
  ASSERT_EQ(1, Utils::divideAndRoundToNearest( 2, 4));
  ASSERT_EQ(1, Utils::divideAndRoundToNearest( 3, 4));
  ASSERT_EQ(1, Utils::divideAndRoundToNearest( 4, 4));
  ASSERT_EQ(1, Utils::divideAndRoundToNearest( 5, 4));
  ASSERT_EQ(2, Utils::divideAndRoundToNearest( 6, 4));
  ASSERT_EQ(2, Utils::divideAndRoundToNearest( 7, 4));
  ASSERT_EQ(2, Utils::divideAndRoundToNearest( 8, 4));
  ASSERT_EQ(2, Utils::divideAndRoundToNearest( 9, 4));
  ASSERT_EQ(3, Utils::divideAndRoundToNearest(10, 4));
  ASSERT_EQ(3, Utils::divideAndRoundToNearest(11, 4));
  ASSERT_EQ(3, Utils::divideAndRoundToNearest(12, 4));
  ASSERT_EQ(3, Utils::divideAndRoundToNearest(13, 4));
}

//------------------------------------------------------------------------------
// Test
//------------------------------------------------------------------------------
TEST_F(TgcUtilsTest, divideAndRoundUp) {
  using namespace eos::mgm::tgc;

  ASSERT_EQ(1, Utils::divideAndRoundUp( 1, 1));
  ASSERT_EQ(2, Utils::divideAndRoundUp( 2, 1));
  ASSERT_EQ(3, Utils::divideAndRoundUp( 3, 1));

  ASSERT_EQ(1, Utils::divideAndRoundUp( 1, 2));
  ASSERT_EQ(1, Utils::divideAndRoundUp( 2, 2));
  ASSERT_EQ(2, Utils::divideAndRoundUp( 3, 2));
  ASSERT_EQ(2, Utils::divideAndRoundUp( 4, 2));
  ASSERT_EQ(3, Utils::divideAndRoundUp( 5, 2));
  ASSERT_EQ(3, Utils::divideAndRoundUp( 6, 2));

  ASSERT_EQ(1, Utils::divideAndRoundUp( 1, 3));
  ASSERT_EQ(1, Utils::divideAndRoundUp( 2, 3));
  ASSERT_EQ(1, Utils::divideAndRoundUp( 3, 3));
  ASSERT_EQ(2, Utils::divideAndRoundUp( 4, 3));
  ASSERT_EQ(2, Utils::divideAndRoundUp( 5, 3));
  ASSERT_EQ(2, Utils::divideAndRoundUp( 6, 3));
  ASSERT_EQ(3, Utils::divideAndRoundUp( 7, 3));
  ASSERT_EQ(3, Utils::divideAndRoundUp( 8, 3));
  ASSERT_EQ(3, Utils::divideAndRoundUp( 9, 3));

  ASSERT_EQ(1, Utils::divideAndRoundUp( 1, 4));
  ASSERT_EQ(1, Utils::divideAndRoundUp( 2, 4));
  ASSERT_EQ(1, Utils::divideAndRoundUp( 3, 4));
  ASSERT_EQ(1, Utils::divideAndRoundUp( 4, 4));
  ASSERT_EQ(2, Utils::divideAndRoundUp( 5, 4));
  ASSERT_EQ(2, Utils::divideAndRoundUp( 6, 4));
  ASSERT_EQ(2, Utils::divideAndRoundUp( 7, 4));
  ASSERT_EQ(2, Utils::divideAndRoundUp( 8, 4));
  ASSERT_EQ(3, Utils::divideAndRoundUp( 9, 4));
  ASSERT_EQ(3, Utils::divideAndRoundUp(10, 4));
  ASSERT_EQ(3, Utils::divideAndRoundUp(11, 4));
  ASSERT_EQ(3, Utils::divideAndRoundUp(12, 4));
}
