/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2018 CERN/Switzerland                                  *
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
//! @brief Class for exploring the namespace
//------------------------------------------------------------------------------

#pragma once

#include "common/FutureWrapper.hh"
#include "namespace/Namespace.hh"
#include "proto/FileMd.pb.h"
#include "proto/ContainerMd.pb.h"
#include "namespace/interface/IContainerMD.hh"
#include "namespace/interface/Identifiers.hh"
#include <string>
#include <vector>
#include <deque>
#include <folly/futures/Future.h>

namespace qclient
{
class QClient;
}

EOSNSNAMESPACE_BEGIN

class ExpansionDecider {
public:
  //----------------------------------------------------------------------------
  //! Returns whether to expand the given container, or ignore it.
  //! Useful to filter out certain parts of the namespace tree.
  //----------------------------------------------------------------------------
  virtual bool shouldExpandContainer(const eos::ns::ContainerMdProto &containerMd) = 0;
};

struct ExplorationOptions {
  int depthLimit;
  std::shared_ptr<ExpansionDecider> expansionDecider;
};

struct NamespaceItem {
  // A simple string for now, we can extend this later.
  std::string fullPath;

  bool isFile;
  // Only one of these are actually filled out.
  eos::ns::FileMdProto fileMd;
  eos::ns::ContainerMdProto containerMd;
};

class NamespaceExplorer;

//------------------------------------------------------------------------------
//! Represents a node in the search tree.
//------------------------------------------------------------------------------
class SearchNode
{
public:
  SearchNode(NamespaceExplorer &explorer, ContainerIdentifier id, SearchNode* prnt);
  inline ContainerIdentifier getID() const
  {
    return id;
  }

  // Return false if this node has no more files to output
  bool fetchChild(eos::ns::FileMdProto& output); // sync, block if not available

  // Handle asynchronous operations - call this as often as possible!
  void handleAsync();

  // Explicit transfer of ownership
  std::unique_ptr<SearchNode> expand();

  // Activate
  void activate();

  // Activate one specific contained child.
  void activateOne(const std::string& name);

  // Clear children.
  void prefetchChildren();

  inline bool isVisited()
  {
    return visited;
  }
  inline void visit()
  {
    visited = true;
  }

  eos::ns::ContainerMdProto& getContainerInfo();

private:
  NamespaceExplorer &explorer;
  ContainerIdentifier id;
  qclient::QClient& qcl;
  SearchNode* parent = nullptr;
  bool visited = false;

  // First round of asynchronous requests fills out:
  common::FutureWrapper<eos::ns::ContainerMdProto> containerMd;
  common::FutureWrapper<IContainerMD::FileMap> fileMap;
  common::FutureWrapper<IContainerMD::ContainerMap> containerMap;

  // Second and final round fills out:
  std::deque<folly::Future<eos::ns::FileMdProto>> pendingFileMds;
  bool pendingFileMdsLoaded = false;

  std::deque<std::unique_ptr<SearchNode>> children; // expanded containers
  bool childrenLoaded = false;

  // @todo (gbitzes): Replace this mess with a nice iterator object which
  // provides all children of a container, fully asynchronous with prefetching.
  void stageFileMds();
  void stageChildren();
};

//------------------------------------------------------------------------------
//! Class to recursively explore the QuarkDB namespace, starting from some path.
//! Useful for "Find" commands - no consistency guarantees, if a write is in
//! the flusher, it might not be seen here.
//!
//! Implemented by simple DFS on the namespace.
//------------------------------------------------------------------------------
class NamespaceExplorer
{
public:
  //----------------------------------------------------------------------------
  //! Inject the QClient to use directly in the constructor. No ownership of
  //! underlying object.
  //----------------------------------------------------------------------------
  NamespaceExplorer(const std::string& path, const ExplorationOptions& options,
                    qclient::QClient& qcl);

  //----------------------------------------------------------------------------
  //! Fetch next item.
  //----------------------------------------------------------------------------
  bool fetch(NamespaceItem& result);

private:
  friend class SearchNode;
  std::string buildStaticPath();
  std::string buildDfsPath();

  std::string path;
  ExplorationOptions options;
  qclient::QClient& qcl;

  std::vector<eos::ns::ContainerMdProto> staticPath;
  eos::ns::FileMdProto lastChunk;
  bool searchOnFile = false;
  bool searchOnFileEnded = false;

  std::vector<std::unique_ptr<SearchNode>> dfsPath;
};

EOSNSNAMESPACE_END
