#pragma once

#include <atomic>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dxgi.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <mutex>
#include <thread>
#include <vector>
#include <wrl/client.h>

class XrContext {
public:
    ~XrContext();
    // The wearer's spike marker: the last few seconds of frame timings.
    void ReportRecentFrames();
    void SetEnabled(bool enabled);
    void SetHeadLookEnabled(bool enabled);
    // Submit no layers while still calling xrEndFrame. Diagnostic only: the
    // headset shows nothing while this is set.
    void SetForceZeroLayers(bool on) { forceZeroLayers_ = on; }
    // eye is 0 for left and 1 for right, and must match the eye offset the
    // frame was actually rendered with.  Only that eye's swapchain is updated;
    // the other keeps its previous image, which is what makes alternate-frame
    // stereo possible without a second render.
    bool SubmitBackbuffer(IDXGISwapChain* gameSwapchain, int eye);
    // SAME-FRAME STEREO. Submits an arbitrary texture as one eye. The
    // backbuffer path above is a special case of this; both end in
    // SubmitFrameCore, so the swapchain, layer and pacing are identical and
    // only the SOURCE of the image differs.
    bool SubmitEyeTexture(ID3D11Texture2D* source, int eye);

    // ---- F3: the decoupled frame loop. xr.decouple, DEFAULT 0. ----
    //
    // Coupled (the shipped path, and what 0 keeps): one XR frame per game
    // frame, every XR call made from inside Present. When the game presents at
    // 40 fps during a level load the compositor gets frames at half the display
    // rate, each late against the predictedDisplayTime xrWaitFrame handed us --
    // measured, 3.6 ms of blocking inside a 25 ms frame. Worse, while Present
    // is blocked for seconds we make no XR calls at all, which is the frozen
    // picture the wearer is describing.
    //
    // Decoupled: Present becomes a PRODUCER ONLY -- it copies the backbuffer
    // into a texture shared with our own D3D11 device and returns, making no
    // OpenXR calls whatsoever -- and a dedicated pacing thread owns the entire
    // frame loop, submitting with a FRESH pose at the display rate. A game frame
    // that never arrives simply leaves the last picture up, reprojected, so a
    // 40 fps game reprojects smoothly instead of juddering.
    //
    // This does NOT shorten the load, and must never be sold as if it does.
    void SetDecoupled(bool enabled);
    bool IsDecoupled() const { return decoupleWanted_; }
    // PRODUCER, called from Present on the game thread while decoupled. Copies
    // the backbuffer into a texture shared with the pacing thread's own D3D11
    // device and returns. It makes NO OpenXR calls at all -- see the note on
    // CreateXrDevice for why that is a requirement rather than a preference.
    //
    // Returns false only when decoupling is off or has stood itself down, and
    // the caller then runs the proven coupled path for this frame.
    bool PublishBackbuffer(IDXGISwapChain* gameSwapchain, int eye);
    bool PublishBackbufferUnused(IDXGISwapChain* gameSwapchain, int eye);
    void SetStereoEnabled(bool enabled);
    // Swaps the two quad layers for a real XrCompositionLayerProjection.  The
    // quad path stays intact and is used whenever the projection layer's
    // preconditions are not met, so this can never leave the headset with
    // nothing to show.
    void SetProjectionLayerEnabled(bool enabled);
    bool IsProjectionLayerEnabled() const { return projectionLayerEnabled_; }
    // ARMED IS NOT RUNNING. auto-arm calls SetXrArmed(true) as soon as a
    // swapchain is adopted, whether or not a headset ever answered: on the
    // 2026-09-07 flat runs IsXrArmed() was true while the runtime reported NO
    // HEADSET and no session existed. Anything choosing between "show this on
    // the monitor" and "show this in the headset" has to ask THIS, not that.
    bool SessionRunning() const { return sessionRunning_; }
    void Shutdown();

private:
    bool Initialize(IDXGISwapChain* gameSwapchain);
    bool CreateSwapchain(const DXGI_SWAP_CHAIN_DESC& gameDesc);
    bool CreateEyeSwapchain(const DXGI_SWAP_CHAIN_DESC& gameDesc, int eye);
    // The config panel's world-fixed quad. See the block at the definitions.
    bool EnsureMenuSwapchain(ID3D11Texture2D* source);
    void CaptureMenuPose(XrTime displayTime);
    bool PrepareMenuLayer(XrCompositionLayerQuad& quad, XrTime displayTime);
    // The game can resize its backbuffer under us (a resolution change resizes
    // the existing swapchain rather than making a new one, so pointer identity
    // still holds and nothing else notices).  Our XR swapchains were created at
    // the old size, and CopyResource between mismatched textures is a silent
    // no-op -- the headset then shows the last good frame forever while the
    // desktop carries on rendering.  This detects that and rebuilds.
    bool HandleBackbufferResize(IDXGISwapChain* gameSwapchain);
    bool PollEvents();
    void UpdateLookInjection(XrTime predictedTime);
    // Task 02 step 1, passive: locates the per-eye pose and FOV the runtime
    // actually wants for this frame.  Nothing consumes the result yet; it is
    // the measurement the projection layer and the FOV match both depend on,
    // and it settles the aspect-ratio question with real numbers instead of an
    // assumption about what a headset frustum looks like.
    void LocateViews(XrTime predictedTime);
    // The runtime's own answer to "what should each eye be rendered at".
    // Queried once at session setup; passive.
    void LogViewConfiguration();
    void DestroySwapchain();
    void Log(const char* message) const;

    bool enabled_ = false;
    bool headLookEnabled_ = false;
    bool initialized_ = false;
    bool sessionRunning_ = false;
    bool haveLookBaseline_ = false;
    float lastYaw_ = 0.0f;
    float lastPitch_ = 0.0f;
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace localSpace_ = XR_NULL_HANDLE;
    // STAGE. Floor-referenced, and the only space that can answer how tall the
    // wearer is. Optional in the spec, so it may legitimately stay null.
    XrSpace floorSpace_ = XR_NULL_HANDLE;
    float eyeHeightSampleMetres_ = 0.0f;
    int eyeHeightStableSamples_ = 0;
    bool eyeHeightSettled_ = false;
    XrSpace viewSpace_ = XR_NULL_HANDLE;
    XrSwapchain colorSwapchain_ = XR_NULL_HANDLE;
    // THE CONFIG PANEL'S OWN SWAPCHAIN AND QUAD. PLAN-VRMENU M6.
    //
    // Entirely separate from the game image: its own swapchain, its own quad,
    // and a pose captured in LOCAL space when the panel is summoned so it stays
    // where the wearer left it instead of riding the view.
    XrSwapchain menuSwapchain_ = XR_NULL_HANDLE;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> menuImages_;
    bool menuWasVisible_ = false;
    bool menuPosed_ = false;
    XrPosef menuPose_{};
    float menuAspect_ = 4.0f / 3.0f;
    bool menuCopyMismatchReported_ = false;
    bool stereoEnabled_ = false;
    // Per-eye swapchains for alternate-frame stereo.  eyeHasImage_ gates
    // submission until both eyes have received at least one frame, so the
    // first frames cannot present one stale/blank eye against a live one.
    XrSwapchain eyeSwapchain_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> eyeImages_[2];
    bool eyeHasImage_[2] = {false, false};
    // Latest per-eye pose and FOV from xrLocateViews, plus the pose each eye's
    // presented image was actually rendered with.  In alternate-frame stereo
    // those differ by a frame, and a projection layer submitted with the
    // current pose rather than the rendered one is the subtle failure this
    // separation exists to prevent.
    XrView locatedView_[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    bool locatedViewsValid_ = false;
    XrView renderedView_[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    bool renderedViewValid_[2] = {false, false};
    // Midpoint of the two eyes when each image was rendered, and a fixed
    // reference midpoint.  The game camera does not translate with the head, so
    // a projection layer must not claim it did; only the per-eye separation is
    // real.  See BuildProjectionEyePosition.
    XrVector3f renderedHeadCentre_[2]{};
    XrVector3f projectionOrigin_{};
    bool projectionOriginValid_ = false;
    unsigned int viewLogCountdown_ = 0;
    bool copyMismatchReported_ = false;
    // Set once the mono swapchain holds any image; lets a frame with no fresh
    // copy resubmit the previous one instead of submitting nothing.
    bool monoHasImage_ = false;
    std::uint64_t framesWithoutFreshImage_ = 0;
    // Per-call submission timing, to attribute the stall that arming OpenXR
    // introduces to one of xrWaitFrame, the image round trip, or xrEndFrame.
    double submitWaitMs_ = 0.0;
    double submitImageMs_ = 0.0;
    double submitEndMs_ = 0.0;
    unsigned submitSamples_ = 0;
    unsigned long long submitLayersTotal_ = 0;
    unsigned long long submitFrameIndex_ = 0;
    // A frame that took this long in xrEndFrame alone is a stall, not jitter:
    // healthy windows measure 1.5-11 ms and the bad ones measured 87-89 ms.
    static constexpr double kEndFrameSpikeMs = 25.0;
    // Capped so a sustained stall cannot fill the log with 90 lines a second and
    // bury the startup lines that say which runtime and resolution are in play.
    static constexpr unsigned kMaxSpikeLines = 40;
    unsigned spikesLogged_ = 0;
    // ---- FRAME-INTERVAL SPIKE WINDOW ---------------------------------------
    //
    // The spike line above times xrEndFrame ALONE and fires at 25 ms, which is
    // a stall. The wearer's report is different in kind: an extra 4-5 ms on an
    // 11.11 ms budget, so a ~16 ms frame, which that threshold can never see
    // and which an average over sixty frames hides by construction.
    //
    // So this measures the SUBMIT-TO-SUBMIT INTERVAL -- the whole frame, ours
    // and the game's alike -- and reports the WORST one in each ten-second
    // window rather than the mean. It reports every window whether or not
    // anything spiked, because "no line" and "instrument not running" have to
    // be tellable apart. One line per ten seconds is nothing next to the 108
    // lines a second this log used to write.
    long long lastSubmitQpc_ = 0;
    unsigned long long spikeWindowStart_ = 0;
    unsigned spikeWindowFrames_ = 0;
    unsigned spikeWindowOver_ = 0;
    double spikeWindowWorstMs_ = 0.0;
    double spikeWorstWaitMs_ = 0.0;
    double spikeWorstCallMs_ = 0.0;   // the xrEndFrame call alone: the runtime
    double spikeWorstPrepMs_ = 0.0;   // our own submit work before it: ours
    // A PAUSE OF SECONDS IS NOT A WORST FRAME. The wearer reported the screen
    // pausing for seconds mid-firefight; the worst single frame all session was
    // 137 ms. So either it was a RUN of bad frames back to back, or the submit
    // loop kept going at 90 fps while the display froze -- and those are
    // different faults with different owners. A single worst-frame number can
    // tell them apart from neither.
    unsigned spikeRunFrames_ = 0;         // current consecutive over-budget run
    double spikeRunMs_ = 0.0;
    unsigned spikeWorstRunFrames_ = 0;    // longest such run in the window
    double spikeWorstRunMs_ = 0.0;
    double spikeLostMs_ = 0.0;            // total time over budget in the window
    double lastPrepMs_ = 0.0;             // our submit work on the last frame
    double lastCallMs_ = 0.0;             // the xrEndFrame call on the last frame
    // ---- THE LAST FEW SECONDS OF FRAMES, for the wearer's own marker --------
    //
    // A window summary says a window was fine; it cannot say whether the thing
    // the wearer FELT at 14:32:07 was in it. This ring keeps the recent frames
    // so a keypress can print what actually happened in the seconds before it,
    // and a "I saw a spike" that lands in a clean window is then a fact about
    // the instrument rather than about the wearer.
    static constexpr int kFrameRing = 512;
    float ringInterval_[kFrameRing]{};
    float ringWait_[kFrameRing]{};
    float ringPrep_[kFrameRing]{};
    float ringCall_[kFrameRing]{};
    std::atomic<unsigned> ringWrite_{0};
    double spikeWindowPeriodMs_ = 0.0;
    unsigned submitShouldRender_ = 0;
    // THE QUANTITY THAT ACTUALLY MATTERS.
    //
    // xrWaitFrame and xrEndFrame are not two independent costs; they are two
    // places the runtime can park the app to hold it to the display rate. Across
    // every archived run their SUM is a constant and the split between them is
    // arbitrary -- the final log has END ranging 1.5 to 11.7 ms while WAIT moves
    // the opposite way and the sum stays at 11.9 ms. Reading END alone measures
    // the phase of the frame, not a stall, which is what sent three sessions
    // chasing a hook that was never in the path.
    //
    // So the window reports the sum and the rate it implies, and compares it
    // against the period the RUNTIME says it wants (predictedDisplayPeriod).
    // Those two numbers separate the only two possibilities that remain:
    // the runtime is pacing us slowly on purpose (declared period ~= actual),
    // or it is overrunning its own declared period (declared ~= 11.9, actual
    // ~= 98), which would be a genuine failure to retire the frame.
    double submitPredictedPeriodMs_ = 0.0;
    // Session state as the runtime last reported it. Only READY/STOPPING/EXITING
    // were ever acted on, so VISIBLE, SYNCHRONIZED and FOCUSED passed through
    // unrecorded -- and "not being displayed" is the leading explanation for a
    // 10 Hz pace, so it has to appear in the log rather than be inferred.
    int sessionState_ = 0;
    // Diagnostic: call xrEndFrame with no layers at all, to separate the cost of
    // compositing what we submit from the cost of the submission itself.
    bool forceZeroLayers_ = false;
    bool projectionLayerEnabled_ = false;
    // Which precondition last sent us back to the quads, so the reason is
    // logged when it changes rather than every frame.
    int projectionFallbackReason_ = 0;
    // V2's falsifier reports once per session: it is a property of the headset's
    // geometry, not of a frame, and a per-frame line would bury it.
    bool falsifierReported_ = false;
    // recommendedImageRect{Width,Height} from xrEnumerateViewConfigurationViews:
    // the per-eye resolution the runtime wants. The game's backbuffer is what we
    // actually have, and the ratio between them is the fidelity ceiling.
    std::uint32_t recommendedEyeWidth_ = 0;
    std::uint32_t recommendedEyeHeight_ = 0;
    std::uint32_t maxEyeWidth_ = 0;
    std::uint32_t maxEyeHeight_ = 0;
    // G4: the one-line session banner. Kept so any report from any headset
    // carries its own geometry, and emitted once, on the first frame where both
    // the runtime's views and the game's frustum are known.
    char runtimeName_[128] = "<unknown>";
    char systemName_[128] = "<unknown>";
    bool headsetBannerLogged_ = false;
    bool geometryLearnedEarly_ = false;   // the resolution half of the banner, done in the menu
    void LogHeadsetBanner(float ipdMetres);
    // The pose half of the layer contract. Written and read only on real game
    // frames from inside SubmitFrameCore, so no synchronisation is needed.
    void TrackPoseAgreement(int eye);
    float poseTrackPrevGame_[2]{};
    float poseTrackPrevDecl_[2]{};
    bool poseTrackHavePrevious_ = false;
    unsigned int poseTrackFrames_ = 0;
    double poseTrackGameSum_ = 0.0;
    double poseTrackDeclSum_ = 0.0;
    double poseTrackDiffSum_ = 0.0;
    float poseTrackPeakStep_ = 0.0f;
    // ---- F3 internals ----
    //
    // ONE OWNER. OpenXR requires xrWaitFrame/xrBeginFrame/xrEndFrame to be
    // externally synchronised, and the cure for that is not a mutex around two
    // callers -- it is having exactly one. While decoupled the pacing thread
    // makes every frame call and Present makes none; arming and disarming are
    // posted as atomics and consumed at the top of the pacing loop, so no lock
    // is ever held across xrWaitFrame (which blocks for a display period by
    // design, and would stall the game thread if the game thread could wait on
    // it).
    // The frame. `doImageWork` is false on the pacing thread, which does the
    // wait/begin/locate/layers/end triple and never touches D3D or a swapchain
    // image; the producer has already put the picture in place and the
    // compositor shows the most recently RELEASED image.
    bool SubmitFrameCore(ID3D11Texture2D* source, int eye, bool doImageWork);
    void PacingThreadMain();
    void MaybeStartPacingThread();
    double MsSinceLastXrFrame() const;
    double MsSinceLastGamePresent() const;
    // xr.decouple_standdown, in display periods. The pacer fills only after the
    // GAME has produced nothing for this long. 1.0 is the old behaviour, which
    // targeted the display rate exactly and left the game no headroom.
    double standDownPeriods_ = 4.0;
    void StopPacingThread();
    // ---- the second device, and why there has to be one ----
    //
    // Two constraints, both measured rather than assumed:
    //
    //  * ONE THREAD must own the frame calls AND the swapchain image calls.
    //    With the pacing thread as sole caller the runtime paced us at 12.58,
    //    12.47 and 12.49 ms against a declared 12.50. Sharing the loop with
    //    Present under a mutex degraded it to 106 Hz and then to 0.06 ms
    //    returns -- the runtime had stopped pacing us at all, started answering
    //    shouldRender=false, and the zero-layer frames that produces are what
    //    the wearer saw as the loading text being drawn, wiped and drawn again.
    //
    //  * THE GAME'S IMMEDIATE CONTEXT must be touched only by the game thread.
    //    Making it thread-safe means ID3D11Multithread, which swaps that
    //    context's vtable; slot 48 stops being the UpdateSubresource the camera
    //    hook's byte matcher knows, and stereo then silently never arms.
    //
    // Together those say the pacing thread cannot use the game's device at all.
    // So it gets its own: the XR session is created on OUR device, the game
    // thread copies its backbuffer into a texture shared between the two
    // devices, and the pacing thread copies that into the XR image on its own
    // context. Neither device is ever used from the other's thread, so no
    // protection is needed and the camera hook sees exactly what it always saw.
    bool CreateXrDevice(const LUID& adapterLuid, D3D_FEATURE_LEVEL minFeatureLevel);
    bool EnsureSharedFrame(const D3D11_TEXTURE2D_DESC& backbufferDesc);
    void ReleaseSharedFrame();
    // Serialises the wait/begin/end triple between Present and the pacing
    // thread. Present LOCKS it; the pacing thread only ever TRY-locks, so a join
    // can never deadlock against it.
    std::mutex frameMutex_;
    // PRESENT HAS PRIORITY, AND IT HAS TO BE EXPLICIT.
    //
    // std::mutex is not fair, and the pacing thread holds this lock across
    // xrWaitFrame -- which blocks for ~11 ms of every 12.5 ms display period.
    // That is a ~90% duty cycle, so left to chance the game loses the race over
    // and over: measured, 120 gap frames filled for every 2 the game managed,
    // with the game thread waiting 1501-1514 ms at a time to get in. And it is
    // self-reinforcing, because a game that cannot present leaves the gap open,
    // which is the pacing thread's cue to fill it again.
    //
    // So Present announces itself BEFORE it blocks, and the pacing thread stops
    // competing the moment it sees that. The game then waits at most one pacing
    // frame rather than an unbounded number of lost races.
    std::atomic_bool presentWaiting_{false};
    // WHEN THE LAST XR FRAME WENT OUT, from EITHER thread, and the period the
    // runtime says it wants between them.
    //
    // Gating the pacing thread on "how long since the game last PRESENTED" was
    // wrong: the runtime does not care who produced a frame, only how many it
    // is being handed. With the game presenting and the pacer filling, the
    // combined loop ran at 106.5 Hz against a declared 80 Hz -- and a runtime
    // handed more frames than it wants starts returning shouldRender=false,
    // which this code correctly turns into a zero-layer submission. Zero layers
    // does not mean "hold the last picture", it means "show nothing", so the
    // loading screen's text was drawn, blanked, and drawn again.
    //
    // Gating on the last XR FRAME instead caps the combined rate at the display
    // rate by construction, whoever produced the frame.
    std::atomic<long long> lastXrFrameQpc_{0};
    std::atomic<double> displayPeriodMs_{12.5};
    // Guards locatedView_/locatedViewsValid_ only. The pacing thread writes
    // them in LocateViews and the producer reads them to record which pose an
    // image was rendered against; an XrView pair is ~120 bytes and a torn read
    // is a wrong pose for a frame. Held across a copy and nothing else, so
    // neither thread can be stalled behind a blocking call.
    std::mutex viewMutex_;
    // How many real game frames have been published. It separates "a new game
    // frame arrived" from "the pacing thread is re-showing the last one".
    std::atomic<unsigned long long> publishedFrames_{0};
    // When the GAME last presented, as distinct from when any XR frame last
    // went out. The pacer gates on the second; the stand-down needs the first,
    // because "the game is slow" and "the game has stopped" look identical
    // through a counter that the pacer's own frames also advance.
    std::atomic<long long> lastGamePresentQpc_{0};
    // How often the pacer declined to fill because the game was merely slow.
    // Zero through a load means the stand-down never engaged and the pacer is
    // back to the behaviour that starved Present.
    std::atomic<unsigned long long> pacingStoodDown_{0};
    // PRESENT beat state. Game thread only.
    unsigned int presentBeatFrames_ = 0;
    double presentWorstGapMs_ = 0.0;
    long long presentBeatStartQpc_ = 0;
    // The eye the last REAL game frame was rendered for. A gap frame re-shows
    // that frame and must carry its eye, never a new one.
    std::atomic<int> lastPublishedEye_{-1};
    // Publishes per eye. Under alternate-frame stereo these must stay within a
    // frame or two of each other; a gap means the interleave has broken and one
    // eye is being shown staler than the other, which is judder by construction.
    std::atomic<unsigned long long> publishedPerEye_[2]{};
    std::thread pacingThread_;
    std::atomic_bool pacingRun_{false};
    std::atomic_bool decoupleActive_{false};
    bool decoupleWanted_ = false;
    // Counted so the falsifier can be read straight off the log: submitted
    // frames against published (real game) frames. A ratio at or above 2 during
    // a load window is the whole claim of this change.
    std::atomic<unsigned long long> pacingSubmitted_{0};
    std::atomic<int> lastSubmittedEye_{-1};
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    PFN_xrGetD3D11GraphicsRequirementsKHR getD3D11Requirements_ = nullptr;
    // device_ is the XR device: OUR OWN when decoupled, the game's otherwise.
    // gameDevice_ is always the game's, and only the game thread ever uses it.
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Device> gameDevice_;
    // The one texture both devices can see. The game thread writes it through
    // gameShared_ and the pacing thread reads it through xrShared_; the keyed
    // mutex hands ownership between them, and BOTH sides use a zero timeout so
    // neither can ever block the other.
    // TWO of them, and the reason is measured. With ONE, producer and consumer
    // ping-pong a single keyed mutex: the consumer only releases the game's key
    // after its copy, which happens AFTER xrWaitFrame has blocked ~11 ms of the
    // 12.5 ms cycle -- about a millisecond before the consumer next reads the
    // generation. The producer therefore lost that race roughly half the time
    // and published on every OTHER consumer frame: 64 per 120, i.e. ~43/sec,
    // leaving each eye at 21 Hz against the coupled path's 40. That is the
    // regression the wearer feels.
    //
    // With two, the producer writes whichever buffer the consumer is not
    // holding and never waits for it at all.
    static constexpr int kSharedCount = 2;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> gameShared_[kSharedCount];
    Microsoft::WRL::ComPtr<ID3D11Texture2D> xrShared_[kSharedCount];
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> gameSharedMutex_[kSharedCount];
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> xrSharedMutex_[kSharedCount];
    HANDLE sharedHandle_[kSharedCount] = {nullptr, nullptr};
    // Which buffer holds the newest published frame, and the next one to write.
    std::atomic<int> sharedNewest_{-1};
    // Which eye each slot holds, and its generation, so the consumer can pick
    // the eye it needs rather than take whatever arrived last.
    std::atomic<int> sharedEye_[kSharedCount]{};
    std::atomic<unsigned long long> sharedGen_[kSharedCount]{};
    // THE POSE EACH SHARED FRAME WAS RENDERED AGAINST, captured when the game
    // publishes it. Coupled, "the previous Present's locate" WAS that pose --
    // one game frame back, exactly. Decoupled the game renders at its own rate
    // and the pacing thread locates at the display rate, so the two are
    // unrelated and the compositor gets told the wrong pose to reproject FROM.
    // The error changes every frame, which is stutter, and it only shows when
    // the head moves. So the pose travels with the frame.
    XrView sharedView_[kSharedCount][2]{};
    bool sharedViewValid_[kSharedCount] = {false, false};
    XrView consumeView_[2]{};
    bool consumeViewValid_ = false;
    // Which slot the pacing thread is reading this frame; -1 when the source is
    // not a shared texture. Only the pacing thread touches it.
    int sharedReadSlot_ = -1;
    // The eye the pacing thread last consumed, so it can ask for the other one.
    int lastConsumedEye_ = -1;
    // When the game last published, so it can be held to the display rate.
    std::atomic<long long> lastPublishQpc_{0};
    // High-resolution timer used to hold the game to the display rate. Sleep()
    // is 1-15 ms granular and that jitter is visible as judder.
    HANDLE paceTimer_ = nullptr;
    int sharedWrite_ = 0;
    std::uint32_t sharedWidth_ = 0, sharedHeight_ = 0;
    DXGI_FORMAT sharedFormat_ = DXGI_FORMAT_UNKNOWN;
    bool sharedValid_ = false;
    // Keyed-mutex keys. 0 means the game may write, 1 means the pacing thread
    // may read. Whoever holds it releases with the other's key.
    static constexpr UINT64 kSharedKeyGame = 0;
    static constexpr UINT64 kSharedKeyXr = 1;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> images_;
    DXGI_SWAP_CHAIN_DESC gameDesc_{};
};

// xr.headlock_quad. The quad path is only ever the menu and the loading screen,
// and the loading screen presents at about half rate; head-locking it removes
// the head-motion component of that judder. See the note at the submission.
void SetHeadlockQuad(bool enabled);

// menu.distance_m / menu.width_m -- where the config panel's world-fixed quad
// is placed and how wide it is, in metres. Applied when the panel is next
// summoned, because the pose is captured per summon; changing them will not
// yank a panel the wearer is currently reading.
void SetMenuQuadDistanceMetres(float metres);
void SetMenuQuadWidthMetres(float metres);
float MenuQuadDistanceMetres();
float MenuQuadWidthMetres();
