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

//------------------------------------------------------------------------------
// author: Lukasz Janyst <ljanyst@cern.ch>
// desc:   HierarchicalView test
//------------------------------------------------------------------------------

#include <cppunit/extensions/HelperMacros.h>

#include <stdint.h>
#include <unistd.h>
#include <sstream>
#include <vector>
#include <algorithm>
#include <numeric>
#include <pthread.h>

#include "namespace/utils/TestHelpers.hh"
#include "namespace/interface/IContainerMD.hh"
#include "namespace/interface/ContainerIterators.hh"
#include "namespace/ns_in_memory/views/HierarchicalView.hh"
#include "namespace/ns_in_memory/accounting/QuotaStats.hh"
#include "namespace/ns_in_memory/persistency/ChangeLogContainerMDSvc.hh"
#include "namespace/ns_in_memory/persistency/ChangeLogFileMDSvc.hh"


//------------------------------------------------------------------------------
// Declaration
//------------------------------------------------------------------------------
class HierarchicalViewTest: public CppUnit::TestCase
{
public:
  CPPUNIT_TEST_SUITE(HierarchicalViewTest);
  CPPUNIT_TEST(reloadTest);
  CPPUNIT_TEST(quotaTest);
  CPPUNIT_TEST(lostContainerTest);
  CPPUNIT_TEST(onlineCompactingTest);
  CPPUNIT_TEST_SUITE_END();

  void reloadTest();
  void quotaTest();
  void lostContainerTest();
  void onlineCompactingTest();
};

CPPUNIT_TEST_SUITE_REGISTRATION(HierarchicalViewTest);

//------------------------------------------------------------------------------
// Concrete implementation tests
//------------------------------------------------------------------------------
void HierarchicalViewTest::reloadTest()
{
  try {
    std::shared_ptr<eos::IContainerMDSvc> contSvc =
      std::shared_ptr<eos::IContainerMDSvc>(new eos::ChangeLogContainerMDSvc());
    std::shared_ptr<eos::IFileMDSvc> fileSvc =
      std::shared_ptr<eos::IFileMDSvc>(new eos::ChangeLogFileMDSvc());
    std::shared_ptr<eos::IView> view =
      std::shared_ptr<eos::IView>(new eos::HierarchicalView());
    fileSvc->setContMDService(contSvc.get());
    contSvc->setFileMDService(fileSvc.get());
    std::map<std::string, std::string> fileSettings;
    std::map<std::string, std::string> contSettings;
    std::map<std::string, std::string> settings;
    std::string fileNameFileMD = getTempName("/tmp", "eosns");
    std::string fileNameContMD = getTempName("/tmp", "eosns");
    contSettings["changelog_path"] = fileNameContMD;
    fileSettings["changelog_path"] = fileNameFileMD;
    fileSvc->configure(fileSettings);
    contSvc->configure(contSettings);
    view->setContainerMDSvc(contSvc.get());
    view->setFileMDSvc(fileSvc.get());
    view->configure(settings);
    view->initialize();
    std::shared_ptr<eos::IContainerMD> cont1 =
      view->createContainer("/test/embed/embed1", true);
    std::shared_ptr<eos::IContainerMD> cont2 =
      view->createContainer("/test/embed/embed2", true);
    std::shared_ptr<eos::IContainerMD> cont3 =
      view->createContainer("/test/embed/embed3", true);
    std::shared_ptr<eos::IContainerMD> cont4 =
      view->createContainer("/test/embed/embed4", true);
    std::shared_ptr<eos::IContainerMD> root  = view->getContainer("/");
    std::shared_ptr<eos::IContainerMD> test  = view->getContainer("/test");
    std::shared_ptr<eos::IContainerMD> embed = view->getContainer("/test/embed");
    CPPUNIT_ASSERT(root != 0);
    CPPUNIT_ASSERT(root->getId() == root->getParentId());
    CPPUNIT_ASSERT(test != 0);
    CPPUNIT_ASSERT(test->findContainer("embed") != 0);
    CPPUNIT_ASSERT(embed != 0);
    CPPUNIT_ASSERT(embed->findContainer("embed1") != 0);
    CPPUNIT_ASSERT(embed->findContainer("embed2") != 0);
    CPPUNIT_ASSERT(embed->findContainer("embed3") != 0);
    CPPUNIT_ASSERT(cont1->getName() == embed->findContainer("embed1")->getName());
    CPPUNIT_ASSERT(cont2->getName() == embed->findContainer("embed2")->getName());
    CPPUNIT_ASSERT(cont3->getName() == embed->findContainer("embed3")->getName());
    view->removeContainer("/test/embed/embed2");
    CPPUNIT_ASSERT(embed->findContainer("embed2") == 0);
    view->createFile("/test/embed/file1");
    view->createFile("/test/embed/file2");
    view->createFile("/test/embed/embed1/file1");
    view->createFile("/test/embed/embed1/file2");
    view->createFile("/test/embed/embed1/file3");
    std::shared_ptr<eos::IFileMD> fileR =
      view->createFile("/test/embed/embed1/fileR");
    CPPUNIT_ASSERT(view->getFile("/test/embed/file1"));
    CPPUNIT_ASSERT(view->getFile("/test/embed/file2"));
    CPPUNIT_ASSERT(view->getFile("/test/embed/embed1/file1"));
    CPPUNIT_ASSERT(view->getFile("/test/embed/embed1/file2"));
    CPPUNIT_ASSERT(view->getFile("/test/embed/embed1/file3"));
    //--------------------------------------------------------------------------
    // Rename
    //--------------------------------------------------------------------------
    CPPUNIT_ASSERT_NO_THROW(view->renameContainer(cont4.get(), "embed4.renamed"));
    CPPUNIT_ASSERT(cont4->getName() == "embed4.renamed");
    CPPUNIT_ASSERT_THROW(view->renameContainer(cont4.get(), "embed1"),
                         eos::MDException);
    CPPUNIT_ASSERT_THROW(view->renameContainer(cont4.get(), "embed1/asd"),
                         eos::MDException);
    CPPUNIT_ASSERT_NO_THROW(view->getContainer("/test/embed/embed4.renamed"));
    CPPUNIT_ASSERT_NO_THROW(view->renameFile(fileR.get(), "fileR.renamed"));
    CPPUNIT_ASSERT(fileR->getName() == "fileR.renamed");
    CPPUNIT_ASSERT_THROW(view->renameFile(fileR.get(), "file1"), eos::MDException);
    CPPUNIT_ASSERT_THROW(view->renameFile(fileR.get(), "file1/asd"),
                         eos::MDException);
    CPPUNIT_ASSERT_NO_THROW(view->getFile("/test/embed/embed1/fileR.renamed"));
    CPPUNIT_ASSERT_THROW(view->renameContainer(root.get(), "rename"),
                         eos::MDException);
    //--------------------------------------------------------------------------
    // Test the "reverse" lookup
    //--------------------------------------------------------------------------
    std::shared_ptr<eos::IFileMD> file = view->getFile("/test/embed/embed1/file3");
    std::shared_ptr<eos::IContainerMD> container =
      view->getContainer("/test/embed/embed1");
    CPPUNIT_ASSERT(view->getUri(container.get()) == "/test/embed/embed1/");
    CPPUNIT_ASSERT(view->getUri(file.get()) == "/test/embed/embed1/file3");
    CPPUNIT_ASSERT_THROW(view->getUri((eos::IFileMD*)0), eos::MDException);
    std::shared_ptr<eos::IFileMD> toBeDeleted =
      view->getFile("/test/embed/embed1/file2");
    toBeDeleted->addLocation(12);
    //--------------------------------------------------------------------------
    // This should not succeed since the file should have a replica
    //--------------------------------------------------------------------------
    CPPUNIT_ASSERT_THROW(view->removeFile(toBeDeleted.get()), eos::MDException);
    //--------------------------------------------------------------------------
    // We unlink the file - at this point the file should not be attached to the
    // hierarchy bu should still be accessible by id and thus the md pointer
    // should stay valid
    //--------------------------------------------------------------------------
    view->unlinkFile("/test/embed/embed1/file2");
    CPPUNIT_ASSERT_THROW(view->getFile("/test/embed/embed1/file2"),
                         eos::MDException);
    CPPUNIT_ASSERT(cont1->findFile("file2") == 0);
    //--------------------------------------------------------------------------
    // We remove the replicas and the file
    //--------------------------------------------------------------------------
    eos::IFileMD::id_t id = toBeDeleted->getId();
    toBeDeleted->clearUnlinkedLocations();
    CPPUNIT_ASSERT_NO_THROW(view->removeFile(toBeDeleted.get()));
    CPPUNIT_ASSERT_THROW(view->getFileMDSvc()->getFileMD(id), eos::MDException);
    view->finalize();
    view->initialize();
    CPPUNIT_ASSERT(view->getContainer("/"));
    CPPUNIT_ASSERT(view->getContainer("/test"));
    CPPUNIT_ASSERT(view->getContainer("/test/embed"));
    CPPUNIT_ASSERT(view->getContainer("/test/embed/embed1"));
    CPPUNIT_ASSERT(view->getFile("/test/embed/file1"));
    CPPUNIT_ASSERT(view->getFile("/test/embed/file2"));
    CPPUNIT_ASSERT(view->getFile("/test/embed/embed1/file1"));
    CPPUNIT_ASSERT(view->getFile("/test/embed/embed1/file3"));
    CPPUNIT_ASSERT_NO_THROW(view->getContainer("/test/embed/embed4.renamed"));
    CPPUNIT_ASSERT_NO_THROW(view->getFile("/test/embed/embed1/fileR.renamed"));
    view->finalize();
    unlink(fileNameFileMD.c_str());
    unlink(fileNameContMD.c_str());
  } catch (eos::MDException& e) {
    CPPUNIT_ASSERT_MESSAGE(e.getMessage().str(), false);
  }
}

//------------------------------------------------------------------------------
// File size mapping function
//------------------------------------------------------------------------------
static uint64_t mapSize(const eos::IFileMD* file)
{
  eos::IFileMD::layoutId_t lid = file->getLayoutId();

  if (lid > 3) {
    eos::MDException e(ENOENT);
    e.getMessage() << "Location does not exist" << std::endl;
    throw (e);
  }

  return lid * file->getSize();
}

//------------------------------------------------------------------------------
// Create files at given path
//------------------------------------------------------------------------------
static void createFiles(const std::string&                          path,
                        std::shared_ptr<eos::IView>                 view,
                        std::map<uid_t, eos::QuotaNodeCore::UsageInfo>& users,
                        std::map<gid_t, eos::QuotaNodeCore::UsageInfo>& groups)
{
  eos::IQuotaNode* node = view->getQuotaNode(view->getContainer(path).get());

  for (int i = 0; i < 1000; ++i) {
    std::ostringstream p;
    p << path << "file" << i;
    std::shared_ptr<eos::IFileMD> file = view->createFile(p.str());
    file->setCUid(random() % 10 + 1);
    file->setCGid(random() % 3 + 1);
    file->setSize(random() % 1000000 + 1);
    file->setLayoutId(random() % 3 + 1);
    view->updateFileStore(file.get());
    node->addFile(file.get());
    uint64_t size = mapSize(file.get());
    eos::QuotaNodeCore::UsageInfo& user  = users[file->getCUid()];
    eos::QuotaNodeCore::UsageInfo& group = groups[file->getCGid()];
    user.space += file->getSize();
    user.physicalSpace += size;
    user.files++;
    group.space += file->getSize();
    group.physicalSpace += size;
    group.files++;
  }
}

//------------------------------------------------------------------------------
// Quota test
//------------------------------------------------------------------------------
void HierarchicalViewTest::quotaTest()
{
  srandom(time(0));
  //----------------------------------------------------------------------------
  // Initialize the system
  //----------------------------------------------------------------------------
  std::shared_ptr<eos::IContainerMDSvc> contSvc =
    std::shared_ptr<eos::IContainerMDSvc>(new eos::ChangeLogContainerMDSvc());
  std::shared_ptr<eos::IFileMDSvc> fileSvc =
    std::shared_ptr<eos::IFileMDSvc>(new eos::ChangeLogFileMDSvc());
  std::shared_ptr<eos::IView> view =
    std::shared_ptr<eos::IView>(new eos::HierarchicalView());
  fileSvc->setContMDService(contSvc.get());
  contSvc->setFileMDService(fileSvc.get());
  std::map<std::string, std::string> fileSettings;
  std::map<std::string, std::string> contSettings;
  std::map<std::string, std::string> settings;
  std::string fileNameFileMD = getTempName("/tmp", "eosns");
  std::string fileNameContMD = getTempName("/tmp", "eosns");
  contSettings["changelog_path"] = fileNameContMD;
  fileSettings["changelog_path"] = fileNameFileMD;
  fileSvc->configure(contSettings);
  contSvc->configure(fileSettings);
  view->setContainerMDSvc(contSvc.get());
  view->setFileMDSvc(fileSvc.get());
  view->configure(settings);
  view->getQuotaStats()->registerSizeMapper(mapSize);
  CPPUNIT_ASSERT_NO_THROW(view->initialize());
  //----------------------------------------------------------------------------
  // Test quota node melding
  //----------------------------------------------------------------------------
  std::map<uid_t, eos::QuotaNodeCore::UsageInfo> users;
  std::map<gid_t, eos::QuotaNodeCore::UsageInfo> groups;
  std::map<uid_t, eos::QuotaNodeCore::UsageInfo>::iterator userIt;
  std::map<gid_t, eos::QuotaNodeCore::UsageInfo>::iterator groupIt;
  eos::IQuotaNode* meldNode1 = new eos::QuotaNode(0, 1);
  eos::IQuotaNode* meldNode2 = new eos::QuotaNode(0, 2);

  for (int i = 0; i < 10000; ++i) {
    uid_t uid = random();
    gid_t gid = random();
    eos::QuotaNodeCore::UsageInfo& user  = users[uid];
    eos::QuotaNodeCore::UsageInfo& group = groups[gid];
    uint64_t userSpace          = random() % 100000;
    uint64_t userPhysicalSpace  = random() % 100000;
    uint64_t userFiles          = random() % 1000;
    uint64_t groupSpace         = random() % 100000;
    uint64_t groupPhysicalSpace = random() % 100000;
    uint64_t groupFiles         = random() % 1000;

    if (random() % 3) {
      static_cast<eos::QuotaNode*>(meldNode1)->changeSpaceUser(uid, userSpace);
      static_cast<eos::QuotaNode*>(meldNode1)->changePhysicalSpaceUser(uid,
          userPhysicalSpace);
      static_cast<eos::QuotaNode*>(meldNode1)->changeNumFilesUser(uid, userFiles);
      user.space          += userSpace;
      user.physicalSpace  += userPhysicalSpace;
      user.files          += userFiles;
      static_cast<eos::QuotaNode*>(meldNode1)->changeSpaceGroup(gid, groupSpace);
      static_cast<eos::QuotaNode*>(meldNode1)->changePhysicalSpaceGroup(gid,
          groupPhysicalSpace);
      static_cast<eos::QuotaNode*>(meldNode1)->changeNumFilesGroup(gid, groupFiles);
      group.space         += groupSpace;
      group.physicalSpace += groupPhysicalSpace;
      group.files         += groupFiles;
    }

    if (random() % 3) {
      static_cast<eos::QuotaNode*>(meldNode2)->changeSpaceUser(uid, userSpace);
      static_cast<eos::QuotaNode*>(meldNode2)->changePhysicalSpaceUser(uid,
          userPhysicalSpace);
      static_cast<eos::QuotaNode*>(meldNode2)->changeNumFilesUser(uid, userFiles);
      user.space          += userSpace;
      user.physicalSpace  += userPhysicalSpace;
      user.files          += userFiles;
      static_cast<eos::QuotaNode*>(meldNode2)->changeSpaceGroup(gid, groupSpace);
      static_cast<eos::QuotaNode*>(meldNode2)->changePhysicalSpaceGroup(gid,
          groupPhysicalSpace);
      static_cast<eos::QuotaNode*>(meldNode2)->changeNumFilesGroup(gid, groupFiles);
      group.space         += groupSpace;
      group.physicalSpace += groupPhysicalSpace;
      group.files         += groupFiles;
    }
  }

  meldNode1->meld(meldNode2);

  for (userIt = users.begin(); userIt != users.end(); ++userIt) {
    CPPUNIT_ASSERT(userIt->second.space == meldNode1->getUsedSpaceByUser(
                     userIt->first));
    CPPUNIT_ASSERT(userIt->second.physicalSpace ==
                   meldNode1->getPhysicalSpaceByUser(userIt->first));
    CPPUNIT_ASSERT(userIt->second.files == meldNode1->getNumFilesByUser(
                     userIt->first));
  }

  for (groupIt = groups.begin(); groupIt != groups.end(); ++groupIt) {
    CPPUNIT_ASSERT(groupIt->second.space == meldNode1->getUsedSpaceByGroup(
                     groupIt->first));
    CPPUNIT_ASSERT(groupIt->second.physicalSpace ==
                   meldNode1->getPhysicalSpaceByGroup(groupIt->first));
    CPPUNIT_ASSERT(groupIt->second.files == meldNode1->getNumFilesByGroup(
                     groupIt->first));
  }

  delete meldNode1;
  delete meldNode2;
  //----------------------------------------------------------------------------
  // Create some structures, insert quota nodes and test their correctness
  //----------------------------------------------------------------------------
  std::shared_ptr<eos::IContainerMD> cont1 =
    view->createContainer("/test/embed/embed1", true);
  std::shared_ptr<eos::IContainerMD> cont2 =
    view->createContainer("/test/embed/embed2", true);
  std::shared_ptr<eos::IContainerMD> cont3 =
    view->createContainer("/test/embed/embed3", true);
  std::shared_ptr<eos::IContainerMD> cont4 = view->getContainer("/test/embed");
  std::shared_ptr<eos::IContainerMD> cont5 = view->getContainer("/test");
  eos::IQuotaNode* qnCreated1 = view->registerQuotaNode(cont1.get());
  eos::IQuotaNode* qnCreated2 = view->registerQuotaNode(cont3.get());
  eos::IQuotaNode* qnCreated3 = view->registerQuotaNode(cont5.get());
  CPPUNIT_ASSERT_THROW(view->registerQuotaNode(cont1.get()), eos::MDException);
  CPPUNIT_ASSERT(qnCreated1);
  CPPUNIT_ASSERT(qnCreated2);
  CPPUNIT_ASSERT(qnCreated3);
  eos::IQuotaNode* qn1 = view->getQuotaNode(cont1.get());
  eos::IQuotaNode* qn2 = view->getQuotaNode(cont2.get());
  eos::IQuotaNode* qn3 = view->getQuotaNode(cont3.get());
  eos::IQuotaNode* qn4 = view->getQuotaNode(cont4.get());
  eos::IQuotaNode* qn5 = view->getQuotaNode(cont5.get());
  CPPUNIT_ASSERT(qn1);
  CPPUNIT_ASSERT(qn2);
  CPPUNIT_ASSERT(qn3);
  CPPUNIT_ASSERT(qn4);
  CPPUNIT_ASSERT(qn5);
  CPPUNIT_ASSERT(qn2 == qn5);
  CPPUNIT_ASSERT(qn4 == qn5);
  CPPUNIT_ASSERT(qn1 != qn5);
  CPPUNIT_ASSERT(qn3 != qn5);
  CPPUNIT_ASSERT(qn3 != qn2);
  //----------------------------------------------------------------------------
  // Create some files
  //----------------------------------------------------------------------------
  std::map<uid_t, eos::QuotaNodeCore::UsageInfo> users1;
  std::map<gid_t, eos::QuotaNodeCore::UsageInfo> groups1;
  std::string path1 = "/test/embed/embed1/";
  createFiles(path1, view, users1, groups1);
  std::map<uid_t, eos::QuotaNodeCore::UsageInfo> users2;
  std::map<gid_t, eos::QuotaNodeCore::UsageInfo> groups2;
  std::string path2 = "/test/embed/embed2/";
  createFiles(path2, view, users2, groups2);
  std::map<uid_t, eos::QuotaNodeCore::UsageInfo> users3;
  std::map<gid_t, eos::QuotaNodeCore::UsageInfo> groups3;
  std::string path3 = "/test/embed/embed3/";
  createFiles(path3, view, users3, groups3);
  //----------------------------------------------------------------------------
  // Verify correctness
  //----------------------------------------------------------------------------
  eos::IQuotaNode* node1 = view->getQuotaNode(view->getContainer(path1).get());
  eos::IQuotaNode* node2 = view->getQuotaNode(view->getContainer(path2).get());

  for (int i = 1; i <= 10; ++i) {
    CPPUNIT_ASSERT(node1->getPhysicalSpaceByUser(i) == users1[i].physicalSpace);
    CPPUNIT_ASSERT(node2->getPhysicalSpaceByUser(i) == users2[i].physicalSpace);
    CPPUNIT_ASSERT(node1->getUsedSpaceByUser(i) == users1[i].space);
    CPPUNIT_ASSERT(node2->getUsedSpaceByUser(i) == users2[i].space);
    CPPUNIT_ASSERT(node1->getNumFilesByUser(i)  == users1[i].files);
    CPPUNIT_ASSERT(node2->getNumFilesByUser(i)  == users2[i].files);
  }

  for (int i = 1; i <= 3; ++i) {
    CPPUNIT_ASSERT(node1->getPhysicalSpaceByGroup(i) == groups1[i].physicalSpace);
    CPPUNIT_ASSERT(node2->getPhysicalSpaceByGroup(i) == groups2[i].physicalSpace);
    CPPUNIT_ASSERT(node1->getUsedSpaceByGroup(i) == groups1[i].space);
    CPPUNIT_ASSERT(node2->getUsedSpaceByGroup(i) == groups2[i].space);
    CPPUNIT_ASSERT(node1->getNumFilesByGroup(i)  == groups1[i].files);
    CPPUNIT_ASSERT(node2->getNumFilesByGroup(i)  == groups2[i].files);
  }

  //----------------------------------------------------------------------------
  // Restart and check if the quota stats are reloaded correctly
  //----------------------------------------------------------------------------
  CPPUNIT_ASSERT_NO_THROW(view->finalize());
  delete view->getQuotaStats();
  view->setQuotaStats(new eos::QuotaStats);
  view->getQuotaStats()->registerSizeMapper(mapSize);
  CPPUNIT_ASSERT_NO_THROW(view->initialize());
  node1 = view->getQuotaNode(view->getContainer(path1).get());
  node2 = view->getQuotaNode(view->getContainer(path2).get());
  CPPUNIT_ASSERT(node1);
  CPPUNIT_ASSERT(node2);

  for (int i = 1; i <= 10; ++i) {
    CPPUNIT_ASSERT(node1->getPhysicalSpaceByUser(i) == users1[i].physicalSpace);
    CPPUNIT_ASSERT(node2->getPhysicalSpaceByUser(i) == users2[i].physicalSpace);
    CPPUNIT_ASSERT(node1->getUsedSpaceByUser(i) == users1[i].space);
    CPPUNIT_ASSERT(node2->getUsedSpaceByUser(i) == users2[i].space);
    CPPUNIT_ASSERT(node1->getNumFilesByUser(i)  == users1[i].files);
    CPPUNIT_ASSERT(node2->getNumFilesByUser(i)  == users2[i].files);
  }

  for (int i = 1; i <= 3; ++i) {
    CPPUNIT_ASSERT(node1->getPhysicalSpaceByGroup(i) == groups1[i].physicalSpace);
    CPPUNIT_ASSERT(node2->getPhysicalSpaceByGroup(i) == groups2[i].physicalSpace);
    CPPUNIT_ASSERT(node1->getUsedSpaceByGroup(i) == groups1[i].space);
    CPPUNIT_ASSERT(node2->getUsedSpaceByGroup(i) == groups2[i].space);
    CPPUNIT_ASSERT(node1->getNumFilesByGroup(i)  == groups1[i].files);
    CPPUNIT_ASSERT(node2->getNumFilesByGroup(i)  == groups2[i].files);
  }

  //----------------------------------------------------------------------------
  // Remove the quota nodes on /test/embed/embed1 and /dest/embed/embed2
  // and check if the on /test has been updated
  //----------------------------------------------------------------------------
  eos::IQuotaNode* parentNode = 0;
  CPPUNIT_ASSERT_NO_THROW(parentNode =
                            view->getQuotaNode(view->getContainer("/test").get()));
  CPPUNIT_ASSERT_NO_THROW(view->removeQuotaNode(view->getContainer(path1).get()));

  for (int i = 1; i <= 10; ++i) {
    CPPUNIT_ASSERT(parentNode->getPhysicalSpaceByUser(i) ==
                   users1[i].physicalSpace + users2[i].physicalSpace);
    CPPUNIT_ASSERT(parentNode->getUsedSpaceByUser(i)     ==
                   users1[i].space + users2[i].space);
    CPPUNIT_ASSERT(parentNode->getNumFilesByUser(i)      ==
                   users1[i].files + users2[i].files);
  }

  for (int i = 1; i <= 3; ++i) {
    CPPUNIT_ASSERT(parentNode->getPhysicalSpaceByGroup(i) ==
                   groups1[i].physicalSpace + groups2[i].physicalSpace);
    CPPUNIT_ASSERT(parentNode->getUsedSpaceByGroup(i)     ==
                   groups1[i].space + groups2[i].space);
    CPPUNIT_ASSERT(parentNode->getNumFilesByGroup(i)      ==
                   groups1[i].files + groups2[i].files);
  }

  CPPUNIT_ASSERT_NO_THROW(view->removeQuotaNode(view->getContainer(path3).get()));
  CPPUNIT_ASSERT_THROW(view->removeQuotaNode(view->getContainer(path3).get()),
                       eos::MDException);

  for (int i = 1; i <= 10; ++i) {
    CPPUNIT_ASSERT(parentNode->getPhysicalSpaceByUser(i)  ==
                   users1[i].physicalSpace + users2[i].physicalSpace + users3[i].physicalSpace);
    CPPUNIT_ASSERT(parentNode->getUsedSpaceByUser(i)      ==
                   users1[i].space + users2[i].space + users3[i].space);
    CPPUNIT_ASSERT(parentNode->getNumFilesByUser(i)       ==
                   users1[i].files + users2[i].files + users3[i].files);
  }

  for (int i = 1; i <= 3; ++i) {
    CPPUNIT_ASSERT(parentNode->getPhysicalSpaceByGroup(i)  ==
                   groups1[i].physicalSpace + groups2[i].physicalSpace + groups3[i].physicalSpace);
    CPPUNIT_ASSERT(parentNode->getUsedSpaceByGroup(i)      ==
                   groups1[i].space + groups2[i].space + groups3[i].space);
    CPPUNIT_ASSERT(parentNode->getNumFilesByGroup(i)       ==
                   groups1[i].files + groups2[i].files + groups3[i].files);
  }

  CPPUNIT_ASSERT_NO_THROW(view->finalize());
  unlink(fileNameFileMD.c_str());
  unlink(fileNameContMD.c_str());
}

//------------------------------------------------------------------------------
// Lost container test
//------------------------------------------------------------------------------
void HierarchicalViewTest::lostContainerTest()
{
  //----------------------------------------------------------------------------
  // Initializer
  //----------------------------------------------------------------------------
  std::shared_ptr<eos::IContainerMDSvc> contSvc =
    std::shared_ptr<eos::IContainerMDSvc>(new eos::ChangeLogContainerMDSvc());
  std::shared_ptr<eos::IFileMDSvc> fileSvc =
    std::shared_ptr<eos::IFileMDSvc>(new eos::ChangeLogFileMDSvc());
  std::shared_ptr<eos::IView> view =
    std::shared_ptr<eos::IView>(new eos::HierarchicalView());
  fileSvc->setContMDService(contSvc.get());
  contSvc->setFileMDService(fileSvc.get());
  std::map<std::string, std::string> fileSettings;
  std::map<std::string, std::string> contSettings;
  std::map<std::string, std::string> settings;
  std::string fileNameFileMD = getTempName("/tmp", "eosns");
  std::string fileNameContMD = getTempName("/tmp", "eosns");
  contSettings["changelog_path"] = fileNameContMD;
  fileSettings["changelog_path"] = fileNameFileMD;
  fileSvc->configure(contSettings);
  contSvc->configure(fileSettings);
  view->setContainerMDSvc(contSvc.get());
  view->setFileMDSvc(fileSvc.get());
  view->configure(settings);
  view->initialize();
  std::shared_ptr<eos::IContainerMD> cont1 =
    view->createContainer("/test/embed/embed1", true);
  std::shared_ptr<eos::IContainerMD> cont2 =
    view->createContainer("/test/embed/embed2", true);
  std::shared_ptr<eos::IContainerMD> cont3 =
    view->createContainer("/test/embed/embed3", true);
  std::shared_ptr<eos::IContainerMD> cont4 =
    view->createContainer("/test/embed/embed1/embedembed", true);
  std::shared_ptr<eos::IContainerMD> cont5 =
    view->createContainer("/test/embed/embed3.conflict",
                          true);

  //----------------------------------------------------------------------------
  // Create some files
  //----------------------------------------------------------------------------
  for (int i = 0; i < 1000; ++i) {
    std::ostringstream s1;
    s1 << "/test/embed/embed1/file" << i;
    std::ostringstream s2;
    s2 << "/test/embed/embed2/file" << i;
    std::ostringstream s3;
    s3 << "/test/embed/embed3/file" << i;
    std::ostringstream s4;
    s4 << "/test/embed/embed1/embedembed/file" << i;
    std::ostringstream s5;
    s5 << "/test/embed/embed3.conflict/file" << i;
    std::ostringstream s6;
    s6 << "/test/embed/embed2/conflict_file" << i;
    (void) view->createFile(s1.str());
    (void) view->createFile(s2.str());
    (void) view->createFile(s3.str());
    (void) view->createFile(s4.str());
    (void) view->createFile(s5.str());
    (void) view->createFile(s6.str());
    std::shared_ptr<eos::IFileMD> file = view->getFile(s6.str());
    file->setName("conflict_file");
    view->updateFileStore(file.get());
  }

  //----------------------------------------------------------------------------
  // Remove one of the container keeping the files register with it to
  // simulate directory metadata loss
  //----------------------------------------------------------------------------
  eos::IContainerMD::id_t removedId        = cont1->getId();
  eos::IContainerMD::id_t removedEmbId     = cont4->getId();
  eos::IContainerMD::id_t conflictId       = cont3->getId();
  eos::IContainerMD::id_t conflictParentId = cont5->getParentId();
  view->getContainerMDSvc()->removeContainer(cont1.get());
  cont5->setName("embed3");
  view->getContainerMDSvc()->updateStore(cont5.get());
  //----------------------------------------------------------------------------
  // Reboot
  //----------------------------------------------------------------------------
  view->finalize();
  view->initialize();
  //----------------------------------------------------------------------------
  // Check the containers
  //----------------------------------------------------------------------------
  std::ostringstream s1;
  s1 << "/lost+found/orphans/" << removedId;
  std::ostringstream s2;
  s2 << "/lost+found/orphans/" << removedId;
  s2 << "/embedembed." << removedEmbId;
  std::ostringstream s3;
  s3 << "/lost+found/name_conflicts/" << conflictParentId;
  s3 << "/embed3." << conflictId;
  CPPUNIT_ASSERT_NO_THROW(cont1 = view->getContainer(s1.str()));
  CPPUNIT_ASSERT_NO_THROW(cont2 = view->getContainer("/test/embed/embed2"));
  CPPUNIT_ASSERT_NO_THROW(cont3 = view->getContainer("/test/embed/embed3"));
  CPPUNIT_ASSERT_NO_THROW(cont4 = view->getContainer(s2.str()));
  CPPUNIT_ASSERT_NO_THROW(cont5 = view->getContainer(s3.str()));
  std::shared_ptr<eos::IContainerMD> cont6;
  std::ostringstream s4;
  s4 << "/lost+found/name_conflicts/";
  s4 << cont2->getId();
  CPPUNIT_ASSERT_NO_THROW(cont6 = view->getContainer(s4.str()));
  CPPUNIT_ASSERT(cont1->getNumFiles() == 1000);
  CPPUNIT_ASSERT(cont2->getNumFiles() == 1001);   // 1000 + 1 conflict
  CPPUNIT_ASSERT(cont3->getNumFiles() == 1000);
  CPPUNIT_ASSERT(cont4->getNumFiles() == 1000);
  CPPUNIT_ASSERT(cont5->getNumFiles() == 1000);
  CPPUNIT_ASSERT(cont6->getNumFiles() == 999);    // 1000 conflicts - 1
  //----------------------------------------------------------------------------
  // Cleanup
  //----------------------------------------------------------------------------
  view->finalize();
  unlink(fileNameFileMD.c_str());
  unlink(fileNameContMD.c_str());
}

//------------------------------------------------------------------------------
// Compact thread
//------------------------------------------------------------------------------
struct CompactData {
  CompactData(): compactData(0), svc(0) {}
  void*                    compactData;
  eos::ChangeLogFileMDSvc* svc;
};

void* compactThread(void* arg)
{
  CompactData* cd = (CompactData*)arg;
  cd->svc->compact(cd->compactData);
  return 0;
}

//------------------------------------------------------------------------------
// Check if everything is as expected
//------------------------------------------------------------------------------
void CheckOnlineComp(std::shared_ptr<eos::IView> view, uint32_t totalFiles,
                     uint32_t changedFiles)
{
  std::shared_ptr<eos::IContainerMD> cont;
  CPPUNIT_ASSERT_NO_THROW(cont = view->getContainer("/test/"));
  uint32_t changedFound = 0;
  std::shared_ptr<eos::IFileMD> fmd;

  for (auto fit = eos::FileMapIterator(cont); fit.valid(); fit.next()) {
    fmd = cont->findFile(fit.key());

    if (fmd->getSize() == 99999) {
      ++changedFound;
    }
  }

  CPPUNIT_ASSERT(totalFiles == cont->getNumFiles());
  CPPUNIT_ASSERT(changedFiles == changedFound);
}

//------------------------------------------------------------------------------
// Online compacting test
//------------------------------------------------------------------------------
void HierarchicalViewTest::onlineCompactingTest()
{
  //----------------------------------------------------------------------------
  // Initializer
  //----------------------------------------------------------------------------
  std::shared_ptr<eos::IContainerMDSvc> contSvc =
    std::shared_ptr<eos::IContainerMDSvc>(new eos::ChangeLogContainerMDSvc());
  std::shared_ptr<eos::IFileMDSvc> fileSvc =
    std::shared_ptr<eos::IFileMDSvc>(new eos::ChangeLogFileMDSvc());
  std::shared_ptr<eos::IView> view =
    std::shared_ptr<eos::IView>(new eos::HierarchicalView());
  fileSvc->setContMDService(contSvc.get());
  contSvc->setFileMDService(fileSvc.get());
  std::map<std::string, std::string> fileSettings;
  std::map<std::string, std::string> contSettings;
  std::map<std::string, std::string> settings;
  std::string fileNameFileMD = getTempName("/tmp", "eosns");
  std::string fileNameContMD = getTempName("/tmp", "eosns");
  contSettings["changelog_path"] = fileNameContMD;
  contSvc->configure(contSettings);
  fileSettings["changelog_path"] = fileNameFileMD;
  fileSvc->configure(fileSettings);
  view->setContainerMDSvc(contSvc.get());
  view->setFileMDSvc(fileSvc.get());
  view->configure(settings);
  view->initialize();
  //----------------------------------------------------------------------------
  // Create some files
  //----------------------------------------------------------------------------
  std::shared_ptr<eos::IContainerMD> cont;
  CPPUNIT_ASSERT_NO_THROW(cont = view->createContainer("/test/", true));

  for (int i = 0; i < 10000; ++i) {
    std::ostringstream s;
    s << "/test/file" << i;
    CPPUNIT_ASSERT_NO_THROW(view->createFile(s.str()));
  }

  //----------------------------------------------------------------------------
  // Remove some files
  //----------------------------------------------------------------------------
  std::vector<int> td(10000);
  std::iota(td.begin(), td.end(), 0);
  std::random_shuffle(td.begin(), td.end());
  td.resize(1000);
  std::vector<int>::iterator it;

  for (it = td.begin(); it != td.end(); ++it) {
    std::ostringstream s;
    s << "/test/file" << *it;
    CPPUNIT_ASSERT_NO_THROW(view->removeFile(view->getFile(s.str()).get()));
  }

  std::string newFileLogName = getTempName("/tmp", "eosns");
  eos::ChangeLogFileMDSvc* clFileSvc = dynamic_cast<eos::ChangeLogFileMDSvc*>
                                       (view->getFileMDSvc());

  if (!clFileSvc) {
    fprintf(stderr, "Failed dynamic cast to ChangeLogFileMDSvc\n");
    exit(-1);
  }

  void* compData = 0;
  CPPUNIT_ASSERT_NO_THROW(compData = clFileSvc->compactPrepare(newFileLogName));
  //----------------------------------------------------------------------------
  // Start the compacting
  //----------------------------------------------------------------------------
  CompactData cd;
  cd.compactData = compData;
  cd.svc         = clFileSvc;
  pthread_t thread;
  CPPUNIT_ASSERT(pthread_create(&thread, 0, compactThread, &cd) == 0);

  //----------------------------------------------------------------------------
  // Do stuff - create some new files, modify existing ones
  //----------------------------------------------------------------------------
  for (int i = 10000; i < 20000; ++i) {
    std::ostringstream s;
    s << "/test/file" << i;
    CPPUNIT_ASSERT_NO_THROW(view->createFile(s.str()));
  }

  int changed = 0;
  std::shared_ptr<eos::IFileMD> fmd;

  for (auto fit = eos::FileMapIterator(cont); fit.valid(); fit.next()) {
    if (random() % 100 < 70) {
      fmd = cont->findFile(fit.key());
      fmd->setSize(99999);
      CPPUNIT_ASSERT_NO_THROW(view->updateFileStore(fmd.get()));
      changed++;
    }
  }

  //----------------------------------------------------------------------------
  // Commit the log and check
  //----------------------------------------------------------------------------
  CPPUNIT_ASSERT(pthread_join(thread, 0) == 0);

  for (int i = 20000; i < 21000; ++i) {
    std::ostringstream s;
    s << "/test/file" << i;
    CPPUNIT_ASSERT_NO_THROW(view->createFile(s.str()));
  }

  for (auto fit = eos::FileMapIterator(cont); fit.valid(); fit.next()) {
    fmd = cont->findFile(fit.key());

    if (fmd->getSize() == 0) {
      if (random() % 100 < 10) {
        fmd->setSize(99999);
        CPPUNIT_ASSERT_NO_THROW(view->updateFileStore(fmd.get()));
        changed++;
      }
    }
  }

  CPPUNIT_ASSERT_NO_THROW(clFileSvc->compactCommit(compData));

  //----------------------------------------------------------------------------
  // Create some new files
  //----------------------------------------------------------------------------
  for (int i = 21000; i < 22000; ++i) {
    std::ostringstream s;
    s << "/test/file" << i;
    CPPUNIT_ASSERT_NO_THROW(view->createFile(s.str()));
  }

  for (auto fit = eos::FileMapIterator(cont); fit.valid(); fit.next()) {
    fmd = cont->findFile(fit.key());

    if (fmd->getSize() == 0) {
      if (random() % 100 < 10) {
        fmd->setSize(99999);
        CPPUNIT_ASSERT_NO_THROW(view->updateFileStore(fmd.get()));
        changed++;
      }
    }
  }

  CheckOnlineComp(view, 21000, changed);
  //----------------------------------------------------------------------------
  // Reinitialize and check again
  //----------------------------------------------------------------------------
  view->finalize();
  fileSettings["changelog_path"] = newFileLogName;
  fileSvc->configure(fileSettings);
  view->initialize();
  CheckOnlineComp(view, 21000, changed);
  view->finalize();
  //----------------------------------------------------------------------------
  // Cleanup
  //----------------------------------------------------------------------------
  unlink(fileNameFileMD.c_str());
  unlink(fileNameContMD.c_str());
  unlink(newFileLogName.c_str());
}
