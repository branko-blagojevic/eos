//------------------------------------------------------------------------------
// File: StagerRmCmd.cc
// Author: Jozsef Makai - CERN
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


#include "StagerRmCmd.hh"
#include "mgm/XrdMgmOfs.hh"
#include "namespace/interface/IView.hh"

EOSMGMNAMESPACE_BEGIN

eos::console::ReplyProto
eos::mgm::StagerRmCmd::ProcessRequest() {
  static constexpr auto tapeFsid = 65535u;
  eos::console::ReplyProto reply;
  std::ostringstream errStream;
  retc = 0;

  const auto& stagerRm = mReqProto.stagerrm();
  cerr << mReqProto.DebugString() << endl;
  cerr << stagerRm.path_size() << endl;

  XrdOucErrInfo errInfo;
  for (auto i = 0; i < stagerRm.path_size(); i++) {
    const auto& path = stagerRm.path(i);

    // check if this file exists
    XrdSfsFileExistence file_exists;
    errInfo.clear();
    if (gOFS->_exists(path.c_str(), file_exists, errInfo, mVid, nullptr)) {
      errStream << "error: unable to run exists on path '" << path << "'" << std::endl;
      retc = errno;
      continue;
    }

    if (file_exists == XrdSfsFileExistNo) {
      errStream << "error: no such file with path '" << path << "'" << std::endl;
      retc = ENODATA;
      continue;
    } else if (file_exists == XrdSfsFileExistIsDirectory) {
      errStream << "error: given path is a directory '" << path << "'" << std::endl;
      retc = EINVAL;
      continue;
    }

    struct stat buf;
    if (gOFS->_stat(path.c_str(), &buf, errInfo, mVid, nullptr, nullptr,
                    false) != 0) {
      errStream << "error: unable to run stat for replicas on path '" << path << "'" << std::endl;
      retc = EINVAL;
      continue;
    }

    // we don't remove anything if it's not on tape
    if ((buf.st_mode & EOS_TAPE_MODE_T) == 0) {
      errStream << "error: no tape replicas for file '" << path << "'" << std::endl;
      retc = EINVAL;
      continue;
    }

    eos::common::Mapping::VirtualIdentity root_vid;
    eos::common::Mapping::Root(root_vid);

    decltype(gOFS->eosView->getFile(path)->getLocations()) locationsCopy;
    {
      eos::common::RWMutexReadLock lock(gOFS->eosViewRWMutex);
      try {
        auto fmd = gOFS->eosView->getFile(path);
        locationsCopy = fmd->getLocations();
      } catch (eos::MDException& ex) {}
    }

    for (auto location : locationsCopy) {
      if (location != tapeFsid) {
        errInfo.clear();
        if (gOFS->_dropstripe(path.c_str(), errInfo, root_vid, location,
                              true) != 0) {
          eos_static_err("Could not delete file replica %s on filesystem %u. Reason: %s",
                         path.c_str(), location, errInfo.getErrText());
          errStream << "error: could not delete replica for file '" << path << "'"
                    << " on filesystem " << location << std::endl;
          retc = SFS_ERROR;
        }
      }
    }
  }

  reply.set_retc(retc);
  reply.set_std_err(errStream.str());
  reply.set_std_out(retc == 0 ? "success: removed all replicas for all given files" : "");
  return reply;
}

EOSMGMNAMESPACE_END