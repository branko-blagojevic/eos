// ----------------------------------------------------------------------
// File: TapeGc.cc
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

#include "mgm/tgc/Constants.hh"
#include "mgm/tgc/DummyTapeGcMgm.hh"

EOSTGCNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
DummyTapeGcMgm::DummyTapeGcMgm():
m_nbCallsToGetTapeGcSpaceConfig(0),
m_nbCallsToFileInNamespaceAndNotScheduledForDeletion(0),
m_nbCallsToGetFileSizeBytes(0),
m_nbCallsToStagerrmAsRoot(0)
{
}

//----------------------------------------------------------------------------
//! @return The configuration of a tape-aware garbage collector for the
//! specified space.
//! @param spaceName The name of the space
//----------------------------------------------------------------------------
TapeGcSpaceConfig
DummyTapeGcMgm::getTapeGcSpaceConfig(const std::string &spaceName) {
  const TapeGcSpaceConfig defaultConfig;

  try {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_nbCallsToGetTapeGcSpaceConfig++;

    auto itor = m_spaceToTapeGcConfig.find(spaceName);
    if(itor == m_spaceToTapeGcConfig.end()) {
      return defaultConfig;
    } else {
      return itor->second;
    }
  } catch(...) {
    return defaultConfig;
  }
}

//----------------------------------------------------------------------------
// Determine if the specified file exists and is not scheduled for deletion
//----------------------------------------------------------------------------
bool
DummyTapeGcMgm::fileInNamespaceAndNotScheduledForDeletion(const IFileMD::id_t /* fid */)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_nbCallsToFileInNamespaceAndNotScheduledForDeletion++;
  return true;
}

//----------------------------------------------------------------------------
// Return numbers of free and used bytes within the specified space
//----------------------------------------------------------------------------
ITapeGcMgm::FreeAndUsedBytes
DummyTapeGcMgm::getSpaceFreeAndUsedBytes(const std::string &space) {
  return FreeAndUsedBytes();
}

//----------------------------------------------------------------------------
// Return size of the specified file
//----------------------------------------------------------------------------
uint64_t
DummyTapeGcMgm::getFileSizeBytes(const IFileMD::id_t /* fid */)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_nbCallsToGetFileSizeBytes++;
  return 1;
}

//----------------------------------------------------------------------------
// Execute stagerrm as user root
//----------------------------------------------------------------------------
void
DummyTapeGcMgm::stagerrmAsRoot(const IFileMD::id_t /* fid */)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_nbCallsToStagerrmAsRoot++;
}

//----------------------------------------------------------------------------
// Set the configuration of the tape-aware garbage collector
//----------------------------------------------------------------------------
void
DummyTapeGcMgm::setTapeGcSpaceConfig(const std::string &space,
  const TapeGcSpaceConfig &config) {
  std::lock_guard<std::mutex> lock(m_mutex);

  m_spaceToTapeGcConfig[space] = config;
}

//------------------------------------------------------------------------------
// Return number of times getTapeGcSpaceConfig() has been called
//------------------------------------------------------------------------------
uint64_t
DummyTapeGcMgm::getNbCallsToGetTapeGcSpaceConfig() const {
  std::lock_guard<std::mutex> lock(m_mutex);

  return m_nbCallsToGetTapeGcSpaceConfig;
}

//------------------------------------------------------------------------------
// Return number of times fileInNamespaceAndNotScheduledForDeletion() has been
// called
//------------------------------------------------------------------------------
uint64_t
DummyTapeGcMgm::getNbCallsToFileInNamespaceAndNotScheduledForDeletion() const {
  std::lock_guard<std::mutex> lock(m_mutex);

  return m_nbCallsToFileInNamespaceAndNotScheduledForDeletion;
}

//------------------------------------------------------------------------------
// Return number of times getFileSizeBytes() has been called
//------------------------------------------------------------------------------
uint64_t
DummyTapeGcMgm::getNbCallsToGetFileSizeBytes() const {
  std::lock_guard<std::mutex> lock(m_mutex);

  return m_nbCallsToGetFileSizeBytes;
}

//------------------------------------------------------------------------------
// Return number of times stagerrmAsRoot() has been called
//------------------------------------------------------------------------------
uint64_t
DummyTapeGcMgm::getNbCallsToStagerrmAsRoot() const {
  std::lock_guard<std::mutex> lock(m_mutex);

  return m_nbCallsToStagerrmAsRoot;
}

EOSTGCNAMESPACE_END
