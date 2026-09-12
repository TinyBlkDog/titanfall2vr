#include "ads_zoom.h"

#include "diagnostics.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// All four addresses derived offline and prologue-checked before any call. See
// S1-ATTACHMENT-API-2026-08-26.md section 3 and S2E-RESULT-2026-08-26.md.
constexpr std::uintptr_t kGetLocalPlayerRva = 0x0014EF00;
constexpr std::uintptr_t kGetActiveWeaponRva = 0x000B19C0;
constexpr std::uintptr_t kGetFovRva = 0x002C5D00;
constexpr std::uintptr_t kGetZoomFracRva = 0x002C8440;
constexpr std::size_t kWeaponZoomFov = 0x1364;

constexpr std::uint8_t kGetLocalPlayerPrologue[] = {0x8B, 0x05};
constexpr std::uint8_t kGetActiveWeaponPrologue[] = {
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x01, 0x48, 0x89, 0x5C, 0x24, 0x30,
};
constexpr std::uint8_t kGetFovPrologue[] = {
    0x48, 0x8B, 0xC4, 0x53, 0x48, 0x83, 0xEC, 0x70,
};
constexpr std::uint8_t kGetZoomFracPrologue[] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x0F, 0x29, 0x74, 0x24, 0x20,
};

using VoidFn = void*(__fastcall*)();
using FromPtrFn = void*(__fastcall*)(void*);
using FloatFromPtrFn = float(__fastcall*)(void*);

VoidFn g_getLocalPlayer = nullptr;
FromPtrFn g_getActiveWeapon = nullptr;
FloatFromPtrFn g_getFov = nullptr;
FloatFromPtrFn g_getZoomFrac = nullptr;
bool g_resolveAttempted = false;
bool g_resolved = false;

constexpr float kDegToRad = 0.01745329252f;

std::atomic<float> g_fovRest{0.0f};
std::atomic<float> g_fovNow{0.0f};
std::atomic<float> g_fovDeclared{0.0f};
std::atomic<float> g_frac{0.0f};
std::atomic<float> g_magnification{1.0f};
std::atomic<float> g_magnificationAtFull{1.0f};
std::atomic<float> g_fovScale{0.0f};
std::atomic_bool g_valid{false};
std::atomic_uint64_t g_faults{0};

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

bool Matches(const std::uint8_t* base, std::uintptr_t rva, const std::uint8_t* bytes,
             std::size_t count) {
    return ReadableFrom(base + rva, count) && std::memcmp(base + rva, bytes, count) == 0;
}

// EVERY ADDRESS CHECKED, AND WHICH ONES FAILED IS NAMED. A partial resolve is
// the failure mode that matters: with GetFOV up and GetZoomFrac down, "at rest"
// would silently become "whatever fov was seen first", which is precisely the
// running-maximum defect this module exists to delete.
bool EnsureResolved() {
    if (g_resolved) return true;
    if (g_resolveAttempted) return false;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return false;  // retry next frame; client.dll loads late
    g_resolveAttempted = true;
    auto* base = reinterpret_cast<std::uint8_t*>(client);

    const bool playerOk =
        Matches(base, kGetLocalPlayerRva, kGetLocalPlayerPrologue, sizeof(kGetLocalPlayerPrologue));
    const bool weaponOk = Matches(base, kGetActiveWeaponRva, kGetActiveWeaponPrologue,
                                  sizeof(kGetActiveWeaponPrologue));
    const bool fovOk = Matches(base, kGetFovRva, kGetFovPrologue, sizeof(kGetFovPrologue));
    const bool fracOk =
        Matches(base, kGetZoomFracRva, kGetZoomFracPrologue, sizeof(kGetZoomFracPrologue));

    if (playerOk) g_getLocalPlayer = reinterpret_cast<VoidFn>(base + kGetLocalPlayerRva);
    if (weaponOk) g_getActiveWeapon = reinterpret_cast<FromPtrFn>(base + kGetActiveWeaponRva);
    if (fovOk) g_getFov = reinterpret_cast<FloatFromPtrFn>(base + kGetFovRva);
    if (fracOk) g_getZoomFrac = reinterpret_cast<FloatFromPtrFn>(base + kGetZoomFracRva);

    g_resolved = playerOk && weaponOk && fovOk && fracOk;
    char line[520]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] ADS zoom source: GetLocalPlayer %s, GetActiveWeapon %s, GetFOV %s, GetZoomFrac "
        "%s. %s The magnification is read from the engine per weapon and live -- no table, no "
        "rendered-frustum observation, no calibrated constant.\n",
        playerOk ? "ok" : "PROLOGUE MISMATCH", weaponOk ? "ok" : "PROLOGUE MISMATCH",
        fovOk ? "ok" : "PROLOGUE MISMATCH", fracOk ? "ok" : "PROLOGUE MISMATCH",
        g_resolved ? "All four verified."
                   : "NOT ALL VERIFIED, so this reports nothing rather than guessing -- a partial "
                     "resolve would turn 'at rest' back into a running maximum, which is the "
                     "defect this exists to remove.");
    Tf2VrLog(line);
    return g_resolved;
}

float TangentRatio(float wideDeg, float narrowDeg) {
    if (wideDeg <= 0.1f || narrowDeg <= 0.1f) return 1.0f;
    const float w = std::tan(wideDeg * 0.5f * kDegToRad);
    const float n = std::tan(narrowDeg * 0.5f * kDegToRad);
    if (n <= 1e-6f) return 1.0f;
    const float ratio = w / n;
    return ratio < 1.0f ? 1.0f : ratio;
}

// PLAN-TITAN-2026-08-28 section 4 riders, both edge-triggered so they cost one
// line per weapon change rather than one per frame.
//
// ZOOMDECL: the `weapon + 0x1364` read on the CURRENT weapon, logged whenever
// it changes -- "log, never trust". A titan weapon is a different object, and
// what this field holds there (a sane fov, a zero, garbage) has never been
// seen. The line is the seeing.
//
// ZOOMFAULT: g_faults counted every swallowed access violation in the guarded
// read path and NOTHING LOGGED IT -- the aggregate-counter-hides-silent-sites
// defect, in the exact module that is suspect #3 for the titan crash. A fault
// swallowed here also spends one of the crash recorder's four first-chance
// slots, so a run where this line appears must read its CRASH records against
// it: same-address first-chance records with the log continuing afterwards are
// THIS read being caught, not the process dying.
void ReportEdges() {
    static float lastDeclared = -1.0f;
    static std::uint64_t lastFaults = 0;

    const std::uint64_t faults = g_faults.load(std::memory_order_relaxed);
    if (faults != lastFaults) {
        char line[300]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] ZOOMFAULT: the guarded ADS zoom read faulted and was swallowed (total %llu, "
            "+%llu). On a titan weapon this is the +0x1364/GetFOV/GetZoomFrac path misreading a "
            "shape it has never seen -- it also consumes first-chance CRASH record slots.\n",
            static_cast<unsigned long long>(faults),
            static_cast<unsigned long long>(faults - lastFaults));
        Tf2VrLog(line);
        lastFaults = faults;
    }

    if (!g_valid.load(std::memory_order_acquire)) return;
    const float declared = g_fovDeclared.load(std::memory_order_acquire);
    if (declared != lastDeclared) {
        char line[260]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] ZOOMDECL: weapon+0x1364 declared zoom fov %.2f (was %.2f) | fovNow %.2f "
            "frac %.3f magAtFull %.3f. A new value means a weapon change; this is the read's "
            "value ON that weapon, logged rather than trusted.\n",
            declared, lastDeclared, g_fovNow.load(std::memory_order_acquire),
            g_frac.load(std::memory_order_acquire),
            g_magnificationAtFull.load(std::memory_order_acquire));
        Tf2VrLog(line);
        lastDeclared = declared;
    }
}

}  // namespace

void TickAdsZoom() {
    // Before the early returns, so a fault edge is reported even on frames
    // where the player or resolve is missing -- a rider gated on an unrelated
    // read succeeding is the mistake this project has already paid for.
    ReportEdges();
    if (!EnsureResolved()) return;
    __try {
        void* player = g_getLocalPlayer();
        if (!player) return;
        void* weapon = g_getActiveWeapon(player);
        const float fovNow = g_getFov(player);
        const float frac = g_getZoomFrac(player);
        if (fovNow <= 0.1f) return;

        float declared = 0.0f;
        if (weapon) {
            const auto* bytes = static_cast<const std::uint8_t*>(weapon);
            if (ReadableFrom(bytes + kWeaponZoomFov, sizeof(float))) {
                declared = *reinterpret_cast<const float*>(bytes + kWeaponZoomFov);
            }
        }

        // AT REST IS DECLARED BY THE ENGINE, NOT INFERRED FROM A MAXIMUM.
        // frac == 0 is hip. The fov at that instant IS the resting fov, so a
        // transient cannot latch it and a held zoom cannot be mistaken for it.
        if (frac <= 0.001f) g_fovRest.store(fovNow, std::memory_order_release);
        const float rest = g_fovRest.load(std::memory_order_acquire);

        // cl_fovScale, derived from the engine's own behaviour at full blend
        // rather than read from a setting that could drift out of step with it.
        // Measured 1.549..1.551 across six weapons, so a value far outside that
        // means something has changed and the guard below refuses it.
        if (frac >= 0.999f && declared > 0.1f) {
            const float scale = fovNow / declared;
            if (scale > 1.0f && scale < 4.0f) g_fovScale.store(scale, std::memory_order_release);
        }
        const float scale = g_fovScale.load(std::memory_order_acquire);

        g_fovNow.store(fovNow, std::memory_order_release);
        g_fovDeclared.store(declared, std::memory_order_release);
        g_frac.store(frac, std::memory_order_release);
        g_magnification.store(TangentRatio(rest, fovNow), std::memory_order_release);
        // What this weapon WILL magnify to at full ADS. A property of the gun,
        // so it is already correct while the blend is still ramping -- which is
        // what lets the optic decision be made on the ADS transition rather
        // than a few frames into it.
        g_magnificationAtFull.store(
            (scale > 0.0f && declared > 0.1f) ? TangentRatio(rest, declared * scale) : 1.0f,
            std::memory_order_release);
        g_valid.store(rest > 0.1f, std::memory_order_release);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        g_valid.store(false, std::memory_order_release);
    }
}

bool GetAdsZoom(AdsZoomState* out) {
    if (!out) return false;
    if (!g_valid.load(std::memory_order_acquire)) return false;
    out->valid = true;
    out->fovRest = g_fovRest.load(std::memory_order_acquire);
    out->fovNow = g_fovNow.load(std::memory_order_acquire);
    out->fovDeclared = g_fovDeclared.load(std::memory_order_acquire);
    out->frac = g_frac.load(std::memory_order_acquire);
    out->magnification = g_magnification.load(std::memory_order_acquire);
    out->magnificationAtFull = g_magnificationAtFull.load(std::memory_order_acquire);
    return true;
}

float AdsZoomFovScale() { return g_fovScale.load(std::memory_order_acquire); }
