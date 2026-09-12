#pragma once

// TASK 05 STEP 2 -- WHAT DOES THE ENGINE USE TO DECIDE WHERE A BULLET GOES?
//
// THE CONTRADICTION THIS EXISTS TO SETTLE.
//
// Two documents in this repo disagree about the single fact the whole of Task 05
// turns on, and both are first-hand:
//
//   00-SHARED-CONTEXT.md (Task 01, 2026-08-15) says the angle write at
//   g_cameraStructAddress+0x0C "rotates the view without rotating the player --
//   movement, the reticle, world-anchored HUD markers and BULLET IMPACTS all
//   stay with the body", and concludes that "aim/view decoupling is therefore
//   the default, not something to build".
//
//   HEADSET-ISSUES-2026-08-16.md issue 4 says the opposite: "head tracking
//   writes the camera angles that ALSO drive the body and aim, so looking down
//   and yawing carries the body around" -- reported from the headset as the
//   shoulder rotating away when you try to look over it.
//
// If the first is right, aim already has its own angles and Task 05 is mostly
// done before it starts. If the second is right, aim must be separated and the
// choice of strategy in step 3 is a real decision. Building on either without
// evidence would be building on a coin toss, and this project's own operating
// notes say every wrong answer in it came from comparing a run to a memory of a
// run.
//
// THE INSTRUMENT IS THE ENGINE'S OWN.
//
// The cvar enumeration found `sv_showfiredbullets` and `cl_showfiredbullets`,
// neither of them cheat-flagged, so the engine will report every bullet it
// fires without sv_cheats. Northstar already hooks the engine spew function and
// writes it to R2Northstar/logs when `spewlog_enable` is set, so the output is
// captured with NO hook of our own -- which matters, because double-detouring a
// function Northstar has already detoured is exactly the class of thing that has
// broken this project before.
//
// So this file only runs console commands, through the same Cbuf path the
// resolution setting already uses. It patches nothing and hooks nothing.
//
// Hotkey-gated and self-reverting: a second press turns every one of them back
// off, because these are live engine settings and one of them prints per shot.

// Toggles the engine's own fire-path reporting on and off.
void ToggleAimProbe();
bool IsAimProbeArmed();

// Reports, by name, which of the cvars step 2 cares about actually exist on this
// build, with their current values and the flags that decide whether they can be
// set at all. Read-only.
void LogAimRelatedCvars();

// TASK 05 ROUTE B PRECONDITION -- MAKE BONE SETUP DETERMINISTIC AND COMPLETE.
//
// Pinning the gun to the hand means writing the engine's own evaluated skeleton
// (see TASK-05-STEP-3-4-CLOSURES). On this build that skeleton is computed
// threaded, asynchronously and SPECULATIVELY:
//
//   cl_threaded_bone_setup 1, cl_async_bone_setup 1,
//   cl_parallel_clientside_animations 1, cl_bones_speculative 1
//
// A write racing that setup fails INTERMITTENTLY, which is the hardest kind of
// failure to attribute and one this project has already lost days to. Worse,
// cl_SetupAllBones is 0, so the engine deliberately skips bones it believes it
// does not need -- the "Don't reblend / Don't retransform bones which we don't
// need to in SetupBones" help strings are precisely that -- and the hand bone may
// therefore never be computed.
//
// So this turns threading, asynchrony, speculation and incremental skipping off
// and forces every bone to be set up, BEFORE any bone write is attempted. It
// converts a race into a repeatable experiment.
//
// None of these are CHEAT-flagged, so all are reachable by name. Self-reverting:
// a second press restores every one of them to the value it had on arm, read back
// rather than assumed.
void ToggleDeterministicBoneSetup();
bool IsDeterministicBoneSetupArmed();
