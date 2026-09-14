
#include "broker.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

#include "fram.hpp"
#include "io.hpp"
#include "log.hpp"
#include "vsock.hpp"

Broker::Broker(BrokerConfig cfg) : cfg_(cfg) {}

int Broker::run_forever() {
  int listen_fd = Vsock::listen(cfg_.host_listen_port);
  if (listen_fd < 0) {
    log_msg(LogLevel::ERROR, "FATAL: cannot listen on port %u",
            cfg_.host_listen_port);
    return 2;
  }

  log_msg(LogLevel::INFO, "Listening for AGL on host vsock port=%u",
          cfg_.host_listen_port);

  for (;;) {
    VsockPeer peer{};
    int agl_fd = Vsock::accept(listen_fd, peer);
    if (agl_fd < 0) {
      log_msg(LogLevel::WARN, "accept() failed: %s", std::strerror(errno));
      continue;
    }

    log_msg(LogLevel::INFO,
            "AGL connected (cid=%u port=%u). Now connecting to Android...",
            peer.cid, peer.port);

    int android_fd = Vsock::connect(cfg_.android_cid, cfg_.android_port,
                                    cfg_.android_connect_timeout_ms);
    if (android_fd < 0) {
      log_msg(LogLevel::WARN,
              "Android connect failed. Closing AGL; back to waiting for AGL.");
      Vsock::close_fd(agl_fd);
      std::this_thread::sleep_for(
          std::chrono::milliseconds(cfg_.retry_sleep_ms));
      continue;
    }

    log_msg(LogLevel::INFO,
            "Android connected (cid=%u port=%u). Forwarding FRAM -> AGL.",
            cfg_.android_cid, cfg_.android_port);

    uint64_t frames = 0;
    fram::Endian locked_endian = fram::Endian::Little;
    bool endian_locked = false;

    for (;;) {
      uint8_t magic[4];
      uint8_t hdr[16];

      ssize_t mr = read_full(android_fd, magic, sizeof(magic));
      if (mr <= 0) {
        log_msg(LogLevel::WARN, "Android disconnected.");
        break;
      }

      if (!fram::is_magic_fram(magic)) {
        log_msg(LogLevel::WARN,
                "Bad magic from Android (not FRAM). Dropping connection.");
        break;
      }

      ssize_t hr = read_full(android_fd, hdr, sizeof(hdr));
      if (hr <= 0) {
        log_msg(LogLevel::WARN, "Android disconnected (header).");
        break;
      }

      if (!endian_locked) {
        locked_endian =
            fram::detect_endian(hdr, /*max_len=*/32u * 1024u * 1024u);
        endian_locked = true;
        log_msg(LogLevel::INFO, "FRAM endian locked: %s",
                (locked_endian == fram::Endian::Little) ? "LE" : "BE");
      }

      fram::Header fh = fram::parse_header(hdr, locked_endian);

      if (fh.len > 32u * 1024u * 1024u) {
        log_msg(LogLevel::WARN, "Unreasonable FRAM len=%u. Dropping.", fh.len);
        break;
      }

      if (cfg_.verbose_headers && (frames % 120 == 0)) {
        log_msg(LogLevel::INFO, "FRAM hdr: len=%u flags=%s pts_hi=%u pts_lo=%u",
                fh.len, fram::flags_to_string(fh.flags).c_str(), fh.pts_hi,
                fh.pts_lo);
      }

      std::string payload;
      payload.resize(fh.len);

      if (fh.len > 0) {
        ssize_t pr = read_full(android_fd, payload.data(), fh.len);
        if (pr <= 0) {
          log_msg(LogLevel::WARN, "Android disconnected (payload).");
          break;
        }
      }

      if (write_full(agl_fd, magic, sizeof(magic)) < 0 ||
          write_full(agl_fd, hdr, sizeof(hdr)) < 0 ||
          (fh.len > 0 && write_full(agl_fd, payload.data(), fh.len) < 0)) {
        log_msg(LogLevel::WARN,
                "AGL connection dropped. Dropping Android now.");
        break;
      }

      frames++;
    }

    ::shutdown(android_fd, SHUT_RDWR);
    Vsock::close_fd(android_fd);

    ::shutdown(agl_fd, SHUT_RDWR);
    Vsock::close_fd(agl_fd);

    log_msg(LogLevel::INFO, "Back to waiting for AGL...");
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.retry_sleep_ms));
  }
}