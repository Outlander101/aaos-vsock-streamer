
#pragma once
#include <cstddef>
#include <cstdint>
struct VsockPeer {
  uint32_t cid = 0;
  uint32_t port = 0;
};
class Vsock {
 public:
  static int listen(uint32_t port, int backlog = 4);
  static int accept(int listen_fd, VsockPeer& peer);
  static int connect(uint32_t cid, uint32_t port, int timeout_ms);
  static void close_fd(int& fd);
};