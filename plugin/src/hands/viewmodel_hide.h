#pragma once

// HIDING THE REAL VIEWMODEL, BY CALLING THE ENGINE DIRECTLY.
//
// This is the first piece of the final mechanism rather than more scaffolding.
// The script route is closed: three flat runs established that
// DisableWeaponViewModel is gated to PREDICTED client script (a WaitFrame
// thread is not that) and that Hide resolves to SetInvisibleForLocalPlayer,
// which refuses a networked entity. No receiver fixes either.
//
// Disassembling the native offline showed why it does not matter. The class
// check and the prediction gate live in the VM WRAPPER; the engine function
// underneath takes an entity pointer and does the work:
//
//   client.dll+0x1441A0  DisableWeaponViewModel   the wrapper
//       call 0x012F80    sq_getthisentity -- unwraps AND validates  <- the gate
//       call 0x0B6E40    the real implementation                    <- what we call
//
// So the whole thing is two direct calls with no VM involved:
//
//   void* player = GetLocalPlayer(0);            client.dll+0x14EF00
//   if (player) Weapon_DisableViewModel(player); client.dll+0x0B6E40
//
// Nothing here touches Northstar, and every address is a client.dll RVA, which
// §0 requires of anything that has to survive the eventual Northstar-free
// delivery.

// Resolves and verifies the three functions. Refuses, loudly, if any prologue
// does not match the shipped build these addresses came from.
void EnsureViewmodelHideResolved(void* clientModule);

// Hides or shows the local player's viewmodel. Does nothing if resolution
// failed or there is no local player yet (out of a map, mid-load).
void SetViewmodelHidden(bool hidden);
bool IsViewmodelHidden();

// S5 verification only: flips the state on a fixed cadence so the A/B and the
// restore come from one mechanism. Call once per frame.
void AdvanceViewmodelHidePulse();

// Unconditionally restores the viewmodel. Called on unload so the game is never
// left with its own weapon invisible.
void RestoreViewmodel();

// INI: `set viewmodel.hide_pulse = 1`.
void SetViewmodelHidePulseEnabled(bool enabled);
// Once per frame; re-asserts the persistent hide if something cleared it.
void AdvancePersistentViewmodelHide();

// H1: hold the viewmodel hidden for the whole session rather than pulsing it.
//
// Re-asserted periodically rather than set once, because r_drawviewmodel is an
// ordinary console variable and anything else in the game or a config exec is
// free to put it back. A gun that reappears halfway through the one headset run
// would read as the hide having failed.
void SetViewmodelHiddenPersistent(bool hidden);
