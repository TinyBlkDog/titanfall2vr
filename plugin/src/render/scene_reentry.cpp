#include "scene_reentry.h"

#include "camera_update_hook.h"
#include "diagnostics.h"
#include "stall_watchdog.h"
#include "jobpool_watch.h"
#include "jt_probe.h"
#include "present_hook.h"
#include "stereo_targets.h"
#include "vb_bind_census.h"
#include "temporal_lever.h"
#include "view_block_camera.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>

// SHARED with camera_update_hook, which tallies uploads per tag. Time windows
// cannot attribute asynchronous work to a pass -- the burst proved that, with
// every frame carrying ~35 uploads that landed in whichever of three windows
// happened to be open -- so the only honest attribution is "issued on the
// re-entering thread, inside the pass". This is that tag. Outside the anonymous
// namespace on purpose: camera_update_hook reads it.
thread_local int t_passTag = 0;

namespace {

// Verified against the only call site, client+0x35AAE3. See the header.
using SceneDrawFn = void*(*)(void* self, void* viewBlock, std::uint32_t a3, std::uint32_t a4);

constexpr std::uintptr_t kSceneDrawSlot = 0x9287F0;
constexpr std::uintptr_t kSceneDrawFunction = 0x3723B0;
// F0 measured both of these live. The gate is an exact compare against them,
// not a range or a heuristic, because F0 showed there is exactly one caller.
constexpr std::uintptr_t kExpectedThisRva = 0x2173CC0;
constexpr std::uintptr_t kExpectedReturnRva = 0x35AAE6;

SceneDrawFn g_original = nullptr;
std::uintptr_t g_clientBase = 0;
bool g_installed = false;
bool g_installRefused = false;
bool g_detoursRequested = false;

std::atomic_bool g_censusWanted{false};
std::atomic_bool g_reentryWanted{false};
std::atomic_bool g_censusArmed{false};
// SHOTS, not a shot. One press arms `stereo.reentry_frames` consecutive
// doubles and the hook takes them one at a time, so a burst cannot outlive its
// press and a repeat still needs another one.
//
// It was deliberately ONE while the question was "does the nested call return",
// because a single shot cannot accumulate corruption while that is being asked.
// It is a burst now for two reasons that arrived together: the guard needs
// repetition and had only two runs behind it, and the nested pass's upload
// count varied 5 to 30 across single armed frames. A single frame cannot tell
// a per-frame property of the GAME from a property of the mechanism; eight
// consecutive frames can, and they repeat the guard eight times while doing it.
std::atomic_int g_shotsRemaining{0};
std::atomic_int g_shotsPerPress{1};
std::atomic_bool g_continuous{false};
std::atomic_uint64_t g_continuousFrames{0};
std::atomic_uint64_t g_epilogueCalls{0};
std::atomic_uint64_t g_epilogueDeclines{0};
std::atomic_bool g_epilogueWanted{true};

// client+0xFB3780 holds a singleton pointer; the engine calls its vtable slot
// +0xB8 immediately after every scene draw. See the call site in SceneDrawHook.
constexpr std::uintptr_t kSceneEpilogueSingletonRva = 0xFB3780;
constexpr std::size_t kSceneEpilogueVtableSlot = 0xB8;

bool ReadablePtr(const void* p, std::size_t bytes) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD bad = PAGE_NOACCESS | PAGE_GUARD;
    if (mbi.Protect & bad) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    return reinterpret_cast<std::uintptr_t>(p) + bytes <= base + mbi.RegionSize;
}

// Runs the engine's own post-scene-draw epilogue for the NESTED pass. Returns
// true only when it actually ran; every refusal is counted, never silent.
bool SceneDrawEpiloguePerformed() {
    if (!g_epilogueWanted.load(std::memory_order_acquire)) return false;
    if (!g_clientBase) { g_epilogueDeclines.fetch_add(1, std::memory_order_relaxed); return false; }
    auto** slot = reinterpret_cast<void**>(g_clientBase + kSceneEpilogueSingletonRva);
    if (!ReadablePtr(slot, sizeof(void*))) { g_epilogueDeclines.fetch_add(1, std::memory_order_relaxed); return false; }
    void* singleton = *slot;
    if (!ReadablePtr(singleton, sizeof(void*))) { g_epilogueDeclines.fetch_add(1, std::memory_order_relaxed); return false; }
    auto* vtable = *reinterpret_cast<void***>(singleton);
    if (!ReadablePtr(vtable, kSceneEpilogueVtableSlot + sizeof(void*))) { g_epilogueDeclines.fetch_add(1, std::memory_order_relaxed); return false; }
    void* fn = vtable[kSceneEpilogueVtableSlot / sizeof(void*)];
    // The entry must be code inside client.dll, or it is a stale/garbage slot.
    if (!fn || reinterpret_cast<std::uintptr_t>(fn) < g_clientBase ||
        reinterpret_cast<std::uintptr_t>(fn) > g_clientBase + 0x2000000) {
        g_epilogueDeclines.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    reinterpret_cast<void(*)(void*)>(fn)(singleton);
    return true;
}
// Which rung of the survival ladder is armed. Scene-draw thread only.
int g_ladderStage = -1;
// The capture ladder's state. A press does not arm shots directly any more; it
// starts this, and the shots arm only once both CONTROL frames are on disk.
int g_captureStage = 0;
int g_captureFrameCounter = 0;
// The post-burst flicker probe's state; see StepPostBurstCapture.
int g_postStage = 0;
int g_postFrames = 0;
// A fault in the nested call disarms the mechanism for the session. Reset is
// explicit -- never automatic. DESIGN section 8.3.
std::atomic_bool g_poisoned{false};

std::atomic_uint64_t g_entries{0};
std::atomic_uint64_t g_pass1Returns{0};
// COUNTED SEPARATELY, and this is the point of the rung: "0 attempts with 0
// returns" must read as "it never ran", never as "it ran and did not come
// back".
std::atomic_uint64_t g_doubleAttempts{0};
std::atomic_uint64_t g_doubleReturns{0};
std::atomic_uint64_t g_doubleFaults{0};
// Every early return the gate takes, named. A silent decline is indistinguish-
// able from a hook that stopped running.
std::atomic_uint64_t g_declineNotWanted{0};
std::atomic_uint64_t g_declineNotArmed{0};
std::atomic_uint64_t g_declinePoisoned{0};
std::atomic_uint64_t g_declineWrongThis{0};
std::atomic_uint64_t g_declineWrongCaller{0};

// The census, unchanged in substance from F0's.
constexpr int kMaxCallers = 8;
struct CallerRecord {
    std::uintptr_t returnAddress;
    std::uintptr_t thisPointer;
    std::uint64_t count;
    std::uint32_t arg3;
    std::uint32_t arg4;
    int jtThreadIndex;
    bool logged;
};
CallerRecord g_callers[kMaxCallers]{};
std::atomic_int g_callerCount{0};
std::atomic_uint64_t g_censusUnclassified{0};

// ---------------------------------------------------------------------------
// THE JOB-GROUP CENSUS. STAGE-O 2.2's hypothesis, measured at last in the only
// place it can be: the calls themselves.
//
// I have been sampling a GLOBAL (client+0xEA9EE4) and the refcount word it
// points at, before and after each pass, and it has read "1 -> 1" every time --
// on the control as well as the nested pass, which disqualifies it. That was
// always the wrong instrument: JT_EndJobGroup is a refcount RELEASE, so what
// matters is how many times it is CALLED and on WHICH id, not what a global
// happens to hold at two arbitrary instants.
//
// client.dll imports both functions, so this is an IAT pointer swap -- the
// project's preferred interception class, and deliberately not registered with
// the hook registry because a faulting PC never lands inside a pointer.
//
// Calls are attributed to a pass by a tag the scene-draw hook sets, so the log
// can say: the engine's own pass makes N Ends on id X, and the nested pass
// makes M more on the SAME id. That is the double-release, stated as a
// measurement instead of an inference -- or refuted.
using BeginJobGroupFn = std::uint32_t(*)(std::uint32_t);
using EndJobGroupFn = void(*)(std::uint32_t);
constexpr std::uintptr_t kIatBeginJobGroup = 0x893830;
constexpr std::uintptr_t kIatEndJobGroup = 0x893838;

BeginJobGroupFn g_origBegin = nullptr;
EndJobGroupFn g_origEnd = nullptr;
bool g_jtCensusInstalled = false;

// 0 = outside any instrumented pass, 1 = the engine's own pass, 2 = nested.
// THREAD-LOCAL, because a process-wide tag attributes every other thread's
// Begin/End to whichever pass happens to be open. The double-release landed on
// the exact id the engine's own pass had just ended, which is strong on its
// own -- but "strong on its own" is not the same as attributed, and the fix is
// two words.
std::atomic_uint64_t g_beginCalls[3]{};
std::atomic_uint64_t g_endCalls[3]{};

// The armed frame's actual sequence, so the ids can be compared rather than
// only the counts. Bounded by construction.
constexpr int kMaxJtEvents = 48;
struct JtEvent { int tag; bool isBegin; std::uint32_t id; std::uint32_t result; };
JtEvent g_jtEvents[kMaxJtEvents]{};
std::atomic_int g_jtEventCount{0};
std::atomic_bool g_jtRecording{false};

// THE DOUBLE-RELEASE DETECTOR, and it fires at the moment it happens rather
// than being inferred afterwards from a sequence dump.
//
// Stage O 2.2 predicted that a second pass would release a job group its own
// pass never acquired. Every attempt to see that so far has been indirect --
// sampling a global refcount at two instants, which read "1 -> 1" on the
// control as well as the nested pass. This watches the actual calls: a small
// ring of ids that have been Ended, and an End arriving for an id already in it
// with no intervening Begin is the double-release itself.
//
// The ring is deliberately tiny. Job ids carry a generation in their high bits,
// so a REUSED slot arrives with a different id and does not false-positive; a
// genuine second release arrives with the identical id.
constexpr int kEndedRing = 16;
std::uint32_t g_endedIds[kEndedRing]{};
std::atomic_int g_endedCursor{0};
std::atomic_uint64_t g_doubleReleases{0};
std::atomic_uint32_t g_lastDoubleReleaseId{0};
std::atomic_int g_lastDoubleReleaseTag{-1};
// Set for the whole duration of the armed frame -- from the moment the gate
// takes a shot until the pass-2 call returns -- so the RENDER thread can ask
// "is this frame doubled?" while it is executing the queued batches. The
// thread-local tag cannot serve here: the D3D work is on another thread.
std::atomic_bool g_frameIsDoubled{false};
std::atomic_bool g_reentryGuard{false};
std::atomic_uint64_t g_suppressedReleases{0};

// R2 -- THE ONCE-PER-FRAME LATCH, RESTORED. `stereo.restore_latch`, default ON.
//
// O2 found it offline and R1 CONFIRMED it live on every doubled frame:
// `this+0xF1C80` is produced once per frame outside the scene draw (writers at
// client+0x36AFA0 and +0x374EFB), tested at client+0x372781, consumed as arg4
// to client+0x3756A0 at +0x3728B6, and CLEARED TO 0 at +0x3728D7. R1's three
// samples per frame read it non-zero before pass 1 and zero before pass 2, 130
// frames out of 130.
//
// So pass 2 has always run with the latch already spent, which is verbatim the
// obligation Halo MCC wrote down: "the scene render consumes a once-per-frame
// latch and clears it, so a second pass in the same frame must restore it or
// silently skip work."
//
// This snapshots the value BEFORE pass 1 and writes THOSE BYTES back before
// pass 2 -- never a value we computed. It is the smallest item on the plan's
// R2 preference order, and it is one behavioural change.
//
// IT IS NOT CLAIMED AS THE CAUSE OF THE HANG. R1 refuted the pool outright and
// nothing measured accumulates, so this is an unfulfilled obligation being
// discharged, not a diagnosis being acted on. The falsifier is the wall: 129
// again means the latch is not it and R2b (replicating client+0x36A060 on the
// three view blocks) is next.
// R4 -- GIVE PASS 2 ITS OWN JOB GROUP. `stereo.own_job_group`, DEFAULT ON.
//
// THE RUNG THE GUARD'S OWN COMMENT NAMED, and its precondition is now met.
// R3's watchdog caught the stalled thread three times, same RIP, same RSP:
//
//     THE STALLED THREAD IS AT ntdll.dll+0x163FD4
//       client.dll+0x375753   <- the verified return address of `call 3748A0`
//                                at client+0x37574E, whose sole caller is
//                                client+0x3756A0 -- the function the latch gates
//       client.dll+0x374917   <- fn 3748A0, which at +0x21 does
//                                JT_WaitForJobAndOnlyHelpWithJobTypes(ecx, 0, -1)
//       tier0.dll+0x9680      <- fn 95E0 = that export
//       tier0.dll+0x75BE      <- fn 7310, the help-or-sleep parking helper
//       KERNELBASE / ntdll    <- parked forever
//
// **It is a job-wait deadlock, not a resource exhaustion**, which is exactly
// why R1's pool watch stayed flat at 121-124 and why nothing accumulates.
//
// The cause is structural. `client+0xEA9EE4` holds ONE view job-group id,
// written once per frame by the Begin stage `client+0x35AEF0` (two
// JT_BeginJobGroups) -- a stage our re-entry does not re-run -- and read by the
// scene draw's three JT_EndJobGroup sites. So pass 2 ends a group it never
// began. With the guard ON that repeat End is suppressed, the group's refcount
// never reaches zero, the group never completes, and a later wait on it never
// returns. With the guard OFF it is a real double release and dies at 16.
// Both arms of the guard are wrong because the group is shared.
//
// This gives pass 2 its own: Begin a group, publish it in `client+0xEA9EE4`
// around the nested call, restore the old id, and End ours once. Pass 2's End
// sites then target OUR group instead of pass 1's.
//
// THE GUARD STAYS ON AND SHOULD GO INERT, which is this rung's positive
// control: `suppressedReleases` must read **0**, because there is no longer an
// id pass 1 already ended for pass 2 to re-release. A non-zero count means the
// group swap did not take and the run is not a test. The existing
// `*** DOUBLE RELEASE ***` detector is the falsifier on the other side.
constexpr std::uintptr_t kViewJobGroupGlobal = 0xEA9EE4;
std::atomic_bool g_ownJobGroup{true};
std::atomic_uint64_t g_ownGroupsBegun{0};
std::atomic_uint64_t g_ownGroupsDeclined{0};
std::atomic_uint64_t g_ownGroupSwapFailed{0};
// R4b -- WHO CLOSES OUR GROUP. R4 assumed pass 2 would not, and the run proved
// otherwise: `jt[09] pass2 End id=0x004872FD` is the exact id R4 had just begun.
// So R4 released it a SECOND time every doubled frame -- a double release, the
// same defect the guard exists to prevent, just on our own group instead of the
// engine's. `batchesFree` rose to 127/127 (a FULL free ring, i.e. slots being
// returned faster than taken) and it died at 23 frames, right at the 16-deep
// per-thread ring bound O2 identified as the over-release failure mode.
//
// The accounting is now MEASURED rather than assumed: HookEndJobGroup marks our
// id when it goes past, and we close the group afterwards only if nobody else
// did. Both outcomes are counted, so the log says which happened instead of
// leaving it to be inferred.
std::atomic_uint32_t g_ownGroupActive{0};
std::atomic_bool g_ownGroupEnded{false};
std::atomic_uint64_t g_ownGroupClosedByPass2{0};
std::atomic_uint64_t g_ownGroupClosedByUs{0};

// R8. `stereo.prewait_job`, DEFAULT ON. See the long note at the call site.
// 12 ms is generous against a 25 ms frame at the doubled rate and still far
// under the 2000 ms the watchdog calls a stall, so a timeout is a real timeout
// and not an artefact of the budget.
// R9. `stereo.prewait_ms`, DEFAULT 250. R8b turned the mystery into a number:
// `ok=126 (alreadyDone=90 spun=36) timedOut=1` — the drain covered 126 of 127
// doubled frames, and **the single timeout IS the frame that hung**. So on that
// one frame the job does not finish inside 12 ms; we gave up, the engine waited,
// and it parked. R7 already showed the generation moves on later, so the job
// does finish — just after the waiter has stopped listening.
//
// Both facts point the same way: widen our own window past the outlier and the
// engine never gets the chance to park. The cost is a hitch on the rare slow
// frame, paid by us instead of a deadlock paid by the process. If the wall moves
// with `timedOut` at 0, the mechanism is fully characterised; if `timedOut`
// stays 1 at 250 ms, that job is not merely slow and this is refuted.
std::atomic<unsigned long long> g_preWaitMaxMs{250};
std::atomic_bool g_preWaitJob{true};
std::atomic_uint64_t g_preWaitZeroed{0};
std::atomic_uint64_t g_preWaitAlreadyDone{0};
std::atomic_uint64_t g_preWaitDrained{0};
std::atomic_uint64_t g_preWaitTimedOut{0};
std::atomic_uint64_t g_preWaitNothing{0};
std::atomic_uint64_t g_preWaitUnknown{0};

constexpr std::uintptr_t kViewRenderLatch = 0xF1C80;
// DEFAULT OFF AS OF THE R2 RUN, WHICH MEASURED IT HARMFUL. The restore worked
// perfectly -- 101 doubled frames, every one restored, readback verified, zero
// declines -- and the failure changed from a HANG at 129/130 to an ACCESS
// VIOLATION at 101, in two array copies (`rep movsb` at client+0x7FD139, and
// client+0x641594 reading a NULL array base out of `[obj+0xC8]` while the count
// at `[obj+0xC4]` said there were 0x70-byte records to copy).
//
// So the latch IS in the causal path and pass 2 HAS been silently skipping
// work, exactly as Halo MCC warned -- but the work it skips appends to arrays
// that are sized and reset once per frame. Letting pass 2 do that work without
// also reproducing the caller's reset is strictly worse. Kept as a config-only
// lever; do not arm it without the reset half.
std::atomic_bool g_restoreLatch{false};
// R2b -- THE VIEW-BLOCK RECOMPUTE. `stereo.recompute_blocks`, DEFAULT ON.
// The plan's item 1, and the one obligation O1 enumerated that has never been
// tried. The caller does this on ALL THREE view blocks immediately before every
// scene draw and our re-entry never has:
//
//     0035AAA5  lea rcx, [rdi+0x12ECC0]  ; the arg2 block
//     0035AAAC  call 0x36A060
//     0035AAB1  lea rcx, [rdi+0xA13C0]   ; the origin block
//     0035AAB8  call 0x36A060
//     0035AABD  lea rcx, [rdi+0xB55C0]   ; the 3D sky camera
//     0035AAC4  call 0x36A060
//
// and `0x36A060` (70 bytes) rebuilds the projection from the block's own fov
// fields and then multiplies proj (+0x80) by view (+0x40) into viewproj (+0xC0)
// -- which matches the settled view-block layout exactly.
//
// It is PURE COMPUTATION on blocks we already own: no allocation, no vtable
// call into a slot we have not read, nothing appended to any array. That is why
// it is the rung after the latch turned out to be harmful -- it is the lowest
// risk item left, and it runs AFTER the camera write so the engine's own
// builder propagates our view matrix into proj and viewproj the way it does for
// pass 1.
constexpr std::uintptr_t kViewBlockRecompute = 0x36A060;
constexpr std::uintptr_t kBlockArg2 = 0x12ECC0;
constexpr std::uintptr_t kBlockOrigin = 0xA13C0;
constexpr std::uintptr_t kBlockSky = 0xB55C0;
using RecomputeFn = void(__fastcall*)(void*);
std::atomic_bool g_recomputeBlocks{true};
std::atomic_uint64_t g_recomputeDone{0};
std::atomic_uint64_t g_recomputeDeclined{0};

std::atomic_uint64_t g_latchRestored{0};
std::atomic_uint64_t g_latchAlreadySet{0};
std::atomic_uint64_t g_latchNothingToRestore{0};
std::atomic_uint64_t g_latchUnreadable{0};

struct BurstStat {
    int shots;
    int shotsClearingIdle;
    unsigned long long sumEngine, sumIdle, sumNested, minNested, maxNested;
    double sumMsEngine, sumMsNested;
};
BurstStat g_burst{};
// The symmetric pre-write, counted where it is CALLED. See the call site.
std::atomic_uint64_t g_symmetricSiteReached{0};
std::atomic_uint64_t g_symmetricArmedHere{0};
std::atomic_uint64_t g_symmetricPreWrites{0};

// Returns true when this End is a repeat of an id already ended with no Begin
// between -- i.e. the double release itself.
bool NoteEnded(std::uint32_t id, int tag) {
    for (int i = 0; i < kEndedRing; ++i) {
        if (g_endedIds[i] != id || id == 0) continue;
        g_doubleReleases.fetch_add(1, std::memory_order_relaxed);
        g_lastDoubleReleaseId.store(id, std::memory_order_relaxed);
        g_lastDoubleReleaseTag.store(tag, std::memory_order_relaxed);
        char line[300]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] *** DOUBLE RELEASE: JT_EndJobGroup(0x%08X) called again with no Begin "
            "between, from pass%d. This is STAGE-O 2.2, measured. ***\n", id, tag);
        Tf2VrLog(line);
        return true;
    }
    const int slot = g_endedCursor.fetch_add(1, std::memory_order_acq_rel) % kEndedRing;
    g_endedIds[slot] = id;
    return false;
}

void ForgetEnded(std::uint32_t id) {
    // A fresh Begin retires the id from the ring, so a legitimate reuse of the
    // same id cannot be reported as a release of the old one.
    for (int i = 0; i < kEndedRing; ++i) {
        if (g_endedIds[i] == id) g_endedIds[i] = 0;
    }
}

bool RecordJt(bool isBegin, std::uint32_t id, std::uint32_t result) {
    const int tag = t_passTag;
    const int safeTag = (tag < 0 || tag > 2) ? 0 : tag;
    (isBegin ? g_beginCalls : g_endCalls)[safeTag].fetch_add(1, std::memory_order_relaxed);
    bool duplicate = false;
    if (isBegin) ForgetEnded(result); else duplicate = NoteEnded(id, safeTag);
    if (g_jtRecording.load(std::memory_order_acquire)) {
        const int slot = g_jtEventCount.fetch_add(1, std::memory_order_acq_rel);
        if (slot < kMaxJtEvents) g_jtEvents[slot] = {tag, isBegin, id, result};
    }
    return duplicate && safeTag == 2;
}

std::uint32_t HookBeginJobGroup(std::uint32_t parent) {
    const std::uint32_t id = g_origBegin(parent);
    RecordJt(true, parent, id);
    return id;
}

// THE GUARD. `stereo.reentry_guard`, default off.
//
// STAGE-O 2.2 predicted the nested pass would release a job group its own pass
// never acquired; three runs measured it, on the exact id the engine's own pass
// had ended one line earlier; and the run that hung did so 180 ms later on a
// materialsystem thread, with the nested call itself having returned normally.
// So the release is the defect and the hang is its consequence -- which is very
// likely what R4 and R5 recorded as "the deadlock", attributed to scheduler
// saturation because nobody was watching the job groups.
//
// The intervention is the smallest one that addresses exactly what was
// measured: a SECOND release, from the nested pass, of an id already ended --
// and only that -- does not reach tier0. The refcount is already correct
// without it, because pass 1's release was the real one.
//
// It is deliberately NOT the other candidate fix -- giving pass 2 its own group
// by writing client+0xEA9EE4 around the call. That is memory surgery on an
// engine global; this is a call that does not happen. If the guard proves the
// mechanism and pass 2 then needs a group of its own for its own jobs, that is
// the next rung and it will have this run's evidence behind it.
void HookEndJobGroup(std::uint32_t id) {
    // R4b: note when pass 2 releases the group we published, so the close after
    // the nested call does not release it a second time.
    if (id != 0 && id == g_ownGroupActive.load(std::memory_order_acquire)) {
        g_ownGroupEnded.store(true, std::memory_order_release);
    }
    const bool suppressible = RecordJt(false, id, 0);
    if (suppressible && g_reentryGuard.load(std::memory_order_acquire)) {
        g_suppressedReleases.fetch_add(1, std::memory_order_relaxed);
        char line[260]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] GUARD: suppressed the nested pass's second JT_EndJobGroup(0x%08X). "
            "Pass 1's release stands; this call does not reach tier0.\n", id);
        Tf2VrLog(line);
        return;
    }
    g_origEnd(id);
}

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

void DescribeAddress(const void* address, char* out, std::size_t outSize) {
    std::snprintf(out, outSize, "%p (unknown)", address);
    MEMORY_BASIC_INFORMATION mbi{};
    if (!address || !VirtualQuery(address, &mbi, sizeof(mbi)) || !mbi.AllocationBase) return;
    char full[MAX_PATH]{};
    if (!GetModuleFileNameA(static_cast<HMODULE>(mbi.AllocationBase), full, MAX_PATH)) return;
    const char* leaf = std::strrchr(full, '\\');
    const auto rva = reinterpret_cast<std::uintptr_t>(address) -
        reinterpret_cast<std::uintptr_t>(mbi.AllocationBase);
    std::snprintf(out, outSize, "%s+0x%llX", leaf ? leaf + 1 : full,
        static_cast<unsigned long long>(rva));
}

void RecordCaller(void* self, const void* ret, std::uint32_t a3, std::uint32_t a4) {
    const auto retValue = reinterpret_cast<std::uintptr_t>(ret);
    const int known = g_callerCount.load(std::memory_order_acquire);
    for (int i = 0; i < known; ++i) {
        if (g_callers[i].returnAddress == retValue) { ++g_callers[i].count; return; }
    }
    if (known >= kMaxCallers) {
        g_censusUnclassified.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_callers[known].returnAddress = retValue;
    g_callers[known].thisPointer = reinterpret_cast<std::uintptr_t>(self);
    g_callers[known].count = 1;
    g_callers[known].arg3 = a3;
    g_callers[known].arg4 = a4;
    g_callers[known].jtThreadIndex = JtCurrentThreadIndex();
    g_callers[known].logged = false;
    g_callerCount.store(known + 1, std::memory_order_release);
}

// SEH LIVES HERE, ON ITS OWN, and only around the SECOND call. The first call
// stays unguarded so a real crash still produces a real dump -- swallowing the
// engine's own faults to protect an experiment is how a project loses the one
// record that would have named the bug. No C++ object with a destructor may be
// in scope in a function with __try, hence the separate function.
bool CallNestedGuarded(SceneDrawFn fn, void* self, void* viewBlock,
                       std::uint32_t a3, std::uint32_t a4, void** resultOut, DWORD* codeOut) {
    __try {
        *resultOut = fn(self, viewBlock, a3, a4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *codeOut = GetExceptionCode();
        return false;
    }
}

// The gate. Every path out is counted and named.
bool ShouldDouble(void* self, const void* ret) {
    if (!g_reentryWanted.load(std::memory_order_acquire)) {
        g_declineNotWanted.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (g_poisoned.load(std::memory_order_acquire)) {
        g_declinePoisoned.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (reinterpret_cast<std::uintptr_t>(self) != g_clientBase + kExpectedThisRva) {
        g_declineWrongThis.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (reinterpret_cast<std::uintptr_t>(ret) != g_clientBase + kExpectedReturnRva) {
        g_declineWrongCaller.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // CONTINUOUS MODE: no shot is spent and the double never stops.
    //
    // Every rung of this route has run as an 8-frame BURST -- a tenth of a
    // second -- because the mechanism was under test and 64 consecutive doubles
    // once crashed. That made sense while the question was "does this work".
    // The question now is whether it is WEARABLE, and nobody can judge stereo
    // from a flash. So this is a latched mode: on until it is switched off.
    //
    // The accumulation crash is NOT fixed and is not pretended to be. What is
    // different is that the run now measures it: g_continuousFrames counts how
    // far it got, the count is logged as it climbs, and the crash recorder
    // already captures the fault. A mode that dies at frame 900 tells us far
    // more about the accumulation than another burst of 8 ever will.
    if (g_continuous.load(std::memory_order_acquire)) {
        // THE BREATH WAS TRIED AND IT MADE THINGS WORSE. Leaving one frame
        // single every 96 doubles crashed within 85 ms of the first breath, at
        // client.dll+0x29160A -- `call [rax+0xD0]`, a virtual call through a
        // bad vtable pointer, and the same address an earlier headset run
        // faulted on. So the doubled->single->doubled TRANSITION is itself
        // hazardous, and "the resource recovers when doubling stops" is
        // REFUTED rather than merely unproven. Removed, not disabled.
        // THE SURVIVAL LADDER: THE WHOLE BISECTION IN ONE RUN.
        //
        // Four runs have each answered ONE question about this hang, because
        // each changed one thing and the wearer paid a full headset session for
        // one bit. That is the wrong trade when the headset is the scarce
        // resource. The rule this route has been following -- one behavioural
        // change per run -- exists for changes whose effects CONFOUND each
        // other. These do not: they are strictly nested, each stage is the
        // previous one plus one addition, and the frame count says exactly
        // where it stopped.
        //
        // So the run steps ITSELF. Every kStageFrames doubled frames survived,
        // the next piece of machinery is armed and announced. Whatever it dies
        // in is the piece that kills it, and one session yields the answer four
        // sessions were going to.
        //
        // kStageFrames is 100, deliberately under the 129-frame wall: if plain
        // doubling is what dies, it dies in stage 0 with nothing of ours armed,
        // and that is the single most valuable outcome available.
        // STAGE 0 MUST OUTLAST THE WALL, or the ladder cannot separate "this
        // stage killed it" from "129 kills it whatever is armed".
        //
        // The first ladder run put 100 frames in stage 0 and it SURVIVED all
        // 100 -- plain doubling, none of our machinery, clean. Then stage 1
        // armed the write substitution and it froze at 128. But 128 is also the
        // wall every previous run hit, so stage and wall coincided and the run
        // could not tell them apart.
        //
        // Stage 0 is now 2000 frames, FIFTEEN times the wall. The first ladder
        // run used 100 -- chosen while the 129 wall was already known -- which
        // guaranteed the one outcome that answers nothing: stage 0 survives
        // its 100, stage 1 arms, and the freeze lands at 128 where stage and
        // wall coincide. A threshold must be set so that EVERY outcome is
        // readable, not just the interesting one. If it dies at ~129
        // with NOTHING of ours armed, the accumulation is intrinsic to the
        // re-entry and every piece of stereo machinery is innocent. If it
        // reaches 400 and then dies shortly after the substitution arms, the
        // substitution is the accumulator. The two outcomes are now
        // unmistakable instead of confounded.
        constexpr unsigned long long kStageEnds[] = {2000, 2400, 2800};
        int stage = 0;
        {
            const unsigned long long f = g_continuousFrames.load(std::memory_order_relaxed);
            for (unsigned long long end : kStageEnds) { if (f >= end) ++stage; }
        }
        if (stage != g_ladderStage) {
            g_ladderStage = stage;
            const char* what = "";
            switch (stage) {
                case 1: SetRtvSubstituteWanted(1);
                        what = "WRITE SUBSTITUTION armed (batch 2 gets its own colour target)";
                        break;
                case 2: SetRtvReadSubstituteWanted(1);
                        what = "READ SUBSTITUTION armed (batch 2's post reads its own world)";
                        break;
                case 3: SetEngineIpd(-1.0f);
                        what = "ENGINE CAMERA WRITE armed (batch 2 gets its own camera)";
                        break;
                default:
                        what = "nothing further to arm; from here it is endurance only";
                        break;
            }
            char line[420]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] SURVIVAL LADDER -> STAGE %d at %llu doubled frames: %s. If the run ends "
                "here, THIS is the addition that ended it.\n",
                stage, g_continuousFrames.load(std::memory_order_relaxed), what);
            Tf2VrLog(line);
            LogStallWatchdogStatus();
        }
        const unsigned long long n =
            g_continuousFrames.fetch_add(1, std::memory_order_relaxed) + 1;
        // THE HEARTBEAT IS THE POINT. If this mode dies, the last number in the
        // log is how many consecutive doubles the engine survived -- which is
        // the first real measurement of the accumulation crash. 64 was known to
        // crash and 8 to be safe; nothing between has ever been tried, and a
        // burst can never tell us where the wall is.
        //
        // Logged on a rising scale rather than every frame: dense early where
        // the known-bad boundary is, then sparse, so a long healthy session does
        // not itself become the thing that fills the disk.
        if (n <= 64 ? (n % 8) == 0 : (n <= 1024 ? (n % 128) == 0 : (n % 1024) == 0)) {
            char line[300]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] CONTINUOUS STEREO: %llu consecutive doubled frames and still running. "
                "R8b DRAIN ok=%llu (alreadyDone=%llu spun=%llu) timedOut=%llu "
                "nothingToWait=%llu unknown=%llu (arm=%s) | "
                "R4b OWN GROUP begun=%llu declined=%llu swapFailed=%llu closedByPass2=%llu "
                "closedByUs=%llu (R4 always closed it itself and that DOUBLE RELEASE killed the "
                "run at 23 frames; closedByUs should now be near 0) | POSITIVE CONTROL: guard "
                "suppressed=%llu (MUST BE 0 -- non-zero means the swap did not take and this run "
                "is NOT a test) | doubleReleases=%llu. "
                "EPILOGUE performed %llu declined %llu. R2b RECOMPUTE done=%llu declined=%llu "
                "(arm=%s). R2 LATCH restored=%llu alreadySet=%llu "
                "nothingToRestore=%llu unreadable=%llu (arm=%s). BASELINES: hang at 129/130 with "
                "neither armed; ACCESS VIOLATION at 101 with the latch armed. Past 130 is the "
                "first real move.\n",
                n,
                g_preWaitZeroed.load(std::memory_order_relaxed),
                g_preWaitAlreadyDone.load(std::memory_order_relaxed),
                g_preWaitDrained.load(std::memory_order_relaxed),
                g_preWaitTimedOut.load(std::memory_order_relaxed),
                g_preWaitNothing.load(std::memory_order_relaxed),
                g_preWaitUnknown.load(std::memory_order_relaxed),
                g_preWaitJob.load(std::memory_order_acquire) ? "ON" : "OFF (control)",
                g_ownGroupsBegun.load(std::memory_order_relaxed),
                g_ownGroupsDeclined.load(std::memory_order_relaxed),
                g_ownGroupSwapFailed.load(std::memory_order_relaxed),
                g_ownGroupClosedByPass2.load(std::memory_order_relaxed),
                g_ownGroupClosedByUs.load(std::memory_order_relaxed),
                g_suppressedReleases.load(std::memory_order_relaxed),
                g_doubleReleases.load(std::memory_order_relaxed),
                g_epilogueCalls.load(std::memory_order_relaxed),
                g_epilogueDeclines.load(std::memory_order_relaxed),
                g_recomputeDone.load(std::memory_order_relaxed),
                g_recomputeDeclined.load(std::memory_order_relaxed),
                g_recomputeBlocks.load(std::memory_order_acquire) ? "ON" : "OFF (control)",
                g_latchRestored.load(std::memory_order_relaxed),
                g_latchAlreadySet.load(std::memory_order_relaxed),
                g_latchNothingToRestore.load(std::memory_order_relaxed),
                g_latchUnreadable.load(std::memory_order_relaxed),
                g_restoreLatch.load(std::memory_order_acquire) ? "ON" : "OFF (control)");
            Tf2VrLog(line);
            LogStallWatchdogStatus();
        }
        return true;
    }
    // LAST, so that a shot is spent only on a frame that passed every other
    // test. A CAS loop rather than a plain decrement, so the counter can never
    // go negative and two threads cannot take the same shot.
    int remaining = g_shotsRemaining.load(std::memory_order_acquire);
    while (remaining > 0 &&
           !g_shotsRemaining.compare_exchange_weak(remaining, remaining - 1,
                                                   std::memory_order_acq_rel)) {
    }
    if (remaining <= 0) {
        g_declineNotArmed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

// One pass's observable footprint. PASS 1 IS PASS 2's POSITIVE CONTROL, and
// that is the whole design of this rung's second attempt.
//
// F1's first run logged "uploads 0 -> 0" across the nested call and the line
// asserted that meant it returned without rendering. It did not: the camera
// detours were never installed in a flat autoarm=0 run, so the counter was dead
// BEFORE the call as well as after. A null from an instrument that was never
// watching is instrument failure, not a refutation -- and the same objection
// applied to the refcount reading, because nobody had ever measured what a
// NORMAL pass does to that refcount either.
//
// So both passes are now measured identically, on the armed frame only. Every
// number below is a delta against the engine's own pass, and any instrument
// that reads zero on pass 1 has disqualified itself from being read on pass 2.
struct PassFootprint {
    unsigned long long uploads;
    // Thread-attributed: uploads ISSUED inside this pass on this thread. The
    // only witness the burst did not destroy.
    unsigned long long uploadsByPass1;
    unsigned long long uploadsByPass2;
    // QUEUED WORK, which is the witness a submission seam actually admits. If
    // the scene draw only queues, "did it render" is the wrong question at this
    // boundary and "did it queue its own job groups" is the right one.
    unsigned long long jtBeginByPass2;
    unsigned long long jtEndByPass2;
    // TWO MORE WITNESSES, because the upload counter came back mute twice and a
    // mute instrument is not an answer.
    //
    // The run before this one installed the camera detours and STILL read zero
    // uploads on the engine's own pass. The independent upload census in
    // camera_update_hook says why in its own words -- "no camera-sized uploads
    // seen, so the hook is not on the upload path this session" -- so that
    // counter cannot answer F1's question in a flat run no matter how it is
    // armed. Adding a second reading of the same dead counter would have been
    // the third run spent on it.
    //
    // So: the upload LADDER, whose whole purpose is to name which stage dies
    // ("read them in order, the first zero is the answer"), and the VIEWPORT
    // total, which is a render witness that does not depend on a camera upload
    // being recognised at all -- the world pass sets a viewport whether or not
    // we can identify its constants.
    unsigned long long viewportTotal;
    unsigned long long ladderInvocations;
    unsigned long long ladderOnOurContext;
    unsigned long long ladderCameraSized;
    bool ladderInstalled;
    std::uint32_t jobId;
    std::int32_t refCount;
    bool jobRead;
    void* result;
    double ms;
};

void SnapshotBefore(PassFootprint* f) {
    f->uploads = HookCameraSizedUploads();
    f->uploadsByPass1 = CameraUploadsForPass(1);
    f->uploadsByPass2 = CameraUploadsForPass(2);
    f->jtBeginByPass2 = g_beginCalls[2].load(std::memory_order_relaxed);
    f->jtEndByPass2 = g_endCalls[2].load(std::memory_order_relaxed);
    f->jobRead = ReadViewJobGroup(&f->jobId, &f->refCount);
    unsigned long long dominant = 0, total = 0;
    float w = 0.0f, h = 0.0f;
    DominantGameViewport(&w, &h, &dominant, &total);
    f->viewportTotal = total;
    UploadLadderCounters ladder{};
    ReadUploadLadderCounters(&ladder);
    f->ladderInstalled = ladder.installed;
    f->ladderInvocations = ladder.invocations;
    f->ladderOnOurContext = ladder.onOurContext;
    f->ladderCameraSized = ladder.cameraSized;
}

void LogPass(const char* label, const PassFootprint& before, const PassFootprint& after) {
    char line[760]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F1 %s: %.3f ms | viewportSets +%llu | uploads +%llu | "
        "ladder installed=%d invocations +%llu onOurContext +%llu cameraSized +%llu | "
        "jobGroup %s id 0x%08X -> 0x%08X refcount %d -> %d | returned %p\n",
        label, after.ms,
        after.viewportTotal - before.viewportTotal,
        after.uploads - before.uploads,
        after.ladderInstalled ? 1 : 0,
        after.ladderInvocations - before.ladderInvocations,
        after.ladderOnOurContext - before.ladderOnOurContext,
        after.ladderCameraSized - before.ladderCameraSized,
        (before.jobRead && after.jobRead) ? "read" : "UNREADABLE",
        before.jobId, after.jobId, before.refCount, after.refCount, after.result);
    Tf2VrLog(line);
}

void* SceneDrawHook(void* self, void* viewBlock, std::uint32_t a3, std::uint32_t a4) {
    const void* ret = _ReturnAddress();
    // N2's RESTORE, and it is deferred here on purpose rather than placed
    // immediately after the nested call. F1's own thread-attribution line says
    // the scene draw issues no D3D on its own thread -- it QUEUES -- so a
    // restore that runs the instant the nested call returns can revert the
    // block before whatever consumes it has read it, and that failure is inert,
    // silent and indistinguishable from "the mechanism does not work". It puts
    // the block back only if the block still holds exactly what the write left
    // there; the engine rebuilds it from its source at client+0x35A9BB, which
    // runs BEFORE this hook, so the expected outcome is a counted decline.
    RestoreBatch2CameraIfUntouched();
    g_entries.fetch_add(1, std::memory_order_relaxed);
    if (g_censusArmed.load(std::memory_order_acquire)) RecordCaller(self, ret, a3, a4);

    // The gate is consulted BEFORE pass 1 now, because pass 1 has to be
    // instrumented on the armed frame to serve as the control.
    const bool doubling = ShouldDouble(self, ret);

    if (!doubling) {
        // TAGGED EVEN WHEN NOT DOUBLING, so the per-frame baseline says how many
        // Begins and Ends a NORMAL scene draw makes. Without that number the
        // armed frame has nothing to be compared against, which is the mistake
        // this rung has already made twice.
        //
        // R1'S CONTROL, AND IT IS FREE. This is an UNDOUBLED frame sampled by
        // the same instrument at the same point in the frame. If the job-group
        // batch store drains here too, it is not our resource and saying so is
        // the entire point of the control. F6 latches, so one run walks
        // undoubled then doubled with no second run and no second arm.
        JobPoolWatchSample("undoubled, pre-draw", self, false, true);
        t_passTag = 1;
        void* plain = g_original(self, viewBlock, a3, a4);
        t_passTag = 0;
        JobPoolWatchSample("undoubled, post-draw", self, false, false);
        g_pass1Returns.fetch_add(1, std::memory_order_relaxed);
        return plain;
    }

    // Tell the render thread this frame is doubled BEFORE pass 1 is issued, so
    // it is already true when the queued batches reach the viewport hook.
    g_frameIsDoubled.store(true, std::memory_order_release);
    // First shot of a press asks for the eye pair. One frame, two files.
    // M2 REPLACES THE F1 EYE PAIR HERE, and the replacement is a retirement.
    // EYE0/EYE1 was never a stereo pair -- M1 measured EYE0 uniformly ~35
    // brighter than EYE1 in every cell, because it is the gamma-encoded HDR
    // scene target against the tonemapped backbuffer: two stages of one frame,
    // not two eyes. It also cost 107 ms on one frame. What M2 needs instead is
    // the private target beside the backbuffer, from the SAME Present.
    //
    // T-A0 TAKES A SECOND PAIR, AT THE LAST SHOT OF THE SAME BURST. The old
    // comment below this block said eight identical bitmaps answer nothing the
    // first one did not -- which assumed the answer, because whether they ARE
    // identical is the question. A temporal history that batch 2 feeds back
    // into gets WORSE every frame, so shot 8 diverges from shot 1; a
    // double-write inside one frame corrupts each frame the same way, so the
    // two match. One burst, one extra file, and it separates T1/T2 from T3.
    //
    // g_shotsRemaining is decremented inside ShouldDouble, which has already
    // run on this frame, so zero here IS the last shot -- the same test the
    // burst summary and the post-burst probe below both already use.
    if (g_burst.shots == 0) {
        RtvCensusRequestBurstCapture(1);
    } else if (g_shotsRemaining.load(std::memory_order_acquire) == 0) {
        RtvCensusRequestBurstCapture(g_burst.shots + 1);
    }
    // T-A: THE TEMPORAL ARM, READ AT THE MEASUREMENT rather than from the
    // keypress that set it. The lever is an engine convar driven through the
    // engine's own console; a set that was parsed and discarded reads back
    // wrong here and disqualifies the burst before anyone looks at a picture.
    if (g_burst.shots == 0) LogTemporalCvarCensus("at the first shot of a burst");
    // N1's SKYBOX CHECK, read-only, once per burst. The three blocks the caller
    // finalises before the scene draw, with their origins: if one of them sits
    // thousands of units away it is the 3D sky camera, it is a SEPARATE block,
    // and the eye write therefore never reaches it. That is the far-field
    // question answered from the blocks themselves rather than from uploads,
    // which is what the shipped guard needs g_referenceOrigin for -- and a flat
    // run may never populate that.
    if (g_burst.shots == 0) CensusViewBlocks(self);
    // Everything below happens once per session at most.
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    const double toMs = freq.QuadPart ? 1000.0 / static_cast<double>(freq.QuadPart) : 0.0;

    char head[320]{};
    std::snprintf(head, sizeof(head),
        "[TF2VR] F1: armed frame. this=%p view=%p arg3=%u arg4=%u. Pass 1 is the engine's own "
        "call and is this run's CONTROL; pass 2 is the nested one.\n",
        self, viewBlock, a3, a4);
    Tf2VrLog(head);

    // ---- PASS 1: the engine's own call. Unguarded, exactly as always. ----
    //
    // ---- R8: DRAIN THE WAIT OURSELVES, THEN LET THE ENGINE SKIP IT. --------
    //
    // R7 caught the deadlock red-handed. The stalled thread is parked in
    // JT_WaitForJobAndOnlyHelpWithJobTypes on id 0x0044B2B0 (slot 688), and
    // across the three watchdog dumps, ~2 s apart, that slot's generation read
    // 0x004572B0, 0x0045F2B0, 0x004632B0. **The slot was being recycled the
    // whole time: the job had long since completed.** The engine's own early-out
    // (tier0+0x9629 `cmp` / `jne 0x9685`, generations masked 0xFFFFC000) was
    // TRUE and the thread was never woken. That is a LOST WAKEUP, not a leak --
    // which is why nothing accumulates, why worker count is irrelevant (R6: 1
    // and 6 both die at 130), and why 35 genuine waits succeeded before one lost
    // the signal.
    //
    // We cannot patch tier0's wait. We do not need to: the engine already has a
    // sanctioned skip -- `test ecx,ecx / je` at client+0x3748B7 means a job id
    // of ZERO does not wait at all.
    //
    // So: poll the completion test OURSELVES -- the engine's own comparison,
    // byte for byte -- and once the job is provably done, write 0 into
    // this+0xF1C80 so the engine takes its own skip path. Polling cannot lose a
    // wakeup because it never parks.
    //
    // WHY THIS IS SEMANTICALLY A NO-OP, which is what makes it safe: when the
    // generations differ, tier0+0x95E0 returns immediately WITHOUT waiting. So
    // on every frame where we zero the field, the engine's wait would have done
    // nothing anyway. We remove the opportunity to park; we do not remove a
    // synchronisation. If the job is genuinely outstanding we spin until it is
    // not, which is exactly what the wait is for.
    //
    // BOUNDED, and the timeout falls back to today's behaviour untouched.
    if (g_preWaitJob.load(std::memory_order_acquire)) {
        auto* slot = reinterpret_cast<std::uint32_t*>(
            reinterpret_cast<std::uint8_t*>(self) + kViewRenderLatch);
        if (!IsReadable(slot, sizeof(std::uint32_t))) {
            g_preWaitUnknown.fetch_add(1, std::memory_order_relaxed);
        } else {
            const std::uint32_t id = *static_cast<volatile std::uint32_t*>(slot);
            const int first = JobPoolWatchJobComplete(id);
            if (id == 0) {
                g_preWaitNothing.fetch_add(1, std::memory_order_relaxed);
            } else if (first < 0) {
                g_preWaitUnknown.fetch_add(1, std::memory_order_relaxed);
            } else {
                int done = first;
                const std::uint64_t start = GetTickCount64();
                unsigned spins = 0;
                while (!done && GetTickCount64() - start < g_preWaitMaxMs.load(std::memory_order_acquire)) {
                    ++spins;
                    Sleep(0);
                    done = JobPoolWatchJobComplete(id);
                }
                if (done > 0) {
                    // R8b: DRAIN ONLY. NO WRITE. R8 zeroed this field and the
                    // run died at SIX doubled frames, in `mov rax,[r12]` at
                    // client+0x1A8A3B on an object read out of a table -- because
                    // the field has a SECOND consumer that R8 ignored:
                    //     client+0x372781  cmp dword [r15+0xF1C80], 0
                    //     client+0x372789  jne 0x372796
                    // which is 0x135 bytes BEFORE the read at +0x3728B6. Zeroing
                    // it flipped that branch for PASS 1 -- the engine's own pass
                    // -- so work that populates the table never ran. O2 recorded
                    // that `cmp` and R8's reasoning used only the wait. One
                    // value, two readers; fix it at both or not at all.
                    //
                    // Nothing needs to be written. Generations only move FORWARD,
                    // so having observed "complete" here, the engine's own check
                    // at tier0+0x9629 must observe it too and return at
                    // `jne 0x9685` WITHOUT parking. Draining is sufficient and it
                    // touches no game state at all -- it only delays pass 1.
                    if (first > 0) g_preWaitAlreadyDone.fetch_add(1, std::memory_order_relaxed);
                    else g_preWaitDrained.fetch_add(1, std::memory_order_relaxed);
                    const std::uint64_t n =
                        g_preWaitZeroed.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (n == 1 || (n % 240) == 0) {
                        char line[340]{};
                        std::snprintf(line, sizeof(line),
                            "[TF2VR] R8b DRAIN #%llu: job 0x%08X %s after %u spins. NOTHING "
                            "WRITTEN -- generations only move forward, so the engine's own check "
                            "at tier0+0x9629 must now also see it complete and return without "
                            "parking.\n",
                            static_cast<unsigned long long>(n), id,
                            first > 0 ? "was ALREADY COMPLETE" : "reached completion", spins);
                        Tf2VrLog(line);
                    }
                } else {
                    // TIMED OUT: leave it. The engine then waits exactly as it
                    // always has, so a timeout is today's behaviour rather than
                    // a new one -- and a climbing count here means the premise
                    // is wrong and the run says so out loud.
                    g_preWaitTimedOut.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }

    // R2: SNAPSHOT THE LATCH BEFORE PASS 1 CONSUMES IT. Read here and nowhere
    // else, because pass 1's draw clears it and after that the original value
    // does not exist anywhere in the process.
    std::uint32_t latchBefore = 0;
    bool latchSnapped = false;
    auto* latchAddress = reinterpret_cast<std::uint32_t*>(
        reinterpret_cast<std::uint8_t*>(self) + kViewRenderLatch);
    if (IsReadable(latchAddress, sizeof(std::uint32_t))) {
        latchBefore = *static_cast<volatile std::uint32_t*>(latchAddress);
        latchSnapped = true;
        JobPoolWatchNoteWaitedJob(latchBefore);
    } else {
        g_latchUnreadable.fetch_add(1, std::memory_order_relaxed);
    }

    // R1 BRACKETS BOTH PASSES, FROM INSIDE THE DOUBLED FRAME. Three samples,
    // and the three together say WHICH pass consumes the batches: if pass 1
    // costs nothing and pass 2 costs one, the nested draw is the consumer and
    // R2 is aimed. A periodic status thread could not answer this -- it would
    // stop before the arm and print zeros that read as findings.
    JobPoolWatchSample("DOUBLED, pre-pass1", self, true, true);
    PassFootprint b1{}, a1{};
    SnapshotBefore(&b1);
    if (g_burst.shots == 0) g_jtRecording.store(true, std::memory_order_release);
    // SYMMETRY, WRITTEN BEFORE PASS 1. With only batch 2 moved, the pair keeps
    // its separation but its midpoint sits half an IPD off the head -- which the
    // wearer reported as BOTH eyes sliding sideways the moment a burst arms.
    // This puts batch 1 at -half; batch 2's existing RELATIVE write then carries
    // the same block from -half to +half, so the pair straddles the head.
    //
    // It depends on the two writes ACCUMULATING, i.e. on the engine not
    // rebuilding the block between the passes. That is an assumption and it is
    // deliberately not asserted here: both writes log their own BEFORE and AFTER
    // origins, so the run itself says whether pass 2 started from -half or from
    // centre. If it started from centre the pair is [-half, +full] and the fix
    // is wrong rather than the idea.
    // COUNTED AT THE CALL SITE, not inside the writer. The writer reports
    // writes and declines and BOTH read clean, while no -0.50 line has ever
    // appeared -- so the question is whether this call is REACHED, and only a
    // counter here can answer it. A counter inside the callee cannot
    // distinguish "never called" from "called and returned early", which is the
    // exact ambiguity that has now cost two runs.
    ++g_symmetricSiteReached;
    if (SymmetricEyePairArmed()) {
        ++g_symmetricArmedHere;
        if (WriteBatch2Camera(self, viewBlock, -0.5f)) ++g_symmetricPreWrites;
    }
    t_passTag = 1;
    LARGE_INTEGER t0{}, t1{};
    QueryPerformanceCounter(&t0);
    // R3: BOTH passes are bracketed, not just the one we expect to die. The
    // whole point of this rung is that the expectation was wrong -- it is pass
    // 1, the engine's own call, that never returns on frame 130.
    StallSectionOpen(1, g_continuousFrames.load(std::memory_order_relaxed) + 1);
    void* result = g_original(self, viewBlock, a3, a4);
    StallSectionClose();
    QueryPerformanceCounter(&t1);
    t_passTag = 0;
    g_pass1Returns.fetch_add(1, std::memory_order_relaxed);
    JobPoolWatchSample("DOUBLED, post-pass1", self, true, false);
    SnapshotBefore(&a1);
    a1.result = result;
    a1.ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * toMs;
    LogPass("pass 1 (CONTROL, the engine's own call)", b1, a1);

    // ---- THE IDLE GAP: the control for asynchronous attribution. ----
    //
    // The witnesses are process-wide counters and the passes are measured back
    // to back, so work QUEUED by pass 1 and executed later lands inside pass 2's
    // window and is credited to pass 2. That is not hypothetical: the last two
    // runs disagreed about which pass did more, in opposite directions.
    //
    // This window does nothing at all. Whatever the counters move by here is
    // work arriving from other threads, and it is the scale against which pass
    // 2's numbers have to be read. A gap comparable to pass 2 means the per-pass
    // split is noise; a gap near zero means the split is real.
    // DURATION-MATCHED, or it is not a control at all. The first version just
    // took two snapshots back to back, so the gap was tens of microseconds
    // against pass 2's ~0.5 ms, and comparing their raw counts compared two
    // different amounts of time. This spins for exactly as long as pass 1 took,
    // making no call, so "what arrives from other threads in a window this
    // size" is measured on the same clock as the pass it has to be read
    // against. It costs one sub-millisecond stall, once per session.
    LARGE_INTEGER gapStart{}, gapNow{};
    QueryPerformanceCounter(&gapStart);
    const long long gapTicks = t1.QuadPart - t0.QuadPart;
    do {
        QueryPerformanceCounter(&gapNow);
    } while (gapNow.QuadPart - gapStart.QuadPart < gapTicks);

    // ---- N2/C-ENGINE: MOVE THE CAMERA. The one behavioural change. -------
    //
    // Placed HERE, after the idle gap and before pass 2's baseline snapshot, so
    // the write and its own log lines land OUTSIDE the window pass 2's timings
    // and upload deltas are measured in. Everything it does is described in
    // view_block_camera.h; the short version is that it moves one float of the
    // engine's own view matrix and lets the engine's own builders rebuild the
    // viewproj and the camera origin, which is what the four mods in
    // `C:\dev\othermods` do and what no rung here has ever tried.
    const bool cameraMoved = WriteBatch2Camera(self, viewBlock);

    PassFootprint b2{}, a2{};
    SnapshotBefore(&b2);
    const unsigned long long gapViewports = b2.viewportTotal - a1.viewportTotal;
    const unsigned long long gapUploads = b2.uploads - a1.uploads;
    const double gapMs = static_cast<double>(gapNow.QuadPart - gapStart.QuadPart) * toMs;
    g_doubleAttempts.fetch_add(1, std::memory_order_relaxed);
    // ---- R2b: RECOMPUTE THE THREE VIEW BLOCKS, as the caller does. ---------
    //
    // Placed after the camera write and immediately before the nested call, in
    // the caller's own order. Every refusal is counted; a zero here with the
    // arm ON means it never ran and the run is a control, not a test.
    if (g_recomputeBlocks.load(std::memory_order_acquire)) {
        const auto fnAddress = g_clientBase ? g_clientBase + kViewBlockRecompute : 0;
        if (!g_clientBase || !IsReadable(reinterpret_cast<const void*>(fnAddress), 16)) {
            g_recomputeDeclined.fetch_add(1, std::memory_order_relaxed);
        } else {
            auto* recompute = reinterpret_cast<RecomputeFn>(fnAddress);
            auto* base = reinterpret_cast<std::uint8_t*>(self);
            bool ok = true;
            const std::uintptr_t blocks[3] = {kBlockArg2, kBlockOrigin, kBlockSky};
            for (const std::uintptr_t off : blocks) {
                // 0x1C0 covers every field 0x36A060 reads or writes: view +0x40,
                // proj +0x80, viewproj +0xC0, and the two ints at +0x198/+0x19C.
                if (!IsReadable(base + off, 0x1C0)) { ok = false; break; }
            }
            if (!ok) {
                g_recomputeDeclined.fetch_add(1, std::memory_order_relaxed);
            } else {
                // Read one word of viewproj either side, so the log says the
                // call CHANGED something rather than merely that it returned.
                const float before =
                    *reinterpret_cast<const float*>(base + kBlockArg2 + 0xC0);
                recompute(base + kBlockArg2);
                recompute(base + kBlockOrigin);
                recompute(base + kBlockSky);
                const float after =
                    *reinterpret_cast<const float*>(base + kBlockArg2 + 0xC0);
                const std::uint64_t n =
                    g_recomputeDone.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n == 1 || (n % 240) == 0) {
                    char line[320]{};
                    std::snprintf(line, sizeof(line),
                        "[TF2VR] R2b RECOMPUTE #%llu: client+0x36A060 on all three blocks before "
                        "pass 2. arg2 viewproj[0] %.6f -> %.6f.\n",
                        static_cast<unsigned long long>(n),
                        static_cast<double>(before), static_cast<double>(after));
                    Tf2VrLog(line);
                }
            }
        }
    }

    // ---- R2: RESTORE THE LATCH. Config-only, DEFAULT OFF (measured harmful).
    //
    // Placed as late as possible -- nothing runs between this and the nested
    // call -- so no engine code can clear it again before pass 2 reads it.
    // Every outcome is counted, including the two that look like success from
    // the outside: "already non-zero" (pass 1 did not consume it after all,
    // which would falsify R1's reading) and "nothing to restore" (the snapshot
    // was itself 0, so this frame had no latch to give back).
    if (g_restoreLatch.load(std::memory_order_acquire) && latchSnapped) {
        const std::uint32_t now = *static_cast<volatile std::uint32_t*>(latchAddress);
        if (latchBefore == 0) {
            g_latchNothingToRestore.fetch_add(1, std::memory_order_relaxed);
        } else if (now != 0) {
            g_latchAlreadySet.fetch_add(1, std::memory_order_relaxed);
        } else {
            *static_cast<volatile std::uint32_t*>(latchAddress) = latchBefore;
            const std::uint64_t n = g_latchRestored.fetch_add(1, std::memory_order_relaxed) + 1;
            // PROVES IT TOOK, on the first and every 240th, by reading the
            // field back after the write. A restore that silently did not land
            // looks exactly like a restore that did not help.
            if (n == 1 || (n % 240) == 0) {
                char line[300]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] R2 LATCH RESTORE #%llu: this+0xF1C80 was 0x%08X before pass 1, 0 "
                    "after it, put back and reads 0x%08X. Pass 2 now sees what pass 1 saw.\n",
                    static_cast<unsigned long long>(n), latchBefore,
                    *static_cast<volatile std::uint32_t*>(latchAddress));
                Tf2VrLog(line);
            }
        }
    }

    DWORD faultCode = 0;
    LARGE_INTEGER t2{}, t3{};
    t_passTag = 2;
    QueryPerformanceCounter(&t2);
    // ---- R4: PASS 2 GETS ITS OWN JOB GROUP. The one change this run. -------
    std::uint32_t ownGroup = 0;
    std::uint32_t savedGroupId = 0;
    auto* groupSlot = g_clientBase
        ? reinterpret_cast<std::uint32_t*>(g_clientBase + kViewJobGroupGlobal)
        : nullptr;
    if (g_ownJobGroup.load(std::memory_order_acquire) && groupSlot && g_origBegin &&
        IsReadable(groupSlot, sizeof(std::uint32_t))) {
        // Parent 0 = top level. A parent would re-couple our group's lifetime to
        // pass 1's, which is the whole thing being undone here.
        ownGroup = g_origBegin(0);
        if (ownGroup == 0) {
            // JT_BeginJobGroup ORs 0x3000 into every id it returns, so 0 is not
            // a valid group and must never be published. Counted, not used.
            g_ownGroupsDeclined.fetch_add(1, std::memory_order_relaxed);
        } else {
            savedGroupId = *static_cast<volatile std::uint32_t*>(groupSlot);
            g_ownGroupEnded.store(false, std::memory_order_release);
            g_ownGroupActive.store(ownGroup, std::memory_order_release);
            *static_cast<volatile std::uint32_t*>(groupSlot) = ownGroup;
            if (*static_cast<volatile std::uint32_t*>(groupSlot) != ownGroup) {
                g_ownGroupSwapFailed.fetch_add(1, std::memory_order_relaxed);
            }
            const std::uint64_t n = g_ownGroupsBegun.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n == 1 || (n % 240) == 0) {
                char line[320]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] R4 OWN GROUP #%llu: began 0x%08X and published it in "
                    "client+0xEA9EE4 (was 0x%08X, restored after pass 2). Pass 2's End sites now "
                    "target OUR group.\n",
                    static_cast<unsigned long long>(n), ownGroup, savedGroupId);
                Tf2VrLog(line);
            }
        }
    } else if (g_ownJobGroup.load(std::memory_order_acquire)) {
        g_ownGroupsDeclined.fetch_add(1, std::memory_order_relaxed);
    }

    void* nestedResult = nullptr;
    StallSectionOpen(2, g_continuousFrames.load(std::memory_order_relaxed) + 1);
    const bool returned =
        CallNestedGuarded(g_original, self, viewBlock, a3, a4, &nestedResult, &faultCode);
    StallSectionClose();
    // Restore the engine's id FIRST, so nothing that runs after this point can
    // see our group published, then drop our own reference. Ordered this way
    // because the release can free the slot and a stale published id would then
    // name a recycled group -- which is the exact class of bug being fixed.
    if (ownGroup != 0 && groupSlot) {
        *static_cast<volatile std::uint32_t*>(groupSlot) = savedGroupId;
        g_ownGroupActive.store(0, std::memory_order_release);
        // R4b: close it ONLY if pass 2 did not. R4 always closed it and that
        // second release is what killed the run at 23 frames.
        if (g_ownGroupEnded.load(std::memory_order_acquire)) {
            g_ownGroupClosedByPass2.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_ownGroupClosedByUs.fetch_add(1, std::memory_order_relaxed);
            if (g_origEnd) g_origEnd(ownGroup);
        }
    }
    QueryPerformanceCounter(&t3);
    // THE CALL THE ENGINE MAKES AFTER EVERY SCENE DRAW, AND WE NEVER DID.
    //
    // The caller at client+0x35A8E0 does this, and the three instructions are
    // consecutive with the scene draw:
    //
    //     0035AAE3  call [rax+0x48]      <- the scene draw we re-enter
    //     0035AAE6  call 0x1ADD50        <- returns *(void**)client+0xFB3780
    //     0035AAEB  mov rdx, [rax]
    //     0035AAF1  call [rdx+0xB8]      <- a virtual call on that singleton
    //
    // Our re-entry calls the middle one and stops, so whatever slot +0xB8 winds
    // up is wound up ONCE per frame instead of once per scene draw. That leaks
    // exactly one per DOUBLED frame, which is precisely the rate measured: the
    // process hangs at 128 doubled frames, cumulative for the life of the
    // process, with none of our stereo machinery armed.
    //
    // The shape came from Halo MCC VR in C:\dev\othermods, which hit this class
    // of bug and wrote it down: "The scene render consumes a once-per-frame
    // latch and clears it, so a second pass in the same frame must restore it
    // or silently skip work", and "its callers zero this span of the render
    // context before every call."
    //
    // GUARDED, because this calls an engine virtual whose slot we have not read
    // the body of. The pointer and its vtable entry must both be readable and
    // the entry must land inside client.dll, or it declines and counts.
    if (SceneDrawEpiloguePerformed()) g_epilogueCalls.fetch_add(1, std::memory_order_relaxed);
    JobPoolWatchSample("DOUBLED, post-pass2", self, true, false);
    t_passTag = 0;
    g_jtRecording.store(false, std::memory_order_release);

    if (!returned) {
        g_doubleFaults.fetch_add(1, std::memory_order_relaxed);
        if (!g_continuous.load(std::memory_order_acquire))
            g_frameIsDoubled.store(false, std::memory_order_release);
        // A fault ends continuous mode too, or the poisoned session keeps
        // trying to double every frame forever.
        g_continuous.store(false, std::memory_order_release);
        g_poisoned.store(true, std::memory_order_release);
        char fault[240]{};
        std::snprintf(fault, sizeof(fault),
            "[TF2VR] F1 pass 2: FAULTED, code 0x%08X. True stereo is POISONED for this "
            "session; pass 1 is untouched and the game continues.\n",
            static_cast<unsigned>(faultCode));
        Tf2VrLog(fault);
        return result;
    }

    // THE ARMED FRAME IS OVER. Without this the flag stays true for the rest
    // of the session and every later frame keeps capturing at its own second
    // scene pass -- measured as 1704 captures from a ONE-frame burst.
    // NOT LOWERED IN CONTINUOUS MODE: the next frame is doubled too, and
    // lowering it here is what let the upload thread see a stale false and
    // apply AER's alternating sign instead of the same-frame one.
    if (!g_continuous.load(std::memory_order_acquire))
        g_frameIsDoubled.store(false, std::memory_order_release);
    g_doubleReturns.fetch_add(1, std::memory_order_relaxed);
    // M1. Tell the render-target census that a doubled submission happened, as
    // a MONOTONIC COUNT rather than a flag. g_frameIsDoubled is raised and
    // lowered on this thread, around a call that only QUEUES: by the time the
    // render thread executes the two batches the flag may already be back to
    // false, and an interval labelled from it would be labelled from a state
    // that no longer describes the work being done. A count can be compared at
    // both ends of an interval, which is what the census does.
    RtvCensusNoteDoubledSubmission();
    SnapshotBefore(&a2);
    a2.result = nestedResult;
    a2.ms = static_cast<double>(t3.QuadPart - t2.QuadPart) * toMs;
    LogPass("pass 2 (NESTED)", b2, a2);
    // N2's own counters, next to the pass they belong to. `cameraMoved` says
    // whether THIS frame took the write; the status line says why not when it
    // did not, and no decline is silent.
    if (!cameraMoved) {
        Tf2VrLog("[TF2VR] N2: this doubled frame did NOT take the engine-side camera write. The "
                 "status line below names which decline it took, and any disparity measured on "
                 "this frame belongs to something else.\n");
    }
    ReportEngineCameraWrite();

    // THE VERDICT, and it names its own witness rather than assuming one works.
    // Each witness is read against the CONTROL pass first: a witness that saw
    // nothing on the engine's own call is disqualified, loudly, instead of
    // being reported as a nested zero.
    const unsigned long long up1 = a1.uploads - b1.uploads;
    const unsigned long long up2 = a2.uploads - b2.uploads;
    const unsigned long long vp1 = a1.viewportTotal - b1.viewportTotal;
    const unsigned long long vp2 = a2.viewportTotal - b2.viewportTotal;
    const double timeRatio = a1.ms > 0.0 ? 100.0 * a2.ms / a1.ms : 0.0;

    // THE WITNESS THAT SURVIVED. Everything above is a time-window delta, and
    // the burst showed those cannot attribute asynchronous work: every frame
    // carried ~35 uploads and they landed in whichever of pass 1, the idle spin
    // or pass 2 happened to be open, with the frame total unchanged. These two
    // are counted inside the upload hook itself, keyed on a thread-local tag,
    // so they say what each pass ISSUED rather than what arrived while it ran.
    const unsigned long long own1 = a1.uploadsByPass1 - b1.uploadsByPass1;
    const unsigned long long own2 = a2.uploadsByPass2 - b2.uploadsByPass2;
    // THE WITNESS'S OWN POSITIVE CONTROL, and it is not optional. "Engine 0,
    // nested 0" is exactly what a thread-local that failed to cross the
    // translation-unit boundary would print, and that is indistinguishable from
    // the real finding without this number. Tag 0 is every upload issued
    // OUTSIDE a tagged pass: if it is large the tally is plumbed and reaching
    // the hook, and the zeros mean what they say. If it is ALSO zero, the
    // witness is broken and nothing on this line counts.
    const unsigned long long untagged = CameraUploadsForPass(0);
    char ownLine[700]{};
    std::snprintf(ownLine, sizeof(ownLine),
        "[TF2VR] F1 THREAD-ATTRIBUTED uploads: engine pass ISSUED %llu | nested pass ISSUED "
        "%llu | CONTROL untagged-total %llu. A large control with two zeros means the scene "
        "draw issues no camera upload on its own thread -- it queues, and the D3D work lands "
        "on another thread. A control of ZERO means the tally never reached the hook and "
        "these two zeros are instrument failure.\n",
        own1, own2, untagged);
    Tf2VrLog(ownLine);

    char queued[420]{};
    std::snprintf(queued, sizeof(queued),
        "[TF2VR] F1 QUEUED BY THE NESTED PASS: JT_BeginJobGroup x%llu, JT_EndJobGroup x%llu. "
        "A submission seam does no D3D of its own, so this -- not an upload count -- is what "
        "says the nested call did engine work rather than falling through.\n",
        a2.jtBeginByPass2 - b2.jtBeginByPass2, a2.jtEndByPass2 - b2.jtEndByPass2);
    Tf2VrLog(queued);

    char gapLine[520]{};
    std::snprintf(gapLine, sizeof(gapLine),
        "[TF2VR] F1 idle gap (ASYNC CONTROL, no call made, DURATION-MATCHED to pass 1): "
        "%.3f ms | viewportSets +%llu | uploads +%llu. This is what arrives from other "
        "threads in a window of this length. Pass 2's numbers mean a second render only "
        "insofar as they exceed THESE.\n",
        gapMs, gapViewports, gapUploads);
    Tf2VrLog(gapLine);

    // THE WITNESS IS CHOSEN BY WHICH ONE THE IDLE CONTROL DOES NOT SWAMP.
    //
    // Picking it by "did it move on pass 1" was wrong: the duration-matched gap
    // moves viewportSets by MORE than pass 1 does, so that witness is ambient
    // traffic and reports a second render whatever happens. A witness is only
    // usable if the nested pass clears the idle window by a wide margin.
    const char* witness = "NONE";
    double ratio = 0.0;
    if (up2 > gapUploads * 3 && up2 > 0) {
        witness = "cameraUploads";
        ratio = 100.0 * static_cast<double>(up2) / static_cast<double>(up1 ? up1 : 1);
    } else if (vp2 > gapViewports * 3 && vp2 > 0) {
        witness = "viewportSets";
        ratio = 100.0 * static_cast<double>(vp2) / static_cast<double>(vp1 ? vp1 : 1);
    }

    char verdict[860]{};
    if (std::strcmp(witness, "NONE") == 0) {
        std::snprintf(verdict, sizeof(verdict),
            "[TF2VR] F1 VERDICT: NO USABLE WITNESS. Nested uploads +%llu vs an idle window of "
            "the same length at +%llu; nested viewportSets +%llu vs idle +%llu. Neither clears "
            "the ambient traffic by 3x, so neither can say whether the nested pass rendered. "
            "Timing is the only live reading: pass 1 %.3f ms, pass 2 %.3f ms (%.1f%%).\n",
            up2, gapUploads, vp2, gapViewports, a1.ms, a2.ms, timeRatio);
    } else {
        const unsigned long long w1 = std::strcmp(witness, "viewportSets") == 0 ? vp1 : up1;
        const unsigned long long w2 = std::strcmp(witness, "viewportSets") == 0 ? vp2 : up2;
        const unsigned long long wg = std::strcmp(witness, "viewportSets") == 0 ? gapViewports : gapUploads;
        std::snprintf(verdict, sizeof(verdict),
            "[TF2VR] F1 VERDICT: witness=%s (it clears the duration-matched idle window). "
            "engine pass %llu | idle %llu | NESTED %llu (%.1f%% of the engine pass). Timing: "
            "pass 1 %.3f ms, pass 2 %.3f ms (%.1f%%). A nested burst well clear of idle is a "
            "SECOND RENDER and F1 exits to F2; at idle level it returned without rendering, "
            "which is a SEAM result and sends this to F1s.\n",
            witness, w1, wg, w2, ratio, a1.ms, a2.ms, timeRatio);
    }
    Tf2VrLog(verdict);

    // THE BURST SUMMARY. A distribution, because one armed frame cannot tell a
    // per-frame property of the GAME from a property of the mechanism -- which
    // is exactly what the 5-to-30 spread across single frames left open.
    {
        BurstStat& s = g_burst;
        ++s.shots;
        s.sumEngine += own1; s.sumIdle += gapUploads; s.sumNested += own2;
        s.sumMsEngine += a1.ms; s.sumMsNested += a2.ms;
        if (s.shots == 1 || own2 < s.minNested) s.minNested = own2;
        if (s.shots == 1 || own2 > s.maxNested) s.maxNested = own2;
        if (own2 > 0) ++s.shotsClearingIdle;
        char b[520]{};
        std::snprintf(b, sizeof(b),
            "[TF2VR] F1 burst shot %d/%d: ISSUED engine %llu, nested %llu | (window deltas, "
            "shown only to expose how far they disagree: engine %llu idle %llu nested %llu) "
            "| %.3f ms vs %.3f ms\n",
            s.shots, g_shotsPerPress.load(std::memory_order_relaxed),
            own1, own2, up1, gapUploads, up2, a1.ms, a2.ms);
        Tf2VrLog(b);
        if (g_shotsRemaining.load(std::memory_order_acquire) == 0) {
            char sum[640]{};
            std::snprintf(sum, sizeof(sum),
                "[TF2VR] F1 BURST SUMMARY over %d consecutive frames, THREAD-ATTRIBUTED: "
                "uploads ISSUED BY the nested pass min %llu / mean %.1f / max %llu, nonzero "
                "on %d of %d shots; issued by the engine's own pass mean %.1f. (Idle-window "
                "mean %.1f is kept only as the scale of the ambient traffic that made the "
                "window deltas useless.) Mean time %.3f ms engine vs %.3f ms nested. Double "
                "releases suppressed: %llu.\n"
                "[TF2VR] READ IT THIS WAY: nested ISSUED near the engine's = a second render, "
                "and F1 exits to F2. Nested ISSUED at zero while the engine's is also zero = "
                "the scene draw does not upload on its own thread at all, and the witness is "
                "wrong rather than the mechanism. Nested zero while the engine's is nonzero = "
                "it returned without rendering, a SEAM result, and this goes to F1s.\n",
                s.shots, s.minNested, s.sumNested / (double)s.shots, s.maxNested,
                s.shotsClearingIdle, s.shots,
                s.sumEngine / (double)s.shots, s.sumIdle / (double)s.shots,
                s.sumMsEngine / s.shots, s.sumMsNested / s.shots,
                g_suppressedReleases.load(std::memory_order_relaxed));
            Tf2VrLog(sum);
        }
    }

    // AND THE DOUBLED FRAME ITSELF. Requested from inside the armed frame, so
    // the next Present carries an image this pass contributed to. One capture
    // per burst -- the first shot only -- because eight identical bitmaps
    // answer nothing the first one did not.
    // The arm state is in the name, for the same reason M2's pair carries it:
    // an armed burst and a disarmed one in the same session must not overwrite
    // each other's evidence.
    if (g_burst.shots == 1) {
        RequestNamedBackbufferCapture(RtvSubstituteArmed() ? "tf2vr-reentry-D-ARMED.bmp"
                                                           : "tf2vr-reentry-D-DISARMED.bmp");
    }
    // The last shot of the burst starts the post-burst flicker probe.
    if (g_shotsRemaining.load(std::memory_order_acquire) == 0) { g_postStage = 1; g_postFrames = 0; }

    // THE JOB-GROUP SEQUENCE, ids and all. This is the reading that decides
    // Stage O 2.2, and it is stated as pass 1's calls beside pass 2's rather
    // than as a global sampled twice.
    const int events = g_jtEventCount.load(std::memory_order_acquire);
    char jtHead[420]{};
    std::snprintf(jtHead, sizeof(jtHead),
        "[TF2VR] F1 jobgroups on the armed frame: pass1 begin=%llu end=%llu | pass2 begin=%llu "
        "end=%llu | outside begin=%llu end=%llu. An End in pass 2 on an id pass 1 already "
        "ended is the double-release; equal counts on DIFFERENT ids is not.\n",
        g_beginCalls[1].load(std::memory_order_relaxed), g_endCalls[1].load(std::memory_order_relaxed),
        g_beginCalls[2].load(std::memory_order_relaxed), g_endCalls[2].load(std::memory_order_relaxed),
        g_beginCalls[0].load(std::memory_order_relaxed), g_endCalls[0].load(std::memory_order_relaxed));
    Tf2VrLog(jtHead);
    const int shown = events > kMaxJtEvents ? kMaxJtEvents : events;
    for (int i = 0; i < shown; ++i) {
        char ev[200]{};
        std::snprintf(ev, sizeof(ev),
            "[TF2VR]   jt[%02d] pass%d %s id=0x%08X%s\n", i, g_jtEvents[i].tag,
            g_jtEvents[i].isBegin ? "Begin" : "End  ", g_jtEvents[i].id,
            g_jtEvents[i].isBegin ? "" : "");
        Tf2VrLog(ev);
    }
    if (events > kMaxJtEvents) {
        char over[160]{};
        std::snprintf(over, sizeof(over),
            "[TF2VR]   jt: %d events TRUNCATED at the %d-entry cap -- the counts above are "
            "complete, the list is not.\n", events, kMaxJtEvents);
        Tf2VrLog(over);
    }
    return result;
}

void InstallJtCensus() {
    if (g_jtCensusInstalled || !g_clientBase) return;
    auto* beginSlot = reinterpret_cast<void**>(g_clientBase + kIatBeginJobGroup);
    auto* endSlot = reinterpret_cast<void**>(g_clientBase + kIatEndJobGroup);
    if (!IsReadable(beginSlot, sizeof(void*)) || !IsReadable(endSlot, sizeof(void*))) return;
    // Refuse if either already holds ours: reading our own stub back as the
    // original builds a call that recurses into itself.
    if (*beginSlot == reinterpret_cast<void*>(&HookBeginJobGroup) ||
        *endSlot == reinterpret_cast<void*>(&HookEndJobGroup)) {
        g_jtCensusInstalled = true;
        return;
    }
    // The IAT must point into tier0, or these are not the imports the offline
    // read named and nothing here is safe to call.
    HMODULE tier0 = GetModuleHandleA("tier0.dll");
    MEMORY_BASIC_INFORMATION mbi{};
    if (!tier0 || !VirtualQuery(*beginSlot, &mbi, sizeof(mbi)) || mbi.AllocationBase != tier0) {
        Tf2VrLog("[TF2VR] JT census: the IAT slots do not point into tier0; NOT installed.\n");
        g_jtCensusInstalled = true;  // Refuse once, loudly, and never retry.
        return;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(beginSlot, sizeof(void*) * 2, PAGE_READWRITE, &oldProtect)) {
        Tf2VrLog("[TF2VR] JT census: IAT would not go writable; NOT installed.\n");
        g_jtCensusInstalled = true;
        return;
    }
    g_origBegin = reinterpret_cast<BeginJobGroupFn>(*beginSlot);
    g_origEnd = reinterpret_cast<EndJobGroupFn>(*endSlot);
    *beginSlot = reinterpret_cast<void*>(&HookBeginJobGroup);
    *endSlot = reinterpret_cast<void*>(&HookEndJobGroup);
    DWORD ignored = 0;
    VirtualProtect(beginSlot, sizeof(void*) * 2, oldProtect, &ignored);
    g_jtCensusInstalled = true;
    Tf2VrLog("[TF2VR] JT census: JT_BeginJobGroup and JT_EndJobGroup counted by IAT swap. "
             "Read-only: both forward unchanged. Calls are attributed to the engine's own "
             "scene-draw pass, the nested one, or neither.\n");
}

void InstallHook() {
    if (g_installed || g_installRefused) return;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return;
    g_clientBase = reinterpret_cast<std::uintptr_t>(client);
    auto* slot = reinterpret_cast<void**>(g_clientBase + kSceneDrawSlot);
    const auto expected = g_clientBase + kSceneDrawFunction;

    if (!IsReadable(slot, sizeof(void*)) || *slot == reinterpret_cast<void*>(&SceneDrawHook)) {
        g_installRefused = true;
        return;
    }
    if (reinterpret_cast<std::uintptr_t>(*slot) != expected) {
        char line[220]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] scene re-entry: slot holds %p, expected client+0x%llX; NOT installed.\n",
            *slot, static_cast<unsigned long long>(kSceneDrawFunction));
        Tf2VrLog(line);
        g_installRefused = true;
        return;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        Tf2VrLog("[TF2VR] scene re-entry: slot would not go writable; NOT installed.\n");
        g_installRefused = true;
        return;
    }
    g_original = reinterpret_cast<SceneDrawFn>(*slot);
    *slot = reinterpret_cast<void*>(&SceneDrawHook);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    g_installed = true;
    Tf2VrLog("[TF2VR] scene re-entry: CViewRender scene-draw slot hooked (client+0x9287F0 -> "
             "client+0x3723B0). Pass 1 is the engine's own call, unguarded, always.\n");
}
}  // namespace

void SetSceneCensusWanted(bool wanted) { g_censusWanted.store(wanted, std::memory_order_release); }

void SetSceneReentryFrames(int frames) {
    // ZERO IS CONTINUOUS. It used to clamp to 1, which silently turned the
    // setting that asks for a MODE into a one-frame burst.
    const int n = frames < 0 ? 0 : (frames > 64 ? 64 : frames);
    g_shotsPerPress.store(n, std::memory_order_release);
    char line[200]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] stereo.reentry_frames = %d: %s\n", n,
        n == 0 ? "ZERO IS CONTINUOUS -- F6 LATCHES every frame doubled and F6 again stops it. "
                 "This is the first unbounded mode this route has had; the 64-double "
                 "accumulation crash is real, unattributed and NOT fixed, so the doubled-frame "
                 "count is logged as it climbs and the run says how far it got."
               : "one press arms that many consecutive doubles.");
    Tf2VrLog(line);
}

void SetSceneReentryGuard(bool on) {
    g_reentryGuard.store(on, std::memory_order_release);
    Tf2VrLog(on ? "[TF2VR] stereo.reentry_guard ON: a SECOND JT_EndJobGroup from the nested pass, "
                  "on an id already ended, will not reach tier0. Nothing else is changed.\n"
                : "[TF2VR] stereo.reentry_guard OFF: the nested pass releases as measured.\n");
}

void SetSceneJobPreWaitMs(float ms) {
    const unsigned long long clamped =
        ms < 0.0f ? 0ull : (ms > 5000.0f ? 5000ull : static_cast<unsigned long long>(ms));
    g_preWaitMaxMs.store(clamped, std::memory_order_release);
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] stereo.prewait_ms = %llu. R8b's single timedOut=1 at 12 ms WAS the hung frame, so "
        "this is the window that has to cover the slow job. timedOut must reach 0.\n", clamped);
    Tf2VrLog(line);
}

void SetSceneJobPreWait(bool on) {
    g_preWaitJob.store(on, std::memory_order_release);
    Tf2VrLog(on ? "[TF2VR] stereo.prewait_job ON: the waited job is polled to completion with the "
                  "engine own test before pass 1, then this+0xF1C80 is zeroed so the draw skips "
                  "its own wait. R7 caught that wait parked on an ALREADY-COMPLETED job.\n"
                : "[TF2VR] stereo.prewait_job OFF: the engine waits as it always has. "
                  "This is the control arm.\n");
}

void SetSceneOwnJobGroup(bool on) {
    g_ownJobGroup.store(on, std::memory_order_release);
    Tf2VrLog(on ? "[TF2VR] stereo.own_job_group ON: pass 2 begins its OWN job group and publishes "
                  "it in client+0xEA9EE4 for the duration of the nested call, then restores the "
                  "engine's id and releases ours. The reentry guard should now suppress NOTHING -- "
                  "suppressedReleases must read 0, and that is this rung's POSITIVE CONTROL.\n"
                : "[TF2VR] stereo.own_job_group OFF: pass 2 ends pass 1's group, as it always "
                  "has. This is the control arm.\n");
}

void SetSceneRecomputeBlocks(bool on) {
    g_recomputeBlocks.store(on, std::memory_order_release);
    Tf2VrLog(on ? "[TF2VR] stereo.recompute_blocks ON: client+0x36A060 runs on all three view "
                  "blocks before pass 2, as the caller does before every scene draw. Pure "
                  "computation -- proj and viewproj rebuilt, nothing allocated.\n"
                : "[TF2VR] stereo.recompute_blocks OFF: pass 2 draws from whatever proj and "
                  "viewproj pass 1 left behind. This is the control arm.\n");
}

void SetSceneLatchRestore(bool on) {
    g_restoreLatch.store(on, std::memory_order_release);
    Tf2VrLog(on ? "[TF2VR] stereo.restore_latch ON: this+0xF1C80 is snapshotted before pass 1 and "
                  "the SAME BYTES are written back before pass 2. R1 measured pass 2 running with "
                  "it at 0 on 130 doubled frames out of 130.\n"
                : "[TF2VR] stereo.restore_latch OFF: pass 2 runs with the latch spent, as it "
                  "always has. This is the A/B control arm.\n");
}

void SetSceneReentryWanted(bool wanted) {
    g_reentryWanted.store(wanted, std::memory_order_release);
    Tf2VrLog(wanted
        ? "[TF2VR] stereo.reentry: the nested call is AVAILABLE. It still fires only on the "
          "arm key, once, and then disarms itself.\n"
        : "[TF2VR] stereo.reentry OFF.\n");
}

bool SceneReentryArmed() { return g_shotsRemaining.load(std::memory_order_acquire) > 0; }

bool SameFrameDoubleArmed() { return g_frameIsDoubled.load(std::memory_order_acquire); }

void ArmOneSceneReentry() {
    if (!g_reentryWanted.load(std::memory_order_acquire)) {
        Tf2VrLog("[TF2VR] F1: arm key pressed, but stereo.reentry is 0. Nothing armed.\n");
        return;
    }
    if (g_poisoned.load(std::memory_order_acquire)) {
        Tf2VrLog("[TF2VR] F1: arm key pressed, but the nested call is POISONED after a fault. "
                 "Restart the session to try again.\n");
        return;
    }
    if (!g_installed) {
        Tf2VrLog("[TF2VR] F1: arm key pressed, but the scene-draw slot is not hooked.\n");
        return;
    }
    // CONTINUOUS IS A LATCH, NOT A BURST. `stereo.reentry_frames = 0` selects
    // it, and then F6 turns stereo ON and F6 turns it OFF -- because the wearer
    // cannot judge stereo from a tenth of a second, and every rung so far has
    // handed them a flash.
    //
    // Off is one keypress away on purpose: this is the first mode that runs
    // unbounded, the accumulation crash is real and unattributed, and the
    // wearer needs to be able to stop it without killing the game.
    if (g_shotsPerPress.load(std::memory_order_acquire) == 0) {
        const bool on = !g_continuous.load(std::memory_order_acquire);
        g_continuous.store(on, std::memory_order_release);
        if (on) g_continuousFrames.store(0, std::memory_order_release);
        if (on) g_ladderStage = -1;
        // LATCHED FOR THE WHOLE MODE, and this is the jitter fix.
        //
        // g_frameIsDoubled is raised and lowered around a call that only
        // QUEUES -- the comment at the lowering site has said so since M1 --
        // and the camera upload that reads it through SameFrameDoubleArmed()
        // runs on another thread. So some uploads land while it is raised and
        // take the forced -1/+1 same-frame sign, and some land while it is
        // lowered and fall back to AER's ALTERNATING sign. The eye offset then
        // flickers between two values frame to frame, which is exactly the
        // "right eye jittering back and forth between two shifts" reported from
        // the headset.
        //
        // In continuous mode the race has a correct answer that needs no
        // synchronisation: EVERY frame is doubled, so the flag is simply true
        // for the whole mode. Burst mode keeps the old raise/lower exactly.
        g_frameIsDoubled.store(on, std::memory_order_release);
        // R1: the arm timestamp every pool line is judged against, taken HERE,
        // at the keypress that changes the behaviour, so a sample stamped
        // earlier is provably pre-arm rather than argued to be.
        if (on) { StartStallWatchdog(); JobPoolWatchNoteArm("F6 continuous ON"); }
        else JobPoolWatchSummary("F6 continuous OFF");
        char line[560]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] CONTINUOUS STEREO %s (F6). %s\n", on ? "ON" : "OFF",
            on ? "Every frame is doubled from now until F6 is pressed again. Expect the frame "
                 "rate to fall -- two scene renders per frame is what true stereo COSTS, not a "
                 "fault. The doubled-frame count is logged as it climbs, so if this dies the run "
                 "says how far it got, which is the first real measurement of the accumulation "
                 "crash anyone has taken."
               : "Back to single frames.");
        Tf2VrLog(line);
        if (!on) {
            char tail[240]{};
            std::snprintf(tail, sizeof(tail),
                "[TF2VR] CONTINUOUS STEREO ran %llu doubled frames before it was switched off.\n",
                g_continuousFrames.load(std::memory_order_relaxed));
            Tf2VrLog(tail);
        }
        return;
    }
    // THE CAPTURE LADDER, and the middle rung is the one that matters.
    //
    // The first attempt captured one control frame and one doubled frame 14 ms
    // apart and measured 39% of pixels changed. That number is worthless on its
    // own: the trees are animated, the weapon idles, and 14 ms of ordinary
    // gameplay moves a lot of pixels. Attributing it to the double would have
    // been the same error as reading a verdict off a window delta.
    //
    // So three captures, at MATCHED two-frame intervals:
    //   A -> B   no double between them. This is what 2 frames of this game
    //            costs by itself, and it is the number the third is read against.
    //   B -> D   the double happens in here.
    // If diff(A,B) and diff(B,D) are the same size, the double changed nothing
    // visible and the flash is something else. If diff(B,D) is much larger, the
    // difference is the double's.
    g_captureStage = 1;
    g_captureFrameCounter = 0;
    return;
}

// Steps the capture ladder above, one plugin frame at a time, and arms the
// shots only once both controls are on disk. Returns true while it owns the
// press, so the arm path does not fire early.
bool StepCaptureLadder() {
    if (g_captureStage == 0) return false;
    if (NamedCaptureOutstanding()) return true;  // let the previous one land
    if (++g_captureFrameCounter < 2) return true;
    g_captureFrameCounter = 0;
    switch (g_captureStage) {
        case 1:
            RequestNamedBackbufferCapture("tf2vr-reentry-A.bmp");
            g_captureStage = 2;
            return true;
        case 2:
            RequestNamedBackbufferCapture("tf2vr-reentry-B.bmp");
            g_captureStage = 3;
            return true;
        default:
            break;
    }
    g_captureStage = 0;
    return false;  // the caller arms the shots now.
}

// ---------------------------------------------------------------------------
// THE FLICKER PROBE. The wearer's report: "After hitting CTRL F4 I see the hud
// components including reticle flickering a bit. This continues until I quit
// the game."
//
// Eight doubles last 55 ms; the flicker lasts the session. So the nested pass
// does not merely double some work for eight frames, it LEAVES SOMETHING
// FLIPPED. Flicker is alternation, and alternation that outlives its cause is
// the signature of a double-buffered per-frame resource left on the wrong
// parity.
//
// A flicker cannot be seen in one frame, so the instrument is a PAIR of
// consecutive frames: if the HUD alternates, two adjacent frames disagree in
// the HUD and agree everywhere else. And the pair means nothing without the
// same pair taken before any double -- A and B already serve as exactly that,
// captured two frames apart with no double between them.
//
//   diff(A,B)   two adjacent frames, HUD healthy      <- the control
//   diff(E,F)   two adjacent frames, HUD flickering   <- the subject
//
// Both are ordinary gameplay pairs at the same spacing, so scene motion cancels
// and what is left in the HUD cells is the alternation itself. Taken well after
// the burst -- ~1.4 s -- so this measures the PERSISTENT state, not the eight
// doubled frames.
// SAMPLED AT THE ALIAS FREQUENCY, first time round. The pair was taken TWO
// frames apart to match the A/B control's spacing -- and something alternating
// every frame is in the SAME PHASE at N and N+2. diff(E,F) came back at 0.3%
// changed against the healthy control's 18.8%: not "no flicker in the
// backbuffer", but a strobe photograph of a spinning wheel.
//
// So: SIX CONSECUTIVE frames, and every adjacent pair is diffed. Consecutive
// catches period 2, six of them catch anything up to period 5 or an element
// that only blinks occasionally. This is the same mistake as reading a
// correlation curve at its argmin -- the reading was real, the sampling was
// wrong, and the fix is to sample where aliasing cannot hide the effect.
constexpr int kPostCaptures = 6;
void StepPostBurstCapture() {
    if (g_postStage == 0) return;
    if (NamedCaptureOutstanding()) return;        // let the previous one land
    if (g_postStage == 1) {
        if (++g_postFrames < 200) return;         // ~1.4 s after the burst
        g_postStage = 2; g_postFrames = 0;
    }
    if (g_postFrames >= kPostCaptures) {
        Tf2VrLog("[TF2VR] F1 flicker probe: six CONSECUTIVE frames captured ~1.4 s after the "
                 "burst (tf2vr-post-0..5). Every adjacent pair gets diffed; two frames apart "
                 "aliased a per-frame alternation into invisibility last time.\n");
        g_postStage = 0; g_postFrames = 0;
        return;
    }
    char name[40]{};
    std::snprintf(name, sizeof(name), "tf2vr-post-%d.bmp", g_postFrames);
    RequestNamedBackbufferCapture(name);
    ++g_postFrames;
}

void ArmShotsNow() {
    const int shots = g_shotsPerPress.load(std::memory_order_acquire);
    // The burst's own accounting is reset per press, so two presses in a
    // session give two independent distributions rather than one blended one.
    g_burst = BurstStat{};
    g_jtEventCount.store(0, std::memory_order_release);
    g_shotsRemaining.store(shots, std::memory_order_release);
    char line[240]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F1: %d nested call(s) armed. They fire on consecutive qualifying frames and "
        "then disarm; a repeat needs another press.\n", shots);
    Tf2VrLog(line);
}

void AdvanceSceneReentry(bool worldReady) {
    if (!g_censusWanted.load(std::memory_order_acquire) &&
        !g_reentryWanted.load(std::memory_order_acquire) &&
        !RtvCensusWanted()) {
        return;
    }
    InstallHook();
    if (!g_installed) return;
    InstallJtCensus();
    // The capture ladder owns the press until both control frames are written,
    // then arms the shots. A->B is the no-double control at the same two-frame
    // spacing as B->D, which is the only way "the doubled frame differs" can be
    // told from "two frames of this game differ".
    if (g_captureStage != 0 && !StepCaptureLadder()) ArmShotsNow();
    StepPostBurstCapture();
    g_censusArmed.store(worldReady && g_censusWanted.load(std::memory_order_acquire),
                        std::memory_order_release);

    // MAKE THE UPLOAD COUNTER LIVE, or F1's render question cannot be asked.
    //
    // F1's first run read "uploads 0 -> 0" across the nested call and the log
    // asserted that meant it had not rendered. The run's own last line said
    // otherwise -- "0 Map calls on the game context ... ZERO, the hook is not
    // installed" -- because a flat autoarm=0 run arms nothing, so the camera
    // detours were never installed and the counter was dead in both directions.
    //
    // These detours are the shipped Map/Unmap/UpdateSubresource slot swaps that
    // have run millions of calls per session without a fault, and they are
    // read-only until an eye offset is armed, which nothing here does.
    // aspect_sweep.cpp already installs them for a diagnostic on the same
    // grounds. Gated on a world being up, per the standing rule that D3D
    // detours go in in-map and not at the menu.
    // GATED ON THE CENSUS, NOT ON THE RE-ENTRY, and that is the point of this
    // build. F1d held the detours and doubled in the same run, then died in a
    // JT bitfield CAS -- so "the nested call corrupts JT state" and "holding the
    // camera detours corrupts something" are not separated by any run yet. F1b
    // doubled WITHOUT the hold and shut down clean. The missing cell is the hold
    // with NO double, and it can only be run if the hold does not depend on the
    // re-entry being wanted.
    // M1 JOINS THIS GATE RATHER THAN INVENTING ANOTHER ONE. The render-target
    // census is installed from inside the camera detour install, so it needs
    // exactly the same precondition -- a world up, detours in, detours HELD --
    // and making it depend on `jt.probe` being set would be a silent
    // dependency between two unrelated ini keys.
    if (worldReady && !g_detoursRequested &&
        (g_censusWanted.load(std::memory_order_acquire) || RtvCensusWanted())) {
        g_detoursRequested = true;
        // HOLD FIRST, THEN INSTALL. Installing alone is not enough and the last
        // run proved it: the detours went in at t=24.1 s and the armed frame at
        // t=33.4 s still read `ladder installed=0`. FinishTraceIfQuiescent tears
        // them down unless something OWNS them, and every owner it recognises is
        // a mutation -- a reader gets uninstalled on the next plugin frame.
        // camera_update_hook's own header records this being measured three
        // times as "install, four invocations, detour NOT INSTALLED, every
        // counter frozen". HoldCameraDetours arms nothing; both detours still
        // forward unchanged.
        HoldCameraDetours(true);
        const bool ok = EnsureGameCameraDetoursInstalled();
        Tf2VrLog(ok ? "[TF2VR] F1: camera detours installed AND HELD, so the witnesses survive "
                      "to the armed frame. Pass 1 is their positive control -- a zero there "
                      "voids the render question rather than answering it.\n"
                    : "[TF2VR] F1: camera detours would NOT install. The witnesses stay dead "
                      "and F1's render question CANNOT be answered this run.\n");
    }

    // M1. RETRIED EVERY FRAME UNTIL IT TAKES, rather than hung off the detour
    // install above. EnsureGameCameraDetoursInstalled short-circuits when a
    // context is already verified, so on a session where anything else got
    // there first -- the ADS probe and the aspect sweep both can, and
    // `ads.probe` is 1 in the live ini -- an install nested inside it would
    // never run and would say nothing about it. InstallRtvCensus refuses
    // itself once it is active, so this costs one predicate a frame.
    if (worldReady && RtvCensusWanted()) {
        void** vtable = nullptr;
        if (ID3D11DeviceContext* context = VerifiedGameContextForSlotSwap(&vtable)) {
            InstallRtvCensus(context, vtable);
        }
    }
    // The vertex-buffer bind census: same verified table, slot 18, read-only.
    if (worldReady) {
        void** vtable = nullptr;
        if (ID3D11DeviceContext* context = VerifiedGameContextForSlotSwap(&vtable)) {
            InstallVbBindCensus(context, vtable);
        }
    }
    // M2 builds its private target here, on the tick, and heartbeats its own
    // counters -- a substitution that declined every time must say so while the
    // run is still going, not once at the end.
    TickRtvCensus();
    // N2 RESOLVES ITS ENGINE BUILDERS ON THE TICK, NOT ON THE FIRST BURST.
    // Resolving inside the write would mean the prologue check first reports
    // seconds into a run that has already been handed over, and "the target
    // bytes differ" is exactly the answer that has to arrive BEFORE a run is
    // spent, not during one.
    TickEngineCameraWrite();

    static std::uint64_t lastTick = 0;
    const std::uint64_t now = GetTickCount64();
    if (now - lastTick < 5000) return;
    lastTick = now;

    {
        char sym[420]{};
        std::snprintf(sym, sizeof(sym),
            "[TF2VR] SYMMETRY SITE: reached %llu, armed-here %llu, PRE-WRITES PERFORMED %llu. "
            "Reached with armed-here ZERO means the toggle is not visible here; armed-here "
            "nonzero with pre-writes ZERO means the writer refused and its own decline counters "
            "name which; reached ZERO means this site never runs at all.\n",
            g_symmetricSiteReached.load(std::memory_order_relaxed),
            g_symmetricArmedHere.load(std::memory_order_relaxed),
            g_symmetricPreWrites.load(std::memory_order_relaxed));
        Tf2VrLog(sym);
    }
    {
        char ep[420]{};
        std::snprintf(ep, sizeof(ep),
            "[TF2VR] SCENE-DRAW EPILOGUE: %s, performed %llu, declined %llu. This is the "
            "singleton virtual the engine calls after EVERY scene draw and our re-entry never "
            "did. Performed climbing with the doubled-frame count is the fix RUNNING; declines "
            "climbing means it refused a guard and the run is a control, not a test.\n",
            g_epilogueWanted.load(std::memory_order_relaxed) ? "ARMED" : "OFF",
            g_epilogueCalls.load(std::memory_order_relaxed),
            g_epilogueDeclines.load(std::memory_order_relaxed));
        Tf2VrLog(ep);
    }

    char line[560]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F1 status: entries=%llu pass1returns=%llu | double attempts=%llu returns=%llu "
        "faults=%llu | declines wanted=%llu armed=%llu poisoned=%llu this=%llu caller=%llu | "
        "armed=%d poisoned=%d callers=%d unclassified=%llu\n",
        g_entries.load(std::memory_order_relaxed),
        g_pass1Returns.load(std::memory_order_relaxed),
        g_doubleAttempts.load(std::memory_order_relaxed),
        g_doubleReturns.load(std::memory_order_relaxed),
        g_doubleFaults.load(std::memory_order_relaxed),
        g_declineNotWanted.load(std::memory_order_relaxed),
        g_declineNotArmed.load(std::memory_order_relaxed),
        g_declinePoisoned.load(std::memory_order_relaxed),
        g_declineWrongThis.load(std::memory_order_relaxed),
        g_declineWrongCaller.load(std::memory_order_relaxed),
        SceneReentryArmed() ? 1 : 0,
        g_poisoned.load(std::memory_order_relaxed) ? 1 : 0,
        g_callerCount.load(std::memory_order_acquire),
        g_censusUnclassified.load(std::memory_order_relaxed));
    Tf2VrLog(line);

    // THE JOB-GROUP BASELINE, PRINTED EVERY HEARTBEAT. The control run collected
    // all of this and printed none of it, because the only report lived on the
    // armed frame -- so a run with no double produced no baseline at all, which
    // is the one thing it was best placed to give. A counter that is only
    // reported by the event it is meant to be compared against is not a
    // baseline.
    unsigned long long sfCaps = 0, sfFails = 0;
    int sfPasses = 0;
    SameFrameCaptureCounts(&sfCaps, &sfFails, &sfPasses);
    char sf[520]{};
    std::snprintf(sf, sizeof(sf),
        "[TF2VR] F1 same-frame: mid-frame eye-0 captures=%llu failures=%llu | scene-sized "
        "viewport passes on the last frame=%d. TWO passes is the boundary being found; ONE "
        "means the doubled frame never produced a second scene-sized viewport and the capture "
        "cannot have fired.\n",
        sfCaps, sfFails, sfPasses);
    Tf2VrLog(sf);

    char jt[420]{};
    std::snprintf(jt, sizeof(jt),
        "[TF2VR] F1 jobgroups: census=%d | engine passes begin=%llu end=%llu | nested passes "
        "begin=%llu end=%llu | outside begin=%llu end=%llu | DOUBLE RELEASES=%llu (last "
        "id=0x%08X pass%d)\n",
        g_jtCensusInstalled ? 1 : 0,
        g_beginCalls[1].load(std::memory_order_relaxed), g_endCalls[1].load(std::memory_order_relaxed),
        g_beginCalls[2].load(std::memory_order_relaxed), g_endCalls[2].load(std::memory_order_relaxed),
        g_beginCalls[0].load(std::memory_order_relaxed), g_endCalls[0].load(std::memory_order_relaxed),
        g_doubleReleases.load(std::memory_order_relaxed),
        g_lastDoubleReleaseId.load(std::memory_order_relaxed),
        g_lastDoubleReleaseTag.load(std::memory_order_relaxed));
    Tf2VrLog(jt);
    if (g_reentryGuard.load(std::memory_order_acquire)) {
        char gd[220]{};
        std::snprintf(gd, sizeof(gd),
            "[TF2VR] F1 guard: ON, %llu nested repeat-releases suppressed so far.\n",
            g_suppressedReleases.load(std::memory_order_relaxed));
        Tf2VrLog(gd);
    }

    const int count = g_callerCount.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i) {
        if (g_callers[i].logged) continue;
        g_callers[i].logged = true;
        char caller[200]{};
        DescribeAddress(reinterpret_cast<const void*>(g_callers[i].returnAddress), caller, sizeof(caller));
        char callerLine[440]{};
        std::snprintf(callerLine, sizeof(callerLine),
            "[TF2VR] F1 scene-draw caller %d: return=%s this=%p arg3=%u arg4=%u "
            "jtThreadIndex=%d calls=%llu\n",
            i, caller, reinterpret_cast<void*>(g_callers[i].thisPointer),
            g_callers[i].arg3, g_callers[i].arg4, g_callers[i].jtThreadIndex,
            g_callers[i].count);
        Tf2VrLog(callerLine);
    }
}

// THE WATCHDOG, AND IT RUNS ON A DIFFERENT THREAD ON PURPOSE.
//
// Continuous mode FROZE at somewhere between 128 and 256 doubled frames. The
// progress heartbeat could not report that, because it is logged from inside
// the scene draw -- the very thread that stopped. A probe driven by the thread
// that stalls cannot observe the stall; it just goes quiet, and quiet is
// indistinguishable from "the wearer pressed F6 off".
//
// This is called from the plugin's own tick. It watches the doubled-frame count
// from outside and says so when the count stops advancing while the mode is
// still armed, which is the difference between a freeze and an orderly stop.
void SceneReentryWatchdogTick() {
    if (!g_continuous.load(std::memory_order_acquire)) return;
    static unsigned long long lastSeen = 0;
    static std::uint64_t lastChange = 0;
    static bool reported = false;
    const std::uint64_t now = GetTickCount64();
    const unsigned long long n = g_continuousFrames.load(std::memory_order_relaxed);
    if (n != lastSeen) {
        lastSeen = n;
        lastChange = now;
        reported = false;
        return;
    }
    if (!lastChange) { lastChange = now; return; }
    if (!reported && now - lastChange > 2000) {
        reported = true;
        char line[420]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] *** CONTINUOUS STEREO STALLED: the doubled-frame count has been stuck at "
            "%llu for %llu ms while the mode is still ARMED. The scene-draw thread is not "
            "returning. This line comes from the plugin tick, which is a DIFFERENT thread, so "
            "its arrival is itself the evidence that the stall is in the render path and not a "
            "whole-process hang. ***\n",
            n, static_cast<unsigned long long>(now - lastChange));
        Tf2VrLog(line);
    }
}

// Slot C this build: A/B the epilogue against its own control in one run.
void ToggleSceneDrawEpilogue() {
    const bool on = !g_epilogueWanted.load(std::memory_order_acquire);
    g_epilogueWanted.store(on, std::memory_order_release);
    char line[520]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] SCENE-DRAW EPILOGUE %s (F3). %s\n", on ? "ARMED" : "OFF",
        on ? "The nested pass now runs the engine's own post-scene-draw virtual, so whatever it "
             "winds up is wound up once per SCENE DRAW rather than once per frame."
           : "THE POSITIVE CONTROL: without it the process must hang again at ~128 cumulative "
             "doubled frames. If it does NOT, this was never what was leaking.");
    Tf2VrLog(line);
}
