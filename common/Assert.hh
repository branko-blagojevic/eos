// ----------------------------------------------------------------------
// File: Assert.hh
// Author: Georgios Bitzers - CERN
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

#pragma once

#define eos_assert(condition) if(!((condition))) { std::cerr << "assertion violation in " << __PRETTY_FUNCTION__ << " at " << __FILE__ << ":" << __LINE__ << ", condition is not true: " << #condition << std::endl; std::quick_exit(1); }
#define SSTR(message) static_cast<std::ostringstream&>(std::ostringstream().flush() << message).str()
#define DBG(message) std::cerr << __FILE__ << ":" << __LINE__ << " -- " << #message << " = " << message << std::endl
