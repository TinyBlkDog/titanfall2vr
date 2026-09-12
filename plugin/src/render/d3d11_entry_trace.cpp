#include "d3d11_entry_trace.h"

#include "diagnostics.h"
#include "hook_registry.h"

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using VSSetConstantBuffersFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);

constexpr size_t kVSSetConstantBuffersVtableIndex = 7;
constexpr size_t kDrawIndexedVtableIndex = 12;
constexpr size_t kDrawVtableIndex = 13;
constexpr size_t kDisplacedBytes = 16;
constexpr size_t kTrampolineBytes = 48;
constexpr std::uint8_t kExpectedPrefix[] = {
    0x48, 0x83, 0xEC, // sub rsp, imm8
};

struct EntryDetour {
    void* target = nullptr;
    void* trampoline = nullptr;
    std::uint8_t original[kDisplacedBytes]{};
};

EntryDetour g_indexedDetour;
EntryDetour g_drawDetour;
EntryDetour g_vsConstantBuffersDetour;
ID3D11DeviceContext* g_gameContext = nullptr;
DrawIndexedFn g_indexedOriginal = nullptr;
DrawFn g_drawOriginal = nullptr;
VSSetConstantBuffersFn g_vsConstantBuffersOriginal = nullptr;
std::atomic_bool g_enabled = false;
std::atomic_uint32_t g_inFlight = 0;
std::atomic_uint32_t g_framesRemaining = 0;
std::atomic_uint32_t g_indexedCount = 0;
std::atomic_uint32_t g_drawCount = 0;
std::atomic_uint32_t g_vsConstantBuffersCount = 0;
std::atomic_uint32_t g_vsSlotBindingCount[16]{};
std::atomic_uint32_t g_vsSlot576ByteBindingCount[16]{};
std::atomic<ID3D11Buffer*> g_cameraBufferCandidate = nullptr;

bool IsExpectedThunk(const std::uint8_t* code) {
    // Current Windows 11 D3D11 forwarding thunks are:
    // sub rsp,imm8; add rcx,-0xD8; call <internal>; ...
    // Validate instructions, but not the internal implementation address.
    return code && std::memcmp(code, kExpectedPrefix, sizeof(kExpectedPrefix)) == 0 &&
        (code[3] == 0x28 || code[3] == 0x38) &&
        code[4] == 0x48 && code[5] == 0x81 && code[6] == 0xC1 &&
        code[7] == 0x28 && code[8] == 0xFF && code[9] == 0xFF && code[10] == 0xFF &&
        code[11] == 0xE8;
}

void WriteAbsoluteJump(std::uint8_t* destination, const void* target) {
    const std::uint8_t jump[6] = {0xFF, 0x25, 0, 0, 0, 0};
    std::memcpy(destination, jump, sizeof(jump));
    const auto address = reinterpret_cast<std::uintptr_t>(target);
    std::memcpy(destination + sizeof(jump), &address, sizeof(address));
}

bool PrepareDetour(EntryDetour& detour, void* target) {
    auto* code = static_cast<std::uint8_t*>(target);
    if (!IsExpectedThunk(code)) return false;
    detour.trampoline = VirtualAlloc(nullptr, kTrampolineBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!detour.trampoline) return false;
    detour.target = target;
    std::memcpy(detour.original, code, kDisplacedBytes);

    // Copy the two whole non-relative instructions (11 bytes), then call the
    // original internal target through r11 (volatile) and resume at +16. The
    // original rel32 call cannot be copied verbatim into an arbitrary buffer.
    auto* trampoline = static_cast<std::uint8_t*>(detour.trampoline);
    std::memcpy(trampoline, detour.original, 11);
    trampoline[11] = 0x49; trampoline[12] = 0xBB; // mov r11, imm64
    const auto internal = reinterpret_cast<std::uintptr_t>(code + 16) +
        *reinterpret_cast<const std::int32_t*>(code + 12);
    std::memcpy(trampoline + 13, &internal, sizeof(internal));
    trampoline[21] = 0x41; trampoline[22] = 0xFF; trampoline[23] = 0xD3; // call r11
    WriteAbsoluteJump(trampoline + 24, code + kDisplacedBytes);
    FlushInstructionCache(GetCurrentProcess(), trampoline, kTrampolineBytes);

    return true;
}

bool PrepareVSConstantBuffersDetour(EntryDetour& detour, void* target) {
    auto* code = static_cast<std::uint8_t*>(target);
    // F2 on this exact driver build reported:
    // test r8d,r8d; jz rel32; mov rax,rsp; mov [rax+10h],rbx.
    // These four complete instructions occupy exactly 16 bytes.  The one
    // rel32 branch is rebuilt below as an inverted short branch over an
    // absolute jump, so the trampoline has no range dependency.
    if (!code || code[0] != 0x45 || code[1] != 0x85 || code[2] != 0xC0 ||
        code[3] != 0x0F || code[4] != 0x84 ||
        code[9] != 0x48 || code[10] != 0x8B || code[11] != 0xC4 ||
        code[12] != 0x48 || code[13] != 0x89 || code[14] != 0x58 || code[15] != 0x10) {
        return false;
    }
    detour.trampoline = VirtualAlloc(nullptr, kTrampolineBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!detour.trampoline) return false;
    detour.target = target;
    std::memcpy(detour.original, code, kDisplacedBytes);

    auto* trampoline = static_cast<std::uint8_t*>(detour.trampoline);
    // test r8d,r8d
    std::memcpy(trampoline, code, 3);
    // jnz +14: the nonzero path skips the following absolute jump.  In the
    // zero case that jump reaches the original jz destination.
    trampoline[3] = 0x0F; trampoline[4] = 0x85;
    const std::int32_t skipAbsoluteJump = 14;
    std::memcpy(trampoline + 5, &skipAbsoluteJump, sizeof(skipAbsoluteJump));
    const auto branchTarget = reinterpret_cast<std::uintptr_t>(code + 9) +
        *reinterpret_cast<const std::int32_t*>(code + 5);
    WriteAbsoluteJump(trampoline + 9, reinterpret_cast<const void*>(branchTarget));
    // Original nonzero path begins at +9: mov rax,rsp; mov [rax+10h],rbx.
    std::memcpy(trampoline + 23, code + 9, 7);
    WriteAbsoluteJump(trampoline + 30, code + kDisplacedBytes);
    FlushInstructionCache(GetCurrentProcess(), trampoline, kTrampolineBytes);
    return true;
}

bool CommitDetour(EntryDetour& detour, const void* hook, const char* registryName) {
    if (!detour.target || !detour.trampoline) return false;
    auto* code = static_cast<std::uint8_t*>(detour.target);
    DWORD oldProtect = 0;
    if (!VirtualProtect(code, kDisplacedBytes, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    WriteAbsoluteJump(code, hook);
    std::memset(code + 14, 0x90, kDisplacedBytes - 14);
    FlushInstructionCache(GetCurrentProcess(), code, kDisplacedBytes);
    DWORD ignored = 0; VirtualProtect(code, kDisplacedBytes, oldProtect, &ignored);
    // P0-c: name both executable ranges for the crash recorder.
    RegisterHookSite(registryName, detour.trampoline, kTrampolineBytes);
    RegisterHookSite(registryName, detour.target, kDisplacedBytes);
    return true;
}

void RestoreDetour(EntryDetour& detour) {
    if (detour.target) {
        DWORD oldProtect = 0;
        if (VirtualProtect(detour.target, kDisplacedBytes, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            std::memcpy(detour.target, detour.original, kDisplacedBytes);
            FlushInstructionCache(GetCurrentProcess(), detour.target, kDisplacedBytes);
            DWORD ignored = 0; VirtualProtect(detour.target, kDisplacedBytes, oldProtect, &ignored);
        }
    }
    if (detour.trampoline) VirtualFree(detour.trampoline, 0, MEM_RELEASE);
    detour = {};
}

void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* context, UINT indexCount, UINT startIndex, INT baseVertex) {
    g_inFlight.fetch_add(1, std::memory_order_acq_rel);
    if (g_enabled.load(std::memory_order_relaxed) && context == g_gameContext)
        g_indexedCount.fetch_add(1, std::memory_order_relaxed);
    g_indexedOriginal(context, indexCount, startIndex, baseVertex);
    g_inFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void STDMETHODCALLTYPE HookDraw(ID3D11DeviceContext* context, UINT vertexCount, UINT startVertex) {
    g_inFlight.fetch_add(1, std::memory_order_acq_rel);
    if (g_enabled.load(std::memory_order_relaxed) && context == g_gameContext)
        g_drawCount.fetch_add(1, std::memory_order_relaxed);
    g_drawOriginal(context, vertexCount, startVertex);
    g_inFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void STDMETHODCALLTYPE HookVSSetConstantBuffers(ID3D11DeviceContext* context, UINT startSlot, UINT bufferCount, ID3D11Buffer* const* buffers) {
    g_inFlight.fetch_add(1, std::memory_order_acq_rel);
    if (g_enabled.load(std::memory_order_relaxed) && context == g_gameContext) {
        g_vsConstantBuffersCount.fetch_add(1, std::memory_order_relaxed);
        // This is a bounded classification only.  It reads an immutable
        // descriptor while the caller-owned buffer pointer is valid; it never
        // retains, maps, copies, substitutes, or rebinds that buffer.
        for (UINT i = 0; buffers && i < bufferCount && startSlot + i < 16; ++i) {
            ID3D11Buffer* buffer = buffers[i];
            if (!buffer) continue;
            const UINT slot = startSlot + i;
            g_vsSlotBindingCount[slot].fetch_add(1, std::memory_order_relaxed);
            D3D11_BUFFER_DESC desc{};
            buffer->GetDesc(&desc);
            if (desc.ByteWidth == 576)
                g_vsSlot576ByteBindingCount[slot].fetch_add(1, std::memory_order_relaxed);
            // F1's previous run established that b2 is uniquely 576 bytes.
            // Retain one instance only so FinishTrace can make a single
            // post-trace staging read.  The game binding itself is passed on
            // untouched below.
            if (slot == 2 && desc.ByteWidth == 576) {
                ID3D11Buffer* expected = nullptr;
                if (g_cameraBufferCandidate.compare_exchange_strong(expected, buffer, std::memory_order_acq_rel))
                    buffer->AddRef();
            }
        }
    }
    g_vsConstantBuffersOriginal(context, startSlot, bufferCount, buffers);
    g_inFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void LogCameraBufferSnapshot() {
    ID3D11Buffer* source = g_cameraBufferCandidate.exchange(nullptr, std::memory_order_acq_rel);
    if (!source || !g_gameContext) { if (source) source->Release(); return; }
    D3D11_BUFFER_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    D3D11_BUFFER_DESC stagingDesc{};
    stagingDesc.ByteWidth = sourceDesc.ByteWidth;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    g_gameContext->GetDevice(&device);
    Microsoft::WRL::ComPtr<ID3D11Buffer> staging;
    const HRESULT createResult = device ? device->CreateBuffer(&stagingDesc, nullptr, &staging) : E_FAIL;
    if (FAILED(createResult)) {
        char line[220]{};
        std::snprintf(line, sizeof(line), "[TF2VR] F1: b2 camera-buffer staging allocation failed: 0x%08lX.\n", static_cast<unsigned long>(createResult));
        Tf2VrLog(line); source->Release(); return;
    }
    g_gameContext->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT mapResult = g_gameContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(mapResult)) {
        char line[220]{};
        std::snprintf(line, sizeof(line), "[TF2VR] F1: b2 camera-buffer staging map failed: 0x%08lX.\n", static_cast<unsigned long>(mapResult));
        Tf2VrLog(line); source->Release(); return;
    }
    // RenderDoc reflection fixes these offsets: origin begins at byte 4 and
    // cameraRelativeToClip begins at byte 16. Read only a compact witness,
    // not the full buffer, to avoid treating a diagnostic as a dump format.
    const auto* bytes = static_cast<const std::uint8_t*>(mapped.pData);
    float origin[3]{}; float matrixRow0[4]{};
    float matrix[16]{};
    std::memcpy(origin, bytes + 4, sizeof(origin));
    std::memcpy(matrixRow0, bytes + 16, sizeof(matrixRow0));
    std::memcpy(matrix, bytes + 16, sizeof(matrix));
    char line[360]{};
    std::snprintf(line, sizeof(line), "[TF2VR] F1: b2 576-byte buffer desc usage=%u bind=0x%X cpu=0x%X; origin=(%.6g,%.6g,%.6g) matrixRow0=(%.6g,%.6g,%.6g,%.6g).\n",
        static_cast<unsigned>(sourceDesc.Usage), static_cast<unsigned>(sourceDesc.BindFlags), static_cast<unsigned>(sourceDesc.CPUAccessFlags),
        origin[0], origin[1], origin[2], matrixRow0[0], matrixRow0[1], matrixRow0[2], matrixRow0[3]);
    Tf2VrLog(line);
    for (UINT row = 0; row < 4; ++row) {
        char matrixLine[240]{};
        std::snprintf(matrixLine, sizeof(matrixLine), "[TF2VR] F1: b2 raw matrix row %u=(%.6g,%.6g,%.6g,%.6g).\n", row,
            matrix[row * 4], matrix[row * 4 + 1], matrix[row * 4 + 2], matrix[row * 4 + 3]);
        Tf2VrLog(matrixLine);
    }
    g_gameContext->Unmap(staging.Get(), 0);
    source->Release();
}

void FinishTraceIfQuiescent() {
    if (g_inFlight.load(std::memory_order_acquire) != 0) return;
    RestoreDetour(g_indexedDetour);
    RestoreDetour(g_drawDetour);
    RestoreDetour(g_vsConstantBuffersDetour);
    LogCameraBufferSnapshot();
    if (g_gameContext) g_gameContext->Release();
    g_gameContext = nullptr; g_indexedOriginal = nullptr; g_drawOriginal = nullptr; g_vsConstantBuffersOriginal = nullptr;
    char line[280]{};
    std::snprintf(line, sizeof(line), "[TF2VR] F1: function-entry trace complete: VSSetConstantBuffers=%u DrawIndexed=%u Draw=%u over 120 plugin frames; hooks removed.\n",
        g_vsConstantBuffersCount.load(std::memory_order_acquire), g_indexedCount.load(std::memory_order_acquire), g_drawCount.load(std::memory_order_acquire));
    Tf2VrLog(line);
    for (UINT slot = 0; slot < 16; ++slot) {
        const auto bindings = g_vsSlotBindingCount[slot].load(std::memory_order_acquire);
        if (!bindings) continue;
        char slotLine[180]{};
        std::snprintf(slotLine, sizeof(slotLine), "[TF2VR] F1: VS b%u bindings=%u 576-byte=%u.\n", slot, bindings,
            g_vsSlot576ByteBindingCount[slot].load(std::memory_order_acquire));
        Tf2VrLog(slotLine);
    }
}
}

void BeginD3D11FunctionEntryTrace(ID3D11Device* device) {
    if (!device || g_gameContext || g_framesRemaining.load(std::memory_order_acquire)) {
        Tf2VrLog("[TF2VR] F1: function-entry trace unavailable/already active.\n"); return;
    }
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context) { Tf2VrLog("[TF2VR] F1: function-entry trace has no immediate context.\n"); return; }
    auto** vtable = *reinterpret_cast<void***>(context.Get());
    if (!vtable || !PrepareVSConstantBuffersDetour(g_vsConstantBuffersDetour, vtable[kVSSetConstantBuffersVtableIndex]) ||
        !PrepareDetour(g_indexedDetour, vtable[kDrawIndexedVtableIndex]) ||
        !PrepareDetour(g_drawDetour, vtable[kDrawVtableIndex])) {
        RestoreDetour(g_vsConstantBuffersDetour); RestoreDetour(g_indexedDetour); RestoreDetour(g_drawDetour);
        Tf2VrLog("[TF2VR] F1: function-entry guard/prepare failed; no trace started.\n"); return;
    }
    // Publish the complete trampoline targets before either entry is patched.
    // A concurrent draw can then only forward correctly, even before tracing
    // becomes enabled for this context.
    g_indexedOriginal = reinterpret_cast<DrawIndexedFn>(g_indexedDetour.trampoline);
    g_drawOriginal = reinterpret_cast<DrawFn>(g_drawDetour.trampoline);
    g_vsConstantBuffersOriginal = reinterpret_cast<VSSetConstantBuffersFn>(g_vsConstantBuffersDetour.trampoline);
    if (!CommitDetour(g_vsConstantBuffersDetour, reinterpret_cast<void*>(&HookVSSetConstantBuffers),
                      "d3d11-trace VSSetConstantBuffers") ||
        !CommitDetour(g_indexedDetour, reinterpret_cast<void*>(&HookDrawIndexed),
                      "d3d11-trace DrawIndexed") ||
        !CommitDetour(g_drawDetour, reinterpret_cast<void*>(&HookDraw),
                      "d3d11-trace Draw")) {
        g_enabled.store(false, std::memory_order_release);
        // Commit failure is expected to happen before a draw can execute on
        // this game thread. Do not block the game waiting for another thread;
        // if one did enter, leave its tiny trampoline allocated for process
        // lifetime rather than freeing executable code under it.
        if (g_inFlight.load(std::memory_order_acquire) == 0) {
            RestoreDetour(g_vsConstantBuffersDetour); RestoreDetour(g_indexedDetour); RestoreDetour(g_drawDetour);
        }
        Tf2VrLog("[TF2VR] F1: function-entry commit failed; no trace enabled.\n"); return;
    }
    g_gameContext = context.Detach();
    g_vsConstantBuffersCount.store(0, std::memory_order_release); g_indexedCount.store(0, std::memory_order_release); g_drawCount.store(0, std::memory_order_release);
    for (UINT slot = 0; slot < 16; ++slot) {
        g_vsSlotBindingCount[slot].store(0, std::memory_order_release);
        g_vsSlot576ByteBindingCount[slot].store(0, std::memory_order_release);
    }
    g_enabled.store(true, std::memory_order_release);
    g_framesRemaining.store(120, std::memory_order_release);
    Tf2VrLog("[TF2VR] F1: bounded D3D function-entry trace armed for 120 plugin frames; context vtable unchanged.\n");
}


void AdvanceD3D11FunctionEntryTrace() {
    const auto remaining = g_framesRemaining.load(std::memory_order_acquire);
    if (!remaining) {
        if (g_gameContext && !g_enabled.load(std::memory_order_acquire)) FinishTraceIfQuiescent();
        return;
    }
    if (g_framesRemaining.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    g_enabled.store(false, std::memory_order_release);
    g_framesRemaining.store(0, std::memory_order_release);
    FinishTraceIfQuiescent();
}

void RemoveD3D11FunctionEntryTrace() {
    g_enabled.store(false, std::memory_order_release);
    g_framesRemaining.store(0, std::memory_order_release);
    FinishTraceIfQuiescent();
}
