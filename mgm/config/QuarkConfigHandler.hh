//------------------------------------------------------------------------------
// @file QuarkConfigHandler.hh
// @author Georgios Bitzes - CERN
//------------------------------------------------------------------------------

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

#pragma once
#include "mgm/Namespace.hh"
#include "common/Status.hh"
#include "namespace/ns_quarkdb/QdbContactDetails.hh"
#include <map>

namespace qclient {
  class QClient;
}

EOSMGMNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Class to perform reads and writes on the MGM configuration stored in QDB
//------------------------------------------------------------------------------
class QuarkConfigHandler {
public:
  //----------------------------------------------------------------------------
  // Constructor
  //----------------------------------------------------------------------------
  QuarkConfigHandler(const QdbContactDetails &cd);

  //----------------------------------------------------------------------------
  // Fetch a given configuration
  //----------------------------------------------------------------------------
  common::Status fetchConfiguration(const std::string &name, std::map<std::string, std::string> &out);

  //----------------------------------------------------------------------------
  // Form hash key
  //----------------------------------------------------------------------------
  static std::string formHashKey(const std::string &name);

  //----------------------------------------------------------------------------
  // Form backup key
  //----------------------------------------------------------------------------
  static std::string formBackupHashKey(const std::string &name, time_t timestamp);


private:
  QdbContactDetails mContactDetails;
  std::unique_ptr<qclient::QClient> mQcl;


};

EOSMGMNAMESPACE_END

