#pragma once

// ---------------------------------------------------------------------------
// PLAN-CURRENT F2 -- DISPLACE THE REAL VIEWMODEL, ANIMATIONS INTACT.
//
// The acceptance test enters here, not at the end: the REAL weapon and arms,
// moved somewhere unmistakable, with fire/reload playing normally -- because
// nothing touches animation, only where its root sits.
//
// Mechanism: wrap client.dll+0x3DB4A0 (CalcAbsolutePosition, the placement
// commit F1 found at 2.000/frame), let it run so the engine's own computation
// and its angle normalise are untouched, then overwrite the committed
// placement:
//
//     position  +0x12C / +0x130 / +0x134     <- engine value, z + 30
//     angles    +0x114 / +0x118 / +0x11C     <- engine value, yaw + spin(t)
//
// ABSOLUTE, NEVER A DELTA. Each write is computed from the value the ENGINE
// just committed on this same call, so re-applying it is idempotent and it
// cannot compound. The wrapper additionally refuses to run on the calls where
// the original early-returned, which is the case where the value read back
// would be our own.
//
// The synthetic drive is deliberately not the hand pose. H1 swaps the source;
// F2 has to answer "does driving this field move the REAL gun, and does it
// keep animating", and a spin plus a fixed offset answers that on a flat
// screen with nobody wearing anything.
// ---------------------------------------------------------------------------

// Installs the wrapper if it is not installed, then arms/disarms the drive.
// Patches code, so it is hotkey-gated and never arms itself on load -- this
// project's standing rule for anything that writes into the game.
void ToggleViewmodelPlacementPin();
bool IsViewmodelPlacementPinArmed();

// Per plugin frame: emits the falsifier line and closes the capture on its own
// clock. Costs one atomic load when nothing is armed.
void AdvanceViewmodelPlacementPin();

// pin.report: the 4 Hz placement diagnostic block. OFF by default; it cost
// 6.0-7.6 ms in the frame it landed on. Does not affect the pin itself.
void SetPlacementPinReportEnabled(bool enabled);

// H1. Swaps the pose source from the synthetic offset to the verified hand
// composition. INI: set pin.hand = 1, reloadable with LEADER then END.
void SetPlacementPinHandDriven(bool hand);
bool IsPlacementPinHandDriven();

// What the pin last WROTE and what the engine had committed before it, so a
// trace can tell "we did not write it" from "we wrote it and it did not move".
bool TryGetPlacementAngles(float applied[3], float base[3]);
// The same for the position: what the pin wrote, and what the engine committed.
bool TryGetPlacementPosition(float applied[3], float base[3]);

