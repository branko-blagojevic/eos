//------------------------------------------------------------------------------
//! @file ICmdHelper.hh
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2017 CERN/Switzerland                                  *
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

#pragma once
#include "proto/ConsoleRequest.pb.h"
#include "console/MgmExecute.hh"

//------------------------------------------------------------------------------
//! Class ICmdHelper
//! @brief Abstract base class to be inherited in all the command
//! implementations
//------------------------------------------------------------------------------
class ICmdHelper
{
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  ICmdHelper():
    mReq(), mMgmExec(), mIsAdmin(false), mHighlight(false), mIsSilent(false)
  {
    if (json) {
      mReq.set_format(eos::console::RequestProto::JSON);
    }
  }

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  virtual ~ICmdHelper() = default;

  //----------------------------------------------------------------------------
  //! Parse command line input
  //!
  //! @param arg input
  //!
  //! @return true if successful, otherwise false
  //----------------------------------------------------------------------------
  virtual bool ParseCommand(const char* arg) = 0;

  //----------------------------------------------------------------------------
  //! Execute command and display any output information
  //! @note When this methods is called the generic request object mReq needs
  //! to already contain the specific commands object.
  //!
  //! @return command return code
  //----------------------------------------------------------------------------
  int Execute(bool printError = true);

  int ExecuteWithoutPrint();

  std::string GetResult();

  std::string GetError();

  bool NeedsConfirmation();

  bool ConfirmOperation();

protected:
  //----------------------------------------------------------------------------
  //! Apply highlighting to text
  //!
  //! @param text text to be highlighted
  //----------------------------------------------------------------------------
  void TextHighlight(std::string& text);

  eos::console::RequestProto mReq; ///< Generic request object send to the MGM
  MgmExecute mMgmExec; ///< Wrapper for executing commands at the MGM
  bool mIsAdmin; ///< If true execute as admin, otherwise as user
  bool mHighlight; ///< If true apply text highlighting to output
  bool mIsSilent; ///< If true execute command but don't display anything
  bool mNeedsConfirmation {false}; ///< If true it requires a strong user confirmation before executing the command
};
