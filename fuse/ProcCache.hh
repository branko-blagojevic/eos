// ----------------------------------------------------------------------
// File: ProcCache.hh
// Author: Geoffray Adde - CERN
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

#ifndef __PROCCACHE__HH__
#define __PROCCACHE__HH__

#include <common/RWMutex.hh>
#include <fstream>
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <unistd.h>
#include <krb5.h>
#include "common/Logging.hh"
#include "CredentialFinder.hh"
#include "ProcessInfo.hh"

/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
class ProcCache;

/*----------------------------------------------------------------------------*/
/**
 * @brief Class representing a Proc File information cache entry for one pid.
 *
 */
/*----------------------------------------------------------------------------*/
class ProcCacheEntry
{
  friend class ProcCache;
  // RWMutex to protect entry
  mutable eos::common::RWMutex pMutex;

  const pid_t pid;

  // internal values
  ProcessInfo pInfo;

  BoundIdentity boundIdentity;
  mutable int pError;
  mutable std::string pErrMessage;

  //! return true if the information is up-to-date after the call, false else
  int
  UpdateIfPsChanged();

public:
  ProcCacheEntry(pid_t pid_init) : pid(pid_init), pError(0)
  {
    pMutex.SetBlocking(true);
  }

  ~ProcCacheEntry()
  {
  }

  bool HasBoundIdentity() const
  {
    eos::common::RWMutexReadLock lock(pMutex);
    if(!boundIdentity.getCreds()) return false;
    return !boundIdentity.getCreds()->empty();
  }

  bool GetBoundIdentity(BoundIdentity& value) const
  {
    eos::common::RWMutexReadLock lock(pMutex);

    if ( boundIdentity.getCreds()->empty()) {
      return false;
    }

    value = boundIdentity;
    return true;
  }

  bool SetBoundIdentity(const BoundIdentity& value)
  {
    eos::common::RWMutexWriteLock lock(pMutex);
    boundIdentity = value;
    return true;
  }

  bool GetSid(pid_t& sid) const
  {
    eos::common::RWMutexReadLock lock(pMutex);
    sid = pInfo.getSid();
    return true;
  }

  bool GetStartupTime(Jiffies &sut) const
  {
    eos::common::RWMutexReadLock lock(pMutex);
    sut = pInfo.getStartTime();
    return true;
  }

  const std::vector<std::string>&
  GetArgsVec() const
  {
    eos::common::RWMutexReadLock lock(pMutex);
    return pInfo.getCmd();
  }

  const std::string&
  GetArgsStr() const
  {
    eos::common::RWMutexReadLock lock(pMutex);
    return pInfo.cmdStr; // TODO(gbitzes): Maybe remove this function eventually?
  }

  bool HasError() const
  {
    eos::common::RWMutexReadLock lock(pMutex);
    return pError;
  }

  const std::string&
  GetErrorMessage() const
  {
    eos::common::RWMutexReadLock lock(pMutex);
    return pErrMessage;
  }
};

/*----------------------------------------------------------------------------*/
/**
 * @brief Class representing a Proc File information cache catalog.
 *
 */
/*----------------------------------------------------------------------------*/
class ProcCache
{
  // RWMUtex; Mutex to protect catalog
  std::map<int, ProcCacheEntry*> pCatalog;
  // RWMutex to protect entry
  eos::common::RWMutex pMutex;
  // path od the proc filesystem
  std::string pProcPath;

public:
  ProcCache()
  {
    pMutex.SetBlocking(true);
    pProcPath = "/proc/";
  }
  ~ProcCache()
  {
    eos::common::RWMutexWriteLock lock(pMutex);

    for (auto it = pCatalog.begin(); it != pCatalog.end(); it++) {
      delete it->second;
    }
  }

  //! returns true if the cache has an entry for the given pid, false else
  //! regardless of the fact it's up-to-date or not
  bool HasEntry(int pid)
  {
    return static_cast<bool>(pCatalog.count(pid));
  }

  void
  SetProcPath(const char* procpath)
  {
    pProcPath = procpath;
  }

  const std::string&
  GetProcPath() const
  {
    return pProcPath;
  }

  //! returns true if the cache has an up-to-date entry after the call
  int
  InsertEntry(int pid)
  {
    int errCode;
    eos::common::RWMutexWriteLock lock(pMutex);

    // if there is no such process return an error and remove the entry from the cache
    if (getpgid(pid) < 0) {
      RemoveEntry(pid);
      return ESRCH;
    }

    if (!HasEntry(pid)) {
      pCatalog[pid] = new ProcCacheEntry(pid);
    }

    auto entry = GetEntry(pid);

    if ((errCode = entry->UpdateIfPsChanged())) {
      eos_static_err("something wrong happened in reading proc stuff %d : %s", pid,
                     pCatalog[pid]->pErrMessage.c_str());
      delete pCatalog[pid];
      pCatalog.erase(pid);
      return errCode;
    }

    return 0;
  }

  //! returns true if the entry is removed after the call
  bool RemoveEntry(int pid)
  {
    if (!HasEntry(pid)) {
      return true;
    } else {
      delete pCatalog[pid];
      pCatalog.erase(pid);
      return true;
    }
  }

  //! returns true if the entry is removed after the call
  int RemoveEntries(const std::set<pid_t>* protect)
  {
    int count = 0;
    eos::common::RWMutexWriteLock lock(pMutex);

    for (auto it = pCatalog.begin(); it != pCatalog.end();) {
      if (protect && protect->count(it->first)) {
        ++it;
      } else {
        pCatalog.erase(it++);
        ++count;
      }
    }

    return count;
  }

  //! get the entry associated to the pid if it exists
  //! gets NULL if the the cache does not have such an entry
  ProcCacheEntry* GetEntry(int pid)
  {
    auto entry = pCatalog.find(pid);

    if (entry == pCatalog.end()) {
      return NULL;
    } else {
      return entry->second;
    }
  }

  bool HasBoundIdentity(int pid)
  {
    eos::common::RWMutexReadLock lock(pMutex);
    auto entry = pCatalog.find(pid);

    if (entry == pCatalog.end()) {
      return false;
    }

    return entry->second->HasBoundIdentity();
  }

  bool GetBoundIdentity(int pid, BoundIdentity& identity)
  {
    eos::common::RWMutexReadLock lock(pMutex);
    auto entry = pCatalog.find(pid);

    if (entry == pCatalog.end()) {
      return false;
    }

    return entry->second->GetBoundIdentity(identity);
  }

  bool GetStartupTime(int pid, Jiffies& sut)
  {
    eos::common::RWMutexReadLock lock(pMutex);
    auto entry = pCatalog.find(pid);

    if (entry == pCatalog.end()) {
      return false;
    }

    return entry->second->GetStartupTime(sut);
  }

  const std::vector<std::string>&
  GetArgsVec(int pid)
  {
    static std::vector<std::string> dummy;
    eos::common::RWMutexReadLock lock(pMutex);
    auto entry = pCatalog.find(pid);

    if (entry == pCatalog.end()) {
      return dummy;
    }

    return entry->second->GetArgsVec();
  }

  const std::string&
  GetArgsStr(int pid)
  {
    static std::string dummy;
    eos::common::RWMutexReadLock lock(pMutex);
    auto entry = pCatalog.find(pid);

    if (entry == pCatalog.end()) {
      return dummy;
    }

    return entry->second->GetArgsStr();
  }

  bool GetSid(int pid, pid_t& sid)
  {
    eos::common::RWMutexReadLock lock(pMutex);
    auto entry = pCatalog.find(pid);

    if (entry == pCatalog.end()) {
      return false;
    }

    return entry->second->GetSid(sid);
  }

  bool SetBoundIdentity(int pid, const BoundIdentity &identity)
  {
    eos::common::RWMutexWriteLock lock(pMutex);
    auto entry = pCatalog.find(pid);

    if (entry == pCatalog.end()) {
      return false;
    }

    return entry->second->SetBoundIdentity(identity);
  }
};

#ifndef __PROCCACHE__NOGPROCCACHE__
//extern ProcCache gProcCache;
extern std::vector<ProcCache> gProcCacheV;
extern int gProcCacheShardSize;
inline ProcCache& gProcCache(int i)
{
  return gProcCacheV[i % gProcCacheShardSize];
}
#endif

#endif
