#pragma once

// ---------------------------------------------------------------------------
// PLAN-ADS-2026-08-26 S2 -- HOW FAR OFF ARE THE SIGHTS, AND BY WHAT.
//
// READ-ONLY. It calls one engine function and writes nothing at all: not a
// field, not a flag, not the FOV-adjust byte the engine's own
// GetAttachmentOrigin_ViewModelNoFOVAdjust clears around its call. That byte is
// deliberately left alone here -- it affects the attachment's ORIGIN (the
// viewmodel FOV warp translates and scales along the view axis) and not the
// attachment's forward AXIS, which is the only thing this measures. S4 will
// clear and restore it when it needs origins; S2 does not, so S2 stays a pure
// read and the "read-only" claim in the plan survives contact.
//
// WHAT IT MEASURES, and it is THREE angles rather than one, because two of them
// are the control for the third:
//
//   sight-vs-aim   the headline. Between an attachment's forward axis and the
//                  aim ray actually in force this tick.
//   root-vs-aim    THE POSITIVE CONTROL. The viewmodel entity's own placement
//                  angles against the same aim ray. In the shipped VR
//                  configuration this must be ~0 BY CONSTRUCTION -- see below
//                  -- so a large value here means the instrument's frame
//                  conversion is wrong and the headline number means nothing.
//   sight-vs-root  the artist's offset: how far the barrel sits off the model
//                  root's own axis. This is the part that is a per-weapon
//                  constant, and it is measurable with no headset and no pin.
//
// WHY THE THIRD ONE IS THE ANSWER. PLAN-ADS section 5 expected the bullet and
// the model to come from two different controller poses. They do not:
// hand_pose.cpp's TryReadRightPose takes POSITION from the grip pose and
// ORIENTATION from the aim pose, once, and BOTH consumers read that one
// composition -- placement_pin.cpp:142 via TryGetComposedHandOffset, aim_cmd.cpp
// :147 via TryGetComposedHandPose -- which derive angles[] line for line
// identically. The yaw-rebase note at hand_pose.cpp:716 says outright that this
// is deliberate. The calibration offsets (hand.off_pitch/yaw/roll) are folded
// into the composition, so they rotate the ray too and cannot be a source of
// divergence either.
//
// So the model ROOT and the bullet are already the same ray, and every degree
// of C3's error is the offset between the model root and the barrel the artist
// modelled. That is a per-weapon constant, it is what S4 has to correct, and
// none of it needs a headset to measure.
//
// IT PROVES IT IS WATCHING. Every second it prints either a measurement with a
// sample count and min/max, or an IDLE line naming which step failed -- no
// player, no viewmodel, no attachments, the engine refused the attachment, or
// no aim angles yet. Silence is never mistaken for alignment, which is the
// specific failure the plan asks this step to rule out. `aim.cmd = 1` is
// required for the aim ray to exist at all: FR1 established that state Log
// populates it and writes nothing.
//
// COST IS BOUNDED BY CONSTRUCTION: the engine call is made at most ten times a
// second whatever the frame rate, one report line a second, and the
// per-attachment enumeration burst fires only when the model changes.
// ---------------------------------------------------------------------------

// INI: `set ads.probe = 1`. Reloadable with LEADER then END.
void SetAdsProbeEnabled(bool enabled);
bool IsAdsProbeEnabled();

// INI: `set ads.probe_attachment = N`. 0 (default) auto-selects by name --
// anything containing "muzzle", else "sight" or "iron", else attachment 1 --
// and the log names which rule fired. A positive value pins the 1-based index,
// for a weapon whose naming the auto-select gets wrong.
void SetAdsProbeAttachment(int index);

// Per plugin frame. One atomic load when it is not armed.
void AdvanceAdsProbe();

// INI: `set ads.trace = 1`. One line per sample while ADS is engaged, with the
// hand, the view, the round, the gun's committed placement, the head and the
// magnification side by side -- and the two DERIVED on-screen angles that
// separate an aiming error from a rendering one. For controlled single-weapon
// tests, which is what several rounds of judging by description could not do.
void SetAdsTrace(bool enabled);
bool IsAdsTrace();

// ---------------------------------------------------------------------------
// PER-WEAPON GRIP -- KICKOFF-GUN-ALIGN-2026-09-03 rung 2. POSITION ONLY.
//
// The census (rung 1, flat, gun range, 2026-09-03) measured where each pilot
// viewmodel puts its own R_HAND attachment in the model's frame, and the
// R-201 the grip was calibrated on reproduced the live grip to 1.5 units
// (control PASS). The pistols' hand point sits ~6.4 units further forward,
// which is the wearer's "pistol a foot in front of the hand".
//
// So the pin's grip becomes ONE user calibration plus a per-weapon term:
//
//   grip_effective = hand.grip_* + (grip.ref_* - R_HAND_local(weapon))
//
// where grip.ref_* is the R_HAND of the weapon the grip was calibrated on
// (printed by the CALIBRATING line, so a recalibration on any weapon records
// its own reference) and R_HAND_local(weapon) is LATCHED per class index from
// the first steady second the weapon is held: the attachment rides an
// animated hand bone, so the live value is only the grip point at idle, and a
// latched constant cannot follow a reload or sprint animation into the pin.
// The term is a pure position in the weapon frame: hip-fire direction, height
// and ADS never read it.
//
// INI: `set grip.perweapon = 1`, `set grip.ref_fwd / ref_right / ref_up`.
// Bare F5 toggles the term for an in-place A/B. Without a reference, or before
// the weapon in hand has latched, the term is zero and the log says which.
void SetPerWeaponGrip(bool enabled);
void TogglePerWeaponGrip();
bool PerWeaponGripArmed();
void SetGripReference(int axis, float value);
bool ReadGripReference(float out[3]);
// The delta the composition adds to the grip, or false (and zeros) when the
// term is off, unreferenced, or the weapon in hand has not latched yet.
bool TryGetPerWeaponGripDelta(float delta[3]);
// The latched R_HAND of the weapon in hand, for the calibration printout.
bool TryGetLatchedRightHand(int* classIndex, float out[3]);
