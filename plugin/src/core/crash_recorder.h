#pragma once

// ---------------------------------------------------------------------------
// THE CRASH RECORDER. PLAN-TITAN-2026-08-27 P0-b.
//
// "Today, every crash destroys its own evidence." The titan crash left nothing
// behind: no faulting address, no module, and -- the part that costs runs --
// no record of WHICH OF OUR FEATURES WERE ARMED when it happened. That turns
// one attribution run into five bisection runs.
//
// This writes ONE line when the process faults:
//
//     faulting address, the module it lands in, the RVA inside that module,
//     and our arm state as a list of names.
//
// and then gets out of the way. It changes nothing about how the game or
// Northstar handle the fault.
// ---------------------------------------------------------------------------

// Installs both recording routes. Safe to call more than once; installs once.
//
// TWO ROUTES, BECAUSE ONE OF THEM MAY NEVER FIRE -- and a handler never seen to
// fire is not evidence when it stays silent.
//
// The plan says "an unhandled-exception filter ... then chain to the previous
// handler", which is SetUnhandledExceptionFilter. Checked against Northstar's
// own source before building it: `primedev/logging/crashhandler.cpp` installs a
// FIRST-CHANCE VECTORED handler (`AddVectoredExceptionHandler(TRUE, ...)`) that,
// for a fatal exception, formats a callstack, writes a minidump, shows a message
// box and can `ExitProcess`. All of that happens BEFORE any unhandled-exception
// filter is reached, and the ExitProcess path means ours might never be reached
// at all. Registering only a UEF would have been the dead-probe failure again.
//
// So the primary recorder is a vectored handler of our own. Ours is registered
// last, so it goes to the FRONT of the chain -- ahead of Northstar's -- and it
// records before anything else has a chance to end the process. It ALWAYS
// returns EXCEPTION_CONTINUE_SEARCH: it observes and passes the exception on
// untouched, so Northstar's crash handler and the game's own SEH see exactly
// what they saw before.
//
// The unhandled-exception filter is kept as the second route, chaining to the
// previous filter exactly as the plan asks. Each line names which route wrote
// it, so the run itself tells us which one is the live one.
void InstallCrashRecorder();

// Publishes what is armed RIGHT NOW, as a bitmask, from the game tick.
//
// THE HANDLER READS A NUMBER, NOT THE FEATURES. Calling twenty getters from
// inside a crashed process is a good way to fault inside the fault -- and one
// of them (IsXrDecoupled) takes the XR mutex, which the crashing thread may
// already hold. So the tick does the asking, once per frame, into one atomic;
// the handler only decodes it against a static table.
void PublishArmStateForCrashRecorder();

// Faults on purpose, to prove the recorder works. The falsifier for P0:
// "a deliberate test fault behind a bare key must produce the line and the
// rotated file."
//
// Gated on `crash.selftest = 1` in the ini and inert without it -- this kills
// the game, and a key that kills the game must not be reachable by a stray
// press in a real session. Returns false and says so if it was not armed.
bool TriggerCrashRecorderSelfTest();
void SetCrashSelfTestArmed(bool armed);
