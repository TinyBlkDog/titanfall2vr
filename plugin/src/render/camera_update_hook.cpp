#include "camera_update_hook.h"
#include "frustum_census.h"
#include "scene_reentry.h"
#include "render_resolution.h"
#include "stereo_targets.h"

// SAME-FRAME STEREO. Defined near the other public accessors at the end of this
// file; declared at GLOBAL scope here because the mid-frame boundary inside the
// anonymous namespace calls it, and a declaration written in there would name a
// different function.
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
void CaptureSameFrameEye0(ID3D11DeviceContext* context, ID3D11Texture2D* source);
bool SameFrameInSecondPass();

#include "asym_projection.h"
#include "world_rect_gate.h"
#include "scan_outcome.h"
#include "ads_lock.h"
#include "ads_probe.h"
#include "camera_hook.h"
#include "placement_pin.h"
#include "ads_zoom.h"

#include "diagnostics.h"
#include "hook_registry.h"
#include "resource_watch.h"
#include "plugin_cost.h"
#include "weapon_settings.h"
#include "viewmodel_bones.h"
#include "bone_staging_probe.h"
#include "placement_watchpoint.h"
#include "xr_input.h"
#include "aim_cmd.h"

#include <d3d11.h>
#include <d3d11_1.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
using UpdateSubresourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                                     const D3D11_BOX*, const void*, UINT, UINT);
// ID3D11DeviceContext1::UpdateSubresource1 -- the SAME upload with a trailing
// copyFlags. A different method, not another implementation of the one above.
//
// The counters say the slot-48 hook is called on the right context every time
// and still sees only about 2% of the traffic: 1.1 calls per frame against the
// 48 per frame the passive trace measured, with 35 of those camera-sized. The
// engine is D3D11.1-era and is doing its uploads through the 11.1 entry; slot
// 48 catches only whatever still comes in by the legacy one.
using UpdateSubresource1Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                                      const D3D11_BOX*, const void*, UINT, UINT, UINT);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);

constexpr size_t kDrawIndexedVtableIndex = 12;
// ID3D11DeviceContext::RSSetViewports. Same table that puts DrawIndexed at 12
// and UpdateSubresource at 48, both already confirmed against this build.
constexpr size_t kRSSetViewportsVtableIndex = 44;
constexpr size_t kUpdateSubresourceVtableIndex = 48;
// ID3D11DeviceContext has 7 inherited entries (IUnknown + ID3D11DeviceChild)
// and 108 of its own, so ID3D11DeviceContext1's additions start at 115:
// CopySubresourceRegion1 = 115, UpdateSubresource1 = 116.
constexpr size_t kUpdateSubresource1VtableIndex = 116;
// Unlike Draw/DrawIndexed, this vtable slot points at the real d3d11.dll
// implementation, not a forwarding thunk.  Its prologue is ten
// position-independent instructions ending at offset 20; offset 13 is the
// nearest earlier boundary and is too short for the 14-byte absolute jump, so
// the whole prologue is displaced.  The first RIP-relative instruction begins
// at 20 and stays at its original address.
constexpr size_t kDisplacedBytes = 24;  // buffer size: the widest variant is 23
constexpr size_t kTrampolineBytes = 64;
constexpr UINT kCameraBufferBytes = 576;
constexpr UINT kTraceFrames = 2;
// Long enough to look around and judge the change, short enough that a bad
// outcome ends on its own without needing a keypress.
constexpr UINT kOffsetTestFrames = 600;
// Deliberately far larger than an interpupillary distance: this first proof
// only has to be unmistakable on screen.
constexpr float kOriginOffsetX = 120.0f;
// Source-derived units are ~1 inch, so a 64 mm interpupillary distance is
// about 2.5 units and each eye sits half that off centre.  Config-tunable so an
// IPD sweep does not need a rebuild.
constexpr float kDefaultHalfInterpupillaryUnits = 1.26f;
std::atomic<float> g_halfInterpupillaryUnits = kDefaultHalfInterpupillaryUnits;
// Set when the config names a value, so the runtime's own IPD does not silently
// overwrite a deliberate choice.
std::atomic_bool g_halfIpdExplicit = false;
// The runtime's real half eye separation in metres. Kept even when the config
// overrides the rendering value, because it is what converts units to metres.
std::atomic<float> g_runtimeHalfIpdMetres = 0.0f;
constexpr size_t kMaxRecords = 128;

// RenderDoc and 3Dmigoto both fix these offsets in CBufCommonPerCamera.
constexpr size_t kCameraOriginOffset = 4;
constexpr size_t kCameraRelativeToClipOffset = 16;
constexpr size_t kMatrixElement15Offset = kCameraRelativeToClipOffset + 15 * sizeof(float);

struct UpdateRecord {
    std::uint32_t frame;
    std::uint32_t ordinal;
    const void* destination;
    std::uint64_t contentHash;
    std::uint64_t matrixHash;
    float origin[3];
    float matrixRow3[4];
    // r0.xyz carries the projection scale 1/(aspect*tan(fov/2)), so its length
    // is a direct read on the FOV this upload renders with. The main scene and
    // the viewmodel share the player origin and cannot be told apart by
    // position, but a weapon-FOV pass differs here. This is the discriminator
    // the viewmodel counter-rotation needs.
    float row0Length;
    // |r1| is 1/tan(fovY/2) by the same reasoning, with no aspect term. Task 02
    // has to make the game's frustum agree with the headset's per-eye FOV, and
    // the horizontal reading alone cannot give both angles without assuming an
    // aspect ratio. Recorded passively; nothing reads it yet.
    float row1Length;
    // Row 2 is the depth row: it encodes the near/far planes. The player-origin
    // uploads split into two distinct matrices with identical |r0| (same FOV)
    // and the same forward axis, so whatever separates them is not projection
    // width. A viewmodel pass classically shares FOV and orientation with the
    // world but uses its own near plane so the weapon does not clip through
    // walls, which would show up here and nowhere else.
    float matrixRow2[4];
    // A partial (boxed) update cannot be read as a whole camera buffer; it is
    // still recorded so a boxed camera write cannot masquerade as "no calls".
    bool partial;
    UINT boxLeft;
    UINT boxRight;
    // Running DrawIndexed total at the moment of this upload.  Differences
    // between consecutive records give the draw burst each upload precedes,
    // which is what identifies the main scene pass.
    std::uint32_t drawsAtUpdate;
};

// The two hooked functions have different entry shapes, so each detour
// carries the number of bytes it displaced.
struct EntryDetour {
    void* target = nullptr;
    void* trampoline = nullptr;
    size_t displaced = 0;
    std::uint8_t original[kDisplacedBytes]{};
};

// The passive trace is driven by Present boundaries, not by plugin frames.
// RunFrame is dispatched faster than the render loop, so a budget of "2 plugin
// frames" expired inside a single render frame: two captures in a row recorded
// only the tail of a frame, from upload #6 onward, and so contained no
// main-scene upload and about a fifth of the draws. Both were misread as menu
// captures. Recording now starts at a frame boundary and lasts whole frames.
std::atomic_bool g_tracePendingStart = false;
std::atomic_uint32_t g_traceFramesRemaining = 0;

EntryDetour g_detour;
// The SECOND UpdateSubresource implementation.
//
// This context's vtable is heap-allocated and sits at context+8 -- a per-object
// dispatch table, not the module's shared one -- and D3D11 swaps slot 48
// between two implementations of this call at runtime. Patching only the one
// present at install time produced exactly eleven corrected uploads and then
// nothing, because the game moved to the other entry: the weapon was corrected
// on a handful of frames and left alone on the rest, which is the reported
// flashing.
//
// Both entries are therefore detoured, each with its own trampoline, and slot
// 48 is re-checked every presented frame so a swap to a third variant is picked
// up rather than silently dropping the correction again.
EntryDetour g_detourAlt;
EntryDetour g_detour1;
// The REAL implementation, behind the thunks.
//
// Disassembling the entries this context's table actually holds settles what
// three runs of counters could only hint at. Both of them --
//
//     lea rbx, [rcx-0D8h]   ;  mov rcx, rbx  ;  call <internal>
//
// -- are adjustor thunks, the same shape the shared context says Draw and
// DrawIndexed have and UpdateSubresource does not. The function we hooked in
// the sessions where the camera path demonstrably worked is a different one:
// a large routine with a stack cookie, sitting 0x110 bytes past the thunk. That
// is the implementation, and it is what sees all ~35 camera uploads a frame.
//
// The implementation is laid out immediately after its own thunk, so it is
// found by scanning forward from the thunk for the next push-heavy prologue
// rather than by scanning the module blind -- that pattern has eight matches in
// d3d11.dll and picking one of those would be a guess.
EntryDetour g_detourImpl;
UpdateSubresourceFn g_originalImpl = nullptr;
EntryDetour g_drawDetour;

// WHAT THE LETTERBOX ACTUALLY IS.
//
// Asking for a 4032x3648 backbuffer produced 2268 rows of content with 690
// black rows at each end -- the game refusing to render taller than 16:9. That
// refusal has to be implemented somewhere, and a viewport smaller than the
// render target is the ordinary way to do it.
//
// If that is what this records, the fix is not to accept 16:9 and stretch a
// frustum into it -- that trades square pixels for anisotropic ones, 36.7 px
// per degree across against 25.2 down, which is the stretch and not a fix. The
// fix is to widen the viewport back to the full target and set the projection
// to match, which is a genuine taller render at square pixels.
//
// Recording only, for now. The distinct rectangles a frame uses are the whole
// question: one full-target viewport means the letterbox is elsewhere and this
// theory is dead.
EntryDetour g_viewportDetour;
using RSSetViewportsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
// Raised by the substituted RUI draw slot in rui_probe.cpp for the span of one
// lower-left widget's draw. It is what splits the two viewport populations,
// and what attributes a Map call to that widget rather than to the two
// million others.
extern "C" volatile long g_ruiInDraw;

// Two populations, tracked apart: viewports set while the HUD widget is
// drawing, and everything else. See the note in HookRSSetViewports.
struct ViewportRange {
    float loX, hiX, loY, hiY, loW, hiW, loH, hiH;
    unsigned long long count;
    bool seen;
};
ViewportRange g_vpInHud{};
ViewportRange g_vpElsewhere{};

RSSetViewportsFn g_viewportOriginal = nullptr;
std::atomic_uint32_t g_viewportTraceFrames = 0;
struct SeenViewport {
    float topLeftX, topLeftY, width, height;
    std::uint32_t count;
};
constexpr size_t kMaxSeenViewports = 12;
SeenViewport g_seenViewports[kMaxSeenViewports]{};
size_t g_seenViewportCount = 0;
ID3D11DeviceContext* g_gameContext = nullptr;
UpdateSubresourceFn g_original = nullptr;
UpdateSubresourceFn g_originalAlt = nullptr;
UpdateSubresource1Fn g_original1 = nullptr;
// The swapped vtable slot and what was in it, so teardown puts back exactly
// what it took rather than assuming the entry detour owns this entry.
void** g_slot1 = nullptr;
void* g_slot1Original = nullptr;
DrawIndexedFn g_drawOriginal = nullptr;
std::atomic_uint32_t g_drawIndexedCount = 0;
// M1's SEPARATE draw tally, and it exists because g_drawIndexedCount above is
// not usable outside a bounded camera trace: HookDrawIndexed only increments it
// while g_enabled is set, and g_enabled is set only between BeginCameraUpdate-
// Trace and its frame budget running out. A census run arms no trace, so every
// per-span draw count taken from that counter would be a fake zero -- exactly
// the failure that disqualified three upload counters this week.
//
// This one is armed by the render-target census and by nothing else, so it is
// live for precisely the run that reads it.
std::atomic_bool g_censusDrawTallyArmed{false};
std::atomic_uint64_t g_censusDraws{0};

// ---------------------------------------------------------------------------
// M3 -- THE CAMERA KEY, B2. `stereo.batch2_ipd`.
//
// The design preferred B1 (write the engine's camera source before the nested
// call and let it bake) and named B2 -- patch batch 2's uploads on the render
// thread -- as the fallback, for one stated reason: no predicate existed that
// the render thread could evaluate. M1 found one, M2 has leaned on it 509 times
// without a misidentification, and the replan's own rule is that B1 goes first
// only in its absence. So this is B2, and it writes no engine struct at all:
// it adds an IPD to c_cameraOrigin in OUR OWN COPY of an upload the engine is
// already making, on the calls the discriminator attributes to batch 2.
//
// The offset is along row0 of c_cameraRelativeToClip, which this file already
// verifies to be the unit-length camera RIGHT axis. That makes it a true eye
// translation rather than a world-axis nudge, and it comes entirely from the
// buffer being uploaded -- no global, no other clock.
std::atomic_bool g_m3Armed{false};

// ---------------------------------------------------------------------------
// Set while M5 reissues the camera buffer itself, so the upload hook forwards
// our own write untouched instead of applying the eye translation to it a
// second time.
thread_local bool t_preDrawWriteInProgress = false;

// M5's PRE-DRAW CAMERA WRITE, and why the batch tag was never going to work.
//
// The span census settled it: BOTH batches upload their world camera AFTER
// their own heavy scene span. Batch 1's land at spans 6,6,6,7 against a
// 153-draw scene span at ~5; batch 2's at 96,96,96,97 against its own at ~95.
// A batch's scene draws therefore cannot be reading the upload that sits inside
// its own span -- they read one from earlier in the pipeline. So "patch the
// upload tagged batch 2 and batch 2's draws will use it" was wrong by a stage,
// which is why the bytes have diverged in every run since M3 opened and the
// pictures never have.
//
// The fix is to stop inferring which upload a draw reads and write the camera
// where the ordering is not in question: immediately after the transition that
// OPENS batch 2's scene span, before any of its draws. That needs the buffer
// the engine uploads to and a copy of what it last put there, both captured
// here where the uploads actually happen.
std::atomic<void*> g_worldCameraBuffer{nullptr};
alignas(16) std::uint8_t g_worldCameraBytes[kCameraBufferBytes]{};
std::atomic_bool g_worldCameraValid{false};
std::atomic_uint64_t g_worldCameraSnapshots{0};
// stereo.batch2_ipd_mode. 0 = offset only inside batch 2 (what M3 wants);
// 1 = offset EVERY qualifying upload, which is what the shipped
// test.eyetranslation does and what visibly moved the world. The only
// difference left between the two is the batch-2 gate, so this makes that gate
// the single variable of a run instead of an assumption.
std::atomic_int g_batch2IpdMode{0};

// WHICH SIDE OF THE SEAM TAKES THE EYE OFFSET.
//
//   0  batch 2 only -- what true stereo wants, and what does not work: the
//      patch is applied inside batch 2's own scene span, at the same relative
//      position batch 1's lands at, and batch 2's draws do not reflect it.
//   1  everything -- the shipped test.eyetranslation's configuration. This DOES
//      move the world, proven by the muzzle firing from off-screen left.
//   2  everything EXCEPT batch 2 -- the inversion, and the point of it.
//
// Mode 2 exists because mode 1 established the half nobody had checked:
// patching BATCH 1's uploads moves batch 1's world. Stereo does not care which
// batch carries the offset, only that the two differ. So rather than keep
// hunting for why batch 2 will not take an offset, give the offset to the side
// that demonstrably takes one and leave batch 2 at the original camera.
//
// It also settles the open question either way. If the pair diverges, batch 2's
// draws read their own constants and the defect is confined to how batch 2's
// upload is delivered. If the pair is still identical, batch 2's draws are
// reading BATCH 1's patched constants -- which would explain every run since
// M3 began, and names the fix as "make batch 2's draws see a different buffer"
// rather than "make batch 2's upload land".
bool Batch2IpdModeWants(bool inBatch2) {
    switch (g_batch2IpdMode.load(std::memory_order_relaxed)) {
        case 1:  return true;
        case 2:  return !inBatch2;
        default: return inBatch2;
    }
}
std::atomic<float> g_batch2Ipd{0.0f};
std::atomic_bool g_batch2OffsetArmed{true};
std::atomic_uint64_t g_batch2OffsetsApplied{0};
std::atomic_uint64_t g_batch2OffsetsSkippedBasis{0};
std::atomic_uint64_t g_batch2OffsetsSkippedFamily{0};

// THE WITNESS, AND IT LIVES INSIDE THE UPLOAD HOOK. The falsifier is "two
// distinct c_cameraOrigin values in ONE frame", so it cannot be a window delta
// and it cannot be a session total: it is a per-Present-interval census of the
// distinct origins uploaded, split by which batch issued them, recorded at the
// moment of upload and reported at the frame boundary.
struct OriginTally { float x, y, z; unsigned long long count; };
// RAISED FROM 4, because 4 truncated: the first run reported "20 further
// uploads did not fit" on both batches, which made every distinct count a floor
// and left the interesting families unlisted.
constexpr int kMaxOriginsPerBatch = 12;
OriginTally g_originsByBatch[3][kMaxOriginsPerBatch]{};
int g_originCountByBatch[3]{};
unsigned long long g_originOverflowByBatch[3]{};
// AND THE ONE THAT ACTUALLY MOVES THE EYE. c_cameraOrigin diverging proved only
// that the bytes changed; the world renders camera-relative through
// c_cameraRelativeToClip, so the view translation is its column 3 --
// (m[3], m[7], m[11]) -- and THAT is what has to differ between the batches for
// the picture to differ. The first run's images measured zero disparity in
// every depth band while the origin census read a clean divergence, which is
// exactly the gap this second table closes.
OriginTally g_viewXlatByBatch[3][kMaxOriginsPerBatch]{};
int g_viewXlatCountByBatch[3]{};
unsigned long long g_viewXlatOverflowByBatch[3]{};
// Where in the interval each WORLD-family camera upload landed, per batch.
constexpr int kMaxWorldUploadSpans = 12;
int g_worldUploadSpans[3][kMaxWorldUploadSpans]{};
int g_worldUploadSpanCount[3]{};
const void* g_worldUploadBuffers[3][kMaxWorldUploadSpans]{};
// N1-Q1. The batch-local VSSetConstantBuffers ordinal at each world-camera
// upload, and the ordinal at which batch 2's scene span opened. Span
// granularity cannot order an upload against the draws INSIDE one span; this
// can. It is NOT the DrawIndexed tally the plan asked for -- slots 12/13 fault
// the driver, so that tally cannot be built here -- and stereo_targets.h says so at
// the accessor.
unsigned long long g_worldUploadBinds[3][kMaxWorldUploadSpans]{};

void TallyTriple(OriginTally* table, int& count, unsigned long long& overflow, const float* v) {
    for (int i = 0; i < count; ++i) {
        // Exact compare on purpose. The positive control is "offset 0 gives
        // BYTE-IDENTICAL values", and a tolerance would hide exactly that.
        if (table[i].x == v[0] && table[i].y == v[1] && table[i].z == v[2]) { ++table[i].count; return; }
    }
    if (count >= kMaxOriginsPerBatch) { ++overflow; return; }
    table[count].x = v[0]; table[count].y = v[1]; table[count].z = v[2];
    table[count].count = 1;
    ++count;
}

void TallyUploadedCamera(int batch, const float* origin, const float* viewXlat) {
    if (batch < 0 || batch > 2) return;
    TallyTriple(g_originsByBatch[batch], g_originCountByBatch[batch],
                g_originOverflowByBatch[batch], origin);
    TallyTriple(g_viewXlatByBatch[batch], g_viewXlatCountByBatch[batch],
                g_viewXlatOverflowByBatch[batch], viewXlat);
}
// Two different candidate translation paths, tested one at a time so a visible
// change is attributable to exactly one of them.
enum class OffsetMode { CameraOrigin, MatrixEyeTranslation, PerEye, ViewmodelProbe };
// Not an offset-test mode: this has to run alongside per-eye stereo rather
// than instead of it, and it follows head tracking rather than a frame budget.
std::atomic_bool g_compensationEnabled = false;
std::atomic_uint32_t g_compensationInstallAttempts = 0;
constexpr std::uint32_t kMaxCompensationInstallAttempts = 600;

// Published by camera_hook while head tracking is armed.
extern "C" volatile float g_headDeltaYawDegrees;
extern "C" volatile std::uint8_t g_headCompensationValid;
// The before/after pair the DETOUR latched, in the same block, for the frame
// the engine actually built its view with. The correction used to read copies
// published on a plugin frame instead; those are updated on a different
// cadence, so during head motion the two drifted apart and the weapon moved
// with the head for a moment before snapping back.
extern "C" volatile std::uint32_t g_cameraBasePosXBits;
extern "C" volatile std::uint32_t g_cameraBasePosYBits;
extern "C" volatile std::uint32_t g_cameraBasePosZBits;
extern "C" volatile std::uint32_t g_cameraBasePitchBits;
extern "C" volatile std::uint32_t g_cameraBaseYawBits;
extern "C" volatile std::uint32_t g_cameraBaseRollBits;
extern "C" volatile std::uint32_t g_cameraAppliedPitchBits;
extern "C" volatile std::uint32_t g_cameraAppliedYawBits;
extern "C" volatile std::uint32_t g_cameraAppliedRollBits;
extern "C" volatile std::uint32_t g_cameraAngleGeneration;
// The head's lean offset in game space, published by camera_hook and added to
// the camera by the detour. The viewmodel needs the opposite of it.
extern "C" volatile std::uint32_t g_cameraOffsetXBits;
extern "C" volatile std::uint32_t g_cameraOffsetYBits;
extern "C" volatile std::uint32_t g_cameraOffsetZBits;
extern "C" volatile std::uint8_t g_cameraPositionWriteActive;
extern "C" volatile float g_headBaseAngles[3];
extern "C" volatile float g_headWrittenAngles[3];
extern "C" volatile std::uint32_t g_headAnglesGeneration;
extern "C" volatile std::uint64_t g_cameraHookCallCount;
// The head's rotation since recentring, as a basis, published by camera_hook
// between two generation bumps. This IS the rotation to cancel: head tracking
// composes written = base * delta, so base^T * written is delta exactly. Taking
// it directly removes a decompose-and-recompose round trip that could only add
// error to the one number this correction depends on.
extern "C" volatile float g_headDeltaBasis[9];
extern "C" volatile std::uint32_t g_headDeltaGeneration;

float BitsToFloatLocal(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
OffsetMode g_offsetModeKind = OffsetMode::CameraOrigin;
std::atomic_bool g_offsetArmed = false;
std::atomic_bool g_offsetMode = false;
std::atomic<float> g_eyeSign = 0.0f;

// The 3D skybox is rendered in its own space at roughly 1/16 scale, so giving
// it the same world-space eye offset as the player camera produces ~16x too
// much parallax on distant scenery.  Distant skybox geometry should have
// essentially zero disparity, so it is excluded from the offset entirely.
//
// It is identified by origin: the player camera accounts for most perspective
// uploads in a frame, while the sky camera sits thousands of units away.  The
// per-frame mode is promoted to the reference at each frame boundary.
constexpr float kSkyboxOriginThreshold = 1000.0f;
constexpr size_t kOriginTableSize = 8;
struct OriginCount { float origin[3]; std::uint32_t count; bool used; };
OriginCount g_originTable[kOriginTableSize]{};
float g_referenceOrigin[3]{};
std::atomic_bool g_referenceValid = false;

// Two switchable variants of shipped behaviour; see camera_update_hook.h.
std::atomic_bool g_hookImplementationAllowed = true;
std::atomic_bool g_hookPassThrough = false;
std::atomic_bool g_uploadNoSubstitute = false;
std::atomic_bool g_uploadIdentityCopy = false;

// BODY LEVEL -- how much of the hook body runs, as a single dial.
//
// Five headset runs have each bought exactly one bit, and four of the five
// refuted a confident prediction. The body is provably free on the CPU
// (0.001 ms a call) yet enabling it costs 87 ms in xrEndFrame, so the cost is
// not where any per-call timer can see it. Rather than keep guessing which
// statement is responsible one run at a time, the body is cut into nested
// levels and the sweep walks them automatically, reporting xrEndFrame for each.
// One run then yields the whole curve, and the level where xrEndFrame jumps
// names the culprit directly.
//
//   0  forward immediately                    (== pass-through, known FAST)
//   1  + the two GetType/GetDesc counter blocks
//   2  + classification, FOV/compensation maths, upload the game's pointer
//   3  + substitute a byte-identical copy      (known SLOW)
//   4  + actually patch the bytes              (full behaviour)
//
// Levels are nested, so the first level that is slow is the one that added the
// cost. 3 being slow and 0 fast is already established; this places 1 and 2.
constexpr int kBodyLevelFull = 4;
std::atomic_int g_bodyLevel = kBodyLevelFull;
std::atomic_bool g_bodySweep = false;
// Camera-sized upload count at the last sweep window, so a window with no
// uploads can be told from one that did work.
std::atomic_uint64_t g_sweepCameraSizedMark = 0;
volatile unsigned long long g_foreignCallsRacy = 0;
volatile double g_bodyMsRacy = 0.0;
volatile unsigned long long g_bodyCallsRacy = 0;
std::atomic_bool g_excludeLowerCamera = false;
// EXCLUDE THE HUD PASS FROM THE VIEWMODEL CORRECTION.
//
// Within the near-plane -1 family two different things share every
// discriminator the filter had: |r0| and |r1| are identical across all of
// them, so FOV cannot separate them. ORIGIN can. The census shows many
// entries at the PLAYER'S world position and exactly one at (0,0,0), drawn
// far more often than any of them.
//
// PHASE1-RESULT section 4 settled which is which by experiment rather than by
// reasoning: translating the near-origin family's matrix "made the entire HUD
// except the reticle disappear and left the gun untouched". Near-origin is the
// HUD. IsBodyWeaponPass assumed it was the gun's camera-relative pass and so
// returned true for it unconditionally -- the guard written to protect the gun
// was the reason the HUD could never be excluded, and the HUD was therefore
// counter-rotated by the rotation meant for the weapon on every upload.
//
// Confirmed live before this was written: INSERT, which disables the
// correction wholesale, makes the wave vanish completely and leaves the HUD
// exactly where a flatscreen game would put it.
std::atomic_bool g_excludeNearOriginFromCorrection = true;
// The complement: exclude the WORLD-origin near-plane-1 passes instead. The
// two flags together bisect the family in one run each, and the counters below
// prove which branch actually fired -- the thing missing last time.
std::atomic_bool g_excludeWorldOriginFromCorrection = false;
// THE HUD ANCHOR. 0 = head-locked (the camera's own frame, what excluding the
// HUD pass from the weapon correction leaves you with). 1 = aim-locked: the
// panels sit where the gun is pointing and stay there while the head turns.
// Default 0, the known-good state defect A was closed in.
// AIM MODE IS BROKEN AND STAYS OFF BY DEFAULT. Reported from the headset:
// partly pinned to the hand, but head movement still drags it, so it wanders.
// The rotation is only half the anchor -- the HUD pass is CAMERA-RELATIVE
// (origin 0,0,0), so its translation still rides the head, and positional head
// tracking moves that origin. Rotating the basis without also cancelling the
// head-relative translation leaves exactly the "moving everywhere" that was
// reported. Kept, not reverted, because the rotation half is proven correct.
std::atomic_int g_hudAnchorMode = 0;
// THE HUD PASS'S FRUSTUM. Measured 2026-09-05 from this plugin's own upload
// census, in logs that already existed: the main scene renders at half-tangents
// 1.4205 x 0.9950 (109.7 x 89.7 deg, the headset) while the NEAR-ORIGIN HUD
// pass carries 0.7502 x 0.5252 (73.7 x 55.4 deg). The ratio is 0.5281
// horizontally and 0.5278 vertically -- THE SAME IN BOTH AXES to 0.06% -- so
// anything world-anchored drawn in that pass sits 1.89x too far from the view
// centre: zero error at the centre, growing outward, bounded by the frustum,
// unaffected by translation, and stable once the head stops. That is exactly
// the shape the wearer's discriminators left standing for the waypoint AND for
// the reticle, and flat never shows it because flat renders the world at the
// same field of view the HUD pass uses.
//
// The WEAPON pass is already refitted to the world's frustum a few lines below
// (see ScaleProjectionRows and targetTanX); the HUD pass is deliberately
// excluded from that refit. This lever acts on the HUD pass either way, using
// the world tangents measured in the SAME frame rather than any constant.
std::atomic_int g_hudFovMode{0};
std::atomic_uint64_t g_hudFovMatchApplied{0};
std::atomic_uint64_t g_hudFovMatchNoWorld{0};
// ---- THE BRIEF WHOLE-HUD SHRINK, wearer 2026-09-06 -------------------------
// "When in the titan and rotating my head left or right, sometimes the ENTIRE
// HUD seems to shrink down briefly and then go back. Elements at the top and
// bottom both shrink towards center and then go back."
//
// A uniform shrink about the screen centre, pass-wide, brief, on head rotation
// is the exact signature of THIS correction dropping out for a few frames: the
// refit is what puts the near-origin HUD pass back to the frustum the game
// uploaded, and without it the pass renders 1.8952x wider, which shows as
// everything in it shrinking toward the centre. So the standing fix is the
// first suspect, not the last, and these counters test it without a keypress
// and without putting the wearer back in the broken state.
//
// The 5 s aggregate above cannot see this: a dropout of a few frames only makes
// the refit count tick slower. What names it is the pass predicate's two halves
// counted SEPARATELY -- an upload that looks like the HUD pass on one half and
// not the other is a dropout -- plus the range of the scale actually applied,
// because a refit that computes 1.0 is a refit that did nothing.
std::atomic_uint64_t g_hudFovNearNotVm{0};   // near-origin, but not a viewmodel candidate
std::atomic_uint64_t g_hudFovVmNotNear{0};   // viewmodel candidate, but not near-origin
std::atomic<float> g_hudFovSxMin{1.0e9f};
std::atomic<float> g_hudFovSxMax{-1.0e9f};
// ---- CLOSING THE WITNESS'S BLIND SPOT, 2026-09-06 --------------------------
// Run 1 of the witness came back with near-origin-but-not-viewmodel at ZERO and
// the scale constant within nearly every window, while the wearer saw the
// shrink about a dozen times. Both of those are consistent with a dropout the
// witness cannot see: the two half-match counters catch only the EXCLUSIVE-OR
// cases, so a frame where BOTH halves fail is counted by neither, and a frame
// with no refit contributes no sample to the scale range either. A dropout is
// exactly that frame.
//
// So count it directly and without needing to identify the pass: how many
// FRAMES carried at least one refit. The HUD pass is uploaded every frame, so a
// frame with none is a dropout frame, and "frames seen" against "frames
// refitted" is the whole measurement. No predicate, nothing to be wrong about.
std::atomic_uint32_t g_hudFovLastRefitFrame{0xFFFFFFFFu};
std::atomic_uint64_t g_hudFovFramesRefitted{0};
std::atomic_uint32_t g_hudFovWindowFirstFrame{0xFFFFFFFFu};
std::atomic_uint32_t g_hudFovWindowLastFrame{0};

// ---- THE BOUND THAT WAS ALSO A GATE, 2026-09-06 ----------------------------
// Run 2 measured the dropout directly: 5-15% of frames missed at rest, bursting
// to 40% while the wearer moved, matching "more than ten times" of visible
// shrink. So the pass predicate IS rejecting the HUD pass intermittently.
//
// Only one of its three terms can vary with head motion. `element15 == 0` and
// `nearPlane == -1` are structural facts about a projection matrix; the third
// is `|origin|^2 <= 1.0` -- the camera origin within ONE unit of zero, about
// 2.5 cm. Rotating your head swings your eyes around your neck by ten
// centimetres or more, which is several times that bound, in every direction.
// That is the whole symptom: move, exceed the bound, lose the correction, stop,
// fall back inside it, get it back.
//
// The bound is a HEURISTIC that was also acting as a GATE, so it is widened
// rather than removed, and widened in a SEPARATE predicate used only by the
// frustum refit -- `IsNearOriginPerspectiveCamera` has three other callers and
// none of them are asking this question. What the bound establishes is "this
// camera sits at the origin rather than out in the world", and a world camera's
// origin is the player's position: thousands to tens of thousands of units. A
// bound of 32 units (about 80 cm, more than any seated head movement) keeps two
// to three orders of magnitude of margin against that, so it only narrows the
// gap between what we accept and what we intend to accept.
//
// The histogram below measures the population INDEPENDENTLY of the bound -- it
// records every structurally-matching upload whether the bound accepts it or
// not -- so widening the bound cannot contaminate the evidence for widening it.
std::atomic<float> g_hudFovOriginMax{32.0f};
std::atomic_uint64_t g_hudFovOriginBand[6]{};   // <=1, <=4, <=16, <=64, <=256, more
std::atomic<float> g_hudFovOriginMaxSeen{0.0f};
std::atomic_uint64_t g_hudAnchorAppliedCount = 0;
std::atomic_uint64_t g_hudAnchorNoAimCount = 0;
std::atomic_uint64_t g_nearOriginSeenCount = 0;
std::atomic_uint64_t g_nearOriginExcludedCount = 0;
std::atomic_uint64_t g_worldOriginSeenCount = 0;
std::atomic_uint64_t g_worldOriginExcludedCount = 0;
// Let the viewmodel family include the camera-relative pass at the origin --
// which is the gun's own pass. See IsViewmodelCandidateCamera.
std::atomic_bool g_viewmodelIncludeNearOrigin = true;

// THE WEAPON FROM ONE VIEWPOINT, THE WORLD FROM BOTH.
//
// -1 off, 0 left, 1 right, 2 CENTRE. Centre is the one that actually works, and
// the reason is not about eyes at all:
//
//   THE BULLET TRAVELS ALONG THE CENTRE CAMERA'S AXIS.
//
// It does not leave from the left eye or the right one. So a sight picture
// rendered from either eye is offset from the path the round takes, by half an
// IPD, and no choice of eye can fix that -- pinning it to one eye merely moves
// which eye sees the disagreement. Measured in the headset: with this set to
// the right eye the sights lined up for the LEFT, and the round still landed
// well right of the sight with the other eye closed. Both facts are the same
// fact, and both go away at centre, where the sight picture and the round share
// an axis.
//
// The weapon was also INCOHERENT WITH ITSELF, which is worth fixing whatever
// viewpoint is chosen. Its family is four passes: three are world-perspective
// and took the alternating per-eye disparity, while the gun's own pass renders
// at origin (0,0,0), fails the |origin|>1 world test, and took none. Parts of
// one weapon drawn from two viewpoints in the same frame.
//
// At centre every pass of the family takes zero lateral offset in both eye
// frames. The weapon then carries no stereo depth of its own -- it reads at
// infinity -- which is the price, and it buys sights that point where the gun
// shoots. The world keeps full stereo throughout.
std::atomic_int g_weaponMonoEye = -1;
// Multiplies the weapon's rendered size on top of the world-FOV match. 1.0 is
// the match alone, which is reported as still too large. Adjusted live rather
// than guessed: see the note where it is applied.
std::atomic<float> g_weaponSize = 1.0f;

// DEPTH WHEN IT HELPS, ALIGNMENT WHEN IT MATTERS.
//
// Centre rendering buys sights that point where the round goes, and it costs
// the weapon all of its stereo depth -- it reads flat, at infinity, because
// zero disparity is exactly what "no depth" means. Reported as "is the weapon
// even 3D?", which is the correct thing to notice.
//
// The two wants do not actually conflict in time. Hip-firing wants a solid,
// nearby, three-dimensional gun and does not care about the sights. Aiming down
// sights wants the sight picture on the round's axis and does not care that the
// weapon has flattened, because at that moment it is a sight and not a prop.
//
// So the weapon renders in full stereo normally and snaps to centre while
// zoomed. The detector is free and already verified: the main-scene frustum
// narrows on ADS, measured animating smoothly from 123.7 degrees down to 85.8
// and back over 103 logged changes. Resting width is tracked as the widest
// seen, and anything appreciably under it is a zoom.
std::atomic_bool g_weaponMonoOnlyWhenZoomed = true;
std::atomic<float> g_restingWorldTanX = 0.0f;
std::atomic<float> g_zoomThreshold = 0.98f;
std::atomic_bool g_zoomLatched = false;
bool WorldIsZoomed();
void NoteFovSignals(float worldTanX);

// A RESTING REFERENCE THAT CAN COME BACK DOWN. Read-only; nothing consumes it
// yet.
//
// g_restingWorldTanX above is an ALL-TIME running maximum, and FOVSIG's own
// comment records what that costs: the 2026-08-21 session ramped 124.7 ->
// 133.1 -> 124.2 degrees, the reference latched at 133.1 for the whole session,
// and the zoom classifier entered ZOOMED and could never leave. One transient
// permanently poisons an all-time extremum, and C1 is about to need a rest
// reference it can divide by.
//
// This is the same quantity taken two ways so the two can be COMPARED before
// either is trusted:
//
//   * SLIDING, so it ages out. Six buckets of five seconds; the reference is
//     the maximum across them. A transient is wrong for at most 30 seconds
//     instead of for the process lifetime.
//   * STABLE SAMPLES ONLY. A sample updates its bucket only if it is within
//     0.5% of the previous sample. The ADS transition is an ANIMATION -- 103
//     logged changes from 123.7 down to 85.8 -- so every frame of a ramp
//     differs from the last and is excluded, while a genuine plateau (rest, or
//     a held zoom) is identical frame to frame and is not. That is what makes
//     this a plateau tracker rather than a slower ratchet.
//
// It deliberately does NOT gate on WorldIsZoomed(): that predicate divides by
// the poisoned reference, so feeding its verdict back in would make a wrong
// reference self-confirming. A held ADS plateau is narrower than rest and so
// cannot raise a maximum anyway.
//
// COST IS BOUNDED BY CONSTRUCTION: six floats and one compare per upload.
// BOTH AXES, because C1 divides by the VERTICAL one and only the vertical one
// is safe to divide by. viewportFullFix rewrites row0 and leaves row1 alone, so
// at the patch site the horizontal has already been changed while the vertical
// is still the game's own. Measured, the game narrows the two by the same
// factor anyway -- 123.7 -> 68.7 across is a tangent ratio of 0.3662 and
// 92.9 -> 42.0 down is 0.3648, a difference of three parts in a thousand -- so
// the vertical is a faithful stand-in and it is the one that cannot have been
// touched underneath us.
constexpr int kRestBuckets = 6;
constexpr std::uint64_t kRestBucketMs = 5000;
std::atomic<float> g_plateauRestTanX = 0.0f;
std::atomic<float> g_plateauRestTanY = 0.0f;
std::atomic_uint64_t g_plateauStableSamples = 0;

void NotePlateauRest(float tanX, float tanY) {
    static float bucketsX[kRestBuckets]{};
    static float bucketsY[kRestBuckets]{};
    static std::uint64_t bucketStart = 0;
    static int cursor = 0;
    static float previousX = 0.0f;
    static float previousY = 0.0f;

    const std::uint64_t now = GetTickCount64();
    if (bucketStart == 0) bucketStart = now;
    if (now - bucketStart >= kRestBucketMs) {
        bucketStart = now;
        cursor = (cursor + 1) % kRestBuckets;
        bucketsX[cursor] = 0.0f;
        bucketsY[cursor] = 0.0f;
    }

    const float lastX = previousX;
    const float lastY = previousY;
    previousX = tanX;
    previousY = tanY;
    // The first sample has nothing to be stable against, so it is skipped
    // rather than admitted -- a single upload is exactly the transient this
    // exists to reject.
    if (lastX <= 0.0001f || lastY <= 0.0001f) return;
    // BOTH axes have to be stable, not either. The ADS animation moves them
    // together, and admitting a sample because one of them happened to sit
    // still for a frame is how a ramp gets into a plateau tracker.
    if (std::fabs(tanX - lastX) > 0.005f * lastX) return;
    if (std::fabs(tanY - lastY) > 0.005f * lastY) return;
    g_plateauStableSamples.fetch_add(1, std::memory_order_relaxed);
    if (tanX > bucketsX[cursor]) bucketsX[cursor] = tanX;
    if (tanY > bucketsY[cursor]) bucketsY[cursor] = tanY;

    float widestX = 0.0f, widestY = 0.0f;
    for (int i = 0; i < kRestBuckets; ++i) {
        if (bucketsX[i] > widestX) widestX = bucketsX[i];
        if (bucketsY[i] > widestY) widestY = bucketsY[i];
    }
    g_plateauRestTanX.store(widestX, std::memory_order_release);
    g_plateauRestTanY.store(widestY, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// C1 -- ads.max_magnification. MAKE THE NARROWING A DIAL.
//
// What ADS does in VR today, measured: the game narrows the main scene from
// 123.7 x 92.9 to 68.7 x 42.0 into the SAME buffer. The projection layer then
// declares the narrowed frustum, so world scale is preserved and it does not
// look like a zoom -- what it actually is is 2.74x the angular resolution
// bought with a black periphery on all four sides. Both halves of that are
// real, so this is a dial and not a bug fix.
//
//   0 (default)  pass the game's own value straight through: today's behaviour
//   1.0          suppress the narrowing entirely -- full periphery, no
//                sharpness bonus, ADS becomes state + reticle + sights
//   ~1.3         most of the periphery, some of the sharpness
//
// THE FALSIFIER FOR ANYTHING TOUCHING THE FRUSTUM IS ADS ITSELF, and this repo
// has already paid for getting it wrong: xr.match_headset_fov clamped the
// frustum to an ABSOLUTE value on EVERY upload, including during ADS, which
// broke the zoom outright and is why it ships OFF for a measured reason.
//
// So this does not clamp. It computes how much the game has magnified relative
// to the resting plateau and widens ONLY the excess over the cap:
//
//     magnification = restTanY / tanY          (1.0 at rest, ~2.74 in ADS)
//     if magnification <= cap: DO NOTHING AT ALL
//     otherwise widen both rows by magnification / cap
//
// At rest magnification is 1.0 by construction and any cap of 1.0 or more
// leaves the upload untouched -- not "scaled by 1.0", untouched, no rows
// written. That is the plan's "must be proven not to clamp at rest" satisfied
// by construction rather than by a constant, and the skipped/adjusted counts
// below are the proof in the log. The 12% horizontal rest overshoot is a
// separate open item and cannot move, because nothing is written at rest.
//
// BOTH ROWS BY THE SAME FACTOR, so the aspect ratio is untouched and the
// horizontal relationship viewportFullFix established survives. This runs
// AFTER that patch for exactly that reason.
//
// MAGNIFIED OPTICS ARE NOT YET EXEMPT, and this is a deliberate omission
// rather than an oversight. The plan is explicit that a 4x scope SHOULD
// magnify and that detecting one is per-weapon and rides C3's weapon identity
// work (S4). The identity is known -- an int at weaponEntity+0x15C8, see
// S1-ATTACHMENT-API-2026-08-26.md section 3 -- but no measurement of what a
// SCOPED weapon's tangent ratio actually reads exists yet, and a threshold
// picked without one would be a speculative calibration knob. Until S4 wires
// the identity, a scope is capped like an iron sight.
std::atomic<float> g_adsMaxMagnification = 0.0f;
std::atomic_uint64_t g_adsWidened = 0;
std::atomic_uint64_t g_adsUntouched = 0;
std::atomic<float> g_adsLastMagnification = 0.0f;

// THE OPTIC EXEMPTION, ON THE WEAPON'S OWN NUMBER.
//
// PLAN-ADS section 3: a 1x iron sight should NOT magnify in VR because your eye
// is already there, but a 4x scope SHOULD, because that is what a scope
// physically does. This was deliberately left unbuilt while the only way to
// tell them apart would have been a threshold nobody had a measurement for.
//
// There is one now, and it comes from the engine per weapon rather than from a
// table of ours. Measured across the whole starting arsenal:
//
//   pistols 1.01x   wingman 1.52x   shotgun 1.75x   smg 2.01-2.74x
//   rspn101 2.74x   dmr 3.26x       lmg 3.26-5.06x
//
// So iron sights and reflex optics live at or under about 2.7x and the scoped
// weapons sit at 3.26x and above. 3.0 splits them with room either side, and it
// is ONE dial rather than a per-weapon list -- a weapon this project has never
// seen declares its own number and is classified by the same rule.
std::atomic<float> g_adsOpticPassthrough = 3.0f;

// Returns the factor to WIDEN both tangents by; exactly 1.0 means "do nothing",
// and callers must test for that rather than multiplying by it, so that an
// untouched upload is untouched rather than round-tripped through a float.
//
// THE MAGNIFICATION NOW COMES FROM THE ENGINE, NOT FROM THE RENDERED FRUSTUM.
// The argument is ignored and kept only so the two call sites stay identical.
// Four flat runs went into observing the frustum through the D3D upload path;
// GetFOV hands the same number back live, per weapon, and reproduced this
// repo's own independently measured 2.74x for the rifle to two decimals.
//
// The plateau tracker beside g_restingWorldTanX is no longer consulted here.
// "At rest" is the engine's zoom fraction being zero, which cannot be poisoned
// by a transient the way a running maximum can.
// THE DECIDER, and it now scales what the layer DECLARES rather than what the
// game renders. Called from GetMainSceneHalfTangents, which is the one function
// the projection layer reads its fov from.
//
// Returns the magnification to declare, where 1.0 means "declare exactly what
// was rendered", i.e. do nothing. At rest the engine reports 1.00, so nothing
// is scaled at rest by construction rather than by a threshold.
float AdsDeclaredMagnification() {
    AdsZoomState zoom;
    if (!AdsEngagedByEngine() || !GetAdsZoom(&zoom) || zoom.magnification <= 1.0f) return 1.0f;
    float desired = zoom.magnification;
    // A magnified optic keeps the whole of its zoom. Judged on what the weapon
    // will reach at FULL ads, which is a property of the gun, so the decision
    // does not change halfway through the blend and cannot flicker while the
    // fraction ramps.
    const float passthrough = g_adsOpticPassthrough.load(std::memory_order_relaxed);
    const bool optic = passthrough > 1.0f && zoom.magnificationAtFull >= passthrough;
    const float cap = g_adsMaxMagnification.load(std::memory_order_relaxed);
    if (cap >= 1.0f && !optic && desired > cap) desired = cap;
    g_adsLastMagnification.store(desired, std::memory_order_relaxed);

    // One line per distinct declared value, so a held zoom does not write a line
    // a frame and a change is never silent.
    static float lastDeclared = 0.0f;
    if (std::fabs(desired - lastDeclared) > 0.01f) {
        lastDeclared = desired;
        g_adsWidened.fetch_add(1, std::memory_order_relaxed);
        char line[440]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] ADS ZOOM: the engine narrowed to %.2fx and the layer now DECLARES %.2fx of "
            "it%s. Declaring the NARROW angle is what made the world hold still inside a black "
            "box -- a 68.7 degree image shown across 68.7 degrees is the same size it always "
            "was. Holding the declared angle while the render narrows is what makes it magnify "
            "and fill the view.\n",
            static_cast<double>(zoom.magnification), static_cast<double>(desired),
            optic ? " (magnified optic, exempt from the cap because a scope should zoom)" : "");
        Tf2VrLog(line);
    }
    return desired;
}
// The WEAPON's own frustum, which may be the better zoom signal.
//
// The world barely moves for a pistol -- 1.4 degrees -- so detecting ADS from
// it will always be marginal for some weapon. The viewmodel's own frustum
// appears to move far more, but every previous reading of it came from an
// instrument that shared one static across four passes and interleaved them.
// Tracked and logged properly here so the next session can choose a signal from
// evidence instead of switching to a second unmeasured one.
std::atomic<float> g_restingWeaponTanX = 0.0f;
std::atomic<float> g_weaponTanX = 0.0f;

// THE ONE-FRAME VIEW HEIGHT DROPOUT ON A STEP UP.
//
// Measured across three steps, each paused on deliberately:
//
//   before 698.60  spike -11.34  then +9.50, +1.5/frame
//   before 722.59  spike  -6.14  then +7.98, +1.0/frame
//   before 729.49  spike  -8.60  then +10.42, +1.17/frame
//
// Exactly one frame sits 6 to 11 units low -- about a step height -- and then
// the height recovers and climbs normally. The game publishes a stale view
// height for a single frame as it steps up. On a monitor that is 16 ms and
// invisible. In a headset it is a 22 cm vertical lurch, and because the view
// drops while the player rises, distant terrain appears to jump UP and then
// correct, which is exactly how it was reported.
//
// The guard holds the previous height for ONE frame and then releases
// unconditionally. That is deliberately self-limiting: a genuine fall is not a
// single frame, so it costs one frame of lag and then proceeds at full speed,
// whereas a rate limiter would fight every fall for as long as it lasted.
// DEFAULT OFF: it was built against our own bug.
//
// The one-frame dropout it suppresses was not the game publishing a stale
// height. It was the positional write pairing this frame's head offset with
// last frame's base position, and the detour now sums them at the write site so
// the dropout does not occur. Confirmed by the glitch vanishing outright with
// positional tracking off, which is the one thing that stops that write.
//
// Kept because the measurement was real and the mechanism may recur elsewhere,
// but a guard against a spike that no longer happens can only mask a future
// one. It should stay off unless something puts a spike back.
// DETACH THE BODY FROM THE HEAD, WHICH IS THE POINT OF LEANING.
//
// Positional tracking moves the CAMERA. The viewmodel -- arms and gun -- is
// drawn camera-relative, so it follows the camera by construction: lean, and
// the gun leans with you. That is indistinguishable from leaning not working,
// and is what "some code was added but frankly does nothing" describes.
//
// The rotational half of this has existed for a long time and is why the gun
// stays with the aim when the head turns: M' = M * Rel cancels the head
// rotation on the weapon's own pass. The translational half was never written.
//
// SIGN: the weapon's camera moves WITH the lean, not against it.
//
// The first version used -offset and made it worse in the reported way: lean
// left and the gun slid further left, off to the wrong side of the body. That
// is the arithmetic being right about magnitude and wrong about direction.
//
// Moving a camera by e shifts everything it draws by -e on screen. The weapon
// already follows the head because it is camera-relative, so cancelling that
// needs a screen shift of -offset, which needs e = +offset. Using -offset
// produced +offset of screen motion, on top of the offset the weapon already
// had: two leans instead of none, which is exactly what was described.
//
// STRENGTH, not a switch. The ask was to PARTIALLY detach the head from the
// body: 1.0 pins the weapon in the world and the head slides freely around it,
// 0.0 is the camera-relative default where it rides along, and between them the
// weapon lags the head by a fraction. Which value feels right is a judgement
// made wearing the headset, so it is a number rather than a boolean.
std::atomic<float> g_detachLean = 1.0f;
// Counts every upload the detach actually ran on, so "the lean detach is armed"
// and "the lean detach is doing something" are different, checkable claims.
std::atomic<unsigned long long> g_detachLeanApplied{0};
std::atomic_bool g_stepGuard = false;
std::atomic<float> g_stepGuardThreshold = 3.0f;
float g_stepCorrection = 0.0f;
std::atomic_uint32_t g_stepCorrectionCount = 0;
// 0 first pass wins, 1 last pass wins, 2 the pass whose origin is the world's.
// See the note in PinFrameAngles. Deliberately starts on the reasoned candidate
// rather than on either ordinal, both of which are measured half-wrong.
std::atomic_int g_pinMode = 2;
unsigned g_mainScenePassesThisFrame = 0;
// Frames left in a step burst, shared so the per-pass logger can see it.
unsigned g_stepBurstFrames = 0;
std::atomic_bool g_eyeBasisPreRotation = false;
std::atomic_uint32_t g_lowerCameraExcludedCount = 0;

// The origin of the most recent main-scene upload (near plane -7).  The
// body/weapon pass shares the player's origin with it; the "lower camera" sits
// 60 units below.  Main-scene uploads come first in the frame (#2/#3/#5 before
// #6-#9), so this reference is same-frame fresh by the time it is consulted.
// A one-frame-stale value moves by the distance walked in a frame -- a few
// units at most -- which the threshold absorbs without ever reaching 60.
float g_mainSceneOrigin[3]{};
std::atomic_bool g_mainSceneOriginValid = false;
// The camera BASE as it stood at the instant the view origin above was
// captured. Their difference is the standing offset between the two references
// -- head offset plus whatever easing the engine is mid-way through -- and it
// is what lets a consumer have the view's LEVEL and the base's FRESHNESS at the
// same time instead of choosing between a spike and a frame of lag.
float g_mainSceneBase[3]{};
// The world pass matrix, for F3 projection checks. See the capture site.
float g_mainSceneMatrix[16]{};
std::atomic_bool g_mainSceneMatrixValid = false;
// The nearest-to-base pass of the frame IN PROGRESS. Written per pass on the
// upload thread, published to the globals above once at the frame boundary.
float g_mainSceneCandidateOrigin[3]{};
float g_mainSceneCandidateBase[3]{};
float g_mainSceneCandidateMatrix[16]{};
bool g_mainSceneCandidateValid = false;
// Held by a read-only diagnostic that needs this hook installed on a run where
// nothing else armed it. See TickViewmodelCompensation.
std::atomic_bool g_diagnosticHookHold = false;
// Latched on the first attempt and never cleared. See TickViewmodelCompensation.
std::atomic_bool g_diagnosticHookAttempted = false;
// Set while the F2/F3 placement pin is armed. Opens the classification gate in
// the upload hook so the world-FOV match can act. See the note at that gate.
std::atomic_bool g_pinOwnsRenderPath = false;
// The main scene's frustum, republished on every main-scene upload. The
// projection layer declares this, so it must track the FOV setting live rather
// than being sampled once.
// The world pass viewport, and the frustum that goes with it. Kept together
// because a frustum without its rectangle cannot be submitted correctly.
// xr.viewport_full. The game clamps the world pass to a shorter viewport than
// the render target; this widens it and corrects the projection to match.
std::atomic_bool g_viewportFull = false;
// xr.declare_rendered_fov. DEFAULT ON: the projection layer must declare the
// frustum the game was HANDED, not the one it asked for. Off reproduces the
// sharp-2026-08-23 behaviour exactly, which is what makes it an A/B rather
// than a claim -- bare F11, so the wearer can flip it without a run boundary.
std::atomic_bool g_declareRenderedFov = true;
// xr.fit_horizontal. DEFAULT OFF, and the default is the finding: narrowing
// row0 only reaches the main scene, so the effects and viewmodel families are
// left on a frustum 12% wider than the world they are drawn over.
std::atomic_bool g_fitHorizontal = false;
bool g_viewportFullReported = false;
// HANDED vs DECLARED bookkeeping. Plain bools/floats: only the upload hook
// touches them, and it is the same thread that owns every other latch here.
bool g_frustumHandedReported = false;
int g_handedReportCountdown = 0;
float g_lastHandedTanX = 0.0f;
float g_lastHandedRatioX = 0.0f;
// STEREO line bookkeeping. The two counters are frame-boundary only (one
// thread); the published sign/magnitude are written from the upload hook, so
// they are atomics.
std::atomic<float> g_lastAppliedEyeSign = 0.0f;
std::atomic<float> g_lastAppliedEyeUnits = 0.0f;
std::atomic<unsigned int> g_worldEyeOffsetsThisFrame = 0;
unsigned int g_stereoLineFrames = 0;
unsigned int g_stereoLineOffsets = 0;
bool g_aspectFixReported = false;
bool g_frustumFirstReported = false;
// G0b. The runtime's recommended per-eye size, republished here so the aspect-
// matched buffer can be computed beside the frustum that decides its shape.
std::atomic<unsigned int> g_recommendedEyeWidth = 0;
std::atomic<unsigned int> g_recommendedEyeHeight = 0;
std::atomic<float> g_backbufferWidth = 0.0f;
std::atomic<float> g_backbufferHeight = 0.0f;
std::atomic<float> g_lastViewportX = 0.0f;
std::atomic<float> g_lastViewportY = 0.0f;
std::atomic<float> g_lastViewportW = 0.0f;
std::atomic<float> g_lastViewportH = 0.0f;
std::atomic<float> g_mainSceneViewportX = 0.0f;
std::atomic<float> g_mainSceneViewportY = 0.0f;
std::atomic<float> g_mainSceneViewportW = 0.0f;
std::atomic<float> g_mainSceneViewportH = 0.0f;
// xr.world_rect_gate, and the counts that say what it did. See world_rect_gate.h.
std::atomic<bool> g_worldRectGate = true;
std::atomic<unsigned long long> g_worldRectAccepted = 0;
std::atomic<unsigned long long> g_worldRectRejected = 0;
std::atomic<unsigned long long> g_worldRectUnknownTarget = 0;
std::atomic<float> g_mainSceneHalfTanX = 0.0f;
std::atomic<float> g_mainSceneHalfTanY = 0.0f;
// WHEN THE PAIR ABOVE WAS LAST WRITTEN. Read-only; nothing behaves differently
// for it yet.
//
// The two tangents have exactly one write site and NO invalidation anywhere, so
// GetMainSceneHalfTangents() returns true for the rest of the process once a
// single main-scene upload has been seen. The projection layer derives both the
// frustum it declares and the sub-rectangle it submits from them. If the game
// stops producing the uploads this hook recognises -- a cutscene, a death
// camera, anything drawn by a pass the near-plane -7 predicate does not match --
// the layer keeps declaring the last GAMEPLAY frustum over whatever is now in
// the buffer, and the compositor stretches it to fit. That is what "zoomed in"
// would look like, and it is what the 117.5-degree latch in KNOWN-ISSUES 9 would
// look like too.
//
// This stamp exists to find out whether that is what is actually happening
// before anything is changed on the strength of it.
std::atomic<std::uint64_t> g_mainSceneTanStampMs{0};
std::atomic<std::uint64_t> g_mainSceneTanWrites{0};

// W2 -- the lens shear. See camera_update_hook.h for the derivation.
//
// DEFAULT ON. This is a correctness fix for every headset whose optics are not
// centred, and it is an exact identity on every headset whose optics are, so
// there is no configuration in which leaving it off is the safer default. The
// key exists to A/B it in one run, not because the default is in doubt.
std::atomic_bool g_lensShearEnabled{true};
std::atomic<float> g_lensTanUp = 0.0f;
std::atomic<float> g_lensTanDown = 0.0f;
std::atomic<float> g_lensShearAppliedNdcY = 0.0f;
// PER-FAMILY VERIFICATION IS THE ACCEPTANCE CRITERION, NOT AN AFTERTHOUGHT.
//
// The bug this project keeps paying for is a projection edit that reaches the
// main scene and nothing else, so the world moves and the effects and viewmodel
// drawn over it do not. The shear is structurally immune -- it asks no family
// questions -- but "structurally immune" is an argument, and the run has to
// produce a NUMBER. Uploads are counted into buckets by near plane, which is
// exactly how the 2026-08-23 census separated the three perspective families:
//
//   near -7.000  the main scene      near -0.007  world effects
//   near -1.000  the viewmodel       anything else
//
// A bucket that stays at zero while its family is on screen is the defect,
// named rather than described.
std::atomic_uint32_t g_lensShearFamilyCount[4]{};
// EVERY EARLY RETURN GETS A COUNTER.
//
// A probe that returns silently 7135 times and reports nothing has already cost
// this project a session. ComputeLensShearNdcY has five ways to decline, and
// four of them are indistinguishable from "the shear is working" if all anyone
// sees is the absence of a LENS SHEAR line. Each is counted and named, so a run
// where nothing happened says WHICH gate it died at instead of leaving the next
// session to guess between a broken predicate, a runtime that never reported
// its optics, and a headset that simply does not need the fix.
std::atomic_uint32_t g_lensShearDecline[5]{};
constexpr int kShearDeclineDisabled = 0;
constexpr int kShearDeclineOrthographic = 1;
constexpr int kShearDeclineNoLens = 2;
constexpr int kShearDeclineNoFrustum = 3;
constexpr int kShearDeclineCentred = 4;
std::atomic_uint32_t g_lensShearReports = 0;
std::atomic_uint32_t g_lensShearMainSceneSeen = 0;
constexpr int kShearFamilyMainScene = 0;
constexpr int kShearFamilyEffects = 1;
constexpr int kShearFamilyViewmodel = 2;
// Unconditional, see the note at the increment. Indexed by the same buckets.
std::atomic_uint64_t g_uploadFamilyTally[4]{};
constexpr int kShearFamilyOther = 3;

// RENDER THE FRUSTUM THE HEADSET CAN ACTUALLY SHOW.
//
// Measured: the game's main scene renders 123.7 x 92.9 degrees while the
// headset displays 110.0 x 90.0. The projection layer submits the game's real
// frustum, so the geometry is correct -- and then the compositor throws away
// everything outside 110 degrees, because that is all the display covers.
//
// The cost is paid in the only currency that matters here. Of 2560 horizontal
// pixels roughly 1961 land inside the visible cone; the rest are rendered and
// discarded. Against a runtime asking for 4032 x 3648 per eye that leaves the
// image about 2.06x short horizontally and 2.67x short vertically, which is
// most of "the resolution is super bad".
//
// Narrowing the rendered frustum to the headset's own costs no visible content,
// because the content it removes is already being cropped. The same 2560 pixels
// then cover 110 degrees instead of 123.7 -- about 1.31x the angular resolution
// horizontally and 1.05x vertically -- for no change in what is on screen.
//
// This is Task 02's open question ("FOV matching is NOT solved"), and the
// answer is that c_cameraRelativeToClip was the right place: ScaleProjectionRows
// already scales the two rows independently, which is what matching a 1.77
// aspect render to a 1.43 aspect display requires.
// DEFAULT OFF, on the evidence, having been written to be default on.
//
// A/B'd from the chair on bare F11, seven alternating toggles in one session
// with a healthy hook (125k entry calls, 13k corrections): no perceptible
// difference either way. The patch demonstrably works -- the recorded frustum
// reads 110.0 x 90.0 against the game's own 123.7 x 92.9 -- so the arithmetic
// is doing exactly what it was built to do and the result is invisible.
//
// The prediction was ~1.31x angular resolution across. That it cannot be seen
// says the bottleneck is downstream of what we render: the game hands us
// 2560x1440 for an eye the runtime wants 4032x3648 for, and no rearrangement
// of a frustum fixes a shortfall in pixels. Worth revisiting only alongside a
// higher game render resolution, when there would be something to sharpen.
//
// Kept, because it still spends ~23% fewer horizontal pixels on scene the
// display crops, which may matter for GPU headroom -- but that is untested,
// and an untested benefit does not justify patching the main scene's
// projection by default.
// True while the world's frustum is narrower than its resting width.
//
// 0.93 was set from the rifle, which narrows to about 0.50 of resting -- so far
// past any threshold that it made the threshold look unimportant. The PISTOL
// barely zooms and landed on the wrong side of it, so ADS was detected for
// rifles and missed for pistols, and the weapon stayed stereo with neither eye
// lining up exactly where it mattered.
//
// The bound that matters is the other one: how much the frustum jitters at
// rest. Measured, that is under half a percent (123.4 against 123.7 degrees, a
// tangent ratio of 0.995). 0.98 clears that comfortably while catching a zoom
// far shallower than a pistol's.
//
// Configurable because it is a threshold set from two measurements, and the
// next weapon that behaves differently should be a config change and not a
// rebuild.
// FOVSIG -- READ-ONLY. It changes nothing; it records the two candidate zoom
// signals continuously instead of only on an edge.
//
// WHY IT EXISTS. The 2026-08-21 session had the gun go flat and stay flat, and
// the log says exactly why: `g_restingWorldTanX` is an ALL-TIME running maximum
// (see its update above -- "the widest frustum seen is the un-zoomed one"), so
// any transient that widens the world FOV raises the reference permanently.
// That session's frustum ramped 124.7 -> 133.1 -> 124.2 degrees and then held
// at 124.2 for its entire second half. 124.2/133.1 = 0.933, while leaving a
// zoom needs threshold*1.015 = 0.995. The classifier latched ON and could not
// release, so `weaponMonoPass` stayed true, `eyeSign` stayed 0, and the weapon
// was drawn from the centre viewpoint in BOTH eyes -- one image, no parallax --
// while the world kept its alternating sign and stayed stereo. That is the
// report, mechanism and all: world 3D, gun identical in each eye. It is the
// only session in the archive that entered ZOOMED and never left; every other
// one balances (zoomed=1 unzoomed=1, or 8 and 22).
//
// WHY THIS IS A PROBE AND NOT A FIX. The baseline cannot be repaired without
// choosing a rule, and every candidate rule needs a constant this repo does not
// yet have a measurement for. Worse, the world FOV alone CANNOT separate "held
// zoomed" from "held at rest" -- both are stable plateaux -- so the reference
// has to come from somewhere else. The code's own comment on g_restingWeaponTanX
// says the weapon frustum is the better candidate and has "never been measured
// un-confounded", and asks the next session to choose from evidence. There is
// exactly ONE sample of that comparison so far, taken at the latching edge:
// world 0.978 of resting against a 0.980 threshold -- a coin flip -- while the
// WEAPON read 0.811. Nine times the separation, from one sample.
//
// One sample is not a choice. This logs both signals, and both references, once
// a second for a whole session, so the next decision rests on the distribution
// across sprint, walk and ADS rather than on one edge. In particular it answers
// the question that decides everything: does the WEAPON reference get poisoned
// by the same transients the world one did?
//
// COST IS BOUNDED BY CONSTRUCTION: one line per second whatever the frame rate.
// IT PROVES IT IS WATCHING: `n` is the sample count and each signal reports its
// min and max over the second, so a second in which nothing moved is visibly
// different from an instrument that stopped reading.
void NoteFovSignals(float worldTanX) {
    const float worldRest = g_restingWorldTanX.load(std::memory_order_acquire);
    const float weaponNow = g_weaponTanX.load(std::memory_order_acquire);
    const float weaponRest = g_restingWeaponTanX.load(std::memory_order_acquire);

    static std::uint64_t samples = 0;
    static float worldMin = 1e9f, worldMax = 0.0f;
    static float weaponMin = 1e9f, weaponMax = 0.0f;
    static std::uint64_t nextReport = 0;

    ++samples;
    if (worldTanX < worldMin) worldMin = worldTanX;
    if (worldTanX > worldMax) worldMax = worldTanX;
    if (weaponNow < weaponMin) weaponMin = weaponNow;
    if (weaponNow > weaponMax) weaponMax = weaponNow;

    const std::uint64_t now = GetTickCount64();
    if (nextReport == 0) { nextReport = now + 1000; return; }
    if (now < nextReport) return;
    nextReport = now + 1000;

    const float worldRatio = worldRest > 0.0001f ? worldTanX / worldRest : 0.0f;
    const float weaponRatio = weaponRest > 0.0001f ? weaponNow / weaponRest : 0.0f;
    char line[380]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] FOVSIG n=%llu | WORLD tan %.4f (min %.4f max %.4f) rest %.4f ratio %.3f "
        "| WEAPON tan %.4f (min %.4f max %.4f) rest %.4f ratio %.3f | zoomed=%d\n",
        static_cast<unsigned long long>(samples),
        static_cast<double>(worldTanX), static_cast<double>(worldMin),
        static_cast<double>(worldMax), static_cast<double>(worldRest),
        static_cast<double>(worldRatio),
        static_cast<double>(weaponNow), static_cast<double>(weaponMin),
        static_cast<double>(weaponMax), static_cast<double>(weaponRest),
        static_cast<double>(weaponRatio),
        WorldIsZoomed() ? 1 : 0);
    Tf2VrLog(line);
    samples = 0;
    worldMin = 1e9f; worldMax = 0.0f;
    weaponMin = 1e9f; weaponMax = 0.0f;
}

bool WorldIsZoomed() {
    // THE ENGINE ANSWERS THIS NOW, WHEN IT CAN.
    //
    // Everything below divides the rendered frustum by an ALL-TIME MAXIMUM, and
    // FOVSIG's own comment records a whole session lost to one transient
    // latching that reference: it entered ZOOMED and could never leave. The
    // hysteresis underneath was added because "the PISTOL barely zooms and
    // landed on the wrong side of it" -- and the pistols turn out not to zoom AT
    // ALL, measured 1.01x from the engine's own fov. No threshold was ever going
    // to separate them; it was a workaround for a correct measurement.
    //
    // The frustum path stays as the FALLBACK, so this cannot quietly become a
    // no-op if the engine source ever fails to resolve.
    AdsZoomState engineZoom;
    if (GetAdsZoom(&engineZoom)) {
        const bool fromEngine = AdsEngagedByEngine();
        g_zoomLatched.store(fromEngine, std::memory_order_relaxed);
        return fromEngine;
    }
    const float resting = g_restingWorldTanX.load(std::memory_order_acquire);
    const float current = g_mainSceneHalfTanX.load(std::memory_order_acquire);
    if (resting <= 0.0001f || current <= 0.0001f) return false;
    // HYSTERESIS, because the pistol sits on the threshold.
    //
    // Measured: the pistol narrows the world from 123.7 to about 122.3 degrees
    // -- a tangent ratio of 0.973 against a 0.980 threshold. It clears it by
    // seven thousandths, so ordinary frame-to-frame variation carries it back
    // and forth and the weapon flickers between centre and stereo. Twenty-eight
    // edges in one session, which is what "the pistol does not trigger" looked
    // like from the chair: not a miss, a chatter.
    //
    // Entering a zoom is judged at the threshold; leaving it needs a further
    // 1.5% of recovery. A state this shallow needs the two edges separated.
    const float threshold = g_zoomThreshold.load(std::memory_order_relaxed);
    const bool wasZoomed = g_zoomLatched.load(std::memory_order_relaxed);
    const float bound = wasZoomed ? threshold * 1.015f : threshold;
    const bool zoomed = current < bound * resting;
    g_zoomLatched.store(zoomed, std::memory_order_relaxed);
    return zoomed;
}

std::atomic_bool g_matchHeadsetFov = false;
std::atomic<float> g_headsetHalfTanX = 0.0f;
std::atomic<float> g_headsetHalfTanY = 0.0f;

// The body/weapon pass has its own FOV, fixed and independent of the game's FOV
// setting: at slider 110 the world renders 123.7 x 92.9 degrees while the
// weapon still renders 86.1 x 55.4. A projection layer applies ONE frustum to
// the whole submitted image, so the weapon is displayed across an angle 1.68x
// wider than it was drawn for -- magnified, and with visibly wrong
// foreshortening. Forcing it onto the world's frustum fixes both.
//
// This is not projection-layer-specific: the quad path displays the image
// uniformly too, so the weapon has always been oversized relative to the world.
// The quad simply halved everything, which hid it.
// Defaults ON: headset-confirmed to fix the weapon, and it is a correctness fix
// in both presentation paths rather than a preference. HOME still toggles it.
std::atomic_bool g_viewmodelMatchWorldFov = true;
// A HUD scale knob lived here briefly and is removed. It selected the
// orthographic near-identity family, which turns out NOT to be the HUD: from
// the headset it scaled "some other layer that might have lighting or shadows"
// and produced artifacts along the screen edges, i.e. it was catching
// full-screen effect passes. The reticle was untouched.
//
// The HUD needs its own identification pass, and there is already a lead: the
// near-origin perspective family is the world-anchored HUD (a translation on it
// removed every HUD element except the reticle), so the reticle is elsewhere
// again. Leaving a control that visibly damages the image is worse than having
// none.
// A WORLD-ANCHORED HUD CONTROL LIVED HERE AND IS REMOVED. It shifted and
// scaled the near-origin PERSPECTIVE family, on the 2026-08-16 lead that a
// translation on that family "removed every HUD element except the reticle".
//
// Measured in the headset, that lead does not hold for this build: 30 units of
// shift and a 25 per cent zoom, with the falsifier proving 1653 uploads
// adjusted, moved NOTHING the wearer could see. The family is real and it is
// touched -- it is simply not where the HUD is drawn.
//
// What IS the HUD, established the same day: the pixel-coordinate orthographic
// pass below. Do not rebuild this one without new evidence; the counter said it
// was working while the wearer said it did nothing, which is the whole reason
// the count is logged.

// THE SCREEN-SPACE 2D PASS -- found by the census, and the reticle's best
// candidate.
//
// The census turned up an orthographic family nobody had named, distinct from
// the near-identity one the failed knob caught:
//
//   ortho  |r0|=0.000496  |r1|=0.000882  origin 0   n=3884
//
// Those are 2/4032 and 2/2268 -- the render's own width and height. It is an
// orthographic projection in PIXEL COORDINATES, which is what a 2D screen-space
// pass is. And it is exactly what `IsUiCamera` cannot select: that predicate
// requires both row lengths within 10% of 1.0, and these are three orders of
// magnitude away. So the earlier experiment caught the near-identity family
// (full-screen effects, |r|=1.0), damaged the image, and never touched this one
// -- which is precisely why it reported "the reticle was untouched".
//
// Units here are PIXELS, not world units, so this target carries its own
// values and its own step.
std::atomic<float> g_hud2dShiftX = 0.0f;
std::atomic<float> g_hud2dShiftY = 0.0f;
std::atomic<float> g_hud2dZoom = 1.0f;
// hud2d.head_anchor: shift the pixel-ortho pass every frame so the engine's
// aim anchor lands at the pass centre. See the block beside hud2dAdjust.
std::atomic<bool> g_hud2dHeadAnchor = false;
std::atomic_uint64_t g_hud2dHeadAnchorApplied = 0;
std::atomic_uint64_t g_hud2dHeadAnchorNoAim = 0;
std::atomic_uint64_t g_hud2dAdjustedCount = 0;
// Where the compensation believes the reticle is, in pixels. Logged so a
// mis-signed projection shows up as a number beside the wearer report rather
// than as "it still moves a bit".
volatile float g_reticleAnchorX = 0.0f;
volatile float g_reticleAnchorY = 0.0f;
volatile float g_reticleYawOffset = 0.0f;
volatile float g_reticlePitchOffset = 0.0f;
// C1, the aim census. The pixels above are the ANSWER; these are the working
// that produced it, and without them a wrong anchor and a right one look the
// same in a log.
//
// The pass extents matter more than they look. The anchor converts an angle to
// NDC with the WORLD's half-tangents and then to pixels with the HUD pass's
// own width and height -- which is only correct if the two passes cover the
// same rectangle. Nothing has ever checked that they do, and the two headsets
// render into differently shaped targets, so the census prints both and the
// question stops being an assumption.
volatile float g_hud2dPassWidth = 0.0f;
volatile float g_hud2dPassHeight = 0.0f;
volatile float g_reticleDivisorX = 0.0f;
volatile float g_reticleDivisorY = 0.0f;
volatile int g_reticleHadAim = 0;
std::atomic_uint64_t g_reticleAnchorUpdates = 0;

// READ-ONLY CENSUS OF THE UPLOAD FAMILIES.
//
// The reticle is in neither family this file can currently name: the ortho
// near-identity one is full-screen effects, and a translation on the
// near-origin perspective one removed every HUD element EXCEPT the reticle. So
// there is a third family and nobody has identified it.
//
// This tallies the distinct signatures of every camera-sized upload -- near
// plane, m15, origin bucket, projection row lengths -- so one run says what the
// families actually are instead of another round of predicate guessing. It
// writes nothing and is bounded by construction: a fixed table, and entries are
// only added, never scanned per draw beyond a short linear walk.
struct UploadFamily {
    bool used;
    float nearPlane;
    float element15;
    float origin[3];
    float rowLength0;
    float rowLength1;
    std::uint64_t count;
};
// 24 WAS A SILENT TRUNCATION WAITING TO HAPPEN, and the run that would have hit
// it is the next one.
//
// A family is split by near plane, ortho-ness, BOTH row lengths and an origin
// within 64 units. At a gun range that is one family per weapon per hip/ADS
// state before anything else is counted -- eight weapons is sixteen -- plus the
// HUD, the skybox, the viewmodel family, and a fresh set every time the player
// walks 64 units. The insertion loop below simply fell off the end when the
// table was full: no counter, no outcome, and a census that looked complete.
//
// "A cap hit exactly is truncation" is written down in this project because a
// previous run reported held=12 against a cap of 12 and nobody noticed for
// twelve runs. So: a cap with room, and a DROP COUNT that the report prints
// through ScanOutcome, so a truncated census says so on its face.
constexpr int kUploadFamilyMax = 96;
UploadFamily g_uploadFamilies[kUploadFamilyMax]{};
std::uint64_t g_uploadFamilyDropped = 0;
std::atomic_bool g_uploadCensusEnabled = false;

constexpr float kBodyOriginThreshold = 20.0f;
constexpr float kMainSceneNearPlane = -7.0f;
// NOT A NEAR PLANE. Element 11 of camera-to-clip is near*far/(far-near): -7.0
// only while the far plane is large. The archive shows it drifting to -6.86 in
// play, and 2026-09-10 23:42 it sat at -6.308 for a whole session -- a far
// plane of ~64 units at the first level's continue point -- and the 0.5
// tolerance rejected the world camera outright: no frustum, no projection
// layer, a flat frame and a wild gun. The next families sit at -4, -2 and -1,
// so 1.5 keeps them out and admits any far plane above ~26 units.
constexpr float kMainSceneNearTolerance = 1.5f;


bool OriginsClose(const float* a, const float* b, float threshold) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz <= threshold * threshold;
}

void TallyOrigin(const float* origin) {
    for (auto& entry : g_originTable) {
        if (entry.used && OriginsClose(entry.origin, origin, 1.0f)) { ++entry.count; return; }
    }
    for (auto& entry : g_originTable) {
        if (!entry.used) {
            entry.used = true; entry.count = 1;
            std::memcpy(entry.origin, origin, sizeof(entry.origin));
            return;
        }
    }
}
std::atomic_uint32_t g_offsetAppliedCount = 0;
// The compensation runs inside the selected-upload path, so the existing
// "shifted" count cannot say whether the correction itself executed. These
// separate the three ways it can come out looking like nothing happened:
// the family was never selected, it was selected but the head data was not
// valid, or it ran and the maths is wrong.
std::atomic_uint32_t g_compensationAppliedCount = 0;
std::atomic_uint32_t g_compensationSkippedCount = 0;
std::atomic_uint32_t g_compensationTornReadCount = 0;
float g_lastCompensationAngles[6]{};
std::atomic_uint32_t g_offsetCandidateCount = 0;
// Always-on, independent of any trace or arming. These three separate the ways
// the correction can come out looking like nothing happened:
//   invocations == 0        -> the entry we patched is not the one being called
//   contextMatched == 0     -> it is called, but on a context that is not ours
//   cameraSized == 0        -> right context, but no 576-byte camera uploads
// Guessing between those has now cost three headset runs.
// The resettable viewport tally the A1 sweep reads. See the note at its only
// write site, in HookRSSetViewports.
constexpr size_t kViewportTallySlots = 8;
struct ViewportTallyEntry { float width; float height; unsigned long long count; };
ViewportTallyEntry g_viewportTally[kViewportTallySlots]{};
size_t g_viewportTallyCount = 0;
unsigned long long g_viewportTallyTotal = 0;
unsigned long long g_viewportTallyOverflow = 0;

// The upload ladder's own stage counters. Racy on purpose: they sit on the
// hottest path in the plugin, they are read once a second at a step boundary,
// and a lost increment cannot change a zero into a non-zero.
// Held by a read-only caller that needs the recorders to keep running. See the
// note at its check in FinishTraceIfQuiescent.
std::atomic_bool g_detourHold{false};

unsigned long long g_cameraSizedSeenRacy = 0;
unsigned long long g_worldPerspectiveSeenRacy = 0;
unsigned long long g_mainSceneSeenRacy = 0;
float g_lastCameraSizedNearPlane = 0.0f;

std::atomic_uint64_t g_hookInvocations = 0;
std::atomic_uint64_t g_hookContextMatched = 0;
std::atomic_uint64_t g_hookCameraSized = 0;
std::atomic_uint64_t g_uploadsByPass[3]{};
// SAME-FRAME STEREO state. See the mid-frame boundary note in HookRSSetViewports.
int g_sameFrameScenePasses = 0;
int g_sameFramePeakPasses = 0;  // high-water mark across the session
Microsoft::WRL::ComPtr<ID3D11Texture2D> g_sameFrameEye0;
std::atomic_bool g_sameFrameEye0Valid{false};
std::atomic_uint64_t g_sameFrameCaptures{0};
std::atomic_uint64_t g_sameFrameCaptureFailures{0};
std::atomic_bool g_sameFrameEyePairWanted{false};
// Uploads that reached a second hooked entry after already being patched by an
// outer one. Non-zero proves the thunks funnel into the implementation.
std::atomic_uint64_t g_hookReentrantCount = 0;
std::atomic_bool g_enabled = false;
std::atomic_uint32_t g_inFlight = 0;
std::atomic_uint32_t g_framesRemaining = 0;
std::atomic_uint32_t g_frameIndex = 0;
std::atomic_uint32_t g_totalCalls = 0;
std::atomic_uint32_t g_constantBufferCalls = 0;
std::atomic_uint32_t g_cameraSizedCalls = 0;
std::atomic_uint32_t g_recordCount = 0;
UpdateRecord g_records[kMaxRecords]{};

std::uint64_t HashBytes(const std::uint8_t* bytes, size_t count) {
    std::uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < count; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

// The exact prologue F2 reported for this build's d3d11.dll:
//   push rbx/rbp/rsi/rdi/r12/r13/r14/r15; sub rsp,0B8h
// Matched in full rather than by shape, so a different d3d11.dll build
// declines to hook instead of displacing bytes that mean something else.
// Sized EXPLICITLY, not by kDisplacedBytes. When that constant grew from 20 to
// 24 to hold the widest variant, this array silently grew with it and was
// zero-padded, so every memcmp against sizeof(kExpectedPrologue) began
// demanding four 0x00 bytes that are not there. Variant A -- the real
// implementation, the one that matters -- could then never match anything.
constexpr std::uint8_t kExpectedPrologue[20] = {
    0x40, 0x53,                                     // push rbx
    0x55,                                           // push rbp
    0x56,                                           // push rsi
    0x57,                                           // push rdi
    0x41, 0x54,                                     // push r12
    0x41, 0x55,                                     // push r13
    0x41, 0x56,                                     // push r14
    0x41, 0x57,                                     // push r15
    0x48, 0x81, 0xEC, 0xB8, 0x00, 0x00, 0x00,       // sub rsp, 0B8h
};

// A SECOND UpdateSubresource implementation, seen at d3d11!...28F0 where the
// one above sits at ...2A00. Both have appeared in vtable slot 48 on this
// machine in different runs of the same build.
//
// This was previously written off as "a different function" purely because the
// prologue did not match, and worked around with a retry loop. That reading was
// wrong: the sixth instruction is `mov rdi, r9`, and r9 carries the FOURTH
// integer argument, which for UpdateSubresource is pDstBox. Saving pDstBox on
// entry is exactly what this method does. It is the same call, compiled
// differently -- plausibly the 11.0 versus 11.1 path.
//
// It also displaces to exactly 20 bytes on whole instruction boundaries
// (5+5+5+1+4), every one position-independent, so the existing trampoline needs
// no change at all.
constexpr std::uint8_t kExpectedPrologueAlt[20] = {
    0x48, 0x89, 0x5C, 0x24, 0x08,                   // mov [rsp+08h], rbx
    0x48, 0x89, 0x6C, 0x24, 0x10,                   // mov [rsp+10h], rbp
    0x48, 0x89, 0x74, 0x24, 0x18,                   // mov [rsp+18h], rsi
    0x57,                                           // push rdi
    0x48, 0x83, 0xEC, 0x70,                         // sub rsp, 70h
};

// A third form, on UpdateSubresource1. Identical to B but with the 7-byte
// `sub rsp, imm32` encoding, so it displaces to 23 rather than 20. Still every
// instruction position-independent.
constexpr std::uint8_t kExpectedPrologueWide[23] = {
    0x48, 0x89, 0x5C, 0x24, 0x08,                   // mov [rsp+08h], rbx
    0x48, 0x89, 0x6C, 0x24, 0x10,                   // mov [rsp+10h], rbp
    0x48, 0x89, 0x74, 0x24, 0x18,                   // mov [rsp+18h], rsi
    0x57,                                           // push rdi
    0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00,       // sub rsp, 80h
};

// THE STACK SIZE IS NOT PART OF THE SHAPE, and pinning it to 0x70 is what has
// kept RSSetViewports unhookable.
//
// Variant B is `mov [rsp+8],rbx / mov [rsp+10],rbp / mov [rsp+18],rsi / push rdi
// / sub rsp, imm8`. Only that last immediate varies, and `sub rsp, imm8` with a
// REX.W prefix is ALWAYS four bytes whatever the immediate, so the displacement
// stays exactly 20 and every displaced instruction stays position-independent.
// Matching the immediate as well was never buying safety, only refusals.
//
// Measured across d3d11.dll: 175 functions carry this shape, with immediates
// 0x20 (82), 0x30 (36), 0x40 (27), 0x50 (12), 0x60 (12) and 0x70 (6). Variant B
// as written accepted the six rarest. RSSetViewports is 0x50 and DrawIndexed is
// 0x60, which is precisely why both have always reported "prologue mismatch".
//
// A mask byte of 0x00 means "any value here"; 0xFF means it must match.
constexpr std::uint8_t kPrologueMaskAlt[20] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF,
    0xFF, 0xFF, 0xFF, 0x00,                         // sub rsp, ANY imm8
};

struct PrologueVariant {
    const std::uint8_t* bytes;
    const std::uint8_t* mask;   // nullptr = every byte must match exactly
    size_t length;
    const char* name;
};
constexpr PrologueVariant kPrologueVariants[] = {
    {kExpectedPrologue, nullptr, sizeof(kExpectedPrologue), "A (push-heavy)"},
    {kExpectedPrologueAlt, kPrologueMaskAlt, sizeof(kExpectedPrologueAlt),
     "B (mov-to-stack, sub rsp imm8)"},
    {kExpectedPrologueWide, nullptr, sizeof(kExpectedPrologueWide), "C (mov-to-stack, sub 80h)"},
};

// Every variant must fit the detour's saved-bytes buffer. This is the check
// that would have caught the zero-padding bug above at compile time, had the
// arrays been sized independently of the buffer in the first place.
static_assert(sizeof(kExpectedPrologue) <= kDisplacedBytes, "variant A exceeds the detour buffer");
static_assert(sizeof(kExpectedPrologueAlt) <= kDisplacedBytes, "variant B exceeds the detour buffer");
static_assert(sizeof(kExpectedPrologueWide) <= kDisplacedBytes, "variant C exceeds the detour buffer");
static_assert(sizeof(kExpectedPrologue) == 20 && sizeof(kExpectedPrologueAlt) == 20 &&
              sizeof(kExpectedPrologueWide) == 23,
              "prologue lengths are load-bearing: they set how many bytes each detour displaces");

const PrologueVariant* MatchPrologue(const std::uint8_t* code) {
    if (!code) return nullptr;
    for (const auto& variant : kPrologueVariants) {
        bool matches = true;
        for (size_t index = 0; index < variant.length && matches; ++index) {
            const std::uint8_t mask = variant.mask ? variant.mask[index] : 0xFF;
            matches = (code[index] & mask) == (variant.bytes[index] & mask);
        }
        if (matches) return &variant;
    }
    return nullptr;
}

bool IsExpectedPrologue(const std::uint8_t* code) { return MatchPrologue(code) != nullptr; }

const char* PrologueVariantName(const std::uint8_t* code) {
    const PrologueVariant* variant = MatchPrologue(code);
    return variant ? variant->name : "unrecognised";
}

void WriteAbsoluteJump(std::uint8_t* destination, const void* target) {
    const std::uint8_t jump[6] = {0xFF, 0x25, 0, 0, 0, 0};
    std::memcpy(destination, jump, sizeof(jump));
    const auto address = reinterpret_cast<std::uintptr_t>(target);
    std::memcpy(destination + sizeof(jump), &address, sizeof(address));
}

// REFUSE TO PATCH A SITE THAT ALREADY HOLDS OUR OWN JUMP.
//
// This is the general safety net for a crash that has now actually happened.
// InstallDetours patches three functions and returns false if ANY of them
// mismatch; the two D3D11 siblings routinely do. A caller that retried on that
// false -- as a diagnostic hold of mine did, 146 times in thirty seconds --
// re-patched the one target that HAD succeeded, captured our own jump as the
// "original bytes", and built a trampoline that jumped straight back to the
// interceptor. Infinite recursion, stack overflow, access violation.
//
// The bytes are unmistakable: FF 25 00 00 00 00 is jmp qword ptr [rip+0], and
// nothing in d3d11.dll starts that way. Detecting it here protects every hook
// in this project rather than only the caller that got caught.
bool AlreadyOurDetour(const std::uint8_t* code) {
    return code && code[0] == 0xFF && code[1] == 0x25 && code[2] == 0 && code[3] == 0 &&
           code[4] == 0 && code[5] == 0;
}

bool PrepareDetour(EntryDetour& detour, void* target) {
    auto* code = static_cast<std::uint8_t*>(target);
    if (AlreadyOurDetour(code)) {
        char line[220]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] REFUSING to re-patch %p: it already holds our own jump. A second patch would "
            "trampoline into itself. Nothing written.\n", target);
        Tf2VrLog(line);
        return false;
    }
    // THE .pdata CHECK, AT RUNTIME, and it is not ceremony.
    //
    // Scanning d3d11.dll offline for variant B's shape found 175 matches, and
    // NINE of them are not function starts at all -- the same bytes occurring a
    // few instructions into a function body (15AD09 inside a function beginning
    // at 15AD00, and eight more). Five percent. Displacing 20 bytes from the
    // middle of a function corrupts it, and the prologue check alone cannot tell
    // the difference.
    //
    // The unwind tables can. RtlLookupFunctionEntry answers where the enclosing
    // function actually begins, so a target that is not its own entry is refused
    // rather than patched. Callers here reach targets through a vtable slot, so
    // this should never fire -- which is exactly why it is worth having: if it
    // ever does, the slot did not hold what we believed.
    ULONG64 imageBase = 0;
    if (const auto* function = RtlLookupFunctionEntry(
            reinterpret_cast<DWORD64>(code), &imageBase, nullptr)) {
        const auto entry = reinterpret_cast<std::uint8_t*>(imageBase + function->BeginAddress);
        if (entry != code) {
            char line[260]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] REFUSING to patch %p: the unwind tables say it is %llu bytes INSIDE the "
                "function that begins at %p. Displacing here would corrupt it. Nothing written.\n",
                target, static_cast<unsigned long long>(code - entry), static_cast<void*>(entry));
            Tf2VrLog(line);
            return false;
        }
    }
    const PrologueVariant* variant = MatchPrologue(code);
    if (!variant) return false;
    detour.trampoline = VirtualAlloc(nullptr, kTrampolineBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!detour.trampoline) return false;
    detour.target = target;
    detour.displaced = variant->length;
    std::memcpy(detour.original, code, variant->length);

    // Every displaced instruction is position-independent, so the trampoline
    // is the prologue verbatim followed by a jump back to the remainder.
    auto* trampoline = static_cast<std::uint8_t*>(detour.trampoline);
    std::memcpy(trampoline, detour.original, detour.displaced);
    WriteAbsoluteJump(trampoline + detour.displaced, code + detour.displaced);
    FlushInstructionCache(GetCurrentProcess(), trampoline, kTrampolineBytes);
    return true;
}

// DrawIndexed is a forwarding thunk, not an implementation entry.  This is the
// same 16-byte displacement and rel32 reconstruction already proven safe in
// d3d11_entry_trace.cpp; it is reproduced here so this bounded correlation
// trace stays independent of that module's own arming.
constexpr size_t kThunkDisplacedBytes = 16;

bool IsExpectedThunk(const std::uint8_t* code) {
    return code && code[0] == 0x48 && code[1] == 0x83 && code[2] == 0xEC &&
        (code[3] == 0x28 || code[3] == 0x38) &&
        code[4] == 0x48 && code[5] == 0x81 && code[6] == 0xC1 &&
        code[7] == 0x28 && code[8] == 0xFF && code[9] == 0xFF && code[10] == 0xFF &&
        code[11] == 0xE8;
}

bool PrepareThunkDetour(EntryDetour& detour, void* target) {
    auto* code = static_cast<std::uint8_t*>(target);
    if (!IsExpectedThunk(code)) return false;
    detour.trampoline = VirtualAlloc(nullptr, kTrampolineBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!detour.trampoline) return false;
    detour.target = target;
    detour.displaced = kThunkDisplacedBytes;
    std::memcpy(detour.original, code, kThunkDisplacedBytes);

    auto* trampoline = static_cast<std::uint8_t*>(detour.trampoline);
    std::memcpy(trampoline, detour.original, 11);
    trampoline[11] = 0x49; trampoline[12] = 0xBB; // mov r11, imm64
    const auto internal = reinterpret_cast<std::uintptr_t>(code + 16) +
        *reinterpret_cast<const std::int32_t*>(code + 12);
    std::memcpy(trampoline + 13, &internal, sizeof(internal));
    trampoline[21] = 0x41; trampoline[22] = 0xFF; trampoline[23] = 0xD3; // call r11
    WriteAbsoluteJump(trampoline + 24, code + kThunkDisplacedBytes);
    FlushInstructionCache(GetCurrentProcess(), trampoline, kTrampolineBytes);
    return true;
}

bool CommitDetour(EntryDetour& detour, const void* hook, const char* registryName) {
    if (!detour.target || !detour.trampoline || detour.displaced < 14) return false;
    auto* code = static_cast<std::uint8_t*>(detour.target);
    DWORD oldProtect = 0;
    if (!VirtualProtect(code, detour.displaced, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    WriteAbsoluteJump(code, hook);
    std::memset(code + 14, 0x90, detour.displaced - 14);
    FlushInstructionCache(GetCurrentProcess(), code, detour.displaced);
    DWORD ignored = 0; VirtualProtect(code, detour.displaced, oldProtect, &ignored);
    // P0-c: both executable ranges this commit created or modified, so a fault
    // in either names its owner instead of `unknown+0x0`.
    RegisterHookSite(registryName, detour.trampoline, kTrampolineBytes);
    RegisterHookSite(registryName, detour.target, detour.displaced);
    return true;
}

void RestoreDetour(EntryDetour& detour) {
    if (detour.target && detour.displaced) {
        DWORD oldProtect = 0;
        if (VirtualProtect(detour.target, detour.displaced, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            std::memcpy(detour.target, detour.original, detour.displaced);
            FlushInstructionCache(GetCurrentProcess(), detour.target, detour.displaced);
            DWORD ignored = 0; VirtualProtect(detour.target, detour.displaced, oldProtect, &ignored);
        }
    }
    // The trampoline is deliberately NOT freed. Foreign-context callers now
    // take a fast path that does not touch g_inFlight, so quiescence no longer
    // proves nobody is inside one -- and freeing executable code out from under
    // a runtime thread is a far worse outcome than leaking a 64-byte page. The
    // same reasoning is already applied on the commit-failure path.
    detour.trampoline = nullptr;
    detour = {};
}

void RecordCameraSizedUpdate(ID3D11Resource* destination, const void* sourceData, const D3D11_BOX* box) {
    const auto ordinal = g_cameraSizedCalls.fetch_add(1, std::memory_order_relaxed);
    const auto index = g_recordCount.fetch_add(1, std::memory_order_relaxed);
    if (index >= kMaxRecords) return;
    const auto* bytes = static_cast<const std::uint8_t*>(sourceData);
    UpdateRecord& record = g_records[index];
    record.frame = g_frameIndex.load(std::memory_order_relaxed);
    record.ordinal = ordinal;
    record.destination = destination;
    record.drawsAtUpdate = g_drawIndexedCount.load(std::memory_order_relaxed);
    if (box) {
        // Only the bytes this call actually supplies may be read.
        record.partial = true;
        record.boxLeft = box->left;
        record.boxRight = box->right;
        record.contentHash = box->right > box->left ? HashBytes(bytes, box->right - box->left) : 0;
        return;
    }
    record.contentHash = HashBytes(bytes, kCameraBufferBytes);
    record.matrixHash = HashBytes(bytes + kCameraRelativeToClipOffset, 64);
    std::memcpy(record.origin, bytes + kCameraOriginOffset, sizeof(record.origin));
    std::memcpy(record.matrixRow3, bytes + kCameraRelativeToClipOffset + 12 * sizeof(float), sizeof(record.matrixRow3));
    float row0[3]{};
    std::memcpy(row0, bytes + kCameraRelativeToClipOffset, sizeof(row0));
    record.row0Length = std::sqrt(row0[0] * row0[0] + row0[1] * row0[1] + row0[2] * row0[2]);
    float row1[3]{};
    std::memcpy(row1, bytes + kCameraRelativeToClipOffset + 4 * sizeof(float), sizeof(row1));
    record.row1Length = std::sqrt(row1[0] * row1[0] + row1[1] * row1[1] + row1[2] * row1[2]);
    std::memcpy(record.matrixRow2, bytes + kCameraRelativeToClipOffset + 8 * sizeof(float),
                sizeof(record.matrixRow2));
}

// Selects the perspective, world-positioned camera families and nothing else.
// Element 15 of c_cameraRelativeToClip is 0 for the perspective cameras and 1
// for the identity/orthographic UI and shadow uploads, which is what keeps the
// 17 UI passes and the shadow cascades untouched.  The near-zero-origin
// perspective upload is excluded by the origin magnitude test.
// The complement of IsWorldPerspectiveCamera: perspective (m[15]==0) but sitting
// at the origin rather than out in the world. In the HOME trace this is upload
// #28, origin ~(0, 0.015, 0.005) with an essentially identity forward and the
// same FOV as the world camera -- the signature of a pass rendered in camera
// space, which is what a viewmodel is. Selecting it is the whole point of this
// probe: it is a guess until the gun is seen to move.
// The viewmodel candidate: a world-positioned perspective upload whose near
// plane is 1 rather than the world's 7.
//
// m[11] carries the near plane (m[15] is 0 and rendering is reversed-Z), and
// it is the only stable difference between the two player-origin families:
// both share the player's origin, the same forward axis and the same |r0|
// (same FOV), which is why every earlier attempt to separate them failed.
// Measured over two captures: main scene -7.0 (bursts 6/110/52), this family
// -1.0 (bursts 10/0/2), 3D skybox -0.007.
//
// The world-positioned test excludes the HUD pass at the origin. The "lower
// camera" 60 units below the player also carries -1 and is not excluded; it
// is a single draw, and for an identification probe that is acceptable.
// Source's own AngleVectors, in Source's order and signs. Rows of the returned
// basis are forward/right/up. Both orientations are built with this one
// function so the compose happens in a single algebra.
void AngleBasis(float pitchDeg, float yawDeg, float rollDeg, float basis[9]) {
    constexpr float toRad = 0.01745329252f;
    const float sp = std::sin(pitchDeg * toRad), cp = std::cos(pitchDeg * toRad);
    const float sy = std::sin(yawDeg * toRad), cy = std::cos(yawDeg * toRad);
    const float sr = std::sin(rollDeg * toRad), cr = std::cos(rollDeg * toRad);
    basis[0] = cp * cy;                    // forward
    basis[1] = cp * sy;
    basis[2] = -sp;
    basis[3] = -sr * sp * cy + cr * sy;    // right
    basis[4] = -sr * sp * sy - cr * cy;
    basis[5] = -sr * cp;
    basis[6] = cr * sp * cy + sr * sy;     // up
    basis[7] = cr * sp * sy - sr * cy;
    basis[8] = cr * cp;
}

bool IsWorldPerspectiveCamera(const std::uint8_t* bytes);
bool IsViewmodelCandidateCamera(const std::uint8_t* bytes);
bool IsNearOriginPerspectiveCamera(const std::uint8_t* bytes);
float NearPlaneElement(const std::uint8_t* bytes);
bool ReadLatchedAngles(float* base, float* applied, std::uint32_t* generation);
bool ReadCorrectionAngles(float* base, float* applied, std::uint32_t* generation);
bool MatchingHeadsetFov(float& halfTanX, float& halfTanY);
bool IsMainSceneCamera(const std::uint8_t* bytes);

// Cancels the head rotation on the body/weapon pass, so it stays in the body's
// frame while the camera turns. Rendering is camera-relative, so rotating p
// about the origin is rotating about the camera.
//
// Applied as M' = M * Rel on the xyz columns, with
//
//     Rel = R_base^T * R_written
//
// The operand order is pinned by measurement, not by derivation. Deriving it
// from "the matrix rows are the camera basis" gives Rel = R_written^T * R_base,
// which for pure yaw expands to Rz(+delta) -- the opposite of the Rz(-delta)
// that the yaw-only version demonstrably pinned the weapon with. That version
// left the weapon free again with 2396 corrections applied and 0 skipped, which
// is how it was diagnosed. The row-vs-column reading of the uploaded matrix is
// the reverse of what that derivation assumed.
//
// Both orientations go through one AngleBasis so the compose happens in a
// single algebra. An earlier version rotated about world Z alone, which pinned
// the weapon horizontally and left it riding the head vertically -- the
// partial-algebra trap the prior-art report warns about.
//
// Column 3 is untouched: the head write rotates the view, it does not move the
// camera.
// Bounded per-upload trace of the correction. Records whole frames, so the four
// corrected uploads of a frame can be compared against each other and against
// the same upload in the next frame. With a constant synthetic pose every one of
// these numbers should be identical; any that is not is the defect, stated as a
// number rather than described as a flicker.
std::atomic_uint32_t g_correctionTraceFrames = 0;
std::atomic_uint32_t g_correctionTraceOrdinal = 0;
std::atomic_uint32_t g_correctionTraceFrameIndex = 0;
// The uploads the correction did NOT touch, over the same window.
//
// The trace only ever recorded corrected uploads, which cannot tell the two
// remaining failure modes apart: the rotation being wrong on the passes we
// select, or the rotation being right on passes that do not draw the gun while
// the pass that does goes untouched. Three uploads a frame are corrected out of
// roughly thirty camera-sized ones, and nothing has ever looked at the other
// twenty-seven. Each is recorded with the two numbers the selectors key on --
// the near-plane element and the origin -- so a viewmodel pass sitting just
// outside the near-plane-1 test names itself instead of being invisible.
std::atomic_uint32_t g_censusOrdinal = 0;

// ONE SET OF ANGLES PER FRAME, PINNED AT THE MAIN SCENE.
//
// The correction used to read "the most recent latch" at each upload, which is
// not the same thing as "the angles this frame was built with". Measured over
// 30 frames while the head was moving, the generation the uploads read went:
//
//   4196 4198 4200 4202 4202 4206 4208 4210 4212 4212 4216 ...
//
// -- every fourth or fifth frame repeats a generation and the next one skips
// by two. So a frame is periodically corrected with the previous frame's
// angles while the world in it was projected with the current ones, and the
// frame after that is corrected with angles a whole view-build stale. The
// error is zero when the head is still and grows with head speed, which is
// exactly the residual wobble that survived every earlier fix.
//
// Within a frame the four uploads were always coherent (identical generations),
// so the defect is between frames, not inside one.
//
// The main-scene upload precedes the near-plane-1 family within a frame -- the
// origin tracking above already depends on that ordering -- so the angle pair
// is snapshotted there and every corrected upload of the frame uses that one
// snapshot. The gun is then rotated by exactly the rotation the scene behind it
// was projected with, which is the property that actually has to hold.
// NOW DEFAULTS TO THE ENGINE-LATCHED PAIR, and the reason is a change made
// elsewhere rather than a change of mind about the argument below.
//
// The written YAW is now summed inside the detour, on the RENDER frame, as
// `fresh base + delta` -- that fixed a genuine cross-clock bug where the
// engine's current yaw was clobbered with a stale one. But the published pair
// (g_headWrittenAngles) is still formed on the PLUGIN frame from the base it
// last saw, so the two now disagree by exactly omega * dt: zero standing still,
// growing with turn speed.
//
// The correction's whole job is to cancel WHAT WAS ACTUALLY WRITTEN. Cancelling
// a slightly different rotation leaves the weapon lagging the view during a
// turn and disagreeing between the uploads of one frame -- reported as the gun
// lagging with a few flashes while rotating, a regression against a symptom
// that had previously been fixed.
//
// The detour's own pair cannot drift that way: base and applied are both
// latched around its write, so `applied` includes the render-frame yaw sum by
// construction. That makes it the only source consistent with what the camera
// now receives.
//
// The argument below is not wrong, it is about a different concern -- which
// pose the COMPOSITOR reprojects to on held frames. If the old held-frame
// symptom returns (the weapon moving with the head for a moment and then
// snapping back), set viewmodel.display_pose_correction = 1 to restore this,
// and the yaw sum should be reverted instead: the two cannot both be right.
//
// Build Rel from the pose the compositor will reproject to (published every
// frame) rather than the one the engine rendered with (held every fourth).
// See ReadCorrectionAngles.
std::atomic_bool g_useDisplayPoseForCorrection = false;
std::atomic_bool g_frameLockedAngles = true;
std::atomic_bool g_frameAnglesValid = false;
float g_frameAngles[6]{};  // base p,y,r then applied p,y,r
std::atomic_uint32_t g_frameAngleGeneration = 0;

// Seqlock read of the pair the camera detour latched. Returns false if it could
// not get a torn-free pair, in which case the caller must not build a rotation
// from it.
bool ReadLatchedAngles(float* base, float* applied, std::uint32_t* generation) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint32_t before = g_cameraAngleGeneration;
        if (before & 1u) continue;  // a latch is in progress
        base[0] = BitsToFloatLocal(g_cameraBasePitchBits);
        base[1] = BitsToFloatLocal(g_cameraBaseYawBits);
        base[2] = BitsToFloatLocal(g_cameraBaseRollBits);
        applied[0] = BitsToFloatLocal(g_cameraAppliedPitchBits);
        applied[1] = BitsToFloatLocal(g_cameraAppliedYawBits);
        applied[2] = BitsToFloatLocal(g_cameraAppliedRollBits);
        if (g_cameraAngleGeneration != before) continue;
        if (generation) *generation = before;
        return true;
    }
    return false;
}

// Seqlock read of the pair head tracking PUBLISHED, which is the pose the
// compositor will reproject to. Same [pitch, yaw, roll] layout as the latch.
bool ReadPublishedHeadAngles(float* base, float* applied, std::uint32_t* generation) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint32_t before = g_headAnglesGeneration;
        if (before & 1u) continue;
        base[0] = g_headBaseAngles[0];
        base[1] = g_headBaseAngles[1];
        base[2] = g_headBaseAngles[2];
        applied[0] = g_headWrittenAngles[0];
        applied[1] = g_headWrittenAngles[1];
        applied[2] = g_headWrittenAngles[2];
        if (g_headAnglesGeneration != before) continue;
        if (generation) *generation = before;
        return true;
    }
    return false;
}

// CANCEL THE ROTATION THE COMPOSITOR WILL REPROJECT TO, NOT THE ONE THE ENGINE
// HAPPENED TO RENDER WITH.
//
// Measured: headGen advances every single frame, gen holds every fourth. We
// publish a fresh pose per frame; the engine reuses a view build. Working
// through what each choice leaves on screen, with W the engine's written
// orientation, B the aim, H_engine the head rotation baked into W, and
// H_display the pose the compositor reprojects to:
//
//   gun rendered at   W - Rel
//   timewarp adds     H_display - H_engine
//   gun ends up at    B + (H_engine - Rel) + (H_display - H_engine)
//
// With Rel = H_engine -- the engine latch, what we do today -- that leaves
// H_display - H_engine, which is zero only when the engine's write is current
// and grows with head speed on every held frame. With Rel = H_display it
// cancels exactly, on held frames too.
//
// The world does not show this because the compositor reprojects it wholesale;
// the gun does, because its counter-rotation is baked into the pixels.
//
// This returns to a source a previous session tried and reverted. The revert
// reasoned that the published angles "can be newer than the ones the engine
// actually wrote", and treated newer as the defect. Newer is the point: newer
// is what the compositor is going to use.
bool ReadCorrectionAngles(float* base, float* applied, std::uint32_t* generation) {
    if (g_useDisplayPoseForCorrection.load(std::memory_order_relaxed) &&
        ReadPublishedHeadAngles(base, applied, generation)) {
        return true;
    }
    return ReadLatchedAngles(base, applied, generation);
}

// Pins this frame's angles at the main-scene upload, which precedes the
// near-plane-1 family. Everything corrected afterwards in this frame rotates by
// the same Rel as the world did.
void PinFrameAngles() {
    // ONCE per frame, not once per matching upload.
    //
    // "Pin at the main-scene upload" assumed there is one such upload a frame.
    // Where a 3D SKYBOX is visible there are two: the sky camera renders the
    // distant vista with the same FOV and near plane as the world, so it passes
    // IsMainSceneCamera identically -- this file already knows it exists, and
    // excludes it from the eye offset by origin distance for exactly that
    // reason. The measured two view builds per submitted frame are these two
    // passes.
    //
    // Each pass carries its own head-angle write, so the generation advances
    // between them, and re-pinning on the second handed the weapon a different
    // rotation from the world it is drawn over. That is a stutter confined to
    // the gun, appearing only where a distant vista is in view and not when
    // aiming at something near -- which is exactly how it was reported.
    //
    // Consistency within the frame is the property that matters, so the first
    // pin wins and the rest of the frame reads it. Whether first or last is the
    // better of the two is a real question and not settled here; either is
    // coherent, and only one of them can be wrong in a way that shows.
    // FIRST OR LAST, AND THE ANSWER IS NEITHER.
    //
    // Last (the original) stuttered when a distant vista was in view. First
    // (the skybox fix) stopped that and started stuttering on CLOSE targets
    // instead. The behaviour flipped, which means the ordinal was never the
    // right way to choose: one of the two main-scene passes is right and it is
    // not reliably the first or reliably the last.
    //
    // The candidate that fits both reports is the 3D skybox, which renders in
    // its own reduced-scale space and so has a WRONG ORIGIN for the world the
    // gun is drawn over. This file already identifies it that way for the eye
    // offset. Mode 2 pins on the pass whose origin is the main scene's, which
    // is a property rather than a position in a sequence.
    //
    // Left selectable and NOT defaulted to a guess: two guesses have each been
    // half right, and a third would be a coin flip presented as a fix. Mode 2
    // is the reasoned candidate; the log now says how many main-scene passes a
    // frame has and where each one is, which is the evidence to choose on.
    const int pinMode = g_pinMode.load(std::memory_order_relaxed);
    if (pinMode == 1) {
        // Last wins: overwrite every time, which is the original behaviour.
    } else if (g_frameAnglesValid.load(std::memory_order_acquire)) {
        return;  // First wins, or mode 2 has already accepted a pass.
    }
    float base[3]{}, applied[3]{};
    std::uint32_t generation = 0;
    if (!ReadCorrectionAngles(base, applied, &generation)) return;
    g_frameAngles[0] = base[0];
    g_frameAngles[1] = base[1];
    g_frameAngles[2] = base[2];
    g_frameAngles[3] = applied[0];
    g_frameAngles[4] = applied[1];
    g_frameAngles[5] = applied[2];
    g_frameAngleGeneration.store(generation, std::memory_order_release);
    g_frameAnglesValid.store(true, std::memory_order_release);
}

void ApplyViewmodelCompensation(std::uint8_t* patched) {
    g_compensationAppliedCount.fetch_add(1, std::memory_order_relaxed);
    // Read the detour-latched pair, not a plugin-frame copy: both halves come
    // from the same pass over the engine's own view build, so they cannot
    // disagree by a frame.
    // The pair the DETOUR latched: exactly what the engine used to build this
    // frame's view, captured on either side of its own write.
    //
    // This is a deliberate return to a source that was tried, reverted, and
    // should not have been. The revert rested on the detour latching several
    // times per rendered frame, which would have let the corrected uploads of
    // one frame disagree -- but the view-build counter then measured 120 latches
    // per 120 frames, exactly one, so that never happens.
    //
    // The reason to prefer it is what the flat harness showed. At a CONSTANT
    // pose the correction is provably stable and exact, so the residual error
    // needs motion. Head tracking publishes on the PLUGIN frame, the engine
    // builds its view on the RENDER frame, and RunFrame is dispatched faster
    // than the render loop -- so the published angles can be newer than the ones
    // the engine actually wrote, by an amount proportional to head velocity, and
    // by exactly nothing when the head is still. The detour's pair cannot drift
    // that way: it is measured at the write, not published alongside it.
    float basePitch = 0.0f, baseYaw = 0.0f, baseRoll = 0.0f;
    float appliedPitch = 0.0f, appliedYaw = 0.0f, appliedRoll = 0.0f;
    std::uint32_t generation = 0;
    bool consistent = false;
    // Prefer the pair pinned at this frame's main-scene upload over whatever
    // the latch holds right now. See the note on g_frameLockedAngles.
    if (g_frameLockedAngles.load(std::memory_order_relaxed) &&
        g_frameAnglesValid.load(std::memory_order_acquire)) {
        basePitch = g_frameAngles[0];
        baseYaw = g_frameAngles[1];
        baseRoll = g_frameAngles[2];
        appliedPitch = g_frameAngles[3];
        appliedYaw = g_frameAngles[4];
        appliedRoll = g_frameAngles[5];
        generation = g_frameAngleGeneration.load(std::memory_order_acquire);
        consistent = true;
    } else {
        float base[3]{}, applied[3]{};
        consistent = ReadCorrectionAngles(base, applied, &generation);
        basePitch = base[0]; baseYaw = base[1]; baseRoll = base[2];
        appliedPitch = applied[0]; appliedYaw = applied[1]; appliedRoll = applied[2];
    }
    // Forwarding unmodified beats forwarding a rotation built from a torn pair.
    if (!consistent) {
        g_compensationTornReadCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_lastCompensationAngles[0] = basePitch;
    g_lastCompensationAngles[1] = baseYaw;
    g_lastCompensationAngles[2] = baseRoll;
    g_lastCompensationAngles[3] = appliedPitch;
    g_lastCompensationAngles[4] = appliedYaw;
    g_lastCompensationAngles[5] = appliedRoll;
    // Rel = base^T * written, the form Task 01b measured. An attempt to shortcut
    // this by consuming the published head delta directly was reverted: it
    // rested on written = base * delta, which is not the composition head
    // tracking performs, so the two are not the same rotation. The measured
    // form is computed from the angles the detour actually latched and stays.
    float rel[9]{};
    float written[9]{}, base[9]{};
    AngleBasis(appliedPitch, appliedYaw, appliedRoll, written);
    AngleBasis(basePitch, baseYaw, baseRoll, base);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            rel[i * 3 + j] = base[0 * 3 + i] * written[0 * 3 + j] +
                             base[1 * 3 + i] * written[1 * 3 + j] +
                             base[2 * 3 + i] * written[2 * 3 + j];
        }
    }
    float matrix[16]{};
    std::memcpy(matrix, patched + kCameraRelativeToClipOffset, sizeof(matrix));
    for (int row = 0; row < 4; ++row) {
        const float m0 = matrix[row * 4 + 0];
        const float m1 = matrix[row * 4 + 1];
        const float m2 = matrix[row * 4 + 2];
        matrix[row * 4 + 0] = m0 * rel[0] + m1 * rel[3] + m2 * rel[6];
        matrix[row * 4 + 1] = m0 * rel[1] + m1 * rel[4] + m2 * rel[7];
        matrix[row * 4 + 2] = m0 * rel[2] + m1 * rel[5] + m2 * rel[8];
    }
    std::memcpy(patched + kCameraRelativeToClipOffset, matrix, sizeof(matrix));

    if (g_correctionTraceFrames.load(std::memory_order_acquire)) {
        const std::uint32_t ordinal = g_correctionTraceOrdinal.fetch_add(1, std::memory_order_acq_rel);
        char line[460]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] corr f%u #%u src=%s gen=%u headGen=%u nearPlane=%.4f: base=(p%.3f y%.3f r%.3f) "
            "applied=(p%.3f y%.3f r%.3f) "
            "rel=[%.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f] outRow0=(%.4f %.4f %.4f)\n",
            g_correctionTraceFrameIndex.load(std::memory_order_acquire), ordinal,
            // gen counts the ENGINE writing its camera angles; headGen counts US
            // publishing a head pose. The written angles hold for a frame and
            // then double-step, so one of these two clocks is slipping against
            // the render loop and this says which: if headGen advances once per
            // frame while gen repeats, the engine is reusing a view; if headGen
            // repeats too, the pose is published too rarely, once per Present,
            // and the fix is to sample it where it is consumed.
            g_useDisplayPoseForCorrection.load(std::memory_order_relaxed) ? "display" : "engine",
            generation, static_cast<unsigned>(g_headAnglesGeneration),
            NearPlaneElement(patched),
            basePitch, baseYaw, baseRoll, appliedPitch, appliedYaw, appliedRoll,
            rel[0], rel[1], rel[2], rel[3], rel[4], rel[5], rel[6], rel[7], rel[8],
            matrix[0], matrix[1], matrix[2]);
        Tf2VrLog(line);
    }
}

// ANCHOR THE HUD TO THE AIM INSTEAD OF THE HEAD.
//
// Exactly the algebra ApplyViewmodelCompensation uses, with the AIM basis in
// place of the body's: rel = X^T * written puts the content into frame X, so
// the weapon correction (X = body) drags the gun back into the body's frame and
// this (X = aim) drags the HUD into the aim's. The panels then sit where the
// gun is pointing and stay there while the head turns, instead of riding the
// headset.
//
// ROLL IS DELIBERATELY DROPPED. The aim carries the wrist's roll, and a HUD
// that rolls with the wrist would be unreadable and unpleasant; yaw and pitch
// are what "centred on where the reticle is facing" means. Gravity stays up.
//
// Falls back to leaving the upload alone whenever the aim is not yet known --
// head-locked is the safe state, and a HUD anchored to a stale aim would swing
// to a direction the player is not pointing.
void ApplyHudAimAnchor(std::uint8_t* patched) {
    float aim[3]{};
    if (!TryGetAimAnglesDegrees(aim)) {
        g_hudAnchorNoAimCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    float basePitch = 0.0f, baseYaw = 0.0f, baseRoll = 0.0f;
    float appliedPitch = 0.0f, appliedYaw = 0.0f, appliedRoll = 0.0f;
    std::uint32_t generation = 0;
    bool consistent = false;
    if (g_frameLockedAngles.load(std::memory_order_relaxed) &&
        g_frameAnglesValid.load(std::memory_order_acquire)) {
        appliedPitch = g_frameAngles[3];
        appliedYaw = g_frameAngles[4];
        appliedRoll = g_frameAngles[5];
        consistent = true;
    } else {
        float base[3]{}, applied[3]{};
        consistent = ReadCorrectionAngles(base, applied, &generation);
        appliedPitch = applied[0]; appliedYaw = applied[1]; appliedRoll = applied[2];
    }
    if (!consistent) {
        g_compensationTornReadCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    (void)basePitch; (void)baseYaw; (void)baseRoll;

    float rel[9]{};
    float written[9]{}, anchor[9]{};
    // ROLL DROPPED ON BOTH SIDES. The engine places this pass's content by the
    // yaw and pitch between attack and view; a head roll in the written basis
    // was the overlay tilting when the head alone moved (wearer, run 5).
    (void)appliedRoll;
    AngleBasis(appliedPitch, appliedYaw, 0.0f, written);
    // aim is [pitch, yaw, roll] in the engine's own layout; roll forced to 0.
    AngleBasis(aim[0], aim[1], 0.0f, anchor);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            rel[i * 3 + j] = anchor[0 * 3 + i] * written[0 * 3 + j] +
                             anchor[1 * 3 + i] * written[1 * 3 + j] +
                             anchor[2 * 3 + i] * written[2 * 3 + j];
        }
    }
    if (g_hudAnchorMode.load(std::memory_order_relaxed) == 2) {
        // The inverse rotation: content the engine placed at the aim comes back
        // to the head. rel is orthonormal, so its inverse is its transpose.
        float t = rel[1]; rel[1] = rel[3]; rel[3] = t;
        t = rel[2]; rel[2] = rel[6]; rel[6] = t;
        t = rel[5]; rel[5] = rel[7]; rel[7] = t;
    }
    float matrix[16]{};
    std::memcpy(matrix, patched + kCameraRelativeToClipOffset, sizeof(matrix));
    for (int row = 0; row < 4; ++row) {
        const float m0 = matrix[row * 4 + 0];
        const float m1 = matrix[row * 4 + 1];
        const float m2 = matrix[row * 4 + 2];
        matrix[row * 4 + 0] = m0 * rel[0] + m1 * rel[3] + m2 * rel[6];
        matrix[row * 4 + 1] = m0 * rel[1] + m1 * rel[4] + m2 * rel[7];
        matrix[row * 4 + 2] = m0 * rel[2] + m1 * rel[5] + m2 * rel[8];
    }
    std::memcpy(patched + kCameraRelativeToClipOffset, matrix, sizeof(matrix));
    g_hudAnchorAppliedCount.fetch_add(1, std::memory_order_relaxed);
}

// Reads a basis published between two generation bumps, retrying on a torn
// write. Same shape as camera_hook.cpp's ReadPublishedBasis; duplicated rather
// than exported because that one lives in its own translation unit's anonymous
// namespace, and a torn basis is not a slightly wrong rotation -- it is not a
// rotation at all, so the check must exist wherever a basis is consumed.
bool ReadPublishedBasisLocal(volatile const float source[9],
                             volatile const std::uint32_t& generation, float out[9]) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint32_t before = generation;
        if (before & 1u) continue;                 // a write is in progress
        for (int i = 0; i < 9; ++i) out[i] = source[i];
        if (generation == before) return true;
    }
    return false;
}

// TASK 05 STEP 4 -- THE GUN PINNED TO THE RIGHT HAND.
//
// THE HANDLE EXISTS, AND THE DOCUMENT THAT SAYS IT DOES NOT PREDATES IT.
//
// RESEARCH-PRIOR-ART-DEEP-DIVE section 6 states that pinning needs a per-object
// transform which "the camera family cannot express... the handle for it does not
// yet exist", because a camera upload moves the whole pass, body and weapon
// together, rigidly. That was written 2026-08-15. On 2026-08-16 commit afc592d
// established, and the comment on g_weaponMonoEye above now records, that the
// viewmodel family is FOUR passes: three world-perspective, and the gun's OWN
// pass rendering at origin (0,0,0). A pass that draws only the gun IS a
// per-object handle for the gun. The document is a day older than the finding.
//
// So this rotates the gun's pass alone by the right controller's rotation,
// relative to a reference captured when it is armed. The other three passes --
// the body and arms -- keep the existing head-cancel and are not touched.
//
// ROTATION ONLY, FOR NOW, AND DELIBERATELY.
//
// Translation needs a metres-to-units scale and a decision about what the gun
// pivots around, and the pass is camera-relative so column 3 rotates the gun
// about the EYE rather than the wrist. Shipping rotation and translation together
// would mean a run that looks wrong cannot say which of them was wrong -- the
// same trap that made the XInput yaw sign ship on its own.
//
// EXPECTED LIMITATION, STATED BEFORE THE RUN. This moves the gun rigidly and
// cannot articulate an arm. If the arms are in the other three passes, the gun
// will follow the hand while the arms stay put. That is either acceptable to
// build on or it is not, and one run settles it -- which is the whole reason to
// try this before writing the engine's evaluated skeleton.
// Bumped once per presented frame, so the bone write can fire on the FIRST
// viewmodel pass of a frame and not on the three that follow it. The write
// applies a delta to whatever is in the array, so a second application in the
// same frame compounds the rotation.
std::atomic_uint32_t g_boneWriteFrameCounter = 0;
std::atomic_uint32_t g_boneWriteLastFrame = 0xFFFFFFFFu;

std::atomic_bool g_weaponPinned = false;
float g_weaponPinReference[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
std::atomic_bool g_haveWeaponPinReference = false;
std::atomic_uint64_t g_weaponPinApplied = 0;
std::atomic_uint64_t g_weaponPinSkipped = 0;

// WHICH PASS IS THE GUN: SELECTED BY FOV, CYCLED IN THE HEADSET.
//
// The first attempt picked the pass by ORIGIN -- viewmodel near plane plus an
// origin at (0,0,0) -- on the strength of HEADSET-ISSUES saying "the gun's own
// pass renders at origin (0,0,0)". Reported from the headset: the gun did not
// move and the HUD did. 00-SHARED-CONTEXT's closed list already said why, and it
// is the entry I should have weighted more heavily than the other: "the
// near-origin perspective family (HOME #28) is the world-anchored HUD, not the
// viewmodel". Origin does not separate the gun from the HUD here.
//
// What does separate the four viewmodel passes is that each has its OWN FIXED
// FOV -- the note on the weapon-frustum zoom signal records "four viewmodel
// passes with four fixed FOVs", which is why an instrument that shared one static
// across them read a spurious 37.7-to-86.1 range.
//
// So the passes are tabulated by their measured FOV as they are seen, and exactly
// one slot is pinned at a time, cycled by a key. Four presses inside one run
// identify the gun, which is an A/B within a session rather than a judgement
// across four of them.
constexpr int kMaxPinSlots = 8;
std::atomic<float> g_pinSlotFov[kMaxPinSlots]{};
std::atomic_uint32_t g_pinSlotOriginNear[kMaxPinSlots]{};   // 1 if origin ~= (0,0,0)
std::atomic_int g_pinSlotCount = 0;
// -1 pins every viewmodel pass, which is the diagnostic that shows what moving
// the whole family looks like. 0..n-1 pin exactly one.
std::atomic_int g_weaponPinSlot = 0;

// Native horizontal FOV of this pass, in degrees, from the projection row length:
// |r0| = 1/halfTan.
float PassNativeFovDegrees(const std::uint8_t* bytes) {
    float row0[3]{};
    std::memcpy(row0, bytes + kCameraRelativeToClipOffset, sizeof(row0));
    const float length = std::sqrt(row0[0] * row0[0] + row0[1] * row0[1] + row0[2] * row0[2]);
    if (length < 0.0001f) return 0.0f;
    return 2.0f * std::atan(1.0f / length) * 57.2957795f;
}

// Registers this pass in the slot table and returns its index. Matched by FOV
// within half a degree, the same tolerance the existing pass tracker uses.
int PinSlotFor(const std::uint8_t* bytes) {
    const float fov = PassNativeFovDegrees(bytes);
    if (fov <= 0.0f) return -1;
    const int count = g_pinSlotCount.load(std::memory_order_acquire);
    for (int slot = 0; slot < count; ++slot) {
        if (std::fabs(g_pinSlotFov[slot].load(std::memory_order_relaxed) - fov) <= 0.5f) return slot;
    }
    if (count >= kMaxPinSlots) return -1;
    g_pinSlotFov[count].store(fov, std::memory_order_relaxed);
    g_pinSlotOriginNear[count].store(IsNearOriginPerspectiveCamera(bytes) ? 1u : 0u,
                                     std::memory_order_relaxed);
    g_pinSlotCount.store(count + 1, std::memory_order_release);
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] weapon pin: viewmodel pass slot %d discovered, native FOV %.1f deg, origin %s.\n",
        count, static_cast<double>(fov), IsNearOriginPerspectiveCamera(bytes) ? "~(0,0,0)" : "world");
    Tf2VrLog(line);
    return count;
}

// Any pass in the viewmodel family. Which of them is actually the gun is what the
// slot cycling is for; origin is now known NOT to answer it.
bool IsPinnablePass(const std::uint8_t* bytes) {
    if (!IsViewmodelCandidateCamera(bytes)) return false;
    const int wanted = g_weaponPinSlot.load(std::memory_order_acquire);
    if (wanted < 0) return true;
    return PinSlotFor(bytes) == wanted;
}

// ---------------------------------------------------------------------------
// SIGHTNDC -- where does each pass put the view axis on screen? 2026-09-04.
//
// In ADS the gun is placed at the rendered view's angles, and the wearer still
// sees it slightly off with the head turned. Placement is proven; what is not
// is the weapon pass's PROJECTION. So: take a point 64 units ahead on the
// view axis (eye + forward of the rendered camera angles) and the same for the
// gun's own axis (its committed origin + forward of its committed angles), and
// push both through the FINAL matrix each pass ships -- the world pass and
// the mono weapon pass. Clip = M * [p - origin, 1] with the translation in
// column 3, the convention the eye offset write above uses; NDC = xy / w.
//
// CONTROL: the world pass must put the view-axis point at ~(0,0) (an eye's
// half-IPD shows as a small x). If it does not, the convention is wrong and
// the weapon numbers are void. The weapon pass's view-axis point is the
// answer: any distance from (0,0) is exactly how far off-centre the gun's
// axis is DRAWN, in NDC, with the head delta and pitch offset beside it.
constexpr float kSightProbeAhead = 64.0f;
float g_sightNdc[2][2][2]{};      // [pass world/weapon][point view/gun][x,y]
float g_sightW[2][2]{};           // clip w, sign says in front of the camera
bool g_sightValid[2] = {false, false};
std::uint64_t g_sightNextReportMs = 0;

bool ProjectShipped(const std::uint8_t* shipped, const float p[3], float ndc[2], float* w) {
    float origin[3]{};
    float m[16]{};
    std::memcpy(origin, shipped + kCameraOriginOffset, sizeof(origin));
    std::memcpy(m, shipped + kCameraRelativeToClipOffset, sizeof(m));
    const float c[4] = {p[0] - origin[0], p[1] - origin[1], p[2] - origin[2], 1.0f};
    float clip[4]{};
    for (int i = 0; i < 4; ++i) {
        clip[i] = m[i * 4 + 0] * c[0] + m[i * 4 + 1] * c[1] + m[i * 4 + 2] * c[2] + m[i * 4 + 3] * c[3];
    }
    if (std::fabs(clip[3]) < 1e-6f) return false;
    ndc[0] = clip[0] / clip[3];
    ndc[1] = clip[1] / clip[3];
    if (w) *w = clip[3];
    return true;
}

void SightNdcProbe(const std::uint8_t* shipped, int pass) {
    float base[3]{}, applied[3]{};
    if (!ReadLatchedCameraAngles(base, applied, nullptr)) return;
    float eye[3]{};
    if (!TryGetMainSceneOrigin(eye)) return;
    float viewBasis[9]{};
    AngleBasis(applied[0], applied[1], applied[2], viewBasis);
    const float pView[3] = {eye[0] + viewBasis[0] * kSightProbeAhead,
                            eye[1] + viewBasis[1] * kSightProbeAhead,
                            eye[2] + viewBasis[2] * kSightProbeAhead};
    float gunPos[3]{}, gunAng[3]{};
    const bool haveGun = TryGetPlacementPosition(gunPos, nullptr) && TryGetPlacementAngles(gunAng, nullptr);
    float pGun[3] = {pView[0], pView[1], pView[2]};
    if (haveGun) {
        float gunBasis[9]{};
        AngleBasis(gunAng[0], gunAng[1], gunAng[2], gunBasis);
        pGun[0] = gunPos[0] + gunBasis[0] * kSightProbeAhead;
        pGun[1] = gunPos[1] + gunBasis[1] * kSightProbeAhead;
        pGun[2] = gunPos[2] + gunBasis[2] * kSightProbeAhead;
    }
    float ndcView[2]{}, ndcGun[2]{}, wView = 0.0f, wGun = 0.0f;
    if (!ProjectShipped(shipped, pView, ndcView, &wView)) return;
    if (!ProjectShipped(shipped, pGun, ndcGun, &wGun)) return;
    g_sightNdc[pass][0][0] = ndcView[0]; g_sightNdc[pass][0][1] = ndcView[1];
    g_sightNdc[pass][1][0] = ndcGun[0];  g_sightNdc[pass][1][1] = ndcGun[1];
    g_sightW[pass][0] = wView; g_sightW[pass][1] = wGun;
    g_sightValid[pass] = true;

    const std::uint64_t now = GetTickCount64();
    if (pass != 1 || now < g_sightNextReportMs) return;
    g_sightNextReportMs = now + 1000;
    if (!g_sightValid[0]) return;
    float headYawDelta = 0.0f, headPitch = 0.0f;
    GetHeadViewDelta(&headYawDelta, &headPitch);
    char line[700]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] SIGHTNDC head yaw delta %+.1f pitch offset %+.1f roll %+.1f | WORLD pass: view-axis point ndc "
        "<%+.4f,%+.4f> (w %.1f) gun-axis <%+.4f,%+.4f> [CONTROL: view-axis must be ~0] | WEAPON pass: view-axis "
        "<%+.4f,%+.4f> (w %.1f) gun-axis <%+.4f,%+.4f> | WEAPON minus WORLD, view-axis <%+.4f,%+.4f> = the drawn "
        "offset of the view axis | gun %s.\n",
        static_cast<double>(headYawDelta), static_cast<double>(ViewPitchOffset()),
        static_cast<double>(applied[2]),
        static_cast<double>(g_sightNdc[0][0][0]), static_cast<double>(g_sightNdc[0][0][1]),
        static_cast<double>(g_sightW[0][0]),
        static_cast<double>(g_sightNdc[0][1][0]), static_cast<double>(g_sightNdc[0][1][1]),
        static_cast<double>(g_sightNdc[1][0][0]), static_cast<double>(g_sightNdc[1][0][1]),
        static_cast<double>(g_sightW[1][0]),
        static_cast<double>(g_sightNdc[1][1][0]), static_cast<double>(g_sightNdc[1][1][1]),
        static_cast<double>(g_sightNdc[1][0][0] - g_sightNdc[0][0][0]),
        static_cast<double>(g_sightNdc[1][0][1] - g_sightNdc[0][0][1]),
        haveGun ? "placement read" : "placement NOT read (view axis used for both)");
    Tf2VrLog(line);
    g_sightValid[0] = g_sightValid[1] = false;
}

// ---------------------------------------------------------------------------
// HUD-PASS FRAME DIGEST -- read-only. WHICH FRAME is the flat HUD pass drawn in?
//
// The wearer (2026-09-10, runs 2 and 3): the black fade panel, the loading
// screen's "Press A" overlay and the reticle are one surface, and it follows
// the RIGHT HAND -- with the placement pin AND the viewmodel correction both
// off (run 3, `autoarm = 4`, `viewmodel.correction = 0`). Nothing of ours moved
// that pass in run 3, so the GAME orients it along something the hand drives:
// the attack angles that aim.cmd = 2 writes are the candidate.
//
// This measures it instead of arguing: for every near-origin perspective
// upload (the HUD pass, by the same predicate hud.fov_match uses), the yaw and
// pitch of its forward axis (row 3 of camera-relative-to-clip) against the
// world pass's, and beside them the aim-minus-written and base-minus-written
// yaw deltas from the angle sources. Whichever explains the HUD-minus-world
// delta best is the frame: HEAD (~0), AIM, or BODY. Per second, printed when
// the verdict changes and on F5. Decides nothing.
// ---------------------------------------------------------------------------
namespace {
struct HudFrameWindow {
    unsigned count = 0;
    double sumHudMinusWorldYaw = 0.0, sumHudMinusWorldPitch = 0.0;
    double sumAimMinusWrittenYaw = 0.0, sumBaseMinusWrittenYaw = 0.0;
    unsigned worldValid = 0, aimValid = 0, anglesValid = 0;
};
HudFrameWindow g_hudFrame;
std::uint64_t g_hudFrameWindowStart = 0;
int g_hudFrameLastVerdict = -1;
unsigned g_hudFrameLines = 0;
unsigned long long g_hudFrameTotal = 0;
constexpr unsigned kHudFrameLineCap = 240;
std::atomic<bool> g_hudFrameMark = false;

float WrapDegrees(float d) {
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

// Forward axis of a camera-relative-to-clip matrix is its w row (row 3): the
// clip w of a camera-relative point is its distance along the view axis. Yaw
// and pitch in Source's convention (x,y horizontal, z up; pitch positive down).
bool ForwardYawPitchDegrees(const float* m, float& yaw, float& pitch) {
    const float fx = m[12], fy = m[13], fz = m[14];
    const float len = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (len < 1e-6f) return false;
    yaw = std::atan2(fy, fx) * 57.2957795f;
    pitch = -std::atan2(fz, std::sqrt(fx * fx + fy * fy)) * 57.2957795f;
    return true;
}

void PrintHudFrameDigest(const char* why, int verdict) {
    const HudFrameWindow& w = g_hudFrame;
    const double n = w.count ? static_cast<double>(w.count) : 1.0;
    static const char* const kVerdict[] = {"HEAD (rides the headset)", "AIM (the right hand)",
                                           "BODY (the engine's view angles)", "NONE of the three"};
    char line[720]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] HUD-PASS FRAME (%s): %u near-origin uploads this window (total %llu) | "
        "HUD minus WORLD: yaw %+.2f pitch %+.2f deg (world valid %u) | AIM minus WRITTEN yaw %+.2f "
        "(aim valid %u) | BODY minus WRITTEN yaw %+.2f (angles valid %u) | VERDICT: %s | HUD anchor "
        "mode %d applied total=%llu no-aim total=%llu\n",
        why, w.count, g_hudFrameTotal,
        w.sumHudMinusWorldYaw / n, w.sumHudMinusWorldPitch / n, w.worldValid,
        w.sumAimMinusWrittenYaw / n, w.aimValid,
        w.sumBaseMinusWrittenYaw / n, w.anglesValid,
        kVerdict[verdict < 0 || verdict > 3 ? 3 : verdict],
        g_hudAnchorMode.load(std::memory_order_relaxed),
        g_hudAnchorAppliedCount.load(std::memory_order_relaxed),
        g_hudAnchorNoAimCount.load(std::memory_order_relaxed));
    Tf2VrLog(line);
}

int HudFrameVerdict() {
    const HudFrameWindow& w = g_hudFrame;
    if (w.count == 0 || w.worldValid == 0) return 3;
    const double n = static_cast<double>(w.count);
    const double d = w.sumHudMinusWorldYaw / n;
    const double aim = w.aimValid ? w.sumAimMinusWrittenYaw / n : 1e9;
    const double body = w.anglesValid ? w.sumBaseMinusWrittenYaw / n : 1e9;
    const double eHead = std::fabs(d), eAim = std::fabs(d - aim), eBody = std::fabs(d - body);
    // A verdict needs the winner to be within 3 degrees AND the alternatives
    // to be clearly worse; otherwise say NONE rather than guess.
    double best = eHead; int verdict = 0;
    if (eAim < best) { best = eAim; verdict = 1; }
    if (eBody < best) { best = eBody; verdict = 2; }
    if (best > 3.0) return 3;
    return verdict;
}

void NoteHudPassFrame(const std::uint8_t* bytes) {
    const std::uint64_t now = GetTickCount64();
    if (g_hudFrameWindowStart == 0) g_hudFrameWindowStart = now;
    ++g_hudFrameTotal;
    HudFrameWindow& w = g_hudFrame;
    ++w.count;
    float hudYaw = 0.0f, hudPitch = 0.0f;
    float m[16]{};
    std::memcpy(m, bytes + kCameraRelativeToClipOffset, sizeof(m));
    const bool hudOk = ForwardYawPitchDegrees(m, hudYaw, hudPitch);
    if (hudOk && g_mainSceneMatrixValid.load(std::memory_order_acquire)) {
        float worldYaw = 0.0f, worldPitch = 0.0f;
        if (ForwardYawPitchDegrees(g_mainSceneMatrix, worldYaw, worldPitch)) {
            ++w.worldValid;
            w.sumHudMinusWorldYaw += WrapDegrees(hudYaw - worldYaw);
            w.sumHudMinusWorldPitch += WrapDegrees(hudPitch - worldPitch);
        }
    }
    if (g_frameAnglesValid.load(std::memory_order_acquire)) {
        ++w.anglesValid;
        const float written = g_frameAngles[4];
        w.sumBaseMinusWrittenYaw += WrapDegrees(g_frameAngles[1] - written);
        float aim[3]{};
        if (TryGetAimAnglesDegrees(aim)) {
            ++w.aimValid;
            w.sumAimMinusWrittenYaw += WrapDegrees(aim[1] - written);
        }
    }
    if (g_hudFrameMark.exchange(false, std::memory_order_relaxed)) {
        PrintHudFrameDigest("F5 MARK, window so far", HudFrameVerdict());
    }
    if (now - g_hudFrameWindowStart >= 1000) {
        const int verdict = HudFrameVerdict();
        static std::uint64_t lastPrint = 0;
        const bool periodic = now - lastPrint >= 15000;
        if ((verdict != g_hudFrameLastVerdict || periodic) && g_hudFrameLines < kHudFrameLineCap) {
            lastPrint = now;
            g_hudFrameLastVerdict = verdict;
            ++g_hudFrameLines;
            PrintHudFrameDigest(g_hudFrameLines == 1 ? "first window" : "verdict changed", verdict);
        }
        w = HudFrameWindow{};
        g_hudFrameWindowStart = now;
    }
}
}  // namespace

// ---------------------------------------------------------------------------
// WEAPON PIN DIGEST -- read-only. WHICH passes does the pin rotate?
//
// The pin rotates the camera-to-clip matrix of EVERY near-plane -1 perspective
// upload by the right hand (IsPinnablePass, with no slot chosen). The gun is
// one such pass. The wearer reports a floating black 16:9 rectangle attached to
// the right hand that shows only when the game fades -- which is what a
// screen-filling canvas drawn through a pinned camera would look like, and the
// HUD/RUI substitution was switched off wholesale without changing it
// (STATE-NEWPLAYER-LOAD, eliminated table). This digest says, per second, how
// many pinned uploads had an origin at ~(0,0,0), near the main-scene camera
// (the player's own eye, within kBodyOriginThreshold), or elsewhere, and lists
// the distinct non-zero origins with the viewport in force at each. It prints
// when that composition changes and on F5 (MarkWeaponPinDigest), so the wearer
// can stamp the moment they see the rectangle. It decides nothing.
// ---------------------------------------------------------------------------
namespace {
constexpr int kPinDigestSlots = 6;
struct PinDigestEntry {
    float origin[3];
    unsigned count;
    float vpW, vpH;
    bool nearScene;
};
PinDigestEntry g_pinDigest[kPinDigestSlots]{};
int g_pinDigestCount = 0;
unsigned g_pinDigestOverflow = 0;
unsigned g_pinDigestAtZero = 0;
unsigned g_pinDigestNearScene = 0;
unsigned g_pinDigestElsewhere = 0;
std::uint64_t g_pinDigestWindowStart = 0;
unsigned g_pinDigestLastSignature = 0xFFFFFFFFu;
unsigned g_pinDigestLines = 0;
constexpr unsigned kPinDigestLineCap = 240;
}  // namespace
std::atomic<bool> g_pinDigestMark = false;
unsigned g_pinDigestMarkFrames = 0;

namespace {
void PrintPinDigest(const char* why) {
    char line[900]{};
    int n = std::snprintf(line, sizeof(line),
        "[TF2VR] PIN DIGEST (%s): pinned uploads this window at-zero=%u near-scene=%u elsewhere=%u "
        "(distinct non-zero origins %d, overflow %u) | applied total=%llu skipped-untracked total=%llu "
        "| scene origin (%.0f %.0f %.0f) valid=%d",
        why, g_pinDigestAtZero, g_pinDigestNearScene, g_pinDigestElsewhere, g_pinDigestCount,
        g_pinDigestOverflow, g_weaponPinApplied.load(std::memory_order_relaxed),
        g_weaponPinSkipped.load(std::memory_order_relaxed),
        g_mainSceneOrigin[0], g_mainSceneOrigin[1], g_mainSceneOrigin[2],
        g_mainSceneOriginValid.load(std::memory_order_acquire) ? 1 : 0);
    for (int i = 0; i < g_pinDigestCount && n > 0 && n < static_cast<int>(sizeof(line)) - 8; ++i) {
        const PinDigestEntry& e = g_pinDigest[i];
        n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
            " | %s (%.0f %.0f %.0f) x%u vp %.0fx%.0f",
            e.nearScene ? "near" : "FAR", e.origin[0], e.origin[1], e.origin[2], e.count, e.vpW, e.vpH);
    }
    if (n > 0 && n < static_cast<int>(sizeof(line)) - 2) {
        line[n] = '\n';
        line[n + 1] = 0;
    }
    Tf2VrLog(line);
}

void ResetPinDigest(std::uint64_t now) {
    g_pinDigestCount = 0;
    g_pinDigestOverflow = 0;
    g_pinDigestAtZero = g_pinDigestNearScene = g_pinDigestElsewhere = 0;
    g_pinDigestWindowStart = now;
}

// Called by ApplyWeaponPin after it has rotated a pass. The pass's origin is
// the game's: the pin touches rows 0..2 of the matrix and nothing else.
void NoteWeaponPinDigest(const std::uint8_t* patched) {
    const std::uint64_t now = GetTickCount64();
    if (g_pinDigestWindowStart == 0) g_pinDigestWindowStart = now;
    float origin[3]{};
    std::memcpy(origin, patched + kCameraOriginOffset, sizeof(origin));
    const bool atZero =
        origin[0] * origin[0] + origin[1] * origin[1] + origin[2] * origin[2] <= 1.0f;
    bool nearScene = false;
    if (atZero) {
        ++g_pinDigestAtZero;
    } else {
        nearScene = g_mainSceneOriginValid.load(std::memory_order_acquire) &&
                    OriginsClose(origin, g_mainSceneOrigin, kBodyOriginThreshold);
        if (nearScene) ++g_pinDigestNearScene; else ++g_pinDigestElsewhere;
        int slot = -1;
        for (int i = 0; i < g_pinDigestCount; ++i) {
            if (OriginsClose(g_pinDigest[i].origin, origin, 1.0f)) { slot = i; break; }
        }
        if (slot < 0 && g_pinDigestCount < kPinDigestSlots) {
            slot = g_pinDigestCount++;
            g_pinDigest[slot] = PinDigestEntry{};
            std::memcpy(g_pinDigest[slot].origin, origin, sizeof(origin));
        }
        if (slot >= 0) {
            ++g_pinDigest[slot].count;
            g_pinDigest[slot].vpW = g_lastViewportW.load(std::memory_order_acquire);
            g_pinDigest[slot].vpH = g_lastViewportH.load(std::memory_order_acquire);
            g_pinDigest[slot].nearScene = nearScene;
        } else {
            ++g_pinDigestOverflow;
        }
    }
    if (g_pinDigestMark.exchange(false, std::memory_order_relaxed)) {
        PrintPinDigest("F5 MARK, window so far");
    }
    if (now - g_pinDigestWindowStart >= 1000) {
        const unsigned signature = (g_pinDigestAtZero ? 1u : 0u) | (g_pinDigestNearScene ? 2u : 0u) |
                                   (g_pinDigestElsewhere ? 4u : 0u) |
                                   (static_cast<unsigned>(g_pinDigestCount) << 3);
        if (signature != g_pinDigestLastSignature && g_pinDigestLines < kPinDigestLineCap) {
            g_pinDigestLastSignature = signature;
            ++g_pinDigestLines;
            PrintPinDigest(g_pinDigestLines == 1 ? "first window" : "composition changed");
        }
        ResetPinDigest(now);
    }
}
}  // namespace

void ApplyWeaponPin(std::uint8_t* patched) {
    float now[9]{};
    if (!ReadPublishedBasisLocal(g_controllerAimBasis[kHandRight],
                                 g_controllerGeneration[kHandRight], now)) {
        g_weaponPinSkipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!(g_controllerPoseFlags[kHandRight] & kControllerAimTracked)) {
        g_weaponPinSkipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!g_haveWeaponPinReference.load(std::memory_order_acquire)) {
        for (int i = 0; i < 9; ++i) g_weaponPinReference[i] = now[i];
        g_haveWeaponPinReference.store(true, std::memory_order_release);
        Tf2VrLog("[TF2VR] weapon pin: reference captured from the right controller's current pose. "
                 "Hold it where the gun sits now, then rotate.\n");
        return;
    }
    // Rel = current^T * reference -- the OPPOSITE operand order to the viewmodel
    // compensation, and measured to be right rather than assumed.
    //
    // The first version reused the compensation's order, reference^T * current.
    // Reported from the headset: the pass moved exactly opposite to the hand --
    // left when the hand went right, up when it went down. That is not a bug in
    // the order so much as a difference in purpose. Rotating a camera by R makes
    // its content appear to rotate by R inverse, so the compensation's order is
    // correct for CANCELLING a rotation, which is all it was ever measured for. A
    // pin APPLIES one, and needs the other order.
    //
    // Both bases are rows forward/right/up, the same layout AngleBasis produces,
    // so nothing is converted on the way in.
    float rel[9]{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            rel[i * 3 + j] = now[0 * 3 + i] * g_weaponPinReference[0 * 3 + j] +
                             now[1 * 3 + i] * g_weaponPinReference[1 * 3 + j] +
                             now[2 * 3 + i] * g_weaponPinReference[2 * 3 + j];
        }
    }
    float matrix[16]{};
    std::memcpy(matrix, patched + kCameraRelativeToClipOffset, sizeof(matrix));
    // xyz columns only; column 3 untouched, per the standing rule that writing
    // into m[12..15] corrupts the forward/w row.
    for (int row = 0; row < 4; ++row) {
        const float m0 = matrix[row * 4 + 0];
        const float m1 = matrix[row * 4 + 1];
        const float m2 = matrix[row * 4 + 2];
        matrix[row * 4 + 0] = m0 * rel[0] + m1 * rel[3] + m2 * rel[6];
        matrix[row * 4 + 1] = m0 * rel[1] + m1 * rel[4] + m2 * rel[7];
        matrix[row * 4 + 2] = m0 * rel[2] + m1 * rel[5] + m2 * rel[8];
    }
    std::memcpy(patched + kCameraRelativeToClipOffset, matrix, sizeof(matrix));
    g_weaponPinApplied.fetch_add(1, std::memory_order_relaxed);
    NoteWeaponPinDigest(patched);
}

float NearPlaneElement(const std::uint8_t* bytes) {
    float element11 = 0.0f;
    std::memcpy(&element11, bytes + kCameraRelativeToClipOffset + 11 * sizeof(float),
                sizeof(element11));
    return element11;
}

// THE VIEWMODEL PASS RENDERS AT THE ORIGIN, AND THAT IS WHAT EXCLUDED IT.
//
// IsWorldPerspectiveCamera requires |origin|^2 > 1, which is how the world
// cameras are told apart from the orthographic/UI uploads. The gun's own pass
// fails that test for the most ordinary reason there is: a viewmodel is drawn
// in view space, so its camera origin is exactly (0,0,0) and the squared length
// is 0, not > 1.
//
// The census caught it directly. Every frame of the trace carried exactly one
// upload with nearPlane = -1.0000 -- the viewmodel discriminator, hit exactly --
// at origin (0,0,0), always the last camera-sized upload of the frame, and it
// was rejected by the origin test before the near-plane test was ever reached:
//
//   uncorrected f0 #26: nearPlane=-1.0000 origin=(-0.0 0.0 0.0)
//                       worldPersp=0 viewmodelFamily=0 mainScene=0
//
// So the correction has been rotating three world passes that are not the gun,
// with the pass that IS the gun structurally unreachable. That is consistent
// with every counter looking healthy -- 3 applied a frame, 0 skipped, 0 torn,
// 0 excluded -- while the gun rode the head anyway.
//
// A near-origin perspective camera was clearly anticipated: the predicate for
// it, IsNearOriginPerspectiveCamera, is already written directly below and has
// never been called from anywhere.
//
// Perspective is the property this family actually needs, and m[15] == 0 is
// what tests it (the code's own note: the orthographic/UI uploads carry
// r3 = (0,0,0,1)). The origin magnitude was never part of being a viewmodel.
// Kept behind a config key so the old behaviour is one line away.
bool IsViewmodelCandidateCamera(const std::uint8_t* bytes) {
    if (std::fabs(NearPlaneElement(bytes) + 1.0f) >= 0.01f) return false;
    if (g_viewmodelIncludeNearOrigin.load(std::memory_order_relaxed)) {
        float element15 = 0.0f;
        std::memcpy(&element15, bytes + kMatrixElement15Offset, sizeof(element15));
        return element15 == 0.0f;
    }
    return IsWorldPerspectiveCamera(bytes);
}

// The main scene, by the same near-plane discriminator that separates the
// viewmodel family from it.
bool IsMainSceneCamera(const std::uint8_t* bytes) {
    if (!IsWorldPerspectiveCamera(bytes)) return false;
    return std::fabs(NearPlaneElement(bytes) - kMainSceneNearPlane) < kMainSceneNearTolerance;
}

// The uploads the viewmodel correction should actually rotate.  With the
// exclusion off this is the whole near-plane-1 family, exactly as shipped and
// verified; with it on, the "lower camera" is dropped by origin.
bool IsBodyWeaponPass(const std::uint8_t* bytes) {
    if (!IsViewmodelCandidateCamera(bytes)) return false;
    // THE NEAR-ORIGIN SPLIT RUNS FIRST, AND UNCONDITIONALLY.
    //
    // It used to sit below the g_excludeLowerCamera early-out, which defaults
    // to FALSE and returns true for everything -- so the split was dead code
    // under the shipped config, and a run testing it looked exactly like a run
    // where the split was wrong. It also had no counter of its own, which is
    // what made the two indistinguishable. Both fixed here.
    const bool nearOrigin = IsNearOriginPerspectiveCamera(bytes);
    if (nearOrigin) {
        g_nearOriginSeenCount.fetch_add(1, std::memory_order_relaxed);
        if (g_excludeNearOriginFromCorrection.load(std::memory_order_relaxed)) {
            g_nearOriginExcludedCount.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }
    g_worldOriginSeenCount.fetch_add(1, std::memory_order_relaxed);
    if (g_excludeWorldOriginFromCorrection.load(std::memory_order_relaxed)) {
        g_worldOriginExcludedCount.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!g_excludeLowerCamera.load(std::memory_order_relaxed)) return true;
    // Without a main-scene reference yet there is nothing to compare against.
    // Correct the pass rather than skip it: that is the shipped behaviour, and
    // an unarmed exclusion must never be the thing that breaks the weapon.
    if (!g_mainSceneOriginValid.load(std::memory_order_acquire)) return true;
    float origin[3]{};
    std::memcpy(origin, bytes + kCameraOriginOffset, sizeof(origin));
    if (OriginsClose(origin, g_mainSceneOrigin, kBodyOriginThreshold)) return true;
    g_lowerCameraExcludedCount.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// The screen-space UI family: orthographic (m[15] != 0) with an essentially
// identity basis. The shadow cascades are also orthographic but carry a tiny
// |r0|, so the near-unit test separates them.
bool IsUiCamera(const std::uint8_t* bytes) {
    float element15 = 0.0f;
    std::memcpy(&element15, bytes + kMatrixElement15Offset, sizeof(element15));
    if (element15 == 0.0f) return false;
    float row0[3]{}, row1[3]{};
    std::memcpy(row0, bytes + kCameraRelativeToClipOffset, sizeof(row0));
    std::memcpy(row1, bytes + kCameraRelativeToClipOffset + 4 * sizeof(float), sizeof(row1));
    const float length0 = std::sqrt(row0[0] * row0[0] + row0[1] * row0[1] + row0[2] * row0[2]);
    const float length1 = std::sqrt(row1[0] * row1[0] + row1[1] * row1[1] + row1[2] * row1[2]);
    return length0 > 0.9f && length0 < 1.1f && length1 > 0.9f && length1 < 1.1f;
}

// MEASURED CLOSED, 2026-08-19. KEPT ONLY AS THE RECORD OF A DEAD END.
//
// The theory below is sound and the mechanism does what it says; it simply does
// not apply to this reticle. Scaling the linear part alone was tried in the
// headset and the reticle "shrinks and moves up and left... each shrink goes
// further up and left" -- toward the pixel-space origin -- while the shots kept
// going to the old position.
//
// That is the answer to the open question: the reticle's screen position lives
// in its VERTICES, in pixel coordinates, not in the column-3 term. Its size and
// its position are the same numbers, so NO linear transform of the pass can
// change one without the other. Both the whole-row scale and this linear-only
// scale are therefore closed, and so is the pass-level route generally.
//
// The size has to be changed where the widget is built. The crosshair draw is
// client.dll+0x15EF90 -- identified because it early-outs on the crosshair
// state global at 0x22AC694 that Crosshair_SetState writes -- and the engine's
// own widget resize is 0x54B160(element, scaleX, scaleY). Go there, not here.
//
// ---- the original reasoning, which was right about the mechanism ----
//
// Scales the LINEAR part of the horizontal and vertical projection rows and
// leaves the column-3 term alone.
//
// THIS IS THE DIFFERENCE BETWEEN RESIZING A THING AND MOVING IT, and it is
// what the reticle needs -- WHEN the position is in the matrix. It is not here.
//
//     clip.x = m00*x + m01*y + m02*z + m03
//
// The first three terms are the geometry's own extent; m03 is where its origin
// lands. ScaleProjectionRows below scales all four, so it changes size AND
// position together -- measured in the headset as the reticle shrinking to a
// quarter and, at the same time, stopping tracking what the gun pointed at,
// because a reticle d from the pass origin moved to s*d.
//
// Scaling only 0..2 scales the glyph ABOUT ITS OWN ANCHOR and leaves the anchor
// exactly where the engine put it -- which is the aim point.
//
// It rests on the reticle's screen position living in the column-3 term rather
// than in its vertices. If it turns out to live in the vertices, this shrinks
// the reticle toward the pass origin instead of in place, and that is
// immediately visible rather than subtle -- which is the point of trying it.
void ScaleProjectionLinear(std::uint8_t* patched, float scaleX, float scaleY) {
    float matrix[16]{};
    std::memcpy(matrix, patched + kCameraRelativeToClipOffset, sizeof(matrix));
    for (int i = 0; i < 3; ++i) {
        matrix[0 * 4 + i] *= scaleX;
        matrix[1 * 4 + i] *= scaleY;
    }
    std::memcpy(patched + kCameraRelativeToClipOffset, matrix, sizeof(matrix));
}

// Scales the horizontal and vertical projection rows. Each row is scaled
// whole, all four components: clip.x is then scaled uniformly, which is exactly
// "change the horizontal FOV" and preserves any centring or sub-pixel jitter
// baked into the row rather than discarding it by rebuilding the basis.
//
// Scaling a row commutes with the viewmodel rotation, which multiplies the xyz
// columns: (s*r0)*Rel == s*(r0*Rel). And the eye offset normalises r0 before
// use, so it is unaffected by the scale as well. Order is therefore free; this
// runs first so everything downstream sees the final basis.
void ScaleProjectionRows(std::uint8_t* patched, float scaleX, float scaleY) {
    float matrix[16]{};
    std::memcpy(matrix, patched + kCameraRelativeToClipOffset, sizeof(matrix));
    for (int i = 0; i < 4; ++i) {
        matrix[0 * 4 + i] *= scaleX;
        matrix[1 * 4 + i] *= scaleY;
    }
    std::memcpy(patched + kCameraRelativeToClipOffset, matrix, sizeof(matrix));
}

// True, with the headset's half-tangents, when the main scene should be
// rendered at the headset's frustum instead of the game's own.
bool MatchingHeadsetFov(float& halfTanX, float& halfTanY) {
    if (!g_matchHeadsetFov.load(std::memory_order_acquire)) return false;
    const float x = g_headsetHalfTanX.load(std::memory_order_acquire);
    const float y = g_headsetHalfTanY.load(std::memory_order_acquire);
    if (x <= 0.0001f || y <= 0.0001f) return false;
    halfTanX = x;
    halfTanY = y;
    return true;
}

// The frustum refit's own origin test. Same structure as
// IsNearOriginPerspectiveCamera, with the bound widened past head movement and
// the sample recorded for the histogram. See the note beside g_hudFovOriginMax.
bool IsHudFovOriginNear(const std::uint8_t* bytes) {
    float element15 = 0.0f;
    float origin[3]{};
    std::memcpy(&element15, bytes + kMatrixElement15Offset, sizeof(element15));
    std::memcpy(origin, bytes + kCameraOriginOffset, sizeof(origin));
    if (element15 != 0.0f) return false;
    const float lengthSquared =
        origin[0] * origin[0] + origin[1] * origin[1] + origin[2] * origin[2];
    // RECORDED BEFORE THE TEST, so the histogram describes the population and
    // not the bound's opinion of it.
    const float length = std::sqrt(lengthSquared);
    int band = 5;
    if (length <= 1.0f) band = 0;
    else if (length <= 4.0f) band = 1;
    else if (length <= 16.0f) band = 2;
    else if (length <= 64.0f) band = 3;
    else if (length <= 256.0f) band = 4;
    g_hudFovOriginBand[band].fetch_add(1, std::memory_order_relaxed);
    float prev = g_hudFovOriginMaxSeen.load(std::memory_order_relaxed);
    while (length > prev && !g_hudFovOriginMaxSeen.compare_exchange_weak(prev, length,
                                                                        std::memory_order_relaxed)) {
    }
    const float bound = g_hudFovOriginMax.load(std::memory_order_relaxed);
    return lengthSquared <= bound * bound;
}

bool IsNearOriginPerspectiveCamera(const std::uint8_t* bytes) {
    float element15 = 0.0f;
    float origin[3]{};
    std::memcpy(&element15, bytes + kMatrixElement15Offset, sizeof(element15));
    std::memcpy(origin, bytes + kCameraOriginOffset, sizeof(origin));
    if (element15 != 0.0f) return false;
    const float lengthSquared = origin[0] * origin[0] + origin[1] * origin[1] + origin[2] * origin[2];
    return lengthSquared <= 1.0f;
}

// The screen-space 2D pass: orthographic, at the origin, with row lengths that
// are the reciprocals of a PIXEL extent rather than near unity. The row-length
// ceiling is what separates it from the near-identity family that the removed
// knob caught, and the origin test keeps the shadow cascades out -- those are
// orthographic too, but sit at a world origin thousands of units away.
bool IsScreenSpaceHudPass(const std::uint8_t* bytes) {
    float element15 = 0.0f, origin[3]{}, row0[3]{}, row1[3]{};
    std::memcpy(&element15, bytes + kMatrixElement15Offset, sizeof(element15));
    if (element15 == 0.0f) return false;
    std::memcpy(origin, bytes + kCameraOriginOffset, sizeof(origin));
    if (origin[0] * origin[0] + origin[1] * origin[1] + origin[2] * origin[2] > 1.0f) return false;
    std::memcpy(row0, bytes + kCameraRelativeToClipOffset, sizeof(row0));
    std::memcpy(row1, bytes + kCameraRelativeToClipOffset + 4 * sizeof(float), sizeof(row1));
    const float length0 = std::sqrt(row0[0] * row0[0] + row0[1] * row0[1] + row0[2] * row0[2]);
    const float length1 = std::sqrt(row1[0] * row1[0] + row1[1] * row1[1] + row1[2] * row1[2]);
    return length0 > 0.0f && length0 < 0.1f && length1 > 0.0f && length1 < 0.1f;
}

// One entry per distinct constant-buffer WIDTH. Small, fixed and add-only, so
// it stops growing as soon as the frame's shapes have all been seen.
struct BufferWidth { bool used; unsigned width; std::uint64_t count; };
constexpr int kBufferWidthMax = 24;
BufferWidth g_bufferWidths[kBufferWidthMax]{};

// THE 64-BYTE BUFFER: 16 floats, i.e. ONE 4x4 MATRIX.
//
// The width census turned up exactly two shapes -- 576 (the camera) and 64 --
// and this plugin has only ever looked at the first. A lone 4x4 is what a 2D UI
// transform looks like, and the non-reticle HUD is in none of the camera
// families, so this is where it has been hiding.
//
// READ-ONLY, and deliberately so: the last knob built on a guess about which
// family was the HUD scaled full-screen effect passes and damaged the image.
// Look at the matrices before touching them.
struct Matrix64 { bool used; float m[16]; std::uint64_t count; };
constexpr int kMatrix64Max = 12;
Matrix64 g_matrix64[kMatrix64Max]{};

void TallyMatrix64(const std::uint8_t* bytes) {
    float m[16]{};
    std::memcpy(m, bytes, sizeof(m));
    for (auto& entry : g_matrix64) {
        if (entry.used) {
            bool same = true;
            for (int i = 0; i < 16; ++i) {
                if (std::fabs(entry.m[i] - m[i]) > 0.001f) { same = false; break; }
            }
            if (same) { ++entry.count; return; }
            continue;
        }
        entry.used = true;
        entry.count = 1;
        std::memcpy(entry.m, m, sizeof(m));
        return;
    }
}

void TallyConstantBufferWidth(unsigned width) {
    for (auto& entry : g_bufferWidths) {
        if (entry.used && entry.width == width) { ++entry.count; return; }
        if (!entry.used) { entry.used = true; entry.width = width; entry.count = 1; return; }
    }
}

// One entry per distinct upload signature. Linear over at most 24 entries and
// only while the census is armed; the table stops growing once the frame's
// families have all been seen, so this is a handful of compares a call.
void TallyUploadFamily(const std::uint8_t* bytes) {
    float element15 = 0.0f, origin[3]{}, row0[3]{}, row1[3]{};
    std::memcpy(&element15, bytes + kMatrixElement15Offset, sizeof(element15));
    std::memcpy(origin, bytes + kCameraOriginOffset, sizeof(origin));
    std::memcpy(row0, bytes + kCameraRelativeToClipOffset, sizeof(row0));
    std::memcpy(row1, bytes + kCameraRelativeToClipOffset + 4 * sizeof(float), sizeof(row1));
    const float nearPlane = NearPlaneElement(bytes);
    const float length0 = std::sqrt(row0[0] * row0[0] + row0[1] * row0[1] + row0[2] * row0[2]);
    const float length1 = std::sqrt(row1[0] * row1[0] + row1[1] * row1[1] + row1[2] * row1[2]);
    for (auto& entry : g_uploadFamilies) {
        if (!entry.used) continue;
        if (std::fabs(entry.nearPlane - nearPlane) > 0.01f) continue;
        if ((entry.element15 == 0.0f) != (element15 == 0.0f)) continue;
        if (std::fabs(entry.rowLength0 - length0) > 0.02f) continue;
        if (std::fabs(entry.rowLength1 - length1) > 0.02f) continue;
        if (!OriginsClose(entry.origin, origin, 64.0f)) continue;
        ++entry.count;
        return;
    }
    for (auto& entry : g_uploadFamilies) {
        if (entry.used) continue;
        entry.used = true;
        entry.count = 1;
        entry.nearPlane = nearPlane;
        entry.element15 = element15;
        entry.rowLength0 = length0;
        entry.rowLength1 = length1;
        std::memcpy(entry.origin, origin, sizeof(entry.origin));
        return;
    }
    // The table is full. Counted, not swallowed -- see the note at
    // kUploadFamilyMax.
    ++g_uploadFamilyDropped;
}

// W2 -- how much to shear THIS upload, and whether to shear it at all.
//
// THE ONLY TEST IS STRUCTURAL. m[15] == 0 is what makes a projection
// perspective, which is the same test IsWorldPerspectiveCamera opens with, and
// it is the whole predicate here: a perspective matrix has a forward row to
// shear along and an orthographic one does not. No family is recognised, no
// near plane is matched, nothing has to be kept exhaustive.
//
// The offset comes from the MAIN SCENE's half-tangent even when this upload
// belongs to another family, and that is deliberate rather than sloppy. The
// same NDC offset added to every perspective pass translates the whole image
// rigidly: the world moves, the effects over it move with it, the gun moves
// with both, and nothing inside the frame shifts relative to anything else.
// Deriving a per-family offset from each family's own frustum would give each
// one a different NDC shift, which is the misregistration this would be trying
// to avoid.
//
// It also tracks ADS for free: as the main scene narrows, halfTanY falls and
// the NDC offset rises by the same factor, so the ANGLE the frustum is aimed at
// stays on the lens axis at every zoom.
bool ComputeLensShearNdcY(const std::uint8_t* bytes, float& ndcY) {
    ndcY = 0.0f;
    if (!g_lensShearEnabled.load(std::memory_order_acquire)) {
        g_lensShearDecline[kShearDeclineDisabled].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    float element15 = 0.0f;
    std::memcpy(&element15, bytes + kMatrixElement15Offset, sizeof(element15));
    if (element15 != 0.0f) {               // orthographic: no forward row to shear along
        g_lensShearDecline[kShearDeclineOrthographic].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const float up = g_lensTanUp.load(std::memory_order_acquire);
    const float down = g_lensTanDown.load(std::memory_order_acquire);
    if (!(up > down)) {                    // the runtime has not named its optics yet
        g_lensShearDecline[kShearDeclineNoLens].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // THE GAME'S OWN VALUE, not GetMainSceneHalfTangents(): that one multiplies
    // by the ADS DECLARED magnification, and this has to divide by the tangent
    // the matrix in front of us was actually built with. Mixing the declared and
    // rendered frustums here is the bug the accessor's own comment records.
    const float halfTanY = g_mainSceneHalfTanY.load(std::memory_order_acquire);
    if (halfTanY <= 0.0001f) {              // no main-scene frustum measured yet
        g_lensShearDecline[kShearDeclineNoFrustum].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ndcY = tf2vr::ShearForCentre(halfTanY, 0.5f * (up + down));
    if (ndcY == 0.0f) g_lensShearDecline[kShearDeclineCentred].fetch_add(1, std::memory_order_relaxed);
    // EXACTLY ZERO ON A CENTRED HEADSET, and then there is nothing to do. This
    // is V2's falsifier at the render: a symmetric lens cannot reach the memcpy
    // below, so the PFD MR's projection matrix is byte-identical to what every
    // previous build handed the engine.
    return ndcY != 0.0f;
}

int LensShearFamilyBucket(const std::uint8_t* bytes) {
    // NOT called `near`: windef.h defines that as an empty macro, and the
    // resulting error names fabs rather than the identifier.
    const float nearPlane = NearPlaneElement(bytes);
    if (std::fabs(nearPlane + 7.0f) < 0.5f) return kShearFamilyMainScene;
    if (std::fabs(nearPlane + 1.0f) < 0.5f) return kShearFamilyViewmodel;
    if (std::fabs(nearPlane) < 0.5f) return kShearFamilyEffects;
    return kShearFamilyOther;
}

bool IsWorldPerspectiveCamera(const std::uint8_t* bytes) {
    float element15 = 0.0f;
    float origin[3]{};
    std::memcpy(&element15, bytes + kMatrixElement15Offset, sizeof(element15));
    std::memcpy(origin, bytes + kCameraOriginOffset, sizeof(origin));
    if (element15 != 0.0f) return false;
    const float lengthSquared = origin[0] * origin[0] + origin[1] * origin[1] + origin[2] * origin[2];
    return lengthSquared > 1.0f;
}

void STDMETHODCALLTYPE HookRSSetViewports(ID3D11DeviceContext* context, UINT count,
                                          const D3D11_VIEWPORT* viewports) {
    // The audit flagged this seam because BeginViewportTrace SELF-ARMS for 120
    // frames on hook install and then walks a table on every call, and the game
    // sets a viewport many times a frame. Measure it rather than argue about it.
    PluginCost::Scope vpScope(PluginCost::kViewport);
    // Remember the last rectangle set on the game's own context, always, not
    // only while a trace is running. The camera upload that follows it is what
    // gives it meaning; see the capture beside g_mainSceneHalfTan.
    if (context == g_gameContext && count && viewports) {
        g_lastViewportX.store(viewports[0].TopLeftX, std::memory_order_release);
        g_lastViewportY.store(viewports[0].TopLeftY, std::memory_order_release);
        g_lastViewportW.store(viewports[0].Width, std::memory_order_release);
        g_lastViewportH.store(viewports[0].Height, std::memory_order_release);
        // ------------------------------------------------------------------
        // SAME-FRAME STEREO: THE MID-FRAME BOUNDARY.
        //
        // This is the piece the alternate-frame path cannot lend us. AER buys
        // separation with TIME -- one whole frame per eye, captured at Present,
        // its boundary being the frame boundary. Same-frame stereo has two eyes
        // inside one Present, so it has to buy separation with SPACE, at a
        // moment that exists only here.
        //
        // The scene draw SUBMITS; both batches execute later on this thread, in
        // order, into the same target. So the boundary is not between the two
        // calls (nothing has been drawn then) -- it is between the two batches'
        // D3D work, and this hook is already standing on that thread with the
        // game's own context.
        //
        // Identified by counting: a scene pass sets a viewport the size of the
        // render target, once per pass. One per frame normally; TWO when the
        // double is armed. The second one is batch 2 about to draw, which makes
        // that instant the only moment eye 0 exists alone.
        //
        // Capturing the BACKBUFFER here would get nothing -- the engine renders
        // the scene into offscreen targets and composites once, at the end of
        // the frame. So we ask the context what is actually bound and copy that.
        if (SameFrameDoubleArmed()) {
            const int rw = RequestedRenderWidth();
            const bool sceneSized = rw > 0 &&
                (int)(viewports[0].Width + 0.5f) == rw &&
                (int)(viewports[0].Height + 0.5f) == RequestedRenderHeight();
            if (sceneSized && ++g_sameFrameScenePasses == 2) {
                Microsoft::WRL::ComPtr<ID3D11RenderTargetView> boundRtv;
                context->OMGetRenderTargets(1, &boundRtv, nullptr);
                if (boundRtv) {
                    Microsoft::WRL::ComPtr<ID3D11Resource> res;
                    boundRtv->GetResource(&res);
                    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
                    if (res && SUCCEEDED(res.As(&tex)) && tex) {
                        CaptureSameFrameEye0(context, tex.Get());
                    }
                }
            }
        }
        // ------------------------------------------------------------------
        // THE WORLD PASS, IDENTIFIED WITHOUT A CAMERA UPLOAD.
        //
        // GetMainSceneViewport only latches when a main-scene camera upload is
        // recognised, and that never happens in a flat session -- which is why
        // the A1 sweep could not start. But the world pass does not need the
        // upload to be identified: it is the rectangle the game sets FAR more
        // often than any other. Measured in the archive, at 4032x3648: the
        // world rect 2281 sets against 359 for the next one, and 120 each for
        // the shadow cascades. It is not close, and it is not a heuristic that
        // has to be exhaustive to be right -- the winner is the winner.
        //
        // Eight slots, a linear scan, on a path PLUGIN COST already measures at
        // 0.000 ms/frame over 62 calls. Non-atomic: this is a tally read at
        // step boundaries, and a lost increment cannot change which rectangle
        // is dominant by three orders of magnitude.
        const float vw = viewports[0].Width;
        const float vh = viewports[0].Height;
        ++g_viewportTallyTotal;
        bool found = false;
        for (size_t i = 0; i < g_viewportTallyCount; ++i) {
            if (g_viewportTally[i].width == vw && g_viewportTally[i].height == vh) {
                ++g_viewportTally[i].count;
                found = true;
                break;
            }
        }
        if (!found && g_viewportTallyCount < kViewportTallySlots) {
            g_viewportTally[g_viewportTallyCount].width = vw;
            g_viewportTally[g_viewportTallyCount].height = vh;
            g_viewportTally[g_viewportTallyCount].count = 1;
            ++g_viewportTallyCount;
        } else if (!found) {
            ++g_viewportTallyOverflow;
        }
    }
    if (g_viewportTraceFrames.load(std::memory_order_relaxed) && context == g_gameContext &&
        count && viewports) {
        // Distinct rectangles only, with a tally. The game sets a viewport many
        // times a frame and the interesting thing is the SET of shapes, not the
        // sequence.
        for (UINT i = 0; i < count; ++i) {
            const D3D11_VIEWPORT& v = viewports[i];
            // SPLIT BY WHETHER THE HUD WIDGET IS DRAWING. The distinct-rectangle
            // tally alone cannot answer this rung's question: a viewport that is
            // constant for the world and moving for the UI shows up in that
            // tally as "a few rectangles", which is what it would look like if
            // nothing were wrong. Tracking the two populations separately is
            // what makes the difference visible.
            ViewportRange& r = g_ruiInDraw ? g_vpInHud : g_vpElsewhere;
            if (!r.seen) {
                r.seen = true;
                r.loX = r.hiX = v.TopLeftX; r.loY = r.hiY = v.TopLeftY;
                r.loW = r.hiW = v.Width;    r.loH = r.hiH = v.Height;
            } else {
                if (v.TopLeftX < r.loX) r.loX = v.TopLeftX;
                if (v.TopLeftX > r.hiX) r.hiX = v.TopLeftX;
                if (v.TopLeftY < r.loY) r.loY = v.TopLeftY;
                if (v.TopLeftY > r.hiY) r.hiY = v.TopLeftY;
                if (v.Width  < r.loW) r.loW = v.Width;
                if (v.Width  > r.hiW) r.hiW = v.Width;
                if (v.Height < r.loH) r.loH = v.Height;
                if (v.Height > r.hiH) r.hiH = v.Height;
            }
            ++r.count;

            bool found = false;
            for (size_t s = 0; s < g_seenViewportCount; ++s) {
                SeenViewport& seen = g_seenViewports[s];
                if (seen.topLeftX == v.TopLeftX && seen.topLeftY == v.TopLeftY &&
                    seen.width == v.Width && seen.height == v.Height) {
                    ++seen.count;
                    found = true;
                    break;
                }
            }
            if (!found && g_seenViewportCount < kMaxSeenViewports) {
                g_seenViewports[g_seenViewportCount++] = {v.TopLeftX, v.TopLeftY, v.Width, v.Height, 1};
            }
        }
    }
    // ---- xr.viewport_full: WIDEN THE WORLD PASS TO THE WHOLE TARGET ----
    //
    // Measured at a 4032x3648 backbuffer: the game renders the world into
    // 4032x2520 at y=0, aspect 1.600, and blacks out the remaining 1128 rows.
    // That clamp is this call, and it is the only thing standing between us and
    // the resolution the runtime asks for -- it wants 4032x3648 per eye and we
    // hand it 2520 rows of content.
    //
    // The rule is narrow on purpose: only a viewport that spans the full target
    // WIDTH but falls short of its HEIGHT is the world pass being clamped.
    // Every other rectangle in the trace -- the 2048x2048 cubemap faces, the
    // 1008x630 quarter-scale pass, the 1x1 and 2x2 probes -- fails it and is
    // forwarded untouched.
    //
    // Widening alone would stretch the image, because the game's projection was
    // built for 1.600. The camera hook corrects the vertical term to match; see
    // kViewportFullAspectFix beside the main-scene upload. Neither half is
    // correct without the other, so both are gated on this one flag.
    if (g_viewportFull.load(std::memory_order_acquire) && context == g_gameContext &&
        count == 1 && viewports) {
        const float targetW = g_backbufferWidth.load(std::memory_order_acquire);
        const float targetH = g_backbufferHeight.load(std::memory_order_acquire);
        const D3D11_VIEWPORT& v = viewports[0];
        if (targetW > 1.0f && targetH > 1.0f && v.TopLeftX == 0.0f &&
            v.Width == targetW && v.Height < targetH - 0.5f) {
            D3D11_VIEWPORT widened = v;
            widened.TopLeftY = 0.0f;
            widened.Height = targetH;
            if (!g_viewportFullReported) {
                g_viewportFullReported = true;
                char line[300]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] viewport WIDENED: the world pass asked for %.0fx%.0f at y=%.0f "
                    "(aspect %.3f) and is being given the whole %.0fx%.0f target (aspect %.3f). The "
                    "camera's vertical term is corrected to match; without that this would stretch.\n",
                    v.Width, v.Height, v.TopLeftY, v.Height > 0.5f ? v.Width / v.Height : 0.0f,
                    targetW, targetH, targetH > 0.5f ? targetW / targetH : 0.0f);
                Tf2VrLog(line);
            }
            // Record the WIDENED rectangle, not the requested one. Recording the
            // request made the frustum line report "3600x2250" while the game
            // was actually being given 3600x2520, so the one instrument that
            // could confirm the widening had taken was reporting the input to it.
            g_lastViewportX.store(widened.TopLeftX, std::memory_order_release);
            g_lastViewportY.store(widened.TopLeftY, std::memory_order_release);
            g_lastViewportW.store(widened.Width, std::memory_order_release);
            g_lastViewportH.store(widened.Height, std::memory_order_release);
            vpScope.PauseForForward();
            g_viewportOriginal(context, count, &widened);
            return;
        }
    }
    vpScope.PauseForForward();
    g_viewportOriginal(context, count, viewports);
}


// ---- RUNG B: THE MAP/UNMAP CENSUS ----------------------------------------
//
// PLAN-CURRENT's rung B, and the plan's own note is the reason it exists:
// every census this project has ever run reads through detours that are all
// UpdateSubresource, so a D3D11_USAGE_DYNAMIC buffer updated with
// Map(WRITE_DISCARD) -- the normal path for per-frame UI data -- has been
// invisible to every instrument here.
//
// WHY IT IS NOW THE ONLY PLACE LEFT. Three things are measured:
//
//   * the HUD's orthographic projection is CONSTANT -- one matrix across 34
//     census prints, while the wearer watched both halves wobble;
//   * the widget's own parameter block is CONSTANT -- 72 fields, ten windows,
//     sixty samples each, including the binding-derived region at blk+0xA0..F0;
//   * the drawn pixels MOVE inside the game's own backbuffer during a turn.
//
// Constant transform and constant inputs producing moving output means the
// motion is applied between the widget and the raster. The widget's last act
// is a call into slot +0x30, which is a BYTECODE INTERPRETER
// (`movzx edx,byte [rax]; inc rax; call [rbp+rdx*8]`, dispatch table at
// engine+0x5F42A0). The block holds static parameters; the per-frame work is
// bytecode, and whatever it computes has to reach the GPU as VERTICES.
//
// AND THE TWO SHORTCUTS ARE CLOSED, on the wearer's own observation: the HUD
// does not clip into walls, so it is not a world-space quad; and the wobble
// does NOT scale with turn speed, which kills the view-lag model I had been
// working from. A fixed-amplitude oscillation that appears only while turning
// is not inertia.
//
// READ-ONLY, AND NO GPU COMMANDS INSIDE THE HOOK -- that last part is a closed
// finding in this project and is not being retested. This records what was
// mapped, how big it was, and whether its CONTENTS CHANGE. Nothing is written.
//
// COST IS BOUNDED BY CONSTRUCTION. Map/Unmap runs thousands of times a second,
// so the hook does one pointer compare and a table walk of at most 32 entries;
// the checksum reads 256 bytes and only on every 16th map of a given resource.
// The table cannot grow.

namespace {

// Raised by the substituted RUI draw slot in rui_probe.cpp for the span of one
// lower-left widget's draw. This is the whole discriminator for rung B.

using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                          D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);

// Same vtable this file already indexes for UpdateSubresource (48), DrawIndexed
// (12) and RSSetViewports (44).
constexpr size_t kMapVtableIndex = 14;
constexpr size_t kUnmapVtableIndex = 15;

MapFn g_mapOriginal = nullptr;
UnmapFn g_unmapOriginal = nullptr;

constexpr int kMaxMapped = 32;
struct MappedResource {
    ID3D11Resource* resource;
    void* volatile lastData;
    UINT byteWidth;
    std::uint64_t maps;
    std::uint32_t firstSum;
    std::uint32_t lastSum;
    std::uint32_t distinctSums;
    bool sumSeen;
    // The first sixteen floats, tracked as a range. A 4x4 matrix is sixteen
    // floats, and the question is which LANES move -- a constant projection
    // shows a range of zero everywhere, a rotation driven by head pose does
    // not. "Distinct contents" cannot answer that: every dynamic buffer has
    // thousands of distinct contents and it means nothing.
    float lo[16];
    float hi[16];
    bool floatsSeen;
};
MappedResource g_mapped[kMaxMapped]{};
volatile long g_mappedCount = 0;
volatile long long g_mapCalls = 0;
volatile long long g_mapDropped = 0;
volatile long long g_mapInDraw = 0;
volatile long long g_mapFaults = 0;
bool g_mapCensusWanted = false;
std::atomic_bool g_mapCensusOn = false;

// Cheap and good enough to say "did these bytes change", which is the only
// question being asked. Not a hash anyone should rely on for identity.
std::uint32_t CheapSum(const void* data, size_t bytes) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint32_t h = 2166136261u;
    for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

MappedResource* FindMapped(ID3D11Resource* resource) {
    long taken = g_mappedCount;
    if (taken > kMaxMapped) taken = kMaxMapped;
    for (long i = 0; i < taken; ++i) {
        if (g_mapped[i].resource == resource) return &g_mapped[i];
    }
    return nullptr;
}

HRESULT STDMETHODCALLTYPE HookMap(ID3D11DeviceContext* context, ID3D11Resource* resource,
                                  UINT subresource, D3D11_MAP mapType, UINT flags,
                                  D3D11_MAPPED_SUBRESOURCE* mapped) {
    const HRESULT hr = g_mapOriginal(context, resource, subresource, mapType, flags, mapped);
    if (FAILED(hr) || !g_mapCensusOn.load(std::memory_order_relaxed) || context != g_gameContext ||
        !resource || !mapped || !mapped->pData) {
        return hr;
    }
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_mapCalls));

    // CONSTANT-BUFFER SIZED ONLY. The widget filter that used to be here is
    // gone, and the reason is the wearer's own observation: the RETICLE rotates
    // with head movement too. The reticle does not go through the RUI widget
    // path at all -- this project established it rides the 576-byte camera pass,
    // which is why hud2d.zoom scales it and the HUD inset does not. If it shows
    // the same symptom, the widget's vertices cannot be the cause and a filter
    // scoped to that widget is looking in the wrong place.
    //
    // It also resolves the contradiction this rung kept producing. "The HUD
    // projection is constant" came from the UPLOAD CENSUS, which only ever sees
    // UpdateSubresource. A constant buffer written through Map/Unmap has been
    // invisible to every instrument in this project -- which is rung B's whole
    // premise, and I pointed it at vertex data instead of at transforms.
    //
    // So: track buffers small enough to BE a transform, and record which float
    // lanes move. Vertex buffers are thousands of bytes and are excluded by
    // shape before any value test runs.
    // The size test lives in the entry-creation block below, where the
    // descriptor is being read anyway -- testing it here would cost a GetDesc
    // on every one of two million calls to learn something that only matters
    // once per resource.

    MappedResource* entry = FindMapped(resource);
    if (!entry) {
        // BUFFERS ONLY, and only ones big enough to hold geometry. Textures map
        // constantly and would fill the table with things that cannot be the
        // HUD's vertices.
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        resource->GetType(&dim);
        if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return hr;
        D3D11_BUFFER_DESC desc{};
        static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
        // A transform is at most a few matrices. Vertex buffers here run to
        // megabytes and would fill the table before any constant buffer was
        // seen -- which is exactly what happened when the filter was "changes
        // or not": 32 slots gone and 297,146 dropped.
        if (desc.ByteWidth > 256 || desc.ByteWidth < 16) return hr;
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_mapInDraw));
        const long slot = InterlockedIncrement(&g_mappedCount) - 1;
        if (slot >= kMaxMapped) {
            InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_mapDropped));
            return hr;
        }
        entry = &g_mapped[slot];
        entry->byteWidth = desc.ByteWidth;
        entry->lastData = mapped->pData;
        // The resource pointer is written LAST: it is what publishes the slot,
        // for the same reason the identity census does it that way.
        _ReadWriteBarrier();
        entry->resource = resource;
    }
    entry->lastData = mapped->pData;
    return hr;
}

void STDMETHODCALLTYPE HookUnmap(ID3D11DeviceContext* context, ID3D11Resource* resource,
                                 UINT subresource) {
    if (g_mapCensusOn.load(std::memory_order_relaxed) && context == g_gameContext && resource) {
        MappedResource* entry = FindMapped(resource);
        // CLAIM THE POINTER AND CLEAR IT IN ONE STEP. This crashed the game,
        // and the crash was created by the in-draw filter rather than being
        // latent:
        //
        //   1. buffer mapped INSIDE the draw -> entry records lastData = P
        //   2. unmapped -> P is checksummed while still mapped, correctly
        //   3. same buffer mapped OUTSIDE the draw -> HookMap now returns
        //      early, so lastData is left holding P
        //   4. unmapped -> P is read again, and P died at step 2
        //
        // WRITE_DISCARD hands back a fresh pointer on every Map, so the old
        // one is invalid the moment it is unmapped. The untargeted version
        // could not hit this because it recorded lastData on EVERY map, so
        // every Unmap had a live pointer; adding the filter broke that
        // invariant without changing the code that relied on it.
        //
        // An interlocked exchange makes each checksum read a pointer from the
        // immediately preceding RECORDED map, once, and never again -- and it
        // closes the two-thread version of the same race rather than assuming
        // this context is only ever touched by one.
        void* data = entry ? InterlockedExchangePointer(&entry->lastData, nullptr) : nullptr;
        if (entry && data) {
            const std::uint64_t n =
                static_cast<std::uint64_t>(InterlockedIncrement64(
                    reinterpret_cast<volatile LONG64*>(&entry->maps)));
            // READ BEFORE UNMAP, which is the whole point of this seam: after
            // Unmap the pointer is not ours to touch. Every sixteenth map, so
            // the cost is a sixteenth of what it looks like.
            if ((n % 16) == 0) {
                const size_t bytes = entry->byteWidth < 256 ? entry->byteWidth : 256;
                // GUARDED, BECAUSE THIS READS MEMORY THE DRIVER OWNS and a
                // diagnostic has no business taking the game down -- which it
                // just did. The fault is COUNTED and reported, not swallowed:
                // a guard that hides a bad read would quietly produce wrong
                // checksums, which is worse than the crash it prevents.
                std::uint32_t sum = 0;
                float snapshot[16]{};
                const int lanes = static_cast<int>(bytes / sizeof(float)) < 16
                                      ? static_cast<int>(bytes / sizeof(float))
                                      : 16;
                bool ok = true;
                __try {
                    sum = CheapSum(data, bytes);
                    // Copied out INSIDE the guard, so the range tracking below
                    // never touches driver memory itself.
                    for (int i = 0; i < lanes; ++i) {
                        snapshot[i] = static_cast<const float*>(data)[i];
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    ok = false;
                    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_mapFaults));
                }
                if (ok) {
                    for (int i = 0; i < lanes; ++i) {
                        const float v = snapshot[i];
                        if (!entry->floatsSeen) { entry->lo[i] = v; entry->hi[i] = v; }
                        else {
                            if (v < entry->lo[i]) entry->lo[i] = v;
                            if (v > entry->hi[i]) entry->hi[i] = v;
                        }
                    }
                    entry->floatsSeen = true;
                }
                if (!ok) {
                    g_unmapOriginal(context, resource, subresource);
                    return;
                }
                if (!entry->sumSeen) {
                    entry->sumSeen = true;
                    entry->firstSum = sum;
                    entry->lastSum = sum;
                    entry->distinctSums = 1;
                } else if (sum != entry->lastSum) {
                    entry->lastSum = sum;
                    ++entry->distinctSums;
                }
            }
        }
    }
    g_unmapOriginal(context, resource, subresource);
}

}  // namespace

bool MapCensusWanted() { return g_mapCensusWanted; }

void SetMapCensusWanted(bool wanted) { g_mapCensusWanted = wanted; }

void SetMapCensusEnabled(bool enabled) {
    g_mapCensusOn.store(enabled, std::memory_order_release);
    Tf2VrLog(enabled ? "[TF2VR] Map/Unmap census ON (read-only; rung B).\n"
                     : "[TF2VR] Map/Unmap census off.\n");
}

void ReportMapCensus() {
    // SAY NOTHING WHEN THERE IS NOTHING. This census is a development rung and
    // ships disarmed, and its empty report printed 28 times in a 159-line smoke
    // test -- a third of the log, every line of it announcing that the hook is
    // not installed and nothing below means anything.
    //
    // It leaked into quiet mode for an almost funny reason: KeepWhenQuiet()
    // allowlists the substring "not installed", and the empty branch of this
    // report contains the words "the hook is not installed". It was kept BECAUSE
    // it said it had nothing to say.
    //
    // Disarmed and never fired is silence. Armed and never fired still prints:
    // that one is a real finding, and the arm is what distinguishes them.
    if (!g_mapCensusOn.load(std::memory_order_acquire) && g_mapCalls == 0) return;
    long taken = g_mappedCount;
    if (taken > kMaxMapped) taken = kMaxMapped;
    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] ======== RUNG B: CONSTANT BUFFERS WRITTEN VIA Map/Unmap ========\n"
        "[TF2VR]   %lld Map calls on the game context; %lld were constant-buffer sized "
        "(16..256 bytes); %ld tracked, %lld dropped, %lld guarded read faults.%s\n",
        g_mapCalls, g_mapInDraw, taken, g_mapDropped, g_mapFaults,
        g_mapCalls == 0
            ? "  ZERO -- the hook is not installed, and nothing below means anything."
            : (g_mapFaults > 0
                   ? "  FAULTS ARE NON-ZERO, so at least one read was refused and the ranges "
                     "below are incomplete."
                   : ""));
    Tf2VrLog(line);
    for (long i = 0; i < taken; ++i) {
        const MappedResource& m = g_mapped[i];
        if (!m.resource) continue;
        std::snprintf(line, sizeof(line),
            "[TF2VR]   buffer %p  %4u bytes  maps %8llu  sampled %llu\n",
            static_cast<void*>(m.resource), m.byteWidth,
            static_cast<unsigned long long>(m.maps),
            static_cast<unsigned long long>(m.maps / 16));
        Tf2VrLog(line);
        if (!m.floatsSeen) continue;
        // WHICH LANES MOVE, not whether the bytes differ. Sixteen floats is a
        // 4x4 matrix; a constant projection ranges zero across all of them and
        // anything driven by head pose does not. Only the moving lanes print,
        // with a static count, because thirty-two buffers times sixteen lanes
        // is noise and noise is how a signal gets missed.
        int moved = 0;
        for (int i = 0; i < 16; ++i) {
            if ((m.hi[i] - m.lo[i]) > 0.0001f) ++moved;
        }
        if (moved == 0) {
            Tf2VrLog("[TF2VR]       all 16 lanes CONSTANT\n");
            continue;
        }
        std::snprintf(line, sizeof(line), "[TF2VR]       %d of 16 lanes move:\n", moved);
        Tf2VrLog(line);
        for (int i = 0; i < 16; ++i) {
            const float range = m.hi[i] - m.lo[i];
            if (range <= 0.0001f) continue;
            std::snprintf(line, sizeof(line),
                "[TF2VR]         [%2d] %12.4f .. %12.4f   range %12.4f\n", i,
                static_cast<double>(m.lo[i]), static_cast<double>(m.hi[i]),
                static_cast<double>(range));
            Tf2VrLog(line);
        }
    }
    Tf2VrLog("[TF2VR] ==========================================\n");
}

// Installs by SWAPPING THE VTABLE SLOT, not by detouring the entry.
//
// The first attempt used the entry detour this file uses everywhere else and
// was refused, correctly and loudly:
//
//   rung B: Map entry 00007FFC91462FE0 starts 48 83 EC 48 4C -- not a prologue
//
// `48 83 EC 48` is `sub rsp, 0x48` -- an ordinary prologue, just not one of the
// three exact byte patterns this file's table knows. Adding it as a fourth is
// not possible either: a variant has to displace at least the fourteen bytes an
// absolute jump needs, and that one instruction is four.
//
// The slot swap sidesteps the whole question. This context's dispatch table is
// HEAP-ALLOCATED AND PER-OBJECT -- this file says so where it hooks
// UpdateSubresource -- so the pointer can simply be replaced. No prologue to
// recognise, no trampoline to build, no bytes displaced, and it is the
// mechanism PLAN-CURRENT names as preferred: a data write rather than a code
// patch.
//
// The originals are read from the slots and called directly, so this composes
// with anything else that has already detoured the implementations behind them.
bool InstallMapCensus(void** vtable) {
    if (g_mapOriginal) return true;
    if (!vtable) return false;
    void** mapSlot = &vtable[kMapVtableIndex];
    void** unmapSlot = &vtable[kUnmapVtableIndex];

    // REFUSE IF A SLOT ALREADY HOLDS OURS. Re-reading our own hook as the
    // "original" builds a call that recurses into itself -- the exact failure
    // this file records having had once already, with a trampoline that jumped
    // back into its own interceptor.
    if (*mapSlot == reinterpret_cast<void*>(&HookMap) ||
        *unmapSlot == reinterpret_cast<void*>(&HookUnmap)) {
        return true;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(mapSlot, sizeof(void*) * 2, PAGE_READWRITE, &oldProtect)) {
        Tf2VrLog("[TF2VR] rung B: the context vtable would not go writable; nothing hooked.\n");
        return false;
    }
    g_mapOriginal = reinterpret_cast<MapFn>(*mapSlot);
    g_unmapOriginal = reinterpret_cast<UnmapFn>(*unmapSlot);
    *mapSlot = reinterpret_cast<void*>(&HookMap);
    *unmapSlot = reinterpret_cast<void*>(&HookUnmap);
    DWORD ignored = 0;
    VirtualProtect(mapSlot, sizeof(void*) * 2, oldProtect, &ignored);

    char line[240]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] rung B: Map and Unmap hooked by vtable slot swap (originals %p / %p). "
        "Read-only.\n",
        reinterpret_cast<void*>(g_mapOriginal), reinterpret_cast<void*>(g_unmapOriginal));
    Tf2VrLog(line);
    return true;
}
void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* context, UINT indexCount, UINT startIndex, INT baseVertex) {
    // THE SEAM THIS REVIEW TURNS ON. Two acq_rel read-modify-writes bracket
    // every draw the game makes, with no foreign-caller fast path, and nothing
    // in this repo has ever counted how many draws that is. The Scope reports
    // the rate as well as the time, and the forward is excluded so this is our
    // cost rather than the driver's.
    PluginCost::Scope costScope(PluginCost::kDrawIndexed);
    g_inFlight.fetch_add(1, std::memory_order_acq_rel);
    if (g_enabled.load(std::memory_order_relaxed) && context == g_gameContext)
        g_drawIndexedCount.fetch_add(1, std::memory_order_relaxed);
    if (g_censusDrawTallyArmed.load(std::memory_order_relaxed) && context == g_gameContext)
        g_censusDraws.fetch_add(1, std::memory_order_relaxed);
    costScope.PauseForForward();
    g_drawOriginal(context, indexCount, startIndex, baseVertex);
    costScope.ResumeAfterForward();
    g_inFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void HookUpdateSubresourceBody(UpdateSubresourceFn forward, ID3D11DeviceContext* context, ID3D11Resource* destination,
                                             UINT subresource, const D3D11_BOX* box, const void* sourceData,
                                             UINT rowPitch, UINT depthPitch) {
    // FOREIGN-CALLER FAST PATH. Nothing above this line, and no atomic.
    //
    // The entry behind the thunks is d3d11's SHARED UpdateSubresource
    // implementation: every context in the process funnels through it, the
    // OpenXR runtime's compositor included. Bisected to exactly this -- the
    // build where this hook first actually installs is the first slow one, and
    // its parent, carrying identical code that never installed because of a
    // broken comparison, is fast.
    //
    // The cost was ours to pay on the runtime's behalf. Every one of its upload
    // calls executed two lock-prefixed read-modify-writes on cache lines shared
    // with the game's threads, and it makes those calls inside xrEndFrame --
    // which is precisely where the 86 ms was measured.
    //
    // We only ever act on our own context, so a call from any other one should
    // cost a compare and a jump. The compare is a plain load: the pointer is
    // written once at install and read everywhere.
    if (context != g_gameContext) {
        // Plain, racy, non-atomic. Its only job is to say whether the runtime
        // calls this entry a handful of times a frame or a hundred thousand --
        // and an atomic here would reintroduce exactly the contention this fast
        // path exists to remove.
        ++g_foreignCallsRacy;
        forward(context, destination, subresource, box, sourceData, rowPitch, depthPitch);
        return;
    }
    // Scope starts HERE, past the foreign-caller test, so a call from the
    // runtime's compositor still costs one non-atomic increment and nothing
    // else. Every forward below is preceded by PauseForForward and none of them
    // resume, because each is the last real work in its branch -- the destructor
    // counts only up to the pause, so the driver's time is never charged to us.
    PluginCost::Scope costScope(PluginCost::kCameraUpload);
    // Past the foreign-caller test, so this IS the game's own immediate
    // context. Two relaxed atomics, and they answer the question the create
    // counter cannot: device creation is free-threaded and overlapping it with
    // an XR call proves nothing, but the immediate context is the one object in
    // D3D11 that is not thread-safe, and run 6 died inside Map on it.
    //
    // Note what the comment above already establishes: the runtime's compositor
    // calls this same shared entry from inside xrEndFrame. So the two really do
    // meet here, and this counts how often.
    NoteGameContextCall();
    // Pass-through mode: the detour stays installed and does NOTHING. If the
    // stall survives this, no amount of trimming the body will help and the cost
    // is the detour's mere presence -- most likely the unwind info for this
    // function no longer describing the prologue, which now executes in a
    // VirtualAlloc'd page that has none.
    const int bodyLevel = g_bodyLevel.load(std::memory_order_relaxed);
    if (g_hookPassThrough.load(std::memory_order_relaxed) || bodyLevel <= 0) {
                    costScope.PauseForForward();
        forward(context, destination, subresource, box, sourceData, rowPitch, depthPitch);
        return;
    }
    // Time the body itself. Pass-through is fast and the full body is slow, on
    // roughly 46 calls a frame -- which would mean about 1.9 ms per call, an
    // implausible figure for a few COM property reads and a 576-byte memcpy. So
    // either the body really is that slow and the number will say so, or the
    // speedup came from something pass-through ALSO skips, namely handing the
    // driver a stack buffer as the upload source. Those are different problems.
    LARGE_INTEGER bodyStart{};
    QueryPerformanceCounter(&bodyStart);
    struct BodyTimer {
        LARGE_INTEGER& start;
        ~BodyTimer() {
            LARGE_INTEGER end{}, frequency{};
            QueryPerformanceCounter(&end);
            QueryPerformanceFrequency(&frequency);
            if (!frequency.QuadPart) return;
            const double ms = static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 /
                              static_cast<double>(frequency.QuadPart);
            g_bodyMsRacy += ms;
            ++g_bodyCallsRacy;
        }
    } bodyTimer{bodyStart};
    g_inFlight.fetch_add(1, std::memory_order_acq_rel);
    g_hookInvocations.fetch_add(1, std::memory_order_relaxed);

    // RE-ENTRANCY GUARD.
    //
    // Three entries are hooked: the legacy thunk, UpdateSubresource1, and the
    // implementation behind them. The thunks CALL the implementation, so an
    // upload arriving through a thunk is patched here, forwarded, and then
    // reaches the implementation hook -- which patches it a second time.
    //
    // The result is a weapon rotated by twice the head delta on paths that
    // traverse both entries and once on paths that reach the implementation
    // directly, mixed together frame to frame. That is the flicker: it affects
    // everything carried in those uploads rather than the weapon alone, which is
    // why a separate shader on the gun flickers independently, and it does not
    // depend on the head moving at all, which is why it happens on a paused,
    // blurred screen.
    //
    // An inner call must forward what the outer one already produced, untouched.
    static thread_local int t_depth = 0;
    if (t_depth > 0) {
        g_hookReentrantCount.fetch_add(1, std::memory_order_relaxed);
        costScope.PauseForForward();
        forward(context, destination, subresource, box, sourceData, rowPitch, depthPitch);
        g_inFlight.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    struct DepthGuard {
        int& depth;
        explicit DepthGuard(int& d) : depth(d) { ++depth; }
        ~DepthGuard() { --depth; }
    } depthGuard(t_depth);
    if (context == g_gameContext) {
        g_hookContextMatched.fetch_add(1, std::memory_order_relaxed);
        if (destination && sourceData && subresource == 0) {
            D3D11_RESOURCE_DIMENSION probeDimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
            destination->GetType(&probeDimension);
            if (probeDimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
                D3D11_BUFFER_DESC probeDesc{};
                static_cast<ID3D11Buffer*>(destination)->GetDesc(&probeDesc);
                if (probeDesc.ByteWidth == kCameraBufferBytes &&
                    probeDesc.BindFlags == D3D11_BIND_CONSTANT_BUFFER) {
                    g_hookCameraSized.fetch_add(1, std::memory_order_relaxed);
                    // ATTRIBUTED BY THREAD, NOT BY TIME WINDOW. The F1 burst
                    // showed every frame carries ~35 of these and that they
                    // land in whichever measurement window happens to be open
                    // -- 26 in an idle spin on one frame, 32 in the nested
                    // pass on the next, with the frame total unchanged. So a
                    // before/after delta around a call cannot say the call
                    // made them. This can: t_passTag is set only on the
                    // re-entering thread, only inside a pass.
                    {
                        const int passTag = t_passTag;
                        if (passTag >= 0 && passTag <= 2)
                            g_uploadsByPass[passTag].fetch_add(1, std::memory_order_relaxed);
                    }
                    // AN ALWAYS-ON FAMILY TALLY, and the reason it is not the
                    // shear's one: g_lensShearFamilyCount only moves when the
                    // shear is armed, and xr.lens_shear is 0, so it reads zero
                    // for every family on every run that matters. A counter that
                    // is only live under a disarmed feature cannot answer "was
                    // the gun drawn at all", which is the first branch of the
                    // ADS report and the cheapest one to settle.
                    {
                        const int family =
                            LensShearFamilyBucket(static_cast<const std::uint8_t*>(sourceData));
                        if (family >= 0 && family < 4) {
                            g_uploadFamilyTally[family].fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    // P0's open branch, answered from bytes already in flight:
                    // the game's OWN matrix, read before anything downstream
                    // patches a copy of it.
                    //
                    // HERE, and not down in the offset/compensation block where
                    // it was first written. That block is gated on something
                    // being ARMED, so on a flat run with nothing armed the call
                    // was unreachable -- which is what produced a dump request
                    // with no dump. This is the only spot in the detour that
                    // runs on every camera-sized upload regardless of state.
                    OfferCameraPassMatrix(static_cast<const unsigned char*>(sourceData));
                }
                // PLAN-CURRENT F1. HERE FOR THE SAME REASON THE DUMP IS HERE.
                //
                // Every other classification site in this detour sits behind
                // the offset/compensation/pin gate, and this write must not:
                // that gate is open in a headset session (auto-arm turns the
                // compensation on) and CLOSED on a flat desk run until F3 is
                // pressed, so a HUD size that only worked with the placement
                // pin armed would read as a dead control on exactly the runs
                // used to tune it. This block runs on every upload regardless
                // of state, which is what it is for.
                //
                // 64 is tested before anything else, so the near-identity
                // 576-byte effects family the removed knob damaged is excluded
                // by shape rather than by predicate.
                //
                // !box IS LOAD-BEARING. With a destination box the call writes
                // a SUB-REGION, and sourceData is that region's bytes rather
                // than a matrix at offset 0 -- reading sixteen floats off it
                // and handing them back would corrupt whatever it actually is.
                // The 576-byte classifier guards on this for the same reason;
                // the read-only probe beside this one does not, and does not
                // have to, because it never substitutes what it read.
            }
        }
    }
    if (g_enabled.load(std::memory_order_relaxed) && context == g_gameContext && destination && sourceData &&
        subresource == 0) {
        g_totalCalls.fetch_add(1, std::memory_order_relaxed);
        // GetType/GetDesc read immutable descriptors on a caller-owned,
        // still-live resource. Nothing is retained, mapped, or written.
        D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        destination->GetType(&dimension);
        if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC desc{};
            static_cast<ID3D11Buffer*>(destination)->GetDesc(&desc);
            if (desc.BindFlags == D3D11_BIND_CONSTANT_BUFFER) {
                g_constantBufferCalls.fetch_add(1, std::memory_order_relaxed);
                if (desc.ByteWidth == kCameraBufferBytes) RecordCameraSizedUpdate(destination, sourceData, box);
            }
        }
    }
    // LEVEL 1 ends here: the counter blocks above have run, nothing has been
    // classified, and the game's own pointer is forwarded untouched.
    if (bodyLevel <= 1) {
                    costScope.PauseForForward();
        forward(context, destination, subresource, box, sourceData, rowPitch, depthPitch);
        g_inFlight.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    // The offset test substitutes a local copy as the source pointer. The
    // game's own bytes are left untouched, so nothing the engine may still
    // read or reuse after this call is disturbed.
    // THE PIN OPENS THIS GATE TOO, AND IT HAS TO.
    //
    // Everything that classifies a camera upload lives inside this block: the
    // main-scene capture that GetMainSceneHalfTangents depends on, and the
    // world-FOV match itself. With only the offset test and the compensation
    // able to open it, a flat run with nothing armed sees no main-scene upload
    // at all -- so the FOV match silently does nothing and HOME toggles a flag
    // no code reads. That is the SECOND measurement this gate has swallowed;
    // it also ate F1's matrix dump.
    //
    // Deliberately a third named opener rather than widening the condition to
    // "or the FOV match is on": g_viewmodelMatchWorldFov DEFAULTS TO TRUE, so
    // keying on it would quietly change the render path for every existing
    // configuration. The pin is the thing that takes ownership of the render
    // path (see P0-RENDER-PATH-SINGLE-OWNER.md section 5), so the pin is what
    // opens the gate, and nothing else behaves differently than before.
    // M3 IS THE FOURTH NAMED OPENER, AND IT IS THE THIRD MEASUREMENT THIS GATE
    // HAS SWALLOWED.
    //
    // The comment above already says it twice -- the FOV match, then F1's
    // matrix dump -- and M3's first run made it three. Everything that
    // classifies a camera upload lives inside this block, so with none of the
    // three openers set, `cameraSized` was false for every upload in the
    // session: the eye offset applied 0 times and the origin census counted 0
    // distinct values on BOTH batches.
    //
    // The run cost nothing to interpret only because the witness carried its
    // own disqualification: "batch 1 reading 0 distinct values means the
    // witness saw nothing and batch 2's number means nothing either", and the
    // upload ladder named the exact stage -- onOurContext +42, cameraSized +0.
    // A witness without that line would have reported "no divergence" and been
    // believed.
    //
    // Added as a NAMED opener rather than by widening the condition, which is
    // the rule the pin's entry above establishes: each opener is a mechanism
    // that has taken ownership of the render path, and it is listed.
    if ((g_offsetArmed.load(std::memory_order_acquire) ||
         g_compensationEnabled.load(std::memory_order_acquire) ||
         g_pinOwnsRenderPath.load(std::memory_order_acquire) ||
         g_m3Armed.load(std::memory_order_acquire)) &&
        context == g_gameContext && destination && sourceData &&
        subresource == 0 && !box) {
        D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        destination->GetType(&dimension);
        if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC desc{};
            static_cast<ID3D11Buffer*>(destination)->GetDesc(&desc);
            const auto* bytes = static_cast<const std::uint8_t*>(sourceData);
            const bool cameraSized = desc.ByteWidth == kCameraBufferBytes &&
                desc.BindFlags == D3D11_BIND_CONSTANT_BUFFER;
            // EVERY CONSTANT BUFFER SHAPE, NOT JUST THE 576-BYTE ONE.
            //
            // The whole census so far has been gated on cameraSized, i.e. on
            // ByteWidth == 576. That is a blind spot the size of the question:
            // the non-reticle HUD moved for none of the camera families, and if
            // it is drawn through a constant buffer of a different shape then
            // nothing here has ever seen it. This tallies the widths so one
            // read-only run says what else exists.
            if (g_uploadCensusEnabled.load(std::memory_order_relaxed) &&
                desc.BindFlags == D3D11_BIND_CONSTANT_BUFFER) {
                TallyConstantBufferWidth(desc.ByteWidth);
                if (desc.ByteWidth == 64) TallyMatrix64(bytes);
            }
            if (cameraSized) g_offsetCandidateCount.fetch_add(1, std::memory_order_relaxed);
            // Track the main scene's origin before anything consults it. The
            // main-scene uploads precede the near-plane-1 family within a frame,
            // so this is same-frame fresh where it is used.
            if (cameraSized && IsMainSceneCamera(bytes)) {
                // STEP SMOOTHING, CAUGHT IN THE ACT.
                //
                // Reported: walking up a step, distant terrain briefly jumps UP
                // -- as though descending -- and then corrects. Lowering a
                // camera is what makes distant things appear to rise, so the
                // view is being moved DOWN at the moment the player moves up.
                //
                // That is what Source's step smoothing does on purpose: the
                // origin snaps up a stair and the view height is offset
                // downward and eased back, so a monitor sees a glide instead of
                // a jolt. In a headset the inner ear has already reported the
                // step, and a view that dips to hide it is the one thing on the
                // list that can actually make someone ill.
                //
                // The upload's origin is the FINAL view origin, smoothing
                // included, so it settles this directly. A burst is recorded
                // around any sudden vertical move: if the height overshoots
                // downward and recovers, the smoothing is real and the fix is
                // to stop it. If it steps cleanly, the glitch is ours and lives
                // somewhere else entirely.
                {
                    // WHAT HAPPENS BEFORE THE JUMP, WHICH THE LAST VERSION
                    // COULD NOT SEE.
                    //
                    // The first attempt triggered ON the jump and recorded only
                    // what followed, so it proved there is no smoothing dip
                    // afterwards -- and was structurally blind to a dip
                    // BEFORE. "The terrain jumps the opposite way and then
                    // corrects" describes something happening around the step,
                    // not after it, so the window has to open earlier than the
                    // event that triggers it.
                    //
                    // A ring of the preceding samples is kept at all times and
                    // dumped when a step fires. It also samples once per FRAME
                    // rather than once per upload: there are three to four
                    // main-scene passes a frame, so the previous version logged
                    // every height three or four times and a "40 frame" window
                    // was really about thirteen.
                    // A RING NEEDS A WRAPPING WRITE INDEX, NOT A SATURATING
                    // COUNT.
                    //
                    // The first version wrote to ring[count % kRing] while
                    // count stopped advancing at kRing, so every sample after
                    // the sixteenth landed in slot 0 and the other fifteen
                    // froze at whatever they held during the first sixteen
                    // frames. The dump then read as one real value followed by
                    // fifteen copies of a stale one, which is not a window into
                    // anything.
                    // EVERY main-scene pass, not just the first.
                    //
                    // The guard fires correctly -- three suppressions of 4.84,
                    // 8.10 and 4.40 units -- and the glitch survives it, so the
                    // thing being corrected is not the thing being seen.
                    //
                    // The reported symptom is that TERRAIN IN THE DISTANCE
                    // jumps, and distant terrain is the 3D skybox, which
                    // renders in its own reduced-scale space. It passes
                    // IsMainSceneCamera identically -- same FOV, same near plane
                    // -- so "the first main-scene pass of the frame" may have
                    // been the skybox for this entire investigation, and a
                    // world-scale correction applied to a reduced-scale camera
                    // moves the distant vista by the wrong amount entirely.
                    //
                    // Three to four passes claim to be the main scene each
                    // frame. Logging one of them and calling it the view was the
                    // mistake underneath the last two conclusions. All of them
                    // are recorded now, with full origins, so the passes can be
                    // told apart by what they actually are.
                    constexpr int kRing = 16;
                    static float ring[kRing]{};
                    static int ringWrite = 0;
                    static int ringCount = 0;
                    static float lastZ = 0.0f;
                    static unsigned burst = 0;
                    static unsigned bursts = 0;
                    // First main-scene pass of the frame. g_frameIndex only
                    // advances inside a bounded trace, so it is frozen during
                    // ordinary play and cannot be used here; this counter is
                    // reset at every frame boundary and has not been
                    // incremented yet at this point in the pass.
                    if (g_mainScenePassesThisFrame == 0) {
                        float origin[3]{};
                        std::memcpy(origin, bytes + kCameraOriginOffset, sizeof(origin));
                        const float dz = origin[2] - lastZ;
                        if (burst) {
                            --burst;
                            g_stepBurstFrames = burst;
                        } else if (std::fabs(dz) > 2.0f && lastZ != 0.0f && bursts < 6) {
                            ++bursts;
                            burst = 6;
                            g_stepBurstFrames = burst;
                            char line[220]{};
                            std::snprintf(line, sizeof(line),
                                "[TF2VR] STEP: view height moved %+.2f in one frame (to %.2f). "
                                "The %d samples before it follow, then 16 after.\n",
                                dz, origin[2], ringCount);
                            Tf2VrLog(line);
                            // Oldest first, starting at the write cursor once
                            // the ring has wrapped, with each step so a dip is
                            // visible as a sign change rather than by comparing
                            // sixteen absolute numbers by eye.
                            float previous = 0.0f;
                            for (int i = 0; i < ringCount; ++i) {
                                const int index = (ringCount == kRing)
                                    ? ((ringWrite + i) % kRing) : i;
                                char before[180]{};
                                std::snprintf(before, sizeof(before),
                                              "[TF2VR]   before z=%.2f (%+.2f)\n", ring[index],
                                              i ? ring[index] - previous : 0.0f);
                                previous = ring[index];
                                Tf2VrLog(before);
                            }
                        }
                        // The guard itself, computed once per frame from the
                        // same sample. Holds the previous height for exactly one
                        // frame when the view drops faster than a step should,
                        // then releases whatever happens next.
                        static bool heldLastFrame = false;
                        g_stepCorrection = 0.0f;
                        if (g_stepGuard.load(std::memory_order_relaxed) && lastZ != 0.0f) {
                            const float threshold = g_stepGuardThreshold.load(std::memory_order_relaxed);
                            if (!heldLastFrame && dz < -threshold) {
                                g_stepCorrection = lastZ - origin[2];
                                heldLastFrame = true;
                                g_stepCorrectionCount.fetch_add(1, std::memory_order_relaxed);
                                if (g_stepCorrectionCount.load(std::memory_order_relaxed) <= 8) {
                                    char line[240]{};
                                    std::snprintf(line, sizeof(line),
                                        "[TF2VR] step guard: view dropped %.2f in one frame; holding "
                                        "the previous height for this frame only.\n", -dz);
                                    Tf2VrLog(line);
                                }
                            } else {
                                heldLastFrame = false;
                            }
                        }
                        ring[ringWrite] = origin[2];
                        ringWrite = (ringWrite + 1) % kRing;
                        if (ringCount < kRing) ++ringCount;
                        // The height we PRESENTED, so the next frame's delta is
                        // measured against what was shown rather than against a
                        // value nobody saw.
                        lastZ = origin[2] + g_stepCorrection;
                    }
                    // Per-pass, for every main-scene pass while a burst is open.
                    // The pass index and the full origin are what distinguish a
                    // reduced-scale sky camera from the player's own.
                    if (g_stepBurstFrames) {
                        float origin[3]{};
                        std::memcpy(origin, bytes + kCameraOriginOffset, sizeof(origin));
                        char line[220]{};
                        std::snprintf(line, sizeof(line),
                            "[TF2VR]   pass %u origin=(%.1f %.1f %.1f)\n",
                            g_mainScenePassesThisFrame, origin[0], origin[1], origin[2]);
                        Tf2VrLog(line);
                    }
                }
                // THE PLAYER'S PASS IS THE ONE NEAREST THE ENGINE'S CAMERA BASE.
                //
                // Measured 2026-09-10 23:51 (the first level's continue point): three to
                // six main-scene passes per frame, all IsMainSceneCamera, at origins
                // within a hundred units of each other and up to 50 units apart
                // vertically -- nothing the 1000-unit skybox test can separate. Last
                // writer won, so g_mainSceneOrigin flipped between passes inside a
                // frame, and everything that reads the anchor offset (CameraAnchor,
                // the hand compose, the pin) inherited the difference: the gun sat
                // 4..13 units ABOVE the eye instead of ~13 below, and jittered.
                //
                // The engine's own camera base is in hand, and the player's pass is
                // the one rendered nearest it (the eye raise apart). First pass of a
                // frame always stores; a later pass replaces it only if nearer.
                {
                    float passOrigin[3]{};
                    std::memcpy(passOrigin, bytes + kCameraOriginOffset, sizeof(passOrigin));
                    const float baseNow[3] = {BitsToFloatLocal(g_cameraBasePosXBits),
                                              BitsToFloatLocal(g_cameraBasePosYBits),
                                              BitsToFloatLocal(g_cameraBasePosZBits)};
                    const float dx = passOrigin[0] - baseNow[0], dy = passOrigin[1] - baseNow[1],
                                dz = passOrigin[2] - baseNow[2];
                    const float distanceNow = dx * dx + dy * dy + dz * dz;
                    static float nearestThisFrame = 0.0f;
                    static unsigned long long nearestReplaced = 0, nearestKept = 0;
                    // INTO A CANDIDATE, PUBLISHED AT THE FRAME BOUNDARY. 2026-09-11
                    // 00:55: with the nearest rule writing the globals directly, the
                    // first pass of every frame still landed there until a nearer one
                    // replaced it, and two CameraAnchor() calls microseconds apart read
                    // 17.7 units apart -- the whole height error. The readers see one
                    // value per frame now: the nearest pass of the LAST frame.
                    const bool firstOfFrame = g_mainScenePassesThisFrame == 0;
                    if (firstOfFrame || distanceNow < nearestThisFrame) {
                        if (!firstOfFrame) ++nearestReplaced;
                        nearestThisFrame = distanceNow;
                        std::memcpy(g_mainSceneCandidateOrigin, passOrigin, sizeof(passOrigin));
                        g_mainSceneCandidateBase[0] = baseNow[0];
                        g_mainSceneCandidateBase[1] = baseNow[1];
                        g_mainSceneCandidateBase[2] = baseNow[2];
                        std::memcpy(g_mainSceneCandidateMatrix, bytes + kCameraRelativeToClipOffset,
                                    sizeof(g_mainSceneCandidateMatrix));
                        g_mainSceneCandidateValid = true;
                    } else {
                        ++nearestKept;
                    }
                    // Once a second while a farther pass is being declined: the
                    // proof that the rule is choosing, and which way.
                    static std::uint64_t lastTick = 0;
                    const std::uint64_t now = GetTickCount64();
                    if (nearestKept && now - lastTick >= 1000) {
                        lastTick = now;
                        char line[260]{};
                        std::snprintf(line, sizeof(line),
                            "[TF2VR] main-scene anchor: nearest-to-base pass kept; farther passes declined "
                            "%llu, replaced-by-nearer %llu | anchor offset from base (%.1f %.1f %.1f) units\n",
                            nearestKept, nearestReplaced,
                            g_mainSceneOrigin[0] - g_mainSceneBase[0],
                            g_mainSceneOrigin[1] - g_mainSceneBase[1],
                            g_mainSceneOrigin[2] - g_mainSceneBase[2]);
                        Tf2VrLog(line);
                    }
                }
                // F3 needs the WHOLE world matrix, not just its half-tangents:
                // the check is "does the pinned rig land where M_world puts its
                // world position", and that is a projection, not a scale. Kept
                // beside the origin so the pair can never come from different
                // frames.
                // The angles this frame's world is being projected with. Pinned
                // here so the gun is corrected by the same rotation rather than
                // by whatever the latch holds when its own upload arrives.
                //
                // Mode 2 accepts only the pass whose origin is the world's. The
                // 3D skybox passes IsMainSceneCamera identically -- same FOV,
                // same near plane -- and differs exactly here, in rendering
                // from its own reduced-scale space.
                {
                    ++g_mainScenePassesThisFrame;
                    bool acceptPass = true;
                    if (g_pinMode.load(std::memory_order_relaxed) == 2 &&
                        g_referenceValid.load(std::memory_order_acquire)) {
                        float origin[3]{};
                        std::memcpy(origin, bytes + kCameraOriginOffset, sizeof(origin));
                        acceptPass = OriginsClose(origin, g_referenceOrigin, kSkyboxOriginThreshold);
                    }
                    if (acceptPass) PinFrameAngles();
                }
                // |r0| = 1/(aspect*tan(fovY/2)) and |r1| = 1/tan(fovY/2), so the
                // reciprocals are the frustum half-tangents directly.
                float row0[3]{}, row1[3]{};
                std::memcpy(row0, bytes + kCameraRelativeToClipOffset, sizeof(row0));
                std::memcpy(row1, bytes + kCameraRelativeToClipOffset + 4 * sizeof(float), sizeof(row1));
                const float length0 = std::sqrt(row0[0] * row0[0] + row0[1] * row0[1] + row0[2] * row0[2]);
                const float length1 = std::sqrt(row1[0] * row1[0] + row1[1] * row1[1] + row1[2] * row1[2]);
                if (length0 > 0.0001f && length1 > 0.0001f) {
                    // What the frame will ACTUALLY be rendered with, which is
                    // the headset's frustum when the match below is on. The
                    // projection layer submits these as the layer's FOV, so
                    // recording the pre-patch values here would tell the
                    // compositor the image covers 123.7 degrees when it covers
                    // 110, and stretch the world by that ratio.
                    float tanX = 1.0f / length0;
                    float tanY = 1.0f / length1;
                    float headsetTanX = 0.0f, headsetTanY = 0.0f;
                    if (MatchingHeadsetFov(headsetTanX, headsetTanY)) {
                        tanX = headsetTanX;
                        tanY = headsetTanY;
                    }
                    // kViewportFullAspectFix -- THE MIRROR OF THE PATCH, and it
                    // has to run HERE, above every use of tanX.
                    //
                    // This block used to sit ~40 lines further down, BELOW the
                    // store into g_mainSceneHalfTanX. tanX is a local; nothing
                    // read it after that line; the store is the only writer of
                    // that atomic. So the correction was computed into a value
                    // that immediately died, and the layer went on declaring
                    // the game's raw frustum for as long as the code existed.
                    // The 2026-08-23 handoff recorded it as "tried, did not fix
                    // it" -- it was never armed. A built fix sitting switched
                    // off is indistinguishable from a missing one, and this is
                    // the third time that has cost this project a session.
                    //
                    // What it mirrors: viewportFullFix rewrites row0 so the
                    // render lands on tanY * (headsetTanX/headsetTanY). This
                    // site reads the ORIGINAL bytes, so without the same
                    // adjustment it publishes a horizontal the game was never
                    // handed -- measured 115.7 deg declared against 109.7 deg
                    // rendered, a factor of 1.120 on the horizontal tangent and
                    // exactly 1.000 on the vertical. That value is used twice:
                    //
                    //   * the projection layer submits it as the layer FOV, so
                    //     the compositor maps the image onto an angle it does
                    //     not span. Horizontal-only, which is why it shows on
                    //     YAW and not on pitch, and depth-independent.
                    //   * matchViewmodelFov fits the weapon pass to it, so the
                    //     gun is scaled to a frustum we are not rendering.
                    //
                    // The predicate is the same one viewportFullFix uses: this
                    // whole block is already inside IsMainSceneCamera, and
                    // !g_matchHeadsetFov is !matchMainSceneFov for such an
                    // upload. The two cannot drift apart by construction.
                    //
                    // xr.declare_rendered_fov (default 1, bare F11) exists so
                    // the wearer can A/B it inside one session rather than
                    // across two runs judged from memory.
                    // AND g_fitHorizontal, because the mirror has to mirror
                    // something. Without this test, turning the narrowing off
                    // while leaving declare_rendered_fov on declares 109.75 for
                    // a render of 115.75 -- the identical error with the sign
                    // reversed, and it would have cost the whole next run. The
                    // rule is not "apply the correction to the published value"
                    // but "publish exactly what the patch below produced", so
                    // every condition the patch tests has to be tested here.
                    if (g_declareRenderedFov.load(std::memory_order_acquire) &&
                        g_fitHorizontal.load(std::memory_order_acquire) &&
                        g_viewportFull.load(std::memory_order_acquire) &&
                        !g_matchHeadsetFov.load(std::memory_order_acquire)) {
                        const float headTanX = g_headsetHalfTanX.load(std::memory_order_acquire);
                        const float headTanY = g_headsetHalfTanY.load(std::memory_order_acquire);
                        if (headTanX > 0.0001f && headTanY > 0.0001f && tanY > 0.0001f) {
                            tanX = tanY * (headTanX / headTanY);
                        }
                    }
                    // DOES THE MEASURED FRUSTUM FOLLOW ADS?
                    //
                    // Zooming narrows the game's FOV. The projection layer
                    // submits whatever is recorded here as the layer's FOV, so
                    // if this tracks the zoom the world correctly occupies less
                    // of the view -- which is what the black border on all four
                    // edges during ADS was, and it was right.
                    //
                    // That border disappearing is therefore not obviously a
                    // fix. If this value stops following the zoom, the layer
                    // keeps claiming the unzoomed FOV, the image fills the view
                    // again, and everything in it is at the wrong angular
                    // scale -- which is what "the sights do not line up" looks
                    // like. 75 frustum reports in the last session were
                    // identical to the decimal across ADS, which is the reason
                    // to doubt rather than the proof.
                    //
                    // THE RESTING REFERENCE IS TAKEN BEFORE C1 TOUCHES ANYTHING,
                    // AND THE ORDER IS LOad-BEARING.
                    //
                    // AdsWidenFactor divides the plateau rest by the current
                    // vertical tangent. Feeding it its own widened output would
                    // close a loop: the widened ADS tangent enters the plateau,
                    // the measured magnification falls, the widening shrinks,
                    // and the dial converges on doing nothing at all. So the
                    // plateau sees the GAME'S value and the store below sees
                    // ours, and the two calls can never be reordered without
                    // reintroducing that feedback.
                    NotePlateauRest(tanX, tanY);

                    // C1 NO LONGER TOUCHES THE RENDER, AND THE RECORDED VALUE
                    // STAYS THE GAME'S OWN.
                    //
                    // The render-side widening existed to SUPPRESS the ADS
                    // narrowing and give the periphery back. The wearer played
                    // it and rejected that outright -- "Zoom is not working at
                    // all... the actual landscape is not zooming" -- so the
                    // magnification is wanted, not dialled away.
                    //
                    // What replaces it scales what the layer DECLARES, in
                    // GetMainSceneHalfTangents, and nothing here. This store
                    // therefore keeps holding the frustum the game really
                    // rendered, which is what every other reader of it expects.
                    //
                    // Logged on change only, so ADS writes exactly two lines.
                    const float previousX = g_mainSceneHalfTanX.load(std::memory_order_acquire);
                    if (previousX > 0.0001f && std::fabs(tanX - previousX) > 0.01f * previousX) {
                        char line[300]{};
                        std::snprintf(line, sizeof(line),
                            "[TF2VR] main-scene frustum CHANGED to %.1f x %.1f deg (was %.1f wide), "
                            "tangent ratio %.3f | viewport at this upload %.0fx%.0f at (%.0f,%.0f), "
                            "aspect %.3f. THE TWO RATIOS MUST AGREE: the projection layer submits "
                            "the frustum as the layer FOV and the viewport as the layer's imageRect, "
                            "so a disagreement means the image is declared to span an angle it does "
                            "not, which the compositor's lens correction turns into visible "
                            "distortion.\n",
                            2.0f * std::atan(tanX) * 57.2957795f,
                            2.0f * std::atan(tanY) * 57.2957795f,
                            2.0f * std::atan(previousX) * 57.2957795f,
                            tanY > 0.0001f ? tanX / tanY : 0.0f,
                            g_lastViewportW.load(std::memory_order_acquire),
                            g_lastViewportH.load(std::memory_order_acquire),
                            g_lastViewportX.load(std::memory_order_acquire),
                            g_lastViewportY.load(std::memory_order_acquire),
                            g_lastViewportH.load(std::memory_order_acquire) > 0.5f
                                ? g_lastViewportW.load(std::memory_order_acquire) /
                                  g_lastViewportH.load(std::memory_order_acquire)
                                : 0.0f);
                        Tf2VrLog(line);
                    }
                    // ONCE, WHEN IT IS FIRST SEEN, not only when it changes.
                    //
                    // The "CHANGED" line above only fires on a change, so a run
                    // where the wearer never touches ADS reports the frustum
                    // NOWHERE -- which is exactly what happened while trying to
                    // confirm whether a projection correction had taken effect.
                    // A quantity this load-bearing should be in every log.
                    if (!g_frustumFirstReported) {
                        g_frustumFirstReported = true;
                        char first[300]{};
                        std::snprintf(first, sizeof(first),
                            "[TF2VR] main-scene frustum FIRST SEEN: %.1f x %.1f deg (tangent ratio "
                            "%.3f) into a %.0fx%.0f viewport (aspect %.3f). The headset displays "
                            "%.1f x %.1f. Rendering wider than the display shows costs twice: pixels "
                            "spent outside the visible cone, and wide-angle distortion inside it.\n",
                            2.0f * std::atan(tanX) * 57.2957795f,
                            2.0f * std::atan(tanY) * 57.2957795f,
                            tanY > 0.0001f ? tanX / tanY : 0.0f,
                            g_lastViewportW.load(std::memory_order_acquire),
                            g_lastViewportH.load(std::memory_order_acquire),
                            g_lastViewportH.load(std::memory_order_acquire) > 0.5f
                                ? g_lastViewportW.load(std::memory_order_acquire) /
                                  g_lastViewportH.load(std::memory_order_acquire)
                                : 0.0f,
                            2.0f * std::atan(g_headsetHalfTanX.load(std::memory_order_acquire)) *
                                57.2957795f,
                            2.0f * std::atan(g_headsetHalfTanY.load(std::memory_order_acquire)) *
                                57.2957795f);
                        Tf2VrLog(first);
                    }
                    g_mainSceneHalfTanX.store(tanX, std::memory_order_release);
                    g_mainSceneHalfTanY.store(tanY, std::memory_order_release);
                    g_mainSceneTanStampMs.store(GetTickCount64(), std::memory_order_release);
                    g_mainSceneTanWrites.fetch_add(1, std::memory_order_acq_rel);
                    // NOTHING ELSE IS ADJUSTED HERE, deliberately.
                    //
                    // This is where the frustum is READ, and an earlier version
                    // "corrected" tanY at this point for xr.viewport_full. That
                    // changed only what we reported to the compositor: the game
                    // carried on projecting its original vertical FOV into the
                    // widened viewport, so the image stretched AND was declared
                    // to span an angle it did not have. The correction belongs
                    // where the projection matrix is written -- see
                    // viewportFullFix below -- and once it is applied there, the
                    // value read here follows on its own, which is the check
                    // that it really took.
                    // THE VIEWPORT THIS FRUSTUM BELONGS TO, captured at the same
                    // instant, because the two are only meaningful together.
                    //
                    // The game sets its viewport and then uploads the camera for
                    // it, so whatever RSSetViewports last saw IS this frustum's
                    // rectangle. Measured at a 4032x3648 target: the world pass
                    // runs in 4032x2520 at y=0, aspect 1.600, matching the
                    // 100.0 x 73.4 frustum exactly -- while the buffer we submit
                    // is 4032x3648, aspect 1.105. Declaring the frustum for the
                    // whole buffer is what stretches 2520 rows of content across
                    // 3648 rows of declared angle, and that is the vertical
                    // squash, the over-long gun and the warp under head motion.
                    {
                        // THE PAIRING IS CHECKED BEFORE IT IS BELIEVED. See
                        // world_rect_gate.h: during the opening video the last
                        // viewport before a main-scene upload is 32x32, four
                        // runs in a row (prev-2/3/4/5, 2026-09-09), and
                        // submitting it as the world is the "tiny magnified
                        // square". A refused rectangle leaves the previously
                        // accepted one standing; nothing else changes. Counted
                        // either way, so the log says which happened.
                        const float vw = g_lastViewportW.load(std::memory_order_acquire);
                        const float vh = g_lastViewportH.load(std::memory_order_acquire);
                        const float tw = g_backbufferWidth.load(std::memory_order_acquire);
                        const float th = g_backbufferHeight.load(std::memory_order_acquire);
                        if (tw < 1.0f || th < 1.0f) {
                            g_worldRectUnknownTarget.fetch_add(1, std::memory_order_relaxed);
                        }
                        const bool plausible = !g_worldRectGate.load(std::memory_order_relaxed) ||
                                               tf2vr::WorldPassViewportPlausible(vw, vh, tw, th);
                        if (plausible) {
                            g_worldRectAccepted.fetch_add(1, std::memory_order_relaxed);
                            g_mainSceneViewportX.store(g_lastViewportX.load(std::memory_order_acquire),
                                                       std::memory_order_release);
                            g_mainSceneViewportY.store(g_lastViewportY.load(std::memory_order_acquire),
                                                       std::memory_order_release);
                            g_mainSceneViewportW.store(vw, std::memory_order_release);
                            g_mainSceneViewportH.store(vh, std::memory_order_release);
                        } else {
                            const unsigned long long rejected =
                                g_worldRectRejected.fetch_add(1, std::memory_order_relaxed) + 1;
                            // The first refusal and every change of the refused
                            // size, capped: a 46 s video would otherwise write
                            // a line per frame. The per-second heartbeat in
                            // NotifyCameraUpdateFrameBoundary carries the count.
                            static float lastRejectedW = -1.0f, lastRejectedH = -1.0f;
                            static unsigned rejectLines = 0;
                            if ((vw != lastRejectedW || vh != lastRejectedH) && rejectLines < 12) {
                                lastRejectedW = vw;
                                lastRejectedH = vh;
                                ++rejectLines;
                                char line[300]{};
                                std::snprintf(line, sizeof(line),
                                    "[TF2VR] WORLD-RECT GATE: refused %.0fx%.0f as the world pass "
                                    "(target %.0fx%.0f, under half in an axis); keeping %.0fx%.0f. "
                                    "Refused %llu so far.\n",
                                    vw, vh, tw, th,
                                    g_mainSceneViewportW.load(std::memory_order_acquire),
                                    g_mainSceneViewportH.load(std::memory_order_acquire), rejected);
                                Tf2VrLog(line);
                            }
                        }
                    }
                    // The widest frustum seen is the un-zoomed one; everything
                    // narrower is a zoom. Taking the maximum rather than a
                    // configured constant means this needs to know nothing
                    // about the player's FOV slider or the weapon in hand.
                    if (tanX > g_restingWorldTanX.load(std::memory_order_acquire)) {
                        g_restingWorldTanX.store(tanX, std::memory_order_release);
                    }
                    NoteFovSignals(tanX);
                    // Announced on the edge, so "the weapon went flat" is
                    // attributable to a zoom rather than to a mystery.
                    static bool wasZoomed = false;
                    const bool zoomed = WorldIsZoomed();
                    if (zoomed != wasZoomed) {
                        wasZoomed = zoomed;
                        if (g_weaponMonoEye.load(std::memory_order_acquire) >= 0 &&
                            g_weaponMonoOnlyWhenZoomed.load(std::memory_order_relaxed)) {
                            // With the ratio, so a weapon that fails to trigger
                            // says by how much it missed rather than saying
                            // nothing at all -- which is how the pistol went
                            // undetected.
                            // Both candidate signals on one line, so which
                            // separates ADS from rest more cleanly is a
                            // comparison and not a guess. The world's ratio is
                            // known to be marginal for a pistol; the weapon's
                            // has never been measured un-confounded.
                            const float restingWeapon =
                                g_restingWeaponTanX.load(std::memory_order_acquire);
                            char line[340]{};
                            std::snprintf(line, sizeof(line),
                                "[TF2VR] %s: world %.3f of resting (threshold %.3f), WEAPON %.3f of "
                                "resting. Weapon %s.\n",
                                zoomed ? "ZOOMED" : "un-zoomed",
                                tanX / g_restingWorldTanX.load(std::memory_order_acquire),
                                g_zoomThreshold.load(std::memory_order_relaxed),
                                restingWeapon > 0.0001f
                                    ? g_weaponTanX.load(std::memory_order_acquire) / restingWeapon
                                    : 0.0f,
                                zoomed ? "snaps to centre, sights on the round's axis, flat while "
                                         "zoomed"
                                       : "returns to full stereo and has depth again");
                            Tf2VrLog(line);
                        }
                    }
                }
            }
            // The viewmodel probe deliberately selects the family every other
            // mode excludes, so the two can never touch the same uploads.
            const bool selected = g_offsetModeKind == OffsetMode::ViewmodelProbe
                ? IsViewmodelCandidateCamera(bytes)
                : IsWorldPerspectiveCamera(bytes);
            // Compensation is not an offset-test mode: it has to be able to run
            // at the same time as per-eye stereo, and both patch the same
            // upload. They are composed into one local copy and forwarded once.
            // The weapon family, drawn from the dominant eye rather than from
            // whichever eye this frame belongs to. Selected here so the gun's
            // own origin-(0,0,0) pass is included too: it is the one that
            // currently takes no offset at all, and leaving it out is exactly
            // the incoherence being fixed.
            const bool weaponMonoPass = cameraSized &&
                g_weaponMonoEye.load(std::memory_order_acquire) >= 0 &&
                (!g_weaponMonoOnlyWhenZoomed.load(std::memory_order_relaxed) || WorldIsZoomed()) &&
                IsViewmodelCandidateCamera(bytes);
            const bool offsetSelected =
                g_offsetArmed.load(std::memory_order_acquire) && (selected || weaponMonoPass);
            // M3. Keyed on the render-thread discriminator and on nothing else:
            // no thread-local (the uploads are not on the submitting thread),
            // no window delta (they are asynchronous), no camera-byte matching
            // (that was the circularity B2 was waiting on).
            // WORLD GEOMETRY ONLY, AND THE VIEWMODEL IS EXEMPT.
            //
            // The first run that actually moved the camera came back with NO
            // WEAPON in the offset eye -- you could see the container that had
            // been behind it. The gun did not vanish: the viewmodel pass took
            // the same 12-unit eye translation, and it renders camera-relative
            // at arm's length, so 12 units throws it clean out of frame.
            //
            // This is the predicate pair `stepCorrect` already uses eight lines
            // below, for the same reason its comment gives: "World geometry
            // only. The viewmodel is camera-relative and rides the camera
            // already." The shipped per-eye path exempts the weapon by name
            // too. I applied the offset to every perspective family and this is
            // what that costs.
            //
            // The gun will need per-eye treatment eventually -- it just is not
            // built by translating its camera, and it is not this rung's
            // question. This rung's question is whether the WORLD diverges.
            const bool batch2WorldFamily =
                cameraSized && IsWorldPerspectiveCamera(bytes) && !IsViewmodelCandidateCamera(bytes);
            // Never patch our own pre-draw write; it is already offset.
            const bool batch2Offset = batch2WorldFamily &&
                                      !t_preDrawWriteInProgress &&
                                      g_m3Armed.load(std::memory_order_relaxed) &&
                                      g_batch2OffsetArmed.load(std::memory_order_relaxed) &&
                                      Batch2IpdModeWants(RtvCensusInBatch2());
            if (cameraSized && !batch2WorldFamily && g_m3Armed.load(std::memory_order_relaxed) &&
                RtvCensusInBatch2()) {
                g_batch2OffsetsSkippedFamily.fetch_add(1, std::memory_order_relaxed);
            }
            // AND THE WITNESS HAS ITS OWN GATE, WIDER THAN THE LEVER'S.
            //
            // The tally lives inside the patch block, and batch 1 arms no
            // lever -- so keying the block on batch2Offset alone would have
            // meant batch 1's uploads never entered it, batch 1's origin list
            // came back EMPTY, and the control read zero while batch 2 read
            // one. That is the shape of every disqualified witness in this
            // project's record, and it would have looked like a result.
            //
            // So while M3 is armed at all, every camera-sized upload builds the
            // copy and gets tallied, on both batches. It costs one 576-byte
            // memcpy on ~40 uploads a frame.
            const bool m3Witness = cameraSized && g_m3Armed.load(std::memory_order_relaxed);
            // Evaluated once: IsBodyWeaponPass also tallies exclusions, and the
            // "skipped for want of head data" count must not absorb them.
            const bool bodyPass = cameraSized &&
                                  g_compensationEnabled.load(std::memory_order_acquire) &&
                                  IsBodyWeaponPass(bytes);
            // THE HEAD MUST NOT TOUCH THE GUN IN ADS.
            //
            // "Tilting my head up down also appears to adjust the vertical angle
            // of the gun slightly... I expected arm to control everything, and
            // head really does nothing."
            //
            // This correction rotates the weapon pass by the head's rotation
            // relative to the recentre reference, to keep the gun in the BODY's
            // frame while the head turns. That is right when the engine owns the
            // gun's placement. In ADS the placement pin writes the gun's pose
            // explicitly from the hand, so cancelling a head rotation on top of
            // it is a second author on one quantity -- and it shows up exactly
            // as reported: the gun drifting with the head by the residual.
            //
            // So it stands down while ADS is engaged. One author for the gun's
            // orientation at a time.
            // THE HEAD-CANCEL STAYS ON IN ADS WHILE THE PIN DRIVES THE HAND.
            // Settled 2026-09-04 by the ADS trace: gun committed yaw 119.6 = the
            // rendered view (engine 70.9 + head 48.6) = the hand, and the wearer
            // saw the gun drawn at 70.9, the body's forward. The weapon pass
            // renders in the body's frame unless this cancel puts the head's
            // rotation back, for a view-locked gun exactly as for a hand-placed
            // one. The one build that stood it down in ADS (7d94a59) is reverted.
            const bool adsHandDriven = AdsLockEngaged() && IsPlacementPinHandDriven() &&
                                       IsViewmodelPlacementPinArmed();
            const bool compensate = bodyPass && g_headCompensationValid &&
                                    (!AdsLockEngaged() || adsHandDriven);
            // The HUD's own pass, which bodyPass now deliberately excludes.
            // Only selected when the wearer has asked for the aim anchor;
            // mode 0 leaves it untouched and head-locked, which is the
            // known-good default.
            // Evaluated ONCE and shared, so the dropout counters below describe
            // the same two tests the passes are actually selected by rather than
            // a second opinion that could disagree with them.
            const bool vmCandidateCam = cameraSized && IsViewmodelCandidateCamera(bytes);
            const bool nearOriginCam = cameraSized && IsNearOriginPerspectiveCamera(bytes);
            // THE HUD PASS BY THE SAME PREDICATE THE FRUSTUM REFIT USES. This gate
            // was IsNearOriginPerspectiveCamera (origin within 1 unit) and the
            // pass sits farther out than that as soon as the head moves -- the
            // note beside hud.fov_origin_max says so -- so the anchor never
            // applied in run 5, in either sign (wearer: "no change on F12").
            const bool hudOriginNear = cameraSized && vmCandidateCam && IsHudFovOriginNear(bytes);
            const bool hudAnchorPass = hudOriginNear && g_headCompensationValid &&
                g_hudAnchorMode.load(std::memory_order_relaxed) != 0;
            // The SAME pass hudAnchorPass names, without the anchor-mode gate:
            // the near-origin perspective camera the type-3 HUD draws through.
            // The refit uses the WIDENED origin bound; the anchor pass above
            // keeps the original, because it is a different feature, it is off,
            // and this run is not about it.
            const bool hudFovMatchPass = hudOriginNear &&
                g_hudFovMode.load(std::memory_order_relaxed) != 0;
            // Read-only: which frame is this pass drawn in? Ungated by the
            // fov-match mode so it reports whether or not that mode is on.
            if (hudOriginNear) NoteHudPassFrame(bytes);
            // The span of frames this window covers, so "frames refitted" has a
            // denominator. Taken on every camera-sized upload, refit or not.
            if (cameraSized) {
                const std::uint32_t f = g_frameIndex.load(std::memory_order_relaxed);
                std::uint32_t firstSeen = g_hudFovWindowFirstFrame.load(std::memory_order_relaxed);
                if (firstSeen == 0xFFFFFFFFu) {
                    g_hudFovWindowFirstFrame.compare_exchange_strong(firstSeen, f,
                                                                     std::memory_order_relaxed);
                }
                std::uint32_t lastSeen = g_hudFovWindowLastFrame.load(std::memory_order_relaxed);
                while (f > lastSeen && !g_hudFovWindowLastFrame.compare_exchange_weak(
                                           lastSeen, f, std::memory_order_relaxed)) {
                }
            }
            // THE DROPOUT WITNESS. An upload that satisfies one half of the pass
            // predicate and not the other is one the refit declines while it
            // still looks like the HUD pass -- which is what a few frames of
            // un-refitted HUD would be. Counted always; costs two compares.
            if (nearOriginCam && !vmCandidateCam) {
                g_hudFovNearNotVm.fetch_add(1, std::memory_order_relaxed);
            } else if (vmCandidateCam && !nearOriginCam) {
                g_hudFovVmNotNear.fetch_add(1, std::memory_order_relaxed);
            }
            if (bodyPass && !compensate) g_compensationSkippedCount.fetch_add(1, std::memory_order_relaxed);
            // Census of the uploads this correction is about to leave alone.
            // Same window as the correction trace, so the two can be read
            // side by side: every camera-sized upload of a frame appears in
            // exactly one of the two lists.
            // First two frames only. The census found what it was built for --
            // the gun's pass being excluded -- and at thirty frames it would
            // bury the corr lines under eight hundred of its own.
            if (cameraSized && !compensate &&
                g_correctionTraceFrames.load(std::memory_order_acquire) &&
                g_correctionTraceFrameIndex.load(std::memory_order_acquire) < 2) {
                float origin[3]{};
                std::memcpy(origin, bytes + kCameraOriginOffset, sizeof(origin));
                // m[15] as well as the origin. worldPersp folds the two together,
                // so a 0 there could mean "orthographic" or "at the origin" and
                // the census could not say which -- and the whole diagnosis
                // turns on it being the second.
                float element15 = 0.0f;
                std::memcpy(&element15, bytes + kMatrixElement15Offset, sizeof(element15));
                char line[340]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] uncorrected f%u #%u: nearPlane=%.4f m15=%.4f origin=(%.1f %.1f %.1f) "
                    "worldPersp=%d viewmodelFamily=%d mainScene=%d\n",
                    g_correctionTraceFrameIndex.load(std::memory_order_acquire),
                    g_censusOrdinal.fetch_add(1, std::memory_order_acq_rel),
                    NearPlaneElement(bytes), element15, origin[0], origin[1], origin[2],
                    IsWorldPerspectiveCamera(bytes) ? 1 : 0,
                    IsViewmodelCandidateCamera(bytes) ? 1 : 0,
                    IsMainSceneCamera(bytes) ? 1 : 0);
                Tf2VrLog(line);
            }

            // Force the weapon onto the world's frustum. Needs a measured world
            // frustum, which is why it silently does nothing until a
            // main-scene upload has been seen this session.
            float worldTanX = 0.0f, worldTanY = 0.0f;
            // THE WEAPON IS FITTED TO THE RENDERED FRUSTUM, NOT THE DECLARED ONE.
            //
            // "zoomed weapons all wrong and still overcompensating on
            // everything ... 2 bricks up = 4 bricks up for the bullets."
            //
            // Fitting to the DECLARED frustum keeps the gun at true angular size
            // while the world is stretched by the magnification, so the gun and
            // the world move across the screen at different rates -- move the
            // arm until the gun crosses two bricks and the round crosses
            // magnification times two. I argued that was correct because a real
            // scope does not magnify the rifle. That was wrong twice over: the
            // wearer aims by the relationship between what they move and where
            // it lands, and that relationship must be 1:1 at every zoom.
            //
            // Fitting to the RENDERED frustum puts the gun through the same
            // stretch as the world, so gun, reticle and round all move together.
            // It costs the gun being drawn magnification times larger, which is
            // a real cost and is what weapon.size exists to trim. A wrong
            // relationship cannot be trimmed.
            // M3'S OPENER MUST BE BEHAVIOURALLY INERT, and this is the price of
            // adding it. Everything that classifies a camera upload lives
            // behind that gate -- but so does every OTHER lever, and two of
            // them would have woken on the live config the moment M3 opened it:
            // `viewmodel.worldfov` DEFAULTS TO TRUE (the gate's own comment
            // says so, in the sentence explaining why the pin was added as a
            // named opener rather than by widening), and `hud2d.zoom` is 0.262
            // in the wearer's ini. Both change the picture, neither is the
            // change this rung declared, and both would have arrived silently.
            //
            // So: when M3 is the ONLY thing holding the gate open, nothing else
            // gets to act. With any of the original three openers set, every
            // lever behaves exactly as it did before this line existed.
            const bool gateOpenedByM3Alone =
                g_m3Armed.load(std::memory_order_acquire) &&
                !g_offsetArmed.load(std::memory_order_acquire) &&
                !g_compensationEnabled.load(std::memory_order_acquire) &&
                !g_pinOwnsRenderPath.load(std::memory_order_acquire);
            const bool matchViewmodelFov = !gateOpenedByM3Alone && cameraSized &&
                g_viewmodelMatchWorldFov.load(std::memory_order_acquire) &&
                IsViewmodelCandidateCamera(bytes) &&
                GetMainSceneRenderedHalfTangents(worldTanX, worldTanY);
            // Narrow the main scene onto the headset's frustum, so no pixel is
            // rendered outside the cone the display can show.
            float headsetTanX = 0.0f, headsetTanY = 0.0f;
            // THE MAIN SCENE IS NOT THE ONLY PASS IN THE FRAME, and that is why
            // the horizontal fit is now OFF by default.
            //
            // The upload census of the 2026-08-23 run lists three perspective
            // families sharing one image, all with the game's own 1.600 tangent
            // ratio:
            //
            //   near=-7.000  |r0|=0.628 |r1|=1.005  n=1931   the main scene
            //   near=-0.007  |r0|=0.628 |r1|=1.005  n=4733   world effects
            //   near=-1.000  |r0|=1.190 |r1|=1.904  n=1671   the viewmodel
            //
            // IsMainSceneCamera keys on the -7 near plane, so ONLY the first is
            // narrowed. The second renders the same world at 115.75 deg while
            // the main scene renders it at 109.75, which puts the same world
            // angle at two different NDC x -- a horizontal misregistration of
            // up to 12%, zero at the centre, growing outward, riding whatever
            // moves. That is the wearer's "effects layers on the gun and on
            // scenery that do not line up and move as I move".
            //
            // Narrowing every pass is not the fix: the families are separated
            // by heuristics on near plane and origin, and a heuristic set that
            // has to be exhaustive to be correct will not stay correct. The
            // frustum ratio has to be one value for the whole frame, and the
            // only value every pass already agrees on is the game's own.
            //
            // So: leave the projection alone, and declare what was rendered.
            // The cost is that the game's 115.75 deg horizontal is wider than
            // the 110 the display shows, so about 5% of the horizontal pixels
            // land outside the visible cone. That is a sharpness cost measured
            // in single figures against a geometry error measured in degrees.
            // NOT gated on g_viewportFull any more. It was, and the F11 arm of
            // the 2026-08-23 viewport A/B therefore logged nothing at all --
            // the wearer reported that arm as WORSE and there is no measurement
            // of it. An instrument must not switch off with the thing it is
            // there to compare.
            const bool mainSceneUpload = cameraSized && IsMainSceneCamera(bytes);
            // WHICH STAGE OF THE LADDER DIES, counted rather than inferred.
            //
            // Three flat sessions produced no FRUSTUM line at all and there was
            // no way to tell "the hook is never called" from "it is called but
            // nothing is camera-sized" from "camera-sized uploads arrive but
            // none passes the near-plane test". The counters for the first two
            // already existed and are only ever PRINTED when the viewmodel
            // correction is armed -- which it never is in a flat run -- so they
            // were invisible exactly where they were needed.
            //
            // The near plane of the last camera-sized upload is the one that
            // decides it: IsMainSceneCamera wants it within 0.5 of -7, and if a
            // flat session uploads a different value that is the whole answer.
            if (cameraSized) {
                ++g_cameraSizedSeenRacy;
                g_lastCameraSizedNearPlane = NearPlaneElement(bytes);
                if (IsWorldPerspectiveCamera(bytes)) ++g_worldPerspectiveSeenRacy;
            }
            if (mainSceneUpload) ++g_mainSceneSeenRacy;
            const bool viewportFullFix = !gateOpenedByM3Alone && mainSceneUpload &&
                g_fitHorizontal.load(std::memory_order_acquire);
            // W2. Evaluated once here so the patch site and the predicate that
            // decides whether to BUILD a patched copy can never disagree about
            // it -- an upload selected for the shear and then not sheared would
            // leave that family behind, which is the exact defect this design
            // exists to make impossible.
            float lensShearNdcY = 0.0f;
            const bool lensShear = !gateOpenedByM3Alone && cameraSized && ComputeLensShearNdcY(bytes, lensShearNdcY);
            // THE RESET, AND IT IS DELIBERATELY OUT HERE RATHER THAN BESIDE THE
            // PATCH. The patch lives inside a block gated on at least one
            // correction being armed, so a reset written next to it would never
            // run in exactly the case that needs it -- the shear switching off.
            //
            // The projection layer declares a frustum built from this offset. If
            // the shear stops (the toggle, a lost frustum measurement, a runtime
            // that never named its optics) and the published offset stayed where
            // it was, the layer would declare a shifted frustum over an
            // unsheared render: a worse error than the one being fixed, and one
            // that would read as the fix half-working. So every main-scene
            // upload publishes, one way or the other, from outside every gate.
            if (mainSceneUpload && !lensShear) {
                g_lensShearAppliedNdcY.store(0.0f, std::memory_order_release);
            }
            // THE HEARTBEAT, AND IT EXISTS TO STOP A SILENT NO-OP LOOKING LIKE A
            // WORKING FIX.
            //
            // The LENS SHEAR line only fires when the shear CHANGES, so a build
            // in which the shear never applies at all produces exactly the same
            // log as a headset that does not need it -- nothing. That is the
            // failure this project has paid for repeatedly: a built fix sitting
            // switched off is indistinguishable from a missing one.
            //
            // So while nothing has ever been sheared, every 900th main-scene
            // upload prints the decline census. Bounded twice over: only until
            // the first shear lands, and at most five reports.
            if (mainSceneUpload) {
                const std::uint32_t seen =
                    g_lensShearMainSceneSeen.fetch_add(1, std::memory_order_relaxed) + 1;
                const bool everSheared =
                    g_lensShearFamilyCount[kShearFamilyMainScene].load(std::memory_order_relaxed) != 0;
                if (!everSheared && seen % 900u == 0u &&
                    g_lensShearReports.fetch_add(1, std::memory_order_relaxed) < 5u) {
                    char line[560]{};
                    std::snprintf(line, sizeof(line),
                        "[TF2VR] LENS SHEAR HAS NEVER APPLIED after %u main-scene uploads. Declines: "
                        "disabled=%u orthographic=%u no-lens-geometry=%u no-frustum-yet=%u "
                        "lens-is-centred=%u. Only the LAST of those is a headset that does not need "
                        "the fix; the others are the fix not running. no-lens-geometry means "
                        "xrLocateViews has not reported optics -- flat, or XR not up yet.\n",
                        seen,
                        g_lensShearDecline[kShearDeclineDisabled].load(std::memory_order_relaxed),
                        g_lensShearDecline[kShearDeclineOrthographic].load(std::memory_order_relaxed),
                        g_lensShearDecline[kShearDeclineNoLens].load(std::memory_order_relaxed),
                        g_lensShearDecline[kShearDeclineNoFrustum].load(std::memory_order_relaxed),
                        g_lensShearDecline[kShearDeclineCentred].load(std::memory_order_relaxed));
                    Tf2VrLog(line);
                }
            }
            const bool matchMainSceneFov = !gateOpenedByM3Alone && cameraSized && IsMainSceneCamera(bytes) &&
                MatchingHeadsetFov(headsetTanX, headsetTanY);
            const float hud2dZoom = g_hud2dZoom.load(std::memory_order_relaxed);
            const float hud2dX = g_hud2dShiftX.load(std::memory_order_relaxed);
            const float hud2dY = g_hud2dShiftY.load(std::memory_order_relaxed);
            const bool hud2dAdjust = !gateOpenedByM3Alone && cameraSized &&
                (std::fabs(hud2dZoom - 1.0f) > 0.0001f || std::fabs(hud2dX) > 0.0001f ||
                 std::fabs(hud2dY) > 0.0001f) &&
                IsScreenSpaceHudPass(bytes);
            if (cameraSized && g_uploadCensusEnabled.load(std::memory_order_relaxed)) {
                TallyUploadFamily(bytes);
            }
            // THE FRUSTUM CENSUS (frustum_census.h): every 576-byte camera
            // upload, bucketed by its own projection matrix, with each family's
            // full matrix printed. NOT gated on the upload census, because the
            // question is which frustums exist at all.
            if (cameraSized) FrustumCensusSample(bytes);
            // World geometry only. The viewmodel is camera-relative and rides
            // the camera already, so correcting it too would move the gun
            // relative to the hands holding it.
            const bool stepCorrect = !gateOpenedByM3Alone && cameraSized && g_stepCorrection != 0.0f &&
                IsWorldPerspectiveCamera(bytes) && !IsViewmodelCandidateCamera(bytes);
            // The weapon family gets the lean taken back off, so the body stays
            // put while the head moves around it.
            // NO LEAN DETACH WHILE THE GUN IS THE VIEW. 2026-09-04, SIGHTNDC.
            // The detach subtracts the head's tracked offset from the weapon
            // pass so a HAND-held gun stays put in the room while the head
            // slides -- right for hip fire, where the gun is placed in the room.
            // The ADS gun is built from the eye and must move with it; detached,
            // it sat 1-2 units beside the view axis whenever the head was turned
            // (lean offset 1.5 u left, 2.5 u right, 0.5 straight; weapon-pass
            // view axis at x +0.0145 / -0.0136 / 0.000 in NDC), the "slightly
            // off with the head turned" the wearer saw. Hip fire is unchanged.
            const bool detachLean = !gateOpenedByM3Alone && cameraSized &&
                g_detachLean.load(std::memory_order_relaxed) > 0.001f &&
                g_cameraPositionWriteActive != 0 &&
                IsViewmodelCandidateCamera(bytes) &&
                !(AdsLockEngaged() && IsPlacementPinHandDriven() && IsViewmodelPlacementPinArmed());
            // The gun's own pass only. The other three viewmodel passes are the
            // body and arms and must keep the head-cancel untouched -- pinning
            // them would move the body with the hand, which is the rigid
            // whole-pass problem this exists to avoid.
            const bool pinWeapon = !gateOpenedByM3Alone && cameraSized &&
                g_weaponPinned.load(std::memory_order_acquire) &&
                IsPinnablePass(bytes);
            // THE BONE WRITE'S SEAM IS NO LONGER HERE.
            //
            // It used to fire from this point -- the viewmodel camera upload --
            // on the reasoning that an upload immediately before the viewmodel
            // draws must sit between the engine's bone setup and its use. That
            // reasoning was never tested, because the write's SURVIVAL was never
            // measured, and so 25 nulls in one day could not distinguish "wrong
            // array" from "right array, dead seam".
            //
            // This is the render side, downstream of the view build that stages
            // what the gun's draw actually reads. The write now happens in
            // CViewRender's view build -- after the engine tick, before the
            // render build -- through clientViewBuildInterceptor in
            // view_build_hook.asm, with per-target survival telemetry beside it.
            // See viewmodel_bones.cpp, ApplyWeaponBonePinAtViewBuild.
            //
            // -- AND IT IS BACK, ON EVIDENCE RATHER THAN ON HOPE. --
            //
            // The flat run of 2026-08-18 measured the view-build write as already
            // gone by the time this point is reached: intact on 0.8% of passes
            // for the 87-bone array and 11.8% for the 74. The engine's own bone
            // setup therefore happens BETWEEN the view build and here, which
            // makes this seam post-SetupBones by observation. That is exactly
            // where the decision table sends a write that is being overwritten,
            // so the write comes back here -- as one of two selectable seams,
            // because a switch lets one run A/B them instead of asking a later
            // run to remember what an earlier one looked like.
            //
            // NO once-per-frame gate any more. The old write applied a DELTA, so
            // a second application in a frame compounded the rotation and the
            // gate was load-bearing. The write is absolute now -- it composes
            // against the rotation the array is already carrying -- so every
            // viewmodel pass can re-apply it harmlessly, and doing so is what
            // gives it the best chance of standing when the draw reads it.
            if (cameraSized && IsViewmodelCandidateCamera(bytes)) {
                ApplyWeaponBonePinAtRenderSide();
            }
            // Read-only, and on EVERY camera-sized upload rather than only the
            // viewmodel ones. The staged pool is populated per MODEL DRAW and
            // rewound immediately -- sampling only at viewmodel uploads caught
            // the window open once in 89 passes, which is far too coarse to
            // catalogue what goes through it. Empty, this costs two pointer
            // reads.
            if (cameraSized) AdvanceBoneStagingSampleAtRender();
            // AND the pose drive on EVERY camera-sized upload, not only the
            // viewmodel ones. Driving it on the viewmodel passes alone reached
            // the arms about half the time and the gun about one frame in a
            // hundred: the value is right, the moment is not. More writes cost
            // two float adds each and give the engine fewer windows in which to
            // recompute over us before a consumer reads.
            if (cameraSized && (offsetSelected || compensate || matchViewmodelFov ||
                                matchMainSceneFov || viewportFullFix || stepCorrect || detachLean || pinWeapon ||
                                hud2dAdjust || lensShear || batch2Offset || m3Witness)) {
                // PERSISTENT, not a stack buffer.
                //
                // The body measures 0.001 ms a call and the runtime never calls
                // this entry, yet pass-through is fast and the full body is
                // slow. The only remaining difference is that patching hands
                // UpdateSubresource a different source pointer -- and, as a
                // stack local, a DIFFERENT ADDRESS on every call. That is free
                // on the CPU, which is why the body timing shows nothing, and
                // not necessarily free afterwards: a driver that pins or tracks
                // the source region sees a fresh one every time instead of the
                // same address reused, and the bill arrives at the next flush,
                // which is xrEndFrame.
                //
                // One buffer per thread, reused forever. Thread-local because
                // the hook is entered from more than one thread and a shared
                // buffer would need a lock in the hot path.
                static thread_local alignas(16) std::uint8_t patched[kCameraBufferBytes];
                std::memcpy(patched, bytes, kCameraBufferBytes);
                // Diagnostic: do every computation, then upload the GAME'S
                // pointer anyway. If a persistent buffer does not restore the
                // framerate, this separates "substituting the source pointer at
                // all is the cost" from "the cost is elsewhere in the body",
                // with the arithmetic held constant across both.
                // LEVEL 2 does all the arithmetic and uploads the game's pointer.
                const void* const uploadSource =
                    (bodyLevel <= 2 || g_uploadNoSubstitute.load(std::memory_order_relaxed))
                        ? sourceData
                        : static_cast<const void*>(patched);
                // Diagnostic: substitute a BYTE-IDENTICAL copy. Same extra
                // buffer, same substituted pointer, same upload call -- only the
                // arithmetic is gone.
                //
                // This is the test no_substitute cannot do. Everything cheap has
                // now been cleared: the body costs a microsecond, the detour is
                // innocent (pass-through is fast), and a persistent buffer
                // changed nothing. What is left is that we hand the driver
                // DIFFERENT BYTES -- and xrEndFrame, where the 87 ms sits,
                // blocks on the GPU finishing the frame. A matrix that makes the
                // GPU's work more expensive costs nothing on the CPU and shows
                // up there and nowhere else, which fits every measurement taken
                // so far.
                //
                //   fast -> the CONTENTS are the cost; the correction is making
                //           the GPU do real extra work, and the suspect is the
                //           projection-row scaling, which can magnify geometry
                //           into large-scale overdraw when its input is wrong.
                //   slow -> substituting at all is the cost, and UpdateSubresource
                //           is the wrong mechanism for patching this buffer.
                // LEVEL 3 ends here: identical bytes, substituted pointer.
                if (bodyLevel <= 3 || g_uploadIdentityCopy.load(std::memory_order_relaxed)) {
                    // AND THIS RETURN SITS IN FRONT OF M3'S EYE TRANSLATION.
                    //
                    // bodyLevel defaults to 4 so it does not fire today, but an
                    // implicit dependency on a diagnostic knob is exactly the
                    // kind of silence that just cost a run at the gate above.
                    // Say it once, by name, instead of applying nothing.
                    if (g_m3Armed.load(std::memory_order_relaxed)) {
                        static bool said = false;
                        if (!said) {
                            said = true;
                            char line[340]{};
                            std::snprintf(line, sizeof(line),
                                "[TF2VR] M3 CANNOT RUN: hook.body_level is %d (or the identity-copy "
                                "diagnostic is on), and that path forwards before the eye "
                                "translation is composed. The offset will apply ZERO times. "
                                "body_level must be 4.\n", bodyLevel);
                            Tf2VrLog(line);
                        }
                    }
                    costScope.PauseForForward();
                    forward(context, destination, subresource, box, patched, rowPitch, depthPitch);
                    g_inFlight.fetch_sub(1, std::memory_order_acq_rel);
                    return;
                }
                if (stepCorrect) {
                    // Same mechanism as the eye offset: moving the camera by e
                    // substitutes p' = p - e, so each row's column-3 term loses
                    // that row's projection of e. Source Z is up, so a purely
                    // vertical e raises the camera back to where it was.
                    //
                    // NOT written into m[12..15]; that form corrupted the
                    // forward/w row when the eye offset first tried it, and the
                    // note there says plainly not to reintroduce it.
                    float matrix[16]{};
                    std::memcpy(matrix, patched + kCameraRelativeToClipOffset, sizeof(matrix));
                    const float e[3] = {0.0f, 0.0f, g_stepCorrection};
                    for (int row = 0; row < 4; ++row) {
                        const float* basis = matrix + row * 4;
                        matrix[row * 4 + 3] -= basis[0] * e[0] + basis[1] * e[1] + basis[2] * e[2];
                    }
                    std::memcpy(patched + kCameraRelativeToClipOffset, matrix, sizeof(matrix));
                    // Keep the origin field consistent with the matrix, for
                    // anything downstream that reads the camera's world position
                    // rather than deriving it from the transform.
                    float origin[3]{};
                    std::memcpy(origin, patched + kCameraOriginOffset, sizeof(origin));
                    origin[2] += g_stepCorrection;
                    std::memcpy(patched + kCameraOriginOffset, origin, sizeof(origin));
                }
                // hud2d.head_anchor -- THE FLAT HUD PASS BACK IN FRONT OF THE HEAD.
                //
                // Runs 4 and 5 (2026-09-10): the engine places this pass's content
                // -- reticle, loading overlay, fade -- where the ATTACK angles
                // point, and aim.cmd = 2 makes those the right hand. Rotating the
                // near-origin perspective pass did nothing (it is not where that
                // content lives). The pixel-ortho pass is, and hud2d.shift already
                // translates it in pass pixels, so this is the same translation
                // driven per frame by the anchor hud2d.zoom scales about: the aim
                // minus the applied camera angles, through the main-scene
                // half-tangents, in pass pixels. Content at the anchor lands at
                // the centre. The reticle comes along -- it is in this pass -- so
                // with this on the aim mark sits at the view centre, not on the
                // gun. A pass-level lever; the per-widget split is the next step.
                //
                // No tracked aim: nothing shifts, which is also what the engine
                // does (wearer: controller out of view, overlay on the head).
                if (!gateOpenedByM3Alone && cameraSized &&
                    g_hud2dHeadAnchor.load(std::memory_order_relaxed) &&
                    IsScreenSpaceHudPass(bytes)) {
                    float matrix[16]{};
                    std::memcpy(matrix, patched + kCameraRelativeToClipOffset, sizeof(matrix));
                    const float m00 = matrix[0 * 4 + 0];
                    const float m11 = matrix[1 * 4 + 1];
                    const float width = (m00 != 0.0f) ? std::fabs(2.0f / m00) : 0.0f;
                    const float height = (m11 != 0.0f) ? std::fabs(2.0f / m11) : 0.0f;
                    float halfTanX = 0.0f, halfTanY = 0.0f;
                    float aim[3]{};
                    float camBase[3]{}, camApplied[3]{};
                    std::uint32_t camGeneration = 0;
                    const bool haveAim = width > 1.0f && height > 1.0f &&
                        GetMainSceneRenderedHalfTangents(halfTanX, halfTanY) &&
                        TryGetAimAnglesDegrees(aim) &&
                        ReadLatchedAngles(camBase, camApplied, &camGeneration) &&
                        halfTanX > 0.0001f && halfTanY > 0.0001f;
                    float yawOff = 0.0f, pitchOff = 0.0f, dx = 0.0f, dy = 0.0f;
                    if (haveAim) {
                        pitchOff = aim[0] - camApplied[0];
                        yawOff = aim[1] - camApplied[1];
                        while (yawOff > 180.0f) yawOff -= 360.0f;
                        while (yawOff < -180.0f) yawOff += 360.0f;
                        while (pitchOff > 180.0f) pitchOff -= 360.0f;
                        while (pitchOff < -180.0f) pitchOff += 360.0f;
                        constexpr float kDegToRad = 3.14159265358979f / 180.0f;
                        const float ndcX = -std::tan(yawOff * kDegToRad) / halfTanX;
                        const float ndcY = std::tan(pitchOff * kDegToRad) / halfTanY;
                        // Anchor in pass pixels, minus the pass centre: the shift
                        // that puts the anchor at the centre, in hud2d.shift's units.
                        dx = 0.5f * ndcX * width;
                        dy = 0.5f * ndcY * height;
                        matrix[0 * 4 + 3] -= m00 * dx;
                        matrix[1 * 4 + 3] -= m11 * dy;
                        std::memcpy(patched + kCameraRelativeToClipOffset, matrix, sizeof(matrix));
                        g_hud2dHeadAnchorApplied.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        g_hud2dHeadAnchorNoAim.fetch_add(1, std::memory_order_relaxed);
                    }
                    // Once a second, from the pass itself: the offset, the shift,
                    // and the raw translation terms the engine uploaded (m03, m13),
                    // which say whether the engine's anchor is in the matrix at
                    // all -- it is expected NOT to be.
                    static std::uint64_t lastTick = 0;
                    const std::uint64_t now = GetTickCount64();
                    if (now - lastTick >= 1000) {
                        lastTick = now;
                        float raw[16]{};
                        std::memcpy(raw, bytes + kCameraRelativeToClipOffset, sizeof(raw));
                        char line[400]{};
                        std::snprintf(line, sizeof(line),
                            "[TF2VR] HEAD ANCHOR (pixel-ortho %.0fx%.0f): aim-minus-applied yaw %+.2f pitch "
                            "%+.2f deg -> shift %+.0f,%+.0f px | raw m03 %+.4f m13 %+.4f | applied=%llu "
                            "no-aim=%llu\n",
                            width, height, yawOff, pitchOff, dx, dy,
                            raw[0 * 4 + 3], raw[1 * 4 + 3],
                            g_hud2dHeadAnchorApplied.load(std::memory_order_relaxed),
                            g_hud2dHeadAnchorNoAim.load(std::memory_order_relaxed));
                        Tf2VrLog(line);
                    }
                }
                if (hud2dAdjust) {
                    // Same two operations as the perspective HUD, but this pass
                    // is orthographic in pixel coordinates, so the shift is in
                    // PIXELS of the render (4032x2268) and the zoom scales
                    // about whatever point the pass calls its origin. The pair
                    // is dialled together for that reason: zoom first, then
                    // shift it back to where it should sit.
                    if (std::fabs(hud2dX) > 0.0001f || std::fabs(hud2dY) > 0.0001f) {
                        float matrix[16]{};
                        std::memcpy(matrix, patched + kCameraRelativeToClipOffset, sizeof(matrix));
                        // Orthographic rows map the pixel axes directly, so a
                        // shift is a straight adjustment of the two column-3
                        // terms scaled by the row length -- no basis projection,
                        // because there is no perspective divide to respect.
                        matrix[0 * 4 + 3] -= matrix[0 * 4 + 0] * hud2dX;
                        matrix[1 * 4 + 3] -= matrix[1 * 4 + 1] * hud2dY;
                        std::memcpy(patched + kCameraRelativeToClipOffset, matrix, sizeof(matrix));
                    }
                    // SCALE ABOUT THE AIM POINT, NOT ABOUT THE PASS ORIGIN.
                    //
                    // Scaling worked from the first attempt; it scaled about
                    // the WRONG CENTRE. The pass is orthographic in pixel
                    // coordinates with its origin at a screen corner, so
                    //
                    //     clip = M_lin*v + m03
                    //
                    // and scaling M_lin by s takes a reticle at pixel P to s*P
                    // -- which is the "shrinks and moves up and left, further
                    // each press" that was measured. Position was never
                    // inseparable from size; the centre of the scaling was
                    // simply a corner of the screen.
                    //
                    // Scaling about an arbitrary point P instead:
                    //
                    //     clip' = M_lin*(P + s*(v - P)) + m03
                    //           = s*M_lin*v  +  (1-s)*M_lin*P  +  m03
                    //
                    // so the linear part scales by s and the column-3 term
                    // gains (1-s)*M_lin*P. The ONLY new thing needed is P.
                    //
                    // And P is not a mystery: the reticle sits at the aim
                    // point, which is what H1 established when the aim write
                    // moved it. Its offset from screen centre is the angular
                    // offset of aim from view, divided by the frustum's own
                    // half-tangents -- and both of those are already measured
                    // elsewhere in this plugin.
                    if (std::fabs(hud2dZoom - 1.0f) > 0.0001f) {
                        float matrix[16]{};
                        std::memcpy(matrix, patched + kCameraRelativeToClipOffset, sizeof(matrix));
                        const float m00 = matrix[0 * 4 + 0];
                        const float m11 = matrix[1 * 4 + 1];
                        // Pixel extents come from the row lengths: |r0| = 2/W.
                        const float width = (m00 != 0.0f) ? std::fabs(2.0f / m00) : 0.0f;
                        const float height = (m11 != 0.0f) ? std::fabs(2.0f / m11) : 0.0f;
                        // Screen centre is the fallback, and it is the right
                        // one: with the gun aligned to the view the reticle IS
                        // centred, so a missing aim offset degrades to exactly
                        // the case where the centre is correct anyway.
                        float px = width * 0.5f;
                        float py = height * 0.5f;
                        float halfTanX = 0.0f, halfTanY = 0.0f;
                        float aim[3]{};
                        float camBase[3]{}, camApplied[3]{};
                        std::uint32_t camGeneration = 0;
                        // AGAINST THE RENDER CAMERA, NOT THE COMMAND'S VIEW.
                        //
                        // The first version subtracted the command's
                        // worldViewAngles, and it put the anchor 12 degrees
                        // low: those are the BODY's angles -- their pitch is
                        // flat 0.00 in every log this project has -- while the
                        // reticle is projected through the camera the frame is
                        // actually rendered with, which head tracking owns and
                        // which pitches with the wearer's neck.
                        //
                        // Task 01 measured exactly this: turning the head moves
                        // the reticle on screen while it stays on the body's
                        // aim in the world. That is only true if the projection
                        // uses the head's camera, so that is the reference.
                        const bool haveAim =
                            // RENDERED, not declared: the reticle is positioned INSIDE
                            // the image, so it must use the frustum the image was
                            // drawn with. Using the declared one put it at 1/mag of
                            // where the round lands. See the note at the accessor.
                            GetMainSceneRenderedHalfTangents(halfTanX, halfTanY) &&
                            TryGetAimAnglesDegrees(aim) &&
                            ReadLatchedAngles(camBase, camApplied, &camGeneration) &&
                            halfTanX > 0.0001f && halfTanY > 0.0001f;
                        float yawOff = 0.0f, pitchOff = 0.0f;
                        if (haveAim) {
                            pitchOff = aim[0] - camApplied[0];
                            yawOff = aim[1] - camApplied[1];
                            while (yawOff > 180.0f) yawOff -= 360.0f;
                            while (yawOff < -180.0f) yawOff += 360.0f;
                            while (pitchOff > 180.0f) pitchOff -= 360.0f;
                            while (pitchOff < -180.0f) pitchOff += 360.0f;
                        }
                        if (haveAim) {
                            constexpr float kDegToRad = 3.14159265358979f / 180.0f;
                            // Source yaw increases to the LEFT and pitch
                            // increases DOWNWARD, while pixel x runs right and
                            // pixel y runs down -- hence the yaw negation and
                            // the absence of one on pitch.
                            const float ndcX = -std::tan(yawOff * kDegToRad) / halfTanX;
                            const float ndcY = std::tan(pitchOff * kDegToRad) / halfTanY;
                            // OFF-SCREEN FALLS BACK TO THE CENTRE, rather than
                            // clamping to a big number.
                            //
                            // Past the frustum edge the tangent runs away, and
                            // a hard clamp still leaves the anchor a long way
                            // out -- which translates the WHOLE pass by
                            // (1-s)*anchor. That pass carries more than the
                            // reticle: the pause menu is drawn in it too (the
                            // wearer noticed it shrinking, and liked it). So a
                            // gun pointing 70 degrees off would have shoved the
                            // menu off the screen, on exactly the frames where
                            // the reticle it was compensating for is not
                            // visible anyway.
                            //
                            // Beyond the frame there is nothing to hold in
                            // place, so hold the centre instead: the reticle is
                            // off-screen either way and everything else in the
                            // pass scales about the middle, which is what it
                            // wants. Measured: 32 of 38 samples were on-screen
                            // and the 6 that were not were all past 60 degrees.
                            constexpr float kVisible = 1.2f;
                            if (ndcX > -kVisible && ndcX < kVisible && ndcY > -kVisible &&
                                ndcY < kVisible) {
                                px = (0.5f + 0.5f * ndcX) * width;
                                py = (0.5f + 0.5f * ndcY) * height;
                            }
                        }
                        matrix[0 * 4 + 0] *= hud2dZoom;
                        matrix[0 * 4 + 1] *= hud2dZoom;
                        matrix[0 * 4 + 2] *= hud2dZoom;
                        matrix[1 * 4 + 0] *= hud2dZoom;
                        matrix[1 * 4 + 1] *= hud2dZoom;
                        matrix[1 * 4 + 2] *= hud2dZoom;
                        matrix[0 * 4 + 3] += (1.0f - hud2dZoom) * m00 * px;
                        matrix[1 * 4 + 3] += (1.0f - hud2dZoom) * m11 * py;
                        std::memcpy(patched + kCameraRelativeToClipOffset, matrix, sizeof(matrix));
                        g_reticleAnchorX = px;
                        g_reticleAnchorY = py;
                        g_reticleYawOffset = yawOff;
                        g_reticlePitchOffset = pitchOff;
                        g_hud2dPassWidth = width;
                        g_hud2dPassHeight = height;
                        g_reticleDivisorX = halfTanX;
                        g_reticleDivisorY = halfTanY;
                        g_reticleHadAim = haveAim ? 1 : 0;
                        g_reticleAnchorUpdates.fetch_add(1, std::memory_order_relaxed);
                    }
                    g_hud2dAdjustedCount.fetch_add(1, std::memory_order_relaxed);
                }
                if (matchMainSceneFov) {
                    // |r0| = 1/halfTanX, so scaling row0 by s gives
                    // halfTanX' = halfTanX / s, and the factor that lands the
                    // game's frustum exactly on the headset's is therefore
                    // gameHalfTan / headsetHalfTan. Measured here that is
                    // 1.865/1.428 = 1.31 across and 1.052/1.000 = 1.05 down:
                    // greater than one, i.e. a narrowing, which is what
                    // rendering 110 degrees instead of 123.7 has to be.
                    //
                    // The two rows scale independently, which is the whole
                    // reason this works -- a 1.77 aspect render has to become a
                    // 1.43 aspect one, and a single uniform zoom cannot do that.
                    float row0[3]{}, row1[3]{};
                    std::memcpy(row0, patched + kCameraRelativeToClipOffset, sizeof(row0));
                    std::memcpy(row1, patched + kCameraRelativeToClipOffset + 4 * sizeof(float), sizeof(row1));
                    const float length0 = std::sqrt(row0[0] * row0[0] + row0[1] * row0[1] + row0[2] * row0[2]);
                    const float length1 = std::sqrt(row1[0] * row1[0] + row1[1] * row1[1] + row1[2] * row1[2]);
                    if (length0 > 0.0001f && length1 > 0.0001f) {
                        ScaleProjectionRows(patched,
                                            (1.0f / headsetTanX) / length0,
                                            (1.0f / headsetTanY) / length1);
                    }
                }
                // xr.viewport_full, THE HALF THAT ACTUALLY CHANGES THE RENDER.
                //
                // The first attempt at this rewrote the RECORDED tangents and
                // nothing else, so the frustum we reported to the compositor
                // changed while the game went on projecting its original
                // vertical FOV into the now-taller viewport. The image was
                // stretched by 3648/2520 and then declared to span an angle it
                // did not have -- two errors compounding, and a plausible source
                // of the distortion the wearer reported as fisheye.
                //
                // The projection matrix is what has to change, by the same
                // row-scaling the headset match uses above: |r1| = 1/halfTanY,
                // so scaling row1 by (1/wantTanY)/length1 lands the vertical
                // half-angle exactly on the widened viewport's shape. Row0 is
                // left alone -- the horizontal is not what the widening changed.
                // CORRECT THE HORIZONTAL, NOT THE VERTICAL, and by a CONSTANT.
                //
                // The first version derived the vertical from the buffer aspect,
                // which at a 1.105 buffer opened it from 92.9 to 113 degrees.
                // That is a 22% wider vertical frustum, and a Source engine is
                // draw-call bound, so it pulled far more geometry through
                // culling and the game fell to 19 fps -- on a 5090, which is the
                // tell that it was never about pixels.
                //
                // The vertical belongs to the game and to cl_fovScale, which the
                // wearer sets. What actually needs correcting is that the
                // headset wants a frustum of aspect 1.428 while the buffer it
                // asks us to render into is 1.105 -- deliberately, because the
                // runtime accounts for lens distortion. The game assumes those
                // are the same number and derives horizontal from the buffer,
                // giving 95.8 degrees where the display shows 110.
                //
                // So: leave row1 alone and widen row0 until tanX = tanY * the
                // HEADSET's ratio. Crucially that is a constant multiple of
                // whatever the game is currently doing, so ADS still narrows
                // proportionally -- which is exactly what xr.match_headset_fov
                // got wrong by clamping to an absolute value on every upload.
                if (viewportFullFix && !matchMainSceneFov) {
                    float row0[3]{}, row1[3]{};
                    std::memcpy(row0, patched + kCameraRelativeToClipOffset, sizeof(row0));
                    std::memcpy(row1, patched + kCameraRelativeToClipOffset + 4 * sizeof(float), sizeof(row1));
                    const float length0 = std::sqrt(row0[0] * row0[0] + row0[1] * row0[1] + row0[2] * row0[2]);
                    const float length1 = std::sqrt(row1[0] * row1[0] + row1[1] * row1[1] + row1[2] * row1[2]);
                    const float headTanX = g_headsetHalfTanX.load(std::memory_order_acquire);
                    const float headTanY = g_headsetHalfTanY.load(std::memory_order_acquire);
                    if (length0 > 0.0001f && length1 > 0.0001f &&
                        headTanX > 0.0001f && headTanY > 0.0001f) {
                        const float haveTanY = 1.0f / length1;
                        const float wantTanX = haveTanY * (headTanX / headTanY);
                        ScaleProjectionRows(patched, (1.0f / wantTanX) / length0, 1.0f);
                    }
                }
                // C1 -- ads.max_magnification, THE HALF THAT CHANGES THE RENDER.
                //
                // Last of the three, so it composes with whatever ran above
                // rather than being composed with. It scales BOTH rows by the
                // same reciprocal, which leaves every ratio the earlier patches
                // established -- viewportFullFix's horizontal relationship
                // included -- exactly where they were.
                //
                // The factor is computed from the VERTICAL, and that is not a
                // convenience. viewportFullFix rewrites row0 and leaves row1
                // alone, so by this point the horizontal in `patched` is ours
                // and the vertical is still the game's. Dividing by a value we
                // wrote would measure our own patch. The read site divides by
                // the same untouched vertical, which is what makes the two
                // agree.
                //
                // NOT GATED ON A ZOOM PREDICATE, deliberately. WorldIsZoomed()
                // is an edge detector with its own latch and its reference is
                // the all-time maximum that one transient can poison for a
                // whole session. The magnification ratio IS the predicate:
                // 1.0 at rest, above the cap only when the game has actually
                // narrowed. One quantity, no second classifier to disagree with
                // the first.
                // THE ADS PATCH THAT USED TO SIT HERE IS GONE, and its removal
                // is the fix rather than a cleanup.
                //
                // It widened the projection rows to SUPPRESS the ADS narrowing,
                // which is the design PLAN-ADS section 3 specified and the
                // wearer rejected on sight: "Zoom is not working at all. I do
                // see a black window boxing but the actual landscape is not
                // zooming at all." The narrowing is wanted. What was missing was
                // that we then DECLARED the narrowed angle to the projection
                // layer, so the compositor showed a 68.7-degree image across
                // 68.7 degrees and world scale was preserved exactly -- no
                // magnification, and black where the layer did not reach.
                //
                // ads.max_magnification survives as a cap on what is DECLARED
                // (GetMainSceneHalfTangents). Nothing patches the projection for
                // ADS any more, so the render is the game's own and the two can
                // no longer disagree about it.
                // THE FALSIFIER FOR THE WHOLE GEOMETRY PATH: what the game was
                // HANDED, against what the compositor is TOLD.
                //
                // Read back out of `patched` AFTER every row scale, never from
                // the wantTanX above. An instrument that reports its own input
                // is worse than none -- the viewport hook already cost this
                // project a run by logging the REQUESTED rectangle while the
                // game was handed a different one.
                //
                // Ratio X of 1.000 means the layer FOV describes the image.
                // Anything else is a horizontal-only reprojection error, which
                // shows on YAW and not on pitch, and is independent of depth.
                if (mainSceneUpload) {
                    float hRow0[4]{}, hRow1[4]{};
                    std::memcpy(hRow0, patched + kCameraRelativeToClipOffset, sizeof(float) * 3);
                    std::memcpy(hRow1, patched + kCameraRelativeToClipOffset + 4 * sizeof(float),
                                sizeof(float) * 3);
                    const float hLen0 = std::sqrt(hRow0[0] * hRow0[0] + hRow0[1] * hRow0[1] + hRow0[2] * hRow0[2]);
                    const float hLen1 = std::sqrt(hRow1[0] * hRow1[0] + hRow1[1] * hRow1[1] + hRow1[2] * hRow1[2]);
                    if (hLen0 > 0.0001f && hLen1 > 0.0001f) {
                        const float renderedTanX = 1.0f / hLen0;
                        const float renderedTanY = 1.0f / hLen1;
                        const float ratioX = renderedTanX > 0.0001f
                            ? g_mainSceneHalfTanX.load(std::memory_order_acquire) / renderedTanX : 0.0f;
                        // Bounded by construction: on change, or once every 300
                        // main-scene uploads. A per-upload line here is a file
                        // write per draw and would cost more than it measures.
                        const bool changed = std::fabs(renderedTanX - g_lastHandedTanX) > 0.002f * renderedTanX ||
                                             std::fabs(ratioX - g_lastHandedRatioX) > 0.002f;
                        if (!g_frustumHandedReported || changed || ++g_handedReportCountdown >= 300) {
                            g_frustumHandedReported = true;
                            g_handedReportCountdown = 0;
                            g_lastHandedTanX = renderedTanX;
                            g_lastHandedRatioX = ratioX;
                            const float declaredX = g_mainSceneHalfTanX.load(std::memory_order_acquire);
                            const float declaredY = g_mainSceneHalfTanY.load(std::memory_order_acquire);
                            // THE TWO RATIOS, TOGETHER, WHICH IS THE ONLY WAY
                            // EITHER MEANS ANYTHING.
                            //
                            // The frustum's tangent ratio and the rectangle it
                            // is rasterised into. For the LAYER these may differ --
                            // NDC maps to the rect and the declared fov undoes
                            // it, which is why the compositor sees correct
                            // geometry either way, and why chasing this through
                            // the layer found nothing.
                            //
                            // For the GAME they may not. Every screen-space
                            // pass -- temporal AA and its motion vectors, blur,
                            // occlusion, any kernel measured in pixels -- is
                            // built assuming the frustum matches the rectangle.
                            // Render a 1.600 frustum into a 1.105 rectangle and
                            // all of it goes anisotropic by 1.448, and temporal
                            // reprojection error shows up exactly when the
                            // camera MOVES. Which is on yaw.
                            const float vpW = g_lastViewportW.load(std::memory_order_acquire);
                            const float vpH = g_lastViewportH.load(std::memory_order_acquire);
                            const float vpAspect = vpH > 0.5f ? vpW / vpH : 0.0f;
                            const float tanRatio = renderedTanY > 0.0001f ? renderedTanX / renderedTanY : 0.0f;
                            char line[820]{};
                            std::snprintf(line, sizeof(line),
                                "[TF2VR] FRUSTUM HANDED vs DECLARED: handed %.2f x %.2f deg "
                                "(tan %.5f x %.5f) | declared to the compositor %.2f x %.2f "
                                "(tan %.5f x %.5f) | declared/handed X=%.4f Y=%.4f | "
                                "declare_rendered_fov=%d viewport_full=%d match_headset_fov=%d "
                                "fit_horizontal=%d | RASTER: frustum tangent ratio %.4f into a "
                                "%.0fx%.0f viewport of aspect %.4f, anisotropy %.4f. X must read "
                                "1.0000 -- that is the LAYER, and it is measured clean. ANISOTROPY "
                                "must read 1.0000 too, and that is the GAME: every screen-space "
                                "pass it runs, temporal AA and its motion vectors first among them, "
                                "assumes the frustum matches the rectangle. Anisotropy shows up "
                                "when the camera MOVES, which is on yaw. G0b: the aspect-matched "
                                "buffer for this runtime and this game is %.0fx%u -- the runtime's "
                                "recommended per-eye HEIGHT, times the game's own world-viewport "
                                "aspect for the width. At that size the game fills the target with "
                                "no letterbox, nothing needs widening, and every fixed-aspect "
                                "sub-resolution pass lines up.\n",
                                2.0f * std::atan(renderedTanX) * 57.2957795f,
                                2.0f * std::atan(renderedTanY) * 57.2957795f,
                                renderedTanX, renderedTanY,
                                2.0f * std::atan(declaredX) * 57.2957795f,
                                2.0f * std::atan(declaredY) * 57.2957795f,
                                declaredX, declaredY,
                                ratioX,
                                renderedTanY > 0.0001f ? declaredY / renderedTanY : 0.0f,
                                g_declareRenderedFov.load(std::memory_order_acquire) ? 1 : 0,
                                g_viewportFull.load(std::memory_order_acquire) ? 1 : 0,
                                g_matchHeadsetFov.load(std::memory_order_acquire) ? 1 : 0,
                                g_fitHorizontal.load(std::memory_order_acquire) ? 1 : 0,
                                tanRatio, vpW, vpH, vpAspect,
                                vpAspect > 0.0001f ? tanRatio / vpAspect : 0.0f,
                                tanRatio * g_recommendedEyeHeight.load(std::memory_order_acquire),
                                g_recommendedEyeHeight.load(std::memory_order_acquire));
                            Tf2VrLog(line);
                        }
                    }
                }
                if (matchViewmodelFov) {
                    // |r0| = 1/halfTanX, so the factor that takes this pass's
                    // frustum to the world's is vmHalfTan / worldHalfTan, which
                    // is worldRowLength / vmRowLength.
                    float row0[3]{}, row1[3]{};
                    std::memcpy(row0, patched + kCameraRelativeToClipOffset, sizeof(row0));
                    std::memcpy(row1, patched + kCameraRelativeToClipOffset + 4 * sizeof(float), sizeof(row1));
                    const float length0 = std::sqrt(row0[0] * row0[0] + row0[1] * row0[1] + row0[2] * row0[2]);
                    const float length1 = std::sqrt(row1[0] * row1[0] + row1[1] * row1[1] + row1[2] * row1[2]);
                    if (length0 > 0.0001f && length1 > 0.0001f) {
                        // HOW BIG THE GUN IS RENDERED, in one line.
                        //
                        // The weapon has its own narrow frustum -- measured at
                        // 86.1 x 55.4 degrees against the world's 123.7 x 92.9 --
                        // and a narrow frustum magnifies. Matching it to the
                        // world's WIDER one therefore makes the gun SMALLER, and
                        // not matching it leaves the gun large.
                        //
                        // Which pass this fires on is the part that moved
                        // recently: the family now includes the gun's own
                        // origin-(0,0,0) pass, which the |origin|>1 world test
                        // used to exclude. So this logs the native frustum, the
                        // target, and the scale, on change -- because "the gun
                        // got bigger" cannot be attributed while every input to
                        // its size is a silent default.
                        // THE SIZE IS A CHOICE, NOT A DERIVATION.
                        //
                        // Matching the weapon to the world's frustum is a
                        // heuristic and nothing more. The viewmodel is authored
                        // to look right on a flat screen at its own 86.1
                        // degrees, and no arrangement of the world's frustum
                        // makes that authored size correct in a headset, where
                        // the gun subtends a real angle at a real distance.
                        //
                        // It is also not even a stable heuristic: the world's
                        // frustum narrows on ADS, so matching to it ENLARGES
                        // the gun exactly when the sights matter. Measured at
                        // rest 86.1 -> 123.7 gives scale 0.499, and mid-zoom
                        // 86.1 -> 68.7 gives 1.366 -- the gun nearly triples
                        // across a zoom.
                        //
                        // So the match sets the baseline and weapon.size scales
                        // it, adjustable live because the right value is a
                        // judgement made wearing the headset and cannot be
                        // derived here. Larger size narrows the target frustum,
                        // which magnifies the gun.
                        // THE THIRD CANDIDATE SIGNAL: THE WEAPON MOVES.
                        //
                        // The first two are now both measured and both dead for
                        // a pistol. The world's frustum does not change at all
                        // -- the one zoom in the last session ran 123.7 to 69.2
                        // degrees and was a rifle -- and the weapon's frustum
                        // reads 1.000 of resting in both states, so the earlier
                        // "native 37.7 to 86.1" range was four passes with four
                        // fixed FOVs interleaved by a broken instrument, not a
                        // zoom.
                        //
                        // What visibly happens when a pistol is aimed is that
                        // the gun MOVES to the centre. That is a translation,
                        // and it lives in column 3 of the same matrix.
                        //
                        // DIAGNOSTIC ONLY. THIS MUST NEVER BECOME THE TRIGGER.
                        //
                        // Motion-controlled weapons are coming, and then the
                        // player moves the gun wherever they like -- including
                        // to the centre, constantly, without aiming. A detector
                        // built on weapon position would fire continuously the
                        // day controllers land, and would do it by working
                        // exactly as designed, which is the worst kind of
                        // failure to diagnose later.
                        //
                        // It is logged because it says what the game does when
                        // a pistol is aimed, and that is worth knowing while
                        // looking for the real signal. The real signal has to be
                        // the game's own zoom STATE -- something that means "the
                        // player is aiming" rather than something that correlates
                        // with it in today's rendering. See the note in
                        // HEADSET-ISSUES.
                        {
                            float translation[3]{};
                            translation[0] = *reinterpret_cast<const float*>(
                                patched + kCameraRelativeToClipOffset + 3 * sizeof(float));
                            translation[1] = *reinterpret_cast<const float*>(
                                patched + kCameraRelativeToClipOffset + 7 * sizeof(float));
                            translation[2] = *reinterpret_cast<const float*>(
                                patched + kCameraRelativeToClipOffset + 11 * sizeof(float));
                            const float nativeDeg = 2.0f * std::atan(1.0f / length0) * 57.2957795f;
                            static float lastX[8]{};
                            static float lastFov[8]{};
                            static unsigned logged = 0;
                            for (int slot = 0; slot < 8; ++slot) {
                                if (lastFov[slot] != 0.0f &&
                                    std::fabs(lastFov[slot] - nativeDeg) > 0.5f) continue;
                                if (lastFov[slot] == 0.0f) lastFov[slot] = nativeDeg;
                                if (std::fabs(translation[0] - lastX[slot]) > 0.02f && logged < 200) {
                                    ++logged;
                                    lastX[slot] = translation[0];
                                    char line[260]{};
                                    std::snprintf(line, sizeof(line),
                                        "[TF2VR] viewmodel pass %.1f deg moved: translation "
                                        "(%.3f %.3f %.3f). A pistol aim shows here if anywhere.\n",
                                        nativeDeg, translation[0], translation[1], translation[2]);
                                    Tf2VrLog(line);
                                }
                                break;
                            }
                        }
                        // The weapon's own frustum, as a candidate zoom signal.
                        // Widest seen is resting, same trick as the world's.
                        const float weaponTanX = 1.0f / length0;
                        g_weaponTanX.store(weaponTanX, std::memory_order_release);
                        if (weaponTanX > g_restingWeaponTanX.load(std::memory_order_acquire)) {
                            g_restingWeaponTanX.store(weaponTanX, std::memory_order_release);
                        }
                        const float size = g_weaponSize.load(std::memory_order_relaxed);
                        const float targetTanX = worldTanX / size;
                        const float targetTanY = worldTanY / size;
                        ScaleProjectionRows(patched, (1.0f / targetTanX) / length0,
                                            (1.0f / targetTanY) / length1);
                    }
                }
                // Rotation next, then any eye translation, so the translation
                // is taken from the matrix that will actually be used.
                if (compensate) ApplyViewmodelCompensation(patched);
                // THE HUD'S ANCHOR. Its pass is excluded from the weapon
                // correction above -- that exclusion is the defect-A fix -- so
                // leaving it alone renders it in the camera's own frame, which
                // is HEAD-LOCKED: the panels rotate rigidly with the headset.
                // Anchoring it to the aim instead is the same algebra with the
                // aim basis in place of the body's, because rel = X^T * written
                // places the content in frame X.
                if (hudAnchorPass) ApplyHudAimAnchor(patched);
                // THE HUD PASS'S FRUSTUM, refitted to the world's in the SAME
                // frame. See the note beside g_hudFovMode for the measurement.
                if (hudFovMatchPass) {
                    if (worldTanX > 0.001f && worldTanY > 0.001f) {
                        float hudMatrix[16]{};
                        std::memcpy(hudMatrix, patched + kCameraRelativeToClipOffset, sizeof(hudMatrix));
                        const float hudLen0 = std::sqrt(hudMatrix[0] * hudMatrix[0] +
                                                        hudMatrix[1] * hudMatrix[1] +
                                                        hudMatrix[2] * hudMatrix[2]);
                        const float hudLen1 = std::sqrt(hudMatrix[4] * hudMatrix[4] +
                                                        hudMatrix[5] * hudMatrix[5] +
                                                        hudMatrix[6] * hudMatrix[6]);
                        // THE ORIGINAL UPLOAD, always -- not just when logging.
                        // The census says this pass is uploaded at 0.75002 x
                        // 0.52516 and it reached this line at 1.42144 x 0.99528,
                        // so something ABOVE us widens it by 1.8952 in both axes.
                        // The marker error deduced from the wearer's own
                        // discriminators was 1.89x. Mode 1 puts the pass back to
                        // what the game uploaded; mode 2 forces it to the world's.
                        float rawMatrix[16]{};
                        std::memcpy(rawMatrix, bytes + kCameraRelativeToClipOffset, sizeof(rawMatrix));
                        const float rawLen0 = std::sqrt(rawMatrix[0] * rawMatrix[0] +
                                                        rawMatrix[1] * rawMatrix[1] +
                                                        rawMatrix[2] * rawMatrix[2]);
                        const float rawLen1 = std::sqrt(rawMatrix[4] * rawMatrix[4] +
                                                        rawMatrix[5] * rawMatrix[5] +
                                                        rawMatrix[6] * rawMatrix[6]);
                        const int hudFovMode = g_hudFovMode.load(std::memory_order_relaxed);
                        const float targetLen0 =
                            hudFovMode == 1 ? rawLen0 : (1.0f / worldTanX);
                        const float targetLen1 =
                            hudFovMode == 1 ? rawLen1 : (1.0f / worldTanY);
                        if (hudLen0 > 1e-6f && hudLen1 > 1e-6f && targetLen0 > 1e-6f &&
                            targetLen1 > 1e-6f) {
                            const float sx = targetLen0 / hudLen0;
                            const float sy = targetLen1 / hudLen1;
                            ScaleProjectionRows(patched, sx, sy);
                            // ONE COUNT PER FRAME, not per upload: the question
                            // is whether the frame got its correction at all.
                            const std::uint32_t frame = g_frameIndex.load(std::memory_order_relaxed);
                            std::uint32_t lastFrame = g_hudFovLastRefitFrame.load(std::memory_order_relaxed);
                            if (lastFrame != frame &&
                                g_hudFovLastRefitFrame.compare_exchange_strong(
                                    lastFrame, frame, std::memory_order_relaxed)) {
                                g_hudFovFramesRefitted.fetch_add(1, std::memory_order_relaxed);
                            }
                            // A refit that computes 1.0 is a refit that changed
                            // nothing, and would look exactly like a dropout.
                            // The range across the window separates the two.
                            float prev = g_hudFovSxMin.load(std::memory_order_relaxed);
                            while (sx < prev &&
                                   !g_hudFovSxMin.compare_exchange_weak(prev, sx,
                                                                        std::memory_order_relaxed)) {
                            }
                            prev = g_hudFovSxMax.load(std::memory_order_relaxed);
                            while (sx > prev &&
                                   !g_hudFovSxMax.compare_exchange_weak(prev, sx,
                                                                        std::memory_order_relaxed)) {
                            }
                            const auto seen = g_hudFovMatchApplied.fetch_add(1, std::memory_order_relaxed);
                            if (seen == 0) {
                                char hudLine[600]{};
                                std::snprintf(hudLine, sizeof(hudLine),
                                    "[TF2VR] HUD FOV mode %d applied: ORIGINAL upload half-tangents "
                                    "%.4f x %.4f | as it reached us after the passes above %.4f x %.4f "
                                    "| the world carries %.4f x %.4f | rows scaled by %.4f x %.4f. Mode "
                                    "1 restores what the game uploaded, mode 2 forces the world's. If "
                                    "the ORIGINAL is narrow and it reached us wide, something above us "
                                    "widened it and mode 1 is the undo.\n",
                                    hudFovMode, rawLen0 > 1e-6f ? 1.0f / rawLen0 : 0.0f,
                                    rawLen1 > 1e-6f ? 1.0f / rawLen1 : 0.0f,
                                    1.0f / hudLen0, 1.0f / hudLen1, worldTanX, worldTanY, sx, sy);
                                Tf2VrLog(hudLine);
                            }
                        }
                    } else {
                        g_hudFovMatchNoWorld.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                // AFTER the head-cancel, not before. The cancel puts the gun back
                // into the body's frame; the pin then rotates it out of that
                // frame by the hand's rotation. Composed the other way round the
                // hand's rotation would be expressed in a frame the cancel then
                // replaces, which is the same ordering error that made the lean
                // translation move the gun whenever the head turned.
                if (pinWeapon) ApplyWeaponPin(patched);
                // AFTER the rotation, for the reason stated three lines above:
                // a translation must be taken from the matrix that will
                // actually be used.
                //
                // Applied before the rotation, this moved the gun whenever the
                // head TURNED. ApplyViewmodelCompensation rewrites columns 0-2
                // and leaves column 3 alone, so a translation composed earlier
                // keeps a column-3 term built from a basis that no longer
                // exists -- and the mismatch is a function of the head rotation
                // itself, which is precisely the symptom.
                if (detachLean) {
                    float matrix[16]{};

                    g_detachLeanApplied.fetch_add(1, std::memory_order_relaxed);
                    std::memcpy(matrix, patched + kCameraRelativeToClipOffset, sizeof(matrix));
                    const float strength = g_detachLean.load(std::memory_order_relaxed);
                    const float e[3] = {BitsToFloatLocal(g_cameraOffsetXBits) * strength,
                                        BitsToFloatLocal(g_cameraOffsetYBits) * strength,
                                        BitsToFloatLocal(g_cameraOffsetZBits) * strength};
                    for (int row = 0; row < 4; ++row) {
                        const float* basis = matrix + row * 4;
                        matrix[row * 4 + 3] -= basis[0] * e[0] + basis[1] * e[1] + basis[2] * e[2];
                    }
                    std::memcpy(patched + kCameraRelativeToClipOffset, matrix, sizeof(matrix));
                }
                if (offsetSelected) {
                if (g_offsetModeKind == OffsetMode::CameraOrigin) {
                    float origin[3]{};
                    std::memcpy(origin, patched + kCameraOriginOffset, sizeof(origin));
                    origin[0] += kOriginOffsetX;
                    std::memcpy(patched + kCameraOriginOffset, origin, sizeof(origin));
                } else {
                    // The convention is clip = M * p with rows r0..r3 and the
                    // translation in column 3.  r3.xyz is the camera forward
                    // axis (it measures as unit length) so clip.w is view
                    // depth, and m[15] is 0 because rendering is camera-
                    // relative.  That is also why m[15] discriminates
                    // perspective from the orthographic/UI uploads, whose
                    // r3 is (0,0,0,1).
                    //
                    // Moving the eye by e substitutes p' = p - e, so each row's
                    // column-3 term loses that row's projection of e.
                    //
                    // A previous attempt subtracted into m[12..15] instead,
                    // which corrupted the forward/w row: no translation, and a
                    // blown-out white centre from degenerate w.  Do not
                    // reintroduce that form.
                    float matrix[16]{};
                    std::memcpy(matrix, patched + kCameraRelativeToClipOffset, sizeof(matrix));
                    float eye[3] = {kOriginOffsetX, 0.0f, 0.0f};
                    if (g_offsetModeKind == OffsetMode::PerEye) {
                        float thisOrigin[3]{};
                        std::memcpy(thisOrigin, bytes + kCameraOriginOffset, sizeof(thisOrigin));
                        TallyOrigin(thisOrigin);
                        // Skip anything that is not the dominant player camera:
                        // the sky camera renders in its own reduced-scale space
                        // and would get ~16x too much disparity.  Verified good
                        // in headset; see the plan doc for the scaled-disparity
                        // alternative, which is deliberately not applied.
                        // The weapon pass is exempt. It renders at the origin by
                        // construction, so an origin-distance test against the
                        // player camera rejects it every time -- which is
                        // precisely how it ended up with no disparity while the
                        // rest of its own family had some.
                        if (!weaponMonoPass && g_referenceValid.load(std::memory_order_acquire) &&
                            !OriginsClose(thisOrigin, g_referenceOrigin, kSkyboxOriginThreshold)) {
                            // patched, not sourceData: this bails out of the eye
                            // offset only, and must not discard a viewmodel
                            // correction already composed into the copy.
                    costScope.PauseForForward();
                            forward(context, destination, subresource, box, uploadSource, rowPitch, depthPitch);
                            g_inFlight.fetch_sub(1, std::memory_order_acq_rel);
                            return;
                        }
                        // r0.xyz points along camera right but carries the
                        // projection scale 1/(aspect*tan(fov/2)), so it must be
                        // normalised before being used as a world-space axis.
                        //
                        // Which r0: on a corrected pass `matrix` has already been
                        // rotated into the body's frame, so the eye separation
                        // follows the body's right axis rather than the head's.
                        // That is deliberate -- it gives the gun a constant,
                        // full-magnitude lateral disparity in its own pass
                        // whatever the head-vs-body yaw is -- but it was never
                        // measured, so the pre-rotation (head-aligned) basis is
                        // selectable for a direct A/B.
                        const float* basisSource = matrix;
                        float preRotation[16]{};
                        if (compensate && g_eyeBasisPreRotation.load(std::memory_order_relaxed)) {
                            std::memcpy(preRotation, bytes + kCameraRelativeToClipOffset, sizeof(preRotation));
                            basisSource = preRotation;
                        }
                        const float length = std::sqrt(basisSource[0] * basisSource[0] +
                                                       basisSource[1] * basisSource[1] +
                                                       basisSource[2] * basisSource[2]);
                        if (length <= 0.0f) {
                    costScope.PauseForForward();
                            forward(context, destination, subresource, box, uploadSource, rowPitch, depthPitch);
                            g_inFlight.fetch_sub(1, std::memory_order_acq_rel);
                            return;
                        }
                        // The weapon takes the DOMINANT eye's sign in every
                        // frame; the world takes the alternating one. That is
                        // the whole of "weapon mono, world stereo".
                        const int monoEye = g_weaponMonoEye.load(std::memory_order_acquire);
                        // 2 = centre: zero lateral offset, so the sight picture
                        // shares an axis with the round. 0/1 pin it to one eye,
                        // which is measurably wrong -- kept only so the two can
                        // be compared rather than argued about.
                        // SAME-FRAME STEREO: THE KEY F2 COULD NOT HAVE.
                        //
                        // The design keys the per-eye offset on "inside the
                        // second call", via a thread-local set by the SR-root
                        // detour. That cannot work: the scene draw SUBMITS, and
                        // these uploads happen later on the render thread -- a
                        // control measured 66,843 of them outside any tagged
                        // pass against zero inside one.
                        //
                        // The mid-frame boundary supplies the key instead. On
                        // THIS thread, the second scene-sized viewport of a
                        // doubled frame means batch 2 is drawing, so every
                        // camera upload after it belongs to eye 1. Same shipped
                        // lever, same 576-byte buffer; only the thing that
                        // decides the sign has moved to where the work is.
                        const bool sameFrameEye1 =
                            SameFrameDoubleArmed() && SameFrameInSecondPass();
                        const float eyeSign = weaponMonoPass
                            ? (monoEye == 2 ? 0.0f : (monoEye == 0 ? -1.0f : 1.0f))
                            : (sameFrameEye1 ? 1.0f
                                             : (SameFrameDoubleArmed()
                                                    ? -1.0f
                                                    : g_eyeSign.load(std::memory_order_acquire)));
                        const float scale = eyeSign *
                            g_halfInterpupillaryUnits.load(std::memory_order_relaxed) / length;
                        eye[0] = basisSource[0] * scale;
                        eye[1] = basisSource[1] * scale;
                        eye[2] = basisSource[2] * scale;
                        // Published for the per-frame STEREO line, which is the
                        // falsifier on every warp report: a judgement about
                        // disparity is void unless the log says which eye the
                        // frame was, and how far the camera was ACTUALLY moved.
                        // The magnitude is recomputed from the vector that was
                        // applied, not from the half-IPD it was asked to be.
                        if (!weaponMonoPass) {
                            g_lastAppliedEyeSign.store(eyeSign, std::memory_order_relaxed);
                            g_lastAppliedEyeUnits.store(
                                std::sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]),
                                std::memory_order_relaxed);
                            g_worldEyeOffsetsThisFrame.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    for (int row = 0; row < 4; ++row) {
                        const float* basis = matrix + row * 4;
                        matrix[row * 4 + 3] -= basis[0] * eye[0] + basis[1] * eye[1] + basis[2] * eye[2];
                    }
                    std::memcpy(patched + kCameraRelativeToClipOffset, matrix, sizeof(matrix));
                }
                }
                // W2 -- THE LENS SHEAR, AND IT GOES LAST ON PURPOSE.
                //
                // Every other projection edit above derives its factor from
                // |row0| or |row1| read back out of `patched`: matchMainSceneFov,
                // viewportFullFix and matchViewmodelFov all do. The shear
                // CHANGES |row1| -- that is what a translation of the frustum
                // does to the row's length -- so running it before any of them
                // would feed them a length that is part ours, and each would
                // then "correct" a frustum the game never asked for. Last means
                // every one of them still measures the game's own rows, and this
                // composes on top of whatever they decided.
                //
                //   row1' = row1 + o * row3
                //
                // row3 is the clip-w row, which is depth along forward, so this
                // adds a constant o to NDC y at every depth: a rigid translation
                // of the image inside the frustum, with the span untouched. Near
                // and far are not involved and are not written.
                if (lensShear) {
                    float matrix[16]{};
                    std::memcpy(matrix, patched + kCameraRelativeToClipOffset, sizeof(matrix));
                    for (int i = 0; i < 4; ++i) {
                        matrix[1 * 4 + i] += lensShearNdcY * matrix[3 * 4 + i];
                    }
                    std::memcpy(patched + kCameraRelativeToClipOffset, matrix, sizeof(matrix));
                    g_lensShearFamilyCount[LensShearFamilyBucket(bytes)].fetch_add(
                        1, std::memory_order_relaxed);
                    // PUBLISHED FROM THE MAIN SCENE ONLY, because the main
                    // scene's frustum is the one the projection layer declares.
                    // Publishing from whichever family happened to upload last
                    // would hand the layer an offset belonging to the gun.
                    if (mainSceneUpload) {
                        const float previous =
                            g_lensShearAppliedNdcY.exchange(lensShearNdcY, std::memory_order_acq_rel);
                        // PER-FAMILY VERIFICATION, AS A NUMBER IN THE LOG.
                        //
                        // Bounded by construction: on change only, so a steady
                        // frame writes nothing and an ADS ramp writes two lines.
                        // The four counts are the acceptance criterion -- a
                        // world-effects or viewmodel bucket sitting at 0 while
                        // the main scene climbs is the "reached one family and
                        // not the others" defect, and it names itself here
                        // instead of being reported as things not lining up.
                        if (std::fabs(lensShearNdcY - previous) > 0.0005f) {
                            const float halfTanY = g_mainSceneHalfTanY.load(std::memory_order_acquire);
                            const float up = g_lensTanUp.load(std::memory_order_acquire);
                            const float down = g_lensTanDown.load(std::memory_order_acquire);
                            char line[520]{};
                            std::snprintf(line, sizeof(line),
                                "[TF2VR] LENS SHEAR %.5f NDC (was %.5f). Lens tan [%.5f, %.5f], centre %.5f "
                                "= %.2f deg off forward; render was [%.5f, %.5f], now [%.5f, %.5f] -- SAME "
                                "SPAN %.5f, so no extra frustum. Uploads sheared: mainScene=%u effects=%u "
                                "viewmodel=%u other=%u. ALL FOUR MUST CLIMB TOGETHER: a family left behind "
                                "renders the world at one angle and what is drawn over it at another, which "
                                "is what fit_horizontal was turned off for.\n",
                                lensShearNdcY, previous, down, up, 0.5f * (up + down),
                                std::atan(0.5f * (up + down)) * 57.2957795f,
                                -halfTanY, halfTanY,
                                (-1.0f - lensShearNdcY) * halfTanY, (1.0f - lensShearNdcY) * halfTanY,
                                2.0f * halfTanY,
                                g_lensShearFamilyCount[kShearFamilyMainScene].load(std::memory_order_relaxed),
                                g_lensShearFamilyCount[kShearFamilyEffects].load(std::memory_order_relaxed),
                                g_lensShearFamilyCount[kShearFamilyViewmodel].load(std::memory_order_relaxed),
                                g_lensShearFamilyCount[kShearFamilyOther].load(std::memory_order_relaxed));
                            Tf2VrLog(line);
                        }
                    }
                }
                // ---- M3: THE EYE TRANSLATION FOR BATCH 2 ----
                //
                // LAST, so it composes on top of whatever else this build is
                // doing to the upload rather than being overwritten by it, and
                // so the origin the witness records is the one that goes to the
                // GPU.
                // THE TRANSLATION GOES INTO THE MATRIX, and the first build put
                // it only in c_cameraOrigin -- which diverged the BYTES and
                // moved nothing.
                //
                // The run proved it both ways at once: the origin census showed
                // batch 2 uploading a value batch 1 never did, ~12.85 units
                // along a purely horizontal axis, present only when armed and
                // byte-identical when not -- and the rendered pair measured
                // ZERO disparity in every depth band, with the armed pair's SAD
                // matching the control pair's to two decimals.
                //
                // This file's own header says which form an IPD needs, in the
                // sentence describing BeginMatrixEyeTranslationTest: "composes
                // a true eye translation into c_cameraRelativeToClip:
                // row3 -= e.x*row0 + e.y*row1 + e.z*row2. This is the form a
                // per-eye IPD offset ultimately needs." The world renders
                // CAMERA-RELATIVE through that matrix; c_cameraOrigin feeds
                // view-dependent shading, not the view transform. The shipped
                // per-eye stereo takes the matrix branch for exactly this
                // reason, and I took the branch beside it.
                //
                // Both fields are written now, from one `e`: the matrix is what
                // moves the eye, and the origin keeps specular and fog
                // consistent with the eye that moved -- and keeps the origin
                // census able to see the divergence at all.
                if (batch2Offset) {
                    float matrix[16]{};
                    std::memcpy(matrix, patched + kCameraRelativeToClipOffset, sizeof(matrix));
                    const float length = std::sqrt(matrix[0] * matrix[0] + matrix[1] * matrix[1] +
                                                   matrix[2] * matrix[2]);
                    // THE GUARD USED TO BE `length > 0.9 && length < 1.1`, AND
                    // IT REJECTED THE MAIN WORLD CAMERA.
                    //
                    // I called that "only a perspective camera has a ~unit right
                    // axis" while quoting, four lines up, the shipped comment
                    // that says the opposite: "r0.xyz points along camera right
                    // but carries the projection scale 1/(aspect*tan(fov/2))".
                    // At 90 degrees on 16:9 that scale is 0.56, so the world
                    // camera failed a test built on the assumption it would be
                    // 1.0, and the only families that passed were narrow-FOV
                    // ones. The run showed it exactly: the VIEWMODEL moved (its
                    // camera was offset, and being camera-relative at arm's
                    // length a 12-unit move threw it out of frame) while the
                    // WORLD did not move at all -- dx=0, dy=0 in every band on
                    // an instrument that self-tests against a known shift.
                    //
                    // Identification is the FAMILY PREDICATE's job, and it now
                    // does it above: IsWorldPerspectiveCamera && not viewmodel,
                    // which is the pair `stepCorrect` uses. All this needs is a
                    // basis it can normalise, which is what the shipped per-eye
                    // path checks and nothing more.
                    if (length > 1e-4f) {
                        // NORMALISED, and the last run measured why: asking for
                        // 12.0 produced a 12.853-unit separation, because row0
                        // is the right axis TIMES the projection's horizontal
                        // scale (~1.071 here) and the 0.9-1.1 guard calls that
                        // "unit". Unnormalised, the IPD would be wrong by the
                        // FOV factor and would CHANGE WITH ADS ZOOM.
                        const float ipd = g_batch2Ipd.load(std::memory_order_relaxed);
                        const float e[3] = {matrix[0] / length * ipd,
                                            matrix[1] / length * ipd,
                                            matrix[2] / length * ipd};
                        // Moving the eye by e substitutes p' = p - e, so each
                        // row's column-3 term loses that row's projection of e.
                        // The same form this file already uses in three places;
                        // the m[12..15] variant is recorded there as corrupting
                        // the w row, and is deliberately not reintroduced.
                        for (int row = 0; row < 4; ++row) {
                            const float* basis = matrix + row * 4;
                            matrix[row * 4 + 3] -= basis[0] * e[0] + basis[1] * e[1] + basis[2] * e[2];
                        }
                        std::memcpy(patched + kCameraRelativeToClipOffset, matrix, sizeof(matrix));
                        float origin[3]{};
                        std::memcpy(origin, patched + kCameraOriginOffset, sizeof(origin));
                        for (int i = 0; i < 3; ++i) origin[i] += e[i];
                        std::memcpy(patched + kCameraOriginOffset, origin, sizeof(origin));
                        g_batch2OffsetsApplied.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        g_batch2OffsetsSkippedBasis.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                // THE WITNESS READS THE BUFFER THAT IS ABOUT TO BE UPLOADED,
                // and it reads it for BOTH batches. Batch 1 is batch 2's
                // positive control on every single frame: if the two lists
                // differ, the frame carried two cameras; if batch 1's list is
                // empty, this witness saw nothing and batch 2's list means
                // nothing either.
                if (cameraSized) {
                    const std::uint8_t* shipped =
                        (uploadSource == static_cast<const void*>(patched) ? patched : bytes);
                    float uploaded[3]{};
                    std::memcpy(uploaded, shipped + kCameraOriginOffset, sizeof(uploaded));
                    // Column 3 of c_cameraRelativeToClip: the VIEW translation,
                    // the thing that decides where the camera actually is.
                    float viewXlat[3]{};
                    std::memcpy(&viewXlat[0], shipped + kCameraRelativeToClipOffset + 3 * sizeof(float), sizeof(float));
                    std::memcpy(&viewXlat[1], shipped + kCameraRelativeToClipOffset + 7 * sizeof(float), sizeof(float));
                    std::memcpy(&viewXlat[2], shipped + kCameraRelativeToClipOffset + 11 * sizeof(float), sizeof(float));
                    TallyUploadedCamera(RtvCensusInBatch2() ? 2 : 1, uploaded, viewXlat);
                    if (IsAdsProbeEnabled() && AdsLockEngaged() && (mainSceneUpload || weaponMonoPass)) {
                        SightNdcProbe(shipped, mainSceneUpload ? 0 : 1);
                    }
                    // ORDERING, READ-ONLY. A camera upload that lands AFTER the
                    // draws that were supposed to read it is a patch on a
                    // buffer nobody looks at again -- and it would present as
                    // exactly what four runs have seen: the bytes diverge and
                    // the picture does not. This records the transition span
                    // that was open at the moment of the upload, so batch 2's
                    // world-camera uploads can be placed relative to batch 2's
                    // own scene span rather than assumed to precede it.
                    if (batch2WorldFamily) {
                        // THE SNAPSHOT M5's PRE-DRAW WRITE NEEDS: the buffer the
                        // engine uploads its world camera into, and the bytes it
                        // last put there. Taken from `bytes`, the engine's own
                        // source, so the copy is never one of our own patched
                        // ones -- writing a patched buffer back through the
                        // pre-draw write would compound the offset every frame.
                        g_worldCameraBuffer.store(destination, std::memory_order_release);
                        std::memcpy(g_worldCameraBytes, bytes, kCameraBufferBytes);
                        g_worldCameraValid.store(true, std::memory_order_release);
                        g_worldCameraSnapshots.fetch_add(1, std::memory_order_relaxed);
                        const int batch = RtvCensusInBatch2() ? 2 : 1;
                        const int span = RtvCensusCurrentSpanIndex();
                        int& n = g_worldUploadSpanCount[batch];
                        if (n < kMaxWorldUploadSpans) {
                            g_worldUploadSpans[batch][n] = span;
                            // AND WHICH BUFFER OBJECT IT WENT TO.
                            //
                            // Ordering is now ruled out: batch 2's world-camera
                            // uploads land at spans 88,88,88,89 against batch
                            // 1's 6,6,6,7 -- exactly 82 apart, both inside
                            // their own batch's scene span. The patch is
                            // applied, in the right place, at the right moment,
                            // and batch 2's draws do not reflect it.
                            //
                            // The remaining cheap explanation is that they are
                            // not reading the buffer we patched. If batch 1 and
                            // batch 2 upload to DIFFERENT constant-buffer
                            // objects and batch 2's draws still have batch 1's
                            // bound, every byte we write goes somewhere nothing
                            // samples. Same pointer on both kills that idea for
                            // the cost of one line.
                            g_worldUploadBuffers[batch][n] = destination;
                            g_worldUploadBinds[batch][n] = RtvCensusCbBindsInBatch();
                            ++n;
                        }
                    }
                }
                if (offsetSelected) g_offsetAppliedCount.fetch_add(1, std::memory_order_relaxed);
                costScope.PauseForForward();
                forward(context, destination, subresource, box, uploadSource, rowPitch, depthPitch);
                g_inFlight.fetch_sub(1, std::memory_order_acq_rel);
                return;
            }
        }
    }
                    costScope.PauseForForward();
    forward(context, destination, subresource, box, sourceData, rowPitch, depthPitch);
    g_inFlight.fetch_sub(1, std::memory_order_acq_rel);
}

// One stub per patched entry, each closing over its own trampoline. A single
// shared hook cannot work here: the two entries need two different originals,
// and there is no way to tell from inside which one was called.
void STDMETHODCALLTYPE HookUpdateSubresource(ID3D11DeviceContext* context, ID3D11Resource* destination,
                                             UINT subresource, const D3D11_BOX* box, const void* sourceData,
                                             UINT rowPitch, UINT depthPitch) {
    HookUpdateSubresourceBody(g_original, context, destination, subresource, box, sourceData, rowPitch, depthPitch);
}

void STDMETHODCALLTYPE HookUpdateSubresourceAlt(ID3D11DeviceContext* context, ID3D11Resource* destination,
                                                UINT subresource, const D3D11_BOX* box, const void* sourceData,
                                                UINT rowPitch, UINT depthPitch) {
    HookUpdateSubresourceBody(g_originalAlt, context, destination, subresource, box, sourceData, rowPitch, depthPitch);
}

// UpdateSubresource1 carries one extra argument, so it cannot share a stub with
// the 7-argument form. Rather than duplicate the whole decision-and-patch body
// -- which is verified and which I do not want two copies of -- the extra
// argument is carried across the call in thread-local storage and re-attached
// by this adapter. The body forwards exactly once, on this thread, so the pair
// is valid for the whole of it.
thread_local UpdateSubresource1Fn t_forward1 = nullptr;
thread_local UINT t_copyFlags = 0;

void STDMETHODCALLTYPE Forward1Adapter(ID3D11DeviceContext* context, ID3D11Resource* destination,
                                       UINT subresource, const D3D11_BOX* box, const void* sourceData,
                                       UINT rowPitch, UINT depthPitch) {
    if (t_forward1) t_forward1(context, destination, subresource, box, sourceData, rowPitch, depthPitch, t_copyFlags);
}

void STDMETHODCALLTYPE HookUpdateSubresource1(ID3D11DeviceContext* context, ID3D11Resource* destination,
                                              UINT subresource, const D3D11_BOX* box, const void* sourceData,
                                              UINT rowPitch, UINT depthPitch, UINT copyFlags) {
    t_forward1 = g_original1;
    t_copyFlags = copyFlags;
    HookUpdateSubresourceBody(&Forward1Adapter, context, destination, subresource, box, sourceData,
                              rowPitch, depthPitch);
}

void STDMETHODCALLTYPE HookUpdateSubresourceImpl(ID3D11DeviceContext* context, ID3D11Resource* destination,
                                                 UINT subresource, const D3D11_BOX* box, const void* sourceData,
                                                 UINT rowPitch, UINT depthPitch) {
    HookUpdateSubresourceBody(g_originalImpl, context, destination, subresource, box, sourceData, rowPitch, depthPitch);
}

// Walks forward from a thunk to the push-heavy implementation that follows it,
// staying inside the thunk's own module and inside a bounded window.
void* FindImplementationAfterThunk(void* thunk) {
    if (!thunk) return nullptr;
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(thunk), &module) || !module) {
        return nullptr;
    }
    auto* code = static_cast<std::uint8_t*>(thunk);
    for (size_t offset = 0x20; offset < 0x400; ++offset) {
        std::uint8_t* candidate = code + offset;
        HMODULE candidateModule = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(candidate), &candidateModule) ||
            candidateModule != module) {
            return nullptr;
        }
        // Only the push-heavy form is an implementation; the mov-to-stack forms
        // are the thunks we are trying to get past.
        if (std::memcmp(candidate, kExpectedPrologue, sizeof(kExpectedPrologue)) == 0) return candidate;
    }
    return nullptr;
}

void LogRecords() {
    if (g_offsetMode.load(std::memory_order_acquire)) {
        // Always reported, including zero: "never applied" and "applied but
        // invisible" are different findings and must not look alike.
        char offsetLine[512]{};
        std::snprintf(offsetLine, sizeof(offsetLine),
            "[TF2VR] END: offset test finished; camera-sized uploads seen=%u, shifted=%u; "
            "compensation applied=%u skipped(no head data)=%u lower-camera excluded=%u "
            "last base=(p%.2f y%.2f r%.2f) "
            "written=(p%.2f y%.2f r%.2f); hooks removed.\n",
            g_offsetCandidateCount.load(std::memory_order_acquire),
            g_offsetAppliedCount.load(std::memory_order_acquire),
            g_compensationAppliedCount.load(std::memory_order_acquire),
            g_compensationSkippedCount.load(std::memory_order_acquire),
            g_lowerCameraExcludedCount.load(std::memory_order_acquire),
            g_lastCompensationAngles[0], g_lastCompensationAngles[1], g_lastCompensationAngles[2],
            g_lastCompensationAngles[3], g_lastCompensationAngles[4], g_lastCompensationAngles[5]);
        Tf2VrLog(offsetLine);
        return;
    }
    const auto recorded = g_recordCount.load(std::memory_order_acquire);
    const auto stored = recorded < kMaxRecords ? recorded : kMaxRecords;
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] HOME: UpdateSubresource trace complete over %u frames: total=%u constant-buffer=%u camera-sized=%u recorded=%zu drawIndexed=%u.\n",
        kTraceFrames, g_totalCalls.load(std::memory_order_acquire),
        g_constantBufferCalls.load(std::memory_order_acquire),
        g_cameraSizedCalls.load(std::memory_order_acquire), stored,
        g_drawIndexedCount.load(std::memory_order_acquire));
    Tf2VrLog(line);
    for (size_t i = 0; i < stored; ++i) {
        const UpdateRecord& record = g_records[i];
        char entry[400]{};
        if (record.partial) {
            std::snprintf(entry, sizeof(entry),
                "[TF2VR] HOME: f%u #%u dst=%p PARTIAL box=[%u,%u) content=%016llX.\n",
                record.frame, record.ordinal, record.destination, record.boxLeft, record.boxRight,
                static_cast<unsigned long long>(record.contentHash));
        } else {
            std::snprintf(entry, sizeof(entry),
                "[TF2VR] HOME: f%u #%u dst=%p content=%016llX matrix=%016llX origin=(%.4f,%.4f,%.4f) row3=(%.4f,%.4f,%.4f,%.4f) |r0|=%.4f |r1|=%.4f row2=(%.5f,%.5f,%.5f,%.5f) drawsAt=%u burst=%u.\n",
                record.frame, record.ordinal, record.destination,
                static_cast<unsigned long long>(record.contentHash), static_cast<unsigned long long>(record.matrixHash),
                record.origin[0], record.origin[1], record.origin[2],
                record.matrixRow3[0], record.matrixRow3[1], record.matrixRow3[2], record.matrixRow3[3],
                record.row0Length, record.row1Length,
                record.matrixRow2[0], record.matrixRow2[1], record.matrixRow2[2], record.matrixRow2[3],
                record.drawsAtUpdate,
                // Draws issued between this upload and the next one.
                i + 1 < stored ? g_records[i + 1].drawsAtUpdate - record.drawsAtUpdate
                               : g_drawIndexedCount.load(std::memory_order_acquire) - record.drawsAtUpdate);
        }
        Tf2VrLog(entry);
    }
    // A trace taken in a menu looks superficially like a gameplay trace: the
    // menu renders a 3D scene, so there are world-positioned perspective
    // uploads and a plausible upload count. What is missing is the main scene
    // itself. Two traces were read as gameplay before this was noticed, so the
    // trace now classifies its own capture rather than leaving it to be
    // spotted by eye in a wall of hex.
    unsigned mainScene = 0, bodyWeapon = 0, otherPerspective = 0, uiOrtho = 0;
    for (size_t i = 0; i < stored; ++i) {
        const UpdateRecord& record = g_records[i];
        if (record.partial) continue;
        // matrixRow2 holds elements 8..11, so [3] is m[11], the near plane.
        if (record.row0Length == 1.0f && record.row1Length == 1.0f) { ++uiOrtho; continue; }
        const float nearPlane = record.matrixRow2[3];
        if (std::fabs(nearPlane - kMainSceneNearPlane) < kMainSceneNearTolerance) ++mainScene;
        else if (std::fabs(nearPlane + 1.0f) < 0.01f) ++bodyWeapon;
        else ++otherPerspective;
    }
    char verdict[560]{};
    std::snprintf(verdict, sizeof(verdict),
        "[TF2VR] capture classification: main-scene(near %.0f)=%u body/weapon(near -1)=%u "
        "other-perspective=%u UI/ortho=%u.%s\n",
        kMainSceneNearPlane, mainScene, bodyWeapon, otherPerspective, uiOrtho,
        mainScene == 0
            ? " No main-scene upload. Now that recording starts at a frame boundary this should not happen"
              " during gameplay; if it does, the world pass genuinely did not render (menu, loading screen,"
              " or spawn transition) rather than the capture having been truncated."
            : "");
    Tf2VrLog(verdict);
}

void FinishTraceIfQuiescent() {
    // Disarm before the quiescence check, never after: if teardown has to be
    // retried on a later frame because a call is still in flight, mutation
    // must already have stopped, and the retry path is gated on it being off.
    g_offsetArmed.store(false, std::memory_order_release);
    if (g_inFlight.load(std::memory_order_acquire) != 0) return;
    // Compensation is not a bounded test and owns the hook for as long as head
    // tracking is armed. Every caller of this reached it by way of a trace or
    // an offset test ending, which says nothing about whether compensation
    // still needs the hook.
    //
    // This is what made the correction work only after DEL: the Present path
    // installed the hook successfully, then AdvanceCameraUpdateTrace tore it
    // down on the very next plugin frame because no trace and no offset test
    // were active. DEL's install stuck only because it also sets g_offsetArmed.
    //
    // Nothing is logged here: this path is reached every plugin frame while
    // compensation is on, so logging would repeat forever. An offset test that
    // ends while head tracking is armed therefore prints no summary; those are
    // diagnostics normally run without head tracking.
    if (g_compensationEnabled.load(std::memory_order_acquire)) return;
    // AN EXPLICIT HOLD, for a caller that wants to MEASURE rather than mutate.
    //
    // The ownership list above is the whole reason a flat instrument could not
    // exist. Every entry is a MUTATION -- the compensation, a bounded offset
    // test, an upload trace -- so anything that only wants to READ the frustum
    // and viewport recorders had no way to say "I am still using these", and was
    // torn down on the next plugin frame. Measured three times: install, four
    // invocations, then `detour NOT INSTALLED` with every counter frozen.
    //
    // Note the viewport trace is NOT in the list either, which is why even the
    // 120-frame self-armed trace did not protect it.
    //
    // This is the same lesson the comment above records about DEL, arrived at
    // from the other side: the install stuck only because it also armed
    // something. A reader now has a way to hold the hook without arming
    // anything at all.
    if (g_detourHold.load(std::memory_order_acquire)) return;
    // A trace that is armed but has not reached its first frame boundary owns
    // the hook just as much as a running one.
    if (g_tracePendingStart.load(std::memory_order_acquire) ||
        g_traceFramesRemaining.load(std::memory_order_acquire)) return;
    RestoreDetour(g_detour);
    RestoreDetour(g_detourAlt);
    // Put the swapped slot back before anything else releases the context. The
    // slot is only restored if it still holds OUR hook: another component may
    // have swapped it after us, and writing our saved pointer over theirs would
    // unhook them rather than us.
    if (g_slot1) {
        DWORD oldProtect = 0;
        if (*g_slot1 == reinterpret_cast<void*>(&HookUpdateSubresource1) &&
            VirtualProtect(g_slot1, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            *g_slot1 = g_slot1Original;
            DWORD ignored = 0;
            VirtualProtect(g_slot1, sizeof(void*), oldProtect, &ignored);
        }
        g_slot1 = nullptr;
        g_slot1Original = nullptr;
    }
    RestoreDetour(g_detour1);
    RestoreDetour(g_detourImpl);
    RestoreDetour(g_drawDetour);
    // Put the census slots back before the context is released, for the same
    // reason the UpdateSubresource1 slot above is: a swapped slot outlives the
    // thing that swapped it.
    RemoveRtvCensus();
    if (g_gameContext) g_gameContext->Release();
    g_gameContext = nullptr;
    g_original = nullptr;
    g_originalAlt = nullptr;
    g_original1 = nullptr;
    g_originalImpl = nullptr;
    g_drawOriginal = nullptr;
    LogRecords();
}
}

namespace {
// Shared by both modes so the offset test cannot reach the game through a
// less-validated path than the passive trace already took.
// On a prologue mismatch, report which entry failed and what was actually
// there.  A single shared "something did not match" message previously made a
// failed install indistinguishable between the two hooked functions.
void LogPrologueMismatch(const char* name, const void* entry) {
    if (!entry) { char l[120]{}; std::snprintf(l, sizeof(l), "[TF2VR] HOME/END: %s entry is null.\n", name); Tf2VrLog(l); return; }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(entry, &info, sizeof(info)) != sizeof(info) ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        char l[160]{}; std::snprintf(l, sizeof(l), "[TF2VR] HOME/END: %s entry %p is not readable.\n", name, entry);
        Tf2VrLog(l); return;
    }
    std::uint8_t bytes[24]{};
    std::memcpy(bytes, entry, sizeof(bytes));
    char hex[24 * 3 + 1]{};
    size_t used = 0;
    for (size_t i = 0; i < sizeof(bytes) && used + 4 < sizeof(hex); ++i)
        used += static_cast<size_t>(std::snprintf(hex + used, sizeof(hex) - used, "%02X ", bytes[i]));
    char line[400]{};
    std::snprintf(line, sizeof(line), "[TF2VR] HOME/END: %s prologue mismatch at %p; bytes=%s\n", name, entry, hex);
    Tf2VrLog(line);
}

// requireDrawCorrelation is false for the offset test: DrawIndexed counting is
// only used to attribute draw bursts during the passive trace, so a mismatch
// there must not block a mutation that does not depend on it.
bool InstallDetours(ID3D11Device* device, bool requireDrawCorrelation) {
    if (!device || g_gameContext || g_framesRemaining.load(std::memory_order_acquire)) {
        Tf2VrLog("[TF2VR] HOME/END: UpdateSubresource hook unavailable/already active.\n"); return false;
    }
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context) { Tf2VrLog("[TF2VR] HOME/END: no immediate context.\n"); return false; }
    auto** vtable = *reinterpret_cast<void***>(context.Get());
    if (!vtable) { Tf2VrLog("[TF2VR] HOME/END: null context vtable.\n"); return false; }
    // Identify the object being hooked, not just the function. The Present-path
    // install fails on the same address every attempt while the DEL path
    // succeeds, which means the two call sites resolve different things; only
    // the context and vtable pointers can say which.
    {
        char line[224]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] InstallDetours: device=%p context=%p vtable=%p slot[%u]=%p\n",
            static_cast<void*>(device), static_cast<void*>(context.Get()),
            static_cast<void*>(vtable), static_cast<unsigned>(kUpdateSubresourceVtableIndex),
            vtable[kUpdateSubresourceVtableIndex]);
        Tf2VrLog(line);
    }
    if (!PrepareDetour(g_detour, vtable[kUpdateSubresourceVtableIndex])) {
        LogPrologueMismatch("UpdateSubresource", vtable[kUpdateSubresourceVtableIndex]);
        RestoreDetour(g_detour);
        return false;
    }
    {
        char line[160]{};
        std::snprintf(line, sizeof(line), "[TF2VR] UpdateSubresource prologue variant %s accepted.\n",
                      PrologueVariantName(static_cast<const std::uint8_t*>(vtable[kUpdateSubresourceVtableIndex])));
        Tf2VrLog(line);
    }
    // If slot 48 gave us a thunk, hook the implementation behind it as well.
    // That is the entry the engine actually reaches, and the only one that sees
    // the whole upload stream.
    {
        auto* slotEntry = static_cast<std::uint8_t*>(vtable[kUpdateSubresourceVtableIndex]);
        const bool slotIsThunk = slotEntry &&
            std::memcmp(slotEntry, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0;
        if (!g_hookImplementationAllowed.load(std::memory_order_acquire)) {
            Tf2VrLog("[TF2VR] implementation hook disabled by config; only the thunks are detoured.\n");
        }
        void* implementation = (slotIsThunk && g_hookImplementationAllowed.load(std::memory_order_acquire))
            ? FindImplementationAfterThunk(slotEntry) : nullptr;
        if (implementation && PrepareDetour(g_detourImpl, implementation)) {
            g_originalImpl = reinterpret_cast<UpdateSubresourceFn>(g_detourImpl.trampoline);
            if (CommitDetour(g_detourImpl, reinterpret_cast<void*>(&HookUpdateSubresourceImpl),
                             "camera-hook UpdateSubresource impl")) {
                char line[220]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] slot 48 is a thunk; the implementation behind it at %p is detoured too "
                    "(+0x%zX past the thunk).\n",
                    implementation,
                    static_cast<size_t>(static_cast<std::uint8_t*>(implementation) - slotEntry));
                Tf2VrLog(line);
            } else {
                if (g_inFlight.load(std::memory_order_acquire) == 0) RestoreDetour(g_detourImpl);
                g_originalImpl = nullptr;
            }
        } else if (slotIsThunk) {
            Tf2VrLog("[TF2VR] slot 48 is a thunk but no implementation was found behind it; "
                     "coverage will stay low and the correction will disarm itself.\n");
        }
    }
    // The 11.1 entry, if this context has one. Reported either way, including
    // its first bytes when the prologue is not recognised, so a wrong vtable
    // index names itself instead of silently doing nothing.
    {
        Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1;
        if (SUCCEEDED(context.As(&context1)) && context1) {
            auto** vtable1 = *reinterpret_cast<void***>(context1.Get());
            void* entry = vtable1 ? vtable1[kUpdateSubresource1VtableIndex] : nullptr;
            // SLOT SWAP FIRST, BECAUSE THAT IS THIS PROJECT'S RULE AND BECAUSE
            // THE ENTRY DETOUR COST THREE FLAT RUNS.
            //
            // S2C measured what the prologue detour was doing here: nothing. It
            // failed with "prologue mismatch at 00007FFC8166DDB0, bytes=40 55 53
            // 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 F9 48 81 EC C8 00" --
            // an ordinary push-heavy frame, simply a shape the matcher was
            // never taught. Meanwhile the SAME machine's VR sessions detoured
            // this entry at a different address entirely and read the frustum
            // 2368 times.
            //
            // So flat, the game's world camera went through the 11.1 entry, the
            // 11.1 entry was unhooked, and the only camera uploads we ever saw
            // were the two ORTHO families arriving through the 11.0 one. The
            // census said so with a cap of 96 and zero drops, which is what
            // makes that a real absence rather than a truncation.
            //
            // A slot swap needs no prologue at all. RSSetViewports below is
            // hooked this way for exactly the same reason, and its comment
            // records that its own prologue detour "has NEVER INSTALLED". The
            // entry detour is kept as the fallback rather than deleted, because
            // a vtable that will not go writable is a real case and the detour
            // is the answer to it.
            //
            // It REFUSES if the slot already holds our hook: reading that back
            // as the original builds a call that recurses into itself, which is
            // the trap the viewport swap already names.
            bool hooked1 = false;
            if (vtable1 && entry && entry != reinterpret_cast<void*>(&HookUpdateSubresource1)) {
                void** slot = &vtable1[kUpdateSubresource1VtableIndex];
                DWORD oldProtect = 0;
                if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                    g_original1 = reinterpret_cast<UpdateSubresource1Fn>(*slot);
                    g_slot1 = slot;
                    g_slot1Original = *slot;
                    *slot = reinterpret_cast<void*>(&HookUpdateSubresource1);
                    DWORD ignored = 0;
                    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
                    hooked1 = true;
                    char line[300]{};
                    std::snprintf(line, sizeof(line),
                        "[TF2VR] UpdateSubresource1 (11.1 entry) hooked BY VTABLE SLOT SWAP at %p, "
                        "no prologue required. This is the entry the world camera uses; the "
                        "prologue detour that used to guard it failed flat and cost three runs.\n",
                        entry);
                    Tf2VrLog(line);
                } else {
                    Tf2VrLog("[TF2VR] UpdateSubresource1: the vtable slot would not go writable; "
                             "falling back to the entry detour.\n");
                }
            }
            if (!hooked1 && entry && IsExpectedPrologue(static_cast<const std::uint8_t*>(entry)) &&
                PrepareDetour(g_detour1, entry)) {
                g_original1 = reinterpret_cast<UpdateSubresource1Fn>(g_detour1.trampoline);
                if (CommitDetour(g_detour1, reinterpret_cast<void*>(&HookUpdateSubresource1),
                                 "camera-hook UpdateSubresource1 entry")) {
                    hooked1 = true;
                    char line[200]{};
                    std::snprintf(line, sizeof(line),
                        "[TF2VR] UpdateSubresource1 (11.1 entry) detoured at %p, variant %s.\n",
                        entry, PrologueVariantName(static_cast<const std::uint8_t*>(entry)));
                    Tf2VrLog(line);
                } else {
                    if (g_inFlight.load(std::memory_order_acquire) == 0) RestoreDetour(g_detour1);
                    g_original1 = nullptr;
                }
            }
            if (!hooked1) LogPrologueMismatch("UpdateSubresource1", entry);
        } else {
            Tf2VrLog("[TF2VR] context has no ID3D11DeviceContext1; only the legacy upload entry exists.\n");
        }
    }
    // RUNG B. Same reasoning as the viewport trace below: read-only, a pointer
    // compare per call, and it answers the question that is open RIGHT NOW --
    // so it should not also be a key someone has to be told to press, which is
    // how the last headset run produced nothing. A failure here must never stop
    // the correction installing.
    // OFF unless the ini asks. It is read-only and it did not crash, but it is
    // 2.6 million hook calls a session and it has told us what it can: the game
    // has thousands of distinct constant buffers and a 32-slot table samples an
    // arbitrary handful of them.
    if (MapCensusWanted() && InstallMapCensus(vtable)) SetMapCensusEnabled(true);

    // VIEWPORT RECORDING, BY VTABLE SLOT SWAP.
    //
    // This has NEVER INSTALLED. Every log this project has kept says the same
    // thing and nobody followed it up:
    //
    //   HOME/END: RSSetViewports prologue mismatch at ...B300; bytes=
    //   48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 50 ...
    //
    // So the viewport is an unmeasured quantity here, and it is exactly the
    // thing that moves a draw whose vertices and projection are both constant
    // -- which is the state every other instrument has now confirmed the HUD is
    // in. If the game sets a different viewport for the UI pass than for the
    // world pass, every HUD layer moves and the world does not, which is the
    // shape of the symptom.
    //
    // The slot swap needs no prologue at all, the same way it rescued
    // Map/Unmap. It refuses if the slot already holds our own hook, because
    // reading that back as the "original" builds a call that recurses into
    // itself.
    if (!g_viewportOriginal) {
        void** slot = &vtable[kRSSetViewportsVtableIndex];
        if (*slot != reinterpret_cast<void*>(&HookRSSetViewports)) {
            DWORD oldProtect = 0;
            if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                g_viewportOriginal = reinterpret_cast<RSSetViewportsFn>(*slot);
                *slot = reinterpret_cast<void*>(&HookRSSetViewports);
                DWORD ignored = 0;
                VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
                Tf2VrLog("[TF2VR] RSSetViewports hooked by vtable slot swap -- the prologue "
                         "detour has never once installed, so this quantity has never been "
                         "measured.\n");
                BeginViewportTrace();
            } else {
                Tf2VrLog("[TF2VR] RSSetViewports: the vtable slot would not go writable.\n");
            }
        }
    }
    const bool drawReady = PrepareThunkDetour(g_drawDetour, vtable[kDrawIndexedVtableIndex]);
    if (!drawReady) {
        LogPrologueMismatch("DrawIndexed", vtable[kDrawIndexedVtableIndex]);
        RestoreDetour(g_drawDetour);
        if (requireDrawCorrelation) { RestoreDetour(g_detour); return false; }
        Tf2VrLog("[TF2VR] END: continuing without DrawIndexed correlation; it is not needed for the offset test.\n");
    }
    // Publish both trampolines before either entry is patched, so a concurrent
    // call can only ever forward correctly.
    g_original = reinterpret_cast<UpdateSubresourceFn>(g_detour.trampoline);
    g_drawOriginal = drawReady ? reinterpret_cast<DrawIndexedFn>(g_drawDetour.trampoline) : nullptr;
    if (!CommitDetour(g_detour, reinterpret_cast<void*>(&HookUpdateSubresource),
                      "camera-hook UpdateSubresource entry") ||
        (drawReady && !CommitDetour(g_drawDetour, reinterpret_cast<void*>(&HookDrawIndexed),
                                    "camera-hook DrawIndexed thunk"))) {
        g_enabled.store(false, std::memory_order_release);
        if (g_inFlight.load(std::memory_order_acquire) == 0) { RestoreDetour(g_detour); RestoreDetour(g_drawDetour); }
        Tf2VrLog("[TF2VR] HOME/END: commit failed; no hook enabled.\n"); return false;
    }
    g_gameContext = context.Detach();
    g_totalCalls.store(0, std::memory_order_release);
    g_constantBufferCalls.store(0, std::memory_order_release);
    g_cameraSizedCalls.store(0, std::memory_order_release);
    g_recordCount.store(0, std::memory_order_release);
    g_frameIndex.store(0, std::memory_order_release);
    g_drawIndexedCount.store(0, std::memory_order_release);
    g_offsetAppliedCount.store(0, std::memory_order_release);
    g_offsetCandidateCount.store(0, std::memory_order_release);
    g_offsetMode.store(false, std::memory_order_release);
    // Every input to how the weapon looks, stated once at install.
    //
    // These are all toggles with defaults, and a default that never announces
    // itself is indistinguishable from a setting someone changed. "The gun got
    // bigger" could not be attributed to any of them because none of them was
    // in the log unless it had been pressed.
    {
        char line[400]{};
        const int monoEye = g_weaponMonoEye.load(std::memory_order_acquire);
        std::snprintf(line, sizeof(line),
            "[TF2VR] weapon rendering state: worldFovMatch=%d (HOME; off makes the gun much "
            "larger), includeNearOrigin=%d (the gun's own origin-(0,0,0) pass), monoEye=%d "
            "(-1 stereo, 0 left, 1 right, 2 centre), headsetFovMatch=%d (F11), lowerCameraExcluded=%d.\n",
            g_viewmodelMatchWorldFov.load(std::memory_order_acquire) ? 1 : 0,
            g_viewmodelIncludeNearOrigin.load(std::memory_order_acquire) ? 1 : 0,
            monoEye,
            g_matchHeadsetFov.load(std::memory_order_acquire) ? 1 : 0,
            g_excludeLowerCamera.load(std::memory_order_acquire) ? 1 : 0);
        Tf2VrLog(line);
    }
    return true;
}
}

void BeginCameraUpdateTrace(ID3D11Device* device) {
    if (!InstallDetours(device, true)) return;
    // Recording deliberately does NOT start here. Arming mid-frame captured the
    // tail of a frame from upload #6 onward -- no shadow cascades, no main
    // scene, a fifth of the draws -- which then read as though the game had
    // been in a menu. It starts at the next Present instead.
    g_tracePendingStart.store(true, std::memory_order_release);
    Tf2VrLog("[TF2VR] trace armed; recording starts at the next frame boundary and covers whole frames.\n");
}

// Called once per presented frame, from the Present hook. Present happens after
// a frame's uploads, so flipping recording on here starts it at the beginning
// of the next frame.
void BeginViewportTrace();
void AdvanceViewportTrace();
void BeginViewportTrace() {
    if (!g_viewportOriginal) {
        Tf2VrLog("[TF2VR] viewport trace: RSSetViewports is not hooked (arm the correction first).\n");
        return;
    }
    g_seenViewportCount = 0;
    for (auto& seen : g_seenViewports) seen = {};
    g_viewportTraceFrames.store(120, std::memory_order_release);
    Tf2VrLog("[TF2VR] viewport trace: recording every distinct viewport rectangle for 120 frames.\n");
}

// Called on the render frame boundary; reports once the window closes.
void AdvanceViewportTrace() {
    // RUNG B rides this clock rather than inventing another. Its own window is
    // longer -- the question is which buffers change ACROSS a turn, not what a
    // single frame contained.
    {
        static std::uint64_t lastMapReport = 0;
        const std::uint64_t now = GetTickCount64();
        if (now - lastMapReport >= 8000) {
            lastMapReport = now;
            ReportMapCensus();
        }
    }
    const std::uint32_t remaining = g_viewportTraceFrames.load(std::memory_order_acquire);
    if (!remaining) return;
    if (g_viewportTraceFrames.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    char header[260]{};
    std::snprintf(header, sizeof(header),
        "[TF2VR] viewport trace complete: %zu distinct rectangle%s over 120 frames. A rectangle "
        "shorter than the backbuffer, offset down the target, IS the letterbox -- and it is fixable "
        "by widening it rather than by accepting 16:9.\n",
        g_seenViewportCount, g_seenViewportCount == 1 ? "" : "s");
    Tf2VrLog(header);
    for (size_t i = 0; i < g_seenViewportCount; ++i) {
        const SeenViewport& seen = g_seenViewports[i];
        char line[220]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR]   viewport %zu: x=%.0f y=%.0f %.0fx%.0f (aspect %.4f) set %u times\n",
            i, seen.topLeftX, seen.topLeftY, seen.width, seen.height,
            seen.height != 0.0f ? seen.width / seen.height : 0.0f, seen.count);
        Tf2VrLog(line);
    }
    // THE ACTUAL QUESTION. A viewport that is constant for the world and moving
    // for the UI is what would move every HUD layer and leave the world alone,
    // and the distinct-rectangle tally above cannot show it.
    const auto dump = [](const char* what, const ViewportRange& r) {
        char line[300]{};
        if (!r.seen) {
            std::snprintf(line, sizeof(line), "[TF2VR]   %s: never set.\n", what);
            Tf2VrLog(line);
            return;
        }
        const float rx = r.hiX - r.loX, ry = r.hiY - r.loY;
        const float rw = r.hiW - r.loW, rh = r.hiH - r.loH;
        std::snprintf(line, sizeof(line),
            "[TF2VR]   %s (%llu sets): x %.1f..%.1f  y %.1f..%.1f  w %.1f..%.1f  h %.1f..%.1f%s\n",
            what, r.count, r.loX, r.hiX, r.loY, r.hiY, r.loW, r.hiW, r.loH, r.hiH,
            (rx > 0.5f || ry > 0.5f || rw > 0.5f || rh > 0.5f) ? "   <-- MOVES" : "   (fixed)");
        Tf2VrLog(line);
    };
    dump("viewport WHILE THE HUD WIDGET DRAWS", g_vpInHud);
    dump("viewport everywhere else          ", g_vpElsewhere);
}

void BeginCorrectionTrace() {
    g_correctionTraceOrdinal.store(0, std::memory_order_release);
    g_censusOrdinal.store(0, std::memory_order_release);
    g_correctionTraceFrameIndex.store(0, std::memory_order_release);
    // Thirty, not four. Four frames is ~48 ms at 84 Hz -- far too short to
    // contain a deliberate head movement, and the residual error under
    // investigation exists ONLY while the head is moving. The window has to be
    // long enough for the thing being diagnosed to happen inside it.
    g_correctionTraceFrames.store(30, std::memory_order_release);
    Tf2VrLog("[TF2VR] correction trace: recording every corrected upload for 30 whole frames "
             "(~0.35 s) -- move your head THROUGHOUT. Within one frame every corrected upload "
             "must share a gen and a rel; if the gun's pass (nearPlane -1) reads a later gen "
             "than the world passes, it is rotated by a different amount than the scene behind "
             "it and that is the jitter.\n");
}

void NotifyCameraUpdateFrameBoundary() {
    // The bone write's once-per-frame gate. Incremented here because this is the
    // one place that is called exactly once per PRESENTED frame.
    g_boneWriteFrameCounter.fetch_add(1, std::memory_order_acq_rel);
    AdvanceViewportTrace();
    // WORLD-RECT GATE HEARTBEAT: once a second, only while the refused count is
    // moving, so a clean run writes nothing and a 46 s video writes ~46 lines.
    {
        static std::uint64_t lastTick = 0;
        static unsigned long long lastRejected = 0;
        const std::uint64_t now = GetTickCount64();
        if (now - lastTick >= 1000) {
            lastTick = now;
            const unsigned long long rejected = g_worldRectRejected.load(std::memory_order_relaxed);
            if (rejected != lastRejected) {
                lastRejected = rejected;
                char line[300]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] world-rect gate: accepted=%llu refused=%llu unknown-target=%llu | "
                    "main-scene rect now %.0fx%.0f at (%.0f,%.0f) | target %.0fx%.0f\n",
                    g_worldRectAccepted.load(std::memory_order_relaxed), rejected,
                    g_worldRectUnknownTarget.load(std::memory_order_relaxed),
                    g_mainSceneViewportW.load(std::memory_order_acquire),
                    g_mainSceneViewportH.load(std::memory_order_acquire),
                    g_mainSceneViewportX.load(std::memory_order_acquire),
                    g_mainSceneViewportY.load(std::memory_order_acquire),
                    g_backbufferWidth.load(std::memory_order_acquire),
                    g_backbufferHeight.load(std::memory_order_acquire));
                Tf2VrLog(line);
            }
        }
    }
    // HUD-PASS FRAME liveness: every 10 s until the digest has printed once, say
    // how many near-origin uploads it has seen, so a silent digest reads as
    // "no such pass" and not as a probe that never ran.
    {
        static std::uint64_t lastLive = 0;
        static unsigned liveLines = 0;
        const std::uint64_t now = GetTickCount64();
        if (g_hudFrameLines == 0 && liveLines < 30 && now - lastLive >= 10000) {
            lastLive = now;
            ++liveLines;
            char line[200]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] HUD-PASS FRAME: watching; %llu near-origin uploads seen so far, no window "
                "closed yet.\n", g_hudFrameTotal);
            Tf2VrLog(line);
        }
    }
    // An F5 mark that no pinned upload answered within 120 frames is reported
    // as such, so "no digest after the mark" reads as a finding and not as a
    // probe that never ran.
    if (g_pinDigestMark.load(std::memory_order_relaxed)) {
        if (++g_pinDigestMarkFrames >= 120) {
            g_pinDigestMark.store(false, std::memory_order_relaxed);
            g_pinDigestMarkFrames = 0;
            char line[220]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] PIN DIGEST (F5 MARK UNANSWERED): no pinned upload in 120 frames after the "
                "mark; the pin was rotating nothing when the wearer pressed. Applied total=%llu "
                "skipped total=%llu.\n",
                g_weaponPinApplied.load(std::memory_order_relaxed),
                g_weaponPinSkipped.load(std::memory_order_relaxed));
            Tf2VrLog(line);
        }
    } else {
        g_pinDigestMarkFrames = 0;
    }
    // The pin belongs to the frame that set it. Dropping it here means a frame
    // with no main-scene upload (a menu, a loading screen) falls back to the
    // live latch rather than silently correcting against a stale frame's
    // angles for as long as that state lasts.
    // How many passes claimed to be the main scene this frame, reported when it
    // changes. One means no skybox and first/last/origin are all the same pass;
    // two means the choice matters, and that is the condition under which both
    // previous attempts went wrong.
    {
        static unsigned lastCount = 0;
        if (g_mainScenePassesThisFrame != lastCount) {
            lastCount = g_mainScenePassesThisFrame;
            char line[240]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] main-scene passes per frame: %u (pin mode %d). Two means a 3D skybox is "
                "up and the pin has a real choice to make.\n",
                g_mainScenePassesThisFrame, g_pinMode.load(std::memory_order_relaxed));
            Tf2VrLog(line);
        }
    }
    if (g_mainSceneCandidateValid) {
        std::memcpy(g_mainSceneOrigin, g_mainSceneCandidateOrigin, sizeof(g_mainSceneOrigin));
        std::memcpy(g_mainSceneBase, g_mainSceneCandidateBase, sizeof(g_mainSceneBase));
        std::memcpy(g_mainSceneMatrix, g_mainSceneCandidateMatrix, sizeof(g_mainSceneMatrix));
        g_mainSceneOriginValid.store(true, std::memory_order_release);
        g_mainSceneMatrixValid.store(true, std::memory_order_release);
        g_mainSceneCandidateValid = false;
    }
    g_mainScenePassesThisFrame = 0;
    g_frameAnglesValid.store(false, std::memory_order_release);
    // Advance the correction trace on the render frame boundary, so its frame
    // numbering matches the uploads it is recording.
    const std::uint32_t tracing = g_correctionTraceFrames.load(std::memory_order_acquire);
    if (tracing) {
        if (g_correctionTraceFrames.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            Tf2VrLog("[TF2VR] correction trace complete.\n");
        } else {
            g_correctionTraceFrameIndex.fetch_add(1, std::memory_order_acq_rel);
            g_correctionTraceOrdinal.store(0, std::memory_order_release);
            g_censusOrdinal.store(0, std::memory_order_release);
        }
    }
    if (!g_gameContext) return;
    if (g_tracePendingStart.exchange(false, std::memory_order_acq_rel)) {
        g_totalCalls.store(0, std::memory_order_release);
        g_constantBufferCalls.store(0, std::memory_order_release);
        g_cameraSizedCalls.store(0, std::memory_order_release);
        g_recordCount.store(0, std::memory_order_release);
        g_frameIndex.store(0, std::memory_order_release);
        g_drawIndexedCount.store(0, std::memory_order_release);
        g_traceFramesRemaining.store(kTraceFrames, std::memory_order_release);
        g_enabled.store(true, std::memory_order_release);
        return;
    }
    if (!g_traceFramesRemaining.load(std::memory_order_acquire)) return;
    if (g_traceFramesRemaining.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        g_frameIndex.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    // Teardown is left to the next plugin frame rather than done here: it
    // restores patched d3d11 code and writes the whole report, neither of which
    // belongs inside Present.
    g_enabled.store(false, std::memory_order_release);
}

// NOT in the anonymous namespace below, because the header declares it now: a
// flat run has to be able to install these detours without arming stereo, which
// is the only thing that installs them today.
// "Already installed" is not a failure to arm.
//
// InstallDetours refuses when g_gameContext is already set, which is correct
// for install but wrong as a gate on everything that needs the hook present.
// Head tracking installs it on F9, so DEL afterwards ran BeginOffsetTest,
// took the early return, and never set g_offsetArmed -- alternate-frame stereo
// came up and reported success while the per-eye offset that produces the
// actual disparity stayed off. Both eyes then render the same viewpoint, which
// is a working stereo pipeline with no stereo in it, and the only trace of it
// is one "hook unavailable/already active" line among the startup noise.
bool EnsureDetoursInstalled(ID3D11Device* device) {
    if (g_gameContext) return true;
    return InstallDetours(device, false);
}

void HoldCameraDetours(bool hold) {
    // Logs only on a transition, so callers may release it from a common exit
    // path without repeating a line every frame.
    if (g_detourHold.exchange(hold, std::memory_order_acq_rel) == hold) return;
    Tf2VrLog(hold ? "[TF2VR] camera detours HELD by a read-only caller: the frustum and viewport "
                    "recorders will keep running instead of being torn down when no mutation owns "
                    "them. Nothing is armed by this.\n"
                  : "[TF2VR] camera detours released; the normal teardown rules apply again.\n");
}

void ResetGameViewportTally() {
    g_viewportTallyCount = 0;
    g_viewportTallyTotal = 0;
    g_viewportTallyOverflow = 0;
    for (auto& entry : g_viewportTally) entry = {};
}

bool DominantGameViewport(float* width, float* height, unsigned long long* count,
                          unsigned long long* total) {
    size_t best = kViewportTallySlots;
    unsigned long long bestCount = 0;
    for (size_t i = 0; i < g_viewportTallyCount; ++i) {
        if (g_viewportTally[i].count > bestCount) { bestCount = g_viewportTally[i].count; best = i; }
    }
    if (total) *total = g_viewportTallyTotal;
    if (count) *count = bestCount;
    if (best >= kViewportTallySlots) { if (width) *width = 0.0f; if (height) *height = 0.0f; return false; }
    if (width) *width = g_viewportTally[best].width;
    if (height) *height = g_viewportTally[best].height;
    return bestCount > 0;
}

void ReadUploadLadderCounters(UploadLadderCounters* out) {
    if (!out) return;
    out->installed = g_gameContext != nullptr;
    out->invocations = g_hookInvocations.load(std::memory_order_relaxed);
    out->onOurContext = g_hookContextMatched.load(std::memory_order_relaxed);
    out->cameraSized = g_cameraSizedSeenRacy;
    out->worldPerspective = g_worldPerspectiveSeenRacy;
    out->mainScene = g_mainSceneSeenRacy;
    out->lastNearPlane = g_lastCameraSizedNearPlane;
    out->expectedNearPlane = kMainSceneNearPlane;
}

namespace {

void BeginOffsetTest(ID3D11Device* device, OffsetMode kind, const char* label) {
    if (!EnsureDetoursInstalled(device)) return;
    // Recording stays off: this run is judged by pixels, not by the log.
    g_offsetModeKind = kind;
    g_offsetMode.store(true, std::memory_order_release);
    g_offsetArmed.store(true, std::memory_order_release);
    g_framesRemaining.store(kOffsetTestFrames, std::memory_order_release);
    char line[280]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] %s (+%.1f X) armed for %u frames on its selected upload family only; auto-reverts.\n",
        label, kOriginOffsetX, kOffsetTestFrames);
    Tf2VrLog(line);
}
}

void BeginCameraOriginOffsetTest(ID3D11Device* device) {
    BeginOffsetTest(device, OffsetMode::CameraOrigin, "c_cameraOrigin offset");
}

void BeginMatrixEyeTranslationTest(ID3D11Device* device) {
    BeginOffsetTest(device, OffsetMode::MatrixEyeTranslation, "c_cameraRelativeToClip eye translation");
}

void SetViewmodelCompensationEnabled(ID3D11Device* device, bool enabled) {
    (void)device;
    // Deliberately does NOT install the hook here. Installing at the moment F9
    // is pressed resolved a function whose prologue did not match
    // UpdateSubresource (observed 48 89 5C 24 08..., expected 40 53 55 56 57
    // 41 54...), so the install failed and the toggle silently controlled a
    // flag nothing read. The same install succeeds from the Present path, so
    // arming only sets intent and TickViewmodelCompensation does the install
    // from a point that is known to work.
    g_compensationEnabled.store(enabled, std::memory_order_release);
    g_compensationInstallAttempts.store(0, std::memory_order_release);
    // Counts are per-arming, so an A/B reads as two clean runs rather than one
    // ever-growing total.
    g_compensationAppliedCount.store(0, std::memory_order_release);
    g_compensationSkippedCount.store(0, std::memory_order_release);
    g_lowerCameraExcludedCount.store(0, std::memory_order_release);
    Tf2VrLog(enabled
        ? "[TF2VR] viewmodel correction ON: the gun is dragged back into the body's frame.\n"
        : "[TF2VR] viewmodel correction OFF: the gun renders in the head's frame, as the engine "
          "built it. If it now stays pinned to the centre of the screen while the world sweeps "
          "past, the viewmodel is view-attached and the correction is structurally required.\n");
    // Release the hook on the way out, now that the guard above will allow it.
    // Skipped while a trace or offset test is still using it; those tear it
    // down on their own schedule.
    if (!enabled && g_gameContext && !g_enabled.load(std::memory_order_acquire) &&
        !g_offsetArmed.load(std::memory_order_acquire) &&
        !g_framesRemaining.load(std::memory_order_acquire)) {
        FinishTraceIfQuiescent();
    }
}

// Called once per presented frame. Retries the install a bounded number of
// times and reports the resolved pointer, so a persistent failure names itself
// instead of looking like a correction that does nothing.
// Reports what the correction is actually doing, once a second while it is
// enabled. Without this, "I pressed INSERT and nothing changed" cannot be told
// apart from "the hook never installed" or "the family was never selected" --
// which is the exact ambiguity that made INSERT look broken during Task 01b,
// when the hook had failed to install and the toggle controlled a flag nothing
// read. Every number needed to tell those cases apart is here.
void ReportViewmodelCompensation() {
    static std::uint32_t tick = 0;
    if (!g_compensationEnabled.load(std::memory_order_acquire)) { tick = 0; return; }
    if (tick++ % 120 != 0) return;
    char line[520]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] viewmodel correction: hook=%s | entry calls=%llu, of those on OUR context=%llu, "
        "of those camera-sized=%llu re-entrant=%llu | applied=%u skipped(no head data)=%u lower-camera excluded=%u "
        "torn reads=%u view builds=%llu weaponhook hits=%llu foreign=%llu body=%.3fms/call over %llu | EYE OFFSET armed=%d sign=%+.1f shifted=%u | "
        "head delta yaw=%+.2f deg | SPLIT nearOrigin seen=%llu excl=%llu worldOrigin seen=%llu excl=%llu.\n",
        g_gameContext ? "installed" : "NOT INSTALLED",
        static_cast<unsigned long long>(g_hookInvocations.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_hookContextMatched.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_hookCameraSized.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_hookReentrantCount.load(std::memory_order_acquire)),
        g_compensationAppliedCount.load(std::memory_order_acquire),
        g_compensationSkippedCount.load(std::memory_order_acquire),
        g_lowerCameraExcludedCount.load(std::memory_order_acquire),
        g_compensationTornReadCount.load(std::memory_order_acquire),
        static_cast<unsigned long long>(g_cameraHookCallCount),
        WeaponSettingsHits(),
        g_foreignCallsRacy,
        g_bodyCallsRacy ? (g_bodyMsRacy / static_cast<double>(g_bodyCallsRacy)) : 0.0,
        g_bodyCallsRacy,
        // The whole image alternating between two viewpoints is what an armed
        // eye offset looks like, and the weapon shows it worst because it is
        // nearest the camera and so has the largest parallax. Armed here without
        // DEL having been pressed would mean it never disarmed, or that
        // something else arms it -- either way it is ours, not the compositor's.
        g_offsetArmed.load(std::memory_order_acquire) ? 1 : 0,
        g_eyeSign.load(std::memory_order_acquire),
        g_offsetAppliedCount.load(std::memory_order_acquire),
        g_headDeltaYawDegrees,
        static_cast<unsigned long long>(g_nearOriginSeenCount.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_nearOriginExcludedCount.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_worldOriginSeenCount.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_worldOriginExcludedCount.load(std::memory_order_acquire)));
    Tf2VrLog(line);
}

// Re-reads slot 48 every presented frame and detours whatever it now points at,
// if that is not already patched. D3D11 swaps this entry at runtime on this
// per-object dispatch table, and a swap we do not follow silently stops the
// correction mid-session.
void TickUpdateSubresourceTargets() {
    if (!g_gameContext) return;
    auto** vtable = *reinterpret_cast<void***>(g_gameContext);
    if (!vtable) return;
    void* current = vtable[kUpdateSubresourceVtableIndex];
    if (!current || current == g_detour.target || current == g_detourAlt.target) return;
    // Our own detour writes a jump at the entry, so a target we already patched
    // no longer looks like a prologue. Only an untouched, recognised entry is
    // adopted.
    if (!IsExpectedPrologue(static_cast<const std::uint8_t*>(current))) return;
    if (g_detourAlt.target) return;  // both slots already spoken for
    if (!PrepareDetour(g_detourAlt, current)) return;
    g_originalAlt = reinterpret_cast<UpdateSubresourceFn>(g_detourAlt.trampoline);
    if (!CommitDetour(g_detourAlt, reinterpret_cast<void*>(&HookUpdateSubresourceAlt),
                      "camera-hook UpdateSubresource alt-context")) {
        if (g_inFlight.load(std::memory_order_acquire) == 0) RestoreDetour(g_detourAlt);
        g_originalAlt = nullptr;
        return;
    }
    char line[240]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] UpdateSubresource slot 48 moved to a SECOND entry (%p, variant %s); it is now detoured too.\n",
        current, PrologueVariantName(static_cast<const std::uint8_t*>(current)));
    Tf2VrLog(line);
}

// Refuses to run a PARTIAL correction.
//
// Correcting a fraction of the camera uploads is strictly worse than correcting
// none: the weapon is put in the body's frame on the frames we catch and left in
// the head's frame on the rest, which reads as flickering. That is exactly what
// shipped when the hook was allowed onto an entry carrying about 2% of the
// traffic -- a regression on the previous behaviour, where the hook declined to
// install and the weapon merely followed the head steadily.
//
// The passive trace measures ~35 camera-sized uploads per frame on this engine.
// Anything below a small fraction of that means the dominant upload path is not
// intercepted, so the correction disarms itself and says so, rather than
// flickering and leaving it to be reported from the headset.
constexpr float kMinCameraUploadsPerFrame = 5.0f;
constexpr std::uint64_t kCoverageGraceFrames = 240;
std::atomic_uint64_t g_coverageFrames = 0;
std::atomic_bool g_coverageFailed = false;

void CheckUploadCoverage() {
    if (!g_compensationEnabled.load(std::memory_order_acquire)) {
        g_coverageFrames.store(0, std::memory_order_release);
        g_coverageFailed.store(false, std::memory_order_release);
        return;
    }
    if (g_coverageFailed.load(std::memory_order_acquire)) return;
    // The coverage guard exists to stop a PARTIAL correction flickering the
    // weapon. During a sweep it does the opposite of its job: arming in a menu
    // legitimately sees zero uploads, the guard latches, and every level after
    // that measures a disarmed hook -- which is exactly how the last run
    // produced no data at all.
    if (g_bodySweep.load(std::memory_order_acquire)) return;
    const std::uint64_t frames = g_coverageFrames.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (frames < kCoverageGraceFrames) return;
    const float perFrame = static_cast<float>(g_hookCameraSized.load(std::memory_order_acquire)) /
                           static_cast<float>(frames);
    if (perFrame >= kMinCameraUploadsPerFrame) return;
    g_coverageFailed.store(true, std::memory_order_release);
    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] viewmodel correction DISARMED: only %.2f camera uploads per frame are reaching the "
        "hook, against ~35 expected. The engine's main upload path is not intercepted, and a partial "
        "correction flickers the weapon -- which is worse than leaving it alone. The weapon will "
        "follow the head steadily until the right entry is hooked.\n", perFrame);
    Tf2VrLog(line);
    g_compensationEnabled.store(false, std::memory_order_release);
}

void TickViewmodelCompensation(ID3D11Device* device) {
    ReportViewmodelCompensation();
    TickUpdateSubresourceTargets();
    CheckUploadCoverage();
    // THE UPLOAD HOOK IS NOT INSTALLED UNLESS SOMETHING ARMED IT.
    //
    // F1's matrix dump asked for a frame of camera passes on a flat run with
    // autoarm = 0, and got silence -- not because the request failed but
    // because the only three things that install this detour (the offset test,
    // the compensation, stereo) were all off, so there was no hook for the
    // request to be read by. The keypress logged "requested" and nothing on
    // the other side of it existed.
    //
    // A read-only diagnostic is entitled to install the hook it reads through.
    // Same call, same refusal-if-already-installed semantics, and it happens
    // before the compensation's own early-out so it does not inherit that gate.
    // ONE ATTEMPT, EVER. Not "retry until g_gameContext is set".
    //
    // InstallDetours patches three functions and returns false if any of them
    // mismatches; UpdateSubresource1 and RSSetViewports routinely do on this
    // build, so it reports failure having already patched the one that matters.
    // Keying the retry on g_gameContext therefore never terminates, and this
    // ran 146 times in thirty seconds, re-patching its own patch until the
    // trampoline jumped into itself and the game died.
    //
    // A read-only diagnostic gets exactly one go, and if that does not take,
    // it says so once and stays quiet.
    if (device && !g_gameContext && !g_diagnosticHookAttempted.load(std::memory_order_acquire) &&
        (IsCameraPassMatrixDumpPending() || g_diagnosticHookHold.load(std::memory_order_acquire))) {
        g_diagnosticHookAttempted.store(true, std::memory_order_release);
        if (!InstallDetours(device, false)) {
            Tf2VrLog("[TF2VR] diagnostic upload hook: one install attempt made and it reported "
                     "failure. Not retrying -- retrying is what re-patches a live patch. Anything "
                     "that needs this hook will read as unavailable rather than crash.\n");
        }
    }
    if (!g_compensationEnabled.load(std::memory_order_acquire)) return;
    if (g_gameContext || !device) return;
    const std::uint32_t attempt = g_compensationInstallAttempts.fetch_add(1, std::memory_order_acq_rel);
    if (attempt >= kMaxCompensationInstallAttempts) return;
    // Every 120th frame. The retry cadence was 20, which produced 30 identical
    // failure lines and buried the one line that matters.
    if (attempt % 120 != 0) return;
    if (InstallDetours(device, false)) {
        Tf2VrLog("[TF2VR] Viewmodel compensation: upload hook installed; body and weapon stay with the aim.\n");
        g_compensationInstallAttempts.store(kMaxCompensationInstallAttempts, std::memory_order_release);
        return;
    }
    if (attempt + 120 >= kMaxCompensationInstallAttempts) {
        Tf2VrLog("[TF2VR] Viewmodel compensation: giving up on installing the upload hook. "
                 "Arming stereo with DEL installs it by another path.\n");
    }
}

void BeginViewmodelProbe(ID3D11Device* device) {
    BeginOffsetTest(device, OffsetMode::ViewmodelProbe,
                    "VIEWMODEL PROBE: player-origin, near-plane 1 family, MATRIX translation");
}

bool BeginStereoEyeOffset(ID3D11Device* device, float sign, unsigned int frames) {
    g_eyeSign.store(sign, std::memory_order_release);
    // The reference camera is re-established per session; a stale origin from
    // another map would misclassify the sky camera.
    g_referenceValid.store(false, std::memory_order_release);
    for (auto& entry : g_originTable) { entry.used = false; entry.count = 0; }
    BeginOffsetTest(device, OffsetMode::PerEye, "per-eye IPD offset");
    // Still bounded, but long enough for a real headset session; the toggle
    // is the primary control and this is the backstop.
    if (g_gameContext && frames) g_framesRemaining.store(frames, std::memory_order_release);
    return g_gameContext != nullptr;
}

void SetStereoEyeSign(float sign) { g_eyeSign.store(sign, std::memory_order_release); }

void SetCorrectWorldOriginPass(bool correct) {
    g_excludeWorldOriginFromCorrection.store(!correct, std::memory_order_release);
    Tf2VrLog(correct
        ? "[TF2VR] Viewmodel correction rotates the WORLD-origin near-plane-1 passes (default).\n"
        : "[TF2VR] Viewmodel correction EXCLUDES the WORLD-origin near-plane-1 passes.\n");
}

void SetCorrectHudPass(bool correct) {
    g_excludeNearOriginFromCorrection.store(!correct, std::memory_order_release);
    Tf2VrLog(correct
        ? "[TF2VR] Viewmodel correction now ALSO rotates the HUD near-origin pass (old behaviour; this is the wave).\n"
        : "[TF2VR] Viewmodel correction EXCLUDES the HUD near-origin pass; only the weapon is counter-rotated.\n");
}


void SetViewmodelLowerCameraExcluded(bool excluded) {
    g_excludeLowerCamera.store(excluded, std::memory_order_release);
    g_lowerCameraExcludedCount.store(0, std::memory_order_release);
    Tf2VrLog(excluded
        ? "[TF2VR] Viewmodel correction now EXCLUDES the lower camera (origin must match the main scene).\n"
        : "[TF2VR] Viewmodel correction now covers the whole near-plane-1 family, as shipped.\n");
}

bool IsViewmodelLowerCameraExcluded() { return g_excludeLowerCamera.load(std::memory_order_acquire); }

void SetWeaponSize(float size) {
    const float clamped = size < 0.2f ? 0.2f : (size > 3.0f ? 3.0f : size);
    g_weaponSize.store(clamped, std::memory_order_release);
    char line[240]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] weapon size %.2f (1.00 is the world-FOV match alone; lower is a smaller gun).\n",
        clamped);
    Tf2VrLog(line);
}

float WeaponSize() { return g_weaponSize.load(std::memory_order_acquire); }

void SetViewmodelDetachLean(float strength) {
    const float clamped = strength < 0.0f ? 0.0f : (strength > 1.0f ? 1.0f : strength);
    g_detachLean.store(clamped, std::memory_order_release);
    char line[260]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] lean detach %.2f: 1.00 pins the weapon in the world and the head slides around it, "
        "0.00 lets it ride along as camera-relative geometry does by default.\n", clamped);
    Tf2VrLog(line);
}

void SetWeaponPinnedToController(bool pinned) {
    g_weaponPinned.store(pinned, std::memory_order_release);
    // Re-zero on every arm, so the gun starts from wherever the hand is now
    // rather than from where it was when the session started.
    g_haveWeaponPinReference.store(false, std::memory_order_release);
    if (!pinned) {
        char line[420]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] weapon pin OFF; the gun is back on the engine's own transform. Applied to %llu "
            "uploads, skipped %llu for want of a tracked right hand.\n",
            static_cast<unsigned long long>(g_weaponPinApplied.load()),
            static_cast<unsigned long long>(g_weaponPinSkipped.load()));
        Tf2VrLog(line);
        return;
    }
    g_weaponPinApplied.store(0, std::memory_order_release);
    g_weaponPinSkipped.store(0, std::memory_order_release);
    const int slot = g_weaponPinSlot.load(std::memory_order_acquire);
    char armLine[700]{};
    std::snprintf(armLine, sizeof(armLine),
        "[TF2VR] weapon pin ON, ROTATION ONLY, pinning viewmodel pass %s. Hold the right controller "
        "still for a moment -- the first tracked pose becomes the reference -- then rotate your "
        "wrist.\n"
        "[TF2VR]   Cycle to the next pass with the pin-slot key until the GUN is the thing that "
        "moves. Origin does not identify the gun: the near-origin pass is the HUD, which is what the "
        "first attempt moved. Each pass is tabulated by its own fixed FOV as it is seen, so the slot "
        "that works is recorded rather than remembered.\n",
        slot < 0 ? "ALL of them (diagnostic)" : "one slot");
    Tf2VrLog(armLine);
    char slotLine[420]{};
    int used = std::snprintf(slotLine, sizeof(slotLine), "[TF2VR]   slot %d selected; known passes:", slot);
    const int count = g_pinSlotCount.load(std::memory_order_acquire);
    if (count == 0) {
        used += std::snprintf(slotLine + used, sizeof(slotLine) - used,
                              " none seen yet, they are discovered as they upload");
    }
    for (int index = 0; index < count && used < static_cast<int>(sizeof(slotLine)) - 40; ++index) {
        used += std::snprintf(slotLine + used, sizeof(slotLine) - used, " [%d]=%.1fdeg%s", index,
                              static_cast<double>(g_pinSlotFov[index].load(std::memory_order_relaxed)),
                              g_pinSlotOriginNear[index].load(std::memory_order_relaxed) ? "(HUD?)" : "");
    }
    std::snprintf(slotLine + used, sizeof(slotLine) - used, "\n");
    Tf2VrLog(slotLine);
}

bool IsWeaponPinnedToController() { return g_weaponPinned.load(std::memory_order_acquire); }

void RecentreWeaponPin() {
    g_haveWeaponPinReference.store(false, std::memory_order_release);
    Tf2VrLog("[TF2VR] weapon pin reference cleared; the next tracked pose re-zeroes the gun.\n");
}

void CycleWeaponPinSlot() {
    const int count = g_pinSlotCount.load(std::memory_order_acquire);
    int slot = g_weaponPinSlot.load(std::memory_order_acquire);
    // Cycle 0..count-1 and then -1 (all), so the "what does moving the whole
    // family look like" diagnostic is reachable without a second key.
    if (count <= 0) {
        Tf2VrLog("[TF2VR] weapon pin slot: no viewmodel passes have uploaded yet, so there is nothing "
                 "to cycle. Load into a world first.\n");
        return;
    }
    slot = (slot < 0) ? 0 : (slot + 1 >= count ? -1 : slot + 1);
    g_weaponPinSlot.store(slot, std::memory_order_release);
    // Re-zero, or the gun jumps by whatever the hand has done since the last one.
    g_haveWeaponPinReference.store(false, std::memory_order_release);
    g_weaponPinApplied.store(0, std::memory_order_release);
    g_weaponPinSkipped.store(0, std::memory_order_release);
    char line[420]{};
    int used = std::snprintf(line, sizeof(line), "[TF2VR] weapon pin slot -> %s. Passes:",
                             slot < 0 ? "ALL (every viewmodel pass at once)" : "one");
    for (int index = 0; index < count && used < static_cast<int>(sizeof(line)) - 40; ++index) {
        used += std::snprintf(line + used, sizeof(line) - used, " %s[%d]=%.1fdeg%s",
                              index == slot ? "->" : "", index,
                              static_cast<double>(g_pinSlotFov[index].load(std::memory_order_relaxed)),
                              g_pinSlotOriginNear[index].load(std::memory_order_relaxed) ? "(HUD?)" : "");
    }
    std::snprintf(line + used, sizeof(line) - used,
                  " -- reference re-zeroed; hold the controller still, then rotate.\n");
    Tf2VrLog(line);
}

void SetStepGuard(bool enabled) {
    g_stepGuard.store(enabled, std::memory_order_release);
    Tf2VrLog(enabled
        ? "[TF2VR] step guard ON: a one-frame view-height dropout is held at the previous height.\n"
        : "[TF2VR] step guard OFF: the game's one-frame drop on a step up is passed through.\n");
}

void SetStepGuardThreshold(float units) {
    const float clamped = units < 1.0f ? 1.0f : (units > 40.0f ? 40.0f : units);
    g_stepGuardThreshold.store(clamped, std::memory_order_release);
    char line[220]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] step guard triggers on a single-frame drop over %.1f units. Measured spikes were "
        "6 to 11; ordinary climbing is about 1.2 a frame.\n", clamped);
    Tf2VrLog(line);
}

void SetViewmodelPinMode(int mode) {
    const int clamped = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
    g_pinMode.store(clamped, std::memory_order_release);
    static const char* const kModes[] = {
        "[TF2VR] viewmodel angles pin on the FIRST main-scene pass (measured: stutters on close "
        "targets).\n",
        "[TF2VR] viewmodel angles pin on the LAST main-scene pass (measured: stutters near distant "
        "vistas).\n",
        "[TF2VR] viewmodel angles pin on the main-scene pass whose ORIGIN is the world's, so the 3D "
        "skybox cannot claim it whichever order it renders in.\n",
    };
    Tf2VrLog(kModes[clamped]);
}

void SetZoomThreshold(float fraction) {
    const float clamped = fraction < 0.5f ? 0.5f : (fraction > 0.999f ? 0.999f : fraction);
    g_zoomThreshold.store(clamped, std::memory_order_release);
    char line[200]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] zoom detected below %.3f of the resting frustum. Rest jitters by about 0.005, so "
        "higher than ~0.995 will trip constantly.\n", clamped);
    Tf2VrLog(line);
}

void SetWeaponMonoOnlyWhenZoomed(bool onlyWhenZoomed) {
    g_weaponMonoOnlyWhenZoomed.store(onlyWhenZoomed, std::memory_order_release);
    Tf2VrLog(onlyWhenZoomed
        ? "[TF2VR] weapon is full stereo normally and snaps to centre only while zoomed.\n"
        : "[TF2VR] weapon uses its chosen viewpoint at all times, so it has no depth even when "
          "hip-firing.\n");
}

void SetWeaponMonoEye(int eye) {
    const int clamped = eye < 0 ? -1 : (eye > 2 ? 2 : eye);
    g_weaponMonoEye.store(clamped, std::memory_order_release);
    if (clamped < 0) {
        Tf2VrLog("[TF2VR] weapon renders in stereo like everything else; its sights will not agree "
                 "with where rounds land.\n");
        return;
    }
    if (clamped == 2) {
        Tf2VrLog("[TF2VR] weapon renders from the CENTRE in both frames; the world stays stereo. "
                 "The round travels along that same axis, so the sights point where it goes. The "
                 "weapon carries no stereo depth of its own, which is the price.\n");
        return;
    }
    char line[260]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] weapon renders through the %s eye in both frames. NOTE: measured wrong -- the "
        "round leaves along the CENTRE axis, so an eye-pinned sight is off by half an IPD whichever "
        "eye is chosen. Use 2 (centre) unless comparing.\n",
        clamped == 0 ? "LEFT" : "RIGHT");
    Tf2VrLog(line);
}

void SetViewmodelDisplayPoseCorrection(bool useDisplayPose) {
    g_useDisplayPoseForCorrection.store(useDisplayPose, std::memory_order_release);
    Tf2VrLog(useDisplayPose
        ? "[TF2VR] Viewmodel correction cancels the DISPLAY pose (published every frame), which is "
          "what the compositor reprojects to.\n"
        : "[TF2VR] Viewmodel correction cancels the ENGINE's written angles, which are held on "
          "roughly every fourth frame.\n");
}

void SetViewmodelFrameLockedAngles(bool locked) {
    g_frameLockedAngles.store(locked, std::memory_order_release);
    Tf2VrLog(locked
        ? "[TF2VR] Viewmodel correction uses ONE angle pair per frame, pinned at the main-scene "
          "upload, so the gun rotates by the same amount the world did.\n"
        : "[TF2VR] Viewmodel correction reads the live latch at each upload; a frame can be "
          "corrected with the previous frame's angles.\n");
}

void SetViewmodelIncludeNearOrigin(bool included) {
    g_viewmodelIncludeNearOrigin.store(included, std::memory_order_release);
    Tf2VrLog(included
        ? "[TF2VR] Viewmodel family INCLUDES the camera-relative pass at the origin -- the gun's own "
          "pass, which the |origin|>1 world test had been rejecting.\n"
        : "[TF2VR] Viewmodel family restricted to world cameras (|origin|>1); the gun's own pass at "
          "the origin is excluded, as it was before.\n");
}

void SetViewmodelEyeBasisPreRotation(bool preRotation) {
    g_eyeBasisPreRotation.store(preRotation, std::memory_order_release);
    Tf2VrLog(preRotation
        ? "[TF2VR] Per-eye separation on the corrected pass now uses the PRE-rotation (head) right axis.\n"
        : "[TF2VR] Per-eye separation on the corrected pass uses the post-rotation (body) right axis, as shipped.\n");
}

bool IsViewmodelEyeBasisPreRotation() { return g_eyeBasisPreRotation.load(std::memory_order_acquire); }

// ---- the world-anchored HUD: position, size, and a census -----------------

void NudgeHud2dPlacement(float x, float y, float zoomFactor) {
    float z = g_hud2dZoom.load(std::memory_order_relaxed) * zoomFactor;
    if (z < 0.05f) z = 0.05f;
    if (z > 20.0f) z = 20.0f;
    g_hud2dShiftX.store(g_hud2dShiftX.load(std::memory_order_relaxed) + x,
                        std::memory_order_release);
    g_hud2dShiftY.store(g_hud2dShiftY.load(std::memory_order_relaxed) + y,
                        std::memory_order_release);
    g_hud2dZoom.store(z, std::memory_order_release);
}

void SetHud2dPlacement(float x, float y, float zoom) {
    if (zoom < 0.05f) zoom = 0.05f;
    if (zoom > 20.0f) zoom = 20.0f;
    g_hud2dShiftX.store(x, std::memory_order_release);
    g_hud2dShiftY.store(y, std::memory_order_release);
    g_hud2dZoom.store(zoom, std::memory_order_release);
}

void SetHud2dHeadAnchor(bool on) {
    g_hud2dHeadAnchor.store(on, std::memory_order_release);
    Tf2VrLog(on
        ? "[TF2VR] hud2d.head_anchor = 1: the pixel-ortho pass is shifted every frame so the aim "
          "anchor sits at the pass centre -- reticle, overlays and fades in front of the head.\n"
        : "[TF2VR] hud2d.head_anchor = 0: the flat HUD pass is left where the engine puts it "
          "(at the attack angles, i.e. the right hand with aim.cmd = 2).\n");
}

void ReadHud2dPlacement(float* x, float* y, float* zoom) {
    if (x) *x = g_hud2dShiftX.load(std::memory_order_acquire);
    if (y) *y = g_hud2dShiftY.load(std::memory_order_acquire);
    if (zoom) *zoom = g_hud2dZoom.load(std::memory_order_acquire);
}

unsigned long long Hud2dAdjustedUploadCount() {
    return g_hud2dAdjustedCount.load(std::memory_order_relaxed);
}

void ReadReticleAnchor(float* x, float* y, float* yawOffset, float* pitchOffset) {
    if (x) *x = g_reticleAnchorX;
    if (y) *y = g_reticleAnchorY;
    if (yawOffset) *yawOffset = g_reticleYawOffset;
    if (pitchOffset) *pitchOffset = g_reticlePitchOffset;
}

float DeclaredMagnificationInUse() { return AdsDeclaredMagnification(); }

bool ReadLatchedCameraAngles(float* base, float* applied, std::uint32_t* generation) {
    return ReadLatchedAngles(base, applied, generation);
}

void ReadReticleAnchorDetail(float* passWidth, float* passHeight, float* divisorX, float* divisorY,
                             int* hadAim, unsigned long long* updates) {
    if (passWidth) *passWidth = g_hud2dPassWidth;
    if (passHeight) *passHeight = g_hud2dPassHeight;
    if (divisorX) *divisorX = g_reticleDivisorX;
    if (divisorY) *divisorY = g_reticleDivisorY;
    if (hadAim) *hadAim = g_reticleHadAim;
    if (updates) *updates = g_reticleAnchorUpdates.load(std::memory_order_relaxed);
}

void SetUploadCensusEnabled(bool enabled) {
    const bool was = g_uploadCensusEnabled.exchange(enabled, std::memory_order_acq_rel);
    if (was == enabled) return;
    // SAY SO. A read-only setting that produces no confirmation is
    // indistinguishable from one that was never applied, and a whole run was
    // spent finding that out the hard way.
    Tf2VrLog(enabled ? "[TF2VR] upload census ARMED: every camera-sized upload family and every "
                       "constant-buffer width is tallied and printed once a second. Read-only.\n"
                     : "[TF2VR] upload census off.\n");
}

// ITS OWN CLOCK, NOT A CALIBRATION MODE'S.
//
// This used to print only while the F12 target happened to be the reticle,
// which meant `set hud.census = 1` did nothing on its own and a run came back
// with no census at all. Gating a READ-ONLY diagnostic behind a UI mode is
// simply wrong: it costs a run to discover and the failure looks like the
// instrument being broken rather than unarmed.
void AdvanceUploadCensus() {
    if (!g_uploadCensusEnabled.load(std::memory_order_relaxed)) return;
    static std::uint64_t lastTick = 0;
    const std::uint64_t now = GetTickCount64();
    if (now - lastTick < 1000) return;
    lastTick = now;
    ReportUploadCensus();
}

// THE CENSUS IS THE INSTRUMENT FOR THE RETICLE, not decoration.
//
// The reticle is in a family nobody has named: the ortho near-identity one is
// full-screen effects, and a translation on the near-origin perspective one
// moved every HUD element except the reticle. This prints every distinct
// upload signature the frame contains, so the next question -- which of these
// is the reticle -- is answered by moving one and looking, instead of by
// another round of guessing at predicates.
void ReportUploadCensus() {
    if (!g_uploadCensusEnabled.load(std::memory_order_relaxed)) return;
    // THE HALF-ANGLE IS PRINTED BESIDE THE ROW LENGTH, because |r| = 1/tan(half
    // FOV) and nobody reads a reciprocal at a glance. This is what makes the
    // census answer the ADS question directly: the widest horizontal is rest,
    // every narrower one is a zoom, and the ratio between them IS the
    // magnification -- without going through IsMainSceneCamera, which is
    // calibrated on a VR near plane and classifies nothing at all flat.
    Tf2VrLog("[TF2VR] upload census -- one line per distinct camera-sized upload family. "
             "|r0| = 1/tan(halfFovX), so the degrees beside it are the real frustum; the widest "
             "is rest and the ratio of any narrower one to it is that weapon's magnification:\n");
    int shown = 0;
    for (const auto& entry : g_uploadFamilies) {
        if (!entry.used) continue;
        ++shown;
        char line[300]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR]   near=%.3f m15=%.3f origin=(%.1f %.1f %.1f) |r0|=%.3f |r1|=%.3f "
            "=> FOV %.1f x %.1f deg  n=%llu  %s%s%s\n",
            entry.nearPlane, entry.element15, entry.origin[0], entry.origin[1], entry.origin[2],
            entry.rowLength0, entry.rowLength1,
            entry.rowLength0 > 0.0001f
                ? 2.0f * std::atan(1.0f / entry.rowLength0) * 57.2957795f : 0.0f,
            entry.rowLength1 > 0.0001f
                ? 2.0f * std::atan(1.0f / entry.rowLength1) * 57.2957795f : 0.0f,
            static_cast<unsigned long long>(entry.count),
            entry.element15 != 0.0f ? "ORTHO(effects) " : "",
            (entry.element15 == 0.0f &&
             entry.origin[0] * entry.origin[0] + entry.origin[1] * entry.origin[1] +
                     entry.origin[2] * entry.origin[2] <= 1.0f)
                ? "NEAR-ORIGIN "
                : "",
            std::fabs(entry.nearPlane + 1.0f) < 0.01f ? "VIEWMODEL-FAMILY" : "");
        Tf2VrLog(line);
    }
    if (shown == 0) {
        Tf2VrLog("[TF2VR]   (nothing tallied -- no camera-sized uploads seen, so the hook is not "
                 "on the upload path this session; that is a hook problem, not a HUD one)\n");
    }
    // WHICH BOUND ENDED IT, in the same line as the count. A census that lost
    // families is not a shorter census, it is a different question answered.
    {
        ScanOutcome outcome;
        outcome.Finish(g_uploadFamilyDropped ? ScanOutcome::End::InstanceCap
                                             : ScanOutcome::End::Complete);
        char line[300]{};
        std::snprintf(line, sizeof(line),
                      "[TF2VR]   families held %d of %d, dropped for capacity %llu. %s\n", shown,
                      kUploadFamilyMax,
                      static_cast<unsigned long long>(g_uploadFamilyDropped), outcome.Describe());
        Tf2VrLog(line);
    }
    // AND THE SHAPES THE FAMILY CENSUS CANNOT SEE.
    //
    // Everything above is 576-byte buffers only. The non-reticle HUD moved for
    // none of those families, so the next question is whether it is uploaded
    // through a constant buffer of a different width entirely -- which nothing
    // in this plugin has ever looked at.
    Tf2VrLog("[TF2VR] constant-buffer widths seen this session (576 is the camera):\n");
    for (const auto& entry : g_bufferWidths) {
        if (!entry.used) continue;
        char line[160]{};
        std::snprintf(line, sizeof(line), "[TF2VR]   %6u bytes  n=%llu%s\n", entry.width,
                      static_cast<unsigned long long>(entry.count),
                      entry.width == kCameraBufferBytes ? "   <- the camera" : "");
        Tf2VrLog(line);
    }
    // The 64-byte buffers, as the 4x4 matrices they are. Rows on separate lines
    // so the shape is readable: an orthographic 2D transform, a projection, or
    // something else entirely is obvious at a glance and guessable from nothing.
    bool anyMatrix = false;
    for (const auto& entry : g_matrix64) {
        if (!entry.used) continue;
        if (!anyMatrix) {
            Tf2VrLog("[TF2VR] the 64-byte buffers, as 4x4 matrices -- the HUD is not in any camera "
                     "family, so this is the remaining candidate:\n");
            anyMatrix = true;
        }
        char line[300]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR]   n=%llu  [%9.4f %9.4f %9.4f %9.4f | %9.4f %9.4f %9.4f %9.4f | "
            "%9.4f %9.4f %9.4f %9.4f | %9.4f %9.4f %9.4f %9.4f]\n",
            static_cast<unsigned long long>(entry.count),
            entry.m[0], entry.m[1], entry.m[2], entry.m[3], entry.m[4], entry.m[5], entry.m[6],
            entry.m[7], entry.m[8], entry.m[9], entry.m[10], entry.m[11], entry.m[12],
            entry.m[13], entry.m[14], entry.m[15]);
        Tf2VrLog(line);
    }
}

void SetHookPassThrough(bool on) { g_hookPassThrough.store(on, std::memory_order_release); }

void SetUploadNoSubstitute(bool on) { g_uploadNoSubstitute.store(on, std::memory_order_release); }

void SetUploadIdentityCopy(bool on) { g_uploadIdentityCopy.store(on, std::memory_order_release); }

void SetHookBodyLevel(int level) {
    if (level < 0) level = 0;
    if (level > kBodyLevelFull) level = kBodyLevelFull;
    g_bodyLevel.store(level, std::memory_order_release);
}

int HookBodyLevel() { return g_bodyLevel.load(std::memory_order_acquire); }

unsigned long long HookCameraSizedUploads() { return g_hookCameraSized.load(std::memory_order_acquire); }

// SAME-FRAME STEREO. Copies the currently-bound scene target into a private
// texture at the mid-frame boundary -- the only instant at which eye 0 exists
// on its own, before batch 2 draws over it. The texture is created lazily to
// match whatever the engine actually bound, because guessing its format is how
// a CopyResource silently does nothing.
void CaptureSameFrameEye0(ID3D11DeviceContext* context, ID3D11Texture2D* source) {
    if (!context || !source) return;
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (g_sameFrameEye0) {
        D3D11_TEXTURE2D_DESC have{};
        g_sameFrameEye0->GetDesc(&have);
        if (have.Width != desc.Width || have.Height != desc.Height ||
            have.Format != desc.Format || have.SampleDesc.Count != desc.SampleDesc.Count) {
            g_sameFrameEye0.Reset();  // the engine changed target; rebuild
        }
    }
    if (!g_sameFrameEye0) {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (!device) { g_sameFrameCaptureFailures.fetch_add(1, std::memory_order_relaxed); return; }
        D3D11_TEXTURE2D_DESC mine = desc;
        mine.Usage = D3D11_USAGE_DEFAULT;
        mine.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        mine.CPUAccessFlags = 0;
        mine.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&mine, nullptr, &g_sameFrameEye0))) {
            // Some scene targets are not directly bindable as an SRV copy; fall
            // back to a plain default texture with no bind flags at all.
            mine.BindFlags = 0;
            if (FAILED(device->CreateTexture2D(&mine, nullptr, &g_sameFrameEye0))) {
                g_sameFrameCaptureFailures.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }
    context->CopyResource(g_sameFrameEye0.Get(), source);
    g_sameFrameEye0Valid.store(true, std::memory_order_release);
    g_sameFrameCaptures.fetch_add(1, std::memory_order_relaxed);
}

ID3D11Texture2D* SameFrameEye0Texture() {
    return g_sameFrameEye0Valid.load(std::memory_order_acquire) ? g_sameFrameEye0.Get() : nullptr;
}

void ResetSameFrameCapture() {
    // LATCH BEFORE ZEROING. The falsifier line is printed from the main thread
    // between frames, i.e. always AFTER this reset, so reading the live counter
    // there reported 0 every time and proved nothing. The peak is what the
    // question was ever about: 2 means the boundary was found.
    if (g_sameFrameScenePasses > g_sameFramePeakPasses) g_sameFramePeakPasses = g_sameFrameScenePasses;
    g_sameFrameScenePasses = 0;
    g_sameFrameEye0Valid.store(false, std::memory_order_release);
}

bool SameFrameInSecondPass() { return g_sameFrameScenePasses >= 2; }

bool SameFrameEyePairWanted() { return g_sameFrameEyePairWanted.load(std::memory_order_acquire); }
void RequestSameFrameEyePair() { g_sameFrameEyePairWanted.store(true, std::memory_order_release); }
void ClearSameFrameEyePairWanted() { g_sameFrameEyePairWanted.store(false, std::memory_order_release); }

void SameFrameCaptureCounts(unsigned long long* captures, unsigned long long* failures,
                            int* scenePassesLastFrame) {
    if (captures) *captures = g_sameFrameCaptures.load(std::memory_order_relaxed);
    if (failures) *failures = g_sameFrameCaptureFailures.load(std::memory_order_relaxed);
    if (scenePassesLastFrame) *scenePassesLastFrame = g_sameFramePeakPasses;
}

unsigned long long CameraUploadsForPass(int tag) {
    if (tag < 0 || tag > 2) return 0;
    return g_uploadsByPass[tag].load(std::memory_order_acquire);
}

// THE CONTEXT AND ITS TABLE, FOR A SLOT SWAP THAT IS NOT INSTALLED FROM INSIDE
// InstallDetours.
//
// Installing M1's census inside InstallDetours would have been tidier and would
// have silently never run. EnsureDetoursInstalled opens with
// `if (g_gameContext) return true;` -- so on any session where something else
// puts the detours in first (the ADS probe and the aspect sweep both can, and
// ads.probe is 1 in the live ini) InstallDetours is never entered again and a
// census hung off it would report nothing, with no line saying why. That is the
// same shape as the dead upload counter that cost F1 a run.
//
// So the census installs from the per-frame tick instead, retrying until it
// succeeds, and this hands it what it needs. Null until a context is verified.
ID3D11DeviceContext* VerifiedGameContextForSlotSwap(void*** vtableOut) {
    if (!g_gameContext) return nullptr;
    if (vtableOut) *vtableOut = *reinterpret_cast<void***>(g_gameContext);
    return g_gameContext;
}

// ---------------------------------------------------------------------------
// M3's public surface.

// ONE IMPLEMENTATION OF THE ALGEBRA, used by both the upload patch and M5's
// pre-draw write. Two copies of this would drift, and the one thing this route
// cannot afford is two eye translations that disagree by a sign or a scale.
//
// Moving the eye by e substitutes p' = p - e, so each row of
// c_cameraRelativeToClip loses that row's projection of e. row0 is the camera
// right axis TIMES the projection scale 1/(aspect*tan(fov/2)), so it is
// normalised before use -- unnormalised, the separation would be wrong by the
// FOV factor and would change with ADS zoom.
bool ApplyEyeTranslationInPlace(unsigned char* bytes, float ipd) {
    if (!bytes) return false;
    float matrix[16]{};
    std::memcpy(matrix, bytes + kCameraRelativeToClipOffset, sizeof(matrix));
    const float length =
        std::sqrt(matrix[0] * matrix[0] + matrix[1] * matrix[1] + matrix[2] * matrix[2]);
    if (!(length > 1e-4f)) return false;
    const float e[3] = {matrix[0] / length * ipd, matrix[1] / length * ipd,
                        matrix[2] / length * ipd};
    for (int row = 0; row < 4; ++row) {
        const float* basis = matrix + row * 4;
        matrix[row * 4 + 3] -= basis[0] * e[0] + basis[1] * e[1] + basis[2] * e[2];
    }
    std::memcpy(bytes + kCameraRelativeToClipOffset, matrix, sizeof(matrix));
    float origin[3]{};
    std::memcpy(origin, bytes + kCameraOriginOffset, sizeof(origin));
    for (int i = 0; i < 3; ++i) origin[i] += e[i];
    std::memcpy(bytes + kCameraOriginOffset, origin, sizeof(origin));
    return true;
}

bool TakeWorldCameraSnapshot(void** bufferOut, unsigned char* bytesOut, unsigned cap) {
    if (!g_worldCameraValid.load(std::memory_order_acquire)) return false;
    if (!bufferOut || !bytesOut || cap < kCameraBufferBytes) return false;
    *bufferOut = g_worldCameraBuffer.load(std::memory_order_acquire);
    if (!*bufferOut) return false;
    std::memcpy(bytesOut, g_worldCameraBytes, kCameraBufferBytes);
    return true;
}

unsigned WorldCameraBufferBytes() { return kCameraBufferBytes; }

void SetPreDrawWriteInProgress(bool on) { t_preDrawWriteInProgress = on; }

void SetBatch2Ipd(float ipd) {
    g_batch2Ipd.store(ipd, std::memory_order_release);
    g_m3Armed.store(true, std::memory_order_release);
    char line[560]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] stereo.batch2_ipd = %.3f world units. M3: batch 2's c_cameraOrigin is "
        "translated along the camera RIGHT axis (row0 of c_cameraRelativeToClip, which this "
        "file verifies to be unit length) by that much, on the uploads the render-thread "
        "discriminator attributes to batch 2. %s\n",
        ipd,
        ipd == 0.0f
            ? "ZERO IS THE POSITIVE CONTROL: the two batches must upload BYTE-IDENTICAL origins, "
              "and the per-frame census below compares them exactly rather than with a tolerance."
            : "The falsifier is TWO DISTINCT c_cameraOrigin values in ONE frame, counted inside "
              "the upload hook. F3 toggles the offset without disturbing the census.");
    Tf2VrLog(line);
}

bool Batch2OffsetArmed() {
    return g_m3Armed.load(std::memory_order_acquire) &&
           g_batch2OffsetArmed.load(std::memory_order_acquire);
}

void SetBatch2IpdMode(int mode) {
    g_batch2IpdMode.store(mode, std::memory_order_release);
    char line[520]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] stereo.batch2_ipd_mode = %d. %s\n", mode,
        mode == 2
            ? "EVERYTHING EXCEPT BATCH 2 takes the eye offset -- the inversion. Mode 1 proved that "
              "patching batch 1's uploads moves batch 1's world, and stereo does not care which "
              "batch carries the offset, only that the two differ. If the pair diverges, M3 has "
              "its stereo; if it is still identical, batch 2's draws are reading BATCH 1's "
              "patched constants, which explains every run since M3 began."
        : mode == 1
            ? "EVERY qualifying upload is offset, not just batch 2's -- which is exactly what the "
              "shipped test.eyetranslation does, and that visibly moved the world. If this run "
              "moves it too, the BATCH-2 GATE is what has been swallowing the offset; if it does "
              "not, my lever differs from the shipped one somewhere else."
            : "Batch 2 only (what true stereo needs).");
    Tf2VrLog(line);
}

void ToggleBatch2Offset() {
    if (!g_m3Armed.load(std::memory_order_acquire)) {
        Tf2VrLog("[TF2VR] M3: F3 pressed, but stereo.batch2_ipd is not in the ini. Nothing to "
                 "toggle.\n");
        return;
    }
    const bool on = !g_batch2OffsetArmed.load(std::memory_order_acquire);
    g_batch2OffsetArmed.store(on, std::memory_order_release);
    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] M3 batch-2 eye offset %s (F3). %s\n", on ? "ARMED" : "DISARMED",
        on ? "The next doubled frame must carry two distinct camera origins."
           : "THE POSITIVE CONTROL: the next doubled frame must carry ONE origin, identical "
             "between the batches. A frame that still shows two means something other than this "
             "lever is diverging them.");
    Tf2VrLog(line);
}

// Called at the Present boundary by the render-target census, which is the only
// thing that knows whether the interval that just closed was doubled.
void ReportBatchCameraOrigins(bool doubledInterval) {
    if (!g_m3Armed.load(std::memory_order_acquire)) return;
    static int reports = 0;
    constexpr int kMaxReports = 24;
    const bool worthPrinting = doubledInterval && reports < kMaxReports;
    if (worthPrinting) {
        ++reports;
        char head[760]{};
        std::snprintf(head, sizeof(head),
            "[TF2VR] M3 CAMERA CENSUS, one doubled frame: batch 1 uploaded %d distinct VIEW "
            "TRANSLATIONS (column 3 of c_cameraRelativeToClip), batch 2 uploaded %d. offset "
            "armed=%d ipd=%.3f | applied %llu, skipped: unnormalisable-basis %llu, not-world-family %llu (viewmodel + ortho, exempt by design). "
            "THE VIEW TRANSLATION IS THE FALSIFIER: the world renders camera-relative through "
            "that matrix, so a c_cameraOrigin that diverges while these do not means the bytes "
            "changed and the camera did not -- which is exactly what the previous build "
            "produced. Batch 1 reading 0 means the witness saw nothing and batch 2's number "
            "means nothing either.\n",
            g_viewXlatCountByBatch[1], g_viewXlatCountByBatch[2],
            g_batch2OffsetArmed.load(std::memory_order_relaxed) ? 1 : 0,
            g_batch2Ipd.load(std::memory_order_relaxed),
            g_batch2OffsetsApplied.load(std::memory_order_relaxed),
            g_batch2OffsetsSkippedBasis.load(std::memory_order_relaxed),
            g_batch2OffsetsSkippedFamily.load(std::memory_order_relaxed));
        Tf2VrLog(head);
        // THE VIEW TRANSLATION FIRST, because it is the one that decides
        // whether the picture differs. The origin list below it is now the
        // secondary reading: the last run had a clean origin divergence and
        // zero measured disparity in every depth band, which is precisely the
        // pair of readings this ordering exists to keep apart.
        for (int batch = 1; batch <= 2; ++batch) {
            for (int i = 0; i < g_viewXlatCountByBatch[batch]; ++i) {
                const OriginTally& t = g_viewXlatByBatch[batch][i];
                char line[240]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR]   batch %d VIEW-XLAT[%d] = (%.4f, %.4f, %.4f) x%llu\n",
                    batch, i, t.x, t.y, t.z, t.count);
                Tf2VrLog(line);
            }
            if (g_viewXlatOverflowByBatch[batch]) {
                char line[220]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR]   batch %d view-xlat: %llu further uploads did not fit the %d-slot "
                    "table -- TRUNCATED, so the distinct count is a floor.\n",
                    batch, g_viewXlatOverflowByBatch[batch], kMaxOriginsPerBatch);
                Tf2VrLog(line);
            }
        }
        for (int batch = 1; batch <= 2; ++batch) {
            char line[560]{};
            // span@buffer@bind. THE BIND ORDINAL IS N1-Q1: the batch-local
            // VSSetConstantBuffers count at the moment of the upload, which
            // orders an upload against the work INSIDE its span where the span
            // index alone cannot. The buffer pointer is the other half of Q2 --
            // compare it against the RTVCEN Q2 line's "b1 last" slot-2 pointer.
            int used = std::snprintf(line, sizeof(line),
                "[TF2VR]   batch %d world-camera uploads, span@buffer@bindOrdinal:", batch);
            for (int i = 0; i < g_worldUploadSpanCount[batch] && used < 480; ++i) {
                used += std::snprintf(line + used, sizeof(line) - used, " %d@%p@%llu",
                                      g_worldUploadSpans[batch][i], g_worldUploadBuffers[batch][i],
                                      g_worldUploadBinds[batch][i]);
            }
            std::snprintf(line + used, sizeof(line) - used,
                          "%s\n", g_worldUploadSpanCount[batch] ? "" : " (none)");
            Tf2VrLog(line);
        }
        for (int batch = 1; batch <= 2; ++batch) {
            for (int i = 0; i < g_originCountByBatch[batch]; ++i) {
                const OriginTally& t = g_originsByBatch[batch][i];
                char line[220]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR]   batch %d origin[%d] = (%.4f, %.4f, %.4f) x%llu\n",
                    batch, i, t.x, t.y, t.z, t.count);
                Tf2VrLog(line);
            }
            if (g_originOverflowByBatch[batch]) {
                char line[200]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR]   batch %d origin: %llu further uploads did not fit the %d-slot "
                    "table -- TRUNCATED, so the distinct count is a floor.\n",
                    batch, g_originOverflowByBatch[batch], kMaxOriginsPerBatch);
                Tf2VrLog(line);
            }
        }
    }
    // Reset EVERY interval, printed or not: the falsifier is about one frame,
    // and a table that accumulates across frames would report two origins for
    // any frame that merely followed a different one.
    for (int batch = 0; batch <= 2; ++batch) {
        g_originCountByBatch[batch] = 0;
        g_originOverflowByBatch[batch] = 0;
        g_viewXlatCountByBatch[batch] = 0;
        g_viewXlatOverflowByBatch[batch] = 0;
        g_worldUploadSpanCount[batch] = 0;
    }
}

void SetGameDrawIndexedTally(bool on) {
    g_censusDrawTallyArmed.store(on, std::memory_order_release);
}

// ARMED IS NOT THE SAME AS LIVE. HookDrawIndexed is an entry detour, and it is
// allowed to fail to install (`requireDrawCorrelation` is false for the census
// path). If it did, this counter cannot move and every per-span draw count is a
// zero that means "nobody was counting", not "nothing drew". So the arm state
// reported here is the conjunction: asked for, AND the hook actually in.
bool GameDrawIndexedTallyArmed() {
    return g_censusDrawTallyArmed.load(std::memory_order_acquire) && g_drawOriginal != nullptr;
}

unsigned long long GameDrawIndexedTally() {
    return g_censusDraws.load(std::memory_order_relaxed);
}

void SetHookBodyLevelSweep(bool on) {
    g_bodySweep.store(on, std::memory_order_release);
    // Start the sweep at the known-fast end, so the first level that is slow is
    // read off directly rather than inferred from where it stopped being slow.
    if (on) g_bodyLevel.store(0, std::memory_order_release);
}

bool AdvanceHookBodyLevelSweep() {
    if (!g_bodySweep.load(std::memory_order_acquire)) return false;
    // The upload count CANNOT move at level 0: that level returns before the
    // counter block, by design. Gating advancement on it therefore pinned the
    // sweep at level 0 forever -- which is why one run produced six level-0
    // windows and nothing else. The count is still published with every window
    // so an idle window can be recognised and discarded when reading the log;
    // it must not be what decides whether to advance.
    g_sweepCameraSizedMark.store(g_hookCameraSized.load(std::memory_order_acquire),
                                 std::memory_order_release);
    const int next = (g_bodyLevel.load(std::memory_order_acquire) + 1) % (kBodyLevelFull + 1);
    g_bodyLevel.store(next, std::memory_order_release);
    return true;
}

void SetHookImplementationAllowed(bool allowed) { g_hookImplementationAllowed.store(allowed, std::memory_order_release); }

void SetViewmodelMatchWorldFov(bool match) {
    g_viewmodelMatchWorldFov.store(match, std::memory_order_release);
    float x = 0.0f, y = 0.0f;
    const bool measured = GetMainSceneHalfTangents(x, y);
    char line[280]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] weapon FOV %s the world's.%s\n",
        match ? "forced to match" : "left at the engine's own value (as shipped),",
        match && !measured
            ? " No main-scene frustum measured yet, so this does nothing until one is seen."
            : "");
    Tf2VrLog(line);
}

bool IsViewmodelMatchWorldFov() { return g_viewmodelMatchWorldFov.load(std::memory_order_acquire); }

bool IsMatchHeadsetFov() { return g_matchHeadsetFov.load(std::memory_order_acquire); }

// Adjusting IPD by eye is the only practical way to settle world scale: too
// large makes the world feel small and close, too small makes it feel huge and
// far. The runtime's reported value is the right starting point but not
// necessarily the right rendering value, because a Source unit is only
// approximately an inch.
void NudgeHalfInterpupillary(float factor) {
    const float current = g_halfInterpupillaryUnits.load(std::memory_order_acquire);
    const float next = current * factor;
    if (!(next > 0.05f && next < 20.0f)) return;
    // A manual adjustment is a deliberate choice; stop the runtime overwriting it.
    g_halfIpdExplicit.store(true, std::memory_order_release);
    g_halfInterpupillaryUnits.store(next, std::memory_order_release);
    char line[200]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] half IPD %.3f Source units (%.1f mm total). Larger makes the world feel smaller and "
        "closer; smaller makes it feel larger and further.\n", next, next * 2.0f * 25.4f);
    Tf2VrLog(line);
}

// The rectangle the world pass was rendered into, for whoever submits the image.
// False until a main-scene camera upload has been seen, because before that
// there is nothing to be right about.
bool GetMainSceneViewport(float& x, float& y, float& width, float& height) {
    const float w = g_mainSceneViewportW.load(std::memory_order_acquire);
    const float h = g_mainSceneViewportH.load(std::memory_order_acquire);
    if (w < 1.0f || h < 1.0f) return false;
    x = g_mainSceneViewportX.load(std::memory_order_acquire);
    y = g_mainSceneViewportY.load(std::memory_order_acquire);
    width = w;
    height = h;
    return true;
}

void SetHeadsetHalfTangents(float halfTanX, float halfTanY) {
    const float previousX = g_headsetHalfTanX.exchange(halfTanX, std::memory_order_acq_rel);
    g_headsetHalfTanY.store(halfTanY, std::memory_order_release);
    // Once, when it first becomes known, rather than every located frame.
    if (previousX <= 0.0001f && halfTanX > 0.0001f) {
        char line[240]{};
        std::snprintf(line, sizeof(line),
            // SAY WHETHER IT IS ACTUALLY DOING ANYTHING. This line used to claim
            // "the main scene will be rendered at it rather than at the game's
            // own" unconditionally -- but it fires when the frustum is merely
            // RECORDED, and xr.match_headset_fov defaults to OFF and is off in
            // the shipped ini. Reading it while hunting a distortion cost real
            // time, because it names a behaviour that was measured, rejected and
            // withdrawn in HEADSET-ISSUES-2026-08-16, and then says it is on.
            "[TF2VR] headset frustum %.1f x %.1f deg recorded. xr.match_headset_fov is %s, so the "
            "main scene is rendered at %s. Matching it was tried and WITHDRAWN: narrowing the "
            "frustum into a buffer of a different aspect gives correct geometry and anisotropic "
            "pixels, which is a stretch.\n",
            2.0f * std::atan(halfTanX) * 57.2957795f, 2.0f * std::atan(halfTanY) * 57.2957795f,
            g_matchHeadsetFov.load(std::memory_order_acquire) ? "ON" : "OFF",
            g_matchHeadsetFov.load(std::memory_order_acquire) ? "the headset's frustum"
                                                              : "the game's own frustum");
        Tf2VrLog(line);
    }
}

void PublishBackbufferSize(float width, float height) {
    const float previousW = g_backbufferWidth.exchange(width, std::memory_order_acq_rel);
    const float previousH = g_backbufferHeight.exchange(height, std::memory_order_acq_rel);
    if (previousW == width && previousH == height) return;
    // A NEW TARGET RETIRES THE OLD RECTANGLE. The main-scene viewport was paired
    // inside the previous target; inside this one it is at best a stale
    // sub-rectangle (2560x1440 inside 5210x3648, right after the mode change)
    // and the submit side would hand the compositor a quarter of the image.
    // Cleared, so GetMainSceneViewport reports nothing and the whole target is
    // submitted until a plausible pairing arrives.
    g_mainSceneViewportX.store(0.0f, std::memory_order_release);
    g_mainSceneViewportY.store(0.0f, std::memory_order_release);
    g_mainSceneViewportW.store(0.0f, std::memory_order_release);
    g_mainSceneViewportH.store(0.0f, std::memory_order_release);
    char line[260]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] render target published: %.0fx%.0f (was %.0fx%.0f). The world-rect gate measures "
        "against this; the main-scene rectangle is cleared until a plausible pairing.\n",
        width, height, previousW, previousH);
    Tf2VrLog(line);
}

// xr.world_rect_gate. See world_rect_gate.h.
void SetWorldRectGate(bool gate) {
    g_worldRectGate.store(gate, std::memory_order_release);
    Tf2VrLog(gate
        ? "[TF2VR] xr.world_rect_gate = 1: a viewport under half the render target in either axis "
          "is refused as the world pass and the previous rectangle stands (the opening video "
          "paired 32x32, four runs in a row).\n"
        : "[TF2VR] xr.world_rect_gate = 0: every viewport paired with a main-scene upload is "
          "believed, which is what submitted 32x32 during the opening video.\n");
}

// F5. Defined here, outside the file's anonymous namespace, so plugin.cpp can
// reach it; the digest state it flips lives with ApplyWeaponPin above.
void MarkWeaponPinDigest() {
    g_pinDigestMark.store(true, std::memory_order_relaxed);
    g_hudFrameMark.store(true, std::memory_order_relaxed);
    Tf2VrLog("[TF2VR] F5 MARK: the wearer sees the rectangle NOW. The next pinned upload prints the "
             "pin digest; 120 frames without one is reported too.\n");
}

void SetViewportFull(bool full) {
    g_viewportFull.store(full, std::memory_order_release);
    g_viewportFullReported = false;
    g_aspectFixReported = false;
    Tf2VrLog(full ? "[TF2VR] xr.viewport_full = 1: the world pass will be widened to the whole render "
                    "target and the camera's vertical term corrected to match, so the rows the game "
                    "would have blacked out carry world instead.\n"
                  : "[TF2VR] xr.viewport_full = 0: the game keeps its own viewport.\n");
}

// xr.declare_rendered_fov, and bare F11.
void SetDeclareRenderedFov(bool declare) {
    g_declareRenderedFov.store(declare, std::memory_order_release);
    g_frustumHandedReported = false;
    Tf2VrLog(declare
        ? "[TF2VR] xr.declare_rendered_fov = 1: the projection layer declares the frustum the game "
          "was HANDED. Watch the FRUSTUM HANDED vs DECLARED line: the X ratio must read 1.000.\n"
        : "[TF2VR] xr.declare_rendered_fov = 0: the layer declares the frustum the game ASKED FOR, "
          "which is the sharp-2026-08-23 behaviour. The X ratio should read about 1.120, and that "
          "is a horizontal-only reprojection error -- it shows on YAW and not on pitch.\n");
}

bool IsDeclareRenderedFovArmed() { return g_declareRenderedFov.load(std::memory_order_acquire); }

// xr.fit_horizontal, and bare F11. Off is the shipped answer; the key exists so
// the two can be compared in one session rather than across two.
void SetFitHorizontal(bool fit) {
    g_fitHorizontal.store(fit, std::memory_order_release);
    g_frustumHandedReported = false;
    Tf2VrLog(fit
        ? "[TF2VR] xr.fit_horizontal = 1: row0 of the MAIN SCENE's projection is narrowed to the "
          "headset's tangent ratio. Only the main scene: the effects family (near plane -0.007) and "
          "the viewmodel family keep the game's own frustum, so they are drawn over a world that no "
          "longer shares their projection. Expect misregistered effects on the gun and on scenery, "
          "horizontal, zero at the centre and growing outward.\n"
        : "[TF2VR] xr.fit_horizontal = 0: no projection is touched, so every pass in the frame "
          "shares one frustum and the layer declares that same frustum. The game renders wider than "
          "the display shows, which costs pixels outside the visible cone and nothing else.\n");
}

bool IsFitHorizontalArmed() { return g_fitHorizontal.load(std::memory_order_acquire); }

bool IsViewportFullArmed() { return g_viewportFull.load(std::memory_order_acquire); }

void SetMatchHeadsetFov(bool match) {
    g_matchHeadsetFov.store(match, std::memory_order_release);
    Tf2VrLog(match
        ? "[TF2VR] main scene renders at the HEADSET frustum; no pixels are spent outside the "
          "visible cone.\n"
        : "[TF2VR] main scene renders at the GAME's frustum; anything past the headset's FOV is "
          "rendered and then cropped.\n");
}

// READ-ONLY VIEWS OF THE ZOOM STATE. They must not call WorldIsZoomed().
//
// WorldIsZoomed() is not a predicate, it is an edge detector: it stores back
// into g_zoomLatched on every call, and that latch is what supplies the
// hysteresis. A second caller on another clock would advance the latch between
// the render path's own calls and change the classification it was reading --
// the "one edge detector across a mode switch" fault, arrived at from the other
// direction. So the probe reads the latch and never drives it.
bool WorldZoomLatched() { return g_zoomLatched.load(std::memory_order_acquire); }
float RestingWorldHalfTanX() { return g_restingWorldTanX.load(std::memory_order_acquire); }
float PlateauRestingWorldHalfTanX() { return g_plateauRestTanX.load(std::memory_order_acquire); }
float PlateauRestingWorldHalfTanY() { return g_plateauRestTanY.load(std::memory_order_acquire); }

void SetAdsOpticPassthrough(float magnification) {
    if (magnification < 0.0f) magnification = 0.0f;
    const float previous = g_adsOpticPassthrough.exchange(magnification, std::memory_order_release);
    if (previous == magnification) return;
    char line[460]{};
    if (magnification <= 1.0f) {
        Tf2VrLog("[TF2VR] ads.optic_passthrough <= 1: NOTHING is exempt, so a scope is capped like "
                 "an iron sight. That is a choice, not a default.\n");
        return;
    }
    std::snprintf(line, sizeof(line),
        "[TF2VR] ads.optic_passthrough = %.2fx. A weapon whose OWN declared zoom reaches this or "
        "more is left entirely alone -- a scope should magnify, because that is what a scope "
        "physically does. Measured across the starting arsenal: pistols 1.01x, wingman 1.52x, "
        "shotgun 1.75x, smg 2.01-2.74x, rspn101 2.74x, dmr 3.26x, lmg up to 5.06x. The number "
        "comes from the weapon every time, so a gun this build has never seen classifies itself.\n",
        static_cast<double>(magnification));
    Tf2VrLog(line);
}

float AdsOpticPassthrough() { return g_adsOpticPassthrough.load(std::memory_order_relaxed); }

void SetAdsMaxMagnification(float cap) {
    if (cap < 0.0f) cap = 0.0f;
    const float previous = g_adsMaxMagnification.exchange(cap, std::memory_order_release);
    if (previous == cap) return;
    char line[420]{};
    if (cap < 1.0f) {
        std::snprintf(line, sizeof(line),
            "[TF2VR] ads.max_magnification = %.2f -> NO CAP. The weapon magnifies by whatever the "
            "engine asks for -- measured 1.01x on the pistols up to 5.06x on a scoped LMG -- and "
            "the layer declares that much, so it fills the view instead of sitting in a black "
            "box.\n",
            static_cast<double>(cap));
    } else {
        std::snprintf(line, sizeof(line),
            "[TF2VR] ads.max_magnification = %.2fx. ADS may magnify up to this and no further. "
            "Holding it back widens what is visible and makes distant things smaller, so it is a "
            "trade rather than a fix. A magnified optic is exempt on its own declared zoom. At "
            "rest the engine reports 1.00x, so nothing is scaled at rest by construction.\n",
            static_cast<double>(cap));
    }
    Tf2VrLog(line);
}

float AdsMaxMagnification() { return g_adsMaxMagnification.load(std::memory_order_relaxed); }

void ReadAdsCapCounters(unsigned long long* widened, unsigned long long* untouched,
                        float* lastMagnification) {
    if (widened) *widened = g_adsWidened.load(std::memory_order_relaxed);
    if (untouched) *untouched = g_adsUntouched.load(std::memory_order_relaxed);
    if (lastMagnification) *lastMagnification = g_adsLastMagnification.load(std::memory_order_relaxed);
}
unsigned long long PlateauRestStableSamples() {
    return g_plateauStableSamples.load(std::memory_order_relaxed);
}

// THE ZOOM THE WEARER COULD NOT SEE, AND WHY DECLARING IS THE WHOLE OF IT.
//
// Reported: "Zoom is not working at all. I do see a black window boxing but the
// actual landscape is not zooming at all."
//
// Both halves of that are one cause. The game DOES narrow its frustum for ADS --
// measured 2.74x on the rifle, from its own fov -- and renders that narrow view
// into the same buffer. We then DECLARED the narrow angle to the projection
// layer, so the compositor faithfully showed a 68.7 degree image across 68.7
// degrees of the headset's 110. World scale is preserved exactly, which is why
// nothing appeared to magnify, and the degrees the layer did not cover are the
// black box.
//
// PLAN-ADS section 0 recorded this as "the apparent zoom is already correctly
// neutralised" and treated it as a feature to dial back for periphery. The
// wearer has now seen it and rejected that reading outright.
//
// Flat has no such problem because a monitor subtends a FIXED angle whatever the
// game renders into it: narrow the frustum and the content magnifies, because
// the display did not shrink to match. The VR equivalent is to hold the DECLARED
// angle still while the rendered one narrows. The narrow image is then stretched
// across the view it always occupied -- magnified by exactly the engine's own
// ratio, and filling it, so the box goes with it.
//
// This scales what we DECLARE and never touches what the game RENDERS, which is
// why it lives here rather than in the upload path: nothing about the render
// moves, so nothing about the render can break.
// THE FRUSTUM THE GAME ACTUALLY RENDERED, with no ADS magnification applied.
//
// The distinction cost a real bug. GetMainSceneHalfTangents below returns the
// DECLARED frustum, which is what the projection layer must be told. But it had
// other callers, and one of them was the reticle's screen placement, which
// divides tan(offset) by the half-tangent to get NDC. The world's own
// projection puts a target at tan(phi)/tan(RENDERED), so dividing the reticle by
// tan(DECLARED) = tan(rendered) * magnification put the reticle at 1/mag of
// where the round actually lands:
//
//   "If I am aiming at a wall of bricks and I move my aim up 5 bricks, the
//    bullets shouldn't move up 10 bricks"
//
// -- at 2.67x on the rifle, precisely that. I had called that correct scope
// behaviour. It is not: a scope changes how far the RIFLE must move, never the
// relationship between where the reticle sits and where the round goes. Those
// two must agree at every magnification, and the wearer was right to reject it
// flatly.
//
// So anything positioning something IN the rendered image uses this one, and
// only the layer uses the declared one.
// How long ago the measured frustum was last written, in milliseconds, and how
// many times it has ever been written. UINT64_MAX for the age means never.
//
// A caller that reads a huge age is looking at a stale declaration, not at a
// missing one -- GetMainSceneHalfTangents() will still hand it the old numbers
// and still return true.
std::uint64_t MainSceneFrustumAgeMs() {
    const std::uint64_t stamp = g_mainSceneTanStampMs.load(std::memory_order_acquire);
    if (!stamp) return ~0ull;
    const std::uint64_t now = GetTickCount64();
    return now > stamp ? now - stamp : 0;
}

std::uint64_t MainSceneFrustumWrites() {
    return g_mainSceneTanWrites.load(std::memory_order_acquire);
}

bool GetMainSceneRenderedHalfTangents(float& halfTanX, float& halfTanY) {
    const float x = g_mainSceneHalfTanX.load(std::memory_order_acquire);
    const float y = g_mainSceneHalfTanY.load(std::memory_order_acquire);
    if (x <= 0.0f || y <= 0.0f) return false;
    halfTanX = x;
    halfTanY = y;
    return true;
}

// W2. The runtime's vertical lens extents, guarded: a pair that is not a
// sane frustum is dropped rather than sheared with.
void SetHeadsetLensVerticalExtents(float tanUp, float tanDown) {
    if (!(tanUp > tanDown)) return;
    if (tanUp - tanDown > 8.0f) return;   // > 165 degrees vertical is not a lens
    g_lensTanUp.store(tanUp, std::memory_order_release);
    g_lensTanDown.store(tanDown, std::memory_order_release);

    // THE KERNEL SELF-TEST, IN THE SHIPPED BINARY, at the moment the geometry
    // becomes known and immediately before the shear starts using it.
    //
    // tools/asym-projection-check.cpp verifies this arithmetic offline, and that
    // is the check that matters -- but it verifies a COMPILATION of the header,
    // not the DLL the wearer is about to run. This re-runs two of its anchors
    // in-process: the Quest 3's numbers must give the shear the offline check
    // pinned to 0.18160, and a symmetric lens must give exactly zero. If the
    // header ever drifts, or is compiled here with different flags, this says so
    // in the log rather than in the headset.
    static std::atomic_flag tested = ATOMIC_FLAG_INIT;
    if (tested.test_and_set(std::memory_order_acq_rel)) return;
    const float quest3 = tf2vr::ShearForCentre(1.27325f, 0.5f * (0.96569f + -1.42815f));
    const float centred = tf2vr::ShearForCentre(1.00000f, 0.5f * (1.00000f + -1.00000f));
    const tf2vr::TanExtents shifted = tf2vr::ShiftedExtents(1.37623f, 1.27325f, 0.0f, quest3);
    const bool pass = std::fabs(quest3 - 0.18160f) < 0.0005f && centred == 0.0f &&
                      std::fabs((shifted.up - shifted.down) - 2.0f * 1.27325f) < 0.0001f &&
                      shifted.down < -1.42815f;
    char line[460]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] LENS SHEAR self-test %s (in this DLL, not the offline check): Quest 3 reference "
        "shear %.5f want 0.18160, centred lens %.5f want exactly 0, sheared span %.5f want %.5f, "
        "sheared bottom %.5f must reach past -1.42815. This headset reports tan up %.5f down %.5f, "
        "centre %.5f = %.2f deg off forward.\n",
        pass ? "PASS" : "FAILED", quest3, centred, shifted.up - shifted.down, 2.0f * 1.27325f,
        shifted.down, tanUp, tanDown, 0.5f * (tanUp + tanDown),
        std::atan(0.5f * (tanUp + tanDown)) * 57.2957795f);
    Tf2VrLog(line);
}

void SetLensShearEnabled(bool enabled) {
    const bool previous = g_lensShearEnabled.exchange(enabled, std::memory_order_acq_rel);
    // LOGS ON THE FIRST CALL EVEN IF NOTHING CHANGED.
    //
    // The default is ON, so `set xr.lens_shear = 1` in the ini would otherwise
    // apply silently and the run would carry no statement of the arm state at
    // all -- which is the same shape as the mistake that has cost this project
    // headset runs: a config that reads back as unchanged looks exactly like a
    // config that never applied.
    static std::atomic_flag announced = ATOMIC_FLAG_INIT;
    const bool first = !announced.test_and_set(std::memory_order_acq_rel);
    if (previous == enabled && !first) return;
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] xr.lens_shear = %d. %s\n", enabled ? 1 : 0,
        enabled
            ? "The rendered frustum is aimed down the lens axis, so the bottom of the display "
              "has content instead of black. Same span, same cost."
            : "The rendered frustum goes back to centred on forward, which is what every build "
              "before this one did. On a centred headset the two are identical.");
    Tf2VrLog(line);
}

bool LensShearEnabled() { return g_lensShearEnabled.load(std::memory_order_acquire); }

float LensShearAppliedNdcY() { return g_lensShearAppliedNdcY.load(std::memory_order_acquire); }

bool GetMainSceneHalfTangents(float& halfTanX, float& halfTanY) {
    const float x = g_mainSceneHalfTanX.load(std::memory_order_acquire);
    const float y = g_mainSceneHalfTanY.load(std::memory_order_acquire);
    if (x <= 0.0f || y <= 0.0f) return false;
    const float declared = AdsDeclaredMagnification();
    halfTanX = x * declared;
    halfTanY = y * declared;
    return true;
}

void MarkHalfInterpupillaryUnitsExplicit() { g_halfIpdExplicit.store(true, std::memory_order_release); }

// The half-IPD actually being applied, for the session banner. What is APPLIED,
// not what was configured: the runtime's value overrides the default unless the
// ini named one, and a banner that echoed the request would hide that.
float HalfInterpupillaryUnits() {
    return g_halfInterpupillaryUnits.load(std::memory_order_acquire);
}

// Source units are ~1 inch. The runtime knows the wearer's actual IPD, so
// deriving it beats a hardcoded 64 mm that is right for nobody in particular.
float SourceUnitsPerMetre() {
    const float halfMetres = g_runtimeHalfIpdMetres.load(std::memory_order_acquire);
    if (halfMetres <= 0.0f) return 1.0f / 0.0254f;  // ~1 unit per inch
    return g_halfInterpupillaryUnits.load(std::memory_order_acquire) / halfMetres;
}

void SetRuntimeIpdMetres(float metres) {
    if (metres > 0.040f && metres < 0.090f) {
        // Recorded even when the config overrides the rendering value: it is
        // the real-world reference that turns units into metres.
        g_runtimeHalfIpdMetres.store(metres * 0.5f, std::memory_order_release);
    }
    if (g_halfIpdExplicit.load(std::memory_order_acquire)) return;
    if (!(metres > 0.040f && metres < 0.090f)) return;
    const float units = (metres * 0.5f) / 0.0254f;
    const float previous = g_halfInterpupillaryUnits.exchange(units, std::memory_order_acq_rel);
    if (std::fabs(previous - units) > 0.005f) {
        char line[200]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] half IPD taken from the runtime: %.3f Source units (%.1f mm total), was %.3f.\n",
            units, metres * 1000.0f, previous);
        Tf2VrLog(line);
    }
}

void SetHalfInterpupillaryUnits(float units) {
    // A zero or negative half-IPD would silently collapse stereo; a huge one is
    // an obvious typo. Refuse both rather than render something unfusable.
    if (!(units > 0.05f && units < 20.0f)) {
        char line[160]{};
        std::snprintf(line, sizeof(line), "[TF2VR] config: stereo.half_ipd_units=%.3f out of range; keeping %.3f.\n",
                      units, g_halfInterpupillaryUnits.load(std::memory_order_relaxed));
        Tf2VrLog(line);
        return;
    }
    g_halfInterpupillaryUnits.store(units, std::memory_order_release);
    char line[128]{};
    std::snprintf(line, sizeof(line), "[TF2VR] Half interpupillary distance set to %.3f Source units.\n", units);
    Tf2VrLog(line);
}

void NotifyStereoFrameBoundary() {
    // Promote the most frequent origin of the frame just rendered to the
    // reference player camera, then reset the tally for the next frame.
    std::uint32_t best = 0;
    for (const auto& entry : g_originTable) {
        if (entry.used && entry.count > best) {
            best = entry.count;
            std::memcpy(g_referenceOrigin, entry.origin, sizeof(g_referenceOrigin));
        }
    }
    if (best) g_referenceValid.store(true, std::memory_order_release);
    for (auto& entry : g_originTable) { entry.used = false; entry.count = 0; }

    // THE STEREO FALSIFIER, once every 120 presented frames.
    //
    // Every warp judgement in this phase is a judgement about geometry, and a
    // geometry report is void without the state it was made in: whether stereo
    // was armed at all, which eye this frame carried, and how far the camera
    // was actually translated for it. The alternate-frame scheme means a run
    // that silently stopped alternating looks exactly like one that is working
    // -- that failure has already cost this project a whole headset session.
    //
    // Counted in frames, not uploads, and cleared here, so the cost is one
    // formatted line every second and a half whatever the scene is doing.
    const unsigned int applied = g_worldEyeOffsetsThisFrame.exchange(0, std::memory_order_relaxed);
    g_stereoLineFrames += 1;
    g_stereoLineOffsets += applied;
    if (g_stereoLineFrames >= 120) {
        char line[520]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] STEREO: offset armed=%d, %u world uploads offset over %u frames (%.2f per "
            "frame; 0 means the world rendered MONO however good everything else looks) | last "
            "applied sign %+.0f, |e| %.4f units = %.2f mm at %.2f units/metre | half-IPD in use "
            "%.4f units | weapon.mono_eye=%d.\n",
            g_offsetArmed.load(std::memory_order_acquire) ? 1 : 0,
            g_stereoLineOffsets, g_stereoLineFrames,
            static_cast<double>(g_stereoLineOffsets) / g_stereoLineFrames,
            g_lastAppliedEyeSign.load(std::memory_order_relaxed),
            g_lastAppliedEyeUnits.load(std::memory_order_relaxed),
            g_lastAppliedEyeUnits.load(std::memory_order_relaxed) * 1000.0f /
                (SourceUnitsPerMetre() > 0.0001f ? SourceUnitsPerMetre() : 1.0f),
            SourceUnitsPerMetre(),
            g_halfInterpupillaryUnits.load(std::memory_order_relaxed),
            g_weaponMonoEye.load(std::memory_order_acquire));
        Tf2VrLog(line);
        g_stereoLineFrames = 0;
        g_stereoLineOffsets = 0;
    }
}

void EndStereoEyeOffset() { RemoveCameraUpdateTrace(); }

void AdvanceCameraUpdateTrace() {
    const auto remaining = g_framesRemaining.load(std::memory_order_acquire);
    if (!remaining) {
        if (g_gameContext && !g_enabled.load(std::memory_order_acquire) &&
            !g_offsetArmed.load(std::memory_order_acquire)) FinishTraceIfQuiescent();
        return;
    }
    if (g_framesRemaining.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        g_frameIndex.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    g_enabled.store(false, std::memory_order_release);
    g_framesRemaining.store(0, std::memory_order_release);
    FinishTraceIfQuiescent();
}

void RemoveCameraUpdateTrace() {
    g_enabled.store(false, std::memory_order_release);
    g_framesRemaining.store(0, std::memory_order_release);
    FinishTraceIfQuiescent();
}

void SetDiagnosticUploadHookHold(bool held) {
    g_diagnosticHookHold.store(held, std::memory_order_release);
}

bool GetMainSceneProjection(float matrix[16], float origin[3]) {
    if (!g_mainSceneMatrixValid.load(std::memory_order_acquire)) return false;
    if (matrix) std::memcpy(matrix, g_mainSceneMatrix, sizeof(g_mainSceneMatrix));
    if (origin) std::memcpy(origin, g_mainSceneOrigin, sizeof(g_mainSceneOrigin));
    return true;
}

bool IsViewmodelCompensationEnabled() {
    return g_compensationEnabled.load(std::memory_order_acquire);
}

void SetPinOwnsRenderPath(bool owns) {
    g_pinOwnsRenderPath.store(owns, std::memory_order_release);
}

bool TryGetMainSceneOrigin(float origin[3]) {
    if (!g_mainSceneOriginValid.load(std::memory_order_acquire)) return false;
    if (origin) std::memcpy(origin, g_mainSceneOrigin, sizeof(float) * 3);
    return true;
}

bool TryGetMainSceneAnchorOrigin(float origin[3]) {
    if (!g_mainSceneOriginValid.load(std::memory_order_acquire)) return false;
    if (origin) std::memcpy(origin, g_mainSceneOrigin, sizeof(float) * 3);
    return true;
}
bool TryGetMainSceneAnchorOffset(float offset[3]) {
    if (!g_mainSceneOriginValid.load(std::memory_order_acquire)) return false;
    if (offset) {
        offset[0] = g_mainSceneOrigin[0] - g_mainSceneBase[0];
        offset[1] = g_mainSceneOrigin[1] - g_mainSceneBase[1];
        offset[2] = g_mainSceneOrigin[2] - g_mainSceneBase[2];
    }
    return true;
}

void SetHudAnchorMode(int mode) {
    const int clamped = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
    g_hudAnchorMode.store(clamped, std::memory_order_release);
    g_hudAnchorAppliedCount.store(0, std::memory_order_release);
    g_hudAnchorNoAimCount.store(0, std::memory_order_release);
    static const char* const kLines[] = {
        "[TF2VR] HUD anchor: OFF (mode 0). The flat HUD pass is left as the engine uploads it. "
        "MEASURED 2026-09-10 run 4: with aim.cmd = 2 the engine places that pass's content -- "
        "reticle, fade, loading overlay -- where the ATTACK angles point, i.e. on the right hand.\n",
        "[TF2VR] HUD anchor: AIM (mode 1). rel = aim^T * written; drags head-frame content to the "
        "aim. Wrist ROLL is dropped on purpose -- a rolling HUD is unreadable.\n",
        "[TF2VR] HUD anchor: HEAD (mode 2). The transpose of mode 1: drags content the engine put "
        "at the aim back to the head, so the fade and the overlays sit in front of the face. The "
        "reticle comes with them. F12 flips 1 <-> 2 live; whichever CENTRES the reticle is the "
        "right sign.\n",
    };
    Tf2VrLog(kLines[clamped]);
}

// F12: flip the anchor's sign live. The rotation's sign cannot be verified
// offline, so the wearer gets both in one run: 1 <-> 2, and from 0 it goes to 2.
void ToggleHudAnchorSign() {
    const int mode = g_hudAnchorMode.load(std::memory_order_acquire);
    SetHudAnchorMode(mode == 2 ? 1 : 2);
}

int HudAnchorMode() { return g_hudAnchorMode.load(std::memory_order_acquire); }

void SetHudFovOriginMax(float units) {
    if (!(units == units) || units <= 0.0f) units = 32.0f;
    g_hudFovOriginMax.store(units, std::memory_order_release);
    char line[520]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] hud.fov_origin_max = %.1f units: how far from the origin a camera may sit and still "
        "be recognised as the HUD pass by the frustum refit. It was hard-coded at 1.0 -- about 2.5 cm "
        "-- and head rotation swings the eyes several times that, which dropped the correction out for "
        "the frames the wearer saw the whole HUD shrink on. A world camera's origin is the player's "
        "position, thousands of units, so this keeps orders of magnitude of margin. The origin-distance "
        "histogram in the HUD FOV line measures the population independently of this bound.\n",
        units);
    Tf2VrLog(line);
}

void SetHudFovMode(int mode) {
    const int clamped = (mode == 1 || mode == 2) ? mode : 0;
    g_hudFovMode.store(clamped, std::memory_order_release);
    g_hudFovMatchApplied.store(0, std::memory_order_release);
    g_hudFovMatchNoWorld.store(0, std::memory_order_release);
    switch (clamped) {
        case 1:
            Tf2VrLog("[TF2VR] HUD FOV mode 1 -- RESTORE THE UPLOAD. The near-origin HUD pass is put back "
                     "to the frustum the GAME uploaded. Measured: it is uploaded at 0.75002 x 0.52516 and "
                     "reaches the GPU at 1.42144 x 0.99528, so something of OURS widens it by 1.8952 in "
                     "both axes -- and 1.89x is exactly the marker error the wearer's own discriminators "
                     "implied. If that is the defect, the reticle and the markers should STOP DRIFTING "
                     "and the HUD may change size.\n");
            break;
        case 2:
            Tf2VrLog("[TF2VR] HUD FOV mode 2 -- FORCE THE WORLD'S. The opposite direction: the pass is "
                     "driven to the world's frustum. Last run this was a no-op because something above "
                     "us had already done it, which is the finding that produced mode 1.\n");
            break;
        default:
            Tf2VrLog("[TF2VR] HUD FOV mode 0 -- OFF, the shipped behaviour.\n");
            break;
    }
}

int HudFovMode() { return g_hudFovMode.load(std::memory_order_acquire); }

void ReportHudFovMatch() {
    if (g_hudFovMode.load(std::memory_order_acquire) == 0) return;
    static std::uint64_t nextReportMs = 0;
    const std::uint64_t nowMs = GetTickCount64();
    if (nowMs < nextReportMs) return;
    nextReportMs = nowMs + 5000;
    const auto applied = g_hudFovMatchApplied.load(std::memory_order_relaxed);
    // Snapshotted BEFORE the reset, so the window's range is the window's.
    const float sxMin = g_hudFovSxMin.exchange(1.0e9f, std::memory_order_relaxed);
    const float sxMax = g_hudFovSxMax.exchange(-1.0e9f, std::memory_order_relaxed);
    const auto nearNotVm = g_hudFovNearNotVm.exchange(0, std::memory_order_relaxed);
    const auto vmNotNear = g_hudFovVmNotNear.exchange(0, std::memory_order_relaxed);
    const auto framesRefitted = g_hudFovFramesRefitted.exchange(0, std::memory_order_relaxed);
    const std::uint32_t firstFrame = g_hudFovWindowFirstFrame.exchange(0xFFFFFFFFu,
                                                                      std::memory_order_relaxed);
    const std::uint32_t lastFrame = g_hudFovWindowLastFrame.exchange(0, std::memory_order_relaxed);
    const long long framesSeen = (firstFrame == 0xFFFFFFFFu || lastFrame < firstFrame)
                                     ? 0
                                     : static_cast<long long>(lastFrame - firstFrame) + 1;
    const long long framesMissed =
        framesSeen > 0 ? framesSeen - static_cast<long long>(framesRefitted) : 0;
    // 1400, not 760. At 760 this message was CUT OFF exactly where the origin
    // histogram began, so the numbers that were supposed to say whether the
    // widened bound admitted anything unintended never reached the log at all.
    // A truncated instrument reads as a silent one; the run could not answer the
    // question it was built to answer.
    char line[1400]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] HUD FOV mode %d: %llu uploads refitted, %llu skipped for no world frustum yet. "
        "SHRINK WITNESS -- this window: FRAMES SEEN %lld, FRAMES REFITTED %llu, FRAMES MISSED %lld. A "
        "missed frame is one where the HUD pass got no correction at all and rendered 1.8952x wider, "
        "which is the wearer's brief shrink toward the centre; this count is the measurement, and it "
        "needs no predicate to be right. Scale applied ranged %.4f to %.4f (a refit computing 1.0 "
        "changed nothing and would look like a miss). Half-matches: %llu near-origin-but-not-viewmodel, "
        "%llu viewmodel-but-not-near-origin -- these catch only exclusive-or cases, so they cannot see "
        "a frame that fails BOTH halves, which is why frames-missed exists. ORIGIN DISTANCE of every "
        "structurally-matching upload, measured independently of the bound that accepts them: <=1 %llu, "
        "<=4 %llu, <=16 %llu, <=64 %llu, <=256 %llu, beyond %llu, largest %.2f units. The bound is %.1f. "
        "The old bound was 1.0, which head movement alone exceeds -- if the bands show a population "
        "sitting between 1 and 32 that vanishes from frames-missed, that was the shrink.%s\n",
        g_hudFovMode.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(applied),
        static_cast<unsigned long long>(g_hudFovMatchNoWorld.load(std::memory_order_relaxed)),
        framesSeen, static_cast<unsigned long long>(framesRefitted), framesMissed,
        sxMax < -1.0e8f ? 0.0f : sxMin, sxMax < -1.0e8f ? 0.0f : sxMax,
        static_cast<unsigned long long>(nearNotVm), static_cast<unsigned long long>(vmNotNear),
        static_cast<unsigned long long>(g_hudFovOriginBand[0].exchange(0, std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_hudFovOriginBand[1].exchange(0, std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_hudFovOriginBand[2].exchange(0, std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_hudFovOriginBand[3].exchange(0, std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_hudFovOriginBand[4].exchange(0, std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_hudFovOriginBand[5].exchange(0, std::memory_order_relaxed)),
        g_hudFovOriginMaxSeen.exchange(0.0f, std::memory_order_relaxed),
        g_hudFovOriginMax.load(std::memory_order_relaxed),
        applied == 0 ? " ZERO REFITS -- the predicate never matched an upload, so NOTHING CHANGED and "
                       "this is not a negative result about the frustum."
                     : "");
    Tf2VrLog(line);
}

// G0b. Published by XrContext once xrEnumerateViewConfigurationViews answers.
void SetRuntimeRecommendedEyeSize(unsigned int width, unsigned int height) {
    g_recommendedEyeWidth.store(width, std::memory_order_release);
    g_recommendedEyeHeight.store(height, std::memory_order_release);
}

unsigned int RuntimeRecommendedEyeWidth() {
    return g_recommendedEyeWidth.load(std::memory_order_acquire);
}

unsigned int RuntimeRecommendedEyeHeight() {
    return g_recommendedEyeHeight.load(std::memory_order_acquire);
}

namespace {
std::atomic<float> g_headsetTanWidth{0.0f};
std::atomic<float> g_headsetTanHeight{0.0f};
}  // namespace

void SetHeadsetFovTangents(float tanWidth, float tanHeight) {
    g_headsetTanWidth.store(tanWidth, std::memory_order_release);
    g_headsetTanHeight.store(tanHeight, std::memory_order_release);
}

bool HeadsetFovTangents(float* tanWidth, float* tanHeight) {
    const float w = g_headsetTanWidth.load(std::memory_order_acquire);
    const float h = g_headsetTanHeight.load(std::memory_order_acquire);
    if (w <= 0.0f || h <= 0.0f) return false;
    if (tanWidth) *tanWidth = w;
    if (tanHeight) *tanHeight = h;
    return true;
}

// ---------------------------------------------------------------------------
// WHICH HEADSET IS ON. See the note in the header for why the NAME is a label
// and the FINGERPRINT is the identity.
// ---------------------------------------------------------------------------
namespace {
char g_headsetSystemName[128] = "<not reported>";
char g_headsetRuntimeName[128] = "<not reported>";
char g_headsetFingerprint[256] = "";
}  // namespace

void SetHeadsetIdentity(const char* runtimeName, const char* systemName) {
    if (runtimeName) {
        std::snprintf(g_headsetRuntimeName, sizeof(g_headsetRuntimeName), "%s", runtimeName);
    }
    if (systemName) {
        std::snprintf(g_headsetSystemName, sizeof(g_headsetSystemName), "%s", systemName);
    }
}

const char* HeadsetSystemName() { return g_headsetSystemName; }
const char* HeadsetRuntimeName() { return g_headsetRuntimeName; }

const char* HeadsetFingerprint() {
    const unsigned int w = RuntimeRecommendedEyeWidth();
    const unsigned int h = RuntimeRecommendedEyeHeight();
    float tanW = 0.0f, tanH = 0.0f;
    const bool haveFov = HeadsetFovTangents(&tanW, &tanH) && tanH > 0.0001f;
    // Empty until the geometry is known, so a caller cannot key anything on a
    // half-built identity and then find it changing mid-session.
    if (!w || !h || !haveFov) { g_headsetFingerprint[0] = '\0'; return g_headsetFingerprint; }
    // The tangents are rounded to three places on purpose: a runtime that
    // recomputes the FOV per session with a hair of float noise must still match
    // itself next launch, or every launch looks like a new headset.
    std::snprintf(g_headsetFingerprint, sizeof(g_headsetFingerprint), "%s|%s|%ux%u|%.3fx%.3f",
                  g_headsetSystemName, g_headsetRuntimeName, w, h,
                  static_cast<double>(tanW), static_cast<double>(tanH));
    return g_headsetFingerprint;
}

// The per-family upload tally, live on every run regardless of the shear.
// Order: main scene, effects, viewmodel, other.
void ReadUploadFamilyTally(unsigned long long out[4]) {
    for (int i = 0; i < 4; ++i) out[i] = g_uploadFamilyTally[i].load(std::memory_order_relaxed);
}

// The lean detach's arm state and its PROOF OF WORK, for the census. Armed and
// applying are different claims: the gate also requires a live camera-position
// write and a viewmodel-family pass, so a strength of 1.00 with a flat count is
// a disarmed gate rather than a working correction.
float ViewmodelDetachLeanStrength() { return g_detachLean.load(std::memory_order_acquire); }
unsigned long long ViewmodelDetachLeanApplied() {
    return g_detachLeanApplied.load(std::memory_order_relaxed);
}

// Published-pose read for other units (the lock-ring correction): the pose the
// compositor will reproject to, [pitch, yaw, roll] base and applied. Wraps the
// file-local seqlock reader so its linkage stays where it was.
bool ReadPublishedHeadAnglesForHud(float* base, float* applied, std::uint32_t* generation) {
    return ReadPublishedHeadAngles(base, applied, generation);
}
