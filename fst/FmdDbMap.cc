//------------- ----------------------------------------------------------------
// File: FmdDbMap.cc
// Author: Geoffray Adde - CERN
//------------- ----------------------------------------------------------------

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

#include "fst/FmdDbMap.hh"
#include "common/Path.hh"
#include "common/ShellCmd.hh"
#include "proto/Fs.pb.h"
#include "proto/ConsoleRequest.pb.h"
#include "fst/checksum/ChecksumPlugins.hh"
#include "fst/io/FileIoPluginCommon.hh"
#include "fst/Config.hh"
#include "XrdCl/XrdClFileSystem.hh"
#include "namespace/ns_quarkdb/persistency/FileMDSvc.hh"
#include "namespace/ns_quarkdb/persistency/MetadataFetcher.hh"
#include "namespace/ns_quarkdb/persistency/RequestBuilder.hh"
#include "namespace/ns_quarkdb/QdbContactDetails.hh"
#include "qclient/structures/QSet.hh"
#include <stdio.h>
#include <sys/mman.h>
#include <fts.h>
#include <iostream>
#include <fstream>
#include <algorithm>

EOSFSTNAMESPACE_BEGIN

// Global objects
FmdDbMapHandler gFmdDbMapHandler;

using eos::common::LayoutId;

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
FmdDbMapHandler::FmdDbMapHandler()
{
  using eos::common::FileSystem;
  SetLogId("CommonFmdDbMapHandler");
  lvdboption.CacheSizeMb = 0;
  lvdboption.BloomFilterNbits = 0;
  mFsMtxMap.set_deleted_key(std::numeric_limits<FileSystem::fsid_t>::max() - 2);
  mFsMtxMap.set_empty_key(std::numeric_limits<FileSystem::fsid_t>::max() - 1);
}

//------------------------------------------------------------------------------
// Convert an MGM env representation to an Fmd struct
//------------------------------------------------------------------------------
bool
FmdDbMapHandler::EnvMgmToFmd(XrdOucEnv& env, struct Fmd& fmd)
{
  // Check that all tags are present
  if (!env.Get("id") ||
      !env.Get("cid") ||
      !env.Get("ctime") ||
      !env.Get("ctime_ns") ||
      !env.Get("mtime") ||
      !env.Get("mtime_ns") ||
      !env.Get("size") ||
      !env.Get("checksum") ||
      !env.Get("lid") ||
      !env.Get("uid") ||
      !env.Get("gid")) {
    return false;
  }

  fmd.set_fid(strtoull(env.Get("id"), 0, 10));
  fmd.set_cid(strtoull(env.Get("cid"), 0, 10));
  fmd.set_ctime(strtoul(env.Get("ctime"), 0, 10));
  fmd.set_ctime_ns(strtoul(env.Get("ctime_ns"), 0, 10));
  fmd.set_mtime(strtoul(env.Get("mtime"), 0, 10));
  fmd.set_mtime_ns(strtoul(env.Get("mtime_ns"), 0, 10));
  fmd.set_mgmsize(strtoull(env.Get("size"), 0, 10));
  fmd.set_lid(strtoul(env.Get("lid"), 0, 10));
  fmd.set_uid((uid_t) strtoul(env.Get("uid"), 0, 10));
  fmd.set_gid((gid_t) strtoul(env.Get("gid"), 0, 10));
  fmd.set_mgmchecksum(env.Get("checksum"));
  fmd.set_locations(env.Get("location") ? env.Get("location") : "");
  size_t cslen = eos::common::LayoutId::GetChecksumLen(fmd.lid()) * 2;
  fmd.set_mgmchecksum(std::string(fmd.mgmchecksum()).erase(std::min(
                        fmd.mgmchecksum().length(), cslen)));
  return true;
}

//----------------------------------------------------------------------------
// Convert namespace file proto object to an Fmd struct
//----------------------------------------------------------------------------
bool
FmdDbMapHandler::NsFileProtoToFmd(eos::ns::FileMdProto&& filemd,
                                  struct Fmd& fmd)
{
  fmd.set_fid(filemd.id());
  fmd.set_cid(filemd.cont_id());
  eos::IFileMD::ctime_t ctime;
  (void) memcpy(&ctime, filemd.ctime().data(), sizeof(ctime));
  eos::IFileMD::ctime_t mtime;
  (void) memcpy(&mtime, filemd.mtime().data(), sizeof(mtime));
  fmd.set_ctime(ctime.tv_sec);
  fmd.set_ctime_ns(ctime.tv_nsec);
  fmd.set_mtime(mtime.tv_sec);
  fmd.set_mtime_ns(mtime.tv_nsec);
  fmd.set_mgmsize(filemd.size());
  fmd.set_lid(filemd.layout_id());
  fmd.set_uid(filemd.uid());
  fmd.set_gid(filemd.gid());
  std::string str_xs;
  uint8_t size = filemd.checksum().size();

  for (uint8_t i = 0; i < size; i++) {
    char hx[3];
    hx[0] = 0;
    snprintf(static_cast<char*>(hx), sizeof(hx), "%02x",
             *(unsigned char*)(filemd.checksum().data() + i));
    str_xs += static_cast<char*>(hx);
  }

  size_t cslen = eos::common::LayoutId::GetChecksumLen(filemd.layout_id()) * 2;
  // Truncate the checksum to the right string length
  str_xs.erase(std::min(str_xs.length(), cslen));
  fmd.set_mgmchecksum(str_xs);
  std::string slocations;

  for (const auto& loc : filemd.locations()) {
    slocations += std::to_string(loc);
    slocations += ",";
  }

  if (!slocations.empty()) {
    slocations.pop_back();
  }

  fmd.set_locations(slocations);
  return true;
}

//------------------------------------------------------------------------------
// Return Fmd from MGM doing getfmd command
//------------------------------------------------------------------------------
int
FmdDbMapHandler::GetMgmFmd(const char* manager,
                           eos::common::FileId::fileid_t fid, struct Fmd& fmd)
{
  if (!fid) {
    return EINVAL;
  }

  int rc = 0;
  XrdCl::Buffer arg;
  XrdCl::XRootDStatus status;
  char sfmd[1024];
  snprintf(sfmd, sizeof(sfmd) - 1, "%llu", fid);
  XrdOucString fmdquery = "/?mgm.pcmd=getfmd&mgm.getfmd.fid=";
  fmdquery += sfmd;
  XrdOucString address = "root://";
  std::string current_mgr;
  std::unique_ptr<XrdCl::Buffer> response;
  XrdCl::Buffer* responseRaw = nullptr;

  if (!manager) {
    // Use the broadcasted manager name
    XrdSysMutexHelper lock(Config::gConfig.Mutex);
    current_mgr = Config::gConfig.Manager.c_str();
  } else {
    current_mgr = manager;
  }

  address += current_mgr.c_str();
  address += "//dummy?xrd.wantprot=sss";
  XrdCl::URL url(address.c_str());
  std::unique_ptr<XrdCl::FileSystem> fs;
again:

  if (!url.IsValid()) {
    eos_static_err("error=URL is not valid: %s", address.c_str());
    return EINVAL;
  }

  fs.reset(new XrdCl::FileSystem(url));

  if (!fs) {
    eos_static_err("error=failed to get new FS object");
    return EINVAL;
  }

  arg.FromString(fmdquery.c_str());
  uint16_t timeout = 10;
  status = fs->Query(XrdCl::QueryCode::OpaqueFile, arg, responseRaw, timeout);
  response.reset(responseRaw);
  responseRaw = nullptr;

  if (status.IsOK()) {
    rc = 0;
    eos_static_debug("got replica file meta data from mgm %s for fxid=%08llx",
                     current_mgr.c_str(), fid);
  } else {
    eos_static_err("msg=\"query error\" fxid=%08llx status=%d code=%d", fid,
                   status.status,
                   status.code);

    if ((status.code >= 100) &&
        (status.code <= 300)) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      eos_static_info("msg=\"retry query\" fxid=%08llx query=\"%s\"", fid,
                      fmdquery.c_str());

      if (!manager) {
        // Use the broadcasted manager name
        XrdSysMutexHelper lock(Config::gConfig.Mutex);
        current_mgr = Config::gConfig.Manager.c_str();
        address = "root://";
        address += current_mgr.c_str();
        address += "//dummy?xrd.wantprot=sss";
        url.Clear();
        url.FromString(address.c_str());
      }

      goto again;
    }

    rc = ECOMM;
    eos_static_err("Unable to retrieve meta data from mgm %s for fxid=%08llx",
                   current_mgr.c_str(), fid);
  }

  if (rc) {
    return EIO;
  }

  // Check if response contains any data
  if (!response->GetBuffer()) {
    eos_static_info("Unable to retrieve meta data from mgm %s for fxid=%08llx, "
                    "result data is empty", current_mgr.c_str(), fid);
    return ENODATA;
  }

  std::string sresult = response->GetBuffer();

  if ((sresult.find("getfmd: retc=0 ")) == std::string::npos) {
    // Remote side couldn't get the record
    eos_static_info("Unable to retrieve meta data on remote mgm %s for "
                    "fxid=%08llx - result=%s", current_mgr.c_str(), fid,
                    response->GetBuffer());
    return ENODATA;
  } else {
    // Truncate 'getfmd: retc=0 ' away
    sresult.erase(0, 15);
  }

  // Get the remote file meta data into an env hash
  XrdOucEnv fmdenv(sresult.c_str());

  if (!EnvMgmToFmd(fmdenv, fmd)) {
    int envlen;
    eos_static_err("Failed to unparse file meta data %s for fxid=%08llx",
                   fmdenv.Env(envlen), fid);
    return EIO;
  }

  // Basic check
  if (fmd.fid() != fid) {
    eos_static_err("Uups! Received wrong meta data from remote server - fid "
                   "is %lu instead of %lu !", fmd.fid(), fid);
    return EIO;
  }

  return 0;
}

//------------------------------------------------------------------------------
// Call the 'auto repair' function e.g. 'file convert --rewrite'
//------------------------------------------------------------------------------
int
FmdDbMapHandler::CallAutoRepair(const char* manager,
                                eos::common::FileId::fileid_t fid)
{
  if (!fid) {
    return EINVAL;
  }

  int rc = 0;
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  XrdCl::XRootDStatus status;
  XrdOucString fmdquery = "/?mgm.pcmd=rewrite&mgm.fid=";
  const std::string hex_fid = eos::common::FileId::Fid2Hex(fid);
  fmdquery += hex_fid.c_str();
  // @todo(esindril) Legacy, remove once fsctl/Rewrite.cc no longer expects 'fxid'
  fmdquery += "&mgm.fxid=";
  fmdquery += hex_fid.c_str(); // legacy
  XrdOucString address = "root://";
  std::string current_mgr;

  if (!manager) {
    // Use the broadcasted manager name
    XrdSysMutexHelper lock(Config::gConfig.Mutex);
    current_mgr = Config::gConfig.Manager.c_str();
  } else {
    current_mgr = manager;
  }

  address += current_mgr.c_str();
  address += "//dummy?xrd.wantprot=sss";
  XrdCl::URL url(address.c_str());

  if (!url.IsValid()) {
    eos_static_err("error=URL is not valid: %s", address.c_str());
    return EINVAL;
  }

  std::unique_ptr<XrdCl::FileSystem> fs(new XrdCl::FileSystem(url));

  if (!fs) {
    eos_static_err("error=failed to get new FS object");
    return EINVAL;
  }

  arg.FromString(fmdquery.c_str());
  status = fs->Query(XrdCl::QueryCode::OpaqueFile, arg, response);

  if (status.IsOK()) {
    eos_static_debug("msg=\"scheduled repair\" mgm=%s fxid=%s",
                     current_mgr.c_str(), hex_fid.c_str());
    rc = 0;
  } else {
    eos_static_err("msg=\"failed to schedule repair\" mgm=%s fxid=%s "
                   "err_msg=\"%s\"", current_mgr.c_str(), hex_fid.c_str(),
                   status.ToString().c_str());
    rc = ECOMM;
  }

  delete response;
  return rc;
}

//------------------------------------------------------------------------------
// Get number of file systems
//------------------------------------------------------------------------------
uint32_t
FmdDbMapHandler::GetNumFileSystems() const
{
  eos::common::RWMutexReadLock rd_lock(mMapMutex);
  return mDbMap.size();
}

//------------------------------------------------------------------------------
// Set a new DB file for a filesystem id
//------------------------------------------------------------------------------
bool
FmdDbMapHandler::SetDBFile(const char* meta_dir, int fsid)
{
  bool is_attached = false;
  {
    // First check if DB is already open - in this case we first do a shutdown
    eos::common::RWMutexWriteLock wr_lock(mMapMutex);

    if (mDbMap.count(fsid)) {
      is_attached = true;
    }
  }

  if (is_attached) {
    if (ShutdownDB(fsid, true)) {
      is_attached = false;
    }
  }

  char fsDBFileName[1024];
  sprintf(fsDBFileName, "%s/fmd.%04d.%s", meta_dir, fsid,
          eos::common::DbMap::getDbType().c_str());
  eos_info("%s DB is now %s", eos::common::DbMap::getDbType().c_str(),
           fsDBFileName);
  eos::common::RWMutexWriteLock wr_lock(mMapMutex);
  FsWriteLock wlock(fsid);

  if (!is_attached) {
    auto result = mDbMap.insert(std::make_pair(fsid, new eos::common::DbMap()));

    if (result.second == false) {
      eos_err("msg=\"failed to insert new db in map, fsid=%lli", fsid);
      return false;
    }
  }

  // Create / or attach the db (try to repair if needed)
  eos::common::LvDbDbMapInterface::Option* dbopt = &lvdboption;

  // If we have not set the leveldb option, use the default (currently, bloom
  // filter 10 bits and 100MB cache)
  if (lvdboption.BloomFilterNbits == 0) {
    dbopt = NULL;
  }

  if (!mDbMap[fsid]->attachDb(fsDBFileName, true, 0, dbopt)) {
    eos_static_err("failed to attach %s database file %s",
                   eos::common::DbMap::getDbType().c_str(), fsDBFileName);
    return false;
  } else {
    mDbMap[fsid]->outOfCore(true);
  }

  return true;
}

//------------------------------------------------------------------------------
// Shutdown an open DB file
//------------------------------------------------------------------------------
bool
FmdDbMapHandler::ShutdownDB(eos::common::FileSystem::fsid_t fsid, bool do_lock)
{
  eos_info("%s DB shutdown for fsid=%u",
           eos::common::DbMap::getDbType().c_str(), fsid);
  eos::common::RWMutexWriteLock wr_lock;

  if (do_lock) {
    wr_lock.Grab(mMapMutex);
  }

  if (mDbMap.count(fsid)) {
    if (mDbMap[fsid]->detachDb()) {
      delete mDbMap[fsid];
      mDbMap.erase(fsid);
      return true;
    }
  }

  return false;
}

//------------------------------------------------------------------------------
// Return/create an Fmd struct for the given file/filesystem id for user
// uid/gid and layout layoutid
//------------------------------------------------------------------------------
FmdHelper*
FmdDbMapHandler::LocalGetFmd(eos::common::FileId::fileid_t fid,
                             eos::common::FileSystem::fsid_t fsid,
                             uid_t uid, gid_t gid,
                             eos::common::LayoutId::layoutid_t layoutid,
                             bool isRW, bool force)
{
  if (fid == 0) {
    eos_warning("fxid=0 requested for fsid=", fsid);
    return 0;
  }

  eos::common::RWMutexReadLock lock(mMapMutex);

  if (mDbMap.count(fsid)) {
    Fmd valfmd;
    {
      FsReadLock fs_rd_lock(fsid);

      if (LocalExistFmd(fid, fsid)) {
        // Reading an existing entry
        FmdHelper* fmd = new FmdHelper();

        if (!fmd) {
          return 0;
        }

        // make a copy of the current record
        valfmd = LocalRetrieveFmd(fid, fsid);
        fmd->Replicate(valfmd);

        if (fmd->mProtoFmd.fid() != fid) {
          eos_crit("unable to get fmd for fid %llu on fs %lu - file id mismatch"
                   " in meta data block (%llu)", fid, (unsigned long) fsid,
                   fmd->mProtoFmd.fid());
          delete fmd;
          return 0;
        }

        if (fmd->mProtoFmd.fsid() != fsid) {
          eos_crit("unable to get fmd for fid %llu on fs %lu - filesystem id "
                   "mismatch in meta data block (%llu)", fid,
                   (unsigned long) fsid, fmd->mProtoFmd.fsid());
          delete fmd;
          return 0;
        }

        // The force flag allows to retrieve 'any' value even with inconsistencies
        // as needed by ResyncAllMgm
        if (!force) {
          if (!eos::common::LayoutId::IsRain(fmd->mProtoFmd.lid())) {
            // If we have a mismatch between the mgm/disk and 'ref' value in size,
            // we don't return the Fmd record
            if ((!isRW) &&
                ((fmd->mProtoFmd.disksize() &&
                  (fmd->mProtoFmd.disksize() != 0xfffffffffff1ULL) &&
                  (fmd->mProtoFmd.disksize() != fmd->mProtoFmd.size())) ||
                 (fmd->mProtoFmd.mgmsize() &&
                  (fmd->mProtoFmd.mgmsize() != 0xfffffffffff1ULL) &&
                  (fmd->mProtoFmd.mgmsize() != fmd->mProtoFmd.size())))) {
              eos_crit("msg=\"size mismatch disk/mgm vs memory\" fxid=%08llx "
                       "fsid=%lu size=%llu disksize=%llu mgmsize=%llu",
                       fid, (unsigned long) fsid, fmd->mProtoFmd.size(),
                       fmd->mProtoFmd.disksize(), fmd->mProtoFmd.mgmsize());
              delete fmd;
              return 0;
            }

            // Don't return a record, if there is a checksum error flagged
            if ((!isRW) &&
                ((fmd->mProtoFmd.filecxerror() == 1) ||
                 (fmd->mProtoFmd.mgmchecksum().length() &&
                  (fmd->mProtoFmd.mgmchecksum() != fmd->mProtoFmd.checksum())))) {
              eos_crit("msg=\"checksum error flagged/detected fxid=%08llx "
                       "fsid=%lu checksum=%s diskchecksum=%s mgmchecksum=%s "
                       "filecxerror=%d blockcxerror=%d", fid,
                       (unsigned long) fsid, fmd->mProtoFmd.checksum().c_str(),
                       fmd->mProtoFmd.diskchecksum().c_str(),
                       fmd->mProtoFmd.mgmchecksum().c_str(),
                       fmd->mProtoFmd.filecxerror(),
                       fmd->mProtoFmd.blockcxerror());
              delete fmd;
              return 0;
            }
          } else {
            // @todo(esindril) decide what flags to set for rain layouts
          }
        }

        return fmd;
      }
    }

    if (isRW) {
      // Create a new record
      struct timeval tv;
      struct timezone tz;
      gettimeofday(&tv, &tz);
      FsWriteLock lock(fsid); // --> (return)
      valfmd.set_uid(uid);
      valfmd.set_gid(gid);
      valfmd.set_lid(layoutid);
      valfmd.set_fsid(fsid);
      valfmd.set_fid(fid);
      valfmd.set_ctime(tv.tv_sec);
      valfmd.set_mtime(tv.tv_sec);
      valfmd.set_atime(tv.tv_sec);
      valfmd.set_ctime_ns(tv.tv_usec * 1000);
      valfmd.set_mtime_ns(tv.tv_usec * 1000);
      valfmd.set_atime_ns(tv.tv_usec * 1000);
      FmdHelper* fmd = new FmdHelper(fid, fsid);

      if (!fmd) {
        return 0;
      }

      fmd->Replicate(valfmd);

      if (Commit(fmd, false)) {
        eos_debug("returning meta data block for fid %llu on fs %d", fid,
                  (unsigned long) fsid);
        // return the mmaped meta data block
        return fmd;
      } else {
        eos_crit("unable to write new block for fid %llu on fs %d - no changelog "
                 "db open for writing", fid, (unsigned long) fsid);
        delete fmd;
        return 0;
      }
    } else {
      eos_warning("unable to get fmd for fid %llu on fs %lu - record not found",
                  fid, (unsigned long) fsid);
      return 0;
    }
  } else {
    eos_crit("unable to get fmd for fid %llu on fs %lu - there is no changelog "
             "file open for that file system id", fid, (unsigned long) fsid);
    return 0;
  }
}

//------------------------------------------------------------------------------
// Delete a record associated with fid and filesystem fsid
//------------------------------------------------------------------------------
bool
FmdDbMapHandler::LocalDeleteFmd(eos::common::FileId::fileid_t fid,
                                eos::common::FileSystem::fsid_t fsid)
{
  bool rc = true;
  eos::common::RWMutexReadLock lock(mMapMutex);
  FsWriteLock wlock(fsid);

  if (LocalExistFmd(fid, fsid)) {
    if (mDbMap[fsid]->remove(eos::common::Slice((const char*)&fid, sizeof(fid)))) {
      eos_err("unable to delete fxid=%08llx from fst table", fid);
      rc = false;
    }
  } else {
    rc = false;
  }

  return rc;
}

//------------------------------------------------------------------------------
// Commit modified Fmd record to the DB file
//------------------------------------------------------------------------------
bool
FmdDbMapHandler::Commit(FmdHelper* fmd, bool lockit)
{
  if (!fmd) {
    return false;
  }

  uint32_t fsid = fmd->mProtoFmd.fsid();
  uint64_t fid = fmd->mProtoFmd.fid();
  struct timeval tv;
  struct timezone tz;
  gettimeofday(&tv, &tz);
  fmd->mProtoFmd.set_mtime(tv.tv_sec);
  fmd->mProtoFmd.set_atime(tv.tv_sec);
  fmd->mProtoFmd.set_mtime_ns(tv.tv_usec * 1000);
  fmd->mProtoFmd.set_atime_ns(tv.tv_usec * 1000);

  if (lockit) {
    mMapMutex.LockRead();
    FsLockWrite(fsid);
  }

  if (mDbMap.count(fsid)) {
    bool res = LocalPutFmd(fid, fsid, fmd->mProtoFmd);

    // Updateed in-memory
    if (lockit) {
      FsUnlockWrite(fsid);
      mMapMutex.UnLockRead();
    }

    return res;
  } else {
    eos_crit("no %s DB open for fsid=%llu", eos::common::DbMap::getDbType().c_str(),
             (unsigned long) fsid);

    if (lockit) {
      FsUnlockWrite(fsid);
      mMapMutex.UnLockRead();
    }
  }

  return false;
}

//------------------------------------------------------------------------------
// Update fmd from disk i.e. physical file extended attributes
//------------------------------------------------------------------------------
bool
FmdDbMapHandler::UpdateWithDiskInfo(eos::common::FileSystem::fsid_t fsid,
                                    eos::common::FileId::fileid_t fid,
                                    unsigned long long disksize,
                                    std::string diskchecksum, unsigned long checktime,
                                    bool filecxerror, bool blockcxerror,
                                    bool flaglayouterror)
{
  if (!fid) {
    eos_info("skipping to insert a file with fid 0");
    return false;
  }

  eos_debug("fsid=%lu fxid=%08llx disksize=%llu diskchecksum=%s checktime=%llu "
            "fcxerror=%d bcxerror=%d flaglayouterror=%d",
            (unsigned long) fsid, fid, disksize, diskchecksum.c_str(), checktime,
            filecxerror, blockcxerror, flaglayouterror);
  eos::common::RWMutexReadLock lock(mMapMutex);
  FsWriteLock vlock(fsid);

  if (mDbMap.count(fsid)) {
    Fmd valfmd = LocalRetrieveFmd(fid, fsid);
    // Update in-memory
    valfmd.set_disksize(disksize);
    valfmd.set_size(disksize);
    valfmd.set_checksum(diskchecksum);
    valfmd.set_fid(fid);
    valfmd.set_fsid(fsid);
    valfmd.set_diskchecksum(diskchecksum);
    valfmd.set_checktime(checktime);
    valfmd.set_filecxerror(filecxerror);
    valfmd.set_blockcxerror(blockcxerror);

    if (flaglayouterror) {
      // If the mgm sync is run afterwards, every disk file is by construction an
      // orphan, until it is synced from the mgm
      valfmd.set_layouterror(LayoutId::kOrphan);
    }

    return LocalPutFmd(fid, fsid, valfmd);
  } else {
    eos_crit("no %s DB open for fsid=%llu", eos::common::DbMap::getDbType().c_str(),
             (unsigned long) fsid);
    return false;
  }
}

//------------------------------------------------------------------------------
// Update fmd from MGM metadata
//------------------------------------------------------------------------------
bool
FmdDbMapHandler::UpdateWithMgmInfo(eos::common::FileSystem::fsid_t fsid,
                                   eos::common::FileId::fileid_t fid,
                                   eos::common::FileId::fileid_t cid,
                                   eos::common::LayoutId::layoutid_t lid,
                                   unsigned long long mgmsize,
                                   std::string mgmchecksum,
                                   uid_t uid, gid_t gid,
                                   unsigned long long ctime,
                                   unsigned long long ctime_ns,
                                   unsigned long long mtime,
                                   unsigned long long mtime_ns,
                                   int layouterror, std::string locations)
{
  if (!fid) {
    eos_info("skipping to insert a file with fid 0");
    return false;
  }

  eos_debug("fsid=%lu fxid=%08llx cid=%llu lid=%lx mgmsize=%llu mgmchecksum=%s",
            (unsigned long) fsid, fid, cid, lid, mgmsize, mgmchecksum.c_str());
  eos::common::RWMutexReadLock lock(mMapMutex);
  FsWriteLock wlock(fsid);

  if (mDbMap.count(fsid)) {
    Fmd valfmd = LocalRetrieveFmd(fid, fsid);

    if (!LocalExistFmd(fid, fsid)) {
      valfmd.set_disksize(0xfffffffffff1ULL);
    }

    valfmd.set_mgmsize(mgmsize);
    valfmd.set_size(mgmsize);
    valfmd.set_checksum(mgmchecksum);
    valfmd.set_mgmchecksum(mgmchecksum);
    valfmd.set_cid(cid);
    valfmd.set_lid(lid);
    valfmd.set_uid(uid);
    valfmd.set_gid(gid);
    valfmd.set_ctime(ctime);
    valfmd.set_ctime_ns(ctime_ns);
    valfmd.set_mtime(mtime);
    valfmd.set_mtime_ns(mtime_ns);
    valfmd.set_layouterror(layouterror);
    valfmd.set_locations(locations);
    // Truncate the checksum to the right string length
    size_t cslen = LayoutId::GetChecksumLen(lid) * 2;
    valfmd.set_mgmchecksum(
      std::string(valfmd.mgmchecksum()).erase(std::min(valfmd.mgmchecksum().length(),
          cslen)));
    valfmd.set_checksum(
      std::string(valfmd.checksum()).erase(std::min(valfmd.checksum().length(),
                                           cslen)));
    return LocalPutFmd(fid, fsid, valfmd);
  } else {
    eos_crit("no %s DB open for fsid=%llu", eos::common::DbMap::getDbType().c_str(),
             (unsigned long) fsid);
    return false;
  }
}

//------------------------------------------------------------------------------
// Update local fmd with info from the scanner
//------------------------------------------------------------------------------
bool
FmdDbMapHandler::UpdateWithScanInfo(eos::common::FileSystem::fsid_t fsid,
                                    const std::string& fs_root,
                                    const std::string& fpath,
                                    bool filexs_err, bool blockxs_err)
{
  eos::common::Path cPath(fpath.c_str());
  eos::common::FileId::fileid_t fid {0ull};

  try {
    fid = std::stoull(cPath.GetName(), 0, 16);
  } catch (...) {
    eos_err("msg=\"failed to extract fid\" path=%s", fpath.c_str());
    return false;
  }

  std::string manager = eos::fst::Config::gConfig.GetManager();

  if (manager.empty()) {
    eos_err("msg=\"no manager hostname info available\"");
    return false;
  }

  // Check if we have this file in the local DB, if not, we resync first
  // the disk and then the MGM meta data
  bool orphaned = false;
  std::unique_ptr<FmdHelper> fmd {LocalGetFmd(fid, fsid, 0, 0, false, true)};

  if (fmd) {
    // Real orphans get rechecked
    if (fmd->mProtoFmd.layouterror() & eos::common::LayoutId::kOrphan) {
      orphaned = true;
    }

    // Unregistered replicas get rechecked
    if (fmd->mProtoFmd.layouterror() & eos::common::LayoutId::kUnregistered) {
      orphaned = true;
    }
  }

  if ((fmd == nullptr) || filexs_err || blockxs_err || orphaned) {
    eos_notice("msg=\"resyncing from disk\" fsid=%d fid=%08llx", fsid, fid);
    // ask the meta data handling class to update the error flags for this file
    ResyncDisk(fpath.c_str(), fsid, false);
    eos_notice("msg=\"resyncing from mgm\" fsid=%d fid=%08llx", fsid, fid);
    bool resynced = false;
    resynced = ResyncMgm(fsid, fid, manager.c_str());
    fmd.reset(LocalGetFmd(fid, fsid, 0, 0, 0, false, true));

    if (resynced && fmd) {
      if ((fmd->mProtoFmd.layouterror() ==  eos::common::LayoutId::kOrphan) ||
          ((!(fmd->mProtoFmd.layouterror() & eos::common::LayoutId::kReplicaWrong))
           && (fmd->mProtoFmd.layouterror() & eos::common::LayoutId::kUnregistered))) {
        char oname[4096];
        snprintf(oname, sizeof(oname), "%s/.eosorphans/%08llx",
                 fs_root.c_str(), (unsigned long long) fid);
        // Store the original path name as an extended attribute in case ...
        std::unique_ptr<FileIo> io(FileIoPluginHelper::GetIoObject(fpath));
        io->attrSet("user.eos.orphaned", fpath.c_str());

        // If orphan move it into the orphaned directory
        if (!rename(fpath.c_str(), oname)) {
          eos_warning("msg=\"orphaned/unregistered quarantined\" "
                      "fst-path=%s orphan-path=%s", fpath.c_str(), oname);
        } else {
          eos_err("msg=\"failed to quarantine orphaned/unregistered\" "
                  "fst-path=%s orphan-path=%s", fpath.c_str(), oname);
        }

        gFmdDbMapHandler.LocalDeleteFmd(fid, fsid);
      }
    }

    // Call the autorepair method on the MGM - but not for orphaned or
    // unregistered files. If MGM autorepair is disabled then it doesn't do
    // anything
    bool do_autorepair = false;

    if (orphaned == false) {
      if (fmd) {
        if ((fmd->mProtoFmd.layouterror() & eos::common::LayoutId::kUnregistered)
            == false) {
          do_autorepair = true;
        }
      } else {
        // The fmd could be null since LocalGetFmd returns null in case of a
        // checksum error
        do_autorepair = true;
      }
    }

    if (do_autorepair) {
      CallAutoRepair(manager.c_str(), fid);
    }
  }

  return true;
}


//------------------------------------------------------------------------------
// Reset disk information
//------------------------------------------------------------------------------
bool
FmdDbMapHandler::ResetDiskInformation(eos::common::FileSystem::fsid_t fsid)
{
  eos::common::RWMutexReadLock lock(mMapMutex);
  FsWriteLock wlock(fsid);

  if (mDbMap.count(fsid)) {
    const eos::common::DbMapTypes::Tkey* k;
    const eos::common::DbMapTypes::Tval* v;
    eos::common::DbMapTypes::Tval val;
    mDbMap[fsid]->beginSetSequence();
    unsigned long cpt = 0;

    for (mDbMap[fsid]->beginIter(false); mDbMap[fsid]->iterate(&k, &v, false);) {
      Fmd f;
      f.ParseFromString(v->value);
      f.set_disksize(0xfffffffffff1ULL);
      f.set_diskchecksum("");
      f.set_checktime(0);
      f.set_filecxerror(-1);
      f.set_blockcxerror(-1);
      val = *v;
      f.SerializeToString(&val.value);
      mDbMap[fsid]->set(*k, val);
      cpt++;
    }

    // The endSetSequence makes it impossible to know which key is faulty
    if (mDbMap[fsid]->endSetSequence() != cpt) {
      eos_err("unable to update fsid=%lu", fsid);
      return false;
    }
  } else {
    eos_crit("no %s DB open for fsid=%llu", eos::common::DbMap::getDbType().c_str(),
             (unsigned long) fsid);
    return false;
  }

  return true;
}

//------------------------------------------------------------------------------
// Reset mgm information
//------------------------------------------------------------------------------
bool
FmdDbMapHandler::ResetMgmInformation(eos::common::FileSystem::fsid_t fsid)
{
  eos::common::RWMutexReadLock lock(mMapMutex);
  FsWriteLock vlock(fsid);

  if (mDbMap.count(fsid)) {
    const eos::common::DbMapTypes::Tkey* k;
    const eos::common::DbMapTypes::Tval* v;
    eos::common::DbMapTypes::Tval val;
    mDbMap[fsid]->beginSetSequence();
    unsigned long cpt = 0;

    for (mDbMap[fsid]->beginIter(false); mDbMap[fsid]->iterate(&k, &v, false);) {
      Fmd f;
      f.ParseFromString(v->value);
      f.set_mgmsize(0xfffffffffff1ULL);
      f.set_mgmchecksum("");
      f.set_locations("");
      val = *v;
      f.SerializeToString(&val.value);
      mDbMap[fsid]->set(*k, val);
      cpt++;
    }

    // The endSetSequence makes it impossible to know which key is faulty
    if (mDbMap[fsid]->endSetSequence() != cpt) {
      eos_err("unable to update fsid=%lu", fsid);
      return false;
    }
  } else {
    eos_crit("no leveldb DB open for fsid=%llu", (unsigned long) fsid);
    return false;
  }

  return true;
}

//------------------------------------------------------------------------------
// Resync a single entry from disk
//------------------------------------------------------------------------------
bool
FmdDbMapHandler::ResyncDisk(const char* path,
                            eos::common::FileSystem::fsid_t fsid,
                            bool flaglayouterror)
{
  bool retc = true;
  off_t disksize = 0;
  eos::common::Path cPath(path);
  eos::common::FileId::fileid_t fid =
    eos::common::FileId::Hex2Fid(cPath.GetName());

  if (fid) {
    std::unique_ptr<eos::fst::FileIo>
    io(eos::fst::FileIoPluginHelper::GetIoObject(path));

    if (io) {
      struct stat buf;

      if ((!io->fileStat(&buf)) && S_ISREG(buf.st_mode)) {
        std::string checksumType, checksumStamp, filecxError, blockcxError;
        std::string diskchecksum = "";
        char checksumVal[SHA_DIGEST_LENGTH];
        size_t checksumLen = 0;
        disksize = buf.st_size;
        memset(checksumVal, 0, sizeof(checksumVal));
        checksumLen = SHA_DIGEST_LENGTH;

        if (io->attrGet("user.eos.checksum", checksumVal, checksumLen)) {
          checksumLen = 0;
        }

        io->attrGet("user.eos.checksumtype", checksumType);
        io->attrGet("user.eos.filecxerror", filecxError);
        io->attrGet("user.eos.blockcxerror", blockcxError);
        io->attrGet("user.eos.timestamp", checksumStamp);
        unsigned long checktime = std::stoul(checksumStamp);

        if (checksumLen) {
          // Use a checksum object to get the hex representation
          XrdOucString envstring = "eos.layout.checksum=";
          envstring += checksumType.c_str();
          XrdOucEnv env(envstring.c_str());
          int checksumtype = LayoutId::GetChecksumFromEnv(env);
          LayoutId::layoutid_t layoutid =
            LayoutId::GetId(LayoutId::kPlain, checksumtype);
          std::unique_ptr<CheckSum> checksum =
            eos::fst::ChecksumPlugins::GetChecksumObjectPtr(layoutid, false);

          if (checksum) {
            if (checksum->SetBinChecksum(checksumVal, checksumLen)) {
              diskchecksum = checksum->GetHexChecksum();
            }
          }
        }

        // Now update the DB
        if (!UpdateFromDisk(fsid, fid, disksize, diskchecksum, checktime,
                            (filecxError == "1") ? 1 : 0,
                            (blockcxError == "1") ? 1 : 0,
                            flaglayouterror)) {
          eos_err("msg=\"failed to update DB\" dbpath=%s fsid=%lu fxid=%08llx",
                  eos::common::DbMap::getDbType().c_str(), (unsigned long) fsid,
                  fid);
          retc = false;
        }
      }
    }
  } else {
    eos_debug("would convert %s (%s) to fid 0", cPath.GetName(), path);
    retc = false;
  }

  return retc;
}

//------------------------------------------------------------------------------
// Resync files under path into DB
//------------------------------------------------------------------------------
bool
FmdDbMapHandler::ResyncAllDisk(const char* path,
                               eos::common::FileSystem::fsid_t fsid,
                               bool flaglayouterror)
{
  char** paths = (char**) calloc(2, sizeof(char*));

  if (!paths) {
    eos_err("error: failed to allocate memory");
    return false;
  }

  paths[0] = (char*) path;
  paths[1] = 0;

  if (flaglayouterror) {
    mIsSyncing[fsid] = true;
  }

  if (!ResetDiskInformation(fsid)) {
    eos_err("failed to reset the disk information before resyncing fsid=%lu",
            fsid);
    free(paths);
    return false;
  }

  // Scan all the files
  FTS* tree = fts_open(paths, FTS_NOCHDIR, 0);

  if (!tree) {
    eos_err("fts_open failed");
    free(paths);
    return false;
  }

  FTSENT* node;
  unsigned long long cnt = 0;

  while ((node = fts_read(tree))) {
    if (node->fts_level > 0 && node->fts_name[0] == '.') {
      fts_set(tree, node, FTS_SKIP);
    } else {
      if (node->fts_info == FTS_F) {
        XrdOucString filePath = node->fts_accpath;

        if (!filePath.matches("*.xsmap")) {
          cnt++;
          eos_debug("file=%s", filePath.c_str());
          ResyncDisk(filePath.c_str(), fsid, flaglayouterror);

          if (!(cnt % 10000)) {
            eos_info("msg=\"synced files so far\" nfiles=%llu fsid=%lu", cnt,
                     (unsigned long) fsid);
          }
        }
      }
    }
  }

  if (fts_close(tree)) {
    eos_err("fts_close failed");
    free(paths);
    return false;
  }

  free(paths);
  return true;
}

//------------------------------------------------------------------------------
// Resync file meta data from MGM into local database
//------------------------------------------------------------------------------
bool
FmdDbMapHandler::ResyncMgm(eos::common::FileSystem::fsid_t fsid,
                           eos::common::FileId::fileid_t fid,
                           const char* manager)
{
  struct Fmd fMd;
  FmdHelper::Reset(fMd);
  int rc = 0;

  if ((!(rc = GetMgmFmd(manager, fid, fMd))) || (rc == ENODATA)) {
    if (rc == ENODATA) {
      eos_warning("no such file on MGM for fxid=%08llx", fid);
      fMd.set_fid(fid);

      if (fid == 0) {
        eos_warning("removing fxid=0 entry");
        return LocalDeleteFmd(fMd.fid(), fsid);
      }
    }

    // Define layouterrors
    fMd.set_layouterror(FmdHelper::LayoutError(fMd, fsid));
    // Get an existing record without creation of the record !!!
    FmdHelper* fmd = LocalGetFmd(fMd.fid(), fsid, fMd.uid(), fMd.gid(),
                                 fMd.lid(), false, true);

    if (fmd) {
      // Check if exists on disk
      if (fmd->mProtoFmd.disksize() == 0xfffffffffff1ULL) {
        if (fMd.layouterror() & LayoutId::kUnregistered) {
          // There is no replica supposed to be here and there is nothing on
          // disk, so remove it from the database
          eos_warning("removing <ghost> entry for fxid=%08llx on fsid=%lu", fid,
                      (unsigned long) fsid);
          delete fmd;
          return LocalDeleteFmd(fMd.fid(), fsid);
        }
      }
    } else {
      if (fMd.layouterror() & LayoutId::kUnregistered) {
        return true;
      }
    }

    if ((!fmd) && (rc == ENODATA)) {
      // No file on MGM and no file locally
      eos_info("fsid=%lu fxid=%08llx msg=\"file removed in the meanwhile\"",
               fsid, fid);
      return true;
    }

    if (fmd) {
      delete fmd;
    }

    // Get/create a record
    fmd = LocalGetFmd(fMd.fid(), fsid, fMd.uid(), fMd.gid(), fMd.lid(),
                      true, true);

    if (fmd) {
      if (!UpdateFromMgm(fsid, fMd.fid(), fMd.cid(), fMd.lid(), fMd.mgmsize(),
                         fMd.mgmchecksum(), fMd.uid(), fMd.gid(), fMd.ctime(),
                         fMd.ctime_ns(), fMd.mtime(), fMd.mtime_ns(),
                         fMd.layouterror(), fMd.locations())) {
        eos_err("failed to update fmd for fxid=%08llx", fid);
        delete fmd;
        return false;
      }

      // Check if it exists on disk
      if (fmd->mProtoFmd.disksize() == 0xfffffffffff1ULL) {
        fMd.set_layouterror(fMd.layouterror() | LayoutId::kMissing);
        eos_warning("found missing replica for fxid=%08llx on fsid=%lu", fid,
                    (unsigned long) fsid);
      }

      // Check if it exists on disk and at the mgm
      if ((fmd->mProtoFmd.disksize() == 0xfffffffffff1ULL) &&
          (fmd->mProtoFmd.mgmsize() == 0xfffffffffff1ULL)) {
        // There is no replica supposed to be here and there is nothing on
        // disk, so remove it from the database
        eos_warning("removing <ghost> entry for fxid=%08llx on fsid=%lu", fid,
                    (unsigned long) fsid);
        delete fmd;
        return LocalDeleteFmd(fMd.fid(), fsid);
      }

      delete fmd;
    } else {
      eos_err("failed to create fmd for fxid=%08llx", fid);
      return false;
    }
  } else {
    eos_err("failed to retrieve MGM fmd for fxid=%08llx", fid);
    return false;
  }

  return true;
}

//------------------------------------------------------------------------------
// Resync all meta data from MGM into local database
//------------------------------------------------------------------------------
bool
FmdDbMapHandler::ResyncAllMgm(eos::common::FileSystem::fsid_t fsid,
                              const char* manager)
{
  if (!ResetMgmInformation(fsid)) {
    eos_err("failed to reset the mgm information before resyncing");
    return false;
  }

  std::string tmpfile;

  if (!ExecuteDumpmd(manager, fsid, tmpfile)) {
    return false;
  }

  // Parse the result and unlink temporary file
  std::ifstream inFile(tmpfile);
  std::string dumpentry;
  unlink(tmpfile.c_str());
  unsigned long long cnt = 0;

  while (std::getline(inFile, dumpentry)) {
    cnt++;
    eos_debug("line=%s", dumpentry.c_str());
    std::unique_ptr<XrdOucEnv> env(new XrdOucEnv(dumpentry.c_str()));

    if (env) {
      struct Fmd fMd;
      FmdHelper::Reset(fMd);

      if (EnvMgmToFmd(*env, fMd)) {
        // get/create one
        FmdHelper* fmd = LocalGetFmd(fMd.fid(), fsid, fMd.uid(), fMd.gid(),
                                     fMd.lid(), true, true);
        fMd.set_layouterror(FmdHelper::LayoutError(fMd, fsid));

        if (fmd) {
          // Check if it exists on disk
          if (fmd->mProtoFmd.disksize() == 0xfffffffffff1ULL) {
            fMd.set_layouterror(fMd.layouterror() | LayoutId::kMissing);
            eos_warning("found missing replica for fxid=%08llx on fsid=%lu", fMd.fid(),
                        (unsigned long) fsid);
          }

          if (!UpdateWithMgmInfo(fsid, fMd.fid(), fMd.cid(), fMd.lid(),
                                 fMd.mgmsize(), fMd.mgmchecksum(), fMd.uid(),
                                 fMd.gid(), fMd.ctime(), fMd.ctime_ns(),
                                 fMd.mtime(), fMd.mtime_ns(),
                                 fMd.layouterror(), fMd.locations())) {
            eos_err("failed to update fmd %s", dumpentry.c_str());
          }

          delete fmd;
        } else {
          eos_err("failed to get/create fmd %s", dumpentry.c_str());
        }
      } else {
        eos_err("failed to convert %s", dumpentry.c_str());
      }
    }

    if (!(cnt % 10000)) {
      eos_info("msg=\"synced files so far\" nfiles=%llu fsid=%lu", cnt,
               (unsigned long) fsid);
    }
  }

  mIsSyncing[fsid] = false;
  return true;
}

//------------------------------------------------------------------------------
// Resync all meta data from QuarkdDB
//------------------------------------------------------------------------------
bool
FmdDbMapHandler::ResyncAllFromQdb(const QdbContactDetails& contactDetails,
                                  eos::common::FileSystem::fsid_t fsid)
{
  using namespace std::chrono;

  if (!ResetMgmInformation(fsid)) {
    eos_err("failed to reset the mgm information before resyncing");
    return false;
  }

  // Collect all file ids on the desired file system
  std::string cursor = "0";
  long long count = 250000;
  std::pair<std::string, std::vector<std::string>> reply;
  std::unique_ptr<qclient::QClient>
  qcl(new qclient::QClient(contactDetails.members,
                           contactDetails.constructOptions()));
  qclient::QSet qset(*qcl.get(),  eos::RequestBuilder::keyFilesystemFiles(fsid));
  std::unordered_set<eos::IFileMD::id_t> file_ids;

  try {
    do {
      reply = qset.sscan(cursor, count);
      cursor = reply.first;

      for (const auto& elem : reply.second) {
        file_ids.insert(std::stoull(elem));
      }
    } while (cursor != "0");
  } catch (const std::runtime_error& e) {
    // It means there are no records for the current file system
  }

  auto start = steady_clock::now();
  uint64_t total = file_ids.size();
  eos_info("resyncing %llu files for file_system %u", total, fsid);
  uint64_t num_files = 0;
  auto it = file_ids.begin();
  std::list<folly::Future<eos::ns::FileMdProto>> files;

  // Pre-fetch the first 1000 files
  while ((it != file_ids.end()) && (num_files < 1000)) {
    ++num_files;
    files.emplace_back(MetadataFetcher::getFileFromId(*qcl.get(),
                       FileIdentifier(*it)));
    ++it;
  }

  while (!files.empty()) {
    struct Fmd ns_fmd;
    FmdHelper::Reset(ns_fmd);

    try {
      NsFileProtoToFmd(files.front().get(), ns_fmd);
    } catch (const eos::MDException& e) {
      eos_err("msg=\"failed to get metadata from QuarkDB: %s\"", e.what());
      files.pop_front();
      continue;
    }

    files.pop_front();
    FmdHelper* local_fmd = LocalGetFmd(ns_fmd.fid(), fsid, ns_fmd.uid(),
                                       ns_fmd.gid(), ns_fmd.lid(), true, true);
    ns_fmd.set_layouterror(FmdHelper::LayoutError(ns_fmd, fsid));

    if (local_fmd) {
      // Check if it exists on disk
      if (local_fmd->mProtoFmd.disksize() == 0xfffffffffff1ULL) {
        ns_fmd.set_layouterror(ns_fmd.layouterror() | LayoutId::kMissing);
        eos_warning("found missing replica for fxid=%08llx on fsid=%lu",
                    ns_fmd.fid(), (unsigned long) fsid);
      }

      if (!UpdateWithMgmInfo(fsid, ns_fmd.fid(), ns_fmd.cid(), ns_fmd.lid(),
                             ns_fmd.mgmsize(), ns_fmd.mgmchecksum(), ns_fmd.uid(),
                             ns_fmd.gid(), ns_fmd.ctime(), ns_fmd.ctime_ns(),
                             ns_fmd.mtime(), ns_fmd.mtime_ns(),
                             ns_fmd.layouterror(), ns_fmd.locations())) {
        eos_err("failed to update fid %llu", ns_fmd.fid());
      }

      delete local_fmd;
    } else {
      eos_err("failed to get/create local fid %llu", ns_fmd.fid());
    }

    if (it != file_ids.end()) {
      ++num_files;
      files.emplace_back(MetadataFetcher::getFileFromId(*qcl.get(),
                         FileIdentifier(*it)));
      ++it;
    }

    if (num_files % 10000 == 0) {
      double rate = 0;
      auto duration = steady_clock::now() - start;
      auto ms = duration_cast<milliseconds>(duration);

      if (ms.count()) {
        rate = (num_files * 1000.0) / (double)ms.count();
      }

      eos_info("fsid=%u resynced %llu/%llu files at a rate of %.2f Hz",
               fsid, num_files, total, rate);
    }
  }

  double rate = 0;
  auto duration = steady_clock::now() - start;
  auto ms = duration_cast<milliseconds>(duration);

  if (ms.count()) {
    rate = (num_files * 1000.0) / (double)ms.count();
  }

  eos_info("fsid=%u resynced %llu/%llu files at a rate of %.2f Hz",
           fsid, num_files, total, rate);
  return true;
}

//------------------------------------------------------------------------------
// Remove ghost entries - entries which are neither on disk nor at the MGM
//------------------------------------------------------------------------------
bool
FmdDbMapHandler::RemoveGhostEntries(const char* path,
                                    eos::common::FileSystem::fsid_t fsid)
{
  bool rc = true;
  eos_static_info("");
  eos::common::FileId::fileid_t fid;
  std::vector<eos::common::FileId::fileid_t> to_delete;

  if (!IsSyncing(fsid)) {
    {
      eos::common::RWMutexReadLock rd_lock(mMapMutex);
      FsReadLock fs_rd_lock(fsid);

      if (!mDbMap.count(fsid)) {
        return true;
      }

      const eos::common::DbMapTypes::Tkey* k;
      const eos::common::DbMapTypes::Tval* v;
      eos::common::DbMap* db_map = mDbMap.find(fsid)->second;
      eos_static_info("msg=\"verifying %d entries on fsid=%lu\"",
                      db_map->size(), fsid);

      // Report values only when we are not in the sync phase from disk/mgm
      for (db_map->beginIter(false); db_map->iterate(&k, &v, false);) {
        Fmd f;
        f.ParseFromString(v->value);
        (void)memcpy(&fid, (void*)k->data(), k->size());

        if (f.layouterror()) {
          int rc = 0;
          struct stat buf;
          XrdOucString fstPath;
          const std::string hex_fid = eos::common::FileId::Fid2Hex(fid);
          eos::common::FileId::FidPrefix2FullPath(hex_fid.c_str(), path, fstPath);

          if ((rc = stat(fstPath.c_str(), &buf))) {
            if ((errno == ENOENT) || (errno == ENOTDIR)) {
              if ((f.layouterror() & LayoutId::kOrphan) ||
                  (f.layouterror() & LayoutId::kUnregistered)) {
                eos_static_info("msg=\"push back for deletion fxid=%08llx\"", fid);
                to_delete.push_back(fid);
              }
            }
          }

          eos_static_info("msg=\"stat %s rc=%d errno=%d\"",
                          fstPath.c_str(), rc, errno);
        }
      }
    }

    // Delete ghost entries from local database
    for (const auto& id : to_delete) {
      if (LocalDeleteFmd(id, fsid)) {
        eos_static_info("msg=\"removed FMD ghost entry fxid=%08llx fsid=%d\"",
                        id, fsid);
      } else {
        eos_static_err("msg=\"failed to removed FMD ghost entry fxid=%08llx "
                       "fsid=%d\"", id, fsid);
      }
    }
  } else {
    rc = false;
  }

  return rc;
}

//------------------------------------------------------------------------------
// Get inconsitency statistics
//------------------------------------------------------------------------------
bool
FmdDbMapHandler::GetInconsistencyStatistics(eos::common::FileSystem::fsid_t
    fsid,
    std::map<std::string, size_t>& statistics,
    std::map<std::string, std::set < eos::common::FileId::fileid_t> >& fidset)
{
  eos::common::RWMutexReadLock lock(mMapMutex);

  if (!mDbMap.count(fsid)) {
    return false;
  }

  // query in-memory
  statistics["mem_n"] = 0; // number of files in DB
  statistics["d_sync_n"] = 0; // number of synced files from disk
  statistics["m_sync_n"] = 0; // number of synced files from MGM server
  // number of files with disk and reference size mismatch
  statistics["d_mem_sz_diff"] = 0;
  // number of files with MGM and reference size mismatch
  statistics["m_mem_sz_diff"] = 0;
  // number of files with disk and reference checksum mismatch
  statistics["d_cx_diff"] = 0;
  // number of files with MGM and reference checksum mismatch
  statistics["m_cx_diff"] = 0;
  statistics["orphans_n"] = 0; // number of orphaned replicas
  statistics["unreg_n"] = 0; // number of unregistered replicas
  statistics["rep_diff_n"] = 0; // number of files with replica number mismatch
  statistics["rep_missing_n"] = 0; // number of files which are missing on disk
  fidset["m_mem_sz_diff"].clear();
  fidset["d_mem_sz_diff"].clear();
  fidset["m_cx_diff"].clear();
  fidset["d_cx_diff"].clear();
  fidset["orphans_n"].clear();
  fidset["unreg_n"].clear();
  fidset["rep_diff_n"].clear();
  fidset["rep_missing_n"].clear();

  if (!IsSyncing(fsid)) {
    const eos::common::DbMapTypes::Tkey* k;
    const eos::common::DbMapTypes::Tval* v;
    eos::common::DbMapTypes::Tval val;

    // We report values only when we are not in the sync phase from disk/mgm
    for (mDbMap[fsid]->beginIter(false); mDbMap[fsid]->iterate(&k, &v, false);) {
      Fmd f;
      f.ParseFromString(v->value);

      if (f.layouterror()) {
        if (f.layouterror() & LayoutId::kOrphan) {
          statistics["orphans_n"]++;
          fidset["orphans_n"].insert(f.fid());
        }

        if (f.layouterror() & LayoutId::kUnregistered) {
          statistics["unreg_n"]++;
          fidset["unreg_n"].insert(f.fid());
        }

        if (f.layouterror() & LayoutId::kReplicaWrong) {
          statistics["rep_diff_n"]++;
          fidset["rep_diff_n"].insert(f.fid());
        }

        if (f.layouterror() & LayoutId::kMissing) {
          statistics["rep_missing_n"]++;
          fidset["rep_missing_n"].insert(f.fid());
        }
      }

      if (f.mgmsize() != 0xfffffffffff1ULL) {
        statistics["m_sync_n"]++;

        if (f.size() != 0xfffffffffff1ULL) {
          if (f.size() != f.mgmsize()) {
            statistics["m_mem_sz_diff"]++;
            fidset["m_mem_sz_diff"].insert(f.fid());
          }
        }
      }

      if (!f.layouterror()) {
        if (f.size() && f.diskchecksum().length() &&
            (f.diskchecksum() != f.checksum())) {
          statistics["d_cx_diff"]++;
          fidset["d_cx_diff"].insert(f.fid());
        }

        if (f.size() && f.mgmchecksum().length() && (f.mgmchecksum() != f.checksum())) {
          statistics["m_cx_diff"]++;
          fidset["m_cx_diff"].insert(f.fid());
        }
      }

      statistics["mem_n"]++;

      if (f.disksize() != 0xfffffffffff1ULL) {
        statistics["d_sync_n"]++;

        if (f.size() != 0xfffffffffff1ULL) {
          // Report missmatch only for replica layout files
          if ((f.size() != f.disksize()) &&
              (eos::common::LayoutId::GetLayoutType(f.lid())
               == eos::common::LayoutId::kReplica)) {
            statistics["d_mem_sz_diff"]++;
            fidset["d_mem_sz_diff"].insert(f.fid());
          }
        }
      }
    }
  }

  return true;
}

//------------------------------------------------------------------------------
// Reset (clear) the contents of the DB
//------------------------------------------------------------------------------
bool
FmdDbMapHandler::ResetDB(eos::common::FileSystem::fsid_t fsid)
{
  bool rc = true;
  eos::common::RWMutexWriteLock lock(mMapMutex);

  // Erase the hash entry
  if (mDbMap.count(fsid)) {
    FsWriteLock fs_wr_lock(fsid);

    // Delete in the in-memory hash
    if (!mDbMap[fsid]->clear()) {
      eos_err("unable to delete all from fst table");
      rc = false;
    } else {
      rc = true;
    }
  } else {
    rc = false;
  }

  return rc;
}

//------------------------------------------------------------------------------
// Trim DB
//------------------------------------------------------------------------------
bool
FmdDbMapHandler::TrimDB()
{
  for (auto it = mDbMap.begin(); it != mDbMap.end(); ++it) {
    eos_static_info("Trimming fsid=%llu ", it->first);

    if (!it->second->trimDb()) {
      eos_static_err("Cannot trim the DB file for fsid=%llu ", it->first);
      return false;
    } else {
      eos_static_info("Trimmed %s DB file for fsid=%llu ",
                      it->second->getDbType().c_str(), it->first);
    }
  }

  return true;
}

//------------------------------------------------------------------------------
// Get number of flies on file system
//------------------------------------------------------------------------------
long long
FmdDbMapHandler::GetNumFiles(eos::common::FileSystem::fsid_t fsid)
{
  eos::common::RWMutexReadLock lock(mMapMutex);
  FsReadLock fs_rd_lock(fsid);

  if (mDbMap.count(fsid)) {
    return mDbMap[fsid]->size();
  } else {
    return 0ll;
  }
}

//------------------------------------------------------------------------------
// Execute "fs dumpmd" on the MGM node
//------------------------------------------------------------------------------
bool
FmdDbMapHandler::ExecuteDumpmd(const std::string& mgm_host,
                               eos::common::FileSystem::fsid_t fsid,
                               std::string& fn_output)
{
  // Create temporary file used as output for the command
  char tmpfile[] = "/tmp/efstd.XXXXXX";
  int tmp_fd = mkstemp(tmpfile);

  if (tmp_fd == -1) {
    eos_static_err("failed to create a temporary file");
    return false;
  }

  (void) close(tmp_fd);
  fn_output = tmpfile;
  std::ostringstream cmd;
  // First try to do the dumpmd using protobuf requests
  using eos::console::FsProto_DumpMdProto;
  eos::console::RequestProto request;
  eos::console::FsProto* fs = request.mutable_fs();
  FsProto_DumpMdProto* dumpmd = fs->mutable_dumpmd();
  dumpmd->set_fsid(fsid);
  dumpmd->set_display(eos::console::FsProto::DumpMdProto::MONITOR);
  request.set_format(eos::console::RequestProto::FUSE);
  std::string b64buff;

  if (eos::common::SymKey::ProtobufBase64Encode(&request, b64buff)) {
    // Increase the request timeout to 4 hours
    cmd << "env XrdSecPROTOCOL=sss XRD_REQUESTTIMEOUT=14400 "
        << "xrdcp -f -s \"root://" << mgm_host.c_str() << "/"
        << "/proc/admin/?mgm.cmd.proto=" << b64buff << "\" "
        << tmpfile;
    eos::common::ShellCmd bootcmd(cmd.str().c_str());
    eos::common::cmd_status rc = bootcmd.wait(1800);

    if (rc.exit_code) {
      eos_static_err("%s returned %d", cmd.str().c_str(), rc.exit_code);
    } else {
      eos_static_debug("%s executed successfully", cmd.str().c_str());
      return true;
    }
  } else {
    eos_static_err("msg=\"failed to serialize protobuf request for dumpmd\"");
  }

  eos_static_info("msg=\"falling back to classic dumpmd command\"");
  cmd.str("");
  cmd.clear();
  cmd << "env XrdSecPROTOCOL=sss XRD_STREAMTIMEOUT=600 xrdcp -f -s \""
      << "root://" << mgm_host.c_str() << "/"
      << "/proc/admin/?&mgm.format=fuse&mgm.cmd=fs&mgm.subcmd=dumpmd&"
      << "mgm.dumpmd.option=m&mgm.fsid=" << fsid << "\" "
      << tmpfile;
  eos::common::ShellCmd bootcmd(cmd.str().c_str());
  eos::common::cmd_status rc = bootcmd.wait(1800);

  if (rc.exit_code) {
    eos_static_err("%s returned %d", cmd.str().c_str(), rc.exit_code);
    return false;
  } else {
    eos_static_debug("%s executed successfully", cmd.str().c_str());
    return true;
  }
}

EOSFSTNAMESPACE_END
