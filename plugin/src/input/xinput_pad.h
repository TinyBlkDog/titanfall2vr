#pragma once

// 1.6-B2 -- THE VR CONTROLLERS PRESENTED AS A GAMEPAD, AT THE GAME'S OWN SEAM.
//
// B1 named the seam by reading inputsystem.dll rather than by probing it (see
// B1-XINPUT-SEAM-2026-08-21.md). The chain, every address verified:
//
//   inputsystem.dll loads XInput1_3.dll  (0x77D2, handle at this+0x17D0)
//   InitializeXDevices (0xCEB0) resolves, via GetProcAddress (IAT 0x4C238):
//       XInputGetState        -> global 0x6C840
//       XInputSetState        -> global 0x6C848
//       XInputGetCapabilities -> global 0x6C850
//   InitializeXDevice (0xCE10), USER INDEX 0 ONLY, calls
//       XInputGetCapabilities(0, XINPUT_FLAG_GAMEPAD, &caps)
//   and attaches the slot only if that returns ERROR_SUCCESS *and*
//   caps.SubType == XINPUT_DEVSUBTYPE_GAMEPAD.
//   PollXDevices (0xDE60) returns immediately when the GetState pointer is
//   null, and SKIPS any slot that did not attach.
//
// So this is Halo MCC's lesson present in the binary: satisfy the CAPABILITIES
// call or the game never reads input at all. Answering only GetState would be
// answering a question nobody asks.
//
// WHY THE HOOK IS ON GetProcAddress AND NOT ON XInput ITSELF.
//
// There is no window in which to swap the three globals: InitializeXDevices
// writes them and then calls InitializeXDevice from inside the same function,
// so swapping before is overwritten and swapping after finds the slot already
// unattached for good -- and PollXDevices has no reconnect path.
//
// The 2026-08-17 attempt in xinput_bridge.cpp -- deleted 2026-09-09, see git
// history -- went at the other end, detouring
// XInputGetState's prologue, and its logs say exactly why that failed:
//
//   "NONE of XInput1_3.dll, XInput1_4.dll or XInput9_1_0.dll is loaded in this
//    process, so there is no entry to patch"
//   "XInput1_4.dll XInputGetState prologue matches no known variant ...
//    bytes=E9 2B E1 9A 8E ..."
//
// -- the module was not loaded when it looked, and the one that was had an E9
// hot-patch jump where the matcher wanted a prologue. Hooking inputsystem's own
// GetProcAddress avoids all of it: no prologue is touched, so a detoured export
// is irrelevant; whichever module the GAME resolves is the one intercepted, so
// there is nothing to guess; and installing at inputsystem's module load is
// necessarily before InitializeXDevices runs. It is also the project's standing
// rule -- slot swaps over entry detours -- and the same technique
// rawinput_probe.cpp already performs on this very module.
//
// SHAPE, FROM THE PRIOR ART. BioShock composes synthetic state OVER real-pad
// passthrough and fails soft; that is what happens here. A real pad always
// wins the capabilities call, its state is read first and ours is merged on
// top, and with the VR stack down nothing is added at all.

// Registers the module-load watch. Must be called from plugin init: the swap
// has to be in place before inputsystem.dll runs its device init, and the
// per-tick retry is too late by construction.
void RegisterXInputPadLoadWatch();

// Per-tick retry, for the case where inputsystem.dll was already loaded when
// the plugin arrived. It cannot rescue a missed InitializeXDevices; it exists
// so that state is at least logged rather than silently absent.
void EnsureXInputPadInstalled();

// input.synthetic_pad. Default OFF -- this sits on the game's input path and
// must not be able to disturb a working stack unless it was asked for.
void SetSyntheticPadEnabled(bool enabled);
bool IsSyntheticPadEnabled();

// `input.y_longpress_ms`. How long Y must be held before it becomes the VIEW
// button (XINPUT_GAMEPAD_BACK, bound to `+showscores`, which restores the
// waypoint markers after they fade). 0 DISABLES the mechanism and returns Y to
// firing a weapon change on the press edge.
//
// While this is non-zero a TAP of Y emits the weapon change on RELEASE rather
// than on press, because nothing can know a press will stay short until it
// ends. A hold past the threshold emits BACK and NEVER emits Y -- the wearer's
// explicit constraint. See the long note at the mapping in the .cpp.
void SetYLongPressMs(unsigned ms);

// Once-a-second counters. Called from the game tick.
void LogXInputPadCounters();

// Per plugin frame. Keeps the controllers buzzing while the game's motor state is
// non-zero: a sustained rumble is set ONCE and left, so a change-triggered pulse
// produced one micro-buzz and then silence.
void AdvanceHaptics();
