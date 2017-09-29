// ----------------------------------------------------------------------
// File: BoundIdentityProvider.hh
// Author: Georgios Bitzes - CERN
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

#ifndef __BOUND_IDENTITY_PROVIDER__HH__
#define __BOUND_IDENTITY_PROVIDER__HH__

#include <atomic>
#include "CredentialCache.hh"
#include "CredentialFinder.hh"
#include "ProcessInfo.hh"
#include "EnvironmentReader.hh"

class BoundIdentityProvider {
public:
  static bool fillCredsFromEnv(const Environment &env, const CredentialConfig &credConfig, CredInfo &creds, uid_t uid);
  std::shared_ptr<const BoundIdentity> retrieve(pid_t pid, uid_t uid, gid_t gid, bool reconnect);

  void setCredentialConfig(const CredentialConfig &conf) {
    credConfig = conf;

    // For some reason, doing this in the constructor results in crazy behavior,
    // like threads not waking up from the condition variable.
    // (or even std::this_thread::sleep_for !! ) Investigate?
    environmentReader.launchWorkers(20);
  }

private:
  CredentialConfig credConfig;
  CredentialCache credentialCache;

  EnvironmentReader environmentReader;

  static bool fillKrb5FromEnv(const Environment &env, CredInfo &creds, uid_t uid);
  static bool fillX509FromEnv(const Environment &env, CredInfo &creds, uid_t uid);
  static bool checkCredsPath(const std::string &path, uid_t uid);

  std::atomic<uint64_t> connectionCounter {1};
};

#endif
