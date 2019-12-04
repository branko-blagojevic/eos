// <shared_mutex> -*- C++ -*-

// Copyright (C) 2013-2016 Free Software Foundation, Inc.
//
// This file is part of the GNU ISO C++ Library.  This library is free
// software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the
// Free Software Foundation; either version 3, or (at your option)
// any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// Under Section 7 of GPL version 3, you are granted additional
// permissions described in the GCC Runtime Library Exception, version
// 3.1, as published by the Free Software Foundation.

// You should have received a copy of the GNU General Public License and
// a copy of the GCC Runtime Library Exception along with this program;
// see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
// <http://www.gnu.org/licenses/>.

/** @file include/shared_mutex
 *  This is a Standard C++ Library header.
 */

#ifndef EOS_GLIBCXX_SHARED_MUTEX
#define EOS_GLIBCXX_SHARED_MUTEX 1

#include "common/Namespace.hh"
#include <mutex>
#include <condition_variable>
#include <chrono>

EOSCOMMONNAMESPACE_BEGIN

/// A shared mutex type implemented using std::condition_variable.
class __shared_mutex_cv
{
  friend class shared_timed_mutex;

  // Based on Howard Hinnant's reference implementation from N2406.

  // The high bit of _M_state is the write-entered flag which is set to
  // indicate a writer has taken the lock or is queuing to take the lock.
  // The remaining bits are the count of reader locks.
  //
  // To take a reader lock, block on gate1 while the write-entered flag is
  // set or the maximum number of reader locks is held, then increment the
  // reader lock count.
  // To release, decrement the count, then if the write-entered flag is set
  // and the count is zero then signal gate2 to wake a queued writer,
  // otherwise if the maximum number of reader locks was held signal gate1
  // to wake a reader.
  //
  // To take a writer lock, block on gate1 while the write-entered flag is
  // set, then set the write-entered flag to start queueing, then block on
  // gate2 while the number of reader locks is non-zero.
  // To release, unset the write-entered flag and signal gate1 to wake all
  // blocked readers and writers.
  //
  // This means that when no reader locks are held readers and writers get
  // equal priority. When one or more reader locks is held a writer gets
  // priority and no more reader locks can be taken while the writer is
  // queued.

  // Only locked when accessing _M_state or waiting on condition variables.
  std::mutex            _M_mut;
  // Used to block while write-entered is set or reader count at maximum.
  std::condition_variable _M_gate1;
  // Used to block queued writers while reader count is non-zero.
  std::condition_variable _M_gate2;
  // The write-entered flag and reader count.
  unsigned    _M_state;

  static constexpr unsigned _S_write_entered
    = 1U << (sizeof(unsigned) * __CHAR_BIT__ - 1);
  static constexpr unsigned _S_max_readers = ~_S_write_entered;

  // Test whether the write-entered flag is set. _M_mut must be locked.
  bool _M_write_entered() const
  {
    return _M_state & _S_write_entered;
  }

  // The number of reader locks currently held. _M_mut must be locked.
  unsigned _M_readers() const
  {
    return _M_state & _S_max_readers;
  }

public:
  __shared_mutex_cv() : _M_state(0) {}

  ~__shared_mutex_cv() {}

  __shared_mutex_cv(const __shared_mutex_cv&) = delete;
  __shared_mutex_cv& operator=(const __shared_mutex_cv&) = delete;

  // Exclusive ownership
  void
  lock()
  {
    std::unique_lock<std::mutex> __lk(_M_mut);
    // Wait until we can set the write-entered flag.
    _M_gate1.wait(__lk, [ = ] { return !_M_write_entered(); });
    _M_state |= _S_write_entered;
    // Then wait until there are no more readers.
    _M_gate2.wait(__lk, [ = ] { return _M_readers() == 0; });
  }

  bool
  try_lock()
  {
    std::unique_lock<std::mutex> __lk(_M_mut, std::try_to_lock);

    if (__lk.owns_lock() && _M_state == 0) {
      _M_state = _S_write_entered;
      return true;
    }

    return false;
  }

  void
  unlock()
  {
    std::lock_guard<std::mutex> __lk(_M_mut);
    _M_state = 0;
    // call notify_all() while mutex is held so that another thread can't
    // lock and unlock the mutex then destroy *this before we make the call.
    _M_gate1.notify_all();
  }

  // Shared ownership
  void
  lock_shared()
  {
    std::unique_lock<std::mutex> __lk(_M_mut);
    _M_gate1.wait(__lk, [ = ] { return _M_state < _S_max_readers; });
    ++_M_state;
  }

  bool
  try_lock_shared()
  {
    std::unique_lock<std::mutex> __lk(_M_mut, std::try_to_lock);

    if (!__lk.owns_lock()) {
      return false;
    }

    if (_M_state < _S_max_readers) {
      ++_M_state;
      return true;
    }

    return false;
  }

  void
  unlock_shared()
  {
    std::lock_guard<std::mutex> __lk(_M_mut);
    auto __prev = _M_state--;

    if (_M_write_entered()) {
      // Wake the queued writer if there are no more readers.
      if (_M_readers() == 0) {
        _M_gate2.notify_one();
      }

      // No need to notify gate1 because we give priority to the queued
      // writer, and that writer will eventually notify gate1 after it
      // clears the write-entered flag.
    } else {
      // Wake any thread that was blocked on reader overflow.
      if (__prev == _S_max_readers) {
        _M_gate1.notify_one();
      }
    }
  }
};

/// The standard shared mutex type.
class shared_mutex
{
public:
  shared_mutex() = default;
  ~shared_mutex() = default;

  shared_mutex(const shared_mutex&) = delete;
  shared_mutex& operator=(const shared_mutex&) = delete;

  // Exclusive ownership

  void lock()
  {
    _M_impl.lock();
  }
  bool try_lock()
  {
    return _M_impl.try_lock();
  }
  void unlock()
  {
    _M_impl.unlock();
  }

  // Shared ownership

  void lock_shared()
  {
    _M_impl.lock_shared();
  }
  bool try_lock_shared()
  {
    return _M_impl.try_lock_shared();
  }
  void unlock_shared()
  {
    _M_impl.unlock_shared();
  }

private:
  __shared_mutex_cv _M_impl;
};

using __shared_timed_mutex_base = __shared_mutex_cv;

/// The standard shared timed mutex type.
class shared_timed_mutex
  : private __shared_timed_mutex_base
{
  using _Base = __shared_timed_mutex_base;

  // Must use the same clock as condition_variable for __shared_mutex_cv.
  typedef std::chrono::system_clock __clock_t;

public:
  shared_timed_mutex() = default;
  ~shared_timed_mutex() = default;

  shared_timed_mutex(const shared_timed_mutex&) = delete;
  shared_timed_mutex& operator=(const shared_timed_mutex&) = delete;

  // Exclusive ownership

  void lock()
  {
    _Base::lock();
  }
  bool try_lock()
  {
    return _Base::try_lock();
  }
  void unlock()
  {
    _Base::unlock();
  }

  template<typename _Rep, typename _Period>
  bool
  try_lock_for(const std::chrono::duration<_Rep, _Period>& __rel_time)
  {
    return try_lock_until(__clock_t::now() + __rel_time);
  }

  // Shared ownership

  void lock_shared()
  {
    _Base::lock_shared();
  }
  bool try_lock_shared()
  {
    return _Base::try_lock_shared();
  }
  void unlock_shared()
  {
    _Base::unlock_shared();
  }

  template<typename _Rep, typename _Period>
  bool
  try_lock_shared_for(const std::chrono::duration<_Rep, _Period>& __rel_time)
  {
    return try_lock_shared_until(__clock_t::now() + __rel_time);
  }


  // Exclusive ownership

  template<typename _Clock, typename _Duration>
  bool
  try_lock_until(const std::chrono::time_point<_Clock, _Duration>& __abs_time)
  {
    std::unique_lock<std::mutex> __lk(_M_mut);

    if (!_M_gate1.wait_until(__lk, __abs_time,
                             [ = ] { return !_M_write_entered(); })) {
      return false;
    }
    _M_state |= _S_write_entered;

    if (!_M_gate2.wait_until(__lk, __abs_time,
                             [ = ] { return _M_readers() == 0; })) {
      _M_state ^= _S_write_entered;
      // Wake all threads blocked while the write-entered flag was set.
      _M_gate1.notify_all();
      return false;
    }
    return true;
  }

  // Shared ownership

  template <typename _Clock, typename _Duration>
  bool
  try_lock_shared_until(const std::chrono::time_point<_Clock,
                        _Duration>& __abs_time)
  {
    std::unique_lock<std::mutex> __lk(_M_mut);

    if (!_M_gate1.wait_until(__lk, __abs_time,
                             [ = ] { return _M_state < _S_max_readers; })) {
      return false;
    }
    ++_M_state;
    return true;
  }
};

EOSCOMMONNAMESPACE_END

#endif // EOS_GLIBCXX_SHARED_MUTEX
