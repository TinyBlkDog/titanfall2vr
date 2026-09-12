#pragma once

// ---------------------------------------------------------------------------
// RUNG 1 -- TITANFALL'S OWN HUD WARP PIPELINE.
//
// The constant-buffer route is measured closed at both widths (R1/F1): the
// 64-byte pixel-ortho matrix was scaled on 798 of 798 uploads with the
// arithmetic verified in the log, and nothing moved. That result stops looking
// like a dead end the moment you read the cvar dump, which has been on disk
// since 08-17:
//
//   hudwarp_xScale 1.2   hudwarp_xWarp 45    hudwarp_viewDist 1.0
//   hudwarp_yScale 1.1   hudwarp_yWarp 30    hudwarp_chopsize 60
//   hudwarp_override 0                       (all CHEAT-flagged)
//
// The engine projects its HUD through a WARP pipeline of its own, parameterised
// here. That is very likely WHY no camera constant buffer ever moved the HUD:
// the HUD is not projected by the camera matrices at all.
//
// THE WRITE. Value slots directly, via TrySetCvarFloat -- the same mechanism
// r_drawviewmodel uses to bypass the FCVAR_CHEAT gate in single-player. Its
// documented cost applies here and is the first thing to suspect if read-backs
// move and the screen does not: it writes the float and int slots but does NOT
// fire a change callback. A parameter the warp pipeline caches on change rather
// than reads per frame would therefore read back correct and do nothing. That
// is a DISTINCT failure from "the pipeline is inert", and the log says which by
// reporting the read-back separately from the visible result.
//
// THE SHAPE OF THE RUN. A stepper, not a switch: one key advances through the
// stages below, each of which arms one parameter, holds it, and restores every
// hudwarp value from the originals captured when the stepper first armed. A run
// that ends mid-stage still restores itself on its own clock.
//
//   0  override 0 -> 1 alone          does arming the family alone do anything?
//   1  + xScale   1.2  -> 0.6         half the horizontal extent
//   2  + yScale   1.1  -> 0.55        half the vertical extent
//   3  + xWarp    45   -> 15
//   4  + yWarp    30   -> 10          the sine-warp rider's best suspect
//   5  + viewDist 1.0  -> 1.5         push the projected surface away
//   6  + chopsize 60   -> 20
//
// Steps are BIG on purpose. A control that moves the lever a few percent and a
// control that is not connected look identical through a headset, and this
// project has now paid for that twice.
// ---------------------------------------------------------------------------

// Advances to the next stage, arming it. While a stage is running a second
// press ends it early and restores, so the key always moves toward the
// restored state.
void StepHudWarp();

// Restores and reports on its own clock when a stage's window elapses.
void AdvanceHudWarp();

// Puts every hudwarp value back to what it was when the stepper first armed.
// Safe to call when nothing was ever armed.
void RestoreHudWarp();
