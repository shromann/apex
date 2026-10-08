#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace apex {

// Minimal typed publish/subscribe hub. Handlers are keyed by the concrete
// payload type, so `subscribe<QuoteEvent>` only receives quotes. Delivery is
// synchronous; handlers are invoked without the internal lock held, so a handler
// may safely publish or unsubscribe.
class EventBus {
 public:
  using Token = std::size_t;

  EventBus() = default;
  EventBus(const EventBus&) = delete;
  EventBus& operator=(const EventBus&) = delete;

  template <typename E>
  Token subscribe(std::function<void(const E&)> handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Token token = next_token_++;
    auto& handlers = handlers_[std::type_index(typeid(E))];
    handlers.push_back(Entry{token, [handler = std::move(handler)](const void* event) {
                               handler(*static_cast<const E*>(event));
                             }});
    return token;
  }

  void unsubscribe(Token token) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [type, handlers] : handlers_) {
      for (auto it = handlers.begin(); it != handlers.end(); ++it) {
        if (it->token == token) {
          handlers.erase(it);
          return;
        }
      }
    }
  }

  template <typename E>
  void publish(const E& event) {
    std::vector<std::function<void(const void*)>> local;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = handlers_.find(std::type_index(typeid(E)));
      if (it == handlers_.end()) return;
      local.reserve(it->second.size());
      for (const auto& entry : it->second) local.push_back(entry.fn);
    }
    for (const auto& fn : local) fn(&event);
  }

  template <typename E>
  std::size_t subscriber_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = handlers_.find(std::type_index(typeid(E)));
    return it == handlers_.end() ? 0 : it->second.size();
  }

 private:
  struct Entry {
    Token token;
    std::function<void(const void*)> fn;
  };

  mutable std::mutex mutex_;
  std::unordered_map<std::type_index, std::vector<Entry>> handlers_;
  Token next_token_ = 1;
};

}  // namespace apex
