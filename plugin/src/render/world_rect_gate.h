#pragma once
// ---------------------------------------------------------------------------
// IS THIS VIEWPORT PLAUSIBLY THE WORLD PASS? Pure function, no engine
// dependencies, so `tools/world-rect-gate-check.cpp` compiles the SAME code the
// plugin runs and checks it against rectangles read out of archived logs.
//
// WHY. The main-scene rectangle is captured by PAIRING: "the game sets its
// viewport and then uploads the camera for it, so whatever RSSetViewports last
// saw IS this frustum's rectangle" (camera_update_hook.cpp, beside
// g_mainSceneHalfTan). In gameplay that holds. During the opening video it
// does not: four runs in a row (prev-2/3/4/5, 2026-09-09) paired a 32x32
// viewport with a main-scene upload and submitted it as the world for 46 s --
// "SUBMIT-RECT TINY: submitted imageRect 32x32 at 0,0; buffer 5210x3648" --
// and the compositor magnified those 32 pixels to fill the eye. That is the
// wearer's "zoomed into a very small piece of the screen".
//
// THE TEST. The world pass is either the whole target or the game's letterbox
// of it, and the letterbox has never been measured shorter than 0.69 of the
// height (4032x2520 inside 4032x3648). Half in either axis is far outside any
// legitimate world rectangle and far inside every impostor seen: 32x32 (video),
// 512x512 (bloom), 2048x2048 (shadow map, the most frequent rectangle at
// 5210x3648). Anything smaller than half the target in either axis is refused
// and the previously accepted rectangle stands.
//
// An unknown target (0x0, before the XR side has published one) accepts
// everything: the old behaviour, so the gate can never make a run WORSE than
// the runs it was measured on. The caller counts that case separately.
// ---------------------------------------------------------------------------
namespace tf2vr {

constexpr float kWorldRectMinFraction = 0.5f;

// True if the target size is unknown, else true iff the viewport spans at least
// kWorldRectMinFraction of the target in BOTH axes.
inline bool WorldPassViewportPlausible(float viewportW, float viewportH,
                                       float targetW, float targetH) {
    if (targetW < 1.0f || targetH < 1.0f) return true;
    if (viewportW < 1.0f || viewportH < 1.0f) return false;
    return viewportW >= targetW * kWorldRectMinFraction &&
           viewportH >= targetH * kWorldRectMinFraction;
}

}  // namespace tf2vr
