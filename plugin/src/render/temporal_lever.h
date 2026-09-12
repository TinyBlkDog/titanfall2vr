#pragma once

// T-A: THE ENGINE'S OWN TEMPORAL LEVER, AND THE CENSUS THAT PROVES IT TOOK.
//
// The open front is that batch 2's frame duplicates once its camera differs
// from batch 1's, while at offset 0 it is pixel-perfect and the draw counts are
// identical armed against control. Nothing is culled, so the defect is in the
// RESOLVE. The leading mechanism is that the two batches share one temporal
// history: batch 1 resolves into it, then batch 2 resolves into it again from a
// different camera, and eight doubled frames interleave two viewpoints in one
// buffer.
//
// This file does not fix that. It TESTS it, with the engine's own controls
// rather than memory surgery, which is the whole reason it is the first rung:
// if forcing the resolve to the current frame alone cleans the eye up, the
// mechanism is confirmed and per-eye temporal resources are the fix. If the
// copies survive with the history weighted out, temporal reuse is refuted as
// the primary cause and this front merges with M4.
//
// THE LEVER IS `tsaa_curframeblendamount`, and its polarity is not a guess: it
// is the CURRENT frame's weight in the temporal blend, it ships at 0.05 -- 95%
// history -- and driving it to 1.0 is "use this frame and nothing else". The
// other candidates were rejected on purpose and the reasons are at the
// definition of kCensus.
//
// It goes through `RunEngineConsoleCommand`, the engine's own Cbuf, not through
// `TrySetCvarFloat`. The direct setter writes the value slots and fires no
// change callback, which for a material-system render flag is exactly the way
// to get a lever that reads back perfectly and does nothing.

// Arms / disarms the current-frame-only resolve. Experiment slot C this build.
void ToggleTemporalCurrentFrameOnly();

// For the capture filename. A run that photographs both arms into one name has
// no evidence in it; this project has already lost a run that way.
bool TemporalCurrentFrameOnlyArmed();

// READ ONLY. Every name in the temporal family, its registered-or-not state and
// its live value, logged at the moment the burst fires. A convar that was never
// registered silently swallows its set, and that is a VOID run rather than a
// null -- so the census is what tells the two apart afterwards, from the run's
// own log, without a second run.
void LogTemporalCvarCensus(const char* when);
