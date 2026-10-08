#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "apex/adapters/mock/mock_broker_gateway.hpp"
#include "apex/adapters/mock/mock_market_data_feed.hpp"
#include "apex/adapters/webull/webull_broker_gateway.hpp"
#include "apex/adapters/webull/webull_config.hpp"
#include "apex/adapters/webull/webull_market_data_feed.hpp"
#include "apex/adapters/webull/webull_rest_client.hpp"
#include "apex/apex.hpp"

namespace {

namespace fs = std::filesystem;
using namespace apex;

struct Options {
  std::string strategy{"ma_cross"};
  std::string plugin{};
  std::string feed{"mock"};    // "mock" | "webull"
  std::string broker{"mock"};  // "mock" | "webull"
  std::vector<std::string> symbols{"AAPL"};
  double duration_seconds{2.0};
  bool list{false};
  LogLevel log_level{LogLevel::Info};
  std::vector<std::pair<std::string, double>> params{};

  // Webull credentials. Provided on the command line, written into the
  // environment, then picked up by WebullConfig::from_environment().
  std::string app_key{};
  std::string app_secret{};
  std::string access_token{};
  std::string account_id{};
  std::string region{};
  std::string device_id{};
  bool sandbox{false};
  bool sandbox_set{false};
  bool probe{false};  // one signed GET to the REST host, then exit
};

std::vector<std::string> split(const std::string& text, char delim) {
  std::vector<std::string> parts;
  std::string current;
  for (char c : text) {
    if (c == delim) {
      if (!current.empty()) parts.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) parts.push_back(current);
  return parts;
}

Options parse(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << name << "\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--strategy") {
      opt.strategy = next("--strategy");
    } else if (arg == "--plugin") {
      opt.plugin = next("--plugin");
    } else if (arg == "--symbols") {
      opt.symbols = split(next("--symbols"), ',');
    } else if (arg == "--duration") {
      opt.duration_seconds = std::stod(next("--duration"));
    } else if (arg == "--param") {
      auto kv = next("--param");
      auto pos = kv.find('=');
      if (pos == std::string::npos) {
        std::cerr << "expected --param key=value\n";
        std::exit(2);
      }
      opt.params.emplace_back(kv.substr(0, pos), std::stod(kv.substr(pos + 1)));
    } else if (arg == "--feed") {
      opt.feed = next("--feed");
    } else if (arg == "--broker") {
      opt.broker = next("--broker");
    } else if (arg == "--app-key") {
      opt.app_key = next("--app-key");
    } else if (arg == "--app-secret") {
      opt.app_secret = next("--app-secret");
    } else if (arg == "--access-token") {
      opt.access_token = next("--access-token");
    } else if (arg == "--account-id") {
      opt.account_id = next("--account-id");
    } else if (arg == "--region") {
      opt.region = next("--region");
    } else if (arg == "--device-id") {
      opt.device_id = next("--device-id");
    } else if (arg == "--sandbox") {
      opt.sandbox = true;
      opt.sandbox_set = true;
    } else if (arg == "--probe") {
      opt.probe = true;
    } else if (arg == "--list") {
      opt.list = true;
    } else if (arg == "--quiet") {
      opt.log_level = LogLevel::Warn;
    } else if (arg == "--verbose") {
      opt.log_level = LogLevel::Debug;
    } else if (arg == "--help" || arg == "-h") {
      std::cout
          << "apex - modular trading engine\n\n"
          << "usage: apex [options]\n"
          << "  --strategy <name>     strategy to run (default: ma_cross)\n"
          << "  --plugin <path>       load a strategy plugin (.dylib/.so)\n"
          << "  --feed <mock|webull>  market data source (default: mock)\n"
          << "  --broker <mock|webull> execution venue (default: mock)\n"
          << "  --symbols A,B,C       instruments to trade (default: AAPL)\n"
          << "  --duration <sec>      how long to run (default: 2.0)\n"
          << "  --param key=value     strategy parameter (repeatable)\n"
          << "  --list                list registered strategies and exit\n"
          << "  --quiet | --verbose   log level\n\n"
          << "Webull credentials (stored as WEBULL_* environment variables):\n"
          << "  --app-key <key>       sets WEBULL_APP_KEY\n"
          << "  --app-secret <secret> sets WEBULL_APP_SECRET\n"
          << "  --access-token <tok>  sets WEBULL_ACCESS_TOKEN (2FA apps)\n"
          << "  --account-id <id>     sets WEBULL_ACCOUNT_ID\n"
          << "  --region <us|hk|jp>   sets WEBULL_REGION\n"
          << "  --device-id <id>      sets WEBULL_DEVICE_ID\n"
          << "  --sandbox             use the sandbox environment\n";
      std::exit(0);
    } else {
      std::cerr << "unknown option: " << arg << " (try --help)\n";
      std::exit(2);
    }
  }
  return opt;
}

// Credentials from the command line are written into the environment so they
// can be configured once and picked up by WebullConfig::from_environment().
void apply_credentials_to_environment(const Options& opt) {
  auto put = [](const char* name, const std::string& value) {
    if (!value.empty()) ::setenv(name, value.c_str(), 1);
  };
  put(webull_env::kAppKey, opt.app_key);
  put(webull_env::kAppSecret, opt.app_secret);
  put(webull_env::kAccessToken, opt.access_token);
  put(webull_env::kAccountId, opt.account_id);
  put(webull_env::kRegion, opt.region);
  put(webull_env::kDeviceId, opt.device_id);
  if (opt.sandbox_set) {
    ::setenv(webull_env::kSandbox, opt.sandbox ? "1" : "0", 1);
  }
}

std::optional<std::string> discover_plugin(const char* argv0) {
  std::error_code ec;
  std::vector<fs::path> directories;
  auto exe = fs::weakly_canonical(fs::path(argv0), ec);
  if (!ec) directories.push_back(exe.parent_path());
  directories.push_back(fs::current_path(ec));

  const char* candidate_names[] = {"libapex_momentum.dylib", "libapex_momentum.so"};
  for (const auto& dir : directories) {
    for (const char* name : candidate_names) {
      fs::path candidate = dir / name;
      if (fs::exists(candidate, ec)) return candidate.string();
    }
  }
  return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt = parse(argc, argv);
  Logger::instance().set_level(opt.log_level);
  apply_credentials_to_environment(opt);

  // Diagnostic: prove TLS + HTTP + signing reach Webull by issuing one signed
  // request and reporting the HTTP status. A 401/403 here (rather than a
  // transport error) means the whole signing pipeline is wired correctly.
  if (opt.probe) {
    WebullConfig config = WebullConfig::from_environment();
    std::cout << "probing https://" << config.api_host
              << "/trading/accounts/list (signed, credentials required)\n";
    WebullRestClient client(config);
    WebullResponse response = client.list_accounts();
    if (!response.transport_ok()) {
      std::cerr << "transport error: " << response.error << "\n";
      return 2;
    }
    std::cout << "http status: " << response.status << "\n";
    std::cout << "body: " << response.body << "\n";
    return response.ok() ? 0 : 1;
  }

  PluginLoader plugin_loader;
  if (!opt.plugin.empty()) {
    if (!plugin_loader.load(opt.plugin)) {
      std::cerr << "failed to load plugin '" << opt.plugin
                << "': " << plugin_loader.last_error() << "\n";
    }
  } else if (auto discovered = discover_plugin(argv[0])) {
    if (plugin_loader.load(*discovered)) {
      std::cout << "loaded plugin: " << *discovered << "\n";
    }
  }

  auto& registry = StrategyRegistry::instance();
  if (opt.list) {
    std::cout << "registered strategies:\n";
    for (const auto& name : registry.names()) std::cout << "  " << name << "\n";
    return 0;
  }

  std::unique_ptr<IStrategy> strategy;
  try {
    strategy = registry.create(opt.strategy);
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << "\nregistered strategies:";
    for (const auto& name : registry.names()) std::cerr << " " << name;
    std::cerr << "\n";
    return 1;
  }

  // Select adapters. The engine is identical either way -- swapping venues is
  // purely an injection choice, which is the point of the port/adapter design.
  std::unique_ptr<IMarketDataFeed> feed;
  std::unique_ptr<IBrokerGateway> broker;
  MockBrokerGateway* mock_broker = nullptr;

  if (opt.broker == "webull") {
    WebullConfig config = WebullConfig::from_environment();
    if (!config.is_configured()) {
      std::cerr << "webull broker requires --app-key/--app-secret "
                   "(or WEBULL_APP_KEY/WEBULL_APP_SECRET)\n";
      return 1;
    }
    broker = std::make_unique<WebullBrokerGateway>(config);
  } else if (opt.broker == "mock") {
    auto mock = std::make_unique<MockBrokerGateway>(MockBrokerConfig{});
    mock_broker = mock.get();
    broker = std::move(mock);
  } else {
    std::cerr << "unknown broker: " << opt.broker << "\n";
    return 1;
  }

  if (opt.feed == "webull") {
    WebullConfig config = WebullConfig::from_environment();
    if (!config.is_configured()) {
      std::cerr << "webull feed requires --app-key/--app-secret "
                   "(or WEBULL_APP_KEY/WEBULL_APP_SECRET)\n";
      return 1;
    }
    feed = std::make_unique<WebullMarketDataFeed>(config);
  } else if (opt.feed == "mock") {
    feed = std::make_unique<MockMarketDataFeed>(MockFeedConfig{});
  } else {
    std::cerr << "unknown feed: " << opt.feed << "\n";
    return 1;
  }

  EngineConfig engine_config;
  engine_config.symbols = opt.symbols;
  Engine engine(engine_config, std::move(feed), std::move(broker));

  engine.set_strategy(std::move(strategy));

  StrategyParams params(opt.symbols);
  for (const auto& [key, value] : opt.params) params.set(key, value);
  engine.configure_strategy(params);

  // Feed the paper broker's price book from the market data the engine
  // publishes (mock broker only; a live gateway gets prices from the venue).
  if (mock_broker != nullptr) {
    engine.bus().subscribe<QuoteEvent>([mock_broker](const QuoteEvent& event) {
      const Quote& quote = event.quote;
      Price price = quote.last > 0.0 ? quote.last : quote.mid();
      if (price > 0.0) mock_broker->update_price(quote.symbol, price);
    });
  }

  if (!engine.start()) {
    std::cerr << "engine failed to start\n";
    return 1;
  }

  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(opt.duration_seconds));
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  // Capture state before disconnecting the gateway.
  const auto positions = engine.positions();
  engine.stop();

  std::cout << "\n=== session summary ===\n";
  if (positions.empty()) std::cout << "no open positions\n";
  for (const auto& position : positions) {
    std::cout << "  " << position.symbol << "  qty=" << position.quantity
              << "  avg=" << position.average_price
              << "  last=" << position.last_price
              << "  uPnL=" << position.unrealized_pnl() << "\n";
  }
  if (mock_broker != nullptr) {
    std::cout << "  cash=" << mock_broker->cash()
              << "  equity=" << mock_broker->equity() << "\n";
  }
  return 0;
}
