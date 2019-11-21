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

//------------------------------------------------------------------------------
//! @author Georgios Bitzes <georgios.bitzes@cern.ch>
//! @brief Container map iterators
//------------------------------------------------------------------------------

#ifndef EOS_NS_CONTAINER_ITERATORS_HH
#define EOS_NS_CONTAINER_ITERATORS_HH

#include "namespace/Namespace.hh"
#include "namespace/interface/IContainerMD.hh"
#include "namespace/interface/IFileMD.hh"

EOSNSNAMESPACE_BEGIN

//------------------------------------------------------------------------------
//! Class FileMapIterator
//------------------------------------------------------------------------------
class FileMapIterator {
public:
  FileMapIterator(IContainerMDPtr cont)
    : container(cont), mLock(cont->mMutex) {
    std::shared_lock<std::shared_timed_mutex> lock(mLock);
    iter = cont->filesBegin();
  }

  bool valid() const {
    return iter != container->filesEnd();
  }

  void next() {
    std::shared_lock<std::shared_timed_mutex> lock (mLock);
    iter++;
  }

  std::string key() const {
    return iter->first;
  }

  IFileMD::id_t value() const {
    return iter->second;
  }

private:
  IContainerMDPtr container;
  std::shared_timed_mutex &mLock;
  eos::IContainerMD::FileMap::const_iterator iter;
};

//------------------------------------------------------------------------------
//! Class ContainerIterator
//------------------------------------------------------------------------------
class ContainerMapIterator {
public:
  ContainerMapIterator(IContainerMDPtr cont)
    : container(cont), mLock(cont->mMutex) { 
    std::shared_lock<std::shared_timed_mutex> lock (mLock);
    iter = cont->subcontainersBegin();
  }

  bool valid() const {
    return iter != container->subcontainersEnd();
  }

  void next() {
    std::shared_lock<std::shared_timed_mutex> lock (mLock);
    iter++;
  }

  std::string key() const {
    return iter->first;
  }

  IFileMD::id_t value() const {
    return iter->second;
  }

private:
  IContainerMDPtr container;
  std::shared_timed_mutex &mLock;
  eos::IContainerMD::ContainerMap::const_iterator iter;
};

EOSNSNAMESPACE_END

#endif
