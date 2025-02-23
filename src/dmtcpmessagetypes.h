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

#include "../jalib/jalloc.h"
#include "../jalib/jassert.h"
#include "constants.h"
#include "dmtcpalloc.h"
#include "uniquepid.h"
#include "workerstate.h"
#include "kvdb.h"

namespace dmtcp
{
enum DmtcpMessageType {
  DMT_NULL,
  DMT_NEW_WORKER,     // on connect established worker-coordinator
  DMT_NAME_SERVICE_WORKER,
  DMT_RESTART_WORKER,     // on connect established worker-coordinator
  DMT_ACCEPT,          // on connect established coordinator-worker
  DMT_REJECT_NOT_RESTARTING,
  DMT_REJECT_WRONG_COMP,
  DMT_REJECT_NOT_RUNNING,

  DMT_UPDATE_PROCESS_INFO_AFTER_FORK,
  DMT_UPDATE_PROCESS_INFO_AFTER_INIT_OR_EXEC,

  DMT_GET_CKPT_DIR,
  DMT_GET_CKPT_DIR_RESULT,
  DMT_UPDATE_CKPT_DIR,
  DMT_CKPT_FILENAME,         // a slave sending it's checkpoint filename to
                             // coordinator
  DMT_UNIQUE_CKPT_FILENAME,  // same as DMT_CKPT_FILENAME, except when
                             // unique-ckpt plugin is being used.

  DMT_USER_CMD,              // on connect established dmtcp_command ->
                             // coordinator
  DMT_USER_CMD_RESULT,       // on reply coordinator -> dmtcp_command

  // OUTLINE OF CONTROL FLOW FOR checkpoint/resume/restart
  // A. Coordinator sends DMTP_DO_CHECKPOINT msg to each worker
  //    On worker side, plugin mgr
  //    1. sends DMTCP_EVENT_PRESUSPEND to ckpt thread for each plugin
  //       (ckpt thread and user threads both active)
  //    2. suspends all user threads
  //    3. sends DMTCP_EVENT_PRECHECKPOINT to each plugin (ckpt thread active)
  //    4. releases control, and the ckpt thread of each worker writes ckpt
  //       image
  // B. Coordinator sends DMT_RESUMING (for resume or restart)
  //    On worker side, plugin manager
  //    1. sends DMTCP_EVENT_RESUME or DMTCP_EVENT_RESTART
  // C. Upon receiving some event, the worker calls the registered callback.
  //    The callback function may call dmtcp_global_barrier, which sends back
  //    a DMT_BARRIER msg to coordinator.  The coordinator then implements the
  //    barrier by responding with DMT_BARRIER_RESPONSE.

  DMT_DO_CHECKPOINT,         // when coordinator wants worker to checkpoint

  DMT_BARRIER,               // workers request global barrier from coordinator
  DMT_BARRIER_RELEASED,      // coord. responds: release workers from barriers

  DMT_WORKER_RESUMING,

  DMT_KILL_PEER,             // send kill message to peer

  DMT_KVDB_REQUEST,
  DMT_KVDB_RESPONSE
};

namespace CoordCmdStatus
{
enum  ErrorCodes {
  NOERROR                 =  0,
  ERROR_INVALID_COMMAND   = -1,
  ERROR_NOT_RUNNING_STATE = -2,
  ERROR_COORDINATOR_NOT_FOUND = -3
};
}

ostream&operator<<(ostream &o, const DmtcpMessageType &s);

#define DMTCPMESSAGE_NUM_PARAMS         2
#define DMTCPMESSAGE_SAME_CKPT_INTERVAL (~0u) /* default value */

// Make sure the struct is of same size on 32-bit and 64-bit systems.
struct DmtcpMessage {
  char _magicBits[16];

  union {
    char barrier[64];
    char nsid[64];
    char kvdbId[64];
  };

  union {
    kvdb::KVDBRequest kvdbRequest;
    kvdb::KVDBResponse kvdbResponse;
    uint64_t _pad;
  };

  uint32_t _msgSize;
  uint32_t extraBytes;

  DmtcpMessageType type;
  WorkerState::eWorkerState state;

  UniquePid from;
  UniquePid compGroup;

  pid_t virtualPid;
  pid_t realPid;

  uint32_t keyLen;
  uint32_t valLen;

  uint32_t numPeers;
  uint32_t isRunning;
  uint32_t coordCmd;
  int32_t coordCmdStatus;

  uint64_t coordTimeStamp;

  int32_t theCheckpointInterval;
  struct in_addr ipAddr;

  uint32_t uniqueIdOffset;
  uint32_t exitAfterCkpt;

  DmtcpMessage(DmtcpMessageType t = DMT_NULL);
  void assertValid() const;
  bool isValid() const;
  void poison();
};
} // namespace dmtcp
#endif // ifndef DMTCPMESSAGETYPES_H
