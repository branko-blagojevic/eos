//------------------------------------------------------------------------------
//! file EosMgmHttpHandler.cc
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

#include "EosMgmHttpHandler.hh"
#include "common/Logging.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm//http/HttpServer.hh"
#include "common/http/ProtocolHandler.hh"
#include "common/StringConversion.hh"
#include "common/StringTokenizer.hh"
#include "common/StringUtils.hh"
#include "common/Timing.hh"
#include "XrdSfs/XrdSfsInterface.hh"
#include "XrdSys/XrdSysPlugin.hh"
#include "XrdAcc/XrdAccAuthorize.hh"
#include <stdio.h>

XrdVERSIONINFO(XrdHttpGetExtHandler, EosMgmHttp);
static XrdVERSIONINFODEF(compiledVer, EosMgmHttp, XrdVNUMBER, XrdVERSION);

//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
OwningXrdSecEntity::~OwningXrdSecEntity()
{
  if (mSecEntity) {
    free(mSecEntity->name);
    free(mSecEntity->host);
    free(mSecEntity->vorg);
    free(mSecEntity->role);
    free(mSecEntity->grps);
    free(mSecEntity->endorsements);
    free(mSecEntity->moninfo);
    free(mSecEntity->creds);
    free(mSecEntity->addrInfo);
    free((char*)mSecEntity->tident);
  }
}

//------------------------------------------------------------------------------
// Copy XrdSecEntity info
//------------------------------------------------------------------------------
void
OwningXrdSecEntity::CreateFrom(const XrdSecEntity& other)
{
  if (mSecEntity == nullptr) {
    mSecEntity = std::make_unique<XrdSecEntity>();
  }

  mSecEntity->Reset();
  strncpy(mSecEntity->prot, other.prot, XrdSecPROTOIDSIZE - 1);

  if (other.name) {
    mSecEntity->name = strdup(other.name);
  }

  if (other.host) {
    mSecEntity->host = strdup(other.host);
  }

  if (other.vorg) {
    mSecEntity->vorg = strdup(other.vorg);
  }

  if (other.role) {
    mSecEntity->role = strdup(other.role);
  }

  if (other.grps) {
    mSecEntity->grps = strdup(other.grps);
  }

  if (other.endorsements) {
    mSecEntity->endorsements = strdup(other.endorsements);
  }

  if (other.moninfo) {
    mSecEntity->moninfo = strdup(other.moninfo);
  }

  if (other.creds) {
    mSecEntity->creds = strdup(other.creds);
  }

  mSecEntity->credslen = other.credslen;
  mSecEntity->rsvd = other.rsvd;
  // @note addrInfo is not copied for the moment
  mSecEntity->addrInfo = nullptr;

  if (other.tident) {
    mSecEntity->tident = strdup(other.tident);
  }

  // @note sessvar is null
  mSecEntity->sessvar = nullptr;
  *mSecEntity;
}


//------------------------------------------------------------------------------
// Configure the external request handler
//------------------------------------------------------------------------------
int
EosMgmHttpHandler::Config(XrdSysError* eDest, const char* confg,
                          const char* parms, XrdOucEnv* myEnv)
{
  using namespace eos::common;
  std::string macaroons_lib_path {""};

  if (getenv("EOSMGMOFS")) {
    //@todo(esindril): this is ugly, there should be some other way of getting
    // this info ...
    gOfs = (XrdMgmOfs*)(strtoull(getenv("EOSMGMOFS"), 0, 10));
    std::string cfg;
    StringConversion::LoadFileIntoString(confg, cfg);
    auto lines = StringTokenizer::split<std::vector<std::string>>(cfg, '\n');

    for (auto& line : lines) {
      if (line.find("eos::mgm::http::redirect-to-https=1") != std::string::npos) {
        mRedirectToHttps = true;
      } else if (line.find("mgmofs.macaroonslib") != std::string::npos) {
        trim(line);
        eos_info("line=\"%s\"", line.c_str());
        auto tokens = StringTokenizer::split<std::vector<std::string>>(line, ' ');

        if (tokens.size() != 2) {
          eos_err("%s", "msg=\"missing mgmofs.macaroonslib configuration\"");
          eos_err("tokens_size=%i", tokens.size());
          return 1;
        }

        macaroons_lib_path = tokens[1];
        // Make sure the path exists
        struct stat info;

        if (stat(macaroons_lib_path.c_str(), &info)) {
          eos_err("msg=\"no such mgmofs.macaroonslib on disk\" path=%s",
                  macaroons_lib_path.c_str());
          return 1;
        }
      }
    }
  }

  if (macaroons_lib_path.empty()) {
    eos_err("%s", "msg=\"missing mandatory mgmofs.macaroonslib config\"");
    return 1;
  }

  eos_notice("configuration: redirect-to-https:%d", mRedirectToHttps);
  // Try to load the XrdHttpGetExtHandler from the libXrdMacaroons library
  XrdHttpExtHandler *(*ep)(XrdHttpExtHandlerArgs);
  std::string http_symbol {"XrdHttpGetExtHandler"};
  XrdSysPlugin* plugin = new XrdSysPlugin(eDest, macaroons_lib_path.c_str(),
                                          "macaroonslib", &compiledVer, 1);
  void* http_addr = plugin->getPlugin(http_symbol.c_str(), 0, 0);
  ep = (XrdHttpExtHandler * (*)(XrdHttpExtHandlerArgs))(http_addr);

  if (ep && (mMacaroonsHandler = ep(eDest, confg, parms, myEnv))) {
    eos_info("%s", "msg=\"XrdHttpGetExthandler from libXrdMacaroons loaded "
             "successfully\"");
  } else {
    eos_err("%s", "msg=\"failed loading XrdHttpGetExtHandler from "
            "libXrdMacaroons\"");
    delete plugin;
    return 1;
  }

  // Try to load the XrdAccAuthorizeObject provided by the libXrdMacaroons
  // library
  XrdAccAuthorize *(*authz_ep)(XrdSysLogger*, const char*, const char*);
  std::string authz_symbol {"XrdAccAuthorizeObject"};
  void* authz_addr = plugin->getPlugin(authz_symbol.c_str(), 0, 0);
  authz_ep = (XrdAccAuthorize * (*)(XrdSysLogger*, const char*,
                                    const char*))(authz_addr);

  // The "parms" argument needs to be set to nullptr to avoid trying to load
  // the default authz plugin which is not used in EOS.
  if (authz_ep &&
      (mAuthzMacaroonsHandler = authz_ep(eDest->logger(), confg, nullptr))) {
    eos_info("%s", "msg=\"XrdAccAuthorizeObject from libXrdMacaroons loaded "
             "successfully\"");
  } else {
    eos_err("%s", "msg=\"failed loading XrdAccAuthorizeObject from "
            "libXrdMacaroons\"");
    delete plugin;
    return 1;
  }

  return 0;
}

//------------------------------------------------------------------------------
// Decide if current handler should be invoked
//------------------------------------------------------------------------------
bool
EosMgmHttpHandler::MatchesPath(const char* verb, const char* path)
{
  eos_static_info("verb=%s path=%s", verb, path);
  return true;
}

//------------------------------------------------------------------------------
// Process the HTTP request and send the response using by calling the
// XrdHttpProtocol directly
//------------------------------------------------------------------------------
int
EosMgmHttpHandler::ProcessReq(XrdHttpExtReq& req)
{
  std::string body;

  if (req.verb == "POST") {
    // Delegate request to the XrdMacaroons library
    eos_info("%s", "msg=\"delegate request to XrdMacaroons library\"");
    return mMacaroonsHandler->ProcessReq(req);
  }

  if (req.verb == "PROPFIND") {
    // read the body
    body.resize(req.length);
    char* data = 0;
    int rbytes = req.BuffgetData(req.length, &data, true);
    body.assign(data, (size_t) rbytes);
  }

  // Normalize the input headers to lower-case
  std::map<std::string, std::string> normalized_headers;

  for (const auto& hdr : req.headers) {
    eos_info("hdr key=%s value=%s", hdr.first.c_str(), hdr.second.c_str());
    normalized_headers[LC_STRING(hdr.first)] = hdr.second;
  }

  std::string path = normalized_headers["xrd-http-fullresource"];
  std::string authz_data = normalized_headers["authorization"];
  Access_Operation oper = AOP_Stat;
  std::string data = "authz=";
  data += authz_data;
  std::unique_ptr<XrdOucEnv> env = std::make_unique<XrdOucEnv>(data.c_str(),
                                   data.length());
  *env;
  // Make a copy of the original XrdSecEntity so that the authorization plugin
  // can update the name of the client from the macaroon info
  OwningXrdSecEntity client(req.GetSecEntity());
  eos_info("before authorization client_name=%s", client.GetObj()->name);
  mAuthzMacaroonsHandler->Access(client.GetObj(), path.c_str(), oper, env.get());
  eos_info("after authorization client_name=%s", client.GetObj()->name);
  std::string query = (normalized_headers.count("xrd-http-query") ?
                       normalized_headers["xrd-http-query"] : "");
  std::map<std::string, std::string> cookies;
  std::unique_ptr<eos::common::ProtocolHandler> handler =
    gOfs->mHttpd->XrdHttpHandler(req.verb, req.resource, normalized_headers,
                                 query, cookies, body, *client.GetObj());
  eos::common::HttpResponse* response = handler->GetResponse();

  if (response == nullptr) {
    std::string errmsg = "failed to create response object";
    return req.SendSimpleResp(500, errmsg.c_str(), "", errmsg.c_str(),
                              errmsg.length());
  }

  std::ostringstream oss_header;
  response->AddHeader("Date",  eos::common::Timing::utctime(time(NULL)));
  auto headers = response->GetHeaders();

  for (const auto& hdr : headers) {
    std::string key = hdr.first;
    std::string val = hdr.second;

    // This is added by SendSimpleResp, don't add it here
    if (key == "Content-Length") {
      continue;
    }

    if (mRedirectToHttps) {
      if (key == "Location") {
        if (normalized_headers["xrd-http-prot"] == "https") {
          if (!normalized_headers.count("xrd-http-redirect-http") ||
              (normalized_headers["xrd-http-redirect-http"] == "0")) {
            // Re-write http: as https:
            val.insert(4, "s");
          }
        }
      }
    }

    if (!oss_header.str().empty()) {
      oss_header << "\r\n";
    }

    oss_header << key << ":" << val;
  }

  eos_debug("response-header: %s", oss_header.str().c_str());
  return req.SendSimpleResp(response->GetResponseCode(),
                            response->GetResponseCodeDescription().c_str(),
                            oss_header.str().c_str(), response->GetBody().c_str(),
                            response->GetBody().length());
}
