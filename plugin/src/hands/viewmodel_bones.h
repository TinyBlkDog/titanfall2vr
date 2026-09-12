#pragma once

#include <cstddef>

// TASK 05 ROUTE B -- FIND THE VIEWMODEL'S BONES, WITHOUT A DISASSEMBLER.
//
// Every cvar-based diagnostic is now closed with evidence: sv_showfiredbullets,
// cl_showfiredbullets, bulletPredictionDebug and cl_ShowBoneSetupEnts all produce
// verified silence, the echo control proves the capture path works, and they stay
// silent even with sv_cheats 1 and enable_debug_overlays 1 (both confirmed to take
// by readback). The overlay drawing is compiled out of the retail client. Those
// cvars exist and are inert.
//
// What is left is the vtable anchor, resolved offline from client.dll's RTTI:
//
//     C_ViewmodelAttachmentModel primary vtable = client.dll + 0x8B8158
//
// An object whose first eight bytes equal that address IS an instance. So rather
// than reverse-engineering the entity list to reach one, this SCANS FOR ONE --
// walk committed read-write private memory, look for the vtable pointer, and
// every hit is a live viewmodel attachment model.
//
// Then, from an instance, look for the bone array. A bone-to-world array is
// recognisable without knowing its offset: it is a run of matrix3x4_t whose 3x3
// parts are orthonormal (unit-length, mutually perpendicular rows) with
// translations in a plausible world range. That signature is specific enough that
// random heap bytes essentially never satisfy it several times consecutively.
//
// PASSIVE. It reads memory and logs. It writes nothing, patches nothing, and
// hooks nothing -- which matters because a heap scan is already the most
// intrusive thing here and it should not also be changing state.
//
// Hotkey-gated and one-shot, because it is a bounded scan of the process address
// space and has a real CPU cost. Everything about it is capped: bytes scanned,
// regions visited, instances recorded, and candidate arrays reported.
void RequestBoneProbe();
void AdvanceBoneProbe();

// ---------------------------------------------------------------------------
// TASK 05 STEP 4 -- THE BONE WRITE.
//
// The scan found the arrays and they cross-check each other:
//
//     C_BaseViewModel            + 0x1870   87 bones
//     C_ViewmodelAttachmentModel + 0x1260   74 bones
//
// stored INLINE, in WORLD space (their first translations are map coordinates,
// and both classes report the same position to five hundredths of a unit).
//
// The attachment's 74 bones ARE the weapon, so transforming all of them rigidly
// moves the gun without first having to identify a hand bone inside the 87-bone
// rig. The arms will not follow -- that is the known and expected cost of a rigid
// drive, and the thing a run has to judge.
//
// WHY THE SCAN IS NOT REPEATED PER FRAME. It walks 4.7 GB and takes seconds. The
// instance is cached instead and revalidated cheaply every write by re-reading
// its vtable pointer; a weapon switch or respawn that frees the object shows up
// as a vtable mismatch, and the drive stops rather than writing into freed
// memory.
// ---------------------------------------------------------------------------

// Caches the instances. Runs the same exhaustive scan, so it is a one-shot with
// a real cost, and it is what the pin arms through.
bool CacheWeaponBoneInstance();

// Applied from the viewmodel camera pass -- per render frame, immediately before
// the viewmodel draws, which is after the engine's bone setup and before anything
// consumes it. Does nothing unless armed.
void ApplyWeaponBonePin();

void SetWeaponBonePinEnabled(bool enabled);
bool IsWeaponBonePinEnabled();
void RecentreWeaponBonePin();

// Cycles which skeleton the pin drives. Naming does not settle which array is
// which -- driving the attachment moved the whole body and left the gun alone --
// so both classes and both instances are enumerated and stepped through by key
// until the GUN is what moves.
void CycleWeaponBoneTarget();

// ---------------------------------------------------------------------------
// What the scripted flat session drives. Same pin, same seam, same telemetry --
// it only removes the human from the cycling, because eight keypresses timed by
// hand is a test nobody can run twice the same way.
// ---------------------------------------------------------------------------

// -1 is ALL. How many arrays the scan found.
int WeaponBoneTargetCount();
void SetWeaponBoneTargetIndex(int index);
// Keeps the pin armed but stops it writing, so the engine's own recomputation
// restores the authored pose. That is what puts a visible STILL beat between
// two spinning phases, and it is what makes a still/spin capture pair of the
// same target comparable.
void SetWeaponBonePinPaused(bool paused);
bool IsWeaponBonePinSynthetic();
// Forces the drive after arming. The flat session calls this rather than
// trusting the arm-time choice: a Quest sitting powered on at the desk reports
// tracked controllers, the pin would pick the hand drive, and a session whose
// entire deliverable is a spin would produce nothing while looking healthy.
void SetWeaponBonePinSyntheticDrive(bool synthetic);

// ---------------------------------------------------------------------------
// WITHIN-FRAME SURVIVAL. THE QUESTION THE OTHER COUNTER CANNOT ANSWER.
//
// The survival telemetry beside the write compares the array against our last
// write on the NEXT view-build pass -- a frame later. An engine that recomputes
// bones every frame therefore reads 100% OVERWRITTEN whatever seam we write
// from, which is exactly what the first run reported, and it says nothing about
// whether the write survived long enough to be DRAWN. Reading the decision
// table off that number would have sent the next change to the wrong place.
//
// This is the same comparison made at the RENDER side, downstream of the view
// build, in the same frame -- the old camera-upload seam, now used read-only for
// exactly the measurement it was never doing when it was a write site. Match
// means our write reached the renderer intact, so a still gun means the renderer
// reads a different buffer (step 4 fallback). Mismatch means the engine
// recomputed between the two seams, so the write has to happen later.
//
// Read-only. It writes nothing and it is what makes the next change attributable.
void ProbeWeaponBoneWriteSurvival();
// The render-side write, called from the viewmodel camera upload when that seam
// is selected. Not once-per-frame gated: the write is absolute now, so applying
// it on every viewmodel pass is idempotent and maximises the chance of being in
// place whenever the draw actually reads the array.
void ApplyWeaponBonePinAtRenderSide();

// ---------------------------------------------------------------------------
// WHICH SEAM THE WRITE FIRES FROM. 0 = view build, 1 = render side.
//
// The flat run of 2026-08-18 measured the view-build write as gone by the time
// the render side ran, on 99.2% of passes for the 87-bone array and 88.2% for
// the 74 -- so the engine's own bone setup happens BETWEEN those two points, and
// the render side is therefore post-SetupBones by observation rather than by
// hope. That is where the decision table sends the write next.
//
// It is a switch and not a move because this project's own rule is to prefer an
// A/B inside one run over a judgement across runs: the flat session sweeps both
// seams against every target, so "later seam is better" is measured here rather
// than remembered from last time. The default is the render side, which is the
// one the evidence points at.
constexpr int kBoneSeamViewBuild = 0;
constexpr int kBoneSeamRenderSide = 1;
void SetWeaponBoneWriteSeam(int seam);
int WeaponBoneWriteSeam();
const char* WeaponBoneWriteSeamName();
// One line of survival telemetry for a target, for the session's end-of-run
// summary. Returns false if the slot is out of range.
bool DescribeWeaponBoneTarget(int slot, char* out, size_t size);
// The array pointer and bone count of a drivable target, for the staging probe's
// cross-check. Returns false for a slot that is out of range or retired.
bool GetWeaponBoneTargetArray(int slot, const void** array, int* boneCount,
                             const char** className);
