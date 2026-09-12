// ---------------------------------------------------------------------------
// OFFLINE CHECK for plugin/src/render/world_rect_gate.h. No headset, no game,
// no build of the plugin -- it compiles the SAME header the plugin includes.
//
// Every rectangle below is READ OUT OF AN ARCHIVED LOG, not invented:
//   grep -a "submit-rect\|SUBMIT-RECT\|game sets most often" in the rotated
//   titanfall2vr.prev-N logs of 2026-09-09, and the 4032x2520 letterbox from
//   the comment beside g_mainSceneHalfTan in camera_update_hook.cpp.
//
// Build and run, from this directory (a VS developer shell, or after vcvars64):
//   cl /EHsc /std:c++17 /nologo /I..\plugin\src\render world-rect-gate-check.cpp
//   .\world-rect-gate-check.exe
// ---------------------------------------------------------------------------
#include "world_rect_gate.h"
#include <cstdio>

int main() {
    struct Case { const char* what; float vw, vh, tw, th; bool expected; };
    const Case cases[] = {
        // The defect: the video's 32x32, paired four runs in a row.
        {"32x32 video viewport in 5210x3648",              32.f,   32.f, 5210.f, 3648.f, false},
        // The world pass at the derived first-launch size.
        {"5210x3648 world in 5210x3648",                 5210.f, 3648.f, 5210.f, 3648.f, true},
        // The wearer's own good session size.
        {"5840x3648 world in 5840x3648",                 5840.f, 3648.f, 5840.f, 3648.f, true},
        // The measured letterbox: 4032x2520 inside 4032x3648 must PASS.
        {"4032x2520 letterbox in 4032x3648",             4032.f, 2520.f, 4032.f, 3648.f, true},
        // The menu at window size.
        {"2560x1440 menu in 2560x1440",                  2560.f, 1440.f, 2560.f, 1440.f, true},
        // The lens-clamped sub-rect the XR side derives later is NOT what is
        // gated here, but it must also pass if it ever were.
        {"4090x2864 clamped in 5210x3648",               4090.f, 2864.f, 5210.f, 3648.f, true},
        // Impostors: the most frequent rectangle at this size is a shadow map.
        {"2048x2048 shadow map in 5210x3648",            2048.f, 2048.f, 5210.f, 3648.f, false},
        {"512x512 bloom in 5210x3648",                    512.f,  512.f, 5210.f, 3648.f, false},
        // A stale 2560x1440 window rect inside the new target is refused too:
        // it is under half the width.
        {"2560x1440 stale window rect in 5210x3648",     2560.f, 1440.f, 5210.f, 3648.f, false},
        // Exactly half passes; one pixel under does not.
        {"exactly half in both axes",                    2605.f, 1824.f, 5210.f, 3648.f, true},
        {"one pixel under half in height",               2605.f, 1823.f, 5210.f, 3648.f, false},
        // Unknown target: the old behaviour, accept.
        {"unknown target accepts 32x32",                   32.f,   32.f,    0.f,    0.f, true},
        // A degenerate viewport is never the world.
        {"0x0 viewport in 5210x3648",                       0.f,    0.f, 5210.f, 3648.f, false},
    };
    int failures = 0;
    for (const Case& c : cases) {
        const bool got = tf2vr::WorldPassViewportPlausible(c.vw, c.vh, c.tw, c.th);
        std::printf("%s  %-44s -> %s\n", got == c.expected ? "ok  " : "FAIL", c.what,
                    got ? "accept" : "reject");
        if (got != c.expected) ++failures;
    }
    std::printf("%d of %zu cases failed\n", failures, sizeof(cases) / sizeof(cases[0]));
    return failures ? 1 : 0;
}
