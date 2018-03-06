// ----------------------------------------------------------------------
// File: Quota.cc
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

/*----------------------------------------------------------------------------*/
#include "mgm/Quota.hh"
#include "mgm/Policy.hh"
#include "mgm/XrdMgmOfs.hh"
#include "namespace/accounting/QuotaStats.hh"
/*----------------------------------------------------------------------------*/
#include <errno.h>
/*----------------------------------------------------------------------------*/

EOSMGMNAMESPACE_BEGIN

std::map<std::string, SpaceQuota*> Quota::gQuota;
std::map<eos::ContainerMD::id_t, SpaceQuota*> gMapInodeQuota;

eos::common::RWMutex Quota::gQuotaMutex;
gid_t Quota::gProjectId = 99;


#ifdef __APPLE__
#define ENONET 64
#endif

/*----------------------------------------------------------------------------*/
SpaceQuota::SpaceQuota (const char* name)
{
  SpaceName = name;
  LastCalculationTime = 0;
  LastEnableCheck = 0;
  QuotaNode = 0;
  PhysicalFreeBytes = PhysicalFreeFiles = PhysicalMaxBytes = PhysicalMaxFiles = 0;
  PhysicalTmpFreeBytes = PhysicalTmpFreeFiles = PhysicalTmpMaxBytes = PhysicalTmpMaxFiles = 0;
  LayoutSizeFactor = 1.0;
  On = false;

  eos::common::RWMutexWriteLock lock(gOFS->eosViewRWMutex);
  eos::ContainerMD *quotadir = 0;

  std::string path = name;

  if (path[0] == '/')
  {
    if (path[path.length() - 1] != '/')
    {
      path += "/";
    }
    SpaceName = path.c_str();

    try
    {
      quotadir = gOFS->eosView->getContainer(path.c_str());
    }
    catch (eos::MDException &e)
    {
      quotadir = 0;
    }
    if (!quotadir)
    {
      try
      {
        quotadir = gOFS->eosView->createContainer(name, true);
        quotadir->setMode(S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH | S_IFDIR);
        gOFS->eosView->updateContainerStore(quotadir);

      }
      catch (eos::MDException &e)
      {
        eos_static_crit("Cannot create quota directory %s", name);
      }
    }

    if (quotadir)
    {
      try
      {
        QuotaNode = gOFS->eosView->getQuotaNode(quotadir, false);
      }
      catch (eos::MDException &e)
      {
        QuotaNode = 0;
      }

      if (!QuotaNode)
      {
        try
        {
          QuotaNode = gOFS->eosView->registerQuotaNode(quotadir);
        }
        catch (eos::MDException &e)
        {
          QuotaNode = 0;
          eos_static_crit("Cannot register quota node %s", name);
        }
      }
    }
  }
  else
  {
    QuotaNode = 0;
  }
  DirtyTarget = true;
}

/*----------------------------------------------------------------------------*/
SpaceQuota::~SpaceQuota ()
{
}

/*----------------------------------------------------------------------------*/
bool
SpaceQuota::UpdateQuotaNodeAddress ()
{
  // this routine has to be called with eosViewMutexRW locked
  eos::ContainerMD *quotadir = 0;

  try
  {
    quotadir = gOFS->eosView->getContainer(SpaceName.c_str());
    try
    {
      QuotaNode = gOFS->eosView->getQuotaNode(quotadir, false);
    }
    catch (eos::MDException &e)
    {
      QuotaNode = 0;
      return false;
    }
  }
  catch (eos::MDException &e)
  {
    quotadir = 0;
    return false;
  }
  return true;
}

/*----------------------------------------------------------------------------*/
void
SpaceQuota::RemoveQuotaNode (XrdOucString &msg, int &retc)

{
  eos::common::RWMutexReadLock lock(gOFS->eosViewRWMutex);
  eos::ContainerMD *quotadir = 0;
  try
  {
    quotadir = gOFS->eosView->getContainer(SpaceName.c_str());
    gOFS->eosView->removeQuotaNode(quotadir);
    retc = 0;
    msg = "success: removed quota node ";
    msg += SpaceName.c_str();
  }
  catch (eos::MDException &e)
  {
    quotadir = 0;
    retc = e.getErrno();
    msg = e.getMessage().str().c_str();
  }
}

/*----------------------------------------------------------------------------*/
void
SpaceQuota::UpdateLogicalSizeFactor ()
{
  // ------------------A------------------------------------------------------------------------------------
  // ! this routine calculates the default factor for a quota node to calculate the logical available bytes
  // ------------------------------------------------------------------------------------------------------

  if (!SpaceName.beginswith("/"))
    return;

  XrdOucErrInfo error;
  eos::common::Mapping::VirtualIdentity vid;
  eos::common::Mapping::Root(vid);
  vid.sudoer = 1;
  eos::ContainerMD::XAttrMap map;

  int retc = gOFS->_attr_ls(SpaceName.c_str(),
                            error,
                            vid,
                            0,
                            map,
                            false);

  if (!retc)
  {
    unsigned long layoutId;
    XrdOucEnv env;
    unsigned long forcedfsid;
    long forcedgroup;
    XrdOucString spn = SpaceName;
    // get the layout in this quota node
    Policy::GetLayoutAndSpace(SpaceName.c_str(),
                              map,
                              vid,
                              layoutId,
                              spn,
                              env,
                              forcedfsid,
                              forcedgroup);
    LayoutSizeFactor = eos::common::LayoutId::GetSizeFactor(layoutId);
  }
  else
  {
    LayoutSizeFactor = 1.0;
  }
  // just a /0 protection 
  if (LayoutSizeFactor < 1.0)
    LayoutSizeFactor = 1.0;
}

/*----------------------------------------------------------------------------*/
bool
SpaceQuota::RmQuota (unsigned long tag, unsigned long id, bool lock)
{
  bool removed = false;
  if (lock) Mutex.Lock();
  if (Quota.count(Index(tag, id)))
    removed = true;
  Quota.erase(Index(tag, id));
  eos_static_debug("rm quota tag=%lu id=%lu", tag, id);
  if (lock) Mutex.UnLock();
  return removed;
  DirtyTarget = true;
}

/*----------------------------------------------------------------------------*/
long long
SpaceQuota::GetQuota (unsigned long tag, unsigned long id, bool lock)
{
  long long ret;
  if (lock) Mutex.Lock();
  ret = Quota[Index(tag, id)];
  eos_static_debug("get quota tag=%lu id=%lu value=%lld", tag, id, ret);
  if (lock) Mutex.UnLock();
  return (unsigned long long) ret;
}

/*----------------------------------------------------------------------------*/
void
SpaceQuota::SetQuota (unsigned long tag, unsigned long id, unsigned long long value, bool lock)
{
  if (lock) Mutex.Lock();
  eos_static_debug("set quota tag=%lu id=%lu value=%llu", tag, id, value);
  Quota[Index(tag, id)] = value;
  if (lock) Mutex.UnLock();

  if ( ( tag == kUserBytesTarget ) ||
      ( tag == kGroupBytesTarget ) ||
      ( tag == kUserFilesTarget ) ||
      ( tag == kGroupFilesTarget ) ||
      ( tag == kUserLogicalBytesTarget ) ||
      ( tag == kGroupLogicalBytesTarget ) )
    DirtyTarget = true;
}

/*----------------------------------------------------------------------------*/
void
SpaceQuota::AddQuota (unsigned long tag, unsigned long id, long long value, bool lock)
{
  if (lock) Mutex.Lock();
  eos_static_debug("add quota tag=%lu id=%lu value=%llu", tag, id, value);


  // user/group quota implementation
  // fix for avoiding negative numbers
  if ((((long long) Quota[Index(tag, id)]) + (long long) value) >= 0)
    Quota[Index(tag, id)] += value;

  eos_static_debug("sum quota tag=%lu id=%lu value=%llu", tag, id, Quota[Index(tag, id)]);
  if (lock) Mutex.UnLock();
}

/*----------------------------------------------------------------------------*/
void
SpaceQuota::UpdateTargetSums ()
{
  if (!DirtyTarget)
    return;

  Mutex.Lock();
  DirtyTarget = false;
  eos_static_debug("updating targets");
  ResetQuota(kAllUserBytesTarget, 0, false);
  ResetQuota(kAllUserFilesTarget, 0, false);
  ResetQuota(kAllGroupBytesTarget, 0, false);
  ResetQuota(kAllGroupFilesTarget, 0, false);
  ResetQuota(kAllUserLogicalBytesTarget, 0, false);
  ResetQuota(kAllGroupLogicalBytesTarget, 0, false);

  std::map<long long, unsigned long long>::const_iterator it;

  for (it = Begin(); it != End(); it++)
  {
    if ((UnIndex(it->first) == kUserBytesTarget))
    {
      AddQuota(kAllUserBytesTarget, 0, it->second, false);
      AddQuota(kAllUserLogicalBytesTarget, 0, it->second / LayoutSizeFactor, false);
    }

    if ((UnIndex(it->first) == kUserFilesTarget))
      AddQuota(kAllUserFilesTarget, 0, it->second, false);

    if ((UnIndex(it->first) == kGroupBytesTarget))
    {
      AddQuota(kAllGroupBytesTarget, 0, it->second, false);
      AddQuota(kAllGroupLogicalBytesTarget, 0, it->second / LayoutSizeFactor, false);
    }

    if ((UnIndex(it->first) == kGroupFilesTarget))
      AddQuota(kAllGroupFilesTarget, 0, it->second, false);
  }

  Mutex.UnLock();
}

/*----------------------------------------------------------------------------*/
void
SpaceQuota::UpdateIsSums ()
{
  Mutex.Lock();
  eos_static_debug("updating IS values");
  ResetQuota(kAllUserBytesIs, 0, false);
  ResetQuota(kAllUserLogicalBytesIs, 0, false);
  ResetQuota(kAllUserFilesIs, 0, false);
  ResetQuota(kAllGroupBytesIs, 0, false);
  ResetQuota(kAllGroupFilesIs, 0, false);
  ResetQuota(kAllGroupLogicalBytesIs, 0, false);

  ResetQuota(kAllUserBytesIs, 0, false);
  ResetQuota(kAllUserLogicalBytesIs, 0, false);
  ResetQuota(kAllUserFilesIs, 0, false);
  ResetQuota(kAllGroupBytesIs, 0, false);
  ResetQuota(kAllGroupFilesIs, 0, false);
  ResetQuota(kAllGroupLogicalBytesIs, 0, false);

  std::map<long long, unsigned long long>::const_iterator it;

  for (it = Begin(); it != End(); it++)
  {
    if ((UnIndex(it->first) == kUserBytesIs))
      AddQuota(kAllUserBytesIs, 0, it->second, false);
    if ((UnIndex(it->first) == kUserLogicalBytesIs))
      AddQuota(kAllUserLogicalBytesIs, 0, it->second, false);
    if ((UnIndex(it->first) == kUserFilesIs))
    {
      AddQuota(kAllUserFilesIs, 0, it->second, false);
    }
    if ((UnIndex(it->first) == kGroupBytesIs))
      AddQuota(kAllGroupBytesIs, 0, it->second, false);
    if ((UnIndex(it->first) == kGroupLogicalBytesIs))
      AddQuota(kAllGroupLogicalBytesIs, 0, it->second, false);
    if ((UnIndex(it->first) == kGroupFilesIs))
      AddQuota(kAllGroupFilesIs, 0, it->second, false);
  }

  Mutex.UnLock();
}

/*----------------------------------------------------------------------------*/
void
SpaceQuota::UpdateFromQuotaNode (uid_t uid, gid_t gid, bool calc_project_quota)
{
  Mutex.Lock();
  eos_static_debug("updating uid/gid values from quota node");
  if (QuotaNode)
  {
    ResetQuota(kUserBytesIs, uid, false);
    ResetQuota(kUserLogicalBytesIs, uid, false);
    ResetQuota(kUserFilesIs, uid, false);
    ResetQuota(kGroupBytesIs, gid, false);
    ResetQuota(kGroupFilesIs, gid, false);
    ResetQuota(kGroupLogicalBytesIs, gid, false);

    AddQuota(kUserBytesIs, uid, QuotaNode->getPhysicalSpaceByUser(uid), false);
    AddQuota(kUserLogicalBytesIs, uid, QuotaNode->getUsedSpaceByUser(uid), false);
    AddQuota(kUserFilesIs, uid, QuotaNode->getNumFilesByUser(uid), false);

    AddQuota(kGroupBytesIs, gid, QuotaNode->getPhysicalSpaceByGroup(gid), false);
    AddQuota(kGroupLogicalBytesIs, gid, QuotaNode->getUsedSpaceByGroup(gid), false);
    AddQuota(kGroupFilesIs, gid, QuotaNode->getNumFilesByGroup(gid), false);

    ResetQuota(kUserBytesIs, Quota::gProjectId, false);
    ResetQuota(kUserLogicalBytesIs, Quota::gProjectId, false);
    ResetQuota(kUserFilesIs, Quota::gProjectId, false);

    if (calc_project_quota)
    {

      // -----------------------------------------------------------------------
      // we recalculate the project quota only every 5 seconds to boost perf.
      // -----------------------------------------------------------------------

      static XrdSysMutex lMutex;
      static time_t lUpdateTime = 0;
      bool docalc = false;
      {
        XrdSysMutexHelper lock(lMutex);
        time_t now = time(NULL);
        if (lUpdateTime < now)
        {
          // next recalculation in 5 second
          docalc = true;
          lUpdateTime = now + 5;
        }
      }

      if (docalc)
      {
        ResetQuota(SpaceQuota::kGroupBytesIs, Quota::gProjectId, false);
        ResetQuota(SpaceQuota::kGroupFilesIs, Quota::gProjectId, false);
        ResetQuota(SpaceQuota::kGroupLogicalBytesIs, Quota::gProjectId, false);

        // loop over user and fill project quota
        for (auto itu = GetQuotaNode()->userUsageBegin();
             itu != GetQuotaNode()->userUsageEnd();
             itu++)
        {
          AddQuota(SpaceQuota::kGroupBytesIs,
                   Quota::gProjectId,
                   itu->second.physicalSpace,
                   false);
          AddQuota(SpaceQuota::kGroupLogicalBytesIs,
                   Quota::gProjectId,
                   itu->second.space,
                   false);
          AddQuota(SpaceQuota::kGroupFilesIs,
                   Quota::gProjectId,
                   itu->second.files,
                   false);
        }
      }
    }
  }
  Mutex.UnLock();
}

/*----------------------------------------------------------------------------*/
void
SpaceQuota::Refresh()
{
  eos::common::RWMutexReadLock nlock(gOFS->eosViewRWMutex);
  Quota::NodeToSpaceQuota(SpaceName.c_str());
  UpdateLogicalSizeFactor();
  UpdateIsSums();
  UpdateTargetSums();
}

/*----------------------------------------------------------------------------*/
void
SpaceQuota::PrintOut (XrdOucString &output, long uid_sel, long gid_sel, bool monitoring, bool translateids)
{
  char headerline[4096];
  eos_static_debug("called");

  std::map<long long, unsigned long long>::const_iterator it;

  int* sortuidarray = (int*) malloc(sizeof (int) * (Quota.size() + 1));
  int* sortgidarray = (int*) malloc(sizeof (int) * (Quota.size() + 1));

  int userentries = 0;
  int groupentries = 0;

  // make a map containing once all the defined uid's+gid's
  std::map<unsigned long, unsigned long > sortuidhash;
  std::map<unsigned long, unsigned long > sortgidhash;

  std::map<unsigned long, unsigned long >::const_iterator sortit;

  if (Quota[Index(kGroupBytesTarget, Quota::gProjectId)] > 0)
  {
    // for project space we just print the user/group entry gProjectId
    gid_sel = Quota::gProjectId;
  }

  if (!SpaceName.beginswith("/"))
  {
    free(sortuidarray);
    free(sortgidarray);
    // we don't show them right now ... maybe if we put quota on physical spaces we will
    return;

    if ((uid_sel < 0) && (gid_sel < 0))
    {
      XrdOucString value1 = "";
      XrdOucString value2 = "";
      XrdOucString value3 = "";
      XrdOucString value4 = "";

      XrdOucString percentage1 = "";
      XrdOucString percentage2 = "";

      // this is a virtual quota node
      if (!monitoring)
      {
        output += "# __________________________________________________________________________________________\n";
        sprintf(headerline, "# ==> Space     : %-16s\n", SpaceName.c_str());
        output += headerline;
        output += "# ------------------------------------------------------------------------------------------\n";
        output += "# ==> Physical\n";
        sprintf(headerline, "     %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s \n", GetTagName(kGroupBytesIs), GetTagName(kGroupFilesIs), GetTagName(kGroupBytesTarget), GetTagName(kGroupFilesTarget), "volume[%]", "status-vol", "inodes[%]", "status-ino");
        output += headerline;
        sprintf(headerline, "PHYS %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s\n",
                eos::common::StringConversion::GetReadableSizeString(value1, PhysicalMaxBytes - PhysicalFreeBytes, "B"),
                eos::common::StringConversion::GetReadableSizeString(value2, PhysicalMaxFiles - PhysicalFreeFiles, "-"),
                eos::common::StringConversion::GetReadableSizeString(value3, PhysicalMaxBytes, "B"),
                eos::common::StringConversion::GetReadableSizeString(value4, PhysicalMaxFiles, "-"),
                GetQuotaPercentage(PhysicalMaxBytes - PhysicalFreeBytes, PhysicalMaxBytes, percentage1),
                GetQuotaStatus(PhysicalMaxBytes - PhysicalFreeBytes, PhysicalMaxBytes),
                GetQuotaPercentage(PhysicalMaxFiles - PhysicalFreeFiles, PhysicalMaxFiles, percentage2),
                GetQuotaStatus(PhysicalMaxFiles - PhysicalFreeFiles, PhysicalMaxFiles));
        output += headerline;
        output += "# ..........................................................................................\n";
      }
      else
      {
        sprintf(headerline, "quota=phys space=%s usedbytes=%llu usedfiles=%llu maxbytes=%llu maxfiles=%llu percentageusedbytes=%s statusbytes=%s percentageusedfiles=%s statusfiles=%s\n", SpaceName.c_str(),
                PhysicalMaxBytes - PhysicalFreeBytes,
                PhysicalMaxFiles - PhysicalFreeFiles,
                PhysicalMaxBytes,
                PhysicalMaxFiles,
                GetQuotaPercentage(PhysicalMaxBytes - PhysicalFreeBytes, PhysicalMaxBytes, percentage1),
                GetQuotaStatus(PhysicalMaxBytes - PhysicalFreeBytes, PhysicalMaxBytes),
                GetQuotaPercentage(PhysicalMaxFiles - PhysicalFreeFiles, PhysicalMaxFiles, percentage2),
                GetQuotaStatus(PhysicalMaxFiles - PhysicalFreeFiles, PhysicalMaxFiles));
        output += headerline;
      }
    }
  }
  else
  {
    XrdOucString header;
    // this is a virtual quota node
    if (!monitoring)
    {
      header += "# _______________________________________________________________________________________________\n";
      sprintf(headerline, "# ==> Quota Node: %-16s\n", SpaceName.c_str());
      header += headerline;
      header += "# _______________________________________________________________________________________________\n";
    }
    for (it = Begin(); it != End(); it++)
    {
      if ((UnIndex(it->first) >= kUserBytesIs) && (UnIndex(it->first) <= kUserFilesTarget))
      {
        eos_static_debug("adding %llx to print list ", UnIndex(it->first));
        unsigned long ugid = (it->first)&0xffffffff;
        // uid selection filter
        if (uid_sel >= 0)
          if (ugid != (unsigned long) uid_sel)
            continue;

        // we don't print the users if a gid is selected
        if (gid_sel >= 0)
          continue;

        sortuidhash[ugid] = ugid;
      }

      if ((UnIndex(it->first) >= kGroupBytesIs) && (UnIndex(it->first) <= kGroupFilesTarget))
      {
        unsigned long ugid = (it->first)&0xffffffff;
        // uid selection filter
        if (gid_sel >= 0)
          if (ugid != (unsigned long) gid_sel)
            continue;
        // we don't print the group if a uid is selected
        if (uid_sel >= 0)
          continue;
        sortgidhash[ugid] = ugid;
      }
    }

    for (sortit = sortuidhash.begin(); sortit != sortuidhash.end(); sortit++)
    {
      sortuidarray[userentries] = (sortit->first);
      eos_static_debug("loop %d %d", userentries, sortuidarray[userentries]);
      userentries++;
    }

    for (sortit = sortgidhash.begin(); sortit != sortgidhash.end(); sortit++)
    {
      // sort only based on the user bytes entries
      sortgidarray[groupentries] = (sortit->first);
      eos_static_debug("loop %d %d", groupentries, sortgidarray[groupentries]);
      groupentries++;
    }

    sort(sortuidarray, sortuidarray + userentries);
    sort(sortgidarray, sortgidarray + groupentries);

    eos_static_debug("sorted");
    for (int k = 0; k < userentries; k++)
    {
      eos_static_debug("sort %d %d", k, sortuidarray[k]);
    }

    for (int k = 0; k < groupentries; k++)
    {
      eos_static_debug("sort %d %d", k, sortgidarray[k]);
    }

    std::vector <std::string> uidout;
    std::vector <std::string> gidout;

    if (((uid_sel < 0) && (gid_sel < 0)) || userentries || groupentries)
    {
      // - we print the header for selected uid/gid's only if there is something to print
      // - if we have a full listing we print even empty quota nodes (=header only) 
      output += header;
    }

    if (userentries)
    {
      // user loop
      if (!monitoring)
      {
        sprintf(headerline, "%-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s\n", GetTagCategory(kUserBytesIs), GetTagName(kUserBytesIs), GetTagName(kUserLogicalBytesIs), GetTagName(kUserFilesIs), GetTagName(kUserBytesTarget), GetTagName(kUserLogicalBytesTarget), GetTagName(kUserFilesTarget), "filled[%]", "vol-status", "ino-status");
        output += headerline;
      }
    }

    for (int lid = 0; lid < userentries; lid++)
    {
      eos_static_debug("loop with id=%d", lid);
      XrdOucString value1 = "";
      XrdOucString value2 = "";
      XrdOucString value3 = "";
      XrdOucString value4 = "";
      XrdOucString value5 = "";
      XrdOucString value6 = "";

      XrdOucString id = "";
      id += sortuidarray[lid];

      if (translateids)
      {
        if (gid_sel == Quota::gProjectId)
        {
          id = "project";
        }
        else
        {
          int errc=0;
          std::string username = eos::common::Mapping::UidToUserName (sortuidarray[lid], errc);
          char uidlimit[16];
          if (username.length())
          {
            snprintf(uidlimit, 11, "%s", username.c_str());
            id = uidlimit;
          }
        }
      }

      XrdOucString percentage = "";
      if (!monitoring)
      {
        sprintf(headerline, "%-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s\n", id.c_str(),
                eos::common::StringConversion::GetReadableSizeString(value1, GetQuota(kUserBytesIs, sortuidarray[lid]), "B"),
                eos::common::StringConversion::GetReadableSizeString(value2, GetQuota(kUserLogicalBytesIs, sortuidarray[lid]), "B"),
                eos::common::StringConversion::GetReadableSizeString(value3, GetQuota(kUserFilesIs, sortuidarray[lid]), "-"),
                eos::common::StringConversion::GetReadableSizeString(value4, GetQuota(kUserBytesTarget, sortuidarray[lid]), "B"),
                eos::common::StringConversion::GetReadableSizeString(value5, (long long) (GetQuota(kUserBytesTarget, sortuidarray[lid]) / LayoutSizeFactor), "B"),
                eos::common::StringConversion::GetReadableSizeString(value6, GetQuota(kUserFilesTarget, sortuidarray[lid]), "-"),
                GetQuotaPercentage(GetQuota(kUserBytesIs, sortuidarray[lid]), GetQuota(kUserBytesTarget, sortuidarray[lid]), percentage),
                GetQuotaStatus(GetQuota(kUserBytesIs, sortuidarray[lid]), GetQuota(kUserBytesTarget, sortuidarray[lid])),
                GetQuotaStatus(GetQuota(kUserFilesIs, sortuidarray[lid]), GetQuota(kUserFilesTarget, sortuidarray[lid])));
      }
      else
      {
        sprintf(headerline, "quota=node uid=%s space=%s usedbytes=%llu usedlogicalbytes=%llu usedfiles=%llu maxbytes=%llu maxlogicalbytes=%llu maxfiles=%llu percentageusedbytes=%s statusbytes=%s statusfiles=%s\n", id.c_str(), SpaceName.c_str(),
                GetQuota(kUserBytesIs, sortuidarray[lid]),
                GetQuota(kUserLogicalBytesIs, sortuidarray[lid]),
                GetQuota(kUserFilesIs, sortuidarray[lid]),
                GetQuota(kUserBytesTarget, sortuidarray[lid]),
                (unsigned long long) (GetQuota(kUserBytesTarget, sortuidarray[lid]) / LayoutSizeFactor),
                GetQuota(kUserFilesTarget, sortuidarray[lid]),
                GetQuotaPercentage(GetQuota(kUserBytesIs, sortuidarray[lid]), GetQuota(kUserBytesTarget, sortuidarray[lid]), percentage),
                GetQuotaStatus(GetQuota(kUserBytesIs, sortuidarray[lid]), GetQuota(kUserBytesTarget, sortuidarray[lid])),
                GetQuotaStatus(GetQuota(kUserFilesIs, sortuidarray[lid]), GetQuota(kUserFilesTarget, sortuidarray[lid])));
      }

      if (!translateids)
      {
        output += headerline;
      }
      else
      {
        uidout.push_back(headerline);
      }
    }

    if (translateids)
    {
      std::sort(uidout.begin(), uidout.end());
      for (size_t i = 0; i < uidout.size(); i++)
      {
        output += uidout[i].c_str();
      }
    }

    if (groupentries)
    {
      // group loop
      if (!monitoring)
      {
        output += "# ...............................................................................................\n";
        sprintf(headerline, "%-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s\n", GetTagCategory(kGroupBytesIs), GetTagName(kGroupBytesIs), GetTagName(kGroupLogicalBytesIs), GetTagName(kGroupFilesIs), GetTagName(kGroupBytesTarget), GetTagName(kGroupLogicalBytesTarget), GetTagName(kGroupFilesTarget), "filled[%]", "vol-status", "ino-status");
        output += headerline;
      }
    }

    for (int lid = 0; lid < groupentries; lid++)
    {
      eos_static_debug("loop with id=%d", lid);
      XrdOucString value1 = "";
      XrdOucString value2 = "";
      XrdOucString value3 = "";
      XrdOucString value4 = "";
      XrdOucString value5 = "";
      XrdOucString value6 = "";

      XrdOucString id = "";
      id += sortgidarray[lid];

      if (translateids)
      {
        if (gid_sel == Quota::gProjectId)
        {
          id = "project";
        }
        else
        {
          int errc=0;
          std::string groupname = eos::common::Mapping::GidToGroupName (sortgidarray[lid], errc);
          char gidlimit[16];
          if (groupname.length())
          {
            snprintf(gidlimit, 11, "%s", groupname.c_str());
            id = gidlimit;
          }
        }
      }

      XrdOucString percentage = "";
      if (!monitoring)
      {
        sprintf(headerline, "%-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s\n", id.c_str(),
                eos::common::StringConversion::GetReadableSizeString(value1, GetQuota(kGroupBytesIs, sortgidarray[lid]), "B"),
                eos::common::StringConversion::GetReadableSizeString(value2, GetQuota(kGroupLogicalBytesIs, sortgidarray[lid]), "B"),
                eos::common::StringConversion::GetReadableSizeString(value3, GetQuota(kGroupFilesIs, sortgidarray[lid]), "-"),
                eos::common::StringConversion::GetReadableSizeString(value4, GetQuota(kGroupBytesTarget, sortgidarray[lid]), "B"),
                eos::common::StringConversion::GetReadableSizeString(value5, (long long) (GetQuota(kGroupBytesTarget, sortgidarray[lid]) / LayoutSizeFactor), "B"),
                eos::common::StringConversion::GetReadableSizeString(value6, GetQuota(kGroupFilesTarget, sortgidarray[lid]), "-"),
                GetQuotaPercentage(GetQuota(kGroupBytesIs, sortgidarray[lid]), GetQuota(kGroupBytesTarget, sortgidarray[lid]), percentage),
                GetQuotaStatus(GetQuota(kGroupBytesIs, sortgidarray[lid]), GetQuota(kGroupBytesTarget, sortgidarray[lid])),
                GetQuotaStatus(GetQuota(kGroupFilesIs, sortgidarray[lid]), GetQuota(kGroupFilesTarget, sortgidarray[lid])));
      }
      else
      {
        sprintf(headerline, "quota=node gid=%s space=%s usedbytes=%llu usedlogicalbytes=%llu usedfiles=%llu maxbytes=%llu maxlogicalbytes=%llu maxfiles=%llu percentageusedbytes=%s statusbytes=%s statusfiles=%s\n", id.c_str(), SpaceName.c_str(),
                GetQuota(kGroupBytesIs, sortgidarray[lid]),
                GetQuota(kGroupLogicalBytesIs, sortgidarray[lid]),
                GetQuota(kGroupFilesIs, sortgidarray[lid]),
                GetQuota(kGroupBytesTarget, sortgidarray[lid]),
                (unsigned long long) (GetQuota(kGroupBytesTarget, sortgidarray[lid]) / LayoutSizeFactor),
                GetQuota(kGroupFilesTarget, sortgidarray[lid]),
                GetQuotaPercentage(GetQuota(kGroupBytesIs, sortgidarray[lid]), GetQuota(kGroupBytesTarget, sortgidarray[lid]), percentage),
                GetQuotaStatus(GetQuota(kGroupBytesIs, sortgidarray[lid]), GetQuota(kGroupBytesTarget, sortgidarray[lid])),
                GetQuotaStatus(GetQuota(kGroupFilesIs, sortgidarray[lid]), GetQuota(kGroupFilesTarget, sortgidarray[lid])));
      }
      if (!translateids)
      {
        output += headerline;
      }
      else
      {
        gidout.push_back(headerline);
      }
    }

    if (translateids)
    {
      std::sort(gidout.begin(), gidout.end());
      for (size_t i = 0; i < gidout.size(); i++)
      {
        output += gidout[i].c_str();
      }
    }

    if ((uid_sel < 0) && (gid_sel < 0))
    {
      if (!monitoring)
      {
        output += "# ----------------------------------------------------------------------------------------------------------\n";
        output += "# ==> Summary\n";
      }

      XrdOucString value1 = "";
      XrdOucString value2 = "";
      XrdOucString value3 = "";
      XrdOucString value4 = "";
      XrdOucString value5 = "";
      XrdOucString value6 = "";

      XrdOucString id = "ALL";
      XrdOucString percentage = "";

      if (!monitoring)
      {
        sprintf(headerline, "%-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s\n", GetTagCategory(kAllUserBytesIs), GetTagName(kAllUserBytesIs), GetTagName(kAllUserLogicalBytesIs), GetTagName(kAllUserFilesIs), GetTagName(kAllUserBytesTarget), GetTagName(kAllUserLogicalBytesTarget), GetTagName(kAllUserFilesTarget), "filled[%]", "vol-status", "ino-status");
        output += headerline;
        sprintf(headerline, "%-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s\n", id.c_str(),
                eos::common::StringConversion::GetReadableSizeString(value1, GetQuota(kAllUserBytesIs, 0), "B"),
                eos::common::StringConversion::GetReadableSizeString(value2, GetQuota(kAllUserLogicalBytesIs, 0), "B"),
                eos::common::StringConversion::GetReadableSizeString(value3, GetQuota(kAllUserFilesIs, 0), "-"),
                eos::common::StringConversion::GetReadableSizeString(value4, GetQuota(kAllUserBytesTarget, 0), "B"),
                eos::common::StringConversion::GetReadableSizeString(value5, GetQuota(kAllUserLogicalBytesTarget, 0), "B"),
                eos::common::StringConversion::GetReadableSizeString(value6, GetQuota(kAllUserFilesTarget, 0), "-"),
                GetQuotaPercentage(GetQuota(kAllUserBytesIs, 0), GetQuota(kAllUserBytesTarget, 0), percentage),
                GetQuotaStatus(GetQuota(kAllUserBytesIs, 0), GetQuota(kAllUserBytesTarget, 0)),
                GetQuotaStatus(GetQuota(kAllUserFilesIs, 0), GetQuota(kAllUserFilesTarget, 0)));
      }
      else
      {
        sprintf(headerline, "quota=node uid=%s space=%s usedbytes=%llu usedlogicalbytes=%llu usedfiles=%llu maxbytes=%llu maxlogicalbytes=%llu maxfiles=%llu percentageusedbytes=%s statusbytes=%s statusfiles=%s\n", id.c_str(), SpaceName.c_str(),
                GetQuota(kAllUserBytesIs, 0),
                GetQuota(kAllUserLogicalBytesIs, 0),
                GetQuota(kAllUserFilesIs, 0),
                GetQuota(kAllUserBytesTarget, 0),
                GetQuota(kAllUserLogicalBytesTarget, 0),
                GetQuota(kAllUserFilesTarget, 0),
                GetQuotaPercentage(GetQuota(kAllUserBytesIs, 0), GetQuota(kAllUserBytesTarget, 0), percentage),
                GetQuotaStatus(GetQuota(kAllUserBytesIs, 0), GetQuota(kAllUserBytesTarget, 0)),
                GetQuotaStatus(GetQuota(kAllUserFilesIs, 0), GetQuota(kAllUserFilesTarget, 0)));
      }
      output += headerline;

      if (!monitoring)
      {
        sprintf(headerline, "%-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s\n", GetTagCategory(kAllGroupBytesIs), GetTagName(kAllGroupBytesIs), GetTagName(kAllGroupLogicalBytesIs), GetTagName(kAllGroupFilesIs), GetTagName(kAllGroupBytesTarget), GetTagName(kAllGroupLogicalBytesTarget), GetTagName(kAllGroupFilesTarget), "filled[%]", "vol-status", "ino-status");
        output += headerline;
        sprintf(headerline, "%-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s\n", id.c_str(),
                eos::common::StringConversion::GetReadableSizeString(value1, GetQuota(kAllGroupBytesIs, 0), "B"),
                eos::common::StringConversion::GetReadableSizeString(value2, GetQuota(kAllGroupLogicalBytesIs, 0), "B"),
                eos::common::StringConversion::GetReadableSizeString(value3, GetQuota(kAllGroupFilesIs, 0), "-"),
                eos::common::StringConversion::GetReadableSizeString(value4, GetQuota(kAllGroupBytesTarget, 0), "B"),
                eos::common::StringConversion::GetReadableSizeString(value5, GetQuota(kAllGroupLogicalBytesTarget, 0), "B"),
                eos::common::StringConversion::GetReadableSizeString(value6, GetQuota(kAllGroupFilesTarget, 0), "-"),
                GetQuotaPercentage(GetQuota(kAllGroupBytesIs, 0), GetQuota(kAllGroupBytesTarget, 0), percentage),
                GetQuotaStatus(GetQuota(kAllGroupBytesIs, 0), GetQuota(kAllGroupBytesTarget, 0)),
                GetQuotaStatus(GetQuota(kAllGroupFilesIs, 0), GetQuota(kAllGroupFilesTarget, 0)));
      }
      else
      {
        sprintf(headerline, "quota=node gid=%s space=%s usedbytes=%llu usedlogicalbytes=%llu usedfiles=%llu maxbytes=%llu maxlogicalbytes=%llu maxfiles=%llu percentageusedbytes=%s statusbytes=%s statusfiles=%s\n", id.c_str(), SpaceName.c_str(),
                GetQuota(kAllGroupBytesIs, 0),
                GetQuota(kAllGroupLogicalBytesIs, 0),
                GetQuota(kAllGroupFilesIs, 0),
                GetQuota(kAllGroupBytesTarget, 0),
                GetQuota(kAllGroupLogicalBytesTarget, 0),
                GetQuota(kAllGroupFilesTarget, 0),
                GetQuotaPercentage(GetQuota(kAllGroupBytesIs, 0), GetQuota(kAllGroupBytesTarget, 0), percentage),
                GetQuotaStatus(GetQuota(kAllGroupBytesIs, 0), GetQuota(kAllGroupBytesTarget, 0)),
                GetQuotaStatus(GetQuota(kAllGroupFilesIs, 0), GetQuota(kAllGroupFilesTarget, 0)));
      }
      output += headerline;
    }
  }
  free(sortuidarray);
  free(sortgidarray);
}

bool
SpaceQuota::CheckWriteQuota (uid_t uid, gid_t gid, long long desiredspace, unsigned int inodes)
{
  // ----------------------------------------------------------------------------
  // Straigh-forward user/group quota checks
  // If user & group quota is defined, both have to be fullfilled
  // ----------------------------------------------------------------------------

  bool hasquota = false;

  // copy info from namespace Quota Node ...
  UpdateFromQuotaNode(uid, gid, GetQuota(kGroupBytesTarget, Quota::gProjectId, false) ? true : false); // get user/group and if defined project quota

  eos_static_info("uid=%d gid=%d size=%llu quota=%llu", uid, gid, desiredspace, GetQuota(kUserBytesTarget, uid, false));

  bool userquota = false;
  bool groupquota = false;
  bool projectquota = false;
  bool hasuserquota = false;
  bool hasgroupquota = false;
  bool hasprojectquota = false;

  bool uservolumequota = false;
  bool userinodequota = false;
  bool groupvolumequota = false;
  bool groupinodequota = false;

  if (GetQuota(kUserBytesTarget, uid, false) > 0)
  {
    userquota = true;
    uservolumequota = true;
  }

  if (GetQuota(kGroupBytesTarget, gid, false) > 0)
  {
    groupquota = true;
    groupvolumequota = true;
  }

  if (GetQuota(kUserFilesTarget, uid, false) > 0)
  {
    userquota = true;
    userinodequota = true;
  }

  if (GetQuota(kGroupFilesTarget, gid, false) > 0)
  {
    groupquota = true;
    groupinodequota = true;
  }

  if (uservolumequota)
  {
    if (((GetQuota(kUserBytesTarget, uid, false)) - (GetQuota(kUserBytesIs, uid, false))) > (long long) (desiredspace))
    {
      hasuserquota = true;
    }
    else
    {
      hasuserquota = false;
    }
  }

  if (userinodequota)
  {
    if (((GetQuota(kUserFilesTarget, uid, false)) - (GetQuota(kUserFilesIs, uid, false))) > (inodes))
    {
      if ( !uservolumequota )
        hasuserquota = true;
    }
    else
    {
      hasuserquota = false;
    }
  }

  if (groupvolumequota)
  {
    if (((GetQuota(kGroupBytesTarget, gid, false)) - (GetQuota(kGroupBytesIs, gid, false))) > (long long) (desiredspace))
    {
      hasgroupquota = true;
    }
    else
    {
      hasgroupquota = false;
    }
  }

  if (groupinodequota)
  {
    if ((((GetQuota(kGroupFilesTarget, gid, false)) - (GetQuota(kGroupFilesIs, gid, false))) > (inodes)))
    {
      if ( !groupvolumequota )
        hasgroupquota = true;
    }
    else
    {
      hasgroupquota = false;
    }
  }

  if ((((GetQuota(kGroupBytesTarget, Quota::gProjectId, false)) - (GetQuota(kGroupBytesIs, Quota::gProjectId, false))) > (long long) (desiredspace)) &&
      (((GetQuota(kGroupFilesTarget, Quota::gProjectId, false)) - (GetQuota(kGroupFilesIs, Quota::gProjectId, false))) > (inodes)))
  {
    hasprojectquota = true;
  }

  if (!userquota && !groupquota)
    projectquota = true;

  eos_static_info("userquota=%d groupquota=%d userquota=%d groupquota=%d userinodequota=%d uservolumequota=%d projectquota=%d hasprojectquota=%d\n", userquota, groupquota, hasuserquota, hasgroupquota, userinodequota, uservolumequota, projectquota, hasprojectquota);
  if ((userquota) && (groupquota))
  {
    // both are defined, we need to have both
    hasquota = hasuserquota & hasgroupquota;
  }
  else
  {
    hasquota = hasuserquota || hasgroupquota;
  }

  if (projectquota && hasprojectquota)
  {
    hasquota = true;
  }

  if ((uid == 0))
  {
    // root does not need any quota
    hasquota = true;
  }

  return hasquota;
}

/* ------------------------------------------------------------------------- */
int
SpaceQuota::FilePlacement (const char* path, //< path to place
                           eos::common::Mapping::VirtualIdentity_t &vid, //< virtual id of client
                           const char* grouptag, //< group tag for placement
                           unsigned long lid, //< layout to be placed
                           std::vector<unsigned int> &avoid_filesystems, //< filesystems to avoid
                           std::vector<unsigned int> &selected_filesystems, //< return filesystems selected by scheduler
                           bool truncate, //< indicates placement with truncation
                           int forced_scheduling_group_index, //< forced index for the scheduling subgroup to be used 
                           unsigned long long bookingsize //< size to book for the placement
                           )
{
  //! -------------------------------------------------------------
  //! the write placement routine checking for quota and calling the scheduler
  //! ------------------------------------------------------------- 

  unsigned int nfilesystems = eos::common::LayoutId::GetStripeNumber(lid) + 1; // 0 = 1 replica !
  bool hasquota = false;
  uid_t uid = vid.uid;
  gid_t gid = vid.gid;

  // first figure out how many filesystems we need
  eos_static_debug("uid=%u gid=%u grouptag=%s place filesystems=%u", uid, gid, grouptag, nfilesystems);

  // check if the uid/gid has enough quota configured to place in this space !

  if (Enabled())
  {
    // we have physical spacequota and namespace spacequota
    SpaceQuota* namespacequota = Quota::GetResponsibleSpaceQuota(path);
    if (namespacequota)
    {
      hasquota = namespacequota->CheckWriteQuota(uid, gid, 1ll * nfilesystems * bookingsize, nfilesystems);
      if (!hasquota)
      {
        eos_static_debug("uid=%u gid=%u grouptag=%s place filesystems=%u has no quota left!", uid, gid, grouptag, nfilesystems);
        return EDQUOT;
      }
    }
    else
    {
      eos_static_err("no namespace quota found for path=%s", path);
      return EDQUOT;
    }
  }
  else
  {
    eos_static_debug("quota is disabled in space=%s", GetSpaceName());
  }

  std::string spacename = SpaceName.c_str();

  if (!FsView::gFsView.mSpaceGroupView.count(spacename))
  {
    eos_static_err("msg=\"no filesystem in space\" space=\"%s\"", spacename.c_str());
    // there is no filesystem in that space
    selected_filesystems.clear();
    return ENOSPC;
  }

  // call the scheduler implementation now
  return Scheduler::FilePlacement(path, vid, grouptag, lid, avoid_filesystems, selected_filesystems, truncate, forced_scheduling_group_index, bookingsize);
}

/*----------------------------------------------------------------------------*/
SpaceQuota *
Quota::GetSpaceQuota (const char* name, bool nocreate)
{
  // the caller has to Readlock gQuotaMutex
  SpaceQuota* spacequota = 0;
  std::string sname = name;

  if (sname[0] == '/')
  {
    // allow sloppy guys to skip the trailing '/'
    if (sname[sname.length() - 1] != '/')
    {
      sname += "/";
    }
  }

  if ((gQuota.count(sname)) && (spacequota = gQuota[sname]))
  {

  }
  else
  {
    if (nocreate)
    {
      return NULL;
    }
    do
    {
      // this is a dangerous way if any other mutex was used from the caller after gQuotaMutex.UnLockRead() => take care not do to that!
      gQuotaMutex.UnLockRead();
      spacequota = new SpaceQuota(sname.c_str());
      gQuotaMutex.LockWrite();
      gQuota[sname] = spacequota;
      gMapInodeQuota[spacequota->GetQuotaNode()->getId()] = spacequota;
      gQuotaMutex.UnLockWrite();
      gQuotaMutex.LockRead();
    }
    while ((!gQuota.count(sname) && (!(spacequota = gQuota[sname]))));
  }

  return spacequota;
}

/*----------------------------------------------------------------------------*/
SpaceQuota*
Quota::GetSpaceQuota(const eos::ContainerMD::id_t qino)
{
  if (gMapInodeQuota.count(qino)) 
  {
    return gMapInodeQuota[qino];
  }
  else 
  {
    return static_cast<SpaceQuota*>(0);
  }
}

/*----------------------------------------------------------------------------*/
SpaceQuota *
Quota::GetResponsibleSpaceQuota (const char* path)
{
  // the caller has to Readlock gQuotaMutex
  SpaceQuota* spacequota = 0;
  XrdOucString matchpath = path;
  std::map<std::string, SpaceQuota*>::const_iterator it;
  for (it = gQuota.begin(); it != gQuota.end(); it++)
  {
    if (matchpath.beginswith(it->second->GetSpaceName()))
    {
      if ((!spacequota) || ((strlen(it->second->GetSpaceName()) > strlen(spacequota->GetSpaceName()))))
      {
        spacequota = it->second;
      }
    }
  }
  return spacequota;
}

/*----------------------------------------------------------------------------*/
int
Quota::GetSpaceNameList (const char* key, SpaceQuota* spacequota, void *Arg)
{
  XrdOucString* spacestring = (XrdOucString*) Arg;
  (*spacestring) += spacequota->GetSpaceName();
  (*spacestring) += ",";
  return 0;
}

int
Quota::QuotaByPath(const char* myspace,
                   const char* path,
                   uid_t uid,
                   gid_t gid,
                   long long& avail_files,
                   long long& avail_bytes,
                   eos::ContainerMD::id_t& quota_inode)
{
  eos::common::RWMutexReadLock lock(Quota::gQuotaMutex);

  SpaceQuota* qspace = GetSpaceQuota(myspace, false);
  if (!qspace)
    return -1;

  if (!qspace->Enabled())
  {
    // we grant infinite resources since quota is disabled
    avail_files = std::numeric_limits<long>::max()/2;
    avail_bytes = std::numeric_limits<long>::max()/2;
    return 0;
  }
  
  SpaceQuota* space = GetResponsibleSpaceQuota(path);
  if (space)
  {
    long long maxbytes_user, maxbytes_group, maxbytes_project;
    long long freebytes_user, freebytes_group, freebytes_project;
    long long freebytes = 0 ;
    long long maxbytes = 0;
    freebytes_user = freebytes_group = freebytes_project = 0;
    maxbytes_user = maxbytes_group = maxbytes_project = 0;
    space->Refresh();
    maxbytes_user  = space->GetQuota(SpaceQuota::kUserBytesTarget, uid);
    maxbytes_group = space->GetQuota(SpaceQuota::kGroupBytesTarget, gid);
    maxbytes_project = space->GetQuota(SpaceQuota::kGroupBytesTarget, Quota::gProjectId);
    freebytes_user = maxbytes_user - space->GetQuota(SpaceQuota::kUserLogicalBytesIs, uid);
    freebytes_group = maxbytes_group - space->GetQuota(SpaceQuota::kGroupLogicalBytesIs, gid);
    freebytes_project = maxbytes_project - space->GetQuota(SpaceQuota::kGroupLogicalBytesIs, Quota::gProjectId);

    if (freebytes_user > freebytes)
      freebytes = freebytes_user;
    if (freebytes_group > freebytes)
      freebytes = freebytes_group;
    if (freebytes_project > freebytes)
      freebytes = freebytes_project;
    if (maxbytes_user > maxbytes)
      maxbytes = maxbytes_user;
    if (maxbytes_group > maxbytes)
      maxbytes = maxbytes_group;
    if (maxbytes_project > maxbytes)
      maxbytes = maxbytes_project;
    
    long long maxfiles_user, maxfiles_group, maxfiles_project;
    long long freefiles_user, freefiles_group, freefiles_project;
    long long freefiles = 0;
    long long maxfiles = 0;
    freefiles_user = freefiles_group = freefiles_project = 0;
    maxfiles_user = maxfiles_group = maxfiles_project = 0;
    maxfiles_user  = space->GetQuota(SpaceQuota::kUserFilesTarget, uid);
    maxfiles_group = space->GetQuota(SpaceQuota::kGroupFilesTarget, gid);
    maxfiles_project = space->GetQuota(SpaceQuota::kGroupFilesTarget, Quota::gProjectId);
    freefiles_user = maxfiles_user - space->GetQuota(SpaceQuota::kUserFilesIs, uid);
    freefiles_group = maxfiles_group - space->GetQuota(SpaceQuota::kGroupFilesIs, gid);
    freefiles_project = maxfiles_project - space->GetQuota(SpaceQuota::kGroupFilesIs, Quota::gProjectId);

    if (freefiles_user > freefiles)
      freefiles = freefiles_user;
    if (freefiles_group > freefiles)
      freefiles = freefiles_group;
    if (freefiles_project > freefiles)
      freefiles = freefiles_project;
    if (maxfiles_user > maxfiles)
      maxfiles = maxfiles_user;
    if (maxfiles_group > maxfiles)
      maxfiles = maxfiles_group;
    if (maxfiles_project > maxfiles)
      maxfiles = maxfiles_project;
    
    avail_files = freefiles;
    avail_bytes = freebytes;
    
    quota_inode = space->GetQuotaNode()->getId();
    return 0;
  }
  return -1;
}

/*----------------------------------------------------------------------------*/
void
Quota::GetIndividualQuota (eos::common::Mapping::VirtualIdentity_t &vid, const char* path, long long &maxbytes, long long &freebytes)
{
  eos::common::Mapping::VirtualIdentity_t m_vid = vid;
  {
    // check for sys.auth='*'

    XrdOucString xownerauth;
    XrdOucErrInfo error;

    struct stat buf;


    if (!gOFS->_stat(path, &buf, error, vid, ""))
    {
      gOFS->_attr_get(path, error, vid, "", "sys.owner.auth", xownerauth);

      std::string ownerauth = xownerauth.c_str();


      if (ownerauth.length())
      {
        if (ownerauth == "*")
        {
          eos_static_info("msg=\"client authenticated as directory owner\" path=\"%s\"uid=\"%u=>%u\" gid=\"%u=>%u\"", path, vid.uid, vid.gid, buf.st_uid, buf.st_gid);
          // yes the client can operate as the owner, we rewrite the virtual id
          m_vid.uid = buf.st_uid;
          m_vid.gid = buf.st_gid;
        }
        else
        {
          ownerauth += ",";
          std::string ownerkey = vid.prot.c_str();
          ownerkey += ":";
          if (vid.prot == "gsi")
          {
            ownerkey += vid.dn.c_str();
          }
          else
          {
            ownerkey += vid.uid_string.c_str();
          }
          if ((ownerauth.find(ownerkey)) != std::string::npos)
          {
            eos_static_info("msg=\"client authenticated as directory owner\" path=\"%s\"uid=\"%u=>%u\" gid=\"%u=>%u\"", path, vid.uid, vid.gid, buf.st_uid, buf.st_gid);
            // yes the client can operate as the owner, we rewrite the virtual id
            m_vid.uid = buf.st_uid;
            m_vid.gid = buf.st_gid;
          }
        }
      }
    }
  }


  eos::common::RWMutexReadLock lock(Quota::gQuotaMutex);
  SpaceQuota* space = Quota::GetResponsibleSpaceQuota(path);
  if (space)
  {
    long long maxbytes_user, maxbytes_group, maxbytes_project;
    long long freebytes_user, freebytes_group, freebytes_project;
    freebytes_user = freebytes_group = freebytes_project = 0;
    maxbytes_user = maxbytes_group = maxbytes_project = 0;
    space->Refresh();
    maxbytes_user  = space->GetQuota(SpaceQuota::kUserBytesTarget, m_vid.uid);
    maxbytes_group = space->GetQuota(SpaceQuota::kGroupBytesTarget, m_vid.gid);
    maxbytes_project = space->GetQuota(SpaceQuota::kGroupBytesTarget, Quota::gProjectId);
    freebytes_user = maxbytes_user - space->GetQuota(SpaceQuota::kUserLogicalBytesIs, m_vid.uid);
    freebytes_group = maxbytes_group - space->GetQuota(SpaceQuota::kGroupLogicalBytesIs, m_vid.gid);
    freebytes_project = maxbytes_project - space->GetQuota(SpaceQuota::kGroupLogicalBytesIs, Quota::gProjectId);

    if (freebytes_user > freebytes)
      freebytes = freebytes_user;
    if (freebytes_group > freebytes)
      freebytes = freebytes_group;
    if (freebytes_project > freebytes)
      freebytes = freebytes_project;
    if (maxbytes_user > maxbytes)
      maxbytes = maxbytes_user;
    if (maxbytes_group > maxbytes)
      maxbytes = maxbytes_group;
    if (maxbytes_project > maxbytes)
      maxbytes = maxbytes_project;
  }
}

/*----------------------------------------------------------------------------*/
void
Quota::PrintOut (const char* space, XrdOucString &output, long uid_sel, long gid_sel, bool monitoring, bool translateids)
{
  {
    // we add this to have all quota nodes visible even if they are not in the configuration file
    LoadNodes();
  }

  eos::common::RWMutexReadLock vlock(FsView::gFsView.ViewMutex);
  eos::common::RWMutexReadLock lock(gQuotaMutex);

  output = "";
  XrdOucString spacenames = "";
  if (space == 0)
  {
    // make sure all configured spaces exist In the quota views
    std::map<std::string, FsSpace*>::const_iterator sit;

    for (sit = FsView::gFsView.mSpaceView.begin(); sit != FsView::gFsView.mSpaceView.end(); sit++)
    {
      Quota::GetSpaceQuota(sit->second->GetMember("name").c_str());
    }

    std::map<std::string, SpaceQuota*>::const_iterator it;

    for (it = gQuota.begin(); it != gQuota.end(); it++)
    {
      it->second->Refresh();
      it->second->PrintOut(output, uid_sel, gid_sel, monitoring, translateids);
    }
  }
  else
  {
    std::string sspace = space;
    SpaceQuota* spacequota = GetResponsibleSpaceQuota(space);
    if (spacequota)
    {
      spacequota->Refresh();
      spacequota->PrintOut(output, uid_sel, gid_sel, monitoring, translateids);
    }
  }
}

/*----------------------------------------------------------------------------*/
bool
Quota::SetQuota (XrdOucString space, long uid_sel, long gid_sel, long long bytes, long long files, XrdOucString &msg, int &retc)
{
  eos_static_debug("space=%s", space.c_str());

  eos::common::RWMutexReadLock lock(gQuotaMutex);

  SpaceQuota* spacequota = 0;

  XrdOucString configstring = "";
  XrdOucString configstringheader = "";
  char configvalue[1024];

  if (!space.endswith("/"))
  {
    space += "/";
  }

  if (!space.length())
  {
    spacequota = GetSpaceQuota("/eos/", false);
    configstringheader += "/eos/";
  }
  else
  {
    spacequota = GetSpaceQuota(space.c_str(), false);
    configstringheader += space.c_str();
  }

  configstringheader += ":";

  retc = EINVAL;

  if (spacequota)
  {
    char printline[1024];
    XrdOucString sizestring;
    msg = "";
    if ((uid_sel >= 0) && (bytes >= -1))
    {
      configstring = configstringheader;
      if (bytes == -1)
      {
        spacequota->RmQuota(SpaceQuota::kUserBytesTarget, uid_sel);
        sprintf(printline, "success: removed quota for uid=%ld from quotanode %s\n", uid_sel, spacequota->GetSpaceName());
      }
      else
      {
        spacequota->SetQuota(SpaceQuota::kUserBytesTarget, uid_sel, bytes);
        sprintf(printline, "success: updated quota for uid=%ld to %s bytes in quotanode %s\n", uid_sel, eos::common::StringConversion::GetReadableSizeString(sizestring, bytes, "B"), spacequota->GetSpaceName());
      }
      configstring += "uid=";
      configstring += (int) uid_sel;
      configstring += ":";
      configstring += SpaceQuota::GetTagAsString(SpaceQuota::kUserBytesTarget);
      sprintf(configvalue, "%llu", bytes);
      msg += printline;
      // store the setting into the config table
      if (bytes == -1)
      {
        // this is just a deletion
        gOFS->ConfEngine->DeleteConfigValue("quota", configstring.c_str());
      }
      else
      {
        gOFS->ConfEngine->SetConfigValue("quota", configstring.c_str(), configvalue);
      }

      retc = 0;
    }

    if ((uid_sel >= 0) && (files >= 0))
    {
      configstring = configstringheader;
      if (bytes == -1)
      {
        spacequota->RmQuota(SpaceQuota::kUserFilesTarget, uid_sel);
        sprintf(printline, "success: removed quota for uid=%ld from quotanode %s\n", uid_sel, spacequota->GetSpaceName());
      }
      else
      {
        spacequota->SetQuota(SpaceQuota::kUserFilesTarget, uid_sel, files);
        sprintf(printline, "success: updated quota for uid=%ld to %s files in quotanode %s\n", uid_sel, eos::common::StringConversion::GetReadableSizeString(sizestring, files, "-"), spacequota->GetSpaceName());
      }
      configstring += "uid=";
      configstring += (int) uid_sel;
      configstring += ":";
      configstring += SpaceQuota::GetTagAsString(SpaceQuota::kUserFilesTarget);
      sprintf(configvalue, "%llu", files);
      msg += printline;
      // store the setting into the config table
      if (files == -1)
      {
        gOFS->ConfEngine->DeleteConfigValue("quota", configstring.c_str());
      }
      else
      {
        gOFS->ConfEngine->SetConfigValue("quota", configstring.c_str(), configvalue);
      }
      retc = 0;
    }

    if ((gid_sel >= 0) && (bytes >= -1))
    {
      configstring = configstringheader;
      if (bytes == -1)
      {
        spacequota->RmQuota(SpaceQuota::kGroupBytesTarget, gid_sel);
        sprintf(printline, "success: removed quota for gid=%ld from quotanode %s\n", gid_sel, spacequota->GetSpaceName());
      }
      else
      {
        spacequota->SetQuota(SpaceQuota::kGroupBytesTarget, gid_sel, bytes);
        if (gid_sel != Quota::gProjectId)
        {
          sprintf(printline, "success: updated quota for gid=%ld to %s bytes in quotanode %s\n", gid_sel, eos::common::StringConversion::GetReadableSizeString(sizestring, bytes, "B"), spacequota->GetSpaceName());
        }
        else
        {
          sprintf(printline, "success: updated project quota to %s bytes in quotanode %s\n", eos::common::StringConversion::GetReadableSizeString(sizestring, bytes, "B"), spacequota->GetSpaceName());

        }
      }
      configstring += "gid=";
      configstring += (int) gid_sel;
      configstring += ":";
      configstring += SpaceQuota::GetTagAsString(SpaceQuota::kGroupBytesTarget);
      sprintf(configvalue, "%llu", bytes);
      msg += printline;
      // store the setting into the config table
      if (bytes == -1)
      {
        gOFS->ConfEngine->DeleteConfigValue("quota", configstring.c_str());
      }
      else
      {
        gOFS->ConfEngine->SetConfigValue("quota", configstring.c_str(), configvalue);
      }
      retc = 0;
    }

    if ((gid_sel >= 0) && (files >= -1))
    {
      configstring = configstringheader;
      if (files == -1)
      {
        spacequota->RmQuota(SpaceQuota::kGroupFilesTarget, gid_sel);
        sprintf(printline, "success: removed quota for gid=%ld from quotanode %s\n", gid_sel, spacequota->GetSpaceName());
      }
      else
      {
        spacequota->SetQuota(SpaceQuota::kGroupFilesTarget, gid_sel, files);
        if (gid_sel != Quota::gProjectId)
        {
          sprintf(printline, "success: updated quota for gid=%ld to %s files in quotanode %s\n", gid_sel, eos::common::StringConversion::GetReadableSizeString(sizestring, files, "-"), spacequota->GetSpaceName());
        }
        else
        {
          sprintf(printline, "success: updated project quota to %s files in quotanode %s\n", eos::common::StringConversion::GetReadableSizeString(sizestring, files, "-"), spacequota->GetSpaceName());
        }
      }

      configstring += "gid=";
      configstring += (int) gid_sel;
      configstring += ":";
      configstring += SpaceQuota::GetTagAsString(SpaceQuota::kGroupFilesTarget);
      sprintf(configvalue, "%llu", files);
      msg += printline;
      // store the setting into the config table
      if (files == 0)
      {
        gOFS->ConfEngine->DeleteConfigValue("quota", configstring.c_str());
      }
      else
      {
        gOFS->ConfEngine->SetConfigValue("quota", configstring.c_str(), configvalue);
      }
      retc = 0;
    }

    spacequota->UpdateLogicalSizeFactor();
    spacequota->UpdateTargetSums();
    return true;
  }
  else
  {
    msg = "error: no space defined with name ";
    msg += space;
    return false;
  }
}

/*----------------------------------------------------------------------------*/
bool
Quota::RmQuota (XrdOucString space, long uid_sel, long gid_sel, XrdOucString &msg, int &retc)
{
  eos_static_debug("space=%s", space.c_str());

  return SetQuota(space, uid_sel, gid_sel, -1, 0, msg, retc);
}

/*----------------------------------------------------------------------------*/
bool
Quota::RmSpaceQuota (XrdOucString space, XrdOucString &msg, int &retc)
{
  eos_static_debug("space=%s", space.c_str());

  eos::common::RWMutexWriteLock lock(gQuotaMutex);

  SpaceQuota* spacequota = 0;

  spacequota = GetSpaceQuota(space.c_str(), true);

  if (!spacequota)
  {
    msg = "error: there is no quota node under this path";
    retc = EINVAL;
    return false;
  }
  else
  {
    msg = "success: removed space quota for ";
    msg += space.c_str();
    spacequota->RemoveQuotaNode(msg, retc);
    gQuota.erase(space.c_str());
    // remove all configuration entries
    XrdOucString match = space.c_str();
    match += ":";
    gOFS->ConfEngine->DeleteConfigValueByMatch("quota", match.c_str());
    if (!gOFS->ConfEngine->AutoSave())
    {
      return false;
    }
    return true;
  }
}

/*----------------------------------------------------------------------------*/

uint64_t
Quota::MapSizeCB (const eos::FileMD * file)
{
  //------------------------------------------------------------------------
  //! Callback function for the namespace to calculate how much space a file occupies
  //------------------------------------------------------------------------

  if (!file)
    return 0;

  eos::FileMD::layoutId_t lid = file->getLayoutId();

  return (unsigned long long)
          file->getSize() * eos::common::LayoutId::GetSizeFactor(lid);
}

/*----------------------------------------------------------------------------*/
void
Quota::LoadNodes ()
{
  // iterate over the defined quota nodes and make them visible as SpaceQuota
  eos::common::RWMutexReadLock lock(gQuotaMutex);
  eos::QuotaStats::NodeMap::iterator it;
  std::vector<std::string> createQuota;

  // load all known nodes
  {
    eos::common::RWMutexReadLock nslock(gOFS->eosViewRWMutex);

    for (it = gOFS->eosView->getQuotaStats()->nodesBegin(); it != gOFS->eosView->getQuotaStats()->nodesEnd(); it++)
    {
      try
      {
        eos::ContainerMD::id_t id = it->first;
        eos::ContainerMD* container = gOFS->eosDirectoryService->getContainerMD(id);
        std::string quotapath = gOFS->eosView->getUri(container);
        SpaceQuota* spacequota = Quota::GetSpaceQuota(quotapath.c_str(), true);
        if (!spacequota)
        {
          createQuota.push_back(quotapath);
        }
      }
      catch (eos::MDException &e)
      {
        errno = e.getErrno();
        eos_static_err("msg=\"exception\" ec=%d emsg=\"%s\"\n",
                       e.getErrno(), e.getMessage().str().c_str());
      }
    }
  }

  // create missing nodes without namespace mutex held
  for (size_t i=0; i < createQuota.size(); ++i)
  {
    SpaceQuota* spacequota = Quota::GetSpaceQuota(createQuota[i].c_str(), false);
    spacequota = Quota::GetSpaceQuota(createQuota[i].c_str(), false);
    if (spacequota)
    {
      eos_static_notice("Created space for quota node: %s", createQuota[i].c_str());
    }
    else
    {
      eos_static_err("Failed to create space for quota node: %s\n", createQuota[i].c_str());
    }
  }
}

/*----------------------------------------------------------------------------*/
void
Quota::NodesToSpaceQuota ()
{
  eos::common::RWMutexReadLock locker(gQuotaMutex);
  eos::common::RWMutexReadLock lock(gOFS->eosViewRWMutex);
  // inserts the current state of the quota nodes into SpaceQuota's 
  eos::QuotaStats::NodeMap::iterator it;
  for (it = gOFS->eosView->getQuotaStats()->nodesBegin(); it != gOFS->eosView->getQuotaStats()->nodesEnd(); it++)
  {
    try
    {
      eos::ContainerMD::id_t id = it->first;
      eos::ContainerMD* container = gOFS->eosDirectoryService->getContainerMD(id);
      std::string quotapath = gOFS->eosView->getUri(container);
      NodeToSpaceQuota(quotapath.c_str());
    }
    catch (eos::MDException &e)
    {
      errno = e.getErrno();
      eos_static_err("msg=\"exception\" ec=%d emsg=\"%s\"\n",
                     e.getErrno(), e.getMessage().str().c_str());
    }
  }
}

/*----------------------------------------------------------------------------*/

void
Quota::NodeToSpaceQuota (const char* name)
{

  // this routine has to be called with gQuotaMutex read-locked 
  // this routine has to be called with eosViewMutexRW locked
  if (!name)
    return;

  SpaceQuota* spacequota = Quota::GetSpaceQuota(name, false);

  if (spacequota && spacequota->UpdateQuotaNodeAddress() && spacequota->GetQuotaNode())
  {
    // insert current state of a single quota node into a SpaceQuota
    eos::QuotaNode::UserMap::const_iterator itu;
    eos::QuotaNode::GroupMap::const_iterator itg;

    spacequota->ResetQuota(SpaceQuota::kGroupBytesIs, gProjectId);
    spacequota->ResetQuota(SpaceQuota::kGroupFilesIs, gProjectId);
    spacequota->ResetQuota(SpaceQuota::kGroupLogicalBytesIs, gProjectId);

    // loop over user
    for (itu = spacequota->GetQuotaNode()->userUsageBegin(); itu != spacequota->GetQuotaNode()->userUsageEnd(); itu++)
    {
      spacequota->ResetQuota(SpaceQuota::kUserBytesIs, itu->first);
      spacequota->AddQuota(SpaceQuota::kUserBytesIs, itu->first, itu->second.physicalSpace);
      spacequota->ResetQuota(SpaceQuota::kUserFilesIs, itu->first);
      spacequota->AddQuota(SpaceQuota::kUserFilesIs, itu->first, itu->second.files);
      spacequota->ResetQuota(SpaceQuota::kUserLogicalBytesIs, itu->first);
      spacequota->AddQuota(SpaceQuota::kUserLogicalBytesIs, itu->first, itu->second.space);

      if (spacequota->GetQuota(SpaceQuota::kGroupBytesTarget, gProjectId) > 0)
      {
        // only account in project quota noes
        spacequota->AddQuota(SpaceQuota::kGroupBytesIs, gProjectId, itu->second.physicalSpace);
        spacequota->AddQuota(SpaceQuota::kGroupLogicalBytesIs, gProjectId, itu->second.space);
        spacequota->AddQuota(SpaceQuota::kGroupFilesIs, gProjectId, itu->second.files);
      }
    }
    for (itg = spacequota->GetQuotaNode()->groupUsageBegin(); itg != spacequota->GetQuotaNode()->groupUsageEnd(); itg++)
    {
      // dont' update the project quota directory from the quota 
      if (itg->first == gProjectId)
        continue;

      spacequota->ResetQuota(SpaceQuota::kGroupBytesIs, itg->first);
      spacequota->AddQuota(SpaceQuota::kGroupBytesIs, itg->first, itg->second.physicalSpace);
      spacequota->ResetQuota(SpaceQuota::kGroupFilesIs, itg->first);
      spacequota->AddQuota(SpaceQuota::kGroupFilesIs, itg->first, itg->second.files);
      spacequota->ResetQuota(SpaceQuota::kGroupLogicalBytesIs, itg->first);
      spacequota->AddQuota(SpaceQuota::kGroupLogicalBytesIs, itg->first, itg->second.space);
    }
  }
}

/*----------------------------------------------------------------------------*/

EOSMGMNAMESPACE_END
