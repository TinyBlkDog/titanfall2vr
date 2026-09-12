#pragma once

// C1 -- THE AIM CENSUS, H3's RIDER, AND D1, IN ONE READ-ONLY INSTRUMENT.
//
// PLAN-HEIGHT-2026-08-28 section 2. Three log streams, one build, one run,
// numbers only. Nothing here writes to the game, patches code, or installs a
// hook; the single behavioural variable in the build is the blind height
// window below, and it is driven by the WEARER, never by this file.
//
// WHY ONE INSTRUMENT AND NOT THREE. Runs are the scarce resource and rebuilds
// are free, so read-only probes stack: three streams that share a sample index,
// an arm label and a headset fingerprint can be joined line-by-line after the
// run, and each answers a different hypothesis in the plan's tree.
//
//   AIM  -- H2. Where the reticle anchor is put, WHICH half-tangent divided,
//           what the two accessors each say, the world pass's viewport against
//           the HUD pass's pixel extents, and the angles the offset came from.
//   POSE -- H3's free rider. Raw LOCAL head y, the recentre reference, the
//           layer's declared pose y and projectionOrigin_. Under H1 these are
//           identical across headsets; under H3 one of them differs by ~0.32 m.
//   CHAR -- D1. The faced character's own eye/head attachment height against
//           the game camera. H1 predicts characterEye - camera = +10 units; a
//           reading of 0 refutes H1 and that is a result worth having.
//
// EVERY STREAM PROVES IT IS WATCHING. Each carries its own sample count, and
// the quantities that come from somewhere else carry the update counter of
// that somewhere -- a frozen anchor and a moving one do not look alike.

// Called once per plugin frame. Gated on a world being up at the call site.
void AimCensusTick();

// ---------------------------------------------------------------------------
// WHAT THE XR SIDE KNOWS AND THE PLUGIN FRAME DOES NOT.
//
// The POSE stream's four quantities are produced on the submit and pacing
// threads inside XrContext, which the census cannot reach and must not lock
// against. They are published here as plain values under a generation counter
// and read on the plugin frame, which is the same shape every other cross-
// thread number in this plugin uses.
//
// The generation is not decoration. `projectionOrigin_` is latched ONCE, when
// the projection layer arms -- which with autoarm is during the loading screen
// -- and the game-side recentre does NOT re-take it. A census line that could
// not say how old the value it is printing is would hide exactly that.
void PublishXrLocalHeadCentre(const float metresXyz[3]);
void PublishXrProjectionPose(bool projectionInUse, bool originValid, const float origin[3],
                             const float renderedHeadCentre[3], const float declaredPose[3]);

// WHAT WE ACTUALLY TELL THE COMPOSITOR, per eye, every frame.
//
// This is the one quantity that differs structurally between the headset that
// reads correctly and the one that does not, and it has never been measured on
// both with a single instrument. It has been INFERRED for both -- from the
// code, and from a V2 falsifier line that only fires when the shear changes --
// and inference is what the last two eliminations were made of.
//
// Angles in degrees, rect in pixels of the swapchain image.
void PublishXrDeclaredView(float up, float down, float left, float right, int rectX, int rectY,
                           int rectW, int rectH);

// vr.viewheight -- THE ENGINE LEVER FOR THE HEIGHT. Default 0 = do nothing.
//
// The measurement is settled: the player's collision hull is 72.00 standing and
// 47.00 crouched (confirmed by the crouch moving it), the view offset is 60.00
// and 38.00, so the eye sits at 83.3% of stature where a human eye is at 93.3%.
// The camera is 7.18 units low standing, 5.85 crouched.
//
// `viewheight` is one of the game's own player-settings fields -- "View height
// for current stance" -- and `_setClassVarServer <name> "<value>"` sets a class
// variable, a command Northstar unlocks from FCVAR_DEVELOPMENTONLY. Driving THAT
// means the game recomputes the eye from its own corrected value, so the camera,
// the gun, the bullets and the reticle move together by construction. That is
// the property the render-camera raise lacked (it moved the eye and left the
// shot origin) and the view-offset write lacked (it fought prediction and made
// the gun vibrate).
//
// IT CARRIES ITS OWN FALSIFIER AND NEEDS NO PERCEPTION AT ALL. The census
// already reads the live view offset out of the player entity at +0xAC. If this
// works the CHAR line reports 67-ish instead of 60.00, and if it does not the
// line reports 60.00 unchanged. The wearer is asked nothing.
void SetViewHeightUnits(float units);
void ApplyViewHeightIfRequested();
