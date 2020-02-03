// ----------------------------------------------------------------------
// File: TapeGc.hh
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

#ifndef __EOSMGM_TAPEGC_HH__
#define __EOSMGM_TAPEGC_HH__

#include "common/Logging.hh"
#include "mgm/Namespace.hh"
#include "mgm/tgc/BlockingFlag.hh"
#include "mgm/tgc/CachedValue.hh"
#include "mgm/tgc/Constants.hh"
#include "mgm/tgc/ITapeGcMgm.hh"
#include "mgm/tgc/Lru.hh"
#include "mgm/tgc/TapeGcStats.hh"
#include "namespace/interface/IFileMD.hh"
#include "proto/ConsoleReply.pb.h"
#include "proto/ConsoleRequest.pb.h"

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <stdint.h>
#include <thread>
#include <time.h>

/*----------------------------------------------------------------------------*/
/**
 * @file TapeGc.hh
 *
 * @brief Class implementing a tape aware garbage collector
 *
 */
/*----------------------------------------------------------------------------*/
EOSTGCNAMESPACE_BEGIN

//------------------------------------------------------------------------------
//! A tape aware garbage collector
//------------------------------------------------------------------------------
class TapeGc
{
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //!
  //! @param mgm interface to the EOS MGM
  //! @param space name of the EOS space that this garbage collector will work
  //! on
  //! @param queryPeriodCacheAgeSecs age at which the cached value of
  //! queryPeriodSecs should be renewed
  //! @param minFreeBytesCacheAgeSecs age at which the cached value of
  //! minFreeBytes should be renewed
  //----------------------------------------------------------------------------
  TapeGc(
    ITapeGcMgm &mgm,
    const std::string &space,
    time_t queryPeriodCacheAgeSecs = TGC_DEFAULT_QUERY_PERIOD_CACHED_AGE_SECS,
    time_t minFreeBytesCacheAgeSecs = TGC_DEFAULT_MIN_FREE_BYTES_CACHE_AGE_SECS
  );

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  ~TapeGc();

  //----------------------------------------------------------------------------
  //! Delete copy constructor
  //----------------------------------------------------------------------------
  TapeGc(const TapeGc &) = delete;

  //----------------------------------------------------------------------------
  //! Delete move constructor
  //----------------------------------------------------------------------------
  TapeGc(const TapeGc &&) = delete;

  //----------------------------------------------------------------------------
  //! Delete assignment operator
  //----------------------------------------------------------------------------
  TapeGc &operator=(const TapeGc &) = delete;

  //----------------------------------------------------------------------------
  //! Enable the GC
  //----------------------------------------------------------------------------
  void enable() noexcept;

  //----------------------------------------------------------------------------
  //! Notify GC the specified file has been opened
  //! @note This method does nothing and returns immediately if the GC has not
  //! been enabled
  //!
  //! @param path file path
  //! @param fid file identifier
  //----------------------------------------------------------------------------
  void fileOpened(const std::string &path, const IFileMD::id_t fid) noexcept;

  //----------------------------------------------------------------------------
  //! @return statistics
  //----------------------------------------------------------------------------
  TapeGcStats getStats() const noexcept;

  //----------------------------------------------------------------------------
  //! @return the number of files successfully stagerrm'ed since boot.  Zero is
  //! returned in the case of error.
  //----------------------------------------------------------------------------
  uint64_t getNbStagerrms() const noexcept;

  //----------------------------------------------------------------------------
  //! @return the size of the LRU queue.  Zero is returned in the case of error.
  //----------------------------------------------------------------------------
  Lru::FidQueue::size_type getLruQueueSize() const noexcept;

  //----------------------------------------------------------------------------
  //! @return the amount of free bytes in the EOS space worked on by this
  //! garbage collector.  Zero is in the case of error.
  //----------------------------------------------------------------------------
  uint64_t getFreeBytes() const noexcept;

  //----------------------------------------------------------------------------
  //! @return the timestamp at which the last free space query was made
  //----------------------------------------------------------------------------
  time_t getFreeSpaceQueryTimestamp();

  //----------------------------------------------------------------------------
  //! @return the timestamp at which the EOS space worked on by this garbage
  //! collector was queried for free space.  Zero is returned in the case of
  //! error.
  //----------------------------------------------------------------------------
  time_t getFreeSpaceQueryTimestamp() const noexcept;

protected:

  /// The interface to the EOS MGM
  ITapeGcMgm &m_mgm;

  /// The name of the EOS sapce being worked on by this garbage collector
  std::string m_space;

  /// Used to ensure the enable() method only starts the worker thread once
  std::atomic_flag m_enabledMethodCalled = ATOMIC_FLAG_INIT;

  /// True if the GC has been enabled
  std::atomic<bool> m_enabled;

  /// True if the worker thread should stop
  BlockingFlag m_stop;

  /// The one and only GC worker thread
  std::unique_ptr<std::thread> m_worker;

  /// Mutex protecting mLruQueue
  mutable std::mutex m_lruQueueMutex;

  /// Queue of Least Recently Used (LRU) files
  Lru m_lruQueue;

  //----------------------------------------------------------------------------
  //! Facilitate unit-testing by enabling this garbage collector without
  //! starting the worker thread
  //----------------------------------------------------------------------------
  void enableWithoutStartingWorkerThread();

  //----------------------------------------------------------------------------
  //! Entry point for the GC worker thread
  //----------------------------------------------------------------------------
  void workerThreadEntryPoint() noexcept;

  //----------------------------------------------------------------------------
  //! Try to garbage collect a single file if necessary and possible.
  //!
  //! Please note that a file is considered successfully garbage collected if
  //! it does not exists in the EOS namespace when it is popped from the LRU
  //! data structure.
  //!
  //! @return True if a file was garbage collected
  //----------------------------------------------------------------------------
  bool tryToGarbageCollectASingleFile() noexcept;

  //----------------------------------------------------------------------------
  //! Return the preamble to be placed at the beginning of every log message
  //!
  //! @param space The name of the EOS space
  //! @param path The file path
  //! @param fid The file identifier
  //----------------------------------------------------------------------------
  static std::string createLogPreamble(const std::string &space,
    const std::string &path, const IFileMD::id_t fid);

  /// Thrown when a string is not a valid unsigned 64-bit integer
  struct InvalidUint64: public std::runtime_error {
    InvalidUint64(const std::string &msg): std::runtime_error(msg) {}
  };

  /// Thrown when a string representing a 64-bit integer is out of range
  struct OutOfRangeUint64: public InvalidUint64 {
    OutOfRangeUint64(const std::string &msg): InvalidUint64(msg) {}
  };

  //----------------------------------------------------------------------------
  //! Cached configuration value for the delay in seconds between space queries
  //! to the EOS MGM
  //----------------------------------------------------------------------------
  CachedValue<time_t> m_queryPeriodSecs;

  //----------------------------------------------------------------------------
  //! Cached value for the minimum number of free bytes to be available in the
  //! EOS space worked on by this garnage collector.  If the actual number of
  //! free bytes is less than this value then the garbage collector will try to
  //! free up space by garbage collecting disk replicas.
  //----------------------------------------------------------------------------
  CachedValue<uint64_t> m_minFreeBytes;

  //----------------------------------------------------------------------------
  //! Mutex to protect m_freeSpaceBytes
  //----------------------------------------------------------------------------
  mutable std::mutex m_freeSpaceBytesMutex;

  //----------------------------------------------------------------------------
  //! The number of free bytes in the EOS space worked on by this garbage
  //! collector
  //----------------------------------------------------------------------------
  uint64_t m_freeSpaceBytes;

  /// The timestamp at which the last free space query was made
  std::atomic<time_t> m_freeSpaceQueryTimestamp;

  //----------------------------------------------------------------------------
  //! Counter that is incremented each time a file is successfully stagerrm'ed
  //----------------------------------------------------------------------------
  std::atomic<uint64_t> m_nbStagerrms;

  //----------------------------------------------------------------------------
  //! @return the configured query period and log if changed
  //----------------------------------------------------------------------------
  time_t getQueryPeriodSecsAndLogIfChanged();

  //----------------------------------------------------------------------------
  //! @return the configured min free bytes for the EOS space worked on by this
  //! garbage collector and log if changed
  //----------------------------------------------------------------------------
  uint64_t getMinFreeBytesAndLogIfChanged();

  //----------------------------------------------------------------------------
  //! Take note of a file queued for deletion so that the amount of free space
  //! can be updated without having to wait for the next query to the EOS MGM
  //----------------------------------------------------------------------------
  void fileQueuedForDeletion(const size_t deletedFileSize);
};

EOSTGCNAMESPACE_END

#endif
