// ----------------------------------------------------------------------
// File: CurrentAndPrev.hh
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

#ifndef __EOSMGMTGC_CURRENTANDPREV_HH__
#define __EOSMGMTGC_CURRENTANDPREV_HH__

#include "mgm/Namespace.hh"

/*----------------------------------------------------------------------------*/
/**
 * @file CurrentAndPrev.hh
 *
 * @brief Templated struct to store a current and previous value.
 *
 */
/*----------------------------------------------------------------------------*/
EOSTGCNAMESPACE_BEGIN

//----------------------------------------------------------------------------
//! Templated struct to store a current and previous value.
//!
//! @tparam ValueType The type of the value.
//----------------------------------------------------------------------------
template <typename ValueType> struct CurrentAndPrev {

  /// The current value
  ValueType current;

  // The previous value
  ValueType prev;
};

EOSTGCNAMESPACE_END

#endif
