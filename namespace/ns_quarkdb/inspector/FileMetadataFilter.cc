/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2020 CERN/Switzerland                                  *
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

#include "namespace/ns_quarkdb/inspector/FileMetadataFilter.hh"
#include "namespace/ns_quarkdb/inspector/AttributeExtraction.hh"
#include "common/Assert.hh"

EOSNSNAMESPACE_BEGIN

//----------------------------------------------------------------------------
//! Constructor
//----------------------------------------------------------------------------
EqualityFileMetadataFilter::EqualityFileMetadataFilter(const std::string &attr, const std::string &value)
: mAttr(attr), mValue(value) {}

//----------------------------------------------------------------------------
// Does the given FileMdProto pass through the filter?
//----------------------------------------------------------------------------
bool EqualityFileMetadataFilter::check(const eos::ns::FileMdProto &proto) {
  std::string value;

  if(!AttributeExtraction::asString(proto, mAttr, value)) {
    return false;
  }

  return value == mValue;
}

//------------------------------------------------------------------------------
// Is the object valid?
//------------------------------------------------------------------------------
common::Status EqualityFileMetadataFilter::isValid() const {
  std::string tmp;
  eos::ns::FileMdProto proto;

  if(AttributeExtraction::asString(proto, mAttr, tmp)) {
    return common::Status();
  }

  return common::Status(EINVAL, SSTR("Unknown FileMD attribute: " << mAttr));
}

//------------------------------------------------------------------------------
// Describe object
//------------------------------------------------------------------------------
std::string EqualityFileMetadataFilter::describe() const {
  common::Status st = isValid();

  if(!st) {
    return SSTR("[" << st.toString() << "]");
  }

  return SSTR(mAttr << " == '" << mValue << "'");
}

//------------------------------------------------------------------------------
// Constructor -- valid parse result
//------------------------------------------------------------------------------
ParsedFileMetadataFilter::ParsedFileMetadataFilter(std::unique_ptr<FileMetadataFilter> sub)
: mFilter(std::move(sub)) {}

//------------------------------------------------------------------------------
// Constructor -- parse error
//------------------------------------------------------------------------------
ParsedFileMetadataFilter::ParsedFileMetadataFilter(const common::Status &err)
: mStatus(err) {}

//------------------------------------------------------------------------------
// Is the object valid?
//------------------------------------------------------------------------------
common::Status ParsedFileMetadataFilter::isValid() const {
  if(!mFilter) return mStatus;
  return mFilter->isValid();
}

//------------------------------------------------------------------------------
// Does the given FileMdProto pass through the filter?
//------------------------------------------------------------------------------
bool ParsedFileMetadataFilter::check(const eos::ns::FileMdProto &proto) {
  if(!mFilter) return false;
  return mFilter->check(proto);
}

//------------------------------------------------------------------------------
// Describe object
//------------------------------------------------------------------------------
std::string ParsedFileMetadataFilter::describe() const {
  if(!mFilter) return SSTR("[failed to parse expression: " << mStatus.toString());
  return mFilter->describe();
}

EOSNSNAMESPACE_END

