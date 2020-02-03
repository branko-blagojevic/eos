// ----------------------------------------------------------------------
// File: TapeGcFreeSpace.hh
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

#ifndef __EOSMGM_TAPEGCFREESPACE_HH__
#define __EOSMGM_TAPEGCFREESPACE_HH__

#include "mgm/Namespace.hh"
#include "mgm/tgc/TapeGcCachedValue.hh"

#include <mutex>
#include <stdexcept>
#include <stdint.h>
#include <string>
#include <time.h>

/*----------------------------------------------------------------------------*/
/**
 * @file TapeGcFreeSpace.hh
 *
 * @brief Class for getting the amount of free space in a specific EOS space.
 *
 */
/*----------------------------------------------------------------------------*/
EOSTGCNAMESPACE_BEGIN

//------------------------------------------------------------------------------
//! Class for getting the amount of free space in a specific EOS space.
//------------------------------------------------------------------------------
class TapeGcFreeSpace {
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //! @param space The name of the space to be queried for free space.
  //! @param queryPeriodSecs The minimum delay between free space queries to the
  //! EOS MGM.
  //----------------------------------------------------------------------------
  TapeGcFreeSpace(const std::string &space, const time_t queryPeriodSecs);

  //----------------------------------------------------------------------------
  //! Notify this object that a file has been queued for deletion so that
  //! the amount of free space can be updated without having to wait for the
  //! next query to the EOS MGM
  //----------------------------------------------------------------------------
  void fileQueuedForDeletion(const size_t deletedFileSize);

  //----------------------------------------------------------------------------
  //! Returns the amount of free space in bytes
  //! @return the amount of free space in bytes
  //! @throw TapeAwareGcSpaceNotFound when the EOS space named m_spaceName
  //! cannot be found
  //----------------------------------------------------------------------------
  uint64_t getFreeBytes();

  //----------------------------------------------------------------------------
  //! @return the timestamp at which the last free space query was made
  //----------------------------------------------------------------------------
  time_t getFreeSpaceQueryTimestamp();

private:

  /// Mutex
  std::mutex m_mutex;

  /// The name of the EOS space to be queried for free space
  std::string m_space;

  //----------------------------------------------------------------------------
  //! Cached configuration value for the delay in seconds between space queries
  //! to the EOS MGM
  //----------------------------------------------------------------------------
  TapeGcCachedValue<time_t> m_queryPeriodSecs;

  /// The current amount of free space in bytes
  uint64_t m_freeSpaceBytes;

  /// The timestamp at which the last free space query was made
  time_t m_freeSpaceQueryTimestamp;

  //----------------------------------------------------------------------------
  //! Queries the EOS MGM for free space
  //! @param spaceName The name of the EOS space to be queried
  //! @return the amount of free space in bytes
  //! @throw TapeAwareGcSpaceNotFound when the EOS space named m_spaceName
  //! cannot be found
  //----------------------------------------------------------------------------
  uint64_t queryMgmForFreeBytes();

  //----------------------------------------------------------------------------
  //! @return The configured delay in seconds between free space queries for the
  //! specified space.  If the configuration value cannot be determined for
  //! whatever reason then the specified default value is returned.
  //!
  //! @param spaceName The name of the space
  //! @param defaultValue The default value
  //----------------------------------------------------------------------------
  static uint64_t getConfSpaceQueryPeriodSecs(const std::string spaceName,
    const uint64_t defaultValue) noexcept;
}; // class TapeGcFreeSpace

EOSTGCNAMESPACE_END

#endif
