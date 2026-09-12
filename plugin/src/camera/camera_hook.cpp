#include "camera_hook.h"
#include "player_eye.h"
#include "player_eye_hook.h"

#include "camera_update_hook.h"
#include "diagnostics.h"
#include "hook_registry.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" volatile std::uintptr_t g_cameraStructAddress = 0;
extern "C" volatile std::uintptr_t g_cameraAnglesAddress = 0;
extern "C" volatile std::uint64_t g_cameraHookCallCount = 0;
extern "C" volatile std::uint32_t g_cameraBasePitchBits = 0;
extern "C" volatile std::uint32_t g_cameraBaseYawBits = 0;
extern "C" volatile std::uint32_t g_cameraBaseRollBits = 0;
extern "C" volatile std::uint32_t g_cameraDesiredPitchBits = 0;
extern "C" volatile std::uint32_t g_cameraDesiredYawBits = 0;
// The yaw DELTA the detour adds to the base it latches on the RENDER frame.
// Publishing an absolute yaw instead paired this frame's write with an older
// frame's base, an error proportional to turn speed. See camera_hook.asm.
extern "C" volatile std::uint32_t g_cameraYawDeltaBits = 0;
extern "C" volatile std::uint32_t g_cameraDesiredRollBits = 0;
// What the detour actually left in the destination this frame, latched beside
// the base in the same block. The viewmodel correction consumes this pair.
extern "C" volatile std::uint32_t g_cameraAppliedPitchBits = 0;
extern "C" volatile std::uint32_t g_cameraAppliedYawBits = 0;
extern "C" volatile std::uint32_t g_cameraAppliedRollBits = 0;
// Seqlock generation over the base/applied pair, bumped by the detour.
extern "C" volatile std::uint32_t g_cameraAngleGeneration = 0;
// Positional head tracking, same latch-then-compose pattern as the angles.
extern "C" volatile std::uint32_t g_cameraBasePosXBits = 0;
extern "C" volatile std::uint32_t g_cameraBasePosYBits = 0;
extern "C" volatile std::uint32_t g_cameraBasePosZBits = 0;
extern "C" volatile std::uint32_t g_cameraDesiredPosXBits = 0;
extern "C" volatile std::uint32_t g_cameraDesiredPosYBits = 0;
extern "C" volatile std::uint32_t g_cameraDesiredPosZBits = 0;
// The head's offset alone, in game space. The detour adds these to the position
// it has just latched, so the sum cannot span two frames. The Desired triple
// above is the old form, which did span two and produced the stair lurch.
extern "C" volatile std::uint32_t g_cameraOffsetXBits = 0;
extern "C" volatile std::uint32_t g_cameraOffsetYBits = 0;
extern "C" volatile std::uint32_t g_cameraOffsetZBits = 0;
extern "C" volatile std::uint8_t g_cameraPositionWriteActive = 0;
// Head position in metres, OpenXR axes, published by xr_context.
extern "C" volatile float g_headPositionMetres[3] = {0.0f, 0.0f, 0.0f};
// The head's orientation as a basis in Source axes, rows forward/right/up,
// published by xr_context between two generation bumps.
extern "C" volatile float g_headBasis[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
extern "C" volatile std::uint32_t g_headBasisGeneration = 0;
// The head's rotation RELATIVE to the recentre reference, as a basis. This is
// exactly what the viewmodel pass has to cancel, so publishing it removes the
// decode-recompose round trip the correction used to do.
extern "C" volatile float g_headDeltaBasis[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
extern "C" volatile std::uint32_t g_headDeltaGeneration = 0;
// Published by xr_context each time the headset pose is located.
extern "C" volatile float g_headYawDegrees = 0.0f;
extern "C" volatile float g_headPitchDegrees = 0.0f;
extern "C" volatile float g_headRollDegrees = 0.0f;
extern "C" volatile std::uint8_t g_headPoseValid = 0;
extern "C" volatile std::uint64_t g_headPoseSequence = 0;
// The yaw the head is currently adding to the engine's view, published for the
// viewmodel pass to cancel. Consumed by camera_update_hook.
extern "C" volatile float g_headDeltaYawDegrees = 0.0f;
// The engine's own angles for this frame, and the angles we actually wrote.
// The viewmodel pass needs the rotation between them, and deriving it from
// these two complete orientations keeps the compose in ONE algebra. Applying
// yaw about world Z and pitch about the camera right axis as separate steps is
// how BioShock ended up with 28 degrees of divergence across three algebras
// (RESEARCH-VR-PRIOR-ART 1.4); they fixed it by composing once.
// Order: pitch, yaw, roll (degrees), matching the engine's own layout.
extern "C" volatile float g_headBaseAngles[3] = {0.0f, 0.0f, 0.0f};
extern "C" volatile float g_headWrittenAngles[3] = {0.0f, 0.0f, 0.0f};
// Seqlock over the pair above, bumped once per frame in AdvanceHeadTracking.
extern "C" volatile std::uint32_t g_headAnglesGeneration = 0;
// Zero while head tracking is not driving the view, so the viewmodel pass
// cannot cancel a rotation that is not being applied.
extern "C" volatile std::uint8_t g_headCompensationValid = 0;
extern "C" volatile std::uint8_t g_cameraYawWriteActive = 0;
extern "C" volatile std::uintptr_t g_cameraSourceAnglesAddress = 0;
extern "C" volatile std::uint32_t g_cameraSourceBaseYawBits = 0;
extern "C" volatile std::uint32_t g_cameraSourceDesiredYawBits = 0;
extern "C" volatile std::uint8_t g_cameraSourceYawWriteActive = 0;
extern "C" std::uintptr_t g_cameraStructInterceptionContinue = 0;
extern "C" volatile std::uint8_t g_cameraWriteBlock = 0;
extern "C" void cameraStructInterceptor();

namespace {
constexpr std::uint8_t kPattern[] = {
    0x89, 0x0E, 0x8B, 0x48, 0x04, 0x89, 0x4E, 0x04, 0x8B, 0x40, 0x08, 0x48,
    0x8B, 0xCB, 0x89, 0x46, 0x08, 0x48, 0x8B, 0x03, 0xFF, 0x90, 0, 0, 0, 0,
    0x8B, 0x08, 0x89, 0x0F, 0x8B, 0x48, 0x04, 0x89, 0x4F, 0x04, 0x8B, 0x40,
    0x08, 0x89, 0x47, 0x08, 0xE8, 0, 0, 0, 0, 0x33, 0xD2,
};
constexpr bool kWildcard[] = {
    false,false,false,false,false,false,false,false,false,false,false,false,
    false,false,false,false,false,false,false,false,false,false,true,true,true,true,
    false,false,false,false,false,false,false,false,false,false,false,false,false,false,
    false,false,false,true,true,true,true,false,false,
};
constexpr size_t kDisplacedBytes = 42;
std::uint8_t* g_patchSite = nullptr;
std::uint8_t g_original[kDisplacedBytes]{};
std::atomic_bool g_snapshotRequested = false;
std::atomic_uint32_t g_angleTraceFramesRemaining = 0;
std::atomic_bool g_nudgeRequested = false;
std::atomic_uint32_t g_nudgeFramesRemaining = 0;
float g_nudgeTargetX = 0.0f;

bool IsReadable(const void* address, size_t size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!address || !VirtualQuery(address, &info, sizeof(info))) return false;
    const DWORD protection = info.Protect & 0xFF;
    return info.State == MEM_COMMIT && protection != PAGE_NOACCESS && protection != PAGE_GUARD &&
        size <= static_cast<size_t>(reinterpret_cast<const std::uint8_t*>(info.BaseAddress) + info.RegionSize - reinterpret_cast<const std::uint8_t*>(address));
}

std::uint8_t* FindUniquePattern(HMODULE module) {
    auto* base = reinterpret_cast<std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const size_t imageSize = nt->OptionalHeader.SizeOfImage;
    std::uint8_t* found = nullptr;
    for (size_t i = 0; i + sizeof(kPattern) <= imageSize; ++i) {
        bool matches = true;
        for (size_t j = 0; j < sizeof(kPattern); ++j) {
            if (!kWildcard[j] && base[i + j] != kPattern[j]) { matches = false; break; }
        }
        if (!matches) continue;
        if (found) return nullptr; // ambiguous is unsafe; never patch.
        found = base + i;
    }
    return found;
}
}

void EnsureCameraHookInstalled() {
    if (g_patchSite) return;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return;
    // LATCH THE FAILURE. This runs once a FRAME from RunFrame, and without the
    // latch a pattern that stops matching -- a game patch, a different build --
    // turns into a full client.dll scan plus a log line EVERY FRAME, forever.
    // That is the exact shape of the cvar probe that put 5850 ms into level
    // entry, and EnsureXInputBridgeInstalled was given the same latch for the
    // same reason. Found by the 2026-08-24 hot-path audit; free today because
    // the pattern currently matches, which is precisely why nobody noticed.
    static bool patternFailed = false;
    if (patternFailed) return;
    std::uint8_t* site = FindUniquePattern(client);
    if (!site) {
        patternFailed = true;
        Tf2VrLog("[TF2VR] Camera AOB had zero or multiple matches; read-only hook not installed. "
                 "NOT retried -- rescanning client.dll every frame is how a missing pattern becomes "
                 "a freeze.\n");
        return;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(site, kDisplacedBytes, PAGE_EXECUTE_READWRITE, &oldProtect)) { Tf2VrLog("[TF2VR] Camera hook VirtualProtect failed.\n"); return; }
    std::memcpy(g_original, site, kDisplacedBytes);
    std::uint8_t detour[14] = {0xFF, 0x25, 0, 0, 0, 0};
    const auto target = reinterpret_cast<std::uintptr_t>(&cameraStructInterceptor);
    std::memcpy(detour + 6, &target, sizeof(target));
    g_cameraStructInterceptionContinue = reinterpret_cast<std::uintptr_t>(site + kDisplacedBytes);
    std::memcpy(site, detour, sizeof(detour));
    std::memset(site + sizeof(detour), 0x90, kDisplacedBytes - sizeof(detour));
    FlushInstructionCache(GetCurrentProcess(), site, kDisplacedBytes);
    DWORD ignored = 0; VirtualProtect(site, kDisplacedBytes, oldProtect, &ignored);
    g_patchSite = site;
    RegisterHookSite("camera-struct AOB patched bytes", site, kDisplacedBytes);
    Tf2VrLog("[TF2VR] Read-only camera pointer detour installed. F8 requests one snapshot.\n");
}

void RemoveCameraHook() {
    if (!g_patchSite) return;
    DWORD oldProtect = 0;
    if (VirtualProtect(g_patchSite, kDisplacedBytes, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::memcpy(g_patchSite, g_original, kDisplacedBytes);
        FlushInstructionCache(GetCurrentProcess(), g_patchSite, kDisplacedBytes);
        DWORD ignored = 0; VirtualProtect(g_patchSite, kDisplacedBytes, oldProtect, &ignored);
    }
    // Release the yaw offset before the detour goes away, so an unload during
    // an armed test cannot leave the engine's angles permanently displaced.
    g_cameraYawWriteActive = 0;
    g_cameraYawDeltaBits = 0;
    g_cameraSourceYawWriteActive = 0;
    g_patchSite = nullptr; g_cameraStructAddress = 0; g_cameraAnglesAddress = 0;
    g_cameraSourceAnglesAddress = 0;
    g_cameraStructInterceptionContinue = 0;
}

void RequestCameraSnapshot() { g_snapshotRequested.store(true, std::memory_order_release); }

void TryLogRequestedCameraSnapshot() {
    if (!g_snapshotRequested.load(std::memory_order_acquire)) return;
    const auto address = reinterpret_cast<const float*>(g_cameraStructAddress);
    if (!IsReadable(address, sizeof(float) * 6)) return;
    float values[6]{}; std::memcpy(values, address, sizeof(values));
    char line[256]{};
    std::snprintf(line, sizeof(line), "[TF2VR] Camera snapshot %p: pos=(%.3f, %.3f, %.3f) angles=(%.3f, %.3f, %.3f)\n",
        address, values[0], values[1], values[2], values[3], values[4], values[5]);
    Tf2VrLog(line);
    g_snapshotRequested.store(false, std::memory_order_release);
}

// Task 01 stage 1: observation only. Nothing is written to game memory; this
// samples the view-angle state the engine has already finished computing for
// the frame, so it cannot perturb what it measures.
//
// It exists to answer three questions before any write is attempted:
//   1. Is the angle destination really g_cameraStructAddress+0x0C? (delta)
//   2. Do the angles track mouse look, i.e. is this the live view? (angles)
//   3. What is the resting noise floor, so a later feedback-driven drift is
//      distinguishable from ordinary jitter? (a still-stick sample)
constexpr std::uint32_t kAngleTraceFrames = 300;
constexpr std::uint32_t kAngleTraceInterval = 12;
std::uint64_t g_lastTraceCallCount = 0;

// The game only turns for mouse input while it owns the foreground. Our
// hotkeys use GetAsyncKeyState, which is process-wide and fires even when the
// game is not focused, so "F8 was received" is not evidence the game was
// listening to the mouse. Checking this here keeps that ambiguity out of the
// result rather than leaving it to be argued about afterwards.
bool GameHasForeground() {
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId();
}

void RequestCameraAngleTrace() {
    g_lastTraceCallCount = g_cameraHookCallCount;
    g_angleTraceFramesRemaining.store(kAngleTraceFrames, std::memory_order_release);
}

void AdvanceCameraAngleTrace() {
    const std::uint32_t remaining = g_angleTraceFramesRemaining.load(std::memory_order_acquire);
    if (!remaining) return;
    g_angleTraceFramesRemaining.store(remaining - 1, std::memory_order_release);

    const std::uint32_t index = kAngleTraceFrames - remaining;
    if (index % kAngleTraceInterval != 0) return;

    const auto position = reinterpret_cast<const float*>(g_cameraStructAddress);
    const auto angles = reinterpret_cast<const float*>(g_cameraAnglesAddress);
    // Report the raw pointers on failure. The first run of this trace could not
    // tell "the hook never ran" from "the hook ran but the address is
    // unreadable", and the true cause was a third thing entirely: a stale
    // orphan .asm that was not in the build. A diagnostic that cannot name its
    // own failure mode sends you looking at the game instead of the build.
    if (!IsReadable(position, sizeof(float) * 3) || !IsReadable(angles, sizeof(float) * 3)) {
        char failure[256]{};
        std::snprintf(failure, sizeof(failure),
            "[TF2VR] F8 angle trace: unusable pointers pos=%p (%s) angles=%p (%s).\n",
            reinterpret_cast<const void*>(position),
            IsReadable(position, sizeof(float) * 3) ? "readable" : (position ? "unreadable" : "null"),
            reinterpret_cast<const void*>(angles),
            IsReadable(angles, sizeof(float) * 3) ? "readable" : (angles ? "unreadable" : "null"));
        Tf2VrLog(failure);
        return;
    }
    // Signed, because a negative or unexpected delta immediately falsifies the
    // "angles live at +0x0C of the position struct" claim taken from IGCS.
    const auto delta = static_cast<std::intptr_t>(g_cameraAnglesAddress) -
                       static_cast<std::intptr_t>(g_cameraStructAddress);
    const std::uint64_t calls = g_cameraHookCallCount;
    const std::uint64_t callsSince = calls - g_lastTraceCallCount;
    g_lastTraceCallCount = calls;

    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F8 angle trace %u/%u: hook calls since last sample=%llu (total=%llu) fg=%s "
        "pos=%p angles=%p delta=%lld pos=(%.3f, %.3f, %.3f) "
        "angles(pitch,yaw,roll)=(%.3f, %.3f, %.3f)\n",
        index, kAngleTraceFrames, static_cast<unsigned long long>(callsSince),
        static_cast<unsigned long long>(calls), GameHasForeground() ? "yes" : "no",
        reinterpret_cast<const void*>(position),
        reinterpret_cast<const void*>(angles), static_cast<long long>(delta),
        position[0], position[1], position[2], angles[0], angles[1], angles[2]);
    Tf2VrLog(line);

    if (remaining == 1) Tf2VrLog("[TF2VR] F8 angle trace complete.\n");
}

// Task 01 stage 2: the first write to the engine's view angles.
//
// Deliberately a fixed offset rather than the headset pose. A head-driven
// write conflates two failures -- "the write does not reach the view" and
// "the pose plumbing is wrong" -- and this stage exists to answer only the
// first. It also holds the offset constant so any rotation that accumulates
// is unambiguously the engine's, not ours.
//
// Bounded and self-reverting: the offset is applied for a fixed number of
// frames and then released, so the worst case is a few seconds of a view
// turned 15 degrees, never a stuck camera.
constexpr float kYawTestOffsetDegrees = 15.0f;
// Was 240 frames (~2s). That was too short to judge the HUD by eye, and the
// conclusion drawn from it was reported as uncertain by the tester -- rightly,
// since a two-second glimpse is not evidence. Both write tests now run long
// and toggle off, so the two levers are compared under identical conditions.
constexpr std::uint32_t kYawTestFrames = 1800;
std::atomic_uint32_t g_yawTestFramesRemaining = 0;
float g_yawTestFirstBase = 0.0f;
bool g_haveYawTestFirstBase = false;

float BitsToFloat(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::uint32_t FloatToBits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// static: this is a private copy, and xr_input.h has its own `inline` one at
// global scope. Release folded them and Debug did not --
//   camera_hook.cpp.obj : error LNK2005: "float __cdecl WrapDegrees(float)"
//   already defined in xr_input.obj
// -- so Debug had not linked for some time. Nobody noticed because Release is
// what ships, but a public repo whose Debug configuration does not build is a
// bad first impression. Internal linkage here rather than deleting one copy:
// the two are identical today, and this file's is used by the bounded yaw
// tests, which should not start depending on the XR header.
static float WrapDegrees(float value) {
    while (value > 180.0f) value -= 360.0f;
    while (value < -180.0f) value += 360.0f;
    return value;
}

// --- Orientation algebra -------------------------------------------------
//
// Row-major 3x3 whose ROWS are the basis vectors forward, right, up -- the
// layout Source's own AngleVectors produces. Every orientation in the head
// chain is carried in this form and composed as a matrix. Nothing adds Euler
// angles: that is only valid when the rotations share an axis frame, which yaw
// does (it is outermost) and pitch does not, so adding a head yaw onto a
// non-zero base pitch rotates about the wrong axis. Level ground hid it; the
// failure shows up as soon as the engine's base pitch is significant.

// Source's AngleVectors, in Source's order and signs.
void AngleBasis(float pitchDeg, float yawDeg, float rollDeg, float basis[9]) {
    constexpr float toRad = 0.01745329252f;
    const float sp = std::sin(pitchDeg * toRad), cp = std::cos(pitchDeg * toRad);
    const float sy = std::sin(yawDeg * toRad), cy = std::cos(yawDeg * toRad);
    const float sr = std::sin(rollDeg * toRad), cr = std::cos(rollDeg * toRad);
    basis[0] = cp * cy;                    // forward
    basis[1] = cp * sy;
    basis[2] = -sp;
    basis[3] = -sr * sp * cy + cr * sy;    // right
    basis[4] = -sr * sp * sy - cr * cy;
    basis[5] = -sr * cp;
    basis[6] = cr * sp * cy + sr * sy;     // up
    basis[7] = cr * sp * sy - sr * cy;
    basis[8] = cr * cp;
}

// Source's MatrixAngles: the exact inverse of AngleBasis. The degenerate branch
// covers looking straight up or down, where yaw and roll are not separable.
void BasisAngles(const float basis[9], float& pitchDeg, float& yawDeg, float& rollDeg) {
    constexpr float toDeg = 57.2957795f;
    const float* forward = basis;
    const float* right = basis + 3;
    const float* up = basis + 6;
    const float xyDistance = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1]);
    pitchDeg = std::atan2(-forward[2], xyDistance) * toDeg;
    if (xyDistance > 0.001f) {
        yawDeg = std::atan2(forward[1], forward[0]) * toDeg;
        // left = -right
        rollDeg = std::atan2(-right[2], up[2]) * toDeg;
    } else {
        yawDeg = std::atan2(right[0], -right[1]) * toDeg;
        rollDeg = 0.0f;
    }
}

void BasisMultiply(const float a[9], const float b[9], float out[9]) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            out[i * 3 + j] = a[i * 3 + 0] * b[0 * 3 + j] +
                             a[i * 3 + 1] * b[1 * 3 + j] +
                             a[i * 3 + 2] * b[2 * 3 + j];
        }
    }
}

// a * b^T.
//
// These bases are stored with the basis vectors as ROWS, which makes each one
// the transpose of the local-to-world rotation it represents. So a composition
// that reads R_view = R_body * R_delta in ordinary rotation-matrix terms comes
// out here as written = delta * base, and the head's rotation since recentring
// comes out as head * reference^T. Both are the reverse of the obvious reading,
// and getting either backwards turns a pure pitch into yaw and roll.
//
// Checked against a worked case rather than argued: body yawed 90 degrees, head
// pitched up 30. The correct pair returns yaw 90, pitch -30, roll 0. The
// reversed pair returns yaw -90, pitch 0.
void BasisMultiplyTransposeRight(const float a[9], const float b[9], float out[9]) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            out[i * 3 + j] = a[i * 3 + 0] * b[j * 3 + 0] +
                             a[i * 3 + 1] * b[j * 3 + 1] +
                             a[i * 3 + 2] * b[j * 3 + 2];
        }
    }
}

// Reads a basis published between two generation bumps, retrying if it caught a
// torn write. A torn basis is not a slightly wrong rotation, it is not a
// rotation at all.
bool ReadPublishedBasis(volatile const float source[9], volatile const std::uint32_t& generation,
                        float out[9]) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint32_t before = generation;
        if (before & 1u) continue;                 // a write is in progress
        for (int i = 0; i < 9; ++i) out[i] = source[i];
        if (generation == before) return true;
    }
    return false;
}

void RequestBoundedYawWriteTest() {
    if (g_yawTestFramesRemaining.load(std::memory_order_acquire)) {
        g_yawTestFramesRemaining.store(0, std::memory_order_release);
        g_cameraYawWriteActive = 0;
        g_cameraYawDeltaBits = 0;
        Tf2VrLog("[TF2VR] F12: yaw write released early by second press.\n");
        return;
    }
    if (!g_cameraStructAddress) {
        Tf2VrLog("[TF2VR] F12 yaw write: camera hook has not run yet; not arming.\n");
        return;
    }
    g_haveYawTestFirstBase = false;
    g_yawTestFramesRemaining.store(kYawTestFrames, std::memory_order_release);
    char line[224]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F12: bounded DESTINATION yaw write armed, %+.1f deg for %u frames (~30s), "
        "press F12 again to release early.\n",
        kYawTestOffsetDegrees, kYawTestFrames);
    Tf2VrLog(line);
}

void AdvanceBoundedYawWriteTest() {
    const std::uint32_t remaining = g_yawTestFramesRemaining.load(std::memory_order_acquire);
    if (!remaining) return;

    // The base is the engine's own pre-write yaw, captured in the detour. The
    // offset is composed onto that, never onto our previous output, so this
    // cannot manufacture the runaway it is looking for.
    const float base = BitsToFloat(g_cameraBaseYawBits);
    g_cameraDesiredYawBits = FloatToBits(WrapDegrees(base + kYawTestOffsetDegrees));
    // The detour adds this to the base it latches itself, so the test offset
    // is a DELTA now. Same value, paired with the right frame.
    g_cameraYawDeltaBits = FloatToBits(kYawTestOffsetDegrees);
    g_cameraYawWriteActive = 1;

    // The detour now writes all three components together, so pitch and roll
    // must be restated as their own base or the fixed yaw test would flatten
    // them to zero.
    g_cameraDesiredPitchBits = g_cameraBasePitchBits;
    g_cameraDesiredRollBits = g_cameraBaseRollBits;

    const std::uint32_t index = kYawTestFrames - remaining;
    if (!g_haveYawTestFirstBase) { g_yawTestFirstBase = base; g_haveYawTestFirstBase = true; }

    if (index % 60 == 0) {
        char line[256]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] F12 yaw write %u/%u: engine base yaw=%.3f (drift from first base=%+.3f) "
            "writing=%.3f\n",
            index, kYawTestFrames, base, WrapDegrees(base - g_yawTestFirstBase),
            WrapDegrees(base + kYawTestOffsetDegrees));
        Tf2VrLog(line);
    }

    g_yawTestFramesRemaining.store(remaining - 1, std::memory_order_release);
    if (remaining == 1) {
        g_cameraYawWriteActive = 0;
        g_cameraYawDeltaBits = 0;
        char line[192]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] F12 yaw write complete; offset released. Total engine base drift=%+.3f deg.\n",
            WrapDegrees(BitsToFloat(g_cameraBaseYawBits) - g_yawTestFirstBase));
        Tf2VrLog(line);
    }
}

// Task 01 stage 3: the same bounded test, one step upstream.
//
// This writes the engine's angle SOURCE rather than the camera destination.
// It is the more invasive of the two by design: this state plausibly also
// feeds aim and movement, so it is the one that could genuinely run away.
// Same bounding, same clean-base composition, same self-revert.
// Long enough to actually inspect the HUD rather than glimpse it: ~30s at
// 60fps. Still a hard cap, and F11 toggles it off early, so "bounded and
// self-reverting" is preserved -- the bound is just a usable length now.
constexpr std::uint32_t kSourceYawTestFrames = 1800;
std::atomic_uint32_t g_sourceYawTestFramesRemaining = 0;
float g_sourceYawTestFirstBase = 0.0f;
float g_sourceYawTestPreviousBase = 0.0f;
std::uint32_t g_sourceYawRunawayStreak = 0;
bool g_haveSourceYawTestFirstBase = false;

void RequestBoundedSourceYawWriteTest() {
    // Pressing F11 again releases immediately, so you are never waiting out
    // the timer if it looks wrong.
    if (g_sourceYawTestFramesRemaining.load(std::memory_order_acquire)) {
        g_sourceYawTestFramesRemaining.store(0, std::memory_order_release);
        g_cameraSourceYawWriteActive = 0;
        Tf2VrLog("[TF2VR] F11: UPSTREAM yaw write released early by second press.\n");
        return;
    }
    if (!g_cameraSourceAnglesAddress) {
        Tf2VrLog("[TF2VR] F11 source yaw write: source pointer not captured yet; not arming.\n");
        return;
    }
    g_haveSourceYawTestFirstBase = false;
    g_sourceYawTestFramesRemaining.store(kSourceYawTestFrames, std::memory_order_release);
    char line[256]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F11: bounded UPSTREAM yaw write armed, %+.1f deg for %u frames (~30s), "
        "press F11 again to release early. source=%p\n",
        kYawTestOffsetDegrees, kSourceYawTestFrames,
        reinterpret_cast<const void*>(g_cameraSourceAnglesAddress));
    Tf2VrLog(line);
}

void AdvanceBoundedSourceYawWriteTest() {
    const std::uint32_t remaining = g_sourceYawTestFramesRemaining.load(std::memory_order_acquire);
    if (!remaining) return;

    const float base = BitsToFloat(g_cameraSourceBaseYawBits);
    g_cameraSourceDesiredYawBits = FloatToBits(WrapDegrees(base + kYawTestOffsetDegrees));
    g_cameraSourceYawWriteActive = 1;

    const std::uint32_t index = kSourceYawTestFrames - remaining;
    if (!g_haveSourceYawTestFirstBase) {
        g_sourceYawTestFirstBase = base;
        g_sourceYawTestPreviousBase = base;
        g_sourceYawRunawayStreak = 0;
        g_haveSourceYawTestFirstBase = true;
    }

    const float drift = WrapDegrees(base - g_sourceYawTestFirstBase);
    if (index % 60 == 0) {
        char line[256]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] F11 source yaw write %u/%u: engine base yaw=%.3f (drift=%+.3f) writing=%.3f\n",
            index, kSourceYawTestFrames, base, drift, WrapDegrees(base + kYawTestOffsetDegrees));
        Tf2VrLog(line);
    }

    // Runaway abort. Total drift is the wrong test over a 30s run -- ordinary
    // mouse look covers far more than 90 degrees -- so this matches the
    // signature instead: feedback re-applies the offset every frame, so the
    // base advances by roughly kYawTestOffsetDegrees per frame, in a fixed
    // direction, and does not stop. Mouse look does not sustain that rate for
    // a third of a second, and a flick that briefly does will reverse or ease
    // off and reset the streak.
    const float step = WrapDegrees(base - g_sourceYawTestPreviousBase);
    g_sourceYawTestPreviousBase = base;
    const bool runawayStep = std::fabs(step) > kYawTestOffsetDegrees * 0.5f &&
                             (step > 0.0f) == (kYawTestOffsetDegrees > 0.0f);
    g_sourceYawRunawayStreak = runawayStep ? g_sourceYawRunawayStreak + 1 : 0;
    if (g_sourceYawRunawayStreak >= 20) {
        g_cameraSourceYawWriteActive = 0;
        g_sourceYawTestFramesRemaining.store(0, std::memory_order_release);
        char line[256]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] F11 source yaw write ABORTED at frame %u: base advanced %+.3f deg/frame in a "
            "fixed direction for 20 frames (total drift %+.3f). That is the feedback signature. "
            "Offset released.\n", index, step, drift);
        Tf2VrLog(line);
        return;
    }

    g_sourceYawTestFramesRemaining.store(remaining - 1, std::memory_order_release);
    if (remaining == 1) {
        g_cameraSourceYawWriteActive = 0;
        char line[192]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] F11 source yaw write complete; offset released. Total base drift=%+.3f deg.\n",
            drift);
        Tf2VrLog(line);
    }
}

// Task 01 stage 4: head tracking proper.
//
// Composed against the engine's own base every frame rather than replacing it,
// so mouse look keeps working underneath and the head contributes an offset on
// top. Yaw and pitch are RELATIVE to a reference captured when armed -- the
// game's heading is arbitrary and the headset's is not, so an absolute mapping
// would snap the view on arm. Roll is taken absolute because the game pins its
// own roll at 0.000 (measured), so there is no base to preserve.
//
// Signs are stated as named constants rather than folded into the expression:
// they are the most likely thing to be wrong on first run, and one test tells
// us all three.
// The three sign constants are gone. They existed to patch up an Euler
// decomposition that did not match the engine's convention; the orientation is
// now carried as a basis and composed as a matrix, so there is no per-axis sign
// left to get wrong.

std::atomic_bool g_headTrackingArmed = false;
// Positional head tracking rides with head tracking but is separable, because
// it is the one write that moves the player's view origin rather than only
// aiming it.
std::atomic_bool g_headPositionalArmed = true;
bool g_haveHeadReference = false;
// Set by the OpenXR reference-space-change event. The wearer long-presses the
// menu button, the runtime moves its own space under us, and our reference --
// captured once and never revisited -- is then stale by however far the
// recentre moved. That is exactly what "recentring did not change this" means.
std::atomic_bool g_recentreRequested = false;

// See GetHeadViewDelta in the header. Written at the compose site, read by the
// ADS alignment on another thread.
std::atomic<float> g_publishedHeadYawDelta{0.0f};
std::atomic<float> g_publishedHeadPitch{0.0f};
std::atomic_bool g_publishedHeadValid{false};
// See the note at the compose site. Set on the ADS transition and CLEARED on
// release, so it is zero at every moment outside ADS and nothing accumulates.
std::atomic<float> g_viewPitchOffset{0.0f};
// The two terms of the written pitch, published for the census. See the note at
// the composition site.
volatile float g_publishedPitchHead = 0.0f;
volatile float g_publishedPitchOffset = 0.0f;

void RequestHeadTrackingRecentreImpl() {
    g_recentreRequested.store(true, std::memory_order_release);
    Tf2VrLog("[TF2VR] Head tracking: the runtime recentred its reference space, so ours is "
             "dropped and re-captured on the next frame. Face the direction you want to be "
             "forward.\n");
}

float g_headYawReference = 0.0f;
float g_headPitchReference = 0.0f;
float g_headRollReference = 0.0f;
float g_headPositionReference[3] = {0.0f, 0.0f, 0.0f};
// ---------------------------------------------------------------------------
// THE STAGE MEASUREMENT. Read-only, and it drives NOTHING -- see below.
// ---------------------------------------------------------------------------

// The wearer's measured eye height above their real floor, from the runtime's
// STAGE space. Zero until measured; zero forever if the runtime has no floor.
std::atomic<float> g_measuredEyeHeightMetres{0.0f};

// WHY THE MEASUREMENT DRIVES NOTHING, and it is not an oversight. The two
// runtimes disagree by about a third of a metre about one unmoved seated
// wearer -- Quest 3 reads 1.215-1.245 m against the PFD MR's 1.541-1.604 m --
// so a floor-derived height would make the same person a different height on
// each headset, which is the whole defect this work removed. It also answers
// the wrong question: what needs correcting is the GAME seating its camera at
// the neck, a constant, not how tall the wearer is. It stays because it is
// what caught the disagreement and it costs one locate call.
float g_headBasisReference[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
// Beyond this the offset is clamped. A tracking glitch that threw the view
// origin metres across the map could push it inside geometry; a head cannot
// reach further than this from where it was when tracking was armed.
constexpr float kMaxHeadOffsetUnits = 40.0f;
// Where the eye sits relative to the neck pivot, in metres. Roughly 10 cm
// forward of and 10 cm above the pivot for an average adult; both are tunable
// because necks differ and because the right value is judged by whether pure
// head rotation moves the weapon, which is a thing seen rather than computed.
std::atomic_bool g_neckModel = true;
std::atomic<float> g_neckForwardMetres = 0.10f;
std::atomic<float> g_neckUpMetres = 0.10f;
std::uint64_t g_lastHeadPoseSequence = 0;
std::uint32_t g_headPoseStaleFrames = 0;

void SetHeadTrackingArmed(bool armed) {
    g_headTrackingArmed.store(armed, std::memory_order_release);
    if (!armed) {
        g_cameraYawWriteActive = 0;
        g_cameraYawDeltaBits = 0;
        // Must be released too, or the view origin would stay displaced by
        // wherever the head happened to be.
        g_cameraPositionWriteActive = 0;
        // Must be cleared too, or the viewmodel would keep cancelling a head
        // rotation that is no longer being applied.
        g_headDeltaYawDegrees = 0.0f;
        g_headCompensationValid = 0;
        Tf2VrLog("[TF2VR] Head tracking disarmed; engine angles and position released.\n");
        return;
    }
    g_haveHeadReference = false;
    Tf2VrLog("[TF2VR] Head tracking armed; recentring on the current head pose.\n");
}

bool IsHeadTrackingArmed() { return g_headTrackingArmed.load(std::memory_order_acquire); }

void SetHeadPositionalTracking(bool armed) {
    g_headPositionalArmed.store(armed, std::memory_order_release);
    if (!armed) g_cameraPositionWriteActive = 0;
    Tf2VrLog(armed
        ? "[TF2VR] positional head tracking on: leaning moves the view origin, the body stays put.\n"
        : "[TF2VR] positional head tracking off: the view origin is pinned to the player's eye point.\n");
}

bool IsHeadPositionalTracking() { return g_headPositionalArmed.load(std::memory_order_acquire); }

void AdvanceHeadTracking() {
    if (!g_headTrackingArmed.load(std::memory_order_acquire)) return;

    // Never hold the view on a stale sample. If the runtime stops delivering
    // poses, releasing hands the view straight back to the engine instead of
    // freezing it at the last orientation, which is the failure the player
    // would least be able to recover from.
    const std::uint64_t sequence = g_headPoseSequence;
    if (sequence == g_lastHeadPoseSequence) {
        if (++g_headPoseStaleFrames > 10) { g_cameraYawWriteActive = 0; g_cameraYawDeltaBits = 0; g_headCompensationValid = 0; }
        return;
    }
    g_lastHeadPoseSequence = sequence;
    g_headPoseStaleFrames = 0;
    if (!g_headPoseValid || !g_cameraAnglesAddress) {
        g_cameraYawWriteActive = 0;
        g_cameraYawDeltaBits = 0;
        g_headCompensationValid = 0;
        return;
    }

    const float headYaw = g_headYawDegrees;
    const float headPitch = g_headPitchDegrees;
    const float headRoll = g_headRollDegrees;

    float headBasis[9]{};
    if (!ReadPublishedBasis(g_headBasis, g_headBasisGeneration, headBasis)) return;

    // The runtime moved its own reference space -- the wearer recentred. Drop
    // ours so the block below re-captures against the space that now exists.
    if (g_recentreRequested.exchange(false, std::memory_order_acq_rel)) {
        g_haveHeadReference = false;
    }
    if (!g_haveHeadReference) {
        g_headYawReference = headYaw;
        g_headPitchReference = headPitch;
        g_headRollReference = headRoll;
        for (int i = 0; i < 9; ++i) g_headBasisReference[i] = headBasis[i];
        for (int i = 0; i < 3; ++i) g_headPositionReference[i] = g_headPositionMetres[i];
        g_haveHeadReference = true;
        // THE SAMPLE, IN FULL. The reference POSITION is the game's eye height
        // for the rest of the session, and until 2026-09-02 this line recorded
        // only the angles -- so a sample taken in a Titan cockpit, or while
        // sitting up to see the monitor, left no evidence of itself. LOCAL y is
        // what the tracked offset is measured from; STAGE is floor-referenced.
        char line[360]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] Head tracking recentred: reference yaw=%.2f pitch=%.2f roll=%.2f deg | "
            "reference position LOCAL x=%.4f y=%.4f z=%.4f m | STAGE eye height %.3f m "
            "(0 = not measured, and it drives nothing) | eye.raise in force %+.1f units.\n",
            headYaw, headPitch, headRoll,
            g_headPositionReference[0], g_headPositionReference[1], g_headPositionReference[2],
            static_cast<double>(g_measuredEyeHeightMetres.load(std::memory_order_acquire)),
            static_cast<double>(EyeHookRaiseInForce()));
        Tf2VrLog(line);
    }

    const float basePitch = BitsToFloat(g_cameraBasePitchBits);
    const float baseYaw = BitsToFloat(g_cameraBaseYawBits);
    const float baseRoll = BitsToFloat(g_cameraBaseRollBits);

    // HEADING IS REFERENCED. PITCH AND ROLL ARE NOT.
    //
    // The head pose is located in the runtime's LOCAL space, which is
    // GRAVITY-ALIGNED by specification: +Y is up, so the head's PITCH and ROLL
    // that come out of it are already absolute. The one axis that is arbitrary
    // is its YAW ORIGIN, which the runtime picks at session start -- and that
    // is the only axis a recentre reference was ever needed for.
    //
    // The previous composition applied the reference to all three axes:
    //
    //   delta   = head * reference^T     (row-basis, so this reads backwards)
    //   written = delta * base
    //
    // The ORDER there is correct -- verified on paper and replayed against the
    // archived logs, which it reproduces to 0.02 degrees. It does NOT conjugate
    // the tilt across body yaw; with the head held still it returns a constant
    // pitch and roll, exactly as the log shows. What it does instead is
    // subtract the reference's PITCH and ROLL, which are gravity-referenced
    // quantities that never needed correcting, and that bakes the posture held
    // at the moment of recentring in as a PERMANENT tilt of the engine camera.
    // Recentre while looking down 18 degrees and every frame afterwards is
    // written 18 degrees off.
    //
    // Measured, 2026-08-20 archive: reference pitch 18.48 roll 5.21; written
    // pitch ran head pitch MINUS 18.2 and written roll head roll MINUS 5.2,
    // held constant across a full revolution of body yaw.
    //
    // The world never showed it, because the projection layer is submitted with
    // the runtime's TRUE pose and the compositor reprojects it. Everything
    // SCREEN-ANCHORED does show it, because it is drawn in the engine camera's
    // frame; body yaw then sweeps that fixed tilt around the vertical, and a
    // rigid frame whose up-axis is tilted by a constant amount is precisely the
    // panels' height and angle tracing a sine and a cosine of one revolution --
    // the "boat riding a wave".
    //
    // Adding the heading delta onto the base yaw is the ONLY legal Euler
    // addition in this file, and it is legal because yaw is OUTERMOST in
    // Source's convention: Rz(a)*Rz(b) = Rz(a+b) leaves the pitch and roll
    // underneath it untouched. Pitch would not survive the same treatment,
    // which is what the algebra note above AngleBasis is about.
    //
    // basePitch and baseRoll are deliberately NOT composed in. They measure
    // 0.00 on every tick -- the body's basis is pure yaw -- and in VR the head
    // owns pitch and roll outright. They stay in the log line below so that a
    // non-zero one would be visible rather than silently absorbed.
    const float headingDelta = WrapDegrees(headYaw - g_headYawReference);
    // Published for the ADS alignment, which has to know how far the RENDERED
    // view is from the game's own before it can put the two together. Written
    // here, at the one place both quantities exist in the same instant, rather
    // than rebuilt by a consumer from globals sampled on other clocks -- that
    // mistake has its own entry in this file's history.
    g_publishedHeadYawDelta.store(headingDelta, std::memory_order_release);
    g_publishedHeadPitch.store(headPitch, std::memory_order_release);
    g_publishedHeadValid.store(true, std::memory_order_release);

    // THE ONE PLACE THE VIEW'S PITCH CAN BE MOVED AT ALL.
    //
    // "If I have a scoped weapon and am pointing it up high before I enter ADS
    //  ... it zooms me in looking at the direction my head is facing and I often
    //  only see the butt of the gun and I have to tilt my head WAY UP to see the
    //  scope itself."
    //
    // I said several times that pitch could not follow the gun because the
    // rendered pitch IS the headset's own absolute pitch and there is no base
    // term to write. That was accurate about the code and wrong as an answer:
    // there is no base term because THIS LINE chose not to have one. "Cannot" was
    // really "should not", and whether the trade is worth taking is the wearer's
    // call rather than mine.
    //
    // So there is an offset now, added once on the ADS transition and cleared on
    // release, exactly as the yaw turn is. It is one-shot and instantaneous for
    // the same reason snap turn is: no intermediate frames means no optical flow
    // to be sick from.
    //
    // WHAT IT COSTS, said plainly because it is the real objection: while it is
    // held, level in the headset is not level in the world. It shifts the
    // horizon rather than tilting it, so it is not the roll mismatch that makes
    // people ill fastest -- but it is still a decoupling, it is why most VR
    // titles refuse to do it, and ads.face_aim_pitch exists so it can be turned
    // off from the panel without a rebuild.
    // THE PITCH COMPOSITION, PUBLISHED SO THE RATCHET CAN BE ATTRIBUTED.
    //
    // The engine's own view pitch was measured climbing monotonically across
    // crouch-plus-ADS cycles -- +20.8, +33.9, +47.9, +68.9, +72.1, +84.2, +86.8
    // -- until it saturated at the +89 clamp two lines below and stayed there
    // for the rest of the session. At that clamp the yaw axis is degenerate, so
    // the gun swings far to the side, which is exactly what was reported.
    //
    // The clamp is OURS, so the saturation point is ours, and what has never
    // been printed is which of the two terms is doing the climbing. headPitch is
    // supposed to be an ABSOLUTE head orientation and the offset is supposed to
    // return to zero on ADS release -- if both hold, nothing here can ratchet,
    // and one of them does not hold. Publishing both separately is the
    // difference between naming the term and guessing at it.
    g_publishedPitchHead = headPitch;
    g_publishedPitchOffset = g_viewPitchOffset.load(std::memory_order_acquire);
    float pitch = headPitch + g_viewPitchOffset.load(std::memory_order_acquire);
    if (pitch > 89.0f) pitch = 89.0f;
    if (pitch < -89.0f) pitch = -89.0f;
    float yaw = WrapDegrees(baseYaw + headingDelta);
    float roll = headRoll;

    // Still published for the viewmodel pass, and still the head's rotation
    // since recentring. Nothing reads it today; it is a diagnostic.
    float delta[9]{};
    BasisMultiplyTransposeRight(headBasis, g_headBasisReference, delta);
    g_headDeltaGeneration = g_headDeltaGeneration + 1;
    for (int i = 0; i < 9; ++i) g_headDeltaBasis[i] = delta[i];
    g_headDeltaGeneration = g_headDeltaGeneration + 1;

    g_cameraDesiredYawBits = FloatToBits(WrapDegrees(yaw));
    // THE YAW THE DETOUR ACTUALLY USES. It adds this to the base it latches on
    // the render frame, so the sum can never pair this frame's write with an
    // older frame's base. `yaw` above is that same sum formed HERE, on the
    // plugin frame, and is kept only for the log line and the published pair.
    g_cameraYawDeltaBits = FloatToBits(headingDelta);
    // A safety clamp only. The pitch written now IS the head's own absolute
    // pitch, which a neck cannot take past vertical, so this only ever engages
    // on a tracking glitch.
    if (pitch > 89.0f) pitch = 89.0f;
    if (pitch < -89.0f) pitch = -89.0f;
    g_cameraDesiredPitchBits = FloatToBits(pitch);
    g_cameraDesiredRollBits = FloatToBits(roll);
    g_cameraYawWriteActive = 1;

    const float deltaYaw = WrapDegrees(yaw - baseYaw);

    // --- Positional head tracking ---------------------------------------
    //
    // Lean and small lateral movement, with the body staying put. The camera
    // position field is the three floats immediately before the angles, and
    // IGCS writes exactly these, so it is render-effective for this game.
    //
    // Scale comes from the stereo half-IPD rather than a hardcoded inches-per-
    // unit constant. That keeps head movement and eye separation in the SAME
    // world scale automatically: tuning IPD until the world feels right also
    // makes a 10 cm lean move the view by the matching number of units, where
    // two independent constants would have to be tuned to agree by hand.
    if (g_headPositionalArmed.load(std::memory_order_acquire)) {
        const float unitsPerMetre = SourceUnitsPerMetre();
        // Room axes are OpenXR's: +X right, +Y up, -Z forward. Source is
        // +X forward, +Y left, +Z up.
        float roomForward = -(g_headPositionMetres[2] - g_headPositionReference[2]) * unitsPerMetre;
        float roomLeft = -(g_headPositionMetres[0] - g_headPositionReference[0]) * unitsPerMetre;
        float up = (g_headPositionMetres[1] - g_headPositionReference[1]) * unitsPerMetre;

        // NO HEIGHT CORRECTION HERE, AND THAT IS THE FIX. 2026-09-02.
        //
        // Two render-camera height levers used to be added to `up` at this
        // point: a floor-derived constant and a debug knob. Both raised the
        // rendered eye WITHOUT raising the shot origin, so the round left the
        // game's own eye and landed below the mark by atan(raise / distance) --
        // reported by the wearer as "rounds land below the reticle", and
        // confirmed by a run at zero where the aim was perfect and the height
        // was short. Both are deleted -- key, branch and getter -- so the trap
        // cannot be re-entered from an ini.
        //
        // The height now lives at the engine's own EyePosition getter, which
        // the camera, the gun and the round all read. See player_eye_hook.h and
        // docs/RESULT-HEIGHT-2026-09-02.md. What this block still computes is
        // TRACKING -- where the wearer's head has moved since the reference --
        // which belongs here and nowhere else.

        // NECK MODEL: the eyes orbit the neck, so rotation is not lean.
        //
        // The runtime reports where the EYES are, and eyes are carried on a
        // neck. Turning the head right swings them right and forward by several
        // centimetres, which this code then read as a lean -- and with the
        // weapon detached and pinned in the world, that lean slid the gun the
        // other way. Reported as: rotate right, gun goes too far left. Real
        // parallax, but exaggerated, because the viewmodel sits far closer than
        // a real weapon would.
        //
        // What should drive lean is the PIVOT, not the eyes. The eye sits a
        // little forward of and above the pivot, so the displacement a pure
        // rotation produces is that offset carried from the reference
        // orientation to the current one. Subtract it and rotation contributes
        // nothing, while genuine leaning -- which moves the pivot too -- is
        // untouched.
        //
        // Both bases are already in hand and in the same frame: headBasis now,
        // g_headBasisReference at recentring, rows forward/right/up.
        if (g_neckModel.load(std::memory_order_relaxed)) {
            const float forwardOffset = g_neckForwardMetres.load(std::memory_order_relaxed) * unitsPerMetre;
            const float upOffset = g_neckUpMetres.load(std::memory_order_relaxed) * unitsPerMetre;
            // Eye position relative to the pivot, expressed in room axes, at
            // the reference orientation and at the current one.
            float induced[3]{};
            for (int axis = 0; axis < 3; ++axis) {
                const float nowComponent = forwardOffset * headBasis[axis] +
                                           upOffset * headBasis[6 + axis];
                const float refComponent = forwardOffset * g_headBasisReference[axis] +
                                           upOffset * g_headBasisReference[6 + axis];
                induced[axis] = nowComponent - refComponent;
            }
            // Room axes here are Source's: X forward, Y left, Z up.
            roomForward -= induced[0];
            roomLeft -= induced[1];
            up -= induced[2];
        }
        // Room forward is not game forward. When the head is at its reference
        // yaw the view points along the engine's base yaw, so the room-to-game
        // rotation is exactly that difference. Without this, leaning left moves
        // you along a fixed world axis instead of along your own left.
        constexpr float toRadians = 0.01745329252f;
        const float mapping = WrapDegrees(baseYaw - g_headYawReference) * toRadians;
        const float cosMap = std::cos(mapping), sinMap = std::sin(mapping);
        float offset[3] = {roomForward * cosMap - roomLeft * sinMap,
                           roomForward * sinMap + roomLeft * cosMap,
                           up};
        for (float& component : offset) {
            if (component > kMaxHeadOffsetUnits) component = kMaxHeadOffsetUnits;
            if (component < -kMaxHeadOffsetUnits) component = -kMaxHeadOffsetUnits;
        }
        // Publish the OFFSET, not a finished position.
        //
        // These used to be summed with g_cameraBasePos* here, on the plugin
        // frame, and the detour wrote the result. But the base is latched on the
        // RENDER frame, so the sum married two different frames -- and on a step
        // the game moves up, the detour latches the new height, then overwrites
        // it with old-height-plus-offset. One frame at the pre-step height, 4 to
        // 11 units low, which is the stair lurch. It disappeared entirely with
        // positional tracking off, which is what identified it as ours.
        //
        // The detour now adds these to the position it has in hand. Same fix as
        // the angles needed, for the same reason.
        // IS LEANING PRODUCING NUMBERS AT ALL?
        //
        // Reported as never really having worked. The output path is now proven
        // -- the stair lurch was this write and vanished when F7 stopped it --
        // so if leaning does nothing the failure is upstream of the write, and
        // there are only three places it can be: the runtime never publishes a
        // position, the reference makes every delta zero, or the scale is so
        // small the result is invisible.
        //
        // All three are visible in one line. Logged on meaningful change only,
        // so standing still is silent and a lean writes a handful of lines.
        {
            static float lastLogged = -1.0f;
            const float magnitude = std::sqrt(offset[0] * offset[0] + offset[1] * offset[1] +
                                              offset[2] * offset[2]);
            if (std::fabs(magnitude - lastLogged) > 0.5f) {
                lastLogged = magnitude;
                char line[300]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] lean: head %.3f %.3f %.3f m from reference -> offset (%.1f %.1f %.1f) "
                    "units, magnitude %.1f. Scale %.1f units/m; clamp %.0f.\n",
                    g_headPositionMetres[0] - g_headPositionReference[0],
                    g_headPositionMetres[1] - g_headPositionReference[1],
                    g_headPositionMetres[2] - g_headPositionReference[2],
                    offset[0], offset[1], offset[2], magnitude,
                    unitsPerMetre, kMaxHeadOffsetUnits);
                Tf2VrLog(line);
            }
        }
        g_cameraOffsetXBits = FloatToBits(offset[0]);
        g_cameraOffsetYBits = FloatToBits(offset[1]);
        g_cameraOffsetZBits = FloatToBits(offset[2]);
        // Kept current for anything still reading it; no longer what is written.
        // THE HEIGHT CENSUS. Read-only, and it exists because six runs went into
        // guessing a number that the game already knows.
        //
        // Four quantities decide where the wearer's eye ends up, and no run has
        // ever printed them together:
        //
        //   * the GAME's own camera height, before we touch it
        //   * the offset WE add
        //   * the sum, which is where the eye actually is
        //   * which headset is connected
        //
        // The contradiction this settles: the aim is correct, so the view is
        // angle-correct; our vertical offset measures ~0 on both headsets, so
        // the eye should sit at the game's own camera on both; therefore both
        // headsets should look identical -- and they do not. One of those is
        // false. If the game's camera height comes out the SAME on both, the
        // difference is downstream of here and the camera is exonerated. If it
        // comes out DIFFERENT, the game is being driven differently per headset
        // and that is a different bug from any that has been chased.
        //
        // Once a second, capped at 40 lines, so a session carries the answer
        // without the log carrying a line per frame.
        {
            static std::atomic_uint64_t lastLogMs{0};
            static std::atomic_uint32_t lines{0};
            const std::uint64_t nowMs = GetTickCount64();
            // NOT const: compare_exchange writes the observed value back into
            // its expected argument on failure, so const_cast-ing a const local
            // here would be a write through a const object. Undefined, and it
            // compiles perfectly quietly.
            std::uint64_t previous = lastLogMs.load(std::memory_order_relaxed);
            if (nowMs - previous >= 1000 && lines.load(std::memory_order_relaxed) < 40 &&
                lastLogMs.compare_exchange_strong(previous, nowMs, std::memory_order_acq_rel)) {
                lines.fetch_add(1, std::memory_order_relaxed);
                const float baseZ = BitsToFloat(g_cameraBasePosZBits);
                char line[430]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] HEIGHT CENSUS: game camera z=%.2f units | our offset z=%+.2f | eye "
                    "ends at z=%.2f | head above recentre reference %.3f m | headset '%s'. STAND IN "
                    "THE SAME SPOT ON BOTH HEADSETS AND COMPARE THE FIRST NUMBER: same means the "
                    "game is doing the same thing and the difference is downstream of the camera; "
                    "different means the game is being driven differently per headset.\n",
                    baseZ, offset[2], baseZ + offset[2],
                    g_headPositionMetres[1] - g_headPositionReference[1],
                    HeadsetFingerprint());
                Tf2VrLog(line);
            }
        }
        g_cameraDesiredPosXBits = FloatToBits(BitsToFloat(g_cameraBasePosXBits) + offset[0]);
        g_cameraDesiredPosYBits = FloatToBits(BitsToFloat(g_cameraBasePosYBits) + offset[1]);
        g_cameraDesiredPosZBits = FloatToBits(BitsToFloat(g_cameraBasePosZBits) + offset[2]);
        g_cameraPositionWriteActive = 1;
    } else {
        g_cameraPositionWriteActive = 0;
    }

    g_headDeltaYawDegrees = deltaYaw;
    // Published ONCE PER FRAME, under a generation, and this is what the
    // viewmodel correction consumes.
    //
    // It used to consume the pair the detour latches instead, which was changed
    // to fix the weapon lurching and snapping back on a cadence mismatch. That
    // swapped one problem for a worse one: the detour latches on every view
    // build, so a value that was stable for a whole frame became one that can
    // change several times within it, and the four corrected uploads of a single
    // frame could each be built from a different pair. The weapon is then drawn
    // from two different corrections in one frame, which is the flicker.
    g_headAnglesGeneration = g_headAnglesGeneration + 1;
    g_headBaseAngles[0] = basePitch;
    g_headBaseAngles[1] = baseYaw;
    g_headBaseAngles[2] = baseRoll;
    g_headWrittenAngles[0] = pitch;
    g_headWrittenAngles[1] = yaw;
    g_headWrittenAngles[2] = roll;
    g_headAnglesGeneration = g_headAnglesGeneration + 1;
    g_headCompensationValid = 1;

    static std::uint32_t sample = 0;
    if (sample++ % 120 == 0) {
        // THE FALSIFIER FOR THE HEADING-ONLY COMPOSITION, carried in the line
        // itself so it needs no arithmetic and no visual judgement.
        //
        //   tilt = written pitch/roll MINUS head pitch/roll.
        //
        // Heading-only writes the head's absolute pitch and roll, so both must
        // read 0.00 on EVERY sample, whatever posture the wearer recentred in.
        // The previous composition wrote head-minus-reference, so both read
        // minus the reference pitch/roll -- in the 2026-08-20 archive, -18.2
        // and -5.2, held constant right around the yaw circle. refp/refr are
        // printed alongside so a deliberately tilted recentre is visible as a
        // large reference with a tilt that STILL reads zero.
        //
        // The positive control for "yaw was not disturbed" is in the same line:
        // written yaw minus base yaw must still equal delta yaw.
        const float tiltPitch = pitch - headPitch;
        const float tiltRoll = roll - headRoll;
        char line[384]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] Head tracking: head=(y%.2f p%.2f r%.2f) base=(y%.2f p%.2f r%.2f) "
            "-> written=(y%.2f p%.2f r%.2f); heading-only, delta yaw %+.2f; "
            "ref=(p%.2f r%.2f) TILT=(p%+.2f r%+.2f).\n",
            headYaw, headPitch, headRoll, baseYaw, basePitch, baseRoll,
            yaw, pitch, roll, deltaYaw,
            g_headPitchReference, g_headRollReference, tiltPitch, tiltRoll);
        Tf2VrLog(line);
    }
}

void RequestOneShotCameraNudge() { g_nudgeRequested.store(true, std::memory_order_release); }

void TryApplyRequestedCameraNudge() {
    const auto address = reinterpret_cast<float*>(g_cameraStructAddress);
    if (g_nudgeRequested.exchange(false, std::memory_order_acq_rel)) {
        if (!IsReadable(address, sizeof(float) * 3)) return;
        g_nudgeTargetX = address[0] + 500.0f;
        g_nudgeFramesRemaining.store(20, std::memory_order_release);
        g_cameraWriteBlock = 1;
        char line[192]{};
        std::snprintf(line, sizeof(line), "[TF2VR] F7 bounded write: X %.3f -> %.3f for 20 frames; primary writer suppressed.\n", address[0], g_nudgeTargetX);
        Tf2VrLog(line);
    }
    const std::uint32_t remaining = g_nudgeFramesRemaining.load(std::memory_order_acquire);
    if (!remaining || !IsReadable(address, sizeof(float) * 3)) return;
    address[0] = g_nudgeTargetX;
    if (g_nudgeFramesRemaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        g_cameraWriteBlock = 0;
        Tf2VrLog("[TF2VR] F7 bounded write complete; primary writer restored.\n");
    }
}

void SetNeckModel(bool enabled) {
    g_neckModel.store(enabled, std::memory_order_release);
    Tf2VrLog(enabled
        ? "[TF2VR] neck model ON: the lean is taken from the neck pivot, so turning the head is not "
          "read as leaning.\n"
        : "[TF2VR] neck model OFF: the lean is taken from the eyes, so head rotation swings them and "
          "reads as a lean.\n");
}

void SetNeckOffsets(float forwardMetres, float upMetres) {
    const float f = forwardMetres < 0.0f ? 0.0f : (forwardMetres > 0.4f ? 0.4f : forwardMetres);
    const float u = upMetres < 0.0f ? 0.0f : (upMetres > 0.4f ? 0.4f : upMetres);
    g_neckForwardMetres.store(f, std::memory_order_release);
    g_neckUpMetres.store(u, std::memory_order_release);
    char line[220]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] neck pivot: eye sits %.3f m forward and %.3f m above it. Too small leaves rotation "
        "moving the weapon; too large moves it the other way.\n", f, u);
    Tf2VrLog(line);
}

bool TryGetHeadPositionReference(float outMetres[3]) {
    if (!g_haveHeadReference) return false;
    for (int i = 0; i < 3; ++i) outMetres[i] = g_headPositionReference[i];
    return true;
}

bool TryGetHeadYawReference(float* degrees) {
    if (!g_haveHeadReference) return false;
    if (degrees) *degrees = g_headYawReference;
    return true;
}

void RequestHeadTrackingRecentre() { RequestHeadTrackingRecentreImpl(); }

bool GetHeadViewDelta(float* yawDelta, float* absolutePitch) {
    if (!g_publishedHeadValid.load(std::memory_order_acquire)) return false;
    if (yawDelta) *yawDelta = g_publishedHeadYawDelta.load(std::memory_order_acquire);
    if (absolutePitch) *absolutePitch = g_publishedHeadPitch.load(std::memory_order_acquire);
    return true;
}

void SetViewPitchOffset(float degrees) {
    if (degrees > 89.0f) degrees = 89.0f;
    if (degrees < -89.0f) degrees = -89.0f;
    const float previous = g_viewPitchOffset.exchange(degrees, std::memory_order_release);
    if (std::fabs(previous - degrees) < 0.05f) return;
    // Logged only on the way in from zero and on the way back to zero: while
    // ADS follows the hand this is set every tick.
    if (previous != 0.0f && degrees != 0.0f) return;
    char line[340]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] view pitch offset %+.1f -> %+.1f deg, in one tick. Set on ADS entry and CLEARED on "
        "release, so outside ADS it is zero and the view sits exactly where the head physically "
        "does. A zero here after a press is the release working, not a missing offset.\n",
        static_cast<double>(previous), static_cast<double>(degrees));
    Tf2VrLog(line);
}

float ViewPitchOffset() { return g_viewPitchOffset.load(std::memory_order_acquire); }

void SetMeasuredEyeHeightMetres(float metres) {
    g_measuredEyeHeightMetres.store(metres, std::memory_order_release);
}

float MeasuredEyeHeightMetres() { return g_measuredEyeHeightMetres.load(std::memory_order_acquire); }


// The game's own camera height, for the view-offset search to close its
// arithmetic against. Latched by the camera detour on the render frame.
float GameCameraHeightUnits() { return BitsToFloat(g_cameraBasePosZBits); }

// All three axes of it, for C1's D1 measurement, which has to decide WHICH
// entity is being faced and not merely how high it is.
void GameCameraBasePosition(float out[3]) {
    out[0] = BitsToFloat(g_cameraBasePosXBits);
    out[1] = BitsToFloat(g_cameraBasePosYBits);
    out[2] = BitsToFloat(g_cameraBasePosZBits);
}

// The head position the runtime last reported, in metres on OpenXR axes and
// BEFORE the recentre reference is subtracted. The census needs the raw value:
// the delta is known to be near zero on both headsets, and H3 is a claim about
// the absolute one.
bool ReadHeadPositionMetres(float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = g_headPositionMetres[i];
    // The same witness the reference uses: until a recentre has happened there
    // is no head pose to have an opinion about, and reporting the zeroes as a
    // measurement would put a fabricated 0.000 m beside the real one.
    return g_haveHeadReference;
}

// The two terms the written camera pitch is composed from, so a ratchet in
// either can be attributed rather than inferred from the sum.
void ReadPitchComposition(float* headPitch, float* viewOffset) {
    if (headPitch) *headPitch = g_publishedPitchHead;
    if (viewOffset) *viewOffset = g_publishedPitchOffset;
}
