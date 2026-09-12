#include "viewmodel_hide.h"

#include "engine_cvars.h"
#include "diagnostics.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>

namespace {

// r_drawviewmodel, the stock Source flag for drawing the viewmodel.
//
// WHY THIS AND NOT THE ENGINE FUNCTION, WHICH IS WHERE THIS FILE STARTED.
//
// Three script runs and two plugin runs went into calling
// Weapon_DisableViewModel, and it cannot be done from here at all. The last of
// those runs measured it rather than guessing: the state field the function
// writes was read back immediately after the call and had not moved --
//
//   asked for HIDDEN; [player+0x1580] reads 0 immediately after the call
//   (expected 2). THE WRITE DID NOT LAND
//
// -- so it had taken the bail-out path, and the string that path loads
// (client.dll+0x8A4120) says "Cannot be called from non-predicted client
// script": the same message the script attempts produced. The prediction gate
// is inside the IMPLEMENTATION, not just the VM wrapper it is reached through,
// so no receiver and no calling convention gets past it from a plugin frame.
//
// The mistake that cost those runs was skipping a rung. The rule is to
// enumerate interfaces top-down -- script, data, convar, engine, memory -- and
// justify each descent; the convar rung was never checked before descending to
// engine calls, and it is where the answer was. A render flag read once a frame
// has no prediction context to be wrong about.
//
// WHAT IS DELIBERATELY NOT DONE HERE.
//
// The next rung down was writing [player+0x1580] = 2 by hand and calling the
// apply function the implementation tail-jumps to. That is memory surgery --
// what this whole plan pivoted away from -- and it is not needed. It stays
// documented rather than implemented:
//
//   Weapon_DisableViewModel  wrapper 0x1441A0  impl 0x0B6E40  writes 2
//   Weapon_EnableViewModel   wrapper 0x144260  impl 0x0B6F60  writes 0
//   GetLocalPlayer                             impl 0x14EF00
//   state field              [player+0x1580]   2 = disabled, 0 = enabled
//
// r_drawviewmodel IS FLAGGED FCVAR_CHEAT, and this sets the value slot directly
// rather than through ConVar::SetValue, so the cheat gate is not consulted.
// That is what makes it work in a normal single-player session. It is also the
// reason this must not be switched on casually in multiplayer, where writing a
// cheat-flagged cvar is a different question from a rendering preference.
constexpr const char* kDrawViewmodelCvar = "r_drawviewmodel";

bool g_hidden = false;
bool g_pulseEnabled = false;
unsigned g_pulseFrames = 0;

// About four seconds either way at the ~140fps this machine runs flat, matching
// the cadence the script steps used so the runs stay comparable.
constexpr unsigned kPulsePeriod = 560;

}  // namespace

void SetViewmodelHidePulseEnabled(bool enabled) { g_pulseEnabled = enabled; }
bool IsViewmodelHidden() { return g_hidden; }

void EnsureViewmodelHideResolved(void*) {
    // Nothing to resolve any more. The convar is looked up by name at each use,
    // which is what the cvar interface is for, and the engine functions this
    // file used to resolve are not called at all.
}

void SetViewmodelHidden(bool hidden) {
    if (hidden == g_hidden) return;

    if (!TrySetCvarFloat(kDrawViewmodelCvar, hidden ? 0.0f : 1.0f)) {
        Tf2VrLog("[TF2VR] viewmodel hide: r_drawviewmodel was not found as a ConVar; nothing "
                 "changed.\n");
        return;
    }
    g_hidden = hidden;

    // Read back rather than trusting the write. A cvar that exists but refuses
    // the value would otherwise look exactly like one that took it.
    float readback = -1.0f;
    const bool read = TryReadCvarFloat(kDrawViewmodelCvar, readback);
    char line[240]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR] viewmodel hide: set r_drawviewmodel = %d (%s); reads back %s%.3f.\n",
                  hidden ? 0 : 1,
                  hidden ? "HIDDEN, arms and gun should be gone" : "SHOWN, restored",
                  read ? "" : "UNREADABLE ", static_cast<double>(readback));
    Tf2VrLog(line);
}

void RestoreViewmodel() {
    // Deliberately not guarded on g_hidden: if the flag and the engine ever
    // disagree, the safe direction is to put the viewmodel back.
    TrySetCvarFloat(kDrawViewmodelCvar, 1.0f);
    g_hidden = false;
}

void AdvanceViewmodelHidePulse() {
    if (!g_pulseEnabled) return;
    if (++g_pulseFrames < kPulsePeriod) return;
    g_pulseFrames = 0;
    SetViewmodelHidden(!g_hidden);
}

namespace {
bool g_persistentHide = false;
unsigned g_reassertFrames = 0;
}  // namespace

void SetViewmodelHiddenPersistent(bool hidden) {
    // Deliberately does NOT try to apply it here. This is called from
    // LoadPluginConfig, which runs before vstdlib.dll is loaded, so the attempt
    // could only fail -- and it used to fail in a way that poisoned the cached
    // cvar interface for the whole session. The per-frame re-assert applies it
    // as soon as the interface is actually available.
    g_persistentHide = hidden;
    if (!hidden) RestoreViewmodel();
}

void AdvancePersistentViewmodelHide() {
    if (!g_persistentHide) return;
    // Check every frame until it has taken hold once, then settle into a slow
    // re-assert. Waiting 60 frames for the FIRST application would leave the
    // viewmodel visible for the first second of the run for no reason.
    if (g_hidden && ++g_reassertFrames < 60) return;
    g_reassertFrames = 0;
    // Read rather than trust the cached flag: the point of re-asserting is to
    // catch the cvar being changed by something that is not us, which the
    // cached flag by definition would not know about.
    // A failed READ is also a retry case, not a skip: at INI-load time the
    // convar does not exist yet, so the first attempt always fails and this is
    // what eventually makes it stick once the game has registered it.
    float current = 1.0f;
    const bool read = TryReadCvarFloat(kDrawViewmodelCvar, current);
    if (!read || current != 0.0f) {
        g_hidden = false;  // force the transition to be taken
        SetViewmodelHidden(true);
    }
}
