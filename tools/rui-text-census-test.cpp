// Offline proof that the RUI text filter can emit, BEFORE a run is spent on it.
//
// The negatives are not invented: every one is a literal run the 2026-09-07
// flat run (config content D5FA26FD) printed as if it were text, four times per
// widget, until both friendly-label candidates had spent their whole budget.
// The positives include a name at the very END of the arena, which the old
// six-run prefix scanner could not reach.

#include "../plugin/src/rui_text_filter.h"

#include <cstdio>
#include <cstring>

namespace {

constexpr int kArena = 0x200C;
int passed = 0, total = 0;

void Check(bool ok, const char* what) {
    ++total;
    if (ok) ++passed; else std::printf("FAIL %s\n", what);
}

// One printable run, isolated by NULs, placed at a chosen offset in the arena.
int RunAt(const char* text, int offset, char* out, int capacity, int* best) {
    static char arena[kArena];
    std::memset(arena, 0, sizeof(arena));
    const int n = static_cast<int>(std::strlen(text));
    std::memcpy(arena + offset, text, static_cast<unsigned>(n));
    return ExtractArenaText(arena, out, capacity, best);
}

void Accepts(const char* text, const char* what) {
    char out[360]{};
    int best = 0;
    const int found = RunAt(text, 64, out, sizeof(out), &best);
    Check(found == 1 && std::strstr(out, text) != nullptr, what);
}

void Rejects(const char* text, const char* what) {
    char out[360]{};
    int best = 0;
    const int found = RunAt(text, 64, out, sizeof(out), &best);
    Check(found == 0, what);
}

}  // namespace

int main() {
    // ---- POSITIVES: what a label, a hint and a prompt actually say ---------
    Accepts("Lt Shaver", "the defect's own text");
    Accepts("Lt Ash", "a short two-word name (no four-letter streak)");
    Accepts("SPAWN AS TITAN", "all-caps hint text");
    Accepts("Cooper's Logbook", "punctuation inside a word");
    Accepts("%weaponcycle%", "a token wrapped in percent signs");
    Accepts("Press %[A_BUTTON|SPACE]% to continue", "a button prompt");

    // ---- NEGATIVES: verbatim from the 2026-09-07 log -----------------------
    // Each of these was printed as if it were a widget's text. The first four
    // hold a three-letter streak, which is why three was not a usable bar.
    Rejects("cDy!", "float bytes with a three-letter streak");
    Rejects("SEx<JE", "float bytes, letters split by punctuation");
    Rejects("BaF*", "float bytes, three letters then a symbol");
    Rejects("iKF@", "float bytes, three letters then a symbol");
    Rejects("1U_?", "float bytes, one letter");
    Rejects("#F0?", "float bytes, one letter");
    Rejects("s?-e", "float bytes, letters never adjacent");
    Rejects("=@}.", "float bytes, no letters at all");
    Rejects(">`3s", "float bytes, one letter");
    Rejects("x|DN", "float bytes, two-letter streak");
    Rejects("35`1%%", "float bytes, no letters at all");
    Rejects("?^Kq", "float bytes, two-letter streak");
    Rejects("[<5 #", "float bytes with a space and no letters");
    Rejects("D`8hD", "float bytes, isolated letters");
    Rejects("@Y)D ", "float bytes with a space and two letters");

    // ---- THE PREFIX BUG THE OLD SCANNER HAD -------------------------------
    // Six junk runs then the name, and the name is what has to come out.
    {
        static char arena[kArena];
        std::memset(arena, 0, sizeof(arena));
        const char* junk[] = {"cDy!", "1U_?", "#F0?", "s?-e", ">`3s", "x|DN", "?^Kq", "=@}."};
        int at = 16;
        for (const char* j : junk) {
            std::memcpy(arena + at, j, std::strlen(j));
            at += static_cast<int>(std::strlen(j)) + 4;
        }
        std::memcpy(arena + 0x1F00, "Lt Shaver", 9);
        char out[360]{};
        int best = 0;
        const int found = ExtractArenaText(arena, out, sizeof(out), &best);
        Check(found == 1, "junk before the name does not crowd it out");
        Check(std::strstr(out, "Lt Shaver") != nullptr, "a name deep in the arena is reached");
        Check(best == 6, "best letter run is reported ('Shaver')");
    }

    // ---- A RUN THAT ENDS AT THE LAST BYTE ---------------------------------
    {
        char out[360]{};
        int best = 0;
        const int found = RunAt("Shaver", kArena - 6, out, sizeof(out), &best);
        Check(found == 1, "a run touching the end of the arena is not dropped");
    }

    // ---- SILENCE IS REPORTED, NOT GUESSED ---------------------------------
    // An arena of pure float noise must yield nothing AND a small best run, so
    // the census can say WHY a widget is silent instead of only that it is.
    {
        static char arena[kArena];
        std::memset(arena, 0, sizeof(arena));
        for (int i = 0; i < 200; ++i) std::memcpy(arena + i * 8, "cDy!", 4);
        char out[360]{};
        int best = -1;
        const int found = ExtractArenaText(arena, out, sizeof(out), &best);
        Check(found == 0, "an arena of float noise yields no text");
        Check(best == 3, "and reports the near-miss streak that explains it");
    }

    // ---- THE BUDGET RULE, STATED AS A TEST --------------------------------
    // Nothing may qualify from an arena the game has not written text into:
    // that is what stops a loading screen spending a widget's whole budget.
    {
        static char arena[kArena];
        std::memset(arena, 0, sizeof(arena));
        char out[360]{};
        int best = -1;
        Check(ExtractArenaText(arena, out, sizeof(out), &best) == 0, "an empty arena is silent");
        Check(best == 0, "and its best letter run is zero");
    }

    std::printf("%d/%d passed\n", passed, total);
    return passed == total ? 0 : 1;
}
