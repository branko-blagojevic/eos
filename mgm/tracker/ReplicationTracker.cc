// ----------------------------------------------------------------------
// File: ReplicationTracker.cc
// Author: Andreas-Joachim Peters - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2019 CERN/Switzerland                                  *
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

#include "common/Path.hh"
#include "common/FileId.hh"
#include "common/IntervalStopwatch.hh"
#include "common/LayoutId.hh"
#include "mgm/Master.hh"
#include "mgm/FsView.hh"
#include "mgm/tracker/ReplicationTracker.hh"
#include "mgm/proc/ProcCommand.hh"
#include "mgm/XrdMgmOfs.hh"
#include "namespace/interface/IView.hh"
#include "namespace/Resolver.hh"
#include "namespace/Prefetcher.hh"

EOSMGMNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
ReplicationTracker::ReplicationTracker(const char* path) : mPath(path)
{
  mVid = eos::common::VirtualIdentity::Root();
  mThread.reset(&ReplicationTracker::backgroundThread, this);

}

//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
ReplicationTracker::~ReplicationTracker()
{
  mThread.join();
}

//------------------------------------------------------------------------------
// Create a new file
//------------------------------------------------------------------------------
void 
ReplicationTracker::Create(std::shared_ptr<eos::IFileMD> fmd)
{
  if (!enabled())
    return;

  std::string prefix = Prefix(fmd);
  std::string tag = prefix + eos::common::FileId::Fid2Hex(fmd->getId());
  std::shared_ptr<eos::IContainerMD> dmd;
  
  try {
    gOFS->eosView->createContainer(prefix, true);
    dmd = gOFS->eosView->getContainer(prefix);
    dmd->setCTimeNow();
    gOFS->eosView->updateContainerStore(dmd.get());
  } catch (const MDException& e) {
  }

  try {
    fmd = gOFS->eosView->createFile(tag.c_str(), 0, 0);
  } catch (const MDException& e) {
    eos_static_crit("failed to create tag file='%s'", tag.c_str());
    return;
  }

  std::string uri = gOFS->eosView->getUri(fmd.get());
  eos_static_info("op=created tag='%s' uri='%s'", tag.c_str(),uri.c_str());
  return;
}

//------------------------------------------------------------------------------
// Commit a file
//------------------------------------------------------------------------------
void 
ReplicationTracker::Commit(std::shared_ptr<eos::IFileMD> fmd)
{
  if (!enabled())
    return;

  // check if this is still a 'temporary' name
  if (fmd->getName().substr(0,strlen(EOS_COMMON_PATH_ATOMIC_FILE_PREFIX)) == EOS_COMMON_PATH_ATOMIC_FILE_PREFIX) {
    // check if this is still a 'temporary' name
    return;
  }

  // check replica count
  if (fmd->getNumLocation() == (eos::common::LayoutId::GetStripeNumber(fmd->getLayoutId())+1)) {
    std::string prefix = Prefix(fmd);
    std::string tag = prefix + eos::common::FileId::Fid2Hex(fmd->getId());
    std::string uri = gOFS->eosView->getUri(fmd.get());
    std::shared_ptr<eos::IFileMD> entry_fmd;
    
    try {
      entry_fmd = gOFS->eosView->getFile(tag);
      gOFS->eosView->unlinkFile(entry_fmd.get());
    } catch (const MDException& e) {
      if (e.getErrno() != ENOENT) {
	eos_static_crit("failed to remove tag file='%s' error='%s'", tag.c_str(), e.what());
      }
      return;
    }

    eos_static_info("op=removed tag='%s' uri='%s'", tag.c_str(), uri.c_str());
  }
  return;
}

//------------------------------------------------------------------------------
// Validate a file
//------------------------------------------------------------------------------
void 
ReplicationTracker::Validate(std::shared_ptr<eos::IFileMD> fmd)
{
}

//------------------------------------------------------------------------------
// Get Tag file prefix
//------------------------------------------------------------------------------
std::string 
ReplicationTracker::Prefix(std::shared_ptr<eos::IFileMD> fmd)
{
  char strackerfile[4096];
  eos::IFileMD::ctime_t ctime;

  fmd->getCTime(ctime);
  time_t now = ctime.tv_sec;
  struct tm nowtm;
  localtime_r(&now, &nowtm);
  snprintf(strackerfile, sizeof(strackerfile), "%s/%04u/%02u/%02u/",
	   mPath.c_str(),
	   1900 + nowtm.tm_year,
	   nowtm.tm_mon + 1,
	   nowtm.tm_mday);
  
  return strackerfile;
}

//------------------------------------------------------------------------------
// Retrieve current LRU configuration options
//------------------------------------------------------------------------------
ReplicationTracker::Options ReplicationTracker::getOptions()
{
  eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
  ReplicationTracker::Options opts;

  // Default options                                                                                                             
  opts.enabled = false;
  opts.interval = std::chrono::minutes(60);

  if (FsView::gFsView.mSpaceView.count("default") &&
      (FsView::gFsView.mSpaceView["default"]->GetConfigMember("tracker") == "on")) {
    opts.enabled = true;
  }

  if (opts.enabled) {
    enable();
    eos_static_debug("creation tracker is enabled");
  } else {
    disable();
  }

  // this is hardcoded to 2 days, it could be 'dangerous' to make this really configurable
  opts.atomic_cleanup_age = 2*86400;
  return opts;
}


//------------------------------------------------------------------------------
// Background Thread cleaning up left-over atomic uploads
//------------------------------------------------------------------------------
void
ReplicationTracker::backgroundThread(ThreadAssistant& assistant) noexcept
{
  gOFS->WaitUntilNamespaceIsBooted(assistant);

  // set the initial state after boot
  Options opts = getOptions();
  if (opts.enabled) {
    enable();
  } else {
    disable();
  }


  assistant.wait_for(std::chrono::seconds(10));
  eos_static_info("msg=\"async thread started\"");

  while (!assistant.terminationRequested()) {
    // every now and then we wake up
    Options opts = getOptions();

    // Only a master needs to run a ReplicationTracker
    if (opts.enabled) {
      enable();
    } else {
      disable();
    }

    common::IntervalStopwatch stopwatch( enabled()? opts.interval : std::chrono::seconds(10) );

    if (opts.enabled && gOFS->mMaster->IsMaster()) {
      eos_static_info("msg=\"scan started!\"");
      Scan(opts.atomic_cleanup_age, true, 0);
      eos_static_info("msg=\"scan finished!\"");
    }

    assistant.wait_for(stopwatch.timeRemainingInCycle());
  }
}


//------------------------------------------------------------------------------
// Scan entries in creation tracker - opt cleanup or output
//------------------------------------------------------------------------------
void
ReplicationTracker::Scan(uint64_t atomic_age, bool cleanup, std::string* out)
{
  eos::common::RWMutexReadLock viewReadLock;

  time_t now = time(NULL);

  std::map<std::string, std::set<std::string> > found;
  XrdOucString stdErr;

  if (!enabled()) {
    *out += "# tracker is disabled - use 'eos space config default space.tracker=on'\n";
  }

  if (!gOFS->_find(mPath.c_str(), 
		   mError,
		   stdErr,
		   mVid,
		   found,
		   0,
		   0,
		   false,
		   10)
      ) {
    for (auto rfoundit = found.rbegin(); rfoundit != found.rend(); rfoundit++) {

      if (!rfoundit->second.size()) {
	std::string creationpath = mPath + "/";
	if (rfoundit->first == creationpath) {
	  // don't delete the creation proc entry
	  continue;
	}
	std::shared_ptr<eos::IContainerMD> dmd;
	eos::IContainerMD::ctime_t ctime;

	// delete this directory if it is older than atomic_age
	eos::common::RWMutexWriteLock viewWriteLock;
	viewWriteLock.Grab(gOFS->eosViewRWMutex);
	

	try {
	  dmd = gOFS->eosView->getContainer(rfoundit->first);
	  dmd->getCTime(ctime);
	  uint64_t age = now - ctime.tv_sec;
	  if (age > atomic_age &&
	      !dmd->getNumFiles() &&
	      !dmd->getNumContainers()) {
	    gOFS->eosView->removeContainer(rfoundit->first);
	  }
	} catch (const MDException& e) {
	  eos_static_crit("failed to remove directory='%s'", rfoundit->first.c_str());
	}
	viewWriteLock.Release();

      } else {

	for (auto fileit = rfoundit->second.begin(); fileit != rfoundit->second.end();
	     fileit++) {
	  std::string fspath = rfoundit->first;
	  std::string entry = *fileit;
	  std::string entry_path = fspath + "/" + entry;
	  
	  XrdOucString fxid = "fxid:";
	  std::string fullpath;
	  bool flag_deletion = false;
	  bool is_atomic = false;
	  std::string reason = "KEEPIT";
	  
	  fxid += entry.c_str();
	  
	  std::shared_ptr<eos::IFileMD> fmd;
	  std::shared_ptr<eos::IFileMD> entry_fmd;
	  
	  size_t n_rep=0;
	  size_t n_layout_rep=0;
	  
	  unsigned long long fid = Resolver::retrieveFileIdentifier(
								    fxid).getUnderlyingUInt64();
	  
	  eos::IFileMD::ctime_t ctime;
	  // reference by fxid
	  eos::Prefetcher::prefetchFileMDAndWait(gOFS->eosView, fid);
	  
	  viewReadLock.Grab(gOFS->eosViewRWMutex);
	  try {
	    fmd = gOFS->eosFileService->getFileMD(fid);
	    fmd->getCTime(ctime);
	    fullpath = gOFS->eosView->getUri(fmd.get());
	    
	    if (fmd->getName().substr(0,strlen(EOS_COMMON_PATH_ATOMIC_FILE_PREFIX)) == EOS_COMMON_PATH_ATOMIC_FILE_PREFIX) {
	      is_atomic = true;
	    }
	    
	    n_rep = fmd->getNumLocation();
	    n_layout_rep = (eos::common::LayoutId::GetStripeNumber(fmd->getLayoutId())+1);
	    if (n_rep < n_layout_rep) {
	      reason = "REPLOW";
	    } else {
	      reason = "REP-OK";
	      flag_deletion=true;
	    }
	  } catch (eos::MDException& e) {
	    errno = e.getErrno();
	    stdErr = "error: cannot retrieve file meta data - ";
	    stdErr += e.getMessage().str().c_str();
	    eos_static_debug("caught exception %d %s\n", e.getErrno(),
			     e.getMessage().str().c_str());
	    reason = "ENOENT";
	    flag_deletion = true;
	    ctime.tv_sec = now - atomic_age -1;
	  }       
	  
	  viewReadLock.Release();
	  
	  uint64_t age = now - ctime.tv_sec;
	  
	  if (is_atomic && (age >  atomic_age)) {
	    flag_deletion = true;
	    reason = "ATOMIC";
	  }
	  
	  if (out) {
	    if (reason == "ENOENT") {
	      // don't show files which had been deleted
	      continue;
	    }

	    char outline[16384];
	    snprintf(outline, sizeof(outline), "key=%s age=%lu (s) delete=%d rep=%lu/%lu atomic=%d reason=%s uri='%s'\n", 
		     entry.c_str(), 
		     age,
		     flag_deletion,
		     n_rep,
		     n_layout_rep,
		     is_atomic,
		     reason.c_str(),
		     fullpath.c_str());
	    *out += outline;

	    if (out->size() > (128*1024*1024)) {
	      * out += "# ... list has been truncated\n";
	      return;
	    }
	  } else {

	    if (reason == "ENOENT") {
	      // mark for tag deletion
	      flag_deletion = 1;
	    }

	    eos_static_info("key=%s age=%lu (s) delete=%d rep=%lu/%lu atomic=%d reason=%s uri='%s'", 
			    entry.c_str(), 
			    age,
			    flag_deletion,
			    n_rep, 
			    n_layout_rep,
			    is_atomic,
			    reason.c_str(),
			    fullpath.c_str());
	  }
	  
	  if (cleanup && flag_deletion) {
	    eos::common::RWMutexWriteLock viewWriteLock;
	    viewWriteLock.Grab(gOFS->eosViewRWMutex);
	    
	    // cleanup the tag entry
	    try {
	      entry_fmd = gOFS->eosView->getFile(entry_path);
	      gOFS->eosView->unlinkFile(entry_fmd.get());
	    } catch (const MDException& e) {
	      eos_static_crit("failed to remove tag file='%s'", entry_path.c_str());
	    }	  
	    
	    if (reason == "ATOMIC") {
	      // cleanup the atomic left-over
	      try {
		fmd = gOFS->eosFileService->getFileMD(fid);
		gOFS->eosView->unlinkFile(fmd.get());
	      } catch (const MDException& e) {
		eos_static_crit("failed to cleanup atomic target file='%s'", fullpath.c_str());
	      }
	    }
	    viewWriteLock.Release();
	  }
	}
      }
    }
  } else {
    eos_static_err("find failed in path='%s' errmsg='%s'",
		   mPath.c_str(),
		   stdErr.c_str());
  }
}

EOSMGMNAMESPACE_END

