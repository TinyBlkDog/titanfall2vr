#include "placement_pin.h"

#include "ads_lock.h"
#include "aim_cmd.h"

#include "viewmodel_bones.h"
#include "diagnostics.h"
#include "camera_update_hook.h"
#include "config.h"
#include "hand_pose.h"
#include "hook_registry.h"
#include "present_hook.h"
#include "viewmodel_instance.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Read by the wrapper on every call. Zero makes it an exact pass-through.
extern "C" volatile std::uint8_t g_placementDriveArmed = 0;
extern "C" volatile std::uint64_t g_placementHits = 0;
extern "C" std::uintptr_t g_placementTrampoline = 0;
// The one object the post-process is allowed to touch. Compared in the
// wrapper, in asm, before the C++ call -- this commit runs for every entity.
extern "C" volatile std::uintptr_t g_placementInstance = 0;

extern "C" void placementCommitInterceptor();

namespace {

// client.dll+0x3DB4A0 -- CalcAbsolutePosition, the REAL entry.
//
// F1's watchpoint recorded the store at +0x3DB7B7, whose .pdata entry carries
// UNW_FLAG_CHAININFO and chains 0x3DB75B -> 0x3DB566 -> 0x3DB4A0. Only the
// last is a function; the first two are continuation fragments with no return
// address on the stack. Hooking the recorded address would have crashed.
constexpr std::uintptr_t kCommitRva = 0x3DB4A0;

// push rbp / push rsi / lea rbp,[rsp-4Fh] / sub rsp,0F8h
// 2 + 1 + 5 + 7 = 15 bytes, every one position independent and ending on an
// instruction boundary. A 14-byte absolute jump fits.
constexpr std::uint8_t kExpectedCommit[] = {
    0x40, 0x55,
    0x56,
    0x48, 0x8D, 0x6C, 0x24, 0xB1,
    0x48, 0x81, 0xEC, 0xF8, 0x00, 0x00, 0x00,
};
constexpr std::size_t kDisplaced = sizeof(kExpectedCommit);

// The spin RATE the angle ramp is built on. The live tf2vr_pin_spin scales it,
// so setting that convar to 0 freezes the rotation -- which is what makes the
// fire animation judgeable without a rotating rig fighting for attention.
constexpr float kSpinDegreesPerSecond = 30.0f;
// Long enough to arm, look, fire and reload without hurrying.
constexpr std::uint64_t kCaptureMs = 30000;

// WHICH ENTITY THE DRIVE POINTS AT -- AND THE GUN IS FIRST NOW.
//
// The bodygroup sweep settled the identification: C_BaseViewModel carries six
// switchable groups and every one is a SCOPE OR MUZZLE, so it is the GUN, and
// C_ViewmodelAttachmentModel is the arms and body. The arms are parented to
// it, which is why moving it moves both and why writing the arms own placement
// is silent.
//
// The gun is therefore target 0. That is not cosmetic: H1 is driven blind with
// a headset on, and "press the key once" has to arm the right entity without
// anyone counting presses.
enum class Target { Viewmodel, Attachment, Count };
const char* const kTargetName[] = {"C_BaseViewModel (THE GUN)",
                                   "C_ViewmodelAttachmentModel (the arms and body)"};
int g_target = 0;
// H1: drive from the composed hand pose instead of the synthetic offset.
// INI: set pin.hand = 1. Off by default, so a flat run is unchanged.
bool g_driveHand = false;

std::uint8_t* g_site = nullptr;
std::uint8_t g_original[24]{};
std::atomic_bool g_installed = false;
std::atomic_bool g_armed = false;
std::uint64_t g_driveStart = 0;
bool g_reportOn = false;   // pin.report, default 0
std::uint64_t g_lastLogTick = 0;

// The falsifier's raw material, written by the post-process and read by the
// per-frame report. Plain floats: one writer, one reader, and a torn read
// costs a log line rather than anything real.
volatile float g_baseAngles[3]{};
volatile float g_basePosition[3]{};
volatile float g_appliedAngles[3]{};
volatile float g_appliedPosition[3]{};
std::atomic_uint64_t g_writes = 0;
std::atomic_uint64_t g_faulted = 0;

bool GuardedPlacementWrite(std::uint8_t* instance, float degrees) {
    __try {
        auto* angles = reinterpret_cast<float*>(instance + kViewmodelAnglesOffset);
        auto* position = reinterpret_cast<float*>(instance + kViewmodelPositionOffset);
        // Read what the ENGINE just committed. This is the base every absolute
        // write is computed from, and it is the falsifier: it must VARY as the
        // player moves, and it must never drift by the offset per frame. A
        // base climbing by 30 a frame means the write is being read back into
        // itself, which is the compounding the dirty-bit gate exists to stop.
        const float baseAngles[3] = {angles[0], angles[1], angles[2]};
        const float basePosition[3] = {position[0], position[1], position[2]};
        for (int i = 0; i < 3; ++i) {
            g_baseAngles[i] = baseAngles[i];
            g_basePosition[i] = basePosition[i];
        }
        // ---- H1: THE HAND, IF THERE IS ONE ----
        //
        // The pose source swaps from synthetic to the verified composition, and
        // nothing about the composition changes: same room-to-Source axis map,
        // same room-origin anchor, same aim-for-orientation / grip-for-position
        // split, all of it headset-verified already. Only the CONSUMER changed
        // -- it used to go through six convars to a script prop, and now it is
        // an absolute write to the placement the engine itself commits.
        //
        // Absolute, like every other write here: the composed pose IS the
        // target, so re-applying it is idempotent and it cannot compound.
        //
        // If no pose has been composed yet the placement is left exactly as the
        // engine committed it. Writing zeros would throw the weapon to the map
        // origin, which looks like a catastrophic failure of the pin rather
        // than like a controller that has not been picked up yet.
        // RELATIVE TO WHAT THE ENGINE JUST COMMITTED, not to a camera global.
        //
        // Four attempts at picking a world anchor each swapped one artefact for
        // another, because every camera global is sampled on a different clock
        // from this instant: the camera base leaps a whole step in one frame,
        // the view origin lags movement by one, and combining them is only as
        // good as the moment each was sampled.
        //
        // basePosition above is the placement the ENGINE just computed for this
        // viewmodel, read microseconds ago in this same function. It is already
        // glued to the eye, already smoothed, already right on stairs -- which
        // is why the gun does not jump in ordinary play. Adding the hand's
        // offset to it is exact by construction, with no clock to skew.
        //
        // The constant difference between the viewmodel's origin and the eye is
        // absorbed by the position calibration, which is what that calibration
        // was always doing anyway.
        // ADS: KEEP THE ENGINE'S CENTRED POSE, CHANGE ONLY WHERE IT POINTS.
        //
        // Two attempts, both reported worse, and they bracket the answer:
        //
        //   pin stands down entirely -- "gun is NOW correctly centered", but
        //   "moving my arms when in ADS still does nothing" and it would not
        //   tilt to where the gun was aimed.
        //
        //   pin drives the whole placement from the hand -- "this is worse. the
        //   gun barely even moves and does not turn into the ADS centered gun
        //   like before." Replacing the placement throws away the centred ADS
        //   pose that was the good part.
        //
        // So the engine keeps the POSITION -- which is what puts the gun in the
        // middle of the screen and is the part that was liked -- and only the
        // PITCH is taken from the hand, so the barrel tilts to where the round
        // is actually going. That is the "translate the gun up/down so it points
        // exactly where the reticle was pointed" ask, done as a rotation in
        // place rather than a displacement.
        //
        // THIS IS A HYPOTHESIS ABOUT THE PLACEMENT ANGLES AND THE RUN IS THE
        // TEST. I have now been wrong twice in a row about how this entity's
        // fields reach the drawn gun, so: if the barrel does not tilt, then
        // +0x114 is not what orients the viewmodel during ADS, and the answer is
        // the weapon's own ADS animation rather than its placement. The applied
        // and base angles are both recorded, so the log distinguishes "we did
        // not write it" from "we wrote it and it did not move".
        if (AdsLockEngaged()) {
            // ADS: THE GUN IS THE VIEW. Settled 2026-09-04 over eleven headset
            // runs; docs/RESULT-AIM-2026-09-04.md carries the evidence.
            //
            //   position  the engine's own centred ADS pose, untouched. It is
            //             built from the view angles we write, so it is already
            //             centred; a rewrite from the render camera sat 4 units
            //             off it with the head turned (the render camera carries
            //             neck-model and lean offsets the engine's eye does not).
            //   pitch/yaw the rendered view's, from aim_cmd.cpp's follow, so
            //             eye, sights and target are collinear. The body yaw is
            //             steered onto the hand every tick; head motion passes
            //             through the camera untouched.
            //   roll      the HAND's. A rifle on the shoulder stays level while
            //             the head cants onto the stock; a gun canted to the
            //             view's roll rotates visibly against the hand holding it.
            //
            // The round is fired along the same rendered angles (aim_cmd.cpp).
            // Until the follow has a rendered frame to read (the entry tick) the
            // engine's pose is used whole. The weapon pass keeps the head-cancel
            // and drops the lean detach in this state (camera_update_hook.cpp).
            float adsAng[3]{}, viewAng[3]{};
            const bool haveHand = TryGetComposedHandOffset(nullptr, adsAng);
            if (haveHand && TryGetAdsViewAngles(viewAng)) {
                angles[0] = viewAng[0];
                angles[1] = viewAng[1];
                angles[2] = adsAng[2];
                for (int i = 0; i < 3; ++i) {
                    g_appliedPosition[i] = basePosition[i];
                    g_appliedAngles[i] = angles[i];
                }
            } else {
                for (int i = 0; i < 3; ++i) {
                    g_appliedPosition[i] = basePosition[i];
                    g_appliedAngles[i] = baseAngles[i];
                }
            }
            NoteAdsLockPlacementReleased();
            return true;
        }
        float handOffset[3]{}, handAng[3]{};
        if (g_driveHand && TryGetComposedHandOffset(handOffset, handAng)) {
            const float handPos[3] = {basePosition[0] + handOffset[0],
                                      basePosition[1] + handOffset[1],
                                      basePosition[2] + handOffset[2]};
            position[0] = handPos[0];
            position[1] = handPos[1];
            position[2] = handPos[2];
            angles[0] = handAng[0];
            angles[1] = handAng[1];
            angles[2] = handAng[2];
            for (int i = 0; i < 3; ++i) {
                g_appliedPosition[i] = handPos[i];
                g_appliedAngles[i] = handAng[i];
            }
            return true;
        }
        // FORWARD, NOT UP. +30 in world Z put the rig directly overhead, where
        // the torso fills the view and the gun is inside it -- which is exactly
        // what run 1 reported. Offsetting along the entity's own facing puts
        // the whole rig out in front of the camera instead, arms and gun both
        // in view, which is the only arrangement in which "does FIRE still
        // animate" is a question anybody can answer by looking.
        //
        // Yaw only, deliberately: folding pitch in would drive the rig into the
        // floor or the sky whenever the player looked up or down, and the thing
        // being judged is whether it animates, not where it points.
        float fwd = 70.0f, side = 0.0f, lift = 0.0f, spin = 15.0f;
        ReadPinOffsets(&fwd, &side, &lift, nullptr);
        constexpr float kToRad = 0.01745329252f;
        const float yaw = baseAngles[1] * kToRad;
        const float forward[2] = {std::cos(yaw), std::sin(yaw)};
        const float rightward[2] = {std::sin(yaw), -std::cos(yaw)};
        // Absolute: the engine's fresh value plus a constant, never += .
        const float applied[3] = {
            basePosition[0] + forward[0] * fwd + rightward[0] * side,
            basePosition[1] + forward[1] * fwd + rightward[1] * side,
            basePosition[2] + lift,
        };
        // Yaw is component 1 of a Source QAngle. Absolute for the same reason.
        // `degrees` arrives already wrapped against the LIVE rate, so it sweeps
        // a full turn and rejoins at 0 continuously. The first version wrapped
        // at 360 and then scaled by spin/30, which at the default spin swept
        // 0..180 and then snapped back -- reported from the run as "rotates to
        // facing behind, then jumps back to 0", and entirely my arithmetic
        // rather than anything the engine was doing.
        const float appliedAngles[3] = {baseAngles[0], baseAngles[1] + degrees, baseAngles[2]};
        position[0] = applied[0];
        position[1] = applied[1];
        position[2] = applied[2];
        angles[0] = appliedAngles[0];
        angles[1] = appliedAngles[1];
        angles[2] = appliedAngles[2];
        for (int i = 0; i < 3; ++i) {
            g_appliedPosition[i] = applied[i];
            g_appliedAngles[i] = appliedAngles[i];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Install() {
    if (g_installed.load(std::memory_order_acquire)) return true;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) {
        Tf2VrLog("[TF2VR] F2 placement pin: client.dll is not loaded; nothing patched.\n");
        return false;
    }
    auto* site = reinterpret_cast<std::uint8_t*>(client) + kCommitRva;
    if (std::memcmp(site, kExpectedCommit, kDisplaced) != 0) {
        char line[420]{};
        int used = std::snprintf(line, sizeof(line),
            "[TF2VR] F2 placement pin: client.dll+0x%llX does not begin with the bytes it was "
            "resolved from, so this is not the build those offsets came from. NOT installed, "
            "nothing patched. Found:", static_cast<unsigned long long>(kCommitRva));
        for (std::size_t i = 0; i < kDisplaced && used < static_cast<int>(sizeof(line)) - 8; ++i) {
            used += std::snprintf(line + used, sizeof(line) - used, " %02X", site[i]);
        }
        std::snprintf(line + used, sizeof(line) - used, "\n");
        Tf2VrLog(line);
        return false;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(site, kDisplaced, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Tf2VrLog("[TF2VR] F2 placement pin: VirtualProtect failed; nothing patched.\n");
        return false;
    }
    std::memcpy(g_original, site, kDisplaced);

    // jmp qword ptr [rip+0], then the absolute target immediately after.
    // Built from a ZEROED array: bytes 2..5 are the displacement and MUST be
    // zero. sway_probe.cpp records what happens otherwise -- a memset of 0x90
    // first left disp32 = 0x90909090 and the first call jumped through a wild
    // address and took the process down.
    std::uint8_t detour[sizeof(g_original)]{};
    detour[0] = 0xFF;
    detour[1] = 0x25;
    const auto target = reinterpret_cast<std::uintptr_t>(placementCommitInterceptor);
    std::memcpy(detour + 6, &target, sizeof(target));
    if (kDisplaced > 14) std::memset(detour + 14, 0x90, kDisplaced - 14);

    std::uint32_t displacement = 0;
    std::memcpy(&displacement, detour + 2, sizeof(displacement));
    std::uintptr_t encoded = 0;
    std::memcpy(&encoded, detour + 6, sizeof(encoded));
    if (detour[0] != 0xFF || detour[1] != 0x25 || displacement != 0 || encoded != target) {
        char line[360]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] F2 placement pin: refusing to patch -- the constructed detour is malformed "
            "(op %02X %02X, disp32 0x%08X, target 0x%llX). Nothing was written.\n",
            detour[0], detour[1], displacement, static_cast<unsigned long long>(encoded));
        Tf2VrLog(line);
        DWORD ignoredProtect = 0;
        VirtualProtect(site, kDisplaced, oldProtect, &ignoredProtect);
        return false;
    }

    auto* trampoline = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        Tf2VrLog("[TF2VR] F2 placement pin: could not allocate a trampoline; nothing patched.\n");
        DWORD ignoredProtect = 0;
        VirtualProtect(site, kDisplaced, oldProtect, &ignoredProtect);
        return false;
    }
    std::memcpy(trampoline, g_original, kDisplaced);
    std::uint8_t* jump = trampoline + kDisplaced;
    jump[0] = 0xFF;
    jump[1] = 0x25;
    std::memset(jump + 2, 0, 4);
    const auto resume = reinterpret_cast<std::uintptr_t>(site + kDisplaced);
    std::memcpy(jump + 6, &resume, sizeof(resume));
    FlushInstructionCache(GetCurrentProcess(), trampoline, 64);
    g_placementTrampoline = reinterpret_cast<std::uintptr_t>(trampoline);
    RegisterHookSite("placement-pin CalcAbsolutePosition trampoline", trampoline, 64);
    RegisterHookSite("placement-pin CalcAbsolutePosition patched entry", site, kDisplaced);

    std::memcpy(site, detour, kDisplaced);
    FlushInstructionCache(GetCurrentProcess(), site, kDisplaced);
    DWORD ignored = 0;
    VirtualProtect(site, kDisplaced, oldProtect, &ignored);
    g_site = site;
    g_installed.store(true, std::memory_order_release);

    char line[380]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F2 placement pin: wrapped CalcAbsolutePosition at client.dll+0x%llX, %zu bytes "
        "displaced, trampoline at %p resuming at client.dll+0x%llX. Pass-through until armed.\n",
        static_cast<unsigned long long>(kCommitRva), kDisplaced,
        static_cast<void*>(trampoline),
        static_cast<unsigned long long>(kCommitRva + kDisplaced));
    Tf2VrLog(line);
    return true;
}

void Disarm(const char* why) {
    g_placementDriveArmed = 0;
    SetDiagnosticUploadHookHold(false);
    SetPinOwnsRenderPath(false);
    g_armed.store(false, std::memory_order_release);
    char line[520]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F2 placement pin DISARMED (%s): %llu commit calls seen, %llu writes applied, "
        "%llu faulted. Last engine base pos=(%.2f %.2f %.2f) ang=(p%.2f y%.2f r%.2f); last "
        "applied pos=(%.2f %.2f %.2f) ang=(p%.2f y%.2f r%.2f).\n",
        why, static_cast<unsigned long long>(g_placementHits),
        static_cast<unsigned long long>(g_writes.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_faulted.load(std::memory_order_acquire)),
        g_basePosition[0], g_basePosition[1], g_basePosition[2],
        g_baseAngles[0], g_baseAngles[1], g_baseAngles[2],
        g_appliedPosition[0], g_appliedPosition[1], g_appliedPosition[2],
        g_appliedAngles[0], g_appliedAngles[1], g_appliedAngles[2]);
    Tf2VrLog(line);
}

}  // namespace

extern "C" void ApplyPlacementPin(void* instance) {
    // The wrapper already compared this against g_placementInstance, so the
    // only thing left to check is that the object is still what we cached --
    // a weapon switch or respawn frees it and the heap stays committed. The
    // class checked must match the class SELECTED, or the validity test
    // rejects a perfectly live object on every call and the drive goes silent
    // while every counter looks healthy.
    auto* bytes = static_cast<std::uint8_t*>(instance);
    const bool valid = g_target == static_cast<int>(Target::Viewmodel)
                           ? ViewmodelInstanceStillValid(bytes)
                           : AttachmentInstanceStillValid(bytes);
    if (!valid) {
        g_placementInstance = 0;
        return;
    }
    const std::uint64_t now = GetTickCount64();
    if (g_driveStart == 0) g_driveStart = now;
    // Built from the LIVE rate and wrapped afterwards, so the sweep is a whole
    // continuous turn whatever tf2vr_pin_spin is set to -- and 0 means frozen
    // rather than "still moving, just slower".
    float spin = kSpinDegreesPerSecond;
    ReadPinOffsets(nullptr, nullptr, nullptr, &spin);
    double degrees = static_cast<double>(now - g_driveStart) / 1000.0 * spin;
    degrees -= 360.0 * std::floor(degrees / 360.0);
    if (GuardedPlacementWrite(bytes, static_cast<float>(degrees))) {
        g_writes.fetch_add(1, std::memory_order_relaxed);
    } else {
        // One fault is enough. Writing into an object that just faulted is how
        // a diagnostic becomes a crash.
        g_faulted.fetch_add(1, std::memory_order_relaxed);
        g_placementInstance = 0;
    }
}

void SetPlacementPinHandDriven(bool hand) { g_driveHand = hand; }
bool IsPlacementPinHandDriven() { return g_driveHand; }

void ToggleViewmodelPlacementPin() {
    // ONE KEY, CYCLING: attachment -> body -> off. In run 1 the gun could not
    // be seen at all, because the body it hides inside was the thing being
    // moved. Being able to advance the target without ending the session is
    // what stops the next answer costing another run.
    if (g_armed.load(std::memory_order_acquire)) {
        Disarm("by key");
        ++g_target;
        if (g_target >= static_cast<int>(Target::Count)) {
            g_target = 0;
            Tf2VrLog("[TF2VR] F2 placement pin: that was the last target; the next press starts "
                     "again at the attachment.\n");
            return;
        }
        char line[260]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] F2 placement pin: the next press drives %s.\n", kTargetName[g_target]);
        Tf2VrLog(line);
        return;
    }
    if (!Install()) return;

    auto pick = [] {
        return g_target == static_cast<int>(Target::Viewmodel) ? FindLiveViewmodelInstance()
                                                               : FindLiveAttachmentInstance();
    };
    const unsigned char* instance = pick();
    if (!instance && !g_driveHand) {
        // THE HEAP SCAN IS A FLAT-RUN TOOL ONLY, NOW.
        //
        // It blocks the main thread for about three seconds. Flat that is
        // merely unpleasant; with an XR session live it killed four armed runs
        // in a row, and the SEH guards never fired -- because the fault was
        // never a read fault on our thread, so no amount of guarding the reads
        // was ever going to help. The engine lookup replaces it and is instant,
        // so the hand-driven path never reaches for it: if the engine lookup
        // fails, arming is REFUSED rather than risking the stall again.
        Tf2VrLog("[TF2VR] F2 placement pin: nothing cached; running the bone scan (flat only).\n");
        CacheWeaponBoneInstance();
        instance = pick();
    }
    if (!instance) {
        char miss[460]{};
        std::snprintf(miss, sizeof(miss),
            "[TF2VR] F2 placement pin: NOT ARMED -- no live %s. %sBe in a level, holding a "
            "weapon, before pressing this.\n", kTargetName[g_target],
            g_driveHand ? "The engine lookup returned nothing, and the heap scan is deliberately "
                          "NOT run while a headset session is live because its three-second "
                          "main-thread stall is what was killing the session. "
                        : "");
        Tf2VrLog(miss);
        return;
    }
    // Both candidates side by side at arm time. They must be DIFFERENT objects;
    // if the pair collapses to one address the scan did not really resolve two
    // entities and "which one moved" would be unanswerable.
    {
        char line[420]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] F2 candidates: viewmodel=%p attachment=%p. These must differ -- one address "
            "for both means the pair was never resolved.\n",
            static_cast<const void*>(FindLiveViewmodelInstance()),
            static_cast<const void*>(FindLiveAttachmentInstance()));
        Tf2VrLog(line);
    }

    g_placementInstance = reinterpret_cast<std::uintptr_t>(instance);
    g_driveStart = 0;
    g_lastLogTick = 0;
    g_writes.store(0, std::memory_order_release);
    g_faulted.store(0, std::memory_order_release);
    g_placementHits = 0;
    g_armed.store(true, std::memory_order_release);
    g_placementDriveArmed = 1;
    // F3 reads the world pass through the upload hook, and on a flat run
    // nothing else installs it. Held for as long as the pin is armed.
    SetDiagnosticUploadHookHold(true);
    SetPinOwnsRenderPath(true);

    // P0 SECTION 5, ENFORCED RATHER THAN REMEMBERED.
    //
    // With the placement pinned, three other things claim the same question and
    // must not: the viewmodel correction (the gun family already carries the
    // world orientation -- measured, 0.000 degrees between its row0 and the
    // world's -- so the correction would double-rotate it), the camera-pass
    // weapon pin (the previous generation's attempt at this goal, rotation-only
    // about the eye), and the lean detach (a body-frame translation with no
    // referent once the gun sits at a real world position).
    //
    // autoarm >= 2 turns the correction ON as part of arming head tracking, so
    // on the ONE headset run all of this would be armed by default and someone
    // would have to remember to press INSERT blind. Single ownership is a
    // property of the design, not something to recall with a headset on.
    // THE LEAN DETACH IS NO LONGER DISARMED HERE. 2026-09-03.
    //
    // The paragraph above is about ORIENTATION -- two writers rotating the gun
    // fight, and the pin wins. The lean detach is a TRANSLATION, and the claim
    // that it has "no referent once the gun sits at a real world position" was
    // wrong about what the pin does: the pin applies an offset in the entity's
    // own yaw frame AFTER the engine's commit, so the gun still rides whatever
    // the engine put it on -- which is the camera. Disarming the detach
    // therefore left the whole viewmodel family riding head translation.
    //
    // Reported from the headset: "when I lean my head left or right, the body
    // AND arms and gun move with it. Those need to stay pinned to where my hand
    // is." That is this disarm, exactly. The detach's own default is 1.0 and it
    // now stands. Bare F5 toggles it live so one run carries both arms.
    if (g_driveHand) {
        SetVerifiedGameViewmodelCompensation(false);
        SetWeaponPinnedToController(false);
        Tf2VrLog("[TF2VR] H1 single ownership: viewmodel correction and the camera-pass pin are "
                 "DISARMED -- the placement pin owns where the gun POINTS. The world-FOV match "
                 "stays ARMED (it owns the frustum), and the lean detach stays ARMED (it owns "
                 "head TRANSLATION, which the pin does not touch). F5 toggles the lean detach.\n");
    }

    float fwd = 0.0f, side = 0.0f, lift = 0.0f, spin = 0.0f;
    ReadPinOffsets(&fwd, &side, &lift, &spin);
    char line[560]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F2 placement pin ARMED on %s at %p: ABSOLUTE offset fwd %.0f / right %.0f / up "
        "%.0f in the entity's own yaw frame, spin %.0f deg/s, applied after the engine's own "
        "commit and only on the calls where it recomputed. Tune from the INI -- pin.fwd / "
        "pin.right / pin.up / pin.spin, reloaded live with LEADER then END. NOT from the console: "
        "setting these as convars crashed the game. pin.spin 0 freezes the rotation so the fire "
        "animation can be watched cleanly. %s\n",
        kTargetName[g_target], static_cast<const void*>(instance), fwd, side, lift, spin,
        g_driveHand ? "Hand-driven: it does NOT time out."
                    : "Diagnostic mode: closes itself after 30 s.");
    Tf2VrLog(line);
}

bool IsViewmodelPlacementPinArmed() { return g_armed.load(std::memory_order_acquire); }

void AdvanceViewmodelPlacementPin() {
    if (!g_armed.load(std::memory_order_acquire)) return;
    const std::uint64_t now = GetTickCount64();
    // NO EXPIRY WHILE THE HAND DRIVES IT.
    //
    // The capture window exists because the NON-hand modes are diagnostics: they
    // pin the viewmodel to a fixed offset to answer a question, and leaving that
    // armed forever would leave the gun stuck somewhere nobody asked for.
    //
    // Hand-driven is not a diagnostic any more, it is how the wearer holds the
    // weapon, and a timeout on it means the gun silently stops tracking the
    // controller mid-session. Reported as "motion control weapons time out after
    // a period of time"; it was 240 s.
    if (!g_driveHand && g_driveStart != 0 && now - g_driveStart >= kCaptureMs) {
        Disarm("capture window elapsed");
        return;
    }
    // THE STALENESS DETECTOR CANNOT LIVE ON A CALL THAT HAS STOPPED HAPPENING.
    //
    // ApplyPlacementPin zeroes g_placementInstance when the object it cached is
    // no longer a viewmodel -- but it only runs when the hooked
    // CalcAbsolutePosition is called WITH that cached pointer. After a level
    // reload the game builds a NEW viewmodel at a new address, the wrapper never
    // matches again, and so the check never runs: g_placementInstance keeps a
    // stale non-zero value, the pin reports itself armed forever, and the write
    // counter simply stops. Measured exactly that -- "F2: 22784801 commits, 9436
    // writes" frozen across three samples with no disarm line anywhere, while
    // the wearer looked at a gun sitting where flat mode draws it.
    //
    // So the liveness question is asked from HERE, on a clock that keeps
    // running, against the entity list rather than against our own cache. A
    // mismatch disarms, and the watchdog in plugin.cpp then re-arms through the
    // ordinary path -- reusing the arming code that is known to work rather than
    // inventing a second way to re-cache a pointer.
    if (g_driveHand && g_placementInstance) {
        const auto* live = FindLiveViewmodelInstance();
        if (live && reinterpret_cast<std::uintptr_t>(live) != g_placementInstance) {
            Disarm("the viewmodel was rebuilt (level reload or respawn); re-arming");
            return;
        }
    }
    if (!g_placementInstance) {
        Disarm("the instance stopped being a C_BaseViewModel -- weapon switch or respawn");
        return;
    }
    // 4 Hz while the hand drives it. The correct room-to-world rotation is
    // going to be FITTED from this capture rather than swept for by hand, and a
    // once-a-second sample across a slow turn is too coarse to fit anything to.
    // EVERYTHING BELOW IS REPORTING, AND IT COST 6.0-7.6 ms IN THE FRAME IT
    // LANDS ON -- at 4 Hz while the hand drives, which is the ~4 hitches a
    // second the wearer felt on sprint and slide (2026-09-08, segment vm.pin).
    // It answers questions from the calibration runs -- the falsifier against
    // the other candidate rig, the predicted screen position, the fit -- and
    // the grip is settled. The PIN ITSELF is not here: it runs from the
    // placement hook and is untouched by this gate.
    if (!g_reportOn) return;
    if (now - g_lastLogTick < (g_driveHand ? 250u : 1000u)) return;
    g_lastLogTick = now;
    // THE FALSIFIER, plus the OTHER candidate's live placement beside it.
    //
    // The unanswered question from run 1 is whether the gun rides the body's
    // root or sits on its own. Driving one entity and reading the other every
    // second answers it from the log rather than from squinting at a screen:
    // if the undriven one's committed position FOLLOWS the driven one out to
    // the offset, the two are one parented rig and moving the root moves both.
    // If it stays put, they are independent and the gun is somewhere else
    // again.
    const unsigned char* other = g_target == static_cast<int>(Target::Viewmodel)
                                     ? FindLiveAttachmentInstance()
                                     : FindLiveViewmodelInstance();
    float otherPos[3]{};
    if (other) {
        __try {
            std::memcpy(otherPos, other + kViewmodelPositionOffset, sizeof(otherPos));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    // ---- F3: WHERE THE WORLD CAMERA SAYS THE RIG SHOULD BE ON SCREEN ----
    //
    // P0 section 7, computed live instead of reconstructed afterwards. The rig
    // is at a world position we chose, so the world pass's own matrix says
    // exactly which pixel it projects to; if the pinned rig is not drawn there,
    // the viewmodel pass is not agreeing with the world and H1 would put the
    // gun somewhere the hand is not.
    //
    // The projection is the WORLD's, deliberately. The gun is drawn through the
    // viewmodel pass, which P0 measured as the same matrix scaled by two -- so
    // predicting from the world and comparing against what is drawn is exactly
    // the test of whether the FOV match is undoing that factor.
    float world[16]{}, eye[3]{};
    char predicted[200]{};
    std::snprintf(predicted, sizeof(predicted), "no main-scene pass seen yet, so no prediction");
    if (GetMainSceneProjection(world, eye)) {
        const float p[3] = {g_appliedPosition[0] - eye[0], g_appliedPosition[1] - eye[1],
                            g_appliedPosition[2] - eye[2]};
        float clip[4]{};
        for (int row = 0; row < 4; ++row) {
            clip[row] = world[row * 4 + 0] * p[0] + world[row * 4 + 1] * p[1] +
                        world[row * 4 + 2] * p[2] + world[row * 4 + 3];
        }
        if (clip[3] > 0.001f) {
            std::snprintf(predicted, sizeof(predicted),
                "predicted screen %.1f%% across, %.1f%% down, depth %.1f units",
                100.0f * (clip[0] / clip[3] + 1.0f) * 0.5f,
                100.0f * (1.0f - clip[1] / clip[3]) * 0.5f, clip[3]);
        } else {
            std::snprintf(predicted, sizeof(predicted),
                "BEHIND THE CAMERA (w=%.2f), so nothing should be drawn", clip[3]);
        }
    }
    char f3[420]{};
    std::snprintf(f3, sizeof(f3),
        "[TF2VR] F3: world-FOV match %s, correction %s, camera-pass pin %s. %s. With the match ON "
        "the rig should sit where this says; with it OFF it should sit about twice as far from "
        "the centre, because the viewmodel frustum is the world's scaled by two.\n",
        IsViewmodelMatchWorldFov() ? "ARMED" : "off",
        IsViewmodelCompensationEnabled() ? "ARMED (it must NOT be)" : "off",
        IsWeaponPinnedToController() ? "ARMED (it must NOT be)" : "off", predicted);
    Tf2VrLog(f3);

    // THE YAW FIT ROW. Everything needed to solve for the room-to-world
    // rotation offline: what the engine put on the viewmodel, what we put
    // there, and the two raw yaws the composition was built from.
    if (g_driveHand) {
        float headYaw = 0.0f, localYaw = 0.0f, gameYaw = 0.0f;
        if (TryGetHandYawInputs(&headYaw, &localYaw, &gameYaw)) {
            char fit[380]{};
            std::snprintf(fit, sizeof(fit),
                "[TF2VR] YAWFIT head=%.2f hand=%.2f game=%.2f engineYaw=%.2f ourYaw=%.2f "
                "pos=(%.2f %.2f %.2f) base=(%.2f %.2f %.2f)\n",
                headYaw, localYaw, gameYaw, g_baseAngles[1], g_appliedAngles[1],
                g_appliedPosition[0], g_appliedPosition[1], g_appliedPosition[2],
                g_basePosition[0], g_basePosition[1], g_basePosition[2]);
            Tf2VrLog(fit);
        }
    }
    char line[680]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F2: %llu commits, %llu writes, target entity %p. ENGINE BASE pos=(%.2f %.2f %.2f) "
        "ang=(p%.2f y%.2f "
        "r%.2f) -> APPLIED pos=(%.2f %.2f %.2f) ang=(p%.2f y%.2f r%.2f). The OTHER candidate sits "
        "at (%.2f %.2f %.2f) -- if that follows the applied position, the two are one parented "
        "rig. The base must MOVE as you walk and must NOT run away frame on frame.\n",
        static_cast<unsigned long long>(g_placementHits),
        static_cast<unsigned long long>(g_writes.load(std::memory_order_acquire)),
        reinterpret_cast<const void*>(g_placementInstance),
        g_basePosition[0], g_basePosition[1], g_basePosition[2],
        g_baseAngles[0], g_baseAngles[1], g_baseAngles[2],
        g_appliedPosition[0], g_appliedPosition[1], g_appliedPosition[2],
        g_appliedAngles[0], g_appliedAngles[1], g_appliedAngles[2],
        otherPos[0], otherPos[1], otherPos[2]);
    Tf2VrLog(line);
}

bool TryGetPlacementAngles(float applied[3], float base[3]) {
    if (g_writes.load(std::memory_order_relaxed) == 0) return false;
    for (int i = 0; i < 3; ++i) {
        if (applied) applied[i] = g_appliedAngles[i];
        if (base) base[i] = g_baseAngles[i];
    }
    return true;
}


bool TryGetPlacementPosition(float applied[3], float base[3]) {
    if (g_writes.load(std::memory_order_relaxed) == 0) return false;
    for (int i = 0; i < 3; ++i) {
        if (applied) applied[i] = g_appliedPosition[i];
        if (base) base[i] = g_basePosition[i];
    }
    return true;
}

// pin.report: the 4 Hz placement diagnostic block. OFF by default -- measured
// at 6.0-7.6 ms in the frame it lands on. The pin mechanism is unaffected.
void SetPlacementPinReportEnabled(bool enabled) { g_reportOn = enabled; }
