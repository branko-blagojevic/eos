//------------------------------------------------------------------------------
// File: TapeAwareLruTests.cc
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

#include "mgm/tgc/DummyTapeGcMgm.hh"
#include "mgm/tgc/SpaceToTapeGcMap.hh"

#include <gtest/gtest.h>

class TgcSpaceToTapeGcMapTest : public ::testing::Test {
protected:

  virtual void SetUp() {
  }

  virtual void TearDown() {
  }
};

//------------------------------------------------------------------------------
// Test
//------------------------------------------------------------------------------
TEST_F(TgcSpaceToTapeGcMapTest, Constructor)
{
  using namespace eos::mgm::tgc;

  DummyTapeGcMgm mgm;
  SpaceToTapeGcMap map(mgm);
}

//------------------------------------------------------------------------------
// Test
//------------------------------------------------------------------------------
TEST_F(TgcSpaceToTapeGcMapTest, getGc_unknown_eos_space)
{
  using namespace eos::mgm::tgc;

  DummyTapeGcMgm mgm;
  SpaceToTapeGcMap map(mgm);
  const std::string space = "space";

  ASSERT_THROW(map.getGc(space), SpaceToTapeGcMap::UnknownEOSSpace);
}

//------------------------------------------------------------------------------
// Test
//------------------------------------------------------------------------------
TEST_F(TgcSpaceToTapeGcMapTest, createGc)
{
  using namespace eos::mgm::tgc;

  DummyTapeGcMgm mgm;
  SpaceToTapeGcMap map(mgm);
  const std::string space = "space";

  TapeGc &gc1 = map.createGc(space);

  TapeGc &gc2 = map.getGc(space);

  ASSERT_EQ(&gc1, &gc2);
}

//------------------------------------------------------------------------------
// Test
//------------------------------------------------------------------------------
TEST_F(TgcSpaceToTapeGcMapTest, createGc_already_exists)
{
  using namespace eos::mgm::tgc;

  DummyTapeGcMgm mgm;
  SpaceToTapeGcMap map(mgm);
  const std::string space = "space";

  map.createGc(space);

  ASSERT_THROW(map.createGc(space), SpaceToTapeGcMap::GcAlreadyExists);
}
