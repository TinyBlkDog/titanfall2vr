#include "rui_hunt.h"
#include "diagnostics.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <vector>

// SAME-SESSION IDENTIFICATION BY DISAPPEARANCE. ONE LAUNCH, EVERY CANDIDATE.
//
// The 2026-09-07 build put a binary search behind on-screen prompts. The prompt
// could not draw (menu_overlay's `!IsXrArmed()` gate, HUD-RESULT-TEXT-CENSUS
// section 1), the wearer had no idea which step they were on, and the run was
// void. So this ladder is built to be usable BLIND: state is "press F3 for the
// next one", and nothing the wearer has to do depends on reading anything.
//
//   F3  NEXT      -- advance one step.
//   F5  THIS ONE  -- the friendly name vanished at THIS step. Always means that
//                    and nothing else, at every step, so there is no mode to
//                    keep track of.
//   F12 RESTORE   -- put everything back and start the ladder over.
//
// The order of the steps:
//
//   step 0  AMMO ONLY. The control the wearer already validated on 2026-09-07
//           ("disappeared and returned"). If the ammo counter does not vanish
//           here, the lever is dead and the rest of the run is worthless -- one
//           press, not forty, to find that out.
//   step 1  EVERYTHING observed on both paths. If the friendly name does NOT
//           vanish with the entire live census blanked, the widget that draws
//           it is not on the substituted path at all, and that is worth knowing
//           in the second press rather than the fortieth.
//   step 2+ ONE widget per press, ordered most-likely first but with NOTHING
//           left out: every identity either path has seen, both tables, in
//           descending draw count with the known nameplate family in front.
//           Ordering is not filtering -- the tail is still walked.
//
// After F5 the marked widget alone stays blanked and F3 toggles it: blanked,
// restored, blanked. That is disappearance / return / repeat with no state for
// the wearer to hold, and it is what turns a suspect into an identification.

namespace {

enum class Stage { Idle, Control, All, Single, Marked, Failed };

struct Row {
    unsigned path;
    std::uintptr_t target;
    unsigned long long calls = 0, emitted = 0, epoch = 0;
};

std::mutex guard;
std::vector<Row> rows;
std::vector<std::size_t> active;   // what is blanked right now
std::vector<std::size_t> ladder;   // the walk, built once at step 1 -> 2
std::vector<unsigned long long> starts;
Stage stage = Stage::Idle;
std::size_t cursor = 0;            // index into `ladder`
std::size_t marked = 0;            // the row F5 named
bool markedHidden = false;         // which half of the toggle we are on
unsigned long long epoch = 0;
unsigned long long entries[2]{}, nullTargets[2]{}, invalidPaths = 0;
unsigned long long lastBeat = 0;
std::uintptr_t base = 0;
int step = 0;

// NOT EMPTY AT LOAD, and that is load-bearing twice over.
//
// rui_probe's census widens substitution from the transform's type mask to
// EVERY live ui(11) target while RuiHuntVisible() is true. With an empty status
// that only became true on the first keypress, so the identities of types the
// mask leaves out (1, 5, 6, 10) were not on the census when the ladder built
// its walk -- a discovery filter hiding inside the start-up order. Standing
// visible from load means the census is complete before the wearer touches
// anything, and it means the prompt says what the keys do before it is needed
// rather than after.
char status[512] =
    "LADDER READY. With ammo and a friendly name on screen: F3 starts it (step 0 hides the AMMO "
    "COUNTER -- if the ammo does not vanish, stop). F5 = the friendly name just vanished. "
    "F12 = restore everything and start over.";

// The nameplate family, in front of the walk and NOT ahead of anything being
// walked. 0x84070 is already a valid negative and stays in, because the run it
// was cleared in is not this run and a ladder that quietly drops a candidate is
// the discovery filter this project keeps paying for.
constexpr unsigned kFirst[] = {0x8A330, 0x84070, 0x86220, 0x920D0};

void SetStatus(const char* value) {
    std::snprintf(status, sizeof(status), "%s", value);
    char log[640]{};
    std::snprintf(log, sizeof(log), "[TF2VR] LADDER: %s\n", value);
    Tf2VrLog(log);
}

// WHAT THE ARM THAT IS ENDING ACTUALLY DID. Printed on the way out of every
// step, zero or not: an arm that never fired excludes nothing, and the only way
// that is visible is if it says so at the moment it is retired.
void CloseArm() {
    if (active.empty()) return;
    unsigned long long total = 0;
    std::size_t reached = 0;
    for (auto i : active) {
        const auto n = rows[i].emitted - starts[i];
        total += n;
        if (n) ++reached;
    }
    char line[400]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] LADDER STEP %d CLOSED: %zu widget(s) armed, %zu of them fired, %llu draws "
        "suppressed. %s\n",
        step, active.size(), reached, total,
        reached == active.size()
            ? "Every armed widget drew, so the wearer's answer is about all of them."
            : (reached ? "SOME ARMED WIDGETS NEVER DREW -- this step excludes only the ones that fired."
                       : "NOTHING FIRED -- this step is INSTRUMENT FAILURE and excludes nothing."));
    Tf2VrLog(line);
    // The reach of the census itself, beside every verdict it produces. A null
    // target is a draw whose identity could not be read, and a step judged
    // against a census that was dropping identities is not a clean step.
    std::snprintf(line, sizeof(line),
        "[TF2VR] LADDER REACH: FC500 entries=%llu null=%llu | FC960 entries=%llu null=%llu | "
        "bad-path=%llu | identities on the census=%zu.\n",
        entries[0], nullTargets[0], entries[1], nullTargets[1], invalidPaths, rows.size());
    Tf2VrLog(line);
}

void Arm(std::vector<std::size_t> selection) {
    active = std::move(selection);
    starts.resize(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) starts[i] = rows[i].emitted;
    lastBeat = GetTickCount64();
}

// ui-relative, never the raw address: the RVA is what every doc, census and
// eliminated-mechanism row in this project is written in.
unsigned long long Rva(std::size_t index) {
    return static_cast<unsigned long long>(rows[index].target - base);
}

void ArmOne(std::size_t index) {
    Arm({index});
    char line[320]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] LADDER STEP %d: hiding ui(11)+0x%llX on %s (%llu draws so far), %zu of %zu in the "
        "walk. F5 if the friendly name is GONE, F3 for the next one.\n",
        step, Rva(index), rows[index].path == 0 ? "FC500/+68" : "FC960/+70",
        rows[index].calls, cursor + 1, ladder.size());
    Tf2VrLog(line);
}

void Fail(const char* why) {
    active.clear();
    stage = Stage::Failed;
    SetStatus(why);
}

// The lock is already held by every caller. Start and Step share this so a
// wearer who has lost the thread only ever needs F12 then F3, and so the two
// entry points cannot drift apart.
void StartLocked(std::uintptr_t uiBase) {
    base = uiBase;
    const auto control = std::find_if(rows.begin(), rows.end(), [=](const Row& r) {
        return uiBase && r.path == 0 && r.target == uiBase + 0x1010 && r.calls;
    });
    if (control == rows.end()) {
        Fail("The ammo widget has not drawn yet. Get into play with ammo on screen, then F12 and F3.");
        return;
    }
    ++epoch;
    step = 0;
    cursor = 0;
    ladder.clear();
    stage = Stage::Control;
    Arm({static_cast<std::size_t>(control - rows.begin())});
    SetStatus("STEP 0 CONTROL: the AMMO COUNTER should be gone. If it is still there, stop -- "
              "the lever is dead. If it is gone, F3.");
}

}  // namespace

// Called from INSIDE both dispatches, never as a delta around them.
bool RuiHuntObserve(unsigned path, std::uintptr_t target) {
    std::lock_guard<std::mutex> lock(guard);
    if (path > 1) { ++invalidPaths; return false; }
    ++entries[path];
    if (!target) { ++nullTargets[path]; return false; }
    auto found = std::find_if(rows.begin(), rows.end(), [=](const Row& r) {
        return r.path == path && r.target == target;
    });
    if (found == rows.end()) {
        rows.push_back({path, target});
        found = rows.end() - 1;
        if (stage == Stage::Single) {
            // The walk was built from what had been seen; something new turned
            // up after it. Say so rather than let the ladder claim coverage it
            // does not have. F12 rebuilds it without relaunching.
            Tf2VrLog("[TF2VR] LADDER: a NEW identity appeared after the walk was built, so the walk "
                     "is not exhaustive. F12 restarts it with the fuller census.\n");
        }
    }
    ++found->calls;
    found->epoch = epoch;
    const auto index = static_cast<std::size_t>(found - rows.begin());
    if (std::find(active.begin(), active.end(), index) == active.end()) return false;
    ++found->emitted;
    // THE HEARTBEAT, so the log carries the arm's liveness even when the wearer
    // says nothing for a minute. Time-based, not count-based: a step that
    // suppresses nothing has to be able to report that it is still the arm.
    const auto now = GetTickCount64();
    if (now - lastBeat >= 5000) {
        lastBeat = now;
        unsigned long long total = 0;
        for (auto i : active) total += rows[i].emitted - starts[i];
        char beat[240]{};
        std::snprintf(beat, sizeof(beat),
            "[TF2VR] LADDER HEARTBEAT: step %d still armed, %zu widget(s), %llu draws suppressed so far.\n",
            step, active.size(), total);
        Tf2VrLog(beat);
    }
    return true;
}

void RuiHuntStep(std::uintptr_t uiBase) {
    std::lock_guard<std::mutex> lock(guard);
    base = uiBase;
    if (stage == Stage::Idle || stage == Stage::Failed) { StartLocked(uiBase); return; }
    if (stage == Stage::Marked) {
        // Disappearance / return / repeat, on one key, with no state to hold.
        markedHidden = !markedHidden;
        if (!markedHidden) CloseArm();
        char line[300]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] LADDER CONFIRM: ui(11)+0x%llX is now %s. The name should follow it. F3 again to "
            "flip it back.\n", Rva(marked), markedHidden ? "HIDDEN" : "RESTORED");
        Tf2VrLog(line);
        if (markedHidden) Arm({marked}); else active.clear();
        return;
    }
    CloseArm();
    ++step;
    if (stage == Stage::Control) {
        // EVERYTHING the census has seen this session, both paths, both tables,
        // no watchlist. If the name survives this, it is not drawn here.
        std::vector<std::size_t> all;
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].calls) all.push_back(i);
        stage = Stage::All;
        Arm(all);
        char line[300]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] LADDER STEP 1: hiding ALL %zu observed widgets on both paths.\n", active.size());
        Tf2VrLog(line);
        SetStatus("STEP 1 ALL: nearly the whole HUD should be gone. F5 if the FRIENDLY NAME went "
                  "with it, F3 if the name is still there.");
        return;
    }
    if (stage == Stage::All) {
        // Build the walk now, from the identities that have actually drawn.
        ladder.clear();
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].calls) ladder.push_back(i);
        std::sort(ladder.begin(), ladder.end(), [&](std::size_t a, std::size_t b) {
            auto rank = [&](std::size_t i) {
                for (int k = 0; k < 4; ++k)
                    if (rows[i].path == 0 && rows[i].target == uiBase + kFirst[k]) return k;
                return 4;
            };
            const int ra = rank(a), rb = rank(b);
            if (ra != rb) return ra < rb;
            return rows[a].calls > rows[b].calls;
        });
        if (ladder.empty()) { Fail("Nothing has drawn, so there is no walk. F12 and F3 in play."); return; }
        stage = Stage::Single;
        cursor = 0;
        ArmOne(ladder[cursor]);
        SetStatus("STEP 2: one widget at a time now. F5 the moment the FRIENDLY NAME disappears, "
                  "F3 for the next one.");
        return;
    }
    // Stage::Single -- walk on.
    if (cursor + 1 >= ladder.size()) {
        active.clear();
        stage = Stage::Failed;
        char line[300]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] LADDER: the walk is finished -- all %zu observed widgets were hidden one at a "
            "time and none of them was named. Everything restored.\n", ladder.size());
        Tf2VrLog(line);
        SetStatus("Walk finished, nothing named, everything restored. F12 then F3 walks it again "
                  "with whatever has drawn since.");
        return;
    }
    ++cursor;
    ArmOne(ladder[cursor]);
}

void RuiHuntMark() {
    std::lock_guard<std::mutex> lock(guard);
    char line[400]{};
    if (stage == Stage::Control) {
        Tf2VrLog("[TF2VR] LADDER: F5 at the CONTROL step means the friendly name vanished when only "
                 "the AMMO widget was hidden. That is not expected; recorded, not interpreted.\n");
        return;
    }
    if (stage == Stage::All) {
        std::snprintf(line, sizeof(line),
            "[TF2VR] LADDER: the name IS on the substituted path -- it vanished with all %zu observed "
            "widgets hidden. F3 starts the one-at-a-time walk.\n", active.size());
        Tf2VrLog(line);
        return;
    }
    if (stage != Stage::Single) return;
    const std::size_t index = ladder[cursor];
    const auto fired = rows[index].emitted - starts[index];
    if (!fired) {
        // A widget that never drew cannot be the one that disappeared. Saying so
        // here is what stops a mis-timed press becoming an identification.
        std::snprintf(line, sizeof(line),
            "[TF2VR] LADDER MARK REFUSED: ui(11)+0x%llX has not drawn once since it was armed, so it "
            "cannot be what vanished. Wait for it to draw, or F3 on.\n",
            Rva(index));
        Tf2VrLog(line);
        return;
    }
    marked = index;
    markedHidden = true;
    stage = Stage::Marked;
    std::snprintf(line, sizeof(line),
        "[TF2VR] ======== LADDER NAMED BY THE WEARER: ui(11)+0x%llX on %s, %llu draws suppressed "
        "while armed, %llu draws this session. F3 now flips it restored / hidden for the "
        "return-and-repeat confirmation. ========\n",
        Rva(index),
        rows[index].path == 0 ? "FC500/+68" : "FC960/+70", fired, rows[index].calls);
    Tf2VrLog(line);
    SetStatus("NAMED. F3 restores it -- the name should come back. F3 again hides it -- the name "
              "should go. Then stop and report.");
}

void RuiHuntReset() {
    std::lock_guard<std::mutex> lock(guard);
    CloseArm();
    active.clear();
    ladder.clear();
    cursor = 0;
    step = 0;
    stage = Stage::Idle;
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] LADDER RESET: everything restored. %zu identities are on the census now; F3 starts "
        "the ladder over with all of them.\n", rows.size());
    Tf2VrLog(line);
    SetStatus("Everything restored. With ammo and a friendly name on screen, F3 starts the ladder.");
}

bool RuiHuntVisible() {
    std::lock_guard<std::mutex> lock(guard);
    return status[0] != 0;
}

// SEPARATE FROM RuiHuntVisible, and the two must not be merged again.
//
// RuiHuntVisible is what widens rui_probe's substitution to every live ui(11)
// target, and it is true from load so the census is complete before the ladder
// builds its walk. If the PROMPT used the same predicate, an ImGui panel would
// be parked in the wearer's view for the whole of any run that is not a ladder
// run -- including the read-only measurement runs where they are just playing.
// The prompt appears when the ladder actually starts, and not before.
bool RuiHuntPromptWanted() {
    std::lock_guard<std::mutex> lock(guard);
    return stage != Stage::Idle;
}

void RuiHuntStatus(char* text, std::size_t capacity) {
    std::lock_guard<std::mutex> lock(guard);
    std::snprintf(text, capacity, "%s", status);
}
