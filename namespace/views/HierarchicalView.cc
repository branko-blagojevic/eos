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
// desc:   Hierarchical view implementation
//------------------------------------------------------------------------------

#include "namespace/views/HierarchicalView.hh"
#include "namespace/utils/PathProcessor.hh"
#include "namespace/IContainerMDSvc.hh"
#include "namespace/IFileMDSvc.hh"
#include "namespace/Constants.hh"
#include <errno.h>

#include <ctime>
#include <list>

#ifdef __APPLE__
#define EBADFD 77
#endif

namespace eos
{
  //----------------------------------------------------------------------------
  // Configure the view
  //----------------------------------------------------------------------------
  void HierarchicalView::configure( std::map<std::string, std::string> &config )
  {
    if( !pContainerSvc )
    {
      MDException e( EINVAL );
      e.getMessage() << "Container MD Service was not set";
      throw e;
    }

    if( !pFileSvc )
    {
      MDException e( EINVAL );
      e.getMessage() << "File MD Service was not set";
      throw e;
    }
  }

  //----------------------------------------------------------------------------
  // Initialize the view
  //----------------------------------------------------------------------------

  void HierarchicalView::initialize() throw( MDException )
  {
    initialize1();
    initialize2();
    initialize3();
  }

  void HierarchicalView::initialize1() throw( MDException )
  {
    pContainerSvc->initialize();

    //--------------------------------------------------------------------------
    // Get root container
    //--------------------------------------------------------------------------
    try
    {
      pRoot = pContainerSvc->getContainerMD( 1 );
    }
    catch( MDException &e )
    {
      pRoot = pContainerSvc->createContainer();
      pRoot->setParentId( pRoot->getId() );
      if (!static_cast<ChangeLogContainerMDSvc*>(pContainerSvc)->getSlaveMode())
	pContainerSvc->updateStore( pRoot );
    }
  }

  void HierarchicalView::initialize2() throw( MDException )
  {
    pFileSvc->initialize();
  }

  void HierarchicalView::initialize3() throw( MDException )
  {
    //--------------------------------------------------------------------------
    // Scan all the files to reattach them to containers - THIS SHOULD NOT
    // BE DONE! THE INFO NEEDS TO BE STORED WITH CONTAINERS
    //--------------------------------------------------------------------------

    FileVisitor visitor( pContainerSvc, pQuotaStats, this );
    pFileSvc->visit( &visitor );
  }

  //----------------------------------------------------------------------------
  // Finalize the view
  //----------------------------------------------------------------------------
  void HierarchicalView::finalize() throw( MDException )
  {
    pContainerSvc->finalize();
    pFileSvc->finalize();
    delete pQuotaStats;
    pQuotaStats = new QuotaStats();
  }

  //----------------------------------------------------------------------------
  // Retrieve a file for given uri
  //----------------------------------------------------------------------------
  FileMD *HierarchicalView::getFile( const std::string &uri, bool follow, size_t* link_depths )
    throw( MDException )
  {
    char uriBuffer[uri.length()+1];
    strcpy( uriBuffer, uri.c_str() );
    std::vector<char*> elements;
    size_t lLinkDepths = 0;

    if ( (uri == "/") || (!uri.length()) )
    {
      MDException e( ENOENT );
      e.getMessage() << " is not a file";
      throw e;
    }

    eos::PathProcessor::splitPath( elements, uriBuffer );
    size_t position;

    ContainerMD *cont = findLastContainer( elements, elements.size()-1,
					   position,
					   link_depths);

    if( position != elements.size()-1 )
    {
      MDException e( ENOENT );
      e.getMessage() << "Container does not exist";
      throw e;
    }

    FileMD *file = cont->findFile( elements[position] );
    if( !file )
    {
      MDException e( ENOENT );
      e.getMessage() << "File does not exist";
      throw e;
    }
    else
    {
      if (file->isLink() && follow)
      {
	if (!link_depths)
	  link_depths = &lLinkDepths;

	(*link_depths)++;

	if ( (*link_depths) > 255)
	{
	  MDException e( ELOOP );
	  e.getMessage() << "Too many symbolic links were encountered in translating the pathname";
	  throw e;
	}
	std::string link = file->getLink();
	if (link[0] != '/')
	{
	  link.insert(0, getUri(cont));
	  absPath(link);
	}
	return getFile(link, true, link_depths);
      }
    }
    return file;
  }

  //------------------------------------------------------------------------
  //! Get real path translating existing symlink
  //------------------------------------------------------------------------
  std::string HierarchicalView::getRealPath( const std::string &uri )
    throw( MDException )
  {
    size_t link_depths=0;
    char uriBuffer[uri.length()+1];
    strcpy( uriBuffer, uri.c_str() );
    std::vector<char*> elements;

    if (uri == "/")
    {
      MDException e( ENOENT );
      e.getMessage() << " is not a file";
      throw e;
    }

    eos::PathProcessor::splitPath( elements, uriBuffer );
    size_t position;

    ContainerMD *cont = findLastContainer( elements, elements.size()-1,
					   position,
					   &link_depths);

    if( position != elements.size()-1 )
    {
      MDException e( ENOENT );
      e.getMessage() << "Container does not exist";
      throw e;
    }

    // replace the last existing container with the resolved container path
    std::string newcontainer = getUri(cont);
    size_t oldlength=0;

    for (size_t i=0; i< position; i++)
    {
      oldlength += strlen(elements[i])+1;
    }

    std::string newpath = uri;
    newpath.erase(0,oldlength+1);
    newpath.insert(0,newcontainer);
    return newpath;
  }


  //----------------------------------------------------------------------------
  // Create a file for given uri
  //----------------------------------------------------------------------------
  FileMD *HierarchicalView::createFile( const std::string &uri,
					uid_t uid, gid_t gid )
    throw( MDException )
  {

    if (uri == "/")
    {
      MDException e( EISDIR );
      e.getMessage() << "/ is a directory";
      throw e;
    }

    //--------------------------------------------------------------------------
    // Split the path and find the last container
    //--------------------------------------------------------------------------
    char uriBuffer[uri.length()+1];
    strcpy( uriBuffer, uri.c_str() );
    std::vector<char*> elements;
    eos::PathProcessor::splitPath( elements, uriBuffer );
    size_t position;
    ContainerMD *cont = findLastContainer( elements, elements.size()-1,
					   position );

    if( position != elements.size()-1 )
    {
      MDException e( ENOENT );
      e.getMessage() << "Container does not exist";
      throw e;
    }

    //--------------------------------------------------------------------------
    // Check it the file of this name can be inserted
    //--------------------------------------------------------------------------
    if( cont->findContainer( elements[position] ) )
    {
      MDException e( EEXIST );
      e.getMessage() << "File exist";
      throw e;
    }

    if( cont->findFile( elements[position] ) )
    {
      MDException e( EEXIST );
      e.getMessage() << "File exist";
      throw e;
    }

    FileMD *file = pFileSvc->createFile();
    if( !file )
    {
      MDException e( EIO );
      e.getMessage() << "File creation failed";
      throw e;
    }

    file->setName( elements[position] );
    file->setCUid( uid );
    file->setCGid( gid );
    file->setCTimeNow();
    file->setMTimeNow();
    file->clearChecksum(0);
    cont->addFile( file );
    pFileSvc->updateStore( file );

    return file;
  }

  //------------------------------------------------------------------------
  //! Create a link for given uri
  //------------------------------------------------------------------------
  void HierarchicalView::createLink( const std::string &uri,
				     const std::string &linkuri,
				     uid_t uid, gid_t gid )
    throw( MDException )
  {
    FileMD *file = createFile(uri, uid, gid);

    if (file) {
      file->setLink(linkuri);
      pFileSvc->updateStore( file );
    }
  }

  //----------------------------------------------------------------------------
  // Remove link
  //----------------------------------------------------------------------------
  void HierarchicalView::removeLink( const std::string &uri )
    throw( MDException )
  {
    return unlinkFile ( uri );
  }

  //----------------------------------------------------------------------------
  // Unlink the file for given uri
  //----------------------------------------------------------------------------
  void HierarchicalView::unlinkFile( const std::string &uri )
    throw( MDException )
  {
    char uriBuffer[uri.length()+1];
    strcpy( uriBuffer, uri.c_str() );
    std::vector<char*> elements;
    eos::PathProcessor::splitPath( elements, uriBuffer );
    size_t position;
    ContainerMD *cont = findLastContainer( elements, elements.size()-1,
					   position );

    if( position != elements.size()-1 )
    {
      MDException e( ENOENT );
      e.getMessage() << "Container does not exist";
      throw e;
    }

    FileMD *file = cont->findFile( elements[position] );

    if( !file )
    {
      MDException e( ENOENT );
      e.getMessage() << "File does not exist";
      throw e;
    }

    cont->removeFile( file->getName() );
    file->setContainerId( 0 );
    file->unlinkAllLocations();
    pFileSvc->updateStore( file );
  }

  //----------------------------------------------------------------------------
  // Remove the file
  //----------------------------------------------------------------------------
  void HierarchicalView::removeFile( FileMD *file ) throw( MDException )
  {
    //--------------------------------------------------------------------------
    // Check if the file can be removed
    //--------------------------------------------------------------------------
    if( file->getNumLocation() != 0 || file->getNumUnlinkedLocation() != 0 )
    {
      MDException ex( EBADFD );
      ex.getMessage() << "Cannot remove the record. Unlinked replicas still ";
      ex.getMessage() << "still exist";
      throw ex;
    }

    if( file->getContainerId() != 0 )
    {
      ContainerMD *cont = pContainerSvc->getContainerMD( file->getContainerId() );
      cont->removeFile( file->getName() );
    }
    pFileSvc->removeFile( file );
  }

  //----------------------------------------------------------------------------
  // Get a container (directory)
  //----------------------------------------------------------------------------
  ContainerMD *HierarchicalView::getContainer( const std::string &uri,
					       bool follow,
					       size_t* link_depths)
    throw( MDException )
  {
    if( uri == "/" )
      return pRoot;

    if ( !uri.length() )
    {
      MDException e( ENOENT );
      e.getMessage() << " is an empty URI";
      throw e;
    }

    size_t lLinkDepth = 0;

    if (!link_depths)
    {
      // use local variable in case
      link_depths = &lLinkDepth;
      (*link_depths)++;
    }

    char uriBuffer[uri.length()+1];
    strcpy( uriBuffer, uri.c_str() );
    std::vector<char*> elements;
    eos::PathProcessor::splitPath( elements, uriBuffer );

    if (!elements.size())
    {
      // this can happen for an empty uri created when trying to follow a symlink out of the filesystem
      // which has been reduced by the absPath function to uri=""
      MDException e( ENOENT );
      e.getMessage() << uri << ": No such file or directory";
      throw e;
    }

    size_t position=0;
    ContainerMD *cont =0;

    if (follow)
    {
      // follow all symlinks for all containers
      cont = findLastContainer( elements, elements.size(), position, link_depths );
    }
    else
    {
      // follow all symlinks but not the final container
      cont = findLastContainer( elements, elements.size()-1, position, link_depths );
      cont = cont->findContainer(elements[elements.size()-1]);
      if (cont)
	++position;
    }

    if( position != (elements.size()) )
    {
      MDException e( ENOENT );
      e.getMessage() << uri << ": No such file or directory";
      throw e;
    }

    return cont;
  }

  //----------------------------------------------------------------------------
  // Create a container (directory)
  //----------------------------------------------------------------------------
  ContainerMD *HierarchicalView::createContainer( const std::string &uri,
						  bool createParents )
    throw( MDException )
  {
    //--------------------------------------------------------------------------
    // Split the path
    //--------------------------------------------------------------------------
    if( uri == "/" )
    {
      MDException e( EEXIST );
      e.getMessage() << uri << ": File exist" << std::endl;
      throw e;
    }

    char uriBuffer[uri.length()+1];
    strcpy( uriBuffer, uri.c_str() );
    std::vector<char*> elements;
    eos::PathProcessor::splitPath( elements, uriBuffer );

    if( elements.size() == 0 )
    {
      MDException e( EEXIST );
      e.getMessage() << uri << ": File exist" << std::endl;
      throw e;
    }

    //--------------------------------------------------------------------------
    // Look for the last existing container
    //--------------------------------------------------------------------------
    size_t position;
    ContainerMD *lastContainer = findLastContainer( elements, elements.size(),
						    position );

    if( position == elements.size() )
    {
      MDException e( EEXIST );
      e.getMessage() << uri << ": File exist" << std::endl;
      throw e;
    }

    //--------------------------------------------------------------------------
    // One of the parent containers does not exist
    //--------------------------------------------------------------------------
    if( (!createParents) && (position < elements.size()-1) )
    {
      MDException e( ENOENT );
      e.getMessage() << uri << ": Parent does not exist" << std::endl;
      throw e;
    }

    if( lastContainer->findFile( elements[position] ) )
    {
      MDException e( EEXIST );
      e.getMessage() << "File exists" << std::endl;
      throw e;
    }

    //--------------------------------------------------------------------------
    // Create the container with all missing parent's if requires
    //--------------------------------------------------------------------------
    for( size_t i = position; i < elements.size(); ++i )
    {
      ContainerMD *newContainer = pContainerSvc->createContainer();
      newContainer->setName( elements[i] );
      newContainer->setCTimeNow();
      lastContainer->addContainer( newContainer );
      lastContainer = newContainer;
      pContainerSvc->updateStore( lastContainer );
    }

    return lastContainer;
  }

  //----------------------------------------------------------------------------
  // Remove a container (directory)
  //----------------------------------------------------------------------------
  void HierarchicalView::removeContainer( const std::string &uri,
					  bool recursive )
    throw( MDException )
  {
    //--------------------------------------------------------------------------
    // Find the container
    //--------------------------------------------------------------------------
    if( uri == "/" )
    {
      MDException e( EPERM );
      e.getMessage() << "Permission denied.";
      throw e;
    }

    char uriBuffer[uri.length()+1];
    strcpy( uriBuffer, uri.c_str() );
    std::vector<char*> elements;
    eos::PathProcessor::splitPath( elements, uriBuffer );

    size_t position;
    ContainerMD *parent = findLastContainer( elements, elements.size()-1, position );
    if( (position != (elements.size()-1)) )
    {
      MDException e( ENOENT );
      e.getMessage() << uri << ": No such file or directory";
      throw e;
    }

    //--------------------------------------------------------------------------
    // Check if the container exist and remove it
    //--------------------------------------------------------------------------
    ContainerMD *cont = parent->findContainer( elements[elements.size()-1] );
    if( !cont )
    {
      MDException e( ENOENT );
      e.getMessage() << uri << ": No such file or directory";
      throw e;
    }

    if( (cont->getNumContainers() != 0 || cont->getNumFiles() != 0) &&
	!recursive )
    {
      MDException e( ENOTEMPTY );
      e.getMessage() << uri << ": Container is not empty";
      throw e;
    }

    parent->removeContainer( cont->getName() );

    if( recursive )
      cleanUpContainer( cont );

    pContainerSvc->removeContainer( cont );

  }

  //----------------------------------------------------------------------------
  // Find the last existing container in the path
  //----------------------------------------------------------------------------
  ContainerMD *HierarchicalView::findLastContainer( std::vector<char*> &elements,
						    size_t end, size_t &index,
						    size_t* link_depths)
  {
    ContainerMD *current  = pRoot;
    ContainerMD *found    = 0;
    size_t       position = 0;

    while( position < end )
    {
      found = current->findContainer( elements[position] );
      if( !found )
      {
	// check if link
	FileMD* flink = current->findFile ( elements[position] );
	if ( flink ) {
	  if ( flink->isLink() )
	  {
	    if (link_depths)
	    {
	      (*link_depths)++;

	      if ( (*link_depths) > 255)
	      {
		MDException e( ELOOP );
		e.getMessage() << "Too many symbolic links were encountered in translating the pathname";
		throw e;
	      }
	    }

	    std::string link = flink->getLink();
	    if (link[0] != '/')
	    {
	      link.insert(0,getUri(current));
	      absPath(link);
	    }
	    found = getContainer( link , false, link_depths);
	    if ( !found )
	    {
	      index = position;
	      return current;
	    }
	  }
	}

	if (!found)
	{
	  index = position;
	  return current;
	}
      }
      current = found;
      ++position;
    }

    index = position;
    return current;
  }

  //----------------------------------------------------------------------------
  // Clean up the container's children
  //----------------------------------------------------------------------------
  void HierarchicalView::cleanUpContainer( ContainerMD *cont )
  {
    ContainerMD::FileMap::iterator itF;
    for( itF = cont->filesBegin(); itF != cont->filesEnd(); ++itF )
      pFileSvc->removeFile( itF->second );

    ContainerMD::ContainerMap::iterator itC;
    for( itC = cont->containersBegin(); itC != cont->containersEnd(); ++itC )
    {
      cleanUpContainer( itC->second );
      pContainerSvc->removeContainer( itC->second );
    }
  }

  //----------------------------------------------------------------------------
  // Update quota
  //----------------------------------------------------------------------------
  void HierarchicalView::FileVisitor::visitFile( FileMD *file )
  {
    if( file->getContainerId() == 0 )
      return;

    ContainerMD *cont = 0;
    try { cont = pContSvc->getContainerMD( file->getContainerId() ); }
    catch( MDException &e ) {}

    if( !cont )
      return;

    //--------------------------------------------------------------------------
    // Update quota stats
    //--------------------------------------------------------------------------
    QuotaNode *node = pView->getQuotaNode( cont );
    if( node )
      node->addFile( file );
  }

  //----------------------------------------------------------------------------
  // Get uri for the container
  //----------------------------------------------------------------------------
  std::string HierarchicalView::getUri( const ContainerMD *container ) const
    throw( MDException )
  {
    //--------------------------------------------------------------------------
    // Check the input
    //--------------------------------------------------------------------------
    if( !container )
    {
      MDException ex;
      ex.getMessage() << "Invalid container (zero pointer)";
      throw ex;
    }

    //--------------------------------------------------------------------------
    // Gather the uri elements
    //--------------------------------------------------------------------------
    std::vector<std::string> elements;
    elements.reserve( 10 );
    const ContainerMD *cursor = container;
    while( cursor->getId() != 1 )
    {
      elements.push_back( cursor->getName() );
      cursor = pContainerSvc->getContainerMD( cursor->getParentId() );
    }

    //--------------------------------------------------------------------------
    // Assemble the uri
    //--------------------------------------------------------------------------
    std::string path = "/";
    std::vector<std::string>::reverse_iterator rit;
    for( rit = elements.rbegin(); rit != elements.rend(); ++rit )
    {
      path += *rit;
      path += "/";
    }
    return path;
  }

  //----------------------------------------------------------------------------
  // Get uri for the file
  //----------------------------------------------------------------------------
  std::string HierarchicalView::getUri( const FileMD *file ) const
    throw( MDException )
  {
    //--------------------------------------------------------------------------
    // Check the input
    //--------------------------------------------------------------------------
    if( !file )
    {
      MDException ex;
      ex.getMessage() << "Invalid file (zero pointer)";
      throw ex;
    }

    //--------------------------------------------------------------------------
    // Get the uri
    //--------------------------------------------------------------------------
    std::string path = getUri( pContainerSvc->getContainerMD(
						    file->getContainerId() ) );
    return path+file->getName();
  }

  //----------------------------------------------------------------------------
  // Get quota node id concerning given container
  //----------------------------------------------------------------------------
  QuotaNode *HierarchicalView::getQuotaNode( const ContainerMD *container,
					     bool               search )
    throw( MDException )
  {
    //--------------------------------------------------------------------------
    // Initial sanity check
    //--------------------------------------------------------------------------
    if( !container )
    {
      MDException ex;
      ex.getMessage() << "Invalid container (zero pointer)";
      throw ex;
    }

    if( !pQuotaStats )
    {
      MDException ex;
      ex.getMessage() << "No QuotaStats placeholder registered";
      throw ex;
    }

    //--------------------------------------------------------------------------
    // Search for the node
    //--------------------------------------------------------------------------
    const ContainerMD *current = container;

    if (search)
    {
      while( current != pRoot && (current->getFlags() & QUOTA_NODE_FLAG) == 0 )
	current = pContainerSvc->getContainerMD( current->getParentId() );
    }

    //--------------------------------------------------------------------------
    // We have either found a quota node or reached root without finding one
    // so we need to double check whether the current container has an
    // associated quota node
    //--------------------------------------------------------------------------
    if( (current->getFlags() & QUOTA_NODE_FLAG) == 0 )
      return 0;

    QuotaNode *node = pQuotaStats->getQuotaNode( current->getId() );
    if( node )
      return node;

    return pQuotaStats->registerNewNode( current->getId() );
  }

  //----------------------------------------------------------------------------
  // Register the container to be a quota node
  //----------------------------------------------------------------------------
  QuotaNode *HierarchicalView::registerQuotaNode( ContainerMD *container )
    throw( MDException )
  {
    //--------------------------------------------------------------------------
    // Initial sanity check
    //--------------------------------------------------------------------------
    if( !container )
    {
      MDException ex;
      ex.getMessage() << "Invalid container (zero pointer)";
      throw ex;
    }

    if( !pQuotaStats )
    {
      MDException ex;
      ex.getMessage() << "No QuotaStats placeholder registered";
      throw ex;
    }

    if( container->getFlags() & QUOTA_NODE_FLAG )
    {
      MDException ex;
      ex.getMessage() << "Already a quota node: " << container->getId();
      throw ex;
    }

    QuotaNode *node = pQuotaStats->registerNewNode( container->getId() );
    container->getFlags() |= QUOTA_NODE_FLAG;
    updateContainerStore( container );

    return node;
  }

  //----------------------------------------------------------------------------
  // Remove the quota node
  //----------------------------------------------------------------------------
  void HierarchicalView::removeQuotaNode( ContainerMD *container )
    throw( MDException )
  {
    //--------------------------------------------------------------------------
    // Sanity checks
    //--------------------------------------------------------------------------
    if( !container )
    {
      MDException ex;
      ex.getMessage() << "Invalid container (zero pointer)";
      throw ex;
    }

    if( !pQuotaStats )
    {
      MDException ex;
      ex.getMessage() << "No QuotaStats placeholder registered";
      throw ex;
    }

    if( !(container->getFlags() & QUOTA_NODE_FLAG) )
    {
      MDException ex;
      ex.getMessage() << "Not a quota node: " << container->getId();
      throw ex;
    }

    //--------------------------------------------------------------------------
    // Get the quota node and meld it with the parent node if present
    //--------------------------------------------------------------------------
    QuotaNode *node   = getQuotaNode( container );
    QuotaNode *parent = 0;
    if( container != pRoot )
      parent = getQuotaNode( pContainerSvc->getContainerMD( container->getParentId() ),
			     true );

    container->getFlags() &= ~QUOTA_NODE_FLAG;
    updateContainerStore( container );
    if( parent )
      parent->meld( node );

    pQuotaStats->removeNode( container->getId() );
  }

  //----------------------------------------------------------------------------
  // Rename container
  //----------------------------------------------------------------------------
  void HierarchicalView::renameContainer( ContainerMD *container,
					  const std::string &newName )
    throw( MDException )
  {
    if( !container )
    {
      MDException ex;
      ex.getMessage() << "Invalid container (zero pointer)";
      throw ex;
    }

    if( newName.empty() )
    {
      MDException ex;
      ex.getMessage() << "Invalid new name (empty)";
      throw ex;
    }

    if( newName.find( '/' ) != std::string::npos )
    {
      MDException ex;
      ex.getMessage() << "Name cannot contain slashes: " << newName;
      throw ex;
    }

    if( container->getId() == container->getParentId() )
    {
      MDException ex;
      ex.getMessage() << "Cannot rename /";
      throw ex;
    }

    ContainerMD *parent = pContainerSvc->getContainerMD( container->getParentId() );
    if( parent->findContainer( newName ) )
    {
      MDException ex;
      ex.getMessage() << "Container exists: " << newName;
      throw ex;
    }

    if( parent->findFile( newName ) )
    {
      MDException ex;
      ex.getMessage() << "File exists: " << newName;
      throw ex;
    }

    parent->removeContainer( container->getName() );
    container->setName( newName );
    parent->addContainer( container );
    updateContainerStore( container );
  }

  //----------------------------------------------------------------------------
  // Rename file
  //----------------------------------------------------------------------------
  void HierarchicalView::renameFile( FileMD *file, const std::string &newName )
    throw( MDException )
  {
    if( !file )
    {
      MDException ex;
      ex.getMessage() << "Invalid file (zero pointer)";
      throw ex;
    }

    if( newName.empty() )
    {
      MDException ex;
      ex.getMessage() << "Invalid new name (empty)";
      throw ex;
    }

    if( newName.find( '/' ) != std::string::npos )
    {
      MDException ex;
      ex.getMessage() << "Name cannot contain slashes: " << newName;
      throw ex;
    }

    ContainerMD *parent = pContainerSvc->getContainerMD( file->getContainerId() );
    if( parent->findContainer( newName ) )
    {
      MDException ex;
      ex.getMessage() << "Container exists: " << newName;
      throw ex;
    }

    if( parent->findFile( newName ) )
    {
      MDException ex;
      ex.getMessage() << "File exists: " << newName;
      throw ex;
    }

    parent->removeFile( file->getName() );
    file->setName( newName );
    parent->addFile( file );
    updateFileStore( file );
  }

  //----------------------------------------------------------------------------
  // abspath sanitizing all '..' and '.' in a path
  //----------------------------------------------------------------------------
  void HierarchicalView::absPath(std::string& mypath)
  {
    std::vector<std::string> elements, abs_path;
    eos::PathProcessor::splitPath(elements, mypath);
    std::ostringstream oss;
    int skip = 0;

    for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
      if ((*it == ".") || it->empty()) {
	continue;
      }

      if (*it == "..") {
	++skip;
	continue;
      }

      if (skip) {
	--skip;
	continue;
      }

      abs_path.push_back(*it);
    }

    for (auto it = abs_path.rbegin(); it != abs_path.rend(); ++it) {
      oss << "/" << *it;
    }

    mypath = oss.str();

    if (mypath.empty()) {
      mypath = "/";
    }
  }
};
