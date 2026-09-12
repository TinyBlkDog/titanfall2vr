#pragma once

struct IDXGISwapChain;

// ---------------------------------------------------------------------------
// THE CONFIG MENU'S FLAT HALF. PLAN-VRMENU M1.
//
// ONE ImGui context, TWO presentation adapters. This is the FLAT adapter: the
// backbuffer RTV taken from the swapchain the Present hook has already
// verified. The VR adapter (M6) is a private 1024x768 texture rendered into a
// dedicated XrSwapchain and shown on one head-locked quad. They differ in which
// render target is bound and nothing else, which is the whole reason for
// building the flat one first -- everything above the render target is shared,
// and all of it can be finished and driven on a monitor for zero headset runs.
//
// WHY THE DRAW LIVES IN PRESENT. The backbuffer is the swapchain's, and the
// swapchain belongs to the presenting thread. Every other capture in this
// project already goes through Present for that reason. Drawing anywhere else
// would mean touching a resource another thread owns.
//
// THE DX11 HAZARD, AND WHY THERE IS A STATE BACKUP. ImGui's D3D11 backend sets
// its own input layout, shaders, blend state, viewport, scissor and render
// target, and does NOT put back what was there. The game's renderer does not
// re-set everything every draw -- it relies on state it left behind. So an
// overlay that draws without saving and restoring produces a corrupted GAME
// image, which looks nothing like an overlay bug and is the failure the M1
// falsifier is aimed at. The backup below is Halo's, and it is not optional.
//
// BACKBUFFER IDENTITY. A resolution change (or the render-resolution work in
// M4b) destroys and recreates the swapchain's buffers, leaving our render
// target view pointing at a dead texture. So the RTV is keyed on the
// backbuffer's own pointer and rebuilt whenever that pointer changes, rather
// than being built once and trusted. This is BioShock's approach.
// ---------------------------------------------------------------------------

// menu.open -- so the panel can be raised without the key. The key is for the
// wearer; the INI key is for a scripted flat run, which has no keyboard.
void SetMenuOpen(bool open);
bool IsMenuOpen();
void ToggleMenu();

// Called from inside the Present hook, on a VERIFIED game swapchain, BEFORE the
// real Present is forwarded. Does nothing at all until the menu is first
// opened: initialisation is lazy, so a session that never opens the menu never
// creates an ImGui context, never subclasses the window, and never allocates a
// vertex buffer.
void MenuOnVerifiedPresent(IDXGISwapChain* swapchain);

// Releases the device objects, the ImGui context and the WndProc subclass.
// Safe to call when nothing was ever initialised.
void ShutdownMenuOverlay();

// The falsifier's readout, on the plugin frame's own clock: whether the overlay
// is initialised, how many frames it has drawn, and whether the window subclass
// took. Called once per plugin frame; silent unless something has changed or a
// second has passed with the menu open.
void AdvanceMenuOverlay();

// menu.ui_scale -- the wearer's multiplier on the resolution-derived panel size.
// The auto term keeps the panel the same apparent size at any render
// resolution; this is the part only the wearer can decide. Default 0.5.
void SetMenuUiScale(float scale);
float MenuUiScale();

// ---------------------------------------------------------------------------
// THE VR PRESENTATION ADAPTER. PLAN-VRMENU M6.
//
// The flat adapter draws into the game's backbuffer, which means the panel is
// baked into the eye image: it rides the view, so turning your head takes it
// with you. The wearer's words: "I really want this to be fixed in front of me
// and not pinned to my head."
//
// So in VR the panel is drawn into a PRIVATE texture instead, which xr_context
// copies into its own XrSwapchain and shows on a quad posed in LOCAL space --
// world-fixed, captured where the wearer was standing when they summoned it.
//
// This is the two-adapter architecture doing the one thing it was for: only the
// bound render target changes. Everything above it -- the registry, the staged
// edits, the nav, the panel itself -- is shared and was finished flat.
//
// Both are null / false until the panel has been opened at least once.
struct ID3D11Texture2D* MenuLayerTexture();
bool MenuLayerVisible();
