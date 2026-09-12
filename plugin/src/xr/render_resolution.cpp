#include "render_resolution.h"
#include "camera_update_hook.h"
#include "present_hook.h"
#include "xr_context.h"

#include "config.h"
#include "engine_cvars.h"
#include "diagnostics.h"
#include "vr_input.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>

namespace {

// Offsets into engine.dll, taken from NorthstarLauncher's r2engine.cpp, which
// is the reference clone in this repo and targets the same build we run under.
// They are still verified before use: an offset that has moved would otherwise
// call into the middle of an unrelated function with a string argument.
constexpr std::uintptr_t kCbufGetCurrentPlayerOffset = 0x120630;
constexpr std::uintptr_t kCbufAddTextOffset = 0x1203B0;
constexpr std::uintptr_t kCbufExecuteOffset = 0x1204B0;

enum class ECommandTarget_t { CBUF_FIRST_PLAYER = 0, CBUF_LAST_PLAYER = 1, CBUF_SERVER = 2 };
enum cmd_source_t { kCommandSrcCode = 0 };

using CbufGetCurrentPlayerFn = ECommandTarget_t (*)();
using CbufAddTextFn = void (*)(ECommandTarget_t, const char*, cmd_source_t);
using CbufExecuteFn = void (*)();

int g_requestedWidth = 0;

// mat_letterbox_aspect_threshold / _goal, the engine's own letterbox dial.
// 0 leaves the game's 1.59 / 1.6 untouched. See ApplyLetterboxFloorIfRequested.
std::atomic<float> g_letterboxFloor{0.0f};
bool g_letterboxApplied = false;

// WHETHER THIS LAUNCH IS ON A MONITOR. `profile = 1` in the ini.
//
// This exists because of a trap that has now cost two flat runs. The headset
// cache is loaded unconditionally and `LoadHeadsetCache` sets the active
// headset to entry 0 whenever the file holds anything at all -- whether or not
// a headset is connected. `DerivedRenderTarget` then happily derives a
// per-eye resolution from a headset that is not on anybody's head, and a flat
// run comes up in a 5840x3648 window -- 8760x5472 once Windows scales it for a
// DPI-unaware game -- on a 3840x2160 panel, and 17% of the picture is visible.
// The wearer met it as "I see a quarter of the screen", and the run was lost.
//
// The resolution has to be decided before OpenXR could report, which is the
// whole reason the cache exists, so "is a headset actually connected" is not
// knowable at that moment. The profile IS: it is the wearer's own statement
// about which kind of run this is, it is already in the ini, and it is already
// the thing every report has to name. So a flat profile ignores the cache and
// uses the size the ini asked for.
bool g_flatProfile = false;
int g_baseWidth = 0;
int g_baseHeight = 0;
int g_requestedHeight = 0;
bool g_requestedWindowed = true;
bool g_settled = false;
bool g_attempted = false;

// Enough of a sanity check to refuse an offset that has moved. Every one of
// these three is a real function entry, so it must be inside the module's
// executable range and must not begin with a byte that cannot start one.
bool LooksLikeCode(const std::uint8_t* address, const std::uint8_t* base, size_t imageSize) {
    if (!address) return false;
    if (address < base || address >= base + imageSize) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    const DWORD protection = info.Protect & 0xFF;
    const bool executable = protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
                            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    if (!executable) return false;
    // int3 padding and a null byte are the two things a function entry is
    // never allowed to be here, and both are what a stale offset lands on.
    return address[0] != 0xCC && address[0] != 0x00;
}

}  // namespace

bool RunEngineConsoleCommand(const char* command) {
    if (!command || !*command) return false;
    HMODULE engine = GetModuleHandleA("engine.dll");
    if (!engine) return false;

    auto* base = reinterpret_cast<std::uint8_t*>(engine);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const size_t imageSize = nt->OptionalHeader.SizeOfImage;

    auto* getPlayer = base + kCbufGetCurrentPlayerOffset;
    auto* addText = base + kCbufAddTextOffset;
    auto* execute = base + kCbufExecuteOffset;
    if (!LooksLikeCode(getPlayer, base, imageSize) || !LooksLikeCode(addText, base, imageSize) ||
        !LooksLikeCode(execute, base, imageSize)) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            Tf2VrLog("[TF2VR] engine console: the Cbuf offsets did not verify, so this build is not the "
                     "one they were taken from. No console command was run, and none will be.\n");
        }
        return false;
    }

    // Cbuf_AddText appends to a text buffer that is then parsed line by line, so
    // a command without a trailing newline runs into whatever is added next.
    // Terminating here rather than at every call site means one caller cannot
    // silently swallow another's command.
    char terminated[512]{};
    std::snprintf(terminated, sizeof(terminated), "%s", command);
    const size_t length = std::strlen(terminated);
    if (length && terminated[length - 1] != '\n' && length + 1 < sizeof(terminated)) {
        terminated[length] = '\n';
        terminated[length + 1] = '\0';
    }

    auto cbufGetCurrentPlayer = reinterpret_cast<CbufGetCurrentPlayerFn>(getPlayer);
    auto cbufAddText = reinterpret_cast<CbufAddTextFn>(addText);
    auto cbufExecute = reinterpret_cast<CbufExecuteFn>(execute);
    cbufAddText(cbufGetCurrentPlayer(), terminated, kCommandSrcCode);
    cbufExecute();
    return true;
}

int RequestedRenderWidth() { return g_requestedWidth; }
int RequestedRenderHeight() { return g_requestedHeight; }
bool RequestedRenderWindowed() { return g_requestedWindowed; }

// Separate and LATER than the convar resample, because it is not a measurement:
// it is the repair, and it has to land after the UI has finished rebuilding.


// The last size actually asked for, so the experiment keys can re-apply it
// exactly rather than recomputing it from a derivation that may have moved.
int g_lastAppliedWidth = 0;
int g_lastAppliedHeight = 0;

bool ForceRenderResolutionNow(int width, int height, bool windowed) {
    if (width <= 0 || height <= 0) return false;
    char command[96]{};
    std::snprintf(command, sizeof(command), "mat_setvideomode %d %d %d\n", width, height,
                  windowed ? 1 : 0);
    if (!RunEngineConsoleCommand(command)) {
        Tf2VrLogWrite("[TF2VR] live resolution change NOT applied: the engine command buffer was "
                 "unavailable or its offsets did not verify. Nothing was called.\n", true);
        return false;
    }
    char line[240]{};
    std::snprintf(line, sizeof(line), "[TF2VR] live resolution change: asked the engine for "
                  "%dx%d (%s).\n", width, height, windowed ? "windowed" : "fullscreen");
    Tf2VrLog(line);
    // The mode change is asynchronous -- Cbuf_Execute runs the command, but the
    // material system restarts over the following frames -- so an immediate
    // second sample would read the old values and prove nothing. This is queued
    // for a couple of seconds later, which is after the restart the wearer sees
    // as the screen blinking.
    // DO NOT PUSH A PENDING SAMPLE FURTHER OUT. The 08-29 run pressed these
    // keys repeatedly and each press reset the timer, so most BEFORE lines
    // never got their AFTER -- a bracket with one arm is not a bracket.
    g_lastAppliedWidth = width;
    g_lastAppliedHeight = height;
    return true;
}

// THE BASELINE, kept separately from the applied size.
//
// render.scale multiplies THIS, so scaling preserves the wearer's tuned ASPECT
// exactly -- and the buffer aspect is what sets the game's horizontal field of
// view. Scaling both axes by the same number therefore changes resolution and
// CANNOT change FOV, which is the property that was missing every previous time
// this was attempted.
void SetRequestedRenderResolution(int width, int height) {
    g_baseWidth = width;
    g_baseHeight = height;
    g_requestedWidth = width;
    g_requestedHeight = height;
}

void SetRequestedRenderWindowed(bool windowed) { g_requestedWindowed = windowed; }

bool RenderResolutionSettled() { return g_settled; }

void ApplyRenderResolutionIfRequested() {
    if (g_settled || g_attempted) return;
    // render.scale multiplies the tuned size from the ini, both axes equally, so
    // the aspect and therefore the field of view are untouched.
    {
        int scaledWidth = 0, scaledHeight = 0;
        ScaledRenderTarget(&scaledWidth, &scaledHeight);
        if (scaledWidth > 0 && scaledHeight > 0) {
            g_requestedWidth = scaledWidth;
            g_requestedHeight = scaledHeight;
            // NAMES WHERE THE NUMBER CAME FROM. It used to say "from the
            // runtime's recommendation" unconditionally, which on a flat run is
            // simply false -- and false in the exact direction that hid a window
            // three times too big for the monitor behind a line that looked
            // healthy. A log line that cannot be wrong about its own source is
            // the difference between a wasted run and a two-second check.
            char line[320]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] render.scale %.2f resolves to %dx%d, from %s.\n",
                static_cast<double>(RenderScale()), scaledWidth, scaledHeight,
                IsFlatProfile()
                    ? "the ini's own render.width/height -- FLAT profile, so the per-headset "
                      "cache is deliberately ignored"
                    : "the connected or last-seen headset's recommendation");
            Tf2VrLog(line);
        }
    }
    if (g_requestedWidth <= 0 || g_requestedHeight <= 0) {
        g_settled = true;
        return;
    }
    if (!GetModuleHandleA("engine.dll")) return;  // Not loaded yet; try again next frame.
    g_attempted = true;

    char command[96]{};
    std::snprintf(command, sizeof(command), "mat_setvideomode %d %d %d\n",
                  g_requestedWidth, g_requestedHeight, g_requestedWindowed ? 1 : 0);
    if (!RunEngineConsoleCommand(command)) {
        Tf2VrLogWrite("[TF2VR] render resolution NOT applied: the engine command buffer was unavailable or "
                 "its offsets did not verify. Nothing was called.\n", true);
        g_settled = true;
        return;
    }

    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] render resolution: asked the engine for %dx%d (%s) through its own console buffer. "
        "The game renders one frame per eye, so this IS the per-eye resolution -- at 2560x1440 it "
        "was about 2.06x short across and 2.67x short down of what the runtime asks for.\n",
        g_requestedWidth, g_requestedHeight, g_requestedWindowed ? "windowed" : "fullscreen");
    Tf2VrLog(line);
    // RECORD IT HERE TOO, and this omission cost a headset run. The experiment
    // keys re-apply "the last size actually asked for", and that was only
    // recorded in ForceRenderResolutionNow -- the LIVE path. A session that
    // never moved the panel's slider therefore had no last size, and both keys
    // refused with "nothing to re-apply" while looking, from the headset, like
    // dead keys. Startup is an application of a resolution exactly as much as
    // the slider is, and it is the one every session performs.
    g_lastAppliedWidth = g_requestedWidth;
    g_lastAppliedHeight = g_requestedHeight;
    g_settled = true;
}

// ---------------------------------------------------------------------------
// render.scale. See the note in the header for why this is one number and not
// two.
// ---------------------------------------------------------------------------
namespace {
std::atomic<float> g_renderScale{kDefaultRenderScale};
}  // namespace

// THE DEFAULT IS kDefaultRenderScale (0.75), AND THE COMMENT THAT WAS HERE WAS
// DESCRIBING A DEFAULT OF 1.20 THAT THIS FILE HAS NOT CARRIED FOR SOME TIME.
// Read the constant, not a comment block above the setter.
//
// Why the number moved off 1.00, 2026-09-11: at 1.00 the derived buffer is the
// runtime's recommended per-eye height widened to the engine's 1.6 aspect
// floor, which on the test headset is 5222x3264 -- 17.0 Mpx, a little over
// TWICE a 4K frame, every frame, targeting 90 Hz. Nothing in this mod looks at
// the GPU before choosing that, and the only machine it has ever been measured
// on has a 5090. Shipping 1.00 makes "it stutters" the first experience on any
// mid-range card, and the fix is a number most people will never find.
//
// Both axes move together, so cost falls with the SQUARE: 0.75 is 56% of the
// pixels, 9.6 Mpx, about 1.16x a 4K frame. The field of view is untouched at
// any scale -- this trades sharpness, and only sharpness.
void SetRenderScale(float scale) {
    // 1.00 is the tuned size unchanged. Both axes move together, so this can
    // never alter the aspect or the field of view.
    // Up to 2x, as asked. The old 1.6 ceiling was arbitrary caution on my part;
    // 2x is four times the pixels and the frame-rate readout is right there to
    // show what it costs, which is a better guard than a cap the wearer cannot
    // reach past.
    if (scale < 0.5f) scale = 0.5f;
    if (scale > 2.0f) scale = 2.0f;
    const float previous = g_renderScale.exchange(scale, std::memory_order_acq_rel);

    int w = 0, h = 0;
    ScaledRenderTarget(&w, &h);
    if (w <= 0 || h <= 0) return;

    // APPLIED IMMEDIATELY, NOT ON THE NEXT LAUNCH.
    //
    // "Never had to exit a VR MOD in my LIFE to change resolution", and that is
    // fair -- the machinery to do it live has been sitting in this file the
    // whole time. ForceRenderResolutionNow drives the engine's own
    // mat_setvideomode, which is exactly what the startup path calls; the only
    // difference was that the startup path is gated to run once.
    //
    // Nothing about the aspect changes, so the XR layer's frustum and the
    // wearer's field of view are untouched. The swapchain IS recreated, which
    // xr_context already handles through HandleBackbufferResize -- that path
    // exists because the game can change resolution on its own.
    //
    // Skipped during startup: ApplyRenderResolutionIfRequested has not run yet,
    // engine.dll may not be loaded, and forcing a mode change before the engine
    // is up is how the startup path used to fail.
    if (previous == scale) return;
    if (!GetModuleHandleA("engine.dll")) return;
    // APPLIED LIVE, and a previous version of this file removed that on evidence
    // that did not cover it.
    //
    // The aspect sweep measured eight mode changes about 2.5 seconds apart in a
    // FLAT session with no XR: the first rendered and every one after went black.
    // I generalised that to "only the first live mode change of a session
    // renders" and made this control startup-only.
    //
    // The wearer's evidence is better and it is of the case that matters:
    // "In all my testing on PFD MR, resolution changes made immediately when
    // exiting the config menu are PERFECT." One change, occasional, with an XR
    // session up. That is not the experiment I ran -- flat against XR, and eight
    // rapid against one deliberate -- so my finding never applied to it.
    //
    // What broke on the Quest 3 was NOT this path. It was the buffer aspect
    // (full extents instead of symmetric cover) and a cl_fovScale typed for a
    // different headset, both fixed elsewhere. Removing a working control on the
    // strength of a finding from another context would have been the real
    // regression.
    if (ForceRenderResolutionNow(w, h, g_requestedWindowed)) {
        g_requestedWidth = w;
        g_requestedHeight = h;
        char line[240]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] render.scale %.2f applied LIVE -> %dx%d per eye. Aspect unchanged, so the "
            "field of view is unchanged.\n",
            static_cast<double>(scale), w, h);
        Tf2VrLog(line);
    }
}

float RenderScale() { return g_renderScale.load(std::memory_order_acquire); }

// THE SCALE MOVES THE HEIGHT. THE WIDTH FOLLOWS THE GAME'S FIELD OF VIEW.
//
// This is the correction to a version of this function that scaled the runtime's
// recommendation in BOTH axes, and it would have quietly undone measured work.
//
// The runtime here recommends 4032x3648 per eye -- aspect 1.105, near square,
// which is what the headset panel actually is. The project renders 5840x3648:
// the recommended HEIGHT exactly, with the width widened to aspect 1.6009 so the
// buffer matches the GAME'S FRUSTUM. That is not decoration. The frustum log
// records why: "every screen-space pass it runs, temporal AA and its motion
// vectors first among them, assumes the frustum matches the rectangle.
// Anisotropy shows up when the camera MOVES."
//
// So scaling both axes off the runtime's recommendation would have rendered a
// 1.105-aspect buffer into a 1.6009 frustum and brought the smearing back.
//
// The aspect is taken from render.width/height when they are set -- that pair IS
// the measured aspect for this game and runtime -- and from the runtime only as
// a fallback when nothing better is known.
//
// Scale 1.00 therefore reproduces the tuned 5840x3648 exactly, because that size
// already IS the recommended height at the game's aspect.
// THE SCALE IS DEFINED IN PANEL TERMS, and everything else is derived from it.
//
// scale = the per-eye pixels DELIVERED TO THE PANEL, relative to what the
// runtime says it wants. 1.00 feeds the headset exactly its requested per-eye
// size. That is the number a wearer can reason about; the internal buffer is a
// consequence, not a control.
//
// The internal buffer is then the delivered size widened by the FOV overshoot,
// because the game renders a wider cone than the headset shows and the buffer
// must match the GAME's shape or its screen-space passes smear. So:
//
//   delivered = recommended x scale
//   internal  = delivered x (game FOV / headset FOV)
//
// which is exactly the relationship the video page reports, in the same order.
// THE BUFFER IS THE PANEL. NOTHING IS RENDERED THAT IS NOT DISPLAYED.
//
// The buffer aspect DRIVES the game's horizontal field of view -- measured, and
// recorded beside game.fov_scale: "vertical FOV is 92.9 deg at cl_fovScale 1.55
// and stays 92.9 across every buffer aspect tried (1.778, 1.600, 1.105) while
// horizontal follows the aspect."
//
// So setting the buffer to the HEADSET'S OWN tangent ratio makes the game render
// exactly the cone the headset shows. Rendered equals displayed, the overshoot
// is zero, and there is no cropping to explain in a footnote.
//
// This is not the withdrawn xr.match_headset_fov. That CLAMPED the frustum on
// every main-scene upload including during ADS, which broke the zoom -- "not
// sharp like before, zooming in at an odd angle". Nothing is clamped here: the
// buffer is simply the right shape, and ADS narrows relative to it exactly as it
// does today.
//
// It is also what the 2026-08-16 note concluded. The objection then was
// anisotropic pixels from narrowing into a 16:9 buffer; "with the aspect set to
// the headset's own tangent ratio (110/90 -> 1.4283) the match is isotropic and
// the objection is gone."
// WHAT OPENXR ASKS FOR, TIMES THE SCALE. NOTHING ELSE.
//
// No aspect correction, no FOV derivation, no overshoot arithmetic. Those were
// all mine and all wrong: I built them on a 110x90 FOV read out of an OLD
// session logged on a different headset, and used it to "correct" the runtime's
// own recommendation on a machine it did not describe.
//
// xrEnumerateViewConfigurationViews reports recommendedImageRectWidth/Height.
// That is the runtime telling us, for THIS headset, the per-eye size it wants.
// It already accounts for the panel and for whatever distortion margin the
// runtime needs. It is not ours to second-guess.
//
//   scale 1.00 -> exactly what the runtime asks for
//   scale 1.50 -> 50 per cent more in each axis
//
// If that shape turns out to be wrong for the game's frustum, that is a
// measurement to make deliberately, not a correction to apply pre-emptively to
// a number the runtime is the authority on.
// THE SLIDER SCALES THE TUNED SIZE, ASPECT UNTOUCHED.
//
// Both axes are multiplied by the same number, so the buffer ASPECT is exactly
// what it was -- and the buffer aspect is what sets the game's horizontal field
// of view. This control therefore changes sharpness and frame rate and CANNOT
// change what the wearer sees of the world.
//
// That property is the whole point. Every earlier attempt derived the size from
// the runtime's recommended rect, whose aspect (1.105) differs from this game's
// frustum (1.60), so every earlier attempt narrowed the FOV as a side effect.
//
//   1.00 -> exactly the tuned 5840x3648
//   1.25 -> 7300x4560, sharper and dearer
//   0.75 -> 4380x2736, softer and cheaper
void ScaledRenderTarget(int* width, int* height) {
    if (width) *width = 0;
    if (height) *height = 0;
    const float scale = g_renderScale.load(std::memory_order_acquire);
    if (scale <= 0.0f) return;
    // THE CACHE FIRST, THE INI AS A FALLBACK FOR A HEADSET NEVER SEEN.
    //
    // Derived wins whenever this headset HAS been seen, because it is right for
    // the headset actually connected rather than for whichever one the ini was
    // written on. The explicit render.width/height is what a FIRST-EVER launch
    // falls back on -- without it a fresh install has no geometry, derives
    // nothing, and the game keeps its own resolution, which the wearer met as
    // "it is looking very low res and changing the slider does nothing".
    //
    // So an explicit size is no longer an override that beats everything. It is
    // the seed for a headset nothing is known about yet, which is the meaning
    // that cannot strand a new install and cannot outlive its own headset.
    // The cache is a PER-HEADSET memory, so a monitor run must not read it.
    // See the note at g_flatProfile: this is the line that put a 5840x3648
    // window on a screen that could show a quarter of it.
    if (!g_flatProfile && DerivedRenderTarget(width, height, scale)) return;
    if (g_baseWidth <= 0 || g_baseHeight <= 0) return;

    // Rounded to EVEN. Odd render dimensions have bitten this project before on
    // the half-resolution passes the engine derives from the main target.
    auto roundEven = [](float v) {
        int n = static_cast<int>(v + 0.5f);
        return n & 1 ? n + 1 : n;
    };
    if (width) *width = roundEven(static_cast<float>(g_baseWidth) * scale);
    if (height) *height = roundEven(static_cast<float>(g_baseHeight) * scale);
}

// THE RESOLUTION IN PANEL TERMS, which is the only form a wearer can reason
// about.
//
// Wearer: "I think we should show a scaled resolution in panel terms - so just
// multiply the scaling factor by the panel resolution. I know this isn't what is
// used internally but it is in terms a user will understand."
//
// Exactly right, and this function's own comment has claimed to do it since it
// was written while the body returned the INTERNAL BUFFER -- 6096x4268, which is
// wider than the panel because it matches the LENS CONE. So the menu offered a
// number that looked like a resolution, was labelled like a panel size, and was
// neither. That is worse than showing nothing.
//
// It is now what it says: the runtime's recommended per-eye size times the
// scale. The live runtime figures are preferred when XR is up; the cached ones
// stand in before it is, so the panel does not read 0x0 in a menu opened early.
//
// This number is DELIBERATELY NOT the buffer the game renders. It is the honest
// answer to "how much detail am I asking for, relative to what my headset wants",
// and the internal buffer is a consequence of it and of the lens shape.
void PanelResolutionForScale(float scale, int* width, int* height) {
    if (width) *width = 0;
    if (height) *height = 0;
    if (scale <= 0.0f) return;
    unsigned int recW = RuntimeRecommendedEyeWidth();
    unsigned int recH = RuntimeRecommendedEyeHeight();
    if (!recW || !recH) CachedRecommendedEyeSize(&recW, &recH);
    if (!recW || !recH) return;
    auto roundEven = [](float v) { int n = static_cast<int>(v + 0.5f); return n & 1 ? n + 1 : n; };
    if (width) *width = roundEven(static_cast<float>(recW) * scale);
    if (height) *height = roundEven(static_cast<float>(recH) * scale);
}

// What the slider is a multiple of: the wearer's tuned size from the ini.
unsigned int ReferenceRenderHeight() { return static_cast<unsigned int>(g_baseHeight); }

float EffectiveRenderScale() {
    // The scale IS the setting now, and it always has a value. No inference from
    // the current buffer, no FOV arithmetic -- both of which were wrong.
    return RenderScale();
}

// ---------------------------------------------------------------------------
// THE LETTERBOX FLOOR. The engine exposed it as a convar the whole time.
//
// A1 measured the world pass obeying `height = min(bufferH, bufferW / 1.600)`
// at seven aspects, to the pixel, and concluded the constant would have to be
// found in the disassembly. The wearer suggested checking whether the modding
// community had already dealt with restrictions like this. It had not -- but
// the search turned up the convar NAMES, and materialsystem_dx11.dll carries
// them with their defaults and its own help text:
//
//   mat_letterbox_aspect_threshold   "1.59"
//   mat_letterbox_aspect_goal        "1.6"
//   "Letterbox when the window aspect ratio is below this threshold"
//
// Those two numbers reproduce all seven measured rows exactly. So the floor is
// not a hardcoded constant needing memory surgery; it is a dial, and the
// interface ladder says take the dial.
//
// Set the THRESHOLD below the buffer aspect and the letterbox never triggers,
// so the world pass fills the buffer, the frustum still follows the buffer, and
// ANISOTROPY STAYS 1.000 -- which is what makes this A3 done properly rather
// than A3 as originally written, where the viewport had to be dragged to match.
//
// The goal is moved with it. If the threshold is never met the goal is never
// read, but leaving a 1.6 goal behind a 1.0 threshold is a trap for whoever
// changes one of them next.
//
// Through the engine's own command buffer, like every other write here. Read
// back afterwards, because a convar that silently refused would look exactly
// like a letterbox we failed to explain.
// THE FLOOR IS FOR THE WORLD; THE MENU KEEPS THE ENGINE'S OWN LETTERBOX.
//
// Wearer, 2026-09-10, on the second headset: every launch's main menu was a
// 16:9 menu cropped to a near-square buffer, and after leaving a level the
// same menu drew small, centred, uncropped. On the Play For Dream MR at
// 5840x3648 (1.60, the engine's own floor) the menu was always right. The
// engine ships mat_letterbox_aspect_threshold 1.59 / goal 1.6 and letterboxes
// any squarer buffer -- the menu included, which is the small centred layout.
// This override lowers both below the buffer aspect so the WORLD fills the
// buffer; applied in the menu, it is also what crops the menu.
//
// So the override is applied only while a world is rendering, and the engine's
// own values -- read back once, before the first write, never typed -- are
// restored whenever there is no world. Each switch is written through the
// engine's command buffer and read back, and logged with the state it set.
float g_engineLetterboxThreshold = 0.0f;
float g_engineLetterboxGoal = 0.0f;
bool g_engineLetterboxRead = false;
int g_letterboxState = -1;   // -1 never written, 0 engine defaults (no world), 1 floor (world)

void ApplyLetterboxFloorIfRequested(bool worldUp) {
    float wanted = g_letterboxFloor.load(std::memory_order_acquire);
    if (wanted == 0.0f) return;                  // 0 = leave the game's own alone
    if (!GetModuleHandleA("engine.dll")) return; // try again next frame
    if (!g_engineLetterboxRead) {
        float t = 0.0f, g = 0.0f;
        if (TryReadCvarFloat("mat_letterbox_aspect_threshold", t) &&
            TryReadCvarFloat("mat_letterbox_aspect_goal", g) && t > 0.1f && g > 0.1f) {
            g_engineLetterboxThreshold = t;
            g_engineLetterboxGoal = g;
            g_engineLetterboxRead = true;
            char line[200]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] LETTERBOX: the engine's own values read once: threshold %g goal %g. These "
                "are restored whenever no world is rendering, so the menu keeps its layout.\n",
                static_cast<double>(t), static_cast<double>(g));
            Tf2VrLog(line);
        } else {
            return;                              // cvars not up yet; try again next frame
        }
    }
    // AUTO FLOOR. Negative means derive: a little below whatever this headset's
    // buffer aspect actually is, so the engine's letterbox can never trigger on
    // our own buffer while a world is up.
    if (wanted < 0.0f) {
        int dw = 0, dh = 0;
        if (DerivedRenderTarget(&dw, &dh, RenderScale()) && dh > 0) {
            wanted = (static_cast<float>(dw) / static_cast<float>(dh)) * 0.95f;
        } else {
            g_letterboxApplied = false;   // no geometry yet; try again next frame
            return;
        }
    }
    const int desired = worldUp ? 1 : 0;
    if (g_letterboxApplied && g_letterboxState == desired) return;
    g_letterboxApplied = true;
    g_letterboxState = desired;
    const float threshold = desired ? wanted : g_engineLetterboxThreshold;
    const float goal = desired ? wanted : g_engineLetterboxGoal;
    char command[80]{};
    std::snprintf(command, sizeof(command), "mat_letterbox_aspect_threshold %g\n",
                  static_cast<double>(threshold));
    const bool a = RunEngineConsoleCommand(command);
    std::snprintf(command, sizeof(command), "mat_letterbox_aspect_goal %g\n",
                  static_cast<double>(goal));
    const bool b = RunEngineConsoleCommand(command);
    float readT = 0.0f, readG = 0.0f;
    const bool rt = TryReadCvarFloat("mat_letterbox_aspect_threshold", readT);
    const bool rg = TryReadCvarFloat("mat_letterbox_aspect_goal", readG);
    char line[520]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] LETTERBOX %s: asked for threshold %g and goal %g (%s/%s); reads back threshold "
        "%s%.4f goal %s%.4f. %s\n",
        desired ? "FLOOR (world up)" : "ENGINE DEFAULTS (no world)",
        static_cast<double>(threshold), static_cast<double>(goal),
        a ? "sent" : "REFUSED", b ? "sent" : "REFUSED",
        rt ? "" : "(unreadable) ", static_cast<double>(readT),
        rg ? "" : "(unreadable) ", static_cast<double>(readG),
        desired ? "FALSIFIER: the world viewport must now equal the FULL buffer height."
                : "The menu should draw letterboxed -- small and centred, not cropped.");
    Tf2VrLogWrite(line, true);
}

void SetLetterboxFloor(float aspect) {
    g_letterboxFloor.store(aspect, std::memory_order_release);
    g_letterboxApplied = false;
}

// ---------------------------------------------------------------------------
// THE HEADSET CACHE. Per headset, because the alternative loses the wearer's
// setting every time they swap.
//
// Wearer: "When a headset is plugged in for the first time, or a user switches
// headsets, I want the default resolution to be exactly panel size."
//
// So a headset this build has never seen starts at scale 1.00 -- the runtime's
// own recommended per-eye size, exactly, nothing invented and nothing inherited
// from another device. And a headset it HAS seen comes back with whatever that
// headset was last set to, because a single shared scale would mean swapping to
// another headset and back would silently reset the Quest's 1.17 -- which is
// precisely the "config issue" this cache exists to avoid.
//
// NOTE WHAT "EXACTLY PANEL SIZE" CAN AND CANNOT MEAN. It fixes the HEIGHT to the
// recommended one. It cannot fix the width to the recommended width, because the
// buffer's shape is set by the LENS CONE and not by the panel's pixel aspect --
// 1.428 against 1.105 on this runtime. Rendering the panel's shape would squash
// the world. So: panel height exactly, width as the lenses demand.
//
// THE ORDERING PROBLEM this exists for is in render_resolution.h: the geometry
// arrives with the XR session, the resolution must be set before that session
// exists, and only the first mode change of a session renders. Learned this
// launch, used the next.
// ---------------------------------------------------------------------------
namespace {

constexpr int kMaxHeadsets = 8;
struct HeadsetEntry {
    char fingerprint[224];
    unsigned int recW, recH;
    float tanW, tanH;
    float scale;      // this headset's own resolution setting
    float fovScale;   // the derived cl_fovScale that lands its vertical FOV
};
HeadsetEntry g_headsets[kMaxHeadsets]{};
int g_headsetCount = 0;
int g_activeHeadset = -1;      // the one THIS launch was set up with
bool g_geometryChanged = false;
bool g_fovAuto = false;
bool g_derivedSetupApplied = false;
bool g_derivedSetupWanted = false;
int g_derivedSetupTicks = 0;
std::uint64_t g_derivedSetupFirstSeenMs = 0;
char g_cachePath[MAX_PATH]{};

bool ResolveCachePath() {
    if (g_cachePath[0]) return true;
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&ResolveCachePath), &self) || !self) {
        return false;
    }
    char path[MAX_PATH]{};
    if (!GetModuleFileNameA(self, path, MAX_PATH)) return false;
    char* slash = std::strrchr(path, 0x5C);
    if (!slash) return false;
    *(slash + 1) = 0;
    std::snprintf(g_cachePath, sizeof(g_cachePath), "%stitanfall2vr.headset.cache", path);
    return true;
}

int FindHeadset(const char* fingerprint) {
    if (!fingerprint || !*fingerprint) return -1;
    for (int i = 0; i < g_headsetCount; ++i) {
        if (std::strcmp(g_headsets[i].fingerprint, fingerprint) == 0) return i;
    }
    return -1;
}

void WriteHeadsetCache() {
    if (!ResolveCachePath()) return;
    char body[4096]{};
    int used = std::snprintf(body, sizeof(body),
        "# Written by titanfall2vr. One line per headset this build has seen.\n"
        "# The geometry is remembered because the resolution has to be set before\n"
        "# OpenXR can report it. `scale` is that headset's OWN resolution setting;\n"
        "# a headset seen for the FIRST time starts at the shipped default, which\n"
        "# is deliberately below the per-eye height the runtime asks for: the\n"
        "# widened buffer at 1.00 is about twice a 4K frame and no GPU check is\n"
        "# made. Raise it in the config menu if you have the headroom.\n"
        "# Delete this file to forget every headset.\n"
        "# fields: recW recH tanW tanH scale fingerprint\n"
        "active %d\n", g_activeHeadset);
    for (int i = 0; i < g_headsetCount && used > 0 && used < static_cast<int>(sizeof(body)); ++i) {
        used += std::snprintf(body + used, sizeof(body) - used,
                              "headset %u %u %.5f %.5f %.4f %.4f %s\n",
                              g_headsets[i].recW, g_headsets[i].recH,
                              static_cast<double>(g_headsets[i].tanW),
                              static_cast<double>(g_headsets[i].tanH),
                              static_cast<double>(g_headsets[i].scale),
                              static_cast<double>(g_headsets[i].fovScale),
                              g_headsets[i].fingerprint);
    }
    if (used <= 0) return;
    HANDLE file = CreateFileA(g_cachePath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    // A WRITE THAT CANNOT HAPPEN MUST SAY SO. This returned in silence, and
    // the consequence is invisible and permanent: with no cache, every launch
    // derives the resolution from scratch, which is the badly-magnified first
    // launch of KNOWN-ISSUES 8 -- every time, for ever. The usual cause is the
    // game sitting under Program Files, where this folder needs elevation.
    // Forced past the quiet filter: it is the difference between a one-line
    // diagnosis and an unanswerable report.
    if (file == INVALID_HANDLE_VALUE) {
        char line[440]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] headset cache: CANNOT WRITE %s (error %lu). Your headset will not be "
            "remembered, so every launch derives the resolution from scratch and the first "
            "minute looks badly magnified. The usual cause is the game being installed under "
            "Program Files, where this folder needs administrator rights.\n",
            g_cachePath, GetLastError());
        Tf2VrLogWrite(line, true);
        return;
    }
    DWORD written = 0;
    WriteFile(file, body, static_cast<DWORD>(used), &written, nullptr);
    CloseHandle(file);
}

}  // namespace

void NoteRenderScaleChosen(float scale) {
    // No headset learned yet: there is no row to record it in, and the next
    // LearnHeadsetGeometry will seed one from the shipped default anyway.
    if (g_activeHeadset < 0) return;
    // The same clamp SetRenderScale applies, so the cache can never hold a
    // value the setter would refuse.
    if (scale < 0.5f) scale = 0.5f;
    if (scale > 2.0f) scale = 2.0f;
    HeadsetEntry& entry = g_headsets[g_activeHeadset];
    if (std::fabs(entry.scale - scale) < 0.0005f) return;   // nothing moved
    const float was = entry.scale;
    entry.scale = scale;
    WriteHeadsetCache();
    char line[340]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] resolution %.0f%% -> %.0f%% for this headset, written to the cache so it "
        "survives a restart. The ini alone did not hold it: the cache is loaded after the "
        "config, deliberately, and wins over it.\n",
        static_cast<double>(was) * 100.0, static_cast<double>(scale) * 100.0);
    // FORCED. It fires only when the wearer actually changes the resolution,
    // so it is rare, and a release log that cannot show whether the change was
    // recorded is the reason this bug survived to be reported by hand.
    Tf2VrLogWrite(line, true);
}

void LoadHeadsetCache() {
    if (!ResolveCachePath()) return;
    HANDLE file = CreateFileA(g_cachePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Tf2VrLogWrite("[TF2VR] headset cache: EMPTY. No headset has been seen yet, so this launch "
                 "cannot derive a resolution -- it uses whatever render.width/height say, or the "
                 "game's own. The headset is learned when OpenXR reports it and the NEXT launch "
                 "starts at exactly its panel height.\n", true);
        return;
    }
    char body[4096]{};
    DWORD read = 0;
    ReadFile(file, body, sizeof(body) - 1, &read, nullptr);
    CloseHandle(file);
    body[read < sizeof(body) ? read : sizeof(body) - 1] = 0;

    for (char* line = std::strtok(body, "\r\n"); line; line = std::strtok(nullptr, "\r\n")) {
        if (line[0] == '#') continue;
        if (std::strncmp(line, "active ", 7) == 0) {
            std::sscanf(line, "active %d", &g_activeHeadset);
            continue;
        }
        if (std::strncmp(line, "headset ", 8) != 0) continue;
        if (g_headsetCount >= kMaxHeadsets) continue;
        HeadsetEntry& e = g_headsets[g_headsetCount];
        int consumed = 0;
        if (std::sscanf(line, "headset %u %u %f %f %f %f %n", &e.recW, &e.recH, &e.tanW, &e.tanH,
                        &e.scale, &e.fovScale, &consumed) != 6 || consumed <= 0) {
            continue;
        }
        std::snprintf(e.fingerprint, sizeof(e.fingerprint), "%s", line + consumed);
        ++g_headsetCount;
    }
    if (g_activeHeadset < 0 || g_activeHeadset >= g_headsetCount) {
        g_activeHeadset = g_headsetCount > 0 ? 0 : -1;
    }
    if (g_activeHeadset < 0) return;

    const HeadsetEntry& e = g_headsets[g_activeHeadset];
    // THE STORED SCALE IS THIS HEADSET'S, AND IT WINS OVER THE INI.
    //
    // The ini carries ONE render.scale for the whole install; the cache carries
    // one per headset. Loaded BEFORE the config, the ini overwrote it and a
    // headset swap restored nothing -- measured offline: cache 117%, ini 50%,
    // and 50% won. So this now runs after.
    //
    // A hand-edited ini value would therefore look dead, which is the exact
    // failure this project has paid for twice. So when the two disagree, say so
    // and say which one is in force.
    // game.fov_auto -- APPLY the derived cl_fovScale for THIS headset.
    //
    // Off by default, because it overrides a value the wearer may have tuned by
    // hand and the known-good profile carries 1.51. On, it is the only way a
    // second headset can be right: 1.51 lands a 90 degree vertical, and a Quest 3
    // wants 110. A hand-typed number is correct for exactly one headset.
    //
    // The value is the one the HEADSET banner has derived and logged as "NOT
    // APPLIED" since G0b, cached per headset because it needs XR to compute and
    // the game's FOV wants setting before the wearer is looking through it.
    if (g_fovAuto && g_headsets[g_activeHeadset].fovScale > 0.01f) {
        SetGameFovScale(g_headsets[g_activeHeadset].fovScale);
        char fov[320]{};
        std::snprintf(fov, sizeof(fov),
            "[TF2VR] game.fov_auto: cl_fovScale set to %.4f, DERIVED for this headset's vertical "
            "field of view rather than typed for another one.\n",
            static_cast<double>(g_headsets[g_activeHeadset].fovScale));
        Tf2VrLogWrite(fov, true);
    }
    const float fromIni = RenderScale();
    SetRenderScale(e.scale);
    char note[220]{};
    if (std::fabs(fromIni - e.scale) > 0.005f) {
        std::snprintf(note, sizeof(note),
            " NOTE: the ini asked for %.0f%%, but this headset's own stored setting wins. Change it "
            "in the config menu, or delete titanfall2vr.headset.cache to forget it.",
            static_cast<double>(fromIni) * 100.0);
    }
    char line[820]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] headset cache: %d known. This launch is set up for '%s' -- asks for %ux%u per "
        "eye, tangents %.5f x %.5f (aspect %.4f), resolution %.0f%%. The live headset is checked "
        "against this once OpenXR reports.%s\n",
        g_headsetCount, e.fingerprint, e.recW, e.recH, static_cast<double>(e.tanW),
        static_cast<double>(e.tanH), static_cast<double>(e.tanW / e.tanH),
        static_cast<double>(e.scale) * 100.0, note);
    Tf2VrLogWrite(line, true);
}

void LearnHeadsetGeometry(const char* fingerprint, unsigned int recommendedWidth,
                          unsigned int recommendedHeight, float coverTanX, float coverTanY,
                          float derivedFovScale) {
    if (!recommendedWidth || !recommendedHeight || coverTanY <= 0.0001f) return;
    if (!fingerprint || !*fingerprint) return;

    const int found = FindHeadset(fingerprint);
    const bool isNew = found < 0;
    const bool firstEver = g_activeHeadset < 0;
    int index = found;
    if (isNew) {
        if (g_headsetCount >= kMaxHeadsets) return;
        index = g_headsetCount++;
        std::snprintf(g_headsets[index].fingerprint, sizeof(g_headsets[index].fingerprint), "%s",
                      fingerprint);
        // NOTHING INHERITED FROM ANOTHER DEVICE. The wearer's rule for a headset
        // this build has never seen still holds: not the scale the headset that
        // was on a moment ago happens to be dialled to, and not a guess.
        //
        // IT IS THE SHIPPED DEFAULT, NOT 1.00, AND THAT MATTERS MORE THAN IT
        // LOOKS. This seed WINS OVER THE INI -- see ApplyHeadsetCache, "THE
        // STORED SCALE IS THIS HEADSET'S, AND IT WINS OVER THE INI". So a
        // hardcoded 1.0 here would silently override the shipped default for
        // every first-time user, which is precisely the population the default
        // exists for: the change would look applied and be inert.
        g_headsets[index].scale = kDefaultRenderScale;
    } else if (index == g_activeHeadset) {
        // Whatever was dialled this session belongs to the headset that was
        // actually on, which is this one.
        g_headsets[index].scale = RenderScale();
    }
    g_headsets[index].recW = recommendedWidth;
    g_headsets[index].recH = recommendedHeight;
    g_headsets[index].tanW = coverTanX;
    g_headsets[index].tanH = coverTanY;
    // The cl_fovScale that lands THIS headset's vertical. Computed since G0b and
    // logged "NOT APPLIED" ever since; a hand-typed value tuned for one headset
    // is simply wrong on another, which is half of "squashed, and a midget down
    // by the floor". Zero if it could not be derived this session, in which case
    // whatever was learned before stands.
    if (derivedFovScale > 0.01f) g_headsets[index].fovScale = derivedFovScale;

    g_geometryChanged = !firstEver && index != g_activeHeadset;
    g_activeHeadset = index;
    WriteHeadsetCache();

    // The restart line, and why it is not an apology. The resolution is set
    // before OpenXR can report, and only the first mode change of a session
    // renders anything -- measured across four runs. So a swap costs one restart
    // and the plugin says so rather than rendering the previous headset's shape
    // in silence.
    const char* restart = firstEver
        ? "It takes effect next launch."
        : "THIS SESSION IS STILL RENDERING THE PREVIOUS HEADSET'S SHAPE -- RESTART ONCE.";
    char line[760]{};
    if (isNew) {
        std::snprintf(line, sizeof(line),
            "[TF2VR] ***** NEW HEADSET: '%s' asks for %ux%u per eye, tangents %.5f x %.5f (aspect "
            "%.4f). Resolution defaulted to %.0f%% of its own panel height -- the shipped "
            "default, nothing inherited from another device. Raise it in the config menu if "
            "your GPU has the headroom. %s\n",
            fingerprint, recommendedWidth, recommendedHeight, static_cast<double>(coverTanX),
            static_cast<double>(coverTanY), static_cast<double>(coverTanX / coverTanY),
            static_cast<double>(kDefaultRenderScale) * 100.0, restart);
    } else if (g_geometryChanged) {
        std::snprintf(line, sizeof(line),
            "[TF2VR] ***** HEADSET SWITCHED to one already known: '%s', %ux%u per eye, and ITS OWN "
            "resolution %.0f%% is restored rather than the last headset's. %s\n",
            fingerprint, recommendedWidth, recommendedHeight,
            static_cast<double>(g_headsets[index].scale) * 100.0, restart);
    } else {
        std::snprintf(line, sizeof(line),
            "[TF2VR] headset confirmed: '%s', %ux%u per eye, resolution %.0f%%. Matches what this "
            "launch was set up with, so nothing needs a restart.\n",
            fingerprint, recommendedWidth, recommendedHeight,
            static_cast<double>(g_headsets[index].scale) * 100.0);
    }
    Tf2VrLog(line);
    // AND ASK FOR THE SETUP -- FROM THE GAME TICK, NOT FROM HERE.
    //
    // Calling ApplyDerivedSetupNow() inline here HUNG THE GAME. The log is
    // unambiguous: headset identified at 17875.6, letterbox floor set at 17876.1,
    // fov_scale set at 17876.1, then FORTY-SIX SECONDS of nothing and an access
    // violation at unknown+0x0 on another thread. It never reached the line that
    // reports the resolution applied, so it hung inside the mode change itself.
    //
    // My reasoning was that this is "the same live path the menu already uses".
    // The FUNCTION is the same; the CONTEXT is not. The menu applies from the
    // game tick, with an established session, on the game's own thread. This runs
    // from inside XR initialisation, mid-session-setup, and drives the engine's
    // console buffer from there. Same call, different world.
    //
    // So it is only REQUESTED here. The tick performs it once the game is
    // rendering a world, which is the context the menu change is known to work
    // in.
    g_derivedSetupWanted = true;
}

bool HeadsetGeometryChangedThisSession() { return g_geometryChanged; }

// render.min_aspect_floor, DEFAULT ON. Never hand the engine a buffer narrower
// than its own letterbox floor.
//
// The Play For Dream MR's panel is 1.60 wide per tall -- on the engine's floor
// (goal 1.6, threshold 1.59) -- so on that headset the engine never letterboxes,
// the 16:9 menu fits, the world fills, and none of the floor machinery is in
// play. A near-square panel (1.08, 2026-09-10) is below the floor, and there
// only two states exist: the engine's bars (a third of the height, the world
// warped in the layer) or the floor lowered (world fills, menu cropped at the
// sides). The engine takes those values at the video mode, so there is no
// mixing them per screen.
//
// So the derivation takes the wider of the lens ratio and the engine's own
// goal, read back from the engine and never typed. The buffer then sits on the
// floor exactly as the PFD MR's does, and the same regime follows on every
// headset. The projection layer already clamps the wider render to the lens.
// The price is pixels: 5222 wide instead of 3528 at 3264 tall; render.scale
// trades it back.
bool g_minAspectFloor = true;
void SetMinAspectFloor(bool on) {
    g_minAspectFloor = on;
    Tf2VrLog(on
        ? "[TF2VR] render.min_aspect_floor = 1: the derived buffer is never narrower than the "
          "engine's own letterbox goal, so the engine never letterboxes and the menu fits.\n"
        : "[TF2VR] render.min_aspect_floor = 0: the derived buffer follows the lens ratio alone; "
          "below the engine's floor the menu is cropped (KNOWN-ISSUES 12).\n");
}

bool DerivedRenderTarget(int* width, int* height, float scale) {
    if (width) *width = 0;
    if (height) *height = 0;
    if (g_activeHeadset < 0 || scale <= 0.0f) return false;
    const HeadsetEntry& e = g_headsets[g_activeHeadset];
    if (!e.recH || e.tanH <= 0.0001f) return false;
    // THE DERIVATION.
    //   height = what the runtime asks for  x  the wearer's scale
    //   width  = height x the headset's own LENS tangent ratio
    // Not the panel's pixel aspect. Those differ by a lot -- 1.105 against 1.428
    // on this runtime -- and using the panel's would render a squashed world.
    auto roundEven = [](float v) { int n = static_cast<int>(v + 0.5f); return n & 1 ? n + 1 : n; };
    const float h = static_cast<float>(e.recH) * scale;
    float aspect = e.tanW / e.tanH;
    // THE 1.6 FLOOR IS FOR THE ENGINE LETTERBOX, AND ONLY FOR IT.
    //
    // If the wearer has armed xr.letterbox_floor at or below the headset's
    // own lens ratio, the letterbox cannot fire, so there is nothing left
    // for the floor to protect and the buffer should follow the lenses.
    //
    // Read from the INI VALUE rather than the engine read-back: this runs at
    // startup from the cache, six seconds before the engine exists to be read,
    // so g_engineLetterboxGoal is still the stale 1.6 at this moment. That
    // ordering is why arming the floor cleared the cropping and did nothing
    // whatever to the frustum.
    const float armedFloor = g_letterboxFloor.load(std::memory_order_acquire);
    const bool letterboxDisarmed = armedFloor > 0.0f && armedFloor <= aspect;
    if (!letterboxDisarmed && g_minAspectFloor && g_engineLetterboxRead &&
        g_engineLetterboxGoal > aspect) {
        aspect = g_engineLetterboxGoal;
    }
    if (height) *height = roundEven(h);
    if (width) *width = roundEven(h * aspect);
    return true;
}

// The recommended per-eye size of the headset this launch was set up for, from
// the cache, for use before OpenXR has reported anything this session.
void CachedRecommendedEyeSize(unsigned int* width, unsigned int* height) {
    if (width) *width = 0;
    if (height) *height = 0;
    if (g_activeHeadset < 0) return;
    if (width) *width = g_headsets[g_activeHeadset].recW;
    if (height) *height = g_headsets[g_activeHeadset].recH;
}

void SetFovAuto(bool enabled) { g_fovAuto = enabled; }
bool FovAuto() { return g_fovAuto; }

// ---------------------------------------------------------------------------
// SET IT UP THE MOMENT THE HEADSET IS KNOWN. No second launch.
//
// Wearer: "If you are proposing that when I launch this mod, a new user will
// install the mod and have to launch twice? That is show stopper."
//
// Correct, and it was never actually necessary. The two-launch design rested on
// this file's own oldest rule -- "applied ONCE and EARLY, before OpenXR arms,
// because a video mode change recreates the swapchain" -- which is contradicted
// forty lines below it by ForceRenderResolutionNow: "safe to call live because a
// windowed mode change RESIZES the swapchain rather than replacing it, so
// pointer identity holds and the XR submission path keeps its adopted
// swapchain". The wearer's own testing settles which is true: "resolution
// changes made immediately when exiting the config menu are PERFECT", with an XR
// session running.
//
// So the ordering problem dissolves. We do not have to know the headset before
// XR starts; we have to know it before the wearer is looking at the world, and
// XR reports it seconds earlier than that. A first-ever launch now starts at
// whatever the ini seeds, and snaps to the derived setup as soon as the runtime
// says what is connected -- the same live path, on the same frame budget, as the
// menu change that already works.
//
// The cache is now an OPTIMISATION rather than a requirement: with it, launch
// one is correct from the first frame and nothing snaps. Without it, launch one
// is correct a few seconds in. Neither needs a restart.
//
// WINDOWED ONLY, because that is the path documented and measured safe. A
// fullscreen mode change replaces the swapchain rather than resizing it, and the
// XR pipeline is built around an adopted one. In fullscreen this defers to the
// cache and says so rather than gambling with a live session.
bool g_fovScaleAppliedLate = false;

void ApplyDerivedSetupNow() {
    if (!g_derivedSetupWanted) return;
    if (g_derivedSetupApplied) {
        // The resolution was applied in the menu, before the game's frustum
        // existed, so the FOV scale was unknown then. The full banner stores it
        // once a world renders; apply it here, from the tick, exactly once.
        if (g_fovAuto && !g_fovScaleAppliedLate && g_activeHeadset >= 0) {
            const float late = g_headsets[g_activeHeadset].fovScale;
            if (late > 0.01f) {
                g_fovScaleAppliedLate = true;
                SetGameFovScale(late);
                Tf2VrLogWrite("[TF2VR] derived setup: FOV scale applied LATE, from the tick, now that the "
                         "game's frustum is known (the resolution itself was applied in the menu).\n", true);
            }
        }
        return;
    }
    if (g_activeHeadset < 0) return;
    if (!GetModuleHandleA("engine.dll")) return;
    // CLEAR OF XR INITIALISATION. The inline version hung the game by driving a
    // mode change from inside session setup; this waits for a settled world, the
    // same context the menu change works in. A handful of ticks past that,
    // because "the world is up" and "the frame loop is steady" are not the same
    // instant and a mode change wants the second one.
    // THE SESSION MUST BE ESTABLISHED -- the actual precondition, stated.
    //
    // The hang this function's history warns about was a mode change driven from
    // INSIDE XR session setup: headset identified, then 46 seconds of nothing and
    // an access violation on another thread. The old gate approximated "not
    // mid-setup" by waiting for GameIsRenderingAWorld() -- true, but far too
    // late. It put the mode change ~25 seconds into the first session, after the
    // UI had bound its abilities, and a live video mode change there is exactly
    // KNOWN-ISSUES 1: the ability buttons go UNBOUND and stay so until a restart.
    // EVERY new player hit that, because a first run has no headset cache and so
    // always derives.
    //
    // Asking whether the session is RUNNING says the same thing precisely, and
    // says it during the loading screen instead -- before the UI has bound
    // anything, so there is nothing to rebuild and no failure to cache.
    //
    // The counter resets while the session is not up, so the settling delay is
    // measured from a running session rather than from the first call.
    if (!IsXrPresentingToHeadset()) { g_derivedSetupTicks = 0; return; }
    // AS SOON AS THE RUNTIME HAS TOLD US THE PANEL SIZE. NOT WHEN A WORLD IS UP.
    //
    // The panel size is known at ~6.5 s -- "view config 0: recommended
    // 4032x3648" -- and OpenXR is armed and presenting before the game's logo.
    // This used to wait for GameIsRenderingAWorld() and so applied at ~46 s on a
    // new campaign, because the opening cutscene means no world exists until
    // 44 s. Forty-five seconds of the game rendering 2560x1440 into a headset
    // expecting 4032x3648, which is the magnified crop the wearer could not see
    // the cutscene through -- and then a live mode change mid-session, which is
    // KNOWN-ISSUES 1 and takes the ability buttons with it.
    //
    // THE WORLD GATE WAS NEVER THE REQUIREMENT. The requirement is "not from
    // inside XR initialisation", which is what hung the game for 46 seconds when
    // this was called inline from there. A call on the GAME TICK is outside that
    // by construction, whether or not a world exists -- the tick is the same
    // context the menu's own resolution change works in.
    //
    // So the only wait left is a short settle after the headset is known, to be
    // clear of session setup rather than to wait for anything in the game.
    //
    // MEASURED IN MILLISECONDS, NOT TICKS, and that distinction cost a run. A
    // 60-tick settle sounds like a second and was TWENTY-ONE: the panel size is
    // known at 7.2 s and the change landed at 28.2 s, because during a loading
    // screen this tick runs at a few hertz, not sixty. A tick count is a
    // duration only if you know the rate, and here the rate is at its lowest
    // exactly when this needs to fire.
    if (!g_derivedSetupFirstSeenMs) g_derivedSetupFirstSeenMs = GetTickCount64();
    if (GetTickCount64() - g_derivedSetupFirstSeenMs < 750) return;
    ++g_derivedSetupTicks;

    const HeadsetEntry& e = g_headsets[g_activeHeadset];
    int wantW = 0, wantH = 0;
    if (!DerivedRenderTarget(&wantW, &wantH, e.scale) || wantW <= 0 || wantH <= 0) return;
    g_derivedSetupApplied = true;

    if (!g_requestedWindowed) {
        char line[420]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] derived setup for this headset is %dx%d, but the game is FULLSCREEN and a "
            "fullscreen mode change replaces the swapchain instead of resizing it. Not applied "
            "live; it is cached and the next launch starts there. Set render.windowed = 1 to have "
            "it applied immediately.\n",
            wantW, wantH);
        Tf2VrLogWrite(line, true);
        return;
    }

    // The floor before the buffer: the letterbox decision is made against the
    // buffer aspect, so a floor that is still the previous headset's would fire
    // on the new shape for however long it took to set them in the other order.
    if (g_letterboxFloor.load(std::memory_order_acquire) != 0.0f) {
        g_letterboxApplied = false;
        ApplyLetterboxFloorIfRequested(true);   // the floor is always on; see plugin.cpp
    }
    if (g_fovAuto && e.fovScale > 0.01f) { SetGameFovScale(e.fovScale); g_fovScaleAppliedLate = true; }

    const bool alreadyRight = wantW == g_requestedWidth && wantH == g_requestedHeight;
    if (alreadyRight) {
        char line[380]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] derived setup: already at %dx%d for this headset, nothing to change. The "
            "cache had it right before OpenXR reported, so there is no snap.\n",
            wantW, wantH);
        Tf2VrLog(line);
        return;
    }
    // SAID BEFORE THE CALL, NOT AFTER. The inline version hung inside this and
    // the log went silent for 46 seconds with nothing naming what it was doing.
    // The log is unbuffered, so this line is on disk before the call runs: if the
    // next hang looks the same, this says the exact size it hung on.
    {
        char about[300]{};
        std::snprintf(about, sizeof(about),
            "[TF2VR] derived setup: about to apply %dx%d windowed for this headset. If the log stops "
            "here, the mode change is the hang.\n", wantW, wantH);
        Tf2VrLogWrite(about, true);
    }
    const int wasW = g_requestedWidth, wasH = g_requestedHeight;
    if (ForceRenderResolutionNow(wantW, wantH, g_requestedWindowed)) {
        g_requestedWidth = wantW;
        g_requestedHeight = wantH;
        char line[460]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] derived setup APPLIED LIVE: %dx%d per eye for this headset (was %dx%d), "
            "after %d ticks of a running XR session. WHERE THIS LINE SITS RELATIVE TO THE "
            "AUTO-ARM WORLD LINE IS THE TEST: before it means the mode change landed during the "
            "loading screen, before the UI bound its abilities, so KNOWN-ISSUES 1 cannot fire. "
            "After it means this was late and the ability buttons will read UNBOUND until a "
            "restart.\n",
            wantW, wantH, wasW, wasH, g_derivedSetupTicks);
        Tf2VrLog(line);
    }
}

// `profile` from the ini, stored rather than only announced. See the note at
// g_flatProfile for the run this cost.
void SetFlatProfile(bool flat) { g_flatProfile = flat; }
bool IsFlatProfile() { return g_flatProfile; }

// Once per plugin frame. Emits the deferred AFTER sample from
// ForceRenderResolutionNow, so the two readings bracket the material-system
// restart rather than both landing before it.


// ---------------------------------------------------------------------------
// RESTORE THE BINDINGS DIRECTLY. res.rebind, CTRL+F2.
//
// Enough archaeology. What is established, and none of it needs the mechanism
// to be named:
//
//   - a real video mode change destroys and recreates the UI Squirrel VM;
//   - the bindings that die are A and Y, whose commands take an ARGUMENT
//     (+ability N), while every argument-less command survives;
//   - the game sometimes restores them by itself and usually does not, which
//     is a race and means waiting is not a fix;
//   - A_BUTTON / X_BUTTON / Y_BUTTON are real key tokens in client.dll, so a
//     gamepad binding is expressible to the engine's own `bind` command.
//
// So this simply re-issues them. If the bindings come back, the repair is
// settled and it becomes automatic after every mode change -- and the question
// of WHY the engine drops them stops being on the critical path.
//
// THE COMMANDS ARE THE GAME'S OWN DEFAULTS, from r2/cfg/config_default_pc.cfg:
// jump is `+ability 3` and the titan mode switch is `+ability 1`. If the wearer
// has rebound either, this puts the DEFAULT back rather than what they chose --
// which is why it is a key to be tried, not a fix to be shipped, until a run
// says the values are right.
// ---------------------------------------------------------------------------

