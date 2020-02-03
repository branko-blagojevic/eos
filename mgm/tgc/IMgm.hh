// ----------------------------------------------------------------------
// File: IMgm.hh
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

#ifndef __EOSMGMTGC_IMGM_HH__
#define __EOSMGMTGC_IMGM_HH__

#include "mgm/Namespace.hh"
#include "namespace/interface/IFileMD.hh"
#include "proto/ConsoleReply.pb.h"

#include <stdint.h>
#include <string>

/*----------------------------------------------------------------------------*/
/**
 * @file IMgm.hh
 *
 * @brief Specifies the tape-aware garbage collector's interface to the EOS MGM
 *
 */
/*----------------------------------------------------------------------------*/
EOSTGCNAMESPACE_BEGIN

//------------------------------------------------------------------------------
//! Specifies the tape-aware garbage collector's interface to the EOS MGM
//------------------------------------------------------------------------------
class IMgm {
public:
  //----------------------------------------------------------------------------
  //! @return The minimum number of free bytes the specified space should have
  //! as set in the configuration variables of the space.  If the minimum
  //! number of free bytes cannot be determined for whatever reason then 0 is
  //! returned.
  //!
  //! @param spaceName The name of the space
  //----------------------------------------------------------------------------
  virtual uint64_t getSpaceConfigMinFreeBytes(const std::string &spaceName) noexcept = 0;

  //----------------------------------------------------------------------------
  //! @param fid The file identifier
  //! @return The size of the specified file in bytes.  If the file cannot be
  //! found in the EOS namespace then a file size of 0 is returned.
  //----------------------------------------------------------------------------
  virtual uint64_t getFileSizeBytes(const IFileMD::id_t fid) = 0;

  //----------------------------------------------------------------------------
  //! Determine if the specified file exists and is not scheduled for deletion
  //!
  //! @param fid The file identifier
  //! @return True if the file exists in the EOS namespace and is not scheduled
  //! for deletion
  //----------------------------------------------------------------------------
  virtual bool fileInNamespaceAndNotScheduledForDeletion(const IFileMD::id_t fid) = 0;

  //----------------------------------------------------------------------------
  //! Execute stagerrm as user root
  //!
  //! \param fid The file identifier
  //! \return stagerrm result
  //----------------------------------------------------------------------------
  virtual console::ReplyProto stagerrmAsRoot(const IFileMD::id_t fid) = 0;
};

EOSTGCNAMESPACE_END

#endif
