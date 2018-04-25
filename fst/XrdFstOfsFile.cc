//------------------------------------------------------------------------------
// File: XrdFstOfsFile.cc
// Author: Elvin-Alin Sindrilaru / Andreas-Joachim Peters - CERN
//------------------------------------------------------------------------------

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
 * but WITHOUT ANY WARRANTY; without even the implied waDon'trranty of  *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/
#define __STDC_FORMAT_MACROS
#include <cinttypes>
#include "common/Path.hh"
#include "common/http/OwnCloud.hh"
#include "common/StringTokenizer.hh"
#include "common/SecEntity.hh"
#include "fst/FmdDbMap.hh"
#include "fst/XrdFstOfsFile.hh"
#include "fst/XrdFstOfs.hh"
#include "fst/layout/Layout.hh"
#include "fst/layout/LayoutPlugin.hh"
#include "fst/checksum/ChecksumPlugins.hh"
#include "fst/storage/FileSystem.hh"
#include "authz/XrdCapability.hh"
#include "XrdOss/XrdOssApi.hh"
#include "fst/io/FileIoPluginCommon.hh"

extern XrdOssSys* XrdOfsOss;

EOSFSTNAMESPACE_BEGIN

const uint16_t XrdFstOfsFile::msDefaultTimeout = 300; // default timeout value

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
XrdFstOfsFile::XrdFstOfsFile(const char* user, int MonID) :
  XrdOfsFile(user, MonID),
  eos::common::LogId(),
  mTpcThreadStatus(EINVAL),
  mTpcRetc(0)
{
  openOpaque = 0;
  capOpaque = 0;
  fstPath = "";
  hasBlockXs = false;
  closed = false;
  opened = false;
  haswrite = false;
  hasReadError = false;
  hasWriteError = false;
  fMd = 0;
  checkSum = 0;
  layOut = 0;
  isRW = 0;
  isCreation = 0;
  commitReconstruction = 0;
  rBytes = wBytes = sFwdBytes = sBwdBytes = sXlFwdBytes = sXlBwdBytes = rOffset =
                                  wOffset = 0;
  rTime.tv_sec = lrTime.tv_sec = rvTime.tv_sec = lrvTime.tv_sec = 0;
  rTime.tv_usec = lrTime.tv_usec = rvTime.tv_usec = lrvTime.tv_usec = 0;
  wTime.tv_sec = lwTime.tv_sec = cTime.tv_sec = 0;
  wTime.tv_usec = lwTime.tv_usec = cTime.tv_usec = 0;
  fileid = 0;
  fsid = 0;
  lid = 0;
  cid = 0;
  rCalls = wCalls = nFwdSeeks = nBwdSeeks = nXlFwdSeeks = nXlBwdSeeks = 0;
  localPrefix = "";
  maxOffsetWritten = 0;
  openSize = 0;
  closeSize = 0;
  isReplication = false;
  isInjection = false;
  isReconstruction = false;
  deleteOnClose = false;
  repairOnClose = false;
  eventOnClose = false;
  syncEventOnClose = false;
  closeTime.tv_sec = closeTime.tv_usec = 0;
  openTime.tv_sec = openTime.tv_usec = 0;
  tz.tz_dsttime = tz.tz_minuteswest = 0;
  viaDelete = remoteDelete = writeDelete = false;
  SecString = "";
  writeErrorFlag = 0;
  mTpcFlag = kTpcNone;
  mTpcState = kTpcIdle;
  ETag = "";
  mForcedMtime = 1;
  mForcedMtime_ms = 0;
  mFusex = false;
  isOCchunk = 0;
  mTimeout = getenv("EOS_FST_STREAM_TIMEOUT") ? strtoul(
               getenv("EOS_FST_STREAM_TIMEOUT"), 0, 10) : msDefaultTimeout;
}

//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
XrdFstOfsFile::~XrdFstOfsFile()
{
  viaDelete = true;

  if (!closed) {
    close();
  }

  if (openOpaque) {
    delete openOpaque;
    openOpaque = 0;
  }

  if (capOpaque) {
    delete capOpaque;
    capOpaque = 0;
  }

  // Unmap the MD record
  if (fMd) {
    delete fMd;
    fMd = 0;
  }

  if (checkSum) {
    delete checkSum;
    checkSum = 0;
  }

  if (layOut) {
    delete layOut;
    layOut = 0;
  }
}

//------------------------------------------------------------------------------
// Open - - low level ofs method which is called from one of the layout plugins
//------------------------------------------------------------------------------
int
XrdFstOfsFile::openofs(const char* path,
                       XrdSfsFileOpenMode open_mode,
                       mode_t create_mode,
                       const XrdSecEntity* client,
                       const char* opaque)
{
  int retc = 0;

  while ((retc = XrdOfsFile::open(path, open_mode, create_mode, client,
                                  opaque)) > 0) {
    eos_static_notice("msg\"xrootd-lock-table busy - snoozing & retry\" "
                      "delay=%d errno=%d", retc, errno);
    XrdSysTimer sleeper;
    sleeper.Snooze(retc);
  }

  return retc;
}

//------------------------------------------------------------------------------
// Drop all replicas from the MGM
//------------------------------------------------------------------------------
int
XrdFstOfsFile::dropall(eos::common::FileId::fileid_t fileid, std::string path,
                       std::string manager)
{
  // If we committed the replica and an error happened remote, we have
  // to unlink it again
  XrdOucString hexstring = "";
  eos::common::FileId::Fid2Hex(fileid, hexstring);
  XrdOucErrInfo error;
  XrdOucString capOpaqueString = "/?mgm.pcmd=drop";
  XrdOucString OpaqueString = "";
  OpaqueString += "&mgm.fid=";
  OpaqueString += hexstring;
  OpaqueString += "&mgm.fsid=anyway";
  OpaqueString += "&mgm.dropall=1";
  XrdOucEnv Opaque(OpaqueString.c_str());
  capOpaqueString += OpaqueString;
  // Delete the replica in the MGM
  int rcode = gOFS.CallManager(&error,
                               path.c_str(),
                               manager.c_str(),
                               capOpaqueString);

  if (rcode && (rcode != -EIDRM)) {
    eos_warning("(unpersist): unable to drop file id %s fsid %u at manager %s",
                hexstring.c_str(), fileid, manager.c_str());
  }

  eos_info("info=\"removing on manager\" manager=%s fid=%llu fsid= drop-allrc=%d",
           manager.c_str(), (unsigned long long) fileid,
           rcode);
  return rcode;
}

//------------------------------------------------------------------------------
// Open file
//------------------------------------------------------------------------------
int
XrdFstOfsFile::open(const char* path, XrdSfsFileOpenMode open_mode,
                    mode_t create_mode, const XrdSecEntity* client,
                    const char* opaque)
{
  EPNAME("open");
  const char* tident = error.getErrUser();
  tIdent = error.getErrUser();
  char* val = 0;
  isRW = false;
  int retc = SFS_OK;
  Path = path;
  gettimeofday(&openTime, &tz);
  XrdOucString stringOpaque = opaque;
  XrdOucString opaqueCheckSum = "";
  XrdOucString opaqueBlockCheckSum = "";
  std::string sec_protocol = client->prot;
  bool hasCreationMode = (open_mode & SFS_O_CREAT);
  bool isRepairRead = false;

  while (stringOpaque.replace("?", "&")) {}

  while (stringOpaque.replace("&&", "&")) {}

  //----------------------------------------------------------------------------
  {
    //@todo (esindril): This should be dropped after Sept 2018 since it's
    // just a temporary fix for an issue on the eos fuse.
    std::string filtered_opaque = FilterTags(stringOpaque.c_str(),
    {"xrdcl.secuid", "xrdcl.secgid"});
    stringOpaque = filtered_opaque.c_str();
  }
  //----------------------------------------------------------------------------
  int envlen;
  XrdOucString maskOpaque = opaque ? opaque : "";
  // mask some opaque parameters to shorten the logging
  eos::common::StringConversion::MaskTag(maskOpaque, "cap.sym");
  eos::common::StringConversion::MaskTag(maskOpaque, "cap.msg");
  eos::common::StringConversion::MaskTag(maskOpaque, "authz");
  // For RAIN layouts if the opaque information contains the tag fst.store=1 the
  // corrupted files are recovered back on disk. There is no other way to make
  // the distinction between an open for write and open for recovery
  store_recovery = false;
  XrdOucEnv tmpOpaque(stringOpaque.c_str());

  if (!CheckFstValidity(tmpOpaque)) {
    eos_err("msg=\"fst validity expired, avoid open replay\"");
    return gOFS.Emsg(epname, error, EINVAL, "open - fst validity expired",
                     Path.c_str());
  }

  if ((val = tmpOpaque.Get("fst.store"))) {
    if (strncmp(val, "1", 1) == 0) {
      store_recovery = true;
      open_mode = SFS_O_RDWR;
      eos_info("msg=\"enabling RAIN store recovery\"");
    }
  }

  if ((open_mode &
       (SFS_O_WRONLY | SFS_O_RDWR | SFS_O_CREAT | SFS_O_TRUNC)) != 0) {
    isRW = true;
  }

  // Set client identity
  SetLogId(0, client, tident);

  if ((val = tmpOpaque.Get("mgm.logid"))) {
    SetLogId(val, client, tident);
  }

  if ((val = tmpOpaque.Get("mgm.etag"))) {
    // extract our ETag from the redirection URL if available
    ETag = val;
  }

  if ((val = tmpOpaque.Get("mgm.mtime"))) {
    // mgm.mtime=0 we set the external mtime=0 and indicated during commit, that
    // it should not update the mtime as in case of a FUSE client which will call
    // utimes
    mForcedMtime = 0;
    mForcedMtime_ms = 0;
  }

  if ((val = tmpOpaque.Get("mgm.fusex"))) {
    // mgm.fusex=1 the commit function suppressed to broadcast the file close
    // to the fusex network
    mFusex = true;
  }

  if ((val = tmpOpaque.Get("mgm.event"))) {
    std::string event = val;

    if (event == "closew") {
      eventOnClose = true;
    } else if (event == "sync::closew") {
      syncEventOnClose = true;
    }

    val = tmpOpaque.Get("mgm.workflow");
    eventWorkflow = (val) ? val : "";
  }

  if (eos::common::OwnCloud::isChunkUpload(tmpOpaque)) {
    // tag as an OC chunk upload
    isOCchunk = true;
  }

  // Extract tpc keys
  eos_info("path=%s info=%s isRW=%d open_mode=%x", Path.c_str(),
           maskOpaque.c_str(), isRW, open_mode);
  std::string tpc_stage = tmpOpaque.Get("tpc.stage") ?
                          tmpOpaque.Get("tpc.stage") : "";
  std::string tpc_key = tmpOpaque.Get("tpc.key") ?
                        tmpOpaque.Get("tpc.key") : "";
  std::string tpc_src = tmpOpaque.Get("tpc.src") ?
                        tmpOpaque.Get("tpc.src") : "";
  std::string tpc_dst = tmpOpaque.Get("tpc.dst") ?
                        tmpOpaque.Get("tpc.dst") : "";
  std::string tpc_org = tmpOpaque.Get("tpc.org") ?
                        tmpOpaque.Get("tpc.org") : "";
  std::string tpc_lfn = tmpOpaque.Get("tpc.lfn") ?
                        tmpOpaque.Get("tpc.lfn") : "";

  // Determine the TPC step that we are in
  if (tpc_stage == "placement") {
    mTpcFlag = kTpcSrcCanDo;
  } else if ((tpc_stage == "copy") && tpc_key.length() && tpc_dst.length()) {
    mTpcFlag = kTpcSrcSetup;
  } else if ((tpc_stage == "copy") && tpc_key.length() && tpc_src.length()) {
    mTpcFlag = kTpcDstSetup;
  } else if (tpc_key.length() && tpc_org.length()) {
    // Notice:
    // XRootD does not full follow the TPC specification and it doesn't set the
    // tpc.stage=copy in the TpcSrcRead step. The above condition should be:
    // else if ((tpc_stage == "copy") && tpc_key.length() && tpc_org.length()) {
    mTpcFlag = kTpcSrcRead;
  }

  if ((mTpcFlag == kTpcSrcSetup) || (mTpcFlag == kTpcDstSetup)) {
    // Create a TPC entry in the TpcMap
    XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex);

    if (gOFS.TpcMap[isRW].count(tpc_key)) {
      return gOFS.Emsg(epname, error, EPERM, "open - tpc key replayed", path);
    }

    // Compute the tpc origin e.g. <name>:<pid>@<host.domain>
    // TODO: Xrootd 4.0
    // std::string origin_host = client->addrInfo->Name();
    std::string origin_host = client->host ? client->host : "<sss-auth>";
    std::string origin_tident = client->tident;
    origin_tident.erase(origin_tident.find(":"));
    tpc_org = origin_tident;
    tpc_org += "@";
    tpc_org += origin_host;
    // Store the TPC initialization
    gOFS.TpcMap[isRW][tpc_key].key = tpc_key;
    gOFS.TpcMap[isRW][tpc_key].org = tpc_org;
    gOFS.TpcMap[isRW][tpc_key].src = tpc_src;
    gOFS.TpcMap[isRW][tpc_key].dst = tpc_dst;
    gOFS.TpcMap[isRW][tpc_key].path = path;
    gOFS.TpcMap[isRW][tpc_key].lfn = tpc_lfn;
    gOFS.TpcMap[isRW][tpc_key].opaque = stringOpaque.c_str();
    gOFS.TpcMap[isRW][tpc_key].expires = time(NULL) + 60; // one minute that's fine
    TpcKey = tpc_key.c_str();

    if (mTpcFlag == kTpcDstSetup) {
      if (!tpc_lfn.length()) {
        return gOFS.Emsg(epname, error, EINVAL, "open - tpc lfn missing", path);
      }

      eos_info("msg=\"tpc dst session\" key=%s, org=%s, src=%s path=%s lfn=%s expires=%llu",
               gOFS.TpcMap[isRW][tpc_key].key.c_str(),
               gOFS.TpcMap[isRW][tpc_key].org.c_str(),
               gOFS.TpcMap[isRW][tpc_key].src.c_str(),
               gOFS.TpcMap[isRW][tpc_key].path.c_str(),
               gOFS.TpcMap[isRW][tpc_key].lfn.c_str(),
               gOFS.TpcMap[isRW][tpc_key].expires);
    } else if (mTpcFlag == kTpcSrcSetup) {
      // For a TpcSrcSetup we need to store the decoded capability contents
      XrdOucEnv* saveOpaque = 0;
      int caprc = gCapabilityEngine.Extract(&tmpOpaque, saveOpaque);

      if (caprc == ENOKEY) {
        delete saveOpaque;
        return gOFS.Emsg(epname, error, caprc, "open - missing capability");
      } else if (caprc != 0) {
        delete saveOpaque;
        return gOFS.Emsg(epname, error, caprc, "open - capability illegal",
                         Path.c_str());
      } else {
        gOFS.TpcMap[isRW][tpc_key].capability = saveOpaque->Env(envlen);
        delete saveOpaque;
      }

      eos_info("msg=\"tpc src session\" key=%s, org=%s, dst=%s path=%s expires=%llu",
               gOFS.TpcMap[isRW][tpc_key].key.c_str(),
               gOFS.TpcMap[isRW][tpc_key].org.c_str(),
               gOFS.TpcMap[isRW][tpc_key].dst.c_str(),
               gOFS.TpcMap[isRW][tpc_key].path.c_str(),
               gOFS.TpcMap[isRW][tpc_key].expires);
    }
  } else if (mTpcFlag == kTpcSrcRead) {
    // Verify a TPC entry in the TpcMap since the destination's open can now
    // come before the transfer has been setup we have to give some time for
    // the TPC client to deposit the key the not so nice side effect is that
    // this thread stays busy during that time
    bool exists = false;

    for (size_t i = 0; i < 150; ++i) {
      {
        // Briefly take lock and release it
        XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex);

        if (gOFS.TpcMap[isRW].count(tpc_key)) {
          exists = true;
          break;
        }
      }

      if (!exists) {
        XrdSysTimer timer;
        timer.Wait(100);
      }
    }

    XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex);
    time_t now = time(NULL);

    if (!gOFS.TpcMap[isRW].count(tpc_key)) {
      eos_err("tpc key=%s not valid", tpc_key.c_str());
      return gOFS.Emsg(epname, error, EPERM, "open - tpc key not valid", path);
    }

    if (gOFS.TpcMap[isRW][tpc_key].expires < now) {
      eos_err("tpc key=%s expired", tpc_key.c_str());
      return gOFS.Emsg(epname, error, EPERM, "open - tpc key expired", path);
    }

    // We trust 'sss' anyway and we miss the host name in the 'sss' entity
    if ((sec_protocol != "sss") && (gOFS.TpcMap[isRW][tpc_key].org != tpc_org)) {
      eos_err("tpc origin missmatch tpc_org=%s, cached_org=%s", tpc_org.c_str(),
              gOFS.TpcMap[isRW][tpc_key].org.c_str());
      return gOFS.Emsg(epname, error, EPERM, "open - tpc origin mismatch", path);
    }

    // Grab the open information and expire entry
    Path = gOFS.TpcMap[isRW][tpc_key].path.c_str();
    stringOpaque = gOFS.TpcMap[isRW][tpc_key].opaque.c_str();
    gOFS.TpcMap[isRW][tpc_key].expires = (now - 10);
    // Store the provided origin to compare with our local connection
    // gOFS.TpcMap[isRW][tpc_key].org = tpc_org;
    TpcKey = tpc_key.c_str();
    eos_info("msg=\"tpc read\" key=%s, org=%s, path=%s expires=%llu",
             gOFS.TpcMap[isRW][tpc_key].key.c_str(),
             gOFS.TpcMap[isRW][tpc_key].org.c_str(),
             gOFS.TpcMap[isRW][tpc_key].src.c_str(),
             gOFS.TpcMap[isRW][tpc_key].path.c_str(),
             gOFS.TpcMap[isRW][tpc_key].expires);
  }

  // Expire keys which are more than one 4 hours expired
  if (mTpcFlag > kTpcNone) {
    time_t now = time(NULL);
    XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex);
    auto it = (gOFS.TpcMap[isRW]).begin();
    auto del = (gOFS.TpcMap[isRW]).begin();

    while (it != (gOFS.TpcMap[isRW]).end()) {
      del = it;
      it++;

      if (now > (del->second.expires + (4 * 3600))) {
        eos_info("msg=\"expire tpc key\" key=%s", del->second.key.c_str());
        gOFS.TpcMap[isRW].erase(del);
      }
    }
  }

  stringOpaque += "&mgm.path=";
  stringOpaque += Path.c_str();
  openOpaque = new XrdOucEnv(stringOpaque.c_str());

  if ((val = openOpaque->Get("mgm.logid"))) {
    SetLogId(val, tident);
  }

  if ((val = openOpaque->Get("mgm.checksum"))) {
    opaqueCheckSum = val;
  }

  if ((val = openOpaque->Get("mgm.blockchecksum"))) {
    opaqueBlockCheckSum = val;
  }

  if ((val = openOpaque->Get("eos.injection"))) {
    isInjection = true;
  }

  int caprc = 0;

  // TpcSrcRead can bypass capability checks
  if (mTpcFlag == kTpcSrcRead) {
    // Grab the capability contents from the tpc key map
    XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex);

    if (gOFS.TpcMap[isRW][tpc_key].capability.length()) {
      if (capOpaque) {
        delete capOpaque;
      }

      capOpaque = new XrdOucEnv(gOFS.TpcMap[isRW][tpc_key].capability.c_str());
    } else {
      return gOFS.Emsg(epname, error, EINVAL, "open - capability not found "
                       "for tpc key %s", tpc_key.c_str());
    }
  } else {
    if ((caprc = gCapabilityEngine.Extract(openOpaque, capOpaque))) {
      if (caprc == ENOKEY) {
        // If we just miss the key, better stall the client
        return gOFS.Stall(error, 10, "FST still misses the required capability key");
      }

      // No capability - go away!
      return gOFS.Emsg(epname, error, caprc, "open - capability illegal",
                       Path.c_str());
    }
  }

  eos_info("capability=%s", capOpaque->Env(envlen));
  const char* hexfid = 0;
  const char* sfsid = 0;
  const char* slid = 0;
  const char* scid = 0;
  const char* smanager = 0;
  const char* sbookingsize = 0;
  const char* stargetsize = 0;
  bookingsize = 0;
  targetsize = 0;
  fileid = 0;
  fsid = 0;
  lid = 0;
  cid = 0;
  const char* secinfo = 0;

  if (!(hexfid = capOpaque->Get("mgm.fid"))) {
    return gOFS.Emsg(epname, error, EINVAL, "open - no file id in capability",
                     Path.c_str());
  }

  if (!(sfsid = capOpaque->Get("mgm.fsid"))) {
    return gOFS.Emsg(epname, error, EINVAL,
                     "open - no file system id in capability", Path.c_str());
  }

  if (!(secinfo = capOpaque->Get("mgm.sec"))) {
    return gOFS.Emsg(epname, error, EINVAL,
                     "open - no security information in capability", Path.c_str());
  } else {
    SecString = secinfo;
  }

  if ((val = capOpaque->Get("mgm.minsize"))) {
    errno = 0;
    minsize = strtoull(val, 0, 10);

    if (errno) {
      eos_err("illegal minimum file size specified <%s>- restricting to 1 byte", val);
      minsize = 1;
    }
  } else {
    minsize = 0;
  }

  if ((val = capOpaque->Get("mgm.maxsize"))) {
    errno = 0;
    maxsize = strtoull(val, 0, 10);

    if (errno) {
      eos_err("illegal maximum file size specified <%s>- restricting to 1 byte", val);
      maxsize = 1;
    }
  } else {
    maxsize = 0;
  }

  if ((val = capOpaque->Get("mgm.repairread"))) {
    isRepairRead = true;
  }

  if ((val = openOpaque->Get("eos.pio.action"))) {
    // figure out if this is a RAIN reconstruction
    XrdOucString action = val;

    if (action == "reconstruct") {
      haswrite = true;
      isReconstruction = true;
    }
  }

  // If we open a replica we have to take the right filesystem id and filesystem
  // prefix for that replica
  if (openOpaque->Get("mgm.replicaindex")) {
    XrdOucString replicafsidtag = "mgm.fsid";
    replicafsidtag += (int) atoi(openOpaque->Get("mgm.replicaindex"));

    if (capOpaque->Get(replicafsidtag.c_str())) {
      sfsid = capOpaque->Get(replicafsidtag.c_str());
    }
  }

  // Extract the local path prefix from the broadcasted configuration!
  if (openOpaque->Get("mgm.fsprefix")) {
    localPrefix = openOpaque->Get("mgm.fsprefix");
    localPrefix.replace("#COL#", ":");
  } else {
    // Extract the local path prefix from the broadcasted configuration!
    eos::common::RWMutexReadLock lock(gOFS.Storage->mFsMutex);
    fsid = atoi(sfsid ? sfsid : "0");

    if (fsid && gOFS.Storage->mFileSystemsMap.count(fsid)) {
      localPrefix = gOFS.Storage->mFileSystemsMap[fsid]->GetPath().c_str();
    }
  }

  // Attention: the localprefix implementation does not work for gateway machines
  if (!localPrefix.length()) {
    return gOFS.Emsg(epname, error, EINVAL, "open - cannot determine the prefix"
                     " path to use for the given filesystem id", Path.c_str());
  }

  if (!(slid = capOpaque->Get("mgm.lid"))) {
    return gOFS.Emsg(epname, error, EINVAL, "open - no layout id in capability",
                     Path.c_str());
  }

  if (!(scid = capOpaque->Get("mgm.cid"))) {
    return gOFS.Emsg(epname, error, EINVAL, "open - no container id in capability",
                     Path.c_str());
  }

  if (!(smanager = capOpaque->Get("mgm.manager"))) {
    return gOFS.Emsg(epname, error, EINVAL, "open - no manager name in capability",
                     Path.c_str());
  }

  RedirectManager = smanager;
  int dpos = RedirectManager.find(":");

  if (dpos != STR_NPOS) {
    RedirectManager.erase(dpos);
  }

  {
    // evt. update the shared hash manager entry
    XrdSysMutexHelper lock(eos::fst::Config::gConfig.Mutex);
    XrdOucString ConfigManager = eos::fst::Config::gConfig.Manager;

    if (ConfigManager != RedirectManager) {
      eos_warning("msg=\"MGM master seems to have changed - adjusting global "
                  "config\" old-manager=\"%s\" new-manager=\"%s\"",
                  ConfigManager.c_str(), RedirectManager.c_str());
      eos::fst::Config::gConfig.Manager = RedirectManager;
    }
  }

  std::string RedirectTried = RedirectManager.c_str();
  RedirectTried += "?tried=";

  if ((val = openOpaque->Get("tried"))) {
    RedirectTried += openOpaque->Get("tried");
    RedirectTried += ",";
  }

  RedirectTried += gOFS.mHostName;
  eos::common::FileId::FidPrefix2FullPath(hexfid, localPrefix.c_str(), fstPath);
  fileid = eos::common::FileId::Hex2Fid(hexfid);
  fsid = atoi(sfsid);
  lid = (unsigned long)atoi(slid);
  cid = strtoull(scid, 0, 10);
  eos_info("Path=%s beginswith=%d", Path.c_str(), Path.beginswith("/replicate:"));

  if (Path.beginswith("/replicate:")) {
    bool isopenforwrite = false;
    {
      XrdSysMutexHelper scope_lock(gOFS.OpenFidMutex);

      if (gOFS.WOpenFid[fsid].count(fileid)) {
        if (gOFS.WOpenFid[fsid][fileid] > 0) {
          isopenforwrite = true;
        }
      }
    }

    if (isopenforwrite) {
      eos_err("forbid to open replica - file %s is opened in RW mode", Path.c_str());
      return gOFS.Emsg(epname, error, ENOENT,
                       "open - cannot replicate: file is opened in RW mode", Path.c_str());
    }

    isReplication = true;
  }

  // Check if this is an open for HTTP
  if ((!isRW) && ((std::string(client->tident) == "http"))) {
    bool isopenforwrite = false;
    {
      XrdSysMutexHelper scope_lock(gOFS.OpenFidMutex);

      if (gOFS.WOpenFid[fsid].count(fileid)) {
        if (gOFS.WOpenFid[fsid][fileid] > 0) {
          isopenforwrite = true;
        }
      }
    }

    if (isopenforwrite) {
      eos_err("forbid to open replica for synchronization - file %s is opened "
              "in RW mode", Path.c_str());
      return gOFS.Emsg(epname, error, ETXTBSY, "open - cannot synchronize this "
                       "file: file is opened in RW mode", Path.c_str());
    }
  }

  if ((val = capOpaque->Get("mgm.logid"))) {
    snprintf(logId, sizeof(logId) - 1, "%s", val);
  }

  SetLogId(logId, vid, tident);
  eos_info("fstpath=%s", fstPath.c_str());
  // Get the layout object
  layOut = eos::fst::LayoutPlugin::GetLayoutObject
           (this, lid, client, &error, fstPath.c_str(), msDefaultTimeout,
            store_recovery);

  if (!layOut) {
    int envlen;
    eos_err("unable to handle layout for %s", capOpaque->Env(envlen));
    delete fMd;
    return gOFS.Emsg(epname, error, EINVAL, "open - illegal layout specified ",
                     capOpaque->Env(envlen));
  }

  layOut->SetLogId(logId, vid, tident);
  errno = 0;

  if ((retc = layOut->GetFileIo()->fileExists())) {
    // We have to distinguish if an Exists call fails or return ENOENT, otherwise
    // we might trigger an automatic clean-up of a file !!!
    if (errno != ENOENT) {
      delete fMd;
      return gOFS.Emsg(epname, error, EIO, "open - unable to check for existance"
                       " of file ", capOpaque->Env(envlen));
    }

    if (isRW || (capOpaque->Get("mgm.zerosize"))) {
      // File does not exist, keep the create flag for writers and readers with 0-size at MGM
      isCreation = true;
      openSize = 0;
      // Used to indicate if a file was written in the meanwhile by someone else
      updateStat.st_mtime = 0;
      open_mode |= SFS_O_CREAT;
      create_mode |= SFS_O_MKPTH;
      eos_debug("adding creation flag because of %d %d", retc, errno);
    } else {
      // The open will fail but the client will get a recoverable error,
      // therefore it will try to read again from the other replicas.
      eos_warning("open for read, local file does not exists");
    }
  } else {
    eos_debug("removing creation flag because of %d %d", retc, errno);

    // Remove the creat flag
    if (open_mode & SFS_O_CREAT) {
      open_mode -= SFS_O_CREAT;
    }
  }

  // Capability access distinction
  if (isRW) {
    if (isCreation) {
      if (!capOpaque->Get("mgm.access")
          || ((strcmp(capOpaque->Get("mgm.access"), "create")) &&
              (strcmp(capOpaque->Get("mgm.access"), "write")) &&
              (strcmp(capOpaque->Get("mgm.access"), "update")))) {
        return gOFS.Emsg(epname, error, EPERM, "open - capability does not "
                         "allow to create/write/update this file", path);
      }
    } else {
      if (!capOpaque->Get("mgm.access")
          || ((strcmp(capOpaque->Get("mgm.access"), "create")) &&
              (strcmp(capOpaque->Get("mgm.access"), "write")) &&
              (strcmp(capOpaque->Get("mgm.access"), "update")))) {
        return gOFS.Emsg(epname, error, EPERM, "open - capability does not "
                         "allow to update/write/create this file", path);
      }
    }
  } else {
    if (!capOpaque->Get("mgm.access")
        || ((strcmp(capOpaque->Get("mgm.access"), "read")) &&
            (strcmp(capOpaque->Get("mgm.access"), "create")) &&
            (strcmp(capOpaque->Get("mgm.access"), "write")) &&
            (strcmp(capOpaque->Get("mgm.access"), "update")))) {
      return gOFS.Emsg(epname, error, EPERM, "open - capability does not allow "
                       "to read this file", path);
    }
  }

  // Bookingsize is only needed for file creation
  if (isRW && isCreation) {
    if (!(sbookingsize = capOpaque->Get("mgm.bookingsize"))) {
      return gOFS.Emsg(epname, error, EINVAL, "open - no booking size in capability",
                       Path.c_str());
    } else {
      bookingsize = strtoull(capOpaque->Get("mgm.bookingsize"), 0, 10);

      if (errno == ERANGE) {
        eos_err("invalid bookingsize in capability bookingsize=%s", sbookingsize);
        return gOFS.Emsg(epname, error, EINVAL,
                         "open - invalid bookingsize in capability", Path.c_str());
      }
    }

    if ((stargetsize = capOpaque->Get("mgm.targetsize"))) {
      targetsize = strtoull(capOpaque->Get("mgm.targetsize"), 0, 10);

      if (errno == ERANGE) {
        eos_err("invalid targetsize in capability targetsize=%s", stargetsize);
        return gOFS.Emsg(epname, error, EINVAL,
                         "open - invalid targetsize in capability", Path.c_str());
      }
    }
  }

  // Check if the booking size violates the min/max-size criteria
  if (bookingsize && maxsize) {
    if (bookingsize > maxsize) {
      eos_err("invalid bookingsize specified - violates maximum file size criteria");
      return gOFS.Emsg(epname, error, ENOSPC,
                       "open - bookingsize violates maximum allowed filesize", Path.c_str());
    }
  }

  if (bookingsize && minsize) {
    if (bookingsize < minsize) {
      eos_err("invalid bookingsize specified - violates minimum file size criteria");
      return gOFS.Emsg(epname, error, ENOSPC,
                       "open - bookingsize violates minimum allowed filesize", Path.c_str());
    }
  }

  // Get the identity
  eos::common::Mapping::VirtualIdentity vid;
  eos::common::Mapping::Nobody(vid);

  if ((val = capOpaque->Get("mgm.ruid"))) {
    vid.uid = atoi(val);
  } else {
    return gOFS.Emsg(epname, error, EINVAL, "open - sec ruid missing",
                     Path.c_str());
  }

  if ((val = capOpaque->Get("mgm.rgid"))) {
    vid.gid = atoi(val);
  } else {
    return gOFS.Emsg(epname, error, EINVAL, "open - sec rgid missing",
                     Path.c_str());
  }

  if ((val = capOpaque->Get("mgm.uid"))) {
    vid.uid_list.clear();
    vid.uid_list.push_back(atoi(val));
  } else {
    return gOFS.Emsg(epname, error, EINVAL, "open - sec uid missing", Path.c_str());
  }

  if ((val = capOpaque->Get("mgm.gid"))) {
    vid.gid_list.clear();
    vid.gid_list.push_back(atoi(val));
  } else {
    return gOFS.Emsg(epname, error, EINVAL, "open - sec gid missing", Path.c_str());
  }

  SetLogId(logId, vid, tident);
  eos_info("fstpath=%s", fstPath.c_str());
  fMd = gFmdDbMapHandler.LocalGetFmd(fileid, fsid, vid.uid, vid.gid, lid, isRW,
                                     isRepairRead);

  if ((!fMd) || gOFS.Simulate_FMD_open_error) {
    if (!gOFS.Simulate_FMD_open_error) {
      // Get the layout object
      if (gFmdDbMapHandler.ResyncMgm(fsid, fileid, RedirectManager.c_str())) {
        eos_info("msg=\"resync ok\" fsid=%lu fid=%llx", (unsigned long) fsid, fileid);
        fMd = gFmdDbMapHandler.LocalGetFmd(fileid, fsid, vid.uid, vid.gid, lid, isRW);
      } else {
        eos_err("msg=\"resync failed\" fsid=%lu fid=%llx", (unsigned long) fsid,
                fileid);
      }
    }

    if ((!fMd) || gOFS.Simulate_FMD_open_error) {
      if ((!isRW) || (layOut->IsEntryServer() && (!isReplication))) {
        eos_crit("no fmd for fileid %llu on filesystem %lu", fileid, fsid);
        eos_warning("failed to get FMD record return recoverable error ENOENT(kXR_NotFound)");

        if (hasCreationMode) {
          // clean-up before re-bouncing
          dropall(fileid, path, RedirectManager.c_str());
        }

        // Return an error that can be recovered at the MGM
        return gOFS.Emsg(epname, error, ENOENT, "open - no FMD record found");
      } else {
        eos_crit("no fmd for fileid %llu on filesystem %lu", fileid,
                 (unsigned long long) fsid);
        return gOFS.Emsg(epname, error, ENOENT, "open - no FMD record found");
      }
    }
  }

  // Call the checksum factory function with the selected layout
  if ((isRW && (opaqueCheckSum != "ignore")) || (opaqueCheckSum != "ignore")) {
    checkSum = eos::fst::ChecksumPlugins::GetChecksumObject(lid);
    eos_debug("checksum requested %d %u", checkSum, lid);
  }

  if (eos::common::LayoutId::GetBlockChecksum(lid) !=
      eos::common::LayoutId::kNone) {
    if (opaqueBlockCheckSum != "ignore") {
      hasBlockXs = true;
    }
  }

  XrdOucString oss_opaque = "";
  oss_opaque += "&mgm.lid=";
  oss_opaque += slid;
  oss_opaque += "&mgm.bookingsize=";
  oss_opaque += static_cast<int>(bookingsize);
  // Open layout implementation
  eos_info("fstpath=%s open-mode=%x create-mode=%x layout-name=%s",
           fstPath.c_str(), open_mode, create_mode, layOut->GetName());
  int rc = layOut->Open(open_mode, create_mode, oss_opaque.c_str());

  if (isReplication && !isCreation) {
    layOut->Stat(&updateStat);
  }

  if ((!rc) && isCreation && bookingsize) {
    // check if the file system is full
    XrdSysMutexHelper(gOFS.Storage->mFsFullMapMutex);

    if (gOFS.Storage->mFsFullMap[fsid]) {
      if (layOut->IsEntryServer() && (!isReplication)) {
        writeErrorFlag = kOfsDiskFullError;
        layOut->Remove();
        eos_warning("not enough space return recoverable error ENODEV(kXR_FSError)");

        if (hasCreationMode) {
          // clean-up before re-bouncing
          dropall(fileid, path, RedirectManager.c_str());
        }

        // Return an error that can be recovered at the MGM
        return gOFS.Emsg(epname, error, ENODEV, "open - not enough space");
      }

      writeErrorFlag = kOfsDiskFullError;
      return gOFS.Emsg("writeofs", error, ENOSPC,
                       "create file - disk space (headroom) exceeded fn=",
                       capOpaque ?
                       (capOpaque->Get("mgm.path") ? capOpaque->Get("mgm.path") : FName())
                       : FName());
    }

    rc = layOut->Fallocate(bookingsize);

    if (rc) {
      eos_crit("file allocation gave return code %d errno=%d for allocation of size=%llu",
               rc, errno, bookingsize);

      if (layOut->IsEntryServer() && (!isReplication)) {
        layOut->Remove();
        eos_warning("not enough space i.e file allocation failed, return "
                    "recoverable error ENODEV(kXR_FSError)");

        if (hasCreationMode) {
          // clean-up before re-bouncing
          dropall(fileid, path, RedirectManager.c_str());
        }

        // Return an error that can be recovered at the MGM
        return gOFS.Emsg(epname, error, ENODEV, "open - file allocation failed");
      } else {
        layOut->Remove();
        return gOFS.Emsg(epname, error, ENOSPC, "open - cannot allocate required space",
                         Path.c_str());
      }
    }
  }

  eos_info("checksum=%llx entryserver=%d", (unsigned long long) checkSum,
           layOut->IsEntryServer());

  if (!isCreation) {
    // Get the real size of the file, not the local stripe size!
    struct stat statinfo;

    if ((retc = layOut->Stat(&statinfo))) {
      return gOFS.Emsg(epname, error, EIO,
                       "open - cannot stat layout to determine file size", Path.c_str());
    }

    // We feed the layout size, not the physical on disk!
    eos_info("msg=\"layout size\": disk_size=%zu db_size= %llu",
             statinfo.st_size, fMd->mProtoFmd.size());

    if ((off_t) statinfo.st_size != (off_t) fMd->mProtoFmd.size()) {
      // in a RAID-like layout if the header is corrupted there is no way to know
      // the size of the initial file, therefore we take the value from the DB
      if (!isReconstruction) {
        openSize = fMd->mProtoFmd.size();
      } else {
        openSize = statinfo.st_size;
      }
    } else {
      openSize = statinfo.st_size;
    }

    if (checkSum && isRW) {
      // Preset with the last known checksum
      eos_info("msg=\"reset init\" file-xs=%s", fMd->mProtoFmd.checksum().c_str());
      checkSum->ResetInit(0, openSize, fMd->mProtoFmd.checksum().c_str());
    }
  }

  // If we are not the entry server for ReedS & RaidDP layouts we disable the
  // checksum object for write. If we read we don't check checksums at all since
  // we have block and parity checking.
  if (((eos::common::LayoutId::GetLayoutType(lid) ==
        eos::common::LayoutId::kRaidDP) ||
       (eos::common::LayoutId::GetLayoutType(lid) == eos::common::LayoutId::kRaid6) ||
       (eos::common::LayoutId::GetLayoutType(lid) == eos::common::LayoutId::kArchive))
      && ((!isRW) || (!layOut->IsEntryServer()))) {
    // This case we need to exclude!
    if (checkSum) {
      delete checkSum;
      checkSum = 0;
    }
  }

  std::string filecxerror = "0";

  if (!rc) {
    // Set the eos lfn as extended attribute
    std::unique_ptr<FileIo> io(FileIoPluginHelper::GetIoObject(
                                 layOut->GetLocalReplicaPath()));

    if (isRW) {
      if (Path.beginswith("/replicate:") || Path.beginswith("/fusex-open")) {
        if (capOpaque->Get("mgm.path")) {
          XrdOucString unsealedpath = capOpaque->Get("mgm.path");
          XrdOucString sealedpath = path;

          if (io->attrSet(std::string("user.eos.lfn"),
                          std::string(unsealedpath.c_str()))) {
            eos_err("unable to set extended attribute <eos.lfn> errno=%d", errno);
          }
        } else {
          eos_err("no lfn in replication capability");
        }
      } else {
        if (io->attrSet(std::string("user.eos.lfn"), std::string(Path.c_str()))) {
          eos_err("unable to set extended attribute <eos.lfn> errno=%d", errno);
        }
      }
    }

    // Try to get error if the file has a scan error
    io->attrGet("user.filecxerror", filecxerror);
  }

  if ((!isRW) && (filecxerror == "1")) {
    if (eos::common::LayoutId::GetLayoutType(lid) ==
        eos::common::LayoutId::kReplica) {
      eos_err("open of %s failed - replica has a checksum mismatch", Path.c_str());
      return gOFS.Emsg(epname, error, EIO, "open - replica has a checksum mismatch",
                       Path.c_str());
    }
  }

  if (!rc) {
    opened = true;
    XrdSysMutexHelper scop_lock(gOFS.OpenFidMutex);

    if (isRW) {
      gOFS.WOpenFid[fsid][fileid]++;
    } else {
      gOFS.ROpenFid[fsid][fileid]++;
    }
  } else {
    // If we have local errors in open we don't disable a filesystem -
    // this is done by the Scrub thread if necessary!
    if (layOut->IsEntryServer() && (!isReplication)) {
      eos_warning("open error return recoverable error EIO(kXR_IOError)");

      if (hasCreationMode) {
        // Clean-up before re-bouncing
        dropall(fileid, path, RedirectManager.c_str());
      }

      // Return an error that can be recovered at the MGM
      return gOFS.Emsg(epname, error, EIO, "open - failed open");
    } else {
      eos_warning("opening %s failed", Path.c_str());
      return gOFS.Emsg(epname, error, EIO, "open", Path.c_str());
    }
  }

  if (rc == SFS_OK) {
    if (isRW) {
      if (!gOFS.Storage->OpenTransaction(fsid, fileid)) {
        eos_crit("cannot open transaction for fsid=%u fid=%llu", fsid, fileid);
      }
    }
  }

  eos_debug("open finished");
  return rc;
}

//------------------------------------------------------------------------------
// Account for total read time
//------------------------------------------------------------------------------
void
XrdFstOfsFile::AddReadTime()
{
  unsigned long mus = (lrTime.tv_sec - cTime.tv_sec) * 1000000 +
                      (lrTime.tv_usec - cTime.tv_usec);
  rTime.tv_sec += (mus / 1000000);
  rTime.tv_usec += (mus % 1000000);
}

//------------------------------------------------------------------------------
// Account for total readv time
//------------------------------------------------------------------------------
void
XrdFstOfsFile::AddReadVTime()
{
  unsigned long mus = (lrvTime.tv_sec - cTime.tv_sec) * 1000000 +
                      (lrvTime.tv_usec - cTime.tv_usec);
  rvTime.tv_sec += (mus / 1000000);
  rvTime.tv_usec += (mus % 1000000);
}

//------------------------------------------------------------------------------
// Account for total write time
//------------------------------------------------------------------------------
void
XrdFstOfsFile::AddWriteTime()
{
  unsigned long mus = ((lwTime.tv_sec - cTime.tv_sec) * 1000000) +
                      lwTime.tv_usec - cTime.tv_usec;
  wTime.tv_sec += (mus / 1000000);
  wTime.tv_usec += (mus % 1000000);
}

//------------------------------------------------------------------------------
// Make report
//------------------------------------------------------------------------------
void
XrdFstOfsFile::MakeReportEnv(XrdOucString& reportString)
{
  // compute avg, min, max, sigma for read and written bytes
  unsigned long long rmin, rmax, rsum;
  unsigned long long rvmin, rvmax, rvsum; // readv bytes
  unsigned long long rsmin, rsmax, rssum; // read single bytes
  unsigned long rcmin, rcmax, rcsum;      // readv count
  unsigned long long wmin, wmax, wsum;
  double rsigma, rvsigma, rssigma, rcsigma, wsigma;
  {
    XrdSysMutexHelper vecLock(vecMutex);
    ComputeStatistics(rvec, rmin, rmax, rsum, rsigma);
    ComputeStatistics(wvec, wmin, wmax, wsum, wsigma);
    ComputeStatistics(monReadvBytes, rvmin, rvmax, rvsum, rvsigma);
    ComputeStatistics(monReadSingleBytes, rsmin, rsmax, rssum, rssigma);
    ComputeStatistics(monReadvCount, rcmin, rcmax, rcsum, rcsigma);
    char report[16384];

    if (rmin == 0xffffffff) {
      rmin = 0;
    }

    if (wmin == 0xffffffff) {
      wmin = 0;
    }

    snprintf(report, sizeof(report) - 1,
             "log=%s&path=%s&ruid=%u&rgid=%u&td=%s&"
             "host=%s&lid=%lu&fid=%llu&fsid=%lu&"
             "ots=%lu&otms=%lu&"
             "cts=%lu&ctms=%lu&"
             "nrc=%lu&nwc=%lu&"
             "rb=%llu&rb_min=%llu&rb_max=%llu&rb_sigma=%.02f&"
             "rv_op=%llu&rvb_min=%llu&rvb_max=%llu&rvb_sum=%llu&rvb_sigma=%.02f&"
             "rs_op=%llu&rsb_min=%llu&rsb_max=%llu&rsb_sum=%llu&rsb_sigma=%.02f&"
             "rc_min=%lu&rc_max=%lu&rc_sum=%lu&rc_sigma=%.02f&"
             "wb=%llu&wb_min=%llu&wb_max=%llu&wb_sigma=%.02f&"
             "sfwdb=%llu&sbwdb=%llu&sxlfwdb=%llu&sxlbwdb=%llu&"
             "nfwds=%lu&nbwds=%lu&nxlfwds=%lu&nxlbwds=%lu&"
             "rt=%.02f&rvt=%.02f&wt=%.02f&osize=%llu&csize=%llu&%s"
             , this->logId
             , capOpaque->Get("mgm.path") ? capOpaque->Get("mgm.path") : Path.c_str()
             , this->vid.uid, this->vid.gid, tIdent.c_str()
             , gOFS.mHostName, lid, fileid, fsid
             , openTime.tv_sec, (unsigned long) openTime.tv_usec / 1000
             , closeTime.tv_sec, (unsigned long) closeTime.tv_usec / 1000
             , rCalls, wCalls
             , rsum, rmin, rmax, rsigma
             , (unsigned long long)monReadvBytes.size(), rvmin, rvmax, rvsum, rvsigma
             , (unsigned long long)monReadSingleBytes.size(), rsmin, rsmax, rssum, rssigma
             , rcmin, rcmax, rcsum, rcsigma
             , wsum
             , wmin
             , wmax
             , wsigma
             , sFwdBytes
             , sBwdBytes
             , sXlFwdBytes
             , sXlBwdBytes
             , nFwdSeeks
             , nBwdSeeks
             , nXlFwdSeeks
             , nXlBwdSeeks
             , ((rTime.tv_sec * 1000.0) + (rTime.tv_usec / 1000.0))
             , ((rvTime.tv_sec * 1000.0) + (rvTime.tv_usec / 1000.0))
             , ((wTime.tv_sec * 1000.0) + (wTime.tv_usec / 1000.0))
             , (unsigned long long) openSize
             , (unsigned long long) closeSize
             , eos::common::SecEntity::ToEnv(SecString.c_str(),
                 ((mTpcFlag == kTpcDstSetup) ||
                  (mTpcFlag == kTpcSrcRead)) ? "tpc" : 0).c_str());
    reportString = report;
  }
}

//------------------------------------------------------------------------------
// Check if file has been modified while in use
//------------------------------------------------------------------------------
int
XrdFstOfsFile::modified()
{
  int rc = 0;
  bool fileExists = true;
  struct stat statinfo;

  if (layOut) {
    if ((layOut->Stat(&statinfo))) {
      fileExists = false;
    }
  } else {
    if ((XrdOfsOss->Stat(fstPath.c_str(), &statinfo))) {
      fileExists = false;
    }
  }

  // Check if the file could have been changed in the meanwhile ...
  if (fileExists && isReplication && (!isRW)) {
    {
      XrdSysMutexHelper scope_lock(gOFS.OpenFidMutex);

      if (gOFS.WOpenFid[fsid].count(fileid)) {
        if (gOFS.WOpenFid[fsid][fileid] > 0) {
          eos_err("file is now open for writing - discarding replication "
                  "[wopen=%d]", gOFS.WOpenFid[fsid][fileid]);
          gOFS.Emsg("closeofs", error, EIO,
                    "guarantee correctness - "
                    "file has been opened for writing during replication",
                    Path.c_str());
          rc = SFS_ERROR;
        }
      }
    }

    if ((statinfo.st_mtime != updateStat.st_mtime)) {
      eos_err("file has been modified during replication");
      rc = SFS_ERROR;
      gOFS.Emsg("closeofs", error, EIO, "guarantee correctness -"
                "file has been modified during replication", Path.c_str());
    }
  }

  return rc;
}

//------------------------------------------------------------------------------
// Close - low level ofs method which is called from one of the layout plugins
//------------------------------------------------------------------------------
int
XrdFstOfsFile::closeofs()
{
  int rc = 0;
  rc |= XrdOfsFile::close();
  return rc;
}

//------------------------------------------------------------------------------
// Layout read callback
//------------------------------------------------------------------------------
int
XrdFstOfsFile::LayoutReadCB(eos::fst::CheckSum::ReadCallBack::callback_data_t*
                            cbd)
{
  return ((Layout*) cbd->caller)->Read(cbd->offset, cbd->buffer, cbd->size);
}

//------------------------------------------------------------------------------
// File read callback
//------------------------------------------------------------------------------
int
XrdFstOfsFile::FileIoReadCB(eos::fst::CheckSum::ReadCallBack::callback_data_t*
                            cbd)
{
  return ((FileIo*) cbd->caller)->fileRead(cbd->offset, cbd->buffer, cbd->size);
}

//------------------------------------------------------------------------------
// Verify checksum method
//------------------------------------------------------------------------------
bool
XrdFstOfsFile::verifychecksum()
{
  bool checksumerror = false;
  int checksumlen = 0;

  // Deal with checksums
  if (checkSum) {
    checkSum->Finalize();

    if (checkSum->NeedsRecalculation()) {
      if ((!isRW) && ((sFwdBytes + sBwdBytes)
                      || (checkSum->GetMaxOffset() != openSize))) {
        // We don't rescan files if they are read non-sequential or only
        // partially
        eos_debug("info=\"skipping checksum (re-scan) for non-sequential "
                  "reading ...\"");
        delete checkSum;
        checkSum = 0;
        return false;
      }
    } else {
      eos_debug("isrw=%d max-offset=%lld opensize=%lld", isRW,
                checkSum->GetMaxOffset(), openSize);

      if (((!isRW) && ((checkSum->GetMaxOffset() != openSize) ||
                       (!checkSum->GetMaxOffset())))) {
        eos_debug("info=\"skipping checksum (re-scan) for access without any IO or "
                  "partial sequential read IO from the beginning...\"");
        delete checkSum;
        checkSum = 0;
        return false;
      }

      if ((isRW) && checkSum->GetMaxOffset() &&
          (checkSum->GetMaxOffset() < openSize)) {
        // If there was a write which was not extending the file the checksum
        // is dirty!
        checkSum->SetDirty();
      }
    }

    // If checksum is not completely computed
    if (checkSum->NeedsRecalculation()) {
      unsigned long long scansize = 0;
      float scantime = 0; // is ms

      if (!XrdOfsFile::fctl(SFS_FCTL_GETFD, 0, error)) {
        // Rescan the file
        eos::fst::CheckSum::ReadCallBack::callback_data_t cbd;
        cbd.caller = (void*) layOut;
        eos::fst::CheckSum::ReadCallBack cb(LayoutReadCB, cbd);

        if (checkSum->ScanFile(cb, scansize, scantime)) {
          XrdOucString sizestring;
          eos_info("info=\"rescanned checksum\" size=%s time=%.02f ms rate=%.02f MB/s %s",
                   eos::common::StringConversion::GetReadableSizeString(sizestring,
                       scansize, "B"),
                   scantime,
                   1.0 * scansize / 1000 / (scantime ? scantime : 99999999999999LL),
                   checkSum->GetHexChecksum());
        } else {
          eos_err("Rescanning of checksum failed");
          delete checkSum;
          checkSum = 0;
          return false;
        }
      } else {
        eos_err("Couldn't get file descriptor");
        delete checkSum;
        checkSum = 0;
        return false;
      }
    } else {
      // This was prefect streaming I/O
      if ((!isRW) && (checkSum->GetMaxOffset() != openSize)) {
        eos_info("info=\"skipping checksum (re-scan) since file was not read "
                 "completely %llu %llu...\"", checkSum->GetMaxOffset(), openSize);
        delete checkSum;
        checkSum = 0;
        return false;
      }
    }

    if (isRW) {
      eos_info("(write) checksum type: %s checksum hex: %s requested-checksum hex: %s",
               checkSum->GetName(), checkSum->GetHexChecksum(),
               openOpaque->Get("mgm.checksum") ? openOpaque->Get("mgm.checksum") : "-none-");

      // Check if the check sum for the file was given at upload time
      if (openOpaque->Get("mgm.checksum")) {
        XrdOucString opaqueChecksum = openOpaque->Get("mgm.checksum");
        XrdOucString hexChecksum = checkSum->GetHexChecksum();

        if ((opaqueChecksum != "disable") && (opaqueChecksum != hexChecksum)) {
          eos_err("requested checksum %s does not match checksum %s of uploaded"
                  " file", opaqueChecksum.c_str(), hexChecksum.c_str());
          delete checkSum;
          checkSum = 0;
          return true;
        }
      }

      checkSum->GetBinChecksum(checksumlen);
      // Copy checksum into meta data
      fMd->mProtoFmd.set_checksum(checkSum->GetHexChecksum());

      if (haswrite) {
        // If we have no write, we don't set this attributes (xrd3cp!)
        // set the eos checksum extended attributes
        std::unique_ptr<eos::fst::FileIo> io(eos::fst::FileIoPluginHelper::GetIoObject(
                                               fstPath.c_str()));

        if (((eos::common::LayoutId::GetLayoutType(lid) ==
              eos::common::LayoutId::kPlain) ||
             (eos::common::LayoutId::GetLayoutType(lid) ==
              eos::common::LayoutId::kReplica))) {
          // Don't put file checksum tags for complex layouts like raid6,readdp, archive
          if (io->attrSet(std::string("user.eos.checksumtype"),
                          std::string(checkSum->GetName()))) {
            eos_err("unable to set extended attribute <eos.checksumtype> errno=%d", errno);
          }

          if (io->attrSet("user.eos.checksum", checkSum->GetBinChecksum(checksumlen),
                          checksumlen)) {
            eos_err("unable to set extended attribute <eos.checksum> errno=%d", errno);
          }
        }

        // Reset any tagged error
        if (io->attrSet("user.eos.filecxerror", "0")) {
          eos_err("unable to set extended attribute <eos.filecxerror> errno=%d", errno);
        }

        if (io->attrSet("user.eos.blockcxerror", "0")) {
          eos_err("unable to set extended attribute <eos.blockcxerror> errno=%d", errno);
        }
      }
    } else {
      // This is a read with checksum check, compare with fMD
      bool isopenforwrite = false;
      // If the file is currently opened for write we don't check checksums!
      {
        XrdSysMutexHelper scope_lock(gOFS.OpenFidMutex);

        if (gOFS.WOpenFid[fsid].count(fileid)) {
          if (gOFS.WOpenFid[fsid][fileid] > 0) {
            isopenforwrite = true;
          }
        }
      }

      if (isopenforwrite) {
        eos_info("(read)  disabling checksum check: file is currently written");
        return false;
      }

      eos_info("(read)  checksum type: %s checksum hex: %s fmd-checksum: %s",
               checkSum->GetName(),
               checkSum->GetHexChecksum(),
               fMd->mProtoFmd.checksum().c_str());
      std::string calculatedchecksum = checkSum->GetHexChecksum();

      // We might fetch an unitialized value, so that is not to be considered
      // a checksum error yet.
      if (fMd->mProtoFmd.checksum() != "none") {
        if (calculatedchecksum != fMd->mProtoFmd.checksum().c_str()) {
          checksumerror = true;
        }
      }
    }
  }

  return checksumerror;
}

//------------------------------------------------------------------------------
// Close file
//------------------------------------------------------------------------------
int
XrdFstOfsFile::close()
{
  EPNAME("close");
  int rc = 0; // return code
  int brc = 0; // return code before 'close' has been called
  bool checksumerror = false;
  bool targetsizeerror = false;
  bool committed = false;
  bool minimumsizeerror = false;
  bool consistencyerror = false;

  // Any close on a file opened in TPC mode invalidates tpc keys
  if (TpcKey.length()) {
    {
      XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex);

      if (gOFS.TpcMap[isRW].count(TpcKey.c_str())) {
        eos_info("msg=\"remove tpc key\" key=%s", TpcKey.c_str());
        gOFS.TpcMap[isRW].erase(TpcKey.c_str());

        try {
          gOFS.TpcMap[isRW].resize(0);
        } catch (const std::length_error& e) {}
      }
    }

    if (mTpcFlag == kTpcDstSetup) {
      if (!mTpcThreadStatus) {
        int retc = XrdSysThread::Join(mTpcThread, NULL);
        eos_debug("TPC job join returned %i", retc);
      } else {
        eos_warning("TPC job was never started successfully");
      }
    }
  }

  // We enter the close logic only once since there can be an explicit close or
  // a close via the destructor
  if (opened && (!closed) && fMd) {
    // Check if the file close comes from a client disconnect e.g. the destructor
    XrdOucString hexstring = "";
    eos::common::FileId::Fid2Hex(fMd->mProtoFmd.fid(), hexstring);
    XrdOucErrInfo error;
    XrdOucString capOpaqueString = "/?mgm.pcmd=drop";
    XrdOucString OpaqueString = "";
    OpaqueString += "&mgm.fsid=";
    OpaqueString += (int) fMd->mProtoFmd.fsid();
    OpaqueString += "&mgm.fid=";
    OpaqueString += hexstring;
    XrdOucEnv Opaque(OpaqueString.c_str());
    capOpaqueString += OpaqueString;

    if ((viaDelete || writeDelete || remoteDelete) && ((isCreation ||
        IsChunkedUpload()) && (!mFusex))) {
      // It is closed by the constructor e.g. no proper close
      // or the specified checksum does not match the computed one
      if (viaDelete) {
        eos_info("msg=\"(unpersist): deleting file\" reason=\"client disconnect\""
                 "  fsid=%u fxid=%08x on fsid=%u", fMd->mProtoFmd.fsid(), fMd->mProtoFmd.fid());
      }

      if (writeDelete) {
        eos_info("msg=\"(unpersist): deleting file\" reason=\"write/policy error\""
                 " fsid=%u fxid=%08x on fsid=%u", fMd->mProtoFmd.fsid(), fMd->mProtoFmd.fid());
      }

      if (remoteDelete) {
        eos_info("msg=\"(unpersist): deleting file\" reason=\"remote deletion\""
                 " fsid=%u fxid=%08x on fsid=%u", fMd->mProtoFmd.fsid(), fMd->mProtoFmd.fid());
      }

      // Delete the file - set the file to be deleted
      deleteOnClose = true;
      layOut->Remove();

      if (layOut->IsEntryServer() && (!isReplication)) {
        capOpaqueString += "&mgm.dropall=1";
      }

      // Delete the replica in the MGM
      int rc = gOFS.CallManager(&error, capOpaque->Get("mgm.path"),
                                capOpaque->Get("mgm.manager"), capOpaqueString);

      if (rc) {
        eos_warning("(unpersist): unable to drop file id %s fsid %u at manager %s",
                    hexstring.c_str(), fMd->mProtoFmd.fid(), capOpaque->Get("mgm.manager"));
      }
    } else {
      // Check if this was a newly created file
      if (isCreation) {
        // If we had space allocation we have to truncate the allocated space to
        // the real size of the file
        if ((strcmp(layOut->GetName(), "raiddp") == 0) ||
            (strcmp(layOut->GetName(), "raid6") == 0) ||
            (strcmp(layOut->GetName(), "archive") == 0)) {
          // the entry server has to truncate only if this is not a recovery action
          if (layOut->IsEntryServer() && !store_recovery) {
            eos_info("msg=\"truncate RAIN layout\" truncate-offset=%llu",
                     maxOffsetWritten);
            layOut->Truncate(maxOffsetWritten);
          }
        } else {
          if ((long long) maxOffsetWritten > (long long) openSize) {
            // Check if we have to deallocate something for this file transaction
            if ((bookingsize) && (bookingsize > (long long) maxOffsetWritten)) {
              eos_info("deallocationg %llu bytes", bookingsize - maxOffsetWritten);
              layOut->Truncate(maxOffsetWritten);
              // We have evt. to deallocate blocks which have not been written
              layOut->Fdeallocate(maxOffsetWritten, bookingsize);
            }
          }
        }
      }

      eos_info("calling verifychecksum");
      checksumerror = verifychecksum();
      targetsizeerror = (targetsize) ? (targetsize != (off_t) maxOffsetWritten) :
                        false;

      if (isCreation) {
        // Check that the minimum file size policy is met!
        minimumsizeerror = (minsize) ? ((off_t) maxOffsetWritten < minsize) : false;

        if (minimumsizeerror) {
          eos_warning("written file %s is smaller than required minimum file "
                      "size=%llu written=%llu", Path.c_str(), minsize,
                      maxOffsetWritten);
        }
      }

      if ((eos::common::LayoutId::GetLayoutType(layOut->GetLayoutId()) ==
           eos::common::LayoutId::kRaidDP) ||
          (eos::common::LayoutId::GetLayoutType(layOut->GetLayoutId()) ==
           eos::common::LayoutId::kRaid6) ||
          (eos::common::LayoutId::GetLayoutType(layOut->GetLayoutId()) ==
           eos::common::LayoutId::kArchive)) {
        // For RAID-like layouts don't do this check
        targetsizeerror = false;
        minimumsizeerror = false;
      }

      eos_debug("checksumerror = %i, targetsizerror= %i,"
                "maxOffsetWritten = %zu, targetsize = %lli",
                checksumerror, targetsizeerror, maxOffsetWritten, targetsize);

      // ---- add error simulation for checksum errors on read
      if ((!isRW) && gOFS.Simulate_XS_read_error) {
        checksumerror = true;
        eos_warning("simlating checksum errors on read");
      }

      // ---- add error simulation for checksum errors on write
      if (isRW && gOFS.Simulate_XS_write_error) {
        checksumerror = true;
        eos_warning("simlating checksum errors on write");
      }

      if (isRW && (checksumerror || targetsizeerror || minimumsizeerror)) {
        // We have a checksum error if the checksum was preset and does not match!
        // We have a target size error, if the target size was preset and does not match!
        // Set the file to be deleted
        deleteOnClose = true;
        layOut->Remove();

        if (layOut->IsEntryServer() && (!isReplication)) {
          capOpaqueString += "&mgm.dropall=1";
        }

        // Delete the replica in the MGM
        int rc = gOFS.CallManager(&error, capOpaque->Get("mgm.path"),
                                  capOpaque->Get("mgm.manager"), capOpaqueString);

        if (rc) {
          eos_warning("(unpersist): unable to drop file id %s fsid %u at manager %s",
                      hexstring.c_str(), fMd->mProtoFmd.fid(), capOpaque->Get("mgm.manager"));
        }
      }

      // Store the entry server information before closing the layout
      bool isEntryServer = false;

      if (layOut->IsEntryServer()) {
        isEntryServer = true;
      }

      // First we assume that, if we have writes, we update it
      closeSize = openSize;

      if ((!checksumerror) && (haswrite || isCreation || commitReconstruction) &&
          (!minimumsizeerror) && (!isReconstruction || !hasReadError)) {
        // Commit meta data
        struct stat statinfo;

        if ((rc = layOut->Stat(&statinfo))) {
          rc = gOFS.Emsg(epname, this->error, EIO, "close - cannot stat closed layout"
                         " to determine file size", Path.c_str());
        }

        if (!rc) {
          if ((statinfo.st_size == 0) || haswrite) {
            // Update size
            closeSize = statinfo.st_size;
            fMd->mProtoFmd.set_size(statinfo.st_size);
            fMd->mProtoFmd.set_disksize(statinfo.st_size);
            fMd->mProtoFmd.set_mgmsize(0xfffffffffff1ULL); // now again undefined
            fMd->mProtoFmd.set_mgmchecksum(""); // now again empty
            fMd->mProtoFmd.set_diskchecksum(""); // now again empty
            fMd->mProtoFmd.set_layouterror(0); // reset layout errors
            fMd->mProtoFmd.set_locations(""); // reset locations
            fMd->mProtoFmd.set_filecxerror(0);
            fMd->mProtoFmd.set_blockcxerror(0);
            fMd->mProtoFmd.set_locations(""); // reset locations
            fMd->mProtoFmd.set_filecxerror(0);
            fMd->mProtoFmd.set_blockcxerror(0);
            fMd->mProtoFmd.set_mtime(statinfo.st_mtime);
#ifdef __APPLE__
            fMd->mProtoFmd.set_mtime_ns(0);
#else
            fMd->mProtoFmd.set_mtime_ns(statinfo.st_mtim.tv_nsec);
#endif
            // Set the container id
            fMd->mProtoFmd.set_cid(cid);

            // For replicat's set the original uid/gid/lid values
            if (capOpaque->Get("mgm.source.lid")) {
              fMd->mProtoFmd.set_lid(strtoul(capOpaque->Get("mgm.source.lid"), 0, 10));
            }

            if (capOpaque->Get("mgm.source.ruid")) {
              fMd->mProtoFmd.set_uid(atoi(capOpaque->Get("mgm.source.ruid")));
            }

            if (capOpaque->Get("mgm.source.rgid")) {
              fMd->mProtoFmd.set_gid(atoi(capOpaque->Get("mgm.source.rgid")));
            }

            // Commit local
            try {
              if (!gFmdDbMapHandler.Commit(fMd)) {
                eos_err("unabel to commit meta data to local database");
                (void) gOFS.Emsg(epname, this->error, EIO, "close - unable to "
                                 "commit meta data", Path.c_str());
              }
            } catch (const std::length_error& e) {}

            // Commit to central mgm cache
            int envlen = 0;
            XrdOucString capOpaqueFile = "";
            XrdOucString mTimeString = "";
            capOpaqueFile += "/?";
            capOpaqueFile += capOpaque->Env(envlen);
            capOpaqueFile += "&mgm.pcmd=commit";
            capOpaqueFile += "&mgm.size=";
            char filesize[1024];
            sprintf(filesize, "%" PRIu64 "", fMd->mProtoFmd.size());
            capOpaqueFile += filesize;

            if (checkSum) {
              capOpaqueFile += "&mgm.checksum=";
              capOpaqueFile += checkSum->GetHexChecksum();
            }

            capOpaqueFile += "&mgm.mtime=";
            capOpaqueFile += eos::common::StringConversion::GetSizeString(mTimeString,
                             (mForcedMtime != 1) ? mForcedMtime : (unsigned long long)
                             fMd->mProtoFmd.mtime());
            capOpaqueFile += "&mgm.mtime_ns=";
            capOpaqueFile += eos::common::StringConversion::GetSizeString(mTimeString,
                             (mForcedMtime != 1) ? mForcedMtime_ms : (unsigned long long)
                             fMd->mProtoFmd.mtime_ns());

            if (mFusex) {
              capOpaqueFile += "&mgm.fusex=1";
            }

            if (haswrite) {
              capOpaqueFile += "&mgm.modified=1";
            }

            capOpaqueFile += "&mgm.add.fsid=";
            capOpaqueFile += (int) fMd->mProtoFmd.fsid();

            // If <drainfsid> is set, we can issue a drop replica
            if (capOpaque->Get("mgm.drainfsid")) {
              capOpaqueFile += "&mgm.drop.fsid=";
              capOpaqueFile += capOpaque->Get("mgm.drainfsid");
            }

            if (isReconstruction) {
              // Indicate that this is a commit of a RAIN reconstruction
              capOpaqueFile += "&mgm.reconstruction=1";

              if (!hasReadError && openOpaque->Get("eos.pio.recfs")) {
                capOpaqueFile += "&mgm.drop.fsid=";
                capOpaqueFile += openOpaque->Get("eos.pio.recfs");
                commitReconstruction = true;
              }
            } else {
              if (isEntryServer && !isReplication && !isInjection) {
                // The entry server commits size and checksum
                capOpaqueFile += "&mgm.commit.size=1&mgm.commit.checksum=1";
              } else {
                capOpaqueFile += "&mgm.replication=1";
              }
            }

            // The log ID to the commit
            capOpaqueFile += "&mgm.logid=";
            capOpaqueFile += logId;

            // Evt. tag as an OC-Chunk commit
            if (isOCchunk) {
              // Add the chunk information
              int envlen;
              capOpaqueFile += eos::common::OwnCloud::FilterOcQuery(openOpaque->Env(envlen));
            }

            rc = gOFS.CallManager(&error, capOpaque->Get("mgm.path"),
                                  capOpaque->Get("mgm.manager"), capOpaqueFile);

            if (rc) {
              if ((rc == -EIDRM) || (rc == -EBADE) || (rc == -EBADR)) {
                if (!gOFS.Storage->CloseTransaction(fsid, fileid)) {
                  eos_crit("cannot close transaction for fsid=%u fid=%llu", fsid, fileid);
                }

                if (rc == -EIDRM) {
                  // This file has been deleted in the meanwhile ... we can
                  // unlink that immedeatly
                  eos_info("info=\"unlinking fid=%08x path=%s - "
                           "file has been already unlinked from the namespace\"",
                           fMd->mProtoFmd.fid(), Path.c_str());
                }

                if (rc == -EBADE) {
                  eos_err("info=\"unlinking fid=%08x path=%s - "
                          "file size of replica does not match reference\"",
                          fMd->mProtoFmd.fid(), Path.c_str());
                  consistencyerror = true;
                }

                if (rc == -EBADR) {
                  eos_err("info=\"unlinking fid=%08x path=%s - "
                          "checksum of replica does not match reference\"",
                          fMd->mProtoFmd.fid(), Path.c_str());
                  consistencyerror = true;
                }

                deleteOnClose = true;
              } else {
                eos_crit("commit returned an uncatched error msg=%s [probably timeout]"
                         " - closing transaction to keep the file save", error.getErrText());

                if (isRW) {
                  gOFS.Storage->CloseTransaction(fsid, fileid);
                }
              }
            } else {
              committed = true;
            }
          }
        }
      }
    }

    if (isRW && (rc == SFS_OK)) {
      gOFS.Storage->CloseTransaction(fsid, fileid);
    }

    // Recompute our ETag
    {
      // If there is a checksum we use the checksum, otherwise we return inode+mtime
      if (checkSum) {
        if (strcmp(checkSum->GetName(), "md5")) {
          // use inode + checksum
          char setag[256];
          snprintf(setag, sizeof(setag) - 1, "\"%llu:%s\"",
                   eos::common::FileId::FidToInode((unsigned long long) fMd->mProtoFmd.fid()),
                   fMd->mProtoFmd.checksum().c_str());
          ETag = setag;
        } else {
          // use checksum, S3 wants the pure MD5
          char setag[256];
          snprintf(setag, sizeof(setag) - 1, "\"%s\"", fMd->mProtoFmd.checksum().c_str());
          ETag = setag;
        }
      } else {
        // use inode + mtime
        char setag[256];
        snprintf(setag, sizeof(setag) - 1, "\"%llu:%llu\"",
                 eos::common::FileId::FidToInode((unsigned long long) fMd->mProtoFmd.fid()),
                 (unsigned long long) fMd->mProtoFmd.mtime());
        ETag = setag;
      }
    }
    int closerc = 0; // return of the close
    brc = rc; // return before the close
    rc |= modified();
    closerc = layOut->Close();
    rc |= closerc;
    closed = true;

    if (closerc || (isReconstruction && hasReadError)) {
      // For RAIN layouts if there is an error on close when writing then we
      // delete the whole file. If we do RAIN reconstruction we cleanup this
      // local replica which was not commited.
      if ((eos::common::LayoutId::GetLayoutType(layOut->GetLayoutId()) ==
           eos::common::LayoutId::kRaidDP) ||
          (eos::common::LayoutId::GetLayoutType(layOut->GetLayoutId()) ==
           eos::common::LayoutId::kRaid6) ||
          (eos::common::LayoutId::GetLayoutType(layOut->GetLayoutId()) ==
           eos::common::LayoutId::kArchive)) {
        deleteOnClose = true;
      } else {
        // Some (remote) replica didn't make it through ... trigger an auto-repair
        if (!deleteOnClose) {
          repairOnClose = true;
        }
      }
    }

    {
      XrdSysMutexHelper scope_lock(gOFS.OpenFidMutex);

      if (isRW) {
        if ((isInjection || isCreation || IsChunkedUpload()) && (!rc) &&
            (gOFS.WOpenFid[fMd->mProtoFmd.fsid()][fMd->mProtoFmd.fid()] > 1)) {
          // indicate that this file was closed properly and disable further delete on close
          gOFS.WNoDeleteOnCloseFid[fMd->mProtoFmd.fsid()][fMd->mProtoFmd.fid()] = true;
        }

        gOFS.WOpenFid[fMd->mProtoFmd.fsid()][fMd->mProtoFmd.fid()]--;
      } else {
        gOFS.ROpenFid[fMd->mProtoFmd.fsid()][fMd->mProtoFmd.fid()]--;
      }

      if (gOFS.WOpenFid[fMd->mProtoFmd.fsid()][fMd->mProtoFmd.fid()] <= 0) {
        gOFS.WOpenFid[fMd->mProtoFmd.fsid()].erase(fMd->mProtoFmd.fid());
        gOFS.WOpenFid[fMd->mProtoFmd.fsid()].resize(0);
        // When the last writer is gone we can remove the prohibiting entry
        gOFS.WNoDeleteOnCloseFid[fMd->mProtoFmd.fsid()].erase(fMd->mProtoFmd.fid());
        gOFS.WNoDeleteOnCloseFid[fMd->mProtoFmd.fsid()].resize(0);
      }

      if (gOFS.ROpenFid[fMd->mProtoFmd.fsid()][fMd->mProtoFmd.fid()] <= 0) {
        gOFS.ROpenFid[fMd->mProtoFmd.fsid()].erase(fMd->mProtoFmd.fid());
        gOFS.ROpenFid[fMd->mProtoFmd.fsid()].resize(0);
      }
    }

    gettimeofday(&closeTime, &tz);

    if (!deleteOnClose) {
      // Prepare a report and add to the report queue
      if (mTpcFlag != kTpcSrcCanDo) {
        // We don't want a report for the source tpc setup. The kTpcSrcRead
        // stage actually uses the opaque info from kTpcSrcSetup and that's
        // why we also generate a report at this stage.
        XrdOucString reportString = "";
        MakeReportEnv(reportString);
        gOFS.ReportQueueMutex.Lock();
        gOFS.ReportQueue.push(reportString);
        gOFS.ReportQueueMutex.UnLock();
      }

      if (isRW) {
        // Store in the WrittenFilesQueue
        gOFS.WrittenFilesQueueMutex.Lock();
        gOFS.WrittenFilesQueue.push(fMd->mProtoFmd);
        gOFS.WrittenFilesQueueMutex.UnLock();
      }
    }

    // Check if the target filesystem has been put into some non-operational mode
    // in the meanwhile, it makes no sense to try to commit in this case
    {
      eos::common::RWMutexReadLock lock(gOFS.Storage->mFsMutex);

      if (gOFS.Storage->mFileSystemsMap.count(fsid) &&
          gOFS.Storage->mFileSystemsMap[fsid]->GetConfigStatus() <
          eos::common::FileSystem::kDrain) {
        eos_notice("msg=\"failing transfer because filesystem has non-"
                   "operational state\" path=%s state=%s", Path.c_str(),
                   eos::common::FileSystem::GetConfigStatusAsString
                   (gOFS.Storage->mFileSystemsMap[fsid]->GetConfigStatus()));
        deleteOnClose = true;
      }
    }
    {
      // Check if the delete on close has been prohibited for this file id
      XrdSysMutexHelper scope_lock(gOFS.OpenFidMutex);

      if (gOFS.WNoDeleteOnCloseFid[fsid].count(fileid)) {
        eos_notice("msg=\"prohibiting delete on close since we had a "
                   "sussessfull put but still an unacknowledged open\" path=%s",
                   Path.c_str());
        deleteOnClose = false;
      }
    }

    if (deleteOnClose && (isInjection || isCreation || IsChunkedUpload()) &&
        (!mFusex)) {
      eos_info("info=\"deleting on close\" fn=%s fstpath=%s",
               capOpaque->Get("mgm.path"), fstPath.c_str());
      int retc = gOFS._rem(Path.c_str(), error, 0, capOpaque, fstPath.c_str(),
                           fileid, fsid, true);

      if (retc) {
        eos_debug("<rem> returned retc=%d", retc);
      }

      if (committed) {
        // If we committed the replica and an error happened remote, we have
        // to unlink it again
        XrdOucString hexstring = "";
        eos::common::FileId::Fid2Hex(fileid, hexstring);
        XrdOucErrInfo error;
        XrdOucString capOpaqueString = "/?mgm.pcmd=drop";
        XrdOucString OpaqueString = "";
        OpaqueString += "&mgm.fsid=";
        OpaqueString += (int) fsid;
        OpaqueString += "&mgm.fid=";
        OpaqueString += hexstring;

        // If deleteOnClose at the gateway then we drop all replicas
        if (layOut->IsEntryServer() && (!isReplication)) {
          OpaqueString += "&mgm.dropall=1";
        }

        XrdOucEnv Opaque(OpaqueString.c_str());
        capOpaqueString += OpaqueString;
        // Delete the replica in the MGM
        int rcode = gOFS.CallManager(&error, capOpaque->Get("mgm.path"),
                                     capOpaque->Get("mgm.manager"), capOpaqueString);

        if (rcode && (rcode != -EIDRM)) {
          eos_warning("(unpersist): unable to drop file id %s fsid %u at manager %s",
                      hexstring.c_str(), fileid, capOpaque->Get("mgm.manager"));
        }

        eos_info("info=\"removing on manager\" manager=%s fid=%llu fsid=%d "
                 "fn=%s fstpath=%s rc=%d", capOpaque->Get("mgm.manager"),
                 fileid, (int) fsid,
                 capOpaque->Get("mgm.path"), fstPath.c_str(), rcode);
      }

      rc = SFS_ERROR;

      if (minimumsizeerror) {
        // Minimum size criteria not fullfilled
        gOFS.Emsg(epname, this->error, EIO, "store file - file has been cleaned "
                  "because it is smaller than the required minimum file size"
                  " in that directory", Path.c_str());
        eos_warning("info=\"deleting on close\" fn=%s fstpath=%s reason="
                    "\"minimum file size criteria\"", capOpaque->Get("mgm.path"),
                    fstPath.c_str());
      } else {
        if (checksumerror) {
          // Checksum error
          gOFS.Emsg(epname, this->error, EIO, "store file - file has been cleaned "
                    "because of a checksum error ", Path.c_str());
          eos_warning("info=\"deleting on close\" fn=%s fstpath=%s reason="
                      "\"checksum error\"", capOpaque->Get("mgm.path"), fstPath.c_str());
        } else {
          if (writeErrorFlag == kOfsSimulatedIoError) {
            // Simulated write error
            gOFS.Emsg(epname, this->error, EIO, "store file - file has been cleaned "
                      "because of a simulated IO error ", Path.c_str());
            eos_warning("info=\"deleting on close\" fn=%s fstpath=%s reason="
                        "\"simulated IO error\"", capOpaque->Get("mgm.path"), fstPath.c_str());
          } else {
            if (writeErrorFlag == kOfsMaxSizeError) {
              // Maximum size criteria not fullfilled
              gOFS.Emsg(epname, this->error, EIO, "store file - file has been cleaned "
                        "because you exceeded the maximum file size settings for "
                        "this namespace branch", Path.c_str());
              eos_warning("info=\"deleting on close\" fn=%s fstpath=%s reason="
                          "\"maximum file size criteria\"", capOpaque->Get("mgm.path"),
                          fstPath.c_str());
            } else {
              if (writeErrorFlag == kOfsDiskFullError) {
                // Disk full detected during write
                gOFS.Emsg(epname, this->error, EIO, "store file - file has been cleaned"
                          " because the target disk filesystem got full and you "
                          "didn't use reservation", Path.c_str());
                eos_warning("info=\"deleting on close\" fn=%s fstpath=%s reason="
                            "\"filesystem full\"", capOpaque->Get("mgm.path"), fstPath.c_str());
              } else {
                if (writeErrorFlag == kOfsIoError) {
                  // Generic IO error on the underlying device
                  gOFS.Emsg(epname, this->error, EIO, "store file - file has been cleaned because"
                            " of an IO error during a write operation", Path.c_str());
                  eos_crit("info=\"deleting on close\" fn=%s fstpath=%s reason="
                           "\"write IO error\"", capOpaque->Get("mgm.path"), fstPath.c_str());
                } else {
                  // Target size is different from the uploaded file size
                  if (targetsizeerror) {
                    gOFS.Emsg(epname, this->error, EIO, "store file - file has been "
                              "cleaned because the stored file does not match "
                              "the provided targetsize", Path.c_str());
                    eos_warning("info=\"deleting on close\" fn=%s fstpath=%s reason="
                                "\"target size mismatch\"", capOpaque->Get("mgm.path"),
                                fstPath.c_str());
                  } else {
                    if (consistencyerror) {
                      gOFS.Emsg(epname, this->error, EIO, "store file - file has been "
                                "cleaned because the stored file does not match "
                                "the reference meta-data size/checksum", Path.c_str());
                      eos_crit("info=\"deleting on close\" fn=%s fstpath=%s reason="
                               "\"meta-data size/checksum mismatch\"", capOpaque->Get("mgm.path"),
                               fstPath.c_str());
                    } else {
                      // Client has disconnected and file is cleaned-up
                      gOFS.Emsg(epname, this->error, EIO, "store file - file has been "
                                "cleaned because of a client disconnect", Path.c_str());
                      eos_warning("info=\"deleting on close\" fn=%s fstpath=%s "
                                  "reason=\"client disconnect\"", capOpaque->Get("mgm.path"),
                                  fstPath.c_str());
                    }
                  }
                }
              }
            }
          }
        }
      }
    } else {
      if (checksumerror) {
        // Checksum error detected
        rc = SFS_ERROR;
        gOFS.Emsg(epname, this->error, EIO,
                  "verify checksum - checksum error for file fn=",
                  capOpaque->Get("mgm.path"));
        int envlen = 0;
        eos_crit("file-xs error file=%s", capOpaque->Env(envlen));
      }
    }

    if ((!isOCchunk) && repairOnClose) {
      // Do an upcall to the MGM and ask to adjust the replica of the uploaded file
      XrdOucString OpaqueString = "/?mgm.pcmd=adjustreplica&mgm.path=";
      OpaqueString += capOpaque->Get("mgm.path");
      eos_info("info=\"repair on close\" path=%s", capOpaque->Get("mgm.path"));

      if (gOFS.CallManager(&error, capOpaque->Get("mgm.path"),
                           capOpaque->Get("mgm.manager"), OpaqueString)) {
        eos_err("failed to execute 'adjustreplica' for path=%s",
                capOpaque->Get("mgm.path"));
        gOFS.Emsg(epname, error, EIO, "create all replicas - uploaded file is "
                  "at risk - only one replica has been successfully stored for fn=",
                  capOpaque->Get("mgm.path"));
      } else {
        if (!brc) {
          // Reset the return code and clean error message
          rc = 0;
          gOFS.Emsg(epname, error, 0, "no error");
        }
      }

      eos_warning("executed 'adjustreplica' for path=%s - file is at low risk "
                  "due to missing replica's", capOpaque->Get("mgm.path"));
    }
  }

  if (!rc && (eventOnClose || syncEventOnClose) && layOut->IsEntryServer()) {
    //trigger an MGM event if asked from the entry point
    XrdOucString capOpaqueFile = "";
    XrdOucString eventType = "";
    capOpaqueFile += "/?";
    int envlen = 0;
    capOpaqueFile += capOpaque->Env(envlen);
    capOpaqueFile += "&mgm.pcmd=event";

    // Set default workflow if nothing is specified
    if (eventWorkflow.length() == 0) {
      eventWorkflow = "default";
    }

    if (isRW) {
      eventType = syncEventOnClose ? "sync::closew" : "closew";
    } else {
      eventType = "closer";
    }

    capOpaqueFile += "&mgm.event=";
    capOpaqueFile += eventType;

    // The log ID to the commit
    capOpaqueFile += "&mgm.logid=";
    capOpaqueFile += logId;
    capOpaqueFile += "&mgm.ruid=";
    capOpaqueFile += capOpaque->Get("mgm.ruid");
    capOpaqueFile += "&mgm.rgid=";
    capOpaqueFile += capOpaque->Get("mgm.rgid");
    capOpaqueFile += "&mgm.sec=";
    capOpaqueFile += capOpaque->Get("mgm.sec");

    capOpaqueFile += "&mgm.workflow=";
    capOpaqueFile += eventWorkflow.c_str();

    eos_info("msg=\"notify\" event=\"%s\" workflow=\"%s\"", eventType.c_str(),
             eventWorkflow.c_str());
    rc = gOFS.CallManager(&error, capOpaque->Get("mgm.path"),
                          capOpaque->Get("mgm.manager"), capOpaqueFile, nullptr, 30, false);
  }

  eos_info("Return code rc=%i.", rc);
  return rc;
}

//------------------------------------------------------------------------------
// Read - low level ofs method which is called from one of the layout plugins
//------------------------------------------------------------------------------
XrdSfsXferSize
XrdFstOfsFile::readofs(XrdSfsFileOffset fileOffset, char* buffer,
                       XrdSfsXferSize buffer_size)
{
  gettimeofday(&cTime, &tz);
  rCalls++;
  int rc = XrdOfsFile::read(fileOffset, buffer, buffer_size);
  eos_debug("read %llu %llu %i rc=%d", this, fileOffset, buffer_size, rc);

  if (gOFS.Simulate_IO_read_error) {
    return gOFS.Emsg("readofs", error, EIO, "read file - simulated IO error fn=",
                     capOpaque ? (capOpaque->Get("mgm.path") ?
                                  capOpaque->Get("mgm.path") : FName()) : FName());
  }

  // Account seeks for monitoring
  if (rOffset != static_cast<unsigned long long>(fileOffset)) {
    if (rOffset < static_cast<unsigned long long>(fileOffset)) {
      nFwdSeeks++;
      sFwdBytes += (fileOffset - rOffset);
    } else {
      nBwdSeeks++;
      sBwdBytes += (rOffset - fileOffset);
    }

    if ((rOffset + (EOS_FSTOFS_LARGE_SEEKS)) < (static_cast<unsigned long long>
        (fileOffset))) {
      sXlFwdBytes += (fileOffset - rOffset);
      nXlFwdSeeks++;
    }

    if ((static_cast<unsigned long long>(rOffset) > (EOS_FSTOFS_LARGE_SEEKS)) &&
        (rOffset - (EOS_FSTOFS_LARGE_SEEKS)) > (static_cast<unsigned long long>
            (fileOffset))) {
      sXlBwdBytes += (rOffset - fileOffset);
      nXlBwdSeeks++;
    }
  }

  if (rc > 0) {
    if (layOut->IsEntryServer() || IsRainLayout(lid)) {
      XrdSysMutexHelper vecLock(vecMutex);
      rvec.push_back(rc);
    }

    rOffset = fileOffset + rc;
  }

  gettimeofday(&lrTime, &tz);
  AddReadTime();
  return rc;
}

//------------------------------------------------------------------------------
// Read method
//------------------------------------------------------------------------------
int
XrdFstOfsFile::read(XrdSfsFileOffset fileOffset, XrdSfsXferSize amount)
{
  //  EPNAME("read");
  int rc = XrdOfsFile::read(fileOffset, amount);
  eos_debug("rc=%d offset=%lu size=%llu", rc, fileOffset, amount);
  return rc;
}

//------------------------------------------------------------------------------
// Read
//------------------------------------------------------------------------------
XrdSfsXferSize
XrdFstOfsFile::read(XrdSfsFileOffset fileOffset, char* buffer,
                    XrdSfsXferSize buffer_size)
{
  eos_debug("fileOffset=%lli, buffer_size=%i", fileOffset, buffer_size);

  //  EPNAME("read");
  if (mTpcFlag == kTpcSrcRead) {
    if (!(rCalls % 10)) {
      if (!TpcValid()) {
        eos_err("msg=\"tcp interrupted by control-c - cancel tcp read\" key=%s",
                TpcKey.c_str());
        return gOFS.Emsg("read", error, EINTR, "read - tpc transfer interrupted"
                         " by client disconnect", FName());
      }
    }
  }

  int rc = layOut->Read(fileOffset, buffer, buffer_size);
  eos_debug("layout read %d checkSum %d", rc, checkSum);

  if ((rc > 0) && (checkSum)) {
    XrdSysMutexHelper cLock(ChecksumMutex);
    checkSum->Add(buffer, static_cast<size_t>(rc),
                  static_cast<off_t>(fileOffset));
  }

  if (rc > 0) {
    rOffset = fileOffset + rc;
  }

  gettimeofday(&lrTime, &tz);
  AddReadTime();

  if (rc < 0) {
    // Here we might take some other action
    int envlen = 0;
    eos_crit("block-read error=%d offset=%llu len=%llu file=%s",
             error.getErrInfo(),
             static_cast<unsigned long long>(fileOffset),
             static_cast<unsigned long long>(buffer_size),
             FName(), capOpaque ? capOpaque->Env(envlen) : FName());
    hasReadError =
      true; // used to understand if a reconstruction of a RAIN file worked
  }

  eos_debug("rc=%d offset=%lu size=%llu", rc, fileOffset,
            static_cast<unsigned long long>(buffer_size));

  if ((fileOffset + buffer_size) >= openSize) {
    if (checkSum) {
      if (!checkSum->NeedsRecalculation()) {
        // If this is the last read of sequential reading, we can verify the checksum now
        if (verifychecksum()) {
          return gOFS.Emsg("read", error, EIO, "read file - wrong file checksum fn=",
                           FName());
        }
      }
    }
  }

  return rc;
}

//------------------------------------------------------------------------------
// Vector read - low level ofs method which is called from one of the
// layout plugins
//------------------------------------------------------------------------------
XrdSfsXferSize
XrdFstOfsFile::readvofs(XrdOucIOVec* readV,
                        uint32_t readCount)
{
  eos_debug("read count=%i", readCount);
  gettimeofday(&cTime, &tz);
  XrdSfsXferSize sz = XrdOfsFile::readv(readV, readCount);
  gettimeofday(&lrvTime, &tz);
  AddReadVTime();
  // Collect monitoring info
  {
    XrdSysMutexHelper scope_lock(vecMutex);

    // If this is the last read of sequential reading, we can verify the checksum now
    for (uint32_t i = 0; i < readCount; ++i) {
      monReadSingleBytes.push_back(readV[i].size);
    }

    monReadvBytes.push_back(sz);
    monReadvCount.push_back(readCount);
  }
  return sz;
}

//------------------------------------------------------------------------------
// Vector read - OFS interface method
//------------------------------------------------------------------------------
XrdSfsXferSize
XrdFstOfsFile::readv(XrdOucIOVec* readV,
                     int readCount)
{
  eos_debug("read count=%i", readCount);
  // Copy the XrdOucIOVec structure to XrdCl::ChunkList
  uint32_t total_read = 0;
  XrdCl::ChunkList chunkList;
  chunkList.reserve(readCount);

  for (int i = 0; i < readCount; ++i) {
    total_read += (uint32_t)readV[i].size;
    chunkList.push_back(XrdCl::ChunkInfo((uint64_t)readV[i].offset,
                                         (uint32_t)readV[i].size,
                                         (void*)readV[i].data));
  }

  return layOut->ReadV(chunkList, total_read);
}

//------------------------------------------------------------------------------
// Read AIO
//------------------------------------------------------------------------------
int
XrdFstOfsFile::read(XrdSfsAio* aioparm)
{
  return SFS_ERROR;
}

//------------------------------------------------------------------------------
// Write to OFS file
//------------------------------------------------------------------------------
XrdSfsXferSize
XrdFstOfsFile::writeofs(XrdSfsFileOffset fileOffset,
                        const char* buffer,
                        XrdSfsXferSize buffer_size)
{
  if (gOFS.Simulate_IO_write_error) {
    writeErrorFlag = kOfsSimulatedIoError;
    return gOFS.Emsg("writeofs", error, EIO, "write file - simulated IO error fn=",
                     capOpaque ? (capOpaque->Get("mgm.path") ?
                                  capOpaque->Get("mgm.path") : FName()) : FName());
  }

  if (fsid) {
    if ((targetsize && (targetsize == bookingsize)) ||
        (bookingsize >= fileOffset + buffer_size)) {
      // Space has been successfully pre-allocated, let client write
    } else {
      // Check if the file system is full
      bool isfull = false;
      {
        XrdSysMutexHelper(gOFS.Storage->mFsFullMapMutex);
        isfull = gOFS.Storage->mFsFullMap[fsid];
      }

      if (isfull) {
        writeErrorFlag = kOfsDiskFullError;
        return gOFS.Emsg("writeofs", error, ENOSPC, "write file - disk space "
                         "(headroom) exceeded fn=", capOpaque ?
                         (capOpaque->Get("mgm.path") ? capOpaque->Get("mgm.path") :
                          FName()) : FName());
      }
    }
  }

  if (maxsize) {
    // Check that the user didn't exceed the maximum file size policy
    if ((fileOffset + buffer_size) > maxsize) {
      writeErrorFlag = kOfsMaxSizeError;
      return gOFS.Emsg("writeofs", error, ENOSPC, "write file - your file "
                       "exceeds the maximum file size setting of bytes<=",
                       capOpaque ? (capOpaque->Get("mgm.maxsize") ?
                                    capOpaque->Get("mgm.maxsize") : "<undef>") : "undef");
    }
  }

  gettimeofday(&cTime, &tz);
  wCalls++;
  int rc = XrdOfsFile::write(fileOffset, buffer, buffer_size);

  if (rc != buffer_size) {
    // Tag an io error
    writeErrorFlag = kOfsIoError;
  }

  // Account seeks for monitoring
  if (wOffset != static_cast<unsigned long long>(fileOffset)) {
    if (wOffset < static_cast<unsigned long long>(fileOffset)) {
      nFwdSeeks++;
      sFwdBytes += (fileOffset - wOffset);
    } else {
      nBwdSeeks++;
      sBwdBytes += (wOffset - fileOffset);
    }

    if ((wOffset + (EOS_FSTOFS_LARGE_SEEKS)) < (static_cast<unsigned long long>
        (fileOffset))) {
      sXlFwdBytes += (fileOffset - wOffset);
      nXlFwdSeeks++;
    }

    if ((static_cast<unsigned long long>(wOffset) > (EOS_FSTOFS_LARGE_SEEKS)) &&
        (wOffset - (EOS_FSTOFS_LARGE_SEEKS)) > (static_cast<unsigned long long>
            (fileOffset))) {
      sXlBwdBytes += (wOffset - fileOffset);
      nXlBwdSeeks++;
    }
  }

  if (rc > 0) {
    if (layOut->IsEntryServer() || IsRainLayout(lid)) {
      XrdSysMutexHelper(vecMutex);
      wvec.push_back(rc);
    }

    wOffset = fileOffset + rc;
  }

  gettimeofday(&lwTime, &tz);
  AddWriteTime();
  return rc;
}

//------------------------------------------------------------------------------
// Write
//------------------------------------------------------------------------------
XrdSfsXferSize
XrdFstOfsFile::write(XrdSfsFileOffset fileOffset,
                     const char* buffer,
                     XrdSfsXferSize buffer_size)
{
  int rc = layOut->Write(fileOffset, const_cast<char*>(buffer), buffer_size);

  //  EPNAME("write");
  if ((rc < 0) && isCreation && (error.getErrInfo() == EREMOTEIO)) {
    if (eos::common::LayoutId::GetLayoutType(lid) ==
        eos::common::LayoutId::kReplica) {
      // If we see a remote IO error, we don't fail, we just call a repair
      // action afterwards (only for replica layouts!)
      repairOnClose = true;
      rc = buffer_size;
    }
  }

  // Evt. add checksum
  if ((rc > 0) && (checkSum)) {
    XrdSysMutexHelper cLock(ChecksumMutex);
    checkSum->Add(buffer,
                  static_cast<size_t>(rc),
                  static_cast<off_t>(fileOffset));
  }

  if (rc > 0) {
    if (static_cast<unsigned long long>(fileOffset + buffer_size) >
        static_cast<unsigned long long>(maxOffsetWritten)) {
      maxOffsetWritten = (fileOffset + buffer_size);
    }
  }

  haswrite = true;
  eos_debug("rc=%d offset=%lu size=%lu", rc, fileOffset,
            static_cast<unsigned long>(buffer_size));

  if (rc < 0) {
    int envlen = 0;

    if (!hasWriteError || EOS_LOGS_DEBUG) {
      eos_crit("block-write error=%d offset=%llu len=%llu file=%s",
               error.getErrInfo(),
               static_cast<unsigned long long>(fileOffset),
               static_cast<unsigned long long>(buffer_size),
               FName(), capOpaque ? capOpaque->Env(envlen) : FName());
    }

    hasWriteError = true;
  }

  if (rc < 0) {
    int envlen = 0;
    // Indicate the deletion flag for write errors
    writeDelete = true;
    XrdOucString errdetail;

    if (isCreation) {
      XrdOucString newerr;
      // Add to the error message that this file has been removed after the error,
      // which happens for creations
      newerr = error.getErrText();

      if (writeErrorFlag == kOfsSimulatedIoError) {
        // Simulated IO error
        errdetail += " => file has been removed because of a simulated IO error";
      } else {
        if (writeErrorFlag == kOfsDiskFullError) {
          // Disk full error
          errdetail +=
            " => file has been removed because the target filesystem  was full";
        } else {
          if (writeErrorFlag == kOfsMaxSizeError) {
            // Maximum file size error
            errdetail += " => file has been removed because the maximum target "
                         "filesize defined for that subtree was exceeded (maxsize=";
            char smaxsize[16];
            snprintf(smaxsize, sizeof(smaxsize) - 1, "%llu", (unsigned long long) maxsize);
            errdetail += smaxsize;
            errdetail += " bytes)";
          } else {
            if (writeErrorFlag == kOfsIoError) {
              // Generic IO error
              errdetail +=
                " => file has been removed due to an IO error on the target filesystem";
            } else {
              errdetail += " => file has been removed due to an IO error (unspecified)";
            }
          }
        }
      }

      newerr += errdetail.c_str();
      error.setErrInfo(error.getErrInfo(), newerr.c_str());
    }

    eos_err("block-write error=%d offset=%llu len=%llu file=%s error=\"%s\"",
            error.getErrInfo(),
            (unsigned long long) fileOffset,
            (unsigned long long) buffer_size, FName(),
            capOpaque ? capOpaque->Env(envlen) : FName(),
            errdetail.c_str());
  }

  return rc;
}

//------------------------------------------------------------------------------
// Write AIO
//------------------------------------------------------------------------------
int
XrdFstOfsFile::write(XrdSfsAio* aioparm)
{
  return SFS_ERROR;
}

//------------------------------------------------------------------------------
// Sync OFS
//------------------------------------------------------------------------------
int
XrdFstOfsFile::syncofs()
{
  return XrdOfsFile::sync();
}

//------------------------------------------------------------------------------
// Verify if a TPC key is still valid
//------------------------------------------------------------------------------
bool
XrdFstOfsFile::TpcValid()
{
  XrdSysMutexHelper scope_lock(gOFS.TpcMapMutex);

  if (TpcKey.length() &&  gOFS.TpcMap[isRW].count(TpcKey.c_str())) {
    return true;
  }

  return false;
}

//------------------------------------------------------------------------------
// Sync file
//------------------------------------------------------------------------------
int
XrdFstOfsFile::sync()
{
  static const int cbWaitTime = 1800;

  // TPC transfer
  if (mTpcFlag == kTpcDstSetup) {
    XrdSysMutexHelper scope_lock(&mTpcJobMutex);

    if (mTpcState == kTpcIdle) {
      eos_info("msg=\"tpc enabled -> 1st sync\"");
      mTpcThreadStatus = XrdSysThread::Run(&mTpcThread,
                                           XrdFstOfsFile::StartDoTpcTransfer,
                                           static_cast<void*>(this), XRDSYSTHREAD_HOLD,
                                           "TPC Transfer Thread");

      if (mTpcThreadStatus == 0) {
        mTpcState = kTpcRun;
        scope_lock.UnLock();
        return SFS_OK;
      } else {
        eos_err("msg=\"failed to start TPC job thread\"");
        mTpcState = kTpcDone;

        if (mTpcInfo.Key) {
          free(mTpcInfo.Key);
        }

        mTpcInfo.Key = strdup("Copy failed, could not start job");
        return mTpcInfo.Fail(&error, "could not start job", ECANCELED);
      }
    } else if (mTpcState == kTpcRun) {
      eos_info("msg=\"tpc running -> 2nd sync\"");

      if (mTpcInfo.SetCB(&error)) {
        return SFS_ERROR;
      }

      error.setErrCode(cbWaitTime);
      mTpcInfo.Engage();
      return SFS_STARTED;
    } else if (mTpcState == kTpcDone) {
      eos_info("msg=\"tpc already finished, retc=%i\"", mTpcRetc);

      if (mTpcRetc) {
        error.setErrInfo(mTpcRetc, (mTpcInfo.Key ? mTpcInfo.Key : "failed tpc"));
        return SFS_ERROR;
      } else {
        return SFS_OK;
      }
    } else {
      eos_err("msg=\"unknown tpc state\"");
      error.setErrInfo(EINVAL, "unknown TPC state");
      return SFS_ERROR;
    }
  } else {
    // Standard file sync
    return layOut->Sync();
  }
}

//------------------------------------------------------------------------------
// Sync
//------------------------------------------------------------------------------
int
XrdFstOfsFile::sync(XrdSfsAio* aiop)
{
  return layOut->Sync();
}

//----------------------------------------------------------------------------
// Static method used to start an asynchronous thread which is doing the TPC
// transfer
//----------------------------------------------------------------------------
void*
XrdFstOfsFile::StartDoTpcTransfer(void* arg)
{
  return reinterpret_cast<XrdFstOfsFile*>(arg)->DoTpcTransfer();
}

//------------------------------------------------------------------------------
// Run method for the thread doing the TPC transfer
//------------------------------------------------------------------------------
void*
XrdFstOfsFile::DoTpcTransfer()
{
  eos_info("msg=\"tpc now running - 1st sync\"");
  std::string src_url = "";
  std::string src_cgi = "";

  // The sync initiates the third party copy
  if (!TpcValid()) {
    eos_err("msg=\"tpc session invalidated during sync\"");
    XrdSysMutexHelper scope_lock(mTpcJobMutex);
    mTpcState = kTpcDone;
    mTpcRetc = ECONNABORTED;
    mTpcInfo.Reply(SFS_ERROR, ECONNABORTED, "sync TPC session closed by "
                   "disconnect");
    return 0;
  }

  {
    XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex);
    // Construct the source URL
    src_url = "root://";
    src_url += gOFS.TpcMap[isRW][TpcKey.c_str()].src;
    src_url += "/";
    src_url += gOFS.TpcMap[isRW][TpcKey.c_str()].lfn;
    src_url += "?fst.readahead=true";
    src_cgi = "tpc.key=";
    src_cgi += TpcKey.c_str();
    src_cgi += "&tpc.org=";
    src_cgi += gOFS.TpcMap[isRW][TpcKey.c_str()].org;
  }

  XrdIo tpcIO(src_url.c_str());
  eos_info("sync-url=%s sync-cgi=%s", src_url.c_str(), src_cgi.c_str());

  if (tpcIO.fileOpen(0, 0, src_cgi.c_str())) {
    eos_err("msg=\"TPC open failed for url=%s cgi=%s\"", src_url.c_str(),
            src_cgi.c_str());
    XrdSysMutexHelper scope_lock(mTpcJobMutex);
    mTpcState = kTpcDone;
    mTpcRetc = EFAULT;
    mTpcInfo.Reply(SFS_ERROR, EFAULT, "sync - TPC open failed");
    return 0;
  }

  if (!TpcValid()) {
    tpcIO.fileClose();
    eos_err("msg=\"tpc session invalidated during sync\"");
    XrdSysMutexHelper scope_lock(mTpcJobMutex);
    mTpcState = kTpcDone;
    mTpcRetc = ECONNABORTED;
    mTpcInfo.Reply(SFS_ERROR, ECONNABORTED, "sync - TPC session closed by "
                   "disconnect");
    return 0;
  }

  int64_t rbytes = 0;
  int64_t wbytes = 0;
  off_t offset = 0;
  std::unique_ptr< std::vector<char> > buffer(
    new std::vector<char>(tpcIO.GetBlockSize()));
  eos_info("msg=\"tpc pull\" ");

  do {
    // Read the remote file in chunks and check after each chunk if the TPC
    // has been aborted already
    // @note this way of reading asynchronously in the buffer without waiting
    // for the async requests works properly only if readahead is enabled.
    // Otherwise, one must call fileWaitAsyncIO().
    rbytes = tpcIO.fileReadAsync(offset, &((*buffer)[0]),
                                 tpcIO.GetBlockSize(), true, 30);
    eos_debug("msg=\"tpc read\" rbytes=%llu request=%llu",
              rbytes, tpcIO.GetBlockSize());

    if (rbytes == -1) {
      (void) tpcIO.fileClose();
      eos_err("msg=\"tpc transfer terminated - remote read failed\"");
      XrdSysMutexHelper scope_lock(mTpcJobMutex);
      mTpcState = kTpcDone;
      mTpcRetc = EIO;
      mTpcInfo.Reply(SFS_ERROR, EIO, "sync - TPC remote read failed");
      return 0;
    }

    if (rbytes > 0) {
      // Write the buffer out through the local object
      wbytes = write(offset, &((*buffer)[0]), rbytes);
      eos_debug("msg=\"tpc write\" wbytes=%llu", wbytes);

      if (rbytes != wbytes) {
        (void) tpcIO.fileClose();
        eos_err("msg=\"tpc transfer terminated - local write failed\"");
        XrdSysMutexHelper scope_lock(mTpcJobMutex);
        mTpcState = kTpcDone;
        mTpcRetc = EIO;
        mTpcInfo.Reply(SFS_ERROR, EIO, "sync - TPC local write failed");
        return 0;
      }

      offset += rbytes;
    }

    // Check validity of the TPC key
    if (!TpcValid()) {
      (void) tpcIO.fileClose();
      eos_err("msg=\"tpc transfer invalidated during sync\"");
      XrdSysMutexHelper scope_lock(mTpcJobMutex);
      mTpcState = kTpcDone;
      mTpcRetc = ECONNABORTED;
      mTpcInfo.Reply(SFS_ERROR, ECONNABORTED, "sync - TPC session closed "
                     "by diconnect");
      return 0;
    }
  } while (rbytes > 0);

  // Close the remote file
  eos_info("done tpc transfer, close remote file and exit");
  XrdCl::XRootDStatus st = tpcIO.fileClose();
  XrdSysMutexHelper scope_lock(mTpcJobMutex);
  mTpcState = kTpcDone;
  mTpcInfo.Reply(SFS_OK, 0, "");
  return 0;
}

//------------------------------------------------------------------------------
// Truncate - low level ofs method which is called from one of the layout plugin
//------------------------------------------------------------------------------
int
XrdFstOfsFile::truncateofs(XrdSfsFileOffset fileOffset)
{
  // Truncation moves the max offset written
  eos_debug("value=%llu", (unsigned long long) fileOffset);
  maxOffsetWritten = fileOffset;
  struct stat buf;

  // stat the current file size
  // if the file has the proper size we don't truncate
  if (!::stat(fstPath.c_str(), &buf)) {
    // if the file has the proper size we don't truncate
    if (buf.st_size == fileOffset) {
      return SFS_OK;
    }
  }

  return XrdOfsFile::truncate(fileOffset);
}

//------------------------------------------------------------------------------
// Truncate method
//------------------------------------------------------------------------------
int
XrdFstOfsFile::truncate(XrdSfsFileOffset fileOffset)
{
  eos_info("openSize=%llu fileOffset=%llu ", openSize, fileOffset);

  if (fileOffset != openSize) {
    haswrite = true;

    if (checkSum) {
      if (fileOffset != checkSum->GetMaxOffset()) {
        checkSum->Reset();
        checkSum->SetDirty();
      }
    }
  }

  return layOut->Truncate(fileOffset);
}

//------------------------------------------------------------------------------
// Stat file
//------------------------------------------------------------------------------
int
XrdFstOfsFile::stat(struct stat* buf)
{
  EPNAME("stat");
  int rc = SFS_OK;

  if (layOut) {
    if ((rc = layOut->Stat(buf)))
      rc = gOFS.Emsg(epname, error, EIO, "stat - cannot stat layout to determine"
                     " file size ", Path.c_str());
  } else {
    rc = gOFS.Emsg(epname, error, ENXIO, "stat - no layout to determine file size ",
                   Path.c_str());
  }

  // store the file id as inode number
  if (!rc) {
    buf->st_ino = fileid << 28;
  }

  // we store the mtime.ns time in st_dev ... sigh@Xrootd ...
#ifdef __APPLE__
  unsigned long nsec = buf->st_mtimespec.tv_nsec;
#else
  unsigned long nsec = buf->st_mtim.tv_nsec;
#endif
  // mask for 10^9
  nsec &= 0x7fffffff;
  // enable bit 32 as indicator
  nsec |= 0x80000000;
  // overwrite st_dev
  buf->st_dev = nsec;
#ifdef __APPLE__
  eos_info("path=%s inode=%lu size=%lu mtime=%lu.%lu", Path.c_str(), fileid,
           (unsigned long) buf->st_size, buf->st_mtimespec.tv_sec,
           buf->st_dev & 0x7ffffff);
#else
  eos_info("path=%s inode=%lu size=%lu mtime=%lu.%lu", Path.c_str(), fileid,
           (unsigned long) buf->st_size, buf->st_mtim.tv_sec, buf->st_dev & 0x7ffffff);
#endif
  return rc;
}

//------------------------------------------------------------------------------
// Implementation dependant commands
//------------------------------------------------------------------------------
int
XrdFstOfsFile::fctl(const int cmd, int alen, const char* args,
                    const XrdSecEntity* client)
{
  eos_debug("cmd=%i, args=%s", cmd, args);

  if (cmd == SFS_FCTL_SPEC1) {
    if (strncmp(args, "delete", alen) == 0) {
      eos_warning("Setting deletion flag for file %s", fstPath.c_str());
      // This indicates to delete the file during the close operation
      viaDelete = true;
      return SFS_OK;
    } else if (strncmp(args, "nochecksum", alen) == 0) {
      int retc = SFS_OK;
      eos_warning("Setting nochecksum flag for file %s", fstPath.c_str());

      if (checkSum) {
        eos::fst::CheckSum* tmpSum = checkSum;
        checkSum = 0;
        delete tmpSum;
      }

      // Propagate command to all the replicas/stripes
      if (layOut) {
        retc = layOut->Fctl(std::string(args), client);
      }

      return retc;
    }
  }

  error.setErrInfo(ENOTSUP, "fctl command not supported");
  return SFS_ERROR;
}

//------------------------------------------------------------------------------
// Get local path
//------------------------------------------------------------------------------
std::string
XrdFstOfsFile::GetFstPath()
{
  std::string ret = fstPath.c_str();
  return ret;
}

//------------------------------------------------------------------------------
// Filter out particular tags from the opaque information
//------------------------------------------------------------------------------
std::string
XrdFstOfsFile::FilterTags(const std::string& opaque,
                          const std::set<std::string> tags)
{
  bool found = false;
  std::ostringstream oss;
  std::list<std::string> tokens = eos::common::StringTokenizer::split
                                  <std::list<std::string>>(opaque, '&');

  for (const auto& token : tokens) {
    found = false;

    for (const auto& tag : tags) {
      if (token.find(tag) == 0) {
        found = true;
        break;
      }
    }

    if (!found && !token.empty()) {
      oss << token << "&";
    }
  }

  std::string new_opaque = oss.str();

  if (!new_opaque.empty()) {
    new_opaque.pop_back();
  }

  return new_opaque;
}

//------------------------------------------------------------------------------
// Return current mtime while open
//------------------------------------------------------------------------------
time_t
XrdFstOfsFile::GetMtime()
{
  if (fMd) {
    return fMd->mProtoFmd.mtime();
  } else {
    return 0;
  }
}

//------------------------------------------------------------------------------
// Return FMD checksum
//------------------------------------------------------------------------------
std::string
XrdFstOfsFile::GetFmdChecksum()
{
  return fMd->mProtoFmd.checksum();
}

//------------------------------------------------------------------------------
// Check fst validity to avoid any open replays
//------------------------------------------------------------------------------
bool
XrdFstOfsFile::CheckFstValidity(XrdOucEnv& env_opaque) const
{
  using namespace std::chrono;
  char* val;

  if ((val = env_opaque.Get("fst.valid"))) {
    try {
      std::string sval = val;
      int64_t valid_sec = std::stoll(sval);
      auto now = system_clock::now();
      auto now_sec = time_point_cast<seconds>(now).time_since_epoch().count();

      if (valid_sec < now_sec) {
        return false;
      }
    } catch (...) {
      // ignore
    }
  }

  return true;
}

//------------------------------------------------------------------------------
// Check if layout encoding indicates a RAIN layout
//------------------------------------------------------------------------------
bool
XrdFstOfsFile::IsRainLayout(unsigned long long lid)
{
  using eos::common::LayoutId;
  unsigned long ltype = LayoutId::GetLayoutType(lid);

  if ((ltype == LayoutId::kArchive) ||
      (ltype == LayoutId::kRaidDP) ||
      (ltype == LayoutId::kRaid6)) {
    return true;
  } else {
    return false;
  }
}

EOSFSTNAMESPACE_END
