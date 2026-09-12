#pragma once

// Task 2: read-only camera-pointer capture. The detour executes the original
// instructions unchanged; F8 merely requests a single diagnostic readout.
void EnsureCameraHookInstalled();
void RemoveCameraHook();
void RequestCameraSnapshot();
void TryLogRequestedCameraSnapshot();
// Task 01 stage 1: bounded, observation-only trace of the engine's own
// view-angle state. Writes nothing to game memory.
void RequestCameraAngleTrace();
void AdvanceCameraAngleTrace();
// Task 01 stage 2: bounded, self-reverting fixed yaw offset written into the
// engine's own view angles. Hotkey-gated; nothing arms on load.
void RequestBoundedYawWriteTest();
void AdvanceBoundedYawWriteTest();
// Task 01 stage 3: same bounded test one step upstream, on the engine's angle
// source rather than the camera destination. Aborts itself on runaway.
void RequestBoundedSourceYawWriteTest();
void AdvanceBoundedSourceYawWriteTest();
// Task 01 stage 4: head tracking. Toggle-gated, composed onto the engine's own
// angles, and released automatically if the headset pose goes stale.
void SetHeadTrackingArmed(bool armed);
bool IsHeadTrackingArmed();
void AdvanceHeadTracking();
// Positional head tracking: leaning and small lateral movement move the view
// origin while the body stays put.  Rides with head tracking, separable because
// it is the one write that moves the view origin rather than only aiming it.
void SetHeadPositionalTracking(bool armed);
// The eyes orbit the neck, so head rotation is not lean. Derives the lean from
// the pivot instead of the eyes.
void SetNeckModel(bool enabled);
void SetNeckOffsets(float forwardMetres, float upMetres);
bool IsHeadPositionalTracking();
void RequestOneShotCameraNudge();
void TryApplyRequestedCameraNudge();

// The head's room-space position at the moment head tracking captured its
// reference, in metres on OpenXR axes. Returns false before a reference exists.
//
// Exposed for the hand composition, which must place the hand relative to a
// FIXED room origin rather than relative to where the head happens to be now.
// Using the live head position makes the hand move whenever the head does --
// the head pivots about the neck, so turning it translates the eyes several
// centimetres -- and that shows up as the held gun sliding the opposite way
// when the player looks around, which is exactly what was reported.
bool TryGetHeadPositionReference(float outMetres[3]);
// The reference YAW the view is composed against (view yaw = body yaw + head yaw
// - this). The hand path must map room to game with the same angle, or the gun
// sits off the view by exactly this many degrees. False until a reference exists.
bool TryGetHeadYawReference(float* degrees);

// DROP THE HEAD-TRACKING REFERENCE so the next frame re-captures it.
//
// The reference is latched once, the first time head tracking runs, and
// whatever pose the headset happened to be in at that instant becomes "forward"
// for the rest of the session. One log line said so plainly: reference
// yaw=-39.58 pitch=17.00 -- captured while the headset was nearly 40 degrees off
// axis and pitched up 17 -- and the wearer then found the gun sitting about 45
// degrees right with no way to correct it.
//
// Long-pressing the menu button recentres the RUNTIME's space, which is why
// that did nothing: it moves the space under us and leaves our own reference
// exactly as stale as it was. This is what the runtime's
// XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING event should drive.
void RequestHeadTrackingRecentre();

// The wearer's real eye height above their real floor, measured in the runtime's
// STAGE space. Zero until measured, and zero forever if the runtime has no floor.
void SetMeasuredEyeHeightMetres(float metres);
float MeasuredEyeHeightMetres();
// The game's own camera height in Source units, as the camera detour last saw it.
float GameCameraHeightUnits();
// All three axes, for the aim census's D1 measurement.
void GameCameraBasePosition(float out[3]);
// The runtime's raw head position in metres, BEFORE the recentre reference is
// subtracted. False until a reference exists.
bool ReadHeadPositionMetres(float out[3]);

// WHERE THE RENDERED VIEW IS, RELATIVE TO THE GAME'S OWN.
//
// In VR these are two different things and almost everything confusing about
// ADS comes from that. The hook composes the rendered view as
//
//     yaw   = baseYaw + (headYaw - headYawReference)
//     pitch = headPitch          <-- ABSOLUTE. The head owns pitch outright and
//                                    the game's own view pitch is not used at all
//
// so the bullet, which travels along worldViewAngles, has never followed the
// head vertically. Hip-firing hides it because the aim write puts the HAND's
// angles in; hand it back to the engine for ADS and the divergence is on show,
// which is what "the gun is NOT centered in the screen but on where I was
// looking" is.
//
// yawDelta is how far the head has turned since the reference; absolutePitch is
// the pitch actually being rendered. False before head tracking has a reference.
bool GetHeadViewDelta(float* yawDelta, float* absolutePitch);

// A pitch offset on the rendered view, set on the ADS transition.
//
// Rendered pitch is the headset's own absolute pitch and has no base term -- by
// CHOICE, not by necessity, which is the correction to several earlier claims
// that pitch "could not" follow the gun. It could; the question was whether it
// should, and that is the wearer's call.
//
// SET on entry and CLEARED on release, so the view returns to where the head
// physically is. Holding it past release was tried and left the horizon wrong for
// ordinary play. Zero outside ADS by construction, so nothing accumulates.
//
// While it is non-zero, level in the headset is not level in the world. It
// SHIFTS the horizon rather than tilting it, so it is not the roll mismatch that
// sickens fastest, but it is a decoupling and it is why most VR titles refuse.
void SetViewPitchOffset(float degrees);
float ViewPitchOffset();

// The two terms of the written camera pitch: the head's own orientation and the
// ADS view-pitch offset. Published separately because their SUM is clamped at
// +/-89 and was measured pinned there, and a sum cannot say which term climbed.
void ReadPitchComposition(float* headPitch, float* viewOffset);
