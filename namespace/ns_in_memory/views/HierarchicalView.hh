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

//------------------------------------------------------------------------------
// author: Lukasz Janyst <ljanyst@cern.ch>
// desc:   Hierarchical namespace implementation
//------------------------------------------------------------------------------

#ifndef EOS_NS_HIERARHICAL_VIEW_HH
#define EOS_NS_HIERARHICAL_VIEW_HH

#include "namespace/interface/IView.hh"
#include "namespace/interface/IContainerMDSvc.hh"
#include "namespace/interface/IFileMDSvc.hh"
#include "namespace/ns_in_memory/accounting/QuotaStats.hh"

#ifdef __clang__
#pragma clang diagnostic ignored "-Wunused-private-field"
#endif

namespace eos
{
//----------------------------------------------------------------------------
//! Implementation of the hierarchical namespace
//----------------------------------------------------------------------------
class HierarchicalView: public IView
{
public:
  //------------------------------------------------------------------------
  //! Constructor
  //------------------------------------------------------------------------
  HierarchicalView(): pContainerSvc((IContainerMDSvc*)0),
    pFileSvc((IFileMDSvc*)0), pRoot((IContainerMD*)0)
  {
    pQuotaStats = new QuotaStats();
  }

  //------------------------------------------------------------------------
  //! Destructor
  //------------------------------------------------------------------------
  virtual ~HierarchicalView()
  {
    delete pQuotaStats;
  }

  //------------------------------------------------------------------------
  //! Specify a pointer to the underlying container service
  //------------------------------------------------------------------------
  virtual void setContainerMDSvc(IContainerMDSvc* containerSvc) override
  {
    pContainerSvc = containerSvc;
  }

  //------------------------------------------------------------------------
  //! Get the container svc pointer
  //------------------------------------------------------------------------
  virtual IContainerMDSvc* getContainerMDSvc() override
  {
    return pContainerSvc;
  }

  //------------------------------------------------------------------------
  //! Specify a pointer to the underlying file service that alocates the
  //! actual files
  //------------------------------------------------------------------------
  virtual void setFileMDSvc(IFileMDSvc* fileMDSvc) override
  {
    pFileSvc = fileMDSvc;
  }

  //------------------------------------------------------------------------
  //! Get the FileMDSvc
  //------------------------------------------------------------------------
  virtual IFileMDSvc* getFileMDSvc() override
  {
    return pFileSvc;
  }

  //------------------------------------------------------------------------
  //! Configure the view
  //------------------------------------------------------------------------
  virtual void configure(const std::map<std::string, std::string>& config)
  override;

  //------------------------------------------------------------------------
  //! Initialize the view
  //------------------------------------------------------------------------
  virtual void initialize() override;

  virtual void initialize1() override;// phase 1 - load & setup container
  virtual void initialize2() override;// phase 2 - load files
  virtual void initialize3() override;// phase 3 - register files in container

  //------------------------------------------------------------------------
  //! Finalize the view
  //------------------------------------------------------------------------
  virtual void finalize() override;

  //------------------------------------------------------------------------
  //! Retrieve a file for given uri, asynchronously
  //------------------------------------------------------------------------
  virtual folly::Future<IFileMDPtr> getFileFut(const std::string& uri, bool follow = true) override;

  //------------------------------------------------------------------------
  //! Retrieve a file for given uri
  //------------------------------------------------------------------------
  virtual std::shared_ptr<IFileMD> getFile(const std::string& uri,
      bool follow = true,
      size_t* link_depths = 0) override;

  //------------------------------------------------------------------------
  //! Create a file for given uri
  //------------------------------------------------------------------------
  virtual std::shared_ptr<IFileMD> createFile(const std::string& uri,
      uid_t uid = 0, gid_t gid = 0) override;

  //------------------------------------------------------------------------
  //! Create a link for given uri
  //------------------------------------------------------------------------
  virtual void createLink(const std::string& uri,
                          const std::string& linkuri,
                          uid_t uid = 0, gid_t gid = 0) override;

  //------------------------------------------------------------------------
  //! Update file store
  //------------------------------------------------------------------------
  virtual void updateFileStore(IFileMD* file) override
  {
    pFileSvc->updateStore(file);
  }

  //------------------------------------------------------------------------
  //! Remove a link
  //------------------------------------------------------------------------
  virtual void removeLink(const std::string& uri) override;

  //------------------------------------------------------------------------
  //! Unlink the file
  //------------------------------------------------------------------------
  virtual void unlinkFile(const std::string& uri) override;

  //------------------------------------------------------------------------
  //! Remove the file from the hierarchy so that it won't be accessible
  //! by path anymore and unlink all of it's replicas. The file needs
  //! to be manually removed (ie. using removeFile method) once it has
  //! no valid replicas.
  //!
  //! @param file IFileMD object to be removed
  //------------------------------------------------------------------------
  virtual void unlinkFile(eos::IFileMD* file) override;

  //------------------------------------------------------------------------
  //! Remove the file
  //------------------------------------------------------------------------
  virtual void removeFile(IFileMD* file) override;

  //------------------------------------------------------------------------
  //! Get a container (directory) asynchronously
  //------------------------------------------------------------------------
  virtual folly::Future<IContainerMDPtr> getContainerFut(const std::string& uri,
      bool follow = true) override;

  //------------------------------------------------------------------------
  //! Get a container (directory)
  //------------------------------------------------------------------------
  virtual std::shared_ptr<IContainerMD>
  getContainer(const std::string& uri, bool follow = true,
               size_t* link_depth = 0) override;

  //------------------------------------------------------------------------
  //! Create a container (directory)
  //------------------------------------------------------------------------
  virtual std::shared_ptr<IContainerMD>
  createContainer(const std::string& uri, bool createParents = false) override;

  //------------------------------------------------------------------------
  //! Update container store
  //------------------------------------------------------------------------
  virtual void updateContainerStore(IContainerMD* container) override
  {
    pContainerSvc->updateStore(container);
  }

  //------------------------------------------------------------------------
  //! Remove a container (directory)
  //------------------------------------------------------------------------
  virtual void removeContainer(const std::string& uri) override;

  //------------------------------------------------------------------------
  //! Get uri for the container
  //------------------------------------------------------------------------
  virtual std::string getUri(const IContainerMD* container) const override;

  //------------------------------------------------------------------------
  //! Get uri for the container - asynchronous version
  //------------------------------------------------------------------------
  virtual folly::Future<std::string> getUriFut(const IContainerMD* container) const override;

  //------------------------------------------------------------------------
  //! Get uri for container id
  //------------------------------------------------------------------------
  virtual std::string getUri(const IContainerMD::id_t cid) const override;

  //------------------------------------------------------------------------
  //! Get uri for the file
  //------------------------------------------------------------------------
  virtual std::string getUri(const IFileMD* file) const override;

  //------------------------------------------------------------------------
  //! Get uri for the file
  //------------------------------------------------------------------------
  virtual folly::Future<std::string> getUriFut(const IFileMD* file) const override;

  //------------------------------------------------------------------------
  //! Get real path translating existing symlink
  //------------------------------------------------------------------------
  virtual std::string getRealPath(const std::string& path) override;

  //------------------------------------------------------------------------
  //! Get quota node id concerning given container
  //------------------------------------------------------------------------
  virtual IQuotaNode* getQuotaNode(const IContainerMD* container,
                                   bool               search = true) override;

  //------------------------------------------------------------------------
  //! Register the container to be a quota node
  //------------------------------------------------------------------------
  virtual IQuotaNode* registerQuotaNode(IContainerMD* container) override;

  //------------------------------------------------------------------------
  //! Remove the quota node
  //------------------------------------------------------------------------
  virtual void removeQuotaNode(IContainerMD* container) override;

  //------------------------------------------------------------------------
  //! Get the quota stats placeholder
  //------------------------------------------------------------------------
  virtual IQuotaStats* getQuotaStats() override
  {
    return pQuotaStats;
  }

  //------------------------------------------------------------------------
  //! Set the quota stats placeholder, currently associated object (if any)
  //! won't beX deleted.
  //------------------------------------------------------------------------
  virtual void setQuotaStats(IQuotaStats* quotaStats) override
  {
    pQuotaStats = quotaStats;
  }

  //------------------------------------------------------------------------
  //! Rename container
  //------------------------------------------------------------------------
  virtual void renameContainer(IContainerMD* container,
                               const std::string& newName) override;

  //------------------------------------------------------------------------
  //! Rename file
  //------------------------------------------------------------------------
  virtual void renameFile(IFileMD* file, const std::string& newName) override;

  //----------------------------------------------------------------------------
  //! Return whether this is an in-memory namespace.
  //----------------------------------------------------------------------------
  virtual bool inMemory() override {
    return true;
  }

private:
  std::shared_ptr<IContainerMD> findLastContainer(
    std::vector<char*>& elements, size_t end, size_t& index,
    size_t* link_depths = 0);

  //------------------------------------------------------------------------
  // File visitor for reloading
  //------------------------------------------------------------------------
  class FileVisitor: public IFileVisitor
  {
  public:
    FileVisitor(IContainerMDSvc* contSvc,
                IQuotaStats* quotaStats,
                IView* view):
      pContSvc(contSvc), pQuotaStats(quotaStats), pView(view) {}

    virtual void visitFile(IFileMD* file);

  private:
    IContainerMDSvc* pContSvc;
    IQuotaStats*     pQuotaStats;
    IView*           pView;
  };

  //------------------------------------------------------------------------
  // Data members
  //------------------------------------------------------------------------
  IContainerMDSvc* pContainerSvc;
  IFileMDSvc*      pFileSvc;
  IQuotaStats*     pQuotaStats;
  std::shared_ptr<IContainerMD> pRoot;
};
};

#endif // EOS_NS_HIERARCHICAL_VIEW_HH
