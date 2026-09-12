
#include "broker/log.hpp"

#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
namespace broker {
static std::atomic<bool> g_verbose{false};
static std::mutex g_mu;
void set_verbose(bool v) { g_verbose.store(v, std::memory_order_relaxed); }
bool verbose_enabled() { return g_verbose.load(std::memory_order_relaxed); }
static const char* lvl_str(LogLevel lvl) {
  switch (lvl) {
    case LogLevel::Debug:
      return "DBG";
    case LogLevel::Info:
      return "INF";
    case LogLevel::Warn:
      return "WRN";
    case LogLevel::Error:
      return "ERR";
  }
  return "UNK";
}
static std::string ts_now() {
  using namespace std::chrono;
  auto now = system_clock::now();
  auto t = system_clock::to_time_t(now);
  auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "." << std::setw(3)
      << std::setfill('0') << ms.count();
  return oss.str();
}
void log(LogLevel lvl, const std::string& msg) {
  if (lvl == LogLevel::Debug && !verbose_enabled()) return;
  std::lock_guard<std::mutex> lk(g_mu);
  std::ostream& os = (lvl == LogLevel::Error) ? std::cerr : std::cout;
  os << "[" << ts_now() << "][" << lvl_str(lvl) << "] " << msg << "\n";
  os.flush();
}
void debug(const std::string& msg) { log(LogLevel::Debug, msg); }
void info(const std::string& msg) { log(LogLevel::Info, msg); }
void warn(const std::string& msg) { log(LogLevel::Warn, msg); }
void error(const std::string& msg) { log(LogLevel::Error, msg); }
}  // namespace broker
