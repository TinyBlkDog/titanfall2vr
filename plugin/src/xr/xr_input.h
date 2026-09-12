#pragma once

#include <openxr/openxr.h>

#include <cmath>
#include <cstdint>

// TASK 05 STEP 1 -- CONTROLLER INPUT, IN ISOLATION.
//
// OpenXR action sets, the grip and aim poses of both hands, and the thumbstick /
// trigger state, read and PUBLISHED and nothing else. Deliberately no engine
// interaction of any kind: no camera write, no viewmodel transform, no cvar, no
// hook. That makes this step testable against the log alone, which is the only
// way to separate "the controllers are not being read" from "the controllers are
// read and the engine ignores what we do with them" -- and the whole of Task 05
// downstream depends on knowing which of those is true.
//
// It is also what Task 04 (the in-VR config menu) is blocked on: the menu needs
// the right hand's AIM pose for its pointing ray and the two thumbstick clicks
// for its L3+R3 summon chord. Both are read here, so the menu has its input the
// moment this lands.

// ---------------------------------------------------------------------------
// Published state. Index 0 is the LEFT hand, 1 the RIGHT.
//
// Same publish/consume discipline as the head pose, and for the same reason
// (handoff section 3): these are written on the XR frame clock inside Present
// and will be consumed on the render frame, which does not tick with it. Nine
// floats cannot be written atomically, and a torn basis is not a slightly wrong
// rotation, it is not a rotation at all -- so each hand's block is written
// between two bumps of its generation counter, odd while the write is in
// progress, and a reader that sees an odd value or a changed value retries.
// Compose values where they are CONSUMED, never here.
// ---------------------------------------------------------------------------

constexpr int kHandLeft = 0;
constexpr int kHandRight = 1;
constexpr int kHandCount = 2;

// Orientation as a BASIS in Source axes -- rows forward, right, up -- not as
// Euler angles. Every Euler round trip in this chain has been a source of
// error; the consumer needs an orientation to compose with, not three numbers
// to add. Positions stay in METRES on OpenXR axes, matching g_headPositionMetres,
// so the metres-to-units scale is applied once, at the consumer.
extern "C" volatile float g_controllerAimBasis[kHandCount][9];
extern "C" volatile float g_controllerAimPositionMetres[kHandCount][3];
extern "C" volatile float g_controllerGripBasis[kHandCount][9];
extern "C" volatile float g_controllerGripPositionMetres[kHandCount][3];
// Odd while that hand's block is being written; even when it is complete.
extern "C" volatile std::uint32_t g_controllerGeneration[kHandCount];
// Bit 0: the aim pose is active and tracked. Bit 1: the grip pose is.
extern "C" volatile std::uint8_t g_controllerPoseFlags[kHandCount];
constexpr std::uint8_t kControllerAimTracked = 1u;
constexpr std::uint8_t kControllerGripTracked = 2u;
// Thumbstick click is what Task 04's L3+R3 chord is made of.
extern "C" volatile std::uint8_t g_controllerThumbstickClick[kHandCount];
extern "C" volatile float g_controllerThumbstick[kHandCount][2];
extern "C" volatile float g_controllerTrigger[kHandCount];
// The grip squeeze, analogue. Left = ordnance, right = tactical; the digital
// threshold belongs to the consumer.
extern "C" volatile float g_controllerSqueeze[kHandCount];
// The MENU button, LEFT controller only: the right one's equivalent belongs to
// the Oculus dash and is never delivered to an application.
extern "C" volatile std::uint8_t g_controllerMenu;
// The capacitive thumbrest, per hand, 1 while a thumb rests on it. Part of the
// core Touch profile; hardware without the pad leaves the action inactive and
// this stays 0, which is what makes the d-pad's thumbrest modifier degrade to
// "never fires" rather than to a fault.
extern "C" volatile std::uint8_t g_controllerThumbrestTouch[kHandCount];
// Face buttons, by POSITION not name: bit 0 = primary (X on the left hand, A on
// the right), bit 1 = secondary (Y / B). Reload is the right hand's secondary.
constexpr std::uint8_t kControllerPrimaryButton = 1u;
constexpr std::uint8_t kControllerSecondaryButton = 2u;
extern "C" volatile std::uint8_t g_controllerButtons[kHandCount];
// Bumped once per successful sync, so a consumer can tell a live sample from a
// frozen one -- the same guard that stops head tracking pinning the view to the
// last pose a dead runtime delivered.
extern "C" volatile std::uint64_t g_controllerSequence;

// ---------------------------------------------------------------------------
// Lifecycle. Every one of these is non-fatal on failure: it logs and returns
// false, and the caller carries on. Head tracking, stereo and the projection
// layer are working and verified in the headset, and reading a controller must
// not be able to take them down.
// ---------------------------------------------------------------------------

// Instance-scoped: the action set, the actions, and the suggested bindings.
// Must be called before the session's action sets are attached.
bool XrInputCreateActions(XrInstance instance);
// Session-scoped: the four action spaces, then xrAttachSessionActionSets. Must
// be called after xrCreateSession and before the first sync. A session's action
// sets can only be attached once, which is why this is separate.
bool XrInputAttachToSession(XrSession session);
// Once per frame, after xrBeginFrame. baseSpace is the LOCAL reference space
// the poses are reported in; viewSpace is the head, used only to verify
// handedness against something rather than assume it.
void XrInputSync(XrSession session, XrSpace baseSpace, XrSpace viewSpace, XrTime displayTime);
// Releases the spaces, actions and action set. Call before the session and
// instance that own them are destroyed.
void XrInputShutdown();

void SetControllerInputEnabled(bool enabled);
bool IsControllerInputEnabled();

// ---------------------------------------------------------------------------
// OpenXR-to-Source axis conversion, in one place.
//
// OpenXR is +X right, +Y up, -Z forward. Source is +X forward, +Y left, +Z up.
// Both are right-handed and this mapping has determinant +1, so no handedness
// flip is introduced. The same mapping is currently also written out inline in
// xr_context.cpp's UpdateLookInjection for the head; that copy is the proven
// one and is deliberately left alone here, but the two must stay in step and
// the head path should adopt these when it is next touched.
// ---------------------------------------------------------------------------

inline void XrRotateVector(const XrQuaternionf& q, float x, float y, float z, float out[3]) {
    // v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
    const float tx = 2.0f * (q.y * z - q.z * y);
    const float ty = 2.0f * (q.z * x - q.x * z);
    const float tz = 2.0f * (q.x * y - q.y * x);
    out[0] = x + q.w * tx + (q.y * tz - q.z * ty);
    out[1] = y + q.w * ty + (q.z * tx - q.x * tz);
    out[2] = z + q.w * tz + (q.x * ty - q.y * tx);
}

inline void XrVectorToSource(const float v[3], float out[3]) {
    out[0] = -v[2];
    out[1] = -v[0];
    out[2] = v[1];
}

// Fills a Source-axis basis, rows forward / right / up, matching the layout the
// engine's own AngleVectors produces and the layout g_headBasis already uses.
inline void XrQuaternionToSourceBasis(const XrQuaternionf& q, float basis[9]) {
    float xrForward[3]{}, xrRight[3]{}, xrUp[3]{};
    XrRotateVector(q, 0.0f, 0.0f, -1.0f, xrForward);   // OpenXR looks down -Z
    XrRotateVector(q, 1.0f, 0.0f, 0.0f, xrRight);
    XrRotateVector(q, 0.0f, 1.0f, 0.0f, xrUp);
    float forward[3]{}, right[3]{}, up[3]{};
    XrVectorToSource(xrForward, forward);
    XrVectorToSource(xrRight, right);
    XrVectorToSource(xrUp, up);
    basis[0] = forward[0]; basis[1] = forward[1]; basis[2] = forward[2];
    basis[3] = right[0];   basis[4] = right[1];   basis[5] = right[2];
    basis[6] = up[0];      basis[7] = up[1];      basis[8] = up[2];
}

// Yaw and pitch in DEGREES from a Source-axis forward vector, by the same
// decomposition Source's own MatrixAngles uses. For logging and for relative
// measurements only -- nothing composes with these.
inline void SourceForwardToYawPitchDegrees(const float forward[3], float& yawDegrees, float& pitchDegrees) {
    constexpr float toDegrees = 57.2957795f;
    const float xyDistance = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1]);
    pitchDegrees = std::atan2(-forward[2], xyDistance) * toDegrees;
    yawDegrees = xyDistance > 0.001f ? std::atan2(forward[1], forward[0]) * toDegrees : 0.0f;
}

inline float WrapDegrees(float value) {
    while (value > 180.0f) value -= 360.0f;
    while (value < -180.0f) value += 360.0f;
    return value;
}

// Sends a haptic pulse to one controller. hand 0 = left, 1 = right; amplitude
// 0..1, where 0 stops any pulse in progress.
//
// The game's own rumble is the source: it calls XInputSetState, xinput_pad
// intercepts that for the synthetic pad, and this is the other end. Each call is
// the runtime's shortest pulse, because the game re-sends its motor state
// continuously and a long pulse would queue behind itself.
void TriggerHaptic(int hand, float amplitude, float frequency = XR_FREQUENCY_UNSPECIFIED,
                   XrDuration duration = 0);
