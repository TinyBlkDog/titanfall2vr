#include "bone_staging_probe.h"

#include "viewmodel_bones.h"
#include "diagnostics.h"
#include "scan_outcome.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <atomic>

namespace {

// All resolved statically; see the header for the derivation and the note for
// the full working.
constexpr std::uintptr_t kModelRenderSystemRva = 0x11C2FB0;
constexpr std::size_t kPoolOffset = 0x20;

// Field offsets inside the pool, taken from Init (client.dll+0x746200) and
// cross-checked against the teardown (client.dll+0x7464A0), which releases the
// same field Init allocates.
constexpr std::size_t kPoolCurrent = 0x00;
constexpr std::size_t kPoolCommittedEnd = 0x08;
constexpr std::size_t kPoolBase = 0x18;
constexpr std::size_t kPoolSize = 0x38;
constexpr std::size_t kPoolAlign = 0x40;
constexpr std::size_t kPoolCommitGranularity = 0x48;
constexpr std::size_t kPoolHighWater = 0x50;

// A bone array is a run of at least this many consecutive bone-shaped matrices.
constexpr int kMinRun = 8;
constexpr int kMaxRunsReported = 8;
// The reservation is 1 MiB by construction. Anything larger than this means the
// fields are not what we think they are, and the probe says so instead of
// walking whatever number it read.
constexpr std::uint64_t kMaxPlausibleSpan = 4ull * 1024 * 1024;

// Every read of this region is guarded. It is engine memory reached through
// offsets derived offline, and a probe that can fault is a probe that costs a
// session. PODs only in this frame, which is what lets SEH sit here.
bool GuardedReadPointer(const std::uint8_t* address, std::uintptr_t& out) {
    __try {
        out = *reinterpret_cast<const std::uintptr_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = 0;
        return false;
    }
}

bool GuardedReadBytes(const void* source, void* destination, std::size_t bytes) {
    __try {
        std::memcpy(destination, source, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsReadableRegion(const void* address, std::size_t bytes) {
    MEMORY_BASIC_INFORMATION info{};
    if (!address || !VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    return reinterpret_cast<std::uintptr_t>(address) + bytes <= start + info.RegionSize;
}

// Same predicate the entity scan uses: the 3x3 part is a rotation times a
// uniform scale, which arbitrary bytes essentially never are.
bool LooksLikeBoneMatrix(const float* m) {
    for (int i = 0; i < 12; ++i) {
        if (!std::isfinite(m[i])) return false;
    }
    float length[3]{};
    for (int row = 0; row < 3; ++row) {
        const float* r = m + row * 4;
        length[row] = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (!(length[row] > 0.001f && length[row] < 1000.0f)) return false;
        if (std::fabs(r[3]) > 1.0e6f) return false;
    }
    const float longest = std::fmax(length[0], std::fmax(length[1], length[2]));
    const float shortest = std::fmin(length[0], std::fmin(length[1], length[2]));
    if (longest > shortest * 1.05f) return false;
    const float* r0 = m; const float* r1 = m + 4; const float* r2 = m + 8;
    const float d01 = (r0[0] * r1[0] + r0[1] * r1[1] + r0[2] * r1[2]) / (length[0] * length[1]);
    const float d02 = (r0[0] * r2[0] + r0[1] * r2[1] + r0[2] * r2[2]) / (length[0] * length[2]);
    const float d12 = (r1[0] * r2[0] + r1[1] * r2[1] + r1[2] * r2[2]) / (length[1] * length[2]);
    return std::fabs(d01) < 0.03f && std::fabs(d02) < 0.03f && std::fabs(d12) < 0.03f;
}

}  // namespace

void ProbeBoneStagingBuffer() {
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) {
        Tf2VrLog("[TF2VR] staging probe: client.dll is not loaded.\n");
        return;
    }
    const auto moduleBase = reinterpret_cast<std::uint8_t*>(client);
    const std::uint8_t* pool = moduleBase + kModelRenderSystemRva + kPoolOffset;

    if (!IsReadableRegion(pool, 0x58)) {
        char line[360]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] staging probe: the pool at client.dll+0x%llX is not readable. Either the "
            "singleton has not been constructed yet or the RVA is wrong for this build. Nothing "
            "was read.\n",
            static_cast<unsigned long long>(kModelRenderSystemRva + kPoolOffset));
        Tf2VrLog(line);
        return;
    }

    std::uintptr_t current = 0, committedEnd = 0, base = 0, size = 0, align = 0, commit = 0,
                   highWater = 0;
    const bool read =
        GuardedReadPointer(pool + kPoolCurrent, current) &&
        GuardedReadPointer(pool + kPoolCommittedEnd, committedEnd) &&
        GuardedReadPointer(pool + kPoolBase, base) &&
        GuardedReadPointer(pool + kPoolSize, size) &&
        GuardedReadPointer(pool + kPoolAlign, align) &&
        GuardedReadPointer(pool + kPoolCommitGranularity, commit) &&
        GuardedReadPointer(pool + kPoolHighWater, highWater);
    if (!read) {
        Tf2VrLog("[TF2VR] staging probe: reading the pool fields faulted. Nothing was walked.\n");
        return;
    }

    char header[820]{};
    std::snprintf(header, sizeof(header),
        "[TF2VR] staging probe: CModelRenderSystem at client.dll+0x%llX, m_BoneToWorld pool at "
        "+0x%llX. base=%p current=%p committedEnd=%p size=0x%llX align=0x%llX commitGranularity="
        "0x%llX highWater=0x%llX.\n",
        static_cast<unsigned long long>(kModelRenderSystemRva),
        static_cast<unsigned long long>(kModelRenderSystemRva + kPoolOffset),
        reinterpret_cast<const void*>(base), reinterpret_cast<const void*>(current),
        reinterpret_cast<const void*>(committedEnd),
        static_cast<unsigned long long>(size), static_cast<unsigned long long>(align),
        static_cast<unsigned long long>(commit), static_cast<unsigned long long>(highWater));
    Tf2VrLog(header);

    // ---- SANITY, STATED BEFORE ANYTHING IS WALKED ----
    //
    // Every one of these is a way the offsets could be wrong for this build, and
    // each says so in its own terms rather than producing a zero that reads like
    // an absence.
    if (base == 0) {
        Tf2VrLog("[TF2VR] staging probe: base is NULL. The pool exists but has never allocated -- "
                 "either nothing has been rendered through CModelRenderSystem yet, or this is not "
                 "the field Init writes. A zero count below would say nothing.\n");
        return;
    }
    if (align != 0x20 || size == 0) {
        char line[420]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] staging probe WARNING: alignment reads 0x%llX where the constructor passes "
            "0x20, and size reads 0x%llX. The offsets may not be right for this build; treat "
            "everything below as unattributable.\n",
            static_cast<unsigned long long>(align), static_cast<unsigned long long>(size));
        Tf2VrLog(line);
    }
    if (current < base) {
        Tf2VrLog("[TF2VR] staging probe: the bump pointer is BELOW the base, so these are not the "
                 "two fields we think they are. Nothing was walked.\n");
        return;
    }

    std::uint64_t span = static_cast<std::uint64_t>(current - base);
    ScanOutcome outcome;
    if (span > kMaxPlausibleSpan) {
        char line[400]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] staging probe: [base, current) spans 0x%llX bytes, far past the 1 MiB the "
            "constructor reserves. Clamping the walk to 0x%llX and reporting it as truncated -- a "
            "count from a span this wrong means nothing on its own.\n",
            static_cast<unsigned long long>(span),
            static_cast<unsigned long long>(kMaxPlausibleSpan));
        Tf2VrLog(line);
        span = kMaxPlausibleSpan;
        outcome.Finish(ScanOutcome::End::ByteCap);
    }
    if (span < 48) {
        char line[360]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] staging probe: the pool is live but EMPTY (%llu bytes allocated). Nothing has "
            "been staged into it at this instant -- which is itself informative if the viewmodel is "
            "on screen.\n", static_cast<unsigned long long>(span));
        Tf2VrLog(line);
        return;
    }

    // ---- WALK [base, current) ----
    const auto* region = reinterpret_cast<const std::uint8_t*>(base);
    int runsFound = 0;
    int runsReported = 0;
    std::uint64_t offset = 0;
    while (offset + 48ull * kMinRun <= span) {
        if (!IsReadableRegion(region + offset, 48ull * kMinRun)) { offset += 16; continue; }
        float probe[12]{};
        if (!GuardedReadBytes(region + offset, probe, sizeof(probe))) { offset += 16; continue; }
        if (!LooksLikeBoneMatrix(probe)) { offset += 16; continue; }
        int run = 0;
        while (offset + 48ull * (run + 1) <= span &&
               IsReadableRegion(region + offset + 48ull * run, 48)) {
            float next[12]{};
            if (!GuardedReadBytes(region + offset + 48ull * run, next, sizeof(next))) break;
            if (!LooksLikeBoneMatrix(next)) break;
            ++run;
        }
        if (run < kMinRun) { offset += 16; continue; }
        ++runsFound;
        if (runsReported < kMaxRunsReported) {
            ++runsReported;
            char line[520]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR]   staged run: +0x%06llX, %d consecutive bone matrices, first translation "
                "(%.2f %.2f %.2f).\n",
                static_cast<unsigned long long>(offset), run,
                static_cast<double>(probe[3]), static_cast<double>(probe[7]),
                static_cast<double>(probe[11]));
            Tf2VrLog(line);
        }
        offset += static_cast<std::uint64_t>(run) * 48;
    }
    outcome.Finish(ScanOutcome::End::Complete);

    char summary[620]{};
    std::snprintf(summary, sizeof(summary),
        "[TF2VR] staging probe: walked 0x%llX bytes of staged data, %d run(s) of %d+ bone matrices, "
        "%d reported. %s\n",
        static_cast<unsigned long long>(span), runsFound, kMinRun, runsReported,
        outcome.Describe());
    Tf2VrLog(summary);

    // ---- THE CROSS-CHECK THAT DECIDES WHETHER THIS IS THE RIGHT BUFFER ----
    //
    // If this region stages the arrays we have been driving, their matrices are
    // in here byte for byte. A hit means the entity-inline array and this buffer
    // hold the same pose, so writing here is writing what the renderer reads. A
    // miss, with runs present, means this buffer holds SOMETHING ELSE's bones --
    // which is equally worth knowing and would rule the fallback out rather than
    // leaving it as another maybe.
    for (int slot = 0; slot < 8; ++slot) {
        const void* array = nullptr;
        int boneCount = 0;
        const char* className = nullptr;
        if (!GetWeaponBoneTargetArray(slot, &array, &boneCount, &className)) continue;
        float wanted[12]{};
        if (!IsReadableRegion(array, sizeof(wanted)) ||
            !GuardedReadBytes(array, wanted, sizeof(wanted))) {
            continue;
        }
        bool matched = false;
        std::uint64_t matchOffset = 0;
        for (std::uint64_t at = 0; at + 48 <= span; at += 16) {
            if (!IsReadableRegion(region + at, 48)) continue;
            float candidate[12]{};
            if (!GuardedReadBytes(region + at, candidate, sizeof(candidate))) continue;
            if (std::memcmp(candidate, wanted, sizeof(wanted)) == 0) {
                matched = true;
                matchOffset = at;
                break;
            }
        }
        char line[560]{};
        if (matched) {
            std::snprintf(line, sizeof(line),
                "[TF2VR]   CROSS-CHECK [%d] %s/%d bones: its bone 0 appears in the staged buffer at "
                "+0x%06llX. This region stages that array, so writing HERE is writing what the "
                "renderer reads.\n",
                slot, className, boneCount, static_cast<unsigned long long>(matchOffset));
        } else {
            std::snprintf(line, sizeof(line),
                "[TF2VR]   CROSS-CHECK [%d] %s/%d bones: its bone 0 is NOT in the staged buffer. "
                "Either this region stages something else's bones, or it is staged at a different "
                "instant than this probe reads.\n",
                slot, className, boneCount);
        }
        Tf2VrLog(line);
    }
}



namespace {

// SNAPSHOT ON SIGHT, AND KEEP LOOKING.
//
// The first honest sample caught the pool populated on 1 of 89 viewmodel passes,
// holding exactly ONE run: 129 consecutive bone matrices at offset 0, first
// translation (1248.84 -6993.06 1688.34). Neither known array's bone 0 was in
// it. That is not a failure -- it is the allocator working as designed. It
// stages ONE model's bones, that model draws, and it rewinds. 129 bones is a
// full character skeleton, so what we caught was a world model, not the
// viewmodel.
//
// So the question is no longer "does this buffer hold bones" -- it does -- but
// "which models pass through it, and is the gun one of them". That needs the
// sampler to stop after the first hit and start CATALOGUING instead: every
// populated window it catches, deduplicated by bone count and position, kept
// until the session ends.
//
// The gun is identifiable in such a catalogue two ways at once. The VPKs ship
// ~60 ptpov_* weapon models, so its bone count should be far below a character's
// 129; and its bones are in world space at the player's hands, so its first
// translation should sit near the known arrays' -- which are also in world space
// and give us the reference for free.
constexpr std::size_t kSnapshotBytes = 256 * 1024;
std::uint8_t g_snapshot[kSnapshotBytes];

constexpr int kMaxCatalogue = 24;
struct StagedModel {
    int boneCount;
    float translation[3];
    std::uint64_t timesSeen;
};
StagedModel g_catalogue[kMaxCatalogue]{};
int g_catalogueCount = 0;

std::atomic_bool g_sampling = false;
std::atomic_uint64_t g_passesSampled = 0;
std::atomic_uint64_t g_passesPopulated = 0;
std::atomic_uint64_t g_maxSpanSeen = 0;
std::atomic_uint64_t g_runsSeen = 0;
std::atomic_uint64_t g_passesPopulatedRaw = 0;
std::atomic_uint64_t g_liveSpan = 0;
// One sampler at a time, and not more often than every 50 ms. The camera hook is
// entered from more than one thread, and the previous version let all of them
// copy into the same buffer and append to the same catalogue at once.
std::atomic_bool g_sampleInProgress = false;
std::atomic_uint64_t g_lastSampleTick = 0;

// Same model twice is the common case -- it is staged once per draw, every
// frame. Position is rounded so a model that is merely animating does not enter
// the catalogue once per frame.
void RecordStagedRun(int boneCount, const float* first) {
    for (int i = 0; i < g_catalogueCount; ++i) {
        if (g_catalogue[i].boneCount != boneCount) continue;
        if (std::fabs(g_catalogue[i].translation[0] - first[3]) > 8.0f) continue;
        if (std::fabs(g_catalogue[i].translation[1] - first[7]) > 8.0f) continue;
        if (std::fabs(g_catalogue[i].translation[2] - first[11]) > 8.0f) continue;
        ++g_catalogue[i].timesSeen;
        return;
    }
    if (g_catalogueCount < 0 || g_catalogueCount >= kMaxCatalogue) return;
    g_catalogue[g_catalogueCount].boneCount = boneCount;
    g_catalogue[g_catalogueCount].translation[0] = first[3];
    g_catalogue[g_catalogueCount].translation[1] = first[7];
    g_catalogue[g_catalogueCount].translation[2] = first[11];
    g_catalogue[g_catalogueCount].timesSeen = 1;
    ++g_catalogueCount;
}

void ScanSnapshot(std::uint64_t span) {
    std::uint64_t offset = 0;
    while (offset + 48ull * kMinRun <= span) {
        const auto* candidate = reinterpret_cast<const float*>(g_snapshot + offset);
        if (!LooksLikeBoneMatrix(candidate)) { offset += 16; continue; }
        int run = 0;
        while (offset + 48ull * (run + 1) <= span &&
               LooksLikeBoneMatrix(reinterpret_cast<const float*>(g_snapshot + offset + 48ull * run))) {
            ++run;
        }
        if (run < kMinRun) { offset += 16; continue; }
        g_runsSeen.fetch_add(1, std::memory_order_relaxed);
        RecordStagedRun(run, candidate);
        offset += static_cast<std::uint64_t>(run) * 48;
    }
}

}  // namespace

void RequestBoneStagingSample() {
    g_catalogueCount = 0;
    g_passesSampled.store(0, std::memory_order_release);
    g_passesPopulated.store(0, std::memory_order_release);
    g_maxSpanSeen.store(0, std::memory_order_release);
    g_runsSeen.store(0, std::memory_order_release);
    g_passesPopulatedRaw.store(0, std::memory_order_release);
    g_liveSpan.store(0, std::memory_order_release);
    g_sampleInProgress.store(false, std::memory_order_release);
    g_lastSampleTick.store(0, std::memory_order_release);
    g_sampling.store(true, std::memory_order_release);
    Tf2VrLog("[TF2VR] staging probe: cataloguing every model staged through "
             "CModelRenderSystem::m_BoneToWorld for the rest of the session. It snapshots the bytes "
             "the instant the pool is non-empty and reports from the copy.\n");
}

// HOW MANY BYTES ARE ACTUALLY READABLE FROM HERE, ASKED OF THE OS.
//
// This is the fix for the second crash I caused. The reservation is made with
// VirtualAlloc(..., MEM_RESERVE, PAGE_NOACCESS) -- r9d is 1 at that call site --
// and pages are committed to read/write only as the allocator grows into them.
// So committedEnd is the ALLOCATOR'S bookkeeping, not a promise from the OS, and
// [base, committedEnd) can span pages that are reserved and unreadable. Copying
// across that boundary is an access violation, and leaning on an SEH guard to
// absorb one on a hot render thread was not good enough: it fired anyway.
//
// Ask the OS instead. VirtualQuery reports the contiguous committed, readable
// run starting at base, and the copy is clamped to it. Nothing is assumed.
std::size_t ReadableSpanFrom(const void* address, std::size_t wanted) {
    std::size_t total = 0;
    const auto* cursor = static_cast<const std::uint8_t*>(address);
    while (total < wanted) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(cursor + total, &info, sizeof(info))) break;
        if (info.State != MEM_COMMIT) break;
        const DWORD protect = info.Protect & 0xFF;
        if (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) break;
        if (protect == PAGE_NOACCESS || protect == PAGE_EXECUTE) break;
        const auto regionStart = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const auto cursorAt = reinterpret_cast<std::uintptr_t>(cursor + total);
        const std::size_t availableHere =
            static_cast<std::size_t>(regionStart + info.RegionSize - cursorAt);
        if (availableHere == 0) break;
        total += availableHere;
    }
    return total < wanted ? total : wanted;
}

void AdvanceBoneStagingSampleAtRender() {
    if (!g_sampling.load(std::memory_order_acquire)) return;

    // RATE-LIMITED AND SINGLE-THREADED.
    //
    // The previous version ran a 32 KB copy and a full bone-run scan on EVERY
    // camera-sized upload -- about 150 a frame -- from whatever thread got
    // there, all of them writing the same snapshot buffer and the same
    // catalogue. That is roughly 1.5 GB of copying and 100 million predicate
    // evaluations over a session, unsynchronised, inside a render hook. One
    // sample every 50 ms accumulates 500 samples across a 25-second session,
    // which is ample, and costs about 0.3% of what that did.
    const std::uint64_t now = GetTickCount64();
    if (now - g_lastSampleTick.load(std::memory_order_acquire) < 50) return;
    bool expected = false;
    if (!g_sampleInProgress.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;   // another thread is inside; one sampler at a time
    }
    g_lastSampleTick.store(now, std::memory_order_release);
    struct ReleaseOnExit {
        ~ReleaseOnExit() { g_sampleInProgress.store(false, std::memory_order_release); }
    } releaseOnExit;

    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return;
    const auto* pool = reinterpret_cast<const std::uint8_t*>(client) + kModelRenderSystemRva +
                       kPoolOffset;
    if (!IsReadableRegion(pool, 0x58)) return;
    std::uintptr_t base = 0, current = 0, committedEnd = 0;
    if (!GuardedReadPointer(pool + kPoolBase, base)) return;
    if (!GuardedReadPointer(pool + kPoolCurrent, current)) return;
    if (!GuardedReadPointer(pool + kPoolCommittedEnd, committedEnd)) return;
    g_passesSampled.fetch_add(1, std::memory_order_relaxed);
    if (base == 0 || committedEnd <= base) return;
    if (current > base) g_passesPopulatedRaw.fetch_add(1, std::memory_order_relaxed);

    // READ THE WHOLE COMMITTED REGION, NOT JUST THE LIVE PART.
    //
    // Sampling [base, current) caught the window open on 205 of 48279 passes and
    // saw exactly ONE model every single time -- 129 bones at a fixed world
    // position 1377 units from the player's hands. One model, 205 times, is not
    // an enumeration; it is a phase lock. The camera uploads we sample from land
    // at a fixed point in the draw order, so they always catch the same draw.
    //
    // A bump allocator rewinds without clearing. Everything staged since the
    // pages were committed is still lying in [base, committedEnd) as residue, so
    // reading the committed region reads recent HISTORY instead of one instant,
    // and no longer depends on catching a 0.4% window at the right moment. It is
    // 32 KB, so it is cheap enough to do on every sample.
    std::uint64_t span = static_cast<std::uint64_t>(committedEnd - base);
    g_liveSpan.store(current > base ? static_cast<std::uint64_t>(current - base) : 0,
                     std::memory_order_release);
    if (span > g_maxSpanSeen.load(std::memory_order_acquire)) {
        g_maxSpanSeen.store(span, std::memory_order_release);
    }
    if (span > kSnapshotBytes) span = kSnapshotBytes;
    // Clamped to what the OS says is actually there, not to what the allocator's
    // own bookkeeping claims.
    span = ReadableSpanFrom(reinterpret_cast<const void*>(base), static_cast<std::size_t>(span));
    if (span < 48ull * kMinRun) return;
    g_passesPopulated.fetch_add(1, std::memory_order_relaxed);
    // COPY FIRST, ASK QUESTIONS AFTER. The window is narrower than the gap
    // between two reads of the same field, so anything that re-reads is
    // comparing two different states.
    if (!GuardedReadBytes(reinterpret_cast<const void*>(base), g_snapshot,
                          static_cast<std::size_t>(span))) {
        return;   // the window shut mid-copy; the next sample tries again
    }
    ScanSnapshot(span);
}

void ReportBoneStagingCollection() {
    if (!g_sampling.load(std::memory_order_acquire)) return;
    g_sampling.store(false, std::memory_order_release);

    char header[620]{};
    std::snprintf(header, sizeof(header),
        "[TF2VR] ==== STAGED MODEL CATALOGUE ====  %llu passes sampled, committed region read on "
        "%llu of them (%llu caught with a LIVE allocation in flight), largest committed span "
        "0x%llX bytes, %llu bone runs seen, %d distinct model(s). The region is read whole, so "
        "these are every model staged recently, not only the one in flight when we looked.\n",
        static_cast<unsigned long long>(g_passesSampled.load()),
        static_cast<unsigned long long>(g_passesPopulated.load()),
        static_cast<unsigned long long>(g_passesPopulatedRaw.load()),
        static_cast<unsigned long long>(g_maxSpanSeen.load()),
        static_cast<unsigned long long>(g_runsSeen.load()), g_catalogueCount);
    Tf2VrLog(header);

    // The known arrays are in world space too, so they hand us the player's
    // position for nothing. Anything staged within a couple of metres of it is
    // in the player's hands, and the weapon is the thing in the player's hands
    // that is not the arms.
    float reference[3]{};
    bool haveReference = false;
    for (int slot = 0; slot < 8 && !haveReference; ++slot) {
        const void* array = nullptr;
        int boneCount = 0;
        const char* className = nullptr;
        if (!GetWeaponBoneTargetArray(slot, &array, &boneCount, &className)) continue;
        float bone0[12]{};
        if (!IsReadableRegion(array, sizeof(bone0))) continue;
        if (!GuardedReadBytes(array, bone0, sizeof(bone0))) continue;
        reference[0] = bone0[3]; reference[1] = bone0[7]; reference[2] = bone0[11];
        haveReference = true;
        char line[380]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR]   reference: %s/%d bones sits at (%.1f %.1f %.1f) -- that is where the player's "
            "hands are.\n", className, boneCount,
            static_cast<double>(reference[0]), static_cast<double>(reference[1]),
            static_cast<double>(reference[2]));
        Tf2VrLog(line);
    }

    if (g_catalogueCount == 0) {
        Tf2VrLog("[TF2VR]   nothing was ever caught staged. Read the pass counts above before "
                 "treating that as an absence: if 'caught populated' is 0 the window was simply "
                 "never open when we looked, which says nothing about what goes through it.\n");
        return;
    }

    // Belt and braces on the bound: the append is single-threaded now, but this
    // array is read from the reporting path and indexed by a plain int, and an
    // out-of-range read here would be the third crash in this file.
    const int catalogued = g_catalogueCount > kMaxCatalogue ? kMaxCatalogue : g_catalogueCount;
    for (int i = 0; i < catalogued; ++i) {
        const StagedModel& model = g_catalogue[i];
        double distance = -1.0;
        if (haveReference) {
            const double dx = model.translation[0] - reference[0];
            const double dy = model.translation[1] - reference[1];
            const double dz = model.translation[2] - reference[2];
            distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        char line[520]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR]   staged model: %3d bones at (%.1f %.1f %.1f), seen %llu time(s)%s%s\n",
            model.boneCount,
            static_cast<double>(model.translation[0]), static_cast<double>(model.translation[1]),
            static_cast<double>(model.translation[2]),
            static_cast<unsigned long long>(model.timesSeen),
            distance >= 0.0 ? "" : ".",
            distance >= 0.0
                ? (distance < 250.0 ? "  <-- IN THE PLAYER'S HANDS. A weapon-sized bone count here "
                                      "is the gun's staged copy."
                                    : ".")
                : "");
        Tf2VrLog(line);
        if (distance >= 0.0 && distance >= 250.0) {
            char extra[200]{};
            std::snprintf(extra, sizeof(extra), "[TF2VR]       (%.0f units from the hands)\n",
                          distance);
            Tf2VrLog(extra);
        }
    }
}
