//------------------------------------------------------------------------------
// File: FsView.cc
// Author: Andreas-Joachim Peters - CERN
//------------------------------------------------------------------------------

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

#include <math.h>
#include "mgm/FsView.hh"
#include "common/StringConversion.hh"
#include "XrdSys/XrdSysTimer.hh"
#ifndef EOSMGMFSVIEWTEST
#include "mgm/GeoTreeEngine.hh"
#endif

EOSMGMNAMESPACE_BEGIN

FsView FsView::gFsView;
std::string FsSpace::gConfigQueuePrefix;
std::string FsGroup::gConfigQueuePrefix;
std::string FsNode::gConfigQueuePrefix;
std::string FsNode::gManagerId;

bool FsSpace::gDisableDefaults = false;

#ifndef EOSMGMFSVIEWTEST
ConfigEngine* FsView::ConfEngine = 0;
#endif

//------------------------------------------------------------------------------
// Destructor - destructs all the branches starting at this node
//------------------------------------------------------------------------------
GeoTreeElement::~GeoTreeElement()
{
  for (auto it = mSons.begin(); it != mSons.end(); it++)
    delete it->second;
}

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
GeoTree::GeoTree() : pLevels(8)
{
  pLevels.resize(1);
  pRoot = new tElement;
  pLevels[0].insert(pRoot);
  pRoot->mTagToken = "<ROOT>";
  pRoot->mFullTag = "<ROOT>";
  pRoot->mFather = NULL;
}

//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
GeoTree::~GeoTree()
{
  delete pRoot;
}

//------------------------------------------------------------------------------
// @brief Insert a FileSystem into the tree
// @param fs the fsid of the FileSystem
// @return true if success, false if failure
//------------------------------------------------------------------------------
bool GeoTree::insert(const fsid_t& fs)
{
  if (pLeaves.count(fs))
    return false;

  std::string geotag = getGeoTag(fs);
  // Tokenize the geotag (geo tag is like adas::acsd::csdw::fee)
  std::vector<std::string> geotokens;
  eos::common::StringConversion::EmptyTokenize(geotag, geotokens, ":");
  size_t s;
  s = geotokens.size();

  for (size_t i = 0; i < s; i++)
  {
    if (geotokens[i].size())
      geotokens.push_back(geotokens[i]);
  }

  geotokens.erase(geotokens.begin(), geotokens.begin() + s);

  if (geotokens.empty())
    geotokens.push_back(""); // geotag is not provided

  tElement* father = pRoot;
  std::string fulltag = pRoot->mFullTag;
  // Insert all the geotokens in the tree
  tElement* currentnode = pRoot;
  tElement* currentleaf = NULL;

  for (int i = 0; i < (int)geotokens.size() - 1; i++)
  {
    const std::string& geotoken = geotokens[i];

    if (currentnode->mSons.count(geotoken))
    {
      currentnode = father->mSons[geotoken];

      if (!fulltag.empty()) fulltag += "::";

      fulltag += geotoken;
    }
    else
    {
      currentnode = new tElement;
      currentnode->mTagToken = geotoken;

      if (!fulltag.empty()) fulltag += "::";

      fulltag += geotoken;
      currentnode->mFullTag = fulltag;
      currentnode->mFather = father;
      father->mSons[geotoken] = currentnode;

      if ((int)pLevels.size() < i + 2) pLevels.resize(i + 2);

      pLevels[i + 1].insert(currentnode);
    }

    father = currentnode;
  }

  // Finally, insert the fs
  if (!father->mSons.count(geotokens.back()))
  {
    currentleaf = new tElement;
    currentleaf->mFather = father;
    currentleaf->mTagToken = geotokens.back();

    if (!fulltag.empty()) fulltag += "::";

    fulltag += geotokens.back();
    currentleaf->mFullTag = fulltag;
    father->mSons[geotokens.back()] = currentleaf;

    if (pLevels.size() < geotokens.size() + 1) pLevels.resize(geotokens.size() + 1);

    pLevels[geotokens.size()].insert(currentleaf);
  }
  else
  {
    // assert(father->mSons[geotokens.back()]->mIsLeaf);
    currentleaf = father->mSons[geotokens.back()];
  }

  if (!currentleaf->mFsIds.count(fs))
  {
    currentleaf->mFsIds.insert(fs);
    pLeaves[fs] = currentleaf;
  }
  else
  {
    return false;
  }

  return true;
}

//------------------------------------------------------------------------------
// @brief Number of FileSystems in the tree
// @return the number of FileSystems in the tree
//------------------------------------------------------------------------------
size_t GeoTree::size() const
{
  return pLeaves.size();
}

//------------------------------------------------------------------------------
// @brief Remove a FileSystem from the tree
// @param fs the fsid of the FileSystem
// @return true if success, false if failure
//------------------------------------------------------------------------------
bool GeoTree::erase(const fsid_t& fs)
{
  tElement* leaf;

  if (!pLeaves.count(fs))
    return false;
  else
    leaf = pLeaves[fs];

  pLeaves.erase(fs);
  leaf->mFsIds.erase(fs);
  tElement* father = leaf;

  if (leaf->mFsIds.empty() && leaf->mSons.empty())
  {
    // Compute the depth for the current father
    int i, depth = -1;

    for (i = (int)pLevels.size() - 1; i >= 0; i--)
    {
      if (pLevels[i].count(father))
      {
        depth = i;
        break;
      }
    }

    assert(depth >= 0); // consistency check

    // Go uproot until there is more than one branch
    while (father->mFather && father->mFather->mSons.size() == 1 &&
           father->mFather->mFsIds.empty())
    {
      if (father->mFather == pRoot) break;

      pLevels[depth--].erase(father);
      // We don't update the father's sons list on purpose in order to keep
      // the reference for the destruction
      father = father->mFather;
    }

    // Erase the full branch
    if (father->mFather)
      father->mFather->mSons.erase(father->mTagToken);

    pLevels[depth].erase(father);
    delete father;
    // Update the pLevels size if needed
    int count = 0;

    for (auto it = pLevels.rbegin(); it != pLevels.rend(); it++)
    {
      if (!it->empty())
      {
        if (count) pLevels.resize(pLevels.size() - count);
        break;
      }

      count++;
    }
  }

  return true;
}

//------------------------------------------------------------------------------
// @brief Get the geotag at which a file system is stored in the tree
// @param fs the fsid of the FileSystem
// @param geoTag returns the geotag if fs was found in the tree
// @return true if success, false if failure
//------------------------------------------------------------------------------
bool GeoTree::getGeoTagInTree(const fsid_t& fs , std::string& geoTag)
{
  if (!pLeaves.count(fs))
    return false;
  else
    geoTag = pLeaves[fs]->mFullTag;

  return true;
}

//------------------------------------------------------------------------------
// @brief Get the geotag of FileSystem
// @param fs the fsid of the FileSystem
// @return return the geotag if found
//------------------------------------------------------------------------------
std::string GeoTree::getGeoTag(const fsid_t& fs) const
{
  return FsView::gFsView.mIdView[fs]->GetString("stat.geotag");
}

//------------------------------------------------------------------------------
// ++ operator
//------------------------------------------------------------------------------
GeoTree::const_iterator GeoTree::const_iterator::operator++(int)
{
  const_iterator it;
  it.mIt = mIt;
  mIt++;
  return it;
}

//------------------------------------------------------------------------------
// -- operator
//------------------------------------------------------------------------------
GeoTree::const_iterator GeoTree::const_iterator::operator--(int)
{
  const_iterator it;
  it.mIt = mIt;
  mIt--;
  return it;
}

//------------------------------------------------------------------------------
// ++ operator
//------------------------------------------------------------------------------
GeoTree::const_iterator GeoTree::const_iterator::operator++()
{
  const_iterator it;
  it.mIt = mIt;
  mIt++;
  return *this;
}

//------------------------------------------------------------------------------
// -- operator
//------------------------------------------------------------------------------
GeoTree::const_iterator GeoTree::const_iterator::operator--()
{
  const_iterator it;
  it.mIt = mIt;
  mIt--;
  return *this;
}

//------------------------------------------------------------------------------
// Pointer operator
//------------------------------------------------------------------------------
const eos::common::FileSystem::fsid_t&
GeoTree::const_iterator::operator*() const
{
  return mIt->first;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
GeoTree::const_iterator::operator
const eos::common::FileSystem::fsid_t* () const
{
  return &mIt->first;
}

//------------------------------------------------------------------------------
// = operator
//------------------------------------------------------------------------------
const GeoTree::const_iterator&
GeoTree::const_iterator::operator= (const const_iterator& it)
{
  mIt = it.mIt;
  return *this;
}

//------------------------------------------------------------------------------
// begin
//------------------------------------------------------------------------------
GeoTree::const_iterator GeoTree::begin() const
{
  const_iterator it;
  it.mIt = pLeaves.begin();
  return it;
}

//------------------------------------------------------------------------------
// cbegin
//------------------------------------------------------------------------------
GeoTree::const_iterator GeoTree::cbegin() const
{
  return begin();
}

//------------------------------------------------------------------------------
// end
//------------------------------------------------------------------------------
GeoTree::const_iterator GeoTree::end() const
{
  const_iterator it;
  it.mIt = pLeaves.end();
  return it;
}

//------------------------------------------------------------------------------
// cend
//------------------------------------------------------------------------------
GeoTree::const_iterator GeoTree::cend() const
{
  return end();
}

//------------------------------------------------------------------------------
// Find
//------------------------------------------------------------------------------
GeoTree::const_iterator GeoTree::find(const fsid_t& fsid) const
{
  const_iterator it;
  it.mIt = pLeaves.find(fsid);
  return it;
}

//------------------------------------------------------------------------------
// @brief Run an aggregator through the tree
// @param aggregator the aggregator to be run
// @return true if success, false if failure
//
// At any depth level, the aggregator is fed ONLY with the data
// of the ONE deeper level in the tree
//------------------------------------------------------------------------------
bool GeoTree::runAggregator(GeoTreeAggregator* aggregator) const
{
  if (pLevels.empty())
    return false;

  // Build the GeoTags and the depth indexes
  size_t elemCount = 0;
  std::vector<std::string> geotags;
  std::vector<size_t> depthlevelsendindexes;

  for (auto itl = pLevels.begin(); itl != pLevels.end(); itl++)
  {
    geotags.resize(geotags.size() + itl->size());

    for (auto ite = itl->rbegin(); ite != itl->rend(); ite++)
    {
      // could be made faster and more complex but probably not necessary for the moment
      geotags[elemCount] = (*ite)->mTagToken;
      GeoTreeElement* element = *ite;

      while (element->mFather)
      {
        element = element->mFather;
        geotags[elemCount] = element->mTagToken + "::" + geotags[elemCount];
      }

      elemCount++;
    }

    depthlevelsendindexes.push_back(elemCount);
  }

  aggregator->init(geotags, depthlevelsendindexes);
  elemCount--;

  for (auto itl = pLevels.rbegin(); itl != pLevels.rend(); itl++)
  {
    for (auto ite = itl->begin(); ite != itl->end(); ite++)
    {
      (*ite)->mId = elemCount;

      if (!aggregator->aggregateLeavesAndNodes((*ite)->mFsIds, (*ite)->mSons,
                                               elemCount--))
        return false;
    }
  }

  return true;
}

//------------------------------------------------------------------------------
// @brief Recursive debug helper function to display the tree
// @param el the tree element to start the display from
// @param fullgeotag the full geotag of the element
//------------------------------------------------------------------------------
char* GeoTree::dumpTree(char* buffer, GeoTreeElement* el,
                        std::string fullgeotag) const
{
  if (!el->mFsIds.empty())
  {
    auto& fsids = el->mFsIds;
    buffer += sprintf(buffer, "%s%s\n", fullgeotag.c_str(), el->mTagToken.c_str());
    buffer += sprintf(buffer, "mFsIds\n");

    for (auto fsit = fsids.begin(); fsit != fsids.end(); fsit++)
    {
      buffer += sprintf(buffer, "%d  ", *fsit);
    }

    if (fsids.begin() != fsids.end())
      buffer += sprintf(buffer, "\n");
  }
  else
  {
    fullgeotag += el->mTagToken;
    fullgeotag += "   ";
    auto& sons = el->mSons;

    for (auto it = sons.cbegin(); it != sons.cend(); it++)
      buffer = dumpTree(buffer, it->second, fullgeotag);
  }

  return buffer;
}

//------------------------------------------------------------------------------
// Debug helper function to display the leaves in the tree
//------------------------------------------------------------------------------
char* GeoTree::dumpLeaves(char* buffer) const
{
  for (auto it = pLeaves.begin(); it != pLeaves.end(); it++)
  {
    buffer += sprintf(buffer, "%d %s\n", it->first, it->second->mFullTag.c_str());
    buffer += sprintf(buffer, "@mLeaves@mFsIds\n");
  }

  return buffer;
}

//------------------------------------------------------------------------------
// Debug helper function to display the elements of the tree sorted by levels
//------------------------------------------------------------------------------
char* GeoTree::dumpLevels(char* buffer) const
{
  int level = 0;

  for (auto it = pLevels.begin(); it != pLevels.end(); it++)
  {
    buffer += sprintf(buffer, "level %d (%lu)\n", level++, it->size());

    for (auto it2 = it->begin(); it2 != it->end(); it2++)
    {
      buffer += sprintf(buffer, "%s\t", (*it2)->mFullTag.c_str());
    }

    buffer += sprintf(buffer, "\n");
  }

  return buffer;
}

//------------------------------------------------------------------------------
// @brief Debug helper function to display all the content of the tree
//------------------------------------------------------------------------------
char* GeoTree::dump(char* buffer) const
{
  buffer += sprintf(buffer, "@mRoot\n");
  buffer = dumpTree(buffer, pRoot);
  buffer += sprintf(buffer, "@mLeaves\n");
  buffer = dumpLeaves(buffer);
  buffer += sprintf(buffer, "@mLevels\n");
  buffer = dumpLevels(buffer);
  return buffer;
}

//------------------------------------------------------------------------------
// @brief Get the sums at each tree element
//------------------------------------------------------------------------------
const std::vector<double>* DoubleAggregator::getSums() const
{
  return &pSums;
}

//------------------------------------------------------------------------------
// @brief Get the averages at each tree element
//------------------------------------------------------------------------------
const std::vector<double>* DoubleAggregator::getMeans() const
{
  return &pMeans;
}

//------------------------------------------------------------------------------
// @brief Get the maximum deviations at each tree element
//------------------------------------------------------------------------------
const std::vector<double>* DoubleAggregator::getMaxAbsDevs() const
{
  return &pMaxAbsDevs;
}

//------------------------------------------------------------------------------
// @brief Get the standard deviations at each tree element
//------------------------------------------------------------------------------
const std::vector<double>* DoubleAggregator::getStdDevs() const
{
  return &pStdDevs;
}

//------------------------------------------------------------------------------
// Get the geotags at each tree element
//------------------------------------------------------------------------------
const std::vector<std::string>* DoubleAggregator::getGeoTags() const
{
  return &pGeoTags;
}

//------------------------------------------------------------------------------
// @brief Get the end index (excluded) for a given depth level
// @param depth the maximum depth to be reached (-1 for unlimited)
// @return the index of the first element in the vectors being deeper that depth
//------------------------------------------------------------------------------
size_t DoubleAggregator::getEndIndex(int depth) const
{
  if (depth < 0 || depth > (int)pDepthLevelsIndexes.size() - 1)
    depth = pDepthLevelsIndexes.size() - 1;

  return pDepthLevelsIndexes[depth];
};

//------------------------------------------------------------------------------
// @brief Constructor
// @param param Name of the parameter statistics have to be computed for
//------------------------------------------------------------------------------
DoubleAggregator::DoubleAggregator(const char* param):
    pParam(param), pView(NULL)
{}

//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
DoubleAggregator::~DoubleAggregator()
{}

//------------------------------------------------------------------------------
// @brief Set the view ordering the statistics. Needs to be set before running
// the aggregator.
// @param view Pointer to the view ordering the statistics
//------------------------------------------------------------------------------
void DoubleAggregator::setView(BaseView* view)
{
  pView = view;
}

//------------------------------------------------------------------------------
// Initialize
//------------------------------------------------------------------------------
bool
DoubleAggregator::init(const std::vector<std::string>& geotags,
                       const std::vector<size_t>& depthLevelsIndexes)
{
  // Check that the view is defined, this is necessary for the subsequent calls
  // to AggregateXXX.
  assert(pView);
  pGeoTags = geotags;
  pDepthLevelsIndexes = depthLevelsIndexes;
  return true;
}

//------------------------------------------------------------------------------
// Aggregate leaves
//------------------------------------------------------------------------------
bool
DoubleAggregator::aggregateLeaves(
    const std::set<eos::common::FileSystem::fsid_t>& leaves, const size_t& idx)
{
  // The following should happen only at the first call
  if ((int)idx > (int)pMeans.size() - 1)
  {
    pSums.resize(idx + 1);
    pMeans.resize(idx + 1);
    pMaxDevs.resize(idx + 1);
    pMinDevs.resize(idx + 1);
    pMaxAbsDevs.resize(idx + 1);
    pStdDevs.resize(idx + 1);
    pNb.resize(idx + 1);
  }

  pNb[idx] = pView->ConsiderCount(false, &leaves);

  if (pNb[idx])
  {
    pSums[idx] = pView->SumDouble(pParam.c_str(), false, &leaves);
    pMeans[idx] = pView->AverageDouble(pParam.c_str(), false, &leaves);
    pMaxDevs[idx] = (pNb[idx] == 1) ? 0 : pView->MaxDeviation(pParam.c_str(), false,
                                                              &leaves);
    pMinDevs[idx] = (pNb[idx] == 1) ? 0 : pView->MinDeviation(pParam.c_str(), false,
                                                              &leaves);
    pStdDevs[idx] = (pNb[idx] == 1) ? 0 : pView->SigmaDouble(pParam.c_str(), false,
                                                             &leaves);
    pMaxAbsDevs[idx] = (pNb[idx] == 1) ? 0 : std::max(abs(pMaxDevs[idx]),
                                                      abs(pMinDevs[idx]));
  }
  else
  {
    pSums[idx] = 0;
    pMeans[idx] = 0;
    pMaxDevs[idx] = 0;
    pMinDevs[idx] = 0;
    pStdDevs[idx] = 0;
    pMaxAbsDevs[idx] = 0;
  }

  return true;
}

//------------------------------------------------------------------------------
// Aggregate nodes
//------------------------------------------------------------------------------
bool
DoubleAggregator::aggregateNodes(
    const std::map<std::string , GeoTreeElement*>& nodes,
    const size_t& idx, bool includeSelf)
{
  double pS, pM, pMAD, pSD, pMiD, pMaD;
  pS = pM = pMAD = pSD = 0;
  pMiD = DBL_MAX;
  pMaD = -DBL_MAX;
  long long pN = 0;

  for (auto it = nodes.begin(); it != nodes.end(); it++)
  {
    size_t i = it->second->mId;
    pS += pSums[i];
    pN += pNb[i];
  }

  if (pN) pM = pS / pN;

  for (auto it = nodes.begin(); it != nodes.end(); it++)
  {
    size_t i = it->second->mId;

    if (pNb[i]) // consider this only if there is something there
    {
      pMiD = std::min(pMiD,
                      std::min((pMinDevs[i] + pMeans[i]) - pM , (pMaxDevs[i] + pMeans[i]) - pM));
      pMaD = std::max(pMaD,
                      std::max((pMinDevs[i] + pMeans[i]) - pM , (pMaxDevs[i] + pMeans[i]) - pM));
      pSD += pNb[i] * (pStdDevs[i] * pStdDevs[i] + pMeans[i] * pMeans[i]);
    }
  }

  if (pN)
  {
    pSD = sqrt(pSD / pN - pM * pM);
    pMAD = std::max(fabs(pMaD) , fabs(pMiD));
  }

  if (includeSelf)
  {
    pS += pSums[idx];
    pN += pNb[idx];

    if (pN) pM = pS / pN;

    pMiD = std::min(pMiD,
                    std::min((pMinDevs[idx] + pMeans[idx]) - pM ,
                             (pMaxDevs[idx] + pMeans[idx]) - pM));
    pMaD = std::max(pMaD,
                    std::max((pMinDevs[idx] + pMeans[idx]) - pM ,
                             (pMaxDevs[idx] + pMeans[idx]) - pM));
    pSD += pNb[idx] * (pStdDevs[idx] * pStdDevs[idx] + pMeans[idx] * pMeans[idx]);

    if (pN)
    {
      pSD = sqrt(pSD / pN - pM * pM);
      pMAD = std::max(fabs(pMaD) , fabs(pMiD));
    }
  }

  pSums[idx] = pS;
  pMeans[idx] = pM;
  pMaxAbsDevs[idx] = pMAD;
  pStdDevs[idx] = pSD;
  pMinDevs[idx] = pMiD;
  pMaxDevs[idx] = pMaD;
  pNb[idx] = pN;
  return true;
}

//------------------------------------------------------------------------------
// DeepAggregate
//------------------------------------------------------------------------------
bool DoubleAggregator::deepAggregate(
    const std::set<eos::common::FileSystem::fsid_t>& leaves,
    const size_t& idx)
{
  // Not necessary for the statistics. Might be usefull for some more advanced
  // statistics requiring using the whole distribution at each depth.
  return false;
}

//------------------------------------------------------------------------------
// @brief Constructor
// @param param Name of the parameter statistics have to be computed for
//------------------------------------------------------------------------------
LongLongAggregator::LongLongAggregator(const char* param):
    pParam(param), pView(NULL)
{}

//------------------------------------------------------------------------------
// @brief Destructor
//------------------------------------------------------------------------------
LongLongAggregator::~LongLongAggregator()
{}

//------------------------------------------------------------------------------
// @brief Set the view ordering the statistics. Needs to be set before running
//        the aggregator
// @param view Pointer to the view ordering the statistics
//------------------------------------------------------------------------------
void LongLongAggregator::setView(BaseView* view)
{
  pView = view;
}

//------------------------------------------------------------------------------
// Initialize
//------------------------------------------------------------------------------
bool
LongLongAggregator::init(const std::vector<std::string>& geotags,
                         const std::vector<size_t>& depthLevelsIndexes)
{
  assert(pView);
  pGeoTags = geotags;
  pDepthLevelsIndexes = depthLevelsIndexes;
  return true;
}

//------------------------------------------------------------------------------
//@brief Get the sums at each tree element
//------------------------------------------------------------------------------
const std::vector<long long>* LongLongAggregator::getSums() const
{
  return &pSums;
}

//------------------------------------------------------------------------------
// @brief Get the geotags at each tree element
//------------------------------------------------------------------------------
const std::vector<std::string>* LongLongAggregator::getGeoTags() const
{
  return &pGeoTags;
}

//------------------------------------------------------------------------------
// @brief Get the end index (excluded) for a given depth level
// @param depth the maximum depth to be reached (-1 for unlimited)
// @return the index of the first element in the vectors being deeper that depth
//------------------------------------------------------------------------------
size_t LongLongAggregator::getEndIndex(int depth) const
{
  if (depth < 0 || depth > (int)pDepthLevelsIndexes.size() - 1)
    depth = pDepthLevelsIndexes.size() - 1;

  return pDepthLevelsIndexes[depth];
};

//------------------------------------------------------------------------------
// Aggregate leaves
//------------------------------------------------------------------------------
bool
LongLongAggregator::aggregateLeaves(
    const std::set<eos::common::FileSystem::fsid_t>& leaves,
    const size_t& idx)
{
  // The following should happen only at the first call
  if ((int)idx > (int)pSums.size() - 1)
  {
    pSums.resize(idx + 1);
  }

  pSums[idx] = 0;
  pSums[idx] = pView->SumLongLong(pParam.c_str(), false, &leaves);
  return true;
}

//------------------------------------------------------------------------------
// Aggregate nodes
//------------------------------------------------------------------------------
bool
LongLongAggregator::aggregateNodes(
    const std::map<std::string , GeoTreeElement*>& nodes, const size_t& idx,
    bool includeSelf)
{
  long long pS = 0;

  for (auto it = nodes.begin(); it != nodes.end(); it++)
  {
    size_t i = it->second->mId;
    pS += pSums[i];
  }

  if (includeSelf)
  {
    pS += pSums[idx];
  }

  pSums[idx] = pS;
  return true;
};

//------------------------------------------------------------------------------
// Deep aggregate
//------------------------------------------------------------------------------
bool
LongLongAggregator::deepAggregate(
    const std::set<eos::common::FileSystem::fsid_t>& leaves,
    const size_t& idx)
{
  // Not necessary for the statistics
  // might be usefull for some more advanced statistics requiring using the
  // whole distribution at each depth e.g median
  return false;
}

//------------------------------------------------------------------------------
// Check if quota is enabled for space
//-----------------------------------------------------------------------------
bool FsView::IsQuotaEnabled(const std::string& space)
{
  bool is_enabled = false;
  std::string key = "quota";

  if (mSpaceView.count(space))
  {
    std::string is_on = mSpaceView[space]->GetConfigMember(key);
    is_enabled = (is_on == "on");
  }

  return is_enabled;
}

//------------------------------------------------------------------------------
// @brief return's the printout format for a given option
// @param option see the implementation for valid options
// @return std;:string with format line passed to the printout routine
//------------------------------------------------------------------------------
std::string
FsView::GetNodeFormat(std::string option)
{
  if (option == "m")
  {
    // monitoring format
    return "member=type:width=1:format=os|sep= |member=hostport:width=1:format=os|sep= |member=status:width=1:format=os|sep= |member=cfg.status:width=1:format=os|sep= |member=cfg.txgw:width=1:format=os|sep= |member=heartbeatdelta:width=1:format=os|sep= |member=nofs:width=1:format=ol|sep= |avg=stat.disk.load:width=1:format=of|sep= |sig=stat.disk.load:width=1:format=of|sep= |sum=stat.disk.readratemb:width=1:format=ol|sep= |sum=stat.disk.writeratemb:width=1:format=ol|sep= |sum=stat.net.ethratemib:width=1:format=ol|sep= |sum=stat.net.inratemib:width=1:format=ol|sep= |sum=stat.net.outratemib:width=1:format=ol|sep= |sum=stat.ropen:width=1:format=ol|sep= |sum=stat.wopen:width=1:format=ol|sep= |sum=stat.statfs.freebytes:width=1:format=ol|sep= |sum=stat.statfs.usedbytes:width=1:format=ol|sep= |sum=stat.statfs.capacity:width=1:format=ol|sep= |sum=stat.usedfiles:width=1:format=ol|sep= |sum=stat.statfs.ffree:width=1:format=ol|sep= |sum=stat.statfs.fused:width=1:format=ol|sep= |sum=stat.statfs.files:width=1:format=ol|sep= |sum=stat.balancer.running:width=1:format=ol:tag=stat.balancer.running|sep= |sum=stat.drainer.running:width=1:format=ol:tag=stat.drainer.running|sep= |member=stat.gw.queued:width=1:format=os:tag=stat.gw.queued|sep= |member=cfg.stat.sys.vsize:width=1:format=ol|sep= |member=cfg.stat.sys.rss:width=1:format=ol|sep= |member=cfg.stat.sys.threads:width=1:format=ol|sep= |member=cfg.stat.sys.sockets:width=1:format=os|sep= |member=cfg.stat.sys.eos.version:width=1:format=os|sep= |member=cfg.stat.sys.kernel:width=1:format=os|sep= |member=cfg.stat.sys.eos.start:width=1:format=os|sep= |member=cfg.stat.sys.uptime:width=1:format=os|sep= |sum=stat.disk.iops?configstatus@rw:width=1:format=ol|sep= |sum=stat.disk.bw?configstatus@rw:width=1:format=ol|sep= |member=cfg.stat.geotag:width=1:format=os|sep= |member=cfg.gw.rate:width=1:format=os|sep= |member=cfg.gw.ntx:width=1:format=os";
  }

  if (option == "io")
  {
    // io format
    return "header=1:member=hostport:width=32:format=sS|sep= |member=cfg.stat.geotag:width=16:format=s|sep= |avg=stat.disk.load:width=10:format=f:tag=diskload|sep= |sum=stat.disk.readratemb:width=12:format=+l:tag=diskr-MB/s|sep= |sum=stat.disk.writeratemb:width=12:format=+l:tag=diskw-MB/s|sep= |sum=stat.net.ethratemib:width=10:format=l:tag=eth-MiB/s|sep= |sum=stat.net.inratemib:width=10:format=l:tag=ethi-MiB|sep= |sum=stat.net.outratemib:width=10:format=l:tag=etho-MiB|sep= |sum=stat.ropen:width=6:format=l:tag=ropen|sep= |sum=stat.wopen:width=6:format=l:tag=wopen|sep= |sum=stat.statfs.usedbytes:width=12:format=+l:unit=B:tag=used-bytes|sep= |sum=stat.statfs.capacity:width=12:format=+l:unit=B:tag=max-bytes|sep= |sum=stat.usedfiles:width=12:format=+l:tag=used-files|sep= |sum=stat.statfs.files:width=11:format=+l:tag=max-files|sep= |sum=stat.balancer.running:width=10:format=l:tag=bal-shd|sep= |sum=stat.drainer.running:width=10:format=l:tag=drain-shd|sep= |member=inqueue:width=10:format=s:tag=gw-queue|sep= |sum=stat.disk.iops?configstatus@rw:width=6:format=l:tag=iops|sep= |sum=stat.disk.bw?configstatus@rw:width=9:format=+l:unit=MB:tag=bw|";
  }

  if (option == "sys")
  {
    // system format
    return "header=1:member=hostport:width=32:format=sS|sep= |member=cfg.stat.geotag:width=16:format=s|sep= |member=cfg.stat.sys.vsize:width=12:format=+l|tag=vsize|sep= |member=cfg.stat.sys.rss:width=12:format=+l:tag=rss|sep= |member=cfg.stat.sys.threads:width=12:format=+l:tag=threads|sep= |member=cfg.stat.sys.sockets:width=10:format=s:tag=sockets|sep= |member=cfg.stat.sys.eos.version:width=12:format=s:tag=eos|sep= |member=cfg.stat.sys.kernel:width=30:format=s:tag=kernel version|sep= |member=cfg.stat.sys.eos.start:width=32:format=s:tag=start|sep= |member=cfg.stat.sys.uptime:width=80:format=s:tag=uptime";
  }

  if (option == "fsck")
  {
    // fsck format
    return "header=1:member=hostport:width=32:format=sS|sep= |sum=stat.fsck.mem_n:width=8:format=l:tag=n(mem)|sep= |sum=stat.fsck.d_sync_n:width=8:format=l:tag=n(disk)|sep= |sum=stat.fsck.m_sync_n:width=8:format=l:tag=n(mgm)|sep= |sum=stat.fsck.orphans_n:width=12:format=l:tag=e(orph)|sep= |sum=stat.fsck.unreg_n:width=12:format=l:tag=e(unreg)|sep= |sum=stat.fsck.rep_diff_n:width=12:format=l:tag=e(layout)|sep= |sum=stat.fsck.rep_missing_n:width=12:format=l:tag=e(miss)|sep= |sum=stat.fsck.d_mem_sz_diff:width=12:format=l:tag=e(disksize)|sep= |sum=stat.fsck.m_mem_sz_diff:width=12:format=l:tag=e(mgmsize)|sep= |sum=stat.fsck.d_cx_diff:width=12:format=l:tag=e(disk-cx)|sep= |sum=stat.fsck.m_cx_diff:width=12:format=l:tag=e(mgm-cx)";
  }

  if (option == "l")
  {
    // long format
    return "header=1:member=type:width=10:format=-s|sep= |member=hostport:width=32:format=s|sep= |member=cfg.stat.geotag:width=16:format=s|sep= |member=status:width=10:format=s|sep= |member=cfg.status:width=12:format=s|sep= |member=cfg.txgw:width=6:format=s|sep= |member=heartbeatdelta:width=16:format=s|sep= |member=nofs:width=5:format=s|sep= |sum=stat.balancer.running:width=10:format=l:tag=balan-shd|sep= |sum=stat.drainer.running:width=10:format=l:tag=drain-shd|sep= |member=inqueue:width=10:format=s:tag=gw-queue";
  }

  // default format
  return "header=1:member=type:width=10:format=-s|sep= |member=hostport:width=32:format=s|sep= |member=cfg.stat.geotag:width=16:format=s|sep= |member=status:width=10:format=s|sep= |member=cfg.status:width=12:format=s|sep= |member=cfg.txgw:width=6:format=s|sep= |member=inqueue:width=10:format=s:tag=gw-queued|sep= |member=cfg.gw.ntx:width=8:format=s:tag=gw-ntx|sep= |member=cfg.gw.rate:width=8:format=s:tag=gw-rate|sep= |member=heartbeatdelta:width=16:format=s|sep= |member=nofs:width=5:format=s";
}

//------------------------------------------------------------------------------
// @brief return's the printout format for a given option
// @param option see the implementation for valid options
// @return std;:string with format line passed to the printout routine
//------------------------------------------------------------------------------
std::string
FsView::GetFileSystemFormat(std::string option)
{
  if (option == "m")
  {
    // monitoring format
    return "key=host:width=1:format=os|sep= |key=port:width=1:format=os|sep= |key=id:width=1:format=os|sep= |key=uuid:width=1:format=os|sep= |key=path:width=1:format=os|sep= |key=schedgroup:width=1:format=os|sep= |key=stat.boot:width=1:format=os|sep= |key=configstatus:width=1:format=os|sep= |key=headroom:width=1:format=os|sep= |key=stat.errc:width=1:format=os|sep= |key=stat.errmsg:width=1:format=oqs|sep= |key=stat.disk.load:width=1:format=of|sep= |key=stat.disk.readratemb:width=1:format=ol|sep= |key=stat.disk.writeratemb:width=1:format=ol|sep= |key=stat.net.ethratemib:width=1:format=ol|sep= |key=stat.net.inratemib:width=1:format=ol|sep= |key=stat.net.outratemib:width=1:format=ol|sep= |key=stat.ropen:width=1:format=ol|sep= |key=stat.wopen:width=1:format=ol|sep= |key=stat.statfs.freebytes:width=1:format=ol|sep= |key=stat.statfs.usedbytes:width=1:format=ol|sep= |key=stat.statfs.capacity:width=1:format=ol|sep= |key=stat.usedfiles:width=1:format=ol|sep= |key=stat.statfs.ffree:width=1:format=ol|sep= |key=stat.statfs.fused:width=1:format=ol|sep= |key=stat.statfs.files:width=1:format=ol|sep= |key=stat.drain:width=1:format=os|sep= |key=stat.drainprogress:width=1:format=ol:tag=progress|sep= |key=stat.drainfiles:width=1:format=ol|sep= |key=stat.drainbytesleft:width=1:format=ol|sep= |key=stat.drainretry:width=1:format=ol|sep= |key=graceperiod:width=1:format=ol|sep= |key=stat.timeleft:width=1:format=ol|sep= |key=stat.active:width=1:format=os|sep= |key=scaninterval:width=1:format=os|sep= |key=stat.balancer.running:width=1:format=ol:tag=stat.balancer.running|sep= |key=stat.drainer.running:width=1:format=ol:tag=stat.drainer.running|sep= |key=stat.disk.iops:width=1:format=ol|sep= |key=stat.disk.bw:width=1:format=of|sep= |key=stat.geotag:width=1:format=os|sep= |key=stat.health:width=1:format=os|sep= |key=stat.health.redundancy_factor:width=1:format=os|sep= |key=stat.health.drives_failed:width=1:format=os|sep= |key=stat.health.drives_total:width=1:format=os|sep= |key=stat.health.indicator:width=1:format=os";
  }

  if (option == "io")
  {
    // io format
    return "header=1:key=hostport:width=32:format=s|sep= |key=id:width=5:format=s|sep= |key=schedgroup:width=16:format=s|sep= |key=stat.geotag:width=16:format=s|sep= |key=stat.disk.load:width=10:format=f:tag=diskload|sep= |key=stat.disk.readratemb:width=12:format=+l:tag=diskr-MB/s|sep= |key=stat.disk.writeratemb:width=12:format=+l:tag=diskw-MB/s|sep= |key=stat.net.ethratemib:width=10:format=l:tag=eth-MiB/s|sep= |key=stat.net.inratemib:width=10:format=l:tag=ethi-MiB|sep= |key=stat.net.outratemib:width=10:format=l:tag=etho-MiB|sep= |key=stat.ropen:width=6:format=l:tag=ropen|sep= |key=stat.wopen:width=6:format=l:tag=wopen|sep= |key=stat.statfs.usedbytes:width=12:format=+l:unit=B:tag=used-bytes|sep= |key=stat.statfs.capacity:width=12:format=+l:unit=B:tag=max-bytes|sep= |key=stat.usedfiles:width=12:format=+l:tag=used-files|sep= |key=stat.statfs.files:width=11:format=+l:tag=max-files|sep= |key=stat.balancer.running:width=10:format=l:tag=bal-shd|sep= |key=stat.drainer.running:width=14:format=l:tag=drain-shd|key=stat.drainer:width=12:format=s:tag=drainpull|sep= |key=stat.disk.iops:width=6:format=l:tag=iops|sep= |key=stat.disk.bw:width=9:format=+l:unit=MB:tag=bw";
  }

  if (option == "fsck")
  {
    // fsck format
    return "header=1:key=hostport:width=32:format=s|sep= |key=id:width=6:format=s|sep= |key=stat.fsck.mem_n:width=8:format=l:tag=n(mem)|sep= |key=stat.fsck.d_sync_n:width=8:format=l:tag=n(disk)|sep= |key=stat.fsck.m_sync_n:width=8:format=l:tag=n(mgm)|sep= |key=stat.fsck.orphans_n:width=12:format=l:tag=e(orph)|sep= |key=stat.fsck.unreg_n:width=12:format=l:tag=e(unreg)|sep= |key=stat.fsck.rep_diff_n:width=12:format=l:tag=e(layout)|sep= |key=stat.fsck.rep_missing_n:width=12:format=l:tag=e(miss)|sep= |key=stat.fsck.d_mem_sz_diff:width=12:format=l:tag=e(disksize)|sep= |key=stat.fsck.m_mem_sz_diff:width=12:format=l:tag=e(mgmsize)|sep= |key=stat.fsck.d_cx_diff:width=12:format=l:tag=e(disk-cx)|sep= |key=stat.fsck.m_cx_diff:width=12:format=l:tag=e(mgm-cx)";
  }

  if (option == "d")
  {
    // drain format
    return "header=1:key=host:width=24:format=S:condition=stat.drain=!nodrain|sep= (|key=port:width=4:format=-s|sep=) |key=id:width=6:format=s|sep= |key=path:width=32:format=s|sep= |key=stat.drain:width=12:format=s|sep= |key=stat.drainprogress:width=12:format=l:tag=progress|sep= |key=stat.drainfiles:width=12:format=+l:tag=files|sep= |key=stat.drainbytesleft:width=12:format=+l:tag=bytes-left:unit=B|sep= |key=stat.timeleft:width=11:format=l:tag=timeleft|sep= |key=stat.drainretry:width=6:format=l:tag=retry|sep= |key=stat.wopen:width=6:format=l:tag=wopen";
  }

  if (option == "l")
  {
    // long format
    return "header=1:key=host:width=24:format=-S|sep= |key=port:width=5:format=s|sep= |key=id:width=6:format=s|sep= |key=uuid:width=36:format=s|sep= |key=path:width=32:format=s|sep= |key=schedgroup:width=16:format=s|sep= |key=headroom:width=10:format=+l|sep= |key=stat.boot:width=12:format=s|sep= |key=configstatus:width=14:format=s|sep= |key=stat.drain:width=12:format=s|sep= |key=stat.active:width=8:format=s|key=scaninterval:width=14:format=s|sep= |key=stat.health:width=16:format=s";
  }

  if (option == "e")
  {
    // error format
    return "header=1:key=host:width=24:format=S:condition=stat.errc=!0|sep= |key=id:width=6:format=s|sep= |key=path:width=32:format=s|sep= |key=stat.boot:width=12:format=s|sep= |key=configstatus:width=14:format=s|sep= |key=stat.drain:width=12:format=s|sep= |key=stat.errc:width=3:format=s|sep= |key=stat.errmsg:width=0:format=s";
  }

  // default format
  return "header=1:key=host:width=24:format=S|sep= (|key=port:width=4:format=-s|sep=) |key=id:width=6:format=s|sep= |key=path:width=32:format=s|sep= |key=schedgroup:width=16:format=s|sep= |key=stat.geotag:width=16:format=s|sep= |key=stat.boot:width=12:format=s|sep= |key=configstatus:width=14:format=s|sep= |key=stat.drain:width=12:format=s|sep= |key=stat.active:width=8:format=s|sep= |key=stat.health:width=16:format=s";
}

//------------------------------------------------------------------------------
// @brief return's the printout format for a given option
// @param option see the implementation for valid options
// @return std;:string with format line passed to the printout routine
//------------------------------------------------------------------------------
std::string
FsView::GetSpaceFormat(std::string option)
{
  if (option == "m")
  {
    // monitoring format
    return "member=type:width=1:format=os|sep= |member=name:width=1:format=os|sep= |member=cfg.groupsize:width=1:format=ol|sep= |member=cfg.groupmod:width=1:format=ol|sep= |member=nofs:width=1:format=ol|sep= |avg=stat.disk.load:width=1:format=of|sep= |sig=stat.disk.load:width=1:format=of|sep= |sum=stat.disk.readratemb:width=1:format=ol|sep= |sum=stat.disk.writeratemb:width=1:format=ol|sep= |sum=stat.net.ethratemib:width=1:format=ol|sep= |sum=stat.net.inratemib:width=1:format=ol|sep= |sum=stat.net.outratemib:width=1:format=ol|sep= |sum=stat.ropen:width=1:format=ol|sep= |sum=stat.wopen:width=1:format=ol|sep= |sum=stat.statfs.usedbytes:width=1:format=ol|sep= |sum=stat.statfs.freebytes:width=1:format=ol|sep= |sum=stat.statfs.capacity:width=1:format=ol|sep= |sum=stat.usedfiles:width=1:format=ol|sep= |sum=stat.statfs.ffiles:width=1:format=ol|sep= |sum=stat.statfs.files:width=1:format=ol|sep= |sum=stat.statfs.capacity?configstatus@rw:width=1:format=ol|sep= |sum=<n>?configstatus@rw:width=1:format=ol|sep= |member=cfg.quota:width=1:format=os|sep= |member=cfg.nominalsize:width=1:format=ol|sep= |member=cfg.balancer:width=1:format=os|sep= |member=cfg.balancer.threshold:width=1:format=ol|sep= |sum=stat.balancer.running:width=1:format=ol:tag=stat.balancer.running|sep= |sum=stat.drainer.running:width=1:format=ol:tag=stat.drainer.running|sep= |sum=stat.disk.iops?configstatus@rw:width=1:format=ol|sep= |sum=stat.disk.bw?configstatus@rw:width=1:format=ol";
  }

  if (option == "io")
  {
    return "header=1:member=name:width=10:format=s|sep= |avg=stat.geotag:width=32:format=s|sep= |avg=stat.disk.load:width=10:format=f:tag=diskload|sep= |sum=stat.disk.readratemb:width=12:format=+l:tag=diskr-MB/s|sep= |sum=stat.disk.writeratemb:width=12:format=+l:tag=diskw-MB/s|sep= |sum=stat.net.ethratemib:width=10:format=l:tag=eth-MiB/s|sep= |sum=stat.net.inratemib:width=10:format=l:tag=ethi-MiB|sep= |sum=stat.net.outratemib:width=10:format=l:tag=etho-MiB|sep= |sum=stat.ropen:width=6:format=l:tag=ropen|sep= |sum=stat.wopen:width=6:format=l:tag=wopen|sep= |sum=stat.statfs.usedbytes:width=12:format=+l:unit=B:tag=used-bytes|sep= |sum=stat.statfs.capacity:width=12:format=+l:unit=B:tag=max-bytes|sep= |sum=stat.usedfiles:width=12:format=+l:tag=used-files|sep= |sum=stat.statfs.files:width=11:format=+l:tag=max-files|sep= |sum=stat.balancer.running:width=10:format=l:tag=bal-shd|sep= |sum=stat.drainer.running:width=10:format=l:tag=drain-shd";
  }

  if (option == "fsck")
  {
    return "header=1:member=name:width=10:format=s|sep= |avg=stat.geotag:width=32:format=s|sep= |sum=stat.fsck.mem_n:width=8:format=l:tag=n(mem)|sep= |sum=stat.fsck.d_sync_n:width=8:format=l:tag=n(disk)|sep= |sum=stat.fsck.m_sync_n:width=8:format=l:tag=n(mgm)|sep= |sum=stat.fsck.orphans_n:width=12:format=l:tag=e(orph)|sep= |sum=stat.fsck.unreg_n:width=12:format=l:tag=e(unreg)|sep= |sum=stat.fsck.rep_diff_n:width=12:format=l:tag=e(layout)|sep= |sum=stat.fsck.rep_missing_n:width=12:format=l:tag=e(miss)|sep= |sum=stat.fsck.d_mem_sz_diff:width=12:format=l:tag=e(disksize)|sep= |sum=stat.fsck.m_mem_sz_diff:width=12:format=l:tag=e(mgmsize)|sep= |sum=stat.fsck.d_cx_diff:width=12:format=l:tag=e(disk-cx)|sep= |sum=stat.fsck.m_cx_diff:width=12:format=l:tag=e(mgm-cx)";
  }

  if (option == "l")
  {
    // long output formag
    return "header=1:member=type:width=10:format=-s|sep= |member=name:width=16:format=s|sep= |avg=stat.geotag:width=32:format=s|sep= |member=cfg.groupsize:width=12:format=s|sep= |member=cfg.groupmod:width=12:format=s|sep= |sum=<n>?*@*:width=6:format=l:tag=N(fs)|sep= |sum=<n>?configstatus@rw:width=9:format=l:tag=N(fs-rw)|sep= |sum=stat.statfs.usedbytes:width=15:format=+l|sep= |sum=stat.statfs.capacity:width=14:format=+l|sep= |sum=stat.statfs.capacity?configstatus@rw:width=13:format=+l:tag=capacity(rw)|sep= |member=cfg.nominalsize:width=13:format=+l:tag=nom.capacity|sep= |member=cfg.quota:width=6:format=s";
  }

  return "header=1:member=type:width=10:format=-s|sep= |member=name:width=16:format=s|sep= |avg=stat.geotag:width=32:format=s|sep= |member=cfg.groupsize:width=12:format=s|sep= |member=cfg.groupmod:width=12:format=s|sep= |member=nofs:width=6:format=s:tag=N(fs)|sep= |sum=<n>?configstatus@rw:width=9:format=l:tag=N(fs-rw)|sep= |sum=stat.statfs.usedbytes:width=15:format=+l|sep= |sum=stat.statfs.capacity:width=14:format=+l|sep= |sum=stat.statfs.capacity?configstatus@rw:width=13:format=+l:tag=capacity(rw)|sep= |member=cfg.nominalsize:width=13:format=+l:tag=nom.capacity|sep= |member=cfg.quota:width=6:format=s|sep= |member=cfg.balancer:width=10:format=s:tag=balancing|sep= |member=cfg.balancer.threshold:width=11:format=+l:tag=threshold|sep= |member=cfg.converter:width=11:format=s:tag=converter|sep= |member=cfg.converter.ntx:width=6:format=+l:tag=ntx|sep= |member=cfg.stat.converter.active:width=8:format=+l:tag=active|sep= |member=cfg.wfe:width=11:format=s:tag=wfe|sep= |member=cfg.wfe.ntx:width=6:format=+l:tag=ntx|sep= |member=cfg.stat.wfe.active:width=8:format=+l:tag=active|sep= |member=cfg.groupbalancer:width=11:format=s:tag=intergroup|";
}

//------------------------------------------------------------------------------
// @brief return's the printout format for a given option
// @param option see the implementation for valid options
// @return std;:string with format line passed to the printout routine
//------------------------------------------------------------------------------
std::string
FsView::GetGroupFormat(std::string option)
{
  if (option == "m")
  {
    // monitoring format
    return "member=type:width=1:format=os|sep= |member=name:width=1:format=os|sep= |member=cfg.status:width=1:format=os|sep= |member=nofs:width=1:format=os|sep= |avg=stat.disk.load:width=1:format=of|sep= |sig=stat.disk.load:width=1:format=of|sep= |sum=stat.disk.readratemb:width=1:format=ol|sep= |sum=stat.disk.writeratemb:width=1:format=ol|sep= |sum=stat.net.ethratemib:width=1:format=ol|sep= |sum=stat.net.inratemib:width=1:format=ol|sep= |sum=stat.net.outratemib:width=1:format=ol|sep= |sum=stat.ropen:width=1:format=ol|sep= |sum=stat.wopen:width=1:format=ol|sep= |sum=stat.statfs.usedbytes:width=1:format=ol|sep= |sum=stat.statfs.freebytes:width=1:format=ol|sep= |sum=stat.statfs.capacity:width=1:format=ol|sep= |sum=stat.usedfiles:width=1:format=ol|sep= |sum=stat.statfs.ffree:width=1:format=ol|sep= |sum=stat.statfs.files:width=1:format=ol|sep= |maxdev=stat.statfs.filled:width=1:format=of|sep= |avg=stat.statfs.filled:width=1:format=of|sep= |sig=stat.statfs.filled:width=1:format=of|sep= |member=cfg.stat.balancing:width=1:format=os:tag=stat.balancing|sep= |sum=stat.balancer.running:width=1:format=ol:tag=stat.balancer.running|sep= |sum=stat.drainer.running:width=1:format=ol:tag=stat.drainer.running";
  }

  if (option == "io")
  {
    // io format
    return "header=1:member=name:width=16:format=-s|sep= |avg=stat.geotag:width=32:format=s|sep= |avg=stat.disk.load:width=10:format=f:tag=diskload|sep= |sum=stat.disk.readratemb:width=12:format=+l:tag=diskr-MB/s|sep= |sum=stat.disk.writeratemb:width=12:format=+l:tag=diskw-MB/s|sep= |sum=stat.net.ethratemib:width=10:format=l:tag=eth-MiB/s|sep= |sum=stat.net.inratemib:width=10:format=l:tag=ethi-MiB|sep= |sum=stat.net.outratemib:width=10:format=l:tag=etho-MiB|sep= |sum=stat.ropen:width=6:format=l:tag=ropen|sep= |sum=stat.wopen:width=6:format=l:tag=wopen|sep= |sum=stat.statfs.usedbytes:width=12:format=+l:unit=B:tag=used-bytes|sep= |sum=stat.statfs.capacity:width=12:format=+l:unit=B:tag=max-bytes|sep= |sum=stat.usedfiles:width=12:format=+l:tag=used-files|sep= |sum=stat.statfs.files:width=11:format=+l:tag=max-files|sep= |sum=stat.balancer.running:width=10:format=l:tag=bal-shd|sep= |sum=stat.drainer.running:width=10:format=l:tag=drain-shd";
  }

  if (option == "l")
  {
    // long format
    return "header=1:member=type:width=10:format=-s|sep= |member=name:width=16:format=s|sep= |member=cfg.status:width=12:format=s|sep= |avg=stat.geotag:width=32:format=s|sep= |key=stat.geotag:width=16:format=s|sep= |sum=<n>?*@*:width=6:format=l:tag=N(fs)";
  }

  // default format
  return "header=1:member=type:width=10:format=-s|sep= |member=name:width=16:format=-s|sep= |member=cfg.status:width=12:format=s|sep= |avg=stat.geotag:width=32:format=s|sep= |sum=<n>?*@*:width=6:format=l:tag=N(fs)|sep= |maxdev=stat.statfs.filled:width=12:format=f:unit=p|sep= |avg=stat.statfs.filled:width=12:format=f:unit=p|sep= |sig=stat.statfs.filled:width=12:format=f:unit=p|sep= |member=cfg.stat.balancing:width=10:format=-s|sep= |sum=stat.balancer.running:width=10:format=l:tag=bal-shd|sep= |sum=stat.drainer.running:width=10:format=l:tag=drain-shd";
}

//------------------------------------------------------------------------------
// @brief register a filesystem object in the filesystem view
// @param fs filesystem to register
// @return true if done, otherwise false
//------------------------------------------------------------------------------
bool
FsView::Register(FileSystem* fs, bool registerInGeoTreeEngine)
{
  if (!fs)
    return false;

  // Create a snapshot of the current variables of the fs
  eos::common::FileSystem::fs_snapshot snapshot;

  if (fs->SnapShotFileSystem(snapshot))
  {
    // Align view by filesystem object and filesystem id
    // Check if there is already a filesystem with the same path on the same node
    if (mNodeView.count(snapshot.mQueue))
    {
      // Loop over all attached filesystems and compare the queue path
      for (auto it = mNodeView[snapshot.mQueue]->begin();
           it != mNodeView[snapshot.mQueue]->end(); it++)
      {
        if (FsView::gFsView.mIdView[*it]->GetQueuePath() == snapshot.mQueuePath)
        {
          // This queuepath already exists, we cannot register
          return false;
        }
      }
    }

    // Check if this is already in the view
    if (mFileSystemView.count(fs))
    {
      // This filesystem is already there, this might be an update
      eos::common::FileSystem::fsid_t fsid = mFileSystemView[fs];

      if (fsid != snapshot.mId)
      {
        // Remove previous mapping
        mIdView.erase(fsid);
        // Setup new two way mapping
        mFileSystemView[fs] = snapshot.mId;
        mIdView[snapshot.mId] = fs;
        eos_debug("updating mapping %u<=>%lld", snapshot.mId, fs);
      }
    }
    else
    {
      mFileSystemView[fs] = snapshot.mId;
      mIdView[snapshot.mId] = fs;
      eos_debug("registering mapping %u<=>%lld", snapshot.mId, fs);
    }

    // Align view by nodename (= MQ queue) e.g. /eos/<host>:<port>/fst
    // Check if we have already a node view
    if (mNodeView.count(snapshot.mQueue))
    {
      mNodeView[snapshot.mQueue]->insert(snapshot.mId);
      eos_debug("inserting into node view %s<=>%u", snapshot.mQueue.c_str(),
                snapshot.mId, fs);
    }
    else
    {
      FsNode* node = new FsNode(snapshot.mQueue.c_str());
      mNodeView[snapshot.mQueue] = node;
      node->insert(snapshot.mId);
      node->SetNodeConfigDefault();
      eos_debug("creating/inserting into node view %s<=>%u", snapshot.mQueue.c_str(),
                snapshot.mId, fs);
    }

    // Align view by groupname
    // Check if we have already a group view
    if (mGroupView.count(snapshot.mGroup))
    {
      mGroupView[snapshot.mGroup]->insert(snapshot.mId);
      eos_debug("inserting into group view %s<=>%u", snapshot.mGroup.c_str(),
                snapshot.mId, fs);
    }
    else
    {
      FsGroup* group = new FsGroup(snapshot.mGroup.c_str());
      mGroupView[snapshot.mGroup] = group;
      group->insert(snapshot.mId);
      group->mIndex = snapshot.mGroupIndex;
      eos_debug("creating/inserting into group view %s<=>%u", snapshot.mGroup.c_str(),
                snapshot.mId, fs);
    }

#ifndef EOSMGMFSVIEWTEST

    if (registerInGeoTreeEngine &&
        !gGeoTreeEngine.insertFsIntoGroup(fs, mGroupView[snapshot.mGroup], false))
    {
      // Roll back the changes
      if (UnRegister(fs, false))
      {
        eos_err("could not insert insert fs %u into GeoTreeEngine : fs was "
                "unregistered and consistency is KEPT between FsView and "
                "GeoTreeEngine", snapshot.mId);
      }
      else
      {
        eos_crit("could not insert insert fs %u into GeoTreeEngine : fs could "
                 "not be unregistered and consistency is BROKEN between FsView "
                 "and GeoTreeEngine", snapshot.mId);
      }

      return false;
    }

#endif
    mSpaceGroupView[snapshot.mSpace].insert(mGroupView[snapshot.mGroup]);

    // Align view by spacename
    // Check if we have already a space view
    if (mSpaceView.count(snapshot.mSpace))
    {
      mSpaceView[snapshot.mSpace]->insert(snapshot.mId);
      eos_debug("inserting into space view %s<=>%u %x", snapshot.mSpace.c_str(),
                snapshot.mId, fs);
    }
    else
    {
      FsSpace* space = new FsSpace(snapshot.mSpace.c_str());
      mSpaceView[snapshot.mSpace] = space;
      space->insert(snapshot.mId);
      eos_debug("creating/inserting into space view %s<=>%u %x",
                snapshot.mSpace.c_str(),
                snapshot.mId, fs);
    }
  }

  StoreFsConfig(fs);
  return true;
}

//------------------------------------------------------------------------------
// @brief Store the filesystem configuration in the configuration engine
// @param fs filesystem object to store
//------------------------------------------------------------------------------
void
FsView::StoreFsConfig(FileSystem* fs)
{
#ifndef EOSMGMFSVIEWTEST

  if (fs)
  {
    std::string key, val;
    fs->CreateConfig(key, val);

    if (FsView::ConfEngine)
      FsView::ConfEngine->SetConfigValue("fs", key.c_str(), val.c_str());
  }

#endif
  return;
}

//------------------------------------------------------------------------------
// @brief Move a filesystem in to a target group
// @param fs filesystem object to move
// @param group target group
// @return true if moved otherwise false
//------------------------------------------------------------------------------
bool
FsView::MoveGroup(FileSystem* fs, std::string group)
{
  if (!fs)
    return false;

  eos::common::FileSystem::fs_snapshot snapshot1;
  eos::common::FileSystem::fs_snapshot snapshot;

  if (fs->SnapShotFileSystem(snapshot1))
  {
#ifndef EOSMGMFSVIEWTEST
    fs->SetString("schedgroup", group.c_str());
    FsGroup* oldgroup = mGroupView.count(snapshot1.mGroup) ?
        mGroupView[snapshot1.mGroup] : NULL;
#endif

    if (fs->SnapShotFileSystem(snapshot))
    {
      // Remove from the original space
      if (mSpaceView.count(snapshot1.mSpace))
      {
        FsSpace* space = mSpaceView[snapshot1.mSpace];
        space->erase(snapshot1.mId);
        eos_debug("unregister space %s from space view",
                  space->GetMember("name").c_str());

        if (!space->size())
        {
          mSpaceView.erase(snapshot1.mSpace);
          delete space;
        }
      }

      // Remove from the original group
      if (mGroupView.count(snapshot1.mGroup))
      {
        FsGroup* group = mGroupView[snapshot1.mGroup];
#ifndef EOSMGMFSVIEWTEST

        if (!gGeoTreeEngine.removeFsFromGroup(fs, group, false))
        {
          // roll-back
          if (mSpaceView.count(snapshot1.mSpace))
          {
            mSpaceView[snapshot1.mSpace]->insert(snapshot1.mId);
            eos_debug("inserting into space view %s<=>%u %x",
                      snapshot1.mSpace.c_str(), snapshot1.mId, fs);
          }
          else
          {
            FsSpace* space = new FsSpace(snapshot1.mSpace.c_str());
            mSpaceView[snapshot1.mSpace] = space;
            space->insert(snapshot1.mId);
            eos_debug("creating/inserting into space view %s<=>%u %x",
                      snapshot1.mSpace.c_str(), snapshot1.mId, fs);
          }

          eos_err("could not remove fs %u from GeoTreeEngine : fs was "
                  "registered back and consistency is KEPT between FsView"
                  " and GeoTreeEngine", snapshot.mId);
          return false;
        }

#endif
        group->erase(snapshot1.mId);
        eos_debug("unregister group %s from group view",
                  group->GetMember("name").c_str());

        if (!group->size())
        {
          if (mSpaceGroupView.count(snapshot1.mSpace))
          {
            mSpaceGroupView[snapshot1.mSpace].erase(mGroupView[snapshot1.mGroup]);
          }

          mGroupView.erase(snapshot1.mGroup);
          delete group;
        }
      }

      // Check if we have already a group view
      if (mGroupView.count(snapshot.mGroup))
      {
        mGroupView[snapshot.mGroup]->insert(snapshot.mId);
        eos_debug("inserting into group view %s<=>%u",
                  snapshot.mGroup.c_str(), snapshot.mId, fs);
      }
      else
      {
        FsGroup* group = new FsGroup(snapshot.mGroup.c_str());
        mGroupView[snapshot.mGroup] = group;
        group->insert(snapshot.mId);
        group->mIndex = snapshot.mGroupIndex;
        eos_debug("creating/inserting into group view %s<=>%u",
                  snapshot.mGroup.c_str(), snapshot.mId, fs);
      }

#ifndef EOSMGMFSVIEWTEST

      if (!gGeoTreeEngine.insertFsIntoGroup(fs, mGroupView[group], false))
      {
        if (fs->SetString("schedgroup", group.c_str()) && UnRegister(fs, false))
        {
          if (oldgroup && fs->SetString("schedgroup", oldgroup->mName.c_str())
              && Register(fs))
          {
            eos_err("while moving fs, could not insert fs %u in group %s. fs "
                    "was registered back to group %s and consistency is KEPT "
                    "between FsView and GeoTreeEngine",
                    snapshot.mId, mGroupView[group]->mName.c_str(),
                    oldgroup->mName.c_str());
          }
          else
          {
            eos_err("while moving fs, could not insert fs %u in group %s. fs "
                    "was unregistered and consistency is KEPT between FsView "
                    "and GeoTreeEngine", snapshot.mId, mGroupView[group]->mName.c_str());
          }
        }
        else
        {
          eos_crit("while moving fs, could not insert fs %u in group %s. fs "
                   "could not be unregistered and consistency is BROKEN between "
                   "FsView and GeoTreeEngine", snapshot.mId, mGroupView[group]->mName.c_str());
        }

        return false;
      }

#endif
      mSpaceGroupView[snapshot.mSpace].insert(mGroupView[snapshot.mGroup]);

      // Check if we have already a space view
      if (mSpaceView.count(snapshot.mSpace))
      {
        mSpaceView[snapshot.mSpace]->insert(snapshot.mId);
        eos_debug("inserting into space view %s<=>%u %x",
                  snapshot.mSpace.c_str(), snapshot.mId, fs);
      }
      else
      {
        FsSpace* space = new FsSpace(snapshot.mSpace.c_str());
        mSpaceView[snapshot.mSpace] = space;
        space->insert(snapshot.mId);
        eos_debug("creating/inserting into space view %s<=>%u %x",
                  snapshot.mSpace.c_str(), snapshot.mId, fs);
      }

      StoreFsConfig(fs);
      return true;
    }
  }

  return false;
}

//------------------------------------------------------------------------------
// Unregister a filesystem from the filesystem view
// @param fs filesystem to unregister
// @return true if done otherwise false
//------------------------------------------------------------------------------
bool
FsView::UnRegister(FileSystem* fs, bool unregisterInGeoTreeEngine)
{
  if (!fs)
    return false;

#ifndef EOSMGMFSVIEWTEST
  // Delete in the configuration engine
  std::string key = fs->GetQueuePath();

  if (FsView::ConfEngine)
    FsView::ConfEngine->DeleteConfigValue("fs", key.c_str());

#endif
  // Create a snapshot of the current variables of the fs
  eos::common::FileSystem::fs_snapshot snapshot;

  if (fs->SnapShotFileSystem(snapshot))
  {
    // Remove view by filesystem object and filesystem id
    // Check if this is in the view
    if (mFileSystemView.count(fs))
    {
      mFileSystemView.erase(fs);
      mIdView.erase(snapshot.mId);
      eos_debug("unregister %lld from filesystem view", fs);
    }

    // Remove fs from node view & evt. remove node view
    if (mNodeView.count(snapshot.mQueue))
    {
      FsNode* node = mNodeView[snapshot.mQueue];
      node->erase(snapshot.mId);
      eos_debug("unregister node %s from node view", node->GetMember("name").c_str());

      if (!node->size())
      {
        mNodeView.erase(snapshot.mQueue);
        delete node;
      }
    }

    // Remove fs from group view & evt. remove group view
    if (mGroupView.count(snapshot.mGroup))
    {
      FsGroup* group = mGroupView[snapshot.mGroup];
#ifndef EOSMGMFSVIEWTEST

      if (unregisterInGeoTreeEngine
          && !gGeoTreeEngine.removeFsFromGroup(fs, group, false))
      {
        if (Register(fs, false))
          eos_err("could not remove fs %u from GeoTreeEngine : fs was "
                  "registered back and consistency is KEPT between FsView "
                  "and GeoTreeEngine", snapshot.mId);
        else
          eos_crit("could not remove fs %u from GeoTreeEngine : fs could not "
                   "be registered back and consistency is BROKEN between "
                   "FsView and GeoTreeEngine", snapshot.mId);

        return false;
      }

#endif
      group->erase(snapshot.mId);
      eos_debug("unregister group %s from group view",
                group->GetMember("name").c_str());

      if (!group->size())
      {
        mSpaceGroupView[snapshot.mSpace].erase(mGroupView[snapshot.mGroup]);
        mGroupView.erase(snapshot.mGroup);
        delete group;
      }
    }

    // Remove fs from space view & evt. remove space view
    if (mSpaceView.count(snapshot.mSpace))
    {
      FsSpace* space = mSpaceView[snapshot.mSpace];
      space->erase(snapshot.mId);
      eos_debug("unregister space %s from space view",
                space->GetMember("name").c_str());

      if (!space->size())
      {
        mSpaceView.erase(snapshot.mSpace);
        delete space;
      }
    }

    // Remove mapping
    RemoveMapping(snapshot.mId, snapshot.mUuid);
    delete fs;
    return true;
  }

  return false;
}

//------------------------------------------------------------------------------
// Checks if a node has already a filesystem registered
//------------------------------------------------------------------------------
bool
FsView::ExistsQueue(std::string queue, std::string queuepath)
{
  if (mNodeView.count(queue))
  {
    // Loop over all attached filesystems and compare the queue path
    for (auto it = mNodeView[queue]->begin(); it != mNodeView[queue]->end(); it++)
    {
      if (FsView::gFsView.mIdView[*it]->GetQueuePath() == queuepath)
      {
        // This queupath exists already, we cannot register
        return true;
      }
    }
  }

  return false;
}

//------------------------------------------------------------------------------
// Add view by nodename (= MQ queue) e.g. /eos/<host>:<port>/fst
//------------------------------------------------------------------------------
bool
FsView::RegisterNode(const char* nodename)
{
  std::string nodequeue = nodename;

  if (mNodeView.count(nodequeue))
  {
    eos_debug("node is existing");
    return false;
  }
  else
  {
    FsNode* node = new FsNode(nodequeue.c_str());
    mNodeView[nodequeue] = node;
    node->SetNodeConfigDefault();
    eos_debug("creating node view %s", nodequeue.c_str());
    return true;
  }
}

//------------------------------------------------------------------------------
// Remove all nodes
//------------------------------------------------------------------------------
void
FsView::UnRegisterNodes()
{
  for (auto it = mNodeView.begin(); it != mNodeView.end(); it++)
    delete(it->second);
}

//------------------------------------------------------------------------------
// Remove view by nodename (= MQ queue) e.g. /eos/<host>:<port>/fst
//------------------------------------------------------------------------------
bool
FsView::UnRegisterNode(const char* nodename)
{
  // We have to remove all the connected filesystems via UnRegister(fs) to keep
  // space, group, node view in sync
  bool retc = true;
  bool hasfs = false;

  if (mNodeView.count(nodename))
  {
    while (mNodeView.count(nodename) &&
           (mNodeView[nodename]->begin() != mNodeView[nodename]->end()))
    {
      eos::common::FileSystem::fsid_t fsid = *(mNodeView[nodename]->begin());
      FileSystem* fs = mIdView[fsid];

      if (fs)
      {
        hasfs = true;
        eos_static_debug("Unregister filesystem fsid=%llu node=%s queue=%s",
                         (unsigned long long) fsid, nodename, fs->GetQueue().c_str());
        retc |= UnRegister(fs);
      }
    }

    if (!hasfs)
    {
      // We have to explicitly remove the node from the view here because no fs
      // was removed
      delete mNodeView[nodename];
      retc = (mNodeView.erase(nodename) ? true : false);
    }
  }

  return retc;
}

//------------------------------------------------------------------------------
// Add view by spacename (= MQ queue) e.g. /eos/<host>:<port>/fst
//------------------------------------------------------------------------------
bool
FsView::RegisterSpace(const char* spacename)
{
  std::string spacequeue = spacename;

  if (mSpaceView.count(spacequeue))
  {
    eos_debug("space is existing");
    return false;
  }
  else
  {
    FsSpace* space = new FsSpace(spacequeue.c_str());
    mSpaceView[spacequeue] = space;
    eos_debug("creating space view %s", spacequeue.c_str());
    return true;
  }
}

//------------------------------------------------------------------------------
// Remove view by spacename (= MQ queue) e.g. /eos/<host>:<port>/fst
//------------------------------------------------------------------------------
bool
FsView::UnRegisterSpace(const char* spacename)
{
  // We have to remove all the connected filesystems via UnRegister(fs) to keep
  // space, group, space view in sync
  bool retc = true;
  bool hasfs = false;

  if (mSpaceView.count(spacename))
  {
    while (mSpaceView.count(spacename) &&
           (mSpaceView[spacename]->begin() != mSpaceView[spacename]->end()))
    {
      eos::common::FileSystem::fsid_t fsid = *(mSpaceView[spacename]->begin());
      FileSystem* fs = mIdView[fsid];

      if (fs)
      {
        hasfs = true;
        eos_static_debug("Unregister filesystem fsid=%llu space=%s queue=%s",
                         (unsigned long long) fsid, spacename, fs->GetQueue().c_str());
        retc |= UnRegister(fs);
      }
    }

    if (!hasfs)
    {
      // We have to explicitly remove the space from the view here because no
      // fs was removed
      delete mSpaceView[spacename];
      retc = (mSpaceView.erase(spacename) ? true : false);
    }
  }

  return retc;
}

//------------------------------------------------------------------------------
// Add view by groupname  e.g. default or default.0
//------------------------------------------------------------------------------
bool
FsView::RegisterGroup(const char* groupname)
{
  std::string groupqueue = groupname;

  if (mGroupView.count(groupqueue))
  {
    eos_debug("group is existing");
    return false;
  }
  else
  {
    FsGroup* group = new FsGroup(groupqueue.c_str());
    mGroupView[groupqueue] = group;
    eos_debug("creating group view %s", groupqueue.c_str());
    return true;
  }
}

//------------------------------------------------------------------------------
// Remove view by groupname e.g. default or default.0
//------------------------------------------------------------------------------
bool
FsView::UnRegisterGroup(const char* groupname)
{
  // We have to remove all the connected filesystems via UnRegister(fs) to keep
  // the group view in sync.
  bool retc = true;
  bool hasfs = false;

  if (mGroupView.count(groupname))
  {
    while (mGroupView.count(groupname) &&
           (mGroupView[groupname]->begin() != mGroupView[groupname]->end()))
    {
      eos::common::FileSystem::fsid_t fsid = *(mGroupView[groupname]->begin());
      // TODO: this looks wrong
      FileSystem* fs = mIdView[fsid];

      if (fs)
      {
        hasfs = true;
        eos_static_debug("Unregister filesystem fsid=%llu group=%s queue=%s",
                         (unsigned long long) fsid, groupname, fs->GetQueue().c_str());
        retc |= UnRegister(fs);
      }
    }

    if (!hasfs)
    {
      std::string sgroupname = groupname;
      std::string spacename = "";
      std::string index = "";

      // remove the direct group reference here
      if (mSpaceGroupView.count(spacename))
      {
        mSpaceGroupView[spacename].erase(mGroupView[groupname]);
      }

      // We have to explicitly remove the group from the view here because no
      // fs was removed
      delete mGroupView[groupname];
      retc = (mGroupView.erase(groupname) ? true : false);
      eos::common::StringConversion::SplitByPoint(groupname, spacename, index);
    }
  }

  return retc;
}

//------------------------------------------------------------------------------
// Remove all filesystems by erasing all spaces
//------------------------------------------------------------------------------
void
FsView::Reset()
{
  {
    eos::common::RWMutexReadLock viewlock(ViewMutex);

    // stop all the threads having only a read-lock
    for (auto it = mSpaceView.begin(); it != mSpaceView.end(); it++)
      it->second->Stop();
  }
  eos::common::RWMutexWriteLock viewlock(ViewMutex);

  while (mSpaceView.size())
  {
    UnRegisterSpace(mSpaceView.begin()->first.c_str());
  }

  //  UnRegisterNodes();
  eos::common::RWMutexWriteLock maplock(MapMutex);
  // Remove all mappins
  Fs2UuidMap.clear();
  Uuid2FsMap.clear();
  SetNextFsId(0);
  // Although this shouldn't be necessary, better run an additional cleanup
  mSpaceView.clear();
  mGroupView.clear();
  mNodeView.clear();
  {
    eos::common::RWMutexWriteLock gwlock(GwMutex);
    mGwNodes.clear();
  }
  mIdView.clear();
  mFileSystemView.clear();
}

//------------------------------------------------------------------------------
// Stores the next fsid into the global config
//------------------------------------------------------------------------------
void
FsView::SetNextFsId(eos::common::FileSystem::fsid_t fsid)
{
  NextFsId = fsid;
  std::string key = "nextfsid";
  char value[1024];
  snprintf(value, sizeof(value) - 1, "%llu", (unsigned long long) fsid);
  std::string svalue = value;
#ifndef EOSMGMFSVIEWTEST

  if (!SetGlobalConfig(key, value))
  {
    eos_static_err("unable to set nextfsid in global config");
  }

#endif
}

//------------------------------------------------------------------------------
// Find a filesystem specifying a queuepath
//------------------------------------------------------------------------------
FileSystem*
FsView::FindByQueuePath(std::string& queuepath)
{
  // Needs an external ViewMutex lock !!!!
  for (auto it = mIdView.begin(); it != mIdView.end(); it++)
  {
    if (it->second->GetQueuePath() == queuepath)
      return it->second;
  }

  return 0;
}

#ifndef EOSMGMFSVIEWTEST

//------------------------------------------------------------------------------
// SetGlobalConfig
//------------------------------------------------------------------------------
bool
FsView::SetGlobalConfig(std::string key, std::string value)
{
  // We need to store this in the shared hash between MGMs
  XrdMqRWMutexReadLock lock(eos::common::GlobalConfig::gConfig.SOM()->HashMutex);
  XrdMqSharedHash* hash = eos::common::GlobalConfig::gConfig.Get(
      MgmConfigQueueName.c_str());

  if (hash)
  {
    hash->Set(key, value);
  }

#ifndef EOSMGMFSVIEWTEST
  // register in the configuration engine
  std::string ckey = MgmConfigQueueName.c_str();
  ckey += "#";
  ckey += key;

  if (FsView::ConfEngine)
    FsView::ConfEngine->SetConfigValue("global", ckey.c_str(), value.c_str());

#endif
  return true;
}

//------------------------------------------------------------------------------
// GetGlobalConfig
//------------------------------------------------------------------------------
std::string
FsView::GetGlobalConfig(std::string key)
{
  XrdMqRWMutexReadLock lock(eos::common::GlobalConfig::gConfig.SOM()->HashMutex);
  XrdMqSharedHash* hash = eos::common::GlobalConfig::gConfig.Get(
      MgmConfigQueueName.c_str());

  if (hash)
  {
    return hash->Get(key);
  }

  return "";
}

#endif

//------------------------------------------------------------------------------
// Static thread startup function calling HeartBeatCheck
//------------------------------------------------------------------------------
void*
FsView::StaticHeartBeatCheck(void* arg)
{
  return reinterpret_cast<FsView*>(arg)->HeartBeatCheck();
}

//------------------------------------------------------------------------------
// Heart beat checker set's filesystem to down if the heart beat is missing
//------------------------------------------------------------------------------
void*
FsView::HeartBeatCheck()
{
  XrdSysThread::SetCancelOn();

  while (1)
  {
    {
      // quickly go through all heartbeats
      eos::common::RWMutexReadLock lock(ViewMutex);
      // iterator over all filesystems
      for (auto it = mIdView.begin(); it != mIdView.end(); it++)
      {
	if (!it->second)
	continue;
	eos::common::FileSystem::fs_snapshot_t snapshot;
	snapshot.mHeartBeatTime = (time_t) it->second->GetLongLong("stat.heartbeattime");

	if (!it->second->HasHeartBeat(snapshot))
	{
	  // mark as offline
	  if (it->second->GetActiveStatus() != eos::common::FileSystem::kOffline)
	  it->second->SetActiveStatus(eos::common::FileSystem::kOffline);
	}
	else
	{
	  std::string queue = it->second->GetString("queue");
	  std::string group = it->second->GetString("schedgroup");

	  if ((FsView::gFsView.mNodeView.count(queue)) &&
	      (FsView::gFsView.mGroupView.count(group)) &&
	      (FsView::gFsView.mNodeView[queue]->GetConfigMember("status") == "on") &&
	      (FsView::gFsView.mGroupView[group]->GetConfigMember("status") == "on"))
	  {
	    if (it->second->GetActiveStatus() != eos::common::FileSystem::kOnline)
	    it->second->SetActiveStatus(eos::common::FileSystem::kOnline);
	  }
	  else
	  {
	    if (it->second->GetActiveStatus() != eos::common::FileSystem::kOffline)
	    it->second->SetActiveStatus(eos::common::FileSystem::kOffline);
	  }
	}
      }
      // iterator over all filesystems
      for (auto it = mNodeView.begin(); it != mNodeView.end(); it++)
      {
        if (!it->second)
        continue;

        eos::common::FileSystem::host_snapshot_t snapshot;
        auto shbt = it->second->GetMember("stat.heartbeattime");
        snapshot.mHeartBeatTime = (time_t) strtoll(shbt.c_str(),NULL,10);

        if (!it->second->HasHeartBeat(snapshot))
        {
          // mark as offline
          if (it->second->GetActiveStatus() != eos::common::FileSystem::kOffline)
          it->second->SetActiveStatus(eos::common::FileSystem::kOffline);
        }
        else
        {
          std::string queue = it->second->mName;

          if ((FsView::gFsView.mNodeView.count(queue)) &&
              (FsView::gFsView.mNodeView[queue]->GetConfigMember("status") == "on") )
          {
            if (it->second->GetActiveStatus() != eos::common::FileSystem::kOnline)
            it->second->SetActiveStatus(eos::common::FileSystem::kOnline);
          }
          else
          {
            if (it->second->GetActiveStatus() != eos::common::FileSystem::kOffline)
            it->second->SetActiveStatus(eos::common::FileSystem::kOffline);
          }
        }
      }
    }
    XrdSysTimer sleeper;
    sleeper.Snooze(10);
    XrdSysThread::CancelPoint();
  }

  return 0;
}

//------------------------------------------------------------------------------
// Return a view member variable
//------------------------------------------------------------------------------
std::string
BaseView::GetMember(std::string member)
{
  if (member == "name") return mName;

  if (member == "type") return mType;

  if (member == "nofs")
  {
    char line[1024];
    snprintf(line, sizeof(line) - 1, "%llu", (unsigned long long) size());
    mSize = line;
    return mSize;
  }

  if (member == "inqueue")
  {
    XrdOucString s = "";
    s += (int) mInQueue;
    return s.c_str();
  }

  if (member == "heartbeat")
  {
    char line[1024];
    snprintf(line, sizeof(line) - 1, "%llu", (unsigned long long) mHeartBeat);
    mHeartBeatString = line;
    return mHeartBeatString;
  }

  if (member == "heartbeatdelta")
  {
    char line[1024];

    if (labs(time(NULL) - mHeartBeat) > 86400)
    {
      snprintf(line, sizeof(line) - 1, "~");
    }
    else
    {
      snprintf(line, sizeof(line) - 1, "%llu",
               (unsigned long long)(time(NULL) - mHeartBeat));
    }

    mHeartBeatDeltaString = line;
    return mHeartBeatDeltaString;
  }

  if (member == "status") return mStatus;

  // Return global config value
  std::string prefix = member;
  prefix.erase(4);

  if (prefix == "cfg.")
  {
    std::string val = "???";
    member.erase(0, 4);
    eos::common::GlobalConfig::gConfig.SOM()->HashMutex.LockRead();
    std::string nodeconfigname = eos::common::GlobalConfig::gConfig.QueuePrefixName(
        GetConfigQueuePrefix(), mName.c_str());
    XrdMqSharedHash* hash = eos::common::GlobalConfig::gConfig.Get(
        nodeconfigname.c_str());

    if (hash)
    {
      val = hash->Get(member.c_str());
    }

    eos::common::GlobalConfig::gConfig.SOM()->HashMutex.UnLockRead();

    // it's otherwise hard to get the default into place
    if (((val == "") || (val == "???")) && (member == "stat.balancing"))
    {
      val = "idle";
    }

    return val;
  }

  return "";
}

//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
FsNode::~FsNode()
{
  if (mGwQueue) delete mGwQueue;
  FsView::gFsView.mGwNodes.erase(mName); // unregister evt. gateway node
}

/*----------------------------------------------------------------------------*/
bool
FsNode::SnapShotHost(FileSystem::host_snapshot_t &host, bool dolock)
{
  auto som = eos::common::GlobalConfig::gConfig.SOM();
  if (dolock)
  {
    som->HashMutex.LockRead();
  }
  XrdMqSharedHash *hash = NULL;
  std::string nodeconfigname = eos::common::GlobalConfig::gConfig.QueuePrefixName(GetConfigQueuePrefix(), mName.c_str());
  if ((hash = som->GetObject(nodeconfigname.c_str(), "hash")))
  {
    host.mQueue = nodeconfigname;
    host.mHost        = GetMember("host");
    host.mHostPort        = GetMember("hostport");
    host.mGeoTag        = hash->Get("stat.geotag");
    host.mPublishTimestamp = hash->GetLongLong("stat.publishtimestamp");
    host.mNetEthRateMiB = hash->GetDouble("stat.net.ethratemib");
    host.mNetInRateMiB  = hash->GetDouble("stat.net.inratemib");
    host.mNetOutRateMiB = hash->GetDouble("stat.net.outratemib");
    host.mGopen = hash->GetLongLong("stat.dataproxy.gopen");
    if (dolock)
    {
      som->HashMutex.UnLockRead();
    }
    return true;
  }
  else
  {
    if (dolock)
    {
      som->HashMutex.UnLockRead();
    }
    host.mQueue = nodeconfigname;
    host.mHost = mName;
    host.mHostPort = "";
    host.mGeoTag        = "";
    host.mPublishTimestamp = 0;
    host.mNetEthRateMiB = 0;
    host.mNetInRateMiB  = 0;
    host.mNetOutRateMiB = 0;
    host.mGopen = 0;
    return false;
  }
}

//------------------------------------------------------------------------------
// GetMember
//------------------------------------------------------------------------------
std::string
FsNode::GetMember(std::string member)
{
  if (member == "hostport")
  {
    std::string hostport =
        eos::common::StringConversion::GetStringHostPortFromQueue(mName.c_str());
    return hostport;
  }
  else
  {
    return BaseView::GetMember(member);
  }
}

/*----------------------------------------------------------------------------*/
bool
FsNode::HasHeartBeat ( eos::common::FileSystem::host_snapshot_t &fs)
{
  time_t now = time(NULL);
  time_t hb = fs.mHeartBeatTime;
  if ((now - hb) < 60)
  {
    // we allow some time drift plus overload delay of 60 seconds
    return true;
  }
  return false;
}

/*----------------------------------------------------------------------------*/
eos::common::FileSystem::fsactive_t
FsNode::GetActiveStatus ()
{
  std::string active = GetMember("stat.active");
  if (active == "online")
  {
    return eos::common::FileSystem::kOnline;
  }
  else
  {
    return eos::common::FileSystem::kOffline;
  }
}

/*----------------------------------------------------------------------------*/
bool
FsNode::SetActiveStatus (eos::common::FileSystem::fsactive_t active)
{
  if (active == eos::common::FileSystem::kOnline)
  return SetConfigMember("stat.active","online",true,mName.c_str(),true);
  else
    return SetConfigMember("stat.active","offline",true,mName.c_str(),true);
}

//------------------------------------------------------------------------------
// Set a configuration member variable (stored in the config engine)
// If 'isstatus'=true we just store the value in the shared hash but don't flush
// it into the configuration engine.
//   => is used to set status variables on config queues (baseview queues)
//------------------------------------------------------------------------------
bool
BaseView::SetConfigMember(std::string key, std::string value, bool create,
                          std::string broadcastqueue, bool isstatus)
{
  bool success = false;
#ifndef EOSMGMFSVIEWTEST
  eos::common::GlobalConfig::gConfig.SOM()->HashMutex.LockRead();
  std::string nodeconfigname = eos::common::GlobalConfig::gConfig.QueuePrefixName(
      GetConfigQueuePrefix(), mName.c_str());
  XrdMqSharedHash* hash = eos::common::GlobalConfig::gConfig.Get(
      nodeconfigname.c_str());

  if (!hash && create)
  {
    eos::common::GlobalConfig::gConfig.SOM()->HashMutex.UnLockRead();

    if (!eos::common::GlobalConfig::gConfig.AddConfigQueue(nodeconfigname.c_str(),
                                                           broadcastqueue.c_str()))
    {
      success = false;
    }

    eos::common::GlobalConfig::gConfig.SOM()->HashMutex.LockRead();
    hash = eos::common::GlobalConfig::gConfig.Get(nodeconfigname.c_str());
  }

  if (hash)
  {
    success = hash->Set(key, value);

    if (key == "txgw")
    {
      eos::common::RWMutexWriteLock gwlock(FsView::gFsView.GwMutex);

      if (value == "on")
      {
        // we have to register this queue into the gw set for fast lookups
        FsView::gFsView.mGwNodes.insert(broadcastqueue);
        // clear the queue if a machine is enabled
        FsView::gFsView.mNodeView[broadcastqueue]->mGwQueue->Clear();
      }
      else
      {
        FsView::gFsView.mGwNodes.erase(broadcastqueue);
      }
    }
  }

  eos::common::GlobalConfig::gConfig.SOM()->HashMutex.UnLockRead();

  // Register in the configuration engine
  if ((!isstatus) && (FsView::ConfEngine))
  {
    nodeconfigname += "#";
    nodeconfigname += key;
    std::string confval = value;
    FsView::ConfEngine->SetConfigValue("global", nodeconfigname.c_str(),
                                       confval.c_str());
  }

#endif
  return success;
}

//------------------------------------------------------------------------------
// Get a configuration member variable (stored in the config engine)
//------------------------------------------------------------------------------
std::string
BaseView::GetConfigMember(std::string key)
{
#ifndef EOSMGMFSVIEWTEST
  XrdMqRWMutexReadLock lock(eos::common::GlobalConfig::gConfig.SOM()->HashMutex);
  std::string nodeconfigname = eos::common::GlobalConfig::gConfig.QueuePrefixName(
      GetConfigQueuePrefix(), mName.c_str());
  XrdMqSharedHash* hash = eos::common::GlobalConfig::gConfig.Get(
      nodeconfigname.c_str());

  if (hash)
  {
    return hash->Get(key);
  }

#endif
  return "";
}

//------------------------------------------------------------------------------
// GetConfigKeys
//------------------------------------------------------------------------------
bool
BaseView::GetConfigKeys(std::vector<std::string>& keys)
{
#ifndef EOSMGMFSVIEWTEST
  XrdMqRWMutexReadLock lock(eos::common::GlobalConfig::gConfig.SOM()->HashMutex);
  std::string nodeconfigname = eos::common::GlobalConfig::gConfig.QueuePrefixName(
      GetConfigQueuePrefix(), mName.c_str());
  XrdMqSharedHash* hash = eos::common::GlobalConfig::gConfig.Get(
      nodeconfigname.c_str());

  if (hash)
  {
    hash->GetKeys(keys);
    return true;
  }

#endif
  return false;
}

//------------------------------------------------------------------------------
// Creates a new filesystem id based on a uuid
//------------------------------------------------------------------------------
eos::common::FileSystem::fsid_t
FsView::CreateMapping(std::string fsuuid)
{
  eos::common::RWMutexWriteLock lock(MapMutex);

  if (Uuid2FsMap.count(fsuuid))
  {
    return Uuid2FsMap[fsuuid];
  }
  else
  {
    if (!NextFsId) SetNextFsId(1);

    std::map<eos::common::FileSystem::fsid_t, std::string>::const_iterator it;

    // use the maximum fsid
    for (it = Fs2UuidMap.begin(); it != Fs2UuidMap.end(); it++)
    {
      if (it->first > NextFsId)
      {
        NextFsId = it->first;
      }
    }

    if (NextFsId > 64000)
    {
      // We don't support more than 64.000 filesystems
      NextFsId = 1;
    }

    while (Fs2UuidMap.count(NextFsId))
    {
      NextFsId++;

      if (NextFsId > 640000)
      {
        // If all filesystem id's are exhausted we better abort the program to avoid a mess!
        eos_static_crit("all filesystem id's exhausted (64.000) - aborting the program");
        exit(-1);
      }
    }

    SetNextFsId(NextFsId);
    Uuid2FsMap[fsuuid] = NextFsId;
    Fs2UuidMap[NextFsId] = fsuuid;
    return NextFsId;
  }
}

//------------------------------------------------------------------------------
// Adds a fsid=uuid pair to the mapping
//------------------------------------------------------------------------------
bool
FsView::ProvideMapping(std::string fsuuid, eos::common::FileSystem::fsid_t fsid)
{
  eos::common::RWMutexWriteLock lock(MapMutex);

  if (Uuid2FsMap.count(fsuuid))
  {
    if (Uuid2FsMap[fsuuid] == fsid)
      return true; // we accept if it is consistent with the existing mapping
    else
      return false;// we reject if it is in contradiction to an existing mapping
  }
  else
  {
    Uuid2FsMap[fsuuid] = fsid;
    Fs2UuidMap[fsid] = fsuuid;
    return true;
  }
}

/*----------------------------------------------------------------------------*/
eos::common::FileSystem::fsid_t
FsView::GetMapping(std::string fsuuid)
{
  //----------------------------------------------------------------
  //! returns an fsid for a uuid
  //----------------------------------------------------------------
  eos::common::RWMutexReadLock lock(MapMutex);

  if (Uuid2FsMap.count(fsuuid))
  {
    return Uuid2FsMap[fsuuid];
  }
  else
  {
    return 0; // 0 means there is no mapping
  }
}


//------------------------------------------------------------------------------
// Removes a mapping entry by fsid
//------------------------------------------------------------------------------
bool
FsView::RemoveMapping(eos::common::FileSystem::fsid_t fsid)
{
  eos::common::RWMutexWriteLock lock(MapMutex);
  bool removed = false;
  std::string fsuuid;

  if (Fs2UuidMap.count(fsid))
  {
    fsuuid = Fs2UuidMap[fsid];
    Fs2UuidMap.erase(fsid);
    removed = true;
  }

  if (Uuid2FsMap.count(fsuuid))
  {
    Uuid2FsMap.erase(fsuuid);
    removed = true;
  }

  return removed;
}

//------------------------------------------------------------------------------
// Removes a mapping entry by providing fsid + uuid
//------------------------------------------------------------------------------
bool
FsView::RemoveMapping(eos::common::FileSystem::fsid_t fsid, std::string fsuuid)
{
  eos::common::RWMutexWriteLock lock(MapMutex);
  bool removed = false;

  if (Uuid2FsMap.count(fsuuid))
  {
    Uuid2FsMap.erase(fsuuid);
    removed = true;
  }

  if (Fs2UuidMap.count(fsid))
  {
    Fs2UuidMap.erase(fsid);
    removed = true;
  }

  return removed;
}

//------------------------------------------------------------------------------
// Print space information to out
//------------------------------------------------------------------------------
void
FsView::PrintSpaces(std::string& out, std::string headerformat,
                    std::string listformat, unsigned int outdepth,
                    const char* selection)
{
  std::map<std::string, FsSpace* >::iterator it;
  std::vector<std::string> selections;
  std::string selected = selection ? selection : "";

  if (selection)
  {
    eos::common::StringConversion::Tokenize(selected, selections , ",");
  }

  for (it = mSpaceView.begin(); it != mSpaceView.end(); it++)
  {
    if (selection)
    {
      bool found = false;
      bool spacefound = false;

      for (size_t i = 0; i < selections.size(); i++)
      {
        std::string sel = selections[i];

        // only apply space:... selections
        if (sel.substr(0, 6) == "space:")
        {
          spacefound = true;
          sel.erase(0, 6);
        }

        if ((it->second->mName.find(sel) != std::string::npos))
          found = true;
      }

      if (selections.size() && (!spacefound))
      {
        found = true;
      }

      if (!found) continue;
    }

    it->second->Print(out, headerformat, listformat, outdepth, selections);

    if (!listformat.length() && ((headerformat.find("header=1:")) == 0))
    {
      headerformat.erase(0, 9);
    }
  }
}

//----------------------------------------------------------------------------
// Print group information to out
//----------------------------------------------------------------------------
void
FsView::PrintGroups(std::string& out, std::string headerformat,
                    std::string listformat, unsigned int outdepth,
                    const char* selection)
{
  std::map<std::string, FsGroup* >::iterator it;
  std::vector<std::string> selections;
  std::string selected = selection ? selection : "";

  if (selection)
  {
    eos::common::StringConversion::Tokenize(selected, selections , ",");
  }

  for (it = mGroupView.begin(); it != mGroupView.end(); it++)
  {
    if (selection)
    {
      bool found = false;

      for (size_t i = 0; i < selections.size(); i++)
      {
        if ((it->second->mName.find(selections[i])) != std::string::npos)
          found = true;
      }

      if (!found)  continue;
    }

    selections.clear();
    it->second->Print(out, headerformat, listformat, outdepth, selections);

    if (!listformat.length() && ((headerformat.find("header=1:")) == 0))
    {
      headerformat.erase(0, 9);
    }
  }
}

//------------------------------------------------------------------------------
// Print node information to out
//------------------------------------------------------------------------------
void
FsView::PrintNodes(std::string& out, std::string headerformat,
                   std::string listformat, unsigned int outdepth,
                   const char* selection)
{
  std::map<std::string, FsNode* >::iterator it;
  std::vector<std::string> selections;
  std::string selected = selection ? selection : "";

  if (selection)
  {
    eos::common::StringConversion::Tokenize(selected, selections , ",");
  }

  for (it = mNodeView.begin(); it != mNodeView.end(); it++)
  {
    if (selection)
    {
      bool found = false;

      for (size_t i = 0; i < selections.size(); i++)
      {
        if ((it->second->mName.find(selections[i])) != std::string::npos)
          found = true;
      }

      if (!found) continue;
    }

    selections.clear();
    it->second->Print(out, headerformat, listformat, outdepth, selections);

    if (!listformat.length() && ((headerformat.find("header=1:")) == 0))
    {
      headerformat.erase(0, 9);
    }
  }
}

#ifndef EOSMGMFSVIEWTEST

//------------------------------------------------------------------------------
// Converts a config engine definition for a filesystem into the FsView
// representation.
//------------------------------------------------------------------------------
bool
FsView::ApplyFsConfig(const char* inkey, std::string& val)
{
  if (!inkey) return false;

  // Convert to map
  std::string key = inkey;
  std::map<std::string, std::string> configmap;
  std::vector<std::string> tokens;
  eos::common::StringConversion::Tokenize(val, tokens);

  for (size_t i = 0; i < tokens.size(); i++)
  {
    std::vector<std::string> keyval;
    std::string delimiter = "=";
    eos::common::StringConversion::Tokenize(tokens[i], keyval, delimiter);
    configmap[keyval[0]] = keyval[1];
  }

  if ((!configmap.count("queuepath")) || (!configmap.count("queue"))
      || (!configmap.count("id")))
  {
    eos_static_err("config definitions missing ...");
    return false;
  }

  eos::common::RWMutexWriteLock viewlock(ViewMutex);
  eos::common::FileSystem::fsid_t fsid = atoi(configmap["id"].c_str());
  FileSystem* fs = 0;

  // Apply only the registration fo a new filesystem if it does not exist
  if (!FsView::gFsView.mIdView.count(fsid))
  {
    fs = new FileSystem(configmap["queuepath"].c_str(), configmap["queue"].c_str(),
                        eos::common::GlobalConfig::gConfig.SOM());
  }
  else
  {
    fs = FsView::gFsView.mIdView[fsid];
  }

  if (fs)
  {
    fs->OpenTransaction();
    fs->SetId(fsid);
    fs->SetString("uuid", configmap["uuid"].c_str());
    std::map<std::string, std::string>::iterator it;

    for (it = configmap.begin(); it != configmap.end(); it++)
    {
      // set config parameters
      fs->SetString(it->first.c_str(), it->second.c_str());
    }

    fs->CloseTransaction();

    if (!FsView::gFsView.Register(fs))
    {
      eos_static_err("cannot register filesystem name=%s from configuration",
                     configmap["queuepath"].c_str());
      return false;
    }

    // insert into the mapping
    FsView::gFsView.ProvideMapping(configmap["uuid"], fsid);
    return true;
  }

  return false;
}

//------------------------------------------------------------------------------
// Converts a config engine definition of a global variable into the FsView
// representation.
//------------------------------------------------------------------------------
bool
FsView::ApplyGlobalConfig(const char* key, std::string& val)
{
  // global variables are stored like key='<queuename>:<variable>' val='<val>'
  std::string configqueue = key;
  std::vector<std::string> tokens;
  std::vector<std::string> paths;
  std::string delimiter = "#";
  std::string pathdelimiter = "/";
  eos::common::StringConversion::Tokenize(configqueue, tokens, delimiter);
  eos::common::StringConversion::Tokenize(configqueue, paths, pathdelimiter);
  bool success = false;

  if (tokens.size() != 2)
  {
    eos_static_err("the key definition of config <%s> is invalid", key);
    return false;
  }

  if (paths.size() < 1)
  {
    eos_static_err("the queue name does not contain any /");
    return false;
  }

  eos::common::GlobalConfig::gConfig.SOM()->HashMutex.LockRead();
  XrdMqSharedHash* hash = eos::common::GlobalConfig::gConfig.Get(
      tokens[0].c_str());

  if (!hash)
  {
    eos::common::GlobalConfig::gConfig.SOM()->HashMutex.UnLockRead();

    // create a global config queue
    if ((tokens[0].find("/node/")) != std::string::npos)
    {
      std::string broadcast = "/eos/";
      broadcast += paths[paths.size() - 1];
      size_t dashpos = 0;

      // remote the #<variable>
      if ((dashpos = broadcast.find("#")) != std::string::npos)
      {
        broadcast.erase(dashpos);
      }

      broadcast += "/fst";

      if (!eos::common::GlobalConfig::gConfig.AddConfigQueue(tokens[0].c_str(),
                                                             broadcast.c_str()))
      {
        eos_static_err("cannot create config queue <%s>", tokens[0].c_str());
      }
    }
    else
    {
      if (!eos::common::GlobalConfig::gConfig.AddConfigQueue(tokens[0].c_str(),
                                                             "/eos/*/mgm"))
      {
        eos_static_err("cannot create config queue <%s>", tokens[0].c_str());
      }
    }

    eos::common::GlobalConfig::gConfig.SOM()->HashMutex.LockRead();
    hash = eos::common::GlobalConfig::gConfig.Get(tokens[0].c_str());
  }

  if (hash)
  {
    success = hash->Set(tokens[1].c_str(), val.c_str());

    // Here we build a set with the gw nodes for fast lookup in the TransferEngine
    if ((tokens[0].find("/node/")) != std::string::npos)
    {
      if (tokens[1] == "txgw")
      {
        std::string broadcast = "/eos/";
        broadcast += paths[paths.size() - 1];
        size_t dashpos = 0;

        // Remote the #<variable>
        if ((dashpos = broadcast.find("#")) != std::string::npos)
        {
          broadcast.erase(dashpos);
        }

        broadcast += "/fst";
        FsView::gFsView.RegisterNode(
            broadcast.c_str()); // the node might not yet exist!
        eos::common::RWMutexWriteLock gwlock(GwMutex);

        if (val == "on")
        {
          // we have to register this queue into the gw set for fast lookups
          FsView::gFsView.mGwNodes.insert(broadcast.c_str());
        }
        else
        {
          FsView::gFsView.mGwNodes.erase(broadcast.c_str());
        }
      }
    }
  }
  else
  {
    eos_static_err("there is no global config for queue <%s>", tokens[0].c_str());
  }

  eos::common::GlobalConfig::gConfig.SOM()->HashMutex.UnLockRead();
  return success;
}
#endif

//------------------------------------------------------------------------------
// Computes the sum for <param> as long
// param="<param>[?<key>=<value] allows to select with matches
//------------------------------------------------------------------------------
long long
BaseView::SumLongLong(const char* param, bool lock,
                      const std::set<eos::common::FileSystem::fsid_t>* subset)
{
  if (lock)
  {
    FsView::gFsView.ViewMutex.LockRead();
  }

  long long sum = 0;
  std::string sparam = param;
  size_t qpos = 0;
  std::string key = "";
  std::string value = "";
  bool isquery = false;

  if ((qpos = sparam.find("?")) != std::string::npos)
  {
    std::string query = sparam;
    query.erase(0, qpos + 1);
    sparam.erase(qpos);
    std::vector<std::string> token;
    std::string delimiter = "@";
    eos::common::StringConversion::Tokenize(query, token, delimiter);
    key = token[0];
    value = token[1];
    isquery = true;
  }

  if (isquery && key == "*" && value == "*")
  {
    // we just count the number of entries
    if (subset)
      return subset->size();
    else
      return size();
  }

  if (subset)
  {
    for (auto it = subset->begin(); it != subset->end(); it++)
    {
      eos::common::FileSystem::fs_snapshot snapshot;

      // for query sum's we always fold in that a group and host has to be enabled
      if ((!key.length())
          || (FsView::gFsView.mIdView[*it]->GetString(key.c_str()) == value))
      {
        if (isquery &&
            ((!eos::common::FileSystem::GetActiveStatusFromString(
                  FsView::gFsView.mIdView[*it]->GetString("stat.active").c_str())) ||
             (eos::common::FileSystem::GetStatusFromString(
                 FsView::gFsView.mIdView[*it]->GetString("stat.boot").c_str()) !=
              eos::common::FileSystem::kBooted)))
        {
          continue;
        }

        long long v = FsView::gFsView.mIdView[*it]->GetLongLong(sparam.c_str());

        if (isquery && v && (sparam == "stat.statfs.capacity"))
        {
          // Correct the capacity(rw) value for headroom
          v -= FsView::gFsView.mIdView[*it]->GetLongLong("headroom");
        }

        sum += v;
      }
    }
  }
  else
  {
    for (auto it = begin(); it != end(); it++)
    {
      eos::common::FileSystem::fs_snapshot snapshot;

      // for query sum's we always fold in that a group and host has to be enabled
      if ((!key.length())
          || (FsView::gFsView.mIdView[*it]->GetString(key.c_str()) == value))
      {
        if (isquery &&
            ((!eos::common::FileSystem::GetActiveStatusFromString(
                  FsView::gFsView.mIdView[*it]->GetString("stat.active").c_str())) ||
             (eos::common::FileSystem::GetStatusFromString(
                 FsView::gFsView.mIdView[*it]->GetString("stat.boot").c_str()) !=
              eos::common::FileSystem::kBooted)))
        {
          continue;
        }

        long long v = FsView::gFsView.mIdView[*it]->GetLongLong(sparam.c_str());

        if (isquery && v && (sparam == "stat.statfs.capacity"))
        {
          // correct the capacity(rw) value for headroom
          v -= FsView::gFsView.mIdView[*it]->GetLongLong("headroom");
        }

        sum += v;
      }
    }
  }

  // We have to rescale the stat.net parameters because they arrive for each filesystem
  if (!sparam.compare(0, 8, "stat.net"))
  {
    if (mType == "spaceview")
    {
      // divide by the number of "cfg.groupmod"
      std::string gsize = "";
      long long groupmod = 1;
      gsize = GetMember("cfg.groupmod");

      if (gsize.length())
      {
        groupmod = strtoll(gsize.c_str(), 0, 10);
      }

      if (groupmod)
      {
        sum /= groupmod;
      }
    }

    if ((mType == "nodesview"))
    {
      // divide by the number of entries we have summed
      if (size())
        sum /= size();
    }
  }

  if (lock)
  {
    FsView::gFsView.ViewMutex.UnLockRead();
  }

  return sum;
}

//------------------------------------------------------------------------------
// Computes the sum for <param> as double
//------------------------------------------------------------------------------
double
BaseView::SumDouble(const char* param, bool lock,
                    const std::set<eos::common::FileSystem::fsid_t>* subset)
{
  if (lock)
  {
    FsView::gFsView.ViewMutex.LockRead();
  }

  double sum = 0;

  if (subset)
  {
    for (auto it = subset->begin(); it != subset->end(); it++)
    {
      sum += FsView::gFsView.mIdView[*it]->GetDouble(param);
    }
  }
  else
  {
    for (auto it = begin(); it != end(); it++)
    {
      sum += FsView::gFsView.mIdView[*it]->GetDouble(param);
    }
  }

  if (lock)
  {
    FsView::gFsView.ViewMutex.UnLockRead();
  }

  return sum;
}

//------------------------------------------------------------------------------
// Computes the average for <param>
//------------------------------------------------------------------------------
double
BaseView::AverageDouble(const char* param, bool lock,
                        const std::set<eos::common::FileSystem::fsid_t>* subset)
{
  if (lock)
  {
    FsView::gFsView.ViewMutex.LockRead();
  }

  double sum = 0;
  int cnt = 0;

  if (subset)
  {
    for (auto it = subset->begin(); it != subset->end(); it++)
    {
      bool consider = true;

      if (mType == "groupview")
      {
        // we only count filesystem which are >=kRO and booted for averages in the group view
        if ((FsView::gFsView.mIdView[*it]->GetConfigStatus() <
             eos::common::FileSystem::kRO) ||
            (FsView::gFsView.mIdView[*it]->GetStatus() != eos::common::FileSystem::kBooted)
            ||
            (FsView::gFsView.mIdView[*it]->GetActiveStatus() ==
             eos::common::FileSystem::kOffline))
        {
          consider = false;
        }
      }

      if (consider)
      {
        cnt++;
        sum += FsView::gFsView.mIdView[*it]->GetDouble(param);
      }
    }
  }
  else
  {
    for (auto it = begin(); it != end(); it++)
    {
      bool consider = true;

      if (mType == "groupview")
      {
        // we only count filesystem which are >=kRO and booted for averages in the group view
        if ((FsView::gFsView.mIdView[*it]->GetConfigStatus() <
             eos::common::FileSystem::kRO) ||
            (FsView::gFsView.mIdView[*it]->GetStatus() != eos::common::FileSystem::kBooted)
            ||
            (FsView::gFsView.mIdView[*it]->GetActiveStatus() ==
             eos::common::FileSystem::kOffline))
        {
          consider = false;
        }
      }

      if (consider)
      {
        cnt++;
        sum += FsView::gFsView.mIdView[*it]->GetDouble(param);
      }
    }
  }

  if (lock)
  {
    FsView::gFsView.ViewMutex.UnLockRead();
  }

  return (cnt) ? (double)(1.0 * sum / cnt) : 0;
}

//------------------------------------------------------------------------------
// Computes the maximum absolute deviation of <param> from the avg of <param>
//------------------------------------------------------------------------------
double
BaseView::MaxAbsDeviation(const char* param, bool lock,
                          const std::set<eos::common::FileSystem::fsid_t>* subset)
{
  if (lock)
  {
    FsView::gFsView.ViewMutex.LockRead();
  }

  double avg = AverageDouble(param, false);
  double maxabsdev = 0;
  double dev = 0;

  if (subset)
  {
    for (auto it = subset->begin(); it != subset->end(); it++)
    {
      bool consider = true;

      if (mType == "groupview")
      {
        // we only count filesystem which are >=kRO and booted for averages in the group view
        if ((FsView::gFsView.mIdView[*it]->GetConfigStatus() <
             eos::common::FileSystem::kRO) ||
            (FsView::gFsView.mIdView[*it]->GetStatus() != eos::common::FileSystem::kBooted)
            ||
            (FsView::gFsView.mIdView[*it]->GetActiveStatus() ==
             eos::common::FileSystem::kOffline))
          consider = false;
      }

      dev = fabs(avg - FsView::gFsView.mIdView[*it]->GetDouble(param));

      if (consider)
      {
        if (dev > maxabsdev)
          maxabsdev = dev;
      }
    }
  }
  else
  {
    for (auto it = begin(); it != end(); it++)
    {
      bool consider = true;

      if (mType == "groupview")
      {
        // we only count filesystem which are >=kRO and booted for averages in the group view
        if ((FsView::gFsView.mIdView[*it]->GetConfigStatus() <
             eos::common::FileSystem::kRO) ||
            (FsView::gFsView.mIdView[*it]->GetStatus() != eos::common::FileSystem::kBooted)
            ||
            (FsView::gFsView.mIdView[*it]->GetActiveStatus() ==
             eos::common::FileSystem::kOffline))
        {
          consider = false;
        }
      }

      dev = fabs(avg - FsView::gFsView.mIdView[*it]->GetDouble(param));

      if (consider)
      {
        if (dev > maxabsdev)
          maxabsdev = dev;
      }
    }
  }

  if (lock)
  {
    FsView::gFsView.ViewMutex.UnLockRead();
  }

  return maxabsdev;
}


//------------------------------------------------------------------------------
// Computes the maximum deviation of <param> from the avg of <param>
//------------------------------------------------------------------------------
double
BaseView::MaxDeviation(const char* param, bool lock,
                       const std::set<eos::common::FileSystem::fsid_t>* subset)
{
  if (lock)
  {
    FsView::gFsView.ViewMutex.LockRead();
  }

  double avg = AverageDouble(param, false);
  double maxdev = -DBL_MAX;
  double dev = 0;

  if (subset)
  {
    for (auto it = subset->begin(); it != subset->end(); it++)
    {
      bool consider = true;

      if (mType == "groupview")
      {
        // we only count filesystem which are >=kRO and booted for averages in the group view
        if ((FsView::gFsView.mIdView[*it]->GetConfigStatus() <
             eos::common::FileSystem::kRO) ||
            (FsView::gFsView.mIdView[*it]->GetStatus() != eos::common::FileSystem::kBooted)
            ||
            (FsView::gFsView.mIdView[*it]->GetActiveStatus() ==
             eos::common::FileSystem::kOffline))
          consider = false;
      }

      dev = -(avg - FsView::gFsView.mIdView[*it]->GetDouble(param));

      if (consider)
      {
        if (dev > maxdev)
          maxdev = dev;
      }
    }
  }
  else
  {
    for (auto it = begin(); it != end(); it++)
    {
      bool consider = true;

      if (mType == "groupview")
      {
        // we only count filesystem which are >=kRO and booted for averages in the group view
        if ((FsView::gFsView.mIdView[*it]->GetConfigStatus() <
             eos::common::FileSystem::kRO) ||
            (FsView::gFsView.mIdView[*it]->GetStatus() != eos::common::FileSystem::kBooted)
            ||
            (FsView::gFsView.mIdView[*it]->GetActiveStatus() ==
             eos::common::FileSystem::kOffline))
        {
          consider = false;
        }
      }

      dev = -(avg - FsView::gFsView.mIdView[*it]->GetDouble(param));

      if (consider)
      {
        if (dev > maxdev)
          maxdev = dev;
      }
    }
  }

  if (lock)
  {
    FsView::gFsView.ViewMutex.UnLockRead();
  }

  return maxdev;
}

//------------------------------------------------------------------------------
// Computes the maximum deviation of <param> from the avg of <param>
//------------------------------------------------------------------------------
double
BaseView::MinDeviation(const char* param, bool lock,
                       const std::set<eos::common::FileSystem::fsid_t>* subset)
{
  if (lock)
  {
    FsView::gFsView.ViewMutex.LockRead();
  }

  double avg = AverageDouble(param, false);
  double mindev = DBL_MAX;
  double dev = 0;

  if (subset)
  {
    for (auto it = subset->begin(); it != subset->end(); it++)
    {
      bool consider = true;

      if (mType == "groupview")
      {
        // we only count filesystem which are >=kRO and booted for averages in the group view
        if ((FsView::gFsView.mIdView[*it]->GetConfigStatus() <
             eos::common::FileSystem::kRO) ||
            (FsView::gFsView.mIdView[*it]->GetStatus() != eos::common::FileSystem::kBooted)
            ||
            (FsView::gFsView.mIdView[*it]->GetActiveStatus() ==
             eos::common::FileSystem::kOffline))
        {
          consider = false;
        }
      }

      dev = -(avg - FsView::gFsView.mIdView[*it]->GetDouble(param));

      if (consider)
      {
        if (dev < mindev)
          mindev = dev;
      }
    }
  }
  else
  {
    for (auto it = begin(); it != end(); it++)
    {
      bool consider = true;

      if (mType == "groupview")
      {
        // we only count filesystem which are >=kRO and booted for averages in the group view
        if ((FsView::gFsView.mIdView[*it]->GetConfigStatus() <
             eos::common::FileSystem::kRO) ||
            (FsView::gFsView.mIdView[*it]->GetStatus() != eos::common::FileSystem::kBooted)
            ||
            (FsView::gFsView.mIdView[*it]->GetActiveStatus() ==
             eos::common::FileSystem::kOffline))
        {
          consider = false;
        }
      }

      dev = -(avg - FsView::gFsView.mIdView[*it]->GetDouble(param));

      if (consider)
      {
        if (dev < mindev)
          mindev = dev;
      }
    }
  }

  if (lock)
  {
    FsView::gFsView.ViewMutex.UnLockRead();
  }

  return mindev;
}

//------------------------------------------------------------------------------
// Computes the sigma for <param>
//------------------------------------------------------------------------------
double
BaseView::SigmaDouble(const char* param, bool lock,
                      const std::set<eos::common::FileSystem::fsid_t>* subset)
{
  if (lock)
  {
    FsView::gFsView.ViewMutex.LockRead();
  }

  double avg = AverageDouble(param, false);
  double sumsquare = 0;
  int cnt = 0;

  if (subset)
  {
    for (auto it = subset->begin(); it != subset->end(); it++)
    {
      bool consider = true;

      if (mType == "groupview")
      {
        // we only count filesystem which are >=kRO and booted for averages in the group view
        if ((FsView::gFsView.mIdView[*it]->GetConfigStatus() <
             eos::common::FileSystem::kRO) ||
            (FsView::gFsView.mIdView[*it]->GetStatus() != eos::common::FileSystem::kBooted)
            ||
            (FsView::gFsView.mIdView[*it]->GetActiveStatus() ==
             eos::common::FileSystem::kOffline))
          consider = false;
      }

      if (consider)
      {
        cnt++;
        sumsquare += pow((avg - FsView::gFsView.mIdView[*it]->GetDouble(param)), 2);
      }
    }
  }
  else
  {
    for (auto it = begin(); it != end(); it++)
    {
      bool consider = true;

      if (mType == "groupview")
      {
        // we only count filesystem which are >=kRO and booted for averages in the group view
        if ((FsView::gFsView.mIdView[*it]->GetConfigStatus() <
             eos::common::FileSystem::kRO) ||
            (FsView::gFsView.mIdView[*it]->GetStatus() != eos::common::FileSystem::kBooted)
            ||
            (FsView::gFsView.mIdView[*it]->GetActiveStatus() ==
             eos::common::FileSystem::kOffline))
        {
          consider = false;
        }
      }

      if (consider)
      {
        cnt++;
        sumsquare += pow((avg - FsView::gFsView.mIdView[*it]->GetDouble(param)), 2);
      }
    }
  }

  sumsquare = (cnt) ? sqrt(sumsquare / cnt) : 0;

  if (lock)
  {
    FsView::gFsView.ViewMutex.UnLockRead();
  }

  return sumsquare;
}

//------------------------------------------------------------------------------
// Computes the considered count
//------------------------------------------------------------------------------
long long
BaseView::ConsiderCount(bool lock,
                        const std::set<eos::common::FileSystem::fsid_t>* subset)
{
  if (lock)
  {
    FsView::gFsView.ViewMutex.LockRead();
  }

  long long cnt = 0;

  if (subset)
  {
    for (auto it = subset->begin(); it != subset->end(); it++)
    {
      bool consider = true;

      if (mType == "groupview")
      {
        // we only count filesystem which are >=kRO and booted for averages in the group view
        if ((FsView::gFsView.mIdView[*it]->GetConfigStatus() <
             eos::common::FileSystem::kRO) ||
            (FsView::gFsView.mIdView[*it]->GetStatus() != eos::common::FileSystem::kBooted)
            ||
            (FsView::gFsView.mIdView[*it]->GetActiveStatus() ==
             eos::common::FileSystem::kOffline))
          consider = false;
      }

      if (consider) cnt++;
    }
  }
  else
  {
    for (auto it = begin(); it != end(); it++)
    {
      bool consider = true;

      if (mType == "groupview")
      {
        // we only count filesystem which are >=kRO and booted for averages in the group view
        if ((FsView::gFsView.mIdView[*it]->GetConfigStatus() <
             eos::common::FileSystem::kRO) ||
            (FsView::gFsView.mIdView[*it]->GetStatus() != eos::common::FileSystem::kBooted)
            ||
            (FsView::gFsView.mIdView[*it]->GetActiveStatus() ==
             eos::common::FileSystem::kOffline))
          consider = false;
      }

      if (consider) cnt++;
    }
  }

  if (lock)
  {
    FsView::gFsView.ViewMutex.UnLockRead();
  }

  return cnt;
}

//------------------------------------------------------------------------------
// Computes the considered count
//------------------------------------------------------------------------------
long long
BaseView::TotalCount(bool lock,
                     const std::set<eos::common::FileSystem::fsid_t>* subset)
{
  if (lock)
  {
    FsView::gFsView.ViewMutex.LockRead();
  }

  long long cnt = 0;

  if (subset)
  {
    cnt = subset->size();
  }
  else
  {
    cnt = size();
  }

  if (lock)
  {
    FsView::gFsView.ViewMutex.UnLockRead();
  }

  return cnt;
}

//------------------------------------------------------------------------------
// Print user defined format to out
//------------------------------------------------------------------------------
void
BaseView::Print(std::string& out, std::string headerformat,
                std::string listformat, unsigned outdepth,
                std::vector<std::string>& selections)
{
  //-------------------------------------------------------------------------------
  // headerformat
  //-------------------------------------------------------------------------------
  // format has to be provided as a chain (separated by "|" ) of the following tags
  // "member=<key>:width=<width>:format=[+][-][so]:unit=<unit>:tag=<tag>"
  //  -> to print a member variable of the view
  // "avg=<key>:width=<width>:format=[fo]"   -> to print the average
  // "sum=<key>:width=<width>:format=[lo]    -> to print a sum
  // "sig=<key>:width=<width>:format=[lo]    -> to print the standard deviation
  // "maxdev=<key>:width=<width>;format=[lo] -> to print the maxdeviation
  // "sep=<seperator>"                       -> to put a seperator
  // "tag=<tag>"                             -> use tag as header not the variable name
  // "header=1" -> put a header with description on top!
  //               This must be the first format tag!!!
  //-------------------------------------------------------------------------------
  // listformat
  //-------------------------------------------------------------------------------
  // format has to be provided as a chain (separated by "|" ) of the following tags
  // "key=<key>:width=<width>:format=[+][-][slfo]:unit=<unit>:tag=<tag>"
  //  -> to print a key of the attached children
  // "sep=<seperator>" -> to put a seperator
  // "header=1" -> put a header with description on top
  //               This must be the first format tag!!!
  // the formats are:
  // 's' : print as string
  // 'S' : print as short string
  // 'l' : print as long long
  // 'f' : print as double
  // 'o' : print as <key>=<val>
  // '-' : left align the printout
  // '+' : convert numbers into k,M,G,T,P ranges
  // the unit is appended to every number:
  // e.g. 1500 with unit=B would end up as '1.5 kB'
  // the command only appends to <out> and DOES NOT initialize it
  // "tag=<tag>" -> use tag as header not the variable name

  // Since we don't display the members with geodepth option, we proceed with
  // the non geodepth display first.
  if (outdepth > 0)
  {
    Print(out, headerformat, listformat, 0, selections);

    // We force-print the header
    if (headerformat.find("header=1") == std::string::npos)
    {
      if (headerformat.find("header=0") != std::string::npos)
      {
        headerformat.replace(headerformat.find("header=0"), 8, "header=1");
      }

      headerformat = "header=1:" + headerformat;
    }
  }

  std::vector<std::string> formattoken;
  bool buildheader = false;
  std::string header = "";
  std::string body = "";

  class DoubleAggregatedStats : public std::map<std::string, DoubleAggregator*>
  {
    BaseView* pThis;

   public:
    DoubleAggregatedStats(BaseView* This) : pThis(This) {}
    DoubleAggregator* operator[](const char* param)
    {
      if (!count(param))
      {
        DoubleAggregator* aggreg = new DoubleAggregator(param);
        aggreg->setView(pThis);
        pThis->runAggregator(aggreg);
        insert(std::make_pair(param, aggreg));
      }

      return find(param)->second;
    }

    ~DoubleAggregatedStats()
    {
      for (auto it = begin(); it != end(); it++)
        delete it->second;
    }
  };

  class LongLongAggregatedStats : public std::map<std::string, LongLongAggregator*>
  {
    BaseView* pThis;
   public:
    LongLongAggregatedStats(BaseView* This) : pThis(This) {}

    LongLongAggregator* operator[](const char* param)
    {
      if (!count(param))
      {
        LongLongAggregator* aggreg = new LongLongAggregator(param);
        aggreg->setView(pThis);
        pThis->runAggregator(aggreg);
        insert(std::make_pair(param, aggreg));
      }

      return find(param)->second;
    }

    ~LongLongAggregatedStats()
    {
      for (auto it = begin(); it != end(); it++)
        delete it->second;
    }
  };

  LongLongAggregatedStats longStats(this);
  DoubleAggregatedStats doubleStats(this);
  unsigned int nLines = 0;

  if (outdepth > 0)
  {
    nLines = longStats["lastHeartBeat"]->getGeoTags()->size();
    nLines = longStats["lastHeartBeat"]->getEndIndex(outdepth);
  }
  else
  {
    nLines = 1;
  }

  eos::common::StringConversion::Tokenize(headerformat, formattoken, "|");

  for (unsigned int l = 0; l < nLines; l++)
  {
    buildheader = false;

    for (unsigned int i = 0; i < formattoken.size(); i++)
    {
      std::vector<std::string> tagtoken;
      std::map<std::string, std::string> formattags;
      eos::common::StringConversion::Tokenize(formattoken[i], tagtoken, ":");

      for (unsigned int j = 0; j < tagtoken.size(); j++)
      {
        std::vector<std::string> keyval;
        eos::common::StringConversion::Tokenize(tagtoken[j], keyval, "=");
        formattags[keyval[0]] = keyval[1];
      }

      // "key=<key>:width=<width>:format=[slfo]"
      bool alignleft = false;

      if (formattags.count("format") &&
          (formattags["format"].find("-") != std::string::npos))
      {
        alignleft = true;
      }

      if (formattags.count("header"))
      {
        // Add the desired seperator
        if (formattags.count("header") == 1 && l == 0)
        {
          buildheader = true;
        }
      }

      // To save display space, we don't print out members with geodepth option
      if (outdepth > 0 && formattags.count("member"))
        continue;

      if (formattags.count("width") && formattags.count("format"))
      {
        unsigned int width = atoi(formattags["width"].c_str());
        // string
        char line[1024];
        char tmpline[1024];
        char lformat[1024];
        char lenformat[1024];
        line[0] = 0;
        lformat[0] = 0;

        if ((formattags["format"].find("s")) != std::string::npos)
          snprintf(lformat, sizeof(lformat) - 1, "%%s");

        if ((formattags["format"].find("l")) != std::string::npos)
          snprintf(lformat, sizeof(lformat) - 1, "%%lld");

        if ((formattags["format"].find("f")) != std::string::npos)
          snprintf(lformat, sizeof(lformat) - 1, "%%.02f");

        // Protect against missing format types
        if (lformat[0] == 0)
          continue;

        if (alignleft)
        {
          snprintf(lenformat, sizeof(lenformat) - 1, "%%-%ds", width);
        }
        else
        {
          snprintf(lenformat, sizeof(lenformat) - 1, "%%%ds", width);
        }

        // Normal member printout
        if (formattags.count("member"))
        {
          if ((formattags["format"].find("+") != std::string::npos) &&
              (formattags["format"].find("s") == std::string::npos))
          {
            std::string ssize;
            eos::common::StringConversion::GetReadableSizeString(
                ssize, (unsigned long long)(strtoll(
                GetMember(formattags["member"]).c_str(), 0, 10)),
                formattags["unit"].c_str());
            snprintf(line, sizeof(line) - 1, lenformat, ssize.c_str());
          }
          else
          {
            std::string member = GetMember(formattags["member"]).c_str();

            if ((formattags["format"].find("S") != std::string::npos))
            {
              size_t colon = member.find(":");
              size_t dot = member.find(".");

              if (dot != std::string::npos)
                member.erase(dot, (colon != std::string::npos) ? colon - dot : colon);
            }

            if ((formattags["format"].find("l") != std::string::npos))
            {
              snprintf(tmpline, sizeof(tmpline) - 1, lformat, strtoll(member.c_str(), 0, 10));
            }
            else
            {
              snprintf(tmpline, sizeof(tmpline) - 1, lformat, member.c_str());
            }

            snprintf(line, sizeof(line) - 1, lenformat, tmpline);
          }

          if (buildheader)
          {
            char headline[1024];
            char lenformat[1024];
            XrdOucString pkey = formattags["member"].c_str();
            pkey.replace("stat.statfs.", "");
            pkey.replace("stat.", "");
            pkey.replace("cfg.", "");

            if (formattags.count("tag"))
            {
              pkey = formattags["tag"].c_str();
            }

            snprintf(lenformat, sizeof(lenformat) - 1, "%%%ds", width - 1);
            snprintf(headline, sizeof(headline) - 1, lenformat, pkey.c_str());
            std::string sline = headline;

            if (sline.length() != (width - 1))
            {
              sline.erase(0, ((sline.length() - width + 1 + 3) > 0) ?
                          (sline.length() - width + 1 + 3) : 0);
              sline.insert(0, "...");
            }

            header += "#";
            header += sline;
          }
        }

        // Sum printout
        if (formattags.count("sum"))
        {
          if (!outdepth)
          {
            snprintf(tmpline, sizeof(tmpline) - 1, lformat,
                     SumLongLong(formattags["sum"].c_str(), false));
          }
          else
          {
            snprintf(tmpline, sizeof(tmpline) - 1, lformat,
                     (*longStats[formattags["sum"].c_str()]->getSums())[l]);
          }

          if ((formattags["format"].find("+") != std::string::npos))
          {
            std::string ssize;

            if (!outdepth)
            {
              eos::common::StringConversion::GetReadableSizeString(
                  ssize, (unsigned long long) SumLongLong(formattags["sum"].c_str(), false),
                  formattags["unit"].c_str());
            }
            else
            {
              eos::common::StringConversion::GetReadableSizeString(
                  ssize, (unsigned long long)(
                      *longStats[formattags["sum"].c_str()]->getSums())[l],
                  formattags["unit"].c_str());
            }

            snprintf(line, sizeof(line) - 1, lenformat, ssize.c_str());
          }
          else
          {
            snprintf(line, sizeof(line) - 1, lenformat, tmpline);
          }

          if (buildheader)
          {
            char headline[1024];
            char lenformat[1024];
            XrdOucString pkey = formattags["sum"].c_str();
            pkey.replace("stat.statfs.", "");
            pkey.replace("stat.", "");
            pkey.replace("cfg.", "");

            if (formattags.count("tag"))
            {
              pkey = formattags["tag"].c_str();
              width += 5;
            }

            snprintf(lenformat, sizeof(lenformat) - 1, "%%%ds", width - 6);
            snprintf(headline, sizeof(headline) - 1, lenformat, pkey.c_str());
            std::string sline = headline;

            if (sline.length() != (width - 6))
            {
              sline.erase(0, ((sline.length() - width + 6 + 3) > 0) ?
                          (sline.length() - width + 6 + 3) : 0);
              sline.insert(0, "...");
            }

            if (!formattags.count("tag"))
            {
              header += "#";
              header += "sum(";
              header += sline;
              header += ")";
            }
            else
            {
              header += "#";
              header += sline;
            }
          }
        }

        if (formattags.count("avg"))
        {
          if (formattags["avg"] == "stat.geotag")
          {
            if (outdepth)
            {
              // This average means anything only when displaying along the topology tree
              snprintf(tmpline, sizeof(tmpline) - 1, lformat,
                       (*longStats["lastHeartBeat"]->getGeoTags())[l].c_str());
              snprintf(line, sizeof(line) - 1, lenformat, tmpline);

              if (buildheader)
              {
                char headline[1024];
                char lenformat[1024];
                snprintf(lenformat, sizeof(lenformat) - 1, "%%%ds", width - 1);
                snprintf(headline, sizeof(headline) - 1, lenformat, "geotag");
                std::string sline = headline;

                if (sline.length() != (width - 1))
                {
                  sline.erase(0, ((sline.length() - width + 1 + 3) > 0) ?
                              (sline.length() - width + 1 + 3) : 0);
                  sline.insert(0, "...");
                }

                header += "#";
                header += sline;
              }
            }
          }
          else // If not geotag special case
          {
            if (!outdepth)
            {
              snprintf(tmpline, sizeof(tmpline) - 1, lformat,
                       AverageDouble(formattags["avg"].c_str(), false));
            }
            else
            {
              snprintf(tmpline, sizeof(tmpline) - 1, lformat,
                       (*doubleStats[formattags["avg"].c_str()]->getMeans())[l]);
            }

            if ((formattags["format"].find("+") != std::string::npos))
            {
              std::string ssize;

              if (!outdepth)
              {
                eos::common::StringConversion::GetReadableSizeString(
                    ssize, (unsigned long long) AverageDouble(formattags["avg"].c_str(), false),
                    formattags["unit"].c_str());
              }
              else
              {
                eos::common::StringConversion::GetReadableSizeString(
                    ssize, (unsigned long long)(
                        *doubleStats[formattags["avg"].c_str()]->getMeans())[l],
                    formattags["unit"].c_str());
              }

              snprintf(line, sizeof(line) - 1, lenformat, ssize.c_str());
            }
            else
            {
              snprintf(line, sizeof(line) - 1, lenformat, tmpline);
            }

            if (buildheader)
            {
              char headline[1024];
              char lenformat[1024];
              XrdOucString pkey = formattags["avg"].c_str();
              pkey.replace("stat.statfs.", "");
              pkey.replace("stat.", "");
              pkey.replace("cfg.", "");

              if (formattags.count("tag"))
              {
                pkey = formattags["tag"].c_str();
                width += 5;
              }

              snprintf(lenformat, sizeof(lenformat) - 1, "%%%ds", width - 6);
              snprintf(headline, sizeof(headline) - 1, lenformat, pkey.c_str());
              std::string sline = headline;

              if (sline.length() != (width - 6))
              {
                sline.erase(0, ((sline.length() - width + 6 + 3) > 0) ?
                            (sline.length() - width + 6 + 3) : 0);
                sline.insert(0, "...");
              }

              if (!formattags.count("tag"))
              {
                header += "#";
                header += "avg(";
                header += sline;
                header += ")";
              }
              else
              {
                header += "#";
                header += sline;
              }
            }
          } // end not geotag case
        }

        if (formattags.count("sig"))
        {
          if (!outdepth)
          {
            snprintf(tmpline, sizeof(tmpline) - 1, lformat,
                     SigmaDouble(formattags["sig"].c_str(), false));
          }
          else
          {
            snprintf(tmpline, sizeof(tmpline) - 1, lformat,
                     (*doubleStats[formattags["sig"].c_str()]->getStdDevs())[l]);
          }

          if ((formattags["format"].find("+") != std::string::npos))
          {
            std::string ssize;

            if (!outdepth)
            {
              eos::common::StringConversion::GetReadableSizeString(
                  ssize, (unsigned long long) SigmaDouble(formattags["sig"].c_str(), false),
                  formattags["unit"].c_str());
            }
            else
            {
              eos::common::StringConversion::GetReadableSizeString(
                  ssize, (unsigned long long)(
                      *doubleStats[formattags["sig"].c_str()]->getStdDevs())[l],
                  formattags["unit"].c_str());
            }

            snprintf(line, sizeof(line) - 1, lenformat, ssize.c_str());
          }
          else
          {
            snprintf(line, sizeof(line) - 1, lenformat, tmpline);
          }

          if (buildheader)
          {
            char headline[1024];
            char lenformat[1024];
            XrdOucString pkey = formattags["sig"].c_str();
            pkey.replace("stat.statfs.", "");
            pkey.replace("stat.", "");
            pkey.replace("cfg.", "");

            if (formattags.count("tag"))
            {
              pkey = formattags["tag"].c_str();
              width += 5;
            }

            snprintf(lenformat, sizeof(lenformat) - 1, "%%%ds", width - 6);
            snprintf(headline, sizeof(headline) - 1, lenformat, pkey.c_str());
            std::string sline = headline;

            if (sline.length() != (width - 6))
            {
              sline.erase(0, ((sline.length() - width + 6 + 3) > 0) ?
                          (sline.length() - width + 6 + 3) : 0);
              sline.insert(0, "...");
            }

            if (!formattags.count("tag"))
            {
              header += "#";
              header += "sig(";
              header += sline;
              header += ")";
            }
            else
            {
              header += "#";
              header += sline;
            }
          }
        }

        if (formattags.count("maxdev"))
        {
          if (!outdepth)
          {
            snprintf(tmpline, sizeof(tmpline) - 1, lformat,
                     MaxAbsDeviation(formattags["maxdev"].c_str(), false));
          }
          else
          {
            snprintf(tmpline, sizeof(tmpline) - 1, lformat,
                     (*doubleStats[formattags["maxdev"].c_str()]->getMaxAbsDevs())[l]);
          }

          if ((formattags["format"].find("+") != std::string::npos))
          {
            std::string ssize;

            if (!outdepth)
            {
              eos::common::StringConversion::GetReadableSizeString(
                  ssize, (unsigned long long) MaxAbsDeviation(formattags["maxdev"].c_str(),
                                                              false),
                  formattags["unit"].c_str());
            }
            else
            {
              eos::common::StringConversion::GetReadableSizeString(
                  ssize, (unsigned long long)(
                      *doubleStats[formattags["maxdev"].c_str()]->getMaxAbsDevs())[l],
                  formattags["unit"].c_str());
            }

            snprintf(line, sizeof(line) - 1, lenformat, ssize.c_str());
          }
          else
          {
            snprintf(line, sizeof(line) - 1, lenformat, tmpline);
          }

          if (buildheader)
          {
            char headline[1024];
            char lenformat[1024];
            XrdOucString pkey = formattags["maxdev"].c_str();
            pkey.replace("stat.statfs.", "");
            pkey.replace("stat.", "");
            pkey.replace("cfg.", "");

            if (formattags.count("tag"))
            {
              pkey = formattags["tag"].c_str();
              width += 5;
            }

            snprintf(lenformat, sizeof(lenformat) - 1, "%%%ds", width - 6);
            snprintf(headline, sizeof(headline) - 1, lenformat, pkey.c_str());
            std::string sline = headline;

            if (sline.length() != (width - 6))
            {
              sline.erase(0, ((sline.length() - width + 6 + 3) > 0) ?
                          (sline.length() - width + 6 + 3) : 0);
              sline.insert(0, "...");
            }

            if (!formattags.count("tag"))
            {
              header += "#";
              header += "dev(";
              header += sline;
              header += ")";
            }
            else
            {
              header += "#";
              header += sline;
            }
          }
        }

        if ((formattags["format"].find("o") != std::string::npos))
        {
          char keyval[4096];
          buildheader = false; // auto disable header

          if (formattags.count("member"))
          {
            snprintf(keyval, sizeof(keyval) - 1, "%s=%s", formattags["member"].c_str(),
                     line);
          }

          if (formattags.count("sum"))
          {
            snprintf(keyval, sizeof(keyval) - 1, "sum.%s=%s", formattags["sum"].c_str(),
                     line);
          }

          if (formattags.count("avg"))
          {
            snprintf(keyval, sizeof(keyval) - 1, "avg.%s=%s", formattags["avg"].c_str(),
                     line);
          }

          if (formattags.count("sig"))
          {
            snprintf(keyval, sizeof(keyval) - 1, "sig.%s=%s", formattags["sig"].c_str(),
                     line);
          }

          if (formattags.count("maxdev"))
          {
            snprintf(keyval, sizeof(keyval) - 1, "dev.%s=%s", formattags["maxdev"].c_str(),
                     line);
          }

          body += keyval;
        }
        else
        {
          std::string sline = line;

          if (sline.length() > width)
          {
            sline.erase(0, ((sline.length() - width + 3) > 0) ? (sline.length() - width + 3)
                        : 0);
            sline.insert(0, "...");
          }

          body += sline;
        }
      }

      if (formattags.count("sep") && body.size() && ((*body.rbegin()) != '\n'))
      {
        // don't add the separator if there is nothing in the line before
        // add the desired seperator
        body += formattags["sep"];

        if (buildheader)
        {
          header += formattags["sep"];
        }
      }
    }

    body += "\n";
  } // l from 0 to nLines

  //---------------------------------------------------------------------------------------
  if (listformat.length())
  {
    bool first = true;

    // If a format was given for the filesystem children, forward the print to
    // the filesystems
    for (auto it = begin(); it != end(); it++)
    {
      std::string lbody;
      bool matches = true;
      FsView::gFsView.mIdView[*it]->Print(lbody, listformat);

      // Apply each selection as a find match in the string
      if (selections.size())
      {
        for (size_t i = 0; i < selections.size(); i++)
        {
          if (selections[i].substr(0, 6) == "space:")
            continue;

          if (lbody.find(selections[i]) == std::string::npos)
            matches = false;
        }
      }

      if (matches)
        body += lbody;

      if (first)
      {
        // Put the header format only in the first node printout
        first = false;

        if ((listformat.find("header=1:")) == 0)
        {
          listformat.erase(0, 9);
        }
      }
    }
  }

  if (header.size())
  {
    std::string line = "";
    line += "#";

    for (unsigned int i = 0; i < (header.length() - 1); i++)
    {
      line += "-";
    }

    line += "\n";
    out += line;
    out += header;
    out += "\n";
    out += line;
    out += body;
  }
  else
  {
    out += body;
  }
}

#ifndef EOSMGMFSVIEWTEST

//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
FsGroup::~FsGroup()
{}

//------------------------------------------------------------------------------
// If a filesystem has not yet these parameters defined, we inherit them from
// the space configuration. This function has to be called with the a read lock
// on the View Mutex! It return true if the fs was modified and the caller should
// evt. store the modification to the config
//------------------------------------------------------------------------------
bool
FsSpace::ApplySpaceDefaultParameters(eos::mgm::FileSystem* fs, bool force)
{
  if (!fs)
    return false;

  bool modified = false;
  eos::common::FileSystem::fs_snapshot_t snapshot;

  if (fs->SnapShotFileSystem(snapshot, false))
  {
    if (force || (!snapshot.mScanInterval))
    {
      // try to apply the default
      if (GetConfigMember("scaninterval").length())
      {
        fs->SetString("scaninterval", GetConfigMember("scaninterval").c_str());
        modified = true;
      }
    }

    if (force || (!snapshot.mGracePeriod))
    {
      // try to apply the default
      if (GetConfigMember("graceperiod").length())
      {
        fs->SetString("graceperiod", GetConfigMember("graceperiod").c_str());
        modified = true;
      }
    }

    if (force || (!snapshot.mDrainPeriod))
    {
      // try to apply the default
      if (GetConfigMember("drainperiod").length())
      {
        fs->SetString("drainperiod", GetConfigMember("drainperiod").c_str());
        modified = true;
      }
    }

    if (force || (!snapshot.mHeadRoom))
    {
      // try to apply the default
      if (GetConfigMember("headroom").length())
      {
        fs->SetString("headroom", GetConfigMember("headroom").c_str())
            ;
        modified = true;
      }
    }
  }

  return modified;
}

//------------------------------------------------------------------------------
// Re-evaluates the drainnig states in all groups and resets the state
//------------------------------------------------------------------------------
void
FsSpace::ResetDraining()
{
  eos_static_info("msg=\"reset drain state\" space=\"%s\"", mName.c_str());
  eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);

  // Iterate over all groups in this space
  for (auto sgit = FsView::gFsView.mSpaceGroupView[mName].begin();
       sgit != FsView::gFsView.mSpaceGroupView[mName].end();
       sgit++)
  {
    bool setactive = false;
    std::string lGroup = (*sgit)->mName;
    FsGroup::const_iterator git;

    for (git = (*sgit)->begin();
         git != (*sgit)->end(); git++)
    {
      if (FsView::gFsView.mIdView.count(*git))
      {
        int drainstatus =
            (eos::common::FileSystem::GetDrainStatusFromString(
                FsView::gFsView.mIdView[*git]->GetString("stat.drain").c_str())
             );

        if ((drainstatus == eos::common::FileSystem::kDraining) ||
            (drainstatus == eos::common::FileSystem::kDrainStalling))
        {
          // if any mGroup filesystem is draining, all the others have
          // to enable the pull for draining!
          setactive = true;
        }
      }
    }

    // if the mGroup get's disabled we stop the draining
    if (FsView::gFsView.mGroupView[lGroup]->GetConfigMember("status") != "on")
    {
      setactive = false;
    }

    for (git = (*sgit)->begin();
         git != (*sgit)->end(); git++)
    {
      eos::mgm::FileSystem* fs = FsView::gFsView.mIdView[*git];

      if (fs)
      {
        if (setactive)
        {
          if (fs->GetString("stat.drainer") != "on")
          {
            fs->SetString("stat.drainer", "on");
          }
        }
        else
        {
          if (fs->GetString("stat.drainer") != "off")
          {
            fs->SetString("stat.drainer", "off");
          }
        }

        eos_static_info("fsid=%05d state=%s", fs->GetId(),
                        fs->GetString("stat.drainer").c_str());
      }
    }
  }
}
#endif

EOSMGMNAMESPACE_END
