//------------------------------------------------------------------------------
// File: FileSystem.cc
// Author: Andreas-Joachim Peters - CERN
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
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

#include "common/Namespace.hh"
#include "common/FileSystem.hh"
#include "common/Logging.hh"
#include "common/TransferQueue.hh"
#include "common/StringUtils.hh"
#include "common/ParseUtils.hh"

EOSCOMMONNAMESPACE_BEGIN;

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
FileSystemLocator::FileSystemLocator(const std::string &_host, int _port,
  const std::string &_localpath) : host(_host), port(_port),
  localpath(_localpath) {}

//------------------------------------------------------------------------------
// Try to parse a "queuepath"
//------------------------------------------------------------------------------
bool FileSystemLocator::fromQueuePath(const std::string &queuepath,
  FileSystemLocator &out) {

  std::string queue = queuepath;

  if(!startsWith(queue, "/eos/")) {
    return false;
  }

  queue.erase(0, 5);

  //----------------------------------------------------------------------------
  // Chop /eos/, extract host+port
  //----------------------------------------------------------------------------
  size_t slashLocation = queue.find("/");
  if(slashLocation == std::string::npos) {
    return false;
  }

  std::string hostPort = std::string(queue.begin(), queue.begin() + slashLocation);
  queue.erase(0, slashLocation);

  //----------------------------------------------------------------------------
  // Separate host from port
  //----------------------------------------------------------------------------
  size_t separator = hostPort.find(":");
  if(separator == std::string::npos) {
    return false;
  }

  out.host = std::string(hostPort.begin(), hostPort.begin() + separator);
  hostPort.erase(0, separator+1);

  int64_t port;
  if(!parseInt64(hostPort, port)) {
    return false;
  }

  out.port = port;

  //----------------------------------------------------------------------------
  // Chop "/fst/", extract local path
  //----------------------------------------------------------------------------
  if(!startsWith(queue, "/fst/")) {
    return false;
  }

  queue.erase(0, 4);
  out.localpath = queue;

  if(out.localpath.size() < 2) {
    // Empty, or "/"? Reject
    return false;
  }

  return true;
}

//------------------------------------------------------------------------------
// Get host
//------------------------------------------------------------------------------
std::string FileSystemLocator::getHost() const {
  return host;
}

//------------------------------------------------------------------------------
// Get hostport, concatenated together as "host:port"
//------------------------------------------------------------------------------
std::string FileSystemLocator::getHostPort() const {
  return SSTR(host << ":" << port);
}

//------------------------------------------------------------------------------
// Get queuepath
//------------------------------------------------------------------------------
std::string FileSystemLocator::getQueuePath() const {
  return SSTR("/eos/" << host << ":" << port << "/fst" << localpath);
}

//------------------------------------------------------------------------------
// Get port
//------------------------------------------------------------------------------
int FileSystemLocator::getPort() const {
  return port;
}

//------------------------------------------------------------------------------
// Get local path
//------------------------------------------------------------------------------
std::string FileSystemLocator::getLocalPath() const {
  return localpath;
}

//------------------------------------------------------------------------------
// Get transient channel for this filesystem - that is, the channel through
// which all transient, non-important information will be transmitted.
//------------------------------------------------------------------------------
std::string FileSystemLocator::getTransientChannel() const {
  return SSTR("filesystem-transient||" << getHostPort() << "||" << getLocalPath());
}

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
FileSystem::FileSystem(const FileSystemLocator &locator, const char* queue,
  XrdMqSharedObjectManager* som, qclient::SharedManager* qsom, bool bc2mgm)
{
  mSharedManager = qsom;
  mQueuePath = locator.getQueuePath();
  mQueue = queue;
  mPath = locator.getLocalPath();
  mSom = som;
  mInternalBootStatus = BootStatus::kDown;
  cActive = ActiveStatus::kOffline;
  cStatus = BootStatus::kDown;
  cConfigStatus = 0;
  cActiveTime = 0;
  cStatusTime = 0;
  cConfigTime = 0;
  std::string broadcast = queue;

  if (bc2mgm) {
    broadcast = "/eos/*/mgm";
  }

  if (mSom) {
    mSom->HashMutex.LockRead();
    XrdMqSharedHash* hash = nullptr;

    if (!(hash = mSom->GetObject(mQueuePath.c_str(), "hash"))) {
      mSom->HashMutex.UnLockRead();
      // create the hash object
      mSom->CreateSharedHash(mQueuePath.c_str(), broadcast.c_str(), som);
      mSom->HashMutex.LockRead();
      hash = mSom->GetObject(mQueuePath.c_str(), "hash");

      if (hash) {
        hash->OpenTransaction();
        hash->Set("queue", mQueue.c_str());
        hash->Set("queuepath", mQueuePath.c_str());
        hash->Set("path", mPath.c_str());
        hash->Set("hostport", locator.getHostPort().c_str());
        hash->Set("host", locator.getHost().c_str());
        hash->Set("port", std::to_string(locator.getPort()).c_str());
        hash->Set("configstatus", "down");
        hash->Set("stat.drain", "nodrain");

        hash->CloseTransaction();
      }

      mSom->HashMutex.UnLockRead();
    } else {
      hash->SetBroadCastQueue(broadcast.c_str());
      hash->OpenTransaction();
      hash->Set("queue", mQueue.c_str());
      hash->Set("queuepath", mQueuePath.c_str());
      hash->Set("path", mPath.c_str());
      hash->Set("hostport", locator.getHostPort().c_str());
      hash->Set("host", locator.getHost().c_str());
      hash->Set("port", std::to_string(locator.getPort()).c_str());
      hash->Set("stat.drain", "nodrain");

      hash->CloseTransaction();
      mSom->HashMutex.UnLockRead();
    }

    mDrainQueue = new TransferQueue(mQueue.c_str(), mQueuePath.c_str(), "drainq",
                                    mSom, bc2mgm);
    mBalanceQueue = new TransferQueue(mQueue.c_str(), mQueuePath.c_str(),
                                      "balanceq", mSom, bc2mgm);
    mExternQueue = new TransferQueue(mQueue.c_str(), mQueuePath.c_str(), "externq",
                                     mSom, bc2mgm);
  } else {
    mDrainQueue = 0;
    mBalanceQueue = 0;
    mExternQueue = 0;
  }

  if (bc2mgm) {
    BroadCastDeletion = false;
  } else {
    BroadCastDeletion = true;
  }
}

//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
FileSystem::~FileSystem()
{
  // remove the shared hash of this file system
  if (mSom) {
    mSom->DeleteSharedHash(mQueuePath.c_str(), BroadCastDeletion);
  }

  if (mDrainQueue) {
    delete mDrainQueue;
  }

  if (mBalanceQueue) {
    delete mBalanceQueue;
  }

  if (mExternQueue) {
    delete mExternQueue;
  }
}

//------------------------------------------------------------------------------
// Return the given status as a string
//------------------------------------------------------------------------------
const char*
FileSystem::GetStatusAsString(BootStatus status)
{
  if (status == BootStatus::kDown) {
    return "down";
  }

  if (status == BootStatus::kOpsError) {
    return "opserror";
  }

  if (status == BootStatus::kBootFailure) {
    return "bootfailure";
  }

  if (status == BootStatus::kBootSent) {
    return "bootsent";
  }

  if (status == BootStatus::kBooting) {
    return "booting";
  }

  if (status == BootStatus::kBooted) {
    return "booted";
  }

  return "unknown";
}

//------------------------------------------------------------------------------
// Return given drain status as a string
//------------------------------------------------------------------------------
const char*
FileSystem::GetDrainStatusAsString(DrainStatus status)
{
  if (status == DrainStatus::kNoDrain) {
    return "nodrain";
  }

  if (status == DrainStatus::kDrainPrepare) {
    return "prepare";
  }

  if (status == DrainStatus::kDrainWait) {
    return "waiting";
  }

  if (status == DrainStatus::kDraining) {
    return "draining";
  }

  if (status == DrainStatus::kDrained) {
    return "drained";
  }

  if (status == DrainStatus::kDrainStalling) {
    return "stalling";
  }

  if (status == DrainStatus::kDrainExpired) {
    return "expired";
  }

  if (status == DrainStatus::kDrainFailed) {
    return "failed";
  }

  return "unknown";
}

//------------------------------------------------------------------------------
// Return given configuration status as a string
//------------------------------------------------------------------------------
const char*
FileSystem::GetConfigStatusAsString(int status)
{
  if (status == kUnknown) {
    return "unknown";
  }

  if (status == kOff) {
    return "off";
  }

  if (status == kEmpty) {
    return "empty";
  }

  if (status == kDrainDead) {
    return "draindead";
  }

  if (status == kDrain) {
    return "drain";
  }

  if (status == kRO) {
    return "ro";
  }

  if (status == kWO) {
    return "wo";
  }

  if (status == kRW) {
    return "rw";
  }

  return "unknown";
}

//------------------------------------------------------------------------------
// Get the status from a string representation
//------------------------------------------------------------------------------
BootStatus
FileSystem::GetStatusFromString(const char* ss)
{
  if (!ss) {
    return BootStatus::kDown;
  }

  if (!strcmp(ss, "down")) {
    return BootStatus::kDown;
  }

  if (!strcmp(ss, "opserror")) {
    return BootStatus::kOpsError;
  }

  if (!strcmp(ss, "bootfailure")) {
    return BootStatus::kBootFailure;
  }

  if (!strcmp(ss, "bootsent")) {
    return BootStatus::kBootSent;
  }

  if (!strcmp(ss, "booting")) {
    return BootStatus::kBooting;
  }

  if (!strcmp(ss, "booted")) {
    return BootStatus::kBooted;
  }

  return BootStatus::kDown;
}


//------------------------------------------------------------------------------
// Return configuration status from a string representation
//------------------------------------------------------------------------------
int
FileSystem::GetConfigStatusFromString(const char* ss)
{
  if (!ss) {
    return kOff;
  }

  if (!strcmp(ss, "unknown")) {
    return kUnknown;
  }

  if (!strcmp(ss, "off")) {
    return kOff;
  }

  if (!strcmp(ss, "empty")) {
    return kEmpty;
  }

  if (!strcmp(ss, "draindead")) {
    return kDrainDead;
  }

  if (!strcmp(ss, "drain")) {
    return kDrain;
  }

  if (!strcmp(ss, "ro")) {
    return kRO;
  }

  if (!strcmp(ss, "wo")) {
    return kWO;
  }

  if (!strcmp(ss, "rw")) {
    return kRW;
  }

  if (!strcmp(ss, "down")) {
    return kOff;
  }

  return kUnknown;
}

//------------------------------------------------------------------------------
// Return drains status from string representation
//------------------------------------------------------------------------------
DrainStatus
FileSystem::GetDrainStatusFromString(const char* ss)
{
  if (!ss) {
    return DrainStatus::kNoDrain;
  }

  if (!strcmp(ss, "nodrain")) {
    return DrainStatus::kNoDrain;
  }

  if (!strcmp(ss, "prepare")) {
    return DrainStatus::kDrainPrepare;
  }

  if (!strcmp(ss, "wait")) {
    return DrainStatus::kDrainWait;
  }

  if (!strcmp(ss, "draining")) {
    return DrainStatus::kDraining;
  }

  if (!strcmp(ss, "stalling")) {
    return DrainStatus::kDrainStalling;
  }

  if (!strcmp(ss, "drained")) {
    return DrainStatus::kDrained;
  }

  if (!strcmp(ss, "expired")) {
    return DrainStatus::kDrainExpired;
  }

  if (!strcmp(ss, "failed")) {
    return DrainStatus::kDrainFailed;
  }

  return DrainStatus::kNoDrain;
}

//------------------------------------------------------------------------------
// Return active status from a string representation
//------------------------------------------------------------------------------
ActiveStatus
FileSystem::GetActiveStatusFromString(const char* ss)
{
  if (!ss) {
    return ActiveStatus::kOffline;
  }

  if (!strcmp(ss, "online")) {
    return ActiveStatus::kOnline;
  }

  if (!strcmp(ss, "offline")) {
    return ActiveStatus::kOffline;
  }

  return ActiveStatus::kOffline;
}

//------------------------------------------------------------------------------
// Return register request string
//------------------------------------------------------------------------------
const char*
FileSystem::GetRegisterRequestString()
{
  return "mgm.cmd=register";
}

//------------------------------------------------------------------------------
// Store a configuration key-val pair.
// Internally, these keys are not prefixed with 'stat.'
//------------------------------------------------------------------------------
void
FileSystem::CreateConfig(std::string& key, std::string& val)
{
  key = val = "";
  RWMutexReadLock lock(mSom->HashMutex);
  key = mQueuePath;
  XrdMqSharedHash* hash = mSom->GetObject(mQueuePath.c_str(), "hash");
  val = hash->SerializeWithFilter("stat.", true);
}

//------------------------------------------------------------------------------
// Snapshots all variables of a filesystem into a snapshot struct
//------------------------------------------------------------------------------
bool
FileSystem::SnapShotFileSystem(FileSystem::fs_snapshot_t& fs, bool dolock)
{
  if (dolock) {
    mSom->HashMutex.LockRead();
  }

  XrdMqSharedHash* hash = nullptr;

  if ((hash = mSom->GetObject(mQueuePath.c_str(), "hash"))) {
    fs.mId = (fsid_t) hash->GetUInt("id");
    fs.mQueue = mQueue;
    fs.mQueuePath = mQueuePath;
    fs.mGroup = hash->Get("schedgroup");
    fs.mUuid = hash->Get("uuid");
    fs.mHost = hash->Get("host");
    fs.mHostPort = hash->Get("hostport");
    fs.mProxyGroup = hash->Get("proxygroup");
    fs.mS3Credentials = hash->Get("s3credentials");
    fs.mFileStickyProxyDepth = -1;

    if (hash->Get("filestickyproxydepth").size()) {
      fs.mFileStickyProxyDepth = hash->GetLongLong("filestickyproxydepth");
    }

    fs.mPort = hash->Get("port");
    std::string::size_type dpos = 0;

    if ((dpos = fs.mGroup.find(".")) != std::string::npos) {
      std::string s = fs.mGroup;
      s.erase(0, dpos + 1);
      fs.mGroupIndex = atoi(s.c_str());
    } else {
      fs.mGroupIndex = 0;
    }

    fs.mSpace = fs.mGroup;

    if (dpos != std::string::npos) {
      fs.mSpace.erase(dpos);
    }

    fs.mPath = mPath;
    fs.mErrMsg = hash->Get("stat.errmsg");
    fs.mGeoTag = hash->Get("stat.geotag");
    fs.mForceGeoTag.clear();

    if (hash->Get("forcegeotag").size()) {
      std::string forceGeoTag = hash->Get("forcegeotag");

      if (forceGeoTag != "<none>") {
        fs.mGeoTag = forceGeoTag;
        fs.mForceGeoTag = forceGeoTag;
      }
    }

    fs.mPublishTimestamp = (size_t)hash->GetLongLong("stat.publishtimestamp");
    fs.mStatus = GetStatusFromString(hash->Get("stat.boot").c_str());
    fs.mConfigStatus = GetConfigStatusFromString(
                         hash->Get("configstatus").c_str());
    fs.mDrainStatus = GetDrainStatusFromString(hash->Get("stat.drain").c_str());
    fs.mActiveStatus = GetActiveStatusFromString(hash->Get("stat.active").c_str());
    //headroom can be configured as KMGTP so the string should be properly converted
    fs.mHeadRoom = StringConversion::GetSizeFromString(hash->Get("headroom"));
    fs.mErrCode = (unsigned int) hash->GetLongLong("stat.errc");
    fs.mBootSentTime = (time_t) hash->GetLongLong("stat.bootsenttime");
    fs.mBootDoneTime = (time_t) hash->GetLongLong("stat.bootdonetime");
    fs.mHeartBeatTime = (time_t) hash->GetLongLong("stat.heartbeattime");
    fs.mDiskUtilization = hash->GetDouble("stat.disk.load");
    fs.mNetEthRateMiB = hash->GetDouble("stat.net.ethratemib");
    fs.mNetInRateMiB = hash->GetDouble("stat.net.inratemib");
    fs.mNetOutRateMiB = hash->GetDouble("stat.net.outratemib");
    fs.mDiskWriteRateMb = hash->GetDouble("stat.disk.writeratemb");
    fs.mDiskReadRateMb = hash->GetDouble("stat.disk.readratemb");
    fs.mDiskType = (long) hash->GetLongLong("stat.statfs.type");
    fs.mDiskFreeBytes = hash->GetLongLong("stat.statfs.freebytes");
    fs.mDiskCapacity = hash->GetLongLong("stat.statfs.capacity");
    fs.mDiskBsize = (long) hash->GetLongLong("stat.statfs.bsize");
    fs.mDiskBlocks = (long) hash->GetLongLong("stat.statfs.blocks");
    fs.mDiskBfree = (long) hash->GetLongLong("stat.statfs.bfree");
    fs.mDiskBused = (long) hash->GetLongLong("stat.statfs.bused");
    fs.mDiskBavail = (long) hash->GetLongLong("stat.statfs.bavail");
    fs.mDiskFiles = (long) hash->GetLongLong("stat.statfs.files");
    fs.mDiskFfree = (long) hash->GetLongLong("stat.statfs.ffree");
    fs.mDiskFused = (long) hash->GetLongLong("stat.statfs.fused");
    fs.mDiskFilled = (double) hash->GetDouble("stat.statfs.filled");
    fs.mNominalFilled = (double) hash->GetDouble("stat.nominal.filled");
    fs.mFiles = (long) hash->GetLongLong("stat.usedfiles");
    fs.mDiskNameLen = (long) hash->GetLongLong("stat.statfs.namelen");
    fs.mDiskRopen = (long) hash->GetLongLong("stat.ropen");
    fs.mDiskWopen = (long) hash->GetLongLong("stat.wopen");
    fs.mWeightRead = 1.0;
    fs.mWeightWrite = 1.0;
    fs.mScanRate = (time_t) hash->GetLongLong("scanrate");
    fs.mScanInterval = (time_t) hash->GetLongLong("scaninterval");
    fs.mGracePeriod = (time_t) hash->GetLongLong("graceperiod");
    fs.mDrainPeriod = (time_t) hash->GetLongLong("drainperiod");
    fs.mDrainerOn   = (hash->Get("stat.drainer") == "on");
    fs.mBalThresh   = hash->GetDouble("stat.balance.threshold");

    if (dolock) {
      mSom->HashMutex.UnLockRead();
    }

    return true;
  } else {
    if (dolock) {
      mSom->HashMutex.UnLockRead();
    }

    fs.mId = 0;
    fs.mQueue = "";
    fs.mQueuePath = "";
    fs.mGroup = "";
    fs.mPath = "";
    fs.mUuid = "";
    fs.mHost = "";
    fs.mHostPort = "";
    fs.mProxyGroup = "";
    fs.mS3Credentials = "";
    fs.mFileStickyProxyDepth = -1;
    fs.mPort = "";
    fs.mErrMsg = "";
    fs.mGeoTag = "";
    fs.mPublishTimestamp = 0;
    fs.mStatus = BootStatus::kDown;
    fs.mConfigStatus = 0;
    fs.mDrainStatus = DrainStatus::kNoDrain;
    fs.mHeadRoom = 0;
    fs.mErrCode = 0;
    fs.mBootSentTime = 0;
    fs.mBootDoneTime = 0;
    fs.mHeartBeatTime = 0;
    fs.mDiskUtilization = 0;
    fs.mNetEthRateMiB = 0;
    fs.mNetInRateMiB = 0;
    fs.mNetOutRateMiB = 0;
    fs.mDiskWriteRateMb = 0;
    fs.mDiskReadRateMb = 0;
    fs.mDiskType = 0;
    fs.mDiskBsize = 0;
    fs.mDiskBlocks = 0;
    fs.mDiskBfree = 0;
    fs.mDiskBused = 0;
    fs.mDiskBavail = 0;
    fs.mDiskFiles = 0;
    fs.mDiskFfree = 0;
    fs.mDiskFused = 0;
    fs.mFiles = 0;
    fs.mDiskNameLen = 0;
    fs.mDiskRopen = 0;
    fs.mDiskWopen = 0;
    fs.mScanRate = 0;
    fs.mDrainerOn = false;
    fs.mBalThresh = 0.0;
    return false;
  }
}

//------------------------------------------------------------------------------
// Snapshots all variables of a filesystem into a snapshot struct
//------------------------------------------------------------------------------
bool
FileSystem::SnapShotHost(XrdMqSharedObjectManager* som,
                         const std::string& queue,
                         FileSystem::host_snapshot_t& host, bool dolock)
{
  if (dolock) {
    som->HashMutex.LockRead();
  }

  XrdMqSharedHash* hash = NULL;

  if ((hash = som->GetObject(queue.c_str(), "hash"))) {
    host.mQueue = queue;
    host.mHost        = hash->Get("stat.host");
    host.mHostPort      = hash->Get("stat.hostport");
    host.mGeoTag        = hash->Get("stat.geotag");
    host.mPublishTimestamp = hash->GetLongLong("stat.publishtimestamp");
    host.mActiveStatus = GetActiveStatusFromString(
                           hash->Get("stat.active").c_str());
    host.mNetEthRateMiB = hash->GetDouble("stat.net.ethratemib");
    host.mNetInRateMiB  = hash->GetDouble("stat.net.inratemib");
    host.mNetOutRateMiB = hash->GetDouble("stat.net.outratemib");
    host.mGopen = hash->GetLongLong("stat.dataproxy.gopen");

    if (dolock) {
      som->HashMutex.UnLockRead();
    }

    return true;
  } else {
    if (dolock) {
      som->HashMutex.UnLockRead();
    }

    host.mQueue = queue;
    host.mHost = "";
    host.mHostPort = "";
    host.mGeoTag        = "";
    host.mPublishTimestamp = 0;
    host.mActiveStatus = ActiveStatus::kOffline;
    host.mNetEthRateMiB = 0;
    host.mNetInRateMiB  = 0;
    host.mNetOutRateMiB = 0;
    host.mGopen = 0;
    return false;
  }
}

//----------------------------------------------------------------------------
// Return the configuration status (via cache)
//----------------------------------------------------------------------------
FileSystem::fsstatus_t
FileSystem::GetConfigStatus(bool cached)
{
  fsstatus_t rConfigStatus = 0;
  XrdSysMutexHelper lock(cConfigLock);

  if (cached) {
    time_t now = time(NULL);

    if (now - cConfigTime) {
      cConfigTime = now;
    } else {
      rConfigStatus = cConfigStatus;
      return rConfigStatus;
    }
  }

  cConfigStatus = GetConfigStatusFromString(GetString("configstatus").c_str());
  rConfigStatus = cConfigStatus;
  return rConfigStatus;
}

//----------------------------------------------------------------------------
// Return the filesystem status (via a cache)
//----------------------------------------------------------------------------
BootStatus
FileSystem::GetStatus(bool cached)
{
  XrdSysMutexHelper lock(cStatusLock);

  if (cached) {
    time_t now = time(NULL);

    if (now - cStatusTime) {
      cStatusTime = now;
    } else {
      return cStatus;
    }
  }

  cStatus = GetStatusFromString(GetString("stat.boot").c_str());
  return cStatus;
}

//----------------------------------------------------------------------------
// Function printing the file system info to the table
//----------------------------------------------------------------------------
void
FileSystem::Print(TableHeader& table_mq_header, TableData& table_mq_data,
                  std::string listformat, const std::string& filter)
{
  XrdMqSharedHash* hash = nullptr;
  RWMutexReadLock lock(mSom->HashMutex);

  if ((hash = mSom->GetObject(mQueuePath.c_str(), "hash"))) {
    hash->Print(table_mq_header, table_mq_data, listformat, filter);
  }
}

//----------------------------------------------------------------------------
// Get the activation status via a cache.
// This can be used with a small cache which 1s expiration time to avoid too
// many lookup's in tight loops.
//----------------------------------------------------------------------------
ActiveStatus
FileSystem::GetActiveStatus(bool cached)
{
  XrdSysMutexHelper lock(cActiveLock);

  if (cached) {
    time_t now = time(NULL);

    if (now - cActiveTime) {
      cActiveTime = now;
    } else {
      return cActive;
    }
  }

  std::string active = GetString("stat.active");

  if (active == "online") {
    cActive = ActiveStatus::kOnline;
    return ActiveStatus::kOnline;
  } else if (active == "offline") {
    cActive = ActiveStatus::kOffline;
    return ActiveStatus::kOffline;
  } else {
    cActive = ActiveStatus::kUndefined;
    return ActiveStatus::kUndefined;
  }
}

EOSCOMMONNAMESPACE_END;
