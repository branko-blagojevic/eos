// ----------------------------------------------------------------------
// File: VirtualIdentity.cc
// Author: Georgios Bitzes - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2019 CERN/Switzerland                                  *
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

#include "common/VirtualIdentity.hh"

EOSCOMMONNAMESPACE_BEGIN

//----------------------------------------------------------------------------
//! Check if the uid vector contained has the requested uid
//----------------------------------------------------------------------------
bool VirtualIdentity::hasUid(uid_t uid) const {
  for(auto it = uid_list.begin(); it != uid_list.end(); it++) {
    if(*it == uid) {
      return true;
    }
  }

  return false;
}

//----------------------------------------------------------------------------
//! Check if the uid vector contained has the requested uid
//----------------------------------------------------------------------------
bool VirtualIdentity::hasGid(gid_t gid) const {
  for(auto it = gid_list.begin(); it != gid_list.end(); it++) {
    if(*it == gid) {
      return true;
    }
  }

  return false;
}


EOSCOMMONNAMESPACE_END
