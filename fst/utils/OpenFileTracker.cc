// ----------------------------------------------------------------------
// File: OpenFileTracker.cc
// Author: Georgios Bitzes - CERN
// ----------------------------------------------------------------------

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

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include "fst/utils/OpenFileTracker.hh"
#include "common/Assert.hh"

EOSFSTNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
OpenFileTracker::OpenFileTracker() {}

//------------------------------------------------------------------------------
// Mark that the given file ID, on the given filesystem ID, was just opened
//------------------------------------------------------------------------------
void OpenFileTracker::up(eos::common::FileSystem::fsid_t fsid, uint64_t fid) {
  std::unique_lock<std::shared_timed_mutex> lock(mMutex);
  mContents[fsid][fid]++;
}

//------------------------------------------------------------------------------
// Mark that the given file ID, on the given filesystem ID, was just closed
//
// Prints warning in the logs if the value was about to go negative - it will
// never go negative.
//------------------------------------------------------------------------------
void OpenFileTracker::down(eos::common::FileSystem::fsid_t fsid, uint64_t fid) {
  std::unique_lock<std::shared_timed_mutex> lock(mMutex);

  auto fsit = mContents.find(fsid);
  if(fsit == mContents.end()) {
    // Can happen if OpenFileTracker is misused
    eos_static_crit("Could not find fsid=%" PRIu64 " when calling OpenFileTracker::down for fid=%" PRIu64, fsid, fid);
    return;
  }

  auto fidit = fsit->second.find(fid);
  if(fidit == fsit->second.end()) {
    // Can happen if OpenFileTracker is misused
    eos_static_crit("Could not find fid=%" PRIu64 " when calling OpenFileTracker::down for fsid=%" PRIu64, fid, fsid);
    return;
  }

  if(fidit->second == 1) {
    // Last use, remove from map
    fsit->second.erase(fidit);

    // Also remove fs from top-level map?
    if(fsit->second.empty()) {
      mContents.erase(fsit);
    }

    return;
  }

  if(fidit->second < 1) {
    eos_static_crit("Should never happen - encountered bogus value in OpenFileTracker::down for fsid=%" PRIu64 ", fid=%" PRIu64 " - dropping", fsid, fid);
    fsit->second.erase(fidit);
    return;
  }

  // Simply decrement
  fidit->second--;
}

//------------------------------------------------------------------------------
// Checks if the given file ID, on the given filesystem ID, is currently open
//------------------------------------------------------------------------------
bool OpenFileTracker::isOpen(eos::common::FileSystem::fsid_t fsid, uint64_t fid) const {
  return getUseCount(fsid, fid) > 0;
}

//----------------------------------------------------------------------------
// Checks if the given file ID, on the given filesystem ID, is currently open
//----------------------------------------------------------------------------
int32_t OpenFileTracker::getUseCount(eos::common::FileSystem::fsid_t fsid, uint64_t fid) const {
  std::shared_lock<std::shared_timed_mutex> lock(mMutex);

  auto fsit = mContents.find(fsid);
  if(fsit == mContents.end()) {
    return 0;
  }

  auto fidit = fsit->second.find(fid);
  if(fidit == fsit->second.end()) {
    return 0;
  }

  return fidit->second;
}

//------------------------------------------------------------------------------
// Checks if there's _any_ operation currently in progress
//------------------------------------------------------------------------------
bool OpenFileTracker::isAnyOpen() const {
  std::shared_lock<std::shared_timed_mutex> lock(mMutex);
  return ! mContents.empty();
}

//----------------------------------------------------------------------------
// Get open file IDs of a filesystem, sorted by usecount
//----------------------------------------------------------------------------
std::map<size_t, std::set<uint64_t>> OpenFileTracker::getSortedByUsecount(
  eos::common::FileSystem::fsid_t fsid) const {

  std::map<size_t, std::set<uint64_t>> contentsSortedByUsecount;
  std::shared_lock<std::shared_timed_mutex> lock(mMutex);

  auto fsit = mContents.find(fsid);
  if(fsit == mContents.end()) {
    // Filesystem has no open files
    return {};
  }

  for(auto it = fsit->second.begin(); it != fsit->second.end(); it++) {
    contentsSortedByUsecount[it->second].insert(it->first);
  }

  return contentsSortedByUsecount;
}

//----------------------------------------------------------------------------
// Get number of distinct open files by filesystem
//----------------------------------------------------------------------------
int32_t OpenFileTracker::getOpenOnFilesystem(eos::common::FileSystem::fsid_t fsid) const {
  std::shared_lock<std::shared_timed_mutex> lock(mMutex);

  auto fsit = mContents.find(fsid);
  if(fsit == mContents.end()) {
    return 0;
  }

  return fsit->second.size();
}

//------------------------------------------------------------------------------
// Get top hot files on current filesystem
//------------------------------------------------------------------------------
std::vector<OpenFileTracker::HotEntry> OpenFileTracker::getHotFiles(
  eos::common::FileSystem::fsid_t fsid, size_t maxEntries) const {

  auto sorted = getSortedByUsecount(fsid);
  std::vector<HotEntry> results;

  for(auto it = sorted.rbegin(); it != sorted.rend(); it++) {
    for(auto it2 =  it->second.begin(); it2 != it->second.end(); it2++) {
      if(results.size() >= maxEntries) {
        goto done;
      }

      results.emplace_back(fsid, *it2, it->first);
    }
  }

done:
  return results;
}

EOSFSTNAMESPACE_END
