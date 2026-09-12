/*
 * VsockUtils.h - VSOCK utility functions
 *
 */

#ifndef VSOCK_UTILS_H
#define VSOCK_UTILS_H

#include <cstddef>
#include <cstdint>

namespace screentransfer {

class VsockUtils {
 public:
  /**
   * Create VSOCK listening socket
   *
   * @param port Port number to listen on
   * @return Socket file descriptor on success, -1 on error
   */
  static int createVsockListener(uint32_t port);

  /**
   * Accept connection on listening socket
   *
   * @param listenFd Listening socket file descriptor
   * @return Connected socket file descriptor on success, -1 on error
   */
  static int acceptConnection(int listenFd);

  /**
   * Write entire buffer to socket
   *
   * Handles partial writes and EINTR automatically.
   *
   * @param fd Socket file descriptor
   * @param buf Data buffer
   * @param size Number of bytes to write
   * @return true if all data written, false on error
   */
  static bool writeFully(int fd, const void* buf, size_t size);

  /**
   * Read exact number of bytes from socket
   *
   * Handles partial reads and EINTR automatically.
   *
   * @param fd Socket file descriptor
   * @param buf Buffer to store data
   * @param size Number of bytes to read
   * @return true if all data read, false on error
   */
  static bool readFully(int fd, void* buf, size_t size);
};

}  // namespace screentransfer

#endif  // VSOCK_UTILS_H