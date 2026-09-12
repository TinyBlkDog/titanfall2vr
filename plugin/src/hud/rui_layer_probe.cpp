#include "rui_layer_probe.h"

#include "camera_hook.h"
#include "diagnostics.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

// The engine camera angles at the seam, BEFORE the head is composed in: body
// pitch and yaw, which is what the head delta is measured relative to.
extern "C" volatile float g_headBaseAngles[3];

// HOW MUCH EACH WIDGET MOVES WHEN ONLY THE HEAD TURNS, AND THE REFERENCE IS THE
// OTHER WIDGETS.
//
// Screen-fixed elements MUST read flat -- that is the instrument's own control.
// World-anchored elements must agree with each other, being points in one world
// seen through one camera. Then the finding is a comparison with no arithmetic
// in it, and no mapping from the RUI space to the eye frustum is assumed.
//
// WHAT THE 2026-09-07 RUNS ESTABLISHED, and why this build asks what it asks.
//
// Run three, per-instance and clean: the lock-on square ui(11)+0x1D560 reads
// +16.00 px/deg at r 0.98 over a 142 degree head sweep, and the name label
// ui(11)+0x8A330 reads +15.06 at r 0.97 -- within 5%, which is inside what two
// elements at different screen positions differ by anyway. So the label's
// HORIZONTAL yaw tracking is correct, and a yaw-gain error joins "projected
// without the head" among the refuted.
//
// Three things were never measured, and this build measures all three:
//
//   1. THE VERTICAL AXIS. Every figure so far is screen x against head yaw.
//      A label correct in yaw and wrong in pitch would read perfect here and
//      still slide off its entity.
//   2. WHAT OUR OWN TRANSFORMS DO TO IT. The probe sampled BEFORE
//      RuiNameplateScale and RuiWidgetUniformScale, so every number so far is
//      the game's, and OUR contribution has never appeared in any of them. The
//      dispatch says type 7 takes the size-only path -- but this front has been
//      wrong reading code twice, so it is sampled a second time AFTER the
//      transforms and the two are printed side by side. A difference is ours.
//   3. Both axes for the reference too, so the comparison stays like for like.
//
// Samples taken while the body is turning are refused, never corrected for.

namespace {

constexpr unsigned kFirstAuthoredReg = 3;
constexpr int kMaxWidgets = 64;
constexpr int kTracksPerWidget = 6;
constexpr int kStages = 2;                 // 0 = before our transforms, 1 = after
constexpr unsigned long long kReportMs = 5000;
constexpr float kBodyStillDeg = 0.6f;
constexpr unsigned long long kBodyRefMs = 250;
constexpr float kTrackJoinPx = 260.0f;
constexpr unsigned long long kTrackStaleMs = 250;

struct Fitter {
    double n = 0, sx = 0, sxx = 0, sy = 0, syy = 0, sxy = 0;
    void Add(double x, double y) {
        n += 1.0; sx += x; sxx += x * x; sy += y; syy += y * y; sxy += x * y;
    }
    void Reset() { *this = Fitter{}; }
    bool Fit(double* slope, double* r) const {
        *slope = 0.0; *r = 0.0;
        if (n < 12) return false;
        const double varX = sxx - sx * sx / n;
        const double varY = syy - sy * sy / n;
        const double cov = sxy - sx * sy / n;
        // No spread in the ANGLE means the experiment did not happen.
        if (varX <= 1e-6) return false;
        // No spread in the POSITION is the opposite: a perfectly screen-fixed
        // widget, which is a real result and the one the control depends on.
        // Treating it as unfittable deleted every control row and left the
        // report claiming nothing read flat.
        if (varY <= 1e-9) return true;
        *slope = cov / varX;
        *r = cov / std::sqrt(varX * varY);
        return true;
    }
};

struct Track {
    Fitter x;                    // screen x against head YAW
    Fitter y;                    // screen y against head PITCH
    float lastX = 0.0f;
    unsigned long long lastMs = 0;
    bool used = false;
};

struct Widget {
    unsigned rva = 0;
    Fitter allX, allY;
    Track tracks[kTracksPerWidget];
    unsigned long long offSpace = 0;
    float minX = 1e9f, maxX = -1e9f;
    float space = 0.0f;
};

Widget g_widgets[kStages][kMaxWidgets]{};
int g_widgetCount[kStages]{};
unsigned long long g_draws[kStages]{};
unsigned long long g_noScratch = 0, g_noRegs = 0, g_noHead = 0;
unsigned long long g_bodyMoved = 0, g_full = 0, g_faults = 0, g_offSpace = 0;
float g_headMin = 1e9f, g_headMax = -1e9f;
float g_pitchMin = 1e9f, g_pitchMax = -1e9f;
float g_lastBodyYaw = 0.0f, g_lastBodyPitch = 0.0f;
float g_prevRefYaw = 0.0f, g_prevRefPitch = 0.0f;
unsigned long long g_bodyRefMs = 0;
bool g_haveLastBody = false, g_havePrevRef = false;
unsigned long long g_lastReport = 0;

// THE BURST. Armed by F6 so it coincides with the screenshot, and capped so a
// press cannot flood the log.
constexpr int kBurstDraws = 48;
volatile long g_burstWanted = 0;
int g_burstId = 0;

const Track* BestTrack(const Widget& w) {
    const Track* best = nullptr;
    for (int i = 0; i < kTracksPerWidget; ++i)
        if (w.tracks[i].used && (!best || w.tracks[i].x.n > best->x.n)) best = &w.tracks[i];
    return best;
}

Widget* FindStageWidget(int stage, unsigned rva) {
    for (int i = 0; i < g_widgetCount[stage]; ++i)
        if (g_widgets[stage][i].rva == rva) return &g_widgets[stage][i];
    return nullptr;
}

}  // namespace

void RuiLayerProbe(unsigned rva, const std::uint8_t* scratch, int stage) {
    if (stage < 0 || stage >= kStages) return;
    ++g_draws[stage];
    if (!scratch) { ++g_noScratch; return; }
    float headYaw = 0.0f, headPitch = 0.0f;
    // A flat run publishes no head delta. Counted, never treated as zero: a
    // zero head delta would make every widget look screen-fixed.
    if (!GetHeadViewDelta(&headYaw, &headPitch)) { ++g_noHead; return; }
    const float bodyYaw = g_headBaseAngles[1];
    const float bodyPitch = g_headBaseAngles[0];
    // THE STICK IS EXCLUDED, NOT CORRECTED FOR. Two references on a 250 ms
    // clock, compared against the OLDER: many draws share a frame, so a
    // per-draw comparison would call a whole turn "still", and with a single
    // reference the draw that establishes it compares the body against itself
    // and is always accepted -- one contaminated sample per window, mid-turn.
    //
    // Only stage 0 advances the clock, so both stages of one draw are judged
    // against the same reference and cannot disagree about whether the body was
    // moving -- which would put different samples into the two stages and make
    // the whole pre/post comparison meaningless.
    const auto nowMs = GetTickCount64();
    if (stage == 0 && (!g_haveLastBody || nowMs - g_bodyRefMs >= kBodyRefMs)) {
        g_prevRefYaw = g_haveLastBody ? g_lastBodyYaw : bodyYaw;
        g_prevRefPitch = g_haveLastBody ? g_lastBodyPitch : bodyPitch;
        g_havePrevRef = g_haveLastBody;
        g_lastBodyYaw = bodyYaw;
        g_lastBodyPitch = bodyPitch;
        g_bodyRefMs = nowMs;
        g_haveLastBody = true;
    }
    if (!g_havePrevRef) { ++g_bodyMoved; return; }
    float yawStep = bodyYaw - g_prevRefYaw;
    while (yawStep > 180.0f) yawStep -= 360.0f;
    while (yawStep < -180.0f) yawStep += 360.0f;
    const float pitchStep = bodyPitch - g_prevRefPitch;
    if (std::fabs(yawStep) > kBodyStillDeg || std::fabs(pitchStep) > kBodyStillDeg) {
        ++g_bodyMoved; return;
    }
    __try {
        const unsigned regs = *reinterpret_cast<const std::uint32_t*>(scratch + 8);
        if (regs <= kFirstAuthoredReg) { ++g_noRegs; return; }
        const auto* space = reinterpret_cast<const float*>(scratch + 0x2DD0);
        const float sx = space[0], sy = space[2];
        if (!(sx > 1.0f) || !(sy > 1.0f)) { ++g_noRegs; return; }
        double sumX = 0.0, sumY = 0.0;
        unsigned used = 0;
        for (unsigned i = kFirstAuthoredReg; i < regs && i < 32; ++i) {
            const auto* r = reinterpret_cast<const float*>(scratch + 0x3A80 + 32 * i);
            sumX += static_cast<double>(r[4]) * sx;
            sumY += static_cast<double>(r[5]) * sy;
            ++used;
        }
        if (!used) { ++g_noRegs; return; }
        const double screenX = sumX / used;
        const double screenY = sumY / used;

        // EVERY LAYER, INDIVIDUALLY, AT ONE MOMENT. No fitting, no tracking, no
        // averaging -- the three things that have gone wrong. Stage 0 only, so
        // one press does not print each layer twice.
        if (stage == 0 && rva == kLabelWidgetRva && g_burstWanted > 0) {
            InterlockedDecrement(&g_burstWanted);
            char burst[300]{};
            std::snprintf(burst, sizeof(burst),
                "[TF2VR] LABEL BURST #%d [%ld left]: ui(11)+0x%X layer at screen %.1f, %.1f of "
                "%.0fx%.0f | head yaw delta %+.2f pitch %+.2f | body yaw %.2f pitch %.2f\n",
                g_burstId, g_burstWanted, rva, screenX, screenY,
                static_cast<double>(sx), static_cast<double>(sy),
                static_cast<double>(headYaw), static_cast<double>(headPitch),
                static_cast<double>(bodyYaw), static_cast<double>(bodyPitch));
            Tf2VrLog(burst);
        }

        Widget* w = FindStageWidget(stage, rva);
        if (!w) {
            if (g_widgetCount[stage] >= kMaxWidgets) { ++g_full; return; }
            w = &g_widgets[stage][g_widgetCount[stage]++];
            w->rva = rva;
        }
        w->space = sx;
        // OFF THE SCREEN IS NOT A POSITION. Half a space of margin either side,
        // because a label part-way off the edge is still being placed; ten
        // thousand pixels outside a 1920-wide space is not.
        if (screenX < -0.5 * sx || screenX > 1.5 * sx) {
            ++w->offSpace; ++g_offSpace; return;
        }
        if (headYaw < g_headMin) g_headMin = headYaw;
        if (headYaw > g_headMax) g_headMax = headYaw;
        if (headPitch < g_pitchMin) g_pitchMin = headPitch;
        if (headPitch > g_pitchMax) g_pitchMax = headPitch;
        if (screenX < w->minX) w->minX = static_cast<float>(screenX);
        if (screenX > w->maxX) w->maxX = static_cast<float>(screenX);
        w->allX.Add(headYaw, screenX);
        w->allY.Add(headPitch, screenY);

        // NEAREST RECENT TRACK, or a new one: one entity's trajectory instead
        // of the mean of everything this widget drew.
        Track* pick = nullptr;
        float bestGap = kTrackJoinPx;
        for (int i = 0; i < kTracksPerWidget; ++i) {
            Track& t = w->tracks[i];
            if (!t.used || nowMs - t.lastMs > kTrackStaleMs) continue;
            const float gap = std::fabs(t.lastX - static_cast<float>(screenX));
            if (gap <= bestGap) { bestGap = gap; pick = &t; }
        }
        if (!pick) {
            for (int i = 0; i < kTracksPerWidget && !pick; ++i)
                if (!w->tracks[i].used) pick = &w->tracks[i];
            if (!pick) {
                Track* stalest = &w->tracks[0];
                for (int i = 1; i < kTracksPerWidget; ++i)
                    if (w->tracks[i].lastMs < stalest->lastMs) stalest = &w->tracks[i];
                pick = stalest;
            }
            pick->x.Reset();
            pick->y.Reset();
            pick->used = true;
        }
        pick->lastX = static_cast<float>(screenX);
        pick->lastMs = nowMs;
        pick->x.Add(headYaw, screenX);
        pick->y.Add(headPitch, screenY);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_faults;
    }
}

void ReportRuiLayerProbe() {
    const auto now = GetTickCount64();
    if (g_lastReport && now - g_lastReport < kReportMs) return;
    g_lastReport = now;
    const float headSpan = (g_headMax > g_headMin) ? (g_headMax - g_headMin) : 0.0f;
    const float pitchSpan = (g_pitchMax > g_pitchMin) ? (g_pitchMax - g_pitchMin) : 0.0f;
    char line[640]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] HEAD RESPONSE: %d widget(s) | %llu draws pre / %llu post | declines no-scratch "
        "%llu no-regs %llu NO-HEAD-POSE %llu body-was-turning %llu off-space %llu table-full %llu "
        "faults %llu | head yaw span %.1f deg, head pitch span %.1f deg.\n",
        g_widgetCount[0], g_draws[0], g_draws[1], g_noScratch, g_noRegs, g_noHead, g_bodyMoved,
        g_offSpace, g_full, g_faults,
        static_cast<double>(headSpan), static_cast<double>(pitchSpan));
    Tf2VrLog(line);
    if (!g_widgetCount[0]) {
        Tf2VrLog("[TF2VR]   VOID: nothing sampled. On a flat run there is no head pose to separate "
                 "from the body, and this census cannot say anything at all.\n");
        return;
    }
    if (headSpan < 10.0f) {
        Tf2VrLog("[TF2VR]   VOID SO FAR: the head has swept under 10 degrees with the body still. "
                 "Every yaw slope below is fitted over a variable that barely moved.\n");
    }
    if (pitchSpan < 10.0f) {
        Tf2VrLog("[TF2VR]   PITCH NOT TESTED: the head has tilted under 10 degrees, so every "
                 "VERTICAL figure below is fitted over a variable that barely moved and says "
                 "nothing. Look UP and DOWN as well as left and right.\n");
    }
    int flat = 0, moving = 0;
    double movingSum = 0.0;
    for (int i = 0; i < g_widgetCount[0]; ++i) {
        const Track* t = BestTrack(g_widgets[0][i]);
        double s = 0, r = 0;
        if (!t || !t->x.Fit(&s, &r)) continue;
        if (std::fabs(s) > g_widgets[0][i].space * 0.1) continue;   // degenerate coordinates
        if (std::fabs(s) < 1.0) ++flat;
        else if (std::fabs(r) >= 0.5) { ++moving; movingSum += s; }
    }
    std::snprintf(line, sizeof(line),
        "[TF2VR]   CONTROL: %d widget(s) read FLAT (screen-fixed, as they must), %d read MOVING "
        "with the head. %s\n",
        flat, moving,
        flat ? "The flat ones are the instrument's own control and they passed."
             : "NOT ONE WIDGET READ FLAT -- the sampling is wrong and NOTHING below can be read.");
    Tf2VrLog(line);
    if (moving >= 1) {
        std::snprintf(line, sizeof(line),
            "[TF2VR]   THE REFERENCE: %d world-anchored widget(s), mean %+.2f px/deg in YAW. That "
            "is what a point fixed in the world does here; a widget off it is the defect and the "
            "gap is its size. No frustum mapping is assumed anywhere.\n",
            moving, movingSum / moving);
        Tf2VrLog(line);
    }
    for (int i = 0; i < g_widgetCount[0]; ++i) {
        const Widget& w = g_widgets[0][i];
        const Track* t = BestTrack(w);
        double xs = 0, xr = 0, ys = 0, yr = 0;
        const bool haveX = t && t->x.Fit(&xs, &xr);
        const bool haveY = t && t->y.Fit(&ys, &yr);
        // OUR OWN CONTRIBUTION, MEASURED. Same widget, same run, sampled again
        // after RuiNameplateScale and RuiWidgetUniformScale have had their turn.
        const Widget* post = FindStageWidget(1, w.rva);
        double ps = 0, pr = 0;
        const Track* pt = post ? BestTrack(*post) : nullptr;
        const bool havePost = pt && pt->x.Fit(&ps, &pr);
        const char* verdict =
            !haveX                            ? "too few samples on any one instance"
            : (std::fabs(xs) > w.space * 0.1) ? "DEGENERATE -- coordinates far outside the space"
            : (std::fabs(xs) < 1.0)           ? "FLAT -- screen-fixed (control)"
            : (std::fabs(xr) < 0.5)           ? "NOISE -- weak even within one instance"
                                              : "MOVES WITH THE HEAD -- world-anchored";
        std::snprintf(line, sizeof(line),
            "[TF2VR]   ui(11)+0x%-6X %5.0f smp | YAW %+8.2f px/deg (r %+.2f) | PITCH %+8.2f px/deg "
            "(r %+.2f) | OURS-AFTER %+8.2f (r %+.2f)%s | x %.0f..%.0f off %llu | %s%s\n",
            w.rva, haveX ? t->x.n : 0.0, xs, xr,
            haveY ? ys : 0.0, haveY ? yr : 0.0,
            havePost ? ps : 0.0, havePost ? pr : 0.0,
            (havePost && std::fabs(ps - xs) > 0.5) ? " CHANGED BY US" : "",
            static_cast<double>(w.minX), static_cast<double>(w.maxX), w.offSpace, verdict,
            w.rva == kLabelWidgetRva ? "   <<< THE NAME LABEL" : "");
        Tf2VrLog(line);
        // EVERY OTHER INSTANCE, NOT JUST THE BUSIEST ONE.
        //
        // 2026-09-07, and this is the whole reason the census said the wrong
        // thing: the wearer reported the friendly label sitting EXACTLY still in
        // the field of view -- head-locked, zero px/deg -- while this report
        // said +15.05 at r 0.97. Both were true. One widget draws several
        // instances, only the busiest was printed, and a widget that draws a
        // tracking instance AND a head-locked one looked like a tracking widget.
        // That is this project's oldest failure wearing new clothes: an
        // aggregate hiding the site that matters.
        //
        // A head-locked instance has a CONSTANT screen position, so its fit is
        // slope 0 -- which the verdict above would call "screen-fixed
        // (control)". For a widget known to be world-anchored that is not a
        // control, it is the DEFECT, and it is named as such below.
        int flatTracks = 0, movingTracks = 0;
        for (int k = 0; k < kTracksPerWidget; ++k) {
            const Track& tr = w.tracks[k];
            if (!tr.used) continue;
            double txs = 0, txr = 0, tys = 0, tyr = 0;
            if (!tr.x.Fit(&txs, &txr)) continue;
            const bool haveTy = tr.y.Fit(&tys, &tyr);
            if (std::fabs(txs) > w.space * 0.1) continue;
            const bool flatTrack = std::fabs(txs) < 1.0;
            if (flatTrack) ++flatTracks; else if (std::fabs(txr) >= 0.5) ++movingTracks;
            std::snprintf(line, sizeof(line),
                "[TF2VR]       instance %d: %5.0f smp | YAW %+8.2f (r %+.2f) | PITCH %+8.2f "
                "(r %+.2f) | last x %.0f | %s\n",
                k, tr.x.n, txs, txr, haveTy ? tys : 0.0, haveTy ? tyr : 0.0,
                static_cast<double>(tr.lastX),
                flatTrack ? "HEAD-LOCKED -- this instance does not move when the head turns"
                          : "moves with the head");
            Tf2VrLog(line);
        }
        if (flatTracks && movingTracks) {
            std::snprintf(line, sizeof(line),
                "[TF2VR]     ==== TWO POPULATIONS in ui(11)+0x%X: %d instance(s) HEAD-LOCKED and "
                "%d tracking the world, in the same widget and the same frames. The head-locked "
                "ones are the defect; nothing widget-wide can explain a split inside one widget. "
                "====\n",
                w.rva, flatTracks, movingTracks);
            Tf2VrLog(line);
        }
    }
    Tf2VrLog("[TF2VR]   OURS-AFTER is the same widget sampled again once our nameplate and widget "
             "scaling have run. Equal to the YAW figure means we do not move it and every number "
             "here is the game's; CHANGED BY US means the difference is ours to fix.\n");
}

void RuiLayerProbeBurst() {
    ++g_burstId;
    InterlockedExchange(&g_burstWanted, kBurstDraws);
    char line[320]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] LABEL BURST #%d ARMED: the next %d draws of ui(11)+0x%X print their own screen "
        "position and head pose. Press F6 again at a DIFFERENT head yaw; if the positions are the "
        "same in both bursts the label is welded to the view, and if they move it is not.\n",
        g_burstId, kBurstDraws, kLabelWidgetRva);
    Tf2VrLog(line);
}
