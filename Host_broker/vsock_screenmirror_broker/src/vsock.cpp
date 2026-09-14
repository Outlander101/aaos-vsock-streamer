
#include "vsock.hpp"

#include <fcntl.h>
#include <linux/vm_sockets.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "log.hpp"
int Vsock::listen(uint32_t port, int backlog) {
  int fd = ::socket(AF_VSOCK, SOCK_STREAM, 0);
  if (fd < 0) {
    log_msg(LogLevel::ERROR, "socket(AF_VSOCK) failed: %s",
            std::strerror(errno));
    return -1;
  }
  sockaddr_vm sa{};
  sa.svm_family = AF_VSOCK;
  sa.svm_cid = VMADDR_CID_ANY;
  sa.svm_port = port;
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (::bind(fd, (sockaddr*)&sa, sizeof(sa)) < 0) {
    log_msg(LogLevel::ERROR, "bind(vsock:%u) failed: %s", port,
            std::strerror(errno));
    ::close(fd);
    return -1;
  }
  if (::listen(fd, backlog) < 0) {
    log_msg(LogLevel::ERROR, "listen(vsock:%u) failed: %s", port,
            std::strerror(errno));
    ::close(fd);
    return -1;
  }
  return fd;
}
int Vsock::accept(int listen_fd, VsockPeer& peer) {
  sockaddr_vm pa{};
  socklen_t plen = sizeof(pa);
  int c = ::accept(listen_fd, (sockaddr*)&pa, &plen);
  if (c < 0) return -1;
  peer.cid = pa.svm_cid;
  peer.port = pa.svm_port;
  return c;
}
static bool wait_writable(int fd, int timeout_ms) {
  pollfd p{};
  p.fd = fd;
  p.events = POLLOUT;
  int r = ::poll(&p, 1, timeout_ms);
  return (r == 1) && (p.revents & POLLOUT);
}
int Vsock::connect(uint32_t cid, uint32_t port, int timeout_ms) {
  int fd = ::socket(AF_VSOCK, SOCK_STREAM, 0);
  if (fd < 0) {
    log_msg(LogLevel::ERROR, "socket(AF_VSOCK) failed: %s",
            std::strerror(errno));
    return -1;
  }
  int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  sockaddr_vm sa{};
  sa.svm_family = AF_VSOCK;
  sa.svm_cid = cid;
  sa.svm_port = port;
  int rc = ::connect(fd, (sockaddr*)&sa, sizeof(sa));
  if (rc == 0) {
    ::fcntl(fd, F_SETFL, flags);
    return fd;
  }
  if (errno != EINPROGRESS) {
    log_msg(LogLevel::ERROR, "connect(vsock %u:%u) failed: %s", cid, port,
            std::strerror(errno));
    ::close(fd);
    return -1;
  }
  if (!wait_writable(fd, timeout_ms)) {
    log_msg(LogLevel::WARN, "connect(vsock %u:%u) timeout after %dms", cid,
            port, timeout_ms);
    ::close(fd);
    return -1;
  }
  int soerr = 0;
  socklen_t slen = sizeof(soerr);
  ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
  if (soerr != 0) {
    log_msg(LogLevel::ERROR, "connect(vsock %u:%u) SO_ERROR=%s", cid, port,
            std::strerror(soerr));
    ::close(fd);
    return -1;
  }
  ::fcntl(fd, F_SETFL, flags);
  return fd;
}
void Vsock::close_fd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}