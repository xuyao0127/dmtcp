#include <fcntl.h>
#include <limits.h>  // for PATH_MAX
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include "protectedfds.h"

#include "dmtcp.h"
#include "jassert.h"
#include "jfilesystem.h"
#include "pluginmanager.h"
#include "threadsync.h"
#include "util.h"
#include "syscallwrappers.h"

namespace dmtcp
{

static bool
isValidAddress(const char *path)
{
  struct stat buf;
  int retval = __xstat(0, path, &buf);
  if (retval == -1 && errno == EFAULT) {
    return false;
  }

  return true;
}

static void
processOpenFd(int fd, const char *path, int flags, mode_t mode)
{
  if (!dmtcp_is_running_state()) {
    return;
  }

  DmtcpEventData_t data;
  data.openFd.fd = fd;
  data.openFd.path = path;
  data.openFd.flags = flags;
  data.openFd.mode = mode;

  PluginManager::eventHook(DMTCP_EVENT_OPEN_FD, &data);
}

static const char*
virtualToRealPath(const char *virtualPath, char *realPath)
{
  // We want to first validate path to make sure it's in our address space.
  // We do this using a preliminary call to _real_xstat(). If path or buf is
  // invalid, return with calling translation functions. Otherwise we proceed to
  // translate the path.

  if (!isValidAddress(virtualPath)) {
    return virtualPath;
  }

  strncpy(realPath, virtualPath, PATH_MAX);
  realPath[PATH_MAX - 1] = 0;

  DmtcpEventData_t data;
  data.virtualToRealPath.path = realPath;

  PluginManager::eventHook(DMTCP_EVENT_VIRTUAL_TO_REAL_PATH, &data);

  return realPath;
}

extern "C" char *
realToVirtualPath(char *path)
{
  // No need to validate valid address for path. The address returned by the
  // underlying syscall is valid on a successful return.

  DmtcpEventData_t data;
  data.realToVirtualPath.path = path;

  PluginManager::eventHook(DMTCP_EVENT_REAL_TO_VIRTUAL_PATH, &data);

  return path;
}

static int
dmtcp_openat(int dirfd, const char *path, int flags, mode_t mode)
{
  WrapperLock wrapperLock;

  char realPath[PATH_MAX] = { 0 };

  int fd = _real_openat(dirfd, virtualToRealPath(path, realPath), flags, mode);

  if (fd != -1) {
    processOpenFd(fd, path, flags, mode);
  }

  return fd;
}

extern "C" int
__wrap_open(const char *path, int flags, ...)
{
  printf("Entering __wrap_open\n");
  mode_t mode = 0;

  if (flags & O_CREAT) {
    va_list arg;
    va_start(arg, flags);
    mode = va_arg(arg, int);
    va_end(arg);
  }

  printf("Calling __real_open\n");
  return dmtcp_openat(AT_FDCWD, path, flags, mode);
}

}
