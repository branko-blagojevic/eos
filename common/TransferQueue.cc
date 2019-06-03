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
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/


EOSCOMMONNAMESPACE_BEGIN

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
TransferQueue::TransferQueue (const char* queue, const char* queuepath, const char* subqueue, XrdMqSharedObjectManager* som, bool bc2mgm)
{
  mQueue = queue;
  mFullQueue = queuepath;
  mFullQueue += "/txqueue/";
  mFullQueue += subqueue;
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
  if (mSom) {
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

/*----------------------------------------------------------------------------*/
//! Destructor

/*----------------------------------------------------------------------------*/
TransferQueue::~TransferQueue ()
{
  if (!mSlave)
  {
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
  if (mSom)
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
TransferJob*
TransferQueue::Get ()
{
  if (mSom)
  {
    mSom->HashMutex.LockRead();

    XrdMqSharedQueue* hashQueue = (XrdMqSharedQueue*) mSom->GetQueue(mFullQueue.c_str());
    if(hashQueue) {
      std::string value = hashQueue->PopFront();

      if (value.empty()) {
        mSom->HashMutex.UnLockRead();
        return 0;
      } else {
        TransferJob* job = TransferJob::Create(value.c_str());
        mSom->HashMutex.UnLockRead();
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

/*----------------------------------------------------------------------------*/

EOSCOMMONNAMESPACE_END
