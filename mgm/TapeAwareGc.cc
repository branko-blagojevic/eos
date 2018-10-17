// ----------------------------------------------------------------------
// File: TapeAwareGc.cc
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
#include "mgm/TapeAwareGc.hh"
#include "mgm/XrdMgmOfs.hh"
#include "namespace/interface/IFileMDSvc.hh"
#include "namespace/Prefetcher.hh"

#include <ios>
#include <sstream>

EOSMGMNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Return the single instance of this class
//------------------------------------------------------------------------------
TapeAwareGc &TapeAwareGc::instance() {
  static TapeAwareGc s_instance;
  return s_instance;
}

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
TapeAwareGc::TapeAwareGc(): m_enabled(false), m_defaultMinFreeBytes(0)
{
}

//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
TapeAwareGc::~TapeAwareGc()
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
void TapeAwareGc::enable(const uint64_t defaultMinFreeBytes) noexcept
{
  try {
    // Do nothing if the calling thread is not the first to call start()
    if (m_enabledMethodCalled.test_and_set()) return;

    m_defaultMinFreeBytes = defaultMinFreeBytes;
    m_enabled = true;

    std::function<void()> entryPoint =
      std::bind(&TapeAwareGc::workerThreadEntryPoint, this);
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
void TapeAwareGc::workerThreadEntryPoint() noexcept
{
  do {
    while(!m_stop && garbageCollect()) {};
  } while(!m_stop.waitForTrue(std::chrono::seconds(10)));
}

//------------------------------------------------------------------------------
// Notify GC the specified file has been opened
//------------------------------------------------------------------------------
void TapeAwareGc::fileOpened(const std::string &path, const IFileMD &fmd)
  noexcept
{
  if(!m_enabled) return;

  try {
    // Only consider files that have a CTA archive ID as only these can be
    // guaranteed to have been successfully closed, committed and intended for
    // tape storage
    if(!fmd.hasAttribute("CTA_ArchiveFileId")) return;

    const auto fid = fmd.getIdentifier();
    const std::string preamble = createLogPreamble(path, fid);
    eos_static_info(preamble.c_str());

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
void TapeAwareGc::fileReplicaCommitted(const std::string &path,
  const IFileMD &fmd) noexcept
{
  if(!m_enabled) return;

  try {
    const auto fid = fmd.getIdentifier();
    const std::string preamble = createLogPreamble(path, fid);
    eos_static_info(preamble.c_str());

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
// Return number of free bytes in the specified space
//------------------------------------------------------------------------------
uint64_t TapeAwareGc::getSpaceNbFreeBytes(const std::string &name) {
  eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
  const auto defaultSpace = FsView::gFsView.mSpaceView.find("default");

  if(FsView::gFsView.mSpaceView.end() == defaultSpace) {
    throw SpaceNotFound(std::string("Cannot find space ") + name);
  }

  return defaultSpace->second->SumLongLong("stat.statfs.freebytes", false);
}

//------------------------------------------------------------------------------
// Garage collect
//------------------------------------------------------------------------------
bool TapeAwareGc::garbageCollect() noexcept {
  try {
    // Return no file was garbage collected if there is still enough free space
    if(getSpaceNbFreeBytes("default") >= m_defaultMinFreeBytes) return false;

    FileIdentifier fid;

    {
      std::lock_guard<std::mutex> lruQueueLock(m_lruQueueMutex);
      if (m_lruQueue.empty()) return false; // No file was garbage collected
      fid = m_lruQueue.getAndPopFidOfLeastUsedFile();
    }

    const auto result = stagerrmAsRoot(fid);

    std::ostringstream preamble;
    preamble << "fxid=" << std::hex << fid.getUnderlyingUInt64();

    if(0 == result.retc()) {
      std::ostringstream msg;
      msg << preamble.str() << " msg=\"Garbage collected file using stagerrm\"";
      eos_static_info(msg.str().c_str());
      return true; // A file was garbage collected
    } else {
      {
        std::ostringstream msg;
        msg << preamble.str() << " msg=\"Unable to stagerrm file at this time: "
          << result.std_err() << "\"";
        eos_static_info(msg.str().c_str());
      }

      // Prefetch before taking lock because metadata may not be in memory
      Prefetcher::prefetchFileMDAndWait(gOFS->eosView, fid.getUnderlyingUInt64());
      common::RWMutexReadLock lock(gOFS->eosViewRWMutex);
      const auto fmd = gOFS->eosFileService->getFileMD(fid.getUnderlyingUInt64());

      if(nullptr != fmd && 0 != fmd->getContainerId()) {
        std::ostringstream msg;
        msg << preamble.str() << " msg=\"Putting file back in GC queue"
          " because it is still in the namespace\"";
        eos_static_info(msg.str().c_str());

        std::lock_guard<std::mutex> lruQueueLock(m_lruQueueMutex);
        m_lruQueue.fileAccessed(fid);
      } else {
        std::ostringstream msg;
        msg << preamble.str() << " msg=\"Not returning file to GC queue"
          " because it is not in the namespace\"";
        eos_static_info(msg.str().c_str());
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
// Execute stagerrm as user root
//----------------------------------------------------------------------------
console::ReplyProto TapeAwareGc::stagerrmAsRoot(const FileIdentifier fid) {
  eos::common::Mapping::VirtualIdentity rootVid;
  eos::common::Mapping::Root(rootVid);

  eos::console::RequestProto req;
  eos::console::StagerRmProto* stagerRm = req.mutable_stagerrm();
  auto file = stagerRm->add_file();
  file->set_fid(fid.getUnderlyingUInt64());

  StagerRmCmd cmd(std::move(req), rootVid);
  return cmd.ProcessRequest();
}

//----------------------------------------------------------------------------
// Return the preamble to be placed at the beginning of every log message
//----------------------------------------------------------------------------
std::string TapeAwareGc::createLogPreamble(const std::string &path,
  const FileIdentifier fid) {
  std::ostringstream preamble;

  preamble << "fxid=" << std::hex << fid.getUnderlyingUInt64() <<
    " path=\"" << path << "\"";

  return preamble.str();
}

EOSMGMNAMESPACE_END
