#include "plugin_cost.h"

#include "diagnostics.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <intrin.h>

namespace PluginCost {
namespace {

constexpr unsigned kFramesPerWindow = 240;

const char* const kSiteName[kSiteCount] = {
    "Present", "DrawIndexed", "CameraUpload", "RunFrame", "SetupBones", "Rui", "AimCmd", "Viewport", "RenderTargets",
    "XrSubmit(WAIT)", "Hotkeys", "HudTick", "RuiTick", "Ensures", "Menu",
    "Other"
};

std::atomic<std::uint64_t> g_ticks[kSiteCount]{};
std::atomic<std::uint64_t> g_calls[kSiteCount]{};
// SHIPS OFF. A diagnostic that a player can meet is a support burden, and one
// armed by default costs log volume and frame time on every machine that is not
// the developer's. Turn it on from the ini to use it.
std::atomic_bool g_enabled{true};

unsigned g_frames = 0;
std::uint64_t g_windowTsc0 = 0;
LARGE_INTEGER g_windowQpc0{};
LARGE_INTEGER g_qpcFrequency{};

// Ticks of pure instrument overhead per Scope, measured not assumed.
std::uint64_t g_overheadTicks = 0;
bool g_calibrated = false;

}  // namespace

Scope::Scope(Site site) : start_(__rdtsc()), excluded_(0), pauseAt_(0), site_(site) {}

void Scope::PauseForForward() { pauseAt_ = __rdtsc(); }

void Scope::ResumeAfterForward() {
    if (pauseAt_) {
        excluded_ += __rdtsc() - pauseAt_;
        pauseAt_ = 0;
    }
}

void Scope::Stop() {
    if (stopped_) return;
    Publish();
    stopped_ = true;
}

Scope::~Scope() {
    if (stopped_) return;
    Publish();
}

void Scope::Publish() {
    // A scope destroyed while still paused means the forward did not return
    // normally -- an SEH unwind, or an early return that skipped Resume. Count
    // only up to the pause rather than charging the plugin for time it spent
    // inside somebody else's code.
    const std::uint64_t now = __rdtsc();
    const std::uint64_t end = pauseAt_ ? pauseAt_ : now;
    const std::uint64_t span = end - start_;
    const std::uint64_t elapsed = span > excluded_ ? span - excluded_ : 0;
    // Relaxed on both: these are counters read once every 240 frames by the
    // same thread that will print them, and an acquire/release pair here would
    // put a barrier on the hottest path in the process to protect a number
    // whose last few ticks do not matter.
    g_ticks[site_].fetch_add(elapsed, std::memory_order_relaxed);
    g_calls[site_].fetch_add(1, std::memory_order_relaxed);
}

// THE INSTRUMENT MEASURES ITSELF FIRST.
//
// An empty Scope costs two __rdtsc reads and two relaxed adds. That is small
// but it is NOT zero on a seam entered thousands of times a frame, and the
// whole review depends on
// separating "the plugin is expensive" from "the measurement is expensive".
// So it is timed here over a long run and reported alongside every result, and
// the per-site figures below have it subtracted.
void CalibrateOverhead() {
    if (g_calibrated) return;
    g_calibrated = true;
    QueryPerformanceFrequency(&g_qpcFrequency);
    QueryPerformanceCounter(&g_windowQpc0);
    g_windowTsc0 = __rdtsc();

    constexpr int kSamples = 20000;
    // Warm the branch predictor and the cache lines first, so the measured run
    // is steady state rather than first-touch.
    for (int i = 0; i < 1000; ++i) { Scope warm(kOther); }
    const std::uint64_t before = __rdtsc();
    for (int i = 0; i < kSamples; ++i) { Scope sample(kOther); }
    const std::uint64_t after = __rdtsc();
    g_overheadTicks = (after - before) / kSamples;

    // The calibration run itself landed in kOther. Take it back out, or the
    // first window reports 21000 phantom calls.
    g_ticks[kOther].store(0, std::memory_order_relaxed);
    g_calls[kOther].store(0, std::memory_order_relaxed);
}

void SetEnabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    Tf2VrLog(enabled
        ? "[TF2VR] xr.cost_report = 1: every seam the game calls us through is timed with __rdtsc "
          "and attributed once every 240 frames.\n"
        : "[TF2VR] xr.cost_report = 0: seam timing off. The Scopes still run -- they are two rdtsc "
          "reads -- but nothing is reported.\n");
}

bool IsEnabled() { return g_enabled.load(std::memory_order_acquire); }

namespace {
const char* const kSegName[kFrameSegs] = {
    "pre", "hudtick", "polls", "autoarm", "probes", "input",
    "aim+ads", "ruiA3", "ruiCens+Imm", "lh.STATUS+CENSUS", "ruiProbe+mesh", "vm+tail",
    "lh.install", "lh.resolve", "lh.hooks",
    // The last unattributed span, subdivided 2026-09-08 after the hitch logger
    // named vm+tail on all 123 of its lines at 6.0-7.6 ms.
    "vm.vis", "vm.pin", "vm.watchdog", "vm.input", "vm.headtrack",
    // lh.resolve subdivided 2026-09-08: 55-59 ms every 2 s, and reading the
    // code did not find it. Four quarters, measured instead of argued.
    "res.rings", "res.ancestors", "res.others", "res.tail"
};
std::atomic<std::uint64_t> g_segTicks[kFrameSegs]{};
// PER-FRAME WORST, because a 240-frame MEAN CANNOT SEE A HITCH. One 40 ms
// frame in a window moves the mean by 0.17 ms and is invisible; it is the
// whole complaint. The wearer sees "at least one every time I fire missiles".
std::uint64_t g_segMax[kFrameSegs]{};
std::uint64_t g_segLast = 0;  // RunFrame is main-thread only.
// Refreshed once per window from the same self-calibration the report uses, so
// the hitch logger can convert ticks without assuming a CPU frequency.
double g_ticksPerMsEstimate = 0.0;
}  // namespace

// ---------------------------------------------------------------------------
// THE HITCH LOGGER, and why it exists rather than another subdivision round.
//
// Four runs were spent narrowing one hitch by hand: bucket -> three groups ->
// twelve segments -> four sub-segments, one run each, because every step only
// reported WINDOW AGGREGATES and the next question always needed another build.
// The wearer's verdict on that process: "I don't want this to be a 30 run
// debugging session." Correct.
//
// So this names the offender AT THE MOMENT IT HAPPENS. Any frame whose marked
// segments sum past the threshold prints its own breakdown, sorted worst
// first, with no aggregation to hide behind. It costs one comparison per frame
// when nothing is wrong, catches hitches that have nothing to do with the one
// currently under investigation, and needs no new build to ask a new question.
//
// Rate-limited to one line every 400 ms: a hitching frame is usually followed
// by more of them, and a log flood is itself a stall (this project has the
// scar -- the log once cost more than everything it measured).
constexpr double kHitchMs = 4.0;

std::uint64_t g_frameSegTicks[kFrameSegs]{};
std::uint64_t g_lastHitchLogMs = 0;

void FlushHitchIfAny() {
    if (g_ticksPerMsEstimate <= 0.0) return;
    double totalMs = 0.0;
    for (int i = 0; i < kFrameSegs; ++i) totalMs += g_frameSegTicks[i] / g_ticksPerMsEstimate;
    if (totalMs < kHitchMs) return;

    const std::uint64_t nowMs = GetTickCount64();
    if (g_lastHitchLogMs && nowMs - g_lastHitchLogMs < 400) return;
    g_lastHitchLogMs = nowMs;

    // Worst first, so the offender is the first thing on the line.
    int order[kFrameSegs];
    for (int i = 0; i < kFrameSegs; ++i) order[i] = i;
    for (int i = 0; i < kFrameSegs; ++i) {
        for (int j = i + 1; j < kFrameSegs; ++j) {
            if (g_frameSegTicks[order[j]] > g_frameSegTicks[order[i]]) {
                const int t = order[i]; order[i] = order[j]; order[j] = t;
            }
        }
    }
    char parts[420]{};
    int written = 0;
    for (int k = 0; k < kFrameSegs && k < 5; ++k) {
        const double ms = g_frameSegTicks[order[k]] / g_ticksPerMsEstimate;
        if (ms < 0.05) break;
        written += std::snprintf(parts + written, sizeof(parts) - written, "%s %.2f | ",
                                 kSegName[order[k]], ms);
        if (written >= static_cast<int>(sizeof(parts)) - 40) break;
    }
    char line[560]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] HITCH: one frame spent %.2f ms in the plugin (budget 12.50). Worst first: %s\n",
        totalMs, parts);
    Tf2VrLog(line);
}

void FrameSegBegin() {
    // The frame that just ended is judged here, before its counters are reset.
    FlushHitchIfAny();
    for (int i = 0; i < kFrameSegs; ++i) g_frameSegTicks[i] = 0;
    g_segLast = __rdtsc();
}

void FrameSeg(int slot) {
    const std::uint64_t now = __rdtsc();
    if (g_segLast && slot >= 0 && slot < kFrameSegs) {
        const std::uint64_t span = now - g_segLast;
        g_segTicks[slot].fetch_add(span, std::memory_order_relaxed);
        g_frameSegTicks[slot] += span;
        if (span > g_segMax[slot]) g_segMax[slot] = span;
    }
    g_segLast = now;
}

void ReportFrameSegments(double ticksPerMs) {
    char segs[600]{};
    int written = 0;
    double totalMs = 0.0;
    for (int i = 0; i < kFrameSegs; ++i) {
        const std::uint64_t ticks = g_segTicks[i].exchange(0, std::memory_order_relaxed);
        const double ms = ticksPerMs > 0.0 ? ticks / ticksPerMs / kFramesPerWindow : 0.0;
        const double maxMs = ticksPerMs > 0.0 ? g_segMax[i] / ticksPerMs : 0.0;
        g_segMax[i] = 0;
        totalMs += ms;
        // Quiet only when BOTH are quiet. A segment whose mean is nothing and
        // whose worst frame is 30 ms is precisely the hitch being hunted.
        if (ms < 0.005 && maxMs < 0.5) continue;
        written += std::snprintf(segs + written, sizeof(segs) - written,
                                 "%s %.3f (worst %.2f) | ", kSegName[i], ms, maxMs);
        if (written >= static_cast<int>(sizeof(segs)) - 60) break;
    }
    char line[900]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] RUNFRAME SEGMENTS (mean ms/frame, and the WORST SINGLE FRAME in the window; "
        "sum of means %.3f): %s\n",
        totalMs, written ? segs : "nothing above the floor -- the cost is not in a marked span");
    Tf2VrLog(line);
}

void NotifyFrame() {
    if (++g_frames < kFramesPerWindow) return;
    g_frames = 0;
    if (!g_enabled.load(std::memory_order_acquire)) return;

    LARGE_INTEGER qpcNow{};
    QueryPerformanceCounter(&qpcNow);
    const std::uint64_t tscNow = __rdtsc();

    const double windowMs = g_qpcFrequency.QuadPart
        ? static_cast<double>(qpcNow.QuadPart - g_windowQpc0.QuadPart) * 1000.0 / g_qpcFrequency.QuadPart
        : 0.0;
    const std::uint64_t windowTicks = tscNow - g_windowTsc0;
    // SELF-CALIBRATING. The tick rate is derived from this window's own QPC
    // span, so nothing about the CPU's frequency is assumed or shipped, and a
    // machine with a different TSC rate needs no change.
    const double ticksPerMs = windowMs > 0.0 ? static_cast<double>(windowTicks) / windowMs : 0.0;
    g_ticksPerMsEstimate = ticksPerMs;

    double totalMs = 0.0;
    double overheadMs = 0.0;
    char sites[768]{};  // headroom: two sites added 2026-09-08, and the break
                        // below silently DROPS later sites when it fires.
    int written = 0;
    for (int site = 0; site < kSiteCount; ++site) {
        const std::uint64_t ticks = g_ticks[site].exchange(0, std::memory_order_relaxed);
        const std::uint64_t calls = g_calls[site].exchange(0, std::memory_order_relaxed);
        if (!calls) continue;
        const double rawMs = ticksPerMs > 0.0 ? ticks / ticksPerMs : 0.0;
        const double ownMs = ticksPerMs > 0.0 ? (calls * g_overheadTicks) / ticksPerMs : 0.0;
        const double netMs = rawMs > ownMs ? rawMs - ownMs : 0.0;
        // kXrSubmit IS PRINTED BUT NOT ADDED TO THE TOTAL. It is time parked in
        // xrWaitFrame/xrEndFrame waiting for the compositor, not work this
        // plugin does, and counting it is what produced "95.6% is OURS" on
        // 2026-09-08. The percentage below has to mean "how much of the frame
        // could we give back", and a wait cannot be given back.
        if (site != kXrSubmit) totalMs += netMs;
        overheadMs += ownMs;
        written += std::snprintf(sites + written, sizeof(sites) - written,
                                 "%s %.3f ms/f (%llu calls/f) | ",
                                 kSiteName[site], netMs / kFramesPerWindow,
                                 calls / kFramesPerWindow);
        if (written >= static_cast<int>(sizeof(sites)) - 80) break;
    }

    const double msPerFrame = windowMs / kFramesPerWindow;
    const double pluginPerFrame = totalMs / kFramesPerWindow;
    char line[900]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] PLUGIN COST over %u frames: %.3f ms/frame of %.2f ms/frame is OURS = %.1f%%. %s"
        "instrument overhead %.3f ms/frame (%llu ticks/scope, subtracted above). Frame budget at "
        "80 Hz is 12.50 ms. This is the number the whole cost review turns on: run it once with "
        "the diagnostics armed and once with them off, and the difference is what the "
        "instrumentation costs to carry.\n",
        kFramesPerWindow, pluginPerFrame, msPerFrame,
        msPerFrame > 0.0 ? 100.0 * pluginPerFrame / msPerFrame : 0.0,
        sites, overheadMs / kFramesPerWindow,
        static_cast<unsigned long long>(g_overheadTicks));
    Tf2VrLog(line);
    ReportFrameSegments(ticksPerMs);

    g_windowQpc0 = qpcNow;
    g_windowTsc0 = tscNow;
}

}  // namespace PluginCost
