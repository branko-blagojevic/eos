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
//! @author Elvin Sindrilaru <esindril@cern.ch>
//! @brief The filesystem view stored in QuarkDB
//------------------------------------------------------------------------------

#ifndef EOS_NS_FILESYSTEM_VIEW_HH
#define EOS_NS_FILESYSTEM_VIEW_HH

#include "namespace/MDException.hh"
#include "namespace/Namespace.hh"
#include "namespace/interface/IFsView.hh"
#include "namespace/ns_quarkdb/Constants.hh"
#include "namespace/ns_quarkdb/accounting/FileSystemView.hh"
#include "namespace/ns_quarkdb/accounting/FileSystemHandler.hh"
#include "qclient/QClient.hh"
#include "qclient/QSet.hh"
#include <utility>

EOSNSNAMESPACE_BEGIN

class MetadataFlusher;

//------------------------------------------------------------------------------
//! File System iterator implementation on top of QuarkDB.
//! The proper solution would be that the object itself contacts redis running
//! SCAN, but this should be fine for now.
//------------------------------------------------------------------------------
class QdbFileSystemIterator:
  public ICollectionIterator<IFileMD::location_t>
{
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  QdbFileSystemIterator(std::set<IFileMD::location_t>&& filesystems)
  {
    pFilesystems = std::move(filesystems);
    iterator = pFilesystems.begin();
  }

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  virtual ~QdbFileSystemIterator() = default;

  //----------------------------------------------------------------------------
  //! Get current fsid
  //----------------------------------------------------------------------------
  IFileMD::location_t getElement() override
  {
    return *iterator;
  }

  //----------------------------------------------------------------------------
  //! Check if iterator is valid
  //----------------------------------------------------------------------------
  bool valid() override
  {
    return iterator != pFilesystems.end();
  }

  //----------------------------------------------------------------------------
  //! Progress iterator by 1 - only has any effect if iterator is valid
  //----------------------------------------------------------------------------
  void next() override
  {
    if (valid()) {
      iterator++;
    }
  }

private:
  std::set<IFileMD::location_t> pFilesystems;
  std::set<IFileMD::location_t>::iterator iterator;
};

//------------------------------------------------------------------------------
//! Class QdbFileIterator that can iterate through a list of files from the
//! FileSystem class. Used to iterate through the files / unlinked files on a
//! filesystem.
//------------------------------------------------------------------------------
class QdbFileIterator:
  public ICollectionIterator<IFileMD::id_t>
{
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  QdbFileIterator(qclient::QClient& qcl, const std::string& key) :
    mSet(qcl, key), mCursor("0")
  {
    mReply = mSet.sscan(mCursor, mCount);
    mCursor = mReply.first;
    mIt = mReply.second.begin();
  }

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  virtual ~QdbFileIterator() = default;

  //----------------------------------------------------------------------------
  //! Check if iterator is valid
  //----------------------------------------------------------------------------
  bool valid() override
  {
    return (mIt != mReply.second.end());
  }

  //----------------------------------------------------------------------------
  //! Get current file id
  //----------------------------------------------------------------------------
  IFileMD::id_t getElement() override
  {
    return std::stoull(*mIt);
  }

  //----------------------------------------------------------------------------
  //! Retrieve next file id
  //----------------------------------------------------------------------------
  void next() override
  {
    if (valid()) {
      ++mIt;

      if ((mIt == mReply.second.end()) && (mCursor != "0")) {
        mReply = mSet.sscan(mCursor, mCount);
        mCursor = mReply.first;
        mIt = mReply.second.begin();
      }
    }
  }

private:
  qclient::QSet mSet; ///< Set to iterate through
  std::string mCursor; ///< Cursor used while scanning the set
  int64_t mCount = 2000000; ///< Max number of elements returned at once
  std::pair<std::string, std::vector<std::string>> mReply;
  std::vector<std::string>::iterator mIt; ///< Iterator to element to return
};

//------------------------------------------------------------------------------
// File System iterator implementation of a in-memory namespace
// Trivial implementation, using the same logic to iterate over filesystems
// as we did with "getNumFileSystems" before.
//------------------------------------------------------------------------------
class ListFileSystemIterator:
  public ICollectionIterator<IFileMD::location_t>
{
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  ListFileSystemIterator(const std::map<IFileMD::location_t,
                         IFsView::FileList>& map)
  {
    for (const auto& pair : map) {
      mList.push_back(pair.first);
    }

    mIt = mList.cbegin();
  }

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  virtual ~ListFileSystemIterator() = default;

  //----------------------------------------------------------------------------
  //! Get current fsid
  //----------------------------------------------------------------------------
  IFileMD::location_t getElement() override
  {
    return *mIt;
  }

  //----------------------------------------------------------------------------
  //! Check if iterator is valid
  //----------------------------------------------------------------------------
  bool valid() override
  {
    return (mIt != mList.cend());
  }

  //----------------------------------------------------------------------------
  //! Retrieve next fsid - returns false when no more filesystems exist
  //----------------------------------------------------------------------------
  void next() override
  {
    if (valid()) {
      ++mIt;
    }
  }

private:
  std::list<IFileMD::location_t> mList;
  std::list<IFileMD::location_t>::const_iterator mIt;
};


//------------------------------------------------------------------------------
//! FileSystemView implementation on top of QuarkDB
//!
//! This class keeps a mapping between filesystem ids and the actual file ids
//! that reside on that particular filesystem. For each fsid we keep a set
//! structure in Redis i.e. fs_id:fsview_files that holds the file ids. E.g.:
//!
//! fsview:1:files -->  fid4, fid87, fid1002 etc.
//! fsview:2:files ...
//! ...
//! fsview:n:files ...
//!
//! Besides these data structures we also have:
//!
//! fsview_noreplicas - file ids that don't have any replicas on any fs
//! fsview:x:unlinked - set of file ids that are unlinked on file system "x"
//------------------------------------------------------------------------------
class FileSystemView : public IFsView
{
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  FileSystemView();

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  virtual ~FileSystemView() = default;

  //----------------------------------------------------------------------------
  //! Notify me about the changes in the main view
  //----------------------------------------------------------------------------
  virtual void fileMDChanged(IFileMDChangeListener::Event* e) override;

  //----------------------------------------------------------------------------
  //! Notify me about files when recovering from changelog - not used
  //----------------------------------------------------------------------------
  virtual void fileMDRead(IFileMD* obj) override {};

  //----------------------------------------------------------------------------
  //! Recheck the current file object and make any modifications necessary so
  //! that the information is consistent in the back-end KV store.
  //!
  //! @param file file object to be checked
  //!
  //! @return true if successful, otherwise false
  //----------------------------------------------------------------------------
  virtual bool fileMDCheck(IFileMD* file) override;

  //----------------------------------------------------------------------------
  //! Get iterator to list of files on a particular file system
  //!
  //! @param location file system id
  //!
  //! @return shared ptr to collection iterator
  //----------------------------------------------------------------------------
  std::shared_ptr<ICollectionIterator<IFileMD::id_t>>
      getFileList(IFileMD::location_t location) override;

  //----------------------------------------------------------------------------
  //! Get an approximately random file residing within the given filesystem.
  //!
  //! @param location file system id
  //! @param retval a file id residing within the given filesystem
  //!
  //! @return bool indicating whether the operation was successful
  //----------------------------------------------------------------------------
  bool getApproximatelyRandomFileInFs(IFileMD::location_t location,
    IFileMD::id_t &retval) override;

  //----------------------------------------------------------------------------
  //! Get number of files on the given file system
  //!
  //! @param fs_id file system id
  //!
  //! @return number of files
  //----------------------------------------------------------------------------
  uint64_t getNumFilesOnFs(IFileMD::location_t fs_id) override;

  //----------------------------------------------------------------------------
  //! Get iterator to list of unlinked files on a particular file system
  //!
  //! @param location file system id
  //!
  //! @return shared ptr to collection iterator
  //----------------------------------------------------------------------------
  std::shared_ptr<ICollectionIterator<IFileMD::id_t>>
      getUnlinkedFileList(IFileMD::location_t location) override;

  //----------------------------------------------------------------------------
  //! Get number of unlinked files on the given file system
  //!
  //! @param fs_id file system id
  //!
  //! @return number of files
  //----------------------------------------------------------------------------
  uint64_t getNumUnlinkedFilesOnFs(IFileMD::location_t fs_id) override;

  //----------------------------------------------------------------------------
  //! Get iterator to list of files without replicas
  //!
  //! @return shard ptr to collection iterator
  //----------------------------------------------------------------------------
  std::shared_ptr<ICollectionIterator<IFileMD::id_t>>
      getNoReplicasFileList() override;

  //----------------------------------------------------------------------------
  //! Get streaming iterator to list of files without replicas
  //!
  //! @return shard ptr to collection iterator
  //----------------------------------------------------------------------------
  std::shared_ptr<ICollectionIterator<IFileMD::id_t>>
      getStreamingNoReplicasFileList() override;

  //----------------------------------------------------------------------------
  //! Get number of files with no replicas
  //----------------------------------------------------------------------------
  uint64_t getNumNoReplicasFiles() override;

  //----------------------------------------------------------------------------
  //! Clear unlinked files for filesystem
  //!
  //! @param location filssystem id
  //!
  //! @return true if cleanup done successfully, otherwise false
  //----------------------------------------------------------------------------
  bool clearUnlinkedFileList(IFileMD::location_t location) override;

  //----------------------------------------------------------------------------
  //! Get iterator object to run through all currently active filesystem IDs
  //----------------------------------------------------------------------------
  std::shared_ptr<ICollectionIterator<IFileMD::location_t>>
      getFileSystemIterator() override;

  //----------------------------------------------------------------------------
  //! Check if file system has file id
  //!
  //! @param fid file id
  //! @param fs_id file system id
  //!
  //! @return true if file is on the provided file system, otherwise false
  //----------------------------------------------------------------------------
  bool hasFileId(IFileMD::id_t fid, IFileMD::location_t fs_id) override;

  //----------------------------------------------------------------------------
  //! Configure
  //!
  //! @param config map of configuration parameters
  //----------------------------------------------------------------------------
  void configure(const std::map<std::string, std::string>& config) override;

  //----------------------------------------------------------------------------
  //! Finalize - no-op for this type of view
  //----------------------------------------------------------------------------
  void finalize() override {};

  //----------------------------------------------------------------------------
  //! Shrink maps - no-op for this type of view
  //----------------------------------------------------------------------------
  void shrink() override {};

  //----------------------------------------------------------------------------
  //! Add tree - no-op for this type of view
  //----------------------------------------------------------------------------
  void AddTree(IContainerMD* obj, int64_t dsize) override {};

  //----------------------------------------------------------------------------
  //! Remove tree - no-op for this type of view
  //----------------------------------------------------------------------------
  void RemoveTree(IContainerMD* obj, int64_t dsize) override {};

private:

  //----------------------------------------------------------------------------
  //! Load view from backend
  //----------------------------------------------------------------------------
  void loadFromBackend();

  //----------------------------------------------------------------------------
  //! Get iterator object to run through all the filesystem IDs stored in the
  //! backend.
  //----------------------------------------------------------------------------
  std::shared_ptr<ICollectionIterator<IFileMD::location_t>>
      getQdbFileSystemIterator(const std::string& pattern);

  //----------------------------------------------------------------------------
  //! Get iterator to list of unlinked files on a particular file system
  //!
  //! @param location file system id
  //!
  //! @return shared ptr to collection iterator
  //----------------------------------------------------------------------------
  std::shared_ptr<ICollectionIterator<IFileMD::id_t>>
      getQdbUnlinkedFileList(IFileMD::location_t location);

  //----------------------------------------------------------------------------
  //! Get iterator to list of files on a particular file system
  //!
  //! @param location file system id
  //!
  //! @return shared ptr to collection iterator
  //----------------------------------------------------------------------------
  std::shared_ptr<ICollectionIterator<IFileMD::id_t>>
      getQdbFileList(IFileMD::location_t location);

  //----------------------------------------------------------------------------
  //! Cache from backend the list of file on the file system
  //!
  //! @param fsid file system id
  //----------------------------------------------------------------------------
  void CacheFiles(IFileMD::location_t fsid);

  //----------------------------------------------------------------------------
  //! Cache from backend the list of unlinked file on the file system
  //!
  //! @param fsid file system id
  //----------------------------------------------------------------------------
  void CacheUnlinkedFiles(IFileMD::location_t fsid);

  ///! Folly executor
  std::unique_ptr<folly::Executor> mExecutor;
  ///! Metadata flusher object
  MetadataFlusher* pFlusher;
  ///! QClient object
  qclient::QClient* pQcl;

  ///! No replicas handler
  std::unique_ptr<FileSystemHandler> mNoReplicas;


  ///! Map of file ids residing on a particular file system
  std::map<IFileMD::location_t, IFsView::FileList> pFiles;
  ///! Mark if file systemd id info is already cached
  std::map<IFileMD::location_t, bool> pFilesCached;
  ///! Map of unlinked file ids residing on a particular file system
  std::map<IFileMD::location_t, IFsView::FileList> pUnlinkedFiles;
  ///! Mark if file systemd id info is already cached
  std::map<IFileMD::location_t, bool> pUnlinkedFilesCached;
};

//------------------------------------------------------------------------------
//! Parse an fs set key, returning its id and whether it points to "files" or
//! "unlinked"
//!
//! @param str input stirng
//! @param fsid parsed fsids
//! @param unlinked if true then this is an fsid from an unlinked list
//!
//! @return true if parsing successful, otherwise false
//------------------------------------------------------------------------------
bool parseFsId(const std::string& str, IFileMD::location_t& fsid,
               bool& unlinked);

EOSNSNAMESPACE_END

#endif // __EOS_NS_FILESYSTEM_VIEW_HH__
