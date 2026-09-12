#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// THE D-PAD, AND THE VIEW BUTTON, FROM INPUTS NOTHING ELSE OWNS.
//
// Titan mode uses all four d-pad directions, and the View button shows the next
// objective. Between them that is five
// controls, and every button on both controllers is already spoken for: the
// left trigger is ADS, the grips are ordnance and tactical, X is use/reload,
// Y is weapon switch, the stick clicks are sprint and melee, the menu button
// opens the game's menu.
//
// So the d-pad is a MODE over the left thumbstick, entered with a modifier
// that is genuinely unbound, which is how the reference mods do it (BioShock's
// flick modifier is the left thumbrest; the chord is necessarily cross-hand,
// because the thumb driving the stick cannot also hold the rest).
//
// TWO MODIFIER SOURCES, because the wearer owns two headsets with different
// hardware:
//
//   THUMBREST (controllers that expose one, e.g. Quest 3).
//     `/input/thumbrest/touch` is part of the core oculus/touch_controller
//     profile -- no extension. A runtime that does not
//     expose it reports the action inactive, which reads false, so the
//     modifier simply never fires and nothing else changes. The PFD MR
//     controllers have no such pad, which is exactly that case.
//
//   HELD BESIDE THE HEAD (every headset). The left controller raised to head
//     height and held near it -- the wearer's own PFD MR gesture. Built from
//     the grip pose and the head position, both of which are already published
//     and neither of which any control reads for input.
//
// HELD MEANS HELD: the shift is on while the modifier is held and off the
// instant it is released. That is the convention these mods share, and it is
// what the wearer asked for -- an earlier draft made the modifier arm and then
// expire, which would have made a deliberate use fail after a moment's
// hesitation. The .cpp keeps the full note on why that reasoning was wrong.
//
// Movement is untouched whenever the modifier is not held: same frame, same
// threshold, no latency, nothing withheld. While it IS held the stick is the
// d-pad, and movement is released rather than frozen at both consumers.
// ---------------------------------------------------------------------------

// Once per plugin frame, BEFORE the input consumers run, so movement and the
// pad see one decision made from one sample rather than each deciding again.
void AdvanceDpadGesture();

// True while the left stick is acting as a d-pad. Movement must be suppressed
// for exactly as long as this is true, and only while it is.
bool DpadGestureEngaged();

// The direction currently held, as XInput d-pad bits (up/down/left/right), or
// 0 for centred. Only ever non-zero while engaged.
std::uint16_t DpadGestureButtons();

// True while the modifier is held AND the left primary (X) is pressed: the
// View button, which the game uses to show the next objective. Separate from
// the direction bits because it is a different button, not a fifth direction.
//
// WHICH PHYSICAL BUTTON THIS IS, settled 2026-09-05 from the wearer's OWN
// `Documents\Respawn\Titanfall2\local\settings.cfg`, so it is not a guess:
//
//     bind "BACK"  "+showscores"          <- THIS. XINPUT_GAMEPAD_BACK.
//     bind "START" "ingamemenu_activate"
//
// On an Xbox One pad BACK is the **View** button, the one with two overlapping
// rectangles. The three-horizontal-lines button is **Menu** = START, and it
// opens the pause menu -- we already send that from `g_controllerMenu`.
//
// An earlier version of this header called BACK "the one with three horizontal
// lines", which is the Menu icon, not View. That single wrong sentence made the
// wearer reasonably conclude we had mapped the wrong button and had never
// mapped the right one. The mapping was always correct; only this comment was
// wrong. Do not describe a pad button by an icon again without checking.
bool DpadGestureViewButton();

// INI. `dpad.modifier`: 0 off, 1 thumbrest only, 2 held-beside-the-head only,
// 3 either (the default -- on hardware without a thumbrest, 3 behaves as 2).
void SetDpadModifierSource(int source);
int DpadModifierSource();

// INI, the head gesture's geometry. `dpad.head_up` is how far above the head's
// own height the hand must reach, in metres, negative meaning below (the
// default -0.15 accepts chin level). `dpad.head_radius` is how near the head
// the hand must be in the horizontal plane.
void SetDpadHeadGestureUp(float metres);
void SetDpadHeadGestureRadius(float metres);
// INI, `dpad.dwell_ms`: DEFAULT 0, meaning pure hold-to-shift. It exists for
// the head gesture, where a hand sweeping PAST the head is a real transient;
// raising it delays engagement by that many milliseconds. `dpad.deadzone`: how far the stick must travel to count as a
// direction -- deliberately larger than the movement deadzone, because a
// direction is a discrete answer and should not be given by accident.
void SetDpadDwellMs(int ms);
void SetDpadDeadzone(float deadzone);
