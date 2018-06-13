//------------------------------------------------------------------------------
//! @file xrdclproxy.cc
//! @author Andreas-Joachim Peters CERN
//! @brief XrdCl proxy class
//------------------------------------------------------------------------------

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

#include "xrdclproxy.hh"
#include "common/Logging.hh"
#include "common/Path.hh"
#include "XrdCl/XrdClXRootDResponses.hh"

using namespace XrdCl;


XrdCl::Proxy::chunk_vector XrdCl::Proxy::sTimeoutWriteAsyncChunks;
XrdCl::Proxy::chunk_rvector XrdCl::Proxy::sTimeoutReadAsyncChunks;
XrdSysMutex XrdCl::Proxy::sTimeoutAsyncChunksMutex;
ssize_t XrdCl::Proxy::sChunkTimeout = 300;

XrdCl::BufferManager XrdCl::Proxy::sWrBufferManager;
XrdCl::BufferManager XrdCl::Proxy::sRaBufferManager;

/* -------------------------------------------------------------------------- */
XRootDStatus
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::Write( uint64_t         offset,
                    uint32_t         size,
                    const void      *buffer,
                    ResponseHandler *handler,
                    uint16_t         timeout)
/* -------------------------------------------------------------------------- */
{
  eos_debug("offset=%lu size=%u", offset, size);
  XRootDStatus status = WaitOpen();
  if (!status.IsOK())
    return status;
  return File::Write(offset, size, buffer, handler, timeout);
}

/* -------------------------------------------------------------------------- */
XRootDStatus
XrdCl::Proxy::Read( uint64_t  offset,
                   uint32_t  size,
                   void     *buffer,
                   uint32_t &bytesRead,
                   uint16_t  timeout)
/* -------------------------------------------------------------------------- */
{
  eos_debug("offset=%lu size=%u", offset, size);
  XRootDStatus status = WaitOpen();

  bytesRead = 0;

  if (!status.IsOK())
    return status;

  eos_debug("----: read: offset=%lu size=%u", offset, size);
  int readahead_window_hit = 0;

  uint64_t current_offset = offset;
  uint32_t current_size = size;

  bool isEOF=false;
  bool request_next = true;
  std::set<uint64_t> delete_chunk;

  void *pbuffer = buffer;


  if (XReadAheadStrategy != NONE)
  {
    ReadCondVar().Lock();

    if ( ChunkRMap().size())
    {
      bool has_successor = false;
      // see if there is anything in our read-ahead map
      for ( auto it = ChunkRMap().begin(); it != ChunkRMap().end(); ++it)
      {
        off_t match_offset;
        uint32_t match_size;

        XrdSysCondVarHelper lLock(it->second->ReadCondVar());

        eos_debug("----: eval offset=%lu chunk-offset=%lu rah-position=%lu", offset, it->second->offset(), mReadAheadPosition);
        if (it->second->matches(current_offset, current_size, match_offset, match_size))
        {
          readahead_window_hit++;

          while ( !it->second->done() )
            it->second->ReadCondVar().WaitMS(25);

          status = it->second->Status();

          if (it->second->Status().IsOK())
          {
            // the match result can change after the read actually returned
            if (!it->second->matches(current_offset, current_size, match_offset, match_size))
            {
              continue;
            }

            eos_debug("----: prefetched offset=%lu m-offset=%lu current-size=%u m-size=%u dim=%ld", current_offset, match_offset, current_size, match_size, (char*) buffer - (char*) pbuffer);
            // just copy what we have
            eos_debug("----: out-buffer=%lx in-buffer=%lx in-buffer-size=%lu", (long unsigned int) buffer, (long unsigned int) it->second->buffer(), it->second->vbuffer().size());


            memcpy(buffer, it->second->buffer() + match_offset - it->second->offset(), match_size);
            bytesRead += match_size;
            mTotalReadAheadHitBytes += match_size;
            buffer = (char*) buffer + match_size;
            current_offset = match_offset + match_size;
            current_size -= match_size;

            isEOF = it->second->eof();
            if (isEOF)
            {
              request_next = false;
	      XReadAheadNom = XReadAheadMin;
              break;
            }
          }
        }
        else
        {
          eos_debug("----: considering chunk address=%lx offset=%ld", it->first, it->second->offset());
          if (!it->second->successor(offset, size))
          {
            eos_debug("----: delete chunk address=%lx offset=%ld", it->first, it->second->offset());
            while ( !it->second->done() )
              it->second->ReadCondVar().WaitMS(25);
            // remove this chunk
            delete_chunk.insert(it->first);
            request_next = false;
          }
          else
          {
            has_successor = true;
          }
        }
      }

      if (!has_successor) {
        request_next = true;
      } else {
        request_next = false;
      }
      // check if we can remove previous prefetched chunks
      for ( auto it = ChunkRMap().begin(); it != ChunkRMap().end(); ++it)
      {
        XrdSysCondVarHelper lLock(it->second->ReadCondVar());
	if (it->second->done() && offset && ( (offset) >= (it->second->offset()+it->second->size())))
	{
	  eos_debug("----: dropping chunk offset=%lu chunk-offset=%lu", offset, it->second->offset());
	  delete_chunk.insert(it->first);
	}
      }

      for ( auto it = delete_chunk.begin(); it != delete_chunk.end(); ++it)
      {
        ChunkRMap().erase(*it);
      }
    }
    else
    {
      if ((off_t) offset == mPosition)
      {
        // re-enable read-ahead if sequential reading occurs
        request_next = true;
	if (!mReadAheadPosition) {
	  mReadAheadPosition = offset + size;
	  // tune the read-ahead size with the read-pattern
	  if (size > XReadAheadNom)
	    XReadAheadNom = size;
	}
      }
      else
      {
        request_next = false;
	XReadAheadNom = XReadAheadMin;
	mReadAheadPosition = 0;
      }
    }

    if (request_next)
    {
      // dynamic window scaling
      if (readahead_window_hit)
      {
        if (XReadAheadStrategy == DYNAMIC)
        {
          // increase the read-ahead window
          XReadAheadNom *=2;
          if (XReadAheadNom > XReadAheadMax)
            XReadAheadNom = XReadAheadMax;
        }
      }

      off_t align_offset = mReadAheadPosition;
      eos_debug("----: pre-fetch window=%lu pf-offset=%lu,",
                XReadAheadNom,
                (unsigned long) align_offset
                );

      if (ChunkRMap().count(align_offset))
      {
        ReadCondVar().UnLock();
      }
      else
      {
        ReadCondVar().UnLock();
        XrdCl::Proxy::read_handler rahread = ReadAsyncPrepare(align_offset, XReadAheadNom);
        XRootDStatus rstatus = PreReadAsync(align_offset, XReadAheadNom,
                                            rahread, timeout);
	mReadAheadPosition = align_offset + XReadAheadNom;
      }
    }
    else
    {
      ReadCondVar().UnLock();
    }
  }

  if (current_size)
  {
    uint32_t rbytes_read=0;
    status = File::Read(current_offset,
                        current_size,
                        buffer, rbytes_read, timeout);
    if (status.IsOK())
    {
      if (rbytes_read)
      {
        eos_debug("----: postfetched offset=%lu size=%u rbytes=%d", current_offset, current_size, rbytes_read);
      }
      bytesRead+=rbytes_read;
    }
  }

  set_readstate(&status);

  if (status.IsOK())
  {

    mPosition = offset + size;
    mTotalBytes += bytesRead;
  }
  return status;
}

/* -------------------------------------------------------------------------- */
XRootDStatus
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::OpenAsync( const std::string &url,
                        OpenFlags::Flags   flags,
                        Access::Mode       mode,
                        uint16_t         timeout)
/* -------------------------------------------------------------------------- */
{
  eos_debug("url=%s flags=%x mode=%x", url.c_str(), (int) flags, (int) mode);
  XrdSysCondVarHelper lLock(OpenCondVar());

  mUrl = url;
  mFlags = flags;
  mMode = mode;
  mTimeout = timeout;

  if ( state() == OPENING )
  {
    XRootDStatus status(XrdCl::stError,
                        suAlreadyDone,
                        XrdCl::errInProgress,
                        "in progress"
                        );
    return status;
  }

  if ( state() == OPENED )
  {
    XRootDStatus status(XrdCl::stOK,
                        0,
                        0,
                        "opened"
                        );

    return status;
  }

  if ( state() == FAILED )
  {

    return XOpenState;
  }

  // Disable recovery on read and write
#if kXR_PROTOCOLVERSION == 0x00000297
  ((XrdCl::File*)(this))->EnableReadRecovery(false);
  ((XrdCl::File*)(this))->EnableWriteRecovery(false);
#else
  SetProperty("ReadRecovery", "false");
  SetProperty("WriteRecovery", "false");
#endif

  XrdCl::XRootDStatus status = Open(url.c_str(),
                                    flags,
                                    mode,
                                    &XOpenAsyncHandler,
                                    timeout);

  if(status.IsOK()) {
    set_state(OPENING);
  }
  else {
    set_state(FAILED);
  }

  return XOpenState;
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::OpenAsyncHandler::HandleResponseWithHosts(XrdCl::XRootDStatus* status,
                                                        XrdCl::AnyObject* response,
                                                        XrdCl::HostList * hostList)
/* -------------------------------------------------------------------------- */
{
  eos_static_debug("");

  {
    XrdSysCondVarHelper openLock(proxy()->OpenCondVar());
    if (status->IsOK())
    {
      proxy()->set_state(OPENED);
      
      openLock.UnLock();
      
      XrdSysCondVarHelper writeLock(proxy()->WriteCondVar());
      while (proxy()->WriteQueue().size())
      {
	write_handler handler = proxy()->WriteQueue().front();
	XRootDStatus status;
	eos_static_debug("sending scheduled write request: off=%ld size=%lu timeout=%hu",
			 handler->offset(),
			 handler->vbuffer().size(),
			 handler->timeout());
	
	writeLock.UnLock();
	status = proxy()->WriteAsync ( (uint64_t)handler->offset(),
				       (uint32_t)(handler->vbuffer().size()),
				       0,
				       handler,
				       handler->timeout()
				       );
	
	writeLock.Lock(&proxy()->WriteCondVar());
	proxy()->WriteQueue().pop_front();
	
	if (!status.IsOK())
	{
	  proxy()->set_writestate(&status);
	}
      }
      
      writeLock.UnLock();
      openLock.Lock(&proxy()->OpenCondVar());
    }
    else
    {
      proxy()->set_state(FAILED, status);
    }
    
    proxy()->OpenCondVar().Signal();

    delete hostList;
    delete status;
    if (response) delete response;
  }

  mProxy->CheckSelfDestruction();
}

/* -------------------------------------------------------------------------- */
XRootDStatus 
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::ReOpenAsync()
/* -------------------------------------------------------------------------- */
{
  if (mUrl.length())
  {
    return OpenAsync(mUrl, mFlags, mMode, mTimeout);
  }
  else
  {
    XRootDStatus status(XrdCl::stError,
			suRetry,
			XrdCl::errUninitialized,
			"never opened before"
			);
    set_state_TS(FAILED, &status);
    return status;
  }
}


/* -------------------------------------------------------------------------- */
XRootDStatus
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::CloseAsync(uint16_t         timeout)
/* -------------------------------------------------------------------------- */
{
  eos_debug("");
  // don't close files attached by several clients
  if (mAttached > 1)
  {
    eos_debug("still attached");
    return XRootDStatus();
  }

  WaitOpen();
  XrdSysCondVarHelper lLock(OpenCondVar());

  // only an opened file requires a close, otherwise we return the last known state
  if (state() == OPENED) {
    XrdCl::XRootDStatus status = XrdCl::File::Close(&XCloseAsyncHandler,
						    timeout);
    set_state(CLOSING, &status);
  } else {
    XrdCl::XRootDStatus status;
    set_state(CLOSED, &status);
  }
  return XOpenState;
}

/* -------------------------------------------------------------------------- */
XRootDStatus
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::ScheduleCloseAsync(uint16_t         timeout)
/* -------------------------------------------------------------------------- */
{
  eos_debug("");
  if (mAttached > 1)
  {
    eos_debug("still attached");
    return XRootDStatus();
  }

  {
    bool no_chunks_left = true;

    if ( (stateTS () == OPENING) ||
	 (stateTS () == OPENED) )
    {
      {
	XrdSysCondVarHelper lLock(WriteCondVar());
	// either we have submitted chunks
	if (ChunkMap().size())
	  no_chunks_left = false;

	// or we have chunks still to be submitted
	if (WriteQueue().size())
	  no_chunks_left = false;
	if (!no_chunks_left)
	{
	  // indicate to close this file when the last write-callback arrived
	  eos_debug("indicating close-after-write");
	  XCloseAfterWrite = true;
	  XCloseAfterWriteTimeout = timeout;
	}
      }

      if (no_chunks_left)
      {
	return CloseAsync (timeout);
      }
      else
      {
	return XOpenState;
      }
    }

  }

  XRootDStatus status(XrdCl::stError,
		      suAlreadyDone,
		      XrdCl::errInvalidOp,
		      "file not open"
		      );

  return status;
}


/* -------------------------------------------------------------------------- */
XRootDStatus
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::Close(uint16_t         timeout)
/* -------------------------------------------------------------------------- */
{
  eos_debug("");
  // don't close files attached by several clients
  if (mAttached > 1)
    return XRootDStatus();

  WaitOpen();
  Collect();
  XrdSysCondVarHelper lLock(OpenCondVar());

  XrdCl::XRootDStatus status = XrdCl::File::Close(timeout);
  set_state(CLOSED, &status);
  return status;
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::CloseAsyncHandler::HandleResponse (XrdCl::XRootDStatus* status,
                                                 XrdCl::AnyObject * response)
/* -------------------------------------------------------------------------- */
{
  eos_static_debug("");
  XrdSysCondVarHelper lLock(mProxy->OpenCondVar());
  if (!status->IsOK())
  {
    // if the open failed before, we leave the open failed state here
    eos_static_debug("current status = %d - setting CLOSEFAILED\n", mProxy->state());
    if (mProxy->state() != XrdCl::Proxy::FAILED)
      mProxy->set_state(XrdCl::Proxy::CLOSEFAILED, status);
  }
  else
  {
    mProxy->set_state(XrdCl::Proxy::CLOSED, status);
  }

  mProxy->OpenCondVar().Signal();
  delete response;
  delete status;

  mProxy->CheckSelfDestruction();
}

/* -------------------------------------------------------------------------- */
XRootDStatus
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::WaitClose()
/* -------------------------------------------------------------------------- */
{
  eos_debug("");
  Collect();
  XrdSysCondVarHelper lLock(OpenCondVar());

  while (state () == CLOSING)
    OpenCondVar().WaitMS(25);

  return XOpenState;
}

/* -------------------------------------------------------------------------- */
XRootDStatus
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::WaitOpen()
/* -------------------------------------------------------------------------- */
{
  eos_debug("");
  XrdSysCondVarHelper lLock(OpenCondVar());

  while (state () == OPENING)
    OpenCondVar().WaitMS(25);

  return XOpenState;
}

/* -------------------------------------------------------------------------- */
int
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::WaitOpen(fuse_req_t req)
/* -------------------------------------------------------------------------- */
{
  eos_debug("");
  XrdSysCondVarHelper lLock(OpenCondVar());

  while (state () == OPENING)
  {
    if (req && fuse_req_interrupted(req))
    {
      return EINTR;
    }
    OpenCondVar().WaitMS(25);
  }
  return 0;
}

/* -------------------------------------------------------------------------- */
bool
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::IsOpening()
/* -------------------------------------------------------------------------- */
{
  XrdSysCondVarHelper lLock(OpenCondVar());
  eos_debug("state=%d", state());
  return (state () == OPENING) ? true : false;
}

/* -------------------------------------------------------------------------- */
bool
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::IsClosing()
/* -------------------------------------------------------------------------- */
{
  XrdSysCondVarHelper lLock(OpenCondVar());
  eos_debug("state=%d", state());
  return (state () == CLOSING) ? true : false;
}

/* -------------------------------------------------------------------------- */
bool
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::IsOpen()
/* -------------------------------------------------------------------------- */
{
  XrdSysCondVarHelper lLock(OpenCondVar());
  eos_debug("state=%d", state());
  return (state () == OPENED) ? true : false;
}

/* -------------------------------------------------------------------------- */
bool
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::IsClosed()
/* -------------------------------------------------------------------------- */
{
  XrdSysCondVarHelper lLock(OpenCondVar());
  eos_debug("state=%d", state());
  return ( (state () == CLOSED) || (state () == CLOSEFAILED) || (state () == FAILED) ) ? true : false;
}

bool

XrdCl::Proxy::IsWaitWrite()
{
  XrdSysCondVarHelper lLock(OpenCondVar());
  eos_debug("state=%d", state());
  return (state () == WAITWRITE) ? true : false;
}

bool
XrdCl::Proxy::HadFailures(std::string &message)
{
  bool ok=true;
  XrdSysCondVarHelper lLock(OpenCondVar());
  if ( state () == CLOSEFAILED)
  {
    message = "file close failed";
    ok = false;
  }
  if ( state () == FAILED)
  {
    message = "file open failed";
    ok = false;
  }
  if ( !write_state().IsOK())
  {
    message = "file writing failed";
    ok = false;
  }
  eos_debug("state=%d had-failures=%d", state(), !ok);
  return !ok;
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::WriteAsyncHandler::HandleResponse (XrdCl::XRootDStatus* status,
                                                 XrdCl::AnyObject * response)
/* -------------------------------------------------------------------------- */
{
  eos_static_debug("");
  bool no_chunks_left = true;
  {
    if (proxy())
    {
      XrdSysCondVarHelper lLock(mProxy->WriteCondVar());
      if (!status->IsOK())
      {
	mProxy->set_writestate(status);
      }
      mProxy->WriteCondVar().Signal();
    }
    delete response;
    delete status;
    if (proxy())
    {
      if ( (mProxy->ChunkMap().size() > 1 ) ||
	   (!mProxy->ChunkMap().count( (uint64_t) this)) )
	no_chunks_left = false;
    }
    else
    {
      return;
    }
  }

  {
    XrdSysCondVarHelper lLock(mProxy->WriteCondVar());
    mProxy->ChunkMap().erase((uint64_t)this);
  }

  if (no_chunks_left)
  {
    if (mProxy->close_after_write())
    {
      eos_static_debug("sending close-after-write");
      // send an asynchronous close now
      XrdCl::XRootDStatus status = mProxy->CloseAsync(mProxy->close_after_write_timeout());
    }
  }

  if (no_chunks_left)
    mProxy->CheckSelfDestruction();
}

/* -------------------------------------------------------------------------- */
XrdCl::Proxy::write_handler
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::WriteAsyncPrepare(uint32_t size, uint64_t offset, uint16_t timeout)
/* -------------------------------------------------------------------------- */
{
  eos_debug("");
  write_handler dst = std::make_shared<WriteAsyncHandler>(this, size, offset, timeout);
  XrdSysCondVarHelper lLock(WriteCondVar());
  ChunkMap()[(uint64_t) dst.get()] = dst;
  return dst;
}

/* -------------------------------------------------------------------------- */
XRootDStatus
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::WriteAsync( uint64_t         offset,
                         uint32_t         size,
                         const void      *buffer,
                         XrdCl::Proxy::write_handler handler,
                         uint16_t         timeout)
/* -------------------------------------------------------------------------- */
{
  eos_debug("");

  // a buffer indicates, the handler buffer is already filled
  if (buffer)
    handler->copy(buffer, size);

  XRootDStatus status = Write(static_cast<uint64_t> (offset),
                              static_cast<uint32_t> (size),
                              handler->buffer(), handler.get(), timeout);

  if (!status.IsOK())
  {
    // remove failing requests
    XrdSysCondVarHelper lLock(WriteCondVar());
    ChunkMap().erase((uint64_t) handler.get());
  }
  return status;
}

/* -------------------------------------------------------------------------- */
XRootDStatus
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::ScheduleWriteAsync(
				 const void   *buffer,
				 write_handler handler
				 )
/* -------------------------------------------------------------------------- */
{
  eos_debug("");
  if (buffer)
    handler->copy(buffer, handler->vbuffer().size());

  XrdSysCondVarHelper openLock(OpenCondVar());
  if (state () == OPENED)
  {
    openLock.UnLock();
    eos_debug("direct");
    inc_write_queue_direct_submissions();
    // we can send off the write request
    return WriteAsync ( handler->offset(),
			(size_t)handler->vbuffer().size(),
			0,
			handler,
			handler->timeout());
  }

  if (state () == OPENING)
  {
    inc_write_queue_scheduled_submissions();
    eos_debug("scheduled");
    // we add this write to the list to be submitted when the open call back arrives
    XrdSysCondVarHelper lLock(WriteCondVar());
    WriteQueue().push_back(handler);

    // we can only say status OK in that case
    XRootDStatus status(XrdCl::stOK,
			0,
                        XrdCl::errInProgress,
			"in progress"
                        );

    return status;
  }

  return XOpenState;
}


/* -------------------------------------------------------------------------- */
XRootDStatus
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::WaitWrite()
/* -------------------------------------------------------------------------- */
{
  eos_debug("");

  WaitOpen();

  if (stateTS() == WAITWRITE)
  {
    XrdSysCondVarHelper openLock(OpenCondVar());
    return XOpenState;
  }

  // check if the open failed
  if (stateTS() != OPENED)
  {
    XrdSysCondVarHelper openLock(OpenCondVar());
    return XOpenState;
  }

  {
    time_t wait_start = time(NULL);
    XrdSysCondVarHelper lLock(WriteCondVar());

    while ( ChunkMap().size() )
    {
      eos_debug("     [..] map-size=%lu", ChunkMap().size());
      WriteCondVar().WaitMS(1000);
      time_t wait_stop  = time(NULL);
      if ( ChunkMap().size() && ((wait_stop-wait_start) > sChunkTimeout))
      {
	// move all pending chunks to the static map
	// in principle this is not supposed to happen
	XrdSysMutexHelper chunkLock(sTimeoutAsyncChunksMutex);
	eos_err("discarding %d chunks  in-flight for writing", ChunkMap().size());
	for (auto it = ChunkMap().begin(); it != ChunkMap().end(); ++it) {
	  it->second->disable();
	  sTimeoutWriteAsyncChunks.push_back(it->second);
	}
	ChunkMap().clear();
	return XRootDStatus(XrdCl::stFatal,
			    suDone,
			    XrdCl::errSocketTimeout,
			    "request timeout"
			    );
      }
    }
    eos_debug(" [..] map-size=%lu", ChunkMap().size());
  }

  {
    XrdSysCondVarHelper writeLock(WriteCondVar());
    return XWriteState;
  }
}

/* -------------------------------------------------------------------------- */
int
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::WaitWrite(fuse_req_t req)
/* -------------------------------------------------------------------------- */
{
  // this waits for all writes to come back and checks for interrupts inbetween
  // this assumes a file is in OPENED state
  {
    XrdSysCondVarHelper lLock(WriteCondVar());

    while ( ChunkMap().size() )
    {
      if (req && fuse_req_interrupted(req))
      {
	return EINTR;
      }

      eos_debug("     [..] map-size=%lu", ChunkMap().size());
      WriteCondVar().WaitMS(1000);
    }
    eos_debug(" [..] map-size=%lu", ChunkMap().size());
  }
  return 0;
}

/* -------------------------------------------------------------------------- */
bool
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::OutstandingWrites()
/* -------------------------------------------------------------------------- */
{
  eos_debug("");
  XrdSysCondVarHelper lLock(WriteCondVar());
  return ChunkMap().size() ? true : false;
}

void
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::ReadAsyncHandler::HandleResponse (XrdCl::XRootDStatus* status,
                                                XrdCl::AnyObject * response)
/* -------------------------------------------------------------------------- */
{
  eos_static_debug("");
  {
    XrdSysCondVarHelper lLock(ReadCondVar());
    mStatus = *status;
    if (status->IsOK())
    {
      XrdCl::ChunkInfo* chunk=0;
      if (response)
      {
	response->Get(chunk);
	if (chunk->length < mBuffer->size())
	{
	  mBuffer->resize(chunk->length);
	  mEOF = true;
	}
	delete response;
      }
      else
	mBuffer->resize(0);
    }
    mDone = true;
    delete status;
    mProxy->dec_read_chunks_in_flight();
    ReadCondVar().Signal();
  }

  if (!proxy())
    return;

  {
    if (!mProxy->HasReadsInFlight())
      mProxy->CheckSelfDestruction();
  }
}

/* -------------------------------------------------------------------------- */
XrdCl::Proxy::read_handler
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::ReadAsyncPrepare(off_t offset, uint32_t size)
/* -------------------------------------------------------------------------- */
{
  eos_debug("");
  read_handler src = std::make_shared<ReadAsyncHandler>(this, offset, size);
  XrdSysCondVarHelper lLock(ReadCondVar());
  if (!ChunkRMap().count(src->offset())) {
    inc_read_chunks_in_flight();
  }
  ChunkRMap()[(uint64_t) src->offset()] = src;
  ReadCondVar().Signal();
  return src;
}

/* -------------------------------------------------------------------------- */
XRootDStatus
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::PreReadAsync( uint64_t         offset,
                           uint32_t          size,
                           read_handler handler,
                           uint16_t          timeout)
/* -------------------------------------------------------------------------- */
{
  eos_debug("");
  XRootDStatus status = WaitOpen();

  if (!status.IsOK())
    return status;

  return File::Read(static_cast<uint64_t> (offset),
                    static_cast<uint32_t> (size),
                    (void*) handler->buffer(), handler.get(), timeout);
}

/* -------------------------------------------------------------------------- */
XRootDStatus
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::WaitRead(read_handler handler)
/* -------------------------------------------------------------------------- */
{
  eos_debug("");
  XrdSysCondVarHelper lLock(handler->ReadCondVar());

  time_t wait_start = time(NULL);
  while ( !handler->done() )
  {
    handler->ReadCondVar().WaitMS(1000);
    time_t wait_stop  = time(NULL);
    if ( ((wait_stop-wait_start) > sChunkTimeout) )
    {
      // move the pending chunk to the static map
      // in principle this is not supposed to happen
      XrdSysMutexHelper chunkLock(sTimeoutAsyncChunksMutex);
      eos_err("discarding %d chunks  in-flight for writing", ChunkMap().size());
      for (auto it = ChunkRMap().begin(); it != ChunkRMap().end(); ++it) {
	it->second->disable();
	sTimeoutReadAsyncChunks.push_back(it->second);
      }
      clear_read_chunks_in_flight();
      ChunkRMap().clear();

      return XRootDStatus(XrdCl::stFatal,
			  suDone,
			  XrdCl::errSocketTimeout,
			  "request timeout"
			  );
    }
  }
  eos_debug(" [..] read-size=%lu", handler->vbuffer().size());
  return handler->Status();
}

/* -------------------------------------------------------------------------- */
XRootDStatus
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::ReadAsync(read_handler handler,
                        uint32_t  size,
                        void     *buffer,
                        uint32_t & bytesRead)
/* -------------------------------------------------------------------------- */
{
  eos_debug("");
  XRootDStatus status = WaitRead(handler);
  if (!status.IsOK())
    return status;
  bytesRead = (size < handler->vbuffer().size()) ? size : handler->vbuffer().size();
  memcpy(buffer, handler->buffer(), bytesRead);
  return status;
}


/* -------------------------------------------------------------------------- */
XRootDStatus
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::Sync( uint16_t timeout )
{
  eos_debug("");
  return File::Sync ( timeout );
}


/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::attach()
/* -------------------------------------------------------------------------- */
{
  XrdSysMutexHelper lLock(mAttachedMutex);
  mAttached++;
  eos_debug("attached=%u", mAttached);
  return;
}

/* -------------------------------------------------------------------------- */
size_t
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::detach()
/* -------------------------------------------------------------------------- */
{
  XrdSysMutexHelper lLock(mAttachedMutex);
  mAttached--;
  eos_debug("attached=%u", mAttached);
  return mAttached;
}

/* -------------------------------------------------------------------------- */
bool
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::attached()
/* -------------------------------------------------------------------------- */
{

  XrdSysMutexHelper lLock(mAttachedMutex);

  return mAttached ? true : false;
}

/* -------------------------------------------------------------------------- */
size_t
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::get_attached()
/* -------------------------------------------------------------------------- */
{

  XrdSysMutexHelper lLock(mAttachedMutex);

  return mAttached;
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
XrdCl::Proxy::CheckSelfDestruction()
{
  if (should_selfdestroy())
  {
    eos_debug("self-destruction");
    delete this;
  }
}
