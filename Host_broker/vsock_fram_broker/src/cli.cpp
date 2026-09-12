
#include "cli.hpp"

#include <cstdlib>
#include <cstring>

#include "log.hpp"
void print_help(const char* prog) {
  std::printf(
      "Usage: %s [options]\n"
      "\n"
      "Options:\n"
      "  --host-listen-port <port>     Host vsock port to accept AGL (default: "
      "5000)\n"
      "  --android-cid <cid>           Android guest CID (default: 3)\n"
      "  --android-port <port>         Android vsock port (default: 22345)\n"
      "  --android-timeout-ms <ms>     Android connect timeout (default: "
      "1500)\n"
      "  --retry-sleep-ms <ms>         Sleep between retries (default: 500)\n"
      "  --verbose-headers             Log FRAM header periodically\n"
      "  -h, --help                    Show help\n",
      prog);
}
static bool parse_u32(const char* s, uint32_t& out) {
  if (!s || !*s) return false;
  char* end = nullptr;
  unsigned long v = std::strtoul(s, &end, 10);
  if (!end || *end != '\0') return false;
  out = (uint32_t)v;
  return true;
}
static bool parse_i32(const char* s, int& out) {
  uint32_t tmp = 0;
  if (!parse_u32(s, tmp)) return false;
  out = (int)tmp;
  return true;
}
CliResult parse_args(int argc, char** argv) {
  CliResult res;
  res.ok = false;
  res.cfg = BrokerConfig{};
  for (int i = 1; i < argc; i++) {
    const char* a = argv[i];
    if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) {
      res.help = true;
      res.ok = true;
      return res;
    } else if (!std::strcmp(a, "--host-listen-port")) {
      if (i + 1 >= argc) {
        res.error = "Missing value for --host-listen-port";
        return res;
      }
      if (!parse_u32(argv[++i], res.cfg.host_listen_port)) {
        res.error = "Invalid --host-listen-port";
        return res;
      }
    } else if (!std::strcmp(a, "--android-cid")) {
      if (i + 1 >= argc) {
        res.error = "Missing value for --android-cid";
        return res;
      }
      if (!parse_u32(argv[++i], res.cfg.android_cid)) {
        res.error = "Invalid --android-cid";
        return res;
      }
    } else if (!std::strcmp(a, "--android-port")) {
      if (i + 1 >= argc) {
        res.error = "Missing value for --android-port";
        return res;
      }
      if (!parse_u32(argv[++i], res.cfg.android_port)) {
        res.error = "Invalid --android-port";
        return res;
      }
    } else if (!std::strcmp(a, "--android-timeout-ms")) {
      if (i + 1 >= argc) {
        res.error = "Missing value for --android-timeout-ms";
        return res;
      }
      if (!parse_i32(argv[++i], res.cfg.android_connect_timeout_ms)) {
        res.error = "Invalid --android-timeout-ms";
        return res;
      }
    } else if (!std::strcmp(a, "--retry-sleep-ms")) {
      if (i + 1 >= argc) {
        res.error = "Missing value for --retry-sleep-ms";
        return res;
      }
      if (!parse_i32(argv[++i], res.cfg.retry_sleep_ms)) {
        res.error = "Invalid --retry-sleep-ms";
        return res;
      }
    } else if (!std::strcmp(a, "--verbose-headers")) {
      res.cfg.verbose_headers = true;
    } else {
      res.error = std::string("Unknown arg: ") + a;
      return res;
    }
  }
  res.ok = true;
  return res;
}
BrokerConfig parse_args_or_die(int argc, char** argv) {
  CliResult r = parse_args(argc, argv);
  if (r.ok && r.help) {
    print_help(argv[0]);
    std::exit(0);
  }
  if (!r.ok) {
    log_msg(LogLevel::ERROR, "%s", r.error.c_str());
    print_help(argv[0]);
    std::exit(2);
  }
  return r.cfg;
}