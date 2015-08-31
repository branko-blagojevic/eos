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
// desc:   Class representing the container metadata
//------------------------------------------------------------------------------

#include "namespace/ContainerMD.hh"
#include "namespace/FileMD.hh"
#include <sys/stat.h>

#include "namespace/persistency/hashtable/PersistentHashtable.hh"

namespace eos
{
  // TODO: Naive hashing function, could be more sophisticated.
  hash_value_t hash_string (const void* contents)
  {
    const char* str = ((std::string*) contents)->c_str();

    char ret = 0;
    for(size_t i = 0; str[i]; ++i) {
      ret ^= str[i];
    }

    return ret;
  }

  int comp_string (const void* a, const void* b)
  {
    return strcmp(
     ((std::string*)a)->c_str(),
     ((std::string*)b)->c_str()
    );
  }

  //----------------------------------------------------------------------------
  // Constructor
  //----------------------------------------------------------------------------
  ContainerMD::ContainerMD( id_t id ):
    pId( id ),
    pParentId( 0 ),
    pFlags( 0 ),
    pName( "" ),
    pCUid( 0 ),
    pCGid( 0 ),
    pMode( 040755 ),
    pACLId( 0 ),
    pSubContainers( 100, hash_string, comp_string ),
    pFiles( 100, hash_string, comp_string )
  {
    pCTime.tv_sec = 0;
    pCTime.tv_nsec = 0;
  }

  //----------------------------------------------------------------------------
  // Copy constructor
  //----------------------------------------------------------------------------
  ContainerMD::ContainerMD( const ContainerMD &other ):
    pSubContainers( 100, hash_string, comp_string ),
    pFiles( 100, hash_string, comp_string )
  {
    *this = other;
  }

  //----------------------------------------------------------------------------
  // Asignment operator
  //----------------------------------------------------------------------------
  ContainerMD &ContainerMD::operator = ( const ContainerMD &other )
  {
    pId       = other.pId;
    pParentId = other.pParentId;
    pFlags    = other.pFlags;
    pCTime    = other.pCTime;
    pName     = other.pName;
    pCUid     = other.pCUid;
    pCGid     = other.pCGid;
    pMode     = other.pMode;
    pACLId    = other.pACLId;
    pXAttrs   = other.pXAttrs;
    pFlags    = other.pFlags;

    // pFiles & pSubContainers are not copied here!!!

    return *this;
  }


  //----------------------------------------------------------------------------
  // Serialize the object to a buffer
  //----------------------------------------------------------------------------
  void ContainerMD::serialize( Buffer &buffer ) throw( MDException )
  {
    buffer.putData( &pId,       sizeof( pId ) );
    buffer.putData( &pParentId, sizeof( pParentId ) );
    buffer.putData( &pFlags,    sizeof( pFlags ) );
    buffer.putData( &pCTime,    sizeof( pCTime ) );
    buffer.putData( &pCUid,     sizeof( pCUid ) );
    buffer.putData( &pCGid,     sizeof( pCGid ) );
    buffer.putData( &pMode,     sizeof( pMode ) );
    buffer.putData( &pACLId,    sizeof( pACLId ) );

    uint16_t len = pName.length()+1;
    buffer.putData( &len,          2 );
    buffer.putData( pName.c_str(), len );

    len = pXAttrs.size();
    buffer.putData( &len, sizeof( len ) );
    XAttrMap::iterator it;
    for( it = pXAttrs.begin(); it != pXAttrs.end(); ++it )
    {
      uint16_t strLen = it->first.length()+1;
      buffer.putData( &strLen, sizeof( strLen ) );
      buffer.putData( it->first.c_str(), strLen );
      strLen = it->second.length()+1;
      buffer.putData( &strLen, sizeof( strLen ) );
      buffer.putData( it->second.c_str(), strLen );
    }
  }

  //----------------------------------------------------------------------------
  // Deserialize the class to a buffer
  //----------------------------------------------------------------------------
  void ContainerMD::deserialize( Buffer &buffer ) throw( MDException )
  {
    uint16_t offset = 0;
    offset = buffer.grabData( offset, &pId,       sizeof( pId ) );
    offset = buffer.grabData( offset, &pParentId, sizeof( pParentId ) );
    offset = buffer.grabData( offset, &pFlags,    sizeof( pFlags ) );
    offset = buffer.grabData( offset, &pCTime,    sizeof( pCTime ) );
    offset = buffer.grabData( offset, &pCUid,     sizeof( pCUid ) );
    offset = buffer.grabData( offset, &pCGid,     sizeof( pCGid ) );
    offset = buffer.grabData( offset, &pMode,     sizeof( pMode ) );
    offset = buffer.grabData( offset, &pACLId,    sizeof( pACLId ) );

    uint16_t len;
    offset = buffer.grabData( offset, &len, 2 );
    char strBuffer[len];
    offset = buffer.grabData( offset, strBuffer, len );
    pName = strBuffer;

    uint16_t len1 = 0;
    uint16_t len2 = 0;
    len = 0;
    offset = buffer.grabData( offset, &len, sizeof( len ) );
    for( uint16_t i = 0; i < len; ++i )
    {
      offset = buffer.grabData( offset, &len1, sizeof( len1 ) );
      char strBuffer1[len1];
      offset = buffer.grabData( offset, strBuffer1, len1 );
      offset = buffer.grabData( offset, &len2, sizeof( len2 ) );
      char strBuffer2[len2];
      offset = buffer.grabData( offset, strBuffer2, len2 );
      pXAttrs.insert( std::make_pair <char*, char*>( strBuffer1, strBuffer2 ) );
    }
  };

  //----------------------------------------------------------------------------
  // Add file
  //----------------------------------------------------------------------------
  void ContainerMD::addFile( FileMD *file )
  {
    file->setContainerId( pId );
    pFiles[file->getName()] = file;
  }

  //----------------------------------------------------------------------------
  // Access checking helpers
  //----------------------------------------------------------------------------
#define CANREAD  0x01
#define CANWRITE 0x02
#define CANENTER 0x04

  static char convertModetUser( mode_t mode )
  {
    char perms = 0;
    if( mode & S_IRUSR ) perms |= CANREAD;
    if( mode & S_IWUSR ) perms |= CANWRITE;
    if( mode & S_IXUSR ) perms |= CANENTER;
    return perms;
  }

  static char convertModetGroup( mode_t mode )
  {
    char perms = 0;
    if( mode & S_IRGRP ) perms |= CANREAD;
    if( mode & S_IWGRP ) perms |= CANWRITE;
    if( mode & S_IXGRP ) perms |= CANENTER;
    return perms;
  }

  static char convertModetOther( mode_t mode )
  {
    char perms = 0;
    if( mode & S_IROTH ) perms |= CANREAD;
    if( mode & S_IWOTH ) perms |= CANWRITE;
    if( mode & S_IXOTH ) perms |= CANENTER;
    return perms;
  }

  static bool checkPerms( char actual, char requested )
  {
    for( int i = 0; i < 3; ++i )
      if( requested & (1<<i) )
        if( !(actual & (1<<i)) )
          return false;
    return true;
  }

  //----------------------------------------------------------------------------
  // Check the access permissions
  //----------------------------------------------------------------------------
  bool ContainerMD::access( uid_t uid, gid_t gid, int flags )
  {
    // root can do everything
    if( uid == 0 )
      return true;

    // daemon can read everything
    if( (uid == 2) &&
        (!(flags & W_OK)) )
      return true;

    //--------------------------------------------------------------------------
    // Convert the flags
    //--------------------------------------------------------------------------
    char convFlags = 0;
    if( flags & R_OK ) convFlags |= CANREAD;
    if( flags & W_OK ) convFlags |= CANWRITE;
    if( flags & X_OK ) convFlags |= CANENTER;

    //--------------------------------------------------------------------------
    // Check the perms
    //--------------------------------------------------------------------------
    if( uid == pCUid )
    {
      char user = convertModetUser( pMode );
      return checkPerms( user, convFlags );
    }

    if( gid == pCGid )
    {
      char group = convertModetGroup( pMode );
      return checkPerms( group, convFlags );
    }

    char other = convertModetOther( pMode );
    return checkPerms( other, convFlags );
  }
}
