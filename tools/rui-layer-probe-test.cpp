// Offline proof that the head-response census separates a screen-fixed widget
// from a world-anchored one, refuses samples taken while the body is turning,
// and disqualifies itself when it cannot measure -- BEFORE a run is spent.
//
// The controls here are the ones the previous two instruments lacked. A census
// whose screen-fixed widgets do not read flat is measuring something other than
// what it claims, and that has to be visible in the log rather than inferred.

#include "../plugin/src/rui_layer_probe.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---- what the probe reads from the rest of the plugin ---------------------
extern "C" volatile float g_headBaseAngles[3] = {0.0f, 0.0f, 0.0f};
namespace { float g_headYaw = 0.0f; bool g_havePose = true; std::vector<std::string> lines; }
void Tf2VrLog(const char* text) { lines.emplace_back(text); }
bool GetHeadViewDelta(float* yawDelta, float* absolutePitch) {
    if (!g_havePose) return false;
    if (yawDelta) *yawDelta = g_headYaw;
    if (absolutePitch) *absolutePitch = 0.0f;
    return true;
}

namespace {

int passed = 0, total = 0;
void Check(bool ok, const char* what) {
    ++total;
    if (ok) ++passed; else std::printf("FAIL %s\n", what);
}
const std::string* Find(const char* needle) {
    for (const auto& l : lines)
        if (l.find(needle) != std::string::npos) return &l;
    return nullptr;
}
// The per-instance figure, which is the one every verdict is taken from.
double SlopeFor(unsigned rva) {
    char key[32];
    std::snprintf(key, sizeof(key), "ui(11)+0x%-6X", rva);
    const std::string* l = Find(key);
    if (!l) return 1e9;
    const auto at = l->find("| YAW ");
    if (at == std::string::npos) return 1e9;
    return std::strtod(l->c_str() + at + 6, nullptr);
}
// The same widget once our own transforms have run. Equal to the yaw figure
// means we do not move it.
double PostSlopeFor(unsigned rva) {
    char key[32];
    std::snprintf(key, sizeof(key), "ui(11)+0x%-6X", rva);
    const std::string* l = Find(key);
    if (!l) return 1e9;
    const auto at = l->find("| OURS-AFTER ");
    if (at == std::string::npos) return 1e9;
    return std::strtod(l->c_str() + at + 13, nullptr);
}

constexpr int kScratch = 0x5400;
alignas(16) unsigned char g_block[kScratch];

const std::uint8_t* MakeScratch(unsigned regs, double screenXpx) {
    std::memset(g_block, 0, sizeof(g_block));
    *reinterpret_cast<std::uint32_t*>(g_block + 8) = regs;
    auto* space = reinterpret_cast<float*>(g_block + 0x2DD0);
    space[0] = 1920.0f; space[1] = 1920.0f; space[2] = 1080.0f; space[3] = 1080.0f;
    for (unsigned i = 3; i < regs; ++i) {
        auto* r = reinterpret_cast<float*>(g_block + 0x3A80 + 32 * i);
        r[4] = static_cast<float>(screenXpx / 1920.0);
        r[5] = 0.5f;
    }
    return g_block;
}

constexpr unsigned kAmmo = 0x1010;      // screen-fixed: the control
constexpr unsigned kMarker = 0x84070;   // world-anchored reference
constexpr double kRate = 12.0;          // px per degree of head yaw

}  // namespace

int main() {
    // ---- FLAT FIRST, because the counters only grow, as in a session -------
    {
        g_havePose = false;
        for (int i = 0; i < 200; ++i) RuiLayerProbe(kAmmo, MakeScratch(8, 500.0), 0);
        ReportRuiLayerProbe();
        const std::string* h = Find("HEAD RESPONSE");
        Check(h && h->find("NO-HEAD-POSE 200") != std::string::npos,
              "a flat run counts every draw as no-head-pose");
        Check(Find("VOID: nothing sampled") != nullptr, "and voids the whole census");
        g_havePose = true;
        lines.clear();
        Sleep(5100);
    }

    // ---- A HEADSET-SHAPED RUN: body held still, head sweeping -------------
    for (int step = 0; step < 200; ++step) {
        const double head = -30.0 + 0.3 * step;          // 60 degrees of head
        g_headBaseAngles[1] = 75.03f;                    // body pinned, as in the real run
        g_headYaw = static_cast<float>(head);
        RuiLayerProbe(kAmmo, MakeScratch(8, 500.0), 0);                    // never moves
        RuiLayerProbe(kMarker, MakeScratch(8, 960.0 + kRate * head), 0);   // tracks
        RuiLayerProbe(kLabelWidgetRva, MakeScratch(32, 960.0 + kRate * head), 0);
        Sleep(3);  // real draws are spread over time; the body reference rolls on a clock
    }
    ReportRuiLayerProbe();

    Check(std::fabs(SlopeFor(kAmmo)) < 1.0, "a screen-fixed widget reads FLAT");
    Check(Find("PITCH") != nullptr, "the vertical axis is reported");
    Check(Find("OURS-AFTER") != nullptr, "our own contribution is reported");
    Check(std::fabs(SlopeFor(kMarker) - kRate) < 0.5, "a world-anchored widget recovers its rate");
    Check(std::fabs(SlopeFor(kLabelWidgetRva) - kRate) < 0.5, "so does the name label");
    Check(Find("FLAT -- screen-fixed (control)") != nullptr, "the control is labelled as such");
    Check(Find("MOVES WITH THE HEAD -- world-anchored") != nullptr, "and the moving ones are too");
    Check(Find("<<< THE NAME LABEL") != nullptr, "the label's row is called out");
    Check(Find("THE REFERENCE") != nullptr, "the world-anchored reference is summarised");
    {
        const std::string* c = Find("CONTROL: ");
        Check(c && c->find("1 widget(s) read FLAT") != std::string::npos,
              "exactly the screen-fixed widget reads flat");
        Check(c && c->find("instrument's own control and they passed") != std::string::npos,
              "and the control is reported as passed");
    }
    Check(Find("VOID SO FAR") == nullptr, "a 60 degree head sweep is not disqualified");

    // ---- OUR OWN TRANSFORMS, MEASURED NOT ASSUMED -------------------------
    // Nothing in this harness runs a transform between stage 0 and stage 1, so
    // a difference here would be the instrument inventing one.
    for (int step = 0; step < 200; ++step) {
        const double head = -30.0 + 0.3 * step;
        g_headBaseAngles[1] = 75.03f;
        g_headYaw = static_cast<float>(head);
        RuiLayerProbe(kMarker, MakeScratch(8, 960.0 + kRate * head), 1);
        Sleep(1);
    }
    lines.clear();
    Sleep(5100);   // the report is throttled to one every five seconds
    ReportRuiLayerProbe();
    Check(std::fabs(PostSlopeFor(kMarker) - kRate) < 0.5,
          "an untouched widget reports the same rate after our transforms");
    Check(Find("CHANGED BY US | x ") == nullptr, "and no row is flagged as changed by us");

    // ---- TWO INSTANCES AT ONCE: THE FAILURE OF 2026-09-07 -----------------
    // Two labels of the same widget, 600 px apart, both tracking at the same
    // rate. The aggregate of the pair is what read r 0.09 and x -8509..10410
    // in the real run; the per-instance tracks must each recover the rate.
    {
        lines.clear();
        Sleep(5100);
        const unsigned rva = 0x77777;
        for (int step = 0; step < 200; ++step) {
            const double head = -30.0 + 0.3 * step;
            g_headBaseAngles[1] = 75.03f;
            g_headYaw = static_cast<float>(head);
            RuiLayerProbe(rva, MakeScratch(8, 500.0 + kRate * head), 0);
            RuiLayerProbe(rva, MakeScratch(8, 1100.0 + kRate * head), 0);
            Sleep(3);
        }
        ReportRuiLayerProbe();
        Check(std::fabs(SlopeFor(rva) - kRate) < 1.0,
              "two instances at once: the per-instance rate is still recovered");
        Check(Find("MOVES WITH THE HEAD") != nullptr, "and it is called world-anchored");
        Check(Find("instance 0:") != nullptr && Find("instance 1:") != nullptr,
              "and BOTH instances are printed, not just the busiest");
    }

    // ---- ONE WIDGET, ONE TRACKING AND ONE HEAD-LOCKED ----------------------
    // The wearer's own report on 2026-09-07: the friendly label sits EXACTLY
    // still in the field of view while the census claimed +15.05 at r 0.97.
    // Both were true -- one widget, two populations, and only the busiest
    // instance was printed. The head-locked one must appear BY NAME.
    {
        lines.clear();
        Sleep(5100);
        const unsigned rva = 0x55555;
        for (int step = 0; step < 200; ++step) {
            const double head = -30.0 + 0.3 * step;
            g_headBaseAngles[1] = 75.03f;
            g_headYaw = static_cast<float>(head);
            // The busier instance tracks the world...
            RuiLayerProbe(rva, MakeScratch(8, 400.0 + kRate * head), 0);
            RuiLayerProbe(rva, MakeScratch(8, 400.0 + kRate * head), 0);
            // ...and this one is welded to the headset.
            RuiLayerProbe(rva, MakeScratch(8, 1500.0), 0);
            Sleep(3);
        }
        ReportRuiLayerProbe();
        Check(Find("HEAD-LOCKED -- this instance does not move") != nullptr,
              "a head-locked instance is named even when a busier one tracks");
        Check(Find("==== TWO POPULATIONS") != nullptr,
              "and the split inside one widget is called out loudly");
        Check(std::fabs(SlopeFor(rva) - kRate) < 1.0,
              "the busiest instance still reports its own rate");
    }

    // ---- OFF-SPACE SAMPLES ARE REJECTED ------------------------------------
    // x = -8509 in a 1920-wide space is not a position. Those samples polluted
    // the real run's aggregate and must not reach any fit.
    {
        lines.clear();
        Sleep(5100);
        const unsigned rva = 0x66666;
        for (int step = 0; step < 200; ++step) {
            g_headBaseAngles[1] = 75.03f;
            g_headYaw = static_cast<float>(-30.0 + 0.3 * step);
            RuiLayerProbe(rva, MakeScratch(8, -8509.0), 0);
            Sleep(3);
        }
        ReportRuiLayerProbe();
        const std::string* h = Find("HEAD RESPONSE");
        Check(h && h->find("off-space ") != std::string::npos, "off-space samples are counted");
        Check(SlopeFor(rva) == 1e9 || Find("too few samples on any one instance") != nullptr,
              "and a widget drawn only off-space is not fitted");
    }

    // ---- A LABEL OFF THE CLUSTER IS WHAT THE RUN IS LOOKING FOR ------------
    // Half the rate: the shape a gain error would take. It must NOT be lumped
    // in with the cluster, and its own slope must come back as half.
    {
        lines.clear();
        Sleep(5100);
        for (int step = 0; step < 200; ++step) {
            const double head = -30.0 + 0.3 * step;
            g_headBaseAngles[1] = 75.03f;
            g_headYaw = static_cast<float>(head);
            RuiLayerProbe(0x86220, MakeScratch(32, 960.0 + kRate * 0.5 * head), 0);
            Sleep(3);
        }
        ReportRuiLayerProbe();
        Check(std::fabs(SlopeFor(0x86220) - kRate * 0.5) < 0.5,
              "a widget responding at half the rate reports half the rate");
    }

    // ---- SAMPLES TAKEN WHILE THE BODY TURNS ARE REFUSED -------------------
    // This is the failure that produced -0.20 px/deg over 169 degrees of body
    // rotation on 2026-09-07. The body moving must EXCLUDE the sample, not be
    // averaged into it.
    {
        lines.clear();
        Sleep(5100);
        const unsigned rva = 0x99999;
        // 160 degrees, deliberately short of a full circle: a body yaw that
        // sweeps past 360 comes back around to where the reference was taken
        // and a handful of samples alias through as "still". Real play does not
        // spin 360 degrees inside the 250 ms reference window, but the test
        // should not pretend the wrap does not exist either.
        for (int step = 0; step < 200; ++step) {
            g_headBaseAngles[1] = static_cast<float>(step * 0.8);
            g_headYaw = static_cast<float>(step % 7);
            RuiLayerProbe(rva, MakeScratch(8, 500.0 + step), 0);
            Sleep(3);
        }
        ReportRuiLayerProbe();
        Check(SlopeFor(rva) == 1e9, "a widget only ever seen while the body turned is not reported");
        // The counter is cumulative over the session, as it is in a run, so the
        // assertion is "at least these 200" rather than a brittle exact figure.
        const std::string* h = Find("HEAD RESPONSE");
        long refused = -1;
        if (h) {
            const auto at = h->find("body-was-turning ");
            if (at != std::string::npos) refused = std::strtol(h->c_str() + at + 17, nullptr, 10);
        }
        Check(refused >= 200, "and every one of those draws is counted as refused");
    }

    std::printf("%d/%d passed\n", passed, total);
    return passed == total ? 0 : 1;
}
