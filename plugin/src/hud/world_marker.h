#pragma once

// ---------------------------------------------------------------------------
// WORLD MARKERS -- waypoints, pickup prompts, Titan lock-on -- corrected at the
// CLIENT SEAM, before the game projects them. HANDOFF-HUD-MARKERS-2026-09-05
// Part 3, lead "correct the INPUT instead of the output".
//
// THE SEAM. The client's per-frame RUI draw loop (client.dll+0x309820) walks
// each instance's tracked bindings and evaluates the vec3 kinds through a
// pointer table at client+0xB21290 (.data), copying the result into the
// instance's argument block at +0x40. The engine projects that raw world
// vector on the render thread (HUD-MAP section 2d). The four vec3 kinds:
//   kind 1  table[1], table[2] -> 0x27A500  entity origin through a handle
//   kind 2  direct call        -> 0x27CA80  attachment point (writes in place)
//   kind 3  table[10]          -> 0x27A530  entity origin + overhead offset
//   kind 4  table[13]          -> 0x27A5A0  entity EYE ANGLES -- not a position
// Kinds 1 and 3 are swapped in the table (pointer swap, no trampoline); kind 2
// is an entry detour (15 displaced bytes); kind 4 is swapped for a counting
// pass-through only. Every RVA .pdata-checked with pescan extent; every
// original verified in place before the swap, refusing loudly otherwise.
//
// THE CORRECTION (`marker.reframe`), YAW ONLY since run 1. Hypothesis H-B:
// the marker is projected in the ENGINE camera's yaw frame while the eye is
// rendered in the head's, so it is off by the difference and rides the head.
// The fix is a data write, a rotation about world Z through the view origin:
//     P' = E + Rz(-marker.gain * headYawDelta) * (P - E)
// E is the origin the view is built from. Exact under the hypothesis at gain
// 1, and a no-op when the head delta is zero. RUN 1 shipped a full basis
// change including PITCH and the wearer saw its pitch half as a "flattened U":
// the frames' pitches differ (body 5.91 vs head 12.0 in the log, body -85.00
// later), which lifted the point 185 units at 2357 units of range = 4.50 deg.
// Pitch is now left exactly as the game evaluated it. The horizontal residual
// run 1 reported -- reversed and smaller -- is what a projection frame already
// carrying a fraction a of the head rotation gives, so the exact gain is 1-a
// and F6 steps the ladder 1.00, 0.85, 0.70, 0.55, OFF.
//
// THE INSTRUMENT (every rung). At most 4 lines/s while any vec3 kind fires:
// P, P', E, the body and head angles, and the marker's azimuth expressed off
// BOTH forwards plus where the write actually put it -- the model, in degrees,
// on one line. Run 1 printed the watched widget's produced screen position
// here instead and it read one of two clamp constants on every sample, so that
// field is gone. Per-kind counters and a 5 s heartbeat naming the dead stage.
// ---------------------------------------------------------------------------

void WorldMarkerTick(bool inMap);
// INI `marker.hook` (default 1). 0 = touch nothing.
void SetWorldMarkerHookWanted(bool wanted);
// INI `marker.reframe`: 0 off (measure only), 1 reframe on.
void SetWorldMarkerArm(int arm);
int WorldMarkerArm();
// INI `marker.gain`: the fraction of the head yaw delta the reframe applies.
void SetWorldMarkerGain(float gain);
float WorldMarkerGain();
// F6 this build: steps the gain ladder 1.00 -> 0.85 -> 0.70 -> 0.55 -> OFF ->
// wrap, each press logging the rung it entered.
void StepWorldMarkerArm();
void RemoveWorldMarkerHook();

// ---------------------------------------------------------------------------
// THE LANE CENSUS -- solve the engine's projection instead of guessing it.
//
// Two builds of guessed frames each traded one residual for another, and the
// wearer is right that a scalar gain cannot nail a projection. What settles it
// is the marker's ACTUAL screen position paired with the world point we handed
// over and the view at that instant: the transform is then two unknowns and a
// pile of samples, solved offline, exactly.
//
// rui_probe calls this on EVERY draw of the widget `rui.marker_target` names,
// with a pointer to that widget's parameter block. Eight lanes (block +0x40
// through +0x5C) are sampled against the head's yaw and pitch, and the running
// Pearson correlation of each lane is printed. THE INSTRUMENT NAMES ITS OWN
// SIGNAL: the lane that is the screen position is the one that correlates; if
// no lane does, the block is not where the position lives and the answer is
// the vertices instead. Read-only, and it needs no key and no posture.
void WorldMarkerRecordWidgetDraw(const float* blockFloats);

// ---------------------------------------------------------------------------
// THE A/B THAT SHOULD HAVE COME FIRST: our OWN transform on the marker's pass.
//
// `hud2d.zoom` (shipping at 0.262) scales the whole screen-space pixel-ortho
// HUD pass about the GUN's aim anchor -- camera_update_hook.cpp ~4885, gated on
// IsScreenSpaceHudPass. The waypoint widget is type 3 at 1920x1080 and draws in
// that pass, so whatever screen position the engine computes for it, WE then
// remap as
//     displayed = anchor + 0.262 * (engine position - anchor)
// That attenuates every motion of the marker roughly fourfold and drags it with
// the gun. "Moves in the direction my head turns, but not at the same rate" --
// the symptom this whole front started from -- is exactly what that produces,
// and no marker build has accounted for it.
//
// So: F6 toggles the zoom between the configured value and 1.0, with the marker
// correction OFF, and the wearer says whether the marker's head-drift changes.
// It costs one look. The HUD and reticle change size while it is at 1.0; the
// same key puts them back.
void ToggleHud2dZoomForMarkerTest();
