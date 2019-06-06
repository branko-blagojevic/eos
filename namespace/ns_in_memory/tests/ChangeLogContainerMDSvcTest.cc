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
// desc:   ChangeLog test
//------------------------------------------------------------------------------

#include <cppunit/extensions/HelperMacros.h>
#include <stdint.h>
#include <unistd.h>

#include "namespace/utils/TestHelpers.hh"
#include "namespace/ns_in_memory/persistency/ChangeLogContainerMDSvc.hh"
#include "namespace/ns_in_memory/persistency/ChangeLogFileMDSvc.hh"


//------------------------------------------------------------------------------
// Declaration
//------------------------------------------------------------------------------
class ChangeLogContainerMDSvcTest: public CppUnit::TestCase
{
  public:
    CPPUNIT_TEST_SUITE( ChangeLogContainerMDSvcTest );
    CPPUNIT_TEST( reloadTest );
    CPPUNIT_TEST_SUITE_END();

    void reloadTest();
};

CPPUNIT_TEST_SUITE_REGISTRATION( ChangeLogContainerMDSvcTest );

//------------------------------------------------------------------------------
// Concrete implementation tests
//------------------------------------------------------------------------------
void ChangeLogContainerMDSvcTest::reloadTest()
{
  try
  {
    std::shared_ptr<eos::IContainerMDSvc> containerSvc =
      std::shared_ptr<eos::IContainerMDSvc>(new eos::ChangeLogContainerMDSvc());
    std::shared_ptr<eos::IFileMDSvc> fileSvc =
      std::shared_ptr<eos::IFileMDSvc>(new eos::ChangeLogFileMDSvc());
    fileSvc->setContMDService(containerSvc.get());
    containerSvc->setFileMDService(fileSvc.get());
    std::map<std::string, std::string> config;
    std::string fileName = getTempName( "/tmp", "eosns" );
    config["changelog_path"] = fileName;
    containerSvc->configure( config );
    containerSvc->initialize();
  
    std::shared_ptr<eos::IContainerMD> container1 = containerSvc->createContainer(0);
    std::shared_ptr<eos::IContainerMD> container2 = containerSvc->createContainer(0);
    std::shared_ptr<eos::IContainerMD> container3 = containerSvc->createContainer(0);
    std::shared_ptr<eos::IContainerMD> container4 = containerSvc->createContainer(0);
    std::shared_ptr<eos::IContainerMD> container5 = containerSvc->createContainer(0);

    eos::IContainerMD::id_t id = container1->getId();

    container1->setName( "root" );
    container1->setParentId( container1->getId() );
    container2->setName( "subContLevel1-1" );
    container3->setName( "subContLevel1-2" );
    container4->setName( "subContLevel2-1" );
    container5->setName( "subContLevel2-2" );

    container5->setCUid( 17 );
    container5->setCGid( 17 );
    container5->setMode( 0750 );

    CPPUNIT_ASSERT( container5->access( 17, 12, X_OK|R_OK|W_OK ) == true);
    CPPUNIT_ASSERT( container5->access( 17, 12, X_OK|R_OK ) == true );
    CPPUNIT_ASSERT( container5->access( 12, 17, X_OK|R_OK|W_OK ) == false );
    CPPUNIT_ASSERT( container5->access( 12, 17, X_OK|W_OK ) == false );
    CPPUNIT_ASSERT( container5->access( 12, 17, X_OK|R_OK ) == true );
    CPPUNIT_ASSERT( container5->access( 12, 12, X_OK|R_OK ) == false );

    container1->addContainer( container2.get() );
    container1->addContainer( container3.get() );
    container3->addContainer( container4.get() );
    container3->addContainer( container5.get() );

    containerSvc->updateStore( container1.get() );
    containerSvc->updateStore( container2.get() );
    containerSvc->updateStore( container3.get() );
    containerSvc->updateStore( container4.get() );
    containerSvc->updateStore( container5.get() );

    container3->removeContainer( "subContainerLevel2-2" );
    containerSvc->removeContainer( container5.get() );

    std::shared_ptr<eos::IContainerMD> container6 = containerSvc->createContainer(0);
    container6->setName( "subContLevel2-3" );
    container3->addContainer( container6.get() );
    containerSvc->updateStore( container6.get() );

    eos::IContainerMD::id_t idAttr = container4->getId();
    container4->setAttribute( "test1", "test1" );
    container4->setAttribute( "test1", "test11" );
    container4->setAttribute( "test2", "test2" );
    container4->setAttribute( "test3", "test3" );
    containerSvc->updateStore( container4.get() );

    CPPUNIT_ASSERT( container4->numAttributes() == 3 );
    CPPUNIT_ASSERT( container4->getAttribute( "test1" ) == "test11" );
    CPPUNIT_ASSERT( container4->getAttribute( "test3" ) == "test3" );
    CPPUNIT_ASSERT_THROW( container4->getAttribute( "test15" ), eos::MDException );

    containerSvc->finalize();

    containerSvc->initialize();
    std::shared_ptr<eos::IContainerMD> cont1 = containerSvc->getContainerMD( id );
    CPPUNIT_ASSERT( cont1->getName() == "root" );

    std::shared_ptr<eos::IContainerMD> cont2 = cont1->findContainer( "subContLevel1-1" );
    CPPUNIT_ASSERT( cont2 != 0 );
    CPPUNIT_ASSERT( cont2->getName() == "subContLevel1-1" );

    cont2 = cont1->findContainer( "subContLevel1-2" );
    CPPUNIT_ASSERT( cont2 != 0 );
    CPPUNIT_ASSERT( cont2->getName() == "subContLevel1-2" );

    cont1 = cont2->findContainer( "subContLevel2-1" );
    CPPUNIT_ASSERT( cont1 != 0 );
    CPPUNIT_ASSERT( cont1->getName() == "subContLevel2-1" );

    cont1 = cont2->findContainer( "subContLevel2-2" );
    CPPUNIT_ASSERT( cont1 == 0 );

    cont1 = cont2->findContainer( "subContLevel2-3" );
    CPPUNIT_ASSERT( cont1 != 0 );
    CPPUNIT_ASSERT( cont1->getName() == "subContLevel2-3" );

    std::shared_ptr<eos::IContainerMD> contAttrs = containerSvc->getContainerMD( idAttr );
    CPPUNIT_ASSERT( contAttrs->numAttributes() == 3 );
    CPPUNIT_ASSERT( contAttrs->getAttribute( "test1" ) == "test11" );
    CPPUNIT_ASSERT( contAttrs->getAttribute( "test3" ) == "test3" );
    CPPUNIT_ASSERT_THROW( contAttrs->getAttribute( "test15" ), eos::MDException );

    containerSvc->finalize();
    unlink( fileName.c_str() );
  }
  catch( eos::MDException &e )
  {
    CPPUNIT_ASSERT_MESSAGE( e.getMessage().str(), false );
  }
}
