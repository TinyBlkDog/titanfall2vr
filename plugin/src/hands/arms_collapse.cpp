#include "arms_collapse.h"

#include "diagnostics.h"
#include "mesh_census.h"
#include "viewmodel_instance.h"
#include "titan_state.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

// arms vtable slot 205, byte offset 205*8. Both are asserted against the
// binary before anything is swapped.
constexpr std::uintptr_t kAttachmentVtableRva = 0x8B8158;
constexpr int kSetupBonesSlot = 205;
constexpr std::size_t kSetupBonesSlotOffset = kSetupBonesSlot * 8;  // 0x668
constexpr std::uintptr_t kSetupBonesRva = 0x0EE950;
// mov [rsp+20h],r9 / mov [rsp+18h],r8 / push rsi / push r13 / sub rsp,1F8h
constexpr std::uint8_t kSetupBonesPrologue[] = {0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89,
                                                0x44, 0x24, 0x18, 0x56, 0x41, 0x55};

// THREE buffers, 64 matrices each -- corrected after the first attempt.
//
// The first version read the gap between r8 ([rbp+0x350]) and r9
// ([rbp+0x1B50]), got 0x1800, divided by 48 and called it 128 bones. There is
// a THIRD buffer at [rbp+0xF50] sitting between them, which the caller passes
// to client.dll+0x0ABAA0 as its own third argument. Each array is therefore
// 0xC00 = 3072 bytes = 64 matrices, and writing 128 from r8 ran straight
// through the middle buffer.
//
// It did not crash, because it stayed inside the caller's frame, and it was
// not wasted: buffers one and two were zeroed on hundreds of frames and the
// arms drew normally throughout. That rules both of them out and points at the
// third, which the first attempt stopped exactly short of.
//
// Measuring a stride from two endpoints only works if nothing lives between
// them. It did.
constexpr int kBonesPerBuffer = 64;
constexpr std::size_t kMatrixBytes = 48;
constexpr std::size_t kBufferBytes = kBonesPerBuffer * kMatrixBytes;  // 0xC00
constexpr std::size_t kExpectedSpan = 0x1800;  // r9 - r8 when the layout holds

// NOT A FIELD. viewmodel_bones found a bone array at object+0x1260 on a fresh level
// load and this project drove it from there for two weeks; on 2026-09-03 the
// BONE-POINTER SCAN showed that address is where the allocator happened to put
// the heap block *(object+0x1008) points at. Kept only so the TARGET log line
// can say whether this instance shows the coincidence. The destination is the
// pointer field (see THE DESTINATION, BY REFERENCE). kMaxCacheBones bounds the
// studiohdr numbones sanity check and the once-per-refusal scan.
constexpr std::size_t kAttachmentBoneArrayOffset = 0x1260;
constexpr int kMaxCacheBones = 160;

void** g_slot = nullptr;
// body.show from the ini / menu: 0 weapon only, 1 weapon + hands, 2 full body.
// g_wanted is "some collapse is wanted" (mode != 2); checked every frame so a
// rebuilt viewmodel re-arms. Defaults match the registry default (0).
int g_bodyMode = 0;
bool g_wanted = true;
bool g_handsFallbackSaid = false;

}  // namespace

extern "C" {
// Shared with arms_bones.asm.
std::uint64_t g_armsBonesInstance = 0;
std::uint8_t g_armsBonesArmed = 0;
std::uint8_t g_armsBonesInFlight = 0;
std::uint64_t g_armsBonesArray = 0;
std::uint64_t g_armsBonesArray2 = 0;
// SetupBones second argument. THE ENGINE READS THE STUDIOHDR AT [rdx+8] -- see
// RealBoneCount. Captured rather than derived from an entity offset, because
// the entity offset that worked for the aim census belongs to a different
// class and read back as unreadable on the arms.
std::uint64_t g_armsBonesModel = 0;
std::uint64_t g_armsBonesSavedReturn = 0;
std::uint64_t g_armsBonesOriginal = 0;
void armsBonesInterceptor();

// Counters, for the report. Written only from the post-process, which the
// in-flight guard keeps to one at a time.
std::uint64_t g_armsBonesCollapses = 0;
int g_armsBonesCacheAccepted = 0;
// What the model declares for the latched target (numbones from the studiohdr).
int g_armsBonesRealCount = -1;
// Which instance the count above was measured on. Zero forces a re-measure.
std::uint64_t g_armsBonesCountedFor = 0;

// COLLAPSE TO ONE POINT, NOT TO EACH BONE'S OWN ORIGIN.
//
// The first version zeroed the 3x3 and kept each bone's own translation. That
// sends every vertex to ITS bone's origin -- and the 73 origins are spread all
// over the arms and body, so triangles spanning two or three bones survive as
// slivers between those points. The wearer saw exactly that: "pure black with
// jagged outlines". Black because the transformed normals are all zero, so
// there is no lighting left; jagged because the geometry still had area.
//
// Sending every bone to the SAME anchor makes every triangle degenerate --
// all three vertices at one point, zero area, no pixels. The anchor is bone
// zero's own translation rather than the origin, so the numbers stay finite
// and stay somewhere the renderer already expected geometry to be.
void FlattenMatrix(float* m, float ax, float ay, float az) {
    // Row-major 3x4: [0..2] rotation, [3] translation, per row.
    m[0] = 0.0f; m[1] = 0.0f; m[2] = 0.0f;  m[3] = ax;
    m[4] = 0.0f; m[5] = 0.0f; m[6] = 0.0f;  m[7] = ay;
    m[8] = 0.0f; m[9] = 0.0f; m[10] = 0.0f; m[11] = az;
}

// Is the translation column finite and in-world? Used for the anchor, which is
// read from bone 0 AFTER earlier frames zeroed its rotation rows.
bool TranslationLooksPlausible(const float* m) {
    for (int row = 0; row < 3; ++row) {
        const float v = m[row * 4 + 3];
        if (!(v == v)) return false;
        if (v > 1.0e6f || v < -1.0e6f) return false;
    }
    return true;
}

// Where bone `bone` collapses to: the root anchor, or -- when anchorFrom names
// another bone -- that bone's own translation read from the target array. Used
// by hands mode to send the forearm to its wrist, so vertices skinned across
// the wrist seam land on the glove instead of stretching to the root anchor.
static void AnchorFor(int bone, const int* anchorFrom, const float* targetRead, int targetCount,
                      float ax, float ay, float az, float out[3]) {
    out[0] = ax; out[1] = ay; out[2] = az;
    if (!anchorFrom || !targetRead) return;
    const int from = anchorFrom[bone];
    if (from < 0 || from >= targetCount) return;
    const float* m = targetRead + from * 12;
    if (!TranslationLooksPlausible(m)) return;
    out[0] = m[3]; out[1] = m[7]; out[2] = m[11];
}

// Flatten `count` matrices in one buffer, skipping any bone whose keep bit is
// set. keep == nullptr flattens every one (weapon-only mode, the original
// path). Kept bones are not touched at all, so they carry exactly what the
// engine's SetupBones just wrote.
static void FlattenBufferMasked(std::uint8_t* buffer, int count, const bool* keep, const int* anchorFrom,
                                const float* targetRead, int targetCount, float ax, float ay, float az) {
    for (int bone = 0; bone < count; ++bone) {
        if (keep && keep[bone]) continue;
        float a[3];
        AnchorFor(bone, anchorFrom, targetRead, targetCount, ax, ay, az, a);
        FlattenMatrix(reinterpret_cast<float*>(buffer + bone * kMatrixBytes), a[0], a[1], a[2]);
    }
}

// HANDS MODE, FOURTH ATTEMPT (2026-09-04): NOT A POINT, A NaN.
//
// Three flat runs and one headset look showed the shape of the problem. A
// collapsed bone sends its vertices to a point, and every triangle that mixes
// a kept bone with a collapsed one -- the cuff ring -- stretches from the
// glove to that point. Two collapse points (one per wrist) shortened the cuff
// streaks to nothing and instead drew the chest boundary as BLACK BANDS tying
// the two gloves together (wearer, headset). One point, two points, three:
// every partition of a connected mesh has a boundary, and the boundary draws.
//
// A vertex whose position is NaN makes the GPU discard the whole triangle.
// So in hands mode the collapsed bones of the ENTITY'S OWN ARRAY (the one
// the draw reads) are set to NaN outright: every body and sleeve triangle is
// gone, every cuff-ring triangle is gone, and only the all-glove triangles
// draw. The caller's stack copies stay finite (flattened as before) so no
// engine CPU code downstream of SetupBones is handed a NaN.
static void NanMatrix(float* m) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (int i = 0; i < 12; ++i) m[i] = nan;
}

// FIFTH ATTEMPT (headset F6 capture, 2026-09-04): NaN alone left the glove
// with holes -- the palm and back of the hand carry small weights on the
// forearm twist bone, and one NaN vertex takes the whole triangle. So the
// bones the glove blends with are not NaN'd but made RIGID WITH THE WRIST:
// their matrix is a copy of the wrist's, so a glove vertex weighted
// wrist+forearm transforms exactly as wrist-only and the glove is complete.
// The forearm-only sleeve vertices then draw as a short stub that follows
// the hand, and the stub ends where the elbow-blended triangles are NaN'd.
// rigidTo[b] >= 0 names the bone to copy from; it must be a kept bone (never
// written), so it is fresh from the engine's own SetupBones.
//
// SIXTH (headset F6 captures of the fourth and fifth): NaN is DEAD. A skinned
// vertex carries four bone slots and the unused ones hold weight ZERO; zero
// times NaN is NaN, so any vertex whose padding slot names a collapsed bone
// dies whole, fingers included. Both NaN builds drew glove fragments only.
// NanMatrix is kept for the record; nothing calls it.
//
// So hands mode is FINITE again: every collapsed bone goes to a point (its
// side's wrist, two points), the forearm twist copies its wrist so the glove
// is whole, and the band between the gloves that two points draw is accepted
// until the mesh/material lever (see the MATERIALS dump below) replaces this.
static void HandsBufferMasked(std::uint8_t* buffer, int count, const bool* keep, const int* rigidTo,
                              const int* anchorFrom, const float* targetRead, int targetCount,
                              float ax, float ay, float az) {
    for (int bone = 0; bone < count; ++bone) {
        if (keep && keep[bone]) continue;
        float* m = reinterpret_cast<float*>(buffer + bone * kMatrixBytes);
        const int from = rigidTo ? rigidTo[bone] : -1;
        if (from >= 0 && from < count && keep && keep[from]) {
            std::memcpy(m, buffer + from * kMatrixBytes, kMatrixBytes);
            continue;
        }
        float a[3];
        AnchorFor(bone, anchorFrom, targetRead, targetCount, ax, ay, az, a);
        FlattenMatrix(m, a[0], a[1], a[2]);
    }
}

// ---------------------------------------------------------------------------
// THE REAL BONE COUNT, AND WHY THE HEURISTIC HAD TO GO. 2026-09-03.
//
// The count used to come from walking the cache and asking "does this still
// look like a bone matrix?" until it stopped. That is an over-count waiting to
// happen: past the end of the array the walk accepts whatever plausible floats
// follow, and then the collapse WRITES ZEROS to every slot it counted. The
// 2026-08-29 titan crash is that write, and the record says so -- a server-side
// object's vtable slot read back as ZERO and the RTTI walk faulted reading
// [0 - 8]. Client and server share one heap in a listen-server campaign. It had
// never run against a titan's model, which is a different size.
//
// The count is in the model. Read out of the engine's own SetupBones
// (client.dll+0x0EE950, whose continuation does):
//
//     mov    rax, [r13+0x8]        ; the studiohdr
//     movsxd r14, [rax+0xA4]       ; +0xA4 = boneindex, an offset
//     add    r14, rax              ; -> the bone array
//     mov    r9d, [rdx+0xA0]       ; +0xA0 = numbones, THE COUNT
//
// An adjacent count/offset pair, which is the standard Source studiohdr shape
// and the cross-check that the reading is right. The studiohdr hangs off the
// entity at +0x1208 and carries 'IDST', both already relied on by the aim
// census. Validated here before a single byte is written.
constexpr std::size_t kStudioHdrPtrOffset = 0x1208;   // entity -> studiohdr
constexpr std::size_t kStudioNumBonesOffset = 0xA0;   // studiohdr -> numbones
constexpr std::uint32_t kIdstMagic = 0x54534449;      // 'IDST'

// ---------------------------------------------------------------------------
// THE BONE TABLE (names and parents), read from the studiohdr. 2026-09-04.
//
// numbones/boneindex are the adjacent pair SetupBones itself reads (above).
// The record stride is NOT stock: the same function's bone loop does
//
//     000EEC06  imul rax, rax, 0xF4          ; bone index * 0xF4
//     000EEC0D  movsxd rcx, [rax+r14+0x4]    ; record+0x4 = parent
//
// so a record is 0xF4 = 244 bytes with parent at +4, and the name is reached
// the way every Source studio record does it: an int at +0 relative to the
// record itself (ads_probe.cpp reads attachments the same way, and its
// R_HAND/L_HAND census is the positive control here: the gun's viewmodel
// lists them on bones 72 and 71, so the arms table must too).
constexpr std::size_t kStudioBoneIndexOffset = 0xA4;   // studiohdr -> bone table (offset)
// +0x0C printed "..." on 2026-09-04: in v53 it is sznameindex (an int); the
// char[64] name is at +0x10 (mesh_census.cpp controls it against "models").
constexpr std::size_t kStudioNameOffset = 0x10;
constexpr std::size_t kStudioHdrBytesNeeded = 0xA8;
constexpr std::size_t kBoneRecordStride = 0xF4;
constexpr std::size_t kBoneRecordNameIndex = 0x00;
constexpr std::size_t kBoneRecordParent = 0x04;
constexpr int kBoneDumpMaxModels = 4;
constexpr int kBoneNameCap = 40;

// The model's own bone count, or -1 when it cannot be established. NEVER falls
// back to a guess: refusing to collapse leaves the arms visible, which is
// cosmetic, while guessing corrupts the heap, which ends the session.
// Reads numbones out of one candidate studiohdr. Negative is a REASON, not a
// single "no": the first attempt returned a bare -1 for three different causes
// and the run could not say which, so each failure is now distinguishable.
//   -1 null pointer   -2 wrong magic   -3 count out of range   -4 faulted
int BoneCountFromStudioHdr(const std::uint8_t* studio, std::uint32_t* magicOut, int* rawOut) {
    if (!studio) return -1;
    __try {
        const std::uint32_t magic = *reinterpret_cast<const std::uint32_t*>(studio);
        if (magicOut) *magicOut = magic;
        if (magic != kIdstMagic) return -2;
        const int count = *reinterpret_cast<const int*>(studio + kStudioNumBonesOffset);
        if (rawOut) *rawOut = count;
        if (count <= 0 || count > kMaxCacheBones) return -3;
        return count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -4;
    }
}

// THE MODEL, FROM THE ENGINE'S OWN ARGUMENT rather than an entity offset.
//
// The first version read the studiohdr at entity+0x1208, which is where the aim
// census finds it -- on the FACED CHARACTER's class. On
// C_ViewmodelAttachmentModel it read back unreadable, and the run said so:
// "model says -1, the old memory walk says 74". Guessing a second offset would
// be the same mistake twice.
//
// SetupBones is handed the model in rdx and dereferences [rdx+8] to reach the
// studiohdr it then takes numbones from. The asm now captures rdx, so this
// follows the engine's own pointer instead of a layout assumption. The entity
// route is kept only as a cross-check, and both are logged.
int RealBoneCount(const std::uint8_t* entity, char* why, std::size_t whySize) {
    std::uint32_t modelMagic = 0, entityMagic = 0;
    int modelRaw = 0, entityRaw = 0;
    const std::uint8_t* fromModel = nullptr;
    __try {
        if (g_armsBonesModel) {
            fromModel = *reinterpret_cast<const std::uint8_t* const*>(
                reinterpret_cast<const std::uint8_t*>(g_armsBonesModel) + 8);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fromModel = nullptr;
    }
    const int viaModel = BoneCountFromStudioHdr(fromModel, &modelMagic, &modelRaw);

    const std::uint8_t* fromEntity = nullptr;
    __try {
        if (entity) {
            fromEntity = *reinterpret_cast<const std::uint8_t* const*>(entity + kStudioHdrPtrOffset);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fromEntity = nullptr;
    }
    const int viaEntity = BoneCountFromStudioHdr(fromEntity, &entityMagic, &entityRaw);

    if (why && whySize) {
        std::snprintf(why, whySize,
            "model-arg rdx=%p studiohdr=%p magic=%08X raw=%d -> %d | entity+0x1208 studiohdr=%p "
            "magic=%08X raw=%d -> %d (negatives: -1 null, -2 magic, -3 range, -4 fault)",
            reinterpret_cast<void*>(g_armsBonesModel), reinterpret_cast<const void*>(fromModel),
            modelMagic, modelRaw, viaModel, reinterpret_cast<const void*>(fromEntity), entityMagic,
            entityRaw, viaEntity);
    }
    if (viaModel > 0) return viaModel;
    if (viaEntity > 0) return viaEntity;
    return -1;
}

bool LooksLikeBoneMatrix(const float* m) {
    for (int i = 0; i < 12; ++i) {
        const float v = m[i];
        if (!(v == v)) return false;                      // NaN
        if (v > 3.0e38f || v < -3.0e38f) return false;    // inf
    }
    for (int row = 0; row < 3; ++row) {
        const float* r = m + row * 4;
        if (r[3] > 1.0e6f || r[3] < -1.0e6f) return false;
        const float len2 = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
        if (len2 < 1.0e-4f || len2 > 1.0e4f) return false;
    }
    return true;
}

// Every early return and every skipped write, counted (see the write gate).
std::uint64_t g_armsBonesCacheWrites = 0;
std::uint64_t g_armsBonesUnvalidatedSkips = 0;
// Instances the walk refused, witnessed ONCE each: what was at +0x1260 when
// the walk read zero is the open question ("where does this instance keep its
// bones"), and a per-frame line would drown the log.
constexpr int kWitnessedInstances = 8;
std::uint64_t g_witnessed[kWitnessedInstances]{};
int g_witnessedCount = 0;

// WHERE DOES THE INSTANCE KEEP ITS BONES? 2026-09-03, run 2EF203DD (third).
//
// Post-exit instances of the SAME class and the SAME 74-bone model held, at
// +0x1260, fifty slots of garbage and zeros and then 69 bone-like matrices
// starting at slot 50 -- and at slot 46 on the next cycle. A field does not
// move by four matrices between instances of one class, so +0x1260 is not a
// field: the bone array is a separate heap block, and on a fresh level load
// the allocator happens to put it right after the entity. Source keeps this as
// a CUtlVector<matrix3x4_t> -- {pointer, allocCount, growSize, size} -- so
// the object holds a POINTER to it, at an offset nobody has found yet.
//
// This scans the object's first 0x1260 bytes for 8-byte fields whose value is
// a readable address at which consecutive bone-like matrices begin, and prints
// each with the three ints that follow it (a CUtlVector would show its counts
// there). Run on validated and unvalidated instances alike, once each: the
// offset common to both is the field. READ-ONLY; nothing acts on it.
constexpr std::size_t kPointerScanBytes = 0x1260;
constexpr int kPointerScanMinWalk = 8;
constexpr int kPointerScanMaxHits = 6;

std::size_t BytesFrom(const void* p, bool needWrite);  // defined with the target code below

void ScanForBonePointerFields(const std::uint8_t* self, int numbones, const char* label) {
    char line[1000]{};
    int used = std::snprintf(line, sizeof(line),
        "[TF2VR] arms-collapse BONE-POINTER SCAN of %s instance %p (model %d bones): fields whose "
        "target walks as bones ->", label, reinterpret_cast<const void*>(self), numbones);
    int hits = 0;
    // Never read past the object's own committed region.
    const std::size_t readable = BytesFrom(self, false);
    const std::size_t scanBytes = readable < kPointerScanBytes ? readable : kPointerScanBytes;
    for (std::size_t off = 0; off + 8 <= scanBytes && hits < kPointerScanMaxHits; off += 8) {
        const auto p = *reinterpret_cast<const std::uintptr_t*>(self + off);
        if (p < 0x10000 || p > 0x00007FFFFFFFFFFFull || (p & 0x3) != 0) continue;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(p), &mbi, sizeof(mbi))) continue;
        if (mbi.State != MEM_COMMIT) continue;
        const DWORD prot = mbi.Protect & 0xFF;
        if (prot != PAGE_READONLY && prot != PAGE_READWRITE && prot != PAGE_EXECUTE_READ &&
            prot != PAGE_EXECUTE_READWRITE) continue;
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        const int fit = static_cast<int>((regionEnd - p) / kMatrixBytes);
        const int limit = fit < kMaxCacheBones ? fit : kMaxCacheBones;
        int walk = 0;
        while (walk < limit &&
               LooksLikeBoneMatrix(reinterpret_cast<const float*>(p + walk * kMatrixBytes))) {
            ++walk;
        }
        if (walk < kPointerScanMinWalk) continue;
        const auto* ints = reinterpret_cast<const std::int32_t*>(self + off + 8);
        const bool intsReadable = off + 20 <= scanBytes;
        used += std::snprintf(line + used, sizeof(line) - static_cast<std::size_t>(used),
            " [+0x%llX -> %p walk %d%s, next ints %d/%d/%d%s]",
            static_cast<unsigned long long>(off), reinterpret_cast<const void*>(p), walk,
            walk == limit ? " (region end)" : "",
            intsReadable ? ints[0] : -999, intsReadable ? ints[1] : -999, intsReadable ? ints[2] : -999,
            p == reinterpret_cast<std::uintptr_t>(self) + kAttachmentBoneArrayOffset ? " == self+0x1260" : "");
        ++hits;
        if (used >= static_cast<int>(sizeof(line)) - 120) break;
    }
    if (hits) {
        std::snprintf(line + used, sizeof(line) - static_cast<std::size_t>(used), " (%d field%s).\n", hits,
                      hits == 1 ? "" : "s");
    } else {
        std::snprintf(line + used, sizeof(line) - static_cast<std::size_t>(used),
                      " NONE in the first 0x1260 bytes.\n");
    }
    Tf2VrLog(line);
}

void WitnessUnvalidatedInstance(const std::uint8_t* self, const char* reason, const char* why) {
    const auto inst = reinterpret_cast<std::uint64_t>(self);
    for (int i = 0; i < g_witnessedCount; ++i) {
        if (g_witnessed[i] == inst) return;
    }
    if (g_witnessedCount >= kWitnessedInstances) return;
    g_witnessed[g_witnessedCount++] = inst;

    // The vptr read is bounded: ValidateTarget only refuses after BytesFrom has
    // vouched for the object, or with a reason that says it could not.
    std::uintptr_t vptr = 0;
    if (BytesFrom(self, false) >= sizeof(void*)) vptr = *reinterpret_cast<const std::uintptr_t*>(self);
    const auto client = reinterpret_cast<std::uintptr_t>(GetModuleHandleA("client.dll"));
    char line[900]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] arms-collapse REFUSED instance %p (#%d): %s. NOTHING is written into it. vptr %p %s the "
        "C_ViewmodelAttachmentModel vtable.\n",
        reinterpret_cast<const void*>(self), g_witnessedCount, reason, reinterpret_cast<void*>(vptr),
        (client && vptr == client + kAttachmentVtableRva) ? "IS" : "is NOT");
    Tf2VrLog(line);
    std::snprintf(line, sizeof(line), "[TF2VR]   bone-count sources: %s\n", why[0] ? why : "(not read)");
    Tf2VrLog(line);
    ScanForBonePointerFields(self, -1, "REFUSED");
}

// ---------------------------------------------------------------------------
// THE DESTINATION, BY REFERENCE. 2026-09-03, runs 2EF203DD (third and fourth).
//
// +0x1260 was never a field. The BONE-POINTER SCAN found, on every instance
// the interceptor was handed -- the pilot arms at load, the pilot arms after a
// titan exit, the titan's own 76-bone attachment -- two fields that agree:
//
//     instance+0x1008  ->  the bone array      (checked by +0x1188, same value)
//
// and at that address exactly the model's numbones of bone matrices begin. On
// the load-time instance the pointer happens to be self+0x1260, which is the
// coincidence the walk lived on for two weeks; after a titan exit it points
// elsewhere, and the walk read fifty slots of garbage, or ran off the end of a
// mapped region and faulted INSIDE the interceptor (titanfall2vr.dll+0x7B80,
// LooksLikeBoneMatrix), after which the game hung ten seconds and died.
//
// So all three of the walk's jobs are replaced, and each by the engine's own
// datum rather than a heuristic over memory:
//   the BOUND        = numbones from the studiohdr SetupBones was handed (rdx),
//                      capped at the 128 the caller's own buffers hold;
//   the DESTINATION  = *(instance+0x1008), required to equal *(instance+0x1188),
//                      to lie in a committed WRITABLE region big enough for the
//                      bound, and to begin with bone-shaped matrices when first
//                      seen (a fresh buffer is pristine);
//   the LATCH        = per instance and per buffer address: once validated, the
//                      same (instance, pointer) keeps being written even though
//                      our own zeros no longer look like bones; a pointer change
//                      (the vector reallocated) is a fresh pristine buffer and
//                      revalidates.
// EVERY READ IS BOUNDED BY VirtualQuery FIRST. No read here may fault: a fault
// in this frame is not recoverable, so it is also LATCHED -- one fault disarms
// the collapse for the rest of the session and AdvanceArmsCollapse says so.
// ---------------------------------------------------------------------------
constexpr std::size_t kBonePtrFieldA = 0x1008;
constexpr std::size_t kBonePtrFieldB = 0x1188;
constexpr std::size_t kObjectBytesNeeded = kBonePtrFieldB + 8;
constexpr int kMaxWrittenBones = 128;  // FH3 section 2: the caller's buffers are 128 deep

std::uint64_t g_armsBonesTargetInstance = 0;
std::uint64_t g_armsBonesTarget = 0;
int g_armsBonesTargetCount = 0;
std::uint8_t g_armsBonesFaulted = 0;
std::uint64_t g_armsBonesValidations = 0;

// Bytes from p to the end of its committed, non-guard, readable (or writable)
// region; 0 if p is not such memory. The bound every read and write here uses.
std::size_t BytesFrom(const void* p, bool needWrite) {
    if (!p) return 0;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    const DWORD prot = mbi.Protect & 0xFF;
    const bool writable = prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE;
    const bool readable = writable || prot == PAGE_READONLY || prot == PAGE_EXECUTE_READ;
    if (needWrite ? !writable : !readable) return 0;
    return reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize -
           reinterpret_cast<std::uintptr_t>(p);
}

// Resolves and checks the destination for `self`. On success fills ptr/count.
// On failure `reason` says which check refused. Reads only what BytesFrom has
// vouched for.
bool ValidateTarget(const std::uint8_t* self, std::uint64_t* ptrOut, int* countOut, bool pristineCheck,
                    char* reason, std::size_t reasonSize, char* why, std::size_t whySize) {
    const std::size_t objectBytes = BytesFrom(self, false);
    if (objectBytes < kObjectBytesNeeded) {
        std::snprintf(reason, reasonSize, "object readable for only %zu bytes, need 0x%zX", objectBytes,
                      kObjectBytesNeeded);
        return false;
    }
    const auto a = *reinterpret_cast<const std::uint64_t*>(self + kBonePtrFieldA);
    const auto b = *reinterpret_cast<const std::uint64_t*>(self + kBonePtrFieldB);
    if (!a || a != b) {
        std::snprintf(reason, reasonSize, "bone-pointer fields disagree: +0x1008=%p +0x1188=%p",
                      reinterpret_cast<void*>(a), reinterpret_cast<void*>(b));
        return false;
    }
    const int count = RealBoneCount(self, why, whySize);
    if (count <= 0) {
        std::snprintf(reason, reasonSize, "model bone count unreadable (%d)", count);
        return false;
    }
    if (count > kMaxWrittenBones) {
        std::snprintf(reason, reasonSize, "model says %d bones, more than the 128 the draw buffers hold",
                      count);
        return false;
    }
    const std::size_t need = static_cast<std::size_t>(count) * kMatrixBytes;
    const std::size_t have = BytesFrom(reinterpret_cast<const void*>(a), true);
    if (have < need) {
        std::snprintf(reason, reasonSize, "target %p has %zu writable bytes, need %zu for %d bones",
                      reinterpret_cast<void*>(a), have, need, count);
        return false;
    }
    if (pristineCheck) {
        const auto* m = reinterpret_cast<const float*>(a);
        if (!LooksLikeBoneMatrix(m) || !LooksLikeBoneMatrix(m + 12)) {
            std::snprintf(reason, reasonSize, "target %p does not begin with bone-shaped matrices",
                          reinterpret_cast<void*>(a));
            return false;
        }
    }
    *ptrOut = a;
    *countOut = count;
    return true;
}

// ---------------------------------------------------------------------------
// BONE TABLE DUMP -- read-only, once per studiohdr, logged as a family.
//
// Rung 1 of the hands front (docs/KICKOFF-HANDS-2026-09-04.md): which bones
// are the hand chains? Every read is bounded by BytesFrom; a record or name
// that runs off committed memory stops the dump with a reason, never a fault.
// The lists it prints are the keep-lists rung 2 flattens around.
// ---------------------------------------------------------------------------
std::uint64_t g_boneDumpedHdrs[kBoneDumpMaxModels]{};
int g_boneDumpedCount = 0;
// True until the first table has been dumped this session. While true the
// interceptor arms even when the collapse is NOT wanted (dump-only mode, no
// writes), so a flat run with the body drawn still produces the table.
bool g_boneDumpPending = true;
std::uint64_t g_boneDumpRefusals = 0;
std::uint64_t g_armsBonesDumpOnlyFrames = 0;

// Static because the in-flight guard keeps this to one caller at a time, and
// 160 x 40 bytes is more than a hook should put on the engine's stack.
char s_boneNames[kMaxCacheBones][kBoneNameCap];
int s_boneParents[kMaxCacheBones];

// THE KEEP-LISTS, one set per dumped model. Hands mode flattens every bone
// whose keep bit is clear. anchorFrom[b] >= 0 sends flattened bone b to THAT
// bone's translation instead of the root anchor (the forearm to its wrist).
// Valid only when the table's positive control passed.
bool s_keep[kBoneDumpMaxModels][kMaxCacheBones];
int s_anchorFrom[kBoneDumpMaxModels][kMaxCacheBones];
// Hands mode, entity array: collapsed bone b copies the matrix of kept bone
// rigidTo[b] instead of going NaN (the forearm twist follows its wrist).
int s_rigidTo[kBoneDumpMaxModels][kMaxCacheBones];
bool s_keepValid[kBoneDumpMaxModels];
int s_keepCount[kBoneDumpMaxModels];
int g_keepSlot = -1;  // the dumped model the current target belongs to
std::uint64_t g_armsBonesHandsFrames = 0;

// Is this bone's name side-prefixed (def_l_, def_r_, ja_l_, ja_r_ ...)? The
// arm bones are; the spine, hips and head are "_c_".
bool IsSideBone(const char* name) {
    for (const char* p = name; p[0] && p[1] && p[2]; ++p) {
        if (p[0] == '_' && (p[1] == 'l' || p[1] == 'L' || p[1] == 'r' || p[1] == 'R') && p[2] == '_') return true;
    }
    return false;
}

bool IsLeftBone(const char* name) {
    for (const char* p = name; p[0] && p[1] && p[2]; ++p) {
        if (p[0] == '_' && (p[1] == 'l' || p[1] == 'L') && p[2] == '_') return true;
    }
    return false;
}

// The highest ancestor of `bone` (itself included) that is still a side bone:
// for a wrist that is the clavicle, the root of the whole arm.
int ArmRootOf(int bone, int numbones) {
    int best = -1;
    int b = bone;
    for (int steps = 0; b >= 0 && b < numbones && steps <= numbones; ++steps) {
        if (!IsSideBone(s_boneNames[b])) break;
        best = b;
        b = s_boneParents[b];
    }
    return best;
}

// Does the ancestor walk from `bone` pass through `root` (root included)?
// Bounded by the count, so a cyclic table cannot spin it.
bool InChain(int bone, int root, int numbones) {
    int b = bone;
    for (int steps = 0; b >= 0 && b < numbones && steps <= numbones; ++steps) {
        if (b == root) return true;
        b = s_boneParents[b];
    }
    return false;
}

// Copies a NUL-terminated printable string into out, byte by byte, each byte
// vouched for by BytesFrom. Returns the length (0 = unreadable or empty).
std::size_t ReadBoundedName(const char* name, char* out, std::size_t cap) {
    std::size_t i = 0;
    for (; i + 1 < cap; ++i) {
        if (BytesFrom(name + i, false) < 1) break;
        const char c = name[i];
        if (c == '\0') break;
        out[i] = (c >= 0x20 && c < 0x7F) ? c : '.';
    }
    out[i] = '\0';
    return i;
}

bool NameIs(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const char ca = (*a >= 'a' && *a <= 'z') ? static_cast<char>(*a - 32) : *a;
        const char cb = (*b >= 'a' && *b <= 'z') ? static_cast<char>(*b - 32) : *b;
        if (ca != cb) return false;
        if (ca == '\0') return true;
    }
}

// The studiohdr SetupBones itself reads ([rdx+8]), else the entity route.
const std::uint8_t* ResolveStudioHdr(const std::uint8_t* entity) {
    __try {
        if (g_armsBonesModel && BytesFrom(reinterpret_cast<const void*>(g_armsBonesModel + 8), false) >= 8) {
            const auto* s = *reinterpret_cast<const std::uint8_t* const*>(g_armsBonesModel + 8);
            if (s && BytesFrom(s, false) >= 4 && *reinterpret_cast<const std::uint32_t*>(s) == kIdstMagic) return s;
        }
        if (entity && BytesFrom(entity + kStudioHdrPtrOffset, false) >= 8) {
            const auto* s = *reinterpret_cast<const std::uint8_t* const*>(entity + kStudioHdrPtrOffset);
            if (s && BytesFrom(s, false) >= 4 && *reinterpret_cast<const std::uint32_t*>(s) == kIdstMagic) return s;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

// Every bone whose ancestor walk passes through `root` (root included).
// Prints the list and returns its size. Parents precede children in a Source
// skeleton, so the walk is bounded by the count and refuses a cycle.
int PrintChain(const char* label, int root, int numbones) {
    char line[900];
    int n = std::snprintf(line, sizeof(line), "[TF2VR] bone-table: keep-list %s (root %d \"%s\"):", label, root,
                          root >= 0 ? s_boneNames[root] : "?");
    int members = 0;
    for (int i = 0; i < numbones && root >= 0; ++i) {
        if (!InChain(i, root, numbones)) continue;
        ++members;
        if (n < static_cast<int>(sizeof(line)) - 16) n += std::snprintf(line + n, sizeof(line) - n, " %d", i);
    }
    std::snprintf(line + n, sizeof(line) - n, "  (%d bones)\n", members);
    Tf2VrLog(line);
    return members;
}

void PrintAncestry(const char* label, int bone, int numbones) {
    char line[900];
    int n = std::snprintf(line, sizeof(line), "[TF2VR] bone-table: %s ancestry:", label);
    int b = bone;
    for (int steps = 0; b >= 0 && b < numbones && steps <= numbones && n < static_cast<int>(sizeof(line)) - 80; ++steps) {
        n += std::snprintf(line + n, sizeof(line) - n, " %d \"%s\" <-", b, s_boneNames[b]);
        b = s_boneParents[b];
    }
    std::snprintf(line + n, sizeof(line) - n, " root\n");
    Tf2VrLog(line);
}

void DumpBoneTableOnce(const std::uint8_t* entity) {
    const std::uint8_t* studio = ResolveStudioHdr(entity);
    char line[400];
    if (!studio) {
        ++g_boneDumpRefusals;
        Tf2VrLog("[TF2VR] bone-table: REFUSED -- no readable IDST studiohdr via model-arg or entity+0x1208.\n");
        return;
    }
    const auto key = reinterpret_cast<std::uint64_t>(studio);
    for (int i = 0; i < g_boneDumpedCount; ++i) {
        if (g_boneDumpedHdrs[i] == key) { g_keepSlot = i; return; }  // already dumped this model
    }
    if (g_boneDumpedCount >= kBoneDumpMaxModels) { g_keepSlot = -1; return; }
    const int slot = g_boneDumpedCount++;
    g_boneDumpedHdrs[slot] = key;
    g_keepSlot = slot;
    s_keepValid[slot] = false;
    s_keepCount[slot] = 0;

    __try {
        if (BytesFrom(studio, false) < kStudioHdrBytesNeeded) {
            ++g_boneDumpRefusals;
            std::snprintf(line, sizeof(line), "[TF2VR] bone-table: REFUSED -- studiohdr %p readable for fewer than 0x%zX bytes.\n",
                          reinterpret_cast<const void*>(studio), kStudioHdrBytesNeeded);
            Tf2VrLog(line);
            return;
        }
        const int numbones = *reinterpret_cast<const int*>(studio + kStudioNumBonesOffset);
        const int boneindex = *reinterpret_cast<const int*>(studio + kStudioBoneIndexOffset);
        char modelName[64];
        ReadBoundedName(reinterpret_cast<const char*>(studio + kStudioNameOffset), modelName, sizeof(modelName));
        std::snprintf(line, sizeof(line),
                      "[TF2VR] bone-table: studiohdr %p model \"%s\" numbones %d boneindex 0x%X stride 0x%zX "
                      "(client.dll+0x0EEC06 imul 0xF4; parent at +4).\n",
                      reinterpret_cast<const void*>(studio), modelName, numbones, boneindex, kBoneRecordStride);
        Tf2VrLog(line);
        // Read-only: the texture, skin and bodypart->model->mesh tables of the
        // same header, once per model (mesh_census.cpp, proven offsets).
        DumpMeshCensusOnce(studio);
        if (numbones <= 0 || numbones > kMaxCacheBones || boneindex <= 0 || boneindex > (16 << 20)) {
            ++g_boneDumpRefusals;
            Tf2VrLog("[TF2VR] bone-table: REFUSED -- numbones/boneindex out of range; table not walked.\n");
            return;
        }
        int read = 0;
        int parentViolations = 0;
        for (int i = 0; i < numbones; ++i) {
            const std::uint8_t* record = studio + boneindex + static_cast<std::size_t>(i) * kBoneRecordStride;
            if (BytesFrom(record, false) < kBoneRecordStride) {
                ++g_boneDumpRefusals;
                std::snprintf(line, sizeof(line), "[TF2VR] bone-table: STOPPED at bone %d -- record %p not readable for 0x%zX bytes.\n",
                              i, reinterpret_cast<const void*>(record), kBoneRecordStride);
                Tf2VrLog(line);
                break;
            }
            const int nameIndex = *reinterpret_cast<const int*>(record + kBoneRecordNameIndex);
            const int parent = *reinterpret_cast<const int*>(record + kBoneRecordParent);
            s_boneParents[i] = parent;
            std::snprintf(s_boneNames[i], kBoneNameCap, "?");
            if (nameIndex != 0 &&
                ReadBoundedName(reinterpret_cast<const char*>(record) + nameIndex, s_boneNames[i], kBoneNameCap) == 0) {
                std::snprintf(s_boneNames[i], kBoneNameCap, "?");
            }
            if (parent < -1 || parent >= i) ++parentViolations;
            std::snprintf(line, sizeof(line), "[TF2VR] BONES[%d] \"%s\" parent %d\n", i, s_boneNames[i], parent);
            Tf2VrLog(line);
            ++read;
        }
        if (read < numbones) return;

        // POSITIVE CONTROL. The gun's own viewmodel lists its R_HAND / L_HAND
        // attachments on bones 72 / 71 (docs/RESULT-GUN-ALIGN-2026-09-03.md);
        // the arms model's table (run 2026-09-04, flat) names those bones
        // ja_r_propHand / ja_l_propHand, hanging off def_r_wrist (44) and
        // def_l_wrist (20). The hand chain is the wrist and every descendant:
        // the fingers and the propHand. A table that fails this is a wrong
        // stride or offset and its keep-lists must not be used.
        const bool rOk = numbones > 72 && NameIs(s_boneNames[72], "ja_r_propHand");
        const bool lOk = numbones > 71 && NameIs(s_boneNames[71], "ja_l_propHand");
        int rWrist = -1, lWrist = -1;
        for (int i = 0; i < numbones; ++i) {
            if (rWrist < 0 && NameIs(s_boneNames[i], "def_r_wrist")) rWrist = i;
            if (lWrist < 0 && NameIs(s_boneNames[i], "def_l_wrist")) lWrist = i;
        }
        const bool pass = rOk && lOk && numbones == 74 && parentViolations == 0 && rWrist >= 0 && lWrist >= 0;
        std::snprintf(line, sizeof(line),
                      "[TF2VR] bone-table CONTROL %s: bone 72 \"%s\" (want ja_r_propHand) %s, bone 71 \"%s\" "
                      "(want ja_l_propHand) %s, count %d (want 74) %s, parent-order violations %d; "
                      "def_r_wrist at %d, def_l_wrist at %d.\n",
                      pass ? "PASS" : "FAIL",
                      numbones > 72 ? s_boneNames[72] : "?", rOk ? "ok" : "MISMATCH",
                      numbones > 71 ? s_boneNames[71] : "?", lOk ? "ok" : "MISMATCH",
                      numbones, numbones == 74 ? "ok" : "MISMATCH", parentViolations, rWrist, lWrist);
        Tf2VrLog(line);
        if (rWrist >= 0) { PrintAncestry("def_r_wrist", rWrist, numbones); PrintChain("R", rWrist, numbones); }
        if (lWrist >= 0) { PrintAncestry("def_l_wrist", lWrist, numbones); PrintChain("L", lWrist, numbones); }

        // THE KEEP-LIST for hands mode, only from a table that passed.
        if (pass) {
            int kept = 0;
            for (int i = 0; i < numbones; ++i) {
                s_keep[slot][i] = InChain(i, rWrist, numbones) || InChain(i, lWrist, numbones);
                s_anchorFrom[slot][i] = -1;
                if (s_keep[slot][i]) ++kept;
            }
            // THE SEAM. The wrist's parent is the ELBOW (def_r_elbow 42), and
            // the forearm (def_r_forearm 43) is the elbow's OTHER child -- a
            // twist bone that skins the forearm mesh, not an ancestor of the
            // hand. Run 2026-09-04 (flat, first hands build) anchored only the
            // wrist's parent and the forearm-skinned vertices trailed off the
            // bottom of the screen toward the root anchor. So every collapsed
            // bone in the elbow's subtree -- elbow, forearm twist, anything
            // else under it -- collapses onto the wrist instead.
            //
            // Second flat run, same day: elbow + forearm on the wrist left
            // "artifacts where the arms are". The upper forearm is skinned to
            // elbowB and the shoulder twists, which sit ABOVE the elbow in
            // this table (def_l_elbowB 16 is a child of the shoulder), so they
            // still went to the feet. So the seam is the WHOLE ARM: every
            // collapsed bone under the arm's root -- the highest ancestor of
            // the wrist that still carries the side prefix (def_r_clav 37 /
            // def_l_clav 13; the next one up is def_c_spineC). Body bones keep
            // the root anchor.
            //
            // Third flat run (screenshot crop-br-9, 2026-09-04): whole arm on
            // the wrist, body on the root -- diagonal strings of beads at the
            // edge of the view. THREE collapse points is the defect: a vertex
            // blended across two groups lands on the segment between them, and
            // where three groups meet the triangles lie in the plane of the
            // three points and have AREA. With exactly TWO collapse points
            // every boundary triangle is collinear and rasterises to nothing;
            // the only non-degenerate triangles left are the cuff ring blended
            // with the kept wrist, and those sit on the glove because the
            // collapse point IS the wrist. So: every collapsed left-side bone
            // -> left wrist, everything else (right arm, torso, head, legs) ->
            // right wrist. The root anchor is not used in hands mode.
            //
            // CORRECTION, same day: the "beaded strings" that motivated the
            // two-point rule were the gun range's WEAPON RACK on the wall,
            // seen edge-on with its outline shader (headset F6 capture,
            // crop vr-edge.png). Not ours. The two-point anchors are kept
            // only for the caller's finite stack copies; the entity array,
            // which is what draws, uses NanBufferMasked above.
            const int lArm = ArmRootOf(lWrist, numbones);
            int toL = 0, toR = 0, rigid = 0;
            for (int i = 0; i < numbones; ++i) {
                s_rigidTo[slot][i] = -1;
                if (s_keep[slot][i]) continue;
                const bool left = (lArm >= 0 && InChain(i, lArm, numbones)) || IsLeftBone(s_boneNames[i]);
                s_anchorFrom[slot][i] = left ? lWrist : rWrist;
                if (left) ++toL; else ++toR;
                // The forearm twist bones (siblings of the wrists under the
                // elbows) follow their wrist rigidly so the glove stays whole.
                if (NameIs(s_boneNames[i], "def_r_forearm")) { s_rigidTo[slot][i] = rWrist; ++rigid; }
                if (NameIs(s_boneNames[i], "def_l_forearm")) { s_rigidTo[slot][i] = lWrist; ++rigid; }
            }
            s_keepCount[slot] = kept;
            s_keepValid[slot] = true;
            std::snprintf(line, sizeof(line),
                          "[TF2VR] bone-table: KEEP-LIST VALID for slot %d: %d of %d bones kept in hands mode; "
                          "%d forearm bones RIGID with their wrist; the other %d collapse to two points: "
                          "%d left-side -> wrist %d, %d others -> wrist %d (all three write sites).\n",
                          slot, kept, numbones, rigid, numbones - kept - rigid, toL, lWrist, toR, rWrist);
            Tf2VrLog(line);
        } else {
            Tf2VrLog("[TF2VR] bone-table: control FAILED, so hands mode has NO keep-list for this model and "
                     "collapses every bone.\n");
        }
        // 2026-09-06: keep dumping until every arms model has been walked, not
        // just the first. There are TWO pilot arms models -- pov_mlt_hero_jack
        // _rifleman.mdl and pov_mlt_hero_jack.mdl -- and the second one is what
        // draws after a titan exit. Stopping at the first left it unfiltered and
        // the wearer saw a full body. Dump-only: this arms the interceptor to
        // READ the table, it writes no bone.
        g_boneDumpPending = (g_boneDumpedCount < kBoneDumpMaxModels);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_boneDumpRefusals;
        Tf2VrLog("[TF2VR] bone-table: FAULTED while reading the table (caught); no further dumps this model.\n");
    }
}

// THE POST-PROCESS. Runs immediately after SetupBones returned, with the
// buffers it filled still live in the caller's frame.
void CollapseArmsBones() {
    auto* first = reinterpret_cast<std::uint8_t*>(g_armsBonesArray);
    auto* third = reinterpret_cast<std::uint8_t*>(g_armsBonesArray2);
    if (!first) return;
    __try {
        auto* self = reinterpret_cast<std::uint8_t*>(g_armsBonesInstance);
        bool haveTarget = false;
        if (self) {
            // Same instance, same buffer as the latch: keep writing. Anything
            // else -- a new instance, or the same instance's vector moved --
            // is a pristine buffer and must validate before a single write.
            const std::size_t objectBytes =
                (g_armsBonesInstance == g_armsBonesTargetInstance) ? kObjectBytesNeeded : BytesFrom(self, false);
            std::uint64_t current = 0;
            if (objectBytes >= kObjectBytesNeeded) {
                current = *reinterpret_cast<const std::uint64_t*>(self + kBonePtrFieldA);
            }
            if (g_armsBonesInstance == g_armsBonesTargetInstance && current != 0 && current == g_armsBonesTarget) {
                haveTarget = true;
            } else {
                char reason[240]{};
                char why[420]{};
                std::uint64_t ptr = 0;
                int count = 0;
                ++g_armsBonesValidations;
                if (ValidateTarget(self, &ptr, &count, true, reason, sizeof(reason), why, sizeof(why))) {
                    const bool moved = g_armsBonesInstance == g_armsBonesTargetInstance;
                    g_armsBonesTargetInstance = g_armsBonesInstance;
                    g_armsBonesTarget = ptr;
                    g_armsBonesTargetCount = count;
                    g_armsBonesCacheAccepted = count;
                    g_armsBonesCountedFor = g_armsBonesInstance;
                    g_armsBonesRealCount = count;
                    haveTarget = true;
                    char line[700]{};
                    std::snprintf(line, sizeof(line),
                        "[TF2VR] arms collapse TARGET for instance %p%s: bones at %p (+0x1008, agreed by "
                        "+0x1188), model says %d bones, %s self+0x1260. Writing %d matrices by reference.\n",
                        reinterpret_cast<void*>(g_armsBonesInstance), moved ? " (buffer MOVED)" : "",
                        reinterpret_cast<void*>(ptr), count,
                        ptr == g_armsBonesInstance + kAttachmentBoneArrayOffset ? "which IS" : "which is NOT",
                        count);
                    Tf2VrLog(line);
                    std::snprintf(line, sizeof(line), "[TF2VR]   bone-count sources: %s\n", why);
                    Tf2VrLog(line);
                    // Read-only: names and parents out of the same studiohdr
                    // the count came from, once per model.
                    DumpBoneTableOnce(self);
                } else {
                    ++g_armsBonesUnvalidatedSkips;
                    WitnessUnvalidatedInstance(self, reason, why);
                }
            }
        }

        // DUMP-ONLY MODE: the interceptor was armed only to read the bone
        // table (collapse not wanted from the ini). Nothing is written.
        if (!g_wanted) {
            ++g_armsBonesDumpOnlyFrames;
            ++g_armsBonesCollapses;
            return;
        }

        // The anchor every bone collapses onto: bone 0's own translation, read
        // from the validated target before anything is written. Translation
        // only -- after the first frame the rotation rows are our own zeros.
        float ax = 0.0f, ay = 0.0f, az = 0.0f;
        if (haveTarget) {
            const auto* bone0 = reinterpret_cast<const float*>(g_armsBonesTarget);
            if (TranslationLooksPlausible(bone0)) {
                ax = bone0[3];
                ay = bone0[7];
                az = bone0[11];
            }
        }

        // HANDS MODE (body.show = 1): flatten everything EXCEPT the two hand
        // chains, in all three write sites. Needs the keep-list of the model
        // the target belongs to; without one it says so once and collapses
        // every bone, which is exactly weapon-only.
        const bool handsMode = g_bodyMode == 1;
        const bool* keep = nullptr;
        const int* anchorFrom = nullptr;
        if (handsMode && haveTarget && g_keepSlot >= 0 && s_keepValid[g_keepSlot]) {
            keep = s_keep[g_keepSlot];
            anchorFrom = s_anchorFrom[g_keepSlot];
        } else if (handsMode && haveTarget && !g_handsFallbackSaid) {
            g_handsFallbackSaid = true;
            Tf2VrLog("[TF2VR] arms-collapse: hands mode wanted but this model has NO valid keep-list "
                     "(bone-table control failed or not dumped); collapsing ALL bones instead.\n");
        }
        const float* targetRead = haveTarget ? reinterpret_cast<const float*>(g_armsBonesTarget) : nullptr;
        const int targetCount = haveTarget ? g_armsBonesTargetCount : 0;

        const int* rigidTo = keep ? s_rigidTo[g_keepSlot] : nullptr;
        HandsBufferMasked(first, kBonesPerBuffer, keep, rigidTo, anchorFrom, targetRead, targetCount, ax, ay, az);
        if (third) HandsBufferMasked(third, kBonesPerBuffer, keep, rigidTo, anchorFrom, targetRead, targetCount, ax, ay, az);
        // The middle buffer is never handed to us in a register -- it is the
        // caller's third array and only 0x0ABAA0 receives it. Reach it by
        // layout, but ONLY when the layout is the one that was read out of the
        // disassembly. If the two pointers are not 0x1800 apart this is some
        // other call site and guessing an address between them would be a
        // write into an unknown stack slot.
        if (third == first + kExpectedSpan) {
            HandsBufferMasked(first + kBufferBytes, kBonesPerBuffer, keep, rigidTo, anchorFrom, targetRead, targetCount,
                              ax, ay, az);
        }

        // AND THE ENTITY'S OWN BONE ARRAY, which is what the draw reads
        // (FH3 section 2: suppressing SetupBones moved the arms; flattening the
        // stack copies alone changed nothing). Kept bones (the wrists the
        // forearms anchor to) are never written, so reading anchors from the
        // same array while writing it is safe.
        if (haveTarget) {
            auto* target = reinterpret_cast<std::uint8_t*>(g_armsBonesTarget);
            if (keep) {
                // Hands mode, finite: two wrist points, forearm rigid with
                // its wrist (HandsBufferMasked above).
                HandsBufferMasked(target, g_armsBonesTargetCount, keep, rigidTo, anchorFrom, targetRead, targetCount,
                                  ax, ay, az);
                ++g_armsBonesHandsFrames;
            } else {
                FlattenBufferMasked(target, g_armsBonesTargetCount, nullptr, nullptr, targetRead, targetCount, ax, ay, az);
            }
            ++g_armsBonesCacheWrites;
        }
        ++g_armsBonesCollapses;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Not recoverable in this frame (run 2EF203DD: a caught fault here was
        // followed by a ten-second hang and a fatal crash). Disarm AND latch,
        // so AdvanceArmsCollapse never re-arms this session.
        g_armsBonesArmed = 0;
        g_armsBonesFaulted = 1;
    }
}
}  // extern "C"

bool InstallArmsCollapse() {
    if (g_slot) return true;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return false;
    auto* base = reinterpret_cast<std::uint8_t*>(client);

    if (std::memcmp(base + kSetupBonesRva, kSetupBonesPrologue, sizeof(kSetupBonesPrologue)) != 0) {
        Tf2VrLog("[TF2VR] arms-collapse: SetupBones prologue does not match this build. "
                 "Refusing.\n");
        return false;
    }

    auto** slot = reinterpret_cast<void**>(base + kAttachmentVtableRva + kSetupBonesSlotOffset);
    auto* expected = reinterpret_cast<void*>(base + kSetupBonesRva);
    if (*slot != expected) {
        char line[224];
        std::snprintf(line, sizeof(line),
                      "[TF2VR] arms-collapse: vtable slot %d holds %p, expected %p. Refusing.\n",
                      kSetupBonesSlot, *slot, expected);
        Tf2VrLog(line);
        return false;
    }

    g_armsBonesOriginal = reinterpret_cast<std::uint64_t>(expected);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        Tf2VrLog("[TF2VR] arms-collapse: VirtualProtect failed on the vtable slot.\n");
        return false;
    }
    *slot = reinterpret_cast<void*>(&armsBonesInterceptor);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    g_slot = slot;
    Tf2VrLog("[TF2VR] arms-collapse: interceptor installed on arms vtable slot 205 "
             "(client.dll+0x0EE950 SetupBones).\n");
    return true;
}

void RemoveArmsCollapse() {
    g_armsBonesArmed = 0;
    if (!g_slot) return;
    if (*g_slot == reinterpret_cast<void*>(&armsBonesInterceptor)) {
        DWORD old = 0;
        if (VirtualProtect(g_slot, sizeof(void*), PAGE_READWRITE, &old)) {
            *g_slot = reinterpret_cast<void*>(g_armsBonesOriginal);
            DWORD ignored = 0;
            VirtualProtect(g_slot, sizeof(void*), old, &ignored);
        }
    }
    g_slot = nullptr;
}

bool IsArmsCollapseArmed() { return g_armsBonesArmed != 0; }

void ToggleArmsCollapse() {
    if (g_armsBonesArmed) {
        g_armsBonesArmed = 0;
        char line[224];
        std::snprintf(line, sizeof(line),
                      "[TF2VR] arms-collapse: DISARMED. %llu frames were collapsed. The arms "
                      "should be back.\n",
                      static_cast<unsigned long long>(g_armsBonesCollapses));
        Tf2VrLog(line);
        return;
    }

    if (!InstallArmsCollapse()) return;

    // NOTHING TO RESOLVE. The interceptor is handed the object the engine is
    // drawing, in rcx, on the only clock where its bone cache is populated.
    // Every attempt to identify it from here instead cost a round of the body
    // coming back: the handle table's first entry can be a pooled attachment
    // that never animates, and checking the bone cache from the plugin frame
    // reads it while it is empty, so nothing is ever chosen at all.
    g_armsBonesInstance = 0;
    g_armsBonesCountedFor = 0;
    // A fresh arm starts with NO count, exactly as the very first arm of a
    // session always has. Carrying the previous instance's count across a
    // re-arm is what wrote 74 matrices into an unvalidated cache after the
    // titan exit (see the write gate in CollapseArmsBones).
    g_armsBonesCacheAccepted = 0;
    g_armsBonesCacheWrites = 0;
    g_armsBonesUnvalidatedSkips = 0;
    g_armsBonesValidations = 0;
    g_witnessedCount = 0;
    // The by-reference latch starts empty too: the next call validates.
    g_armsBonesTargetInstance = 0;
    g_armsBonesTarget = 0;
    g_armsBonesTargetCount = 0;
    g_armsBonesCollapses = 0;
    g_armsBonesInFlight = 0;
    g_armsBonesArmed = 1;

    Tf2VrLog(!g_wanted ? "[TF2VR] arms-collapse: ARMED (DUMP-ONLY). The interceptor reads the bone table of the "
                         "first validated arms model and writes nothing; it removes itself afterwards.\n"
             : g_bodyMode == 1
                 ? "[TF2VR] arms-collapse: ARMED (body.show = 1, weapon + hands). Every arms bone outside the "
                   "two hand chains is collapsed after SetupBones runs.\n"
                 : "[TF2VR] arms-collapse: ARMED (body.show = 0, weapon only). Every C_ViewmodelAttachmentModel "
                   "the engine draws gets its bones collapsed after SetupBones runs.\n");
}

void AdvanceArmsCollapse() {
    // ARM ITSELF WHEN THE INI ASKED FOR IT, and then get out of the way.
    //
    // There is no instance to track, re-validate or re-resolve any more: the
    // interceptor is handed the drawn object in rcx every call. Everything
    // this function used to do about instances was an attempt to answer a
    // question the engine was already answering, and it got it wrong four
    // times -- pooled attachments that never animate, and a bone cache read
    // from the plugin frame where it is empty.
    // ---- TITAN STATE OWNS THE ARMS. 2026-09-03 -----------------------------
    //
    // In a titan the wearer IS the cockpit: the body and arms belong on screen,
    // and the collapse has no business writing bones on a titan's model at all.
    // On foot they want them gone -- that is how this mod has been played for
    // weeks. So the collapse follows the state rather than the ini alone.
    //
    // This also removes the 2026-08-29 crash surface entirely rather than only
    // bounding it: the write never runs against a titan's model. The bound fix
    // above is still the real repair, because a wrong bound is a heap bug on
    // ANY model; this is the second line of defence and the feature at once.
    //
    // IsInTitanNow fails safe to "on foot", so a probe that never installed
    // leaves the long-standing behaviour exactly as it was.
    const bool inTitan = IsInTitanNow();
    if (inTitan && g_armsBonesArmed) {
        RemoveArmsCollapse();
        Tf2VrLog("[TF2VR] arms-collapse: STOOD DOWN -- in a titan, so the body and arms come back "
                 "and nothing writes bones on the titan's model. It re-arms on foot.\n");
        return;
    }
    // A FAULT IN THE POST-PROCESS LATCHES THE COLLAPSE OFF FOR THE SESSION.
    // Run 2EF203DD (fourth): the caught fault was followed by a ten-second
    // hang, a re-arm from this very loop, a second fault and a fatal crash.
    // Re-arming after a fault is how one bad read became a dead game.
    if (g_armsBonesFaulted) {
        static bool said = false;
        if (!said) {
            said = true;
            Tf2VrLog("[TF2VR] arms-collapse: FAULTED inside the post-process and LATCHED OFF for this "
                     "session. The body stays visible; nothing re-arms it. The crash recorder's "
                     "first-chance line above names the read.\n");
        }
        return;
    }
    // DUMP-ONLY ARM (2026-09-04): with the collapse not wanted, the hook still
    // installs once so the bone table can be read, and comes straight back out
    // once the table has been logged. No frame in that window writes a bone.
    const bool armFor = g_wanted || g_boneDumpPending;
    if (!inTitan && TitanStateSettled() && armFor && !g_armsBonesArmed) {
        static std::uint64_t lastTry = 0;
        const std::uint64_t tick = GetTickCount64();
        if (tick - lastTry >= 500) {
            lastTry = tick;
            ToggleArmsCollapse();
        }
    }
    if (!g_wanted && !g_boneDumpPending && g_armsBonesArmed) {
        // Either the dump-only window closed, or body.show was switched to
        // full body while armed. Both mean: hook out, engine's bones untouched.
        RemoveArmsCollapse();
        char done[300];
        std::snprintf(done, sizeof(done),
                      "[TF2VR] arms-collapse: STOOD DOWN (body.show = 2 / dump done); hook REMOVED after %llu "
                      "intercepted frames, %llu bone-array writes, %llu dump-only frames, %llu dump refusals.\n",
                      static_cast<unsigned long long>(g_armsBonesCollapses),
                      static_cast<unsigned long long>(g_armsBonesCacheWrites),
                      static_cast<unsigned long long>(g_armsBonesDumpOnlyFrames),
                      static_cast<unsigned long long>(g_boneDumpRefusals));
        Tf2VrLog(done);
        return;
    }
    if (!g_armsBonesArmed) return;

    // One line a second, so a run that does nothing says so rather than
    // leaving the wearer to guess whether it is armed.
    static std::uint64_t lastReport = 0;
    const std::uint64_t now = GetTickCount64();
    if (now - lastReport < 1000) return;
    lastReport = now;
    char line[520];
    const bool keepLive = g_keepSlot >= 0 && s_keepValid[g_keepSlot];
    std::snprintf(line, sizeof(line),
                  "[TF2VR] arms-collapse: armed (%s), %llu frames collapsed, %llu hands-mode frames (keep-list %s, "
                  "%d kept); target %p x%d bones for instance %p "
                  "(via +0x1008); bone-array writes %llu, validations %llu, refused %llu (current instance %p "
                  "%s).%s%s\n",
                  !g_wanted ? "DUMP-ONLY, no writes" : g_bodyMode == 1 ? "body.show=1 hands" : "body.show=0 weapon only",
                  static_cast<unsigned long long>(g_armsBonesCollapses),
                  static_cast<unsigned long long>(g_armsBonesHandsFrames),
                  keepLive ? "VALID" : "none", keepLive ? s_keepCount[g_keepSlot] : 0,
                  reinterpret_cast<void*>(g_armsBonesTarget), g_armsBonesTargetCount,
                  reinterpret_cast<void*>(g_armsBonesTargetInstance),
                  static_cast<unsigned long long>(g_armsBonesCacheWrites),
                  static_cast<unsigned long long>(g_armsBonesValidations),
                  static_cast<unsigned long long>(g_armsBonesUnvalidatedSkips),
                  reinterpret_cast<void*>(g_armsBonesInstance),
                  (g_armsBonesInstance && g_armsBonesInstance == g_armsBonesTargetInstance)
                      ? (g_wanted ? "VALIDATED, being written" : "VALIDATED, read only")
                      : "UNVALIDATED, not written",
                  g_armsBonesCollapses == 0
                      ? "  ZERO FRAMES -- the interceptor is not being reached; nothing you see "
                        "is it."
                      : "",
                  (g_wanted && g_armsBonesCollapses > 0 && g_armsBonesCacheWrites == 0)
                      ? "  ZERO CACHE WRITES -- no instance has validated, so nothing is being "
                        "collapsed and the body is visible."
                      : "");
    Tf2VrLog(line);
}


void SetBodyShowMode(int mode) {
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    g_bodyMode = mode;
    // 2026-09-05: mode 1 is the MESH filter (draw_item_census.cpp); the bone collapse writes only in mode 0.
    // 2026-09-06: the mesh filter (vb_bind_census.cpp, ApplyGateFilter) now hides the gear, lower-
    // body and upper-body meshes at studiorender's own per-mesh gate, and the wearer confirmed it:
    // weapon and gloves only -- except the gauntlet mesh also carries the FOREARM to the elbow, and
    // no mesh-level lever can split one mesh.
    //
    // So hands mode comes back ON TOP of the filter, to collapse the forearm to its wrist. The
    // 09-04 verdicts on this are real but were all measured with ALL FIVE MESHES DRAWING: the black
    // bands were the BODY mesh's boundary tying the gloves together, and the cuff streaks ran from
    // glove to SLEEVE. Both of those meshes are now gone before they are ever bound, so the only
    // boundary left is inside the gauntlet itself, at the wrist -- where the collapse point also
    // sits. (NaN stays dead regardless: zero-weight padding slots make it eat the fingers.)
    //
    // If the wrist seam still draws a streak, that is the same partition-boundary law as 09-04 and
    // the mesh filter alone is the shipped answer.
    // WEAPON-ONLY (0) ONLY. Hands mode is NOT this module's job.
    //
    // On 2026-09-09 I read the DUMP-ONLY status line -- "0 hands-mode frames
    // (keep-list VALID, 36 kept)" -- as this module failing to do hands, and
    // changed it to `mode <= 1` so the bone collapse would run for body.show=1.
    // The wearer's verdict on the result: "it looks like a very old attempt we
    // made before we ended up on the glove/arms approach that works well." It
    // was, and this was the wrong module.
    //
    // Hands mode belongs to vb_bind_census: a per-mesh gate that writes 0 to
    // every mesh except the gloves, installed from AdvanceSceneReentry once a
    // world is ready. The bone collapse here is the older, coarser route and
    // must stay out of its way. The dump-only path for mode 1 is deliberate.
    g_wanted = mode == 0;
    g_handsFallbackSaid = false;
    char line[240];
    std::snprintf(line, sizeof(line),
                  "[TF2VR] body.show = %d (%s). %s\n", mode,
                  mode == 0 ? "weapon only" : mode == 1 ? "weapon + hands" : "weapon + full body",
                  mode == 2 ? "No collapse; the SetupBones hook stands down if armed."
                            : "The collapse arms itself as soon as the arms entity exists and applies on the "
                              "next SetupBones; it stands down in a titan.");
    Tf2VrLog(line);
}

int BodyShowMode() { return g_bodyMode; }
