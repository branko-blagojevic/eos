/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2016 CERN/Switzerland                                  *
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
//! @author Elvin-Alin Sindrilaru <esindril@cern.ch.
//! @brief QClient singleton
//------------------------------------------------------------------------------

#pragma once
#include "namespace/Namespace.hh"
#include "qclient/QClient.hh"
#include "qclient/QHash.hh"
#include "qclient/QSet.hh"
#include "qclient/Members.hh"
#include "qclient/AsyncHandler.hh"
#include <atomic>
#include <map>
#include <mutex>

EOSNSNAMESPACE_BEGIN

class MetadataFlusherFactory;
class QdbContactDetails;

//------------------------------------------------------------------------------
//! Singleton client class used throughout the namespace implementation
//------------------------------------------------------------------------------
class BackendClient
{
public:
  //----------------------------------------------------------------------------
  //! Initialize
  //----------------------------------------------------------------------------
  static void Initialize() noexcept;

  //----------------------------------------------------------------------------
  //! Finalize
  //----------------------------------------------------------------------------
  static void Finalize();

  //----------------------------------------------------------------------------
  //! Get client for a particular quarkdb instance specified as a list of
  //! cluster members.
  //!
  //! @param qdb_members QuarkDB cluster members
  //! @param tag string used to differentiate between different use-cases
  //!        when using the qclient.
  //!
  //! @note Each unique conbiantino of qdb_members and tag will return always
  //!       the same qclient object
  //!
  //! @return qclient object
  //----------------------------------------------------------------------------
  static qclient::QClient* getInstance(const QdbContactDetails &contactDetails,
                                       const std::string &tag = "default");
private:
  static std::atomic<qclient::QClient*> sQdbClient;
  static std::string sQdbHost; ///< quarkdb instance host
  static int sQdbPort;         ///< quarkddb instance port
  static std::map<std::string, qclient::QClient*> pMapClients;
  static std::mutex pMutexMap; ///< Mutex to protect the access to the map
};

EOSNSNAMESPACE_END
