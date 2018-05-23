//------------------------------------------------------------------------------
//! @file FileIoPlugin.cc
//! @author Geoffray Adde - CERN
//! @brief Implementation of the FileIoPlugin for a client
//------------------------------------------------------------------------------

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

#include "fst/io/FileIoPlugin.hh"
#include "fst/io/FileIoPluginCommon.hh"
#include "fst/io/local/LocalIo.hh"

EOSFSTNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Get IO object
//------------------------------------------------------------------------------
FileIo*
FileIoPlugin::GetIoObject(std::string path,
                          XrdFstOfsFile* file,
                          const XrdSecEntity* client,
                          XrdOucEnv* env)
{
  return FileIoPluginHelper::GetIoObject(path, file, client, env);
}

EOSFSTNAMESPACE_END
