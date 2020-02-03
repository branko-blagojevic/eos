// ----------------------------------------------------------------------
// File: TapeGc.cc
// Author: Steven Murray - CERN
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

#include "mgm/FsView.hh"
#include "mgm/proc/admin/StagerRmCmd.hh"
#include "mgm/tgc/Constants.hh"
#include "mgm/tgc/TapeGc.hh"
#include "mgm/tgc/SpaceNotFound.hh"
#include "mgm/tgc/Utils.hh"
#include "mgm/XrdMgmOfs.hh"
#include "namespace/interface/IFileMDSvc.hh"
#include "namespace/Prefetcher.hh"

#include <functional>
#include <ios>
#include <sstream>
#include <time.h>

EOSTGCNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
TapeGc::TapeGc():
  m_enabled(false),
  m_cachedDefaultSpaceMinFreeBytes(
    std::bind(getSpaceConfigMinNbFreeBytes, "default"), // Value getter
    10), // Maximum age of cached value in seconds
  m_freeSpaceInDefault("default", TAPEGC_DEFAULT_SPACE_QUERY_PERIOD_SECS),
  m_nbStagerrms(0)
{
}

//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
TapeGc::~TapeGc()
{
  try {
    // m_enabled is an std::atomic and is set within enable() after m_worker
    if(m_enabled && m_worker) {
      m_stop.setToTrue();
      m_worker->join();
    }
  } catch(std::exception &ex) {
    eos_static_err("msg=\"%s\"", ex.what());
  } catch(...) {
    eos_static_err("msg=\"Caught an unknown exception\"");
  }
}

//------------------------------------------------------------------------------
// Enable the GC
//------------------------------------------------------------------------------
void
TapeGc::enable() noexcept
{
  try {
    // Do nothing if the calling thread is not the first to call start()
    if (m_enabledMethodCalled.test_and_set()) return;

    m_enabled = true;

    std::function<void()> entryPoint = std::bind(&TapeGc::workerThreadEntryPoint, this);
    m_worker.reset(new std::thread(entryPoint));
  } catch(std::exception &ex) {
    eos_static_err("msg=\"%s\"", ex.what());
  } catch(...) {
    eos_static_err("msg=\"Caught an unknown exception\"");
  }
}

//------------------------------------------------------------------------------
// Entry point for the GC worker thread
//------------------------------------------------------------------------------
void
TapeGc::workerThreadEntryPoint() noexcept
{
  try {
    eos_static_info("msg=\"TapeGc worker thread started\"");
  } catch(...) {
  }

  do {
    while(!m_stop && tryToGarbageCollectASingleFile()) {
    };
  } while(!m_stop.waitForTrue(std::chrono::seconds(10)));
}

//------------------------------------------------------------------------------
// Notify GC the specified file has been opened
//------------------------------------------------------------------------------
void
TapeGc::fileOpened(const std::string &path, const IFileMD &fmd) noexcept
{
  if(!m_enabled) return;

  try {
    const auto fid = fmd.getId();
    const std::string preamble = createLogPreamble(path, fid);
    eos_static_debug(preamble.c_str());

    // Only consider files that have a CTA archive ID as only these can be
    // guaranteed to have been successfully closed, committed and intended for
    // tape storage
    if(!fmd.hasAttribute("CTA_ArchiveFileId")) return;

    std::lock_guard<std::mutex> lruQueueLock(m_lruQueueMutex);
    const bool exceededBefore = m_lruQueue.maxQueueSizeExceeded();
    m_lruQueue.fileAccessed(fid);

    // Only log crossing the max queue size threshold - don't log each access
    if(!exceededBefore && m_lruQueue.maxQueueSizeExceeded()) {
      eos_static_warning("%s msg=\"Tape aware max queue size has been passed - "
        "new files will be ignored\"", preamble.c_str());
    }
  } catch(std::exception &ex) {
    eos_static_err("msg=\"%s\"", ex.what());
  } catch(...) {
    eos_static_err("msg=\"Caught an unknown exception\"");
  }
}

//------------------------------------------------------------------------------
// Notify GC a replica of the specified file has been committed
//------------------------------------------------------------------------------
void
TapeGc::fileReplicaCommitted(const std::string &path, const IFileMD &fmd) noexcept
{
  if(!m_enabled) return;

  try {
    const auto fid = fmd.getId();
    const std::string preamble = createLogPreamble(path, fid);
    eos_static_debug(preamble.c_str());

    // Only consider files that have a CTA archive ID as only these can be
    // guaranteed to have been successfully closed, committed and intended for
    // tape storage
    if(!fmd.hasAttribute("CTA_ArchiveFileId")) return;

    std::lock_guard<std::mutex> lruQueueLock(m_lruQueueMutex);
    const bool exceededBefore = m_lruQueue.maxQueueSizeExceeded();
    m_lruQueue.fileAccessed(fid);

    // Only log crossing the max queue size threshold - don't log each access
    if(!exceededBefore && m_lruQueue.maxQueueSizeExceeded()) {
      eos_static_warning("%s msg=\"Tape aware max queue size has been passed - "
        "new files will be ignored\"", preamble.c_str());
    }
  } catch(std::exception &ex) {
    eos_static_err("msg=\"%s\"", ex.what());
  } catch(...) {
    eos_static_err("msg=\"Caught an unknown exception\"");
  }
}

//------------------------------------------------------------------------------
// Return the minimum number of free bytes the specified space should have
// as set in the configuration variables of the space.  If the minimum
// number of free bytes cannot be determined for whatever reason then 0 is
// returned.
//------------------------------------------------------------------------------
uint64_t
TapeGc::getSpaceConfigMinNbFreeBytes(const std::string &spaceName) noexcept
{
  try {
    std::string valueStr;
    {
      eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
      const auto spaceItor = FsView::gFsView.mSpaceView.find(spaceName);
      if (FsView::gFsView.mSpaceView.end() == spaceItor) return 0;
      if (nullptr == spaceItor->second) return 0;
      const auto &space = *(spaceItor->second);
      valueStr = space.GetConfigMember("tapeawaregc.minfreebytes");
    }

    if(valueStr.empty()) {
     return 0;
    } else {
      return Utils::toUint64(valueStr);
    }
  } catch(...) {
    return 0;
  }
}

//------------------------------------------------------------------------------
// Try to garage collect a single file if necessary and possible
//------------------------------------------------------------------------------
bool
TapeGc::tryToGarbageCollectASingleFile() noexcept
{
  try {
    uint64_t defaultSpaceMinFreeBytes = 0;

    try {
      bool defaultSpaceMinFreeBytesHasChanged = false;
      defaultSpaceMinFreeBytes = m_cachedDefaultSpaceMinFreeBytes.get(defaultSpaceMinFreeBytesHasChanged);
      if(defaultSpaceMinFreeBytesHasChanged) {
        std::ostringstream msg;
        msg << "msg=\"defaultSpaceMinFreeBytes has been changed to " << defaultSpaceMinFreeBytes << "\"";
        eos_static_info(msg.str().c_str());
      }
    } catch(SpaceNotFound &) {
      // Return no file was garbage collected if the space was not found
      return false;
    }

    try {
      // Return no file was garbage collected if there is still enough free space
      const auto actualDefaultSpaceNbFreeBytes = m_freeSpaceInDefault.getFreeBytes();
      if(actualDefaultSpaceNbFreeBytes >= defaultSpaceMinFreeBytes) return false;
    } catch(SpaceNotFound &) {
      // Return no file was garbage collected if the space was not found
      return false;
    }

    IFileMD::id_t fid;

    {
      std::lock_guard<std::mutex> lruQueueLock(m_lruQueueMutex);
      if (m_lruQueue.empty()) return false; // No file was garbage collected
      fid = m_lruQueue.getAndPopFidOfLeastUsedFile();
    }

    const uint64_t fileToBeDeletedSizeBytes = getFileSizeBytes(fid);
    const auto result = stagerrmAsRoot(fid);

    std::ostringstream preamble;
    preamble << "fxid=" << std::hex << fid;

    if(0 == result.retc()) {
      m_freeSpaceInDefault.fileQueuedForDeletion(fileToBeDeletedSizeBytes);
      std::ostringstream msg;
      msg << preamble.str() << " msg=\"Garbage collected file using stagerrm\"";
      eos_static_info(msg.str().c_str());

      m_nbStagerrms++;

      return true; // A file was garbage collected
    } else {
      if(fileInNamespaceAndNotScheduledForDeletion(fid)) {
        {
          std::ostringstream msg;
          msg << preamble.str() << " msg=\"Unable to stagerrm file at this time: "
            << result.std_err() << "\"";
          eos_static_info(msg.str().c_str());
        }
        {
          std::ostringstream msg;
          msg << preamble.str() << " msg=\"Putting file back in GC queue"
                                   " because it is still in the namespace\"";
          eos_static_info(msg.str().c_str());
        }

        std::lock_guard<std::mutex> lruQueueLock(m_lruQueueMutex);
        m_lruQueue.fileAccessed(fid);
      } else {
        // Please note that a file is considered successfully garbage collected
        // if it does not exists in the EOS namespace when it is popped from the
        // LRU data structure.
        return true;
      }
    }
  } catch(std::exception &ex) {
    eos_static_err("msg=\"%s\"", ex.what());
  } catch(...) {
    eos_static_err("msg=\"Caught an unknown exception\"");
  }

  return false; // No file was garbage collected
}

//----------------------------------------------------------------------------
// Determine if the specified file exists and is not scheduled for deletion
//----------------------------------------------------------------------------
bool TapeGc::fileInNamespaceAndNotScheduledForDeletion(const IFileMD::id_t fid) {
  // Prefetch before taking lock because metadata may not be in memory
  Prefetcher::prefetchFileMDAndWait(gOFS->eosView, fid);
  common::RWMutexReadLock lock(gOFS->eosViewRWMutex);
  const auto fmd = gOFS->eosFileService->getFileMD(fid);

  // A file scheduled for deletion has a container ID of 0
  return nullptr != fmd && 0 != fmd->getContainerId();
}

//----------------------------------------------------------------------------
// Return size of the specified file
//----------------------------------------------------------------------------
uint64_t TapeGc::getFileSizeBytes(const IFileMD::id_t fid) {
  // Prefetch before taking lock because metadata may not be in memory
  Prefetcher::prefetchFileMDAndWait(gOFS->eosView, fid);
  common::RWMutexReadLock lock(gOFS->eosViewRWMutex);
  const auto fmd = gOFS->eosFileService->getFileMD(fid);

  if(nullptr != fmd) {
    return fmd->getSize();
  } else {
    return 0;
  }
}

//----------------------------------------------------------------------------
// Execute stagerrm as user root
//----------------------------------------------------------------------------
console::ReplyProto
TapeGc::stagerrmAsRoot(const IFileMD::id_t fid)
{
  eos::common::VirtualIdentity rootVid = eos::common::VirtualIdentity::Root();

  eos::console::RequestProto req;
  eos::console::StagerRmProto* stagerRm = req.mutable_stagerrm();
  auto file = stagerRm->add_file();
  file->set_fid(fid);

  StagerRmCmd cmd(std::move(req), rootVid);
  return cmd.ProcessRequest();
}

//----------------------------------------------------------------------------
// Return the preamble to be placed at the beginning of every log message
//----------------------------------------------------------------------------
std::string
TapeGc::createLogPreamble(const std::string &path, const IFileMD::id_t fid)
{
  std::ostringstream preamble;

  preamble << "fxid=" << std::hex << fid << " path=\"" << path << "\"";

  return preamble.str();
}

//----------------------------------------------------------------------------
// Return the number of files successfully stagerrm'ed since boot
//----------------------------------------------------------------------------
uint64_t
TapeGc::getNbStagerrms() const
{
  return m_nbStagerrms;
}

//----------------------------------------------------------------------------
// Return the size of the LRUE queue
//----------------------------------------------------------------------------
Lru::FidQueue::size_type
TapeGc::getLruQueueSize()
{
  std::lock_guard<std::mutex> lruQueueLock(m_lruQueueMutex);
  return m_lruQueue.size();
}

//----------------------------------------------------------------------------
// Return the amount of free bytes in the EOS space named default
//----------------------------------------------------------------------------
uint64_t
TapeGc::getDefaultSpaceFreeBytes() {
  return m_freeSpaceInDefault.getFreeBytes();
}

//----------------------------------------------------------------------------
// Return the amount of free bytes in the EOS space named default
//----------------------------------------------------------------------------
time_t
TapeGc::getDefaultSpaceFreeSpaceQueryTimestamp() {
  return m_freeSpaceInDefault.getFreeSpaceQueryTimestamp();
}

EOSTGCNAMESPACE_END
