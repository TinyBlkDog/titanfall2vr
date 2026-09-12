#include "render_hook.h"

#include "diagnostics.h"
#include "d3d11_trace.h"
#include "hook_registry.h"
#include "present_hook.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" std::uintptr_t g_renderResolver = 0;
extern "C" volatile std::uintptr_t g_renderResolvedObject = 0;
extern "C" volatile std::uintptr_t g_renderVtableTarget = 0;
extern "C" volatile std::uint64_t g_renderCallCount = 0;
extern "C" volatile std::uint32_t g_renderLastArgument = 0;
extern "C" volatile std::uint64_t g_renderArgumentMask = 0;
extern "C" volatile std::uintptr_t g_renderReturnAddress = 0;
extern "C" volatile std::uint32_t g_nativeSlotCaptureArmed = 0;
extern "C" volatile std::uint32_t g_nativeSlotPreMask = 0;
extern "C" volatile std::uint32_t g_nativeSlotActive = 0;
extern "C" void renderEntryInterceptor();

namespace {
constexpr std::uintptr_t kWrapperOffset = 0xDD150;
constexpr std::uintptr_t kResolverOffset = 0x247650;
constexpr size_t kPatchLength = 14;
std::uint8_t* g_patchSite = nullptr;
std::uint8_t g_original[kPatchLength]{};
std::atomic_uint32_t g_traceFramesRemaining = 0;
std::uint64_t g_traceStartCalls = 0;

bool IsEngineAddress(const void* address, HMODULE engine) {
    if (!address || !engine) return false;
    auto* base = reinterpret_cast<const std::uint8_t*>(engine);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    return value >= start && value < start + nt->OptionalHeader.SizeOfImage;
}
}

void EnsureRenderHookInstalled() {
    if (g_patchSite) return;
    HMODULE engine = GetModuleHandleA("engine.dll");
    if (!engine) return;
    auto* site = reinterpret_cast<std::uint8_t*>(engine) + kWrapperOffset;
    // Guard the documented entry bytes before modifying anything.
    constexpr std::uint8_t expected[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xDA, 0xE8};
    if (std::memcmp(site, expected, sizeof(expected)) != 0) {
        Tf2VrLog("[TF2VR] Task 3 wrapper bytes differ; render trace not installed.\n"); return;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(site, kPatchLength, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Tf2VrLog("[TF2VR] Task 3 render hook VirtualProtect failed.\n"); return;
    }
    std::memcpy(g_original, site, kPatchLength);
    std::uint8_t detour[kPatchLength] = {0xFF, 0x25, 0, 0, 0, 0};
    const auto target = reinterpret_cast<std::uintptr_t>(&renderEntryInterceptor);
    std::memcpy(detour + 6, &target, sizeof(target));
    g_renderResolver = reinterpret_cast<std::uintptr_t>(engine) + kResolverOffset;
    std::memcpy(site, detour, sizeof(detour));
    FlushInstructionCache(GetCurrentProcess(), site, kPatchLength);
    DWORD ignored = 0; VirtualProtect(site, kPatchLength, oldProtect, &ignored);
    g_patchSite = site;
    RegisterHookSite("render-hook engine wrapper patched entry", site, kPatchLength);
    Tf2VrLog("[TF2VR] Task 3 engine render-wrapper trace installed; F6 captures 120 frames.\n");
}

void RemoveRenderHook() {
    if (!g_patchSite) return;
    DWORD oldProtect = 0;
    if (VirtualProtect(g_patchSite, kPatchLength, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::memcpy(g_patchSite, g_original, kPatchLength);
        FlushInstructionCache(GetCurrentProcess(), g_patchSite, kPatchLength);
        DWORD ignored = 0; VirtualProtect(g_patchSite, kPatchLength, oldProtect, &ignored);
    }
    g_patchSite = nullptr;
}

void BeginRenderTrace() {
    EnsureRenderHookInstalled();
    if (!g_patchSite) return;
    g_traceStartCalls = g_renderCallCount;
    g_renderArgumentMask = 0;
    g_traceFramesRemaining.store(120, std::memory_order_release);
    Tf2VrLog("[TF2VR] F6: Task 3 render trace started for 120 plugin frames.\n");
}


extern "C" void CaptureNativeSlotPreOutput(std::uint32_t slot) {
    const std::uint32_t bit = slot == 1 ? 1u : slot == 2 ? 2u : 0u;
    if (!bit || (g_nativeSlotCaptureArmed & 1u) == 0) return;
    auto pre = std::atomic_ref<volatile std::uint32_t>(g_nativeSlotPreMask);
    std::uint32_t seen = pre.load(std::memory_order_acquire);
    if ((seen & bit) != 0 || !pre.compare_exchange_strong(seen, seen | bit, std::memory_order_acq_rel)) return;
    const char* filename = slot == 1 ? "titanfall2vr-native-slot1-before.bmp" : "titanfall2vr-native-slot2-before.bmp";
    BeginNativeModeDrawTrace(slot);
    const bool dumped = DumpVerifiedGameBackbuffer(filename);
    char line[240]{};
    std::snprintf(line, sizeof(line), "[TF2VR] F12: native slot %u pre-dispatch capture %s (%s).\n",
        slot, dumped ? "written" : "failed", filename);
    Tf2VrLog(line);
}

extern "C" void CaptureNativeSlotOutput(std::uint32_t slot) {
    auto state = std::atomic_ref<volatile std::uint32_t>(g_nativeSlotCaptureArmed);
    std::uint32_t value = state.load(std::memory_order_acquire);
    const std::uint32_t bit = slot == 1 ? 2u : slot == 2 ? 4u : 0u;
    if (bit && (value & 1u) != 0 && (value & bit) == 0) {
        const std::uint32_t desired = value | bit;
        if (state.compare_exchange_strong(value, desired, std::memory_order_acq_rel)) {
            const char* filename = slot == 1 ? "titanfall2vr-native-slot1.bmp" : "titanfall2vr-native-slot2.bmp";
            const bool dumped = DumpVerifiedGameBackbuffer(filename);
            EndNativeModeDrawTrace(slot);
            char line[220]{};
            std::snprintf(line, sizeof(line), "[TF2VR] F12: native slot %u post-dispatch capture %s (%s).\n",
                slot, dumped ? "written" : "failed", filename);
            Tf2VrLog(line);
            if (desired == 7) {
                state.store(0, std::memory_order_release);
                Tf2VrLog("[TF2VR] F12: native slot capture complete.\n");
            }
        }
    }
}

void AdvanceRenderTrace() {
    const auto remaining = g_traceFramesRemaining.load(std::memory_order_acquire);
    if (!remaining) return;
    if (g_traceFramesRemaining.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    HMODULE engine = GetModuleHandleA("engine.dll");
    HMODULE client = GetModuleHandleA("client.dll");
    const auto object = reinterpret_cast<const void*>(g_renderResolvedObject);
    const auto target = reinterpret_cast<const void*>(g_renderVtableTarget);
    const auto caller = reinterpret_cast<const void*>(g_renderReturnAddress);
    const char* caller_module = IsEngineAddress(caller, engine) ? "engine" : IsEngineAddress(caller, client) ? "client" : "other";
    const auto caller_base = IsEngineAddress(caller, engine) ? reinterpret_cast<std::uintptr_t>(engine) :
        IsEngineAddress(caller, client) ? reinterpret_cast<std::uintptr_t>(client) : 0ull;
    char line[460]{};
    std::snprintf(line, sizeof(line), "[TF2VR] Task 3 result: wrapper calls=%llu over 120 frames; edx last=%u mask(mod64)=0x%016llX; object=%p; vtable+0xB8=%p (engine=%s, +0x%llX); caller=%p (%s+0x%llX).\n",
        static_cast<unsigned long long>(g_renderCallCount - g_traceStartCalls),
        static_cast<unsigned>(g_renderLastArgument), static_cast<unsigned long long>(g_renderArgumentMask), object, target,
        IsEngineAddress(target, engine) ? "yes" : "no",
        engine && target ? static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(target) - reinterpret_cast<std::uintptr_t>(engine)) : 0ull,
        caller, caller_module,
        caller_base ? static_cast<unsigned long long>(g_renderReturnAddress - caller_base) : 0ull);
    Tf2VrLog(line);
}
