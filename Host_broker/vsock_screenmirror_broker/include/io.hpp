
#pragma once
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
// Read exactly n bytes unless EOF/error.
// Returns:
//  n  on success
//  0  if EOF before any more bytes
// -1  on error
inline ssize_t read_full(int fd, void* buf, size_t n) {
  uint8_t* p = static_cast<uint8_t*>(buf);
  size_t got = 0;
  while (got < n) {
    ssize_t r = ::read(fd, p + got, n - got);
    if (r == 0) return 0;  // EOF
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    got += static_cast<size_t>(r);
  }
  return (ssize_t)got;
}
// Write exactly n bytes unless error.
// Uses MSG_NOSIGNAL to avoid SIGPIPE.
// Returns:
//  n  on success
//  0  if write returns 0 (rare)
// -1  on error
inline ssize_t write_full(int fd, const void* buf, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(buf);
  size_t sent = 0;
  while (sent < n) {
    ssize_t w = ::send(fd, p + sent, n - sent, MSG_NOSIGNAL);
    if (w == 0) return 0;
    if (w < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    sent += static_cast<size_t>(w);
  }
  return (ssize_t)sent;
}