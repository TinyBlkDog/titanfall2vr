#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// R3 -- THE STALL WATCHDOG. Names the instruction the process dies on.
//
// WHY THIS RUNG EXISTS. Three arms have now hit the same wall and the log
// ordering says something none of them assumed:
//
//     R1 pool DOUBLED #129 (pre-pass1) / (post-pass1) / (post-pass2)
//     R1 pool DOUBLED #130 (pre-pass1)          <- and nothing ever again
//
// `post-pass1` prints for every doubled frame inside the 160-frame window, so
// its absence on frame 130 is not a cap and not a flush: **PASS 1 NEVER
// RETURNED.** The call that hangs is the ENGINE'S OWN scene draw, unmodified,
// this run's control -- not our nested pass.
//
// That retires the whole "make pass 2 look like a first pass" family as an
// explanation, which is exactly what the epilogue (refuted), the latch
// (measured harmful) and the block recompute (refuted, wall unmoved at 129/130)
// each failed to move. The re-entry poisons something over 129 frames and then
// the engine's own draw deadlocks on it.
//
// So the question stopped being "what do we skip" and became "where does it
// block", and that is answerable: `client+0x3723B0` is 5168 bytes and the
// stalled thread's RIP names the instruction outright.
//
// WHY A SEPARATE THREAD, AND IT IS THE WHOLE POINT. A probe driven by the
// thread that stalls cannot report the stall -- it is stalled. This runs on its
// own thread, holds a duplicated handle to the thread inside the scene draw,
// and when a section has been open too long it suspends that thread, reads its
// context, resolves RIP to module+RVA, walks the stack conservatively for
// return addresses that land inside a loaded module, resumes it, and logs. The
// process is already dead at that point; suspending it costs nothing and buys
// the one fact three runs of guessing did not produce.
//
// Read-only with respect to game state: it reads a thread context and stack
// memory. It writes nothing, patches nothing, and fires only after a section
// has been open for kStallMs -- which is ~4000x a healthy pass 1 (0.5 ms) and
// ~95x a healthy pass 2 (21 ms), so it cannot fire on a slow frame.
// ---------------------------------------------------------------------------

// Starts the watchdog thread. Idempotent; safe to call every frame.
void StartStallWatchdog();

// Bracket a section that must not block. `pass` is 1 or 2. Open records the
// calling thread, the frame number and the time; Close clears it. The FIRST
// open also captures the thread handle the dump will use.
void StallSectionOpen(int pass, unsigned long long frame);
void StallSectionClose();

// Logged from the continuous heartbeat so the run says the watchdog is alive
// and armed rather than merely compiled in.
void LogStallWatchdogStatus();
