#include "player_eye.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "diagnostics.h"

namespace {

// client.dll+0x14EF00, already resolved and prologue-verified by ads_probe.cpp
// for the ADS work. Re-resolved here rather than shared, because a second
// consumer of a cached function pointer is a second thing that can be left
// holding a stale one after a module reload.
constexpr std::uintptr_t kGetLocalPlayerRva = 0x14EF00;
constexpr std::uint8_t kGetLocalPlayerPrologue[] = {0x8B, 0x05};
using GetLocalPlayerFn = void*(__fastcall*)();
GetLocalPlayerFn g_getLocalPlayer = nullptr;
std::atomic_bool g_resolved{false};

std::atomic_bool g_writeEnabled{false};
std::atomic<float> g_raiseUnits{0.0f};

// The identified field, as a byte offset into the player entity. Negative until
// the arithmetic identity below has picked exactly one candidate.
std::atomic<int> g_viewOffsetField{-1};
// The origin that CLOSED the arithmetic for that field, recorded rather than
// re-derived. A second consumer that recomputed "the origin is 0x1C below"
// from a comment would be free to be wrong on a build where it is not, and the
// search already knows the answer at the moment it accepts the candidate.
std::atomic<int> g_originField{-1};
std::atomic_bool g_searchDone{false};
std::atomic_uint32_t g_faults{0};

// What we last wrote, so our own value is not mistaken for the game's and
// raised again on the next tick. The engine moves this field on crouch, stand
// and mantle, and every one of those has to come through untouched.
std::atomic<float> g_lastWritten{-99999.0f};
std::atomic<float> g_lastBase{-99999.0f};

// How far into the entity to look. The Source player's networked vectors sit
// well inside this; scanning further costs time and invites false positives.
constexpr int kScanBytes = 0x2000;

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

void Resolve() {
    if (g_resolved.exchange(true, std::memory_order_acq_rel)) return;
    const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleA("client.dll"));
    if (!base) return;
    // The same prologue check ads_probe makes. An RVA that has drifted with a
    // game patch would otherwise be called blind.
    if (!ReadableFrom(base + kGetLocalPlayerRva, sizeof(kGetLocalPlayerPrologue))) return;
    if (std::memcmp(base + kGetLocalPlayerRva, kGetLocalPlayerPrologue,
                    sizeof(kGetLocalPlayerPrologue)) != 0) {
        Tf2VrLog("[TF2VR] player eye: GetLocalPlayer prologue did not verify; the view-offset write "
                 "stays off and the height keeps coming from the render camera.\n");
        return;
    }
    g_getLocalPlayer = reinterpret_cast<GetLocalPlayerFn>(
        const_cast<std::uint8_t*>(base) + kGetLocalPlayerRva);
}

void* LivePlayer() {
    if (!g_getLocalPlayer) return nullptr;
    __try {
        return g_getLocalPlayer();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
}

// THE IDENTIFICATION. Two constraints at once, which is what makes it an
// identification rather than a guess:
//
//   * the candidate reads (~0, ~0, z) with z a human eye height in units
//   * some OTHER vector in the same entity is a world position whose z, plus
//     the candidate's z, lands on the camera height the census measured
//
// A lone (0,0,64) triple would match several unrelated fields. One that also
// closes the arithmetic against an independently measured camera height, to
// within half a unit, does not.
void Search(const std::uint8_t* entity, float cameraZ) {
    int found = -1;
    int foundOrigin = -1;
    int matches = 0;
    char detail[600]{};
    int used = std::snprintf(detail, sizeof(detail), "[TF2VR] player eye SEARCH (camera z=%.2f): ",
                             cameraZ);
    for (int candidate = 0; candidate + 12 <= kScanBytes; candidate += 4) {
        float v[3]{};
        std::memcpy(v, entity + candidate, sizeof(v));
        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) continue;
        if (std::fabs(v[0]) > 0.05f || std::fabs(v[1]) > 0.05f) continue;
        // A HUMAN EYE HEIGHT, not merely a positive number. The first version
        // accepted 20..100 and matched a coincidental (0,0,100.00) -- 2.5 m,
        // which is nobody's eye -- alongside the real (0,0,60.00). Widening a
        // range to be safe is what made the search ambiguous and threw away an
        // identification it had already made.
        if (v[2] < 40.0f || v[2] > 80.0f) continue;
        // Does any world-position vector in here close the arithmetic?
        for (int origin = 0; origin + 12 <= kScanBytes; origin += 4) {
            if (origin == candidate) continue;
            // THE ORIGIN AND THE VIEW OFFSET LIVE IN THE SAME BLOCK. Requiring
            // them within 0x100 of each other is what tells a real struct
            // relationship from arithmetic that happens to close: the true pair
            // measured 0x1C apart, the false one 0x143C -- over five kilobytes,
            // which is not a layout, it is a coincidence.
            if (std::abs(origin - candidate) > 0x100) continue;
            float o[3]{};
            std::memcpy(o, entity + origin, sizeof(o));
            if (!std::isfinite(o[0]) || !std::isfinite(o[2])) continue;
            // A world position, not another near-origin vector.
            if (std::fabs(o[0]) < 1.0f && std::fabs(o[1]) < 1.0f) continue;
            if (std::fabs(o[2] + v[2] - cameraZ) > 0.5f) continue;
            ++matches;
            if (found < 0) { found = candidate; foundOrigin = origin; }
            if (used < static_cast<int>(sizeof(detail)) - 80) {
                used += std::snprintf(detail + used, sizeof(detail) - static_cast<size_t>(used),
                                      "[+0x%X z=%.2f via origin +0x%X] ", candidate, v[2], origin);
            }
            break;  // one witness is enough for this candidate
        }
    }
    std::snprintf(detail + used, sizeof(detail) - static_cast<size_t>(used),
                  "-- %d candidate%s. %s\n", matches, matches == 1 ? "" : "s",
                  matches == 1 ? "Exactly one closes the arithmetic, so that IS the view offset."
                               : "NOT exactly one, so nothing is written and the height stays on "
                                 "the render camera. Ambiguous is not identified.");
    Tf2VrLog(detail);
    // ONLY an unambiguous match is trusted. Writing into a player entity on a
    // guess is how a session ends in a crash with nothing learned.
    if (matches == 1) {
        g_originField.store(foundOrigin, std::memory_order_release);
        g_viewOffsetField.store(found, std::memory_order_release);
    }
}

}  // namespace

void SetPlayerEyeWriteEnabled(bool enabled) { g_writeEnabled.store(enabled, std::memory_order_release); }
void SetPlayerEyeRaiseUnits(float units) {
    if (units > -60.0f && units < 60.0f) g_raiseUnits.store(units, std::memory_order_release);
}

bool PlayerEyeWriteActive() {
    return g_writeEnabled.load(std::memory_order_acquire) &&
           g_viewOffsetField.load(std::memory_order_acquire) >= 0;
}

// C1's D1 rider. THE SAME SEARCH, WITHOUT THE WRITE.
//
// The identification was sound and only the write was unusable, so the census
// needs the field without arming anything: the plan ships height last, and a
// read that required `vr.eye_write` would have made the census itself a
// behavioural change. The search runs once, caches, and is shared with the
// write path -- two searches could disagree, and a field that two parts of one
// build disagree about is worse than no field at all.
bool PlayerEyeReadOnly(float cameraZ, float* originZ, float* viewOffsetZ, int* fieldOffset) {
    Resolve();
    void* player = LivePlayer();
    if (!player) return false;
    const auto* entity = static_cast<const std::uint8_t*>(player);
    if (!ReadableFrom(entity, kScanBytes)) return false;

    int field = g_viewOffsetField.load(std::memory_order_acquire);
    if (field < 0) {
        if (cameraZ < 1.0f || g_searchDone.load(std::memory_order_acquire)) return false;
        g_searchDone.store(true, std::memory_order_release);
        __try {
            Search(entity, cameraZ);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_faults.fetch_add(1, std::memory_order_relaxed);
        }
        field = g_viewOffsetField.load(std::memory_order_acquire);
        if (field < 0) return false;
    }
    const int origin = g_originField.load(std::memory_order_acquire);
    if (origin < 0) return false;
    if (fieldOffset) *fieldOffset = field;
    __try {
        const float offsetZ = *reinterpret_cast<const float*>(entity + field + 8);
        const float baseZ = *reinterpret_cast<const float*>(entity + origin + 8);
        if (!std::isfinite(offsetZ) || !std::isfinite(baseZ)) return false;
        if (viewOffsetZ) *viewOffsetZ = offsetZ;
        if (originZ) *originZ = baseZ;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void PlayerEyeTick(float cameraZ) {
    if (!g_writeEnabled.load(std::memory_order_acquire)) return;
    Resolve();
    void* player = LivePlayer();
    if (!player) return;
    const auto* entity = static_cast<const std::uint8_t*>(player);
    if (!ReadableFrom(entity, kScanBytes)) return;

    int field = g_viewOffsetField.load(std::memory_order_acquire);
    if (field < 0) {
        // The search needs a camera height that is actually a player's eye. On
        // the loading screen the census reads whatever the menu camera is doing,
        // and identifying against that would pick a field at random.
        if (cameraZ < 1.0f || g_searchDone.load(std::memory_order_acquire)) return;
        g_searchDone.store(true, std::memory_order_release);
        __try {
            Search(entity, cameraZ);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_faults.fetch_add(1, std::memory_order_relaxed);
        }
        field = g_viewOffsetField.load(std::memory_order_acquire);
        if (field < 0) return;
    }

    const float raise = g_raiseUnits.load(std::memory_order_acquire);
    __try {
        auto* z = reinterpret_cast<float*>(const_cast<std::uint8_t*>(entity) + field + 8);
        const float current = *z;
        if (!std::isfinite(current)) return;
        // OUR OWN VALUE MUST NOT BE RAISED AGAIN.
        //
        // The engine moves this field on crouch, stand and mantle, and every one
        // of those has to pass through. So: if what is there is what we last
        // wrote, the game has not touched it and there is nothing to do.
        // Anything else is a fresh value from the engine and becomes the new
        // base. Without this the eye climbs by `raise` every tick and the wearer
        // is in orbit within a second.
        if (std::fabs(current - g_lastWritten.load(std::memory_order_acquire)) < 0.0005f) return;
        g_lastBase.store(current, std::memory_order_release);
        const float wanted = current + raise;
        if (wanted < 0.0f || wanted > 200.0f) return;   // never leave it somewhere absurd
        *z = wanted;
        g_lastWritten.store(wanted, std::memory_order_release);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        // A faulting write is not retried: the field was wrong, or the entity
        // went away underneath us, and either way continuing to poke it is how a
        // diagnostic becomes a crash.
        g_viewOffsetField.store(-1, std::memory_order_release);
        g_writeEnabled.store(false, std::memory_order_release);
        Tf2VrLog("[TF2VR] player eye: the view-offset write FAULTED and has been disarmed. The "
                 "height falls back to the render camera.\n");
    }
}

const void* PlayerEyeLocalPlayerForReading() {
    Resolve();
    return LivePlayer();
}

// THE PLAYER'S COLLISION HULL, WHICH IS THE ONE SIZE THAT CANNOT BE STALE.
//
// The attachment route failed: in first person the player's world model is not
// animated, so its attachment table reads NaN or zero, and the one figure it
// ever produced (HEADFOCUS 56.54) was a leftover that happened to pass a
// validity band. An argument was built on it. It should not have been.
//
// The hull cannot go stale the same way, because the engine collides against it
// every tick. Source stores it as two Vectors, mins and maxs, with maxs.z the
// standing height -- 72 on a stock player -- and x/y a symmetric half-width
// around 16. That shape is distinctive enough to find by identity rather than
// by offset: x and y equal and positive, z a plausible body height.
//
// AND IT COMES WITH ITS OWN POSITIVE CONTROL. Crouching moves the view offset
// 60 -> 38, measured. The hull must move with it. A candidate whose z does NOT
// change across a crouch is not the hull however well it matched, and one that
// does is confirmed by the game's own behaviour rather than by my arithmetic.
// So every candidate is reported with its live z on every call, and the run
// picks the winner rather than the search doing it blind.
void LogPlayerHullCandidates(float cameraZ) {
    Resolve();
    void* player = LivePlayer();
    if (!player) return;
    const auto* entity = static_cast<const std::uint8_t*>(player);
    if (!ReadableFrom(entity, kScanBytes)) return;

    char detail[620]{};
    int used = std::snprintf(detail, sizeof(detail),
        "[TF2VR] PLAYER HULL candidates (camera z=%.2f, view offset field +0x%X): ", cameraZ,
        g_viewOffsetField.load(std::memory_order_acquire) < 0
            ? 0u
            : static_cast<unsigned>(g_viewOffsetField.load(std::memory_order_acquire)));
    int found = 0;
    __try {
        for (int candidate = 0; candidate + 12 <= kScanBytes; candidate += 4) {
            float v[3]{};
            std::memcpy(v, entity + candidate, sizeof(v));
            if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) continue;
            // A symmetric half-width and a body height. Stock Source is
            // (16, 16, 72); anything of that shape is worth reporting, and the
            // crouch decides between them.
            if (v[0] < 8.0f || v[0] > 40.0f) continue;
            if (std::fabs(v[1] - v[0]) > 0.01f) continue;
            if (v[2] < 30.0f || v[2] > 100.0f) continue;
            ++found;
            if (used < static_cast<int>(sizeof(detail)) - 60) {
                used += std::snprintf(detail + used, sizeof(detail) - static_cast<std::size_t>(used),
                                      "[+0x%X (%.1f,%.1f,%.2f)] ", candidate, v[0], v[1], v[2]);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    std::snprintf(detail + used, sizeof(detail) - static_cast<std::size_t>(used),
                  "-- %d candidate%s. The one whose z CHANGES when you crouch is the hull; a "
                  "72-ish standing z against a 60.00 view offset puts the eye at 83%% of stature "
                  "where a human sits at 93%%.\n", found, found == 1 ? "" : "s");
    Tf2VrLog(detail);
}

namespace {
// TWO TABLES, BECAUSE THE CLIENT PREDICTS ITS OWN VIEW OFFSET.
//
// Writing only the server's was measured doing nothing: the record held 67.20
// across a whole crouch while the entity lerped 60.00 -> 59.94 -> 59.75 and back,
// never once following it. The client keeps its OWN copy of the player settings
// so it can predict, and the census reads the client entity -- so the client copy
// is the one that had to change all along.
//
// Found by matching the server lookup's own instruction shape (movsxd, movsxd,
// imul 0x110) in client.dll, with the server hit as the positive control that
// the pattern finds the right thing. The strides differ because the client and
// server structs differ, which is exactly why the address could not be assumed.
struct SettingsTable {
    const char* module;
    std::uintptr_t tableRva;
    std::size_t classStride;
};
constexpr SettingsTable kSettingsTables[] = {
    {"server.dll", 0x1351978, 0x68D0},
    {"client.dll", 0x00C42870, 0x68E8},
};
constexpr std::uintptr_t kSettingsTableRva = 0x1351978;
constexpr std::size_t kSettingsClassStride = 0x68D0;
constexpr std::size_t kSettingsStanceStride = 0x110;
constexpr std::size_t kSettingsViewHeightOffset = 0x08;
constexpr std::size_t kSettingsIndexOffset = 0x1B98;
std::atomic<float> g_eyeLiftRatio{0.0f};
std::atomic_bool g_eyeLiftReported{false};
// What the slot held BEFORE we touched it, so our own work is recognisable.
// Without this the idempotency check reads back our value, fails the entity
// cross-check and reports "that is not the view-height record" about the record
// it had just correctly written -- which is what the first run did.
std::atomic<float> g_eyeLiftOriginal{0.0f};

float* ViewHeightSlotIn(const SettingsTable& table, int settingsIndex, int stance) {
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleA(table.module));
    if (!base) return nullptr;
    return reinterpret_cast<float*>(
        base + table.tableRva + static_cast<std::size_t>(settingsIndex) * table.classStride +
        static_cast<std::size_t>(stance) * kSettingsStanceStride + kSettingsViewHeightOffset);
}

float* ViewHeightSlot(const std::uint8_t* serverBase, int settingsIndex, int stance) {
    return reinterpret_cast<float*>(
        const_cast<std::uint8_t*>(serverBase) + kSettingsTableRva +
        static_cast<std::size_t>(settingsIndex) * kSettingsClassStride +
        static_cast<std::size_t>(stance) * kSettingsStanceStride + kSettingsViewHeightOffset);
}
}  // namespace

void SetEyeLiftRatio(float ratio) {
    // 1.0 is a no-op and anything past 1.4 would put the eye above the model's
    // head. Refused rather than clamped: a typo should not half-work.
    if (ratio != 0.0f && !(ratio > 0.8f && ratio < 1.4f)) {
        Tf2VrLog("[TF2VR] vr.eye_lift out of range (want 0, or 0.8..1.4); ignored.\n");
        return;
    }
    g_eyeLiftRatio.store(ratio, std::memory_order_release);
}

void ApplyEyeLift(float measuredStandingViewHeight) {
    const float ratio = g_eyeLiftRatio.load(std::memory_order_acquire);
    if (ratio <= 0.0f) return;
    Resolve();
    void* player = LivePlayer();
    const auto* entity = static_cast<const std::uint8_t*>(player);

    __try {
        int index = 0;
        if (entity && ReadableFrom(entity, kSettingsIndexOffset + 4)) {
            index = *reinterpret_cast<const int*>(entity + kSettingsIndexOffset);
        }
        if (index < 0 || index > 512) return;
        // BOTH TABLES, EACH GATED SEPARATELY. The server's is what a server-side
        // consumer reads; the client's is what the client predicts from, and the
        // client's is the one the entity actually followed. Neither is assumed
        // to be present or correct: each is recognised on its own terms and a
        // table that does not hold a view height is skipped, not written.
        for (const SettingsTable& table : kSettingsTables) {
            float* stand = ViewHeightSlotIn(table, index, 0);
            float* crouch = ViewHeightSlotIn(table, index, 1);
            if (!stand || !crouch) continue;
            if (!ReadableFrom(stand, 4) || !ReadableFrom(crouch, 4)) continue;
            const float standNow = *stand;
            const float crouchNow = *crouch;
            // Our own value is not a stranger: if the slot already holds what we
            // wrote, this is a later call on a record we have corrected.
            const bool alreadyOurs =
                std::isfinite(standNow) && standNow > 60.5f && standNow < 100.0f;
            if (alreadyOurs) continue;
            // THE POSITIVE CONTROL, per table. A plausible view height, crouched
            // below standing, and -- when a live player exists to ask -- the
            // standing one matching what the census reads at +0xAC by a
            // completely separate route.
            const bool recognised =
                std::isfinite(standNow) && std::isfinite(crouchNow) && standNow > 20.0f &&
                standNow < 100.0f && crouchNow > 10.0f && crouchNow < standNow &&
                (measuredStandingViewHeight <= 0.0f ||
                 std::fabs(standNow - measuredStandingViewHeight) < 0.5f);
            if (!recognised) continue;
            *stand = standNow * ratio;
            *crouch = crouchNow * ratio;
            char line[420]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] vr.eye_lift x%.3f APPLIED in %s (index %d): standing %.2f -> %.2f, "
                "crouching %.2f -> %.2f. The falsifier is the entity's own view offset.\n",
                static_cast<double>(ratio), table.module, index, static_cast<double>(standNow),
                static_cast<double>(standNow * ratio), static_cast<double>(crouchNow),
                static_cast<double>(crouchNow * ratio));
            Tf2VrLog(line);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        g_eyeLiftRatio.store(0.0f, std::memory_order_release);
        Tf2VrLog("[TF2VR] vr.eye_lift FAULTED and is disarmed for this session.\n");
    }
}

void LogViewHeightRecordVsEntity(float entityViewOffsetZ, float cameraZ) {
    static std::uint64_t nextMs = 0;
    static std::uint32_t lines = 0;
    static float lastStand = -1.0f;
    static float lastEntity = -1.0f;
    const auto* serverBase = reinterpret_cast<const std::uint8_t*>(GetModuleHandleA("server.dll"));
    if (!serverBase) return;
    Resolve();
    void* player = LivePlayer();
    const auto* entity = static_cast<const std::uint8_t*>(player);

    __try {
        int index = 0;
        if (entity && ReadableFrom(entity, kSettingsIndexOffset + 4)) {
            index = *reinterpret_cast<const int*>(entity + kSettingsIndexOffset);
        }
        if (index < 0 || index > 512) return;
        const float* stand = ViewHeightSlot(serverBase, index, 0);
        const float* crouch = ViewHeightSlot(serverBase, index, 1);
        if (!ReadableFrom(stand, 4) || !ReadableFrom(crouch, 4)) return;
        const float standNow = *stand;
        const float crouchNow = *crouch;
        // ON CHANGE, plus a slow heartbeat. The interesting moment is a stance
        // transition and it lasts a fraction of a second, so a fixed 2 Hz clock
        // would sample either side of it and miss the step. Anything that MOVES
        // is printed immediately; a still scene costs one line every 2 s.
        const std::uint64_t now = GetTickCount64();
        const bool moved = std::fabs(standNow - lastStand) > 0.01f ||
                           std::fabs(entityViewOffsetZ - lastEntity) > 0.01f;
        if (!moved && now < nextMs) return;
        if (lines >= 200) return;
        ++lines;
        nextMs = now + 2000;
        lastStand = standNow;
        lastEntity = entityViewOffsetZ;
        char line[460]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] VIEWHEIGHT#%u index %d | RECORD stand %.2f crouch %.2f | ENTITY view offset "
            "%.2f | camera z %.2f (%s). Record raised and entity not following means the field is "
            "not the source; record back at its original means our write was reverted.\n",
            lines, index, static_cast<double>(standNow), static_cast<double>(crouchNow),
            static_cast<double>(entityViewOffsetZ), static_cast<double>(cameraZ),
            cameraZ < 85.0f ? "CROUCHED" : "standing");
        Tf2VrLog(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lines = 200;
    }
}

void LogViewHeightEndpointCandidates(float entityViewOffsetZ, float cameraZ) {
    if (cameraZ > 85.0f) return;
    static std::uint32_t lines = 0;
    if (lines >= 6) return;
    Resolve();
    void* player = LivePlayer();
    if (!player) return;
    const auto* entity = static_cast<const std::uint8_t*>(player);
    if (!ReadableFrom(entity, kScanBytes)) return;
    ++lines;

    // A PAIR, ADJACENT, NOT TWO LOOSE VALUE MATCHES.
    //
    // The first version asked for any float equal to 60.00 and any equal to
    // 38.00 anywhere in the entity, and got 221 and 220 hits -- 60 and 38 are
    // ordinary numbers and the entity is full of them. That is the same loose
    // predicate that once turned the view-offset identification into two
    // candidates and an "ambiguous, write nothing".
    //
    // Cached stance endpoints are a PAIR, so they are stored together. Requiring
    // 60.00 and 38.00 in ADJACENT slots, in either order, is a far narrower
    // claim than either value alone -- and the distance from the known view
    // offset at +0xAC is printed beside each hit, because a pair inside the same
    // struct block is a layout and one a kilobyte away is a coincidence.
    char detail[620]{};
    int used = std::snprintf(detail, sizeof(detail),
        "[TF2VR] ENDPOINT PAIRS (live offset %.2f, camera %.2f): ",
        static_cast<double>(entityViewOffsetZ), static_cast<double>(cameraZ));
    int found = 0;
    __try {
        for (int at = 0; at + 8 <= kScanBytes; at += 4) {
            float a = 0.0f, b = 0.0f;
            std::memcpy(&a, entity + at, sizeof(a));
            std::memcpy(&b, entity + at + 4, sizeof(b));
            const bool pair = (std::fabs(a - 60.0f) < 0.01f && std::fabs(b - 38.0f) < 0.01f) ||
                              (std::fabs(a - 38.0f) < 0.01f && std::fabs(b - 60.0f) < 0.01f);
            if (!pair) continue;
            ++found;
            if (used < static_cast<int>(sizeof(detail)) - 70) {
                used += std::snprintf(detail + used, sizeof(detail) - static_cast<std::size_t>(used),
                                      "[+0x%X %.0f,%.0f d=%+d] ", at, static_cast<double>(a),
                                      static_cast<double>(b), at - 0xAC);
            }
        }
        std::snprintf(detail + used, sizeof(detail) - static_cast<std::size_t>(used),
                      "-- %d adjacent pair%s. d is the distance from the view offset at +0xAC: a "
                      "pair in the same struct block is a layout, one a kilobyte away is a "
                      "coincidence.\n", found, found == 1 ? "" : "s");
        Tf2VrLog(detail);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lines = 6;
    }
}
