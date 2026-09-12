#pragma once

// A flat, headset-free harness.
//
// Nine or ten headset runs went into the current flicker, and almost every one
// was spent falsifying a guess of mine that a measurement could have killed
// first. The prior-art survey names this as the highest-leverage thing this
// project could build: BioShock ships a simulated OpenXR runtime precisely so
// that "asking the user to put the Quest 3 on for something the simulator could
// have answered is a wasted test session".
//
// This is the small version of that idea, aimed at the part that keeps going
// wrong -- the camera and viewmodel maths -- rather than at the compositor.
//
// 1. A SYNTHETIC HEAD POSE. Publishes a fixed, configured orientation straight
//    into the same globals the OpenXR path feeds, so head tracking runs with no
//    headset, no runtime and no OpenXR at all. The pose is CONSTANT, which is
//    the point: with the head still and the player still, every frame should
//    produce identical camera matrices. Anything that varies is the defect,
//    with no perception or judgement involved.
//
// 2. A PER-UPLOAD CORRECTION TRACE. Records, for a few whole frames, every
//    upload the viewmodel correction touches -- its position within the frame,
//    the angles the correction was built from, and the rotation it applied.
//    If the four corrected uploads of one frame disagree, or if the same upload
//    differs between frames at a fixed head pose, that shows up as numbers in a
//    log rather than as a flicker someone has to describe.

void SetSyntheticPoseEnabled(bool enabled);
// Cycles OFF -> STATIC -> SWEEP -> OFF on a single key.
void CycleSyntheticPoseMode();
bool IsSyntheticPoseEnabled();
// Call once per plugin frame. Does nothing unless enabled.
void AdvanceSyntheticPose();
// Configured pose, in degrees. Non-zero yaw is the useful default: it puts the
// correction under real load rather than testing it at the identity, where a
// wrong rotation and a right one look the same.
void SetSyntheticPoseAngles(float pitchDegrees, float yawDegrees, float rollDegrees);

// Steps the synthetic yaw, wrapping at +-90.
//
// This exists because the first version of this harness could not test anything.
// Head tracking RECENTRES when it is armed, so a pose that is constant from
// before arming makes the reference equal to the pose, the delta permanently
// zero, and the correction a no-op -- it dutifully applied the identity matrix
// to every upload, four frames running. The pose has to be able to MOVE after
// the reference is captured, or the correction is never under load.
//
// Sequence that actually exercises it: enable the pose (yaw 0), arm head
// tracking so the reference is captured at 0, then step the yaw. The delta is
// then a known, constant, non-zero rotation.
void NudgeSyntheticYaw(float deltaDegrees);
float SyntheticYaw();

// A TILTED synthetic pose. The harness could previously only vary yaw, which
// left the recentre reference always level and made a composition that
// subtracts the reference's pitch/roll indistinguishable from one that does
// not. Both default to zero.
void SetSyntheticPosePitch(float pitchDegrees);
void SetSyntheticPoseRoll(float rollDegrees);
