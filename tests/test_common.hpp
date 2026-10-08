#pragma once

#include <cstdio>
#include <string>

namespace apextest {

inline int& failures() {
  static int value = 0;
  return value;
}

inline int& checks() {
  static int value = 0;
  return value;
}

inline void check(bool condition, const char* expression, const char* file,
                  int line) {
  ++checks();
  if (!condition) {
    ++failures();
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
  }
}

inline int summary(const char* name) {
  std::printf("%s: %d checks, %d failure(s)\n", name, checks(), failures());
  return failures() == 0 ? 0 : 1;
}

}  // namespace apextest

#define CHECK(condition) \
  ::apextest::check((condition), #condition, __FILE__, __LINE__)
