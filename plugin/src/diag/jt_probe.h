#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// F0 -- THE SCHEDULER PRECONDITION, READ ONLY. STAGE-O-RESULT-2026-08-29 section 5.
//
// Nothing here writes game state, patches a byte, or changes a scheduler
// setting. It reads three exported tier0 counters, and -- at mode 2 -- swaps
// ONE vtable slot in client.dll's .rdata for a thunk that logs and tail-jumps
// to the original. The slot swap is the project's thrice-shipped interception
// class (Map, Unmap, RSSetViewports); the thunk changes no argument and calls
// the original exactly once.
//
// WHAT F0 EXISTS TO DECIDE. Stage O's verdict is `-numworkerthreads 1`, and
// every line of the reasoning behind it turns on facts this probe measures
// rather than assumes:
//
//   * that the switch is honoured at all (the positive control: a run WITHOUT
//     it must read a different count, or the run is VOID -- not "serialisation
//     failed");
//   * that the thread which would carry a same-frame re-entry is JT index 1,
//     the materialsystem_dx11 render thread with a registered message pump.
//     Stage O 1.5 says a thread with a pump cannot deadlock on a missed wake
//     and a thread without one can. If the index logs as anything else, that
//     reasoning is void for this thread and must be redone BEFORE F1.
//
// THE ARM LABEL IS READ AT THE MEASUREMENT, not at arm time: the probe logs
// the process command line beside the counts, so the run's own log says which
// arm produced the numbers.
// ---------------------------------------------------------------------------

// `jt.probe`. 0 off (the default -- nothing arms on load), 1 counters only,
// 2 counters plus the scene-draw caller census.
void SetJtProbeMode(int mode);
int JtProbeMode();

// Per-frame. Resolves the exports once, logs the banner once, and heartbeats.
// `worldReady` is the caller's own gameplay predicate, passed in rather than
// reached for: it lives in plugin.cpp's anonymous namespace and this probe has no
// business widening its linkage. Gating the census ARM on it (not the install)
// is what stops a capped diagnostic from sampling the loading screen.
void AdvanceJtProbe(bool worldReady);

// The current thread's JT index, or -1 when tier0 has not been resolved yet.
// Safe to call from any thread; it is one exported read with no lock.
int JtCurrentThreadIndex();

// The CViewRender job-group id (client+0xEA9EE4) and that group's refcount word
// in tier0's table. Returns false when either read is unsafe, which callers log
// rather than rendering as a zero. STAGE-O-RESULT section 2.2.
bool ReadViewJobGroup(std::uint32_t* idOut, std::int32_t* refCountOut);
