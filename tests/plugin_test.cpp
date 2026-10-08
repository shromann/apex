#include <cstdlib>
#include <memory>
#include <string>

#include "apex/plugin/plugin_loader.hpp"
#include "apex/strategy/strategy_registry.hpp"
#include "test_common.hpp"

int main() {
  const char* path = std::getenv("APEX_MOMENTUM_PLUGIN");
  if (path == nullptr) {
    std::printf("plugin: APEX_MOMENTUM_PLUGIN not set, skipping\n");
    return 0;
  }

  apex::PluginLoader loader;
  if (!loader.load(path)) {
    std::fprintf(stderr, "FAIL: could not load plugin '%s': %s\n", path,
                 loader.last_error().c_str());
    return 1;
  }

  auto& registry = apex::StrategyRegistry::instance();
  CHECK(registry.contains("momentum"));

  auto strategy = registry.create("momentum");
  CHECK(strategy != nullptr);
  CHECK(std::string(strategy->name()) == "momentum");

  return apextest::summary("plugin");
}
