#include "exosfile.hh"
#include <stdio.h>
#include <uuid/uuid.h>

exosmanager* exosfile::sManager = 0;



int 
exosmanager::connect(std::map<std::string,std::string> &params)
{
  int retc = 0;

  if (!mConnected)
  {
    if (debug) fprintf(stderr,"... connecting as %s\n", params["rados.user"].c_str());
    retc = mCluster.init(params["rados.user"].c_str());
    if (retc)
      return retc;

    if (debug) fprintf(stderr,"... reading config %s\n", params["rados.config"].c_str());
    retc = mCluster.conf_read_file(params["rados.config"].length()?params["rados.config"].c_str():0);
    if (retc)
    {
      mCluster.shutdown();
      return retc;
    }
    retc = mCluster.connect();
    if (debug) fprintf(stderr,"... connected retc=%d\n", retc);
    if (retc)
    {
      mCluster.shutdown();
      return retc;
    }
    mConnected = true;
  }

  if (!mIoCtx.count(params["rados.md"]))
  {
    ioctx io = std::make_shared<librados::IoCtx>();

    if (debug) fprintf(stderr,"... ioctx for pool %s\n", params["rados.md"].c_str());

    // create md cluster context
    retc = mCluster.ioctx_create(params["rados.md"].c_str(), *io);
    if (retc)
    {
      return retc;
    }
    //    io.application_enable("rados", true);
    mIoCtx[params["rados.md"]] = io;
  }

  if (!mIoCtx.count(params["rados.data"]))
  {
    ioctx io = std::make_shared<librados::IoCtx>();

    if (debug) fprintf(stderr,"... ioctx for pool %s\n", params["rados.data"].c_str());

    // create data cluster context
    retc = mCluster.ioctx_create(params["rados.data"].c_str(), *io);
    if (retc)
    {
      return retc;
    }
    //    io.application_enable("rados", true);
    mIoCtx[params["rados.data"]] = io;
  }

  return 0;
}

exosmanager::ioctx 
exosmanager::getIoCtx(const std::string& pool)
{
  return mIoCtx[pool];
}


exosmanager::~exosmanager()
{
  mCluster.shutdown();
}

void
exosfile::init(const std::string& name, const std::string& cgi)
{
  if (!sManager)
  {
    sManager = new exosmanager();
  }

  params = parse(cgi);
  mName = name;
  mFlags = 0;
  mOpened = false;
  mPrepared = false;
  mSize = 0;
  mMtime.tv_sec = mMtime.tv_nsec = 0;
  mBlockSize = EXOSMANAGER_DEFAULT_BLOCKSIZE;
  mSeqWrite = true;
  mLocked = false;
  mLockedExclusive = false;
  mLockExpires = 0;

  uuid_t uuid;
  uuid_generate_time(uuid);
  uuid_unparse(uuid, mUUID);
}

exosfile::exosfile(const std::string& name, const std::string& cgi)
{
  init(name, cgi);
}

exosfile::~exosfile()
{
  if (mOpened)
    close();
  if (mLocked)
    unlock();
}

std::string
exosfile::dump()
{
  char sdump[1024];
  snprintf(sdump, sizeof(sdump),"%s => %s size:%lu mtime:%lu.%lu",
	   mName.c_str(),mInode.c_str(),mSize,mMtime.tv_sec,mMtime.tv_nsec);
  return sdump;
}

int 
exosfile::stat(struct stat* buf)
{
  int retc = 0;
  if (!mOpened)
  {
    if (prepare())
      return -ENOTCONN;
    retc = get_md();
  }
  if (!retc) {
    memset(buf, 0, sizeof(struct stat));
    buf->st_dev = 0xff;
    buf->st_ino = strtoull(mInode.c_str(),0,16);
    buf->st_size = mSize;
#ifdef __APPLE__
    buf->st_mtimespec.tv_nsec = mMtime.tv_nsec;
    buf->st_mtimespec.tv_sec = mMtime.tv_sec;
#else
    buf->st_mtim.tv_nsec = mMtime.tv_nsec;
    buf->st_mtim.tv_sec = mMtime.tv_sec;
#endif

    buf->st_mode = S_IRWXU;
    buf->st_nlink = 1;
    buf->st_blksize = mBlockSize;
    buf->st_blocks = (mSize / 512) + 1;
  }
  return retc;
}

int 
exosfile::prepare()
{
  if (mPrepared)
    return 0;

  if (connect())
    return -ENOTCONN;

  mPrepared = true;

  mPool = params["rados.md"];
  mDataPool = params["rados.data"];
  return 0;
}

int 
exosfile::open(int flags)
{
  std::lock_guard<std::mutex> lock(exist_mutex);

  if (sManager->debug) fprintf(stderr,"... flags=%x\n", flags);

  int retc = 0;

  if (mOpened)
    return -EALREADY;

  mFlags = flags;

  if (prepare())
    return -ENOTCONN;

  retc = open_md();
  
  if (mFlags & O_CREAT) {
    if ( !retc && (mFlags & O_EXCL) )
    { 
      return -EEXIST;
    }
    if ( retc )
    {
      // otherwise create in metadata pool 
      retc = create_md();
    }
  }

  // define default read-ahead settings
  set_readahead_strategy(STATIC, 32 * 1024 * 1024, 32 * 1024 * 1024, 32 * 1024 * 1024);

  mPosition = 0;
  mWritePosition = mSize;
  mTotalBytes = 0;
  mTotalReadAheadHitBytes  = 0;

  if (sManager->debug) fprintf(stderr,"... open retc=%d\n", retc);

  return retc;
}

int 
exosfile::close()
{
  int retc = 0;
  std::lock_guard<std::mutex> lock(exist_mutex);
  if (sManager->debug) fprintf(stderr,"... close opend=%d\n", mOpened);

  if (!mOpened)
    return -EBADF;

  if ( (mFlags & O_WRONLY) || (mFlags & O_RDWR) )
    retc = aio_flush();
  
  mOpened = false;
  return retc;
}

ssize_t 
exosfile::write(const char* buffer, off_t offset, size_t len)
{
  int retc = 0;
  if (!mOpened)
    return -EBADF;

  if (! ( (mFlags & O_WRONLY) || (mFlags & O_RDWR) ) )
    return -EBADF;

  std::lock_guard<std::mutex> lock(write_mutex);

  mSeqWrite = false;

  std::vector<extents_t> objmap = object_extents(offset,len);

  for (auto it=objmap.begin(); it!=objmap.end(); ++it)
  {
    librados::ObjectWriteOperation op;
    librados::bufferlist b;
    b.append(buffer+it->offset-offset,it->len);
    op.write(it->oid_offset, b);
    retc |= sManager->getIoCtx(mDataPool)->operate(it->oid, &op);
    if (sManager->debug) fprintf(stderr,"... wrote %s off:%lu len:%lu retc:%d\n",
				 it->oid.c_str(), it->offset, it->len, retc);
  }

  if (!retc)
  {
    mWritePosition = offset+len;
    if (mWritePosition > mSize)
      mSize = mWritePosition;
  }

  return retc;
}

ssize_t 
exosfile::aio_write(const char* buffer, off_t offset, size_t len)
{
  if (sManager->debug) fprintf(stderr,"... aiowrite off:%lu len:%lu write-position:%lu seq:%d\n",offset, len, mWritePosition, mSeqWrite);
  int retc = 0;

  if (!mOpened)
    return -EBADF;

  if (! ( (mFlags & O_WRONLY) || (mFlags & O_RDWR) ) )
    return -EBADF;

  std::lock_guard<std::mutex> lock(write_mutex);

  retc = aio_collect();

  if (retc)
    return retc;

  // optimization code for sequential writing
  if (mSeqWrite && (offset == mWritePosition))
  {
    fprintf(stderr,"seq write detected\n");
    // seq write detected
    if (!mSeqWriteHandler)
      mSeqWriteHandler = std::make_shared<AsyncHandler>(offset, mBlockSize);
    size_t append_len = len;
    size_t wlen = len;
    if (append_len > (mBlockSize-mSeqWriteHandler->buffer.length()))
      append_len = (mBlockSize-mSeqWriteHandler->buffer.length());

    do {
      // ship-off all required buffers for that write
      mSeqWriteHandler->buffer.append(buffer, append_len);
      wlen -= append_len;
      
      if (mSeqWriteHandler->buffer.length() == mBlockSize)
      {
	std::vector<extents_t> objmap = object_extents(mSeqWriteHandler->offset,mSeqWriteHandler->len);
	assert (objmap.size()==1);
	sManager->getIoCtx(mDataPool)->aio_write(objmap[0].oid, mSeqWriteHandler->completion, mSeqWriteHandler->buffer, objmap[0].len, objmap[0].oid_offset);
	off_t newoffset = mSeqWriteHandler->offset + mSeqWriteHandler->len;
	ChunkWMap.insert(mSeqWriteHandler);
	mSeqWriteHandler = std::make_shared<AsyncHandler>(newoffset, mBlockSize);
	if (len > mBlockSize)
	  append_len = mBlockSize;
	else
	  append_len = wlen;
	
	if (sManager->debug) fprintf(stderr,"... seq-push %s off:%lu len:%lu retc:%d\n",
				     objmap[0].oid.c_str(), objmap[0].offset, objmap[0].len, retc);
	
      }
      else 
      {
	fprintf(stderr,"seq-write in memory\n");
      }
    } while (wlen);
    mWritePosition = offset + len;
    if (mWritePosition > mSize)
      mSize = mWritePosition;
    fprintf(stderr,"returning seq write %lu\n", mWritePosition);
    return len;
  }
  else
  {
    mSeqWrite = false;
  }

  // non sequential writing
  std::vector<extents_t> objmap = object_extents(offset,len);

  for (auto writ=objmap.begin(); writ!=objmap.end(); ++writ)
  {
    io_handler dst = std::make_shared<AsyncHandler>(writ->offset, writ->len);

    fprintf(stderr,"I am pushing %s\n%ld %ld %ld %ld\n", buffer, offset, len, writ->offset, writ->len);
    dst->buffer.append(buffer+writ->offset - offset,writ->len);

    ChunkWMap.insert(dst);
    retc |= sManager->getIoCtx(mDataPool)->aio_write(writ->oid, dst->completion, dst->buffer, writ->len, writ->oid_offset);

    if (sManager->debug) fprintf(stderr,"... wrote %s off:%lu len:%u retc:%d\n",
				 writ->oid.c_str(), dst->offset, dst->len, retc);
  }

  mWritePosition = offset + len;
  if (mWritePosition > mSize)
    mSize = mWritePosition;

  return retc;
}


ssize_t 
exosfile::read(char* buffer, off_t offset, size_t len)
{
  if (sManager->debug) fprintf(stderr,"... read off:%lu len:%lu\n",offset, len);
  if (!mOpened)
    return -EBADF;


  if (mFlags & O_WRONLY)
    return -EBADF;

  // we must make sure there is no write in flight when we read
  aio_collect();

  if ( (offset + len ) > mSize )
  {
    // restrict to possible offset,len values
    if (offset >= mSize) {
      if (sManager->debug) fprintf(stderr,"... short-read out of filesize bounds\n");
      return 0;
    }
    else
      len = mSize - offset;
  }

  ssize_t bytesRead = 0;

  std::vector<extents_t> objmap = object_extents(offset,len);

  void *pbuffer = (void*)buffer;

  for (auto oit = objmap.begin(); oit != objmap.end(); ++oit) {
    int readahead_window_hit = 0;
    bool request_next = true;
    std::set<uint64_t> delete_chunk;
    uint64_t current_offset = oit->offset;
    uint64_t current_size = oit->len;
    
    if (XReadAheadStrategy != NONE) {
      std::lock_guard<std::mutex> lock(read_mutex);
      if ( ChunkRMap.size()) {
	bool has_successor = false;
	// see if there is anything in our read-ahead map
	for ( auto it = ChunkRMap.begin(); it != ChunkRMap.end(); ++it) {
	  off_t match_offset;
	  uint32_t match_size;
	  
	  if (sManager->debug) fprintf(stderr, "----: eval offset=%lu chunk-offset=%lu\n", offset, it->second->offset);
	  if (it->second->matches(current_offset, current_size, match_offset, match_size))
	  {
	    readahead_window_hit++;
	    
	    it->second->completion->wait_for_complete_and_cb();
	    if (sManager->debug) fprintf(stderr, "----: window hit - retc=%d\n", it->second->completion->get_return_value());
	    
	    if (it->second->completion->get_return_value()==it->second->len)
	    {
	      if (sManager->debug) fprintf(stderr, "----: prefetched offset=%lu m-offset=%lu current-size=%lu m-size=%u dim=%ld\n", current_offset, match_offset, current_size, match_size, (char*) buffer - (char*) pbuffer);
	      // just copy what we have
	      if (sManager->debug) fprintf(stderr, "----: out-buffer=%lx in-buffer=%lx in-buffer-size=%u\n", (long unsigned int) buffer, (long unsigned int) it->second->buffer.c_str(), it->second->buffer.length());

	      
	      memcpy(buffer, it->second->buffer.c_str() + match_offset - it->second->offset, match_size);
	      bytesRead += match_size;
	      mTotalReadAheadHitBytes += match_size;
	      buffer = (char*) buffer + match_size;
	      current_offset = match_offset + match_size;
	      current_size -= match_size;
	      
	      if (it->second->isEOF())
	      {
		fprintf(stderr,"----: EOF => reset min\n");
		request_next = false;
		XReadAheadNom = XReadAheadMin;
		break;
	      } 
	    }
	  } else {
	    if (sManager->debug) fprintf(stderr, "----: considering chunk offset=%ld offset=%ld\n", it->first, it->second->offset);
	    if (!it->second->successor(oit->offset, oit->len, XReadAheadNom)) {
	      fprintf(stderr,"----: delete chunk address=%ld offset=%ld\n", it->first, it->second->offset);
	      it->second->completion->wait_for_complete_and_cb();
	      // remove this chunk
	      delete_chunk.insert(it->first);
	      request_next = false;
	    } else {
	      has_successor = true;
	    }
	  }
	}
	
	if (!has_successor)
	  request_next = true;
	else
	{
	  request_next = false;
	}
	
	// check if we can remove previous prefetched chunks
	for ( auto it = ChunkRMap.begin(); it != ChunkRMap.end(); ++it) {
	  if (it->second->completion->is_complete_and_cb() && offset && ( (offset) >= (it->second->offset+it->second->len))) {
	    if (sManager->debug) fprintf(stderr, "----: dropping chunk offset=%lu chunk-offset=%lu\n", offset, it->second->offset);
	    delete_chunk.insert(it->first);
	  }
	}
	
	for ( auto it = delete_chunk.begin(); it != delete_chunk.end(); ++it) {
	  ChunkRMap.erase(*it);
	}
      } else {
	if ((off_t) offset == mPosition) {
	  // re-enable read-ahead if sequential reading occurs
	  request_next = true;
	} else {
	  request_next = false;
	  XReadAheadNom = XReadAheadMin;
	}
      }
      
      if (request_next) {
	off_t align_offset = aligned_offset( ChunkRMap.size() ? offset + XReadAheadNom : offset);
	if (sManager->debug) fprintf(stderr, "----: pre-fetch window=%lu pf-offset=%lu\n",
				     XReadAheadNom,
				     (unsigned long) align_offset
				     );

	if (ChunkRMap.count(align_offset)) {
	} else {
	  // the read-ahead block can be also distributed over several blocks
	  std::vector<extents_t> ramap = object_extents(align_offset, XReadAheadNom);
	  for (auto rait = ramap.begin(); rait != ramap.end(); ++rait) {
	    io_handler src = std::make_shared<AsyncHandler>(rait->offset, rait->len);
	    ChunkRMap[(uint64_t) src->offset] = src;
	    sManager->getIoCtx(mDataPool)->aio_read(rait->oid, src->completion, &src->buffer, src->len, rait->oid_offset);
	    if (sManager->debug) fprintf(stderr, "----: pre-fetching %s pf-offset=%lu pf-size=%lu oid-offset=%lu\n",
					 rait->oid.c_str(),src->offset, src->len, rait->oid_offset);
	    
	    
	  }
	}
      } else {
      }
    }

    librados::bufferlist b;
    int retc=0;
    if (current_size) {
      retc = sManager->getIoCtx(mDataPool)->read(oit->oid, b, current_size, oit->oid_offset + current_offset - oit->offset);

      if (sManager->debug) fprintf(stderr, "----: sync-read %s offset=%lu/%lu size=%u read-bytes=%u\n", 
				   oit->oid.c_str(),
				   current_offset, oit->offset, current_size, b.length());
      
      bytesRead += b.length();
      memcpy(buffer+current_offset-offset, b.c_str(), b.length());
      current_size -= b.length();
      current_offset += b.length();
    }

    // deal with reads on sparse files
    if ( current_size ) {
      if ( current_offset < mSize ) {
	// fill in with zero's
	memset(buffer+current_offset-offset, 0, mSize - current_offset);
	bytesRead += (mSize - current_offset);
      }
    }

    mPosition = offset + bytesRead;
    mTotalBytes += bytesRead;

  }
  return bytesRead;
}

ssize_t 
exosfile::truncate(off_t offset)
{
  int retc = 0;
  if (!mOpened)
    return -EBADF;

  {
    std::lock_guard<std::mutex> lock(write_mutex);
    mSeqWrite = false;
  }


  retc = aio_flush();

  if (retc)
    return retc;
  

  return unlink(offset);
}


int 
exosfile::aio_flush()
{
  if (sManager->debug) fprintf(stderr,"... aio_flush - opened:%d\n", mOpened);
  int retc=0;

  if (!mOpened)
    return -EBADF;

  if (! ((mFlags & O_WRONLY) || (mFlags & O_RDWR)) )
    return 0;

  std::lock_guard<std::mutex> lock(write_mutex);
  if (mSeqWriteHandler)
  {
    if (mSeqWriteHandler->buffer.length()) {
      std::vector<extents_t> objmap = object_extents(mSeqWriteHandler->offset,mSeqWriteHandler->buffer.length());
      assert (objmap.size()==1);
      retc = sManager->getIoCtx(mDataPool)->write(objmap[0].oid, mSeqWriteHandler->buffer, objmap[0].len, objmap[0].oid_offset);
    if (sManager->debug) fprintf(stderr,"... seq-flush %s off:%lu len:%lu retc:%d\n",
				 objmap[0].oid.c_str(), objmap[0].offset, objmap[0].len, retc);
    }
    mSeqWriteHandler = 0;
  }

  for (auto it = ChunkWMap.begin(); it != ChunkWMap.end();)
  {
    (*it)->completion->wait_for_safe_and_cb();
    retc |= (*it)->completion->get_return_value();
    auto delit = it;
    ++it;
    ChunkWMap.erase(delit);
  }

  retc |= store_md();
  return retc;
}

int
exosfile::aio_collect()
{
  if (sManager->debug) fprintf(stderr,"... aio_collect\n");
  int retc=0;
  for (auto it = ChunkWMap.begin(); it != ChunkWMap.end();)
  {
    if ((*it)->completion->is_safe_and_cb())
    {
      retc |= (*it)->completion->get_return_value();
      auto delit = it;
      ++it;
      ChunkWMap.erase(delit);
    }
  }
  if (sManager->debug) fprintf(stderr,"... aio_collect retc=%d\n", retc);
  return retc;
}

int 
exosfile::setxattr(const std::map<std::string,std::string>& xattr)
{
  if (prepare())
    return -ENOTCONN;

  std::map<std::string, librados::bufferlist> omap;

  librados::ObjectWriteOperation op;
  for (auto it = xattr.begin(); it != xattr.end(); ++it) {
    if (it->first.substr(0, std::string(EXOSMANAGER_XATTR_RESERVED_PREFIX).length()) == std::string(EXOSMANAGER_XATTR_RESERVED_PREFIX)) {
      fprintf(stderr,"error: attribute key uses the reserved namespace prefix %s\n", EXOSMANAGER_XATTR_RESERVED_PREFIX);
      return -EINVAL;
    }
    omap[it->first].append(it->second);
  }
  op.omap_set(omap);
  int retc = sManager->getIoCtx(mPool)->operate(mName, &op);
  if (sManager->debug) fprintf(stderr,"... store user xattr retc=%d\n", retc);
  return retc;
}

int 
exosfile::getxattr(std::map<std::string,std::string>& xattr)
{
  if (prepare())
    return -ENOTCONN;

  std::map<std::string, librados::bufferlist> omap;
  bool omore;
  int  prval;
  librados::ObjectReadOperation op;
  
  op.omap_get_vals2(std::string(""),
		    1024, 
		    &omap,
		    &omore,
		    &prval);

  int retc = sManager->getIoCtx(mPool)->operate(mName, &op, 0);
  if (sManager->debug) fprintf(stderr,"... get user xattr retc=%d\n", retc);
  for (auto it = omap.begin(); it != omap.end(); ++it) {
    if (it->first.substr(0, std::string(EXOSMANAGER_XATTR_RESERVED_PREFIX).length()) == std::string(EXOSMANAGER_XATTR_RESERVED_PREFIX))
      continue;
    std::string value(it->second.c_str(), it->second.length());
    xattr[it->first] = value;
  }

  return retc;
}


int 
exosfile::rmxattr(const std::set<std::string>& xattr)
{
  if (prepare())
    return -ENOTCONN;
  
  std::map<std::string, librados::bufferlist> omap;

  librados::ObjectWriteOperation op;
  op.omap_rm_keys(xattr);
  int retc = sManager->getIoCtx(mPool)->operate(mName, &op);
  if (sManager->debug) fprintf(stderr,"... delete user xattr retc=%d\n", retc);
  return retc;
}


int 
exosfile::lock(bool exclusive, time_t duration)
{
  std::lock_guard<std::mutex> lock(exist_mutex);

  if (sManager->debug) fprintf(stderr,"... lock exclusive=%d duration=%lu locked=%d\n", exclusive, duration, mLocked);

  if (mLocked) {
    return -EAGAIN;
  }

  time_t now = time(NULL);
  struct timeval tv;
  tv.tv_sec = duration;
  int retc = 0;

  if (exclusive) {
    retc = sManager->getIoCtx(mPool)->lock_exclusive(mName, "filelock", mUUID, "exclusive file lock", 0, 0);
  } else {
    retc = sManager->getIoCtx(mPool)->lock_shared(mName, "filelock", mUUID, "shared file lock", "tag", 0, 0);
  }

  if (!retc)
  {
    mLocked = true;
    if (exclusive)
      mLockedExclusive = true;
    mLockExpires = now + duration;
  }
  return retc;
}

int 
exosfile::unlock(bool breakall)
{
  std::lock_guard<std::mutex> lock(exist_mutex);

  int retc = 0;

  if (sManager->debug) fprintf(stderr,"... lock exclusive=%d expires=%lu (%ld) locked=%d\n", mLockedExclusive, mLockExpires, mLockExpires-time(NULL), mLocked);

  if (!breakall) {
    if (!mLocked) {
      return 0;
    }
    
    retc = sManager->getIoCtx(mPool)->unlock(mName,"filelock", mUUID);
    
    if (!retc) {
      mLocked = false;
      mLockedExclusive = false;
      mLockExpires = 0;
    }
  }
  return retc;
}

bool
exosfile::locked()
{
  return mLocked;
}

bool 
exosfile::locked_exclusive()
{
  return (mLocked && mLockedExclusive);
}

int 
exosfile::unlink(ssize_t offset)
{
  int retc = 0;
  int p_retc = 0;

  std::lock_guard<std::mutex> lock(exist_mutex);

  if (sManager->debug) fprintf(stderr,"... unlink off=%ld opened=%d\n", offset, mOpened);

  if (!mOpened)
    return -EBADF;
  
  // for offset = -1, this function deleted the namespace object and all extents
  // for offset >= 0 it truncates and removes and keeps the namespace object
  
  ssize_t len = (offset >=0)? ((mSize > offset)? (mSize - offset) : (1)): mSize;
  ssize_t ext_offset = (offset >=0)? offset:0; // map -1 to offset 0

  std::vector<extents_t> objmap = object_extents(ext_offset, len);

  std::map<std::string, std::shared_ptr<AsyncHandler>> deletions;

  if (sManager->debug) fprintf(stderr,"... unlink extents off=%ld len=%ld\n", ext_offset, len);

  // delete chunks
  for (auto it = objmap.begin(); it != objmap.end(); ++it) {
    if ( (it == objmap.begin() && (it->oid_offset)) ) {
      if (sManager->debug) fprintf(stderr,"... truncate oid=%s offset=%lu\n", it->oid.c_str(), it->oid_offset);
      // truncate this oid, we do this synchronously, there is no aio_truncate (?)
      librados::ObjectWriteOperation op;
      op.truncate(it->oid_offset);
      p_retc = sManager->getIoCtx(mPool)->operate(it->oid, &op);
      if (p_retc != -ENOENT)
	retc |= p_retc;
    } else {
      if (sManager->debug) fprintf(stderr,"... delete oid=%s\n", it->oid.c_str());
      // delete this oid
      deletions[it->oid] = std::make_shared<AsyncHandler>(it->offset, it->len);
      sManager->getIoCtx(mDataPool)->aio_remove(it->oid, deletions[it->oid]->completion);
    }
  }

  // in case of full unlink
  if ( offset < 0) {
    // delete namespace object
    if (sManager->debug) fprintf(stderr,"... delete oid=%s\n", mName.c_str());
    deletions[mName] = std::make_shared<AsyncHandler>(0, mSize);
    sManager->getIoCtx(mPool)->aio_remove(mName, deletions[mName]->completion);
  }
    
  // collect asynchronous deletions
  for (auto it = deletions.begin(); it != deletions.end() ; ++it) {
    it->second->completion->wait_for_complete_and_cb();
    p_retc = it->second->completion->get_return_value();
    if (p_retc != -ENOENT)
      retc |= p_retc;
  }

  // in case of full unlink
  if ( offset < 0) {
    deletions[mName]->completion->wait_for_complete_and_cb();
    p_retc |= deletions[mName]->completion->get_return_value();
    if (p_retc != -ENOENT)
      retc |= p_retc;
  }

  // store the new offset
  if (offset>=0)
    // store the offset
    mSize = offset;
  else
    // an unlink automatically closes a file
    mOpened = false;

  return retc;
}

std::string
exosfile::nextInode()
{
  int retc = 0;
  std::string oid = EXOSMANAGER_OBJECT;

  std::set<std::string> keys;
  std::map<std::string, librados::bufferlist> omap;
  keys.insert(EXOSMANAGER_INODE_KEY);

  std::string new_inode;

  do {
    librados::ObjectReadOperation op;
    op.omap_get_vals_by_keys(keys, &omap, 0);
    op.assert_exists();
    
    retc = sManager->getIoCtx(params["rados.md"])->operate(oid, &op, 0);
    if (retc == -2)
    {
      // create the initial inode
      librados::ObjectWriteOperation op;
      omap[EXOSMANAGER_INODE_KEY].clear();
      omap[EXOSMANAGER_INODE_KEY].append("0000000000000000");
      op.create(true);
      op.omap_set(omap);
      retc = sManager->getIoCtx(params["rados.md"])->operate(oid, &op);
      if (sManager->debug) fprintf(stderr,"... setino retc=%d\n", retc);
    }
  } while(retc);

  do {
    librados::ObjectReadOperation op;
    op.omap_get_vals_by_keys(keys, &omap, 0);
    op.assert_exists();
    
    retc = sManager->getIoCtx(params["rados.md"])->operate(oid, &op, 0);
    if (retc)
      break;

    librados::bufferlist bl = omap[EXOSMANAGER_INODE_KEY];
    std::string value(bl.c_str(), bl.length());
    fprintf(stderr,"value=%s\n", value.c_str());
    uint64_t currentino = strtoull(value.c_str(), 0, 16);

    currentino ++;
    char newino[256];
    snprintf(newino,sizeof(newino), "%016lx", currentino);
    librados::ObjectWriteOperation wop;
    omap[EXOSMANAGER_INODE_KEY].clear();
    omap[EXOSMANAGER_INODE_KEY].append(newino);
    wop.omap_set(omap);

    int r;
    std::map<std::string, std::pair<librados::bufferlist, int> > assertions;
    assertions[EXOSMANAGER_INODE_KEY] = std::make_pair(bl, LIBRADOS_CMPXATTR_OP_EQ);
    wop.omap_cmp(assertions, &r);

    retc = sManager->getIoCtx(params["rados.md"])->operate(oid, &wop);
    if (sManager->debug) fprintf(stderr,"... setino retc=%d\n", retc);
    new_inode = newino;

  } while(retc);

  if (sManager->debug) fprintf(stderr,"... nextino retc=%d\n", retc);
  
  return new_inode;
}

std::string
exosfile::timespec2string(struct timespec& ltime)
{
  char stime[256];
  snprintf(stime,sizeof(stime),"%lu.%lu", ltime.tv_sec, ltime.tv_nsec);
  return stime;
}


struct timespec
exosfile::string2timespec(std::string stime)
{
  struct timespec spec;
  spec.tv_sec = spec.tv_nsec = 0;
  std::string tvsec;
  std::string tvnsec;
  size_t dotpos = stime.find(".");
  if (dotpos != std::string::npos)
  {
    tvsec = stime.substr(0, dotpos);
    tvnsec = stime.substr(dotpos+1);
    spec.tv_sec = strtoull(tvsec.c_str(),0,10);
    spec.tv_nsec = strtoull(tvnsec.c_str(),0,10);
  }
  return spec;
}


int 
exosfile::open_md()
{
  int retc = get_md();
  mOpened = (!retc)?true:false;
  return retc;
}


int 
exosfile::create_md()
{
  std::map<std::string, librados::bufferlist> omap;
  // get a new inode
  mInode = nextInode();

  clock_gettime(CLOCK_REALTIME, &mMtime);

  librados::ObjectWriteOperation op;
  omap[EXOSMANAGER_INODE_KEY].append(mInode);
  omap[EXOSMANAGER_SIZE_KEY].append("0");
  omap[EXOSMANAGER_MTIME_KEY].append(timespec2string(mMtime));
  omap[EXOSMANAGER_POOL_KEY].append(mDataPool);

  op.create(true);
  op.omap_set(omap);
  int retc = sManager->getIoCtx(mPool)->operate(mName, &op);
  if (sManager->debug) fprintf(stderr,"... createino retc=%d\n", retc);
  mOpened = (!retc)?true:false;
  return retc;
}

int 
exosfile::store_md()
{
  std::map<std::string, librados::bufferlist> omap;

  clock_gettime(CLOCK_REALTIME, &mMtime);

  char ssize[1024];
  snprintf(ssize, sizeof(ssize),"%lu", mSize);

  librados::ObjectWriteOperation op;
  omap[EXOSMANAGER_INODE_KEY].append(mInode);
  omap[EXOSMANAGER_SIZE_KEY].append(ssize);
  omap[EXOSMANAGER_MTIME_KEY].append(timespec2string(mMtime));
  omap[EXOSMANAGER_POOL_KEY].append(mDataPool);

  op.omap_set(omap);
  int retc = sManager->getIoCtx(mPool)->operate(mName, &op);
  if (sManager->debug) fprintf(stderr,"... store md size=%s mtime=%s retc=%d\n", ssize, timespec2string(mMtime).c_str(), retc);
  return retc;
}


int 
exosfile::get_md()
{
  std::set<std::string> keys;
  std::map<std::string, librados::bufferlist> omap;
  keys.insert(EXOSMANAGER_INODE_KEY);
  keys.insert(EXOSMANAGER_SIZE_KEY);
  keys.insert(EXOSMANAGER_MTIME_KEY);
  keys.insert(EXOSMANAGER_POOL_KEY);

  std::string new_inode;

  librados::ObjectReadOperation op;
  op.omap_get_vals_by_keys(keys, &omap, 0);
  op.assert_exists();
  
  int retc = sManager->getIoCtx(mPool)->operate(mName, &op, 0);
  if (!retc)
  {
    mInode = std::string(omap[EXOSMANAGER_INODE_KEY].c_str(), omap[EXOSMANAGER_INODE_KEY].length());
    mSize = strtoull(std::string(omap[EXOSMANAGER_SIZE_KEY].c_str(), omap[EXOSMANAGER_SIZE_KEY].length()).c_str(),0,10);
    mMtime = string2timespec(std::string(omap[EXOSMANAGER_MTIME_KEY].c_str(), omap[EXOSMANAGER_MTIME_KEY].length()));
    mDataPool = std::string(omap[EXOSMANAGER_POOL_KEY].c_str(), omap[EXOSMANAGER_POOL_KEY].length());

    if (sManager->debug) fprintf(stderr,"... md:%s => data:%s size:%lu mtime:%lu.%lu pool:%s/%s\n", 
				 mName.c_str(), mInode.c_str(), mSize, mMtime.tv_sec, mMtime.tv_nsec, mPool.c_str(),mDataPool.c_str());
  }
  return retc;
}

std::vector<exosfile::extents_t> 
exosfile::object_extents(off_t offset, uint64_t len)
{
  std::vector<extents_t> objmap;
  extents_t extent;
  size_t obj_begin = offset / mBlockSize;
  size_t obj_end = (offset + len) / mBlockSize;

  fprintf(stderr,"block %lu=>%lu\n", obj_begin, obj_end);

  off_t next_offset =offset;
  char suffix[16];
  snprintf(suffix, sizeof(suffix), "%04lx", obj_begin);
  // compute first extent                                                                                                      
  extent.oid = mInode + std::string("#") + suffix;
  extent.offset = offset;
  extent.len = ( (offset + len) > ((obj_begin+1)*mBlockSize) ) ? ( ((obj_begin+1)*mBlockSize)-offset): len;
  next_offset += extent.len;
  extent.oid_offset =(offset - (obj_begin*mBlockSize));
  objmap.push_back(extent);

  // compute intermediate extent                                                                                               
  for(size_t i = obj_begin+1; i< ((obj_end?obj_end:1)); ++i) {
    snprintf(suffix, sizeof(suffix), "%04lx", i);
    extent.oid = mInode + std::string("#") + suffix;
    extent.offset = next_offset;
    extent.len = mBlockSize;
    next_offset += extent.len;
    extent.oid_offset= 0;
    if (extent.len)
      objmap.push_back(extent);
  }
  
  // compute last extent                                                                                                       
  if (obj_end> obj_begin) {
    snprintf(suffix, sizeof(suffix), "%04lx", obj_end);
    extent.oid = mInode + std::string("#") + suffix;
    extent.offset = next_offset;
    extent.len = (offset + len - next_offset);
    extent.oid_offset= 0;
    if (extent.len)
      objmap.push_back(extent);
  }

  if (sManager->debug) {
    for (size_t i = 0; i < objmap.size(); ++i) {
      fprintf(stderr,"%04lu: oid=%s off=%lu len=%lu oid-off=%lu\n", i , objmap[i].oid.c_str(), objmap[i].offset, objmap[i].len, objmap[i].oid_offset);
    }
  }
  return objmap;
}
