#include <stdio.h>
#include "XrdSfs/XrdSfsInterface.hh"
#include "common/Logging.hh"
#include "fst/XrdFstOfs.hh"
#include "fst/http/HttpServer.hh"
#include "common/http/ProtocolHandler.hh"
#include "common/StringConversion.hh"
#include "common/Timing.hh"
#include "EosFstHttpHandler.hh"

XrdVERSIONINFO(XrdSfsGetFileSystem, EosFstHttp);

bool 
EosFstHttpHandler::MatchesPath(const char *verb, const char *path)
{
  if (EOS_LOGS_DEBUG) {
    eos_static_debug("verb=%s path=%s", verb, path);
  }
  if (std::string(verb) == "COPY") {
    return false;
  } else {
    return true;
  }
}



int
EosFstHttpHandler::ProcessReq(XrdHttpExtReq &req)
{
  std::string body;

  if (!OFS) {
    eos_static_crit("OFS not accessible");
    return -1;
  }

  std::map<std::string,std::string> cookies;

  // normalize the input headers to lower-case
  std::map<std::string,std::string> normalized_headers;
  for (auto it = req.headers.begin(); it != req.headers.end(); ++it) {
    normalized_headers[LC_STRING(it->first)] = it->second;
  }

  std::string query = normalized_headers.count("xrd-http-query")? normalized_headers["xrd-http-query"]: "";
  
  std::string verb = req.verb;
  if (req.verb == "PUT") {
    verb = "CREATE"; // CREATE makes sure, the the handler just opens the file and all writes are done later
  }
  std::unique_ptr<eos::common::ProtocolHandler> handler = OFS->Httpd->XrdHttpHandler(verb,
										     req.resource, 
										     normalized_headers,
										     query,
										     cookies,
										     body, 
										     req.GetSecEntity());

  eos::common::HttpResponse* response = handler->GetResponse();
  
  if (response) {
    std::string header;

    response->AddHeader("Date",  eos::common::Timing::utctime(time(NULL)));

    off_t content_length = 0;
    auto headers = response->GetHeaders();
    for ( auto it = headers.begin(); it != headers.end(); ++it) {
      if (it->first == "Content-Length") {
	// this is added by SendSimpleResp, don't add it here
	content_length = strtoull(it->second.c_str(), 0, 10);
	continue;
      }
      header += it->first;
      header += ": ";
      header += it->second;
      header += "\r\n";
    }

    if (headers.size()) {
      header.erase(header.length()-2);
    }
    if (EOS_LOGS_DEBUG) {
      eos_static_debug("response-header: %s", header.c_str());
    }

    if ( req.verb == "HEAD" ) {
      return req.SendSimpleResp(response->GetResponseCode(), response->GetResponseCodeDescription().c_str(),
				header.c_str(), response->GetBody().c_str(), response->GetBody().length());
    }
    
    if ( req.verb == "GET") {
      if (response->GetResponseCode() != 200) {
	return req.SendSimpleResp(response->GetResponseCode(), response->GetResponseCodeDescription().c_str(),
				  header.c_str(), response->GetBody().c_str(), response->GetBody().length());
      }	else {
	int retc = 0;
	retc = req.SendSimpleResp(0, response->GetResponseCodeDescription().c_str(),
			   header.c_str(), 0 , content_length);
	if (retc) 
	  return retc;

	ssize_t nread = 0;
	off_t pos = 0;

	// allocate an IO buffer of 1M or if smaller the required content length
	std::vector<char> buffer (content_length > (1*1024*1024)? (1*1024*1024): content_length);
	do { 
	  if (EOS_LOGS_DEBUG) {
	    eos_static_debug("pos=%llu size=%u", pos, buffer.capacity());
	  }

	  nread = OFS->Httpd->FileReader(handler.get(),pos, &buffer[0], buffer.capacity());

	  if (nread >= 0) {
	    pos += nread;
	    retc |= req.SendSimpleResp(1, 0, 0, &buffer[0], nread);
	    eos_static_debug("retc=%d", retc);
	  } else {
	    retc = -1;
	  }
	} while ( (pos != content_length) && (nread>0) && !retc);

	OFS->Httpd->FileClose(handler.get(), retc);
	return retc;
      }
    }

    if ( req.verb == "PUT") {
      content_length = strtoull(normalized_headers.count("content-length")? normalized_headers["content-length"].c_str(): "-1",0,10);

      if (EOS_LOGS_DEBUG) {
	eos_static_debug("response-code=%d", response->GetResponseCode());
      }

      if (( response->GetResponseCode() != 0 ) && 
	  ( response->GetResponseCode() != 200 ) 
	  ) {
	return req.SendSimpleResp(response->GetResponseCode(), response->GetResponseCodeDescription().c_str(),
				  header.c_str(), response->GetBody().c_str(), response->GetBody().length());
      } else {
	if ( (response->GetResponseCode() == 0) && normalized_headers.count("expect") && 
	     ( normalized_headers["expect"] == "100-continue" ) ) {
	  // reply to 100-CONTINUE request
	  if (EOS_LOGS_DEBUG) {
	    eos_static_debug("sending 100-continue");
	  }
	  req.SendSimpleResp(100, "","","",0);
	}

	int retc = 0;
	off_t content_left = content_length;
	do {
	  size_t content_read = std::min(1*1024*1024l, content_left);
	  char* data = 0;
	  size_t rbytes = req.BuffgetData(content_read, &data, true);

	  // @TODO: improve me by avoiding to copy the buffer
	  body.reserve(content_read);
	  body.assign(data, rbytes);

	  if (EOS_LOGS_DEBUG) {
	    eos_static_info("content-read=%lu rbytes=%lu body=%u", content_read, rbytes, body.size());
	  }
	  
	  if (rbytes != content_read) {
	    eos_static_crit("short read during put - receveid %lu instead of %lu bytes",
			    content_read, rbytes);
	    retc = -1;
	  } else {
	    retc |= OFS->Httpd->FileWriter(handler.get(),
					   req.verb,
					   req.resource, 
					   normalized_headers,
					   query,
					   cookies,
					   body);

	    if (!retc) {
	      content_left -= content_read;
	    }
	  }
	} while ( !retc && content_left );
	
	if (EOS_LOGS_DEBUG) {
	  eos_static_debug("retc=%d", retc);
	}

	if (!retc) {
	  // trigger the close handler by calling with empty body
	  body.clear();
	  retc |= OFS->Httpd->FileWriter(handler.get(),
					 req.verb,
					 req.resource, 
					 normalized_headers,
					 query,
					 cookies,
					 body);
	}

	eos::common::HttpResponse* response = handler->GetResponse();

	
	if (response && response->GetResponseCode()) {
	  return req.SendSimpleResp(response->GetResponseCode(), response->GetResponseCodeDescription().c_str(),
				    header.c_str(), response->GetBody().c_str(), response->GetBody().length());
	} else {
	  return req.SendSimpleResp(500, "fatal internal error", "", "", 0);
	}
      }
    }

    return 0;
  } else {
    std::string errmsg = "failed to create response object";
    return req.SendSimpleResp(500, errmsg.c_str(), "", errmsg.c_str(), errmsg.length());
  }
}

int 
EosFstHttpHandler::Init(const char *cfgfile)
{
  if (getenv("EOSFSTOFS")) {
    OFS = (eos::fst::XrdFstOfs*) (strtoull(getenv("EOSFSTOFS"),0,10));
  }

  std::string cfg;
  eos::common::StringConversion::LoadFileIntoString(cfgfile, cfg);
  size_t fpos = cfg.find("xrd.protocol XrdHttp:");

  if ( fpos != std::string::npos) {
    size_t epos = cfg.find(" ", fpos+21);
    if (epos != std::string::npos) {
      std::string port = cfg.substr(fpos+21, epos-fpos-21);
      setenv("EOSFSTXRDHTTP",port.c_str(),1);
      eos_static_notice("publishing HTTP port: %s", port.c_str());
    }
  }
  



  return 0;
}
