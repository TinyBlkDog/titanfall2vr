#include "placement_watchpoint.h"

#include "viewmodel_bones.h"
#include "diagnostics.h"
#include "viewmodel_instance.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Presented-frame count, defined in present_hook.cpp. Read from the exception
// handler, so it is a plain volatile word rather than an atomic.
extern "C" volatile std::uint32_t g_tf2vrPresentFrame;

namespace {

// F1 CORRECTED THE POSITION OFFSET. This probe first ran on +0x128 and
// reported zero writers -- true, and misleading, because the placement commit
// stores to +0x12C. The shared header owns both offsets now, so the probe and
// the pin cannot drift apart about what they point at.
constexpr std::size_t kOriginOffset = kViewmodelPositionOffset;
constexpr std::size_t kAnglesOffset = kViewmodelAnglesOffset;

// Two slots: DR0 on the origin, DR1 on the angles. DR2/DR3 are left alone so
// anything else in the process that wants a hardware breakpoint still can.
constexpr int kSlotOrigin = 0;
constexpr int kSlotAngles = 1;
constexpr int kSlotCount = 2;
const char* const kSlotName[kSlotCount] = {"position +0x12C", "angles +0x114"};

// 48 was enough for the camera angles. The placement is written from fewer
// places than that, and a table that fills silently is worse than one that
// reports being full -- so the count is reported either way.
constexpr std::uint32_t kMaxSites = 64;
// Generous: at ~120 fps a once-per-frame writer contributes 120 hits a second,
// so this is minutes of capture for the writer we want and a hard stop for a
// per-bone writer that would otherwise fire 87 times a frame.
constexpr std::uint32_t kMaxHits = 60000;
// Long enough for a once-per-frame writer to build a cadence nobody can argue
// with -- twelve seconds is well over a thousand frames -- and short enough
// that a pathological writer costs one unpleasant beat rather than a run.
constexpr std::uint64_t kCaptureMs = 12000;

struct Site {
    std::uintptr_t rip;
    std::uint32_t slot;
    std::uint32_t count;
    std::uint32_t firstFrame;
    std::uint32_t lastFrame;
};

Site g_sites[kMaxSites]{};
std::atomic_uint32_t g_siteCount = 0;
std::atomic_uint32_t g_hitCount = 0;
std::atomic_uint32_t g_overflowHits = 0;
std::atomic_bool g_armed = false;
PVOID g_handler = nullptr;
const std::uint8_t* g_instance = nullptr;
std::uintptr_t g_watchAddress[kSlotCount]{};
std::uint32_t g_armFrame = 0;
std::uint64_t g_armTick = 0;

// Called from the exception handler: no allocation, no logging, no lock. A
// fixed table with a linear scan is the whole design, exactly as in
// angle_watchpoint.cpp.
void RecordSite(std::uintptr_t rip, std::uint32_t slot, std::uint32_t frame) {
    const std::uint32_t count = g_siteCount.load(std::memory_order_acquire);
    for (std::uint32_t i = 0; i < count && i < kMaxSites; ++i) {
        // Keyed on the PAIR. One instruction can write both fields -- a 24-byte
        // copy of the whole placement would -- and collapsing that onto one row
        // would hide the very thing being measured.
        if (g_sites[i].rip == rip && g_sites[i].slot == slot) {
            ++g_sites[i].count;
            g_sites[i].lastFrame = frame;
            return;
        }
    }
    if (count >= kMaxSites) { g_overflowHits.fetch_add(1, std::memory_order_relaxed); return; }
    const std::uint32_t index = g_siteCount.fetch_add(1, std::memory_order_acq_rel);
    if (index >= kMaxSites) { g_overflowHits.fetch_add(1, std::memory_order_relaxed); return; }
    g_sites[index].rip = rip;
    g_sites[index].slot = slot;
    g_sites[index].count = 1;
    g_sites[index].firstFrame = frame;
    g_sites[index].lastFrame = frame;
}

LONG CALLBACK PlacementHandler(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD64 status = info->ContextRecord->Dr6;
    // Bits 0 and 1 are our DR0/DR1. Anything else is not ours and must be
    // passed on untouched -- swallowing another component's single-step would
    // be its own hard-to-find bug.
    if (!(status & 0x3)) return EXCEPTION_CONTINUE_SEARCH;

    const std::uint32_t frame = g_tf2vrPresentFrame;
    const auto rip = static_cast<std::uintptr_t>(info->ContextRecord->Rip);
    if (status & 0x1) RecordSite(rip, kSlotOrigin, frame);
    if (status & 0x2) RecordSite(rip, kSlotAngles, frame);
    info->ContextRecord->Dr6 = 0;
    if (g_hitCount.fetch_add(1, std::memory_order_acq_rel) + 1 >= kMaxHits) {
        // Self-limiting: clear this thread's control bits so a hot site cannot
        // spin the handler indefinitely if the disarm request is slow.
        info->ContextRecord->Dr7 &= ~static_cast<DWORD64>(0xF);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// DR7 for two write watchpoints of four bytes each.
//   L0 = bit 0, G0 = bit 1, L1 = bit 2, G1 = bit 3   -- enables
//   RW0 = bits 16-17, LEN0 = bits 18-19              -- slot 0 control
//   RW1 = bits 20-21, LEN1 = bits 22-23              -- slot 1 control
// RW = 01 is "data WRITES only" and LEN = 11 is four bytes, so each control
// nibble is 0xD. angle_watchpoint.cpp uses the same 0xD; its comment calling
// that "read-or-write" mis-describes the encoding while the mask itself is
// right, and this is the corrected reading.
//
// The mask clears the GLOBAL enables (bits 1 and 3) as well as the local ones.
// Setting only the locals while leaving a stale global set would leave the
// watchpoint live after disarm, on every thread, with our handler already
// removed -- an unhandled single-step per write, which is a crash and not a
// measurement.
constexpr DWORD64 kDr7Mask = 0x00FF000Full;
constexpr DWORD64 kDr7Bits = 0x00DD0005ull;
static_assert((kDr7Bits & ~kDr7Mask) == 0, "every bit armed must also be a bit disarm clears");

void ApplyDebugRegisters(CONTEXT& context, bool arm) {
    if (arm) {
        context.Dr0 = g_watchAddress[kSlotOrigin];
        context.Dr1 = g_watchAddress[kSlotAngles];
        context.Dr7 = (context.Dr7 & ~kDr7Mask) | kDr7Bits;
    } else {
        context.Dr0 = 0;
        context.Dr1 = 0;
        context.Dr7 &= ~kDr7Mask;
    }
}

// The calling thread must be covered too, and separately: it cannot be
// suspended by itself, and the snapshot walk below skips it for that reason.
// Omitting it is what made angle_watchpoint's first run return 0 hits across
// 111 threads -- the plugin callback runs on the game's main thread, which is
// the one thread guaranteed to matter and the one thread that was excluded.
void ApplyToCurrentThread(bool arm, std::uint32_t& touched) {
    CONTEXT context{};
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(GetCurrentThread(), &context)) return;
    ApplyDebugRegisters(context, arm);
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (SetThreadContext(GetCurrentThread(), &context)) ++touched;
}

void ForEachOtherThread(bool arm, std::uint32_t& touched) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
            HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                                       FALSE, entry.th32ThreadID);
            if (!thread) continue;
            if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(thread, &context)) {
                    ApplyDebugRegisters(context, arm);
                    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (SetThreadContext(thread, &context)) ++touched;
                }
                ResumeThread(thread);
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

bool ReadableSpan(const void* address, std::size_t bytes) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    return start + info.RegionSize - at >= bytes;
}

void DescribeRip(std::uintptr_t rip, const char** module, std::uintptr_t* rva) {
    struct Known { const char* name; HMODULE handle; };
    // engine and client cover everything seen so far; server is listed because
    // a shared prediction path would land there and reporting it as "unknown"
    // would be the one result nobody could act on.
    const Known known[] = {
        {"client.dll", GetModuleHandleA("client.dll")},
        {"engine.dll", GetModuleHandleA("engine.dll")},
        {"server.dll", GetModuleHandleA("server.dll")},
    };
    *module = "unknown";
    *rva = rip;
    for (const Known& entry : known) {
        if (!entry.handle) continue;
        const auto base = reinterpret_cast<std::uintptr_t>(entry.handle);
        if (rip <= base || rip - base >= 0x2000000) continue;
        *module = entry.name;
        *rva = rip - base;
        return;
    }
}

void Arm() {
    // The scan is a one-shot 4.7 GB walk with a real cost, so it runs only if
    // nothing is cached yet -- and it is what the pin arms through already.
    const std::uint8_t* instance = reinterpret_cast<const std::uint8_t*>(FindLiveViewmodelInstance());
    if (!instance) {
        Tf2VrLog("[TF2VR] F1 placement watchpoint: no live C_BaseViewModel cached; running the "
                 "bone scan first.\n");
        CacheWeaponBoneInstance();
        instance = reinterpret_cast<const std::uint8_t*>(FindLiveViewmodelInstance());
    }
    if (!instance) {
        Tf2VrLog("[TF2VR] F1 placement watchpoint: NOT ARMED -- the scan found no live "
                 "C_BaseViewModel. Be in a level, holding a weapon, before pressing this.\n");
        return;
    }
    // The placement fields must be readable before the CPU is asked to watch
    // them. A watchpoint on an unmapped address is a fault per access, not a
    // measurement.
    if (!ReadableSpan(instance + kAnglesOffset, 0x20)) {
        Tf2VrLog("[TF2VR] F1 placement watchpoint: NOT ARMED -- the placement fields are not "
                 "readable from the cached instance.\n");
        return;
    }

    g_instance = instance;
    g_watchAddress[kSlotOrigin] = reinterpret_cast<std::uintptr_t>(instance) + kOriginOffset;
    g_watchAddress[kSlotAngles] = reinterpret_cast<std::uintptr_t>(instance) + kAnglesOffset;
    // Four-byte watchpoints must be four-byte aligned or the CPU watches the
    // wrong span. 0x128 and 0x114 both are; this asserts it at runtime because
    // an instance base that is not 4-aligned would break the same guarantee.
    if ((g_watchAddress[kSlotOrigin] & 3) || (g_watchAddress[kSlotAngles] & 3)) {
        Tf2VrLog("[TF2VR] F1 placement watchpoint: NOT ARMED -- a watch address is not 4-byte "
                 "aligned, which would watch the wrong bytes.\n");
        return;
    }

    g_siteCount.store(0, std::memory_order_release);
    g_hitCount.store(0, std::memory_order_release);
    g_overflowHits.store(0, std::memory_order_release);
    std::memset(g_sites, 0, sizeof(g_sites));

    g_handler = AddVectoredExceptionHandler(1, PlacementHandler);
    if (!g_handler) {
        Tf2VrLog("[TF2VR] F1 placement watchpoint: AddVectoredExceptionHandler failed; not "
                 "arming.\n");
        return;
    }
    std::uint32_t touched = 0;
    std::uint32_t self = 0;
    ForEachOtherThread(true, touched);
    ApplyToCurrentThread(true, self);
    g_armFrame = g_tf2vrPresentFrame;
    g_armTick = GetTickCount64();
    g_armed.store(true, std::memory_order_release);

    // OUR OWN WRITERS WOULD APPEAR IN THE TABLE AS ENGINE WRITERS.
    //
    // The pose drive writes all six copies of the angle triple and the bone pin
    // writes the skeleton; either one armed during this capture puts
    // titanfall2vr.dll RIPs in the results, and "unknown+0x..." at a high
    // cadence is exactly what a real once-per-frame engine writer looks like.
    // This is the check the memory note calls for: a built lever left switched
    // on looks the same as a missing one, and here it looks like a FINDING.
    if (IsWeaponBonePinEnabled()) {
        Tf2VrLog("[TF2VR] F1 WARNING: the viewmodel pose drive and/or the bone pin is ARMED. Both "
                 "write the fields being watched, so this capture will contain OUR writers "
                 "alongside the engine's. Disarm them and re-arm the watchpoint.\n");
    }

    // The whole placement, logged at arm time. A writer table means nothing
    // unless the fields were actually carrying a pose when the capture started,
    // and this is the falsifier for "we watched the right object": these six
    // numbers must be a plausible map position and a plausible angle triple.
    float placement[6]{};
    std::memcpy(placement, instance + kAnglesOffset, 3 * sizeof(float));
    std::memcpy(placement + 3, instance + kOriginOffset, 3 * sizeof(float));
    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F1 placement watchpoint ARMED on C_BaseViewModel %p across %u other threads + %u "
        "calling thread. position +0x12C = (%.2f %.2f %.2f) at %p; angles +0x114 = (p%.2f y%.2f "
        "r%.2f) at %p. Writes only; closes itself after 12 s, or press the same key again to "
        "close it early.\n",
        static_cast<const void*>(instance), touched, self,
        placement[3], placement[4], placement[5],
        reinterpret_cast<void*>(g_watchAddress[kSlotOrigin]),
        placement[0], placement[1], placement[2],
        reinterpret_cast<void*>(g_watchAddress[kSlotAngles]));
    Tf2VrLog(line);
}

void Disarm() {
    std::uint32_t touched = 0;
    std::uint32_t self = 0;
    ForEachOtherThread(false, touched);
    ApplyToCurrentThread(false, self);
    if (g_handler) { RemoveVectoredExceptionHandler(g_handler); g_handler = nullptr; }
    g_armed.store(false, std::memory_order_release);

    const std::uint32_t frames = g_tf2vrPresentFrame - g_armFrame;
    const std::uint64_t elapsedMs = GetTickCount64() - g_armTick;
    const std::uint32_t hits = g_hitCount.load(std::memory_order_acquire);
    const std::uint32_t sites = g_siteCount.load(std::memory_order_acquire);
    const bool stillValid = ViewmodelInstanceStillValid(g_instance);

    char header[520]{};
    std::snprintf(header, sizeof(header),
        "[TF2VR] F1 placement watchpoint DISARMED: %u hits across %u distinct (site, field) pairs "
        "over %u presented frames / %llu ms. Instance %p %s. %s\n",
        hits, sites, frames, static_cast<unsigned long long>(elapsedMs),
        static_cast<const void*>(g_instance),
        stillValid ? "still valid" : "WENT AWAY DURING THE CAPTURE -- treat the table as suspect",
        hits >= kMaxHits ? "HIT CAP REACHED: the capture stopped early, so per-frame rates below "
                           "the cap point are still good but the totals are truncated."
                         : "");
    Tf2VrLog(header);
    if (const std::uint32_t overflow = g_overflowHits.load(std::memory_order_acquire)) {
        char line[220]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] F1: the site table filled; %u hits went unattributed. The rows below are the "
            "first %u pairs seen, not the busiest.\n", overflow, kMaxSites);
        Tf2VrLog(line);
    }
    if (!frames) {
        Tf2VrLog("[TF2VR] F1: zero presented frames elapsed, so no cadence can be computed. Hold "
                 "the capture open for several seconds.\n");
    }

    // THE DELIVERABLE. Sorted is not worth the code; the cadence column is what
    // is read, and a once-per-frame writer stands out at a glance.
    for (std::uint32_t i = 0; i < sites && i < kMaxSites; ++i) {
        const Site& site = g_sites[i];
        const char* module = "unknown";
        std::uintptr_t rva = 0;
        DescribeRip(site.rip, &module, &rva);
        // Cadence over the span this site was ACTUALLY active, not over the
        // whole capture: a writer that only runs while a weapon is deployed
        // would otherwise be averaged down by the frames before it started and
        // read as "less than once per frame" when it is exactly once.
        const std::uint32_t span = site.lastFrame - site.firstFrame + 1;
        const double perFrame = span ? static_cast<double>(site.count) / span : 0.0;
        char line[300]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] F1 site %2u: %s+0x%llX writes %s  hits=%u  frames %u..%u (span %u)  "
            "%.3f/frame\n",
            i, module, static_cast<unsigned long long>(rva), kSlotName[site.slot],
            site.count, site.firstFrame, site.lastFrame, span, perFrame);
        Tf2VrLog(line);
    }
    if (!sites) {
        Tf2VrLog("[TF2VR] F1: NO WRITERS AT ALL. That is the plan's abandon-if: the placement is "
                 "not written on this instance, so it is not entity-field-rooted here. Follow the "
                 "named fallback -- post-SetupBones rigid transform of the finished bone arrays -- "
                 "and do not grind this route.\n");
    }
    g_instance = nullptr;
}

// ---------------------------------------------------------------------------
// The rider: one frame of camera-pass matrices.
// ---------------------------------------------------------------------------
constexpr std::size_t kCameraOriginOffset = 4;
constexpr std::size_t kRelativeToClipOffset = 16;
// PERSPECTIVE PASSES ONLY, AND THAT IS WHY THE CAP CAN BE SMALL.
//
// The 2026-08-17 census counted about 27 camera-sized uploads in a frame and
// found the gun's own pass was the LAST of them. A flat cap on all uploads
// would therefore have truncated before reaching the one pass this dump exists
// to capture. m[15] == 0 selects perspective and drops the orthographic UI and
// shadow-cascade flood, which is nearly all of that 27 -- leaving the main
// scene, the 3D skybox and the near-plane-1 family, and those fit easily.
constexpr std::uint32_t kMaxDumpedPasses = 16;

std::atomic_bool g_dumpRequested = false;
std::uint32_t g_dumpFrame = 0;
std::uint32_t g_dumpedPasses = 0;
std::uint32_t g_dumpUploadOrdinal = 0;
bool g_dumping = false;

}  // namespace

void ToggleViewmodelPlacementWatchpoint() {
    if (g_armed.load(std::memory_order_acquire)) Disarm();
    else Arm();
}

void AdvanceViewmodelPlacementWatchpoint() {
    if (!g_armed.load(std::memory_order_acquire)) return;
    const bool timeUp = GetTickCount64() - g_armTick >= kCaptureMs;
    const bool capped = g_hitCount.load(std::memory_order_acquire) >= kMaxHits;
    if (!timeUp && !capped) return;
    Tf2VrLog(capped
        ? "[TF2VR] F1: hit cap reached; closing the capture.\n"
        : "[TF2VR] F1: capture window elapsed; closing.\n");
    Disarm();
}

bool IsViewmodelPlacementWatchpointArmed() { return g_armed.load(std::memory_order_acquire); }

bool IsCameraPassMatrixDumpPending() {
    return g_dumpRequested.load(std::memory_order_acquire);
}

void RequestCameraPassMatrixDump() {
    g_dumpedPasses = 0;
    g_dumping = false;
    g_dumpRequested.store(true, std::memory_order_release);
    Tf2VrLog("[TF2VR] camera pass matrix dump requested: the next frame's camera uploads, in "
             "full, once.\n");
}

void OfferCameraPassMatrix(const unsigned char* bytes) {
    if (!bytes) return;
    if (!g_dumping) {
        if (!g_dumpRequested.load(std::memory_order_acquire)) return;
        // Latch the frame on the first upload seen, so the dump is ONE frame's
        // worth of passes and not a slice across two. Two view builds per
        // presented frame is a known property here, so the frame number is the
        // only thing that keeps the set coherent.
        g_dumpFrame = g_tf2vrPresentFrame;
        g_dumpUploadOrdinal = 0;
        g_dumping = true;
    }
    if (g_tf2vrPresentFrame != g_dumpFrame || g_dumpedPasses >= kMaxDumpedPasses) {
        g_dumping = false;
        g_dumpRequested.store(false, std::memory_order_release);
        return;
    }
    // The ordinal counts EVERY camera-sized upload, not just the dumped ones,
    // so "the gun is the last upload of the frame" stays checkable against the
    // census that established it.
    const std::uint32_t ordinal = g_dumpUploadOrdinal++;
    float origin[3]{};
    float m[16]{};
    std::memcpy(origin, bytes + kCameraOriginOffset, sizeof(origin));
    std::memcpy(m, bytes + kRelativeToClipOffset, sizeof(m));
    if (m[15] != 0.0f) return;  // orthographic: UI and shadow cascades.
    ++g_dumpedPasses;
    // Row by row, whole. P0 §6 needs the matrix, not a summary of it: |r0| and
    // the near plane were the summary, and they are exactly what could not tell
    // a world-oriented pass from a projection-only one.
    char line[520]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] pass f%u #%u origin=(%.3f %.3f %.3f) m11=%.4f m15=%.4f\n"
        "[TF2VR]   r0=(%.6f %.6f %.6f | %.6f)\n"
        "[TF2VR]   r1=(%.6f %.6f %.6f | %.6f)\n"
        "[TF2VR]   r2=(%.6f %.6f %.6f | %.6f)\n"
        "[TF2VR]   r3=(%.6f %.6f %.6f | %.6f)\n",
        g_dumpFrame, ordinal, origin[0], origin[1], origin[2], m[11], m[15],
        m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
        m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    Tf2VrLog(line);
}
