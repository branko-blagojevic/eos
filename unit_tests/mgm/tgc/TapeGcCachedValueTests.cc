//------------------------------------------------------------------------------
// File: TapeGcCachedValueTests.cc
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

#include "mgm/tgc/CachedValue.hh"

#include <gtest/gtest.h>
#include <stdint.h>

class TapeGcCachedValueTest : public ::testing::Test {
protected:

  virtual void SetUp() {
  }

  virtual void TearDown() {
  }
};

//------------------------------------------------------------------------------
// Test
//------------------------------------------------------------------------------
TEST_F(TapeGcCachedValueTest, changedFollowedByNoChange)
{
  using namespace eos::mgm::tgc;

  const uint64_t value = 5678;
  auto getter = [value]()->uint64_t{return value;};
  const time_t maxAgeSecs = 1000;
  CachedValue<uint64_t> cachedValue(getter, maxAgeSecs);

  {
    bool valueChanged = false;
    const uint64_t firstRetrievedValue = cachedValue.get(valueChanged);

    ASSERT_EQ(value, firstRetrievedValue);
    ASSERT_TRUE(valueChanged);
  }

  {
    bool valueChanged = false;
    const uint64_t firstRetrievedValue = cachedValue.get(valueChanged);

    ASSERT_EQ(value, firstRetrievedValue);
    ASSERT_FALSE(valueChanged);
  }
}
