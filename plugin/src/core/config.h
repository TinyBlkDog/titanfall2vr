#pragma once

#include <windows.h>

#include <cstdint>

// Hotkey bindings and tunable values, both loaded from an optional INI file
// next to the DLL.  The file BINDS and TUNES, and -- since `autoarm` -- it can
// also ARM.
//
// That is a deliberate change to the old "nothing arms on load" rule, not an
// oversight.  The rule was there so the plugin could never surprise a player
// who just wanted to launch the game, and that is still worth having, so
// `autoarm` defaults to 0 and the rule holds unchanged for anyone who does not
// set it.  What it cost was paid by the person wearing the headset: bringing VR
// up meant four keys in order, typed blind, and a key that silently fails to
// take is indistinguishable from one that was mistyped.  Opting in to that
// sequence happening by itself is the player's call to make, and the INI is
// where they make it.
//
// The experiment flags still only change what an already-armed correction does.

// How far the plugin arms itself with no keypresses at all.
// 0 off, 1 XR, 2 +head tracking, 3 +stereo, 4 +projection layer.
void SetAutoArmTarget(int stage);
// viewmodel.correction, DEFAULT ON. Whether auto-arm stage 2 arms the viewmodel
// correction (the counter-rotation that keeps the weapon family in the body's
// frame while the head turns). 0 leaves the weapon family on the engine's own
// camera; INSERT still toggles it by hand. Exists so the whole "viewmodel
// family leaves the head frame" subsystem can be switched off from the ini in
// one run, together with autoarm = 4 (no placement pin).
void SetViewmodelCorrectionWanted(bool wanted);
// game.fov_scale -- holds the engine's own cl_fovScale. Preferred over
// overriding the projection, because ADS narrows RELATIVE to this and an
// override fights it.
void SetGameFovScale(float scale);
float GameFovScale();
void SetAutoArmRequireLiveView(bool required);
// headtracking.recentre_on_spawn. Re-takes the head reference once, a few
// seconds after the world is up, so the eye height is sampled from a wearer who
// is sitting the way they mean to play rather than one mid loading screen.
void SetRecentreOnSpawn(bool enabled);
//
// The binding scheme is a LEADER KEY, not a modifier.  We cannot suppress
// input -- the game reads raw input and sees every key we do -- so a modifier
// chord fires the game's binding for the modifier as well as ours.  CTRL+ALT+U
// made the player crouch.  The rule is therefore that every key in a binding
// must be one the game itself ignores: press the leader, then one of the
// F-keys, the navigation cluster or the numpad.

enum class Action : unsigned {
    // Everyday toggles.  These keep their bare keys: they are muscle memory,
    // they are what every document in this repo names, and they are the only
    // ones pressed with a headset on.
    ArmOpenXr,
    ToggleHeadTracking,
    ToggleAlternateFrameStereo,
    ToggleViewmodelCorrection,
    ToggleProjectionLayer,
    ToggleViewmodelWorldFov,
    ToggleMatchHeadsetFov,
    WeaponSmaller,
    WeaponLarger,
    TogglePositionalTracking,
    ToggleSyntheticPose,
    IpdSmaller,
    IpdLarger,

    // The two open questions from the Task 01b outcome, made A/B-able rather
    // than settled by argument.  Both default to the shipped behaviour.

    // Diagnostics.  Behind a modifier, because working ones were being evicted
    // by new ones for want of a key.
    ReloadConfig,
    LogBindings,
    // Dump both eye images of the current frame to %TEMP% (read-only). The
    // wearer's screenshot key: the VR window is larger than the monitor.
    CaptureEyes,
    StepRuiMarkerLadder,
    StepUseAim,
    StepWorldMarker,
    ToggleHud2dZoomAB,
    ToggleHudFovMatch,
    StepMarkerSize,
    StepLowerLeftShiftX,
    TagVisibleWidgets,
    MarkFrameSpike,
    StepInstructionPanelScale,
    StepGauntletTimerShift,
    StepMarkerTextX,
    StepHudWidgetScale,
    StepHudWidgetSkip,
    StepHudWidgetNamedOnly,
    StepHudWidgetScreenSpaceOnly,
    StepHudCockpitScale,
    StepHudBarsShiftY,
    StepHudNameplateScale,
    StepHudNameplateMode,
    ToggleMarkerUnzoom,
    ToggleVguiDraw,
    DumpLabelWidget,
    StepWidgetHunt,
    ResetWidgetHunt,
    ToggleHudAnchorSign,
    MarkRuiImmediate,
    CaptureLockHuntFrame,
    ToggleHudScaleAB,
    ExperimentStereoSlot,
    ToggleAimProbe,
    ToggleWeaponPin,
    RecentreWeaponPin,
    CycleWeaponPinSlot,
    ToggleDeterministicBoneSetup,
    ToggleWeaponBonePin,
    CycleWeaponBoneTarget,

    // PLAN-CURRENT F1: the placement watchpoint, and its free rider.
    ToggleViewmodelPlacementWatchpoint,
    DumpCameraPassMatrices,
    ToggleViewmodelVisibilityProbe,
    // GATE 1, PLAN-CURRENT section 3: the EF_NODRAW route.
    // Controller -> the engine's own kbutton state: move, turn, jump, crouch,
    // fire, reload.
    ToggleVrInput,
    // PLAN-CURRENT P0: cycles the client command-build seam through
    // OFF -> LOG -> WRITE attackangles -> WRITE worldViewAngles. One key
    // carries all four so FR1 and FR2's in-run A/B need no ini edit and no
    // console -- and every press names the state it entered in the log.
    CycleAimCmd,
    // Flips what the arrow cluster drives: grip, or pivot-only.

    // Live grip calibration, reachable by feel with a headset on.
    GripForwardMore, GripForwardLess,
    GripRightMore, GripRightLess,
    GripUpMore, GripUpLess,

    // THE CONFIG MENU. PLAN-VRMENU M1.
    //
    // LEADER+DELETE, and the key was chosen by counting what is left rather
    // than by preference. Every bare key this keyboard physically has is taken,
    // and of the leader chords only DELETE and PAUSE are free -- PAUSE being
    // the leader itself. Bare DELETE is already relied on elsewhere as a key
    // the game ignores, which is the property that matters, and it sits in the
    // INSERT/HOME/END block that is findable by feel.
    ToggleConfigMenu,

    // TWO ADJACENT BARE KEYS, AND NOTHING TO COUNT OR READ.
    //
    // The first version of this made the wearer cycle F12 until a log line said
    // HUD SIZE, which is unusable with a headset on: the mode is invisible and
    // the log is unreachable. F6 now arms the HUD size AND points the six-key
    // cluster at it in one press, and F5 writes the dialled numbers out.

    // 1.6-G: A/B the projection layer's declared frustum against the one the
    // game was handed, inside one session. Takes bare F11 from
    // ToggleMatchHeadsetFov, whose route is closed with evidence -- it clamped
    // the frustum on every upload and broke ADS -- and which stays settable
    // from the ini.
    ToggleDeclareRenderedFov,
    ToggleFitHorizontal,
    // xr.lens_shear -- W2. Bound to CTRL+F1 so the wearer can A/B the height
    // without a relaunch. A bare key would have to displace a live one: every
    // F-key and every leader chord is already taken.
    ToggleViewportFull,
    ToggleDecouple,

    // RESET HEIGHT. Re-takes the head reference so your CURRENT pose becomes the
    // game's eye height and forward. LEADER+TAB, the last free chord on this
    // keyboard -- every bare key and every other leader chord is spoken for, and
    // TAB is a key the game only shows a scoreboard for.
    RecentreView,

    // PLAN-TITAN P0-b's falsifier: fault the process ON PURPOSE and check that
    // the recorder wrote its line and that the previous session survived the
    // rotation. Bare PRINT SCREEN -- see the binding table for why that key and
    // why it is inert unless `crash.selftest = 1` says otherwise.
    // ONE burst of nested scene draws, then it disarms itself. BARE F6 -- an
    // experiment slot, no modifier.
    //
    // It shipped on CTRL+F4 for two runs and both rules broken by that are
    // written at the top of this file: modifiers are game-bound, "CTRL+ALT+U
    // made the player crouch", and every key in a binding must be one the game
    // itself ignores. CTRL IS CROUCH. So the arm key for a burst whose whole
    // purpose is comparing two pictures of one scene was also moving the camera
    // and the viewmodel, every single press. The wearer had to press CTRL, wait
    // out the crouch animation, and only then hit F4 -- which is the sound a
    // broken binding makes.
    //
    // F6 is the one key in the table with an explicit in-game vetting beside it
    // ("F6 is not a Titanfall binding, so it does nothing in the game at all").
    // Off F4 entirely, so the arms.collapse hazard is retired rather than
    // guarded against for a third time.

    // M2 -- the substitution's POSITIVE CONTROL, and the reason it is a key at
    // all: "the artefact disappeared" is only evidence if the same session can
    // put it back. BARE F5, the neighbouring experiment slot. Inert unless
    // `stereo.substitute = 1`.
    ToggleRtvSubstitution,

    // HEIGHT A2 -- applies / removes `eye.raise` at the engine's own eye-position
    // getter (see player_eye_hook.h), so one run carries the pass-through
    // control and the raised arm. BARE F5, slot B, reassigned this build; the
    // RTV substitution toggle it displaces goes to chord {} and comes back with
    // `bind stereo.substitute.toggle = F5` in the ini.
    ToggleEyeRaise,

    // The lean detach, A/B-able live. 1.00 cancels head TRANSLATION on the
    // viewmodel family so the gun stays where the hand is while the head slides
    // around it; 0.00 lets it ride along. Bare F5, slot B.
    ToggleLeanDetach,
    // PER-WEAPON GRIP A/B (KICKOFF-GUN-ALIGN-2026-09-03 rung 2): toggles the
    // per-weapon position term composed from the model's own R_HAND attachment.
    // Bare F5, slot B, this build. See ads_probe.h.
    TogglePerWeaponGrip,

    // M3 -- toggles batch 2.s eye offset without disturbing the census, so one
    // session carries both arms. RETIRED from slot C this build (chord {}); it
    // is armed from the ini by `stereo.substitute_reads` and one `bind` line
    // brings the key back with no rebuild.
    ToggleBatch2EyeOffset,

    // T-A -- drives the engine's OWN temporal resolve to current-frame-only
    // through its console, and reads the convar back to prove the set took.
    // BARE F3, experiment slot C this build. The four slots are reassigned per
    // build rather than grown; this one takes C because the read-side
    // substitution it displaces stays armed from the ini all run and its toggle
    // is not the question.
    ToggleTemporalCurrentFrameOnly,

    // N2 -- toggles the ENGINE-SIDE eye write without disturbing any census, so
    // one session carries the armed burst and its zero-offset control. BARE
    // F12, experiment slot D. Inert unless `stereo.engine_ipd` is in the ini.
    ToggleEngineCameraWrite,

    CrashRecorderSelfTest,

    Count,
};

constexpr unsigned kModCtrl = 1u;
constexpr unsigned kModAlt = 2u;
constexpr unsigned kModShift = 4u;

struct KeyChord {
    unsigned vk = 0;   // 0 means unbound
    unsigned mods = 0; // supported, but discouraged: modifiers are game-bound
    bool leader = false;
};

// Reads <dll directory>\titanfall2vr.ini if it exists.  Safe to call again to
// pick up edits without restarting the game.
void LoadPluginConfig(HMODULE self);
void ReloadPluginConfig();
void LogActiveBindings();

const KeyChord& BindingFor(Action action);
// The key that opens the window in which a LEADER+key binding will fire.
const KeyChord& LeaderKey();
unsigned LeaderTimeoutMs();
const char* ActionName(Action action);
// Formats a chord as "CTRL+ALT+U" into caller storage; returns the buffer.
const char* DescribeChord(const KeyChord& chord, char* buffer, size_t size);

// Hotkeys are polled with GetAsyncKeyState, which is process-wide and fires
// while the game is alt-tabbed.  Off by default only if the config says so.
bool HotkeysRequireForeground();

// F3 PIN OFFSETS, from the INI: pin.fwd, pin.right, pin.up, pin.spin.
//
// Forward/right/up are units in the driven entity own yaw frame; spin is
// deg/s, and 0 freezes the rotation so an animation can be watched cleanly.
// Tunable live with LEADER then END, which re-reads the file.
//
// NOT console variables. That was tried and it crashed the game -- see the
// note in hand_cvars.h.
void ReadPinOffsets(float* forward, float* right, float* up, float* spinDegreesPerSecond);

// HAND CALIBRATION from the INI: hand.off_pitch, hand.off_yaw, hand.off_roll,
// hand.yawcomp. Reloadable live with LEADER then END.
//
// These replace reads of the tf2vr_hand_* convars in the composition. Reading a
// convar was always safe; SETTING one from the console crashes, so calibration
// had no working path while wearing a headset. yawcomp defaults to 0, the
// measured value.
void ReadHandCalibration(float* offPitch, float* offYaw, float* offRoll, float* yawComp);
// Nudges where the gun POINTS: hand.off_pitch / off_yaw / off_roll, in degrees.
// Positive yaw is LEFT in Source's convention.
void NudgeHandAngleOffsets(float pitch, float yaw, float roll);

// POSITION calibration for the hand pose, in the PLAYER's frame: hand.off_fwd,
// hand.off_left, hand.off_up, in Source units (about an inch each). Reloadable
// live with LEADER then END, so an offset can be dialled out while wearing the
// headset instead of costing a rebuild per guess.
void ReadHandPositionOffset(float* forward, float* left, float* up);

// THE GRIP OFFSET, in the WEAPON's own frame: hand.grip_fwd, hand.grip_right,
// hand.grip_up, in Source units. Rotated by the hand's orientation before it
// is applied, which is what makes the gun pivot ABOUT THE HAND rather than
// swinging on a lever arm from a model origin authored near the eye.
void ReadGripOffset(float* forward, float* right, float* up);
// Live adjustment without taking the headset off. See the arrow-key bindings.
void NudgeGripOffset(float forward, float right, float up);

// Which rotation axes the hand drives: hand.use_pitch, hand.use_roll (1/0).
// Diagnostic. With a whole body rigidly attached to the gun, wrist roll sweeps
// it through an arc large enough to hide what the yaw is doing.
void ReadHandAxisGates(float* usePitch, float* useRoll);

// PIVOT CALIBRATION -- moves the point the gun ROTATES ABOUT, without moving
// the gun.
//
// The grip offset already IS the pivot: the composed position is
// `hand + Rz(bodyYaw)*playerOffset + R(weapon)*grip`, so the model-space point
// that stays put under a wrist rotation is exactly `-grip`. Nudging grip
// therefore moves the pivot -- but it ALSO translates the gun by `R*delta`,
// and that translation is all the wearer can see. Reported as "the tooling
// moved the GUN; I want to move the PIVOT".
//
// So this nudges grip and simultaneously takes `R*delta` back out of the
// player-frame offset, using the orientation as it is at the moment of the
// press. The gun does not move; only what it swings around changes. That makes
// the pivot judgeable by eye, which is the whole problem.
void NudgePivotOffset(float forward, float right, float up);

// Adds to the player-frame position offset. Exposed for the compensation
// above; the wearer's own position keys still go through the INI values.
void NudgeHandPositionOffset(float forward, float left, float up);

// WHICH TARGET THE ARROW CLUSTER DRIVES. F12 cycles it.
//
// The six keys the wearer already knows by feel keep working; only their
// meaning changes. A third target was added rather than new keys because the
// keyboard has none left -- every leader chord on a key this keyboard
// physically has is bound, and F13-F24 parse but no key sends them, which
// would be a dead control that looks like a broken one.
enum class CalibrationTarget {
    Pivot = 0,        // the point the gun rotates about; the gun does not move
    GunPosition = 1,  // where the gun sits; its pivot travels with it
    Reticle = 2,      // the reticle: its SIZE, at the crosshair emit
    // The non-reticle HUD, on the RUI layer coordinate space. Separate axes,
    // because that space grows from its top-left origin: vertical enlargement
    // lifts the HUD off the bottom edge (wanted) while horizontal enlargement
    // only drags it left (not wanted), so they cannot share one key.
    HudScale = 3,
    // The LOWER-LEFT group, which is a different mechanism from HudScale above
    // and has to stay a separate target because of it.
    //
    // HudScale drives the engine's own safe-area inset, which reaches the
    // top-left cluster. The lower-left group is measured NOT to be on that
    // path -- per layer type in the last session, and now per widget -- so it
    // is driven by scaling the coordinate space of its two draw targets
    // instead, which scales it about the SCREEN CENTRE by the reciprocal.
    //
    // Two halves of one HUD, two levers, one key each. They are dialled
    // separately because they respond to different fields, not because anyone
    // wanted two knobs.
    HudLowerLeft = 4,
    // WHERE the HUD is anchored, as opposed to how big it is. Head-locked (it
    // rides the headset) or aim-locked (it sits where the gun is pointing).
    // A mode rather than a new key, because the six calibration keys are
    // already known by feel and one key changing what they mean is the scheme
    // this project settled on.
    HudAnchor = 5,
    // WHERE THE GUN POINTS. hand.off_yaw and friends were wired but reachable
    // only by editing the ini and restarting, which is dialling blind.
    HandAngle = 6,
    // HOW BIG THE WORLD IS, which is how TALL THE WEARER IS in it.
    //
    // Stereo separation sets perceived scale: a half-IPD too small in game units
    // makes the world read as larger and the wearer as shorter. It is derived
    // from the runtime's real IPD and an assumed "Source units are ~1 inch", and
    // that assumption has never been checked against this game.
    //
    // "Playing like a dwarf is not acceptable" -- and the knob to fix it existed
    // with NO KEY ON IT (ipd.smaller / ipd.larger, both bound to nothing), which
    // is this project's recurring dead-control failure. UP/DOWN drives it here,
    // where the six keys are already known by feel.
    WorldScale = 7,
    // NOTHING. The six keys do nothing at all, and this is the DEFAULT.
    //
    // They used to be live from the moment the game launched, on the reasoning
    // that a stray press "only means anything while the hand-driven pin is
    // armed -- which is never true in a menu". The pin IS armed in a menu, and
    // the pause menu is navigated with the arrow cluster, so every DOWN press
    // while picking a menu item walked the gun pivot forward half a unit. It
    // was reported as the gun drifting further every time the menu was opened,
    // and the log showed grip_fwd marching -6.75 -> -10.75 with off_fwd
    // tracking it, which is NudgePivotOffset's exact signature.
    //
    // Cheaper and more robust than detecting the menu: make the keys inert
    // until a target is deliberately selected, and let the cycle return here.
    Off = 8,
    Count = 9,
};
void SetCalibrationTarget(CalibrationTarget target);
CalibrationTarget CurrentCalibrationTarget();

// Kept so the existing call sites and the grip report keep reading naturally.
void SetPivotCalibrationMode(bool pivotMode);
bool PivotCalibrationMode();
