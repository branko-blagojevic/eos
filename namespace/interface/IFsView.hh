//------------------------------------------------------------------------------
//! @file IFsView.hh
//! @author Elvin-Alin Sindrilaru <esindril@cern.ch>
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2015 CERN/Switzerland                                  *
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

#ifndef __EOS_NS_IFSVIEW_HH__
#define __EOS_NS_IFSVIEW_HH__

#include "namespace/Namespace.hh"
#include "namespace/MDException.hh"
#include "namespace/interface/IFileMDSvc.hh"
#include <google/dense_hash_set>
#include <set>

EOSNSNAMESPACE_BEGIN

//------------------------------------------------------------------------------
//! Container iterator abtract class
//------------------------------------------------------------------------------
template<typename T>
class ICollectionIterator
{
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  ICollectionIterator() {}

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  virtual ~ICollectionIterator() {}

  //----------------------------------------------------------------------------
  //! Get current fsid
  //----------------------------------------------------------------------------
  virtual T getElement() = 0;

  //----------------------------------------------------------------------------
  //! Check if iterator is valid
  //----------------------------------------------------------------------------
  virtual bool valid() = 0;

  //----------------------------------------------------------------------------
  //! Progress iterator by 1 - only has any effect if iterator is valid
  //----------------------------------------------------------------------------
  virtual void next() = 0;
};

//------------------------------------------------------------------------------
//! File System view abtract class
//------------------------------------------------------------------------------
class IFsView: public IFileMDChangeListener
{
public:

  //------------------------------------------------------------------------
  // Google sparse table is used for much lower memory overhead per item
  // than a list and it's fragmented structure speeding up deletions.
  // The filelists we keep are quite big - a list would be faster
  // but more memory consuming, a vector would be slower but less
  // memory consuming. We changed to dense hash set since it is much faster
  // and the memory overhead is not visible in a million file namespace.
  //------------------------------------------------------------------------
  typedef google::dense_hash_set <IFileMD::id_t,
          Murmur3::MurmurHasher<uint64_t> > FileList;

  //----------------------------------------------------------------------------
  //! Contructor
  //----------------------------------------------------------------------------
  IFsView() = default;

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  virtual ~IFsView() = default;

  //----------------------------------------------------------------------------
  //! Configure
  //!
  //! @param config map of configuration parameters
  //----------------------------------------------------------------------------
  virtual void configure(const std::map<std::string, std::string>& config) = 0;

  //----------------------------------------------------------------------------
  //! Notify me about the changes in the main view
  //! IFileeMDChangeListener interface
  //----------------------------------------------------------------------------
  virtual void fileMDChanged(IFileMDChangeListener::Event* e) = 0;

  //----------------------------------------------------------------------------
  //! Notify me about files when recovering from changelog
  //! IFileeMDChangeListener interface
  //----------------------------------------------------------------------------
  virtual void fileMDRead(IFileMD* obj) = 0;

  //----------------------------------------------------------------------------
  //! Get iterator to list of files on a particular file system
  //!
  //! @param location file system id
  //!
  //! @return shared ptr to collection iterator
  //----------------------------------------------------------------------------
  virtual std::shared_ptr<ICollectionIterator<IFileMD::id_t>>
      getFileList(IFileMD::location_t location) = 0;

  //----------------------------------------------------------------------------
  //! Get streaming iterator to list of files on a particular file system
  //!
  //! @param location file system id
  //!
  //! @return shared ptr to collection iterator
  //----------------------------------------------------------------------------
  virtual std::shared_ptr<ICollectionIterator<IFileMD::id_t>>
      getStreamingFileList(IFileMD::location_t location) = 0;

  //----------------------------------------------------------------------------
  //! Erase an entry from all filesystem view collections
  //!
  //! @param location where to remove
  //! @param file id to remove
  //!
  //! @return
  //----------------------------------------------------------------------------
  virtual void eraseEntry(IFileMD::location_t location, IFileMD::id_t) = 0;

  //----------------------------------------------------------------------------
  //! Get an approximately random file residing within the given filesystem.
  //!
  //! @param location file system id
  //! @param retval a file id residing within the given filesystem
  //!
  //! @return bool indicating whether the operation was successful
  //----------------------------------------------------------------------------
  virtual bool getApproximatelyRandomFileInFs(IFileMD::location_t location,
      IFileMD::id_t& retval) = 0;

  //----------------------------------------------------------------------------
  //! Get number of files on the given file system
  //!
  //! @param fs_id file system id
  //!
  //! @return number of files
  //----------------------------------------------------------------------------
  virtual uint64_t getNumFilesOnFs(IFileMD::location_t fs_id) = 0;

  //----------------------------------------------------------------------------
  //! Get iterator to list of unlinked files on a particular file system
  //!
  //! @param location file system id
  //!
  //! @return shared ptr to collection iterator
  //----------------------------------------------------------------------------
  virtual std::shared_ptr<ICollectionIterator<IFileMD::id_t>>
      getUnlinkedFileList(IFileMD::location_t location) = 0;

  //----------------------------------------------------------------------------
  //! Get number of unlinked files on the given file system
  //!
  //! @param fs_id file system id
  //!
  //! @return number of files
  //----------------------------------------------------------------------------
  virtual uint64_t getNumUnlinkedFilesOnFs(IFileMD::location_t fs_id) = 0;

  //----------------------------------------------------------------------------
  //! Clear unlinked files for filesystem
  //!
  //! @param location filssystem id
  //!
  //! @return True if cleanup done successfully, otherwise false.
  //----------------------------------------------------------------------------
  virtual bool clearUnlinkedFileList(IFileMD::location_t location) = 0;

  //----------------------------------------------------------------------------
  //! Get iterator to list of files without replicas
  //!
  //! @return shard ptr to collection iterator
  //----------------------------------------------------------------------------
  virtual std::shared_ptr<ICollectionIterator<IFileMD::id_t>>
      getNoReplicasFileList() = 0;

  //----------------------------------------------------------------------------
  //! Get streaming iterator to list of files without replicas
  //!
  //! @return shard ptr to collection iterator
  //----------------------------------------------------------------------------
  virtual std::shared_ptr<ICollectionIterator<IFileMD::id_t>>
      getStreamingNoReplicasFileList() = 0;

  //----------------------------------------------------------------------------
  //! Get number of files with no replicas
  //----------------------------------------------------------------------------
  virtual uint64_t getNumNoReplicasFiles() = 0;

  //----------------------------------------------------------------------------
  //! Get iterator object to run through all currently active filesystem IDs
  //----------------------------------------------------------------------------
  virtual std::shared_ptr<ICollectionIterator<IFileMD::location_t>>
      getFileSystemIterator() = 0;

  //----------------------------------------------------------------------------
  //! Check if file system has file id
  //!
  //! @param fid file id
  //! @param fs_id file system id
  //!
  //! @return true if file is on the provided file system, otherwise false
  //----------------------------------------------------------------------------
  virtual bool hasFileId(IFileMD::id_t fid, IFileMD::location_t fs_id) = 0;

  //----------------------------------------------------------------------------
  //! Finalize
  //----------------------------------------------------------------------------
  virtual void finalize() = 0;

  //----------------------------------------------------------------------------
  //! Shrink maps
  //----------------------------------------------------------------------------
  virtual void shrink() = 0;
};

//------------------------------------------------------------------------------
// File System iterator implementation of a in-memory namespace
// Trivial implementation, using the same "logic" to iterate over filesystems
// as we did with "getNumFileSystems" before.
//------------------------------------------------------------------------------
class StupidFileSystemIterator:
  public ICollectionIterator<IFileMD::location_t>
{
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  StupidFileSystemIterator(IFileMD::location_t maxfs) :
    pCurrentFS(0), pMaxFS(maxfs) {}

  //----------------------------------------------------------------------------
  //! Get current fsid
  //----------------------------------------------------------------------------
  IFileMD::location_t getElement() override
  {
    return pCurrentFS;
  }

  //----------------------------------------------------------------------------
  //! Check if iterator is valid
  //----------------------------------------------------------------------------
  bool valid() override
  {
    return pCurrentFS < pMaxFS;
  }

  //----------------------------------------------------------------------------
  //! Retrieve next fsid - returns false when no more filesystems exist
  //----------------------------------------------------------------------------
  void next() override
  {
    if (valid()) {
      pCurrentFS++;
    }
  }

private:
  IFileMD::location_t pCurrentFS;
  IFileMD::location_t pMaxFS;
};

//------------------------------------------------------------------------------
//! Class FileIterator that can iteratoe through a list of files from the
//! FileSystem class. Used to iterate through the files / unlinked files on a
//! filesystem.
//------------------------------------------------------------------------------
class FileIterator:
  public ICollectionIterator<IFileMD::id_t>
{
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  FileIterator(const IFsView::FileList& list) :
    mList(list), mIt(mList.begin()) {}

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  virtual ~FileIterator()  = default;

  //----------------------------------------------------------------------------
  //! Get current file id
  //----------------------------------------------------------------------------
  IFileMD::id_t getElement() override
  {
    return *mIt;
  }

  //----------------------------------------------------------------------------
  //! Check if iterator is valid
  //----------------------------------------------------------------------------
  bool valid() override
  {
    return (mIt != mList.end());
  }

  //----------------------------------------------------------------------------
  //! Retrieve next file id
  //----------------------------------------------------------------------------
  void next() override
  {
    if (valid()) {
      ++mIt;
    }
  }

private:
  const IFsView::FileList& mList; ///< Reference to list
  IFsView::FileList::const_iterator mIt; ///< List iterator
};

EOSNSNAMESPACE_END

#endif // __EOS_NS_IFSVIEW_HH__
