#include "dpad_gesture.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>

#include "camera_hook.h"
#include "diagnostics.h"
#include "xr_input.h"

namespace {

// XInput's own d-pad bits, repeated rather than shared because xinput_pad.cpp's
// copies are file-local there and this module must not depend on that file's
// internals to name a value the API defines.
constexpr std::uint16_t kDpadUp = 0x0001;
constexpr std::uint16_t kDpadDown = 0x0002;
constexpr std::uint16_t kDpadLeft = 0x0004;
constexpr std::uint16_t kDpadRight = 0x0008;

std::atomic<int> g_source{3};
std::atomic<float> g_headUp{-0.15f};
std::atomic<float> g_headRadius{0.45f};
std::atomic<int> g_dwellMs{0};
std::atomic<float> g_deadzone{0.60f};

std::atomic_bool g_engaged{false};
std::atomic<std::uint16_t> g_buttons{0};
std::atomic_bool g_viewButton{false};

bool g_modifierWasHeld = false;
std::uint64_t g_modifierSinceTick = 0;
bool g_engagedLatched = false;

// FRESHNESS, not a latching "it worked once". All of these are printed on the
// status line, so a gesture that never fires says WHICH half is missing -- the
// modifier never recognised, or the stick never pushed under it.
std::uint64_t g_frames = 0;
std::uint64_t g_modifierHeldFrames = 0;
std::uint64_t g_engagements = 0;
std::uint64_t g_directionsEmitted = 0;
std::uint64_t g_viewPresses = 0;
bool g_thumbrestEverSeen = false;
bool g_headGestureEverSeen = false;

// THE TWO EARLY RETURNS IN THE HEAD GESTURE, COUNTED.
//
// `head gesture seen=0` was read for four sessions as "the wearer never tried
// it". It cannot mean that on its own: the test returns early, silently, if the
// left grip is not tracked or if no head position is published, and a gesture
// that is never EVALUATED reads exactly the same as one that is evaluated and
// rejected. Two nulls from an instrument that may never have reached its target
// are about the instrument. These separate the three cases:
//
//   evaluated > 0, seen = 0   the test ran and said no -- the gesture, or its
//                             geometry, is what to fix
//   evaluated = 0             the test never ran -- fix whichever counter below
//                             is doing the blocking, not the geometry
std::uint64_t g_headNoGripPose = 0;
std::uint64_t g_headNoHeadPose = 0;
std::uint64_t g_headEvaluated = 0;

bool ThumbrestModifierHeld() {
    // EITHER hand's rest. The wearer's description is the pad left of A/B --
    // the RIGHT controller, shifting the LEFT stick, which is the cross-hand
    // grip these mods use. The left rest is accepted too and costs nothing.
    const bool held = g_controllerThumbrestTouch[0] != 0 || g_controllerThumbrestTouch[1] != 0;
    if (held) g_thumbrestEverSeen = true;
    return held;
}

bool HeadGestureModifierHeld() {
    // The LEFT controller raised to head height and held near it. Grip pose,
    // because that is where the hand physically is; the aim ray is about where
    // it points, which is not what "beside my head" means.
    if ((g_controllerPoseFlags[0] & kControllerGripTracked) == 0) { ++g_headNoGripPose; return false; }
    float head[3]{};
    if (!ReadHeadPositionMetres(head)) { ++g_headNoHeadPose; return false; }
    ++g_headEvaluated;
    const float handX = g_controllerGripPositionMetres[0][0];
    const float handY = g_controllerGripPositionMetres[0][1];
    const float handZ = g_controllerGripPositionMetres[0][2];
    // OpenXR: +Y is up, and X/Z span the horizontal plane. The horizontal test
    // is a RADIUS rather than a signed left/right, deliberately: it is the left
    // hand, so "beside the head" is already on the left, and a signed test
    // would need the head's yaw -- a second clock, and a second thing to be
    // wrong -- to say which way left is.
    if (handY - head[1] < g_headUp.load(std::memory_order_relaxed)) return false;
    const float dx = handX - head[0];
    const float dz = handZ - head[2];
    const float radius = g_headRadius.load(std::memory_order_relaxed);
    const bool held = (dx * dx + dz * dz) <= radius * radius;
    if (held) g_headGestureEverSeen = true;
    return held;
}

bool ModifierHeld() {
    switch (g_source.load(std::memory_order_relaxed)) {
        case 0: return false;
        case 1: return ThumbrestModifierHeld();
        case 2: return HeadGestureModifierHeld();
        default: return ThumbrestModifierHeld() || HeadGestureModifierHeld();
    }
}

}  // namespace

void AdvanceDpadGesture() {
    ++g_frames;
    const float lx = g_controllerThumbstick[0][0];
    const float ly = g_controllerThumbstick[0][1];
    const float deadzone = g_deadzone.load(std::memory_order_relaxed);

    const bool modifierHeld = ModifierHeld();
    const std::uint64_t now = GetTickCount64();
    if (modifierHeld && !g_modifierWasHeld) g_modifierSinceTick = now;
    g_modifierWasHeld = modifierHeld;
    if (modifierHeld) ++g_modifierHeldFrames;

    // HELD MEANS HELD. The shift is on while the pad is touched and off the
    // instant it is released -- the convention every mod that does this uses,
    // and the wearer's own correction.
    //
    // AN EARLIER DRAFT GOT THIS WRONG and the mistake is worth keeping written
    // down. It made the modifier ARM for a window and then expire, to stop a
    // thumb resting on a capacitive pad from holding the stick hostage. That
    // reasoning ignored where the thumb physically is: a thumb ON the rest is
    // not on the stick, and the shift only does anything while the stick is
    // pushed, so the resting case resolves itself. What the window WOULD have
    // done is make a correct, deliberate use fail -- touch, hesitate, push, and
    // find the mode gone -- which is far worse than the case it guarded.
    //
    // `dpad.dwell_ms` remains, defaulting to 0 (no delay at all). It is there
    // for the head gesture, where a hand sweeping PAST the head on its way
    // somewhere else is a real transient, and it can be raised without touching
    // code if that ever shows up. At 0 it is exactly hold-to-shift.
    const std::uint64_t dwell = static_cast<std::uint64_t>(g_dwellMs.load(std::memory_order_relaxed));
    const bool engaged = modifierHeld && (now - g_modifierSinceTick) >= dwell;

    if (engaged && !g_engagedLatched) {
        ++g_engagements;
        // A short pulse, so the mode announces itself without a HUD element and
        // without the wearer reading a log.
        TriggerHaptic(0, 0.35f);
        char line[320]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] DPAD shift ON (#%llu): the left stick is a d-pad while the modifier is held, "
            "and movement is suppressed for exactly that long. Source: %s.\n",
            static_cast<unsigned long long>(g_engagements),
            g_headGestureEverSeen && !g_thumbrestEverSeen ? "held beside the head"
            : g_thumbrestEverSeen && !g_headGestureEverSeen ? "thumbrest"
                                                            : "either (both have been seen)");
        Tf2VrLog(line);
    } else if (!engaged && g_engagedLatched) {
        Tf2VrLog("[TF2VR] DPAD shift OFF: the left stick is movement again.\n");
    }
    g_engagedLatched = engaged;

    std::uint16_t buttons = 0;
    if (engaged) {
        // ONE DIRECTION AT A TIME, by dominance. The game's prompts are
        // single-choice, and a diagonal that answered two of them at once would
        // be a worse control than none.
        if (std::fabs(ly) >= std::fabs(lx)) {
            if (ly >= deadzone) buttons = kDpadUp;
            else if (ly <= -deadzone) buttons = kDpadDown;
        } else {
            if (lx >= deadzone) buttons = kDpadRight;
            else if (lx <= -deadzone) buttons = kDpadLeft;
        }
        if (buttons) ++g_directionsEmitted;
    }
    g_buttons.store(buttons, std::memory_order_release);
    g_engaged.store(engaged, std::memory_order_release);

    // THE VIEW BUTTON: the modifier held, plus the left primary (X).
    const bool view = engaged && (g_controllerButtons[0] & kControllerPrimaryButton) != 0;
    static bool viewWasDown = false;
    if (view && !viewWasDown) {
        ++g_viewPresses;
        TriggerHaptic(0, 0.25f);
    }
    viewWasDown = view;
    g_viewButton.store(view, std::memory_order_release);

    // The status line, on its own clock. It prints whether or not anything
    // fired, because "the gesture does nothing" and "the gesture is not built
    // into this dll" read identically in a silent log.
    static std::uint64_t lastReport = 0;
    if (now - lastReport < 10000) return;
    lastReport = now;
    if (g_source.load(std::memory_order_relaxed) == 0) return;
    char line[900]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] DPAD status: source=%d | frames %llu, modifier held on %llu | shifts %llu, "
        "directions %llu, view presses %llu | thumbrest seen=%d, head gesture seen=%d | head test: "
        "evaluated %llu, skipped no-grip-pose %llu, skipped no-head-pose %llu. A zero in BOTH "
        "'seen' columns means no modifier has ever been recognised -- that is the half to fix, not "
        "the stick. If 'evaluated' is ALSO zero the head test never ran at all, and the skip "
        "counter that is non-zero says why; the geometry is then not the thing to change.\n",
        g_source.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(g_frames),
        static_cast<unsigned long long>(g_modifierHeldFrames),
        static_cast<unsigned long long>(g_engagements),
        static_cast<unsigned long long>(g_directionsEmitted),
        static_cast<unsigned long long>(g_viewPresses),
        g_thumbrestEverSeen ? 1 : 0, g_headGestureEverSeen ? 1 : 0,
        static_cast<unsigned long long>(g_headEvaluated),
        static_cast<unsigned long long>(g_headNoGripPose),
        static_cast<unsigned long long>(g_headNoHeadPose));
    Tf2VrLog(line);
}

bool DpadGestureEngaged() { return g_engaged.load(std::memory_order_acquire); }
std::uint16_t DpadGestureButtons() { return g_buttons.load(std::memory_order_acquire); }
bool DpadGestureViewButton() { return g_viewButton.load(std::memory_order_acquire); }

void SetDpadModifierSource(int source) {
    if (source < 0) source = 0;
    if (source > 3) source = 3;
    g_source.store(source, std::memory_order_release);
    static const char* const kNames[] = {
        "OFF -- the left stick is movement only, and the d-pad is unreachable",
        "THUMBREST only (controllers that have one; the PFD MR controllers have no such pad, so this is "
        "inert there)",
        "HELD BESIDE THE HEAD only (works on every headset)",
        "EITHER the thumbrest or held beside the head (on hardware without a thumbrest this "
        "behaves exactly as 2)",
    };
    char line[420]{};
    std::snprintf(line, sizeof(line), "[TF2VR] dpad.modifier = %d: %s.\n", source, kNames[source]);
    Tf2VrLog(line);
}

int DpadModifierSource() { return g_source.load(std::memory_order_acquire); }

void SetDpadHeadGestureUp(float metres) { g_headUp.store(metres, std::memory_order_release); }
void SetDpadHeadGestureRadius(float metres) {
    if (metres > 0.05f) g_headRadius.store(metres, std::memory_order_release);
}
void SetDpadDwellMs(int ms) {
    if (ms < 0) ms = 0;
    g_dwellMs.store(ms, std::memory_order_release);
}
void SetDpadDeadzone(float deadzone) {
    if (deadzone > 0.05f && deadzone < 1.0f) g_deadzone.store(deadzone, std::memory_order_release);
}
