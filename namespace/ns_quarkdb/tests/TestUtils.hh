//------------------------------------------------------------------------------
// File: TestUtils.hh
// Author: Georgios Bitzes <georgios.bitzes@cern.ch>
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2017 CERN/Switzerland                                  *
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
#pragma once

#include <gtest/gtest.h>
#include "Namespace.hh"
#include "qclient/Members.hh"
#include <memory>

#define DBG(message) std::cerr << __FILE__ << ":" << __LINE__ << " -- " << #message << " = " << message << std::endl

namespace qclient
{
class QClient;
}

namespace eos
{
class IContainerMDSvc;
class IFileMDSvc;
class IView;
class IFsView;
class MetadataFlusher;
class IFileMD;
}

EOSNSTESTING_BEGIN

//------------------------------------------------------------------------------
// Verify contents of iterator (unordered)
//------------------------------------------------------------------------------
template<typename T, typename Iterator>
bool verifyContents(Iterator it, std::set<T> contents) {
  while(true) {
    if(!it->valid() && !contents.empty()) {
      std::cerr << "Iterator is no longer valid, but set contains more items!" << std::endl;
      return false;
    }

    if(!it->valid() && contents.empty()) {
      // All done, everything looks good.
      return true;
    }

    if(contents.count(it->getElement()) != 1u) {
      std::cerr << "Found item in iterator which is not in the set!" << std::endl;
      return false;
    }

    contents.erase(it->getElement());
    it->next();
  }
}

//------------------------------------------------------------------------------
// Verify contents of iterator (unordered)
//------------------------------------------------------------------------------
template<typename T, typename Iterator>
bool verifyContents(Iterator start, Iterator end, std::set<T> contents) {
  for(auto it = start; it != end; it++) {
    if(contents.count(*it) != 1u) {
      std::cerr << "Found item in iterator which is not in the set!" << std::endl;
      return false;
    }

    contents.erase(*it);
  }

  if(!contents.empty()) {
    std::cerr << "Iterator is no longer valid, but set contains more items!" << std::endl;
    return false;
  }

  // All done, everything looks good.
  return true;
}

//------------------------------------------------------------------------------
//! Class FlushAllOnDestruction
//------------------------------------------------------------------------------
class FlushAllOnConstruction
{
public:
  FlushAllOnConstruction(const qclient::Members& mbr);
  ~FlushAllOnConstruction();

private:
  qclient::Members members;
};

//------------------------------------------------------------------------------
//! Test fixture providing generic utilities and initialization / destruction
//! boilerplate code
//------------------------------------------------------------------------------

typedef uint64_t (*SizeMapper)(const IFileMD* file);

class NsTestsFixture : public ::testing::Test
{
public:
  NsTestsFixture();
  ~NsTestsFixture();

  // Lazy initialization.
  eos::IContainerMDSvc* containerSvc();
  eos::IFileMDSvc* fileSvc();
  eos::IView* view();
  eos::IFsView* fsview();

  void shut_down_everything();
  void shut_down_view();

  // explicit transfer of ownership
  std::unique_ptr<qclient::QClient> createQClient();

  // Return test cluster members
  qclient::Members getMembers();

  // Return default qclient instance. Use if you need just one qclient, and
  // you're too lazy to call createQClient.
  qclient::QClient& qcl();

  // Return flushers
  eos::MetadataFlusher* mdFlusher();
  eos::MetadataFlusher* quotaFlusher();

  // Register size mapper
  void setSizeMapper(SizeMapper sizeMapper);

  // Populate namespace with dummy test data.
  void populateDummyData1();

private:
  void initServices();

  std::map<std::string, std::string> testconfig;
  std::unique_ptr<eos::ns::testing::FlushAllOnConstruction> guard;
  std::unique_ptr<eos::IContainerMDSvc> containerSvcPtr;
  std::unique_ptr<eos::IFileMDSvc> fileSvcPtr;
  std::unique_ptr<eos::IView> viewPtr;
  std::unique_ptr<eos::IFsView> fsViewPtr;

  std::unique_ptr<qclient::QClient> qclPtr;

  // No ownership
  eos::MetadataFlusher* mdFlusherPtr = nullptr;
  eos::MetadataFlusher* quotaFlusherPtr = nullptr;

  // Size mapper, if avaliable
  SizeMapper sizeMapper = nullptr;
};




EOSNSTESTING_END
