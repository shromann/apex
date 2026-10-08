# apex

A modular trading engine in C++. Strategies are interchangeable: the engine
knows nothing about any particular one, and you can add, swap, or hot-load them
without touching engine code.

Market connectivity is pluggable too. Webull is just one adapter behind a port;
a paper-trading simulator ships in the box so the engine runs end to end with no
live credentials.

## Design

apex uses **hexagonal (ports & adapters)** architecture, the **Strategy**
pattern, a self-registering **Registry/Factory**, and a typed **EventBus**:

```
                    ┌──────────────────────────────────────────┐
                    │                 Engine                   │
                    │  owns feed + broker + strategy           │
                    │  implements IStrategyContext             │
                    └──────┬─────────────┬─────────────┬───────┘
        IMarketDataFeed ◄──┘   IBrokerGateway ◄────────┘   EventBus
        (port)                  (port)                     (observers)
             ▲                        ▲
      ┌──────┴──────┐         ┌───────┴────────┐
      │ Mock feed   │         │ Mock broker    │
      │ Webull feed │         │ Webull broker  │
      └─────────────┘         └────────────────┘

        IStrategy ◄── StrategyRegistry / APEX_REGISTER_STRATEGY / dlopen
```

Why this scales:

- **Strategies only see `IStrategyContext`.** They never touch Webull, sockets,
  or threads. That single seam is what makes them portable and unit-testable
  with a fake context.
- **Adapters implement ports.** `IMarketDataFeed` and `IBrokerGateway` decouple
  the engine from any vendor. Adding a venue means adding an adapter, not editing
  the engine.
- **The registry is the factory.** Built-in strategies self-register; plugin
  libraries register into the *same* `StrategyRegistry` instance because the
  engine is a shared library. Selection is by name at runtime.
- **The EventBus decouples observers.** Risk checks, telemetry, and a paper
  broker's price book subscribe to market and execution events without the
  engine knowing they exist.

## Layout

```
include/apex/            public API (the contract)
  core/                  value types: types, market_data, order, event, log
  market/                IMarketDataFeed port
  broker/                IBrokerGateway port
  strategy/              IStrategy, IStrategyContext, registry, registrar macro
  plugin/                PluginLoader + APEX_PLUGIN_ENTRY
  engine/                Engine + typed EventBus
  adapters/              mock + webull adapters
src/                     implementations, main
strategies/momentum/     an example strategy built as a loadable plugin
tests/                   registry, engine, and plugin tests
```

## Build

Requires a C++23 compiler and CMake ≥ 3.20.

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Products land in `build/bin/`: the `apex` executable, `libapex_core`, and the
example plugin `libapex_momentum.{dylib,so}`.

## Run

```sh
# built-in strategy
./build/bin/apex --strategy ma_cross --symbols AAPL --duration 3

# list what is registered (including anything loaded from a plugin)
./build/bin/apex --list

# load the example plugin and run its strategy
./build/bin/apex --plugin build/bin/libapex_momentum.dylib --strategy momentum
```

If `--plugin` is omitted, `apex` looks for `libapex_momentum.{dylib,so}` next to
the executable and loads it automatically.

The default run uses `MockMarketDataFeed` (a seeded random walk) and
`MockBrokerGateway` (paper trading), so it prints a session summary with
positions and P&L without a market connection.

## Adding a strategy

Implement `IStrategy` and register it — that is the whole contract.

```cpp
#include "apex/strategy/strategy_registrar.hpp"

class MyStrategy final : public apex::IStrategy {
 public:
  std::string_view name() const override { return "my_strategy"; }
  void configure(const apex::StrategyParams& p) override { /* read p.number(...) */ }
  void on_bar(const apex::Bar& bar, apex::IStrategyContext& ctx) override {
    // trade only through ctx
    // ctx.market_buy(bar.symbol, 100, "my_strategy");
  }
};

APEX_REGISTER_STRATEGY(MyStrategy, "my_strategy");
```

Compile it into the engine (add the `.cpp` to the `apex` target) or ship it as a
plugin: build it as a shared library that links `apex_core`, add
`#include "apex/plugin/plugin_entry.hpp"` and define `APEX_PLUGIN_ENTRY() {}` in
one translation unit, then load it with `--plugin`.

## Webull integration

Webull has no official C++ SDK, so the adapter speaks the protocol directly.
It is self-contained apart from OpenSSL:

```
src/net/tls_socket      OpenSSL TLS client (cert verification + SNI always on)
src/net/http_client     HTTP/1.1 (Content-Length + chunked), signed requests
src/net/mqtt_client     MQTT 3.1.1 over TLS -- the real-time market data stream
src/net/grpc_client     scoped HTTP/2 + gRPC for trade events (order updates)
src/util/json           compact builder (exact bytes for signing) + parser
webull_crypto/signature HMAC-SHA1 + MD5 + Base64 + URL-encode request signing
webull_rest_client      signed REST endpoints (accounts, assets, orders, data)
```

Request signing follows Webull's documented algorithm exactly
(`HMAC-SHA1(app_secret + "&", urlencode(path + "&" + sorted(k=v) + ["&" + MD5(body)]))`)
and is pinned to their published worked example in `tests/webull_test.cpp`.

### Credentials

Credentials are passed on the command line, stored as environment variables, and
picked up by the adapter (`WebullConfig::from_environment`):

```sh
./build/bin/apex --feed webull --broker webull \
  --app-key  <APP_KEY>   --app-secret <APP_SECRET> \
  --access-token <TOKEN>        # only when 2FA is enabled \
  --account-id  <ACCOUNT_ID> \
  --region us  [--sandbox] \
  --strategy ma_cross --symbols AAPL --duration 60
```

Or set them directly: `WEBULL_APP_KEY`, `WEBULL_APP_SECRET`,
`WEBULL_ACCESS_TOKEN`, `WEBULL_ACCOUNT_ID`, `WEBULL_REGION`, `WEBULL_SANDBOX`,
`WEBULL_API_HOST`, `WEBULL_QUOTES_HOST`, `WEBULL_EVENTS_HOST`, `WEBULL_MQTT_HOST`.

### Environments

| | production | sandbox |
|---|---|---|
| REST | `api.webull.com` | `api.sandbox.webull.com` |
| MQTT | `data-api.webull.com:1883` | `data-api.sandbox.webull.com:1883` |
| gRPC events | `events-api.webull.com` | `events-api.sandbox.webull.com` |

### Scope and caveats

- **Verified**: signing (against Webull's own worked example), crypto vectors,
  JSON round-trip, host resolution, and a live TLS+HTTP+signing probe against
  `api.webull.com` (returns Webull's standard `401 Invalid credentials` for
  dummy keys, proving the transport and signing pipeline).
- **Not verified against a live account**: order/position/bar response mapping,
  the MQTT topic layout and streaming credentials, and the gRPC trade-events
  stream. Response field names are read defensively and can be corrected in one
  place.
- Order **mutation** paths (`/trading/orders/{place,replace,cancel}`) and the
  streaming subscribe paths are derived from the documented `/trading/orders/*`
  convention; every path can be retargeted at runtime with
  `WebullRestClient::set_path_override()` without recompiling.
- `net::grpc_client` is a purpose-built subset of HTTP/2 + gRPC (static-table
  HPACK only, no Huffman, no flow control), not a general implementation.
- A `--probe` flag performs one signed request and prints the HTTP status;
  useful for checking credentials and connectivity.
