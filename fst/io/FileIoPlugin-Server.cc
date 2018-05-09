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

#include <fst/XrdFstOfs.hh>
#include <fst/storage/FileSystem.hh>
#include "fst/io/FileIoPlugin.hh"
#include "fst/io/FileIoPluginCommon.hh"
#include "fst/io/kinetic/KineticIo.hh"
#include "fst/io/local/LocalIo.hh"

#ifdef RADOS_FOUND
#include "fst/io/exos/ExosIo.hh"
#endif

#ifdef DAVIX_FOUND
#include "fst/io/davix/DavixIo.hh"
#endif

EOSFSTNAMESPACE_BEGIN

using eos::common::LayoutId;

//------------------------------------------------------------------------------
// Get IO object
//------------------------------------------------------------------------------
FileIo*
FileIoPlugin::GetIoObject(std::string path,
                          XrdFstOfsFile* file,
                          const XrdSecEntity* client)
{
  auto ioType = eos::common::LayoutId::GetIoType(path.c_str());

  if (ioType == LayoutId::kLocal) {
    return static_cast<FileIo*>(new LocalIo(path, file, client));
  } else if (ioType == LayoutId::kXrdCl) {
    return static_cast<FileIo*>(new XrdIo(path));
  } else if (ioType == LayoutId::kKinetic) {
    FileIo* kio = NULL;
    try {
      kio = static_cast<FileIo*>((FileIo*)new KineticIo(path));
    } catch (const std::exception& e) {
      eos_static_err("Failed constructing kinetic io object: %s", e.what());
    }
    return kio;
  } else if (ioType == LayoutId::kRados) {
    return static_cast<FileIo*>(new RadosIo(path));
  } else if (ioType == LayoutId::kDavix) {
#ifdef DAVIX_FOUND
    std::string s3credentials = "";

    // Attempt to retrieve S3 credentials from the filesystem
    if (file) {
      FileSystem *fileSystem =
          gOFS.Storage->GetFileSystemById(file->getFileSystemId());
      s3credentials = fileSystem->GetString("s3credentials");
    }

    return static_cast<FileIo*>(new DavixIo(path, s3credentials));
#endif
    eos_static_warning("EOS has been compiled without DAVIX support.");
    return NULL;
  } else {
    return FileIoPluginHelper::GetIoObject(path, file, client);
  }
}

EOSFSTNAMESPACE_END
