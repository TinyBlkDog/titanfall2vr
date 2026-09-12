#pragma once

// ---------------------------------------------------------------------------
// BONE READER WATCHPOINT -- read-only, one shot, the hands-by-mesh front
// (2026-09-05).
//
// THE QUESTION. The studiorender.dll draw path is switched off by a global
// (its DrawModel and dispatcher both bail on [x+0x5C] == 0, and the observer
// on its per-mesh material gate counted zero calls across a whole run), and
// no other module reads the studiohdr's mesh or skin tables at draw time. So
// the live model draw consumes structures built at load, and static analysis
// has not found it. What it MUST consume every frame is this model's bone
// matrices: the validated arms bone array at *(instance+0x1008) that
// SetupBones writes and arms_collapse rewrites. Whoever reads it is the live
// path for this model, by construction.
//
// THE INSTRUMENT. One hardware watchpoint (DR0, RW=11 read-or-write, 8
// bytes) on the first matrix of that array, every thread covered, every hit's
// RIP and [rsp] recorded as module+RVA with a count and the thread id. It
// arms itself once the arms hook has validated a target, closes on a wall
// clock or a hit cap, and reports once. The SetupBones writer (client.dll)
// is the positive control: a report without it is an instrument failure.
//
// PASSIVE, as angle_watchpoint / placement_watchpoint before it: nothing in
// game memory is written, no code is patched, the debug registers are
// cleared from every thread on the way out.
// ---------------------------------------------------------------------------

// Per plugin frame. Costs one load when nothing is pending.
void AdvanceBoneReaderWatch();
