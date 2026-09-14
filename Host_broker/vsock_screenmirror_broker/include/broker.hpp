
#pragma once
#include <cstdint>
struct BrokerConfig {
  uint32_t host_listen_port = 5000;  // AGL connects to host here
  uint32_t android_cid = 3;          // Android guest CID
  uint32_t android_port = 22345;     // Android listens here
  int android_connect_timeout_ms = 1500;
  int retry_sleep_ms = 500;
  bool verbose_headers = false;
};
class Broker {
 public:
  explicit Broker(BrokerConfig cfg);
  int run_forever();  // never exits except fatal bind
 private:
  BrokerConfig cfg_;
};