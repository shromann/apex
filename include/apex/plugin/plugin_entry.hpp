#pragma once

// Convenience header for plugin authors. Place APEX_PLUGIN_ENTRY(); in exactly
// one translation unit of the plugin library. It defines the C-linkage symbol
// the PluginLoader resolves:
//
//   extern "C" void apex_plugin_register() {}
//
// Strategies in the plugin self-register with APEX_REGISTER_STRATEGY; defining
// the entry point here guarantees their registration code is linked in and not
// discarded by the linker.

#define APEX_PLUGIN_ENTRY() extern "C" void apex_plugin_register()
