#include "ads_probe.h"

#include "aim_cmd.h"
#include "placement_pin.h"
#include "ads_lock.h"
#include "ads_zoom.h"
#include "camera_hook.h"
#include "hand_pose.h"
#include "camera_update_hook.h"
#include "config.h"
#include "diagnostics.h"
#include "scan_outcome.h"
#include "viewmodel_instance.h"
#include "weapon_settings.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

// The one object the placement pin writes (placement_pin.cpp). Compared only,
// so the census can say whether the viewmodel it measures is the pin's target.
extern "C" volatile std::uintptr_t g_placementInstance;

namespace {

// ---------------------------------------------------------------------------
// THE ONE ENGINE CALL. See S1-ATTACHMENT-API-2026-08-26.md sections 1 and 6.
//
//   bool CBaseAnimating::GetAttachment(int index, matrix3x4_t& out)
//   client.dll+0x000F4930, this = entity + 0x10, index is ONE-BASED.
//
// .pdata: root F4930 .. F49E5, 181 bytes, ONE fragment, no UNW_FLAG_CHAININFO.
// A real function entry and not a continuation -- the trap
// F1-PLACEMENT-WRITER-FOUND.md records. pescan selftest on this client.dll
// first: functions 46493, instructions 1738832, 0.9872% bad.
//
// This is a CALL target, never a hook target. Nothing here patches a byte.
//
// It is not a pure read either, and saying so plainly matters: the function
// begins by calling the entity's own bone/attachment setup through
// [vtable+0x760] and FAILS rather than handing back stale data if that setup
// declines. That is the property that makes the number trustworthy, and it is
// also this probe's built-in "am I watching" answer -- a false return is a real
// "no attachment this frame", not a missing instrument. It is the engine's own
// entry point on the engine's own thread, which is the rung above reproducing
// the attachment array by hand.
constexpr std::uintptr_t kGetAttachmentRva = 0x0F4930;
constexpr std::uint8_t kGetAttachmentPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08,        // mov [rsp+0x8], rbx
    0x48, 0x89, 0x74, 0x24, 0x10,        // mov [rsp+0x10], rsi
    0x57,                                // push rdi
    0x48, 0x83, 0xEC, 0x20,              // sub rsp, 0x20
};
using GetAttachmentFn = bool(__fastcall*)(void*, int, float*);

// Field offsets, all derived in S1-ATTACHMENT-API-2026-08-26.md section 7.
constexpr std::size_t kAnimatingSubObject = 0x0010;
constexpr std::size_t kAttachmentCount = 0x11D8;
constexpr std::size_t kStudioHdrWrapper = 0x1208;
constexpr std::size_t kStudioHdrInner = 0x0008;
constexpr std::size_t kStudioNumAttachments = 0x00F4;
constexpr std::size_t kStudioAttachmentIndex = 0x00F8;
constexpr std::size_t kStudioAttachmentStride = 0x005C;
// UNVERIFIED, and labelled as such wherever it is printed. Stock Source puts
// studiohdr_t::name at +0x0C, but the two offsets above are Respawn's own
// (0xF4/0xF8 against stock's 0x98/0x9C), so the header demonstrably moved. The
// value is printed only after a printable-ASCII test, and the log says
// "unverified" beside it, so a wrong offset reads as a wrong offset rather than
// as a weapon name.
constexpr std::size_t kStudioNameGuess = 0x000C;
constexpr std::uint32_t kStudioMagic = 0x54534449u;  // 'IDST'

// ---------------------------------------------------------------------------
// WHICH WEAPON IS IN HAND. S1-ATTACHMENT-API-2026-08-26.md section 3.
//
// S2 came back with model="?" because the studiohdr name offset guessed there
// was stock Source's and does not hold on this build -- it printed "?" rather
// than garbage, which was the point of the printable test, but it left every
// per-weapon row unlabelled. The class index was always the better key anyway:
// it is an int, and it is stable across a map load where a studiohdr pointer is
// not.
//
//   GetActiveWeapon(player)   client.dll+0x0B19C0   .pdata root, 3 fragments
//   classIndex                weapon + 0x15C8       int, 0xFF/0xFFFF = none
//   GetWeaponClassName(weapon) client.dll+0x5A9940  -> const char*
//
// 0x5A9940 has NO .pdata entry, and that is correct rather than suspicious: it
// is nine instructions ending in a tail `jmp [rax+0x48]` through a global
// interface, so it allocates no frame and needs no unwind data. S1 section 6
// records the same thing for the other leaves. It is a CALL target, not a hook
// target, and nothing here patches a byte.
constexpr std::uintptr_t kGetLocalPlayerRva = 0x14EF00;
constexpr std::uintptr_t kGetActiveWeaponRva = 0x0B19C0;
constexpr std::uintptr_t kGetWeaponClassNameRva = 0x5A9940;
constexpr std::size_t kWeaponClassIndex = 0x15C8;
constexpr std::uint8_t kGetLocalPlayerPrologue[] = {0x8B, 0x05};
constexpr std::uint8_t kGetActiveWeaponPrologue[] = {
    0x48, 0x83, 0xEC, 0x28,              // sub rsp, 0x28
    0x48, 0x8B, 0x01,                    // mov rax, [rcx]
    0x48, 0x89, 0x5C, 0x24, 0x30,        // mov [rsp+0x30], rbx
};
constexpr std::uint8_t kGetWeaponClassNamePrologue[] = {
    0x8B, 0x91, 0xC8, 0x15, 0x00, 0x00,  // mov edx, [rcx+0x15C8]
    0x81, 0xFA, 0xFF, 0xFF, 0x00, 0x00,  // cmp edx, 0xFFFF
};
using GetLocalPlayerFn = void*(__fastcall*)();
using GetActiveWeaponFn = void*(__fastcall*)(void*);
using GetWeaponClassNameFn = const char*(__fastcall*)(void*);

// The zoom now lives in ads_zoom.cpp, which is its single owner: C1 needs it on
// every camera upload and whether or not this probe is armed, and two copies of
// one derivation is exactly what drifts apart. This file reports what that owner
// says, so the log describes the numbers C1 actually acts on.

constexpr int kMaxAttachments = 64;
constexpr std::uint64_t kSampleIntervalMs = 100;
constexpr std::uint64_t kReportIntervalMs = 1000;
constexpr float kRadToDeg = 57.2957795131f;
constexpr float kDegToRad = 0.01745329252f;

std::atomic_bool g_enabled = false;
std::atomic_int g_wantAttachment = 0;
// ads.trace: one line per sample while ADS is engaged. See TraceAds.
std::atomic_bool g_adsTrace = false;

GetAttachmentFn g_getAttachment = nullptr;
GetLocalPlayerFn g_getLocalPlayer = nullptr;
GetActiveWeaponFn g_getActiveWeapon = nullptr;
GetWeaponClassNameFn g_getWeaponClassName = nullptr;
bool g_resolveAttempted = false;

bool ReadableFrom(const void* address, std::size_t bytes) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    return start + info.RegionSize - at >= bytes;
}

// Resolved once, and REFUSED rather than called if the bytes are not the ones
// it was read from on disk. Same discipline as viewmodel_instance.cpp: we do
// not have to trust the address, only check what is at it.
bool EnsureResolved() {
    if (g_getAttachment) return true;
    if (g_resolveAttempted) return false;
    g_resolveAttempted = true;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return false;
    auto* site = reinterpret_cast<std::uint8_t*>(client) + kGetAttachmentRva;
    if (!ReadableFrom(site, sizeof(kGetAttachmentPrologue))) return false;
    if (std::memcmp(site, kGetAttachmentPrologue, sizeof(kGetAttachmentPrologue)) != 0) {
        char line[360]{};
        int used = std::snprintf(line, sizeof(line),
            "[TF2VR] ADSPROBE: client.dll+0x%llX does not begin with the bytes CBaseAnimating::"
            "GetAttachment was read from, so this is not that build. NOT called. Found:",
            static_cast<unsigned long long>(kGetAttachmentRva));
        for (std::size_t i = 0; i < sizeof(kGetAttachmentPrologue) &&
                                used < static_cast<int>(sizeof(line)) - 8; ++i) {
            used += std::snprintf(line + used, sizeof(line) - used, " %02X", site[i]);
        }
        std::snprintf(line + used, sizeof(line) - used, "\n");
        Tf2VrLog(line);
        return false;
    }
    g_getAttachment = reinterpret_cast<GetAttachmentFn>(site);

    // The identity trio. EACH IS CHECKED SEPARATELY AND REPORTED SEPARATELY:
    // a run that has the angles but not the weapon name is still a useful run,
    // and one that silently lost the name would produce an unlabelled
    // per-weapon table that reads exactly like a complete one.
    auto* base = reinterpret_cast<std::uint8_t*>(client);
    const bool playerOk =
        ReadableFrom(base + kGetLocalPlayerRva, sizeof(kGetLocalPlayerPrologue)) &&
        std::memcmp(base + kGetLocalPlayerRva, kGetLocalPlayerPrologue,
                    sizeof(kGetLocalPlayerPrologue)) == 0;
    const bool weaponOk =
        ReadableFrom(base + kGetActiveWeaponRva, sizeof(kGetActiveWeaponPrologue)) &&
        std::memcmp(base + kGetActiveWeaponRva, kGetActiveWeaponPrologue,
                    sizeof(kGetActiveWeaponPrologue)) == 0;
    const bool nameOk =
        ReadableFrom(base + kGetWeaponClassNameRva, sizeof(kGetWeaponClassNamePrologue)) &&
        std::memcmp(base + kGetWeaponClassNameRva, kGetWeaponClassNamePrologue,
                    sizeof(kGetWeaponClassNamePrologue)) == 0;
    if (playerOk) g_getLocalPlayer = reinterpret_cast<GetLocalPlayerFn>(base + kGetLocalPlayerRva);
    if (weaponOk) g_getActiveWeapon = reinterpret_cast<GetActiveWeaponFn>(base + kGetActiveWeaponRva);
    if (nameOk) {
        g_getWeaponClassName =
            reinterpret_cast<GetWeaponClassNameFn>(base + kGetWeaponClassNameRva);
    }

    char line[620]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] ADSPROBE: CBaseAnimating::GetAttachment resolved at client.dll+0x%llX, prologue "
        "verified. Identity: GetLocalPlayer %s, GetActiveWeapon %s, GetWeaponClassName %s. The "
        "ZOOM comes from ads_zoom.cpp, which reports its own four prologues on its own line -- "
        "one owner, so this log describes the numbers C1 actually acts on. Read-only: this probe "
        "calls, and patches nothing.\n",
        static_cast<unsigned long long>(kGetAttachmentRva),
        playerOk ? "ok" : "PROLOGUE MISMATCH -- weapons will be unlabelled",
        weaponOk ? "ok" : "PROLOGUE MISMATCH -- weapons will be unlabelled",
        nameOk ? "ok" : "PROLOGUE MISMATCH -- weapons will be unlabelled");
    Tf2VrLog(line);
    return true;
}

// The whole ADS state, read from the engine in one guarded pass. Returns false
// only if there is no player or no weapon; a missing accessor leaves its field
// at zero and the report says which.
struct ZoomState {
    float fovNow = 0.0f;       // GetFOV: current, blend included
    float fovRest = 0.0f;      // GetFOV latched at frac == 0. Not inferred.
    float fovWeaponZoom = 0.0f;// weapon+0x1364: what THIS weapon declares
    float frac = 0.0f;         // GetZoomFrac: 0 hip, 1 fully aimed
    float magnificationNow = 0.0f;   // rest/now, as tangents
    float declaredRatio = 0.0f;      // rest vs the DECLARED field, before fov scale
    bool valid = false;
};
std::uint64_t g_identityFaults = 0;

ZoomState g_zoom;

// Fills `name` with the game's own weapon class name and returns the class
// index, or -1. Guarded: a wrong player pointer would otherwise fault inside
// the engine call rather than in ours.
// Player and weapon in one guarded pass, so the identity read and the zoom read
// cannot end up looking at two different weapons a frame apart.
bool GetPlayerAndWeapon(void** outPlayer, void** outWeapon) {
    *outPlayer = nullptr;
    *outWeapon = nullptr;
    if (!g_getLocalPlayer || !g_getActiveWeapon) return false;
    __try {
        void* player = g_getLocalPlayer();
        if (!player) return false;
        *outPlayer = player;
        *outWeapon = g_getActiveWeapon(player);
        return *outWeapon != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_identityFaults;
        return false;
    }
}

int ReadWeaponIdentity(char* name, std::size_t cap) {
    std::snprintf(name, cap, "?");
    void* player = nullptr;
    void* weapon = nullptr;
    if (!GetPlayerAndWeapon(&player, &weapon)) return -1;
    __try {
        const auto* bytes = static_cast<const std::uint8_t*>(weapon);
        if (!ReadableFrom(bytes + kWeaponClassIndex, sizeof(int))) return -1;
        const int classIndex = *reinterpret_cast<const int*>(bytes + kWeaponClassIndex);
        if (g_getWeaponClassName) {
            const char* text = g_getWeaponClassName(weapon);
            if (text && ReadableFrom(text, 1)) {
                std::size_t i = 0;
                for (; i + 1 < cap; ++i) {
                    if (!ReadableFrom(text + i, 1)) break;
                    const char c = text[i];
                    if (c == '\0') break;
                    name[i] = (c >= 0x20 && c < 0x7F) ? c : '.';
                }
                name[i] = '\0';
                if (i == 0) std::snprintf(name, cap, "?");
            }
        }
        return classIndex;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_identityFaults;
        std::snprintf(name, cap, "?");
        return -1;
    }
}

// ---- geometry -------------------------------------------------------------

// Source QAngle -> unit forward. pitch is component 0 and is POSITIVE DOWN,
// which is why the z term is negated; the same convention camera_hook uses.
void AnglesToForward(const float angles[3], float out[3]) {
    const float p = angles[0] * kDegToRad;
    const float y = angles[1] * kDegToRad;
    const float cp = std::cos(p);
    out[0] = cp * std::cos(y);
    out[1] = cp * std::sin(y);
    out[2] = -std::sin(p);
}

float AngleBetweenDegrees(const float a[3], const float b[3]) {
    const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    const float lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (la < 1e-6f || lb < 1e-6f) return -1.0f;
    float dot = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb);
    if (dot > 1.0f) dot = 1.0f;
    if (dot < -1.0f) dot = -1.0f;
    return std::acos(dot) * kRadToDeg;
}

// matrix3x4_t is row-major, three rows of four. Column 0 is forward and column
// 3 is the origin -- read out of the engine's own consumers, not assumed:
// ScriptGetAttachmentForward calls MatrixGetColumn(m, 0, out) at 0x62D990, and
// ScriptGetAttachmentOrigin reads [m+0x0C], [m+0x1C], [m+0x2C].
void MatrixForward(const float m[12], float out[3]) {
    out[0] = m[0];
    out[1] = m[4];
    out[2] = m[8];
}
void MatrixOrigin(const float m[12], float out[3]) {
    out[0] = m[3];
    out[1] = m[7];
    out[2] = m[11];
}

// ---- the studiohdr attachment name table ----------------------------------

// Returns the studiohdr_t, or null. WHICH LAYER MATCHED IS REPORTED, because
// "the wrapper is the header on this build" and "the layout moved" are
// otherwise the same silence. See S1 section 2.
const std::uint8_t* ResolveStudioHdr(const std::uint8_t* entity, int* layerOut) {
    if (layerOut) *layerOut = -1;
    if (!ReadableFrom(entity + kStudioHdrWrapper, sizeof(void*))) return nullptr;
    const std::uint8_t* wrapper = nullptr;
    __try {
        wrapper = *reinterpret_cast<const std::uint8_t* const*>(entity + kStudioHdrWrapper);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (!wrapper) return nullptr;

    __try {
        if (ReadableFrom(wrapper, 4) &&
            *reinterpret_cast<const std::uint32_t*>(wrapper) == kStudioMagic) {
            if (layerOut) *layerOut = 0;
            return wrapper;
        }
        if (!ReadableFrom(wrapper + kStudioHdrInner, sizeof(void*))) return nullptr;
        const auto* inner = *reinterpret_cast<const std::uint8_t* const*>(wrapper + kStudioHdrInner);
        if (inner && ReadableFrom(inner, 4) &&
            *reinterpret_cast<const std::uint32_t*>(inner) == kStudioMagic) {
            if (layerOut) *layerOut = 1;
            return inner;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return nullptr;
}

// Fills `out` with the attachment's name, or "?" if it cannot be read. Never
// walks a pointer it has not range-checked: a wrong sznameindex would otherwise
// fault inside the enumeration.
void AttachmentName(const std::uint8_t* studio, int oneBasedIndex, char* out, std::size_t cap) {
    std::snprintf(out, cap, "?");
    if (!studio) return;
    __try {
        const int count = *reinterpret_cast<const int*>(studio + kStudioNumAttachments);
        if (oneBasedIndex < 1 || oneBasedIndex > count) return;
        const int tableOffset = *reinterpret_cast<const int*>(studio + kStudioAttachmentIndex);
        if (tableOffset <= 0) return;
        const std::uint8_t* record = studio + tableOffset +
            static_cast<std::size_t>(oneBasedIndex - 1) * kStudioAttachmentStride;
        if (!ReadableFrom(record, kStudioAttachmentStride)) return;
        const int nameOffset = *reinterpret_cast<const int*>(record);
        if (nameOffset == 0) return;
        const char* name = reinterpret_cast<const char*>(record) + nameOffset;
        if (!ReadableFrom(name, 1)) return;
        std::size_t i = 0;
        for (; i + 1 < cap; ++i) {
            if (!ReadableFrom(name + i, 1)) break;
            const char c = name[i];
            if (c == '\0') break;
            out[i] = (c >= 0x20 && c < 0x7F) ? c : '.';
        }
        out[i] = '\0';
        if (i == 0) std::snprintf(out, cap, "?");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::snprintf(out, cap, "?");
    }
}

void StudioModelName(const std::uint8_t* studio, char* out, std::size_t cap) {
    std::snprintf(out, cap, "?");
    if (!studio) return;
    __try {
        const char* name = reinterpret_cast<const char*>(studio + kStudioNameGuess);
        if (!ReadableFrom(name, 8)) return;
        std::size_t i = 0;
        for (; i + 1 < cap && i < 64; ++i) {
            if (!ReadableFrom(name + i, 1)) break;
            const char c = name[i];
            if (c == '\0') break;
            if (c < 0x20 || c >= 0x7F) { std::snprintf(out, cap, "?"); return; }
            out[i] = c;
        }
        out[i] = '\0';
        if (i == 0) std::snprintf(out, cap, "?");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::snprintf(out, cap, "?");
    }
}

bool EndsWith(const char* text, const char* suffix) {
    const std::size_t n = std::strlen(text);
    const std::size_t m = std::strlen(suffix);
    if (m > n) return false;
    return std::strcmp(text + (n - m), suffix) == 0;
}

bool NameContains(const char* haystack, const char* needle) {
    for (const char* h = haystack; *h; ++h) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b) {
            const char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
            if (ca != *b) break;
            ++a;
            ++b;
        }
        if (!*b) return true;
    }
    return false;
}

// ---- the running state ----------------------------------------------------

const std::uint8_t* g_lastStudio = nullptr;
int g_selected = 0;
// THE REAR SIGHT, WHEN THE MODEL HAS ONE. S2 auto-selected the muzzle and
// measured its forward AXIS, which is the bore. That is a fair proxy and it is
// not what the eye does: sighting down irons is a line from the REAR sight
// through the FRONT one, and the rifle turns out to carry HCOG_FRONT/HCOG_REAR,
// ACGS_FRONT/ACGS_REAR, CRO_FRONT/CRO_REAR and HOLO_FRONT/HOLO_REAR.
//
// So when a _FRONT/_REAR pair exists the sight line is normalize(front origin -
// rear origin), which is the physical thing, and the muzzle axis is the
// fallback. Zero means no pair and the front attachment's own forward is used.
int g_selectedRear = 0;
char g_selectedName[64] = "?";
char g_selectedRearName[64] = "?";
const char* g_selectedRule = "none";
int g_weaponClassIndex = -1;
char g_weaponClassName[64] = "?";

std::uint64_t g_nextSample = 0;
std::uint64_t g_nextReport = 0;

// Sights sit a few units apart on a viewmodel, so anything under this is not a
// sight line -- it is two attachments reporting the same place, which S2 saw
// happen. Normalising that difference would turn float noise into a direction.
constexpr float kMinSightSeparation = 0.25f;

std::uint64_t g_samples = 0;
std::uint64_t g_engineRefused = 0;
std::uint64_t g_pairTooClose = 0;
float g_lastSeparation = 0.0f;
std::uint64_t g_faults = 0;
float g_sightAimMin = 1e9f, g_sightAimMax = -1e9f, g_sightAimLast = -1.0f;
float g_rootAimMin = 1e9f, g_rootAimMax = -1e9f, g_rootAimLast = -1.0f;
float g_sightRootMin = 1e9f, g_sightRootMax = -1e9f, g_sightRootLast = -1.0f;
float g_lastSightFwd[3]{};
float g_lastAimFwd[3]{};
float g_lastOrigin[3]{};

// Which step failed, for the IDLE line. A count with no reason beside it sends
// the next session looking in the wrong place.
const char* g_idleReason = "not sampled yet";
std::uint64_t g_idleTicks = 0;

// A free function rather than a lambda inside Sample(): Sample() uses
// __try/__except throughout, and MSVC refuses a function that mixes structured
// exception handling with anything requiring object unwinding.
void Track(float value, float& low, float& high) {
    if (value < 0.0f) return;  // AngleBetweenDegrees signals "no answer" with -1
    if (value < low) low = value;
    if (value > high) high = value;
}

void ResetWindow() {
    g_samples = 0;
    g_engineRefused = 0;
    g_faults = 0;
    g_idleTicks = 0;
    g_sightAimMin = g_rootAimMin = g_sightRootMin = 1e9f;
    g_sightAimMax = g_rootAimMax = g_sightRootMax = -1e9f;
}

bool CallGetAttachment(const std::uint8_t* entity, int index, float matrix[12]) {
    bool ok = false;
    __try {
        auto* self = const_cast<std::uint8_t*>(entity) + kAnimatingSubObject;
        ok = g_getAttachment(self, index, matrix);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_faults;
        return false;
    }
    return ok;
}

// Fired when the model changes, so it costs nothing in steady state. Bounded by
// kMaxAttachments and reports WHICH bound ended it, per scan_outcome.h.
void EnumerateAttachments(const std::uint8_t* entity, const std::uint8_t* studio, int count,
                          const float aimForward[3], const float rootForward[3]) {
    char model[96]{};
    StudioModelName(studio, model, sizeof(model));

    char header[560]{};
    std::snprintf(header, sizeof(header),
        "[TF2VR] ADSPROBE model CHANGED: WEAPON class=%d \"%s\" declares zoom fov %.1f (weapon+"
        "0x1364, the engine's own per-weapon number -- no table of ours) | studiohdr=%p model=\"%s\" (name "
        "offset UNVERIFIED and expected to read \"?\" on this build, see S1 section 2 -- the class "
        "name above is the real identity) attachments=%d. Each row is that attachment's forward "
        "axis against the aim ray and against the model root, at this instant.\n",
        g_weaponClassIndex, g_weaponClassName, static_cast<double>(g_zoom.fovWeaponZoom),
        static_cast<const void*>(studio), model, count);
    Tf2VrLog(header);

    ScanOutcome outcome;
    int listed = 0;
    // Three preference tiers, resolved AFTER the whole list has been walked
    // rather than while walking it. Choosing during the walk makes the answer
    // depend on attachment order, which is the artist's business and not a
    // rule -- a weapon whose "sight" attachment happens to precede its
    // "muzzle_flash" would silently get a different selection from one whose
    // does not.
    int muzzleIndex = 0, sightIndex = 0, firstIndex = 0;
    char muzzleName[64] = "?", sightName[64] = "?", firstName[64] = "?";
    // Front/rear sight pairs, matched by a shared prefix. Bounded by
    // kMaxAttachments like everything else in this loop.
    struct Named { int index; char name[64]; };
    Named fronts[16]{};
    Named rears[16]{};
    int frontCount = 0, rearCount = 0;

    for (int index = 1; index <= count; ++index) {
        if (listed >= kMaxAttachments) {
            outcome.Finish(ScanOutcome::End::InstanceCap);
            break;
        }
        char name[64]{};
        AttachmentName(studio, index, name, sizeof(name));
        float matrix[12]{};
        const bool ok = CallGetAttachment(entity, index, matrix);
        float forward[3]{}, origin[3]{};
        float toAim = -1.0f, toRoot = -1.0f;
        if (ok) {
            MatrixForward(matrix, forward);
            MatrixOrigin(matrix, origin);
            toAim = AngleBetweenDegrees(forward, aimForward);
            toRoot = AngleBetweenDegrees(forward, rootForward);
        }
        ++listed;

        char line[320]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] ADSPROBE   att[%2d] \"%s\" %s fwd <%.3f,%.3f,%.3f> origin <%.1f,%.1f,%.1f> "
            "| to aim %.2f deg | to root %.2f deg\n",
            index, name, ok ? "ok" : "ENGINE REFUSED", static_cast<double>(forward[0]),
            static_cast<double>(forward[1]), static_cast<double>(forward[2]),
            static_cast<double>(origin[0]), static_cast<double>(origin[1]),
            static_cast<double>(origin[2]), static_cast<double>(toAim),
            static_cast<double>(toRoot));
        Tf2VrLog(line);

        // Only attachments the engine actually accepted are candidates: an
        // index it refuses has no axis to measure.
        if (!ok) continue;
        if (muzzleIndex == 0 && NameContains(name, "muzzle")) {
            muzzleIndex = index;
            std::snprintf(muzzleName, sizeof(muzzleName), "%s", name);
        }
        if (sightIndex == 0 && (NameContains(name, "sight") || NameContains(name, "iron"))) {
            sightIndex = index;
            std::snprintf(sightName, sizeof(sightName), "%s", name);
        }
        if (firstIndex == 0) {
            firstIndex = index;
            std::snprintf(firstName, sizeof(firstName), "%s", name);
        }
        if (EndsWith(name, "_FRONT") && frontCount < 16) {
            fronts[frontCount].index = index;
            std::snprintf(fronts[frontCount].name, sizeof(fronts[frontCount].name), "%s", name);
            ++frontCount;
        } else if (EndsWith(name, "_REAR") && rearCount < 16) {
            rears[rearCount].index = index;
            std::snprintf(rears[rearCount].name, sizeof(rears[rearCount].name), "%s", name);
            ++rearCount;
        }
    }
    outcome.Finish(ScanOutcome::End::Complete);

    // Pair them by prefix. The FIRST pair wins, and which one that is is logged,
    // because a weapon with three optics modelled has three of these and the
    // sight the wearer is actually looking through is whichever bodygroup is
    // switched on -- something this probe cannot see. Naming the pair used means
    // a wrong choice is visible in the log rather than baked into a number.
    int pairFront = 0, pairRear = 0;
    char pairFrontName[64] = "?", pairRearName[64] = "?";
    for (int f = 0; f < frontCount && pairFront == 0; ++f) {
        const std::size_t stem = std::strlen(fronts[f].name) - 6;  // minus "_FRONT"
        for (int r = 0; r < rearCount; ++r) {
            if (std::strlen(rears[r].name) != stem + 5) continue;  // minus "_REAR"
            if (std::strncmp(fronts[f].name, rears[r].name, stem) != 0) continue;
            pairFront = fronts[f].index;
            pairRear = rears[r].index;
            std::snprintf(pairFrontName, sizeof(pairFrontName), "%s", fronts[f].name);
            std::snprintf(pairRearName, sizeof(pairRearName), "%s", rears[r].name);
            break;
        }
    }

    // Muzzle first, because the bore is the axis a round actually leaves along
    // and it is the attachment every weapon has; a named sight second;
    // whatever the engine accepted first last, so there is always an answer.
    int bestIndex = 0;
    int bestRear = 0;
    const char* bestRule = "none: the engine accepted no attachment";
    char bestName[64] = "?";
    char bestRearName[64] = "?";
    if (pairFront != 0) {
        bestIndex = pairFront;
        bestRear = pairRear;
        bestRule = "REAR->FRONT sight pair, which is what sighting down irons physically is";
        std::snprintf(bestName, sizeof(bestName), "%s", pairFrontName);
        std::snprintf(bestRearName, sizeof(bestRearName), "%s", pairRearName);
    } else if (muzzleIndex != 0) {
        bestIndex = muzzleIndex;
        bestRule = "name contains \"muzzle\"";
        std::snprintf(bestName, sizeof(bestName), "%s", muzzleName);
    } else if (sightIndex != 0) {
        bestIndex = sightIndex;
        bestRule = "name contains \"sight\" or \"iron\"";
        std::snprintf(bestName, sizeof(bestName), "%s", sightName);
    } else if (firstIndex != 0) {
        bestIndex = firstIndex;
        bestRule = "FALLBACK: no muzzle or sight name -- first attachment the engine accepted";
        std::snprintf(bestName, sizeof(bestName), "%s", firstName);
    }

    const int forced = g_wantAttachment.load(std::memory_order_relaxed);
    if (forced > 0) {
        g_selected = forced;
        g_selectedRear = 0;
        AttachmentName(studio, forced, g_selectedName, sizeof(g_selectedName));
        std::snprintf(g_selectedRearName, sizeof(g_selectedRearName), "%s", "-");
        g_selectedRule = "pinned by ads.probe_attachment; forward axis, not a pair";
    } else {
        g_selected = bestIndex;
        g_selectedRear = bestRear;
        std::snprintf(g_selectedName, sizeof(g_selectedName), "%s", bestName);
        std::snprintf(g_selectedRearName, sizeof(g_selectedRearName), "%s", bestRearName);
        g_selectedRule = bestRule;
    }

    char tail[420]{};
    if (g_selectedRear != 0) {
        std::snprintf(tail, sizeof(tail),
            "[TF2VR] ADSPROBE listed %d of %d attachments (%d front, %d rear). %s Tracking the "
            "line from att[%d] \"%s\" to att[%d] \"%s\" (%s).\n",
            listed, count, frontCount, rearCount, outcome.Describe(), g_selectedRear,
            g_selectedRearName, g_selected, g_selectedName, g_selectedRule);
    } else {
        std::snprintf(tail, sizeof(tail),
            "[TF2VR] ADSPROBE listed %d of %d attachments (%d front, %d rear). %s Tracking att[%d] "
            "\"%s\" forward axis (%s).\n",
            listed, count, frontCount, rearCount, outcome.Describe(), g_selected, g_selectedName,
            g_selectedRule);
    }
    Tf2VrLog(tail);
}

// ---------------------------------------------------------------------------
// THE CONTROLLED TEST. `set ads.trace = 1`.
//
// The wearer, after several rounds of judging by feel: "We really should
// probably pick a single gun like a pistol and do some controlled tests with
// logging." Right, and overdue -- the last four changes were steered by
// description rather than by number.
//
// One line per sample while ADS is engaged, with every angle that could be
// responsible side by side, so the question "which of these disagrees" is read
// off rather than inferred:
//
//   hand      what the arm is doing, straight from the composition
//   view      worldViewAngles -- what the ENGINE thinks you are looking at
//   attack    attackangles -- what the ROUND follows
//   gun       the viewmodel placement angles as actually committed
//   head      the rendered pitch, and the head's yaw offset from the body
//   mag       the magnification the layer is declaring
//
// AND THE TWO DERIVED NUMBERS THAT SETTLE THE DISPLAY QUESTION. A world
// direction at angle phi from the view axis is DISPLAYED at atan(mag*tan(phi)),
// because the layer declares mag times what was rendered. The gun is fitted to
// the declared tangent, so it is displayed at phi. onScreenAim and onScreenGun
// are those two, and if they differ the gun cannot line up with the world no
// matter how correct its angles are -- which is the difference between an
// aiming bug and a rendering one.
void TraceAds() {
    if (!g_adsTrace.load(std::memory_order_relaxed)) return;
    if (!AdsEngagedByEngine()) return;
    AdsZoomState zoom;
    if (!GetAdsZoom(&zoom)) return;

    float hand[3]{};
    const bool haveHand = TryGetComposedHandPose(nullptr, hand);
    float attack[3]{};
    const bool haveAttack = TryGetAimAnglesDegrees(attack);
    float headYawDelta = 0.0f, headPitch = 0.0f;
    const bool haveHead = GetHeadViewDelta(&headYawDelta, &headPitch);
    float engineView[3]{};
    const bool haveView = TryGetEngineViewAngles(engineView);
    float gunAngles[3]{}, gunBase[3]{};
    const bool haveGun = TryGetPlacementAngles(gunAngles, gunBase);

    // Displayed angle for a world direction that far off the view axis, and for
    // the gun at the same angle. Vertical only: it is the axis the report is
    // about and one number is easier to compare than two.
    const float phi = haveHand && haveHead ? (hand[0] - headPitch) : 0.0f;
    const float onScreenAim =
        std::atan(zoom.magnification * std::tan(phi * kDegToRad)) * kRadToDeg;
    const float onScreenGun = phi;

    char line[700]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] ADSTRACE %s | hand p%.2f y%.2f | view p%.2f y%.2f | attack p%.2f y%.2f | "
        "gun p%.2f y%.2f (engine had p%.2f y%.2f) | head pitch %.2f yawDelta %.2f | mag %.2fx | "
        "gun-vs-view pitch %+.2f -> the WORLD shows that direction at %+.2f on screen and the GUN "
        "at %+.2f, a %+.2f deg display gap that is not an aiming error\n",
        g_weaponClassName, static_cast<double>(haveHand ? hand[0] : 0.0f),
        static_cast<double>(haveHand ? hand[1] : 0.0f),
        static_cast<double>(haveView ? engineView[0] : 0.0f),
        static_cast<double>(haveView ? engineView[1] : 0.0f),
        static_cast<double>(haveAttack ? attack[0] : 0.0f),
        static_cast<double>(haveAttack ? attack[1] : 0.0f),
        static_cast<double>(haveGun ? gunAngles[0] : 0.0f),
        static_cast<double>(haveGun ? gunAngles[1] : 0.0f),
        static_cast<double>(haveGun ? gunBase[0] : 0.0f),
        static_cast<double>(haveGun ? gunBase[1] : 0.0f),
        static_cast<double>(haveHead ? headPitch : 0.0f),
        static_cast<double>(haveHead ? headYawDelta : 0.0f),
        static_cast<double>(zoom.magnification), static_cast<double>(phi),
        static_cast<double>(onScreenAim), static_cast<double>(onScreenGun),
        static_cast<double>(onScreenAim - onScreenGun));
    Tf2VrLog(line);
}

void Report() {
    float worldTanX = 0.0f, worldTanY = 0.0f;
    const bool haveFrustum = GetMainSceneHalfTangents(worldTanX, worldTanY);
    const float ratchetRest = RestingWorldHalfTanX();
    const float plateauRest = PlateauRestingWorldHalfTanX();

    if (g_samples == 0) {
        char line[420]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] ADSPROBE IDLE ticks=%llu: %s | ADS zoomed=%d worldTan %.4f x %.4f "
            "(ratchet rest %.4f, plateau rest %.4f, stable %llu) | engine refused %llu faults %llu. "
            "This line IS the probe reporting; silence would not be alignment.\n",
            static_cast<unsigned long long>(g_idleTicks), g_idleReason,
            WorldZoomLatched() ? 1 : 0, static_cast<double>(haveFrustum ? worldTanX : 0.0f),
            static_cast<double>(haveFrustum ? worldTanY : 0.0f),
            static_cast<double>(ratchetRest), static_cast<double>(plateauRest),
            static_cast<unsigned long long>(PlateauRestStableSamples()),
            static_cast<unsigned long long>(g_engineRefused),
            static_cast<unsigned long long>(g_faults));
        Tf2VrLog(line);
        ResetWindow();
        return;
    }

    const float ratchetRatio =
        (haveFrustum && ratchetRest > 0.0001f) ? worldTanX / ratchetRest : 0.0f;
    const float plateauRatio =
        (haveFrustum && plateauRest > 0.0001f) ? worldTanX / plateauRest : 0.0f;
    // The magnification the wearer is actually being given, which is what
    // ads.max_magnification will cap. 1/ratio, because a narrower frustum in
    // the same buffer is a magnification by exactly that factor.
    const float magnification = plateauRatio > 0.0001f ? 1.0f / plateauRatio : 0.0f;

    // C1's falsifier, carried in the same line as the ratio it is about, so
    // "the cap is set" and "the cap did anything" are never inferred from each
    // other. widened=0 with a cap set and the wearer in ADS is a defect;
    // widened>0 while un-zoomed is the failure mode xr.match_headset_fov had.
    unsigned long long capWidened = 0, capUntouched = 0;
    float capMeasured = 0.0f;
    ReadAdsCapCounters(&capWidened, &capUntouched, &capMeasured);

    char line[1600]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] ADSPROBE n=%llu WEAPON class=%d \"%s\" | sight %s att[%d] \"%s\" "
        "| ZOOM(engine) fov now %.1f rest %.1f weapon-declares %.1f frac %.2f -> MAGNIFICATION now %.2fx (live, and this is the one C1 uses); declared-value ratio %.2f, which is NOT a magnification -- see the note in ads_zoom.cpp "
        "| ADS zoomed=%d worldTan %.4f x %.4f "
        "ratio ratchet %.3f plateau %.3f -> magnification %.2fx (rests %.4f / %.4f, stable %llu) "
        "| SIGHT-vs-AIM %.2f deg (min %.2f max %.2f) | root-vs-aim %.2f deg (min %.2f max %.2f) "
        "[CONTROL: with the pin hand-driven this must be ~0; a large value means the frame "
        "conversion is wrong and the headline is meaningless] | SIGHT-vs-ROOT %.2f deg "
        "(min %.2f max %.2f) [the artist's offset, the per-weapon constant S4 corrects] "
        "| sight fwd <%.3f,%.3f,%.3f> aim fwd <%.3f,%.3f,%.3f> muzzle <%.1f,%.1f,%.1f> "
        "| CAP %.2fx measured %.2fx widened %llu untouched %llu "
        "| engine refused %llu faults %llu pair-too-close %llu sep %.2f\n",
        static_cast<unsigned long long>(g_samples), g_weaponClassIndex, g_weaponClassName,
        g_selectedRear != 0 ? "REAR->FRONT" : "forward-axis", g_selected, g_selectedName,
        static_cast<double>(g_zoom.fovNow), static_cast<double>(g_zoom.fovRest),
        static_cast<double>(g_zoom.fovWeaponZoom), static_cast<double>(g_zoom.frac),
        static_cast<double>(g_zoom.magnificationNow),
        static_cast<double>(g_zoom.declaredRatio),
        WorldZoomLatched() ? 1 : 0, static_cast<double>(worldTanX), static_cast<double>(worldTanY),
        static_cast<double>(ratchetRatio), static_cast<double>(plateauRatio),
        static_cast<double>(magnification), static_cast<double>(ratchetRest),
        static_cast<double>(plateauRest),
        static_cast<unsigned long long>(PlateauRestStableSamples()),
        static_cast<double>(g_sightAimLast), static_cast<double>(g_sightAimMin),
        static_cast<double>(g_sightAimMax), static_cast<double>(g_rootAimLast),
        static_cast<double>(g_rootAimMin), static_cast<double>(g_rootAimMax),
        static_cast<double>(g_sightRootLast), static_cast<double>(g_sightRootMin),
        static_cast<double>(g_sightRootMax), static_cast<double>(g_lastSightFwd[0]),
        static_cast<double>(g_lastSightFwd[1]), static_cast<double>(g_lastSightFwd[2]),
        static_cast<double>(g_lastAimFwd[0]), static_cast<double>(g_lastAimFwd[1]),
        static_cast<double>(g_lastAimFwd[2]), static_cast<double>(g_lastOrigin[0]),
        static_cast<double>(g_lastOrigin[1]), static_cast<double>(g_lastOrigin[2]),
        static_cast<double>(AdsMaxMagnification()), static_cast<double>(capMeasured),
        capWidened, capUntouched,
        static_cast<unsigned long long>(g_engineRefused),
        static_cast<unsigned long long>(g_faults),
        static_cast<unsigned long long>(g_pairTooClose),
        static_cast<double>(g_lastSeparation));
    Tf2VrLog(line);
    ResetWindow();
}

// Rows forward, right, up. The SAME formula as AngleBasis in
// camera_update_hook.cpp and the grip block in hand_pose.cpp, copied rather
// than linked (AngleBasis is file-local there); a convention shared by copy is
// still one convention, and the control row below would expose a divergence.
void GripBasis(float pitchDeg, float yawDeg, float rollDeg, float basis[9]) {
    const float sp = std::sin(pitchDeg * kDegToRad), cp = std::cos(pitchDeg * kDegToRad);
    const float sy = std::sin(yawDeg * kDegToRad), cy = std::cos(yawDeg * kDegToRad);
    const float sr = std::sin(rollDeg * kDegToRad), cr = std::cos(rollDeg * kDegToRad);
    basis[0] = cp * cy;                    // forward
    basis[1] = cp * sy;
    basis[2] = -sp;
    basis[3] = -sr * sp * cy + cr * sy;    // right
    basis[4] = -sr * sp * sy - cr * cy;
    basis[5] = -sr * cp;
    basis[6] = cr * sp * cy + sr * sy;     // up
    basis[7] = cr * sp * sy - sr * cy;
    basis[8] = cr * cp;
}

// ---------------------------------------------------------------------------
// GRIP CENSUS -- KICKOFF-GUN-ALIGN-2026-09-03 rung 1. READ-ONLY.
//
// Per weapon in hand, where the model's own R_HAND (and L_HAND) attachment sits
// in the VIEWMODEL's local frame: position as forward/right/up from the model
// origin, and the attachment's orientation relative to the model's own angles.
// Both come from the engine's world-space attachment matrix with the entity's
// committed placement (+0x12C origin, +0x114 angles) subtracted, so the number
// is the one the pin would need: "which model-local point sits on the hand".
//
// THE CONTROL. The pin places the model origin at hand + R*grip (hand_pose.cpp,
// the grip block), so the model-local point that sits on the hand today is
// exactly -grip, and on the weapon the wearer calibrated the R_HAND row must
// land within a couple of units of that. If it does not, the frame is wrong
// and every other row is void -- the row says which, and PASS names its gate.
//
// It runs BEFORE the aim-angle gate so it does not need aim.cmd, and it uses
// the same basis convention as hand_pose.cpp / AngleBasis (rows forward, right,
// up) so a sign error here cannot disagree with the convention the pin uses.
// Bounded: at most one engine call per hand per sample, one line a second.
//
// The studiohdr record's own local matrix is read too (record+0x0C, bone at
// record+0x08: stock mstudioattachment_t is 0x5C bytes -- sznameindex, flags,
// localbone, matrix3x4 local, int unused[8] -- so the 0x5C stride IS stock).
// It is BONE-relative, so it is reported as context, not as the answer.
constexpr std::size_t kAttRecordLocalBone = 0x08;
constexpr std::size_t kAttRecordLocalMatrix = 0x0C;
constexpr float kGripControlUnits = 2.5f;

struct GripStat {
    float low[3], high[3], sum[3];
    int n;
    void Reset() {
        for (int i = 0; i < 3; ++i) { low[i] = 1e9f; high[i] = -1e9f; sum[i] = 0.0f; }
        n = 0;
    }
    void Add(const float v[3]) {
        for (int i = 0; i < 3; ++i) {
            if (v[i] < low[i]) low[i] = v[i];
            if (v[i] > high[i]) high[i] = v[i];
            sum[i] += v[i];
        }
        ++n;
    }
    float Mean(int i) const { return n > 0 ? sum[i] / static_cast<float>(n) : 0.0f; }
    float Spread(int i) const { return n > 0 ? high[i] - low[i] : 0.0f; }
};

const std::uint8_t* g_gripStudio = nullptr;
int g_gripIndex[2] = {0, 0};            // 1-based attachment index, 0 = absent
char g_gripName[2][32] = {"?", "?"};
int g_gripLocalBone[2] = {-1, -1};
float g_gripRecordOrigin[2][3]{};
GripStat g_gripPos[2], g_gripAng[2];
// THE PLAYER'S OWN MOTION over the same window as the attachment samples. The
// attachment can sit perfectly still in the weapon's frame while the player
// sprints, so the attachment's own spread cannot tell idle from steady.
GripStat g_gripBase;
// THE SAME SAMPLE IN BOTH CANDIDATE FRAMES. Which placement CallGetAttachment
// built its bones from decides which origin and basis the attachment must be
// measured against, and a flat census cannot tell them apart because without a
// pin they are the same pose. Measured both ways, the census row is the known
// answer: whichever frame reproduces it is the frame the bones are in.
float g_gripPinDisplace[3]{};
float g_gripEngineOrigin[3]{}, g_gripEngineAngles[3]{};
bool g_gripEngineValid = false;
// Why a latch did not happen, so a weapon that never latches says which gate
// held it rather than going quiet.
std::uint64_t g_gripHeldByAds = 0;
std::uint64_t g_gripHeldByMotion = 0;
std::uint64_t g_gripHeldBySpread = 0;
std::uint64_t g_gripHeldByPin = 0;
std::uint64_t g_gripHeldBySettle = 0;
// Which placement each sample was measured against. A latch taken while
// g_gripAgainstStructFields was rising is contaminated by our own pin.
std::uint64_t g_gripAgainstEngineBase = 0;
std::uint64_t g_gripAgainstStructFields = 0;
std::uint64_t g_gripNextReport = 0;
std::uint64_t g_gripNextIdle = 0;
std::uint64_t g_gripRefused = 0;
std::uint64_t g_gripFaults = 0;
std::uint64_t g_gripPlacementUnreadable = 0;
std::uint64_t g_gripSamples = 0;
int g_gripClassIndex = -1;
char g_gripClassName[64] = "?";

bool NameEqualsNoCase(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
        const char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + 32) : *b;
        if (ca != cb) return false;
        if (ca == '\0') return true;
    }
}

void GripCensusReset() {
    for (int h = 0; h < 2; ++h) {
        g_gripPos[h].Reset();
        g_gripAng[h].Reset();
    }
    g_gripBase.Reset();
}

// ---- PER-WEAPON GRIP: the latch and the published delta (ads_probe.h) ------
//
// One R_HAND per class index, latched from the first steady second (five or
// more samples, spread under half a unit) the weapon is held. First-steady
// wins: the draw animation ends in idle, and idle is the pose the pin should
// glue to the hand. A later window never overwrites, so the term is a constant
// per weapon per session and the log records exactly when it was taken.
//
// The delta the composition reads is three atomics, republished whenever the
// weapon in hand or its latch changes; a torn read across them costs one frame
// of a mixed delta on a weapon swap and nothing else.
constexpr int kGripLatchCap = 64;
constexpr int kGripLatchMinSamples = 5;
constexpr float kGripLatchMaxSpread = 0.5f;
// How far the PLAYER may drift over the sample window and still count as at
// rest, in source units. Idle bob and breathing are well under a unit; a walk
// crosses several units a second. Generous on purpose: failing this gate costs
// nothing but a later latch, and a weapon that never latches falls back to the
// dialled grip, which is never catastrophically wrong.
constexpr float kGripIdleMaxMotion = 2.0f;
// HOW FAR A MEASURED R_HAND MAY SIT FROM THE REFERENCE AND STILL BE BELIEVED,
// per axis, in source units.
//
// This is a plausibility guard, not an explanation. The eight weapons of the
// 2026-09-03 census span right 2.48..4.07 and up -4.11..-7.60 -- rifles,
// shotguns, LMGs and pistols all inside 1.6 units of each other on right and
// 3.5 on up, with forward the only axis that really moves (6.04..13.10). A
// ninth weapon reading right 8.57 and up -21.86 is not a rifle grip fifteen
// inches below every other rifle's; it is a measurement of something that is
// not the grip. The wearer settled it independently: grip.perweapon = 0 puts
// the Flatline right, so its true correction is about zero.
//
// A weapon outside these bounds gets NO correction rather than a large wrong
// one, which is exactly the state the wearer confirmed is correct for it. Why
// this model measures where it does is still open and is written up in
// KNOWN-ISSUES 13.
constexpr float kGripTrustFwd = 9.0f;
constexpr float kGripTrustRight = 4.0f;
constexpr float kGripTrustUp = 5.0f;
std::uint64_t g_gripRefusedImplausible = 0;

struct GripLatch { int classIndex; float rhand[3]; };
GripLatch g_gripLatch[kGripLatchCap]{};
int g_gripLatched = 0;
std::atomic_bool g_perWeapon = false;
std::atomic<float> g_gripRef[3] = {0.0f, 0.0f, 0.0f};
std::atomic_bool g_gripRefSet[3] = {false, false, false};
std::atomic<float> g_gripDelta[3] = {0.0f, 0.0f, 0.0f};
std::atomic_bool g_gripDeltaValid = false;
std::atomic_int g_gripDeltaClass = -1;
std::uint64_t g_gripLatchOverflow = 0;
std::uint64_t g_gripBoundTick = 0;
// A latch may only be taken once the draw animation has ended. The first VR
// run latched the P2016 at level entry, before the pin armed, at a frozen
// intro pose 10 units off, and first-steady-wins kept it all session.
constexpr std::uint64_t kGripLatchSettleMs = 1500;
// How long the live-vs-table control may keep the sampler open before it gives
// up. Past this the weapon is seeded, placement is correct, and the only thing
// still running is a verification that has not found its moment.
constexpr std::uint64_t kGripControlDeadlineMs = 10000;

// THE POSITIVE CONTROL FOR THE CORRECTED MEASUREMENT. The seed table is eight
// weapons whose right answer is already known, and until now a seeded weapon
// was never measured live at all -- which is exactly why a broken live latch
// could sit here unnoticed while every weapon the wearer tried looked right.
// With the placement corrected, a live measurement of a seeded weapon must
// reproduce its table row. One comparison per class, then sampling stops, so
// the engine call this costs is bounded by construction.
int g_gripControlClass = -1;
bool g_gripControlDone = true;
float g_gripControlTable[3]{};

// THE SEED TABLE -- measured flat at the gun range, 2026-09-03 (the VR run
// reproduced every value to 0.01). Keyed by class NAME, which survives a
// class-index reshuffle; the class index is only the session key. A weapon
// in this table is correct on first pickup and costs no engine call at all; a
// weapon not in it latches from its first steady idle second and prints the
// row to add here. Titan weapons (r_hand_ik, another frame) are not seeded.
struct GripSeed { const char* className; float rhand[3]; };
constexpr GripSeed kGripSeeds[] = {
    {"mp_weapon_rspn101",        { 6.04f, 2.73f, -6.09f}},   // R-201, the reference
    {"mp_weapon_shotgun",        { 6.69f, 3.51f, -5.98f}},   // EVA-8
    {"mp_weapon_dmr",            { 7.39f, 4.07f, -5.99f}},   // Longbow
    {"mp_weapon_lmg",            { 7.56f, 3.76f, -7.60f}},   // Spitfire
    {"mp_weapon_alternator_smg", {12.30f, 3.67f, -5.54f}},
    {"mp_weapon_semipistol",     {13.10f, 2.65f, -4.11f}},   // P2016
    {"mp_weapon_autopistol",     {13.10f, 2.65f, -4.12f}},   // RE-45
    {"mp_weapon_wingman",        {12.78f, 2.48f, -4.82f}},
    // V-47 FLATLINE, the first campaign level's rifle and the first weapon
    // a new player sees. Censused 2026-09-11 in a headset run rather than
    // flat, because it is not offered at the range: latched at rest in the
    // pinned frame, delta <-1.68,-0.90,+0.06>, wearer-confirmed on the hand.
    // Seeded so it is right on the frame it is drawn -- unseeded it waited
    // fifteen seconds to latch and then visibly snapped two inches.
    {"mp_weapon_vinson",         { 7.72f, 3.63f, -6.15f}},
};

const GripLatch* FindGripLatch(int classIndex) {
    for (int i = 0; i < g_gripLatched; ++i) {
        if (g_gripLatch[i].classIndex == classIndex) return &g_gripLatch[i];
    }
    return nullptr;
}

bool GripReferenceComplete() {
    return g_gripRefSet[0].load(std::memory_order_acquire) &&
           g_gripRefSet[1].load(std::memory_order_acquire) &&
           g_gripRefSet[2].load(std::memory_order_acquire);
}

// Recomputes the published delta for the weapon in hand, and says why when it
// cannot. Called on bind, on latch, on toggle and on a reference change.
void PublishGripDelta(const char* why) {
    const int cls = g_gripClassIndex;
    g_gripDeltaClass.store(cls, std::memory_order_relaxed);
    const GripLatch* latch = FindGripLatch(cls);
    const bool armed = g_perWeapon.load(std::memory_order_acquire);
    const bool refOk = GripReferenceComplete();
    float delta[3] = {0.0f, 0.0f, 0.0f};
    const bool valid = armed && refOk && latch != nullptr && cls >= 0;
    if (valid) {
        for (int i = 0; i < 3; ++i) {
            delta[i] = g_gripRef[i].load(std::memory_order_acquire) - latch->rhand[i];
        }
    }
    for (int i = 0; i < 3; ++i) g_gripDelta[i].store(delta[i], std::memory_order_release);
    g_gripDeltaValid.store(valid, std::memory_order_release);
    char line[400]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] GRIP per-weapon term %s (%s): class=%d \"%s\" delta fwd/right/up <%.2f,%.2f,%.2f> "
        "| armed %d, reference %s, latched %s | effective grip = hand.grip_* + this.\n",
        valid ? "APPLIED" : "ZERO", why, cls, g_gripClassName,
        static_cast<double>(delta[0]), static_cast<double>(delta[1]), static_cast<double>(delta[2]),
        armed ? 1 : 0, refOk ? "set" : "MISSING (set grip.ref_fwd/right/up)",
        latch ? "yes" : "NOT YET (hold the weapon still for a second)");
    Tf2VrLog(line);
}

// Live-versus-table for a seeded weapon, once. Read-only: it never writes a
// latch, so the gun is placed from the table exactly as before whatever this
// says. It only answers "does the corrected live path reproduce a known-good
// number", which is the question the vinson could not answer for itself.
void GripMaybeControl() {
    if (g_gripControlDone || g_gripControlClass < 0) return;
    if (g_gripClassIndex != g_gripControlClass) return;
    // A DEADLINE, BECAUSE "BOUNDED BY CONSTRUCTION" WAS FALSE.
    //
    // Six gates have to line up at once for the control to fire, and nothing
    // made them. Until they did, the sampling early-out stayed open and
    // CallGetAttachment -- which runs the entity's bone setup on this thread,
    // and which this file's own comment calls a cost the wearer can feel --
    // ran on every sample for the rest of the session, on a SEEDED weapon that
    // used to cost nothing. The Flatline case that motivated all this took 99
    // seconds to line up under a LOOSER gate.
    //
    // Worse, one of those gates could never pass at all: g_gripBase is only fed
    // when TryGetPlacementPosition and TryGetPlacementAngles both succeed, so
    // if either read fails the idle test blocks forever while the counter
    // blames motion.
    //
    // Ten seconds of trying, then give up and stop paying. The control is a
    // nicety -- it re-verifies a census row -- and a nicety does not get to
    // cost frames indefinitely.
    if (GetTickCount64() - g_gripBoundTick > kGripControlDeadlineMs) {
        g_gripControlDone = true;
        char line[300]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] GRIP CONTROL class=%d: gave up after %llu ms without a still, non-ADS moment. "
            "Sampling stops; the table row stands unverified this session.\n",
            g_gripControlClass, static_cast<unsigned long long>(kGripControlDeadlineMs));
        Tf2VrLog(line);
        return;
    }
    if (g_gripPos[0].n < kGripLatchMinSamples) return;
    if (!(IsPlacementPinHandDriven() && IsViewmodelPlacementPinArmed())) return;
    if (GetTickCount64() - g_gripBoundTick < kGripLatchSettleMs) return;
    // THE SAME IDLE GATE THE LATCH USES. A control taken mid-sprint would
    // disagree with a table row measured at rest and read as the live path
    // failing, which is the one mistake this control exists to prevent.
    if (AdsEngagedByEngine()) return;
    const float motion = std::sqrt(g_gripBase.Spread(0) * g_gripBase.Spread(0) +
                                   g_gripBase.Spread(1) * g_gripBase.Spread(1) +
                                   g_gripBase.Spread(2) * g_gripBase.Spread(2));
    if (g_gripBase.n < kGripLatchMinSamples || motion > kGripIdleMaxMotion) return;
    const float spread = std::sqrt(g_gripPos[0].Spread(0) * g_gripPos[0].Spread(0) +
                                   g_gripPos[0].Spread(1) * g_gripPos[0].Spread(1) +
                                   g_gripPos[0].Spread(2) * g_gripPos[0].Spread(2));
    if (spread > kGripLatchMaxSpread) return;
    g_gripControlDone = true;
    const float live[3] = {g_gripPos[0].Mean(0), g_gripPos[0].Mean(1), g_gripPos[0].Mean(2)};
    const float d[3] = {live[0] - g_gripControlTable[0], live[1] - g_gripControlTable[1],
                        live[2] - g_gripControlTable[2]};
    const float err = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] GRIP CONTROL class=%d \"%s\": live R_HAND <%.2f,%.2f,%.2f> vs table "
        "<%.2f,%.2f,%.2f> -- error <%+.2f,%+.2f,%+.2f>, %.2f units over %d samples (spread %.2f). "
        "The placement read succeeded %llu times and failed %llu (the measurement is in the "
        "struct frame either way). PASS is "
        "error under 1.5 units: the live path then reproduces a known-good row and the table is "
        "redundant.\n",
        g_gripControlClass, g_gripClassName,
        static_cast<double>(live[0]), static_cast<double>(live[1]), static_cast<double>(live[2]),
        static_cast<double>(g_gripControlTable[0]), static_cast<double>(g_gripControlTable[1]),
        static_cast<double>(g_gripControlTable[2]),
        static_cast<double>(d[0]), static_cast<double>(d[1]), static_cast<double>(d[2]),
        static_cast<double>(err), g_gripPos[0].n, static_cast<double>(spread),
        static_cast<unsigned long long>(g_gripAgainstEngineBase),
        static_cast<unsigned long long>(g_gripAgainstStructFields));
    Tf2VrLog(line);
    // THE FRAME A/B LIVED HERE AND IS REMOVED, 2026-09-11. It answered, three
    // runs running: pinned-struct err 0.00, 0.01, 0.00 against a census row,
    // engine-base 10.16, 8.46, 14.63. The bones are built at the viewmodel's
    // CURRENT fields and the measurement belongs in that frame. Keeping a
    // second projection of every sample alive to re-answer a settled question
    // is exactly the kind of cost this week was spent removing.

}

// One steady window on the weapon in hand: latch it if it is not latched.
void GripMaybeLatch() {
    const int cls = g_gripClassIndex;
    if (cls < 0 || g_gripPos[0].n < kGripLatchMinSamples) return;
    if (FindGripLatch(cls)) return;
    // Only a pose the pin is actually driving, and only past the draw
    // animation: the engine's own intro/holster placement is a real pose with
    // a zero spread and the wrong answer.
    if (!(IsPlacementPinHandDriven() && IsViewmodelPlacementPinArmed())) { ++g_gripHeldByPin; return; }
    if (GetTickCount64() - g_gripBoundTick < kGripLatchSettleMs) { ++g_gripHeldBySettle; return; }
    // IDLE, NOT MERELY STEADY. 2026-09-11: the census that produced the seed
    // table ran flat at the gun range, where you pick a weapon up and stand
    // there, so its first steady second WAS idle. The first campaign level
    // gives neither condition -- the Flatline is drawn in a cutscene, so the
    // just-drawn window is missed, and it latched 99 seconds in, mid-firefight,
    // at spread 0.39: well inside the old gate. A sprint hold is steady. ADS is
    // steady. The attachment rides an animated bone and is only the grip point
    // at rest, so a gate that says "still" when it means "at rest" records a
    // combat pose as the weapon's grip.
    if (AdsEngagedByEngine()) { ++g_gripHeldByAds; return; }
    const float motion = std::sqrt(g_gripBase.Spread(0) * g_gripBase.Spread(0) +
                                   g_gripBase.Spread(1) * g_gripBase.Spread(1) +
                                   g_gripBase.Spread(2) * g_gripBase.Spread(2));
    if (g_gripBase.n < kGripLatchMinSamples || motion > kGripIdleMaxMotion) {
        ++g_gripHeldByMotion;
        return;
    }
    const float spread = std::sqrt(g_gripPos[0].Spread(0) * g_gripPos[0].Spread(0) +
                                   g_gripPos[0].Spread(1) * g_gripPos[0].Spread(1) +
                                   g_gripPos[0].Spread(2) * g_gripPos[0].Spread(2));
    if (spread > kGripLatchMaxSpread) { ++g_gripHeldBySpread; return; }
    if (g_gripLatched >= kGripLatchCap) {
        ++g_gripLatchOverflow;
        return;
    }
    // THE PLAUSIBILITY GUARD, before anything is written down.
    {
        float ref[3]{};
        if (ReadGripReference(ref)) {
            const float d[3] = {g_gripPos[0].Mean(0) - ref[0], g_gripPos[0].Mean(1) - ref[1],
                                g_gripPos[0].Mean(2) - ref[2]};
            if (std::fabs(d[0]) > kGripTrustFwd || std::fabs(d[1]) > kGripTrustRight ||
                std::fabs(d[2]) > kGripTrustUp) {
                ++g_gripRefusedImplausible;
                static int told[kGripLatchCap]{};
                static int toldCount = 0;
                bool seen = false;
                for (int i = 0; i < toldCount; ++i) {
                    if (told[i] == cls) { seen = true; break; }
                }
                if (!seen && toldCount < kGripLatchCap) {
                    told[toldCount++] = cls;
                    char refuse[420]{};
                    std::snprintf(refuse, sizeof(refuse),
                        "[TF2VR] GRIP REFUSED class=%d \"%s\": measured R_HAND <%.2f,%.2f,%.2f> is "
                        "<%+.2f,%+.2f,%+.2f> from the reference, outside the trusted band "
                        "(%.1f/%.1f/%.1f) that all eight censused weapons sit inside. NOT latched: this "
                        "weapon uses the dialled grip with no correction, which is the state the wearer "
                        "confirmed is right for it. Why it measures here is KNOWN-ISSUES 13.\n",
                        cls, g_gripClassName,
                        static_cast<double>(g_gripPos[0].Mean(0)),
                        static_cast<double>(g_gripPos[0].Mean(1)),
                        static_cast<double>(g_gripPos[0].Mean(2)),
                        static_cast<double>(d[0]), static_cast<double>(d[1]),
                        static_cast<double>(d[2]),
                        static_cast<double>(kGripTrustFwd), static_cast<double>(kGripTrustRight),
                        static_cast<double>(kGripTrustUp));
                    Tf2VrLog(refuse);
                }
                return;
            }
        }
    }
    GripLatch& slot = g_gripLatch[g_gripLatched++];
    slot.classIndex = cls;
    {
        char why[340]{};
        std::snprintf(why, sizeof(why),
            "[TF2VR] GRIP IDLE GATE: latched at rest after holds refused for ads %llu, motion %llu, "
            "attachment spread %llu, pin %llu, settle %llu. Player drift %.2f units over %d samples "
            "(gate %.1f).\n",
            static_cast<unsigned long long>(g_gripHeldByAds),
            static_cast<unsigned long long>(g_gripHeldByMotion),
            static_cast<unsigned long long>(g_gripHeldBySpread),
            static_cast<unsigned long long>(g_gripHeldByPin),
            static_cast<unsigned long long>(g_gripHeldBySettle),
            static_cast<double>(motion), g_gripBase.n,
            static_cast<double>(kGripIdleMaxMotion));
        Tf2VrLog(why);
    }
    for (int i = 0; i < 3; ++i) slot.rhand[i] = g_gripPos[0].Mean(i);
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] GRIP LATCHED class=%d \"%s\": R_HAND local fwd/right/up <%.2f,%.2f,%.2f> from %d "
        "samples, spread %.2f (gate %.1f). Constant for this session; %d weapons latched.\n",
        cls, g_gripClassName, static_cast<double>(slot.rhand[0]), static_cast<double>(slot.rhand[1]),
        static_cast<double>(slot.rhand[2]), g_gripPos[0].n, static_cast<double>(spread),
        static_cast<double>(kGripLatchMaxSpread), g_gripLatched);
    Tf2VrLog(line);
    // THE TWO-FRAME LATCH LINE WENT WITH THE A/B. The frame question is
    // settled -- pinned-struct, err 0.00 against a census row -- so recording a
    // second frame's answer for every latch is recording the losing arm of a
    // finished experiment.
    PublishGripDelta("latched");
}

// Which attachment is the hand, per model. Pilot viewmodels name it R_HAND /
// L_HAND; titan weapons r_hand_ik / l_hand_ik. Exact names, case-insensitive,
// so a similarly named attachment cannot be mistaken for the hand.
void GripCensusBindModel(const std::uint8_t* vm, const std::uint8_t* studio, int count) {
    g_gripStudio = studio;
    GripCensusReset();
    g_gripClassIndex = ReadWeaponIdentity(g_gripClassName, sizeof(g_gripClassName));
    g_gripBoundTick = GetTickCount64();
    // Seed from the table before anything else, so a known weapon is right on
    // the frame it is drawn and never waits for a latch.
    if (g_gripClassIndex >= 0 && !FindGripLatch(g_gripClassIndex) && g_gripLatched < kGripLatchCap) {
        for (const GripSeed& seed : kGripSeeds) {
            if (std::strcmp(seed.className, g_gripClassName) != 0) continue;
            GripLatch& slot = g_gripLatch[g_gripLatched++];
            slot.classIndex = g_gripClassIndex;
            for (int i = 0; i < 3; ++i) slot.rhand[i] = seed.rhand[i];
            char seeded[200]{};
            std::snprintf(seeded, sizeof(seeded),
                "[TF2VR] GRIP SEEDED class=%d \"%s\" from the table: R_HAND <%.2f,%.2f,%.2f>.\n",
                g_gripClassIndex, g_gripClassName, static_cast<double>(seed.rhand[0]),
                static_cast<double>(seed.rhand[1]), static_cast<double>(seed.rhand[2]));
            Tf2VrLog(seeded);
            g_gripControlClass = g_gripClassIndex;
            g_gripControlDone = false;
            for (int i = 0; i < 3; ++i) g_gripControlTable[i] = seed.rhand[i];
            break;
        }
    }
    for (int h = 0; h < 2; ++h) {
        g_gripIndex[h] = 0;
        g_gripLocalBone[h] = -1;
        std::snprintf(g_gripName[h], sizeof(g_gripName[h]), "?");
        g_gripRecordOrigin[h][0] = g_gripRecordOrigin[h][1] = g_gripRecordOrigin[h][2] = 0.0f;
    }
    const int limit = count < kMaxAttachments ? count : kMaxAttachments;
    for (int index = 1; index <= limit && studio; ++index) {
        char name[64]{};
        AttachmentName(studio, index, name, sizeof(name));
        int h = -1;
        if (NameEqualsNoCase(name, "R_HAND") || NameEqualsNoCase(name, "r_hand_ik")) h = 0;
        else if (NameEqualsNoCase(name, "L_HAND") || NameEqualsNoCase(name, "l_hand_ik")) h = 1;
        if (h < 0 || g_gripIndex[h] != 0) continue;
        g_gripIndex[h] = index;
        std::snprintf(g_gripName[h], sizeof(g_gripName[h]), "%s", name);
        __try {
            const int tableOffset = *reinterpret_cast<const int*>(studio + kStudioAttachmentIndex);
            const std::uint8_t* record = studio + tableOffset +
                static_cast<std::size_t>(index - 1) * kStudioAttachmentStride;
            if (tableOffset > 0 && ReadableFrom(record, kStudioAttachmentStride)) {
                g_gripLocalBone[h] = *reinterpret_cast<const int*>(record + kAttRecordLocalBone);
                const auto* local = reinterpret_cast<const float*>(record + kAttRecordLocalMatrix);
                g_gripRecordOrigin[h][0] = local[3];
                g_gripRecordOrigin[h][1] = local[7];
                g_gripRecordOrigin[h][2] = local[11];
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ++g_gripFaults;
        }
    }
    char line[400]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] GRIP model bound: class=%d \"%s\" attachments=%d | right hand %s att[%d] "
        "(bone %d, record-local origin <%.2f,%.2f,%.2f> bone-relative) | left hand %s att[%d] "
        "(bone %d) | pin target %s this viewmodel.\n",
        g_gripClassIndex, g_gripClassName, count,
        g_gripIndex[0] ? g_gripName[0] : "NOT FOUND", g_gripIndex[0], g_gripLocalBone[0],
        static_cast<double>(g_gripRecordOrigin[0][0]), static_cast<double>(g_gripRecordOrigin[0][1]),
        static_cast<double>(g_gripRecordOrigin[0][2]),
        g_gripIndex[1] ? g_gripName[1] : "NOT FOUND", g_gripIndex[1], g_gripLocalBone[1],
        reinterpret_cast<std::uintptr_t>(vm) == g_placementInstance ? "IS" : "is NOT");
    Tf2VrLog(line);
    PublishGripDelta("weapon changed");
}

// Source MatrixAngles on a local (forward, left, up) triple. pitch positive
// down, same convention as AnglesToForward above.
void LocalAxesToAngles(const float f[3], const float l[3], const float u[3], float out[3]) {
    const float xy = std::sqrt(f[0] * f[0] + f[1] * f[1]);
    if (xy > 0.001f) {
        out[1] = std::atan2(f[1], f[0]) * kRadToDeg;
        out[0] = std::atan2(-f[2], xy) * kRadToDeg;
        out[2] = std::atan2(l[2], u[2]) * kRadToDeg;
    } else {
        out[1] = std::atan2(-l[0], l[1]) * kRadToDeg;
        out[0] = std::atan2(-f[2], xy) * kRadToDeg;
        out[2] = 0.0f;
    }
}

void GripCensusSample(const std::uint8_t* vm, int count) {
    int layer = -1;
    const std::uint8_t* studio = ResolveStudioHdr(vm, &layer);
    if (!studio) return;  // counted by the ADS probe's own idle reason
    if (studio != g_gripStudio) GripCensusBindModel(vm, studio, count);
    // No engine call in ordinary play: with the ADS probe off, a weapon that is
    // seeded or latched has nothing left to measure. GetAttachment runs the
    // entity's bone setup on this thread, and that is a cost the wearer can
    // feel, so it is spent only on a weapon the table does not know.
    // ... and a seeded weapon keeps sampling only until its control has
    // reported once. g_gripControlDone starts true, so a weapon the table does
    // not know costs nothing extra.
    if (!g_enabled.load(std::memory_order_acquire) && FindGripLatch(g_gripClassIndex) &&
        g_gripControlDone) {
        return;
    }

    float angles[3]{}, origin[3]{};
    __try {
        if (!ReadableFrom(vm + kViewmodelAnglesOffset, sizeof(float) * 3) ||
            !ReadableFrom(vm + kViewmodelPositionOffset, sizeof(float) * 3)) {
            ++g_gripPlacementUnreadable;
            return;
        }
        std::memcpy(angles, vm + kViewmodelAnglesOffset, sizeof(angles));
        std::memcpy(origin, vm + kViewmodelPositionOffset, sizeof(origin));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_gripFaults;
        return;
    }
    // MEASURED IN THE PINNED POSE, WHICH IS THE POSE THE BONES ARE IN.
    //
    // 2026-09-11, GRIP FRAME A/B on mp_weapon_semipistol: the pinned-struct
    // frame reproduced the census row <13.10,2.65,-4.11> with error 0.00, and
    // the engine-base frame was 10.16 units out. CallGetAttachment runs the
    // entity's bone setup on demand and builds it at the viewmodel's CURRENT
    // fields -- the pose the pin wrote -- so the attachment must be measured
    // against those same fields. The measurement is then independent of where
    // we pinned the weapon, which is why it reproduces a flat census exactly.
    //
    // An earlier build subtracted the engine's committed placement instead, on
    // the reasoning that the pin contaminates the struct. It does not: both the
    // attachment and the origin move with the pin, so the difference cancels.
    // That change made the Flatline's applied delta <-4.77,-26.63,+17.95>
    // instead of <-1.68,-5.84,+15.77> -- 26 units of left shift that the wearer
    // saw and reported. A flat census cannot tell the two frames apart, because
    // with no pin they are the same pose.
    float structOrigin[3]{}, structAngles[3]{};
    std::memcpy(structOrigin, origin, sizeof(structOrigin));
    std::memcpy(structAngles, angles, sizeof(structAngles));
    {
        float baseOrigin[3]{}, baseAngles[3]{};
        if (TryGetPlacementPosition(nullptr, baseOrigin) &&
            TryGetPlacementAngles(nullptr, baseAngles)) {
            // Read for the idle test and the frame A/B only. The sampling
            // frame stays the struct pose; this no longer replaces it.
            for (int i = 0; i < 3; ++i) g_gripPinDisplace[i] = structOrigin[i] - baseOrigin[i];
            std::memcpy(g_gripEngineOrigin, baseOrigin, sizeof(g_gripEngineOrigin));
            std::memcpy(g_gripEngineAngles, baseAngles, sizeof(g_gripEngineAngles));
            g_gripEngineValid = true;
            ++g_gripAgainstEngineBase;
            // The same read doubles as the idle test: this is the player's
            // placement, so its spread over the window IS how far they moved.
            g_gripBase.Add(baseOrigin);
        } else {
            // Counted, not silent: if this is the arm that ran, the numbers
            // below are the old contaminated ones and must not be read as a
            // property of the weapon.
            ++g_gripAgainstStructFields;
        }
    }
    float basis[9]{};
    GripBasis(angles[0], angles[1], angles[2], basis);
    const float* fwd = basis;
    const float* right = basis + 3;
    const float* up = basis + 6;

    for (int h = 0; h < 2; ++h) {
        if (g_gripIndex[h] == 0) continue;
        float m[12]{};
        if (!CallGetAttachment(vm, g_gripIndex[h], m)) {
            ++g_gripRefused;
            continue;
        }
        const float d[3] = {m[3] - origin[0], m[7] - origin[1], m[11] - origin[2]};
        const float local[3] = {
            d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2],
            d[0] * right[0] + d[1] * right[1] + d[2] * right[2],
            d[0] * up[0] + d[1] * up[1] + d[2] * up[2]};
        // The attachment's axes -- columns 0 (forward), 1 (left), 2 (up) --
        // in the model's frame, then as Source angles. Expressed on the
        // (x forward, y left, z up) convention MatrixAngles expects, so the
        // right-row dot is negated into a left component.
        float af[3], al[3], au[3];
        const float cols[3][3] = {{m[0], m[4], m[8]}, {m[1], m[5], m[9]}, {m[2], m[6], m[10]}};
        float* outs[3] = {af, al, au};
        for (int c = 0; c < 3; ++c) {
            const float* v = cols[c];
            outs[c][0] = v[0] * fwd[0] + v[1] * fwd[1] + v[2] * fwd[2];
            outs[c][1] = -(v[0] * right[0] + v[1] * right[1] + v[2] * right[2]);
            outs[c][2] = v[0] * up[0] + v[1] * up[1] + v[2] * up[2];
        }
        float rel[3]{};
        LocalAxesToAngles(af, al, au, rel);
        g_gripPos[h].Add(local);
        g_gripAng[h].Add(rel);
    }
    ++g_gripSamples;
}

void GripCensusReport() {
    const std::uint64_t now = GetTickCount64();
    if (now < g_gripNextReport) return;
    g_gripNextReport = now + kReportIntervalMs;
    GripMaybeLatch();
    GripMaybeControl();
    if (!g_enabled.load(std::memory_order_acquire)) {
        GripCensusReset();
        return;
    }

    if (g_gripPos[0].n == 0) {
        if (now < g_gripNextIdle) return;
        g_gripNextIdle = now + 5000;
        char idle[300]{};
        std::snprintf(idle, sizeof(idle),
            "[TF2VR] GRIP idle: no right-hand sample this second (model %s, right hand att[%d], "
            "samples %llu, engine refused %llu, placement unreadable %llu, faults %llu).\n",
            g_gripStudio ? "bound" : "not bound", g_gripIndex[0],
            static_cast<unsigned long long>(g_gripSamples),
            static_cast<unsigned long long>(g_gripRefused),
            static_cast<unsigned long long>(g_gripPlacementUnreadable),
            static_cast<unsigned long long>(g_gripFaults));
        Tf2VrLog(idle);
        return;
    }

    float gf = 0.0f, gr = 0.0f, gu = 0.0f, of = 0.0f, ol = 0.0f, ou = 0.0f;
    ReadGripOffset(&gf, &gr, &gu);
    ReadHandPositionOffset(&of, &ol, &ou);
    // What the pin puts on the hand today: the model-local point -grip.
    const float expect[3] = {-gf, -gr, -gu};
    const float delta[3] = {g_gripPos[0].Mean(0) - expect[0], g_gripPos[0].Mean(1) - expect[1],
                            g_gripPos[0].Mean(2) - expect[2]};
    const float miss = std::sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
    const float spread = std::sqrt(g_gripPos[0].Spread(0) * g_gripPos[0].Spread(0) +
                                   g_gripPos[0].Spread(1) * g_gripPos[0].Spread(1) +
                                   g_gripPos[0].Spread(2) * g_gripPos[0].Spread(2));

    char line[1200]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] GRIP class=%d \"%s\" size %.2f pin=%s | R_HAND local fwd/right/up "
        "<%.2f,%.2f,%.2f> spread <%.2f,%.2f,%.2f> n=%d | R_HAND rel angles p/y/r <%.1f,%.1f,%.1f> "
        "spread <%.1f,%.1f,%.1f> | L_HAND local <%.2f,%.2f,%.2f> rel <%.1f,%.1f,%.1f> n=%d "
        "| CONTROL: live grip puts model point <%.2f,%.2f,%.2f> on the hand (player-frame offset "
        "<%.2f fwd,%.2f left,%.2f up> beside it); R_HAND minus that = <%.2f,%.2f,%.2f> = %.2f units "
        "-> %s (gate %.1f units, valid only on the weapon the grip was calibrated on; on any other "
        "weapon this delta IS the per-weapon term) | steady %s (spread %.2f) | refused %llu faults %llu\n",
        g_gripClassIndex, g_gripClassName, static_cast<double>(WeaponSize()),
        IsPlacementPinHandDriven() && IsViewmodelPlacementPinArmed() ? "hand" : "engine",
        static_cast<double>(g_gripPos[0].Mean(0)), static_cast<double>(g_gripPos[0].Mean(1)),
        static_cast<double>(g_gripPos[0].Mean(2)), static_cast<double>(g_gripPos[0].Spread(0)),
        static_cast<double>(g_gripPos[0].Spread(1)), static_cast<double>(g_gripPos[0].Spread(2)),
        g_gripPos[0].n,
        static_cast<double>(g_gripAng[0].Mean(0)), static_cast<double>(g_gripAng[0].Mean(1)),
        static_cast<double>(g_gripAng[0].Mean(2)), static_cast<double>(g_gripAng[0].Spread(0)),
        static_cast<double>(g_gripAng[0].Spread(1)), static_cast<double>(g_gripAng[0].Spread(2)),
        static_cast<double>(g_gripPos[1].Mean(0)), static_cast<double>(g_gripPos[1].Mean(1)),
        static_cast<double>(g_gripPos[1].Mean(2)), static_cast<double>(g_gripAng[1].Mean(0)),
        static_cast<double>(g_gripAng[1].Mean(1)), static_cast<double>(g_gripAng[1].Mean(2)),
        g_gripPos[1].n,
        static_cast<double>(expect[0]), static_cast<double>(expect[1]), static_cast<double>(expect[2]),
        static_cast<double>(of), static_cast<double>(ol), static_cast<double>(ou),
        static_cast<double>(delta[0]), static_cast<double>(delta[1]), static_cast<double>(delta[2]),
        static_cast<double>(miss), miss <= kGripControlUnits ? "PASS" : "FAIL",
        static_cast<double>(kGripControlUnits),
        spread <= 0.5f ? "YES" : "NO (animating or moving; use a steady second)",
        static_cast<double>(spread),
        static_cast<unsigned long long>(g_gripRefused),
        static_cast<unsigned long long>(g_gripFaults));
    Tf2VrLog(line);
    GripCensusReset();
}

// The census alone: viewmodel, attachment count, sample, window. Used when
// the ADS probe is off and the per-weapon grip term is on.
void GripOnlySample() {
    const std::uint8_t* vm = FindLiveViewmodelInstance();
    if (!vm) return;
    int count = 0;
    __try {
        if (!ReadableFrom(vm + kAttachmentCount, sizeof(int))) return;
        count = *reinterpret_cast<const int*>(vm + kAttachmentCount);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_gripFaults;
        return;
    }
    if (count < 1) return;
    GripCensusSample(vm, count);
    GripCensusReport();
}

void Sample() {
    g_idleReason = "sampling";

    // THE ZOOM FIRST, AND IT DOES NOT DEPEND ON THE VIEWMODEL. Every other
    // measurement here needs a live C_BaseViewModel; this one needs a player and
    // a weapon, so it keeps working through the states where the attachment half
    // reports IDLE. Read every sample, because the fraction moves through the
    // ADS animation and the interesting numbers are on that ramp.
    {
        // From ads_zoom, the single owner, so the probe reports exactly the
        // numbers C1 acts on. A second copy here could disagree with the one
        // doing the work, and the log would be describing the wrong thing.
        AdsZoomState z;
        if (GetAdsZoom(&z)) {
            g_zoom.fovNow = z.fovNow;
            g_zoom.fovRest = z.fovRest;
            g_zoom.fovWeaponZoom = z.fovDeclared;
            g_zoom.frac = z.frac;
            g_zoom.magnificationNow = z.magnification;
            g_zoom.declaredRatio = z.magnificationAtFull;
            g_zoom.valid = true;
        }
    }

    const std::uint8_t* vm = FindLiveViewmodelInstance();
    if (!vm) {
        g_idleReason = "no live C_BaseViewModel (not in a level, or no weapon drawn)";
        ++g_idleTicks;
        return;
    }
    int count = 0;
    __try {
        if (!ReadableFrom(vm + kAttachmentCount, sizeof(int))) {
            g_idleReason = "viewmodel+0x11D8 not readable";
            ++g_idleTicks;
            return;
        }
        count = *reinterpret_cast<const int*>(vm + kAttachmentCount);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_faults;
        g_idleReason = "faulted reading the attachment count";
        ++g_idleTicks;
        return;
    }
    if (count < 1) {
        g_idleReason = "the viewmodel reports zero attachments";
        ++g_idleTicks;
        return;
    }

    // GRIP CENSUS, ahead of the aim-angle gate: it needs the viewmodel and its
    // studiohdr and nothing else, so a flat run without aim.cmd still reports.
    GripCensusSample(vm, count);
    GripCensusReport();

    float aimAngles[3]{};
    if (!TryGetAimAnglesDegrees(aimAngles)) {
        g_idleReason = "no aim angles yet -- the command seam needs `set aim.cmd = 1` "
                       "(state Log reads and writes nothing)";
        ++g_idleTicks;
        return;
    }
    float aimForward[3]{};
    AnglesToForward(aimAngles, aimForward);

    float rootAngles[3]{};
    __try {
        if (!ReadableFrom(vm + kViewmodelAnglesOffset, sizeof(float) * 3)) {
            g_idleReason = "viewmodel placement angles not readable";
            ++g_idleTicks;
            return;
        }
        std::memcpy(rootAngles, vm + kViewmodelAnglesOffset, sizeof(rootAngles));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_faults;
        g_idleReason = "faulted reading the viewmodel placement angles";
        ++g_idleTicks;
        return;
    }
    float rootForward[3]{};
    AnglesToForward(rootAngles, rootForward);

    int layer = -1;
    const std::uint8_t* studio = ResolveStudioHdr(vm, &layer);
    if (studio != g_lastStudio) {
        g_lastStudio = studio;
        // Read the identity BEFORE the enumeration burst, so the burst's own
        // header carries the weapon's name rather than the previous weapon's.
        g_weaponClassIndex = ReadWeaponIdentity(g_weaponClassName, sizeof(g_weaponClassName));
        if (!studio) {
            Tf2VrLog("[TF2VR] ADSPROBE: no IDST magic at viewmodel+0x1208 nor at its +0x08. "
                     "Attachment NAMES are unavailable this model; indices and angles still "
                     "work, because those come from the engine call and not from the header.\n");
            g_selected = g_wantAttachment.load(std::memory_order_relaxed) > 0
                             ? g_wantAttachment.load(std::memory_order_relaxed)
                             : 1;
            std::snprintf(g_selectedName, sizeof(g_selectedName), "?");
            g_selectedRule = "no studiohdr: index only";
        } else {
            char which[200]{};
            std::snprintf(which, sizeof(which),
                "[TF2VR] ADSPROBE: studiohdr found at %s (IDST verified).\n",
                layer == 0 ? "viewmodel+0x1208 DIRECTLY -- the wrapper indirection is not on this "
                             "build"
                           : "*(viewmodel+0x1208)+0x08, the CStudioHdr wrapper, as S1 derived");
            Tf2VrLog(which);
            EnumerateAttachments(vm, studio, count, aimForward, rootForward);
        }
    }

    if (g_selected < 1 || g_selected > count) {
        g_idleReason = "the selected attachment index is outside this model's range";
        ++g_idleTicks;
        return;
    }

    float matrix[12]{};
    if (!CallGetAttachment(vm, g_selected, matrix)) {
        ++g_engineRefused;
        g_idleReason = "the engine refused the attachment this tick (bone setup not ready) -- "
                       "that is a real answer, not a missing probe";
        ++g_idleTicks;
        return;
    }

    float sightForward[3]{};
    MatrixForward(matrix, sightForward);
    MatrixOrigin(matrix, g_lastOrigin);
    // REAR THROUGH FRONT, when the model carries the pair. The two origins are
    // fetched in the same sample, so they are one pose and not two.
    //
    // A separation guard, because this is a difference of two nearly equal
    // world positions: sights sit a few units apart, and if the engine ever
    // hands back the same origin for both -- an attachment not placed this
    // frame, which S2 saw happen to SWAY_ROTATE_ZOOMED and HOLO_FRONT -- the
    // normalise would amplify float noise into a wild direction. Below the
    // guard the front attachment's own forward axis is used and the sample is
    // counted as a fallback rather than silently mixed in with the good ones.
    if (g_selectedRear != 0) {
        float rearMatrix[12]{};
        if (CallGetAttachment(vm, g_selectedRear, rearMatrix)) {
            float rearOrigin[3]{};
            MatrixOrigin(rearMatrix, rearOrigin);
            const float dx = g_lastOrigin[0] - rearOrigin[0];
            const float dy = g_lastOrigin[1] - rearOrigin[1];
            const float dz = g_lastOrigin[2] - rearOrigin[2];
            const float separation = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (separation >= kMinSightSeparation) {
                sightForward[0] = dx;
                sightForward[1] = dy;
                sightForward[2] = dz;
                g_lastSeparation = separation;
            } else {
                ++g_pairTooClose;
                g_lastSeparation = separation;
            }
        } else {
            ++g_engineRefused;
        }
    }
    std::memcpy(g_lastSightFwd, sightForward, sizeof(g_lastSightFwd));
    std::memcpy(g_lastAimFwd, aimForward, sizeof(g_lastAimFwd));

    g_sightAimLast = AngleBetweenDegrees(sightForward, aimForward);
    g_rootAimLast = AngleBetweenDegrees(rootForward, aimForward);
    g_sightRootLast = AngleBetweenDegrees(sightForward, rootForward);

    Track(g_sightAimLast, g_sightAimMin, g_sightAimMax);
    Track(g_rootAimLast, g_rootAimMin, g_rootAimMax);
    Track(g_sightRootLast, g_sightRootMin, g_sightRootMax);
    ++g_samples;
}

}  // namespace

void SetAdsProbeEnabled(bool enabled) {
    const bool was = g_enabled.exchange(enabled, std::memory_order_release);
    if (was == enabled) return;
    if (enabled) {
        Tf2VrLog("[TF2VR] ADSPROBE armed (ads.probe = 1). READ-ONLY: it calls "
                 "CBaseAnimating::GetAttachment and writes nothing, not even the FOV-adjust byte "
                 "the engine's own NoFOVAdjust wrapper clears. It needs `set aim.cmd = 1` for the "
                 "aim ray to exist.\n");
        g_nextSample = 0;
        g_nextReport = 0;
        g_lastStudio = nullptr;
        ResetWindow();
    } else {
        Tf2VrLog("[TF2VR] ADSPROBE disarmed (ads.probe = 0).\n");
    }
}

bool IsAdsProbeEnabled() { return g_enabled.load(std::memory_order_acquire); }

void SetAdsProbeAttachment(int index) {
    g_wantAttachment.store(index < 0 ? 0 : index, std::memory_order_relaxed);
    // Force a re-selection on the next sample rather than waiting for the model
    // to change, so changing this from the ini and reloading takes effect.
    g_lastStudio = nullptr;
}

void SetPerWeaponGrip(bool enabled) {
    const bool was = g_perWeapon.exchange(enabled, std::memory_order_acq_rel);
    if (was == enabled) return;
    Tf2VrLog(enabled ? "[TF2VR] GRIP per-weapon term ON (grip.perweapon = 1; bare F5 toggles it).\n"
                     : "[TF2VR] GRIP per-weapon term OFF (one grip for every weapon, the old behaviour).\n");
    PublishGripDelta(enabled ? "armed" : "disarmed");
}

void TogglePerWeaponGrip() { SetPerWeaponGrip(!g_perWeapon.load(std::memory_order_acquire)); }

bool PerWeaponGripArmed() { return g_perWeapon.load(std::memory_order_acquire); }

void SetGripReference(int axis, float value) {
    if (axis < 0 || axis > 2) return;
    g_gripRef[axis].store(value, std::memory_order_release);
    g_gripRefSet[axis].store(true, std::memory_order_release);
    if (GripReferenceComplete()) PublishGripDelta("reference set");
}

bool ReadGripReference(float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = g_gripRef[i].load(std::memory_order_acquire);
    return GripReferenceComplete();
}

bool TryGetPerWeaponGripDelta(float delta[3]) {
    if (!g_gripDeltaValid.load(std::memory_order_acquire)) {
        delta[0] = delta[1] = delta[2] = 0.0f;
        return false;
    }
    for (int i = 0; i < 3; ++i) delta[i] = g_gripDelta[i].load(std::memory_order_acquire);
    return true;
}

bool TryGetLatchedRightHand(int* classIndex, float out[3]) {
    const int cls = g_gripDeltaClass.load(std::memory_order_acquire);
    const GripLatch* latch = FindGripLatch(cls);
    if (!latch) return false;
    if (classIndex) *classIndex = cls;
    for (int i = 0; i < 3; ++i) out[i] = latch->rhand[i];
    return true;
}

void AdvanceAdsProbe() {
    // The per-weapon grip term needs the census sampling even with the ADS
    // probe off; it runs the same reads on the same clock and prints only
    // its latch and publish lines.
    const bool probe = g_enabled.load(std::memory_order_acquire);
    if (!probe && !g_perWeapon.load(std::memory_order_acquire)) return;
    if (!EnsureResolved()) return;

    const std::uint64_t now = GetTickCount64();
    if (g_nextSample == 0) {
        g_nextSample = now;
        g_nextReport = now + kReportIntervalMs;
    }
    if (now >= g_nextSample) {
        g_nextSample = now + kSampleIntervalMs;
        if (probe) {
            Sample();
        } else {
            GripOnlySample();
        }
        // On the SAMPLE clock, not per frame: ten lines a second while ADS is
        // held is a readable trace, and one per frame is a wall.
        if (probe) TraceAds();
    }
    if (now >= g_nextReport) {
        g_nextReport = now + kReportIntervalMs;
        if (probe) Report();
    }
}

void SetAdsTrace(bool enabled) {
    const bool was = g_adsTrace.exchange(enabled, std::memory_order_release);
    if (was == enabled) return;
    Tf2VrLog(enabled
        ? "[TF2VR] ads.trace = 1: ADSTRACE writes one line per sample while ADS is engaged. Pick "
          "ONE weapon and move only one axis at a time -- the point is a controlled comparison, "
          "and the line puts every candidate angle beside every other so the disagreeing pair is "
          "read off rather than guessed at.\n"
        : "[TF2VR] ads.trace = 0.\n");
}

bool IsAdsTrace() { return g_adsTrace.load(std::memory_order_acquire); }
