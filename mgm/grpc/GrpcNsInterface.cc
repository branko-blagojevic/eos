// ----------------------------------------------------------------------
// File: GrpcNsInterface.cc
// Author: Andreas-Joachim Peters - CERN
// ----------------------------------------------------------------------

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

/*----------------------------------------------------------------------------*/
#include "GrpcNsInterface.hh"
/*----------------------------------------------------------------------------*/
#include "common/LayoutId.hh"
#include "mgm/Acl.hh"
#include "namespace/Prefetcher.hh"
#include "namespace/MDException.hh"
#include "namespace/interface/ContainerIterators.hh"

#include "mgm/XrdMgmOfs.hh"

/*----------------------------------------------------------------------------*/


EOSMGMNAMESPACE_BEGIN

#ifdef EOS_GRPC
grpc::Status
GrpcNsInterface::GetMD(eos::common::VirtualIdentity& vid,
                       grpc::ServerWriter<eos::rpc::MDResponse>* writer,
                       const eos::rpc::MDRequest* request, bool check_perms)
{
  if (request->type() == eos::rpc::FILE) {
    // stream file meta data
    eos::common::RWMutexReadLock viewReadLock;
    std::shared_ptr<eos::IFileMD> fmd;
    std::shared_ptr<eos::IContainerMD> pmd;
    unsigned long fid = 0;
    uint64_t clock = 0;
    std::string path;

    if (request->id().ino()) {
      // get by inode
      fid = eos::common::FileId::InodeToFid(request->id().ino());
    } else if (request->id().id()) {
      // get by fileid
      fid = request->id().id();
    }

    if (fid) {
      eos::Prefetcher::prefetchFileMDAndWait(gOFS->eosView, fid);
    } else {
      eos::Prefetcher::prefetchFileMDAndWait(gOFS->eosView, request->id().path());
    }

    viewReadLock.Grab(gOFS->eosViewRWMutex);

    if (fid) {
      try {
        fmd = gOFS->eosFileService->getFileMD(fid, &clock);
        path = gOFS->eosView->getUri(fmd.get());

        if (check_perms) {
          pmd = gOFS->eosDirectoryService->getContainerMD(fmd->getContainerId());
        }
      } catch (eos::MDException& e) {
        errno = e.getErrno();
        eos_static_debug("caught exception %d %s\n", e.getErrno(),
                         e.getMessage().str().c_str());
        return grpc::Status((grpc::StatusCode)(errno), e.getMessage().str().c_str());
      }
    } else {
      try {
        fmd = gOFS->eosView->getFile(request->id().path());
        path = gOFS->eosView->getUri(fmd.get());

        if (check_perms) {
          pmd = gOFS->eosDirectoryService->getContainerMD(fmd->getContainerId());
        }
      } catch (eos::MDException& e) {
        errno = e.getErrno();
        eos_static_debug("caught exception %d %s\n", e.getErrno(),
                         e.getMessage().str().c_str());
        return grpc::Status((grpc::StatusCode)(errno), e.getMessage().str().c_str());
      }
    }

    if (check_perms && !Access(vid, R_OK, pmd)) {
      return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                          "access to parent container denied");
    }

    // create GRPC protobuf object
    eos::rpc::MDResponse gRPCResponse;
    gRPCResponse.set_type(eos::rpc::FILE);
    eos::rpc::FileMdProto gRPCFmd;
    gRPCResponse.mutable_fmd()->set_name(fmd->getName());
    gRPCResponse.mutable_fmd()->set_id(fmd->getId());
    gRPCResponse.mutable_fmd()->set_cont_id(fmd->getContainerId());
    gRPCResponse.mutable_fmd()->set_uid(fmd->getCUid());
    gRPCResponse.mutable_fmd()->set_gid(fmd->getCGid());
    gRPCResponse.mutable_fmd()->set_size(fmd->getSize());
    gRPCResponse.mutable_fmd()->set_layout_id(fmd->getLayoutId());
    gRPCResponse.mutable_fmd()->set_flags(fmd->getFlags());
    gRPCResponse.mutable_fmd()->set_link_name(fmd->getLink());
    eos::IFileMD::ctime_t ctime;
    eos::IFileMD::ctime_t mtime;
    fmd->getCTime(ctime);
    fmd->getMTime(mtime);
    gRPCResponse.mutable_fmd()->mutable_ctime()->set_sec(ctime.tv_sec);
    gRPCResponse.mutable_fmd()->mutable_ctime()->set_n_sec(ctime.tv_nsec);
    gRPCResponse.mutable_fmd()->mutable_mtime()->set_sec(mtime.tv_sec);
    gRPCResponse.mutable_fmd()->mutable_mtime()->set_n_sec(mtime.tv_nsec);
    gRPCResponse.mutable_fmd()->mutable_checksum()->set_value(
      fmd->getChecksum().getDataPtr(), fmd->getChecksum().size());
    gRPCResponse.mutable_fmd()->mutable_checksum()->set_type(
      eos::common::LayoutId::GetChecksumStringReal(fmd->getLayoutId()));

    for (const auto& loca : fmd->getLocations()) {
      gRPCResponse.mutable_fmd()->add_locations(loca);
    }

    for (const auto& loca : fmd->getUnlinkedLocations()) {
      gRPCResponse.mutable_fmd()->add_unlink_locations(loca);
    }

    for (const auto& elem : fmd->getAttributes()) {
      (*gRPCResponse.mutable_fmd()->mutable_xattrs())[elem.first] = elem.second;
    }

    gRPCResponse.mutable_fmd()->set_path(path);
    writer->Write(gRPCResponse);
    return grpc::Status::OK;
  } else if (request->type() == eos::rpc::CONTAINER) {
    // stream container meta data
    eos::common::RWMutexReadLock viewReadLock;
    std::shared_ptr<eos::IContainerMD> cmd;
    std::shared_ptr<eos::IContainerMD> pmd;
    unsigned long cid = 0;
    uint64_t clock = 0;
    std::string path;

    if (request->id().ino()) {
      // get by inode
      cid = request->id().ino();
    } else if (request->id().id()) {
      // get by containerid
      cid = request->id().id();
    }

    if (!cid) {
      eos::Prefetcher::prefetchContainerMDAndWait(gOFS->eosView,
          request->id().path());
    }

    viewReadLock.Grab(gOFS->eosViewRWMutex);

    if (cid) {
      try {
        cmd = gOFS->eosDirectoryService->getContainerMD(cid, &clock);
        path = gOFS->eosView->getUri(cmd.get());
        pmd = gOFS->eosDirectoryService->getContainerMD(cmd->getParentId());
      } catch (eos::MDException& e) {
        errno = e.getErrno();
        eos_static_debug("caught exception %d %s\n", e.getErrno(),
                         e.getMessage().str().c_str());
        return grpc::Status((grpc::StatusCode)(errno), e.getMessage().str().c_str());
      }
    } else {
      try {
        cmd = gOFS->eosView->getContainer(request->id().path());
        path = gOFS->eosView->getUri(cmd.get());
        pmd = gOFS->eosDirectoryService->getContainerMD(cmd->getParentId());
      } catch (eos::MDException& e) {
        errno = e.getErrno();
        eos_static_debug("caught exception %d %s\n", e.getErrno(),
                         e.getMessage().str().c_str());
        return grpc::Status((grpc::StatusCode)(errno), e.getMessage().str().c_str());
      }
    }

    if (!Access(vid, R_OK, pmd)) {
      return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                          "access to parent container denied");
    }

    // create GRPC protobuf object
    eos::rpc::MDResponse gRPCResponse;
    gRPCResponse.set_type(eos::rpc::CONTAINER);
    eos::rpc::ContainerMdProto gRPCFmd;
    gRPCResponse.mutable_cmd()->set_name(cmd->getName());
    gRPCResponse.mutable_cmd()->set_id(cmd->getId());
    gRPCResponse.mutable_cmd()->set_parent_id(cmd->getParentId());
    gRPCResponse.mutable_cmd()->set_uid(cmd->getCUid());
    gRPCResponse.mutable_cmd()->set_gid(cmd->getCGid());
    gRPCResponse.mutable_cmd()->set_tree_size(cmd->getTreeSize());
    gRPCResponse.mutable_cmd()->set_flags(cmd->getFlags());
    eos::IContainerMD::ctime_t ctime;
    eos::IContainerMD::ctime_t mtime;
    eos::IContainerMD::ctime_t stime;
    cmd->getCTime(ctime);
    cmd->getMTime(mtime);
    cmd->getTMTime(stime);
    gRPCResponse.mutable_cmd()->mutable_ctime()->set_sec(ctime.tv_sec);
    gRPCResponse.mutable_cmd()->mutable_ctime()->set_n_sec(ctime.tv_nsec);
    gRPCResponse.mutable_cmd()->mutable_mtime()->set_sec(mtime.tv_sec);
    gRPCResponse.mutable_cmd()->mutable_mtime()->set_n_sec(mtime.tv_nsec);
    gRPCResponse.mutable_cmd()->mutable_stime()->set_sec(stime.tv_sec);
    gRPCResponse.mutable_cmd()->mutable_stime()->set_n_sec(stime.tv_nsec);

    for (const auto& elem : cmd->getAttributes()) {
      (*gRPCResponse.mutable_cmd()->mutable_xattrs())[elem.first] = elem.second;
    }

    gRPCResponse.mutable_cmd()->set_path(path);
    writer->Write(gRPCResponse);
    return grpc::Status::OK;
  }

  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "invalid argument");
}

grpc::Status
GrpcNsInterface::StreamMD(eos::common::VirtualIdentity& vid,
                          grpc::ServerWriter<eos::rpc::MDResponse>* writer,
                          const eos::rpc::MDRequest* request)
{
  // stream container meta data
  eos::common::RWMutexReadLock viewReadLock;
  std::shared_ptr<eos::IContainerMD> cmd;
  unsigned long cid = 0;
  uint64_t clock = 0;
  std::string path;

  if (request->id().ino()) {
    // get by inode
    cid = request->id().ino();
  } else if (request->id().id()) {
    // get by containerid
    cid = request->id().id();
  }

  if (!cid) {
    eos::Prefetcher::prefetchContainerMDWithChildrenAndWait(gOFS->eosView,
        request->id().path());
  }

  viewReadLock.Grab(gOFS->eosViewRWMutex);

  if (cid) {
    try {
      cmd = gOFS->eosDirectoryService->getContainerMD(cid, &clock);
      path = gOFS->eosView->getUri(cmd.get());
    } catch (eos::MDException& e) {
      errno = e.getErrno();
      eos_static_debug("caught exception %d %s\n", e.getErrno(),
                       e.getMessage().str().c_str());
      return grpc::Status((grpc::StatusCode)(errno), e.getMessage().str().c_str());
    }
  } else {
    try {
      cmd = gOFS->eosView->getContainer(request->id().path());
      path = gOFS->eosView->getUri(cmd.get());
    } catch (eos::MDException& e) {
      errno = e.getErrno();
      eos_static_debug("caught exception %d %s\n", e.getErrno(),
                       e.getMessage().str().c_str());
      return grpc::Status((grpc::StatusCode)(errno), e.getMessage().str().c_str());
    }
  }

  // stream the requested container
  eos::rpc::MDRequest c_dir;
  c_dir.mutable_id()->set_id(cid);
  c_dir.set_type(eos::rpc::CONTAINER);
  grpc::Status status;
  status = GetMD(vid, writer, &c_dir, true);

  if (!status.ok()) {
    return status;
  }

  bool first = true;

  // stream all the children files
  for (auto it = eos::FileMapIterator(cmd); it.valid(); it.next()) {
    eos::rpc::MDRequest c_file;
    c_file.mutable_id()->set_id(it.value());
    c_file.set_type(eos::rpc::FILE);
    status = GetMD(vid, writer, &c_file, first);

    if (!status.ok()) {
      return status;
    }

    first = false;
  }

  // stream all the children container
  for (auto it = eos::ContainerMapIterator(cmd); it.valid(); it.next()) {
    eos::rpc::MDRequest c_dir;
    c_dir.mutable_id()->set_id(it.value());
    c_dir.set_type(eos::rpc::CONTAINER);
    status = GetMD(vid, writer, &c_dir, first);

    if (!status.ok()) {
      return status;
    }

    first = false;
  }

  // finished streaming
  return grpc::Status::OK;
}

bool
GrpcNsInterface::Access(eos::common::VirtualIdentity& vid, int mode,
                        std::shared_ptr<eos::IContainerMD> cmd)
{
  // UNIX permissions
  if (cmd->access(vid.uid, vid.gid, mode)) {
    return true;
  }

  // ACLs - WARNING: this does not support ACLs to be linked attributes !
  eos::IContainerMD::XAttrMap xattr = cmd->getAttributes();
  eos::mgm::Acl acl(xattr, vid);

  // check for immutable
  if (vid.uid && !acl.IsMutable() && (mode & W_OK)) {
    return false;
  }

  bool permok = false;

  if (acl.HasAcl()) {
    permok = true;

    if ((mode & W_OK) && (!acl.CanWrite())) {
      permok = false;
    }

    if (mode & R_OK) {
      if (!acl.CanRead()) {
        permok = false;
      }
    }

    if (mode & X_OK) {
      if ((!acl.CanBrowse())) {
        permok = false;
      }
    }
  }

  return permok;
}

grpc::Status
GrpcNsInterface::FileInsert(eos::common::VirtualIdentity& vid,
                            eos::rpc::InsertReply* reply,
                            const eos::rpc::FileInsertRequest* request)

{
  if (!vid.sudoer) {
    // block every one who is not a sudoer
    reply->add_retc(EPERM);
    return grpc::Status::OK;
  }

  std::shared_ptr<eos::IFileMD> newfile;
  eos::common::RWMutexWriteLock lock(gOFS->eosViewRWMutex);
  std::vector<folly::Future<eos::IFileMDPtr>> conflicts;

  for (auto it : request->files()) {
    if (it.id() <= 0) {
      conflicts.emplace_back(eos::IFileMDPtr(
                               nullptr)); // folly::makeFuture<eos::IFileMDPtr>(eos::IFileMDPtr(nullptr)));
    } else {
      conflicts.emplace_back(gOFS->eosFileService->getFileMDFut(it.id()));
    }
  }

  int counter = -1;

  for (auto it : request->files()) {
    counter++;
    conflicts[counter].wait();

    if (!conflicts[counter].hasException() && conflicts[counter].get() != nullptr) {
      eos_static_err("Attempted to create file with id=%llu, which already exists",
                     it.id());
      reply->add_retc(EINVAL);
      continue;
    }

    eos_static_info("creating path=%s id=%lx", it.path().c_str(), it.id());

    try {
      try {
        newfile = gOFS->eosView->createFile(it.path(), it.uid(), it.gid(), it.id());
      } catch (eos::MDException& e) {
        std::ostringstream msg;
        msg << "Failed to call gOFS->eosView->createFile(): " << e.getMessage().str();
        e.getMessage().str(msg.str());
        throw;
      }
      eos::IFileMD::ctime_t ctime;
      eos::IFileMD::ctime_t mtime;
      ctime.tv_sec  = it.ctime().sec();
      ctime.tv_nsec = it.ctime().n_sec();
      mtime.tv_sec  = it.mtime().sec();
      mtime.tv_nsec = it.mtime().n_sec();
      newfile->setFlags(it.flags());
      newfile->setCTime(ctime);
      newfile->setMTime(mtime);
      newfile->setCUid(it.uid());
      newfile->setCGid(it.gid());
      newfile->setLayoutId(it.layout_id());
      newfile->setSize(it.size());
      newfile->setChecksum(it.checksum().value().c_str(),
                           it.checksum().value().size());

      for (auto attrit : it.xattrs()) {
        newfile->setAttribute(attrit.first, attrit.second);
      }

      for (auto locit : it.locations()) {
        newfile->addLocation(locit);
      }

      try {
        gOFS->eosView->updateFileStore(newfile.get());
      } catch (eos::MDException& e) {
        std::ostringstream msg;
        msg << "Failed to call gOFS->eosView->updateFileStore(): " << e.getMessage().str();
        e.getMessage().str(msg.str());
        throw;
      }

      reply->add_retc(0);
    } catch (eos::MDException& e) {
      eos_static_err("msg=\"exception\" ec=%d emsg=\"%s\" path=\"%s\" fxid=%08llx\n",
                     e.getErrno(), e.getMessage().str().c_str(), it.path().c_str(), it.id());
      reply->add_retc(-1);
    }
  }

  return grpc::Status::OK;
}


grpc::Status
GrpcNsInterface::ContainerInsert(eos::common::VirtualIdentity& vid,
                                 eos::rpc::InsertReply* reply,
                                 const eos::rpc::ContainerInsertRequest* request)

{
  if (!vid.sudoer) {
    // block every one who is not a sudoer
    reply->add_retc(EPERM);
    return grpc::Status::OK;
  }

  std::shared_ptr<eos::IContainerMD> newdir;
  eos::common::RWMutexWriteLock lock(gOFS->eosViewRWMutex);
  std::vector<folly::Future<eos::IContainerMDPtr>> conflicts;

  for (auto it : request->container()) {
    if (it.id() <= 0) {
      conflicts.emplace_back(eos::IContainerMDPtr(nullptr));
    } else {
      conflicts.emplace_back(gOFS->eosDirectoryService->getContainerMDFut(it.id()));
    }
  }

  int counter = -1;

  for (auto it : request->container()) {
    counter++;
    conflicts[counter].wait();

    if (!conflicts[counter].hasException() && conflicts[counter].get() != nullptr) {
      eos_static_err("Attempted to create container with id=%llu, which already exists",
                     it.id());
      reply->add_retc(EINVAL);
      continue;
    }

    eos_static_info("creating path=%s id=%lx", it.path().c_str(), it.id());

    try {
      try {
        newdir = gOFS->eosView->createContainer(it.path(), false, it.id());
      } catch (eos::MDException& e) {
        std::ostringstream msg;
        msg << "Failed to call gOFS->eosView->createContainer(): " << e.getMessage().str();
        e.getMessage().str(msg.str());
        throw;
      }
      eos::IContainerMD::ctime_t ctime;
      eos::IContainerMD::ctime_t mtime;
      eos::IContainerMD::ctime_t stime;
      ctime.tv_sec  = it.ctime().sec();
      ctime.tv_nsec = it.ctime().n_sec();
      mtime.tv_sec  = it.mtime().sec();
      mtime.tv_nsec = it.mtime().n_sec();
      stime.tv_sec  = it.stime().sec();
      stime.tv_nsec = it.stime().n_sec();
      // we can send flags or mode to store in flags ... sigh
      newdir->setFlags(it.mode() | it.flags());
      newdir->setCTime(ctime);
      newdir->setMTime(mtime);
      newdir->setTMTime(stime);
      newdir->setCUid(it.uid());
      newdir->setCGid(it.gid());
      newdir->setMode(it.mode() | S_IFDIR);

      for (auto attrit : it.xattrs()) {
        newdir->setAttribute(attrit.first, attrit.second);
      }

      try {
        gOFS->eosView->updateContainerStore(newdir.get());
      } catch (eos::MDException& e) {
        std::ostringstream msg;
        msg << "Failed to call gOFS->eosView->updateContainerStore(): " << e.getMessage().str();
        e.getMessage().str(msg.str());
        throw;
      }
      reply->add_retc(0);
    } catch (eos::MDException& e) {
      eos_static_err("msg=\"exception\" ec=%d emsg=\"%s\" path=\"%s\" fxid=%08llx\n",
                     e.getErrno(), e.getMessage().str().c_str(), it.path().c_str(), it.id());
      reply->add_retc(e.getErrno());
    }
  }

  return grpc::Status::OK;
}

#endif

EOSMGMNAMESPACE_END
