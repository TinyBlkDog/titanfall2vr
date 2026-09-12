#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11Texture2D;
struct ID3D11DeviceContext;

// Thread 1 stage 1: a passive, bounded function-entry trace of
// ID3D11DeviceContext::UpdateSubresource, filtered to 576-byte constant-buffer
// uploads on the verified game immediate context.  It records only a hash and
// a few reflected fields per call and always forwards the game's own arguments
// unmodified: no resource is created, substituted, rebound, or mapped.
void BeginCameraUpdateTrace(ID3D11Device* device);
// Must be called once per presented frame.  The trace is budgeted in RENDER
// frames, counted here, not in plugin frames: RunFrame is dispatched faster
// than the render loop, so a plugin-frame budget expired inside a single frame
// and captured only its tail.
void NotifyCameraUpdateFrameBoundary();
// Bounded per-upload trace of the viewmodel correction: 4 whole frames of
// inputs and applied rotation. Pair it with the synthetic pose, where every
// number must come out identical.
void BeginCorrectionTrace();
void BeginViewportTrace();
// Thread 1 stage 2: a bounded, deliberately obvious world-camera shift applied
// by adding an offset to c_cameraOrigin (0x04) of the perspective, world-origin
// camera uploads only.  The game's own source bytes are never written: a local
// copy is passed as pSrcData to the real, original UpdateSubresource.  No
// resource is created, substituted, rebound, or mapped, and no GPU command is
// issued from inside the hook.
void BeginCameraOriginOffsetTest(ID3D11Device* device);
// Same delivery path, but composes a true eye translation into
// c_cameraRelativeToClip: row3 -= e.x*row0 + e.y*row1 + e.z*row2.  This is the
// form a per-eye IPD offset ultimately needs.
void BeginMatrixEyeTranslationTest(ID3D11Device* device);
// Bounded identification probe for the viewmodel. Offsets c_cameraOrigin on the
// near-origin perspective uploads (HOME trace #28) and nothing else, so if the
// gun moves and the world does not, that family is the viewmodel.
void BeginViewmodelProbe(ID3D11Device* device);
// Cancels the head rotation on the body/weapon pass so it stays with the aim
// while the camera turns. Follows head tracking rather than a frame budget, and
// composes with per-eye stereo rather than replacing it.
void SetViewmodelCompensationEnabled(ID3D11Device* device, bool enabled);
// F3 asserts this is OFF: P0 measured the gun family as already carrying the
// world orientation, so the correction would double-rotate a world-placed rig.
bool IsViewmodelCompensationEnabled();
// Must be called once per presented frame: this is where the upload hook is
// actually installed, because installing it at arm time resolved the wrong
// function.
void TickViewmodelCompensation(ID3D11Device* device);
// Per-eye IPD offset along the (normalised) camera right axis.  sign is -1 for
// the left eye and +1 for the right.  Driven by the Present hook so that the
// captured frame and the eye it was rendered for cannot drift apart.
// frames of 0 keeps the default short test budget; pass a larger value for a
// headset session.  The offset always self-reverts eventually.
bool BeginStereoEyeOffset(ID3D11Device* device, float sign, unsigned int frames = 0);
// Installs the UpdateSubresource and RSSetViewports detours WITHOUT arming any
// correction: both forward unchanged until something asks them not to. Already
// idempotent -- it returns true immediately once the context is captured.
//
// It is exposed because the main-scene frustum and viewport recorders hang off
// these detours and are deliberately UNGATED, so anything that wants to MEASURE
// the render needs the detours in without arming anything. Today they only go in
// when stereo arms, which means a flat run measures nothing at all.
bool EnsureDetoursInstalled(ID3D11Device* device);

// THE WORLD PASS WITHOUT A CAMERA UPLOAD.
//
// GetMainSceneViewport below only latches during a recognised main-scene camera
// upload, and three flat sessions have now produced none -- so it reports
// nothing at all in a flat run even while the world renders. This is the same
// question answered from the viewport stream alone: the world pass is the
// rectangle the game sets FAR more often than any other (measured in the
// archive at 4032x3648: 2281 sets against 359 for the next). Reset it, let a
// window of frames pass, read it back.
// KEEPS THE DETOURS INSTALLED FOR A READER, and this is why a flat instrument
// could not exist before.
//
// FinishTraceIfQuiescent tears the camera detours down unless something owns
// them, and every owner it recognises is a MUTATION: the viewmodel compensation,
// a bounded offset test, an upload trace. Not even the self-armed 120-frame
// viewport trace counts. So anything that only wants to READ the frustum and
// viewport recorders was uninstalled on the next plugin frame -- measured three
// times as "install, four invocations, detour NOT INSTALLED, every counter
// frozen". Arms nothing; both detours still forward unchanged.
void HoldCameraDetours(bool hold);

void ResetGameViewportTally();
bool DominantGameViewport(float* width, float* height, unsigned long long* count,
                          unsigned long long* total);

// WHICH STAGE OF THE UPLOAD LADDER DIES. Every one of these already existed as
// a counter or was one increment away; what did not exist was any way to PRINT
// them without the viewmodel correction armed, which a flat run never does.
//
// Read them in order. The first zero is the answer:
//   invocations 0      -> the detour is not being called at all
//   onOurContext 0     -> called, but never on the context we captured
//   cameraSized 0      -> our context uploads, but none is camera-sized
//   worldPerspective 0 -> camera-sized uploads that are not world perspective
//   mainScene 0        -> world cameras whose near plane is not the main scene's,
//                         and lastNearPlane vs expectedNearPlane says by how much
struct UploadLadderCounters {
    bool installed;
    unsigned long long invocations;
    unsigned long long onOurContext;
    unsigned long long cameraSized;
    unsigned long long worldPerspective;
    unsigned long long mainScene;
    float lastNearPlane;
    float expectedNearPlane;
};
void ReadUploadLadderCounters(UploadLadderCounters* out);
void SetStereoEyeSign(float sign);
// Called once per presented frame so the dominant player-camera origin can be
// re-established; this is what keeps the 3D skybox out of the eye offset.
void NotifyStereoFrameBoundary();
void EndStereoEyeOffset();
void AdvanceCameraUpdateTrace();
void RemoveCameraUpdateTrace();

// --- Two shipped assumptions from Task 01b, made switchable rather than
// --- settled by argument.  Both default to the behaviour already verified in
// --- the headset, so loading the config changes nothing until one is flipped.

// The "lower camera" (one draw, 60 units below the player) shares m[11] == -1
// with the body/weapon pass, so the viewmodel correction currently rotates it
// too.  Enabling this excludes it, by requiring the upload's origin to match
// the most recent main-scene origin.  OFF by default: excluding it is only
// obviously right if that pass is not part of the body render, which is exactly
// what has never been checked.
void SetViewmodelLowerCameraExcluded(bool excluded);
void SetViewmodelIncludeNearOrigin(bool included);
// One angle pair per frame, pinned at the main-scene upload, so every corrected
// upload rotates by the same Rel the world was projected with.
void SetViewmodelFrameLockedAngles(bool locked);
// Cancel the pose the compositor reprojects to (published every frame) rather
// than the engine's written angles (held on roughly every fourth frame).
void SetViewmodelDisplayPoseCorrection(bool useDisplayPose);
// -1 off, 0 left, 1 right, 2 CENTRE. The weapon family is drawn from one fixed
// viewpoint while the world keeps full stereo. Centre is the correct one: the
// round travels along the centre axis, so only there do sight and bullet agree.
void SetWeaponMonoEye(int eye);
// Multiplies the weapon's rendered size on top of the world-FOV match.
void SetWeaponSize(float size);
float WeaponSize();
// Full stereo normally, centre only while zoomed. Depth when it helps,
// alignment when it matters.
void SetWeaponMonoOnlyWhenZoomed(bool onlyWhenZoomed);
// Fraction of the resting frustum below which the world counts as zoomed.
void SetZoomThreshold(float fraction);
// 0 first main-scene pass, 1 last, 2 the one whose origin is the world's.
void SetViewmodelPinMode(int mode);
// Suppress the game's one-frame view-height dropout on a step up.
// Take the lean back off the weapon, so the body stays put while the head moves.
void SetViewmodelDetachLean(float strength);

// TASK 05 STEP 4 -- pin the gun to the right controller.
//
// Transforms ONE pass of the viewmodel family by the right controller's rotation,
// relative to a reference captured on arm. Which pass is selected by
// CycleWeaponPinSlot.
//
// ORIGIN DOES NOT IDENTIFY THE GUN. The first version selected the pass by
// "viewmodel near plane plus origin (0,0,0)", on the strength of HEADSET-ISSUES
// saying the gun's own pass renders there. Measured in the headset: the gun did
// not move and the HUD did, exactly opposite to the hand. 00-SHARED-CONTEXT's
// closed list already recorded the reason -- the near-origin perspective family
// is the world-anchored HUD -- and that entry deserved more weight than the other.
// The four viewmodel passes are separated by their four FIXED FOVs instead, which
// is what the slot table does.
//
// ROTATION ONLY at present, and rotation of a camera pass, so the content turns
// about the eye rather than the wrist. It moves whatever is in that pass rigidly
// and cannot articulate an arm. Whether that is good enough to build on, or
// whether the engine's evaluated skeleton has to be driven instead, is what the
// slot cycling is meant to answer.
void SetWeaponPinnedToController(bool pinned);
bool IsWeaponPinnedToController();
// Re-captures the reference from the controller's current pose, so the gun can be
// re-zeroed without disarming.
void RecentreWeaponPin();
void SetStepGuard(bool enabled);
void SetStepGuardThreshold(float units);
bool IsViewmodelLowerCameraExcluded();

// On the corrected pass the per-eye translation is taken from the matrix AFTER
// the correction, so eye separation follows the body's right axis.  Enabling
// this takes it from the pre-correction (head-aligned) matrix instead.  OFF by
// default; see the analysis in the Task 01b follow-up notes for why the shipped
// choice is probably the better one and what measurement decides it.
void SetViewmodelEyeBasisPreRotation(bool preRotation);
bool IsViewmodelEyeBasisPreRotation();

// Half the interpupillary distance in Source units (~1 inch each).  Config
// tunable so an IPD sweep does not need a rebuild.
void SetHalfInterpupillaryUnits(float units);
// view.world_scale. 1.00 leaves the runtime-derived eye separation alone;
// larger divides it, which makes the world feel bigger.
void SetWorldScale(float scale);
float WorldScale();
// Marks the value above as explicitly chosen, so the runtime's own IPD does not
// overwrite it.
void MarkHalfInterpupillaryUnitsExplicit();
// The runtime knows the wearer's real IPD; use it unless the config overrode it.
void SetRuntimeIpdMetres(float metres);

// WHICH HEADSET IS ON, from the runtime itself.
//
// xrGetSystemProperties.systemName and xrGetInstanceProperties.runtimeName.
// XrContext already read both and kept them for its own banner; nothing else
// could see them, so the config menu could not name the headset it was
// configuring and the wearer had to infer it from a pixel count.
//
// TREAT THE NAME AS A LABEL, NOT AN IDENTITY. A streaming runtime can report a
// generic or wrong model -- this project has a session logging system "Meta
// Quest 3" while the wearer was describing a different headset. Anything that
// must TELL HEADSETS APART keys on HeadsetFingerprint() below, which folds in
// the geometry and does not care whether the name is honest.
void SetHeadsetIdentity(const char* runtimeName, const char* systemName);
const char* HeadsetSystemName();
const char* HeadsetRuntimeName();

// A stable key for "this headset on this runtime", from the name PLUS the
// recommended per-eye size PLUS the FULL FOV tangent extents (right-left,
// up-down -- NOT half-angles; a hand-written cache line using halves will not
// match and shows up as a phantom second headset).

// the same name are still told apart and one that reports nonsense is still
// matched to itself next launch. Empty until XR has reported its geometry.
const char* HeadsetFingerprint();

// The half-tangents of the MAIN SCENE's frustum, from the most recent
// near-plane -7 upload: halfTanX = 1/|r0|, halfTanY = 1/|r1|.  This is what the
// projection layer must declare, because it describes the image we actually
// submit.  Returns false until a main-scene upload has been seen.
//
// Note this is the main scene's frustum specifically, not the body/weapon
// pass's: those are different, and the difference grows with the FOV setting.
// At slider 110 the world renders 123.7 x 92.9 degrees while the weapon renders
// 86.1 x 55.4, which is why the two families must never be conflated here.
bool GetMainSceneHalfTangents(float& halfTanX, float& halfTanY);
// Age in ms of the measurement the two functions above return, and the number of
// times it has been written. The pair is never invalidated, so a caller cannot
// tell a fresh frustum from a latched one without this.
std::uint64_t MainSceneFrustumAgeMs();
std::uint64_t MainSceneFrustumWrites();
// The frustum the game actually RENDERED, with no ADS magnification applied.
// Anything positioning something inside the image -- the reticle above all --
// must use this; only the projection layer uses the declared one. Mixing them
// put the reticle at 1/magnification of where the round lands.
bool GetMainSceneRenderedHalfTangents(float& halfTanX, float& halfTanY);

// ---------------------------------------------------------------------------
// W2 -- THE LENS SHEAR. `xr.lens_shear`, default ON.
// REPLAN-2026-08-28-MECHANISM-B section 4, and PLAN-ASYMMETRIC-FOV V3.
//
// The Quest 3's per-eye optical axis points 13.02 degrees BELOW forward, while
// the engine renders a frustum centred on forward. It renders a 103.71 degree
// vertical -- nearly the whole 99 degrees the lens spans -- so nothing needs to
// be widened. It is only aimed wrong, and 3.15 degrees fall off the bottom of
// every frame as a result.
//
// The fix adds a multiple of the clip-w row to the clip-y row, which TRANSLATES
// the frustum and leaves its span alone. Two properties follow, and both matter:
//
//   * FULL COVERAGE COSTS NOTHING. Same span, same draw calls. The plan feared
//     opening the vertical frustum onto the axis that once cost 19 fps; a shear
//     does not open it at all.
//   * IT NEEDS NO FAMILY HEURISTICS. It is a uniform NDC translation, so every
//     perspective pass takes the same edit and the image's internal composition
//     is untouched. `fit_horizontal` defaults OFF because SCALING one family's
//     row left the effects and viewmodel behind -- "a heuristic set that has to
//     be exhaustive to be correct will not stay correct". Nothing here has to
//     know which family it is looking at.
//
// HORIZONTAL IS DELIBERATELY NOT SHEARED. The Quest 3's horizontal asymmetry is
// larger (15.03 deg per eye, mirrored) but the symmetric render already covers
// that lens to within 0.006 degrees, so shearing it would buy no coverage while
// changing stereo -- a second variable in a run that is testing the first.
// ---------------------------------------------------------------------------

// The runtime's own per-eye vertical extents, in tangent space, from
// xrLocateViews. Both eyes report the same pair on every headset measured, so
// one pair drives the render; the DECLARATION is still per-eye and reads the
// view directly. Ignored entirely if the pair is not a sane frustum.
void SetHeadsetLensVerticalExtents(float tanUp, float tanDown);

// `xr.lens_shear`. Bound to CTRL+F1 so the wearer can A/B it without a relaunch.
void SetLensShearEnabled(bool enabled);
bool LensShearEnabled();

// The NDC offset the last main-scene upload was ACTUALLY patched with -- read
// back out of the decision, never recomputed at the layer.
//
// The layer has to declare the frustum that was RENDERED, and recomputing the
// shear there would declare the one we MEANT to render. Those differ whenever
// the cap trips, whenever the lens is not known yet, and every frame between
// the toggle flipping and the next upload. Publishing what was applied is the
// only version that cannot silently disagree with the pixels.
float LensShearAppliedNdcY();
// The zoom classification, READ WITHOUT DRIVING IT. WorldIsZoomed() stores back
// into the latch that supplies its own hysteresis, so it is an edge detector
// and calling it from a second clock changes the answer the first one gets.
bool WorldZoomLatched();
// The two candidate resting references for "how much has ADS narrowed the
// frustum", side by side so they can be compared before either is trusted.
// RestingWorldHalfTanX is the shipped ALL-TIME maximum, which one transient
// poisons for the life of the process. PlateauRestingWorldHalfTanX is a
// 30-second sliding maximum over frame-to-frame-stable samples only, so it ages
// out and the ADS animation's own ramp cannot enter it. Nothing consumes the
// plateau value yet; it exists to be measured first.
float RestingWorldHalfTanX();
float PlateauRestingWorldHalfTanX();
float PlateauRestingWorldHalfTanY();
unsigned long long PlateauRestStableSamples();

// C1 -- ads.max_magnification. How far ADS is allowed to narrow the frustum.
//
// 0 (the default) passes the game's own behaviour through unchanged. 1.0
// suppresses the narrowing entirely and gives the periphery back. Anything in
// between is the trade: some sharpness, most of the periphery.
//
// It is NOT a clamp. xr.match_headset_fov clamped to an absolute value on every
// upload including ADS, broke the zoom, and ships OFF for that measured reason.
// This compares the current frustum with the resting plateau and writes ONLY
// when the game has magnified past the cap -- so at rest, where the ratio is
// 1.0, no upload is touched at all and the 12% rest overshoot cannot move.
void SetAdsMaxMagnification(float cap);
float AdsMaxMagnification();
// ads.optic_passthrough. A weapon whose OWN declared zoom reaches this is left
// entirely alone: a 4x scope should magnify, an iron sight should not, and the
// weapon says which it is. ONE dial rather than a per-weapon table -- a gun this
// build has never seen classifies itself by the same rule.
void SetAdsOpticPassthrough(float magnification);
float AdsOpticPassthrough();
// The falsifier, in counts: how many main-scene uploads were widened and how
// many were left untouched, plus the magnification last measured. "Untouched"
// means no rows were written, not "scaled by one".
void ReadAdsCapCounters(unsigned long long* widened, unsigned long long* untouched,
                        float* lastMagnification);
// The rectangle the world pass was actually rendered into. At a 16:9 target it
// is the whole buffer; at a taller one the game clamps it -- measured 4032x2520
// inside a 4032x3648 target -- and submitting the whole buffer while declaring
// this rectangle's frustum is what warps the image.
bool GetMainSceneViewport(float& x, float& y, float& width, float& height);
// xr.viewport_full. The game clamps the world pass to a shorter viewport than
// the render target -- measured 4032x2520 inside 4032x3648 -- and blacks out the
// rest. This widens it and corrects the camera's vertical term to match, which
// only makes sense as a pair: widening alone stretches.
void SetViewportFull(bool full);

// xr.declare_rendered_fov, DEFAULT ON. Publishes the main-scene half-tangents
// the game was HANDED rather than the ones it asked for, so the projection
// layer's FOV describes the image it is attached to. Off reproduces
// sharp-2026-08-23 exactly, which is what makes it an A/B.
void SetDeclareRenderedFov(bool declare);
bool IsDeclareRenderedFovArmed();

// xr.fit_horizontal, DEFAULT OFF. Narrows row0 of the MAIN SCENE to the headset's
// tangent ratio. Off because it reaches only that one pass: the effects and
// viewmodel families keep the game's frustum and mis-register against a world
// that no longer shares it. Kept switchable so the two are comparable.
void SetFitHorizontal(bool fit);
bool IsFitHorizontalArmed();
// xr.viewport_full, for the bare-key A/B against the known-good state.
bool IsViewportFullArmed();
// G0b: the runtime's recommended per-eye size, for the aspect-matched buffer.
void SetRuntimeRecommendedEyeSize(unsigned int width, unsigned int height);
void PublishBackbufferSize(float width, float height);
// hud2d.head_anchor, DEFAULT OFF. Shifts the pixel-ortho pass per frame so the
// aim anchor lands at the pass centre: reticle, overlays and fades in front of
// the head instead of at the right hand. Pass-level: the reticle moves too.
void SetHud2dHeadAnchor(bool on);
// xr.world_rect_gate, DEFAULT ON. A viewport under half the render target in
// either axis is refused as the world pass and the previously accepted
// rectangle stands. The opening video paired 32x32 with a main-scene upload
// four runs in a row and the compositor magnified it to fill the eye. Pure
// predicate in world_rect_gate.h, checked offline by
// tools/world-rect-gate-check.cpp.
void SetWorldRectGate(bool gate);
// F5: "the wearer sees the floating rectangle NOW". Read-only; the next pinned
// upload prints the weapon-pin digest (which passes the pin is rotating).
void MarkWeaponPinDigest();
// rui.hud_anchor: 0 off, 1 aim, 2 aim->head. F12 flips 1 <-> 2 live.
void ToggleHudAnchorSign();
// Published by the XR side once the view configuration is known, so the main
// scene can be rendered at the frustum the display actually shows.
void SetHeadsetHalfTangents(float halfTanX, float halfTanY);
void SetMatchHeadsetFov(bool match);

// Forces the body/weapon pass onto the main scene's frustum.  The weapon has
// its own fixed FOV, so when the whole image is displayed through one frustum
// -- which both the projection layer and the quad do -- it appears magnified
// and wrongly foreshortened.  Off by default; it changes shipped rendering.
// Skip detouring the shared d3d11 UpdateSubresource IMPLEMENTATION, leaving only
// the thunks hooked. That implementation is used by every D3D11 context in the
// process, including the OpenXR runtime compositor, so hooking it taxes the
// runtime as well as the game.
void SetHookImplementationAllowed(bool allowed);
// Install the detours but make their bodies do nothing, to separate the cost of
// our work from the cost of the detour existing at all.
void SetHookPassThrough(bool on);
// Compute the patch as usual but upload the game's own pointer, so the cost of
// substituting the upload source can be measured with the arithmetic held
// constant. Diagnostic only: the correction has no effect while this is set.
void SetUploadNoSubstitute(bool on);
// Upload a byte-identical copy: same buffer, same substituted pointer, no
// arithmetic. Separates the cost of the CONTENTS we write from the cost of
// substituting the upload source at all. Diagnostic only.
void SetUploadIdentityCopy(bool on);

// How much of the hook body runs, as nested levels: 0 forwards immediately,
// 1 adds the counter blocks, 2 adds all classification and arithmetic while
// still uploading the game's pointer, 3 substitutes a byte-identical copy,
// 4 patches for real. The first level that is slow is the one that added the
// cost.
void SetHookBodyLevel(int level);
int HookBodyLevel();
// Walk the levels automatically, one per XR submit-timing window, so a single
// headset run reports xrEndFrame at every level instead of one bit per run.
void SetHookBodyLevelSweep(bool on);
// Advances the sweep. Returns false when the sweep is off. Called from the
// submit-timing report so each window is attributed to exactly one level.
bool AdvanceHookBodyLevelSweep();
// Camera-sized uploads seen since install. Reported with each sweep window so a
// window that measured real work can be told from one spent in a menu.
unsigned long long HookCameraSizedUploads();
// Camera-sized uploads ISSUED ON THE RE-ENTERING THREAD inside a tagged pass.
// 0 = outside, 1 = the engine's own scene draw, 2 = the nested one. A
// before/after delta around a call cannot attribute asynchronous work to it;
// this can. See scene_reentry.h's t_passTag.
unsigned long long CameraUploadsForPass(int tag);

// M1's per-span draw tally, armed only by the render-target census.
//
// Deliberately NOT the shipped g_drawIndexedCount: that one only moves while a
// bounded camera trace is running, so it reads zero for a whole census run and
// every per-span draw count taken from it would be a fake zero. Only
// DrawIndexed is counted -- a fullscreen post pass issued with Draw() reads 0
// here, and that is an absence of counting, not an absence of work.
// M3. stereo.batch2_ipd -- the eye translation applied to batch 2 only, keyed
// on the render-thread discriminator. 0 is the POSITIVE CONTROL (byte-identical
// origins). F3 toggles it without disturbing the census.
void SetBatch2Ipd(float ipd);
void ToggleBatch2Offset();
void SetBatch2IpdMode(int mode);
// M5. One implementation of the eye-translation algebra, shared by the upload
// patch and the pre-draw write, so the two can never disagree by a sign or a
// scale. Returns false when the basis cannot be normalised.
bool ApplyEyeTranslationInPlace(unsigned char* bytes, float ipd);
// The buffer the engine uploads the world camera into, and the last bytes it put
// there -- captured where the uploads happen, so the pre-draw write can reissue
// them offset immediately before batch 2 s scene draws.
bool TakeWorldCameraSnapshot(void** bufferOut, unsigned char* bytesOut, unsigned cap);
unsigned WorldCameraBufferBytes();
// Suppresses the upload patch for the duration of M5 s own reissue, so the write
// is not offset twice.
void SetPreDrawWriteInProgress(bool on);
// For the capture filenames: the pair must say which arm produced it.
bool Batch2OffsetArmed();
// Per Present interval, from the render-target census, which is the only thing
// that knows whether the interval that just closed was doubled. Prints the
// per-frame distinct-origin census and resets it.
void ReportBatchCameraOrigins(bool doubledInterval);

void SetGameDrawIndexedTally(bool on);
// The verified game immediate context and its per-object dispatch table, for a
// slot swap installed from the per-frame tick rather than from inside
// InstallDetours. Null until a context has been verified. See the note at the
// definition: hanging the census off InstallDetours would have made it depend
// on nothing else having installed the detours first.
ID3D11DeviceContext* VerifiedGameContextForSlotSwap(void*** vtableOut);
bool GameDrawIndexedTallyArmed();
unsigned long long GameDrawIndexedTally();

// SAME-FRAME STEREO. Eye 0, copied out of the scene target at the mid-frame
// boundary -- the one instant it exists before batch 2 draws over it. Null when
// this frame produced no capture. Reset once per Present.
// True once the second scene pass of a doubled frame has begun, on the render
// thread. This is what keys the per-eye camera offset -- the thread-local the
// design specified cannot, because the uploads are not on that thread.
bool SameFrameInSecondPass();
// ONE doubled frame, both eyes to disk. The proof standard, offline: near
// geometry must shift horizontally between them, the sky must not.
bool SameFrameEyePairWanted();
void RequestSameFrameEyePair();
void ClearSameFrameEyePairWanted();
ID3D11Texture2D* SameFrameEye0Texture();
void ResetSameFrameCapture();
void SameFrameCaptureCounts(unsigned long long* captures, unsigned long long* failures,
                           int* scenePassesLastFrame);

void SetViewmodelMatchWorldFov(bool match);
bool IsViewmodelMatchWorldFov();
bool IsMatchHeadsetFov();

// Multiplies the half IPD, for settling world scale by eye in the headset.
// Marks the value explicit, so the runtime's own IPD stops overriding it.
void NudgeHalfInterpupillary(float factor);

// Source units per real metre, derived from the half IPD currently in use
// against the runtime's reported eye separation.  Positional head tracking uses
// this so head movement and eye separation share one world scale: tuning the
// IPD until the world feels right also makes a lean move the right distance.
// Falls back to 1 unit per inch before the runtime has reported anything.
float HalfInterpupillaryUnits();
float SourceUnitsPerMetre();

// Cycles which viewmodel pass the pin drives: 0..n-1 for one pass each, then -1
// for all of them at once. Origin does NOT identify the gun -- the near-origin
// pass is the world-anchored HUD, which is what the first attempt moved -- so the
// passes are tabulated by their own fixed FOVs and stepped through in the headset
// until the gun is the thing that moves.
void CycleWeaponPinSlot();

// Keeps the UpdateSubresource hook installed for a read-only diagnostic on a
// run where nothing else armed it. The three things that normally install it --
// the offset test, the compensation, stereo -- are all off on a flat run, and
// F1 already lost a measurement to exactly that.
void SetDiagnosticUploadHookHold(bool held);

// The world pass matrix and eye origin, captured together at the main-scene
// upload so they can never come from different frames. False until one has been
// seen this session. This is what F3 predicts the pinned rig's screen position
// FROM.
bool GetMainSceneProjection(float matrix[16], float origin[3]);

// Declares that the placement pin owns the render path, which opens the upload
// hook classification gate so the world-FOV match can actually run. Without it
// a flat run never sees a main-scene upload and the match is inert.
void SetPinOwnsRenderPath(bool owns);

// THE ORIGIN THE VIEW IS ACTUALLY BUILT FROM, at the first main-scene pass.
//
// Not the same thing as camera_hook's g_cameraBasePos*, and during a step not
// even close: a single frame measured the base moving -3.79 to 700.03 while
// this moved -2.33 to 700.78. Anything that has to sit at a fixed offset from
// what the player SEES has to use this one, or it rides a different camera and
// bounces relative to the view every time the two diverge.
bool TryGetMainSceneOrigin(float origin[3]);

// The standing difference between the view origin and the camera base, sampled
// at the same instant so it is a clean offset rather than two clocks subtracted.
//
// Adding it to a FRESH base gives the view's level with the base's update rate.
// Anchoring straight to the view origin instead costs a frame of lag, because
// it is captured once per frame on the first main-scene pass while the base is
// latched on every render pass -- which is exactly the movement lag and stutter
// that anchoring to it reintroduced.
bool TryGetMainSceneAnchorOffset(float offset[3]);
// The published rendered origin of the frame's nearest-to-base main-scene
// pass: one value per frame, stable between frame boundaries. The camera base
// bits are rewritten by the camera hook on every engine camera commit and flip
// within a frame wherever several passes run.
bool TryGetMainSceneAnchorOrigin(float origin[3]);

// ---------------------------------------------------------------------------
// A WORLD-ANCHORED HUD CONTROL LIVED HERE AND IS REMOVED.
//
// It shifted and scaled the near-origin PERSPECTIVE family, on the 2026-08-16
// lead that a translation there "removed every HUD element except the
// reticle". Measured in the headset that lead does not hold for this build:
// 30 units of shift and a 25 per cent zoom, with 1653 uploads provably
// adjusted, moved nothing the wearer could see.
//
// The HUD content that CAN be reached is in the pixel-coordinate orthographic
// pass declared below. Do not rebuild the perspective one without new evidence.

// Read-only census of the distinct camera-sized upload families -- the
// instrument that found the pixel-ortho pass, and the one to reach for when the
// next "where is this drawn" question comes up.
void SetUploadCensusEnabled(bool enabled);
void ReportUploadCensus();
// Prints the census on its own clock whenever it is enabled. NOT gated on any
// calibration mode: a read-only diagnostic that only works while a UI happens
// to be in the right state costs a run to discover and looks broken rather than
// unarmed.
void AdvanceUploadCensus();

// THE SCREEN-SPACE 2D PASS -- where the reticle is drawn. CONFIRMED, and the
// controls below are MEASURED CLOSED: see ScaleProjectionLinear in the .cpp.
// The reticle scales but travels toward the pixel-space origin as it does,
// because its position lives in its vertices rather than in the matrix. Left
// inert (zoom 1.0, shifts 0) as the record of a dead end; the size lever is
// 0x54B160 on the widget, reached from the draw at client.dll+0x15EF90.
//
// An orthographic family in PIXEL coordinates (row lengths 2/4032 and 2/2268),
// distinct from the near-identity orthographic family the removed knob caught.
// `IsUiCamera` cannot select it -- that predicate wants both row lengths within
// 10% of 1.0, and these are three orders of magnitude away, which is exactly
// why the earlier experiment hit full-screen effects and reported the reticle
// untouched.
//
// Shifts are in PIXELS of the render, not world units, so this target keeps its
// own values and its own step size.
void NudgeHud2dPlacement(float x, float y, float zoomFactor);
void SetHud2dPlacement(float x, float y, float zoom);
void ReadHud2dPlacement(float* x, float* y, float* zoom);
unsigned long long Hud2dAdjustedUploadCount();

// ---- PLAN-CURRENT F1: THE NON-RETICLE HUD, IN THE 64-BYTE FAMILY ----------
//
// R1 (2026-08-20) dumped the 64-byte constant buffers. The result is as sharp
// as this project gets: across 115 census prints and 8,245 uploads there is
// EXACTLY ONE distinct matrix, unchanged from the first print to the last, and
// its count equals the whole family. One slot of twelve was used, so the table
// never saturated and nothing was dropped.
//
//   [ 0.0005  0       0        0      ]      2/4032 = 0.000496  -> prints 0.0005
//   [ 0       0.0009  0        0      ]      2/2268 = 0.000882  -> prints 0.0009
//   [ 0       0       0.2222  -0.2222 ]
//   [ 0       0       0        1      ]
//
// m15 = 1, no perspective row, diagonal linear part at the render's own width
// and height: an ORTHOGRAPHIC PROJECTION IN PIXEL COORDINATES. Branch A.
//
// WHY THIS IS THE HUD, AND WHY IT IS NOT THE RETICLE. The 576-byte camera
// family already contains a pixel-ortho pass with the SAME 2/W and 2/H linear
// part -- the one hud2d.zoom drives at 0.262. The wearer's own reports separate
// the two, and no run has to be spent re-deriving it: that zoom visibly shrank
// the reticle AND the pause menu, and visibly did NOT shrink the main HUD. So
// the reticle rides the 576-byte pass and the main HUD does not; the 64-byte
// matrix is the only other pixel-ortho transform in the frame. Scaling it
// therefore CANNOT double-scale the reticle -- different buffer, different
// consumer, and the separation is an observation rather than an assumption.
//
// WHY THE REMOVED KNOB'S FAILURE CANNOT REPEAT. That knob scaled the
// near-identity ortho family (|r| = 1.0), which is full-screen effects, and it
// damaged the image. Full-screen effects are 576 bytes. This write is gated on
// ByteWidth == 64 first, so the family that failed is excluded by shape before
// any value test runs.
//
// THE ANCHOR, which is the one thing R1 could not settle. The anchored-scale
// algebra is the reticle's, unchanged:
//
//     clip' = s*M_lin*v + (1-s)*M_lin*P + m03
//
// with P the screen centre in the matrix's OWN input units. Which units those
// are depends on where the vertices sit, and the matrix answers it by what it
// declines to do: m23 = -0.2222 shows this matrix does carry a column-3 offset
// where it needs one, and m03 = m13 = 0.0000 exactly says x and y need none.
// A corner-origin pixel input (0..W) through m00 = 2/W would land in [0,2] and
// need a -1 that is measurably absent. So the input is already CENTRED on the
// screen centre, P = (0,0), the compensation term vanishes, and a pure linear
// scale is anchored at the centre for free.
//
// That is a reasoned default, not a measured one, so it is DIALLABLE rather
// than baked: if F1 shows the HUD collapsing toward a corner instead of the
// centre, the vertices are corner-origin after all and the anchor becomes
// (W/2, H/2) = (2016, 1134) from the INI, with no rebuild. Both outcomes are
// one bounded pulse away and neither costs a second build.
//
// Inert at 1.0, which is the default: nothing arms on load.
// The anchor in the matrix's own input units. (0,0) is the centred reading
// above; (2016, 1134) is the corner-origin reading. Live from the INI.
// F1's bounded pulse: scale for a fixed window, then restore, so one run is a
// self-contained A/B with the restore proving the write is what moved it.
// The falsifier. adjusted counts uploads this write actually changed; skipped
// counts 64-byte constant buffers that failed the ortho discriminator. Both
// are needed: "the HUD did not move" reads identically for a wrong subset, a
// wrong scale and a hook that never ran.

// Where the reticle-scale compensation believes the reticle is, in pixels of
// the render. Logged beside the wearer's report so a mis-signed projection is a
// number rather than "it still drifts a bit".
void ReadReticleAnchor(float* x, float* y, float* yawOffset, float* pitchOffset);

// C1's aim census: the WORKING behind those pixels. `passWidth`/`passHeight`
// are the HUD pass's own pixel extents, `divisorX`/`divisorY` the half-tangents
// that were actually divided by, `hadAim` whether an aim reference was in hand
// at all, and `updates` a monotonic count so a frozen anchor is distinguishable
// from a stationary one.
void ReadReticleAnchorDetail(float* passWidth, float* passHeight, float* divisorX, float* divisorY,
                             int* hadAim, unsigned long long* updates);

// The magnification the DECLARED half-tangents carry and the RENDERED ones do
// not -- 1.0 at rest, so the two accessors are identical outside ADS. Exposed
// for the census, which has to print which of the two a consumer used and what
// the other one would have given. A forwarder rather than the decider itself:
// the decider is file-local by design and stays that way.
float DeclaredMagnificationInUse();

// Seqlock read of the angle pair the camera detour latched: [pitch, yaw, roll]
// as the game handed them and as we wrote them. False means the pair tore and
// the caller must not build anything from it. Forwarder, for the same reason as
// the magnification above.
bool ReadLatchedCameraAngles(float* base, float* applied, std::uint32_t* generation);

// Whether the viewmodel correction also rotates the HUD's near-origin pass.
// It should not: that pass is the HUD, and correcting it is what made the
// panels ride a wave. Default is EXCLUDED; set viewmodel.correct_hud_pass = 1
// to restore the old behaviour.
void SetCorrectHudPass(bool correct);

// Excludes the WORLD-origin near-plane-1 passes instead of the near-origin one.
// The complement of SetCorrectHudPass; together they bisect the family.
void SetCorrectWorldOriginPass(bool correct);

// WHERE the HUD is anchored. 0 = head-locked (it rides the headset; the
// default, and the state defect A was closed in). 1 = aim-locked: the panels
// sit where the gun is pointing and stay there while the head turns.
// INI: rui.hud_anchor. Also on the calibration cluster under HUD ANCHOR mode.
void SetHudAnchorMode(int mode);
// THE HUD PASS'S FRUSTUM, refitted to the world's. See the note beside
// g_hudFovMatch: the near-origin HUD pass renders at 73.7 x 55.4 deg while the
// eye is displayed at 109.7 x 89.7, so anything world-anchored drawn in it sits
// 1.89x too far from the view centre -- zero at the middle, growing outward.
void SetHudFovMode(int mode);
// The frustum refit's origin bound, in units. Default 32; it was hard-coded at
// 1.0, which head movement alone exceeds -- see the note at g_hudFovOriginMax.
void SetHudFovOriginMax(float units);
int HudFovMode();
void ReportHudFovMatch();
int HudAnchorMode();

// What the OpenXR runtime asks for per eye, as reported by
// xrEnumerateViewConfigurationViews. Zero until XR has been initialised. This is
// the reference the Home page's resolution slider is a multiple OF: 1.00 means
// "render exactly what the headset asked for".
unsigned int RuntimeRecommendedEyeWidth();
unsigned int RuntimeRecommendedEyeHeight();

// The HEADSET's own cone, as full tangent extents (right-left, up-down),
// published by XrContext from xrLocateViews. Zero until XR is running.
//
// This -- not the runtime's recommended rect -- is the reference for "are we
// rendering pixels nobody can see". The recommended rect's aspect need not match
// the FOV shape at all (1.105 against 1.428 on this runtime), so a pixel-count
// ratio against it is not an efficiency measure.
void SetHeadsetFovTangents(float tanWidth, float tanHeight);
bool HeadsetFovTangents(float* tanWidth, float* tanHeight);

// Camera-buffer uploads seen per family since load, in the order main scene,
// effects, viewmodel, other. Unconditional -- unlike the shear's own tally,
// which only moves while xr.lens_shear is armed and therefore reads zero on
// every run that matters. "Was the gun drawn at all" is a counter question and
// this is the counter.
void ReadUploadFamilyTally(unsigned long long out[4]);

// viewmodel.detach_lean: the strength in force, and how many uploads it has
// actually been applied to. See the note at the definition -- armed and
// applying are different claims and the census prints both.
float ViewmodelDetachLeanStrength();
unsigned long long ViewmodelDetachLeanApplied();

// The pose the compositor will reproject to, [pitch, yaw, roll] base and applied,
// torn-free or false. The lock-ring correction projects into THIS frame, not the
// latched applied camera, because the layer is submitted with this pose.
bool ReadPublishedHeadAnglesForHud(float* base, float* applied, std::uint32_t* generation);
