//------------------------------------------------------------------------------
// File: QoSCmd.cc
// Author: Mihai Patrascoiu - CERN
//------------------------------------------------------------------------------

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

#include "common/LayoutId.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/Scheduler.hh"
#include "QoSCmd.hh"

EOSMGMNAMESPACE_BEGIN

// Helper function forward declaration
static int CheckIsFile(const char*, eos::common::VirtualIdentity&,
                       std::string&);

//------------------------------------------------------------------------------
// Method implementing the specific behaviour of the command executed
//------------------------------------------------------------------------------
eos::console::ReplyProto
QoSCmd::ProcessRequest() noexcept
{
  eos::console::ReplyProto reply;
  eos::console::QoSProto qos = mReqProto.qos();
  const auto& subcmd = qos.subcmd_case();
  bool jsonOutput =
    (mReqProto.format() == eos::console::RequestProto::JSON);

  if (subcmd == eos::console::QoSProto::kGet) {
    GetSubcmd(qos.get(), reply, jsonOutput);
  } else if (subcmd == eos::console::QoSProto::kSet) {
    SetSubcmd(qos.set(), reply, jsonOutput);
  } else {
    reply.set_retc(EINVAL);
    reply.set_std_err("error: command not supported");
  }

  return reply;
}

//------------------------------------------------------------------------------
// Execute get subcommand
//------------------------------------------------------------------------------
void QoSCmd::GetSubcmd(const eos::console::QoSProto_GetProto& get,
                       eos::console::ReplyProto& reply,
                       bool jsonOutput)
{
  std::ostringstream out;
  std::ostringstream err;
  XrdOucString spath;
  int retc = 0;

  spath = PathFromIdentifierProto(get.identifier());

  if (!spath.length()) {
    reply.set_std_err(stdErr.c_str());
    reply.set_retc(ENOENT);
    return;
  }

  XrdOucErrInfo errInfo;
  std::string errmsg;

  // Check path points to a valid file
  if ((retc = CheckIsFile(spath.c_str(), mVid, errmsg))) {
    reply.set_std_err(errmsg);
    reply.set_retc(retc);
    return;
  }

  // Check for access permission
  if (gOFS->_access(spath.c_str(), R_OK, errInfo, mVid, 0)) {
    err << "error: " << errInfo.getErrText();
    reply.set_std_err(err.str().c_str());
    reply.set_retc(errInfo.getErrInfo());
    return;
  }

  // Keep a key set to avoid processing duplicates
  std::set<std::string> qosKeys;
  eos::IFileMD::QoSAttrMap qosMap;

  for (const auto& key: get.key()) {
    if (key == "all") {
      qosKeys.clear();
      break;
    }

    qosKeys.insert(key);
  }

  // Process specified keys
  for (const auto& key: qosKeys) {
    if (key == "cdmi") {
      eos::IFileMD::QoSAttrMap cdmiMap;

      if (gOFS->_qos_ls(spath.c_str(), errInfo, mVid, cdmiMap, true)) {
        err << "error: " << errInfo.getErrText() << std::endl;
        retc = errInfo.getErrInfo();
        continue;
      }

      qosMap.insert(cdmiMap.begin(), cdmiMap.end());
    } else {
      XrdOucString value;

      if (gOFS->_qos_get(spath.c_str(), errInfo, mVid, key.c_str(), value)) {
        err << "error: " << errInfo.getErrText() << std::endl;
        retc = errInfo.getErrInfo();
        continue;
      }

      qosMap[key] = value.c_str();
    }
  }

  // No keys specified -- extract all
  if (qosKeys.empty()) {
    if (gOFS->_qos_ls(spath.c_str(), errInfo, mVid, qosMap)) {
      err << "error: " << errInfo.getErrText() << std::endl;
      retc = errInfo.getErrInfo();
    }
  }

  // Format QoS properties map to desired output
  out << (jsonOutput ? MapToJSONOutput(qosMap)
                     : MapToDefaultOutput(qosMap));

  reply.set_retc(retc);
  reply.set_std_out(out.str());
  reply.set_std_err(err.str());
}

//------------------------------------------------------------------------------
// Execute set subcommand
//------------------------------------------------------------------------------
void QoSCmd::SetSubcmd(const eos::console::QoSProto_SetProto& set,
                       eos::console::ReplyProto& reply,
                       bool jsonOutput)
{
  using eos::common::LayoutId;
  std::ostringstream out;
  std::ostringstream err;
  XrdOucString spath;
  int retc = 0;

  spath = PathFromIdentifierProto(set.identifier());

  if (!spath.length()) {
    reply.set_std_err(stdErr.c_str());
    reply.set_retc(ENOENT);
    return;
  }

  XrdOucErrInfo errInfo;
  std::string errmsg;

  // Check path points to a valid file
  if ((retc = CheckIsFile(spath.c_str(), mVid, errmsg))) {
    reply.set_std_err(errmsg);
    reply.set_retc(retc);
    return;
  }

  int layout = -1;
  int checksum = -1;
  int nstripes = -1;
  std::string policy = "";

  for (const auto& pair: set.pair()) {
    const auto& key = pair.key();
    const auto& value = pair.value();

    if (!IsValidPair(key, value)) {
      err << "warning: invalid QoS property "
          << key << "=" << value << std::endl;
      continue;
    }

    // Extract new layout components
    if (key == "layout") {
      layout = LayoutId::GetLayoutFromString(value);
    } else if (key == "replica") {
      nstripes = std::stoi(value);
    } else if (key == "checksum") {
      checksum = LayoutId::GetChecksumFromString(value);
    } else if (key == "placement") {
      policy = value;
    }
  }

  if ((layout == -1) && (checksum == -1) &&
      (nstripes == -1) && (policy.empty())) {
    reply.set_std_err("error: no valid QoS properties found");
    reply.set_retc(EINVAL);
    return;
  }

  std::string conversion_id = "";

  if (gOFS->_qos_set(spath.c_str(), errInfo, mVid, conversion_id,
                     layout, nstripes, checksum, policy)) {
    err << "error: " << errInfo.getErrText() << std::endl;
    retc = errInfo.getErrInfo();
  }

  if (!retc && !jsonOutput) {
    out << "scheduled QoS conversion job: " << conversion_id;
  } else if (jsonOutput) {
    Json::Value jsonOut;
    jsonOut["retc"] = (Json::Value::UInt64) retc;
    jsonOut["conversionid"] = (retc) ? "null" : conversion_id;
    out << jsonOut;
  }

  reply.set_retc(retc);
  reply.set_std_out(out.str());
  reply.set_std_err(err.str());
}

//----------------------------------------------------------------------------
// Check the given <key>=<value> is a valid QoS property.
//----------------------------------------------------------------------------
bool QoSCmd::IsValidPair(const std::string& key, const std::string& value)
{
  if (key == "placement") {
    return Scheduler::PlctPolicyFromString(value) != -1;
  } else if (key == "layout") {
    return eos::common::LayoutId::GetLayoutFromString(value) != -1;
  } else if (key == "checksum") {
    return eos::common::LayoutId::GetChecksumFromString(value) != -1;
  } else if (key == "replica") {
    int number = std::stoi(value);
    return (number >= 1 && number <= 16);
  }

  return false;
}

//------------------------------------------------------------------------------
// Translate the identifier proto into a namespace path
//------------------------------------------------------------------------------
XrdOucString QoSCmd::PathFromIdentifierProto(
  const eos::console::QoSProto_IdentifierProto& identifier)
{
  using eos::console::QoSProto;
  const auto& type = identifier.Identifier_case();
  XrdOucString path = "";

  if (type == QoSProto::IdentifierProto::kPath) {
    path = identifier.path().c_str();
  } else if (type == QoSProto::IdentifierProto::kFileId) {
    GetPathFromFid(path, identifier.fileid(), "error: ");
  } else {
    stdErr = "error: received empty string path";
  }

  return path;
}

//------------------------------------------------------------------------------
// Process a QoS properties map into a default printable output
//------------------------------------------------------------------------------
std::string QoSCmd::MapToDefaultOutput(const eos::IFileMD::QoSAttrMap& map)
{
  std::ostringstream out;

  for (const auto& it: map) {
    out << it.first << "=" << it.second << std::endl;
  }

  return out.str();
}

//------------------------------------------------------------------------------
// Process a QoS properties map into a default printable output
//------------------------------------------------------------------------------
std::string QoSCmd::MapToJSONOutput(const eos::IFileMD::QoSAttrMap& map)
{
  Json::Value jsonOut, jsonCDMI;

  for (const auto& it: map) {
    XrdOucString key = it.first.c_str();

    if (key.beginswith("cdmi_")) {
      jsonCDMI[it.first] = it.second;
    } else {
      jsonOut[it.first] = it.second;
    }
  }

  if (!jsonCDMI.empty()) {
    jsonOut["metadata"] = jsonCDMI;
  }

  return static_cast<std::ostringstream&>(
    std::ostringstream() << jsonOut).str();
}

//------------------------------------------------------------------------------
//! Check that the given path points to a valid file.
//!
//! @param path the path to check
//! @param vid virtual identity of the client
//! @param err_msg string to place error message
//!
//! @return 0 if path points to file, error code otherwise
//------------------------------------------------------------------------------
static int CheckIsFile(const char* path,
                       eos::common::VirtualIdentity& vid,
                       std::string& err_msg)
{
  XrdSfsFileExistence fileExists;
  XrdOucErrInfo errInfo;

  // Check for path existence
  if (gOFS->_exists(path, fileExists, errInfo, vid)) {
    err_msg = "error: unable to check for path existence";
    return errInfo.getErrInfo();
  }

  if (fileExists == XrdSfsFileExistNo) {
    err_msg = "error: path does not point to a valid entry";
    return EINVAL;
  } else if (fileExists != XrdSfsFileExistIsFile) {
    err_msg = "error: path does not point to a file";
    return EINVAL;
  }

  return 0;
}

EOSMGMNAMESPACE_END
