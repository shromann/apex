#include "apex/strategy/strategy_registry.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace apex {

StrategyRegistry& StrategyRegistry::instance() {
  // Intentionally leaked, never destroyed. The registry outlives static
  // teardown: a plugin's factories are std::function objects whose code lives
  // in a shared library that PluginLoader dlcloses before static destructors
  // run. Destroying the registry here would invoke those dangling function
  // objects and crash. As a process-lifetime service it is not torn down.
  static StrategyRegistry* registry = new StrategyRegistry();
  return *registry;
}

bool StrategyRegistry::register_strategy(std::string name, Creator creator) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto [it, inserted] = creators_.emplace(std::move(name), std::move(creator));
  return inserted;
}

bool StrategyRegistry::unregister_strategy(std::string_view name) {
  std::lock_guard<std::mutex> lock(mutex_);
  return creators_.erase(std::string(name)) > 0;
}

bool StrategyRegistry::contains(std::string_view name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return creators_.count(std::string(name)) > 0;
}

std::unique_ptr<IStrategy> StrategyRegistry::create(std::string_view name) const {
  Creator creator;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = creators_.find(std::string(name));
    if (it == creators_.end()) {
      throw std::out_of_range("unknown strategy: " + std::string(name));
    }
    creator = it->second;  // copy out; never run user code under the lock
  }
  return creator();
}

std::vector<std::string> StrategyRegistry::names() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> result;
  result.reserve(creators_.size());
  for (const auto& [name, creator] : creators_) {
    (void)creator;
    result.push_back(name);
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::size_t StrategyRegistry::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return creators_.size();
}

}  // namespace apex
