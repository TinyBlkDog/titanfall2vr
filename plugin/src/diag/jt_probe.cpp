#include "jt_probe.h"

#include "diagnostics.h"
#include "scene_reentry.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

// Decorated names, verified against tier0.dll's export directory offline
// (STAGE-O-RESULT-2026-08-29 section 5): ordinals 104, 113 and 95, RVAs 0x8D00,
// 0x9210 and 0x7B10. GetProcAddress is used rather than an RVA so a game patch
// that moves them fails LOUDLY here instead of reading a wrong address.
constexpr const char* kNumWorkerThreads = "?JT_NumWorkerThreads@@YAIXZ";
constexpr const char* kThreadIndexEnd = "?JT_ThreadIndexEnd@@YAIXZ";
constexpr const char* kGetCurrentThread = "?JT_GetCurrentThread@@YAIXZ";

using UintFn = unsigned int(__cdecl*)();

UintFn g_numWorkerThreads = nullptr;
UintFn g_threadIndexEnd = nullptr;
UintFn g_getCurrentThread = nullptr;
std::atomic<bool> g_resolved{false};
bool g_resolveAttempted = false;
bool g_bannerLogged = false;
std::atomic_int g_mode{0};

// STAGE-O-RESULT section 2.2. The scene draw's three JT_EndJobGroup calls all
// take their JobID from ONE global, and JT_EndJobGroup is a refcount release
// rather than an idempotent flush -- so a second pass releases a group its own
// pass never acquired. F0 took this reading's RESTING shape (id changes every
// frame, refcount 1 at every sample); F1 takes it across the nested call.
constexpr std::uintptr_t kClientViewJobIdGlobal = 0xEA9EE4;
constexpr std::uintptr_t kTier0JobGroupTable = 0x74F00;
constexpr std::uintptr_t kJobGroupStride = 0x40;
constexpr std::uintptr_t kJobGroupRefCountOffset = 0x14;

bool IsReadable(const void* address, std::size_t bytes) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!address || !VirtualQuery(address, &mbi, sizeof(mbi))) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto end = begin + bytes;
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    const DWORD protect = mbi.Protect & 0xFF;
    return mbi.State == MEM_COMMIT && end >= begin && end <= regionEnd &&
        protect != PAGE_NOACCESS && protect != PAGE_GUARD;
}

bool ResolveExports() {
    if (g_resolveAttempted) return g_resolved.load(std::memory_order_acquire);
    HMODULE tier0 = GetModuleHandleA("tier0.dll");
    if (!tier0) return false;  // Not loaded yet; try again next frame.
    g_resolveAttempted = true;
    g_numWorkerThreads = reinterpret_cast<UintFn>(GetProcAddress(tier0, kNumWorkerThreads));
    g_threadIndexEnd = reinterpret_cast<UintFn>(GetProcAddress(tier0, kThreadIndexEnd));
    g_getCurrentThread = reinterpret_cast<UintFn>(GetProcAddress(tier0, kGetCurrentThread));

    // EVERY FAILED RESOLVE IS NAMED. A null read back as 0 would look exactly
    // like "zero workers", which is the one answer this probe must never
    // fabricate.
    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] JT probe: resolve NumWorkerThreads=%s ThreadIndexEnd=%s GetCurrentThread=%s.\n",
        g_numWorkerThreads ? "ok" : "FAILED",
        g_threadIndexEnd ? "ok" : "FAILED",
        g_getCurrentThread ? "ok" : "FAILED");
    Tf2VrLog(line);
    const bool ok = g_numWorkerThreads && g_threadIndexEnd && g_getCurrentThread;
    g_resolved.store(ok, std::memory_order_release);
    return ok;
}
}  // namespace

bool ReadViewJobGroup(std::uint32_t* idOut, std::int32_t* refCountOut) {
    HMODULE client = GetModuleHandleA("client.dll");
    HMODULE tier0 = GetModuleHandleA("tier0.dll");
    if (!client || !tier0 || !idOut || !refCountOut) return false;
    const auto* idAddress = reinterpret_cast<const std::uint32_t*>(
        reinterpret_cast<std::uint8_t*>(client) + kClientViewJobIdGlobal);
    if (!IsReadable(idAddress, sizeof(std::uint32_t))) return false;
    const std::uint32_t id = *idAddress;
    *idOut = id;
    *refCountOut = 0;
    const auto* record = reinterpret_cast<const std::uint8_t*>(tier0) + kTier0JobGroupTable +
        (id & 0xFFF) * kJobGroupStride;
    if (!IsReadable(record + kJobGroupRefCountOffset, sizeof(std::int32_t))) return false;
    std::memcpy(refCountOut, record + kJobGroupRefCountOffset, sizeof(std::int32_t));
    return true;
}

int JtCurrentThreadIndex() {
    if (!g_resolved.load(std::memory_order_acquire) || !g_getCurrentThread) return -1;
    return static_cast<int>(g_getCurrentThread());
}

void SetJtProbeMode(int mode) {
    const int clamped = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
    g_mode.store(clamped, std::memory_order_release);
    // Mode 2 asks scene_reentry for the caller census. The hook itself lives
    // there now, because F1 shares it and this file's contract is read-only.
    SetSceneCensusWanted(clamped >= 2);
    static const char* const kNames[] = {
        "[TF2VR] jt.probe OFF.\n",
        "[TF2VR] jt.probe: JT counters only. Read-only, no interception.\n",
        "[TF2VR] jt.probe: JT counters plus the scene-draw caller census.\n",
    };
    Tf2VrLog(kNames[clamped]);
}

int JtProbeMode() { return g_mode.load(std::memory_order_acquire); }

void AdvanceJtProbe(bool worldReady) {
    const int mode = g_mode.load(std::memory_order_acquire);
    if (mode <= 0) return;
    if (!ResolveExports()) return;

    if (!g_bannerLogged) {
        g_bannerLogged = true;
        // THE ARM LABEL, READ AT THE MEASUREMENT. The command line is logged
        // beside the counts so the run's own log says which arm produced them.
        // The positive control lives in the comparison between two runs of this
        // one line: identical counts in both arms means the switch was not
        // honoured and the run is VOID.
        const char* commandLine = GetCommandLineA();
        char line[900]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] F0: JT_NumWorkerThreads=%u JT_ThreadIndexEnd=%u "
            "(Stage O: threadSlots = min(workers + 2, 28), so these two must differ by 2 "
            "until the 28 cap binds). cmdline=%s\n",
            g_numWorkerThreads(), g_threadIndexEnd(), commandLine ? commandLine : "(null)");
        Tf2VrLog(line);
    }

    static std::uint64_t lastTick = 0;
    const std::uint64_t now = GetTickCount64();
    if (now - lastTick < 5000) return;
    lastTick = now;

    std::uint32_t jobId = 0;
    std::int32_t refCount = 0;
    const bool jobRead = ReadViewJobGroup(&jobId, &refCount);

    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F0 status: workers=%u threadSlots=%u thisThreadJtIndex=%d world=%d "
        "viewJobGroup=%s id=0x%08X slot=%u refcount=%d\n",
        g_numWorkerThreads(), g_threadIndexEnd(), JtCurrentThreadIndex(),
        worldReady ? 1 : 0, jobRead ? "read" : "UNREADABLE", jobId, jobId & 0xFFF, refCount);
    Tf2VrLog(line);
}
