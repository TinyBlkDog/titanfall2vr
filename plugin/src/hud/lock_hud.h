#pragma once

// ---------------------------------------------------------------------------
// THE MISSILE-LOCK RINGS, AND THE FRAME THEY ARE PROJECTED THROUGH.
//
// Found 2026-09-07 after the RUI widget layer was closed by measurement:
//
//   * `rui_drawEnable 0` blanked the whole HUD and the lock group survived.
//     That convar is NORTHSTAR's, not the game's: its registration at
//     Northstar+0x5D4F0 resolves engine+0xFC500 and hooks it, and the detour is
//     unconditional. So it gates FC500 and nothing else, and the lock group is
//     simply not drawn through FC500 -- which is what this project's census has
//     been saying for 75 runs.
//   * `r_drawvgui 0` killed the rings, the missile icons and the LOCK text all
//     at once. The lock group is VGUI, not RUI.
//   * `#HUD_LOCKED` and `#HUD_LOCKING` are referenced from exactly one
//     function, `client.dll+0x4DFE80`, which is that draw.
//
// WHAT IT DOES. At entry it takes the client view object (`client+0x216F9C0`,
// accessor at vtable +0x60), snapshots the view ORIGIN from +0xE4/+0xE8/+0xEC
// and the view ANGLES from +0xF0/+0xF4/+0xF8, then builds a basis with
// AngleVectors (`client+0x627290`) and projects each target's world position
// against it.
//
// WHY THAT IS THE DEFECT. In flat, aim and view are one thing, so projecting
// through the view angles puts a ring on each target. In VR they are split and
// this plugin drives the engine's angles from the hand, so the lock group is
// projected through a frame that follows the motion controller. The rings
// therefore sit relative to the aim square instead of on the targets, which is
// exactly what the wearer sees.
//
// THE FIX. Wrap that one function. For the duration of its call, and for
// nothing else, the view object carries the angles the frame was actually
// RENDERED with instead of the aim's. Pitch and yaw only: roll is left alone
// because nothing in the wearer's report is about head tilt and a needless
// third axis is a second thing to be wrong.
// ---------------------------------------------------------------------------

// `lockhud.viewfix` in the ini. 0 ships; the hook is not installed until asked.
void SetLockHudViewFix(bool on);
bool LockHudViewFixOn();
float LockHudRingScale();
// Multiplies the ring size the game asks for; the ring centre is held fixed.
void SetLockHudRingScale(float k);

// F3: flip it live, so one press is an A/B the wearer can look at.
void ToggleLockHudViewFix();

// F5: flips r_drawvgui through the engine cvar interface and logs before/after.
// Anything still on screen with it at 0 is not VGUI. Read-only otherwise.
void ToggleVguiDraw();

// Per plugin frame: installs once client.dll is up, and prints the status line
// on its own clock. Every early return is counted and every counter prints
// whether it is zero or not.
void AdvanceLockHud(bool worldReady);

// lockhud.census: the 5 s status/census dump. OFF by default; it cost 102-158
// ms in the frame it landed on.
void SetLockHudCensusEnabled(bool enabled);
