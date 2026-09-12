#include "viewmodel_bones.h"

#include "aim_probe.h"
#include "bone_staging_probe.h"
#include "diagnostics.h"
#include "present_hook.h"
#include "scan_outcome.h"
#include "view_build_hook.h"
#include "xr_input.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

// THE BYTE THE VIEW-BUILD DETOUR TESTS.
//
// view_build_hook.asm compares this against zero at the top of
// clientViewBuildInterceptor and calls into the pin only when it is set, so a
// disarmed plugin leaves that seam the exact pass-through it has always been.
// Set only after the detour is confirmed installed -- an armed write against a
// seam that never got patched would look precisely like a dead write, and that
// is the class of null this whole step exists to abolish.
extern "C" volatile std::uint8_t g_bonePinViewBuildArmed = 0;

namespace {

// Resolved offline by walking client.dll's RTTI: the type name
// ".?AVC_ViewmodelAttachmentModel@@" at file offset 0xAF4198 gives type
// descriptor RVA 0xAF5388, whose Complete Object Locator at object offset 0
// yields this vtable. Four secondary vtables exist at object offsets 16/24/32/40
// (0x8B88D8, 0x8B89E8, 0x8B8AA0, 0x8B8AD0) -- the multiple-inheritance shape of a
// Source client entity, which is the cross-check that this is the right class and
// not a coincidence.
// THE CLASSES, ALL RESOLVED THE SAME WAY, AND WHY MORE THAN ONE.
//
// The first scan found five C_ViewmodelAttachmentModel instances -- so the RTTI
// anchor works and the method is sound -- but no bone array on any of them. The
// likely reason is that an ATTACHMENT model is the weapon part hanging off the
// viewmodel, and the skeleton that matters (the hands and arms) belongs to the
// PARENT. BioShock's approach is explicitly to move the hand cluster, so the
// parent is the object to find.
//
// Every one of these was resolved offline by walking client.dll's RTTI, and every
// one produced the same five-vtable shape at object offsets 0/16/24/32/40 -- the
// multiple-inheritance layout of a Source client entity. That consistency across
// four independent classes is the cross-check that the resolution method is right
// rather than four coincidences.
struct EntityClass {
    const char* name;
    std::uintptr_t vtableRva;
    const char* why;
};
constexpr EntityClass kEntityClasses[] = {
    {"C_BaseViewModel", 0x8A86C8, "the viewmodel itself -- the hands and arms, and the most likely "
                                  "owner of the skeleton we need"},
    {"C_ViewmodelAttachmentModel", 0x8B8158, "the weapon parts attached to it; five were found last "
                                             "run, with no bones of their own"},
    {"C_WeaponX", 0x998638, "the weapon entity"},
    {"C_BaseAnimating", 0x8AED48, "exact matches only, so this finds direct instances rather than "
                                  "any derived entity -- a zero here is expected and not a failure"},
};

// THE CAP THAT MADE THIS A LOTTERY.
//
// At 512 MiB, two runs of identical code disagreed: one found five
// C_ViewmodelAttachmentModel instances after 524.8 MiB, the next found zero after
// 514 MiB. BOTH terminated on the byte cap rather than on running out of address
// space, so neither scan was ever exhaustive -- each walked whatever regions came
// first and stopped, and whether the instances fell inside that prefix was decided
// by heap layout.
//
// A diagnostic whose answer depends on allocation order is worse than no
// diagnostic, because it produces confident contradictions. The cap is now high
// enough to cover this process's committed private data several times over, and
// -- more importantly -- whether it was reached is REPORTED, so a truncated scan
// can never again be read as an absence.
constexpr std::size_t kMaxBytesScanned = 8ull * 1024 * 1024 * 1024;
constexpr std::size_t kMaxRegionBytes = 64ull * 1024 * 1024;
constexpr int kMaxInstances = 16;
constexpr std::size_t kInstanceScanBytes = 0x4000;   // how far into an object to look
constexpr int kMaxCandidatesReported = 6;
// Widened from 0x600. The first scan searched only the head of each object, and
// "no pointer found" and "the pointer is further in" are indistinguishable from
// the outside -- so the range is now generous and the depth is two.
constexpr std::size_t kPointerScanBytes = 0x1000;
// A bone array is a run of at least this many consecutive orthonormal matrices.
// Three is already far beyond what random bytes produce; the run length is also
// reported so a longer one can be preferred.
constexpr int kMinBoneRun = 3;

std::atomic_bool g_requested = false;
std::atomic_bool g_done = false;

bool IsScannableRegion(const MEMORY_BASIC_INFORMATION& info) {
    if (info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD protect = info.Protect & 0xFF;
    // Read-write data only. Entities live in the heap; skipping executable and
    // read-only regions removes most of the address space at no cost to the
    // search.
    if (protect != PAGE_READWRITE) return false;
    if (info.Type != MEM_PRIVATE) return false;
    return info.RegionSize <= kMaxRegionBytes;
}

// A matrix3x4_t in Source is 12 floats, row-major: three rows of [basis xyz,
// translation]. The test is that the 3x3 part is orthonormal, which a rotation
// always is and arbitrary bytes essentially never are.
bool LooksLikeBoneMatrix(const float* m) {
    for (int i = 0; i < 12; ++i) {
        if (!std::isfinite(m[i])) return false;
    }
    // SCALE IS ALLOWED, and demanding unit length was probably why the first scan
    // found nothing. Bone-to-world matrices routinely carry a uniform scale, so
    // the invariant is not "rows are unit length" but "rows are the SAME length
    // and mutually perpendicular" -- which is what a rotation-times-scale is, and
    // still something random heap bytes essentially never are.
    float length[3]{};
    for (int row = 0; row < 3; ++row) {
        const float* r = m + row * 4;
        length[row] = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (!(length[row] > 0.001f && length[row] < 1000.0f)) return false;
        // Translation in a plausible world range. Bones can be far from the origin
        // on a large map, but not astronomically so, and this rejects float noise.
        if (std::fabs(r[3]) > 1.0e6f) return false;
    }
    const float longest = std::fmax(length[0], std::fmax(length[1], length[2]));
    const float shortest = std::fmin(length[0], std::fmin(length[1], length[2]));
    if (longest > shortest * 1.05f) return false;   // non-uniform: not a bone transform
    // Perpendicular after normalisation. Equal lengths alone are satisfied by
    // three copies of one vector, which is not a rotation.
    const float* r0 = m; const float* r1 = m + 4; const float* r2 = m + 8;
    const float d01 = (r0[0] * r1[0] + r0[1] * r1[1] + r0[2] * r1[2]) / (length[0] * length[1]);
    const float d02 = (r0[0] * r2[0] + r0[1] * r2[1] + r0[2] * r2[2]) / (length[0] * length[2]);
    const float d12 = (r1[0] * r2[0] + r1[1] * r2[1] + r1[2] * r2[2]) / (length[1] * length[2]);
    return std::fabs(d01) < 0.03f && std::fabs(d02) < 0.03f && std::fabs(d12) < 0.03f;
}

bool IsReadable(const void* address, std::size_t bytes) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    return reinterpret_cast<std::uintptr_t>(address) + bytes <= start + info.RegionSize;
}

// Looks through one instance for runs of bone matrices, reporting the longest few.
void ReportBoneCandidates(const std::uint8_t* instance, int instanceIndex) {
    int reported = 0;
    std::size_t offset = 0;
    while (offset + 48 <= kInstanceScanBytes && reported < kMaxCandidatesReported) {
        const auto* candidate = reinterpret_cast<const float*>(instance + offset);
        if (!IsReadable(candidate, 48) || !LooksLikeBoneMatrix(candidate)) {
            offset += 4;
            continue;
        }
        // Measure the run.
        int run = 0;
        std::size_t runOffset = offset;
        while (runOffset + 48 <= kInstanceScanBytes) {
            const auto* next = reinterpret_cast<const float*>(instance + runOffset);
            if (!IsReadable(next, 48) || !LooksLikeBoneMatrix(next)) break;
            ++run;
            runOffset += 48;
        }
        if (run >= kMinBoneRun) {
            ++reported;
            char line[520]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] bone probe: instance %d, offset +0x%04zX -- run of %d consecutive "
                "orthonormal matrix3x4 (%d bones' worth). First translation (%.2f %.2f %.2f).\n",
                instanceIndex, offset, run, run,
                static_cast<double>(candidate[3]), static_cast<double>(candidate[7]),
                static_cast<double>(candidate[11]));
            Tf2VrLog(line);
        }
        offset = runOffset > offset ? runOffset : offset + 4;
    }
    if (reported == 0) {
        char line[300]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] bone probe: instance %d had no run of %d orthonormal matrices in its first "
            "0x%zX bytes. The array is probably reached through a POINTER rather than stored inline, "
            "which is the usual Source layout.\n", instanceIndex, kMinBoneRun, kInstanceScanBytes);
        Tf2VrLog(line);
    }
}

// Follows pointer fields in the instance, since Source normally stores the bone
// array out of line and keeps a pointer to it.
// Two levels of indirection, because Source commonly reaches bones through an
// accessor object (entity -> CBoneAccessor -> matrix3x4_t*) rather than holding
// the array pointer directly.
int ReportPointedBoneCandidates(const std::uint8_t* instance, int instanceIndex,
                                const char* className, int depth) {
    int reported = 0;
    for (std::size_t offset = 0; offset + 8 <= kPointerScanBytes && reported < kMaxCandidatesReported;
         offset += 8) {
        if (!IsReadable(instance + offset, 8)) continue;
        const auto target = *reinterpret_cast<const std::uint8_t* const*>(instance + offset);
        if (!target || !IsReadable(target, 48 * kMinBoneRun)) continue;
        int run = 0;
        while (run < 512 && IsReadable(target + run * 48, 48) &&
               LooksLikeBoneMatrix(reinterpret_cast<const float*>(target + run * 48))) {
            ++run;
        }
        if (run >= kMinBoneRun) {
            ++reported;
            const auto* first = reinterpret_cast<const float*>(target);
            char line[620]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] bone probe: %s instance %d, pointer at +0x%03zX (depth %d) -> %p, run of %d "
                "bone-shaped matrix3x4. First translation (%.2f %.2f %.2f), row scale %.3f. A pointer "
                "to an out-of-line run of these IS what a Source bone-to-world array looks like.\n",
                className, instanceIndex, offset, depth, static_cast<const void*>(target), run,
                static_cast<double>(first[3]), static_cast<double>(first[7]),
                static_cast<double>(first[11]),
                static_cast<double>(std::sqrt(first[0] * first[0] + first[1] * first[1] +
                                              first[2] * first[2])));
            Tf2VrLog(line);
            continue;
        }
        // Not an array itself -- but it might be the accessor that holds one.
        if (depth < 1 && IsReadable(target, 0x40)) {
            reported += ReportPointedBoneCandidates(target, instanceIndex, className, depth + 1);
        }
    }
    return reported;
}

constexpr int kClassCount = sizeof(kEntityClasses) / sizeof(kEntityClasses[0]);

void RunProbe() {
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) {
        Tf2VrLog("[TF2VR] bone probe: client.dll is not loaded.\n");
        return;
    }
    Tf2VrLog("[TF2VR] bone probe: one exhaustive walk, matching every class at once. A vtable match "
             "is a POSITIVE identification rather than a heuristic.\n");

    std::uintptr_t vtables[kClassCount]{};
    const std::uint8_t* instances[kClassCount][kMaxInstances]{};
    int instanceCount[kClassCount]{};
    for (int index = 0; index < kClassCount; ++index) {
        vtables[index] = reinterpret_cast<std::uintptr_t>(client) + kEntityClasses[index].vtableRva;
    }

    // ONE WALK FOR ALL FOUR, not four walks. Separate passes each restarted from
    // the bottom of the address space and each stopped at the same cap, so all
    // four saw the same truncated prefix -- and, being separate, they could also
    // have disagreed with each other about a heap that moves between them.
    std::size_t bytesScanned = 0;
    unsigned regionsVisited = 0;
    ScanOutcome outcome;

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    auto* address = static_cast<const std::uint8_t*>(systemInfo.lpMinimumApplicationAddress);
    const auto* limit = static_cast<const std::uint8_t*>(systemInfo.lpMaximumApplicationAddress);

    for (;;) {
        if (address >= limit) { outcome.Finish(ScanOutcome::End::Complete); break; }
        if (bytesScanned >= kMaxBytesScanned) { outcome.Finish(ScanOutcome::End::ByteCap); break; }
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(address, &info, sizeof(info))) {
            outcome.Finish(ScanOutcome::End::WalkFailed);
            break;
        }
        const auto* base = static_cast<const std::uint8_t*>(info.BaseAddress);
        const std::size_t size = info.RegionSize;
        if (IsScannableRegion(info)) {
            ++regionsVisited;
            bytesScanned += size;
            // Objects are pointer-aligned, so step by 8 rather than by 1.
            for (std::size_t offset = 0; offset + 8 <= size; offset += 8) {
                const auto value = *reinterpret_cast<const std::uintptr_t*>(base + offset);
                for (int index = 0; index < kClassCount; ++index) {
                    if (value != vtables[index]) continue;
                    if (instanceCount[index] < kMaxInstances) {
                        instances[index][instanceCount[index]++] = base + offset;
                    } else {
                        // A dropped match makes the result incomplete even if the
                        // walk itself later reaches the end of the address space.
                        outcome.Finish(ScanOutcome::End::InstanceCap);
                    }
                    break;
                }
            }
        }
        if (size == 0) { outcome.Finish(ScanOutcome::End::WalkFailed); break; }
        address = base + size;
    }

    char coverage[620]{};
    std::snprintf(coverage, sizeof(coverage),
        "[TF2VR] bone probe: walked %u regions, %.0f MB. %s. THIS LINE DECIDES WHETHER A ZERO "
        "BELOW MEANS ANYTHING. Two earlier runs of this probe disagreed -- five instances then "
        "none -- because both stopped at a 512 MB cap and each saw a different prefix of a moving "
        "heap.\n",
        regionsVisited, static_cast<double>(bytesScanned) / (1024.0 * 1024.0),
        outcome.Describe());
    Tf2VrLog(coverage);

    for (int index = 0; index < kClassCount; ++index) {
        char summary[520]{};
        std::snprintf(summary, sizeof(summary),
            "[TF2VR] bone probe: %-28s client.dll+0x%06llX -> %d instance(s). (%s)\n",
            kEntityClasses[index].name,
            static_cast<unsigned long long>(kEntityClasses[index].vtableRva),
            instanceCount[index], kEntityClasses[index].why);
        Tf2VrLog(summary);

        int candidates = 0;
        for (int slot = 0; slot < instanceCount[index]; ++slot) {
            candidates += ReportPointedBoneCandidates(instances[index][slot], slot,
                                                      kEntityClasses[index].name, 0);
            ReportBoneCandidates(instances[index][slot], slot);
        }
        if (instanceCount[index] > 0 && candidates == 0) {
            char line[540]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] bone probe: %s -- instances exist, and NO bone-shaped array is reachable "
                "through any pointer in their first 0x%zX bytes at up to two levels. The array is "
                "further in, behind more indirection, or not laid out as matrix3x4.\n",
                kEntityClasses[index].name, kPointerScanBytes);
            Tf2VrLog(line);
        }
    }
    Tf2VrLog("[TF2VR] bone probe: done. C_BaseViewModel is the one that matters -- it is the hands "
             "and arms, and the attachment models hang off it.\n");
}

}  // namespace

void RequestBoneProbe() {
    g_requested.store(true, std::memory_order_release);
    g_done.store(false, std::memory_order_release);
}

void AdvanceBoneProbe() {
    if (!g_requested.load(std::memory_order_acquire)) return;
    if (g_done.load(std::memory_order_acquire)) return;
    g_done.store(true, std::memory_order_release);
    g_requested.store(false, std::memory_order_release);
    RunProbe();
}

// ===========================================================================
// TASK 05 STEP 4 -- THE BONE WRITE
// ===========================================================================

namespace {

// Measured by the scan above, and cross-checked between two classes that
// independently reported the same world position for their first bone.
constexpr std::size_t kAttachmentBoneOffset = 0x1260;
constexpr int kAttachmentBoneCount = 74;
constexpr std::uintptr_t kAttachmentVtableRva = 0x8B8158;

// EVERY SKELETON WE CAN REACH, CYCLED IN THE HEADSET.
//
// Reported from the first run: driving the attachment's 74 bones moved the entire
// BODY and the gun did not move at all. So the write mechanism is right -- seam,
// arithmetic and pivot all demonstrably move a real skeleton in real time -- and
// the target is wrong. Naming does not settle which array is which, and it has
// already misled once, so both classes and both instances are enumerated and
// stepped through by key until the GUN is what moves.
constexpr std::size_t kViewmodelBoneOffset = 0x1870;
constexpr int kViewmodelBoneCount = 87;
constexpr std::uintptr_t kViewmodelVtableRva = 0x8A86C8;

struct BoneTarget {
    const std::uint8_t* instance;   // the bone array itself
    // The OBJECT that owns the array, kept so its vtable can be re-checked on
    // every pass. The array pointer alone cannot be validated: freed heap stays
    // committed, so a readability test passes long after the entity is gone and
    // the write lands in whatever was allocated there next.
    const std::uint8_t* owner;
    std::uintptr_t vtableRva;
    std::size_t boneOffset;
    int boneCount;
    const char* className;
    // Set after a write into this target faulted. Nothing touches it again.
    bool retired;
};
constexpr int kMaxBoneTargets = 8;
// Index meaning "drive every discovered array at once".
constexpr int kBoneTargetAll = -1;
BoneTarget g_boneTargets[kMaxBoneTargets]{};
std::atomic_int g_boneTargetCount = 0;
std::atomic_int g_boneTargetIndex = 0;

std::atomic_bool g_bonePinEnabled = false;
// Armed but not writing. Distinct from disarmed: the targets, the scan and the
// chosen drive all survive a pause, so a still beat costs nothing to re-enter.
std::atomic_bool g_bonePinPaused = false;
std::atomic_int g_boneWriteSeam = kBoneSeamRenderSide;
std::atomic_bool g_haveBoneReference = false;
float g_boneReference[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
std::atomic_uint64_t g_boneWrites = 0;
std::atomic_uint64_t g_boneSkippedNoInstance = 0;
std::atomic_uint64_t g_boneSkippedNoPose = 0;
std::atomic_uint64_t g_boneAlreadyApplied = 0;
std::atomic_uint64_t g_boneLastReportTick = 0;

// THE EVERY-OTHER-FRAME FLICKER, AND WHY A FRAME GATE CANNOT FIX IT.
//
// Reported: the driven skeleton alternated between the moved pose and the
// original, one frame each. That is the clock split in section 3 of the handoff --
// there are TWO view builds per presented frame, and the gate fired once per
// PRESENTED frame, so one render got the write and the other did not.
//
// Counting more carefully would not fix it, because the real requirement is that
// the array is rotated exactly once per ENGINE recomputation, and neither clock
// tells us when that happened. So the write is made IDEMPOTENT instead: a copy of
// what we last wrote is kept, and each pass compares against it. Identical means
// we already rotated this computation and the pass is skipped; different means the
// engine has recomputed and a fresh rotation is applied.
//
// That is self-correcting whatever the ratio of passes to frames to renders turns
// out to be, which is the property a clock gate can never have.
constexpr int kMaxTrackedBones = 128;
// One snapshot PER TARGET, because ALL mode drives several arrays in a pass and a
// single shared snapshot would make every array after the first look already-
// rotated.
float g_lastWritten[kMaxBoneTargets][kMaxTrackedBones * 12]{};
int g_lastWrittenCounts[kMaxBoneTargets]{};
// The last ENGINE-produced contents we saw, kept beside the last thing we wrote.
// Two snapshots, not one, because "the array changed" has two very different
// meanings -- the engine recomputed it, or the engine recomputed it to a pose
// that is itself moving -- and only the pair can tell them apart.
float g_lastEngineState[kMaxBoneTargets][kMaxTrackedBones * 12]{};
bool g_haveEngineState[kMaxBoneTargets]{};

// WRITE-SURVIVAL TELEMETRY. THE INSTRUMENT THAT MAKES A NULL ATTRIBUTABLE.
//
// Every "drove array X, the gun did not move" result this project has produced
// shares one defect: nobody measured whether the write was still there when the
// draw ran. A write recomputed over before its consumer reads it produces
// exactly "nothing", for every target, whether or not the target was right --
// so 25 nulls in one day distinguished nothing at all.
//
// So on each pass, BEFORE writing, the array is compared against what we last
// wrote into it:
//
//   SURVIVED     -- byte-identical to our last write. Nothing touched it in
//                   between, so if the gun still does not move, this array is
//                   not what the gun's renderer reads (fallback: the staged
//                   CModelRenderSystem::m_BoneToWorld copy).
//   OVERWRITTEN  -- the engine replaced it. If the gun does not move, the seam
//                   is still upstream of this consumer and the write has to
//                   happen later (post-SetupBones for that entity).
//
// Those two point at completely different fixes, which is the entire reason the
// counter exists.
struct TargetTelemetry {
    std::uint64_t survived = 0;
    std::uint64_t overwritten = 0;
    // Of the overwritten passes, how many landed on exactly the pose the engine
    // produced last time. High here means a static authored pose being recomputed
    // every frame; low means the pose is genuinely animating.
    std::uint64_t overwrittenToSamePose = 0;
    std::uint64_t written = 0;
    // How far bone 0's world translation had moved between our write and this
    // pass. Zero with OVERWRITTEN high means a rewrite of the same values; a real
    // distance means the whole rig moved.
    float lastDriftUnits = 0.0f;
};
TargetTelemetry g_telemetry[kMaxBoneTargets]{};

// The rotation each array is currently CARRYING, relative to the engine's own
// authored pose, so a pass can apply the difference rather than the whole thing.
//
// The old code applied the full rotation to whatever was in the array and relied
// on the engine having reset it in between. That is only correct while the engine
// recomputes every frame -- which is the very thing the telemetry above exists to
// question, and which the synthetic drive below makes false the moment a write
// survives. Tracking what is already applied makes the pin correct under BOTH
// answers: survived means apply the difference, overwritten means apply the lot.
float g_appliedRotation[kMaxBoneTargets][9]{};
bool g_appliedValid[kMaxBoneTargets]{};

// THE SYNTHETIC DRIVE. WHAT MAKES THIS TESTABLE AT A DESK.
//
// With no headset there is no hand pose, and every previous version of the pin
// simply idled -- so the only way to ask "does writing this array move the gun"
// was to put the headset on, which is the scarcest resource in the project. A
// fixed 30 deg/s yaw about the array's own pivot asks exactly the same question
// flat, on a monitor, and answers it more clearly than a hand ever could: a
// steady spin is unmistakable, and it cannot be confused with tracking jitter.
constexpr float kSyntheticYawDegreesPerSecond = 30.0f;
std::atomic_bool g_syntheticDrive = false;
std::atomic_uint64_t g_syntheticStartTick = 0;

bool ReadBasisGuarded(volatile const float source[9], volatile const std::uint32_t& generation,
                      float out[9]) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint32_t before = generation;
        if (before & 1u) continue;
        for (int i = 0; i < 9; ++i) out[i] = source[i];
        if (generation == before) return true;
    }
    return false;
}

// Still the object we cached? A weapon switch or respawn frees it, and writing
// matrices into freed memory is the one failure here that pressing the key again
// would not recover from.
bool TargetStillValid(const BoneTarget& target) {
    if (!target.instance) return false;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return false;
    if (!IsReadable(target.instance, 8)) return false;
    const auto expected = reinterpret_cast<std::uintptr_t>(client) + target.vtableRva;
    if (*reinterpret_cast<const std::uintptr_t*>(target.instance) != expected) return false;
    return IsReadable(target.instance + target.boneOffset, 48 * target.boneCount);
}

}  // namespace

// TARGETS FOUND BY POSITION, NOT BY CLASS NAME.
//
// Two runs have now shown that class identity does not lead to the gun: driving
// C_ViewmodelAttachmentModel's 74 bones moved the whole BODY, and no enumerated
// class array moved the weapon at all. Names have misled twice, so this stops
// using them.
//
// What is reliable is GEOMETRY. The viewmodel's own bone 0 is at the player's
// hands, in world coordinates, and anything drawn in the player's hands -- the
// weapon included -- must have its bones within a metre or two of it. So the
// search now collects every bone-shaped run near that reference, whatever object
// owns it and whether or not the class can be named -- but bounded to the INSIDE
// of known entities, not across all of memory. The unbounded version froze a run.
//
// This also catches the double buffer. The flicker survived being made
// idempotent, which rules out re-application: alternating with the ORIGINAL pose
// means a second copy of the skeleton is being rendered on those frames. A
// position search finds both copies, because both are in the player's hands, and
// the ALL mode below writes to every one of them at once.
// A HARD WALL-CLOCK BUDGET FOR EVERY SCAN IN THIS FILE, AND ONE PER PASS.
//
// Without a budget at all, the position search froze a run: it ran the full bone
// predicate at every 4-byte offset across 4.7 GB, over a billion evaluations.
//
// With ONE budget shared between the two passes, it did something worse, twice.
// The instance walk consumed the entire 4 s by itself -- 4.4 GB and still not
// finished -- and the deep pass that actually finds the arrays then either never
// entered its loop (when the shared outcome read TIMED OUT) or broke out on its
// first instance (when a full instance list had recorded a different ending
// first). Both runs reported "0 bone array(s) near the hands" having never
// looked inside a single instance, and the second reported it under an outcome
// line that blamed the instance cap.
//
// So the passes get separate clocks and separate outcomes, and BOTH are printed.
// A budget shared between a producer and its consumer is not a budget, it is a
// race the consumer always loses.
constexpr std::uint64_t kInstanceWalkBudgetMs = 6000;
constexpr std::uint64_t kDeepSearchBudgetMs = 1500;
// How far into an entity to look for its bone array. Generous, because the two
// known arrays sit at +0x1260 and +0x1870, and cheap, because the search is
// bounded to entities instead of all of memory.
constexpr std::size_t kDeepInstanceScanBytes = 0x8000;
constexpr float kNearReferenceUnits = 250.0f;
constexpr int kMinDiscoveredRun = 8;

// THE WHOLE SCAN, UNDER SEH.
//
// The per-region guard below was necessary and not sufficient: it covered
// pass 1, and the scan faulted again in the same place from the log reader
// point of view -- because the walk summary is only printed AFTER passes 1b
// and 2, and both of those read memory too. Pass 1b dereferences every
// candidate instance to test its bone array; pass 2 walks INSIDE those
// instances. Every one of those reads races the same way: VirtualQuery said
// readable, another thread freed it.
//
// Guarding each site in turn is whack-a-mole on a function whose entire job is
// reading memory that might vanish. So the whole scan runs under one guard: a
// fault anywhere abandons the scan and reports it, instead of killing an armed
// headset session. The targets are cleared on the way out because a scan that
// died halfway has no business leaving half a table behind.
//
// The SEH lives in this tiny wrapper rather than in the scan itself, because
// __try cannot share a function with C++ unwinding and the scan has locals.
bool CacheWeaponBoneInstanceUnguarded();

bool CacheWeaponBoneInstance() {
    __try {
        return CacheWeaponBoneInstanceUnguarded();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_boneTargetCount.store(0, std::memory_order_release);
        Tf2VrLog("[TF2VR] bone scan: FAULTED and was abandoned. A region the walk was told was "
                 "readable went away under it. Nothing is cached and nothing is armed -- the game "
                 "is still running, which is the point. Try again; the race is timing-dependent.\n");
        return false;
    }
}

// One region, read under SEH. Returns false if it faulted partway.
//
// Separated out because a function containing __try may not also need C++
// unwinding, and because "skip the region" has to be expressible as a single
// early exit. Everything it mutates is passed by reference so the caller's
// tallies survive a fault that happens halfway through a region.
bool ScanRegionForVtables(const std::uint8_t* regionBase, std::size_t size,
                          std::uintptr_t vtableLow, std::uintptr_t vtableSpan,
                          const std::uintptr_t* vtableAddresses, int classCount,
                          int maxPerClass, int* perClassCount, const std::uint8_t** instances,
                          int* instanceClass, int& instanceCount, int maxInstances,
                          bool& capped) {
    __try {
        for (std::size_t offset = 0; offset + 8 <= size && instanceCount < maxInstances;
             offset += 8) {
            const auto value = *reinterpret_cast<const std::uintptr_t*>(regionBase + offset);
            if (value - vtableLow > vtableSpan) continue;
            for (int classIndex = 0; classIndex < classCount; ++classIndex) {
                if (value != vtableAddresses[classIndex]) continue;
                if (perClassCount[classIndex] >= maxPerClass) { capped = true; break; }
                ++perClassCount[classIndex];
                instances[instanceCount] = regionBase + offset;
                instanceClass[instanceCount] = classIndex;
                ++instanceCount;
                break;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CacheWeaponBoneInstanceUnguarded() {
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(client);

    g_boneTargetCount.store(0, std::memory_order_release);
    int found = 0;

    // ---- PASS 1: instances of every known class, by vtable pointer ----
    struct KnownClass {
        std::uintptr_t vtableRva;
        const char* name;
        // The array this class is KNOWN to carry, measured and validated by
        // effect on 2026-08-17. Zero count means no known array for this class.
        std::size_t boneOffset;
        int boneCount;
    };
    const KnownClass knownClasses[] = {
        {kViewmodelVtableRva, "viewmodel", kViewmodelBoneOffset, kViewmodelBoneCount},
        {kAttachmentVtableRva, "attachment", kAttachmentBoneOffset, kAttachmentBoneCount},
        {0x998638, "weaponx", 0, 0},
        {0x8AED48, "baseanimating", 0, 0},
    };
    constexpr int kClassesHere = 4;
    // Raised from 16/96. The 16 filled on one class and recorded an instance-cap
    // ending that then masked the real one. Pass 2 costs about 2000 predicate
    // evaluations per instance, so 32 per class is microseconds, not seconds.
    constexpr int kMaxPerClass = 32;
    constexpr int kMaxScannedInstances = 128;
    const std::uint8_t* instances[kMaxScannedInstances]{};
    int instanceClass[kMaxScannedInstances]{};
    int instanceCount = 0;
    int perClassCount[kClassesHere]{};
    std::size_t bytesScanned = 0;
    unsigned regionsVisited = 0;
    ScanOutcome walkOutcome;

    // ONE RANGE TEST BEFORE THE FOUR-WAY COMPARE. Every candidate vtable is
    // client.dll + an RVA inside a 1 MB window, so the overwhelmingly common
    // case -- a qword that is not any vtable -- is rejected by a subtract and a
    // compare instead of four. The walk did not finish inside its budget at four
    // compares per 8 bytes across 4.4 GB; this is why it now does.
    std::uintptr_t vtableLow = ~static_cast<std::uintptr_t>(0);
    std::uintptr_t vtableHigh = 0;
    for (int index = 0; index < kClassesHere; ++index) {
        const std::uintptr_t address = base + knownClasses[index].vtableRva;
        if (address < vtableLow) vtableLow = address;
        if (address > vtableHigh) vtableHigh = address;
    }
    const std::uintptr_t vtableSpan = vtableHigh - vtableLow;
    std::uintptr_t vtableAddresses[kClassesHere]{};
    for (int index = 0; index < kClassesHere; ++index) {
        vtableAddresses[index] = base + knownClasses[index].vtableRva;
    }
    unsigned regionsFaulted = 0;

    const std::uint64_t walkStart = GetTickCount64();
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    auto* address = static_cast<const std::uint8_t*>(systemInfo.lpMinimumApplicationAddress);
    const auto* limit = static_cast<const std::uint8_t*>(systemInfo.lpMaximumApplicationAddress);
    for (;;) {
        if (address >= limit) { walkOutcome.Finish(ScanOutcome::End::Complete); break; }
        if (instanceCount >= kMaxScannedInstances) {
            walkOutcome.Finish(ScanOutcome::End::InstanceCap);
            break;
        }
        if (bytesScanned >= kMaxBytesScanned) {
            walkOutcome.Finish(ScanOutcome::End::ByteCap);
            break;
        }
        if (GetTickCount64() - walkStart > kInstanceWalkBudgetMs) {
            walkOutcome.Finish(ScanOutcome::End::TimeBudget);
            break;
        }
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(address, &info, sizeof(info))) {
            walkOutcome.Finish(ScanOutcome::End::WalkFailed);
            break;
        }
        const auto* regionBase = static_cast<const std::uint8_t*>(info.BaseAddress);
        const std::size_t size = info.RegionSize;
        if (IsScannableRegion(info)) {
            ++regionsVisited;
            bytesScanned += size;
            // GUARDED, BECAUSE VirtualQuery IS A SNAPSHOT AND THIS IS A LIVE
            // PROCESS.
            //
            // The query says a region is committed and readable; by the time
            // the read happens another thread may have freed or decommitted it,
            // and then this faults and takes the game down. That is not a
            // hypothetical -- it is what killed an armed headset session at
            // exactly this line, with the log stopping on "running the bone
            // scan first" and nothing after it.
            //
            // It had been lucky for many runs. The full VR stack churns
            // allocations far harder than a flat run does, which is why it
            // finally lost. A faulting region is skipped rather than fatal:
            // the scan is a search, and missing one region costs a candidate,
            // where dying costs the session.
            bool capped = false;
            if (!ScanRegionForVtables(regionBase, size, vtableLow, vtableSpan, vtableAddresses,
                                      kClassesHere, kMaxPerClass, perClassCount, instances,
                                      instanceClass, instanceCount, kMaxScannedInstances,
                                      capped)) {
                ++regionsFaulted;
            }
            if (capped) walkOutcome.Finish(ScanOutcome::End::InstanceCap);
        }
        if (size == 0) { walkOutcome.Finish(ScanOutcome::End::WalkFailed); break; }
        address = regionBase + size;
    }
    const std::uint64_t walkMs = GetTickCount64() - walkStart;

    // ---- PASS 1b: THE ARRAYS WE ALREADY KNOW, REGISTERED FIRST ----
    //
    // C_BaseViewModel + 0x1870 (87 bones) and C_ViewmodelAttachmentModel +
    // 0x1260 (74 bones) were measured, cross-checked against each other, and the
    // 74 was validated BY EFFECT -- writing it visibly moved the rig. Making the
    // position search the only way to reach them meant that when the search
    // failed, targets that were never in doubt went missing too, and the pin
    // refused to arm with 18 live instances sitting in the list.
    //
    // So the known offsets go in first, unconditionally, subject only to being
    // readable and actually looking like bones. The position search then ADDS
    // whatever else is near the hands -- which is what it was for: catching the
    // second buffer, not replacing what is known.
    float reference[3]{};
    bool haveReference = false;
    for (int index = 0; index < instanceCount && found < kMaxBoneTargets; ++index) {
        const KnownClass& owner = knownClasses[instanceClass[index]];
        if (owner.boneCount <= 0) continue;
        const std::uint8_t* array = instances[index] + owner.boneOffset;
        if (!IsReadable(array, 48ull * owner.boneCount)) continue;
        if (!LooksLikeBoneMatrix(reinterpret_cast<const float*>(array))) continue;
        bool duplicate = false;
        for (int seen = 0; seen < found; ++seen) {
            if (g_boneTargets[seen].instance == array) { duplicate = true; break; }
        }
        if (duplicate) continue;
        g_boneTargets[found] = {array, instances[index], owner.vtableRva, owner.boneOffset,
                                owner.boneCount, owner.name, false};
        ++found;
        if (!haveReference) {
            const auto* first = reinterpret_cast<const float*>(array);
            reference[0] = first[3];
            reference[1] = first[7];
            reference[2] = first[11];
            haveReference = true;
        }
    }
    const int knownFound = found;
    int positionCandidates = 0;

    // ---- PASS 2: everything else bone-shaped near the same position ----
    //
    // Bounded to the INSIDE of known entities, not across all of memory: a bone
    // array owned by an entity lives inside that entity, and the unbounded
    // version froze a run. Its own clock, so it cannot be starved by pass 1.
    ScanOutcome deepOutcome;
    const std::uint64_t deepStart = GetTickCount64();
    if (!haveReference) {
        // Nothing known to anchor on. Take the first long run found anywhere in
        // the instances as the reference, then keep everything near it.
        for (int index = 0; index < instanceCount && !haveReference; ++index) {
            if (GetTickCount64() - deepStart > kDeepSearchBudgetMs) {
                deepOutcome.Finish(ScanOutcome::End::TimeBudget);
                break;
            }
            const std::uint8_t* instance = instances[index];
            for (std::size_t offset = 0;
                 offset + 48ull * kMinDiscoveredRun <= kDeepInstanceScanBytes; offset += 16) {
                if (!IsReadable(instance + offset, 48ull * kMinDiscoveredRun)) continue;
                const auto* candidate = reinterpret_cast<const float*>(instance + offset);
                if (!LooksLikeBoneMatrix(candidate)) continue;
                int run = 0;
                while (run < 256 && IsReadable(instance + offset + 48ull * run, 48) &&
                       LooksLikeBoneMatrix(candidate + run * 12)) {
                    ++run;
                }
                if (run < kMinDiscoveredRun) continue;
                reference[0] = candidate[3];
                reference[1] = candidate[7];
                reference[2] = candidate[11];
                haveReference = true;
                break;
            }
        }
    }
    if (haveReference) {
        for (int index = 0; index < instanceCount && found < kMaxBoneTargets; ++index) {
            if (GetTickCount64() - deepStart > kDeepSearchBudgetMs) {
                deepOutcome.Finish(ScanOutcome::End::TimeBudget);
                break;
            }
            const std::uint8_t* instance = instances[index];
            std::size_t offset = 0;
            while (offset + 48ull * kMinDiscoveredRun <= kDeepInstanceScanBytes &&
                   found < kMaxBoneTargets) {
                if (!IsReadable(instance + offset, 48ull * kMinDiscoveredRun)) { offset += 16; continue; }
                const auto* candidate = reinterpret_cast<const float*>(instance + offset);
                if (!LooksLikeBoneMatrix(candidate)) { offset += 16; continue; }
                int run = 0;
                while (run < 256 && IsReadable(instance + offset + 48ull * run, 48) &&
                       LooksLikeBoneMatrix(candidate + run * 12)) {
                    ++run;
                }
                if (run < kMinDiscoveredRun) { offset += 16; continue; }
                const float dx = candidate[3] - reference[0];
                const float dy = candidate[7] - reference[1];
                const float dz = candidate[11] - reference[2];
                if (dx * dx + dy * dy + dz * dz <= kNearReferenceUnits * kNearReferenceUnits) {
                    // DEDUPLICATE BY ADDRESS. Entities overlap in memory and a
                    // vtable match is an entry point, not an owner: without this
                    // the same array was reported twice, ALL mode rotated it
                    // twice, and cycling between [0] and [1] cycled between the
                    // same thing.
                    bool duplicate = false;
                    for (int seen = 0; seen < found; ++seen) {
                        if (g_boneTargets[seen].instance == instance + offset) { duplicate = true; break; }
                    }
                    // LOGGED, NOT DRIVEN. This is what crashed the game.
                    //
                    // "A run of orthonormal matrix3x4" is a shape, not a proof of
                    // ownership. Driving position-discovered runs means writing
                    // rotated matrices into whatever heap object happens to carry
                    // that shape -- and on 2026-08-18 it took the process down
                    // with an access violation, mid-run, having found a 59- and a
                    // 27-bone "array" that the previous run had not seen at all.
                    // A target list that differs between two runs of the same
                    // code in the same scene is not a target list.
                    //
                    // The two KNOWN arrays are registered by construction and are
                    // safe. Candidates are still reported, because the gun's real
                    // array may well be among them and that is worth knowing --
                    // but nothing writes to one until it has been identified by
                    // something better than its silhouette.
                    if (!duplicate) {
                        ++positionCandidates;
                        char candidate[420]{};
                        std::snprintf(candidate, sizeof(candidate),
                            "[TF2VR] bone pin CANDIDATE (not driven): %d bone-shaped matrices at %p, "
                            "inside a %s instance, %.0f units from the reference. Reported only -- "
                            "driving unidentified runs crashed the game.\n",
                            run, static_cast<const void*>(instance + offset),
                            knownClasses[instanceClass[index]].name,
                            static_cast<double>(std::sqrt(dx * dx + dy * dy + dz * dz)));
                        Tf2VrLog(candidate);
                    }
                }
                offset += static_cast<std::size_t>(run) * 48;
            }
            if (found >= kMaxBoneTargets) deepOutcome.Finish(ScanOutcome::End::InstanceCap);
        }
    }
    deepOutcome.Finish(ScanOutcome::End::Complete);
    const std::uint64_t deepMs = GetTickCount64() - deepStart;

    g_boneTargetCount.store(found, std::memory_order_release);
    g_boneTargetIndex.store(found > 0 ? kBoneTargetAll : 0, std::memory_order_release);

    // PER-CLASS COUNTS, NOT JUST A TOTAL.
    //
    // The first run of this scan reported 34 instances and exactly one usable
    // array, and there was no way to tell whether the 74-bone attachment was
    // missing because no instance of that class exists in this scene or because
    // instances exist and +0x1260 did not hold bone-shaped data. Those need
    // completely different fixes -- one is "wrong scene", the other is "stale
    // offset" -- and a total cannot separate them.
    char walkLine[900]{};
    int used = std::snprintf(walkLine, sizeof(walkLine),
        "[TF2VR] bone pin, INSTANCE WALK: %d instance(s) of known classes over %u regions, %.0f MB "
        "in %llu ms, %u region(s) skipped after faulting.",
        instanceCount, regionsVisited, static_cast<double>(bytesScanned) / (1024.0 * 1024.0),
        static_cast<unsigned long long>(walkMs), regionsFaulted);
    for (int index = 0; index < kClassesHere && used < static_cast<int>(sizeof(walkLine)) - 80;
         ++index) {
        used += std::snprintf(walkLine + used, sizeof(walkLine) - used, " %s=%d%s",
                              knownClasses[index].name, perClassCount[index],
                              perClassCount[index] >= kMaxPerClass ? "(CAPPED)" : "");
    }
    std::snprintf(walkLine + used, sizeof(walkLine) - used, ". %s\n", walkOutcome.Describe());
    Tf2VrLog(walkLine);

    char deepLine[700]{};
    std::snprintf(deepLine, sizeof(deepLine),
        "[TF2VR] bone pin, ARRAYS: %d known-offset array(s) registered and DRIVABLE; %d further "
        "bone-shaped run(s) reported as CANDIDATES ONLY, in %llu ms. %s\n",
        knownFound, positionCandidates, static_cast<unsigned long long>(deepMs),
        deepOutcome.Describe());
    Tf2VrLog(deepLine);

    char line[980]{};
    used = std::snprintf(line, sizeof(line), "[TF2VR] bone pin: %d array(s) to drive.", found);
    for (int index = 0; index < found && used < static_cast<int>(sizeof(line)) - 60; ++index) {
        used += std::snprintf(line + used, sizeof(line) - used, " [%d]=%s/%dbones", index,
                              g_boneTargets[index].className, g_boneTargets[index].boneCount);
    }
    std::snprintf(line + used, sizeof(line) - used, "\n");
    Tf2VrLog(line);

    if (found == 0) {
        char causes[560]{};
        std::snprintf(causes, sizeof(causes),
            "[TF2VR] bone pin: nothing to drive. Every cause, in order: the instance walk found %d "
            "instances -- if that is 0, no entities exist yet and you are not in a match; if it is "
            "non-zero then neither known offset (+0x%zX/%d and +0x%zX/%d) held bone-shaped data on "
            "any instance, and no array was found by position within 0x%zX bytes of one. Read BOTH "
            "outcome lines above before treating this zero as an absence.\n",
            instanceCount, kViewmodelBoneOffset, kViewmodelBoneCount, kAttachmentBoneOffset,
            kAttachmentBoneCount, kDeepInstanceScanBytes);
        Tf2VrLog(causes);
    }
    return found > 0;
}
namespace {

constexpr float kIdentityRotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

// R = a * b^T, the rotation that takes b to a. Both are row-major 3x3.
void RotationDifference(const float a[9], const float b[9], float out[9]) {
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            out[row * 3 + col] = a[row * 3 + 0] * b[col * 3 + 0] +
                                 a[row * 3 + 1] * b[col * 3 + 1] +
                                 a[row * 3 + 2] * b[col * 3 + 2];
        }
    }
}

bool IsNearIdentity(const float r[9]) {
    for (int index = 0; index < 9; ++index) {
        if (std::fabs(r[index] - kIdentityRotation[index]) > 1.0e-5f) return false;
    }
    return true;
}

// The owner is still the object the scan identified, checked by its vtable
// pointer. Cheap, and the only check that separates a live entity from a freed
// block that is still committed.
bool OwnerStillValid(const BoneTarget& target) {
    if (!target.owner || target.vtableRva == 0) return false;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return false;
    if (!IsReadable(target.owner, sizeof(std::uintptr_t))) return false;
    const auto expected = reinterpret_cast<std::uintptr_t>(client) + target.vtableRva;
    return *reinterpret_cast<const std::uintptr_t*>(target.owner) == expected;
}

// NOTHING THIS FILE DOES MAY TAKE THE GAME DOWN.
//
// A bone write is a diagnostic writing rotated matrices into heap memory it
// identified by shape and by offset. On 2026-08-18 that faulted mid-run and
// killed the process, which costs a whole session and teaches nothing. Every
// touch of a target's memory now goes through one of these two, so the worst
// case is a target that retires itself and says so in the log.
//
// PODs only in these frames, which is what lets SEH sit in a C++ translation
// unit compiled with /EHsc.
bool GuardedCompare(const float* bones, const float* snapshot, std::size_t bytes, bool& equal) {
    __try {
        equal = std::memcmp(bones, snapshot, bytes) == 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        equal = false;
        return false;
    }
}

bool GuardedRotate(float* bones, int boneCount, const float d[9]) {
    __try {
        const float pivot[3] = {bones[3], bones[7], bones[11]};
        for (int bone = 0; bone < boneCount; ++bone) {
            float* m = bones + bone * 12;
            float rotated[12]{};
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) {
                    rotated[row * 4 + col] = d[row * 3 + 0] * m[0 * 4 + col] +
                                             d[row * 3 + 1] * m[1 * 4 + col] +
                                             d[row * 3 + 2] * m[2 * 4 + col];
                }
            }
            const float rel[3] = {m[3] - pivot[0], m[7] - pivot[1], m[11] - pivot[2]};
            for (int row = 0; row < 3; ++row) {
                rotated[row * 4 + 3] = pivot[row] + d[row * 3 + 0] * rel[0] +
                                       d[row * 3 + 1] * rel[1] + d[row * 3 + 2] * rel[2];
            }
            std::memcpy(m, rotated, sizeof(rotated));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A target that faulted is out for the rest of the session. It is not retried,
// not silently skipped, and the log says which one and why -- a crash that this
// used to cause takes a whole run with it and teaches nothing.
void RetireTarget(int slot, const char* why) {
    if (slot < 0 || slot >= kMaxBoneTargets) return;
    if (g_boneTargets[slot].retired) return;
    g_boneTargets[slot].retired = true;
    g_lastWrittenCounts[slot] = 0;
    g_haveEngineState[slot] = false;
    g_appliedValid[slot] = false;
    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] bone pin: target [%d] %s/%d bones RETIRED -- %s. Its memory is no longer the "
        "object the scan identified. Nothing writes to it again this session; re-arm to rescan.\n",
        slot, g_boneTargets[slot].className, g_boneTargets[slot].boneCount, why);
    Tf2VrLog(line);
}

}  // namespace

// Applies `desired` -- the rotation this array should be CARRYING relative to the
// engine's authored pose -- to one discovered array, and records how the array
// arrived in the state it was found in. Returns true if bytes were written.
bool RotateOneTarget(const BoneTarget& target, const float desired[9], int slot) {
    if (!target.instance || target.retired) return false;
    if (slot < 0 || slot >= kMaxBoneTargets) return false;
    // THE OWNER MUST STILL BE THE OBJECT WE SCANNED. A weapon switch, a respawn
    // or a level change frees the entity, and the freed block stays committed --
    // so IsReadable keeps saying yes while the write goes into whatever now lives
    // there. Re-reading the vtable pointer is cheap and is the only check that
    // actually distinguishes the two.
    if (!OwnerStillValid(target)) return false;
    const int boneCount = target.boneCount < kMaxTrackedBones ? target.boneCount : kMaxTrackedBones;
    if (!IsReadable(target.instance, 48ull * boneCount)) return false;
    auto* bones = reinterpret_cast<float*>(const_cast<std::uint8_t*>(target.instance));
    const std::size_t bytes = static_cast<std::size_t>(boneCount) * 12 * sizeof(float);
    TargetTelemetry& telemetry = g_telemetry[slot];

    // ---- SURVIVAL, MEASURED BEFORE ANYTHING IS WRITTEN ----
    const bool haveSnapshot = g_lastWrittenCounts[slot] == boneCount;
    bool survived = false;
    if (haveSnapshot && !GuardedCompare(bones, g_lastWritten[slot], bytes, survived)) {
        RetireTarget(slot, "the comparison faulted");
        return false;
    }
    if (haveSnapshot) {
        if (survived) {
            ++telemetry.survived;
        } else {
            ++telemetry.overwritten;
            bool samePose = false;
            if (g_haveEngineState[slot] &&
                GuardedCompare(bones, g_lastEngineState[slot], bytes, samePose) && samePose) {
                ++telemetry.overwrittenToSamePose;
            }
            const float dx = bones[3] - g_lastWritten[slot][3];
            const float dy = bones[7] - g_lastWritten[slot][7];
            const float dz = bones[11] - g_lastWritten[slot][11];
            telemetry.lastDriftUnits = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    }
    // Whatever the engine last handed us, kept so "recomputed to the same pose"
    // can be told from "recomputed to a moving pose" on the next pass.
    if (!survived) {
        bool ignored = false;
        if (!GuardedCompare(bones, bones, bytes, ignored)) {
            RetireTarget(slot, "the array became unreadable");
            return false;
        }
        std::memcpy(g_lastEngineState[slot], bones, bytes);
        g_haveEngineState[slot] = true;
    }

    // ---- THE INCREMENT TO APPLY ----
    //
    // If the array survived it already carries g_appliedRotation, so only the
    // difference goes on. If the engine overwrote it, it carries nothing and the
    // whole of `desired` goes on. Either way the array ends up carrying exactly
    // `desired` relative to the authored pose, which is what makes a survived
    // write and an overwritten one produce the same picture on screen.
    const float* carried = (survived && g_appliedValid[slot]) ? g_appliedRotation[slot]
                                                              : kIdentityRotation;
    float d[9]{};
    RotationDifference(desired, carried, d);
    if (survived && IsNearIdentity(d)) return false;

    if (!GuardedRotate(bones, boneCount, d)) {
        RetireTarget(slot, "the write faulted");
        return false;
    }
    std::memcpy(g_lastWritten[slot], bones, bytes);
    g_lastWrittenCounts[slot] = boneCount;
    std::memcpy(g_appliedRotation[slot], desired, sizeof(g_appliedRotation[slot]));
    g_appliedValid[slot] = true;
    ++telemetry.written;
    return true;
}

namespace {

void ResetPinState() {
    g_haveBoneReference.store(false, std::memory_order_release);
    for (int slot = 0; slot < kMaxBoneTargets; ++slot) {
        g_lastWrittenCounts[slot] = 0;
        g_haveEngineState[slot] = false;
        g_appliedValid[slot] = false;
        g_telemetry[slot] = TargetTelemetry{};
    }
    g_syntheticStartTick.store(GetTickCount64(), std::memory_order_release);
}

// The 30 deg/s flat-test spin, as an ABSOLUTE rotation for the current instant
// rather than a per-pass increment -- the increment is worked out against what
// the array is already carrying, which is what keeps the rate honest whether the
// engine recomputes between passes or not.
//
// Yaw is about Source's world Z, which is up, so the motion is horizontal and
// reads unambiguously on a monitor.
void SyntheticRotation(float out[9], float& degreesOut) {
    const std::uint64_t start = g_syntheticStartTick.load(std::memory_order_acquire);
    const double seconds = static_cast<double>(GetTickCount64() - start) / 1000.0;
    double degrees = seconds * kSyntheticYawDegreesPerSecond;
    degrees -= 360.0 * std::floor(degrees / 360.0);
    degreesOut = static_cast<float>(degrees);
    const double radians = degrees * 0.01745329252;
    const auto c = static_cast<float>(std::cos(radians));
    const auto s = static_cast<float>(std::sin(radians));
    out[0] = c; out[1] = -s; out[2] = 0;
    out[3] = s; out[4] = c;  out[5] = 0;
    out[6] = 0; out[7] = 0;  out[8] = 1;
}

void ReportSurvival(int index, int count, bool synthetic, float syntheticDegrees) {
    const std::uint64_t nowTick = GetTickCount64();
    if (nowTick - g_boneLastReportTick.load(std::memory_order_acquire) < 1000) return;
    g_boneLastReportTick.store(nowTick, std::memory_order_release);

    char header[600]{};
    std::snprintf(header, sizeof(header),
        "[TF2VR] bone pin @ VIEW-BUILD seam: target %s of %d array(s), drive = %s. SURVIVED means "
        "nothing touched our write between passes; OVERWRITTEN means the engine recomputed over it. "
        "If the gun does not move: OVERWRITTEN -> the seam is still upstream of the gun's consumer; "
        "SURVIVED -> this array is not what the gun's renderer reads.\n",
        index == kBoneTargetAll ? "ALL" : "one", count,
        synthetic ? "SYNTHETIC 30 deg/s yaw" : "right controller aim");
    Tf2VrLog(header);

    for (int slot = 0; slot < count; ++slot) {
        if (index != kBoneTargetAll && index != slot) continue;
        const TargetTelemetry& telemetry = g_telemetry[slot];
        const std::uint64_t seen = telemetry.survived + telemetry.overwritten;
        const double survivedPct = seen ? 100.0 * static_cast<double>(telemetry.survived) /
                                          static_cast<double>(seen)
                                        : 0.0;
        char line[680]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR]   [%d] %s/%d bones @%p -- %llu written, SURVIVED %llu (%.1f%%), OVERWRITTEN "
            "%llu (of those %llu onto an identical pose), bone0 drift %.2f units%s\n",
            slot, g_boneTargets[slot].className, g_boneTargets[slot].boneCount,
            static_cast<const void*>(g_boneTargets[slot].instance),
            static_cast<unsigned long long>(telemetry.written),
            static_cast<unsigned long long>(telemetry.survived), survivedPct,
            static_cast<unsigned long long>(telemetry.overwritten),
            static_cast<unsigned long long>(telemetry.overwrittenToSamePose),
            static_cast<double>(telemetry.lastDriftUnits),
            seen == 0 ? " -- NO PASS HAS YET COMPARED THIS ARRAY, so these zeros say nothing."
                      : ".");
        Tf2VrLog(line);
    }
    if (synthetic) {
        char spin[260]{};
        std::snprintf(spin, sizeof(spin),
            "[TF2VR]   synthetic yaw now %.0f deg. On screen that is a steady horizontal spin; "
            "report WHAT spins -- the GUN, the arms/body, or nothing.\n",
            static_cast<double>(syntheticDegrees));
        Tf2VrLog(spin);
    }
}

}  // namespace

void ApplyWeaponBonePin() {
    if (!g_bonePinEnabled.load(std::memory_order_acquire)) return;
    if (g_bonePinPaused.load(std::memory_order_acquire)) return;
    const int count = g_boneTargetCount.load(std::memory_order_acquire);
    if (count <= 0) {
        g_boneSkippedNoInstance.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    float desired[9]{};
    float syntheticDegrees = 0.0f;
    const bool synthetic = g_syntheticDrive.load(std::memory_order_acquire);
    if (synthetic) {
        SyntheticRotation(desired, syntheticDegrees);
    } else {
        if (!(g_controllerPoseFlags[kHandRight] & kControllerAimTracked)) {
            g_boneSkippedNoPose.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        float now[9]{};
        if (!ReadBasisGuarded(g_controllerAimBasis[kHandRight], g_controllerGeneration[kHandRight],
                              now)) {
            g_boneSkippedNoPose.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!g_haveBoneReference.load(std::memory_order_acquire)) {
            for (int i = 0; i < 9; ++i) g_boneReference[i] = now[i];
            g_haveBoneReference.store(true, std::memory_order_release);
            for (int slot = 0; slot < kMaxBoneTargets; ++slot) {
                g_lastWrittenCounts[slot] = 0;
                g_appliedValid[slot] = false;
            }
            Tf2VrLog("[TF2VR] bone pin: reference captured; hold the controller still, then rotate.\n");
            return;
        }
        // D = now^T * reference, the same operand order as the camera pin.
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                desired[i * 3 + j] = now[0 * 3 + i] * g_boneReference[0 * 3 + j] +
                                     now[1 * 3 + i] * g_boneReference[1 * 3 + j] +
                                     now[2 * 3 + i] * g_boneReference[2 * 3 + j];
            }
        }
    }

    const int index = g_boneTargetIndex.load(std::memory_order_acquire);
    int applied = 0;
    int skipped = 0;
    if (index == kBoneTargetAll) {
        for (int slot = 0; slot < count; ++slot) {
            if (RotateOneTarget(g_boneTargets[slot], desired, slot)) ++applied; else ++skipped;
        }
    } else if (index >= 0 && index < count) {
        if (RotateOneTarget(g_boneTargets[index], desired, index)) ++applied; else ++skipped;
    }
    if (applied) g_boneWrites.fetch_add(1, std::memory_order_relaxed);
    if (skipped) g_boneAlreadyApplied.fetch_add(1, std::memory_order_relaxed);

    ReportSurvival(index, count, synthetic, syntheticDegrees);
}

// THE SEAM ITSELF. Called from clientViewBuildInterceptor in view_build_hook.asm,
// which is CViewRender's view-build entry -- after the engine tick, and before the
// render build that consumes the pose. The camera-upload hook this replaced sat
// downstream of that build, and no null taken there could ever say whether the
// target was wrong or the seam was.
extern "C" void ApplyWeaponBonePinAtViewBuild() {
    if (g_boneWriteSeam.load(std::memory_order_acquire) != kBoneSeamViewBuild) return;
    ApplyWeaponBonePin();
}

// THE LATER SEAM, chosen by measurement. The camera upload runs after the
// engine's own bone setup -- which is not an assumption any more: the view-build
// write is observably gone by the time this point is reached. No once-per-frame
// gate, because the write is now absolute rather than a delta, so applying it on
// every viewmodel pass of a frame is idempotent and gives it the best chance of
// being in place whenever the draw actually reads it.
void ApplyWeaponBonePinAtRenderSide() {
    if (g_boneWriteSeam.load(std::memory_order_acquire) != kBoneSeamRenderSide) return;
    ApplyWeaponBonePin();
}

void CycleWeaponBoneTarget() {
    const int count = g_boneTargetCount.load(std::memory_order_acquire);
    if (count <= 0) {
        Tf2VrLog("[TF2VR] bone pin: no arrays cached yet; arm the pin first.\n");
        return;
    }
    // ALL first, then each array in turn, then back to ALL.
    const int current = g_boneTargetIndex.load(std::memory_order_acquire);
    const int next = (current == kBoneTargetAll) ? 0 : (current + 1 >= count ? kBoneTargetAll : current + 1);
    g_boneTargetIndex.store(next, std::memory_order_release);
    ResetPinState();
    g_boneWrites.store(0, std::memory_order_release);
    g_boneAlreadyApplied.store(0, std::memory_order_release);
    char line[520]{};
    if (next == kBoneTargetAll) {
        std::snprintf(line, sizeof(line),
            "[TF2VR] bone pin target -> ALL %d arrays at once. Survival telemetry re-zeroed. If the "
            "flicker stops ONLY in this mode, the alternation was a second buffer.\n",
            count);
    } else {
        std::snprintf(line, sizeof(line),
            "[TF2VR] bone pin target -> [%d] of %d, %s, %d bones at %p. Survival telemetry "
            "re-zeroed. Report WHAT moves: the GUN, the arms/body, or nothing.\n",
            next, count, g_boneTargets[next].className, g_boneTargets[next].boneCount,
            static_cast<const void*>(g_boneTargets[next].instance));
    }
    Tf2VrLog(line);
}

void SetWeaponBonePinEnabled(bool enabled) {
    if (!enabled) {
        // Stop the seam BEFORE clearing the state it reads. The view build runs on
        // its own thread and could otherwise be mid-pass over snapshots being
        // zeroed underneath it.
        g_bonePinEnabled.store(false, std::memory_order_release);
        g_bonePinViewBuildArmed = 0;
        ResetPinState();
        char line[480]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] bone pin OFF after %llu applied passes (%llu already-rotated, %llu no hand). "
            "The view-build seam is an exact pass-through again. The engine recomputes bones every "
            "frame, so releasing restores the authored pose.\n",
            static_cast<unsigned long long>(g_boneWrites.load()),
            static_cast<unsigned long long>(g_boneAlreadyApplied.load()),
            static_cast<unsigned long long>(g_boneSkippedNoPose.load()));
        Tf2VrLog(line);
        return;
    }
    ResetPinState();
    g_boneWrites.store(0, std::memory_order_release);
    g_boneAlreadyApplied.store(0, std::memory_order_release);
    g_boneSkippedNoInstance.store(0, std::memory_order_release);
    g_boneSkippedNoPose.store(0, std::memory_order_release);

    // ---- THE PRECONDITION, ARMED HERE AND SAID OUT LOUD ----
    //
    // Deterministic bone setup is not an option this shares a keyboard with, it is
    // what turns an intermittent race into a repeatable experiment. It was built,
    // documented and bound to F3 -- and a run was then spent with it switched off,
    // where a precondition sitting unarmed looks exactly like a dead write. So the
    // pin arms it itself, before the scan, and logs that it did.
    if (IsDeterministicBoneSetupArmed()) {
        Tf2VrLog("[TF2VR] bone pin: deterministic bone setup was ALREADY armed; leaving it alone.\n");
    } else {
        Tf2VrLog("[TF2VR] bone pin: AUTO-ARMING deterministic bone setup first -- it is the "
                 "precondition for every bone write, and a run has already been lost to it sitting "
                 "switched off.\n");
        ToggleDeterministicBoneSetup();
        if (!IsDeterministicBoneSetupArmed()) {
            Tf2VrLog("[TF2VR] bone pin WARNING: the auto-arm did not take. Bone setup may still be "
                     "threaded, asynchronous and speculative, so anything that fails below could be "
                     "a race rather than a dead write.\n");
        }
    }

    // ---- THE SEAM, CONFIRMED INSTALLED BEFORE ANYTHING IS ARMED ----
    EnsureClientViewBuildHookInstalled();
    if (!IsClientViewBuildHookInstalled()) {
        Tf2VrLog("[TF2VR] bone pin NOT armed: the view-build detour is not installed, so nothing "
                 "would ever call the write. This refuses rather than arming silently -- a pin armed "
                 "against a seam that was never patched is exactly the unattributable null this step "
                 "exists to abolish.\n");
        return;
    }

    Tf2VrLog("[TF2VR] bone pin: scanning for every bone array near the player's hands (one-shot, a "
             "few seconds).\n");
    if (!CacheWeaponBoneInstance()) {
        Tf2VrLog("[TF2VR] bone pin NOT armed: nothing to write to.\n");
        return;
    }

    // ---- THE DRIVE, CHOSEN ONCE, HERE, ON WHAT IS ACTUALLY TRACKED ----
    //
    // No headset means no hand pose, and an idle pin at a desk asks nothing at
    // all. The synthetic spin asks the same question flat that the hand asks in
    // VR, and asks it more legibly: a steady 30 deg/s cannot be confused with
    // tracking jitter, and it needs nobody to be wearing anything.
    //
    // THE TEST IS NOT "IS XR ARMED". `autoarm = 4` arms OpenXR from the ini as
    // soon as the swapchain is ready, headset or no headset, so at a desk
    // IsXrArmed() is true and a pin keyed on it alone would sit there skipping
    // every pass for want of a hand -- a dead-looking write whose real cause was
    // a flag. What decides is whether a right-hand pose is actually TRACKED.
    //
    // Latched at arm time rather than re-decided per pass, so that a momentary
    // tracking dropout in the headset run cannot drop a live hand pin into a
    // synthetic spin mid-test. Press the key again to re-choose.
    const bool xrArmed = IsXrArmed();
    const bool handTracked = (g_controllerPoseFlags[kHandRight] & kControllerAimTracked) != 0;
    const bool synthetic = !xrArmed || !handTracked;
    g_syntheticDrive.store(synthetic, std::memory_order_release);
    g_syntheticStartTick.store(GetTickCount64(), std::memory_order_release);

    g_bonePinEnabled.store(true, std::memory_order_release);
    g_bonePinViewBuildArmed = 1;

    // The staged bone-to-world buffer, read and reported alongside the targets.
    // Read-only, and it runs here so it needs no key and no extra step: whatever
    // arms the pin gets the fallback's evidence in the same log, at the same
    // instant, with the same entities live.
    ProbeBoneStagingBuffer();
    RequestBoneStagingSample();

    Tf2VrLog("[TF2VR] bone pin ARMED in ALL mode, ROTATION ONLY, at the VIEW-BUILD seam.\n"
             "[TF2VR]   The write now happens in CViewRender's view build -- after the engine tick, "
             "before the render build -- not in the camera upload, which was render side and "
             "downstream of it.\n"
             "[TF2VR]   Class names are not used to choose the target. Two runs showed they do not "
             "lead to the gun -- the array named 'attachment' drives the BODY -- so every "
             "bone-shaped array near the player's hands is collected by POSITION, whatever object "
             "owns it, and stepped through by key.\n"
             "[TF2VR]   Write survival is measured on every pass and reported once a second, so a "
             "null from here names its own cause instead of being another unattributable zero.\n");
    char drive[520]{};
    if (synthetic) {
        std::snprintf(drive, sizeof(drive),
            "[TF2VR]   DRIVE: SYNTHETIC, %.0f deg/s yaw about each array's own pivot. Chosen "
            "because OpenXR is %s and the right hand's aim pose is %s. No headset needed -- watch "
            "the monitor and report WHAT spins.\n",
            static_cast<double>(kSyntheticYawDegreesPerSecond),
            xrArmed ? "armed" : "NOT armed", handTracked ? "tracked" : "NOT tracked");
    } else {
        std::snprintf(drive, sizeof(drive),
            "[TF2VR]   DRIVE: the right controller's AIM pose -- OpenXR is armed and the hand is "
            "tracked. Hold still for the reference, then rotate.\n");
    }
    Tf2VrLog(drive);
}

bool IsWeaponBonePinEnabled() { return g_bonePinEnabled.load(std::memory_order_acquire); }

void RecentreWeaponBonePin() {
    ResetPinState();
    Tf2VrLog("[TF2VR] bone pin reference and survival telemetry cleared; the next pass re-zeroes "
             "them.\n");
}

int WeaponBoneTargetCount() { return g_boneTargetCount.load(std::memory_order_acquire); }

void SetWeaponBoneTargetIndex(int index) {
    const int count = g_boneTargetCount.load(std::memory_order_acquire);
    if (count <= 0) return;
    const int clamped = (index < 0 || index >= count) ? kBoneTargetAll : index;
    g_boneTargetIndex.store(clamped, std::memory_order_release);
    // Fresh telemetry per target. A phase that inherited the previous target's
    // counters would report survival for an array it never touched.
    ResetPinState();
}

void SetWeaponBonePinPaused(bool paused) {
    if (paused == g_bonePinPaused.load(std::memory_order_acquire)) return;
    g_bonePinPaused.store(paused, std::memory_order_release);
    // Coming out of a pause, every snapshot describes a pose the engine has since
    // recomputed several times over. Clearing them stops the first pass of the new
    // phase being counted as an overwrite that says nothing.
    if (!paused) ResetPinState();
}

bool IsWeaponBonePinSynthetic() { return g_syntheticDrive.load(std::memory_order_acquire); }

bool DescribeWeaponBoneTarget(int slot, char* out, size_t size) {
    if (!out || size == 0) return false;
    if (slot < 0 || slot >= g_boneTargetCount.load(std::memory_order_acquire)) return false;
    const TargetTelemetry& telemetry = g_telemetry[slot];
    const unsigned long long seen = telemetry.survived + telemetry.overwritten;
    const double survivedPct = seen ? 100.0 * static_cast<double>(telemetry.survived) /
                                      static_cast<double>(seen)
                                    : 0.0;
    std::snprintf(out, size,
        "[%d] %s/%d bones @%p -- %llu written, SURVIVED %llu (%.1f%%), OVERWRITTEN %llu, bone0 "
        "drift %.2f units%s",
        slot, g_boneTargets[slot].className, g_boneTargets[slot].boneCount,
        static_cast<const void*>(g_boneTargets[slot].instance),
        static_cast<unsigned long long>(telemetry.written),
        static_cast<unsigned long long>(telemetry.survived), survivedPct,
        static_cast<unsigned long long>(telemetry.overwritten),
        static_cast<double>(telemetry.lastDriftUnits),
        seen == 0 ? " -- NO PASS COMPARED THIS ARRAY, so these zeros say nothing" : "");
    return true;
}

void SetWeaponBonePinSyntheticDrive(bool synthetic) {
    if (synthetic == g_syntheticDrive.load(std::memory_order_acquire)) return;
    g_syntheticDrive.store(synthetic, std::memory_order_release);
    g_syntheticStartTick.store(GetTickCount64(), std::memory_order_release);
    ResetPinState();
    Tf2VrLog(synthetic
        ? "[TF2VR] bone pin drive FORCED to the synthetic 30 deg/s yaw, whatever the controllers "
          "are reporting.\n"
        : "[TF2VR] bone pin drive FORCED to the right controller's aim pose.\n");
}

namespace {
std::atomic_uint64_t g_renderSurvived[kMaxBoneTargets]{};
std::atomic_uint64_t g_renderOverwritten[kMaxBoneTargets]{};
std::atomic_uint64_t g_renderLastReportTick = 0;

void ResetRenderSurvivalCounters() {
    for (int slot = 0; slot < kMaxBoneTargets; ++slot) {
        g_renderSurvived[slot].store(0, std::memory_order_release);
        g_renderOverwritten[slot].store(0, std::memory_order_release);
    }
}
}  // namespace

void ProbeWeaponBoneWriteSurvival() {
    if (!g_bonePinEnabled.load(std::memory_order_acquire)) return;
    if (g_bonePinPaused.load(std::memory_order_acquire)) return;
    const int count = g_boneTargetCount.load(std::memory_order_acquire);
    if (count <= 0) return;
    const int index = g_boneTargetIndex.load(std::memory_order_acquire);

    for (int slot = 0; slot < count && slot < kMaxBoneTargets; ++slot) {
        if (index != kBoneTargetAll && index != slot) continue;
        const BoneTarget& target = g_boneTargets[slot];
        if (!target.instance) continue;
        const int boneCount = target.boneCount < kMaxTrackedBones ? target.boneCount
                                                                  : kMaxTrackedBones;
        if (g_lastWrittenCounts[slot] != boneCount) continue;
        if (!IsReadable(target.instance, 48ull * boneCount)) continue;
        const auto* bones = reinterpret_cast<const float*>(target.instance);
        const std::size_t bytes = static_cast<std::size_t>(boneCount) * 12 * sizeof(float);
        if (std::memcmp(bones, g_lastWritten[slot], bytes) == 0) {
            g_renderSurvived[slot].fetch_add(1, std::memory_order_relaxed);
        } else {
            g_renderOverwritten[slot].fetch_add(1, std::memory_order_relaxed);
        }
    }

    const std::uint64_t nowTick = GetTickCount64();
    if (nowTick - g_renderLastReportTick.load(std::memory_order_acquire) < 1000) return;
    g_renderLastReportTick.store(nowTick, std::memory_order_release);
    for (int slot = 0; slot < count && slot < kMaxBoneTargets; ++slot) {
        if (index != kBoneTargetAll && index != slot) continue;
        const std::uint64_t survived = g_renderSurvived[slot].load();
        const std::uint64_t overwritten = g_renderOverwritten[slot].load();
        const std::uint64_t seen = survived + overwritten;
        if (seen == 0) continue;
        char line[640]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] END-OF-FRAME survival [%d] %s/%d bones: our %s was still intact at PRESENT "
            "on %llu of %llu frames (%.1f%%). %s\n",
            slot, g_boneTargets[slot].className, g_boneTargets[slot].boneCount,
            WeaponBoneWriteSeamName(),
            static_cast<unsigned long long>(survived), static_cast<unsigned long long>(seen),
            100.0 * static_cast<double>(survived) / static_cast<double>(seen),
            survived * 2 >= seen
                ? "INTACT: the write is still standing when the frame ends, so a still gun means "
                  "the gun's renderer reads a DIFFERENT buffer -- the staged-copy fallback, not a "
                  "later seam."
                : "LOST: the engine recomputes over this write before the frame ends, so it has to "
                  "happen later still, not elsewhere.");
        Tf2VrLog(line);
    }
}

void SetWeaponBoneWriteSeam(int seam) {
    const int clamped = seam == kBoneSeamViewBuild ? kBoneSeamViewBuild : kBoneSeamRenderSide;
    if (clamped == g_boneWriteSeam.load(std::memory_order_acquire)) return;
    g_boneWriteSeam.store(clamped, std::memory_order_release);
    ResetPinState();
    ResetRenderSurvivalCounters();
    char line[300]{};
    std::snprintf(line, sizeof(line), "[TF2VR] bone pin write seam -> %s. Telemetry re-zeroed.\n",
                  WeaponBoneWriteSeamName());
    Tf2VrLog(line);
}

int WeaponBoneWriteSeam() { return g_boneWriteSeam.load(std::memory_order_acquire); }

const char* WeaponBoneWriteSeamName() {
    return g_boneWriteSeam.load(std::memory_order_acquire) == kBoneSeamViewBuild
        ? "VIEW-BUILD seam write" : "RENDER-SIDE seam write";
}

bool GetWeaponBoneTargetArray(int slot, const void** array, int* boneCount,
                             const char** className) {
    if (slot < 0 || slot >= g_boneTargetCount.load(std::memory_order_acquire)) return false;
    const BoneTarget& target = g_boneTargets[slot];
    if (!target.instance || target.retired) return false;
    if (array) *array = target.instance;
    if (boneCount) *boneCount = target.boneCount;
    if (className) *className = target.className;
    return true;
}
