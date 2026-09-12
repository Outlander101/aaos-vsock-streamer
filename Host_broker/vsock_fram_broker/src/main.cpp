
#include <csignal>

#include "broker.hpp"
#include "cli.hpp"
#include "log.hpp"
int main(int argc, char** argv) {
  // Prevent process death when writing to a dead socket
  std::signal(SIGPIPE, SIG_IGN);
  BrokerConfig cfg = parse_args_or_die(argc, argv);
  log_msg(LogLevel::INFO,
          "Config: host_listen_port=%u android_cid=%u android_port=%u "
          "android_timeout_ms=%d retry_sleep_ms=%d verbose_headers=%d",
          cfg.host_listen_port, cfg.android_cid, cfg.android_port,
          cfg.android_connect_timeout_ms, cfg.retry_sleep_ms,
          cfg.verbose_headers ? 1 : 0);
  Broker b(cfg);
  return b.run_forever();
}