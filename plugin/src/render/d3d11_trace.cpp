#include "d3d11_trace.h"

#include "diagnostics.h"

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

extern "C" volatile std::uint32_t g_nativeSlotActive;

namespace {
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
// IUnknown occupies 0..2 and ID3D11DeviceChild occupies 3..6.  The
// ID3D11DeviceContext methods therefore start at 7: VS constants (7), PS
// resources (8), PS shader (9), PS samplers (10), VS shader (11), then draw.
constexpr size_t kVSSetConstantBuffersVtableIndex = 7;
constexpr size_t kDrawIndexedVtableIndex = 12;
constexpr size_t kDrawVtableIndex = 13;
constexpr size_t kUpdateSubresourceVtableIndex = 48;
constexpr size_t kContextVtableEntries = 128;

ID3D11DeviceContext* g_gameContext = nullptr;
void** g_originalVtable = nullptr;
void** g_hookVtable = nullptr;
DrawIndexedFn g_originalDrawIndexed = nullptr;
DrawFn g_originalDraw = nullptr;
std::atomic_uint32_t g_indexed[3]{};
std::atomic_uint32_t g_draw[3]{};

void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* context, UINT indexCount, UINT startIndex, INT baseVertex) {
    const auto mode = g_nativeSlotActive;
    if (context == g_gameContext && mode >= 1 && mode <= 2) g_indexed[mode].fetch_add(1, std::memory_order_relaxed);
    g_originalDrawIndexed(context, indexCount, startIndex, baseVertex);
}

void STDMETHODCALLTYPE HookDraw(ID3D11DeviceContext* context, UINT vertexCount, UINT startVertex) {
    const auto mode = g_nativeSlotActive;
    if (context == g_gameContext && mode >= 1 && mode <= 2) g_draw[mode].fetch_add(1, std::memory_order_relaxed);
    g_originalDraw(context, vertexCount, startVertex);
}

void LogFunctionEntry(const char* name, const void* entry) {
    if (!entry) { Tf2VrLog("[TF2VR] F2: null D3D draw entry.\n"); return; }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(entry, &mbi, sizeof(mbi)) != sizeof(mbi) ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        Tf2VrLog("[TF2VR] F2: D3D draw entry is not readable.\n"); return;
    }
    const auto available = static_cast<size_t>(reinterpret_cast<const std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize -
                                               reinterpret_cast<const std::uint8_t*>(entry));
    const size_t count = available < 24 ? available : 24;
    std::uint8_t bytes[24]{};
    std::memcpy(bytes, entry, count);
    HMODULE module = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(entry), &module);
    char path[MAX_PATH]{};
    if (module) GetModuleFileNameA(module, path, MAX_PATH);
    char hex[24 * 3 + 1]{};
    size_t used = 0;
    for (size_t i = 0; i < count && used + 4 < sizeof(hex); ++i)
        used += static_cast<size_t>(std::snprintf(hex + used, sizeof(hex) - used, "%02X ", bytes[i]));
    char line[760]{};
    std::snprintf(line, sizeof(line), "[TF2VR] F2: %s entry=%p module=%s rva=0x%llX bytes=%s\n", name, entry,
        module ? path : "(unknown)", module ? static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(entry) - reinterpret_cast<std::uintptr_t>(module)) : 0ull, hex);
    Tf2VrLog(line);
}
}


void RemoveD3D11DrawTraceHooks() {
    if (g_gameContext && g_hookVtable && g_originalVtable &&
        *reinterpret_cast<void***>(g_gameContext) == g_hookVtable) {
        DWORD protect = 0, ignored = 0;
        auto*** objectVtable = reinterpret_cast<void***>(g_gameContext);
        if (VirtualProtect(objectVtable, sizeof(void*), PAGE_READWRITE, &protect)) {
            *objectVtable = g_originalVtable;
            VirtualProtect(objectVtable, sizeof(void*), protect, &ignored);
        }
    }
    if (g_gameContext) g_gameContext->Release();
    delete[] g_hookVtable;
    g_gameContext = nullptr; g_originalVtable = nullptr; g_hookVtable = nullptr;
    g_originalDrawIndexed = nullptr; g_originalDraw = nullptr;
}

void ProbeD3D11DrawFunctionEntries(ID3D11Device* device) {
    if (!device) { Tf2VrLog("[TF2VR] F2: draw-entry probe skipped; no game device.\n"); return; }
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context) { Tf2VrLog("[TF2VR] F2: draw-entry probe skipped; no immediate context.\n"); return; }
    auto** vtable = *reinterpret_cast<void***>(context.Get());
    if (!vtable) { Tf2VrLog("[TF2VR] F2: draw-entry probe skipped; null context vtable.\n"); return; }
    Tf2VrLog("[TF2VR] F2: read-only D3D function-entry probe (no hook installed).\n");
    LogFunctionEntry("VSSetConstantBuffers", vtable[kVSSetConstantBuffersVtableIndex]);
    LogFunctionEntry("DrawIndexed", vtable[kDrawIndexedVtableIndex]);
    LogFunctionEntry("Draw", vtable[kDrawVtableIndex]);
    LogFunctionEntry("UpdateSubresource", vtable[kUpdateSubresourceVtableIndex]);
}

void BeginNativeModeDrawTrace(unsigned int mode) {
    if (mode < 1 || mode > 2) return;
    g_indexed[mode].store(0, std::memory_order_release);
    g_draw[mode].store(0, std::memory_order_release);
}

void EndNativeModeDrawTrace(unsigned int mode) {
    if (mode < 1 || mode > 2) return;
    char line[220]{};
    std::snprintf(line, sizeof(line), "[TF2VR] F12: native mode %u direct D3D draws: indexed=%u nonindexed=%u.\n", mode,
        g_indexed[mode].load(std::memory_order_acquire), g_draw[mode].load(std::memory_order_acquire));
    Tf2VrLog(line);
}
