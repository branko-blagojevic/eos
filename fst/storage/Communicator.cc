// ----------------------------------------------------------------------
// File: Communicator.cc
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

#include "fst/storage/Storage.hh"
#include "fst/XrdFstOfs.hh"
#include "fst/storage/FileSystem.hh"
#include "common/SymKeys.hh"

EOSFSTNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Get configuration value from global FST config
//------------------------------------------------------------------------------
bool
Storage::getFSTConfigValue(const std::string &key, std::string &value) const {
  eos::common::RWMutexReadLock lock(gOFS.ObjectManager.HashMutex);

  XrdMqSharedHash* hash = gOFS.ObjectManager.GetObject(
    Config::gConfig.getFstNodeConfigQueue("getConfigValue", false).c_str(),
    "hash");

  if(!hash) {
    return false;
  }

  value = hash->Get(key.c_str());
  return true;
}

bool
Storage::getFSTConfigValue(const std::string &key, unsigned long long &value) {
  std::string strVal;
  if(!getFSTConfigValue(key, strVal)) {
    return false;
  }

  value = atoi(strVal.c_str());
  return true;
}

//------------------------------------------------------------------------------
// Process incoming configuration change
//------------------------------------------------------------------------------
void
Storage::processIncomingFstConfigurationChange(const std::string &key) {
  std::string tmpValue;

  if(!getFSTConfigValue(key.c_str(), tmpValue)) {
    return;
  }

  if (key == "symkey") {
    eos_static_info("symkey=%s", tmpValue.c_str());
    eos::common::gSymKeyStore.SetKey64(tmpValue.c_str(), 0);
    return;
  }

  if (key == "manager") {
    eos_static_info("manager=%s", tmpValue.c_str());
    XrdSysMutexHelper lock(Config::gConfig.Mutex);
    Config::gConfig.Manager = tmpValue.c_str();
    return;
  }

  if (key == "publish.interval") {
    eos_static_info("publish.interval=%s", tmpValue.c_str());
    XrdSysMutexHelper lock(Config::gConfig.Mutex);
    Config::gConfig.PublishInterval = atoi(tmpValue.c_str());
    return;
  }

  if (key == "debug.level") {
    std::string debuglevel = tmpValue;
    eos::common::Logging& g_logging = eos::common::Logging::GetInstance();
    int debugval = g_logging.GetPriorityByString(debuglevel.c_str());

    if (debugval < 0) {
      eos_static_err("debug level %s is not known!", debuglevel.c_str());
    } else {
      // we set the shared hash debug for the lowest 'debug' level
      if (debuglevel == "debug") {
        gOFS.ObjectManager.SetDebug(true);
      } else {
        gOFS.ObjectManager.SetDebug(false);
      }

      g_logging.SetLogPriority(debugval);
    }
    return;
  }

  // creation/deletion of gateway transfer queue
  if (key == "txgw") {
    std::string gw = tmpValue;
    eos_static_info("txgw=%s", gw.c_str());

    if (gw == "off") {
      // just stop the multiplexer
      mGwMultiplexer.Stop();
      eos_static_info("Stopping transfer multiplexer");
    }

    if (gw == "on") {
      mGwMultiplexer.Run();
      eos_static_info("Starting transfer multiplexer");
    }

    return;
  }

  if (key == "gw.rate") {
    // modify the rate settings of the gw multiplexer
    std::string rate = tmpValue;
    eos_static_info("cmd=set gw.rate=%s", rate.c_str());
    mGwMultiplexer.SetBandwidth(atoi(rate.c_str()));
    return;
  }

  if (key == "gw.ntx") {
    // modify the parallel transfer settings of the gw multiplexer
    std::string ntx = tmpValue;
    eos_static_info("cmd=set gw.ntx=%s", ntx.c_str());
    mGwMultiplexer.SetSlots(atoi(ntx.c_str()));
    return;
  }

  if (key == "error.simulation") {
    std::string value = tmpValue;
    eos_static_info("cmd=set error.simulation=%s", tmpValue.c_str());
    gOFS.SetSimulationError(tmpValue.c_str());
    return;
  }

}


//------------------------------------------------------------------------------
// Communicator
//------------------------------------------------------------------------------
void
Storage::Communicator(ThreadAssistant& assistant)
{
  eos_static_info("Communicator activated ...");
  std::string watch_id = "id";
  std::string watch_bootsenttime = "bootsenttime";
  std::string watch_scaninterval = "scaninterval";
  std::string watch_scanrate = "scanrate";
  std::string watch_symkey = "symkey";
  std::string watch_manager = "manager";
  std::string watch_publishinterval = "publish.interval";
  std::string watch_debuglevel = "debug.level";
  std::string watch_gateway = "txgw";
  std::string watch_gateway_rate = "gw.rate";
  std::string watch_gateway_ntx = "gw.ntx";
  std::string watch_error_simulation = "error.simulation";
  std::string watch_regex = ".*";
  bool ok = true;
  ok &= gOFS.ObjectNotifier.SubscribesToKey("communicator", watch_id,
        XrdMqSharedObjectChangeNotifier::kMqSubjectModification);
  ok &= gOFS.ObjectNotifier.SubscribesToKey("communicator", watch_bootsenttime,
        XrdMqSharedObjectChangeNotifier::kMqSubjectModification);
  ok &= gOFS.ObjectNotifier.SubscribesToKey("communicator", watch_scanrate,
        XrdMqSharedObjectChangeNotifier::kMqSubjectModification);
  ok &= gOFS.ObjectNotifier.SubscribesToKey("communicator", watch_scaninterval,
        XrdMqSharedObjectChangeNotifier::kMqSubjectModification);
  ok &= gOFS.ObjectNotifier.SubscribesToKey("communicator", watch_symkey,
        XrdMqSharedObjectChangeNotifier::kMqSubjectModification);
  ok &= gOFS.ObjectNotifier.SubscribesToKey("communicator", watch_manager,
        XrdMqSharedObjectChangeNotifier::kMqSubjectModification);
  ok &= gOFS.ObjectNotifier.SubscribesToKey("communicator", watch_publishinterval,
        XrdMqSharedObjectChangeNotifier::kMqSubjectModification);
  ok &= gOFS.ObjectNotifier.SubscribesToKey("communicator", watch_debuglevel,
        XrdMqSharedObjectChangeNotifier::kMqSubjectModification);
  ok &= gOFS.ObjectNotifier.SubscribesToKey("communicator", watch_gateway,
        XrdMqSharedObjectChangeNotifier::kMqSubjectModification);
  ok &= gOFS.ObjectNotifier.SubscribesToKey("communicator", watch_gateway_rate,
        XrdMqSharedObjectChangeNotifier::kMqSubjectModification);
  ok &= gOFS.ObjectNotifier.SubscribesToKey("communicator", watch_gateway_ntx,
        XrdMqSharedObjectChangeNotifier::kMqSubjectModification);
  ok &= gOFS.ObjectNotifier.SubscribesToKey("communicator",
        watch_error_simulation,
        XrdMqSharedObjectChangeNotifier::kMqSubjectModification);
  ok &= gOFS.ObjectNotifier.SubscribesToSubjectRegex("communicator", watch_regex,
        XrdMqSharedObjectChangeNotifier::kMqSubjectCreation);

  if (!ok) {
    eos_crit("error subscribing to shared objects change notifications");
  }

  gOFS.ObjectNotifier.BindCurrentThread("communicator");

  if (!gOFS.ObjectNotifier.StartNotifyCurrentThread()) {
    eos_crit("error starting shared objects change notifications");
  }

  XrdSysThread::SetCancelDeferred();

  while (true) {
    // wait for new filesystem definitions
    gOFS.ObjectNotifier.tlSubscriber->mSubjSem.Wait();
    XrdSysThread::CancelPoint();
    eos_static_debug("received shared object notification ...");
    // we always take a lock to take something from the queue and then release it
    gOFS.ObjectNotifier.tlSubscriber->mSubjMtx.Lock();

    while (gOFS.ObjectNotifier.tlSubscriber->NotificationSubjects.size()) {
      XrdMqSharedObjectManager::Notification event;
      event = gOFS.ObjectNotifier.tlSubscriber->NotificationSubjects.front();
      gOFS.ObjectNotifier.tlSubscriber->NotificationSubjects.pop_front();
      gOFS.ObjectNotifier.tlSubscriber->mSubjMtx.UnLock();
      eos_static_info("FST shared object notification subject is %s",
                      event.mSubject.c_str());
      XrdOucString queue = event.mSubject.c_str();

      if (event.mType == XrdMqSharedObjectManager::kMqSubjectCreation) {
        // Handle subject creation
        if (queue == Config::gConfig.FstQueueWildcard) {
          gOFS.ObjectNotifier.tlSubscriber->mSubjMtx.Lock();
          continue;
        }

        if ((queue.find("/txqueue/") != STR_NPOS)) {
          // this is a transfer queue we, don't need to take action
          gOFS.ObjectNotifier.tlSubscriber->mSubjMtx.Lock();
          continue;
        }

        if (!queue.beginswith(Config::gConfig.FstQueue)) {
          // ! endswith seems to be buggy if the comparable suffix is longer than the string !
          if (queue.beginswith("/config/") &&
              (queue.length() > Config::gConfig.FstHostPort.length()) &&
              queue.endswith(Config::gConfig.FstHostPort)) {
            // This is the configuration entry and we should store it to have
            // access to it since it's name depends on the instance name and
            // we don't know (yet)
            Config::gConfig.setFstNodeConfigQueue(queue);
            eos_static_info("storing config queue name <%s>", queue.c_str());
          } else {
            eos_static_info("no action on creation of subject <%s> - we are <%s>",
                            event.mSubject.c_str(), Config::gConfig.FstQueue.c_str());
          }

          gOFS.ObjectNotifier.tlSubscriber->mSubjMtx.Lock();
          continue;
        } else {
          eos_static_info("received creation notification of subject <%s> - we are <%s>",
                          event.mSubject.c_str(), Config::gConfig.FstQueue.c_str());
        }

        eos::common::RWMutexWriteLock lock(mFsMutex);
        FileSystem* fs = 0;

        if (!(mQueue2FsMap.count(queue.c_str()))) {

          common::FileSystemLocator locator;
          if(!common::FileSystemLocator::fromQueuePath(queue.c_str(), locator)) {
            eos_static_crit("Unable to parse queuepath: %s", queue.c_str());
            continue;
          }

          fs = new FileSystem(locator, &gOFS.ObjectManager, gOFS.mQSOM.get());
          mQueue2FsMap[queue.c_str()] = fs;
          mFsVect.push_back(fs);
          mFileSystemsMap[fs->GetId()] = fs;
          eos_static_info("setting up filesystem %s", queue.c_str());
          fs->SetStatus(eos::common::BootStatus::kDown);
        }

        gOFS.ObjectNotifier.tlSubscriber->mSubjMtx.Lock();
        continue;
      }

      if (event.mType == XrdMqSharedObjectManager::kMqSubjectDeletion) {
        // Handle subject deletion
        if ((queue.find("/txqueue/") != STR_NPOS)) {
          // this is a transfer queue we, don't need to take action
          gOFS.ObjectNotifier.tlSubscriber->mSubjMtx.Lock();
          continue;
        }

        if (!queue.beginswith(Config::gConfig.FstQueue)) {
          eos_static_err("illegal subject found in deletion list <%s> - we are <%s>",
                         event.mSubject.c_str(), Config::gConfig.FstQueue.c_str());
          gOFS.ObjectNotifier.tlSubscriber->mSubjMtx.Lock();
          continue;
        } else {
          eos_static_info("received deletion notification of subject <%s> - we are <%s>",
                          event.mSubject.c_str(), Config::gConfig.FstQueue.c_str());
        }

        // we don't delete filesystem objects anymore ...
        gOFS.ObjectNotifier.tlSubscriber->mSubjMtx.Lock();
        continue;
      }

      if (event.mType == XrdMqSharedObjectManager::kMqSubjectModification) {
        // Handle subject modification
        // seperate <path> from <key>
        XrdOucString key = queue;
        int dpos = 0;

        if ((dpos = queue.find(";")) != STR_NPOS) {
          key.erase(0, dpos + 1);
          queue.erase(dpos);
        }

        std::string tmpValue;

        if (queue == Config::gConfig.getFstNodeConfigQueue("communicator", false)) {
          processIncomingFstConfigurationChange(key.c_str());


        } else {
          mFsMutex.LockRead();

          if ((mQueue2FsMap.count(queue.c_str()))) {
            eos_static_info("got modification on <subqueue>=%s <key>=%s", queue.c_str(),
                            key.c_str());
            gOFS.ObjectManager.HashMutex.LockRead();
            XrdMqSharedHash* hash = gOFS.ObjectManager.GetObject(queue.c_str(), "hash");

            if (hash) {
              if (key == "id") {
                unsigned int fsid = hash->GetUInt(key.c_str());
                gOFS.ObjectManager.HashMutex.UnLockRead();

                if ((!mFileSystemsMap.count(fsid)) ||
                    (mFileSystemsMap[fsid] != mQueue2FsMap[queue.c_str()])) {
                  mFsMutex.UnLockRead();
                  mFsMutex.LockWrite();
                  // setup the reverse lookup by id
                  mFileSystemsMap[fsid] = mQueue2FsMap[queue.c_str()];
                  eos_static_info("setting reverse lookup for fsid %u", fsid);
                  mFsMutex.UnLockWrite();
                  mFsMutex.LockRead();
                }

                // check if we are autobooting
                if (eos::fst::Config::gConfig.autoBoot &&
                    (mQueue2FsMap[queue.c_str()]->GetStatus() <= eos::common::BootStatus::kDown) &&
                    (mQueue2FsMap[queue.c_str()]->GetConfigStatus() >
                     eos::common::ConfigStatus::kOff)) {
                  // start a boot thread
                  RunBootThread(mQueue2FsMap[queue.c_str()]);
                }
              } else {
                gOFS.ObjectManager.HashMutex.UnLockRead();

                if (key == "bootsenttime") {
                  // Request to (re-)boot a filesystem
                  if (mQueue2FsMap.count(queue.c_str())) {
                    if ((mQueue2FsMap[queue.c_str()]->GetInternalBootStatus() ==
                         eos::common::BootStatus::kBooted)) {
                      if (mQueue2FsMap[queue.c_str()]->GetLongLong("bootcheck")) {
                        eos_static_info("queue=%s status=%d check=%lld msg='boot enforced'",
                                        queue.c_str(), mQueue2FsMap[queue.c_str()]->GetStatus(),
                                        mQueue2FsMap[queue.c_str()]->GetLongLong("bootcheck"));
                        RunBootThread(mQueue2FsMap[queue.c_str()]);
                      } else {
                        eos_static_info("queue=%s status=%d check=%lld msg='skip boot - we are already booted'",
                                        queue.c_str(), mQueue2FsMap[queue.c_str()]->GetStatus(),
                                        mQueue2FsMap[queue.c_str()]->GetLongLong("bootcheck"));
                        mQueue2FsMap[queue.c_str()]->SetStatus(eos::common::BootStatus::kBooted);
                      }
                    } else {
                      eos_static_info("queue=%s status=%d check=%lld msg='booting - we are not booted yet'",
                                      queue.c_str(), mQueue2FsMap[queue.c_str()]->GetStatus(),
                                      mQueue2FsMap[queue.c_str()]->GetLongLong("bootcheck"));
                      // start a boot thread;
                      RunBootThread(mQueue2FsMap[queue.c_str()]);
                    }
                  } else {
                    eos_static_err("got boot time update on not existant filesystem %s",
                                   queue.c_str());
                  }
                } else {
                  if ((key == "scaninterval") || (key == "scanrate")) {
                    auto it_fs = mQueue2FsMap.find(queue.c_str());

                    if (it_fs != mQueue2FsMap.end()) {
                      FileSystem* fs = it_fs->second;
                      long long value = fs->GetLongLong(key.c_str());

                      if (value > 0) {
                        fs->ConfigScanner(&mFstLoad, key.c_str(), value);
                      }
                    }
                  }
                }
              }
            } else {
              gOFS.ObjectManager.HashMutex.UnLockRead();
            }
          } else {
            eos_static_err("illegal subject found - no filesystem object "
                           "existing for modification %s;%s",
                           queue.c_str(), key.c_str());
          }

          mFsMutex.UnLockRead();
        }

        gOFS.ObjectNotifier.tlSubscriber->mSubjMtx.Lock();
        continue;
      }
    }

    gOFS.ObjectNotifier.tlSubscriber->mSubjMtx.UnLock();
    XrdSysThread::CancelPoint();
  }

  XrdSysThread::SetCancelOn();
}

//------------------------------------------------------------------------------
// QdbCommunicator
//------------------------------------------------------------------------------
void
Storage::QdbCommunicator(QdbContactDetails contactDetails,
                         ThreadAssistant& assistant)
{
  while (!assistant.terminationRequested()) {
    assistant.wait_for(std::chrono::seconds(1));
  }
}

EOSFSTNAMESPACE_END
