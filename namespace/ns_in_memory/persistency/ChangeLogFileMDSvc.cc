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
// desc:   Change log based FileMD service
//------------------------------------------------------------------------------

#include "ChangeLogFileMDSvc.hh"
#include "ChangeLogContainerMDSvc.hh"
#include "ChangeLogConstants.hh"
#include "common/ShellCmd.hh"
#include "common/Parallel.hh"
#include "namespace/Constants.hh"
#include "namespace/utils/Locking.hh"
#include "namespace/utils/ThreadUtils.hh"
#include "namespace/ns_in_memory/FileMD.hh"
#include "namespace/ns_in_memory/persistency/ChangeLogContainerMDSvc.hh"
#include "XrdSys/XrdSysTimer.hh"

#include <algorithm>
#include <utility>
#include <set>
#include <features.h>
#if __GNUC_PREREQ(4,8)
#include <atomic>
#endif

//------------------------------------------------------------------------------
// Follower
//------------------------------------------------------------------------------
namespace eos
{
class FileMDFollower: public eos::ILogRecordScanner
{
public:
  FileMDFollower(eos::ChangeLogFileMDSvc* fileSvc):
    pFileSvc(fileSvc)
  {
    pContSvc    = pFileSvc->pContSvc;
    pQuotaStats = pFileSvc->pQuotaStats;
  }

  virtual void publishOffset(uint64_t offset)
  {
    pFileSvc->setFollowOffset(offset);
  }

  // Unpack new data and put it in the queue
  virtual bool processRecord(uint64_t offset, char type,
                             const eos::Buffer& buffer)
  {
    // Update
    if (type == UPDATE_RECORD_MAGIC) {
      std::shared_ptr<IFileMD> file = std::make_shared<FileMD>(0, pFileSvc);
      file->deserialize((Buffer&)buffer);
      FileMap::iterator it = pUpdated.find(file->getId());

      if (file->getId() >= pFileSvc->pFirstFreeId) {
        pFileSvc->pFirstFreeId = file->getId() + 1;
      }

      if (it != pUpdated.end()) {
        it->second.file   = file;
        it->second.offset = offset;
      } else {
        pUpdated[file->getId()] = FileHelper(offset, file);
      }
    }
    // Deletion
    else if (type == DELETE_RECORD_MAGIC) {
      IFileMD::id_t id;
      buffer.grabData(0, &id, sizeof(IFileMD::id_t));
      FileMap::iterator it = pUpdated.find(id);

      if (it != pUpdated.end()) {
        pUpdated.erase(it);
      }

      pDeleted.insert(id);
    }

    return true;
  }

  // Try to commit the data in the queue to the service
  void commit()
  {
    pFileSvc->getSlaveLock()->writeLock();
    ChangeLogFileMDSvc::IdMap*      fileIdMap = &pFileSvc->pIdMap;
    ChangeLogContainerMDSvc::IdMap* contIdMap = &pContSvc->pIdMap;
    ChangeLogContainerMDSvc::DeletionSet* deletionSet =
      &pContSvc->pFollowerDeletions;
    // Handle deletions
    std::set<IFileMD::id_t>::iterator itD;

    for (itD = pDeleted.begin(); itD != pDeleted.end(); ++itD) {
      // We don't have the file, nothing to delete
      ChangeLogFileMDSvc::IdMap::iterator it;
      it = fileIdMap->find(*itD);

      if (it == fileIdMap->end()) {
        continue;
      }

      // We have the file but we need to check if we have a corresponding
      // container
      std::shared_ptr<IFileMD> currentFile = it->second.ptr;
      ChangeLogContainerMDSvc::IdMap::iterator itP;
      itP = contIdMap->find(currentFile->getContainerId());

      if ((itP != contIdMap->end()) || (!currentFile->getContainerId())) {
        // We need to check whether the pointer of the file we're trying
        // to delete is actually the same as the pointer of the file
        // attached to the parent container under the same file name.
        // It may happen that the pointers differ if there was a name
        // conflict
        // Additionally we have to deal with detached files e.g.
        // containerId=0.
        if (currentFile->getContainerId()) {
          std::shared_ptr<IContainerMD> container = itP->second.ptr;
          std::shared_ptr<IFileMD> existingFile = container->findFile(
              currentFile->getName());

          if (existingFile.get() == currentFile.get()) {
            container->removeFile(currentFile->getName());
            IQuotaNode* node = getQuotaNode(container.get());

            if (node) {
              node->removeFile(currentFile.get());
            }
          }
        }

        // If the file was not attached to the container it's safe to
        // remove it, if it had been attached it has been detached by
        // the code above.
        handleReplicas(currentFile.get(), 0);
        fileIdMap->erase(it);
        IFileMDChangeListener::Event e(currentFile.get(),
                                       IFileMDChangeListener::Deleted);
        pFileSvc->notifyListeners(&e);
      }
    }

    pDeleted.clear();
    // Handle updates
    FileMap::iterator itU;
    std::list<IFileMD::id_t> processed;

    for (itU = pUpdated.begin(); itU != pUpdated.end(); ++itU) {
      ChangeLogFileMDSvc::IdMap::iterator it;
      ChangeLogContainerMDSvc::IdMap::iterator itP;
      std::shared_ptr<IFileMD> currentFile = itU->second.file;
      uint64_t  currentOffset = itU->second.offset;
      it = fileIdMap->find(currentFile->getId());

      // It's a new file
      if (it == fileIdMap->end()) {
        // We register it only if we have a corresponding container,
        // otherwise it will need to wait for the next commit and hope that
        // the container has been inserted
        itP = contIdMap->find(currentFile->getContainerId());

        if (itP != contIdMap->end()) {
          // We check if the file with the given name already exists in
          // the container, if it does, we have a name conflict in which
          // case we attach the new file and remove the old one
          std::shared_ptr<IContainerMD> container = itP->second.ptr;
          std::shared_ptr<IFileMD> existingFile   = container->findFile(
                currentFile->getName());
          IQuotaNode* node = getQuotaNode(container.get());

          if (existingFile) {
            if (node) {
              node->removeFile(existingFile.get());
            }

            container->removeFile(existingFile->getName());
          }

          container->addFile(currentFile.get());
          (*fileIdMap)[currentFile->getId()] =
            ChangeLogFileMDSvc::DataInfo(currentOffset, currentFile);
          IFileMDChangeListener::Event e(currentFile.get(),
                                         IFileMDChangeListener::Created);
          pFileSvc->notifyListeners(&e);
          handleReplicas(0, currentFile.get());

          if (node) {
            node->addFile(currentFile.get());
          }

          processed.push_back(currentFile->getId());
        } else {
          if (deletionSet->count(currentFile->getContainerId()) ||
              (!currentFile->getContainerId())) {
            //----------------------------------------------------------------
            // It might happen that the parent container has been lost already
            // e.g. rm -r /cont/ and the files appear detached on the slave.
            // In this case we have to create the file entry to avoid
            // that files appear as orphaned and don't get deleted on FSTs
            // after a slave=>master promotion
            //----------------------------------------------------------------
            (*fileIdMap)[currentFile->getId()] =
              ChangeLogFileMDSvc::DataInfo(currentOffset, currentFile);
            IFileMDChangeListener::Event e(currentFile.get(),
                                           IFileMDChangeListener::Created);
            pFileSvc->notifyListeners(&e);
            handleReplicas(0, currentFile.get());
            processed.push_back(currentFile->getId());
          }
        }
      }
      // It's an update
      else {
        // We have the file already but it might have changed the parent
        // container or might have been unlinked, so we need to check it up
        // in its original container
        std::shared_ptr<IFileMD>  originalFile = it->second.ptr;
        std::shared_ptr<IContainerMD> originalContainer;
        ChangeLogContainerMDSvc::IdMap::iterator itP;
        itP = contIdMap->find(originalFile->getContainerId());

        // If the container does not exist it means that the file was
        // either an orphan or was detached due to a conflict and it's
        // container has been removed
        if (itP != contIdMap->end()) {
          originalContainer = itP->second.ptr;
        }

        // The parent container did not change
        bool readd = false;

        if (originalFile->getContainerId() == currentFile->getContainerId()) {
          if (originalContainer) {
            std::shared_ptr<IFileMD> existingFile =
              originalContainer->findFile(originalFile->getName());

            if (existingFile &&
                existingFile->getId() == originalFile->getId()) {
              // Update quota
              IQuotaNode* node = getQuotaNode(originalContainer.get());

              if (node) {
                node->removeFile(existingFile.get());
                node->addFile(currentFile.get());
              }

              // Rename
              originalContainer->removeFile(originalFile->getName());
              existingFile->setName(currentFile->getName());
              readd = true;
            }
          }

          handleReplicas(originalFile.get(), currentFile.get());
          // Cast to derived class implementation to avoid "slicing" of info
          auto tmp_orig = dynamic_cast<eos::FileMD*>(originalFile.get());
          auto tmp_curr = dynamic_cast<eos::FileMD*>(currentFile.get());

          if (tmp_orig && tmp_curr) {
            *tmp_orig = *tmp_curr;
          } else {
            fprintf(stderr, "error: FileMD dynamic cast failed\n");
            exit(1);
          }

          originalFile->setFileMDSvc(pFileSvc);

          if (originalContainer && readd) {
            originalContainer->addFile(originalFile.get());
          }

          it->second.logOffset = currentOffset;
          processed.push_back(currentFile->getId());
          IFileMDChangeListener::Event e(originalFile.get(),
                                         IFileMDChangeListener::Updated);
          pFileSvc->notifyListeners(&e);
        }
        // The parent container changed
        else {
          // Check if the new container exists, if it doesn't we need
          // to wait for it, so we do nothing for the time being
          ChangeLogContainerMDSvc::IdMap::iterator itPN;
          itPN = contIdMap->find(currentFile->getContainerId());

          if ((itPN == contIdMap->end()) && (currentFile->getContainerId())) {
            continue;
          }

          // If the file is present in the original container, we remove
          // it from there and update the quota
          if (originalContainer) {
            std::shared_ptr<IFileMD> existingFile =
              originalContainer->findFile(originalFile->getName());

            if (existingFile &&
                existingFile->getId() == originalFile->getId()) {
              IQuotaNode* node = getQuotaNode(originalContainer.get());

              if (node) {
                node->removeFile(existingFile.get());
              }

              originalContainer->removeFile(existingFile->getName());
            }
          }

          // Update the file and handle the replicas
          handleReplicas(originalFile.get(), currentFile.get());
          // Cast to derived class implementation to avoid "slicing" of info
          auto tmp_orig = dynamic_cast<eos::FileMD*>(originalFile.get());
          auto tmp_curr = dynamic_cast<eos::FileMD*>(currentFile.get());

          if (tmp_orig && tmp_curr) {
            *tmp_orig = *tmp_curr;
          } else {
            fprintf(stderr, "error: FileMD dynamic cast failed\n");
            exit(1);
          }

          originalFile->setFileMDSvc(pFileSvc);
          it->second.logOffset = currentOffset;

          // The file was unlinked so our job is done
          if (originalFile->getContainerId() == 0) {
            processed.push_back(originalFile->getId());
            IFileMDChangeListener::Event e(originalFile.get(),
                                           IFileMDChangeListener::Updated);
            pFileSvc->notifyListeners(&e);
          }
          // The file has moved
          else {
            // We check if the file with the given name already exists in
            // the container, if it does, we have a name conflict in which
            // case we attach the new file and remove the old one
            std::shared_ptr<IContainerMD> newContainer = itPN->second.ptr;
            IQuotaNode* node           = getQuotaNode(newContainer.get());
            std::shared_ptr<IFileMD> existingFile = newContainer->findFile(
                originalFile->getName());

            if (existingFile) {
              if (node) {
                node->removeFile(existingFile.get());
              }

              newContainer->removeFile(existingFile->getName());
            }

            newContainer->addFile(originalFile.get());

            if (node) {
              node->addFile(originalFile.get());
            }

            processed.push_back(originalFile->getId());
            IFileMDChangeListener::Event e(originalFile.get(),
                                           IFileMDChangeListener::Updated);
            pFileSvc->notifyListeners(&e);
          }
        }
      }
    }

    // Clear processed updates and leave the remaining ones for the next cycle
    std::list<FileMD::id_t>::iterator itPro;

    for (itPro = processed.begin(); itPro != processed.end(); ++itPro) {
      pUpdated.erase(*itPro);
    }

    pFileSvc->setFollowPending(pUpdated.size());
    pContSvc->getSlaveLock()->unLock();
  }

private:

  //------------------------------------------------------------------------
  // Get quota node id concerning given container
  //------------------------------------------------------------------------
  IQuotaNode* getQuotaNode(const IContainerMD* container)
  {
    // Initial sanity check
    if (!container) {
      return 0;
    }

    if (!pQuotaStats) {
      return 0;
    }

    // Search for the node
    const IContainerMD* current = container;

    try {
      while ((current->getId() != 1) &&
             ((current->getFlags() & QUOTA_NODE_FLAG) == 0) &&
             current->getParentId()) {
        current = pContSvc->getContainerMD(current->getParentId()).get();
      }
    } catch (MDException& e) {
      // The corresponding container is not there (yet).
      // We catch this exception and accept this extremely rare condition and resulting miscounting
      // since the logic to wait for the container to arrive is difficult to implement at this stage.
      return 0;
    }

    // We have either found a quota node or reached root without finding one
    // so we need to double check whether the current container has an
    // associated quota node
    if ((current->getFlags() & QUOTA_NODE_FLAG) == 0) {
      return 0;
    }

    IQuotaNode* node = pQuotaStats->getQuotaNode(current->getId());

    if (node) {
      return node;
    }

    return pQuotaStats->registerNewNode(current->getId());
  }

  //------------------------------------------------------------------------
  // Generate the replica handling events
  //------------------------------------------------------------------------
  void handleReplicas(eos::IFileMD* file1, eos::IFileMD* file2)
  {
    // Prepare
    if (file1 == file2) {
      return;
    }

    if (file1 && file2 && file1->getId() != file2->getId()) {
      return;
    }

    IFileMD* file = 0;

    if (file1) {
      file = file1;
    }

    if (file2) {
      file = file2;
    }

    if (!file1) {
      file1 = new FileMD(0, 0);
    }

    if (!file2) {
      file2 = new FileMD(0, 0);
    }

    std::set<FileMD::location_t> toBeUnlinked;
    std::set<FileMD::location_t> toBeRemoved;
    std::set<FileMD::location_t> toBeAdded;
    // Check if there is any replicas to be added
    IFileMD::LocationVector::const_iterator it;
    IFileMD::LocationVector loc_vect2 = file2->getLocations();

    for (it = loc_vect2.begin(); it != loc_vect2.end(); ++it) {
      if (!file1->hasLocation(*it)) {
        toBeAdded.insert(*it);
      }
    }

    // Check if there is any replicas to be unlinked
    IFileMD::LocationVector loc_vect1 = file1->getLocations();

    for (it = loc_vect1.begin(); it != loc_vect1.end(); ++it) {
      if (!file2->hasLocation(*it)) {
        toBeUnlinked.insert(*it);
      }
    }

    IFileMD::LocationVector unlink_vect2 = file2->getUnlinkedLocations();

    for (it = unlink_vect2.begin();  it != unlink_vect2.end(); ++it) {
      if (!file1->hasUnlinkedLocation(*it)) {
        toBeUnlinked.insert(*it);
      }
    }

    // Check if there is any replicas to be removed
    IFileMD::LocationVector unlink_vect1 = file1->getUnlinkedLocations();

    for (it = unlink_vect1.begin(); it != unlink_vect1.end(); ++it) {
      if (!file2->hasUnlinkedLocation(*it)) {
        toBeRemoved.insert(*it);
      }
    }

    std::set<FileMD::location_t>::iterator itS;

    for (itS = toBeUnlinked.begin(); itS != toBeUnlinked.end(); ++itS) {
      if (!file2->hasUnlinkedLocation(*itS)) {
        toBeRemoved.insert(*itS);
      }
    }

    // Commit additions
    if (file2->getId() == 0) {
      file->clearLocations();
      file->clearUnlinkedLocations();
    }

    for (itS = toBeAdded.begin(); itS != toBeAdded.end(); ++itS) {
      IFileMDChangeListener::Event e(file,
                                     IFileMDChangeListener::LocationAdded,
                                     *itS);
      pFileSvc->notifyListeners(&e);
    }

    // Commit unlinks
    for (itS = toBeUnlinked.begin(); itS != toBeUnlinked.end(); ++itS) {
      IFileMDChangeListener::Event e(file,
                                     IFileMDChangeListener::LocationUnlinked,
                                     *itS);
      pFileSvc->notifyListeners(&e);
    }

    // Commit removals
    for (itS = toBeRemoved.begin(); itS != toBeRemoved.end(); ++itS) {
      IFileMDChangeListener::Event e(file,
                                     IFileMDChangeListener::LocationRemoved,
                                     *itS);
      pFileSvc->notifyListeners(&e);
    }

    // Cleanup
    if (file1->getId() == 0) {
      delete file1;
    }

    if (file2->getId() == 0) {
      delete file2;
    }
  }

  //--------------------------------------------------------------------------
  // File Helper
  //--------------------------------------------------------------------------
  struct FileHelper {
    FileHelper(): offset(0), file((IFileMD*)0) {}
    FileHelper(uint64_t _offset, std::shared_ptr<eos::IFileMD> _file):
      offset(_offset), file(_file) {}
    uint64_t      offset;
    std::shared_ptr<eos::IFileMD> file;
  };

  typedef std::map<eos::IFileMD::id_t, FileHelper> FileMap;
  FileMap                       pUpdated;
  std::set<eos::IFileMD::id_t>  pDeleted;
  eos::ChangeLogFileMDSvc*      pFileSvc;
  eos::ChangeLogContainerMDSvc* pContSvc;
  eos::IQuotaStats*             pQuotaStats;
};
}

extern "C"
{
  //----------------------------------------------------------------------------
  // Follow the change log
  //----------------------------------------------------------------------------
  static void* followerThread(void* data)
  {
    eos::ThreadUtils::blockAIOSignals();
    eos::ChangeLogFileMDSvc* fileSvc = reinterpret_cast<eos::ChangeLogFileMDSvc*>
                                       (data);
    uint64_t                 offset  = fileSvc->getFollowOffset();
    eos::ChangeLogFile*      file    = fileSvc->getChangeLog();
    uint32_t                 pollInt = fileSvc->getFollowPollInterval();
    eos::FileMDFollower f(fileSvc);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, 0);

    while (1) {
      pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, 0);
      offset = file->follow(&f, offset);
      fileSvc->setFollowOffset(offset);
      f.commit();
      fileSvc->setFollowOffset(offset);
      pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, 0);
      file->wait(pollInt);
    }

    return 0;
  }
}

//------------------------------------------------------------------------------
// Helper structures for online compacting
//------------------------------------------------------------------------------
namespace
{
//------------------------------------------------------------------------------
// Store info about old and new offset for a given file id
//------------------------------------------------------------------------------
struct RecordData {
  RecordData(): offset(0), newOffset(0), fileId(0) {}
  RecordData(uint64_t o, eos::IFileMD::id_t i, uint64_t no = 0):
    offset(o), newOffset(no), fileId(i) {}
  uint64_t          offset;
  uint64_t          newOffset;
  eos::IFileMD::id_t fileId;
};

//------------------------------------------------------------------------------
// Carry the data between compacting stages
//------------------------------------------------------------------------------
struct CompactingData {
  //---------------------------------------------------------------------------
  // Constructor
  //---------------------------------------------------------------------------
  CompactingData():
    newLog(new eos::ChangeLogFile()),
    originalLog(0),
    newRecord(0)
  {}

  //---------------------------------------------------------------------------
  // Destructor
  //---------------------------------------------------------------------------
  ~CompactingData()
  {
    delete newLog;
  }

  std::string              logFileName;
  eos::ChangeLogFile*      newLog;
  eos::ChangeLogFile*      originalLog;
  std::vector<RecordData>  records;
  uint64_t                 newRecord;
};

//------------------------------------------------------------------------------
// Compare record data objects in order to sort them
//------------------------------------------------------------------------------
struct OffsetComparator {
  bool operator()(const RecordData& a, const RecordData& b)
  {
    return a.offset < b.offset;
  }
};

//------------------------------------------------------------------------------
// Process the records being scanned and copy them to the new log
//------------------------------------------------------------------------------
class UpdateHandler: public eos::ILogRecordScanner
{
public:

  //------------------------------------------------------------------------
  // Constructor
  //------------------------------------------------------------------------
  UpdateHandler(std::map<eos::IFileMD::id_t, RecordData>& updates,
                eos::ChangeLogFile*                      newLog):
    pUpdates(updates), pNewLog(newLog) {}

  //------------------------------------------------------------------------
  // Process the records
  //------------------------------------------------------------------------
  virtual bool processRecord(uint64_t           offset,
                             char               type,
                             const eos::Buffer& buffer)
  {
    // Write to the new change log - we need to cast - nasty, but safe in
    // this case
    uint64_t newOffset = pNewLog->storeRecord(type, (eos::Buffer&)buffer);
    // Put the right stuff in the updates map
    eos::IFileMD::id_t id;
    buffer.grabData(0, &id, sizeof(eos::IFileMD::id_t));

    if (type == eos::UPDATE_RECORD_MAGIC) {
      pUpdates[id] = RecordData(offset, id, newOffset);
    } else if (type == eos::DELETE_RECORD_MAGIC) {
      pUpdates.erase(id);
    }

    return true;
  }

private:
  std::map<eos::IFileMD::id_t, RecordData>& pUpdates;
  eos::ChangeLogFile*                      pNewLog;
};
}

namespace eos
{
//------------------------------------------------------------------------
// Initizlize the file service
//------------------------------------------------------------------------
void ChangeLogFileMDSvc::initialize()
{
  pIdMap.resize(pResSize);

  if (!pContSvc) {
    MDException e(EINVAL);
    e.getMessage() << "FileMDSvc: container service not set";
    throw e;
  }

  // Decide on how to open the change log
  int logOpenFlags = 0;

  if (pSlaveMode) {
    if (!pSlaveLock) {
      MDException e(EINVAL);
      e.getMessage() << "FileMDSvc: slave lock not set";
      throw e;
    }

    logOpenFlags = ChangeLogFile::ReadOnly;
  } else {
    logOpenFlags = ChangeLogFile::Create | ChangeLogFile::Append;
  }

  // Rescan the change log if needed
  //
  // In the master mode we go through the entire file
  // In the slave mode up until the compaction mark or not at all
  // if the compaction mark is not present
  pChangeLog->open(pChangeLogPath, logOpenFlags, FILE_LOG_MAGIC);
  bool logIsCompacted = (pChangeLog->getUserFlags() & LOG_FLAG_COMPACTED);
  pFollowStart = pChangeLog->getFirstOffset();

  if (!pSlaveMode || logIsCompacted) {
    FileMDScanner scanner(pIdMap, pSlaveMode);
    pChangeLog->mmap();
    pFollowStart = pChangeLog->scanAllRecords(&scanner);
    pFirstFreeId = scanner.getLargestId() + 1;
    time_t start_time = time(0);
    time_t now = start_time;
    uint64_t end = pIdMap.size();
#if __GNUC_PREREQ(4,8)
    std::atomic_ulong cnt(0);
    int nthread = std::thread::hardware_concurrency();

    if (pIdMap.size() / nthread && getenv("EOS_NS_BOOT_PARALLEL")) {
      fprintf(stderr, "INFO     [ doing parallel boot ]\n");
      // Recreate the files
      std::mutex critical;
      size_t chunk = pIdMap.size() / nthread;
      size_t last_chunk = chunk + pIdMap.size() - (chunk * nthread);

      if (!(pIdMap.size() / nthread)) {
        nthread = 1;
        last_chunk = chunk = pIdMap.size();
      }

      eos::common::Parallel::For(0, nthread , [&](int i) {
        IdMap::iterator it = pIdMap.begin();
        std::advance(it, i * chunk);
        time_t now = time(NULL);
        size_t progress = 0;

        for (size_t n = 0; n < ((i == (nthread - 1)) ? last_chunk : chunk); ++n) {
          cnt++;
          //------------------------------------------------------------------
          // Unpack the serialized buffers
          //------------------------------------------------------------------
          std::shared_ptr<IFileMD> file = std::make_shared<FileMD>(0, this);
          file->deserialize(*it->second.buffer);
          it->second.ptr = file;
          delete it->second.buffer;
          it->second.buffer = 0;
          uint64_t lcnt = cnt.load();

          if ((!i) && ((100.0 * lcnt / end) > progress)) {
            //std::lock_guard<std::mutex> lock(critical);
            now = time(NULL);
            double estimate = (1 + end - lcnt) / ((1.0 * lcnt / (now + 1 - start_time)));

            if (progress == 0) {
              fprintf(stderr, "PROGRESS [ load %-64s ] %02u%% estimate none \n", "file-load",
                      (unsigned int) progress);
            } else {
              fprintf(stderr, "PROGRESS [ load %-64s ] %02u%% estimate %3.01fs "
                      "[ %lus/%.0fs ] [%lu/%lu]\n", "file-load",
                      (unsigned int) progress, estimate,  time(NULL) - start_time,
                      (double) time(NULL) - (double) start_time + estimate, lcnt, end);
            }

            progress += 2;
          }

          ++it;
        }
      });
      pChangeLog->munmap();
      IdMap::iterator it;
      start_time = time(0);
      uint64_t gcnt = 0;
      size_t progress = 0;

      for (it = pIdMap.begin(); it != pIdMap.end(); ++it) {
        std::shared_ptr<IFileMD> file = it->second.ptr;
        gcnt++;
        ListenerList::iterator lit;

        for (lit = pListeners.begin(); lit != pListeners.end(); ++lit) {
          (*lit)->fileMDRead(file.get());
        }

        if ((100.0 * gcnt / end) > progress) {
          now = time(NULL);
          double estimate = (1 + end - gcnt) / ((1.0 * gcnt / (now + 1 - start_time)));

          if (progress == 0) {
            fprintf(stderr, "PROGRESS [ load %-64s ] %02u%% estimate none \n",
                    "file-notify", (unsigned int) progress);
          } else {
            fprintf(stderr,
                    "PROGRESS [ load %-64s ] %02u%% estimate %3.01fs  [ %lus/%.0fs ] [%lu/%lu]\n",
                    "file-notify", (unsigned int) progress, estimate,  time(NULL) - start_time,
                    (double) time(NULL) - (double) start_time + estimate, gcnt, end);
          }

          progress += 2;
        }
      }

      cnt.store(0);
      std::mutex c_critical[256];
      start_time = time(NULL);
      eos::common::Parallel::For(0, nthread , [&](int i) {
        IdMap::iterator it = pIdMap.begin();
        std::advance(it, i * chunk);
        time_t now = time(NULL);
        size_t progress = 0;

        for (size_t n = 0; n < ((i == (nthread - 1)) ? last_chunk : chunk); ++n) {
          std::shared_ptr<IFileMD> file = it->second.ptr;
          cnt++;

          // Attach to the hierarchy
          if (file->getContainerId() == 0) {
            ++it;
            continue;
          }

          std::shared_ptr<IContainerMD> cont;

          try {
            cont = pContSvc->getContainerMD(file->getContainerId());
          } catch (MDException& e) {}

          if (!cont) {
            std::lock_guard<std::mutex> lock(critical);

            if (!pSlaveMode) {
              attachBroken("orphans", file.get());
            }

            ++it;
            continue;
          }

          std::lock_guard<std::mutex> lock(c_critical[cont->getId() % 256]);

          if (cont->findFile(file->getName())) {
            std::lock_guard<std::mutex> lock(critical);

            if (!pSlaveMode) {
              attachBroken("name_conflicts", file.get());
            }

            ++it;
            continue;
          } else {
            cont->addFile(file.get());
          }

          uint64_t lcnt = cnt.load();

          if ((i == (nthread - 1)) && ((100.0 * lcnt / end) > progress)) {
            now = time(NULL);
            double estimate = (1 + end - lcnt) / ((1.0 * lcnt / (now + 1 - start_time)));

            if (progress == 0) {
              fprintf(stderr, "PROGRESS [ load %-64s ] %02u%% estimate none \n",
                      "file-attach", (unsigned int) progress);
            } else {
              fprintf(stderr,
                      "PROGRESS [ load %-64s ] %02u%% estimate %3.01fs  [ %lus/%.0fs ] [%lu/%lu]\n",
                      "file-attach", (unsigned int) progress, estimate,  time(NULL) - start_time,
                      (double) time(NULL) - (double) start_time + estimate, lcnt, end);
            }

            progress += 2;
          }

          ++it;
        }
      });
    } else
#endif
    {
      // Recreate the files
      IdMap::iterator it;

      for (it = pIdMap.begin(); it != pIdMap.end(); ++it) {
        // Unpack the serialized buffers
        std::shared_ptr<IFileMD> file = std::make_shared<FileMD>(0, this);
        file->deserialize(*it->second.buffer);
        it->second.ptr = file;
        delete it->second.buffer;
        it->second.buffer = 0;
        ListenerList::iterator it;

        for (it = pListeners.begin(); it != pListeners.end(); ++it) {
          (*it)->fileMDRead(file.get());
        }

        // Attach to the hierarchy
        if (file->getContainerId() == 0) {
          continue;
        }

        std::shared_ptr<IContainerMD> cont;

        try {
          cont = pContSvc->getContainerMD(file->getContainerId());
        } catch (MDException& e) {}

        if (!cont) {
          if (!pSlaveMode) {
            attachBroken("orphans", file.get());
          }

          continue;
        }

        if (cont->findFile(file->getName())) {
          if (!pSlaveMode) {
            attachBroken("name_conflicts", file.get());
          }

          continue;
        } else {
          cont->addFile(file.get());
        }
      }
    }
  }

  if (!pSlaveMode && !logIsCompacted) {
    // If we have a new changelog file in master mode we add the compaction mark
    pChangeLog->addCompactionMark();
  }
}

//------------------------------------------------------------------------------
// Make a transition from slave to master
//------------------------------------------------------------------------------
void ChangeLogFileMDSvc::slave2Master(
  std::map<std::string, std::string>& config)
{
  // Find the new changelog path
  std::map<std::string, std::string>::iterator it;
  it = config.find("changelog_path");

  if (it == config.end()) {
    MDException e(EINVAL);
    e.getMessage() << "changelog_path not specified" ;
    throw e;
  }

  if (it->second == pChangeLogPath) {
    MDException e(EINVAL);
    e.getMessage() << "changelog_path must differ from the original ";
    e.getMessage() << "changelog_path";
    throw e;
  }

  // Copy the current changelog file to the previous name
  std::string tmpChangeLogPath     = pChangeLogPath;
  tmpChangeLogPath += ".tmp";
  std::string currentChangeLogPath = pChangeLogPath;
  std::string copyCmd = "cp -f ";
  copyCmd += currentChangeLogPath.c_str();
  copyCmd += " ";
  copyCmd += tmpChangeLogPath.c_str();

  if (getenv("EOS_MGM_CP_ON_FAILOVER")) {
    eos::common::ShellCmd scmd(copyCmd);
    eos::common::cmd_status rc = scmd.wait(60);

    if (rc.exit_code) {
      MDException e(EIO) ;
      e.getMessage() << "Failed to copy the current change log file <";
      e.getMessage() << pChangeLogPath << ">";
    }
  }

  // Redefine the valid changelog path
  pChangeLogPath = it->second;

  // Rename the current changelog file to the new file name
  if (rename(currentChangeLogPath.c_str(), pChangeLogPath.c_str())) {
    MDException e(EINVAL);
    e.getMessage() << "Failed to rename changelog file from <";
    e.getMessage() << currentChangeLogPath << "> to <" << pChangeLogPath;
    throw e;
  }

  if (getenv("EOS_MGM_CP_ON_FAILOVER")) {
    // Rename the temp changelog file to the new file name
    if (rename(tmpChangeLogPath.c_str(), currentChangeLogPath.c_str())) {
      MDException e(EINVAL);
      e.getMessage() << "Failed to rename changelog file from <";
      e.getMessage() << tmpChangeLogPath << "> to <" << currentChangeLogPath;
      throw e;
    }
  }

  // Stop the follower thread
  stopSlave();
  // Reopen changelog file in writable mode = close + open (append)
  pChangeLog->close() ;
  int logOpenFlags = ChangeLogFile::Create | ChangeLogFile::Append;
  pChangeLog->open(pChangeLogPath, logOpenFlags, FILE_LOG_MAGIC);
}

//------------------------------------------------------------------------------
// Switch the namespace to read-only mode
//------------------------------------------------------------------------------
void ChangeLogFileMDSvc::makeReadOnly()
{
  pChangeLog->close() ;
  int logOpenFlags = ChangeLogFile::ReadOnly;
  pChangeLog->open(pChangeLogPath, logOpenFlags, FILE_LOG_MAGIC);
}

//------------------------------------------------------------------------------
// Configure the file service
//------------------------------------------------------------------------------
void ChangeLogFileMDSvc::configure(
  const std::map<std::string, std::string>& config)
{
  // Configure the changelog
  std::map<std::string, std::string>::const_iterator it;
  it = config.find("changelog_path");

  if (it == config.end()) {
    MDException e(EINVAL);
    e.getMessage() << "changelog_path not specified" ;
    throw e;
  }

  pChangeLogPath = it->second;
  // Check whether we should run in the slave mode
  it = config.find("slave_mode");

  if (it != config.end() && it->second == "true") {
    pSlaveMode = true;
    int32_t pollInterval = 1000;
    it = config.find("poll_interval_us");

    if (it != config.end()) {
      pollInterval = strtol(it->second.c_str(), 0, 0);

      if (pollInterval == 0) {
        pollInterval = 1000;
      }
    }
  }

  it = config.find("ns_size");

  if (it != config.end()) {
    pResSize = strtoull(it->second.c_str(), 0, 10);
  }

  if (getenv("EOS_NS_COMPRESSION")) {
    if (std::string(getenv("EOS_NS_COMPRESSION")) == "true") {
      it = config.find("dictionary_path");

      if (it != config.end()) {
        pChangeLog->setDictionary(it->second);
      } else {
        MDException e(EINVAL);
        e.getMessage() << "FileMD dictionary_path not specified" ;
        throw e;
      }
    }
  }
}

//------------------------------------------------------------------------------
// Finalize the file service
//------------------------------------------------------------------------------
void ChangeLogFileMDSvc::finalize()
{
  pChangeLog->close();
  pIdMap.clear();
}

//------------------------------------------------------------------------------
// Get the file metadata information for the given file ID
//------------------------------------------------------------------------------
std::shared_ptr<IFileMD>
ChangeLogFileMDSvc::getFileMD(IFileMD::id_t id)
{
  IdMap::iterator it = pIdMap.find(id);

  if (it == pIdMap.end()) {
    MDException e(ENOENT);
    e.getMessage() << "File #" << id << " not found";
    throw e;
  }

  if (it->second.ptr == NULL) {
    MDException e(ENOENT);
    e.getMessage() << "File #" << id << " not found in map";
    throw e;
  }

  it->second.ptr->setFileMDSvc(this);
  return it->second.ptr;
}

//------------------------------------------------------------------------------
// Create new file metadata object
//------------------------------------------------------------------------------
std::shared_ptr<IFileMD> ChangeLogFileMDSvc::createFile()
{
  std::shared_ptr<IFileMD> file = std::make_shared<FileMD>(pFirstFreeId++, this);
  pIdMap.insert(std::make_pair(file->getId(), DataInfo(0, file)));
  IFileMDChangeListener::Event e(file.get(), IFileMDChangeListener::Created);
  notifyListeners(&e);
  return file;
}

//------------------------------------------------------------------------------
// Update the file metadata
//------------------------------------------------------------------------------
void ChangeLogFileMDSvc::updateStore(IFileMD* obj)
{
  // Find the object in the map
  IdMap::iterator it = pIdMap.find(obj->getId());

  if (it == pIdMap.end()) {
    MDException e(ENOENT);
    e.getMessage() << "File #" << obj->getId() << " not found. ";
    e.getMessage() << "The object was not created in this store!";
    throw e;
  }

  // Store the file in the changelog and notify the listener
  eos::Buffer buffer;
  obj->serialize(buffer);
  it->second.logOffset = pChangeLog->storeRecord(eos::UPDATE_RECORD_MAGIC,
                         buffer);
  IFileMDChangeListener::Event e(obj, IFileMDChangeListener::Updated);
  notifyListeners(&e);
}

//------------------------------------------------------------------------------
// Remove object from the store
//------------------------------------------------------------------------------
void ChangeLogFileMDSvc::removeFile(IFileMD* obj)
{
  // Find the object in the map
  IFileMD::id_t fileId = obj->getId();
  IdMap::iterator it = pIdMap.find(fileId);

  if (it == pIdMap.end()) {
    MDException e(ENOENT);
    e.getMessage() << "File #" << fileId << " not found. ";
    e.getMessage() << "The object was not created in this store!";
    throw e;
  }

  // Store the file in the changelog and notify the listener
  eos::Buffer buffer;
  buffer.putData(&fileId, sizeof(FileMD::id_t));
  pChangeLog->storeRecord(eos::DELETE_RECORD_MAGIC, buffer);
  IFileMDChangeListener::Event e(obj, IFileMDChangeListener::Deleted);
  notifyListeners(&e);
  pIdMap.erase(it);
}

//------------------------------------------------------------------------------
// Add file listener
//------------------------------------------------------------------------------
void ChangeLogFileMDSvc::addChangeListener(IFileMDChangeListener* listener)
{
  pListeners.push_back(listener);
}

//------------------------------------------------------------------------------
// Visit all the files
//------------------------------------------------------------------------------
void ChangeLogFileMDSvc::visit(IFileVisitor* visitor)
{
  IdMap::iterator it;
  time_t start_time = time(0);
  time_t now = start_time;
  uint64_t cnt = 0;
  uint64_t end = pIdMap.size();
  size_t progress = 0;

  for (it = pIdMap.begin(); it != pIdMap.end(); ++it) {
    cnt++;
    visitor->visitFile(it->second.ptr.get());

    if ((100.0 * cnt / end) > progress) {
      now = time(NULL);
      double estimate = (1 + end - cnt) / ((1.0 * cnt / (now + 1 - start_time)));

      if (progress == 0) {
        fprintf(stderr, "PROGRESS [ scan %-64s ] %02u%% estimate none \n",
                "file-visit", (unsigned int)progress);
      } else {
        fprintf(stderr, "PROGRESS [ scan %-64s ] %02u%% estimate %3.01fs  "
                "[ %lus/%.0fs ] [%lu/%lu]\n", "file-visit",
                (unsigned int)progress, estimate, time(NULL) - start_time,
                (double)time(NULL) - (double)start_time + estimate, cnt, end);
      }

      progress += 10;
    }
  }

  now = time(NULL);
  fprintf(stderr, "ALERT    [ %-64s ] finnished in %ds\n", "file-visit",
          (int)(now - start_time));
}

//------------------------------------------------------------------------------
// Scan the changelog and put the appropriate data in the lookup table
//------------------------------------------------------------------------------
bool ChangeLogFileMDSvc::FileMDScanner::processRecord(uint64_t      offset,
    char          type,
    const Buffer& buffer)
{
  // Update
  if (type == UPDATE_RECORD_MAGIC) {
    static int cnt = 0;
    cnt++;
    IFileMD::id_t id;
    buffer.grabData(0, &id, sizeof(IFileMD::id_t));
    DataInfo& d = pIdMap[id];
    d.logOffset = offset;

    if (!d.buffer) {
      d.buffer = new Buffer(0);
    }

    (*d.buffer) = buffer;

    if (pLargestId < id) {
      pLargestId = id;
    }
  }
  // Deletion
  else if (type == DELETE_RECORD_MAGIC) {
    IFileMD::id_t id;
    buffer.grabData(0, &id, sizeof(IFileMD::id_t));
    IdMap::iterator it = pIdMap.find(id);

    if (it != pIdMap.end()) {
      delete it->second.buffer;
      pIdMap.erase(it);
    }

    if (pLargestId < id) {
      pLargestId = id;
    }
  }
  // Compaction mark - we stop scanning here
  else if (type == COMPACT_STAMP_RECORD_MAGIC) {
    fprintf(stderr, "INFO     [ found file compaction mark at offset=%lu ] \n",
            offset);

    if (pSlaveMode) {
      return false;
    }
  }

  return true;
}

//------------------------------------------------------------------------------
// Prepare for online compacting.
//------------------------------------------------------------------------------
void* ChangeLogFileMDSvc::compactPrepare(const std::string& newLogFileName)
{
  // Try to open a new log file for writing
  ::CompactingData* data = new ::CompactingData();

  try {
    data->newLog->open(newLogFileName, ChangeLogFile::Create,
                       FILE_LOG_MAGIC);
    data->logFileName = newLogFileName;
    data->originalLog = pChangeLog;
    data->newRecord   = pChangeLog->getNextOffset();
  } catch (MDException& e) {
    delete data;
    throw;
  }

  // Shrink the pIdMap
  pIdMap.resize(0);
  // Get the list of records
  IdMap::const_iterator it;

  for (it = pIdMap.begin(); it != pIdMap.end(); ++it) {
    if (it->second.logOffset) {
      data->records.push_back(::RecordData(it->second.logOffset, it->first));
    }
  }

  return data;
}

//------------------------------------------------------------------------------
// Do the compacting.
//------------------------------------------------------------------------------
void ChangeLogFileMDSvc::compact(void*& compactingData)
{
  // Sort the records to avoid random seeks
  ::CompactingData* data = (::CompactingData*)compactingData;

  if (!data) {
    MDException e(EINVAL);
    e.getMessage() << "Compacting data incorrect" ;
    throw e;
  }

  std::sort(data->records.begin(), data->records.end(),
            ::OffsetComparator());

  //--------------------------------------------------------------------------
  // Copy the records to the new file
  //--------------------------------------------------------------------------
  try {
    std::vector<RecordData>::iterator it;

    for (it = data->records.begin(); it != data->records.end(); ++it) {
      Buffer  buff;
      uint8_t type;
      type = data->originalLog->readRecord(it->offset, buff);
      it->newOffset = data->newLog->storeRecord(type, buff);
    }
  } catch (MDException& e) {
    data->newLog->close();
    delete data;
    compactingData = 0;
    throw;
  }
}

//------------------------------------------------------------------------------
// Commit the compacting information.
//------------------------------------------------------------------------------
void ChangeLogFileMDSvc::compactCommit(void* compactingData, bool autorepair)
{
  ::CompactingData* data = (::CompactingData*)compactingData;

  if (!data) {
    MDException e(EINVAL);
    e.getMessage() << "Compacting data incorrect" ;
    throw e;
  }

  //--------------------------------------------------------------------------
  // Copy the part of the old log that has been appended after we
  // prepared
  //--------------------------------------------------------------------------
  std::map<eos::IFileMD::id_t, RecordData> updates;

  try {
    ::UpdateHandler updateHandler(updates, data->newLog);
    data->originalLog->scanAllRecordsAtOffset(&updateHandler,
        data->newRecord,
        autorepair);
  } catch (MDException& e) {
    data->newLog->close();
    delete data;
    throw;
  }

  //--------------------------------------------------------------------------
  // Looks like we're all good and we won't be throwing any exceptions any
  // more so we may get to updating the in-memory structures.
  //
  // We start with the originally copied records
  //--------------------------------------------------------------------------
  uint64_t fileCounter = 0;
  IdMap::iterator it;
  std::vector<RecordData>::iterator itO;

  for (itO = data->records.begin(); itO != data->records.end(); ++itO) {
    // Check if we still have the file, if not, it must have been deleted
    // so we don't care
    it = pIdMap.find(itO->fileId);

    if (it == pIdMap.end()) {
      continue;
    }

    // If the original offset does not match it means that we must have
    // be updated later, if not we've messed up so we die in order not
    // to lose data
    assert(it->second.logOffset >= itO->offset);

    if (it->second.logOffset == itO->offset) {
      it->second.logOffset = itO->newOffset;
      ++fileCounter;
    }
  }

  // Now we handle updates, if we don't have the file, we're messed up,
  // if the original offsets don't match we're messed up too
  std::map<IFileMD::id_t, RecordData>::iterator itU;

  for (itU = updates.begin(); itU != updates.end(); ++itU) {
    it = pIdMap.find(itU->second.fileId);
    assert(it != pIdMap.end());
    assert(it->second.logOffset == itU->second.offset);
    it->second.logOffset = itU->second.newOffset;
    ++fileCounter;
  }

  assert(fileCounter == pIdMap.size());
  // Replace the logs
  pChangeLog = data->newLog;
  pChangeLog->addCompactionMark();
  pChangeLogPath = data->logFileName;
  data->newLog = 0;
  data->originalLog->close();
  delete data;
}

//------------------------------------------------------------------------------
// Start the slave
//------------------------------------------------------------------------------
void ChangeLogFileMDSvc::startSlave()
{
  if (!pSlaveMode) {
    MDException e(errno);
    e.getMessage() << "ContainerMDSvc: not in slave mode";
    throw e;
  }

  if (pthread_create(&pFollowerThread, 0, followerThread, this) != 0) {
    MDException e(errno);
    e.getMessage() << "ContainerMDSvc: unable to start the slave follower: ";
    e.getMessage() << strerror(errno);
    throw e;
  }

  pSlaveStarted = true;
}

//------------------------------------------------------------------------------
// Stop the slave mode
//------------------------------------------------------------------------------
void ChangeLogFileMDSvc::stopSlave()
{
  if (!pSlaveMode) {
    MDException e(errno);
    e.getMessage() << "ContainerMDSvc: not in slave mode";
    throw e;
  }

  if (!pSlaveStarted) {
    MDException e(errno);
    e.getMessage() << "ContainerMDSvc: the slave follower is not started";
    throw e;
  }

  if (pthread_cancel(pFollowerThread) != 0) {
    MDException e(errno);
    e.getMessage() << "ContainerMDSvc: unable to cancel the slave follower: ";
    e.getMessage() << strerror(errno);
    throw e;
  }

  if (pthread_join(pFollowerThread, 0) != 0) {
    MDException e(errno);
    e.getMessage() << "ContainerMDSvc: unable to join the slave follower: ";
    e.getMessage() << strerror(errno);
    throw e;
  }

  pSlaveStarted = false;
  pSlaveMode = false;
  pFollowerThread = 0;
}

//------------------------------------------------------------------------------
// Attach a broken file to lost+found
//------------------------------------------------------------------------------
void ChangeLogFileMDSvc::attachBroken(const std::string& parent,
                                      IFileMD*            file)
{
  std::ostringstream s1, s2;
  std::shared_ptr<IContainerMD> parentCont = pContSvc->getLostFoundContainer(
        parent);
  s1 << file->getContainerId();
  std::shared_ptr<IContainerMD> cont = parentCont->findContainer(s1.str());

  if (!cont) {
    cont = pContSvc->createInParent(s1.str(), parentCont.get());
  }

  s2 << file->getName() << "." << file->getId();
  file->setName(s2.str());
  cont->addFile(file);
}

//------------------------------------------------------------------------------
// Get changelog warning messages
//------------------------------------------------------------------------------
std::vector<std::string>
ChangeLogFileMDSvc::getWarningMessages()
{
  return pChangeLog->getWarningMessages();
}

//------------------------------------------------------------------------------
// Clear changelog warning messages
//------------------------------------------------------------------------------
void
ChangeLogFileMDSvc::clearWarningMessages()
{
  pChangeLog->clearWarningMessages();
}

//------------------------------------------------------------------------------
// Set container service
//------------------------------------------------------------------------------
void
ChangeLogFileMDSvc::setContMDService(IContainerMDSvc* cont_svc)
{
  auto tmp_cont_svc = dynamic_cast<eos::ChangeLogContainerMDSvc*>(cont_svc);

  if (tmp_cont_svc) {
    pContSvc = tmp_cont_svc;
  }
}

//------------------------------------------------------------------------------
// Set the QuotaStats object for the follower
//------------------------------------------------------------------------------
void
ChangeLogFileMDSvc::setQuotaStats(IQuotaStats* quota_stats)
{
  pQuotaStats = quota_stats;
}

}
