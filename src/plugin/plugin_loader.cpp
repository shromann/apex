#include "apex/plugin/plugin_loader.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace apex {

PluginLoader::~PluginLoader() { unload_all(); }

bool PluginLoader::load(const std::string& path) {
#if defined(__unix__) || defined(__APPLE__)
  void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (handle == nullptr) {
    const char* err = ::dlerror();
    last_error_ = err != nullptr ? err : "dlopen failed";
    return false;
  }

  ::dlerror();  // clear any stale error before dlsym
  void* symbol = ::dlsym(handle, kPluginEntrySymbol);
  if (symbol == nullptr) {
    const char* err = ::dlerror();
    last_error_ = std::string("missing symbol '") + kPluginEntrySymbol +
                  "' in " + path + (err != nullptr ? std::string(": ") + err : "");
    ::dlclose(handle);
    return false;
  }

  auto entry = reinterpret_cast<ApexPluginRegisterFn>(symbol);
  entry();

  handles_.push_back(handle);
  paths_.push_back(path);
  last_error_.clear();
  return true;
#else
  (void)path;
  last_error_ = "dynamic loading unsupported on this platform";
  return false;
#endif
}

std::size_t PluginLoader::load_directory(const std::string& directory) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::is_directory(directory, ec)) {
    last_error_ = "not a directory: " + directory;
    return 0;
  }

  std::size_t loaded = 0;
  for (const auto& entry : fs::directory_iterator(directory, ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec)) continue;

    const std::string ext = entry.path().extension().string();
    const std::string filename = entry.path().filename().string();
    const bool candidate =
        ext == ".dylib" || ext == ".so" || filename.find(".so.") != std::string::npos;
    if (!candidate) continue;

    if (load(entry.path().string())) {
      ++loaded;
    }
  }
  return loaded;
}

void PluginLoader::unload_all() {
#if defined(__unix__) || defined(__APPLE__)
  for (void* handle : handles_) {
    if (handle != nullptr) ::dlclose(handle);
  }
#endif
  handles_.clear();
  paths_.clear();
}

}  // namespace apex
