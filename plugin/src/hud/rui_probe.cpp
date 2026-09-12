#include "rui_probe.h"
#include "rui_decline.h"
#include "rui_hunt.h"
#include "rui_layer_probe.h"
#include "rui_asset_dump.h"
#include "rui_immediate.h"
#include "world_marker.h"

#include "engine_cvars.h"
#include "diagnostics.h"
#include "hook_registry.h"
#include "present_hook.h"
#include "camera_update_hook.h"
#include "camera_hook.h"
#include "weapon_settings.h"
#include "aim_cmd.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

extern "C" {
// Read by the asm thunk on every call, so it is a plain byte rather than an
// atomic: one aligned byte store is atomic enough for a flag whose only job is
// to make the thunk fall straight through.
volatile unsigned char g_ruiProbeArmed = 0;
// Absolute address of the real engine.dll+0xFC500. The thunk tail-jumps here,
// which means Northstar's E9 on that entry still runs and rui_drawEnable still
// works -- we are composing with its hook, not replacing it.
volatile std::uintptr_t g_ruiOriginal = 0;
// The layer type currently under test, and the factor applied to its
// coordinate-space size. Read by the thunk on the render thread.
// A MASK of layer types to inset. Types 4..7 already take the engine own inset
// path, so only 0..3 are candidates; 0x0F is all of them. The HUD turned out to
// be split across layers -- type 3 is the top-left cluster and the lower-left
// health group is elsewhere -- so one type was never going to be enough.
volatile std::int32_t g_ruiTypeMask = 0;
// The inset, as a fraction of the layer per edge, and a position bias. The
// engine caps its own safe area at 0.05 (rui_safeAreaFrac x 0.05), which is why
// that cvar was invisible even where it applied; we drive the slot directly and
// are not bound by that cap.
volatile float g_ruiInset = 0.0f;
volatile std::uint64_t g_ruiWrites = 0;
}

// Not read by the thunk: what the INI asked for, applied to g_ruiTargetType
// only while the toggle is on, so turning the scale off cannot lose the setting.
static std::int32_t g_ruiTypeMaskWanted = 0x0F;

extern "C" {
void ruiCallInterceptor();
}

namespace {

constexpr std::uint64_t kRuiDrawRva = 0xFC500;
constexpr std::uint64_t kCallSiteRva = 0xFC87A;
constexpr std::size_t kCallBytes = 5;
// The exact instruction the recon run saw on disk and pescan disassembled:
// E8 rel32 with rel32 = 0xFFFFFC81, i.e. FC87F + (-0x37F) = FC500.
constexpr std::uint8_t kExpectedCall[kCallBytes] = {0xE8, 0x81, 0xFC, 0xFF, 0xFF};

std::uint8_t* g_site = nullptr;
std::uint8_t g_savedCall[kCallBytes]{};
std::atomic_bool g_installed = false;

// Defined with SetRuiInset, below, because that is where the value it pushes
// lives. Declared here because AdvanceRuiProbe -- which is what retries it --
// comes first in the file. Its two pieces of state have to live here for the
// same reason: the falsifier prints them.
bool PushRuiInsetToCvar();
bool g_insetTook = false;
float g_insetReadBack = 0.0f;

// A page within +/-2GB of the call site, because an E8 is a rel32 and this DLL
// can easily be mapped further away than that. The stub is a 14-byte absolute
// jump; the five-byte call reaches the stub, the stub reaches us.
std::uint8_t* AllocateNear(std::uint8_t* anchor) {
    // 64KB allocation granularity, walking outward from the anchor.
    for (std::int64_t delta = 0x10000; delta < 0x60000000; delta += 0x10000) {
        for (int direction = 0; direction < 2; ++direction) {
            auto candidate = reinterpret_cast<std::uintptr_t>(anchor) +
                             (direction ? static_cast<std::uintptr_t>(delta)
                                        : static_cast<std::uintptr_t>(-delta));
            auto* page = static_cast<std::uint8_t*>(
                VirtualAlloc(reinterpret_cast<void*>(candidate & ~static_cast<std::uintptr_t>(0xFFFF)),
                             64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            if (page) return page;
        }
    }
    return nullptr;
}

}  // namespace

void ToggleRuiProbe() {
    if (g_installed.load(std::memory_order_acquire)) {
        RemoveRuiProbe();
        return;
    }
    HMODULE engine = GetModuleHandleA("engine.dll");
    if (!engine) {
        Tf2VrLog("[TF2VR] RUI probe: engine.dll is not loaded; nothing patched.\n");
        return;
    }
    auto* base = reinterpret_cast<std::uint8_t*>(engine);
    auto* site = base + kCallSiteRva;

    // THE BYTES ARE CHECKED BEFORE ANYTHING IS WRITTEN. If this build's call
    // site is not the instruction the recon run measured, the RVA is wrong for
    // it and patching would corrupt whatever is really there.
    if (std::memcmp(site, kExpectedCall, kCallBytes) != 0) {
        char line[320]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] RUI probe: REFUSING to patch. engine.dll+0x%llX reads %02X %02X %02X %02X "
            "%02X, expected E8 81 FC FF FF. Nothing was written.\n",
            static_cast<unsigned long long>(kCallSiteRva), site[0], site[1], site[2], site[3],
            site[4]);
        Tf2VrLog(line);
        return;
    }
    // And the call really does land on FC500, decoded rather than assumed.
    std::int32_t rel = 0;
    std::memcpy(&rel, site + 1, sizeof(rel));
    const auto target = reinterpret_cast<std::uintptr_t>(site + kCallBytes + rel);
    if (target != reinterpret_cast<std::uintptr_t>(base + kRuiDrawRva)) {
        Tf2VrLog("[TF2VR] RUI probe: REFUSING to patch -- the call at +0xFC87A does not resolve "
                 "to +0xFC500. Nothing was written.\n");
        return;
    }

    auto* stub = AllocateNear(site);
    if (!stub) {
        Tf2VrLog("[TF2VR] RUI probe: no page free within 2GB of the call site, so a rel32 call "
                 "cannot reach the interceptor. Nothing patched.\n");
        return;
    }
    // Built from a zeroed buffer: bytes 2..5 are the rip displacement and MUST
    // be zero. sway_probe.cpp records a memset of 0x90 leaving 0x90909090 there
    // and taking the process down on the first call.
    std::uint8_t jump[14]{};
    jump[0] = 0xFF;
    jump[1] = 0x25;
    const auto interceptor = reinterpret_cast<std::uintptr_t>(&ruiCallInterceptor);
    std::memcpy(jump + 6, &interceptor, sizeof(interceptor));
    std::memcpy(stub, jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), stub, sizeof(jump));

    const std::int64_t displacement =
        reinterpret_cast<std::int64_t>(stub) - reinterpret_cast<std::int64_t>(site + kCallBytes);
    if (displacement > 0x7FFFFFFF || displacement < -0x7FFFFFFF) {
        Tf2VrLog("[TF2VR] RUI probe: the stub landed out of rel32 range. Nothing patched.\n");
        VirtualFree(stub, 0, MEM_RELEASE);
        return;
    }

    g_ruiOriginal = target;
    std::memcpy(g_savedCall, site, kCallBytes);

    DWORD oldProtect = 0;
    if (!VirtualProtect(site, kCallBytes, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Tf2VrLog("[TF2VR] RUI probe: VirtualProtect failed; nothing patched.\n");
        VirtualFree(stub, 0, MEM_RELEASE);
        return;
    }
    std::uint8_t patch[kCallBytes]{};
    patch[0] = 0xE8;
    const auto rel32 = static_cast<std::int32_t>(displacement);
    std::memcpy(patch + 1, &rel32, sizeof(rel32));
    std::memcpy(site, patch, kCallBytes);
    FlushInstructionCache(GetCurrentProcess(), site, kCallBytes);
    DWORD ignored = 0;
    VirtualProtect(site, kCallBytes, oldProtect, &ignored);

    g_site = site;
    g_ruiWrites = 0;
    RegisterHookSite("rui-probe near-stub", stub, 14);
    RegisterHookSite("rui-probe FC87A patched call", site, kCallBytes);
    g_installed.store(true, std::memory_order_release);
    g_ruiProbeArmed = 1;

    char line[520]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] RUI call site patched: engine.dll+0x%llX now reaches a stub at %p, and the real "
        "+0x%llX still runs through Northstar's own detour, so rui_drawEnable keeps working. "
        "INERT until a layer type is selected -- until then every call passes straight through "
        "untouched.\n",
        static_cast<unsigned long long>(kCallSiteRva), static_cast<void*>(stub),
        static_cast<unsigned long long>(kRuiDrawRva));
    Tf2VrLog(line);
}

void RemoveRuiProbe() {
    if (!g_installed.load(std::memory_order_acquire) || !g_site) return;
    g_ruiProbeArmed = 0;
    DWORD oldProtect = 0;
    if (VirtualProtect(g_site, kCallBytes, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::memcpy(g_site, g_savedCall, kCallBytes);
        FlushInstructionCache(GetCurrentProcess(), g_site, kCallBytes);
        DWORD ignored = 0;
        VirtualProtect(g_site, kCallBytes, oldProtect, &ignored);
    }
    g_installed.store(false, std::memory_order_release);
    Tf2VrLog("[TF2VR] RUI probe removed; the original call is back.\n");
}


// ---- the shipped control: one layer type, scaled, persisted ---------------
//
// The per-type sweep did its job on the first pulse: type 3 -- 59% of all RUI
// draw calls -- shrank the HUD. The other five types are not worth a run.
// Types 1 and 0 are element-sized coordinate spaces (256x256 and 925x28), so
// scaling them would distort individual widgets rather than reposition the HUD.
//
// So this stops being a sweep and becomes the control: the layer type and the
// factor both come from the INI and are reloadable live with LEADER then END,
// which is this project's tuning path and the only one that works with a
// headset on. The key is now a straight A/B toggle, because judging a size by
// eye means seeing it with and without, back to back, in one session -- every
// wrong answer in this project's history came from comparing across runs.
namespace {
bool g_scaleOn = false;
}  // namespace

void SetRuiLayerType(int mask) {
    g_ruiTypeMaskWanted = mask & 0x0F;
    if (g_scaleOn) g_ruiTypeMask = g_ruiTypeMaskWanted;
}

void ToggleRuiLayerScale() {
    if (!g_installed.load(std::memory_order_acquire)) {
        ToggleRuiProbe();
        if (!g_installed.load(std::memory_order_acquire)) return;
    }
    g_scaleOn = !g_scaleOn;
    g_ruiWrites = 0;
    g_ruiTypeMask = g_scaleOn ? g_ruiTypeMaskWanted : 0;
    // The magnitude follows the arm state in the same breath, so the A/B is a
    // real A/B: turning the inset off also hands the engine its own safe-area
    // fraction back, instead of leaving types 4/6/7 insetted by our value.
    PushRuiInsetToCvar();
    char line[440]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] HUD size %s: layer types 0x%X, inset %.3f. %s\n",
        g_scaleOn ? "ON" : "OFF", g_ruiTypeMaskWanted, static_cast<double>(g_ruiInset),
        g_scaleOn ? "Inset shrinks it about the CENTRE; bias slides it."
                  : "Back to the game's own layout.");
    Tf2VrLog(line);
}

// The falsifier, on its own clock. A control that is on but reaching no calls
// and one reaching calls with a value too small to see are different problems
// that look identical from the chair, and the wearer cannot read this while
// wearing the headset -- so it has to still be here afterwards.
void AdvanceRuiProbe() {
    // FIRST, AND WHETHER OR NOT ANYTHING IS ARMED: land the magnitude. The INI
    // parse ran before engine.dll owned a cvar table, so the write that carries
    // the inset's SIZE was thrown away and the flag it sets is the only record
    // of that. Retry on a slow clock until a read-back agrees; after that this
    // costs nothing.
    if (!g_insetTook) {
        static std::uint64_t lastPush = 0;
        const std::uint64_t tick = GetTickCount64();
        if (tick - lastPush >= 500) {
            lastPush = tick;
            const bool took = PushRuiInsetToCvar();
            static bool announced = false;
            if (took && !announced) {
                announced = true;
                char line[280]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] HUD size: rui_safeAreaFrac now holds %.2f (inset %.3f), read back "
                    "and confirmed. Until this line the magnitude was the engine's own default.\n",
                    static_cast<double>(g_insetReadBack), static_cast<double>(g_ruiInset));
                Tf2VrLog(line);
            }
        }
    }
    if (!g_scaleOn) return;
    static std::uint64_t lastTick = 0;
    const std::uint64_t now = GetTickCount64();
    if (now - lastTick < 2000) return;
    lastTick = now;
    // The falsifier carries the magnitude's read-back as well as the call count,
    // because "armed and reaching calls" and "armed at the size we asked for"
    // are different questions and only one of them was ever answered here.
    char line[360]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] HUD size: types 0x%X, inset %.3f, %llu calls adjusted; "
        "rui_safeAreaFrac wanted %.2f read %.2f %s\n",
        g_ruiTypeMaskWanted, static_cast<double>(g_ruiInset),
        static_cast<unsigned long long>(g_ruiWrites),
        static_cast<double>(g_ruiInset * 20.0f), static_cast<double>(g_insetReadBack),
        g_insetTook ? "OK." : "-- NOT TOOK, so the size on screen is not the size asked for.");
    Tf2VrLog(line);
}

bool RuiLayerScaleOn() { return g_scaleOn; }

// ---- arming on load, ONCE -------------------------------------------------
//
// Deliberately not the arms.collapse pattern. That one keeps a `wanted` flag
// and re-arms from a clock, which is right for a feature with no A/B: disarming
// it is never something you want to persist. Here F6 IS the A/B, and a re-arm
// on the next tick would put the HUD back a sixth of a second after the wearer
// asked to see it without -- a key that does nothing, which this project has
// already learned is indistinguishable from a broken one. So the INI flag fires
// exactly once and then hands the control over.
namespace {
bool g_armOnLoad = false;
}  // namespace

void SetRuiArmOnLoad(bool wanted) {
    g_armOnLoad = wanted;
    Tf2VrLog(wanted ? "[TF2VR] HUD size: WANTED from the ini; it arms itself once, at the ini's "
                      "inset, as soon as a world is being rendered. F6 still A/Bs it.\n"
                    : "[TF2VR] HUD size: not wanted from the ini; F6 arms it.\n");
}

bool RuiArmOnLoad() { return g_armOnLoad; }

void ReportRuiLayerScale() {
    // Deliberately loud and deliberately in the ini's own syntax. This is the
    // end of a session spent dialling by feel with a headset on, where the
    // wearer could not see a single line of the log while doing it -- so the
    // one thing that must survive is the pair of numbers, in a form that can be
    // pasted straight into the file without being transcribed.
    // The bias_x / bias_y lines that used to be here named knobs that do not
    // exist: the inset went through the engine's own safe-area branch, which
    // takes ONE scalar off all four edges, and the per-edge position control
    // died with the post-call version. They were also two format specifiers
    // more than there were arguments, so this printed garbage for the layer
    // type and read past the end of the argument list to do it.
    char line[560]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] ======== HUD SIZE SAVED -- BOTH HALVES ========\n"
        "[TF2VR] set rui.inset                      = %.3f\n"
        "[TF2VR] set rui.layer_type                 = %d\n"
        "[TF2VR] set rui.arm                        = 1\n"
        "[TF2VR] set rui.ll_scale                   = %.3f\n"
        "[TF2VR] set rui.ll_arm                     = 1\n"
        "[TF2VR] (top-left %s, lower-left %s; %llu RUI calls adjusted)\n"
        "[TF2VR] ==============================================\n",
        static_cast<double>(g_ruiInset), g_ruiTypeMaskWanted,
        static_cast<double>(RuiLowerLeftScale()), g_scaleOn ? "ON" : "OFF",
        RuiLowerLeftArmed() ? "ON" : "OFF", static_cast<unsigned long long>(g_ruiWrites));
    Tf2VrLog(line);
}

float RuiInset() { return g_ruiInset; }

// THE ENGINE DOES THE MATH NOW. Its safe-area branch computes size*(1-2f) and
// writes the per-edge offset itself, at the point inside FC500 where both are
// still going to be used -- which is what the post-call version could not reach.
// All we supply is f, through the cvar the branch reads.
//
// f = inset / 0.05, because FC500 multiplies rui_safeAreaFrac by the constant at
// 5F45B0, which is 0.05. The slot write bypasses any clamp the console setter
// would apply, so the engine 5 per cent ceiling does not bind us.
namespace {
// SUBMITTED IS NOT EXECUTED IS NOT TOOK, and this write has a load-time hole
// wide enough to arm the whole feature at the wrong size.
//
// `SetRuiInset` is called from the INI parse, which runs from the plugin's
// Load() -- before engine.dll has registered a single cvar. `FindVar` returns
// null there, the write fails, and NOTHING SAYS SO. Everything still looked
// right in the headset runs only because the wearer nudged, and a nudge calls
// this again once the cvar exists. An arm-on-load that never nudges would take
// the engine's own default frac (1.0 x 0.05 = a 5% inset) while the log
// happily printed "inset 0.100".
//
// So the wanted value is kept here and re-pushed on the frame clock until a
// READ-BACK agrees. Retrying costs one FindVar every 500 ms until it lands,
// then nothing. Both flags are declared at the top of the file, where the
// falsifier can see them.
// A REGRESSION I INTRODUCED AND THE FIRST A1 RUN CAUGHT.
//
// `rui_safeAreaFrac` is the ENGINE'S OWN cvar, and layer types 4, 6 and 7 take
// its safe-area path unaided -- that is the whole reason the type-3
// substitution works. So writing it is not inert: it insets those three layer
// types whether or not our inset is armed. The first census run logged
// "rui_safeAreaFrac now holds 2.00" with `rui.arm = 0`, which means that run's
// baseline capture is NOT vanilla for types 4/6/7, and the claim that the
// census "changes nothing" was wrong by exactly that much.
//
// The engine's own value is therefore captured before the first write and put
// back on disarm, and the push only happens while the inset is actually on.
float g_fracOriginal = 0.0f;
bool g_fracOriginalKnown = false;

bool PushRuiInsetToCvar() {
    float actual = 0.0f;
    if (!g_fracOriginalKnown) {
        if (!TryReadCvarFloat("rui_safeAreaFrac", actual)) return false;
        g_fracOriginal = actual;
        g_fracOriginalKnown = true;
    }
    // The value the engine should be holding right now: ours while the inset is
    // armed, its own the rest of the time.
    const float wanted = RuiLayerScaleOn() ? g_ruiInset * 20.0f : g_fracOriginal;
    if (!TrySetCvarFloat("rui_safeAreaFrac", wanted)) {
        g_insetTook = false;
        return false;
    }
    if (!TryReadCvarFloat("rui_safeAreaFrac", actual)) {
        g_insetTook = false;
        return false;
    }
    g_insetReadBack = actual;
    // Exact, because both sides are the same float written and read through the
    // same slot -- there is no arithmetic between them to lose a bit to.
    g_insetTook = (actual == wanted);
    return g_insetTook;
}
}  // namespace

bool RuiInsetCvarTook() { return g_insetTook; }
float RuiInsetCvarReadBack() { return g_insetReadBack; }

void SetRuiInset(float v) {
    if (v < -0.45f) v = -0.45f;
    if (v > 0.45f) v = 0.45f;
    g_ruiInset = v;
    PushRuiInsetToCvar();
}

// ---- isolation: which layer type is the lower-left group? -----------------
//
// With every candidate insetting at once, only the top-left cluster moved. That
// rules out 4/6/7 -- those take the engine's path unaided and were already
// getting the full frac -- and type 3 is the top-left. So the lower-left group
// is type 0 or type 1, the element-sized spaces, and something about them is
// not honouring the inset.
//
// NOTHING TO COUNT. Each press isolates the next single type and says so in the
// log. The wearer presses, looks, and stops the moment the lower-left moves; the
// log's last line names the type. That keeps the judgement where it belongs --
// on what is visible -- and the bookkeeping where it belongs, in the file.
void IsolateNextRuiType() {
    static const int kTypes[] = {0, 1, 3, 2};
    static int next = 0;
    const int type = kTypes[next % 4];
    next = (next + 1) % 4;
    g_ruiTypeMaskWanted = 1 << type;
    if (g_scaleOn) g_ruiTypeMask = g_ruiTypeMaskWanted;
    g_ruiWrites = 0;
    char line[400]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] ======== ISOLATING RUI LAYER TYPE %d ========\n"
        "[TF2VR] Only this type is being inset now (inset %.3f). If the lower-left group "
        "moved on THIS press, type %d is the one -- stop here and say so.\n",
        type, static_cast<double>(g_ruiInset), type);
    Tf2VrLog(line);
}

// ---- RUNG A1: the read-only identity census ------------------------------
//
// See the long note in rui_probe.h for why this needs no second code patch.
// Everything below runs on the render thread, inside the thunk, once per RUI
// layer draw -- roughly 3,700 times a second.
//
// COST IS BOUNDED BY CONSTRUCTION. The tables are fixed size and cannot grow,
// so a run left going for an hour costs exactly what a run of ten seconds
// costs: a walk of at most 32 pointers per call. The expensive part -- turning
// two addresses into module+RVA -- happens ONCE PER IDENTITY, at most forty
// times in a session, and never again.

extern "C" {
volatile unsigned char g_ruiCensusArmed = 0;
}

namespace {

// TYPE 0 GETS ITS OWN BUDGET, and that is not tidiness.
//
// The last census saw type 0 1,572 times against type 3's 83,434 -- fifty to
// one. A single shared table of identities fills up with whatever is frequent,
// and the one layer this project has an open, untested question about is
// precisely the rare one. A shared cap lets the frequent side starve the rare
// side; separate tables cannot.
// THE FIRST RUN PROVED THIS PATH IS MULTI-THREADED, and it did so twice over.
//
// The falsifier came back UNBALANCED: 368,074 calls counted against 370,851
// tallied. A table cannot tally more than the counter that is incremented
// before it, from one thread -- but a plain `++` on the single hottest counter
// loses updates under contention, and that counter runs on 100% of calls while
// each identity's runs on a fraction. So the global lost the most, and the
// delta came out negative.
//
// And the table came back with TWO BLANK ROWS. Publishing by incrementing the
// count last is single-thread-correct and no help here: two threads both read
// count = N, both fill slot N, and both increment -- so count reaches N+2 while
// slot N+1 was never written, and one identity is lost outright.
//
// Both are now fixed the same way: counters are interlocked, and a slot is
// CLAIMED with an interlocked increment before it is filled. Two threads
// racing on the same descriptor can now produce a duplicate ROW, which is
// harmless -- the counts still sum correctly and the duplicate is visible in
// the dump -- rather than a blank one, which loses an identity silently.
//
// The cap is raised because the first run reached 31 of 32 with two of those
// wasted, so it was one identity away from dropping real ones.
constexpr int kMaxIdentities = 64;
constexpr int kMaxType0Identities = 16;
// Per identity, a few of the distinct LAYER objects behind it. This is the
// "is the descriptor grain fine enough" measurement: if one descriptor covers
// many layer objects, there is a finer key available and A2 may need it.
constexpr int kMaxLayersPerIdentity = 8;

struct Identity {
    std::uintptr_t descriptor;
    std::uintptr_t target;
    float w;
    float h;
    volatile long typeBits;   // one bit per layer-type byte seen with it
    volatile long long calls;
    std::uintptr_t layers[kMaxLayersPerIdentity];
    volatile long layerCount;
    volatile long layersDropped;
    char descWhere[64];
    char targetWhere[64];
};

Identity g_identities[kMaxIdentities]{};

// ---- EAGER SUBSTITUTION, and why it is a FIX rather than an optimisation ---
//
// Wearer, 2026-09-06: the waypoint comes up about twice the size for two or
// three seconds and then settles, and it does that only the FIRST time it
// appears in a session. Every later appearance is already correct.
//
// That is us, not the game. `hud.marker_size` is applied in RuiA3Transform,
// which only runs once the widget's descriptor carries OUR draw function. Until
// that substitution lands the widget draws at its natural size -- the 1.8952x
// magnification that restoring the pass's uploaded frustum brought back -- and
// 1/0.45 = 2.22x is exactly the "about 2x, maybe a bit more" reported. The
// substitution used to arrive on the block census's 500 ms retry, seconds after
// the widget first drew, and it is permanent once installed, which is why only
// the first appearance was ever affected. Every part of the widget was in its
// right place throughout, which is what the wearer described and what ruled out
// an animation: a size-change detector watching this widget's own boxes across
// 8.9 seconds and 2892 draws fired exactly once, on the first draw.
//
// The census hook runs at FC500's ENTRY and the descriptor's draw function is
// virtual-called at FC6B1, so substituting from here catches the very first
// draw rather than the hundredth. Declared up here, defined next to
// SubstituteOne, which owns the table.
bool TrySubstituteIdentityNow(Identity& id);
// The table is touched from the render thread now as well as the game thread.
// Nothing here blocks: a failed claim just means the other path is installing,
// and there are hundreds of draws a second to retry on.
volatile long g_subLock = 0;
volatile long g_identityReserved = 0;
volatile long long g_identitiesDropped = 0;

Identity g_type0[kMaxType0Identities]{};
volatile long g_type0Reserved = 0;
volatile long long g_type0Dropped = 0;

// Counted in the helper, independently of the tables. THE FALSIFIER: the two
// must agree, and if they do not, the table is lying about the distribution.
volatile long long g_censusCalls = 0;
// THE VERIFIED POSITIVE CONTROL. These eight counters are computed without
// touching the identity tables at all, and they are directly comparable to the
// per-type census in HANDOFF-TO-PLANNER-2026-08-20-EVENING.md section 3 --
// numbers that were MEASURED, and that the type-3 isolation then tied to a
// visible change on screen. If this histogram reproduces those proportions,
// the instrument is demonstrably reading the same thing that has already been
// correlated with the HUD moving. A control that is itself unverified is what
// cost this project a whole run once already.
volatile long long g_typeHistogram[8]{};
volatile long long g_typeOutOfRange = 0;
// The type-0 passive rider. UNTESTED and never to be reported closed: type 0
// is situational, and no isolation window has ever caught one. The first
// sighting is stamped so we learn WHAT SPAWNS IT rather than just that it did.
std::uint64_t g_type0FirstSeenTick = 0;
std::uint64_t g_censusArmedTick = 0;
bool g_type0Announced = false;
bool g_censusWanted = false;

// Turns a live address into something a report can carry across runs. Done on
// first sight of an identity only.
void DescribeAddress(std::uintptr_t address, char* out, std::size_t size) {
    out[0] = 0;
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(address), &module) ||
        !module) {
        // NOT AN ERROR, AND THE MOST IMPORTANT CASE. An address outside every
        // module is heap -- which means it is NOT STABLE ACROSS RUNS, and A2
        // cannot key its suppression on it. Say so here rather than let a
        // later run discover it the expensive way.
        std::snprintf(out, size, "heap(unstable key)");
        return;
    }
    char path[MAX_PATH]{};
    GetModuleFileNameA(module, path, MAX_PATH);
    const char* name = path;
    for (const char* p = path; *p; ++p) {
        if (*p == 92 || *p == '/') name = p + 1;
    }
    std::snprintf(out, size, "%s+0x%llX", name,
                  static_cast<unsigned long long>(address -
                                                  reinterpret_cast<std::uintptr_t>(module)));
}

void NoteLayer(Identity& id, std::uintptr_t layer) {
    long taken = id.layerCount;
    if (taken > kMaxLayersPerIdentity) taken = kMaxLayersPerIdentity;
    for (long i = 0; i < taken; ++i) {
        if (id.layers[i] == layer) return;
    }
    // Same interlocked claim as the identity table itself. This column exists
    // only to say whether a finer grain than the descriptor exists, so a
    // duplicate under a race costs nothing -- but a torn index would walk off
    // the end of the array, which costs everything.
    const long slot = InterlockedIncrement(&id.layerCount) - 1;
    if (slot >= kMaxLayersPerIdentity) {
        InterlockedIncrement(&id.layersDropped);
        return;
    }
    id.layers[slot] = layer;
}

// THE SLOT IS CLAIMED BEFORE IT IS FILLED, with an interlocked increment, so
// no two threads can ever be handed the same one. `reserved` is how many slots
// have been handed out; `published` is how many are safe to read, and an entry
// is published only once every field in it has been written.
// A COUNT OF PUBLISHED SLOTS WOULD NOT WORK, because slots do not finish in the
// order they are claimed: if thread A takes slot 5 and B takes 6, B can finish
// first, and there is no count that describes "6 is ready but 5 is not". So
// there is no published count -- the DESCRIPTOR ITSELF is the publish flag. It
// is written last, and a slot still being filled reads as zero, which simply
// never matches and is skipped by the reporter.
Identity* FindOrAdd(Identity* table, volatile long& reserved, int capacity,
                    volatile long long& dropped, std::uintptr_t descriptor, bool& isNew) {
    isNew = false;
    long taken = reserved;
    if (taken > capacity) taken = capacity;
    for (long i = 0; i < taken; ++i) {
        if (table[i].descriptor == descriptor) return &table[i];
    }
    const long slot = InterlockedIncrement(&reserved) - 1;
    if (slot >= capacity) {
        // A FULL TABLE MUST SAY SO. Silently ignoring the overflow would make
        // an incomplete census look like a complete one, which is the exact
        // shape of this project's most expensive mistakes.
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&dropped));
        return nullptr;
    }
    isNew = true;
    return &table[slot];
}

}  // namespace

extern "C" void RuiCensusSample(void* context, void* layer) {
    if (!context || !layer) return;
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_censusCalls));

    const auto type = *(reinterpret_cast<const std::uint8_t*>(context) + 0x10);
    if (type < 8) {
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_typeHistogram[type]));
    } else {
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_typeOutOfRange));
    }

    // r11 = [rdx], exactly as FC500+0x0E does it.
    const auto descriptor = *reinterpret_cast<const std::uintptr_t*>(layer);
    if (!descriptor) return;

    const bool isType0 = (type == 0);
    bool isNew = false;
    Identity* id = isType0
        ? FindOrAdd(g_type0, g_type0Reserved, kMaxType0Identities, g_type0Dropped, descriptor,
                    isNew)
        : FindOrAdd(g_identities, g_identityReserved, kMaxIdentities, g_identitiesDropped,
                    descriptor, isNew);
    if (!id) return;

    if (isNew) {
        // First sight of this identity: the only place the expensive work runs,
        // and the slot is ours alone -- the interlocked claim guarantees it.
        id->target = *reinterpret_cast<const std::uintptr_t*>(
            reinterpret_cast<const std::uint8_t*>(descriptor) + 0x68);
        id->w = *reinterpret_cast<const float*>(
            reinterpret_cast<const std::uint8_t*>(descriptor) + 0x18);
        id->h = *reinterpret_cast<const float*>(
            reinterpret_cast<const std::uint8_t*>(descriptor) + 0x1C);
        DescribeAddress(descriptor, id->descWhere, sizeof(id->descWhere));
        DescribeAddress(id->target, id->targetWhere, sizeof(id->targetWhere));
        // THE DESCRIPTOR IS WRITTEN LAST, and writing it is what publishes the
        // slot: until this store lands the entry reads as zero, matches
        // nothing, and is skipped by the reporter.
        _ReadWriteBarrier();
        id->descriptor = descriptor;
        // THE SECOND DRAW SLOT. This census reads +0x68 because that is what
        // FC500 virtual-calls, but a descriptor carries a second draw function
        // at +0x70 that the immediate path (engine+0xFC960) calls instead, and
        // nothing here has ever looked at it. Handing the descriptor over lets
        // rui_immediate.cpp read both slots and run the watchlist over them.
        // Also supplies that module's pool-scan positive control.
        RuiImmediateNoteFc500Descriptor(descriptor);
    }
    InterlockedOr(&id->typeBits, static_cast<long>(1u << type));
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&id->calls));
    NoteLayer(*id, reinterpret_cast<std::uintptr_t>(layer));

    // BEFORE THE DRAW THIS CALL IS ABOUT TO MAKE. See the note at
    // TrySubstituteIdentityNow: this is what stops the marker's first
    // appearance being drawn unscaled.
    TrySubstituteIdentityNow(*id);

    if (isType0 && !g_type0Announced) {
        g_type0Announced = true;
        g_type0FirstSeenTick = GetTickCount64();
    }
}

namespace {

void ReportIdentities(const char* heading, const Identity* table, long reserved, int capacity,
                      long long dropped) {
    long taken = reserved;
    if (taken > capacity) taken = capacity;
    // Slots that have been claimed but whose descriptor store has not landed
    // yet read as zero. They are in flight, not empty -- and printing them as
    // rows is what produced the two blank lines in the first run's dump.
    long live = 0;
    for (long i = 0; i < taken; ++i) {
        if (table[i].descriptor) ++live;
    }
    char line[440]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR]   %s: %ld identities (%ld slots claimed, %ld in flight), %lld dropped "
                  "for want of a slot.\n",
                  heading, live, taken, taken - live, dropped);
    Tf2VrLog(line);
    for (long i = 0; i < taken; ++i) {
        const Identity& id = table[i];
        if (!id.descriptor) continue;
        char types[40]{};
        int at = 0;
        for (int t = 0; t < 8 && at < 30; ++t) {
            if (id.typeBits & (1L << t)) {
                at += std::snprintf(types + at, sizeof(types) - at, at ? ",%d" : "%d", t);
            }
        }
        long shown = id.layerCount;
        if (shown > kMaxLayersPerIdentity) shown = kMaxLayersPerIdentity;
        std::snprintf(line, sizeof(line),
            "[TF2VR]     desc %s  target %s  space %.1fx%.1f  types{%s}  calls %lld  "
            "layers %ld(+%ld)\n",
            id.descWhere, id.targetWhere, static_cast<double>(id.w), static_cast<double>(id.h),
            types[0] ? types : "-", id.calls, shown, id.layersDropped);
        Tf2VrLog(line);
    }
}

}  // namespace

void ReportRuiCensus() {
    long generalTaken = g_identityReserved;
    if (generalTaken > kMaxIdentities) generalTaken = kMaxIdentities;
    long type0Taken = g_type0Reserved;
    if (type0Taken > kMaxType0Identities) type0Taken = kMaxType0Identities;

    long long tallied = 0;
    for (long i = 0; i < generalTaken; ++i) tallied += g_identities[i].calls;
    for (long i = 0; i < type0Taken; ++i) tallied += g_type0[i].calls;
    const long long dropped = g_identitiesDropped + g_type0Dropped;
    const long long accounted = tallied + dropped;
    const long long calls = g_censusCalls;

    char line[640]{};
    Tf2VrLog("[TF2VR] ======== RUNG A1: RUI IDENTITY CENSUS (read-only) ========\n");
    // THE COUNTERS ARE INTERLOCKED NOW, so a delta here is no longer explained
    // by lost updates. But they are still read one at a time while the render
    // threads keep going, so a SMALL POSITIVE delta -- calls ahead of the
    // tables -- is just the snapshot skew of reading the tables first. A
    // negative delta, or a large one, is not, and means something real.
    std::snprintf(line, sizeof(line),
        "[TF2VR]   FALSIFIER: thunk saw %lld calls, tables tallied %lld, dropped %lld, "
        "delta %+lld. %s\n",
        calls, tallied, dropped, calls - accounted,
        calls == 0
            ? "ZERO CALLS -- the call site is not patched, so nothing below means anything."
            : (calls == accounted
                   ? "Balanced."
                   : (calls > accounted && (calls - accounted) < (calls / 100 + 64)
                          ? "Within snapshot skew (tables read first, calls read after)."
                          : "OUT OF TOLERANCE -- do not trust the distribution below.")));
    Tf2VrLog(line);

    // The positive control, printed next to the numbers it has to reproduce.
    std::snprintf(line, sizeof(line),
        "[TF2VR]   POSITIVE CONTROL, layer-type histogram (compare with the MEASURED census in "
        "HANDOFF 2026-08-20-EVENING s3: t3 83434, t4 21536, t1 15321, t6 14928, t7 5107, "
        "t0 1572, no t2):\n"
        "[TF2VR]     t0=%lld t1=%lld t2=%lld t3=%lld t4=%lld t5=%lld t6=%lld t7=%lld oor=%lld\n",
        g_typeHistogram[0], g_typeHistogram[1], g_typeHistogram[2], g_typeHistogram[3],
        g_typeHistogram[4], g_typeHistogram[5], g_typeHistogram[6], g_typeHistogram[7],
        g_typeOutOfRange);
    Tf2VrLog(line);

    ReportIdentities("general", g_identities, g_identityReserved, kMaxIdentities,
                     g_identitiesDropped);
    // ALWAYS PRINTED, EVEN EMPTY. Type 0 is the open question this project has
    // twice failed to test, and an absent section reads as "not applicable"
    // where an empty one reads as "still not caught" -- which is the truth.
    ReportIdentities("type 0 (SITUATIONAL, UNTESTED -- never report this closed)", g_type0,
                     g_type0Reserved, kMaxType0Identities, g_type0Dropped);
    if (type0Taken == 0) {
        Tf2VrLog("[TF2VR]     no type-0 layer was seen in this run at all. Still UNTESTED, not "
                 "cleared: it needs whatever game state spawns a 925x28 space.\n");
    } else if (g_type0FirstSeenTick) {
        // RELATIVE TO THE CENSUS ARMING, not to GetTickCount64's own epoch --
        // which is system uptime, and printed "88640453 ms into the process" in
        // the first run, i.e. twenty-five days, which is nonsense.
        std::snprintf(line, sizeof(line),
            "[TF2VR]     first type-0 sighting was %.1f s after the census armed -- note what was "
            "on screen then.\n",
            static_cast<double>(g_type0FirstSeenTick - g_censusArmedTick) / 1000.0);
        Tf2VrLog(line);
    }
    Tf2VrLog("[TF2VR] ==========================================================\n");
}

void SetRuiCensusWanted(bool wanted) {
    g_censusWanted = wanted;
    // A LIVE OFF SWITCH, because LEADER then END is this project's only tuning
    // path and a setting that cannot be turned back off inside a session is a
    // setting that costs a restart to undo. PUBLISHES BEFORE IT DISARMS: a
    // census that goes quiet without printing has thrown its run away.
    if (!wanted && g_ruiCensusArmed) {
        Tf2VrLog("[TF2VR] RUI identity census: turned off from the ini. Final dump follows.\n");
        ReportRuiCensus();
        g_ruiCensusArmed = 0;
    }
    Tf2VrLog(wanted ? "[TF2VR] RUI identity census: WANTED from the ini. Read-only -- it installs "
                      "the call-site patch and changes nothing; the inset stays off unless "
                      "rui.arm asks for it.\n"
                    : "[TF2VR] RUI identity census: not wanted.\n");
}

bool RuiCensusWanted() { return g_censusWanted; }

void ArmRuiCensus() {
    if (!g_installed.load(std::memory_order_acquire)) {
        ToggleRuiProbe();
        if (!g_installed.load(std::memory_order_acquire)) return;
    }
    g_censusArmedTick = GetTickCount64();
    g_ruiCensusArmed = 1;
    Tf2VrLog("[TF2VR] RUI identity census ARMED: read-only per-call descriptor identity at the "
             "0xFC6B1 seam. The census dumps itself every five seconds, so a run that is never "
             "disarmed still leaves a readable instrument.\n");
}

void AdvanceRuiCensus() {
    if (!g_ruiCensusArmed) return;
    static std::uint64_t last = 0;
    const std::uint64_t now = GetTickCount64();
    if (now - last < 5000) return;
    last = now;
    ReportRuiCensus();
}

// ---- RUNG A2: selective suppression by resolved draw target ---------------
//
// A1 answered the question A2 was going to have to guess at. Every identity's
// `descriptor+0x68` resolves into **ui(11).dll** -- `r2\paks\Win64\ui(11).dll`,
// the shipped RUI module -- and each RUI widget class has its OWN function
// there. That makes the target a far better key than the descriptor:
//
//   - the descriptor sits in engine.dll's `.data`, whose virtual size is
//     0x145279C0 (340 MB) against 0xDC400 of raw data, so those addresses are
//     runtime-populated slots in a pool. Stable-looking, but a slot;
//   - the target is a CODE address in a shipped file. It is the same RVA in
//     every run, on every machine, and it MEANS something.
//
// And it can be read offline. Each of those functions loads the RUI asset
// paths it draws as rip-relative string pointers, so `pescan fullfunc` plus a
// dump of what it references names the widget without spending a run:
//
//   ui(11)+0x2B190  rui/hud/earn_meter/*, titanfall_marker_arrow, #CORE_ACTIVE,
//                   #CHANGE_BOOST, and `$rui/death_recap/health_icon%%`0%.0f`
//   ui(11)+0x1010   hud\weapon_status.rui, rui/hud/cooldown_segment/gradient
//   ui(11)+0x5090   `0NO AMMO / `0LOW AMMO / `0RELOAD
//   ui(11)+0x68150  rui/hud/training/gauntlet_hint  (the 925x28 type-0 layer)
//
// So the sweep is RANKED, not blind: 0x2B190 is the health-icon-and-earn-meter
// function and the lower-left group is where Titanfall 2 draws exactly that.
//
// THIS IS STILL A GUESS UNTIL IT IS TESTED. The offline reading is evidence,
// not a result -- a confirmed defect need not be the defect -- which is what
// this rung exists to settle.
//
// EVERY ARM CARRIES ITS OWN POSITIVE CONTROL. `suppressed` counts the calls
// actually skipped for the selected target. If it is zero, "nothing changed on
// screen" says nothing at all about that candidate: the arm never fired. A null
// without a non-zero count is instrument failure, not a refutation.

extern "C" {
volatile std::uintptr_t g_ruiSuppressTarget = 0;
volatile std::uint64_t g_ruiSuppressed = 0;
// Raised only while one of the two lower-left widgets is inside its own draw.
// Read by rung B in camera_update_hook.cpp, which is how a Map call gets
// attributed to this widget rather than to the two million others.
volatile long g_ruiInDraw = 0;
}

static std::atomic<unsigned long long> g_controlCalls{0}, g_controlNull{0},
    g_controlKnown{0}, g_controlUnknown{0};
static unsigned long long g_controlStartCalls = 0, g_controlStartNull = 0,
    g_controlStartKnown = 0, g_controlStartUnknown = 0;

namespace {

struct Candidate {
    unsigned rva;        // in ui(11).dll
    const char* name;    // what its own strings say it draws
};

// Ranked by how well the strings match "the lower-left health group", then by
// how central the widget looks. The wearer stops the moment it blanks.
const Candidate kCandidates[] = {
    {0x2B190, "earn meter + death_recap/health_icon + titanfall marker + #CORE_ACTIVE/#CHANGE_BOOST"},
    {0x1010,  "hud/weapon_status.rui + cooldown_segment/gradient (3 calls a frame)"},
    {0x55D00, "hud/weapon_status.rui + cooldown_segment/gradient (second instance)"},
    {0x5090,  "ammo state: NO AMMO / LOW AMMO / RELOAD"},
    {0x127E0, "weapon_status + hud_defs + weapon_xp/pip (3.4 calls a frame)"},
    {0x4320,  "weapon_status + sb_dead_enemy + weaponcycle"},
    {0xC4D0,  "hud/battery/battery_capture_friendly|enemy"},
    {0xBF60,  "hud_defs + #HUD_DISTANCE_METERS + battery_generator + --:--"},
    {0x66DD0, "status_pilot_default + new_loadout_icon + %.0f m"},
    {0x5B800, "raid_bomb_icon + lockon_indicator + score_bar_fw_shield_fill"},
    {0x766D0, "#HUD_RODEO_ALERT + countermeasure/evade prompts"},
    {0x69570, "overhead_icon_titan_arrow_you"},
    {0x5F6B0, "#TITAN_READY + ammo_counter_mag + arc_launcher_reticle"},
    {0x809B0, "gamemode_fd + target_info + #HUD_DISTANCE_KILOMETERS"},
};
constexpr int kCandidateCount = static_cast<int>(sizeof(kCandidates) / sizeof(kCandidates[0]));

int g_candidate = -1;            // -1 = suppression off (the A/B arm)
std::uintptr_t g_uiBase = 0;
// Full-frame capture is requested a few frames AFTER the change, not on the
// same frame: the capture happens at the next Present, and a capture racing the
// arm would photograph the wrong half of the A/B.
int g_captureCountdown = 0;
char g_captureName[64]{};

std::uintptr_t UiModuleBase() {
    if (g_uiBase) return g_uiBase;
    // The exact basename A1 read back out of GetModuleFileNameA, so this is not
    // a guess about what the module is called.
    HMODULE m = GetModuleHandleA("ui(11).dll");
    if (!m) {
        Tf2VrLog("[TF2VR] A2: ui(11).dll is not loaded under that name. Suppression cannot "
                 "resolve a target, and nothing was armed.\n");
        return 0;
    }
    g_uiBase = reinterpret_cast<std::uintptr_t>(m);
    return g_uiBase;
}

}  // namespace

std::uint64_t RuiSuppressedCalls() { return g_ruiSuppressed; }

// Reports the arm that is ending, THEN moves on -- so the count that decides
// whether the last candidate was even reached is published before it is reset.
void SuppressNextRuiCandidate() {
    const std::uintptr_t base = UiModuleBase();
    if (!base) return;

    if (g_candidate >= 0) {
        char done[440]{};
        std::snprintf(done, sizeof(done),
            "[TF2VR] A2 candidate %d/%d (ui(11)+0x%X) ENDS: %llu calls suppressed. %s\n",
            g_candidate + 1, kCandidateCount, kCandidates[g_candidate].rva,
            static_cast<unsigned long long>(g_ruiSuppressed),
            g_ruiSuppressed == 0
                ? "ZERO -- this arm never fired, so anything seen during it says NOTHING about "
                  "this candidate."
                : "Reached; whatever was seen during it is about this candidate.");
        Tf2VrLog(done);
    }

    ++g_candidate;
    if (g_candidate >= kCandidateCount) {
        g_candidate = -1;
        g_ruiSuppressTarget = 0;
        g_ruiSuppressed = 0;
        Tf2VrLog("[TF2VR] ======== A2: LADDER EXHAUSTED, suppression OFF ========\n"
                 "[TF2VR] Every candidate has been through. If the lower-left group never "
                 "blanked, it is not one of these targets and rung A2 is closed by "
                 "measurement -- go to rung B.\n");
        return;
    }

    g_ruiSuppressed = 0;
    g_ruiSuppressTarget = base + kCandidates[g_candidate].rva;
    std::snprintf(g_captureName, sizeof(g_captureName), "tf2vr-a2-%02d-%X.bmp", g_candidate + 1,
                  kCandidates[g_candidate].rva);
    g_captureCountdown = 20;

    char line[520]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] ======== A2 CANDIDATE %d of %d: ui(11)+0x%X ========\n"
        "[TF2VR]   %s\n"
        "[TF2VR]   If the LOWER-LEFT group vanished on THIS press, this is the one -- stop here "
        "and say so.\n",
        g_candidate + 1, kCandidateCount, kCandidates[g_candidate].rva,
        kCandidates[g_candidate].name);
    Tf2VrLog(line);
}

void AdvanceRuiSuppression() {
    if (g_captureCountdown > 0 && --g_captureCountdown == 0) {
        RequestNamedBackbufferCapture(g_captureName);
    }
    if (g_candidate < 0) return;
    static std::uint64_t last = 0;
    const std::uint64_t now = GetTickCount64();
    if (now - last < 2000) return;
    last = now;
    // The falsifier for this rung, on its own clock: an arm that is reaching no
    // calls looks exactly like an arm that changes nothing.
    char line[400]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] A2: candidate %d/%d ui(11)+0x%X, %llu calls suppressed.%s\n",
        g_candidate + 1, kCandidateCount, kCandidates[g_candidate].rva,
        static_cast<unsigned long long>(g_ruiSuppressed),
        g_ruiSuppressed == 0 ? "  ZERO -- NOT REACHING THIS TARGET." : "");
    Tf2VrLog(line);
}

// ---- RUNG A3: transform the lower-left group's own placement --------------
//
// A2 named the two halves. This moves and resizes them, using the placement
// FC500 computes for itself in the seven instructions before the seam -- a
// pipeline the failed inset sweeps never touched.
//
// WHAT THOSE SEVEN INSTRUCTIONS DO. FC617..FC6A6 is an ASPECT FIT: it fits the
// layer's authored space (w,h from descriptor+0x18/+0x1C) into the target rect
// (layer+0x10/+0x14), producing a uniform scale where one axis is exactly 1.0
// and the other is the ratio. Tracking the lanes through the two `unpcklps`
// and the two `pshufd` (0xF0 = lanes 0,0,3,3; 0xCC = lanes 0,3,0,3):
//
//   [r10+0x3AC0] = [sx,   0,    0,    sy  ]        the fitted scale
//   [r10+0x2DF0] = [w*sx, w*sx, h*sy, h*sy]        the fitted size
//   [r10+0x3AD0] = [(1-sx)/2, (1-sy)/2, ... ]      THE POSITION
//
// That last one is the prize. The constant it is built from, at 5F3EB0, is
// [0.5,0.5,0.5,0.5], and `0.5 - 0.5*s` is exactly where a layer of normalised
// size s starts if it is CENTRED. So the engine already computes this group's
// position as a centred placement in normalised screen space -- lanes 0 and 2
// are x, lanes 1 and 3 are y -- and a centred rescale is one line of algebra
// rather than a transform we invent.
//
// r10 is `*(void**)(layer+0x18)`, a per-frame scratch the CALLER hands FC500
// (it is `lea rax,[rsp+0x1820]` at FC868 -- stack, rebuilt every call). So
// there is nothing to restore per call: the only persistent write is the
// descriptor slot itself.
//
// EVERY WRITE IS A DELTA AGAINST WHAT THE ENGINE JUST COMPUTED. The scale is
// read back out of 0x3AC0 and the position rebuilt from THAT, never from a
// remembered value or a global on another clock.

extern "C" void ruiA3Interceptor();
extern "C" void ruiPrimEndInterceptor();

namespace {

// The two halves A2 identified, by ui(11).dll RVA.
constexpr unsigned kLowerLeftLeftHalf = 0x1010;   // tactical + ordnance + bar
constexpr unsigned kLowerLeftRightHalf = 0x4320;  // weapon silhouette + ammo

struct Substitution {
    std::uintptr_t descriptor;   // whose +0x68 we overwrote
    std::uintptr_t original;     // what was in it
    unsigned rva;                // which half, for the log
    long typeBits;               // the layer types it was seen drawing on
};
// Eight, not four. Four was exactly the lower-left pair plus slack; the marker
// census adds a third target and a widget can carry more than one descriptor,
// so a cap of four could TRUNCATE the substitution set -- and this project has
// already been burned once by a cap hit exactly ("held=12 (cap 12)" meaning
// twelve runs of an arbitrary subset). The refusal below is loud for the same
// reason: a silently dropped descriptor is a census of a subset that reads
// exactly like a census of the whole.
// 64, not 8. Eight covered the lower-left pair, the marker and slack; the HUD
// size control substitutes every type-3 widget the census names and there are
// about fifteen of those, so eight would TRUNCATE the set -- and a silently
// dropped descriptor is a HUD where one element does not scale with the rest.
// SubstituteOne already refuses loudly when the table fills.
// 128, not 64. One slot per DESCRIPTOR, not per RVA, and the ladder substitutes
// every live ui(11) target rather than the transform's four types -- the run
// that measured 31 was substituting from the first keypress, not from load.
// A table that fills is a walk of a SUBSET, and this project has already been
// burned by a cap hit exactly; SubstituteOne still refuses loudly when it does.
// BACK TO 64, which is what the wearer's HUD was tuned against.
//
// It was raised to 128 so the ladder's walk could cover every widget. One slot
// per DESCRIPTOR, and substitution is what feeds the size transforms -- so
// doubling the table did not just observe more, it TRANSFORMED more: widgets
// that used to escape because the table filled at 64 were suddenly being
// scaled. Together with the ungated substitution above that is why the bottom
// bars and the background graphics moved as well as the health bar.
//
// SubstituteOne still refuses loudly when it fills, so a walk that needs more
// coverage will say so rather than quietly truncate.
Substitution g_subs[64]{};
int g_subCount = 0;

// `hud.size_arm`. Substitutes EVERY type-3 widget the census names, so the HUD
// size control below can reach the whole pilot and Titan HUD rather than the one
// marker widget. Read by the census hook on the render thread.
// `hud.widget_types`: a DECIMAL bitmask of the FC500 draw types the size
// control substitutes and scales. bit0=1 bit3=8 bit4=16 bit7=128, so 24 is
// types 3+4 and 153 is types 0+3+4+7.
//
// 2026-09-06: 24 was not enough. Every type-3/4 widget WAS substituted and the
// box witness cleared all of them -- not one moved its bottom edge down. So the
// jump/slide bars never moved at all; everything around them came inward and
// left them at the bottom, which is what the wearer saw. The same explains the
// friend and enemy labels staying large. Both live in types 0 or 7, which the
// census shows drawing throughout gameplay and which nothing was substituting.
// 2026-09-06 pass 2: the mask was DEAD -- declared, documented, and never read,
// because both substitution gates hardcoded bits 3 and 4. It is wired up now and
// carries bit 7, where the census puts the friend and enemy name labels: three
// widgets, 0x84070, 0x86220 and 0x8A330, each with thousands of simultaneous
// layers, which is one per labelled entity. Type 0 is deliberately NOT included:
// it is the frontend and menus.
volatile unsigned g_hudSizeTypes = 152;  // types 3, 4 and 7
volatile bool g_hudSizeArm = false;
// `rui.marker_target`, declared here because InstallA3 below needs it. Its full
// note and the candidate RVAs are at the block census, where it is consumed.
unsigned g_markerTargetRva = 0;
// The marker ladder. 1.0 is inert; F6 steps it. g_markerAdjusted is the
// falsifier: a null with zero adjusted draws is instrument failure, not a
// refutation -- the widget was never on screen.
volatile float g_markerScale = 1.0f;
// The head-delta correction gain. 0 = off, +1 and -1 are the two signs.
volatile float g_markerHeadFix = 0.0f;
volatile unsigned long long g_markerAdjusted = 0;
int g_markerStage = 0;

// THE LADDER IS THE RUN. Rather than dial blind, each press steps one arm whose
// expected effect is stated up front, so a single flat run settles both "does
// this reach the consumer" and "what do the lanes mean".
// ATTEMPT 2. Attempt 1 wrote the aspect-fit fields (0x3AC0 scale, 0x2DF0
// fitted size, 0x3AD0 position) and moved nothing -- with a passing control:
// the identity arm was correctly a no-op, the other arms verifiably wrote large
// values, and roughly a thousand draws a stage went through the substituted
// slot. The log said why it never had a chance: `engine gave
// scale(1.0000,1.0000) pos(0.0000,0.0000)`. The aspect fit is DEGENERATE here
// -- 1920x1080 authored into a 16:9 target is a ratio of exactly 1.0 -- so
// those fields are a letterbox correction carrying no positional information
// for this widget, and nothing reads them.
//
// What the disassembly of ui(11)+0x4320 shows instead: the widget asks the RUI
// primitive interface for a POINTER (slot +0x18 is literally
// `mov rax,[rcx+0x18]; add rax,0x2DD0; ret`) and fills a parameter block from
// there -- constants out of its own .rdata ([1920,1920,1080,1080] at D4A00,
// [216,216,72,72], [168,168,64,64] and so on, all pixel geometry in the
// authored space and all in FC500's own [x,x,y,y] broadcast layout) plus values
// evaluated from RUI data bindings.
//
// So the block base IS scratch+0x2DD0, and the three vectors FC500 writes are
// its first three entries. Which makes the cheap test the coordinate space at
// +0x2DD0 itself: it is the lever that demonstrably shrank the top-left cluster
// in rung 3, and it has never once been applied to THESE TWO WIDGETS -- rung 3
// scaled it by layer type, which we now know sprays twenty widget classes at
// once, and every later sweep used the inset mechanism instead.
//
// Every stage below is a one-line write into a block FC500 has just filled and
// nobody has yet read. All are absolute-free: each is a factor applied to what
// the engine computed, so nothing is rebuilt from a global on another clock.
// ---- THE LEVER, MEASURED --------------------------------------------------
//
// Scaling the coordinate space at scratch+0x2DD0 by f scales this group about
// the SCREEN CENTRE by 1/f. Not about the space's origin at top-left, which is
// what rung 3's corner-scale problem would have predicted and what this code
// originally said to expect -- the wearer called it immediately ("it shrunk
// towards the CENTER of the screen, not the top left") and the captures agree
// to two decimal places:
//
//     position_new = 0.5 + (position_old - 0.5) / f
//     size_new     = size_old / f
//
//   x  0.19 -> 0.345   predicted 0.345
//   y  0.90 -> 0.70    predicted 0.70
//
// THAT IS THE WHOLE COMPLAINT IN ONE NUMBER. The group is too big and too close
// to the bottom edge; a centred shrink makes it smaller, lifts it off the
// bottom AND pulls it in from the left, together, because they were never two
// problems. This is the "one cause, not two" the milestone was written around,
// and it is the first mechanism in the project to deliver it.
//
// The other two candidates are measured dead for this widget: the effective
// size at 0x2DE0 and the engine's own per-edge inset at 0x3AB0 both went
// through with the right values written and changed nothing. The second of
// those independently confirms the handoff's "the inset never reaches the
// lower-left" -- previously known only per LAYER TYPE, now known per WIDGET.
//
// Scoped, and checked rather than assumed: the top-left "JUMPKIT STATUS" text
// is the same glyph size armed and unarmed (it is further through its own
// typing animation, which is what made it look different at a glance).
enum class A3Mode {
    Identity,      // the control
    SpaceScale,    // scratch+0x2DD0 -- the authored coordinate space. THE LEVER.
    EffSizeScale,  // scratch+0x2DE0 -- measured dead for this widget
    Inset,         // scratch+0x3AB0 -- measured dead for this widget
};

struct A3Stage {
    A3Mode mode;
    float value;
    const char* expect;
};

// MAGNITUDE NOW, not mechanism. The mechanism is settled; this run is only
// choosing how much, so the arms are the plausible shipping range and the
// control still leads.
const A3Stage kStages[] = {
    {A3Mode::Identity, 1.0f,
     "IDENTITY -- the control. Must look exactly like the game's own layout."},
    {A3Mode::SpaceScale, 1.10f, "10% smaller, lifted slightly off the bottom-left corner."},
    {A3Mode::SpaceScale, 1.20f, "20% smaller. Probably the shipping neighbourhood."},
    {A3Mode::SpaceScale, 1.35f, "35% smaller and well clear of both edges."},
    {A3Mode::SpaceScale, 1.50f, "Third smaller again -- likely too far, which is the point of "
                                "having it: it brackets the answer from above."},
};
const char* const kModeNames[] = {"identity", "coordinate space", "effective size", "per-edge inset"};
constexpr int kStageCount = static_cast<int>(sizeof(kStages) / sizeof(kStages[0]));

int g_stage = -1;                       // -1 = off, the A/B arm
volatile std::uint64_t g_a3Calls = 0;
// What the transform actually applies when the ladder is not driving: the
// shipped, ini-backed value. 1.0 is inert.
volatile float g_llLive = 1.0f;
// `hud.marker_size`. Scales the watched marker widget's coordinate space, which
// is size-only: see the note at its use. 1.0 is off; 0.5277 exactly cancels the
// 1.8952x magnification that restoring the pass's uploaded frustum brings back.
volatile float g_markerSize = 0.45f;
// The F5 ladder for it, so the wearer can settle the size in one run.
constexpr float kMarkerSizeLadder[6] = {0.45f, 0.40f, 0.35f, 0.30f, 0.25f, 0.20f};
int g_markerSizeRung = 0;
volatile unsigned char g_a3Armed = 0;
int g_a3Capture = 0;
char g_a3CaptureName[64]{};
// One sample of the actual numbers, so the report can show what the engine
// computed and what we turned it into rather than asserting it worked.
float g_sampleBefore[4]{};
float g_sampleAfter[4]{};
bool g_sampleTaken = false;

}  // namespace

// ---- THE MARKER POSITION FIX, at RUI interface slot +0x30 -----------------
//
// Measured, not assumed: writing block+0x50 before the widget draws did nothing
// on 1118 proven draws, because ui(11)+0x80EFC fills it from a data binding
// during the call. The widget's LAST act is `call [rsi+0x30]`, which consumes
// the block -- so that call is the one instant after every fill and before use.
//
// The interface is SHARED (our own asm documents it as engine.dll+0x5F4320), so
// the slot is swapped for exactly the span of our widget's draw and put back the
// moment it returns. Nothing else can reach the thunk.
extern "C" {
volatile std::uintptr_t g_ruiPrimEndOriginal = 0;
}

namespace {
std::uint8_t* g_markerScratch = nullptr;
// THE SLOT IS WRITTEN ONCE PER SESSION AND NEVER AGAIN.
//
// The first version installed the thunk before each marker draw and restored
// the slot after it, and it CRASHED on the first press. The interface is
// `engine.dll+0x5F4320` -- `lea rcx, [rip]{5F4320}` at engine+0xFC668, a static
// address in read-only memory. The install did VirtualProtect, wrote, and then
// put the ORIGINAL PROTECTION BACK; the removal then wrote to a read-only page
// and took an access violation inside our own DLL.
//
// So the swap is now permanent and the GATE is a flag instead. The thunk stays
// in the slot for the session and is transparent whenever the flag is down:
// it saves the volatile registers, calls a function that returns immediately,
// restores them and tail-jumps to the original. Nothing is written to engine
// memory after the one install, so there is no protection hazard left to get
// wrong.
std::uintptr_t* g_primEndSlot = nullptr;
bool g_primEndInstalled = false;
// Raised for exactly the span of our marker widget's draw.
volatile long g_markerDrawActive = 0;
// Freshness, so a null says WHICH stage failed rather than leaving it to guess.
volatile unsigned long long g_markerSwaps = 0;      // slot swapped in
volatile unsigned long long g_markerThunkHits = 0;  // thunk actually entered
volatile unsigned long long g_markerFixes = 0;      // position actually rewritten
volatile float g_markerLastBefore = 0.0f;
volatile float g_markerLastAfter = 0.0f;
volatile float g_markerLastDelta = 0.0f;
}  // namespace

// Called from the thunk with NO arguments, immediately before the widget's
// block is consumed. Reads globals only.
extern "C" void RuiMarkerFixBlock() {
    // FIRST LINE, AND EVERYTHING DEPENDS ON IT. The thunk now sits in the
    // interface permanently, so this runs for EVERY RUI widget's final call.
    // The flag is raised only across our marker widget's draw; with it down
    // this returns before touching anything and the thunk is a no-op.
    if (!g_markerDrawActive) return;
    ++g_markerThunkHits;
    auto* scratch = g_markerScratch;
    if (!scratch) return;
    const float gain = g_markerHeadFix;
    if (gain == 0.0f) return;
    float yawDelta = 0.0f, headPitch = 0.0f;
    if (!GetHeadViewDelta(&yawDelta, &headPitch)) return;
    float pw = 0.0f, ph = 0.0f, halfTanX = 0.0f, halfTanY = 0.0f;
    int hadAim = 0;
    unsigned long long upd = 0;
    ReadReticleAnchorDetail(&pw, &ph, &halfTanX, &halfTanY, &hadAim, &upd);
    if (!(halfTanX > 0.0001f)) return;
    auto* space = reinterpret_cast<const float*>(scratch + 0x2DD0);
    const float W = space[0];
    if (!(W > 1.0f)) return;
    auto* px = reinterpret_cast<float*>(scratch + 0x2E20);   // block+0x50
    const float half = W * 0.5f;
    const float before = px[0];
    // Angle space, not pixels: the marker sits far off centre, where a pixel
    // offset would be wrong by the tangent's curvature.
    const float ndc = before / half - 1.0f;
    const float t = ndc * halfTanX;
    const float corrected =
        std::tan(std::atan(t) - gain * yawDelta * 3.14159265f / 180.0f);
    const float outPx = (corrected / halfTanX + 1.0f) * half;
    // Refuse a value that is not finite rather than writing a NaN into a live
    // draw: the widget would take it straight to the GPU.
    if (!(outPx == outPx) || outPx > 1.0e6f || outPx < -1.0e6f) return;
    px[0] = outPx;
    px[1] = outPx;
    g_markerLastBefore = before;
    g_markerLastAfter = outPx;
    g_markerLastDelta = yawDelta;
    ++g_markerFixes;
}

// Called unconditionally the instant the widget's draw returns. WRITES NO
// ENGINE MEMORY -- it only lowers the gate. That is the whole point of the
// redesign: the crash came from writing a read-only page on this path.
extern "C" void RuiMarkerRemoveSlotSwap() {
    g_markerDrawActive = 0;
    g_markerScratch = nullptr;
}

namespace {
// One write, once, and only when the fix is actually armed.
bool EnsurePrimEndInstalled(void* iface) {
    if (g_primEndInstalled) return true;
    if (!iface) return false;
    auto* slot = reinterpret_cast<std::uintptr_t*>(
        reinterpret_cast<std::uint8_t*>(iface) + 0x30);
    DWORD oldProt = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
        Tf2VrLog("[TF2VR] MARKER: could not make the RUI interface slot writable; the position "
                 "fix cannot arm. Nothing was changed.\n");
        return false;
    }
    g_ruiPrimEndOriginal = *slot;
    *slot = reinterpret_cast<std::uintptr_t>(&ruiPrimEndInterceptor);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProt, &ignored);
    g_primEndSlot = slot;
    g_primEndInstalled = true;
    ++g_markerSwaps;
    char line[260]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] MARKER: RUI interface slot +0x30 substituted ONCE for the session; original "
        "saved. It is inert unless a marker draw has the gate raised.\n");
    Tf2VrLog(line);
    return true;
}
}  // namespace

extern "C" void* RuiA3Transform(void* layer, void* iface) {
    if (!layer) return nullptr;
    const auto descriptor = *reinterpret_cast<std::uintptr_t*>(layer);
    std::uintptr_t original = 0;
    unsigned rva = 0;
    for (int i = 0; i < g_subCount; ++i) {
        if (g_subs[i].descriptor == descriptor) {
            original = g_subs[i].original;
            rva = g_subs[i].rva;
            break;
        }
    }
    if (!original) return nullptr;
    // THE BRACKET FOR RUNG B. Set here, cleared in RuiA3AfterDraw, so it is
    // raised for exactly the span of the widget's own draw -- and only for the
    // two targets, because only those descriptors reach this function.
    //
    // The first census tracked every buffer the game mapped: two million calls,
    // the 32-slot table full, 297,000 dropped, and almost every entry marked
    // CHANGES. Dynamic buffers change every frame, so "does it change" cannot
    // separate the HUD from the world. This can: a buffer mapped INSIDE this
    // window belongs to this widget's draw and nothing else does.
    //
    // Both exits are matched: this is only raised when a real original is about
    // to be called, and AfterDraw runs on exactly that path.
    g_ruiInDraw = 1;

    // THE MARKER LADDER RUNS BEFORE THE g_a3Armed GATE, AND THAT ORDER IS THE
    // WHOLE POINT. `g_a3Armed` is raised only by ArmRuiLowerLeft, which is
    // gated on `rui.ll_arm` -- and the stock-HUD baseline sets that to 0. So a
    // marker ladder placed after that gate can never apply, in exactly the
    // configuration this instrument was built for. That is the SAME failure as
    // the one-shot InstallA3 and the `if (!g_subCount)` caller, for the third
    // time in one sitting: a fix parked behind a flag that the run's own config
    // leaves off.
    //
    // The marker path owns its own arming -- g_markerScale, which is 1.0 until
    // F6 is pressed -- so it needs nothing from the lower-left feature and must
    // not inherit its gate.
    if (rva != kLowerLeftLeftHalf && rva != kLowerLeftRightHalf) {
        if (g_markerTargetRva && rva == g_markerTargetRva) {
            auto* mscratch = *reinterpret_cast<std::uint8_t**>(
                reinterpret_cast<std::uint8_t*>(layer) + 0x18);
            if (mscratch) {
                auto* mspace = reinterpret_cast<float*>(mscratch + 0x2DD0);
                // THE SIZE, SEPARATED FROM THE TRACKING. 2026-09-05: restoring
                // the HUD pass's uploaded frustum (hud.fov_match = 1) made the
                // waypoint track its world point perfectly and left everything
                // in that pass 1.8952x larger, because the widening we were
                // doing had been shrinking it. Position and size are separable
                // here: the 2026-09-05 scale ladder moved this coordinate space
                // across 3726 draws and changed the marker's SIZE while leaving
                // the head-tracking error untouched. So the frustum owns the
                // tracking and this owns the size. 1/1.8952 = 0.5277 is the
                // value that exactly cancels the magnification.
                // THE RECIPROCAL, and the sign of this was a shipped bug.
                //
                // The coordinate space is the size of the layer the authored
                // content is laid out in, so ENLARGING it makes that content a
                // smaller fraction of the layer -- "bigger number = smaller
                // HUD", as this file's own ini comment for rui.ll_scale has said
                // all along. The first ladder multiplied by the wanted size
                // directly, so 0.30 ENLARGED the marker by 3.3x and the rung
                // labelled smallest, 0.10, was the largest of the six. The
                // wearer stepped all six and reported every one huge, which is
                // exactly what that produces.
                //
                // The knob keeps the meaning a person expects -- 0.30 is thirty
                // per cent of full size -- and the inversion lives here.
                // THE SPACE SCALE SHRINKS ABOUT THE TOP-LEFT, AND THE TEXT SHOWS IT.
                //
                // Scaling the space is right for the TARGET, which the widget
                // re-projects from the world point into whatever space it is
                // given, so it lands correctly however the space is scaled --
                // the wearer confirmed it holds across all six rungs. It is
                // wrong for the parts that are NOT recomputed: the distance
                // text and the leader line are laid out at authored positions,
                // and enlarging the space by k moves an authored point to 1/k
                // of its coordinates. The space's origin is the TOP LEFT, so at
                // 0.40 (k = 2.5) the text lands 40% of the way in from the left
                // edge of where it belongs -- "WAAAY off to the left side of the
                // screen where it can't be read". rui_hook.asm's opening comment
                // records exactly this failure for the whole-layer version of
                // the same scale; it applies here too, to the authored parts.
                //
                // The engine's own per-edge INSET is a shrink about the CENTRE
                // instead: FC500's safe-area branch writes size*(1-2f) with
                // offset f, so f = (1 - wanted)/2 gives the same size with the
                // content centred rather than dragged into the corner. The
                // target should still land correctly because the widget projects
                // into the effective area it is handed -- and if it does not,
                // this run says so immediately by moving the target as well.
                // BACK TO THE SPACE SCALE, WHICH IS THE ONLY LIVE LEVER.
                //
                // I switched this to the engine's per-edge inset to get a shrink
                // about the CENTRE, and F5 then did nothing at all. The answer
                // was already written twenty lines above the A3Mode enum in this
                // same file: EffSizeScale (0x2DE0) and Inset (0x3AB0) are both
                // "measured dead for this widget" -- written with the right
                // values, went through, changed nothing -- and SpaceScale
                // (0x2DD0) is marked THE LEVER. I changed mechanism without
                // reading the note recording that the mechanism was dead.
                //
                // So: the space scale, by the reciprocal, which does control the
                // size and does hold the target's world position. What it does
                // NOT do is keep the authored text and leader line where they
                // belong, because the space grows from its origin at the top
                // left. That is the open item; it needs the position field
                // found, not another mechanism guessed.
                if (g_markerSize != 1.0f && g_markerSize > 0.0f) {
                    const float spaceScale = 1.0f / g_markerSize;
                    for (int i = 0; i < 4; ++i) mspace[i] *= spaceScale;
                }
                // ARM THE SLOT SWAP FOR THIS DRAW ONLY. Installed here, removed
                // by RuiMarkerRemoveSlotSwap the instant the widget returns, on
                // the single asm path that can reach either. Skipped entirely
                // when the fix is off, so arm 1 of the ladder touches nothing.
                // Install once, then just raise the gate. No engine memory is
                // written per draw, which is what the crash was.
                if (g_markerHeadFix != 0.0f && EnsurePrimEndInstalled(iface)) {
                    g_markerScratch = mscratch;
                    g_markerDrawActive = 1;
                }
                // THE MARKER'S PROJECTED SCREEN POSITION, found by the census on
                // 2026-09-05 and not guessed. In a TURNING window blk+0x50 and
                // blk+0x58 sweep 746.5261..1920 and 36.3158..1080 -- non-round
                // values clamping at the authored screen edges, while every
                // other moving lane holds round padding numbers (8/16/24/72).
                // In a STILL window they do not move at all. Position, in the
                // authored 1920x1080 space, already projected.
                //
                // THE CORRECTION. The scale ladder changed the marker's SIZE and
                // left the head error untouched, which rules the coordinate
                // space out as the position's source and leaves the frame delta:
                // the marker is projected in the engine camera's frame and shown
                // in the rendered view, and those differ by the head-yaw delta
                // this project already publishes every frame.
                //
                // So undo that rotation on the projected point. Done in ANGLE
                // space, not pixels: a pixel offset is only correct near the
                // centre, and the wearer's marker sits far off it.
                //
                //   ndc = px/(W/2) - 1        tan = ndc * halfTanX
                //   tan' = tan(atan(tan) - g*delta)
                //   px'  = (tan'/halfTanX + 1) * W/2
                //
                // `g` is the ladder arm: 0 is off, +1 and -1 are the two signs,
                // because which way the delta runs is not knowable in advance
                // and a one-sided ladder cannot tell a wrong sign from a wrong
                // mechanism. Yaw only -- the published pitch is absolute rather
                // than a delta, so correcting with it would be guessing.
                // The PRE-DRAW write that used to sit here is DELETED, not left
                // inert: it is measured dead (1118 adjusted draws, no movement)
                // because the widget overwrites block+0x50 during its own call.
                // Leaving it would mean two writes to one field, one of which is
                // known not to work, and a later reader could not tell which was
                // responsible for anything. The live write is in
                // RuiMarkerFixBlock, at the slot-+0x30 seam.
                ++g_markerAdjusted;
                // The size ladder stays available on the same key set, inert at 1.0.
                const float m = g_markerScale;
                if (m != 1.0f) {
                    for (int i = 0; i < 4; ++i) mspace[i] *= m;
                }
            }
        }
        return reinterpret_cast<void*>(original);
    }

    if (!g_a3Armed) return reinterpret_cast<void*>(original);

    // THE SCALE BELONGS TO THE TWO LOWER-LEFT HALVES AND TO NOTHING ELSE.
    //
    // Until the marker census existed, "substituted" and "is the lower-left
    // group" were the same set, so the transform below could key on membership
    // alone. Adding ANY other descriptor to g_subs silently widened it -- and a
    // world marker scaled by the ammo counter's slider is the exact shape of
    // this project's recurring "a predicate's reach grows with bookkeeping"
    // failure. It would also have been invisible today, because rui.ll_scale is
    // 1.0 and a scale of 1.0 is inert: the bug would have arrived the first
    // time the wearer dialled the corner size, in a run about something else.
    //
    // The gate is the RVA the substitution recorded, so the marker census is a
    // genuine pass-through no matter what the corner slider is set to.
    auto* scratch = *reinterpret_cast<std::uint8_t**>(
        reinterpret_cast<std::uint8_t*>(layer) + 0x18);
    if (!scratch) return reinterpret_cast<void*>(original);

    auto* space = reinterpret_cast<float*>(scratch + 0x2DD0);
    auto* effective = reinterpret_cast<float*>(scratch + 0x2DE0);
    auto* inset = reinterpret_cast<float*>(scratch + 0x3AB0);

    if (!g_sampleTaken) {
        g_sampleTaken = true;
        g_sampleBefore[0] = space[0];
        g_sampleBefore[1] = space[2];
        g_sampleBefore[2] = effective[0];
        g_sampleBefore[3] = inset[0];
    }

    // The LADDER drives while a stage is selected; otherwise the shipped ini
    // value does. One transform, two sources, so tuning and shipping cannot
    // drift apart.
    const bool onLadder = (g_stage >= 0);
    const float v = onLadder ? kStages[g_stage].value : g_llLive;
    const A3Mode mode = onLadder ? kStages[g_stage].mode : A3Mode::SpaceScale;
    switch (mode) {
        case A3Mode::Identity:
            break;
        case A3Mode::SpaceScale:
            for (int i = 0; i < 4; ++i) space[i] *= v;
            break;
        case A3Mode::EffSizeScale:
            for (int i = 0; i < 4; ++i) effective[i] *= v;
            break;
        case A3Mode::Inset:
            for (int i = 0; i < 4; ++i) inset[i] = v;
            break;
    }

    g_sampleAfter[0] = space[0];
    g_sampleAfter[1] = space[2];
    g_sampleAfter[2] = effective[0];
    g_sampleAfter[3] = inset[0];

    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_a3Calls));
    return reinterpret_cast<void*>(original);
}

namespace {

// Writes our address into the descriptors whose target is one of A2's two
// halves. A DATA write -- .data is already writable, but the protect call is
bool AlreadySubstituted(std::uintptr_t descriptor);
// kept because assuming a page's protection is how this project lost a run.
bool SubstituteOne(Identity& id, unsigned rva, std::uintptr_t uiBase) {
    if (id.target != uiBase + rva) return false;
    if (AlreadySubstituted(id.descriptor)) return false;
    if (g_subCount >= static_cast<int>(sizeof(g_subs) / sizeof(g_subs[0]))) {
        char full[240]{};
        std::snprintf(full, sizeof(full),
            "[TF2VR] A3: SUBSTITUTION TABLE FULL at %d -- ui(11)+0x%X was NOT substituted. Any "
            "census below is of a SUBSET and must not be read as complete.\n",
            g_subCount, rva);
        Tf2VrLog(full);
        return false;
    }
    auto* slot = reinterpret_cast<std::uintptr_t*>(id.descriptor + 0x68);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        Tf2VrLog("[TF2VR] A3: could not make a descriptor slot writable; nothing substituted.\n");
        return false;
    }
    g_subs[g_subCount].descriptor = id.descriptor;
    g_subs[g_subCount].original = *slot;
    g_subs[g_subCount].rva = rva;
    g_subs[g_subCount].typeBits = id.typeBits;
    ++g_subCount;
    *slot = reinterpret_cast<std::uintptr_t>(&ruiA3Interceptor);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    char line[320]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] A3: descriptor %s +0x68 now points at us; its original ui(11)+0x%X is saved.\n",
        id.descWhere, rva);
    Tf2VrLog(line);
    return true;
}

// Already ours? Substituting a descriptor twice would save OUR interceptor as
// the "original" and recurse on the next draw.
bool AlreadySubstituted(std::uintptr_t descriptor) {
    for (int i = 0; i < g_subCount; ++i) {
        if (g_subs[i].descriptor == descriptor) return true;
    }
    return false;
}

// NOT ONE-SHOT ANY MORE, AND THAT WAS A REAL BUG THAT COST A RUN.
//
// This used to open with `if (g_subCount) return;`. The two lower-left widgets
// draw from the moment a map loads, so the first call caught them and the guard
// then locked the function out forever. A WORLD MARKER only exists while there
// is something to point at, so its descriptor enters the census MINUTES later --
// and could never be substituted, no matter how many times this was called.
//
// The 2026-09-05 marker run is what that cost: seven ladder arms pressed by the
// wearer, every one a no-op, `draws adjusted: 0` on all of them and NO SAMPLES
// from the block census. The falsifier did its job and said the widget never
// drew through our slot; the guard above is why.
//
// So it now rescans every time it is called, substituting only what is missing.
// The cost is a walk of a handful of identities per census window.
// Defined here because it needs SubstituteOne and the table it owns. Called
// from RuiCensusSample on the render thread, at FC500's entry, which is the one
// moment early enough to catch a widget's FIRST draw.
//
// Scoped to the marker target on purpose. It is the only substituted widget
// that carries a SIZE correction, so it is the only one where arriving late is
// visible; the lower-left pair's scale is 1.0 and inert, and widening this
// would put a VirtualProtect on the render thread for widgets that gain
// nothing from it.
bool TrySubstituteIdentityNow(Identity& id) {
    if (!id.descriptor || !id.target) return false;
    if (AlreadySubstituted(id.descriptor)) return false;
    static std::uintptr_t uiBase = 0;
    if (!uiBase) {
        HMODULE m = GetModuleHandleA("ui(11).dll");
        if (!m) return false;
        uiBase = reinterpret_cast<std::uintptr_t>(m);
    }
    // THE HUD SIZE CONTROL widens this from one widget to every type-3 one.
    // Same reason as the marker: a widget substituted late draws unscaled until
    // the substitution lands, which is exactly the "comes up twice the size and
    // settles" the wearer reported, and doing it here means it never happens.
    unsigned rva = 0;
    if (g_markerTargetRva && id.target == uiBase + g_markerTargetRva) {
        rva = g_markerTargetRva;
    } else if (RuiHuntPromptWanted()) {
        // THE LADDER'S WIDE SUBSTITUTION, AND IT IS NOW GATED ON THE LADDER
        // ACTUALLY RUNNING. It used to key on RuiHuntVisible(), which was made
        // true from LOAD so the ladder's census would be complete before it
        // built its walk. That widened substitution to every live ui(11) target
        // for the whole session -- and substitution is what feeds the size
        // transforms, so it widened those too, for every run since.
        //
        // The wearer's HUD paid for it three times: the top health bar shrank
        // into the corner twice, and then the bottom bars and the background
        // graphics with it. The ladder identified its widget days ago and does
        // not need to be running now, so this reverts to the wearer's tuned set
        // the moment it is idle.
        // The search must not inherit the transform's type mask. This census
        // entry is shared by g_identities AND g_type0. Substitute every live
        // ui target for observation; AfterDraw still keeps transformation
        // eligibility separate. Module extent comes from the loaded PE.
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(uiBase);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(uiBase + dos->e_lfanew);
        if (id.target < uiBase || id.target - uiBase >= nt->OptionalHeader.SizeOfImage)
            return false;
        rva = static_cast<unsigned>(id.target - uiBase);
    } else if (g_hudSizeArm && (id.typeBits & static_cast<long>(g_hudSizeTypes)) != 0 &&
               id.target > uiBase && (id.target - uiBase) < 0x1000000) {
        rva = static_cast<unsigned>(id.target - uiBase);
    } else {
        return false;
    }
    if (InterlockedCompareExchange(&g_subLock, 1, 0) != 0) return false;
    const bool done = SubstituteOne(id, rva, uiBase);
    InterlockedExchange(&g_subLock, 0);
    if (done && rva != g_markerTargetRva) return true;
    if (done) {
        Tf2VrLog("[TF2VR] A3: marker widget substituted AT ITS FIRST DRAW, from the census hook at "
                 "FC500's entry, so it is never drawn unscaled. This is the fix for the wearer's "
                 "\"it comes up about twice the size and settles after a couple of seconds\": the "
                 "size correction lives behind this substitution, and it used to arrive on a 500 ms "
                 "retry seconds later. Needs rui.census = 1, which is what runs this hook.\n");
    }
    return done;
}

void InstallA3() {
    HMODULE m = GetModuleHandleA("ui(11).dll");
    if (!m) {
        Tf2VrLog("[TF2VR] A3: ui(11).dll not loaded; nothing substituted.\n");
        return;
    }
    const auto uiBase = reinterpret_cast<std::uintptr_t>(m);
    // The census is what knows which descriptor carries which target, so A3
    // composes with A1's instrument rather than re-deriving it. If the census
    // never ran, this finds nothing and says so instead of guessing.
    long taken = g_identityReserved;
    if (taken > kMaxIdentities) taken = kMaxIdentities;
    // The eager path writes this table from the RENDER thread, so this one
    // claims it first. A failed claim is not an error: this is a retry path and
    // the eager path is doing the same work.
    if (InterlockedCompareExchange(&g_subLock, 1, 0) != 0) {
        Tf2VrLog("[TF2VR] A3: the substitution table is being written by the census hook right now; "
                 "leaving it to that path and retrying next tick.\n");
        return;
    }
    for (long i = 0; i < taken; ++i) {
        if (!g_identities[i].descriptor) continue;
        SubstituteOne(g_identities[i], kLowerLeftLeftHalf, uiBase);
        SubstituteOne(g_identities[i], kLowerLeftRightHalf, uiBase);
        // The world-marker census, when one is selected. Same pass-through, and
        // it is never given a scale -- g_llLive only ever applies to the two
        // lower-left halves above.
        if (g_markerTargetRva) SubstituteOne(g_identities[i], g_markerTargetRva, uiBase);
        // Every type-3 widget, when the HUD size control is armed. The backstop
        // for the eager path in the census hook, which catches first draws.
        // TYPES 3 AND 4. Run 1 substituted only type 3 and every world-anchored
        // element was missing from the census. Type 4 is where STATE puts the
        // distance readouts and flyouts, which is the shape of an enemy or
        // friendly name label, so the class the classifier exists to find was
        // never sampled.
        if (g_hudSizeArm && (g_identities[i].typeBits & static_cast<long>(g_hudSizeTypes)) != 0) {
            const auto target = g_identities[i].target;
            if (target > uiBase && (target - uiBase) < 0x1000000) {
                SubstituteOne(g_identities[i], static_cast<unsigned>(target - uiBase), uiBase);
            }
        }
    }
    InterlockedExchange(&g_subLock, 0);
    if (!g_subCount) {
        Tf2VrLog("[TF2VR] A3: NO descriptor in the census carries ui(11)+0x1010 or +0x4320. "
                 "Either the census has not run yet (needs rui.census = 1 and a few seconds in "
                 "a map) or the mapping moved. Nothing substituted.\n");
    }
}

}  // namespace

void RemoveRuiA3() {
    for (int i = 0; i < g_subCount; ++i) {
        auto* slot = reinterpret_cast<std::uintptr_t*>(g_subs[i].descriptor + 0x68);
        // ONLY IF IT IS STILL OURS. These descriptors are runtime-populated
        // slots in engine.dll's .data, and a map change or RUI reload could
        // repoint one while we hold a saved original for the widget that used
        // to live there. Writing that back would install a stale function
        // pointer into a live descriptor -- a far worse failure than declining
        // to restore.
        if (*slot != reinterpret_cast<std::uintptr_t>(&ruiA3Interceptor)) {
            Tf2VrLog("[TF2VR] A3: a descriptor slot no longer holds our pointer, so it was "
                     "repointed under us. NOT restoring it -- writing the old value back would "
                     "install a stale function pointer.\n");
            continue;
        }
        DWORD old = 0;
        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
            *slot = g_subs[i].original;
            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(void*), old, &ignored);
        }
    }
    g_subCount = 0;
    g_a3Armed = 0;
    Tf2VrLog("[TF2VR] A3: descriptors restored to their own draw functions.\n");
}

void StepRuiA3() {
    if (!g_subCount) {
        InstallA3();
        if (!g_subCount) return;
    }
    if (g_stage >= 0) {
        char done[300]{};
        std::snprintf(done, sizeof(done),
            "[TF2VR] A3 stage %d ENDS: %llu draws transformed.%s\n",
            g_stage + 1, static_cast<unsigned long long>(g_a3Calls),
            g_a3Calls == 0 ? "  ZERO -- this arm never fired; it says NOTHING." : "");
        Tf2VrLog(done);
    }
    ++g_stage;
    if (g_stage >= kStageCount) {
        g_stage = -1;
        g_a3Armed = 0;
        Tf2VrLog("[TF2VR] ======== A3: LADDER DONE, transform OFF (the A/B arm) ========\n");
        std::snprintf(g_a3CaptureName, sizeof(g_a3CaptureName), "tf2vr-a3-off.bmp");
        g_a3Capture = 20;
        return;
    }
    g_a3Calls = 0;
    g_sampleTaken = false;
    g_a3Armed = 1;
    std::snprintf(g_a3CaptureName, sizeof(g_a3CaptureName), "tf2vr-a3-%02d.bmp", g_stage + 1);
    g_a3Capture = 20;
    char line[560]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] ======== A3 STAGE %d of %d: %s, value %.3f ========\n"
        "[TF2VR]   EXPECT: %s\n",
        g_stage + 1, kStageCount, kModeNames[static_cast<int>(kStages[g_stage].mode)],
        static_cast<double>(kStages[g_stage].value), kStages[g_stage].expect);
    Tf2VrLog(line);
}

void AdvanceRuiA3() {
    if (g_a3Capture > 0 && --g_a3Capture == 0) RequestNamedBackbufferCapture(g_a3CaptureName);
    if (g_stage < 0) return;
    static std::uint64_t last = 0;
    const std::uint64_t now = GetTickCount64();
    if (now - last < 2000) return;
    last = now;
    char line[480]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] A3 stage %d (%s): %llu draws transformed; engine gave space(%.1f,%.1f) "
        "eff(%.1f) inset(%.4f), we left space(%.1f,%.1f) eff(%.1f) inset(%.4f).%s\n",
        g_stage + 1, kModeNames[static_cast<int>(kStages[g_stage].mode)],
        static_cast<unsigned long long>(g_a3Calls),
        static_cast<double>(g_sampleBefore[0]), static_cast<double>(g_sampleBefore[1]),
        static_cast<double>(g_sampleBefore[2]), static_cast<double>(g_sampleBefore[3]),
        static_cast<double>(g_sampleAfter[0]), static_cast<double>(g_sampleAfter[1]),
        static_cast<double>(g_sampleAfter[2]), static_cast<double>(g_sampleAfter[3]),
        g_a3Calls == 0 ? "  ZERO DRAWS -- NOT REACHING THE SUBSTITUTED SLOT." : "");
    Tf2VrLog(line);
}

namespace { bool g_a2Selected = false; }
void SetRuiA2LadderSelected(bool selected) {
    g_a2Selected = selected;
    Tf2VrLog(selected ? "[TF2VR] F5 steps the A2 SUPPRESSION ladder.\n"
                      : "[TF2VR] F5 steps the A3 TRANSFORM ladder.\n");
}
bool RuiA2LadderSelected() { return g_a2Selected; }

// ---- the shipped lower-left control --------------------------------------
namespace {
float g_llScaleWanted = 1.0f;
bool g_llArmOnLoad = false;
}  // namespace

void SetRuiLowerLeftScale(float scale) {
    // Guarded because this multiplies a coordinate space: a zero would divide
    // the group's placement by nothing, and a negative would mirror it.
    if (scale < 0.2f) scale = 0.2f;
    if (scale > 5.0f) scale = 5.0f;
    g_llScaleWanted = scale;
    // Live, so LEADER then END re-dials it without a restart -- and only if it
    // is already armed, so setting it cannot arm it behind anyone's back.
    if (g_a3Armed && g_stage < 0) g_llLive = scale;
    char line[240]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] lower-left HUD: scale %.3f (the group is drawn %.0f%% of its normal size, "
        "shrunk about the screen centre).\n",
        static_cast<double>(scale), 100.0 / static_cast<double>(scale));
    Tf2VrLog(line);
}

float RuiLowerLeftScale() { return g_llScaleWanted; }

void SetRuiLowerLeftArmOnLoad(bool wanted) {
    g_llArmOnLoad = wanted;
    Tf2VrLog(wanted ? "[TF2VR] lower-left HUD: WANTED from the ini; it arms once, at rui.ll_scale, "
                      "as soon as a world is being rendered.\n"
                    : "[TF2VR] lower-left HUD: not wanted from the ini.\n");
}

bool RuiLowerLeftArmOnLoad() { return g_llArmOnLoad; }

void ArmRuiLowerLeft() {
    InstallA3();
    if (!g_subCount) {
        // InstallA3 finds its descriptors THROUGH THE CENSUS, so a shipped
        // feature would otherwise depend on a diagnostic being switched on --
        // and would fail silently in any profile that had it off. Arm the
        // census ourselves and let the caller retry; it is read-only.
        if (!RuiCensusArmed()) {
            Tf2VrLog("[TF2VR] lower-left HUD: no descriptors yet -- arming the identity census, "
                     "which is how they are found, and retrying.\n");
            ArmRuiCensus();
        }
        return;
    }
    g_llLive = g_llScaleWanted;
    g_a3Armed = 1;
    g_stage = -1;   // not on the ladder: this is the shipped value
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] lower-left HUD ARMED at scale %.3f on %d descriptor(s).\n",
        static_cast<double>(g_llScaleWanted), g_subCount);
    Tf2VrLog(line);
}

bool RuiLowerLeftArmed() { return g_a3Armed != 0 && g_stage < 0; }

// THE RECIPROCAL IS INVERTED EXACTLY ONCE, HERE.
//
// Bigger on screen means a SMALLER coordinate space, and that inversion is a
// trap every caller would otherwise have to remember: the six-key cluster says
// "up = bigger" and would have produced a smaller HUD. So the sign is resolved
// at the single point where a human intention ("bigger") meets the engine's
// units, and nothing above this level knows the lever is inverted.
void NudgeRuiLowerLeft(int steps, bool fine) {
    if (!steps) return;
    if (!RuiLowerLeftArmed()) ArmRuiLowerLeft();
    // Multiplicative, because this is a scale: a fixed additive step is coarse
    // at 1.0 and useless at 2.0.
    const float factor = fine ? 1.02f : 1.06f;
    float scale = g_llScaleWanted;
    for (int i = 0; i < (steps < 0 ? -steps : steps); ++i) {
        scale = steps > 0 ? scale / factor : scale * factor;
    }
    SetRuiLowerLeftScale(scale);
    g_llLive = g_llScaleWanted;
}

namespace { bool g_a3Selected = false; }
void SetRuiA3LadderSelected(bool selected) {
    g_a3Selected = selected;
    Tf2VrLog(selected ? "[TF2VR] F5 steps the A3 magnitude ladder.\n"
                      : "[TF2VR] F5 calibrates the lower-left HUD group.\n");
}
bool RuiA3LadderSelected() { return g_a3Selected; }

bool RuiCensusArmed() { return g_ruiCensusArmed != 0; }

// ---- the warp burst -------------------------------------------------------
namespace {
bool g_warpProbe = false;
int g_burstLeft = 0;
int g_burstGap = 0;
int g_burstIndex = 0;
}  // namespace

void SetRuiWarpProbe(bool enabled) {
    g_warpProbe = enabled;
    Tf2VrLog(enabled ? "[TF2VR] warp probe ON: F5 now starts a 12-frame full-frame burst instead "
                       "of calibrating. Turn smoothly and steadily while it runs.\n"
                     : "[TF2VR] warp probe off.\n");
}

bool RuiWarpProbeSelected() { return g_warpProbe; }

// THE WARP IS VR-ONLY -- MEASURED, by turning flat and seeing nothing. That
// rules out the mechanism I was about to chase: hudwarp_* is a game feature and
// would warp on a monitor too.
//
// And it is not our XR submission either, on the evidence already in hand: the
// handoff records the TOP-LEFT cluster as not warping while the lower-left
// does. A reprojection or pose-mismatch artefact moves the whole frame, so it
// cannot move one half of the HUD and leave the other still.
//
// What is left is that these two widgets read something view-dependent that
// only MOVES in VR. Flat, a mouse turn is yaw and nothing else. In a headset,
// rotating the body composes yaw with pitch and roll continuously, and head
// tracking writes all three. So the flat test that failed was the wrong axis,
// not a refutation -- yaw was the one axis a mouse turn could not distinguish
// from doing nothing.
//
// Three bursts, three axes, one key. Each keeps its own filenames so a later
// burst cannot overwrite the evidence from an earlier one -- which is exactly
// what the single-name version would have done on the second press.
namespace {
// FOUR AXES, AND THE FOURTH IS THE ONE VR ACTUALLY ADDS.
//
// Yaw came back null on the monitor. That is a real result but a narrow one: a
// mouse turn is pure yaw, and it is the only axis of the four that a monitor
// reproduces exactly as a headset does.
//
// The strongest remaining candidate is ALTERNATE-FRAME STEREO, because it is
// the one thing in this stack that is VR-only by construction: consecutive
// frames are rendered from DIFFERENT eye positions. Anything in the HUD whose
// placement depends on the camera -- rather than on pure screen space -- lands
// in a different spot on alternate frames, and a two-position alternation at
// half the frame rate is exactly what "a sine wave that only happens in VR"
// looks like. It also explains why the top-left cluster does not do it: that
// group is measured to take its placement from the layer inset, which has no
// camera term at all.
//
// And it can be tested on a MONITOR. Alternate-frame stereo is a camera
// manipulation, not an XR one -- DEL arms it with no headset attached -- so
// this costs a desk run rather than a headset run.
// THE QUESTION THIS ANSWERS, AND IT IS A BINARY.
//
// These captures come from the GAME'S BACKBUFFER -- the flat image the engine
// renders, before our XR layer submits it to the headset. So:
//
//   * if the lower-left group is RIGID across the frames while the wearer
//     turns, the game drew it correctly and the wobble is added AFTER
//     rendering, by our presentation path;
//   * if it MOVES within the backbuffer, the wobble is in the game's own
//     rendering, responding to something we write into the camera.
//
// One of those is a compositor problem and one is a HUD problem, and they have
// nothing in common. Seven suspects were eliminated one at a time without ever
// asking which half of the pipeline to look in -- that was the wrong order and
// this fixes it.
//
// The wearer also reports the HUD ROTATING WITH THEIR HEAD, and the same
// captures settle that too: a screen-space HUD baked into the image is rigid in
// the backbuffer BY CONSTRUCTION, so if head roll moves it here, it is not
// screen-space at all and everything assumed about it is wrong.
//
// EIGHT frames, not twelve: in VR the frame is 4032x2268, so each capture is
// 36 MB rather than 14. Four bursts of eight is about 1.2 GB, which is the most
// this is worth spending on one run.
const char* const kBurstNames[] = {"STICKTURN", "HEADROLL", "HEADPITCH", "STILL"};
const char* const kBurstAsk[] = {
    "TURN WITH THE STICK, the way you normally do, smoothly, about a quarter "
    "turn. This is the symptom's own precondition -- it is the one that must "
    "reproduce.",
    "ROLL YOUR HEAD side to side, ear toward shoulder, twice. This is the "
    "rotation you just reported the HUD following.",
    "LOOK UP AND DOWN through a big arc, twice, without turning.",
    "STAND COMPLETELY STILL and hold your head steady. The control: these eight "
    "frames should be identical in the HUD. If they are not, the instrument is "
    "measuring something other than what it claims.",
};
int g_burstRun = 0;
}  // namespace

void StartRuiWarpBurst() {
    g_burstIndex = 0;
    g_burstLeft = 8;
    g_burstGap = 0;
    const int which = g_burstRun % 4;
    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] ======== WARP BURST %d: %s -- 8 frames, about a second and a half ========\n"
        "[TF2VR]   %s\n",
        g_burstRun + 1, kBurstNames[which], kBurstAsk[which]);
    Tf2VrLog(line);
    ++g_burstRun;
}

// SCREENSHOTS, AUTOMATICALLY. The wearer asked whether I had grabbed any
// and I had not -- an eight-run investigation into something VISUAL with no
// picture of it since the backbuffer bursts. This fires one burst per
// session, unprompted, so there is always an image to look at.
void AdvanceRuiAutoShots() {
    if (!RuiLowerLeftArmed()) return;
    static int fired = 0;
    if (fired) return;
    static std::uint64_t firstSeen = 0;
    const std::uint64_t now = GetTickCount64();
    if (!firstSeen) { firstSeen = now; return; }
    // Twenty seconds in: long enough to be in a map and moving, early
    // enough that a short session still produces the pictures.
    if (now - firstSeen < 20000) return;
    fired = 1;
    g_burstRun = 0;
    StartRuiWarpBurst();
}

void AdvanceRuiWarpBurst() {
    if (g_burstLeft <= 0) return;
    if (g_burstGap > 0) { --g_burstGap; return; }
    // Roughly every ninth frame: fast enough to catch a swing, slow enough that
    // twelve frames span a real turn rather than a twitch.
    g_burstGap = 9;
    const int which = (g_burstRun - 1) % 4;
    char name[80]{};
    std::snprintf(name, sizeof(name), "tf2vr-warp-%d-%s-%02d.bmp", g_burstRun, kBurstNames[which],
                  g_burstIndex++);
    RequestNamedBackbufferCapture(name);
    if (--g_burstLeft == 0) {
        char done[200]{};
        std::snprintf(done, sizeof(done),
            "[TF2VR] warp burst %d (%s) complete: 8 frames written.\n", g_burstRun,
            kBurstNames[which]);
        Tf2VrLog(done);
    }
}

// ---- THE SINE-WAVE WARP: A SUSPECT LADDER, RUN IN VR ----------------------
//
// Chased flat for two runs and that was a mistake. The symptom is VR-only by
// measurement -- the wearer turned on a monitor and saw nothing, and has never
// seen it there -- so the flat harness cannot reproduce it BY DEFINITION.
// Testing where the symptom cannot appear produces nulls that mean nothing,
// which is the same trap as an instrument that cannot see what it is aimed at.
//
// So this runs in the headset, where it is known to reproduce, and it changes
// ONE THING PER STAGE against a baseline that is otherwise exactly what the
// wearer already experiences.
//
// STAGE 1 IS A POSITIVE CONTROL AND IS NOT OPTIONAL. Everything is left as it
// normally is and the wobble must be VISIBLE. If it is not, the session says
// nothing about any of the suspects that follow -- a symptom that failed to
// show and a suspect that was innocent look identical from the chair.
//
// WHAT IS ALREADY ELIMINATED, and how far:
//   - the game's own HUD warp (hudwarp_*): the symptom is VR-only, and hudwarp
//     is a game feature that would warp a monitor too;
//   - XR reprojection / head-pose mismatch: that displaces the whole submitted
//     frame, and the TOP-LEFT cluster provably does not warp while the
//     lower-left does. Both are drawn into the same backbuffer;
//   - a camera-POSITION term in the HUD: tested flat with alternate-frame
//     stereo armed and the half-IPD exaggerated 15x, standing still. No
//     alternation. That kills the position term. It does NOT kill stereo as a
//     participant in something that also needs rotation, which is why stereo
//     is still on this ladder.
//
// Each stage restores the one before it, so exactly one mechanism differs from
// baseline at a time -- except the last, which turns them all off together to
// catch an interaction that no single stage would show.

namespace {

enum class Suspect { Baseline, Sway, AimWrite, Stereo, OurTransform, AllOff, Restore };

struct SuspectStage {
    Suspect what;
    const char* name;
    const char* why;
};

const SuspectStage kSuspects[] = {
    {Suspect::Baseline, "BASELINE -- the positive control",
     "Nothing changed. TURN and confirm the wobble IS there. If it is not, stop: "
     "nothing after this would mean anything."},
    {Suspect::Sway, "weapon sway suppression OFF",
     "The strongest suspect. This is armed with head tracking in VR and absent "
     "flat, and the group we are chasing is content\\r2\\ui\\hud\\weapon_status.rui "
     "-- the WEAPON status. If the HUD reads the weapon's sway while we suppress "
     "that sway on the render side, the two disagree, and they disagree most "
     "while turning."},
    {Suspect::AimWrite, "the aim write OFF",
     "aim.cmd writes view angles into CreateMove every command tick. The frame "
     "rate is 84 Hz and the tick rate is not, so anything reading those angles "
     "per frame sees a staircase."},
    {Suspect::Stereo, "alternate-frame stereo OFF",
     "Not eliminated -- only its standing-still form was. Consecutive frames "
     "render from different eye positions, and that could still matter once "
     "rotation is in the mix."},
    {Suspect::OurTransform, "our own lower-left transform OFF",
     "Almost certainly innocent: the warp was reported long before this existed. "
     "Cheap to exclude, and 'almost certainly' is not a measurement."},
    {Suspect::AllOff, "ALL FOUR OFF at once",
     "If no single stage stopped it, this says whether the cause is here at all. "
     "Still wobbling with all four off means it is none of them."},
    {Suspect::Restore, "everything restored",
     "Back to normal. Nothing is left armed or disarmed behind you."},
};
constexpr int kSuspectCount = static_cast<int>(sizeof(kSuspects) / sizeof(kSuspects[0]));

int g_suspectStage = -1;
bool g_suspectActive = false;
// What baseline looked like, captured once, so the restore is to what WAS
// rather than to what the defaults claim.
bool g_baseSway = false;
bool g_baseStereo = false;
bool g_baseLowerLeft = false;
bool g_baseCaptured = false;

void CaptureBaseline() {
    if (g_baseCaptured) return;
    g_baseSway = IsWeaponSwaySuppressed();
    g_baseStereo = IsAlternateFrameStereoArmed();
    g_baseLowerLeft = RuiLowerLeftArmed();
    g_baseCaptured = true;
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] suspect ladder: baseline recorded -- sway suppressed %d, stereo %d, "
        "lower-left transform %d.\n",
        g_baseSway ? 1 : 0, g_baseStereo ? 1 : 0, g_baseLowerLeft ? 1 : 0);
    Tf2VrLog(line);
}

// Puts every suspect back to baseline, then applies just the one this stage is
// about. One variable at a time is the whole point of the ladder.
void ApplyBaselineState() {
    if (IsWeaponSwaySuppressed() != g_baseSway) SetWeaponSwaySuppressed(g_baseSway);
    if (IsAlternateFrameStereoArmed() != g_baseStereo) ToggleVerifiedGameAlternateFrameStereo();
    SetAimCmdState(static_cast<int>(AimCmdState::WriteAttack));
    if (g_baseLowerLeft && !RuiLowerLeftArmed()) ArmRuiLowerLeft();
    if (!g_baseLowerLeft && RuiLowerLeftArmed()) g_a3Armed = 0;
}

}  // namespace

void StepRuiSuspectLadder() {
    CaptureBaseline();
    ++g_suspectStage;
    if (g_suspectStage >= kSuspectCount) g_suspectStage = kSuspectCount - 1;

    ApplyBaselineState();
    switch (kSuspects[g_suspectStage].what) {
        case Suspect::Baseline:
        case Suspect::Restore:
            break;
        case Suspect::Sway:
            SetWeaponSwaySuppressed(false);
            break;
        case Suspect::AimWrite:
            SetAimCmdState(static_cast<int>(AimCmdState::Off));
            break;
        case Suspect::Stereo:
            if (IsAlternateFrameStereoArmed()) ToggleVerifiedGameAlternateFrameStereo();
            break;
        case Suspect::OurTransform:
            g_a3Armed = 0;
            break;
        case Suspect::AllOff:
            SetWeaponSwaySuppressed(false);
            SetAimCmdState(static_cast<int>(AimCmdState::Off));
            if (IsAlternateFrameStereoArmed()) ToggleVerifiedGameAlternateFrameStereo();
            g_a3Armed = 0;
            break;
    }

    char line[620]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] ======== SUSPECT %d of %d: %s ========\n"
        "[TF2VR]   %s\n"
        "[TF2VR]   TURN NOW, with the stick, the way you normally do. Does the lower-left "
        "group still ride up and down?\n",
        g_suspectStage + 1, kSuspectCount, kSuspects[g_suspectStage].name,
        kSuspects[g_suspectStage].why);
    Tf2VrLog(line);
    g_suspectActive = true;
}

void AdvanceRuiSuspectLadder() {
    if (!g_suspectActive) return;
    static std::uint64_t last = 0;
    const std::uint64_t now = GetTickCount64();
    if (now - last < 3000) return;
    last = now;
    // The falsifier: what is ACTUALLY armed right now, not what the stage
    // intended. A stage that failed to change anything and a suspect that was
    // innocent are the same thing from the chair, and only this line separates
    // them afterwards.
    char line[380]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] suspect %d/%d live state: sway suppressed %d, stereo %d, aim write %d, "
        "lower-left transform %d.\n",
        g_suspectStage + 1, kSuspectCount, IsWeaponSwaySuppressed() ? 1 : 0,
        IsAlternateFrameStereoArmed() ? 1 : 0, static_cast<int>(CurrentAimCmdState()), RuiLowerLeftArmed() ? 1 : 0);
    Tf2VrLog(line);
}

namespace { bool g_suspectSelected = false; }
void SetRuiSuspectLadder(bool selected) {
    g_suspectSelected = selected;
    Tf2VrLog(selected ? "[TF2VR] F5 steps the VR SUSPECT LADDER for the sine-wave warp.\n"
                      : "[TF2VR] F5 does not step the suspect ladder.\n");
}
bool RuiSuspectLadderSelected() { return g_suspectSelected; }

// ---- THE PARAMETER-BLOCK CENSUS: which field carries the wobble? ----------
//
// MEASURED, not assumed: the wobble is in the GAME'S BACKBUFFER. Across eight
// captured frames the group is rigid standing still, and moves +43 px right and
// -102 px up during a stick turn, accelerating as the turn continues. Our XR
// layer never touches it -- the placement is already wrong when the frame is
// handed over.
//
// That kills every compositor explanation and it kills the four suspects the
// ladder walked, all of which wobbled. What is left is the widget's own
// placement, and the widget writes that placement into the parameter block at
// scratch+0x2DD0 -- the block it asks for with primitive slot +0x18
// (`mov rax,[rcx+0x18]; add rax,0x2DD0; ret`) and fills from its own .rdata
// constants and from RUI data bindings.
//
// So: record the block over a turn and see which floats move. A field that
// tracks the view is the wobble; everything else is constant or steps with the
// ammo count.
//
// WHY THE DRIFT ACCELERATES, and why a mouse could never show it: a stick turn
// holds a CONSTANT angular velocity for a second or more, so a placement term
// that lags the view reaches a large steady offset and reverses when the turn
// reverses -- which is a sine wave. A mouse turn is a flick: the lag settles
// before it is visible. That is the mouse-versus-stick difference the wearer
// spotted, and it predicts the symptom is really SUSTAINED-TURN-specific
// rather than VR-specific.
//
// BOUNDED BY CONSTRUCTION: a fixed 64-sample ring of 32 floats, one sample
// every eighth draw of ONE target. It cannot grow, and it reports RANGES rather
// than 64 lines of raw floats -- the question is which field moves, not what
// its values were.

namespace {

// 72 floats = 0x2DD0 .. 0x2EEF.
//
// THE FIRST VERSION SAMPLED 32 AND MEASURED NOTHING, because 32 floats reach
// only 0x2E4F -- and the disassembly of ui(11)+0x4320, which I had already
// done, shows the widget writing CONSTANTS into block+0x30..+0x90 and its
// BINDING-DERIVED values into block+0xA0..+0xF0, i.e. 0x2E70..0x2EC0. So the
// window covered exactly the half that cannot move, and returned seven windows
// of zeroes that proved the constants are constant.
//
// 72 covers every write the disassembly names, with one vector of margin.
constexpr int kBlockFloats = 72;
constexpr int kBlockSamples = 64;

float g_blockMin[kBlockFloats];
float g_blockMax[kBlockFloats];
bool g_blockSeen = false;
int g_blockSamples = 0;
int g_blockSkip = 0;
bool g_blockArmed = false;
bool g_blockSelected = false;
int g_blockWindow = 0;
std::uint64_t g_blockWindowStart = 0;
// Which of the two halves to watch. 0x4320 is the weapon-and-ammo half, the
// one whose text was already measured misbehaving, and it draws once a frame
// rather than three times -- so its samples are one per frame rather than a
// mixture of three different widgets.
unsigned g_blockTargetRva = 0x4320;

// ---- THE WORLD-MARKER CENSUS ---------------------------------------------
//
// `rui.marker_target`. A ui(11) RVA to substitute and watch IN ADDITION to the
// lower-left pair, so the block census can be pointed at a WORLD-ANCHORED
// widget instead of a screen-anchored one. Read-only: the substitution is the
// same pass-through the shipped lower-left control installs, and no scale or
// offset is applied to it.
//
// The candidates, all confirmed drawing in the H3 run and all named from the
// asset strings each function references (docs/HUD-NOMENCLATURE-2026-09-05.md):
//
//   0x809B0  #HUD_DISTANCE_METERS / KILOMETERS + weapon-flyout borders
//            -- the distance-labelled world marker. THE WAYPOINT. Start here.
//   0xBF60   the same family on layer type 4
//   0x69570  overhead_icon_titan_arrow_you
//   0xC4D0   battery capture friendly/enemy
//   0x5B800  lockon_indicator centre + edge -- the Titan missile lock
//
// WHY THIS IS THE RIGHT INSTRUMENT FOR THE REPORTED SYMPTOM. The wearer sees a
// distant marker move WITH the head but at the wrong RATE, so the question is
// arithmetic: what did the widget receive, and what did the view do in the same
// instant? Sampling the parameter block alone cannot answer it -- that was the
// mistake the 2026-08-20 block census made, sampling 72 fields and reporting
// "constant" without ever recording what the VIEW was doing. So this prints the
// head-view delta and the rendered half-tangents ON THE SAME LINE as the block,
// and both hypotheses become predictions rather than arguments:
//
//   uniform frame delta -> the marker's error tracks yawDelta, and no lane in
//                          the block moves with it;
//   frustum-scale error -> the error is proportional to the marker's own
//                          offset from view centre, so a lane in the block DOES
//                          move, and its range scales with the half-tangents.
// (the variable itself is declared beside g_subCount, because InstallA3 needs it first)

}  // namespace

void ToggleHudWidgetScaleAB() {
    const float now = HudWidgetScale();
    const float next = (now > 0.99f) ? 0.55f : 1.0f;
    SetHudWidgetScale(next);
    char line[380]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F12: HUD size %.2f -> %.2f (%s). Take one capture on each side with F3; the two "
        "images are what finally says WHERE the jump/slide bars go, instead of me deducing it.\n",
        now, next, next > 0.99f ? "UNSCALED, the game's own size" : "scaled, the wearer's 0.55");
    Tf2VrLog(line);
}



void SetRuiHudSizeArm(bool armed) {
    g_hudSizeArm = armed;
    Tf2VrLog(armed
        ? "[TF2VR] hud.widget_arm = 1: every type-3 widget the census names is substituted as a "
          "pass-through. Substitution happens at each widget's FIRST draw from the census hook, so "
          "nothing comes up unscaled and settles. This alone changes NOTHING on screen -- it only "
          "makes the widgets reachable.\n"
        : "[TF2VR] hud.widget_arm = 0: only the marker widget is substituted.\n");
}

void SetRuiMarkerTarget(unsigned rva) {
    g_markerTargetRva = rva;
    if (!rva) {
        Tf2VrLog("[TF2VR] rui.marker_target = 0: no world-marker widget is watched.\n");
        return;
    }
    // The block census follows the marker automatically. Pointing one at a
    // widget and leaving the other on the lower-left pair is the kind of split
    // that produces a confident census of the wrong thing.
    g_blockTargetRva = rva;
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] rui.marker_target = ui(11)+0x%X: this widget is substituted (pass-through, "
        "nothing scaled) and the block census now watches IT rather than the lower-left pair.\n",
        rva);
    Tf2VrLog(line);
}

unsigned RuiMarkerTarget() { return g_markerTargetRva; }

extern "C" void RuiA3AfterDraw(void* layer) {
    // FIRST, AND UNCONDITIONALLY: close the rung-B bracket. Every early return
    // below would otherwise leave it raised, and a flag stuck on turns the
    // targeted census straight back into the untargeted one that just failed.
    g_ruiInDraw = 0;
    ++g_controlCalls;
    if (!layer) { ++g_controlNull; return; }
    // THE MARKER WIDGET'S VM INSTRUMENTS (rui_asset_dump.h), before the block
    // census gate: that census arms in six-second windows and is down between
    // them, and a lever gated on somebody else's window is the "fix parked
    // behind a flag the run's config leaves off" failure for a fourth time.
    // Keyed on the RVA the substitution recorded, so it is the one marker
    // widget and never the lower-left pair.
    {
        const auto d = *reinterpret_cast<std::uintptr_t*>(layer);
        bool known = false;
        for (int i = 0; i < g_subCount; ++i) {
            if (g_subs[i].descriptor != d) continue;
            known = true;
            ++g_controlKnown;
            // Resolve identity from the saved original, never the overwritten
            // descriptor slot. The original widget has completed; tell FC500
            // to decline its submission using its own checked/reset flag.
            const bool huntSuppress = RuiHuntObserve(0, g_subs[i].original);
            if (RuiDeclineCompletedLayer(layer, g_subs[i].original,
                                        huntSuppress ? g_subs[i].original : g_ruiSuppressTarget)) {
                InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_ruiSuppressed));
                return;
            }
            auto* sc = *reinterpret_cast<std::uint8_t**>(
                reinterpret_cast<std::uint8_t*>(layer) + 0x18);
            if (g_markerTargetRva && g_subs[i].rva == g_markerTargetRva) {
                RuiMarkerAfterDrawInstrument(layer, sc);
            }
            // THE CLASSIFIER sees EVERY substituted widget including the
            // marker, because the marker is its positive control.
            RuiWidgetClassify(g_subs[i].rva, sc);
            // WHAT IT SAYS, once per widget per session, read-only. The wearer
            // named the friendly label by its text ("Lt Shaver") and the
            // scratch carries a string arena, so the widget can be asked
            // directly instead of guessed at from geometry -- which is how the
            // last three identifications on this front went wrong.
            RuiWidgetStrings(g_subs[i].rva, sc);
            RuiLabelDump(g_subs[i].rva, sc);
            // THE PER-LAYER PROBE on the widget named by disappearance
            // 2026-09-07. It draws both name labels, so its own enemy layers
            // are the control for its friendly ones -- same draw, same frame.
            RuiLayerProbe(g_subs[i].rva, sc, 0);
            // THE FIX, on the game's numbers and before any size transform of
            // ours: a layer of the label widget that is measurably not moving
            // when the head turns has the head rotation put back into it.
            // ONLY TYPES 3, 4 AND 7 ARE EVER TRANSFORMED. Everything else is
            // substituted so it can be CLASSIFIED and READ and nothing more:
            // type 0 is the frontend and the menus, and type 6 is the crosshair
            // family, which the nomenclature marks explicitly out of scope.
            // Reaching a widget and transforming it stay separate things.
            if ((g_subs[i].typeBits & ((1L << 3) | (1L << 4) | (1L << 7))) == 0) {
                // ONE exception, and it is a correction rather than a
                // transform: a world-anchored marker on the pixel-ortho pass
                // has hud2d.zoom cancelled for it alone, so it lands on its
                // target while the reticle and the menus keep their shrink.
                // The crosshair shares this layer, is not world-anchored, and
                // therefore cannot be touched by it.
                if ((g_subs[i].typeBits & (1L << 6)) != 0) {
                    RuiUnzoomWorldMarkers(g_subs[i].rva, sc);
                }
                NoteWidgetPath(g_subs[i].rva, 5);
                break;
            }
            // hud.canvas_centre, for EVERY substituted draw regardless of type:
            // the rule keys on the widget's own registers (a full-space element),
            // not on the draw-context type, so the fade and the loading overlay
            // are caught whatever type byte they arrive under.
            // THE CENSUS, BEFORE EVERY TYPE GATE. A survey that inherits the
            // gate of the thing it surveys cannot find what the gate excludes.
            RuiNoteWidgetDraw(g_subs[i].rva, g_subs[i].typeBits, sc);
            RuiCanvasCentre(g_subs[i].rva, sc);
            if (g_markerTargetRva && g_subs[i].rva == g_markerTargetRva) {
                /* the marker instrument above already ran */
            } else if (g_hudSizeArm && (g_subs[i].typeBits & (1L << 7)) != 0) {
                // THE NAME LABELS. World-anchored, so they take the size-only
                // transform that scales about their own centre and never the
                // screen-centre one, which would walk them off the entity.
                RuiNameplateScale(g_subs[i].rva, sc);
            } else if (g_hudSizeArm &&
                       (g_subs[i].typeBits & static_cast<long>(g_hudSizeTypes)) != 0) {
                // THE HUD SIZE CONTROL. The marker widget is deliberately NOT
                // in here: its target is re-projected from a world point, so
                // scaling its space about the screen centre would walk the
                // target off the thing it is pointing at. It keeps its own
                // hud.marker_size, which is size-only by measurement.
                //
                // THE TYPE MASK IS BACK ON THIS BRANCH, AND IT IS A REGRESSION
                // FIX. 2026-09-07: the identification ladder needs the census to
                // see every live ui(11) target, so RuiHuntVisible() widens
                // SUBSTITUTION past hud.widget_types. This branch had no type
                // check of its own -- it did not need one while substitution
                // was itself type-gated -- so widening the census silently
                // widened the TRANSFORM too, and types 1/5/6/10 that the wearer
                // had never opted in started being scaled about the screen
                // centre. The wearer saw the top health bar shrink and move,
                // and reported it as the same regression the lock-ring work
                // had already fixed once.
                //
                // The substitution site says transformation eligibility stays
                // separate. This is what makes that true.
                RuiWidgetUniformScale(g_subs[i].rva, sc);
            }
            // THE SAME MEASUREMENT AGAIN, NOW THAT WE HAVE HAD OUR TURN. Every
            // head-response figure this project has produced was sampled above,
            // before these transforms, so OUR contribution has never appeared in
            // any of them. The dispatch says type 7 takes the size-only path and
            // nothing moves -- and this front has twice been wrong reading code.
            // Sampling both sides makes it a measurement instead of a claim.
            RuiLayerProbe(g_subs[i].rva, sc, 1);
            break;
        }
        if (!known) ++g_controlUnknown;
    }
    if (!g_blockArmed) return;
    const auto descriptor = *reinterpret_cast<std::uintptr_t*>(layer);
    if (!descriptor) return;
    // Only the one target, so the samples describe a single widget.
    std::uintptr_t uiBase = 0;
    if (HMODULE m = GetModuleHandleA("ui(11).dll")) uiBase = reinterpret_cast<std::uintptr_t>(m);
    if (!uiBase) return;
    const auto target = *reinterpret_cast<const std::uintptr_t*>(
        reinterpret_cast<const std::uint8_t*>(descriptor) + 0x68);
    // The slot now holds OUR interceptor, so compare against the saved original.
    std::uintptr_t original = 0;
    for (int i = 0; i < g_subCount; ++i) {
        if (g_subs[i].descriptor == descriptor) { original = g_subs[i].original; break; }
    }
    (void)target;
    if (original != uiBase + g_blockTargetRva) return;
    // THE LANE CENSUS (world_marker.h): every draw of this widget, so the
    // engine's own projection can be solved instead of guessed. Read-only.
    if (auto* sc = *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(layer) + 0x18)) {
        WorldMarkerRecordWidgetDraw(reinterpret_cast<const float*>(sc + 0x2DD0));
    }
    if (++g_blockSkip < 8) return;
    g_blockSkip = 0;
    if (g_blockSamples >= kBlockSamples) return;

    auto* scratch = *reinterpret_cast<std::uint8_t**>(
        reinterpret_cast<std::uint8_t*>(layer) + 0x18);
    if (!scratch) return;
    const auto* block = reinterpret_cast<const float*>(scratch + 0x2DD0);
    for (int i = 0; i < kBlockFloats; ++i) {
        const float v = block[i];
        if (!g_blockSeen) { g_blockMin[i] = v; g_blockMax[i] = v; }
        else {
            if (v < g_blockMin[i]) g_blockMin[i] = v;
            if (v > g_blockMax[i]) g_blockMax[i] = v;
        }
    }
    g_blockSeen = true;
    ++g_blockSamples;
}

void ReportRuiBlockCensus() {
    if (!g_blockSeen) {
        Tf2VrLog("[TF2VR] block census: NO SAMPLES. The target was never drawn through our slot, "
                 "so nothing below would mean anything.\n");
        return;
    }
    int movers = 0;
    for (int i = 0; i < kBlockFloats; ++i) {
        if ((g_blockMax[i] - g_blockMin[i]) > 0.001f) ++movers;
    }
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] ======== BLOCK CENSUS window %d: ui(11)+0x%X, %d samples, %d of %d fields "
        "moved ========\n",
        g_blockWindow, g_blockTargetRva, g_blockSamples, movers, kBlockFloats);
    Tf2VrLog(line);

    // THE VIEW, ON THE SAME LINE AS THE BLOCK. The 2026-08-20 census sampled 72
    // fields, reported "constant", and was correct and useless -- because it
    // never recorded what the VIEW was doing while it sampled. A world marker
    // that misplaces itself does so RELATIVE to the view, so a block reading
    // without the view beside it cannot distinguish the two live hypotheses.
    //
    // yawDelta is how far the rendered view has turned from the engine camera's
    // own frame; the half-tangents are the frustum the image is presented
    // through. Both are already published every frame by the camera work, so
    // this costs a read rather than a new measurement.
    float yawDelta = 0.0f, headPitch = 0.0f;
    const bool haveHead = GetHeadViewDelta(&yawDelta, &headPitch);
    float passW = 0.0f, passH = 0.0f, halfTanX = 0.0f, halfTanY = 0.0f;
    int hadAim = 0;
    unsigned long long anchorUpdates = 0;
    ReadReticleAnchorDetail(&passW, &passH, &halfTanX, &halfTanY, &hadAim, &anchorUpdates);
    char view[400]{};
    std::snprintf(view, sizeof(view),
        "[TF2VR]   VIEW AT THIS WINDOW: head yaw delta %s%.3f deg, head pitch %.3f deg | "
        "rendered half-tangents %.5f x %.5f | hud2d pass %.0fx%.0f. A marker that is off by a "
        "CONSTANT frame delta tracks the first number; one off by a FRUSTUM SCALE tracks its own "
        "distance from view centre instead.\n",
        haveHead ? "" : "(STALE) ", static_cast<double>(yawDelta),
        static_cast<double>(headPitch), static_cast<double>(halfTanX),
        static_cast<double>(halfTanY), static_cast<double>(passW), static_cast<double>(passH));
    Tf2VrLog(view);
    if (!haveHead) {
        Tf2VrLog("[TF2VR]   HEAD DELTA NOT PUBLISHED -- the camera hook has not run this session, "
                 "so the first number above is a default and this window cannot separate the two "
                 "hypotheses. Fix that before reading anything below.\n");
    }
    // THE FIRST WINDOW PRINTS EVERYTHING, so the block's full layout is on the
    // record once and a later reader can see what the static fields hold rather
    // than having to trust that they were looked at. After that only the movers
    // print, with the static count stated -- 72 fields across a dozen windows is
    // noise, and noise is how a signal gets missed.
    const bool full = (g_blockWindow <= 1);
    for (int i = 0; i < kBlockFloats; ++i) {
        const float range = g_blockMax[i] - g_blockMin[i];
        const bool moved = range > 0.001f;
        if (!full && !moved) continue;
        std::snprintf(line, sizeof(line),
            "[TF2VR]   +0x%04X (blk+0x%02X)  %12.4f %12.4f  range %10.4f%s\n",
            0x2DD0 + i * 4, i * 4, static_cast<double>(g_blockMin[i]),
            static_cast<double>(g_blockMax[i]), static_cast<double>(range),
            moved ? "   <-- MOVES" : "");
        Tf2VrLog(line);
    }
    if (!full && movers == 0) {
        Tf2VrLog("[TF2VR]   nothing moved in this window -- every field held its value.\n");
    }
    Tf2VrLog("[TF2VR] ============================================================\n");
}

// NO KEYPRESS. The previous run produced nothing because F5 never registered,
// and a headset run that depends on a press the wearer cannot verify landed is
// a run designed to be wasted. This arms itself and reports on a rolling clock,
// so the wearer only has to play and turn.
//
// ROLLING WINDOWS rather than one shot: min/max accumulate for six seconds,
// print, reset, repeat. Whatever the wearer happened to be doing in each window
// is captured, so the windows that coincide with a sustained turn show the
// movement and the still ones are their own control. One-shot capture would
// have to guess when the turning starts.
void StartRuiBlockCensus() {
    // UNCONDITIONAL, and this is the SECOND HALF of the 2026-09-05 bug. Making
    // InstallA3 rescan achieved nothing while its only live caller still asked
    // for it exclusively when NOTHING was substituted: the lower-left pair is
    // caught in the first seconds of a map, so `if (!g_subCount)` locked the
    // rescan out just as effectively as the guard inside InstallA3 did. The fix
    // and the thing that calls the fix have to agree, or the fix is dead code.
    //
    // It is cheap and idempotent -- it walks a handful of census identities and
    // substitutes only what is missing -- so there is nothing to gate it on.
    InstallA3();
    {
        if (!g_subCount) {
            Tf2VrLog("[TF2VR] block census: the descriptors are not substituted, so the widget "
                     "never calls us and there is nothing to read.\n");
            return;
        }
    }
    g_blockSeen = false;
    g_blockSamples = 0;
    g_blockSkip = 0;
    g_blockArmed = true;
    ++g_blockWindow;
    if (g_blockWindow == 1) {
        Tf2VrLog("[TF2VR] ======== BLOCK CENSUS ARMED (rolling, no keypress) ========\n"
                 "[TF2VR]   Six-second windows. Just play: TURN back and forth with the stick "
                 "sometimes, and stand still sometimes. The still windows are the control.\n");
    }
}

void AdvanceRuiBlockCensus() {
    if (!g_blockSelected) return;
    // Arms itself once the substitution exists, and re-arms each window.
    if (!g_blockArmed) {
        static std::uint64_t lastTry = 0;
        const std::uint64_t tick = GetTickCount64();
        if (tick - lastTry < 500) return;
        lastTry = tick;
        StartRuiBlockCensus();
        g_blockWindowStart = GetTickCount64();
        return;
    }
    const std::uint64_t now = GetTickCount64();
    if (now - g_blockWindowStart < 6000) return;
    g_blockWindowStart = now;
    ReportRuiBlockCensus();
    // Reset for the next window rather than stopping: the run is a sequence of
    // windows, and the interesting one is whichever coincided with a turn.
    g_blockArmed = false;
}

void SetRuiBlockCensusSelected(bool selected) {
    g_blockSelected = selected;
    Tf2VrLog(selected ? "[TF2VR] F5 arms the parameter-block census.\n"
                      : "[TF2VR] F5 does not arm the block census.\n");
}
bool RuiBlockCensusSelected() { return g_blockSelected; }

// ---- census snapshot on demand --------------------------------------------
//
// The five-second timer mixes a UI panel in with whatever follows it. A caller
// that knows something interesting just happened -- a button press that
// dismissed a screen -- can ask for a labelled dump at that instant instead,
// so two moments can be compared rather than averaged together.
//
// Read-only, and it does NOT reset the tallies: the counters are cumulative by
// design and clearing them here would silently break the periodic report.
void RequestRuiCensusSnapshot(int label) {
    if (!RuiCensusArmed()) {
        char line[200];
        std::snprintf(line, sizeof(line),
                      "[TF2VR] RUI census snapshot #%d requested, but the census is NOT ARMED -- "
                      "nothing was drawn here for want of an instrument, not for want of a "
                      "screen.\n", label);
        Tf2VrLog(line);
        return;
    }
    char line[160];
    std::snprintf(line, sizeof(line),
                  "[TF2VR] ==== RUI CENSUS SNAPSHOT #%d (taken AT the press) ====\n", label);
    Tf2VrLog(line);
    ReportRuiCensus();
}

// rui.census_early -- see the note at its use site in plugin.cpp.
namespace { bool g_ruiCensusEarly = false; }
void SetRuiCensusEarly(bool early) {
    g_ruiCensusEarly = early;
    Tf2VrLog(early ? "[TF2VR] RUI census will arm EARLY, before a world is rendered.\n"
                   : "[TF2VR] RUI census arms on the world gate, as usual.\n");
}
bool RuiCensusEarly() { return g_ruiCensusEarly; }

// ---- THE MARKER LADDER, on F6 ---------------------------------------------
//
// One press steps one arm. The FIRST arm is the identity: if that moves
// anything on screen the reading is wrong and nothing below it means anything.
// Both sides of 1.0 are stepped because the direction of a frustum-ratio error
// is not known in advance, and a ladder that only goes one way cannot tell
// "wrong direction" from "not a scale at all".
void StepRuiMarkerLadder() {
    // THE LADDER NOW STEPS THE HEAD-DELTA CORRECTION, not the size. The size
    // ladder did its job on 2026-09-05: it visibly changed the marker's size and
    // left the head error untouched, which is what ruled the coordinate space
    // out as the position's source. Keeping it would spend the wearer's presses
    // on a question already answered.
    //
    // Three arms only, because that is all the question needs: off, and the two
    // signs. If one sign locks the marker to the world, that is the fix and its
    // direction in a single press.
    static const float kFixArms[] = {0.0f, 1.0f, -1.0f};
    static const float kArms[] = {1.0f, 1.0f, 1.0f};
    constexpr int kArmCount = static_cast<int>(sizeof(kArms) / sizeof(kArms[0]));
    if (!g_markerTargetRva) {
        Tf2VrLog("[TF2VR] MARKER LADDER: rui.marker_target is 0, so no widget is substituted and "
                 "this key can do nothing. Set it in the ini (DECIMAL) and relaunch.\n");
        return;
    }
    // REFUSE RATHER THAN MISLEAD. On 2026-09-05 the wearer pressed all seven
    // arms and reported "no difference on any of them" -- and every one was a
    // no-op, because the widget had never been substituted. The counter said so
    // afterwards, in the log, which is exactly one run too late. If the lever
    // cannot reach the widget, this now says so at the FIRST press, in the same
    // place the wearer is already looking.
    bool substituted = false;
    for (int i = 0; i < g_subCount; ++i) {
        if (g_subs[i].rva == g_markerTargetRva) { substituted = true; break; }
    }
    if (!substituted) {
        char miss[420]{};
        std::snprintf(miss, sizeof(miss),
            "[TF2VR] MARKER LADDER REFUSED: ui(11)+0x%X is NOT substituted, so every arm would be "
            "a no-op and 'no difference' would mean nothing. The widget draws only while its "
            "marker is on screen -- bring a waypoint up (long-press Y) and give the census a few "
            "seconds to catch it, then press again. %d descriptor(s) are substituted so far.\n",
            g_markerTargetRva, g_subCount);
        Tf2VrLog(miss);
        return;
    }
    const unsigned long long before = g_markerAdjusted;
    g_markerStage = (g_markerStage + 1) % kArmCount;
    g_markerScale = kArms[g_markerStage];
    g_markerHeadFix = kFixArms[g_markerStage];
    // EVERY STAGE COUNTED SEPARATELY, so "no change" names its own cause
    // instead of costing another run to diagnose. draws = the widget ran;
    // swaps = we armed the interface slot; thunk = the slot was actually
    // called; fixes = the position was actually rewritten. The first zero
    // walking left to right is the broken stage.
    char line[640]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] ======== MARKER LADDER arm %d/%d: head-delta correction gain %+.1f on "
        "ui(11)+0x%X ========\n"
        "[TF2VR]   draws %llu | slot swaps %llu | thunk entered %llu | POSITIONS REWRITTEN %llu"
        " | last %.1f -> %.1f px at yaw delta %.2f deg\n"
        "[TF2VR]   %s\n",
        g_markerStage + 1, kArmCount, static_cast<double>(g_markerHeadFix), g_markerTargetRva,
        before,
        static_cast<unsigned long long>(g_markerSwaps),
        static_cast<unsigned long long>(g_markerThunkHits),
        static_cast<unsigned long long>(g_markerFixes),
        static_cast<double>(g_markerLastBefore), static_cast<double>(g_markerLastAfter),
        static_cast<double>(g_markerLastDelta),
        g_markerStage == 0
            ? "ARM 1 IS OFF -- the marker must behave exactly as it always has. This is the "
              "control; if it looks different here the reading is wrong."
            : "Look at a waypoint and TURN YOUR HEAD. Does the marker now HOLD STILL against the "
              "world? One of the two signs is the fix; the other will make it worse, which is "
              "itself confirmation. A zero adjusted count means the widget never drew.");
    Tf2VrLog(line);
}

void SetRuiMarkerSize(float scale) {
    // CLAMPED, NOT REJECTED. The old form returned silently on an out-of-range
    // value, so a stale ini would leave the size at whatever it happened to be
    // with nothing in the log saying why. The wearer set the floor at 0.20:
    // below that the marker is too small to read.
    if (scale < 0.20f) scale = 0.20f;
    if (scale > 0.80f) scale = 0.80f;
    g_markerSize = scale;
    for (int i = 0; i < 6; ++i) {
        if (kMarkerSizeLadder[i] == scale) { g_markerSizeRung = i; break; }
    }
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] hud.marker_size = %.2f: the marker draws at this fraction of full size. SMALLER "
        "NUMBER = SMALLER MARKER; the coordinate space is scaled by the RECIPROCAL, because enlarging "
        "the space is what shrinks the content laid out in it. SIZE ONLY -- the frustum owns the "
        "tracking, and the wearer confirmed the waypoint holds its spot at every rung. Range 0.10 to "
        "0.50, default 0.30. F5 steps the ladder.\n",
        scale);
    Tf2VrLog(line);
}

float RuiMarkerSize() { return g_markerSize; }

void StepRuiMarkerSize() {
    g_markerSizeRung = (g_markerSizeRung + 1) % 6;
    g_markerSize = kMarkerSizeLadder[g_markerSizeRung];
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F5 rung %d of 6: hud.marker_size = %.2f, i.e. %.0f%% of full size (ladder 0.45, 0.40, "
        "0.35, 0.30, 0.25, 0.20). Space scale by the reciprocal -- the ONLY live lever for this "
        "widget; the effective size at 0x2DE0 and the per-edge inset at 0x3AB0 are both recorded dead "
        "for it. The target holds its world spot; the authored text and leader line still ride toward "
        "the top-left, which is the open item.\n",
        g_markerSizeRung + 1, g_markerSize, g_markerSize * 100.0f);
    Tf2VrLog(line);
}

unsigned RuiHudSizeTypes() { return g_hudSizeTypes; }

void SetRuiHudSizeTypes(unsigned mask) {
    g_hudSizeTypes = mask;
    char line[340]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] hud.widget_types = %u: the size control substitutes draw types%s%s%s%s. Type 7 is "
        "the friend and enemy name labels; type 0 is the frontend and menus.\n",
        mask,
        (mask & 1) ? " 0" : "", (mask & 8) ? " 3" : "",
        (mask & 16) ? " 4" : "", (mask & 128) ? " 7" : "");
    Tf2VrLog(line);
}

// ---------------------------------------------------------------------------
// THE LOCK HUNT. One press, one widget, and the wearer's own eyes decide.
//
// Run 1 answered the counting question and closed two theories. No widget draws
// once per locked target: across the whole run only ui(11)+0x84070 (6 to 14 a
// frame, tracking what is in view) and ui(11)+0x1010 (3 a frame) draw more than
// once, so the eight rings are either one widget emitting eight elements in a
// single draw, or they are not RUI at all. Naming widgets by the assets their
// code references got the wrong answer twice and the wearer was right to
// distrust it.
//
// So this stops naming and starts suppressing. Each F3 press blanks exactly one
// widget by returning the engine's own "declined to draw" result for it, which
// is the same path rui_drawEnable takes. The wearer holds the lock and taps
// through the list; the press that makes the rings disappear names the widget
// outright, with no inference in between.
//
// ORDER IS BY EVIDENCE, not by address. 0x84070 first because it is the only
// per-entity widget in the set, it carries no asset strings of its own (so it
// draws what it is handed), and it is also the unnamed widget left over from
// the friendly-label defect -- one press tests both fronts at once.
//
// EVERY ARM CARRIES ITS OWN CONTROL. The count of suppressed calls prints when
// the arm ends. Zero suppressed means the arm never fired, and anything the
// wearer saw during it says NOTHING about that widget -- a null without a
// non-zero count is instrument failure, not a refutation.
namespace {
struct LockCandidate { unsigned rva; const char* note; };
constexpr LockCandidate kLockHunt[] = {
    {0x84070, "per-entity, 6-14 draws a frame, NO asset strings of its own. Prime suspect, and the friendly-label suspect too."},
    {0x1010,  "3 draws a frame, weapon_status.rui"},
    {0x86220, "unnamed, no asset strings"},
    {0x7D7B0, "unnamed, draws 'white' primitives and a %05.2f number"},
    {0x1D560, "draws 'white' primitives, 11 registers in symmetric pairs"},
    {0x1CA90, "crosshair_arc"},
    {0x22820, "damage_indicator_arc + objective_marker"},
    {0x93BE0, "hud_defs.rui"},
    {0x5B500, "weapon_status.rui"},
    {0x5B1B0, "cooldown_segment / dropship icons"},
    {0x8A330, "target_info.rui, the ENEMY name label"},
    {0x69570, "overhead_icon_titan_arrow_you"},
    {0xC4D0,  "battery / gametype icons"},
    {0x5090,  "gametype icons"},
    {0x53D90, "hit_indicator"},
    {0x66DD0, "button_purchase / gametype icons"},
    {0x76420, "hud_defs.rui"},
    {0x766D0, "scoreboard"},
    {0x5F6B0, "arc_launcher_reticle.rui"},
    {0xBF60,  "hud_defs.rui, noise_uniform"},
    {0x15DE0, "hud_defs.rui"},
    {0x2110,  "cockpit base plate"},
    {0x2B190, "earn meter"},
    {0x36A0,  "cockpit dial"},
    {0x3A00,  "the two Titan dash bars"},
    {0x4320,  "weapon_status.rui"},
    {0x55D00, "weapon_status.rui"},
    {0x5B800, "lockon_indicator centre+edge"},
};
constexpr int kLockHuntCount = sizeof(kLockHunt) / sizeof(kLockHunt[0]);
int g_lockHunt = -1;
}  // namespace

void StepRuiLockHunt() {
    HMODULE m = GetModuleHandleA("ui(11).dll");
    if (!m) {
        Tf2VrLog("[TF2VR] LOCK HUNT: ui(11).dll is not loaded yet; nothing suppressed.\n");
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(m);
    char line[720]{};
    if (g_lockHunt >= 0 && g_lockHunt < kLockHuntCount) {
        std::snprintf(line, sizeof(line),
            "[TF2VR] LOCK HUNT arm %d/%d (ui(11)+0x%X) ENDS: %llu draws were suppressed. %s\n",
            g_lockHunt + 1, kLockHuntCount, kLockHunt[g_lockHunt].rva,
            static_cast<unsigned long long>(g_ruiSuppressed),
            g_ruiSuppressed == 0
                ? "ZERO -- this arm NEVER FIRED, so whatever was on screen during it says nothing "
                  "about this widget and it must be retried."
                : "Reached. Whatever the wearer saw during it is about this widget.");
        Tf2VrLog(line);
    }
    ++g_lockHunt;
    if (g_lockHunt >= kLockHuntCount) {
        g_lockHunt = -1;
        g_ruiSuppressTarget = 0;
        g_ruiSuppressed = 0;
        Tf2VrLog("[TF2VR] ======== LOCK HUNT EXHAUSTED, suppression OFF ========\n"
                 "[TF2VR] Every widget that draws in a world has been blanked once. If the lock "
                 "rings never disappeared, they are not drawn by an RUI widget at all, and the "
                 "next place to look is the client's own sprite and particle path.\n");
        return;
    }
    g_ruiSuppressed = 0;
    g_ruiSuppressTarget = base + kLockHunt[g_lockHunt].rva;
    std::snprintf(line, sizeof(line),
        "[TF2VR] ======== LOCK HUNT arm %d of %d: ui(11)+0x%X BLANKED ========\n"
        "[TF2VR]   %s\n"
        "[TF2VR]   If the LOCK RINGS vanished on this press, this is the widget. Press F6 and "
        "stop. If something else vanished, say what.\n",
        g_lockHunt + 1, kLockHuntCount, kLockHunt[g_lockHunt].rva, kLockHunt[g_lockHunt].note);
    Tf2VrLog(line);
}


// Same-session disappearance search. Only the verified suppression lever moves.
// F3 is the only key that advances it, and it advances from ANY state, so a
// wearer who has lost the thread never has to work out which key they are on.
void StepRuiWidgetHunt() {
    g_ruiSuppressTarget = 0; // retire the old single-target arm before the ladder
    ReportRuiSuppressionControl(); // includes null-layer/unknown-descriptor entries
    RuiHuntStep(reinterpret_cast<std::uintptr_t>(GetModuleHandleA("ui(11).dll")));
}

// F5. The wearer's report that the friendly name went away at THIS step.
void MarkRuiWidgetHunt() {
    ReportRuiSuppressionControl();
    RuiHuntMark();
}
void ReportRuiSuppressionControl() {
    char line[640]{};
    const auto suppressed = g_ruiSuppressed;
    std::snprintf(line, sizeof(line),
        "[TF2VR] SUPPRESSION CONTROL active=%d completed-widget entries=%llu null-layer=%llu known=%llu unknown-descriptor=%llu suppressed=%llu | %s\n",
        g_ruiSuppressTarget != 0,
        g_controlCalls.load() - g_controlStartCalls,
        g_controlNull.load() - g_controlStartNull,
        g_controlKnown.load() - g_controlStartKnown,
        g_controlUnknown.load() - g_controlStartUnknown,
        static_cast<unsigned long long>(suppressed),
        !g_ruiSuppressTarget ? "OFF; no identification claim."
        : suppressed ? "GATE: matched original identity and requested engine decline. Wearer must identify what disappeared; count alone identifies nothing."
        : "INVALID: ZERO emissions. Instrument/control failure; no widget is cleared.");
    Tf2VrLog(line);
}
// The paired control for the lock hunt: a shot of the SAME arm without the lock
// up, so "the rings are gone" can be told apart from "this widget was never
// drawing anything I was looking at". Logs which arm is currently blanked and
// how many draws it has swallowed so far, without advancing the ladder.
void LogRuiLockHuntState() {
    char line[560]{};
    if (g_lockHunt < 0 || g_lockHunt >= kLockHuntCount) {
        std::snprintf(line, sizeof(line),
            "[TF2VR] LOCK HUNT: no arm is active, nothing is blanked. This frame is the CLEAN "
            "baseline.\n");
    } else {
        std::snprintf(line, sizeof(line),
            "[TF2VR] LOCK HUNT: frame captured with arm %d/%d STILL BLANKED (ui(11)+0x%X), %llu "
            "draws suppressed so far. Ladder NOT advanced. %s\n",
            g_lockHunt + 1, kLockHuntCount, kLockHunt[g_lockHunt].rva,
            static_cast<unsigned long long>(g_ruiSuppressed),
            g_ruiSuppressed == 0 ? "ZERO suppressed -- this arm is not firing, so this frame says "
                                   "nothing about it."
                                 : "Arm is firing, so this frame is about this widget.");
    }
    Tf2VrLog(line);
}

// ---------------------------------------------------------------------------
// THE SPLIT: is the lock ring RUI AT ALL?
//
// The per-widget hunt eliminated nineteen widgets with the rings still on
// screen, including the prime suspect at 1470 suppressed draws. Nine arms read
// zero suppressed and are void. Sweeping the rest one at a time is another long
// run for a narrow answer, and it still could not see anything outside the
// FC500 path.
//
// So this stops asking WHICH widget and asks whether it is a widget. The
// engine's own `rui_drawEnable` blanks the entire RUI system -- this project
// already measured that it takes the whole HUD with it (RUNG2-RESULT). One
// press, one look:
//
//   rings GONE with all RUI off  -> they are RUI, and the hunt simply missed
//                                   the widget (nine untested arms, plus
//                                   anything outside the FC500 path).
//   rings STILL THERE            -> they are not RUI at all, every widget-level
//                                   avenue on this front is closed by
//                                   measurement, and the client's own sprite
//                                   and particle path is next.
//
// The whole HUD vanishing is the built-in positive control: if the rest of the
// HUD does NOT vanish, the convar did not take and the observation is void.
void ToggleRuiDrawEnable() {
    static bool off = false;
    off = !off;
    const bool ok = TrySetCvarFloat("rui_drawEnable", off ? 0.0f : 1.0f);
    char line[620]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] RUI DRAW %s (rui_drawEnable = %d, write %s). CONTROL: the WHOLE HUD must "
        "disappear on this press. If it did not, the write did not take and this frame says "
        "nothing. If the HUD went but the LOCK RINGS STAYED, the rings are not RUI and every "
        "widget-level avenue is closed by measurement.\n",
        off ? "OFF" : "back ON", off ? 0 : 1, ok ? "accepted" : "REFUSED");
    Tf2VrLog(line);
}

// ---------------------------------------------------------------------------
// WHICH DRAW SYSTEM PUTS THE LOCK RINGS ON SCREEN.
//
// Northstar's `rui_drawEnable` is not a general RUI switch. Its registration at
// Northstar+0x5D4F0 resolves engine+0xFC500 and hooks it, and the detour at
// +0x5D5D0 is unconditional -- convar false, return, no type filter. So it
// blanks everything drawn through FC500 and nothing else.
//
// That is why the wearer's HUD vanished and the lock rings, the missile icons
// and the LOCK text did not: those three are not drawn through FC500. The only
// other widget-draw call site in the engine's RUI region is FC960, and this
// plugin has been watching that all session -- 29 descriptors, every one first
// seen in the menus, none at lock time. So the lock group is not an RUI widget.
//
// It has to be drawn by SOMETHING, and the engine exposes a switch per system.
// One press turns off exactly one of them with the others left alone, so the
// press that kills the rings names the system outright instead of narrowing it.
// Each arm reports whether the convar write was accepted: a refused write means
// the arm never ran and its frame says nothing.
namespace {
struct DrawSystem { const char* cvar; const char* what; };
constexpr DrawSystem kDrawSystems[] = {
    {"rui_drawEnable",  "RUI, via Northstar's FC500 gate (known: rings SURVIVE this)"},
    {"r_drawvgui",      "VGUI -- the other UI system, panels and Hud_* elements"},
    {"r_drawparticles", "the particle system -- lock FX attached to weapon or target"},
    {"r_drawsprites",   "sprites -- screen-space quads drawn in the 3D pass"},
    {"cl_drawhud",      "the client HUD switch"},
};
constexpr int kDrawSystemCount = sizeof(kDrawSystems) / sizeof(kDrawSystems[0]);
int g_drawSystem = -1;
}  // namespace

void StepDrawSystemHunt() {
    // Everything back on first, so exactly one system is ever off and the arm
    // that kills the rings is the one that owns them.
    for (int i = 0; i < kDrawSystemCount; ++i) TrySetCvarFloat(kDrawSystems[i].cvar, 1.0f);
    ++g_drawSystem;
    char line[620]{};
    if (g_drawSystem >= kDrawSystemCount) {
        g_drawSystem = -1;
        Tf2VrLog("[TF2VR] ======== DRAW SYSTEM HUNT: every system restored, ladder reset ========\n"
                 "[TF2VR] If the rings survived ALL of them, they are drawn by a path none of "
                 "these convars gate, and the next step is a frame capture rather than a switch.\n");
        return;
    }
    const bool ok = TrySetCvarFloat(kDrawSystems[g_drawSystem].cvar, 0.0f);
    std::snprintf(line, sizeof(line),
        "[TF2VR] ======== DRAW SYSTEM arm %d/%d: %s = 0 (write %s) ========\n"
        "[TF2VR]   %s\n"
        "[TF2VR]   If the LOCK RINGS vanished on this press, that system draws them. If the write "
        "was REFUSED this arm never ran and the frame says nothing.\n",
        g_drawSystem + 1, kDrawSystemCount, kDrawSystems[g_drawSystem].cvar,
        ok ? "accepted" : "REFUSED", kDrawSystems[g_drawSystem].what);
    Tf2VrLog(line);
}

// ---------------------------------------------------------------------------
// ASK VGUI TO NAME ITS OWN PANELS.
//
// Three string-led guesses at the lock draw have now failed, the last one with
// all three candidates reading ZERO calls. The idiom I searched for -- read the
// client view object, call AngleVectors -- is simply not how this draws.
//
// So stop searching. `r_drawvgui 0` took the rings, the missile icons and the
// LOCK text away together, so it is VGUI, and VGUI can print its own panel tree
// on screen: engine.dll carries `vgui_drawtree`. The panel that appears when
// the trigger is held names the thing outright.
//
// THIS PART IS FLAT-TESTABLE. Identifying the panel needs no headset -- only
// the eventual fix does -- so it costs the cheap kind of run.
void ToggleVguiDrawTree() {
    static bool on = false;
    on = !on;
    const bool tree = TrySetCvarFloat("vgui_drawtree", on ? 1.0f : 0.0f);
    const bool hidden = TrySetCvarFloat("vgui_drawtree_hidden", on ? 1.0f : 0.0f);
    char line[560]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] VGUI PANEL TREE %s (vgui_drawtree write %s, vgui_drawtree_hidden write %s). "
        "The tree lists every live panel by name. With the lock held, the panel that owns the "
        "rings is in that list. If BOTH writes were refused the convars are not registered and "
        "this press says nothing.\n",
        on ? "ON" : "OFF", tree ? "accepted" : "REFUSED", hidden ? "accepted" : "REFUSED");
    Tf2VrLog(line);
}
