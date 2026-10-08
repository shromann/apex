#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "apex/strategy/strategy.hpp"
#include "apex/strategy/strategy_registrar.hpp"
#include "apex/strategy/strategy_registry.hpp"
#include "test_common.hpp"

namespace {

class DummyStrategy final : public apex::IStrategy {
 public:
  std::string_view name() const override { return "dummy"; }
};

// A strategy that self-registers through the macro, as built-ins do.
class SelfRegisteredStrategy final : public apex::IStrategy {
 public:
  std::string_view name() const override { return "self_registered"; }
};

}  // namespace

APEX_REGISTER_STRATEGY(SelfRegisteredStrategy, "self_registered");

int main() {
  using apex::StrategyRegistry;

  auto& registry = StrategyRegistry::instance();

  // Self-registration via the macro ran at static init time.
  CHECK(registry.contains("self_registered"));

  // Explicit registration and duplicate rejection.
  CHECK(registry.register_strategy(
      "dummy", [] { return std::make_unique<DummyStrategy>(); }));
  CHECK(!registry.register_strategy(
      "dummy", [] { return std::make_unique<DummyStrategy>(); }));

  // Factory instantiation.
  auto instance = registry.create("dummy");
  CHECK(instance != nullptr);
  CHECK(std::string(instance->name()) == "dummy");

  // Unknown names throw.
  bool threw = false;
  try {
    registry.create("does_not_exist");
  } catch (const std::out_of_range&) {
    threw = true;
  }
  CHECK(threw);

  // Listing reflects registrations.
  const auto names = registry.names();
  bool has_dummy = false;
  for (const auto& name : names) has_dummy = has_dummy || name == "dummy";
  CHECK(has_dummy);

  // Unregistration.
  CHECK(registry.unregister_strategy("dummy"));
  CHECK(!registry.contains("dummy"));
  CHECK(!registry.unregister_strategy("dummy"));

  return apextest::summary("strategy_registry");
}
