#pragma once

// ---------------------------------------------------------------------------
// ads.lock -- IN ADS, HAND THE GUN BACK TO THE ENGINE. "Just like flat."
//
// The wearer, 2026-08-26, after playing it:
//
//   "Where I'm looking is NOT where I want it to zoom in. We must use the
//    reticle as the focus point for the zoom. I don't care if it is
//    disorienting. The current behavior is worse. We also need to move the gun
//    into the center of the screen at the right angle - just like flat. I am
//    positive about this. And we need to HEAVILY dampen movement at that
//    point."
//
// THE MECHANISM IS A REMOVAL, NOT AN ADDITION, and that is why it is the right
// one. The engine ALREADY has a canonical ADS pose: it is what flat shows --
// gun centred, sights on the view axis, animated in and out. This project spends
// its effort OVERRIDING that, in two places: placement_pin writes the hand's
// pose over the engine's committed viewmodel placement, and aim_cmd writes the
// hand's angles over the engine's attackangles.
//
// So "just like flat" is not something to compute. It is what happens when both
// overrides stand down. No canonical offset to calibrate, no sight-line solve,
// no forced view rotation, and nothing new that can be wrong per weapon.
//
// WHAT IT COSTS, SAID PLAINLY. While ADS is held the gun no longer points where
// your hand points -- it points where you LOOK, which is what flat ADS is. The
// thing the gun was aimed at on entry is not preserved. That was the trade the
// wearer was asked about and settled: "I don't care if it is disorienting. The
// current behavior is worse."
//
// AND IT ANSWERS THE ZOOM COMPLAINT WITHOUT TOUCHING THE PROJECTION. The zoom
// magnifies about the view axis. The reticle sits at the aim point. Those are
// different places in VR, so zooming pushed the reticle further off-screen --
// the reported annoyance. Once the aim IS the view, the reticle is at the view
// axis, and the zoom centres on it by construction.
//
// THE DAMPING IS THE TURN, NOT THE HAND. Flat ADS mutes look sensitivity, and
// in VR the stick turns the body. Damping head motion is neither possible nor
// wanted; damping the stick is exactly the flat behaviour and is one multiply
// in the pad composer. The C4 brace still owns the hand-to-gun path and applies
// when ads.lock is off.
// ---------------------------------------------------------------------------

// INI: `set ads.lock = 1`. Default OFF, so nothing changes until it is asked
// for.
void SetAdsLock(bool locked);
bool IsAdsLock();

// True when the overrides should stand down: ads.lock armed AND the engine's
// own zoom fraction past the entry threshold. ONE predicate, asked by all three
// consumers, so the gun, the aim and the turn can never disagree about whether
// we are in ADS.
//
// Hysteresis, because the fraction ramps through the transition and three
// consumers flipping on different frames would be visible as the gun and the
// bullet disagreeing for a frame or two at every press.
bool AdsLockEngaged();

// INI: `set ads.turn_scale`. The stick's turn output is multiplied by this
// while the lock is engaged. 1.0 is no damping.
void SetAdsTurnScale(float scale);
float AdsTurnScale();

// Per plugin frame: advances the latch and emits the falsifier line on its own
// clock -- how many frames engaged, and how many times each consumer stood
// down, so "the gun handed back but the aim did not" is a number rather than a
// feeling.
void AdvanceAdsLock();

// Counted by the consumers, so the report can show all three moving together.
void NoteAdsLockPlacementReleased();
void NoteAdsLockAimReleased();
void NoteAdsLockTurnDamped();

// IS THE GAME IN ADS, ACCORDING TO THE ENGINE. Independent of ads.lock, so
// anything that needs the state can ask without also opting into the lock.
//
// This replaces WorldIsZoomed() wherever it is used as a predicate. That one
// divides the rendered frustum by an ALL-TIME MAXIMUM, which one transient
// poisons for a whole session, and it needed a hysteresis latch bolted on
// because the pistol sat on its threshold. The pistols do not zoom at all --
// measured 1.01x -- so no threshold was ever going to separate them, and the
// engine's own fraction has none of that trouble.
bool AdsEngagedByEngine();
