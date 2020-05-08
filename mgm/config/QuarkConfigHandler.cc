// ----------------------------------------------------------------------
// File: QuarkConfigHandler.cc
// Author: Georgios Bitzes - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2020 CERN/Switzerland                                  *
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

#include "mgm/config/QuarkConfigHandler.hh"
#include "common/Assert.hh"

#include <qclient/QClient.hh>
#include <qclient/ResponseParsing.hh>

EOSMGMNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
QuarkConfigHandler::QuarkConfigHandler(const QdbContactDetails &cd)
: mContactDetails(cd) {
  mQcl = std::unique_ptr<qclient::QClient>(
    new qclient::QClient(mContactDetails.members, mContactDetails.constructOptions()));
}

//------------------------------------------------------------------------------
// Fetch a given configuration
//------------------------------------------------------------------------------
bool QuarkConfigHandler::fetchConfiguration(const std::string &name, std::map<std::string, std::string> &out, std::string &err) {
  qclient::redisReplyPtr reply = mQcl->exec("HGETALL", SSTR("eos-config:" << name)).get();
  qclient::HgetallParser parser(reply);

  if(!parser.ok()) {
    err = parser.err();
    return false;
  }

  out = parser.value();
  return true;
}

EOSMGMNAMESPACE_END
