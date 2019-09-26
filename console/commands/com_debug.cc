// ----------------------------------------------------------------------
// File: com_debug.cc
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

/*----------------------------------------------------------------------------*/
#include "common/StringTokenizer.hh"
#include "console/ConsoleMain.hh"
/*----------------------------------------------------------------------------*/

/* Debug Level Setting */
int
com_debug(char* arg1)
{
  // split subcommands
  eos::common::StringTokenizer subtokenizer(arg1);
  subtokenizer.GetLine();
  XrdOucString level = subtokenizer.GetToken();
  XrdOucString nodequeue = subtokenizer.GetToken();
  XrdOucString filterlist = "";

  if ((level != "-h") && (level != "--help")) {
    if (level == "this") {
      debug = !debug;
      fprintf(stdout, "info: toggling shell debugmode to debug=%d\n", debug);
      return (0);
    }

    // @todo the addition of a subcommand makes the variable names of the parsed token strings very misleading. Change them
    if (level == "getloglevel") {
      XrdOucString in = "mgm.cmd=debug&mgm.subcmd=";
      in += level;
      global_retc = output_result(client_command(in, true));
      return (0);
    }

    if (level.length()) {
      XrdOucString in = "mgm.cmd=debug&mgm.debuglevel=";
      in += level;

      if (nodequeue.length()) {
        if (nodequeue == "--filter") {
          filterlist = subtokenizer.GetToken();
          in += "&mgm.filter=";
          in += filterlist;
        } else {
          in += "&mgm.nodename=";
          in += nodequeue;
          nodequeue = subtokenizer.GetToken();

          if (nodequeue == "--filter") {
            filterlist = subtokenizer.GetToken();
            in += "&mgm.filter=";
            in += filterlist;
          }
        }
      }

      global_retc = output_result(client_command(in, true));
      return (0);
    }
  }

  fprintf(stdout,
          "Usage: debug [node-queue] this|<level> [--filter <unitlist>]\n");
  fprintf(stdout,
          "'[eos] debug ...' allows to modify the verbosity of the EOS log files in MGM and FST services.\n\n");
  fprintf(stdout, "Options:\n");
  fprintf(stdout, "debug  this :\n");
  fprintf(stdout,
          "                                                  toggle EOS shell debug mode\n");
  fprintf(stdout, "debug  <level> [--filter <unitlist>] :\n");
  fprintf(stdout,
          "                                                  set the MGM where the console is connected to into debug level <level>\n");
  fprintf(stdout, "debug  <level> <node-queue> [--filter <unitlist>] :\n");
  fprintf(stdout,
          "                                                  set the <node-queue> into debug level <level>. <node-queue> are internal EOS names e.g. '/eos/<hostname>:<port>/fst'\n");
  fprintf(stdout,
          "     <unitlist> : a comma separated list of strings of software units which should be filtered out in the message log!\n");
  fprintf(stdout,
          "                  The default filter list is: 'Process,AddQuota,Update,UpdateHint,UpdateQuotaStatus,SetConfigValue,Deletion,GetQuota,PrintOut,RegisterNode,SharedHash,listenFsChange,\n");
  fprintf(stdout,
          "                  placeNewReplicas,placeNewReplicasOneGroup,accessReplicas,accessReplicasOneGroup,accessHeadReplicaMultipleGroup,updateTreeInfo,updateAtomicPenalties,updateFastStructures,work'.\n\n");
  fprintf(stdout,
          "The allowed debug levels are: debug info warning notice err crit alert emerg\n\n");
  fprintf(stdout, "Examples:\n");
  fprintf(stdout,
          "  debug info *                         set MGM & all FSTs into debug mode 'info'\n\n");
  fprintf(stdout,
          "  debug err /eos/*/fst                 set all FSTs into debug mode 'err'\n\n");
  fprintf(stdout,
          "  debug crit /eos/*/mgm                set MGM into debug mode 'crit'\n\n");
  fprintf(stdout,
          "  debug debug --filter MgmOfsMessage   set MGM into debug mode 'debug' and filter only messages coming from unit 'MgmOfsMessage'.\n\n");
  global_retc = EINVAL;
  return (0);
}
