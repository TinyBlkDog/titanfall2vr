#pragma once

// Weapon viewmodel sway, suppressed at its source.
//
// The swing the weapon makes while you turn is the ENGINE'S OWN animation, not
// a residual of our viewmodel correction -- confirmed in the headset by turning
// with the mouse and head tracking off, which reproduces it exactly. No
// camera-matrix correction can cancel an animation, which is why the existing
// counter-rotation fights it instead of fixing it.
//
// It is driven by per-weapon KeyValue data, not by a cvar: the block
// sway_min/max_{x,y,z,pitch,yaw,roll}, sway_translate_gain, sway_rotate_gain,
// bob_cycle_time and their _zoomed variants appears in client.dll with no help
// strings, where every real cvar in that binary sits beside its description.
// Northstar can patch KeyValues files from a mod, but that needs every weapon's
// filename out of the VPKs; patching the parsed struct needs none of that.
//
// Field offsets were recovered statically -- see the comment on
// weaponSettingsInterceptor in camera_hook.asm for the derivation.

// Installs the passive capture hook. Records the settings object during the
// weapon script parse and mutates nothing.
void EnsureWeaponSettingsHookInstalled();
void RemoveWeaponSettingsHook();

// Zeroes (or restores) sway_translate_gain and sway_rotate_gain on every
// settings object captured so far. Original values are saved on the first
// suppression, so this is reversible within a session.
//
// Weapons are parsed at level load, so a weapon whose settings were parsed
// before this was first armed is only covered once it has been seen; re-arming
// after a map change picks up anything new.
void SetWeaponSwaySuppressed(bool suppressed);
// Reapplies the current suppression to anything captured since it was armed,
// so a map change does not silently restore the sway. Cheap and silent unless
// it actually covered new weapons. Call once per plugin frame.
void TickWeaponSwaySuppression();
// Set false BEFORE client.dll loads to leave the hook uninstalled entirely.
void SetWeaponSettingsHookAllowed(bool allowed);
// Detour invocations. Per level load is expected; per frame is not.
unsigned long long WeaponSettingsHits();
bool IsWeaponSwaySuppressed();


