#pragma once

#include <d3d11.h>

// THE LOAD-CRASH INSTRUMENT. Log-only, and it changes nothing about what the
// game renders.
//
// The 2026-08-23 decouple crash is the game calling ID3D11DeviceContext::Map
// with a NULL pResource, loaded out of one of its own structures, on a tier0
// worker thread at LevelInit. The leading candidate for how a null gets into
// that structure is a resource CREATION that failed and whose HRESULT was not
// checked. Nothing in the log could have shown that, so nothing did.
//
// This swaps three slots of the game device's vtable -- CreateBuffer (3),
// CreateTexture2D (5), CreateShaderResourceView (7) -- and logs any call that
// returns a FAILED HRESULT, with what was asked for. Slot swaps, not entry
// detours, per the standing rule: a data write rather than a code patch, no
// prologue to recognise and no trampoline.
//
// A failure appearing just before the crash names the resource and turns "why
// is a resource null" into "why did this creation fail", which is tractable.
// No failure eliminates the candidate cleanly, which is worth as much.
bool InstallResourceWatch(ID3D11Device* device);

// Per-window health, called from the frame beat. Reports what a creation
// failure would most likely be caused by, and whether the device has already
// died without the game having noticed yet:
//   * IDXGIAdapter3::QueryVideoMemoryInfo -- budget against current usage
//   * ID3D11Device::GetDeviceRemovedReason -- non-zero BEFORE the game reports
//     it is the difference between "we removed it" and "we noticed it"
void LogResourceHealth(const char* tag);

// THE OVERLAP COUNTER, and it is here rather than in the pacing thread on
// purpose.
//
// The minidump caught the pacing thread asleep, which proves less than it
// looks: at roughly 40 fills a second of about 0.01 ms each, the pacer is
// inside an XR call about 0.04% of wall time, so a single snapshot finds it
// asleep whether or not it races. One sample cannot answer this question.
//
// These count it properly instead. The pacer marks its XR calls, and the
// create hooks -- which run on the very worker threads that crashed -- check
// the mark. A non-zero overlap is direct evidence of the game touching D3D
// while the runtime is inside an XR call on the same device; a zero overlap
// over a whole load retires the concurrency theory on measurement rather than
// on one stack.
void NotifyPacerEnterXr();
void NotifyPacerLeaveXr();

// Enables ID3D11Multithread on the game's immediate context, and logs the
// vtable pointer either side so the swap is a measured fact. Must be called at
// swapchain adoption, before anything resolves a context vtable.
bool MaybeEnableMultithreadProtection(ID3D11Device* device);
// Called from the game's own immediate-context work. See the note in the .cpp:
// device creation is free-threaded, the immediate context is not, and only this
// counter measures the illegal pattern.
void NoteGameContextCall();
void LogMultithreadStatus();

// xr.mt_protect, DEFAULT 0. Off because it is measured harmful: enabling it
// moves the implementation behind context vtable slot 48 to a prologue
// PrepareDetour does not recognise, so the camera hook never installs, stereo
// never arms, and the session renders flat.
void SetMultithreadProtectionWanted(bool wanted);
