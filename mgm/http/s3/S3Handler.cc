// ----------------------------------------------------------------------
// File: S3Handler.cc
// Author: Andreas-Joachim Peters & Justin Lewis Salmon - CERN
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

/*----------------------------------------------------------------------------*/
#include "mgm/http/s3/S3Handler.hh"
#include "mgm/XrdMgmOfs.hh"
#include "common/http/PlainHttpResponse.hh"
#include "common/Logging.hh"
#include "common/SymKeys.hh"
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
#include <string>
#include <map>
/*----------------------------------------------------------------------------*/

EOSMGMNAMESPACE_BEGIN

char s3_rfc3986[256] = {0};
char s3_html5[256] = {0};

void
s3_uri_encode(unsigned char* s, char* enc, char* tb)
{
  for (; *s; s++) {
    if (tb[*s]) {
      sprintf(enc, "%c", tb[*s]);
    } else {
      sprintf(enc, "%%%02X", *s);
    }

    while (*++enc);
  }
}

S3Store* S3Handler::mS3Store = 0;

/*----------------------------------------------------------------------------*/
S3Handler::S3Handler(eos::common::Mapping::VirtualIdentity* vid) :
  eos::common::ProtocolHandler(vid)
{
  mIsS3 = false;
  mId = mSignature = mHost = mContentMD5 = mContentType = mUserAgent = "";
  mHttpMethod = mPath = mQuery = mBucket = mDate = "";
  mVirtualHost = false;

  if (!mS3Store) {
    // create the store if it does not exist yet
    mS3Store = new S3Store(gOFS->MgmProcPath.c_str());

    // initialize encoding table
    for (int i = 0; i < 256; i++) {
      s3_rfc3986[i] = isalnum(i) || i == '-' || i == '.' || i == '_'
                      || i == '@'
                      ? i : 0;
      s3_html5[i] = isalnum(i) || i == '*' || i == '-' || i == '.' || i == '_'
                    ? i : (i == ' ') ? '+' : 0;
    }
  }
}

XrdOucString
S3Handler::EncodeURI(const char* uri)
{
  XrdOucString nUri;
  char enc[(strlen(uri) + 1) * 3];
  s3_uri_encode((unsigned char*) uri, enc, s3_rfc3986);
  XrdOucString lUri = enc;
  return lUri;
}

/*----------------------------------------------------------------------------*/
bool
S3Handler::Matches(const std::string& method, HeaderMap& headers)
{
  if (headers.count("authorization")) {
    if (headers["authorization"].substr(0, 3) == "AWS") {
      eos_static_debug("msg=\"matched S3 protocol for request\"");
      return true;
    }
  }

  return false;
}

/*----------------------------------------------------------------------------*/
void
S3Handler::HandleRequest(eos::common::HttpRequest* request)
{
  eos_static_debug("msg=\"handling s3 request\"");
  eos::common::HttpResponse* response = 0;
  // Parse the headers
  ParseHeader(request);
  // Refresh the data store
  mS3Store->Refresh();

  if (!mS3Store->GetKeys().count(GetId())) {
    response = RestErrorResponse(response->FORBIDDEN, "InvalidAccessKeyId",
                                 "No corresponding S3 account was found",
                                  GetId(), "");
  } else if (VerifySignature()) {
    request->AddEosApp();
    int meth = ParseMethodString(request->GetMethod());

    switch (meth) {
    case GET:
      response = Get(request);
      break;

    case HEAD:
      response = Head(request);
      break;

    case PUT:
      response = Put(request);
      break;

    case DELETE:
      response = Delete(request);
      break;

    default:
      response = new eos::common::PlainHttpResponse();
      response->SetResponseCode(eos::common::HttpResponse::NOT_IMPLEMENTED);
    }
  } else {
    response = RestErrorResponse(response->FORBIDDEN,
                                 "SignatureDoesNotMatch", "", GetBucket(), "");
  }

  mHttpResponse = response;
}

/*----------------------------------------------------------------------------*/
bool
S3Handler::VerifySignature()
{
  std::string secure_key = mS3Store->GetKeys()[GetId()];
  std::string string2sign = GetHttpMethod();
  string2sign += "\n";
  string2sign += GetContentMD5();
  string2sign += "\n";
  string2sign += GetContentType();
  string2sign += "\n";
  string2sign += GetDate();
  string2sign += "\n";
  string2sign += GetCanonicalizedAmzHeaders();

  if (GetBucket().length()) {
    string2sign += "/";
    string2sign += GetBucket();
  };

  string2sign += GetPath();

  if (ExtractSubResource().length()) {
    string2sign += "?";
    string2sign += GetSubResource();
  }

  eos_static_debug("s2sign=%s key=%s", string2sign.c_str(), secure_key.c_str());
  // get hmac sha1 hash
  std::string hmac1 = eos::common::SymKey::HmacSha1(string2sign,
                      secure_key.c_str());
  XrdOucString b64mac1;
  // base64 encode the hash
  eos::common::SymKey::Base64Encode((char*) hmac1.c_str(),
                                    SHA_DIGEST_LENGTH, b64mac1);
  std::string verify_signature = b64mac1.c_str();
  eos_static_debug("in_signature=%s out_signature=%s\n",
                   GetSignature().c_str(), verify_signature.c_str());

  if (verify_signature != GetSignature()) {
    // --------------------------------------------------------------------------
    // try if the non-bucket path needs '/' encoded as '%2F' as done by Cyberduck
    // e.g. /<bucket>/<path-without-slash-inthe-beginnging>
    // --------------------------------------------------------------------------
    XrdOucString encodedPath = GetPath().c_str();
    encodedPath = EncodeURI(encodedPath.c_str() + 1);
    encodedPath.insert('/', 0);
    XrdOucString newstring2sign = string2sign.c_str();
    newstring2sign.replace(GetPath().c_str(), encodedPath.c_str());
    string2sign = newstring2sign.c_str();
    hmac1 = eos::common::SymKey::HmacSha1(string2sign, secure_key.c_str());
    b64mac1 = "";
    eos::common::SymKey::Base64Encode((char*) hmac1.c_str(),
                                      SHA_DIGEST_LENGTH, b64mac1);
    verify_signature = b64mac1.c_str();
    eos_static_debug("s2sign=%s key=%s", string2sign.c_str(), secure_key.c_str());
    eos_static_debug("in_signature=%s out_signature=%s\n",
                     GetSignature().c_str(), verify_signature.c_str());
    return (verify_signature == GetSignature());
  }

  return true;
}

/*----------------------------------------------------------------------------*/
eos::common::HttpResponse*
S3Handler::Get(eos::common::HttpRequest* request)
{
  eos::common::HttpResponse* response = 0;

  if (GetBucket() == "") {
    response = mS3Store->ListBuckets(GetId());
  } else {
    if (GetPath() == "/") {
      response = mS3Store->ListBucket(GetBucket(), GetQuery());
    } else {
      response = mS3Store->GetObject(request, GetId(), GetBucket(), GetPath(),
                                     GetQuery());
    }
  }

  return response;
}

/*----------------------------------------------------------------------------*/
eos::common::HttpResponse*
S3Handler::Head(eos::common::HttpRequest* request)
{
  eos::common::HttpResponse* response = 0;

  if (GetPath() == "/") {
    response = mS3Store->HeadBucket(GetId(), GetBucket(), GetDate());
  } else {
    response = mS3Store->HeadObject(GetId(), GetBucket(), GetPath(), GetDate());
  }

  return response;
}

/*----------------------------------------------------------------------------*/
eos::common::HttpResponse*
S3Handler::Put(eos::common::HttpRequest* request)
{
  eos::common::HttpResponse* response = 0;
  response = mS3Store->PutObject(request, GetId(), GetBucket(), GetPath(),
                                 GetQuery());
  return response;
}

/*----------------------------------------------------------------------------*/
eos::common::HttpResponse*
S3Handler::Delete(eos::common::HttpRequest* request)
{
  eos::common::HttpResponse* response = 0;
  response = mS3Store->DeleteObject(request, GetId(), GetBucket(), GetPath());
  return response;
}

/*----------------------------------------------------------------------------*/
EOSMGMNAMESPACE_END
