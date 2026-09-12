#pragma once

// ---------------------------------------------------------------------------
// THE CONTROLLER DRIVES THE GAME'S OWN INPUT STATE.
//
// Two routes were already closed before this one, and neither should be
// re-derived:
//
//   XInput      MEASURED CLOSED 2026-08-17. The module that tried it,
//               xinput_bridge, was deleted 2026-09-09. inputsystem.dll
//               imports no XInput at all -- raw HID, SETUPAPI and HID.DLL --
//               and with the hook armed the game made ZERO XInputGetState
//               calls across two attempts.
//   ReadUsercmd server.dll+0x2603F0 reads fine, but it DELTA-DECOMPRESSES from
//               the previous command, so it is unsound to write. It is also the
//               SERVER's copy, and the client predicts from its own -- the
//               adversarial review flags that a write there can be invisible
//               because the client draws its own prediction.
//
// So drive what the keyboard drives. Every `+command` in this engine sets a
// `kbutton_t`, and the engine folds those into the usercmd itself -- on the
// client, before prediction, before networking. Writing them means movement,
// jumping, firing and reloading all take the game's own path, with its own
// acceleration, prediction, animation and aim assist, and nothing of ours in
// the loop.
//
//   kbutton_t { int down[2]; int state; }   state bit 0 = held, bit 1 = pressed
//
// Confirmed by disassembling IN_JumpDown at client.dll+0x252910: it writes
// down[0]/down[1] and then `or state, 3`, which is Source's KeyDown exactly.
// Every handler below is the same 109-byte shape and every one is a .pdata
// FUNCTION ENTRY, checked rather than assumed -- though nothing here is hooked,
// so that check is corroboration that the address is the function it should be
// rather than a hooking prerequisite.
//
// WE WRITE THE STATE, WE DO NOT CALL THE HANDLERS. The handlers parse a console
// command argument and take a key number; calling them would need a fabricated
// command context. The state word is the whole output of that parsing, so it is
// the thing to write.
//
// MAPPING, as asked for:
//
//   left stick            move  (forward / back / strafe)
//   right stick X         turn  (the game's own +left / +right)
//   right stick Y up      d-pad up   -- answers a two-option prompt, and
//   right stick Y down    d-pad down    nothing else. Jump is A, crouch is B.
//   right trigger         fire
//   right B (secondary)   crouch
//
// Movement and turn are DIGITAL for now, because kbutton_t is a digital latch:
// off-centre past a deadzone reads as the key being held. That is playable and
// it is honest about what it is. Analogue movement needs the client's own
// usercmd `move` field at its CreateMove seam, which is a separate piece of
// work and is not this.
// ---------------------------------------------------------------------------

// Resolves the kbutton addresses and verifies each handler's prologue. Refuses,
// loudly, on any mismatch rather than writing to an address that moved.
bool EnsureVrInputResolved();

// Per plugin frame: reads the published controller state and drives the
// kbuttons. Releases everything it holds when it is disarmed, so disarming can
// never leave the player walking into a wall.
void AdvanceVrInput();

// INI: `set vrinput.enabled = 1`. Same key arms and disarms.
void ToggleVrInput();
void SetVrInputEnabled(bool enabled);
// The INTENT behind vrinput.enabled. See the note at the definition: this is
// the faithful mirror of what SetVrInputEnabled stored, where IsVrInputEnabled
// reports whether it has actually managed to arm yet.
bool IsVrInputWanted();
bool IsVrInputEnabled();

// FORCE CONTROLLER MODE. Swaps the vtable slot behind the game's own
// IsControllerModeActive native, whose help text reads "Returns whether the
// user is using a controller instead of a keyboard". INI: input.force_controller_mode.
// Default OFF; reversible by not setting it. Note that prompt mode is measured
// to be LAST INPUT DEVICE, so this may not be sufficient on its own.
void SetForceControllerMode(bool force);
bool IsControllerModeBelievedActive();

// INTERIM: A (right primary) sends SPACE outside the menu, for screens that
// read "press A" under forced controller mode but are still dismissed by the
// keyboard SPACE. INI: input.a_sends_space. Removed once a virtual gamepad
// sends real buttons.
void SetASendsSpace(bool enabled);

// input.keyboard_menu_nav. Retires the injected-key menu path in one switch.
void SetKeyboardMenuNav(bool enabled);

// input.hold_keyboard_mode. Holds the game own controller-mode byte at 0 so the
// dropship continue gate keeps skipping while the forced native keeps glyphs.
void SetHoldKeyboardMode(bool hold);

// Called every tick; see the note at its definition.
void HoldGameControllerFlagDown();

// Read-only object watch; see the note at its definition.
void WatchControllerModeObject();

// Raw XR button edges, ungated; see the note at its definition.
void WatchRawControllerButtonEdges();

// ---------------------------------------------------------------------------
// WHICH GRIP THROWS THE GRENADE: the RIGHT one, and it is not configurable.
//
// kb_act.lst is authoritative on what the two offhand commands ARE --
// +offhand0 is #ORDNANCE_GRENADE and +offhand1 is #TACTICAL_ABILITY -- and the
// grenade goes on the right, beside the gun hand, with the tactical on the
// left. That matches the game's own pad layout, where the grenade is the right
// shoulder.
//
// It was a setting, input.grenade_hand, and it is gone: a player who wants them
// the other way round rebinds the shoulders in the GAME, which this mod feeds
// through unchanged. A second place to express the same preference is a second
// place for it to disagree with itself.

// ---------------------------------------------------------------------------
// CROUCH. input.crouch_mode, and it is three-valued because the engine turned
// out to own a toggle-crouch command of its own.
//
//   0  HOLD              +duck, asserted while the right stick is held down.
//                        The original behaviour.
//   1  ENGINE TOGGLE     +toggle_duck -- the game's own #TOGGLE_CROUCH, which
//                        is the very command its Hold/Toggle menu option picks
//                        between. Default. The engine does the toggling, so its
//                        interactions with slide, wallrun and titan entry are
//                        the game's own and not ours to get wrong.
//   2  OUR TOGGLE        our edge-detect over +duck. A FALLBACK, and it exists
//                        purely to save a headset run: if 1 does not toggle,
//                        that is one INI digit rather than a rebuild.
//
// WHAT IS STILL NOT DELIVERED: reading which of the two the GAME's own option
// has selected, so the mode follows it automatically. kb_act.lst lists both
// "+duck" -> #CROUCH and "+toggle_duck" -> #TOGGLE_CROUCH, which is what makes
// mode 1 possible and also what says the game's setting is a BINDING choice
// rather than a convar -- so reading it means reading the player's bind, which
// is not done. Until it is, this setting says what it does and does not claim
// to follow anything.
void SetCrouchMode(int mode);
int CrouchMode();

// ---------------------------------------------------------------------------
// RIGHT-STICK CROSS-TALK. input.turn_deadzone, input.jumpcrouch_deadzone,
// input.stick_cross_ratio.
//
// The right stick carries turn on X and jump/crouch on Y. With a plain per-axis
// threshold one diagonal push satisfies both, so turning clips a crouch and a
// crouch clips a turn -- reported by the wearer from both sides.
//
// The fix is AXIS DOMINANCE rather than a bigger deadzone: an axis counts only
// if its own deflection is at least stick_cross_ratio times the other's. At the
// default 0.6 that is roughly a +/-31 degree exclusive cone around each axis,
// with a wide diagonal band where BOTH still fire -- deliberately, because the
// ask was for a concerted push, not for simultaneous turn-and-crouch to be
// removed. Raising the ratio narrows the diagonal band; 0 restores the old
// per-axis behaviour exactly.
void SetTurnDeadzone(float value);
void SetJumpCrouchDeadzone(float value);
void SetStickCrossRatio(float value);
float TurnDeadzone();
float JumpCrouchDeadzone();
float StickCrossRatio();

// ---------------------------------------------------------------------------
// THE GAME'S OWN PAUSE, for the config panel. PLAN-VRMENU 2.4.
//
// Opening our panel pauses the game by opening the GAME's menu, and that is
// deliberate rather than incidental: MenuIsOpen() going true is what already
// makes vr_input call ReleaseAll() and return before any Apply(). So gameplay
// input suppression comes from the shipped, measured path rather than a second
// gate bolted on beside it.
//
// GameMenuIsOpen() is the same predicate that branch uses -- exposed, not
// duplicated -- so the caller can tell whether a pause is needed or whether the
// wearer already had a menu up. Sending ESC blind would CLOSE a menu that was
// already open and unpause the game underneath our panel.
bool GameMenuIsOpen();
// Sends ESC once. Only meaningful when it will actually change the state; the
// caller checks GameMenuIsOpen() first and remembers what it did, so closing
// does not leave the pause half-armed.
void SendGameMenuToggle();


// The TRACKED menu belief, flipped by the menu button and by a physical Escape.
// Pair it with GameMenuIsOpen(): the cursor test that backs the latter was
// measured dead in VR (the cursor is not shown while the pause menu is up), so
// anything gated on the cursor alone gates away the buttons that menu needs.
// Neither is reliable alone. OR them, and log both terms.
bool GameMenuBelievedOpen();

// True from the frame the L3+R3 chord latches until BOTH clicks are released.
// Exposed because the SYNTHETIC PAD must honour it too -- vr_input suppressing
// sprint and melee on the kbutton route means nothing if xinput_pad reports the
// same clicks by its own route. It is never true before the chord latches, so
// it costs the first click no latency.
bool ConsumeThumbClicks();

// input.turn_speed -- degrees per second for stick turning; 0 leaves the game's
// own cl_yawspeed alone. Set through the engine's console, because turning
// already goes through the engine's +left/+right and cl_yawspeed is the number
// that path reads. A turn rate of our own would be a second mechanism for the
// head-tracking composition to disagree with.
void SetTurnSpeed(float degreesPerSecond);
float TurnSpeed();

// input.snap_turn -- degrees per flick; 0 is smooth turning. Built on the same
// +left/+right path: the button is held for degrees/yawspeed seconds, so it is a
// very fast turn rather than an instantaneous jump, and it cannot fight the
// head-tracking composition the way a direct view-angle write would.
void SetSnapTurnDegrees(float degrees);
float SnapTurnDegrees();

// Applies the stored turn speed once engine.dll is loaded. Per plugin frame.
void AdvanceTurnSpeed();

// input.turn_mode -- 0 = smooth, 1 = snap. A MODE, not a magic zero on the snap
// angle: the two are different behaviours with different settings, and encoding
// "snap off" as an angle of zero meant you could not keep a 30-degree step while
// switching back to smooth.
void SetTurnMode(int mode);
int TurnMode();

// The fraction the synthetic pad multiplies the right stick's X by, from
// input.turn_speed. Turning is double-routed -- the pad's stick turns through
// Respawn's own gamepad look, which ignores cl_yawspeed -- so the setting scales
// what we SEND rather than trying to configure a path we cannot name.
float TurnStickScale();

// input.haptic_strength -- multiplier on the game's own rumble, as a fraction.
// A pad's weighted motors and a controller's linear actuator do not deliver the
// same force for the same nominal amplitude, so the game's number passed through
// unchanged under-drives the controller. 0 turns rumble off.
void SetHapticStrength(float percent);
float HapticStrength();

// input.turn_curve -- the response curve on the turn stick. 1.0 is linear;
// higher values give finer control near centre while keeping the same maximum.
// Linear after a large deadzone spends most of the remaining travel near the
// top, which reads as an abrupt ramp.
void SetTurnCurve(float curve);

// AIM DOWN SIGHTS, HELD. The latched left-trigger state that drives the
// engine's own +zoom kbutton -- the same latch, read, so the brace and the
// command can never disagree about whether the wearer is aiming.
//
// It is false whenever vr input is not enabled, which flat it never is. The
// brace therefore has ads.brace_force for a flat run, and says so in its log
// rather than reading as "the brace does nothing".
bool IsAdsHeld();
float TurnCurve();

// ---------------------------------------------------------------------------
// THE LEGACY ENGINE-WRITE ROUTE. `input.kbuttons`, default 0 = RETIRED.
//
// The plugin used to deliver every button TWICE: once by writing the engine's
// own kbutton state directly, and once through the synthetic pad. Two owners
// per action meant a hand-written referee rule for each, and those rules --
// with their "is a game menu up" predicate -- are what stopped B backing out
// of the game's own d-pad screen while A still selected it.
//
// The pad is now the only route, which is what every comparable mod does
// (Halo MCC VR, BioShock and Cyberpunk all hook XInput and write no engine
// memory; only CoD4 VR writes engine input, and it is a source port that owns
// its engine). The game's own controller bindings therefore decide what each
// button means, in gameplay and in menus alike.
//
// 1 restores the old behaviour, including the menu button's injected ESC
// keystroke, in one ini line. It is kept only until a headset run confirms
// nothing was lost; then the flag, the key and the branches go together.
void SetKbuttonRouteEnabled(bool enabled);
bool IsKbuttonRouteEnabled();

// The game's OWN input-mode flag, read directly rather than through the native
// we forced: 1 controller, 0 keyboard, -1 unreadable. Read-only.
int ReadGameControllerModeFlag();
