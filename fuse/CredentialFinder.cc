//------------------------------------------------------------------------------
// File: CredentialFinder.cc
// Author: Georgios Bitzes - CERN
//------------------------------------------------------------------------------

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

#include <iostream>
#include <sstream>
#include "CredentialFinder.hh"
#include "Utils.hh"

void Environment::fromFile(const std::string &path) {
  std::ostringstream ss;

  const int BUFFER_SIZE = 1024;
  char buffer[BUFFER_SIZE];

  FILE *in = fopen(path.c_str(), "rb");
  if(!in) {
    return;
  }

  while(true) {
    size_t bytesRead = fread(buffer, 1, BUFFER_SIZE, in);

    if(bytesRead > 0) {
      ss.write(buffer, bytesRead);
    }

    if(bytesRead != BUFFER_SIZE) {
      break;
    }
  }

  fclose(in);

  fromString(ss.str());
}

void Environment::fromString(const std::string &str) {
  std::string nullbyte("\0", 1);
  contents = split(str, nullbyte);

  if(contents[contents.size()-1].size() == 0) {
    contents.pop_back();
  }
}

void Environment::fromVector(const std::vector<std::string> &vec) {
  contents = vec;
}

std::string Environment::get(const std::string &key) const {
  std::string keyWithEquals = key + "=";

  for(size_t i = 0; i < contents.size(); i++) {
    if(startswith(contents[i], keyWithEquals)) {
      return contents[i].substr(keyWithEquals.size());
    }
  }

  return "";
}

std::vector<std::string> Environment::getAll() const {
  return contents;
}

std::string CredentialFinder::locateKerberosTicket(const Environment &env) {
  std::string krb5ccname = env.get("KRB5CCNAME");
  const std::string prefix = "FILE:";

  if(startswith(krb5ccname, prefix)) {
    krb5ccname = krb5ccname.substr(prefix.size());
  }

  return krb5ccname;
}

std::string CredentialFinder::locateX509Proxy(const Environment &env, uid_t uid) {
  std::string proxyPath = env.get("X509_USER_PROXY");

  if(proxyPath.empty()) {
    proxyPath = SSTR("/tmp/x509up_u" << uid);
  }

  return proxyPath;
}
