#include "stereo_targets.h"

#include "camera_update_hook.h"
#include "diagnostics.h"
#include "plugin_cost.h"
#include "render_resolution.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdio>
#include <cstring>

extern "C" volatile std::uint32_t g_tf2vrPresentFrame;

namespace {

// The per-object dispatch table's indices. ID3D11DeviceContext inherits 7
// entries (IUnknown's 3 + ID3D11DeviceChild's 4), so its own methods start at
// 7 and OMSetRenderTargets lands at 33. That arithmetic is not taken on faith:
// the SAME arithmetic puts DrawIndexed at 12, RSSetViewports at 44 and
// UpdateSubresource at 48, and all three are confirmed against this build in
// camera_update_hook.cpp.
constexpr size_t kOmSetRenderTargetsSlot = 33;
// PSSetShaderResources. Same table, same arithmetic that puts DrawIndexed at 12
// and OMSetRenderTargets at 33: ID3D11DeviceContext's own methods start at 7,
// and PSSetShaderResources is the second of them.
//
// THE READ SIDE, AND IT IS WHY M3'S EYE IS CONTAMINATED. The wearer, on the
// first pair where the two cameras plainly differed: "image 2 has a faint copy
// of the unshifted crate from image 1 on it... Almost like the effects layers
// from image 1 got attached onto image 2."
//
// M2 redirects where batch 2 WRITES. Nothing redirects what it READS. So batch
// 2's post chain samples the engine's own composite -- batch 1's finished
// image, weapon and all -- and blends it into our private target. M1 wrote that
// down as inference and never measured it; this census measures it. It also
// explains the crates-through-the-weapon artefact that has been sitting inside
// the private target unexplained for four runs: batch 1's composite already
// contains the weapon.
//
// Read-only, batch-2 only, capped. It changes no binding.
constexpr size_t kPsSetShaderResourcesSlot = 8;
// VSSetConstantBuffers, one slot below PSSetShaderResources by the same
// arithmetic that puts DrawIndexed at 12 and OMSetRenderTargets at 33.
//
// THE QUESTION NOTHING HAS MEASURED. Every camera attempt has patched the
// buffer the engine UPLOADS to -- and the ordering census confirmed both batches
// upload to the same object, 000002422AC76DA0. What has never been measured is
// which buffer is BOUND to the draws. The pre-draw write settles that it
// matters: the camera is now rewritten at the transition that opens batch 2's
// scene span, 16 times a run with 0 declines, 120 units, and batch 2's world
// does not move by a pixel -- while the same lever plainly moved BATCH 1's world
// at 40 units.
//
// Draws that do not move when the buffer they supposedly read is rewritten
// immediately beforehand are not reading that buffer. This is the same shape as
// the ghost, which was also an upload-side assumption, and the same class of
// instrument settled it: census what is BOUND, with batch 1 as the control.
constexpr size_t kVsSetConstantBuffersSlot = 7;
// SLOT 34 IS HOOKED TOO, AND ONLY TO CLOSE A BLIND SPOT. If the engine binds
// its targets through OMSetRenderTargetsAndUnorderedAccessViews, a census
// watching only slot 33 reports a short sequence and looks exactly like a
// census that proves batch 2 does not rebind. That is the failure this rung
// exists to avoid, so the other entry point is counted too. Zero calls here is
// a real answer -- it says slot 33 is the whole story.
constexpr size_t kOmSetRenderTargetsAndUavsSlot = 34;

using OmSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
                                                      ID3D11RenderTargetView* const*,
                                                      ID3D11DepthStencilView*);
using OmSetRenderTargetsAndUavsFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
    UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using PsSetShaderResourcesFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT,
                                                        ID3D11ShaderResourceView* const*);
using VsSetConstantBuffersFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT,
                                                        ID3D11Buffer* const*);

std::atomic_int g_wanted{0};
std::atomic_bool g_active{false};
std::atomic_uint32_t g_inFlight{0};

ID3D11DeviceContext* g_censusContext = nullptr;
void** g_slot33 = nullptr;
void** g_slot34 = nullptr;
void* g_slot33Original = nullptr;
void* g_slot34Original = nullptr;
OmSetRenderTargetsFn g_origOm = nullptr;
OmSetRenderTargetsAndUavsFn g_origOmUav = nullptr;

// THE MONOTONIC DOUBLE COUNT. See the header: a transient flag raised on the
// submitting thread cannot label an interval on the render thread.
std::atomic_uint64_t g_doubledSubmissions{0};

// M2's batch state, declared here because ResetInterval below clears it.
// Touched only under the census lock, on the render thread.
int g_depthOnlyOpens = 0;         // transitions with no RTV and a DSV bound
// The span index at which batch 2 opened, so a batch-2 write can be matched to
// the batch-1 span it mirrors. -1 until batch 2 starts.
int g_batch2StartSpan = -1;
// One pre-draw camera write per batch-2 scene span, not one per transition.
bool g_preDrawDoneThisSpan = false;
std::atomic_uint64_t g_preDrawWrites{0};
std::atomic_uint64_t g_preDrawDeclines{0};
// The constant-buffer binding census. Per batch: how many binds, and whether the
// camera buffer we patch was among them. Batch 1 is the control -- its world
// draws provably use the camera we can move, so if IT never shows the buffer
// bound, the witness is wrong rather than batch 2.
VsSetConstantBuffersFn g_origVsCb = nullptr;
void** g_slotVsCb = nullptr;
void* g_slotVsCbOriginal = nullptr;
unsigned long long g_cbBinds[3]{};
unsigned long long g_cbCameraBinds[3]{};
unsigned char g_cbCameraSlot[3]{};
// N1-Q2. Last-writer-wins on VS slot 2, per batch, and the bind ordinal it
// landed at. Read-only; see CbSnapshot for what the pair of pointers decides.
void* g_vsSlot2Last[3]{};
unsigned long long g_vsSlot2At[3]{};
std::atomic<void*> g_cameraBufferWatched{nullptr};
// M5. Batch 2 makes ZERO constant-buffer binds -- 1782 on batch 1 against 0 on
// batch 2 -- so its draws inherit batch 1's bindings, and the camera buffer we
// patch is bound at VS slot 2 during batch 1 only. Updating that buffer's
// CONTENTS cannot reach those draws: UpdateSubresource on a bound constant
// buffer lets the runtime rename the resource, and the existing binding keeps
// reading the old data. So batch 2 gets its OWN buffer, bound into the slot the
// census named, at the transition that opens its scene span.
ID3D11Buffer* g_privateCameraCb = nullptr;
std::atomic_bool g_bindArmed{false};
std::atomic_uint64_t g_cameraBinds{0};
std::atomic_uint64_t g_cameraBindDeclines{0};
unsigned g_cameraCbSlot = 2;
std::atomic_bool g_preDrawArmed{false};
std::atomic<float> g_preDrawIpd{0.0f};
std::atomic_int g_batch{1};
// 0xFF is kNoTarget, spelled out because that constant is defined below and
// zero-initialising these would make them target id 0 -- a real target, and the
// shadow atlas at that.
unsigned char g_lastRtv0 = 0xFF;
unsigned char g_victimTargetId = 0xFF;
// THE ARM LABEL, READ AT THE MEASUREMENT. The proof capture must know how many
// substitutions happened in the very interval it photographed -- not how many
// happened this session. An armed capture that reads 0 here is void and says
// so, instead of being compared against the control as though it meant
// something.
int g_substitutionsThisInterval = 0;
std::atomic_int g_capturedIntervalSubstitutions{-1};
std::atomic_int g_capturedIntervalOpens{-1};
// The read-side census state, declared here because ResetInterval clears it.
constexpr int kMaxSrvNotes = 16;
struct SrvNote { unsigned char targetId; unsigned char slot; int span; };
SrvNote g_srvNotes[kMaxSrvNotes]{};
int g_srvNoteCount = 0;
unsigned long long g_srvNoteOverflow = 0;
unsigned long long g_srvSelfReads = 0;   // batch 2 sampling the victim itself
unsigned long long g_srvInvocations = 0;      // raw calls on our context this interval
unsigned long long g_srvViewsSeen = 0;       // non-null views inspected
unsigned long long g_srvMatchesByBatch[3]{}; // matches against our target table
// Snapshot taken at the flip, because the dump runs AFTER ResetInterval.
struct SrvSnapshot {
    unsigned long long invocations, views, matches1, matches2, selfReads;
    unsigned long long mask1, mask2;
    int noteCount;
    SrvNote notes[kMaxSrvNotes];
};
unsigned long long g_srvTargetMask[3]{};
// M5. The read-side substitution: batch 2's post reads batch 2's own world.
std::atomic_bool g_readSubstituteArmed{false};
std::atomic_int g_readSubstituteWanted{0};
std::atomic_uint64_t g_readSubstitutions{0};
// PER BATCH, because the last run put two anomalies on BATCH 1 -- the side
// nothing here was supposed to touch -- and a session total cannot tell "batch 2
// only" from "leaked into batch 1". Any nonzero in the [1] slot is a leak.
std::atomic_uint64_t g_writeSubsByBatch[3]{};
std::atomic_uint64_t g_readSubsByBatch[3]{};
std::atomic_uint64_t g_hdrRoutes{0};
std::atomic_uint64_t g_ldrRoutes{0};
unsigned char g_batch1SceneTargetId = 0xFF;
ID3D11ShaderResourceView* g_privateSrv = nullptr;
// AND AN HDR ONE. The LDR clone above is cloned from the COMPOSITE, because
// that is the target the engine hands batch 2 for its scene. Batch 2's post
// chain, though, expects the HDR SCENE target -- so pointing it at the LDR copy
// hands linear-HDR maths an sRGB 8-bit image and the eye comes back near black
// with the blur stages amplifying the remains. Measured: the READFIX eye needed
// 7x exposure before its geometry was visible.
//
// So batch 2 gets TWO private targets, matching the two the engine uses:
//   HDR, cloned from batch 1's scene target -- batch 2's scene writes here and
//                                              batch 2's post samples here
//   LDR, cloned from the composite          -- batch 2's post OUTPUT lands here,
//                                              and this is the eye we capture
// They are told apart by the signature already latched for batch 1: the scene
// pass is the only span binding TWO colour targets at once.
ID3D11Texture2D* g_privateHdrTex = nullptr;
ID3D11RenderTargetView* g_privateHdrRtv = nullptr;
ID3D11ShaderResourceView* g_privateHdrSrv = nullptr;
std::atomic_int g_sceneTargetPublished{-1};
// AND ITS DEPTH. M2 left the depth shared on purpose and said so: "batch 1 is
// finished with both, and batch 2 s world demonstrably reached the screen
// through an already-populated depth buffer". That held while batch 2 drew into
// the composite. It does not hold now: with batch 2 rendering into its own HDR
// target against BATCH 1 s populated depth, the world geometry z-fails at equal
// depth and the eye comes back with the static world missing -- crates, ground
// and rock black -- while the Titan, the god-rays, the viewmodel and the HUD
// (all drawn later, or with different depth handling) survive.
//
// M2 named this exact deferral: "a private depth is an M3/M5 question if
// divergence needs one." It does.
ID3D11Texture2D* g_privateDepthTex = nullptr;
ID3D11DepthStencilView* g_privateDepthDsv = nullptr;
std::atomic_int g_sceneDepthPublished{-1};
SrvSnapshot g_srvSnapshot{};
struct CbSnapshot {
    unsigned long long binds1, binds2, cam1, cam2;
    unsigned slot1, slot2;
    // N1-Q2, THE IDENTITY QUESTION. Last-writer-wins on VS slot 2, per batch,
    // plus the bind ordinal it happened at. Batch 2 issues no constant-buffer
    // binds at all -- 1782 against 0 -- so whatever batch 1 last put in VS slot
    // 2 is what batch 2's draws read. Comparing THAT pointer against the buffer
    // the engine uploads the world camera into (which the M3 camera census
    // prints on its own line) is the whole of Q2: same pointer means the draws
    // do read the uploaded buffer and the defect is ordering; different pointer
    // means every camera attempt so far wrote a buffer the draws never sample.
    void* vsSlot2Last1;
    void* vsSlot2Last2;
    unsigned long long vsSlot2At1;
    unsigned long long vsSlot2At2;
};
CbSnapshot g_cbSnapshot{};

// ---------------------------------------------------------------------------
// The lock. Held for the append (render thread) and for the buffer flip
// (presenting thread), never for file I/O. If those turn out to be the same
// thread the whole thing is uncontended and costs one interlocked exchange per
// transition; the census logs both thread ids so nobody has to assume.
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
struct Guard {
    Guard() { while (g_lock.test_and_set(std::memory_order_acquire)) YieldProcessor(); }
    ~Guard() { g_lock.clear(std::memory_order_release); }
};

// ---------------------------------------------------------------------------
// The distinct render-target RESOURCES seen this session. Identity is the
// resource pointer, not the view: several views wrap one texture and the
// question "which target is bound" is about the texture.
//
// Pointer identity is honest only within a session and only while the textures
// live -- a freed texture address can be reused. The scene targets are created
// at level load and outlive the run, and the census prints this caveat rather
// than pretending otherwise.
constexpr int kMaxTargets = 40;
struct TargetRecord {
    const void* resource;
    unsigned width, height, format, arraySize, mips, samples;
    bool depth;
    bool sceneSized;
    bool announced;
    unsigned long long bindings;
};
TargetRecord g_targets[kMaxTargets]{};
int g_targetCount = 0;
unsigned long long g_targetOverflow = 0;

// A view-to-target cache, so GetResource and QueryInterface run once per
// distinct view rather than once per call. The game reuses its views, so this
// is a linear scan of a small table on the hot path and nothing else.
constexpr int kMaxViews = 128;
struct ViewRecord { const void* view; unsigned char targetId; };
ViewRecord g_views[kMaxViews]{};
int g_viewCount = 0;
unsigned long long g_viewCacheOverflow = 0;

constexpr unsigned char kNoTarget = 0xFF;

const char* FormatName(unsigned format) {
    switch (format) {
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
        case DXGI_FORMAT_R16G16B16A16_FLOAT:    return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R16G16B16A16_UNORM:    return "R16G16B16A16_UNORM";
        case DXGI_FORMAT_R10G10B10A2_UNORM:     return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R11G11B10_FLOAT:       return "R11G11B10_FLOAT";
        case DXGI_FORMAT_R8G8B8A8_UNORM:        return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM:        return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return "R8G8B8A8_TYPELESS";
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return "B8G8R8A8_TYPELESS";
        case DXGI_FORMAT_R32_TYPELESS:          return "R32_TYPELESS";
        case DXGI_FORMAT_R24G8_TYPELESS:        return "R24G8_TYPELESS";
        case DXGI_FORMAT_D24_UNORM_S8_UINT:     return "D24_UNORM_S8_UINT";
        case DXGI_FORMAT_D32_FLOAT:             return "D32_FLOAT";
        case DXGI_FORMAT_R32G32B32A32_FLOAT:    return "R32G32B32A32_FLOAT";
        case DXGI_FORMAT_R8_UNORM:              return "R8_UNORM";
        case DXGI_FORMAT_R16_FLOAT:             return "R16_FLOAT";
        case DXGI_FORMAT_R32_FLOAT:             return "R32_FLOAT";
        default:                                return "?";
    }
}

// Registers a texture, or returns the id it already has. Called under the lock.
unsigned char RegisterTarget(ID3D11Resource* resource, bool depth) {
    const void* key = resource;
    for (int i = 0; i < g_targetCount; ++i) {
        if (g_targets[i].resource == key) return static_cast<unsigned char>(i);
    }
    if (g_targetCount >= kMaxTargets) { ++g_targetOverflow; return kNoTarget; }
    TargetRecord& r = g_targets[g_targetCount];
    r = TargetRecord{};
    r.resource = key;
    r.depth = depth;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                               reinterpret_cast<void**>(&tex))) && tex) {
            D3D11_TEXTURE2D_DESC d{};
            tex->GetDesc(&d);
            r.width = d.Width; r.height = d.Height; r.format = d.Format;
            r.arraySize = d.ArraySize; r.mips = d.MipLevels; r.samples = d.SampleDesc.Count;
            tex->Release();
        }
    }
    const int rw = RequestedRenderWidth();
    const int rh = RequestedRenderHeight();
    r.sceneSized = rw > 0 && rh > 0 && r.width == static_cast<unsigned>(rw) &&
                   r.height == static_cast<unsigned>(rh) && !depth;
    return static_cast<unsigned char>(g_targetCount++);
}

// Under the lock. The view is an RTV or a DSV; both live in one cache because
// the key is the raw pointer.
unsigned char ResolveView(IUnknown* view, bool depth) {
    if (!view) return kNoTarget;
    for (int i = 0; i < g_viewCount; ++i) {
        if (g_views[i].view == view) return g_views[i].targetId;
    }
    ID3D11Resource* resource = nullptr;
    if (depth) static_cast<ID3D11DepthStencilView*>(view)->GetResource(&resource);
    else       static_cast<ID3D11RenderTargetView*>(view)->GetResource(&resource);
    if (!resource) return kNoTarget;
    const unsigned char id = RegisterTarget(resource, depth);
    resource->Release();
    if (g_viewCount < kMaxViews) {
        g_views[g_viewCount].view = view;
        g_views[g_viewCount].targetId = id;
        ++g_viewCount;
    } else {
        // NOT a silent fallback. The resolve still works; it just costs a
        // GetResource per call from here on, and this count says how often.
        ++g_viewCacheOverflow;
    }
    return id;
}

// ---------------------------------------------------------------------------
// One Present interval's ordered sequence. Double-buffered: the render thread
// appends to one while the presenting thread reads the other.
constexpr int kMaxSpans = 384;
// S-A: THE SPAN RECORDS THE WRITE, NOT ONLY THE REQUEST.
//
// rtv0/rtv1/dsv are the views the ENGINE asked for. They are recorded here, and
// the substitution happens three hundred lines further down -- so batch 2's
// tokens in the sequence dump are identical to batch 1's BY CONSTRUCTION and
// cannot say what batch 2 actually got. That is not a theoretical gap: reading
// batch 2's `/04` depth tokens as "batch 2 shares batch 1's depth" is a
// conclusion this file's own dump invites and the code contradicts.
//
// `batch` and `routed` close it. `routed` is what we FORWARDED, so a redirected
// write and an untouched one stop looking alike.
constexpr unsigned char kRoutedNone     = 0;      // forwarded exactly as asked
constexpr unsigned char kRoutedRtvLdr   = 1u;     // rtv0 -> the LDR clone
constexpr unsigned char kRoutedRtvHdr   = 2u;     // rtv0 -> the HDR clone
constexpr unsigned char kRoutedDepth    = 4u;     // dsv  -> the private depth
struct Span {
    unsigned char rtv0, rtv1, dsv, rtvCount;
    unsigned char batch;    // 1 or 2, as the census classified this span
    unsigned char routed;   // bitmask above; 0 means it landed in the engine's own target
    unsigned int threadId;
    unsigned long long drawsAtEntry;
    unsigned int draws;   // DrawIndexed calls between this transition and the next
};
constexpr int kMaxIntervalThreads = 4;
struct Interval {
    Span spans[kMaxSpans];
    int count;
    unsigned long long transitions;   // TRUE total, uncapped; count is what fitted
    unsigned long long uavCalls;
    unsigned long long doublesAtOpen;
    unsigned int presentFrame;
    unsigned int threadIds[kMaxIntervalThreads];
    unsigned int threadHits[kMaxIntervalThreads];
    int threadCount;
    unsigned int threadOverflow;
};
Interval g_intervals[2]{};
int g_cur = 0;

void ResetInterval(Interval& in) {
    in.count = 0;
    in.transitions = 0;
    in.uavCalls = 0;
    in.threadCount = 0;
    in.threadOverflow = 0;
    in.doublesAtOpen = g_doubledSubmissions.load(std::memory_order_relaxed);
    in.presentFrame = g_tf2vrPresentFrame;
    // M2's batch state. There is nothing to RESTORE at the batch boundary --
    // the substitution changes an argument, never a piece of engine state -- so
    // the boundary is exactly this reset, and Present is the boundary.
    g_depthOnlyOpens = 0;
    g_batch.store(1, std::memory_order_release);
    g_batch2StartSpan = -1;
    g_preDrawDoneThisSpan = false;
    g_batch1SceneTargetId = kNoTarget;
    g_victimTargetId = kNoTarget;
    g_lastRtv0 = kNoTarget;
    g_substitutionsThisInterval = 0;
    g_srvNoteCount = 0;
    g_srvNoteOverflow = 0;
    g_srvSelfReads = 0;
    g_srvInvocations = 0;
    g_srvViewsSeen = 0;
    g_srvMatchesByBatch[1] = 0;
    g_srvMatchesByBatch[2] = 0;
    g_cbBinds[1] = 0; g_cbBinds[2] = 0;
    g_cbCameraBinds[1] = 0; g_cbCameraBinds[2] = 0;
    g_vsSlot2Last[1] = nullptr; g_vsSlot2Last[2] = nullptr;
    g_vsSlot2At[1] = 0; g_vsSlot2At[2] = 0;
    g_srvTargetMask[1] = 0;
    g_srvTargetMask[2] = 0;
}

void NoteThread(Interval& in, unsigned int tid) {
    for (int i = 0; i < in.threadCount; ++i) {
        if (in.threadIds[i] == tid) { ++in.threadHits[i]; return; }
    }
    if (in.threadCount < kMaxIntervalThreads) {
        in.threadIds[in.threadCount] = tid;
        in.threadHits[in.threadCount] = 1;
        ++in.threadCount;
    } else {
        ++in.threadOverflow;
    }
}

// ---------------------------------------------------------------------------
// M2. THE SUBSTITUTION. Everything below is the one behavioural change; the
// census above is untouched and still records what actually happened, so the
// same run carries the measurement and the intervention.
std::atomic_int g_substituteWanted{0};
std::atomic_bool g_substituteArmed{true};
std::atomic_bool g_substitutePoisoned{false};
std::atomic_bool g_burstCaptureWanted{false};
// T-A0. Zero means "not from a burst", which is what an unlabelled capture used
// to be; the filename says so rather than implying it.
std::atomic_int g_burstCaptureShot{0};

// The composite target as seen on ORDINARY frames -- the rtv0 of the last
// transition of the interval. Published so the private target can be built on
// the plugin tick, before any double, rather than discovered halfway through
// the first one.
std::atomic_int g_compositeTargetId{-1};
// How recently a doubled submission happened, in Present intervals. The
// structural marker alone would be enough, but the substitution is a
// behavioural change and a second, independent precondition costs nothing:
// nothing is substituted unless a double is also known to be in flight.
std::atomic_int g_intervalsSinceDouble{9999};
constexpr int kDoubleFreshnessIntervals = 4;

ID3D11Texture2D* g_privateTex = nullptr;
ID3D11RenderTargetView* g_privateRtv = nullptr;
int g_privateFromTargetId = -1;
// The victim as actually observed at a batch-2 open, or -1 before the first
// doubled frame of the session. Authoritative over the end-of-interval seed.
std::atomic_int g_observedVictimId{-1};
bool g_privateTried = false;

std::atomic_uint64_t g_batch2Opens{0};
std::atomic_uint64_t g_substitutions{0};
std::atomic_uint64_t g_clears{0};
// EVERY DECLINE IS COUNTED AND NAMED. A substitution that silently never
// happens looks exactly like one that happened and did nothing.
std::atomic_uint64_t g_declineNoPrivate{0};
std::atomic_uint64_t g_declineDisarmed{0};
std::atomic_uint64_t g_declinePoisoned{0};
std::atomic_uint64_t g_declineStaleDouble{0};
std::atomic_uint64_t g_declineVictimMismatch{0};
std::atomic_uint64_t g_batch2SpansNotVictim{0};

bool SubstitutionLive() {
    if (!g_substituteWanted.load(std::memory_order_relaxed)) return false;
    if (g_substitutePoisoned.load(std::memory_order_relaxed)) { ++g_declinePoisoned; return false; }
    if (!g_substituteArmed.load(std::memory_order_relaxed)) { ++g_declineDisarmed; return false; }
    if (!g_privateRtv) { ++g_declineNoPrivate; return false; }
    if (g_intervalsSinceDouble.load(std::memory_order_relaxed) > kDoubleFreshnessIntervals) {
        ++g_declineStaleDouble;
        return false;
    }
    return true;
}

// Returns true when rtv0 should be replaced; sets hdrOut when the replacement is
// the HDR clone rather than the LDR one.
bool RecordTransition(ID3D11DeviceContext* context, UINT numViews,
                      ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
                      bool* hdrOut = nullptr, bool* preDrawOut = nullptr) {
    const unsigned long long draws = GameDrawIndexedTally();
    const unsigned int tid = GetCurrentThreadId();
    bool substitute = false;
    bool substituteHdr = false;
    bool clearNow = false;
    bool preDrawWrite = false;
    {
        Guard guard;
        Interval& in = g_intervals[g_cur];
        ++in.transitions;
        NoteThread(in, tid);
        if (in.count > 0) {
            Span& prev = in.spans[in.count - 1];
            prev.draws = static_cast<unsigned int>(draws - prev.drawsAtEntry);
        }
        const unsigned char rtv0 = (numViews >= 1 && rtvs) ? ResolveView(rtvs[0], false) : kNoTarget;
        const unsigned char rtv1 = (numViews >= 2 && rtvs) ? ResolveView(rtvs[1], false) : kNoTarget;
        const unsigned char dsvId = ResolveView(dsv, true);

        // BATCH 1'S SCENE TARGET, LATCHED FROM ITS SIGNATURE RATHER THAN NAMED.
        //
        // The scene pass is the one that binds TWO colour targets at once --
        // `03+05/04` in the census's own notation, and the only span shape in
        // the frame that does. So the first two-RTV binding of batch 1 names the
        // target batch 1 renders its world into, per frame, with no address and
        // no format written down anywhere.
        //
        // This is the target batch 2's post chain samples: 534 matches a frame
        // on each batch, and t03 in BOTH sampled sets. Batch 2's own scene went
        // to our private copy, so that target still holds batch 1's world, and
        // reading it is how batch 1's picture gets composited into our eye.
        if (g_batch.load(std::memory_order_relaxed) == 1 && rtv0 != kNoTarget &&
            rtv1 != kNoTarget && g_batch1SceneTargetId == kNoTarget) {
            g_batch1SceneTargetId = rtv0;
            if (dsvId != kNoTarget) g_sceneDepthPublished.store(dsvId, std::memory_order_release);
            // ANNOUNCED ONCE, so "the latch never fired" and "the latch fired on
            // the wrong target" stop looking identical from the log. The
            // decline path below says the same thing from the other side; one
            // of the two lines must appear, and the last run printed neither.
            if (g_sceneTargetPublished.exchange(rtv0, std::memory_order_acq_rel) != rtv0) {
                char line[300]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] M5: batch 1's scene target identified by signature as t%02u "
                    "(the first binding of two colour targets at once, rtv1 = t%02u).\n",
                    rtv0, rtv1);
                Tf2VrLog(line);
            }
        }

        // THE BATCH-2 MARKER. No RTV and a DSV bound: the shadow pass, and a
        // frame has exactly one. The second one in an interval is batch 2
        // starting over. No target is named, no address is written down.
        if (rtv0 == kNoTarget && dsvId != kNoTarget) {
            if (++g_depthOnlyOpens == 2) {
                g_batch.store(2, std::memory_order_release);
                g_batch2StartSpan = in.count;
                // THE VICTIM, derived rather than declared: the colour target
                // batch 1 left bound. That is batch 1's finished composite, and
                // it is what batch 2 will render the whole frame into.
                g_victimTargetId = g_lastRtv0;
                // RECORDED BEFORE THE MISMATCH GUARD CAN ZERO IT. The VR profile
                // ran 5962 batch-2 opens and made ZERO substitutions, every one
                // refused as a victim mismatch, because the private target is
                // cloned from "the colour target the interval ENDS bound to"
                // and the victim is what batch 1 leaves bound MID-frame. At
                // 2560x1440 flat those are the same target. At 5840x3648 they
                // are not, and the guard did its job silently and correctly
                // while true stereo never once ran on the headset.
                //
                // So the victim names itself here, and the tick retargets the
                // clone to it. The heuristic stays as the SEED -- it is what
                // lets the first burst of a session work at all -- and this is
                // the correction when the seed was wrong.
                g_observedVictimId.store(g_victimTargetId, std::memory_order_release);
                g_batch2Opens.fetch_add(1, std::memory_order_relaxed);
                if (g_victimTargetId != kNoTarget &&
                    g_privateFromTargetId >= 0 &&
                    g_victimTargetId != static_cast<unsigned char>(g_privateFromTargetId)) {
                    // The private target was built from a different texture's
                    // desc. Refuse rather than bind a mismatched target.
                    g_declineVictimMismatch.fetch_add(1, std::memory_order_relaxed);
                    g_victimTargetId = kNoTarget;
                } else if (SubstitutionLive()) {
                    clearNow = true;
                }
            }
        }
        if (rtv0 != kNoTarget) g_lastRtv0 = rtv0;

        substitute = g_batch.load(std::memory_order_relaxed) == 2 && rtv0 != kNoTarget && rtv0 == g_victimTargetId &&
                     SubstitutionLive();
        // WHICH private target, decided POSITIONALLY rather than by rtv1.
        //
        // The old rule was "two colour targets bound => the scene pass => HDR,
        // otherwise LDR". It is too crude, and the wearer found the symptom
        // before I found the rule: "There is supposed to be flame on that mech
        // in the distance that I don't see", and the eye looking muted.
        //
        // Batch 1's sequence binds its SCENE target on plenty of single-RTV
        // spans too -- the transparents and the effect stages, which is exactly
        // where additive fire lives. In batch 2 those same spans bind the
        // composite, so the rtv1 test routed them to the LDR clone: additive
        // HDR effects accumulating into an 8-bit sRGB target, which is both the
        // missing flame and the muting.
        //
        // Batch 2's span sequence mirrors batch 1's one-for-one -- M1 measured
        // that and M2 has relied on it 500 times a run -- so the honest rule is
        // to ask what BATCH 1 bound at the mirrored position and send batch 2's
        // write to the clone of that. No span shape is guessed at.
        substituteHdr = false;
        if (substitute && g_privateHdrRtv && g_batch2StartSpan >= 0) {
            const int mirror = in.count - g_batch2StartSpan;
            if (mirror >= 0 && mirror < in.count) {
                const unsigned char batch1Here = in.spans[mirror].rtv0;
                substituteHdr = batch1Here != kNoTarget &&
                                batch1Here == g_batch1SceneTargetId;
            }
        }
        if (g_batch.load(std::memory_order_relaxed) == 2 && rtv0 != kNoTarget && rtv0 != g_victimTargetId) {
            g_batch2SpansNotVictim.fetch_add(1, std::memory_order_relaxed);
        }

        if (in.count < kMaxSpans) {
            Span& s = in.spans[in.count++];
            s.rtvCount = static_cast<unsigned char>(numViews > 255 ? 255 : numViews);
            s.rtv0 = rtv0;
            s.rtv1 = rtv1;
            s.dsv = dsvId;
            s.threadId = tid;
            s.drawsAtEntry = draws;
            s.draws = 0;
            // FILLED FROM THE DECISION THAT HAS JUST BEEN MADE, in this same
            // critical section, so it is the forwarding this span will actually
            // receive rather than a guess reconstructed later. The forwarding
            // itself reads exactly these two flags at the OMSetRenderTargets
            // call below; if that ever stops being true these fields lie, so
            // they are set here and nowhere else.
            s.batch = static_cast<unsigned char>(g_batch.load(std::memory_order_relaxed));
            s.routed = kRoutedNone;
            if (substitute) {
                s.routed |= substituteHdr ? kRoutedRtvHdr : kRoutedRtvLdr;
                if (substituteHdr && g_privateDepthDsv) s.routed |= kRoutedDepth;
            }
        }
        if (rtv0 != kNoTarget) ++g_targets[rtv0].bindings;
        if (rtv1 != kNoTarget) ++g_targets[rtv1].bindings;
        if (dsvId != kNoTarget) ++g_targets[dsvId].bindings;
    }
    // OUTSIDE THE LOCK. One clear per doubled frame, on our own view, so the
    // private target holds THIS frame's batch 2 rather than an accumulation of
    // every burst since the session started. The engine clears its own target
    // through a view we never see, so nothing else would.
    if (clearNow) {
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        context->ClearRenderTargetView(g_privateRtv, black);
        // BOTH targets. Batch 2 now renders its scene into the HDR clone and its
        // composite into the LDR one, so leaving either uncleared carries the
        // previous burst forward into this frame's eye.
        if (g_privateHdrRtv) context->ClearRenderTargetView(g_privateHdrRtv, black);
        // Depth to 1.0 and stencil to 0, the ordinary scene-start state. Without
        // this the private depth carries the previous doubled frame forward and
        // z-fails this one in the same way sharing batch 1's did.
        if (g_privateDepthDsv) {
            context->ClearDepthStencilView(g_privateDepthDsv,
                                           D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        }
        g_clears.fetch_add(1, std::memory_order_relaxed);
    }
    // M5's PRE-DRAW CAMERA WRITE, fired at the transition that OPENS batch 2's
    // scene span and nowhere else.
    //
    // Every previous attempt patched an upload tagged batch 2 and assumed batch
    // 2's draws would read it. The span census says they cannot: both batches
    // upload their world camera AFTER their own heavy scene span, so a draw
    // reads a camera from earlier in the pipeline and the batch tag is off by a
    // stage. Writing here removes the inference -- these draws have not happened
    // yet, and this is the last write to that buffer before they do.
    preDrawWrite = substituteHdr && !g_preDrawDoneThisSpan;
    if (substituteHdr) g_preDrawDoneThisSpan = true;
    if (substitute) {
        g_substitutions.fetch_add(1, std::memory_order_relaxed);
        ++g_substitutionsThisInterval;
        const int b = g_batch.load(std::memory_order_relaxed);
        if (b >= 1 && b <= 2) g_writeSubsByBatch[b].fetch_add(1, std::memory_order_relaxed);
        (substituteHdr ? g_hdrRoutes : g_ldrRoutes).fetch_add(1, std::memory_order_relaxed);
    }
    if (hdrOut) *hdrOut = substituteHdr;
    if (preDrawOut) *preDrawOut = preDrawWrite;
    return substitute;
}

// SEH LIVES HERE, AND ONLY AROUND THE SUBSTITUTED FORWARD. The unsubstituted
// path stays unguarded so a real driver fault still produces a real dump. No
// C++ object with a destructor may be in scope in a function with __try.
bool ForwardSubstitutedGuarded(OmSetRenderTargetsFn fn, ID3D11DeviceContext* context,
                               UINT numViews, ID3D11RenderTargetView* const* rtvs,
                               ID3D11DepthStencilView* dsv, DWORD* codeOut) {
    __try {
        fn(context, numViews, rtvs, dsv);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *codeOut = GetExceptionCode();
        return false;
    }
}

void PoisonSubstitution(DWORD code) {
    g_substitutePoisoned.store(true, std::memory_order_release);
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] M2: the substituted OMSetRenderTargets FAULTED, code 0x%08X. The "
        "substitution is POISONED for this session; the census keeps running and the game "
        "goes back to the unmodified binding.\n", static_cast<unsigned>(code));
    Tf2VrLog(line);
}

// ---------------------------------------------------------------------------
// THE READ-SIDE CENSUS. Which of OUR OWN known render targets does batch 2 bind
// as a shader input, and in which span?
//
// Deliberately narrow: it resolves an SRV only against the resource table the
// render-target census already built, so it says nothing about the thousands of
// ordinary textures the game samples and everything about the one question --
// does batch 2's post chain read a target batch 1 wrote?
PsSetShaderResourcesFn g_origPsSrv = nullptr;
void** g_slotPsSrv = nullptr;
void* g_slotPsSrvOriginal = nullptr;

void STDMETHODCALLTYPE HookVsSetConstantBuffers(ID3D11DeviceContext* context, UINT startSlot,
                                                UINT numBuffers, ID3D11Buffer* const* buffers) {
    PluginCost::Scope costScope(PluginCost::kRenderTargets);
    g_inFlight.fetch_add(1, std::memory_order_acq_rel);
    if (g_active.load(std::memory_order_relaxed) && context == g_censusContext && buffers) {
        const int batch = g_batch.load(std::memory_order_relaxed);
        void* watched = g_cameraBufferWatched.load(std::memory_order_relaxed);
        if (batch >= 1 && batch <= 2) {
            Guard guard;
            ++g_cbBinds[batch];
            // N1-Q2, read-only: whatever ends up in VS slot 2. Not keyed on the
            // watched buffer -- the whole question is whether the thing in the
            // slot IS the watched buffer, and a witness that only records
            // matches can never answer it.
            if (startSlot <= 2u && startSlot + numBuffers > 2u) {
                g_vsSlot2Last[batch] = buffers[2u - startSlot];
                g_vsSlot2At[batch] = g_cbBinds[batch];
            }
            if (watched) {
                for (UINT i = 0; i < numBuffers && i < 16; ++i) {
                    if (buffers[i] == watched) {
                        ++g_cbCameraBinds[batch];
                        g_cbCameraSlot[batch] = static_cast<unsigned char>(startSlot + i);
                        break;
                    }
                }
            }
        }
    }
    costScope.PauseForForward();
    g_origVsCb(context, startSlot, numBuffers, buffers);
    costScope.ResumeAfterForward();
    g_inFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void STDMETHODCALLTYPE HookPsSetShaderResources(ID3D11DeviceContext* context, UINT startSlot,
                                                UINT numViews,
                                                ID3D11ShaderResourceView* const* views) {
    PluginCost::Scope costScope(PluginCost::kRenderTargets);
    g_inFlight.fetch_add(1, std::memory_order_acq_rel);
    // BOTH BATCHES, AND A RAW INVOCATION COUNT. The first build of this was
    // scoped to `g_batch == 2` and reported "batch 2 bound 0 of our targets" on
    // every doubled interval -- a number that cannot be read, because a hook
    // that never fires produces it too. Batch 1's post chain provably samples
    // the HDR scene target to produce the composite, so batch 1 is the control:
    // if IT reads zero, the witness is dead and batch 2's zero means nothing.
    //
    // The same rule this project applies to every other counter, and I left it
    // off the one instrument built to answer the wearer's own observation.
    // A LOCAL COPY, only written when something is actually substituted. The
    // caller's array is the engine's and is never touched.
    ID3D11ShaderResourceView* substituted[8]{};
    bool anySubstituted = false;
    UINT forwardCount = numViews;
    if (g_active.load(std::memory_order_relaxed) && context == g_censusContext && views) {
        const int batch = g_batch.load(std::memory_order_relaxed);
        Guard guard;
        ++g_srvInvocations;
        forwardCount = numViews > 8 ? 8 : numViews;
        for (UINT c = 0; c < forwardCount; ++c) substituted[c] = views[c];
        for (UINT i = 0; i < numViews && i < 8; ++i) {
            if (!views[i]) continue;
            ID3D11Resource* resource = nullptr;
            views[i]->GetResource(&resource);
            if (!resource) continue;
            ++g_srvViewsSeen;
            // THE SUBSTITUTION, SYMMETRIC TO M2'S ON SLOT 33.
            //
            // While in batch 2, an SRV that resolves to the target BATCH 1's
            // scene wrote is handed our private copy instead -- so batch 2's
            // post chain reads batch 2's own world rather than batch 1's.
            //
            // Narrow on purpose: one target, one batch, and only when armed. It
            // is the exact inverse of the write-side redirect that has been
            // shipping since M2, and it is what the read-side census was built
            // to justify rather than assume.
            if (batch == 2 && g_readSubstituteArmed.load(std::memory_order_relaxed) &&
                g_privateSrv && g_batch1SceneTargetId != kNoTarget &&
                g_targets[g_batch1SceneTargetId].resource == resource) {
                substituted[i] = g_privateHdrSrv ? g_privateHdrSrv : g_privateSrv;
                anySubstituted = true;
                g_readSubstitutions.fetch_add(1, std::memory_order_relaxed);
                if (batch >= 1 && batch <= 2)
                    g_readSubsByBatch[batch].fetch_add(1, std::memory_order_relaxed);
            }
            // Match against the render-target table ONLY. An SRV that is not one
            // of our known targets is an ordinary texture and not the question.
            for (int t = 0; t < g_targetCount; ++t) {
                if (g_targets[t].resource != resource) continue;
                if (batch >= 1 && batch <= 2) {
                    ++g_srvMatchesByBatch[batch];
                    // THE DISTINCT SET, NOT A SEQUENTIAL LIST. The note table is
                    // 16 entries and came back filled with repeats of the shadow
                    // atlas, so the interesting target could not appear in it at
                    // all. A bitmask over the target table is complete and
                    // cannot truncate.
                    //
                    // And the self-read check was aimed at the wrong target. It
                    // asked whether batch 2 samples the COMPOSITE it was
                    // redirected away from writing. The ghost does not need
                    // that: batch 2's post samples the HDR SCENE target the way
                    // batch 1's does, and that target still holds BATCH 1's
                    // scene, because batch 2's scene writes went to our private
                    // copy. Reading batch 1's scene and compositing it into our
                    // eye is precisely the faint unshifted crate.
                    if (t < 64) g_srvTargetMask[batch] |= (1ull << t);
                }
                if (batch == 2 && t == g_victimTargetId) ++g_srvSelfReads;
                if (batch == 2 && g_srvNoteCount < kMaxSrvNotes) {
                    g_srvNotes[g_srvNoteCount].targetId = static_cast<unsigned char>(t);
                    g_srvNotes[g_srvNoteCount].slot = static_cast<unsigned char>(startSlot + i);
                    g_srvNotes[g_srvNoteCount].span = g_intervals[g_cur].count;
                    ++g_srvNoteCount;
                } else if (batch == 2) {
                    ++g_srvNoteOverflow;
                }
                break;
            }
            resource->Release();
        }
    }
    costScope.PauseForForward();
    if (anySubstituted) g_origPsSrv(context, startSlot, forwardCount, substituted);
    else                g_origPsSrv(context, startSlot, numViews, views);
    costScope.ResumeAfterForward();
    g_inFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void STDMETHODCALLTYPE HookOmSetRenderTargets(ID3D11DeviceContext* context, UINT numViews,
                                              ID3D11RenderTargetView* const* rtvs,
                                              ID3D11DepthStencilView* dsv) {
    PluginCost::Scope costScope(PluginCost::kRenderTargets);
    g_inFlight.fetch_add(1, std::memory_order_acq_rel);
    bool substitute = false;
    bool substituteHdr = false;
    bool preDrawWrite = false;
    if (g_active.load(std::memory_order_relaxed) && context == g_censusContext) {
        substitute = RecordTransition(context, numViews, rtvs, dsv, &substituteHdr, &preDrawWrite);
    }
    // OUTSIDE THE CENSUS LOCK, and BEFORE the substituted OMSetRenderTargets is
    // forwarded -- so the buffer already holds batch 2's camera by the time the
    // engine binds the targets those draws will use.
    // Publish the camera buffer for the binding census even when the pre-draw
    // write is disarmed, so the census has something to watch for on any run.
    {
        void* watchBuf = nullptr;
        alignas(16) unsigned char probe[1024];
        if (TakeWorldCameraSnapshot(&watchBuf, probe, sizeof(probe)))
            g_cameraBufferWatched.store(watchBuf, std::memory_order_relaxed);
    }
    if (preDrawWrite && g_preDrawArmed.load(std::memory_order_relaxed)) {
        void* buffer = nullptr;
        alignas(16) unsigned char camera[1024];
        const unsigned cap = WorldCameraBufferBytes();
        if (cap <= sizeof(camera) && TakeWorldCameraSnapshot(&buffer, camera, sizeof(camera)) &&
            ApplyEyeTranslationInPlace(camera, g_preDrawIpd.load(std::memory_order_relaxed))) {
            // SUPPRESSED IN THE UPLOAD HOOK, or our own write comes back through
            // it, gets the offset applied a second time, and the eye separates
            // by twice the IPD -- growing every frame the snapshot is retaken.
            SetPreDrawWriteInProgress(true);
            if (g_bindArmed.load(std::memory_order_relaxed) && g_privateCameraCb) {
                // BIND OURS, do not rewrite theirs. The binding census is what
                // licenses this: batch 2 issues ZERO constant-buffer binds
                // (1782 on batch 1 against 0 on batch 2), so its draws inherit
                // batch 1's bindings -- and updating the contents of a bound
                // constant buffer cannot reach them, because the runtime is
                // free to rename the resource and leave the existing binding on
                // the old data. Every camera attempt since M3 opened wrote
                // contents. This changes the binding.
                context->UpdateSubresource(g_privateCameraCb, 0, nullptr, camera, 0, 0);
                ID3D11Buffer* ours = g_privateCameraCb;
                context->VSSetConstantBuffers(g_cameraCbSlot, 1, &ours);
                // The pixel stage reads the camera too on plenty of passes, and
                // binding one without the other would light the geometry from a
                // different eye than it is drawn from.
                context->PSSetConstantBuffers(g_cameraCbSlot, 1, &ours);
                g_cameraBinds.fetch_add(1, std::memory_order_relaxed);
            } else {
                context->UpdateSubresource(static_cast<ID3D11Resource*>(buffer), 0, nullptr, camera, 0, 0);
            }
            SetPreDrawWriteInProgress(false);
            g_preDrawWrites.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_preDrawDeclines.fetch_add(1, std::memory_order_relaxed);
        }
    }
    costScope.PauseForForward();
    if (substitute) {
        // A LOCAL COPY. The caller's array is the engine's and is never written.
        ID3D11RenderTargetView* local[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        const UINT n = numViews > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT
                           ? D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT : numViews;
        for (UINT i = 0; i < n; ++i) local[i] = rtvs ? rtvs[i] : nullptr;
        local[0] = substituteHdr ? g_privateHdrRtv : g_privateRtv;
        // AND THE DEPTH, on the scene pass only. The composite and UI spans keep
        // the engine's depth: they do not depth-test world geometry, and giving
        // them a private buffer would be a change with no reason behind it.
        ID3D11DepthStencilView* useDsv =
            (substituteHdr && g_privateDepthDsv) ? g_privateDepthDsv : dsv;
        DWORD code = 0;
        if (!ForwardSubstitutedGuarded(g_origOm, context, n, local, useDsv, &code)) {
            PoisonSubstitution(code);
            g_origOm(context, numViews, rtvs, dsv);
        }
    } else {
        g_origOm(context, numViews, rtvs, dsv);
    }
    costScope.ResumeAfterForward();
    g_inFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void STDMETHODCALLTYPE HookOmSetRenderTargetsAndUavs(
    ID3D11DeviceContext* context, UINT numViews, ID3D11RenderTargetView* const* rtvs,
    ID3D11DepthStencilView* dsv, UINT uavStart, UINT numUavs,
    ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
    PluginCost::Scope costScope(PluginCost::kRenderTargets);
    g_inFlight.fetch_add(1, std::memory_order_acq_rel);
    if (g_active.load(std::memory_order_relaxed) && context == g_censusContext) {
        // D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL means "leave the bindings
        // alone"; recording that as a transition to nothing would invent one.
        // NOT SUBSTITUTED, deliberately: this entry point fired 0 or 2 times a
        // frame in M1's census against slot 33's ~70, so it is counted and left
        // alone. One behavioural change per run means one call site.
        if (numViews != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
            RecordTransition(context, numViews, rtvs, dsv);
        }
        Guard guard;
        ++g_intervals[g_cur].uavCalls;
    }
    costScope.PauseForForward();
    g_origOmUav(context, numViews, rtvs, dsv, uavStart, numUavs, uavs, counts);
    costScope.ResumeAfterForward();
    g_inFlight.fetch_sub(1, std::memory_order_acq_rel);
}

// ---------------------------------------------------------------------------
// The reporting side. Everything below runs on the presenting thread, outside
// the lock, and only for intervals worth the log lines.
constexpr int kControlDumps = 2;
constexpr int kMaxDoubledDumps = 20;
// The census installs the moment the detours go in, which is the moment a world
// comes up. Give the frame time to settle before taking the control: the first
// intervals after install are not what steady gameplay looks like.
constexpr unsigned kSettleIntervals = 120;

int g_controlsDumped = 0;
int g_doubledDumped = 0;
int g_tailToDump = 0;
unsigned g_intervalsSeen = 0;
unsigned long long g_intervalsWithScene = 0;
unsigned long long g_intervalsWithoutScene = 0;
unsigned long long g_intervalsTallied = 0;
unsigned int g_presentThreadId = 0;
bool g_saidThreads = false;

void AnnounceNewTargets() {
    // Reads the target table without the lock. Registration only ever appends,
    // and it fills a row before bumping the count, so the worst a race can cost
    // is announcing one target an interval later.
    const int count = g_targetCount;
    for (int i = 0; i < count && i < kMaxTargets; ++i) {
        if (g_targets[i].announced) continue;
        g_targets[i].announced = true;
        char line[340]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] RTVCEN target t%02d = %ux%u fmt %u (%s) arr%u mip%u s%u %s%s\n",
            i, g_targets[i].width, g_targets[i].height, g_targets[i].format,
            FormatName(g_targets[i].format), g_targets[i].arraySize, g_targets[i].mips,
            g_targets[i].samples, g_targets[i].depth ? "DEPTH" : "COLOUR",
            g_targets[i].sceneSized ? " <- SCENE-SIZED (matches the requested render size)" : "");
        Tf2VrLog(line);
    }
}

// The ordered sequence, chunked into log lines. Bounded by construction: at
// most kSeqLines lines, and it SAYS where it stopped rather than trailing off.
constexpr int kSeqLines = 10;
void DumpSequence(const Interval& in) {
    char buf[600];
    int used = 0;
    int lines = 0;
    unsigned int lastThread = 0;
    bool first = true;
    for (int i = 0; i < in.count; ++i) {
        const Span& s = in.spans[i];
        char token[64]{};
        char threadMark[20]{};
        if (first || s.threadId != lastThread) {
            std::snprintf(threadMark, sizeof(threadMark), "|T%u|", s.threadId);
            lastThread = s.threadId;
            first = false;
        }
        char rtv0[8]{}, rtv1[8]{}, dsvName[8]{};
        if (s.rtv0 == kNoTarget) std::strcpy(rtv0, "--");
        else std::snprintf(rtv0, sizeof(rtv0), "%02u", s.rtv0);
        if (s.rtv1 != kNoTarget) std::snprintf(rtv1, sizeof(rtv1), "+%02u", s.rtv1);
        if (s.dsv == kNoTarget) std::strcpy(dsvName, "--");
        else std::snprintf(dsvName, sizeof(dsvName), "%02u", s.dsv);
        std::snprintf(token, sizeof(token), "%s%s%s/%s:%u ", threadMark, rtv0, rtv1, dsvName, s.draws);
        const int need = static_cast<int>(std::strlen(token));
        if (used + need >= static_cast<int>(sizeof(buf)) - 2) {
            buf[used] = '\n'; buf[used + 1] = '\0';
            Tf2VrLog(buf);
            used = 0;
            if (++lines >= kSeqLines) {
                char stop[220]{};
                std::snprintf(stop, sizeof(stop),
                    "[TF2VR]   ... sequence CUT at span %d of %d recorded (the %d-line cap). The "
                    "tally below covers ALL recorded spans, not only the printed ones.\n",
                    i, in.count, kSeqLines);
                Tf2VrLog(stop);
                return;
            }
        }
        if (used == 0) { std::strcpy(buf, "[TF2VR]   "); used = 10; }
        std::strcpy(buf + used, token);
        used += need;
    }
    if (used > 10) { buf[used] = '\n'; buf[used + 1] = '\0'; Tf2VrLog(buf); }
}

// Per-target occurrence tally for one interval. THIS is the line that answers
// the post-grading question: if the scene family's occurrence count doubles on
// a doubled frame and the grade/post family's does not, post runs ONCE over
// both batches, and a redirected batch 2 comes out ungraded -- DOOM's wall.
void DumpTally(const Interval& in, const char* label) {
    unsigned int occurrences[kMaxTargets]{};
    unsigned int drawsOn[kMaxTargets]{};
    for (int i = 0; i < in.count; ++i) {
        const Span& s = in.spans[i];
        if (s.rtv0 != kNoTarget && s.rtv0 < kMaxTargets) {
            ++occurrences[s.rtv0];
            drawsOn[s.rtv0] += s.draws;
        }
    }
    char buf[700];
    std::snprintf(buf, sizeof(buf), "[TF2VR] RTVCEN tally[%s itv %u]: ", label, in.presentFrame);
    int used = static_cast<int>(std::strlen(buf));
    for (int i = 0; i < g_targetCount && i < kMaxTargets; ++i) {
        if (!occurrences[i]) continue;
        char token[64]{};
        std::snprintf(token, sizeof(token), "t%02d x%u(di %u)%s ", i, occurrences[i], drawsOn[i],
                      g_targets[i].sceneSized ? "*" : "");
        const int need = static_cast<int>(std::strlen(token));
        if (used + need >= static_cast<int>(sizeof(buf)) - 2) break;
        std::strcpy(buf + used, token);
        used += need;
    }
    buf[used] = '\n'; buf[used + 1] = '\0';
    Tf2VrLog(buf);
}

// S-A. THE TABLE THE MERGED FRONT PICKS ITS TARGET FROM.
//
// The tally above is per-target but batch-blind, and the M2 status carries
// "batch-2 spans on OTHER targets (left alone)" as ONE NUMBER -- 1504 against
// 791 substitutions. An aggregate that large hides which targets it is made of,
// and this project has already been caught by a counter whose named reason was
// 3 of 274.
//
// This splits it: for every target, what batch 1 bound, what batch 2 bound, and
// -- the part the sequence dump structurally cannot say -- whether batch 2's
// write was REDIRECTED or landed in batch 1's own buffer.
//
// BATCH 1 IS THE CONTROL ON EVERY ROW. A table where batch 1 binds nothing is
// instrument failure, not a finding, and the trailer says so rather than
// leaving a reader to notice.
void DumpSharedTargets(const Interval& in, const char* label) {
    unsigned int b1[kMaxTargets]{}, b2[kMaxTargets]{};
    unsigned int d1[kMaxTargets]{}, d2[kMaxTargets]{};
    unsigned int b2Untouched[kMaxTargets]{};
    unsigned int b1Dsv[kMaxTargets]{}, b2Dsv[kMaxTargets]{}, b2DsvUntouched[kMaxTargets]{};
    unsigned int b1Total = 0, b2Total = 0, b2UntouchedTotal = 0;
    for (int i = 0; i < in.count; ++i) {
        const Span& s = in.spans[i];
        const bool two = s.batch == 2;
        if (s.rtv0 != kNoTarget && s.rtv0 < kMaxTargets) {
            (two ? b2 : b1)[s.rtv0] += 1;
            (two ? d2 : d1)[s.rtv0] += s.draws;
            (two ? b2Total : b1Total) += 1;
            if (two && !(s.routed & (kRoutedRtvHdr | kRoutedRtvLdr))) {
                ++b2Untouched[s.rtv0];
                ++b2UntouchedTotal;
            }
        }
        if (s.dsv != kNoTarget && s.dsv < kMaxTargets) {
            (two ? b2Dsv : b1Dsv)[s.dsv] += 1;
            if (two && !(s.routed & kRoutedDepth)) ++b2DsvUntouched[s.dsv];
        }
    }

    char head[420]{};
    std::snprintf(head, sizeof(head),
        "[TF2VR] RTVCEN SHARED[%s itv %u]: rows are t<id> b1=<binds> b2=<binds>(<n> LANDED IN "
        "BATCH 1'S OWN BUFFER) draws b1/b2 | dsv b1=<binds> b2=<binds>(<n> shared). A row with "
        "b2 untouched > 0 is a per-frame resource WRITTEN TWICE.\n",
        label, in.presentFrame);
    Tf2VrLog(head);

    char buf[700];
    int used = 0;
    for (int i = 0; i < g_targetCount && i < kMaxTargets; ++i) {
        if (!b1[i] && !b2[i] && !b1Dsv[i] && !b2Dsv[i]) continue;
        char token[160]{};
        std::snprintf(token, sizeof(token),
            "t%02d b1=%u b2=%u(%u) di %u/%u dsv %u/%u(%u)%s | ",
            i, b1[i], b2[i], b2Untouched[i], d1[i], d2[i], b1Dsv[i], b2Dsv[i], b2DsvUntouched[i],
            g_targets[i].sceneSized ? "*" : "");
        const int need = static_cast<int>(std::strlen(token));
        if (used == 0) { std::strcpy(buf, "[TF2VR]   "); used = 10; }
        if (used + need >= static_cast<int>(sizeof(buf)) - 2) {
            buf[used] = '\n'; buf[used + 1] = '\0'; Tf2VrLog(buf);
            std::strcpy(buf, "[TF2VR]   "); used = 10;
        }
        std::strcpy(buf + used, token);
        used += need;
    }
    if (used > 10) { buf[used] = '\n'; buf[used + 1] = '\0'; Tf2VrLog(buf); }

    char tail[560]{};
    std::snprintf(tail, sizeof(tail),
        "[TF2VR] RTVCEN SHARED VERDICT[itv %u]: batch 1 bound %u colour spans, batch 2 bound %u, "
        "of which %u LANDED IN BATCH 1'S OWN TARGET. %s\n",
        in.presentFrame, b1Total, b2Total, b2UntouchedTotal,
        b1Total == 0
            ? "*** BATCH 1 BOUND NOTHING: the control is zero, this table is instrument failure "
              "and NOTHING may be concluded from it. ***"
            : (b2Total == 0
                   ? "Batch 2 bound nothing -- this interval was not doubled, which is what the "
                     "single-frame baseline is supposed to look like."
                   : "Batch 1 is nonzero, so the row counts are a fact about the frame."));
    Tf2VrLog(tail);
}

void DumpFalsifier() {
    // THE CENSUS OWN POSITIVE CONTROL, and it is not optional. If the census
    // cannot see the engine's own scene target on an ordinary frame, then a
    // doubled frame showing one batch says nothing about batch 2 -- it says the
    // instrument is dead. The same rule that disqualified three upload counters
    // this week.
    char line[760]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] RTVCEN CONTROL: %llu of %llu intervals bound a scene-sized colour target "
        "(%llu did NOT). Distinct targets %d (overflow %llu), view cache %d entries (misses "
        "after full: %llu). DrawIndexed tally armed=%d, session total %llu. A census that "
        "cannot see pass 1's own scene target every frame is BROKEN, and nothing it says "
        "about a second batch counts.\n",
        g_intervalsWithScene, g_intervalsTallied, g_intervalsWithoutScene,
        g_targetCount, g_targetOverflow, g_viewCount, g_viewCacheOverflow,
        GameDrawIndexedTallyArmed() ? 1 : 0, GameDrawIndexedTally());
    Tf2VrLog(line);
    // AND THE CONTROL'S OWN CONTROL. "scene-sized" above is a LABEL, computed
    // against the ini's requested render size -- if the engine renders the
    // world into a target of some other size, that count reads 0 for every
    // interval and looks exactly like a census that sees nothing. So the two
    // targets that could not be missed are named outright: the largest colour
    // target and the most-bound one. If those are populated while the
    // scene-sized count is 0, the LABEL is wrong and the census is fine.
    int largest = -1, mostBound = -1;
    unsigned long long largestPixels = 0, mostBindings = 0;
    for (int i = 0; i < g_targetCount && i < kMaxTargets; ++i) {
        if (g_targets[i].depth) continue;
        const unsigned long long pixels =
            static_cast<unsigned long long>(g_targets[i].width) * g_targets[i].height;
        if (pixels > largestPixels) { largestPixels = pixels; largest = i; }
        if (g_targets[i].bindings > mostBindings) { mostBindings = g_targets[i].bindings; mostBound = i; }
    }
    char second[520]{};
    std::snprintf(second, sizeof(second),
        "[TF2VR] RTVCEN CONTROL b: largest colour target t%02d %ux%u fmt %u (%s), bound %llu "
        "times; most-bound colour target t%02d %ux%u fmt %u, bound %llu times. These two are "
        "the census seeing SOMETHING regardless of whether the scene-sized label above is "
        "computed against the right size.\n",
        largest, largest >= 0 ? g_targets[largest].width : 0,
        largest >= 0 ? g_targets[largest].height : 0,
        largest >= 0 ? g_targets[largest].format : 0,
        FormatName(largest >= 0 ? g_targets[largest].format : 0),
        largest >= 0 ? g_targets[largest].bindings : 0,
        mostBound, mostBound >= 0 ? g_targets[mostBound].width : 0,
        mostBound >= 0 ? g_targets[mostBound].height : 0,
        mostBound >= 0 ? g_targets[mostBound].format : 0, mostBindings);
    Tf2VrLog(second);
}

// THE READ SIDE. Which targets batch 2 SAMPLED, and whether one of them was the
// very buffer we redirected it away from writing.
void DumpReadSide(const Interval& in) {
    char line[600];
    int used = std::snprintf(line, sizeof(line),
        "[TF2VR] RTVCEN READ-SIDE[itv %u]: %llu PSSetShaderResources calls, %llu views%s. "
        "Matches against OUR render targets -- batch 1 (the CONTROL, its post provably samples "
        "the scene target) %llu | batch 2 %llu. Self-reads of the redirected target: %llu.",
        in.presentFrame, g_srvSnapshot.invocations, g_srvSnapshot.views,
        g_slotPsSrv ? "" : " (HOOK NOT INSTALLED -- every number here is meaningless)",
        g_srvSnapshot.matches1, g_srvSnapshot.matches2, g_srvSnapshot.selfReads);
    used += std::snprintf(line + used, sizeof(line) - used, " | batch 1 SAMPLED:");
    for (int t = 0; t < 64 && t < kMaxTargets && used < 470; ++t) {
        if (g_srvSnapshot.mask1 & (1ull << t)) {
            used += std::snprintf(line + used, sizeof(line) - used, " t%02d", t);
        }
    }
    used += std::snprintf(line + used, sizeof(line) - used, " | batch 2 SAMPLED:");
    for (int t = 0; t < 64 && t < kMaxTargets && used < 500; ++t) {
        if (g_srvSnapshot.mask2 & (1ull << t)) {
            used += std::snprintf(line + used, sizeof(line) - used, " t%02d", t);
        }
    }
    std::snprintf(line + used, sizeof(line) - used,
        ". THE TWO SAMPLED SETS ARE THE ANSWER: any target in batch 2's set that batch 1 WROTE is "
        "batch 1's picture being read into our eye. The HDR scene target is the one to look for -- "
        "batch 2's scene went to our private copy, so that target still holds BATCH 1's scene, and "
        "batch 2's post sampling it produces exactly the faint unshifted crate the wearer saw. "
        "Batch 1 matching zero still disqualifies the whole line.\n");
    Tf2VrLog(line);
}

// WHICH BUFFER THE DRAWS ACTUALLY SEE. Batch 1 is the control: its world draws
// provably use the camera this project can move, so batch 1 showing ZERO binds
// of the watched buffer disqualifies the line rather than indicting batch 2.
void DumpBindingCensus(const Interval& in) {
    char line[640]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] RTVCEN BINDINGS[itv %u]: VSSetConstantBuffers calls b1=%llu b2=%llu%s. The camera "
        "buffer we patch (%p) was BOUND b1=%llu times (slot %u) b2=%llu times (slot %u). "
        "Batch 1 at zero disqualifies this line. Batch 1 nonzero with batch 2 ZERO means batch "
        "2's draws never see the buffer every camera attempt has been writing -- which is why "
        "rewriting it immediately before those draws, 120 units, moved nothing.\n",
        in.presentFrame, g_cbSnapshot.binds1, g_cbSnapshot.binds2,
        g_slotVsCb ? "" : " (HOOK NOT INSTALLED -- meaningless)",
        g_cameraBufferWatched.load(std::memory_order_relaxed),
        g_cbSnapshot.cam1, g_cbSnapshot.slot1, g_cbSnapshot.cam2, g_cbSnapshot.slot2);
    Tf2VrLog(line);
    // N1-Q2, ON ITS OWN LINE. What is actually SITTING in VS slot 2, whatever
    // it is -- not "how often the watched buffer was bound there". Compare
    // b1-last against the buffer pointer on the M3 census's "world-camera
    // uploads, span@buffer" line: same pointer means batch 2's draws do read
    // the buffer the engine uploads into and the remaining question is purely
    // ordering; a different pointer means every camera attempt so far wrote a
    // buffer those draws never sample, and C-BOUND is live.
    char q2[620]{};
    std::snprintf(q2, sizeof(q2),
        "[TF2VR] RTVCEN Q2 VS-SLOT-2[itv %u]: b1 last=%p (at bind %llu of %llu), b2 last=%p (at "
        "bind %llu of %llu). Batch 2 binding NOTHING is the expected reading, and then b1's "
        "pointer is what batch 2's draws inherit. b1 last=NULL means this witness saw no slot-2 "
        "bind at all on the control and neither pointer here means anything. PS slot 2 is NOT "
        "measured -- no PSSetConstantBuffers hook exists -- so this answers Q2 for the vertex "
        "stage only.\n",
        in.presentFrame, g_cbSnapshot.vsSlot2Last1, g_cbSnapshot.vsSlot2At1, g_cbSnapshot.binds1,
        g_cbSnapshot.vsSlot2Last2, g_cbSnapshot.vsSlot2At2, g_cbSnapshot.binds2);
    Tf2VrLog(q2);
}

void DumpInterval(const Interval& in, const char* label) {
    unsigned long long sceneBindings = 0;
    unsigned long long totalDraws = 0;
    for (int i = 0; i < in.count; ++i) {
        totalDraws += in.spans[i].draws;
        if (in.spans[i].rtv0 != kNoTarget && in.spans[i].rtv0 < kMaxTargets &&
            g_targets[in.spans[i].rtv0].sceneSized) {
            ++sceneBindings;
        }
    }
    char head[820]{};
    std::snprintf(head, sizeof(head),
        "[TF2VR] RTVCEN seq[%s itv %u]: %llu transitions (%d recorded, %s), %llu UAV-form calls, "
        "%llu DrawIndexed in the interval, %llu of the transitions bound a SCENE-SIZED colour "
        "target. Threads seen: %u/%u %u/%u %u/%u (id/hits), Present on %u. Token is "
        "rtv0[+rtv1]/dsv:DrawIndexed-in-span; -- is none. NOTE: only DrawIndexed is counted, so "
        "a fullscreen post pass drawn with Draw() reads 0 and that is not silence.\n",
        label, in.presentFrame, in.transitions, in.count,
        in.transitions > static_cast<unsigned long long>(in.count) ? "TRUNCATED at the span cap"
                                                                   : "complete",
        in.uavCalls, totalDraws, sceneBindings,
        in.threadCount > 0 ? in.threadIds[0] : 0, in.threadCount > 0 ? in.threadHits[0] : 0,
        in.threadCount > 1 ? in.threadIds[1] : 0, in.threadCount > 1 ? in.threadHits[1] : 0,
        in.threadCount > 2 ? in.threadIds[2] : 0, in.threadCount > 2 ? in.threadHits[2] : 0,
        g_presentThreadId);
    Tf2VrLog(head);
    DumpSequence(in);
    DumpTally(in, label);
    DumpSharedTargets(in, label);
    DumpReadSide(in);
    DumpBindingCensus(in);
}

}  // namespace

// Forward-declared at GLOBAL scope, which is where its definition lives. The
// first attempt put this inside the anonymous namespace and produced a SECOND,
// never-defined function of the same name -- the compiler called it ambiguous
// rather than silently picking one, which is the only reason it was noticed.
void EnsureHdrPrivateTarget();

void SetRtvCensusWanted(int mode) {
    g_wanted.store(mode, std::memory_order_release);
    char line[460]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] stereo.stereo_targets = %d. %s\n", mode,
        mode ? "M1: OMSetRenderTargets (slot 33) and its UAV form (34) are censused by "
               "per-object vtable slot swap. READ-ONLY -- no argument is modified and both "
               "forward unchanged. Slots 12/13 and the context vptr are not touched."
             : "OFF; nothing is hooked.");
    Tf2VrLog(line);
}

bool RtvCensusWanted() { return g_wanted.load(std::memory_order_acquire) != 0; }

void RtvCensusNoteDoubledSubmission() {
    g_doubledSubmissions.fetch_add(1, std::memory_order_release);
    // FRESHNESS IS MARKED HERE, NOT AT THE NEXT PRESENT, and the difference
    // cost the whole of M2's first run.
    //
    // The freshness guard was belt-and-braces: never substitute unless a double
    // is known to be in flight. But the counter it reads was only reset in
    // RtvCensusOnPresent -- i.e. AFTER the interval containing the batches had
    // already closed. So on the first doubled interval of every burst the guard
    // read "stale" and declined, by construction, every time. The run said so
    // in numbers I nearly read past: clears=7 against batch2 opens=8, and 14
    // against 16. Exactly one missed interval per press, always the first.
    //
    // And the proof capture is pinned to the first shot, so both captured
    // frames were UNSUBSTITUTED and the image pair tested nothing at all.
    g_intervalsSinceDouble.store(0, std::memory_order_release);
}

void InstallRtvCensus(ID3D11DeviceContext* context, void** vtable) {
    if (!RtvCensusWanted() || g_active.load(std::memory_order_acquire)) return;
    if (!context || !vtable) return;
    void** slot33 = &vtable[kOmSetRenderTargetsSlot];
    void** slot34 = &vtable[kOmSetRenderTargetsAndUavsSlot];
    // REFUSE IF EITHER ALREADY HOLDS OURS. Reading our own stub back as the
    // original builds a call that recurses into itself.
    if (*slot33 == reinterpret_cast<void*>(&HookOmSetRenderTargets) ||
        *slot34 == reinterpret_cast<void*>(&HookOmSetRenderTargetsAndUavs)) {
        Tf2VrLog("[TF2VR] RTVCEN: a slot already holds our own hook; NOT installed.\n");
        return;
    }
    // The slots must point into d3d11.dll, or this is not the table the offline
    // read named and nothing here is safe to call. The same check the JT census
    // makes against tier0.
    HMODULE d3d11 = GetModuleHandleA("d3d11.dll");
    MEMORY_BASIC_INFORMATION mbi{};
    if (!d3d11 || !VirtualQuery(*slot33, &mbi, sizeof(mbi)) || mbi.AllocationBase != d3d11) {
        char line[240]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] RTVCEN: slot 33 holds %p, which is not inside d3d11.dll; NOT installed.\n",
            *slot33);
        Tf2VrLog(line);
        return;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot33, sizeof(void*) * 2, PAGE_READWRITE, &oldProtect)) {
        Tf2VrLog("[TF2VR] RTVCEN: the vtable slots would not go writable; NOT installed.\n");
        return;
    }
    g_origOm = reinterpret_cast<OmSetRenderTargetsFn>(*slot33);
    g_origOmUav = reinterpret_cast<OmSetRenderTargetsAndUavsFn>(*slot34);
    g_slot33Original = *slot33;
    g_slot34Original = *slot34;
    *slot33 = reinterpret_cast<void*>(&HookOmSetRenderTargets);
    *slot34 = reinterpret_cast<void*>(&HookOmSetRenderTargetsAndUavs);
    DWORD ignored = 0;
    VirtualProtect(slot33, sizeof(void*) * 2, oldProtect, &ignored);
    // THE BINDING CENSUS, installed the same way and equally read-only.
    {
        void** slotCb = &vtable[kVsSetConstantBuffersSlot];
        if (*slotCb != reinterpret_cast<void*>(&HookVsSetConstantBuffers)) {
            DWORD cbProtect = 0;
            if (VirtualProtect(slotCb, sizeof(void*), PAGE_READWRITE, &cbProtect)) {
                g_origVsCb = reinterpret_cast<VsSetConstantBuffersFn>(*slotCb);
                g_slotVsCbOriginal = *slotCb;
                *slotCb = reinterpret_cast<void*>(&HookVsSetConstantBuffers);
                DWORD cbIgnored = 0;
                VirtualProtect(slotCb, sizeof(void*), cbProtect, &cbIgnored);
                g_slotVsCb = slotCb;
            } else {
                Tf2VrLog("[TF2VR] RTVCEN: the VSSetConstantBuffers slot would not go writable; the "
                         "binding census is NOT installed and its zeros mean nothing.\n");
            }
        }
    }
    // THE READ SIDE, installed the same way and equally read-only.
    {
        void** slotSrv = &vtable[kPsSetShaderResourcesSlot];
        if (*slotSrv != reinterpret_cast<void*>(&HookPsSetShaderResources)) {
            DWORD srvProtect = 0;
            if (VirtualProtect(slotSrv, sizeof(void*), PAGE_READWRITE, &srvProtect)) {
                g_origPsSrv = reinterpret_cast<PsSetShaderResourcesFn>(*slotSrv);
                g_slotPsSrvOriginal = *slotSrv;
                *slotSrv = reinterpret_cast<void*>(&HookPsSetShaderResources);
                DWORD srvIgnored = 0;
                VirtualProtect(slotSrv, sizeof(void*), srvProtect, &srvIgnored);
                g_slotPsSrv = slotSrv;
            } else {
                Tf2VrLog("[TF2VR] RTVCEN: the PSSetShaderResources slot would not go writable; "
                         "the read-side census is NOT installed and its zero means nothing.\n");
            }
        }
    }
    g_slot33 = slot33;
    g_slot34 = slot34;
    g_censusContext = context;
    g_targetCount = 0;
    g_viewCount = 0;
    g_targetOverflow = 0;
    g_viewCacheOverflow = 0;
    g_intervalsSeen = 0;
    ResetInterval(g_intervals[0]);
    ResetInterval(g_intervals[1]);
    // THE DRAW TALLY IS ARMED SEPARATELY, and this is why: the shipped
    // DrawIndexed counter only moves while a bounded camera trace is running,
    // so on a census run it reads zero -- a per-span draw count of 0 everywhere
    // would look like "nothing drew" rather than "nobody was counting".
    SetGameDrawIndexedTally(true);
    g_active.store(true, std::memory_order_release);
    char line[560]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] RTVCEN installed by vtable slot swap: slot 33 OMSetRenderTargets was %p, slot 34 "
        "OMSetRenderTargetsAndUnorderedAccessViews was %p, on the immediate context %p. "
        "Read-only; both forward unchanged. Slots 12/13 and the vptr are NOT touched -- those "
        "are the two things that faulted the driver on 2026-08-14.\n",
        g_slot33Original, g_slot34Original, static_cast<void*>(context));
    Tf2VrLog(line);
}

void RemoveRtvCensus() {
    if (!g_slot33 && !g_slot34) return;
    g_active.store(false, std::memory_order_release);
    SetGameDrawIndexedTally(false);
    if (g_inFlight.load(std::memory_order_acquire) != 0) {
        Tf2VrLog("[TF2VR] RTVCEN: a call is still in flight; the slots stay ours this pass.\n");
        return;
    }
    DWORD oldProtect = 0;
    if (VirtualProtect(g_slot33, sizeof(void*) * 2, PAGE_READWRITE, &oldProtect)) {
        // Only if the slots still hold OURS: another component may have swapped
        // after us, and writing the saved pointer over theirs unhooks them.
        if (*g_slot33 == reinterpret_cast<void*>(&HookOmSetRenderTargets)) *g_slot33 = g_slot33Original;
        if (*g_slot34 == reinterpret_cast<void*>(&HookOmSetRenderTargetsAndUavs)) *g_slot34 = g_slot34Original;
        DWORD ignored = 0;
        VirtualProtect(g_slot33, sizeof(void*) * 2, oldProtect, &ignored);
    }
    g_slot33 = nullptr;
    g_slot34 = nullptr;
    g_censusContext = nullptr;
    DumpFalsifier();
}

void RtvCensusOnPresent() {
    if (!g_active.load(std::memory_order_acquire)) return;
    g_presentThreadId = GetCurrentThreadId();
    int done;
    {
        Guard guard;
        Interval& in = g_intervals[g_cur];
        if (in.count > 0) {
            Span& last = in.spans[in.count - 1];
            last.draws = static_cast<unsigned int>(GameDrawIndexedTally() - last.drawsAtEntry);
        }
        // READ THE CLOSING INTERVAL'S ARM LABEL BEFORE THE RESET WIPES IT. The
        // capture is serviced from this value, so the image can never be
        // compared under an arm it did not actually have.
        g_capturedIntervalSubstitutions.store(g_substitutionsThisInterval, std::memory_order_release);
        g_capturedIntervalOpens.store(g_depthOnlyOpens, std::memory_order_release);
        // AND THE READ-SIDE COUNTERS, FOR EXACTLY THE SAME REASON -- which I
        // wrote three lines above and then did not apply eight lines below.
        //
        // DumpReadSide reads globals, and ResetInterval zeroes those globals
        // HERE, at the flip, before the dump runs. So the read-side census
        // could only ever print zeros: not "the hook never fired", but "the
        // numbers were cleared and then printed". The previous run's zero was
        // void twice over -- once for having no control, and once for this.
        g_srvSnapshot.invocations = g_srvInvocations;
        g_srvSnapshot.views = g_srvViewsSeen;
        g_srvSnapshot.matches1 = g_srvMatchesByBatch[1];
        g_srvSnapshot.matches2 = g_srvMatchesByBatch[2];
        g_srvSnapshot.selfReads = g_srvSelfReads;
        g_srvSnapshot.mask1 = g_srvTargetMask[1];
        g_srvSnapshot.mask2 = g_srvTargetMask[2];
        g_cbSnapshot.binds1 = g_cbBinds[1];
        g_cbSnapshot.binds2 = g_cbBinds[2];
        g_cbSnapshot.cam1 = g_cbCameraBinds[1];
        g_cbSnapshot.cam2 = g_cbCameraBinds[2];
        g_cbSnapshot.slot1 = g_cbCameraSlot[1];
        g_cbSnapshot.slot2 = g_cbCameraSlot[2];
        g_cbSnapshot.vsSlot2Last1 = g_vsSlot2Last[1];
        g_cbSnapshot.vsSlot2Last2 = g_vsSlot2Last[2];
        g_cbSnapshot.vsSlot2At1 = g_vsSlot2At[1];
        g_cbSnapshot.vsSlot2At2 = g_vsSlot2At[2];
        g_srvSnapshot.noteCount = g_srvNoteCount;
        for (int i = 0; i < g_srvNoteCount && i < kMaxSrvNotes; ++i) {
            g_srvSnapshot.notes[i] = g_srvNotes[i];
        }
        done = g_cur;
        g_cur ^= 1;
        ResetInterval(g_intervals[g_cur]);
    }
    // SECOND CALL SITE, deliberately. The tick is supposed to do this every
    // frame and the last run shows it did not get there, so Present -- which
    // demonstrably runs, since every census line in the log is printed from it
    // -- tries as well. The function is idempotent and self-limiting.
    EnsureHdrPrivateTarget();
    const Interval& in = g_intervals[done];
    ++g_intervalsSeen;
    // M3. The origin census belongs to the frame, so it is reported and reset at
    // the frame boundary -- and only this side knows whether the interval that
    // just closed was doubled.
    ReportBatchCameraOrigins(g_capturedIntervalOpens.load(std::memory_order_acquire) >= 2);

    // THE COMPOSITE, published from ORDINARY frames. The victim can only be
    // named halfway through a doubled interval, which is far too late to
    // allocate a texture -- so the same target is identified here, every frame,
    // as the colour target the interval ends bound to, and the tick builds the
    // private copy from it long before the first press.
    for (int i = in.count - 1; i >= 0; --i) {
        if (in.spans[i].rtv0 != kNoTarget) {
            // THE SEED ONLY. Once a doubled frame has NAMED the victim, that
            // observation wins -- otherwise this heuristic would overwrite the
            // retarget on the very next Present and the clone would rebuild
            // from the wrong target forever.
            if (g_observedVictimId.load(std::memory_order_acquire) < 0)
                g_compositeTargetId.store(in.spans[i].rtv0, std::memory_order_release);
            break;
        }
    }
    {
        const unsigned long long doubles = g_doubledSubmissions.load(std::memory_order_acquire);
        static unsigned long long lastDoubles = 0;
        if (doubles != lastDoubles) { lastDoubles = doubles; g_intervalsSinceDouble.store(0, std::memory_order_release); }
        else {
            const int n = g_intervalsSinceDouble.load(std::memory_order_relaxed);
            if (n < 9999) g_intervalsSinceDouble.store(n + 1, std::memory_order_release);
        }
    }

    // The always-on falsifier tally. Every interval, dumped or not.
    bool sawScene = false;
    for (int i = 0; i < in.count; ++i) {
        const unsigned char id = in.spans[i].rtv0;
        if (id != kNoTarget && id < kMaxTargets && g_targets[id].sceneSized) { sawScene = true; break; }
    }
    ++g_intervalsTallied;
    if (sawScene) ++g_intervalsWithScene; else ++g_intervalsWithoutScene;

    AnnounceNewTargets();

    if (!g_saidThreads && in.threadCount > 0) {
        g_saidThreads = true;
        char line[560]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] RTVCEN THREADS: the first interval's render-target transitions came from "
            "thread %u (%u of them); Present is on thread %u. The same id means the batches "
            "execute on the presenting thread; different ids mean the census and the interval "
            "boundary sit on different threads and the sequence is read with that in mind.\n",
            in.threadIds[0], in.threadHits[0], g_presentThreadId);
        Tf2VrLog(line);
    }

    const unsigned long long doublesNow = g_doubledSubmissions.load(std::memory_order_acquire);
    const bool sawDouble = doublesNow > in.doublesAtOpen;

    if (sawDouble && g_doubledDumped < kMaxDoubledDumps) {
        ++g_doubledDumped;
        // A CAP HIT EXACTLY IS TRUNCATION, and it has to say so out loud: without
        // this line a fourth press would produce no dumps and look exactly like
        // a press that armed nothing.
        if (g_doubledDumped == kMaxDoubledDumps) {
            Tf2VrLog("[TF2VR] RTVCEN: the doubled-interval dump cap of 20 is now REACHED. "
                     "Later doubled intervals are still counted and still tallied by the "
                     "CONTROL line, but their sequences are NOT printed. This is truncation, "
                     "not silence.\n");
        }
        g_tailToDump = 2;
        DumpInterval(in, "DOUBLED-SUBMISSION");
        DumpFalsifier();
    } else if (g_tailToDump > 0) {
        --g_tailToDump;
        // NOT A SPARE DUMP. The submission happens on one thread and the batches
        // execute on another, so the batches belonging to a doubled submission
        // can land in the NEXT interval. Reading only the submission interval
        // would be the same class of error as attributing asynchronous uploads
        // to whichever window they arrived in.
        DumpInterval(in, "TAIL-AFTER-DOUBLE");
    } else if (!sawDouble && g_controlsDumped < kControlDumps &&
               g_intervalsSeen > kSettleIntervals && sawScene) {
        ++g_controlsDumped;
        DumpInterval(in, "CONTROL");
        if (g_controlsDumped == kControlDumps) DumpFalsifier();
    } else if ((g_intervalsSeen % 900) == 0) {
        DumpFalsifier();
    }
}

// ---------------------------------------------------------------------------
// M2's public surface.

void SetRtvSubstituteWanted(int mode) {
    g_substituteWanted.store(mode, std::memory_order_release);
    char line[540]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] stereo.substitute = %d. %s\n", mode,
        mode ? "M2: while the census says batch 2, the colour target batch 2 would scribble on "
               "is replaced with a private one. THE FALSIFIER IS VISIBLE: the crates artefact "
               "disappears and the weapon goes opaque again in a doubled frame. F5 "
               "disarms it mid-session and the artefact must come back."
             : "OFF: the census records and changes nothing (M1 behaviour).");
    Tf2VrLog(line);
}

void ToggleRtvSubstitute() {
    if (!g_substituteWanted.load(std::memory_order_acquire)) {
        Tf2VrLog("[TF2VR] M2: F5 pressed, but stereo.substitute is 0. Nothing to toggle.\n");
        return;
    }
    const bool on = !g_substituteArmed.load(std::memory_order_acquire);
    g_substituteArmed.store(on, std::memory_order_release);
    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] M2 substitution %s (F5). %s\n", on ? "ARMED" : "DISARMED",
        on ? "The next doubled frame sends batch 2 to the private target again."
           : "THIS IS THE POSITIVE CONTROL: the next doubled frame must put the crates back "
             "through the weapon. If the artefact does NOT return, the substitution was never "
             "what removed it.");
    Tf2VrLog(line);
}

bool RtvSubstituteArmed() {
    return g_substituteWanted.load(std::memory_order_acquire) != 0 &&
           g_substituteArmed.load(std::memory_order_acquire) &&
           !g_substitutePoisoned.load(std::memory_order_acquire);
}

void RtvCensusRequestBurstCapture(int shot) {
    g_burstCaptureShot.store(shot, std::memory_order_release);
    g_burstCaptureWanted.store(true, std::memory_order_release);
}
int RtvCensusBurstCaptureShot() { return g_burstCaptureShot.load(std::memory_order_acquire); }

// GATED ON THE INTERVAL THAT IS ABOUT TO BE PHOTOGRAPHED ACTUALLY BEING A
// DOUBLED ONE. Without this the pair is simply whatever frame Present reached
// first, and a capture of an ordinary frame is indistinguishable from a capture
// of a doubled frame in which nothing was substituted.
bool RtvCensusBurstCaptureWanted() {
    return g_burstCaptureWanted.load(std::memory_order_acquire) &&
           g_capturedIntervalOpens.load(std::memory_order_acquire) >= 2;
}
int RtvCensusCapturedIntervalSubstitutions() {
    return g_capturedIntervalSubstitutions.load(std::memory_order_acquire);
}
void RtvCensusClearBurstCapture() { g_burstCaptureWanted.store(false, std::memory_order_release); }
ID3D11Texture2D* RtvCensusPrivateTexture() { return g_privateTex; }

// THE HDR CLONE, and it gets its OWN retry rather than riding the LDR one.
// The tick returns early once the LDR target exists, so a failure here -- or
// simply the scene target not being identified yet on the tick that created
// the LDR one -- would leave the read substitution permanently pointing at the
// wrong format with nothing saying so.
bool g_privateHdrTried = false;
void EnsureHdrPrivateTarget() {
    if (g_privateHdrRtv || g_privateHdrTried || !g_censusContext) return;
    const int sceneId = g_sceneTargetPublished.load(std::memory_order_acquire);
    // A SILENT DECLINE IS WHY THE LAST RUN PRODUCED NOTHING. This returned
    // without a word when the scene target had not been identified, so the HDR
    // clone was simply absent, the read substitution fell back to the LDR view,
    // and the eye came back dark again with no line anywhere saying why. Every
    // early return this project has ever left unnamed has cost a run; this one
    // cost one too.
    if (sceneId < 0 || sceneId >= kMaxTargets || !g_targets[sceneId].width) {
        static std::uint64_t lastMoan = 0;
        const std::uint64_t now = GetTickCount64();
        if (now - lastMoan > 5000) {
            lastMoan = now;
            char line[420]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] M5 HDR private target NOT YET CREATED: batch 1's scene target is still "
                "unidentified (published id %d, targets known %d). The latch wants batch 1's first "
                "binding of TWO colour targets at once; until it fires, the read substitution "
                "falls back to the LDR view and the eye stays dark.\n",
                sceneId, g_targetCount);
            Tf2VrLog(line);
        }
        return;
    }
    g_privateHdrTried = true;
    // NARRATED FROM HERE, because the last run proved both guards above work
    // and the creation between them still produced no line at all. Every step
    // now says it happened, so "which line did it not get past" is answerable
    // from the log instead of by reading the function again.
    {
        char line[240]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] M5 HDR: attempting creation, scene target t%02d, %ux%u fmt %u.\n",
            sceneId, g_targets[sceneId].width, g_targets[sceneId].height,
            g_targets[sceneId].format);
        Tf2VrLog(line);
    }
    ID3D11Device* device = nullptr;
    g_censusContext->GetDevice(&device);
    if (!device) {
        Tf2VrLog("[TF2VR] M5 HDR: the context would not hand over its device.\n");
        return;
    }
    // THE HDR CLONE, from BATCH 1'S SCENE TARGET rather than the composite.
    //
    // This is the half the first build got wrong. Batch 2's post chain samples
    // the scene target and does linear-HDR maths on it; handing it an 8-bit
    // sRGB copy produced an eye that needed 7x exposure before its geometry was
    // visible. Cloning the format the shader actually expects is the fix, and
    // the desc comes from the target the census identified by signature, not
    // from a format written down here.
    {
        const int sceneId = g_sceneTargetPublished.load(std::memory_order_acquire);
        if (sceneId >= 0 && sceneId < kMaxTargets && g_targets[sceneId].width) {
            D3D11_TEXTURE2D_DESC hdr{};
            hdr.Width = g_targets[sceneId].width;
            hdr.Height = g_targets[sceneId].height;
            hdr.MipLevels = 1;
            hdr.ArraySize = 1;
            hdr.Format = static_cast<DXGI_FORMAT>(g_targets[sceneId].format);
            hdr.SampleDesc.Count = 1;
            hdr.Usage = D3D11_USAGE_DEFAULT;
            hdr.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            HRESULT hhr = device->CreateTexture2D(&hdr, nullptr, &g_privateHdrTex);
            if (SUCCEEDED(hhr) && g_privateHdrTex) {
                // TYPELESS has no default view, so both views are given the
                // FLOAT interpretation of the same bits -- which is what an HDR
                // scene target holds and what the post chain samples it as.
                DXGI_FORMAT viewFormat = hdr.Format;
                if (viewFormat == DXGI_FORMAT_R16G16B16A16_TYPELESS)
                    viewFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
                D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
                rtvDesc.Format = viewFormat;
                rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                srvDesc.Format = viewFormat;
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MipLevels = 1;
                hhr = device->CreateRenderTargetView(g_privateHdrTex, &rtvDesc, &g_privateHdrRtv);
                if (SUCCEEDED(hhr)) {
                    hhr = device->CreateShaderResourceView(g_privateHdrTex, &srvDesc, &g_privateHdrSrv);
                }
            }
            // AND THE PRIVATE DEPTH, cloned from the depth the scene pass used.
            const int depthId = g_sceneDepthPublished.load(std::memory_order_acquire);
            if (depthId >= 0 && depthId < kMaxTargets && g_targets[depthId].width) {
                D3D11_TEXTURE2D_DESC dd{};
                dd.Width = g_targets[depthId].width;
                dd.Height = g_targets[depthId].height;
                dd.MipLevels = 1;
                dd.ArraySize = 1;
                dd.Format = static_cast<DXGI_FORMAT>(g_targets[depthId].format);
                dd.SampleDesc.Count = 1;
                dd.Usage = D3D11_USAGE_DEFAULT;
                dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
                HRESULT dhr = device->CreateTexture2D(&dd, nullptr, &g_privateDepthTex);
                if (SUCCEEDED(dhr) && g_privateDepthTex) {
                    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
                    // TYPELESS again: R24G8 has no default view, so the DSV is
                    // given the depth-stencil interpretation of the same bits.
                    dsvDesc.Format = (dd.Format == DXGI_FORMAT_R24G8_TYPELESS)
                                         ? DXGI_FORMAT_D24_UNORM_S8_UINT
                                         : dd.Format;
                    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                    dhr = device->CreateDepthStencilView(g_privateDepthTex, &dsvDesc,
                                                         &g_privateDepthDsv);
                }
                char dline[400]{};
                std::snprintf(dline, sizeof(dline),
                    "[TF2VR] M5 private DEPTH %s: %ux%u fmt %u, cloned from t%02d -- the depth the "
                    "scene pass binds. Sharing batch 1's populated depth z-failed batch 2's world "
                    "at equal depth and left the eye with no static geometry in it.\n",
                    (SUCCEEDED(dhr) && g_privateDepthDsv) ? "created" : "FAILED",
                    dd.Width, dd.Height, static_cast<unsigned>(dd.Format), depthId);
                Tf2VrLog(dline);
            }
            char hline[420]{};
            std::snprintf(hline, sizeof(hline),
                "[TF2VR] M5 HDR private target %s: %ux%u fmt %u, cloned from the scene target the "
                "census named t%02d by signature. Batch 2's scene writes here and batch 2's post "
                "samples here, so the post chain gets the format it expects.\n",
                (SUCCEEDED(hhr) && g_privateHdrRtv && g_privateHdrSrv) ? "created" : "FAILED",
                hdr.Width, hdr.Height, static_cast<unsigned>(hdr.Format), sceneId);
            Tf2VrLog(hline);
        }
    }
    // M5's OWN CAMERA CONSTANT BUFFER, so batch 2 can be given a binding rather
    // than have the engine's contents rewritten under a binding it never made.
    if (!g_privateCameraCb) {
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = WorldCameraBufferBytes();
        cb.Usage = D3D11_USAGE_DEFAULT;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const HRESULT chr = device->CreateBuffer(&cb, nullptr, &g_privateCameraCb);
        char cline[300]{};
        std::snprintf(cline, sizeof(cline),
            "[TF2VR] M5 private camera constant buffer %s: %u bytes, to be bound at VS/PS slot %u "
            "-- the slot the binding census named.\n",
            SUCCEEDED(chr) ? "created" : "FAILED", cb.ByteWidth, g_cameraCbSlot);
        Tf2VrLog(cline);
    }
    device->Release();
}

void TickRtvCensus() {
    if (!g_active.load(std::memory_order_acquire)) return;
    EnsureHdrPrivateTarget();
    if (!g_substituteWanted.load(std::memory_order_acquire)) return;
    // THE RETARGET. A clone built from the wrong descriptor is worse than no
    // clone: the guard refuses every substitution, all the decline counters
    // that get read first stay at zero, and the only line that says so is
    // victim-mismatch at the very end of the M2 status. That is how a whole
    // headset session ran with SUBSTITUTIONS=0 and looked, from the front of
    // the status line, exactly like a healthy run.
    //
    // If the victim we have actually SEEN is not the target the clone was built
    // from, tear the clone down and let the creation path below rebuild it from
    // the right one on the next tick. One burst is lost to this and it says so.
    {
        const int observed = g_observedVictimId.load(std::memory_order_acquire);
        if (observed >= 0 && g_privateFromTargetId >= 0 && observed != g_privateFromTargetId) {
            char line[520]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] M2 RETARGET: the private target was cloned from t%02d, but the victim "
                "batch 1 actually leaves bound is t%02d. Every substitution has been refused as a "
                "victim mismatch. Releasing the clone and rebuilding it from t%02d; the next "
                "doubled frame after that substitutes.\n",
                g_privateFromTargetId, observed, observed);
            Tf2VrLog(line);
            if (g_privateSrv) { g_privateSrv->Release(); g_privateSrv = nullptr; }
            if (g_privateRtv) { g_privateRtv->Release(); g_privateRtv = nullptr; }
            if (g_privateTex) { g_privateTex->Release(); g_privateTex = nullptr; }
            g_privateFromTargetId = -1;
            g_privateTried = false;
            g_compositeTargetId.store(observed, std::memory_order_release);
        }
    }
    if (g_privateRtv || g_privateTried) {
        // Heartbeat, every 5 s, so a run that substituted NOTHING says so while
        // it is still running rather than at the end.
        static std::uint64_t lastTick = 0;
        const std::uint64_t now = GetTickCount64();
        if (now - lastTick < 5000) return;
        lastTick = now;
        char line[640]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] M2 status: armed=%d poisoned=%d private=%d | batch2 opens=%llu | "
            "SUBSTITUTIONS=%llu | clears=%llu | batch-2 spans on OTHER targets (left alone)=%llu "
            "| declines: no-private=%llu disarmed=%llu poisoned=%llu stale-double=%llu "
            "| CAMERA BINDS=%llu "
            "| PRE-DRAW camera writes=%llu declines=%llu "
            "| LEAK CHECK -- write substitutions by batch: b1=%llu b2=%llu; read substitutions "
            "by batch: b1=%llu b2=%llu. ANY NONZERO b1 IS A LEAK OUTSIDE BATCH 2, and the last "
            "run put both of its anomalies on batch 1 -- the side nothing here is supposed to "
            "touch. Routing: HDR=%llu LDR=%llu (additive effect stages must land HDR; sending "
            "them to the LDR clone is what ate the flame). victim-mismatch=%llu\n",
            RtvSubstituteArmed() ? 1 : 0, g_substitutePoisoned.load(std::memory_order_relaxed) ? 1 : 0,
            g_privateRtv ? 1 : 0, g_batch2Opens.load(std::memory_order_relaxed),
            g_substitutions.load(std::memory_order_relaxed), g_clears.load(std::memory_order_relaxed),
            g_batch2SpansNotVictim.load(std::memory_order_relaxed),
            g_declineNoPrivate.load(std::memory_order_relaxed),
            g_declineDisarmed.load(std::memory_order_relaxed),
            g_declinePoisoned.load(std::memory_order_relaxed),
            g_declineStaleDouble.load(std::memory_order_relaxed),
            g_cameraBinds.load(std::memory_order_relaxed),
            g_preDrawWrites.load(std::memory_order_relaxed),
            g_preDrawDeclines.load(std::memory_order_relaxed),
            g_writeSubsByBatch[1].load(std::memory_order_relaxed),
            g_writeSubsByBatch[2].load(std::memory_order_relaxed),
            g_readSubsByBatch[1].load(std::memory_order_relaxed),
            g_readSubsByBatch[2].load(std::memory_order_relaxed),
            g_hdrRoutes.load(std::memory_order_relaxed),
            g_ldrRoutes.load(std::memory_order_relaxed),
            g_declineVictimMismatch.load(std::memory_order_relaxed));
        Tf2VrLog(line);
        return;
    }
    const int composite = g_compositeTargetId.load(std::memory_order_acquire);
    if (composite < 0 || composite >= kMaxTargets) return;
    if (!g_targets[composite].width || !g_targets[composite].height) return;
    if (!g_censusContext) return;
    g_privateTried = true;   // one attempt, loudly, never a retry loop.

    ID3D11Device* device = nullptr;
    g_censusContext->GetDevice(&device);
    if (!device) {
        Tf2VrLog("[TF2VR] M2: the context would not hand over its device; no private target.\n");
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = g_targets[composite].width;
    desc.Height = g_targets[composite].height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = static_cast<DXGI_FORMAT>(g_targets[composite].format);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &g_privateTex);
    if (SUCCEEDED(hr) && g_privateTex) {
        hr = device->CreateRenderTargetView(g_privateTex, nullptr, &g_privateRtv);
    }
    // AND AN SRV OVER IT, before the device goes -- so batch 2's post chain can
    // be pointed at batch 2's own world instead of batch 1's. The texture is
    // created with BIND_SHADER_RESOURCE for exactly this.
    if (SUCCEEDED(hr) && g_privateTex &&
        FAILED(device->CreateShaderResourceView(g_privateTex, nullptr, &g_privateSrv))) {
        g_privateSrv = nullptr;
        Tf2VrLog("[TF2VR] M5: the private target has no SRV, so the read-side substitution "
                 "CANNOT arm. It will decline, counted.\n");
    }
    device->Release();
    if (FAILED(hr) || !g_privateRtv) {
        char line[320]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] M2: could not create the private %ux%u fmt %u target: 0x%08lX. The "
            "substitution CANNOT arm this session and will decline, counted, rather than "
            "silently do nothing.\n",
            desc.Width, desc.Height, static_cast<unsigned>(desc.Format),
            static_cast<unsigned long>(hr));
        Tf2VrLog(line);
        if (g_privateTex) { g_privateTex->Release(); g_privateTex = nullptr; }
        return;
    }
    g_privateFromTargetId = composite;
    char line[560]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] M2: private per-eye target created, %ux%u fmt %u (%s), cloned from the "
        "composite the census names as t%02d -- the target every ordinary interval ends bound "
        "to, and the one batch 2 renders its whole frame into. Batch 2's rtv0 is redirected "
        "here; its rtv1 and depth are left shared on purpose.\n",
        desc.Width, desc.Height, static_cast<unsigned>(desc.Format),
        FormatName(static_cast<unsigned>(desc.Format)), composite);
    Tf2VrLog(line);
}

// ---------------------------------------------------------------------------
// M3. THE DISCRIMINATOR, EXPORTED.
//
// This is the one predicate the whole replan turned on: a thing the RENDER
// THREAD can evaluate that says "the D3D call I am looking at belongs to batch
// 2". M1 found it and M2 has now leaned on it 509 times without a
// misidentification, so the camera key does not need the pre-call source write
// the design preferred -- the replan's own rule was that B1 goes first ONLY if
// no structural discriminator exists, and one does.
bool RtvCensusInBatch2() { return g_batch.load(std::memory_order_relaxed) == 2; }

// M3 diagnosis: where in the interval's transition sequence are we right now,
// and which batch. The camera census records this against every world-family
// upload, so "batch 2 uploads its world camera" can be told apart from "batch 2
// uploads its world camera BEFORE its own scene pass draws" -- which would make
// the patch land on a buffer the draws had already read past.
int RtvCensusCurrentSpanIndex() { return g_intervals[g_cur].count; }

// N1-Q1, THE ORDERING CLOCK -- AND IT IS NOT THE ONE THE PLAN ASKED FOR.
//
// The plan wants the DrawIndexed tally at each camera upload, because span
// granularity cannot order an upload against the draws INSIDE one span. It
// cannot be built: CLAUDE.md records that per-object context-table slot swaps
// are proven EXCEPT slots 12 and 13, Draw and DrawIndexed, which fault the
// driver. So the finest clock this project can legally read is the
// VSSetConstantBuffers ordinal within the current batch -- 1782 of them on
// batch 1 against 147 draws in the scene span, so it is FINER than per-draw,
// not coarser, and it ticks throughout the span rather than only at its edges.
// It answers the same question; it is a different instrument and is labelled as
// one wherever it is printed.
unsigned long long RtvCensusCbBindsInBatch() {
    const int batch = g_batch.load(std::memory_order_relaxed);
    if (batch < 1 || batch > 2) return 0;
    return g_cbBinds[batch];
}

// ---------------------------------------------------------------------------
// M5's read-side substitution.

void SetRtvReadSubstituteWanted(int mode) {
    g_readSubstituteWanted.store(mode, std::memory_order_release);
    g_readSubstituteArmed.store(mode != 0, std::memory_order_release);
    char line[620]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] stereo.substitute_reads = %d. %s\n", mode,
        mode ? "While in batch 2, an SRV resolving to the target BATCH 1's scene wrote is handed "
               "our private copy instead, so batch 2's post chain reads batch 2's own world. The "
               "census measured the leak first: 534 SRV matches a frame on EACH batch, with the "
               "HDR scene target in both sampled sets -- batch 2 reading the scene batch 1 wrote, "
               "because batch 2's own scene went to our private target. That is the faint "
               "unshifted crate. FALSIFIER: the ghost leaves the private eye."
             : "OFF -- batch 2's post keeps reading batch 1's scene, which is the ghost.");
    Tf2VrLog(line);
}

bool RtvReadSubstituteArmed() {
    return g_readSubstituteWanted.load(std::memory_order_acquire) != 0 &&
           g_readSubstituteArmed.load(std::memory_order_acquire);
}

void ToggleRtvReadSubstitute() {
    if (!g_readSubstituteWanted.load(std::memory_order_acquire)) {
        Tf2VrLog("[TF2VR] M5: F3 pressed, but stereo.substitute_reads is 0. Nothing to toggle.\n");
        return;
    }
    const bool on = !g_readSubstituteArmed.load(std::memory_order_acquire);
    g_readSubstituteArmed.store(on, std::memory_order_release);
    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] M5 read-side substitution %s (F3). %llu substitutions so far. %s\n",
        on ? "ARMED" : "DISARMED", g_readSubstitutions.load(std::memory_order_relaxed),
        on ? "Batch 2's post reads batch 2's world."
           : "THE POSITIVE CONTROL: the ghost of batch 1 must come back into the private eye.");
    Tf2VrLog(line);
}

// ---------------------------------------------------------------------------
// M5's pre-draw camera write.

void SetPreDrawCameraBind(int mode) {
    g_bindArmed.store(mode != 0, std::memory_order_release);
    char line[640]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] stereo.predraw_bind = %d. %s\n", mode,
        mode ? "Batch 2 is given its OWN camera constant buffer, BOUND into VS and PS slot 2 at "
               "the transition that opens its scene span. The binding census licenses it: batch 2 "
               "issues ZERO constant-buffer binds against batch 1's 1782, so batch 2 inherits "
               "batch 1's bindings -- and rewriting a bound buffer's CONTENTS cannot reach those "
               "draws, because the runtime may rename the resource and leave the binding on the "
               "old data. Every camera attempt since M3 opened wrote contents; this changes the "
               "binding."
             : "OFF -- the camera is written into the engine's own buffer, which the last run "
               "showed batch 2's draws do not read.");
    Tf2VrLog(line);
}

void SetPreDrawCameraIpd(float ipd) {
    g_preDrawIpd.store(ipd, std::memory_order_release);
    g_preDrawArmed.store(ipd != 0.0f, std::memory_order_release);
    char line[700]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] stereo.predraw_camera = %.3f. %s\n", ipd,
        ipd != 0.0f
            ? "The camera is written into the engine's own constant buffer at the transition that "
              "OPENS batch 2's scene span, before any of its draws. Every earlier attempt patched "
              "an upload TAGGED batch 2 and assumed batch 2's draws would read it -- the span "
              "census says they cannot, because both batches upload their world camera AFTER "
              "their own heavy scene span, so a draw reads a camera from earlier in the pipeline "
              "and the tag is off by a stage. That is why the bytes diverged in every run since "
              "M3 opened and the pictures never did. FALSIFIER: measured horizontal disparity "
              "between the eyes that falls with distance, with dy = 0."
            : "OFF.");
    Tf2VrLog(line);
}

unsigned long long RtvPreDrawWrites() { return g_preDrawWrites.load(std::memory_order_relaxed); }
unsigned long long RtvPreDrawDeclines() { return g_preDrawDeclines.load(std::memory_order_relaxed); }
