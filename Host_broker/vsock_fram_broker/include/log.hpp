
#pragma once
#include <cstdarg>
#include <cstdio>
#include <ctime>

enum class LogLevel { INFO, WARN, ERROR, DEBUG };

inline const char* lvl_name(LogLevel l) {
  switch (l) {
    case LogLevel::INFO:
      return "INFO";
    case LogLevel::WARN:
      return "WARN";
    case LogLevel::ERROR:
      return "ERROR";
    case LogLevel::DEBUG:
      return "DEBUG";
  }
  return "UNK";
}

inline void log_msg(LogLevel lvl, const char* fmt, ...) {
  // Get local time in a thread-safe way
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif

  // Build timestamp safely with strftime; "%Y-%m-%d %H:%M:%S" -> 19 chars
  char ts[32];
  if (std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm) == 0) {
    // Fallback if formatting fails (very unlikely)
    std::snprintf(ts, sizeof(ts), "0000-00-00 00:00:00");
  }

  // Print prefix "[broker][LEVEL] YYYY-MM-DD HH:MM:SS "
  std::fprintf(stdout, "[broker][%s] %s ", lvl_name(lvl), ts);

  // Forward the variadic message
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stdout, fmt, ap);
  va_end(ap);

  // Terminate the line and flush
  std::fprintf(stdout, "\n");
  std::fflush(stdout);
}
