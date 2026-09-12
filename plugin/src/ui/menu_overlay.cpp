// ATTRIBUTION. The Dear ImGui overlay in this file -- device/context capture at
// Present, the ImGui_ImplDX11 + ImGui_ImplWin32 pairing, and the draw-and-render
// shape around them -- is adapted from bioshock-trilogy-vr (MIT),
// https://github.com/VR-Stereo-Hub/bioshock-trilogy-vr. The vendored Dear ImGui
// pin is the same commit that project ships. See THIRD_PARTY_NOTICES.md.

#include "rui_hunt.h"
#include "menu_overlay.h"

#include "diagnostics.h"
#include "plugin_cost.h"
#include "vr_input.h"
#include "xr_input.h"
#include "settings_registry.h"
#include "present_hook.h"
#include "render_resolution.h"
#include "camera_update_hook.h"
#include "version.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

// ImGui's own Win32 message handler. Declared here rather than relying on the
// backend header exposing it, which is how upstream's examples do it too.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace {

std::atomic_bool g_open{false};
// Set on the presenting thread only, after a successful init. Read from the
// plugin frame for the status line, so it is atomic.
std::atomic_bool g_ready{false};
std::atomic_bool g_initFailed{false};
std::atomic_uint64_t g_framesDrawn{0};
// Prompt heartbeat: a channel with no counter cannot report that it is dead.
std::uint64_t g_promptDraws = 0;
std::atomic_bool g_subclassed{false};
// Set on every open so the panel takes nav focus exactly once per summon.
bool g_needFocus = false;
// Which registry row the panel builder is on, for the fault guard's report.
int g_buildingIndex = -1;
int g_activePage = 0;
void StepMenuCategory(int delta);   // defined with the panel, used by the nav feed

// menu.ui_scale -- the wearer's own multiplier on the resolution-derived size.
//
// 1.9 after the shell change. The old 0.94 was tuned when the panel was a small
// floating window inside the texture; the window now FILLS the 2304-tall texture,
// so the same multiplier put the text at a fraction of its former size relative
// to the panel. A number tuned against one layout means nothing after the layout
// changes, which is the second time that has caught me this session.
std::atomic<float> g_uiScale{1.9f};   // unchanged; the QUAD shrinks, so text shrinks with it
float UiScale();   // defined below; used by the backbuffer remap above it.

// Accent for the active tab and the pane title. A deep blue, as asked -- Halo's
// orange was inherited along with their layout and the wearer called it hideous.
// Dark enough to read as a heading rather than a warning, and it does not
// collide with the amber used for pending changes.
constexpr ImU32 kAccentU32 = IM_COL32(64, 132, 214, 255);
constexpr ImVec4 kAccentV4 = ImVec4(0.25f, 0.52f, 0.84f, 1.0f);

Microsoft::WRL::ComPtr<ID3D11Device> g_device;
Microsoft::WRL::ComPtr<ID3D11DeviceContext> g_context;
Microsoft::WRL::ComPtr<ID3D11RenderTargetView> g_rtv;
// THE IDENTITY THE RTV IS KEYED ON. Not a bool "we made one already": a
// resolution change destroys the swapchain's buffers and a stale RTV then
// points at freed memory. Comparing the raw pointer is what makes the rebuild
// automatic rather than something someone has to remember to trigger.
ID3D11Texture2D* g_rtvBackbuffer = nullptr;

// THE BACKBUFFER'S OWN SIZE, which is NOT the window's.
//
// ImGui's win32 backend sets io.DisplaySize from GetClientRect, and its DX11
// backend then sets the D3D viewport to io.DisplaySize -- but we render into the
// BACKBUFFER, and under this project's render-resolution override those two are
// different spaces entirely (the VR profile runs 5840x3648 while the desktop
// window is monitor-sized). Left alone, the panel is laid out for one space and
// drawn into another: wrong size, wrong place, and unreadably small on a frame
// that wide. The wearer hit exactly this -- "only 1/4 appears on my monitor".
//
// So DisplaySize is driven from these, and the mouse is scaled into the same
// space below. It matters again for M6: the VR adapter renders into a private
// 1024x768 texture that has nothing to do with the game window at all.
UINT g_backbufferWidth = 0;
UINT g_backbufferHeight = 0;

// THE VR ADAPTER'S PRIVATE TARGET. PLAN-VRMENU M6 specifies 1024x768.
//
// Fixed rather than derived from the backbuffer: this texture becomes a quad of
// a fixed angular size in the world, so its pixel count is about how sharp the
// panel is, not about matching anything the game is doing.
// 3072x2304, against the plan's 1024x768. A 3.6 m quad at 1.5 m subtends about
// 100 degrees, so 3072 across it is roughly 30 pixels per degree -- at or above
// what the headset itself resolves. 28 MB, allocated once, drawn only while the
// panel is up.
//
// Resolution was only half of the blur the wearer reported. The other half was
// the font being rasterised at 9 px and magnified; see FontScaleMain below.
constexpr UINT kMenuTextureWidth = 3072;
constexpr UINT kMenuTextureHeight = 2304;
Microsoft::WRL::ComPtr<ID3D11Texture2D> g_menuTexture;
Microsoft::WRL::ComPtr<ID3D11RenderTargetView> g_menuRtv;

HWND g_window = nullptr;
WNDPROC g_originalWndProc = nullptr;

// ---------------------------------------------------------------------------
// THE D3D11 STATE BACKUP.
//
// ImGui's backend sets its own pipeline state and does not restore it. The game
// does not re-set everything per draw -- it relies on state it left behind --
// so drawing over it without this produces a corrupted GAME image on the next
// frame. That failure looks nothing like an overlay bug, which is exactly why
// the M1 falsifier names it separately.
// ---------------------------------------------------------------------------
struct D3DStateBackup {
    UINT scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    ID3D11RasterizerState* rasterizer = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4]{};
    UINT sampleMask = 0;
    UINT stencilRef = 0;
    ID3D11DepthStencilState* depthStencil = nullptr;
    ID3D11ShaderResourceView* pixelShaderResource = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11PixelShader* pixelShader = nullptr;
    ID3D11VertexShader* vertexShader = nullptr;
    ID3D11GeometryShader* geometryShader = nullptr;
    UINT pixelShaderInstanceCount = 256;
    UINT vertexShaderInstanceCount = 256;
    UINT geometryShaderInstanceCount = 256;
    ID3D11ClassInstance* pixelShaderInstances[256]{};
    ID3D11ClassInstance* vertexShaderInstances[256]{};
    ID3D11ClassInstance* geometryShaderInstances[256]{};
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer* indexBuffer = nullptr;
    ID3D11Buffer* vertexBuffer = nullptr;
    ID3D11Buffer* vertexConstantBuffer = nullptr;
    UINT indexBufferOffset = 0;
    UINT vertexBufferStride = 0;
    UINT vertexBufferOffset = 0;
    DXGI_FORMAT indexBufferFormat = DXGI_FORMAT_UNKNOWN;
    ID3D11InputLayout* inputLayout = nullptr;
    ID3D11RenderTargetView* renderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* depthStencilView = nullptr;

    void Capture(ID3D11DeviceContext* ctx) {
        ctx->RSGetScissorRects(&scissorCount, scissors);
        ctx->RSGetViewports(&viewportCount, viewports);
        ctx->RSGetState(&rasterizer);
        ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
        ctx->OMGetDepthStencilState(&depthStencil, &stencilRef);
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, renderTargets,
                                &depthStencilView);
        ctx->PSGetShaderResources(0, 1, &pixelShaderResource);
        ctx->PSGetSamplers(0, 1, &sampler);
        ctx->PSGetShader(&pixelShader, pixelShaderInstances, &pixelShaderInstanceCount);
        ctx->VSGetShader(&vertexShader, vertexShaderInstances, &vertexShaderInstanceCount);
        ctx->VSGetConstantBuffers(0, 1, &vertexConstantBuffer);
        ctx->GSGetShader(&geometryShader, geometryShaderInstances, &geometryShaderInstanceCount);
        ctx->IAGetPrimitiveTopology(&topology);
        ctx->IAGetIndexBuffer(&indexBuffer, &indexBufferFormat, &indexBufferOffset);
        ctx->IAGetVertexBuffers(0, 1, &vertexBuffer, &vertexBufferStride, &vertexBufferOffset);
        ctx->IAGetInputLayout(&inputLayout);
    }

    void Restore(ID3D11DeviceContext* ctx) {
        ctx->RSSetScissorRects(scissorCount, scissors);
        ctx->RSSetViewports(viewportCount, viewports);
        ctx->RSSetState(rasterizer);
        Release(rasterizer);
        ctx->OMSetBlendState(blend, blendFactor, sampleMask);
        Release(blend);
        ctx->OMSetDepthStencilState(depthStencil, stencilRef);
        Release(depthStencil);
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, renderTargets,
                                depthStencilView);
        for (auto*& target : renderTargets) Release(target);
        Release(depthStencilView);
        ctx->PSSetShaderResources(0, 1, &pixelShaderResource);
        Release(pixelShaderResource);
        ctx->PSSetSamplers(0, 1, &sampler);
        Release(sampler);
        ctx->PSSetShader(pixelShader, pixelShaderInstances, pixelShaderInstanceCount);
        Release(pixelShader);
        for (UINT i = 0; i < pixelShaderInstanceCount; ++i) Release(pixelShaderInstances[i]);
        ctx->VSSetShader(vertexShader, vertexShaderInstances, vertexShaderInstanceCount);
        Release(vertexShader);
        for (UINT i = 0; i < vertexShaderInstanceCount; ++i) Release(vertexShaderInstances[i]);
        ctx->VSSetConstantBuffers(0, 1, &vertexConstantBuffer);
        Release(vertexConstantBuffer);
        ctx->GSSetShader(geometryShader, geometryShaderInstances, geometryShaderInstanceCount);
        Release(geometryShader);
        for (UINT i = 0; i < geometryShaderInstanceCount; ++i) Release(geometryShaderInstances[i]);
        ctx->IASetPrimitiveTopology(topology);
        ctx->IASetIndexBuffer(indexBuffer, indexBufferFormat, indexBufferOffset);
        Release(indexBuffer);
        ctx->IASetVertexBuffers(0, 1, &vertexBuffer, &vertexBufferStride, &vertexBufferOffset);
        Release(vertexBuffer);
        ctx->IASetInputLayout(inputLayout);
        Release(inputLayout);
    }

    template <typename T>
    static void Release(T*& p) {
        if (p) {
            p->Release();
            p = nullptr;
        }
    }
};

LRESULT CALLBACK MenuWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // ONLY while the menu is open. A subclass that fed every message to ImGui
    // even when closed would be an input path the wearer cannot see, and the
    // first thing anyone would blame for a stuck key.
    if (g_open.load(std::memory_order_acquire) && ImGui::GetCurrentContext() != nullptr) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return 1;
        // Swallow mouse and keyboard from the GAME while the panel is up, so a
        // click on a slider is not also a click in the world. Movement messages
        // still reach ImGui above; this only stops the pass-through.
        switch (msg) {
            case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
            case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
                return 0;
            default:
                break;
        }
    }
    return CallWindowProcW(g_originalWndProc, hwnd, msg, wParam, lParam);
}

void ReleaseRenderTarget() {
    g_rtv.Reset();
    g_rtvBackbuffer = nullptr;
}

// Rebuilds the RTV whenever the swapchain's backbuffer POINTER changes, which
// is what a resolution change looks like from here. Returns false if there is
// nothing to draw into, in which case the frame is skipped rather than drawn
// into a dead view.
// THE VR ADAPTER'S TARGET. Created once, from the game's own device, in the
// game's own backbuffer format -- xr_context copies straight from it into an
// XrSwapchain image, and CopyResource across mismatched formats or sizes is a
// SILENT no-op, so matching the format the XR swapchain will be created with
// matters more than picking a pretty one.
bool EnsureMenuTexture(IDXGISwapChain* swapchain) {
    if (g_menuTexture && g_menuRtv) return true;
    DXGI_SWAP_CHAIN_DESC swapDesc{};
    if (FAILED(swapchain->GetDesc(&swapDesc))) return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kMenuTextureWidth;
    desc.Height = kMenuTextureHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = swapDesc.BufferDesc.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_menuTexture))) {
        Tf2VrLog("[TF2VR] menu: could not create the VR panel texture; the panel will stay in the "
                 "backbuffer and remain head-locked.\n");
        return false;
    }
    if (FAILED(g_device->CreateRenderTargetView(g_menuTexture.Get(), nullptr, &g_menuRtv))) {
        g_menuTexture.Reset();
        Tf2VrLog("[TF2VR] menu: could not create the VR panel render target view.\n");
        return false;
    }
    char line[220]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR] menu: VR panel target created, %ux%u, format %u.\n",
                  kMenuTextureWidth, kMenuTextureHeight,
                  static_cast<unsigned>(desc.Format));
    Tf2VrLog(line);
    return true;
}

bool EnsureRenderTarget(IDXGISwapChain* swapchain) {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer))) || !backbuffer) {
        ReleaseRenderTarget();
        return false;
    }
    if (g_rtv && backbuffer.Get() == g_rtvBackbuffer) return true;

    ReleaseRenderTarget();
    if (FAILED(g_device->CreateRenderTargetView(backbuffer.Get(), nullptr, &g_rtv))) {
        Tf2VrLog("[TF2VR] menu: could not create a render target view for the backbuffer; the "
                 "overlay will not draw this frame.\n");
        return false;
    }
    // Raw, and deliberately NOT owning: it is an identity token compared by
    // value, never dereferenced. Holding a reference here would keep a
    // superseded backbuffer alive across a resize.
    g_rtvBackbuffer = backbuffer.Get();
    D3D11_TEXTURE2D_DESC desc{};
    backbuffer->GetDesc(&desc);
    g_backbufferWidth = desc.Width;
    g_backbufferHeight = desc.Height;
    char line[220]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR] menu: render target (re)built for a %ux%u backbuffer.\n",
                  desc.Width, desc.Height);
    Tf2VrLog(line);
    return true;
}

bool Initialise(IDXGISwapChain* swapchain) {
    if (g_initFailed.load(std::memory_order_acquire)) return false;

    DXGI_SWAP_CHAIN_DESC swapDesc{};
    if (FAILED(swapchain->GetDesc(&swapDesc)) || !swapDesc.OutputWindow) {
        Tf2VrLog("[TF2VR] menu: the swapchain reports no output window; cannot host the overlay.\n");
        g_initFailed.store(true, std::memory_order_release);
        return false;
    }
    if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&g_device))) || !g_device) {
        Tf2VrLog("[TF2VR] menu: could not get the device from the verified swapchain.\n");
        g_initFailed.store(true, std::memory_order_release);
        return false;
    }
    g_device->GetImmediateContext(&g_context);
    if (!g_context) {
        Tf2VrLog("[TF2VR] menu: the device returned no immediate context.\n");
        g_initFailed.store(true, std::memory_order_release);
        return false;
    }
    g_window = swapDesc.OutputWindow;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // NEVER write imgui.ini. All three mods REVIEW-VRMENU read do the same, all
    // three with the same reason: this project's own INI is the only config file
    // that should appear anywhere near the game folder.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // ImGui draws its own pointer. The game hides and clips the OS cursor, so
    // relying on that cursor would make the panel look unresponsive when it is
    // in fact working perfectly -- a failure that would read as a broken build.
    // 2.7: the stick and buttons drive ImGui's own nav. The mouse is NOT used --
    // see the note above FeedNavFromControllers for why that path was dropped
    // rather than fixed.
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // NEVER TOUCH THE OS CURSOR. This is not cosmetic: vr_input gates ALL
    // gameplay input on MenuIsOpen(), which is a CURSOR-VISIBILITY test. ImGui's
    // win32 backend shows and hides the OS cursor as it sees fit, so opening this
    // panel left the cursor visible, that predicate latched true, and jump,
    // crouch and everything else stopped working -- while the panel itself kept
    // responding, because it reads the sticks directly.
    //
    // The wearer reported it as "up/down on right joystick broke ... the right
    // joystick DOES work in the config menu", which is exactly that shape.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(g_window)) {
        Tf2VrLog("[TF2VR] menu: ImGui_ImplWin32_Init failed.\n");
        ImGui::DestroyContext();
        g_initFailed.store(true, std::memory_order_release);
        return false;
    }
    if (!ImGui_ImplDX11_Init(g_device.Get(), g_context.Get())) {
        Tf2VrLog("[TF2VR] menu: ImGui_ImplDX11_Init failed.\n");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_initFailed.store(true, std::memory_order_release);
        return false;
    }

    g_originalWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(MenuWndProc)));
    g_subclassed.store(g_originalWndProc != nullptr, std::memory_order_release);
    if (!g_originalWndProc) {
        // NOT fatal. The panel still draws and can still be closed by the key,
        // which goes through the plugin's own hotkey path and not this window.
        // It is the MOUSE that is lost, and saying so is the difference between
        // a diagnosable run and "the menu did not work".
        Tf2VrLog("[TF2VR] menu: SetWindowLongPtrW did not return a previous WndProc, so the window "
                 "was NOT subclassed. The panel will draw but will NOT take mouse input.\n");
    }

    g_ready.store(true, std::memory_order_release);
    char line[300]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR] menu: overlay initialised. ImGui %s, backbuffer %ux%u, hwnd %p, window "
                  "subclassed %s.\n",
                  IMGUI_VERSION, swapDesc.BufferDesc.Width, swapDesc.BufferDesc.Height,
                  static_cast<void*>(g_window), g_originalWndProc ? "yes" : "NO");
    Tf2VrLog(line);
    return true;
}


// ---------------------------------------------------------------------------
// STICK / BUTTON NAVIGATION. PLAN-VRMENU 2.7.
//
// This is NOT a stand-in for the mouse. The mouse path was dropped outright:
// inputsystem.dll reads raw HID and pins the OS cursor to screen centre, so
// ImGui's backend reads "centre" every frame while the wearer's movement drives
// the player instead. Making that work needs a raw-input virtual cursor, and it
// would be deleted the moment the ray lands.
//
// Nav is the opposite: 2.7 specifies it as the permanent accessibility path,
// fed from the SAME published stick and button state the VR controllers already
// produce. It is what makes the panel usable before the ray exists and it stays
// afterwards.
//
// CRITICALLY, in this branch the sticks feed ImGui and NEVER SendKey (2.4.5).
// vr_input's menu branch is gated on our panel being closed for exactly that
// reason -- the game's pause menu is live underneath ours while paused.
//
// THE EDGE DETECTORS RUN WHETHER OR NOT THE PANEL IS OPEN, and are discarded
// when it is closed. That is this project's existing discipline: otherwise the
// first press after opening reads as a fresh edge from a stick that has been
// held the whole time.
// ---------------------------------------------------------------------------

// Halo's numbers, and the plan cites them: 0.25 deadzone, 0.12 per unit.
constexpr float kNavDeadzone = 0.5f;
constexpr float kWheelDeadzone = 0.25f;
constexpr float kWheelPerUnit = 0.12f;
// NO REPEAT MACHINERY HERE ON PURPOSE. ImGui's nav runs its own d-pad repeat,
// so directions are fed as LEVELS and a hand-rolled repeat would only fight it.

// Whichever hand is pushed further, so either controller drives the panel --
// the wearer should not have to remember which one the menu listens to.
float DominantAxis(int axis) {
    const float left = g_controllerThumbstick[0][axis];
    const float right = g_controllerThumbstick[1][axis];
    return (left < 0.0f ? -left : left) > (right < 0.0f ? -right : right) ? left : right;
}

void FeedNavFromControllers(bool panelOpen) {
    // WAIT FOR CENTRE AFTER OPENING. The chord is L3+R3, and a wearer whose
    // thumbs are still on the sticks would otherwise navigate the instant the
    // panel appears. Directions are ignored until both sticks have returned to
    // rest once.
    static bool armed = false;
    static bool lastPanelOpen = false;
    if (panelOpen && !lastPanelOpen) armed = false;
    lastPanelOpen = panelOpen;

    const float navX = DominantAxis(0);
    const float navY = DominantAxis(1);
    const float absX = navX < 0.0f ? -navX : navX;
    const float absY = navY < 0.0f ? -navY : navY;
    if (!armed && absX < 0.25f && absY < 0.25f) armed = true;

    if (!panelOpen) return;

    ImGuiIO& io = ImGui::GetIO();
    // WITHOUT THIS FLAG IMGUI DISCARDS EVERY GAMEPAD KEY, EVERY FRAME.
    //
    // This is the whole reason the panel could be summoned, drawn and focused
    // and still not be driven. imgui.cpp: "Clear gamepad data if disabled" --
    // if BackendFlags_HasGamepad is not set, it walks the entire gamepad key
    // range and forces every one of them to Down=false, and nav_gamepad_active
    // is false everywhere. ImGui_ImplWin32 never sets the flag because it does
    // not handle gamepads; the backend that feeds them is supposed to, and here
    // that backend is us.
    //
    // Set every frame rather than once at init, so no backend re-init can
    // silently take it away again.
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

    // LEVELS, NOT PULSES. ImGui's nav does its OWN repeat for the d-pad, so
    // feeding a held direction as a level is both simpler and better behaved
    // than synthesising edges here -- a hand-rolled repeat would fight the one
    // ImGui already runs.
    const bool up = armed && navY > kNavDeadzone;
    const bool down = armed && navY < -kNavDeadzone;
    const bool right = armed && navX > kNavDeadzone;
    const bool left = armed && navX < -kNavDeadzone;
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, up);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, down);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, left);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, right);

    const std::uint8_t rb = g_controllerButtons[1];
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, armed && (rb & kControllerPrimaryButton) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, armed && (rb & kControllerSecondaryButton) != 0);

    // THE GRIPS CHANGE CATEGORY. PLAN-VRMENU 2.7: "left/right shoulder or grips
    // -- tween between sidebar categories".
    //
    // This is why the sidebar is not focusable and the stick never has to cross
    // from one child window to another. ImGuiChildFlags_NavFlattened is supposed
    // to allow that crossing and is marked [BETA]; it did not work here, and
    // rather than fight it the plan's own answer is better anyway -- the stick
    // stays entirely inside the pane and the category is a separate gesture.
    //
    // The grips are ordnance and tactical in gameplay, but gameplay input is
    // suppressed while the panel is up, so they are free.
    {
        static bool leftGripHeld = false;
        static bool rightGripHeld = false;
        const bool leftGrip = g_controllerSqueeze[0] > 0.6f;
        const bool rightGrip = g_controllerSqueeze[1] > 0.6f;
        if (leftGrip && !leftGripHeld) StepMenuCategory(-1);
        if (rightGrip && !rightGripHeld) StepMenuCategory(1);
        leftGripHeld = leftGrip;
        rightGripHeld = rightGrip;
    }

    // Right stick Y scrolls, above its own deadzone. Halo's 0.12 per unit.
    const float wheel = g_controllerThumbstick[1][1];
    if (wheel > kWheelDeadzone || wheel < -kWheelDeadzone) {
        io.AddMouseWheelEvent(0.0f, wheel * kWheelPerUnit);
    }

    // THE READOUT THAT SETTLES IT, once a second while open.
    //
    // Three failures have looked identical from inside a headset -- the events
    // never arriving, ImGui discarding them, and ImGui accepting them with
    // nothing focusable to move to. These four numbers separate all three:
    //
    //   hasgamepad=0            ImGui is clearing the keys; nothing else matters
    //   navactive=0 with a
    //   stick clearly deflected no window has nav focus
    //   navactive=1 and the
    //   highlight still still   nav is running but the window has no focusable
    //                           item -- a layout problem, not an input one
    static std::uint64_t lastLog = 0;
    const std::uint64_t nowMs = GetTickCount64();
    if (nowMs - lastLog >= 1000) {
        lastLog = nowMs;
        char line[320]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] menu nav: hasgamepad=%d navactive=%d navvisible=%d armed=%d | stick "
            "x=%.2f y=%.2f | dpad u%d d%d l%d r%d | A=%d B=%d\n",
            (io.BackendFlags & ImGuiBackendFlags_HasGamepad) ? 1 : 0,
            io.NavActive ? 1 : 0, io.NavVisible ? 1 : 0, armed ? 1 : 0,
            static_cast<double>(navX), static_cast<double>(navY),
            up ? 1 : 0, down ? 1 : 0, left ? 1 : 0, right ? 1 : 0,
            (rb & kControllerPrimaryButton) ? 1 : 0,
            (rb & kControllerSecondaryButton) ? 1 : 0);
        Tf2VrLog(line);
    }
}
// Puts ImGui into BACKBUFFER space, immediately after the win32 backend has put
// it into WINDOW space.
//
// Two things have to move together or the panel is unusable:
//
//   DisplaySize  the DX11 backend sets the D3D viewport and the projection from
//                this, so it must be the surface we are actually drawing into.
//   MousePos     the win32 backend reports it in window client pixels. Scaled
//                by the same ratio, or the pointer lands nowhere near the widget
//                it is over -- which looks exactly like a dead panel.
//
// And the SCALE, which is not cosmetic here. A 520-pixel panel on a 5840-pixel
// frame is about nine per cent of the width; the wearer would be reading 8-pixel
// text across a headset's 110 degrees. The font is scaled by the frame height
// against a 1080p reference so the panel stays the same APPARENT size whatever
// the render resolution is.
void RemapToSurface(UINT surfaceWidth, UINT surfaceHeight) {
    if (!surfaceWidth || !surfaceHeight) return;
    ImGuiIO& io = ImGui::GetIO();
    const float windowWidth = io.DisplaySize.x;
    const float windowHeight = io.DisplaySize.y;
    const float outWidth = static_cast<float>(surfaceWidth);
    const float outHeight = static_cast<float>(surfaceHeight);

    // The mouse is scaled into the same space. It is vestigial in VR -- the
    // panel is driven by the sticks -- but a flat run still uses it, and an
    // unscaled pointer lands nowhere near the widget it is over.
    if (windowWidth > 0.0f && windowHeight > 0.0f &&
        io.MousePos.x > -FLT_MAX / 2.0f && io.MousePos.y > -FLT_MAX / 2.0f) {
        io.MousePos.x *= outWidth / windowWidth;
        io.MousePos.y *= outHeight / windowHeight;
    }
    io.DisplaySize = ImVec2(outWidth, outHeight);
    // style.FontScaleMain, NOT io.FontGlobalScale.
    //
    // 1.92 moved it, and the difference is exactly the blur the wearer reported.
    // FontGlobalScale is the legacy compat field: it scales the glyphs at DRAW
    // time, so the atlas is still rasterised at the base size and the result is
    // magnified. FontScaleMain feeds 1.92's dynamic font system, which
    // RE-RASTERISES the atlas at the requested size -- sharp at any scale.
    ImGui::GetStyle().FontScaleMain = UiScale();
}
// HOW BIG THE TEXT IS, as a multiple of ImGui's base font size.
//
// Fed to style.FontScaleMain, which in 1.92 RE-RASTERISES the atlas at the
// requested size rather than magnifying it. That is why this can be a large
// number without going soft.
//
// The derived term keeps the panel the same apparent size across surfaces of
// different pixel heights; the wearer's multiplier is the part only they can
// decide. In VR the surface is the fixed panel texture, so the derived term is
// constant and menu.ui_scale is effectively the whole control.
//
// Default 1.5. Against the 2304-tall panel texture that is FontScaleMain 3.2,
// so roughly 42-pixel text on a quad about 100 degrees wide -- the previous
// default computed to 0.71, rasterising the font at 9 px and then magnifying it,
// which is the "REALLY blurry" the wearer reported.
float UiScale() {
    // Must agree with vrTarget below, or the font is sized for the surface the
    // panel is NOT being drawn into.
    const UINT height = (IsXrPresentingToHeadset() && g_menuTexture) ? kMenuTextureHeight
                                                                    : g_backbufferHeight;
    if (!height) return 1.0f;
    float scale = static_cast<float>(height) / 1080.0f;
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 4.0f) scale = 4.0f;
    return scale * g_uiScale.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// M1's content: the Status page ONLY. The eight-category sidebar is M3, and
// building it here would mean building it against a presentation path that has
// not yet been shown to work.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// THE PANEL. Every control is a registry entry -- there are no hand-written
// widgets bound to anything, because a control with no setting behind it is a
// second store by another name.
//
// EDITS ARE STAGED, NOT APPLIED. The game is live behind this panel, so a slider
// that wrote through on every frame of a drag would be changing the world while
// the wearer was still deciding. Everything here reads StagedOrLiveValue and
// writes StageSettingValue; the commit happens once, on close.
// ---------------------------------------------------------------------------
// THE VIDEO PAGE. TWO NUMBERS.
//
// What the headset asks for, and what we are giving it. That is the whole page.
//
// Everything else that used to be here -- internal buffer, FOV overshoot,
// pixels-per-degree, cropped-away percentages -- is gone. It was all derived
// from an aspect correction I invented, using a 110x90 field of view read out of
// an OLD session logged on a DIFFERENT headset. None of it described this
// machine, and it buried the one number the wearer actually wanted under four
// they did not.
//
// The runtime is the authority on what it wants per eye. Show that, show what it
// is getting, and stop.
// ---------------------------------------------------------------------------
// THE PANEL SHELL, TAKEN FROM Halo-MCC-VR's menu.cpp RATHER THAN INVENTED.
//
// The wearer asked at the outset to look at the polished mods in dev/othermods
// for the layout. I did not, and shipped a single scrolling dump of thirty rows
// -- "a chinese menu of a hundred sliders that looks like garbage". That was the
// whole of the problem, and the fix is to copy what already works.
//
// Halo's shell, and every part of it earns its place:
//
//   THE WINDOW FILLS THE PANEL TEXTURE AND IS LOCKED. Pos and size are
//   Cond_Always, not FirstUseEver, so ImGui cannot keep a position of its own;
//   NoMove/NoResize/NoTitleBar remove every drag target. A floating box on a
//   quad has a transparent surround to slide around inside, which is what mine
//   was doing.
//
//   TWO COLUMNS. A sidebar of categories, then the pane for the selected one.
//   Both sized to -footerHeight so the footer sits below both.
//
//   ONE CATEGORY AT A TIME. This is the difference between eight short pages and
//   one endless list, and it is why every shipped mod does it.
//
//   AN ACCENT BAR down the left edge of the selected sidebar row, so the current
//   category reads at a glance from across the panel.
//
//   A TITLE AND A ONE-LINE BLURB at the top of the pane, so each page says what
//   it is for.
//
// PLAN-VRMENU 2.5 drew this exact shell in ASCII and I built something else.
// ---------------------------------------------------------------------------
// THE HOME PAGE'S READOUT. TWO NUMBERS AND A FRAME RATE.
//
// What the headset asks for, and what it is getting. The runtime is the
// authority on the first; the second is that times the one slider.
// ---------------------------------------------------------------------------
void DrawVideoReadout() {
    const unsigned recW = RuntimeRecommendedEyeWidth();
    const unsigned recH = RuntimeRecommendedEyeHeight();
    const int curW = RequestedRenderWidth();
    const int curH = RequestedRenderHeight();

    // The STAGED slider value, so the resolution moves as the wearer drags it
    // rather than only after a restart.
    float pendingScale = RenderScale();
    for (int i = 0; i < SettingCount(); ++i) {
        const Setting* s = SettingAt(i);
        if (s && std::strcmp(s->name, "render.scale") == 0) {
            pendingScale = StagedOrLiveValue(i);
            break;
        }
    }
    int wantW = 0, wantH = 0;
    PanelResolutionForScale(pendingScale, &wantW, &wantH);

    // ONE SIZE, ONCE. The wearer, on the first version of this panel: "I am
    // still not loving these odd sizes being displayed TWICE in the config menu
    // when the panel size is shown once."
    //
    // They were right, and there were three of them: the panel size, the
    // delivered size (panel x scale) and the internal buffer. Only ONE is a
    // number anybody recognises -- the one the headset reports. The other two
    // are consequences of the scale and of the lens cone, they belong in the log
    // where support can find them, and putting them on a config screen just
    // invites the question "why does that not match my headset?".
    //
    // So: name the headset, state its per-eye size once, and make the control a
    // percentage of it.
    // THE NAME IS THE RUNTIME'S CLAIM AND THE PANEL SAYS SO. VDXR reports
    // "Meta Quest 3" for a Play For Dream MR -- a headset Virtual Desktop has
    // supported for a year and identifies correctly in its own UI, including
    // its performance overlay -- so this is not a missing profile and not
    // something the plugin can detect its way around. We read
    // XrSystemProperties::systemName, which is the field OpenXR specifies for
    // it, and that is what the runtime puts there.
    //
    // So the panel attributes it rather than asserting it. "as reported by"
    // costs one word and turns "why does this say the wrong headset?" into a
    // question with a visible answer. The per-eye size below it is measured,
    // differs between headsets, and is what the resolution actually uses.
    ImGui::Text("Headset                 %s", HeadsetSystemName());
    ImGui::TextDisabled("                        as reported by %s", HeadsetRuntimeName());
    if (recW && recH) {
        ImGui::Text("Per eye                 %u x %u", recW, recH);
    }
    // THE SCALED SIZE, IN PANEL TERMS. Wearer: "just multiply the scaling factor
    // by the panel resolution. I know this isn't what is used internally but it
    // is in terms a user will understand." It sits ON the resolution line rather
    // than under it, so the page still shows one size per row and the percentage
    // and the pixels it means are read together.
    if (wantW > 0 && wantH > 0) {
        ImGui::Text("Resolution              %.0f%%   (%d x %d per eye)",
                    static_cast<double>(pendingScale) * 100.0, wantW, wantH);
    } else {
        ImGui::Text("Resolution              %.0f%%", static_cast<double>(pendingScale) * 100.0);
    }
    // Compare the SCALES, not the sizes. wantW/wantH are panel pixels now and
    // curW/curH are the internal buffer, so comparing them would never match and
    // this line would fire on a page nobody had touched.
    //
    // IT SAID "Takes effect on restart", WHICH IS FALSE. SetRenderScale drives
    // the engine's own mat_setvideomode the moment the staged value commits,
    // which is when this panel closes -- the comment above that function says so
    // in as many words, and the log line it prints says "applied LIVE". Telling
    // the wearer to restart for a change that has already happened teaches them
    // to distrust the panel.
    if (std::fabs(pendingScale - RenderScale()) > 0.005f) {
        ImGui::TextColored(kAccentV4, "Applies when you close this panel");
    }
    ImGui::Text("Frame rate              %.0f fps",
                static_cast<double>(PresentedFramesPerSecond()));
}

// ---------------------------------------------------------------------------


struct CategoryPage {
    SettingCategory category;
    const char* label;
    const char* blurb;
};

// Order is the order they appear. Home first because it is the landing page and
// the one thing reached for most.
const CategoryPage kPages[] = {
    {SettingCategory::Home,        "Home",           "Resolution and frame rate."},
    {SettingCategory::View,        "View",           "How the world sits in front of your eyes."},
    {SettingCategory::HandsWeapon, "Hands & Weapon", "Where the gun sits in your hand."},
    {SettingCategory::Hud,         "HUD",            "Size and position of the game's own HUD."},
    {SettingCategory::Reticle,     "Reticle",        "The floating aim mark."},
    {SettingCategory::Body,        "Body",           "How much of the pilot you can see."},
    {SettingCategory::Controls,    "Controls",       "Sticks, triggers and deadzones."},
    {SettingCategory::Advanced,    "Advanced",       "Diagnostics and starting over."},
};
constexpr int kPageCount = static_cast<int>(sizeof(kPages) / sizeof(kPages[0]));

// How many real settings a category has. A page with none is not shown at all --
// PLAN-VRMENU is explicit that a category with no controls is omitted, not
// stubbed, and an empty tab is worse than a missing one.
// ---------------------------------------------------------------------------
// PRESENTATION, KEPT OUT OF THE REGISTRY.
//
// How a setting LOOKS is not part of what a setting IS. The registry carries the
// key, the range, the getter and the tier -- the things that must not drift from
// the mechanism. Grouping, units and number formatting are a property of the
// page, so they live here, keyed on the setting name.
//
// This is also why adding a row does not mean editing thirty of them.
//
// The idioms are taken from the shipped mods rather than invented, and
// Halo-MCC-VR/src/dll/menu.cpp is the clearest example of all four:
//
//   UNITS IN THE LABEL      "Screen width (m)", "Turn speed (deg/s)"
//   HONEST PRECISION        "%.1f" and "%.0f", not "%.2f" on everything
//   ONE NOTE PER GROUP      a TextDisabled line under a cluster of related
//                           sliders, instead of a tooltip on each
//   SPACING BETWEEN GROUPS  ImGui::Spacing() so a page reads as sections
// ---------------------------------------------------------------------------
struct RowPresentation {
    const char* name;     // the setting key
    const char* group;    // heading this row opens, or nullptr to continue one
    const char* label;    // overrides the registry label, usually to add units
    const char* format;   // slider format, or nullptr for the default
    const char* note;     // printed under the LAST row of the group
    // Shown only when this OTHER setting has this value. Halo shows the turn
    // speed OR the snap increment, never both, because only one of them is doing
    // anything -- a control that provably cannot act is worse than a hidden one.
    const char* showWhenKey;
    float showWhenValue;
};

// Order here is the order rows appear on their page. Anything not listed falls
// back to the registry's own label and is drawn after the grouped rows.
const RowPresentation kRows[] = {
    // ---- Home -----------------------------------------------------------
    {"render.scale", "Picture", "Resolution", "%.2f x",
     "1.00 is your current resolution. Higher is sharper and costs frame rate.\n"
     "Both axes move together, so your field of view never changes."},

    // ---- View -----------------------------------------------------------
    {"view.world_scale", "World scale", "World scale", "%.2f",
     "How large everything looks. Lower it if hands and weapons seem oversized;\n"
     "the 3D gets stronger as you do, because both come from the same number."},
    {"game.fov_auto", "Field of view", "Work it out from the headset", nullptr,
     "On at least one headset this derives a view far wider than the display\n"
     "can show, which costs a third of the frame for world you cannot see."},
    {"game.fov_scale", nullptr, "Field of view", "%.2f",
     "Used only when auto above is off. It changes how much world you see and\n"
     "what it costs to draw, not how big anything looks."},
    {"eye.raise", "Height", "Eye height above the game's own (units, ~1 in)", "%.0f",
     "The game sits you at neck level. 7 to 10 puts you at a character's eyes.\n"
     "Camera, gun and rounds move together, so aim stays true at any value."},
    {"headtracking.neck_forward", "Neck model", "Neck length forward (m)", "%.2f", nullptr},
    {"headtracking.neck_up", nullptr, "Neck length up (m)", "%.2f",
     "Where your eyes sit relative to the neck pivot. Larger values make leaning\n"
     "your head translate the view further."},

    {"ads.max_magnification", "Aiming down sights", "Magnification cap", "%.2f x", nullptr},
    {"ads.optic_passthrough", nullptr, "Let scopes zoom past", "%.2f x",
     "0 cap lets each weapon magnify as much as the game intends, from about 1x\n"
     "on a pistol to 5x on a scoped LMG. A lower cap holds it back: wider view,\n"
     "smaller distant things. A scope zooming past the second value is exempt."},

    // ---- Hands & Weapon --------------------------------------------------
    {"hand.grip_fwd", "Grip", "Grip forward (in)", "%.2f", nullptr},
    {"hand.grip_right", nullptr, "Grip right (in)", "%.2f", nullptr},
    {"hand.grip_up", nullptr, "Grip up (in)", "%.2f",
     "Where the gun sits in your hand. This is also the pivot: the point that\n"
     "stays put when you rotate your wrist."},

    {"hand.off_fwd", "Weapon position", "Forward (in)", "%.2f", nullptr},
    {"hand.off_left", nullptr, "Left (in)", "%.2f", nullptr},
    {"hand.off_up", nullptr, "Up (in)", "%.2f",
     "Moves the whole weapon in your own frame, rather than the gun's."},

    {"hand.off_pitch", "Weapon angle", "Pitch (deg)", "%.1f", nullptr},
    {"hand.off_yaw", nullptr, "Yaw (deg)", "%.1f", nullptr},
    {"hand.off_roll", nullptr, "Roll (deg)", "%.1f",
     "Tilts the weapon relative to your hand."},

    {"hand.use_pitch", "Wrist tracking", "Follow wrist pitch", nullptr, nullptr},
    {"hand.use_roll", nullptr, "Follow wrist roll", nullptr,
     "Roll is NOT free to change once the grip is dialled in: the grip offset is\n"
     "rotated by the gun's orientation, so turning roll off moves the gun."},

    {"ads.lock", "Aiming down sights", "Sights take over", nullptr, nullptr},
    {"ads.face_aim", nullptr, "Turn to face the gun", nullptr, nullptr},
    {"ads.face_aim_pitch", nullptr, "Tilt to face the gun", nullptr, nullptr},
    {"ads.turn_scale", nullptr, "Turn speed while aiming", "%.2f",
     "The gun centres and shoots where you look, as it does on a monitor, and\n"
     "turning slows to the fraction above. Your hand stops steering the gun\n"
     "until you let go."},
    {"ads.brace_gain", "Aim brace", "Brace strength", "%.2f", nullptr},
    {"ads.brace_cone_deg", nullptr, "Fine-tune cone (deg)", "%.1f", nullptr},
    {"ads.brace_readopt_deg", nullptr, "Release angle (deg)", "%.1f",
     "Down the sights the gun moves a fraction of what your hand does, in the\n"
     "same frame -- steadier, never later. Move past the release angle and it\n"
     "follows your hand normally again. 1.00 strength turns the brace off."},


    {"hand.yawcomp", "Advanced", "Yaw compensation", "%.2f", nullptr},
    {"pin.fwd", nullptr, "Pin forward", "%.0f", nullptr},
    {"pin.right", nullptr, "Pin right", "%.0f", nullptr},
    {"pin.up", nullptr, "Pin up", "%.0f", nullptr},
    {"pin.spin", nullptr, "Pin spin (deg/s)", "%.0f",
     "Diagnostics. Yaw compensation is measured at 0 -- do not sweep it without a\n"
     "reason. The pin values are ignored while the hand drives the weapon."},

    // ---- HUD --------------------------------------------------------------
    // Grouped by WHICH MECHANISM MOVES THEM, not by where they sit on screen,
    // because that is the thing a wearer has to know to pick the right control:
    // the two corner groups and the flat pass are three separate paths, and a
    // slider that does nothing is the confusing case this ordering avoids.
    {"rui.ll_scale", "Corners", "Ammo and ability corner size", "%.2f", nullptr},
    {"hud.prompt_scale", nullptr, "Dialogue response prompt, size", "%.3f",
     "The up/down prompt shown when you are asked a question. 1.82 is the size the game draws; "
     "above that it moves down as it grows, so lift it with the control below."},
    {"hud.prompt_shift_y", nullptr, "Dialogue response prompt, up and down", "%.3f",
     "Moves that prompt vertically, as a fraction of screen height. Negative is up."},
    {"hud.instruction_scale", nullptr, "Gauntlet instruction panel, size", "%.3f",
     "The big instruction panel the training gauntlet puts on screen. Size only: changing it does not move the panel."},
    {"hud.instruction_shift_y", nullptr, "Gauntlet instruction panel, up and down", "%.3f",
     "Moves that panel vertically, as a fraction of screen height. Positive is down."},
    {"hud.timer_scale", nullptr, "Gauntlet timer, size", "%.3f",
     "The gauntlet's run timer. Size only: changing it does not move the timer."},
    {"hud.timer_shift_x", nullptr, "Gauntlet timer, across", "%.3f",
     "Moves the timer sideways. Negative is left, in from the edge."},
    {"hud.timer_shift_y", nullptr, "Gauntlet timer, up and down", "%.3f",
     "Moves the timer vertically. Negative is up."},
    {"hud.highlight_scale", nullptr, "Weapon pickup highlight, size", "%.3f",
     "The prompt over a weapon you can pick up or swap. It rides the same size control as the rest of the HUD, which makes it too small to read in a headset; this brings it back on its own. 1.82 is the size the game draws."},
    {"hud.ll_shift_x_titan", nullptr, "Ammo corner across, in a Titan", "%.3f",
     "The Titan's loadout bar is a different readout with more in it, so it\n"
     "gets its own sideways amount. On foot the setting below applies."},
    {"hud.ll_shift_x", nullptr, "Ammo and ability corner, across", "%.3f",
     "Moves the lower-left weapon and ammo group sideways only. Positive is\n"
     "right. The size control moves this group by scaling it about the screen\n"
     "centre, so it cannot come in from the edge without shrinking; this is the\n"
     "half that moves it without changing its size."},
    {"rui.inset", nullptr, "Top-left cluster inset", "%.2f",
     "The lower-left group is weapon, ammo, tactical and ordnance; the top-left\n"
     "cluster is a separate path with its own control. Bigger numbers pull each\n"
     "further in from its corner, which a headset usually wants."},
    {"hud2d.shift_y", "Reticle and flat HUD", "Move up and down (px)", "%.0f", nullptr},
    {"hud2d.shift_x", nullptr, "Move across (px)", "%.0f", nullptr},
    {"hud2d.zoom", nullptr, "Size", "%.2f x",
     "These move the flat pass, the reticle included. If the aim mark and where\n"
     "your rounds actually land disagree vertically, move it here."},

    // ---- The headset-tuned controls, 2026-09-06 -------------------------
    // Split by WHO SEES THEM, which is the division the wearer named. The
    // waypoint marker pair belongs in the first group, not beside the flat
    // pass where it used to sit: the marker is on screen on foot and in a
    // Titan, and it is a type-3 widget rather than part of that pass, so
    // filing it under the reticle read as a promise this project cannot keep.
    {"hud.widget_scale", "On foot and in a Titan", "Weapon and ammo group size", "%.2f x",
     "The lower-left weapon and ammo group and the other flat segments that sit\n"
     "at the screen edges. Smaller pulls them in from the corners. In a headset\n"
     "the screen edge is outside comfortable vision, which is why 0.55 reads\n"
     "better than the game's own 1.00."},
    {"hud.marker_size", nullptr, "Waypoint marker size", "%.2f x", nullptr},
    {"hud.marker_text_x", nullptr, "Waypoint text position", "%.2f",
     "Marker size is size only -- the marker holds its spot in the world at\n"
     "every setting, because where it sits is decided by the HUD pass's frustum\n"
     "and not by this.\n"
     "Text position moves the distance readout and its leader line sideways. The\n"
     "game drops that block at one of two places and the wrong one is jammed\n"
     "against the left edge, unreadable in a headset. The target icon never\n"
     "moves and nothing changes height; the leader line is re-attached so its\n"
     "far end still lands on the target. 0 turns the fix off."},
    {"hud.nameplate_scale", nullptr, "Name label size", "%.2f x",
     "The floating labels over friends and enemies, and the health bar under an\n"
     "enemy one. Size only: a label stays on the player it names, and the bar\n"
     "shrinks with its label as one piece rather than drifting away from it.\n"
     "These draw on their own layer, which nothing was reaching before, which is\n"
     "why they stayed full size while the rest of the HUD came in."},
    {"hud.cockpit_scale", "Titan cockpit only", "Cockpit HUD size", "%.2f x", nullptr},
    {"hud.bars_scale", nullptr, "Dash bars size", "%.2f x", nullptr},
    {"hud.bars_shift_y", nullptr, "Dash bars height", "%+.3f",
     "Cockpit HUD size moves the grey surround and the top health bar together,\n"
     "because they are one widget. The two dash bars are a separate widget and\n"
     "keep their own size and height, since the cockpit setting shrank them when\n"
     "they wanted to be larger. On height, NEGATIVE MOVES THEM UP: that widget\n"
     "sits on cockpit geometry whose vertical axis runs opposite to the rest of\n"
     "the HUD. Measured in the headset, not assumed."},
    {"lockhud.viewfix", "Titan lock rings", "Rings stay on their targets", nullptr,
     "Multi-target missiles: hold the trigger, sweep the square over enemies, and\n"
     "each painted one gets a ring. On, the rings stay on their targets while the\n"
     "square follows your hand. Off, they ride the square. Every number behind\n"
     "this is read live from the game, so it holds at any resolution."},
    {"lockhud.ring_scale", nullptr, "Ring size", "%.2f x",
     "Size of each lock ring. Placed by its centre, so size never moves it.\n"
     "1.00 is the game's own size; 0.60 is the settled value."},

    // ---- Reticle ---------------------------------------------------------
    {"ads.keep_reticle", "Reticle", "Keep it while aiming", nullptr,
     "Flat hides the aim mark in ADS because the monitor locks your eye to the\n"
     "sights. In VR nothing does, so hiding it leaves no reference at all.\n"
     "The mark sits where the round goes."},
    {"reticle.hidden", nullptr, "Hide the reticle", nullptr,
     "Removes the aim mark entirely. Wins over the setting above if both are on."},
    {"crosshair.scale", nullptr, "Element scale", "%.2f x",
     "Element scale is MEASURED to do nothing -- the float it drives is not the\n"
     "reticle's size. It is kept because it is real and reversible, not because\n"
     "it works."},

    // ---- Body ------------------------------------------------------------
    {"body.show", "Pilot body", "Show", nullptr,
     "Weapon only hides the arms and body. Weapon + hands keeps the gloves on\n"
     "the gun and hides the rest. In a titan nothing is hidden in any mode."},

    // ---- Controls --------------------------------------------------------
    {"input.crouch_mode", nullptr, "Crouch", nullptr,
     "Crouch uses the game's own toggle command, so slide and wallrun behave\n"
     "exactly as they do without the mod."},

    {"input.turn_mode", "Turning", "Turning", nullptr, nullptr, nullptr, 0.0f},
    {"input.turn_speed", nullptr, "Turn speed (%)", "%.0f%%",
     "A percentage of the game's own turn rate. Your in-game look sensitivity "
     "still applies on top.",
     "input.turn_mode", 0.0f},
    {"input.turn_curve", nullptr, "Response curve", "%.1f",
     "1.0 is straight-line. Higher gives finer control near centre without "
     "lowering the top speed.",
     "input.turn_mode", 0.0f},
    {"input.snap_turn", nullptr, "Snap step (deg)", "%.0f",
     "Each flick turns you this far, instantly. 30 suits most people; larger "
     "steps mean fewer flicks but a bigger jump each time.",
     "input.turn_mode", 1.0f},

    {"input.haptic_strength", "Rumble", "Strength (%)", "%.0f%%",
     "The game's own rumble, scaled. 100 is the strongest OpenXR will deliver."},

    {"input.turn_deadzone", "Right stick", "Turn deadzone", "%.2f", nullptr},
    {"input.jumpcrouch_deadzone", nullptr, "Jump / crouch deadzone", "%.2f", nullptr},
    {"input.stick_cross_ratio", nullptr, "Separation", "%.2f",
     "Separation stops turning from clipping a crouch and the reverse. Higher\n"
     "separates them more; 0 lets both fire from one diagonal push."},

    // ---- Advanced --------------------------------------------------------
    {"xr.decouple", "Headset", "Decouple the frame loop", nullptr,
     "Gives the headset its own pacing thread so it keeps fresh poses while the\n"
     "game is slow. It does not shorten a level load."},

    {"hotkeys.require_foreground", "Hotkeys", "Only when the game has focus", nullptr, nullptr},
    {"hotkeys.leader_timeout_ms", nullptr, "Leader timeout (ms)", "%.0f",
     "The keyboard hotkeys, which are only reachable at a desk."},
};
constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));

const RowPresentation* PresentationFor(const char* name) {
    for (const auto& row : kRows) {
        if (std::strcmp(row.name, name) == 0) return &row;
    }
    return nullptr;
}

int RowsInCategory(SettingCategory category);

// Steps to the next/previous category that actually has something on it, so a
// grip press never lands the wearer on an empty page.
void StepMenuCategory(int delta) {
    if (delta == 0) return;
    for (int step = 0; step < kPageCount; ++step) {
        g_activePage += delta;
        if (g_activePage < 0) g_activePage = kPageCount - 1;
        if (g_activePage >= kPageCount) g_activePage = 0;
        if (kPages[g_activePage].category == SettingCategory::Home ||
            RowsInCategory(kPages[g_activePage].category) > 0) {
            return;
        }
    }
}

int RowsInCategory(SettingCategory category) {
    int count = 0;
    for (int i = 0; i < SettingCount(); ++i) {
        const Setting* setting = SettingAt(i);
        if (setting && setting->category == category) ++count;
    }
    return count;
}

// Draws one registry row. Returns the row index if it took focus this frame.
// Draws one registry row. Returns the row index if it took focus this frame.
int DrawSettingRow(int index, const RowPresentation* look) {
    const Setting* setting = SettingAt(index);
    if (!setting) return -1;

    ImGui::PushID(index);
    g_buildingIndex = index;
    float value = StagedOrLiveValue(index);
    bool changed = false;
    const char* label = (look && look->label) ? look->label : setting->label;
    const char* format = (look && look->format) ? look->format : "%.2f";

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
    switch (setting->widget) {
        case SettingWidget::Toggle: {
            bool on = value != 0.0f;
            if (ImGui::Checkbox(label, &on)) {
                value = on ? 1.0f : 0.0f;
                changed = true;
            }
            break;
        }
        case SettingWidget::Combo: {
            int choice = static_cast<int>(value + 0.5f);
            int count = 0;
            if (setting->comboLabels) {
                while (setting->comboLabels[count]) ++count;
            }
            if (count > 0 && ImGui::Combo(label, &choice, setting->comboLabels, count)) {
                value = static_cast<float>(choice);
                changed = true;
            }
            break;
        }
        case SettingWidget::Numeric: {
            int whole = static_cast<int>(value + 0.5f);
            if (ImGui::DragInt(label, &whole, 1.0f, static_cast<int>(setting->minimum),
                               static_cast<int>(setting->maximum))) {
                value = static_cast<float>(whole);
                changed = true;
            }
            break;
        }
        case SettingWidget::Slider:
        default:
            if (ImGui::SliderFloat(label, &value, setting->minimum, setting->maximum, format)) {
                changed = true;
            }
            break;
    }
    if (changed) StageSettingValue(index, value);
    const bool focused = ImGui::IsItemFocused() || ImGui::IsItemHovered();

    // A RESET, inline and only on the row that has focus. Halo puts one beside
    // the controls that need it ("Raw (0%)##headset"); showing one on every row
    // would double the width of the page for a button nobody is reaching for.
    if (focused && setting->widget != SettingWidget::Toggle) {
        ImGui::SameLine();
        if (ImGui::SmallButton("reset")) StageSettingValue(index, setting->defaultValue);
    }

    // The tier is printed ON the control for anything not live -- Halo's
    // convention, and the plan adopts it.
    const char* tier = SettingTierLabel(setting->tier);
    if (tier && tier[0]) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", tier);
    }
    ImGui::PopID();
    return focused ? index : -1;
}

void BuildConfigPanel() {
    // FILLS THE PANEL TEXTURE, LOCKED. Cond_Always so ImGui cannot keep a
    // position of its own, and every drag target removed.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    // THE PARENT WINDOW NEVER SCROLLS, and pinning it is the fix for "the top
    // menu with the categories is cut off".
    //
    // The tab row is drawn in THIS window and the settings pane below is a
    // child with NavFlattened, which is what lets the stick reach the rows from
    // here. The cost of flattening is that a focused row belongs to this window
    // for navigation, so ImGui will scroll THIS window to keep it visible -- and
    // the only thing above the pane to scroll away is the tab row. Any layout
    // that overflows this window by even a few pixels (a footer estimate off by
    // a line, a font that rasterises a shade taller) makes it scrollable, and
    // then the categories slide off the top exactly as reported.
    //
    // Pinned to zero every frame rather than merely hiding the scrollbar: the
    // flags below stop a scrollbar and the mouse wheel, but NOT navigation
    // scrolling, which is the one doing it. The pane keeps its own scrolling,
    // so long pages still reach their last row.
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    if (!ImGui::Begin("Titanfall 2 VR", nullptr,
                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                          ImGuiWindowFlags_NoBringToFrontOnFocus |
                          ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    if (g_needFocus) {
        g_needFocus = false;
        ImGui::SetWindowFocus();
    }

    const float footerHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f +
                               ImGui::GetStyle().ItemSpacing.y * 2.0f + 8.0f;
    const float scale = UiScale();

    // ---- the header, ABOVE THE TABS --------------------------------------
    //
    // The window is NoTitleBar, so the string passed to Begin() is an ImGui id
    // and is never drawn: without this line nothing on the panel names the mod
    // or says which build you are looking at.
    //
    // NOTHING ON THIS LINE IS SPELLED HERE. The name, the author and the version
    // all come out of the generated version.h, which is configured from the
    // three lines in plugin/CMakeLists.txt that also name the zip, the git tag,
    // the log banner and the name Northstar is given. A version painted on the
    // UI that could disagree with the DLL painting it is worse than showing
    // none -- it is exactly the string a bug report will quote back -- and the
    // same goes for an author who is one person in LICENSE and another here.
    ImGui::TextColored(kAccentV4, "%s Mod", TF2VR_PRODUCT_NAME);
    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.45f);
    ImGui::TextDisabled("by %s  (%s)", TF2VR_AUTHOR, TF2VR_VERSION_TAG);
    ImGui::Spacing();

    // ---- the tabs, ACROSS THE TOP ----------------------------------------
    //
    // Was a left sidebar, taken from Halo. The wearer wants them along the top,
    // and on a panel this shape they are right: a 4:3 quad has far more width to
    // spare than height, and a sidebar was eating a quarter of the width to show
    // eight short words.
    //
    // Still not focusable -- the grips change page. The row is an indicator.
    for (int i = 0; i < kPageCount; ++i) {
        if (RowsInCategory(kPages[i].category) == 0 &&
            kPages[i].category != SettingCategory::Home) {
            continue;
        }
        // Real space between tabs. SameLine() alone butts them together at the
        // default item spacing, which on a wide panel reads as one run-on line.
        if (i > 0) ImGui::SameLine(0.0f, ImGui::GetFontSize() * 1.6f);
        const bool selected = g_activePage == i;
        if (selected) {
            ImGui::TextColored(kAccentV4, "%s", kPages[i].label);
            // An accent underline on the active tab, which is the top-of-screen
            // equivalent of the bar Halo puts down the left edge of its rows.
            const ImVec2 lo = ImGui::GetItemRectMin();
            const ImVec2 hi = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(lo.x, hi.y), ImVec2(hi.x, hi.y + 3.0f * scale), kAccentU32);
        } else {
            ImGui::TextDisabled("%s", kPages[i].label);
        }
    }
    ImGui::Spacing();
    ImGui::Separator();


    // ---- the pane --------------------------------------------------------
    int hintIndex = -1;
    // NavFlattened so nav reaches the rows from the parent window.
    ImGui::BeginChild("##pane", ImVec2(0.0f, -footerHeight),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    if (g_activePage >= 0 && g_activePage < kPageCount) {
        const CategoryPage& page = kPages[g_activePage];
        ImGui::TextColored(kAccentV4, "%s", page.label);
        ImGui::TextDisabled("%s", page.blurb);
        ImGui::Separator();
        ImGui::Spacing();

        if (page.category == SettingCategory::Home) {
            DrawVideoReadout();
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }

        // ROWS IN PRESENTATION ORDER, GROUPED. The presentation table decides
        // the order and the headings; anything it does not mention is drawn
        // afterwards so a new registry row still appears without being styled.
        const char* openGroup = nullptr;
        for (int r = 0; r < kRowCount; ++r) {
            const Setting* setting = FindSetting(kRows[r].name);
            if (!setting || setting->category != page.category) continue;

            // Rows that only apply in one mode are hidden in the other. Read
            // against the STAGED value, so switching the mode re-draws the page
            // immediately rather than after the panel is closed.
            if (kRows[r].showWhenKey) {
                const Setting* gate = FindSetting(kRows[r].showWhenKey);
                int gateIndex = -1;
                for (int g = 0; g < SettingCount(); ++g) {
                    if (SettingAt(g) == gate) { gateIndex = g; break; }
                }
                if (gateIndex < 0) continue;
                const float gateValue = StagedOrLiveValue(gateIndex);
                if (gateValue < kRows[r].showWhenValue - 0.5f ||
                    gateValue > kRows[r].showWhenValue + 0.5f) {
                    continue;
                }
            }
            int index = -1;
            for (int i = 0; i < SettingCount(); ++i) {
                if (SettingAt(i) == setting) { index = i; break; }
            }
            if (index < 0) continue;

            if (kRows[r].group) {
                if (openGroup) ImGui::Spacing();
                ImGui::SeparatorText(kRows[r].group);
                openGroup = kRows[r].group;
            }
            const int focused = DrawSettingRow(index, &kRows[r]);
            if (focused >= 0) hintIndex = focused;

            // ONE NOTE PER GROUP, under its last row, rather than a tooltip on
            // each. This is Halo's shape and it is the difference between a page
            // that reads and a page that is thirty labelled sliders.
            if (kRows[r].note) {
                ImGui::TextDisabled("%s", kRows[r].note);
                ImGui::Spacing();
            }
        }

        // Anything the presentation table does not cover.
        bool strayHeading = false;
        for (int i = 0; i < SettingCount(); ++i) {
            const Setting* setting = SettingAt(i);
            if (!setting || setting->category != page.category) continue;
            if (PresentationFor(setting->name)) continue;
            if (!strayHeading) {
                strayHeading = true;
                ImGui::Spacing();
                ImGui::SeparatorText("Other");
            }
            const int focused = DrawSettingRow(i, nullptr);
            if (focused >= 0) hintIndex = focused;
        }
    }
    ImGui::EndChild();

    // ---- the footer ------------------------------------------------------
    ImGui::Separator();
    const Setting* hint = hintIndex >= 0 ? SettingAt(hintIndex) : nullptr;
    if (hint) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(hint->tooltip);
        ImGui::PopTextWrapPos();
    } else {
        const int staged = StagedEditCount();
        if (staged > 0) {
            ImGui::TextColored(kAccentV4, "%d change%s pending - applied and saved when you close",
                               staged, staged == 1 ? "" : "s");
        } else {
            ImGui::TextDisabled("Stick moves - A changes a setting - GRIPS change page - L3+R3 closes");
        }
    }

    ImGui::End();
}


// THE PANEL BUILD, BEHIND A FAULT GUARD.
//
// The first build of the real registry crashed the game on summon, and a crash
// costs the wearer a whole run to learn one bit. This turns that into a named
// line and keeps the game alive: on a fault it reports WHICH row was being
// built, disables the overlay for the rest of the session, and returns.
//
// g_buildingIndex is written before each row, so the report names the row rather
// than just the function. -1 means the fault was outside the loop entirely,
// which is itself the answer to a different question.
bool BuildConfigPanelGuarded() {
    __try {
        BuildConfigPanel();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        const int index = g_buildingIndex;
        const Setting* setting = SettingAt(index);
        char line[340]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] menu: FAULTED building the panel at row %d (%s). The overlay is disabled for "
            "the rest of this session and the game keeps running.\n",
            index, setting ? setting->name : "outside the row loop");
        Tf2VrLog(line);
        return false;
    }
}
}  // namespace

void SetMenuOpen(bool open) {
    const bool was = g_open.exchange(open, std::memory_order_acq_rel);
    if (was == open) return;
    if (open) {
        g_needFocus = true;
        // Nothing should be carried in from a previous session of the panel.
        DiscardStagedSettings();
    } else {
        // AUTO-COMMIT ON CLOSE, as asked. Commit and Discard are deliberately
        // separate calls so a confirmation step can be added later without
        // moving anything -- the wearer has already said they want one
        // eventually.
        CommitStagedSettings();
    }

    // NO ESC, AND NO GAME PAUSE MENU. This was tried and measured, and it is
    // recorded here because it looked obviously right and was obviously wrong.
    //
    // The first design opened the GAME's menu on the way in, because that pauses
    // the game AND -- per PLAN-VRMENU 2.4 -- was supposed to suppress gameplay
    // input for free: vr_input's MenuIsOpen() going true is what calls
    // ReleaseAll() and returns before any Apply().
    //
    // THAT PREMISE IS FALSE IN THIS CONFIGURATION, and the run proved it.
    // MenuIsOpen() is a CURSOR-VISIBILITY test, and in VR the cursor is not
    // shown while the pause menu is up. Every single log line from that run read
    // "game menu down" -- including the ones written while the wearer was
    // looking at the pause menu. So the suppression never engaged, the sticks
    // drove the pause menu underneath our panel, B stopped working because it
    // was gated on that same dead predicate, and the ESC toggles ran a state
    // machine one step out of phase with what the wearer saw.
    //
    // So the pause is gone and suppression keys on OUR OWN panel state, which we
    // set, own, and can therefore trust. The game keeps running while the panel
    // is up -- the wearer is stationary and cannot act, but is not invulnerable.
    // Real pausing wants an engine lever we have actually verified, not a
    // synthetic keystroke whose effect we cannot observe.
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] menu: %s | gameplay input %s (vr_input releases its kbuttons and the synthetic "
        "pad reports nothing while this is up). The game is NOT paused.\n",
        open ? "OPEN" : "closed",
        open ? "SUPPRESSED" : "handed back");
    Tf2VrLog(line);
}

bool IsMenuOpen() { return g_open.load(std::memory_order_acquire); }

void ToggleMenu() { SetMenuOpen(!g_open.load(std::memory_order_acquire)); }

void MenuOnVerifiedPresent(IDXGISwapChain* swapchain) {
    // THE CLOSED FAST PATH, AND IT IS FIRST ON PURPOSE. Until the menu has been
    // opened at least once there is no ImGui context, no subclass and no vertex
    // buffer -- a session that never opens it pays one atomic load per frame.
    // WHY `!IsXrArmed()` IS GONE, and it cost a whole run.
    //
    // 2026-09-07, flat run D5FA26FD: auto-arm called SetXrArmed(true) at
    // 6777 ms, and thirty milliseconds later the runtime reported NO HEADSET
    // and created no session. IsXrArmed() therefore stayed true for a run that
    // was rendering to a monitor, so this predicate was false from 6.8 s
    // onward -- thirteen seconds before the wearer pressed F3. The overlay
    // never even initialised (no "overlay initialised" line in that log), the
    // wearer saw no prompt, and a search that counted 11127 suppressions told
    // us nothing because nobody was told what to look at.
    //
    // "Armed" is not "being displayed", and the prompt does not need to know
    // the difference: it is drawn into whichever surface this function is
    // already choosing below, and MenuLayerVisible() now submits the VR quad
    // for it as well as for the panel.
    const bool huntPrompt = RuiHuntPromptWanted();
    if (!g_open.load(std::memory_order_acquire) && !g_ready.load(std::memory_order_acquire) && !huntPrompt) return;
    if (!swapchain) return;

    // Measured separately from kPresent, so "the overlay costs nothing while
    // closed" is a number rather than the absence of one.
    PluginCost::Scope costScope(PluginCost::kMenu);

    if (!g_ready.load(std::memory_order_acquire)) {
        if (!Initialise(swapchain)) return;
    }
    if (!g_open.load(std::memory_order_acquire) && !huntPrompt) return;
    // WHICH SURFACE THE PANEL IS DRAWN INTO, and this single choice is the
    // entire difference between the two presentation adapters.
    //
    // XR armed  -> a private 1024x768 texture, which xr_context shows on a
    //              world-fixed quad. The panel stays where it was summoned when
    //              the wearer turns their head.
    // flat      -> the game's backbuffer, as before, so a monitor run still
    //              works and still verifies everything above the render target.
    //
    // Everything else in this function is identical for both, which is what the
    // "one context, two adapters" architecture was for.
    // IsXrPresentingToHeadset, not IsXrArmed. On the 2026-09-07 flat run this
    // line chose the VR panel texture because armed was true with no session,
    // and the prompt's own heartbeat then recorded 3600 draws into a quad
    // nothing submitted while the wearer watched a bare monitor. The gate above
    // was fixed and this one was not, so the same root cause survived one
    // repair -- which is why the heartbeat had to exist to catch it.
    const bool vrTarget = IsXrPresentingToHeadset() && EnsureMenuTexture(swapchain);
    if (!vrTarget && !EnsureRenderTarget(swapchain)) return;
    const UINT surfaceWidth = vrTarget ? kMenuTextureWidth : g_backbufferWidth;
    const UINT surfaceHeight = vrTarget ? kMenuTextureHeight : g_backbufferHeight;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    RemapToSurface(surfaceWidth, surfaceHeight);
    // BEFORE NewFrame, not after: ImGui drains its input event queue inside
    // NewFrame, so anything added afterwards would not be seen until the NEXT
    // frame -- a whole frame of lag on every stick flick.
    if (g_open.load(std::memory_order_acquire)) FeedNavFromControllers(true);
    ImGui::NewFrame();
    if (huntPrompt) {
        // THE HEARTBEAT. The old prompt had none, so a channel that never drew
        // a single pixel was indistinguishable in the log from one the wearer
        // was reading. It says which surface it went into, because "drawn" into
        // a VR panel texture that is never submitted is still not seen.
        ++g_promptDraws;
        if (g_promptDraws == 1 || (g_promptDraws % 1200) == 0) {
            char beat[300]{};
            std::snprintf(beat, sizeof(beat),
                "[TF2VR] HUNT PROMPT drawn %llu time(s) into the %s (%ux%u). If the wearer cannot "
                "read this on screen the channel is dead, whatever any suppression counter says.\n",
                static_cast<unsigned long long>(g_promptDraws),
                vrTarget ? "VR panel quad" : "game backbuffer", surfaceWidth, surfaceHeight);
            Tf2VrLog(beat);
        }
        char prompt[512]{};
        RuiHuntStatus(prompt, sizeof(prompt));
        ImGui::SetNextWindowPos(ImVec2(surfaceWidth * 0.5f, ImGui::GetFontSize()),
                               ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::Begin("Friendly name identification", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 44.0f);
        ImGui::TextUnformatted(prompt);
        ImGui::PopTextWrapPos();
        ImGui::End();
    }
    if (g_open.load(std::memory_order_acquire) && !BuildConfigPanelGuarded()) {
        // The frame is still ended cleanly -- ImGui state must not be left
        // half-built -- and then the overlay takes itself out of the session.
        ImGui::EndFrame();
        g_initFailed.store(true, std::memory_order_release);
        g_open.store(false, std::memory_order_release);
        return;
    }
    ImGui::Render();

    D3DStateBackup backup;
    backup.Capture(g_context.Get());
    ID3D11RenderTargetView* target = vrTarget ? g_menuRtv.Get() : g_rtv.Get();
    // No depth-stencil: the panel is an overlay and must not be occluded by, or
    // write into, the game's depth buffer.
    g_context->OMSetRenderTargets(1, &target, nullptr);
    if (vrTarget) {
        // The private target is OURS alone, so it is cleared each frame. The
        // backbuffer is emphatically not -- clearing that would erase the game.
        const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        g_context->ClearRenderTargetView(g_menuRtv.Get(), transparent);
    }
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    backup.Restore(g_context.Get());

    g_framesDrawn.fetch_add(1, std::memory_order_relaxed);
}

void ShutdownMenuOverlay() {
    if (!g_ready.exchange(false, std::memory_order_acq_rel)) return;
    if (g_window && g_originalWndProc) {
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
        g_originalWndProc = nullptr;
    }
    g_subclassed.store(false, std::memory_order_release);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ReleaseRenderTarget();
    g_context.Reset();
    g_device.Reset();
    g_window = nullptr;
    Tf2VrLog("[TF2VR] menu: overlay shut down and the WndProc restored.\n");
}

void AdvanceMenuOverlay() {
    // THE FALSIFIER'S READOUT. The three ways M1 fails are separable from one
    // flat run, and none of them look alike in this line:
    //
    //   open, ready=0            init never ran or failed -- Present is not
    //                            reaching us on a verified swapchain
    //   open, ready=1, frames    the draw is running; if nothing is on screen
    //   not climbing             the RTV is the question
    //   open, frames climbing,   the draw is landing somewhere that is not the
    //   nothing visible          presented backbuffer
    //
    // Silent unless something changed or a second has passed with it open, so a
    // closed session costs one comparison and writes nothing.
    static bool lastOpen = false;
    static bool lastReady = false;
    static std::uint64_t lastTick = 0;
    static std::uint64_t lastFrames = 0;

    const bool open = g_open.load(std::memory_order_acquire);
    const bool ready = g_ready.load(std::memory_order_acquire);

    // STEP THE NAV EDGE DETECTORS EVEN WHILE CLOSED, and discard the result.
    // Without this, opening the panel with a stick already held reads as a
    // fresh edge and the focus jumps before the wearer has touched anything.
    // Safe before init: it returns without touching ImGui when closed.
    if (!open) FeedNavFromControllers(false);
    const std::uint64_t tick = GetTickCount64();
    const bool changed = (open != lastOpen) || (ready != lastReady);
    if (!changed && (!open || tick - lastTick < 1000)) return;
    lastOpen = open;
    lastReady = ready;
    lastTick = tick;

    const std::uint64_t frames = g_framesDrawn.load(std::memory_order_relaxed);
    char line[300]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR] menu: open=%d ready=%d subclassed=%d initfailed=%d | backbuffer %ux%u | "
                  "drawn=%llu (+%llu since last second)\n",
                  open ? 1 : 0, ready ? 1 : 0,
                  g_subclassed.load(std::memory_order_acquire) ? 1 : 0,
                  g_initFailed.load(std::memory_order_acquire) ? 1 : 0,
                  g_backbufferWidth, g_backbufferHeight,
                  static_cast<unsigned long long>(frames),
                  static_cast<unsigned long long>(frames - lastFrames));
    Tf2VrLog(line);
    lastFrames = frames;
}

void SetMenuUiScale(float scale) {
    // Clamped so a stray value cannot make the panel unreadable or fill the
    // frame -- this is dialled by feel with a headset on, and there is no way
    // back from a panel too small to read the control that shrank it.
    if (scale < 0.15f) scale = 0.15f;
    if (scale > 3.0f) scale = 3.0f;
    g_uiScale.store(scale, std::memory_order_release);
    char line[200]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR] menu: ui scale %.2f (x the resolution-derived size).\n",
                  static_cast<double>(scale));
    Tf2VrLog(line);
}

float MenuUiScale() { return g_uiScale.load(std::memory_order_acquire); }

ID3D11Texture2D* MenuLayerTexture() { return g_menuTexture.Get(); }

bool MenuLayerVisible() {
    // The texture-with-something-in-it half is load-bearing and stays: reporting
    // visible with a texture that has never been drawn would show the compositor
    // a frame of uninitialised memory.
    //
    // The open-panel half now also admits the identification prompt. In a
    // headset run the prompt was drawn into this texture and then never
    // submitted, because the quad was gated on the CONFIG PANEL being open --
    // so fixing the flat gate alone would have moved the same silent failure
    // from the monitor to the headset.
    return (g_open.load(std::memory_order_acquire) || RuiHuntPromptWanted()) && g_menuTexture &&
           g_framesDrawn.load(std::memory_order_relaxed) > 0;
}
