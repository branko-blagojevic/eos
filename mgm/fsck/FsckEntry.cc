//------------------------------------------------------------------------------
//! @file FsckEntry.hh
//! @author Elvin Sindrilaru - CERN
//------------------------------------------------------------------------------

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

#include "mgm/fsck/FsckEntry.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/FsView.hh"
#include "namespace/interface/IView.hh"
#include "namespace/interface/IFileMDSvc.hh"
#include "common/StringConversion.hh"
#include "common/LayoutId.hh"
#include "namespace/Prefetcher.hh"
#include "namespace/ns_quarkdb/persistency/MetadataFetcher.hh"

using eos::common::StringConversion;
using eos::common::LayoutId;

EOSMGMNAMESPACE_BEGIN

//----------------------------------------------------------------------------
//! Constructor
//----------------------------------------------------------------------------
FsckEntry::FsckEntry(eos::IFileMD::id_t fid,
                     eos::common::FileSystem::fsid_t fsid_err,
                     const std::string& expected_err):
  mFid(fid), mFsidErr(fsid_err),
  mReportedErr(ConvertToFsckErr(expected_err)), mRepairFactory()
{
  mMapRepairOps = {
    {FsckErr::MgmXsDiff, {&FsckEntry::RepairMgmXsSzDiff}},
    {FsckErr::MgmSzDiff, {&FsckEntry::RepairMgmXsSzDiff}},
    {FsckErr::FstXsDiff, {&FsckEntry::RepairFstXsSzDiff}},
    {FsckErr::FstSzDiff, {&FsckEntry::RepairFstXsSzDiff}},
    {FsckErr::UnregRepl, {&FsckEntry::RepairReplicaInconsistencies}},
    {FsckErr::DiffRepl,  {&FsckEntry::RepairReplicaInconsistencies}},
    {FsckErr::MissRepl,  {&FsckEntry::RepairReplicaInconsistencies}}
  };
  mRepairFactory = [](eos::common::FileId::fileid_t fid,
                      eos::common::FileSystem::fsid_t fsid_src,
                      eos::common::FileSystem::fsid_t fsid_trg ,
                      std::set<eos::common::FileSystem::fsid_t> exclude_srcs,
                      std::set<eos::common::FileSystem::fsid_t> exclude_dsts,
                      bool drop_src,
  const std::string & app_tag) {
    return std::make_shared<FsckRepairJob>(fid, fsid_src, fsid_trg,
                                           exclude_srcs, exclude_dsts,
                                           drop_src, app_tag);
  };
}

//------------------------------------------------------------------------------
// Collect MGM file metadata information
//------------------------------------------------------------------------------
void
FsckEntry::CollectMgmInfo(qclient::QClient& qcl)
{
  mMgmFmd = eos::MetadataFetcher::getFileFromId(qcl, FileIdentifier(mFid)).get();
}

//------------------------------------------------------------------------------
// Collect FST file metadata information from all replicas
//------------------------------------------------------------------------------
void
FsckEntry::CollectAllFstInfo()
{
  for (const auto fsid : mMgmFmd.locations()) {
    CollectFstInfo(fsid);
  }
}

//------------------------------------------------------------------------------
// Method to repair an mgm checksum difference error
//------------------------------------------------------------------------------
bool
FsckEntry::RepairMgmXsSzDiff()
{
  // This only makes sense for replica layouts
  if (LayoutId::IsRain(mMgmFmd.layout_id())) {
    return true;
  }

  std::string mgm_xs_val =
    StringConversion::BinData2HexString(mMgmFmd.checksum().c_str(),
                                        SHA_DIGEST_LENGTH,
                                        LayoutId::GetChecksumLen(mMgmFmd.layout_id()));
  // Make sure the disk xs and size values match between all the replicas
  uint64_t sz_val {0ull};
  std::string xs_val;
  bool mgm_xs_sz_match = false; // one of the disk xs matches the mgm one
  bool disk_xs_sz_match = true; // flag to mark that all disk xs match

  for (auto it = mFstFileInfo.cbegin(); it != mFstFileInfo.cend(); ++it) {
    auto& finfo = it->second;

    if (finfo->mFstErr != FstErr::None) {
      eos_err("msg=\"unavailable replica info\" fid=%08llx fsid=%lu",
              mFid, it->first);
      disk_xs_sz_match = true;
      break;
    }

    if (xs_val.empty() && (sz_val == 0ull)) {
      xs_val = finfo->mFstFmd.diskchecksum();
      sz_val = finfo->mFstFmd.size();

      if ((mgm_xs_val == xs_val) && (mMgmFmd.size() == sz_val)) {
        mgm_xs_sz_match = true;
        break;
      }
    } else {
      uint64_t current_sz_val = finfo->mFstFmd.size();
      std::string current_xs_val = finfo->mFstFmd.diskchecksum();

      if ((mgm_xs_val == current_xs_val) && (mMgmFmd.size() == current_sz_val)) {
        mgm_xs_sz_match = true;
        break;
      }

      if ((xs_val != current_xs_val) || (sz_val != current_sz_val)) {
        // There is a xs/size diff between two replicas, we can not fix this case
        disk_xs_sz_match = false;
        break;
      }
    }
  }

  if (mgm_xs_sz_match) {
    eos_warning("msg=\"mgm xs/size repair skip - found replica with matching "
                "xs and size\" fid=%08llx", mFid);
    return false;
  }

  if (disk_xs_sz_match) {
    size_t out_sz;
    auto xs_binary = StringConversion::Hex2BinDataChar(xs_val, out_sz);
    eos::Buffer xs_buff;
    xs_buff.putData(xs_binary.get(), SHA_DIGEST_LENGTH);

    if (gOFS) {
      try {
        // Grab the file metadata object and update it
        eos::Prefetcher::prefetchFileMDAndWait(gOFS->eosView, mFid);
        eos::common::RWMutexReadLock ns_rd_lock(gOFS->eosViewRWMutex);
        auto fmd = gOFS->eosFileService->getFileMD(mFid);
        fmd->setChecksum(xs_buff);
        fmd->setSize(sz_val);
        gOFS->eosView->updateFileStore(fmd.get());
      } catch (const eos::MDException& e) {
        eos_err("msg=\"mgm xs/size repair failed - no such filemd\" fid=%08llx", mFid);
        return false;
      }
    } else {
      // For testing we just update the MGM fmd object
      mMgmFmd.set_checksum(xs_buff.getDataPtr(), xs_buff.getSize());
      mMgmFmd.set_size(sz_val);
    }

    eos_info("msg=\"mgm xs/size repair successful\" fid=%08llx old_mgm_xs=\"%s\" "
             "new_mgm_xs=\"%s\"", mFid, mgm_xs_val.c_str(), xs_val.c_str());
  } else {
    eos_err("msg=\"mgm xs/size repair failed - no all disk xs/size match\" "
            "fid=%08llx", mFid);
  }

  return disk_xs_sz_match;
}

//----------------------------------------------------------------------------
// Method to repair an FST checksum and/or size difference error
//----------------------------------------------------------------------------
bool
FsckEntry::RepairFstXsSzDiff()
{
  // This only makes sense for replica layouts
  if (LayoutId::IsRain(mMgmFmd.layout_id())) {
    return true;
  }

  std::string mgm_xs_val =
    StringConversion::BinData2HexString(mMgmFmd.checksum().c_str(),
                                        SHA_DIGEST_LENGTH,
                                        LayoutId::GetChecksumLen(mMgmFmd.layout_id()));
  // Make sure at least one disk xs and size match the MGM ones
  uint64_t sz_val {0ull};
  std::string xs_val;
  std::set<eos::common::FileSystem::fsid_t> good_fsids;
  std::set<eos::common::FileSystem::fsid_t> bad_fsids;

  for (auto it = mFstFileInfo.cbegin(); it != mFstFileInfo.cend(); ++it) {
    auto& finfo = it->second;

    if (finfo->mFstErr != FstErr::None) {
      eos_err("msg=\"unavailable replica info\" fid=%08llx fsid=%lu",
              mFid, it->first);
      bad_fsids.insert(finfo->mFstFmd.fsid());
      continue;
    }

    xs_val = finfo->mFstFmd.diskchecksum();
    sz_val = finfo->mFstFmd.disksize();

    // The disksize/xs must also match the original reference size/xs
    if ((mgm_xs_val == xs_val) && (mMgmFmd.size() == sz_val) &&
        (finfo->mFstFmd.size() == sz_val) &&
        (finfo->mFstFmd.checksum() == xs_val)) {
      good_fsids.insert(finfo->mFstFmd.fsid());
    } else {
      bad_fsids.insert(finfo->mFstFmd.fsid());
    }
  }

  if (bad_fsids.empty()) {
    eos_warning("msg=\"fst xs/size repair skip - no bad replicas\" fid=%08llx",
                mFid);
    return true;
  }

  if (good_fsids.empty()) {
    eos_err("msg=\"fst xs/size repair failed - no good replicas\" fid=%08llx",
            mFid);
    return false;
  }

  bool all_repaired {true};
  eos::common::FileSystem::fsid_t good_fsid = *good_fsids.begin();

  for (auto bad_fsid : bad_fsids) {
    // Trigger an fsck repair job (much like a drain job) doing a TPC
    auto repair_job = mRepairFactory(mFid, good_fsid, 0, bad_fsids, {},
                                     true, "fsck");
    repair_job->DoIt();

    if (repair_job->GetStatus() != FsckRepairJob::Status::OK) {
      eos_err("msg=\"fst xs/size repair failed\" fid=%08llx bad_fsid=%lu "
              "good_fsid=%lu", mFid, bad_fsid, good_fsid);
      all_repaired = false;
    } else {
      eos_info("msg=\"fst xs/size repair successful\" fid=%08llx bad_fsid=%lu",
               mFid, bad_fsid);
    }
  }

  // @todo(esindril): after any fsck repair we should also trigger an MGM
  // resync on all the replicas so that the locations get updated properly
  // in the local DB of the FSTs. Not critical for the moment as we don't
  // rely on this info for the time beging.
  return all_repaired;
}


//------------------------------------------------------------------------------
// Method to repair an unregistered FST replica
//------------------------------------------------------------------------------
bool
FsckEntry::RepairReplicaInconsistencies()
{
  if (LayoutId::IsRain(mMgmFmd.layout_id())) {
    // Any stripe inconsistency translates into a rewrite of the file
    return true;
  }

  std::string mgm_xs_val =
    StringConversion::BinData2HexString(mMgmFmd.checksum().c_str(),
                                        SHA_DIGEST_LENGTH,
                                        LayoutId::GetChecksumLen(mMgmFmd.layout_id()));
  std::set<eos::common::FileSystem::fsid_t> to_drop;
  std::set<eos::common::FileSystem::fsid_t> unreg_fsids;
  std::set<eos::common::FileSystem::fsid_t> repmiss_fsids;

  // Account for missing replicas from MGM's perspective
  for (const auto& fsid : mMgmFmd.locations()) {
    auto it = mFstFileInfo.find(fsid);

    if ((it == mFstFileInfo.end()) ||
        (it->second->mFstErr == FstErr::NotOnDisk)) {
      repmiss_fsids.insert(it->first);
    }
  }

  // Account for unregisterd replicas and other replicas to be dropped
  for (const auto& elem : mFstFileInfo) {
    bool found = false;

    for (const auto& loc : mMgmFmd.locations()) {
      if (elem.first == loc) {
        found = true;
        break;
      }
    }

    auto& finfo = elem.second;

    if (found) {
      if (finfo->mFstErr == FstErr::NotOnDisk) {
        to_drop.insert(elem.first);
      }
    } else {
      // Make sure the FST size/xs match the MGM ones
      if ((finfo->mFstFmd.disksize() != mMgmFmd.size()) ||
          (finfo->mFstFmd.diskchecksum() != mgm_xs_val)) {
        to_drop.insert(elem.first);
      } else {
        unreg_fsids.insert(elem.first);
      }
    }
  }

  // First drop any missing replicas from the MGM
  for (const auto& drop_fsid : repmiss_fsids) {
    // Update the local MGM fmd object
    auto mutable_loc = mMgmFmd.mutable_locations();

    for (auto it = mutable_loc->begin(); it != mutable_loc->end(); ++it) {
      if (*it == drop_fsid) {
        mutable_loc->erase(it);
        break;
      }
    }

    if (gOFS) {
      try { // Update the MGM file md object
        eos::Prefetcher::prefetchFileMDWithParentsAndWait(gOFS->eosView, mFid);
        eos::common::RWMutexReadLock ns_rd_lock(gOFS->eosViewRWMutex);
        auto fmd = gOFS->eosFileService->getFileMD(mFid);
        fmd->unlinkLocation(drop_fsid);
        fmd->removeLocation(drop_fsid);
        gOFS->eosView->updateFileStore(fmd.get());
        eos_info("msg=\"remove missing replica\" fid=%08llx drop_fsid=%lu",
                 mFid, drop_fsid);
      } catch (const eos::MDException& e) {
        eos_err("msg=\"replica inconsistency repair failed, no file metadata\" "
                "fid=%08llx", mFid);
        return false;
      }
    }
  }

  // Then drop any other inconsistent replicas from both the MGM and the FST
  for (auto fsid : to_drop) {
    (void) DropReplica(fsid);
    // Drop also from the locasl map of FST fmd info
    mFstFileInfo.erase(fsid);
    auto mutable_loc = mMgmFmd.mutable_locations();

    for (auto it = mutable_loc->begin(); it != mutable_loc->end(); ++it) {
      if (*it == fsid) {
        mutable_loc->erase(it);
        break;
      }
    }
  }

  to_drop.clear();
  // Decide if we need to attach or discard any replicas
  uint32_t num_expected_rep = LayoutId::GetStripeNumber(mMgmFmd.layout_id()) + 1;
  uint32_t num_actual_rep = mMgmFmd.locations().size();

  if (num_actual_rep >= num_expected_rep) { // over-replicated
    int over_replicated = num_actual_rep - num_expected_rep;
    // All the unregistered replicas can be dropped
    to_drop.insert(unreg_fsids.begin(), unreg_fsids.end());

    while ((over_replicated > 0) && !mMgmFmd.locations().empty()) {
      to_drop.insert(mMgmFmd.locations(0));
      mMgmFmd.mutable_locations()->erase(mMgmFmd.locations().begin());
      --over_replicated;
    }
  } else {
    if (num_actual_rep < num_expected_rep) { // under-replicated
      // While under-replicated and we still have unregistered replicas then
      // attach them
      while ((num_actual_rep < num_expected_rep) && !unreg_fsids.empty()) {
        eos::common::FileSystem::fsid_t new_fsid = *unreg_fsids.begin();
        unreg_fsids.erase(unreg_fsids.begin());
        mMgmFmd.add_locations(new_fsid);

        if (gOFS) {
          try {
            eos::Prefetcher::prefetchFileMDWithParentsAndWait(gOFS->eosView, mFid);
            eos::common::RWMutexReadLock ns_rd_lock(gOFS->eosViewRWMutex);
            auto fmd = gOFS->eosFileService->getFileMD(mFid);
            fmd->addLocation(new_fsid);
            gOFS->eosView->updateFileStore(fmd.get());
            eos_info("msg=\"attached unregistered replica\" fid=%08llx "
                     "new_fsid=%lu", mFid, new_fsid);
          } catch (const eos::MDException& e) {
            eos_err("msg=\"unregistered replica repair failed, no file metadata\" "
                    "fid=%08llx", mFid);
            return false;
          }
        }

        ++num_actual_rep;
      }

      // Drop any remaining unregistered replicas
      to_drop.insert(unreg_fsids.begin(), unreg_fsids.end());

      // If still under-replicated then start creating new replicas
      while (num_actual_rep < num_expected_rep) {
        // Trigger a fsck repair job but without dropping the source, this is
        // similar to adjust replica
        eos::common::FileSystem::fsid_t good_fsid = mMgmFmd.locations(0);
        auto repair_job = mRepairFactory(mFid, good_fsid, 0, {}, to_drop,
                                         false, "fsck");
        repair_job->DoIt();

        if (repair_job->GetStatus() != FsckRepairJob::Status::OK) {
          eos_err("msg=\"replica inconsistency repair failed\" fid=%08llx "
                  "src_fsid=%lu", mFid, good_fsid);
          return false;
        } else {
          eos_info("msg=\"replica inconsistency repair successful\" fid=%08llx "
                   "src_fsid=%lu", mFid, good_fsid);
        }

        ++num_actual_rep;
      }
    }
  }

  // Discard unregistered/bad replicas
  for (auto fsid : to_drop) {
    (void) DropReplica(fsid);
    // Drop also from the locasl map of FST fmd info
    mFstFileInfo.erase(fsid);
  }

  return true;
}

//------------------------------------------------------------------------------
// Drop replica form FST and also update the namespace view for the given
// file system id
//------------------------------------------------------------------------------
bool
FsckEntry::DropReplica(eos::common::FileSystem::fsid_t fsid) const
{
  bool retc = true;
  eos_info("msg=\"drop (unregistered) replica\" fid=%08llx fsid=%lu",
           mFid, fsid);

  // Send external deletion to the FST
  if (gOFS && !gOFS->DeleteExternal(fsid, mFid)) {
    eos_err("msg=\"failed to send unlink to FST\" fid=%08llx fsid=%lu",
            mFid, fsid);
    retc = false;
  }

  // Drop from the namespace, we don't need the path as root can drop by fid
  XrdOucErrInfo err;
  eos::common::VirtualIdentity vid = eos::common::VirtualIdentity::Root();

  if (gOFS && gOFS->_dropstripe("", mFid, err, vid, fsid, true)) {
    eos_err("msg=\"failed to drop replicas from ns\" fid=%08llx fsid=%lu",
            mFid, fsid);
  }

  return retc;
}

//------------------------------------------------------------------------------
// Generate repair workflow for the current entry
//------------------------------------------------------------------------------
std::list<RepairFnT>
FsckEntry::GenerateRepairWokflow()
{
  auto it = mMapRepairOps.find(mReportedErr);

  if (it == mMapRepairOps.end()) {
    return {};
  }

  return it->second;
}

//------------------------------------------------------------------------------
// Collect FST file metadata information
//------------------------------------------------------------------------------
void
FsckEntry::CollectFstInfo(eos::common::FileSystem::fsid_t fsid)
{
  using eos::common::FileId;
  std::string host_port;
  std::string fst_local_path;
  {
    eos::common::RWMutexReadLock fs_rd_lock(FsView::gFsView.ViewMutex);
    FileSystem* fs = FsView::gFsView.mIdView.lookupByID(fsid);

    if (fs) {
      host_port = fs->GetString("hostport");
      fst_local_path = fs->GetPath();
    }
  }

  if (host_port.empty() || fst_local_path.empty()) {
    eos_err("msg=\"missing or misconfigured file system\" fsid=%lu", fsid);
    mFstFileInfo.emplace(fsid, std::make_unique<FstFileInfoT>("",
                         FstErr::NoContact));
    return;
  }

  std::ostringstream oss;
  oss << "root://" << host_port << "//dummy";
  std::string surl = oss.str();
  XrdCl::URL url(surl);

  if (!url.IsValid()) {
    eos_err("msg=\"invalid url\" url=\"%s\"", surl.c_str());
    mFstFileInfo.emplace(fsid, std::make_unique<FstFileInfoT>("",
                         FstErr::NoContact));
    return;
  }

  XrdOucString fpath_local;
  FileId::FidPrefix2FullPath(FileId::Fid2Hex(mFid).c_str(),
                             fst_local_path.c_str(),
                             fpath_local);
  // Check that the file exists on disk
  XrdCl::StatInfo* stat_info_raw {nullptr};
  std::unique_ptr<XrdCl::StatInfo> stat_info(stat_info_raw);
  uint16_t timeout = 10;
  XrdCl::FileSystem fs(url);
  XrdCl::XRootDStatus status = fs.Stat(fpath_local.c_str(), stat_info_raw,
                                       timeout);

  if (!status.IsOK()) {
    if (status.code == XrdCl::errOperationExpired) {
      mFstFileInfo.emplace(fsid, std::make_unique<FstFileInfoT>("",
                           FstErr::NoContact));
    } else {
      mFstFileInfo.emplace(fsid, std::make_unique<FstFileInfoT>("",
                           FstErr::NotOnDisk));
    }

    return;
  }

  // Collect file metadata stored on the FST about the current file
  auto ret_pair =  mFstFileInfo.emplace(fsid, std::make_unique<FstFileInfoT>
                                        (fpath_local.c_str(), FstErr::None));
  auto& finfo = ret_pair.first->second;
  finfo->mDiskSize = stat_info->GetSize();

  if (!GetFstFmd(finfo, fs, fsid)) {
    return;
  }
}

//------------------------------------------------------------------------------
// Get file metadata info stored at the FST
//------------------------------------------------------------------------------
bool
FsckEntry::GetFstFmd(std::unique_ptr<FstFileInfoT>& finfo,
                     XrdCl::FileSystem& fs,
                     eos::common::FileSystem::fsid_t fsid)
{
  XrdCl::Buffer* raw_response {nullptr};
  std::unique_ptr<XrdCl::Buffer> response(raw_response);
  // Create query command for file metadata
  std::ostringstream oss;
  oss << "/?fst.pcmd=getfmd&fst.getfmd.fsid=" << fsid
      << "&fst.getfmd.fid=" << std::hex << mFid;
  XrdCl::Buffer arg;
  arg.FromString(oss.str().c_str());
  uint16_t timeout = 10;
  XrdCl::XRootDStatus status = fs.Query(XrdCl::QueryCode::OpaqueFile, arg,
                                        raw_response, timeout);

  if (!status.IsOK()) {
    if (status.code == XrdCl::errOperationExpired) {
      eos_err("msg=\"timeout file metadata query\" fsid=%lu", fsid);
      finfo->mFstErr = FstErr::NoContact;
    } else {
      eos_err("msg=\"failed file metadata query\" fsid=%lu", fsid);
      finfo->mFstErr = FstErr::NoFmdInfo;
    }

    return false;
  }

  if ((response == nullptr) ||
      (strncmp(response->GetBuffer(), "ERROR", 5) == 0)) {
    finfo->mFstErr = FstErr::NoFmdInfo;
    return false;
  }

  // Parse in the file metadata info
  XrdOucEnv fmd_env(response->GetBuffer());

  if (!eos::fst::EnvToFstFmd(fmd_env, finfo->mFstFmd)) {
    eos_err("msg=\"failed parsing fmd env\" fsid=%lu", fsid);
    finfo->mFstErr = FstErr::NoFmdInfo;
    return false;
  }

  return true;
}

//------------------------------------------------------------------------------
// Convert string to FsckErr type
//------------------------------------------------------------------------------
FsckErr ConvertToFsckErr(const std::string& serr)
{
  if (serr == "m_cx_diff") {
    return FsckErr::MgmXsDiff;
  } else if (serr == "m_mem_sz_diff") {
    return FsckErr::MgmSzDiff;
  } else if (serr == "d_cx_diff") {
    return FsckErr::FstXsDiff;
  } else if (serr == "d_mem_sz_diff") {
    return FsckErr::FstSzDiff;
  } else if (serr == "unreg_n") {
    return FsckErr::UnregRepl;
  } else if (serr == "rep_diff_n") {
    return FsckErr::DiffRepl;
  } else if (serr == "rep_missing_n") {
    return FsckErr::MissRepl;
  } else {
    return FsckErr::None;
  }
}

EOSMGMNAMESPACE_END
