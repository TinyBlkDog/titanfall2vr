#include "view_build_hook.h"

#include "diagnostics.h"
#include "hook_registry.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" volatile std::uintptr_t g_clientViewThis = 0;
extern "C" volatile std::uintptr_t g_clientDispatchThis = 0;
extern "C" volatile std::uintptr_t g_clientOuterThis = 0;
extern "C" std::uintptr_t g_clientViewInterceptContinue = 0;
extern "C" volatile std::uint64_t g_clientViewCallCount = 0;
extern "C" std::uintptr_t g_clientViewBuildOriginal = 0;
extern "C" std::uintptr_t g_clientViewPostBuild = 0;
extern "C" std::uintptr_t g_clientViewCallsiteContinue = 0;
extern "C" std::uintptr_t g_clientSlotPrepare = 0;
extern "C" std::uintptr_t g_clientSlotPrepareOriginalGlobal = 0;
extern "C" std::uintptr_t g_clientSlotPrepareContinue = 0;
extern "C" void clientViewBuildInterceptor();
extern "C" void clientViewCallsiteInterceptor();
extern "C" void clientViewSlotPrepareInterceptor();

namespace {
constexpr std::uintptr_t kClientViewBuildOffset = 0x35AEF0;
constexpr size_t kPatchLength = 20;
constexpr std::uintptr_t kClientViewCallsiteOffset = 0x35AB6D;
constexpr size_t kCallsitePatchLength = 14;
constexpr std::uintptr_t kClientSlotPrepareReturnOffset = 0x35A9C0;
constexpr size_t kSlotPreparePatchLength = 16;
std::uint8_t* g_patchSite = nullptr;
std::uint8_t g_original[kPatchLength]{};
std::uint8_t* g_callsitePatch = nullptr;
std::uint8_t g_callsiteOriginal[kCallsitePatchLength]{};
std::uint8_t* g_slotPreparePatch = nullptr;
std::uint8_t g_slotPrepareOriginal[kSlotPreparePatchLength]{};
std::atomic_uint32_t g_traceFramesRemaining = 0;
std::uint64_t g_traceStartCalls = 0;

bool IsClientAddress(const void* address, HMODULE client) {
    if (!address || !client) return false;
    const auto* base = reinterpret_cast<const std::uint8_t*>(client);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    return value >= start && value < start + nt->OptionalHeader.SizeOfImage;
}

bool IsReadableAddress(const void* address, size_t bytes) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!address || !VirtualQuery(address, &mbi, sizeof(mbi))) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto end = begin + bytes;
    const auto region_end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    const DWORD protect = mbi.Protect & 0xFF;
    return mbi.State == MEM_COMMIT && end >= begin && end <= region_end &&
        protect != PAGE_NOACCESS && protect != PAGE_GUARD;
}

void ModuleNameForAddress(const void* address, char* result, size_t result_size) {
    if (!result || !result_size) return;
    std::snprintf(result, result_size, "unknown");
    MEMORY_BASIC_INFORMATION mbi{};
    if (!address || !VirtualQuery(address, &mbi, sizeof(mbi)) || !mbi.AllocationBase) return;
    char full[MAX_PATH]{};
    if (!GetModuleFileNameA(static_cast<HMODULE>(mbi.AllocationBase), full, MAX_PATH)) return;
    const char* leaf = std::strrchr(full, '\\');
    std::snprintf(result, result_size, "%s", leaf ? leaf + 1 : full);
}
}

void EnsureClientViewBuildHookInstalled() {
    if (g_patchSite) return;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return;
    auto* site = reinterpret_cast<std::uint8_t*>(client) + kClientViewBuildOffset;
    constexpr std::uint8_t expected[] = {
        0x48, 0x8B, 0xC4, 0x55, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0x88,
        0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x68, 0x02, 0x00, 0x00,
    };
    if (std::memcmp(site, expected, sizeof(expected)) != 0) {
        Tf2VrLog("[TF2VR] Upstream CViewRender bytes differ; F4 trace not installed.\n");
        return;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(site, kPatchLength, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Tf2VrLog("[TF2VR] Upstream CViewRender hook VirtualProtect failed.\n");
        return;
    }
    std::memcpy(g_original, site, kPatchLength);
    std::uint8_t detour[kPatchLength] = {0xFF, 0x25, 0, 0, 0, 0};
    const auto target = reinterpret_cast<std::uintptr_t>(&clientViewBuildInterceptor);
    std::memcpy(detour + 6, &target, sizeof(target));
    std::memcpy(site, detour, sizeof(detour));
    std::memset(site + 14, 0x90, kPatchLength - 14);
    g_clientViewInterceptContinue = reinterpret_cast<std::uintptr_t>(site + kPatchLength);
    FlushInstructionCache(GetCurrentProcess(), site, kPatchLength);
    DWORD ignored = 0;
    VirtualProtect(site, kPatchLength, oldProtect, &ignored);
    g_patchSite = site;
    RegisterHookSite("view-build CViewRender patched entry", site, kPatchLength);
    Tf2VrLog("[TF2VR] Upstream CViewRender pass-through trace installed; F4 captures 120 frames.\n");
}

bool IsClientViewBuildHookInstalled() { return g_patchSite != nullptr; }

void RemoveClientViewBuildHook() {
    if (!g_patchSite) return;
    DWORD oldProtect = 0;
    if (VirtualProtect(g_patchSite, kPatchLength, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::memcpy(g_patchSite, g_original, kPatchLength);
        FlushInstructionCache(GetCurrentProcess(), g_patchSite, kPatchLength);
        DWORD ignored = 0;
        VirtualProtect(g_patchSite, kPatchLength, oldProtect, &ignored);
    }
    g_patchSite = nullptr;
    g_clientViewThis = 0;
    g_clientDispatchThis = 0;
    g_clientOuterThis = 0;
    g_clientViewInterceptContinue = 0;
}

void EnsureClientViewCallsiteHookInstalled() {
    if (g_callsitePatch) return;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return;
    auto* site = reinterpret_cast<std::uint8_t*>(client) + kClientViewCallsiteOffset;
    // mov rax,[rcx]; mov edx,1; call qword ptr [rax+0xA0]
    constexpr std::uint8_t expected[] = {0x48, 0x8B, 0x01, 0xBA, 0x01, 0x00, 0x00, 0x00, 0xFF, 0x90, 0xA0, 0x00, 0x00, 0x00};
    if (std::memcmp(site, expected, sizeof(expected)) != 0) {
        Tf2VrLog("[TF2VR] High-level client render dispatch bytes differ; F5 same-frame pair not installed.\n");
        return;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(site, kCallsitePatchLength, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Tf2VrLog("[TF2VR] High-level client render dispatch VirtualProtect failed.\n");
        return;
    }
    std::memcpy(g_callsiteOriginal, site, kCallsitePatchLength);
    // 13-byte absolute jump through volatile r11: mov r11, imm64; jmp r11,
    // followed by one NOP because the guarded original sequence is 14 bytes.
    std::uint8_t detour[kCallsitePatchLength] = {0x49, 0xBB};
    const auto target = reinterpret_cast<std::uintptr_t>(&clientViewCallsiteInterceptor);
    std::memcpy(detour + 2, &target, sizeof(target));
    detour[10] = 0x41; detour[11] = 0xFF; detour[12] = 0xE3;
    detour[13] = 0x90;
    g_clientViewBuildOriginal = reinterpret_cast<std::uintptr_t>(client) + kClientViewBuildOffset;
    g_clientViewCallsiteContinue = reinterpret_cast<std::uintptr_t>(site + kCallsitePatchLength);
    std::memcpy(site, detour, sizeof(detour));
    FlushInstructionCache(GetCurrentProcess(), site, kCallsitePatchLength);
    DWORD ignored = 0;
    VirtualProtect(site, kCallsitePatchLength, oldProtect, &ignored);
    g_callsitePatch = site;
    RegisterHookSite("view-build render-dispatch patched callsite", site, kCallsitePatchLength);
    Tf2VrLog("[TF2VR] High-level client render F5 dispatch installed; idle path is exact pass-through.\n");
}

void RemoveClientViewCallsiteHook() {
    if (!g_callsitePatch) return;
    DWORD oldProtect = 0;
    if (VirtualProtect(g_callsitePatch, kCallsitePatchLength, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::memcpy(g_callsitePatch, g_callsiteOriginal, kCallsitePatchLength);
        FlushInstructionCache(GetCurrentProcess(), g_callsitePatch, kCallsitePatchLength);
        DWORD ignored = 0;
        VirtualProtect(g_callsitePatch, kCallsitePatchLength, oldProtect, &ignored);
    }
    g_callsitePatch = nullptr;
    g_clientViewBuildOriginal = 0;
    g_clientViewCallsiteContinue = 0;
}

void EnsureClientViewSlotPrepareHookInstalled() {
    if (g_slotPreparePatch) return;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return;
    auto* site = reinterpret_cast<std::uint8_t*>(client) + kClientSlotPrepareReturnOffset;
    // Original continuation after CViewRender::PrepareSlot(outer, slot):
    // mov rcx,[rip+disp32]; mov rax,[rcx]; call qword ptr [rax+0x3B8]
    constexpr std::uint8_t expected[] = {
        0x48, 0x8B, 0x0D, 0xF9, 0x95, 0xAE, 0x02,
        0x48, 0x8B, 0x01, 0xFF, 0x90, 0xB8, 0x03, 0x00, 0x00,
    };
    if (std::memcmp(site, expected, sizeof(expected)) != 0) {
        Tf2VrLog("[TF2VR] Native slot-prepare continuation bytes differ; F5 slot test not installed.\n");
        return;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, site + 3, sizeof(displacement));
    const auto global_indirection = reinterpret_cast<std::uintptr_t>(site + 7 + displacement);
    if (!IsClientAddress(reinterpret_cast<const void*>(global_indirection), client)) {
        Tf2VrLog("[TF2VR] Native slot-prepare global indirection is outside client; F5 slot test not installed.\n");
        return;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(site, kSlotPreparePatchLength, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Tf2VrLog("[TF2VR] Native slot-prepare hook VirtualProtect failed.\n");
        return;
    }
    std::memcpy(g_slotPrepareOriginal, site, kSlotPreparePatchLength);
    std::uint8_t detour[kSlotPreparePatchLength] = {0xFF, 0x25, 0, 0, 0, 0};
    const auto target = reinterpret_cast<std::uintptr_t>(&clientViewSlotPrepareInterceptor);
    std::memcpy(detour + 6, &target, sizeof(target));
    std::memcpy(site, detour, sizeof(detour));
    std::memset(site + 14, 0x90, kSlotPreparePatchLength - 14);
    g_clientSlotPrepare = reinterpret_cast<std::uintptr_t>(client) + 0x35A500;
    g_clientSlotPrepareOriginalGlobal = global_indirection;
    g_clientSlotPrepareContinue = reinterpret_cast<std::uintptr_t>(site + kSlotPreparePatchLength);
    FlushInstructionCache(GetCurrentProcess(), site, kSlotPreparePatchLength);
    DWORD ignored = 0;
    VirtualProtect(site, kSlotPreparePatchLength, oldProtect, &ignored);
    g_slotPreparePatch = site;
    RegisterHookSite("view-build slot-prepare patched continuation", site, kSlotPreparePatchLength);
    Tf2VrLog("[TF2VR] Native CViewRender slot-prepare F5 hook installed; idle path replays the displaced continuation.\n");
}

void RemoveClientViewSlotPrepareHook() {
    if (!g_slotPreparePatch) return;
    DWORD oldProtect = 0;
    if (VirtualProtect(g_slotPreparePatch, kSlotPreparePatchLength, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::memcpy(g_slotPreparePatch, g_slotPrepareOriginal, kSlotPreparePatchLength);
        FlushInstructionCache(GetCurrentProcess(), g_slotPreparePatch, kSlotPreparePatchLength);
        DWORD ignored = 0;
        VirtualProtect(g_slotPreparePatch, kSlotPreparePatchLength, oldProtect, &ignored);
    }
    g_slotPreparePatch = nullptr;
    g_clientSlotPrepare = 0;
    g_clientSlotPrepareOriginalGlobal = 0;
    g_clientSlotPrepareContinue = 0;
}

void BeginClientViewBuildTrace() {
    EnsureClientViewBuildHookInstalled();
    if (!g_patchSite) return;
    g_traceStartCalls = g_clientViewCallCount;
    g_traceFramesRemaining.store(120, std::memory_order_release);
    Tf2VrLog("[TF2VR] F4: upstream CViewRender trace started for 120 plugin frames.\n");
}

void AdvanceClientViewBuildTrace() {
    const auto remaining = g_traceFramesRemaining.load(std::memory_order_acquire);
    if (!remaining || g_traceFramesRemaining.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    HMODULE client = GetModuleHandleA("client.dll");
    const auto view = reinterpret_cast<const void*>(g_clientViewThis);
    const auto dispatch = reinterpret_cast<const std::uintptr_t*>(g_clientDispatchThis);
    const auto outer = reinterpret_cast<const std::uintptr_t*>(g_clientOuterThis);
    std::uintptr_t vtable_address = 0;
    std::uintptr_t dispatch_slotA0 = 0;
    std::uintptr_t outer_slot30 = 0, outer_slot48 = 0, outer_slotA0 = 0;
    if (dispatch && IsClientAddress(dispatch, client)) {
        const auto vtable = reinterpret_cast<const std::uintptr_t*>(dispatch[0]);
        if (vtable && IsClientAddress(vtable, client)) {
            vtable_address = reinterpret_cast<std::uintptr_t>(vtable);
            outer_slot30 = vtable[0x30 / sizeof(std::uintptr_t)];
            outer_slot48 = vtable[0x48 / sizeof(std::uintptr_t)];
            outer_slotA0 = vtable[0xA0 / sizeof(std::uintptr_t)];
        }
    }
    const auto dispatch_vtable = dispatch && IsReadableAddress(dispatch, sizeof(std::uintptr_t))
        ? reinterpret_cast<const std::uintptr_t*>(dispatch[0]) : nullptr;
    if (dispatch_vtable && IsReadableAddress(dispatch_vtable, 0xA0 + sizeof(std::uintptr_t))) {
        dispatch_slotA0 = dispatch_vtable[0xA0 / sizeof(std::uintptr_t)];
    }
    char dispatch_module[MAX_PATH]{};
    char dispatch_target_module[MAX_PATH]{};
    ModuleNameForAddress(dispatch, dispatch_module, sizeof(dispatch_module));
    ModuleNameForAddress(reinterpret_cast<const void*>(dispatch_slotA0), dispatch_target_module, sizeof(dispatch_target_module));
    char line[800]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F4 result: upstream calls=%llu/120; builder-this=%p (client=%s); dispatch-rcx=%p (%s) vtable=%p target+A0=%p (%s); dispatch-rdi=%p; CViewRender vtable=%p targets +30=%p +48=%p +A0=%p.\n",
        static_cast<unsigned long long>(g_clientViewCallCount - g_traceStartCalls), view,
        IsClientAddress(view, client) ? "yes" : "no", dispatch, dispatch_module,
        dispatch_vtable, reinterpret_cast<const void*>(dispatch_slotA0), dispatch_target_module, outer,
        reinterpret_cast<const void*>(vtable_address),
        reinterpret_cast<const void*>(outer_slot30), reinterpret_cast<const void*>(outer_slot48),
        reinterpret_cast<const void*>(outer_slotA0));
    Tf2VrLog(line);
}
