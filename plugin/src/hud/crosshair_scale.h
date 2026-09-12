#pragma once

// ---------------------------------------------------------------------------
// THE RETICLE'S SIZE, AT THE ONE PLACE THAT IS RETICLE-ONLY.
//
// The wearer's standing complaint after H1: the reticle is much too big. The
// cause is not a HUD bug -- it is a screen-space glyph presented across the
// headset's 110x90 degrees, which makes any fixed fraction of the frame
// enormous in angular terms (HEADSET-ISSUES-2026-08-16.md).
//
// EVERY PASS-LEVEL ROUTE IS CLOSED, AND MEASURED CLOSED:
//
//   - scaling the whole projection row of the reticle's pass shrank it and
//     moved it, because a row scale multiplies position as well as size;
//   - scaling only the linear part shrank it and STILL moved it -- "up and
//     left, each shrink goes further up and left" -- which says the reticle's
//     screen position lives in its VERTICES, in pixel coordinates. Size and
//     position are then the same numbers and no linear transform can separate
//     them.
//
// So the size has to be changed where the widget is built, not where it is
// projected. This is that place.
//
// THE TARGET, and why it is this function rather than the draw itself:
//
//   client.dll+0x15EF90   the crosshair draw. Identified positively: its third
//                         instruction reads the crosshair-state global at
//                         0x22AC694 that Crosshair_SetState writes, and
//                         early-outs when it is non-zero.
//   client.dll+0x158EB0   called from exactly TWO sites, both inside that draw,
//                         and from nowhere else in client.dll. It receives a
//                         per-element float as its fifth argument -- read from
//                         crosshairObj + index*4 + 0x2E50 -- and forwards it
//                         unchanged into 0x3D3710 for every sub-element.
//
// Hooking 0x158EB0 therefore cannot affect anything but the reticle, which the
// draw function itself could not promise.
//
// MEASURED CLOSED, 2026-08-19. The hypothesis was wrong and the test was cheap,
// which is what it was for. With scale 0.262 the falsifier counted 5718 scaled
// emits and the reticle was IDENTICAL -- so the float reaches the draw and is
// not the size.
//
// What it actually is: 0x158EB0 forwards it to 0x3D3710, which is a jump-table
// dispatch on a type code -- a generic RUI data-binding evaluator that computes
// a value into a destination slot. The float is one input among many that only
// some opcodes consume. Nothing about the reticle geometry runs through it.
//
// The hook is kept because it is correct, scoped and inert at 1.0, and because
// the next attempt on this draw will want the same entry point. Do not spend
// another run scaling this argument.
//
// ---- the original reasoning ----
//
// THIS IS A HYPOTHESIS WITH A CHEAP TEST, NOT A KNOWN FIX. A per-element float
// threaded untouched through a draw is the shape of a scale, but it could be an
// alpha, a rotation or a time. Every outcome is visible and none is harmful:
// a scale resizes the reticle, an alpha fades it, anything else changes
// something obvious. The falsifier below separates "wrong guess about the
// float" from "hook never ran", which are the two failures that look alike.
// ---------------------------------------------------------------------------

// Installs the hook on first use. Patches code, so it is gated and never arms
// itself on load: 1.0 is an exact pass-through and the wrapper does not even
// touch the argument.
void SetCrosshairScale(float scale);
float CrosshairScale();

// Multiplicative, for tuning by feel on the F12 cluster -- equal presses have
// to feel equal at any size.
void NudgeCrosshairScale(float factor);

// Per plugin frame: the falsifier line, on its own clock.
//
//   hits=0 with a scale off 1.0  -> the hook is not installed or not reached;
//                                   the reticle's size is not the question yet
//   hits climbing, nothing seen  -> the float is not a scale; try the widget
//                                   setter 0x54B160 instead
void AdvanceCrosshairScale();

// ---------------------------------------------------------------------------
// THE RETICLE, HIDDEN OUTRIGHT. A different mechanism from the scale above and
// a much cheaper one: no code patch, no trampoline, one int store.
//
//   client.dll+0x379BA0   Crosshair_SetState -- a five-instruction wrapper
//                         whose whole body is a store of the state enum to the
//                         global below (P0-HUD-2026-08-19.md).
//   client.dll+0x22AC694  the crosshair state global. 0 = show all, 2 = HIDE
//                         ALL. The crosshair draw at 0x15EF90 reads it in its
//                         third instruction and early-outs when it is non-zero,
//                         which is how the draw was positively identified.
//
// So hiding the reticle is a store of 2, and it needs no VM, no script and no
// handle resolution.
//
// WHY THIS RE-ASSERTS RATHER THAN STORING ONCE. The engine owns this global and
// Crosshair_SetState writes it on every state change, so a one-shot store is
// live only until the game next changes crosshair state. Re-asserting each
// plugin frame is what makes it stick.
//
// AND THE RE-ASSERT IS ITS OWN PROBE. "A write that reads back proves nothing":
// the falsifier line counts the frames on which the value was NOT already 2
// when we looked -- i.e. the frames the engine really did overwrite us. That
// number separates three states which otherwise look identical from inside a
// headset:
//
//   reasserts climbing        the engine is fighting the write and the
//                             re-assert is load-bearing
//   reasserts stuck at 1      one store was enough; the loop is insurance
//   reasserts at 0 with the
//   reticle still visible     the store is landing somewhere that is not the
//                             state this build's draw reads -- the address is
//                             wrong for this build, not the mechanism
//
// Disarming restores the value that was there before the first store, so
// turning it off does not leave the engine's own state permanently clobbered.
void SetReticleHidden(bool hidden);
bool IsReticleHidden();
// Per plugin frame: the re-assert and its falsifier line, on their own clock.
void AdvanceReticleHidden();

// ---------------------------------------------------------------------------
// ads.keep_reticle -- hold the reticle visible through ADS.
//
// Flat hides it because the monitor's camera is GLUED to the sight line, so the
// game takes your eye to the sights for you and a reticle would be redundant.
// VR removes exactly that guarantee: the gun is where your hand is and nothing
// forces your eye onto the rear sight. Hiding it there removes the only aiming
// reference without supplying the one it stood in for.
//
// We are not the ones hiding it -- reticle.hidden is a separate setting and it
// is off. This re-asserts the VISIBLE value over the engine's, on the same
// global and with the same falsifier as SetReticleHidden, and the visible value
// is LATCHED from the engine while the ADS fraction is zero rather than assumed.
//
// It stands down if reticle.hidden is also armed and says so, because two
// producers writing one global with opposite intents is a flicker nobody can
// attribute.
void SetAdsKeepReticle(bool keep);
bool IsAdsKeepReticle();
// Per plugin frame. One atomic load when it is not armed.
void AdvanceAdsKeepReticle();
