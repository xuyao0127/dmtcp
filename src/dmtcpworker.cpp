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

#include "dmtcpworker.h"
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/time.h>
#include "../jalib/jbuffer.h"
#include "../jalib/jconvert.h"
#include "../jalib/jfilesystem.h"
#include "../jalib/jsocket.h"
#include "coordinatorapi.h"
#include "kvdb.h"
#include "pluginmanager.h"
#include "processinfo.h"
#include "procselfmaps.h"
#include "shareddata.h"
#include "syscallwrappers.h"
#include "syslogwrappers.h"
#include "threadlist.h"
#include "threadsync.h"
#include "util.h"

using namespace dmtcp;

LIB_PRIVATE void dmtcp_prepare_atfork(void);

EXTERNC void *ibv_get_device_list(void *) __attribute__((weak));
EXTERNC ProcSelfMaps *procSelfMaps;

/* The following instance of the DmtcpWorker is just to trigger the constructor
 * to allow us to hijack the process
 */
static ATOMIC_SHARED_GLOBAL bool exitInProgress = false;
static bool exitAfterCkpt = 0;
static bool dmtcp_initialized = false;


/* NOTE:  Please keep this function in sync with its copy at:
 *   dmtcp_nocheckpoint.cpp:restoreUserLDPRELOAD()
 */
void
restoreUserLDPRELOAD()
{
  /* A call to setenv() can result in a call to malloc(). The setenv() call may
   * also grab a low-level libc lock before calling malloc. The malloc()
   * wrapper, if present, will try to acquire the wrapper lock. This can lead
   * to a deadlock in the following scenario:
   *
   * T1 (main thread): fork() -> acquire exclusive lock
   * T2 (ckpt thread): setenv() -> acquire low-level libc lock ->
   *                   malloc -> wait for wrapper-exec lock.
   * T1: setenv() -> block on low-level libc lock (held by T2).
   *
   * The simpler solution would have been to not call setenv from DMTCP, and
   * use putenv instead. This would require larger change.
   *
   * The solution used here is to set LD_PRELOAD to "" before * user main().
   * This is as good as unsetting it.  Later, the ckpt-thread * can unset it
   * if it is still NULL, but then there is a possibility of a race between
   * user code and ckpt-thread.
   */

  // We have now successfully used LD_PRELOAD to execute prior to main()
  // Next, hide our value of LD_PRELOAD, in a global variable.
  // At checkpoint and restart time, we will no longer need our LD_PRELOAD.
  // We will need it in only one place:
  // when the user application makes an exec call:
  // If anybody calls our execwrapper, we will reset LD_PRELOAD then.
  // EXCEPTION:  If anybody directly calls _real_execve with env arg of NULL,
  // they will not be part of DMTCP computation.
  // This has the advantage that our value of LD_PRELOAD will always come
  // before any paths set by user application.
  // Also, bash likes to keep its own envp, but we will interact with bash only
  // within the exec wrapper.
  // NOTE:  If the user called exec("ssh ..."), we currently catch this in
  // src/pugin/dmtcp_ssh.cp:main(), and edit this into
  // exec("dmtcp_launch ... dmtcp_ssh ..."), and re-execute.
  // NOTE:  If the user called exec("dmtcp_nocheckpoint ..."), we will
  // reset LD_PRELOAD back to ENV_VAR_ORIG_LD_PRELOAD in dmtcp_nocheckpoint
  char *preload = getenv("LD_PRELOAD");
  char *userPreload = getenv(ENV_VAR_ORIG_LD_PRELOAD);

  JASSERT(userPreload == NULL || strlen(userPreload) <= strlen(preload))
  (preload)(userPreload);

  // Destructively modify environment variable "LD_PRELOAD" in place:
  preload[0] = '\0';
  if (userPreload == NULL) {
    // _dmtcp_unsetenv("LD_PRELOAD");
  } else {
    strcat(preload, userPreload);

    // setenv("LD_PRELOAD", userPreload, 1);
  }
  JTRACE("LD_PRELOAD") (preload) (userPreload) (getenv(ENV_VAR_HIJACK_LIBS))
    (getenv(ENV_VAR_HIJACK_LIBS_M32)) (getenv("LD_PRELOAD"));
}

// This should be visible to library only.  DmtcpWorker will call
// this to initialize tmp (ckpt signal) at startup time.  This avoids
// any later calls to getenv(), at which time the user app may have
// a wrapper around getenv, modified environ, or other tricks.
// (Matlab needs this or else it segfaults on restart, and bash plays
// similar tricks with maintaining its own environment.)
// Used in mtcpinterface.cpp and signalwrappers.cpp.
// FIXME: DO we still want it to be library visible only?
// __attribute__ ((visibility ("hidden")))
int
DmtcpWorker::determineCkptSignal()
{
  int sig = CKPT_SIGNAL;
  char *endp = NULL;
  static const char *tmp = getenv(ENV_VAR_SIGCKPT);

  if (tmp != NULL) {
    sig = strtol(tmp, &endp, 0);
    if ((errno != 0) || (tmp == endp)) {
      sig = CKPT_SIGNAL;
    }
    if (sig < 1 || sig >= SIGRTMAX) {
      sig = CKPT_SIGNAL;
    }
  }
  return sig;
}

static void
prepareLogAndProcessdDataFromSerialFile()
{
  if (Util::isValidFd(PROTECTED_LIFEBOAT_FD)) {
    jalib::JBinarySerializeReaderRaw rd("", PROTECTED_LIFEBOAT_FD);
    rd.rewind();
    UniquePid::serialize(rd);

    DmtcpEventData_t edata;
    edata.postExec.serializationFd = PROTECTED_LIFEBOAT_FD;
    PluginManager::eventHook(DMTCP_EVENT_POST_EXEC, &edata);
    _real_close(PROTECTED_LIFEBOAT_FD);
  } else {
    // Brand new process (was never under ckpt-control),

    // Initialize the log file
    Util::initializeLogFile(SharedData::getTmpDir());

    JTRACE("Root of processes tree");
    ProcessInfo::instance().isRootOfProcessTree = 1;
  }
}

static void
segFaultHandler(int sig, siginfo_t *siginfo, void *context)
{
  while (1) {
    sleep(1);
  }
}

static void
installSegFaultHandler()
{
  // install SIGSEGV handler
  struct sigaction act;

  memset(&act, 0, sizeof(act));
  act.sa_sigaction = segFaultHandler;
  act.sa_flags = SA_SIGINFO;
  JASSERT(sigaction(SIGSEGV, &act, NULL) == 0) (JASSERT_ERRNO);
}

/* This function is called at the very beginning of the DmtcpWorker constructor
 * to do some initialization work so that DMTCP can later use _real_XXX
 * functions reliably. Read the comment at the top of syscallsreal.c for more
 * details.
 */
// Initialize wrappers, etc.
extern "C" void
dmtcp_initialize()
{
  dmtcp_prepare_wrappers();
}

#ifdef DEBUG
// This is a test function which we can use to simulate foreign constructors
// that might be invoked before dmtcp_initialize_entry_point is called. We
// should be able to put arbitrary code here without worrying about the order of
// initialization. The constructor attribute priority is set to 100, so it will
// be called before dmtcp_initialize_entry_point.
// Note that 1-100 are reserved for implementation, but we are okay since this
// function is only enabled for debugging.
extern "C" void __attribute__((constructor(100)))
dmtcp_initialize_entry_point_test()
{
  cpu_set_t cpuset;
  pthread_t thread = pthread_self();
  pthread_getaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
}
#endif

// Initialize remaining components.
extern "C" void __attribute__((constructor(101)))
dmtcp_initialize_entry_point()
{
  if (dmtcp_initialized) {
    return;
  }

  dmtcp_initialized = true;

  dmtcp_initialize();

  initializeJalib();
  dmtcp_prepare_atfork();

  WorkerState::setCurrentState(WorkerState::RUNNING);

  PluginManager::initialize();

  prepareLogAndProcessdDataFromSerialFile();

  JTRACE("libdmtcp.so:  Running ")
    (jalib::Filesystem::GetProgramName()) (getenv("LD_PRELOAD"));

  if (getenv("DMTCP_SEGFAULT_HANDLER") != NULL) {
    // Install a segmentation fault handler (for debugging).
    installSegFaultHandler();
  }

  // This is called for side effect only.  Force this function to call
  // getenv(ENV_VAR_SIGCKPT) now and cache it to avoid getenv calls later.
  DmtcpWorker::determineCkptSignal();

  // Also cache programName and arguments
  string programName = jalib::Filesystem::GetProgramName();

  JASSERT(programName != "dmtcp_coordinator" &&
          programName != "dmtcp_launch" &&
          programName != "dmtcp_nocheckpoint" &&
          programName != "dmtcp_comand" &&
          programName != "dmtcp_restart" &&
          programName != "mtcp_restart" &&
          programName != "rsh" &&
          programName != "ssh")
    (programName).Text("This program should not be run under ckpt control");

  restoreUserLDPRELOAD();

  // Initialize data-structures related to motherofall thread.
  ThreadSync::initMotherOfAll();
  ThreadList::init();

  // In libdmtcp.so, notify this event for each plugin.
  PluginManager::eventHook(DMTCP_EVENT_INIT, NULL);

  // Initialize the timezone.
  // tzset() requires a malloc during initialization. We want to do it here to
  // avoid a later malloc during gmtime_r/localtime_r.
  tzset();

  // Create checkpoint-thread at the very end of the initialization process.
  ThreadList::createCkptThread();
}

void
DmtcpWorker::resetOnFork()
{
  exitInProgress = false;

  WorkerState::setCurrentState(WorkerState::RUNNING);

  ThreadSync::initMotherOfAll();
}

// Called after user main() by user thread or during exit() processing.
// With a high priority, we are hoping to be called first. This would allow us
// to set the exitInProgress flag for the ckpt thread to process later on.
// There is a potential race here. If the ckpt-thread suspends the user thread
// after the user thread has called exit() but before it is able to set
// `exitInProgress` to true, the ckpt thread will go about business as usual.
// This could be problematic if the exit() handlers had destroyed some
// resources.
// A potential solution is to not rely on user-destroyable resources. That way,
// we would have everything we need in order to perform a checkpoint. On
// restart, the process will then continue through the rest of the exit
// process.
void __attribute__((destructor(65535)))
dmtcp_finalize()
{
  /* If the destructor was called, we know that we are exiting
   * After setting this, the wrapper execution locks will be ignored.
   * FIXME:  A better solution is to add a ZOMBIE state to DmtcpWorker,
   *         instead of using a separate variable, _exitInProgress.
   */
  exitInProgress = true;
  PluginManager::eventHook(DMTCP_EVENT_EXIT, NULL);

//   ThreadSync::resetLocks();
  WorkerState::setCurrentState(WorkerState::UNKNOWN);

  JTRACE("Process exiting.");
}

void
DmtcpWorker::ckptThreadPerformExit()
{
  JTRACE("User thread is performing exit(). Ckpt thread exit()ing as well");

  // Ideally, we would like to perform pthread_exit(), but we are in the middle
  // of process cleanup (due to the user thread's exit() call) and as a result,
  // the static objects are being destroyed.  A call to pthread_exit() also
  // results in execution of various cleanup routines.  If the thread tries to
  // access any static objects during some cleanup routine, it will cause a
  // segfault.
  //
  // Our approach to loop here while we wait for the process to terminate.
  // This guarantees that we never access any static objects from this point
  // forward.
  while (1) {
    sleep(1);
  }
}

bool
DmtcpWorker::isExitInProgress()
{
  return exitInProgress;
}

void
DmtcpWorker::waitForPreSuspendMessage()
{
  SharedData::resetBarrierInfo();

  JTRACE("waiting for CHECKPOINT message");

  DmtcpMessage msg;
  CoordinatorAPI::recvMsgFromCoordinator(&msg);

  // Before validating message; make sure we are not exiting.
  if (exitInProgress) {
    ckptThreadPerformExit();
  }

  msg.assertValid();

  JASSERT(msg.type == DMT_DO_CHECKPOINT) (msg.type);

  // Coordinator sends some computation information along with the SUSPEND
  // message. Extracting that.
  SharedData::updateGeneration(msg.compGroup.computationGeneration());
  JASSERT(SharedData::getCompId() == msg.compGroup.upid())
    (SharedData::getCompId()) (msg.compGroup);

  ProcessInfo::instance().compGroup = SharedData::getCompId();
  exitAfterCkpt = msg.exitAfterCkpt;
}

void
DmtcpWorker::waitForCheckpointRequest()
{
  JTRACE("running");

  WorkerState::setCurrentState(WorkerState::RUNNING);

  PluginManager::eventHook(DMTCP_EVENT_RUNNING);

  waitForPreSuspendMessage();

  WorkerState::setCurrentState(WorkerState::PRESUSPEND);

  // Here we want to prevent any race with a user thread calling vfork(). In
  // vfork, we call acquireLocks(), but the child process later calls
  // resetLocks(). The parent ckpt-thread can then go ahead and acquire locks,
  // leading to memory corruption. These locks ensure that the ckpt-thread
  // doesn't get to acquireLocks until the vfork child has exec'd.
  // Further, we also want to prevent any overlap between an event-hook call
  // made here vs. an event-hook call made by the user thread in vfork().
  ThreadSync::presuspendEventHookLockLock();
  JTRACE("Processing pre-suspend barriers");
  PluginManager::eventHook(DMTCP_EVENT_PRESUSPEND);
  ThreadSync::presuspendEventHookLockUnlock();

  JTRACE("Preparing to acquire locks before DMT:SUSPEND barrier");
  ThreadSync::acquireLocks();

  JTRACE("Waiting for DMT:SUSPEND barrier");
  if (!CoordinatorAPI::waitForBarrier("DMT:SUSPEND")) {
    JASSERT(exitInProgress);
    ckptThreadPerformExit();
  }

  JTRACE("Starting checkpoint, suspending threads...");
}

// now user threads are stopped
void
DmtcpWorker::releaseLocks()
{
  JTRACE("Threads suspended");
  WorkerState::setCurrentState(WorkerState::SUSPENDED);

  ThreadSync::releaseLocks();

  if (exitInProgress) {
    // There is no reason to continue checkpointing this process as it would
    // simply die right after resume/restore.
    // Release user threads from ckpt signal handler.
    ThreadList::resumeThreads();
    ckptThreadPerformExit();
  }
}

// now user threads are stopped
void
DmtcpWorker::preCheckpoint()
{
  // Update generation, in case user callback calls dmtcp_get_generation().
  uint32_t computationGeneration =
    SharedData::getCompId()._computation_generation;
  ProcessInfo::instance().set_generation(computationGeneration);

  // initialize local number of peers on this node:
  //   sharedDataHeader->barrierInfo.numCkptPeers
  SharedData::prepareForCkpt();

  uint32_t numPeers;
  JTRACE("Waiting for DMT_CHECKPOINT barrier");
  CoordinatorAPI::waitForBarrier("DMT:CHECKPOINT", &numPeers);
  JTRACE("Computation information") (numPeers);

  // initialize global number of peers:
  ProcessInfo::instance().numPeers = numPeers;

  WorkerState::setCurrentState(WorkerState::CHECKPOINTING);
  PluginManager::eventHook(DMTCP_EVENT_PRECHECKPOINT);
}

void
DmtcpWorker::postCheckpoint()
{
  // Send ckpt maps to coordinator.
  string workerPath("/worker/" + ProcessInfo::instance().upidStr());

  jalib::JAllocArena *arenas;
  int numArenas = 0;
  {
    jalib::JAlloc::getAllocArenas(&arenas, &numArenas);
    ostringstream o;
    for (int i = 0; i < numArenas; i++) {
      if (arenas[i].startAddr != NULL) {
        o << std::hex << (uint64_t) arenas[i].startAddr
          << "-" << (uint64_t) arenas[i].endAddr
          << " " << ((uint64_t)arenas[i].endAddr - (uint64_t)arenas[i].startAddr) << "\n";
      }
    }

    kvdb::set(workerPath, "ProcSelfMaps_JAllocArenas", o.str());
  }

  {
    kvdb::set(
      workerPath,
      "ProcSelfMaps_Ckpt",
      procSelfMaps->getData());
  }

  WorkerState::setCurrentState(WorkerState::CHECKPOINTED);

  // TODO: Merge this barrier with the previous `sendCkptFilename` msg.
  JTRACE("Waiting for Write-Ckpt barrier");
  CoordinatorAPI::waitForBarrier("DMT:WriteCkpt");

  /* Now that temp checkpoint file is complete, rename it over old permanent
   * checkpoint file.  Uses rename() syscall, which doesn't change i-nodes.
   * So, gzip process can continue to write to file even after renaming.
   */
  JASSERT(rename(ProcessInfo::instance().getTempCkptFilename().c_str(),
                 ProcessInfo::instance().getCkptFilename().c_str()) == 0);

  CoordinatorAPI::sendCkptFilename();

  if (exitAfterCkpt) {
    JTRACE("Asked to exit after checkpoint. Exiting!");
    _exit(0);
  }

  PluginManager::eventHook(DMTCP_EVENT_RESUME);

  // Inform Coordinator of RUNNING state.
  WorkerState::setCurrentState(WorkerState::RUNNING);
  JTRACE("Informing coordinator of RUNNING status") (UniquePid::ThisProcess());
  CoordinatorAPI::sendMsgToCoordinator(DMT_WORKER_RESUMING);
}

void
DmtcpWorker::postRestart(double ckptReadTime)
{
  JTRACE("begin postRestart()");
  WorkerState::setCurrentState(WorkerState::RESTARTING);

  JTRACE("Waiting for Restart barrier");
  CoordinatorAPI::waitForBarrier("DMT:Restart");

  PluginManager::eventHook(DMTCP_EVENT_RESTART);

  JTRACE("got resume message after restart");

  {
    // Send ckpt maps to coordinator.
    string workerPath("/worker/" + ProcessInfo::instance().upidStr());
    ProcSelfMaps procSelfMaps;
    kvdb::set(
      workerPath,
      "ProcSelfMaps_Rst",
      procSelfMaps.getData());
  }

  // Inform Coordinator of RUNNING state.
  WorkerState::setCurrentState(WorkerState::RUNNING);
  JTRACE("Informing coordinator of RUNNING status") (UniquePid::ThisProcess());
  CoordinatorAPI::sendMsgToCoordinator(DMT_WORKER_RESUMING);
}
