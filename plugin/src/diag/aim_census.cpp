#include "aim_census.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "aim_cmd.h"
#include "camera_hook.h"
#include "camera_update_hook.h"
#include "engine_cvars.h"
#include "render_resolution.h"
#include "diagnostics.h"
#include "ads_lock.h"
#include "ads_zoom.h"
#include "player_eye.h"
#include "player_eye_hook.h"
#include "viewmodel_instance.h"

// The camera offset the detour adds to the game's own eye on the render frame:
// tracked delta + neck model, clamped. Its z is how far the rendered eye sits
// above the point the round actually leaves from.
extern "C" volatile std::uint32_t g_cameraOffsetZBits;
extern "C" volatile std::uint8_t g_cameraPositionWriteActive;

namespace {

constexpr float kDegToRad = 3.14159265358979f / 180.0f;
constexpr float kRadToDeg = 180.0f / 3.14159265358979f;

float CameraRaiseUnits() {
    const std::uint32_t bits = g_cameraOffsetZBits;
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// THE PARALLAX, AS A NUMBER. A round leaves the game's eye, not the rendered
// one, so with the camera raised by R units a mark drawn where the aim
// direction projects sits above the impact by atan(R / distance). This is the
// prediction the wearer's "rounds land below the reticle" is compared against.
float ParallaxDegreesAt(float raiseUnits, float metres) {
    const float distanceUnits = metres * SourceUnitsPerMetre();
    if (distanceUnits <= 0.0f) return 0.0f;
    return std::atan(raiseUnits / distanceUnits) * kRadToDeg;
}

// ---------------------------------------------------------------------------
// THE SAMPLE CLOCK, AND WHY THE CAPS ARE PER STREAM
//
// One shared cap lets the cheap stream starve the expensive one -- the AIM
// stream runs every second and the CHAR stream every two, so a single budget
// would have spent itself on AIM before D1 had ten samples. Separate budgets,
// separate counters, and every line carries its own so a truncated stream is
// recognisable as truncated rather than as silence.
constexpr std::uint64_t kAimPeriodMs = 1000;
constexpr std::uint64_t kPosePeriodMs = 1000;
constexpr std::uint64_t kCharPeriodMs = 2000;
constexpr std::uint32_t kAimLineCap = 400;
constexpr std::uint32_t kPoseLineCap = 400;
constexpr std::uint32_t kCharLineCap = 200;

std::uint64_t g_nextAimMs = 0;
std::uint64_t g_nextPoseMs = 0;
std::uint64_t g_nextCharMs = 0;
std::uint32_t g_aimLines = 0;
std::uint32_t g_poseLines = 0;
std::uint32_t g_charLines = 0;
std::uint32_t g_sample = 0;
// The camera position the last census sample saw, so the ADS recorder can state
// the gun-to-camera distance without re-reading the latch on its own clock.
float g_lastCameraPos[3]{};


// ---------------------------------------------------------------------------
// THE POSE MAILBOX. Written on the XR threads, read on the plugin frame.
struct PoseMailbox {
    std::atomic_uint32_t generation{0};
    std::atomic_uint64_t localUpdates{0};
    std::atomic_uint64_t projectionUpdates{0};
    std::atomic<float> localHead[3]{};
    std::atomic<float> origin[3]{};
    std::atomic<float> headCentre[3]{};
    std::atomic<float> declared[3]{};
    std::atomic_bool projectionInUse{false};
    std::atomic_bool originValid{false};
};
PoseMailbox g_pose;

// WHAT WE DECLARE TO THE COMPOSITOR. See the note in the header: this is the
// one structural difference between the headset that reads correctly and the one
// that does not, and it has only ever been inferred.
struct DeclaredMailbox {
    std::atomic<float> up{0.0f}, down{0.0f}, left{0.0f}, right{0.0f};
    std::atomic<int> rectX{0}, rectY{0}, rectW{0}, rectH{0};
    std::atomic_uint64_t updates{0};
};
DeclaredMailbox g_declared;

// ---------------------------------------------------------------------------
// D1 -- THE FACED CHARACTER'S OWN EYE HEIGHT
//
// Offsets from S1-ATTACHMENT-API-2026-08-26 section 6 and 7, every one of them
// read out of the disassembly rather than guessed, and from player_eye.cpp's
// identification of the origin.
//
// READ-ONLY BY CONSTRUCTION. The engine's own GetAttachment at 0xF4930 would
// be the tidier call, but it invokes the bone-setup virtual on an entity we do
// not own, which is a write to game state in everything but name. The table it
// COPIES FROM is a plain array of world matrices at a known offset, so this
// reads that instead and validates what it read.
constexpr std::uintptr_t kEntityListRva = 0xB0F030;
constexpr std::size_t kEntityListStride = 32;
constexpr std::size_t kEntityListEntityOffset = 8;
constexpr int kEntityListMax = 0x4000;
constexpr std::size_t kOriginOffset = 0x90;        // Vector, from player_eye's identification
constexpr std::size_t kStudioHdrPtrOffset = 0x1208;
constexpr std::size_t kAttachmentTablePtr = 0x11C0;
constexpr std::size_t kAttachmentCountOffset = 0x11D8;
constexpr std::size_t kAttachmentTableStride = 0x50;
constexpr std::size_t kStudioNumAttachments = 0xF4;
constexpr std::size_t kStudioAttachmentIndex = 0xF8;
constexpr std::size_t kStudioAttachmentStride = 0x5C;
constexpr std::uint32_t kIdstMagic = 0x54534449;   // 'IDST'

std::atomic_bool g_charDisarmed{false};
std::atomic_uint32_t g_charFaults{0};
double g_charWorstMs = 0.0;
// One list per model is plenty; a name list does not change between frames.
constexpr std::uint32_t kNameDumpCap = 6;
std::uint32_t g_namesDumped = 0;
std::uint32_t g_selfNamesDumped = 0;
std::uint32_t g_hullLines = 0;

bool ReadableFrom(const void* address, std::size_t bytes) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info)) return false;
    if (info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto regionEnd =
        reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return start + bytes <= regionEnd;
}

// A pointer that could plausibly be a live heap object, tested WITHOUT a
// syscall. The entity list is 16384 slots and a VirtualQuery on each would be
// 16384 kernel transitions per sample -- the per-item syscall that has hidden a
// stall in this project before.
bool PlausiblePointer(const void* p) {
    const auto value = reinterpret_cast<std::uintptr_t>(p);
    if (value < 0x10000) return false;
    if (value & 0x7) return false;
    return value < 0x7FFFFFFFFFFFull;
}

// THE FAULT IS THE EXPENSIVE OUTCOME, NOT THE SYSCALL.
//
// An SEH frame around the whole walk keeps a bad read from ending the process,
// but it also ends the walk and disarms D1 for the session -- so the run that
// was meant to answer D1 comes back with one fault line and no measurement.
// A per-entity VirtualQuery would prevent that and cost 16384 kernel
// transitions a sample.
//
// Entities live in a handful of large heap regions, so remembering the last
// region that answered gets both: the query happens when a pointer leaves the
// cached span, which after the first few entities is almost never.
struct RegionCache {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::uint32_t queries = 0;
};

bool RegionAllows(RegionCache& cache, const void* address, std::size_t bytes) {
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    if (start >= cache.begin && start + bytes <= cache.end) return true;
    MEMORY_BASIC_INFORMATION info{};
    ++cache.queries;
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info)) return false;
    if (info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    cache.begin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    cache.end = cache.begin + info.RegionSize;
    return start + bytes <= cache.end;
}

bool Finite3(const float v[3]) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

struct CharacterReading {
    bool found = false;
    float originZ = 0.0f;
    float distance = 0.0f;
    int attachmentCount = 0;
    bool studioOk = false;
    char attachmentName[32] = "";
    float attachmentZ = 0.0f;
    float topAttachmentZ = 0.0f;
    char topName[32] = "";
    int examined = 0;
    int withStudio = 0;
    int slots = 0;
    bool topValid = false;
    std::uint32_t regionQueries = 0;
};

// The name of attachment `index` (0-based) on this entity's model, or nullptr.
// Called at most 128 times, and only for the ONE entity that won the distance
// filter, so a VirtualQuery per hop is affordable here in a way it is not
// inside the 16384-slot walk. The model header is a different allocation from
// the entity, so the entity's own region check says nothing about it.
const char* AttachmentName(const std::uint8_t* entity, int index) {
    const auto* studioHdrWrapper =
        *reinterpret_cast<const std::uint8_t* const*>(entity + kStudioHdrPtrOffset);
    if (!PlausiblePointer(studioHdrWrapper) || !ReadableFrom(studioHdrWrapper, 16)) return nullptr;
    const auto* studio = *reinterpret_cast<const std::uint8_t* const*>(studioHdrWrapper + 8);
    if (!PlausiblePointer(studio) || !ReadableFrom(studio, 0x100)) return nullptr;
    // THE SELF-PROVING CHECK. `IDST` is the studio header's magic in every
    // Source fork, so this distinguishes "the layout moved" from "there is no
    // model", which otherwise produce identical silence.
    if (*reinterpret_cast<const std::uint32_t*>(studio) != kIdstMagic) return nullptr;
    const int count = *reinterpret_cast<const int*>(studio + kStudioNumAttachments);
    if (index < 0 || index >= count || count > 256) return nullptr;
    const int tableOffset = *reinterpret_cast<const int*>(studio + kStudioAttachmentIndex);
    if (tableOffset <= 0 || tableOffset > 0x4000000) return nullptr;
    const std::uint8_t* record =
        studio + tableOffset + static_cast<std::size_t>(index) * kStudioAttachmentStride;
    if (!ReadableFrom(record, kStudioAttachmentStride)) return nullptr;
    const int nameOffset = *reinterpret_cast<const int*>(record);
    if (nameOffset <= 0 || nameOffset > 0x4000000) return nullptr;
    const char* name = reinterpret_cast<const char*>(record + nameOffset);
    if (!ReadableFrom(name, 64)) return nullptr;
    return name;
}

bool NameLooksLikeEye(const char* name) {
    for (const char* p = name; *p && p - name < 40; ++p) {
        if ((p[0] == 'e' || p[0] == 'E') && (p[1] == 'y' || p[1] == 'Y') &&
            (p[2] == 'e' || p[2] == 'E')) {
            return true;
        }
    }
    return false;
}

// The nearest entity in front of the camera that has a model with attachments,
// and where its eye attachment sits. Returns raw material, not a verdict: the
// highest attachment and its name are reported beside the eye one so a model
// with no attachment called "eye" still says something rather than nothing.

// OUR OWN BODY'S SAME TWO ATTACHMENTS.
//
// "The camera is 3.4 units below that model's HEADFOCUS" is not yet a defect:
// the faced character may simply be taller. The question only becomes answerable
// when both numbers come off the SAME skeleton -- our own -- because then the
// comparison is camera against the eye line of the very model the camera is
// supposed to be inside.
//
// Returns the named attachment's height ABOVE THE ENTITY'S OWN ORIGIN, which is
// the frame both sides have to be expressed in; world z would carry the floor
// difference between two characters standing in different places.
bool SelfAttachmentAboveOrigin(const std::uint8_t* entity, const char* wanted, float* aboveOrigin,
                               float* worldZ) {
    if (!entity) return false;
    float origin[3]{};
    std::memcpy(origin, entity + kOriginOffset, sizeof(origin));
    if (!Finite3(origin)) return false;
    const int count = *reinterpret_cast<const int*>(entity + kAttachmentCountOffset);
    if (count <= 0 || count > 128) return false;
    const auto* table = *reinterpret_cast<const std::uint8_t* const*>(entity + kAttachmentTablePtr);
    if (!PlausiblePointer(table)) return false;
    if (!ReadableFrom(table, static_cast<std::size_t>(count) * kAttachmentTableStride)) return false;
    for (int i = 0; i < count; ++i) {
        const char* name = AttachmentName(entity, i);
        if (!name || std::strncmp(name, wanted, 24) != 0) continue;
        const auto* m = reinterpret_cast<const float*>(
            table + static_cast<std::size_t>(i) * kAttachmentTableStride);
        if (!std::isfinite(m[11])) return false;
        // AN ATTACHMENT BELOW THE FEET IS AN UNFILLED TABLE, NOT A LOW HEAD.
        // The first sample of a session read HEADFOCUS at world z 0, which this
        // band happily reported as "-32.03 above our origin" -- the absence of a
        // reading formatted as one, for the second time. A head focus is never
        // below the origin it is measured from.
        if (m[11] - origin[2] < 10.0f || m[11] - origin[2] > 200.0f) return false;
        if (aboveOrigin) *aboveOrigin = m[11] - origin[2];
        if (worldZ) *worldZ = m[11];
        return true;
    }
    return false;
}
CharacterReading ReadFacedCharacter(const float cameraPos[3], float cameraYawDegrees) {
    CharacterReading out;
    if (g_charDisarmed.load(std::memory_order_acquire)) return out;
    const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleA("client.dll"));
    if (!base) return out;
    if (!ReadableFrom(base + kEntityListRva, 8)) return out;

    const float yaw = cameraYawDegrees * kDegToRad;
    const float forwardX = std::cos(yaw), forwardY = std::sin(yaw);

    float bestDistance = 1e30f;
    const std::uint8_t* best = nullptr;
    RegionCache cache;

    __try {
        const auto* list = *reinterpret_cast<const std::uint8_t* const*>(base + kEntityListRva);
        if (!PlausiblePointer(list)) return out;
        if (!ReadableFrom(list, kEntityListStride * kEntityListMax)) return out;
        for (int index = 0; index < kEntityListMax; ++index) {
            const auto* entity = *reinterpret_cast<const std::uint8_t* const*>(
                list + static_cast<std::size_t>(index) * kEntityListStride +
                kEntityListEntityOffset);
            if (!PlausiblePointer(entity)) continue;
            // EVERY OFFSET THIS FUNCTION WILL TOUCH, checked in one go against a
            // cached region. An entity that is really some smaller object does
            // not reach 0x1210 and is skipped rather than faulted on.
            if (!RegionAllows(cache, entity, 0x1210)) continue;
            ++out.slots;
            float origin[3]{};
            std::memcpy(origin, entity + kOriginOffset, sizeof(origin));
            if (!Finite3(origin)) continue;
            const float dx = origin[0] - cameraPos[0];
            const float dy = origin[1] - cameraPos[1];
            const float dz = origin[2] - cameraPos[2];
            const float planar = std::sqrt(dx * dx + dy * dy);
            // A CHARACTER BEING LOOKED AT, and each bound is a stated reason:
            // closer than 40 units is the player's own body and its weapon;
            // past 1500 is not what "the character in front of me" means; more
            // than 60 units of height difference is a different floor; and the
            // dot product keeps it in the forward half of the view.
            if (planar < 40.0f || planar > 1500.0f) continue;
            if (std::fabs(dz) > 60.0f) continue;
            if ((dx * forwardX + dy * forwardY) < 0.7f * planar) continue;
            ++out.examined;
            const int attachments = *reinterpret_cast<const int*>(entity + kAttachmentCountOffset);
            if (attachments <= 0 || attachments > 128) continue;
            if (!PlausiblePointer(
                    *reinterpret_cast<const std::uint8_t* const*>(entity + kAttachmentTablePtr))) {
                continue;
            }
            ++out.withStudio;
            if (planar < bestDistance) {
                bestDistance = planar;
                best = entity;
            }
        }
        out.regionQueries = cache.queries;
        if (!best) return out;
        float origin[3]{};
        std::memcpy(origin, best + kOriginOffset, sizeof(origin));
        out.found = true;
        out.originZ = origin[2];
        out.distance = bestDistance;
        out.attachmentCount = *reinterpret_cast<const int*>(best + kAttachmentCountOffset);
        const auto* table = *reinterpret_cast<const std::uint8_t* const*>(best + kAttachmentTablePtr);
        if (out.attachmentCount > 128) out.attachmentCount = 128;
        if (!ReadableFrom(table, static_cast<std::size_t>(out.attachmentCount) *
                                     kAttachmentTableStride)) {
            out.attachmentCount = 0;
        }
        out.topAttachmentZ = -1e30f;
        for (int i = 0; i < out.attachmentCount && i < 128; ++i) {
            const auto* matrix =
                reinterpret_cast<const float*>(table + static_cast<std::size_t>(i) *
                                                            kAttachmentTableStride);
            const float x = matrix[3], y = matrix[7], z = matrix[11];
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
            // THE VALIDITY GATE, and it is what makes a stale table visible.
            // An attachment belongs to this model, so its world position is
            // within a body's reach of the model's own origin. A table that has
            // not been filled this frame fails that, and a failing table is
            // silently skipped rather than reported as an eye height.
            if (std::fabs(x - origin[0]) > 200.0f || std::fabs(y - origin[1]) > 200.0f) continue;
            if (z - origin[2] < -50.0f || z - origin[2] > 200.0f) continue;
            const char* name = AttachmentName(best, i);
            if (z > out.topAttachmentZ) {
                out.topAttachmentZ = z;
                std::snprintf(out.topName, sizeof(out.topName), "%s", name ? name : "<unnamed>");
            }
            if (name && !out.attachmentName[0] && NameLooksLikeEye(name)) {
                std::snprintf(out.attachmentName, sizeof(out.attachmentName), "%s", name);
                out.attachmentZ = z;
            }
        }
        out.studioOk = AttachmentName(best, 0) != nullptr;
        // EVERY ATTACHMENT NAME, ONCE PER MODEL, and it is not decoration.
        //
        // The flat run found no attachment with "eye" in its name and fell back
        // to the highest one, which turned out to be HEADSHOT -- the head
        // hitbox, i.e. head CENTRE rather than the eye line. That is a usable
        // proxy but it is a proxy, and knowing it was one required reading a
        // name the census only printed because it happened to be the highest.
        // Printing the whole list once says what the model actually offers, so
        // the next reading is chosen rather than defaulted into.
        if (out.attachmentCount > 0 && g_namesDumped < kNameDumpCap) {
            ++g_namesDumped;
            char names[700]{};
            int used = std::snprintf(names, sizeof(names),
                                     "[TF2VR] CHAR attachment names on the faced model (%d):",
                                     out.attachmentCount);
            for (int i = 0; i < out.attachmentCount; ++i) {
                if (used >= static_cast<int>(sizeof(names)) - 40) break;
                const char* name = AttachmentName(best, i);
                const auto* m = reinterpret_cast<const float*>(
                    table + static_cast<std::size_t>(i) * kAttachmentTableStride);
                used += std::snprintf(names + used, sizeof(names) - static_cast<std::size_t>(used),
                                      " [%d]%s z=%.1f", i, name ? name : "<unnamed>",
                                      static_cast<double>(m[11]));
            }
            std::snprintf(names + used, sizeof(names) - static_cast<std::size_t>(used), "\n");
            Tf2VrLog(names);
        }
        // NOT ZERO WHEN NOTHING PASSED. Leaving it at 0 made the line print
        // "highest attachment '' z=0.00 -> -92.03 units", which reads as a
        // measurement of a camera 92 units above the character's head and is
        // nothing of the kind -- it is the absence of a reading, formatted as
        // one. The flag says so instead.
        out.topValid = out.topAttachmentZ > -1e29f;
        if (!out.topValid) out.topAttachmentZ = 0.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // A read-only walk that faults has told us the layout is not what this
        // build believes. It is disarmed rather than retried: a probe that
        // faults once a sample costs the run it is meant to inform.
        g_charFaults.fetch_add(1, std::memory_order_relaxed);
        g_charDisarmed.store(true, std::memory_order_release);
        Tf2VrLog("[TF2VR] CHAR census FAULTED on the entity walk and is disarmed for this session. "
                 "The AIM and POSE streams are unaffected.\n");
        out.found = false;
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------

void PublishXrLocalHeadCentre(const float metresXyz[3]) {
    for (int i = 0; i < 3; ++i) g_pose.localHead[i].store(metresXyz[i], std::memory_order_relaxed);
    g_pose.localUpdates.fetch_add(1, std::memory_order_release);
}

void PublishXrProjectionPose(bool projectionInUse, bool originValid, const float origin[3],
                             const float renderedHeadCentre[3], const float declaredPose[3]) {
    for (int i = 0; i < 3; ++i) {
        g_pose.origin[i].store(origin[i], std::memory_order_relaxed);
        g_pose.headCentre[i].store(renderedHeadCentre[i], std::memory_order_relaxed);
        g_pose.declared[i].store(declaredPose[i], std::memory_order_relaxed);
    }
    g_pose.projectionInUse.store(projectionInUse, std::memory_order_relaxed);
    g_pose.originValid.store(originValid, std::memory_order_relaxed);
    g_pose.projectionUpdates.fetch_add(1, std::memory_order_release);
}


// ---------------------------------------------------------------------------
// THE ADS TRANSITION RECORDER
//
// The wearer reports ADS working and then breaking, and the existing instrument
// cannot see it: `ads.lock` counts ENGAGEMENTS, and it logged an identical 2:1
// engage/stand-down ratio in all twelve of them across a run where ADS both
// worked and failed. A counter that reads the same on the good and bad cases is
// not measuring the thing that differs.
//
// So this records the four candidates the wearer would have had to tell apart
// by eye, and it records them as numbers at the moment of the transition rather
// than as a per-second average that a sub-second entry blend hides inside:
//
//   gun not drawn at all       -> the viewmodel family's upload count stops
//   gun drawn in the wrong place -> its count climbs but the origin jumps
//   sight and round disagree   -> aim angles part company with camera angles
//   the view snaps on entry    -> the camera's applied angles step
//
// EDGE-TRIGGERED AND THEN FAST. One line when ads.lock changes state and one
// every 200 ms while engaged, capped, so an entry blend that lasts a third of a
// second is sampled several times instead of once by luck.


void AdsTransitionTick() {
    static bool wasEngaged = false;
    static std::uint64_t nextMs = 0;
    static std::uint32_t lines = 0;
    static unsigned long long previousTally[4]{};
    static std::uint32_t transitions = 0;
    constexpr std::uint32_t kAdsLineCap = 300;

    const bool engaged = AdsLockEngaged();
    const std::uint64_t now = GetTickCount64();
    const bool edge = engaged != wasEngaged;
    if (edge) ++transitions;
    if (!edge && (!engaged || now < nextMs)) { wasEngaged = engaged; return; }
    wasEngaged = engaged;
    if (lines >= kAdsLineCap) return;
    ++lines;
    nextMs = now + 200;

    unsigned long long tally[4]{};
    ReadUploadFamilyTally(tally);
    unsigned long long delta[4]{};
    for (int i = 0; i < 4; ++i) {
        delta[i] = tally[i] - previousTally[i];
        previousTally[i] = tally[i];
    }

    AdsZoomState zoom;
    const bool haveZoom = GetAdsZoom(&zoom);
    float base[3]{}, applied[3]{};
    const bool haveAngles = ReadLatchedCameraAngles(base, applied, nullptr);
    float aim[3]{};
    const bool haveAim = TryGetAimAnglesDegrees(aim);

    // WHERE THE GUN IS, from the entity the pin already resolves without a heap
    // walk. A null instance IS the answer to "no gun appears" and is reported as
    // such rather than as zeroes.
    const auto* viewmodel = FindLiveViewmodelInstance();
    float gun[3]{};
    bool haveGun = false;
    if (viewmodel) {
        __try {
            std::memcpy(gun, viewmodel + 0x90, sizeof(gun));
            haveGun = std::isfinite(gun[0]) && std::isfinite(gun[1]) && std::isfinite(gun[2]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            haveGun = false;
        }
    }

    char line[700]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] ADSXN#%u/%u %s | zoom frac %.3f mag %.3f fov now %.1f rest %.1f%s | uploads since "
        "last: world %llu effects %llu VIEWMODEL %llu other %llu | gun origin %s(%.1f %.1f %.1f) "
        "dist from camera %.1f | cam applied<p%+.2f y%+.2f r%+.2f> base<p%+.2f y%+.2f>%s | "
        "aim<p%+.2f y%+.2f>%s | aim MINUS cam <p%+.2f y%+.2f>. VIEWMODEL at 0 while world climbs "
        "is the gun not being drawn; a jump in gun origin is it being drawn somewhere else.\n",
        transitions, lines, engaged ? "ENGAGED" : "released",
        haveZoom ? static_cast<double>(zoom.frac) : 0.0,
        haveZoom ? static_cast<double>(zoom.magnification) : 0.0,
        haveZoom ? static_cast<double>(zoom.fovNow) : 0.0,
        haveZoom ? static_cast<double>(zoom.fovRest) : 0.0, haveZoom ? "" : " (no zoom state)",
        delta[0], delta[1], delta[2], delta[3],
        haveGun ? "" : viewmodel ? "UNREADABLE " : "NO LIVE VIEWMODEL ",
        static_cast<double>(gun[0]), static_cast<double>(gun[1]), static_cast<double>(gun[2]),
        haveGun ? static_cast<double>(std::sqrt(
                      (gun[0] - g_lastCameraPos[0]) * (gun[0] - g_lastCameraPos[0]) +
                      (gun[1] - g_lastCameraPos[1]) * (gun[1] - g_lastCameraPos[1]) +
                      (gun[2] - g_lastCameraPos[2]) * (gun[2] - g_lastCameraPos[2])))
                : 0.0,
        applied[0], applied[1], applied[2], base[0], base[1], haveAngles ? "" : " (torn)",
        aim[0], aim[1], haveAim ? "" : " (none)",
        haveAim && haveAngles ? aim[0] - applied[0] : 0.0,
        haveAim && haveAngles ? aim[1] - applied[1] : 0.0);
    Tf2VrLog(line);
}
void AimCensusTick() {
    const std::uint64_t now = GetTickCount64();

    static bool announced = false;
    if (!announced) {
        announced = true;
        char line[520]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] AIM CENSUS live: three read-only streams (AIM every %llu ms, POSE every %llu "
            "ms, CHAR every %llu ms; caps %u/%u/%u lines). READ-ONLY: this census changes no "
            "height and touches no aim. The height in force is eye.raise = %+.2f units, applied "
            "at the engine's own eye getter -- see player_eye_hook.h.\n",
            kAimPeriodMs, kPosePeriodMs, kCharPeriodMs, kAimLineCap, kPoseLineCap, kCharLineCap,
            static_cast<double>(EyeHookRaiseInForce()));
        Tf2VrLog(line);
    }

    float cameraBase[3]{}, cameraApplied[3]{};
    std::uint32_t angleGeneration = 0;
    const bool haveAngles = ReadLatchedCameraAngles(cameraBase, cameraApplied, &angleGeneration);
    float cameraPos[3]{};
    GameCameraBasePosition(cameraPos);
    for (int i = 0; i < 3; ++i) g_lastCameraPos[i] = cameraPos[i];
    // Edge-triggered, so it runs every plugin frame and logs only on a change or
    // on its own 200 ms clock while engaged.
    AdsTransitionTick();

    // ---- AIM ---------------------------------------------------------------
    if (now >= g_nextAimMs && g_aimLines < kAimLineCap) {
        g_nextAimMs = now + kAimPeriodMs;
        ++g_aimLines;
        ++g_sample;

        float renderedX = 0.0f, renderedY = 0.0f;
        const bool haveRendered = GetMainSceneRenderedHalfTangents(renderedX, renderedY);
        float declaredX = 0.0f, declaredY = 0.0f;
        const bool haveDeclared = GetMainSceneHalfTangents(declaredX, declaredY);
        float aim[3]{};
        const bool haveAim = TryGetAimAnglesDegrees(aim);
        float anchorX = 0.0f, anchorY = 0.0f, yawOffset = 0.0f, pitchOffset = 0.0f;
        ReadReticleAnchor(&anchorX, &anchorY, &yawOffset, &pitchOffset);
        float passWidth = 0.0f, passHeight = 0.0f, divisorX = 0.0f, divisorY = 0.0f;
        int anchorHadAim = 0;
        unsigned long long anchorUpdates = 0;
        ReadReticleAnchorDetail(&passWidth, &passHeight, &divisorX, &divisorY, &anchorHadAim,
                                &anchorUpdates);
        float pitchHeadTerm = 0.0f, pitchOffsetTerm = 0.0f;
        ReadPitchComposition(&pitchHeadTerm, &pitchOffsetTerm);
        float hudX = 0.0f, hudY = 0.0f, hudZoom = 1.0f;
        ReadHud2dPlacement(&hudX, &hudY, &hudZoom);
        float viewX = 0.0f, viewY = 0.0f, viewW = 0.0f, viewH = 0.0f;
        const bool haveViewport = GetMainSceneViewport(viewX, viewY, viewW, viewH);

        // THE ANCHOR'S OWN NDC, RECOMPUTED HERE FROM THE PIXELS IT WROTE, so
        // the census does not simply repeat the number the anchor believed. If
        // these disagree with tan(offset)/halfTan the anchor is not doing what
        // its own comment says.
        const float anchorNdcX = passWidth > 1.0f ? (2.0f * anchorX / passWidth - 1.0f) : 0.0f;
        const float anchorNdcY = passHeight > 1.0f ? (2.0f * anchorY / passHeight - 1.0f) : 0.0f;
        const float predictedNdcY =
            (haveRendered && renderedY > 0.0001f)
                ? std::tan(pitchOffset * kDegToRad) / renderedY
                : 0.0f;

        char line[1700]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] AIM#%u/%u raise=%+.1f | anchor px<%.1f,%.1f> ndc<%+.4f,%+.4f> "
            "predictedNdcY %+.4f | updates=%llu hadAim=%d | divisor USED <%.5f,%.5f> RENDERED "
            "<%.5f,%.5f>%s DECLARED <%.5f,%.5f>%s adsMag=%.4f | hud2d pass %.0fx%.0f zoom=%.3f | "
            "world viewport %.0fx%.0f at (%.0f,%.0f)%s | cam base<p%+.2f y%+.2f r%+.2f> "
            "applied<p%+.2f y%+.2f r%+.2f> gen=%u%s | aim<p%+.2f y%+.2f>%s | off<yaw%+.2f "
            "pitch%+.2f> | pitch terms: head %+.2f + adsOffset %+.2f (their SUM is what is clamped at 89) | shear=%d appliedNdcY=%+.5f | camZ=%.2f | "
            "HEIGHT: eye.raise %+.2f units, applied at the game's OWN eye, so the shot origin "
            "rises with the camera and this contributes no parallax | TRACKING residual: head "
            "tracking has the rendered eye %+.2f units off the game eye (write=%d), which the "
            "round does NOT follow -- %.2f deg at 5 m, %.2f deg at 20 m. Leaning is what makes "
            "this non-zero. | LEAN DETACH: strength %.2f, applied to %llu uploads -- strength 1.00 "
            "with a FLAT count is a CLOSED GATE, not a working correction | "
            "hud2d shift <%+.0f,%+.0f> px | headset '%s'\n",
            g_sample, g_aimLines, static_cast<double>(EyeHookRaiseInForce()),
            anchorX, anchorY, anchorNdcX, anchorNdcY, predictedNdcY,
            anchorUpdates, anchorHadAim,
            divisorX, divisorY,
            renderedX, renderedY, haveRendered ? "" : "(none)",
            declaredX, declaredY, haveDeclared ? "" : "(none)",
            static_cast<double>(DeclaredMagnificationInUse()),
            passWidth, passHeight, static_cast<double>(hudZoom),
            viewW, viewH, viewX, viewY, haveViewport ? "" : "(none)",
            cameraBase[0], cameraBase[1], cameraBase[2],
            cameraApplied[0], cameraApplied[1], cameraApplied[2], angleGeneration,
            haveAngles ? "" : "(torn)",
            aim[0], aim[1], haveAim ? "" : "(none)",
            yawOffset, pitchOffset,
            pitchHeadTerm, pitchOffsetTerm,
            LensShearEnabled() ? 1 : 0, static_cast<double>(LensShearAppliedNdcY()),
            static_cast<double>(cameraPos[2]),
            static_cast<double>(EyeHookRaiseInForce()),
            static_cast<double>(CameraRaiseUnits()), g_cameraPositionWriteActive ? 1 : 0,
            static_cast<double>(ParallaxDegreesAt(CameraRaiseUnits(), 5.0f)),
            static_cast<double>(ParallaxDegreesAt(CameraRaiseUnits(), 20.0f)),
            static_cast<double>(ViewmodelDetachLeanStrength()), ViewmodelDetachLeanApplied(),
            static_cast<double>(hudX), static_cast<double>(hudY),
            HeadsetFingerprint());
        Tf2VrLog(line);
    }

    // ---- POSE (H3's rider) -------------------------------------------------
    if (now >= g_nextPoseMs && g_poseLines < kPoseLineCap) {
        g_nextPoseMs = now + kPosePeriodMs;
        ++g_poseLines;

        float reference[3]{};
        const bool haveReference = TryGetHeadPositionReference(reference);
        float head[3]{};
        const bool haveHead = ReadHeadPositionMetres(head);
        const std::uint64_t localUpdates = g_pose.localUpdates.load(std::memory_order_acquire);
        const std::uint64_t projectionUpdates =
            g_pose.projectionUpdates.load(std::memory_order_acquire);
        // FRESHNESS, NOT A FLAG. `projectionInUse` latches true on the first
        // submitted projection layer and stays true afterwards, so a layer that
        // arms and then falls back to the quad path reads identically to one
        // that is still running. The count SINCE THE LAST SAMPLE cannot: a
        // stalled layer publishes zero.
        static std::uint64_t previousProjectionUpdates = 0;
        static std::uint64_t previousLocalUpdates = 0;
        const std::uint64_t projectionSinceLast = projectionUpdates - previousProjectionUpdates;
        const std::uint64_t localSinceLast = localUpdates - previousLocalUpdates;
        previousProjectionUpdates = projectionUpdates;
        previousLocalUpdates = localUpdates;
        const float localY = g_pose.localHead[1].load(std::memory_order_relaxed);
        const float originY = g_pose.origin[1].load(std::memory_order_relaxed);
        const float centreY = g_pose.headCentre[1].load(std::memory_order_relaxed);
        const float declaredY = g_pose.declared[1].load(std::memory_order_relaxed);

        char line[820]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] POSE#%u raise=%+.1f | raw LOCAL head y=%.4f m (locates=%llu, +%llu "
            "since last) | game-side head y=%.4f m%s | recentre reference y=%.4f m%s | "
            "delta=%.4f m | projectionOrigin y=%.4f m valid=%d | renderedHeadCentre y=%.4f m | "
            "declared layer pose y=%.4f m | layer ever used=%d (submits=%llu, +%llu since last) | "
            "STAGE floor eye height=%.3f m | units/m=%.2f | headset '%s'. STAGE is reported and "
            "drives NOTHING: the two runtimes disagree by ~0.34 m about one unmoved seated "
            "wearer, so it cannot be what a height rests on.\n",
            g_sample, static_cast<double>(EyeHookRaiseInForce()),
            static_cast<double>(localY), localUpdates, localSinceLast,
            static_cast<double>(head[1]), haveHead ? "" : "(none)",
            static_cast<double>(reference[1]), haveReference ? "" : "(none)",
            static_cast<double>(head[1] - reference[1]),
            static_cast<double>(originY), g_pose.originValid.load(std::memory_order_relaxed) ? 1 : 0,
            static_cast<double>(centreY), static_cast<double>(declaredY),
            g_pose.projectionInUse.load(std::memory_order_relaxed) ? 1 : 0, projectionUpdates,
            projectionSinceLast,
            static_cast<double>(MeasuredEyeHeightMetres()),
            static_cast<double>(SourceUnitsPerMetre()), HeadsetFingerprint());
        Tf2VrLog(line);
        // ---- LAYER: what we hand the compositor, per headset ----------------
        {
            char layer[560]{};
            std::snprintf(layer, sizeof(layer),
                "[TF2VR] LAYER#%u declared to the compositor: up %+.2f down %+.2f left %+.2f right "
                "%+.2f deg | vertical centre %+.2f deg off forward | rect %dx%d at (%d,%d) | "
                "submits=%llu | headset '%s'. The PFD MR reads correct and the Quest 3 does not, "
                "and this is the only structural difference between them that has never been "
                "measured on BOTH with one instrument.\n",
                g_poseLines,
                static_cast<double>(g_declared.up.load(std::memory_order_relaxed)),
                static_cast<double>(g_declared.down.load(std::memory_order_relaxed)),
                static_cast<double>(g_declared.left.load(std::memory_order_relaxed)),
                static_cast<double>(g_declared.right.load(std::memory_order_relaxed)),
                0.5 * (static_cast<double>(g_declared.up.load(std::memory_order_relaxed)) +
                       static_cast<double>(g_declared.down.load(std::memory_order_relaxed))),
                g_declared.rectW.load(std::memory_order_relaxed),
                g_declared.rectH.load(std::memory_order_relaxed),
                g_declared.rectX.load(std::memory_order_relaxed),
                g_declared.rectY.load(std::memory_order_relaxed),
                g_declared.updates.load(std::memory_order_acquire), HeadsetFingerprint());
            Tf2VrLog(layer);
        }
    }

    // THE ENGINE LEVER, once, and only once a world is up: a class var set
    // against no player is a command shouted at nothing.
    ApplyViewHeightIfRequested();

    // ---- CHAR (D1) ---------------------------------------------------------
    if (now >= g_nextCharMs && g_charLines < kCharLineCap &&
        !g_charDisarmed.load(std::memory_order_acquire)) {
        g_nextCharMs = now + kCharPeriodMs;
        ++g_charLines;

        LARGE_INTEGER start{}, end{}, frequency{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        const CharacterReading reading =
            ReadFacedCharacter(cameraPos, haveAngles ? cameraApplied[1] : 0.0f);
        QueryPerformanceCounter(&end);
        const double elapsedMs =
            frequency.QuadPart
                ? 1000.0 * static_cast<double>(end.QuadPart - start.QuadPart) /
                      static_cast<double>(frequency.QuadPart)
                : 0.0;
        if (elapsedMs > g_charWorstMs) g_charWorstMs = elapsedMs;

        // The hull, on the CHAR clock so it is sampled either side of a crouch.
        // Capped: the crouch A/B needs a handful of samples, not a stream.
        if (g_hullLines < 40) {
            ++g_hullLines;
            LogPlayerHullCandidates(cameraPos[2]);
        }
        float playerOriginZ = 0.0f, playerViewOffsetZ = 0.0f;
        int playerField = -1;
        const bool havePlayer =
            PlayerEyeReadOnly(cameraPos[2], &playerOriginZ, &playerViewOffsetZ, &playerField);
        // THE HEIGHT FIX, at the game's own source. Placed AFTER the census has
        // read the view offset out of the entity, because that read is the
        // positive control the write is gated on: two independent routes to one
        // quantity agreeing is what licenses touching memory at all.
        //
        // Idempotent by construction -- it re-applies only when it sees the
        // original value back after a re-parse, so it is a data correction and
        // not a per-tick writer fighting prediction.
        ApplyEyeLift(playerViewOffsetZ);
        // THE ENGINE'S OWN NAME FOR THE QUANTITY, read-only.
        //
        // pescan found a convar "viewheight" in client.dll whose help text is
        // "View height for current stance" -- which is exactly the 60.00 / 38.00
        // the census measures at +0xAC. If the two agree, the height has an
        // ENGINE LEVER and does not need memory surgery at all: everything
        // downstream of it -- camera, gun, bullets, reticle -- derives from the
        // one quantity, which is the property every previous fix attempt lacked.
        //
        // Read only. Whether it can be SET, and whether setting it survives
        // prediction, are separate questions and neither is asked here.
        {
            static bool announced = false;
            float viewheight = 0.0f;
            const bool have = TryReadCvarFloat("viewheight", viewheight);
            if (!announced || g_charLines < 8) {
                announced = true;
                char line[420]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] VIEWHEIGHT cvar: %s%.2f | measured view offset z=%.2f | hull is "
                    "72.00 standing / 47.00 crouched | camera z=%.2f. If the cvar tracks the "
                    "measured offset it IS the quantity, and the height has an engine lever.\n",
                    have ? "" : "NOT FOUND ", static_cast<double>(viewheight),
                    static_cast<double>(playerViewOffsetZ), static_cast<double>(cameraPos[2]));
                Tf2VrLog(line);
            }
        }

        // D1's decisive half: the SAME two attachments on OUR OWN body. Both
        // sides are then heights above their own origin, so a taller character
        // cannot masquerade as a low camera.
        float selfHead = 0.0f, selfChest = 0.0f, selfHeadWorld = 0.0f, selfChestWorld = 0.0f;
        bool haveSelfHead = false, haveSelfChest = false;
        const auto* self = static_cast<const std::uint8_t*>(PlayerEyeLocalPlayerForReading());
        if (self) {
            __try {
                haveSelfHead = SelfAttachmentAboveOrigin(self, "HEADFOCUS", &selfHead, &selfHeadWorld);
                haveSelfChest =
                    SelfAttachmentAboveOrigin(self, "CHESTFOCUS", &selfChest, &selfChestWorld);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                haveSelfHead = false;
                haveSelfChest = false;
            }
        }

        // OUR OWN MODEL'S WHOLE ATTACHMENT SET, ground-referenced.
        //
        // Everything about the height now turns on one comparison and it has
        // never been measured on one skeleton: where the camera sits against
        // OUR OWN eye line and OUR OWN feet. The faced NPC gives 63.8 units
        // from its soles to HEADSHOT, which at 39.37 units/metre is a 1.74 m
        // adult -- so the world scale is right and the camera is nonetheless
        // 6.61 units below that NPC's eye line.
        //
        // The remaining unknown is whether OUR body is the same size as theirs.
        // Source's standard player is a 72-unit hull with a 64-unit view
        // offset; Titanfall's view offset measures 60, which on a 72 hull is
        // 83% of stature where a human eye sits at 93%. That predicts the
        // camera is about 7 units low -- but "on a 72 hull" is an ASSUMPTION,
        // and this is the line that removes it. foot_L_sole gives the ground
        // and HEADSHOT gives the eye, both off the model the camera is
        // supposed to be inside.
        if (self && g_selfNamesDumped < 3) {
            __try {
                const int count = *reinterpret_cast<const int*>(self + kAttachmentCountOffset);
                const auto* table =
                    *reinterpret_cast<const std::uint8_t* const*>(self + kAttachmentTablePtr);
                if (count > 0 && count <= 128 && PlausiblePointer(table) &&
                    ReadableFrom(table, static_cast<std::size_t>(count) * kAttachmentTableStride)) {
                    ++g_selfNamesDumped;
                    char names[760]{};
                    int used = std::snprintf(names, sizeof(names),
                        "[TF2VR] SELF attachment names on OUR OWN model (%d), camera world z=%.2f:",
                        count, static_cast<double>(cameraPos[2]));
                    for (int i = 0; i < count; ++i) {
                        if (used >= static_cast<int>(sizeof(names)) - 40) break;
                        const char* name = AttachmentName(self, i);
                        const auto* m = reinterpret_cast<const float*>(
                            table + static_cast<std::size_t>(i) * kAttachmentTableStride);
                        used += std::snprintf(names + used,
                                              sizeof(names) - static_cast<std::size_t>(used),
                                              " [%d]%s z=%.1f", i, name ? name : "<unnamed>",
                                              static_cast<double>(m[11]));
                    }
                    std::snprintf(names + used, sizeof(names) - static_cast<std::size_t>(used),
                                  "\n");
                    Tf2VrLog(names);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                g_selfNamesDumped = 3;
            }
        }
        if (haveSelfHead || haveSelfChest) {
            const float cameraAboveOwnOrigin = cameraPos[2] - playerOriginZ;
            char self_line[600]{};
            std::snprintf(self_line, sizeof(self_line),
                "[TF2VR] SELF#%u | OUR OWN model, everything above OUR OWN origin (z=%.2f): camera "
                "%.2f | HEADFOCUS %s%.2f | CHESTFOCUS %s%.2f | camera MINUS our own HEADFOCUS = "
                "%+.2f units (%.1f cm). This is the number that matters: both sides are one "
                "skeleton, so a taller character cannot masquerade as a low camera. Near zero means "
                "the camera IS at our eye line and the residual is not game-side.\n",
                g_charLines, static_cast<double>(playerOriginZ),
                static_cast<double>(cameraAboveOwnOrigin),
                haveSelfHead ? "" : "(none)", static_cast<double>(selfHead),
                haveSelfChest ? "" : "(none)", static_cast<double>(selfChest),
                static_cast<double>(cameraAboveOwnOrigin - selfHead),
                static_cast<double>(cameraAboveOwnOrigin - selfHead) / 39.37 * 100.0);
            Tf2VrLog(self_line);
        }

        char line[900]{};
        if (reading.found) {
            const bool haveEye = reading.attachmentName[0] != '\0';
            // Formatted as text, so an absent reading cannot be printed as a
            // number that looks like one.
            char topDelta[48]{};
            if (reading.topValid) {
                std::snprintf(topDelta, sizeof(topDelta), "%+.2f units",
                              static_cast<double>(reading.topAttachmentZ - cameraPos[2]));
            } else {
                std::snprintf(topDelta, sizeof(topDelta), "no valid attachment, so NO READING");
            }
            std::snprintf(line, sizeof(line),
                "[TF2VR] CHAR#%u/%u raise=%+.1f | nearest faced model at %.0f units, origin "
                "z=%.2f, %d readable attachments (names %s) | eye attachment '%s' z=%.2f -> eye MINUS camera "
                "= %+.2f units | highest attachment '%s' z=%.2f -> %s | our own player "
                "origin z=%.2f + viewOffset z=%.2f = %.2f (field +0x%X)%s | camera z=%.2f | "
                "slots=%d examined=%d withModel=%d queries=%u walk %.2f ms (worst %.2f) | headset "
                "'%s'. D1: H1 predicts "
                "the character's eye sits about +10 units ABOVE the camera; a reading near 0 "
                "refutes H1.\n",
                g_sample, g_charLines, static_cast<double>(EyeHookRaiseInForce()),
                static_cast<double>(reading.distance), static_cast<double>(reading.originZ),
                reading.attachmentCount, reading.studioOk ? "readable" : "UNREADABLE",
                haveEye ? reading.attachmentName : "<none named eye>",
                static_cast<double>(reading.attachmentZ),
                haveEye ? static_cast<double>(reading.attachmentZ - cameraPos[2]) : 0.0,
                reading.topValid ? reading.topName : "<no attachment passed validation>",
                static_cast<double>(reading.topAttachmentZ),
                topDelta,
                static_cast<double>(playerOriginZ), static_cast<double>(playerViewOffsetZ),
                static_cast<double>(playerOriginZ + playerViewOffsetZ),
                playerField < 0 ? 0 : static_cast<unsigned>(playerField),
                havePlayer ? "" : " (not identified)",
                static_cast<double>(cameraPos[2]), reading.slots, reading.examined,
                reading.withStudio, reading.regionQueries, elapsedMs, g_charWorstMs,
                HeadsetFingerprint());
        } else {
            std::snprintf(line, sizeof(line),
                "[TF2VR] CHAR#%u/%u raise=%+.1f | NO faced model: slots=%d readable, "
                "examined=%d in the cone, withModel=%d, queries=%u | our own player origin z=%.2f "
                "+ viewOffset z=%.2f = %.2f (field +0x%X)%s | camera z=%.2f | walk %.2f ms | "
                "headset '%s'. slots=0 means the entity list itself did not read, which is a "
                "layout problem and not an empty map; examined=0 with slots>0 means nothing was in "
                "the forward cone between 40 and 1500 units -- stand facing a character.\n",
                g_sample, g_charLines, static_cast<double>(EyeHookRaiseInForce()),
                reading.slots, reading.examined, reading.withStudio, reading.regionQueries,
                static_cast<double>(playerOriginZ), static_cast<double>(playerViewOffsetZ),
                static_cast<double>(playerOriginZ + playerViewOffsetZ),
                playerField < 0 ? 0 : static_cast<unsigned>(playerField),
                havePlayer ? "" : " (not identified)",
                static_cast<double>(cameraPos[2]), elapsedMs, HeadsetFingerprint());
        }
        Tf2VrLog(line);
    }
}

void PublishXrDeclaredView(float up, float down, float left, float right, int rectX, int rectY,
                           int rectW, int rectH) {
    g_declared.up.store(up, std::memory_order_relaxed);
    g_declared.down.store(down, std::memory_order_relaxed);
    g_declared.left.store(left, std::memory_order_relaxed);
    g_declared.right.store(right, std::memory_order_relaxed);
    g_declared.rectX.store(rectX, std::memory_order_relaxed);
    g_declared.rectY.store(rectY, std::memory_order_relaxed);
    g_declared.rectW.store(rectW, std::memory_order_relaxed);
    g_declared.rectH.store(rectH, std::memory_order_relaxed);
    g_declared.updates.fetch_add(1, std::memory_order_release);
}

namespace {
std::atomic<float> g_viewHeightUnits{0.0f};
std::atomic_bool g_viewHeightApplied{false};
}  // namespace

void SetViewHeightUnits(float units) {
    // Range-gated: this moves the wearer's eye, and a typo of 600 would put them
    // above the map with nothing in the log saying why.
    if (!(units >= 0.0f && units < 120.0f)) {
        Tf2VrLog("[TF2VR] vr.viewheight out of range; ignored and the height is untouched.\n");
        return;
    }
    g_viewHeightUnits.store(units, std::memory_order_release);
}

void ApplyViewHeightIfRequested() {
    const float units = g_viewHeightUnits.load(std::memory_order_acquire);
    if (units <= 0.0f) return;
    if (g_viewHeightApplied.exchange(true, std::memory_order_acq_rel)) return;
    char command[96]{};
    std::snprintf(command, sizeof(command), "_setClassVarServer viewheight \"%.2f\"\n",
                  static_cast<double>(units));
    const bool ran = RunEngineConsoleCommand(command);
    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] vr.viewheight: %s `_setClassVarServer viewheight \"%.2f\"`. The falsifier is the "
        "CHAR line's own view-offset read: 60.00 means nothing happened, and about %.2f means the "
        "game took it. Nobody has to judge anything.\n",
        ran ? "ran" : "COULD NOT RUN (the engine command buffer did not verify)",
        static_cast<double>(units), static_cast<double>(units));
    Tf2VrLog(line);
}
