#pragma once
#include <cstdio>

// WHAT COUNTS AS TEXT, AND WHY THE OLD READER COULD NOT TELL.
//
// The old reader printed the first six printable runs of four or more bytes
// anywhere in a 0x200C arena. Four bytes of a float pair read as ASCII clear
// that bar constantly -- "[1U_?]", "[#F0?]", "[s?-e]" are all real output from
// the 2026-09-07 run -- so the reader printed noise, spent the widget's whole
// budget on it, and stopped six runs into an 8204-byte block. A name sitting
// further in was unreachable by construction.
//
// Three consecutive letters is NOT enough, and the log proves it: "[cDy!]",
// "[SEx<JE]", "[BaF*]" and "[iKF@]" are float bytes and every one of them holds
// a three-letter streak. Four is enough to reject all of them while keeping
// "Lt Shaver", "SPAWN AS TITAN", "Cooper's Logbook" and "%weaponcycle%".
//
// A short two-word name like "Lt Ash" has no four-letter streak, so the second
// clause takes runs made of space-separated all-letter words -- a shape those
// garbage runs do not have, because their separators are punctuation.
//
// THE FILTER CANNOT HIDE THE ANSWER. bestStreak reports the longest letter run
// the arena held whether or not anything qualified, so a widget that prints
// nothing is reported with the evidence for WHY, and a filter set one notch too
// tight is visible as a silent widget whose best run is 3 rather than 1.
inline int ExtractArenaText(const char* arena, char* out, int capacity, int* bestStreak) {
    int used = 0, found = 0, run = 0, arenaBest = 0;
    char word[80]{};
    // <= 0x200C so the final run is closed by the loop rather than dropped: the
    // old scanner lost any run that reached the last byte of the arena.
    for (int i = 0; i <= 0x200C; ++i) {
        const char c = (i < 0x200C) ? arena[i] : '\0';
        if (c >= 32 && c <= 126 && run < 79) { word[run++] = c; continue; }
        if (run >= 4) {
            word[run] = '\0';
            int letters = 0, streak = 0, best = 0, words = 0, wordLetters = 0;
            bool wordClean = true;
            for (int k = 0; k <= run; ++k) {
                const char w = (k < run) ? word[k] : ' ';
                const bool alpha = (w >= 'A' && w <= 'Z') || (w >= 'a' && w <= 'z');
                if (alpha) { ++letters; ++wordLetters; if (++streak > best) best = streak; }
                else {
                    streak = 0;
                    if (w == ' ') {
                        if (wordClean && wordLetters >= 2) ++words;
                        wordClean = true; wordLetters = 0;
                    } else {
                        wordClean = false; wordLetters = 0;
                    }
                }
            }
            if (best > arenaBest) arenaBest = best;
            const bool phrase = words >= 2 && letters >= 5 && letters * 5 >= run * 3;
            if ((best >= 4 || phrase) && used + run + 4 < capacity) {
                used += std::snprintf(out + used, capacity - used, " [%s]", word);
                ++found;
            }
        }
        run = 0;
    }
    if (bestStreak) *bestStreak = arenaBest;
    return found;
}
