
#include <unistd.h>
#include <sys/syscall.h>

#include "util.h"
#include "protectedfds.h"
#include "jfilesystem.h"
#include "socketconnection.h"
#include "socketconnlist.h"
#include "kernelbufferdrainer.h"
#include "connectionrewirer.h"

using namespace dmtcp;
static bool _hasIPv4Sock = false;
static bool _hasIPv6Sock = false;
static bool _hasUNIXSock = false;

void dmtcp_SocketConnList_EventHook(DmtcpEvent_t event, DmtcpEventData_t *data)
{
  SocketConnList::instance().eventHook(event, data);
}

void dmtcp_SocketConn_ProcessFdEvent(int event, int arg1, int arg2)
{
  if (event == SYS_close) {
    SocketConnList::instance().processClose(arg1);
  } else if (event == SYS_dup) {
    SocketConnList::instance().processDup(arg1, arg2);
  } else {
    JASSERT(false);
  }
}

static SocketConnList *socketConnList = NULL;
SocketConnList& SocketConnList::instance()
{
  if (socketConnList == NULL) {
    socketConnList = new SocketConnList();
  }
  return *socketConnList;
}

void SocketConnList::preCkptRegisterNSData()
{
  for (iterator i = begin(); i != end(); ++i) {
    Connection *con = i->second;
    /* NOTE: We need to explicitly call checkLocking() here because
     * _hasLock is set only in this function.
     */
    con->checkLocking();
    if (con->hasLock() && con->conType() == Connection::TCP) {
      ((TcpConnection *)con)->sendPeerInformation();
    }
  }
}

void SocketConnList::preCkptSendQueries()
{
  for (iterator i = begin(); i != end(); ++i) {
    Connection *con = i->second;
    if (con->hasLock() && con->conType() == Connection::TCP) {
      ((TcpConnection *)con)->recvPeerInformation();
    }
  }
}

void SocketConnList::drain()
{
  // First, let all the Connection prepare for drain
  ConnectionList::drain();

  //this will block until draining is complete
  KernelBufferDrainer::instance().monitorSockets(DRAINER_CHECK_FREQ);
  //handle disconnected sockets
  const map<ConnectionIdentifier, vector<char> >& discn =
    KernelBufferDrainer::instance().getDisconnectedSockets();
  map<ConnectionIdentifier, vector<char> >::const_iterator it;
  for (it = discn.begin(); it != discn.end(); it++) {
    const ConnectionIdentifier& id = it->first;
    TcpConnection *con =
      (TcpConnection*) SocketConnList::instance().getConnection(id);
    JLOG(SOCKET)("recreating disconnected socket") (id);

    //reading from the socket, and taking the error, resulted in an
    //implicit close().
    //we will create a new, broken socket that is not closed
    con->onError();
  }
  dmtcp_update_max_required_fd(getMaxFd());
}

void SocketConnList::preCkpt()
{
  //handshake is done after one barrier after drain
  JLOG(SOCKET)("beginning handshakes");
  DmtcpUniqueProcessId coordId = dmtcp_get_coord_id();
  //must send first to avoid deadlock
  //we are relying on OS buffers holding our message without blocking
  for (iterator i = begin(); i != end(); ++i) {
    Connection *con = i->second;
    if (con->hasLock() && con->conType() == Connection::TCP) {
      ((TcpConnection*)con)->doSendHandshakes(coordId);
    }
  }

  //now receive
  for (iterator i = begin(); i != end(); ++i) {
    Connection *con = i->second;
    if (con->hasLock() && con->conType() == Connection::TCP) {
      ((TcpConnection*)con)->doRecvHandshakes(coordId);
    }
  }
  JLOG(SOCKET)("handshaking done");
  _hasIPv4Sock = _hasIPv6Sock = _hasUNIXSock = false;
  // Now check if we have IPv4, IPv6, or UNIX domain sockets to restore.
  for (iterator i = begin(); i != end(); ++i) {
    Connection *con = i->second;
    if (con->hasLock() && con->conType() == Connection::TCP) {
      int domain = ((TcpConnection*)con)->sockDomain();
      if (domain == AF_INET) {
        _hasIPv4Sock = true;
      } else if (domain == AF_INET6) {
        _hasIPv6Sock = true;
      } else if (domain == AF_UNIX) {
        _hasUNIXSock = true;
      }
    }
  }
}

void SocketConnList::postRestart()
{
  ConnectionRewirer::instance().openRestoreSocket(_hasIPv4Sock, _hasIPv6Sock,
                                                  _hasUNIXSock);
  ConnectionList::postRestart();
}

void SocketConnList::registerNSData(bool isRestart)
{
  if (isRestart) {
    ConnectionRewirer::instance().registerNSData();
  }
  ConnectionList::registerNSData(isRestart);
}

void SocketConnList::sendQueries(bool isRestart)
{
  if (isRestart) {
    ConnectionRewirer::instance().sendQueries();
    ConnectionRewirer::instance().doReconnect();
    ConnectionRewirer::destroy();
  }
  ConnectionList::sendQueries(isRestart);
}

void SocketConnList::refill(bool isRestart)
{
  KernelBufferDrainer::instance().refillAllSockets();
  ConnectionList::refill(isRestart);
}

void SocketConnList::scanForPreExisting()
{
  // TODO: This is a hack when SLURM + MPI are used:
  // when we use command
  // srun/ibrun dmtcp_launch a.out
  // inside the SLURM submission script, the MPI launching
  // process will not run under the control of DMTCP. Instead,
  // only the computing processes are. The launching process
  // will create some sockets, and then create the computing
  // processes. Hence the sockets are shared among the created
  // processes at the time when dmtcp_launch is launched. DMTCP
  // will treat these sockets as pre-existing sockets instead of
  // shared sockets.
  //
  // In the future, we should generalize the processing of
  // pre-existing fds. For example, at checkpoint time, determine
  // which sockets are shared, regardless of whether they are
  // pre-existing or not. This can be done by adding an extra round
  // of leader election.

  if (getenv("SLURM_JOBID") || (getenv("SLURM_JOB_ID"))) {
    return;
  }

  // FIXME: Detect stdin/out/err fds to detect duplicates.
  vector<int> fds = jalib::Filesystem::ListOpenFds();
  for (size_t i = 0; i < fds.size(); ++i) {
    int fd = fds[i];
    if (!Util::isValidFd(fd)) continue;
    if (dmtcp_is_protected_fd(fd)) continue;

    string device = jalib::Filesystem::GetDeviceName(fd);

    JLOG(SOCKET)("scanning pre-existing device") (fd) (device);
    if (device == jalib::Filesystem::GetControllingTerm()) {
    } else if(dmtcp_is_bq_file && dmtcp_is_bq_file(device.c_str())) {
    } else if( fd <= 2 ){
    } else if (Util::strStartsWith(device, "/")) {
    } else {
      JNOTE("found pre-existing socket... will not be restored")
        (fd) (device);
      TcpConnection* con = new TcpConnection(0, 0, 0);
      con->markPreExisting();
      add(fd, con);
    }
  }
}

Connection *SocketConnList::createDummyConnection(int type)
{
  if (type == Connection::TCP) {
    return new TcpConnection();
  } else if (type == Connection::RAW){
    return new RawSocketConnection();
  }
  return NULL;
}
