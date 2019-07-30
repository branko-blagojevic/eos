//------------------------------------------------------------------------------
// @file: QuotaCmd.hh
// @author: Fabio Luchetti - CERN
//------------------------------------------------------------------------------

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


#pragma once
#include "mgm/Namespace.hh"
#include "proto/Quota.pb.h"
#include "mgm/proc/ProcCommand.hh"



EOSMGMNAMESPACE_BEGIN

//------------------------------------------------------------------------------
//! Class QuotaCmd - class handling quota commands
//------------------------------------------------------------------------------
class QuotaCmd: public IProcCommand
{
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //!
  //! @param req client ProtocolBuffer request
  //! @param vid client virtual identity
  //----------------------------------------------------------------------------
  explicit QuotaCmd(eos::console::RequestProto&& req,
                    eos::common::VirtualIdentity& vid):
    IProcCommand(std::move(req), vid, false)
  {}

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  ~QuotaCmd() override = default;

  //----------------------------------------------------------------------------
  //! Method implementing the specific behaviour of the command executed by the
  //! asynchronous thread
  //----------------------------------------------------------------------------
  eos::console::ReplyProto ProcessRequest() noexcept override;

private:
  //----------------------------------------------------------------------------
  //! Execute lsuser subcommand
  //!
  //! @param lsuser lsuser subcommand proto object
  //! @param reply reply proto object
  //----------------------------------------------------------------------------
  void LsuserSubcmd(const eos::console::QuotaProto_LsuserProto& lsuser,
                    eos::console::ReplyProto& reply);

  //----------------------------------------------------------------------------
  //! Execute ls subcommand
  //!
  //! @param ls ls subcommand proto object
  //! @param reply reply proto object
  //----------------------------------------------------------------------------
  void LsSubcmd(const eos::console::QuotaProto_LsProto& ls,
                    eos::console::ReplyProto& reply);

  //----------------------------------------------------------------------------
  //! Execute set subcommand
  //!
  //! @param set set subcommand proto object
  //! @param reply reply proto object
  //----------------------------------------------------------------------------
  void SetSubcmd(const eos::console::QuotaProto_SetProto& set,
                    eos::console::ReplyProto& reply);

  //----------------------------------------------------------------------------
  //! Execute rm subcommand
  //!
  //! @param rm rm subcommand proto object
  //! @param reply reply proto object
  //----------------------------------------------------------------------------
  void RmSubcmd(const eos::console::QuotaProto_RmProto& rm,
                    eos::console::ReplyProto& reply);

  //----------------------------------------------------------------------------
  //! Execute rmnode subcommand
  //!
  //! @param rmnode rmnode subcommand proto object
  //! @param reply reply proto object
  //----------------------------------------------------------------------------
  void RmnodeSubcmd(const eos::console::QuotaProto_RmnodeProto& rmnode,
                    eos::console::ReplyProto& reply);

};

EOSMGMNAMESPACE_END
