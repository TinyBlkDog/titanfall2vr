#include "resource_watch.h"

#include "diagnostics.h"

#include <atomic>
#include <cstdio>
#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace {

// ID3D11Device vtable, after the three IUnknown slots:
//   3 CreateBuffer   4 CreateTexture1D   5 CreateTexture2D   6 CreateTexture3D
//   7 CreateShaderResourceView
constexpr size_t kCreateBufferSlot = 3;
constexpr size_t kCreateTexture2DSlot = 5;
constexpr size_t kCreateShaderResourceViewSlot = 7;

using CreateBufferFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*,
                                                   const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
using CreateTexture2DFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                                      const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using CreateSrvFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*,
                                                const D3D11_SHADER_RESOURCE_VIEW_DESC*,
                                                ID3D11ShaderResourceView**);

CreateBufferFn g_createBuffer = nullptr;
CreateTexture2DFn g_createTexture2D = nullptr;
CreateSrvFn g_createSrv = nullptr;
ID3D11Device* g_watchedDevice = nullptr;

// Bounded by construction. A creation failure storm during a dying load could
// otherwise write thousands of lines and bury the first one, which is the only
// one that matters.
constexpr int kMaxFailureLines = 40;
std::atomic_int g_failureLines{0};
std::atomic<unsigned long long> g_failuresTotal{0};

// The overlap counter. See the header for why one minidump sample could not
// settle this.
std::atomic_int g_pacerInXr{0};
std::atomic<unsigned long long> g_overlaps{0};
std::atomic<unsigned long long> g_createsSeen{0};

void NoteResult(const char* what, HRESULT hr, const char* detail) {
    g_createsSeen.fetch_add(1, std::memory_order_relaxed);
    if (g_pacerInXr.load(std::memory_order_acquire) > 0) {
        g_overlaps.fetch_add(1, std::memory_order_relaxed);
    }
    if (SUCCEEDED(hr)) return;
    g_failuresTotal.fetch_add(1, std::memory_order_relaxed);
    if (g_failureLines.fetch_add(1, std::memory_order_relaxed) >= kMaxFailureLines) return;
    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] CREATE FAILED: %s returned 0x%08lX. %s  A creation that fails here and whose "
        "HRESULT the game does not check is how a null resource gets into a game structure, and a "
        "null resource is what the load crash dereferenced in Map.\n",
        what, static_cast<unsigned long>(hr), detail ? detail : "");
    Tf2VrLog(line);
}

HRESULT STDMETHODCALLTYPE HookCreateBuffer(ID3D11Device* device, const D3D11_BUFFER_DESC* desc,
                                           const D3D11_SUBRESOURCE_DATA* initial, ID3D11Buffer** out) {
    const HRESULT hr = g_createBuffer(device, desc, initial, out);
    if (FAILED(hr)) {
        char detail[200]{};
        if (desc) {
            std::snprintf(detail, sizeof(detail),
                "%u bytes, usage %u, bind 0x%X, cpuAccess 0x%X.",
                desc->ByteWidth, desc->Usage, desc->BindFlags, desc->CPUAccessFlags);
        }
        NoteResult("CreateBuffer", hr, detail);
    } else {
        NoteResult("CreateBuffer", hr, nullptr);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateTexture2D(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* desc,
                                              const D3D11_SUBRESOURCE_DATA* initial, ID3D11Texture2D** out) {
    const HRESULT hr = g_createTexture2D(device, desc, initial, out);
    if (FAILED(hr)) {
        char detail[200]{};
        if (desc) {
            std::snprintf(detail, sizeof(detail),
                "%ux%u, %u mips, %u slices, format %u, usage %u, bind 0x%X, samples %u.",
                desc->Width, desc->Height, desc->MipLevels, desc->ArraySize,
                static_cast<unsigned>(desc->Format), desc->Usage, desc->BindFlags,
                desc->SampleDesc.Count);
        }
        NoteResult("CreateTexture2D", hr, detail);
    } else {
        NoteResult("CreateTexture2D", hr, nullptr);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateSrv(ID3D11Device* device, ID3D11Resource* resource,
                                        const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,
                                        ID3D11ShaderResourceView** out) {
    const HRESULT hr = g_createSrv(device, resource, desc, out);
    NoteResult("CreateShaderResourceView", hr,
               resource ? nullptr : "the RESOURCE passed in was already null.");
    return hr;
}

}  // namespace

void NotifyPacerEnterXr() { g_pacerInXr.fetch_add(1, std::memory_order_release); }
void NotifyPacerLeaveXr() { g_pacerInXr.fetch_sub(1, std::memory_order_release); }

bool InstallResourceWatch(ID3D11Device* device) {
    if (!device) return false;
    if (g_watchedDevice == device && g_createBuffer) return true;

    void** vtable = *reinterpret_cast<void***>(device);
    if (!vtable) return false;
    void** slots = &vtable[kCreateBufferSlot];

    // Refuse if a slot already holds ours: reading our own hook back as the
    // original builds a call that recurses into itself, which this project has
    // already done once with a trampoline.
    if (vtable[kCreateBufferSlot] == reinterpret_cast<void*>(&HookCreateBuffer)) {
        g_watchedDevice = device;
        return true;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(slots, sizeof(void*) * 5, PAGE_READWRITE, &oldProtect)) {
        Tf2VrLog("[TF2VR] resource watch: the device vtable would not go writable; nothing hooked. "
                 "The load-crash instrument is NOT armed and a run with it is void.\n");
        return false;
    }
    g_createBuffer = reinterpret_cast<CreateBufferFn>(vtable[kCreateBufferSlot]);
    g_createTexture2D = reinterpret_cast<CreateTexture2DFn>(vtable[kCreateTexture2DSlot]);
    g_createSrv = reinterpret_cast<CreateSrvFn>(vtable[kCreateShaderResourceViewSlot]);
    vtable[kCreateBufferSlot] = reinterpret_cast<void*>(&HookCreateBuffer);
    vtable[kCreateTexture2DSlot] = reinterpret_cast<void*>(&HookCreateTexture2D);
    vtable[kCreateShaderResourceViewSlot] = reinterpret_cast<void*>(&HookCreateSrv);
    DWORD ignored = 0;
    VirtualProtect(slots, sizeof(void*) * 5, oldProtect, &ignored);
    g_watchedDevice = device;

    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] resource watch ARMED by vtable slot swap on the game device: CreateBuffer, "
        "CreateTexture2D, CreateShaderResourceView (originals %p / %p / %p). Every call is "
        "forwarded unchanged; only FAILURES are logged, capped at %d lines. FALSIFIER: the "
        "per-window line must show a non-zero creates count, or this instrument is watching "
        "nothing and its silence means nothing.\n",
        reinterpret_cast<void*>(g_createBuffer), reinterpret_cast<void*>(g_createTexture2D),
        reinterpret_cast<void*>(g_createSrv), kMaxFailureLines);
    Tf2VrLog(line);
    return true;
}

void LogResourceHealth(const char* tag) {
    // VRAM first: an allocation failing under pressure is the one mechanism
    // that ties the pacing thread -- which adds a stream of compositor work
    // through the load -- to a null resource without the two ever being on the
    // same stack.
    char vram[220]{};
    std::snprintf(vram, sizeof(vram), "video memory unavailable");
    if (g_watchedDevice) {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
        if (SUCCEEDED(g_watchedDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) &&
            SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter.As(&adapter3))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO local{};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) {
                std::snprintf(vram, sizeof(vram),
                    "VRAM %llu MB used of %llu MB budget (%.1f%%), reserved %llu MB",
                    local.CurrentUsage / (1024ull * 1024ull), local.Budget / (1024ull * 1024ull),
                    local.Budget ? 100.0 * local.CurrentUsage / local.Budget : 0.0,
                    local.CurrentReservation / (1024ull * 1024ull));
            }
        }
    }

    // GetDeviceRemovedReason BEFORE the game notices. Run 2 of this phase died
    // with DXGI_ERROR_DEVICE_REMOVED and no TDR in the Windows System log, and
    // there was no way to tell how long the device had already been dead.
    long removedReason = 0;
    if (g_watchedDevice) removedReason = static_cast<long>(g_watchedDevice->GetDeviceRemovedReason());

    // LOUD WHEN IT MATTERS, SILENT WHEN IT DOES NOT.
    //
    // This is the ONLY place a removed device is reported, so it must not be
    // droppable -- and until now it was kept in a quiet release log BY ACCIDENT,
    // because its own explanatory sentence contained the words "could not" and
    // that is a KeepWhenQuiet() needle. 229 routine lines survived a release run
    // on the strength of a phrase in their own prose.
    //
    // So the failure is forced through explicitly instead of riding on a word,
    // and the routine sample is left droppable. A run where nothing is wrong now
    // says nothing; a run where the device died, an allocation failed, or the
    // game and the runtime touched the device at once still says so in a quiet
    // release log -- and now says it because it was asked to.
    const unsigned long long failures = g_failuresTotal.load(std::memory_order_relaxed);
    const unsigned long long overlaps = g_overlaps.load(std::memory_order_relaxed);
    const bool wrong = failures != 0 || overlaps != 0 || removedReason != 0;

    char line[620]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] RESOURCE HEALTH [%s]: %s | creates seen %llu, failures %llu | pacer/game D3D "
        "overlaps %llu | device removed reason 0x%08lX.%s\n",
        tag ? tag : "", vram,
        g_createsSeen.load(std::memory_order_relaxed),
        failures, overlaps,
        static_cast<unsigned long>(removedReason),
        wrong ? "  SOMETHING IS WRONG: a non-zero failure, overlap or removed reason. An overlap "
                "means the game was creating a resource while the runtime was inside an XR call "
                "on the same device."
              : "");
    Tf2VrLogWrite(line, wrong);
}

// ---------------------------------------------------------------------------
// ID3D11Multithread, and the evidence that decides whether it was needed.
// ---------------------------------------------------------------------------

namespace {
std::atomic_bool g_mtProtected{false};
std::atomic<unsigned long long> g_contextOverlaps{0};
std::atomic<unsigned long long> g_contextCalls{0};
}  // namespace

// Called from the game's own immediate-context work (the UpdateSubresource
// hook). THIS is the overlap that matters.
//
// The create-call counter above overlaps device methods, and D3D11 device
// creation is FREE-THREADED -- those overlaps are legal and prove only that
// two things were busy at once. The immediate CONTEXT is the object that is
// not thread-safe, and run 6's crash was Map on the immediate context from a
// tier0 worker thread. A non-zero count here is the illegal pattern itself.
void NoteGameContextCall() {
    g_contextCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_pacerInXr.load(std::memory_order_acquire) > 0) {
        g_contextOverlaps.fetch_add(1, std::memory_order_relaxed);
    }
}

// xr.mt_protect. DEFAULT OFF, and the default is a measured result rather than
// caution: turning it on broke the camera hook and the run rendered flat.
std::atomic_bool g_mtWanted{false};

void SetMultithreadProtectionWanted(bool wanted) { g_mtWanted.store(wanted, std::memory_order_release); }

bool MaybeEnableMultithreadProtection(ID3D11Device* device) {
    if (!device) return false;
    if (!g_mtWanted.load(std::memory_order_acquire)) {
        Tf2VrLog("[TF2VR] MT PROTECT: not requested (xr.mt_protect = 0). Turning it on moved the "
                 "UpdateSubresource implementation behind vtable slot 48 to a prologue the byte "
                 "matcher does not know, so the camera hook never installed, stereo never armed and "
                 "the whole session rendered flat. Do not turn it on again without teaching "
                 "PrepareDetour that prologue first.\n");
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context) return false;
    void** before = *reinterpret_cast<void***>(context.Get());
    // THE SLOT, NOT THE VTABLE POINTER.
    //
    // The first version of this logged the vtable POINTER either side and
    // reported "UNCHANGED", which read as reassuring and was worthless: what
    // SetMultithreadProtected moves is the IMPLEMENTATION each slot points at,
    // in place. Slot 48 went from 816246D0 to 81696240 -- a different function
    // with a different prologue -- while the vtable pointer never moved. An
    // instrument that watches the wrong object reports success either way.
    void* slot48Before = before ? before[48] : nullptr;

    Microsoft::WRL::ComPtr<ID3D11Multithread> mt;
    if (FAILED(context.As(&mt)) || !mt) {
        Tf2VrLog("[TF2VR] MT PROTECT: the immediate context does not expose ID3D11Multithread; the "
                 "pacing thread cannot be made safe this way and decouple must stay off.\n");
        return false;
    }
    const BOOL was = mt->SetMultithreadProtected(TRUE);
    g_mtProtected.store(true, std::memory_order_release);
    void** after = *reinterpret_cast<void***>(context.Get());

    void* slot48After = after ? after[48] : nullptr;
    char line[700]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] MT PROTECT armed: SetMultithreadProtected(TRUE), previously %s. Immediate-context "
        "vtable %p -> %p (%s), and SLOT 48 %p -> %p (%s). The slot is the one that matters: the "
        "vtable pointer does not move, the implementations behind it do. FALSIFIER: the STEREO line "
        "must still report a non-zero offset count, or the camera hook resolved against a prologue "
        "it does not know and the run is void.\n",
        was ? "ON" : "OFF", static_cast<void*>(before), static_cast<void*>(after),
        before == after ? "unchanged" : "changed",
        slot48Before, slot48After,
        slot48Before == slot48After ? "UNCHANGED" : "CHANGED -- the camera hook will not install");
    Tf2VrLog(line);
    return true;
}

void LogMultithreadStatus() {
    char line[560]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] MT STATUS: protection %s | game immediate-context calls %llu, of which %llu "
        "overlapped an XR call on the pacing thread. A non-zero overlap is concurrent use of the "
        "one object in D3D11 that is not thread-safe, and it is the pattern run 6 died inside. Zero "
        "over a whole load retires the concurrency theory by measurement.\n",
        g_mtProtected.load(std::memory_order_acquire) ? "ON" : "OFF",
        g_contextCalls.load(std::memory_order_relaxed),
        g_contextOverlaps.load(std::memory_order_relaxed));
    Tf2VrLog(line);
}
