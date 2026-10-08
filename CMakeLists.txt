cmake_minimum_required(VERSION 3.20)
project(apex VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Keep every runtime artifact in one directory so the plugin loader can find
# libapex_core next to the executable via the loader path.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

find_package(Threads REQUIRED)
# OpenSSL backs the in-tree TLS client and Webull's request signature.
find_package(OpenSSL REQUIRED)
if(NOT OpenSSL_FOUND)
  message(FATAL_ERROR "apex requires OpenSSL for the Webull adapter (TLS + signing)")
endif()

set(APEX_WARNINGS -Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# apex_core: the engine. Built as a shared library so that strategies loaded
# at runtime from a plugin share a single StrategyRegistry instance with the
# engine.
# ---------------------------------------------------------------------------
add_library(apex_core SHARED
  src/core/log.cpp
  src/strategy/strategy_registry.cpp
  src/plugin/plugin_loader.cpp
  src/engine/engine.cpp
  src/util/json.cpp
  src/net/tls_socket.cpp
  src/net/http_client.cpp
  src/net/mqtt_client.cpp
  src/net/grpc_client.cpp
  src/adapters/mock/mock_market_data_feed.cpp
  src/adapters/mock/mock_broker_gateway.cpp
  src/adapters/webull/webull_crypto.cpp
  src/adapters/webull/webull_signature.cpp
  src/adapters/webull/webull_config.cpp
  src/adapters/webull/webull_rest_client.cpp
  src/adapters/webull/webull_market_data_feed.cpp
  src/adapters/webull/webull_broker_gateway.cpp
)

target_include_directories(apex_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(apex_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(apex_core PRIVATE ${APEX_WARNINGS})
target_link_libraries(apex_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS} OpenSSL::SSL OpenSSL::Crypto)
set_target_properties(apex_core PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  BUILD_RPATH "@loader_path;${CMAKE_BINARY_DIR}/bin"
  INSTALL_RPATH "@loader_path"
  BUILD_WITH_INSTALL_RPATH FALSE
)

# ---------------------------------------------------------------------------
# apex: the runnable engine executable, with the built-in strategies.
# ---------------------------------------------------------------------------
add_executable(apex
  src/main.cpp
  src/strategies/ma_cross_strategy.cpp
)
target_link_libraries(apex PRIVATE apex_core)
target_compile_options(apex PRIVATE ${APEX_WARNINGS})
set_target_properties(apex PROPERTIES
  BUILD_RPATH "@loader_path;${CMAKE_BINARY_DIR}/bin"
)

# ---------------------------------------------------------------------------
# apex_momentum: an example strategy compiled as a runtime-loadable plugin.
# ---------------------------------------------------------------------------
add_library(apex_momentum MODULE
  strategies/momentum/momentum_strategy.cpp
)
target_link_libraries(apex_momentum PRIVATE apex_core)
target_compile_options(apex_momentum PRIVATE ${APEX_WARNINGS})
set_target_properties(apex_momentum PROPERTIES
  PREFIX "lib"
  OUTPUT_NAME "apex_momentum"
  BUILD_RPATH "@loader_path;${CMAKE_BINARY_DIR}/bin"
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

add_executable(apex_strategy_registry_test tests/strategy_registry_test.cpp)
target_link_libraries(apex_strategy_registry_test PRIVATE apex_core)
target_compile_options(apex_strategy_registry_test PRIVATE ${APEX_WARNINGS})
add_test(NAME strategy_registry COMMAND apex_strategy_registry_test)

add_executable(apex_engine_test tests/engine_test.cpp)
target_link_libraries(apex_engine_test PRIVATE apex_core)
target_compile_options(apex_engine_test PRIVATE ${APEX_WARNINGS})
add_test(NAME engine COMMAND apex_engine_test)

add_executable(apex_webull_test tests/webull_test.cpp)
target_link_libraries(apex_webull_test PRIVATE apex_core)
target_include_directories(apex_webull_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(apex_webull_test PRIVATE ${APEX_WARNINGS})
add_test(NAME webull COMMAND apex_webull_test)

add_executable(apex_plugin_test tests/plugin_test.cpp)
target_link_libraries(apex_plugin_test PRIVATE apex_core)
target_compile_options(apex_plugin_test PRIVATE ${APEX_WARNINGS})
add_test(NAME plugin COMMAND apex_plugin_test)
set_tests_properties(plugin PROPERTIES
  ENVIRONMENT "APEX_MOMENTUM_PLUGIN=$<TARGET_FILE:apex_momentum>"
)
