/*----------------------------------------------------------------------------*/
#include "XrdMqOfs/XrdMqMessaging.hh"
#include "XrdMgmOfs/XrdMgmFstNode.hh"
/*----------------------------------------------------------------------------*/

XrdOucHash<XrdMgmFstNode> XrdMgmFstNode::gFstNodes;
XrdSysMutex XrdMgmFstNode::gMutex;

/*----------------------------------------------------------------------------*/
bool
XrdMgmFstNode::SetNodeStatus(int status) 
{
  if (status == kOffline) {
    int fsstatus = XrdMgmFstFileSystem::kDown;
    // disable the filesystems here!
    fileSystems.Apply(XrdMgmFstNode::SetStatusFileSystem, &fsstatus);    
  }
  nodeStatus = status;
}

/*----------------------------------------------------------------------------*/
bool
XrdMgmFstNode::Update(XrdAdvisoryMqMessage* advmsg) 
{
  if (!advmsg)
    return false;

  gMutex.Lock();
  XrdMgmFstNode* node = gFstNodes.Find(advmsg->kQueue.c_str());
  if (!node) {
    // create one
    node = new XrdMgmFstNode(advmsg->kQueue.c_str());
    node->hostPortName = advmsg->kQueue.c_str();
    int pos = node->hostPortName.find("/",2);
    if (pos != STR_NPOS) 
      node->hostPortName.erase(0,pos-1);
    gFstNodes.Add(advmsg->kQueue.c_str(), node);
  } else {
    // update the one
    node->lastHeartBeat = advmsg->kMessageHeader.kSenderTime_sec;
    node->SetNodeStatus(advmsg->kOnline);
  }
  gMutex.UnLock();
  return true;
}

/*----------------------------------------------------------------------------*/
bool
XrdMgmFstNode::Update(XrdOucEnv &config) 
{
  XrdOucString infsname = config.Get("mgm.fsname");
  XrdOucString sid      = config.Get("mgm.fsid");
  XrdOucString schedgroup = config.Get("mgm.fsschedgroup");
  XrdOucString fsstatus     = config.Get("mgm.fsstatus");
  XrdOucString serrc = config.Get("errc");
  int errc=0;
  XrdOucString errmsg = config.Get("errmsg");
  if (serrc.length()) 
      errc = atoi(serrc.c_str());

  int envlen;
  int id = atoi(sid.c_str());
  if (!id) 
    return false;
  
  int statusid = XrdCommonFileSystem::GetStatusFromString(fsstatus.c_str());

  return Update(infsname.c_str(), id, schedgroup.c_str(),statusid, errc, errmsg.c_str());
}


/*----------------------------------------------------------------------------*/
bool
XrdMgmFstNode::Update(const char* infsname, int id, const char* schedgroup, int bootstatus, int errc, const char* errmsg) 
{
  if (!infsname) 
    return false;

  if (!schedgroup) {
    schedgroup = "default";
  }

  XrdOucString fsname = infsname;
  XrdOucString lQueue="";
  // remove // from fsnames
  while (fsname.replace("//","/")) {}
  // end fsnames with a /
  if (!fsname.endswith("/")) fsname+="/";

  lQueue = fsname;

  XrdOucString nodename = fsname;
  int spos = nodename.find("/fst/");
  if (!spos)
    return false;

  nodename.erase(spos+4);
  fsname.erase(0,spos+4);

  // get the node
  XrdMgmFstNode* node = gFstNodes.Find(nodename.c_str());
  if (!node) {
    // create one
    node = new XrdMgmFstNode(nodename.c_str());
    node->hostPortName = fsname.c_str();
    int pos = node->hostPortName.find("/",2);
    if (pos != STR_NPOS) 
      node->hostPortName.erase(0,pos-1);
    gFstNodes.Add(nodename.c_str(), node);
  } 

  // get the filesystem
  XrdMgmFstFileSystem* fs = node->fileSystems.Find(fsname.c_str());
  if (!fs) {
    // create a new filesystem there
    fs = new XrdMgmFstFileSystem(id, fsname.c_str(), nodename.c_str(), schedgroup);
    node->fileSystems.Add(fsname.c_str(),fs);
  } else {
    fs->SetId(id);
    fs->SetPath(fsname.c_str());
    fs->SetSchedulingGroup(schedgroup);
    if (bootstatus!= XrdCommonFileSystem::kDown) 
      fs->SetBootStatus(bootstatus);
  }

  if (errc) {
    fs->SetError(errc, errmsg);
  }
  return true;
}

/*----------------------------------------------------------------------------*/
XrdMgmFstNode*
XrdMgmFstNode::GetNode(const char* queue) 
{
  
  gMutex.Lock();
  XrdMgmFstNode* node = gFstNodes.Find(queue);
  gMutex.UnLock();
  return node;
}

/*----------------------------------------------------------------------------*/
int
XrdMgmFstNode::ListNodes(const char* key, XrdMgmFstNode* node, void* Arg)  
{
  XrdOucString* listing = (XrdOucString*) Arg;
  *listing += node->GetInfoString();
  *listing += XrdMgmFstFileSystem::GetInfoHeader();
  node->fileSystems.Apply(XrdMgmFstNode::ListFileSystems, Arg);
  return 0;
}

/*----------------------------------------------------------------------------*/
int
XrdMgmFstNode::ListFileSystems(const char* key, XrdMgmFstFileSystem* filesystem, void* Arg)
{
  XrdOucString* listing = (XrdOucString*) Arg;
  *listing += filesystem->GetInfoString();
  return 0;
}

/*----------------------------------------------------------------------------*/
int
XrdMgmFstNode::ExistsNodeFileSystemId(const char* key, XrdMgmFstNode* node, void* Arg)  
{
  unsigned int* listing = (unsigned int*) Arg;

  if (*listing) {
    node->fileSystems.Apply(XrdMgmFstNode::ExistsFileSystemId, Arg);
  }
  return 0;
}

/*----------------------------------------------------------------------------*/
int
XrdMgmFstNode::ExistsFileSystemId(const char* key, XrdMgmFstFileSystem* filesystem, void* Arg)
{
  unsigned int* listing = (unsigned int*) Arg;

  if (*listing) {
    if (filesystem->GetId() == *listing) 
      *listing = 0;
  }
      
  return 0;
}

/*----------------------------------------------------------------------------*/
int
XrdMgmFstNode::FindNodeFileSystem(const char* key, XrdMgmFstNode* node, void* Arg)  
{
  struct FindStruct* finder = (struct FindStruct*) Arg;
  if (!finder->found) {
    node->fileSystems.Apply(XrdMgmFstNode::FindFileSystem, Arg);
    if (finder->found) {
      finder->nodename = node->GetQueue();
      return 1;
    }
  }
  return 0;
}

/*----------------------------------------------------------------------------*/
int
XrdMgmFstNode::BootNode(const char* key, XrdMgmFstNode* node, void* Arg)  
{
  XrdOucString* bootfs = (XrdOucString*) Arg;
  (*bootfs)+="mgm.nodename=";(*bootfs)+= node->GetQueue();
  (*bootfs)+="\t";
  (*bootfs)+=" mgm.fsnames=";
  node->fileSystems.Apply(XrdMgmFstNode::BootFileSystem, Arg);
  (*bootfs)+="\n";
  return 0;
}

/*----------------------------------------------------------------------------*/
int
XrdMgmFstNode::FindFileSystem(const char* key, XrdMgmFstFileSystem* filesystem, void* Arg)
{
  struct FindStruct* finder = (struct FindStruct*) Arg;

  // find by id
  if (finder->id) {
    if (filesystem->GetId() == finder->id) {
      finder->found = true;
      finder->fsname = filesystem->GetPath(); 
      return 1;
    }
  } else {
    // find by name
    XrdOucString path = filesystem->GetPath();
    if (path.length()) {
      if (path == finder->fsname) {
	finder->found = true;
	finder->id = filesystem->GetId();
	return 1;
      }
    }
  }
  
  return 0;
}

/*----------------------------------------------------------------------------*/
int
XrdMgmFstNode::BootFileSystem(const char* key, XrdMgmFstFileSystem* filesystem, void *Arg)
{
  XrdOucString* bootfs = (XrdOucString*) Arg;
  XrdMqMessage message("mgm"); XrdOucString msgbody="";
  XrdOucEnv config(filesystem->GetBootString());

  XrdCommonFileSystem::GetBootRequestString(msgbody,config);

  message.SetBody(msgbody.c_str());
  XrdOucString lastchar = bootfs->c_str()+bootfs->length()-1;
  if (lastchar != "=") {
    *bootfs+=",";
  }
  if (XrdMqMessaging::gMessageClient.SendMessage(message, filesystem->GetQueue())) {
    *bootfs+= filesystem->GetPath();
    filesystem->SetBootSent();
  } else {
    filesystem->SetBootFailure("no fst listening on this queue");
  }

  // set boot status


  return 0;
}

/*----------------------------------------------------------------------------*/
int
XrdMgmFstNode::SetStatusFileSystem(const char* key, XrdMgmFstFileSystem* filesystem, void *Arg)
{
  int* status = (int*) Arg;
  if (filesystem)
    filesystem->SetBootStatus(*status);

  return 0;
}

/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
