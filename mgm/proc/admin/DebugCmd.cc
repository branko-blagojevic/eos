//------------------------------------------------------------------------------
// File: DebugCmd.cc
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

#include "DebugCmd.hh"
#include "mgm/proc/ProcInterface.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/Messaging.hh"
#include "mgm/FsView.hh"

EOSMGMNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Method implementing the specific behavior of the command executed by the
// asynchronous thread
//------------------------------------------------------------------------------
eos::console::ReplyProto
DebugCmd::ProcessRequest() noexcept
{
  eos::console::ReplyProto reply;
  eos::console::DebugProto debug = mReqProto.debug();
  eos::console::DebugProto::SubcmdCase subcmd = debug.subcmd_case();

  switch (subcmd) {
  case eos::console::DebugProto::kGet:
    GetSubcmd(debug.get(), reply);
    break;

  case eos::console::DebugProto::kSet:
    SetSubcmd(debug.set(), reply);
    break;

  default:
    reply.set_retc(EINVAL);
    reply.set_std_err("error: not supported");
  }

  return reply;
}

//------------------------------------------------------------------------------
// Execute get subcommand
//------------------------------------------------------------------------------
void DebugCmd::GetSubcmd(const eos::console::DebugProto_GetProto& get,
                         eos::console::ReplyProto& reply)
{
  std::string std_out, std_err;
  eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
  eos::common::Logging& g_logging = eos::common::Logging::GetInstance();
  std_out +=
    "# ------------------------------------------------------------------------------------\n";
  std_out += "# Debug log level\n";
  std_out +=
    "# ....................................................................................\n";
  std::string priority = g_logging.GetPriorityString(g_logging.gPriorityLevel);
  std::for_each(priority.begin(), priority.end(), [](char& c) {
    c = ::tolower(static_cast<unsigned char>(c));
  });
  std_out += "/eos/" + (std::string) gOFS->HostName + ':'
             + std::to_string(gOFS->ManagerPort).c_str()
             + "/mgm := \t"
             + priority.c_str()
             + '\n';
  auto nodes = FsView::gFsView.mNodeView;

  for (auto node = nodes.begin(); node != nodes.end(); ++node) {
    std_out += (node->first + " := \t" +
                FsView::gFsView.mNodeView[node->first]->GetConfigMember("debug.state") +
                '\n').c_str();
  }

  reply.set_std_out(std_out.c_str());
  reply.set_std_err(std_err.c_str());
  reply.set_retc(ret_c);
}

//------------------------------------------------------------------------------
// @todo(faluchet): add a comment on why you need this function and what is
// doing
//------------------------------------------------------------------------------
std::string rebuild_pOpaque(const eos::console::DebugProto_SetProto& set)
{
  std::string in = "mgm.cmd=debug";

  if (set.debuglevel().length()) {
    in += "&mgm.debuglevel=" + set.debuglevel();
  }

  if (set.nodename().length()) {
    in += "&mgm.nodename=" + set.nodename();
  }

  if (set.filter().length()) {
    in += "&mgm.filter=" + set.filter();
  }

  return in;
}

//------------------------------------------------------------------------------
// Execute set subcommand
//------------------------------------------------------------------------------
void DebugCmd::SetSubcmd(const eos::console::DebugProto_SetProto& set,
                         eos::console::ReplyProto& reply)
{
  if (mVid.uid != 0) {
    std_err = "error: you have to take role 'root' to execute this command";
    ret_c = EPERM;
  } else {
    XrdMqMessage message("debug");
    // @todo(faluchet): do you still need this commented part?
    // @note(faluchet) not really, it is related to the 'rebuild_opaque' fun.
    // Some function calls of the debug command still relies on the old
    // implementation of the command (not-protobuf) and I want to be sure that
    // the new behaviour mirrors the old one 1:1.
    // I will double check to clean everything before merging.
    // int envlen; //
    // std::string body = pOpaque->Env(envlen); //
    std::string body;
    // filter out several *'s ...
    int nstars = 0;
    int npos = 0;

    while ((npos = set.nodename().find("*", npos)) != STR_NPOS) {
      npos++;
      nstars++;
    }

    if (nstars > 1) {
      std_err = "error: debug level node can only contain one wildcard character (*) !";
      ret_c = EINVAL;
    } else {
      body = rebuild_pOpaque(set);
      //envlen = body.length(); @note (faluchet)
      message.SetBody(body.c_str());
      eos::common::Logging& g_logging = eos::common::Logging::GetInstance();
      // always check debug level exists first
      int debugval = g_logging.GetPriorityByString(set.debuglevel().c_str());

      if (debugval < 0) {
        std_err = ("error: debug level " + set.debuglevel() + " is not known!").c_str();
        ret_c = EINVAL;
      } else {
        if ((set.nodename() == "*") || (set.nodename() == "") ||
            (XrdOucString(set.nodename().c_str()) == gOFS->MgmOfsQueue)) {
          // this is for us!
          // int debugval = g_logging.GetPriorityByString(set.debuglevel().c_str());
          g_logging.SetLogPriority(debugval);
          std_out = ("success: debug level is now <" + set.debuglevel() + '>').c_str();
          eos_static_notice("setting debug level to <%s>", set.debuglevel());

          if (set.filter().length()) {
            g_logging.SetFilter(set.filter().c_str());
            std_out += (" filter=" + set.filter()).c_str();
            eos_static_notice("setting message logid filter to <%s>", set.filter());
          }

          if (set.debuglevel() == "debug" &&
              ((g_logging.gAllowFilter.Num() &&
                g_logging.gAllowFilter.Find("SharedHash")) ||
               ((g_logging.gDenyFilter.Num() == 0) ||
                (g_logging.gDenyFilter.Find("SharedHash") == 0)))
             ) {
            gOFS->ObjectManager.SetDebug(true);
          } else {
            gOFS->ObjectManager.SetDebug(false);
          }
        }

        if (set.nodename() == "*") {
          std::string all_nodes;
          all_nodes = "/eos/*/fst";

          if (!Messaging::gMessageClient.SendMessage(message, all_nodes.c_str())) {
            std_err = ("error: could not send debug level to nodes mgm.nodename=" +
                       all_nodes + "\n").c_str();
            ret_c = EINVAL;
          } else {
            std_out = ("success: switched to mgm.debuglevel=" + set.debuglevel() +
                       " on nodes mgm.nodename=" + all_nodes + "\n").c_str();
            eos_static_notice("forwarding debug level <%s> to nodes mgm.nodename=%s",
                              set.debuglevel().c_str(), all_nodes.c_str());
          }

          all_nodes = "/eos/*/mgm";
          // Ignore return value as we've already set the loglevel for the
          // current instance. We're doing this only for the slave.
          (void) Messaging::gMessageClient.SendMessage(message,
              all_nodes.c_str()); // @note (faluchet)
          // if (!Messaging::gMessageClient.SendMessage(message, all_nodes.c_str())) {
          //   std_err += ("error: could not send debug level to nodes mgm.nodename=" +
          //              all_nodes).c_str();
          //   ret_c = EINVAL;
          // } else {
          std_out += ("success: switched to mgm.debuglevel=" + set.debuglevel() +
                      " on nodes mgm.nodename=" + all_nodes).c_str();
          eos_static_notice("forwarding debug level <%s> to nodes mgm.nodename=%s",
                            set.debuglevel().c_str(), all_nodes.c_str());
          // }
        } else {
          if (set.nodename() != "") {
            // send to the specified list
            if (!Messaging::gMessageClient.SendMessage(message, set.nodename().c_str())) {
              std_err = ("error: could not send debug level to nodes mgm.nodename=" +
                         set.nodename()).c_str();
              ret_c = EINVAL;
            } else {
              std_out = ("success: switched to mgm.debuglevel=" + set.debuglevel() +
                         " on nodes mgm.nodename=" + set.nodename()).c_str();
              eos_static_notice("forwarding debug level <%s> to nodes mgm.nodename=%s",
                                set.debuglevel().c_str(), set.nodename().c_str());
            }
          }
        }
      }
    }
  }

  reply.set_std_out(std_out.c_str());
  reply.set_std_err(std_err.c_str());
  reply.set_retc(ret_c);
}

EOSMGMNAMESPACE_END
