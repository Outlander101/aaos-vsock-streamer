/*
 * VsockUtils.h - VSOCK utility implementations for screentransfer
 *
 * Provides helper functions for:
 *   - Creating listening sockets
 *   - Accepting connections
 *   - Reading/writing with proper error handling
 *
 */

#include "VsockUtils.h"

#include <errno.h>
#include <linux/vm_sockets.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define LOG_TAG "VsockUtils"
#include <utils/Log.h>

namespace screentransfer {

int VsockUtils::createVsockListener(uint32_t port) {
  int listenFd = socket(AF_VSOCK, SOCK_STREAM, 0);
  if (listenFd < 0) {
    ALOGE("Failed to create VSOCK socket: %s", strerror(errno));
    return -1;
  }

  // Allow socket reuse for quick restarts
  int optval = 1;
  setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

  // Bind to VMADDR_CID_ANY (accept from any CID)
  struct sockaddr_vm addr;
  memset(&addr, 0, sizeof(addr));
  addr.svm_family = AF_VSOCK;
  addr.svm_cid = VMADDR_CID_ANY;
  addr.svm_port = port;

  if (bind(listenFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    ALOGE("Failed to bind VSOCK port %u: %s", port, strerror(errno));
    close(listenFd);
    return -1;
  }

  // Start listening (backlog = 1)
  if (listen(listenFd, 1) < 0) {
    ALOGE("Failed to listen on VSOCK port %u: %s", port, strerror(errno));
    close(listenFd);
    return -1;
  }

  return listenFd;
}

int VsockUtils::acceptConnection(int listenFd) {
  struct sockaddr_vm remoteAddr;
  socklen_t len = sizeof(remoteAddr);

  int clientFd = accept(listenFd, (struct sockaddr*)&remoteAddr, &len);
  if (clientFd < 0) {
    if (errno != EINTR) {
      ALOGE("accept() failed: %s", strerror(errno));
    }
    return -1;
  }

  ALOGI("Accepted connection from CID %u", remoteAddr.svm_cid);
  return clientFd;
}

bool VsockUtils::writeFully(int fd, const void* buf, size_t size) {
  const uint8_t* ptr = static_cast<const uint8_t*>(buf);
  size_t remaining = size;

  while (remaining > 0) {
    ssize_t written = write(fd, ptr, remaining);
    if (written < 0) {
      if (errno == EINTR) continue;  // Retry on interrupt
      ALOGE("write() failed: %s", strerror(errno));
      return false;
    }
    if (written == 0) {
      ALOGW("write() returned 0 (connection closed)");
      return false;
    }
    ptr += written;
    remaining -= written;
  }
  return true;
}

bool VsockUtils::readFully(int fd, void* buf, size_t size) {
  uint8_t* ptr = static_cast<uint8_t*>(buf);
  size_t remaining = size;

  while (remaining > 0) {
    ssize_t bytesRead = read(fd, ptr, remaining);
    if (bytesRead < 0) {
      if (errno == EINTR) continue;  // Retry on interrupt
      ALOGE("read() failed: %s", strerror(errno));
      return false;
    }
    if (bytesRead == 0) {
      ALOGW("read() returned 0 (connection closed)");
      return false;
    }
    ptr += bytesRead;
    remaining -= bytesRead;
  }
  return true;
}

}  // namespace screentransfer