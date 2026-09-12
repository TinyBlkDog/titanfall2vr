#include "hand_pose.h"

#include "camera_hook.h"
#include "camera_update_hook.h"
#include "engine_cvars.h"
#include "diagnostics.h"
#include "config.h"
#include "vr_input.h"
#include "xr_input.h"
#include "ads_probe.h"
#include "titan_state.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Written by the camera interceptor on the RENDER frame, unconditionally --
// the arming check in that path gates only the write-back, not the capture --
// so these are live even with the VR stack switched off, which is what makes
// the composition checkable flat.
extern "C" volatile std::uint32_t g_cameraBaseYawBits;
extern "C" volatile std::uint32_t g_cameraBasePosXBits;
extern "C" volatile std::uint32_t g_cameraBasePosYBits;
extern "C" volatile std::uint32_t g_cameraBasePosZBits;
extern "C" volatile float g_headPositionMetres[3];
extern "C" volatile std::uint32_t g_cameraAppliedYawBits;
extern "C" volatile std::uint32_t g_cameraDesiredPosXBits;
extern "C" volatile std::uint32_t g_cameraDesiredPosYBits;
extern "C" volatile std::uint32_t g_cameraDesiredPosZBits;
extern "C" volatile std::uint8_t g_cameraPositionWriteActive;
extern "C" volatile float g_headYawDegrees;

float SourceUnitsPerMetre();

namespace {

// THE LIVE WEAPON BASIS, CAPTURED EVERY FRAME.
//
// NudgePivotOffset has to take R*delta back out of the position, and R is the
// orientation AT THE MOMENT OF THE PRESS -- not a stored angle, not a rest
// pose. The composition below already builds that basis, so it is captured
// there rather than rebuilt from angles at the keypress, where a second copy of
// the convention could disagree with the first.
//
// Plain floats, no atomics: the composition and the key polling both run on the
// plugin frame, on one thread. A torn read here would misplace the gun by a
// quarter of an inch, not crash anything.
float g_basisFwd[3] = {1.0f, 0.0f, 0.0f};
float g_basisRight[3] = {0.0f, 1.0f, 0.0f};
float g_basisUp[3] = {0.0f, 0.0f, 1.0f};
float g_basisBodyYaw = 0.0f;
bool g_basisValid = false;

void StoreWeaponBasis(float pitch, float yaw, float roll, float bodyYaw) {
    constexpr float kD2R = 3.14159265358979323846f / 180.0f;
    const float sp = std::sin(pitch * kD2R), cp = std::cos(pitch * kD2R);
    const float sy = std::sin(yaw * kD2R), cy = std::cos(yaw * kD2R);
    const float sr = std::sin(roll * kD2R), cr = std::cos(roll * kD2R);
    g_basisFwd[0] = cp * cy;  g_basisFwd[1] = cp * sy;  g_basisFwd[2] = -sp;
    g_basisRight[0] = -sr * sp * cy + cr * sy;
    g_basisRight[1] = -sr * sp * sy - cr * cy;
    g_basisRight[2] = -sr * cp;
    g_basisUp[0] = cr * sp * cy + sr * sy;
    g_basisUp[1] = cr * sp * sy - sr * cy;
    g_basisUp[2] = cr * cp;
    g_basisBodyYaw = bodyYaw;
    g_basisValid = true;
}

HandPoseSource g_source = HandPoseSource::Off;
std::uint64_t g_lastLogTick = 0;
float g_lastYawComp = 1.0f;

float BitsToFloat(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kRadToDeg = 180.0f / kPi;

struct Pose {
    float x, y, z;
    float pitch, yaw, roll;
};

// ---------------------------------------------------------------------------
// C4 -- THE BRACE. A GAIN CHANGE, AND NEVER A FILTER.
//
//     on ADS enter:   anchor = the hand where it is, captured once
//     while held:     aim = anchor + (hand - anchor) * gain
//     on release:     gain returns to 1.0
//
// The response is INSTANTANEOUS: same frame, no history, no smoothing window,
// nothing that arrives late. It is simply smaller. That is the whole reason
// this is compatible with the standing rule against latency on an input path --
// the input is never delayed, only scaled -- and it is why a low-pass filter is
// the wrong mechanism rather than a wrong constant. Tremor damping comes free
// by the same factor, so there is nothing left for a filter to do either.
//
// If the wearer reports this as LAGGY rather than STEADY, that is a defect
// report and not a tuning request: there is no filter in here, so lag means
// something upstream is filtering or the brace is on the wrong clock. Read the
// ratio line below; do not reach for the gain.
//
// THE DIVERGENCE PROBLEM, AND THE SHAPE BOTH REFERENCE MODS USED. Scaling a
// delta makes hand and gun drift apart. BioShock freezes its drive's reference
// and re-adopts past a threshold; Halo parks inside restEnterRadians and takes
// authority again past restExitRadians. Same shape here, with two thresholds
// doing real work:
//
//   dev <= cone                 pure fine-tune. The ratio is EXACTLY the gain.
//   cone < dev < readopt        the gain ramps to 1.0, so the brace releases
//                               under the hand instead of hitting a wall.
//   dev >= readopt              the anchor is dragged so dev stays at readopt.
//                               Further hand movement moves the gun ONE FOR
//                               ONE, which is the flat game's "ADS makes
//                               turning slow, not impossible".
//
// At dev = readopt the ramp has reached 1.0, so the gun sits exactly on the
// hand and the divergence is ZERO -- push out past the cone and there is no
// snap on release at all. Inside the cone the residual is bounded by
// cone * (1 - gain), and that bound is printed in the log rather than claimed.
//
// PITCH AND BODY-FRAME YAW, NOT WORLD YAW. The anchor is held relative to the
// body's own yaw, so turning with the stick passes through the brace untouched
// and moves gun and body together. Muting the STICK in ADS is the flat game's
// other cost and it belongs to PLAN-TURNING -- it is a different control, and
// the rule that these two never share a run is exactly why it is not folded in
// here. ROLL IS NOT BRACED: rolling the wrist does not move where the round
// goes, so scaling it would fight the hand for nothing.
constexpr float kBraceOffGain = 1.0f;
std::atomic<float> g_braceGain = kBraceOffGain;
std::atomic<float> g_braceConeDeg = 4.0f;
std::atomic<float> g_braceReadoptDeg = 12.0f;
std::atomic_bool g_braceForce = false;
// See AddAdsYawCompensation in the header.
std::atomic<float> g_adsYawCompensation{0.0f};
// Named apart from WrapDegrees because xr_input.h declares one too and the two
// are ambiguous at namespace scope. Same arithmetic, unambiguous name.
float WrapAdsDegrees(float degrees) {
    while (degrees > 180.0f) degrees -= 360.0f;
    while (degrees < -180.0f) degrees += 360.0f;
    return degrees;
}

float WrapDegrees(float degrees) {
    while (degrees > 180.0f) degrees -= 360.0f;
    while (degrees < -180.0f) degrees += 360.0f;
    return degrees;
}

// Every one of the plan's falsifiers, as counts and ratios rather than as
// claims. Reset each report window.
struct BraceWindow {
    std::uint64_t samples = 0;
    std::uint64_t inConeSamples = 0;
    std::uint64_t pastConeSamples = 0;
    std::uint64_t stillSamples = 0;
    std::uint64_t lagSamples = 0;
    double handSum = 0.0;
    double gunSum = 0.0;
    double inConeHandSum = 0.0;
    double inConeGunSum = 0.0;
    double pastConeHandSum = 0.0;
    double pastConeGunSum = 0.0;
    float handMax = 0.0f;
    float gunMax = 0.0f;
    float stillGunMax = 0.0f;
    float devMax = 0.0f;
    float divergenceMax = 0.0f;
    float anchorDriftWhileStill = 0.0f;
};
BraceWindow g_braceWindow;
bool g_braceEngaged = false;
float g_braceAnchorPitch = 0.0f;
float g_braceAnchorYawBody = 0.0f;
float g_bracePrevHandPitch = 0.0f;
float g_bracePrevHandYawBody = 0.0f;
float g_bracePrevOutPitch = 0.0f;
float g_bracePrevOutYawBody = 0.0f;
bool g_bracePrevValid = false;
std::uint64_t g_braceEngagedAtMs = 0;
std::uint64_t g_braceNextReportMs = 0;

void ReportBrace(bool finalLine) {
    const BraceWindow w = g_braceWindow;
    g_braceWindow = BraceWindow{};
    if (w.samples == 0 && !finalLine) return;
    const double overall = w.handSum > 1e-6 ? w.gunSum / w.handSum : 0.0;
    const double inCone = w.inConeHandSum > 1e-6 ? w.inConeGunSum / w.inConeHandSum : 0.0;
    const double pastCone = w.pastConeHandSum > 1e-6 ? w.pastConeGunSum / w.pastConeHandSum : 0.0;
    const float gain = g_braceGain.load(std::memory_order_relaxed);
    const float cone = g_braceConeDeg.load(std::memory_order_relaxed);
    char line[900]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] ADS BRACE %s n=%llu gain %.2f cone %.1f readopt %.1f | hand moved %.2f deg "
        "(max %.3f/frame) gun moved %.2f deg (max %.3f/frame) | RATIO overall %.3f, IN CONE %.3f "
        "over %llu frames [must equal the gain], PAST CONE %.3f over %llu frames [must return to "
        "~1.0] | STILL frames %llu, largest gun move on one %.4f deg [must be 0.0000], anchor "
        "drift while still %.4f deg [must be 0.0000] | LAG frames %llu [must be 0: a hand move "
        "with no gun move in the same frame is a filter, and there is no filter in here] | dev max "
        "%.2f deg, divergence max %.2f deg (bound cone*(1-gain) = %.2f) | held %.1f s\n",
        finalLine ? "RELEASED" : "held", static_cast<unsigned long long>(w.samples),
        static_cast<double>(gain), static_cast<double>(cone),
        static_cast<double>(g_braceReadoptDeg.load(std::memory_order_relaxed)),
        w.handSum, static_cast<double>(w.handMax), w.gunSum, static_cast<double>(w.gunMax),
        overall, inCone, static_cast<unsigned long long>(w.inConeSamples), pastCone,
        static_cast<unsigned long long>(w.pastConeSamples),
        static_cast<unsigned long long>(w.stillSamples), static_cast<double>(w.stillGunMax),
        static_cast<double>(w.anchorDriftWhileStill),
        static_cast<unsigned long long>(w.lagSamples), static_cast<double>(w.devMax),
        static_cast<double>(w.divergenceMax), static_cast<double>(cone * (1.0f - gain)),
        g_braceEngagedAtMs ? (GetTickCount64() - g_braceEngagedAtMs) / 1000.0 : 0.0);
    Tf2VrLog(line);
}

void ApplyAdsBrace(Pose& pose, float bodyYaw) {
    const float gain = g_braceGain.load(std::memory_order_relaxed);
    const bool wanted = IsAdsHeld() || g_braceForce.load(std::memory_order_relaxed);
    // A gain of 1.0 IS "no brace" arithmetically, so it needs no second flag and
    // there is no state in which a flag and a value can disagree.
    const bool engaged = wanted && gain < kBraceOffGain;

    const float handPitch = pose.pitch;
    const float handYawBody = WrapDegrees(pose.yaw - bodyYaw);

    if (!engaged) {
        if (g_braceEngaged) {
            g_braceEngaged = false;
            ReportBrace(true);
        }
        g_bracePrevValid = false;
        return;
    }

    const std::uint64_t now = GetTickCount64();
    if (!g_braceEngaged) {
        // ENTER. The anchor is the hand exactly where it is, so dev is 0 and the
        // output equals the input on this frame -- ADS never begins with a jump.
        g_braceEngaged = true;
        g_braceAnchorPitch = handPitch;
        g_braceAnchorYawBody = handYawBody;
        g_braceWindow = BraceWindow{};
        g_bracePrevValid = false;
        g_braceEngagedAtMs = now;
        g_braceNextReportMs = now + 1000;
        char line[300]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] ADS BRACE engaged: anchor captured at pitch %.2f, body-frame yaw %.2f. Gain "
            "%.2f inside a %.1f deg cone, re-adopting at %.1f deg.\n",
            static_cast<double>(handPitch), static_cast<double>(handYawBody),
            static_cast<double>(gain), static_cast<double>(g_braceConeDeg.load(std::memory_order_relaxed)),
            static_cast<double>(g_braceReadoptDeg.load(std::memory_order_relaxed)));
        Tf2VrLog(line);
    }

    float cone = g_braceConeDeg.load(std::memory_order_relaxed);
    float readopt = g_braceReadoptDeg.load(std::memory_order_relaxed);
    if (cone < 0.1f) cone = 0.1f;
    // READOPT MUST EXCEED THE CONE. If it does not there is no ramp between
    // them, the effective gain steps from `gain` to 1.0 at a single value, and
    // the hand sitting on that value alternates the two every frame -- which is
    // exactly the chatter the second threshold exists to prevent.
    if (readopt <= cone) readopt = cone * 1.5f;

    float deltaPitch = WrapDegrees(handPitch - g_braceAnchorPitch);
    float deltaYaw = WrapDegrees(handYawBody - g_braceAnchorYawBody);
    // Yaw degrees subtend less angle the further from level you are, so the cone
    // is a real cone rather than a rectangle in the angle pair.
    const float cosPitch = std::cos(g_braceAnchorPitch * 0.01745329252f);
    const float yawArc = deltaYaw * cosPitch;
    float dev = std::sqrt(deltaPitch * deltaPitch + yawArc * yawArc);

    const float anchorPitchBefore = g_braceAnchorPitch;
    const float anchorYawBefore = g_braceAnchorYawBody;

    if (dev > readopt) {
        // DRAG, not a jump. The anchor moves only by the excess, so dev lands
        // exactly on readopt and every further degree of hand movement carries
        // the anchor with it one for one.
        const float keep = readopt / dev;
        g_braceAnchorPitch = WrapDegrees(g_braceAnchorPitch + deltaPitch * (1.0f - keep));
        g_braceAnchorYawBody = WrapDegrees(g_braceAnchorYawBody + deltaYaw * (1.0f - keep));
        deltaPitch *= keep;
        deltaYaw *= keep;
        dev = readopt;
    }

    float effective = gain;
    if (dev > cone) {
        const float t = (dev - cone) / (readopt - cone);
        effective = gain + (1.0f - gain) * (t > 1.0f ? 1.0f : t);
    }

    const float outPitch = WrapDegrees(g_braceAnchorPitch + deltaPitch * effective);
    const float outYawBody = WrapDegrees(g_braceAnchorYawBody + deltaYaw * effective);

    // ---- the falsifiers, measured on the way past ----
    if (g_bracePrevValid) {
        const float handStepP = WrapDegrees(handPitch - g_bracePrevHandPitch);
        const float handStepY = WrapDegrees(handYawBody - g_bracePrevHandYawBody) * cosPitch;
        const float gunStepP = WrapDegrees(outPitch - g_bracePrevOutPitch);
        const float gunStepY = WrapDegrees(outYawBody - g_bracePrevOutYawBody) * cosPitch;
        const float handStep = std::sqrt(handStepP * handStepP + handStepY * handStepY);
        const float gunStep = std::sqrt(gunStepP * gunStepP + gunStepY * gunStepY);
        BraceWindow& w = g_braceWindow;
        ++w.samples;
        w.handSum += handStep;
        w.gunSum += gunStep;
        if (handStep > w.handMax) w.handMax = handStep;
        if (gunStep > w.gunMax) w.gunMax = gunStep;
        // "Still" is the hand not having moved measurably this frame. The gun
        // must then not move AT ALL, and the anchor must not creep.
        constexpr float kStill = 0.0005f;
        if (handStep <= kStill) {
            ++w.stillSamples;
            if (gunStep > w.stillGunMax) w.stillGunMax = gunStep;
            const float driftP = WrapDegrees(g_braceAnchorPitch - anchorPitchBefore);
            const float driftY = WrapDegrees(g_braceAnchorYawBody - anchorYawBefore);
            const float drift = std::sqrt(driftP * driftP + driftY * driftY);
            if (drift > w.anchorDriftWhileStill) w.anchorDriftWhileStill = drift;
        } else {
            // A hand step with NO gun step in the same frame is what a filter
            // looks like from outside. There is no filter here, so this must
            // stay at zero, and it is counted rather than asserted.
            if (gunStep <= 0.0f) ++w.lagSamples;
            if (dev <= cone) {
                ++w.inConeSamples;
                w.inConeHandSum += handStep;
                w.inConeGunSum += gunStep;
            } else {
                ++w.pastConeSamples;
                w.pastConeHandSum += handStep;
                w.pastConeGunSum += gunStep;
            }
        }
        if (dev > w.devMax) w.devMax = dev;
        const float divergence = dev * (1.0f - effective);
        if (divergence > w.divergenceMax) w.divergenceMax = divergence;
    }
    g_bracePrevHandPitch = handPitch;
    g_bracePrevHandYawBody = handYawBody;
    g_bracePrevOutPitch = outPitch;
    g_bracePrevOutYawBody = outYawBody;
    g_bracePrevValid = true;

    pose.pitch = outPitch;
    pose.yaw = WrapDegrees(outYawBody + bodyYaw);
    // Roll is deliberately left as it came in.

    if (now >= g_braceNextReportMs) {
        g_braceNextReportMs = now + 1000;
        ReportBrace(false);
    }
}

// A basis whose rows are forward/right/up in Source axes, turned into the
// pitch/yaw/roll Source itself uses. Standard MatrixAngles, with left = -right.
void BasisToAngles(const float basis[9], float& pitch, float& yaw, float& roll) {
    const float fx = basis[0], fy = basis[1], fz = basis[2];
    const float rx = basis[3], ry = basis[4], rz = basis[5];
    const float ux = basis[6], uy = basis[7], uz = basis[8];
    (void)rx;
    (void)ry;
    (void)ux;
    (void)uy;

    const float horizontal = std::sqrt(fx * fx + fy * fy);
    if (horizontal > 0.001f) {
        yaw = std::atan2(fy, fx) * kRadToDeg;
        pitch = std::atan2(-fz, horizontal) * kRadToDeg;
        roll = std::atan2(-rz, uz) * kRadToDeg;
    } else {
        // Looking straight up or down: yaw and roll are the same rotation and
        // atan2 of two near-zeros is noise. Pin roll and keep yaw.
        yaw = std::atan2(-basis[4], basis[3]) * kRadToDeg;
        pitch = std::atan2(-fz, horizontal) * kRadToDeg;
        roll = 0.0f;
    }
}

// Position from the GRIP pose, orientation from the AIM pose, both read through
// the one seqlock that guards the whole hand block.
//
// Both are taken inside a single generation check rather than in two passes, so
// the position and the orientation are guaranteed to be from the same sample.
// Reading them separately could pair a position from one frame with an
// orientation from the next, which during fast motion is a pose that never
// existed.
bool TryReadRightPose(float positionMetres[3], float basis[9]) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint32_t before = g_controllerGeneration[kHandRight];
        if (before & 1u) continue;  // a write is in progress
        const std::uint8_t flags = g_controllerPoseFlags[kHandRight];
        if ((flags & kControllerGripTracked) == 0) return false;
        for (int i = 0; i < 3; ++i) {
            positionMetres[i] = g_controllerGripPositionMetres[kHandRight][i];
        }
        // Fall back to the grip orientation if the aim pose is not tracked --
        // wrong by the grip-to-barrel angle, but present, which beats the gun
        // freezing at its last orientation.
        const bool aim = (flags & kControllerAimTracked) != 0;
        for (int i = 0; i < 9; ++i) {
            basis[i] = aim ? g_controllerAimBasis[kHandRight][i]
                           : g_controllerGripBasis[kHandRight][i];
        }
        if (g_controllerGeneration[kHandRight] == before) return true;
    }
    return false;
}

// room-space metres (OpenXR axes) -> Source units (forward, left, up).
//
// The mapping is camera_hook.cpp:725-727's, unchanged: Source forward is -xrZ,
// Source left is -xrX, Source up is +xrY.
void RoomOffsetToSource(const float handMetres[3], const float headMetres[3], float& forward,
                        float& left, float& up) {
    const float unitsPerMetre = SourceUnitsPerMetre();
    forward = -(handMetres[2] - headMetres[2]) * unitsPerMetre;
    left = -(handMetres[0] - headMetres[0]) * unitsPerMetre;
    up = (handMetres[1] - headMetres[1]) * unitsPerMetre;
}

// Places a local (forward, left, up) offset into the world at the camera.
//
// THE ROTATION IS A CONSTANT, AND THE LOG IS WHY.
//
// A yaw-compensation term was added on the theory that physical turning was
// being counted twice -- once in the room-space hand position, once in the
// game's yaw. The log killed that theory outright. Across an entire run the
// game's own yaw never moved:
//
//   game -28.0  head   2.6  applied -25.3
//   game -28.0  head -37.0  applied -61.2
//   game -28.0  head  34.6  applied   1.3
//
// It sat at -28.0 while the head swung 55 degrees either way, and applied is
// simply game + head. Turning physically does not rotate the player in-game at
// all; the whole rotation lives in head tracking. So the room-to-world rotation
// is CONSTANT, which is k = 0, and the reported behaviour of the other two
// values follows: k = 1 subtracts a rotation that was never added (the gun
// lagged), k = -1 adds it twice (the gun ran far ahead).
//
// The term is kept, defaulted to 0, because it is what produced the log that
// settled this and it costs nothing to leave tunable.

// THE ANCHOR THE GUN HANGS FROM: the origin the VIEW is built from.
//
// This used to be camera_hook's g_cameraBasePos*, and on a step that is simply
// the wrong quantity. Measured across one +12.95 step:
//
//   our base    +12.95 -> 712.99, then +3.99, +0.73, +0.65 ...
//   view height  +2.60 -> 704.90
//
// The base leaps the whole step in a single frame while the view eases into it
// over several, and the two sit about eight units apart besides. A gun glued to
// the base therefore shoots ten units above where the player is looking and
// then settles -- the reported "single frame considerably high, much higher
// than the step".
//
// AND THE HEAD OFFSET DOES NOT DOUBLE-COUNT, which is why an earlier attempt
// avoided this and applied a correction term instead. RoomOffsetToSource
// computes the hand as hand MINUS head, so what is composed is the hand's
// position RELATIVE TO THE HEAD. The head offset was never in it, so anchoring
// on the rendered eye adds it exactly once, which is correct.
//
// Falls back to the base when the view origin is not available yet, so nothing
// is worse than it was before the first main-scene pass.
// THE VIEW'S LEVEL, THE BASE'S FRESHNESS.
//
// Anchoring straight to the view origin killed the step spike and brought the
// movement lag and stutter straight back, because the two references update at
// different rates: the base is latched on EVERY render pass, the view origin
// once a frame on the first main-scene pass. Pick either alone and you choose
// which of the two faults to have.
//
// They are only in conflict if the offset between them is treated as noise. It
// is not -- it is the head offset plus whatever easing the engine is part-way
// through, and camera_update_hook now samples it at the instant it captures the
// view origin, so it is one clean quantity rather than two clocks subtracted.
//
// Fresh base plus that standing offset is the view's position, updated as often
// as the base is.
//
// LOAD-BEARING. A 2026-08-19 handoff listed this and TryGetMainSceneAnchorOffset
// as "no longer load-bearing" dead code. That claim is WITHDRAWN: it predates
// 1.6-A, and the THIRD call below (in the consume path) IS that re-base. Three
// live callers -- pose build, seqlock base record, and that re-base.
// Do not delete on the authority of that list. See M0-PREP-HYGIENE-2026-08-24.md.
void CameraAnchor(float out[3]) {
    out[0] = BitsToFloat(g_cameraBasePosXBits);
    out[1] = BitsToFloat(g_cameraBasePosYBits);
    out[2] = BitsToFloat(g_cameraBasePosZBits);
    // THE PUBLISHED ORIGIN, NOT BASE PLUS OFFSET. 2026-09-11 01:00: `up` read a
    // steady -16 units while pose-minus-anchor swung -13..+8 between two calls
    // of this function inside one tick, with the published offset at -0.4. The
    // moving term was the base: the camera hook rewrites those bits on every
    // engine camera commit, and the first level runs three to six passes per
    // frame at origins tens of units apart (the training level runs one, and
    // the gun is right there). The nearest-to-base pass's origin is published
    // once per frame and cannot move between the compose and the record.
    // Bounded against the base as before, so a pass that is not ours cannot
    // drag the gun across the map; then the old path is the fallback.
    {
        float published[3]{};
        if (TryGetMainSceneAnchorOrigin(published)) {
            bool withinBound = true;
            for (int axis = 0; axis < 3; ++axis) {
                const float d = published[axis] - out[axis];
                if (d <= -128.0f || d >= 128.0f) withinBound = false;
            }
            if (withinBound) {
                out[0] = published[0];
                out[1] = published[1];
                out[2] = published[2];
                return;
            }
        }
    }
    float offset[3]{};
    if (!TryGetMainSceneAnchorOffset(offset)) return;
    // Bounded for the same reason the old correction was: the 3D skybox pass
    // runs its own scaled camera, and a pass that is not ours to follow must not
    // drag the gun across the map.
    for (int axis = 0; axis < 3; ++axis) {
        if (offset[axis] > -128.0f && offset[axis] < 128.0f) out[axis] += offset[axis];
    }
}
// hand.ref_yaw: subtract the head-yaw reference in the hand composition (see
// ComposeWorld). Default on; 0 is the pre-2026-09-04 behaviour for an A/B.
bool g_handRefYaw = true;

Pose ComposeWorld(float forward, float left, float up, float localPitch, float localYaw,
                  float localRoll) {
    // FROM THE INI, NOT THE CONVAR. Reading a convar was safe, but setting one
    // from the console crashes, so calibration had no usable path in a headset.
    //
    // The old fallback here was 1.0f whenever the convar read failed, which is
    // the DOUBLE-COUNTED rotation the measurement rejected -- so a session with
    // hand.cvars off silently composed the pose wrong and it looked like lag.
    // The INI default is 0, the measured value.
    float yawComp = 0.0f;
    ReadHandCalibration(nullptr, nullptr, nullptr, &yawComp);
    g_lastYawComp = yawComp;
    // THE SAME ROOM-TO-GAME MAPPING THE HEAD USES. 2026-09-04.
    //
    // The view is composed as base + (headRoomYaw - reference); this path
    // composed the hand as base + handRoomYaw, and rotated the hand's room
    // offset by base alone. So the gun sat off the view by exactly the
    // reference yaw, in angle AND in position -- invisible after a clean runtime
    // recentre (reference ~0), and wrong by whatever the head was pointing at
    // after every automatic spawn recentre or LEADER+TAB. Reported as aim that
    // drifts as you play and needs a headset reset to fix. Subtracting the
    // reference here makes the reference what it is for the view: the
    // definition of forward, and nothing the gun can feel. hand.ref_yaw = 0
    // is the old behaviour.
    float referenceYaw = 0.0f;
    if (g_handRefYaw && !TryGetHeadYawReference(&referenceYaw)) referenceYaw = 0.0f;
    if (!g_handRefYaw) referenceYaw = 0.0f;
    {
        static float lastLogged = 1e9f;
        if (std::fabs(referenceYaw - lastLogged) > 0.05f) {
            lastLogged = referenceYaw;
            char line[200]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] hand frame: reference yaw %.2f deg now subtracted from the hand's room yaw and "
                "position mapping (hand.ref_yaw = %d). The gun follows the hand whatever the reference is.\n",
                static_cast<double>(referenceYaw), g_handRefYaw ? 1 : 0);
            Tf2VrLog(line);
        }
    }
    const float bodyYaw = BitsToFloat(g_cameraBaseYawBits) - referenceYaw - yawComp * g_headYawDegrees;
    const float c = std::cos(bodyYaw * kDegToRad);
    const float s = std::sin(bodyYaw * kDegToRad);

    // Position calibration, in the PLAYER's frame and applied BEFORE the yaw
    // rotation -- so "forward" keeps meaning forward whichever way the player
    // faces, which is what makes it dialable by someone describing the error as
    // "too low" or "too far in front".
    float offForward = 0.0f, offLeft = 0.0f, offUp = 0.0f;
    ReadHandPositionOffset(&offForward, &offLeft, &offUp);
    forward += offForward;
    left += offLeft;
    up += offUp;

    float anchor[3]{};
    CameraAnchor(anchor);
    Pose pose{};
    pose.x = anchor[0] + forward * c - left * s;
    pose.y = anchor[1] + forward * s + left * c;
    pose.z = anchor[2] + up;

    // The orientation rotation is yaw-only, so composing it is one addition
    // rather than a matrix multiply followed by a second angle extraction.
    // The calibration offsets are added LAST, after the body-yaw composition, so
    // they mean "rotate the model relative to the hand" rather than "rotate the
    // hand relative to the world" -- which is what someone dialling them in
    // while wearing the headset will expect them to mean.
    float offPitch = 0.0f, offYaw = 0.0f, offRoll = 0.0f;
    ReadHandCalibration(&offPitch, &offYaw, &offRoll, nullptr);

    // Axis gates, applied to the HAND's contribution only -- the calibration
    // offsets still apply, so a gated axis can still be trimmed to a constant.
    float usePitch = 1.0f, useRoll = 1.0f;
    ReadHandAxisGates(&usePitch, &useRoll);
    pose.pitch = localPitch * usePitch + offPitch;
    pose.yaw = localYaw + bodyYaw + offYaw;
    pose.roll = localRoll * useRoll + offRoll;

    // THE GRIP OFFSET, ROTATED INTO THE WEAPON'S FRAME. This is the pivot fix.
    //
    // Applied AFTER the final angles are known, because the whole point is that
    // it rotates WITH the gun: the model origin then sits wherever it must for
    // the grip to stay on the hand, and a wrist rotation turns the weapon about
    // the hand instead of swinging it around a distant origin.
    //
    // Same basis convention as AngleBasis in camera_update_hook.cpp -- rows
    // forward, right, up -- reused rather than re-derived, so a sign error here
    // cannot disagree with a convention that is already right.
    float gripForward = 0.0f, gripRight = 0.0f, gripUp = 0.0f;
    ReadGripOffset(&gripForward, &gripRight, &gripUp);
    // THE PER-WEAPON TERM (ads_probe.h, rung 2). Position only, in the same
    // weapon frame as the grip, so it rotates with the gun exactly as the grip
    // does and nothing about the angles changes. Zero until the weapon in hand
    // has latched its R_HAND, and zero when F5 has it off.
    //
    // NEVER IN A TITAN (2026-09-06). The term is (grip reference - this
    // weapon's latched R_HAND), and the reference was dialled in on a PILOT
    // weapon. A titan weapon's R_HAND belongs to a different rig at a
    // different scale, so that subtraction is not a small correction but a
    // large bogus offset: the wearer found the titan's right-hand weapon
    // shifted directly into their face. The feature is right; it was simply
    // being applied to something it was never calibrated against.
    //
    // TitanStateSettled() rides along for the same reason it does in the mesh
    // filter and the bone collapse: IsInTitanNow() fails safe to "on foot", so
    // a save that LOADS inside a titan would apply the term for its first
    // frames. Not settled means no term, which is exactly the behaviour from
    // before this feature existed.
    float perWeapon[3]{};
    const bool perWeaponAllowed = TitanStateSettled() && !IsInTitanNow();
    if (perWeaponAllowed && TryGetPerWeaponGripDelta(perWeapon)) {
        gripForward += perWeapon[0];
        gripRight += perWeapon[1];
        gripUp += perWeapon[2];
    }
    // Say it once per transition, so a log can tell "suppressed in a titan"
    // apart from "the term was zero anyway".
    {
        static int lastAllowed = -1;
        const int now = perWeaponAllowed ? 1 : 0;
        if (now != lastAllowed) {
            lastAllowed = now;
            Tf2VrLog(now ? "[TF2VR] GRIP per-weapon term: applied again (on foot, titan state settled).\n"
                         : "[TF2VR] GRIP per-weapon term: SUPPRESSED -- in a titan (or the titan state is not "
                           "settled yet). The reference is a pilot-weapon constant and a titan weapon's R_HAND "
                           "is a different rig, so the delta would throw the weapon into the face.\n");
        }
    }
    if (gripForward != 0.0f || gripRight != 0.0f || gripUp != 0.0f) {
        const float sp = std::sin(pose.pitch * kDegToRad), cp = std::cos(pose.pitch * kDegToRad);
        const float sy = std::sin(pose.yaw * kDegToRad), cy = std::cos(pose.yaw * kDegToRad);
        const float sr = std::sin(pose.roll * kDegToRad), cr = std::cos(pose.roll * kDegToRad);
        const float fwd[3] = {cp * cy, cp * sy, -sp};
        const float right[3] = {-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp};
        const float upv[3] = {cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp};
        pose.x += fwd[0] * gripForward + right[0] * gripRight + upv[0] * gripUp;
        pose.y += fwd[1] * gripForward + right[1] * gripRight + upv[1] * gripUp;
        pose.z += fwd[2] * gripForward + right[2] * gripRight + upv[2] * gripUp;
    }
    StoreWeaponBasis(pose.pitch, pose.yaw, pose.roll, bodyYaw);
    return pose;
}

void LogOccasionally(const char* what, const Pose& pose) {
    const std::uint64_t now = GetTickCount64();
    if (now - g_lastLogTick < 2000) return;
    g_lastLogTick = now;
    char line[260]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR] hand pose (%s): world <%.1f, %.1f, %.1f> angles <%.1f, %.1f, %.1f>; "
                  "game yaw %.1f, head yaw %.1f, applied yaw %.1f, comp %.2f.\n",
                  what, static_cast<double>(pose.x), static_cast<double>(pose.y),
                  static_cast<double>(pose.z), static_cast<double>(pose.pitch),
                  static_cast<double>(pose.yaw), static_cast<double>(pose.roll),
                  static_cast<double>(BitsToFloat(g_cameraBaseYawBits)),
                  static_cast<double>(g_headYawDegrees),
                  static_cast<double>(BitsToFloat(g_cameraAppliedYawBits)),
                  static_cast<double>(g_lastYawComp));
    Tf2VrLog(line);
}

// hand.yaw_rebase. Armed by default; 0 restores the stale-yaw behaviour in one
// INI line, which is the revert path for the fix and its A/B.
bool g_yawRebase = true;

}  // namespace

void SetHandPoseSource(int source) {
    // 1 was SyntheticSpin, removed 2026-09-08 with the convar bridge it existed
    // to exercise. Refused out loud rather than silently reused, so an old ini
    // carrying `hand.source = 1` says so instead of quietly meaning something
    // else.
    if (source == 1) {
        Tf2VrLog("[TF2VR] hand.source = 1 (synthetic spin) was removed with the convar bridge; "
                 "falling back to 0 (off). Use 2 for the flat-checkable fixed offset, 3 for the "
                 "right controller.\n");
        source = 0;
    }
    if (source < 0 || source > 3) source = 0;
    g_source = static_cast<HandPoseSource>(source);
}

HandPoseSource CurrentHandPoseSource() { return g_source; }

void SetHandReferenceYaw(bool enabled) {
    g_handRefYaw = enabled;
    Tf2VrLog(enabled ? "[TF2VR] hand.ref_yaw = 1: the hand is composed against the head-yaw reference, as the view is.\n"
                     : "[TF2VR] hand.ref_yaw = 0: OLD composition -- the gun sits off the view by the reference yaw.\n");
}

void SetHandYawRebase(bool enabled) {
    g_yawRebase = enabled;
    Tf2VrLog(enabled
        ? "[TF2VR] hand yaw RE-BASED at consume time: the gun's yaw follows the render frame's "
          "body yaw, not the plugin frame's. Measured stale by one frame, up to 8 deg while "
          "turning.\n"
        : "[TF2VR] hand yaw NOT re-based: the composed yaw is used raw (the one-frame turn lag "
          "is back).\n");
}

// THE COMPOSED POSE, FOR THE PIN.
//
// H1 drives the viewmodel's placement directly, so the convar bridge that fed
// the retired script prop is no longer the consumer. Same numbers, same
// composition, published to a global the pin reads on the engine's own frame
// instead of through six console variables.
float g_composed[6]{};
// The camera base this composition was formed against, so the consumer can add
// on however far the camera has travelled since. See TryGetComposedHandPose.
float g_composedBase[3]{};
// The BODY YAW half of the same idea, and the one the composition never used.
// g_composed[4] is an ABSOLUTE Source yaw formed on the plugin frame as
// localYaw + (base it saw) + offYaw, so it carries that frame's base -- while
// the base itself is latched on the RENDER frame. Recorded here, inside the
// same seqlock, so the consumer can measure (and later remove) the difference.
float g_composedBaseYaw = 0.0f;
std::atomic_bool g_composedValid = false;

// Defined with NoteYawLag, below the pose readers that both use it.
float YawRebaseDelta(float composedBaseYawUsed);
void RotateOffsetByYaw(float offset[3], float degrees);
void NoteYawLag(float composedYaw, float composedBaseYawUsed, const float offset[3]);

// How often the seqlock could not get a clean snapshot. Printed by YAWLAG, so
// the fix below is not taken on trust: if this reads 0 for a whole session it
// was never the glitch, and that is worth knowing before anything else is
// blamed on it.
std::atomic_uint64_t g_seqlockMisses{0};

// ONE READER, AND IT NEVER PUBLISHES A POSE IT DID NOT READ.
//
// The two copies of this loop that this replaces both had the same hole. On the
// LAST attempt, an odd generation took the `continue` branch, the loop ended,
// and the caller went on to use a `composed[6]{}` that had never been written
// -- a zero pose. For the pin that means offset (0,0,0) and angles (0,0,0): the
// gun SNAPS to the engine's default placement, pointing at world yaw zero, for
// one frame. A guard against torn reads that fails open with zeros is worse
// than no guard, because a torn read is a few wrong millimetres and this is a
// whole wrong gun.
//
// Failing CLOSED is not good enough either: returning false makes the pin leave
// the engine's own viewmodel placement, which is also a one-frame snap, just to
// a different wrong place. So the fallback is THIS THREAD'S LAST GOOD SNAPSHOT
// -- one frame of stale hand pose, which is invisible, instead of a jump.
//
// thread_local because the pin reads on the render thread and the aim write
// reads in CreateMove: a shared cache would reintroduce, in the fallback, the
// exact tearing the seqlock exists to prevent.
bool ReadComposedSnapshot(float composed[6], float composedBase[3], float* composedBaseYaw);

// A SEQLOCK, BECAUSE THESE NINE FLOATS ARE ONE VALUE.
//
// g_composed and g_composedBase are written here on the PLUGIN frame and read
// by the pin on the RENDER frame, and the consumer subtracts one from the
// other. Pairing a new composed position with the previous base is therefore
// not a slightly stale answer -- it is the base movement counted TWICE.
//
// Standing still that is invisible, because the base barely moves between two
// frames. On a step it is one whole step height, for exactly one frame, which
// is the "gun gets drawn considerably high for a single frame, much higher
// than the step" that was reported. Then it corrects and eases up with the
// view, which looked like a second bounce and was actually the fix working.
//
// Same discipline as the controller blocks in xr_input.h and the camera angle
// pair in camera_hook.asm, for the same reason: odd while a write is in
// progress, and a reader that sees an odd or changed value retries.
std::atomic<std::uint32_t> g_composedGeneration{0};

bool ReadComposedSnapshot(float composed[6], float composedBase[3], float* composedBaseYaw) {
    thread_local float cachedPose[6]{};
    thread_local float cachedBase[3]{};
    thread_local float cachedYaw = 0.0f;
    thread_local bool cacheValid = false;

    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::uint32_t before = g_composedGeneration.load(std::memory_order_acquire);
        if (before & 1u) continue;   // a write is in progress
        std::memcpy(composed, g_composed, sizeof(float) * 6);
        std::memcpy(composedBase, g_composedBase, sizeof(float) * 3);
        *composedBaseYaw = g_composedBaseYaw;
        if (g_composedGeneration.load(std::memory_order_acquire) != before) continue;
        std::memcpy(cachedPose, composed, sizeof(cachedPose));
        std::memcpy(cachedBase, composedBase, sizeof(cachedBase));
        cachedYaw = *composedBaseYaw;
        cacheValid = true;
        return true;
    }

    g_seqlockMisses.fetch_add(1, std::memory_order_relaxed);
    if (!cacheValid) return false;   // nothing has ever been read cleanly
    std::memcpy(composed, cachedPose, sizeof(cachedPose));
    std::memcpy(composedBase, cachedBase, sizeof(cachedBase));
    *composedBaseYaw = cachedYaw;
    return true;
}

void RecordComposed(const Pose& pose) {
    g_composedGeneration.fetch_add(1, std::memory_order_acq_rel);   // odd: writing
    // C4 -- THE BRACE, APPLIED HERE AND NOWHERE ELSE.
    //
    // The plan requires it at BOTH consumers from one source of truth. This IS
    // that source: the pin (placement_pin.cpp:142) and the aim write
    // (aim_cmd.cpp:147) both read g_composed through functions that derive
    // angles[] line for line identically, so bracing the value that goes IN
    // means the two cannot diverge -- not by discipline, by construction.
    //
    // It also has to happen ONCE PER FRAME, and that is the second reason it is
    // here. The anchor is dragged when the hand travels far enough, and the two
    // consumers run at different rates on different threads -- the pin on the
    // render frame, the aim write inside CreateMove at the command tick. An
    // anchor advanced per CONSUME would be dragged twice per frame by one
    // consumer and once by the other, and they would read different anchors.
    // Advanced per PUBLISH it is one value, one clock.
    //
    // Bracing the pose before it is stored also means the yaw re-base further
    // down still applies to both consumers on top, unchanged. The re-base
    // corrects a clock gap in the BODY's yaw; the brace scales the HAND's
    // movement. They are different quantities and they compose.
    Pose braced = pose;
    // Undo the body turns WE made before anything else looks at this pose, so
    // every consumer sees the hand where the arm physically points.
    braced.yaw = WrapAdsDegrees(braced.yaw - g_adsYawCompensation.load(std::memory_order_acquire));
    ApplyAdsBrace(braced, BitsToFloat(g_cameraBaseYawBits));
    g_composed[0] = braced.x; g_composed[1] = braced.y; g_composed[2] = braced.z;
    g_composed[3] = braced.pitch; g_composed[4] = braced.yaw; g_composed[5] = braced.roll;
    CameraAnchor(g_composedBase);
    g_composedBaseYaw = BitsToFloat(g_cameraBaseYawBits);
    g_composedGeneration.fetch_add(1, std::memory_order_acq_rel);   // even: complete
    g_composedValid.store(true, std::memory_order_release);
}

void PublishHandPoseForFrame() {
    switch (g_source) {
        case HandPoseSource::Off:
            return;

        // SyntheticSpin is gone with the convar bridge (2026-09-08); SetHandPoseSource
        // refuses the value, so this case is unreachable and the enum no longer has it.

        case HandPoseSource::SyntheticFixedOffset: {
            // Half a metre forward, a quarter right, a quarter down: roughly
            // where a held rifle sits, and far enough off-centre that a wrong
            // sign in the room->Source mapping puts it somewhere obviously
            // wrong rather than somewhere plausible.
            //
            // Deliberately CONSTANT in room space. Everything the player then
            // does -- walking, turning, looking around -- goes through exactly
            // the same composition the real hand will, so if the gun holds its
            // place relative to the body while they move, the composition is
            // right and only the input is left to swap.
            const float head[3] = {0.0f, 0.0f, 0.0f};
            const float hand[3] = {0.25f, -0.25f, -0.50f};  // xr: +x right, +y up, -z forward
            float forward = 0.0f, left = 0.0f, up = 0.0f;
            RoomOffsetToSource(hand, head, forward, left, up);
            const Pose pose = ComposeWorld(forward, left, up, 0.0f, 0.0f, 0.0f);
            RecordComposed(pose);
            LogOccasionally("fixed offset", pose);
            return;
        }

        case HandPoseSource::RightController: {
            // THE AIM POSE, NOT THE GRIP POSE, AND THE LOGGED ANGLES SAY WHY.
            //
            // The first headset run used the grip pose and the gun sat pitched
            // about 45 degrees into the air -- the log shows pitch running -44
            // to -57, and Source pitch is negative upward, so that matches the
            // report exactly rather than being inferred from it.
            //
            // That is what a grip pose IS. OpenXR defines it around the axis of
            // the fist holding a rod, so its forward runs along the HANDLE. A
            // rifle's barrel leaves the hand at a large angle to the grip, so
            // using it as a barrel direction is wrong by roughly that angle.
            // The aim pose is the one OpenXR defines as pointing where the user
            // is aiming, which is the barrel direction this needs.
            //
            // Position still comes from the GRIP, because that is where the
            // hand actually is and it is what the gun should be attached to.
            // Taking orientation and position from different poses is the
            // normal thing to do here, not a bodge.
            float handMetres[3]{};
            float basis[9]{};
            if (!TryReadRightPose(handMetres, basis)) {
                // Not tracked, or the block was being written every time we
                // looked. Publishing nothing leaves the last good pose in the
                // convars, which holds the gun still rather than snapping it to
                // the origin -- the failure that looks like a bug rather than
                // like a lost controller.
                return;
            }
            // THE ROOM ORIGIN, NOT THE LIVE HEAD POSITION.
            //
            // This used to subtract where the head IS, which made the hand's
            // world position depend on the head. The head pivots about the neck,
            // so turning it translates the eyes several centimetres -- and the
            // reported symptom was exactly that: with the hand held still,
            // rotating the head slid the gun slightly the OPPOSITE way, by about
            // the distance the eyes travel.
            //
            // The subtraction was only ever there to cancel the room origin, and
            // the reference head tracking captured when it armed is a fixed
            // point that cancels it just as well without moving. It is also the
            // same origin the camera's own positional tracking is measured from,
            // so the hand and the view agree about where the room is.
            float headMetres[3]{};
            if (!TryGetHeadPositionReference(headMetres)) {
                // No reference yet means head tracking has not armed. Falling
                // back to the live position keeps the gun roughly right rather
                // than placing it at the room origin.
                headMetres[0] = g_headPositionMetres[0];
                headMetres[1] = g_headPositionMetres[1];
                headMetres[2] = g_headPositionMetres[2];
            }
            float forward = 0.0f, left = 0.0f, up = 0.0f;
            RoomOffsetToSource(handMetres, headMetres, forward, left, up);

            float pitch = 0.0f, yaw = 0.0f, roll = 0.0f;
            BasisToAngles(basis, pitch, yaw, roll);

            const Pose pose = ComposeWorld(forward, left, up, pitch, yaw, roll);
            RecordComposed(pose);
            LogOccasionally("right grip", pose);
            return;
        }
    }
}

// The RAW INPUTS to the composition, so the correct room-to-world rotation can
// be derived from a capture instead of guessed at by sweeping.
//
// headYaw is the headset's rotation in room space; localYaw is the HAND's, from
// the same room basis; gameYaw is the engine's own view yaw before head
// tracking is added. If the player holds the controller still against their
// chest and turns, headYaw and localYaw must move TOGETHER, and whatever keeps
// the composed world yaw fixed relative to the player is the answer.
bool TryGetHandYawInputs(float* headYaw, float* localYaw, float* gameYaw) {
    if (!g_composedValid.load(std::memory_order_acquire)) return false;
    if (headYaw) *headYaw = g_headYawDegrees;
    if (gameYaw) *gameYaw = BitsToFloat(g_cameraBaseYawBits);
    // localYaw is what the composition added the body yaw to, recovered rather
    // than stored separately so it cannot drift from what was actually used.
    if (localYaw) *localYaw = g_composed[4] - (BitsToFloat(g_cameraBaseYawBits) -
                                               g_lastYawComp * g_headYawDegrees);
    return true;
}

// RE-BASE ONTO THE CURRENT CAMERA POSITION AT CONSUME TIME.
//
// The composition runs on the PLUGIN frame and the pin consumes it on the
// RENDER frame, and the camera base is latched on the render frame. So the
// composed position carries whichever base the plugin frame last saw, and
// while the player is moving that is a whole frame of travel out of date --
// which appears as the gun sitting displaced OPPOSITE to the direction of
// motion, holding that displacement for as long as the movement lasts, and
// snapping back on stop. Reported exactly that way: "if I move left (strafe)
// the gun shifts right a bit and stays there till I finish movement".
//
// The two clocks also do not tick together, so the error is not even constant
// frame to frame, which is the "very jerky" half of the same report.
//
// Adding the base movement since composition removes both. This is the rule
// the codebase already states in xr_input.h -- compose where the value is
// CONSUMED -- and the same bug the camera hook's own comment records having
// fixed once already for positional head tracking, where a sum formed on the
// plugin frame left one frame at the pre-step height.
bool TryGetComposedHandPose(float position[3], float angles[3]) {
    if (!g_composedValid.load(std::memory_order_acquire)) return false;

    // Take the composed pose and the base it was formed against as ONE value.
    // See RecordComposed: reading them from different writes counts the base
    // movement twice, which is invisible standing still and is a whole step
    // height for one frame on a stair.
    float composed[6]{};
    float composedBase[3]{};
    float composedBaseYaw = 0.0f;
    if (!ReadComposedSnapshot(composed, composedBase, &composedBaseYaw)) return false;

    const float yawDelta = YawRebaseDelta(composedBaseYaw);

    if (position) {
        float baseNow[3]{};
        CameraAnchor(baseNow);
        float offset[3] = {composed[0] - composedBase[0], composed[1] - composedBase[1],
                           composed[2] - composedBase[2]};
        RotateOffsetByYaw(offset, yawDelta);
        position[0] = baseNow[0] + offset[0];
        position[1] = baseNow[1] + offset[1];
        position[2] = baseNow[2] + offset[2];

        // NO STEP CORRECTION HERE ANY MORE.
        //
        // A term was added that added back `viewOrigin - written`, on the
        // theory that the difference between what we wrote to the camera and
        // what the view was built from is purely the engine step easing, with
        // our own head offset cancelling. The log says otherwise: three
        // consecutive frames on a +6.21 step gave out=710.21, 710.21, then
        // 705.31, where base+offset says 710.2 throughout. The term was
        // injecting a 4.9-unit drop one frame after every step -- the "goes
        // back down then animates up" half of the report.
        //
        // It cancels only if both quantities come from the same pass, and they
        // do not: the main-scene origin is latched on the first main-scene pass
        // and desiredPos is whatever the camera hook last wrote, so the
        // difference carries a frame of HEAD motion as well as the step. That
        // is not a small error in VR.
    }
    // The SAME re-base the pin's consumer applies, for the same reason and by
    // the same quantity. See the note at YawRebaseDelta: gun and aim must move
    // together or a turn opens a gap between where it points and where it hits.
    if (angles) {
        angles[0] = composed[3];
        angles[1] = composed[4] + yawDelta;
        angles[2] = composed[5];
    }

    return true;
}

// MOVE THE PIVOT, LEAVE THE GUN WHERE IT IS.
//
// Composition: world = hand + Rz(bodyYaw)*playerOffset + R(weapon)*grip.
// The model-space point that stays fixed under a wrist rotation is therefore
// exactly -grip, so grip already IS the pivot control. What made it unusable is
// that changing it also translates the gun by R*delta, and that translation is
// the only part the wearer can see -- "the tooling moved the GUN; I want to
// move the PIVOT".
//
// So: apply delta to grip, then subtract the same R*delta from the player-frame
// offset, converting it back through the body yaw the composition used. The
// world position is unchanged at the orientation the wearer is holding right
// now, and only the point it swings about has moved. That makes the pivot
// something you can see by rotating your wrist and watching what stays still.
void NudgePivotOffset(float forward, float right, float up) {
    NudgeGripOffset(forward, right, up);
    if (!g_basisValid) return;

    // The world displacement the grip change just introduced.
    const float wx = g_basisFwd[0] * forward + g_basisRight[0] * right + g_basisUp[0] * up;
    const float wy = g_basisFwd[1] * forward + g_basisRight[1] * right + g_basisUp[1] * up;
    const float wz = g_basisFwd[2] * forward + g_basisRight[2] * right + g_basisUp[2] * up;

    // Back into the player frame. The composition does
    //   x = fwd*c - left*s ;  y = fwd*s + left*c
    // so the inverse is a rotation by -bodyYaw.
    constexpr float kD2R = 3.14159265358979323846f / 180.0f;
    const float c = std::cos(g_basisBodyYaw * kD2R);
    const float s = std::sin(g_basisBodyYaw * kD2R);
    const float pf = wx * c + wy * s;
    const float pl = -wx * s + wy * c;

    NudgeHandPositionOffset(-pf, -pl, -wz);
}

// THE YAW RE-BASE. Measured first, then applied -- see the numbers below.
//
// The position half of this pose is a DELTA (composed - composedBase) added to
// what the engine just committed, so it carries no cross-clock error. The
// ANGLES were absolutes: g_composed[4] is formed on the PLUGIN frame as
// localYaw + (the base that frame saw) + offYaw, and it is consumed on the
// RENDER frame, where the base has already moved on. The difference is
//
//     lag = baseYaw(render, now) - baseYaw(plugin, at composition)
//
// and the 2026-08-21 headset run measured it directly. On EVERY line of that
// run the largest `lag` equalled the largest `turned` to three decimals:
//
//     turned max -8.05 deg/consume | lag max -8.049
//     turned max -7.31             | lag max -7.307
//     turned max +3.91             | lag max +3.912
//     turned max +0.00             | lag max +0.000
//
// `lag == turn` means baseAtCompose at consume k IS baseNow at consume k-1:
// the composition is stale by EXACTLY ONE FRAME in body yaw. Zero standing
// still, 2.6-4.1 degrees at ordinary stick speed, 8 at full turn rate -- the
// gun trailing the view while turning and snapping back on stop, and, because
// the amount differs frame to frame, a different error in each eye under
// alternate-frame stereo, which is the flicker.
//
// The fix is the same one camera_hook.asm records having already made for the
// camera's own yaw write ("the error is the angular velocity times the gap")
// and the same one the position re-base above made for translation: add the
// base movement since composition, at the point of CONSUMPTION.
//
// It is added identically at BOTH consume sites -- the pin's placement and the
// aim write -- because both read the same composed absolute. Applying it to
// one alone would open a turn-rate-proportional gap between where the gun is
// drawn and where it shoots, which is a new defect, not half a fix.
//
// PITCH AND ROLL ARE NOT TOUCHED. They are head-sourced absolutes in the
// runtime's gravity-aligned space and derive from no base, so they carry no
// cross-clock error -- the same reasoning camera_hook.asm states for its own
// pitch and roll, and the reason defect A's fix took heading only.
//
// COST IS BOUNDED BY CONSTRUCTION: a handful of floats per consume and ONE log
// line per second, whatever the frame rate.
//
// IT PROVES IT IS WATCHING: `n` is the sample count and `turned` is the largest
// base-yaw movement between consecutive consumes. A run where `turned` stays
// near zero did not turn, and its `lag` of zero means nothing -- that is
// reported rather than being allowed to read as a refutation.
float YawRebaseDelta(float composedBaseYawUsed) {
    float lag = BitsToFloat(g_cameraBaseYawBits) - composedBaseYawUsed;
    while (lag > 180.0f) lag -= 360.0f;
    while (lag < -180.0f) lag += 360.0f;
    return g_yawRebase ? lag : 0.0f;
}

// THE POSITION HALF OF THE SAME RE-BASE, AND IT IS EXACT, NOT AN APPROXIMATION.
//
// The first pass at this corrected the composed YAW and left the composed
// OFFSET alone, which is only half the stale body yaw -- and the wearer saw
// exactly that: "a bit better on rotation but not perfect".
//
// Read the composition (ComposeHandPose, above). The body yaw enters it in
// precisely two places:
//
//   1. the player-frame offset, rotated:  (fwd*c - left*s, fwd*s + left*c)
//   2. pose.yaw = localYaw + bodyYaw + offYaw, which then orients the grip
//      through the forward/right/up basis
//
// and the consumed offset is exactly the sum of those two terms
// (composed - composedBase). Adding d to a Source yaw is a LEFT-multiplication
// by Rz(d) on that basis -- check the rows: fwd = (cp*cy, cp*sy, -sp) becomes
// (cp*cos(y+d), cp*sin(y+d), -sp) = Rz(d)*fwd, and right and up have the same
// (A*cy + B*sy, A*sy - B*cy) form, which transforms the same way, with the z
// components independent of yaw throughout.
//
// So rotating the WHOLE offset by Rz(d) and adding d to the yaw reproduces,
// term for term, what the composition would have produced had it run with the
// render frame's body yaw. Nothing is left carrying the stale value.
//
// The size of what this corrects: the offset is the hand's reach plus the grip,
// tens of Source units, so 4 degrees of lag swings the gun a couple of units
// sideways and 8 degrees roughly twice that -- a sway that appears only while
// turning, which is what "not perfect" was pointing at. YAWLAG prints it.
void RotateOffsetByYaw(float offset[3], float degrees) {
    if (degrees == 0.0f) return;
    constexpr float kD2R = 3.14159265358979323846f / 180.0f;
    const float c = std::cos(degrees * kD2R);
    const float s = std::sin(degrees * kD2R);
    const float x = offset[0] * c - offset[1] * s;
    const float y = offset[0] * s + offset[1] * c;
    offset[0] = x;
    offset[1] = y;   // z is unchanged: the rotation is about the vertical
}

void NoteYawLag(float composedYaw, float composedBaseYawUsed, const float offset[3]) {
    const float baseNow = BitsToFloat(g_cameraBaseYawBits);
    float lag = baseNow - composedBaseYawUsed;
    while (lag > 180.0f) lag -= 360.0f;
    while (lag < -180.0f) lag += 360.0f;

    static float lastBaseNow = 0.0f;
    static bool haveLast = false;
    float turn = 0.0f;
    if (haveLast) {
        turn = baseNow - lastBaseNow;
        while (turn > 180.0f) turn -= 360.0f;
        while (turn < -180.0f) turn += 360.0f;
    }
    lastBaseNow = baseNow;
    haveLast = true;

    static std::uint64_t samples = 0;
    static double lagSum = 0.0;
    static float lagMax = 0.0f;
    static float turnMax = 0.0f;
    static float lagAtTurnMax = 0.0f;
    static std::uint64_t nextReport = 0;

    // The horizontal reach the yaw correction actually swings, so the position
    // half of the fix is a measured quantity in the log rather than an argument
    // in a comment: swing = |offset_xy| * sin(lag), in Source units.
    static float reachAtTurnMax = 0.0f;
    const float reach =
        std::sqrt(offset[0] * offset[0] + offset[1] * offset[1]);

    ++samples;
    lagSum += std::fabs(lag);
    if (std::fabs(lag) > std::fabs(lagMax)) lagMax = lag;
    if (std::fabs(turn) > std::fabs(turnMax)) {
        turnMax = turn;
        lagAtTurnMax = lag;
        reachAtTurnMax = reach;
    }

    const std::uint64_t now = GetTickCount64();
    if (nextReport == 0) { nextReport = now + 1000; return; }
    if (now < nextReport) return;
    nextReport = now + 1000;

    constexpr float kD2R = 3.14159265358979323846f / 180.0f;
    const float swing = reachAtTurnMax * std::sin(lagAtTurnMax * kD2R);
    const unsigned long long misses = g_seqlockMisses.load(std::memory_order_relaxed);
    static unsigned long long lastMisses = 0;

    char line[400]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR] YAWLAG rebase=%d n=%llu turned max %+.2f deg/consume | lag mean %.3f "
                  "max %+.3f | lag at fastest turn %+.3f over reach %.1f u = swing %+.2f u "
                  "| seqlock miss %llu this second, %llu total | composedYaw %.2f "
                  "baseAtCompose %.2f baseNow %.2f\n",
                  g_yawRebase ? 1 : 0,
                  static_cast<unsigned long long>(samples), static_cast<double>(turnMax),
                  samples ? lagSum / static_cast<double>(samples) : 0.0,
                  static_cast<double>(lagMax), static_cast<double>(lagAtTurnMax),
                  static_cast<double>(reachAtTurnMax), static_cast<double>(swing),
                  misses - lastMisses, misses,
                  static_cast<double>(composedYaw),
                  static_cast<double>(composedBaseYawUsed), static_cast<double>(baseNow));
    Tf2VrLog(line);
    lastMisses = misses;
    samples = 0; lagSum = 0.0; lagMax = 0.0f; turnMax = 0.0f; lagAtTurnMax = 0.0f;
    reachAtTurnMax = 0.0f;
}

bool TryGetComposedHandOffset(float offset[3], float angles[3]) {
    if (!g_composedValid.load(std::memory_order_acquire)) return false;
    float composed[6]{};
    float composedBase[3]{};
    float composedBaseYaw = 0.0f;
    if (!ReadComposedSnapshot(composed, composedBase, &composedBaseYaw)) return false;

    const float yawDelta = YawRebaseDelta(composedBaseYaw);
    float local[3] = {composed[0] - composedBase[0], composed[1] - composedBase[1],
                      composed[2] - composedBase[2]};
    RotateOffsetByYaw(local, yawDelta);

    if (offset) { offset[0] = local[0]; offset[1] = local[1]; offset[2] = local[2]; }
    if (angles) {
        angles[0] = composed[3];
        angles[1] = composed[4] + yawDelta;
        angles[2] = composed[5];
    }
    NoteYawLag(composed[4], composedBaseYaw, local);
    return true;
}

// ---------------------------------------------------------------------------
// C4's four settings. All live, all reloadable with LEADER then END, and all
// read by ApplyAdsBrace on the plugin frame -- so a dial in the panel takes
// effect on the next composed pose and no rebuild is needed to judge one.
void SetAdsBraceGain(float gain) {
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    const float previous = g_braceGain.exchange(gain, std::memory_order_relaxed);
    if (previous == gain) return;
    char line[420]{};
    if (gain >= kBraceOffGain) {
        Tf2VrLog("[TF2VR] ads.brace_gain = 1.00 -> the brace is OFF. A gain of one IS no brace "
                 "arithmetically, so there is no separate flag that could disagree with it.\n");
        return;
    }
    std::snprintf(line, sizeof(line),
        "[TF2VR] ads.brace_gain = %.2f. While ADS is held the gun moves this fraction of what the "
        "hand moves, SAME FRAME -- a scaled delta, never a filter, so nothing arrives late. A 1 "
        "degree tremor becomes %.2f degrees by construction.\n",
        static_cast<double>(gain), static_cast<double>(gain));
    Tf2VrLog(line);
}
float AdsBraceGain() { return g_braceGain.load(std::memory_order_relaxed); }

void SetAdsBraceConeDegrees(float degrees) {
    if (degrees < 0.1f) degrees = 0.1f;
    g_braceConeDeg.store(degrees, std::memory_order_relaxed);
}
float AdsBraceConeDegrees() { return g_braceConeDeg.load(std::memory_order_relaxed); }

void SetAdsBraceReadoptDegrees(float degrees) {
    if (degrees < 0.1f) degrees = 0.1f;
    const float cone = g_braceConeDeg.load(std::memory_order_relaxed);
    // SAID OUT LOUD RATHER THAN SILENTLY CORRECTED. ApplyAdsBrace enforces this
    // again at use, because the two can also be set in the other order from the
    // ini -- but a value the wearer typed and did not get should appear in the
    // log, not only in the behaviour.
    if (degrees <= cone) {
        char line[320]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] ads.brace_readopt_deg = %.1f is not above ads.brace_cone_deg = %.1f. It has "
            "to be, or the effective gain steps from the gain to 1.0 at a single value and a hand "
            "resting on that value alternates the two every frame. Using %.1f.\n",
            static_cast<double>(degrees), static_cast<double>(cone),
            static_cast<double>(cone * 1.5f));
        Tf2VrLog(line);
    }
    g_braceReadoptDeg.store(degrees, std::memory_order_relaxed);
}
float AdsBraceReadoptDegrees() { return g_braceReadoptDeg.load(std::memory_order_relaxed); }

void SetAdsBraceForced(bool forced) {
    const bool previous = g_braceForce.exchange(forced, std::memory_order_relaxed);
    if (previous == forced) return;
    Tf2VrLog(forced
        ? "[TF2VR] ads.brace_force = 1: the brace is held engaged without the trigger. THIS IS FOR "
          "A FLAT RUN. IsAdsHeld() is false whenever vr input is off, which flat it always is, so "
          "without this the brace could not be exercised on a monitor at all -- and its ratio "
          "falsifiers are exactly what a monitor CAN judge. Turn it off for anything in a headset.\n"
        : "[TF2VR] ads.brace_force = 0: the brace follows the left trigger again.\n");
}
bool IsAdsBraceForced() { return g_braceForce.load(std::memory_order_relaxed); }

void NoteExternalBodyYawWrite(float degrees) {
    if (degrees == 0.0f) return;
    // Inside the seqlock, because a consumer reading composedBaseYaw between
    // these two bumps would pair a corrected reference with an uncorrected
    // pose -- which is the tearing the seqlock exists to prevent, arriving by
    // a different door.
    g_composedGeneration.fetch_add(1, std::memory_order_acq_rel);   // odd: writing
    g_composedBaseYaw += degrees;
    g_composedGeneration.fetch_add(1, std::memory_order_acq_rel);   // even: complete
    // THE BRACE MUST NOT SEE OUR OWN TURN. 2026-09-04. Its anchor is held in
    // body-frame yaw so a STICK turn moves gun and body together. A face-aim
    // turn is compensated -- the gun does not turn with the body -- so the same
    // physical hand reads `degrees` further round in the body frame, inside
    // the ramp zone for any aim under the re-adopt angle. The brace then
    // pulled the aim part-way back, the follow turned the body after it, and
    // they settled a fraction off, growing with the aim's offset from centre:
    // "aim a bit right, it goes into ADS a bit further right". The anchor and
    // the step reference move with the body, so the deviation stays zero.
    if (g_braceEngaged) {
        // WrapDegrees is ambiguous at this scope (a header twin); wrap by hand.
        const auto wrap = [](float v) {
            while (v > 180.0f) v -= 360.0f;
            while (v < -180.0f) v += 360.0f;
            return v;
        };
        g_braceAnchorYawBody = wrap(g_braceAnchorYawBody - degrees);
        g_bracePrevHandYawBody = wrap(g_bracePrevHandYawBody - degrees);
        g_bracePrevOutYawBody = wrap(g_bracePrevOutYawBody - degrees);
    }
}

void AddAdsYawCompensation(float degrees, bool log) {
    if (degrees == 0.0f) return;
    g_adsYawCompensation.store(
        WrapAdsDegrees(g_adsYawCompensation.load(std::memory_order_acquire) + degrees),
        std::memory_order_release);
    if (!log) return;
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] ADS yaw compensation %+.1f, total %+.1f. The body was turned by us, not by the "
        "player, so the hand mapping carries the difference and the gun stays pointing where the "
        "arm physically points.\n",
        static_cast<double>(degrees),
        static_cast<double>(g_adsYawCompensation.load(std::memory_order_acquire)));
    Tf2VrLog(line);
}

float AdsYawCompensation() { return g_adsYawCompensation.load(std::memory_order_acquire); }
