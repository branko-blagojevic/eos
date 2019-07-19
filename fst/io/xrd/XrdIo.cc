//------------------------------------------------------------------------------
// File: XrdIo.cc
// Author: Elvin-Alin Sindrilaru - CERN
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

#include <stdint.h>
#include <cstdlib>
#include "fst/io/xrd/XrdIo.hh"
#include "fst/io/ChunkHandler.hh"
#include "fst/io/VectChunkHandler.hh"
#include "fst/io/AsyncMetaHandler.hh"
#include "common/FileMap.hh"
#include "common/Logging.hh"
#include "XrdCl/XrdClDefaultEnv.hh"
#include "XrdCl/XrdClBuffer.hh"
#include "XrdCl/XrdClConstants.hh"
#include "XrdSfs/XrdSfsInterface.hh"

// Linux compat for Apple
#ifdef __APPLE__
#ifndef EREMOTEIO
#define EREMOTEIO 121
#endif
#endif

EOSFSTNAMESPACE_BEGIN

// Static variables
eos::common::XrdConnPool XrdIo::mXrdConnPool;

namespace
{
std::string getAttrUrl(std::string path)
{
  size_t qfind = path.rfind("?");
  size_t rfind = path.rfind("/", qfind);

  if (rfind != std::string::npos) {
    path.insert(rfind + 1, ".");
  }

  path += ".xattr";
  return path;
}
}

//------------------------------------------------------------------------------
// Handle asynchronous open responses
//------------------------------------------------------------------------------
void
AsyncIoOpenHandler::HandleResponseWithHosts(XrdCl::XRootDStatus* status,
    XrdCl::AnyObject* response,
    XrdCl::HostList* hostList)
{
  eos_info("handling response in AsyncIoOpenHandler");
  delete hostList;

  // Response shoud be nullptr in general
  if (response) {
    delete response;
  }

  mFileIO->mXrdFile->GetProperty("LastURL", mFileIO->mLastTriedUrl);

  if (status->IsOK()) {
    // Store the last URL we are connected after open
    mFileIO->mXrdFile->GetProperty("LastURL", mFileIO->mLastUrl);
    mFileIO->mIsOpen = true;
  }

  mLayoutOpenHandler->HandleResponseWithHosts(status, 0, 0);
  delete this;
}

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
XrdIo::XrdIo(std::string path) :
  FileIo(path, "XrdIo"),
  mDoReadahead(false),
  mNumRdAheadBlocks(InitNumRdAheadBlocks()),
  mDefaultBlocksize(InitBlocksize()),
  mBlocksize(mDefaultBlocksize),
  mXrdFile(NULL),
  mMetaHandler(new AsyncMetaHandler()),
  mXrdIdHelper(nullptr)
{
  // Set the TimeoutResolution to 1
  XrdCl::Env* env = XrdCl::DefaultEnv::GetEnv();
  env->PutInt("TimeoutResolution", 1);
  size_t qpos;

  // Opaque info can be part of the 'path'
  if (((qpos = mFilePath.find("?")) != std::string::npos)) {
    mOpaque = mFilePath.substr(qpos + 1);
  } else {
    mOpaque = "";
  }

  // Set url for xattr requests
  mAttrUrl = getAttrUrl(mFilePath.c_str());
  setAttrSync(false);// by default sync attributes lazyly
  mAttrLoaded = false;
  mAttrDirty = false;
}

//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
XrdIo::~XrdIo()
{
  if (mIsOpen) {
    fileClose();
  }

  while (!mQueueBlocks.empty()) {
    ReadaheadBlock* ptr_readblock = mQueueBlocks.front();
    mQueueBlocks.pop();
    delete ptr_readblock;
  }

  while (!mMapBlocks.empty()) {
    delete mMapBlocks.begin()->second;
    mMapBlocks.erase(mMapBlocks.begin());
  }

  delete mMetaHandler;

  // deal with asynchrnous dirty attributes
  if (!mAttrSync && mAttrDirty) {
    std::string lMap = mFileMap.Trim();

    if (!XrdIo::Upload(mAttrUrl, lMap)) {
      mAttrDirty = false;
    } else {
      eos_static_err("msg=\"unable to upload to remote file map\" url=\"%s\"",
                     mAttrUrl.c_str());
    }
  }

  if (mXrdFile) {
    delete mXrdFile;
  }
}

//------------------------------------------------------------------------------
// Open file - synchronously
//------------------------------------------------------------------------------
int
XrdIo::fileOpen(XrdSfsFileOpenMode flags,
                mode_t mode,
                const std::string& opaque,
                uint16_t timeout)
{
  const char* val = 0;
  std::string lOpaque;
  XrdOucEnv open_opaque(mOpaque.c_str());
  XrdCl::XRootDStatus okstatus;
  mWriteStatus = okstatus;

  // Decide if readahead is used and the block size
  if ((val = open_opaque.Get("fst.readahead")) &&
      (strncmp(val, "true", 4) == 0)) {
    eos_debug("Enabling the readahead.");
    mDoReadahead = true;
    val = 0;

    if ((val = open_opaque.Get("fst.blocksize"))) {
      mBlocksize = static_cast<uint64_t>(atoll(val));
    }

    for (unsigned int i = 0; i < mNumRdAheadBlocks; i++) {
      mQueueBlocks.push(new ReadaheadBlock(mBlocksize));
    }
  }

  // Final path + opaque info used in the open
  std::string request;
  ProcessOpaqueInfo(opaque, request);

  if (mXrdFile) {
    delete mXrdFile;
    mXrdFile = NULL;
  }

  mXrdFile = new XrdCl::File();
  mTargetUrl.FromString(request);
  mXrdIdHelper.reset(new eos::common::XrdConnIdHelper(mXrdConnPool, mTargetUrl));

  if (mXrdIdHelper->HasNewConnection()) {
    eos_info("xrd_connection_id=%s", mTargetUrl.GetHostId().c_str());
  }

  // Disable recovery on read and write
  if (!mXrdFile->SetProperty("ReadRecovery", "false") ||
      !mXrdFile->SetProperty("WriteRecovery", "false")) {
    eos_warning("failed to set XrdCl::File properties read recovery and write "
                "recovery to false");
  }

  XrdCl::OpenFlags::Flags flags_xrdcl = eos::common::LayoutId::MapFlagsSfs2XrdCl(
                                          flags);
  XrdCl::Access::Mode mode_xrdcl = eos::common::LayoutId::MapModeSfs2XrdCl(mode);
  XrdCl::XRootDStatus status = mXrdFile->Open(mTargetUrl.GetURL().c_str(),
                               flags_xrdcl, mode_xrdcl,
                               timeout);
  mXrdFile->GetProperty("LastURL", mLastTriedUrl);

  if (!status.IsOK()) {
    mLastErrMsg = status.ToString().c_str();
    mLastErrCode  = status.code;
    mLastErrNo  = status.errNo;
    eos_err("error= \"open failed url=%s, errno=%i, errc=%i, msg=%s\"",
            mTargetUrl.GetURL().c_str(), mLastErrNo, mLastErrCode, mLastErrMsg.c_str());
    errno = status.errNo;
    return SFS_ERROR;
  } else {
    errno = 0;
    mIsOpen = true;
  }

  // Save the last URL we are connected after open
  mXrdFile->GetProperty("LastURL", mLastUrl);
  return SFS_OK;
}

//------------------------------------------------------------------------------
// Open file asynchronously
//------------------------------------------------------------------------------
int
XrdIo::fileOpenAsync(void* io_handler,
                     XrdSfsFileOpenMode flags, mode_t mode,
                     const std::string& opaque, uint16_t timeout)
{
  const char* val = 0;
  std::string request;
  std::string lOpaque;
  size_t qpos = 0;

  // Opaque info can be part of the 'path'
  if (((qpos = mFilePath.find("?")) != std::string::npos)) {
    lOpaque = mFilePath.substr(qpos + 1);
    //    mFilePath.erase(qpos);
  } else {
    lOpaque = opaque;
  }

  XrdOucEnv open_opaque(lOpaque.c_str());

  // Decide if readahead is used and the block size
  if ((val = open_opaque.Get("fst.readahead")) &&
      (strncmp(val, "true", 4) == 0)) {
    eos_debug("Enabling the readahead.");
    mDoReadahead = true;
    val = 0;

    if ((val = open_opaque.Get("fst.blocksize"))) {
      mBlocksize = static_cast<uint64_t>(atoll(val));
    }

    for (unsigned int i = 0; i < mNumRdAheadBlocks; i++) {
      mQueueBlocks.push(new ReadaheadBlock(mBlocksize));
    }
  }

  request = mFilePath;
  request += "?";
  request += lOpaque;

  if (mXrdFile) {
    delete mXrdFile;
    mXrdFile = NULL;
  }

  mXrdFile = new XrdCl::File();
  mTargetUrl.FromString(request);
  mXrdIdHelper.reset(new eos::common::XrdConnIdHelper(mXrdConnPool, mTargetUrl));

  if (mXrdIdHelper->HasNewConnection()) {
    eos_info("xrd_connection_id=%s", mTargetUrl.GetHostId().c_str());
  }

  // Disable recovery on read and write
  if (!mXrdFile->SetProperty("ReadRecovery", "false") ||
      !mXrdFile->SetProperty("WriteRecovery", "false")) {
    eos_warning("failed to set XrdCl::File properties read recovery and write "
                "recovery to false");
  }

  XrdCl::OpenFlags::Flags flags_xrdcl =
    eos::common::LayoutId::MapFlagsSfs2XrdCl(flags);
  XrdCl::Access::Mode mode_xrdcl = eos::common::LayoutId::MapModeSfs2XrdCl(mode);
  XrdCl::XRootDStatus status =
    mXrdFile->Open(mTargetUrl.GetURL().c_str(), flags_xrdcl, mode_xrdcl,
                   (XrdCl::ResponseHandler*)(io_handler), timeout);

  if (!status.IsOK()) {
    eos_err("error=opening remote XrdClFile");
    errno = status.errNo;
    mLastErrMsg = status.ToString().c_str();
    mLastErrCode  = status.code;
    mLastErrNo  = status.errNo;
    return SFS_ERROR;
  }

  return SFS_OK;
}

//------------------------------------------------------------------------------
// Read from file - sync
//------------------------------------------------------------------------------
int64_t
XrdIo::fileRead(XrdSfsFileOffset offset, char* buffer, XrdSfsXferSize length,
                uint16_t timeout)
{
  eos_debug("offset=%llu length=%llu", static_cast<uint64_t>(offset),
            static_cast<uint64_t>(length));
  uint32_t bytes_read = 0;

  if (!mXrdFile) {
    errno = EIO;
    return SFS_ERROR;
  }

  XrdCl::XRootDStatus status = mXrdFile->Read(static_cast<uint64_t>(offset),
                               static_cast<uint32_t>(length),
                               buffer, bytes_read, timeout);

  if (!status.IsOK()) {
    errno = status.errNo;
    mLastErrMsg = status.ToString().c_str();
    mLastErrCode  = status.code;
    mLastErrNo  = status.errNo;
    return SFS_ERROR;
  }

  return bytes_read;
}

//------------------------------------------------------------------------------
// Read from file - async
//------------------------------------------------------------------------------
int64_t
XrdIo::fileReadAsync(XrdSfsFileOffset offset, char* buffer,
                     XrdSfsXferSize length, bool readahead, uint16_t timeout)
{
  eos_debug("offset=%llu length=%llu",
            static_cast<uint64_t>(offset),
            static_cast<uint64_t>(length));

  if (!mXrdFile) {
    errno = EIO;
    return SFS_ERROR;
  }

  bool done_read = false;
  int64_t nread = 0;
  char* pBuff = buffer;
  XrdCl::XRootDStatus status;

  if (!mDoReadahead) {
    readahead = false;
    eos_debug("Readahead is disabled");
  }

  if (!readahead) {
    return fileRead(offset, pBuff, length, timeout);
  } else {
    eos_debug("readahead enabled, request offset=%lli, length=%i", offset, length);
    uint64_t read_length = 0;
    uint32_t aligned_length;
    uint32_t shift;
    std::map<uint64_t, ReadaheadBlock*>::iterator iter;
    mPrefetchMutex.Lock(); // -->

    while (length) {
      iter = FindBlock(offset);

      if (iter != mMapBlocks.end()) {
        // Block found in prefetched blocks
        SimpleHandler* sh = iter->second->handler;
        shift = offset - iter->first;

        // We can prefetch another block if we still have available blocks in
        // the queue or if first read was from second prefetched block
        if (!mQueueBlocks.empty() || (iter != mMapBlocks.begin())) {
          if (iter != mMapBlocks.begin()) {
            eos_debug("recycle the oldest block");
            mQueueBlocks.push(mMapBlocks.begin()->second);
            mMapBlocks.erase(mMapBlocks.begin());
          }

          eos_debug("prefetch new block(2)");

          if (!PrefetchBlock(offset + mBlocksize, false, timeout)) {
            eos_warning("failed to send prefetch request(2)");
            break;
          }
        }

        if (sh->WaitOK()) {
          eos_debug("block in cache, blk_off=%lld, req_off= %lld", iter->first, offset);

          if (sh->GetRespLength() <= 0) {
            // The request got a response but it read 0 bytes
            eos_debug("response contains 0 bytes");
            done_read = true;
            nread = sh->GetRespLength();
            break;
          }

          aligned_length = sh->GetRespLength() - shift;
          read_length = ((uint32_t) length < aligned_length) ? length : aligned_length;

          // If prefetch block smaller than mBlocksize and current offset at end
          // of the prefetch block then we reached the end of file
          if ((sh->GetRespLength() != mBlocksize) &&
              ((uint64_t) offset >= iter->first + sh->GetRespLength())) {
            done_read = true;
            break;
          }

          pBuff = static_cast<char*>(memcpy(pBuff, iter->second->buffer + shift,
                                            read_length));
          pBuff += read_length;
          offset += read_length;
          length -= read_length;
          nread += read_length;
        } else {
          // Error while prefetching, remove block from map
          mQueueBlocks.push(iter->second);
          mMapBlocks.erase(iter);
          eos_err("error=prefetching failed, disable it and remove block from map");
          mDoReadahead = false;
          break;
        }
      } else {
        // Remove all elements from map so that we can align with the new
        // requests and prefetch a new block. But first we need to collect any
        // responses which are in-flight as otherwise these response might
        // arrive later on, when we are expecting replies for other blocks since
        // we are recycling the SimpleHandler objects.
        while (!mMapBlocks.empty()) {
          SimpleHandler* sh = mMapBlocks.begin()->second->handler;

          if (sh->HasRequest()) {
            // Not interested in the result - discard it
            sh->WaitOK();
          }

          mQueueBlocks.push(mMapBlocks.begin()->second);
          mMapBlocks.erase(mMapBlocks.begin());
        }

        if (!mQueueBlocks.empty()) {
          eos_debug("prefetch new block(1)");

          if (!PrefetchBlock(offset, false, timeout)) {
            eos_err("error=failed to send prefetch request(1)");
            mDoReadahead = false;
            break;
          }
        }
      }
    }

    mPrefetchMutex.UnLock(); // <--

    // If readahead not useful, use the classic way to read
    if (length && !done_read) {
      eos_debug("readahead useless, use the classic way for reading");
      return fileRead(offset, pBuff, length, timeout);
    }
  }

  return nread;
}

//------------------------------------------------------------------------------
// Try to find a block in cache which contains the required offset
//------------------------------------------------------------------------------
PrefetchMap::iterator
XrdIo::FindBlock(uint64_t offset)
{
  if (mMapBlocks.empty()) {
    return mMapBlocks.end();
  }

  PrefetchMap::iterator iter = mMapBlocks.lower_bound(offset);

  if ((iter != mMapBlocks.end()) && (iter->first == offset)) {
    // Found exactly the block needed
    return iter;
  } else {
    if (iter == mMapBlocks.begin()) {
      // Only blocks with bigger offsets, return pointer to end of the map
      return mMapBlocks.end();
    } else {
      // Check if the previous block, we know the map is not empty
      iter--;

      if ((iter->first <= offset) && (offset < (iter->first + mBlocksize))) {
        return iter;
      } else {
        return mMapBlocks.end();
      }
    }
  }
}

//------------------------------------------------------------------------------
// Vector read - sync
//------------------------------------------------------------------------------
int64_t
XrdIo::fileReadV(XrdCl::ChunkList& chunkList, uint16_t timeout)
{
  eos_debug("read count=%i", chunkList.size());
  int64_t nread = 0;

  if (!mXrdFile) {
    errno = EIO;
    return SFS_ERROR;
  }

  XrdCl::VectorReadInfo* vReadInfo = 0;
  XrdCl::XRootDStatus status = mXrdFile->VectorRead(chunkList, 0,
                               vReadInfo, timeout);

  if (!status.IsOK())  {
    errno = status.errNo;
    mLastErrMsg = status.ToString().c_str();
    mLastErrCode  = status.code;
    mLastErrNo  = status.errNo;
    return SFS_ERROR;
  }

  nread = vReadInfo->GetSize();
  delete vReadInfo;
  return nread;
}

//------------------------------------------------------------------------------
// Vector read - async
//------------------------------------------------------------------------------
int64_t
XrdIo::fileReadVAsync(XrdCl::ChunkList& chunkList, uint16_t timeout)
{
  if (!mXrdFile) {
    errno = EIO;
    return SFS_ERROR;
  }

  // Get vector handler and send async request
  VectChunkHandler* vhandler = 0;
  XrdCl::XRootDStatus status;
  eos_debug("read count=%i", chunkList.size());
  vhandler = mMetaHandler->Register(chunkList, NULL, false);

  if (!vhandler) {
    eos_err("unable to get vector handler");
    return SFS_ERROR;
  }

  int64_t nread = vhandler->GetLength();
  status = mXrdFile->VectorRead(chunkList, static_cast<void*>(0),
                                static_cast<XrdCl::ResponseHandler*>(vhandler),
                                timeout);

  if (!status.IsOK()) {
    // TODO: for the time being we call this ourselves but this should be
    // dropped once XrdCl will call the handler for a request as it knows it
    // has already failed
    mMetaHandler->HandleResponse(&status, vhandler);
    mLastErrMsg = status.ToString().c_str();
    mLastErrCode  = status.code;
    mLastErrNo  = status.errNo;
    return SFS_ERROR;
  }

  return nread;
}

//------------------------------------------------------------------------------
// Write to file - sync
//------------------------------------------------------------------------------
int64_t
XrdIo::fileWrite(XrdSfsFileOffset offset, const char* buffer,
                 XrdSfsXferSize length, uint16_t timeout)
{
  eos_debug("offset=%llu length=%llu", static_cast<uint64_t>(offset),
            static_cast<uint64_t>(length));

  if (!mXrdFile) {
    errno = EIO;
    return SFS_ERROR;
  }

  XrdCl::XRootDStatus status = mXrdFile->Write(static_cast<uint64_t>(offset),
                               static_cast<uint32_t>(length),
                               buffer, timeout);

  if (!status.IsOK()) {
    errno = status.errNo;
    mLastErrMsg = status.ToString().c_str();
    mLastErrCode  = status.code;
    mLastErrNo  = status.errNo;
    return SFS_ERROR;
  }

  return length;
}

//------------------------------------------------------------------------------
// Write to file - async
//------------------------------------------------------------------------------
int64_t
XrdIo::fileWriteAsync(XrdSfsFileOffset offset, const char* buffer,
                      XrdSfsXferSize length, uint16_t timeout)
{
  eos_debug("offset=%llu length=%i", static_cast<uint64_t>(offset), length);

  if (!mXrdFile) {
    errno = EIO;
    return SFS_ERROR;
  }

  if (!mWriteStatus.IsOK()) {
    // if there was any async write error, we always return it again
    return SFS_ERROR;
  }

  ChunkHandler* handler = mMetaHandler->Register(offset, length, (char*)buffer,
                          true);

  // If previous write requests failed then we won't get a new handler
  // and we return directly an error
  if (!handler) {
    return SFS_ERROR;
  }

  // Obs: Use the handler buffer for write requests
  XrdCl::XRootDStatus status = mXrdFile->Write(static_cast<uint64_t>(offset),
                               static_cast<uint32_t>(length),
                               handler->GetBuffer(),
                               handler, timeout);

  if (!status.IsOK()) {
    // remember write failures 'forever'
    mWriteStatus = status;
    mMetaHandler->HandleResponse(&status, handler);
    return SFS_ERROR;
  }

  return length;
}

//------------------------------------------------------------------------------
// Wait for async IO
//------------------------------------------------------------------------------
int
XrdIo::fileWaitAsyncIO()
{
  bool async_ok = true;
  {
    XrdSysMutexHelper scope_lock(mPrefetchMutex);

    // Wait for any requests on the fly and then close
    while (!mMapBlocks.empty()) {
      SimpleHandler* shandler = mMapBlocks.begin()->second->handler;

      if (shandler->HasRequest()) {
        async_ok = shandler->WaitOK();
      }

      delete mMapBlocks.begin()->second;
      mMapBlocks.erase(mMapBlocks.begin());
    }
  }

  // Wait for any async requests before closing
  if (mMetaHandler) {
    if (mMetaHandler->WaitOK() != XrdCl::errNone) {
      eos_err("error=async requests failed for file path=%s", mFilePath.c_str());
      async_ok = false;
    }
  }

  if (async_ok) {
    return 0;
  } else {
    errno = EIO;
    return -1;
  }
}

//------------------------------------------------------------------------------
// Truncate file
//------------------------------------------------------------------------------
int
XrdIo::fileTruncate(XrdSfsFileOffset offset, uint16_t timeout)
{
  if (!mXrdFile) {
    errno = EIO;
    return SFS_ERROR;
  }

  XrdCl::XRootDStatus status = mXrdFile->Truncate(static_cast<uint64_t>(offset),
                               timeout);

  if (!status.IsOK()) {
    errno = status.errNo;
    mLastErrMsg = status.ToString().c_str();
    mLastErrCode  = status.code;
    mLastErrNo  = status.errNo;
    return SFS_ERROR;
  }

  return SFS_OK;
}

//------------------------------------------------------------------------------
// Sync file to disk
//------------------------------------------------------------------------------
int
XrdIo::fileSync(uint16_t timeout)
{
  if (!mXrdFile) {
    errno = EIO;
    return SFS_ERROR;
  }

  XrdCl::XRootDStatus status = mXrdFile->Sync(timeout);

  if (!status.IsOK()) {
    errno = status.errNo;
    mLastErrMsg = status.ToString().c_str();
    mLastErrCode  = status.code;
    mLastErrNo  = status.errNo;
    return SFS_ERROR;
  }

  return SFS_OK;
}

//------------------------------------------------------------------------------
// Get stats about the file
//------------------------------------------------------------------------------
int
XrdIo::fileStat(struct stat* buf, uint16_t timeout)
{
  if (!mXrdFile) {
    eos_info("underlying XrdClFile object doesn't exist");
    errno = EIO;
    return SFS_ERROR;
  }

  int rc = SFS_ERROR;
  XrdCl::StatInfo* stat = 0;
  XrdCl::XRootDStatus status = mXrdFile->Stat(true, stat, timeout);

  if (!status.IsOK()) {
    errno = status.errNo;
    mLastErrMsg = status.ToString().c_str();
    mLastErrCode  = status.code;
    mLastErrNo  = status.errNo;
    eos_info("errcode=%i, errno=%i, errmsg=%s", mLastErrCode, mLastErrNo,
             mLastErrMsg.c_str());
  } else {
    buf->st_dev = static_cast<dev_t>(atoi(stat->GetId().c_str()));
    buf->st_mode = static_cast<mode_t>(stat->GetFlags());
    buf->st_size = static_cast<off_t>(stat->GetSize());
    buf->st_mtime = static_cast<time_t>(stat->GetModTime());
    rc = SFS_OK;
  }

  if (stat) {
    delete stat;
  }

  return rc;
}

//------------------------------------------------------------------------------
// Execute implementation dependant commands
//------------------------------------------------------------------------------
int
XrdIo::fileFctl(const std::string& cmd, uint16_t timeout)
{
  if (!mXrdFile) {
    eos_info("underlying XrdClFile object doesn't exist");
    errno = EIO;
    return SFS_ERROR;
  }

  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  (void) arg.FromString(cmd);
  XrdCl::XRootDStatus status = mXrdFile->Fcntl(arg, response, timeout);
  delete response;
  return status.status;
}

//------------------------------------------------------------------------------
// Close file
//------------------------------------------------------------------------------
int
XrdIo::fileClose(uint16_t timeout)
{
  if (!mXrdFile) {
    errno = EIO;
    return SFS_ERROR;
  }

  XrdCl::XRootDStatus okstatus;
  mWriteStatus = okstatus;
  bool async_ok = true;
  mIsOpen = false;

  if (fileWaitAsyncIO()) {
    async_ok = false;
  }

  XrdCl::XRootDStatus status = mXrdFile->Close(timeout);

  if (!status.IsOK()) {
    errno = status.errNo;
    mLastErrMsg = status.ToString().c_str();
    mLastErrCode  = status.code;
    mLastErrNo  = status.errNo;
    return SFS_ERROR;
  }

  // If any of the async requests failed then we have an error
  if (!async_ok) {
    return SFS_ERROR;
  }

  return SFS_OK;
}

//------------------------------------------------------------------------------
// Remove file
//------------------------------------------------------------------------------
int
XrdIo::fileRemove(uint16_t timeout)
{
  if (!mXrdFile) {
    errno = EIO;
    return SFS_ERROR;
  }

  // Send opaque coamand to file object to mark it for deletion
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  (void) arg.FromString("delete");
  XrdCl::XRootDStatus status = mXrdFile->Fcntl(arg, response, timeout);
  delete response;

  if (!status.IsOK()) {
    eos_err("failed to mark the file for deletion:%s", mFilePath.c_str());
    return SFS_ERROR;
  }

  return SFS_OK;
}

//------------------------------------------------------------------------------
// Check for existence
//------------------------------------------------------------------------------
int
XrdIo::fileExists()
{
  XrdCl::URL xUrl(mFilePath);
  XrdCl::FileSystem fs(xUrl);
  XrdCl::StatInfo* stat;
  XrdCl::XRootDStatus status = fs.Stat(xUrl.GetPath(), stat);
  errno = 0;

  if (!status.IsOK()) {
    if (status.errNo == kXR_NotFound) {
      errno = ENOENT;
      mLastErrMsg = "no such file or directory";
      mLastErrCode  = status.code;
      mLastErrNo  = status.errNo;
    } else {
      errno = EIO;
      mLastErrMsg = "failed to check for existence";
      mLastErrCode  = status.code;
      mLastErrNo  = status.errNo;
    }

    return SFS_ERROR;
  }

  if (stat) {
    delete stat;
    return SFS_OK;
  } else {
    errno = ENODATA;
    return SFS_ERROR;
  }
}

//------------------------------------------------------------------------------
// Delete file by path
//------------------------------------------------------------------------------
int
XrdIo::fileDelete(const char* url)
{
  XrdCl::URL xUrl(url);
  std::string attrurl = getAttrUrl(url);
  XrdCl::URL xAttrUrl(attrurl);
  XrdCl::FileSystem fs(xUrl);
  XrdCl::XRootDStatus status = fs.Rm(xUrl.GetPath());
  XrdCl::XRootDStatus status_attr = fs.Rm(xAttrUrl.GetPath());
  errno = 0;

  if (!status.IsOK()) {
    eos_err("error=failed to delete file - %s", url);
    mLastErrMsg = "failed to delete file";
    mLastErrCode  = status.code;
    mLastErrNo  = status.errNo;
    errno = EIO;
    return SFS_ERROR;
  }

  return true;
}

//------------------------------------------------------------------------------
// Clean read cache
//------------------------------------------------------------------------------
void
XrdIo::CleanReadCache()
{
  fileWaitAsyncIO();

  if (mQueueBlocks.empty()) {
    for (unsigned int i = 0; i < mNumRdAheadBlocks; i++) {
      mQueueBlocks.push(new ReadaheadBlock(mBlocksize));
    }
  }
}

//------------------------------------------------------------------------------
// Prefetch block using the readahead mechanism
//------------------------------------------------------------------------------
bool
XrdIo::PrefetchBlock(int64_t offset, bool isWrite, uint16_t timeout)
{
  bool done = true;
  ReadaheadBlock* block = NULL;
  eos_debug("try to prefetch with offset: %lli, length: %lu",
            offset, mBlocksize);

  if (!mQueueBlocks.empty()) {
    block = mQueueBlocks.front();
    mQueueBlocks.pop();
  } else {
    done = false;
    return done;
  }

  if (FindBlock(offset) != mMapBlocks.end()) {
    // Block is already prefetched
    return true;
  }
  
  block->handler->Update(offset, mBlocksize, isWrite);
  XrdCl::XRootDStatus status = mXrdFile->Read(offset, mBlocksize, block->buffer,
                               block->handler, timeout);

  if (!status.IsOK()) {
    // Create tmp status which is deleted in the HandleResponse method
    XrdCl::XRootDStatus* tmp_status = new XrdCl::XRootDStatus(status);
    block->handler->HandleResponse(tmp_status, NULL);
    mQueueBlocks.push(block);
    done = false;
  } else {
    mMapBlocks.insert(std::make_pair(offset, block));
  }

  return done;
}

//------------------------------------------------------------------------------
// Get pointer to async meta handler object
//------------------------------------------------------------------------------
void*
XrdIo::fileGetAsyncHandler()
{
  return static_cast<void*>(mMetaHandler);
}

//------------------------------------------------------------------------------
// Run a space query command as statfs
//------------------------------------------------------------------------------
int
XrdIo::Statfs(struct statfs* sfs)
{
  XrdCl::URL xUrl(mFilePath);
  XrdCl::FileSystem fs(xUrl);
  XrdCl::Buffer* response = 0;
  XrdCl::Buffer arg(xUrl.GetPath().size());
  arg.FromString(xUrl.GetPath());
  XrdCl::XRootDStatus status = fs.Query(XrdCl::QueryCode::Space, arg,
                                        response, (uint16_t) 15);
  errno = 0;

  if (!status.IsOK()) {
    eos_err("msg=\"failed to statfs remote XRootD\" url=\"%s\"", mFilePath.c_str());
    mLastErrMsg = "failed to statfs remote XRootD";
    mLastErrCode  = status.code;
    mLastErrNo  = status.errNo;
    errno = EREMOTEIO;
    return errno;
  }

  if (response) {
    // oss.cgroup=default&oss.space=469799256416256&oss.free=468894771826688&
    // oss.maxf=68719476736&oss.used=904484589568&oss.quota=469799256416256
    XrdOucEnv spaceEnv(response->ToString().c_str());
    unsigned long long free_bytes = 0;
    unsigned long long total_bytes = 0;

    if (spaceEnv.Get("oss.free")) {
      free_bytes = strtoull(spaceEnv.Get("oss.free"), 0, 10);
    } else {
      errno = EINVAL;
      return errno;
    }

    if (spaceEnv.Get("oss.space")) {
      total_bytes = strtoull(spaceEnv.Get("oss.space"), 0, 10);
    } else {
      errno = EINVAL;
      return errno;
    }

#ifdef __APPLE__
    sfs->f_iosize = 4096;
    sfs->f_bsize = sfs->f_iosize;
    sfs->f_blocks = (fsblkcnt_t)(total_bytes / sfs->f_iosize);
    sfs->f_bavail = (fsblkcnt_t)(free_bytes / sfs->f_iosize);
#else
    sfs->f_frsize = 4096;
    sfs->f_bsize = sfs->f_frsize;
    sfs->f_blocks = (fsblkcnt_t)(total_bytes / sfs->f_frsize);
    sfs->f_bavail = (fsblkcnt_t)(free_bytes / sfs->f_frsize);
#endif
    sfs->f_bfree = sfs->f_bavail;
    sfs->f_files = 1000000;
    sfs->f_ffree = 1000000;
    delete response;
    return 0;
  } else {
    errno = EREMOTEIO;
    return errno;
  }
}

//------------------------------------------------------------------------------
//                      **** Attribute Interface ****
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// Set attr
//------------------------------------------------------------------------------
int
XrdIo::attrSet(const char* name, const char* value, size_t len)
{
  if (!mAttrSync && mAttrLoaded) {
    std::string key = name;
    std::string val;
    val.assign(value, len);

    if (val == "#__DELETE_ATTR_#") {
      mFileMap.Remove(key);
    } else {
      // just modify
      mFileMap.Set(key, val);
    }

    mAttrDirty = true;
    return 0;
  }

  std::string lBlob;

  // download
  if (!XrdIo::Download(mAttrUrl, lBlob) || errno == ENOENT) {
    mAttrLoaded = true;

    if (mFileMap.Load(lBlob)) {
      std::string key = name;
      std::string val;

      if (val == "#__DELETE_ATTR_#") {
        mFileMap.Remove(key);
      } else {
        val.assign(value, len);
        mFileMap.Set(key, val);
      }

      mAttrDirty = true;

      if (mAttrSync) {
        std::string lMap = mFileMap.Trim();

        if (!XrdIo::Upload(mAttrUrl, lMap)) {
          mAttrDirty = false;
          return SFS_OK;
        } else {
          eos_static_err("msg=\"unable to upload to remote file map\" url=\"%s\"",
                         mAttrUrl.c_str());
        }
      }
    } else {
      eos_static_err("msg=\"unable to parse remote file map\" url=\"%s\"",
                     mAttrUrl.c_str());
      errno = EINVAL;
    }
  } else {
    eos_static_err("msg=\"unable to download remote file map\" url=\"%s\"",
                   mAttrUrl.c_str());
  }

  return SFS_ERROR;
}

//------------------------------------------------------------------------------
// Set a string attribute (name has to start with 'user.' !!!)
//------------------------------------------------------------------------------
int
XrdIo::attrSet(string name, std::string value)
{
  return attrSet(name.c_str(), value.c_str(), value.length());
}

//------------------------------------------------------------------------------
// Get a binary attribute by name (name has to start with 'user.' !!!)
//------------------------------------------------------------------------------
int
XrdIo::attrGet(const char* name, char* value, size_t& size)
{
  errno = 0;

  if (!mAttrSync && mAttrLoaded) {
    std::string val = mFileMap.Get(name);
    size_t len = val.length() + 1;

    if (len > size) {
      len = size;
    }

    memcpy(value, val.c_str(), len);
    eos_static_info("key=%s value=%s", name, value);
    return 0;
  }

  std::string lBlob;

  if (!XrdIo::Download(mAttrUrl, lBlob) || errno == ENOENT) {
    mAttrLoaded = true;

    if (mFileMap.Load(lBlob)) {
      std::string val = mFileMap.Get(name);
      size_t len = val.length() + 1;

      if (len > size) {
        len = size;
      }

      memcpy(value, val.c_str(), len);
      eos_static_info("key=%s value=%s", name, value);
      return SFS_OK;
    }
  } else {
    eos_static_err("msg=\"unable to download remote file map\" url=\"%s\"",
                   mAttrUrl.c_str());
  }

  return SFS_ERROR;
}

///------------------------------------------------------------------------------
// Get a string attribute by name (name has to start with 'user.' !!!)
//------------------------------------------------------------------------------
int
XrdIo::attrGet(string name, std::string& value)
{
  errno = 0;

  if (!mAttrSync && mAttrLoaded) {
    value = mFileMap.Get(name);
    return SFS_OK;
  }

  std::string lBlob;

  if (!XrdIo::Download(mAttrUrl, lBlob) || errno == ENOENT) {
    mAttrLoaded = true;

    if (mFileMap.Load(lBlob)) {
      value = mFileMap.Get(name);
      return SFS_OK;
    }
  } else {
    eos_static_err("msg=\"unable to download remote file map\" url=\"%s\"",
                   mAttrUrl.c_str());
  }

  return SFS_ERROR;
}

//------------------------------------------------------------------------------
// Delete a binary attribute by name
//------------------------------------------------------------------------------
int
XrdIo::attrDelete(const char* name)
{
  errno = 0;
  return attrSet(name, "#__DELETE_ATTR_#");
}

//------------------------------------------------------------------------------
// List all attributes for the associated path
//------------------------------------------------------------------------------
int
XrdIo::attrList(std::vector<std::string>& list)
{
  if (!mAttrSync && mAttrLoaded) {
    std::map<std::string, std::string> lMap = mFileMap.GetMap();

    for (auto it = lMap.begin(); it != lMap.end(); ++it) {
      list.push_back(it->first);
    }

    return 0;
  }

  std::string lBlob;

  if (!XrdIo::Download(mAttrUrl, lBlob) || errno == ENOENT) {
    mAttrLoaded = true;

    if (mFileMap.Load(lBlob)) {
      std::map<std::string, std::string> lMap = mFileMap.GetMap();

      for (auto it = lMap.begin(); it != lMap.end(); ++it) {
        list.push_back(it->first);
      }

      return 0;
    }
  } else {
    eos_static_err("msg=\"unable to download remote file map\" url=\"%s\"",
                   mAttrUrl.c_str());
  }

  return -1;
}

//--------------------------------------------------------------------------
//          **** Traversing filesystem/storage routines ****
//--------------------------------------------------------------------------

//--------------------------------------------------------------------------
// Open a cursor to traverse a storage system
//--------------------------------------------------------------------------
FileIo::FtsHandle*
XrdIo::ftsOpen()
{
  XrdCl::URL url(mFilePath.c_str());
  XrdCl::FileSystem fs(url);
  std::vector<std::string> files;
  std::vector<std::string> directories;
  XrdCl::XRootDStatus status =
    XrdIo::GetDirList(&fs, url, &files, &directories);

  if (!status.IsOK()) {
    eos_err("error=listing remote XrdClFile - %s", status.ToString().c_str());
    errno = status.errNo;
    mLastErrMsg = status.ToString().c_str();
    mLastErrCode  = status.code;
    mLastErrNo  = status.errNo;
    return 0;
  }

  FtsHandle* handle = new FtsHandle(mFilePath.c_str());

  if (!handle) {
    return 0;
  }

  for (auto it = files.begin(); it != files.end(); ++it) {
    XrdOucString fname = it->c_str();

    // Skip attribute files
    if (fname.beginswith(".") && fname.endswith(".xattr")) {
      continue;
    }

    handle->found_files.push_back(mFilePath + *it);
  }

  for (auto it = directories.begin(); it != directories.end(); ++it) {
    eos_info("adding dir=%s deepness=%d", (mFilePath + *it + "/").c_str(),
             handle->deepness);
    handle->found_dirs[0].push_back(mFilePath + *it + "/");
  }

  return (FileIo::FtsHandle*)(handle);
}

//------------------------------------------------------------------------------
// Return the next path related to a traversal cursor obtained with ftsOpen
//------------------------------------------------------------------------------
std::string
XrdIo::ftsRead(FileIo::FtsHandle* fts_handle)
{
  FtsHandle* handle = (FtsHandle*) fts_handle;

  if (!handle->found_files.size()) {
    do {
      XrdCl::XRootDStatus status;
      std::vector<std::string> files;
      std::vector<std::string> directories;
      auto dit = handle->found_dirs[handle->deepness].begin();
      bool found = true;

      while (dit == handle->found_dirs[handle->deepness].end()) {
        // move to next level
        handle->deepness++;
        handle->found_dirs.resize(handle->deepness + 1);

        if (!handle->found_dirs[handle->deepness].size()) {
          found = false;
          break;
        } else {
          dit = handle->found_dirs[handle->deepness].begin();
        }
      }

      if (!found) {
        break;
      }

      eos_info("searching at deepness=%d directory=%s", handle->deepness,
               dit->c_str());
      std::string surl_dir = *dit;
      XrdCl::URL url(surl_dir);
      XrdCl::FileSystem fs(url);
      status = XrdIo::GetDirList(&fs, url, &files, &directories);

      if (!status.IsOK()) {
        eos_err("error=listing remote XrdClFile - %s", status.ToString().c_str());
        errno = status.errNo;
        mLastErrMsg = status.ToString().c_str();
        mLastErrCode  = status.code;
        mLastErrNo  = status.errNo;
        return "";
      } else {
        handle->found_dirs[handle->deepness].erase(dit);
      }

      std::string new_file{""};
      std::string new_dir{""};

      for (auto it = files.begin(); it != files.end(); ++it) {
        XrdOucString fname = it->c_str();

        if (fname.beginswith(".") && fname.endswith(".xattr")) {
          continue;
        }

        new_file = surl_dir + *it;
        eos_info("adding file=%s", new_file.c_str());
        handle->found_files.push_back(new_file);
      }

      for (auto it = directories.begin(); it != directories.end(); ++it) {
        new_dir = surl_dir + *it + "/";
        eos_info("adding dir=%s deepness=%d", new_dir.c_str(),
                 handle->deepness + 1);
        handle->found_dirs[handle->deepness + 1].push_back(new_dir);
      }
    } while (!handle->found_files.size());
  }

  if (handle->found_files.size()) {
    std::string new_path = handle->found_files.front();
    handle->found_files.pop_front();
    return new_path;
  }

  return "";
}

//------------------------------------------------------------------------------
// Close a traversal cursor
//------------------------------------------------------------------------------
int
XrdIo::ftsClose(FileIo::FtsHandle* fts_handle)
{
  FtsHandle* handle = (FtsHandle*) fts_handle;
  handle->found_files.clear();
  handle->found_dirs.resize(1);
  handle->found_dirs[0].resize(1);
  handle->deepness = 0;
  return 0;
}

//------------------------------------------------------------------------------
// Download a remote file into a string object
//------------------------------------------------------------------------------
int
XrdIo::Download(std::string url, std::string& download)
{
  errno = 0;
  static int s_blocksize = 65536;
  XrdIo io(url.c_str());
  off_t offset = 0;
  std::string opaque;

  if (!io.fileOpen(0, 0, opaque, 10)) {
    ssize_t rbytes = 0;
    download.resize(s_blocksize);

    do {
      rbytes = io.fileRead(offset, (char*) download.c_str(), s_blocksize, 30);

      if (rbytes == s_blocksize) {
        download.resize(download.size() + 65536);
      }

      if (rbytes > 0) {
        offset += rbytes;
      }
    } while (rbytes == s_blocksize);

    io.fileClose();
    download.resize(offset);
    return 0;
  }

  if (errno == 3011) {
    return 0;
  }

  return -1;
}

//------------------------------------------------------------------------------
// Upload a string object into a remote file
//------------------------------------------------------------------------------
int
XrdIo::Upload(std::string url, std::string& upload)
{
  errno = 0;
  XrdIo io(url.c_str());
  std::string opaque;
  int rc = 0;

  if (!io.fileOpen(SFS_O_WRONLY | SFS_O_CREAT, S_IRWXU | S_IRGRP | SFS_O_MKPTH,
                   opaque, 10)) {
    eos_static_info("opened %s", url.c_str());

    if ((io.fileWrite(0, upload.c_str(), upload.length(),
                      30)) != (ssize_t) upload.length()) {
      eos_static_err("failed to write %d", upload.length());
      rc = -1;
    } else {
      eos_static_info("uploaded %d\n", upload.length());
    }

    io.fileClose();
  } else {
    eos_static_err("failed to open %s", url.c_str());
    rc = -1;
  }

  return rc;
}

//------------------------------------------------------------------------------
// Get a list of files and a list of directories inside a remote directory
//------------------------------------------------------------------------------
XrdCl::XRootDStatus
XrdIo::GetDirList(XrdCl::FileSystem* fs, const XrdCl::URL& url,
                  std::vector<std::string>* files,
                  std::vector<std::string>* directories)
{
  eos_info("url=%s", url.GetURL().c_str());
  using namespace XrdCl;
  DirectoryList* list;
  XrdCl::XRootDStatus status;
  status = fs->DirList(url.GetPath(), DirListFlags::Stat, list);

  if (!status.IsOK()) {
    return status;
  }

  for (DirectoryList::Iterator it = list->Begin(); it != list->End(); ++it) {
    if ((*it)->GetStatInfo()->TestFlags(StatInfo::IsDir)) {
      std::string directory = (*it)->GetName();
      directories->push_back(directory);
    } else {
      std::string file = (*it)->GetName();
      files->push_back(file);
    }
  }

  return XRootDStatus();
}

//------------------------------------------------------------------------------
// Process opaque info
//------------------------------------------------------------------------------
void
XrdIo::ProcessOpaqueInfo(const std::string& opaque, std::string& out) const
{
  using namespace std::chrono;
  // Add extra capability expiration time based on the XRD_STREAMTIMEOUT value
  uint64_t xrdcl_streamtimeout = XrdCl::DefaultStreamTimeout;
  std::string env_val;

  if (XrdCl::DefaultEnv::GetEnv()->GetString("StreamTimeout", env_val)) {
    try {
      xrdcl_streamtimeout = std::stoull(env_val);
    } catch (...) {}
  }

  auto now = system_clock::now();
  auto valid_sec = time_point_cast<seconds>(now).time_since_epoch().count()
                   + xrdcl_streamtimeout - 1;
  std::ostringstream oss;
  oss << mFilePath;

  // This could already contain opaque info
  if (mFilePath.find('?') == std::string::npos) {
    oss << '?';
  } else {
    oss << '&';
  }

  oss << "fst.valid=" << valid_sec;

  if (opaque.length()) {
    oss << '&' << opaque;
  }

  out = oss.str();
}

EOSFSTNAMESPACE_END
