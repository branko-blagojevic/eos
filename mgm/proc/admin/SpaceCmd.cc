//------------------------------------------------------------------------------
// File: SpaceCmd.cc
// Author: Fabio Luchetti - CERN
//------------------------------------------------------------------------------

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

#include "SpaceCmd.hh"
#include "mgm/proc/ProcInterface.hh"
#include "mgm/XrdMgmOfs.hh"

#include "common/Path.hh"
#include "mgm/tracker/ReplicationTracker.hh"
#include "mgm/inspector/FileInspector.hh"
#include "mgm/Egroup.hh"
#include "namespace/interface/IChLogFileMDSvc.hh"
#include "namespace/interface/IChLogContainerMDSvc.hh"
#include "namespace/interface/IFsView.hh"
#include "namespace/interface/IContainerMDSvc.hh"
#include "namespace/interface/IView.hh"

#include "common/StringTokenizer.hh"

EOSMGMNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Method implementing the specific behavior of the command executed by the
// asynchronous thread
//------------------------------------------------------------------------------
eos::console::ReplyProto
SpaceCmd::ProcessRequest() noexcept
{
  eos::console::ReplyProto reply;
  eos::console::SpaceProto space = mReqProto.space();
  eos::console::SpaceProto::SubcmdCase subcmd = space.subcmd_case();

  if (subcmd == eos::console::SpaceProto::kLs) {
    LsSubcmd(space.ls(), reply);
  } else if (subcmd == eos::console::SpaceProto::kSet) {
    SetSubcmd(space.set(), reply);
  } else if (subcmd == eos::console::SpaceProto::kStatus) {
    StatusSubcmd(space.status(), reply);
  } else if (subcmd == eos::console::SpaceProto::kNodeSet) {
    NodeSetSubcmd(space.nodeset(), reply);
  } else if (subcmd == eos::console::SpaceProto::kNodeGet) {
    NodeGetSubcmd(space.nodeget(), reply);
  } else if (subcmd == eos::console::SpaceProto::kReset) {
    ResetSubcmd(space.reset(), reply);
  } else if (subcmd == eos::console::SpaceProto::kDefine) {
    DefineSubcmd(space.define(), reply);
  } else if (subcmd == eos::console::SpaceProto::kConfig) {
    ConfigSubcmd(space.config(), reply);
  } else if (subcmd == eos::console::SpaceProto::kQuota) {
    QuotaSubcmd(space.quota(), reply);
  } else if (subcmd == eos::console::SpaceProto::kRm) {
    RmSubcmd(space.rm(), reply);
  } else if (subcmd == eos::console::SpaceProto::kTracker) {
    TrackerSubcmd(space.tracker(), reply);
  } else if (subcmd == eos::console::SpaceProto::kInspector) {
    InspectorSubcmd(space.inspector(), reply);
  }else {
    reply.set_std_err("error: not supported");
    reply.set_retc(EINVAL);
  }

  return reply;

}

//----------------------------------------------------------------------------
// Execute ls subcommand
//----------------------------------------------------------------------------
void SpaceCmd::LsSubcmd(const eos::console::SpaceProto_LsProto& ls, eos::console::ReplyProto& reply) {

      std::string format;
      std::string list_format;

      switch (ls.outformat()) {
        case eos::console::SpaceProto_LsProto::LISTING:
          format = FsView::GetSpaceFormat("l");
          list_format = FsView::GetFileSystemFormat("l");
          break;

        case eos::console::SpaceProto_LsProto::MONITORING:
          format = FsView::GetSpaceFormat("m");
          break;

        case eos::console::SpaceProto_LsProto::IO:
          format = FsView::GetSpaceFormat("io");
          break;

        case eos::console::SpaceProto_LsProto::FSCK:
          format = FsView::GetSpaceFormat("fsck");
          break;

        default : // NONE
          format = FsView::GetSpaceFormat("");
          break;
      }

      std::string std_out;
      eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
      FsView::gFsView.PrintSpaces(std_out, format, list_format, ls.outdepth(), ls.selection().c_str(), "", mReqProto.dontcolor());

      reply.set_std_out(std_out);

}

//----------------------------------------------------------------------------
// Execute status subcommand
//----------------------------------------------------------------------------
void SpaceCmd::StatusSubcmd(const eos::console::SpaceProto_StatusProto& status, eos::console::ReplyProto& reply) {

  const char* fmtstr = status.outformat_m() ? "%s=%s " : "%-32s := %s\n";
  eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);

  if (!FsView::gFsView.mSpaceView.count(status.mgmspace())) {
    reply.set_std_err("error: cannot find space - no space with name=" + status.mgmspace());
    reply.set_retc(ENOENT);
    return;
  }

  std::string std_out;

  if (!status.outformat_m()) {
    std_out += "# ------------------------------------------------------------------------------------\n";
    std_out += "# Space Variables\n";
    std_out += "# ....................................................................................\n";
  }

  std::vector <std::string> keylist;
  FsView::gFsView.mSpaceView[status.mgmspace()]->GetConfigKeys(keylist);
  std::sort(keylist.begin(), keylist.end());

  for (auto & i : keylist) {
    char line[32678];

    if (((i == "nominalsize") || (i == "headroom")) && !status.outformat_m()) {
      XrdOucString sizestring;
      // size printout
      snprintf(line, sizeof(line) - 1, fmtstr, i.c_str(),
               eos::common::StringConversion::GetReadableSizeString(sizestring,
                                                                    strtoull(
                                                                        FsView::gFsView.mSpaceView[status.mgmspace()]->GetConfigMember(
                                                                            i).c_str(), nullptr, 10), "B"));
    } else {
      snprintf(line, sizeof(line) - 1, fmtstr, i.c_str(),
               FsView::gFsView.mSpaceView[status.mgmspace()]->GetConfigMember(i).c_str());
    }

    std_out += line;
  }

  reply.set_std_out(std_out);

}

//----------------------------------------------------------------------------
// Execute set subcommand
//----------------------------------------------------------------------------
void SpaceCmd::SetSubcmd(const eos::console::SpaceProto_SetProto& set, eos::console::ReplyProto& reply) {

  std::string std_out, std_err;
  int ret_c = 0;

  if ( mVid.uid != 0 ) {
    reply.set_std_err("error: you have to take role 'root' to execute this command");
    reply.set_retc(EPERM);
    return;
  }

  if ( set.mgmspace().empty() ) {
    reply.set_std_err("error: illegal parameters");
    reply.set_retc(EINVAL);
    return;
  }

  eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);

  if (!FsView::gFsView.mSpaceView.count(set.mgmspace())) {
    reply.set_std_err("error: no such space - define one using 'space define' or add a filesystem under that space!");
    reply.set_retc(EINVAL);
    return;
  }

  std::string key = "status";
  std::string status = (set.state_switch()) ? "on" : "off";

  // loop over all groups
  std::map<std::string, FsGroup*>::const_iterator it1;
  for (it1 = FsView::gFsView.mGroupView.begin(); it1 != FsView::gFsView.mGroupView.end(); it1++) {
    if (!it1->second->SetConfigMember(key, status, true, "/eos/*/mgm")) {
      std_err += "error: cannot set status in group <" + it1->first + ">\n";
      ret_c = EIO;
    }
  }
  // loop over all nodes
  std::map<std::string, FsNode *>::const_iterator it2;
  for (it2 = FsView::gFsView.mNodeView.begin();
       it2 != FsView::gFsView.mNodeView.end(); it2++) {
    if (!it2->second->SetConfigMember(key, status, true, "/eos/*/mgm")) {
      std_err += "error: cannot set status for node <" + it2->first + ">\n";
      ret_c = EIO;
    }
  }

  reply.set_std_out(std_out);
  reply.set_std_err(std_err);
  reply.set_retc(ret_c);

}

//----------------------------------------------------------------------------
// Execute node-set subcommand
//----------------------------------------------------------------------------
void SpaceCmd::NodeSetSubcmd(const eos::console::SpaceProto_NodeSetProto& nodeset, eos::console::ReplyProto& reply) {

  if ( mVid.uid != 0 ) {
    reply.set_std_err("error: you have to take role 'root' to execute this command");
    reply.set_retc(EPERM);
    return;
  }

  if ( nodeset.mgmspace().empty() || nodeset.nodeset_key().empty() || nodeset.nodeset_value().empty() ) {
    reply.set_std_err("error: illegal parameters");
    reply.set_retc(EINVAL);
    return;
  }


  eos::common::RWMutexWriteLock lock(FsView::gFsView.ViewMutex);

  if (!FsView::gFsView.mSpaceView.count(nodeset.mgmspace())) {
    reply.set_std_err("error: no such space - define one using 'space define' or add a filesystem under that space!");
    reply.set_retc(EINVAL);
    return;
  }

  std::string std_out, std_err;
  int ret_c = 0;

  std::string val = nodeset.nodeset_value();

  {
    // loop over all nodes
    std::map<std::string, FsNode*>::const_iterator it;
    for (it = FsView::gFsView.mNodeView.begin(); it != FsView::gFsView.mNodeView.end(); it++) {
      XrdOucString file = val.c_str();

      if (file.beginswith("file:/")) {
        // load the file on the MGM
        file.erase(0, 5);
        eos::common::Path iPath(file.c_str());
        XrdOucString fpath = iPath.GetPath();

        if (!fpath.beginswith("/var/eos/")) {
          std_err = ("error: cannot load requested file=" + file + " - only files under /var/eos/ can bo loaded\n").c_str();
          ret_c = EINVAL;
        } else {
          std::ifstream ifs(file.c_str(), std::ios::in | std::ios::binary);

          if (!ifs) {
            std_err = ("error: cannot load requested file=" + file).c_str();
            ret_c = EINVAL;
          } else {
            val = std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            // store the value b64 encoded
            XrdOucString val64;
            eos::common::SymKey::Base64Encode((char*) val.c_str(), val.length(), val64);
            val = ("base64:" + val64).c_str();
            std_out += "success: loaded contents \n" + val;
          }
        }
      }
      if (!ret_c && !it->second->SetConfigMember(nodeset.nodeset_key(), val, true, "/eos/*/mgm")) {
        std_err += "error: cannot set node-set for node <" + it->first + ">\n";
        ret_c = EIO;
      }
    }
  }

  reply.set_std_out(std_out);
  reply.set_std_err(std_err);
  reply.set_retc(ret_c);

}

//----------------------------------------------------------------------------
// Execute node-get subcommand
//----------------------------------------------------------------------------
void SpaceCmd::NodeGetSubcmd(const eos::console::SpaceProto_NodeGetProto& nodeget, eos::console::ReplyProto& reply) {

  if ( mVid.uid != 0 ) {
    reply.set_std_err("error: you have to take role 'root' to execute this command");
    reply.set_retc(EPERM);
    return;
  }

  if ( nodeget.mgmspace().empty() || nodeget.nodeget_key().empty()) {
    reply.set_std_err("error: illegal parameters");
    reply.set_retc(EINVAL);
    return;
  }

  eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);

  if (!FsView::gFsView.mSpaceView.count(nodeget.mgmspace())) {
    reply.set_std_err("error: no such space - define one using 'space define' or add a filesystem under that space!");
    reply.set_retc(EINVAL);
    return;
  }

  std::string std_out;

  {
    std::string val;
    std::string new_val;
    bool identical = true;
    // loop over all nodes
    std::map<std::string, FsNode*>::const_iterator it;

    for (it = FsView::gFsView.mNodeView.begin(); it != FsView::gFsView.mNodeView.end(); it++) {
      new_val = it->second->GetConfigMember(nodeget.nodeget_key());

      if (val.length() && new_val != val) {
        identical = false;
      }
      val = new_val;
      std_out += "# [ " + (it->first).substr(0,it->first.find(':')) + " ]\n" + new_val + '\n';

    }
    if (identical) {
      std_out = "*:=" + val + '\n';
    }
  }

  reply.set_std_out(std_out);

}

//----------------------------------------------------------------------------
// Execute reset subcommand
//----------------------------------------------------------------------------
void SpaceCmd::ResetSubcmd(const eos::console::SpaceProto_ResetProto& reset, eos::console::ReplyProto& reply) {

  std::string std_out, std_err;
  int ret_c = 0;

  eos::common::RWMutexReadLock fsViewLock(FsView::gFsView.ViewMutex);

  if (reset.option() == eos::console::SpaceProto_ResetProto::DRAIN || reset.option() == eos::console::SpaceProto_ResetProto::NONE) {
    if (FsView::gFsView.mSpaceView.count(reset.mgmspace())) {
      FsView::gFsView.mSpaceView[reset.mgmspace()]->ResetDraining();
      std_out += "info: reset draining in space '" + reset.mgmspace() + "'";
    } else {
      std_err += "error: illegal space name";
      ret_c = EINVAL;
    }
  }

  if (reset.option() == eos::console::SpaceProto_ResetProto::EGROUP || reset.option() == eos::console::SpaceProto_ResetProto::NONE) {
    gOFS->EgroupRefresh->Reset();
    std_out += "\ninfo: clear cached EGroup information ...";
  }

  switch (reset.option()) {
    case eos::console::SpaceProto_ResetProto::DRAIN: {
      if (FsView::gFsView.mSpaceView.count(reset.mgmspace())) {
        FsView::gFsView.mSpaceView[reset.mgmspace()]->ResetDraining();
        std_out += "info: reset draining in space '" + reset.mgmspace() + "'";
      } else {
        std_err += "error: illegal space name";
        ret_c = EINVAL;
      }
    } break;

    case eos::console::SpaceProto_ResetProto::EGROUP: {
      gOFS->EgroupRefresh->Reset();
      std_out += "\ninfo: clear cached EGroup information ...";
    }  break;

    case eos::console::SpaceProto_ResetProto::NSFILESISTEMVIEW: {
      eos::common::RWMutexWriteLock lock(gOFS->eosViewRWMutex);
      gOFS->eosFsView->shrink();
      std_out += "\ninfo: resized namespace filesystem view ...";
    } break;

    case eos::console::SpaceProto_ResetProto::NSFILEMAP: {
      auto *eos_chlog_filesvc = dynamic_cast<eos::IChLogFileMDSvc *>(gOFS->eosFileService);
      if (eos_chlog_filesvc) {
        eos::common::RWMutexWriteLock lock(gOFS->eosViewRWMutex);
        eos_chlog_filesvc->resize();
        std_out += "\ninfo: resized namespace file map ...";
      } else {
        std_out += "\n info: ns does not support file map resizing";
      }
    } break;

    case eos::console::SpaceProto_ResetProto::NSDIRECTORYMAP: {
      auto *eos_chlog_dirsvc = dynamic_cast<eos::IChLogContainerMDSvc *>(gOFS->eosDirectoryService);
      if (eos_chlog_dirsvc) {
        eos::common::RWMutexWriteLock lock(gOFS->eosViewRWMutex);
        eos_chlog_dirsvc->resize();
        std_out += "\ninfo: resized namespace directory map ...";
      } else {
        std_out += "\ninfo: ns does not support directory map resizing";
      }
    } break;

    case eos::console::SpaceProto_ResetProto::NS: {
      eos::common::RWMutexWriteLock lock(gOFS->eosViewRWMutex);
      gOFS->eosFsView->shrink();
      auto *eos_chlog_filesvc = dynamic_cast<eos::IChLogFileMDSvc *>(gOFS->eosFileService);
      auto *eos_chlog_dirsvc = dynamic_cast<eos::IChLogContainerMDSvc *>(gOFS->eosDirectoryService);

      if (eos_chlog_filesvc && eos_chlog_dirsvc) {
        eos_chlog_filesvc->resize();
        eos_chlog_dirsvc->resize();
        std_out += "\ninfo: resized all namespace map ...";
      } else {
        std_out += "\ninfo: ns does not support map resizing";
      }
    } break;

    case eos::console::SpaceProto_ResetProto::MAPPING: {
      eos::common::Mapping::Reset();
      std_out += "\ninfo: clear all user/group uid/gid caches ...\n";
    } break;

    case eos::console::SpaceProto_ResetProto::SCHEDULEDRAIN: {
      gOFS->mDrainingTracker.Clear();
      std_out = "info: reset drain scheduling map in space '" + reset.mgmspace() + "'";
    } break;

    case eos::console::SpaceProto_ResetProto::SCHEDULEBALANCE: {
      gOFS->mBalancingTracker.Clear();
      std_out = "info: reset balance scheduling map in space '" + reset.mgmspace() + "'";
    } break;

    default: { // NONE - when NONE does cases DRAIN and EGROUP and MAPPING
      if (FsView::gFsView.mSpaceView.count(reset.mgmspace())) {
        FsView::gFsView.mSpaceView[reset.mgmspace()]->ResetDraining();
        std_out += "info: reset draining in space '" + reset.mgmspace() + "'";
      } else {
        std_err += "error: illegal space name";
        ret_c = EINVAL;
      }
      gOFS->EgroupRefresh->Reset();
      std_out += "\ninfo: clear cached EGroup information ...";
      eos::common::Mapping::Reset();
      std_out += "\ninfo: clear all user/group uid/gid caches ...\n";
    } break;
  }

  reply.set_std_out(std_out);
  reply.set_std_err(std_err);
  reply.set_retc(ret_c);

}

//----------------------------------------------------------------------------
// Execute define subcommand
//----------------------------------------------------------------------------
void SpaceCmd::DefineSubcmd(const eos::console::SpaceProto_DefineProto& define, eos::console::ReplyProto& reply) {

  if (mVid.uid != 0) {
    reply.set_std_err("error: you have to take role 'root' to execute this command");
    reply.set_retc(EPERM);
    return;
  }

  if (define.mgmspace().empty()) {
    reply.set_std_err("error: illegal parameters <space-name>");
    reply.set_retc(EINVAL);
    return;
  }
  if (define.groupsize() < 0 || (define.groupsize() > 1024)) {
    reply.set_std_err("error: <groupsize> must be a positive integer (<=1024)!");
    reply.set_retc(EINVAL);
    return;
  }
  if (define.groupmod() < 0 || (define.groupmod() > 256)) {
    reply.set_std_err("error: <groupmod> must be a positive integer (<=256)!");
    reply.set_retc(EINVAL);
    return;
  }


  eos::common::RWMutexWriteLock lock(FsView::gFsView.ViewMutex);

  if (!FsView::gFsView.mSpaceView.count(define.mgmspace())) {

    reply.set_std_out("info: creating space '" + define.mgmspace() + "'");

    if (!FsView::gFsView.RegisterSpace(define.mgmspace().c_str())) {
      reply.set_std_err("error: cannot register space <" + define.mgmspace() + ">");
      reply.set_retc(EIO);
    } else {
      // set this new space parameters
      if ((!FsView::gFsView.mSpaceView[define.mgmspace()]->SetConfigMember(std::string("groupsize"), std::to_string(define.groupsize()), true, "/eos/*/mgm")) ||
          (!FsView::gFsView.mSpaceView[define.mgmspace()]->SetConfigMember(std::string("groupmod"), std::to_string(define.groupmod()), true, "/eos/*/mgm"))) {
        reply.set_retc(EIO);
        reply.set_std_err("error: cannot set space config value");
      }
    }

  }

}

//----------------------------------------------------------------------------
// Execute config subcommand
//----------------------------------------------------------------------------
void SpaceCmd::ConfigSubcmd(const eos::console::SpaceProto_ConfigProto& config, eos::console::ReplyProto& reply) {

  if (mVid.uid != 0) {
    reply.set_std_err("error: you have to take role 'root' to execute this command");
    reply.set_retc(EPERM);
    return;
  }

  if ( config.mgmspace_name().empty() || config.mgmspace_key().empty() || config.mgmspace_value().empty() ) {
    reply.set_std_err("error: illegal parameters");
    reply.set_retc(EINVAL);
    return;
  }

  std::string key = config.mgmspace_key();
  std::string value = config.mgmspace_value();

  std::string std_out, std_err;
  int ret_c = 0;


  eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
  FileSystem *fs = nullptr;
  // by host:port name

    if (FsView::gFsView.mSpaceView.count(config.mgmspace_name())) {
      // set a space related parameter
      if (!key.compare(0, 6, "space.")) {
        key.erase(0, 6);
        if (key.substr(0, 7) == "policy.") {
          if (value == "remove") {
            if (!FsView::gFsView.mSpaceView[config.mgmspace_name()]->DeleteConfigMember(key)) {
              ret_c = ENOENT;
              std_err = "error: key has not been deleted";
            } else {
              std_out = "success: removed space policy '" + key + "'\n";
            }
          } else {
            // set a space policy parameters e.g. default placement attributes
            if (!FsView::gFsView.mSpaceView[config.mgmspace_name()]->SetConfigMember(key, value, true, "/eos/*/mgm")) {
              ret_c = EIO;
              std_err = "error: cannot set space config value";
            } else {
              std_out = "success: configured policy in space='" + config.mgmspace_name() + "' as " + key + "='" + value + "'\n";
              ret_c = 0;
            }
          }
        } else {
          if ((key == "nominalsize") ||
              (key == "headroom") ||
              (key == "scaninterval") ||
              (key == "scanrate") ||
              (key == "graceperiod") ||
              (key == "drainperiod") ||
              (key == "balancer") ||
              (key == "balancer.node.rate") ||
              (key == "balancer.node.ntx") ||
              (key == "drainer.node.rate") ||
              (key == "drainer.node.ntx") ||
              (key == "drainer.node.nfs") ||
              (key == "drainer.retries") ||
              (key == "drainer.fs.ntx") ||
              (key == "converter") ||
              (key == "tracker") ||
              (key == "inspector") ||
              (key == "inspector.interval") ||
              (key == "lru") ||
              (key == "lru.interval") ||
              (key == "wfe") ||
              (key == "wfe.interval") ||
              (key == "wfe.ntx") ||
              (key == "converter.ntx") ||
              (key == "autorepair") ||
              (key == "groupbalancer") ||
              (key == "groupbalancer.ntx") ||
              (key == "groupbalancer.threshold") ||
              (key == "geobalancer") ||
              (key == "geobalancer.ntx") ||
              (key == "geobalancer.threshold") ||
              (key == "geo.access.policy.read.exact") ||
              (key == "geo.access.policy.write.exact") ||
              (key == "filearchivedgc") ||
              (key == "tapeawaregc.spacequeryperiodsecs") ||
              (key == "tapeawaregc.minfreebytes") ||
              (key == "balancer.threshold")) {
            if ((key == "balancer") || (key == "converter") || (key == "tracker") || (key == "inspector") ||
                (key == "autorepair") || (key == "lru") ||
                (key == "groupbalancer") || (key == "geobalancer") ||
                (key == "geo.access.policy.read.exact") ||
                (key == "geo.access.policy.write.exact") ||
                (key == "filearchivedgc")) {
              if ((value != "on") && (value != "off")) {
                ret_c = EINVAL;
                std_err = "error: value has to either on or off";
              } else {
                if (!FsView::gFsView.mSpaceView[config.mgmspace_name()]->SetConfigMember(key, value, true, "/eos/*/mgm")) {
                  ret_c = EIO;
                  std_err = "error: cannot set space config value";
                } else {
                  if (key == "balancer") {
                    if (value == "on") {
                      std_out += "success: balancer is enabled!";
                    } else {
                      std_out += "success: balancer is disabled!";
                    }
                  }

                  if (key == "converter") {
                    if (value == "on") {
                      std_out += "success: converter is enabled!";
                    } else {
                      std_out += "success: converter is disabled!";
                    }
                  }

                  if (key == "tracker") {
                    if (value == "on") {
                      gOFS->mReplicationTracker->enable();
                      std_out += "success: tracker is enabled!";
                    } else {
                      gOFS->mReplicationTracker->disable();
                      std_out += "success: tracker is disabled!";
                    }
                  }

                  if (key == "inspector") {
                    if (value == "on") {
                      gOFS->mFileInspector->enable();
                      std_out += "success: file inspector is enabled!";
                    } else {
                      gOFS->mFileInspector->disable();
                      std_out += "success: file inspector is disabled!";
                    }
                  }

                  if (key == "autorepair") {
                    if (value == "on") {
                      std_out += "success: auto-repair is enabled!";
                    } else {
                      std_out += "success: auto-repair is disabled!";
                    }
                  }

                  if (key == "groupbalancer") {
                    if (value == "on") {
                      std_out += "success: groupbalancer is enabled!";
                    } else {
                      std_out += "success: groupbalancer is disabled!";
                    }
                  }

                  if (key == "geobalancer") {
                    if (value == "on") {
                      std_out += "success: geobalancer is enabled!";
                    } else {
                      std_out += "success: geobalancer is disabled!";
                    }
                  }

                  if (key == "geo.access.policy.read.exact") {
                    if (value == "on") {
                      std_out += "success: geo access policy prefers the exact geo matching replica for reading!";
                    } else {
                      std_out += "success: geo access policy prefers with a weight the geo matching replica for reading!";
                    }
                  }

                  if (key == "geo.access.policy.write.exact") {
                    if (value == "on") {
                      std_out += "success: geo access policy prefers the exact geo matching replica for placements!";
                    } else {
                      std_out += "success: geo access policy prefers with a weight the geo matching replica for placements!";
                    }
                  }

                  if (key == "scheduler.skip.overloaded") {
                    if (value == "on") {
                      std_out += "success: scheduler skips overloaded eth-out nodes!";
                    } else {
                      std_out += "success: scheduler does not skip overloaded eth-out nodes!";
                    }
                  }

                  if (key == "filearchivedgc") {
                    if (value == "on") {
                      std_out += "success: 'file archived' garbage collector is enabled";
                    } else {
                      std_out += "success: 'file archived' garbage collector is disabled";
                    }
                  }
                }
              }
            } else if (key == "wfe") {
              if ((value != "on") && (value != "off") && (value != "paused")) {
                ret_c = EINVAL;
                std_err = "error: value has to either on, paused or off";
              } else {
                if (!FsView::gFsView.mSpaceView[config.mgmspace_name()]->SetConfigMember(key, value, true, "/eos/*/mgm")) {
                  ret_c = EIO;
                  std_err = "error: cannot set space config value";
                }
              }
            } else {
              errno = 0;
              unsigned long long size = eos::common::StringConversion::GetSizeFromString(value.c_str());

              if (!errno) {
                if ((key != "balancer.threshold") &&
                    (key != "groupbalancer.threshold") &&
                    (key != "geobalancer.threshold")) {
                  // the threshold is allowed to be decimal!
                  char ssize[1024];
                  snprintf(ssize, sizeof(ssize) - 1, "%llu", size);
                  value = ssize;
                }

                if (!FsView::gFsView.mSpaceView[config.mgmspace_name()]->SetConfigMember(key, value, true, "/eos/*/mgm")) {
                  ret_c = EIO;
                  std_err = "error: cannot set space config value";
                } else {
                  std_out = "success: setting " + key + "=" + value;
                }
              } else {
                ret_c = EINVAL;
                std_err = "error: value has to be a positiv number";
              }
            }
          }
        }
      }

      // Set a filesystem related parameter
      if (!key.compare(0, 3, "fs.")) {
        key.erase(0, 3);
        // we disable the autosave, do all the updates and then switch back
        // to autosave and evt. save all changes
        gOFS->ConfEngine->SetAutoSave(false);

        // Store these as a global parameters of the space
        if (((key == "headroom") || (key == "scaninterval") ||
             (key == "scanrate") || (key == "graceperiod") ||
             (key == "drainperiod"))) {
          unsigned long long size = eos::common::StringConversion::GetSizeFromString(
              value.c_str());
          char ssize[1024];
          snprintf(ssize, sizeof(ssize) - 1, "%llu", size);

          if ((!FsView::gFsView.mSpaceView[config.mgmspace_name()]->SetConfigMember(key, ssize, true, "/eos/*/mgm"))) {
            std_err += "error: failed to set space parameter <" + key + ">\n";
            ret_c = EINVAL;
          }
        } else {
          if (key != "configstatus") {
            std_err += "error: not an allowed parameter <" + key + ">\n";
            ret_c = EINVAL;
          }
        }

        for (auto it = FsView::gFsView.mSpaceView[config.mgmspace_name()]->begin(); it != FsView::gFsView.mSpaceView[config.mgmspace_name()]->end(); ++it) {

          fs = FsView::gFsView.mIdView.lookupByID(*it);

          if (fs) {
            // check the allowed strings
            if (((key == "configstatus") &&
                 (eos::common::FileSystem::GetConfigStatusFromString(value.c_str()) !=
                  eos::common::ConfigStatus::kUnknown))) {
              fs->SetString(key.c_str(), value.c_str());

              if (value == "off") {
                // we have to remove the errc here, otherwise we cannot
                // terminate drainjobs on file systems with errc set
                fs->SetString("errc", "0");
              }

              FsView::gFsView.StoreFsConfig(fs);
            } else {
              errno = 0;
              eos::common::StringConversion::GetSizeFromString(value.c_str());

              if (((key == "headroom") || (key == "scaninterval") ||
                   (key == "scanrate") || (key == "graceperiod") ||
                   (key == "drainperiod")) && (!errno)) {
                fs->SetLongLong(key.c_str(), eos::common::StringConversion::GetSizeFromString(value.c_str()));
                FsView::gFsView.StoreFsConfig(fs);
              } else {
                std_err += "error: not an allowed parameter <" + key + ">\n";
                ret_c = EINVAL;
                break;
              }
            }
          } else {
            std_err += "error: cannot identify the filesystem by <" + config.mgmspace_name() + ">\n";
            ret_c = EINVAL;
          }
        }

        gOFS->ConfEngine->SetAutoSave(true);
        gOFS->ConfEngine->AutoSave();
      }
    } else {
      ret_c = EINVAL;
      std_err = "error: cannot find space <" + config.mgmspace_name() + ">";
    }


  reply.set_std_out(std_out);
  reply.set_std_err(std_err);
  reply.set_retc(ret_c);


}

//----------------------------------------------------------------------------
// Execute quota subcommand
//----------------------------------------------------------------------------
void SpaceCmd::QuotaSubcmd(const eos::console::SpaceProto_QuotaProto& quota, eos::console::ReplyProto& reply) {

  std::string key = "quota";
  std::string onoff = (quota.quota_switch()) ? "on" : "off" ;

  if (mVid.uid != 0) {
    reply.set_std_err("error: you have to take role 'root' to execute this command");
    reply.set_retc(EPERM);
    return;
  }

  if ( quota.mgmspace().empty() ) {
    reply.set_std_err("error: illegal parameters");
    reply.set_retc(EINVAL);
    return;
  }

  eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);

  if (FsView::gFsView.mSpaceView.count(quota.mgmspace())) {
    if (!FsView::gFsView.mSpaceView[quota.mgmspace()]->SetConfigMember(key, onoff, true, "/eos/*/mgm")) {
      reply.set_std_err("error: cannot set space config value");
      reply.set_retc(EIO);

    }
  } else {
    reply.set_std_err("error: no such space defined");
    reply.set_retc(EINVAL);
  }


}

//----------------------------------------------------------------------------
// Execute rm subcommand
//----------------------------------------------------------------------------
void SpaceCmd::RmSubcmd(const eos::console::SpaceProto_RmProto& rm, eos::console::ReplyProto& reply) {

  if (mVid.uid != 0) {
    reply.set_std_err("error: you have to take role 'root' to execute this command");
    reply.set_retc(EPERM);
    return;
  }

  if ( rm.mgmspace().empty() ) {
    reply.set_std_err("error: illegal parameters");
    reply.set_retc(EINVAL);
    return;
  }

  eos::common::RWMutexWriteLock lock(FsView::gFsView.ViewMutex);

  if (!FsView::gFsView.mSpaceView.count(rm.mgmspace())) {
    reply.set_std_err("error: no such space '" + rm.mgmspace() + "'");
    reply.set_retc(ENOENT);
    return;
  }


  for (auto it = FsView::gFsView.mSpaceView[rm.mgmspace()]->begin(); it != FsView::gFsView.mSpaceView[rm.mgmspace()]->end(); it++) {

    FileSystem* fs = FsView::gFsView.mIdView.lookupByID(*it);

    if (fs) {
      // check that filesystems are empty
      if ((fs->GetConfigStatus(false) != eos::common::ConfigStatus::kEmpty)) {
        reply.set_std_err("error: unable to remove space '" + rm.mgmspace() + "' - filesystems are not all in empty state - try to drain them or: space config <name> configstatus=empty\n");
        reply.set_retc(EBUSY);
        return;
      }
    }
  }

  std::string spaceconfigname = eos::common::GlobalConfig::QueuePrefixName(FsSpace::sGetConfigQueuePrefix(), rm.mgmspace().c_str());

  if (!eos::common::GlobalConfig::gConfig.SOM()->DeleteSharedHash(spaceconfigname.c_str())) {
    reply.set_std_err("error: unable to remove config of space '" + rm.mgmspace() + "'");
    retc = EIO;
  } else {
    if (FsView::gFsView.UnRegisterSpace(rm.mgmspace().c_str())) {
      reply.set_std_out("success: removed space '" + rm.mgmspace() + "'");
    } else {
      reply.set_std_err("error: unable to unregister space '" + rm.mgmspace() + "'");
    }
  }

}

//----------------------------------------------------------------------------
// Execute tracker subcommand
//----------------------------------------------------------------------------
void SpaceCmd::TrackerSubcmd(const eos::console::SpaceProto_TrackerProto& tracker, eos::console::ReplyProto& reply) {

  std::string std_out;

  std::string tmp;
  gOFS->mReplicationTracker->Scan(2*86400, false, &tmp);
  std_out += "# ------------------------------------------------------------------------------------\n";
  std_out += tmp;
  std_out += "# ------------------------------------------------------------------------------------\n";

  reply.set_std_out(std_out);
  reply.set_retc(SFS_OK);

}

//----------------------------------------------------------------------------
// Execute inspector subcommand
//----------------------------------------------------------------------------
void SpaceCmd::InspectorSubcmd(const eos::console::SpaceProto_InspectorProto& inspector, eos::console::ReplyProto& reply) {

  std::string options = inspector.options();

  std::string std_out;

  gOFS->mFileInspector->Dump(std_out, options);

  reply.set_std_out(std_out);
  reply.set_retc(SFS_OK);

}


EOSMGMNAMESPACE_END