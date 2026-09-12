#include "viewmodel_visibility.h"

#include "viewmodel_bones.h"
#include "diagnostics.h"
#include "viewmodel_instance.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr std::uintptr_t kSetVisibleRva = 0x3F0140;
// sub rsp,28h / mov r8,rcx / test rcx,rcx -- refuse anything else.
constexpr std::uint8_t kSetVisiblePrologue[] = {0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8B, 0xC1,
                                                0x48, 0x85, 0xC9};
// The byte SetVisible tests before it will do anything. Zero means it takes the
// error path, so this is read and obeyed rather than discovered.
constexpr std::size_t kVisibilityGateOffset = 0x4B8;
constexpr std::size_t kVisibilityClearOffset = 0x16C;
constexpr std::size_t kVisibilitySetOffset = 0x170;

using SetVisibleFn = void(__fastcall*)(void* entity, int visible);
SetVisibleFn g_setVisible = nullptr;
bool g_resolved = false;

// Long enough to look, short enough that four of them is a brief run.
constexpr std::uint64_t kPhaseMs = 6000;

enum class Phase { HideArms, ShowArms, HideGun, ShowGun, Done };
const char* const kPhaseName[] = {"ARMS HIDDEN (the gun should remain)",
                                  "arms restored",
                                  "GUN HIDDEN (a control -- the arms should remain)",
                                  "gun restored", "done"};

std::atomic_bool g_running = false;
int g_phase = 0;
std::uint64_t g_phaseStart = 0;
std::uint8_t* g_arms = nullptr;
std::uint8_t* g_gun = nullptr;
bool g_armsGated = false;
bool g_gunGated = false;

std::uint8_t ReadGate(const std::uint8_t* entity) {
    __try {
        return *(entity + kVisibilityGateOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

std::uint32_t ReadDword(const std::uint8_t* entity, std::size_t offset) {
    __try {
        return *reinterpret_cast<const std::uint32_t*>(entity + offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool CallSetVisible(std::uint8_t* entity, int visible) {
    __try {
        g_setVisible(entity, visible);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Resolve() {
    if (g_resolved) return g_setVisible != nullptr;
    g_resolved = true;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return false;
    auto* at = reinterpret_cast<std::uint8_t*>(client) + kSetVisibleRva;
    if (std::memcmp(at, kSetVisiblePrologue, sizeof(kSetVisiblePrologue)) != 0) {
        Tf2VrLog("[TF2VR] visibility probe: client.dll+0x3F0140 does not begin with the bytes "
                 "SetVisible was resolved from. This is not that build. NOTHING called.\n");
        return false;
    }
    g_setVisible = reinterpret_cast<SetVisibleFn>(at);
    return true;
}

void Report(const std::uint8_t* entity, const char* label, bool* gated) {
    const std::uint8_t gate = ReadGate(entity);
    *gated = gate != 0;
    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] %s (%p): gate byte +0x4B8 = %u -> SetVisible %s. Visibility words: +0x16C=0x%08X "
        "+0x170=0x%08X\n",
        label, static_cast<const void*>(entity), gate,
        gate ? "WILL RUN" : "WOULD REFUSE, so it will not be called",
        ReadDword(entity, kVisibilityClearOffset), ReadDword(entity, kVisibilitySetOffset));
    Tf2VrLog(line);
}

void EnterPhase(int phase) {
    g_phase = phase;
    g_phaseStart = GetTickCount64();
    std::uint8_t* entity = nullptr;
    int visible = 1;
    switch (static_cast<Phase>(phase)) {
        case Phase::HideArms: entity = g_arms; visible = 0; break;
        case Phase::ShowArms: entity = g_arms; visible = 1; break;
        case Phase::HideGun: entity = g_gun; visible = 0; break;
        case Phase::ShowGun: entity = g_gun; visible = 1; break;
        default: return;
    }
    const bool gated = (entity == g_arms) ? g_armsGated : g_gunGated;
    if (!entity || !gated) {
        char skip[260]{};
        std::snprintf(skip, sizeof(skip),
                      "[TF2VR] visibility phase %d (%s): SKIPPED -- no entity, or its gate is "
                      "clear and calling would take the error path.\n",
                      phase + 1, kPhaseName[phase]);
        Tf2VrLog(skip);
        return;
    }
    const bool ok = CallSetVisible(entity, visible);
    char line[380]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR] visibility phase %d of 4: %s%s. +0x16C=0x%08X +0x170=0x%08X. LOOK.\n",
                  phase + 1, kPhaseName[phase], ok ? "" : "  (CALL FAULTED)",
                  ReadDword(entity, kVisibilityClearOffset),
                  ReadDword(entity, kVisibilitySetOffset));
    Tf2VrLog(line);
}

// Puts everything back regardless of which phase we stopped in. Called on the
// normal end, on the key, and when an entity goes away.
void RestoreAll(const char* why) {
    // Revalidated by CLASS before touching either: a weapon switch frees these
    // and the heap stays committed, so a readability test alone would pass.
    if (g_arms && g_armsGated && AttachmentInstanceStillValid(g_arms)) CallSetVisible(g_arms, 1);
    if (g_gun && g_gunGated && ViewmodelInstanceStillValid(g_gun)) CallSetVisible(g_gun, 1);
    char line[220]{};
    std::snprintf(line, sizeof(line), "[TF2VR] visibility probe finished (%s); both entities set "
                                      "visible again.\n", why);
    Tf2VrLog(line);
    g_arms = nullptr;
    g_gun = nullptr;
    g_running.store(false, std::memory_order_release);
}

}  // namespace

// READ-ONLY. Finds where the GUN entity references the ARMS entity.
//
// SetVisible ran on the arms with a permissive gate and hid nothing, so the
// entity's own visibility state is not what suppresses its draw. The arms are
// a CHILD, so the parent must hold a reference to them, and whatever the draw
// walks to reach that reference -- a list, a count, a flag beside it -- is the
// candidate for stopping it being drawn.
//
// Scanning for the pointer locates that reference without guessing an offset,
// which is the same move the bone scan makes for entities. Both directions,
// because the link may be recorded either way.
void ScanForLink(const std::uint8_t* haystack, const char* haystackName,
                 const std::uint8_t* needle, const char* needleName) {
    constexpr std::size_t kSpan = 0x2000;
    const auto wanted = reinterpret_cast<std::uintptr_t>(needle);
    int hits = 0;
    for (std::size_t offset = 0; offset + 8 <= kSpan; offset += 8) {
        std::uintptr_t value = 0;
        __try {
            value = *reinterpret_cast<const std::uintptr_t*>(haystack + offset);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        if (value != wanted) continue;
        ++hits;
        // The NEIGHBOURHOOD matters more than the hit itself: a list pointer
        // usually sits beside its count, and a flag beside what it gates.
        std::uint32_t around[8]{};
        const std::size_t start = offset >= 16 ? offset - 16 : 0;
        __try {
            std::memcpy(around, haystack + start, sizeof(around));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        char line[420]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR]   %s+0x%04zX holds the %s pointer. dwords from +0x%04zX: "
            "%08X %08X %08X %08X %08X %08X %08X %08X\n",
            haystackName, offset, needleName, start, around[0], around[1], around[2], around[3],
            around[4], around[5], around[6], around[7]);
        Tf2VrLog(line);
    }
    char line[260]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR]   %s -> %s: %d reference(s) in the first 0x%zX bytes.\n", haystackName,
                  needleName, hits, kSpan);
    Tf2VrLog(line);
}

void ToggleViewmodelVisibilityProbe() {
    if (g_running.load(std::memory_order_acquire)) { RestoreAll("stopped by key"); return; }
    if (!Resolve()) return;

    const unsigned char* gun = FindLiveViewmodelInstance();
    if (!gun) {
        Tf2VrLog("[TF2VR] visibility probe: nothing cached yet; running the bone scan first.\n");
        CacheWeaponBoneInstance();
        gun = FindLiveViewmodelInstance();
    }
    const unsigned char* arms = FindLiveAttachmentInstance();
    if (!gun && !arms) {
        Tf2VrLog("[TF2VR] visibility probe: NOT started -- neither entity is live. Be in a level, "
                 "holding a weapon.\n");
        return;
    }

    Tf2VrLog("[TF2VR] === VIEWMODEL VISIBILITY: reading the gate before calling anything ===\n");
    g_armsGated = false;
    g_gunGated = false;
    if (arms) Report(arms, "ARMS  C_ViewmodelAttachmentModel", &g_armsGated);
    if (gun) Report(gun, "GUN   C_BaseViewModel", &g_gunGated);

    // The link, read-only, before anything is called. This is the deliverable
    // now that SetVisible is known inert on the arms.
    if (gun && arms) {
        Tf2VrLog("[TF2VR] === PARENT/CHILD LINK (read-only) ===\n");
        ScanForLink(gun, "GUN", arms, "ARMS");
        ScanForLink(arms, "ARMS", gun, "GUN");
    }
    if (!g_armsGated && !g_gunGated) {
        Tf2VrLog("[TF2VR] visibility probe: BOTH gates are clear, so SetVisible would refuse on "
                 "both and nothing is called. That is a real answer -- this lever does not apply "
                 "to the viewmodel pair, and the section 5 ladder is next.\n");
        return;
    }

    g_arms = const_cast<std::uint8_t*>(arms);
    g_gun = const_cast<std::uint8_t*>(gun);
    g_running.store(true, std::memory_order_release);
    char line[340]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] === VISIBILITY SEQUENCE: 4 phases, %llu s each. Hides the ARMS, restores them, "
        "then hides the GUN as a control and restores that. Everything is put back. ===\n",
        static_cast<unsigned long long>(kPhaseMs / 1000));
    Tf2VrLog(line);
    EnterPhase(0);
}

bool IsViewmodelVisibilityProbeRunning() { return g_running.load(std::memory_order_acquire); }

void AdvanceViewmodelVisibilityProbe() {
    if (!g_running.load(std::memory_order_acquire)) return;
    if (GetTickCount64() - g_phaseStart < kPhaseMs) return;
    if (g_phase + 1 >= static_cast<int>(Phase::Done)) { RestoreAll("all phases shown"); return; }
    EnterPhase(g_phase + 1);
}
