// ----------------------------------------------------------------------
// File: XrdMgmOfs.cc
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

#include "common/Constants.hh"
#include "common/Mapping.hh"
#include "common/FileId.hh"
#include "common/LayoutId.hh"
#include "common/Path.hh"
#include "common/SecEntity.hh"
#include "common/StackTrace.hh"
#include "common/SymKeys.hh"
#include "common/http/OwnCloud.hh"
#include "common/plugin_manager/Plugin.hh"
#include "common/ZMQ.hh"
#include "common/JeMallocHandler.hh"
#include "namespace/Constants.hh"
#include "namespace/interface/ContainerIterators.hh"
#include "authz/XrdCapability.hh"
#include "mgm/Stat.hh"
#include "mgm/Access.hh"
#include "mgm/FileSystem.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/XrdMgmOfsDirectory.hh"
#include "mgm/XrdMgmOfsFile.hh"
#include "mgm/XrdMgmOfsTrace.hh"
#include "mgm/XrdMgmOfsSecurity.hh"
#include "mgm/Policy.hh"
#include "mgm/Quota.hh"
#include "mgm/Acl.hh"
#include "mgm/Workflow.hh"
#include "mgm/proc/ProcInterface.hh"
#include "mgm/txengine/TransferEngine.hh"
#include "mgm/Recycle.hh"
#include "mgm/Macros.hh"
#include "mgm/GeoTreeEngine.hh"
#include "mgm/VstMessaging.hh"
#include "mgm/Egroup.hh"
#include "mgm/http/HttpServer.hh"
#include "mgm/ZMQ.hh"
#include "mgm/Iostat.hh"
#include "mgm/LRU.hh"
#include "mgm/WFE.hh"
#include "mgm/Fsck.hh"
#include "mgm/Master.hh"
#include "namespace/interface/IFsView.hh"
#include "namespace/Prefetcher.hh"
#include "namespace/utils/Stat.hh"
#include <XrdVersion.hh>
#include <XrdOss/XrdOss.hh>
#include <XrdOuc/XrdOucBuffer.hh>
#include <XrdOuc/XrdOucEnv.hh>
#include <XrdOuc/XrdOucTokenizer.hh>
#include <XrdOuc/XrdOucTrace.hh>
#include <XrdSys/XrdSysError.hh>
#include <XrdSys/XrdSysLogger.hh>
#include <XrdSys/XrdSysPthread.hh>
#include <XrdSys/XrdSysTimer.hh>
#include <XrdSec/XrdSecInterface.hh>
#include <XrdSfs/XrdSfsAio.hh>
#include <XrdSfs/XrdSfsFlags.hh>
#include "google/protobuf/io/zero_copy_stream_impl.h"

#ifdef __APPLE__
#define ECOMM 70
#endif

#ifndef S_IAMB
#define S_IAMB  0x1FF
#endif

/*----------------------------------------------------------------------------*/
XrdSysError gMgmOfsEroute(0);
XrdSysError* XrdMgmOfs::eDest;
XrdOucTrace gMgmOfsTrace(&gMgmOfsEroute);
const char* XrdMgmOfs::gNameSpaceState[] = {"down", "booting", "booted", "failed", "compacting"};
XrdMgmOfs* gOFS = 0;

// Set the version information
XrdVERSIONINFO(XrdSfsGetFileSystem, MgmOfs);

//------------------------------------------------------------------------------
//! Filesystem Plugin factory function
//!
//! @param native_fs (not used)
//! @param lp the logger object
//! @param configfn the configuration file name
//!
//! @returns configures and returns our MgmOfs object
//------------------------------------------------------------------------------
extern "C"
XrdSfsFileSystem*
XrdSfsGetFileSystem(XrdSfsFileSystem* native_fs,
                    XrdSysLogger* lp,
                    const char* configfn)
{
  gMgmOfsEroute.SetPrefix("MgmOfs_");
  gMgmOfsEroute.logger(lp);
  static XrdMgmOfs myFS(&gMgmOfsEroute);
  XrdOucString vs = "MgmOfs (meta data redirector) ";
  vs += VERSION;
  gMgmOfsEroute.Say("++++++ (c) 2015 CERN/IT-DSS ", vs.c_str());

  // Initialize the subsystems
  if (!myFS.Init(gMgmOfsEroute)) {
    return nullptr;
  }

  // Disable XRootd log rotation
  lp->setRotate(0);
  gOFS = &myFS;
  // By default enable stalling and redirection
  gOFS->IsStall = true;
  gOFS->IsRedirect = true;
  myFS.ConfigFN = (configfn && *configfn ? strdup(configfn) : nullptr);

  if (myFS.Configure(gMgmOfsEroute)) {
    return nullptr;
  }

  // Initialize authorization module ServerAcc
  gOFS->CapabilityEngine = (XrdCapability*) XrdAccAuthorizeObject(lp, configfn,
                           nullptr);

  if (!gOFS->CapabilityEngine) {
    return nullptr;
  }

  return gOFS;
}


/******************************************************************************/
/* MGM Meta Data Interface                                                    */
/******************************************************************************/

//------------------------------------------------------------------------------
// Constructor MGM Ofs
//------------------------------------------------------------------------------
XrdMgmOfs::XrdMgmOfs(XrdSysError* ep):
  ConfigFN(0), ConfEngine(0), CapabilityEngine(0), mCapabilityValidity(3600),
  MgmOfsMessaging(0), MgmOfsVstMessaging(0),  ManagerPort(1094),
  MgmOfsConfigEngineRedisPort(0), LinuxStatsStartup{0},
  StartTime(0), HostName(0), HostPref(0), Initialized(kDown),
  InitializationTime(0), Shutdown(false), RemoveStallRuleAfterBoot(false),
  BootFileId(0), BootContainerId(0), IsRedirect(true), IsStall(true),
  authorize(false), IssueCapability(false), MgmRedirector(false),
  ErrorLog(true), eosDirectoryService(0), eosFileService(0), eosView(0),
  eosFsView(0), eosContainerAccounting(0), eosSyncTimeAccounting(0),
  deletion_tid(0), stats_tid(0), fsconfiglistener_tid(0), auth_tid(0),
  mFrontendPort(0), mNumAuthThreads(0), zMQ(nullptr), Authorization(0),
  MgmStatsPtr(new eos::mgm::Stat()), MgmStats(*MgmStatsPtr),
  mCommentLog(nullptr),
  FsckPtr(new eos::mgm::Fsck()), FsCheck(*FsckPtr),
  MasterPtr(new eos::mgm::Master()), MgmMaster(*MasterPtr),
  LRUPtr(new eos::mgm::LRU()), LRUd(*LRUPtr),
  WFEPtr(new eos::mgm::WFE()), WFEd(*WFEPtr),
  UTF8(false), mFstGwHost(""), mFstGwPort(0), mQdbCluster(""),mHttpdPort(8000),
  mSubmitterTid(0),
  mJeMallocHandler(new eos::common::JeMallocHandler())
{
  eDest = ep;
  ConfigFN = 0;

  if (getenv("EOS_MGM_HTTP_PORT")) {
    mHttpdPort = strtol(getenv("EOS_MGM_HTTP_PORT"), 0, 10);
  }

  eos::common::LogId::SetSingleShotLogId();
  mZmqContext = new zmq::context_t(1);
  IoStats.reset(new eos::mgm::Iostat());
  Httpd.reset(new eos::mgm::HttpServer(mHttpdPort));
  EgroupRefresh.reset(new eos::mgm::Egroup());
  Recycler.reset(new eos::mgm::Recycle());
}

//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
XrdMgmOfs::~XrdMgmOfs()
{
  StopArchiveSubmitter();
  delete mZmqContext;
}

//------------------------------------------------------------------------------
// This is just kept to be compatible with standard OFS plugins, but it is not
// used for the moment.
//------------------------------------------------------------------------------
bool
XrdMgmOfs::Init(XrdSysError& ep)
{
  return true;
}

//------------------------------------------------------------------------------
// Return a MGM directory object
//------------------------------------------------------------------------------
XrdSfsDirectory*
XrdMgmOfs::newDir(char* user, int MonID)
{
  return (XrdSfsDirectory*)new XrdMgmOfsDirectory(user, MonID);
}

//------------------------------------------------------------------------------
// Return MGM file object
//------------------------------------------------------------------------------
XrdSfsFile*
XrdMgmOfs::newFile(char* user, int MonID)
{
  return (XrdSfsFile*)new XrdMgmOfsFile(user, MonID);
}

//------------------------------------------------------------------------------
// Notify filesystem that a client has disconnected
//------------------------------------------------------------------------------
void
XrdMgmOfs::Disc(const XrdSecEntity* client)
{
  if (client) {
    ProcInterface::DropSubmittedCmd(client->tident);
  }
}

//------------------------------------------------------------------------------
// Implementation Source Code Includes
//------------------------------------------------------------------------------
#include "XrdMgmOfs/Access.cc"
#include "XrdMgmOfs/Attr.cc"
#include "XrdMgmOfs/Auth.cc"
#include "XrdMgmOfs/Chksum.cc"
#include "XrdMgmOfs/Chmod.cc"
#include "XrdMgmOfs/Chown.cc"
#include "XrdMgmOfs/DeleteExternal.cc"
#include "XrdMgmOfs/Exists.cc"
#include "XrdMgmOfs/Find.cc"
#include "XrdMgmOfs/FsConfigListener.cc"
#include "XrdMgmOfs/Fsctl.cc"
#include "XrdMgmOfs/Link.cc"
#include "XrdMgmOfs/Merge.cc"
#include "XrdMgmOfs/Mkdir.cc"
#include "XrdMgmOfs/PathMap.cc"
#include "XrdMgmOfs/PathRoute.cc"
#include "XrdMgmOfs/Remdir.cc"
#include "XrdMgmOfs/Rename.cc"
#include "XrdMgmOfs/Rm.cc"
#include "XrdMgmOfs/SendResync.cc"
#include "XrdMgmOfs/SharedPath.cc"
#include "XrdMgmOfs/ShouldRedirect.cc"
#include "XrdMgmOfs/ShouldRoute.cc"
#include "XrdMgmOfs/ShouldStall.cc"
#include "XrdMgmOfs/Shutdown.cc"
#include "XrdMgmOfs/Stacktrace.cc"
#include "XrdMgmOfs/Stat.cc"
#include "XrdMgmOfs/Stripes.cc"
#include "XrdMgmOfs/Touch.cc"
#include "XrdMgmOfs/Utimes.cc"
#include "XrdMgmOfs/Version.cc"

//------------------------------------------------------------------------------
// Test for stall rule
//------------------------------------------------------------------------------
bool
XrdMgmOfs::HasStall(const char* path,
                    const char* rule,
                    int& stalltime,
                    XrdOucString& stallmsg)
{
  if (!rule) {
    return false;
  }

  eos::common::RWMutexReadLock lock(Access::gAccessMutex);

  if (Access::gStallRules.count(std::string(rule))) {
    stalltime = atoi(Access::gStallRules[std::string(rule)].c_str());
    stallmsg =
      "Attention: you are currently hold in this instance and each request is stalled for ";
    stallmsg += (int) stalltime;
    stallmsg += " seconds after an errno of type: ";
    stallmsg += rule;
    eos_static_info("info=\"stalling\" path=\"%s\" errno=\"%s\"", path, rule);
    return true;
  } else {
    return false;
  }
}

//------------------------------------------------------------------------------
// Test for redirection rule
//------------------------------------------------------------------------------
bool
XrdMgmOfs::HasRedirect(const char* path,
                       const char* rule,
                       XrdOucString& host,
                       int& port)
{
  if (!rule) {
    return false;
  }

  std::string srule = rule;
  eos::common::RWMutexReadLock lock(Access::gAccessMutex);

  if (Access::gRedirectionRules.count(srule)) {
    std::string delimiter = ":";
    std::vector<std::string> tokens;
    eos::common::StringConversion::Tokenize(Access::gRedirectionRules[srule],
                                            tokens, delimiter);

    if (tokens.size() == 1) {
      host = tokens[0].c_str();
      port = 1094;
    } else {
      host = tokens[0].c_str();
      port = atoi(tokens[1].c_str());

      if (port == 0) {
        port = 1094;
      }
    }

    eos_static_info("info=\"redirect\" path=\"%s\" host=%s port=%d errno=%s",
                    path, host.c_str(), port, rule);

    if (srule == "ENONET") {
      gOFS->MgmStats.Add("RedirectENONET", 0, 0, 1);
    } else if (srule == "ENOENT") {
      gOFS->MgmStats.Add("RedirectENOENT", 0, 0, 1);
    } else if (srule == "ENETUNREACH") {
      gOFS->MgmStats.Add("RedirectENETUNREACH", 0, 0, 1);
    }

    return true;
  } else {
    return false;
  }
}

//------------------------------------------------------------------------------
// Return the version of the MGM software
//------------------------------------------------------------------------------
const char*
XrdMgmOfs::getVersion()
{
  static XrdOucString FullVersion = XrdVERSION;
  FullVersion += " MgmOfs ";
  FullVersion += VERSION;
  return FullVersion.c_str();
}


//------------------------------------------------------------------------------
// Prepare a file (EOS will call a prepare workflow if defined)
//------------------------------------------------------------------------------
int
XrdMgmOfs::prepare(XrdSfsPrep& pargs, XrdOucErrInfo& error,
                   const XrdSecEntity* client)
{
  static const char* epname = "prepare";
  const char* tident = error.getErrUser();
  eos::common::Mapping::VirtualIdentity vid;
  EXEC_TIMING_BEGIN("IdMap");
  XrdOucTList* pptr = pargs.paths;
  XrdOucTList* optr = pargs.oinfo;
  std::string info;
  info = (optr ? (optr->text ? optr->text : "") : "");
  eos::common::Mapping::IdMap(client, info.c_str(), tident, vid);
  EXEC_TIMING_END("IdMap");
  gOFS->MgmStats.Add("IdMap", vid.uid, vid.gid, 1);
  ACCESSMODE_W;
  MAYSTALL;
  {
    const char* path="/";
    const char* ininfo="";
    MAYREDIRECT;
  }
  std::string cmd = "mgm.pcmd=event";
  int retc = SFS_OK;
  std::list<std::pair<char**, char**>> pathsWithPrepare;
  std::string event = pargs.opts & Prep_FRESH ? "sync::abort_prepare" :
                      "sync::prepare";

  // check that all files exist
  while (pptr) {
    XrdOucString prep_path = (pptr->text ? pptr->text : "");
    eos_info("msg =\"checking file exists\" path=\"%s\"", prep_path.c_str());
    XrdSfsFileExistence check;

    {
      const char* inpath = prep_path.c_str();
      const char* ininfo = "";
      NAMESPACEMAP;
      prep_path = path;
    }

    {
      const char* path = prep_path.c_str();
      const char* ininfo = "";
      MAYREDIRECT;
    }

    if (_exists(prep_path.c_str(), check, error, client, "") ||
        (check != XrdSfsFileExistIsFile)) {
      if (check != XrdSfsFileExistIsFile) {
        Emsg(epname, error, ENOENT,
             "prepare - file does not exist or is not accessible to you",
             prep_path.c_str());
      }

      return SFS_ERROR;
    }

    eos::IContainerMD::XAttrMap attributes;

    if (_attr_ls(eos::common::Path(prep_path.c_str()).GetParentPath(), error, vid,
                 nullptr, attributes) == 0) {
      bool foundPrepareTag = false;
      std::string eventAttr = "sys.workflow." + event;

      for (const auto& attrEntry : attributes) {
        foundPrepareTag |= attrEntry.first.find(eventAttr) == 0;
      }

      if (foundPrepareTag) {
        pathsWithPrepare.emplace_back(&(pptr->text),
                                      optr != nullptr ? & (optr->text) : nullptr);
      } else {
        // don't do workflow if no such tag
        pptr = pptr->next;

        if (optr) {
          optr = optr->next;
        }

        continue;
      }
    } else {
      // don't do workflow if we can't check attributes
      pptr = pptr->next;

      if (optr) {
        optr = optr->next;
      }

      continue;
    }

    // check that we have write permission on path
    if (gOFS->_access(prep_path.c_str(), W_OK | P_OK, error, vid, "")) {
      return Emsg(epname, error, EPERM,
                  "prepare - you don't have write and workflow permission",
                  prep_path.c_str());
    }

    pptr = pptr->next;

    if (optr) {
      optr = optr->next;
    }
  }

  for (auto& pathPair : pathsWithPrepare) {
    XrdOucString prep_path = (*pathPair.first ? *pathPair.first : "");
    eos_info("msg=\"about to trigger WFE\" path=\"%s\"", prep_path.c_str());
    XrdOucString prep_info = pathPair.second != nullptr ? (*pathPair.second ?
                             *pathPair.second : "") : "";
    XrdOucEnv prep_env(prep_info.c_str());
    prep_info = cmd.c_str();
    prep_info += "&mgm.event=";
    prep_info += event.c_str();
    prep_info += "&mgm.workflow=";

    if (prep_env.Get("eos.workflow")) {
      prep_info += prep_env.Get("eos.workflow");
    } else {
      prep_info += "default";
    }

    prep_info += "&mgm.fid=0&mgm.path=";
    prep_info += prep_path.c_str();
    prep_info += "&mgm.logid=";
    prep_info += this->logId;
    prep_info += "&mgm.ruid=";
    prep_info += (int)vid.uid;
    prep_info += "&mgm.rgid=";
    prep_info += (int)vid.gid;
    XrdSecEntity lClient(vid.prot.c_str());
    lClient.name = (char*) vid.name.c_str();
    lClient.tident = (char*) vid.tident.c_str();
    lClient.host = (char*) vid.host.c_str();
    XrdOucString lSec = "&mgm.sec=";
    lSec += eos::common::SecEntity::ToKey(&lClient,
                                          "eos").c_str();
    prep_info += lSec;
    XrdSfsFSctl args;
    args.Arg1 = prep_path.c_str();
    args.Arg1Len = prep_path.length();
    args.Arg2 = prep_info.c_str();
    args.Arg2Len = prep_info.length();

    auto ret_wfe = XrdMgmOfs::FSctl(SFS_FSCTL_PLUGIN, args,
                                    error, &lClient);

    if (ret_wfe != SFS_DATA) {
      retc = Emsg(epname, error, ret_wfe,
                  "prepare - synchronous prepare workflow error", prep_path.c_str());
    }
  }

  return retc;
}

//------------------------------------------------------------------------------
//! Truncate a file (not supported in EOS, only via the file interface)
//------------------------------------------------------------------------------
int
XrdMgmOfs::truncate(const char*,
                    XrdSfsFileOffset,
                    XrdOucErrInfo& error,
                    const XrdSecEntity* client,
                    const char* path)
{
  static const char* epname = "truncate";
  const char* tident = error.getErrUser();
  // use a thread private vid
  eos::common::Mapping::VirtualIdentity vid;
  EXEC_TIMING_BEGIN("IdMap");
  eos::common::Mapping::IdMap(client, 0, tident, vid);
  EXEC_TIMING_END("IdMap");
  gOFS->MgmStats.Add("IdMap", vid.uid, vid.gid, 1);
  ACCESSMODE_W;
  MAYSTALL;
  {
    const char* ininfo = "";
    MAYREDIRECT;
  }
  gOFS->MgmStats.Add("Truncate", vid.uid, vid.gid, 1);
  return Emsg(epname, error, EOPNOTSUPP, "truncate", path);
}

//------------------------------------------------------------------------------
// Return error message
//------------------------------------------------------------------------------
int
XrdMgmOfs::Emsg(const char* pfx,
                XrdOucErrInfo& einfo,
                int ecode,
                const char* op,
                const char* target)
{
  char* etext, buffer[4096], unkbuff[64];

  // Get the reason for the error
  if (ecode < 0) {
    ecode = -ecode;
  }

  if (!(etext = strerror(ecode))) {
    sprintf(unkbuff, "reason unknown (%d)", ecode);
    etext = unkbuff;
  }

  // Format the error message
  snprintf(buffer, sizeof(buffer), "Unable to %s %s", op, target);

  if ((ecode == EIDRM) || (ecode == ENODATA)) {
    eos_debug("Unable to %s %s; %s", op, target, etext);
  } else {
    if ((!strcmp(op, "get-if-clock")) || (!strcmp(op, "stat")) ||
        ((!strcmp(pfx, "attr_get") || (!strcmp(pfx, "attr_ls"))) &&
         (ecode == ENOENT))) {
      eos_debug("Unable to %s %s; %s", op, target, etext);
    } else {
      eos_err("Unable to %s %s; %s", op, target, etext);
    }
  }

  // Print it out if debugging is enabled
#ifndef NODEBUG
  //   XrdMgmOfs::eDest->Emsg(pfx, buffer);
#endif
  // Place the error message in the error object and return
  einfo.setErrInfo(ecode, buffer);
  return SFS_ERROR;
}

//------------------------------------------------------------------------------
// Create stall response
//------------------------------------------------------------------------------
int
XrdMgmOfs::Stall(XrdOucErrInfo& error,
                 int stime,
                 const char* msg)

{
  XrdOucString smessage = msg;
  smessage += "; come back in ";
  smessage += stime;
  smessage += " seconds!";
  EPNAME("Stall");
  const char* tident = error.getErrUser();
  ZTRACE(delay, "Stall " << stime << ": " << smessage.c_str());
  // Place the error message in the error object and return
  error.setErrInfo(0, smessage.c_str());
  return stime;
}

//------------------------------------------------------------------------------
// Create redirect response
//------------------------------------------------------------------------------
int
XrdMgmOfs::Redirect(XrdOucErrInfo& error,
                    const char* host,
                    int& port)
{
  EPNAME("Redirect");
  const char* tident = error.getErrUser();
  ZTRACE(delay, "Redirect " << host << ":" << port);
  // Place the error message in the error object and return
  error.setErrInfo(port, host);
  return SFS_REDIRECT;
}

//------------------------------------------------------------------------------
// Statistics circular buffer thread startup function
//------------------------------------------------------------------------------
void*
XrdMgmOfs::StartMgmStats(void* pp)
{
  XrdMgmOfs* ofs = (XrdMgmOfs*) pp;
  ofs->MgmStats.Circulate();
  return 0;
}

//------------------------------------------------------------------------------
// Filesystem error/config listener thread startup function
//------------------------------------------------------------------------------
void*
XrdMgmOfs::StartMgmFsConfigListener(void* pp)
{
  XrdMgmOfs* ofs = (XrdMgmOfs*) pp;
  ofs->FsConfigListener();
  return 0;
}

//------------------------------------------------------------------------------
// Static method to start a thread that will queue, build and submit backup
// operations to the archiver daemon.
//------------------------------------------------------------------------------
void*
XrdMgmOfs::StartArchiveSubmitter(void* arg)
{
  return reinterpret_cast<XrdMgmOfs*>(arg)->ArchiveSubmitter();
}

//------------------------------------------------------------------------------
// Method to stop the submitter thread
//------------------------------------------------------------------------------
void
XrdMgmOfs::StopArchiveSubmitter()
{
  XrdSysThread::Cancel(mSubmitterTid);
  XrdSysThread::Join(mSubmitterTid, NULL);
}

//------------------------------------------------------------------------------
// Implementation of the archive/backup sumitter thread
//------------------------------------------------------------------------------
void*
XrdMgmOfs::ArchiveSubmitter()
{
  ProcCommand pcmd;
  XrdSysTimer timer;
  std::string job_opaque;
  XrdOucString std_out, std_err;
  int max, running, pending;
  eos::common::Mapping::VirtualIdentity root_vid;
  eos::common::Mapping::Root(root_vid);
  eos_debug("msg=\"starting archive/backup submitter thread\"");
  std::ostringstream cmd_json;
  cmd_json << "{\"cmd\": \"stats\", "
           << "\"opt\": \"\", "
           << "\"uid\": \"0\", "
           << "\"gid\": \"0\" }";

  while (1) {
    XrdSysThread::SetCancelOff();
    {
      XrdSysMutexHelper lock(mJobsQMutex);

      if (!mPendingBkps.empty()) {
        // Check if archiver has slots available
        if (!pcmd.ArchiveExecuteCmd(cmd_json.str())) {
          std_out.resize(0);
          std_err.resize(0);
          pcmd.AddOutput(std_out, std_err);

          if ((sscanf(std_out.c_str(), "max=%i running=%i pending=%i",
                      &max, &running, &pending) == 3)) {
            while ((running + pending < max) && !mPendingBkps.empty()) {
              running++;
              job_opaque = mPendingBkps.back();
              mPendingBkps.pop_back();
              job_opaque += "&mgm.backup.create=1";

              if (pcmd.open("/proc/admin", job_opaque.c_str(), root_vid, 0)) {
                pcmd.AddOutput(std_out, std_err);
                eos_err("failed backup, msg=\"%s\"", std_err.c_str());
              }
            }
          }
        } else {
          eos_err("failed to send stats command to archive daemon");
        }
      }
    }
    XrdSysThread::SetCancelOn();
    timer.Wait(5000);
  }

  return 0;
}

//------------------------------------------------------------------------------
// Submit backup job
//------------------------------------------------------------------------------
bool
XrdMgmOfs::SubmitBackupJob(const std::string& job_opaque)
{
  XrdSysMutexHelper lock(mJobsQMutex);
  auto it = std::find(mPendingBkps.begin(), mPendingBkps.end(), job_opaque);

  if (it == mPendingBkps.end()) {
    mPendingBkps.push_front(job_opaque);
    return true;
  }

  return false;
}

//------------------------------------------------------------------------------
// Get vector of pending backups
//------------------------------------------------------------------------------
std::vector<ProcCommand::ArchDirStatus>
XrdMgmOfs::GetPendingBkps()
{
  std::vector<ProcCommand::ArchDirStatus> bkps;
  XrdSysMutexHelper lock(mJobsQMutex);

  for (auto it = mPendingBkps.begin(); it != mPendingBkps.end(); ++it) {
    XrdOucEnv opaque(it->c_str());
    bkps.emplace_back("N/A", "N/A", opaque.Get("mgm.backup.dst"), "backup",
                      "pending at MGM");
  }

  return bkps;
}

//------------------------------------------------------------------------------
// Discover/search for a service provided to the plugins by the platform
//------------------------------------------------------------------------------
int32_t
XrdMgmOfs::DiscoverPlatformServices(const char* svc_name, void* opaque)
{
  std::string sname = svc_name;

  if (sname == "NsViewMutex") {
    PF_Discovery_Service* ns_lock = (PF_Discovery_Service*)(opaque);
    // TODO (esindril): Use this code when we drop SLC6 support
    //std::string htype = std::to_string(typeid(&gOFS->eosViewRWMutex).hash_code());
    std::string htype = "eos::common::RWMutex*";
    ns_lock->objType = (char*)calloc(htype.length() + 1, sizeof(char));
    (void) strcpy(ns_lock->objType, htype.c_str());
    ns_lock->ptrService = static_cast<void*>(&gOFS->eosViewRWMutex);
  } else {
    return EINVAL;
  }

  return 0;
}

//------------------------------------------------------------------------------
// Cast a change message to all fusex clients
//------------------------------------------------------------------------------
void
XrdMgmOfs::FuseXCast(uint64_t inode)
{
  gOFS->zMQ->gFuseServer.Cap().BroadcastReleaseFromExternal(inode);
}

//----------------------------------------------------------------------------
// Check if name space is booted
//----------------------------------------------------------------------------
bool
XrdMgmOfs::IsNsBooted() const
{
  XrdSysMutexHelper lock(InitializationMutex);
  return ((Initialized == kBooted) || (Initialized == kCompacting));
}

std::string
XrdMgmOfs::MacroStringError(int errcode) {
  if (errcode == ENOTCONN) {
    return "ENOTCONN";
  }
  else if (errcode == EPROTO) {
    return "EPROTO";
  }
  else if (errcode == EAGAIN) {
    return "EAGAIN";
  }
  else {
    return "EINVAL";
  }
}

//----------------------------------------------------------------------------
// write report record for final deletion
//----------------------------------------------------------------------------
void
XrdMgmOfs::WriteRmRecord(eos::IFileMD& fmd)
{
  {
    // write a deletion report to IoStat
                                                                                                      
    char report[16384];
    eos::IFileMD::ctime_t ctime;
    eos::IFileMD::ctime_t mtime;
    fmd.getCTime(ctime);
    fmd.getMTime(mtime);
    snprintf(report, sizeof(report) - 1,
                   "log=%s&"
                   "host=%s&fid=%llu&"
	           "ruid=%u&rgid=%u"
                   "dc_ts=%lu&dc_tns=%lu&"
                   "dm_ts=%lu&dm_tns=%lu&"
                   "dsize=%lu&sec.app=rm"
	     , this->logId
	     , gOFS->ManagerId.c_str()
	     , fmd.getId()
	     , fmd.getCUid()
	     , fmd.getCGid()
	     , ctime.tv_sec, ctime.tv_nsec
	     , mtime.tv_sec, mtime.tv_nsec
	     , fmd.getSize());

    std::string record = report;
    gOFS->IoStats->WriteRecord(record);
  }
}

//----------------------------------------------------------------------------
// write report record for recycle bin deletion
//----------------------------------------------------------------------------
void
XrdMgmOfs::WriteRecycleRecord(eos::IFileMD& fmd)
{
  {
    // write a deletion report to IoStat
                                                                                                       
    char report[16384];
    eos::IFileMD::ctime_t ctime;
    eos::IFileMD::ctime_t mtime;
    fmd.getCTime(ctime);
    fmd.getMTime(mtime);
    snprintf(report, sizeof(report) - 1,
                   "log=%s&"
                   "host=%s&fid=%llu&"
	            "ruid=%u&rgid=%u"
                   "dc_ts=%lu&dc_tns=%lu&"
                   "dm_ts=%lu&dm_tns=%lu&"
                   "dsize=%lu&sec.app=recycle"
	     , this->logId
	     , gOFS->ManagerId.c_str()
	     , fmd.getId()
	     , fmd.getCUid()
	     , fmd.getCGid()
	     , ctime.tv_sec, ctime.tv_nsec
	     , mtime.tv_sec, mtime.tv_nsec
	     , fmd.getSize());

    std::string record = report;
    gOFS->IoStats->WriteRecord(record);
  }
}
