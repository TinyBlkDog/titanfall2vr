#include "temporal_lever.h"

#include "engine_cvars.h"
#include "diagnostics.h"
#include "render_resolution.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

// THE FAMILY, and every entry is here because the ARCHIVED CVAR TABLE says what
// it is -- %TEMP%\titanfall2vr-cvars.txt, 3247 rows, dumped by engine_cvars on an
// earlier run of this same game build. That table is why this rung cost no run
// to design:
//
//   tsaa_curframeblendamount        0.05   THE LEVER. The current frame's
//                                          weight. 0.05 means the shipped
//                                          resolve is 95% history.
//   tsaa_blendfactoroverride       -1      registered, but its POLARITY is not
//                                          established: "blendfactor" could be
//                                          either side of the lerp, and -1 is
//                                          its disabled value. Setting it blind
//                                          is a coin flip that could force FULL
//                                          history, which would read as a
//                                          refutation of exactly the mechanism
//                                          under test. Left alone.
//   mat_object_motion_blur_enable   0      ALREADY OFF in the shipped config,
//                                          so object motion blur cannot be
//                                          contributing and needs no run.
//   mat_fxaa_enable                 0      already off, and spatial anyway.
//   mat_antialias_mode              --     A COMMAND, not a convar. The plan
//                                          listed it as a convar; the table
//                                          says cmd. It is deliberately NOT
//                                          censused as a cvar, because a
//                                          read-back returning "unregistered"
//                                          for it would look like a stale
//                                          offset rather than a category error.
//   tsaa_supersample, tsaa_depthupscale    strings in client.dll, ABSENT from
//                                          the registered table. Censused
//                                          anyway, precisely so the log says so
//                                          from the run itself rather than from
//                                          a file on my disk.
//
// None of these carries CHEAT or DEVONLY in the table, so all of them take a
// plain console set with no sv_cheats.
struct CensusEntry {
    const char* name;
    const char* note;
};
const CensusEntry kCensus[] = {
    {"tsaa_curframeblendamount", "THE LEVER: the current frame's weight in the temporal blend"},
    {"tsaa_blendfactoroverride", "registered, polarity unestablished, deliberately untouched"},
    {"tsaa_neighborhoodclamping", "clamps history to the neighbourhood; ships at 1"},
    {"tsaa_neighborhoodclampingsoftened", "ships at 1"},
    {"tsaa_blendfactormaxesoutatvelocity", "velocity at which the blend saturates"},
    {"tsaa_blendfactorincreaseatmaxvelocity", "velocity-driven multiplier"},
    {"tsaa_blendfactorincreasewhenunoccluded", "disocclusion multiplier"},
    {"tsaa_blendfactormodulationonsparklesandunocclusion", "ships at 1"},
    {"tsaa_numsamples", "ships at 64"},
    {"tsaa_debugresponsiveflag", "ships at 0"},
    {"mat_object_motion_blur_enable", "ships at 0 -- already off, so not a contributor"},
    {"mat_fxaa_enable", "ships at 0, and spatial rather than temporal"},
    {"tsaa_supersample", "named in the plan; expected UNREGISTERED"},
    {"tsaa_depthupscale", "named in the plan; expected UNREGISTERED"},
};
constexpr int kCensusCount = static_cast<int>(sizeof(kCensus) / sizeof(kCensus[0]));

const char* const kLeverName = "tsaa_curframeblendamount";
constexpr float kCurrentFrameOnly = 1.0f;

std::atomic_bool g_armed{false};
// THE DEFAULT IS READ FROM THE ENGINE AT THE FIRST ARM, never hardcoded from
// the archived table. That table is a dump from a different session; if this
// build's default has moved, restoring 0.05 from a constant would leave the
// game in a state neither arm of the A/B was taken in, and every later burst in
// the run would be measuring an unnamed third condition.
float g_savedDefault = 0.0f;
std::atomic_bool g_savedDefaultValid{false};

// Counted, not silent. An arm that could not run its command and an arm that
// ran it and had it ignored look identical from the pictures alone.
std::atomic_uint g_cbufFailures{0};

bool SetLever(float value) {
    char command[128]{};
    std::snprintf(command, sizeof(command), "%s %.6f", kLeverName, value);
    if (!RunEngineConsoleCommand(command)) {
        g_cbufFailures.fetch_add(1, std::memory_order_relaxed);
        char line[460]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] T-A: the engine console refused '%s' (engine.dll absent, or the Cbuf "
            "offsets did not verify). THE LEVER WAS NOT APPLIED -- any burst taken now is a "
            "VOID run, not a null result.\n", command);
        Tf2VrLog(line);
        return false;
    }
    return true;
}

}  // namespace

bool TemporalCurrentFrameOnlyArmed() { return g_armed.load(std::memory_order_acquire); }

void LogTemporalCvarCensus(const char* when) {
    char head[460]{};
    std::snprintf(head, sizeof(head),
        "[TF2VR] T-A TEMPORAL CENSUS (%s). Arm: current-frame-only is %s. Read back from the "
        "engine's own convar table by NAME, at the measurement. A name that reads UNREGISTERED "
        "cannot have been set, and any burst that depended on it is VOID.\n",
        when ? when : "?", TemporalCurrentFrameOnlyArmed() ? "ARMED" : "OFF (shipped resolve)");
    Tf2VrLog(head);

    int registered = 0;
    for (int i = 0; i < kCensusCount; ++i) {
        float value = 0.0f;
        char text[64]{};
        const bool ok = TryReadCvarFloat(kCensus[i].name, value);
        const bool gotText = TryReadCvarString(kCensus[i].name, text, sizeof(text));
        if (ok) ++registered;
        char line[460]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR]   %-52s %s value %.6f string '%s'  -- %s\n",
            kCensus[i].name, ok ? "REGISTERED  " : "UNREGISTERED",
            ok ? value : 0.0f, gotText ? text : "",
            kCensus[i].note);
        Tf2VrLog(line);
    }

    // THE POSITIVE CONTROL FOR THE CENSUS ITSELF. If nothing at all reads back,
    // the convar interface is what is broken and every "UNREGISTERED" above is
    // an instrument failure rather than a fact about the engine.
    float lever = 0.0f;
    const bool leverOk = TryReadCvarFloat(kLeverName, lever);
    char tail[620]{};
    std::snprintf(tail, sizeof(tail),
        "[TF2VR] T-A CENSUS VERDICT: %d of %d names registered. %s Lever '%s' = %.6f. Cbuf "
        "refusals this session: %u.\n",
        registered, kCensusCount,
        registered == 0
            ? "*** ZERO REGISTERED: the convar interface is DEAD, not the engine's table. The "
              "census is DISQUALIFIED and so is any conclusion drawn from this burst. ***"
            : "The interface is live, so an UNREGISTERED row above is a fact about the engine.",
        kLeverName, leverOk ? lever : -999.0f,
        g_cbufFailures.load(std::memory_order_relaxed));
    Tf2VrLog(tail);
}

void ToggleTemporalCurrentFrameOnly() {
    // THE DEFAULT IS CAPTURED BEFORE THE FIRST WRITE, and only once.
    if (!g_savedDefaultValid.load(std::memory_order_acquire)) {
        float shipped = 0.0f;
        if (!TryReadCvarFloat(kLeverName, shipped)) {
            char line[520]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] T-A: '%s' does not read back as a registered convar, so there is "
                "nothing to drive and nothing to restore. NOT ARMED. This is a VOID lever, not "
                "a refutation of temporal reuse.\n", kLeverName);
            Tf2VrLog(line);
            return;
        }
        g_savedDefault = shipped;
        g_savedDefaultValid.store(true, std::memory_order_release);
    }

    const bool want = !g_armed.load(std::memory_order_acquire);
    const float target = want ? kCurrentFrameOnly : g_savedDefault;
    if (!SetLever(target)) return;

    // READ IT BACK. A set the engine parsed and discarded is the failure mode
    // this whole rung has to survive, and it is invisible from the images.
    float after = 0.0f;
    const bool ok = TryReadCvarFloat(kLeverName, after);
    const bool took = ok && after > target - 0.0005f && after < target + 0.0005f;
    if (took) g_armed.store(want, std::memory_order_release);

    char line[760]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] T-A CURRENT-FRAME-ONLY %s (F3): asked the engine's console for '%s %.6f', read "
        "back %.6f -- %s. Shipped default was %.6f. %s\n",
        took ? (want ? "ARMED" : "DISARMED") : "**NOT APPLIED**",
        kLeverName, target, ok ? after : -999.0f,
        took ? "IT TOOK" : "IT DID NOT TAKE",
        g_savedDefault,
        !took
            ? "The next burst is VOID: the resolve is in neither arm's state. Do not read the "
              "pictures."
            : want
              ? "The temporal resolve now weights the CURRENT frame at 1.0, so batch 2 cannot be "
                "reading batch 1's history. A CLEAN, SHIFTED eye 1 CONFIRMS temporal reuse. "
                "Multiple copies SURVIVING here refutes it as the primary cause."
              : "THE POSITIVE CONTROL: the shipped resolve is back, so the next burst at the "
                "same offset must show the duplication again. If it does not, the lever was "
                "never what changed the picture.");
    Tf2VrLog(line);
    LogTemporalCvarCensus(took ? "after the toggle" : "after a toggle that did not take");
}
