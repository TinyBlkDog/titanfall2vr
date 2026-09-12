#pragma once

#include <cstdint>

// PASS 1 OF THE COST REVIEW: how much of the game's frame does this plugin
// actually take?
//
// The question is not rhetorical. Gameplay measures 11.32-11.45 ms of work
// against a 12.50 ms display period -- 1.05 ms of headroom -- and this project
// has twice found its own code inside a problem it was blaming on the game:
// the cvar probe added 5850 ms to level entry, and the log itself was once
// costing more than everything it measured. A third of the implementation is
// instrumentation. Nobody has ever measured what it costs.
//
// CORRECTION, and it is the reason this comment is long. The first version of
// this file justified __rdtsc by asserting the upload hook is entered "about
// 4360 times a frame". It is not. 4360 is a CUMULATIVE session total printed
// every 120 frames by a counter that is never reset; the archived deltas are
// 4504, 4374, 4369, 4817, 6167, 6189 per 120-frame window, so the real rate is
// ~37-52 entries a frame. Two QPC calls at that rate cost about 2.5 us a frame,
// a quarter of one percent of the headroom -- QPC would have been perfectly
// fine there, and the stated reason for choosing rdtsc was arithmetic done on a
// number read off an instrument without checking what it counted. That is the
// same mistake, twice in one session, as reading a vtable pointer that never
// moves and calling it proof.
//
// __rdtsc is KEPT, for a reason that survives the correction: the seam that may
// actually be hot is DrawIndexed, which brackets every draw call the game makes
// with two atomics and whose rate NOTHING in this repo has ever measured. If
// that is thousands a frame, QPC there would cost a fifth of the headroom and
// the instrument would be measuring itself. rdtsc costs about a tenth of that,
// and the residue is MEASURED and SUBTRACTED rather than hoped away -- see the
// calibration note in the .cpp.
//
// TSC is not converted to time at the call site. Raw ticks are accumulated and
// converted once per window against QPC, which also self-calibrates the tick
// rate, so no assumption about CPU frequency is shipped.
namespace PluginCost {

// One bucket per place the GAME calls into US. Keep this list short: the point
// is to attribute frame time to seams, not to profile every function.
enum Site : int {
    kPresent = 0,      // the whole Present hook body, XR submission included
    kDrawIndexed,      // THE UNMEASURED ONE. Two acq_rel atomics bracket every
                       // draw the game makes, with no foreign-caller fast path,
                       // and no count of it exists anywhere in this repo.
    kCameraUpload,     // HookUpdateSubresourceBody -- ~37-52 entries a frame,
                       // measured, which is far less hot than assumed
    kRunFrame,         // the per-frame plugin callback
    kSetupBones,       // the arms-collapse interceptor
    kRui,              // the RUI per-layer call-site patch
    kAimCmd,           // CreateMove post-process
    kViewport,         // RSSetViewports
    kRenderTargets,    // M1: the OMSetRenderTargets census. It is a slot swap on a
                       // seam the game hits a few hundred times a frame, and a
                       // read-only probe that costs frame time is still a
                       // behavioural change -- so it reports its own rate.
    kXrSubmit,         // xrWaitFrame..xrEndFrame. SEPARATE FROM kPresent AND
                       // EXCLUDED FROM IT, because it is overwhelmingly a WAIT:
                       // the runtime parks us here to pace the frame. Reported
                       // for completeness, but it is not work and it cannot be
                       // optimised away -- 2026-09-08, when kPresent read 10.6
                       // -11.2 ms and the report claimed "95.6% is OURS".
    kHotkeys,          // every ActionPressed call, i.e. the per-frame binding
                       // poll inside RunFrame. 104 calls a frame as of
                       // 2026-09-08, one GetAsyncKeyState each. Broken out
                       // because RunFrame regressed ~10x since 2026-08-24 and
                       // "it must be the key poll" is a guess until measured.
    // THE THREE BLOCKS RunFrame SPENDS ITS TIME IN. Added 2026-09-08 after the
    // run that showed frame total tracking RunFrame almost exactly: every
    // window with RunFrame above ~0.6 ms overran the 12.50 ms budget, and the
    // wearer reported glitches in the same session. Hotkeys came back at 0.017
    // ms, so the key poll is NOT it and the rest of RunFrame is unattributed.
    // These three partition it; whatever is left is RunFrame's own residual.
    kHudTick,          // the per-frame HUD/world block: eye hook, use targeting,
                       // world markers, anchor probe, frustum census, fov match.
                       // Every one of these arrived with the HUD front, which is
                       // also when RunFrame regressed. First suspect.
    kRuiTick,          // the RUI ladders and the widget classifier.
    kEnsures,          // the lazy hook installers and the sway reapply. Each is
                       // meant to be a latch check costing nothing once armed --
                       // a latch that stopped latching would show up here.
    kMenu,             // the config overlay's ImGui build + draw, inside Present.
                       // SEPARATE from kPresent on purpose: "the overlay costs
                       // nothing while closed" has to be a measurement with its
                       // own number, not the absence of a number.
    kOther,
    kSiteCount
};

// RAII. Two __rdtsc reads and one relaxed add; no branch on an arming flag,
// because a scope that is conditionally entered measures a different thing on
// each side of the condition and the whole point is to compare armed against
// disarmed.
struct Scope {
    explicit Scope(Site site);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    // EXCLUDE THE FORWARDED CALL, or this measures the wrong program.
    //
    // Every one of these seams is a hook that calls the game's or the driver's
    // original. DrawIndexed's body is two atomics wrapped around
    // g_drawOriginal(...) -- and g_drawOriginal is the D3D11 driver doing the
    // actual draw. A scope spanning the whole body would report the driver's
    // time as the plugin's, and on the one seam the review turns on it would
    // report a number some thousands of times too large.
    //
    // So the forward is bracketed and subtracted. Costs two more rdtsc reads on
    // forwarding seams; worth it to be measuring us rather than Respawn.
    void PauseForForward();
    void ResumeAfterForward();

    // END THE SCOPE EARLY, once, where the span it measures does not line up
    // with a C++ block. Publishes exactly as the destructor would and makes the
    // destructor a no-op, so the count is not doubled. Added for the XR
    // submission in the Present hook, whose span starts and ends mid-function.
    // Calling it twice is harmless; not calling it leaves the RAII behaviour
    // unchanged.
    void Stop();

private:
    void Publish();

    std::uint64_t start_;
    std::uint64_t excluded_;
    std::uint64_t pauseAt_;
    int site_;
    bool stopped_ = false;
};

// ---------------------------------------------------------------------------
// RUNFRAME SEGMENTS.
//
// One bucket said "the cost is inside RunFrame" and RunFrame is 1,100 lines.
// Three grouped sub-buckets (HudTick, RuiTick, Ensures) then came back at
// 0.001-0.007 ms against RunFrame's 0.44-1.52, so the cost is in the parts
// they did not cover and guessing a fourth group would be a third run spent on
// a hypothesis.
//
// So: checkpoints. Twelve marks at top-level statement boundaries, each
// closing the span since the previous mark into its own slot, reported on the
// same 240-frame window as everything else. Two __rdtsc reads per mark, and
// the marks are on the main thread only.
//
// This is deliberately NOT a general profiler. It answers one question about
// one function, and it comes out once that question is answered.
constexpr int kFrameSegs = 24;
void FrameSegBegin();
void FrameSeg(int slot);

// Called once per presented frame, from the Present hook. Counts frames and
// emits the window report when one is due.
void NotifyFrame();

// The measured cost of the measurement, established at load by timing a long
// run of empty Scopes. Printed with every window report, because a probe that
// does not state its own cost is asking to be believed rather than checked.
void CalibrateOverhead();

// xr.cost_report, DEFAULT 1. Cheap enough to leave on: two __rdtsc reads per
// hooked call and one formatted line every 240 frames.
void SetEnabled(bool enabled);
bool IsEnabled();

}  // namespace PluginCost
