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
//! @brief Master interface
//------------------------------------------------------------------------------
#pragma once
#include "mgm/Namespace.hh"
#include "common/Logging.hh"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

//------------------------------------------------------------------------------
//! @note: the defines after have to be in agreements with the defins in
//! XrdMqOfs.cc but we don't want to create a link in the code between the two
//------------------------------------------------------------------------------
//! Existance indicates that this node is to be treated as a master
#define EOSMGMMASTER_SUBSYS_RW_LOCKFILE "/var/eos/eos.mgm.rw"
//! Existance indicates that the local MQ should redirect to the remote MQ
#define EOSMQMASTER_SUBSYS_REMOTE_LOCKFILE "/var/eos/eos.mq.remote.up"

EOSMGMNAMESPACE_BEGIN

//------------------------------------------------------------------------------
//! Class IMaster
//------------------------------------------------------------------------------
class IMaster: public eos::common::LogId
{
public:
  //----------------------------------------------------------------------------
  //! Transition types
  //----------------------------------------------------------------------------
  struct Transition {
    enum Type {
      kMasterToMaster               = 0,
      kSlaveToMaster                = 1,
      kMasterToMasterRO             = 2,
      kMasterROToSlave              = 3,
      kSecondarySlaveMasterFailover = 4
    };
  };

  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  IMaster(): mLog("") {};

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  virtual ~IMaster() = default;

  //----------------------------------------------------------------------------
  //! Init method to determine the current master/slave state
  //!
  //! @return true if successful, otherwise false
  //----------------------------------------------------------------------------
  virtual bool Init() = 0;

  //----------------------------------------------------------------------------
  //! Boot namespace
  //!
  //1 @return true if successful, otherwise false
  //----------------------------------------------------------------------------
  virtual bool BootNamespace() = 0;

  //----------------------------------------------------------------------------
  //! Apply configuration setting
  //!
  //! @param stdOut output string
  //! @param stdErr output error string
  //! @param transition_type transition type
  //!
  //! @return true if successful, otherwise false
  //----------------------------------------------------------------------------
  virtual bool ApplyMasterConfig(std::string& stdOut, std::string& stdErr,
                                 Transition::Type transitiontype) = 0;

  //----------------------------------------------------------------------------
  //! Check if we are the master host
  //!
  //! @return true if master, otherwise false
  //----------------------------------------------------------------------------
  virtual bool IsMaster() = 0;

  //----------------------------------------------------------------------------
  //! Check if remove master is OK
  //!
  //! @return true if OK, otherwise false
  //----------------------------------------------------------------------------
  virtual bool IsRemoteMasterOk() const = 0;

  //----------------------------------------------------------------------------
  //! Get current master identifier ie. hostname:port
  //----------------------------------------------------------------------------
  virtual const std::string GetMasterId() const = 0;

  //----------------------------------------------------------------------------
  //! Set the new master hostname
  //!
  //! @param hostname new master hostname
  //! @param port new master port, default 1094
  //! @param err_msg error message
  //!
  //! @return true if successful, otherwise false
  //----------------------------------------------------------------------------
  virtual bool SetMasterId(const std::string& hostname, int port,
                           std::string& err_msg) = 0;

  //----------------------------------------------------------------------------
  //! Return a delay time for balancing & draining since after a transition
  //! we don't know the maps of already scheduled ID's and we have to make
  //! sure not to reissue a transfer too early!
  //----------------------------------------------------------------------------
  virtual size_t GetServiceDelay() = 0;

  //----------------------------------------------------------------------------
  //! Get master log
  //----------------------------------------------------------------------------
  virtual void GetLog(std::string& stdOut) = 0;

  //----------------------------------------------------------------------------
  //! Reset master log
  //----------------------------------------------------------------------------
  inline void ResetLog()
  {
    mLog.clear();
  }

  //----------------------------------------------------------------------------
  //! Add to master Log
  //----------------------------------------------------------------------------
  inline void MasterLog(const char* log)
  {
    if (log && strlen(log)) {
      mLog += log;
      mLog += '\n';
    }
  }

  //------------------------------------------------------------------------------
  //! Create status file
  //!
  //! @param path path to file to be created if it doesn't exist already
  //------------------------------------------------------------------------------
  bool CreateStatusFile(const char* path)
  {
    struct stat buf;

    if (::stat(path, &buf)) {
      int fd = 0;

      if ((fd = ::creat(path, S_IRWXU | S_IRGRP | S_IROTH)) == -1) {
        MasterLog(eos_static_err("msg=\"failed to create %s\" errno=%d", path,
                                 errno));
        return false;
      }

      close(fd);
    }

    return true;
  }

  //------------------------------------------------------------------------------
  //! Remove status file
  //!
  //! @param path path to file to be unlinked
  //------------------------------------------------------------------------------
  bool RemoveStatusFile(const char* path)
  {
    struct stat buf;

    if (!::stat(path, &buf)) {
      if (::unlink(path)) {
        MasterLog(eos_static_err("msg=\"failed to unlink %s\" errno=%d", path,
                                 errno));
        return false;
      }
    }

    return true;
  }

  //----------------------------------------------------------------------------
  //! Show the current master/slave run configuration (used by ns stat)
  //!
  //! @return string describing the status
  //----------------------------------------------------------------------------
  virtual std::string PrintOut() = 0;

protected:
  std::string mLog; ///< Master logs
};

EOSMGMNAMESPACE_END
