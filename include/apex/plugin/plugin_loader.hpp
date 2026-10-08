#pragma once

#include <string>
#include <vector>

namespace apex {

// Every plugin shared library must export this C-linkage entry point. The loader
// calls it after dlopen(); inside, the plugin's registrars run and populate the
// shared StrategyRegistry.
//
//   extern "C" void apex_plugin_register();
//
// The APEX_PLUGIN_ENTRY() macro expands to exactly that definition.
using ApexPluginRegisterFn = void (*)();

inline constexpr const char* kPluginEntrySymbol = "apex_plugin_register";

// Loads strategy plugins from shared libraries at runtime (dlopen/dlsym on
// POSIX). Strategies exported this way are indistinguishable from built-ins.
class PluginLoader {
 public:
  PluginLoader() = default;
  ~PluginLoader();

  PluginLoader(const PluginLoader&) = delete;
  PluginLoader& operator=(const PluginLoader&) = delete;

  // dlopen(path), resolve the entry symbol, and invoke it. Returns false and
  // populates last_error() on failure.
  bool load(const std::string& path);

  // Convenience: load every plugin in a directory (best-effort).
  std::size_t load_directory(const std::string& directory);

  void unload_all();

  const std::vector<std::string>& loaded_paths() const { return paths_; }
  const std::string& last_error() const { return last_error_; }

 private:
  std::vector<void*> handles_;
  std::vector<std::string> paths_;
  std::string last_error_;
};

}  // namespace apex
