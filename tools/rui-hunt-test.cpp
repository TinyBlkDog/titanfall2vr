// Offline proof that the identification ladder can walk EVERY candidate and
// name one, before a run is spent on it.
//
// It exercises the state machine the wearer drives with three bare keys, and
// asserts the two properties the run depends on: step 0 blanks the ammo control
// and NOTHING else, and the walk covers every observed identity exactly once --
// the 2026-09-07 ladder listed 23 widgets when the census held 38, and that
// discovery filter is what made its result meaningless even before its asm
// comparison turned out to be broken too.

#include "../plugin/src/rui_hunt.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

std::vector<std::string> lines;
int passed = 0, total = 0;

void Check(bool ok, const char* what) {
    ++total;
    if (ok) ++passed; else std::printf("FAIL %s\n", what);
}

bool Logged(const char* needle) {
    for (const auto& line : lines)
        if (line.find(needle) != std::string::npos) return true;
    return false;
}

struct Widget { unsigned path; std::uintptr_t target; };

}  // namespace

void Tf2VrLog(const char* text) { lines.emplace_back(text); }

int main() {
    const std::uintptr_t base = 0x180000000ull;

    // ---- NO CONTROL, NO LADDER --------------------------------------------
    // FIRST, because the census only ever grows -- F12 restores the arms and
    // deliberately keeps the identities, so this is the one moment in a session
    // when the ammo widget has genuinely not been seen. The ladder must refuse:
    // without step 0 the wearer cannot tell a working lever from a dead one,
    // and every step after it would be uninterpretable.
    {
        RuiHuntObserve(1, base + 0x2010);        // census, but no ammo
        RuiHuntStep(base);
        Check(Logged("ammo widget has not drawn yet"), "the ladder refuses to start without its control");
        std::set<std::uintptr_t> hidden;
        if (RuiHuntObserve(1, base + 0x2010)) hidden.insert(base + 0x2010);
        Check(hidden.empty(), "and nothing is hidden while it is refusing");
        RuiHuntReset();
    }

    for (int count : {2, 6, 23, 38, 65}) {
        // The ammo control first, then a mix of both paths, as the live census
        // produces them.
        std::vector<Widget> widgets{{0, base + 0x1010}};
        for (int i = 1; i < count; ++i)
            widgets.push_back({unsigned(i % 2), base + 0x2000 + std::uintptr_t(i) * 0x10});

        // A draw of everything, which is what a frame of play looks like.
        auto frame = [&](std::set<std::uintptr_t>* hidden) {
            for (auto w : widgets)
                if (RuiHuntObserve(w.path, w.target) && hidden) hidden->insert(w.target);
        };

        for (int targetIndex = 1; targetIndex < count; ++targetIndex) {
            lines.clear();
            RuiHuntReset();
            frame(nullptr);  // the census sees everything before the ladder starts

            // ---- STEP 0: the control, and ONLY the control ----------------
            RuiHuntStep(base);
            std::set<std::uintptr_t> hidden;
            frame(&hidden);
            Check(hidden.size() == 1 && hidden.count(base + 0x1010) == 1,
                  "step 0 hides the ammo control and nothing else");

            // ---- STEP 1: everything, so a name that survives is elsewhere --
            RuiHuntStep(base);
            hidden.clear();
            frame(&hidden);
            Check(hidden.size() == widgets.size(), "step 1 hides every observed widget");

            // ---- STEP 2..N: one at a time, and EVERY one of them ----------
            std::set<std::uintptr_t> walked;
            std::uintptr_t marked = 0;
            for (std::size_t s = 0; s < widgets.size(); ++s) {
                RuiHuntStep(base);
                hidden.clear();
                frame(&hidden);
                if (hidden.size() != 1) break;  // walk finished
                const std::uintptr_t here = *hidden.begin();
                walked.insert(here);
                if (here == widgets[static_cast<std::size_t>(targetIndex)].target) {
                    RuiHuntMark();
                    marked = here;
                    break;
                }
            }
            Check(marked == widgets[static_cast<std::size_t>(targetIndex)].target,
                  "the walk reaches the target and F5 names it");
            Check(Logged("LADDER NAMED BY THE WEARER"), "naming is logged loudly");

            // ---- CONFIRMATION: restored, then hidden again ----------------
            RuiHuntStep(base);          // restore
            hidden.clear();
            frame(&hidden);
            Check(hidden.empty(), "F3 after naming restores the widget so the name can return");
            RuiHuntStep(base);          // hide again
            hidden.clear();
            frame(&hidden);
            Check(hidden.size() == 1 && hidden.count(marked) == 1,
                  "F3 again hides only the named widget, for the repeat");

            // ---- F12 puts everything back ---------------------------------
            RuiHuntReset();
            hidden.clear();
            frame(&hidden);
            Check(hidden.empty(), "F12 restores everything");
        }

        // ---- THE WHOLE WALK, with nothing marked: every identity, once ----
        lines.clear();
        RuiHuntReset();
        frame(nullptr);
        RuiHuntStep(base);                       // step 0
        RuiHuntStep(base);                       // step 1
        std::set<std::uintptr_t> walked;
        int steps = 0;
        for (std::size_t s = 0; s < widgets.size() + 4; ++s) {
            RuiHuntStep(base);
            std::set<std::uintptr_t> hidden;
            frame(&hidden);
            if (hidden.empty()) break;
            Check(hidden.size() == 1, "each walk step hides exactly one widget");
            const bool fresh = walked.insert(*hidden.begin()).second;
            Check(fresh, "no widget is walked twice");
            ++steps;
        }
        Check(walked.size() == widgets.size(), "the walk covers EVERY observed identity");
        Check(steps == static_cast<int>(widgets.size()), "and takes exactly one press per identity");
        Check(Logged("the walk is finished"), "the end of the walk says so");
        RuiHuntReset();
    }

    // ---- A MARK THE INSTRUMENT MUST REFUSE --------------------------------
    // F5 on a step whose widget has not drawn since it was armed cannot be an
    // identification: nothing disappeared, because nothing was there.
    {
        lines.clear();
        RuiHuntReset();
        std::vector<Widget> widgets{{0, base + 0x1010}, {0, base + 0x84070}, {0, base + 0x8A330}};
        for (auto w : widgets) RuiHuntObserve(w.path, w.target);
        RuiHuntStep(base);                       // step 0, control
        RuiHuntObserve(0, base + 0x1010);
        RuiHuntStep(base);                       // step 1, all
        RuiHuntObserve(0, base + 0x1010);
        RuiHuntStep(base);                       // step 2, first of the walk
        RuiHuntMark();                           // nothing has drawn since arming
        Check(Logged("LADDER MARK REFUSED"), "a mark on a widget that has not drawn is refused");
        Check(!Logged("LADDER NAMED BY THE WEARER"), "and does not become an identification");
        RuiHuntReset();
    }

    std::printf("%d/%d passed\n", passed, total);
    return passed == total ? 0 : 1;
}
