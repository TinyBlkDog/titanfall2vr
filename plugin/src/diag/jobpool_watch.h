#pragma once

// ---------------------------------------------------------------------------
// R1 -- THE JOB-GROUP POOL WATCH. READ ONLY. `docs/O1-O2-RESULT-2026-08-31.md`.
//
// The 128-frame hang is a pool exhaustion and O2 found the pool offline, in
// tier0's job-group slot allocator, which nobody here had ever disassembled:
//
//   * `JT_BeginJobGroup` (tier0+0x6B90) takes its slot from tier0+0x5670.
//   * That allocator pops from a PER-THREAD free ring, 16 deep
//     (tier0+0x6E140 + jtIndex*0x40, head word +0x1C, tail word +0x1E, & 0xF).
//   * When the local ring is empty it refills 8 slots from a GLOBAL store of
//     BATCHES whose head and tail are two 7-bit fields packed into the single
//     qword at tier0+0x6EA80 -- so the ring holds 128 batches and the
//     initializer (tier0+0x8210) seeds it head=0, tail=127.
//   * 127 batches x 8 = 1016 free slots out of 1024 records at tier0+0x74F00.
//   * When BOTH are empty the thread does not crash. It calls tier0+0x7310
//     with the predicate at tier0+0x6440 and PARKS in the JT help/sleep loop
//     waiting for a slot that never arrives. A whole-process hang with the
//     render thread asleep and nothing logging again -- exactly what the run
//     that died at 129 doubled frames recorded.
//
// So there is one number to watch and its resting value is known in advance:
// batchesFree, 127 at process start. This instrument reads it, decodes it, and
// prints it from INSIDE the doubled frame.
//
// WHAT MAKES IT FALSIFIABLE, and each of these is a rung a previous run died
// on rather than a nicety:
//
//   * ITS OWN POSITIVE CONTROL IS THE RESTING VALUE. 127 is not a guess, it is
//     the constant the initializer stores. A first sample that is not a
//     plausible fraction of 127 means the address or the decode is wrong, and
//     the instrument says INSTRUMENT VOID in the log rather than printing a
//     zero that would read as a finding.
//   * EVERY LINE CARRIES THE ARM TIMESTAMP alongside its own. A line whose
//     sample predates the arm is labelled VOID in the log itself.
//   * IT PRINTS TO 160 DOUBLED FRAMES, not 100 and not 128 -- a cap that
//     coincided with the wall would answer nothing.
//   * THE CONTROL IS FREE AND IN THE SAME RUN. F6 latches doubling, so the run
//     walks undoubled then doubled, and undoubled frames print the same line.
//     A pool that drains identically without doubling is not our resource.
//
// It also carries three riders, all read-only and all answering a hypothesis
// the plan raised, so one run retires the whole list:
//   P2  `this+0xF1C80` -- the read-and-cleared latch inside the scene draw.
//   P4  client+0x2777A58 / +0x2777A5C -- the handle pool of PLAN-128 section 1.1.
//   the per-thread ring occupancy, which separates "this thread is starving"
//   from "the global store is dry".
//
// Nothing here writes game state. There is no INI key and no key binding on
// purpose: a read-only watch that has to be switched on is a watch that will
// be found switched off.
// ---------------------------------------------------------------------------

// Resolves tier0/client once and logs the banner plus the first reading. Safe
// to call repeatedly; only the first does work. Returns false when the pool
// could not be located, in which case every later sample says so.
bool JobPoolWatchReady();

// Called the moment doubling arms, so every later line can be compared against
// it. Idempotent per arm.
void JobPoolWatchNoteArm(const char* why);

// ONE SAMPLE, PRINTED FROM INSIDE THE ACTIVITY IT MEASURES. `where` names the
// point in the frame ("pre-pass1", "post-pass2", "undoubled"); `doubled` picks
// which ordinal the line is counted and capped against.
// `newFrame` must be true on exactly ONE sample per frame -- the first. The
// printed ordinal is then the FRAME number, which is the thing that has to be
// read against 128; a sample ordinal would silently be 2-3x the frame count and
// would put the cap and the wall in different places.
void JobPoolWatchSample(const char* where, void* viewRender, bool doubled, bool newFrame);

// The end-of-run summary: first reading, last reading, the low-water mark, and
// the drain rate per doubled frame. Logged from the burst summary.
void JobPoolWatchSummary(const char* tag);

// R7. Decodes a JobID against tier0's record table into `out` -- id, slot,
// refcount, flags, parent, generation -- refusing any id lacking the 0x3000 tag
// JT_BeginJobGroup stamps on every id it returns. Exposed so the stall watchdog
// can report the state of the job the scene draw is blocked on AT THE MOMENT OF
// THE STALL: every per-frame sample is taken BEFORE the wait, this one DURING.
void JobPoolWatchDescribeJob(unsigned int id, char* out, unsigned long long outSize);

// The id pass 1 was about to wait on, recorded at pre-pass1 by the hook.
void JobPoolWatchNoteWaitedJob(unsigned int id);
unsigned int JobPoolWatchLastWaitedJob();

// R8. Is this job COMPLETE, by the engine's own test? tier0+0x95E0 decides
// whether to wait at all by comparing the id's generation against the live
// record's, both masked with 0xFFFFC000 (`and ebp, -0x4000` at tier0+0x9608):
// a MISMATCH means the slot has moved on, the job is done, and the wait returns
// immediately at tier0+0x962B. Returns 1 = complete, 0 = still outstanding,
// -1 = cannot tell (unreadable, or an id lacking the 0x3000 tag).
int JobPoolWatchJobComplete(unsigned int id);

// R10. The ENGINE-side job fence, which is where the stall actually is.
//
// R9 refuted the client-side drain: at a 250 ms budget every one of 127 doubled
// frames verified its job complete before pass 1 (`timedOut=0`) and it STILL
// hung at 130. Re-reading the stall stack BY OFFSET rather than by order --
//
//   +0x0E0 tier0+0x75BE  +0x160 tier0+0x9680  +0x1A0 engine+0xB6CB3
//   +0x1F0 client+0x374917  +0x220 client+0x375753
//
// -- shows client+0x374917 is the return of `call [rax+0x30]` at client+0x374914
// (a virtual call on the [C3D950] singleton), NOT of the JT wait at 0x3748C1,
// which would return to 0x3748C7. So the blocking wait is one frame DEEPER than
// assumed, inside engine.dll, on a job we never touched.
//
//   engine+0xB6CAD  JT_WaitForJobAndOnlyHelpWithJobTypes([12208E94], 0, -1)
//   engine+0xB6CB3  <- parked here
//   engine+0xB6CB5  JT_BeginJobGroup -> stored back into [12208E94]
//
// and the entire block is gated on `cmp dword [12208EA4], 0` at engine+0xB6C3F,
// which the same function sets to 1 at +0xB6C6F. A SECOND call in one frame
// therefore skips the rotation entirely -- which is exactly what doubling the
// scene draw does.
//
// These are read-only and they name the mechanism if it is real: the gate flag,
// the three rotating job ids, and the fence counter.
void JobPoolWatchDescribeEngineFence(char* out, unsigned long long outSize);
