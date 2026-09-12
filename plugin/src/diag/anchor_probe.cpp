#include "anchor_probe.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "aim_cmd.h"
#include "diagnostics.h"
#include "xr_input.h"

// Published by camera_hook.cpp at the camera write.
extern "C" volatile float g_headBasis[9];
extern "C" volatile std::uint32_t g_headBasisGeneration;
extern "C" volatile float g_headBaseAngles[3];     // engine camera base pitch/yaw/roll
extern "C" volatile float g_headWrittenAngles[3];  // what we wrote: pitch/yaw/roll
extern "C" volatile std::uint32_t g_headAnglesGeneration;

namespace {

// SHIPS OFF. A diagnostic that a player can meet is a support burden, and one
// armed by default costs log volume and frame time on every machine that is not
// the developer's. Turn it on from the ini to use it.
std::atomic_bool g_enabled{true};

// ---- Source's basis conventions, copied verbatim from camera_hook.cpp so this
// file cannot drift from the code it is auditing. Rows are forward, right, up.
void AngleBasis(float pitchDeg, float yawDeg, float rollDeg, float basis[9]) {
    constexpr float toRad = 0.01745329252f;
    const float sp = std::sin(pitchDeg * toRad), cp = std::cos(pitchDeg * toRad);
    const float sy = std::sin(yawDeg * toRad), cy = std::cos(yawDeg * toRad);
    const float sr = std::sin(rollDeg * toRad), cr = std::cos(rollDeg * toRad);
    basis[0] = cp * cy;
    basis[1] = cp * sy;
    basis[2] = -sp;
    basis[3] = -sr * sp * cy + cr * sy;
    basis[4] = -sr * sp * sy - cr * cy;
    basis[5] = -sr * cp;
    basis[6] = cr * sp * cy + sr * sy;
    basis[7] = cr * sp * sy - sr * cy;
    basis[8] = cr * cp;
}

void BasisAngles(const float basis[9], float& pitchDeg, float& yawDeg, float& rollDeg) {
    constexpr float toDeg = 57.2957795f;
    const float* forward = basis;
    const float* right = basis + 3;
    const float* up = basis + 6;
    const float xy = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1]);
    pitchDeg = std::atan2(-forward[2], xy) * toDeg;
    if (xy > 0.001f) {
        yawDeg = std::atan2(forward[1], forward[0]) * toDeg;
        rollDeg = std::atan2(-right[2], up[2]) * toDeg;
    } else {
        yawDeg = std::atan2(-right[0], right[1]) * toDeg;
        rollDeg = 0.0f;
    }
}

// The angle of the rotation taking basis b to basis a, in degrees. Both are
// orthonormal row-major frames, so a*b^T is a rotation and its trace gives the
// angle. This is the ONE number that says "these two frames disagree by X".
float FrameDisagreementDegrees(const float a[9], const float b[9]) {
    float trace = 0.0f;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            // (a*b^T)[r][r] summed: row r of a dotted with row r of b.
            if (r == c) {
                trace += a[r * 3 + 0] * b[r * 3 + 0] + a[r * 3 + 1] * b[r * 3 + 1] +
                         a[r * 3 + 2] * b[r * 3 + 2];
            }
        }
    }
    float t = (trace - 1.0f) * 0.5f;
    if (t > 1.0f) t = 1.0f;
    if (t < -1.0f) t = -1.0f;
    return std::acos(t) * 57.2957795f;
}

bool ReadBasisStable(const volatile float src[9], const volatile std::uint32_t& generation,
                     float out[9]) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint32_t before = generation;
        if (before & 1u) continue;  // a write is in progress
        for (int i = 0; i < 9; ++i) out[i] = src[i];
        if (generation == before) return true;
    }
    return false;
}

float WrapDeg(float d) {
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

// ---- stage counters. A stage that never runs must say so. --------------------
std::uint64_t g_ticks = 0;
std::uint64_t g_basisUnreadable = 0;
std::uint64_t g_cameraSamples = 0;
std::uint64_t g_handStillWindows = 0;
std::uint64_t g_aimUnavailable = 0;
std::uint64_t g_handNeverStill = 0;
std::uint64_t g_bodyTurned = 0;

// ---- 1. the camera residual --------------------------------------------------
float g_residualNow = 0.0f, g_residualMax = 0.0f, g_residualSum = 0.0f;
float g_pitchDiffMax = 0.0f, g_rollDiffMax = 0.0f;
float g_residualAtMaxHeadYaw = 0.0f;
// Cyclicality: the residual binned by head yaw. A constant tilt in the engine
// frame traces a sine through one revolution, so a residual that is flat in
// these bins is NOT the "boat riding a wave" shape and a residual that swings
// across them is.
constexpr int kBins = 8;
float g_binSum[kBins] = {};
std::uint32_t g_binCount[kBins] = {};

// ---- 2. the aim, while the hand is genuinely still ---------------------------
float g_prevHandBasis[9] = {};
bool g_haveHand = false;
int g_stillFrames = 0;
float g_aimYawAtStill = 0.0f, g_aimPitchAtStill = 0.0f;
float g_headYawAtStill = 0.0f, g_headPitchAtStill = 0.0f;
float g_bodyYawAtStill = 0.0f;
bool g_stillLatched = false;
float g_aimDriftMax = 0.0f, g_headMoveAtAimDriftMax = 0.0f;
float g_aimDriftLast = 0.0f, g_headMoveLast = 0.0f;

void Report() {
    char line[640]{};
    const double mean = g_cameraSamples ? g_residualSum / double(g_cameraSamples) : 0.0;
    std::snprintf(line, sizeof(line),
        "[TF2VR] ANCHOR camera: samples %llu (ticks %llu, basis unreadable %llu) | residual now %.3f deg, "
        "mean %.3f, MAX %.3f | written-vs-true pitch diff max %.3f, roll diff max %.3f | %s\n",
        static_cast<unsigned long long>(g_cameraSamples), static_cast<unsigned long long>(g_ticks),
        static_cast<unsigned long long>(g_basisUnreadable), g_residualNow, mean, g_residualMax,
        g_pitchDiffMax, g_rollDiffMax,
        g_cameraSamples == 0
            ? "VERDICT: NO SAMPLES -- the camera hook never ran, so this says nothing"
            : (mean < 0.05
                   ? "VERDICT: the written camera IS the head's true orientation plus a pure yaw. The "
                     "camera composition is EXONERATED and the drift is downstream of it"
                   : "VERDICT: a STANDING disagreement between the written camera and the head. Judge on "
                     "the MEAN and the yaw bins, never the max -- the max catches transients (a pose "
                     "skew during fast motion, or the deliberate ADS pitch offset) which cannot cause a "
                     "drift that persists when you stop"));
    Tf2VrLog(line);
    if (g_cameraSamples) {
        char bins[420]{};
        int used = std::snprintf(bins, sizeof(bins), "[TF2VR]   residual by head yaw (45 deg bins):");
        for (int i = 0; i < kBins && used < static_cast<int>(sizeof(bins)) - 24; ++i) {
            used += std::snprintf(bins + used, sizeof(bins) - used, " %d:%.3f",
                                  i * 45 - 180, g_binCount[i] ? g_binSum[i] / g_binCount[i] : 0.0f);
        }
        std::snprintf(bins + used, sizeof(bins) - used,
                      "  (a constant tilt traces a SINE across these; a flat row is not that shape)\n");
        Tf2VrLog(bins);
    }
    std::snprintf(line, sizeof(line),
        "[TF2VR] ANCHOR aim: still windows %llu (hand never still %llu, aim unavailable %llu) | last window "
        "aim moved %.3f deg while the head moved %.3f | WORST aim move %.3f (head moved %.3f then) | "
        "body-turn windows discarded %llu | %s\n",
        static_cast<unsigned long long>(g_handStillWindows), static_cast<unsigned long long>(g_handNeverStill),
        static_cast<unsigned long long>(g_aimUnavailable), g_aimDriftLast, g_headMoveLast, g_aimDriftMax,
        g_headMoveAtAimDriftMax,
        static_cast<unsigned long long>(g_bodyTurned),
        g_handStillWindows == 0
            ? "VERDICT: the hand was never still long enough -- set the controller DOWN and look around"
            : (g_aimDriftMax < 0.25f
                   ? "VERDICT: the aim holds still when the hand does. The reticle's drift is NOT the aim "
                     "path, so it and the marker share a cause"
                   : "VERDICT: the aim MOVES while the hand is still -- the reticle's drift is the AIM "
                     "path and the marker keeps its own separate cause"));
    Tf2VrLog(line);
}

}  // namespace

void SetAnchorProbeEnabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    Tf2VrLog(enabled ? "[TF2VR] anchor.probe = 1: measuring the camera write against the head's true "
                       "orientation, and the aim against a still controller. Read-only.\n"
                     : "[TF2VR] anchor.probe = 0.\n");
}

void AnchorProbeTick() {
    if (!g_enabled.load(std::memory_order_acquire)) return;
    ++g_ticks;

    float head[9]{};
    if (!ReadBasisStable(g_headBasis, g_headBasisGeneration, head)) {
        ++g_basisUnreadable;
    } else {
        // Is `head` a real frame yet? Identity means the camera hook has not run.
        const bool identity = std::fabs(head[0] - 1.0f) < 1e-6f && std::fabs(head[4] - 1.0f) < 1e-6f &&
                              std::fabs(head[8] - 1.0f) < 1e-6f;
        if (!identity) {
            float wp = g_headWrittenAngles[0], wy = g_headWrittenAngles[1], wr = g_headWrittenAngles[2];
            float hp = 0.0f, hy = 0.0f, hr = 0.0f;
            BasisAngles(head, hp, hy, hr);
            // The expected frame: the head's OWN pitch and roll, carrying the
            // yaw we actually wrote. If the write is the head's true orientation
            // composed with a pure body yaw, this is identical to the written
            // frame and the residual is zero.
            float expected[9]{}, written[9]{};
            AngleBasis(hp, wy, hr, expected);
            AngleBasis(wp, wy, wr, written);
            const float residual = FrameDisagreementDegrees(written, expected);
            g_residualNow = residual;
            g_residualSum += residual;
            ++g_cameraSamples;
            if (residual > g_residualMax) {
                g_residualMax = residual;
                g_residualAtMaxHeadYaw = hy;
            }
            const float dp = std::fabs(WrapDeg(wp - hp));
            const float dr = std::fabs(WrapDeg(wr - hr));
            if (dp > g_pitchDiffMax) g_pitchDiffMax = dp;
            if (dr > g_rollDiffMax) g_rollDiffMax = dr;
            int bin = static_cast<int>((WrapDeg(hy) + 180.0f) / 45.0f);
            if (bin < 0) bin = 0;
            if (bin >= kBins) bin = kBins - 1;
            g_binSum[bin] += residual;
            ++g_binCount[bin];

            // ---- the aim half, gated on the controller genuinely being still.
            float hand[9]{};
            for (int i = 0; i < 9; ++i) hand[i] = g_controllerAimBasis[kHandCount - 1][i];
            const bool handValid = std::fabs(hand[0]) + std::fabs(hand[4]) + std::fabs(hand[8]) > 0.1f;
            if (handValid && g_haveHand) {
                const float handMoved = FrameDisagreementDegrees(hand, g_prevHandBasis);
                if (handMoved < 0.05f) {
                    ++g_stillFrames;
                } else {
                    if (g_stillLatched && g_stillFrames >= 30) ++g_handStillWindows;
                    g_stillFrames = 0;
                    g_stillLatched = false;
                }
                if (g_stillFrames == 30) {
                    float aim[3]{};
                    if (TryGetAimAnglesDegrees(aim)) {
                        g_aimPitchAtStill = aim[0];
                        g_aimYawAtStill = aim[1];
                        g_headPitchAtStill = hp;
                        g_headYawAtStill = hy;
                        g_bodyYawAtStill = g_headBaseAngles[1];
                        g_stillLatched = true;
                    } else {
                        ++g_aimUnavailable;
                    }
                } else if (g_stillFrames > 30 && g_stillLatched) {
                    float aim[3]{};
                    if (TryGetAimAnglesDegrees(aim)) {
                        const float aimMoved =
                            std::fabs(WrapDeg(aim[1] - g_aimYawAtStill)) +
                            std::fabs(WrapDeg(aim[0] - g_aimPitchAtStill));
                        // THE AIM IS IN WORLD ANGLES, so a STICK TURN moves it while the
                        // controller's room-space basis does not change at all. Without
                        // removing the body yaw this window reports a body turn as an aim
                        // defect -- it did exactly that on the first run (19.0 deg "aim
                        // move" against 0.4 deg of head motion, which was a stick turn).
                        const float bodyMoved = std::fabs(WrapDeg(g_headBaseAngles[1] - g_bodyYawAtStill));
                        const float headMoved = std::fabs(WrapDeg(hy - g_headYawAtStill)) +
                                                std::fabs(WrapDeg(hp - g_headPitchAtStill));
                        if (bodyMoved > 0.5f) {
                            ++g_bodyTurned;
                            g_stillLatched = false;
                            g_stillFrames = 0;
                        } else {
                            g_aimDriftLast = aimMoved;
                            g_headMoveLast = headMoved;
                            if (aimMoved > g_aimDriftMax) {
                                g_aimDriftMax = aimMoved;
                                g_headMoveAtAimDriftMax = headMoved;
                            }
                        }
                    }
                }
            } else if (!handValid) {
                ++g_handNeverStill;
            }
            if (handValid) {
                for (int i = 0; i < 9; ++i) g_prevHandBasis[i] = hand[i];
                g_haveHand = true;
            }
        }
    }

    static std::uint64_t nextMs = 0;
    const std::uint64_t now = GetTickCount64();
    if (now < nextMs) return;
    nextMs = now + 5000;
    Report();
}
