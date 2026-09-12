#pragma once

// THE RIGHT HAND, COMPOSED INTO WORLD SPACE.
//
// Everything S1-S5 built moves a prop to whatever world position and angles the
// six convars carry. This is the part that decides what those six numbers are:
// it takes the hand where the headset reports it -- room space, metres, OpenXR
// axes -- and turns it into a Source world pose the prop can be put at.
//
// THE MATH IS NOT NEW, WHICH IS THE POINT.
//
//   room -> Source axes    (-xrZ, -xrX, +xrY) = (forward, left, up)
//
// is the mapping camera_hook.cpp:725-727 already uses for head positional
// tracking, which is working and verified in the headset. Reusing it rather
// than deriving a second one means a sign error here cannot disagree with a
// sign convention that is already right.
//
// The offset used is HEAD to HAND, not hand alone. That cancels the room origin
// and the recentring reference in one subtraction, so nothing here has to know
// where the play space was when the session started.
//
// It is rotated by the camera's BASE yaw, not its applied yaw. Base is the
// game's own view direction before head tracking adds the headset's rotation to
// it -- the body's facing. Rotating by the applied yaw instead would swing the
// gun around the player every time they turned their head, which would look
// exactly like broken tracking.

enum class HandPoseSource {
    Off = 0,
    // 1 was SyntheticSpin, the 1 Hz circle that proved the plugin->script convar
    // transport. Removed 2026-09-08 with that bridge. The value is REFUSED by
    // SetHandPoseSource rather than reused, so an old ini does not silently mean
    // something new.
    // A FIXED offset from the head, composed through the real world path. No
    // headset needed, and it is what makes the composition checkable flat: the
    // gun must sit at one place relative to the player and stay there while
    // they walk and turn. Nothing else in this file is exercised differently by
    // the real hand.
    SyntheticFixedOffset = 2,
    // The right controller's grip pose. H1.
    RightController = 3,
};

// INI: `set hand.source = 0..3`.
void SetHandPoseSource(int source);

// hand.yaw_rebase. Armed by default. Adds the body yaw the camera base has
// moved through since the composition was formed, at the point the pose is
// CONSUMED -- the yaw counterpart of the position re-base, measured at up to
// 8 degrees per frame while turning. 0 restores the stale behaviour.
void SetHandYawRebase(bool enabled);
// hand.ref_yaw (default 1): compose the hand against the same head-yaw reference
// the view uses. 0 restores the old composition, which left the gun off the
// view by the reference yaw after every non-runtime recentre.
void SetHandReferenceYaw(bool enabled);
HandPoseSource CurrentHandPoseSource();

// Composes and publishes once. Call once per frame.
void PublishHandPoseForFrame();

// THE COMPOSED WORLD POSE, for the H1 placement pin.
//
// Same numbers PublishHandPoseForFrame sends to the convars, read directly.
// The convar bridge existed to reach the retired script prop; H1 drives the
// viewmodel placement in C++, so the pin reads this instead.
//
// False until a pose has been composed at least once -- the pin must then
// leave the placement alone rather than writing zeros, which would throw the
// weapon to the map origin.
bool TryGetComposedHandPose(float position[3], float angles[3]);

// The raw inputs to the composition, for deriving the room-to-world rotation
// from a capture rather than sweeping for it by hand.
bool TryGetHandYawInputs(float* headYaw, float* localYaw, float* gameYaw);

// THE HAND AS AN OFFSET FROM THE CAMERA, not an absolute world position.
//
// Four attempts at choosing a world anchor each traded one artefact for
// another, because every camera global available on the plugin frame is
// sampled on a different clock from the moment the pin writes: the camera base
// is latched every render pass and leaps a whole step in one frame, the view
// origin is captured once a frame and lags movement by one.
//
// The pin does not need a world anchor at all. It already reads the placement
// the ENGINE just committed for the viewmodel -- glued to the eye, smoothed,
// correct on stairs, which is why the gun never jumps in ordinary play -- and
// then overwrites it. Adding the hand's offset to THAT is exact by
// construction: one value, one instant, no clocks to skew.
//
// The offset is composed minus the anchor it was composed against, both from
// the same seqlock snapshot, so which anchor was used cancels out entirely.
// The constant difference between the viewmodel's origin and the eye is
// absorbed by the existing position calibration.
bool TryGetComposedHandOffset(float offset[3], float angles[3]);

// ---------------------------------------------------------------------------
// C4 -- THE BRACE. In ADS the gun moves a fraction of what the hand moves, so
// aiming is fine-tuning rather than swinging, and it costs something to hold.
//
// A GAIN CHANGE, NEVER A FILTER: aim = anchor + (hand - anchor) * gain, applied
// in the same frame with no history at all. The input is never delayed, only
// scaled, which is what keeps it inside the standing rule against latency on an
// input path. Tremor damping falls out of the same factor for free.
//
// Applied in RecordComposed -- the ONE place the composed pose is published --
// so the pin and the aim write get it by construction rather than by two
// copies kept in step by discipline, and the anchor advances once per FRAME
// rather than once per consume.
//
// INI, all live and reloadable with LEADER then END:
//   ads.brace_gain        delta scale inside the cone. 1.0 IS off.
//   ads.brace_cone_deg    fine-tune half-angle
//   ads.brace_readopt_deg past this the anchor follows the hand one for one.
//                         Must exceed the cone, or the gain steps at a single
//                         value and a hand resting there chatters.
//   ads.brace_force       hold it engaged without the trigger, FOR A FLAT RUN.
void SetAdsBraceGain(float gain);
float AdsBraceGain();
void SetAdsBraceConeDegrees(float degrees);
float AdsBraceConeDegrees();
void SetAdsBraceReadoptDegrees(float degrees);
float AdsBraceReadoptDegrees();
void SetAdsBraceForced(bool forced);
bool IsAdsBraceForced();

// A BODY-YAW CHANGE THAT THE PLAYER DID NOT MAKE.
//
// The composed pose carries the body yaw it was formed against, and the consume
// path adds whatever the body has turned since -- the yaw re-base, which exists
// because the composition happens on the plugin frame and is consumed on the
// render frame, and up to 8 degrees of body turn can happen in between.
//
// That is right for a turn the PLAYER made. It is wrong for one WE made: the
// ADS entry alignment writes the body's yaw itself, and the re-base then adds
// our own write to the hand's angles as though the arm had swung by it. The gun
// and the reticle move by the turn amount immediately after the turn, which
// reads from the chair as the turn overshooting.
//
// So anything that writes the body's yaw directly tells the composition about
// it, and the re-base sees no change from it.
void NoteExternalBodyYawWrite(float degrees);

// THE ADS TURN MUST NOT TAKE THE GUN WITH IT.
//
// The composition maps the hand into world space THROUGH the body's yaw:
// pose.yaw = localYaw + bodyYaw + offYaw. So when the ADS alignment turns the
// body to face where the gun was pointing, the next composition puts the gun
// that much further round -- and the gun is off centre by exactly the turn,
// again:
//
//   "it correctly rotates me 15 degrees but my gun is still aiming 15 degrees
//    right instead of being snapped to center of the screen"
//
// It is a fixed point that no choice of turn can satisfy: rotating the body
// moves the view and the gun by the same amount, so their difference is
// invariant. The coupling has to be broken, not tuned.
//
// This is the compensation that breaks it: a yaw offset subtracted from the
// composed world pose, so a body turn WE made leaves the hand pointing where it
// physically points WHILE ADS IS HELD. It is NOT allowed to persist: the
// release undoes the entry turn and removes it again (aim_cmd.cpp, ADS
// RELEASE). The earlier claim that it "accumulates, and that is correct,
// exactly as a snap turn leaves it" was wrong -- a snap turn changes the body
// yaw the composition already includes, so the hand follows it with no
// compensation at all -- and the accumulated total was the foot of sideways
// gun shift after an edge-of-view ADS and the hip aim that drifted with play.
void AddAdsYawCompensation(float degrees, bool log = true);
float AdsYawCompensation();
