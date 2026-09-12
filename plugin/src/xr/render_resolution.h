#pragma once

// Sets the game's render resolution from code, because it cannot be raised
// from where it matters otherwise.
//
// The game renders one frame per eye under alternate-frame stereo, so the
// backbuffer IS the per-eye resolution. It was running windowed at 2560x1440
// on a 4K display, for an eye the runtime asks 4032x3648 for. Measured, that
// image is about 2.06x short across and 2.67x short down, and no arrangement
// of the frustum fixes a shortfall in pixels -- the headset-FOV match was
// built, measured, and could not be seen for exactly this reason.
//
// Applied through the engine's own console buffer (Cbuf_AddText /
// Cbuf_Execute, resolved from engine.dll), so this is the same path typing
// mat_setvideomode into the console would take. Nothing is patched.
//
// Applied ONCE and EARLY -- before OpenXR arms -- because a video mode change
// recreates the swapchain, and the whole XR pipeline is built around an adopted
// one. Changing it underneath a live session is a different and much larger
// problem than setting it before there is a session at all.
void ApplyRenderResolutionIfRequested();

// 0 in either dimension leaves the game's own setting alone, which is the
// default: this writes to the engine, so it stays opt-in.
void SetRequestedRenderResolution(int width, int height);
void SetRequestedRenderWindowed(bool windowed);

// True once the resolution has been applied, or once it has been established
// that none was asked for. Auto-arm waits on this so the XR swapchain is never
// adopted at a size that is about to change.
bool RenderResolutionSettled();

// Runs one console command through the engine's own command buffer -- exactly
// the path typing it into the console would take. The three Cbuf offsets are
// verified as code before anything is called, so a build these offsets do not
// match refuses rather than calling into the middle of an unrelated function
// with a string argument.
//
// Lives here because this is where those offsets were first resolved and
// verified. Returns false if they did not verify or the engine is not loaded,
// and says so in the log the first time.
bool RunEngineConsoleCommand(const char* command);

// What the ini asked for, so a temporary change can be put back exactly.
// Zero width or height means the ini asked for nothing and the game's own
// setting is in force -- in which case there is nothing to restore TO, and a
// caller must leave the resolution alone rather than guess.
int RequestedRenderWidth();
int RequestedRenderHeight();
bool RequestedRenderWindowed();

// A resolution change OUTSIDE the one-shot startup path, for the scripted flat
// session: the shipped 4032x2268 is far larger than the monitor, so at a desk
// only the top-left corner of the frame is visible and the weapon -- which sits
// low and right of centre -- is off-screen entirely. Windowed only, and the
// session puts the original back when it finishes or aborts.
//
// Safe to call live because a windowed mode change RESIZES the swapchain rather
// than replacing it, so pointer identity holds and the XR submission path keeps
// its adopted swapchain (see IsVerifiedGameSwapchain).
bool ForceRenderResolutionNow(int width, int height, bool windowed);

// ---------------------------------------------------------------------------
// render.scale -- THE SINGLE RESOLUTION CONTROL, and the only one the menu
// offers.
//
// 1.00 means "render exactly what the headset asked for per eye". The width and
// height are BOTH derived from the runtime's recommendation times this one
// number, so the aspect ratio can never be changed by accident -- which it
// could when render.width and render.height were two independent controls, and
// a wrong aspect is a stretched world rather than an obviously wrong setting.
//
// render.width / render.height remain as an absolute override for anyone who
// really wants one, and they WIN when both are set: an explicit pair of numbers
// is a clearer statement of intent than a multiplier. They are not in the menu.
//
// Takes effect on the next launch, like every other resolution change here.
// THE SHIPPED DEFAULT RENDER SCALE. Read by three places that must never
// disagree: the g_renderScale initialiser, the seed for a headset this build
// has never seen (which WINS over the ini), and the registry row the settings
// panel shows. Changing it in one place only is how a default becomes inert.
constexpr float kDefaultRenderScale = 0.75f;

// THE WEARER DELIBERATELY CHOSE THIS SCALE, FOR THE HEADSET THAT IS ON.
//
// SetRenderScale applies a value. This RECORDS one, which is a different
// thing and the reason a panel edit did not survive a restart: the panel
// wrote the ini, and LoadHeadsetCache is loaded AFTER the config on purpose
// and its stored per-headset scale wins over the ini. So the edit applied,
// persisted to the ini, and was then silently overridden on the next launch
// by a cache nothing had updated.
//
// Call this from the paths that represent a CHOICE -- not from the paths that
// merely apply one, or the ini would start winning again and the per-headset
// value this cache exists for would be lost on every launch.
void NoteRenderScaleChosen(float scale);

void SetRenderScale(float scale);
float RenderScale();
// The size render.scale currently implies: the runtime's recommendation times
// the scale, rounded to even. Zero if XR has not reported a recommendation yet.
void ScaledRenderTarget(int* width, int* height);

// `profile` from the ini: 1 = FLAT (monitor), 2 = VR. Stored, not only
// announced, because the render target has to know. A flat launch has no
// headset, and the per-headset resolution cache is loaded unconditionally --
// so without this a monitor run sizes its window from whichever headset was
// last worn. It did: 5840x3648 asked, 17% of the screen visible, run lost.
void SetFlatProfile(bool flat);
bool IsFlatProfile();
// The runtime's recommended per-eye HEIGHT -- what render.scale is a multiple
// of. Height rather than pixel count, because height is the axis the headset and
// the render agree on: the width is set by the game's field of view.
unsigned int ReferenceRenderHeight();

// What the CURRENT resolution already amounts to, in headset-panel terms:
// delivered per-eye width over the runtime's recommended width. Used so the
// menu's resolution slider starts where the wearer actually is instead of at a
// bare 0, which reads as broken. Returns 1.0 when nothing is known.
float EffectiveRenderScale();

// The per-eye PANEL size a given scale would deliver, for the menu to show while
// the slider moves. Panel pixels, not the internal buffer.
void PanelResolutionForScale(float scale, int* width, int* height);
// The recommended per-eye size of the headset this launch was set up for, from
// the cache, for the menu to use before OpenXR has reported this session.
void CachedRecommendedEyeSize(unsigned int* width, unsigned int* height);
// `game.fov_auto`. Applies the per-headset DERIVED cl_fovScale instead of a
// hand-typed one. Off by default: a typed value is right for exactly one
// headset, and that headset might be the one in the ini.
void SetFovAuto(bool enabled);
bool FovAuto();
// render.min_aspect_floor, DEFAULT ON: the derived buffer is never narrower
// than the engine's own letterbox goal, so the engine never letterboxes.
void SetMinAspectFloor(bool on);
// Applies this headset's derived setup -- resolution, FOV scale and letterbox
// floor -- the moment OpenXR has said which headset is connected, so a first
// install does NOT need a second launch. Windowed only; see the note at the
// definition. Runs at most once per session.
void ApplyDerivedSetupNow();

// ---------------------------------------------------------------------------
// `xr.letterbox_floor` — THE 1.600 FLOOR, WHICH IS A CONVAR AND NOT A CONSTANT.
//
// A1 measured the world pass obeying `height = min(bufferH, bufferW / 1.600)`
// at seven buffer aspects, to the pixel, and concluded that collecting the
// wasted ~10% needed that constant found in the disassembly. It is not a
// constant. `materialsystem_dx11.dll` registers it, with its own help text:
//
//   mat_letterbox_aspect_threshold  "1.59"   "Letterbox when the window aspect
//   mat_letterbox_aspect_goal       "1.6"     ratio is below this threshold"
//
// Those two defaults reproduce all seven measured rows exactly.
//
// Set this BELOW the buffer aspect and the letterbox never triggers: the world
// pass fills the buffer, the frustum still follows the buffer, and anisotropy
// stays 1.000. That is what makes it A3 done properly rather than A3 as
// written, which would have dragged the viewport to match the frustum and broke
// the invariant last time it was tried.
//
// 0 (the default) leaves the game's own values alone.
void SetLetterboxFloor(float aspect);
// Applied once, early, through the engine's command buffer, and read back. Must
// run BEFORE the startup resolution is applied: only the first mode change of a
// session renders, so the buffer and the floor have to be right together.
// worldUp: the floor is applied only while a world renders; otherwise the
// engine's own letterbox values are restored so the menu keeps its layout.
void ApplyLetterboxFloorIfRequested(bool worldUp);

// ---------------------------------------------------------------------------
// THE HEADSET GEOMETRY CACHE, and the ordering problem that forces it.
//
// The wearer owns several headsets and wants to switch freely. Every pixel count
// should therefore be DERIVED: height = the runtime's recommended per-eye
// height x render.scale, width = height x the headset's own tangent ratio.
//
// THE PROBLEM: the resolution has to be applied EARLY, before OpenXR arms (see
// ApplyRenderResolutionIfRequested above -- a mode change recreates the
// swapchain and the XR pipeline is built around an adopted one). But the
// headset's FOV comes from xrLocateViews, which needs a running session, which
// needs the adopted swapchain. The two inputs are on opposite sides of the same
// door, and it is not a door we can open twice: measured across four runs, only
// the FIRST live mode change of a session renders anything at all.
//
// So the geometry is LEARNED at XR init and USED at the next startup. Keyed by
// HeadsetFingerprint(), so each headset keeps its own entry and a runtime that
// reports a generic name still matches itself.
//
// Switching headsets therefore costs ONE restart, and the plugin says so out
// loud rather than rendering the previous headset's shape in silence. That is
// the honest cost of a single-shot mode change; it is not a limitation anyone
// gets to design away.
//
// Learned from XR once the geometry is known. Writes the cache file beside the
// ini. Safe to call every session; it only writes when something changed.
void LearnHeadsetGeometry(const char* fingerprint, unsigned int recommendedWidth,
                          unsigned int recommendedHeight, float coverTanX, float coverTanY,
                          float derivedFovScale);
// True if the geometry now reported differs from what this launch was set up
// with -- i.e. the wearer swapped headsets since the last run.
bool HeadsetGeometryChangedThisSession();

// Reads the cache file beside the dll. Call once, early, before the resolution
// is applied.
void LoadHeadsetCache();
// The size render.scale implies from the CACHED headset geometry, for when the
// ini names no explicit render.width/height. False until a headset has been seen
// once.
bool DerivedRenderTarget(int* width, int* height, float scale);
