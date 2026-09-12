#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// F1 -- DOES THE NESTED CALL RETURN? PLAN-TRUE-STEREO section 4, under
// STAGE-O-RESULT-2026-08-29 and F0-RESULT-2026-08-29.
//
// This owns the one interception both rungs share: CViewRender's scene-draw
// vtable slot, client+0x9287F0, holding client+0x3723B0. F0 proved that slot is
// a clean seam -- one caller, one call per frame, zero unclassified, and a
// `this` that is a single static object -- so F1's caller gate is an exact
// pointer compare rather than a heuristic.
//
// THE SIGNATURE IS KNOWN, NOT GUESSED. client+0x3723B0 has zero direct callers
// and is reached only through this slot, so there is exactly ONE call site to
// read, and it is client+0x35AAE3:
//
//     35AAD0  mov rax, [rdi]              ; vptr
//     35AAD3  lea rdx, [rdi+0x12ECC0]     ; arg2, a member of `this`
//     35AADA  mov r9d, r14d               ; arg4
//     35AADD  xor r8d, r8d                ; arg3 -- a hardcoded ZERO
//     35AAE0  mov rcx, rdi                ; arg1 = this
//     35AAE3  call [rax+0x48]
//
// Four integer arguments, no stack arguments, no xmm setup, and rcx is `this`
// rather than a hidden return buffer. That is why this is a plain C++ hook and
// no longer an assembly thunk: with the signature read off the only site that
// exists, the thunk's ignorance bought nothing and cost the ability to wrap the
// call in SEH.
//
// arg3 is logged, never interpreted. A hardcoded zero at the only call site is
// suggestive next to R5's `PrepareSlot(outer, 1)`, and suggestive is not a
// reason to write a 1 into it.
//
// WHAT F1 DOES, AND ITS BLAST RADIUS. On one keypress it makes ONE extra call,
// on ONE frame, and disarms itself. Not N frames, not until toggled: one. That
// is the whole of the plan's question -- does the nested call return -- and a
// single shot cannot accumulate corruption while it is being asked. If it
// returns clean, a later rung earns the right to repeat it.
//
// It does NOT write a camera, substitute a render target, or balance the job
// group. STAGE-O section 2.2 says the second pass releases a job group its own
// pass never acquired, and F0 raised that from an inference to the leading
// hypothesis -- but it is still unproven, and the standing rule is that a fix
// never ships in the same run as the measurement that justifies it. F1
// MEASURES the job group across the nested call; it does not correct it.
// ---------------------------------------------------------------------------

// `jt.probe >= 2` asks for the read-only caller census (F0's instrument).
void SetSceneCensusWanted(bool wanted);

// `stereo.reentry = 1` makes the second call AVAILABLE. It still does nothing
// until the key arms one shot, so an ini left at 1 costs a log line, not a run.
void SetSceneReentryWanted(bool wanted);

// THE PASS TAG, shared with camera_update_hook. 0 = outside any instrumented
// pass, 1 = the engine's own scene draw, 2 = the nested one. Thread-local, so
// only work issued BY the re-entering thread inside the pass is attributed to
// it -- which is the only attribution that survived the burst.
extern thread_local int t_passTag;

// `stereo.reentry_frames`. How many consecutive doubles ONE press arms.
// Default 1. A burst is what separates the game's per-frame variance from the
// mechanism's, and it repeats the guard once per shot.
void SetSceneReentryFrames(int frames);

// `stereo.reentry_guard`. Suppresses ONLY the nested pass's repeat release of an
// id already ended -- the exact call three runs measured. Default off.
void SetSceneReentryGuard(bool on);

// R2. `stereo.restore_latch`, DEFAULT ON. Restores the once-per-frame latch at
// this+0xF1C80 between the two passes, using the bytes snapshotted before pass
// 1 -- never a computed value. Off is the A/B control arm.
void SetSceneLatchRestore(bool on);

// R2b. `stereo.recompute_blocks`, DEFAULT ON. Runs client+0x36A060 on the arg2,
// origin and sky view blocks before pass 2 -- the caller pre-draw work O1
// enumerated and no rung has ever replicated. Off is the control arm.
void SetSceneRecomputeBlocks(bool on);

// R4. `stereo.own_job_group`, DEFAULT ON. Pass 2 begins its own job group and
// publishes it in client+0xEA9EE4 around the nested call. Off is the control.
void SetSceneOwnJobGroup(bool on);

// The key. Arms exactly one nested call on the next qualifying frame.
void ArmOneSceneReentry();

// Per frame: installs the hook, arms the census once a world is up, heartbeats.
void AdvanceSceneReentry(bool worldReady);

// For the status line and the crash recorder's arm bits.
bool SceneReentryArmed();

// True while a nested call is armed for the CURRENT frame, read from the render
// thread by the mid-frame capture. Not the same as SceneReentryArmed(), which
// asks whether shots remain.
bool SameFrameDoubleArmed();
void SceneReentryWatchdogTick();
void ToggleSceneDrawEpilogue();

// R8. `stereo.prewait_job`, DEFAULT ON. Polls the engine's own job-completion
// test before pass 1 and, once the job is provably done, writes 0 into
// this+0xF1C80 so the scene draw takes its own sanctioned skip-the-wait path.
// Semantically a no-op -- when the generations differ the engine's wait returns
// immediately anyway -- but it cannot lose a wakeup, because it never parks.
void SetSceneJobPreWait(bool on);

// R9. `stereo.prewait_ms`, DEFAULT 250. How long the pre-wait drain may poll
// before giving up and letting the engine wait as it always has. R8b measured
// the hung frame as the ONE frame whose job did not complete within 12 ms.
void SetSceneJobPreWaitMs(float ms);
