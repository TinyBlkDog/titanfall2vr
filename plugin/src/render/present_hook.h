#pragma once

#include <atomic>

struct ID3D11Texture2D;

void InstallPresentHook();
void RemovePresentHook();
// F2a. How long the game has been holding the same picture, in milliseconds,
// as of right now. Zero before the first Present.
//
// This is the seam the not-live indicator reads. It is published from the
// Present hook and answerable from ANY thread, which is the point: while the
// game is blocked in a level load nothing runs on the presenting thread, so
// only an off-thread reader can notice that the image has stopped moving.
double Tf2VrPresentStalenessMs();
// xr.stale_ms -- how long the image must be held before it counts as stale.
// Default 250 ms: long enough that ordinary frame-rate dips are not events,
// short enough that the wearer's 2-4 s freeze is caught at its start.
void SetPresentStaleThresholdMs(double ms);
// xr.decouple -- DEFAULT 0, and one line in the ini reverts it.
//
// 0 keeps the shipped path: one XR frame per game frame, submitted from inside
// Present. 1 hands the XR frame loop to a dedicated pacing thread and demotes
// Present to a producer, so the headset keeps getting fresh-pose frames while
// the game is slow or blocked. It does NOT shorten a level load.
void SetXrDecoupled(bool enabled);
// For the bare-key kill switch: the pacing thread is the one mechanism here
// that has taken a run down, so the wearer must be able to stop it in place.
bool IsXrDecoupled();
void SetXrArmed(bool armed);
void SetHeadLookArmed(bool armed);
// Diagnostic: call xrEndFrame with no layers, to separate the cost of
// compositing what we submit from the cost of the submission itself. The
// headset shows nothing while this is set.
void SetXrForceZeroLayers(bool on);
bool IsXrArmed();
// True only when a headset session is actually running -- see the note in
// present_hook.cpp. Use this, not IsXrArmed, to choose where a human looks.
bool IsXrPresentingToHeadset();
bool IsHeadLookArmed();
// Called from the bounded Task 4 render interceptor after the first normal
// render and before the fresh pre-resolver offset render.
extern "C" void CaptureStereoFirstEye();
// Bounded diagnostic used after a *native* slot dispatch has returned.
bool DumpVerifiedGameBackbuffer(const char* filename);
// Asks for ONE backbuffer dump under the given %TEMP% filename, performed on
// the presenting thread inside Present, under the same lock every other
// capture here uses. The scripted flat session calls this rather than dumping
// directly, because it runs on the plugin frame and the swapchain belongs to
// Present. We copy the BACKBUFFER, not the screen, so the whole frame is
// captured however little of the window the monitor happens to show.
void RequestNamedBackbufferCapture(const char* filename);
// Same, but only honoured on a frame rendered for the given eye (-1 = any).
//
// Under alternate-frame stereo the game draws one frame per EYE, so two captures
// taken a second apart are usually different eyes and the ENTIRE image is shifted
// between them. The first flat run's pairs differed across the whole frame for
// exactly that reason, which made every pixel comparison meaningless. Pinning a
// comparison to one eye is what makes the difference within it mean anything.
void RequestNamedBackbufferCaptureOnEye(const char* filename, int eye);
// Which eye the last completed named capture landed on, and whether one is still
// outstanding -- so a caller can pin the second half of a pair to the eye the
// first half actually got.
int LastNamedCaptureEye();
bool NamedCaptureOutstanding();
// Same, but only honoured on a frame rendered for the given eye (-1 = any).
//
// Under alternate-frame stereo the game draws one frame per EYE, so two captures
// taken a second apart are usually different eyes and the entire image is shifted
// between them. The first flat run's pairs differed across the whole frame for
// exactly this reason, which made every pixel comparison meaningless. Pairing a
// comparison to one eye is what makes the difference within it mean anything.
void RequestNamedBackbufferCaptureOnEye(const char* filename, int eye);
// Which eye the last completed named capture was taken on, and whether one is
// still outstanding -- so a caller can pin the second half of a pair to the eye
// the first half actually landed on.
int LastNamedCaptureEye();
bool NamedCaptureOutstanding();
// Same, but only honoured on a frame rendered for the given eye (-1 = any).
//
// Under alternate-frame stereo the game draws one frame per EYE, so two captures
// taken a second apart are usually different eyes and the entire image is shifted
// between them. The first flat run's pairs differed across the whole frame for
// exactly this reason, which made every pixel comparison meaningless. Pairing a
// comparison to one eye is what makes the difference within it mean anything.
void RequestNamedBackbufferCaptureOnEye(const char* filename, int eye);
// Which eye the last completed named capture was taken on, and whether one is
// still outstanding -- so a caller can pin the second half of a pair to the eye
// the first half actually landed on.
int LastNamedCaptureEye();
bool NamedCaptureOutstanding();
// Read-only report of the D3D11 method entry bytes this project detours; the
// diagnostic behind the 20-vs-16 displaced-byte finding.  Installs nothing.
void ProbeVerifiedGameDrawFunctionEntries();
// Bounded function-entry trace: the draw-burst correlation that built the
// camera-family table.  Task 02 needs to re-read that table if the projection
// changes which uploads exist.
void BeginVerifiedGameDrawFunctionEntryTrace();
void AdvanceVerifiedGameDrawFunctionEntryTrace();
void BeginVerifiedGameCameraUpdateTrace();
void BeginVerifiedGameCameraOriginOffsetTest();
void BeginVerifiedGameMatrixEyeTranslationTest();
// Bounded probe: offsets c_cameraOrigin on the near-origin perspective family
// only, to identify whether that family is the viewmodel.
void BeginVerifiedGameViewmodelProbe();
void SetVerifiedGameViewmodelCompensation(bool enabled);

// `viewmodel.compensation` -- THE SAME ARM F9 MAKES, REACHABLE FROM THE INI.
//
// SetVerifiedGameViewmodelCompensation early-returns when the game's swapchain
// has not been verified yet, and the ini is parsed long before a device exists.
// So the ini records a WANT and the tick applies it once the device is up,
// exactly as arms.collapse does. Without that, an ini line would log
// "skipped; game swapchain not verified yet" and silently never arm.
//
// WHY A MEASURING RUN WANTS IT. Installing this is what installs the
// UpdateSubresource interception, and that interception is where the main-scene
// frustum is read. With it off there is no tangent, no ADS ratio and no
// magnification -- which is exactly how the S2 run came back with worldTan
// 0.0000 on every line.
//
// WHAT IT COSTS. It also enables the viewmodel counter-rotation, which is a
// correction and not an instrument. With no head data the correction applies
// NOTHING, and its own falsifier line says so in counts:
// "applied=0 skipped(no head data)=N". Read that line before trusting a flat
// run that has this armed -- a non-zero `applied` means the render was being
// changed underneath the measurement.
void SetViewmodelCompensationWanted(bool wanted);
bool IsViewmodelCompensationWanted();
// Per plugin frame. Applies the want once the device is verified, then logs
// once and does nothing further.
void TickViewmodelCompensationWanted();
void BeginVerifiedGameStereoPairCapture();
void ToggleVerifiedGameAlternateFrameStereo();
bool IsAlternateFrameStereoArmed();
bool IsProjectionLayerArmed();
bool IsVerifiedGameSwapchainReady();
// The ADOPTED swapchain's CURRENT backbuffer size, read live.
//
// Not PublishBackbufferSize's copy: that is written once, from xr_context, when
// XR adopts the swapchain -- so it is never written at all on a flat run, and it
// goes stale the moment a mode change resizes the buffer. A sweep that has to
// know whether the engine GRANTED the size it was asked for needs the live
// value, in both modes.
//
// Reads the swapchain pointer without the module mutex, exactly as
// IsVerifiedGameSwapchainReady does: it is written once at adoption and never
// rewritten, and taking a lock the Present path owns from the game tick is the
// hazard settings_registry.h warns about. Returns false until adoption.
bool VerifiedGameBackbufferSize(unsigned int& width, unsigned int& height);
// Puts the camera detours in on the ADOPTED device, arming no correction.
//
// The device stays private to this module -- every other caller of the camera
// hook goes through a helper here for the same reason. Exists because the
// main-scene frustum and viewport recorders only run once those detours are
// installed, and the only thing that installs them today is stereo arming: so a
// flat run, which never arms stereo, measures nothing at all. Returns false if
// no swapchain has been adopted yet.
bool EnsureGameCameraDetoursInstalled();

// IS THERE ANYTHING ON THE SCREEN? A resolution test that cannot see its own
// output can produce a clean, monotonic, entirely believable cost curve while
// the wearer is looking at black -- which is exactly what happened on
// 2026-08-27, and I defended the curve with an argument when a look would have
// settled it.
//
// Point-samples a 320x200 thumbnail out of a staging copy: one small BMP in
// %TEMP% to eyeball, plus the statistic that makes eyeballing optional. NOT a
// full dump -- at 7256x4268 that is 124 MB a step, and the Map is a full GPU
// barrier that would land inside the frame-time window it is meant to validate.
// Call it once per step, after timing has closed.
struct BackbufferContent {
    bool valid;
    unsigned width, height;
    float nonBlackFraction;  // 0.0 = every sampled pixel is black
    float meanLuma;          // 0..255
    unsigned brightest;      // 0..255
};
// QUEUED, AND SERVICED INSIDE Present. The swapchain is
// DXGI_SWAP_EFFECT_DISCARD, so after Present the backbuffer is UNDEFINED --
// sampling it from the game tick reported 0.0% non-black on a window the wearer
// was looking at. Request from anywhere; it is taken before the flip, on the
// frame the game just drew, and collected on a later tick.
//
// Sampled there, it says what the game DREW. Content here plus black on the
// monitor is a PRESENTATION fault; black here is the game not drawing. The old
// placement could not tell those apart.
void RequestBackbufferContentSample(const char* thumbnailName);
bool TakeBackbufferContentSample(BackbufferContent* out);
bool BackbufferContentSampleOutstanding();
// Swaps the two quad layers for XrCompositionLayerProjection. The quad path is
// kept and is used whenever the projection layer's preconditions do not hold.
void ToggleVerifiedGameProjectionLayer();
// RenderDoc-only native-dispatch labels. These do not intercept D3D methods,
// change render state, or invoke an engine render path.
extern "C" void BeginNativeSlotRenderDocMarker(unsigned int slot);
extern "C" void EndNativeSlotRenderDocMarker(unsigned int slot);

// Presented frames per second, averaged over the last second. For the Home
// page's readout; it is the same counter the PRESENT beat line reports, exposed
// so the wearer can see it without reading a log.
float PresentedFramesPerSecond();

// The wearer presses a key when they FEEL a spike; this prints the last few
// seconds of frame timings so their perception can be checked against ours.
void XrReportRecentFrames();
