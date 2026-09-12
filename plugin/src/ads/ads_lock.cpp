#include "ads_lock.h"

#include "ads_zoom.h"
#include "diagnostics.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace {

std::atomic_bool g_locked = false;
std::atomic<float> g_turnScale = 0.25f;
std::atomic_bool g_engaged = false;

// HYSTERESIS ON THE ENGINE'S OWN FRACTION.
//
// The fraction ramps through the ADS transition, so a bare threshold would let
// three consumers -- the placement, the aim write and the turn -- flip on
// different frames as each happens to sample it. The gun and the bullet
// disagreeing for even one frame is exactly the clock-split class this project
// has paid for before, so the latch is computed ONCE per frame and all three
// ask the same question.
constexpr float kEnterFrac = 0.35f;
constexpr float kExitFrac = 0.15f;

std::uint64_t g_frames = 0;
std::uint64_t g_placementReleased = 0;
std::uint64_t g_aimReleased = 0;
std::uint64_t g_turnDamped = 0;
std::uint64_t g_nextReport = 0;

}  // namespace

void SetAdsLock(bool locked) {
    const bool was = g_locked.exchange(locked, std::memory_order_acq_rel);
    if (was == locked) return;
    Tf2VrLog(locked
        ? "[TF2VR] ads.lock = 1: aiming down sights TURNS THE VIEW to face where the gun was "
          "pointing, drives the body's pitch from the hand so the round and the reticle go where "
          "it was aimed, and damps the stick. The hand keeps the gun and keeps aiming it. Handing "
          "both to the engine was tried and it took the arm out of aiming altogether, which is "
          "not what a VR sight picture should be.\n"
        : "[TF2VR] ads.lock = 0: no alignment on entry, no turn damping.\n");
}

bool IsAdsLock() { return g_locked.load(std::memory_order_acquire); }

bool AdsEngagedByEngine() { return g_engaged.load(std::memory_order_acquire); }

// The lock is the ENGINE state AND the setting. Kept separate so the predicate
// can be reused by things that must not be switched on and off by ads.lock.
bool AdsLockEngaged() {
    return g_locked.load(std::memory_order_acquire) && g_engaged.load(std::memory_order_acquire);
}

void SetAdsTurnScale(float scale) {
    if (scale < 0.0f) scale = 0.0f;
    if (scale > 1.0f) scale = 1.0f;
    const float previous = g_turnScale.exchange(scale, std::memory_order_release);
    if (previous == scale) return;
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] ads.turn_scale = %.2f. The stick's turn is multiplied by this while ADS is held. "
        "This is the flat game's muted look sensitivity, and it is the stick rather than the head "
        "because damping head motion is neither possible nor wanted. 1.00 is no damping.\n",
        static_cast<double>(scale));
    Tf2VrLog(line);
}

float AdsTurnScale() { return g_turnScale.load(std::memory_order_acquire); }

void NoteAdsLockPlacementReleased() { ++g_placementReleased; }
void NoteAdsLockAimReleased() { ++g_aimReleased; }
void NoteAdsLockTurnDamped() { ++g_turnDamped; }

void AdvanceAdsLock() {
    // The LATCH runs whether or not ads.lock is armed: WorldIsZoomed's
    // replacement needs the engine state regardless, and a predicate that only
    // works when an unrelated setting happens to be on is the "built fix sitting
    // switched off" fault in another costume.
    AdsZoomState zoom;
    if (!GetAdsZoom(&zoom)) {
        g_engaged.store(false, std::memory_order_release);
        return;
    }
    const bool was = g_engaged.load(std::memory_order_relaxed);
    const bool now = was ? (zoom.frac > kExitFrac) : (zoom.frac >= kEnterFrac);
    if (now != was) {
        char line[300]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] ads.lock %s at zoom fraction %.2f (enter %.2f, exit %.2f).\n",
            now ? "ENGAGED -- the engine owns the gun" : "released -- the hand owns the gun again",
            static_cast<double>(zoom.frac), static_cast<double>(kEnterFrac),
            static_cast<double>(kExitFrac));
        Tf2VrLog(line);
    }
    g_engaged.store(now, std::memory_order_release);
    if (now) ++g_frames;

    const std::uint64_t tick = GetTickCount64();
    if (g_nextReport == 0) { g_nextReport = tick + 5000; return; }
    if (tick < g_nextReport) return;
    g_nextReport = tick + 5000;
    if (g_frames == 0) return;

    // ALL THREE CONSUMERS, IN ONE LINE. The whole point of a single latch is
    // that the gun, the bullet and the turn move together; the way to know they
    // did is to count each one separately and read them side by side. Placement
    // and aim run on different threads at different rates, so their counts will
    // not match exactly -- but either sitting at ZERO while the other climbs is
    // the failure this line exists to catch.
    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] ads.lock: engaged on %llu frames | placement stood down %llu, aim stood down "
        "%llu, turn damped %llu (scale %.2f). Either of the first two at zero while the other "
        "climbs means the gun and the bullet are not agreeing about ADS.\n",
        static_cast<unsigned long long>(g_frames),
        static_cast<unsigned long long>(g_placementReleased),
        static_cast<unsigned long long>(g_aimReleased),
        static_cast<unsigned long long>(g_turnDamped),
        static_cast<double>(g_turnScale.load(std::memory_order_relaxed)));
    Tf2VrLog(line);
    g_frames = 0;
    g_placementReleased = 0;
    g_aimReleased = 0;
    g_turnDamped = 0;
}
