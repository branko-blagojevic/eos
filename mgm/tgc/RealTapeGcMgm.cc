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

#include "mgm/proc/admin/StagerRmCmd.hh"
#include "mgm/FsView.hh"
#include "mgm/Policy.hh"
#include "mgm/tgc/Constants.hh"
#include "mgm/tgc/RealTapeGcMgm.hh"
#include "mgm/tgc/SpaceNotFound.hh"
#include "mgm/tgc/Utils.hh"
#include "namespace/interface/IFileMDSvc.hh"
#include "namespace/ns_quarkdb/inspector/FileScanner.hh"
#include "namespace/ns_quarkdb/qclient/include/qclient/QClient.hh"
#include "namespace/Prefetcher.hh"

#include <sstream>
#include <stdexcept>

EOSTGCNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
RealTapeGcMgm::RealTapeGcMgm(XrdMgmOfs &ofs): m_ofs(ofs) {
}

//----------------------------------------------------------------------------
// Return the configuration of a tape-aware garbage collector
//----------------------------------------------------------------------------
SpaceConfig
RealTapeGcMgm::getTapeGcSpaceConfig(const std::string &spaceName) {
  SpaceConfig config;

  config.queryPeriodSecs = getSpaceConfigMemberUint64(spaceName, TGC_NAME_QRY_PERIOD_SECS, TGC_DEFAULT_QRY_PERIOD_SECS);
  config.availBytes = getSpaceConfigMemberUint64(spaceName, TGC_NAME_AVAIL_BYTES, TGC_DEFAULT_AVAIL_BYTES);
  config.totalBytes = getSpaceConfigMemberUint64(spaceName, TGC_NAME_TOTAL_BYTES, TGC_DEFAULT_TOTAL_BYTES);
  return config;
}

//----------------------------------------------------------------------------
// Return the value of the specified space configuration variable
//----------------------------------------------------------------------------
std::uint64_t
RealTapeGcMgm::getSpaceConfigMemberUint64(
  const std::string &spaceName,
  const std::string &memberName,
  const std::uint64_t defaultValue) noexcept
{
  try {
    std::string valueStr;
    {
      eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
      const auto spaceItor = FsView::gFsView.mSpaceView.find(spaceName);
      if (FsView::gFsView.mSpaceView.end() == spaceItor) throw std::exception();
      if (nullptr == spaceItor->second) throw std::exception();
      const auto &space = *(spaceItor->second);
      valueStr = space.GetConfigMember(memberName);
    }

    if(valueStr.empty()) {
      throw std::exception();
    } else {
      return Utils::toUint64(valueStr);
    }
  } catch(...) {
    return defaultValue;
  }
}

//----------------------------------------------------------------------------
// Determine if the specified file exists and is not scheduled for deletion
//----------------------------------------------------------------------------
bool RealTapeGcMgm::fileInNamespaceAndNotScheduledForDeletion(const IFileMD::id_t fid) {
  // Prefetch before taking lock because metadata may not be in memory
  Prefetcher::prefetchFileMDAndWait(m_ofs.eosView, fid);
  common::RWMutexReadLock lock(m_ofs.eosViewRWMutex);
  const auto fmd = m_ofs.eosFileService->getFileMD(fid);

  // A file scheduled for deletion has a container ID of 0
  return nullptr != fmd && 0 != fmd->getContainerId();
}

//----------------------------------------------------------------------------
// Return statistics about the specified EOS space
//----------------------------------------------------------------------------
SpaceStats
RealTapeGcMgm::getSpaceStats(const std::string &space) const
{
  SpaceStats stats;

  // Policy::GetSpacePolicyLayout() implicitly takes a lock on FsView::gFsView.ViewMutex
  const auto layoutId = Policy::GetSpacePolicyLayout(space.c_str());

  {
    eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
    const auto spaceItor = FsView::gFsView.mSpaceView.find(space);

    if(FsView::gFsView.mSpaceView.end() == spaceItor) {
      throw SpaceNotFound(std::string(__FUNCTION__) + ": Cannot find space " +
                          space + ": FsView does not know the space name");
    }

    if(nullptr == spaceItor->second) {
      throw SpaceNotFound(std::string(__FUNCTION__) + ": Cannot find space " +
                          space + ": Pointer to FsSpace is nullptr");
    }

    const FsSpace &fsSpace = *(spaceItor->second);

    for(const auto fsid: fsSpace) {
      FileSystem * const fs = FsView::gFsView.mIdView.lookupByID(fsid);

      // Skip this file system if it cannot be found
      if(nullptr == fs) {
        std::ostringstream msg;
        msg << "Unable to find file system: space=" << space << " fsid=" << fsid;
        eos_static_warning(msg.str().c_str());
        continue;
      }

      const common::BootStatus fsStatus = FileSystem::GetStatusFromString(fs->GetString("stat.boot").c_str());
      const common::ActiveStatus fsActiveStatus =
        FileSystem::GetActiveStatusFromString(fs->GetString("stat.active").c_str());
      const common::ConfigStatus fsConfigStatus =
        FileSystem::GetConfigStatusFromString(fs->GetString("configstatus").c_str());

      // Only consider file systems that are booted, on-line and read/write
      if(common::BootStatus::kBooted == fsStatus &&
         common::ActiveStatus::kOnline == fsActiveStatus &&
         common::ConfigStatus::kRW == fsConfigStatus) {
        const uint64_t diskBsize = (std::uint64_t)fs->GetLongLong("stat.statfs.bsize");
        const uint64_t diskBlocks = (std::uint64_t)fs->GetLongLong("stat.statfs.blocks");
        const uint64_t diskBavail = (std::uint64_t)fs->GetLongLong("stat.statfs.bavail");

        const auto headroomBytes = fs->GetLongLong("headroom");
        const uint64_t bytesBeforeSubHeadroom = diskBlocks * diskBsize;
        const uint64_t availBytesBeforeSubHeadroom = diskBavail * diskBsize;
        if (0 > headroomBytes) {
          // Ignore headroom if it is negative
          stats.totalBytes += bytesBeforeSubHeadroom;
          stats.availBytes += availBytesBeforeSubHeadroom;
        } else {
          // Avoid going negative when subtracting headroom
          if (bytesBeforeSubHeadroom > (uint64_t)headroomBytes) {
            stats.totalBytes += bytesBeforeSubHeadroom - (uint64_t)headroomBytes;
          }
          if (availBytesBeforeSubHeadroom > (uint64_t )headroomBytes) {
            stats.availBytes += availBytesBeforeSubHeadroom - (uint64_t)headroomBytes;
          }
        }
      }
    }
  }

  // Scale stats if there is a space policy layout
  if (0 != layoutId) {
    const float scaleFactor = eos::common::LayoutId::GetSizeFactor(layoutId);
    if (1.0 < scaleFactor) {
      stats.totalBytes /= scaleFactor;
      stats.availBytes /= scaleFactor;
    }
  }

  return stats;
}

//----------------------------------------------------------------------------
// Return the size of the specified file
//----------------------------------------------------------------------------
std::uint64_t RealTapeGcMgm::getFileSizeBytes(const IFileMD::id_t fid) {
  try {
    // Prefetch before taking lock because metadata may not be in memory
    Prefetcher::prefetchFileMDAndWait(m_ofs.eosView, fid);
  } catch(std::exception &ex) {
    std::ostringstream msg;
    msg << __FUNCTION__ << ": fid=" << fid << ": prefetchFileMDAndWait() failed: " << ex.what();
    throw FailedToGetFileSize(msg.str());
  } catch(...) {
    std::ostringstream msg;
    msg << __FUNCTION__ << ": fid=" << fid << ": prefetchFileMDAndWait() failed: Unknown exception";
    throw FailedToGetFileSize(msg.str());
  }

  common::RWMutexReadLock lock(m_ofs.eosViewRWMutex);

  std::shared_ptr<eos::IFileMD> fmd;
  try {
    fmd = m_ofs.eosFileService->getFileMD(fid);
  } catch(std::exception &ex) {
    std::ostringstream msg;
    msg << __FUNCTION__ << ": fid=" << fid << ": getFileMD() failed: " << ex.what();
    throw FailedToGetFileSize(msg.str());
  } catch(...) {
    std::ostringstream msg;
    msg << __FUNCTION__ << ": fid=" << fid << ": getFileMD() failed: Unknown exception";
    throw FailedToGetFileSize(msg.str());
  }

  if(nullptr == fmd) {
    std::ostringstream msg;
    msg << __FUNCTION__ << ": fid=" << fid << ": getFileMD() returned nullptr";
    throw FailedToGetFileSize(msg.str());
  }

  std::uint64_t fileSizeBytes = 0;
  try {
    fileSizeBytes = fmd->getSize();
  } catch(std::exception &ex) {
    std::ostringstream msg;
    msg << __FUNCTION__ << ": fid=" << fid << ": getSize() failed: " << ex.what();
    throw FailedToGetFileSize(msg.str());
  } catch(...) {
    std::ostringstream msg;
    msg << __FUNCTION__ << ": fid=" << fid << ": getSize() failed: Unknown exception";
    throw FailedToGetFileSize(msg.str());
  }

  IContainerMD::id_t containerId = 0;
  try {
    containerId = fmd->getContainerId();
  } catch(std::exception &ex) {
    std::ostringstream msg;
    msg << __FUNCTION__ << ": fid=" << fid << ": getContainerId() failed: " << ex.what();
    throw FailedToGetFileSize(msg.str());
  } catch(...) {
    std::ostringstream msg;
    msg << __FUNCTION__ << ": fid=" << fid << ": getContainerId() failed: Unknown exception";
    throw FailedToGetFileSize(msg.str());
  }

  // A file scheduled for deletion has a container ID of 0
  if(0 == containerId) {
    std::ostringstream msg;
    msg << __FUNCTION__ << ": fid=" << fid << ": File has been scheduled for deletion";
    throw FailedToGetFileSize(msg.str());
  }

  return fileSizeBytes;
}

//----------------------------------------------------------------------------
// Execute stagerrm as user root
//----------------------------------------------------------------------------
void
RealTapeGcMgm::stagerrmAsRoot(const IFileMD::id_t fid)
{
  eos::common::VirtualIdentity rootVid = eos::common::VirtualIdentity::Root();

  eos::console::RequestProto req;
  eos::console::StagerRmProto* stagerRm = req.mutable_stagerrm();
  auto file = stagerRm->add_file();
  file->set_fid(fid);

  StagerRmCmd cmd(std::move(req), rootVid);
  auto const result = cmd.ProcessRequest();
  if(result.retc()) {
    throw std::runtime_error(result.std_err());
  }
}

//----------------------------------------------------------------------------
// Return map from file system ID to EOS space name
//----------------------------------------------------------------------------
std::map<common::FileSystem::fsid_t, std::string>
RealTapeGcMgm::getFsIdToSpaceMap()
{
  std::map<common::FileSystem::fsid_t, std::string> fsIdToSpace;

  eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);

  const auto spaces = getSpaces();

  for (const auto &space: spaces) {
    const auto spaceItor = FsView::gFsView.mSpaceView.find(space);

    if (FsView::gFsView.mSpaceView.end() == spaceItor) {
      throw SpaceNotFound(std::string(__FUNCTION__) + ": Cannot find space " +
                          space + ": FsView does not know the space name");
    }

    if (nullptr == spaceItor->second) {
      throw SpaceNotFound(std::string(__FUNCTION__) + ": Cannot find space " +
                          space + ": Pointer to FsSpace is nullptr");
    }

    const FsSpace &fsSpace = *(spaceItor->second);

    for (const auto fsId: fsSpace) {
      const auto itor = fsIdToSpace.find(fsId);
      if (fsIdToSpace.end() != itor) {
        std::ostringstream msg;
        msg << __FUNCTION__ << " failed: Found a filesystem in more than one EOS space: fsId=" << fsId <<
          " firstSpace=" << itor->second << " secondSpace=" << space;
        throw std::runtime_error(msg.str());
      }
      fsIdToSpace[fsId] = space;
    }
  }

  return fsIdToSpace;
}

//----------------------------------------------------------------------------
// Return a list of the names of all the EOS spaces
//----------------------------------------------------------------------------
std::set<std::string>
RealTapeGcMgm::getSpaces() const
{
  std::set<std::string> spaces;

  eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);

  for (const auto nameAndSpace : FsView::gFsView.mSpaceView) {
    if (0 != spaces.count(nameAndSpace.first)) {
      std::ostringstream msg;
      msg << __FUNCTION__ << " failed: Detected two EOS spaces with the same name: space=" <<
        nameAndSpace.first;
      throw std::runtime_error(msg.str());
    }
    spaces.insert(nameAndSpace.first);
  }

  return spaces;
}

//----------------------------------------------------------------------------
// Return map from EOS space name to disk replicas within that space
//----------------------------------------------------------------------------
std::map<std::string, std::set<ITapeGcMgm::FileIdAndCtime> >
RealTapeGcMgm::getSpaceToDiskReplicasMap(const std::set<std::string> &spacesToMap, std::atomic<bool> &stop,
  uint64_t &nbFilesScanned)
{
  nbFilesScanned = 0;

  if (m_ofs.mQdbContactDetails.members.empty()) {
    std::ostringstream msg;
    msg << __FUNCTION__ << " failed: QdbContactDetails.members is empty";
    eos_static_warning(msg.str().c_str());
    throw std::runtime_error(msg.str());
  }

  std::map<std::string, std::set<FileIdAndCtime> > spaceToReplicas;
  const auto fsIdToSpace = getFsIdToSpaceMap();
  qclient::QClient qdbClient(m_ofs.mQdbContactDetails.members, m_ofs.mQdbContactDetails.constructOptions());
  FileScanner fileScanner(qdbClient);

  std::set<int> fsIdsWithNoSpace;

  while (fileScanner.valid()) {
    eos::ns::FileMdProto file;

    if (stop) {
      eos_static_info("The creation of the EOS space name to files map has been requested to stop");
      break;
    }

    if (!fileScanner.getItem(file)) {
      eos_static_warning("msg=\"fileScanner stopped iterating early\"");
      break;
    }

    const auto ctime = Utils::bufToTimespec(file.ctime());
    const int locationsSize = file.locations_size();
    for (int locationIndex = 0; locationIndex < locationsSize; locationIndex++) {
      const int fsId = file.locations(locationIndex);
      const auto itor = fsIdToSpace.find(fsId);
      if (fsIdToSpace.end() == itor) {
        fsIdsWithNoSpace.insert(fsId);
      } else {
        const std::string &space = itor->second;
        if (spacesToMap.count(space)) {
          spaceToReplicas[space].emplace(file.id(), ctime);
        }
      }
    }

    nbFilesScanned++;
    fileScanner.next();
  }

  if (!fsIdsWithNoSpace.empty()) {
    std::ostringstream msg;
    msg << "msg=\"Found file system IDs with no EOS space\" fsIds=\"";
    bool isFirstFsId = true;
    for(const auto fsId: fsIdsWithNoSpace) {
      if (isFirstFsId) {
        isFirstFsId = false;
      } else {
        msg << ",";
      }
      msg << fsId;
    }
    eos_static_warning(msg.str().c_str());
  }

  return spaceToReplicas;
}

EOSTGCNAMESPACE_END
