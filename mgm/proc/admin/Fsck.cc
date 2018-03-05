// ----------------------------------------------------------------------
// File: proc/admin/Fsck.cc
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
 * You should have received a copy of the AGNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

#include "mgm/proc/ProcInterface.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/Fsck.hh"

EOSMGMNAMESPACE_BEGIN

int
ProcCommand::Fsck ()
{
  if (pVid->uid == 0)
  {
    if (mSubCmd == "report")
    {
      auto option = pOpaque->Get("mgm.option") ? pOpaque->Get("mgm.option") : "";
      XrdOucString selection = pOpaque->Get("mgm.fsck.selection") ? pOpaque->Get("mgm.fsck.selection") : "";
      gOFS->FsCheck.Report(stdOut, option, selection);
    }

    if (mSubCmd == "repair")
    {
      XrdOucString option = "";
      XrdOucString mSelection = "";
      option = pOpaque->Get("mgm.option") ? pOpaque->Get("mgm.option") : "";
      if (option == "all")
      {
        retc = (
          gOFS->FsCheck.Repair(stdOut, stdErr, "checksum") &&
          gOFS->FsCheck.Repair(stdOut, stdErr, "unlink-unregistered") &&
          gOFS->FsCheck.Repair(stdOut, stdErr, "unlink-orphans") &&
          gOFS->FsCheck.Repair(stdOut, stdErr, "adjust-replicas") &&
          gOFS->FsCheck.Repair(stdOut, stdErr, "drop-missing-replicas") &&
          //               gOFS->FsCheck.Repair(stdOut, stdErr, "unlink-zero-replicas") && // we don't do that anymore for the 'all' option
          gOFS->FsCheck.Repair(stdOut, stdErr, "resync"));
        if (retc)
          retc = 0;
        else
          retc = EINVAL;
      }
      else
      {
        if (gOFS->FsCheck.Repair(stdOut, stdErr, option))
          retc = 0;
        else
          retc = EINVAL;
      }
    }
  }

  if (mSubCmd == "stat")
  {
    XrdOucString option = ""; // not used for the moment
    eos_info("fsck stat");
    gOFS->FsCheck.Stat(stdOut);
  }
  return SFS_OK;
}

EOSMGMNAMESPACE_END
