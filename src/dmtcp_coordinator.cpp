/****************************************************************************
 *   Copyright (C) 2006-2013 by Jason Ansel, Kapil Arya, and Gene Cooperman *
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

/****************************************************************************
 * Coordinator code logic:                                                  *
 * main calls eventLoop, a top-level event loop.                            *
 * eventLoop calls:  onConnect, onData, onDisconnect, startCheckpoint       *
 *   when client or dmtcp_command talks to coordinator.                     *
 * onConnect called on msg at listener port.  It passes control to:         *
 *   handleUserCommand, which takes single char arg ('s', 'c', 'k', 'q', ...)*
 * handleUserCommand calls broadcastMessage to send data back               *
 * any message sent by broadcastMessage takes effect only on returning      *
 *   back up to top level monitorSockets                                    *
 * Hence, even for checkpoint, handleUserCommand just changes state,        *
 *   broadcasts an initial checkpoint command, and then returns to top      *
 *   level.  Replies from clients then drive further state changes.         *
 * The prefix command 'b' (blocking) from dmtcp_command modifies behavior   *
 *   of 'c' so that the reply to dmtcp_command happens only when clients    *
 *   are back in RUNNING state.                                             *
 * onData called when a message arrives at a client's port.  It either      *
 *   processes a per-client special request, or continues the protocol      *
 *   for a checkpoint or restart sequence (see below).                      *
 *                                                                          *
 * updateMinimumState() is responsible for keeping track of states.         *
 * The coordinator keeps a ComputationStatus, with minimumState and         *
 *   maximumState for states of all workers, accessed through getStatus()   *
 *   or through minimumState()                                              *
 * The states for a worker (client) are:                                    *
 * Checkpoint: RUNNING -> SUSPENDED -> CHECKPOINTING                        *
 *                     -> (Checkpoint barriers) -> CHECKPOINTED             *
 *                     -> (Resume barriers) -> RUNNING                      *
 *             [State returns to UNKNOWN if no active workers.]
 *                                                                          *
 * Restart:    RESTARTING -> (Restart barriers) -> RUNNING                  *
 * If debugging, set gdb breakpoint on:                                     *
 *   DmtcpCoordinator::onConnect                                            *
 *   DmtcpCoordinator::onData                                               *
 *   DmtcpCoordinator::handleUserCommand                                    *
 *   DmtcpCoordinator::broadcastMessage                                     *
 ****************************************************************************/

#include <arpa/inet.h>
#include <fcntl.h>
#include <limits.h>  // for HOST_NAME_MAX
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <iomanip>
#include "dmtcp_coordinator.h"
#include "../jalib/jassert.h"
#include "../jalib/jconvert.h"
#include "../jalib/jfilesystem.h"
#include "../jalib/jtimer.h"
#include "constants.h"
#include "dmtcpmessagetypes.h"
#include "lookup_service.h"
#include "protectedfds.h"
#include "restartscript.h"
#include "tokenize.h"
#include "syscallwrappers.h"
#include "util.h"
#include "coordinatorplugin.h"
#undef min
#undef max

#define BINARY_NAME "dmtcp_coordinator"

using namespace dmtcp;

static const char *theHelpMessage =
  "COMMANDS:\n"
  "  l: List connected nodes\n"
  "  s: Print status message\n"
  "  c: Checkpoint all nodes\n"
  "  ck: kc: \n"
  "     Checkpoint and then kill all nodes\n"
  "  i: Print current checkpoint interval\n"
  "     (To change checkpoint interval, use dmtcp_command)\n"
  "  k: Kill all nodes\n"
  "  q: Kill all nodes and quit\n"
  "  ?: Show this message\n"
  "\n";

static const char *theUsage =
  "Usage: dmtcp_coordinator [OPTIONS] [port]\n"
  "Coordinates checkpoints between multiple processes.\n\n"
  "Options:\n"
  "  -p, --coord-port PORT_NUM (environment variable DMTCP_COORD_PORT)\n"
  "      Port to listen on (default: " STRINGIFY(DEFAULT_PORT) ")\n"
  "  --port-file filename\n"
  "      File to write listener port number.\n"
  "      (Useful with '--port 0', which is used to assign a random port)\n"
  "  --status-file filename\n"
  "      File to write host, port, pid, etc., info.\n"
  "  --ckptdir (environment variable DMTCP_CHECKPOINT_DIR):\n"
  "      Directory to store dmtcp_restart_script.sh (default: ./)\n"
  "  --tmpdir (environment variable DMTCP_TMPDIR):\n"
  "      Directory to store temporary files (default: env var TMDPIR or /tmp)\n"
  "  --write-kv-data:\n"
  "      Writes key-value store data to a json file in the working directory\n"
  "  --exit-on-last\n"
  "      Exit automatically when last client disconnects\n"
  "  --kill-after-ckpt\n"
  "      Kill peer processes of computation after first checkpoint is created\n"
  "  --timeout seconds\n"
  "      Coordinator exits after <seconds> even if jobs are active\n"
  "      (Useful during testing to prevent runaway coordinator processes)\n"
  "  --stale-timeout seconds\n"
  "      Coordinator exits after <seconds> if no active job (default: 8 hrs)\n"
  "      (Default prevents runaway coord's; Override w/ larger timeout or -1)\n"
  "  --daemon\n"
  "      Run silently in the background after detaching from the parent "
  "process.\n"
  "  -i, --interval (environment variable DMTCP_CHECKPOINT_INTERVAL):\n"
  "      Time in seconds between automatic checkpoints\n"
  "      (default: 0, disabled)\n"
  "  --coord-logfile PATH (environment variable DMTCP_COORD_LOG_FILENAME\n"
  "              Coordinator will dump its logs to the given file\n"
  "  -q, --quiet \n"
  "      Skip startup msg; Skip NOTE msgs; if given twice, also skip WARNINGs\n"
  "  --help:\n"
  "      Print this message and exit.\n"
  "  --version:\n"
  "      Print version information and exit.\n"
  "\n"
  "COMMANDS:\n"
  "      type '?<return>' at runtime for list\n"
  "\n"
  HELP_AND_CONTACT_INFO
  "\n";


CoordFlags flags;
static int offset_after_first_line = 0;
static bool blockUntilDone = false;
static bool killAfterCkptOnce = false;
static int blockUntilDoneRemote = -1;

static DmtcpCoordinator theCoordinator;

/* The coordinator can receive a second checkpoint request while processing the
 * first one.  If the second request comes at a point where the coordinator has
 * broadcast DMT_DO_SUSPEND message but the workers haven't replied, the
 * coordinator sends another DMT_DO_SUSPEND message.  The workers, having
 * replied to the first DMTCP_DO_SUSPEND message (by suspending all the user
 * threads), are waiting for the next message (DMT_DO_FD_LEADER_ELECTION or
 * DMT_KILL_PEER), however they receive DMT_DO_SUSPEND message and thus exit()
 * indicating an error.
 * The fix to this problem is to introduce a global
 * variable "workersRunningAndSuspendMsgSent" which, as the name implies,
 * indicates that the DMT_DO_SUSPEND message has been sent and the coordinator
 * is waiting for replies from the workers.  If this variable is set, the
 * coordinator will not process another checkpoint request.
*/
static bool workersRunningAndSuspendMsgSent = false;

static bool killInProgress = false;
static bool uniqueCkptFilenames = false;

const int STDIN_FD = fileno(stdin);

JTIMER(checkpoint);
JTIMER(restart);

static int workersAtCurrentBarrier = 0;
static string currentBarrier;
static string prevBarrier;
static ssize_t eventId = 0;

static UniquePid compId;
static int numRestartPeers = -1;
static time_t curTimeStamp = -1;
static time_t ckptTimeStamp = -1;

static LookupService lookupService;

static string coordHostname;
static struct in_addr localhostIPAddr;

#define MAX_EVENTS 10000
struct epoll_event events[MAX_EVENTS];
int epollFd;
static jalib::JSocket *listenSock = NULL;

static void removeStaleSharedAreaFile();
static void preExitCleanup();
static uint64_t getCurrTimestamp();

static pid_t _nextVirtualPid = INITIAL_VIRTUAL_PID;

static int theNextClientNumber = 1;
vector<CoordClient *>clients; // Default constructor sets 'size() == 0'
vector<CoordinatorPlugin*> CoordPluginMgr::plugins;
StaleTimeoutManager *CoordPluginMgr::staleTimeoutManager;
TimeoutManager *CoordPluginMgr::timeoutManager;
CkptIntervalManager *CoordPluginMgr::ckptIntervalManager;

char *get_ftime(char *buf, int size) {
  time_t rawtime;
  struct tm *timeinfo;
  time(&rawtime);
  timeinfo = localtime(&rawtime);
  strftime(buf, size, "%Y-%m-%d %H:%M:%S", timeinfo);
  return buf;
}

// This handler will be called on exit(), or signal termination,
//   but not if _exit() is called.
void atexit_handler() {
  FILE *file_ptr = std::fopen(flags.theStatusFile.c_str(), "a");
  char buffer[80];
  fprintf(file_ptr,
           "Coordinator exited: %s\n", get_ftime(buffer, sizeof(buffer)));
  fclose(file_ptr);
}

void signal_handler(int signum) {
  FILE *file_ptr = std::fopen(flags.theStatusFile.c_str(), "a");
  fprintf(file_ptr, "\n> Exiting due to signal %d received.\n", signum);
  fclose(file_ptr);
  atexit_handler();
  // Re-raise the signal to terminate this process.
  signal(signum, SIG_DFL);
  kill(getpid(), signum);
}

static inline void
ltrim(string &s)
{
  for (auto i = s.begin(); i != s.end(); ++i) {
    if (!std::isspace(*i)) {
      s.erase(s.begin(), i);
      break;
    }
  }
}

CoordClient::CoordClient(const jalib::JSocket &sock,
                         const struct sockaddr_storage *addr,
                         socklen_t len,
                         DmtcpMessage &hello_remote,
                         int isNSWorker)
  : _sock(sock),
    _barrier("")
{
  _isNSWorker = isNSWorker;
  _realPid = hello_remote.realPid;
  _clientNumber = theNextClientNumber++;
  _identity = hello_remote.from;
  _state = hello_remote.state;
  struct sockaddr_in *in = (struct sockaddr_in *)addr;
  _ip = inet_ntoa(in->sin_addr);
}

void
CoordClient::readProcessInfo(DmtcpMessage &msg)
{
  if (msg.extraBytes > 0) {
    char *extraData = new char[msg.extraBytes];
    _sock.readAll(extraData, msg.extraBytes);
    _hostname = extraData;
    _progname = extraData + _hostname.length() + 1;
    delete[] extraData;
  }
}

int DmtcpCoordinator::numClients()
{
  return clients.size();
}

pid_t
DmtcpCoordinator::getNewVirtualPid()
{
  pid_t pid = -1;

  ASSERT_LT(_virtualPidToClientMap.size(), (size_t)MAX_VIRTUAL_PID);

  while (1) {
    pid = _nextVirtualPid;
    _nextVirtualPid += VIRTUAL_PID_STEP;
    if (_nextVirtualPid > MAX_VIRTUAL_PID) {
      _nextVirtualPid = INITIAL_VIRTUAL_PID;
    }
    if (_virtualPidToClientMap.find(pid) == _virtualPidToClientMap.end()) {
      break;
    }
  }
  JASSERT(pid != -1).Text("Not Reachable");
  return pid;
}

static string replyData = "";

void
DmtcpCoordinator::handleUserCommand(string cmd, DmtcpMessage *reply /*= NULL*/)
{
  if (reply != NULL) {
    reply->coordCmdStatus = CoordCmdStatus::NOERROR;
  }

  if (cmd == "bc" || cmd == "kc" || cmd == "ck" || cmd == "K" || cmd == "c") {
    if (cmd == "bc") {
      blockUntilDone = true;
      JTRACE("blocking checkpoint beginning...");
    } else if (cmd == "kc" || cmd == "ck" || cmd == "K") {
      // dmtcp_command encodes this as 'cmd == "K"; '-kc' is the user flag.
      JTRACE("Will kill peers after creating the checkpoint...");
      killAfterCkptOnce = true;
    } else {
      JTRACE("checkpointing...");
    }

    if (startCheckpoint()) {
      if (reply != NULL) {
        reply->numPeers = getStatus().numPeers;
      }
    } else {
      if (reply != NULL) {
        reply->coordCmdStatus = CoordCmdStatus::ERROR_NOT_RUNNING_STATE;
      }
    }
  } else if (cmd == "l" || cmd == "t") {
    if (reply != NULL) {
      replyData = printList();
      reply->extraBytes = replyData.length();
    } else {
      JASSERT_STDERR << printList();
    }
  } else if (cmd == "u") {
    JASSERT_STDERR << "Host List:\n";
    JASSERT_STDERR << "HOST => # connected clients \n";
    dmtcp::map<string, int>clientHosts;
    for (size_t i = 0; i < clients.size(); i++) {
      if (clientHosts.find(clients[i]->hostname()) == clientHosts.end()) {
        clientHosts[clients[i]->hostname()] = 1;
      } else {
        clientHosts[clients[i]->hostname()] += 1;
      }
    }
    for (dmtcp::map<string, int>::iterator it = clientHosts.begin();
         it != clientHosts.end();
         ++it) {
      JASSERT_STDERR << it->first << " => " << it->second << '\n';
    }
  } else if (cmd == "q") {
    JNOTE("killing all connected peers and quitting ...");
    broadcastMessage(DMT_KILL_PEER);
    JASSERT_STDERR << "DMTCP coordinator exiting... (per request)\n";
    for (size_t i = 0; i < clients.size(); i++) {
      clients[i]->sock().close();
    }
    listenSock->close();
    preExitCleanup();
    JTRACE("Exiting ...");
    recordEvent("Exiting");
    serializeKVDB();
    exit(0);
  } else if (cmd == "i") {
    // Already handled by CkptIntervalManager
  } else if (cmd == "k") {
    JNOTE("Killing all connected peers...");
    broadcastMessage(DMT_KILL_PEER);
  } else if (cmd == "h" || cmd == "?") {
    JASSERT_STDERR << theHelpMessage;
  } else if (cmd == "s") {
    ComputationStatus s = getStatus();
    bool running = (s.minimumStateUnanimous &&
                    s.minimumState == WorkerState::RUNNING);
    if (reply != NULL) {
      reply->numPeers = s.numPeers;
      reply->isRunning = running;
      reply->theCheckpointInterval = CoordPluginMgr::ckptIntervalManager->theCheckpointInterval;
    } else {
      printStatus(s.numPeers, running);
    }
  } else {
    JNOTE("unhandled user command")(cmd);
    if (reply != NULL) {
      reply->coordCmdStatus = CoordCmdStatus::ERROR_INVALID_COMMAND;
    }
  }
}

void DmtcpCoordinator::getStatusStr(ostream *o)
{
  *o << "Status..." << std::endl
     << "Host: " << coordHostname
     << " (" << inet_ntoa(localhostIPAddr) << ")" << std::endl
     << "Port: " << flags.thePort << std::endl
     << "Checkpoint Interval: ";

  CoordPluginMgr::writeStatusToStream(o);
}

void
DmtcpCoordinator::writeStatusToFile()
{
  truncate(flags.theStatusFile.c_str(), offset_after_first_line);
  ofstream o;
  // Don't use std::ios::trunc.  A timestamp was previously written.
  o.open(flags.theStatusFile.c_str(), std::ios::app);
  JASSERT(!o.fail()) (flags.theStatusFile)
    .Text("Failed to truncate and open status file");

  getStatusStr(&o);

  o.close();
}

void
DmtcpCoordinator::printStatus(size_t numPeers, bool isRunning)
{
  ostringstream o;

  getStatusStr(&o);

  o << "Exit on last client: " << flags.exitOnLast << std::endl
    << "Kill after checkpoint: " << flags.killAfterCkpt
    << std::endl

    // << "Kill after checkpoint (first time only): " << killAfterCkptOnce
    // << std::endl
    << "Computation Id: " << compId << std::endl
    << "Checkpoint Dir: " << flags.ckptDir << std::endl
    << "NUM_PEERS=" << numPeers << std::endl
    << "RUNNING=" << (isRunning ? "yes" : "no") << std::endl
    << std::endl;
  printf("%s", o.str().c_str());
  fflush(stdout);
}

string
DmtcpCoordinator::printList()
{
  ostringstream o;

  o << "Client List:\n";
  o << "#, PROG[virtPID:realPID]@HOST, DMTCP-UNIQUEPID, STATE, BARRIER\n";
  for (size_t i = 0; i < clients.size(); i++) {
    o << clients[i]->clientNumber() << ", " << clients[i]->progname() << "["
      << clients[i]->identity().pid() << ":" << clients[i]->realPid() << "]@"
      << clients[i]->hostname()
#ifdef PRINT_REMOTE_IP
      << "(" << clients[i]->ip() << ")"
#endif // ifdef PRINT_REMOTE_IP
      << ", " << clients[i]->identity() << ", " << clients[i]->state() << ", "
      << clients[i]->barrier() << "\n";
  }
  o << "\n";
  return o.str();
}

void
DmtcpCoordinator::recordEvent(string const& event)
{
  eventId++;
  ostringstream o;
  o << std::setfill('0') << std::setw(5) << eventId << "-" << event;

  lookupService.set("/Event_Timestamp_Ms", Util::getTimestampStr(), o.str());
}

void
DmtcpCoordinator::serializeKVDB()
{
  if (!flags.writeKvData) {
    return;
  }

  ostringstream o;
  o << "dmtcp_coordinator_db-" << compId
    << "-" << Util::getTimestampStr() << ".json";
  lookupService.serialize(o.str());
  JNOTE("Wrote coordinator key-value db") (o.str());
}

void
DmtcpCoordinator::releaseBarrier(const string &barrier)
{
  ComputationStatus status = getStatus();

  if (workersAtCurrentBarrier == status.numPeers) {
    if (numRestartPeers > 0 && status.numPeers != numRestartPeers) {
      JNOTE("Waiting for all restarting processes to connect.")
        (numRestartPeers) (status.numPeers);
      return;
    }

    recordEvent("Barrier-" + barrier);
    JTRACE("Releasing barrier") (barrier);

    prevBarrier = currentBarrier;
    currentBarrier.clear();
    workersAtCurrentBarrier = 0;

    _numCkptWorkers = status.numPeers;
    broadcastMessage(DMT_BARRIER_RELEASED,
                     prevBarrier.length() + 1,
                     prevBarrier.c_str());
    if (status.minimumState == WorkerState::CHECKPOINTED) {
      JNOTE("Checkpoint complete; all workers running");
    }
  }
}

void
DmtcpCoordinator::processBarrier(const string &barrier)
{
  // Check if this is the first process to reach barrier.
  if (currentBarrier.empty()) {
    currentBarrier = barrier;
  } else {
    JASSERT(barrier == currentBarrier) (barrier) (currentBarrier);
  }

  ++workersAtCurrentBarrier;

  releaseBarrier(barrier);
}

void
DmtcpCoordinator::recordCkptFilename(CoordClient *client, const char *extraData)
{
  client->setState(WorkerState::CHECKPOINTED);
  JASSERT(extraData != NULL)
  .Text("extra data expected with DMT_CKPT_FILENAME message");

  string ckptFilename = extraData;
  string hostname = extraData + ckptFilename.length() + 1;
  string shellType;

  ckptFilename = extraData;
  shellType = extraData + ckptFilename.length() + 1;
  hostname = extraData + shellType.length() + 1 + ckptFilename.length() + 1;

  JTRACE("recording restart info") (ckptFilename) (hostname);
  JTRACE ( "recording restart info with shellType" )
    ( ckptFilename ) ( hostname ) (shellType);
  if(shellType.empty())
    _restartFilenames[hostname].push_back ( ckptFilename );
  else if(shellType == "rsh")
    _rshCmdFileNames[hostname].push_back( ckptFilename );
  else if(shellType == "ssh")
    _sshCmdFileNames[hostname].push_back( ckptFilename );
  else {
    JASSERT(0)(shellType)
      .Text("Shell command not supported. Report this to DMTCP community.");
  }
  _numRestartFilenames++;

  if (_numRestartFilenames == _numCkptWorkers) {
    const string restartScriptPath =
      RestartScript::writeScript(
        flags.ckptDir,
        uniqueCkptFilenames,
        ckptTimeStamp,
        CoordPluginMgr::ckptIntervalManager->theCheckpointInterval,
        flags.thePort,
        compId,
        _restartFilenames,
        _rshCmdFileNames,
        _sshCmdFileNames);

    JNOTE("Checkpoint complete. Wrote restart script") (restartScriptPath);

    JTIMER_STOP(checkpoint);
    recordEvent("Ckpt-Complete");
    serializeKVDB();

    if (blockUntilDone) {
      DmtcpMessage blockUntilDoneReply(DMT_USER_CMD_RESULT);
      JNOTE("replying to dmtcp_command:  we're done");

      // These were set in DmtcpCoordinator::onConnect in this file
      jalib::JSocket remote(blockUntilDoneRemote);
      remote << blockUntilDoneReply;
      remote.close();
      blockUntilDone = false;
      blockUntilDoneRemote = -1;
    }

    killAfterCkptOnce = false;
    _numRestartFilenames = 0;
    _numCkptWorkers = 0;

    // All the workers have checkpointed so now it is safe to reset this flag.
    workersRunningAndSuspendMsgSent = false;
  }
}

void
DmtcpCoordinator::onData(CoordClient *client)
{
  DmtcpMessage msg;

  JASSERT(client != NULL);

  if (client->sock().readAll((char*)&msg, sizeof(msg)) != sizeof(msg)) {
    JTRACE("Failed to read DmtcpMessage; probably dead connection.")
      (client->identity());
    return;
  }

  msg.assertValid();
  char *extraData = 0;
  if (msg.extraBytes > 0) {
    extraData = new char[msg.extraBytes];
    client->sock().readAll(extraData, msg.extraBytes);
  }

  WorkerState::eWorkerState prevClientState = client->state();
  client->setState(msg.state);

  switch (msg.type) {
  case DMT_WORKER_RESUMING:
  {
    JTRACE("Worker resuming execution")
      (msg.from) (prevClientState) (msg.state);

    client->setBarrier("");

    ComputationStatus s = getStatus();

    if (s.minimumStateUnanimous && s.minimumState == WorkerState::RUNNING) {
      // A worker is switching from RESTARTING, stop restart timer.
      // Multiple calls are harmless.
      if (prevClientState == WorkerState::RESTARTING) {
          JTIMER_STOP(restart);
          recordEvent("Restart-Complete");
          serializeKVDB();
          CoordPluginMgr::resumeAfterRestart(s);
        } else {
          CoordPluginMgr::resumeAfterCkpt(s);
        }
    }

    break;
  }

  case DMT_BARRIER:
  {
    string barrier = msg.barrier;
    JTRACE("got DMT_BARRIER message")
      (msg.from) (prevClientState) (msg.state) (barrier);

    // Warn if we have two consecutive barriers of the same name.
    JWARNING(barrier != client->barrier()) (barrier) (client->barrier());
    client->setBarrier(barrier);
    processBarrier(barrier);
    break;
  }

  case DMT_UNIQUE_CKPT_FILENAME:
    uniqueCkptFilenames = true;

  // Fall though
  case DMT_CKPT_FILENAME:
    recordCkptFilename(client, extraData);
    break;

  case DMT_GET_CKPT_DIR:
  {
    DmtcpMessage reply(DMT_GET_CKPT_DIR_RESULT);
    reply.extraBytes = flags.ckptDir.length() + 1;
    client->sock() << reply;
    client->sock().writeAll(flags.ckptDir.c_str(), reply.extraBytes);
    break;
  }
  case DMT_UPDATE_CKPT_DIR:
  {
    JASSERT(extraData != 0)
    .Text("extra data expected with DMT_UPDATE_CKPT_DIR message");
    if (strcmp(flags.ckptDir.c_str(), extraData) != 0) {
      flags.ckptDir = extraData;
      JNOTE("Updated ckptDir") (flags.ckptDir);
    }
    break;
  }

  case DMT_UPDATE_PROCESS_INFO_AFTER_FORK:
  {
    JNOTE("Updating process Information after fork()")
      (client->hostname()) (client->progname()) (msg.from) (client->identity());
    client->identity(msg.from);
    client->realPid(msg.realPid);
    break;
  }
  case DMT_UPDATE_PROCESS_INFO_AFTER_INIT_OR_EXEC:
  {
    string progname = extraData;
    JNOTE("Updating process Information after exec()")
      (progname) (msg.from) (client->identity());
    client->setState(msg.state);
    client->progname(progname);
    client->identity(msg.from);
    if (workersRunningAndSuspendMsgSent) {
      // If we received this message from the worker _after_ we broadcasted
      // DMT_DO_CHECKPOINT message to workers, there are two possible scenarios:
      // 1. User thread called exec before ckpt-thread had a chance to read the
      //    DMT_DO_CHECKPOINT message from the coordinator-socket. In this case,
      //    once the exec completes and a new ckpt-thread is created, that
      //    thread will read the pending DMT_DO_CHECKPOINT message and proceed
      //    as expected.
      // 2. The ckpt-thread read the DMT_DO_CHECKPOINT message, but before it
      //    could quiesce user threads, one of them called exec. After
      //    completing exec, the new ckpt-thread won't know that the coordinator
      //    had requested a checkpoint via DMT_DO_CHECKPOINT message. The
      //    ckpt-thread will block until it gets a DMT_DO_CHECKPOINT message
      //    while the coordinator is waiting for this process to respond to the
      //    earlier DMT_DO_CHECKPOINT message, leading to a deadlocked
      //    computation.
      //
      // In order to handle case (2) above, we send a second DMT_DO_CHECKPOINT
      // message to this worker. If this worker already processed the previous
      // DMT_DO_CHECKPOINT message as in case (1) above, it will ignore this
      // duplicate message.

      ResendDoCheckpointMsgToWorker(client);
    }
    break;
  }

  case DMT_KVDB_REQUEST:
  {
    JTRACE("received DMT_KVDB_REQUEST msg") (client->identity());
    lookupService.processRequest(client->sock(), msg, extraData);
    break;
  }

  default:
    JWARNING(false) (msg.type) (client->identity()) .Text(
      "unexpected message from worker. Closing connection");
    onDisconnect(client);
    break;
  }

  delete[] extraData;
}

static void
removeStaleSharedAreaFile()
{
  ostringstream o;

  o << flags.tmpDir
    << "/dmtcpSharedArea." << compId << "." << std::hex << curTimeStamp;
  JTRACE("Removing sharedArea file.") (o.str());
  unlink(o.str().c_str());
}

static void
preExitCleanup()
{
  removeStaleSharedAreaFile();
  JTRACE("Removing port-file") (flags.thePortFile);
  unlink(flags.thePortFile.c_str());
}

void
DmtcpCoordinator::onDisconnect(CoordClient *client)
{
  if (client->isNSWorker()) {
    client->sock().close();
    delete client;
    return;
  }
  for (size_t i = 0; i < clients.size(); i++) {
    if (clients[i] == client) {
      clients.erase(clients.begin() + i);
      break;
    }
  }
  client->sock().close();
  JNOTE("client disconnected") (client->identity()) (client->progname());
  _virtualPidToClientMap.erase(client->virtualPid());

  ComputationStatus s = getStatus();
  if (s.numPeers < 1) {
    if (flags.exitOnLast) {
      JNOTE("last client exited, shutting down..");
      handleUserCommand("q");
    } else {
      removeStaleSharedAreaFile();
    }

    // If a kill in is progress, the coordinator refuses any new connections,
    // thus we need to reset it to false once all the processes in the
    // computations have disconnected.
    killInProgress = false;
  } else {
    // If the coordinator waits at currentBarrier, try to release it.
    if (!currentBarrier.empty()) {
      // If already registered as a worker at current barrier,
      // decrement the worker counter before try to release the barrier.
      if (client->barrier() == currentBarrier) {
        --workersAtCurrentBarrier;
      }
      releaseBarrier(currentBarrier);
    }
  }

  CoordPluginMgr::clientDisconnected(client, s);

  delete client;
}

void
DmtcpCoordinator::initializeComputation()
{
  JNOTE("Resetting computation");

  // this is the first connection, do some initializations
  workersRunningAndSuspendMsgSent = false;
  killInProgress = false;

  // _nextVirtualPid = INITIAL_VIRTUAL_PID;

  // drop current computation group to 0
  compId = UniquePid(0, 0, 0);
  curTimeStamp = 0; // Drop timestamp to 0
  numRestartPeers = -1; // Drop number of peers to unknown
  blockUntilDone = false;
  killAfterCkptOnce = false;
  workersAtCurrentBarrier = 0;

  prevBarrier.clear();
  currentBarrier.clear();
}

void
DmtcpCoordinator::onConnect()
{
  struct sockaddr_storage remoteAddr;
  socklen_t remoteLen = sizeof(remoteAddr);
  jalib::JSocket remote = listenSock->accept(&remoteAddr, &remoteLen);

  JTRACE("accepting new connection") (remote.sockfd());

  if (!remote.isValid()) {
    remote.close();
    return;
  }

  DmtcpMessage hello_remote;
  hello_remote.poison();
  JTRACE("Reading from incoming connection...");
  remote >> hello_remote;
  if (!remote.isValid()) {
    remote.close();
    return;
  }

  if (hello_remote.type == DMT_NAME_SERVICE_WORKER) {
    CoordClient *client = new CoordClient(remote, &remoteAddr, remoteLen,
                                          hello_remote);

    addDataSocket(client);
    return;
  }

  if (hello_remote.type == DMT_USER_CMD) {
    // NOTE:  If 'dmtcp_command -i XX', then it comes through DMT_USER_CMD.
    //  In processDmtUserCmd, it wil update theDefaultCheckpointInterval and
    //  theCheckpointInterval.  But if the user tped 'dmtcp_launch -i XX ...',
    //  then it goes to DMT_NEW_WORKER, running initializeComputation(), and
    //  then updateCheckpointInterval(hello_remote.theCheckpointInterval),
    //  _without_ changing theDefaultCheckpointInterval.
    processDmtUserCmd(hello_remote, remote);
    return;
  }

  if (killInProgress) {
    JNOTE("Connection request received in the middle of killing computation. "
          "Sending it the kill message.");
    DmtcpMessage msg;
    msg.type = DMT_KILL_PEER;
    remote << msg;
    remote.close();
    return;
  }

  // If no client is connected to Coordinator, then there can be only zero data
  // sockets OR there can be one data socket and that should be STDIN.
  if (clients.size() == 0) {
    initializeComputation();
  }

  CoordClient *client = new CoordClient(remote, &remoteAddr, remoteLen,
                                        hello_remote);

  if (hello_remote.extraBytes > 0) {
    client->readProcessInfo(hello_remote);
  }

  if (hello_remote.type == DMT_RESTART_WORKER) {
    if (!validateRestartingWorkerProcess(hello_remote, remote,
                                         &remoteAddr, remoteLen)) {
      return;
    }
    client->virtualPid(hello_remote.from.pid());
    _virtualPidToClientMap[client->virtualPid()] = client;
  } else if (hello_remote.type == DMT_NEW_WORKER) {
    if (!validateNewWorkerProcess(hello_remote, remote, client,
                                  &remoteAddr, remoteLen)) {
      return;
    }
    _virtualPidToClientMap[client->virtualPid()] = client;
  } else {
    JWARNING(false) (hello_remote.type)
    .Text("Rejecting connect request from unknown remote process type.");
    remote.close();
    return;
  }

  JNOTE("worker connected") (hello_remote.from) (client->progname());

  clients.push_back(client);
  addDataSocket(client);

  ComputationStatus status = getStatus();
  CoordPluginMgr::clientConnected(client, hello_remote, status);
}

void
DmtcpCoordinator::processDmtUserCmd(DmtcpMessage &hello_remote,
                                    jalib::JSocket &remote)
{
  // dmtcp_command doesn't handshake (it is antisocial)
  JTRACE("got user command from dmtcp_command")((char)hello_remote.coordCmd);
  DmtcpMessage reply;
  reply.type = DMT_USER_CMD_RESULT;

  string cmd(1, hello_remote.coordCmd);

  // if previous 'b' blocking prefix command had set blockUntilDone
  if (blockUntilDone && blockUntilDoneRemote == -1 &&
      hello_remote.coordCmd == 'c') {
    // Reply will be done in DmtcpCoordinator::onData in this file.
    blockUntilDoneRemote = remote.sockfd();
    handleUserCommand(cmd, &reply);
  } else if (hello_remote.coordCmd == 'i') {
    handleUserCommand(cmd, &reply);
    remote << reply;
    remote.close();
  } else {
    handleUserCommand(cmd, &reply);
    remote << reply;
    if (reply.extraBytes > 0) {
      remote.writeAll(replyData.c_str(), reply.extraBytes);
    }
    remote.close();
  }
}

/*
 * Returns the current timestamp with nanosecond resolution
 */
static uint64_t
getCurrTimestamp()
{
  struct timespec value;
  uint64_t nsecs = 0;
  JASSERT(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
  nsecs = value.tv_sec*1000000000L + value.tv_nsec;
  return nsecs;
}

bool
DmtcpCoordinator::validateRestartingWorkerProcess(
  DmtcpMessage &hello_remote,
  jalib::JSocket &remote,
  const struct sockaddr_storage *remoteAddr,
  socklen_t remoteLen)
{
  if (hello_remote.state != WorkerState::RESTARTING) {
    JWARNING(false) (hello_remote.state)
    .Text("Rejecting restarting worker process with non-RESTARTING state.");
    remote.close();
    return false;
  }

  const struct sockaddr_in *sin = (const struct sockaddr_in *)remoteAddr;
  string remoteIP = inet_ntoa(sin->sin_addr);
  DmtcpMessage hello_local(DMT_ACCEPT);

  if (compId == UniquePid(0, 0, 0)) {
    lookupService.reset();
    recordEvent("Restarting-Computation");
    JASSERT(minimumState() == WorkerState::UNKNOWN) (minimumState())
    .Text("Coordinator should be idle at this moment");

    // Coordinator is free at this moment - set up all the things
    compId = hello_remote.compGroup;
    numRestartPeers = hello_remote.numPeers;
    curTimeStamp = getCurrTimestamp();
    JNOTE("FIRST restart connection. Set numRestartPeers. Generate timestamp")
      (numRestartPeers) (curTimeStamp) (compId);
    JTIMER_START(restart);
    recordEvent("Restart-Start");
  } else if (minimumState() != WorkerState::RESTARTING) {
    JNOTE("Computation not in RESTARTING state."
          "  Reject incoming computation process requesting restart.")
      (compId) (hello_remote.compGroup) (minimumState());
    hello_local.type = DMT_REJECT_NOT_RESTARTING;
    remote << hello_local;
    remote.close();
    return false;
  } else if (hello_remote.compGroup != compId) {
    JNOTE("Reject incoming computation process requesting restart,"
          " since it is not from current computation.")
      (compId) (hello_remote.compGroup);
    hello_local.type = DMT_REJECT_WRONG_COMP;
    remote << hello_local;
    remote.close();
    return false;
  }

  // dmtcp_restart already connected and compGroup created.
  // Computation process connection
  JASSERT(curTimeStamp != 0);

  JTRACE("Connection from (restarting) computation process")
    (compId) (hello_remote.compGroup) (minimumState());

  hello_local.coordTimeStamp = curTimeStamp;
  if (Util::strStartsWith(remoteIP.c_str(), "127.")) {
    memcpy(&hello_local.ipAddr, &localhostIPAddr, sizeof localhostIPAddr);
  } else {
    memcpy(&hello_local.ipAddr, &sin->sin_addr, sizeof localhostIPAddr);
  }
  remote << hello_local;

  // NOTE: Sending the same message twice. We want to make sure that the
  // worker process receives/processes the first messages as soon as it
  // connects to the coordinator. The second message will be processed in
  // postRestart routine in DmtcpWorker.
  //
  // The reason to do this is the following. The dmtcp_restart process
  // connects to the coordinator at a very early stage. Later on, before
  // exec()'ing into mtcp_restart, it reconnects to the coordinator using
  // it's original UniquiePid and closes the earlier socket connection.
  // However, the coordinator might process the disconnect() before it
  // processes the connect() which would lead to a situation where the
  // coordinator is not connected to any worker processes. The coordinator
  // would now process the connect() and may reject the worker because the
  // worker state is RESTARTING, but the minimumState() is UNKNOWN.
  // remote << hello_local;

  return true;
}

void
DmtcpCoordinator::ResendDoCheckpointMsgToWorker(CoordClient *client)
{
  JASSERT(workersRunningAndSuspendMsgSent);
  /* Worker trying to connect after SUSPEND message has been sent.
    * This happens if the worker process is executing a fork() or exec() system
    * call when the DMT_DO_SUSPEND is broadcast. We need to make sure that the
    * child process is allowed to participate in the current checkpoint.
    */
  ComputationStatus s = getStatus();
  JASSERT(s.numPeers > 0) (s.numPeers);
  JASSERT(s.minimumState != WorkerState::SUSPENDED) (s.minimumState);

  JNOTE("Sending DMT_DO_CHECKPOINT msg to worker") (client->identity());

  // Now send DMT_DO_CHECKPOINT message so that this process can also
  // participate in the current checkpoint
  DmtcpMessage suspendMsg(DMT_DO_CHECKPOINT);
  suspendMsg.compGroup = compId;
  client->sock() << suspendMsg;
}

bool
DmtcpCoordinator::validateNewWorkerProcess(
  DmtcpMessage &hello_remote,
  jalib::JSocket &remote,
  CoordClient *client,
  const struct sockaddr_storage *remoteAddr,
  socklen_t remoteLen)
{
  // Coming from dmtcp_launch or fork(), ssh(), etc.
  if (hello_remote.state != WorkerState::RUNNING &&
      hello_remote.state != WorkerState::UNKNOWN) {
    JWARNING(false) (hello_remote.state)
    .Text("state is not RUNNING or UNKNOWN; rejecting new connection");
    return false;
  }

  if (hello_remote.virtualPid != -1) {
    JWARNING(false) (hello_remote.virtualPid)
    .Text("virtualPid is not -1; rejecting new connection");
    return false;
  }

  const struct sockaddr_in *sin = (const struct sockaddr_in *)remoteAddr;
  string remoteIP = inet_ntoa(sin->sin_addr);
  DmtcpMessage hello_local(DMT_ACCEPT);

  client->virtualPid(getNewVirtualPid());

  hello_local.virtualPid = client->virtualPid();
  ComputationStatus s = getStatus();

  if (workersRunningAndSuspendMsgSent == true) {
    // Handshake
    hello_local.compGroup = compId;
    remote << hello_local;

    ResendDoCheckpointMsgToWorker(client);
  } else if (s.numPeers > 0 && s.minimumState != WorkerState::RUNNING &&
             s.minimumState != WorkerState::UNKNOWN) {
    // If some of the processes are not in RUNNING state
    JNOTE("Current computation not in RUNNING state."
          "  Refusing to accept new connections.")
      (compId) (hello_remote.from)
      (s.numPeers) (s.minimumState);
    hello_local.type = DMT_REJECT_NOT_RUNNING;
    remote << hello_local;
    remote.close();
    return false;
  } else if (hello_remote.compGroup != UniquePid()) {
    // New Process trying to connect to Coordinator but already has compGroup
    JNOTE("New process not part of currently running computation group"
          "on this theCoordinator.  Rejecting.")
      (hello_remote.compGroup);

    hello_local.type = DMT_REJECT_WRONG_COMP;
    remote << hello_local;
    remote.close();
    return false;
  } else {
    // If first process, create the new computation group
    if (compId == UniquePid(0, 0, 0)) {
      // Connection of new computation.
      compId = UniquePid(hello_remote.from.hostid(), client->virtualPid(),
                         hello_remote.from.time(),
                         hello_remote.from.computationGeneration());

      // Get the resolution down to 100 mili seconds.
      curTimeStamp = getCurrTimestamp();
      numRestartPeers = -1;
      JTRACE("First process connected.  Creating new computation group.")
        (compId);
      recordEvent("Initializing-Computation");
    } else {
      JTRACE("New process connected")
        (hello_remote.from) (client->virtualPid());
    }
    hello_local.compGroup = compId;
    hello_local.coordTimeStamp = curTimeStamp;
    if (Util::strStartsWith(remoteIP.c_str(), "127.")) {
      memcpy(&hello_local.ipAddr, &localhostIPAddr, sizeof localhostIPAddr);
    } else {
      memcpy(&hello_local.ipAddr, &sin->sin_addr, sizeof localhostIPAddr);
    }
    remote << hello_local;
  }
  return true;
}

bool
DmtcpCoordinator::startCheckpoint()
{
  ComputationStatus s = getStatus();
  if (s.minimumState == WorkerState::RUNNING && s.minimumStateUnanimous
      && !workersRunningAndSuspendMsgSent) {
    uniqueCkptFilenames = false;
    time(&ckptTimeStamp);
    JTIMER_START(checkpoint);
    recordEvent("Ckpt-Start");
    _numRestartFilenames = 0;
    numRestartPeers = -1;
    _restartFilenames.clear();
    _rshCmdFileNames.clear();
    _sshCmdFileNames.clear();
    compId.incrementGeneration();
    JNOTE("starting checkpoint; incrementing generation; suspending all nodes")
      (s.numPeers) (compId.computationGeneration());

    // Pass number of connected peers to all clients
    broadcastMessage(DMT_DO_CHECKPOINT);
    // On worker side, after receiving DMT_DO_CHECKPOINT, the plugin manager
    // sends out DMTCP_EVENT_PRESUSPEND followed by DMTCP_EVENT_CHECKPOINT
    // to each plugin.  The callbacks for those events may call
    // dmtcp_global_barrier(), which sends back a DMT_BARRIER msg before
    // the workers do the actual checkpoint.

    // Suspend Message has been sent but the workers are still in running
    // state.  If the coordinator receives another checkpoint request from user
    // at this point, it should fail.
    workersRunningAndSuspendMsgSent = true;
    return true;
  } else {
    if (s.numPeers > 0) {
      JTRACE("delaying checkpoint, workers not ready") (s.minimumState)
        (s.numPeers);
    }
    return false;
  }
}

void
DmtcpCoordinator::broadcastMessage(DmtcpMessageType type,
                                   size_t extraBytes,
                                   const void *extraData)
{
  DmtcpMessage msg;

  msg.type = type;
  msg.compGroup = compId;
  msg.numPeers = clients.size();
  // From DMTCP coord viewpoint, we are killing peers after ckpt.
  // From DMTCP peer viewpoint, we will exit after ckpt.
  msg.exitAfterCkpt = flags.killAfterCkpt || killAfterCkptOnce;
  msg.extraBytes = extraBytes;

  if (msg.type == DMT_KILL_PEER && clients.size() > 0) {
    killInProgress = true;
  }

  JTRACE("sending message")(type);
  for (size_t i = 0; i < clients.size(); i++) {
    clients[i]->sock() << msg;
    if (extraBytes > 0) {
      clients[i]->sock().writeAll((const char *)extraData, extraBytes);
    }
  }
  workersAtCurrentBarrier = 0;
}

ComputationStatus
DmtcpCoordinator::getStatus() const
{
  ComputationStatus status;
  const static WorkerState::eWorkerState INITIAL_MIN = WorkerState::_MAX;
  const static WorkerState::eWorkerState INITIAL_MAX = WorkerState::UNKNOWN;
  int min = INITIAL_MIN;
  int max = INITIAL_MAX;
  int count = 0;
  bool unanimous = true;

  for (size_t i = 0; i < clients.size(); i++) {
    WorkerState::eWorkerState cliState = clients[i]->state();
    count++;
    unanimous = unanimous && (min == cliState || min == INITIAL_MIN);
    if (cliState < min) {
      min = cliState;
    }
    if (cliState > max) {
      max = cliState;
    }
  }

  status.minimumStateUnanimous = unanimous;
  status.minimumState = (min == INITIAL_MIN ? WorkerState::UNKNOWN
                         : (WorkerState::eWorkerState)min);
  if (status.minimumState == WorkerState::RESTARTING &&
      count < numRestartPeers) {
    JTRACE("minimal state counted as RESTARTING but not all processes"
           " are connected yet.  So we wait.") (numRestartPeers) (count);
    status.minimumState = WorkerState::RESTARTING;
    status.minimumStateUnanimous = false;
  }

  status.maximumState = (max == INITIAL_MAX ? WorkerState::UNKNOWN
                         : (WorkerState::eWorkerState)max);
  status.numPeers = count;

  ASSERT_EQ(0, clock_gettime(CLOCK_MONOTONIC, &status.timestamp));
  return status;
}


static void
signalHandler(int signum)
{
  if (signum == SIGINT) {
    theCoordinator.handleUserCommand("q");
  } else {
    JWARNING(false)(signum).Text("Ignoring unexpected signal");
  }
}

static void
setupSignalHandlers()
{
  struct sigaction action;

  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  action.sa_handler = signalHandler;

  sigaction(SIGINT, &action, NULL);
}

// This code is also copied to ssh.cpp:updateCoordHost()
static void
calcLocalAddr()
{
  char hostname[HOST_NAME_MAX];

  JASSERT(gethostname(hostname, sizeof hostname) == 0) (JASSERT_ERRNO);

  struct addrinfo *result = NULL;
  struct addrinfo *res;
  int error;
  struct addrinfo hints;

  memset(&localhostIPAddr, 0, sizeof localhostIPAddr);
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_UNSPEC; // accept AF_INET and AF_INET6
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  hints.ai_protocol = 0;
  hints.ai_canonname = NULL;
  hints.ai_addr = NULL;
  hints.ai_next = NULL;

  // FROM: Wikipedia:CNAME_record:
  //  When a DNS resolver encounters a CNAME record while looking for a regular
  //  resource record, it will restart the query using the canonical name
  //  instead of the original name. (If the resolver is specifically told to
  //  look for CNAME records, the canonical name (right-hand side) is returned,
  //  rather than restarting the query.)
  hints.ai_flags |= AI_CANONNAME;
  error = getaddrinfo(hostname, NULL, &hints, &result);
  hints.ai_flags ^= AI_CANONNAME;
  if (error == 0 && result) {
    // if hostname was not fully qualified with domainname, replace it with
    // canonname.  Otherwise, keep current alias returned from gethostname().
    if ( Util::strStartsWith(result->ai_canonname, hostname) &&
         result->ai_canonname[strlen(hostname)] == '.' &&
         strlen(result->ai_canonname) < sizeof(hostname) ) {
      strncpy(hostname, result->ai_canonname, sizeof hostname);
    }
    freeaddrinfo(result);
  }
  // OPTIONAL:  If we still don't have a domainname, we could resolve with DNS
  //   (similar to 'man 1 host'), but we ont't know if Internet is present.

  /* resolve the hostname into a list of addresses */
  error = getaddrinfo(hostname, NULL, &hints, &result);
  if (error == 0) {
    /* loop over all returned results and do inverse lookup */
    bool success = false;
    bool at_least_one_match = false;
    char name[NI_MAXHOST] = "";
    for (res = result; res != NULL; res = res->ai_next) {
      struct sockaddr_in *s = (struct sockaddr_in *)res->ai_addr;

      error = getnameinfo(res->ai_addr,
                          res->ai_addrlen,
                          name,
                          NI_MAXHOST,
                          NULL,
                          0,
                          0);
      if (error != 0) {
        JTRACE("getnameinfo() failed.") (gai_strerror(error));
        continue;
      } else {
        JASSERT(sizeof localhostIPAddr == sizeof s->sin_addr);
        if ( strncmp( name, hostname, sizeof hostname ) == 0 ) {
          success = true;
          memcpy(&localhostIPAddr, &s->sin_addr, sizeof s->sin_addr);
          break; // Stop here.  We found a matching hostname.
        }
        if (!at_least_one_match) { // Prefer the first match over later ones.
          at_least_one_match = true;
          memcpy(&localhostIPAddr, &s->sin_addr, sizeof s->sin_addr);
        }
      }
    }
    if (result) {
      freeaddrinfo(result);
    }
    if (at_least_one_match) {
      success = true;  // Call it a success even if hostname != name
      if ( strncmp( name, hostname, sizeof hostname ) != 0 ) {
        JTRACE("Canonical hostname different from original hostname")
              (name)(hostname);
      }
    }

    JWARNING(success) (hostname)
      .Text("Failed to find coordinator IP address.  DMTCP may fail.");
  } else {
    if (error == EAI_SYSTEM) {
      perror("getaddrinfo");
    } else {
      JTRACE("Error in getaddrinfo") (gai_strerror(error));
    }
    inet_aton("127.0.0.1", &localhostIPAddr);
  }

  coordHostname = hostname;
}

void
printPrompt()
{
  if (std::cin.eof() != 1) {
    printf("dmtcp> ");
    fflush(stdout);
  }
}

void
clearPrompt()
{
  printf("\r");
  fflush(stdout);
}

void DmtcpCoordinator::queueCheckpoint()
{
  theCoordinator.checkpointQueued = true;
}

void
DmtcpCoordinator::eventLoop()
{
  struct epoll_event ev;

  epollFd = epoll_create(MAX_EVENTS);
  JASSERT(epollFd != -1) (JASSERT_ERRNO);

  ev.events = EPOLLIN;
  ev.data.ptr = listenSock;
  JASSERT(epoll_ctl(epollFd, EPOLL_CTL_ADD, listenSock->sockfd(), &ev) != -1)
    (JASSERT_ERRNO);

  if (!flags.daemon &&

      // epoll_ctl below fails if STDIN is pointing to /dev/null.
      // Not sure why.
      jalib::Filesystem::GetDeviceName(0) != "/dev/null" &&
      jalib::Filesystem::GetDeviceName(0) != "/dev/zero" &&
      jalib::Filesystem::GetDeviceName(0) != "/dev/random") {
    ev.events = EPOLLIN;
#ifdef EPOLLRDHUP
    ev.events |= EPOLLRDHUP;
#endif // ifdef EPOLLRDHUP
    ev.data.ptr = (void *)STDIN_FILENO;
    JASSERT(epoll_ctl(epollFd, EPOLL_CTL_ADD, STDIN_FILENO, &ev) != -1)
      (JASSERT_ERRNO);
  }

  while (true) {
    // Update plugins in case there was some client activity.
    CoordPluginMgr::tick(getStatus());

    printPrompt();

    // Wait until either there is some activity on client sockets, or the timer
    // has expired.
    int nfds;
    do {
      nfds = epoll_wait(epollFd, events, MAX_EVENTS, 1000);
      if (nfds == 0) {
        // Epoll due to timeout. Let's trigger a tick() for plugins.
        // The plugins can use status.timestamp to handle timeouts, etc.
        CoordPluginMgr::tick(getStatus());
      }
    } while ((nfds == 0 || (nfds < 0 && errno == EINTR)) && !checkpointQueued);

    clearPrompt();

    // alarm() is not always the only source of interrupts.
    // For example, any signal, including signal 0 or SIGWINCH can cause this.
    JASSERT(nfds != -1 || errno == EINTR) (JASSERT_ERRNO);

    for (int n = 0; n < nfds; ++n) {
      void *ptr = events[n].data.ptr;

      // Epoll wait may return EPOLLIN along with EPOLLHUP if the client closed
      // the socket right after sending a message to the coordinator. This is
      // noticed when using kill-after-checkpoint flag where the client sends
      // the checkpoint filename to the coordinator and immediately calls exit()
      // (which in turn closes the coordinator socket). If we don't process
      // EPOLLIN before processing EPOLLHUP, we lose the DMTCP_CKPT_FILENAME
      // message altogether and fail to write restart script.

      // First read any available data from the client socket.
      if (events[n].events & EPOLLIN) {
        if (ptr == (void *)listenSock) {
          onConnect();
        } else if (ptr == (void *)STDIN_FILENO) {
          string cmd;
          std::getline(std::cin, cmd);
          if (std::cin.eof() == 1) {
            JASSERT_STDERR << "\n  Closing stdin...\n";
            JASSERT(epoll_ctl(epollFd, EPOLL_CTL_DEL, STDIN_FILENO, &ev) != -1)
              (JASSERT_ERRNO);
            close(STDIN_FD);
          } else {
            std::transform(cmd.begin(), cmd.end(), cmd.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            ltrim(cmd);
            if (!cmd.empty()) {
              handleUserCommand(cmd);
            }
          }
        } else {
          onData((CoordClient *)ptr);
        }
      }

      // Now check if the client socket is closed.
      if ((events[n].events & EPOLLHUP) ||
#ifdef EPOLLRDHUP
          (events[n].events & EPOLLRDHUP) ||
#endif // ifdef EPOLLRDHUP
          (events[n].events & EPOLLERR)) {
        JASSERT(ptr != listenSock);
        if (ptr == (void *)STDIN_FILENO) {
          JASSERT(epoll_ctl(epollFd, EPOLL_CTL_DEL, STDIN_FILENO, &ev) != -1)
            (JASSERT_ERRNO);
          close(STDIN_FD);
        } else {
          onDisconnect((CoordClient *)ptr);
        }
      }
    }

    if (checkpointQueued) {
      checkpointQueued = false;
      startCheckpoint();
    }
  }
}

void
DmtcpCoordinator::addDataSocket(CoordClient *client)
{
  struct epoll_event ev;

#ifdef EPOLLRDHUP
  ev.events = EPOLLIN | EPOLLRDHUP;
#else // ifdef EPOLLRDHUP
  ev.events = EPOLLIN;
#endif // ifdef EPOLLRDHUP
  ev.data.ptr = client;
  JASSERT(epoll_ctl(epollFd, EPOLL_CTL_ADD, client->sock().sockfd(), &ev) != -1)
    (JASSERT_ERRNO);
}

#define min(x, y) ((x) < (y) ? (x) : (y))

// Copy name+suffix into short_buf of length len, and truncate name to fit.
// This keeps only the last component of name (after last '/')
char *short_name(char short_buf[], char *name, unsigned int len, char *suffix) {
  char name_copy[strlen(name)+1];
  memcpy(name_copy, name, strlen(name)+1);
  char *base_name = strrchr(name_copy, '/') == NULL ?
                    name_copy : strrchr(name_copy, '/') + 1;
  int suffix_len = strlen(suffix);
  if (6 + strlen(suffix) > len) {
    return NULL;
  }
  memset(short_buf, '\0', len);
  int cmd_len = min(strlen(base_name)+1, len);
  memcpy(short_buf, base_name, cmd_len);
  int short_buf_len = min(strlen(base_name), len - suffix_len - 1);
  strncpy(short_buf + short_buf_len, suffix, suffix_len);
  return short_buf;
}

void set_short_cmdline(char *argv0, const char *port) {
  char buf[16] = "               ";
  char port_str[16] =":";
  strncpy(port_str+1, port, sizeof(port_str)+1-strlen(port));
  short_name(buf, argv0, sizeof buf, port_str);
  // For debugging
  // printf("buf: %s\n", buf);
  prctl(PR_SET_NAME, buf); // set /proc/self/comm
}

void set_long_cmdline(char *argv0, const char *port) {
  char *argv0_copy = (char *)malloc(strlen(argv0) + 1);
  strcpy(argv0_copy, argv0);
  char *base_argv0 = strrchr(argv0_copy, '/') == NULL ?
                     argv0_copy : strrchr(argv0_copy, '/') + 1;
  char port_flag_long[100];
  char port_flag_short[100];
  snprintf(port_flag_long, sizeof(port_flag_long), " --port %s", port);
  snprintf(port_flag_short, sizeof(port_flag_short), " -p%s", port);
  if (strlen(base_argv0)+strlen(port_flag_long) <= strlen(argv0) &&
      short_name(argv0, argv0, strlen(argv0), port_flag_long) != NULL) {
  // For debugging
  // printf("base_argv0-1: %s\n", argv0);
  } else if (short_name(argv0, argv0, strlen(argv0), port_flag_short) != NULL) {
  // For debugging
  // printf("base_argv0-2: %s\n", argv0);
  } else {
  // For debugging
  // printf("base_argv0-3: %s\n", argv0);
  }
  free(argv0_copy);
}

#define shift argc--; argv++

int
main(int argc, char **argv)
{
  initializeJalib();

  flags.progName = argv[0];

  set_short_cmdline(argv[0], jalib::XToString(flags.thePort).c_str());
  set_long_cmdline(argv[0], jalib::XToString(flags.thePort).c_str());

  /* NOTE: The convention is that user-specified explicit runtime arguments
   *       get a higher priority than env. vars. The logFilename variable will
   *       be over-written if the coordinator was invoked with
   *       `--logfile <filename>.
   */

  shift;
  while (argc > 0) {
    string s = argv[0];
    if (s == "-h" || s == "--help") {
      printf("%s", theUsage);
      return 0;
    } else if ((s == "--version") && argc == 1) {
      printf("%s", DMTCP_VERSION_AND_COPYRIGHT_INFO);
      return 0;
    } else if (s == "-q" || s == "--quiet") {
      flags.quiet = true;
      flags.jassert_quiet++;
      shift;
    } else if (s == "--exit-on-last") {
      flags.exitOnLast = true;
      shift;
    } else if (s == "--kill-after-ckpt") {
      flags.killAfterCkpt = true;
      shift;
    } else if (argc > 1 && s == "--timeout") {
      flags.timeout = atol(argv[1]);
      shift; shift;
    } else if (argc > 1 && s == "--stale-timeout") {
      flags.staleTimeout = atol(argv[1]);
      shift; shift;
    } else if (s == "--daemon") {
      flags.daemon = true;
      shift;
    } else if (s == "--coord-logfile") {
      flags.useLogFile = true;
      flags.logFilename = argv[1];
      shift; shift;
    } else if (s == "-i" || s == "--interval") {
      flags.interval = atol(argv[1]);
      shift; shift;
    } else if (argv[0][0] == '-' && argv[0][1] == 'i' &&
               isdigit(argv[0][2])) { // else if -i5, for example
      flags.interval = atol(&argv[0][2]);
      shift;
    } else if (argc > 1 &&
               (s == "-p" || s == "--port" || s == "--coord-port")) {
      flags.thePort = jalib::StringToInt(argv[1]);
      shift; shift;
    } else if (argv[0][0] == '-' && argv[0][1] == 'p' &&
               isdigit(argv[0][2])) { // else if -p0, for example
      flags.thePort = jalib::StringToInt(argv[0] + 2);
      shift;
    } else if (argc > 1 && s == "--port-file") {
      flags.thePortFile = argv[1];
      shift; shift;
    } else if (argc > 1 && s == "--status-file") {
      flags.theStatusFile = argv[1];
      shift; shift;
    } else if (argc > 1 && (s == "-c" || s == "--ckptdir")) {
      flags.ckptDir = argv[1];
      shift; shift;
    } else if (argc > 1 && (s == "-t" || s == "--tmpdir")) {
      flags.tmpDir = argv[1];
      shift; shift;
    } else if (s == "--write-kv-data") {
      flags.writeKvData = true;
      shift;
    } else if (argc == 1) { // last arg can be port
      char *endptr;
      long x = strtol(argv[0], &endptr, 10);
      if ((ssize_t)strlen(argv[0]) != endptr - argv[0]) {
        fprintf(stderr, theUsage, DEFAULT_PORT);
        return 1;
      } else {
        flags.thePort = jalib::StringToInt(argv[0]);
        shift;
      }
      x++, x--; // to suppress unused variable warning
    } else {
      fprintf(stderr, theUsage, DEFAULT_PORT);
      return 1;
    }
  }

  flags.tmpDir = Util::calcTmpDir(flags.tmpDirArg.c_str());
  Util::initializeLogFile(flags.tmpDir.c_str(), "dmtcp_coordinator");

  JTRACE("New DMTCP coordinator starting.")
    (UniquePid::ThisProcess());

  if (flags.thePort < 0) {
    fprintf(stderr, theUsage, DEFAULT_PORT);
    return 1;
  }

  calcLocalAddr();

  /*Test if the listener socket is already open*/
  if (fcntl(PROTECTED_COORD_FD, F_GETFD) != -1) {
    listenSock = new jalib::JServerSocket(PROTECTED_COORD_FD);
    JASSERT(listenSock->port() != -1).Text("Invalid listener socket");
    JTRACE("Using already created listener socket") (listenSock->port());
  } else {
    errno = 0;
    listenSock = new jalib::JServerSocket(jalib::JSockAddr::ANY, flags.thePort, 128);
    JASSERT(listenSock->isValid()) (flags.thePort) (JASSERT_ERRNO)
    .Text("Failed to create listen socket."
          "\nIf msg is \"Address already in use\", "
          "this may be an old coordinator."
          "\nKill default coordinator and try again:  dmtcp_command -q"
          "\nIf that fails, \"pkill -9 dmtcp_coord\","
          " and try again in a minute or so.");
  }

  flags.thePort = listenSock->port();
  if (!flags.thePortFile.empty()) {
    Util::writeCoordPortToFile(flags.thePort, flags.thePortFile.c_str());
  }
  JTRACE("Listening on port")(flags.thePort);

  if (!flags.quiet) {
    fprintf(stderr, "dmtcp_coordinator starting..."
                    "\n    Host: %s (%s)"
                    "\n    Port: %d"
                    "\n    Checkpoint Interval: ",
            coordHostname.c_str(), inet_ntoa(localhostIPAddr), flags.thePort);
    if (flags.interval == 0) {
      fprintf(stderr, "disabled (checkpoint manually instead)");
    } else {
      fprintf(stderr, "%d", flags.interval);
    }
    fprintf(stderr, "\n    Exit on last client: %d\n", flags.exitOnLast);
  }

  if (flags.daemon) {
    if (!flags.quiet) {
      JASSERT_STDERR << "Backgrounding...\n";
    }
    int fd = -1;
    if (!flags.useLogFile) {
      fd = open("/dev/null", O_RDWR);
      JASSERT(dup2(fd, STDIN_FILENO) == STDIN_FILENO);
    } else {
      fd = open(flags.logFilename.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0666);
      JASSERT_SET_LOG(flags.logFilename);
      int nullFd = open("/dev/null", O_RDWR);
      JASSERT(dup2(nullFd, STDIN_FILENO) == STDIN_FILENO);
      close(nullFd);
    }
    JASSERT(dup2(fd, STDOUT_FILENO) == STDOUT_FILENO);
    JASSERT(dup2(fd, STDERR_FILENO) == STDERR_FILENO);
    JASSERT_CLOSE_STDERR();
    if (fd > STDERR_FILENO) {
      close(fd);
    }

    if (fork() > 0) {
      JTRACE("Parent Exiting after fork()");
      exit(0);
    }

    // pid_t sid = setsid();
  } else {
    if (!flags.quiet) {
      JASSERT_STDERR <<
        "Type '?' for help." <<
        "\n\n";
    }
  }

  /* We set up the signal handler for SIGINT and SIGALRM.
   * SIGINT is used to send DMT_KILL_PEER message to all the connected peers
   * before exiting.
   * SIGALRM is used for interval checkpointing.
   */
  setupSignalHandlers();

  /* If the coordinator was started transparently by dmtcp_launch, then we
   * want to block signals, such as SIGINT.  To see why this is important:
   * % gdb dmtcp_launch a.out
   * (gdb) run
   * ^C   # Stop gdb to get its attention, and continue debugging.
   * # The above scenario causes the SIGINT to go to a.out and its child,
   * # the dmtcp_coordinator.  The coord then triggers the SIGINT handler,
   * # which sends DMT_KILL_PEER to kill a.out.
   */
  if (flags.exitOnLast && flags.daemon) {
    sigset_t set;
    sigfillset(&set);

    // unblock SIGALRM because we are using alarm() for interval checkpointing
    sigdelset(&set, SIGALRM);

    // sigprocmask is only per-thread; but the coordinator is single-threaded.
    sigprocmask(SIG_BLOCK, &set, NULL);
  }

  if (!flags.theStatusFile.empty()) {
    FILE *file_ptr = std::fopen(flags.theStatusFile.c_str(), "w");
    if (file_ptr == NULL) {
      fprintf(stderr, "Error opening file %s\n", flags.theStatusFile.c_str());
      return 1;
    }
    char buffer[80];
    char output[80];
    snprintf(output, sizeof(output),
             "Coordinator started: %s\n", get_ftime(buffer, sizeof(buffer)));
    offset_after_first_line = strlen(output);
    fprintf(file_ptr, "%s", output);
    fclose(file_ptr);
    theCoordinator.writeStatusToFile();

    atexit(atexit_handler);
    signal(SIGINT, signal_handler);  // Ctrl+C
    signal(SIGTERM, signal_handler); // Termination request
    signal(SIGABRT, signal_handler); // Abort signal
    signal(SIGSEGV, signal_handler); // segfault
    signal(SIGQUIT, signal_handler); // quit signal
  }

  CoordPluginMgr::initialize(flags);
  theCoordinator.eventLoop();
  return 0;
}
