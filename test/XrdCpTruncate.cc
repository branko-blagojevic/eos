// ----------------------------------------------------------------------
// File: XrdCpTruncate.cc
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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <XrdPosix/XrdPosixXrootd.hh>
#include <XrdCl/XrdClFileSystem.hh>
#include <XrdOuc/XrdOucString.hh>

XrdPosixXrootd posixXrootd;

int main(int argc, char* argv[])
{
  XrdOucString urlFile = argv[1];

  if (!urlFile.length()) {
    fprintf(stderr, "usage: xrdcpabort <url>\n");
    exit(EINVAL);
  }

  int fd = XrdPosixXrootd::Open(urlFile.c_str(),
                                O_CREAT | O_TRUNC | O_RDWR,
                                kXR_ur | kXR_uw | kXR_gw | kXR_gr | kXR_or);

  if (fd >= 0) {
    size_t sz = 10000000;
    char* buffer = (char*)malloc(sz);

    if (!buffer) {
      exit(-1);
    }

    for (size_t i = 0; i < sizeof(buffer); i++) {
      buffer[i] = i % 255;
    }

    XrdPosixXrootd::Pwrite(fd, buffer, sz, 0);
    XrdPosixXrootd::Ftruncate(fd, 2000000);
    XrdPosixXrootd::Pwrite(fd, buffer, sz, 1024);
    free(buffer);
  } else {
    exit(-1);
  }
}
