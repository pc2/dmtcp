/****************************************************************************
 *   Copyright (C) 2006-2008 by Jason Ansel, Kapil Arya, and Gene Cooperman *
 *   jansel@csail.mit.edu, kapil@ccs.neu.edu, gene@ccs.neu.edu              *
 *                                                                          *
 *  This file is part of DMTCP.                                             *
 *                                                                          *
 *  DMTCP is free software: you can redistribute it and/or                  *
 *  modify it under the terms of the GNU Lesser General Public License as   *
 *  published by the Free Software Foundation, either version 3 of the      *
 *  License, or (at your option) any later version.                         *
 *                                                                          *
 *  DMTCP is distributed in the hope that it will be useful,                *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 *  GNU Lesser General Public License for more details.                     *
 *                                                                          *
 *  You should have received a copy of the GNU Lesser General Public        *
 *  License along with DMTCP:dmtcp/src.  If not, see                        *
 *  <http://www.gnu.org/licenses/>.                                         *
 ****************************************************************************/

#ifndef DMTCPMESSAGETYPES_H
#define DMTCPMESSAGETYPES_H

#include "dmtcpalloc.h"
#include "uniquepid.h"
#include "../jalib/jassert.h"
#include "../jalib/jalloc.h"
#include "constants.h"
#include "workerstate.h"

namespace dmtcp
{

  enum DmtcpMessageType
  {
    DMT_NULL,
    DMT_NEW_WORKER,   // on connect established worker-coordinator
    DMT_NAME_SERVICE_WORKER,
    DMT_RESTART_WORKER,   // on connect established worker-coordinator
    DMT_ACCEPT,        // on connect established coordinator-worker
    DMT_REJECT_NOT_RESTARTING,
    DMT_REJECT_WRONG_COMP,
    DMT_REJECT_NOT_RUNNING,

    DMT_UPDATE_PROCESS_INFO_AFTER_FORK,
    DMT_UPDATE_PROCESS_INFO_AFTER_INIT_OR_EXEC,

    DMT_UPDATE_CKPT_DIR,
    DMT_UPDATE_GLOBAL_CKPT_DIR,
    DMT_UPDATE_GLOBAL_CKPT_DIR_SUCCEED,
    DMT_UPDATE_GLOBAL_CKPT_DIR_FAIL,
    DMT_CKPT_FILENAME,       // a slave sending it's checkpoint filename to coordinator
    DMT_UNIQUE_CKPT_FILENAME,// same as DMT_CKPT_FILENAME, except when
                             //   unique-ckpt plugin is being used.

    DMT_USER_CMD,            // on connect established dmtcp_command -> coordinator
    DMT_USER_CMD_RESULT,     // on reply coordinator -> dmtcp_command

    DMT_DO_SUSPEND,          // when coordinator wants slave to suspend        8
    DMT_DO_RESUME,           // when coordinator wants slave to resume (after checkpoint)
    DMT_DO_FD_LEADER_ELECTION, // when coordinator wants slaves to do leader election
#ifdef COORD_NAMESERVICE
    DMT_DO_PRE_CKPT_NAME_SERVICE_DATA_REGISTER,
    DMT_DO_PRE_CKPT_NAME_SERVICE_DATA_QUERY,
#endif
    DMT_DO_DRAIN,            // when coordinator wants slave to flush
    DMT_DO_CHECKPOINT,       // when coordinator wants slave to checkpoint
#ifdef COORD_NAMESERVICE
    DMT_DO_REGISTER_NAME_SERVICE_DATA,
    DMT_DO_SEND_QUERIES,
#endif
    DMT_DO_REFILL,           // when coordinator wants slave to refill buffers
    DMT_KILL_PEER,           // send kill message to peer

    DMT_REGISTER_NAME_SERVICE_DATA,
    DMT_REGISTER_NAME_SERVICE_DATA_SYNC,
    DMT_REGISTER_NAME_SERVICE_DATA_SYNC_RESPONSE,
    DMT_NAME_SERVICE_QUERY,
    DMT_NAME_SERVICE_QUERY_RESPONSE,
    DMT_NAME_SERVICE_GET_UNIQUE_ID,
    DMT_NAME_SERVICE_GET_UNIQUE_ID_RESPONSE,

    DMT_UPDATE_LOGGING,

    DMT_OK,                  // slave telling coordinator it is done (response
                             //   to DMT_DO_*)  this means slave reached barrier
  };

  namespace CoordCmdStatus {
    enum  ErrorCodes {
      NOERROR                 =  0,
      ERROR_INVALID_COMMAND   = -1,
      ERROR_NOT_RUNNING_STATE = -2,
      ERROR_COORDINATOR_NOT_FOUND = -3
    };
  }

  ostream& operator << (ostream& o, const DmtcpMessageType& s);

#define DMTCPMESSAGE_NUM_PARAMS 2
#define DMTCPMESSAGE_SAME_CKPT_INTERVAL (~0u) /* default value */

  // Make sure the struct is of same size on 32-bit and 64-bit systems.
  struct DmtcpMessage
  {
    char _magicBits[16];

    uint32_t  _msgSize;
    uint32_t extraBytes;

    DmtcpMessageType type;
    WorkerState::eWorkerState state;

    UniquePid   from;
    UniquePid   compGroup;

    pid_t       virtualPid;
    pid_t       realPid;

//#ifdef COORD_NAMESERVICE
    char        nsid[8];
    uint32_t    keyLen;
    uint32_t    valLen;
//#endif

    uint32_t numPeers;
    uint32_t isRunning;
    uint32_t coordCmd;
    int32_t coordCmdStatus;

    uint64_t coordTimeStamp;

    uint32_t theCheckpointInterval;
    struct in_addr ipAddr;

    uint32_t uniqueIdOffset;

    uint32_t logMask;

    static void setDefaultCoordinator ( const DmtcpUniqueProcessId& id );
    static void setDefaultCoordinator ( const UniquePid& id );
    DmtcpMessage ( DmtcpMessageType t = DMT_NULL );
    void assertValid() const;
    bool isValid() const;
    void poison();
  };

}//namespace dmtcp



#endif



