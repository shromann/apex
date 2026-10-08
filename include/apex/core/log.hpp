#pragma once

#include <atomic>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace apex {

enum class LogLevel { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5 };

inline std::string_view to_string(LogLevel v) {
  switch (v) {
    case LogLevel::Trace:
      return "TRACE";
    case LogLevel::Debug:
      return "DEBUG";
    case LogLevel::Info:
      return "INFO";
    case LogLevel::Warn:
      return "WARN";
    case LogLevel::Error:
      return "ERROR";
    case LogLevel::Off:
      return "OFF";
  }
  return "?";
}

// Process-wide logger. Thread-safe; a single instance is shared by the engine
// and every adapter. Strategies should log through IStrategyContext::log.
class Logger {
 public:
  static Logger& instance();

  void set_level(LogLevel level) { level_.store(level); }
  LogLevel level() const { return level_.load(); }
  bool enabled(LogLevel level) const {
    return static_cast<int>(level) >= static_cast<int>(level_.load());
  }

  void write(LogLevel level, std::string_view tag, std::string_view message);

  template <typename... Args>
  void logf(LogLevel level, std::string_view tag,
            std::format_string<Args...> fmt, Args&&... args) {
    if (!enabled(level)) return;
    write(level, tag, std::format(fmt, std::forward<Args>(args)...));
  }

 private:
  Logger() = default;
  std::atomic<LogLevel> level_{LogLevel::Info};
  std::mutex mutex_;
};

}  // namespace apex

#define APEX_LOG_TRACE(tag, ...) \
  ::apex::Logger::instance().logf(::apex::LogLevel::Trace, tag, __VA_ARGS__)
#define APEX_LOG_DEBUG(tag, ...) \
  ::apex::Logger::instance().logf(::apex::LogLevel::Debug, tag, __VA_ARGS__)
#define APEX_LOG_INFO(tag, ...) \
  ::apex::Logger::instance().logf(::apex::LogLevel::Info, tag, __VA_ARGS__)
#define APEX_LOG_WARN(tag, ...) \
  ::apex::Logger::instance().logf(::apex::LogLevel::Warn, tag, __VA_ARGS__)
#define APEX_LOG_ERROR(tag, ...) \
  ::apex::Logger::instance().logf(::apex::LogLevel::Error, tag, __VA_ARGS__)
