//------------------------------------------------------------------------------
// File: TgcCachedValueTests.cc
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

class TgcCachedValueTest : public ::testing::Test {
protected:

  virtual void SetUp() {
  }

  virtual void TearDown() {
  }
};

TEST_F(TgcCachedValueTest, get_value_not_change)
{
  using namespace eos::mgm::tgc;

  const uint64_t sourceValue = 1234;
  auto getter = [&sourceValue]()->uint64_t{return sourceValue;};
  const time_t maxAgeSecs = 0;
  CachedValue<uint64_t> cachedValue(getter, maxAgeSecs);

  {
    const auto retrievedValue = cachedValue.get();
    ASSERT_EQ(retrievedValue.current, retrievedValue.prev);
    ASSERT_EQ(sourceValue, retrievedValue.current);
  }
}

//------------------------------------------------------------------------------
// Test
//------------------------------------------------------------------------------
TEST_F(TgcCachedValueTest, get_value_changed_no_cache)
{
  using namespace eos::mgm::tgc;

  uint64_t sourceValue = 1234;
  auto getter = [&sourceValue]()->uint64_t{return sourceValue;};
  const time_t maxAgeSecs = 0; // No cache
  CachedValue<uint64_t> cachedValue(getter, maxAgeSecs);

  {
    const auto retrievedValue = cachedValue.get();
    ASSERT_EQ(1234, retrievedValue.prev);
    ASSERT_EQ(1234, retrievedValue.current);
  }

  sourceValue = 5678;

  {
    const auto retrievedValue = cachedValue.get();
    ASSERT_EQ(1234, retrievedValue.prev);
    ASSERT_EQ(5678, retrievedValue.current);
  }

  {
    const auto retrievedValue = cachedValue.get();
    ASSERT_EQ(5678, retrievedValue.prev);
    ASSERT_EQ(5678, retrievedValue.current);
  }
}

//------------------------------------------------------------------------------
// Test
//------------------------------------------------------------------------------
TEST_F(TgcCachedValueTest, get_value_changed_long_wait_cache) {
  using namespace eos::mgm::tgc;

  uint64_t sourceValue = 1234;
  auto getter = [&sourceValue]() -> uint64_t { return sourceValue; };
  const time_t maxAgeSecs = 1000; // Long wait cache
  CachedValue<uint64_t> cachedValue(getter, maxAgeSecs);

  {
    const auto retrievedValue = cachedValue.get();
    ASSERT_EQ(retrievedValue.current, retrievedValue.prev);
    ASSERT_EQ(1234, retrievedValue.current);
  }

  sourceValue = 5678;

  {
    const auto retrievedValue = cachedValue.get();
    ASSERT_EQ(1234, retrievedValue.prev);
    ASSERT_EQ(1234, retrievedValue.current);
  }
}
