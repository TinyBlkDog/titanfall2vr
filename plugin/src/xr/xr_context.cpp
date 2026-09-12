#include "xr_context.h"

#include "xr_breadcrumb.h"
#include "menu_overlay.h"
#include "aim_census.h"
#include "asym_projection.h"

#include "camera_hook.h"
#include "camera_update_hook.h"
#include "render_resolution.h"
#include "flat_harness.h"
#include "diagnostics.h"
#include "xr_input.h"
#include "engine_cvars.h"
#include "resource_watch.h"
#include "present_hook.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// Published headset orientation in degrees, consumed by the camera detour.
// Defined in camera_hook.cpp so the write side owns the storage.
extern "C" volatile float g_headYawDegrees;
extern "C" volatile float g_headPitchDegrees;
extern "C" volatile float g_headRollDegrees;
extern "C" volatile std::uint8_t g_headPoseValid;
extern "C" volatile std::uint64_t g_headPoseSequence;
// Nonzero only while head tracking is actually driving the engine's view.
// The projection layer depends on this: it declares that the submitted image
// was rendered looking along the head's direction, which is a lie unless the
// game's camera is following the head. Submitting a projection layer without
// it head-locks the world to your face, which is the specific failure the
// task brief calls actively nauseating.
extern "C" volatile std::uint8_t g_headCompensationValid;
// Head position in metres, OpenXR axes, for positional head tracking.
extern "C" volatile float g_headPositionMetres[3];
// The head's orientation as a BASIS in Source axes -- rows forward, right, up.
// Published as a matrix rather than as Euler angles on purpose: every Euler
// round trip in this chain has been a source of error, and the consumer needs
// an orientation to compose, not three numbers to add.
extern "C" volatile float g_headBasis[9];
extern "C" volatile std::uint32_t g_headBasisGeneration;

namespace {

// xr.headlock_quad. TRIED AND REJECTED. Default OFF and it should stay off.
//
// The wearer's verdict, 2026-08-22, unambiguous: "both main menu and loading
// are now pinned to my head -- WHICH I HATE BY THE WAY. I do NOT want this to
// stay pinned to my head. I want it on a screen in front of me I can move my
// head to see fully."
//
// It also did not work: the loading screen judders the same head-locked as
// world-locked, which is itself a finding -- a view-space quad cannot move
// relative to the head by construction, so whatever the judder is, it is NOT
// the layer's anchoring. Kept only as a documented dead end so nobody reaches
// for it again.
bool g_headlockQuad = false;

// menu.distance_m / menu.width_m -- where the config panel's quad sits and how
// big it is, in metres. 2.4 m wide at 1.5 m away: the first VR placement used
// 1.2 and the wearer asked for about double in both axes. Height follows the
// 4:3 panel texture, so widening does both.
float g_menuDistanceM = 1.5f;
float g_menuWidthM = 1.7f;
bool XrOk(XrResult result, const char* action) {
    if (XR_SUCCEEDED(result)) return true;
    char message[160];
    std::snprintf(message, sizeof(message), "[TF2VR] OpenXR %s failed: %d\n", action, result);
    Tf2VrLog(message);
    return false;
}
// Named, not numbered. "format 29" needs a lookup table to read and "TYPELESS"
// versus "_SRGB" is the entire difference between a correct image and one that
// is gamma-encoded twice.
const char* DxgiFormatName(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS(linear-typed!)";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM(linear-typed!)";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS(linear-typed!)";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM(linear-typed!)";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT(linear)";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM(linear-typed!)";
        default: return "other";
    }
}

const char* SessionStateName(XrSessionState state) {
    switch (state) {
        case XR_SESSION_STATE_IDLE: return "IDLE";
        case XR_SESSION_STATE_READY: return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED (not displayed)";
        case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
        case XR_SESSION_STATE_STOPPING: return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING: return "EXITING";
        default: return "UNKNOWN";
    }
}
float WrapRadians(float value) {
    constexpr float pi = 3.14159265358979323846f;
    while (value > pi) value -= 2.0f * pi;
    while (value < -pi) value += 2.0f * pi;
    return value;
}
}

XrContext::~XrContext() { Shutdown(); }
void XrContext::Log(const char* message) const { Tf2VrLog(message); }
void XrContext::SetEnabled(bool enabled) { enabled_ = enabled; haveLookBaseline_ = false; }
void XrContext::SetHeadLookEnabled(bool enabled) { headLookEnabled_ = enabled; haveLookBaseline_ = false; }

bool XrContext::Initialize(IDXGISwapChain* gameSwapchain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(gameSwapchain->GetDesc(&desc)) ||
        FAILED(gameSwapchain->GetDevice(IID_PPV_ARGS(&gameDevice_)))) return false;
    // Provisional: replaced by our own device below if decoupling is wanted and
    // one can be made on the adapter the runtime demands.
    device_ = gameDevice_;
    // Before anything else touches the device: the load-crash instrument has to
    // be watching from the first creation, because the one that matters is the
    // one that happens before the crash and there is no second chance at it.
    InstallResourceWatch(gameDevice_.Get());
    // The camera hook needs the target's real size to know when the game has
    // clamped the world pass to something shorter than it.
    PublishBackbufferSize(static_cast<float>(desc.BufferDesc.Width),
                          static_cast<float>(desc.BufferDesc.Height));
    gameDesc_ = desc;

    const char* extensions[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(instanceInfo.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "Titanfall2VR");
    // Request the baseline OpenXR 1.0 API.  The loader SDK may be newer than
    // the active runtime (Virtual Desktop currently advertises a 1.0 runtime),
    // and requesting the SDK's current 1.1.x version makes xrCreateInstance
    // fail before the D3D11 extension can be negotiated.
    instanceInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    instanceInfo.enabledExtensionCount = 1;
    instanceInfo.enabledExtensionNames = extensions;
    if (!XrOk(xrCreateInstance(&instanceInfo, &instance_), "create instance")) return false;
    if (!XrOk(xrGetInstanceProcAddr(instance_, "xrGetD3D11GraphicsRequirementsKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&getD3D11Requirements_)), "load D3D11 requirements")) return false;

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    // XR_ERROR_FORM_FACTOR_UNAVAILABLE means the headset is not connected or is
    // asleep. It is called out by name because it presents EXACTLY like a
    // successful run -- the game plays normally and at full speed, since no
    // session exists to submit to -- and one whole headset run was spent
    // interpreting "fast throughout" as a result about layer submission when in
    // fact OpenXR had never started.
    const XrResult systemResult = xrGetSystem(instance_, &systemInfo, &systemId_);
    if (systemResult == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        Log("[TF2VR] OpenXR: NO HEADSET. The runtime reports no head-mounted display connected or "
            "awake, so no session was created and nothing was submitted. The game will run at full "
            "speed with VR doing nothing -- which looks identical to a good run. Put the headset on, "
            "wait for the runtime to see it, then arm OpenXR again.\n");
        return false;
    }
    if (!XrOk(systemResult, "get HMD system")) return false;
    // WHICH RUNTIME, and what it calls the headset. The stall is now known to be
    // the cost of compositing what we submit, and the two live suspects -- an
    // enormous per-eye resolution, and compositing on the game's own device --
    // are both properties of the runtime rather than of this code. Guessing at
    // which runtime is in use, and at whether it changed between a fast day and
    // a slow one, is not something to do from memory.
    {
        XrInstanceProperties instanceProperties{XR_TYPE_INSTANCE_PROPERTIES};
        XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
        const bool haveInstance = XR_SUCCEEDED(xrGetInstanceProperties(instance_, &instanceProperties));
        const bool haveSystem = XR_SUCCEEDED(xrGetSystemProperties(instance_, systemId_, &systemProperties));
        char line[420]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] OpenXR runtime '%s' version %llu.%llu.%llu; system '%s' vendorId 0x%X "
            "systemId %llu. THE NAME IS THE RUNTIME'S CLAIM, NOT A DETECTION. VDXR reports "
            "'Meta Quest 3' for a Play For Dream MR, which Virtual Desktop's own UI identifies "
            "correctly, so the OpenXR systemName does not track the device VD knows it is talking "
            "to. vendorId is logged in case it distinguishes where the name does not.\n",
            haveInstance ? instanceProperties.runtimeName : "<unknown>",
            haveInstance ? static_cast<unsigned long long>(XR_VERSION_MAJOR(instanceProperties.runtimeVersion)) : 0ull,
            haveInstance ? static_cast<unsigned long long>(XR_VERSION_MINOR(instanceProperties.runtimeVersion)) : 0ull,
            haveInstance ? static_cast<unsigned long long>(XR_VERSION_PATCH(instanceProperties.runtimeVersion)) : 0ull,
            haveSystem ? systemProperties.systemName : "<unknown>",
            haveSystem ? systemProperties.vendorId : 0u,
            haveSystem ? static_cast<unsigned long long>(systemProperties.systemId) : 0ull);
        Log(line);
        // Kept for the G4 session banner, which has to name the runtime it was
        // measured on -- a geometry report without it cannot be compared.
        if (haveInstance) {
            std::strncpy(runtimeName_, instanceProperties.runtimeName, sizeof(runtimeName_) - 1);
            runtimeName_[sizeof(runtimeName_) - 1] = '\0';
        }
        if (haveSystem) {
            std::strncpy(systemName_, systemProperties.systemName, sizeof(systemName_) - 1);
            systemName_[sizeof(systemName_) - 1] = 0;
            SetHeadsetIdentity(runtimeName_, systemName_);
        }
    }
    LogViewConfiguration();
    // Task 05 step 1. Instance-scoped, so it goes here: the actions and their
    // suggested bindings must exist before any session attaches them, and a
    // failure is non-fatal by design -- the verified head-tracking, stereo and
    // projection path must not be takeable down by a controller.
    XrInputCreateActions(instance_);
    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!XrOk(getD3D11Requirements_(instance_, systemId_, &requirements), "get D3D11 requirements")) return false;
    // OUR OWN DEVICE, if decoupling is wanted. Everything below -- the adapter
    // check, the graphics binding, the swapchains -- then refers to it, and the
    // game's device is used only by the game's own thread.
    //
    // If this fails we keep the game's device and carry on: the coupled path is
    // still correct, it just cannot be decoupled. A headset that works without
    // smoothing beats one that does not work at all.
    // THE SESSION STAYS ON THE GAME'S DEVICE, and CreateXrDevice is no longer
    // called. Kept, because it is correct and the reasoning in it is worth
    // having if a future design needs its own device again.
    //
    // Why it is not used: with the session on our device, only the pacing thread
    // can submit, so GAMEPLAY had to go through it too -- and then the game
    // thread has to be paced by a clock we construct, which is a SECOND clock.
    // Measured, the best that got to was 12.75 ms +/- 0.52 against a 12.50 ms
    // display period, so it slips a whole frame every fifty or so, forever.
    // Coupled measures 12.50 +/- 0.19 because xrWaitFrame IS the display clock.
    // No amount of timer precision closes that: the game thread can only be on
    // the display clock by calling xrWaitFrame itself.
    //
    // So Present keeps the frame loop for gameplay, exactly as it always had,
    // and the pacing thread fills ONLY when the game has stalled -- where there
    // is no pacing to preserve because the game is producing nothing. That is
    // the loading screen, which is the result this whole feature was for.
    (void)requirements;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDesc{};
    if (FAILED(device_.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&adapterDesc))) {
        Log("[TF2VR] Could not identify the game's D3D11 adapter.\n");
        return false;
    }
    if (std::memcmp(&adapterDesc.AdapterLuid, &requirements.adapterLuid, sizeof(LUID)) != 0 ||
        device_->GetFeatureLevel() < requirements.minFeatureLevel) {
        Log("[TF2VR] Game D3D11 device does not meet the OpenXR runtime adapter/feature requirement.\n");
        return false;
    }

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device_.Get();
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = systemId_;
    if (!XrOk(xrCreateSession(instance_, &sessionInfo, &session_), "create D3D11 session")) return false;

    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    if (!XrOk(xrCreateReferenceSpace(session_, &spaceInfo, &localSpace_), "create local space")) return false;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!XrOk(xrCreateReferenceSpace(session_, &spaceInfo, &viewSpace_), "create view space")) return false;

    // THE FLOOR. This is the space every released VR mod uses to answer "how
    // tall is the person wearing this", and until now we never asked for it.
    //
    // LOCAL is anchored at wherever the head happens to be when the session
    // starts, so a head position in it is a delta from an arbitrary moment and
    // says NOTHING about height above the ground. That is why seated and
    // standing measured the same here, why both headsets reported head y ~ 0,
    // and why no amount of work on the frustum could ever have fixed the
    // wearer's height: the quantity was never in the program.
    //
    // STAGE is floor-referenced -- y = 0 is the ground the wearer is standing
    // on -- so locating the eyes in it gives their real eye height directly.
    // `C:\dev\othermods\CallOfDuty4_VR` does exactly this and calls the result
    // the calibration floor; Halo and Cyberpunk carry the same quantity under
    // different names.
    //
    // ENUMERATED BEFORE IT IS CREATED, as CoD4 does: STAGE is optional in the
    // spec and a runtime with no room setup may not offer it. Creating it blind
    // would fail the whole session-init path over an optional feature, so a
    // missing floor is recorded and everything else carries on.
    std::uint32_t spaceCount = 0;
    if (XR_SUCCEEDED(xrEnumerateReferenceSpaces(session_, 0, &spaceCount, nullptr)) && spaceCount) {
        std::vector<XrReferenceSpaceType> spaces(spaceCount);
        if (XR_SUCCEEDED(xrEnumerateReferenceSpaces(session_, spaceCount, &spaceCount, spaces.data()))) {
            for (std::uint32_t i = 0; i < spaceCount; ++i) {
                if (spaces[i] != XR_REFERENCE_SPACE_TYPE_STAGE) continue;
                XrReferenceSpaceCreateInfo floorInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
                floorInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
                floorInfo.poseInReferenceSpace.orientation.w = 1.0f;
                if (XR_SUCCEEDED(xrCreateReferenceSpace(session_, &floorInfo, &floorSpace_))) break;
                floorSpace_ = XR_NULL_HANDLE;
            }
        }
    }
    {
        char line[300]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] floor reference: STAGE %s. This is what measures the wearer's real eye height "
            "above the ground; without it their height has to be assumed rather than known.\n",
            floorSpace_ != XR_NULL_HANDLE ? "AVAILABLE and created"
                                          : "UNAVAILABLE -- no room setup this runtime will report");
        Log(line);
    }
    // Session-scoped, and a session's action sets can only ever be attached
    // once, which is why this is a second call rather than part of the first.
    // Also non-fatal.
    XrInputAttachToSession(session_);
    if (!CreateSwapchain(desc)) return false;
    initialized_ = true;
    Log("[TF2VR] OpenXR D3D11 session initialized; waiting for runtime READY.\n");
    return true;
}

bool XrContext::CreateSwapchain(const DXGI_SWAP_CHAIN_DESC& gameDesc) {
    std::uint32_t formatCount = 0;
    if (!XrOk(xrEnumerateSwapchainFormats(session_, 0, &formatCount, nullptr), "enumerate formats")) return false;
    std::vector<std::int64_t> formats(formatCount);
    if (!XrOk(xrEnumerateSwapchainFormats(session_, formatCount, &formatCount, formats.data()), "read formats")) return false;
    const auto wanted = static_cast<std::int64_t>(gameDesc.BufferDesc.Format);
    // EVERY FORMAT IN PLAY, BY NAME.
    //
    // Exactly one extra gamma encode is the classic washed-out-in-the-headset
    // bug: mid-grey 0.5 arrives at ~0.73, blacks lift, mid-distance texture
    // detail disappears. It is a presentation bug that looks exactly like a
    // rendering bug, which is how it costs hours -- and it turns entirely on
    // which of these formats is sRGB-typed and which is not.
    //
    // The reasoning is short. The runtime always encodes for display, and
    // decodes first only if the swapchain is _SRGB. The game's backbuffer holds
    // bytes that are ALREADY display-encoded, since they are what the monitor
    // shows and the monitor is right. So the correct arrangement is an _SRGB
    // swapchain receiving those bytes RAW: decode and encode cancel. A linear
    // (UNORM) swapchain given the same bytes gets encoded a second time.
    //
    // CopyResource is the right delivery either way -- it converts nothing --
    // so the whole question is the swapchain's declared type, and until now
    // nothing recorded it. An unlogged format choice is an unverifiable
    // inference, and this file already notes that runtimes hand back TYPELESS
    // for a typed request, which is precisely the case where the declared type
    // and the observed type disagree.
    {
        char line[512]{};
        int used = std::snprintf(line, sizeof(line),
            "[TF2VR] colour formats: game backbuffer %s; requesting the same for the XR swapchain. "
            "Runtime offers:",
            DxgiFormatName(gameDesc.BufferDesc.Format));
        for (std::int64_t format : formats) {
            if (used >= static_cast<int>(sizeof(line)) - 24) break;
            used += std::snprintf(line + used, sizeof(line) - used, " %s",
                                  DxgiFormatName(static_cast<DXGI_FORMAT>(format)));
        }
        std::snprintf(line + used, sizeof(line) - used, "\n");
        Log(line);
    }
    if (std::find(formats.begin(), formats.end(), wanted) == formats.end()) {
        char line[300]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] OpenXR runtime does not support the game's backbuffer format %s. NOTE: if a "
            "non-sRGB format is substituted here, the image WILL be too bright -- the runtime would "
            "encode already-encoded bytes a second time.\n",
            DxgiFormatName(gameDesc.BufferDesc.Format));
        Log(line);
        return false;
    }
    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.format = wanted;
    createInfo.sampleCount = 1;
    createInfo.width = gameDesc.BufferDesc.Width;
    createInfo.height = gameDesc.BufferDesc.Height;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;
    XrBreadcrumb::NoteSwapchainChanged(true);
    if (!XrOk(xrCreateSwapchain(session_, &createInfo, &colorSwapchain_), "create flat-image swapchain")) return false;
    std::uint32_t imageCount = 0;
    xrEnumerateSwapchainImages(colorSwapchain_, 0, &imageCount, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> xrImages(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (!XrOk(xrEnumerateSwapchainImages(colorSwapchain_, imageCount, &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(xrImages.data())), "read D3D11 images")) return false;
    images_.reserve(imageCount);
    for (const auto& image : xrImages) images_.emplace_back(image.texture);
    // What the runtime actually handed back, which need not be what was asked
    // for. A TYPELESS image is expected and harmless -- CopyResource converts
    // nothing and the SWAPCHAIN's declared format is what the runtime uses to
    // decide whether to decode. A linear UNORM here would be the bug: the
    // already-encoded bytes would be encoded once more for display.
    if (!images_.empty() && images_[0]) {
        D3D11_TEXTURE2D_DESC desc{};
        images_[0]->GetDesc(&desc);
        char line[360]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] XR swapchain images are %s (%ux%u). Requested %s. TYPELESS here is normal; the "
            "declared swapchain format is what decides the runtime's decode, and the copy converts "
            "nothing either way.\n",
            DxgiFormatName(desc.Format), desc.Width, desc.Height,
            DxgiFormatName(static_cast<DXGI_FORMAT>(createInfo.format)));
        Log(line);
    }
    // The two per-eye swapchains are additional; the mono one above is left in
    // place so a stereo failure can fall back to the presentation that is
    // already verified working in the headset.
    if (!CreateEyeSwapchain(gameDesc, 0) || !CreateEyeSwapchain(gameDesc, 1)) {
        Log("[TF2VR] Per-eye swapchain creation failed; stereo unavailable, mono quad still active.\n");
        return true;
    }
    return true;
}

bool XrContext::CreateEyeSwapchain(const DXGI_SWAP_CHAIN_DESC& gameDesc, int eye) {
    if (eye < 0 || eye > 1) return false;
    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.format = static_cast<std::int64_t>(gameDesc.BufferDesc.Format);
    createInfo.sampleCount = 1;
    createInfo.width = gameDesc.BufferDesc.Width;
    createInfo.height = gameDesc.BufferDesc.Height;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;
    XrBreadcrumb::NoteSwapchainChanged(true);
    if (!XrOk(xrCreateSwapchain(session_, &createInfo, &eyeSwapchain_[eye]), "create per-eye swapchain")) return false;
    std::uint32_t imageCount = 0;
    xrEnumerateSwapchainImages(eyeSwapchain_[eye], 0, &imageCount, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> xrImages(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (!XrOk(xrEnumerateSwapchainImages(eyeSwapchain_[eye], imageCount, &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(xrImages.data())), "read per-eye D3D11 images")) return false;
    eyeImages_[eye].reserve(imageCount);
    for (const auto& image : xrImages) eyeImages_[eye].emplace_back(image.texture);
    {
        // HOW DEEP THE SWAPCHAIN IS, because the pacing thread submits frames
        // referencing images whose acquire/release cycle Present alone drives.
        // The runtime chooses this count -- OpenXR gives the app no say -- so
        // it has to be read back rather than assumed, and a shallow chain plus
        // a second submitter is a hazard worth being able to see.
        char line[260]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] eye %d swapchain: %u images of %ux%u. Present drives every acquire and release; "
            "the pacing thread submits extra frames against whatever was last released.\n",
            eye, imageCount, gameDesc.BufferDesc.Width, gameDesc.BufferDesc.Height);
        Log(line);
    }
    return true;
}

bool XrContext::HandleBackbufferResize(IDXGISwapChain* gameSwapchain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(gameSwapchain->GetDesc(&desc))) return true;
    if (desc.BufferDesc.Width == gameDesc_.BufferDesc.Width &&
        desc.BufferDesc.Height == gameDesc_.BufferDesc.Height &&
        desc.BufferDesc.Format == gameDesc_.BufferDesc.Format) {
        return true;
    }
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] game backbuffer changed from %ux%u (format %u) to %ux%u (format %u); rebuilding the XR swapchains.\n",
        gameDesc_.BufferDesc.Width, gameDesc_.BufferDesc.Height, static_cast<unsigned>(gameDesc_.BufferDesc.Format),
        desc.BufferDesc.Width, desc.BufferDesc.Height, static_cast<unsigned>(desc.BufferDesc.Format));
    Log(line);
    DestroySwapchain();
    gameDesc_ = desc;
    // The camera hook measures the world-rect gate against the target's real
    // size. It was published once at Initialize and never again, so after the
    // mode change it still said 2560x1440 while the buffer was 5210x3648.
    PublishBackbufferSize(static_cast<float>(desc.BufferDesc.Width),
                          static_cast<float>(desc.BufferDesc.Height));
    if (!CreateSwapchain(desc)) {
        // Say so loudly. A failure here presents exactly like a working plugin
        // with a frozen headset, which is the failure mode this whole function
        // exists to stop being invisible.
        Log("[TF2VR] XR swapchain rebuild FAILED at the new backbuffer size; the headset will not update. "
            "Disarm and re-arm OpenXR, or set the resolution back.\n");
        return false;
    }
    Log("[TF2VR] XR swapchains rebuilt; both eyes refill over the next two frames.\n");
    return true;
}

bool XrContext::PollEvents() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& state = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
            // Every transition, by name. VISIBLE/FOCUSED/SYNCHRONIZED were being
            // dropped on the floor, and the difference between them is exactly
            // the difference between a session the runtime is displaying and one
            // it is only keeping alive -- which is what a 10 Hz pace looks like.
            if (state.state != sessionState_) {
                const bool leftFocus = sessionState_ == XR_SESSION_STATE_FOCUSED &&
                                       state.state != XR_SESSION_STATE_FOCUSED;
                sessionState_ = static_cast<int>(state.state);
                char line[200]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] XR session state -> %s. FOCUSED/VISIBLE means the runtime is displaying "
                    "us; SYNCHRONIZED means it is not, and a runtime that is not displaying a session "
                    "is entitled to pace it as slowly as it likes.\n",
                    SessionStateName(state.state));
                Log(line);
                // LOSING FOCUS IS THE MOMENT TO ASK WHO TOOK IT.
                //
                // 2026-09-09: the player died, and 300 ms later xrSyncActions
                // returned XR_SESSION_NOT_FOCUSED and the session dropped to
                // VISIBLE. It never came back. shouldRender was false for every
                // one of the next 1500 frames, so this submitted ZERO layers for
                // twenty-six seconds and the wearer sat looking at the runtime's
                // own screen -- a black rectangle that follows the head, because
                // that is what a compositor shows when an app hands it nothing.
                //
                // Whether the cause is a system overlay or the GAME WINDOW LOSING
                // WINDOWS FOREGROUND is the whole question, and those want
                // opposite responses. Read-only: this asks, it does not act.
                if (leftFocus) {
                    const HWND fg = GetForegroundWindow();
                    DWORD fgPid = 0;
                    if (fg) GetWindowThreadProcessId(fg, &fgPid);
                    char title[96]{};
                    if (fg) GetWindowTextA(fg, title, sizeof(title) - 1);
                    char cls[64]{};
                    if (fg) GetClassNameA(fg, cls, sizeof(cls) - 1);
                    char who[420]{};
                    std::snprintf(who, sizeof(who),
                        "[TF2VR] LEFT XR FOCUS. Windows foreground window is %p pid %lu (ours is %lu) "
                        "class '%s' title '%s'. A DIFFERENT pid here means the game lost the desktop "
                        "focus and the runtime followed; OUR pid means the runtime took focus for its "
                        "own overlay and the window is fine.\n",
                        static_cast<void*>(fg), static_cast<unsigned long>(fgPid),
                        static_cast<unsigned long>(GetCurrentProcessId()), cls, title);
                    Log(who);
                }
            }
            if (state.state == XR_SESSION_STATE_READY && !sessionRunning_) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XrOk(xrBeginSession(session_, &begin), "begin session")) sessionRunning_ = true;
            } else if (state.state == XR_SESSION_STATE_STOPPING && sessionRunning_) {
                xrEndSession(session_); sessionRunning_ = false;
            } else if (state.state == XR_SESSION_STATE_EXITING || state.state == XR_SESSION_STATE_LOSS_PENDING) {
                enabled_ = false;
            }
        }
        // THE WEARER'S OWN RECENTRE, WHICH WAS BEING DROPPED ON THE FLOOR.
        //
        // Long-pressing the menu button makes the runtime move its reference
        // space. Our head-tracking reference is latched ONCE, the first frame
        // head tracking runs, so after a recentre it is stale by exactly how far
        // the space moved -- and the gesture that should fix the problem does
        // nothing at all, which is what was reported: the gun sat about 45
        // degrees right and recentring did not change it. One log line had
        // already said why, and nobody had connected it: the reference was
        // captured at yaw -39.58, pitch 17.00, i.e. while the headset was not
        // pointing anywhere near forward.
        if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            // Named, so the three recentre paths (LEADER+TAB, the automatic
            // one after spawn, and this one) are distinguishable in the log.
            Tf2VrLog("[TF2VR] RESET HEIGHT (runtime): the runtime moved its reference space -- the "
                     "wearer recentred from the headset itself -- so the head pose is re-sampled.\n");
            RequestHeadTrackingRecentre();
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return sessionRunning_;
}

void XrContext::UpdateLookInjection(XrTime predictedTime) {
    if (!headLookEnabled_) return;
    // The synthetic pose owns the head globals while it is on, so the harness
    // can be used with OpenXR ARMED. That combination is the bisection the flat
    // runs cannot make on their own: a perfect, known head pose driving the
    // camera while the real XR submission path -- xrWaitFrame pacing, the
    // swapchain copy, the compositor -- runs underneath it. Without this guard
    // the two would both publish every frame and the real pose would win.
    if (IsSyntheticPoseEnabled()) return;
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if (!XrOk(xrLocateSpace(viewSpace_, localSpace_, predictedTime, &location), "locate HMD")) return;
    if (!(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return;
    // Position is published separately from orientation and only when its own
    // flag is set: a runtime can report a valid orientation with no position,
    // and feeding an invalid position to the camera would jerk the view origin.
    if (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
        g_headPositionMetres[0] = location.pose.position.x;
        g_headPositionMetres[1] = location.pose.position.y;
        g_headPositionMetres[2] = location.pose.position.z;
    }
    const auto& q = location.pose.orientation;

    // Decompose into SOURCE angles, via Source's own MatrixAngles, rather than
    // reading Euler angles straight off the XR quaternion.
    //
    // The previous version extracted yaw/pitch/roll with a quaternion-to-Euler
    // formula in XR's axis convention and fed the result to the engine as if it
    // were Source's. Near the identity that is almost right, which is why it
    // passed a headset check; away from it the axis orders disagree and a pure
    // yaw motion bleeds into roll. Reported from the headset as "turning left
    // and right also rotates the angle of my head, it isn't a smooth yaw", and
    // as the reticle appearing to rotate on a tilted plane as the head turns --
    // the same roll, seen through a world-anchored projection layer.
    //
    // MatrixAngles is the exact inverse of the AngleVectors the engine composes
    // with, so a pure rotation about world up now produces a pure yaw delta and
    // nothing else. This is the "compose once, in one algebra" lesson from the
    // viewmodel work applied to the input side.
    const auto rotate = [&q](float x, float y, float z, float out[3]) {
        // v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
        const float tx = 2.0f * (q.y * z - q.z * y);
        const float ty = 2.0f * (q.z * x - q.x * z);
        const float tz = 2.0f * (q.x * y - q.y * x);
        out[0] = x + q.w * tx + (q.y * tz - q.z * ty);
        out[1] = y + q.w * ty + (q.z * tx - q.x * tz);
        out[2] = z + q.w * tz + (q.x * ty - q.y * tx);
    };
    float xrForward[3]{}, xrRight[3]{}, xrUp[3]{};
    rotate(0.0f, 0.0f, -1.0f, xrForward);   // OpenXR looks down -Z
    rotate(1.0f, 0.0f, 0.0f, xrRight);
    rotate(0.0f, 1.0f, 0.0f, xrUp);

    // OpenXR is +X right, +Y up, -Z forward. Source is +X forward, +Y left,
    // +Z up. Both right-handed, and this mapping has determinant +1, so no
    // handedness flip is introduced.
    const auto toSource = [](const float v[3], float out[3]) {
        out[0] = -v[2];
        out[1] = -v[0];
        out[2] = v[1];
    };
    float forward[3]{}, left[3]{}, up[3]{};
    const float xrLeft[3] = {-xrRight[0], -xrRight[1], -xrRight[2]};
    toSource(xrForward, forward);
    toSource(xrLeft, left);
    toSource(xrUp, up);

    // Publish the basis itself, rows forward/right/up, matching the layout the
    // engine's own AngleVectors produces. The consumer composes with this
    // directly; nothing in the chain converts to Euler angles until the single
    // final write into the game's angle field.
    //
    // Written between two odd/even generation bumps so a reader can tell it
    // caught a torn update. Nine floats cannot be published atomically, and a
    // torn basis is not a slightly wrong orientation, it is a non-rotation.
    g_headBasisGeneration = g_headBasisGeneration + 1;   // odd: write in progress
    g_headBasis[0] = forward[0]; g_headBasis[1] = forward[1]; g_headBasis[2] = forward[2];
    g_headBasis[3] = -left[0];   g_headBasis[4] = -left[1];   g_headBasis[5] = -left[2];
    g_headBasis[6] = up[0];      g_headBasis[7] = up[1];      g_headBasis[8] = up[2];
    g_headBasisGeneration = g_headBasisGeneration + 1;   // even: complete

    // Euler angles are still published, but only for logging and for the
    // recentre reference. Nothing composes with them any more.
    const float xyDistance = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1]);
    float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
    pitch = std::atan2(-forward[2], xyDistance);
    if (xyDistance > 0.001f) {
        yaw = std::atan2(forward[1], forward[0]);
        roll = std::atan2(left[2], up[2]);
    } else {
        yaw = std::atan2(-left[0], left[1]);
        roll = 0.0f;
    }

    constexpr float toDegrees = 57.2957795f;
    g_headYawDegrees = yaw * toDegrees;
    g_headPitchDegrees = pitch * toDegrees;
    g_headRollDegrees = roll * toDegrees;
    g_headPoseValid = 1;
    // Incremented so the consumer can tell a live pose from a frozen one. If
    // the runtime stops delivering, the camera write must release rather than
    // pin the view at the last sample.
    ++g_headPoseSequence;

    if (!haveLookBaseline_) { lastYaw_ = yaw; lastPitch_ = pitch; haveLookBaseline_ = true; return; }

    // This used to drive SendInput mouse injection. That mechanism is closed —
    // the game reads raw input — so the injection is gone rather than left
    // armable behind F9, where it could only confuse a head-tracking test.
    // What remains is a passive readout of the headset pose in the same units
    // and sign convention the real lever will use, so the numbers logged here
    // can be compared directly against the engine's own angles from F8.
    constexpr float degreesPerRadian = 57.2957795f;
    static unsigned int sample = 0;
    if (sample++ % 60 == 0) {
        char line[192]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] F9 head pose (passive): yaw=%.3f deg pitch=%.3f deg; "
            "delta since last sample yaw=%.3f pitch=%.3f\n",
            yaw * degreesPerRadian, pitch * degreesPerRadian,
            WrapRadians(yaw - lastYaw_) * degreesPerRadian,
            WrapRadians(pitch - lastPitch_) * degreesPerRadian);
        Log(line);
    }
    lastYaw_ = yaw; lastPitch_ = pitch;
}

// Passive, once per session. The runtime's recommended per-eye render target
// size is the honest target for image quality; everything downstream of the
// game's backbuffer is upscaling. Reported against the game's actual backbuffer
// so the gap is a number rather than an impression.
void XrContext::LogViewConfiguration() {
    std::uint32_t count = 0;
    if (!XrOk(xrEnumerateViewConfigurationViews(instance_, systemId_,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr), "count view configurations")) return;
    if (!count) return;
    std::vector<XrViewConfigurationView> views(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (!XrOk(xrEnumerateViewConfigurationViews(instance_, systemId_,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, views.data()), "read view configurations")) return;
    recommendedEyeWidth_ = views[0].recommendedImageRectWidth;
    recommendedEyeHeight_ = views[0].recommendedImageRectHeight;
    maxEyeWidth_ = views[0].maxImageRectWidth;
    maxEyeHeight_ = views[0].maxImageRectHeight;
    SetRuntimeRecommendedEyeSize(recommendedEyeWidth_, recommendedEyeHeight_);
    for (std::uint32_t view = 0; view < count; ++view) {
        char line[300]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] view config %u: recommended %ux%u (%u sample%s), max %ux%u, max samples %u.\n",
            view, views[view].recommendedImageRectWidth, views[view].recommendedImageRectHeight,
            views[view].recommendedSwapchainSampleCount,
            views[view].recommendedSwapchainSampleCount == 1 ? "" : "s",
            views[view].maxImageRectWidth, views[view].maxImageRectHeight,
            views[view].maxSwapchainSampleCount);
        Log(line);
    }
}

// THE OTHER HALF OF THE LAYER CONTRACT: does the declared POSE match the pose
// the game rendered with?
//
// FRUSTUM HANDED vs DECLARED settled the FOV half by measurement -- 1.0000 on
// both axes, in both arms of the fit_horizontal A/B, with the warp unchanged.
// A projection layer is a frustum AND a pose, and the pose half has never been
// measured. It is also where a yaw-only, FOV-invariant warp would live: the
// compositor reprojects from the declared pose to the display pose, so if the
// declared orientation is not the one the image was rendered with, every frame
// is reprojected by that error, and reprojection error is proportional to how
// fast the head is turning. That is a warp that appears on yaw, ignores every
// frustum change, and settles the instant the head stops.
//
// MAGNITUDES ONLY, AND STEPS RATHER THAN ABSOLUTES. The game is Source (Z up)
// and the runtime is OpenXR (Y up); converting between them to compare absolute
// headings is exactly the kind of sign-and-handedness work that produces a
// broken instrument, and a broken instrument here would print the most
// interesting result in the run. So this compares the SIZE of each frame's yaw
// step in each system's own frame. A rotation is the same size in both, whatever
// the convention, so the comparison needs no conversion to be valid.
//
// Reading it:
//   ratio ~ 0.0  the declared pose tracks the render; the pose half is clean
//   ratio ~ 1.0  the declared pose is one game frame away from the render
//   ratio ~ 2.0  two frames
// The ratio is normalised by the turn rate, so it means the same thing whether
// the wearer was turning slowly or quickly -- and the mean step is printed
// beside it so a window with no head motion in it can be discarded rather than
// read as a clean result.
void XrContext::TrackPoseAgreement(int eye) {
    float matrix[16]{};
    if (!GetMainSceneProjection(matrix, nullptr)) return;
    // Row 3's xyz is the camera forward axis and measures unit length; that is
    // what makes clip.w the view depth. Source is Z-up, so the heading lives in
    // the XY plane.
    const float gx = matrix[12], gy = matrix[13];
    const float gLen = std::sqrt(gx * gx + gy * gy);
    // The declared orientation's forward, by rotating (0,0,-1) through the
    // quaternion. OpenXR is Y-up, so the heading lives in the XZ plane.
    const XrQuaternionf& q = renderedView_[eye].pose.orientation;
    const float dx = -2.0f * (q.x * q.z + q.w * q.y);
    const float dz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const float dLen = std::sqrt(dx * dx + dz * dz);
    if (gLen < 0.01f || dLen < 0.01f) return;
    const float gnx = gx / gLen, gny = gy / gLen;
    const float dnx = dx / dLen, dnz = dz / dLen;
    if (poseTrackHavePrevious_) {
        constexpr float toDegrees = 57.2957795f;
        // Unsigned angle between successive headings, in each own plane.
        const float gameStep =
            std::acos(std::max(-1.0f, std::min(1.0f, gnx * poseTrackPrevGame_[0] + gny * poseTrackPrevGame_[1]))) * toDegrees;
        const float declStep =
            std::acos(std::max(-1.0f, std::min(1.0f, dnx * poseTrackPrevDecl_[0] + dnz * poseTrackPrevDecl_[1]))) * toDegrees;
        poseTrackGameSum_ += gameStep;
        poseTrackDeclSum_ += declStep;
        poseTrackDiffSum_ += std::fabs(declStep - gameStep);
        if (gameStep > poseTrackPeakStep_) poseTrackPeakStep_ = gameStep;
        if (++poseTrackFrames_ >= 120) {
            const double meanGame = poseTrackGameSum_ / poseTrackFrames_;
            const double meanDiff = poseTrackDiffSum_ / poseTrackFrames_;
            char line[620]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] POSE RENDERED vs DECLARED over %u frames: mean yaw step rendered %.4f deg, "
                "declared %.4f deg, mean |difference| %.4f deg, ratio %.3f, peak rendered step "
                "%.3f deg. Ratio near 0 is the declared pose tracking the render; near 1 is one "
                "whole game frame of disagreement, which the compositor reprojects away as warp on "
                "every turn. CONTROLS: a mean rendered step under about 0.02 deg means the head was "
                "still and the window says nothing; a declared mean near zero while the rendered "
                "mean is not means the quaternion read is broken and the ratio is an artefact, NOT "
                "a finding.\n",
                poseTrackFrames_, meanGame, poseTrackDeclSum_ / poseTrackFrames_, meanDiff,
                meanGame > 1e-4 ? meanDiff / meanGame : 0.0, poseTrackPeakStep_);
            Log(line);
            poseTrackFrames_ = 0;
            poseTrackGameSum_ = 0.0;
            poseTrackDeclSum_ = 0.0;
            poseTrackDiffSum_ = 0.0;
            poseTrackPeakStep_ = 0.0f;
        }
    }
    poseTrackPrevGame_[0] = gnx; poseTrackPrevGame_[1] = gny;
    poseTrackPrevDecl_[0] = dnx; poseTrackPrevDecl_[1] = dnz;
    poseTrackHavePrevious_ = true;
}

// G4 -- ONE LINE PER SESSION THAT CARRIES THE WHOLE CONFIGURATION.
//
// Every report from every headset has to arrive with its own geometry attached.
// Without it a future "it warps" from a different panel is unreadable, because
// none of the numbers the answer depends on are in the file.
//
// Everything here is READ BACK, never echoed from what was requested:
//   * the buffer is the swapchain the game actually created, against the size
//     the runtime recommended, so the scale factor is measured not assumed;
//   * cl_fovScale is read out of the engine, because a map load can replicate a
//     server value over ours and a reverted setting looks exactly like one that
//     never worked;
//   * the game frustum is the one the upload hook last published.
//
// The derived fov_scale is LOGGED AND NOT APPLIED. Source scales the 4:3 FOV by
// the cvar and then applies ScaleFOVByWidthRatio, so the aspect cancels out of
// the vertical and tanY = tan(B*s/2) * 3/4, with 3/4 the engine's 4:3 reference
// -- a GAME constant. B is the game's base fov_desired, recovered here from one
// measured pair rather than shipped as a number, which is what makes the target
// scale computable on a headset nobody has tested. Against the three archived
// calibration points on this one (1.48/87.7, 1.51/89.7, 1.55/92.9) it recovers
// B = 70.30 / 70.18 / 70.34 and lands on 1.511-1.514 for a 90 degree vertical,
// which is the dialled 1.51. It has to reproduce that here before anything is
// allowed to depend on it.
// The engine's own vertical ceiling, shared by the early learn below and the
// full banner: both must store the SAME usable tangent or the resolution
// derived in the menu would differ from the one derived after a world.
constexpr float kEngineMaxVerticalTanShared = 1.27325f;

void XrContext::LogHeadsetBanner(float ipdMetres) {
    if (headsetBannerLogged_) return;
    // THE RESOLUTION HALF FIRST, IN THE MENU. The first launch on a second
    // headset (2026-09-10 18:36) reported the panel at 8.3 s and learned it at
    // 26.7 s, because everything below waits for the GAME'S frustum -- which
    // exists only once a world renders. The per-eye size needs nothing from the
    // game: the recommended rect and the lens tangents, both known here. So the
    // geometry is learned once as soon as the fingerprint is valid, with the
    // FOV scale left at 0 (unknown); the full banner below fills the scale in
    // when the frustum arrives, and ApplyDerivedSetupNow applies it late. With
    // the geometry known, the derive fires from the tick 750 ms later, in the
    // menu, before anything the UI binds can be broken by a mode change.
    if (!geometryLearnedEarly_ && locatedViewsValid_ && recommendedEyeWidth_ &&
        recommendedEyeHeight_ && HeadsetFingerprint()[0]) {
        XrView early[2]{};
        {
            std::scoped_lock lock(viewMutex_);
            early[0] = locatedView_[0];
            early[1] = locatedView_[1];
        }
        const float earlyTanX = std::max(std::fabs(std::tan(early[0].fov.angleLeft)),
                                         std::fabs(std::tan(early[0].fov.angleRight)));
        const float earlyTanY = std::max(std::fabs(std::tan(early[0].fov.angleUp)),
                                         std::fabs(std::tan(early[0].fov.angleDown)));
        const float earlyUsableY =
            earlyTanY < kEngineMaxVerticalTanShared ? earlyTanY : kEngineMaxVerticalTanShared;
        if (earlyTanX > 0.0001f && earlyUsableY > 0.0001f) {
            geometryLearnedEarly_ = true;
            char line[300]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] headset geometry learned EARLY, in the menu: '%s', %ux%u per eye, tangents "
                "%.5f x %.5f. The FOV scale follows once the game's frustum exists.\n",
                HeadsetFingerprint(), recommendedEyeWidth_, recommendedEyeHeight_,
                static_cast<double>(earlyTanX), static_cast<double>(earlyUsableY));
            Log(line);
            LearnHeadsetGeometry(HeadsetFingerprint(), recommendedEyeWidth_, recommendedEyeHeight_,
                                 earlyTanX, earlyUsableY, 0.0f);
        }
    }
    float gameTanX = 0.0f, gameTanY = 0.0f;
    if (!GetMainSceneHalfTangents(gameTanX, gameTanY)) return;   // no frustum yet; try again next time
    if (!locatedViewsValid_ || gameTanY <= 0.0001f) return;
    headsetBannerLogged_ = true;

    constexpr float toDegrees = 57.2957795f;
    XrView view[2];
    { std::scoped_lock lock(viewMutex_); view[0] = locatedView_[0]; view[1] = locatedView_[1]; }

    // The symmetric cover of the runtime's per-eye FOV, by the same rule the
    // render uses (xr_context.cpp, LocateViews): the LARGER half of each axis,
    // so an asymmetric frustum is covered rather than clipped. Recomputed here
    // instead of read back, because this line has to state what the cover IS
    // alongside the angles it was built from.
    const float headTanX = std::max(std::fabs(std::tan(view[0].fov.angleLeft)),
                                    std::fabs(std::tan(view[0].fov.angleRight)));
    const float headTanY = std::max(std::fabs(std::tan(view[0].fov.angleUp)),
                                    std::fabs(std::tan(view[0].fov.angleDown)));

    // THE ENGINE'S OWN FOV CEILING, and the buffer aspect has to respect it.
    //
    // Measured, not assumed: asked for cl_fovScale 1.775 -- a 124.6 degree
    // equivalent -- and the game rendered a vertical of 103.71, a 119.0 degree
    // equivalent, whose half-tangent is 0.75 * tan(59.5) = 1.27325. That matched
    // the handed frustum to five places. Swept engine.dll and client.dll for a
    // max/clamp convar: there is none. The ceiling is the engine's, like the 0.75
    // 4:3 reference beside it, and it is a GAME constant rather than a headset
    // one.
    //
    // WHY THIS CHANGES THE ASPECT. The buffer aspect sets the game's HORIZONTAL
    // field of view as vertical x aspect. I had been using the headset's own
    // cover ratio, coverTanX / coverTanY -- correct only if the engine can
    // actually deliver coverTanY. On a Quest 3 it cannot: the headset wants 110
    // degrees of vertical and the engine stops at 103.71. Using the headset's
    // ratio then shrinks the HORIZONTAL to match a vertical that never arrives,
    // and the wearer gets 87.62 x 89.73 rendered into a 108.00 x 110.00 view --
    // a small window in the middle of the display, which is "I'm still too low to
    // the floor" and "the gun is way in the distance".
    //
    // So the aspect is the headset's horizontal cover over the vertical the
    // ENGINE CAN REACH. On a headset the engine can satisfy this is exactly the
    // old rule -- the PFD MR still lands on 1.4282 and cl_fovScale 1.5137, the
    // known-good pair, which is the falsifier for the change.
    constexpr float kEngineMaxVerticalTan = kEngineMaxVerticalTanShared;
    const float usableTanY = headTanY < kEngineMaxVerticalTan ? headTanY : kEngineMaxVerticalTan;

    // The correction factor, from the runtime's tangents against the game's.
    // Below 1 it narrows, above 1 it widens; the sign is not assumed anywhere.
    const float k = (headTanY > 0.0001f && gameTanX > 0.0001f)
        ? (gameTanY * (headTanX / headTanY)) / gameTanX : 0.0f;

    float scaleNow = 0.0f;
    const bool haveScale = TryReadCvarFloat("cl_fovScale", scaleNow);
    float baseFov = 0.0f, scaleTarget = 0.0f;
    if (haveScale && scaleNow > 0.01f && headTanY > 0.0001f) {
        baseFov = 2.0f * std::atan(gameTanY / 0.75f) * toDegrees / scaleNow;
        if (baseFov > 1.0f) scaleTarget = 2.0f * std::atan(usableTanY / 0.75f) * toDegrees / baseFov;
    }

    // A canted headset (Pimax-class) reports the two eyes with DIFFERENT
    // orientations. The stereo here is parallel-eyes and does not model that, so
    // it must be said out loud rather than rendered silently wrong.
    const XrQuaternionf& q0 = view[0].pose.orientation;
    const XrQuaternionf& q1 = view[1].pose.orientation;
    const float cantDot = std::fabs(q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w);
    const bool canted = cantDot < 0.99999f;

    const float scaleW = recommendedEyeWidth_
        ? static_cast<float>(gameDesc_.BufferDesc.Width) / recommendedEyeWidth_ : 0.0f;
    const float scaleH = recommendedEyeHeight_
        ? static_cast<float>(gameDesc_.BufferDesc.Height) / recommendedEyeHeight_ : 0.0f;

    char line[1100]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] HEADSET: runtime '%s' system '%s' | recommended per-eye %ux%u, max %ux%u | buffer "
        "%ux%u = %.3fx / %.3fx recommended | eye0 fov L%.2f R%.2f U%.2f D%.2f, eye1 fov L%.2f R%.2f "
        "U%.2f D%.2f deg | symmetric cover %.2f x %.2f deg (tan %.5f x %.5f), centre offset "
        "x=%.4f y=%.4f | IPD %.1f mm, half %.3f units, %.2f units/metre | game frustum %.2f x %.2f "
        "deg (tan %.5f x %.5f) | k=%.5f (%s) | cl_fovScale read back %.4f%s.\n",
        runtimeName_, systemName_,
        recommendedEyeWidth_, recommendedEyeHeight_, maxEyeWidth_, maxEyeHeight_,
        gameDesc_.BufferDesc.Width, gameDesc_.BufferDesc.Height, scaleW, scaleH,
        view[0].fov.angleLeft * toDegrees, view[0].fov.angleRight * toDegrees,
        view[0].fov.angleUp * toDegrees, view[0].fov.angleDown * toDegrees,
        view[1].fov.angleLeft * toDegrees, view[1].fov.angleRight * toDegrees,
        view[1].fov.angleUp * toDegrees, view[1].fov.angleDown * toDegrees,
        2.0f * std::atan(headTanX) * toDegrees, 2.0f * std::atan(headTanY) * toDegrees,
        headTanX, headTanY,
        (std::tan(view[0].fov.angleRight) + std::tan(view[0].fov.angleLeft)) /
            (std::tan(view[0].fov.angleRight) - std::tan(view[0].fov.angleLeft)),
        (std::tan(view[0].fov.angleUp) + std::tan(view[0].fov.angleDown)) /
            (std::tan(view[0].fov.angleUp) - std::tan(view[0].fov.angleDown)),
        ipdMetres * 1000.0f, HalfInterpupillaryUnits(), SourceUnitsPerMetre(),
        2.0f * std::atan(gameTanX) * toDegrees, 2.0f * std::atan(gameTanY) * toDegrees,
        gameTanX, gameTanY,
        k, k < 1.0f ? "narrowing" : "widening",
        haveScale ? scaleNow : 0.0f,
        haveScale ? "" : " (UNREADABLE -- the derived scale below is void)");
    Tf2VrLogWrite(line, true);

    char derived[520]{};
    std::snprintf(derived, sizeof(derived),
        "[TF2VR] HEADSET derived: base fov_desired recovered as %.3f deg from the measured pair "
        "(scale %.4f, vertical %.2f deg); the scale that lands this runtime's %.2f deg vertical is "
        "%.4f. LOGGED, NOT APPLIED. On this headset it must land on the dialled 1.51 -- if it does "
        "not, the relation is wrong and nothing may be built on it.\n",
        baseFov, haveScale ? scaleNow : 0.0f, 2.0f * std::atan(gameTanY) * toDegrees,
        2.0f * std::atan(headTanY) * toDegrees, scaleTarget);
    Log(derived);

    // LEARN THE HEADSET HERE, where every number it needs already exists and
    // was, until now, computed only to be printed.
    //
    // THE SYMMETRIC COVER, NOT THE FULL EXTENTS. headTanX/headTanY above are
    // max(|left|,|right|) and max(|up|,|down|) -- the smallest SYMMETRIC cone
    // that covers this headset's asymmetric one, which is what a game rendering
    // a symmetric frustum actually needs. The earlier derivation used the full
    // extent ratio (right-left over up-down), which is the same number only when
    // the frustum happens to be symmetric.
    //
    // Every headset this project had measured was symmetric, because it was
    // Virtual Desktop's fallback profile for a device it did not recognise. The
    // first real one -- a Quest 3 at L-54 R40 U44 D-55 -- gave 0.9255 by the old
    // rule against 0.9638 by this one, and the wearer saw "squashed, and a midget
    // down by the floor".
    //
    // scaleTarget is the cl_fovScale that lands THIS headset's vertical field of
    // view. It has been computed and logged "NOT APPLIED" since G0b; a hand-typed
    // 1.51 tuned for one headset's 90 degrees is the other half of that same
    // symptom on a headset whose vertical is 110.
    LearnHeadsetGeometry(HeadsetFingerprint(), recommendedEyeWidth_, recommendedEyeHeight_,
                         headTanX, usableTanY, scaleTarget);

    if (canted) {
        Log("[TF2VR] HEADSET LIMITATION: this runtime reports the two eyes with DIFFERENT "
            "orientations (canted panels, Pimax-class). The stereo here is parallel-eyes and does "
            "not model cant, so the render is a documented approximation, not a correct one. Treat "
            "any geometry judgement from this headset accordingly.\n");
    }
}

// Passive. Locates where the runtime wants each eye to be and what frustum it
// wants rendered there. Nothing here changes what is submitted; the quad path
// is untouched. It exists so the projection-layer work starts from measured
// numbers rather than from assumptions about headset geometry -- in particular
// the per-eye aspect ratio, which decides whether a 16:9 game image can be
// submitted through a projection layer at all without stretching.
void XrContext::LocateViews(XrTime predictedTime) {
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = predictedTime;
    locate.space = localSpace_;
    XrViewState state{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    std::uint32_t count = 0;
    if (!XrOk(xrLocateViews(session_, &locate, &state, 2, &count, views), "locate views")) return;
    // Both flags matter: a valid orientation with an invalid position would
    // give a projection layer the right shape in the wrong place.
    if (count < 2 || !(state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) ||
        !(state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
        std::scoped_lock lock(viewMutex_);
        locatedViewsValid_ = false;
        return;
    }
    {
        // Written on the pacing thread, read by the producer when it records
        // which pose an image was rendered against.
        std::scoped_lock lock(viewMutex_);
        locatedView_[0] = views[0];
        locatedView_[1] = views[1];
        locatedViewsValid_ = true;
    }

    // C1's POSE stream: the head's ABSOLUTE position in the runtime's LOCAL
    // space, which nothing else in this plugin ever prints. Every head number
    // the game sees is a delta from the recentre reference and measures ~0 on
    // both headsets; H3 is a claim about the value that delta was taken from,
    // and it cannot be tested against a quantity that has been differenced away.
    {
        const float centre[3] = {
            0.5f * (views[0].pose.position.x + views[1].pose.position.x),
            0.5f * (views[0].pose.position.y + views[1].pose.position.y),
            0.5f * (views[0].pose.position.z + views[1].pose.position.z)};
        PublishXrLocalHeadCentre(centre);
    }

    // The frustum the display can actually show, published so the main scene
    // can be rendered at it instead of at the game's wider one. Symmetric
    // half-tangents: this runtime reports a centred FOV (centre offset 0,0),
    // and the larger half of each axis is the safe choice if it ever does not.
    {
        const float halfTanX = std::max(std::fabs(std::tan(views[0].fov.angleLeft)),
                                        std::fabs(std::tan(views[0].fov.angleRight)));
        const float halfTanY = std::max(std::fabs(std::tan(views[0].fov.angleUp)),
                                        std::fabs(std::tan(views[0].fov.angleDown)));
        if (halfTanX > 0.0001f && halfTanY > 0.0001f) SetHeadsetHalfTangents(halfTanX, halfTanY);
    }

    // W2 -- the SIGNED vertical extents, which the symmetric pair above throws
    // away. std::max(|tanUp|, |tanDown|) is the right cover for "how wide must a
    // centred frustum be", and it is exactly the wrong thing for "where is the
    // centre": it maps the Quest 3's +44/-55 and a hypothetical +55/-44 onto the
    // same number. Both eyes report the same vertical pair on every headset
    // measured, so eye 0 speaks for the render; the DECLARATION is still built
    // per-eye from that eye's own view.
    SetHeadsetLensVerticalExtents(std::tan(views[0].fov.angleUp), std::tan(views[0].fov.angleDown));

    // THE WEARER'S REAL EYE HEIGHT, measured against the floor rather than
    // assumed. Same call, same instant, different space: locating the views in
    // STAGE gives eye positions whose y IS the height above the ground.
    //
    // Averaged across both eyes, because a head tilted to one side puts one eye
    // higher than the other and neither alone is "the wearer's eye height".
    //
    // MEASURED REPEATEDLY UNTIL IT SETTLES, then left alone. A single sample can
    // land while the headset is still on a desk, mid-hand-off, or being lifted
    // onto the head -- and that one bad sample would then BE the wearer's height
    // for the whole session. Requiring several consecutive samples to agree
    // within a centimetre costs nothing and cannot latch a transient.
    if (floorSpace_ != XR_NULL_HANDLE && !eyeHeightSettled_) {
        XrViewLocateInfo floorLocate{XR_TYPE_VIEW_LOCATE_INFO};
        floorLocate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        floorLocate.displayTime = predictedTime;
        floorLocate.space = floorSpace_;
        XrViewState floorState{XR_TYPE_VIEW_STATE};
        XrView floorViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        std::uint32_t floorCount = 0;
        const XrViewStateFlags need =
            XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        if (XR_SUCCEEDED(xrLocateViews(session_, &floorLocate, &floorState, 2, &floorCount, floorViews)) &&
            floorCount >= 2 && (floorState.viewStateFlags & need) == need) {
            const float metres =
                0.5f * (floorViews[0].pose.position.y + floorViews[1].pose.position.y);
            // A human eye, wearing a headset, sits between about 0.8 m (seated,
            // low chair) and 2.0 m. Anything outside that is the headset on a
            // desk or a runtime with no real floor, and must not become a height.
            if (metres > 0.80f && metres < 2.00f) {
                if (std::fabs(metres - eyeHeightSampleMetres_) < 0.01f) {
                    if (++eyeHeightStableSamples_ >= 5) {
                        eyeHeightSettled_ = true;
                        SetMeasuredEyeHeightMetres(metres);
                        char line[420]{};
                        std::snprintf(line, sizeof(line),
                            "[TF2VR] EYE HEIGHT MEASURED against the floor: %.3f m (%.1f inches), "
                            "stable across %d samples. This is the wearer's ACTUAL eye height, not "
                            "an assumption -- seated it reads low and standing it reads high, which "
                            "is the whole point. The camera correction derived from it follows.\n",
                            metres, metres * 39.3701f, eyeHeightStableSamples_);
                        Log(line);
                    }
                } else {
                    eyeHeightSampleMetres_ = metres;
                    eyeHeightStableSamples_ = 1;
                }
            }
        }
    }

    if (viewLogCountdown_) return;
    viewLogCountdown_ = 600;
    constexpr float toDegrees = 57.2957795f;
    for (int eye = 0; eye < 2; ++eye) {
        const XrFovf& fov = views[eye].fov;
        // Published for the menu's video page. The HEADSET'S OWN cone is the only
        // honest reference for "are we rendering pixels nobody sees" -- the
        // runtime's recommended RECT is not, because its aspect (1.105 here) is
        // nothing like its FOV shape (1.428), and comparing a pixel count against
        // it produces a waste figure that is simply wrong.
        if (eye == 0) {
            SetHeadsetFovTangents(std::tan(fov.angleRight) - std::tan(fov.angleLeft),
                                  std::tan(fov.angleUp) - std::tan(fov.angleDown));
        }
        const XrVector3f& position = views[eye].pose.position;
        // Tangents, because that is the form the projection matrix needs, and
        // the aspect they imply is the number that decides the layout.
        const float tanLeft = std::tan(fov.angleLeft), tanRight = std::tan(fov.angleRight);
        const float tanDown = std::tan(fov.angleDown), tanUp = std::tan(fov.angleUp);
        const float width = tanRight - tanLeft, height = tanUp - tanDown;
        char line[420]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] xrLocateViews eye %d: fov L=%.2f R=%.2f U=%.2f D=%.2f deg "
            "(horizontal=%.2f vertical=%.2f, aspect=%.4f, centre offset x=%.4f y=%.4f) "
            "pose=(%.4f,%.4f,%.4f) quat=(%.4f,%.4f,%.4f,%.4f)\n",
            eye, fov.angleLeft * toDegrees, fov.angleRight * toDegrees,
            fov.angleUp * toDegrees, fov.angleDown * toDegrees,
            (fov.angleRight - fov.angleLeft) * toDegrees, (fov.angleUp - fov.angleDown) * toDegrees,
            height != 0.0f ? width / height : 0.0f,
            width != 0.0f ? (tanRight + tanLeft) / width : 0.0f,
            height != 0.0f ? (tanUp + tanDown) / height : 0.0f,
            position.x, position.y, position.z,
            views[eye].pose.orientation.x, views[eye].pose.orientation.y,
            views[eye].pose.orientation.z, views[eye].pose.orientation.w);
        Log(line);
    }
    // The interpupillary distance the runtime is actually reporting, against
    // the 64 mm the camera offset currently assumes.
    const float dx = locatedView_[1].pose.position.x - locatedView_[0].pose.position.x;
    const float dy = locatedView_[1].pose.position.y - locatedView_[0].pose.position.y;
    const float dz = locatedView_[1].pose.position.z - locatedView_[0].pose.position.z;
    const float ipdMetres = std::sqrt(dx * dx + dy * dy + dz * dz);
    SetRuntimeIpdMetres(ipdMetres);
    LogHeadsetBanner(ipdMetres);
    char line[240]{};
    float halfTanX = 0.0f, halfTanY = 0.0f;
    if (GetMainSceneHalfTangents(halfTanX, halfTanY)) {
        std::snprintf(line, sizeof(line),
            "[TF2VR] runtime IPD=%.1f mm; game main-scene frustum %.1f x %.1f deg vs headset %.1f x %.1f.\n",
            ipdMetres * 1000.0f, 2.0f * std::atan(halfTanX) * toDegrees, 2.0f * std::atan(halfTanY) * toDegrees,
            (locatedView_[0].fov.angleRight - locatedView_[0].fov.angleLeft) * toDegrees,
            (locatedView_[0].fov.angleUp - locatedView_[0].fov.angleDown) * toDegrees);
    } else {
        std::snprintf(line, sizeof(line),
            "[TF2VR] runtime IPD=%.1f mm; the game's frustum has not been measured yet (no camera hook, "
            "or no main-scene upload seen).\n", ipdMetres * 1000.0f);
    }
    Log(line);
    // The game image's aspect and size, for the same reason: a projection layer
    // with headset FOV and a 16:9 image stretches unless the two are
    // reconciled, and the game's backbuffer is the fidelity ceiling for both
    // eyes regardless of what the runtime would like.
    char sizeLine[400]{};
    std::snprintf(sizeLine, sizeof(sizeLine),
        "[TF2VR] game backbuffer %ux%u (aspect %.4f); runtime wants %ux%u per eye "
        "(%.2fx the width, %.2fx the height, %.2fx the pixels of one game frame).\n",
        gameDesc_.BufferDesc.Width, gameDesc_.BufferDesc.Height,
        gameDesc_.BufferDesc.Height ? static_cast<float>(gameDesc_.BufferDesc.Width) / gameDesc_.BufferDesc.Height : 0.0f,
        recommendedEyeWidth_, recommendedEyeHeight_,
        gameDesc_.BufferDesc.Width ? static_cast<float>(recommendedEyeWidth_) / gameDesc_.BufferDesc.Width : 0.0f,
        gameDesc_.BufferDesc.Height ? static_cast<float>(recommendedEyeHeight_) / gameDesc_.BufferDesc.Height : 0.0f,
        (gameDesc_.BufferDesc.Width && gameDesc_.BufferDesc.Height)
            ? (static_cast<float>(recommendedEyeWidth_) * recommendedEyeHeight_) /
              (static_cast<float>(gameDesc_.BufferDesc.Width) * gameDesc_.BufferDesc.Height)
            : 0.0f);
    Log(sizeLine);
}

void XrContext::SetStereoEnabled(bool enabled) {
    stereoEnabled_ = enabled;
    if (!enabled) { eyeHasImage_[0] = false; eyeHasImage_[1] = false; }
}

void XrContext::SetProjectionLayerEnabled(bool enabled) {
    projectionLayerEnabled_ = enabled;
    // Re-anchor on each arm, so the world sits where the player is standing now
    // rather than where they were when the session started.
    projectionOriginValid_ = false;
    // Force the next frame to state which path it took and why.
    projectionFallbackReason_ = -1;
}

// THE GAME'S OWN FRAME. Unchanged from the path that has worked for months,
// except that it now takes frameMutex_ so the pacing thread cannot be inside a
// frame at the same time.
bool XrContext::SubmitEyeTexture(ID3D11Texture2D* source, int eye) {
    if (!source) return false;
    return SubmitFrameCore(source, eye, true);
}

bool XrContext::SubmitBackbuffer(IDXGISwapChain* gameSwapchain, int eye) {
    if (!enabled_) return true;
    // Initialisation before the lock: it can call Shutdown(), which joins the
    // pacing thread, and joining while holding a lock that thread wants would
    // be a deadlock.
    if (!initialized_ && !Initialize(gameSwapchain)) { Shutdown(); return false; }
    // Announce before blocking, so the pacing thread stops competing. Cleared
    // once we hold the lock. See presentWaiting_ for what happens without it.
    presentWaiting_.store(true, std::memory_order_release);
    std::unique_lock<std::mutex> lock(frameMutex_);
    presentWaiting_.store(false, std::memory_order_release);
    if (!PollEvents()) return true;
    // Before any frame is begun, so a rebuild never happens between
    // xrBeginFrame and xrEndFrame -- and under the lock, so the pacing thread
    // cannot be referencing the swapchains while they are destroyed.
    if (!HandleBackbufferResize(gameSwapchain)) return false;
    MaybeStartPacingThread();
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
    // A failed GetBuffer is not fatal to the frame: the core still runs the
    // wait/begin/end triple and resubmits the previous image, which is what
    // keeps a missed copy from becoming a black flash.
    gameSwapchain->GetBuffer(0, IID_PPV_ARGS(&source));
    const bool ok = SubmitFrameCore(source.Get(), eye, true);
    // The eye and the count of REAL game frames, so the pacing thread can
    // re-show one without ever advancing the eye itself.
    lastPublishedEye_.store(eye, std::memory_order_release);
    publishedFrames_.fetch_add(1, std::memory_order_acq_rel);
    // THE JANK METRIC, and it works whether or not the pacer is running.
    //
    // The DECOUPLE beat only exists while decoupled, so the coupled loading
    // screen -- the thing being complained about -- has never been measured at
    // all. What matters to the wearer is not the mean rate but the LONGEST gap
    // between frames: a 250 ms hole is a visible freeze however good the
    // average looks around it.
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const long long previous = lastGamePresentQpc_.exchange(now.QuadPart, std::memory_order_acq_rel);
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    if (previous && frequency.QuadPart) {
        const double gapMs = static_cast<double>(now.QuadPart - previous) * 1000.0 / frequency.QuadPart;
        if (gapMs > presentWorstGapMs_) presentWorstGapMs_ = gapMs;
        ++presentBeatFrames_;
        if (!presentBeatStartQpc_) presentBeatStartQpc_ = previous;
        const double sinceBeat =
            static_cast<double>(now.QuadPart - presentBeatStartQpc_) * 1000.0 / frequency.QuadPart;
        if (sinceBeat >= 2000.0) {
            char line[560]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] PRESENT beat: %u game frames in %.0f ms = %.1f/s against a declared %.1f Hz "
                "| WORST gap %.1f ms (%.1f frames' worth) | decoupled=%d. The worst gap is what the "
                "wearer actually sees on a loading screen; the mean hides it.\n",
                presentBeatFrames_, sinceBeat, presentBeatFrames_ * 1000.0 / sinceBeat,
                displayPeriodMs_.load(std::memory_order_acquire) > 0.0
                    ? 1000.0 / displayPeriodMs_.load(std::memory_order_acquire) : 0.0,
                presentWorstGapMs_,
                displayPeriodMs_.load(std::memory_order_acquire) > 0.0
                    ? presentWorstGapMs_ / displayPeriodMs_.load(std::memory_order_acquire) : 0.0,
                decoupleActive_.load(std::memory_order_acquire) ? 1 : 0);
            Log(line);
            if (!decoupleActive_.load(std::memory_order_acquire)) { LogResourceHealth("coupled"); LogMultithreadStatus(); }
            presentBeatFrames_ = 0;
            presentWorstGapMs_ = 0.0;
            presentBeatStartQpc_ = now.QuadPart;
        }
    }
    return ok;
}

bool XrContext::SubmitFrameCore(ID3D11Texture2D* source, int eye, bool doImageWork) {
    // Per-call timing, but read the sum, not the parts.
    //
    // "xrWaitFrame is supposed to block and xrEndFrame is not" was the premise
    // of three sessions of work and it is wrong for this runtime. Both calls
    // draw on the same pacing budget and the runtime parks the app in whichever
    // one the frame's phase puts it in: across the archived runs WAIT+END is a
    // per-run constant (~11.9 ms healthy, ~98 ms stalled) while the split
    // between them wanders freely -- one healthy window reads WAIT 0.21 / END
    // 11.72 and the next WAIT 10.38 / END 1.48.
    //
    // So an 88 ms xrEndFrame next to a 10 ms xrWaitFrame is not a stall inside
    // xrEndFrame. It is the app being paced at 10 Hz with the wait happening to
    // land in END. The parts are still recorded, because the split is evidence
    // of where the runtime chose to block, but every conclusion comes from the
    // sum and from the period the runtime declares alongside it.
    LARGE_INTEGER tickFrequency{}, t0{}, t1{}, t2{}, t3{};
    QueryPerformanceFrequency(&tickFrequency);
    QueryPerformanceCounter(&t0);

    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    XrBreadcrumb::SetStage(XrBreadcrumb::Stage::WaitFrame);
    if (!XrOk(xrWaitFrame(session_, &waitInfo, &frameState), "wait frame")) return false;
    QueryPerformanceCounter(&t1);
    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    XrBreadcrumb::SetStage(XrBreadcrumb::Stage::BeginFrame);
    if (!XrOk(xrBeginFrame(session_, &beginInfo), "begin frame")) return false;
    // THE GAME'S HEAD POSE UPDATES ONCE PER GAME FRAME, NOT ONCE PER XR FRAME.
    //
    // This publishes g_headYawDegrees, which is the orientation the game renders
    // the world with. Running it on every XR frame gives it the pacing thread's
    // clock -- and with the game held near but not locked to that rate (measured
    // 11.64 ms against a 12.5 ms period) the two BEAT, about six times a second.
    // That is the head-motion stutter: not a wrong pose, two clocks.
    //
    // Gating it on a real game frame locks it to the game's rendering, which is
    // the relationship the coupled path had for free by being the same call.
    //
    // LocateViews below still runs EVERY frame, so the compositor keeps getting
    // a fresh pose to reproject against at the display rate -- which is what
    // makes the loading screen smooth, and is deliberately left alone.
    if (doImageWork) UpdateLookInjection(frameState.predictedDisplayTime);
    // Once per frame, on the same predicted display time the head pose uses, so
    // the hands and the head are sampled for the same instant. It publishes and
    // logs; nothing downstream consumes it yet.
    // REAL GAME FRAMES ONLY. xrSyncActions is a heavyweight runtime call and it
    // republishes the controller state the game thread reads; running it again
    // on every gap frame would sync input two or three times per game frame,
    // from a second thread, for no benefit. Input belongs to the game's frame.
    if (doImageWork) XrInputSync(session_, localSpace_, viewSpace_, frameState.predictedDisplayTime);
    // Snapshot before relocating: in alternate-frame stereo the image about to
    // be copied was rendered against the PREVIOUS frame's camera, so that is
    // the pose a projection layer would have to be submitted with.
    XrView previousLocated[2]{};
    bool previousLocatedValid = false;
    {
        std::scoped_lock lock(viewMutex_);
        previousLocated[0] = locatedView_[0];
        previousLocated[1] = locatedView_[1];
        previousLocatedValid = locatedViewsValid_;
    }
    if (viewLogCountdown_) --viewLogCountdown_;
    LocateViews(frameState.predictedDisplayTime);

    // In stereo the frame just rendered belongs to exactly one eye, so only
    // that eye's swapchain is written.  The other eye keeps the image it was
    // given on the previous frame.
    const bool useStereo = stereoEnabled_ && eye >= 0 && eye <= 1 &&
        eyeSwapchain_[0] != XR_NULL_HANDLE && eyeSwapchain_[1] != XR_NULL_HANDLE;
    const XrSwapchain target = useStereo ? eyeSwapchain_[eye] : colorSwapchain_;
    auto& targetImages = useStereo ? eyeImages_[eye] : images_;

    std::uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo imageWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; imageWait.timeout = XR_INFINITE_DURATION;
    bool acquired = false;
    bool readyToSubmit = false;
    // NO SOURCE, NO ACQUIRE -- and this matters far more once the pacing thread
    // exists.
    //
    // Acquiring an image and releasing it without writing to it does not mean
    // "keep showing the last frame". The compositor shows the most recently
    // RELEASED image, so an untouched acquire publishes whatever that slot held
    // two or three frames ago: the view jumps backwards in time. Coupled, that
    // happened only on the rare frame whose copy failed. Decoupled it would
    // happen on every frame the game did not produce -- which during a load is
    // nearly all of them -- and the wearer would see the picture flickering
    // between two different old frames rather than holding still.
    //
    // Leaving the swapchain alone and submitting the layer anyway is the
    // supported way to re-show the last released image, so that is what a frame
    // with no fresh source does.
    if (doImageWork && source && frameState.shouldRender &&
        (XrBreadcrumb::SetStage(XrBreadcrumb::Stage::AcquireImage), true) &&
        XrOk(xrAcquireSwapchainImage(target, &acquire, &imageIndex), "acquire image")) {
        acquired = true;
        XrBreadcrumb::SetStage(XrBreadcrumb::Stage::WaitImage);
        readyToSubmit = XrOk(xrWaitSwapchainImage(target, &imageWait), "wait image");
    }
    if (readyToSubmit) {
        if (source) {
            // CopyResource between differently sized or formatted textures is a
            // silent no-op: no HRESULT, no exception, just a swapchain image
            // that never updates and a headset frozen on its last good frame.
            // Check it, and say so once rather than once per frame.
            D3D11_TEXTURE2D_DESC sourceDesc{}, targetDesc{};
            source->GetDesc(&sourceDesc);
            targetImages[imageIndex]->GetDesc(&targetDesc);
            // DIMENSIONS ONLY. The format deliberately is not compared: OpenXR
            // runtimes routinely hand back a TYPELESS image for a typed request
            // (R8G8B8A8_UNORM_SRGB in, R8G8B8A8_TYPELESS out), and CopyResource
            // is perfectly happy across a typeless family. An earlier version of
            // this check compared formats too, rejected every copy on a healthy
            // session, and turned the headset black the moment OpenXR was armed.
            if (sourceDesc.Width != targetDesc.Width || sourceDesc.Height != targetDesc.Height) {
                if (!copyMismatchReported_) {
                    copyMismatchReported_ = true;
                    char line[320]{};
                    std::snprintf(line, sizeof(line),
                        "[TF2VR] backbuffer %ux%u (format %u) does not match the XR image %ux%u (format %u); "
                        "the copy would be a no-op, so this frame is not submitted.\n",
                        sourceDesc.Width, sourceDesc.Height, static_cast<unsigned>(sourceDesc.Format),
                        targetDesc.Width, targetDesc.Height, static_cast<unsigned>(targetDesc.Format));
                    Log(line);
                }
                readyToSubmit = false;
            } else {
                copyMismatchReported_ = false;
                // THE KEYED MUTEX IS HELD ONLY ACROSS THIS COPY.
                //
                // When the source is the cross-device shared texture, ownership
                // is taken here and handed straight back, so the game thread is
                // locked out for the length of one CopyResource rather than for
                // the ~11 ms this function spends blocked in xrWaitFrame.
                const int sharedSlot = sharedReadSlot_;
                const bool sharedSource = sharedSlot >= 0 && sharedSlot < kSharedCount &&
                                          xrSharedMutex_[sharedSlot];
                if (sharedSource &&
                    xrSharedMutex_[sharedSlot]->AcquireSync(kSharedKeyXr, 0) != S_OK) {
                    // The game is mid-publish on this slot. Nothing newer to
                    // show than what the swapchain already holds, so re-show it.
                    readyToSubmit = false;
                } else {
                    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context; device_->GetImmediateContext(&context);
                    context->CopyResource(targetImages[imageIndex].Get(), source);
                    // Hand it straight back. Holding it any longer than the copy
                    // is what limited the game to publishing every other frame.
                    if (sharedSource) xrSharedMutex_[sharedSlot]->ReleaseSync(kSharedKeyGame);
                }
            }
        } else {
            // No source this frame. The wait/begin/end triple still completes
            // and the previous image is resubmitted, which is the whole reason
            // this is not treated as an error.
            readyToSubmit = false;
        }
    }
    if (acquired) {
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        XrBreadcrumb::SetStage(XrBreadcrumb::Stage::ReleaseImage);
        if (!XrOk(xrReleaseSwapchainImage(target, &release), "release image")) readyToSubmit = false;
    }
    QueryPerformanceCounter(&t2);
    if (useStereo && readyToSubmit) {
        eyeHasImage_[eye] = true;
        // Prefer the pose captured WITH the frame. Decoupled, "the previous
        // locate" belongs to this thread's own cadence and not to the moment the
        // game rendered the picture; coupled, consumeViewValid_ is false and the
        // previous locate is exactly right, as it always was.
        const XrView* renderedFrom = consumeViewValid_ ? consumeView_ : previousLocated;
        if (consumeViewValid_ || previousLocatedValid) {
            renderedView_[eye] = renderedFrom[eye];
            renderedHeadCentre_[eye] = {
                0.5f * (renderedFrom[0].pose.position.x + renderedFrom[1].pose.position.x),
                0.5f * (renderedFrom[0].pose.position.y + renderedFrom[1].pose.position.y),
                0.5f * (renderedFrom[0].pose.position.z + renderedFrom[1].pose.position.z)};
            renderedViewValid_[eye] = true;
            if (!projectionOriginValid_) {
                projectionOrigin_ = renderedHeadCentre_[eye];
                projectionOriginValid_ = true;
            }
        }
        if (doImageWork) TrackPoseAgreement(eye);
    }
    const XrExtent2Di extent{static_cast<std::int32_t>(gameDesc_.BufferDesc.Width),
                             static_cast<std::int32_t>(gameDesc_.BufferDesc.Height)};
    const float quadWidth = 2.4f;
    const float quadHeight = quadWidth * static_cast<float>(gameDesc_.BufferDesc.Height) / gameDesc_.BufferDesc.Width;

    // Both eyes must have been filled at least once before stereo is shown,
    // otherwise one eye would present a blank image against a live one.
    const bool stereoReady = useStereo && eyeHasImage_[0] && eyeHasImage_[1];
    // Stereo is gated on the same condition, for the same reason.
    const bool mayShowLayers = frameState.shouldRender;

    // STRANDED: shouldRender false for SECONDS, not for a frame.
    //
    // Everything below is deliberately gated on shouldRender, and that gate is
    // right for the case it was written for -- a frame or two where the runtime
    // says it is not displaying us, during which handing it layers anyway makes
    // xrEndFrame block for 138 ms. It has no answer for shouldRender staying
    // false, and on 2026-09-09 it stayed false from the moment the player died
    // until the wearer quit: 26 seconds of submitting nothing, which the
    // compositor renders as its own screen and the wearer described as "a black
    // square stuck to my view as I move my head".
    //
    // THE REMEDY IS CONDITIONAL ON THE DIAGNOSIS, not on a hunch. If the Windows
    // foreground window belongs to ANOTHER process, the game lost the desktop
    // and the runtime followed it -- and taking the foreground back is the fix
    // for that, and only that. If the foreground is already ours, a system
    // overlay is up, the runtime is entitled to want no layers, and this does
    // nothing at all except say so.
    //
    // Bounded: at most one attempt every five seconds, each one logged, and
    // never while the session is genuinely not being displayed.
    {
        static std::uint64_t strandedSinceMs = 0;
        static std::uint64_t lastAttemptMs = 0;
        const std::uint64_t nowMs = GetTickCount64();
        const bool displayed = sessionState_ == XR_SESSION_STATE_VISIBLE ||
                               sessionState_ == XR_SESSION_STATE_FOCUSED;
        if (frameState.shouldRender || !displayed) {
            strandedSinceMs = 0;
        } else {
            if (!strandedSinceMs) strandedSinceMs = nowMs;
            if (nowMs - strandedSinceMs >= 2000 && nowMs - lastAttemptMs >= 5000) {
                lastAttemptMs = nowMs;
                const HWND fg = GetForegroundWindow();
                DWORD fgPid = 0;
                if (fg) GetWindowThreadProcessId(fg, &fgPid);
                const bool foreignForeground = fg && fgPid != GetCurrentProcessId();
                char line[420]{};
                if (foreignForeground) {
                    const HWND ours = gameDesc_.OutputWindow;   // the swapchain names its own window
                    const bool asked = ours && SetForegroundWindow(ours) != 0;
                    std::snprintf(line, sizeof(line),
                        "[TF2VR] STRANDED %llu ms with no layers: the runtime says do not render and "
                        "ANOTHER PROCESS (pid %lu) holds the Windows foreground. Asking for it back "
                        "for our window %p: %s.\n",
                        static_cast<unsigned long long>(nowMs - strandedSinceMs),
                        static_cast<unsigned long>(fgPid), static_cast<void*>(ours),
                        !ours ? "no window recorded" : asked ? "accepted" : "REFUSED by Windows");
                } else {
                    std::snprintf(line, sizeof(line),
                        "[TF2VR] STRANDED %llu ms with no layers, and the Windows foreground is ALREADY "
                        "OURS. So this is the runtime holding focus for its own overlay, not a lost "
                        "window, and nothing here can take it back. Not acting.\n",
                        static_cast<unsigned long long>(nowMs - strandedSinceMs));
                }
                Log(line);
            }
        }
    }

    XrCompositionLayerQuad quads[2]{};
    for (int index = 0; index < 2; ++index) {
        quads[index].type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        // WORLD-LOCKED, AND IT STAYS THAT WAY. The wearer wants "a screen in
        // front of me I can move my head to see fully", which is exactly what
        // localSpace_ gives. Head-locking was tried and hated; see the note at
        // g_headlockQuad, which also records that it did not cure the judder.
        //
        // That result is worth keeping for whoever picks the judder up next: a
        // VIEW-space quad cannot move relative to the head, so the loading
        // screen juddering identically in both spaces rules the layer's
        // ANCHORING out entirely. What is left is frame TIMING -- the loading
        // window measures 40 fps against 80 in gameplay, with xrWaitFrame
        // blocking only 3.6 ms of a 25 ms frame, so every submission is about
        // one display period late.
        quads[index].space = g_headlockQuad ? viewSpace_ : localSpace_;
        quads[index].pose.orientation.w = 1.0f;
        quads[index].pose.position.z = -2.0f;
        quads[index].size = {quadWidth, quadHeight};
        quads[index].subImage.imageRect.extent = extent;
    }
    quads[0].eyeVisibility = stereoReady ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_BOTH;
    quads[0].subImage.swapchain = stereoReady ? eyeSwapchain_[0] : colorSwapchain_;
    quads[1].eyeVisibility = XR_EYE_VISIBILITY_RIGHT;
    quads[1].subImage.swapchain = stereoReady ? eyeSwapchain_[1] : colorSwapchain_;

    // The projection layer. It declares the frustum the game ACTUALLY rendered
    // rather than the headset's preferred one -- that is what makes it correct
    // without touching the camera matrix. The XrFovf on a projection view
    // describes the submitted image; state it truthfully and the compositor
    // places the world correctly. Matching the headset's FOV exactly is only a
    // pixel-efficiency question, and at FOV 110 the game's 123.7 x 92.9 already
    // contains the headset's 110 x 90 in both axes, so nothing is missing from
    // the edges.
    //
    // Each eye is submitted with the pose ITS frame was rendered for, not the
    // current pose: in alternate-frame stereo the two images are a frame apart,
    // and using the current pose for both would look right in a screenshot and
    // wrong on a moving head.
    float halfTanX = 0.0f, halfTanY = 0.0f;
    int fallbackReason = 0;
    if (!projectionLayerEnabled_) fallbackReason = 1;
    else if (!stereoReady) fallbackReason = 2;
    else if (!g_headCompensationValid) fallbackReason = 3;
    else if (!GetMainSceneHalfTangents(halfTanX, halfTanY)) fallbackReason = 4;
    else if (!renderedViewValid_[0] || !renderedViewValid_[1]) fallbackReason = 5;

    if (fallbackReason != projectionFallbackReason_) {
        projectionFallbackReason_ = fallbackReason;
        static const char* const kReasons[] = {
            "[TF2VR] projection layer active; the world is submitted as a real frustum.\n",
            "[TF2VR] projection layer off; using the quad path.\n",
            "[TF2VR] projection layer waiting: both eyes need an image first (arm stereo).\n",
            "[TF2VR] projection layer waiting: head tracking is not driving the view. Without it the "
                "world would be head-locked to your face. Arm head tracking.\n",
            "[TF2VR] projection layer waiting: the main scene's frustum has not been measured yet. The "
                "camera upload hook has to be installed and a near-plane -7 upload seen.\n",
            "[TF2VR] projection layer waiting: neither eye has a recorded render pose yet.\n",
        };
        Log(kReasons[fallbackReason]);
    }

    // IS THE FRUSTUM WE ARE ABOUT TO DECLARE THE ONE THE GAME JUST DREW?
    //
    // READ-ONLY. Nothing below behaves differently for this; it decides nothing
    // and gates nothing. It exists because the answer is currently unknown and
    // the wearer should not spend a run finding out by eye.
    //
    // The measured pair has one write site and no invalidation, so once a single
    // main-scene upload has been seen GetMainSceneHalfTangents() succeeds for the
    // rest of the process -- with whatever it last saw. Both the declared FOV and
    // the sub-rectangle submitted below come from it. If the game stops producing
    // uploads this hook recognises, the compositor is handed a stale frustum over
    // a live buffer and stretches one to the other.
    //
    // EDGE-TRIGGERED, WITH A HEARTBEAT. A line per frame would bury the run log;
    // a line only on the edge would make a four-minute stall look like one blip,
    // which is precisely the shape KNOWN-ISSUES 9 was reported in. So: both
    // edges, plus one line per second for as long as it lasts.
    {
        static bool stale = false;
        static std::uint64_t lastBeatMs = 0;
        static std::uint64_t stalledSinceMs = 0;
        static unsigned episodes = 0;
        const std::uint64_t ageMs = MainSceneFrustumAgeMs();
        const std::uint64_t nowMs = GetTickCount64();
        // 250 ms is ~20 frames at 80 Hz and ~15 at the loading-screen rate. Well
        // past any single dropped frame, well short of anything a wearer could
        // fail to notice.
        const bool nowStale = ageMs != ~0ull && ageMs > 250;
        if (nowStale != stale) {
            stale = nowStale;
            if (stale) { stalledSinceMs = nowMs; ++episodes; }
            char line[420]{};
            std::snprintf(line, sizeof(line),
                stale
                    ? "[TF2VR] FRUSTUM STALE (episode %u): the measured main-scene frustum is %llu ms "
                      "old and the projection layer is still declaring it. %llu measurements so far "
                      "this session. If the picture looks magnified right now, this is why.\n"
                    : "[TF2VR] frustum fresh again after %u episode(s): last measurement %llu ms ago, "
                      "%llu total. The stall lasted %llu ms.\n",
                episodes, static_cast<unsigned long long>(ageMs),
                static_cast<unsigned long long>(MainSceneFrustumWrites()),
                static_cast<unsigned long long>(stale ? 0 : nowMs - stalledSinceMs));
            Log(line);
            lastBeatMs = nowMs;
        } else if (stale && nowMs - lastBeatMs >= 1000) {
            lastBeatMs = nowMs;
            char line[300]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR]   frustum still stale: %llu ms, declaring %.1f x %.1f deg over the buffer.\n",
                static_cast<unsigned long long>(ageMs),
                static_cast<double>(2.0f * std::atan(halfTanX) * 57.2957795f),
                static_cast<double>(2.0f * std::atan(halfTanY) * 57.2957795f));
            Log(line);
        }
    }

    XrCompositionLayerProjectionView projectionViews[2]{};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const bool useProjection = fallbackReason == 0;
    if (useProjection) {
        // renderedView_/renderedHeadCentre_/projectionOrigin_ are written by the
        // PRODUCER when it copies an image in, and read here on the pacing
        // thread. Same lock the producer takes: this is the pose the compositor
        // reprojects against, and half of one frame's pose mixed with half of
        // another's is exactly the per-eye error that read as flicker in 1.6-A.
        std::scoped_lock lock(viewMutex_);
        for (int eye = 0; eye < 2; ++eye) {
            projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            // Orientation is honest: the game camera really does follow the
            // head's rotation. Position is not -- the game camera is pinned to
            // the player's eye point and does not translate with the head, so
            // the only real displacement is the eye separation. Claiming the
            // full tracked position makes the compositor reproject for a
            // translation the image was never rendered with, which reads as the
            // world warping when you lean. Keep the per-eye offset, anchor the
            // midpoint. Proper 6DoF means driving the game's camera position;
            // until then this is the truthful pose, not a workaround.
            projectionViews[eye].pose.orientation = renderedView_[eye].pose.orientation;
            projectionViews[eye].pose.position = {
                projectionOrigin_.x + (renderedView_[eye].pose.position.x - renderedHeadCentre_[eye].x),
                projectionOrigin_.y + (renderedView_[eye].pose.position.y - renderedHeadCentre_[eye].y),
                projectionOrigin_.z + (renderedView_[eye].pose.position.z - renderedHeadCentre_[eye].z)};
            // C1's POSE stream, from the one place that holds all three at once.
            // Left eye only: the two differ by the eye separation and nothing
            // else, so publishing both would double the traffic to say the same
            // thing twice.
            if (eye == 0) {
                const float origin[3] = {projectionOrigin_.x, projectionOrigin_.y,
                                         projectionOrigin_.z};
                const float centre[3] = {renderedHeadCentre_[0].x, renderedHeadCentre_[0].y,
                                         renderedHeadCentre_[0].z};
                const float declared[3] = {projectionViews[0].pose.position.x,
                                           projectionViews[0].pose.position.y,
                                           projectionViews[0].pose.position.z};
                PublishXrProjectionPose(true, projectionOriginValid_, origin, centre, declared);
            }
            // W2 -- DECLARE THE FRUSTUM THE SHEAR ACTUALLY PRODUCED, clamped to
            // the lens, and hand over the matching sub-rectangle.
            //
            // The shear translates the rendered frustum down the lens axis
            // without changing its span, so the extents below are the symmetric
            // pair the game rendered, shifted by the NDC offset the upload hook
            // REPORTS HAVING APPLIED. Read back, never recomputed: recomputing it
            // here would declare the shear we meant to apply rather than the one
            // the pixels were drawn with, and the two differ on every frame
            // between the toggle flipping and the next camera upload.
            //
            // With the shear off this is exactly the old symmetric declaration,
            // because the offset is then exactly zero.
            const float shearNdcY = LensShearAppliedNdcY();
            const tf2vr::TanExtents rendered =
                tf2vr::ShiftedExtents(halfTanX, halfTanY, 0.0f, shearNdcY);
            tf2vr::TanExtents lens;
            lens.left = std::tan(renderedView_[eye].fov.angleLeft);
            lens.right = std::tan(renderedView_[eye].fov.angleRight);
            lens.up = std::tan(renderedView_[eye].fov.angleUp);
            lens.down = std::tan(renderedView_[eye].fov.angleDown);
            // The lens where we have pixels for it, the render where we do not.
            // Declaring an angle we did not draw is what puts content where it
            // is not; declaring less than we drew only wastes what we drew.
            const tf2vr::TanExtents window = tf2vr::ClampToRendered(lens, rendered);
            projectionViews[eye].fov.angleLeft = std::atan(window.left);
            projectionViews[eye].fov.angleRight = std::atan(window.right);
            projectionViews[eye].fov.angleUp = std::atan(window.up);
            projectionViews[eye].fov.angleDown = std::atan(window.down);
            projectionViews[eye].subImage.swapchain = eyeSwapchain_[eye];
            projectionViews[eye].subImage.imageArrayIndex = 0;
            // SUBMIT THE RECTANGLE THE WORLD WAS RENDERED INTO, not the whole
            // buffer -- because the FOV above describes that rectangle.
            //
            // At a 16:9 target the two are the same and nothing changes. At a
            // taller one they are not: measured at 4032x3648, the game renders
            // the world into 4032x2520 at y=0 (aspect 1.600) and its frustum
            // follows to 100.0 x 73.4 deg, whose tangent ratio is 1.596. Handing
            // the compositor the full 1.105-aspect buffer while telling it the
            // image spans that frustum stretches 2520 rows of content over 3648
            // rows of declared angle. That is the vertical squash, the over-long
            // gun and the warping under head movement, all one cause.
            //
            // imageRect is exactly the OpenXR mechanism for this: it says which
            // part of the swapchain image the layer occupies.
            projectionViews[eye].subImage.imageRect.offset = {0, 0};
            projectionViews[eye].subImage.imageRect.extent = extent;
            float vpX = 0.0f, vpY = 0.0f, vpW = 0.0f, vpH = 0.0f;
            if (GetMainSceneViewport(vpX, vpY, vpW, vpH) &&
                vpW >= 1.0f && vpH >= 1.0f &&
                vpX + vpW <= static_cast<float>(gameDesc_.BufferDesc.Width) + 0.5f &&
                vpY + vpH <= static_cast<float>(gameDesc_.BufferDesc.Height) + 0.5f) {
                projectionViews[eye].subImage.imageRect.offset = {
                    static_cast<std::int32_t>(vpX), static_cast<std::int32_t>(vpY)};
                projectionViews[eye].subImage.imageRect.extent = {
                    static_cast<std::int32_t>(vpW), static_cast<std::int32_t>(vpH)};
            }
            // COMPOSE WITH THE LETTERBOX RECT, DO NOT REPLACE IT.
            //
            // Two separate rect adjustments, and both have to survive. The one
            // above selects the world pass out of the buffer; this one selects
            // the part of THAT which spans the declared frustum. Feeding the
            // window the rect just computed is what composes them -- an earlier
            // sketch of this wrote a fresh rect from the buffer and would have
            // silently undone the letterbox fix on any headset where the two
            // differ.
            tf2vr::ImageRect base;
            base.x = static_cast<float>(projectionViews[eye].subImage.imageRect.offset.x);
            base.y = static_cast<float>(projectionViews[eye].subImage.imageRect.offset.y);
            base.w = static_cast<float>(projectionViews[eye].subImage.imageRect.extent.width);
            base.h = static_cast<float>(projectionViews[eye].subImage.imageRect.extent.height);
            const tf2vr::ImageRect sub = tf2vr::WindowToRect(base, rendered, window);
            // Rounded INWARD, so the declared angle is never wider than the
            // pixels behind it. A rect rounded outward claims a row of pixels
            // that is not in the window, which is the same class of error as
            // declaring an angle we did not render, just a pixel of it.
            const std::int32_t subX = static_cast<std::int32_t>(std::ceil(sub.x));
            const std::int32_t subY = static_cast<std::int32_t>(std::ceil(sub.y));
            const std::int32_t subW = static_cast<std::int32_t>(std::floor(sub.x + sub.w)) - subX;
            const std::int32_t subH = static_cast<std::int32_t>(std::floor(sub.y + sub.h)) - subY;
            // A degenerate rect is refused rather than submitted. The runtime
            // would reject it and the layer would drop for the frame, which
            // reads as a black flash; keeping the previous rect is strictly
            // better than that and cannot happen with sane geometry anyway.
            if (subW > 0 && subH > 0) {
                projectionViews[eye].subImage.imageRect.offset = {subX, subY};
                projectionViews[eye].subImage.imageRect.extent = {subW, subH};
            }
            // IS THE RECTANGLE WE ARE ABOUT TO SUBMIT PLAUSIBLY THE WORLD PASS?
            //
            // READ-ONLY. Decides nothing, gates nothing, changes no pixel.
            //
            // The compositor scales whatever imageRect names up to the declared
            // FOV. Submit a small rectangle and a small rectangle is what fills
            // the wearer's whole view, magnified -- which is the reported
            // symptom, in those words: "a tiny square of the cutscene zoomed to
            // fill my entire view."
            //
            // imageRect starts as the world viewport, and that is captured on the
            // ASSUMPTION at camera_update_hook.cpp:4226 -- "the game sets its
            // viewport and then uploads the camera for it, so whatever
            // RSSetViewports last saw IS this frustum's rectangle." g_lastViewport
            // is overwritten by EVERY RSSetViewports call: shadow cascades, post,
            // UI, video. In gameplay the assumption holds. Nothing enforces it,
            // and a cutscene does not run gameplay's pass order.
            //
            // The only check on the result today is >= 1x1 and inside the buffer,
            // so a 512x512 bloom or shadow rectangle passes and is submitted as
            // the world.
            //
            // THE HONEST ANSWER IS ALREADY IN THE TREE. camera_update_hook.cpp:3036
            // identifies the world pass without a camera upload at all, by tally:
            // "the world rect 2281 sets against 359 for the next one, and 120 each
            // for the shadow cascades. It is not close." DominantGameViewport()
            // exports it. Comparing the two costs nothing and says whether the
            // pairing assumption is what breaks.
            if (eye == 0) {
                static std::int32_t lastW = -1, lastH = -1;
                const std::int32_t rw = projectionViews[0].subImage.imageRect.extent.width;
                const std::int32_t rh = projectionViews[0].subImage.imageRect.extent.height;
                // A change of more than two pixels. The lens window rounds inward
                // from floats, and at 5222x3264 the height flipped 2868/2869 every
                // few milliseconds -- 599 lines in one run saying nothing.
                if (std::abs(rw - lastW) > 2 || std::abs(rh - lastH) > 2) {
                    lastW = rw; lastH = rh;
                    float domW = 0.0f, domH = 0.0f;
                    unsigned long long domCount = 0, domTotal = 0;
                    const bool haveDom = DominantGameViewport(&domW, &domH, &domCount, &domTotal);
                    // "Much smaller than the rectangle the game sets most often"
                    // is the whole test. Half in either axis is far outside any
                    // legitimate lens crop, which trims the edges of the world
                    // pass and never halves it.
                    const bool tiny = haveDom && domW > 0.5f && domH > 0.5f &&
                                      (static_cast<float>(rw) < domW * 0.5f ||
                                       static_cast<float>(rh) < domH * 0.5f);
                    char line[520]{};
                    std::snprintf(line, sizeof(line),
                        "[TF2VR] %s submitted imageRect %dx%d at %d,%d; buffer %ux%u; the rectangle "
                        "the game sets most often is %.0fx%.0f (%llu of %llu sets)%s\n",
                        tiny ? "SUBMIT-RECT TINY:" : "submit-rect changed:",
                        rw, rh,
                        projectionViews[0].subImage.imageRect.offset.x,
                        projectionViews[0].subImage.imageRect.offset.y,
                        gameDesc_.BufferDesc.Width, gameDesc_.BufferDesc.Height,
                        static_cast<double>(domW), static_cast<double>(domH),
                        domCount, domTotal,
                        tiny ? " -- THE COMPOSITOR WILL MAGNIFY THIS TO FILL THE EYE."
                             : ".");
                    Log(line);
                }
            }
            // PUBLISHED HERE, where every field is final. An earlier placement
            // sat above the fov assignment and would have reported zeroes --
            // the absence of a reading dressed as one, for the third time
            // today. Left eye only: the two differ by eye separation alone.
            if (eye == 0) {
                PublishXrDeclaredView(
                    projectionViews[0].fov.angleUp * 57.2957795f,
                    projectionViews[0].fov.angleDown * 57.2957795f,
                    projectionViews[0].fov.angleLeft * 57.2957795f,
                    projectionViews[0].fov.angleRight * 57.2957795f,
                    projectionViews[0].subImage.imageRect.offset.x,
                    projectionViews[0].subImage.imageRect.offset.y,
                    projectionViews[0].subImage.imageRect.extent.width,
                    projectionViews[0].subImage.imageRect.extent.height);
            }
            // V2 -- THE FALSIFIER, AS AN ASSERTION IN CODE, AND IT LOGS EITHER
            // WAY. "A falsifier that only runs when someone remembers to look is
            // not a falsifier."
            //
            // The PFD MR reports perfectly symmetric optics, so a correct
            // implementation CANNOT alter it: the shear must be exactly zero,
            // the sub-rect must equal the rect that was already there, and the
            // declared FOV must equal the symmetric pair every previous build
            // submitted. Checked on eye 0 only and reported once, because this
            // is a property of the geometry and not of the frame.
            //
            // ITS CLAIM IS NARROWER THAN IT LOOKS, and that is recorded here
            // rather than discovered later: the PFD MR reports systemName "Meta
            // Quest 3" with a round 4032x3648 whose aspect (1.105) is nothing
            // like its FOV shape (1.428), so it is almost certainly a Virtual
            // Desktop fallback profile. A symmetric input must produce an
            // identity transform whatever it describes -- but "unchanged on the
            // PFD MR" then means unchanged on a FALLBACK PROFILE, not on that
            // headset's real lenses.
            if (eye == 0 && !falsifierReported_) {
                falsifierReported_ = true;
                const bool symmetric = tf2vr::IsSymmetric(lens, 0.0001f);
                const bool rectHeld = subW <= 0 || subH <= 0 ||
                                      (subX == static_cast<std::int32_t>(base.x) &&
                                       subY == static_cast<std::int32_t>(base.y) &&
                                       subW == static_cast<std::int32_t>(base.w) &&
                                       subH == static_cast<std::int32_t>(base.h));
                const bool fovHeld =
                    std::fabs(window.up - halfTanY) < 0.0001f &&
                    std::fabs(window.down + halfTanY) < 0.0001f &&
                    std::fabs(window.left + halfTanX) < 0.0001f &&
                    std::fabs(window.right - halfTanX) < 0.0001f;
                const bool shearHeld = shearNdcY == 0.0f;
                const bool pass = !symmetric || (rectHeld && fovHeld && shearHeld);
                char line[720]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] V2 FALSIFIER %s: optics are %s (tan U%.5f D%.5f L%.5f R%.5f). shear=%.5f "
                    "rect %.0fx%.0f at (%.0f,%.0f) -> %dx%d at (%d,%d) | declared U%.2f D%.2f L%.2f "
                    "R%.2f deg vs previous builds' symmetric U%.2f D%.2f L%.2f R%.2f. A SYMMETRIC "
                    "HEADSET MUST BE UNTOUCHED -- shear exactly 0, rect held, FOV held. Note the claim "
                    "is narrower than it looks if this is the PFD MR: its recommended rect aspect and "
                    "its FOV shape disagree by 29%%, so it is probably a Virtual Desktop fallback "
                    "profile, and 'unchanged' then means unchanged on that profile.\n",
                    pass ? "PASS" : "FAILED",
                    symmetric ? "SYMMETRIC (this is the test)" : "asymmetric (test not applicable)",
                    lens.up, lens.down, lens.left, lens.right, shearNdcY,
                    base.w, base.h, base.x, base.y, subW, subH, subX, subY,
                    std::atan(window.up) * 57.2957795f, std::atan(window.down) * 57.2957795f,
                    std::atan(window.left) * 57.2957795f, std::atan(window.right) * 57.2957795f,
                    std::atan(halfTanY) * 57.2957795f, -std::atan(halfTanY) * 57.2957795f,
                    -std::atan(halfTanX) * 57.2957795f, std::atan(halfTanX) * 57.2957795f);
                Log(line);
            }
        }
        projection.space = localSpace_;
        projection.viewCount = 2;
        projection.views = projectionViews;
    }

    const XrCompositionLayerBaseHeader* quadLayers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quads[0]),
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quads[1])};
    const XrCompositionLayerBaseHeader* projectionLayers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO}; end.displayTime = frameState.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    // Mono submits its quad whenever the swapchain HAS an image, not only on
    // frames that produced a fresh one.
    //
    // Submitting zero layers does not mean "leave the last frame up" -- it means
    // "show nothing", and the compositor obliges with a black frame. Any
    // intermittent cause of a missed copy (shouldRender false for a frame, a
    // failed acquire, the size guard tripping) therefore became a visible flash
    // rather than a repeated frame, while a perfectly good previous image sat
    // unused in the swapchain.
    //
    // This is where the flicker survives every camera-side explanation: it
    // reproduces with a synthetic, provably exact pose, and it needs OpenXR
    // armed, which is the one thing the clean flat runs never had.
    // Resubmitting the previous image is only valid while the runtime is
    // actually displaying. When shouldRender is false it is not, and it expects
    // NO layers; handing it layers anyway makes xrEndFrame block -- measured at
    // 138 ms average against a healthy 4.8 ms xrWaitFrame and a 0.03 ms image
    // round trip, which is the entire stall that arming OpenXR introduced.
    //
    // So the resubmission is gated on shouldRender. That keeps the property it
    // was added for -- a frame that merely failed to produce a fresh copy shows
    // the previous one instead of black -- without submitting into a runtime
    // that has told us it is not rendering.
    if (readyToSubmit && !useStereo) monoHasImage_ = true;
    const std::uint32_t monoLayers = (monoHasImage_ && frameState.shouldRender) ? 1u : 0u;
    // Only a frame that HAD an image and failed to place it is a miss worth
    // reporting. Decoupled, most frames deliberately carry no source -- the
    // pacing thread is re-showing the last game frame at the headset's rate,
    // which is the feature -- and counting those as failures would fill the log
    // with a fault line describing correct behaviour.
    if (!readyToSubmit && source) {
        const std::uint64_t missed = ++framesWithoutFreshImage_;
        // Reported sparsely: this is expected occasionally and pathological
        // continuously, and the rate is what distinguishes the two.
        if (missed == 1 || missed % 120 == 0) {
            char line[240]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] %llu frames have had no fresh image (shouldRender=%d). The previous image is "
                "resubmitted rather than showing nothing.\n",
                static_cast<unsigned long long>(missed), frameState.shouldRender ? 1 : 0);
            Log(line);
        }
    }
    // Every path is gated on shouldRender. A runtime that has told us it is not
    // rendering wants no layers at all, and submitting them anyway is what made
    // xrEndFrame block for 138 ms a frame.
    // THE CONFIG PANEL'S QUAD, COMPOSED ON TOP OF WHATEVER THE GAME SUBMITTED.
    //
    // Built into a local array with the game layer(s) FIRST and the panel LAST,
    // because composition layer order is draw order -- the panel has to be over
    // the world, not under it.
    //
    // It is prepared even when the game contributes nothing, so the panel stays
    // visible through a loading screen or a frame with no fresh image. But it is
    // still gated on mayShowLayers: a runtime that has told us it is not
    // rendering wants no layers at all, and handing it one anyway is what made
    // xrEndFrame block for 138 ms a frame.
    XrCompositionLayerQuad menuQuad{};
    const bool menuLayerReady =
        mayShowLayers && PrepareMenuLayer(menuQuad, frameState.predictedDisplayTime);
    const XrCompositionLayerBaseHeader* composed[4]{};
    std::uint32_t composedCount = 0;
    if (!mayShowLayers) {
        end.layerCount = 0;
        end.layers = nullptr;
    } else {
        if (useProjection) {
            composed[composedCount++] = projectionLayers[0];
        } else {
            const std::uint32_t gameLayers = stereoReady ? 2u : monoLayers;
            for (std::uint32_t i = 0; i < gameLayers; ++i) composed[composedCount++] = quadLayers[i];
        }
        if (menuLayerReady) {
            composed[composedCount++] =
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menuQuad);
        }
        end.layerCount = composedCount;
        end.layers = composedCount ? composed : nullptr;
    }
    // DIAGNOSTIC: force zero layers while still calling xrEndFrame.
    //
    // The sweep settled the camera hook's part in this: at body level 0 the
    // hook forwards immediately and does nothing, and xrEndFrame still measures
    // 87-89 ms. The stall is inside the submission, and our own contribution to
    // it (the image copy) is 0.01 ms.
    //
    // Submitting nothing separates the two remaining mechanisms:
    //   still ~87 ms -> not composition of our images at all. The runtime is
    //                   blocking on frame sync, and the suspect is that it
    //                   shares the GAME'S D3D11 device, so its work serialises
    //                   against a game rendering flat out.
    //   drops to ~0  -> compositing our layers is genuinely what costs, and the
    //                   fix belongs in what we submit and at what size.
    //
    // This precedent is why the test is worth doing rather than reasoning
    // about: submitting layers when shouldRender was false ALSO presented as a
    // blocking xrEndFrame, at 138 ms, and no amount of argument found it.
    if (forceZeroLayers_) {
        end.layerCount = 0;
        end.layers = nullptr;
    }
    submitLayersTotal_ += end.layerCount;
    if (frameState.shouldRender) ++submitShouldRender_;
    XrBreadcrumb::SetStage(XrBreadcrumb::Stage::EndFrame);
    // THE RUNTIME CALL ALONE. The t2..t3 span the spike line has been
    // calling "xrEndFrame" is nothing of the sort: it covers the projection
    // view maths, PrepareMenuLayer, the layer assembly and several Log()
    // calls, and only ENDS with the runtime. A 101 ms reading there could be
    // entirely ours, and on the strength of it I nearly told the wearer
    // their streaming stack was at fault.
    LARGE_INTEGER tCall0{}, tCall1{};
    QueryPerformanceCounter(&tCall0);
    const bool ended = XrOk(xrEndFrame(session_, &end), "end frame");
    QueryPerformanceCounter(&tCall1);
    const double callScale = tickFrequency.QuadPart
        ? 1000.0 / static_cast<double>(tickFrequency.QuadPart) : 0.0;
    lastPrepMs_ = static_cast<double>(tCall0.QuadPart - t2.QuadPart) * callScale;
    lastCallMs_ = static_cast<double>(tCall1.QuadPart - tCall0.QuadPart) * callScale;
    XrBreadcrumb::SetStage(XrBreadcrumb::Stage::Idle);
    // Stamp the frame, whoever produced it. The pacing thread gates on this, so
    // the two threads together can never hand the runtime frames faster than it
    // asked for them.
    {
        LARGE_INTEGER frameNow{};
        QueryPerformanceCounter(&frameNow);
        lastXrFrameQpc_.store(frameNow.QuadPart, std::memory_order_release);
        if (frameState.predictedDisplayPeriod > 0) {
            displayPeriodMs_.store(
                static_cast<double>(frameState.predictedDisplayPeriod) / 1.0e6,
                std::memory_order_release);
        }
    }
    QueryPerformanceCounter(&t3);
    if (tickFrequency.QuadPart) {
        const double scale = 1000.0 / static_cast<double>(tickFrequency.QuadPart);
        submitWaitMs_ += static_cast<double>(t1.QuadPart - t0.QuadPart) * scale;
        submitImageMs_ += static_cast<double>(t2.QuadPart - t1.QuadPart) * scale;
        submitEndMs_ += static_cast<double>(t3.QuadPart - t2.QuadPart) * scale;
        // What the runtime SAYS it wants, in the same units as what we measure.
        // predictedDisplayPeriod is nanoseconds.
        submitPredictedPeriodMs_ += static_cast<double>(frameState.predictedDisplayPeriod) / 1.0e6;
        // PER-FRAME SPIKE CAPTURE.
        //
        // The stall is intermittent: it dominated several sessions and then did
        // not occur at all across a full run with identical code. Sixty-frame
        // averages can only say a window was bad, long after the fact, which is
        // how a bisect ended up attributing run-to-run variance to commits.
        //
        // Three candidate mechanisms remain and none is ranked yet: the runtime
        // compositing on the GAME'S D3D11 device and serialising behind its
        // queue (ours), the encoder saturating at this per-eye resolution (not
        // ours), and something specific to the layer we submit (ours). All three
        // need the same thing first -- a stall caught in the act, with the
        // conditions recorded at the moment it happens rather than averaged.
        //
        // Deliberately always on and self-limiting: the next occurrence should
        // document itself without anyone having to arm anything first.
        const double endMs = static_cast<double>(t3.QuadPart - t2.QuadPart) * scale;
        const double waitMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * scale;
        if (endMs >= kEndFrameSpikeMs && spikesLogged_ < kMaxSpikeLines) {
            ++spikesLogged_;
            char line[560]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] xrEndFrame SPIKE %.1f ms (xrWaitFrame %.2f, image %.2f) on frame %llu: "
                "layers=%u shouldRender=%d stereo=%d projection=%d session=%s | WAIT+END %.1f ms "
                "(%.1f Hz) against a declared period of %.2f ms (%.1f Hz). A high END next to a low "
                "WAIT is the pacing wait landing in END, not a stall; read the SUM against the "
                "declared period.%s\n",
                endMs, waitMs, static_cast<double>(t2.QuadPart - t1.QuadPart) * scale,
                static_cast<unsigned long long>(submitFrameIndex_), end.layerCount,
                frameState.shouldRender ? 1 : 0, stereoReady ? 1 : 0, useProjection ? 1 : 0,
                SessionStateName(static_cast<XrSessionState>(sessionState_)),
                endMs + waitMs, (endMs + waitMs) > 0.0 ? 1000.0 / (endMs + waitMs) : 0.0,
                static_cast<double>(frameState.predictedDisplayPeriod) / 1.0e6,
                frameState.predictedDisplayPeriod > 0
                    ? 1.0e9 / static_cast<double>(frameState.predictedDisplayPeriod) : 0.0,
                spikesLogged_ == kMaxSpikeLines ? " (further spikes not logged)" : "");
            Log(line);
        }
        // ---- THE FRAME-INTERVAL SPIKE WINDOW, always on and quiet-proof -----
        {
            const double declaredMs = frameState.predictedDisplayPeriod > 0
                ? static_cast<double>(frameState.predictedDisplayPeriod) / 1.0e6 : 11.11;
            if (lastSubmitQpc_ != 0) {
                const double intervalMs =
                    static_cast<double>(t3.QuadPart - lastSubmitQpc_) * scale;
                ++spikeWindowFrames_;
                // A frame is "over" when it misses the runtime's own declared
                // period by a quarter. At 90 Hz that is 13.9 ms, which catches
                // the 4-5 ms overrun the wearer describes; the old 25 ms
                // endFrame gate could not see it at all.
                if (intervalMs > declaredMs * 1.25) {
                    ++spikeWindowOver_;
                    ++spikeRunFrames_;
                    spikeRunMs_ += intervalMs;
                    spikeLostMs_ += intervalMs - declaredMs;
                    if (spikeRunMs_ > spikeWorstRunMs_) {
                        spikeWorstRunMs_ = spikeRunMs_;
                        spikeWorstRunFrames_ = spikeRunFrames_;
                    }
                } else {
                    spikeRunFrames_ = 0;
                    spikeRunMs_ = 0.0;
                }
                // A HALF-SECOND GAP IS NOT JITTER, and it must not wait for the
                // window to end before it is reported. If the submit loop
                // itself stopped, this is where it shows; if it did NOT stop
                // and the wearer still saw a freeze, the display froze while we
                // kept submitting, which is a different fault entirely.
                if (intervalMs > 500.0) {
                    char gap[360]{};
                    std::snprintf(gap, sizeof(gap),
                        "[TF2VR] SUBMIT GAP: %.0f ms between submitted frames -- the submit loop "
                        "itself stopped. xrWaitFrame %.2f, our prep %.2f, the xrEndFrame call "
                        "%.2f. Whichever of those three holds the time is the one that stalled.\n",
                        intervalMs, waitMs, lastPrepMs_, lastCallMs_);
                    Tf2VrLogAlways(gap);
                }
                if (intervalMs > spikeWindowWorstMs_) {
                    spikeWindowWorstMs_ = intervalMs;
                    spikeWorstWaitMs_ = waitMs;
                    // THESE TWO WERE MISSING and the columns read 0.00 in all
                    // fourteen windows of the 2026-09-11 run. A silent replace
                    // in a patch script never wired them, so the fields sat at
                    // their initialiser and the instrument reported a number it
                    // had never measured. Three zeros across every window is
                    // what gave it away: an instrument that reads exactly zero
                    // everywhere is broken, not lucky.
                    spikeWorstPrepMs_ = lastPrepMs_;
                    spikeWorstCallMs_ = lastCallMs_;
                }
                spikeWindowPeriodMs_ = declaredMs;
                // Every frame goes in the ring, so the wearer's marker can look
                // backwards at what actually happened rather than forwards.
                const unsigned w = ringWrite_.load(std::memory_order_relaxed);
                const int slot = static_cast<int>(w % kFrameRing);
                ringInterval_[slot] = static_cast<float>(intervalMs);
                ringWait_[slot] = static_cast<float>(waitMs);
                ringPrep_[slot] = static_cast<float>(lastPrepMs_);
                ringCall_[slot] = static_cast<float>(lastCallMs_);
                ringWrite_.store(w + 1, std::memory_order_release);
            }
            lastSubmitQpc_ = t3.QuadPart;
            const unsigned long long now = GetTickCount64();
            if (spikeWindowStart_ == 0) spikeWindowStart_ = now;
            if (now - spikeWindowStart_ >= 10000 && spikeWindowFrames_ > 0) {
                // 900, not 520: the format literal is 661 chars and was cut at
                // 519 -- taking the trailing newline with it, so every one of
                // these ran into whatever logged next, and the run and lost
                // figures this line exists for fell off the end entirely.
                char line[900]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] FRAME SPIKES over %u frames in %llu ms: %u missed the declared period "
                    "of %.2f ms by more than a quarter (%.1f%%), worst frame %.1f ms "
                    "(xrWaitFrame %.2f | OUR submit prep %.2f | the xrEndFrame CALL %.2f). Those last "
                    "two split what used to be one number: the prep is ours, the call is the "
                    "runtime, and only one of them can be blamed for a stall. Worst is "
                    "reported, not the mean. Worst unbroken RUN of bad frames %u frames over %.0f ms, "
                    "and %.0f ms of this window was spent over budget in total -- a pause the "
                    "wearer can feel is a RUN, which no single worst-frame number can show. This "
                    "line prints either way, so silence never means the instrument stopped.\n",
                    spikeWindowFrames_, now - spikeWindowStart_, spikeWindowOver_,
                    spikeWindowPeriodMs_,
                    100.0 * static_cast<double>(spikeWindowOver_) /
                        static_cast<double>(spikeWindowFrames_),
                    spikeWindowWorstMs_, spikeWorstWaitMs_, spikeWorstPrepMs_,
                    spikeWorstCallMs_, spikeWorstRunFrames_, spikeWorstRunMs_, spikeLostMs_);
                Tf2VrLogAlways(line);
                spikeWindowStart_ = now;
                spikeWindowFrames_ = 0;
                spikeWindowOver_ = 0;
                spikeWindowWorstMs_ = 0.0;
                spikeWorstWaitMs_ = 0.0;
                spikeWorstCallMs_ = 0.0;
                spikeWorstPrepMs_ = 0.0;
                spikeWorstRunFrames_ = 0;
                spikeWorstRunMs_ = 0.0;
                spikeLostMs_ = 0.0;
                spikeRunFrames_ = 0;
                spikeRunMs_ = 0.0;
            }
        }
        ++submitFrameIndex_;
        // Sixty, not 240: this window has to report while the game is STALLED, and at
        // 12 fps a 240-frame window needs twenty seconds -- longer than the last
        // test ran, which is why it printed nothing at all.
        if (++submitSamples_ >= 60) {
            const double avgWait = submitWaitMs_ / submitSamples_;
            const double avgEnd = submitEndMs_ / submitSamples_;
            const double avgPaced = avgWait + avgEnd;
            const double avgDeclared = submitPredictedPeriodMs_ / submitSamples_;
            char line[820]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] XR submit breakdown [%s] over %u frames (avg ms) at hook body level %d, "
                "%llu camera uploads so far: xrWaitFrame %.2f | acquire+copy+release %.2f | "
                "xrEndFrame %.2f | layers %.2f/frame, shouldRender on %u of %u, session %s.\n"
                "[TF2VR]   PACING: xrWaitFrame+xrEndFrame = %.2f ms (%.1f Hz); the runtime's own "
                "predictedDisplayPeriod = %.2f ms (%.1f Hz). These two calls are one budget, not two "
                "costs -- the runtime parks us in whichever one the frame's phase lands in, so END "
                "alone means nothing and only the SUM is a rate. If the sum matches the declared "
                "period, the runtime is pacing us on purpose and the question is why it chose that "
                "rate; if the sum is far above the declared period, it is missing its own target.\n",
                // WHICH THREAD THIS IS. Decoupled, these numbers are the PACING
                // thread's and the game thread's XR cost is separately ~0; read
                // coupled numbers against decoupled ones without this label and
                // the comparison is meaningless.
                decoupleActive_.load(std::memory_order_acquire) ? "PACING THREAD, decoupled"
                                                                : "game thread, coupled",
                submitSamples_, HookBodyLevel(),
                static_cast<unsigned long long>(HookCameraSizedUploads()),
                avgWait, submitImageMs_ / submitSamples_, avgEnd,
                static_cast<double>(submitLayersTotal_) / submitSamples_,
                submitShouldRender_, submitSamples_,
                SessionStateName(static_cast<XrSessionState>(sessionState_)),
                avgPaced, avgPaced > 0.0 ? 1000.0 / avgPaced : 0.0,
                avgDeclared, avgDeclared > 0.0 ? 1000.0 / avgDeclared : 0.0);
            Log(line);
            submitPredictedPeriodMs_ = 0.0;
            submitLayersTotal_ = 0;
            submitShouldRender_ = 0;
            // Attribute each window to exactly one level: advance only after the
            // window it measured has been reported.
            AdvanceHookBodyLevelSweep();
            submitWaitMs_ = submitImageMs_ = submitEndMs_ = 0.0;
            submitSamples_ = 0;
        }
    }
    return ended;
}

// ---------------------------------------------------------------------------
// F3 -- the decoupled frame loop.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// F3 -- the gap-filling pacing thread. xr.decouple, DEFAULT 0.
//
// THIRD DESIGN, and the two that failed are why this one is shaped as it is.
//
//  1. The pacing thread owned the frame loop and copied from a ring. Two
//     threads on the immediate context meant enabling ID3D11Multithread, which
//     swaps that context's vtable; slot 48 stopped being the UpdateSubresource
//     the camera hook's byte matcher knows, so stereo silently never armed and
//     a whole headset run rendered mono while looking perfectly healthy.
//  2. The producer did acquire/copy/release on the game thread while the pacing
//     thread ran the frame loop. An unbounded image wait froze the game outright
//     on the menu; a polled wait then left an image acquired across frame
//     boundaries, the runtime blocked 79.5 ms inside xrEndFrame waiting on it,
//     and the headset went black.
//
// What both had in common: they REPLACED the working frame loop with a new one.
// This one does not. The game's Present still runs the entire frame, image work
// included, exactly as it always has. The pacing thread only fills GAPS: when
// the game has not presented for longer than a display period, it takes the
// same lock and submits one extra frame with a FRESH pose and no image work.
//
// The consequence that matters: while the game is keeping up this thread does
// NOTHING AT ALL, so normal play runs the shipped path untouched and this change
// cannot regress it. It can only act during the stalls it exists for -- which is
// the loading screen, which is the whole point.
//
// No swapchain image is ever acquired by one thread and released by another, or
// held across a frame boundary, because only Present ever touches an image at
// all. That is what makes the black screen structurally impossible rather than
// unlikely.
// ---------------------------------------------------------------------------

// Our own D3D11 device, on the adapter the OpenXR runtime demands. See the
// note in the header for why the pacing thread cannot borrow the game's.
//
// DEAD ON PURPOSE -- NO CALLERS, AND THAT IS NOT A REASON TO DELETE IT.
// Verified callerless 2026-08-24. It is kept because it carries the reasoning
// four failed frame-loop designs paid for; see the failed-designs table in
// KNOWN-GOOD-2026-08-23.md. Deleting it destroys the record, not just the code.
// If a future design needs its own device, this is the design note for it.
bool XrContext::CreateXrDevice(const LUID& adapterLuid, D3D_FEATURE_LEVEL minFeatureLevel) {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        Log("[TF2VR] decouple: could not create a DXGI factory to find the runtime's adapter.\n");
        return false;
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf()) !=
                         DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        if (std::memcmp(&desc.AdapterLuid, &adapterLuid, sizeof(LUID)) == 0) break;
        adapter.Reset();
    }
    if (!adapter) {
        Log("[TF2VR] decouple: no adapter matches the LUID the OpenXR runtime requires.\n");
        return false;
    }
    // The same adapter the runtime named, so the shared texture never crosses a
    // GPU boundary. No debug layer: this device is in the wearer's hot path.
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    Microsoft::WRL::ComPtr<ID3D11Device> created;
    const HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                         levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                         created.GetAddressOf(), nullptr, nullptr);
    if (FAILED(hr) || !created) {
        char line[200]{};
        std::snprintf(line, sizeof(line),
                      "[TF2VR] decouple: D3D11CreateDevice for our own XR device failed (0x%08lX).\n",
                      static_cast<unsigned long>(hr));
        Log(line);
        return false;
    }
    if (created->GetFeatureLevel() < minFeatureLevel) {
        Log("[TF2VR] decouple: our XR device does not meet the runtime's minimum feature level.\n");
        return false;
    }
    device_ = created;
    Log("[TF2VR] decouple: created our OWN D3D11 device on the runtime's adapter. The XR session "
        "will be built on it, so the pacing thread never touches the game's immediate context and "
        "ID3D11Multithread is never needed -- which is what broke the camera hook the first time.\n");
    return true;
}

// The one texture both devices can see.
// TWO textures both devices can see. See the header for why one was not enough.
bool XrContext::EnsureSharedFrame(const D3D11_TEXTURE2D_DESC& backbufferDesc) {
    if (sharedValid_ && sharedWidth_ == backbufferDesc.Width &&
        sharedHeight_ == backbufferDesc.Height && sharedFormat_ == backbufferDesc.Format) {
        return true;
    }
    ReleaseSharedFrame();
    if (!gameDevice_ || !device_) return false;
    if (backbufferDesc.SampleDesc.Count != 1) {
        Log("[TF2VR] decouple: the backbuffer is multisampled and cannot be shared as-is. Staying "
            "on the coupled path.\n");
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = backbufferDesc;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = 0;
    // NTHANDLE + KEYEDMUTEX is the only combination that gives both a handle we
    // can open on the other device and a synchronisation primitive to hand
    // ownership across with.
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    Microsoft::WRL::ComPtr<ID3D11Device1> xrDevice1;
    if (FAILED(device_.As(&xrDevice1))) {
        Log("[TF2VR] decouple: our XR device does not expose ID3D11Device1.\n");
        return false;
    }
    for (int index = 0; index < kSharedCount; ++index) {
        if (FAILED(gameDevice_->CreateTexture2D(&desc, nullptr,
                                                gameShared_[index].ReleaseAndGetAddressOf()))) {
            char line[220]{};
            std::snprintf(line, sizeof(line),
                          "[TF2VR] decouple: could not create shared texture %d at %ux%u format %u.\n",
                          index, desc.Width, desc.Height, static_cast<unsigned>(desc.Format));
            Log(line);
            ReleaseSharedFrame();
            return false;
        }
        Microsoft::WRL::ComPtr<IDXGIResource1> resource;
        if (FAILED(gameShared_[index].As(&resource)) ||
            FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ |
                                                DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                                &sharedHandle_[index])) ||
            FAILED(xrDevice1->OpenSharedResource1(sharedHandle_[index],
                                                  IID_PPV_ARGS(&xrShared_[index]))) ||
            FAILED(gameShared_[index].As(&gameSharedMutex_[index])) ||
            FAILED(xrShared_[index].As(&xrSharedMutex_[index]))) {
            Log("[TF2VR] decouple: could not share a frame texture between the two devices.\n");
            ReleaseSharedFrame();
            return false;
        }
    }
    sharedWidth_ = desc.Width;
    sharedHeight_ = desc.Height;
    sharedFormat_ = desc.Format;
    for (int index = 0; index < kSharedCount; ++index) {
        sharedEye_[index].store(-1, std::memory_order_release);
        sharedGen_[index].store(0, std::memory_order_release);
    }
    sharedNewest_.store(-1, std::memory_order_release);
    sharedWrite_ = 0;
    sharedValid_ = true;
    char line[240]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR] decouple: %d shared frame textures ready, %ux%u format %u, keyed mutex on "
                  "both devices. Two, so the game never waits for the pacing thread to hand one back.\n",
                  kSharedCount, desc.Width, desc.Height, static_cast<unsigned>(desc.Format));
    Log(line);
    return true;
}

void XrContext::ReleaseSharedFrame() {
    sharedValid_ = false;
    for (int index = 0; index < kSharedCount; ++index) {
        gameSharedMutex_[index].Reset();
        xrSharedMutex_[index].Reset();
        xrShared_[index].Reset();
        gameShared_[index].Reset();
        if (sharedHandle_[index]) { CloseHandle(sharedHandle_[index]); sharedHandle_[index] = nullptr; }
    }
    for (int index = 0; index < kSharedCount; ++index) {
        sharedEye_[index].store(-1, std::memory_order_release);
        sharedGen_[index].store(0, std::memory_order_release);
    }
    sharedNewest_.store(-1, std::memory_order_release);
    sharedWrite_ = 0;
    sharedWidth_ = sharedHeight_ = 0;
    sharedFormat_ = DXGI_FORMAT_UNKNOWN;
}

// RETIRED, and left as a stub rather than deleted so the call site reads
// honestly: Present no longer publishes anything, it runs the whole frame.
// Returning false always means the caller takes the coupled path, which is the
// point. The shared-texture machinery above goes unused with it.
bool XrContext::PublishBackbuffer(IDXGISwapChain*, int) { return false; }

bool XrContext::PublishBackbufferUnused(IDXGISwapChain* gameSwapchain, int eye) {
    if (!enabled_ || !decoupleWanted_) return false;
    if (!initialized_ && !Initialize(gameSwapchain)) { Shutdown(); return false; }
    // Initialize() clears decoupleWanted_ if our own device could not be made,
    // so re-check: without it this would publish into a shared texture that the
    // coupled path is not reading.
    if (!decoupleWanted_) return false;

    // A resize destroys the XR swapchains, which the pacing thread references
    // every frame, so it is stopped first and restarted below.
    DXGI_SWAP_CHAIN_DESC chain{};
    if (FAILED(gameSwapchain->GetDesc(&chain))) return true;
    const bool needsRebuild = chain.BufferDesc.Width != gameDesc_.BufferDesc.Width ||
                              chain.BufferDesc.Height != gameDesc_.BufferDesc.Height ||
                              chain.BufferDesc.Format != gameDesc_.BufferDesc.Format ||
                              !sharedValid_;
    if (needsRebuild && decoupleActive_.load(std::memory_order_acquire)) StopPacingThread();

    Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
    if (FAILED(gameSwapchain->GetBuffer(0, IID_PPV_ARGS(&source))) || !source) return true;
    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    if (needsRebuild) {
        // Do NOT assign gameDesc_ here. HandleBackbufferResize compares against
        // it and sets it itself; writing it first makes that comparison always
        // match, so a genuine resize would silently skip rebuilding the XR
        // swapchains and the headset would hold its last good frame forever.
        if (!HandleBackbufferResize(gameSwapchain)) return false;
        if (!EnsureSharedFrame(sourceDesc)) { decoupleWanted_ = false; return false; }
    }

    // NON-BLOCKING, BOTH SIDES. A zero timeout means the worst case is that this
    // frame is not published and the pacing thread re-shows the previous one --
    // never that the game thread waits. The whole class of freeze this feature
    // produced twice came from a blocking wait on this thread.
    // THE EYE CHOICE BELONGS TO THE CONSUMER, NOT HERE.
    //
    // Gating publication on "the other eye" was right about the problem and
    // wrong about where to solve it. It made the producer skip frames waiting
    // for the eye it wanted, which caps publishing at every other game frame --
    // 72/sec at best, 49/sec measured -- and left each eye at about 24 Hz
    // against the coupled path's 40.
    //
    // So publish EVERYTHING and record which eye each slot holds. With two
    // slots the consumer usually has both eyes in hand and can take the one it
    // needs, which gets each eye to the full alternating rate instead of to
    // whatever the producer happened to guess.

    // Write whichever buffer is free. With two of them the consumer can be
    // holding one for its whole 12.5 ms frame and the game still never waits.
    // TAKE A CONSUMED SLOT, OR RECLAIM AN UNCONSUMED ONE.
    //
    // A keyed mutex leaves a published slot standing at the consumer's key until
    // the consumer actually reads it. Trying only kSharedKeyGame therefore meant
    // that once both slots were published-but-unread the producer could write
    // NEITHER, and was throttled to the one slot the consumer freed per frame --
    // measured at 29 publishes per 120 submitted frames, with the eye counts
    // drifting apart because the consumer kept choosing the same slot and the
    // other stayed locked. That is a double buffer behaving worse than a single
    // one.
    //
    // So the producer also reclaims with kSharedKeyXr: the key it released with,
    // which is legal for anyone to take, and which means "the consumer never got
    // to this frame, so overwrite it". Dropping an unconsumed frame is exactly
    // right -- it is older than the one being written.
    int slot = -1;
    for (int attempt = 0; attempt < kSharedCount && slot < 0; ++attempt) {
        const int candidate = (sharedWrite_ + attempt) % kSharedCount;
        if (!gameSharedMutex_[candidate]) continue;
        if (gameSharedMutex_[candidate]->AcquireSync(kSharedKeyGame, 0) == S_OK ||
            gameSharedMutex_[candidate]->AcquireSync(kSharedKeyXr, 0) == S_OK) {
            slot = candidate;
        }
    }
    if (slot < 0) return true;   // the consumer holds both right now
    sharedWrite_ = (slot + 1) % kSharedCount;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    gameDevice_->GetImmediateContext(&context);
    context->CopyResource(gameShared_[slot].Get(), source.Get());
    // NO Flush() HERE, and that omission is the fix for a measured regression.
    //
    // A Flush on the game's immediate context every frame kicks the GPU and
    // breaks the driver's pipelining, and it cost the game HALF ITS FRAME RATE:
    // 120 XR frames against 59 game frames in a window, i.e. ~40 fps where the
    // coupled path held 80. With alternate-frame stereo that puts each eye at
    // 20 Hz, which is what the wearer felt as "SUPER jerky".
    //
    // It is not needed. ReleaseSync and AcquireSync on a keyed mutex are
    // context-ordered operations and the driver guarantees the consuming device
    // sees the producer's writes -- that is the entire purpose of a keyed mutex
    // over a plain shared surface.
    gameSharedMutex_[slot]->ReleaseSync(kSharedKeyXr);
    // Publish the slot BEFORE the generation, so a consumer that sees the new
    // generation is guaranteed to read the slot that goes with it.
    // The pose this frame belongs to, captured now, while the game thread still
    // owns the slot. The pacing thread's locate at consume time would be up to a
    // whole display period adrift from when the game actually rendered this.
    {
        std::scoped_lock lock(viewMutex_);
        sharedViewValid_[slot] = locatedViewsValid_;
        if (locatedViewsValid_) {
            sharedView_[slot][0] = locatedView_[0];
            sharedView_[slot][1] = locatedView_[1];
        }
    }
    sharedEye_[slot].store(eye, std::memory_order_release);
    sharedGen_[slot].store(publishedFrames_.load(std::memory_order_acquire) + 1,
                           std::memory_order_release);
    sharedNewest_.store(slot, std::memory_order_release);

    lastPublishedEye_.store(eye, std::memory_order_release);
    if (eye >= 0 && eye <= 1) publishedPerEye_[eye].fetch_add(1, std::memory_order_acq_rel);
    publishedFrames_.fetch_add(1, std::memory_order_acq_rel);
    MaybeStartPacingThread();

    // HOLD THE GAME TO THE DISPLAY RATE, and this is what the head-motion
    // stutter comes down to.
    //
    // g_headYawDegrees -- the orientation the game renders the world with -- is
    // written only by UpdateLookInjection, which runs on the pacing thread at
    // the display rate. Let the game run free at 144 fps and its head
    // orientation still only changes every ~1.8 frames, so the frames we then
    // sample carry orientations that advance in uneven steps: one displayed
    // frame repeats the last orientation, the next jumps two. That is stutter,
    // and no correction to the reprojection pose can fix it because the SOURCE
    // is uneven.
    //
    // Coupled, xrWaitFrame held the game to the display rate, so every game
    // frame had a fresh orientation and every one was shown. This restores that
    // relationship without an XR call on this thread: a plain deadline sleep,
    // bounded by one display period, that cannot wait on anything external.
    //
    // The stall case is untouched -- a game blocked in a level load is not
    // reaching this line at all, and the pacing thread keeps submitting.
    const double period = displayPeriodMs_.load(std::memory_order_acquire);
    LARGE_INTEGER freq{}, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (period > 1.0 && freq.QuadPart && lastPublishQpc_.load(std::memory_order_acquire)) {
        // A HIGH-RESOLUTION TIMER, because Sleep() is not accurate enough to
        // pace a headset and the difference is visible.
        //
        // Coupled, xrWaitFrame held the game to 12.40 / 12.50 / 12.59 ms -- a
        // spread of 0.19 ms. This loop with Sleep(1) produced 11.00 / 11.70 /
        // 13.01, a spread of 2.0 ms, because Windows' default timer granularity
        // is 1-15 ms. Ten times the frame-time jitter, and uneven frame times
        // ARE judder however correct every pose is.
        //
        // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION gives sub-millisecond waits
        // without touching the global timer resolution with timeBeginPeriod,
        // which would affect the whole process. The wait is a plain deadline
        // with a timeout, so it cannot park on anything that never arrives.
        if (!paceTimer_) {
            paceTimer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                TIMER_ALL_ACCESS);
            if (!paceTimer_) {
                paceTimer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
                Log("[TF2VR] decouple: no high-resolution waitable timer; frame pacing will be "
                    "coarser and head movement may read as slightly uneven.\n");
            }
        }
        QueryPerformanceCounter(&now);
        const double elapsed = static_cast<double>(now.QuadPart -
                                                   lastPublishQpc_.load(std::memory_order_acquire)) *
                               1000.0 / static_cast<double>(freq.QuadPart);
        const double remainMs = period - elapsed;
        if (paceTimer_ && remainMs > 0.05 && remainMs < 100.0) {
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(remainMs * 10000.0);   // 100 ns units, relative
            if (SetWaitableTimer(paceTimer_, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(paceTimer_, static_cast<DWORD>(remainMs) + 2);
            }
        }
    }
    QueryPerformanceCounter(&now);
    lastPublishQpc_.store(now.QuadPart, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------
// F3 -- the pacing thread OWNS the frame loop. xr.decouple, DEFAULT 0.
//
// It makes every xrWaitFrame, xrBeginFrame, xrEndFrame, and every swapchain
// image call. Present makes NONE while this is running; it only copies the
// backbuffer into the shared texture. That division is not a preference, it is
// what the runtime measured out as a requirement -- see the header note.
//
// The pacing thread submits at the display rate whether or not the game has
// produced anything, so a game frame that never arrives simply leaves the last
// picture up, reprojected against a fresh pose. That is the smoothing.
// ---------------------------------------------------------------------------

void XrContext::SetDecoupled(bool enabled) {
    decoupleWanted_ = enabled;
    Log(enabled
        ? "[TF2VR] xr.decouple = 1: a pacing thread will own the XR frame loop on its own D3D11 "
          "device, and Present becomes a producer that only copies the backbuffer into a shared "
          "texture. This makes a slow frame SMOOTH; it does not make the level load SHORTER.\n"
        : "[TF2VR] xr.decouple = 0: one XR frame per game frame, submitted from inside Present. This "
          "is the shipped path.\n");
    if (!enabled) StopPacingThread();
}

// Milliseconds since the last XR frame from EITHER thread. This, not the time
// since the last Present, is what the pacing thread gates on: the runtime does
// not care who produced a frame, only how many it is handed, and gating on
// Present let the combined loop overrun the display rate.
double XrContext::MsSinceLastXrFrame() const {
    const long long last = lastXrFrameQpc_.load(std::memory_order_acquire);
    if (!last) return 1.0e9;   // nothing submitted yet; do not hold the pacer back
    LARGE_INTEGER frequency{}, now{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    if (!frequency.QuadPart) return 0.0;
    return static_cast<double>(now.QuadPart - last) * 1000.0 /
           static_cast<double>(frequency.QuadPart);
}

// Milliseconds since the GAME last presented. Distinct from MsSinceLastXrFrame,
// which the pacer's own submissions advance -- so it can never distinguish a
// game that is slow from one that has stopped, which is the whole question.
double XrContext::MsSinceLastGamePresent() const {
    const long long last = lastGamePresentQpc_.load(std::memory_order_acquire);
    if (!last) return 1.0e9;   // nothing presented yet: the menu has not started
    LARGE_INTEGER frequency{}, now{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    if (!frequency.QuadPart) return 0.0;
    return static_cast<double>(now.QuadPart - last) * 1000.0 /
           static_cast<double>(frequency.QuadPart);
}

void XrContext::MaybeStartPacingThread() {
    if (!decoupleWanted_ || decoupleActive_.load(std::memory_order_acquire)) return;
    if (!initialized_ || !sessionRunning_) return;
    pacingRun_.store(true, std::memory_order_release);
    decoupleActive_.store(true, std::memory_order_release);
    pacingThread_ = std::thread(&XrContext::PacingThreadMain, this);
    Log("[TF2VR] decouple: gap-filling pacing thread STARTED. Present keeps the whole frame loop for "
        "itself -- image work included -- so gameplay runs the shipped path untouched and stays on "
        "the runtime's own clock. This thread submits ONLY when the game has produced nothing for a "
        "display period, which is a stall, which is the loading screen. FALSIFIERS: the filled count "
        "must stay near zero in normal play and climb only during a stall; the game's frame time must "
        "stay near the runtime's declared period; and the camera hook must still report 'prologue "
        "variant B accepted'.\n");
}

void XrContext::StopPacingThread() {
    if (!pacingThread_.joinable()) { decoupleActive_.store(false, std::memory_order_release); return; }
    pacingRun_.store(false, std::memory_order_release);
    // Safe to join without frameMutex_: the pacing thread only ever TRY-locks it.
    pacingThread_.join();
    decoupleActive_.store(false, std::memory_order_release);
    Log("[TF2VR] decouple: pacing thread stopped; the frame loop is Present's alone again.\n");
}

// FILL GAPS. NOTHING ELSE.
//
// Every lesson from the attempts that failed is a line in here:
//
//  * no D3D at all, and no swapchain image is ever touched -- the compositor
//    re-shows the last image Present released, reprojected against a pose
//    located in THIS frame. That is what makes a stalled loading screen smooth,
//    and it is why no second device is needed.
//  * gate on the last XR FRAME from either thread, not on the last Present.
//    The runtime does not care who produced a frame, only how many it is
//    handed; gating on Present let the combined loop reach 106 Hz against a
//    declared 80, at which point the runtime answered shouldRender=false and
//    the zero-layer frames that produces read as the loading text being drawn,
//    wiped and drawn again.
//  * Present announces itself before it blocks and this thread refuses to
//    compete while that is set. Without it the game lost the lock race
//    indefinitely -- 120 filled frames per 2 game frames, 1.5 s stalls.
//  * Sleep(1), never Sleep(0). Sleep(0) yields only to an equal-priority thread
//    on the same core, so it spins, and spinning here starved the game.
//  * xrSyncActions and the head-pose injection happen on REAL game frames only,
//    which is why SubmitFrameCore is called with doImageWork false here.
void XrContext::PacingThreadMain() {
    unsigned long long lastReport = 0;
    // G1's instrument, and it has to live on THIS thread. The question the run
    // asks -- is the pacer covering for a stall, or starving Present -- is only
    // answerable while the game thread is the thing that has stopped, and a
    // heartbeat driven by the stalled thread cannot report the stall.
    //
    // Time-based, not fill-based: the existing report fires every 120 FILLED
    // frames, so a healthy run (zero fills, which is what healthy means here)
    // prints nothing at all, and a run that merely halved its framerate prints
    // once a minute. Both of those read identically to "the feature is off".
    LARGE_INTEGER freq{}, lastBeat{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&lastBeat);
    unsigned long long beatFilled = pacingSubmitted_.load(std::memory_order_acquire);
    unsigned long long beatPresented = publishedFrames_.load(std::memory_order_acquire);
    while (pacingRun_.load(std::memory_order_acquire)) {
        if (freq.QuadPart) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            const double sinceBeat =
                static_cast<double>(now.QuadPart - lastBeat.QuadPart) / static_cast<double>(freq.QuadPart);
            if (sinceBeat >= 2.0) {
                const unsigned long long filledNow = pacingSubmitted_.load(std::memory_order_acquire);
                const unsigned long long presentedNow = publishedFrames_.load(std::memory_order_acquire);
                const double filledRate = static_cast<double>(filledNow - beatFilled) / sinceBeat;
                const double presentedRate = static_cast<double>(presentedNow - beatPresented) / sinceBeat;
                const double period = displayPeriodMs_.load(std::memory_order_acquire);
                char beat[620]{};
                std::snprintf(beat, sizeof(beat),
                    "[TF2VR] DECOUPLE beat: filled %.1f/s + presented %.1f/s = %.1f/s against a "
                    "declared %.1f Hz | stood down %llu times this window (%.0f ms threshold) | "
                    "totals filled=%llu presented=%llu | stereo=%d projection=%d. "
                    "presented near the display rate and filled near zero is the game keeping up; "
                    "presented LOW and filled making up the difference is the pacer covering a "
                    "stall, which is what it is for; the SUM running over the declared rate is the "
                    "pacer competing with Present rather than covering for it.\n",
                    filledRate, presentedRate, filledRate + presentedRate,
                    period > 0.0 ? 1000.0 / period : 0.0,
                    pacingStoodDown_.exchange(0, std::memory_order_relaxed),
                    standDownPeriods_ * period,
                    filledNow, presentedNow,
                    stereoEnabled_ ? 1 : 0, projectionLayerEnabled_ ? 1 : 0);
                Log(beat);
                LogResourceHealth(presentedRate < 1.0 ? "GAME SILENT" : "loading or running");
                LogMultithreadStatus();
                lastBeat = now;
                beatFilled = filledNow;
                beatPresented = presentedNow;
            }
        }
        if (!enabled_ || !initialized_ || !sessionRunning_) { Sleep(2); continue; }
        // Has a frame -- from either thread -- gone out within a display period?
        // If so the game is keeping up and there is nothing to cover for.
        if (MsSinceLastXrFrame() < displayPeriodMs_.load(std::memory_order_acquire)) {
            Sleep(1);
            continue;
        }
        // STAND DOWN WHILE THE GAME IS MERELY SLOW.
        //
        // Measured, 2026-08-23 run 2, the run that ended in DEVICE_REMOVED with
        // the loading screen never drawn:
        //
        //   filled  1.5/s + presented 75.0/s   menu, healthy
        //   filled 65.5/s + presented 13.5/s   load starts
        //   filled 69.5/s + presented  9.5/s
        //   filled 61.0/s + presented 18.5/s
        //   filled 76.0/s + presented  0.0/s   and never again
        //
        // The middle three are the failure. The game was PRESENTING -- slow,
        // not stopped -- and the pacer was taking four fifths of a budget the
        // runtime caps at 80/s, with the sum pinned at 79.0-79.5 the whole
        // time. There is no headroom in a gate that targets exactly the display
        // rate: every frame the pacer takes is one the game has to contend for.
        // The runtime then answered shouldRender=false on 60 of 60 frames, and
        // zero-layer frames are why the "Press A to continue" prompt was never
        // seen at all.
        //
        // So the gate becomes: fill only once the GAME has produced nothing for
        // several display periods. A stalled loading screen still gets the full
        // rate, because during a real stall this is always true. A slow one
        // keeps its own frames, because the pacer now waits out four periods
        // before deciding anything is missing.
        //
        // Deliberately NOT a duty-cycle cap or a servo. The known-good record
        // has four dead frame-loop designs in it and every one of them lost to
        // the same thing: a second clock. This adds no clock, it only widens
        // the silence the existing gate waits for.
        const double sinceGamePresent = MsSinceLastGamePresent();
        if (sinceGamePresent < standDownPeriods_ * displayPeriodMs_.load(std::memory_order_acquire)) {
            pacingStoodDown_.fetch_add(1, std::memory_order_relaxed);
            Sleep(1);
            continue;
        }
        if (presentWaiting_.load(std::memory_order_acquire)) { Sleep(1); continue; }
        {
            std::unique_lock<std::mutex> lock(frameMutex_, std::try_to_lock);
            if (!lock.owns_lock()) { Sleep(1); continue; }
            if (presentWaiting_.load(std::memory_order_acquire)) continue;
            if (MsSinceLastXrFrame() < displayPeriodMs_.load(std::memory_order_acquire)) continue;
            if (!enabled_ || !sessionRunning_) continue;
            if (!PollEvents()) continue;
            // Marked so the create hooks on the game's loading threads can
            // count overlaps. This is the whole XR call, which is the window in
            // which the runtime may touch the game's device.
            NotifyPacerEnterXr();
            SubmitFrameCore(nullptr, lastPublishedEye_.load(std::memory_order_acquire), false);
            NotifyPacerLeaveXr();
        }
        const unsigned long long filled =
            pacingSubmitted_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (filled - lastReport >= 120) {
            lastReport = filled;
            char line[560]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] DECOUPLE [stereo=%d projection=%d]: %llu gap frames filled this session "
                "against %llu game frames presented. These are frames the wearer saw that the game "
                "did not produce, each reprojected against a fresh pose. Climbing during a load and "
                "flat in normal play is this working; climbing in normal play means it is competing "
                "with Present rather than covering for it. stereo=0 means the session is MONO however "
                "good the pacing looks.\n",
                stereoEnabled_ ? 1 : 0, projectionLayerEnabled_ ? 1 : 0, filled,
                publishedFrames_.load(std::memory_order_acquire));
            Log(line);
        }
    }
}

void XrContext::DestroySwapchain() {
    images_.clear();
    locatedViewsValid_ = false;
    viewLogCountdown_ = 0;
    for (int eye = 0; eye < 2; ++eye) {
        eyeImages_[eye].clear();
        eyeHasImage_[eye] = false;
        renderedViewValid_[eye] = false;
        if (eyeSwapchain_[eye] != XR_NULL_HANDLE) { XrBreadcrumb::NoteSwapchainChanged(false); xrDestroySwapchain(eyeSwapchain_[eye]); eyeSwapchain_[eye] = XR_NULL_HANDLE; }
    }
    if (colorSwapchain_ != XR_NULL_HANDLE) { XrBreadcrumb::NoteSwapchainChanged(false); xrDestroySwapchain(colorSwapchain_); colorSwapchain_ = XR_NULL_HANDLE; }
}
void XrContext::Shutdown() {
    // The pacing thread calls into the session on every iteration, so it must
    // be stopped and JOINED before anything it touches is destroyed. Tearing
    // the session down under a live frame loop is a use-after-free with a
    // headset attached.
    StopPacingThread();
    enabled_ = false; sessionRunning_ = false; initialized_ = false; haveLookBaseline_ = false;
    // A restarted session is a new session as far as a report is concerned:
    // re-emit the banner rather than leave the second half of a log with no
    // configuration attached to it.
    headsetBannerLogged_ = false;
    // Before the session and instance that own the action spaces and action set.
    XrInputShutdown();
    DestroySwapchain();
    if (viewSpace_ != XR_NULL_HANDLE) { xrDestroySpace(viewSpace_); viewSpace_ = XR_NULL_HANDLE; }
    if (localSpace_ != XR_NULL_HANDLE) { xrDestroySpace(localSpace_); localSpace_ = XR_NULL_HANDLE; }
    if (session_ != XR_NULL_HANDLE) { xrDestroySession(session_); session_ = XR_NULL_HANDLE; }
    if (instance_ != XR_NULL_HANDLE) { xrDestroyInstance(instance_); instance_ = XR_NULL_HANDLE; }
    ReleaseSharedFrame();
    if (paceTimer_) { CloseHandle(paceTimer_); paceTimer_ = nullptr; }
    device_.Reset(); gameDevice_.Reset(); systemId_ = XR_NULL_SYSTEM_ID;
}

// xr.headlock_quad. Defined out here rather than as a member: the quad path has
// no per-session state and this must survive a session restart.
void SetHeadlockQuad(bool enabled) {
    g_headlockQuad = enabled;
    Tf2VrLog(enabled
        ? "[TF2VR] menu/loading quad is HEAD-LOCKED (view space): it rides the head, so head "
          "motion shows none of the loading screen's half-rate judder.\n"
        : "[TF2VR] menu/loading quad is WORLD-LOCKED (local space), as it was before.\n");
}

// ---------------------------------------------------------------------------
// THE CONFIG PANEL'S QUAD LAYER. PLAN-VRMENU M6.
//
// The panel used to be drawn into the game's backbuffer, which meant it was
// baked into the eye image and rode the view: turning your head took it with
// you. The wearer's words, and the whole reason this exists: "I really want this
// to be fixed in front of me and not pinned to my head."
//
// So the panel is drawn into its own private texture (menu_overlay owns it),
// copied here into its own XrSwapchain, and shown on a quad posed in LOCAL
// space. Local space does not follow the head, so the panel stays exactly where
// it was when it was summoned and the wearer can look around it, lean in, or
// step back from it.
//
// It is a SEPARATE swapchain from the game image on purpose. Sharing one would
// mean the panel's update rate, format and size were tied to the game's, and the
// panel wants none of those things -- it is 1024x768 and only changes when a
// widget moves.
// ---------------------------------------------------------------------------

bool XrContext::EnsureMenuSwapchain(ID3D11Texture2D* source) {
    if (menuSwapchain_ != XR_NULL_HANDLE) return true;
    if (session_ == XR_NULL_HANDLE || !source) return false;

    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    if (!sourceDesc.Width || !sourceDesc.Height) return false;

    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    // The SOURCE's format, not a preferred one. CopyResource across mismatched
    // formats or sizes is a silent no-op -- no HRESULT, no exception, just a
    // panel that never updates -- and this project has already lost a run to
    // exactly that on the game image.
    createInfo.format = static_cast<std::int64_t>(sourceDesc.Format);
    createInfo.sampleCount = 1;
    createInfo.width = sourceDesc.Width;
    createInfo.height = sourceDesc.Height;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;
    if (!XrOk(xrCreateSwapchain(session_, &createInfo, &menuSwapchain_), "create menu swapchain")) {
        menuSwapchain_ = XR_NULL_HANDLE;
        return false;
    }
    std::uint32_t imageCount = 0;
    xrEnumerateSwapchainImages(menuSwapchain_, 0, &imageCount, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> xrImages(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (!XrOk(xrEnumerateSwapchainImages(menuSwapchain_, imageCount, &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(xrImages.data())),
            "read menu D3D11 images")) {
        return false;
    }
    menuImages_.clear();
    menuImages_.reserve(imageCount);
    for (const auto& image : xrImages) menuImages_.emplace_back(image.texture);
    menuAspect_ = static_cast<float>(sourceDesc.Width) / static_cast<float>(sourceDesc.Height);

    char line[260]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] menu quad: swapchain created, %u images of %ux%u, aspect %.3f, format %u.\n",
        imageCount, sourceDesc.Width, sourceDesc.Height,
        static_cast<double>(menuAspect_), static_cast<unsigned>(sourceDesc.Format));
    Log(line);
    return true;
}

// WHERE THE PANEL IS PUT, captured ONCE per summon.
//
// Taken from where the wearer is looking at the moment they open it, projected
// forward, and then YAW ONLY: a panel that inherited the head's pitch and roll
// would hang at whatever angle they happened to be looking, which reads as
// broken rather than as deliberate. Level and facing them is what "a screen in
// front of me" means.
//
// Height is the head's own height, so it appears at eye level wherever they are
// standing rather than at some fixed altitude the room knows nothing about.
void XrContext::CaptureMenuPose(XrTime displayTime) {
    menuPose_ = XrPosef{};
    menuPose_.orientation.w = 1.0f;
    menuPose_.position = {0.0f, 0.0f, -g_menuDistanceM};

    if (viewSpace_ == XR_NULL_HANDLE || localSpace_ == XR_NULL_HANDLE) return;
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if (!XrOk(xrLocateSpace(viewSpace_, localSpace_, displayTime, &location),
              "locate the head for the menu quad")) {
        return;
    }
    const bool haveOrientation =
        (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
    const bool havePosition =
        (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
    if (!haveOrientation || !havePosition) {
        // Untracked at the moment of summon. Leave the default in front of the
        // local origin rather than placing it somewhere arbitrary, and say so --
        // a panel in the wrong place with no explanation is the sort of thing
        // that gets blamed on the quad rather than on tracking.
        Log("[TF2VR] menu quad: the head pose was not valid when the panel was summoned, so it is "
            "placed at the local origin instead of in front of the wearer.\n");
        return;
    }

    const XrQuaternionf& q = location.pose.orientation;
    // Forward is -Z in OpenXR. Rotate it by the head orientation, then keep only
    // the horizontal component -- that is the yaw, without the arithmetic going
    // through Euler angles and their singularities.
    const float fx = -2.0f * (q.x * q.z + q.w * q.y);
    const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const float length = std::sqrt(fx * fx + fz * fz);
    float dirX = 0.0f;
    float dirZ = -1.0f;
    if (length > 1e-4f) {
        dirX = fx / length;
        dirZ = fz / length;
    }
    // Looking straight up or down makes the horizontal projection degenerate;
    // the guard above keeps the default forward rather than producing a NaN
    // orientation that the compositor would reject.
    // THE QUAD MUST FACE BACK ALONG THE VIEW DIRECTION, NOT ALONG IT.
    //
    // A quad with identity orientation faces +Z in its own space -- which is why
    // the game's own quads sit at z = -2 with an identity pose and are visible
    // to a viewer looking down -Z. So the panel's +Z has to point FROM the panel
    // TOWARDS the wearer, i.e. along -dir, not along dir.
    //
    // A yaw of theta about Y maps +Z to (sin theta, cos theta). Setting that
    // equal to (-dirX, -dirZ) gives theta = atan2(-dirX, -dirZ).
    //
    // The previous version used atan2(dirX, -dirZ). Straight ahead both give 0,
    // which is why it looked almost right and was reported as "angled slightly
    // pointing to my left" rather than as obviously broken -- the error is twice
    // the summoning yaw offset, so it only shows when the panel is summoned
    // off-axis, and it grows to a full 180 degrees when summoned facing sideways.
    const float yaw = std::atan2(-dirX, -dirZ);
    menuPose_.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    menuPose_.position = {location.pose.position.x + dirX * g_menuDistanceM,
                          location.pose.position.y,
                          location.pose.position.z + dirZ * g_menuDistanceM};

    char line[260]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] menu quad: placed at (%.2f, %.2f, %.2f) facing yaw %.1f deg, %.2f m away, %.2f m "
        "wide. World-fixed: it stays here while the head moves.\n",
        static_cast<double>(menuPose_.position.x), static_cast<double>(menuPose_.position.y),
        static_cast<double>(menuPose_.position.z),
        static_cast<double>(yaw * 57.2957795f), static_cast<double>(g_menuDistanceM),
        static_cast<double>(g_menuWidthM));
    Log(line);
}

// Copies the panel texture into the menu swapchain and fills in the quad.
// Returns false if there is nothing to show this frame, in which case the caller
// simply does not submit the layer.
bool XrContext::PrepareMenuLayer(XrCompositionLayerQuad& quad, XrTime displayTime) {
    ID3D11Texture2D* source = MenuLayerTexture();
    const bool visible = MenuLayerVisible() && source != nullptr;
    if (!visible) {
        menuWasVisible_ = false;
        menuPosed_ = false;
        return false;
    }
    if (!EnsureMenuSwapchain(source)) return false;

    // The pose is captured on the EDGE, not every frame. Capturing every frame
    // is precisely the head-locked behaviour this replaces.
    if (!menuWasVisible_ || !menuPosed_) {
        CaptureMenuPose(displayTime);
        menuPosed_ = true;
    }
    menuWasVisible_ = true;

    std::uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!XrOk(xrAcquireSwapchainImage(menuSwapchain_, &acquire, &imageIndex),
              "acquire menu image")) {
        return false;
    }
    XrSwapchainImageWaitInfo imageWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    imageWait.timeout = XR_INFINITE_DURATION;
    bool ok = XrOk(xrWaitSwapchainImage(menuSwapchain_, &imageWait), "wait menu image");
    if (ok && imageIndex < menuImages_.size()) {
        D3D11_TEXTURE2D_DESC sourceDesc{}, targetDesc{};
        source->GetDesc(&sourceDesc);
        menuImages_[imageIndex]->GetDesc(&targetDesc);
        // Dimensions only, for the same reason the game-image copy checks
        // dimensions only: runtimes hand back TYPELESS for a typed request, and
        // comparing formats rejected every copy on a healthy session once
        // already.
        if (sourceDesc.Width != targetDesc.Width || sourceDesc.Height != targetDesc.Height) {
            if (!menuCopyMismatchReported_) {
                menuCopyMismatchReported_ = true;
                char line[300]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] menu quad: panel texture %ux%u does not match the XR image %ux%u, so "
                    "the copy would be a silent no-op and the layer is not submitted.\n",
                    sourceDesc.Width, sourceDesc.Height, targetDesc.Width, targetDesc.Height);
                Log(line);
            }
            ok = false;
        } else {
            menuCopyMismatchReported_ = false;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
            device_->GetImmediateContext(&context);
            context->CopyResource(menuImages_[imageIndex].Get(), source);
        }
    }
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (!XrOk(xrReleaseSwapchainImage(menuSwapchain_, &release), "release menu image")) ok = false;
    if (!ok) return false;

    quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    // Blended, because the panel texture is cleared to transparent and only the
    // panel itself is opaque. Without this the quad is an opaque black slab with
    // a window drawn on it.
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    // LOCAL space, never view space. This is the whole feature.
    quad.space = localSpace_;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.pose = menuPose_;
    quad.size = {g_menuWidthM, g_menuWidthM / (menuAspect_ > 0.01f ? menuAspect_ : 1.333f)};
    quad.subImage.swapchain = menuSwapchain_;
    quad.subImage.imageArrayIndex = 0;
    quad.subImage.imageRect.offset = {0, 0};
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    quad.subImage.imageRect.extent = {static_cast<std::int32_t>(desc.Width),
                                      static_cast<std::int32_t>(desc.Height)};
    return true;
}

// menu.distance_m / menu.width_m. Live: the pose is captured per summon, so a
// change is picked up the next time the panel is opened rather than yanking a
// panel the wearer is currently reading.
void SetMenuQuadDistanceMetres(float metres) {
    if (metres < 0.3f) metres = 0.3f;
    if (metres > 6.0f) metres = 6.0f;
    g_menuDistanceM = metres;
}

void SetMenuQuadWidthMetres(float metres) {
    if (metres < 0.2f) metres = 0.2f;
    if (metres > 5.0f) metres = 5.0f;
    g_menuWidthM = metres;
}

float MenuQuadDistanceMetres() { return g_menuDistanceM; }
float MenuQuadWidthMetres() { return g_menuWidthM; }

// ---- THE WEARER'S SPIKE MARKER --------------------------------------------
//
// Pressed the moment a spike is FELT. Prints the last few seconds of frames as
// a summary plus the worst handful, each split into xrWaitFrame, our submit
// prep and the xrEndFrame call.
//
// The point is not to FIND spikes -- the window summary does that. It is to
// check the instrument against the wearer. If they mark a freeze and these
// frames are all 11 ms, the submit loop was healthy while the DISPLAY was not,
// and that is a different fault with a different owner. No counter of ours
// could reach that conclusion alone.
void XrContext::ReportRecentFrames() {
    const unsigned w = ringWrite_.load(std::memory_order_acquire);
    if (w == 0) {
        Tf2VrLogAlways("[TF2VR] SPIKE MARK: no frames recorded yet.\n");
        return;
    }
    const int have = static_cast<int>(w < static_cast<unsigned>(kFrameRing)
                                      ? w : static_cast<unsigned>(kFrameRing));
    const int look = have < 360 ? have : 360;   // about four seconds at 90 Hz
    int over = 0;
    double total = 0.0, worst = 0.0;
    int worstIdx[4] = {-1, -1, -1, -1};
    double worstVal[4] = {0, 0, 0, 0};
    for (int n = 0; n < look; ++n) {
        const int slot = static_cast<int>((w - 1 - static_cast<unsigned>(n)) % kFrameRing);
        const double v = ringInterval_[slot];
        total += v;
        if (v > worst) worst = v;
        if (v > 13.9) ++over;
        for (int k = 0; k < 4; ++k) {
            if (v > worstVal[k]) {
                for (int m = 3; m > k; --m) { worstVal[m] = worstVal[m-1]; worstIdx[m] = worstIdx[m-1]; }
                worstVal[k] = v; worstIdx[k] = slot;
                break;
            }
        }
    }
    char head[420]{};
    std::snprintf(head, sizeof(head),
        "[TF2VR] ==== SPIKE MARK: the wearer felt one HERE. Last %d frames span %.0f ms, %d of "
        "them over 13.9 ms, worst %.1f ms. If these are all near 11 ms the submit loop was fine "
        "and the DISPLAY froze without us. ====\n",
        look, total, over, worst);
    Tf2VrLogAlways(head);
    for (int k = 0; k < 4; ++k) {
        if (worstIdx[k] < 0) continue;
        char line[300]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] MARK   worst #%d: frame %.1f ms = xrWaitFrame %.2f + our prep %.2f + the "
            "xrEndFrame call %.2f (the rest is the game's own frame).\n",
            k + 1, worstVal[k], static_cast<double>(ringWait_[worstIdx[k]]),
            static_cast<double>(ringPrep_[worstIdx[k]]),
            static_cast<double>(ringCall_[worstIdx[k]]));
        Tf2VrLogAlways(line);
    }
}
