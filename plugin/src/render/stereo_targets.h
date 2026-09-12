#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

// ---------------------------------------------------------------------------
// M1 -- THE RENDER-TARGET CENSUS. REPLAN-TRUE-STEREO-2026-08-29 section 3, M1.
// Read-only. It changes no argument, substitutes no target, and returns
// whatever the runtime returns.
//
// WHY THIS EXISTS. F1 established that client+0x3723B0 SUBMITS: it issues no
// D3D on its own thread (66,843 camera uploads outside the tagged passes
// against zero inside either), and the render thread later executes BOTH
// batches, in order, into ONE target -- which is why the wearer can see crates
// through the weapon on a doubled frame. Separating the two eyes therefore
// needs one thing this project does not have: a predicate the RENDER THREAD
// can evaluate that says "this D3D call belongs to batch 2".
//
// The last attempt at that predicate guessed -- "the second scene-sized
// viewport of the frame" -- and was wrong: the peak is 39 full-resolution
// viewport sets per frame, because the post-process passes run at full
// resolution too. This rung stops guessing and measures the sequence.
//
// WHY SLOT 33 IS ALLOWED. REVIEW-STEREO-2026-08 section 2.1 re-scoped the
// 2026-08-14 closure: what faulted the NVIDIA driver was replacing the
// context's vptr and swapping the Draw/DrawIndexed slots (12/13). Slot swaps
// on OTHER slots of this per-object table have shipped three times --
// Map (14), Unmap (15), RSSetViewports (44) -- at millions of calls a session
// without a fault. OMSetRenderTargets is slot 33, and the index is corroborated
// three ways in this build: 12 = DrawIndexed, 44 = RSSetViewports and
// 48 = UpdateSubresource all sit where the same arithmetic puts them.
//
// It is introduced exactly the way Map/Unmap were: census first, argument
// substitution (M2) only after the census has run clean.
//
// WHY THE IMMEDIATE CONTEXT IS THE RIGHT OBJECT, and this is measured rather
// than assumed: the 66,843 untagged camera uploads F1 counted were all gated on
// `context == g_gameContext`. The thread that executes the queued batches uses
// the immediate context, so its OMSetRenderTargets calls come through this
// table.
// ---------------------------------------------------------------------------

// `stereo.stereo_targets`. 0 = off (nothing is hooked), 1 = on.
void SetRtvCensusWanted(int mode);
bool RtvCensusWanted();

// Installs the slot swaps on the game's immediate-context vtable. Called from
// the camera detour install, which already holds the verified context and its
// per-object table, and which scene_reentry gates on a world being up.
//
// REFUSES if a slot already holds ours -- reading our own stub back as the
// original builds a call that recurses into itself, the trap the viewport swap
// already names.
void InstallRtvCensus(ID3D11DeviceContext* context, void** vtable);

// Puts back exactly what was taken, and only if the slot still holds ours.
void RemoveRtvCensus();

// THE PRESENT-INTERVAL BOUNDARY. Called from the Present hook, before the real
// Present. Closes the interval, decides whether it is worth dumping, and dumps
// it OUTSIDE the census lock so the render thread is never blocked on file I/O.
void RtvCensusOnPresent();

// Called by scene_reentry once per doubled frame, after pass 2's submission has
// returned. The census cannot read `g_frameIsDoubled` and believe it: that flag
// is raised and lowered on the SUBMITTING thread, and the batches it describes
// execute later, on the render thread, in a Present interval that may already
// have moved on. A monotonic count can be compared at both ends of an interval,
// which a transient flag cannot.
void RtvCensusNoteDoubledSubmission();

// ---------------------------------------------------------------------------
// M2 -- SEPARATION. The one behavioural change: while the census says batch 2,
// the colour target batch 2 would scribble on is replaced with a private one.
//
// M1 measured what batch 2 actually does, and it is not what the design
// assumed. Batch 2 is a STRUCTURALLY COMPLETE replay of batch 1 -- shadows,
// scene, lighting, volumetrics, exposure, tonemap, HUD, every intermediate
// doubling exactly -- with one substitution of its own: everywhere batch 1
// binds the HDR scene target, batch 2 binds the LDR COMPOSITE that batch 1's
// tonemap just wrote. The engine reads its scene colour target from state
// batch 1 advanced. So batch 2 renders the whole frame again on top of the
// finished picture, which is precisely why the wearer can see crates through
// the weapon.
//
// So the substitution is one target, not a set, and it is identified per frame
// rather than by an address: the victim is the rtv0 of the last transition
// before batch 2 opens -- batch 1's own final composite.
//
// THE BATCH-2 MARKER NEEDS NO TARGET IDENTITY EITHER. A frame contains exactly
// one OMSetRenderTargets with no RTV and a DSV bound (the shadow atlas). A
// doubled frame contains exactly two. Measured across 12 single-batch and 16
// doubled intervals at two locations: 1 and 2 respectively, no exceptions in
// either direction. The second one opens batch 2; Present closes it.
//
// FALSIFIER, visible at a glance: the crates artefact disappears -- the weapon
// is opaque again in a doubled frame.
// POSITIVE CONTROL: F5 disarms it mid-session and the artefact returns.
// ---------------------------------------------------------------------------

// `stereo.substitute`. 0 = census only (M1 behaviour), 1 = substitute.
void SetRtvSubstituteWanted(int mode);
// The positive control's key. Toggles the substitution without disturbing the
// census, so one session carries both arms.
void ToggleRtvSubstitute();
bool RtvSubstituteArmed();

// The burst's proof pair, asked for by scene_reentry on the first shot of a
// press and serviced inside Present so both images come from ONE frame.
void SetPreDrawCameraIpd(float ipd);
void SetPreDrawCameraBind(int mode);
unsigned long long RtvPreDrawWrites();
unsigned long long RtvPreDrawDeclines();
void SetRtvReadSubstituteWanted(int mode);
void ToggleRtvReadSubstitute();
bool RtvReadSubstituteArmed();
// T-A0: THE SHOT NUMBER TRAVELS WITH THE REQUEST. Capturing shot 1 AND shot 8
// of one burst is what separates a history that FEEDS BACK (T1/T2 -- shot 8 is
// worse than shot 1) from a double-write inside one frame (T3 -- the two are
// identical). Two files with the same name would have answered neither.
void RtvCensusRequestBurstCapture(int shot);
// Which shot of the burst the pending capture is OF, for the filename.
int RtvCensusBurstCaptureShot();
bool RtvCensusBurstCaptureWanted();
void RtvCensusClearBurstCapture();
// How many substitutions happened in the interval the pair was photographed IN.
// The image carries its own arm label: an ARMED capture reading 0 here is void.
int RtvCensusCapturedIntervalSubstitutions();
// The private per-eye target, or null before it has been created.
struct ID3D11Texture2D* RtvCensusPrivateTexture();

// M3. True while the render thread is executing batch 2 of a doubled frame.
// This is the discriminator the whole replan turned on, and the camera key is
// keyed on it: the replan said B1 (the pre-call engine-struct write) goes first
// ONLY if no structural discriminator exists. M1 found one and M2 has leaned on
// it 509 times without a misidentification, so B2 -- the shipped
// c_cameraOrigin lever, relocated to where the uploads actually happen -- is
// available now, and it needs no memory surgery on an engine struct.
bool RtvCensusInBatch2();
// The index of the transition span currently open, for ordering an upload
// against the draws around it. Read-only.
int RtvCensusCurrentSpanIndex();
// N1-Q1's ordering clock: the VSSetConstantBuffers ordinal reached so far
// inside the CURRENT batch. Finer than the span index and finer than a per-draw
// tally (1782 binds against 147 scene draws), and it is what stands in for the
// DrawIndexed count the plan asks for -- slots 12/13 fault the driver, so that
// tally cannot be built. Read-only.
unsigned long long RtvCensusCbBindsInBatch();

// Per plugin frame: creates the private target once the census has identified
// the composite, and heartbeats the substitution's counters. Deliberately NOT
// done inside the hook -- a resource creation on the render thread at a render-
// target transition is a bigger thing to have to defend than a tick.
void TickRtvCensus();
