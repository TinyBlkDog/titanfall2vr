#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// GATE 1 -- MAKE THE ARMS AND BODY DRAW NOTHING.
//
// The route, and why it is this one, is in FH3-RESULT-2026-08-19.md. Short
// version: nothing on the entity gates its drawing, because the entity is not
// in any render list. It is drawn from the bone matrices that slot 205
// (SetupBones, client.dll+0x0EE950) writes into the caller's stack, and the
// wearer proved that by watching the arms come adrift when the call was
// suppressed.
//
// So we let SetupBones run and then flatten what it wrote.
//
// HOW THE FLATTENING WORKS. A matrix3x4_t is a 3x4 row-major transform: the
// left 3x3 is rotation and scale, the last column is translation. Zeroing the
// 3x3 and KEEPING the translation sends every vertex weighted to that bone to
// the bone's own origin. Triangles whose vertices share a bone collapse to a
// point; triangles spanning two bones collapse to a segment between two bone
// origins. Either way the area is zero and a rasteriser emits no pixels.
//
// Zeroing the whole matrix would work too, but this way every number stays
// finite and stays near where it already was, so nothing downstream that
// normalises or bounds these matrices is handed a degenerate case it has never
// seen. Cheap insurance against a crash in code we have not read.
//
// WHY THIS DOES NOT TOUCH THE GUN. The gun is a different entity with its own
// SetupBones call into its own buffer, and the interceptor compares `this`
// against the arms pointer before it does anything at all. The filter is in
// asm ahead of everything, exactly as the placement pin's is.
//
// WHY THIS IS NOT THE CLOSED BONE ROUTE. The earlier bone work died at seams
// where only 12-16% of writes survived to the screen. This seam is measured:
// slot 205 fires once per frame on our instance -- 480 calls in six seconds --
// and its output is provably what the draw consumes. And the old route's
// ribbon risk came from collapsing SOME bones while their neighbours stayed
// put; collapsing all of them leaves nothing to stretch toward.
// ---------------------------------------------------------------------------

// Resolves the vtable slot, verifies it holds the function this was written
// for, and swaps in the interceptor. Idempotent.
bool InstallArmsCollapse();

// Puts the slot back. Safe when nothing is installed.
void RemoveArmsCollapse();

// Same key arms and disarms. Installs on first use.
void ToggleArmsCollapse();
bool IsArmsCollapseArmed();

// Per plugin frame: keeps the watched instance current and reports.
void AdvanceArmsCollapse();

// INI / menu: `set body.show = N`. 0 = weapon only (every arms bone
// collapsed), 1 = weapon + hands (the two hand chains from the model's own
// bone table are left alone, everything else collapsed), 2 = weapon + full
// body (no collapse; the hook stands down). Live: a change applies on the next
// SetupBones. The collapse arms itself as soon as the arms entity exists and
// re-arms after a map change; in a titan it always stands down (titan_state).
void SetBodyShowMode(int mode);
int BodyShowMode();
