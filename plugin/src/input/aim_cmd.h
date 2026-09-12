#pragma once

// ---------------------------------------------------------------------------
// PLAN-CURRENT P0 -- THE CLIENT COMMAND-BUILD SEAM.
//
// Target: client.dll+0x254D10, CInput::CreateMove. Identified from the kbutton
// fold as the plan directed: the ten `+command` kbuttons are read by
// GetButtonBits (client.dll+0x251620, CInput vtable slot 3 at vtable+0x18),
// whose only caller is client.dll+0x254D10 at +0x2B3 -- and the very next
// instruction there is `mov [rdi+0x40], eax`, which IS `cmd->buttons`. So the
// seam is not inferred from a name; it is the function that folds our own
// button latches into a CUserCmd.
//
// .pdata: a SINGLE entry, 0x254D10..0x2554B1, no UNW_FLAG_CHAININFO. It is a
// real function entry and not a continuation fragment -- the trap F1 paid for.
//
// CUserCmd, confirmed CLIENT-SIDE from CreateMove's OWN stores (the plan said
// verify rather than assume the server layout transfers -- it does):
//
//     +0x00  command_number        mov [rdi], ebx        (the sequence arg)
//     +0x04  tick_count
//     +0x0C  worldViewAngles[3]    movss [rdi+0xC/0x10/0x14]  <- from the
//                                  engine's GetViewAngles, [C3D940] vtable
//                                  +0x1F8, called at CreateMove+0x22F
//     +0x18  flag byte, +0x1C vec3 (a second, gated angle triple)
//     +0x28  attackangles[3]       NOT WRITTEN BY CreateMove -- see below
//     +0x34  move[3]
//     +0x40  buttons               mov [rdi+0x40], eax
//     +0x44  impulse
//     +0x50  a rolling hash of +0x28 and +0x0C
//     size 0x148; commands are a 300-entry ring.
//
// THE THING THAT WOULD HAVE COST FR2, AND WHY THE HOOK IS SHAPED LIKE THIS.
//
// The command is FROZEN when CreateMove returns. Its last three acts are:
//
//     +0x761  call 0x33C580   hash(+0x9C, +0x28, +0x08, +0x0C, +0xA0) -> cmd+0x50
//     +0x76C  call 0x33C3A0   copy cmd -> m_pVerifiedCommands[i].m_cmd
//     +0x774  call 0x33C650   CRC32 over the fields -> m_pVerifiedCommands[i].m_crc
//
// and before the command is serialised, CInput::WriteUsercmdDeltaToBuffer
// (0x256600) calls CInput::VerifyUserCmd (0x256430), which recomputes the CRC
// and, ON MISMATCH, COPIES THE STORED VERIFIED COMMAND BACK OVER THE LIVE ONE.
//
// So any write to a CUserCmd made after CreateMove returns reads back
// perfectly and is then silently reverted before it reaches the wire. That is
// the handoff's "a write that reads back proves nothing" in its purest form,
// and it would have produced a clean, confident, wrong null in FR2.
//
// The post-process therefore re-runs the engine's own three calls, in the
// engine's own order, with our value already in place. No CRC is
// re-implemented; we call CUserCmd::GetChecksum itself.
//
// WHAT THE OFFLINE READ FOUND ABOUT attackangles, stated plainly because it
// weakens Candidate A and FR1 is what settles it:
//
//   - +0x28 IS on the wire. server.dll's ReadUsercmd (0x2603F0) decodes three
//     vec3 blocks -- +0x0C, the gated +0x1C, and +0x28 -- and client.dll's
//     WriteUsercmd (0x33CAA0) is its exact structural mirror. So a client-side
//     write does reach the server.
//   - But in the whole of client.dll, +0x28 is only ever READ (the two
//     integrity hashes, the serialiser) and COPIED (operator=). No writer and
//     no consumer was found. CUserCmd::Reset zeroes it and CreateMove never
//     refills it.
//
// Since VerifyUserCmd freezes the command at CreateMove's exit, anything that
// writes attackangles must do so inside CreateMove's dynamic extent -- so
// either a callee this scan missed writes it, or the client genuinely sends
// zeros and the server-side equality the handoff measured came from the
// server's own decode. FR1 answers that in one desk run by logging the field
// the engine leaves behind, which is why FR1 is log-only and comes first.
// ---------------------------------------------------------------------------

// The four states the one hotkey cycles through. Each press logs a banner
// naming the state it entered, so the control cannot be pressed "invisibly".
enum class AimCmdState {
    Off = 0,          // installed or not, the wrapper is a pass-through
    Log = 1,          // FR1: read and log the engine's own two angle fields
    WriteAttack = 2,  // FR2 pulse A / Candidate A: cmd->attackangles
    WriteView = 3,    // FR2 pulse B / Candidate B: cmd->worldViewAngles
};

// Installs the wrapper on first use, then advances Off -> Log -> WriteAttack
// -> Off. Patches code, so it is hotkey-gated.
//
// WriteView is NOT in the key cycle. FR2 measured that a worldViewAngles write
// compounds -- the engine reads it back into its own view state, so +30 a tick
// at 80 Hz is a 2400 deg/s spin. That was a fine positive control at a desk and
// is a hazard in a headset, so it is reachable only from the ini
// (`set aim.cmd = 3`) and never one keypress away from the H1 state.
void CycleAimCmd();
AimCmdState CurrentAimCmdState();

// Per plugin frame: emits the falsifier summary on its own clock. One atomic
// load when nothing is armed.
void AdvanceAimCmd();

// INI, all reloadable with LEADER then END.
//
//   set aim.cmd        = 0..3   arm at this state on load (default 0)
//   set aim.cmd_source = 0|1    0 synthetic yaw offset, 1 composed controller
//                               aim pose (default 0)
//   set aim.cmd_yaw    = 30     degrees, the synthetic offset
void SetAimCmdState(int state);
void SetAimCmdSource(int source);
void SetAimCmdYawDegrees(float degrees);

// THE AIM POINT'S OFFSET FROM THE VIEW, in degrees, for whoever needs to know
// WHERE ON SCREEN the reticle is.
//
// The reticle sits at the aim point -- H1 established that, and it is why the
// aim write moved it. So its screen position is entirely determined by how far
// the aim angles are from the view angles, which is exactly the pair this
// module already reads out of the command every tick.
//
// False when nothing has been read yet, so a consumer can fall back rather than
// use a stale or zero offset.
bool TryGetAimOffsetDegrees(float* yawOffset, float* pitchOffset);

// The ABSOLUTE aim angles in world space -- the value actually in force this
// tick, which is what we wrote while driving and the engine's own attackangles
// when not. Callers that need a SCREEN position must subtract the angles of the
// camera the screen is rendered with, not the command's view angles: those are
// the BODY's, and in VR the render camera is head-driven and pitches
// independently. That mistake put the reticle's estimated anchor 12 degrees low.
bool TryGetAimAnglesDegrees(float angles[3]);

// ---------------------------------------------------------------------------
// SNAP TURN. A one-shot yaw delta, applied to worldViewAngles on the next
// command and then cleared.
//
// INSTANTANEOUS BY CONSTRUCTION. FR2 measured that a worldViewAngles write
// COMPOUNDS -- the engine reads it back into its own view state -- so applying
// one for a single tick moves the view by the delta and leaves it there, with no
// intermediate frames at all. That is what snap turn has to be: the point is to
// remove the optical flow that causes the vestibular mismatch, and a fast smooth
// sweep can make a motion-sick player worse rather than better.
//
// Requests REPLACE rather than queue: a fast double-flick should turn once.
void RequestSnapYaw(float degrees);
// Whether the command wrapper is installed. A snap request with no wrapper
// vanishes silently, which reads as "snap turn does nothing".
bool SnapTurnAvailable();
unsigned long long SnapTurnWrites();

// ads.face_aim. On ADS entry, turn to face where the GUN was pointing instead
// of leaving the view where it is and letting the gun adopt it.
//
// YAW ONLY, and the asymmetry is not an oversight. The rendered yaw is
// baseYaw + headDelta, so writing the base moves it; the rendered PITCH is the
// headset's own absolute pitch and there is no base term to write. Giving it
// one would mean level in the headset stops being level in the world, which is
// a persistent vestibular mismatch rather than a transient one.
void SetAdsFaceAim(bool faceAim);
// ads.face_aim_return (default 1): on ADS release the body turns back by the
// face-aim entry turn and the turn leaves the hand compensation, so nothing
// accumulates across presses. 0 keeps the pre-2026-09-04 behaviour.
void SetAdsFaceAimReturn(bool enabled);
bool IsAdsFaceAimReturn();
bool IsAdsFaceAim();

// The engine's own worldViewAngles as of the last command, for a trace that has
// to tell the body's view from the head's render and from the hand.
bool TryGetEngineViewAngles(float angles[3]);

// ads.face_aim_pitch. The ADS entry turn, in the vertical.
//
// Separate from ads.face_aim because it is the half with a real chance of being
// sickening: while the offset is non-zero, level in the headset is not level in
// the world. The yaw turn is settled and good, so keeping one and dropping the
// other has to be possible without a rebuild.
void SetAdsFaceAimPitch(bool enabled);
bool IsAdsFaceAimPitch();

// While ADS follows the hand (ads.face_aim), the rendered view angles the gun
// is drawn at and the round is fired along. False outside that state.
bool TryGetAdsViewAngles(float out[3]);
