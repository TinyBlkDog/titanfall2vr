#pragma once
// THE EYE POSITION, RAISED AT ITS SOURCE.
//
// The height defect is a game constant: the player's camera sits at 83.3% of
// the hull (view offset 60.00 of 72.00), where a human eye sits at 93.3%, so
// every headset reads neck-level at an offset of zero (HANDOFF-2026-08-28-
// EVENING s2). Raising the RENDER camera fixes the height and breaks the aim,
// because the round leaves the game's own eye: with the camera R units above
// it a mark drawn where the aim direction projects sits above the impact by
// atan(R / distance), and the wearer reported exactly that with +12 armed
// (A0, 2026-09-02: at 0 the aim agrees and the height is neck-level).
//
// So the one quantity every consumer must agree on is the EYE POSITION, and
// the engine has one getter for it: the virtual EyePosition on the player,
// which the script natives call and which returns origin + view offset via an
// out-parameter. Resolved offline with pescan from the shipped binaries:
//
//   client.dll  C_Player primary vtable 0x8C7A58, slot 0x5A0 -> 0x2C3F40
//   server.dll  CPlayer  primary vtable 0x9524F8, slot 0x428 -> 0x5CF7D0
//
// Both were found from the ScriptEyePosition native (client 0x3DBA80, server
// 0x40A4F0), each of which does `mov rax,[rcx]; call [rax+slot]`, and both
// vtables from the RTTI complete object locators of ".?AVC_Player@@" and
// ".?AVCPlayer@@" (object offset 0). Neither slot function has a single direct
// caller: every use is virtual, so swapping the slot covers them all.
//
// A vtable slot swap, the preferred interception class in this project. The
// interceptor calls the original and adds the raise to z of the returned
// vector. No stored state changes, so prediction has nothing to fight -- the
// gun vibration that killed the m_vecViewOffset write cannot recur here.
//
// PROVE THE SEAM FIRST. The hook installs with the raise NOT in force (a bit-
// exact pass-through) and counts every call per side, with the first distinct
// return addresses resolved to module+rva so the log names the camera and the
// fire paths. F5 (`eye.raise.toggle`) then applies `eye.raise` and removes it
// again, so one run carries the control and the treatment and the heartbeat
// prints the value in force beside every count.

void PlayerEyeHookTick();

// eye.hook: install (1) or leave alone (0). Installs lazily when each module
// is present; the server side arrives with the first level.
void SetPlayerEyeHookWanted(bool wanted);

// eye.raise: Source units added to the eye, the same on every headset, in
// force from the moment it is set (the config menu's live slider lands here).
// FLAT PROOF 2026-09-02: with +10 in force on both sides, impacts stayed on
// the crosshair hip and ADS and the view stepped up; the pass-through control
// in the same run answered ~5,400 client and ~740 server calls a second at
// the game's own 92.03.
void SetEyeHookRaiseUnits(float units);
float EyeHookRaiseConfigured();

// F5, the A/B: drops the raise to zero and brings it back. Logged both ways.
void ToggleEyeHookRaise();

// The raise currently being added, for the census and the crash recorder.
float EyeHookRaiseInForce();

void RemovePlayerEyeHook();

// eye.lean_follow -- OFF by default and refuted by a run: see the note at the
// definition. It moves the SERVER eye so a shot fired while leaning leaves the
// point the reticle is drawn from, but the eye is shared and ADS reads it too,
// so hard-sight alignment breaks. Left reachable for a future attempt that
// moves the shot ORIGIN instead of the shared eye.
void SetEyeHookLeanFollow(bool enabled);
