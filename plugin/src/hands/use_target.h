#pragma once

// ---------------------------------------------------------------------------
// USE TARGETING -- make "which item am I pointing at" follow the GUN, not the
// head. docs/USE-TARGETING-2026-09-05.md is the diagnosis; this is the build.
//
// THE CHAIN (client.dll, every RVA .pdata-checked with pescan extent):
//   0x149CC0 per-frame local-player update
//     -> 0x15FD00 UpdateUsePrompt
//       -> 0x2C50E0 FindUseTarget: origin = EyePosition() [slot 0x5A0, already
//          ours], angles = EyeAngles() [slot 0x5B0, THE HEAD], then
//       -> 0x2C44B0 search(player, &origin, &angles, range, flag) -> entity
//          which converts `angles` to a forward vector at its first call
//          (0x6274C0 = AngleVectors: cos/sin of pitch and yaw) and ranks the
//          candidates near that ray.
//
// THE SERVER HAS A TWIN, AND THE ACTION LIVES THERE. Run 1 (2026-09-05, client
// only): the highlight followed the gun and the X swap picked a DIFFERENT item.
// The aim write puts the hand in the command's ATTACK angles, not its view
// angles, so the server's CPlayer::EyeAngles is still the body's and its own
// search (server.dll+0x5CFD00, one caller 0x5D0777 after EyePosition slot
// 0x428 and EyeAngles slot 0x438) picks along the body. Both sides are wrapped
// the same way; the ladder arm is shared and the counters are per side.
//
// Each search has exactly ONE caller (pescan callers) and no vtable
// reference, so wrapping its entry touches nothing else in the game. The wrap
// replaces the `angles` argument with the composed hand angles the aim write
// already produces every tick (hand_pose.h TryGetComposedHandPose: Source
// pitch/yaw/roll, the same triple aim_cmd writes into the command). Shots
// leave the eye along those angles, so eye origin + hand angles IS the
// reticle's ray -- arm 1 is exactly "select what the reticle is on".
//
// THE LADDER (F6 steps it; `use.aim` sets the arm the run starts on):
//   0  OFF. Pass-through. The interceptor still measures: what the game passed
//      versus the hand, so a run at 0 says whether EyeAngles was ever the head.
//   1  angles := hand angles. The narrowest fix and the one the doc specifies.
//   2  angles := hand angles AND origin := hand position. The ray leaves the
//      controller itself rather than the eye. Same lever, one rung further.
//
// EVERY STAGE COUNTS ITSELF. calls (the wrap fired), no-pose (hand pose not
// valid -> pass-through), off (arm 0), along-hand, from-hand, hits (search
// returned an entity), faulted. The 5 s heartbeat prints them with a verdict
// naming the dead stage when one is dead, so a null in the headset costs a
// log read and not a run.
//
// `use.hook = 0` leaves the game's code untouched (rollback without a build).
// ---------------------------------------------------------------------------

// Per frame from RunFrame. Lazy install once client.dll exists (byte-verified;
// refuses loudly on a different build), then the heartbeat. `inMap` is what
// the heartbeat uses to tell "no calls because we are in the menu" from "no
// calls because the wrap is dead".
void UseTargetTick(bool inMap);

// INI `use.hook` (default 1). 0 = never patch anything.
void SetUseTargetHookWanted(bool wanted);

// INI `use.aim` = 0, 1 or 2 (see the ladder above). Applies at once.
void SetUseAimArm(int arm);
int UseAimArm();

// F6 this build: 0 -> 1 -> 2 -> 0, each press logs the state it entered.
void StepUseAim();

void RemoveUseTargetHook();
