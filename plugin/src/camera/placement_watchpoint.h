#pragma once

// ---------------------------------------------------------------------------
// F1 -- FIND THE VIEWMODEL'S ONCE-PER-FRAME PLACEMENT WRITER.
//
// PLAN-CURRENT §3/F1. The viewmodel's animated bones are rebuilt every frame by
// SetupBones, ROOTED on the entity's placement:
//
//     C_BaseViewModel + 0x0128   origin  (3 floats)
//     C_BaseViewModel + 0x0114   angles  (3 floats)
//
// "+0x114 has 39 writers" is a static count over the whole module and does not
// say which of them writes THIS instance, nor at what cadence. This measures it
// on the live object: two hardware watchpoints, every writer's RIP recorded as
// a module-relative RVA, with a hit count and -- the part the static count
// cannot give -- hits per PRESENTED FRAME.
//
// The cadence is the whole deliverable. A once-per-frame writer is the
// placement; an interpolation or duplicate-copy writer shows a different rate
// or a different target field, and the two are told apart by the numbers rather
// than by their names.
//
// WHY THIS IS SAFE, and it is the same argument angle_watchpoint.cpp already
// banked on the camera angles:
//   - Nothing in game memory is written. Debug registers are per-thread CPU
//     state; the handler only reads Rip and clears the status bit.
//   - RW=01 (data WRITES only), so readers never fire and the noise floor is
//     the writers themselves. x86 offers no read-only watchpoint, which is why
//     this direction is the cheap one.
//   - Bounded: it disarms itself after a hit cap, and the debug registers are
//     cleared from every thread on the way out.
//   - The instance is revalidated by its vtable pointer before arming and again
//     at disarm, so a weapon switch or respawn that frees the object is
//     reported rather than silently producing a table of writers to whatever
//     was allocated there next.
//
// PASSIVE. It installs no detour, patches no code, and moves no float.
// ---------------------------------------------------------------------------

// Arms both watchpoints on the live C_BaseViewModel. Runs the bone scan first
// if no instance is cached yet -- that scan is a one-shot 4.7 GB walk, so this
// is a deliberate keypress and never automatic. Pressing again disarms and
// dumps the report.
void ToggleViewmodelPlacementWatchpoint();
bool IsViewmodelPlacementWatchpointArmed();

// SELF-TERMINATING, AND THAT IS A SAFETY PROPERTY AND NOT A CONVENIENCE.
//
// Every write to a watched field costs a vectored exception. If the placement
// turns out to be written from a hot loop rather than once a frame, the game
// drops to single-digit frames the instant this arms -- and the tester is then
// asked to hit a key to stop it, on a machine that has stopped responding at
// the speed keys are pressed. The capture therefore closes itself on a fixed
// wall clock, and the key is only needed to stop it EARLY.
//
// Called every plugin frame. Costs one atomic load when nothing is armed.
void AdvanceViewmodelPlacementWatchpoint();

// ---------------------------------------------------------------------------
// THE F1 RIDER (P0 §6): one-shot dump of every camera pass's full matrix.
//
// P0 left exactly one question open -- whether the gun is drawn through a pass
// carrying the world orientation or through a projection-only pass whose
// content is already in view space -- and it changes F3's prediction, not F2's
// mechanism. The answer is sixteen floats that are already in flight past a
// hook we already own. Read-only, one frame, no new seam, and it costs the F1
// run nothing: no extra keypress, no change to what the tester does.
// ---------------------------------------------------------------------------
void RequestCameraPassMatrixDump();
// True while a request is outstanding. The upload detour is only installed by
// things that ARM something, so on a flat run with nothing armed there is no
// hook to be read through -- this lets the per-frame tick install one for a
// read-only dump.
bool IsCameraPassMatrixDumpPending();
// Called from the camera upload hook for every camera-sized upload. Does
// nothing at all unless a dump was requested.
void OfferCameraPassMatrix(const unsigned char* bytes);
