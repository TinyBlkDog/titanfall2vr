#pragma once

#include <cstdint>
#include <windows.h>

// Kept local so this plugin is independently buildable. These declarations
// mirror NorthstarLauncher/primedev/plugins/interfaces for PluginId001 and
// PluginCallbacks001; do not reorder virtual functions.
//
// ATTRIBUTION. The struct layouts and the virtual-function order below are
// derived from NorthstarLauncher (MIT),
// https://github.com/R2Northstar/NorthstarLauncher. See THIRD_PARTY_NOTICES.md.
enum class PluginString : int { NAME, LOG_NAME, DEPENDENCY_NAME };
enum class PluginField : int { CONTEXT, COLOR };
namespace PluginContext { constexpr std::uint64_t CLIENT = 0x2; }

struct PluginNorthstarData { HMODULE pluginHandle; std::uint64_t size; };
struct CSquirrelVM;

class IPluginId {
public:
    virtual const char* GetString(PluginString prop) = 0;
    virtual std::int64_t GetField(PluginField prop) = 0;
};

class IPluginCallbacks {
public:
    virtual void Init(HMODULE northstarModule, const PluginNorthstarData* initData, bool reloaded) = 0;
    virtual void Finalize() = 0;
    virtual bool Unload() = 0;
    virtual void OnSqvmCreated(CSquirrelVM* sqvm) = 0;
    virtual void OnSqvmDestroying(CSquirrelVM* sqvm) = 0;
    virtual void OnLibraryLoaded(HMODULE module, const char* name) = 0;
    virtual void RunFrame() = 0;
};
