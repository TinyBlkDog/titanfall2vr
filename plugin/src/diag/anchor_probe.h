#pragma once

// ---------------------------------------------------------------------------
// WORLD-ANCHORING FUNDAMENTALS. Read-only, no keys, no wearer judgement.
//
// WHY THIS EXISTS. The waypoint marker and the RETICLE both drift cyclically
// under head rotation while the thing they point at is fixed in the world
// (wearer, 2026-09-05, controller set down). Flat is perfect, so it is ours.
// Two fundamentals could produce that, and this measures BOTH in one run:
//
//   1. THE CAMERA. Is the orientation we write to the engine camera actually
//      the head's true orientation, composed with the body's yaw and nothing
//      else? The write is `pitch = headPitch, yaw = baseYaw + headingDelta,
//      roll = headRoll` (camera_hook.cpp ~865). That recipe equals
//      Rz(a) * headBasis EXACTLY -- yaw is outermost in Source's convention --
//      but only if the three angle globals are a faithful decomposition of the
//      basis at the same instant. They are read as three separate volatiles
//      just above a generation-guarded read of the basis, so a skew is
//      possible, and any skew tilts the frame everything screen-anchored is
//      drawn in. So: rebuild the written basis, rebuild the expected basis from
//      the head's own basis plus the same yaw, and report the residual rotation
//      between them. ZERO is the healthy answer and it is the answer the
//      algebra predicts, which makes any non-zero reading load-bearing.
//
//   2. THE AIM. The reticle is drawn along the aim direction, so if the aim
//      itself wanders when the head moves, the reticle's drift is an AIM bug
//      and says nothing about the camera -- and the two symptoms would then
//      have two causes rather than one. Gated on the controller actually being
//      still (its own tracked basis unchanged), this reports how far the
//      composed aim angles moved during that stillness, and how far the head
//      moved in the same window.
//
// WHAT THE PAIR DECIDES:
//   camera residual non-zero, cyclical  -> the camera composition is the cause
//   aim moves while the hand is still   -> the reticle drift is the aim path,
//                                          and the marker keeps its own cause
//   both clean                          -> both fundamentals are exonerated
//                                          numerically and the error is
//                                          downstream of the camera write
//
// Every stage counts itself; a stage that never runs says so rather than
// reading as a healthy zero. `anchor.probe = 0` disables it entirely.
// ---------------------------------------------------------------------------

void AnchorProbeTick();
void SetAnchorProbeEnabled(bool enabled);
