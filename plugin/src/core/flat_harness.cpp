#include "flat_harness.h"

#include "diagnostics.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>

// The same globals the OpenXR path publishes. Writing them here is what makes
// the headset optional: nothing downstream knows or cares where a pose came
// from, which is also what makes this a fair test rather than a parallel one.
extern "C" volatile float g_headBasis[9];
extern "C" volatile std::uint32_t g_headBasisGeneration;
extern "C" volatile float g_headPositionMetres[3];
extern "C" volatile float g_headYawDegrees;
extern "C" volatile float g_headPitchDegrees;
extern "C" volatile float g_headRollDegrees;
extern "C" volatile std::uint8_t g_headPoseValid;
extern "C" volatile std::uint64_t g_headPoseSequence;

namespace {
// A constant pose proved the correction exact and stable, so whatever is left
// needs MOTION. Sweep supplies motion that is smooth, repeatable and headset
// free, which a hand waving a headset around is not: the same test can be run
// twice and the two runs compared.
enum class Mode { Off, Static, Sweep };
std::atomic<Mode> g_mode = Mode::Off;
std::atomic_bool g_enabled = false;
std::atomic<float> g_pitch = 0.0f;
// Starts at ZERO so head tracking can be armed here and the reference captured
// at a known origin; the yaw is then stepped to put the correction under load.
std::atomic<float> g_yaw = 0.0f;
std::atomic<float> g_roll = 0.0f;

// Source's AngleVectors, rows forward/right/up -- the same basis convention the
// OpenXR path publishes, so the consumer cannot tell the two apart.
void AngleBasisRows(float pitchDeg, float yawDeg, float rollDeg, float basis[9]) {
    constexpr float toRad = 0.01745329252f;
    const float sp = std::sin(pitchDeg * toRad), cp = std::cos(pitchDeg * toRad);
    const float sy = std::sin(yawDeg * toRad), cy = std::cos(yawDeg * toRad);
    const float sr = std::sin(rollDeg * toRad), cr = std::cos(rollDeg * toRad);
    basis[0] = cp * cy;   basis[1] = cp * sy;   basis[2] = -sp;
    basis[3] = -sr * sp * cy + cr * sy;
    basis[4] = -sr * sp * sy - cr * cy;
    basis[5] = -sr * cp;
    basis[6] = cr * sp * cy + sr * sy;
    basis[7] = cr * sp * sy - sr * cy;
    basis[8] = cr * cp;
}
}  // namespace

// One key cycles OFF -> STATIC -> SWEEP -> OFF, so motion needs no extra
// binding on a keyboard that has already run out of safe keys.
void CycleSyntheticPoseMode() {
    const Mode current = g_mode.load(std::memory_order_acquire);
    const Mode next = current == Mode::Off ? Mode::Static
                    : current == Mode::Static ? Mode::Sweep
                    : Mode::Off;
    g_mode.store(next, std::memory_order_release);
    g_enabled.store(next != Mode::Off, std::memory_order_release);
    if (next == Mode::Off) {
        g_headPoseValid = 0;
        Tf2VrLog("[TF2VR] synthetic pose OFF; the headset is the pose source again.\n");
        return;
    }
    char line[320]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] synthetic pose %s, yaw %.1f deg, no headset and no OpenXR.%s\n",
        next == Mode::Static ? "STATIC" : "SWEEPING",
        g_yaw.load(std::memory_order_relaxed),
        next == Mode::Sweep
            ? " Yaw oscillates +-25 deg at a steady rate; a velocity-proportional error shows up"
              " here and cannot hide behind a hand that is not perfectly still."
            : " Arm head tracking, then step the yaw to put the correction under load.");
    Tf2VrLog(line);
}

void SetSyntheticPoseEnabled(bool enabled) {
    g_mode.store(enabled ? Mode::Static : Mode::Off, std::memory_order_release);
    g_enabled.store(enabled, std::memory_order_release);
    if (!enabled) {
        // Hand the pose source back. Head tracking releases itself when the
        // sequence stops advancing, so nothing is left pinned.
        g_headPoseValid = 0;
        Tf2VrLog("[TF2VR] synthetic pose OFF; the headset is the pose source again.\n");
        return;
    }
    char line[280]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] synthetic pose ON: a CONSTANT head pose of pitch %.1f yaw %.1f roll %.1f deg, "
        "published with no headset and no OpenXR. Arm head tracking and the view should hold "
        "perfectly still. Anything that moves is ours.\n",
        g_pitch.load(std::memory_order_relaxed), g_yaw.load(std::memory_order_relaxed),
        g_roll.load(std::memory_order_relaxed));
    Tf2VrLog(line);
}

bool IsSyntheticPoseEnabled() { return g_enabled.load(std::memory_order_acquire); }

void NudgeSyntheticYaw(float deltaDegrees) {
    float next = g_yaw.load(std::memory_order_relaxed) + deltaDegrees;
    while (next > 90.0f) next -= 180.0f;
    while (next < -90.0f) next += 180.0f;
    g_yaw.store(next, std::memory_order_release);
    char line[280]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] synthetic yaw now %.1f deg. If head tracking was armed at a different yaw, the "
        "correction is now under a constant non-zero rotation -- which is the state the first "
        "version of this harness could never reach.\n", next);
    Tf2VrLog(line);
}

float SyntheticYaw() { return g_yaw.load(std::memory_order_acquire); }

void SetSyntheticPoseAngles(float pitchDegrees, float yawDegrees, float rollDegrees) {
    g_pitch.store(pitchDegrees, std::memory_order_release);
    g_yaw.store(yawDegrees, std::memory_order_release);
    g_roll.store(rollDegrees, std::memory_order_release);
}

// A TILTED synthetic pose, which is the one thing this harness could not
// produce and the one thing the heading-only composition has to be tested
// against.
//
// The harness could only ever vary YAW: pitch and roll were fixed at zero, so
// the recentre reference was always captured level, and a composition that
// subtracts the reference's pitch and roll looked identical to one that does
// not. That is a null A/B -- the flat test could not tell the two apart, and
// would have reported "no change" for a real fix.
//
// Set these and the reference is captured TILTED, which is exactly the state
// that bakes a constant tilt into the old composition. Both default to zero,
// so nothing changes for anyone who does not set them.
void SetSyntheticPosePitch(float pitchDegrees) {
    g_pitch.store(pitchDegrees, std::memory_order_release);
}

void SetSyntheticPoseRoll(float rollDegrees) {
    g_roll.store(rollDegrees, std::memory_order_release);
}

void AdvanceSyntheticPose() {
    if (!g_enabled.load(std::memory_order_acquire)) return;
    const float pitch = g_pitch.load(std::memory_order_relaxed);
    float yaw = g_yaw.load(std::memory_order_relaxed);
    const float roll = g_roll.load(std::memory_order_relaxed);

    if (g_mode.load(std::memory_order_acquire) == Mode::Sweep) {
        // A slow, smooth oscillation about the configured yaw. Slow enough to
        // watch, fast enough that a velocity-proportional error is obvious, and
        // driven by the wall clock so it is identical between runs.
        const double seconds = static_cast<double>(GetTickCount64()) / 1000.0;
        yaw += 25.0f * static_cast<float>(std::sin(seconds * 1.2));
    }

    float basis[9]{};
    AngleBasisRows(pitch, yaw, roll, basis);

    g_headBasisGeneration = g_headBasisGeneration + 1;
    for (int i = 0; i < 9; ++i) g_headBasis[i] = basis[i];
    g_headBasisGeneration = g_headBasisGeneration + 1;

    g_headPitchDegrees = pitch;
    g_headYawDegrees = yaw;
    g_headRollDegrees = roll;
    // Held at the origin: positional tracking then contributes exactly nothing,
    // so it cannot muddy a rotation test.
    g_headPositionMetres[0] = 0.0f;
    g_headPositionMetres[1] = 0.0f;
    g_headPositionMetres[2] = 0.0f;
    g_headPoseValid = 1;
    // Head tracking only acts on a CHANGED sequence, and releases the view if it
    // stops advancing -- so this must tick even though the pose is constant.
    ++g_headPoseSequence;
}
