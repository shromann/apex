#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "apex/strategy/strategy.hpp"

namespace apex {

// Process-wide factory keyed by strategy name. Built-in strategies self-register
// at static-initialization time; dynamically loaded plugins register through the
// very same instance (the engine is a shared library, so there is one registry).
class StrategyRegistry {
 public:
  using Creator = std::function<std::unique_ptr<IStrategy>()>;

  static StrategyRegistry& instance();

  StrategyRegistry(const StrategyRegistry&) = delete;
  StrategyRegistry& operator=(const StrategyRegistry&) = delete;

  // Registers a factory under `name`. Returns false if the name is taken.
  bool register_strategy(std::string name, Creator creator);
  bool unregister_strategy(std::string_view name);

  bool contains(std::string_view name) const;

  // Instantiates a strategy. Throws std::out_of_range if unknown.
  std::unique_ptr<IStrategy> create(std::string_view name) const;

  std::vector<std::string> names() const;
  std::size_t size() const;

 private:
  StrategyRegistry() = default;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, Creator> creators_;
};

}  // namespace apex
