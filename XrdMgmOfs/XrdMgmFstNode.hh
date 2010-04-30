#ifndef __XRDMGMOFS_FSTNODES__HH__
#define __XRDMGMOFS_FSTNODES__HH__


/*----------------------------------------------------------------------------*/
#include "XrdMgmFstFileSystem.hh"
#include "XrdMqOfs/XrdMqMessage.hh"
/*----------------------------------------------------------------------------*/
#include "XrdOuc/XrdOucString.hh"
#include "XrdOuc/XrdOucHash.hh"
#include "XrdSys/XrdSysPthread.hh"
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/


class XrdMgmFstNode;

class XrdMgmFstNode {
  friend class XrdMgmProcCommand;
  friend class XrdMgmMessaging;
private:
  char infoString[1024];
  XrdOucString queueName;
  
  time_t lastHeartBeat; // time when last hb was sent
  int nodeStatus;       // status of the node

protected:

  XrdOucHash<XrdMgmFstFileSystem> fileSystems;

public:
  XrdOucString hostPortName;

  const char* GetNodeStatusString() {if (nodeStatus==kHeartBeatLow) return "lowhb"; if (nodeStatus==kOffline) return "offline"; if (nodeStatus==kOnline) return "online"; return "";}
  enum eNodeStatus { kHeartBeatLow=-1, kOffline=0, kOnline=1}; 

  static bool Update(XrdAdvisoryMqMessage* advmsg);
  static bool Update(XrdOucEnv &config);
  static bool Update(const char* infsname, int id, const char* schedgroup = "default", int bootstatus=XrdCommonFileSystem::kDown, int errc=0, const char* errmsg=0);

  bool SetNodeStatus(int status); 

  static XrdMgmFstNode* GetNode(const char* queue);
   
  XrdMgmFstNode(const char* queue){queueName = queue;};
  ~XrdMgmFstNode(){}

  unsigned int GetNumberOfFileSystems() {return fileSystems.Num();}

  const char* GetQueue() {return queueName.c_str();}
  time_t GetLastHeartBeat() {return lastHeartBeat;}

  static const char* GetInfoHeader() {static char infoHeader[1024];sprintf(infoHeader,"%-36s %-4s %-10s %-s\n","QUEUE","HBT","STATUS","#FS");return infoHeader;}
  const char* GetInfoString() {
    if (time(NULL)-GetLastHeartBeat()<10000) {
      sprintf(infoString,"%-36s %04d %-10s %02d\n", GetQueue(), time(NULL)-GetLastHeartBeat(), GetNodeStatusString(), GetNumberOfFileSystems()); return infoString;
    } else {
      sprintf(infoString,"%-36s ---- %-10s %02d\n", GetQueue(), GetNodeStatusString(), GetNumberOfFileSystems()); return infoString;}
  }
  static XrdOucHash<XrdMgmFstNode> gFstNodes;  
  static XrdSysMutex  gMutex;  // mutex to protect node hash access


  // to do listings
  static int                    ListNodes(const char* key, XrdMgmFstNode* node, void* Arg);
  static int                    ListFileSystems(const char* key, XrdMgmFstFileSystem* filesystem, void* Arg);
  
  // to check if an id is already in use
  static int                    ExistsNodeFileSystemId(const char* key, XrdMgmFstNode* node, void *Arg);
  static int                    ExistsFileSystemId(const char* key, XrdMgmFstFileSystem* filesystem, void *Arg);
  
  // to remove by id
  struct FindStruct {
  public:
    unsigned int id;
    XrdOucString nodename;
    XrdOucString fsname;
    bool found;
    
    FindStruct(unsigned int lid = 0, XrdOucString name = "") {id = lid; nodename="";fsname=name;found=false;}
    ~FindStruct() {}
  };

  // returns the node + filesystem names of an id
  static int                    FindNodeFileSystem(const char* key, XrdMgmFstNode* node, void *Arg);
  static int                    BootNode(const char* key, XrdMgmFstNode* node, void *Arg);

  static int                    FindFileSystem(const char* key, XrdMgmFstFileSystem* filesystem, void *Arg);
  static int                    BootFileSystem(const char* key, XrdMgmFstFileSystem* filesystem, void *Arg);
  static int                    SetStatusFileSystem(const char* key ,XrdMgmFstFileSystem* filesystem, void *Arg);
};

#endif

