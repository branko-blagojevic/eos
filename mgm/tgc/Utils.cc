// ----------------------------------------------------------------------
// File: Utils.cc
// Author: Steven Murray - CERN
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

#include "mgm/tgc/Utils.hh"

#include <cstring>

EOSTGCNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Return the integer representation of the specified string
//------------------------------------------------------------------------------
std::uint64_t
Utils::toUint64(const std::string &str)
{
  bool outOfRange = false;

  if(isValidUInt(str)) {
    try {
      return std::stoul(str);
    } catch(std::out_of_range &) {
      outOfRange = true;
    }
  }

  std::ostringstream errMsg;
  errMsg << "Invalid unsigned 64-bit integer: value=" << str;
  if(outOfRange) {
    errMsg << ",reason='Out of range'";
    throw OutOfRangeUint64(errMsg.str());
  } else {
    throw InvalidUint64(errMsg.str());
  }
}

//------------------------------------------------------------------------------
// Return true if the specified string is a valid unsigned integer
//------------------------------------------------------------------------------
bool
Utils::isValidUInt(std::string str)
{
  // left trim
  str.erase(0, str.find_first_not_of(" \t"));

  // An empty string is not a valid unsigned integer
  if(str.empty()) {
    return false;
  }

  // For each character in the string
  for(std::string::const_iterator itor = str.begin(); itor != str.end();
    itor++) {

    // If the current character is not a valid numerical digit
    if(*itor < '0' || *itor > '9') {
      return false;
    }
  }

  return true;
}

//------------------------------------------------------------------------------
// Return a copy of the specified buffer in the form of a timespec structure
//------------------------------------------------------------------------------
timespec
Utils::bufToTimespec(const std::string &buf) {
  if (sizeof(timespec) != buf.size()) {
    std::ostringstream msg;
    msg << __FUNCTION__ << " failed: Buffer size does match sizeof(timespec): buf.size()=" << buf.size() <<
      " sizeof(timespec)" << sizeof(timespec);
    throw BufSizeMismatch(msg.str());
  }

  timespec result;
  std::memcpy(&result, buf.data(), sizeof(timespec));

  return result;
}

EOSTGCNAMESPACE_END
