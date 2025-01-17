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

#include <stdlib.h>
#include <sys/time.h>
#include <sys/resource.h>
#include "dmtcpworker.h"
#include "dmtcpmessagetypes.h"
#include "workerstate.h"
#include "mtcpinterface.h"
#include "threadsync.h"
#include "processinfo.h"
#include "syscallwrappers.h"
#include "util.h"
#include "syslogwrappers.h"
#include "coordinatorapi.h"
#include "shareddata.h"
#include "threadlist.h"
#include  "../jalib/jsocket.h"
#include  "../jalib/jfilesystem.h"
#include  "../jalib/jconvert.h"
#include  "../jalib/jbuffer.h"

using namespace dmtcp;

LIB_PRIVATE void pthread_atfork_prepare();
LIB_PRIVATE void pthread_atfork_parent();
LIB_PRIVATE void pthread_atfork_child();

void pidVirt_pthread_atfork_child() __attribute__((weak));

/* This is defined by newer gcc version unique for each module.  */
extern void *__dso_handle __attribute__ ((__weak__,
					  __visibility__ ("hidden")));

EXTERNC int __register_atfork(void (*prepare)(void), void (*parent)(void),
                              void (*child)(void), void *dso_handle);

EXTERNC void *ibv_get_device_list(void *) __attribute__((weak));

/* The following instance of the DmtcpWorker is just to trigger the constructor
 * to allow us to hijack the process
 */
DmtcpWorker DmtcpWorker::theInstance;
bool DmtcpWorker::_exitInProgress = false;

/* NOTE:  Please keep this function in sync with its copy at:
 *   dmtcp_nocheckpoint.cpp:restoreUserLDPRELOAD()
 */
void restoreUserLDPRELOAD()
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
  //  when the user application makes an exec call:
  //   If anybody calls our execwrapper, we will reset LD_PRELOAD then.
  //   EXCEPTION:  If anybody directly calls _real_execve with env arg of NULL,
  //   they will not be part of DMTCP computation.
  // This has the advantage that our value of LD_PRELOAD will always come
  //   before any paths set by user application.
  // Also, bash likes to keep its own envp, but we will interact with bash only
  //   within the exec wrapper.
  // NOTE:  If the user called exec("ssh ..."), we currently catch this in
  //   src/pugin/dmtcp_ssh.cp:main(), and edit this into
  //   exec("dmtcp_launch ... dmtcp_ssh ..."), and re-execute.
  // NOTE:  If the user called exec("dmtcp_nocheckpoint ..."), we will
  //   reset LD_PRELOAD back to ENV_VAR_ORIG_LD_PRELOAD in dmtcp_nocheckpoint
  char *preload = getenv("LD_PRELOAD");
  char *userPreload = getenv(ENV_VAR_ORIG_LD_PRELOAD);
  JASSERT(userPreload == NULL || strlen(userPreload) <= strlen(preload));
  // Destructively modify environment variable "LD_PRELOAD" in place:
  preload[0] = '\0';
  if (userPreload == NULL) {
    //_dmtcp_unsetenv("LD_PRELOAD");
  } else {
    strcat(preload, userPreload);
    //setenv("LD_PRELOAD", userPreload, 1);
  }
  JLOG(DMTCP)("LD_PRELOAD") (preload) (userPreload) (getenv(ENV_VAR_HIJACK_LIBS))
    (getenv(ENV_VAR_HIJACK_LIBS_M32)) (getenv("LD_PRELOAD"));
}

// This should be visible to library only.  DmtcpWorker will call
//   this to initialize tmp (ckpt signal) at startup time.  This avoids
//   any later calls to getenv(), at which time the user app may have
//   a wrapper around getenv, modified environ, or other tricks.
//   (Matlab needs this or else it segfaults on restart, and bash plays
//   similar tricks with maintaining its own environment.)
// Used in mtcpinterface.cpp and signalwrappers.cpp.
// FIXME: DO we still want it to be library visible only?
//__attribute__ ((visibility ("hidden")))
int DmtcpWorker::determineCkptSignal()
{
  int sig = CKPT_SIGNAL;
  char* endp = NULL;
  static const char* tmp = getenv(ENV_VAR_SIGCKPT);
  if (tmp != NULL) {
      sig = strtol(tmp, &endp, 0);
      if ((errno != 0) || (tmp == endp))
        sig = CKPT_SIGNAL;
      if (sig < 1 || sig > 31)
        sig = CKPT_SIGNAL;
  }
  return sig;
}

/* This function is called at the very beginning of the DmtcpWorker constructor
 * to do some initialization work so that DMTCP can later use _real_XXX
 * functions reliably. Read the comment at the top of syscallsreal.c for more
 * details.
 */
static void dmtcp_prepare_atfork(void)
{
  /* Register pidVirt_pthread_atfork_child() as the first post-fork handler
   * for the child process. This needs to be the first function that is
   * called by libc:fork() after the child process is created.
   *
   * pthread_atfork_child() needs to be the second post-fork handler for the
   * child process.
   *
   * Some dmtcp plugin might also call pthread_atfork and so we call it right
   * here before initializing the wrappers.
   *
   * NOTE: If this doesn't work and someone is able to call pthread_atfork
   * before this call, we might want to install a pthread_atfork() wrapper.
   */

  /* If we use pthread_atfork here, it fails for Ubuntu 14.04 on ARM.
   * To fix it, we use __register_atfork and use the __dso_handle provided by
   * the gcc compiler.
   */
  JASSERT(__register_atfork(NULL, NULL,
                         pidVirt_pthread_atfork_child,
                         __dso_handle) == 0);

  JASSERT(pthread_atfork(pthread_atfork_prepare,
                         pthread_atfork_parent,
                         pthread_atfork_child) == 0);
}

static string getLogFilePath()
{
#ifdef LOGGING
  ostringstream o;
  o << "/proc/self/fd/" << PROTECTED_JASSERTLOG_FD;
  return jalib::Filesystem::ResolveSymlink(o.str());

#else // ifdef LOGGING
  return "";
#endif // ifdef LOGGING
}

static void writeCurrentLogFileNameToPrevLogFile(string& path)
{
#ifdef LOGGING
  ostringstream o;
  o << "========================================\n"
    << "This process exec()'d into a new program\n"
    << "Program Name: " << jalib::Filesystem::GetProgramName() << "\n"
    << "New JAssertLog Path: " << getLogFilePath() << "\n"
    << "========================================\n";

  int fd = open(path.c_str(), O_WRONLY | O_APPEND, 0);
  if (fd != -1) {
    Util::writeAll(fd, o.str().c_str(), o.str().length());
  }
  _real_close(fd);
#endif // ifdef LOGGING
}

static void prepareLogAndProcessdDataFromSerialFile()
{

  if (Util::isValidFd(PROTECTED_LIFEBOAT_FD)) {
    // This process was under ckpt-control and exec()'d into a new program.
    // Find out path of previous log file so that later, we can write the name
    // of the new log file into that one.
    string prevLogFilePath = getLogFilePath();

    jalib::JBinarySerializeReaderRaw rd ("", PROTECTED_LIFEBOAT_FD);
    rd.rewind();
    UniquePid::serialize (rd);
    Util::initializeLogFile(SharedData::getTmpDir(), "", prevLogFilePath);

    writeCurrentLogFileNameToPrevLogFile(prevLogFilePath);

    DmtcpEventData_t edata;
    edata.serializerInfo.fd = PROTECTED_LIFEBOAT_FD;
    DmtcpWorker::eventHook(DMTCP_EVENT_POST_EXEC, &edata);
    _real_close(PROTECTED_LIFEBOAT_FD);
  } else {
    // Brand new process (was never under ckpt-control),
    // Initialize the log file
    Util::initializeLogFile(SharedData::getTmpDir());

    JLOG(DMTCP)("Root of processes tree");
    ProcessInfo::instance().setRootOfProcessTree();
  }
}

static void segFaultHandler(int sig, siginfo_t* siginfo, void* context)
{
  while (1) sleep(1);
}

static void installSegFaultHandler()
{
  // install SIGSEGV handler
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_sigaction = segFaultHandler;
  act.sa_flags = SA_SIGINFO;
  JASSERT (sigaction(SIGSEGV, &act, NULL) == 0) (JASSERT_ERRNO);
}

static bool inDmtcpWorker = false;
// A weak symbol will have default value 0 (same as false)
extern "C" int dmtcpInMalloc __attribute__((weak));

//called before user main() to initialize DMTCP
extern "C" void dmtcp_initialize()
{
  static bool initialized = false;
  // FIXME:
  // PR #742: DMTCP malloc wrapper called prematurely, causing DMTCP
  //    to initialize prematuruely.  'dmtcpInMalloc' test fixes that.
  //    But apparently the 'emacs' test causes a different DMTCP
  //    wrapper to be called prematurely and initialize DMTCP prematurely.
  //    We should discover all such cases of premature DMTCP initialization,
  //    and guard against them.  It seems that in github Travis
  //    in July, 2019, the emacs test fails when
  //    '(! inDmtcpWorker && dmtcpInMalloc)' is tested for.
  //    Presumably, there is another function being called in the emacs test
  //    besides DMTCP's malloc(), and CentOS 7.5 reproduces this failure
  //    for 'emacs -nw' (emacs-nox not found).
  //    Similarly, the second restart on vim is failing.
  //  It seems to be a corner case that can be fixed in the next release.
  //    For now, both the 'vim' and 'emacs -nw' tests are commented out
  //    in test/autotest.py
  if ( initialized || (! inDmtcpWorker && dmtcpInMalloc) ) {
    return;
  }
  initialized = true;
  WorkerState::setCurrentState(WorkerState::UNKNOWN);
  dmtcp_prepare_wrappers();
  initializeJalib();
  dmtcp_prepare_atfork();
  prepareLogAndProcessdDataFromSerialFile();

  JLOG(DMTCP)("libdmtcp.so:  Running ")
    (jalib::Filesystem::GetProgramName()) (getenv ("LD_PRELOAD"));

  if (getenv("DMTCP_SEGFAULT_HANDLER") != NULL) {
    // Install a segmentation fault handler (for debugging).
    installSegFaultHandler();
  }

  //This is called for side effect only.  Force this function to call
  // getenv(ENV_VAR_SIGCKPT) now and cache it to avoid getenv calls later.
  DmtcpWorker::determineCkptSignal();

  // Also cache programName and arguments
  string programName = jalib::Filesystem::GetProgramName();

  JASSERT(programName != "dmtcp_coordinator"  &&
          programName != "dmtcp_launch"   &&
          programName != "dmtcp_nocheckpoint" &&
          programName != "dmtcp_comand"       &&
          programName != "dmtcp_restart"      &&
          programName != "mtcp_restart"       &&
          programName != "rsh"                &&
          programName != "ssh")
    (programName) .Text("This program should not be run under ckpt control");

  ProcessInfo::instance().calculateArgvAndEnvSize();
  restoreUserLDPRELOAD();

  WorkerState::setCurrentState (WorkerState::RUNNING);

  if (ibv_get_device_list && !dmtcp_infiniband_enabled) {
    JNOTE("\n\n*** InfiniBand library detected."
          "  Please use dmtcp_launch --ib ***\n");
  }

  // In libdmtcp.so, notify this event for each plugin.
  DmtcpWorker::eventHook(DMTCP_EVENT_INIT, NULL);

  initializeMtcpEngine();
  DmtcpWorker::informCoordinatorOfRUNNINGState();
}

DmtcpWorker::DmtcpWorker()
{
  inDmtcpWorker = true;
  dmtcp_initialize();
  inDmtcpWorker = false;
}

void DmtcpWorker::resetOnFork()
{
  eventHook(DMTCP_EVENT_ATFORK_CHILD, NULL);

  cleanupWorker();

  /* If parent process had file connections and it fork()'d a child
   * process, the child process would consider the file connections as
   * pre-existing and hence wouldn't restore them. This is fixed by making sure
   * that when a child process is forked, it shouldn't be looking for
   * pre-existing connections because the parent has already done that.
   *
   * So, here while creating the instance, we do not want to execute everything
   * in the constructor since it's not relevant. All we need to call is
   * connectToCoordinatorWithHandshake() and initializeMtcpEngine().
   */
  //new ( &theInstance ) DmtcpWorker ( false );

  DmtcpWorker::_exitInProgress = false;

  WorkerState::setCurrentState ( WorkerState::RUNNING );

}

void DmtcpWorker::cleanupWorker()
{
  ThreadSync::resetLocks();
  WorkerState::setCurrentState(WorkerState::UNKNOWN);
  JLOG(DMTCP)("disconnecting from dmtcp coordinator");
}

void DmtcpWorker::interruptCkpthread()
{
  if (ThreadSync::destroyDmtcpWorkerLockTryLock() == EBUSY) {
    ThreadList::killCkpthread();
    ThreadSync::destroyDmtcpWorkerLockLock();
  }
}

//called after user main()
DmtcpWorker::~DmtcpWorker()
{
  /* If the destructor was called, we know that we are exiting
   * After setting this, the wrapper execution locks will be ignored.
   * FIXME:  A better solution is to add a ZOMBIE state to DmtcpWorker,
   *         instead of using a separate variable, _exitInProgress.
   */
  setExitInProgress();
  eventHook(DMTCP_EVENT_EXIT, NULL);
  interruptCkpthread();
  cleanupWorker();
}

static void ckptThreadPerformExit()
{
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
  while (1) sleep(1);
}

void DmtcpWorker::waitForCoordinatorMsg(string msgStr,
                                               DmtcpMessageType type)
{
  if (dmtcp_no_coordinator()) {
    if (type == DMT_DO_SUSPEND) {
      string shmFile = jalib::Filesystem::GetDeviceName(PROTECTED_SHM_FD);
      JASSERT(!shmFile.empty());
      unlink(shmFile.c_str());
      CoordinatorAPI::instance().waitForCheckpointCommand();
      ProcessInfo::instance().numPeers(1);
      ProcessInfo::instance().compGroup(SharedData::getCompId());
    }
    return;
  }

  if (type == DMT_DO_SUSPEND) {
    if (ThreadSync::destroyDmtcpWorkerLockTryLock() != 0) {
      JLOG(DMTCP)("User thread is performing exit()."
               " ckpt thread exit()ing as well");
      ckptThreadPerformExit();
    }
    if (exitInProgress()) {
      ThreadSync::destroyDmtcpWorkerLockUnlock();
      ckptThreadPerformExit();
    }
  }

  DmtcpMessage msg;
  char *replyData = NULL;

  if (type == DMT_DO_SUSPEND) {
    // Make a dummy syscall to inform superior of our status before we go into
    // select. If ptrace is disabled, this call has no significant effect.
    _real_syscall(DMTCP_FAKE_SYSCALL);
  } else {
    msg.type = DMT_OK;
    msg.state = WorkerState::currentState();
    CoordinatorAPI::instance().sendMsgToCoordinator(msg);
  }

  JLOG(DMTCP)("waiting for " + msgStr + " message");
  do {
    CoordinatorAPI::instance().recvMsgFromCoordinator(&msg, (void **)&replyData);
    if (type == DMT_DO_SUSPEND && exitInProgress()) {
      ThreadSync::destroyDmtcpWorkerLockUnlock();
      ckptThreadPerformExit();
    }

    msg.assertValid();
    if (msg.type == DMT_KILL_PEER) {
      JLOG(DMTCP)("Received KILL message from coordinator, exiting");
      _exit (0);
    }
    if (msg.type == DMT_UPDATE_LOGGING) {
      SharedData::setLogMask(msg.logMask);
    } else {
      break;
    }
  } while (1);

  JASSERT(msg.type == type) (msg.type) (type);

  // Coordinator sends some computation information along with the SUSPEND
  // message. Extracting that.
  if (type == DMT_DO_SUSPEND) {
    SharedData::updateGeneration(msg.compGroup.computationGeneration());
    JASSERT(SharedData::getCompId() == msg.compGroup.upid())
      (SharedData::getCompId()) (msg.compGroup);
    // Coordinator sends the global checkpoint dir.
    if (msg.extraBytes > 0) {
      ProcessInfo::instance().setCkptDir(replyData);
      JALLOC_HELPER_FREE(replyData);
    }
  } else if (type == DMT_DO_FD_LEADER_ELECTION) {
    JLOG(DMTCP)("Computation information") (msg.compGroup) (msg.numPeers);
    ProcessInfo::instance().compGroup(msg.compGroup);
    ProcessInfo::instance().numPeers(msg.numPeers);
  }
}

void DmtcpWorker::informCoordinatorOfRUNNINGState()
{
  DmtcpMessage msg;

  JASSERT(WorkerState::currentState() == WorkerState::RUNNING);

  msg.type = DMT_OK;
  msg.state = WorkerState::currentState();
  CoordinatorAPI::instance().sendMsgToCoordinator(msg);
}

void DmtcpWorker::waitForStage1Suspend()
{
  JLOG(DMTCP)("running");

  WorkerState::setCurrentState (WorkerState::RUNNING);

  waitForCoordinatorMsg ("SUSPEND", DMT_DO_SUSPEND);

  JLOG(DMTCP)("got SUSPEND message, preparing to acquire all ThreadSync locks");
  ThreadSync::acquireLocks();

  JLOG(DMTCP)("Starting checkpoint, suspending...");
}

void DmtcpWorker::waitForStage2Checkpoint()
{
  WorkerState::setCurrentState (WorkerState::SUSPENDED);
  JLOG(DMTCP)("suspended");

  if (exitInProgress()) {
    ThreadSync::destroyDmtcpWorkerLockUnlock();
    ckptThreadPerformExit();
  }
  ThreadSync::destroyDmtcpWorkerLockUnlock();

  ThreadSync::releaseLocks();

  // Prepare SharedData for ckpt.
  SharedData::prepareForCkpt();

  eventHook(DMTCP_EVENT_THREADS_SUSPEND, NULL);

  waitForCoordinatorMsg ("FD_LEADER_ELECTION", DMT_DO_FD_LEADER_ELECTION);

  eventHook(DMTCP_EVENT_LEADER_ELECTION, NULL);

  WorkerState::setCurrentState (WorkerState::FD_LEADER_ELECTION);

  waitForCoordinatorMsg ("PRE_CKPT_NAME_SERVICE_DATA_REGISTER", DMT_DO_PRE_CKPT_NAME_SERVICE_DATA_REGISTER);

  eventHook(DMTCP_EVENT_PRE_CKPT_NAME_SERVICE_DATA_REGISTER, NULL);

  WorkerState::setCurrentState (WorkerState::PRE_CKPT_NAME_SERVICE_DATA_REGISTER);

  waitForCoordinatorMsg ("PRE_CKPT_NAME_SERVICE_DATA_QUERY", DMT_DO_PRE_CKPT_NAME_SERVICE_DATA_QUERY);

  eventHook(DMTCP_EVENT_PRE_CKPT_NAME_SERVICE_DATA_QUERY, NULL);

  WorkerState::setCurrentState (WorkerState::PRE_CKPT_NAME_SERVICE_DATA_QUERY);

  waitForCoordinatorMsg ("DRAIN", DMT_DO_DRAIN);

  WorkerState::setCurrentState (WorkerState::DRAINED);

  eventHook(DMTCP_EVENT_DRAIN, NULL);

  waitForCoordinatorMsg ("CHECKPOINT", DMT_DO_CHECKPOINT);
  JLOG(DMTCP)("got checkpoint message");

  eventHook(DMTCP_EVENT_WRITE_CKPT, NULL);

  SharedData::writeCkpt();
}

void DmtcpWorker::waitForStage3Refill(bool isRestart)
{
  DmtcpEventData_t edata;
  JLOG(DMTCP)("checkpointed");

  WorkerState::setCurrentState (WorkerState::CHECKPOINTED);

#ifdef COORD_NAMESERVICE
  waitForCoordinatorMsg("REGISTER_NAME_SERVICE_DATA",
                          DMT_DO_REGISTER_NAME_SERVICE_DATA);
  edata.nameserviceInfo.isRestart = isRestart;
  eventHook(DMTCP_EVENT_REGISTER_NAME_SERVICE_DATA, &edata);
  JLOG(DMTCP)("Key Value Pairs registered with the coordinator");
  WorkerState::setCurrentState(WorkerState::NAME_SERVICE_DATA_REGISTERED);

  waitForCoordinatorMsg("SEND_QUERIES", DMT_DO_SEND_QUERIES);
  eventHook(DMTCP_EVENT_SEND_QUERIES, &edata);
  JLOG(DMTCP)("Queries sent to the coordinator");
  WorkerState::setCurrentState(WorkerState::DONE_QUERYING);
#endif

  waitForCoordinatorMsg ("REFILL", DMT_DO_REFILL);

  edata.refillInfo.isRestart = isRestart;
  DmtcpWorker::eventHook(DMTCP_EVENT_REFILL, &edata);
}

void DmtcpWorker::waitForStage4Resume(bool isRestart)
{
  JLOG(DMTCP)("refilled");
  WorkerState::setCurrentState (WorkerState::REFILLED);
  waitForCoordinatorMsg ("RESUME", DMT_DO_RESUME);
  JLOG(DMTCP)("got resume message");
  DmtcpEventData_t edata;
  edata.resumeInfo.isRestart = isRestart;
  DmtcpWorker::eventHook(DMTCP_EVENT_THREADS_RESUME, &edata);
}

void dmtcp_CoordinatorAPI_EventHook(DmtcpEvent_t event, DmtcpEventData_t *data);
void dmtcp_ProcessInfo_EventHook(DmtcpEvent_t event, DmtcpEventData_t *data);
void dmtcp_UniquePid_EventHook(DmtcpEvent_t event, DmtcpEventData_t *data);
void dmtcp_Terminal_EventHook(DmtcpEvent_t event, DmtcpEventData_t *data);
void dmtcp_Syslog_EventHook(DmtcpEvent_t event, DmtcpEventData_t *data);
void dmtcp_Alarm_EventHook(DmtcpEvent_t event, DmtcpEventData_t *data);

void DmtcpWorker::eventHook(DmtcpEvent_t event, DmtcpEventData_t *data)
{
  static jalib::JBuffer *buf = NULL;
  if (buf == NULL) {
    // Technically, this is a memory leak, but buf is static and so it happens
    // only once.
    buf = new jalib::JBuffer(0); // To force linkage of jbuffer.cpp
  }
  dmtcp_Syslog_EventHook(event, data);
  dmtcp_Terminal_EventHook(event, data);
  dmtcp_UniquePid_EventHook(event, data);
  dmtcp_CoordinatorAPI_EventHook(event, data);
  dmtcp_ProcessInfo_EventHook(event, data);
  dmtcp_Alarm_EventHook(event, data);
  if (dmtcp_event_hook != NULL) {
    dmtcp_event_hook(event, data);
  }
}
