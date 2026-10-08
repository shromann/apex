#pragma once

#include <memory>
#include <string>
#include <utility>

#include "apex/strategy/strategy_registry.hpp"

namespace apex {

// Instantiate one of these at namespace scope to self-register a strategy type
// with the global registry. Prefer the APEX_REGISTER_STRATEGY macro.
template <typename T>
class StrategyRegistrar {
 public:
  explicit StrategyRegistrar(std::string name) {
    StrategyRegistry::instance().register_strategy(
        std::move(name), [] { return std::make_unique<T>(); });
  }
};

}  // namespace apex

#define APEX_DETAIL_CONCAT_(a, b) a##b
#define APEX_DETAIL_CONCAT(a, b) APEX_DETAIL_CONCAT_(a, b)

// APEX_REGISTER_STRATEGY(MyStrategy, "my_strategy");
// Must be used at namespace scope in a .cpp that is compiled into the engine or
// into a plugin library.
#define APEX_REGISTER_STRATEGY(TYPE, NAME)                                  \
  namespace {                                                              \
  const ::apex::StrategyRegistrar<TYPE> APEX_DETAIL_CONCAT(               \
      apex_strategy_registrar_, __LINE__){(NAME)};                        \
  }                                                                        \
  static_assert(true, "APEX_REGISTER_STRATEGY requires a trailing semicolon")
