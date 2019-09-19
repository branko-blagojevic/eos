// ----------------------------------------------------------------------
// File: GlobalConfig.cc
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

#include "common/GlobalConfig.hh"
#include "common/Assert.hh"
#include "common/InstanceName.hh"

EOSCOMMONNAMESPACE_BEGIN

GlobalConfig
GlobalConfig::gConfig; //! Singleton for global configuration access

//------------------------------------------------------------------------------
// Get the global MGM configuration queue
//------------------------------------------------------------------------------
std::string
GlobalConfig::GetGlobalMgmConfigQueue() const
{
  return SSTR("/config/" << InstanceName::get() << "/mgm/");
}

EOSCOMMONNAMESPACE_END
