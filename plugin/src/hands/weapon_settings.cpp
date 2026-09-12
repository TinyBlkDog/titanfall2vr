#include "weapon_settings.h"

#include "diagnostics.h"
#include "hook_registry.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" volatile std::uintptr_t g_weaponSettingsTablePtr = 0;
extern "C" volatile std::uint64_t g_weaponSettingsIndex = 0;
extern "C" std::uintptr_t g_weaponSettingsContinue = 0;
// Total detour invocations. If this climbs per FRAME rather than per level
// load, the site is hot and this detour is in the render path -- which is the
// user report that the sway fix introduced the stall.
extern "C" volatile std::uint64_t g_weaponSettingsHits = 0;
extern "C" void weaponSettingsInterceptor();

namespace {
constexpr size_t kTableSize = 128;
// Written by the detour, read here. Entries repeat and wrap; the reader
// de-duplicates rather than assuming uniqueness.
volatile std::uintptr_t g_table[kTableSize]{};

// movaps xmm2, xmm9 ; mov rcx, rbx ; movss [rdi+0x84], xmm0
// Verified unique across .text in the shipped client.dll.
constexpr std::uint8_t kSignature[] = {
    0x41, 0x0F, 0x28, 0xD1,
    0x48, 0x8B, 0xCB,
    0xF3, 0x0F, 0x11, 0x87, 0x84, 0x00, 0x00, 0x00,
};
constexpr size_t kDisplacedBytes = sizeof(kSignature);
static_assert(kDisplacedBytes >= 14, "an absolute jump needs 14 bytes");

constexpr std::uintptr_t kSwayTranslateGainOffset = 0x84;
constexpr std::uintptr_t kSwayRotateGainOffset = 0x88;

// THE WHOLE SWAY/BOB BLOCK, paired name-to-offset on 2026-09-04 by walking the
// unrolled parse at client.dll+0x592810 (each `movss [rdi+N]` stores the field
// parsed by the PREVIOUS call; the pairing reproduces SWAY-ROUTE-2026-08-18 §1
// exactly for +0x54..+0x94). Two contiguous runs: hip +0x54..+0x19C and zoomed
// +0x1F4..+0x33C. Every entry is an AMPLITUDE or a CLAMP, zeroed together.
// Deliberately absent -- periods and speeds a zero could divide by:
//   +0x8C bob_cycle_time, +0x90 bob_min_speed, +0x94 bob_max_speed, and
//   +0x22C/+0x230/+0x234 zoomed; +0x50/+0x1F0 sway_rotate_attach_blend_time.
// The two gains that the 2026-08-16 suppression zeroed are in here too.
struct SwayField { std::uint32_t offset; const char* name; };
constexpr SwayField kSwayFields[] = {
    {0x054, "sway_min_x"},
    {0x058, "sway_min_y"},
    {0x05C, "sway_min_z"},
    {0x060, "sway_max_x"},
    {0x064, "sway_max_y"},
    {0x068, "sway_max_z"},
    {0x06C, "sway_min_pitch"},
    {0x070, "sway_min_yaw"},
    {0x074, "sway_min_roll"},
    {0x078, "sway_max_pitch"},
    {0x07C, "sway_max_yaw"},
    {0x080, "sway_max_roll"},
    {0x084, "sway_translate_gain"},
    {0x088, "sway_rotate_gain"},
    {0x098, "bob_vert_dist"},
    {0x09C, "bob_horz_dist"},
    {0x0A0, "bob_pitch"},
    {0x0A4, "bob_yaw"},
    {0x0A8, "bob_roll"},
    {0x0AC, "bob_gain"},
    {0x0B0, "sway_move_forward_translate_x"},
    {0x0B4, "sway_move_forward_translate_y"},
    {0x0B8, "sway_move_forward_translate_z"},
    {0x0BC, "sway_move_forward_rotate_pitch"},
    {0x0C0, "sway_move_forward_rotate_yaw"},
    {0x0C4, "sway_move_forward_rotate_roll"},
    {0x0C8, "sway_move_back_translate_x"},
    {0x0CC, "sway_move_back_translate_y"},
    {0x0D0, "sway_move_back_translate_z"},
    {0x0D4, "sway_move_back_rotate_pitch"},
    {0x0D8, "sway_move_back_rotate_yaw"},
    {0x0DC, "sway_move_back_rotate_roll"},
    {0x0E0, "sway_move_left_translate_x"},
    {0x0E4, "sway_move_left_translate_y"},
    {0x0E8, "sway_move_left_translate_z"},
    {0x0EC, "sway_move_left_rotate_pitch"},
    {0x0F0, "sway_move_left_rotate_yaw"},
    {0x0F4, "sway_move_left_rotate_roll"},
    {0x0F8, "sway_move_right_translate_x"},
    {0x0FC, "sway_move_right_translate_y"},
    {0x100, "sway_move_right_translate_z"},
    {0x104, "sway_move_right_rotate_pitch"},
    {0x108, "sway_move_right_rotate_yaw"},
    {0x10C, "sway_move_right_rotate_roll"},
    {0x110, "sway_move_up_translate_x"},
    {0x114, "sway_move_up_translate_y"},
    {0x118, "sway_move_up_translate_z"},
    {0x11C, "sway_move_up_rotate_pitch"},
    {0x120, "sway_move_up_rotate_yaw"},
    {0x124, "sway_move_up_rotate_roll"},
    {0x128, "sway_move_down_translate_x"},
    {0x12C, "sway_move_down_translate_y"},
    {0x130, "sway_move_down_translate_z"},
    {0x134, "sway_move_down_rotate_pitch"},
    {0x138, "sway_move_down_rotate_yaw"},
    {0x13C, "sway_move_down_rotate_roll"},
    {0x140, "sway_turn_left_translate_x"},
    {0x144, "sway_turn_left_translate_y"},
    {0x148, "sway_turn_left_translate_z"},
    {0x14C, "sway_turn_left_rotate_pitch"},
    {0x150, "sway_turn_left_rotate_yaw"},
    {0x154, "sway_turn_left_rotate_roll"},
    {0x158, "sway_turn_right_translate_x"},
    {0x15C, "sway_turn_right_translate_y"},
    {0x160, "sway_turn_right_translate_z"},
    {0x164, "sway_turn_right_rotate_pitch"},
    {0x168, "sway_turn_right_rotate_yaw"},
    {0x16C, "sway_turn_right_rotate_roll"},
    {0x170, "sway_turn_up_translate_x"},
    {0x174, "sway_turn_up_translate_y"},
    {0x178, "sway_turn_up_translate_z"},
    {0x17C, "sway_turn_up_rotate_pitch"},
    {0x180, "sway_turn_up_rotate_yaw"},
    {0x184, "sway_turn_up_rotate_roll"},
    {0x188, "sway_turn_down_translate_x"},
    {0x18C, "sway_turn_down_translate_y"},
    {0x190, "sway_turn_down_translate_z"},
    {0x194, "sway_turn_down_rotate_pitch"},
    {0x198, "sway_turn_down_rotate_yaw"},
    {0x19C, "sway_turn_down_rotate_roll"},
    {0x1F4, "sway_min_x_zoomed"},
    {0x1F8, "sway_min_y_zoomed"},
    {0x1FC, "sway_min_z_zoomed"},
    {0x200, "sway_max_x_zoomed"},
    {0x204, "sway_max_y_zoomed"},
    {0x208, "sway_max_z_zoomed"},
    {0x20C, "sway_min_pitch_zoomed"},
    {0x210, "sway_min_yaw_zoomed"},
    {0x214, "sway_min_roll_zoomed"},
    {0x218, "sway_max_pitch_zoomed"},
    {0x21C, "sway_max_yaw_zoomed"},
    {0x220, "sway_max_roll_zoomed"},
    {0x224, "sway_translate_gain_zoomed"},
    {0x228, "sway_rotate_gain_zoomed"},
    {0x238, "bob_vert_dist_zoomed"},
    {0x23C, "bob_horz_dist_zoomed"},
    {0x240, "bob_pitch_zoomed"},
    {0x244, "bob_yaw_zoomed"},
    {0x248, "bob_roll_zoomed"},
    {0x24C, "bob_gain_zoomed"},
    {0x250, "sway_move_forward_translate_x_zoomed"},
    {0x254, "sway_move_forward_translate_y_zoomed"},
    {0x258, "sway_move_forward_translate_z_zoomed"},
    {0x25C, "sway_move_forward_rotate_pitch_zoomed"},
    {0x260, "sway_move_forward_rotate_yaw_zoomed"},
    {0x264, "sway_move_forward_rotate_roll_zoomed"},
    {0x268, "sway_move_back_translate_x_zoomed"},
    {0x26C, "sway_move_back_translate_y_zoomed"},
    {0x270, "sway_move_back_translate_z_zoomed"},
    {0x274, "sway_move_back_rotate_pitch_zoomed"},
    {0x278, "sway_move_back_rotate_yaw_zoomed"},
    {0x27C, "sway_move_back_rotate_roll_zoomed"},
    {0x280, "sway_move_left_translate_x_zoomed"},
    {0x284, "sway_move_left_translate_y_zoomed"},
    {0x288, "sway_move_left_translate_z_zoomed"},
    {0x28C, "sway_move_left_rotate_pitch_zoomed"},
    {0x290, "sway_move_left_rotate_yaw_zoomed"},
    {0x294, "sway_move_left_rotate_roll_zoomed"},
    {0x298, "sway_move_right_translate_x_zoomed"},
    {0x29C, "sway_move_right_translate_y_zoomed"},
    {0x2A0, "sway_move_right_translate_z_zoomed"},
    {0x2A4, "sway_move_right_rotate_pitch_zoomed"},
    {0x2A8, "sway_move_right_rotate_yaw_zoomed"},
    {0x2AC, "sway_move_right_rotate_roll_zoomed"},
    {0x2B0, "sway_move_up_translate_x_zoomed"},
    {0x2B4, "sway_move_up_translate_y_zoomed"},
    {0x2B8, "sway_move_up_translate_z_zoomed"},
    {0x2BC, "sway_move_up_rotate_pitch_zoomed"},
    {0x2C0, "sway_move_up_rotate_yaw_zoomed"},
    {0x2C4, "sway_move_up_rotate_roll_zoomed"},
    {0x2C8, "sway_move_down_translate_x_zoomed"},
    {0x2CC, "sway_move_down_translate_y_zoomed"},
    {0x2D0, "sway_move_down_translate_z_zoomed"},
    {0x2D4, "sway_move_down_rotate_pitch_zoomed"},
    {0x2D8, "sway_move_down_rotate_yaw_zoomed"},
    {0x2DC, "sway_move_down_rotate_roll_zoomed"},
    {0x2E0, "sway_turn_left_translate_x_zoomed"},
    {0x2E4, "sway_turn_left_translate_y_zoomed"},
    {0x2E8, "sway_turn_left_translate_z_zoomed"},
    {0x2EC, "sway_turn_left_rotate_pitch_zoomed"},
    {0x2F0, "sway_turn_left_rotate_yaw_zoomed"},
    {0x2F4, "sway_turn_left_rotate_roll_zoomed"},
    {0x2F8, "sway_turn_right_translate_x_zoomed"},
    {0x2FC, "sway_turn_right_translate_y_zoomed"},
    {0x300, "sway_turn_right_translate_z_zoomed"},
    {0x304, "sway_turn_right_rotate_pitch_zoomed"},
    {0x308, "sway_turn_right_rotate_yaw_zoomed"},
    {0x30C, "sway_turn_right_rotate_roll_zoomed"},
    {0x310, "sway_turn_up_translate_x_zoomed"},
    {0x314, "sway_turn_up_translate_y_zoomed"},
    {0x318, "sway_turn_up_translate_z_zoomed"},
    {0x31C, "sway_turn_up_rotate_pitch_zoomed"},
    {0x320, "sway_turn_up_rotate_yaw_zoomed"},
    {0x324, "sway_turn_up_rotate_roll_zoomed"},
    {0x328, "sway_turn_down_translate_x_zoomed"},
    {0x32C, "sway_turn_down_translate_y_zoomed"},
    {0x330, "sway_turn_down_translate_z_zoomed"},
    {0x334, "sway_turn_down_rotate_pitch_zoomed"},
    {0x338, "sway_turn_down_rotate_yaw_zoomed"},
    {0x33C, "sway_turn_down_rotate_roll_zoomed"},
};
constexpr size_t kSwayFieldCount = sizeof(kSwayFields) / sizeof(kSwayFields[0]);
constexpr std::uintptr_t kSwayBlockBegin = 0x54;
constexpr std::uintptr_t kSwayBlockEnd = 0x340;  // one past the last zoomed field

std::uint8_t* g_patchSite = nullptr;
std::uint8_t g_original[kDisplacedBytes]{};
// ALWAYS ON since 2026-09-04: the hand-driven gun and the ADS design assume the
// gun is exactly where the hand or the view says; the engine's own sway is
// incompatible with both, so there is no user switch. Restored only at unload.
std::atomic_bool g_suppressed = true;
// Lets the whole hook be left uninstalled, so its cost can be measured by its
// absence rather than argued about.
std::atomic_bool g_hookAllowed = true;

constexpr std::uintptr_t kSwayHipEnd = 0x1A0;      // one past sway_turn_down_rotate_roll
constexpr std::uintptr_t kSwayZoomBegin = 0x1F4;

struct SavedBlock {
    std::uintptr_t object;
    float values[kSwayFieldCount];
    bool valid;
    // Which of the two runs was writable when the block was saved. An object
    // whose allocation ends inside the zoomed run (seen once in 68 on
    // 2026-09-04) keeps its hip run suppressed rather than being skipped whole.
    bool hip;
    bool zoom;
    bool checked;   // writability probed once
    bool hipOk;
    bool zoomOk;
};

bool FieldInRun(std::uint32_t offset, bool hip, bool zoom) {
    if (offset < kSwayHipEnd) return hip;
    return zoom;
}
SavedBlock g_saved[kTableSize]{};
size_t g_savedCount = 0;


bool IsReadableWritable(const void* address, size_t bytes) {
    MEMORY_BASIC_INFORMATION info{};
    if (!address || !VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    const DWORD protection = info.Protect & 0xFF;
    if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
        protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) {
        return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return begin + bytes <= end;
}

std::uint8_t* FindSignature(HMODULE module) {
    auto* base = reinterpret_cast<std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const size_t imageSize = nt->OptionalHeader.SizeOfImage;
    std::uint8_t* found = nullptr;
    for (size_t i = 0; i + sizeof(kSignature) <= imageSize; ++i) {
        if (std::memcmp(base + i, kSignature, sizeof(kSignature)) != 0) continue;
        // Ambiguous is unsafe; never patch.
        if (found) return nullptr;
        found = base + i;
    }
    return found;
}

SavedBlock* FindOrAddSaved(std::uintptr_t object) {
    for (size_t i = 0; i < g_savedCount; ++i) {
        if (g_saved[i].object == object) return &g_saved[i];
    }
    if (g_savedCount >= kTableSize) return nullptr;
    SavedBlock& entry = g_saved[g_savedCount++];
    entry.object = object;
    entry.valid = false;
    entry.hip = entry.zoom = entry.checked = entry.hipOk = entry.zoomOk = false;
    return &entry;
}
}  // namespace

void EnsureWeaponSettingsHookInstalled() {
    if (g_patchSite) return;
    if (!g_hookAllowed.load(std::memory_order_acquire)) return;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return;
    // Same latch, same reason as EnsureCameraHookInstalled: called once a frame,
    // and a signature that stops matching would otherwise rescan client.dll and
    // log on every single frame.
    static bool signatureFailed = false;
    if (signatureFailed) return;
    std::uint8_t* site = FindSignature(client);
    if (!site) {
        signatureFailed = true;
        Tf2VrLog("[TF2VR] weapon-settings signature had zero or multiple matches; sway capture not "
                 "installed. NOT retried.\n");
        return;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(site, kDisplacedBytes, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Tf2VrLog("[TF2VR] weapon-settings VirtualProtect failed.\n");
        return;
    }
    std::memcpy(g_original, site, kDisplacedBytes);
    g_weaponSettingsTablePtr = reinterpret_cast<std::uintptr_t>(&g_table[0]);
    g_weaponSettingsIndex = 0;
    g_weaponSettingsContinue = reinterpret_cast<std::uintptr_t>(site + kDisplacedBytes);
    std::uint8_t detour[kDisplacedBytes];
    std::memset(detour, 0x90, sizeof(detour));
    detour[0] = 0xFF; detour[1] = 0x25;
    std::memset(detour + 2, 0, 4);
    const auto target = reinterpret_cast<std::uintptr_t>(&weaponSettingsInterceptor);
    std::memcpy(detour + 6, &target, sizeof(target));
    std::memcpy(site, detour, sizeof(detour));
    FlushInstructionCache(GetCurrentProcess(), site, kDisplacedBytes);
    DWORD ignored = 0;
    VirtualProtect(site, kDisplacedBytes, oldProtect, &ignored);
    g_patchSite = site;
    RegisterHookSite("weapon-settings parse patched bytes", site, kDisplacedBytes);
    Tf2VrLog("[TF2VR] weapon-settings capture installed (passive; records the settings object only).\n");
}

void RemoveWeaponSettingsHook() {
    // Put the gains back before the detour goes away, so unloading cannot leave
    // every weapon permanently swayless.
    if (g_suppressed.load(std::memory_order_acquire)) SetWeaponSwaySuppressed(false);
    if (!g_patchSite) return;
    DWORD oldProtect = 0;
    if (VirtualProtect(g_patchSite, kDisplacedBytes, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::memcpy(g_patchSite, g_original, kDisplacedBytes);
        FlushInstructionCache(GetCurrentProcess(), g_patchSite, kDisplacedBytes);
        DWORD ignored = 0;
        VirtualProtect(g_patchSite, kDisplacedBytes, oldProtect, &ignored);
    }
    g_patchSite = nullptr;
    g_weaponSettingsTablePtr = 0;
    g_weaponSettingsContinue = 0;
}

// Applies the current suppression state to every object captured so far and
// reports how many were newly zeroed. COST-BOUNDED (2026-09-04): the
// writability of each object's two runs is probed ONCE (two VirtualQuery
// syscalls per object, not per frame), a new object is saved and zeroed the
// frame it appears, and an already-zeroed object is rewritten only when
// `force` is set -- the tick forces on a parse-detour hit (a re-parse can
// rewrite the fields) and once a second as a safety net. Per frame in steady
// state this loop is a pointer scan and nothing else.
unsigned ApplyWeaponSwayState(unsigned* touchedOut, unsigned* skippedOut, bool force) {
    const bool suppressed = g_suppressed.load(std::memory_order_acquire);
    unsigned touched = 0, skipped = 0, newlyZeroed = 0;
    for (size_t i = 0; i < kTableSize; ++i) {
        const auto object = g_table[i];
        if (!object) continue;
        auto* block = reinterpret_cast<std::uint8_t*>(object);
        SavedBlock* saved = FindOrAddSaved(object);
        if (!saved) { ++skipped; continue; }
        if (!saved->checked) {
            saved->checked = true;
            saved->hipOk = IsReadableWritable(block + kSwayBlockBegin, kSwayHipEnd - kSwayBlockBegin);
            saved->zoomOk = IsReadableWritable(block + kSwayZoomBegin, kSwayBlockEnd - kSwayZoomBegin);
            if (!saved->hipOk || !saved->zoomOk) {
                char note[220]{};
                std::snprintf(note, sizeof(note),
                    "[TF2VR] weapon sway: settings object %p is only partly writable (hip run %s, zoomed run %s); "
                    "the writable run is suppressed, the other is left as parsed.\n",
                    reinterpret_cast<const void*>(object), saved->hipOk ? "ok" : "NOT writable",
                    saved->zoomOk ? "ok" : "NOT writable");
                Tf2VrLog(note);
            }
        }
        const bool hipOk = saved->hipOk, zoomOk = saved->zoomOk;
        if (!hipOk && !zoomOk) { ++skipped; continue; }
        if (suppressed) {
            if (!saved->valid) {
                saved->hip = hipOk;
                saved->zoom = zoomOk;
                for (size_t f = 0; f < kSwayFieldCount; ++f) {
                    if (!FieldInRun(kSwayFields[f].offset, hipOk, zoomOk)) { saved->values[f] = 0.0f; continue; }
                    std::memcpy(&saved->values[f], block + kSwayFields[f].offset, sizeof(float));
                }
                saved->valid = true;
                ++newlyZeroed;
            } else if (!force) {
                ++touched;
                continue;
            }
            const float zero = 0.0f;
            for (size_t f = 0; f < kSwayFieldCount; ++f) {
                if (!FieldInRun(kSwayFields[f].offset, saved->hip, saved->zoom)) continue;
                {
                    std::memcpy(block + kSwayFields[f].offset, &zero, sizeof(float));
                }
            }
        } else if (saved->valid) {
            for (size_t f = 0; f < kSwayFieldCount; ++f) {
                if (!FieldInRun(kSwayFields[f].offset, saved->hip, saved->zoom)) continue;
                std::memcpy(block + kSwayFields[f].offset, &saved->values[f], sizeof(float));
            }
            saved->valid = false;
        }
        ++touched;
    }
    if (touchedOut) *touchedOut = touched;
    if (skippedOut) *skippedOut = skipped;
    return newlyZeroed;
}

void TickWeaponSwaySuppression() {
    if (!g_suppressed.load(std::memory_order_acquire)) return;
    static unsigned long long lastHits = 0;
    static std::uint64_t nextForcedMs = 0;
    const unsigned long long hits = g_weaponSettingsHits;
    const std::uint64_t now = GetTickCount64();
    const bool force = hits != lastHits || now >= nextForcedMs;
    if (force) {
        lastHits = hits;
        nextForcedMs = now + 1000;
    }
    unsigned touched = 0, skipped = 0;
    if (!ApplyWeaponSwayState(&touched, &skipped, force)) return;
    char line[224]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] weapon sway suppressed on newly parsed weapons; %u object%s now covered (%u skipped). "
        "This is the map-change case picking itself up.\n",
        touched, touched == 1 ? "" : "s", skipped);
    Tf2VrLog(line);
}

void SetWeaponSwaySuppressed(bool suppressed) {
    g_suppressed.store(suppressed, std::memory_order_release);
    unsigned touched = 0, skipped = 0;
    ApplyWeaponSwayState(&touched, &skipped, true);
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] weapon sway %s on %u weapon setting object%s (%u skipped), all %zu sway/bob block "
        "fields per object (clamps, gains, bob amplitudes, move and turn amounts, hip and zoomed). "
        "Objects parsed later are covered as they appear.\n",
        suppressed ? "SUPPRESSED" : "restored", touched, touched == 1 ? "" : "s", skipped,
        kSwayFieldCount);
    Tf2VrLog(line);
}


void SetWeaponSettingsHookAllowed(bool allowed) { g_hookAllowed.store(allowed, std::memory_order_release); }
unsigned long long WeaponSettingsHits() { return g_weaponSettingsHits; }

bool IsWeaponSwaySuppressed() { return g_suppressed.load(std::memory_order_acquire); }

