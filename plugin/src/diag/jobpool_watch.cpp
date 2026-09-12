#include "jobpool_watch.h"

#include "diagnostics.h"
#include "jt_probe.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace {

// Every one of these is quoted with the instruction that establishes it in
// docs/O1-O2-RESULT-2026-08-31.md, so a game patch that moves them can be
// re-derived rather than guessed at again.
constexpr std::uintptr_t kTier0GlobalBatchStore = 0x6EA80;   // 0x8322: = 0x3F8000000000
constexpr std::uintptr_t kTier0ThreadRecords = 0x6E140;      // 0x56A5: + jtIndex*0x40
constexpr std::uintptr_t kTier0ThreadRecordStride = 0x40;
constexpr std::uintptr_t kTier0LocalRingHead = 0x1C;         // 0x56B3, & 0xF at 0x57A0
constexpr std::uintptr_t kTier0LocalRingTail = 0x1E;         // 0x56B7
constexpr const char* kGetCurrentThread = "?JT_GetCurrentThread@@YAIXZ";

// PLAN-128 section 1.1's handle pool, carried as a rider so P4 is answered in
// the same run rather than assumed away.
constexpr std::uintptr_t kClientHandleCount = 0x2777A58;
constexpr std::uintptr_t kClientHandleFreed = 0x2777A5C;
// NOT A LATCH -- A JOBID. `client+0x3728B6` loads this into r9d and passes it as
// arg4 to client+0x3756A0, which at +0x37 does `mov edi, r9d` and at +0xAE
// passes it as ecx to client+0x3748A0, which does:
//     test ecx, ecx / je   (0 means SKIP)
//     JT_WaitForJobAndOnlyHelpWithJobTypes(ecx, 0, -1)
// and the draw then zeroes it at +0x3728D7 because the job has been consumed.
// So pass 2 reading 0 is CORRECT -- it is the engine's own "nothing to wait
// for" path -- and R2's restore made pass 2 wait a second time on a consumed
// id whose slot may since have been recycled. That is why R2 crashed at 101.
constexpr std::uintptr_t kViewRenderLatch = 0xF1C80;
// The job-group record table, from O2's read of the initializer at tier0+0x8210:
// 1024 records of 0x40 at tier0+0x74F00. The slot index is TEN bits (`and eax,
// 0x3FF` at tier0+0x57D2) -- jt_probe.cpp's 0xFFF is loose; use the real mask.
constexpr std::uintptr_t kTier0JobRecords = 0x74F00;
constexpr std::uintptr_t kJobRecordStride = 0x40;
constexpr std::uint32_t kJobSlotMask = 0x3FF;

// The initializer seeds tail=127, head=0. Threads that have allocated hold up
// to 16 slots each in their local rings, so the resting reading is a little
// under 127 rather than exactly 127 -- hence a floor rather than an equality.
// Below this the reading is not "low", it is WRONG, and it is labelled so.
constexpr unsigned kRestingFloor = 64;
constexpr unsigned kRingCapacity = 128;

// PRINT TO 160. The wall is at ~128 doubled frames; a cap at 100 or at 128
// would put the end of the log and the end of the process at the same place
// and settle nothing.
constexpr unsigned long long kPerFrameLinesUntil = 160;

using UintFn = unsigned int(__cdecl*)();

std::atomic_bool g_resolveAttempted{false};
std::atomic_bool g_ready{false};
std::uint8_t* g_tier0 = nullptr;
std::uint8_t* g_client = nullptr;
UintFn g_getCurrentThread = nullptr;

std::atomic_uint64_t g_doubledSeen{0};
std::atomic_uint64_t g_undoubledSeen{0};
std::atomic<unsigned> g_firstFree{kRingCapacity};
std::atomic<unsigned> g_lastFree{kRingCapacity};
std::atomic<unsigned> g_lowFree{kRingCapacity};
std::atomic_uint64_t g_freeAtArm{kRingCapacity};
std::atomic_uint64_t g_armTickMs{0};
std::atomic_uint64_t g_armDoubledOrdinal{0};
std::atomic_bool g_voidReported{false};

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

// The one read the whole rung turns on. Returns false only when the store
// itself is unreadable, which is a different answer from "empty" and is
// reported as such.
bool ReadGlobalStore(std::uint64_t* rawOut, unsigned* headOut, unsigned* tailOut,
                     unsigned* freeOut) {
    if (!g_tier0) return false;
    const auto* p = reinterpret_cast<const volatile std::uint64_t*>(g_tier0 + kTier0GlobalBatchStore);
    const std::uint64_t raw = *p;
    const unsigned head = static_cast<unsigned>((raw >> 32) & 0x7F);
    const unsigned tail = static_cast<unsigned>((raw >> 39) & 0x7F);
    *rawOut = raw;
    *headOut = head;
    *tailOut = tail;
    // tier0+0x5670 treats head == tail as EMPTY, so the occupancy is the
    // forward distance from head to tail in a 128-entry ring.
    *freeOut = (tail - head) & 0x7F;
    return true;
}

bool ReadLocalRing(int jtIndex, unsigned* headOut, unsigned* tailOut, unsigned* freeOut) {
    if (!g_tier0 || jtIndex < 0 || jtIndex > 31) return false;
    const std::uint8_t* rec =
        g_tier0 + kTier0ThreadRecords + static_cast<std::uintptr_t>(jtIndex) * kTier0ThreadRecordStride;
    if (!IsReadable(rec, kTier0ThreadRecordStride)) return false;
    const unsigned head = *reinterpret_cast<const volatile std::uint16_t*>(rec + kTier0LocalRingHead);
    const unsigned tail = *reinterpret_cast<const volatile std::uint16_t*>(rec + kTier0LocalRingTail);
    *headOut = head;
    *tailOut = tail;
    *freeOut = (tail - head) & 0xF;
    return true;
}

// THE JOB THE SCENE DRAW WAITS ON. Decodes the id in this+0xF1C80 against the
// record table and reports the fields JT_EndJobGroup's release path
// (tier0+0x8D10) actually tests: the refcount at +0x14, the flag byte at +0x11
// whose sign bit selects the "still growing" path, and the parent at +0x18.
//
// A job that never completes is the whole hang, so this says what state it is
// in on the frames leading up to the wall -- and whether frame 130's id looks
// any different from frame 1's.
void DescribeWaitedJob(std::uint32_t id, char* out, std::size_t outSize) {
    if (id == 0) {
        std::snprintf(out, outSize, "id=0 (engine's own SKIP-THE-WAIT path)");
        return;
    }
    // Every id JT_BeginJobGroup returns carries 0x3000 (tier0+0x6C03). An id
    // without it did not come from there and must not be decoded as a slot.
    const bool wellFormed = (id & 0x3000) == 0x3000;
    if (!g_tier0 || !wellFormed) {
        std::snprintf(out, outSize, "id=0x%08X MALFORMED (no 0x3000 tag) -- not decoded", id);
        return;
    }
    const std::uint8_t* rec = g_tier0 + kTier0JobRecords +
        static_cast<std::uintptr_t>(id & kJobSlotMask) * kJobRecordStride;
    if (!IsReadable(rec, kJobRecordStride)) {
        std::snprintf(out, outSize, "id=0x%08X slot=%u RECORD UNREADABLE", id, id & kJobSlotMask);
        return;
    }
    const std::int32_t refCount = *reinterpret_cast<const volatile std::int32_t*>(rec + 0x14);
    const std::uint8_t flags = *reinterpret_cast<const volatile std::uint8_t*>(rec + 0x11);
    const std::uint32_t parent = *reinterpret_cast<const volatile std::uint32_t*>(rec + 0x18);
    const std::uint32_t gen = *reinterpret_cast<const volatile std::uint32_t*>(rec + 0x24);
    std::snprintf(out, outSize,
                  "id=0x%08X slot=%u refcount=%d flags=0x%02X parent=0x%08X gen=0x%08X",
                  id, id & kJobSlotMask, refCount, flags, parent, gen);
}

}  // namespace

bool JobPoolWatchReady() {
    if (g_resolveAttempted.load(std::memory_order_acquire)) return g_ready.load(std::memory_order_acquire);

    HMODULE tier0 = GetModuleHandleA("tier0.dll");
    HMODULE client = GetModuleHandleA("client.dll");
    if (!tier0) return false;  // Not loaded yet; try again next frame, do not latch.
    g_resolveAttempted.store(true, std::memory_order_release);
    g_tier0 = reinterpret_cast<std::uint8_t*>(tier0);
    g_client = reinterpret_cast<std::uint8_t*>(client);
    // RESOLVED HERE RATHER THAN BORROWED FROM jt_probe, which only resolves
    // when `jt.probe` is non-zero. A watch that silently depends on another
    // module's toggle is a watch that reads -1 in exactly the run that matters.
    g_getCurrentThread = reinterpret_cast<UintFn>(GetProcAddress(tier0, kGetCurrentThread));

    const bool storeReadable =
        IsReadable(g_tier0 + kTier0GlobalBatchStore, sizeof(std::uint64_t));
    std::uint64_t raw = 0;
    unsigned head = 0, tail = 0, freeBatches = 0;
    const bool read = storeReadable && ReadGlobalStore(&raw, &head, &tail, &freeBatches);

    // THE POSITIVE CONTROL, STATED BEFORE ANY MEASUREMENT IS TRUSTED. The
    // initializer at tier0+0x8210 stores head=0 tail=127, so a healthy process
    // reads near 127. Anything below the floor means the address or the decode
    // is wrong and every later number is VOID, not low.
    const bool plausible = read && freeBatches >= kRestingFloor && freeBatches < kRingCapacity;
    g_ready.store(plausible, std::memory_order_release);
    if (plausible) {
        g_firstFree.store(freeBatches, std::memory_order_relaxed);
        g_lastFree.store(freeBatches, std::memory_order_relaxed);
        g_lowFree.store(freeBatches, std::memory_order_relaxed);
    }

    char line[720]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] R1 JOB-POOL WATCH: tier0=%p store=tier0+0x%llX raw=0x%016llX head=%u tail=%u "
        "batchesFree=%u (ring 128, initializer seeds head=0 tail=127, 8 slots per batch, "
        "1016 free slots of 1024 records at tier0+0x74F00). getCurrentThread=%s. "
        "POSITIVE CONTROL: %s -- %s\n",
        static_cast<void*>(g_tier0), static_cast<unsigned long long>(kTier0GlobalBatchStore),
        static_cast<unsigned long long>(raw), head, tail, freeBatches,
        g_getCurrentThread ? "ok" : "FAILED",
        plausible ? "PASS" : "FAIL",
        plausible
            ? "the resting reading is a plausible fraction of 127, so later readings mean what "
              "they say. Exhaustion (0) makes tier0+0x5670 park in the JT help/sleep loop -- a "
              "hang, not a crash."
            : "the first reading is NOT near 127. The address or the decode is wrong: every "
              "batchesFree below is VOID, NOT ZERO, and no conclusion may be drawn from it.");
    Tf2VrLog(line);
    return plausible;
}

void JobPoolWatchNoteArm(const char* why) {
    JobPoolWatchReady();
    const std::uint64_t now = GetTickCount64();
    g_armTickMs.store(now, std::memory_order_release);
    g_armDoubledOrdinal.store(g_doubledSeen.load(std::memory_order_acquire), std::memory_order_release);

    std::uint64_t raw = 0;
    unsigned head = 0, tail = 0, freeBatches = 0;
    const bool read = ReadGlobalStore(&raw, &head, &tail, &freeBatches);
    if (read) g_freeAtArm.store(freeBatches, std::memory_order_release);

    char line[520]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] R1 ARM at t=%llu ms (%s): batchesFree=%s%u, doubledSoFar=%llu. Every line below "
        "carries this timestamp; a sample stamped EARLIER than this is VOID, not zero.\n",
        static_cast<unsigned long long>(now), why ? why : "(unnamed)",
        read ? "" : "UNREADABLE:", freeBatches,
        static_cast<unsigned long long>(g_doubledSeen.load(std::memory_order_acquire)));
    Tf2VrLog(line);
}

void JobPoolWatchSample(const char* where, void* viewRender, bool doubled, bool newFrame) {
    // OFF MEANS FREE, AND IT DID NOT USED TO.
    //
    // This runs TWICE PER SCENE DRAW, on every frame, for every player. The
    // guard below it cannot stop that: JobPoolWatchReady() sets
    // g_resolveAttempted on its first call, so from the second frame onward the
    // second half of that && is always false and the early return can never
    // fire again. Nothing here consulted jt.probe at all, so the pool read, the
    // low-water CAS loop and the periodic log line ran whether the probe was
    // asked for or not.
    //
    // That is the same shape as the diagnostics the 2026-09-08 performance work
    // found armed by default -- reporting built around a mechanism and left
    // switched on. The wearer's requirement is that the unfinished same-frame
    // stereo path costs a player nothing, and this sits directly on it.
    if (JtProbeMode() == 0) return;
    if (!JobPoolWatchReady() && !g_resolveAttempted.load(std::memory_order_acquire)) return;

    // THE ORDINAL IS THE FRAME NUMBER. Only the first sample of a frame
    // advances it, so "#129" in the log means the 129th doubled frame and can
    // be read straight against the wall.
    auto& counter = doubled ? g_doubledSeen : g_undoubledSeen;
    const unsigned long long ordinal = newFrame
        ? counter.fetch_add(1, std::memory_order_acq_rel) + 1
        : counter.load(std::memory_order_acquire);

    std::uint64_t raw = 0;
    unsigned head = 0, tail = 0, freeBatches = 0;
    const bool read = ReadGlobalStore(&raw, &head, &tail, &freeBatches);
    if (read) {
        g_lastFree.store(freeBatches, std::memory_order_relaxed);
        unsigned low = g_lowFree.load(std::memory_order_relaxed);
        while (freeBatches < low &&
               !g_lowFree.compare_exchange_weak(low, freeBatches, std::memory_order_relaxed)) {
        }
    }

    // THE PRINT GATE. Every doubled frame to 160 -- past the 128/129 wall on
    // purpose -- then one in eight. Undoubled frames are the control and only
    // need a trend, so they print sparsely; but the FIRST four always print, or
    // the control has no baseline to be a control against.
    const bool print = doubled
        ? (ordinal <= kPerFrameLinesUntil || (ordinal % 8) == 0)
        : (ordinal <= 4 || (ordinal % 64) == 0);
    if (!print) return;

    const int jtIndex = g_getCurrentThread ? static_cast<int>(g_getCurrentThread()) : -1;
    unsigned lHead = 0, lTail = 0, lFree = 0;
    const bool localRead = ReadLocalRing(jtIndex, &lHead, &lTail, &lFree);

    // P2's latch, and P4's handle pool. Both read-only riders; both cost one
    // load each and retire a hypothesis the plan would otherwise spend a run on.
    unsigned latch = 0;
    bool latchRead = false;
    if (viewRender && IsReadable(reinterpret_cast<std::uint8_t*>(viewRender) + kViewRenderLatch, 4)) {
        latch = *reinterpret_cast<const volatile std::uint32_t*>(
            reinterpret_cast<std::uint8_t*>(viewRender) + kViewRenderLatch);
        latchRead = true;
    }
    unsigned handleCount = 0, handleFreed = 0;
    bool handleRead = false;
    if (g_client && IsReadable(g_client + kClientHandleCount, 8)) {
        handleCount = *reinterpret_cast<const volatile std::uint32_t*>(g_client + kClientHandleCount);
        handleFreed = *reinterpret_cast<const volatile std::uint32_t*>(g_client + kClientHandleFreed);
        handleRead = true;
    }

    char fence[220]{};
    JobPoolWatchDescribeEngineFence(fence, sizeof(fence));
    char job[200]{};
    if (latchRead) {
        DescribeWaitedJob(latch, job, sizeof(job));
    } else {
        std::snprintf(job, sizeof(job), "UNREADABLE");
    }

    const std::uint64_t now = GetTickCount64();
    const std::uint64_t arm = g_armTickMs.load(std::memory_order_acquire);
    // A READING OLDER THAN THE ARM IS VOID AND SAYS SO IN THE LOG. It is not a
    // zero, it is not a low, and it is not evidence.
    const char* validity = (arm == 0)
        ? "PRE-ARM (no arm yet; this is the resting control)"
        : (now < arm ? "VOID: SAMPLE PREDATES THE ARM" : "post-arm");

    if (!g_ready.load(std::memory_order_acquire) && !g_voidReported.exchange(true)) {
        Tf2VrLog("[TF2VR] R1: the pool watch FAILED its positive control at resolve. Every "
                 "batchesFree line in this run is VOID, NOT ZERO.\n");
    }

    char line[760]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] R1 pool %s #%llu (%s): batchesFree=%s%u/127 head=%u tail=%u raw=0x%016llX | "
        "jt[%d] localRing=%s%u/16 (h=%u t=%u) | ENGINE FENCE %s | WAITED JOB %s | raw=%s0x%X | "
        "handlePool count=%s%u freed=%u | t=%llu arm=%llu %s%s\n",
        doubled ? "DOUBLED" : "undoubled", ordinal, where ? where : "?",
        read ? "" : "UNREADABLE:", freeBatches, head, tail,
        static_cast<unsigned long long>(raw),
        jtIndex, localRead ? "" : "UNREADABLE:", lFree, lHead, lTail, fence, job,
        latchRead ? "" : "UNREADABLE:", latch,
        handleRead ? "" : "UNREADABLE:", handleCount, handleFreed,
        static_cast<unsigned long long>(now), static_cast<unsigned long long>(arm), validity,
        g_ready.load(std::memory_order_acquire) ? "" : " [INSTRUMENT VOID]");
    Tf2VrLog(line);
}

void JobPoolWatchSummary(const char* tag) {
    if (!g_resolveAttempted.load(std::memory_order_acquire)) return;
    const unsigned first = g_firstFree.load(std::memory_order_relaxed);
    const unsigned last = g_lastFree.load(std::memory_order_relaxed);
    const unsigned low = g_lowFree.load(std::memory_order_relaxed);
    const unsigned atArm = static_cast<unsigned>(g_freeAtArm.load(std::memory_order_relaxed));
    const unsigned long long doubled = g_doubledSeen.load(std::memory_order_relaxed);
    const unsigned long long armOrdinal = g_armDoubledOrdinal.load(std::memory_order_relaxed);
    const unsigned long long since = doubled > armOrdinal ? doubled - armOrdinal : 0;
    // The number the whole rung exists to produce: batches consumed per doubled
    // frame. 1.0 names the resource outright; 0.0 with the wall still at 128
    // refutes the pool and sends R2 elsewhere.
    const double drain = since ? (static_cast<double>(atArm) - static_cast<double>(low)) /
                                     static_cast<double>(since)
                               : 0.0;
    char line[640]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] R1 POOL SUMMARY (%s): first=%u atArm=%u low=%u last=%u of 127 | doubledFrames=%llu "
        "(%llu since arm) | drain=%.3f batches per doubled frame (x8 slots). %s%s\n",
        tag ? tag : "?", first, atArm, low, last, doubled, since, drain,
        g_ready.load(std::memory_order_acquire) ? "" : "INSTRUMENT VOID -- ",
        low == 0 ? "REACHED ZERO: tier0+0x5670 parks in the JT help/sleep loop here."
                 : "Did not reach zero.");
    Tf2VrLog(line);
}

namespace {
std::atomic<unsigned int> g_lastWaitedJob{0};
}  // namespace

void JobPoolWatchDescribeJob(unsigned int id, char* out, unsigned long long outSize) {
    JobPoolWatchReady();
    DescribeWaitedJob(static_cast<std::uint32_t>(id), out, static_cast<std::size_t>(outSize));
}

void JobPoolWatchNoteWaitedJob(unsigned int id) {
    g_lastWaitedJob.store(id, std::memory_order_release);
}

unsigned int JobPoolWatchLastWaitedJob() {
    return g_lastWaitedJob.load(std::memory_order_acquire);
}

int JobPoolWatchJobComplete(unsigned int id) {
    JobPoolWatchReady();
    if (id == 0) return 1;                       // nothing to wait for
    if ((id & 0x3000) != 0x3000) return -1;      // not a JT_BeginJobGroup id
    if (!g_tier0) return -1;
    const std::uint8_t* rec = g_tier0 + kTier0JobRecords +
        static_cast<std::uintptr_t>(id & kJobSlotMask) * kJobRecordStride;
    if (!IsReadable(rec, kJobRecordStride)) return -1;
    const std::uint32_t live = *reinterpret_cast<const volatile std::uint32_t*>(rec + 0x24);
    // The engine's own comparison, byte for byte.
    return ((live & 0xFFFFC000u) != (id & 0xFFFFC000u)) ? 1 : 0;
}

void JobPoolWatchDescribeEngineFence(char* out, unsigned long long outSize) {
    HMODULE engine = GetModuleHandleA("engine.dll");
    if (!engine) { std::snprintf(out, static_cast<std::size_t>(outSize), "engine.dll not loaded"); return; }
    auto* base = reinterpret_cast<std::uint8_t*>(engine);
    // engine+0xB6C30's own globals, quoted with the instruction that names each.
    constexpr std::uintptr_t kGate = 0x12208EA4;   // B6C3F cmp / B6C6F mov 1 / B6CD2 mov 0
    constexpr std::uintptr_t kBusy = 0x12208EA0;   // B6C5E mov 1
    constexpr std::uintptr_t kJobA = 0x12208E94;   // B6C97 read, B6CAD WAITED ON, B6CC1 written
    constexpr std::uintptr_t kJobB = 0x12208E98;   // B6C7F read, B6C91 waited on
    constexpr std::uintptr_t kJobC = 0x12208E9C;   // B6C8B written
    constexpr std::uintptr_t kCount = 0x12208E88;  // B6CBB inc
    if (!IsReadable(base + kJobA, 4) || !IsReadable(base + kGate, 4)) {
        std::snprintf(out, static_cast<std::size_t>(outSize), "engine fence UNREADABLE");
        return;
    }
    const auto rd = [base](std::uintptr_t off) {
        return *reinterpret_cast<const volatile std::uint32_t*>(base + off);
    };
    const std::uint32_t jobA = rd(kJobA);
    // Whether the job the engine is ABOUT to wait on is already complete is the
    // single most useful bit here: if it is outstanding when pass 2 runs, pass 2
    // is what will be found parked on it.
    const int completeA = JobPoolWatchJobComplete(jobA);
    std::snprintf(out, static_cast<std::size_t>(outSize),
                  "gate=%u busy=%u jobA(waited)=0x%08X[%s] jobB=0x%08X jobC=0x%08X fences=%u",
                  rd(kGate), rd(kBusy), jobA,
                  completeA > 0 ? "done" : (completeA == 0 ? "OUTSTANDING" : "?"),
                  rd(kJobB), rd(kJobC), rd(kCount));
}
