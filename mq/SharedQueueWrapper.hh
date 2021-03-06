// ----------------------------------------------------------------------
// File: SharedQueueWrapper.hh
// Author: Georgios Bitzes - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2019 CERN/Switzerland                                  *
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

#ifndef EOS_MQ_SHARED_QUEUE_WRAPPER_HH
#define EOS_MQ_SHARED_QUEUE_WRAPPER_HH

#include "mq/Namespace.hh"
#include "common/Locators.hh"
#include <memory>

namespace qclient {
  class SharedDeque;
}

class XrdMqSharedHash;
class XrdMqSharedObjectManager;

EOSMQNAMESPACE_BEGIN

class MessagingRealm;

//------------------------------------------------------------------------------
//! Compatibility class for shared queues - work in progress.
//------------------------------------------------------------------------------
class SharedQueueWrapper {
public:

  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  SharedQueueWrapper(mq::MessagingRealm* realm,
    const common::TransferQueueLocator& locator, bool broadcast);

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  ~SharedQueueWrapper();

  //----------------------------------------------------------------------------
  //! Clear contents
  //----------------------------------------------------------------------------
  void clear();

  //----------------------------------------------------------------------------
  //! Get size
  //----------------------------------------------------------------------------
  size_t size();

  //----------------------------------------------------------------------------
  //! Get item, if available
  //----------------------------------------------------------------------------
  std::string getItem();

  //----------------------------------------------------------------------------
  //! push item
  //----------------------------------------------------------------------------
  bool push_back(const std::string &item);

private:
  mq::MessagingRealm *mRealm;
  common::TransferQueueLocator mLocator;
  bool mBroadcast;
  std::string mQueue;
  std::string mFullQueue;

  XrdMqSharedObjectManager* mSom;
  std::shared_ptr<qclient::SharedDeque> mSharedDeque;

};


EOSMQNAMESPACE_END

#endif