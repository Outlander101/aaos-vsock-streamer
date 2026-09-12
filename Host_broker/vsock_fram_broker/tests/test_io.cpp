
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <test_harness.hpp>
#include <vector>

#include "io.hpp"
static void make_pair(int fds[2]) {
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
}
TEST(io_write_read_small) {
  int fds[2];
  make_pair(fds);
  const char msg[] = "hello";
  ASSERT_EQ(write_full(fds[0], msg, sizeof(msg)), (ssize_t)sizeof(msg));
  char out[16]{};
  ASSERT_EQ(read_full(fds[1], out, sizeof(msg)), (ssize_t)sizeof(msg));
  ASSERT_TRUE(std::memcmp(out, msg, sizeof(msg)) == 0);
  ::close(fds[0]);
  ::close(fds[1]);
}
TEST(io_read_full_eof_returns_0) {
  int fds[2];
  make_pair(fds);
  ::close(fds[0]);  // writer closed
  char out[8]{};
  ASSERT_EQ(read_full(fds[1], out, 8), (ssize_t)0);
  ::close(fds[1]);
}
TEST(io_write_full_large_buffer) {
  int fds[2];
  make_pair(fds);
  std::vector<uint8_t> buf(64 * 1024);
  for (size_t i = 0; i < buf.size(); i++) buf[i] = (uint8_t)(i & 0xFF);
  ASSERT_EQ(write_full(fds[0], buf.data(), buf.size()), (ssize_t)buf.size());
  std::vector<uint8_t> out(buf.size());
  ASSERT_EQ(read_full(fds[1], out.data(), out.size()), (ssize_t)out.size());
  ASSERT_TRUE(out == buf);
  ::close(fds[0]);
  ::close(fds[1]);
}
TEST(io_write_full_peer_closed_returns_minus1) {
  int fds[2];
  make_pair(fds);
  ::close(fds[1]);  // close receiver
  char b[4] = {1, 2, 3, 4};
  ASSERT_EQ(write_full(fds[0], b, sizeof(b)), (ssize_t)-1);
  ::close(fds[0]);
}
TEST(io_read_full_partial_then_eof_returns_0) {
  int fds[2];
  make_pair(fds);
  // Write fewer bytes than requested then close.
  char b[3] = {9, 8, 7};
  ASSERT_EQ(::write(fds[0], b, 3), 3);
  ::close(fds[0]);
  char out[8]{};
  // We request 8, but EOF will occur; per our contract, read_full returns 0.
  ASSERT_EQ(read_full(fds[1], out, 8), (ssize_t)0);
  ::close(fds[1]);
}
TEST(io_roundtrip_multiple_messages) {
  int fds[2];
  make_pair(fds);
  const char a[] = "A";
  const char b[] = "BC";
  const char c[] = "DEF";
  ASSERT_EQ(write_full(fds[0], a, sizeof(a)), (ssize_t)sizeof(a));
  ASSERT_EQ(write_full(fds[0], b, sizeof(b)), (ssize_t)sizeof(b));
  ASSERT_EQ(write_full(fds[0], c, sizeof(c)), (ssize_t)sizeof(c));
  char oa[2]{};
  char ob[3]{};
  char oc[4]{};
  ASSERT_EQ(read_full(fds[1], oa, sizeof(a)), (ssize_t)sizeof(a));
  ASSERT_EQ(read_full(fds[1], ob, sizeof(b)), (ssize_t)sizeof(b));
  ASSERT_EQ(read_full(fds[1], oc, sizeof(c)), (ssize_t)sizeof(c));
  ASSERT_TRUE(std::memcmp(oa, a, sizeof(a)) == 0);
  ASSERT_TRUE(std::memcmp(ob, b, sizeof(b)) == 0);
  ASSERT_TRUE(std::memcmp(oc, c, sizeof(c)) == 0);
  ::close(fds[0]);
  ::close(fds[1]);
}
TEST(io_write_full_zero_len_is_ok) {
  int fds[2];
  make_pair(fds);
  char dummy = 0;
  ASSERT_EQ(write_full(fds[0], &dummy, 0), (ssize_t)0);
  ::close(fds[0]);
  ::close(fds[1]);
}
TEST(io_read_full_zero_len_is_ok) {
  int fds[2];
  make_pair(fds);
  char dummy = 0;
  ASSERT_EQ(read_full(fds[1], &dummy, 0), (ssize_t)0);
  ::close(fds[0]);
  ::close(fds[1]);
}
TEST(io_write_then_shutdown_reader) {
  int fds[2];
  make_pair(fds);
  ::shutdown(fds[1], SHUT_RDWR);
  char b[8] = {0};
  ASSERT_EQ(write_full(fds[0], b, sizeof(b)), (ssize_t)-1);
  ::close(fds[0]);
  ::close(fds[1]);
}
TEST(io_read_after_shutdown_writer) {
  int fds[2];
  make_pair(fds);
  ::shutdown(fds[0], SHUT_RDWR);
  char out[4]{};
  ASSERT_EQ(read_full(fds[1], out, sizeof(out)), (ssize_t)0);
  ::close(fds[0]);
  ::close(fds[1]);
}