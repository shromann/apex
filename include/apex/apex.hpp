#pragma once

// Public umbrella header for the apex trading engine.

#include "apex/broker/broker_gateway.hpp"
#include "apex/core/event.hpp"
#include "apex/core/log.hpp"
#include "apex/core/market_data.hpp"
#include "apex/core/order.hpp"
#include "apex/core/types.hpp"
#include "apex/engine/engine.hpp"
#include "apex/engine/event_bus.hpp"
#include "apex/market/market_data_feed.hpp"
#include "apex/plugin/plugin_entry.hpp"
#include "apex/plugin/plugin_loader.hpp"
#include "apex/strategy/strategy.hpp"
#include "apex/strategy/strategy_context.hpp"
#include "apex/strategy/strategy_registrar.hpp"
#include "apex/strategy/strategy_registry.hpp"
