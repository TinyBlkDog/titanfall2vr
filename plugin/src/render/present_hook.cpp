#include "present_hook.h"

#include "xr_breadcrumb.h"
#include "resource_watch.h"
#include "plugin_cost.h"
#include "menu_overlay.h"
#include "camera_update_hook.h"
#include "d3d11_trace.h"
#include "d3d11_entry_trace.h"
#include "diagnostics.h"
#include "viewmodel_bones.h"
#include "stereo_experiment.h"
#include "xr_context.h"
#include "stereo_targets.h"
#include "view_block_camera.h"
#include "temporal_lever.h"

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <atomic>
#include <mutex>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cmath>

// Presented-frame count for the whole session, defined here because this is the
// only place that sees every frame. Read from a VEH handler in
// placement_watchpoint.cpp, so it is deliberately a plain volatile word.
extern "C" volatile std::uint32_t g_tf2vrPresentFrame = 0;

namespace {
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
constexpr size_t kPresentVtableIndex = 8;

std::mutex g_mutex;
std::mutex g_xrMutex;
PresentFn g_originalPresent = nullptr;
void** g_presentSlot = nullptr;
XrContext g_xr;

std::atomic_bool g_xrArmed = false;
std::atomic_bool g_headLookArmed = false;
std::atomic_bool g_unloading = false;
std::atomic_uint32_t g_presentInFlight = 0;
Microsoft::WRL::ComPtr<IDXGISwapChain> g_gameSwapchain;
Microsoft::WRL::ComPtr<ID3D11Device> g_gameDevice;

// EVERY CAPTURE REPORTS WHAT IT COST, because one of these was hiding inside a
// level-entry freeze for two days.
//
// The Map(D3D11_MAP_READ) below is a BARRIER, not a copy: it blocks this thread
// until the GPU has retired everything queued ahead of it. Called from Present
// during a level load that is seconds of GPU work, and the game -- input
// included -- waits for all of it. The bytes are incidental; the wait is the
// cost, which is why halving the render resolution refuted nothing.
//
// So the phases are timed and printed. A capture that cost 6 ms and a capture
// that cost 6000 ms produced identical log lines before this, and the
// difference is the whole defect.
struct CaptureTiming {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER t0{}, tStaging{}, tCopy{}, tMap{}, tWrite{};
    void Start() {
        if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&t0);
    }
    double Ms(const LARGE_INTEGER& from, const LARGE_INTEGER& to) const {
        if (!frequency.QuadPart) return 0.0;
        return static_cast<double>(to.QuadPart - from.QuadPart) * 1000.0 /
               static_cast<double>(frequency.QuadPart);
    }
};

// SAME-FRAME STEREO NEEDS TO DUMP A TEXTURE, NOT A SWAPCHAIN. Eye 0 lives in
// the scene target, copied out at the mid-frame boundary; it never reaches a
// swapchain at all. Everything below the old GetBuffer was already generic, so
// this is the same function with its source generalised, and DumpBackbuffer
// below is now two lines that fetch buffer 0 and call it.
bool DumpTexture(ID3D11Texture2D* source, const char* filename) {
    CaptureTiming timing;
    timing.Start();
    if (!source) return false;
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    const bool is_32bpp_bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool is_32bpp_rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    // THE SCENE TARGET IS R10G10B10A2, and refusing it cost a run. The backbuffer
    // is 8-bit BGRA but the target the engine renders the world into is
    // 2560x1440 format 9 -- still 32 bits per pixel, just packed as ten bits per
    // channel. Eye 0 lives there and nowhere else, so a dumper that only accepts
    // the backbuffer's format can never write it. Converted on the way out,
    // which loses two bits per channel and matters not at all for measuring
    // parallax.
    const bool is_r10g10b10a2 = desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM;
    // AND THE SCENE TARGET IS NOT THAT EITHER. The comment above is what the
    // last run believed; the census that followed read the desc off the bound
    // target and it is `R16G16B16A16_TYPELESS` -- format 9, SIXTY-FOUR bits per
    // pixel. The R10G10B10A2 converter above is wrong twice over for it: wrong
    // packing AND wrong stride, so it would have walked half the row and
    // produced a striped, meaningless image that looked like a capture bug
    // rather than a decoder bug. Any eye dump taken before this fix is void.
    //
    // TYPELESS means the desc does not say how to read the sixteen bits, so the
    // dumper does not guess in silence: it decodes a sample as half-float and
    // looks at the result. A UNORM value of 0.5 is 0x8000, whose top bit is a
    // half-float SIGN bit, so UNORM data read as half-float comes back full of
    // negatives; an HDR scene target does not. Whichever way it lands is
    // LOGGED with the statistic behind it.
    const bool is_r16g16b16a16 = desc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS ||
                                 desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                                 desc.Format == DXGI_FORMAT_R16G16B16A16_UNORM;
    if (desc.SampleDesc.Count != 1 &&
        (is_32bpp_bgra || is_32bpp_rgba || is_r10g10b10a2 || is_r16g16b16a16)) {
        char line[220]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] F5 capture refuses a MULTISAMPLED source: %ux%u format=%u samples=%u. "
            "It needs a ResolveSubresource first, which this path does not do.\n",
            desc.Width, desc.Height, static_cast<unsigned>(desc.Format), desc.SampleDesc.Count);
        Tf2VrLog(line);
        return false;
    }
    if (desc.SampleDesc.Count != 1 ||
        (!is_32bpp_bgra && !is_32bpp_rgba && !is_r10g10b10a2 && !is_r16g16b16a16)) {
        char line[200]{};
        std::snprintf(line, sizeof(line), "[TF2VR] F5 capture unsupported backbuffer: %ux%u format=%u samples=%u.\n", desc.Width, desc.Height, static_cast<unsigned>(desc.Format), desc.SampleDesc.Count);
        Tf2VrLog(line);
        return false;
    }
    D3D11_TEXTURE2D_DESC staging=desc; staging.Usage=D3D11_USAGE_STAGING; staging.BindFlags=0; staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ; staging.MiscFlags=0;
    Microsoft::WRL::ComPtr<ID3D11Device> device; source->GetDevice(&device); Microsoft::WRL::ComPtr<ID3D11Texture2D> copy;
    const HRESULT create_result = device->CreateTexture2D(&staging,nullptr,&copy);
    if (FAILED(create_result)) {
        char line[160]{};
        std::snprintf(line, sizeof(line), "[TF2VR] F5 capture staging texture failed: 0x%08lX.\n", static_cast<unsigned long>(create_result));
        Tf2VrLog(line);
        return false;
    }
    QueryPerformanceCounter(&timing.tStaging);
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(&context); context->CopyResource(copy.Get(),source); D3D11_MAPPED_SUBRESOURCE map{};
    QueryPerformanceCounter(&timing.tCopy);
    // THE BARRIER. Everything queued on the GPU has to retire before this
    // returns, so during a level load this single call is the freeze.
    const HRESULT map_result = context->Map(copy.Get(),0,D3D11_MAP_READ,0,&map);
    QueryPerformanceCounter(&timing.tMap);
    if (FAILED(map_result)) {
        char line[160]{};
        std::snprintf(line, sizeof(line), "[TF2VR] F5 capture map failed: 0x%08lX.\n", static_cast<unsigned long>(map_result));
        Tf2VrLog(line);
        return false;
    }
    char path[MAX_PATH]{}; if(!GetTempPathA(MAX_PATH,path)){Tf2VrLog("[TF2VR] F5 capture GetTempPath failed.\n");context->Unmap(copy.Get(),0);return false;} std::strncat(path,filename,sizeof(path)-std::strlen(path)-1);
    HANDLE file=CreateFileA(path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr); if(file==INVALID_HANDLE_VALUE){char line[320]{};std::snprintf(line,sizeof(line),"[TF2VR] F5 capture CreateFile failed (%lu): %s.\n",GetLastError(),path);Tf2VrLog(line);context->Unmap(copy.Get(),0);return false;}
    // Write the two on-disk headers separately. A containing C++ struct can
    // insert padding between the 14-byte file header and the DIB header.
    BITMAPFILEHEADER file_header{};
    BITMAPINFOHEADER info_header{};
    file_header.bfType=0x4D42; file_header.bfOffBits=sizeof(file_header)+sizeof(info_header); file_header.bfSize=file_header.bfOffBits+desc.Width*desc.Height*4;
    info_header.biSize=sizeof(info_header);info_header.biWidth=static_cast<LONG>(desc.Width);info_header.biHeight=-static_cast<LONG>(desc.Height);info_header.biPlanes=1;info_header.biBitCount=32;info_header.biCompression=BI_RGB;
    DWORD written=0;
    bool wrote_ok = WriteFile(file,&file_header,sizeof(file_header),&written,nullptr) && written == sizeof(file_header);
    wrote_ok = wrote_ok && WriteFile(file,&info_header,sizeof(info_header),&written,nullptr) && written == sizeof(info_header);
    if (is_r16g16b16a16) {
        // 64 BITS PER PIXEL, FOUR 16-BIT CHANNELS, RGBA order, into the BMP's
        // 32-bit BGRA. The interpretation is chosen from the data, once, over a
        // sample of rows, and the verdict is logged with its evidence.
        auto half_to_float = [](std::uint16_t h) -> float {
            const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
            const std::uint32_t exponent = (h >> 10) & 0x1Fu;
            const std::uint32_t mantissa = h & 0x3FFu;
            std::uint32_t bits;
            if (exponent == 0) {
                if (mantissa == 0) { bits = sign; }
                else {
                    // Subnormal: normalise it. Cheap and exact.
                    int e = -1;
                    std::uint32_t m = mantissa;
                    do { ++e; m <<= 1; } while ((m & 0x400u) == 0);
                    m &= 0x3FFu;
                    bits = sign | ((127 - 15 - e) << 23) | (m << 13);
                }
            } else if (exponent == 0x1Fu) {
                bits = sign | 0x7F800000u | (mantissa << 13);   // Inf / NaN
            } else {
                bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
            }
            float f;
            std::memcpy(&f, &bits, sizeof(f));
            return f;
        };
        bool as_float = desc.Format != DXGI_FORMAT_R16G16B16A16_UNORM;
        unsigned long long sampled = 0, oddities = 0;
        if (desc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS) {
            const UINT step = desc.Height > 32 ? desc.Height / 32 : 1;
            for (UINT y = 0; y < desc.Height; y += step) {
                const auto* src = reinterpret_cast<const std::uint16_t*>(
                    static_cast<const std::uint8_t*>(map.pData) + y * map.RowPitch);
                for (UINT x = 0; x < desc.Width; x += 8) {
                    for (int c = 0; c < 3; ++c) {
                        const std::uint16_t h = src[x * 4 + c];
                        ++sampled;
                        // A sign bit or a non-finite exponent is impossible in a
                        // linear HDR colour target and routine in UNORM bits
                        // read as half-float.
                        if ((h & 0x8000u) || ((h >> 10) & 0x1Fu) == 0x1Fu) ++oddities;
                    }
                }
            }
            as_float = sampled == 0 || (oddities * 50 < sampled);   // < 2% odd -> float
        }
        char verdict[440]{};
        std::snprintf(verdict, sizeof(verdict),
            "[TF2VR] F5 capture: %s is R16G16B16A16 (64 bpp), decoded as %s. %llu of %llu "
            "sampled channels carried a sign bit or a non-finite exponent, which UNORM data "
            "read as half-float does and an HDR target does not. Float output is clamped to "
            "0..1 and gamma-encoded for viewing -- a MONOTONIC per-pixel transform, so a "
            "parallax comparison between two dumps taken this way is unaffected.\n",
            filename, as_float ? "HALF-FLOAT" : "UNORM", oddities, sampled);
        Tf2VrLog(verdict);
        std::vector<std::uint8_t> row(desc.Width * 4);
        for (UINT y = 0; wrote_ok && y < desc.Height; ++y) {
            const auto* src = reinterpret_cast<const std::uint16_t*>(
                static_cast<const std::uint8_t*>(map.pData) + y * map.RowPitch);
            for (UINT x = 0; x < desc.Width; ++x) {
                std::uint8_t out[3]{};
                for (int c = 0; c < 3; ++c) {
                    if (as_float) {
                        float v = half_to_float(src[x * 4 + c]);
                        if (!(v > 0.0f)) v = 0.0f;     // also catches NaN
                        if (v > 1.0f) v = 1.0f;
                        const float encoded = std::pow(v, 1.0f / 2.2f) * 255.0f + 0.5f;
                        out[c] = static_cast<std::uint8_t>(encoded > 255.0f ? 255.0f : encoded);
                    } else {
                        out[c] = static_cast<std::uint8_t>(src[x * 4 + c] >> 8);
                    }
                }
                row[x * 4 + 0] = out[2];  // B
                row[x * 4 + 1] = out[1];  // G
                row[x * 4 + 2] = out[0];  // R
                row[x * 4 + 3] = 255;
            }
            wrote_ok = WriteFile(file, row.data(), desc.Width * 4, &written, nullptr) &&
                       written == desc.Width * 4;
        }
    } else if (is_r10g10b10a2) {
        // Ten bits per channel down to eight, into the BGRA order the BMP wants.
        std::vector<std::uint8_t> row(desc.Width * 4);
        for (UINT y = 0; wrote_ok && y < desc.Height; ++y) {
            const auto* src = reinterpret_cast<const std::uint32_t*>(
                static_cast<const std::uint8_t*>(map.pData) + y * map.RowPitch);
            for (UINT x = 0; x < desc.Width; ++x) {
                const std::uint32_t p = src[x];
                row[x * 4 + 0] = static_cast<std::uint8_t>(((p >> 20) & 0x3FF) >> 2);  // B
                row[x * 4 + 1] = static_cast<std::uint8_t>(((p >> 10) & 0x3FF) >> 2);  // G
                row[x * 4 + 2] = static_cast<std::uint8_t>(((p >>  0) & 0x3FF) >> 2);  // R
                row[x * 4 + 3] = 255;
            }
            wrote_ok = WriteFile(file, row.data(), desc.Width * 4, &written, nullptr) &&
                       written == desc.Width * 4;
        }
    } else
    for(UINT y=0; wrote_ok && y<desc.Height; ++y) wrote_ok = WriteFile(file,static_cast<const std::uint8_t*>(map.pData)+y*map.RowPitch,desc.Width*4,&written,nullptr) && written == desc.Width*4;
    CloseHandle(file);context->Unmap(copy.Get(),0);
    QueryPerformanceCounter(&timing.tWrite);
    {
        const double stagingMs = timing.Ms(timing.t0, timing.tStaging);
        const double copyMs = timing.Ms(timing.tStaging, timing.tCopy);
        const double mapMs = timing.Ms(timing.tCopy, timing.tMap);
        const double writeMs = timing.Ms(timing.tMap, timing.tWrite);
        const double totalMs = timing.Ms(timing.t0, timing.tWrite);
        char line[430]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] capture COST %.0f ms for %s (%ux%u): staging %.1f | CopyResource %.1f | "
            "Map(READ) %.1f | write %.1f. Map is a GPU BARRIER on the game thread -- if it dominates, "
            "this capture is not measuring a stall, it IS one, and anything it was taken to compare "
            "is measuring the capture.\n",
            totalMs, filename, desc.Width, desc.Height, stagingMs, copyMs, mapMs, writeMs);
        Tf2VrLog(line);
    }
    if (!wrote_ok) Tf2VrLog("[TF2VR] F5 capture WriteFile failed.\n");
    return wrote_ok;
}

bool DumpBackbuffer(IDXGISwapChain* swapchain, const char* filename) {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
    const HRESULT hr = swapchain->GetBuffer(0, IID_PPV_ARGS(&source));
    if (FAILED(hr)) {
        char line[160]{};
        std::snprintf(line, sizeof(line), "[TF2VR] F5 capture GetBuffer failed: 0x%08lX.\n",
                      static_cast<unsigned long>(hr));
        Tf2VrLog(line);
        return false;
    }
    return DumpTexture(source.Get(), filename);
}

bool IsVerifiedGameSwapchain(IDXGISwapChain* swapchain) {
    if (!swapchain) return false;
    if (g_gameSwapchain) {
        if (g_gameSwapchain.Get() == swapchain) return true;
        // A resolution change resizes the existing swapchain, so identity holds
        // and this does not fire. A fullscreen-mode change can replace it
        // outright, and then we stop submitting entirely: the headset freezes
        // with nothing in the log to say why. Say it once.
        static bool reported = false;
        if (!reported) {
            reported = true;
            Tf2VrLog("[TF2VR] Present arrived on a DIFFERENT swapchain than the verified one; no frames are "
                     "reaching the headset. Disarm and re-arm OpenXR to rebind.\n");
        }
        return false;
    }

    // The global DXGI vtable hook can see non-game swapchains too.  Accept
    // exactly one plausible, process-owned presentation swapchain, then use
    // pointer identity for every later submission.
    DXGI_SWAP_CHAIN_DESC desc{};
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    DWORD window_pid = 0;
    const bool got_desc = SUCCEEDED(swapchain->GetDesc(&desc));
    const bool got_device = SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(&device)));
    if (got_desc && desc.OutputWindow) GetWindowThreadProcessId(desc.OutputWindow, &window_pid);
    const bool accept = got_desc && got_device && desc.OutputWindow &&
        window_pid == GetCurrentProcessId() &&
        desc.BufferDesc.Width >= 320 && desc.BufferDesc.Height >= 200;

    // Every candidate is reported once. The whole camera path hangs off which
    // swapchain is adopted -- it decides the device, which decides the
    // immediate context we detour -- and adopting the wrong one presents as a
    // hook that installs cleanly and then sees almost no traffic. Nothing in
    // the log used to say which one was picked, or what else was on offer.
    {
        static const IDXGISwapChain* reported[8]{};
        static size_t reportedCount = 0;
        bool seen = false;
        for (size_t i = 0; i < reportedCount; ++i) {
            if (reported[i] == swapchain) { seen = true; break; }
        }
        if (!seen && reportedCount < 8) {
            reported[reportedCount++] = swapchain;
            char title[96]{};
            char klass[64]{};
            if (got_desc && desc.OutputWindow) {
                GetWindowTextA(desc.OutputWindow, title, sizeof(title) - 1);
                GetClassNameA(desc.OutputWindow, klass, sizeof(klass) - 1);
            }
            char line[420]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] Present candidate swapchain=%p device=%p hwnd=%p pid=%lu %ux%u fmt=%u "
                "class='%s' title='%s' -> %s\n",
                static_cast<void*>(swapchain), static_cast<void*>(device.Get()),
                static_cast<void*>(got_desc ? desc.OutputWindow : nullptr), window_pid,
                got_desc ? desc.BufferDesc.Width : 0, got_desc ? desc.BufferDesc.Height : 0,
                got_desc ? static_cast<unsigned>(desc.BufferDesc.Format) : 0, klass, title,
                accept ? "ADOPTED" : "rejected");
            Tf2VrLog(line);
        }
    }
    if (!accept) return false;
    g_gameSwapchain = swapchain;
    g_gameDevice = device;
    // MULTITHREAD PROTECTION, AND IT HAS TO HAPPEN HERE -- FIRST.
    //
    // Enabling it swaps the immediate context's vtable, which is why the first
    // attempt at this cost a whole run: the camera hook had already resolved
    // slot 48, that slot stopped being UpdateSubresource, stereo silently never
    // armed, and every falsifier passed. Doing it at ADOPTION, before anything
    // resolves a context vtable, removes the ordering problem entirely -- every
    // hook this plugin installs is installed later than this line.
    //
    // The vtable pointer is logged before and after so the swap is a measured
    // fact rather than a claim, and so a future reader can see immediately
    // whether hook resolution has to move.
    MaybeEnableMultithreadProtection(device.Get());
    // The two D3D context-vtable approaches (shared and object-local) both
    // fault in the NVIDIA driver. Keep the stable native-mode capture path
    // independent while GPU capture moves to RenderDoc's supported API.
    Tf2VrLog("[TF2VR] Verified game swapchain for OpenXR submission.\n");
    return true;
}

extern "C" void CaptureStereoFirstEye() {
    // The render hook only enables this after a verified game Present has
    // supplied pointer-identity-checked swapchain ownership.  This is a
    // bounded diagnostic copy, not an XR submission or a new render path.
    if (!g_gameSwapchain) {
        Tf2VrLog("[TF2VR] F5 eye-0 capture skipped: no verified game swapchain.\n");
        return;
    }
    Tf2VrLog(DumpBackbuffer(g_gameSwapchain.Get(), "titanfall2vr-eye0-normal.bmp")
        ? "[TF2VR] F5 eye-0 (normal first render) dumped.\n"
        : "[TF2VR] F5 eye-0 dump failed.\n");
}

extern "C" void CaptureStereoSecondEye() {
    if (!g_gameSwapchain) {
        Tf2VrLog("[TF2VR] F5 eye-1 capture skipped: no verified game swapchain.\n");
    } else {
        Tf2VrLog(DumpBackbuffer(g_gameSwapchain.Get(), "titanfall2vr-eye1-offset.bmp")
            ? "[TF2VR] F5 eye-1 (offset second render) dumped.\n"
            : "[TF2VR] F5 eye-1 dump failed.\n");
    }
}

// Stereo pair capture, stepped once per verified Present.  A capture that
// simply diffed a left frame against a right frame would prove nothing:
// consecutive frames already differ from TAA jitter and animation.  So two
// frames are captured at the SAME eye to measure that noise floor, and only
// then is the eye flipped.  The stereo signal has to clearly exceed the floor.
// One pending named capture, requested from the plugin frame and consumed on
// the presenting thread. Name written before the flag is set, flag read before
// the name is read, so a half-written name can never be used.
char g_namedCaptureName[96]{};
std::atomic_bool g_namedCapturePending = false;
std::atomic_int g_namedCaptureWantEye = -1;
std::atomic_int g_namedCaptureLastEye = -1;
std::atomic_bool g_stereoCaptureActive = false;
std::atomic_uint32_t g_stereoCaptureStep = 0;
std::atomic_bool g_stereoArmed = false;
std::atomic_int g_currentEye = 0;

void StepStereoPairCapture(IDXGISwapChain* swapchain) {
    if (!g_stereoCaptureActive.load(std::memory_order_acquire)) return;
    // A sign change applies to the next frame's uploads, so a transition frame
    // is skipped before the opposite-eye capture.
    switch (g_stereoCaptureStep.fetch_add(1, std::memory_order_acq_rel)) {
    case 0:
        break; // Arming frame: its camera uploads may already have happened.
    case 1:
        Tf2VrLog(DumpBackbuffer(swapchain, "titanfall2vr-stereo-a-left.bmp")
            ? "[TF2VR] PGDN: captured A (left eye).\n" : "[TF2VR] PGDN: capture A failed.\n");
        break;
    case 2:
        Tf2VrLog(DumpBackbuffer(swapchain, "titanfall2vr-stereo-b-left.bmp")
            ? "[TF2VR] PGDN: captured B (left eye, noise-floor control).\n" : "[TF2VR] PGDN: capture B failed.\n");
        SetStereoEyeSign(1.0f);
        break;
    case 3:
        break; // Transition frame: the new sign takes effect during this one.
    case 4:
        Tf2VrLog(DumpBackbuffer(swapchain, "titanfall2vr-stereo-c-right.bmp")
            ? "[TF2VR] PGDN: captured C (right eye).\n" : "[TF2VR] PGDN: capture C failed.\n");
        break;
    default:
        g_stereoCaptureActive.store(false, std::memory_order_release);
        EndStereoEyeOffset();
        Tf2VrLog("[TF2VR] PGDN: stereo pair capture complete; camera restored.\n");
        break;
    }
}

// Frame-time instrumentation.
//
// xrWaitFrame BLOCKS, and we call it from inside the Present hook to pace the
// game to the headset. That is normal for a VR runtime, but it means arming
// OpenXR changes the game's frame timing -- and an engine that interpolates its
// viewmodel against frame time will show that as the weapon jittering, while
// the world, which is far less sensitive to dt, looks fine.
//
// This is the difference every clean flat run was missing: no XR armed, no
// blocking, no pacing. Measured rather than asserted, because four hypotheses
// have already died on this flicker.
struct FrameTiming {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER lastPresent{};
    double minMs = 1e9, maxMs = 0.0, totalMs = 0.0;
    double waitMinMs = 1e9, waitMaxMs = 0.0, waitTotalMs = 0.0;
    unsigned samples = 0;
};
FrameTiming g_timing;

double ElapsedMs(const LARGE_INTEGER& from, const LARGE_INTEGER& to) {
    if (!g_timing.frequency.QuadPart) return 0.0;
    return static_cast<double>(to.QuadPart - from.QuadPart) * 1000.0 /
           static_cast<double>(g_timing.frequency.QuadPart);
}

// F2a -- THE STALE WINDOW, MEASURED PER EVENT.
//
// The 240-frame window above cannot localise a freeze: it reports one MAX for a
// window that is 13 SECONDS long during a level entry, so "MAX 5788" says a
// 5.8 s gap happened somewhere in there and nothing more. That coarseness is
// what made dating the regression from the archive a whole session's work.
//
// This records each gap as its own event: when the image went stale, when it
// came back, and how long the wearer was looking at a picture the game was no
// longer updating. That is F2's falsifier half, and it is deliberately
// SEPARATE from F2's visible indicator -- see the note on the accessor below
// for why the indicator cannot live here.
//
// Sampled from the SAME `ms` the window telemetry uses, on purpose. A stale
// event of 5788 ms must appear as the MAX of whichever 240-frame window it
// lands in, so the two instruments check each other and a disagreement means
// one of them is broken rather than the game being strange.
struct StaleWatch {
    double thresholdMs = 250.0;   // xr.stale_ms
    double sessionMs = 0.0;       // wall clock since the first Present we saw
    unsigned long long presents = 0;
    unsigned events = 0;          // gaps at or over the threshold
    double worstMs = 0.0;
    double totalStaleMs = 0.0;
    unsigned linesLogged = 0;
    // Bounded by construction: a session that somehow stalls every frame must
    // not be able to fill the log and bury the startup lines. The tally keeps
    // counting after the lines stop, so the summary stays true.
    static constexpr unsigned kMaxLines = 60;
};
StaleWatch g_stale;

// The seam F2b (the visible not-live marker) will read. It is published here
// and consumed by nothing yet, deliberately.
//
// The indicator CANNOT be driven from this file. Every XR call we make --
// xrWaitFrame through xrEndFrame -- happens inside one call from this hook, so
// while Present is blocked for 5.8 s no code of ours runs at all: there is no
// frame to dim and no moment to clear the marker on. A heartbeat that survives
// a blocked Present is the pacing thread, and that is why the indicator ships
// with the decoupling rather than ahead of it.
std::atomic<long long> g_lastPresentQpc{0};

void RecordStaleWindow(double gapMs) {
    ++g_stale.presents;
    g_stale.sessionMs += gapMs;
    if (gapMs < g_stale.thresholdMs) return;
    ++g_stale.events;
    g_stale.totalStaleMs += gapMs;
    if (gapMs > g_stale.worstMs) g_stale.worstMs = gapMs;
    if (g_stale.linesLogged >= StaleWatch::kMaxLines) return;
    ++g_stale.linesLogged;
    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] STALE WINDOW #%u: the presented image was frozen for %.0f ms -- entered at t=%.2f s, "
        "cleared at t=%.2f s, on present frame %llu. POSITIVE CONTROL: %llu presents seen this session, "
        "threshold %.0f ms. FALSIFIER: this gap must also be the MAX of the 240-frame window it lands "
        "in; if that window reports a smaller MAX, one of the two instruments is wrong and neither "
        "reading counts.\n",
        g_stale.events, gapMs, (g_stale.sessionMs - gapMs) / 1000.0, g_stale.sessionMs / 1000.0,
        static_cast<unsigned long long>(g_tf2vrPresentFrame), g_stale.presents, g_stale.thresholdMs);
    Tf2VrLog(line);
    if (g_stale.linesLogged == StaleWatch::kMaxLines) {
        Tf2VrLog("[TF2VR] STALE WINDOW: line cap reached; further events are counted in the summary "
                 "but not printed individually.\n");
    }
}

void RecordFrameTiming(double waitMs) {
    if (!g_timing.frequency.QuadPart) QueryPerformanceFrequency(&g_timing.frequency);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    g_lastPresentQpc.store(now.QuadPart, std::memory_order_release);
    if (g_timing.lastPresent.QuadPart) {
        const double ms = ElapsedMs(g_timing.lastPresent, now);
        RecordStaleWindow(ms);
        if (ms < g_timing.minMs) g_timing.minMs = ms;
        if (ms > g_timing.maxMs) g_timing.maxMs = ms;
        g_timing.totalMs += ms;
        if (waitMs < g_timing.waitMinMs) g_timing.waitMinMs = waitMs;
        if (waitMs > g_timing.waitMaxMs) g_timing.waitMaxMs = waitMs;
        g_timing.waitTotalMs += waitMs;
        if (++g_timing.samples >= 240) {
            char line[360]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] frame time over %u frames: min %.2f avg %.2f MAX %.2f ms; "
                "of which XR submit min %.2f avg %.2f MAX %.2f ms. A large max, or a max far above "
                "the average, is the game being paced unevenly -- which a lag-interpolated viewmodel "
                "shows as jitter.\n",
                g_timing.samples, g_timing.minMs, g_timing.totalMs / g_timing.samples, g_timing.maxMs,
                g_timing.waitMinMs, g_timing.waitTotalMs / g_timing.samples, g_timing.waitMaxMs);
            Tf2VrLog(line);
            g_timing.minMs = 1e9; g_timing.maxMs = 0.0; g_timing.totalMs = 0.0;
            g_timing.waitMinMs = 1e9; g_timing.waitMaxMs = 0.0; g_timing.waitTotalMs = 0.0;
            g_timing.samples = 0;
        }
    }
    g_timing.lastPresent = now;
}

// TIMING IS EVERYTHING HERE, AND THE FIRST VERSION GOT IT WRONG.
//
// The swapchain is DXGI_SWAP_EFFECT_DISCARD, so AFTER Present the backbuffer
// contents are UNDEFINED. The first version of this sampled from the game tick
// -- which is after Present -- and read 0.0% non-black on every single sample,
// including the handshake window the wearer was looking at with their own eyes.
// It reported a black screen that was not black.
//
// The existing named-capture path already had this right and I did not read it:
// it services requests INSIDE the Present hook, before g_originalPresent, while
// the backbuffer still holds the frame. So does this now. The request is queued
// from any thread, serviced in Present, and collected on a later tick.
//
// This also makes the sample answer the question that matters. Sampled before
// the flip, it is what the game DREW. If that has content while the wearer sees
// black, the fault is presentation. If it is black, the game stopped drawing.
// Those are different bugs and the old placement could not tell them apart.
std::atomic_bool g_contentSamplePending{false};
std::atomic_bool g_contentSampleReady{false};
char g_contentSampleName[64]{};
BackbufferContent g_contentSampleResult{};

// IS THERE ANYTHING ON THE SCREEN? Answered with a number, not a judgement.
//
// The 2026-08-27 aspect sweep produced a clean, monotonic cost curve while the
// wearer was looking at a BLACK SCREEN. I argued the render was real from the
// viewport traffic and the shape of the curve, and that argument was probably
// right -- but it was an argument, and the wearer's answer was better: look at
// the screen. So the sweep now looks.
//
// NOT a full backbuffer dump. At 7256x4268 that is a 124 MB BMP per step, and
// the Map is a full GPU barrier that would land inside the frame-time window it
// is meant to validate. This point-samples a THUMBNAIL out of the staging copy
// -- one small file per step to eyeball, plus the statistic that makes eyeballing
// optional -- and the sweep calls it once per step AFTER timing has closed.
bool SampleBackbufferContentNow(IDXGISwapChain* swapchain, const char* thumbnailName,
                                BackbufferContent* out) {
    if (out) *out = {};
    if (!swapchain) return false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&source)))) return false;
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                      desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                      desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (desc.SampleDesc.Count != 1 || (!bgra && !rgba)) return false;

    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    source->GetDevice(&device);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> copy;
    if (FAILED(device->CreateTexture2D(&staging, nullptr, &copy))) return false;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    context->CopyResource(copy.Get(), source.Get());
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;

    constexpr int kThumbW = 320;
    constexpr int kThumbH = 200;
    static std::uint32_t thumbnail[kThumbW * kThumbH];
    unsigned long long nonBlack = 0;
    unsigned long long lumaSum = 0;
    unsigned brightest = 0;
    const auto* base = static_cast<const std::uint8_t*>(map.pData);
    for (int ty = 0; ty < kThumbH; ++ty) {
        const unsigned sy = static_cast<unsigned>(
            (static_cast<unsigned long long>(ty) * desc.Height) / kThumbH);
        const auto* row = base + static_cast<size_t>(sy) * map.RowPitch;
        for (int tx = 0; tx < kThumbW; ++tx) {
            const unsigned sx = static_cast<unsigned>(
                (static_cast<unsigned long long>(tx) * desc.Width) / kThumbW);
            const std::uint8_t* px = row + static_cast<size_t>(sx) * 4;
            const unsigned b = bgra ? px[0] : px[2];
            const unsigned g = px[1];
            const unsigned r = bgra ? px[2] : px[0];
            // Integer luma, near enough for "is this black".
            const unsigned luma = (r * 77 + g * 151 + b * 28) >> 8;
            lumaSum += luma;
            if (luma > 8) ++nonBlack;
            if (luma > brightest) brightest = luma;
            thumbnail[ty * kThumbW + tx] =
                (0xFFu << 24) | (r << 16) | (g << 8) | b;
        }
    }
    context->Unmap(copy.Get(), 0);

    constexpr int kSamples = kThumbW * kThumbH;
    if (out) {
        out->valid = true;
        out->width = desc.Width;
        out->height = desc.Height;
        out->nonBlackFraction = static_cast<float>(nonBlack) / kSamples;
        out->meanLuma = static_cast<float>(lumaSum) / kSamples;
        out->brightest = brightest;
    }

    if (!thumbnailName) return true;
    char path[MAX_PATH]{};
    if (!GetTempPathA(MAX_PATH, path)) return true;
    std::strncat(path, thumbnailName, sizeof(path) - std::strlen(path) - 1);
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return true;
    BITMAPFILEHEADER fileHeader{};
    BITMAPINFOHEADER infoHeader{};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
    fileHeader.bfSize = fileHeader.bfOffBits + sizeof(thumbnail);
    infoHeader.biSize = sizeof(infoHeader);
    infoHeader.biWidth = kThumbW;
    infoHeader.biHeight = -kThumbH;  // top-down
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biCompression = BI_RGB;
    DWORD written = 0;
    WriteFile(file, &fileHeader, sizeof(fileHeader), &written, nullptr);
    WriteFile(file, &infoHeader, sizeof(infoHeader), &written, nullptr);
    WriteFile(file, thumbnail, sizeof(thumbnail), &written, nullptr);
    CloseHandle(file);
    return true;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swapchain, UINT syncInterval, UINT flags) {
    // RemovePresentHook sets g_unloading before waiting for this count.  A
    // Everything this hook does before handing off to the real Present is the
    // plugin's frame cost: the XR submission, the captures, the frame-boundary
    // ticks. The real Present is bracketed out below -- it is the game waiting
    // for the display, not us.
    PluginCost::Scope costScope(PluginCost::kPresent);
    // second check after increment closes the race with a just-starting call.
    g_presentInFlight.fetch_add(1, std::memory_order_acq_rel);
    // THE ONLY FRAME COUNTER THAT ALWAYS ADVANCES.
    //
    // camera_update_hook's g_frameIndex only moves while a bounded trace is
    // running, so it cannot time anything outside one. This one counts
    // presented frames for the whole session, and F1's watchpoint reads it
    // from a vectored exception handler -- which is why it is a plain volatile
    // word and not an atomic with an operator that could be lowered to a
    // locked read-modify-write the handler would have to wait on. A torn or
    // stale count costs a fraction of one frame in a cadence average.
    ++g_tf2vrPresentFrame;
    // M1's PRESENT-INTERVAL BOUNDARY. Closes the render-target census interval
    // and dumps it if it is one of the few worth log lines. Here rather than on
    // the plugin frame because the interval this census measures is the span
    // between two Presents, and because this is the thread that ends it.
    // Cheap when nothing is armed: one acquire load and a return.
    RtvCensusOnPresent();
    // SAME-FRAME STEREO: THE PROOF PAIR AND THE PER-FRAME RESET, BOTH OUTSIDE
    // THE XR PATH.
    //
    // Both of these lived inside the XR submit block for one run, and a flat
    // autoarm=0 run never enters it. The cost was total: the scene-pass counter
    // was never reset, so it reached 33,248 and "the second scene pass" fired on
    // the second of the SESSION rather than of the frame; and the eye pair was
    // never written at all. The falsifier caught it -- the line that should read
    // 2 read 33248 -- which is the only reason the run was recognised as void
    // instead of read as a result.
    //
    // Neither of these needs XR. The pair is two files on disk, and the reset is
    // per-frame bookkeeping that must happen on EVERY Present or the counter is
    // meaningless.
    // M2's PROOF PAIR, and both halves come from ONE Present so the comparison
    // cannot be between two different frames. The backbuffer is what the wearer
    // sees; the private target is where batch 2 was sent instead. The falsifier
    // is that the world image has moved from the first to the second.
    if (RtvCensusBurstCaptureWanted()) {
        ID3D11Texture2D* priv = RtvCensusPrivateTexture();
        Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
        if (priv && SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
            // THE ARM STATE IS IN THE FILENAME, and that is not cosmetic. Eight
            // doubled frames last about a tenth of a second, which is a flash,
            // not something anyone can judge live -- so the falsifier is read
            // off the images, and the positive control needs its own images
            // from the same session. Fixed names would have the disarmed burst
            // overwrite the armed one and force a round trip between them.
            const bool armed = RtvSubstituteArmed();
            // THE ARM LABEL IS READ AT THE MEASUREMENT, from the interval this
            // pair is a photograph OF -- not from the session totals, and not
            // from the flag alone. M2's first run captured two frames that were
            // both unsubstituted, compared them, and the difference between
            // them was seventy seconds of idle animation. This number is what
            // makes that impossible to repeat quietly.
            const int subs = RtvCensusCapturedIntervalSubstitutions();
            // THE NAME CARRIES THE ARM OF WHICHEVER LEVER IS THE LIVE QUESTION.
            // Once M3 is armed, the pair IS the stereo pair, and its control is
            // the same pair with the eye offset off -- so the M2 suffix would
            // have let the control overwrite the evidence.
            // THE NAME CARRIES THE ARM OF THE LEVER UNDER TEST, AND THAT LEVER
            // MOVED. F3 used to toggle the eye offset and now toggles the
            // read-side substitution -- so keying the filename on
            // Batch2OffsetArmed() stopped tracking the experiment, both presses
            // of a run wrote the same two files, and the control silently
            // overwrote the armed capture. One run, no evidence.
            //
            // The read state is now IN the name rather than implied by it, so an
            // A/B in one session cannot collide however the slot is reassigned
            // next.
            const bool m3 = Batch2OffsetArmed();
            const bool readFix = RtvReadSubstituteArmed();
            // N2 TAKES THE NAME WHILE IT IS THE LIVE LEVER, for exactly the
            // reason written three paragraphs above: the file name has to carry
            // the arm of the lever under test, or the zero-offset control
            // silently overwrites the armed capture and the run yields no
            // evidence. That has already happened once here.
            const bool engineWrite = EngineCameraWriteArmed();
            // THE MAGNITUDE GOES IN THE NAME. The ladder photographs several
            // offsets in ONE session, so a name that only said ENGINEWRITE would
            // have every rung overwrite the last and the run would end with one
            // picture instead of a curve. Same reason the arm went into the name
            // in the first place, one failure later.
            // AND SO DO THE SHOT NUMBER AND THE TEMPORAL ARM, for the third and
            // fourth time in this function's history and for the same reason
            // every time: a name that does not carry the variable under test
            // has the control silently overwrite the evidence, and the run ends
            // with one picture instead of an experiment. T-A0 photographs shot 1
            // and shot 8 of ONE burst, and T-A takes each of those in two states
            // of the engine's temporal resolve. That is four files per offset
            // and exactly one name shape that keeps them apart.
            //   S1/S8  -- which shot of the burst
            //   TAA    -- the shipped resolve, 95% history
            //   CUR    -- current-frame-only, the T-A arm
            char n2Priv[80]{};
            char n2Back[80]{};
            const float activeIpd = EngineIpdActive();
            const char* privName;
            const char* backName;
            if (EngineIpd() != 0.0f) {
                const int shot = RtvCensusBurstCaptureShot();
                const char* temporal = TemporalCurrentFrameOnlyArmed() ? "CUR" : "TAA";
                std::snprintf(n2Priv, sizeof(n2Priv), "tf2vr-N2-eye1-private-U%03d-S%d-%s.bmp",
                              static_cast<int>(activeIpd + 0.5f), shot, temporal);
                std::snprintf(n2Back, sizeof(n2Back), "tf2vr-N2-eye0-back-U%03d-S%d-%s.bmp",
                              static_cast<int>(activeIpd + 0.5f), shot, temporal);
                privName = n2Priv;
                backName = n2Back;
            } else if (m3 && readFix) {
                privName = "tf2vr-eye1-private-READFIX.bmp";
                backName = "tf2vr-eye0-back-READFIX.bmp";
            } else if (m3) {
                privName = "tf2vr-eye1-private-GHOST.bmp";
                backName = "tf2vr-eye0-back-GHOST.bmp";
            } else if (RtvCensusCapturedIntervalSubstitutions() >= 0 && armed) {
                privName = "tf2vr-M3-CONTROL-private.bmp";
                backName = "tf2vr-M3-CONTROL-back.bmp";
            } else {
                privName = "tf2vr-M2-private-DISARMED.bmp";
                backName = "tf2vr-M2-back-DISARMED.bmp";
            }
            DumpTexture(priv, privName);
            DumpTexture(back.Get(), backName);
            char line[700]{};
            if (EngineIpd() != 0.0f) {
                // N2 IS THE LIVE LEVER, so the pair is described in N2's terms.
                // The M2/M3 sentence below would say "the eye offset is off, so
                // the two files must show the SAME viewpoint" on an armed N2
                // burst, which is the opposite of what is being asked.
                std::snprintf(line, sizeof(line),
                    "[TF2VR] N2 PAIR written from ONE Present, %d substitutions in the captured "
                    "interval: %s and %s. Burst shot %d. Temporal resolve %s. Engine-side eye "
                    "write %s at %.2f world units (ladder top %.2f). %s\n",
                    subs, backName, privName, RtvCensusBurstCaptureShot(),
                    TemporalCurrentFrameOnlyArmed()
                        ? "CURRENT-FRAME-ONLY (T-A armed)"
                        : "SHIPPED (history-weighted)",
                    engineWrite ? "ARMED" : "DISARMED", activeIpd,
                    EngineIpd(),
                    engineWrite
                        ? "The private eye is batch 2's own world from a camera moved along the "
                          "camera RIGHT axis by that much. Horizontal shift against the "
                          "backbuffer that FALLS with distance, dy = 0. A shift that does not "
                          "fall with distance, or any vertical shift, is not this lever."
                        : "THE POSITIVE CONTROL: the write is off, so this pair must show the "
                          "SAME viewpoint and bmpshift against the armed pair's backbuffer must "
                          "be flat. An unflat control disqualifies the armed reading.");
                Tf2VrLog(line);
            } else if (armed && subs <= 0) {
                std::snprintf(line, sizeof(line),
                    "[TF2VR] M2 PAIR (ARMED) is VOID: the interval it photographed made %d "
                    "substitutions. The substitution is armed but did not run on THIS frame, so "
                    "the backbuffer is an unsubstituted doubled frame and comparing it against "
                    "the control proves nothing. The M2 status line names which decline it "
                    "took.\n", subs);
            } else {
                std::snprintf(line, sizeof(line),
                    "[TF2VR] PAIR written from ONE Present (%s), %d substitutions in the captured "
                    "interval: %s and %s. %s\n",
                    m3 ? (readFix ? "eye offset ARMED, read-fix ARMED"
                                  : "eye offset ARMED, read-fix OFF -- the GHOST control")
                       : (armed ? "M3 CONTROL, eye offset OFF" : "M2 DISARMED"),
                    subs, backName, privName,
                    !m3 ? "The eye offset is off, so the two files must show the SAME viewpoint."
                    : readFix
                        ? "The private eye should now hold batch 2's OWN world, post-processed "
                          "from batch 2's own scene -- no faint unshifted copy of the backbuffer's "
                          "geometry in it."
                        : "The GHOST control: batch 2's post is reading batch 1's scene, so the "
                          "private eye must still carry the faint unshifted copy. If it does not, "
                          "the read-fix was not what removed it.");
            }
            Tf2VrLog(line);
            RtvCensusClearBurstCapture();
        } else if (!priv) {
            Tf2VrLog("[TF2VR] M2 PAIR skipped: there is no private target, so the substitution "
                     "never armed. The M2 status line says which decline it took.\n");
            RtvCensusClearBurstCapture();
        }
    }
    if (SameFrameEyePairWanted()) {
        if (ID3D11Texture2D* eye0 = SameFrameEye0Texture()) {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
            if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
                // INDEXED, not overwritten. Two shots of the same scene under
                // two settings is the whole point of a capture pair, and a
                // fixed filename means the second shot destroys the first --
                // the wearer would take both and I would have one.
                static int pairIndex = 0;
                char name0[64]{};
                char name1[64]{};
                std::snprintf(name0, sizeof(name0), "tf2vr-EYE0-%d.bmp", pairIndex);
                std::snprintf(name1, sizeof(name1), "tf2vr-EYE1-%d.bmp", pairIndex);
                ++pairIndex;
                DumpTexture(eye0, name0);
                DumpTexture(back.Get(), name1);
                Tf2VrLog("[TF2VR] F1 EYE PAIR written: %TEMP%\\tf2vr-EYE0.bmp is the scene "
                         "target at the mid-frame boundary, EYE1 is the backbuffer after the "
                         "second pass. Near geometry must shift HORIZONTALLY between them; the "
                         "sky must not.\n");
                ClearSameFrameEyePairWanted();
            }
        } else {
            // No mid-frame eye-0 target on this path (flat, or stereo not
            // armed): the backbuffer alone is still the wearer's screenshot.
            Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
            if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
                // Numbered per press, so several presses in one run all survive.
                static int s_f6Shot = 0;
                char name[64];
                std::snprintf(name, sizeof(name), "tf2vr-F6-%02d.bmp", ++s_f6Shot);
                DumpTexture(back.Get(), name);
                char line[200];
                std::snprintf(line, sizeof(line),
                              "[TF2VR] F6 SCREENSHOT #%d written: %%TEMP%%\\%s. This is the path "
                              "that works in this profile -- there is no mid-frame eye-0 target "
                              "here, so the stereo branch above is skipped and this fallback is "
                              "what fires.\n", s_f6Shot, name);
                Tf2VrLog(line);
            }
            ClearSameFrameEyePairWanted();
        }
    }
    const bool may_submit = !g_unloading.load(std::memory_order_acquire);
    LARGE_INTEGER submitStart{}, submitEnd{};
    QueryPerformanceCounter(&submitStart);
    // THE XR SUBMISSION IS A WAIT, NOT WORK, AND IT LEAVES kPresent HERE.
    //
    // xrWaitFrame..xrEndFrame all happen inside this span, and the runtime
    // parks us in whichever of them the frame phase lands in -- that is how it
    // paces us. Leaving it inside kPresent made the report read "14.750 ms of
    // 15.43 ms is OURS = 95.6%" on 2026-09-08, which is true only if you count
    // blocking on the compositor as plugin cost. It is bracketed out the same
    // way the real Present already is, and reported on its own as kXrSubmit.
    //
    // If an early return between here and submitEnd skips the resume, ~Scope
    // charges us only up to the pause -- an under-count, which is the safe
    // direction for a number used to decide what to optimise.
    costScope.PauseForForward();
    PluginCost::Scope xrWaitScope(PluginCost::kXrSubmit);
    // Capture before the real Present advances/discards the game backbuffer.
    // THE CONFIG PANEL IS BUILT HERE: BEFORE the XR submission, and OUTSIDE the
    // XR mutex. Both halves of that are load-bearing and each was learned from a
    // failed run.
    //
    // BEFORE the submission, because g_xr.SubmitBackbuffer is what hands the
    // backbuffer to the headset. Drawing after it put the panel on the desktop
    // window and never in the eye.
    //
    // OUTSIDE the mutex, because the panel calls registry GETTERS, and a getter
    // is free to take whatever lock its own module uses. IsXrDecoupled() takes
    // g_xrMutex -- so building the panel inside the scoped_lock below re-locked
    // a non-recursive std::mutex on the same thread. That is undefined
    // behaviour, and it crashed the game on summon; with the fault guard in
    // place it faulted identically three times at "row 28 (xr.decouple)", which
    // is what named it.
    //
    // The rule this establishes, and it is written on the registry too: a
    // registry getter must be callable from the presenting thread with no XR
    // lock held. Keeping the build out here means that stays true even for
    // getters nobody has written yet.
    //
    // The cost is that a backbuffer capture taken while the panel is UP now
    // contains the panel. That is a diagnostic path, the panel is only up when
    // the wearer summons it, and a deadlock is not a trade worth making to avoid
    // it.
    if (may_submit && IsVerifiedGameSwapchain(swapchain)) MenuOnVerifiedPresent(swapchain);
    if (may_submit) {
        std::scoped_lock lock(g_xrMutex);
        if (!g_unloading.load(std::memory_order_acquire) && IsVerifiedGameSwapchain(swapchain)) {
            // BEFORE g_originalPresent, for the same reason the named capture
            // is: DXGI_SWAP_EFFECT_DISCARD leaves the backbuffer undefined
            // afterwards, and sampling it from the game tick read black on a
            // window the wearer could plainly see.
            if (g_contentSamplePending.load(std::memory_order_acquire)) {
                g_contentSamplePending.store(false, std::memory_order_release);
                SampleBackbufferContentNow(swapchain,
                                           g_contentSampleName[0] ? g_contentSampleName : nullptr,
                                           &g_contentSampleResult);
                g_contentSampleReady.store(true, std::memory_order_release);
            }
            if (g_namedCapturePending.load(std::memory_order_acquire)) {
                // The eye this frame was RENDERED for. In stereo the sign is
                // flipped after submission, so the value standing here is the one
                // the image now in the backbuffer belongs to.
                const int renderingEye = g_stereoArmed.load(std::memory_order_acquire)
                    ? g_currentEye.load(std::memory_order_acquire) : -1;
                const int wantEye = g_namedCaptureWantEye.load(std::memory_order_acquire);
                // A request pinned to an eye waits for that eye to come round,
                // which under alternate-frame stereo is the very next frame.
                // Nothing is dropped and nothing is retried.
                if (wantEye < 0 || renderingEye < 0 || wantEye == renderingEye) {
                    char name[sizeof(g_namedCaptureName)]{};
                    std::memcpy(name, g_namedCaptureName, sizeof(name));
                    g_namedCaptureLastEye.store(renderingEye, std::memory_order_release);
                    g_namedCapturePending.store(false, std::memory_order_release);
                    const bool ok = DumpBackbuffer(swapchain, name);
                    char line[260]{};
                    std::snprintf(line, sizeof(line),
                                  "[TF2VR] flat session capture %s (eye %d): %%TEMP%%\\%s\n",
                                  ok ? "written" : "FAILED", renderingEye, name);
                    Tf2VrLog(line);
                }
            }
            const auto capture=ConsumeStereoCaptureAction();
            if(capture==StereoCaptureAction::Baseline){Tf2VrLog(DumpBackbuffer(swapchain,"titanfall2vr-baseline.bmp")?"[TF2VR] F5 baseline dumped.\n":"[TF2VR] F5 baseline dump failed.\n");}
            else if(capture==StereoCaptureAction::Control){Tf2VrLog(DumpBackbuffer(swapchain,"titanfall2vr-control.bmp")?"[TF2VR] F5 control frame dumped.\n":"[TF2VR] F5 control dump failed.\n");BeginStereoSlotExperiment();}
            else if(capture==StereoCaptureAction::Test){Tf2VrLog(DumpBackbuffer(swapchain,"titanfall2vr-slot1-offset.bmp")?"[TF2VR] F5 slot-1-prepared frame dumped.\n":"[TF2VR] F5 slot-1 frame dump failed.\n");EndStereoSlotExperiment();}
            StepStereoPairCapture(swapchain);
            // Present follows a frame's uploads, so this is the boundary the
            // passive trace starts and counts on.
            //
            // It is also the LAST point in the frame we own, which is why the bone
            // write's survival is measured here. Measuring it at the render side
            // could only ever answer "did it survive to there"; the question that
            // decides the next move is whether it was still standing when the frame
            // ended. Read-only.
            ProbeWeaponBoneWriteSurvival();
            NotifyCameraUpdateFrameBoundary();

            if(g_xrArmed.load(std::memory_order_acquire)) {
                // The frame about to be submitted was rendered with the eye
                // sign set on the previous Present, so submit it as that eye
                // and only then flip for the next frame.  Flipping first would
                // silently swap the eyes.
                const int renderedEye = g_stereoArmed.load(std::memory_order_acquire)
                    ? g_currentEye.load(std::memory_order_acquire) : -1;
                // PRESENT RUNS THE WHOLE FRAME, decoupled or not, and that is
                // the conclusion the measurements forced.
                //
                // xrWaitFrame IS the display clock. Called from here it paced
                // the game at 12.50 ms +/- 0.19. Every arrangement that moved
                // the frame loop off this thread had to pace the game with a
                // clock of our own instead, and the best that ever reached was
                // 12.75 +/- 0.52 -- a second clock, slipping a whole frame every
                // fifty or so, which is head-motion judder that no amount of
                // timer precision removes.
                //
                // So gameplay keeps the shipped path exactly, and the pacing
                // SAME-FRAME STEREO SUBMIT. When the mid-frame boundary caught
                // eye 0, this frame carries BOTH eyes: eye 0 from the scene
                // target as it stood before batch 2 drew over it, and eye 1 from
                // the backbuffer, which now holds batch 2's composite.
                //
                // Nothing downstream changes. The compositor already receives a
                // two-view projection layer with a persistent swapchain image
                // per eye, and already carries two full eye images every frame
                // -- today the second one is filled from the PREVIOUS frame.
                // This fills it from THIS frame, which is the whole difference
                // between alternate-frame and true stereo.
                //
                // The eye-sign flip below is left alone deliberately: it is the
                // alternate-frame path's own bookkeeping, and while true stereo
                // is a per-press experiment rather than a mode, disturbing it
                // would break the shipping path on every frame that is NOT
                // doubled.
                // EYE 1 COMES FROM WHERE BATCH 2 ACTUALLY RENDERS.
                //
                // The branch below this one predates M2's substitution. It took
                // eye 1 from the BACKBUFFER on the reasoning, true at the time,
                // that batch 2 had drawn over it. That stopped being true the
                // moment the substitution began redirecting batch 2 into a
                // private target: since then the backbuffer has held BATCH 1's
                // composite, so that path submits batch 1 to BOTH eyes and the
                // headset sees a doubled mono frame. Nothing said so, because
                // every judgement about this feature has been made from BMPs --
                // `RtvCensusPrivateTexture()` had exactly one caller in the
                // plugin and it was the dump a few hundred lines above.
                //
                // The pair that is KNOWN GOOD is the one the capture writes,
                // because it is the pair the wearer looked at and called a
                // perfect parallax shift: eye 0 the backbuffer, eye 1 the
                // private target. This submits exactly those two textures, so
                // what reaches the compositor is the thing that was judged
                // rather than a second arrangement nobody has seen.
                //
                // EYE ASSIGNMENT. Batch 2's camera moves along camera RIGHT, so
                // batch 2 is the RIGHT eye and batch 1 is the left. OpenXR view
                // 0 is left and view 1 is right, so backbuffer -> 0 and private
                // -> 1. That also matches the direction the wearer read off the
                // frame: world content shifted LEFT, i.e. the camera went right.
                ID3D11Texture2D* privateEye = RtvCensusPrivateTexture();
                const int intervalSubs = RtvCensusCapturedIntervalSubstitutions();
                if (privateEye && RtvSubstituteArmed() && intervalSubs > 0) {
                    XrBreadcrumb::NoteSubmittedTexture(0, swapchain);
                    g_xr.SubmitBackbuffer(swapchain, 0);
                    XrBreadcrumb::NoteSubmittedTexture(1, privateEye);
                    g_xr.SubmitEyeTexture(privateEye, 1);
                    // IT SAYS SO, ONCE AND THEN RARELY. A submit path that
                    // silently does the right thing is indistinguishable from
                    // one that never runs, and this one is reached only on
                    // doubled frames -- which are a burst, not a mode.
                    static unsigned long long trueStereoSubmits = 0;
                    if (++trueStereoSubmits == 1 || (trueStereoSubmits % 240) == 0) {
                        char line[420]{};
                        std::snprintf(line, sizeof(line),
                            "[TF2VR] TRUE STEREO SUBMIT #%llu: eye 0 = the backbuffer (batch 1), "
                            "eye 1 = batch 2's PRIVATE target, both from THIS frame, %d "
                            "substitutions in the interval. This is the first path that sends "
                            "batch 2 anywhere but a .bmp.\n",
                            trueStereoSubmits, intervalSubs);
                        Tf2VrLog(line);
                    }
                } else if (ID3D11Texture2D* eye0 = SameFrameEye0Texture()) {
                    XrBreadcrumb::NoteSubmittedTexture(0, eye0);
                    g_xr.SubmitEyeTexture(eye0, 0);
                    XrBreadcrumb::NoteSubmittedTexture(1, swapchain);
                    g_xr.SubmitBackbuffer(swapchain, 1);
                } else {
                    XrBreadcrumb::NoteSubmittedTexture(renderedEye, swapchain);
                    g_xr.SubmitBackbuffer(swapchain, renderedEye);
                }
                // The eye advances on REAL GAME FRAMES ONLY, and this is the
                // only place that sees one. A gap frame re-shows the last real
                // image and carries its eye rather than advancing.
                if (renderedEye >= 0) {
                    NotifyStereoFrameBoundary();
                    const int nextEye = renderedEye ^ 1;
                    g_currentEye.store(nextEye, std::memory_order_release);
                    SetStereoEyeSign(nextEye == 0 ? -1.0f : 1.0f);
                }
            }
        }
    }
    // Installing the upload hook from here works, where installing it at the
    // moment F9 is pressed resolved the wrong function.
    QueryPerformanceCounter(&submitEnd);
    xrWaitScope.Stop();
    costScope.ResumeAfterForward();
    RecordFrameTiming(ElapsedMs(submitStart, submitEnd));
    if (g_gameDevice) TickViewmodelCompensation(g_gameDevice.Get());
    costScope.PauseForForward();
    const HRESULT result = g_originalPresent(swapchain, syncInterval, flags);
    costScope.ResumeAfterForward();
    PluginCost::NotifyFrame();
    // SAME-FRAME STEREO: the per-frame reset, AFTER everything that consumes it.
    // It clears the eye-0 validity and zeroes the scene-pass counter, so putting
    // it any earlier would have taken eye 0 away from the submit that needs it.
    // On EVERY Present, armed or not, because a counter that only resets on
    // armed frames is the 33,248 again.
    ResetSameFrameCapture();
    g_presentInFlight.fetch_sub(1, std::memory_order_acq_rel);
    return result;
}
}

void RequestNamedBackbufferCaptureOnEye(const char* filename, int eye) {
    if (!filename || !*filename) return;
    std::snprintf(g_namedCaptureName, sizeof(g_namedCaptureName), "%s", filename);
    g_namedCaptureWantEye.store(eye, std::memory_order_release);
    g_namedCapturePending.store(true, std::memory_order_release);
}

void RequestNamedBackbufferCapture(const char* filename) {
    RequestNamedBackbufferCaptureOnEye(filename, -1);
}

int LastNamedCaptureEye() { return g_namedCaptureLastEye.load(std::memory_order_acquire); }
bool NamedCaptureOutstanding() { return g_namedCapturePending.load(std::memory_order_acquire); }

bool DumpVerifiedGameBackbuffer(const char* filename) {
    if (!g_gameSwapchain) {
        Tf2VrLog("[TF2VR] Native-slot capture skipped: no verified game swapchain.\n");
        return false;
    }
    return DumpBackbuffer(g_gameSwapchain.Get(), filename);
}

// Read-only: reports the entry bytes of the D3D11 methods this project detours.
// This is the diagnostic that established UpdateSubresource displaces 20 bytes
// where Draw/DrawIndexed displace 16, and it is how a "which function did we
// actually resolve" question gets answered without installing anything.
void ProbeVerifiedGameDrawFunctionEntries() {
    if (!g_gameDevice) {
        Tf2VrLog("[TF2VR] draw-entry probe skipped; game swapchain not verified yet.\n");
        return;
    }
    ProbeD3D11DrawFunctionEntries(g_gameDevice.Get());
}

void BeginVerifiedGameDrawFunctionEntryTrace() {
    if (!g_gameDevice) { Tf2VrLog("[TF2VR] function-entry trace skipped; game swapchain not verified yet.\n"); return; }
    BeginD3D11FunctionEntryTrace(g_gameDevice.Get());
}

void AdvanceVerifiedGameDrawFunctionEntryTrace() { AdvanceD3D11FunctionEntryTrace(); }

void BeginVerifiedGameCameraUpdateTrace() {
    if (!g_gameDevice) { Tf2VrLog("[TF2VR] HOME: UpdateSubresource trace skipped; game swapchain not verified yet.\n"); return; }
    BeginCameraUpdateTrace(g_gameDevice.Get());
}

void BeginVerifiedGameCameraOriginOffsetTest() {
    if (!g_gameDevice) { Tf2VrLog("[TF2VR] END: offset test skipped; game swapchain not verified yet.\n"); return; }
    BeginCameraOriginOffsetTest(g_gameDevice.Get());
}

void ToggleVerifiedGameAlternateFrameStereo() {
    if (!g_gameDevice) { Tf2VrLog("[TF2VR] DEL: stereo skipped; game swapchain not verified yet.\n"); return; }
    if (g_stereoArmed.load(std::memory_order_acquire)) {
        g_stereoArmed.store(false, std::memory_order_release);
        EndStereoEyeOffset();
        std::scoped_lock lock(g_xrMutex);
        g_xr.SetStereoEnabled(false);
        Tf2VrLog("[TF2VR] DEL: alternate-frame stereo disarmed; camera restored.\n");
        return;
    }
    // ~30 minutes at 120 fps, as a backstop behind the toggle.
    if (!BeginStereoEyeOffset(g_gameDevice.Get(), -1.0f, 216000)) { Tf2VrLog("[TF2VR] DEL: stereo could not arm the camera hook.\n"); return; }
    g_currentEye.store(0, std::memory_order_release);
    { std::scoped_lock lock(g_xrMutex); g_xr.SetStereoEnabled(true); }
    g_stereoArmed.store(true, std::memory_order_release);
    Tf2VrLog("[TF2VR] DEL: alternate-frame stereo armed; each frame renders one eye.\n");
}

bool IsAlternateFrameStereoArmed() { return g_stereoArmed.load(std::memory_order_acquire); }

bool IsVerifiedGameSwapchainReady() { return g_gameDevice != nullptr; }


void RequestBackbufferContentSample(const char* thumbnailName) {
    std::snprintf(g_contentSampleName, sizeof(g_contentSampleName), "%s",
                  thumbnailName ? thumbnailName : "");
    g_contentSampleReady.store(false, std::memory_order_release);
    g_contentSamplePending.store(true, std::memory_order_release);
}

bool TakeBackbufferContentSample(BackbufferContent* out) {
    if (!g_contentSampleReady.load(std::memory_order_acquire)) return false;
    if (out) *out = g_contentSampleResult;
    g_contentSampleReady.store(false, std::memory_order_release);
    return true;
}

bool BackbufferContentSampleOutstanding() {
    return g_contentSamplePending.load(std::memory_order_acquire);
}

bool EnsureGameCameraDetoursInstalled() {
    if (!g_gameDevice) return false;
    return EnsureDetoursInstalled(g_gameDevice.Get());
}

bool VerifiedGameBackbufferSize(unsigned int& width, unsigned int& height) {
    width = 0;
    height = 0;
    IDXGISwapChain* swapchain = g_gameSwapchain.Get();
    if (!swapchain) return false;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapchain->GetDesc(&desc))) return false;
    width = desc.BufferDesc.Width;
    height = desc.BufferDesc.Height;
    return width > 0 && height > 0;
}

bool IsProjectionLayerArmed() {
    std::scoped_lock lock(g_xrMutex);
    return g_xr.IsProjectionLayerEnabled();
}

void ToggleVerifiedGameProjectionLayer() {
    std::scoped_lock lock(g_xrMutex);
    const bool enabled = !g_xr.IsProjectionLayerEnabled();
    g_xr.SetProjectionLayerEnabled(enabled);
    if (!enabled) {
        Tf2VrLog("[TF2VR] projection layer disabled; back to the verified quad path.\n");
        return;
    }
    // Say the preconditions out loud on arming. All three have to hold, and a
    // projection layer that silently falls back looks identical to one that is
    // simply not working.
    Tf2VrLog("[TF2VR] projection layer requested. It needs OpenXR armed, head tracking armed, and "
             "alternate-frame stereo armed; until then the quad path stays up and the next frame logs "
             "which precondition is missing.\n");
}

void BeginVerifiedGameStereoPairCapture() {
    if (!g_gameDevice) { Tf2VrLog("[TF2VR] PGDN: stereo capture skipped; game swapchain not verified yet.\n"); return; }
    if (g_stereoCaptureActive.load(std::memory_order_acquire)) { Tf2VrLog("[TF2VR] PGDN: stereo capture already running.\n"); return; }
    if (!BeginStereoEyeOffset(g_gameDevice.Get(), -1.0f)) { Tf2VrLog("[TF2VR] PGDN: stereo capture could not arm the camera hook.\n"); return; }
    g_stereoCaptureStep.store(0, std::memory_order_release);
    g_stereoCaptureActive.store(true, std::memory_order_release);
    Tf2VrLog("[TF2VR] PGDN: stereo pair capture armed (A/B left for noise floor, C right for signal).\n");
}

void BeginVerifiedGameMatrixEyeTranslationTest() {
    if (!g_gameDevice) { Tf2VrLog("[TF2VR] PGUP: eye-translation test skipped; game swapchain not verified yet.\n"); return; }
    BeginMatrixEyeTranslationTest(g_gameDevice.Get());
}

void BeginVerifiedGameViewmodelProbe() {
    if (!g_gameDevice) { Tf2VrLog("[TF2VR] F1: viewmodel probe skipped; game swapchain not verified yet.\n"); return; }
    BeginViewmodelProbe(g_gameDevice.Get());
}

void SetVerifiedGameViewmodelCompensation(bool enabled) {
    if (!g_gameDevice) {
        if (enabled) Tf2VrLog("[TF2VR] viewmodel compensation skipped; game swapchain not verified yet.\n");
        return;
    }
    SetViewmodelCompensationEnabled(g_gameDevice.Get(), enabled);
}

// See the note in present_hook.h. The ini is parsed before any device exists, so
// the want is held and applied on the first frame that has one.
namespace {
std::atomic_bool g_compensationWanted{false};
std::atomic_bool g_compensationApplied{false};
}  // namespace

// The wearer's spike marker reaches the XR context through here, because this
// is where the context actually lives. OUTSIDE the anonymous namespace above:
// inside it the definition has internal linkage and the plugin fails to link,
// which is how the first attempt at this went.
void XrReportRecentFrames() { g_xr.ReportRecentFrames(); }

void SetViewmodelCompensationWanted(bool wanted) {
    g_compensationWanted.store(wanted, std::memory_order_release);
    if (!wanted) g_compensationApplied.store(false, std::memory_order_release);
}

bool IsViewmodelCompensationWanted() {
    return g_compensationWanted.load(std::memory_order_acquire);
}

void TickViewmodelCompensationWanted() {
    if (!g_compensationWanted.load(std::memory_order_acquire)) return;
    if (g_compensationApplied.load(std::memory_order_acquire)) return;
    if (!g_gameDevice) return;
    g_compensationApplied.store(true, std::memory_order_release);
    SetViewmodelCompensationEnabled(g_gameDevice.Get(), true);
    Tf2VrLog("[TF2VR] viewmodel.compensation = 1 applied from the ini, now that the swapchain is "
             "verified. This is what installs the UpdateSubresource interception, and that "
             "interception is where the main-scene frustum is read -- with it off there is no "
             "tangent, no ADS ratio and no magnification. It ALSO enables the viewmodel "
             "counter-rotation, which is a correction and not an instrument: check its own line "
             "reads applied=0 skipped(no head data)=N before trusting a flat measurement taken "
             "with this armed.\n");
}

namespace {
void NativeSlotRenderDocMarker(unsigned int slot, bool begin) {
    if (slot < 1 || slot > 2 || g_unloading.load(std::memory_order_acquire) ||
        !GetModuleHandleA("renderdoc.dll")) return;
    std::scoped_lock lock(g_xrMutex);
    if (!g_gameDevice || g_unloading.load(std::memory_order_acquire)) return;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<ID3DUserDefinedAnnotation> annotation;
    g_gameDevice->GetImmediateContext(&context);
    if (!context || FAILED(context.As(&annotation)) || !annotation) return;
    if (begin) {
        annotation->BeginEvent(slot == 1 ? L"TF2VR native slot 1" : L"TF2VR native slot 2");
    } else {
        annotation->EndEvent();
    }
}
}

extern "C" void BeginNativeSlotRenderDocMarker(unsigned int slot) { NativeSlotRenderDocMarker(slot, true); }
extern "C" void EndNativeSlotRenderDocMarker(unsigned int slot) { NativeSlotRenderDocMarker(slot, false); }

void InstallPresentHook() {
    std::scoped_lock lock(g_mutex);
    if (g_originalPresent) return;
    g_unloading.store(false, std::memory_order_release);

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = 16;
    desc.BufferDesc.Height = 16;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 1;
    desc.OutputWindow = GetDesktopWindow();
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<IDXGISwapChain> temporarySwapchain;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &desc, &temporarySwapchain, &device, nullptr, &context))) return;

    auto** vtable = *reinterpret_cast<void***>(temporarySwapchain.Get());
    g_presentSlot = &vtable[kPresentVtableIndex];
    DWORD oldProtect = 0;
    if (!VirtualProtect(g_presentSlot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) return;
    g_originalPresent = reinterpret_cast<PresentFn>(*g_presentSlot);
    *g_presentSlot = reinterpret_cast<void*>(&HookPresent);
    DWORD ignored = 0;
    VirtualProtect(g_presentSlot, sizeof(void*), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), g_presentSlot, sizeof(void*));
    Tf2VrLog("[TF2VR] IDXGISwapChain::Present hook installed.\n");
}

double Tf2VrPresentStalenessMs() {
    const long long last = g_lastPresentQpc.load(std::memory_order_acquire);
    if (!last || !g_timing.frequency.QuadPart) return 0.0;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - last) * 1000.0 /
           static_cast<double>(g_timing.frequency.QuadPart);
}

void SetXrDecoupled(bool enabled) {
    std::scoped_lock lock(g_xrMutex);
    g_xr.SetDecoupled(enabled);
}

void SetPresentStaleThresholdMs(double ms) {
    // A threshold at or below a display period would make every frame an event.
    g_stale.thresholdMs = ms < 20.0 ? 20.0 : ms;
}

static void LogStaleWindowSummary() {
    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] STALE WINDOW summary: %u event(s) over %.0f ms across %llu presents (%.1f s of "
        "session); worst %.0f ms, %.0f ms stale in total. ZERO EVENTS with a non-zero present count "
        "means the image never froze this session; zero presents means this instrument never ran and "
        "the event count says nothing.\n",
        g_stale.events, g_stale.thresholdMs, g_stale.presents, g_stale.sessionMs / 1000.0,
        g_stale.worstMs, g_stale.totalStaleMs);
    Tf2VrLog(line);
}

void RemovePresentHook() {
    LogStaleWindowSummary();
    g_unloading.store(true, std::memory_order_release);
    while (g_presentInFlight.load(std::memory_order_acquire) != 0) Sleep(0);
    std::scoped_lock lock(g_mutex, g_xrMutex);
    g_xr.Shutdown();
    g_gameSwapchain.Reset();
    g_gameDevice.Reset();
    if (!g_originalPresent || !g_presentSlot) return;
    DWORD oldProtect = 0;
    if (VirtualProtect(g_presentSlot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        *g_presentSlot = reinterpret_cast<void*>(g_originalPresent);
        DWORD ignored = 0;
        VirtualProtect(g_presentSlot, sizeof(void*), oldProtect, &ignored);
    }
    g_presentSlot = nullptr;
}

void SetXrArmed(bool armed) {
    std::scoped_lock lock(g_xrMutex);
    g_xrArmed.store(armed, std::memory_order_release);
    g_xr.SetEnabled(armed);
}
void SetHeadLookArmed(bool armed) {
    std::scoped_lock lock(g_xrMutex);
    g_headLookArmed.store(armed, std::memory_order_release);
    g_xr.SetHeadLookEnabled(armed);
}
void SetXrForceZeroLayers(bool on) {
    std::scoped_lock lock(g_xrMutex);
    g_xr.SetForceZeroLayers(on);
}
bool IsXrArmed() { return g_xrArmed.load(); }

// ARMED IS NOT REACHING A WEARER'S EYES, and two runs were lost to the gap.
// auto-arm sets armed as soon as a swapchain is adopted; on 2026-09-07 the
// runtime then reported NO HEADSET and created no session, so armed stayed true
// for a monitor run. The overlay chose the XR panel texture on that basis and
// drew the identification prompt into a quad nothing was submitting -- 3600
// times, by its own heartbeat, while the wearer saw a bare screen.
//
// Anything deciding WHERE to put something a human has to read must ask this.
// Same lock discipline as IsXrDecoupled: called from the presenting thread with
// no XR lock already held.
bool IsXrPresentingToHeadset() {
    if (!g_xrArmed.load()) return false;
    std::scoped_lock lock(g_xrMutex);
    return g_xr.SessionRunning();
}
bool IsHeadLookArmed() { return g_headLookArmed.load(); }

// The bare-key state read for the F11 kill switch. Taken under the same lock
// the setter uses, because the pacing thread's lifetime hangs off it.
bool IsXrDecoupled() {
    std::scoped_lock lock(g_xrMutex);
    return g_xr.IsDecoupled();
}

float PresentedFramesPerSecond() {
    // Sampled off the free-running present counter rather than kept as a
    // running average, so it costs nothing on the presenting thread: the reader
    // does the arithmetic, and only while the panel is open.
    static std::uint32_t lastFrame = 0;
    static std::uint64_t lastTick = 0;
    static float lastFps = 0.0f;
    const std::uint64_t now = GetTickCount64();
    if (lastTick == 0) {
        lastTick = now;
        lastFrame = g_tf2vrPresentFrame;
        return 0.0f;
    }
    const std::uint64_t elapsed = now - lastTick;
    if (elapsed >= 500) {
        const std::uint32_t frames = g_tf2vrPresentFrame - lastFrame;
        lastFps = static_cast<float>(frames) * 1000.0f / static_cast<float>(elapsed);
        lastTick = now;
        lastFrame = g_tf2vrPresentFrame;
    }
    return lastFps;
}
