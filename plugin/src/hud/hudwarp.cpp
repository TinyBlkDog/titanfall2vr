#include "hudwarp.h"

#include "engine_cvars.h"
#include "diagnostics.h"
#include "present_hook.h"
#include "render_resolution.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// ---------------------------------------------------------------------------
// ONE STAGE = ONE CVAR, DRIVEN BY THE MECHANISM THAT CVAR ACTUALLY NEEDS.
//
// Two mechanisms, and picking the wrong one costs a run:
//
//   Console  -- the engine's own command buffer, exactly as typing it would.
//               Fires change callbacks, which is what a value whose owner
//               recomputes a layout on change requires. REFUSED BY THE CHEAT
//               GATE, so it is only valid for un-flagged cvars.
//   Slot     -- writes the ConVar float and int slots directly. Bypasses the
//               cheat gate (this is how r_drawviewmodel is driven) but fires NO
//               change callback, so a cached-on-change consumer will not notice.
//
// This distinction is not academic. The first positive control was
// "cl_drawhud 0" sent through the console, and cl_drawhud is CHEAT-flagged: the
// command was submitted, the gate refused it, the HUD stayed, and the control
// reported failure for a reason that had nothing to do with the HUD.
//
// THE READ-BACK IS WHY THAT IS NOW CHEAP. Every stage reads its cvar back after
// the engine has had a chance to consume the write, and logs requested against
// actual. "Submitted" is not "executed" and "executed" is not "took", and the
// log now separates all three instead of collapsing them into a silent null.
// ---------------------------------------------------------------------------
enum class How { Console, Slot };

struct Stage {
    const char* name;
    float value;    // what to drive it to
    float restore;  // its real default, taken from the cvar dump
    How how;
    bool control;   // a positive control rather than a hypothesis
    const char* what;
};

// Defaults below are the ones in the 08-17 cvar dump, not guesses. rui_padDist
// defaults to 0.7 and NOT to 0, which an earlier draft of this table would have
// "restored" to the wrong value, leaving the HUD altered for the rest of the run.
// ONLY THE STAGES STILL WORTH PRESSING.
//
// The five that have run are deleted rather than skipped. Asking the operator
// to double-tap past known results is a bad interface and an error-prone one --
// a mistimed second press ends the stage you wanted instead of the one you
// meant to skip, and the log then disagrees with what was actually on screen.
// One press, one test, nothing to count past.
//
// Already run and recorded in RUNG2-RESULT-2026-08-20.md: rui_drawEnable
// (blanked the HUD), cl_drawhud, hidehud, rui_safeAreaFrac at 0.15 and 0.30.
constexpr Stage kStages[] = {
    {"rui_padDist", 5.0f, 0.7f, How::Console, false,
     "rui_padDist 0.7 -> 5.0. The one worth watching: RUI is confirmed as what draws the "
     "HUD, and a padding distance is the closest thing in the cvar table to getting it off "
     "the bottom edge."},
    {"cl_safearea", 1.0f, 0.0f, How::Console, false, "cl_safearea 0 -> 1"},
    {"hudwarp_viewDist", 1.5f, 1.0f, How::Slot, false,
     "RUNG 1 leftover: hudwarp_viewDist 1.0 -> 1.5, with override armed alongside"},
    {"hudwarp_chopsize", 20.0f, 60.0f, How::Slot, false,
     "RUNG 1 leftover: hudwarp_chopsize 60 -> 20, with override armed alongside"},
};
constexpr int kStageCount = static_cast<int>(sizeof(kStages) / sizeof(kStages[0]));

constexpr std::uint64_t kHoldMs = 10000;
// Long enough that the command buffer has executed and any layout has been
// recomputed before the read-back and the armed-frame capture are taken.
constexpr std::uint64_t kSettleMs = 700;


// ---------------------------------------------------------------------------
// READ-ONLY RECON ON THE RUI DRAW FUNCTION, engine.dll+0xFC500.
//
// Rung 2 proved the HUD is RUI (rui_drawEnable 0 blanks it) and reversing
// Northstar's gate gave up the engine-side address it detours to do that.
// pescan .pdata-checks it as an exact function start, FC500..FC6DA, 474 bytes,
// one fragment, no chaining -- a legal hook target on paper.
//
// ON PAPER IS NOT ENOUGH HERE, WHICH IS THE ENTIRE POINT OF THIS.
//
// Northstar has ALREADY detoured that entry at runtime. Writing our own jump
// over its jump is how this project previously re-patched a live patch and got
// a trampoline that jumped into itself, killing the game 146 calls later. So
// before anything is installed, one run simply LOOKS at the bytes and says what
// is there.
//
//   48 89 5C 24 08 ...  the original prologue -- unpatched, safe to hook
//   E9 / FF 25 / 48 B8  a jump -- ALREADY DETOURED, do not touch this entry;
//                       attach further in, or hook the callee at 0xFC140
//
// Writes nothing, installs nothing, and runs once. A read-only diagnostic is
// never gated behind a UI mode, so it fires on its own as soon as the game is
// rendering a world -- no key, nothing to remember, nothing to mistime.
// ---------------------------------------------------------------------------
constexpr std::uint64_t kRuiDrawRva = 0xFC500;

bool g_ruiReported = false;

void ReportRuiDrawBytes() {
    if (g_ruiReported) return;
    HMODULE engine = GetModuleHandleA("engine.dll");
    if (!engine) return;   // not loaded yet; try again next frame
    g_ruiReported = true;

    const auto* p = reinterpret_cast<const std::uint8_t*>(engine) + kRuiDrawRva;
    // The module is mapped and this is inside it, but a bad RVA here would fault
    // in the render thread, and a crash is a far worse answer than "unreadable".
    if (IsBadReadPtr(p, 16)) {
        Tf2VrLog("[TF2VR] RUI recon: engine.dll+0xFC500 is not readable. Nothing was touched.\n");
        return;
    }
    char hex[64]{};
    for (int i = 0; i < 16; ++i) std::snprintf(hex + i * 3, 4, "%02X ", p[i]);

    const bool jmpRel32 = p[0] == 0xE9;
    const bool jmpMem = p[0] == 0xFF && p[1] == 0x25;
    const bool movAbs = p[0] == 0x48 && p[1] == 0xB8;
    const bool patched = jmpRel32 || jmpMem || movAbs;

    char line[720]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] RUI recon: engine.dll=%p, RUI draw at +0x%llX = %p\n"
        "[TF2VR]   first 16 bytes: %s\n"
        "[TF2VR]   %s\n",
        static_cast<void*>(engine), static_cast<unsigned long long>(kRuiDrawRva),
        static_cast<const void*>(p), hex,
        patched
            ? (jmpRel32 ? "ALREADY DETOURED (E9 rel32) -- Northstar owns this entry. Do NOT "
                          "install here; attach further in or hook the callee 0xFC140."
                        : (jmpMem ? "ALREADY DETOURED (FF 25 indirect jump) -- same conclusion."
                                  : "ALREADY DETOURED (48 B8 mov rax, imm64) -- same conclusion."))
            : "UNPATCHED: this is the original prologue (48 89 5C 24 08 = mov [rsp+8], rbx), so "
              "the entry is free and a hook here would not be fighting Northstar for it.");
    Tf2VrLog(line);
}

int g_stage = -1;
std::uint64_t g_stageStart = 0;
bool g_settled = false;
int g_next = 0;

bool Apply(const Stage& s, float value, const char* tag) {
    bool submitted = false;
    if (s.how == How::Console) {
        char command[128]{};
        std::snprintf(command, sizeof(command), "%s %g\n", s.name, value);
        submitted = RunEngineConsoleCommand(command);
    } else {
        // hudwarp_* only does anything with the family armed, and arming it is
        // harmless for every other slot cvar, so it is unconditional rather than
        // a special case somebody has to remember.
        if (std::strncmp(s.name, "hudwarp_", 8) == 0) TrySetCvarFloat("hudwarp_override", 1.0f);
        submitted = TrySetCvarFloat(s.name, value);
    }
    char line[360]{};
    std::snprintf(line, sizeof(line), "[TF2VR] %s: %s := %g via %s -- %s\n", tag, s.name, value,
                  s.how == How::Console ? "CONSOLE" : "SLOT WRITE",
                  submitted ? "submitted"
                            : (s.how == How::Console
                                   ? "REFUSED: the Cbuf offsets did not verify, NOTHING was sent"
                                   : "REFUSED: not resolvable as a ConVar, NOTHING was written"));
    Tf2VrLog(line);
    return submitted;
}

void ReadBackAndReport(const Stage& s, float wanted) {
    float actual = -12345.0f;
    const bool read = TryReadCvarFloat(s.name, actual);
    const bool took = read && std::fabs(actual - wanted) < 0.001f;
    char line[460]{};
    std::snprintf(line, sizeof(line), "[TF2VR] read-back: %s wanted %g, %s%g  -> %s\n", s.name,
                  wanted, read ? "reads " : "UNREADABLE ", actual,
                  took ? "TOOK. Whatever happens on screen now is this cvar doing it."
                       : "DID NOT TAKE. This stage measured NOTHING -- a CHEAT-flagged cvar sent "
                         "through the console is refused by the gate; treat any null here as void.");
    Tf2VrLog(line);
}

void BeginStage(int index) {
    g_stage = index;
    g_stageStart = GetTickCount64();
    g_settled = false;
    const Stage& s = kStages[index];

    char name[96]{};
    std::snprintf(name, sizeof(name), "tf2vr-rung2-s%d-before.bmp", index);
    RequestNamedBackbufferCapture(name);

    char line[640]{};
    std::snprintf(line, sizeof(line), "[TF2VR] ---- STAGE %d of %d%s: %s ----\n", index,
                  kStageCount - 1, s.control ? " [POSITIVE CONTROL]" : "", s.what);
    Tf2VrLog(line);
    Apply(s, s.value, "apply");
}

void EndStage(const char* why) {
    if (g_stage < 0) return;
    const Stage& s = kStages[g_stage];
    Apply(s, s.restore, "restore");
    char line[240]{};
    std::snprintf(line, sizeof(line), "[TF2VR] STAGE %d restored to %g (%s).\n", g_stage, s.restore,
                  why);
    Tf2VrLog(line);
    g_stage = -1;
}

}  // namespace

void StepHudWarp() {
    if (g_stage >= 0) {
        // A press during a stage ends it rather than skipping on, so the key
        // always moves toward restored and a run cannot be trapped with the HUD
        // switched off by its own positive control.
        EndStage("ended early by key");
        Tf2VrLog("[TF2VR] stage ended early. Press again for the next one.\n");
        return;
    }
    if (g_next >= kStageCount) {
        Tf2VrLog("[TF2VR] every stage has run. READ THE THREE POSITIVE CONTROLS FIRST: if none of "
                 "rui_drawEnable, cl_drawhud or hidehud blanked the HUD, and all three read back "
                 "as TOOK, then nothing in this run reached the HUD and every null below them is "
                 "void. Press again to cycle.\n");
        g_next = 0;
        return;
    }
    BeginStage(g_next++);
}

void AdvanceHudWarp() {
    ReportRuiDrawBytes();
    if (g_stage < 0) return;
    const std::uint64_t elapsed = GetTickCount64() - g_stageStart;
    if (!g_settled && elapsed >= kSettleMs) {
        g_settled = true;
        // Read back and capture at the same moment, so the log line and the
        // picture describe the same frame.
        ReadBackAndReport(kStages[g_stage], kStages[g_stage].value);
        char name[96]{};
        std::snprintf(name, sizeof(name), "tf2vr-rung2-s%d-armed.bmp", g_stage);
        RequestNamedBackbufferCapture(name);
    }
    if (elapsed < kHoldMs) return;
    EndStage("window elapsed");
}

void RestoreHudWarp() { EndStage("shutdown"); }
