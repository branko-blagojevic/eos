// ----------------------------------------------------------------------
// File: com_rm.cc
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
#include "console/ConsoleMain.hh"
#include "common/StringTokenizer.hh"
#include "common/Path.hh"
/*----------------------------------------------------------------------------*/

/* Remove a file */
int
com_rm(char* arg1)
{
  // split subcommands
  eos::common::StringTokenizer subtokenizer(arg1);
  subtokenizer.GetLine();
  XrdOucString s1 = subtokenizer.GetToken();
  XrdOucString s2 = subtokenizer.GetToken();
  XrdOucString path;
  XrdOucString option;
  eos::common::Path* cPath = 0;
  XrdOucString in = "mgm.cmd=rm&";

  if (wants_help(arg1)) {
    goto com_rm_usage;
  }

  if ((s1 == "--help") || (s1 == "-h")) {
    goto com_rm_usage;
  }

  if (s1 == "-r") {
    option = "r";
    path = s2;
  } else if ((s1 == "-rF") || (s1 == "-Fr")) {
    option = "rf";
    path = s2;
  } else if (s1 == "-f") {
    option = "f";
    path = s2;
  } else if (s1.beginswith("-")) {
    goto com_rm_usage;
  } else {
    option = "";
    path = s1;
  }

  do {
    XrdOucString param = subtokenizer.GetToken();

    if (param.length()) {
      path += " ";
      path += param;
    } else {
      break;
    }
  } while (1);

  // remove escaped blanks
  while (path.replace("\\ ", " ")) {
  }

  if (!path.length()) {
    goto com_rm_usage;
  } else {
    unsigned long long id;
    if (Path2FileDenominator(path, id)) {
      in += "&mgm.file.id=";
      in += std::to_string(id).c_str();
    } else {
      if (Path2ContainerDenominator(path, id)) {
	in += "&mgm.container.id=";
	in += std::to_string(id).c_str();
      } else {
	path = abspath(path.c_str());
	in += "&mgm.path=";
	in += path;
      }
    }

    in += "&mgm.option=";
    in += option;
    cPath = new eos::common::Path(path.c_str());

    if ((option == "r") && (cPath->GetSubPathSize() < 4)) {
      string s;
      fprintf(stdout, "Do you really want to delete ALL files starting at %s ?\n",
              path.c_str());
      fprintf(stdout, "Confirm the deletion by typing => ");
      XrdOucString confirmation = "";

      for (int i = 0; i < 10; i++) {
        confirmation += (int)(9.0 * rand() / RAND_MAX);
      }

      fprintf(stdout, "%s\n", confirmation.c_str());
      fprintf(stdout, "                               => ");
      getline(std::cin, s);
      std::string sconfirmation = confirmation.c_str();

      if (s == sconfirmation) {
        fprintf(stdout, "\nDeletion confirmed\n");
        in += "&mgm.deletion=deep";
        delete cPath;
      } else {
        fprintf(stdout, "\nDeletion aborted\n");
        global_retc = EINTR;
        delete cPath;
        return (0);
      }
    }

    global_retc = output_result(client_command(in));
    return (0);
  }

com_rm_usage:
  fprintf(stdout,
          "usage: rm [-rF] [<path>|fid:<fid-dec>|fxid:<fid-hex>]                    :  remove file <path>\n");
  fprintf(stdout,
          "                                                                    -r :  remove recursivly\n");
  fprintf(stdout,
          "                                                                    -F :  remove bypassing recycling policies (you have to take the root role to use this flag!)\n");
  global_retc = EINVAL;
  return (0);
}
