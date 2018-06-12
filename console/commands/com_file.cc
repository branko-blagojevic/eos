// ----------------------------------------------------------------------
// File: com_file.cc
// Author: Andreas-Joachim Peters - CERN
// ----------------------------------------------------------------------

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

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include "console/ConsoleMain.hh"
#include "common/StringTokenizer.hh"
#include "common/FileId.hh"
#include "common/SymKeys.hh"
#include "common/LayoutId.hh"
#include "fst/Fmd.hh"
#include "XrdCl/XrdClFileSystem.hh"

#ifdef __APPLE__
#define ECOMM 70
#endif

//------------------------------------------------------------------------------
//! Convert an FST env representation to an Fmd struct
//!
//! @param env env representation
//! @param fmd reference to Fmd struct
//!
//! @return true if successful otherwise false
//------------------------------------------------------------------------------
bool
EnvFstToFmd(XrdOucEnv& env, struct eos::fst::Fmd& fmd)
{
  // Check that all tags are present
  if (!env.Get("id") ||
      !env.Get("cid") ||
      !env.Get("ctime") ||
      !env.Get("ctime_ns") ||
      !env.Get("mtime") ||
      !env.Get("mtime_ns") ||
      !env.Get("size") ||
      !env.Get("lid") ||
      !env.Get("uid") ||
      !env.Get("gid")) {
    return false;
  }

  fmd.set_fid(strtoull(env.Get("id"), 0, 10));
  fmd.set_cid(strtoull(env.Get("cid"), 0, 10));
  fmd.set_ctime(strtoul(env.Get("ctime"), 0, 10));
  fmd.set_ctime_ns(strtoul(env.Get("ctime_ns"), 0, 10));
  fmd.set_mtime(strtoul(env.Get("mtime"), 0, 10));
  fmd.set_mtime_ns(strtoul(env.Get("mtime_ns"), 0, 10));
  fmd.set_size(strtoull(env.Get("size"), 0, 10));
  fmd.set_lid(strtoul(env.Get("lid"), 0, 10));
  fmd.set_uid((uid_t) strtoul(env.Get("uid"), 0, 10));
  fmd.set_gid((gid_t) strtoul(env.Get("gid"), 0, 10));

  if (env.Get("checksum")) {
    fmd.set_checksum(env.Get("checksum"));

    if (fmd.checksum() == "none") {
      fmd.set_checksum("");
    }
  } else {
    fmd.set_checksum("");
  }

  return true;
}


//------------------------------------------------------------------------------
//! Return a remote file attribute
//!
//! @param manager host:port of the server to contact
//! @param key extended attribute key to get
//! @param path file path to read attributes from
//! @param attribute reference where to store the attribute value
//------------------------------------------------------------------------------
int
GetRemoteAttribute(const char* manager, const char* key,
                   const char* path, XrdOucString& attribute)
{
  if ((!key) || (!path)) {
    return EINVAL;
  }

  int rc = 0;
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  XrdCl::XRootDStatus status;
  XrdOucString fmdquery = "/?fst.pcmd=getxattr&fst.getxattr.key=";
  fmdquery += key;
  fmdquery += "&fst.getxattr.path=";
  fmdquery += path;
  XrdOucString address = "root://";
  address += manager;
  address += "//dummy";
  XrdCl::URL url(address.c_str());

  if (!url.IsValid()) {
    eos_static_err("error=URL is not valid: %s", address.c_str());
    return EINVAL;
  }

  std::unique_ptr<XrdCl::FileSystem> fs(new XrdCl::FileSystem(url));

  if (!fs) {
    eos_static_err("error=failed to get new FS object");
    return EINVAL;
  }

  arg.FromString(fmdquery.c_str());
  status = fs->Query(XrdCl::QueryCode::OpaqueFile, arg, response);

  if (status.IsOK()) {
    rc = 0;
    eos_static_debug("got attribute meta data from server %s for key=%s path=%s"
                     " attribute=%s", manager, key, path, response->ToString());
  } else {
    rc = ECOMM;
    eos_static_err("Unable to retrieve meta data from server %s for key=%s path=%s",
                   manager, key, path);
  }

  if (rc) {
    delete response;
    return EIO;
  }

  if (!strncmp(response->ToString(), "ERROR", 5)) {
    // remote side couldn't get the record
    eos_static_info("Unable to retrieve meta data on remote server %s for key=%s "
                    "path=%s", manager, key, path);
    delete response;
    return ENODATA;
  }

  attribute = response->GetBuffer();
  delete response;
  return 0;
}

//------------------------------------------------------------------------------
//! Return Fmd from a remote filesystem
//!
//! @param manager host:port of the server to contact
//! @param shexfid hex string of the file id
//! @param sfsid string of filesystem id
//! @param fmd reference to the Fmd struct to store Fmd
//------------------------------------------------------------------------------
int
GetRemoteFmdFromLocalDb(const char* manager, const char* shexfid,
                        const char* sfsid, struct eos::fst::Fmd& fmd)
{
  if ((!manager) || (!shexfid) || (!sfsid)) {
    return EINVAL;
  }

  int rc = 0;
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  XrdCl::XRootDStatus status;
  XrdOucString fmdquery = "/?fst.pcmd=getfmd&fst.getfmd.fid=";
  fmdquery += shexfid;
  fmdquery += "&fst.getfmd.fsid=";
  fmdquery += sfsid;
  XrdOucString address = "root://";
  address += manager;
  address += "//dummy";
  XrdCl::URL url(address.c_str());

  if (!url.IsValid()) {
    eos_static_err("error=URL is not valid: %s", address.c_str());
    return EINVAL;
  }

  std::unique_ptr<XrdCl::FileSystem> fs(new XrdCl::FileSystem(url));

  if (!fs) {
    eos_static_err("error=failed to get new FS object");
    return EINVAL;
  }

  arg.FromString(fmdquery.c_str());
  status = fs->Query(XrdCl::QueryCode::OpaqueFile, arg, response);

  if (status.IsOK()) {
    rc = 0;
    eos_static_debug("got replica file meta data from server %s for fid=%s fsid=%s",
                     manager, shexfid, sfsid);
  } else {
    rc = ECOMM;
    eos_static_err("Unable to retrieve meta data from server %s for fid=%s fsid=%s",
                   manager, shexfid, sfsid);
  }

  if (rc) {
    delete response;
    return EIO;
  }

  if (!strncmp(response->ToString(), "ERROR", 5)) {
    // remote side couldn't get the record
    eos_static_info("Unable to retrieve meta data on remote server %s for fid=%s fsid=%s",
                    manager, shexfid, sfsid);
    delete response;
    return ENODATA;
  }

  // get the remote file meta data into an env hash
  XrdOucEnv fmdenv(response->GetBuffer());

  if (!EnvFstToFmd(fmdenv, fmd)) {
    int envlen;
    eos_static_err("Failed to unparse file meta data %s", fmdenv.Env(envlen));
    delete response;
    return EIO;
  }

  // very simple check
  if (fmd.fid() != eos::common::FileId::Hex2Fid(shexfid)) {
    eos_static_err("Uups! Received wrong meta data from remote server - fid "
                   "is %lu instead of %lu !", fmd.fid(),
                   eos::common::FileId::Hex2Fid(shexfid));
    delete response;
    return EIO;
  }

  delete response;
  return 0;
}


/* Get file information */
int
com_fileinfo(char* arg1)
{
  XrdOucString savearg = arg1;
  // split subcommands
  eos::common::StringTokenizer subtokenizer(arg1);
  subtokenizer.GetLine();
  XrdOucString path = subtokenizer.GetToken();
  XrdOucString option = "";

  do {
    XrdOucString newoption = subtokenizer.GetToken();

    if (!newoption.length()) {
      break;
    } else {
      if (newoption == "s") {
        option += "silent";
      } else {
        option += newoption;
      }
    }
  } while (1);

  XrdOucString in = "mgm.cmd=fileinfo&";

  if (wants_help(savearg.c_str())) {
    goto com_fileinfo_usage;
  }

  if (!path.length()) {
    goto com_fileinfo_usage;
  } else {
    if (path.beginswith("-")) {
      goto com_fileinfo_usage;
    }

    if ((!path.beginswith("fid:")) && (!path.beginswith("fxid:")) &&
        (!path.beginswith("pid:")) && (!path.beginswith("pxid:")) &&
        (!path.beginswith("inode:"))) {
      path = abspath(path.c_str());
    }

    in += "mgm.path=";
    in += path;

    if (option.length()) {
      in += "&mgm.file.info.option=";
      in += option;
    }

    if ((option.find("silent") == STR_NPOS)) {
      global_retc = output_result(client_command(in));
    }

    return (0);
  }

com_fileinfo_usage:
  fprintf(stdout,
          "usage: fileinfo <path> [--path] [--fxid] [--fid] [--size] [--checksum] [--fullpath] [-m] [--silent] [--env] :  print file information for <path>\n");
  fprintf(stdout,
          "       fileinfo fxid:<fid-hex>                                           :  print file information for fid <fid-hex>\n");
  fprintf(stdout,
          "       fileinfo fid:<fid-dec>                                            :  print file information for fid <fid-dec>\n");
  fprintf(stdout,
          "       fileinfo inode:<fid-dec>                                          :  print file information for inode (decimal)>\n");
  fprintf(stdout,
          "                                                                 --path  :  selects to add the path information to the output\n");
  fprintf(stdout,
          "                                                                 --fxid  :  selects to add the hex file id information to the output\n");
  fprintf(stdout,
          "                                                                 --fid   :  selects to add the base10 file id information to the output\n");
  fprintf(stdout,
          "                                                                 --size  :  selects to add the size information to the output\n");
  fprintf(stdout,
          "                                                              --checksum :  selects to add the checksum information to the output\n");
  fprintf(stdout,
          "                                                              --fullpath :  selects to add the full path information to each replica\n");
  fprintf(stdout,
          "                                                              --proxy    :  selects to add the proxy information if any\n");
  fprintf(stdout,
          "                                                                  -m     :  print single line in monitoring format\n");
  fprintf(stdout,
          "                                                                  --env  :  print in OucEnv format\n");
  fprintf(stdout,
          "                                                                  -s     :  silent - used to run as internal command\n");
  return (0);
}

int
com_file(char* arg1)
{
  XrdOucString savearg = arg1;
  XrdOucString arg = arg1;
  eos::common::StringTokenizer subtokenizer(arg1);
  XrdOucString option = "";
  XrdOucString path = "";
  subtokenizer.GetLine();
  XrdOucString cmd = subtokenizer.GetToken();
  XrdOucString tmpArg;

  do {
    tmpArg = subtokenizer.GetToken();

    if (tmpArg.beginswith("-")) {
      while (tmpArg.replace("-", "")) {
      }

      option += tmpArg;
    } else {
      path = tmpArg;
      break;
    }
  } while (1);

  XrdOucString fsid1 = subtokenizer.GetToken();
  XrdOucString fsid2 = subtokenizer.GetToken();
  XrdOucString fsid3 = subtokenizer.GetToken();

  if ((!path.beginswith("fid:")) && (!path.beginswith("fxid:"))) {
    path = abspath(path.c_str());
  }

  XrdOucString in = "mgm.cmd=file";

  if (wants_help(savearg.c_str())) {
    goto com_file_usage;
  }

  if ((cmd != "drop") && (cmd != "move") && (cmd != "touch") &&
      (cmd != "replicate") && (cmd != "check") && (cmd != "adjustreplica") &&
      (cmd != "info") && (cmd != "layout") && (cmd != "verify") &&
      (cmd != "rename") && (cmd != "copy") && (cmd != "convert") &&
      (cmd != "share") && (cmd != "purge") && (cmd != "version") &&
      (cmd != "versions") && (cmd != "symlink") && (cmd != "tag") &&
      (cmd != "workflow")) {
    goto com_file_usage;
  }

  // convenience function
  if (cmd == "info") {
    arg.erase(0, arg.find(" ") + 1);
    return com_fileinfo((char*) arg.c_str());
  }

  if (cmd == "rename") {
    if (!path.length() || !fsid1.length()) {
      goto com_file_usage;
    }

    fsid1 = abspath(fsid1.c_str());
    in += "&mgm.subcmd=rename";
    in += Path2FileDenominator(path) ? "&mgm.file.id=" : "&mgm.path=";
    in += path;
    in += "&mgm.file.source=";
    in += path;
    in += "&mgm.file.target=";
    in += fsid1.c_str();
  }

  if (cmd == "symlink") {
    if (!path.length() || !fsid1.length()) {
      goto com_file_usage;
    }

    in += "&mgm.path=";
    in += path;
    in += "&mgm.subcmd=symlink";
    in += "&mgm.file.source=";
    in += path.c_str();
    in += "&mgm.file.target=";
    in += fsid1.c_str();
  }

  if (cmd == "share") {
    if (!path.length()) {
      goto com_file_usage;
    }

    path = abspath(path.c_str());
    in += "&mgm.path=";
    in += path;
    in += "&mgm.subcmd=share";
    in += "&mgm.file.expires=";
    unsigned long long expires = (time(NULL) + 28 * 86400);

    if (fsid1.length()) {
      expires = time(NULL) + eos::common::StringConversion::GetSizeFromString(fsid1);
    }

    char sexpires[1024];
    snprintf(sexpires, sizeof(sexpires) - 1, "%llu", expires);
    in += sexpires;
  }

  if (cmd == "touch") {
    if (!path.length()) {
      goto com_file_usage;
    }

    in += Path2FileDenominator(path) ? "&mgm.file.id=" : "&mgm.path=";
    in += path;
    in += "&mgm.subcmd=touch";
  }

  if (cmd == "drop") {
    if (!path.length() || !fsid1.length()) {
      goto com_file_usage;
    }

    in += "&mgm.subcmd=drop";
    in += Path2FileDenominator(path) ? "&mgm.file.id=" : "&mgm.path=";
    in += path;
    in += "&mgm.file.fsid=";
    in += fsid1;

    if (fsid2 == "-f") {
      in += "&mgm.file.force=1";
    } else {
      if (fsid2.length()) {
        goto com_file_usage;
      }
    }
  }

  if (cmd == "move") {
    if (!path.length() || !fsid1.length() || !fsid2.length()) {
      goto com_file_usage;
    }

    in += "&mgm.subcmd=move";
    in += Path2FileDenominator(path) ? "&mgm.file.id=" : "&mgm.path=";
    in += path;
    in += "&mgm.file.sourcefsid=";
    in += fsid1;
    in += "&mgm.file.targetfsid=";
    in += fsid2;
  }

  if (cmd == "copy") {
    XrdOucString dest_path = fsid1;

    if (!path.length() || !dest_path.length()) {
      goto com_file_usage;
    }

    in += "&mgm.subcmd=copy";
    in += Path2FileDenominator(path) ? "&mgm.file.id=" : "&mgm.path=";
    in += path;

    if (option.length()) {
      XrdOucString checkoption = option;
      checkoption.replace("f", "");
      checkoption.replace("s", "");
      checkoption.replace("c", "");

      if (checkoption.length()) {
        goto com_file_usage;
      }

      in += "&mgm.file.option=";
      in += option;
    }

    dest_path = abspath(dest_path.c_str());
    in += "&mgm.file.target=";
    in += dest_path;
  }

  if (cmd == "convert") {
    XrdOucString layout = fsid1;
    XrdOucString space = fsid2;
    XrdOucString plctplcty = fsid3;

    if (!path.length()) {
      goto com_file_usage;
    }

    in += "&mgm.subcmd=convert";
    in += Path2FileDenominator(path) ? "&mgm.file.id=" : "&mgm.path=";
    in += path;

    if (layout.length()) {
      in += "&mgm.convert.layout=";
      in += layout;
    }

    if (space.length()) {
      in += "&mgm.convert.space=";
      in += space;
    }

    if (plctplcty.length()) {
      in += "&mgm.convert.placementpolicy=";
      in += plctplcty;
    }

    if (option == "sync") {
      fprintf(stderr, "error: --sync is currently not supported\n");
      goto com_file_usage;
    }

    if (option == "rewrite") {
      in += "&mgm.option=rewrite";
    } else {
      if (option.length()) {
        goto com_file_usage;
      }
    }
  }

  if (cmd == "replicate") {
    if (!path.length() || !fsid1.length() || !fsid2.length()) {
      goto com_file_usage;
    }

    in += "&mgm.subcmd=replicate";
    in += Path2FileDenominator(path) ? "&mgm.file.id=" : "&mgm.path=";
    in += path;
    in += "&mgm.file.sourcefsid=";
    in += fsid1;
    in += "&mgm.file.targetfsid=";
    in += fsid2;
  }

  if ((cmd == "purge") || (cmd == "version")) {
    if (!path.length()) {
      goto com_file_usage;
    }

    in += "&mgm.subcmd=";
    in += cmd;
    in += "&mgm.path=";
    in += path;
    in += "&mgm.purge.version=";

    if (fsid1.length()) {
      in += fsid1;
    } else {
      in += "-1";
    }
  }

  if (cmd == "versions") {
    if (!path.length()) {
      goto com_file_usage;
    }

    in += "&mgm.subcmd=";
    in += cmd;
    in += Path2FileDenominator(path) ? "&mgm.file.id=" : "&mgm.path=";
    in += path;
    in += "&mgm.grab.version=";

    if (fsid1.length()) {
      in += fsid1;
    } else {
      in += "-1";
    }
  }

  if (cmd == "adjustreplica") {
    if (!path.length()) {
      goto com_file_usage;
    }

    in += "&mgm.subcmd=adjustreplica";
    in += Path2FileDenominator(path) ? "&mgm.file.id=" : "&mgm.path=";
    in += path;

    if (fsid1.length()) {
      in += "&mgm.file.desiredspace=";
      in += fsid1;

      if (fsid2.length()) {
        in += "&mgm.file.desiredsubgroup=";
        in += fsid2;
      }
    }
  }

  if (cmd == "layout") {
    if (!path.length()) {
      goto com_file_usage;
    }

    in += "&mgm.subcmd=layout";
    in += Path2FileDenominator(path) ? "&mgm.file.id=" : "&mgm.path=";
    in += path;

    if ((fsid1 != "-stripes") && (fsid1 != "-checksum")) {
      goto com_file_usage;
    }

    if (!fsid2.length()) {
      goto com_file_usage;
    }

    if (fsid1 == "-stripes") {
      in += "&mgm.file.layout.stripes=";
    }

    if (fsid1 == "-checksum") {
      in += "&mgm.file.layout.ckecksum=";
    }

    in += fsid2;
  }

  if (cmd == "workflow") {
    if (!path.length()) {
      goto com_file_usage;
    }

    if (!fsid1.length()) {
      goto com_file_usage;
    }

    if (!fsid2.length()) {
      goto com_file_usage;
    }

    in += "&mgm.subcmd=workflow";
    in += "&mgm.path=";
    in += path;
    in += "&mgm.workflow=";
    in += fsid1;
    in += "&mgm.event=";
    in += fsid2;
  }

  if (cmd == "tag") {
    if (!path.length()) {
      goto com_file_usage;
    }

    in += "&mgm.subcmd=tag";
    in += "&mgm.path=";
    in += path;

    if ((!fsid1.beginswith("+")) &&
        (!fsid1.beginswith("-")) &&
        (!fsid1.beginswith("~"))) {
      goto com_file_usage;
    }

    in += "&mgm.file.tag.fsid=";
    in += fsid1;
  }

  if (cmd == "verify") {
    if (!path.length()) {
      goto com_file_usage;
    }

    const char* opt;
    std::vector<std::string> options;
    in += "&mgm.subcmd=verify";
    in += "&mgm.path=";
    in += path;

    // TODO: all this is silly and should be properly re-written
    if (fsid1.length()) {
      if ((fsid1 != "-checksum") && (fsid1 != "-commitchecksum") &&
          (fsid1 != "-commitsize") && (fsid1 != "-commitfmd") && (fsid1 != "-rate")) {
        if (fsid1.beginswith("-")) {
          goto com_file_usage;
        }

        in += "&mgm.file.verify.filterid=";
        in += fsid1;

        if (fsid2.length()) {
          options.push_back(fsid2.c_str());

          if (fsid3.length()) {
            options.push_back(fsid3.c_str());
          }

          while ((opt = subtokenizer.GetToken())) {
            options.push_back(opt);
            opt = 0;
          }
        }
      } else {
        options.push_back(fsid1.c_str());

        if (fsid2.length()) {
          options.push_back(fsid2.c_str());
        }

        if (fsid3.length()) {
          options.push_back(fsid3.c_str());
        }

        while ((opt = subtokenizer.GetToken())) {
          options.push_back(opt);
          opt = 0;
        }
      }
    }

    for (auto& elem : options) {
      if (elem.length()) {
        if (elem == "-checksum") {
          in += "&mgm.file.compute.checksum=1";
        } else if (elem == "-commitchecksum") {
          in += "&mgm.file.commit.checksum=1";
        } else if (elem == "-commitsize") {
          in += "&mgm.file.commit.size=1";
        } else if (elem == "-commitfmd") {
          in += "&mgm.file.commit.fmd=1";
        } else if (elem == "-rate") {
          in += "&mgm.file.verify.rate=";
        } else {
          goto com_file_usage;
        }
      }
    }
  }

  if (cmd == "check") {
    if (!path.length()) {
      goto com_file_usage;
    }

    in += "&mgm.subcmd=getmdlocation";
    in += "&mgm.format=fuse";
    in += "&mgm.path=";
    in += path;
    XrdOucString option = fsid1;
    XrdOucEnv* result = client_command(in);

    if (!result) {
      fprintf(stderr, "error: getmdlocation query failed\n");
      global_retc = EINVAL;
      return (0);
    }

    int envlen = 0;
    XrdOucEnv* newresult = new XrdOucEnv(result->Env(envlen));
    delete result;
    XrdOucString checksumattribute = "NOTREQUIRED";
    bool consistencyerror = false;
    bool down = false;

    if (envlen) {
      XrdOucString checksumtype = newresult->Get("mgm.checksumtype");
      XrdOucString checksum = newresult->Get("mgm.checksum");
      XrdOucString size = newresult->Get("mgm.size");

      if ((option.find("%silent") == STR_NPOS) && (!silent)) {
        fprintf(stdout, "path=\"%s\" fid=\"%4s\" size=\"%s\" nrep=\"%s\" "
                "checksumtype=\"%s\" checksum=\"%s\"\n",
                path.c_str(), newresult->Get("mgm.fid0"),
                size.c_str(), newresult->Get("mgm.nrep"),
                checksumtype.c_str(), newresult->Get("mgm.checksum"));
      }

      int i = 0;
      XrdOucString inconsistencylable = "";
      int nreplicaonline = 0;

      for (i = 0; i < eos::common::LayoutId::kSixteenStripe; i++) {
        XrdOucString repurl = "mgm.replica.url";
        repurl += i;
        XrdOucString repfid = "mgm.fid";
        repfid += i;
        XrdOucString repfsid = "mgm.fsid";
        repfsid += i;
        XrdOucString repbootstat = "mgm.fsbootstat";
        repbootstat += i;
        XrdOucString repfstpath = "mgm.fstpath";
        repfstpath += i;

        if (newresult->Get(repurl.c_str())) {
          // Query
          XrdCl::StatInfo* stat_info = 0;
          XrdCl::XRootDStatus status;
          XrdOucString address = "root://";
          address += newresult->Get(repurl.c_str());
          address += "//dummy";
          XrdCl::URL url(address.c_str());

          if (!url.IsValid()) {
            fprintf(stderr, "error=URL is not valid: %s", address.c_str());
            global_retc = EINVAL;
            return (0);
          }

          // Get XrdCl::FileSystem object
          std::unique_ptr<XrdCl::FileSystem> fs {new XrdCl::FileSystem(url)};

          if (!fs) {
            fprintf(stderr, "error=failed to get new FS object");
            global_retc = ECOMM;
            return (0);
          }

          XrdOucString bs = newresult->Get(repbootstat.c_str());

          if (bs != "booted") {
            down = true;
          } else {
            down = false;
          }

          struct eos::fst::Fmd fmd;

          int retc = 0;

          int oldsilent = silent;

          if ((option.find("%silent")) != STR_NPOS) {
            silent = true;
          }

          if (down && ((option.find("%force")) == STR_NPOS)) {
            consistencyerror = true;
            inconsistencylable = "DOWN";

            if (!silent) {
              fprintf(stderr,
                      "error: unable to retrieve file meta data from %s [ status=%s ]\n",
                      newresult->Get(repurl.c_str()), bs.c_str());
            }
          } else {
            // fprintf( stderr,"%s %s %s\n",newresult->Get(repurl.c_str()),
            //          newresult->Get(repfid.c_str()),newresult->Get(repfsid.c_str()) );
            if ((option.find("%checksumattr") != STR_NPOS)) {
              checksumattribute = "";

              if ((retc = GetRemoteAttribute(newresult->Get(repurl.c_str()),
                                             "user.eos.checksum",
                                             newresult->Get(repfstpath.c_str()),
                                             checksumattribute))) {
                if (!silent) {
                  fprintf(stderr, "error: unable to retrieve extended attribute from %s [%d]\n",
                          newresult->Get(repurl.c_str()), retc);
                }
              }
            }

            //..................................................................
            // Do a remote stat using XrdCl::FileSystem
            //..................................................................
            uint64_t rsize;
            XrdOucString statpath = newresult->Get(repfstpath.c_str());

            if (!statpath.beginswith("/")) {
              // base 64 encode this path
              XrdOucString statpath64;
              eos::common::SymKey::Base64(statpath, statpath64);
              statpath = "/#/";
              statpath += statpath64;
            }

            status = fs->Stat(statpath.c_str(), stat_info);

            if (!status.IsOK()) {
              consistencyerror = true;
              inconsistencylable = "STATFAILED";
              rsize = -1;
            } else {
              rsize = stat_info->GetSize();
            }

            // Free memory
            delete stat_info;

            if ((retc = GetRemoteFmdFromLocalDb(newresult->Get(repurl.c_str()),
                                                newresult->Get(repfid.c_str()),
                                                newresult->Get(repfsid.c_str()), fmd))) {
              if (!silent) {
                fprintf(stderr, "error: unable to retrieve file meta data from %s [%d]\n",
                        newresult->Get(repurl.c_str()), retc);
              }

              consistencyerror = true;
              inconsistencylable = "NOFMD";
            } else {
              XrdOucString cx = fmd.checksum().c_str();

              for (unsigned int k = (cx.length() / 2); k < SHA_DIGEST_LENGTH; k++) {
                cx += "00";
              }

              if ((option.find("%size")) != STR_NPOS) {
                char ss[1024];
                sprintf(ss, "%" PRIu64, fmd.size());
                XrdOucString sss = ss;

                if (sss != size) {
                  consistencyerror = true;
                  inconsistencylable = "SIZE";
                } else {
                  if (fmd.size() != (unsigned long long) rsize) {
                    if (!consistencyerror) {
                      consistencyerror = true;
                      inconsistencylable = "FSTSIZE";
                    }
                  }
                }
              }

              if ((option.find("%checksum")) != STR_NPOS) {
                if (cx != checksum) {
                  consistencyerror = true;
                  inconsistencylable = "CHECKSUM";
                }
              }

              if ((option.find("%checksumattr") != STR_NPOS)) {
                if ((checksumattribute.length() < 8) || (!cx.beginswith(checksumattribute))) {
                  consistencyerror = true;
                  inconsistencylable = "CHECKSUMATTR";
                }
              }

              nreplicaonline++;

              if (!silent) {
                fprintf(stdout, "nrep=\"%02d\" fsid=\"%s\" host=\"%s\" fstpath=\"%s\" "
                        "size=\"%" PRIu64 "\" statsize=\"%llu\" checksum=\"%s\"",
                        i, newresult->Get(repfsid.c_str()),
                        newresult->Get(repurl.c_str()),
                        newresult->Get(repfstpath.c_str()),
                        fmd.size(),
                        static_cast<long long>(rsize),
                        cx.c_str());
              }

              if ((option.find("%checksumattr") != STR_NPOS)) {
                if (!silent) {
                  fprintf(stdout, " checksumattr=\"%s\"\n", checksumattribute.c_str());
                }
              } else {
                if (!silent) {
                  fprintf(stdout, "\n");
                }
              }
            }
          }

          if ((option.find("%silent")) != STR_NPOS) {
            silent = oldsilent;
          }
        } else {
          break;
        }
      }

      if ((option.find("%nrep")) != STR_NPOS) {
        int nrep = 0;
        int stripes = 0;

        if (newresult->Get("mgm.stripes")) {
          stripes = atoi(newresult->Get("mgm.stripes"));
        }

        if (newresult->Get("mgm.nrep")) {
          nrep = atoi(newresult->Get("mgm.nrep"));
        }

        if (nrep != stripes) {
          consistencyerror = true;

          if (inconsistencylable != "NOFMD") {
            inconsistencylable = "REPLICA";
          }
        }
      }

      if ((option.find("%output")) != STR_NPOS) {
        if (consistencyerror) {
          fprintf(stdout,
                  "INCONSISTENCY %s path=%-32s fid=%s size=%s stripes=%s nrep=%s nrepstored=%d nreponline=%d checksumtype=%s checksum=%s\n",
                  inconsistencylable.c_str(), path.c_str(), newresult->Get("mgm.fid0"),
                  size.c_str(), newresult->Get("mgm.stripes"), newresult->Get("mgm.nrep"), i,
                  nreplicaonline, checksumtype.c_str(), newresult->Get("mgm.checksum"));
        }
      }

      delete newresult;
    } else {
      fprintf(stderr, "error: couldn't get meta data information\n");
      global_retc = EIO;
      return (0);
    }

    return (consistencyerror);
  }

  if (option.length()) {
    in += "&mgm.file.option=";
    in += option;
  }

  global_retc = output_result(client_command(in));
  return (0);
com_file_usage:
  fprintf(stdout,
          "Usage: file adjustreplica|check|convert|copy|drop|info|layout|move|purge|rename|replicate|verify|version ...\n");
  fprintf(stdout,
          "'[eos] file ..' provides the file management interface of EOS.\n");
  fprintf(stdout, "Options:\n");
  fprintf(stdout,
          "file adjustreplica [--nodrop] <path>|fid:<fid-dec>|fxid:<fid-hex> [space [subgroup]] :\n");
  fprintf(stdout,
          "                                                  tries to bring a files with replica layouts to the nominal replica level [ need to be root ]\n");
  fprintf(stdout,
          "file check [<path>|fid:<fid-dec>|fxid:<fid-hex>] [%%size%%checksum%%nrep%%checksumattr%%force%%output%%silent] :\n");
  fprintf(stdout,
          "                                                  retrieves stat information from the physical replicas and verifies the correctness\n");
  fprintf(stdout,
          "       - %%size                                                       :  return with an error code if there is a mismatch between the size meta data information\n");
  fprintf(stdout,
          "       - %%checksum                                                   :  return with an error code if there is a mismatch between the checksum meta data information\n");
  fprintf(stdout,
          "       - %%nrep                                                       :  return with an error code if there is a mismatch between the layout number of replicas and the existing replicas\n");
  fprintf(stdout,
          "       - %%checksumattr                                               :  return with an error code if there is a mismatch between the checksum in the extended attributes on the FST and the FMD checksum\n");
  fprintf(stdout,
          "       - %%silent                                                     :  suppresses all information for each replic to be printed\n");
  fprintf(stdout,
          "       - %%force                                                      :  forces to get the MD even if the node is down\n");
  fprintf(stdout,
          "       - %%output                                                     :  prints lines with inconsitency information\n");
  fprintf(stdout,
          "file convert [--sync|--rewrite] [<path>|fid:<fid-dec>|fxid:<fid-hex>] [<layout>:<stripes> | <layout-id> | <sys.attribute.name>] [target-space] [placement-policy]:\n");
  fprintf(stdout,
          "                                                                         convert the layout of a file\n");
  fprintf(stdout,
          "        <layout>:<stripes>   : specify the target layout and number of stripes\n");
  fprintf(stdout,
          "        <layout-id>          : specify the hexadecimal layout id \n");
  fprintf(stdout,
          "        <conversion-name>    : specify the name of the attribute sys.conversion.<name> in the parent directory of <path> defining the target layout\n");
  fprintf(stdout,
          "        <target-space>       : optional name of the target space or group e.g. default or default.3\n");
  fprintf(stdout,
          "        <placement-policy>   : optional placement policy valid values are 'scattered','hybrid:<some_geotag>' and 'gathered:<some_geotag>'\n");
  fprintf(stdout,
          "        --sync               : run convertion in synchronous mode (by default conversions are asynchronous) - not supported yet\n");
  fprintf(stdout,
          "        --rewrite            : run convertion rewriting the file as is creating new copies and dropping old\n");
  fprintf(stdout,
          "file copy [-f] [-s] [-c] <src> <dst>                                   :  synchronous third party copy from <src> to <dst>\n");
  fprintf(stdout,
          "         <src>                                                         :  source can be a file or a directory (<path>|fid:<fid-dec>|fxid:<fid-hex>) \n");
  fprintf(stdout,
          "         <dst>                                                         :  destination can be a file (if source is a file) or a directory\n");
  fprintf(stdout,
          "                                                                     -f :  force overwrite\n");
  fprintf(stdout,
          "                                                                     -c :  clone the file (keep ctime,mtime)\n");
  fprintf(stdout,
          "file drop [<path>|fid:<fid-dec>|fxid:<fid-hex>] <fsid> [-f] :\n");
  fprintf(stdout,
          "                                                  drop the file <path> from <fsid> - force removes replica without trigger/wait for deletion (used to retire a filesystem) \n");
  fprintf(stdout, "file info [<path>|fid:<fid-dec>|fxid:<fid-hex>] :\n");
  fprintf(stdout,
          "                                                  convenience function aliasing to 'fileinfo' command\n");
  fprintf(stdout,
          "file layout <path>|fid:<fid-dec>|fxid:<fid-hex>  -stripes <n> :\n");
  fprintf(stdout,
          "                                                  change the number of stripes of a file with replica layout to <n>\n");
  fprintf(stdout,
          "file layout <path>|fid:<fid-dec>|fxid:<fid-hex>  -checksum <checksum-type> :\n");
  fprintf(stdout,
          "                                                  change the checksum-type of a file to <checksum-type>\n");
  fprintf(stdout,
          "file move [<path>|fid:<fid-dec>|fxid:<fid-hex>] <fsid1> <fsid2> :\n");
  fprintf(stdout,
          "                                                  move the file <path> from  <fsid1> to <fsid2>\n");
  fprintf(stdout, "file purge <path> [purge-version] :\n");
  fprintf(stdout,
          "                                                  keep maximumg <purge-version> versions of a file. If not specified apply the attribute definition from sys.versioning.\n");
  fprintf(stdout, "file rename [<path>|fid:<fid-dec>|fxid:<fid-hex>] <new> :\n");
  fprintf(stdout,
          "                                                  rename from <old> to <new> name (works for files and directories!).\n");
  fprintf(stdout,
          "file replicate [<path>|fid:<fid-dec>|fxid:<fid-hex>] <fsid1> <fsid2> :\n");
  fprintf(stdout,
          "                                                  replicate file <path> part on <fsid1> to <fsid2>\n");
  fprintf(stdout, "file symlink <name> <link-name> :\n");
  fprintf(stdout,
          "                                                  create a symlink with <name> pointing to <link-name>\n");
  fprintf(stdout, "file tag <name> +|-|~<fsid> :\n");
  fprintf(stdout,
          "                                                  add/remove/unlink a filesystem location to/from a file in the location index - attention this does not move any data!\n");
  fprintf(stdout,
          "                                                  unlink keeps the location in the list of deleted files e.g. the location get's a deletion request\n");
  fprintf(stdout, "file touch [<path>|fid:<fid-dec>|fxid:<fid-hex>] :\n");
  fprintf(stdout,
          "                                                   create a 0-size/0-replica file if <path> does not exist or update modification time of an existing file to the present time\n");
  fprintf(stdout,
          "file verify <path>|fid:<fid-dec>|fxid:<fid-hex> [<fsid>] [-checksum] [-commitchecksum] [-commitsize] [-rate <rate>] : \n");
  fprintf(stdout,
          "                                                  verify a file against the disk images\n");
  fprintf(stdout,
          "       <fsid>          : verifies only the replica on <fsid>\n");
  fprintf(stdout,
          "       -checksum       : trigger the checksum calculation during the verification process\n");
  fprintf(stdout,
          "       -commitchecksum : commit the computed checksum to the MGM\n");
  fprintf(stdout, "       -commitsize     : commit the file size to the MGM\n");
  fprintf(stdout,
          "       -rate <rate>    : restrict the verification speed to <rate> per node\n");
  fprintf(stdout, "file version <path> [purge-version] :\n");
  fprintf(stdout,
          "                                                 create a new version of a file by cloning\n");
  fprintf(stdout, "file versions [grab-version] :\n");
  fprintf(stdout,
          "                                                 list versions of a file\n");
  fprintf(stdout,
          "                                                 grab a version of a file\n");
  fprintf(stdout,
          "        <purge-version>: defines the max. number of versions to keep\n");
  fprintf(stdout, "\n");
  fprintf(stdout,
          "                         if not specified it will add a new version without purging any previous version\n");
  fprintf(stdout, "file share <path> [lifetime] :\n");
  fprintf(stdout, "       <path>          : path to create a share link\n");
  fprintf(stdout,
          "        <lifetime>      : validity time of the share link like 1, 1s, 1d, 1w, 1mo, 1y, ... default is 28d\n");
  fprintf(stdout, "\n");
  fprintf(stdout,
          " file workflow <path>|fid:<fid-dec>|fxid:<fid-hex> <workflow> <event> :\n");
  fprintf(stdout,
          "                                                  trigger workflow <workflow> with event <event> on <path>\n");
  fprintf(stdout, "\n");
  global_retc = EINVAL;
  return (0);
}
