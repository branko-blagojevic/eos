// ----------------------------------------------------------------------
// File: TransferQueue.cc
// Author: Andreas-Joachim Peters - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2011 CERN/Switzerland                                  *
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

/*----------------------------------------------------------------------------*/
#include "common/TransferQueue.hh"
#include "common/StringTokenizer.hh"
#include <qclient/structures/QDeque.hh>
#include <qclient/shared/SharedManager.hh>
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/


EOSCOMMONNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
TransferQueueLocator::TransferQueueLocator(const FileSystemLocator &fsLocator, const std::string &tag)
: mLocator(fsLocator), mTag(tag) {}

//------------------------------------------------------------------------------
// Constructor: Queue tied to an FST
//------------------------------------------------------------------------------
TransferQueueLocator::TransferQueueLocator(const std::string &fstQueue, const std::string &tag)
: mFstQueue(fstQueue), mTag(tag) {}

//------------------------------------------------------------------------------
// Get "queue"
//------------------------------------------------------------------------------
std::string TransferQueueLocator::getQueue() const {
  if(!mFstQueue.empty()) {
    return mFstQueue;
  }
  else {
    return mLocator.getFSTQueue();
  }
}

//------------------------------------------------------------------------------
// Get "queuepath"
//------------------------------------------------------------------------------
std::string TransferQueueLocator::getQueuePath() const {
  if(!mFstQueue.empty()) {
    return SSTR(mFstQueue << "/gw/txqueue/" << mTag);
  }
  else {
    return SSTR(mLocator.getQueuePath() << "/txqueue/" << mTag);
  }
}

//------------------------------------------------------------------------------
// Get QDB key for this queue
//------------------------------------------------------------------------------
std::string TransferQueueLocator::getQDBKey() const {
  if(!mFstQueue.empty()) {
    std::vector<std::string> parts;
    parts = eos::common::StringTokenizer::split<std::vector<std::string>>(mFstQueue, '/');
    return SSTR("txqueue-fst||" << parts[1] << "||" << mTag);
  }
  else {
    return SSTR("txqueue-filesystem||" << mLocator.getHostPort() << "||" << mLocator.getStoragePath() << "||" << mTag);
  }
}

/*----------------------------------------------------------------------------*/
/**
 * Constructor for a transfer queue
 *
 * @param queue name of the queue e.g. /eos/'host'/fst/
 * @param queuepath name of the queue path e.g. /eos/'host'/fst/'mountpoint'/
 * @param subqueue name of the subqueue e.g. drainq,balanceq,externalq
 * @param fs pointer to filesytem object to add the queue
 * @param som pointer to shared object manager
 * @param bc2mgm broadcast-to-manager flag indicating if changes are broadcasted to manager nodes
 */
/*----------------------------------------------------------------------------*/
TransferQueue::TransferQueue(const TransferQueueLocator &locator, XrdMqSharedObjectManager* som, qclient::SharedManager* qsom, bool bc2mgm)
{
  mQueue = locator.getQueue();
  mFullQueue = locator.getQueuePath();
  mJobGetCount = 0;

  if (bc2mgm)
  {
    // the fst has to reply to the mgm and set up the right broadcast queue
    mQueue = "/eos/*/mgm";
    mSlave = true;
  }
  else
  {
    mSlave = false;
  }


  mSom = som;
  mQsom = qsom;

  if(mQsom) {
    mQDeque.reset(new qclient::QDeque(*mQsom->getQClient(), locator.getQDBKey()));
    if(!mSlave) {
      mQDeque->clear();
    }
  }
  else if (mSom) {
    mSom->HashMutex.LockRead();
    XrdMqSharedQueue* hashQueue = (XrdMqSharedQueue*) mSom->GetObject(mFullQueue.c_str(), "queue");
    if(!hashQueue) {
      mSom->HashMutex.UnLockRead();
      // create the hash object
      if (mSom->CreateSharedQueue(mFullQueue.c_str(), mQueue.c_str(), som)) {
        mSom->HashMutex.LockRead();
        hashQueue = (XrdMqSharedQueue*) mSom->GetObject(mFullQueue.c_str(), "queue");
        mSom->HashMutex.UnLockRead();
      }
    }
    else {
      // remove all scheduled objects
      if (!mSlave) {
        hashQueue->Clear();
      }
      mSom->HashMutex.UnLockRead();
    }
  }
}

//------------------------------------------------------------------------------
//! Get queue path
//------------------------------------------------------------------------------
std::string TransferQueue::getQueuePath() const {
  return mFullQueue;
}

/*----------------------------------------------------------------------------*/
//! Destructor
/*----------------------------------------------------------------------------*/
TransferQueue::~TransferQueue ()
{
  if (!mSlave) {
    Clear();
  }
}

/*----------------------------------------------------------------------------*/
/**
 * Add a transfer job to the queue
 *
 * @param job pointer to job to add
 *
 * @return true if successful otherwise false
 */

/*----------------------------------------------------------------------------*/
bool
TransferQueue::Add (eos::common::TransferJob* job)
{
  bool retc = false;
  if(mQsom) {
    return mQDeque->push_back(job->GetSealed()).ok();
  }
  else if (mSom)
  {
    mSom->HashMutex.LockRead();
    XrdMqSharedQueue* hashQueue = (XrdMqSharedQueue*) mSom->GetQueue(mFullQueue.c_str());
    if(hashQueue) {
      retc = hashQueue->PushBack("", job->GetSealed());
    }
    else {
      fprintf(stderr, "error: couldn't get queue %s!\n", mFullQueue.c_str());
    }
    mSom->HashMutex.UnLockRead();
  }
  return retc;
}

/*----------------------------------------------------------------------------*/
/**
 * Get a job from the queue. The caller has to clean-up the job object.
 *
 *
 * @return pointer to job
 */

/*----------------------------------------------------------------------------*/
std::unique_ptr<TransferJob>
TransferQueue::Get ()
{
  if(mQsom) {
    std::string sealed;
    if(!mQDeque->pop_front(sealed).ok()) {
      return {};
    }

    std::unique_ptr<TransferJob> job = TransferJob::Create(sealed.c_str());
    IncGetJobCount();
    return job;
  }
  else if (mSom) {
    mSom->HashMutex.LockRead();

    XrdMqSharedQueue* hashQueue = (XrdMqSharedQueue*) mSom->GetQueue(mFullQueue.c_str());
    if(hashQueue) {
      std::string value = hashQueue->PopFront();
      mSom->HashMutex.UnLockRead();

      if (value.empty()) {
        return 0;
      } else {
        std::unique_ptr<TransferJob> job = TransferJob::Create(value.c_str());
        IncGetJobCount();
        return job;
      }
    } else {
      fprintf(stderr, "error: couldn't get queue %s!\n", mFullQueue.c_str());
    }

    mSom->HashMutex.UnLockRead();
  }
  return 0;
}

// ---------------------------------------------------------------------------
//! Clear all jobs from the queue
// ---------------------------------------------------------------------------
bool TransferQueue::Clear()
{
  if(mQsom) {
    return mQDeque->clear().ok();
  }
  else if (mSom) {
    RWMutexReadLock lock(mSom->HashMutex);
    XrdMqSharedQueue* hashQueue = (XrdMqSharedQueue*) mSom->GetQueue(
                                  mFullQueue.c_str());

    if (hashQueue) {
      hashQueue->Clear();
      return true;
    }
  }

  return false;
}

//------------------------------------------------------------------------------
//! Get the current size of the queue
//------------------------------------------------------------------------------
size_t TransferQueue::Size()
{
  if(mQsom) {
    size_t output = 0;
    mQDeque->size(output);
    return output;
  }
  else if (mSom) {
    RWMutexReadLock lock(mSom->HashMutex);
    XrdMqSharedQueue* hashQueue = (XrdMqSharedQueue*) mSom->GetQueue(
                                  mFullQueue.c_str());

    if (hashQueue) {
      return hashQueue->GetSize();
    }
  }

  return 0;
}

/*----------------------------------------------------------------------------*/

EOSCOMMONNAMESPACE_END
