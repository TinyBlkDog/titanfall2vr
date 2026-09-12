#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// XR BREADCRUMB — what were WE doing when the runtime faulted?
//
// 2026-09-08. The wearer crashed leaving a level and loading the training map.
// The crash recorder caught it: an access violation at an address in no loaded
// module, reached through
//
//     d3d11.dll  <-  VirtualDesktop.LibOVRRT64_1.dll  <-  virtualdesktop-openxr.dll
//
// with ZERO titanfall2vr frames anywhere on the stack.
//
// THAT IS NOT AN ALIBI, and reading it as one is the mistake this file exists
// to prevent. The most likely way for a VR runtime to fault inside its own
// submission path is that the application handed it a resource that has since
// been destroyed -- and a level transition is exactly when the game tears down
// and rebuilds its D3D resources underneath us. If we submit a texture the
// game freed, the runtime dereferences it and dies in its own code with none
// of ours on the stack. Same signature, and it would be entirely our fault.
//
// So this records, per frame and with no logging in the hot path:
//
//   * which XR call we were last INSIDE, so the crash record can say whether we
//     were mid-submit or idle;
//   * the exact texture pointers we handed the runtime this frame;
//   * a swapchain EPOCH bumped on every create and destroy, plus the time of
//     the last one -- so a submit carrying an epoch older than the current one
//     is a stale-resource submit, named as such rather than inferred.
//
// Cost: a few relaxed stores per frame. No allocation, no locks, no I/O. It is
// read only by the crash recorder, from inside the handler.
// ---------------------------------------------------------------------------

namespace XrBreadcrumb {

// Where we were. Ordered as the submit path runs, so a crash record reading
// "AcquireImage" means we never reached the copy, and so on.
enum class Stage : int {
    Idle = 0,
    WaitFrame,
    BeginFrame,
    AcquireImage,
    WaitImage,
    CopyIntoImage,
    ReleaseImage,
    EndFrame,
    SwapchainCreate,
    SwapchainDestroy,
};

void SetStage(Stage stage);

// The texture handed to the runtime for this eye, recorded at the moment it is
// submitted rather than when it was created.
void NoteSubmittedTexture(int eye, const void* texture);

// Called on every xrCreateSwapchain and xrDestroySwapchain. Bumps the epoch and
// stamps the time, which is what makes "stale" a measurement.
void NoteSwapchainChanged(bool created);

// Formats the whole breadcrumb into `out`. Safe to call from a crash handler:
// it touches only the atomics above and never allocates.
void Describe(char* out, unsigned int cap);

}  // namespace XrBreadcrumb
