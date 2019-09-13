/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2018 CERN/Switzerland                                  *
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
// author: Georgios Bitzes <georgios.bitzes@cern.ch>
// desc:   Namespace checksum utilities
//------------------------------------------------------------------------------

#ifndef EOS_NS_CHECKSUM_HH
#define EOS_NS_CHECKSUM_HH

#include "common/Logging.hh"
#include "common/LayoutId.hh"
#include "proto/FileMd.pb.h"
#include "namespace/utils/Buffer.hh"
#include "namespace/interface/IFileMD.hh"


namespace eos
{
  //----------------------------------------------------------------------------
  //! Append FileMD checksum onto the given string. Return false only if we're
  //! not able to determine checksum type for given layout id.
  //!
  //! Use the given separator to separate each two hexadecimal digits.
  //! i.e. "b5 e1 70 20" instead of "b5e17020"
  //!
  //! We use a template to support both std::string and XrdOucString...
  //----------------------------------------------------------------------------
  template<typename StringType>
  bool appendChecksumOnStringAsHexNoFmd(IFileMD::layoutId_t layoutId,
                                   const Buffer &buffer,
                                   StringType &out,
                                   char separator = 0x00,
                                   int overrideLength = -1) {
    // All this is to maintain backward compatibility in all places where
    // we print checksums.. I'm not sure if we absolutely need to pad with
    // zeroes, for example.

    unsigned int nominalChecksumLength =
      eos::common::LayoutId::GetChecksumLen(layoutId);
    unsigned int targetChecksumLength;

    if (overrideLength == -1) {
      targetChecksumLength = nominalChecksumLength;
    } else {
      targetChecksumLength = overrideLength;
    }

    for (unsigned int i = 0; i < targetChecksumLength; i++) {
      unsigned char targetCharacter = 0x00;
      char hb[4];

      if (i < nominalChecksumLength) {
        targetCharacter = buffer.getDataPadded(i);
      }

      if (separator != 0x00 && i != (targetChecksumLength-1)) {
        sprintf(hb, "%02x%c", targetCharacter, separator);
        out += hb;
      } else {
        sprintf(hb, "%02x", targetCharacter);
        out += hb;
      }
    }

    return (nominalChecksumLength > 0);
  }

  template<typename StringType>
  bool appendChecksumOnStringAsHex(const eos::IFileMD *fmd, StringType &out,
                                   char separator = 0x00,
                                   int overrideLength = -1) {
    if(!fmd) return false;
    return appendChecksumOnStringAsHexNoFmd(fmd->getLayoutId(), fmd->getChecksum(),
      out, separator, overrideLength);
  }

  inline bool appendChecksumOnStringProtobuf(const eos::ns::FileMdProto &proto,
    std::string &out, char separator = 0x00, int overrideLength = -1) {

    Buffer checksumBuffer(proto.checksum().size());
    checksumBuffer.putData((void*)proto.checksum().data(), proto.checksum().size());
    return appendChecksumOnStringAsHexNoFmd(proto.layout_id(), checksumBuffer, out, separator, overrideLength);
  }

  inline bool hexArrayToByteArray(const char* hexArray, size_t sz, std::string &byteArray) {
    byteArray.clear();

    if(sz == 0) {
      return true;
    }

    if(sz % 2 != 0) {
      return false;
    }

    for(size_t i = 0; i < sz; i += 2) {
      char *endptr = nullptr;

      char tmpArray[3];
      tmpArray[0] = hexArray[i];
      tmpArray[1] = hexArray[i+1];
      tmpArray[2] = 0x00;

      char byte = static_cast<char>(strtol(tmpArray, &endptr, 16));
      byteArray.push_back(byte);

      if(endptr != tmpArray+2) {
        byteArray.clear();
        return false;
      }
    }

    return true;
  }

  inline bool hexArrayToByteArray(const std::string &hexArray, std::string &byteArray) {
    return hexArrayToByteArray(hexArray.c_str(), hexArray.size(), byteArray);
  }

}

#endif
