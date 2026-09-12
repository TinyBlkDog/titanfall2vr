#include "aim_probe.h"

#include "engine_cvars.h"
#include "diagnostics.h"
#include "render_resolution.h"

#include <cmath>
#include <cstdio>

namespace {

bool g_armed = false;

// Each is a console command, run through the engine's own command buffer.
//
// `spewlog_enable` is NORTHSTAR'S cvar, not the engine's: Northstar hooks the
// engine spew function and writes it to R2Northstar/logs when this is on. It is
// what makes the rest of these readable without us hooking anything.
//
// Neither showfiredbullets is cheat-flagged, which the enumeration confirmed, so
// they take effect with sv_cheats at 0. The visualize/debug ones ARE cheat
// flagged and are deliberately NOT included -- they would silently refuse, and a
// setting that silently refuses is indistinguishable from one that reported
// nothing, which is the null result this whole file is designed to avoid.
// THE POSITIVE CONTROL, AND WHY THIS RUN NEEDED ONE.
//
// The first attempt armed cleanly, every cvar read back as 1, and Northstar's
// log contained NO bullet output whatsoever. That is unreadable as it stands: it
// means either the engine printed nothing, or it printed somewhere this capture
// does not reach, and those two have completely different consequences.
//
// `echo` is the discriminator. Northstar detours ConCommand_echo by name and
// writes it to its own log, so an echo that arrives proves the whole path --
// our Cbuf call, the engine's command parser, Northstar's capture, the file --
// is working end to end. An echo that does not arrive proves the absence of
// bullet output says nothing about bullets.
//
// This is the rule about verifying that a diagnostic can distinguish what it
// claims, applied to a diagnostic that had already failed to.
constexpr const char* kControlMarker = "echo TF2VR_PROBE_CONTROL_MARKER";

const char* const kEnableCommands[] = {
    kControlMarker,
    "spewlog_enable 1",
    // TRY THE MASTER SWITCH, WHICH SHOULD HAVE BEEN TRIED FIRST.
    //
    // Three separate diagnostics have now produced verified silence --
    // sv_showfiredbullets, cl_showfiredbullets, cl_ShowBoneSetupEnts -- and the
    // reason each time is that they draw debug OVERLAYS, and
    // enable_debug_overlays is 0 and CHEAT-flagged, so it silently refuses.
    //
    // But `sv_cheats` is itself NOT cheat-flagged, and this is single-player, where
    // the client hosts the server. So it may simply be settable, and if it is, the
    // entire overlay family unlocks at once -- including the trace visualisers and
    // viewangle_debug. Writing off five diagnostics without testing the one switch
    // that gates all of them was the wrong order to do this in.
    //
    // Both are read back after setting, so "it refused" and "it took" are
    // distinguishable rather than inferred from whatever appears next.
    "sv_cheats 1",
    "enable_debug_overlays 1",
    "sv_showfiredbullets 1",
    "cl_showfiredbullets 1",
    "bulletPredictionDebug 1",
    // ROUTE B RECONNAISSANCE, on the same proven capture path.
    //
    // cl_ShowBoneSetupEnts lives INSIDE the bone-setup path, so whatever it emits
    // is emitted from the code Route B has to find. If it names entities, that is
    // the viewmodel identified for free and without a disassembler.
    //
    // It may well produce nothing, exactly as the showfiredbullets family did --
    // that turned out to be a debug OVERLAY behind enable_debug_overlays, which is
    // CHEAT-flagged and unreachable at sv_cheats 0. Which is precisely why it
    // rides along with the echo control: a silent result will be readable as
    // silence rather than mistaken for a broken capture.
    "cl_ShowBoneSetupEnts 1",
    "cl_bones_oldhack 0",
};

const char* const kDisableCommands[] = {
    kControlMarker,
    "sv_showfiredbullets 0",
    "cl_showfiredbullets 0",
    "bulletPredictionDebug 0",
    "cl_ShowBoneSetupEnts 0",
    "enable_debug_overlays 0",
    // Last, so the overlays are turned off while cheats are still permitted to
    // turn them off.
    "sv_cheats 0",
    "spewlog_enable 0",
};

// The named handles step 2 cares about, and why each is worth a line.
struct NamedCvar {
    const char* name;
    const char* why;
};

const NamedCvar kNamedCvars[] = {
    {"sv_showfiredbullets", "the engine's own report of every bullet fired -- the instrument"},
    {"cl_showfiredbullets", "the client-side half of the same"},
    {"bulletPredictionDebug", "client/server bullet prediction, which is where a mismatch would show"},
    {"spewlog_enable", "Northstar's: routes engine spew to R2Northstar/logs. Without it the rest is invisible"},
    {"sv_cheats", "gates the visualize/debug cvars; 0 means those silently refuse"},
    {"mp_autocrosshair", "aim assist. Non-zero means something adjusts aim AFTER the player's angles"},
    {"cl_predict_viewangles", "the client predicts view angles, so a write may be overwritten by prediction"},
    {"viewangles_simpler", "DEVONLY, and its existence implies more than one viewangle path"},
    {"first_person_bullet_delay", "bullets are delayed in first person, so impact lags the angle that made it"},
    {"projectile_muzzleOffsetFirstPersonDecayDist",
     "THE INTERESTING ONE: projectiles already spawn at a muzzle offset in first person and decay to "
     "true aim. Cyberpunk's VR mod had to build this; this engine has it natively"},
    {"projectile_muzzleOffsetFirstPersonDecayMaxTime", "the time half of the same"},
    {"projectile_prediction", "whether projectiles are predicted at all"},
    {"cl_approx_tracer_origin", "tracers approximate their origin, so a tracer is not evidence of the ray"},
    // Route B: the bone pipeline. Reported so a session can see at a glance
    // whether the precondition is currently held.
    {"cl_ShowBoneSetupEnts", "emits from INSIDE the bone-setup path; if it names entities the "
                             "viewmodel is identified without a disassembler"},
    {"cl_threaded_bone_setup", "1 means bone setup runs on worker threads and a bone write would race it"},
    {"cl_SetupAllBones", "0 means the engine SKIPS bones it thinks are unneeded -- possibly the hand"},
    {"cl_bones_speculative", "speculative setup would compute bones we then overwrite"},
    {"enable_debug_overlays", "CHEAT-gated master switch; 0 is why the showfiredbullets family printed "
                              "nothing, and it gates any other overlay-style diagnostic too"},
};

// The bone-pipeline settings Route B needs held still, and the value each one
// must take. Every one was read out of the live cvar table, so none of these
// names is guessed.
struct BoneSetting {
    const char* name;
    float wanted;
    const char* why;
};

const BoneSetting kBoneSettings[] = {
    {"cl_threaded_bone_setup", 0.0f, "bone setup off worker threads, so a write cannot race it"},
    {"cl_async_bone_setup", 0.0f, "and not asynchronous, so it completes before the frame is built"},
    {"cl_parallel_clientside_animations", 0.0f, "client animation single-threaded for the same reason"},
    {"cl_bones_speculative", 0.0f, "no speculative setup, which would compute bones we then overwrite"},
    {"cl_SetupAllBones", 1.0f, "compute EVERY bone; at 0 the engine skips ones it thinks are unneeded, "
                              "and the hand may be one of them"},
    {"cl_bones_incremental_transform", 0.0f, "no incremental skipping of transforms"},
    {"cl_bones_incremental_blend", 0.0f, "nor of blends"},
};

bool g_boneSetupArmed = false;
// Read back on arm rather than assumed, so the restore returns the values this
// machine actually had rather than the ones this file thinks are default.
float g_boneSettingSaved[sizeof(kBoneSettings) / sizeof(kBoneSettings[0])]{};
bool g_boneSettingSavedValid[sizeof(kBoneSettings) / sizeof(kBoneSettings[0])]{};

}  // namespace

bool IsDeterministicBoneSetupArmed() { return g_boneSetupArmed; }

void ToggleDeterministicBoneSetup() {
    constexpr size_t kCount = sizeof(kBoneSettings) / sizeof(kBoneSettings[0]);
    if (g_boneSetupArmed) {
        unsigned restored = 0;
        for (size_t index = 0; index < kCount; ++index) {
            if (!g_boneSettingSavedValid[index]) continue;
            char command[128]{};
            std::snprintf(command, sizeof(command), "%s %g", kBoneSettings[index].name,
                          static_cast<double>(g_boneSettingSaved[index]));
            if (RunEngineConsoleCommand(command)) ++restored;
        }
        g_boneSetupArmed = false;
        char line[300]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] deterministic bone setup OFF; %u of %zu settings restored to the values read at "
            "arm.\n", restored, kCount);
        Tf2VrLog(line);
        return;
    }
    Tf2VrLog("[TF2VR] deterministic bone setup ARMING. This is the PRECONDITION for pinning the gun "
             "to the hand, not the feature: bone setup on this build is threaded, asynchronous and "
             "speculative, and a write racing it would fail intermittently rather than visibly.\n");
    unsigned applied = 0;
    for (size_t index = 0; index < kCount; ++index) {
        float current = 0.0f;
        g_boneSettingSavedValid[index] = TryReadCvarFloat(kBoneSettings[index].name, current);
        g_boneSettingSaved[index] = current;
        char command[128]{};
        std::snprintf(command, sizeof(command), "%s %g", kBoneSettings[index].name,
                      static_cast<double>(kBoneSettings[index].wanted));
        const bool ran = RunEngineConsoleCommand(command);
        // Read back after setting. A cvar that refuses -- because it is
        // write-protected, or because the name is right but the value is clamped --
        // must not be reported as applied, or a later intermittent failure would
        // be blamed on the bone write instead of on this.
        float after = 0.0f;
        const bool readBack = TryReadCvarFloat(kBoneSettings[index].name, after);
        const bool took = readBack && std::fabs(after - kBoneSettings[index].wanted) < 0.001f;
        if (took) ++applied;
        char line[520]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR]   %-36s %s was %.0f -> now %.0f %s (%s)\n",
            kBoneSettings[index].name,
            g_boneSettingSavedValid[index] ? "" : "[ABSENT on this build]",
            static_cast<double>(current), static_cast<double>(after),
            took ? "OK" : (ran ? "DID NOT TAKE" : "COMMAND FAILED"),
            kBoneSettings[index].why);
        Tf2VrLog(line);
    }
    g_boneSetupArmed = true;
    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] deterministic bone setup ARMED: %u of %zu took. Expect a frame-rate cost -- that is "
        "the point, it is the threading being turned off. What this run has to establish is only that "
        "the game still renders and plays correctly like this, because every later bone experiment "
        "depends on it.\n", applied, kCount);
    Tf2VrLog(line);
}

void LogAimRelatedCvars() {
    Tf2VrLog("[TF2VR] aim probe: the named handles step 2 depends on, read by name.\n");
    for (const auto& entry : kNamedCvars) {
        float value = 0.0f;
        char text[96]{};
        const bool haveValue = TryReadCvarFloat(entry.name, value);
        const bool haveText = TryReadCvarString(entry.name, text, sizeof(text));
        char line[520]{};
        if (haveValue) {
            std::snprintf(line, sizeof(line), "[TF2VR]   %-46s = %-10.4f (%s) -- %s\n",
                          entry.name, static_cast<double>(value),
                          haveText ? text : "<no string>", entry.why);
        } else {
            // A name that does not resolve is a real finding, not a gap: it means
            // this build does not have that handle and nothing downstream can use
            // it.
            std::snprintf(line, sizeof(line), "[TF2VR]   %-46s ABSENT on this build -- %s\n",
                          entry.name, entry.why);
        }
        Tf2VrLog(line);
    }
}

bool IsAimProbeArmed() { return g_armed; }

void ToggleAimProbe() {
    g_armed = !g_armed;
    const char* const* commands = g_armed ? kEnableCommands : kDisableCommands;
    const size_t count = g_armed ? sizeof(kEnableCommands) / sizeof(kEnableCommands[0])
                                 : sizeof(kDisableCommands) / sizeof(kDisableCommands[0]);
    unsigned ran = 0;
    for (size_t index = 0; index < count; ++index) {
        if (RunEngineConsoleCommand(commands[index])) ++ran;
    }
    if (ran != count) {
        // Say so loudly. A partly-applied probe would produce a log that looks
        // like a measurement and is not one.
        char line[300]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] aim probe: only %u of %zu console commands could be run; the engine command "
            "buffer was unavailable. Do NOT read the result of this run as a measurement.\n",
            ran, count);
        Tf2VrLog(line);
        return;
    }
    if (!g_armed) {
        Tf2VrLog("[TF2VR] aim probe DISARMED; the engine's bullet reporting is off again.\n");
        // Read back on the way out. A cvar that was 1 when armed and is 0 now
        // was reset by something -- a map load replicating a server value is the
        // obvious candidate for the REPLICATED ones -- and that alone would
        // explain an empty result without the capture path being at fault.
        LogAimRelatedCvars();
        return;
    }
    Tf2VrLog(
        "[TF2VR] aim probe ARMED. Any engine output lands in R2Northstar\\logs\\nslog<date>.txt, NOT "
        "in this file, because the capture is Northstar's existing spew hook rather than one of "
        "ours.\n"
        "[TF2VR]   Route B reconnaissance rides along: cl_ShowBoneSetupEnts emits from inside the "
        "bone-setup path, so if it names entities the viewmodel is identified without a "
        "disassembler. It may equally produce nothing, as the showfiredbullets family did once "
        "enable_debug_overlays turned out to be CHEAT-gated and off.\n"
        "[TF2VR]   THE LINE TO READ THIS TIME is sv_cheats below. Three diagnostics have now produced "
        "verified silence because they draw overlays and enable_debug_overlays is CHEAT-gated and "
        "off. sv_cheats is NOT cheat-flagged and this is single-player, so it may simply take -- and "
        "if it does, the whole overlay family unlocks at once. If it reads back 0, that family is "
        "closed for good and the remaining work is disassembly rather than cvars.\n"
        "[TF2VR] READ THE CONTROL FIRST. TF2VR_PROBE_CONTROL_MARKER is echoed on arm and on disarm. "
        "If those two markers are in the Northstar log, the path works end to end and an absence of "
        "bullet output is a real absence -- these cvars do not print, and the fire path has to be "
        "reached another way. If the markers are MISSING, the capture never worked and the absence "
        "of bullet output means nothing at all.\n");
    LogAimRelatedCvars();
}
