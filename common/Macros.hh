// ----------------------------------------------------------------------
// File: Macros.hh
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

/**
 * @file   Macros.hh
 *
 * @brief  Convenience macros for 64-bit network byte order conversion
 *
 *
 */

#ifndef __EOSCOMMON_MACROS_HH__
#define __EOSCOMMON_MACROS_HH__

#include <arpa/inet.h>

inline unsigned long long h_tonll(unsigned long long n)
{
#if __BYTE_ORDER == __BIG_ENDIAN
  return n;
#else
  return (((unsigned long long)htonl(n)) << 32) + htonl(n >> 32);
#endif
}

inline unsigned long long n_tohll(unsigned long long n)
{
#if __BYTE_ORDER == __BIG_ENDIAN
  return n;
#else
  return (((unsigned long long)ntohl(n)) << 32) + ntohl(n >> 32);
#endif
}

#endif // #ifndef __EOSCOMMON_MACROS_HH__
