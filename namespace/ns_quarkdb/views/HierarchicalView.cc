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

#include "common/Logging.hh"
#include "common/Assert.hh"
#include "namespace/ns_quarkdb/views/HierarchicalView.hh"
#include "namespace/Constants.hh"
#include "namespace/interface/IContainerMDSvc.hh"
#include "namespace/interface/IFileMDSvc.hh"
#include "namespace/ns_quarkdb/persistency/ContainerMDSvc.hh"
#include "namespace/utils/PathProcessor.hh"
#include <cerrno>
#include <ctime>
#include <functional>

#define DBG(message) std::cerr << __FILE__ << ":" << __LINE__ << " -- " << #message << " = " << message << std::endl

using std::placeholders::_1;

#ifdef __APPLE__
#define EBADFD 77
#endif

EOSNSNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
HierarchicalView::HierarchicalView()
  : pContainerSvc(nullptr), pFileSvc(nullptr),
    pQuotaStats(new QuotaStats()), pRoot(nullptr)
{}

//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
HierarchicalView::~HierarchicalView()
{
  delete pQuotaStats;
}

//------------------------------------------------------------------------------
// Configure the view
//------------------------------------------------------------------------------
void
HierarchicalView::configure(const std::map<std::string, std::string>& config)
{
  if (pContainerSvc == nullptr) {
    MDException e(EINVAL);
    e.getMessage() << "Container MD Service was not set";
    throw e;
  }

  if (pFileSvc == nullptr) {
    MDException e(EINVAL);
    e.getMessage() << "File MD Service was not set";
    throw e;
  }

  delete pQuotaStats;
  pQuotaStats = new QuotaStats();
  pQuotaStats->configure(config);
}

//------------------------------------------------------------------------------
// Initialize the view
//------------------------------------------------------------------------------
void
HierarchicalView::initialize()
{
  initialize1();
  initialize2();
  initialize3();
}

void
HierarchicalView::initialize1()
{
  pContainerSvc->initialize();

  // Get root container
  try {
    pRoot = pContainerSvc->getContainerMD(1);
  } catch (MDException& e) {
    pRoot = pContainerSvc->createContainer();

    if (pRoot->getId() != 1) {
      eos_static_crit("Error when creating root '/' path - directory inode is not 1, but %d!",
                      pRoot->getId());
      std::quick_exit(1);
    }

    pRoot->setName("/");
    pRoot->setParentId(pRoot->getId());
    updateContainerStore(pRoot.get());
  }
}

//------------------------------------------------------------------------------
// Initialize phase 2
//------------------------------------------------------------------------------
void
HierarchicalView::initialize2()
{
  pFileSvc->initialize();
}

//------------------------------------------------------------------------------
// Initialize phase 3
//------------------------------------------------------------------------------
void
HierarchicalView::initialize3()
{
  //--------------------------------------------------------------------------
  // Scan all the files to reattach them to containers - THIS SHOULD NOT
  // BE DONE! THE INFO NEEDS TO BE STORED WITH CONTAINERS
  //--------------------------------------------------------------------------
  // FileVisitor visitor( pContainerSvc, pQuotaStats, this );
  // pFileSvc->visit( &visitor );
}

//------------------------------------------------------------------------------
// Finalize the view
//------------------------------------------------------------------------------
void
HierarchicalView::finalize()
{
  pContainerSvc->finalize();
  pFileSvc->finalize();
  delete pQuotaStats;
  pQuotaStats = nullptr;
}

//------------------------------------------------------------------------------
// Extract IFileMDPtr out of PathLookupState
//------------------------------------------------------------------------------
static IFileMDPtr extractFileMDPtr(PathLookupState state) {
  return state.file;
}

//------------------------------------------------------------------------------
// Retrieve a file for given uri, asynchronously
//------------------------------------------------------------------------------
folly::Future<IFileMDPtr>
HierarchicalView::getFileFut(const std::string& uri, bool follow)
{
  return lookupFileURL(uri, 0, follow)
    .then(extractFileMDPtr);
}

//----------------------------------------------------------------------------
//! Lookup file, using the URL.
//----------------------------------------------------------------------------
folly::Future<PathLookupState> HierarchicalView::lookupFileURL(const std::string &uri, size_t symlinkDepth, bool follow)
{
  //----------------------------------------------------------------------------
  //! Short-circuit for "/"
  //----------------------------------------------------------------------------
  if (uri == "/") {
    return folly::makeFuture<PathLookupState>(make_mdexception(ENOENT, "/ is not a file"));
  }

  //----------------------------------------------------------------------------
  //! Split URL into directories and filenames
  //----------------------------------------------------------------------------
  std::vector<std::string> chunks;
  eos::PathProcessor::splitPath(chunks, uri);

  std::string filename = chunks.back();
  chunks.pop_back();

  //----------------------------------------------------------------------------
  // We call getContainer with follow set to always true, since the callers
  // "follow" value pertains only to the last chunk, the filename.
  //----------------------------------------------------------------------------
  return lookupContainer(pRoot, chunks, symlinkDepth, true)
    .then(std::bind(&HierarchicalView::lookupFile, this, _1, filename, follow));
}

//------------------------------------------------------------------------------
//! Lookup file inside a directory.
//------------------------------------------------------------------------------
folly::Future<PathLookupState>
HierarchicalView::lookupFile(PathLookupState parent, std::string name, bool follow)
{
  //----------------------------------------------------------------------------
  // Symlink depth exceeded?
  //----------------------------------------------------------------------------
  if(parent.symlinkDepth > 255) {
      return folly::makeFuture<PathLookupState>(make_mdexception(
        ELOOP, "Too many symbolic links were encountered in translating the pathname"));
  }

  //----------------------------------------------------------------------------
  // Lookup.
  //----------------------------------------------------------------------------
  folly::Future<PathLookupState> fut = parent.current->findFileFut(name)
    .then([this, parent, name, follow](IFileMDPtr result) {

      if(!result) {
        //----------------------------------------------------------------------
        // Nope, not found.
        //----------------------------------------------------------------------
        return folly::makeFuture<PathLookupState>(make_mdexception(ENOENT, "No such file or directory"));
      }

      //------------------------------------------------------------------------
      // Easy case: This is not a symlink, or we're not following symlinks.
      // Just give back result.
      //------------------------------------------------------------------------
      if(!follow || !result->isLink()) {
        PathLookupState newState;
        newState.file = result;
        newState.symlinkDepth = parent.symlinkDepth;
        return folly::makeFuture<PathLookupState>(std::move(newState));
      }

      //------------------------------------------------------------------------
      // We have a symlink to follow.. return a future yet again.
      //------------------------------------------------------------------------
      return lookupFileURL(result->getLink(), parent.symlinkDepth+1, true);
    } );

  return fut;
}

//------------------------------------------------------------------------------
// Retrieve a file for given uri
//------------------------------------------------------------------------------
std::shared_ptr<IFileMD>
HierarchicalView::getFile(const std::string& uri, bool follow,
                          size_t* link_depths)
{
  return getFileFut(uri, follow).get();
}

//------------------------------------------------------------------------------
// Create a file for given uri
//------------------------------------------------------------------------------
std::shared_ptr<IFileMD>
HierarchicalView::createFile(const std::string& uri, uid_t uid, gid_t gid)
{
  // Split the path and find the last container
  char uriBuffer[uri.length() + 1];
  strcpy(static_cast<char*>(uriBuffer), uri.c_str());
  std::vector<char*> elements;
  eos::PathProcessor::splitPath(elements, static_cast<char*>(uriBuffer));
  size_t position;
  std::shared_ptr<IContainerMD> cont =
    findLastContainer(elements, elements.size() - 1, position);

  if (position != elements.size() - 1) {
    MDException e(ENOENT);
    e.getMessage() << "Container does not exist";
    throw e;
  }

  // Check if the file of this name can be inserted
  if (cont->findContainer(elements[position])) {
    MDException e(EEXIST);
    e.getMessage() << "File exist";
    throw e;
  }

  if (cont->findFile(elements[position])) {
    MDException e(EEXIST);
    e.getMessage() << "File exist";
    throw e;
  }

  std::shared_ptr<IFileMD> file{pFileSvc->createFile()};

  if (!file) {
    MDException e(EIO);
    e.getMessage() << "File creation failed";
    throw e;
  }

  file->setName(elements[position]);
  file->setCUid(uid);
  file->setCGid(gid);
  file->setCTimeNow();
  file->setMTimeNow();
  file->clearChecksum(0);
  cont->addFile(file.get());
  updateFileStore(file.get());
  return file;
}

//------------------------------------------------------------------------
//! Create a link for given uri
//------------------------------------------------------------------------
void
HierarchicalView::createLink(const std::string& uri, const std::string& linkuri,
                             uid_t uid, gid_t gid)
{
  std::shared_ptr<IFileMD> file = createFile(uri, uid, gid);

  if (file) {
    file->setLink(linkuri);
    updateFileStore(file.get());
  }
}

//------------------------------------------------------------------------------
// Remove link
//------------------------------------------------------------------------------
void
HierarchicalView::removeLink(const std::string& uri)
{
  return unlinkFile(uri);
}

//------------------------------------------------------------------------------
// Unlink the file for given uri
//------------------------------------------------------------------------------
void
HierarchicalView::unlinkFile(const std::string& uri)
{
  std::vector<std::string> chunks;
  eos::PathProcessor::splitPath(chunks, uri);

  if(chunks.size() == 0) {
    MDException e(ENOENT);
    e.getMessage() << "Not a file";
    throw e;
  }

  std::string lastChunk = chunks[chunks.size()-1];
  chunks.pop_back();

  IContainerMDPtr parent = lookupContainer(pRoot, chunks, 0, true).get().current;
  std::shared_ptr<IFileMD> file = parent->findFile(lastChunk);

  if (!file) {
    MDException e(ENOENT);
    e.getMessage() << "File does not exist";
    throw e;
  }

  unlinkFile(file.get());
}

//------------------------------------------------------------------------------
// Unlink the file - this is only used for testing
//------------------------------------------------------------------------------
void
HierarchicalView::unlinkFile(eos::IFileMD* file)
{
  std::shared_ptr<IContainerMD> cont =
    pContainerSvc->getContainerMD(file->getContainerId());
  file->setContainerId(0);
  file->unlinkAllLocations();
  cont->removeFile(file->getName());
  updateFileStore(file);
}

//------------------------------------------------------------------------------
// Remove the file
//------------------------------------------------------------------------------
void
HierarchicalView::removeFile(IFileMD* file)
{
  // Check if the file can be removed
  if (file->getNumLocation() != 0 || file->getNumUnlinkedLocation() != 0) {
    MDException ex(EBADFD);
    ex.getMessage() << "Cannot remove the record. Unlinked replicas ";
    ex.getMessage() << "still exist";
    throw ex;
  }

  if (file->getContainerId() != 0) {
    std::shared_ptr<IContainerMD> cont =
      pContainerSvc->getContainerMD(file->getContainerId());
    cont->removeFile(file->getName());
  }

  pFileSvc->removeFile(file);
}

//------------------------------------------------------------------------------
// Extract IContainerMDPtr out of PathLookupState
//------------------------------------------------------------------------------
static IContainerMDPtr extractContainerMDPtr(PathLookupState state) {
  return state.current;
}

//------------------------------------------------------------------------------
// Get a container (directory) asynchronously
//------------------------------------------------------------------------------
folly::Future<IContainerMDPtr>
HierarchicalView::getContainerFut(const std::string& uri, bool follow)
{
  if (uri == "/") {
    return std::shared_ptr<IContainerMD> {pContainerSvc->getContainerMD(1)};
  }

  // Follow all symlinks for all containers, except last one if "follow" is set.
  return lookupContainer(pRoot, uri, 0, follow).then(extractContainerMDPtr);
}

//------------------------------------------------------------------------------
// Get a container (directory)
//------------------------------------------------------------------------------
IContainerMDPtr
HierarchicalView::getContainer(const std::string& uri, bool follow,
                               size_t* link_depth)
{
  return getContainerFut(uri, follow).get();
}

//------------------------------------------------------------------------------
// Create container - method eventually consistent
//------------------------------------------------------------------------------
std::shared_ptr<IContainerMD>
HierarchicalView::createContainer(const std::string& uri, bool createParents)
{
  // Split the path
  if (uri == "/") {
    MDException e(EEXIST);
    e.getMessage() << uri << ": Container exist" << std::endl;
    throw e;
  }

  char uriBuffer[uri.length() + 1];
  strcpy(static_cast<char*>(uriBuffer), uri.c_str());
  std::vector<char*> elements;
  eos::PathProcessor::splitPath(elements, static_cast<char*>(uriBuffer));

  if (elements.empty()) {
    MDException e(EEXIST);
    e.getMessage() << uri << ": File exist" << std::endl;
    throw e;
  }

  // Look for the last existing container
  size_t position;
  std::shared_ptr<IContainerMD> lastContainer =
    findLastContainer(elements, elements.size(), position);

  if (position == elements.size()) {
    MDException e(EEXIST);
    e.getMessage() << uri << ": Container exist" << std::endl;
    throw e;
  }

  // One of the parent containers does not exist
  if ((!createParents) && (position < elements.size() - 1)) {
    MDException e(ENOENT);
    e.getMessage() << uri << ": Parent does not exist" << std::endl;
    throw e;
  }

  if (lastContainer->findFile(elements[position])) {
    MDException e(EEXIST);
    e.getMessage() << "File exists" << std::endl;
    throw e;
  }

  // Create the container with all missing parents if required. If a crash
  // happens during the addContainer call and the updateContainerStore then
  // we curate the list of subcontainers in the ContainerMD::findContainer
  // method.
  for (size_t i = position; i < elements.size(); ++i) {
    std::shared_ptr<IContainerMD> newContainer{
      pContainerSvc->createContainer()};
    newContainer->setName(elements[i]);
    newContainer->setCTimeNow();
    lastContainer->addContainer(newContainer.get());
    lastContainer.swap(newContainer);
    updateContainerStore(lastContainer.get());
  }

  return lastContainer;
}

//------------------------------------------------------------------------------
// Remove container
//------------------------------------------------------------------------------
void
HierarchicalView::removeContainer(const std::string& uri)
{
  // Find the container
  if (uri == "/") {
    MDException e(EPERM);
    e.getMessage() << "Permission denied.";
    throw e;
  }

  //----------------------------------------------------------------------------
  // Lookup last container
  //----------------------------------------------------------------------------
  std::vector<std::string> chunks;
  eos::PathProcessor::splitPath(chunks, uri);

  eos_assert(chunks.size() != 0);
  std::string lastChunk = chunks[chunks.size()-1];
  chunks.pop_back();

  IContainerMDPtr parent = pRoot;
  if(chunks.size() != 0) {
    parent = lookupContainer(pRoot, chunks, 0, true).get().current;
  }

  // Check if the container exist and remove it
  auto cont = parent->findContainer(lastChunk);

  if (!cont) {
    MDException e(ENOENT);
    e.getMessage() << uri << ": No such file or directory";
    throw e;
  }

  if (cont->getNumContainers() != 0 || cont->getNumFiles() != 0) {
    MDException e(ENOTEMPTY);
    e.getMessage() << uri << ": Container is not empty";
    throw e;
  }

  // This is a two-step delete
  pContainerSvc->removeContainer(cont.get());
  parent->removeContainer(cont->getName());
}

//------------------------------------------------------------------------------
//! Lookup symlink, expect to find a directory there.
//------------------------------------------------------------------------------
folly::Future<PathLookupState> HierarchicalView::lookupContainerSymlink(IFileMDPtr symlink, IContainerMDPtr parent, size_t symlinkDepth)
{
  if(!symlink || !symlink->isLink()) {
    //--------------------------------------------------------------------------
    //! Nope, not a symlink.
    //--------------------------------------------------------------------------
    return folly::makeFuture<PathLookupState>(make_mdexception(ENOENT, "No such file or directory"));
  }

  //----------------------------------------------------------------------------
  //! Absolute symlink? Start lookup from root.
  //----------------------------------------------------------------------------
  if(symlink->getLink().size() >= 0 && symlink->getLink()[0] == '/') {
    return lookupContainer(pRoot, symlink->getLink(), symlinkDepth, true);
  }

  //----------------------------------------------------------------------------
  //! Relative symlink, root is symlink's parent container
  //----------------------------------------------------------------------------
  return lookupContainer(parent, symlink->getLink(), symlinkDepth, true);
}

//------------------------------------------------------------------------------
//! Lookup a URL, while following symlinks.
//------------------------------------------------------------------------------
folly::Future<PathLookupState> HierarchicalView::lookupContainer(
  IContainerMDPtr root, const std::string &url, size_t symlinkDepth, bool follow)
{
  std::vector<std::string> chunks;
  eos::PathProcessor::splitPath(chunks, url);
  return lookupContainer(root, chunks, symlinkDepth, follow);
}

//------------------------------------------------------------------------------
//! Lookup a URL, while following symlinks.
//------------------------------------------------------------------------------
folly::Future<PathLookupState> HierarchicalView::lookupContainer(
    IContainerMDPtr root,
    const std::vector<std::string> &chunks,
    size_t symlinkDepth, bool follow)
{

  PathLookupState initialState;
  initialState.current = root;
  initialState.symlinkDepth = symlinkDepth;

  folly::Future<PathLookupState> fut = folly::makeFuture<PathLookupState>(std::move(initialState));

  for(size_t i = 0; i < chunks.size(); i++) {
    bool localFollow = true;
    if(!follow && i == chunks.size() -1) localFollow = false;

    fut = fut.then(std::bind(&HierarchicalView::lookupSubcontainer, this, _1, chunks[i], localFollow));
  }

  return fut;
}

//------------------------------------------------------------------------------
//! Lookup a subdirectory, single shot.
//------------------------------------------------------------------------------
folly::Future<PathLookupState> HierarchicalView::lookupSubcontainer(
    PathLookupState parent, std::string name, bool follow)
{
  //----------------------------------------------------------------------------
  // Symlink depth exceeded?
  //----------------------------------------------------------------------------
  if(parent.symlinkDepth > 255) {
      return folly::makeFuture<PathLookupState>(make_mdexception(
        ELOOP, "Too many symbolic links were encountered in translating the pathname"));
  }

  //----------------------------------------------------------------------------
  // Looking up "." ? Just return the directory itself.
  //----------------------------------------------------------------------------
  if(name == ".") {
    return parent;
  }

  //----------------------------------------------------------------------------
  // Looking up ".." ? Just return the parent directory.
  //----------------------------------------------------------------------------
  if(name == "..") {
    return pContainerSvc->getContainerMDFut(parent.current->getParentId())
      .then([this, parent](IContainerMDPtr result) {
        if(!result) {
          //--------------------------------------------------------------------
          // This should not really happen.
          //--------------------------------------------------------------------
          eos_static_crit("Could not lookup parent %lld of ContainerID %lld, wtf", parent.current->getParentId(),  parent.current->getId());
          return folly::makeFuture<PathLookupState>(make_mdexception(ENOENT, "No such file or directory"));
        }

        //----------------------------------------------------------------------
        // Directory was found, all is OK, return a concrete result.
        //----------------------------------------------------------------------
        PathLookupState newState;
        newState.current = result;
        newState.symlinkDepth = parent.symlinkDepth;
        return folly::makeFuture<PathLookupState>(std::move(newState));
      } );
  }

  //----------------------------------------------------------------------------
  // Lookup.
  //----------------------------------------------------------------------------
  folly::Future<PathLookupState> fut = parent.current->findContainerFut(name)
    .then([this, parent, name, follow](IContainerMDPtr result) {

      if(result) {
        //----------------------------------------------------------------------
        // Directory was found, all is OK, return a concrete result.
        //----------------------------------------------------------------------
        PathLookupState newState;
        newState.current = result;
        newState.symlinkDepth = parent.symlinkDepth;
        return folly::makeFuture<PathLookupState>(std::move(newState));
      }
      else {
        //----------------------------------------------------------------------
        // Uh-oh.. maybe we're dealing with a symlink here, lookup a possible
        // symlink, and return a future again.
        //----------------------------------------------------------------------
        if(!follow) {
          //--------------------------------------------------------------------
          // Not following symlinks, short circuit.
          //--------------------------------------------------------------------
          return folly::makeFuture<PathLookupState>(make_mdexception(ENOENT, "No such file or directory"));
        }

        return parent.current->findFileFut(name)
          .then(std::bind(&HierarchicalView::lookupContainerSymlink, this, _1, parent.current, parent.symlinkDepth+1));
      }
    } );

  return fut;
}

//------------------------------------------------------------------------------
// Find the last existing container in the path
//------------------------------------------------------------------------------
std::shared_ptr<IContainerMD>
HierarchicalView::findLastContainer(std::vector<char*>& elements, size_t end,
                                    size_t& index, size_t* link_depths)
{
  std::shared_ptr<IContainerMD> current = pRoot;
  std::shared_ptr<IContainerMD> found;
  size_t position = 0;

  while (position < end) {
    found = current->findContainer(elements[position]);

    if (!found) {
      // Check if link
      std::shared_ptr<IFileMD> flink = current->findFile(elements[position]);

      if (flink) {
        if (flink->isLink()) {
          if (link_depths != nullptr) {
            (*link_depths)++;

            if ((*link_depths) > 255) {
              MDException e(ELOOP);
              e.getMessage() << "Too many symbolic links were encountered "
                             "in translating the pathname";
              throw e;
            }
          }

          std::string link = flink->getLink();

          if (link[0] != '/') {
            link.insert(0, getUri(current.get()));
            eos::PathProcessor::absPath(link);
          }

          found = getContainer(link, false, link_depths);

          if (!found) {
            index = position;
            return current;
          }
        }
      }

      if (!found) {
        index = position;
        return current;
      }
    }

    current = found;
    ++position;
  }

  index = position;
  return current;
}

//------------------------------------------------------------------------------
// Get uri for the container
//------------------------------------------------------------------------------
std::string
HierarchicalView::getUri(const IContainerMD* container) const
{
  // Check the input
  if (container == nullptr) {
    MDException ex;
    ex.getMessage() << "Invalid container (zero pointer)";
    throw ex;
  }

  return getUri(container->getId());
}

//------------------------------------------------------------------------------
// Get uri for container id
//------------------------------------------------------------------------------
std::string
HierarchicalView::getUri(const IContainerMD::id_t cid) const
{
  // Gather the uri elements
  std::vector<std::string> elements;
  elements.reserve(10);
  std::shared_ptr<IContainerMD> cursor = pContainerSvc->getContainerMD(cid);

  while (cursor->getId() != 1) {
    elements.push_back(cursor->getName());
    cursor = pContainerSvc->getContainerMD(cursor->getParentId());
  }

  // Assemble the uri
  std::string path = "/";
  std::vector<std::string>::reverse_iterator rit;

  for (rit = elements.rbegin(); rit != elements.rend(); ++rit) {
    path += *rit;
    path += "/";
  }

  return path;
}

//------------------------------------------------------------------------------
// Get uri for the file
//------------------------------------------------------------------------------
std::string
HierarchicalView::getUri(const IFileMD* file) const
{
  // Check the input
  if (file == nullptr) {
    MDException ex;
    ex.getMessage() << "Invalid file (zero pointer)";
    throw ex;
  }

  // Get the uri
  std::shared_ptr<IContainerMD> cont =
    pContainerSvc->getContainerMD(file->getContainerId());
  std::string path = getUri(cont.get());
  return path + file->getName();
}

//------------------------------------------------------------------------
// Get real path translating existing symlink
//------------------------------------------------------------------------
std::string HierarchicalView::getRealPath(const std::string& uri)
{
  if (uri == "/") {
    MDException e(ENOENT);
    e.getMessage() << " is not a file";
    throw e;
  }

  std::vector<std::string> chunks;
  eos::PathProcessor::splitPath(chunks, uri);

  eos_assert(chunks.size() != 0);
  if(chunks.size() == 1) return chunks[0];

  //----------------------------------------------------------------------------
  // Remove last chunk
  //----------------------------------------------------------------------------
  std::string lastChunk = chunks[chunks.size()-1];
  chunks.pop_back();

  //----------------------------------------------------------------------------
  // Lookup parent container..
  //----------------------------------------------------------------------------
  IContainerMDPtr cont = lookupContainer(pRoot, chunks, 0, true).get().current;
  return SSTR(getUri(cont.get()) << lastChunk);
}

//------------------------------------------------------------------------------
// Get quota node id concerning given container
//------------------------------------------------------------------------------
IQuotaNode*
HierarchicalView::getQuotaNode(const IContainerMD* container, bool search)
{
  // Initial sanity check
  if (container == nullptr) {
    MDException ex;
    ex.getMessage() << "Invalid container (zero pointer)";
    throw ex;
  }

  if (pQuotaStats == nullptr) {
    MDException ex;
    ex.getMessage() << "No QuotaStats placeholder registered";
    throw ex;
  }

  // Search for the node
  std::shared_ptr<IContainerMD> current =
    pContainerSvc->getContainerMD(container->getId());

  if (search) {
    while (current->getName() != pRoot->getName() &&
           (current->getFlags() & QUOTA_NODE_FLAG) == 0) {
      current = pContainerSvc->getContainerMD(current->getParentId());
    }
  }

  // We have either found a quota node or reached root without finding one
  // so we need to double check whether the current container has an
  // associated quota node
  if ((current->getFlags() & QUOTA_NODE_FLAG) == 0) {
    return nullptr;
  }

  IQuotaNode* node = pQuotaStats->getQuotaNode(current->getId());

  if (node != nullptr) {
    return node;
  }

  return pQuotaStats->registerNewNode(current->getId());
}

//------------------------------------------------------------------------------
// Register the container to be a quota node
//------------------------------------------------------------------------------
IQuotaNode*
HierarchicalView::registerQuotaNode(IContainerMD* container)
{
  // Initial sanity check
  if (container == nullptr) {
    MDException ex;
    ex.getMessage() << "Invalid container (zero pointer)";
    throw ex;
  }

  if (pQuotaStats == nullptr) {
    MDException ex;
    ex.getMessage() << "No QuotaStats placeholder registered";
    throw ex;
  }

  if ((container->getFlags() & QUOTA_NODE_FLAG) != 0) {
    MDException ex;
    ex.getMessage() << "Already a quota node: " << container->getId();
    throw ex;
  }

  IQuotaNode* node = pQuotaStats->registerNewNode(container->getId());
  container->setFlags(container->getFlags() | QUOTA_NODE_FLAG);
  updateContainerStore(container);
  return node;
}

//------------------------------------------------------------------------------
// Remove the quota node
//------------------------------------------------------------------------------
void
HierarchicalView::removeQuotaNode(IContainerMD* container)
{
  // Sanity checks
  if (container == nullptr) {
    MDException ex;
    ex.getMessage() << "Invalid container (zero pointer)";
    throw ex;
  }

  if (pQuotaStats == nullptr) {
    MDException ex;
    ex.getMessage() << "No QuotaStats placeholder registered";
    throw ex;
  }

  if ((container->getFlags() & QUOTA_NODE_FLAG) == 0) {
    MDException ex;
    ex.getMessage() << "Not a quota node: " << container->getId();
    throw ex;
  }

  // Get the quota node and meld it with the parent node if present
  IQuotaNode* node = getQuotaNode(container);
  IQuotaNode* parent = nullptr;

  if (container->getId() != 1) {
    parent = getQuotaNode(
               pContainerSvc->getContainerMD(container->getParentId()).get(), true);
  }

  container->setFlags(container->getFlags() & ~QUOTA_NODE_FLAG);
  updateContainerStore(container);

  if (parent != nullptr) {
    try {
      parent->meld(node);
    } catch (const std::runtime_error& e) {
      MDException ex;
      ex.getMessage() << "Failed quota node meld: " << e.what();
      throw ex;
    }
  }

  pQuotaStats->removeNode(container->getId());
}

//------------------------------------------------------------------------------
// Rename container
//------------------------------------------------------------------------------
void
HierarchicalView::renameContainer(IContainerMD* container,
                                  const std::string& newName)
{
  if (container == nullptr) {
    MDException ex;
    ex.getMessage() << "Invalid container (zero pointer)";
    throw ex;
  }

  if (newName.empty()) {
    MDException ex;
    ex.getMessage() << "Invalid new name (empty)";
    throw ex;
  }

  if (newName.find('/') != std::string::npos) {
    MDException ex;
    ex.getMessage() << "Name cannot contain slashes: " << newName;
    throw ex;
  }

  if (container->getId() == container->getParentId()) {
    MDException ex;
    ex.getMessage() << "Cannot rename /";
    throw ex;
  }

  std::shared_ptr<IContainerMD> parent{
    pContainerSvc->getContainerMD(container->getParentId())};

  if (parent->findContainer(newName) != nullptr) {
    MDException ex;
    ex.getMessage() << "Container exists: " << newName;
    throw ex;
  }

  if (parent->findFile(newName) != nullptr) {
    MDException ex;
    ex.getMessage() << "File exists: " << newName;
    throw ex;
  }

  parent->removeContainer(container->getName());
  container->setName(newName);
  parent->addContainer(container);
  updateContainerStore(container);
}

//------------------------------------------------------------------------------
// Rename file
//------------------------------------------------------------------------------
void
HierarchicalView::renameFile(IFileMD* file, const std::string& newName)
{
  if (file == nullptr) {
    MDException ex;
    ex.getMessage() << "Invalid file (zero pointer)";
    throw ex;
  }

  if (newName.empty()) {
    MDException ex;
    ex.getMessage() << "Invalid new name (empty)";
    throw ex;
  }

  if (newName.find('/') != std::string::npos) {
    MDException ex;
    ex.getMessage() << "Name cannot contain slashes: " << newName;
    throw ex;
  }

  std::shared_ptr<IContainerMD> parent{
    pContainerSvc->getContainerMD(file->getContainerId())};

  if (parent->findContainer(newName) != nullptr) {
    MDException ex;
    ex.getMessage() << "Container exists: " << newName;
    throw ex;
  }

  if (parent->findFile(newName) != nullptr) {
    MDException ex;
    ex.getMessage() << "File exists: " << newName;
    throw ex;
  }

  parent->removeFile(file->getName());
  file->setName(newName);
  parent->addFile(file);
  updateFileStore(file);
}

EOSNSNAMESPACE_END
