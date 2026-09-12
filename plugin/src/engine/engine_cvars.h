#pragma once

// TASK 05 STEP 2, ATTACK 1 -- THE ENGINE'S OWN CVARS, BY NAME, WITH NO OFFSETS.
//
// `g_pCVar` comes from vstdlib.dll's `VEngineCvar007` factory, so every console
// variable the engine has registered is reachable by NAME. No pattern scan, no
// offset that can go stale, and the interface is the one NorthstarLauncher --
// the reference clone in this repo, built against this same game -- already uses
// in production.
//
// This is the cheapest of the three routes in the step 2 plan, and it is worth
// running first for a second reason: it also serves the still-open ADS detection
// problem (HEADSET-ISSUES-2026-08-16.md section 5), where the two signals
// measured so far are both dead and the remaining lead is "the game's own zoom
// STATE". One enumeration answers both questions or rules the mechanism out for
// both at once.
//
// READ ONLY. It enumerates and it reads. Nothing here sets a cvar, and nothing
// here writes to the engine.

// Runs the enumeration once, the next time the game is known to be rendering a
// world. Cvars are registered as their owning module loads, so enumerating
// during startup would report a fraction of the table and look like a complete
// answer -- the same trap that made the auto-arm install its hook on the wrong
// entry.
void RequestCvarProbe();
void AdvanceCvarProbe();

// Reads a single cvar by name. This is the handle everything downstream would
// use -- an ADS detector, an aim-state reader -- and it is exposed now so that
// whatever step 2 concludes can be acted on without rediscovering the interface.
// Returns false if the interface is unavailable or the name is not a ConVar.
bool TryReadCvarFloat(const char* name, float& value);
bool TryReadCvarString(const char* name, char* buffer, unsigned size);
// Sets a cvar by name. Writes the value slots directly, so it updates neither
// the string form nor any change callback -- see the note at the definition.
bool TrySetCvarFloat(const char* name, float value);
