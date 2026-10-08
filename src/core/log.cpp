#include "apex/core/log.hpp"

#include <chrono>
#include <format>
#include <iostream>

namespace apex {

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

void Logger::write(LogLevel level, std::string_view tag, std::string_view message) {
  if (!enabled(level)) return;

  const auto now_wall = std::chrono::system_clock::now();
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
      now_wall.time_since_epoch());
  const std::string stamp = std::format("{:%H:%M:%S}", millis);

  std::lock_guard<std::mutex> lock(mutex_);
  std::cerr << '[' << stamp << "] [" << to_string(level) << "] " << tag << ": "
            << message << '\n';
}

}  // namespace apex
