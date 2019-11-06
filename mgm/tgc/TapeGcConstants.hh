// ----------------------------------------------------------------------
// File: TapeGcConstants.hh
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

#ifndef __EOSMGM_TAPEGCCONSTANTS_HH__
#define __EOSMGM_TAPEGCCONSTANTS_HH__

#include "mgm/Namespace.hh"

#include <stdint.h>

/*----------------------------------------------------------------------------*/
/**
 * @file TapeGcConstants.hh
 *
 * @brief Constants specific to the implementation of the tape aware garbage
 * collector.
 *
 */
/*----------------------------------------------------------------------------*/
EOSTGCNAMESPACE_BEGIN

/// Default delay in seconds between free space queries
const uint64_t TAPEGC_DEFAULT_SPACE_QUERY_PERIOD_SECS=310;

EOSTGCNAMESPACE_END

#endif
