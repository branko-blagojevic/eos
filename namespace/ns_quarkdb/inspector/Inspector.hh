/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2019 CERN/Switzerland                                  *
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

//------------------------------------------------------------------------------
//! @author Georgios Bitzes <georgios.bitzes@cern.ch>
//! @brief Class for inspecting namespace contents - talks directly to QDB
//------------------------------------------------------------------------------

#pragma once
#include "namespace/Namespace.hh"
#include <string>
#include <map>

namespace qclient {
  class QClient;
}

EOSNSNAMESPACE_BEGIN

class ContainerScanner;
class FileScanner;

//------------------------------------------------------------------------------
//! Inspector class
//------------------------------------------------------------------------------
class Inspector {
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  Inspector(qclient::QClient &qcl);

  //----------------------------------------------------------------------------
  //! Is the connection to QDB ok? If not, pointless to run anything else.
  //----------------------------------------------------------------------------
  bool checkConnection(std::string &err);

  //----------------------------------------------------------------------------
  //! Dump contents of the given path. ERRNO-like integer return value, 0
  //! means no error.
  //----------------------------------------------------------------------------
  int dump(const std::string &path, bool relative, bool rawPaths, bool noDirs, std::ostream &out);

  //----------------------------------------------------------------------------
  //! Scan all directories in the namespace, and print out some information
  //! about each one. (even potentially unreachable directories)
  //----------------------------------------------------------------------------
  int scanDirs(bool onlyNoAttrs, bool fullPaths, std::ostream &out, std::ostream &err);

  //----------------------------------------------------------------------------
  //! Scan all file metadata in the namespace, and print out some information
  //! about each one. (even potentially unreachable directories)
  //----------------------------------------------------------------------------
  int scanFileMetadata(std::ostream &out, std::ostream &err);

  //----------------------------------------------------------------------------
  //! Forcefully overwrite the given ContainerMD - USE WITH CAUTION
  //----------------------------------------------------------------------------
  int overwriteContainerMD(uint64_t id, uint64_t parentId, const std::string &name, std::ostream &out, std::ostream &err);

  //----------------------------------------------------------------------------
  //! Check intra-container conflicts, such as a container having two entries
  //! with the name name.
  //----------------------------------------------------------------------------
  int checkNamingConflicts(std::ostream &out, std::ostream &err);

  //----------------------------------------------------------------------------
  //! Search for files / containers with cursed names
  //----------------------------------------------------------------------------
  int checkCursedNames(std::ostream &out, std::ostream &err);

  //----------------------------------------------------------------------------
  //! Search for holes in FsView: Items which should be in FsView according to
  //! FMD locations / unlinked locations, but are not there.
  //----------------------------------------------------------------------------
  int checkFsViewMissing(std::ostream &out, std::ostream &err);

  //----------------------------------------------------------------------------
  //! Search for elements which are present in FsView, but not FMD locations
  //----------------------------------------------------------------------------
  int checkFsViewExtra(std::ostream &out, std::ostream &err);

  //----------------------------------------------------------------------------
  //! Search for shadow directories
  //----------------------------------------------------------------------------
  int checkShadowDirectories(std::ostream &out, std::ostream &err);

  //----------------------------------------------------------------------------
  //! Check for corrupted ...eos.ino... hardlink-simulation files
  //----------------------------------------------------------------------------
  int checkSimulatedHardlinks(std::ostream &out, std::ostream &err);

  //----------------------------------------------------------------------------
  // Find files with layout = 1 replica
  //----------------------------------------------------------------------------
  int oneReplicaLayout(std::ostream &out, std::ostream &err);

  //----------------------------------------------------------------------------
  //! Find files with non-nominal number of stripes (replicas)
  //----------------------------------------------------------------------------
  int stripediff(bool printTime, std::ostream &out, std::ostream &err);

  //----------------------------------------------------------------------------
  //! Find orphan files and orphan directories
  //----------------------------------------------------------------------------
  int checkOrphans(std::ostream &out, std::ostream &err);

  //----------------------------------------------------------------------------
  //! Change the given fid - USE WITH CAUTION
  //----------------------------------------------------------------------------
  int changeFid(uint64_t id, uint64_t newParent, const std::string &newChecksum, int64_t newSize, std::ostream &out, std::ostream &err);

  //----------------------------------------------------------------------------
  //! Rename the given fid fully, taking care of the container maps as well
  //----------------------------------------------------------------------------
  int renameFid(uint64_t id, uint64_t newParent, const std::string& newName, std::ostream &out, std::ostream &err);

  //----------------------------------------------------------------------------
  //! Print out _everything_ known about the given file.
  //----------------------------------------------------------------------------
  int printFileMD(uint64_t fid, std::ostream &out, std::ostream &err);

  //----------------------------------------------------------------------------
  //! Print out _everything_ known about the given directory.
  //----------------------------------------------------------------------------
  int printContainerMD(uint64_t cid, std::ostream& out, std::ostream& err);


private:
  qclient::QClient &mQcl;

};

EOSNSNAMESPACE_END
