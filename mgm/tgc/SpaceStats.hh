// ----------------------------------------------------------------------
// File: SpaceStats.hh
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

#ifndef __EOSMGMTGC_SPACESTATS_HH__
#define __EOSMGMTGC_SPACESTATS_HH__

#include "mgm/Namespace.hh"

#include <cstdint>
#include <string>

/*----------------------------------------------------------------------------*/
/**
 * @file FreeAndUsedBytes.hh
 *
 * @brief Structure to store statistics about an EOS space
 *
 */
/*----------------------------------------------------------------------------*/
EOSTGCNAMESPACE_BEGIN

/*----------------------------------------------------------------------------*/
//! Structure to store the statistics about an EOS space
/*----------------------------------------------------------------------------*/
struct SpaceStats {
  std::uint64_t freeBytes;
  std::uint64_t usedBytes;

  SpaceStats(): freeBytes(0), usedBytes(0) {}
};

EOSTGCNAMESPACE_END

#endif
