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

#include "namespace/ns_quarkdb/explorer/NamespaceExplorer.hh"
#include "namespace/utils/PathProcessor.hh"
#include "namespace/ns_quarkdb/persistency/MetadataFetcher.hh"
#include "common/Assert.hh"
#include <memory>
#include <numeric>

#define DBG(message) std::cerr << __FILE__ << ":" << __LINE__ << " -- " << #message << " = " << message << std::endl

EOSNSNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
SearchNode::SearchNode(NamespaceExplorer &expl, ContainerIdentifier d, eos::SearchNode* prnt)
  : explorer(expl), id(d), qcl(explorer.qcl), parent(prnt),
    containerMd(MetadataFetcher::getContainerFromId(qcl, ContainerIdentifier(id)))
{
  fileMap = MetadataFetcher::getFilesInContainer(qcl, ContainerIdentifier(id));
  containerMap = MetadataFetcher::getSubContainers(qcl, ContainerIdentifier(id));
}

//------------------------------------------------------------------------------
// Send off more requests if results are ready, otherwise do nothing.
// If search needs some result, it'll block.
//------------------------------------------------------------------------------
void SearchNode::handleAsync()
{
  if (!pendingFileMdsLoaded && fileMap.ready()) {
    stageFileMds();
  }

  if (!childrenLoaded && containerMap.ready()) {
    stageChildren();
  }
}

//------------------------------------------------------------------------------
// Unconditionally stage file mds, block if necessary. Call this only if:
// - Search really needs the result.
// - When prefetching, when you know fileMap is ready.
//------------------------------------------------------------------------------
void SearchNode::stageFileMds()
{
  if (pendingFileMdsLoaded) {
    return;
  }

  pendingFileMdsLoaded = true;
  // fileMap is hashmap, thus unsorted... must sort first by filename.. sigh.
  // storing into a vector and calling std::sort might be faster, TODO
  std::map<std::string, IFileMD::id_t> sortedFileMap;

  for (auto it = fileMap->begin(); it != fileMap->end(); it++) {
    sortedFileMap[it->first] = it->second;
  }

  for (auto it = sortedFileMap.begin(); it != sortedFileMap.end(); it++) {
    pendingFileMds.push_back(MetadataFetcher::getFileFromId(qcl, FileIdentifier(it->second)));
  }
}

//------------------------------------------------------------------------------
// Get more subcontainers if available
//------------------------------------------------------------------------------
std::unique_ptr<SearchNode> SearchNode::expand()
{
  ExpansionDecider *decider = explorer.options.expansionDecider.get();
  if(decider && !decider->shouldExpandContainer(getContainerInfo())) {
    return {}; // nope, this node is being filtered out
  }

  stageChildren();

  if (children.empty()) {
    return {}; // nullptr, node has no more children to expand
  }

  // Explicit transfer of ownership
  std::unique_ptr<SearchNode> retval = std::move(children.front());
  children.pop_front();
  return retval;
}

//------------------------------------------------------------------------------
// @todo (gbitzes): Remove this eventually, once we are confident the two find
// implementations match, apart for the order.
//------------------------------------------------------------------------------
struct FilesystemEntryComparator {
  bool operator()(const std::string& lhs, const std::string& rhs) const
  {
    for (size_t i = 0; i < std::min(lhs.size(), rhs.size()); i++) {
      if (lhs[i] != rhs[i]) {
        return lhs[i] < rhs[i];
      }
    }

    return lhs.size() > rhs.size();
  }
};

//------------------------------------------------------------------------------
// Unconditionally stage container mds, block if necessary. Call this only if:
// - Search really needs the result.
// - When prefetching, when you know containerMap is ready.
//------------------------------------------------------------------------------
void SearchNode::stageChildren()
{
  if (childrenLoaded) {
    return;
  }

  childrenLoaded = true;
  // containerMap is hashmap, thus unsorted... must sort first by filename.. sigh.
  // storing into a vector and calling std::sort might be faster, TODO
  std::map<std::string, IContainerMD::id_t, FilesystemEntryComparator> sortedContainerMap;

  for (auto it = containerMap->begin(); it != containerMap->end(); it++) {
    sortedContainerMap[it->first] = it->second;
  }

  for (auto it = sortedContainerMap.begin(); it != sortedContainerMap.end();
       it++) {
    children.emplace_back(new SearchNode(explorer, ContainerIdentifier(it->second), this));
  }
}

//------------------------------------------------------------------------------
// Fetch a file entry
//------------------------------------------------------------------------------
bool SearchNode::fetchChild(eos::ns::FileMdProto& output)
{
  stageFileMds();

  if (pendingFileMds.empty()) {
    return false;
  }

  output = pendingFileMds[0].get();
  pendingFileMds.pop_front();
  return true;
}

//------------------------------------------------------------------------------
// Get container md proto info
//------------------------------------------------------------------------------
eos::ns::ContainerMdProto& SearchNode::getContainerInfo()
{
  return containerMd.get();
}

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
NamespaceExplorer::NamespaceExplorer(const std::string& pth,
                                     const ExplorationOptions& opts,
                                     qclient::QClient& qclient)
  : path(pth), options(opts), qcl(qclient)
{
  std::vector<std::string> pathParts;
  eos::PathProcessor::splitPath(pathParts, path);
  // This part is synchronous by necessity.
  staticPath.emplace_back(MetadataFetcher::getContainerFromId(qcl, ContainerIdentifier(1)).get());

  if (pathParts.empty()) {
    // We're running a search on the root node, expand.
    dfsPath.emplace_back(new SearchNode(*this, ContainerIdentifier(1), nullptr));
  }

  // TODO: This for loop looks like a useful primitive for MetadataFetcher,
  // maybe move there?
  for (size_t i = 0; i < pathParts.size(); i++) {
    // We don't know if the last chunk of pathParts is supposed to be a container
    // or name..
    ContainerIdentifier parentID = ContainerIdentifier(staticPath.back().id());
    bool threw = false;
    ContainerIdentifier nextId;

    try {
      nextId = MetadataFetcher::getContainerIDFromName(qcl, parentID,
               pathParts[i]).get();
    } catch (const MDException& exc) {
      threw = true;
      // Maybe the user called "Find" on a single file, and the last chunk is
      // actually a file. Weird, but possible.

      if (i != pathParts.size() - 1) {
        // Nope, not last part.
        throw;
      }

      if (exc.getErrno() != ENOENT) {
        // Nope, different kind of error
        throw;
      }

      if (exc.getErrno() == ENOENT) {
        // This may throw again, propagate to caller if so
        FileIdentifier nextId = MetadataFetcher::getFileIDFromName(qcl, parentID,
                      pathParts[i]).get();
        lastChunk = MetadataFetcher::getFileFromId(qcl, nextId).get();
        searchOnFile = true;
      }
    }

    if (!threw) {
      if (i != pathParts.size() - 1) {
        staticPath.emplace_back(MetadataFetcher::getContainerFromId(qcl, nextId).get());
      } else {
        // Final node, expand
        dfsPath.emplace_back(new SearchNode(*this, nextId, nullptr));
      }
    }
  }
}

//------------------------------------------------------------------------------
// Build static path
//------------------------------------------------------------------------------
std::string NamespaceExplorer::buildStaticPath()
{
  if (staticPath.size() == 1) {
    return "/";
  }

  // TODO: Cache this?
  std::stringstream ss;

  for (size_t i = 0; i < staticPath.size(); i++) {
    if (i == 0) {
      // Root node
      ss << "/";
    } else {
      ss << staticPath[i].name() << "/";
    }
  }

  return ss.str();
}

//------------------------------------------------------------------------------
// Build depth-first-search path
//------------------------------------------------------------------------------
std::string NamespaceExplorer::buildDfsPath()
{
  // TODO: cache this somehow?
  std::stringstream ss;
  ss << buildStaticPath();

  for (size_t i = 0; i < dfsPath.size(); i++) {
    if (dfsPath[i]->getContainerInfo().id() == 1) {
      continue;
    } else {
      ss << dfsPath[i]->getContainerInfo().name() << "/";
    }
  }

  return ss.str();
}

//------------------------------------------------------------------------------
// Fetch children under current path
//------------------------------------------------------------------------------
bool NamespaceExplorer::fetch(NamespaceItem& item)
{
  // Handle weird case: Search was called on a single file
  if (searchOnFile) {
    if (searchOnFileEnded) {
      return false;
    }

    item.fullPath = buildStaticPath() + lastChunk.name();
    item.isFile = true;
    item.fileMd = lastChunk;
    searchOnFileEnded = true;
    return true;
  }

  while (!dfsPath.empty()) {
    dfsPath.back()->handleAsync();

    // Has top node been visited yet?
    if (!dfsPath.back()->isVisited()) {
      dfsPath.back()->visit();
      item.isFile = false;
      item.fullPath = buildDfsPath();
      item.containerMd = dfsPath.back()->getContainerInfo();
      return true;
    }

    // Does the top node have any pending file children?
    if (dfsPath.back()->fetchChild(item.fileMd)) {
      item.isFile = true;
      item.fullPath = buildDfsPath() + item.fileMd.name();
      return true;
    }

    // Can we expand this node?
    std::unique_ptr<SearchNode> child = dfsPath.back()->expand();

    if (child) {
      dfsPath.push_back(std::move(child));
      continue;
    }

    // Node has neither files, nor containers, pop.
    dfsPath.pop_back();
  }

  // Search is over.
  return false;
}

EOSNSNAMESPACE_END
