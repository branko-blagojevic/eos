//------------------------------------------------------------------------------
//! @file IProcCommand.hh
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2017 CERN/Switzerland                                  *
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
 * You should have received a copy of the AGNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

#pragma once
#include "mgm/Namespace.hh"
#include "common/Mapping.hh"
#include "common/Logging.hh"
#include "proto/ConsoleReply.pb.h"
#include "proto/ConsoleRequest.pb.h"
#include "XrdSfs/XrdSfsInterface.hh"
#include <future>
#include <sstream>

//! Forward declarations
class XrdOucErrInfo;

EOSMGMNAMESPACE_BEGIN

//------------------------------------------------------------------------------
//! Class IProcCommand - interface that needs to be implemented by all types
//! of commands executed by the MGM.
//------------------------------------------------------------------------------
class IProcCommand: public eos::common::LogId
{
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  IProcCommand():
    mExecRequest(false), mReqProto(), mDoAsync(false), mForceKill(false),
    mVid(), pVid(0), mCmd(""), mSubCmd(""), mArgs(""), mComment(""),
    mClosed(false), stdOut(), stdErr(), stdJson(), retc(0), mTmpResp()
  {
    mExecTime = time(NULL);
  }

  //----------------------------------------------------------------------------
  //! Constructor
  //!
  //! @param req client protobuf request
  //! @param vid client virtual identity
  //! @param async if true then use thread pool to execute the command
  //----------------------------------------------------------------------------
  IProcCommand(eos::console::RequestProto&& req,
               eos::common::Mapping::VirtualIdentity& vid, bool async):
    IProcCommand()
  {
    mReqProto = req;
    mVid = vid;
    mDoAsync = async;
  }

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  virtual ~IProcCommand()
  {
    mForceKill.store(true);

    if (ofstdoutStream.is_open()) {
      ofstdoutStream.close();
    }

    unlink(ofstdoutStreamFilename.c_str());

    if (ofstderrStream.is_open()) {
      ofstderrStream.close();
    }

    unlink(ofstderrStreamFilename.c_str());
  }

  //----------------------------------------------------------------------------
  //! Open a proc command e.g. call the appropriate user or admin commmand and
  //! store the output in a resultstream of in case of find in temporary output
  //! files.
  //!
  //! @param inpath path indicating user or admin command
  //! @param info CGI describing the proc command
  //! @param vid_in virtual identity of the user requesting a command
  //! @param error object to store errors
  //!
  //! @return SFS_OK in any case
  //----------------------------------------------------------------------------
  virtual int open(const char* path, const char* info,
                   eos::common::Mapping::VirtualIdentity& vid,
                   XrdOucErrInfo* error);

  //----------------------------------------------------------------------------
  //! Read a part of the result stream created during open
  //!
  //! @param boff offset where to start
  //! @param buff buffer to store stream
  //! @param blen len to return
  //!
  //! @return number of bytes read
  //----------------------------------------------------------------------------
  virtual size_t read(XrdSfsFileOffset offset, char* buff, XrdSfsXferSize blen);

  //----------------------------------------------------------------------------
  //! Get the size of the result stream
  //!
  //! @param buf stat structure to fill
  //!
  //! @return SFS_OK in any case
  //----------------------------------------------------------------------------
  virtual int stat(struct stat* buf)
  {
    off_t size = 0;

    if (readStdOutStream) {
      ifstdoutStream.seekg(0, ifstdoutStream.end);
      size += ifstdoutStream.tellg();
      ifstdoutStream.seekg(0, ifstdoutStream.beg);
      ifstderrStream.seekg(0, ifstderrStream.end);
      size += ifstderrStream.tellg();
      ifstderrStream.seekg(0, ifstderrStream.beg);
      iretcStream.seekg(0, iretcStream.end);
      size += iretcStream.tellg();
      iretcStream.seekg(0, iretcStream.beg);
    } else {
      size = mTmpResp.length();
    }

    memset(buf, 0, sizeof(struct stat));
    buf->st_size = size;
    return SFS_OK;
  }

  //----------------------------------------------------------------------------
  //! Close the proc stream and store the clients comment for the command in the
  //! comment log file
  //!
  //! @return 0 if comment has been successfully stored otherwise != 0
  //----------------------------------------------------------------------------
  virtual int close();

  //----------------------------------------------------------------------------
  //! Method implementing the specific behavior of the command executed
  //----------------------------------------------------------------------------
  virtual eos::console::ReplyProto ProcessRequest() = 0;

  //----------------------------------------------------------------------------
  //! Lauch command asynchronously, creating the corresponding promise and
  //! future
  //----------------------------------------------------------------------------
  virtual void LaunchJob() final;

  //----------------------------------------------------------------------------
  //! Check if we can safely delete the current object as there is no async
  //! thread executing the ProcessResponse method
  //!
  //! @return true if deletion if safe, otherwise false
  //----------------------------------------------------------------------------
  virtual bool KillJob() final;

protected:
  virtual bool OpenTemporaryOutputFiles();
  virtual bool CloseTemporaryOutputFiles();

  //----------------------------------------------------------------------------
  //! Get a file's full path using the fid information stored in the opaque
  //! data.
  //!
  //! @param path full path of the file
  //! @param opaque request opaque information
  //! @param err_msg error message to be displayed in case of exception
  //----------------------------------------------------------------------------
  void GetPathFromFid(XrdOucString& path, unsigned long long fid,
                      const std::string& err_msg);

  //----------------------------------------------------------------------------
  //! Get a containers's full path using the cid information stored in the opaque
  //! data.
  //!
  //! @param path full path of the container
  //! @param opaque request opaque information
  //! @param err_msg error message to be displayed in case of exception
  //----------------------------------------------------------------------------
  void GetPathFromCid(XrdOucString& path, unsigned long long cid,
                      const std::string& err_msg);

  //----------------------------------------------------------------------------
  //! Format console reply as json output
  //!
  //! @param reply console command reply object
  //! @param oss outputs string stream
  //----------------------------------------------------------------------------
  static void ConvertToJsonFormat(eos::console::ReplyProto& reply,
                                  std::ostringstream& oss);

  //----------------------------------------------------------------------------
  //! Check if operation forbidden
  //!
  //! @param inpath path of the request
  //!
  //! @return SFS_OK if operation forbidde, SFS_ERRRO if operation allowed
  //----------------------------------------------------------------------------
  int IsOperationForbidden(const char* inpath);

  bool mExecRequest; ///< Indicate if request is launched asynchronously
  eos::console::RequestProto mReqProto; ///< Client request protobuf object
  std::future<eos::console::ReplyProto> mFuture; ///< Response future
  bool mDoAsync; ///< If true use thread pool to do the work
  std::atomic<bool> mForceKill; ///< Flag to notify worker thread
  eos::common::Mapping::VirtualIdentity mVid; ///< Copy of original vid
  eos::common::Mapping::VirtualIdentity* pVid; ///< Pointer to virtual identity
  XrdOucString mCmd; ///< proc command name
  XrdOucString mSubCmd; ///< proc sub command name
  XrdOucString mArgs; ///< full args from opaque input
  XrdOucString mComment; ///< comment issued by the user for the proc command
  bool mClosed; ///< indicates the proc command has been closed already
  XrdOucString stdOut; ///< stdOut returned by proc command
  XrdOucString stdErr; ///< stdErr returned by proc command
  XrdOucString stdJson; ///< JSON output returned by proc command
  int retc; ///< return code from the proc command
  std::string mTmpResp; ///< String used for streaming the response
  time_t mExecTime; ///< execution time measured for the proc command
  std::ofstream ofstdoutStream;
  std::ofstream ofstderrStream;
  std::string ofstdoutStreamFilename;
  std::string ofstderrStreamFilename;
  std::ifstream ifstdoutStream;
  std::ifstream ifstderrStream;
  std::istringstream iretcStream;
  bool readStdOutStream {false};
  bool readStdErrStream {false};
  bool readRetcStream {false};
  static std::atomic_uint_least64_t uuid;
};

EOSMGMNAMESPACE_END
