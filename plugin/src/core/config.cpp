#include "config.h"

#include "camera_hook.h"
#include "player_eye.h"
#include "player_eye_hook.h"
#include "camera_update_hook.h"
#include "resource_watch.h"
#include "plugin_cost.h"
#include "present_hook.h"
#include "aim_census.h"
#include "render_resolution.h"
#include "hand_pose.h"
#include "placement_pin.h"
#include "viewmodel_hide.h"
#include "arms_collapse.h"
#include "titan_state.h"
#include "dpad_gesture.h"
#include "aim_cmd.h"
#include "ads_lock.h"
#include "ads_probe.h"
#include "crash_recorder.h"
#include "jt_probe.h"
#include "scene_reentry.h"
#include "stereo_targets.h"
#include "view_block_camera.h"
#include "crosshair_scale.h"
#include "menu_overlay.h"
#include "settings_registry.h"
#include "flat_harness.h"
#include "rui_probe.h"
#include "rui_asset_dump.h"
#include "rui_immediate.h"
#include "lock_hud.h"
#include "use_target.h"
#include "world_marker.h"
#include "anchor_probe.h"
#include "frustum_census.h"
#include "vr_input.h"
#include "xinput_pad.h"
#include "squirrel_natives.h"
#include "weapon_settings.h"
#include "diagnostics.h"
#include "xr_input.h"
#include "xr_context.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// F3 PIN OFFSETS, TUNED FROM THE INI AND NOT FROM THE CONSOLE.
//
// These were briefly console variables. Setting one produced an access
// violation: they are hand-built ConVar objects and the engine setter
// reallocates the value string through a slot this layout does not really
// support. The read path was fine and had been used for a year; the WRITE path
// was assumed and never exercised, and the first instruction to exercise it
// crashed the game.
//
// The INI is our own parser, it allocates nothing in the engine, and LEADER
// then END reloads it live -- so this is strictly better tuning than the
// console was ever going to be, as well as being the one that works.
std::atomic<float> g_pinForward = 70.0f;
std::atomic<float> g_pinRight = 0.0f;
std::atomic<float> g_pinUp = 0.0f;
std::atomic<float> g_pinSpin = 15.0f;

// HAND CALIBRATION, ALSO FROM THE INI.
//
// hand_pose.cpp read these from the tf2vr_hand_* convars. Reading a convar is
// safe; SETTING one from the console crashes (see hand_cvars.h), so anyone
// dialling calibration in while wearing the headset had no working path. These
// are the INI equivalents, reloadable with LEADER then END.
//
// yawcomp = 0 is CONFIRMED, and by a better test than any sweep: with the gun
// pinned to the hand, the wearer turned their body and reported that it "does
// not affect the position of the gun in my hand". A constant room-to-world
// rotation is precisely what that means. Do not sweep it again.
//
// It also defaults to 0 because that was the MEASURED answer: across a whole headset
// run the game's own yaw sat still while head yaw swung 55 degrees either way,
// so physical rotation lives entirely in head tracking and the room-to-world
// rotation is a constant. hand_pose.cpp defaulted this to 1.0 whenever the
// convar read failed, which silently reintroduced the double-counted rotation
// that was reported as lag.
std::atomic<float> g_handOffPitch = 0.0f;
std::atomic<float> g_handOffYaw = 0.0f;
std::atomic<float> g_handOffRoll = 0.0f;
std::atomic<float> g_handYawComp = 0.0f;

// POSITION calibration, in the PLAYER's frame -- forward, left, up, in Source
// units (about an inch each).
//
// H1 tracked correctly and sat at a constant offset: the gun a foot or two
// low, the rig a foot or two in front. That is the plan's "offset but
// tracking" row, which is calibration -- except there was nothing to calibrate
// WITH. The three existing knobs are all rotations, so a translation error had
// no knob at all and would have cost a rebuild per guess.
//
// The player's frame rather than the world's, because that is how the error is
// actually described: "in front of me", "too low". Applied before the body-yaw
// rotation, so it means the same thing whichever way the player is facing.
//
// Defaults are what the wearer dialled in and reported, not guesses.
std::atomic<float> g_handOffForward = 3.76f;
std::atomic<float> g_handOffLeft = -1.23f;
std::atomic<float> g_handOffUp = 3.90f;

// THE GRIP OFFSET -- in the GUN's OWN frame, and this is the pivot fix.
//
// Reported: "the pivot point is COMPLETELY wrong. If I move the controller it
// is NOT moving the gun the same way."
//
// The viewmodel's model origin is not at the grip. Source viewmodels are
// authored with their origin up near the EYE, with the weapon sitting forward,
// right and down from it. Placing that origin AT the hand and rotating about
// it swings the gun on a long lever arm, so a small wrist rotation throws the
// weapon through a wide arc instead of turning it in place.
//
// The correction is a fixed offset expressed in the WEAPON's frame, rotated by
// the hand's own orientation before being added. Then the model origin sits
// wherever it has to for the GRIP to land on the hand, and rotating the wrist
// rotates the gun about the hand -- which is what a held object does.
//
// The player-frame offsets above stay for gross placement; these are the ones
// that fix the pivot, and they are the ones to dial first.
// DIALLED IN, IN THE HEADSET, AND KEPT. These are not guesses: they are what
// the wearer landed on with the arrow keys and reported through the calibration
// dump. Code defaults as well as INI values, so a fresh install is already
// close instead of starting the whole exercise again.
std::atomic<float> g_gripForward = -8.75f;
std::atomic<float> g_gripRight = -3.00f;
std::atomic<float> g_gripUp = 5.75f;

// WHICH ROTATION AXES THE HAND IS ALLOWED TO DRIVE. 1 = on, 0 = off.
//
// The rig is a whole BODY rigidly attached to the gun, so 50 degrees of wrist
// roll sweeps it through an enormous arc and the pivot looks like it is metres
// away. That makes the yaw -- the thing actually being calibrated -- impossible
// to judge. Turning roll off calms the rig enough to see what the yaw is doing.
//
// Diagnostic, not a design choice: a held weapon obviously needs its roll. This
// exists so roll can be taken out of the picture while something else is being
// measured, which is the same reason the spin can be frozen.
//
// AND IT IS NOT FREE TO TOGGLE. The grip offset is rotated by the gun's own
// orientation, so zeroing 50 degrees of roll RE-POINTS IT and moves the whole
// rig. Turning roll off after someone has dialled a grip offset in looks
// exactly like their calibration was lost -- which is what happened, and it
// was this flag rather than anything they did. Default 1; leave it there
// unless something is actively being measured.
std::atomic<float> g_useRoll = 1.0f;
std::atomic<float> g_usePitch = 1.0f;

int g_renderWidth = 0;
int g_renderHeight = 0;
float g_neckForward = 0.10f;
float g_neckUp = 0.10f;

struct ActionEntry {
    Action action;
    const char* name;
    KeyChord defaultChord;
};

// Bare keys are exactly the four toggles pressed by feel with a headset on.
// Everything else is LEADER first, then a key the game ignores. The letter row
// is unusable: the game binds it, and we cannot stop it seeing the press.
//
// The leader-prefixed F9/F10/DEL/INSERT do not collide with the bare ones --
// while the leader window is open the bare bindings are suppressed.
constexpr bool kLeader = true;
constexpr ActionEntry kActions[] = {
    {Action::ArmOpenXr,                  "openxr",                  {}},
    {Action::ToggleHeadTracking,         "headtracking",            {}},
    {Action::ToggleAlternateFrameStereo, "stereo",                  {}},
    {Action::ToggleViewmodelCorrection,  "viewmodel",               {}},
    // Bare, like the other presentation toggles: this is compared against the
    // quad path back to back, so it wants to be one key. END is in the same
    // navigation cluster as the DEL and INSERT bindings that already work, and
    // that cluster was the project's original hotkey range.
    {Action::ToggleProjectionLayer,      "projection",              {}},
    // Also bare, and also nav-cluster: these are judged by eye in the headset
    // and adjusted until they look right, which wants single keys.
    {Action::ToggleViewmodelWorldFov,    "viewmodel.worldfov",      {}},
    // Bare, and deliberately so. This exists to A/B a change against itself
    // WITHIN one session: judgements made by eye across separate runs are what
    // produced every wrong answer in this project's history, and a toggle the
    // wearer can reach is the difference between comparing two things and
    // remembering one of them.
    // UNBOUND, and the route is closed with evidence: match_headset_fov clamped
    // the frustum on every upload, ADS included, so the zoom stopped working.
    // It stays settable from the ini; bare F11 goes to live work.
    {Action::ToggleMatchHeadsetFov,      "xr.match_headset_fov",    {}},
    // BARE F11, inheriting the key and the reason for it. The declared-vs-
    // rendered frustum is judged by eye while yawing the head, so it has to be
    // flippable without ending the run: comparing two things beats remembering
    // one of them, and this project's wrong answers all came from the latter.
    {Action::ToggleDeclareRenderedFov,   "xr.declare_rendered_fov", {}},
    // BARE F11. declare_rendered_fov is now an invariant rather than an option
    // -- the layer must never declare a frustum the game was not handed -- so
    // the key moves to the question that is actually open: whether narrowing
    // the main scene alone is worth what it does to every other pass.
    // UNBOUND. Measured inert for the warp: both arms declared exactly what they
    // rendered (X=1.0000) and the wearer reported no difference. It stays at 0
    // for the effects families and stays settable from the ini.
    {Action::ToggleFitHorizontal,        "xr.fit_horizontal",       {}},
    // CTRL+F1. W2, the lens shear, and the one behavioural variable this build
    // carries: OFF is every previous build, ON aims the rendered frustum down
    // the lens axis. F1 because the wearer already knows where it is (weapon
    // size), and the CTRL modifier because no bare key is free.
    // CTRL chords: they cannot be struck while typing in the game console,
    // which is how aim.cmd was switched off for a whole run once.
    // BARE F11, and this is now the only untested config difference against the
    // known-good state that does not warp: the world pass viewport being widened
    // from the game's 4032x2520 to the whole 4032x3648 target.
    // UNBOUND: settled. The aspect-matched buffer removes the letterbox the
    // widening existed for, so viewport_full stays 0 and the A/B is over.
    {Action::ToggleViewportFull,         "xr.viewport_full",        {}},
    // BARE F11, and it is a SAFETY VALVE before it is an A/B. This path ended a
    // run in DEVICE_REMOVED with the loading screen never drawn; the wearer
    // needs to be able to stop the pacing thread without quitting the game.
    {Action::ToggleDecouple,             "xr.decouple",             {}},
    // Bare F1/F2: the only free bare keys, adjacent, and findable by feel at
    // the end of the row. The right weapon size is a judgement made wearing the
    // headset, so it has to be reachable without taking it off.
    {Action::WeaponSmaller,              "weapon.smaller",          {}},
    {Action::WeaponLarger,               "weapon.larger",           {}},
    // Bare F7, not a numpad key: this keyboard has no numpad, and it is one of
    // the toggles a comfort A/B reaches for constantly.
    {Action::TogglePositionalTracking,   "headtracking.positional", {}},
    // Bare: this is the headline comfort toggle and wants one key. F8 is in the
    // range the project used bare before the leader scheme.
    // UNBOUND: a synthetic-pose diagnostic is worth less than a bare key, and F6
    // is now the HUD scale toggle. Rebind from the ini if this is ever needed.
    {Action::ToggleSyntheticPose,      "sim.pose",                {}},
    // UNBOUND: PGUP/PGDN go to live grip calibration, which is the thing being
    // dialled in right now. IPD has a measured default and is not being swept.
    {Action::IpdSmaller,                 "ipd.smaller",             {}},
    {Action::IpdLarger,                  "ipd.larger",              {}},

    // Ordered by how often a session reaches for them.
    {Action::LogBindings,                "config.list",             {}},
    {Action::ReloadConfig,               "config.reload",           {}},
    // UNBOUND, and this one is a SAFETY fix rather than housekeeping. The route
    // it arms is recorded in UPSTREAM-VIEW-BUILD-TRACE-2026-08-14 as "Closed by
    // deadlock... The hook is removed from the active plugin configuration and
    // must not be retried" -- and it was still one leader chord away from a
    // wearer who cannot see a log. A key that can freeze the game is not a
    // diagnostic. Rebindable from the ini if it is ever deliberately re-tested.
    {Action::ExperimentStereoSlot,       "experiment.stereoslot",   {}},
    // Also off the numpad. Rarely used, so they take the arrow keys, which the
    // game leaves alone; rebind in the ini if that is ever wrong.
    // SCROLLLOCK because every other leader key is taken and the game ignores
    // it. The probe also runs itself once when a world is being rendered, so
    // this is for re-running it after a map change brings new cvars in.
    // Bare SCROLLLOCK: this one is pressed with a headset on, mid-firefight, and
    // it has to be findable by feel. The game ignores the key.
    {Action::ToggleAimProbe,             "probe.aim",               {}},
    // UNBOUND: the XInput route is measured closed (the game never calls
    // XInputGetState), so this key is released for work that is still live.
    // Rebind from the ini to re-test.
    // UNBOUND: attackangles is measured vestigial, so the probe that reads it has
    // served its purpose. It still patches server.dll, so if it is rebound from
    // the ini it must stay hotkey-gated and never automatic.
    // UNBOUND: the write it performs already gave its answer -- the rounds did not
    // move -- so the key goes to live work.
    // UNBOUND: Route A (pin the gun by transforming its camera pass) is closed with
    // evidence -- it moves body, arms, gun and HUD together. F12 goes to the bone
    // pin, which is the live route.
    {Action::ToggleWeaponPin,            "weapon.pin",              {}},
    {Action::RecentreWeaponPin,          "weapon.pin.recentre",     {}},
    // Route A is closed with evidence; the action stays so it can be re-tested from
    // the ini, but it holds no key.
    {Action::CycleWeaponPinSlot,         "weapon.pin.slot",         {}},
    // THE ARROW KEYS ARE NOT FREE, and binding four bone actions to them was a real
    // mistake. They navigate the game menus -- the reporter uses UP then ENTER to
    // pick CONTINUE on every single run -- so menu navigation was silently firing
    // these: RIGHT started a 4.7 GB heap scan, LEFT armed a bone write, DOWN
    // rewrote seven bone cvars, all before the level had even loaded.
    //
    // The rule at the top of this table already said it: every key must be one the
    // GAME ignores. Arrows are not, and "the game leaves them alone" was an
    // assumption, never checked.
    //
    // The four bone keys now take F-keys reclaimed from routes closed with evidence
    // -- XInput, attackangles, and the camera-pass pin -- so nothing working was
    // evicted to make room.
    // UNBOUND, and reclaimed by the scripted session below. Deterministic bone
    // setup is now armed automatically by whatever needs it -- the pin arms it, and
    // the flat session arms it and puts it back -- so a key that only ever had to be
    // remembered separately is a key that only ever got forgotten. Rebind from the
    // ini to drive it by hand.
    {Action::ToggleDeterministicBoneSetup, "bones.deterministic",   {}},
    // LEADER+BACKSPACE, freed above. It is a one-shot 4.7 GB heap scan, run
    // deliberately, and the pin arms it by itself when needed -- so it never wants
    // a bare key.
    // Bare F5, NOT F12: F12 is Steam overlay screenshot, which would fire on every
    // press. F3/F4/F5 are not Titanfall bindings and are not in the reporter menu
    // path (UP, ENTER, SPACE).
    // UNBOUND, and reclaimed by PLAN-CURRENT F1 below. The bone-array write
    // route at these seams is on the plan's closed list (~1% survival on the
    // 87-bone array, 12-16% on the attachment), so these two keys were held by
    // a route that is not coming back at this seam. The actions stay so the
    // ini can re-bind them; the scripted flat session drives the same targets
    // through SetWeaponBoneTargetIndex and never needed the keys.
    {Action::ToggleWeaponBonePin,        "weapon.bonepin",          {}},
    {Action::CycleWeaponBoneTarget,      "weapon.bonepin.target",   {}},
    // THE WHOLE FLAT TEST ON ONE KEY, on the F3 reclaimed above. Eight timed
    // keypresses judged by eye is not a test that can be run twice the same way,
    // and it could not be run at this desk at all: the shipped 4032x2268 window is
    // bigger than the monitor and the weapon sits in the part that is off-screen.
    // This drives the resolution, the precondition, the scan, every target and the
    // captures by itself, and restores all of it. Same key aborts.
    // UNBOUND, and reclaimed by PLAN-CURRENT F2 below. This session sweeps the
    // bone-array write route, which is on the plan closed list, so its key is
    // better spent on the live route. The action stays; rebind from the ini if
    // the F1 fallback (post-SetupBones rigid transform) ever needs it.
    // PLAN-CURRENT F1, on the F5 and F4 reclaimed directly above.
    //
    // THIS KEYBOARD HAS NO NUMPAD. That is a hardware fact about the machine
    // every one of these tests is run on, and it rules out NUMPAD0-9 and the
    // numpad operators as defaults permanently -- not just here. A binding
    // nobody can physically press is a run thrown away, so new diagnostics
    // take a key from a route the plan has closed rather than inventing one.
    //
    // F3/F4/F5 are already vetted: not Titanfall bindings, and not in the
    // reporter's menu path (UP, ENTER, SPACE). F12 stays off limits -- Steam
    // overlay screenshot.
    //
    // Not behind the leader, deliberately: the F1 capture is a timed window
    // and a two-key chord at each end adds latency to the very thing being
    // measured in frames.
    //
    // The watchpoint arms HARDWARE DEBUG REGISTERS and runs a 4.7 GB heap scan
    // if nothing is cached, so it is deliberate by construction and never
    // automatic -- the same rule the bone scan follows.
    // UNBOUND: F5 is the HUD-size save key now. The placement watchpoint is a
    // diagnostic that no HUD session needs, and a key reachable by feel is worth
    // more than one more probe. Rebind from the ini if it is ever wanted again.
    {Action::ToggleViewmodelPlacementWatchpoint, "watchpoint.placement", {}},
    // BARE F5, next to F6. Two adjacent keys, no cycling, nothing to count:
    // F6 arms and A/Bs the HUD size, F5 writes the dialled numbers to the log.
    // UNBOUND: the matrix dump did its job. P0 section 6 is closed with real
    // data -- world 123.7 x 92.9, viewmodel 86.1 x 55.4, row0 ratio 2.0022,
    // 0.000 degrees between them -- so the key goes to live work. Rebind from
    // the ini to re-dump.
    {Action::DumpCameraPassMatrices,     "dump.camerapasses",       {}},
    // Bodygroup enumeration and sweep, on the F4 reclaimed just above. It
    // CALLS ENGINE FUNCTIONS and writes m_nBody, so it is hotkey-gated and
    // never automatic, and it restores the original value on the way out.
    // UNBOUND: the bodygroup sweep answered its question. The six switchable
    // groups are SCOPES AND MUZZLES, which is what proved C_BaseViewModel is
    // the GUN and the attachment is the arms. Rebind from the ini to re-run.
    // UNBOUND: SetVisible is closed. It ran on the arms -- the +0x4B8 gate read
    // 1 -- and set bits at +0x170 that nothing ever consumes, so nothing hid.
    // Rebind from the ini only to re-demonstrate that.
    {Action::ToggleViewmodelVisibilityProbe, "probe.visibility",    {}},
    // GATE 1, FH1. Takes the F4 the visibility probe just gave up. READ-ONLY:
    // reads m_fEffects / m_nRenderMode / the client-only gate back on both
    // entities, then counts ShouldDraw on the ARMS CLASS ONLY for ten seconds
    // to settle whether the render path consults the field at all. Writes
    // nothing and restores the vtable slot it borrows.
    // UNBOUND: the FH3 sweep answered its question -- no entity virtual hides the
    // arms, and slot 205 is where the drawing gets its data. Rebind from the ini
    // only to re-run the sweep.
    // Movement, turn, jump, crouch, fire and reload from the controllers.
    // LEADER+F8 rather than a bare key: it drives the player, so an accidental
    // press while reaching for something else should not be possible.
    {Action::ToggleVrInput,              "vrinput",                 {}},
    // PLAN-CURRENT P0. BARE BACKSPACE, and the key was chosen by counting what
    // is left rather than by preference: EVERY leader-prefixed chord on a key
    // this keyboard physically has is already bound (LEADER+BACKSPACE is
    // probe.bones), the numpad does not exist here, and F13-F24 parse but no
    // key sends them -- a dead control that looks like a broken one.
    //
    // Bare is defensible for this one: BACKSPACE is already relied on as a key
    // the game ignores (probe.bones sits on it), it is unmistakable by feel at
    // the top right of the main block, and FR2's A/B wants a single press
    // rather than a two-key sequence typed blind.
    // BEHIND THE LEADER, because BACKSPACE is a CONSOLE EDITING KEY and this
    // action silently disables aiming. Keys are read with GetAsyncKeyState,
    // which is global and does not care that the game's console has focus, so
    // correcting a typo while typing `map sp_crashsite` cycles this. Measured
    // on the 2026-08-29 headset run: aim.cmd went OFF at 52.3 s, seven seconds
    // BEFORE the world came up at 59.9 s, and with nothing left to align the
    // body's pitch to the gun the engine fired along the body's own pitch --
    // base p-53.98 against a rendered p+19.72 -- putting every round into the
    // sky for the whole fight. One run lost, and the state it lands in is
    // indistinguishable from a broken aim system.
    {Action::CycleAimCmd,                "aim.cmd",                 {}},

    // LIVE GRIP CALIBRATION, ON THE ARROW CLUSTER AND PGUP/PGDN.
    //
    // Editing an INI with a headset on was reported as very difficult, and it
    // is: the file is on a monitor nobody can see. These six are findable by
    // FEEL -- the arrow cluster is unmistakable under the fingers -- and they
    // move the grip offset a quarter inch per press.
    //
    // The arrow keys were once bound to bone actions and that WAS a mistake:
    // the reporter navigates menus with UP then ENTER, so menu navigation was
    // silently starting heap scans. These are safe where those were not,
    // because the worst a stray press can do is move a calibration offset by a
    // quarter inch, and it only means anything while the hand-driven pin is
    // armed -- which is never true in a menu.
    {Action::GripForwardMore,            "grip.fwd.more",           {}},
    // DOWN and LEFT join RIGHT as dials this build, for the same reason: the
    // arrow cluster is findable by feel and the calibration actions that own
    // it are inert. All three are rebindable and none of them can move the gun.
    {Action::GripForwardLess,            "grip.fwd.less",           {}},
    {Action::StepGauntletTimerShift,     "hud.gauntlet_timer.step", {}},
    // RIGHT ARROW IS THE TAG THIS BUILD, at the wearer's request: F6 is hard to
    // find quickly with a headset on, and the tag only means anything if it is
    // pressed while the thing being identified is still on screen. The arrow
    // cluster is the one control surface this project's own notes call
    // unmistakable by feel.
    //
    // It costs nothing to borrow. The six calibration keys are inert: the
    // target defaults to Off and SetPivotCalibrationMode has no caller anywhere
    // in the tree. RIGHT rather than UP or DOWN because menus are navigated
    // vertically, so a stray press is least likely here -- and a stray tag is
    // only a harmless block in the log, never a moved gun.
    // RIGHT is the tag again this run, riding along with the performance test:
    // a read-only instrument costs nothing to stack and the wearer has a HUD
    // element to capture while they are in there anyway.
    {Action::GripRightMore,              "grip.right.more",         {}},
    {Action::GripRightLess,              "grip.right.less",         {}},
    {Action::StepInstructionPanelScale,  "hud.instruction.step",    {}},
    {Action::GripUpMore,                 "grip.up.more",            {}},
    {Action::GripUpLess,                 "grip.up.less",            {}},
    // Says the three numbers out loud so they can be copied into the INI once
    // they are right, instead of being lost when the session ends.
    // THE CONFIG MENU, on the one free leader chord. See the note in config.h.
    {Action::ToggleConfigMenu,           "menu.toggle",             {}},
    // ONE key changes what the six calibration keys MEAN, so the cluster the
    // wearer already knows by feel keeps working and nothing has to be found
    // blind. F12 is the only bare function key left; rebind from the ini if a
    // screenshot binding takes it.
    // PLAN-CURRENT F2, on the F3 reclaimed just above, so the three keys this
    // plan uses sit together under one hand: F3 drives, F4 dumps the matrices,
    // F5 watches.
    //
    // This one PATCHES CODE, at a function every entity in the map goes
    // through, so it is hotkey-gated and never arms itself on load.
    // PLAN-CURRENT rung 1. LEADER then SPACE, inherited from the 64-byte pulse
    // that is now measured closed: every bare function key is taken and every
    // leader+function key is taken, and SPACE is the one remaining chord that is
    // unmistakable by feel with a headset on. Behind the leader deliberately --
    // SPACE is JUMP, and a bare binding would fire it every time the wearer
    // jumped.
    // BARE F6, and not LEADER+SPACE, which was a bad choice: we cannot suppress
    // input, so the game saw every SPACE and the player JUMPED each time the HUD
    // was being judged. F6 is not a Titanfall binding, so it does nothing in the
    // game at all -- which is the whole requirement for a key pressed while
    // staring at the thing being measured.
    // BARE PRINT SCREEN, and it is the only bare key this keyboard physically
    // has that nothing here and nothing in the game already owns. Every F-key,
    // the whole navigation cluster, BACKSPACE and SCROLL LOCK are bound bare;
    // the numpad does not exist on this keyboard; the letter row is unusable
    // because the game reads raw input and acts on it. Windows may pop the
    // snipping overlay on this key, which is harmless and does not stop
    // GetAsyncKeyState seeing it.
    //
    // THIS KEY ENDS THE PROCESS, so a stray press must not be able to reach it.
    // It is inert unless `crash.selftest = 1` is in the ini, and the handler
    // says so in the log when it is pressed without that. Bare rather than a
    // leader chord because the plan's falsifier is written as a bare key and
    // this is a flat test at the keyboard, not something reached with a headset
    // on.
    // ---- THE FOUR EXPERIMENT SLOTS: F3, F5, F6, F12. NO MODIFIERS. ----
    //
    // Two rules, both learned the hard way, both already written down here, and
    // both ignored by me until the wearer said so.
    //
    // NO MODIFIER CHORDS, EVER. KeyChord's own comment says modifiers are
    // "supported, but discouraged: modifiers are game-bound" -- and CTRL is
    // CROUCH. Every CTRL+F4 press this project has asked for crouched the
    // character, moved the camera and moved the viewmodel, in the middle of a
    // burst whose entire purpose was to compare two pictures of one scene. It
    // is the same fault as LEADER+SPACE making the player JUMP each time the
    // HUD was being judged, which is recorded a few lines above as the reason
    // bare F6 was chosen for `rui.scale.toggle`. Exact modifier matching does
    // not help: the problem is not that the wrong ACTION fires, it is that the
    // right one fires AND THE GAME MOVES.
    //
    // FOUR FIXED SLOTS, REASSIGNED PER BUILD, NEVER ADDED TO. This table had
    // accumulated twenty-seven chords belonging to closed rungs -- traces,
    // probes, watchpoints, calibrations -- every one still bound, including
    // both standing collisions. They are RETIRED above (chord `{}`), which
    // frees F3, F5, F6, F12 and CTRL+F1. Nothing is deleted: every retired
    // action still exists and any of them comes back with one
    // `bind <name> <chord>` line in the ini, no rebuild. From here an
    // experiment takes a slot or it does not get a key.
    //
    //   F5  slot A -- this build: toggle the M2 substitution (positive control)
    //   F6  slot B -- this build: arm one burst of nested scene draws
    //   F3  slot C -- this build: toggle the M2 read-side substitution
    //   F12 slot D -- this build: N2, toggle the ENGINE-SIDE eye write
    //
    // F6 carries the arm key deliberately. It is the one key in this table with
    // an explicit in-game vetting written beside it -- "F6 is not a Titanfall
    // binding, so it does nothing in the game at all, which is the whole
    // requirement for a key pressed while staring at the thing being measured"
    // -- and that is precisely this key's job.
    //
    // AND IT IS OFF F4 ENTIRELY, which retires the hazard the old comment here
    // spent sixteen lines defending against instead of removing: bare F4 is
    // `arms.collapse`, one press of it armed a documented heap-corruption
    // crash, and CTRL+F4 was only ever safe because ModifiersMatch is exact.
    // Not sharing the key beats matching carefully on it.
    // F6 = the wearer's screenshot. Writes %TEMP%\tf2vr-EYE0.bmp / EYE1.bmp
    // from inside Present; the 5840x3648 VR window shows only its corner on
    // the monitor, so Windows screenshots miss the hands.
    // RETIRED from F6 this build (the marker ladder), not deleted: `bind
    // capture.eyes = F6` in the ini brings it straight back.

    // RETIRED from F6 this build, not deleted: the marker ladder's last rung
    // (the interface slot +0x30 rewrite) landed 1012 times and moved nothing,
    // so the ladder has no live rung. `bind rui.marker.step = F6` in the ini
    // brings it straight back.
    {Action::StepRuiMarkerLadder,        "rui.marker.step",         {}},
    // RETIRED from F6 this build, not deleted: use targeting is confirmed working
    // (use.aim = 1 ships). `bind use.aim.step = F6` in the ini brings the A/B back.
    {Action::StepUseAim,                 "use.aim.step",            {}},
    // RETIRED from F6 this build, not deleted: the gain ladder answered its
    // question (no rung nails it). `bind marker.step = F6` brings it back.
    {Action::StepWorldMarker,            "marker.step",             {}},
    // F6 THIS BUILD, and the earlier elimination does NOT cover this. That one
    // was about the WAYPOINT: hud2d.zoom changes the aiming reticle and never
    // the waypoint, which matches the pass mapping. The TARGET BRACKET is a
    // different element on the other side of that boundary -- ui(11)+0x1D560,
    // layer type 6, the crosshair family, which is exactly what the pixel-ortho
    // pass carries. It was unreachable until 2026-09-07 and its numbers say the
    // attenuation is real: over 44.3 deg of view sweep a world-locked point
    // should cross about 0.40 of the screen and it crossed 0.132, which is 3.0x
    // short against a zoom of 0.28 that predicts 3.6x.
    // UNBOUND again. Turning the zoom off is not a shippable fix: the wearer set
    // the reticle and menu size deliberately and full size is not acceptable.
    // The marker correction below is the fix; this stays as a mechanism check.
    {Action::ToggleHud2dZoomAB,          "hud2d.zoom.ab",           {}},
    // THE HUNT MOVES TO THE ARROW CLUSTER. Two guesses at the weapon highlight
    // have now failed, so it is identified by HIDING widgets one at a time
    // until it vanishes -- and that ladder is pressed many times with a headset
    // on, which is exactly what F3 is bad for. LEFT steps, DOWN marks. The two
    // HUD dials that owned them are finished and unbound.
    {Action::StepWidgetHunt,             "rui.widgethunt.step",     {}},
    {Action::ResetWidgetHunt,            "rui.widgethunt.reset",    {}},
    // F12 THIS BUILD: the HUD anchor sign. rui.hud_anchor = 2 takes the flat HUD
    // pass from the aim frame to the head; the sign of that rotation cannot be
    // verified offline, so F12 flips 1 <-> 2 and the wearer says which centres.
    {Action::ToggleHudAnchorSign,        "rui.hud_anchor.sign",     {}},
    // F12 THIS BUILD: the marker correction on against off, with the reticle
    // small on BOTH sides. Nothing the wearer likes changes across this press.
    {Action::ToggleMarkerUnzoom,         "hud.marker_unzoom.ab",    {}},
    {Action::ToggleVguiDraw,             "vgui.draw.ab",            {}},
    // F5 is the ladder's "the friendly name vanished at THIS step". The old
    // `label.dump` name outlived what the key does, and the startup binding list
    // is the only place the wearer can read what their keys mean.
    {Action::DumpLabelWidget,            "rui.widgethunt.mark",     {}},
    // F3 THIS BUILD: the missile-lock mark. Read-only and behaviourally inert --
    // it stamps the log and dumps the descriptor pool plus both draw-path
    // censuses, so the instant the wearer sees the brackets is labelled. F3 was
    // verified free: it carries no default binding here and the live ini binds
    // only aim.cmd.
    {Action::MarkRuiImmediate,           "rui.immediate.mark",      {}},
    // UNBOUND, DELIBERATELY. This cycles the HUD pass's frustum 0 -> 1 -> 2, and
    // mode 1 is now the SHIPPED FIX for world anchoring. On a key, one stray
    // press leaves the wearer in mode 2 -- the broken frustum -- and it presents
    // exactly as a regression: 2026-09-05, "It does not hold position. It moves
    // around now", which was a mode-2 press and nothing else. A confirmed fix
    // must not be one keypress from being switched off. `bind hud.fov_match.ab
    // = F6` in the ini brings the A/B back for anyone who wants to re-run it.
    {Action::ToggleHudFovMatch,          "hud.fov_match.ab",        {}},
    // RETIRED from F5 this build (height A2), not deleted: `bind
    // stereo.substitute.toggle = F5` in the ini brings it straight back.
    {Action::ToggleRtvSubstitution,      "stereo.substitute.toggle", {}},
    // Slot B, this build: the eye-position raise, applied / removed live.
    // RETIRED from F5 this build: the height is settled and lives on the View
    // tab's slider, so the slot goes to the live question. `bind eye.raise.toggle
    // = F5` in the ini brings the height A/B back.
    {Action::ToggleEyeRaise,             "eye.raise.toggle",        {}},
    // Slot B, this build: does cancelling head translation on the viewmodel
    // family stop the gun, arms and body riding a lean? See placement_pin.cpp.
    // RETIRED from F5 this build (gun-align rung 2), not deleted: the lean detach
    // is shipped (`viewmodel.detach_lean = 1`, reloadable) and `bind
    // viewmodel.lean_detach.toggle = F5` in the ini brings the A/B straight back.
    {Action::ToggleLeanDetach,           "viewmodel.lean_detach.toggle", {}},
    // Slot B, this build: the per-weapon grip term on and off, so each weapon
    // can be A/Bed in place at the gun range. Position only.
    // RETIRED from F5 this build, not deleted: `bind grip.perweapon.toggle = F5`
    // in the ini brings the per-weapon grip A/B straight back.
    {Action::TogglePerWeaponGrip,        "grip.perweapon.toggle",   {}},
    // F5 THIS BUILD. The marker size ladder, now that hud.fov_match = 1 fixed the
    // tracking and brought its 1.8952x magnification back with it.
    {Action::StepMarkerSize,             "hud.marker_size.step",    {}},
    // F5 THIS BUILD: shoot the SAME lock-hunt arm again without advancing it, so
    // each widget is seen both with the lock up and without it.
    // UNBOUND: with RB held the rings are on screen continuously, so the arm that
    // makes them vanish is self-evident and a paired baseline shot is needless
    // ceremony. One key does the whole hunt.
    {Action::CaptureLockHuntFrame,       "rui.lockhunt.capture",    {}},
    // F6 THIS BUILD (2026-09-06): the marker text-position ladder.
    // Steps where the waypoint's distance text and leader line are pinned.
    // The target icon never moves, so the wearer judges one thing per press.
    // Nothing else is on F6: hud.fov_match stays keyless on purpose.
    // RETIRED from F6, not deleted: the text pin is settled at 0.32 and
    // confirmed, and a confirmed fix must not sit one keypress from changing.
    // `bind hud.marker_text_x.step = F6` in the ini brings the ladder back.
    {Action::StepMarkerTextX,            "hud.marker_text_x.step",  {}},
    // F6 THIS BUILD: the HUD size ladder, which is the thing the wearer asked
    // for and can judge in one press.
    // RETIRED from F6 for this build: the size is set at 0.55 and the wearer is
    // happy with it. `bind hud.widget_scale.step = F6` brings the ladder back.
    {Action::StepHudWidgetScale,         "hud.widget_scale.step",   {}},
    // RETIRED from F6: three runs of stepping widgets inside the screen-space set
    // never moved the two bars once, which proves they are not in that set.
    // `bind hud.widget_skip.step = F6` in the ini brings the ladder back.
    {Action::StepHudWidgetSkip,          "hud.widget_skip.step",    {}},
    // RETIRED from F6: it did its job -- releasing the group named all three of
    // its widgets in one press. `bind hud.widget_screenspace_only.step = F6`.
    {Action::StepHudWidgetScreenSpaceOnly, "hud.widget_screenspace_only.step", {}},
    // RETIRED from F6: settled at 0.60 by the wearer stepping it.
    // `bind hud.cockpit_scale.step = F6` in the ini brings the ladder back.
    {Action::StepHudCockpitScale,        "hud.cockpit_scale.step",  {}},
    // RETIRED from F6: settled at -0.090 by the wearer stepping it.
    // `bind hud.bars_shift_y.step = F6` in the ini brings the ladder back.
    {Action::StepHudBarsShiftY,          "hud.bars_shift_y.step",   {}},
    // RETIRED from F6: settled at 0.50 by the wearer stepping it.
    // `bind hud.nameplate_scale.step = F6` in the ini brings the ladder back.
    {Action::StepHudNameplateScale,      "hud.nameplate_scale.step", {}},
    // RETIRED from F6: mode 0 is settled and wearer-confirmed for enemy labels.
    // `bind hud.nameplate_mode.step = F6` in the ini brings the A/B back.
    {Action::StepHudNameplateMode,       "hud.nameplate_mode.step", {}},
    // RETIRED from F6, not deleted: the allowlist is a blunt instrument and it
    // stopped things moving that the wearer wanted moved.
    // `bind hud.widget_named_only.step = F6` in the ini brings the A/B back.
    {Action::StepHudWidgetNamedOnly,     "hud.widget_named_only.step", {}},
    // RETIRED this build, not deleted: the read-side substitution is armed from
    // `stereo.substitute_reads` and stays armed for the whole run, so its
    // toggle is not the question T-A asks. `bind stereo.read_substitute.toggle
    // F3` in the ini brings it straight back.
    {Action::ToggleBatch2EyeOffset,      "stereo.read_substitute.toggle", {}},
    // Slot C, this build: T-A's temporal lever.
    // RETIRED from F3 for this build (HUD front, not the stereo one):
    // `bind stereo.temporal_curframe.toggle = F3` in the ini brings it back.
    {Action::ToggleTemporalCurrentFrameOnly, "stereo.temporal_curframe.toggle", {}},
    // F3 THIS BUILD: the wearer's screenshot. Indexed, so two shots survive.
    // F6 THIS BUILD, and it is the ONE key the wearer has to think about. They
    // asked for a screenshot key the agent controls, so this is it: press it and
    // the backbuffer lands in %TEMP% numbered per press, and the agent reads it
    // off disk. F3 is left free rather than carrying a second capture key.
    // RETIRED from F6 this build, not deleted: `bind capture.eyes = F6` in the
    // ini brings the eye captures straight back. F6 is the lower-left shift
    // ladder for the launch-week HUD pass, so the wearer can dial it in one run
    // instead of one value per run.
    {Action::CaptureEyes,                "capture.eyes",            {}},
    // F6 THIS BUILD.
    // RETIRED from F6: the lower-left shift is dialled and shipped as a
    // default, so its ladder has done its job. `bind hud.ll_shift_x.step = F6`
    // brings it back for a re-dial on different hardware.
    {Action::StepLowerLeftShiftX,        "hud.ll_shift_x.step",     {}},
    // F6 THIS BUILD: the wearer's tag. Pressed while something is on screen, it
    // names every widget drawing at that moment.
    // The CORE message finds itself by its text now, so the tag is not needed
    // this run and RIGHT goes to the thing the wearer asked for.
    {Action::TagVisibleWidgets,          "rui.tag",                 {}},
    // RIGHT THIS RUN: pressed when a spike is FELT. It prints the last few
    // seconds of frame timings, so a spike the wearer sees in a window the
    // instrument called clean becomes a fact about the instrument.
    // Unbound for release. It is a dev instrument and the arrow cluster belongs
    // to the calibration keys a player can reach; `bind perf.mark = RIGHT` in
    // the ini brings it straight back when a spike needs chasing again.
    {Action::MarkFrameSpike,             "perf.mark",               {}},
    // Slot D, this build: N2's positive control. Nothing else has ever been on
    // F12 and the game binds nothing to it.
    // RETIRED from F12 for this build (HUD front, not the stereo one):
    // `bind stereo.engine_write.toggle = F12` in the ini brings it back.
    {Action::ToggleEngineCameraWrite,    "stereo.engine_write.toggle", {}},
    // F12 THIS BUILD: HUD size 0.55 <-> 1.00, so the wearer can capture the same
    // scene on both sides of the transform.
    // RETIRED from F12: the screen-space HUD size is settled at 0.55 and the
    // zoom A/B needs the slot. `bind hud.widget_scale.ab = F12` brings it back.
    {Action::ToggleHudScaleAB,           "hud.widget_scale.ab",     {}},
    {Action::CrashRecorderSelfTest,      "crash.selftest.fire",     {}},
    // RESET HEIGHT AND FORWARD, the thing every other VR mod has and this one
    // did not. LEADER+TAB: the last free chord, and TAB is a key the game only
    // answers with a scoreboard.
    {Action::RecentreView,               "view.recentre",           {}},
};
static_assert(sizeof(kActions) / sizeof(kActions[0]) == static_cast<size_t>(Action::Count),
              "every Action needs a name and a default chord");

KeyChord g_bindings[static_cast<size_t>(Action::Count)]{};
bool g_requireForeground = true;
char g_configPath[MAX_PATH]{};
// Pause/Break: present on full keyboards, and Titanfall 2 binds nothing to it.
// Rebindable, because it is missing or Fn-shifted on some laptops.
KeyChord g_leaderKey{VK_PAUSE, 0, false};
unsigned g_leaderTimeoutMs = 4000;

// Every name ApplyValue has rejected as unknown, for the whole process. The
// registry self-check reads it either side of each apply; see
// settings_registry.h for why that is the instrument rather than the log line.
unsigned long long g_unknownSettingCount = 0;

struct NamedKey { const char* name; unsigned vk; };
constexpr NamedKey kNamedKeys[] = {
    {"INSERT", VK_INSERT}, {"HOME", VK_HOME}, {"END", VK_END},
    {"PGUP", VK_PRIOR}, {"PAGEUP", VK_PRIOR}, {"PGDN", VK_NEXT}, {"PAGEDOWN", VK_NEXT},
    {"DEL", VK_DELETE}, {"DELETE", VK_DELETE}, {"BACKSPACE", VK_BACK},
    {"SPACE", VK_SPACE}, {"TAB", VK_TAB}, {"ENTER", VK_RETURN}, {"RETURN", VK_RETURN},
    {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
    {"SCROLLLOCK", VK_SCROLL}, {"PAUSE", VK_PAUSE},
    // The last free bare key on this keyboard; the crash self-test defaults to
    // it. Both spellings, because either is what someone would type.
    {"PRINTSCREEN", VK_SNAPSHOT}, {"PRTSCN", VK_SNAPSHOT},
    // The numpad operators, parseable but never a DEFAULT: this keyboard has
    // no numpad at all, so nothing here may ship bound to one. Kept because
    // the ini documentation advertises numpad keys and someone on another
    // keyboard may want them -- and if they do, these four are the ones to
    // reach for. NUMPAD0-9 report their navigation twin with NumLock off
    // (numpad 7 arrives as VK_HOME, which is bound to the world-FOV match);
    // these four have no twin.
    {"NUMPLUS", VK_ADD}, {"NUMMINUS", VK_SUBTRACT},
    {"NUMSTAR", VK_MULTIPLY}, {"NUMSLASH", VK_DIVIDE},
};

void TrimAscii(char* text) {
    char* start = text;
    while (*start == ' ' || *start == '\t') ++start;
    size_t length = std::strlen(start);
    while (length && (start[length - 1] == ' ' || start[length - 1] == '\t' ||
                      start[length - 1] == '\r' || start[length - 1] == '\n')) {
        start[--length] = '\0';
    }
    if (start != text) std::memmove(text, start, length + 1);
}

void UpperAscii(char* text) {
    for (; *text; ++text) {
        if (*text >= 'a' && *text <= 'z') *text = static_cast<char>(*text - 'a' + 'A');
    }
}

unsigned KeyNameToVirtualKey(const char* name) {
    if (!name || !*name) return 0;
    const size_t length = std::strlen(name);
    if (length == 1) {
        const char c = name[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return static_cast<unsigned>(c);
        return 0;
    }
    if (name[0] == 'F' && length <= 3) {
        const int index = std::atoi(name + 1);
        if (index >= 1 && index <= 24) return static_cast<unsigned>(VK_F1 + index - 1);
    }
    if (length == 7 && std::memcmp(name, "NUMPAD", 6) == 0 && name[6] >= '0' && name[6] <= '9') {
        return static_cast<unsigned>(VK_NUMPAD0 + (name[6] - '0'));
    }
    for (const auto& entry : kNamedKeys) {
        if (std::strcmp(entry.name, name) == 0) return entry.vk;
    }
    return 0;
}

// "LEADER+F1", "F9", "none".  Returns false without touching `chord` if the
// text does not parse, so a typo leaves the default binding in place rather
// than silently unbinding an action.
//
// CTRL/ALT/SHIFT still parse, for anyone who knows their own binds and wants
// them, but they are not used by any default: the game sees the modifier press
// too and acts on it.
bool ParseChord(char* text, KeyChord& chord) {
    UpperAscii(text);
    if (std::strcmp(text, "NONE") == 0) { chord = {}; return true; }
    KeyChord parsed{};
    char* cursor = text;
    for (;;) {
        char* plus = std::strchr(cursor, '+');
        if (!plus) break;
        *plus = '\0';
        TrimAscii(cursor);
        if (std::strcmp(cursor, "LEADER") == 0) parsed.leader = true;
        else if (std::strcmp(cursor, "CTRL") == 0 || std::strcmp(cursor, "CONTROL") == 0) parsed.mods |= kModCtrl;
        else if (std::strcmp(cursor, "ALT") == 0) parsed.mods |= kModAlt;
        else if (std::strcmp(cursor, "SHIFT") == 0) parsed.mods |= kModShift;
        else return false;
        cursor = plus + 1;
    }
    TrimAscii(cursor);
    parsed.vk = KeyNameToVirtualKey(cursor);
    if (!parsed.vk) return false;
    // CTRL+ALT+DEL never reaches a process; binding it would produce an action
    // that silently never fires.
    if (parsed.vk == VK_DELETE && (parsed.mods & kModCtrl) && (parsed.mods & kModAlt)) return false;
    chord = parsed;
    return true;
}

int ActionIndexByName(const char* name) {
    for (const auto& entry : kActions) {
        if (std::strcmp(entry.name, name) == 0) return static_cast<int>(entry.action);
    }
    return -1;
}

// THE REGISTRY'S `defaultValue` COLUMN WAS NOT A DEFAULT.
//
// Until 2026-09-09 this function set the bindings and the leader key and
// nothing else, and `Setting::defaultValue` was read in exactly ONE place in
// the whole tree: the menu's per-row "reset" button. So the value a fresh
// install actually ran with came from each global's C++ initialiser, and the
// number the registry called its default was a number the menu could type in
// for you. Two sources of truth for one idea, free to disagree, and they did.
//
// Now the registry IS the default. Every row is applied through ApplyValue --
// the same path the ini uses -- so a setting has one declared default, one
// place to change it, and the reset button and a fresh install cannot diverge.
// `registry-crosscheck.sh` already guarantees every registry name has an
// ApplyValue branch, which is what makes this loop total.
//
// ORDER: this runs BEFORE ParseConfigFile at both call sites, so an ini still
// overrides everything here. Settings NOT in the registry keep their C++
// initialisers; those are the ini-only keys, and they have no declared default
// to apply.
// ---------------------------------------------------------------------------
// THE SHIPPED DEFAULTS FOR EVERY KEY THAT HAS NO REGISTRY ROW.
//
// The 60 registry settings declare their own default in kSettings[]. These are
// the ini-only keys -- arming switches, hook arms, geometry references, probe
// gates -- which had no declared default at all: whatever their C++ initialiser
// happened to be WAS the default, scattered across twenty files, and nothing
// stated it in one place or could be reviewed as a set.
//
// Every value here is taken from the wearer's own working configuration, which
// is the only configuration this mod has ever been played on. A fresh install
// with no ini now behaves like that one. That is the point: before this, a
// bare install armed rendering and left the controllers dead, because
// input.synthetic_pad and vrinput.enabled defaulted off and no ini said
// otherwise.
//
// Where a key was set twice in that ini the LAST value is used, which is what
// the parser itself does.
//
// render.width / render.height are deliberately absent: they are absolute
// pixel overrides tuned to one headset, and 0 means derive from whatever
// headset is present -- 100% of its own panel.
struct IniDefault { const char* name; float value; };
constexpr IniDefault kIniDefaults[] = {
    {"ads.brace_force", 0.0f},
    {"ads.face_aim_return", 1.0f},
    {"aim.cmd", 2.0f},
    {"aim.cmd_source", 1.0f},
    {"aim.cmd_yaw", 30.0f},
    {"autoarm", 5.0f},
    {"autoarm.require_live_view", 1.0f},
    {"viewmodel.correction", 1.0f},
    {"dpad.deadzone", 0.60f},
    {"dpad.dwell_ms", 0.0f},
    {"dpad.head_radius", 0.45f},
    {"dpad.head_up", -0.15f},
    {"dpad.modifier", 3.0f},
    {"eye.hook", 1.0f},
    {"eye.lean_follow", 0.0f},
    {"game.fov_auto", 1.0f},
    {"render.min_aspect_floor", 1.0f},
    {"game.fov_scale", 1.51f},
    {"grip.perweapon", 1.0f},
    {"grip.ref_fwd", 6.04f},
    {"grip.ref_right", 2.73f},
    {"grip.ref_up", -6.09f},
    {"hand.ref_yaw", 1.0f},
    {"hand.source", 3.0f},
    {"hand.yaw_rebase", 1.0f},
    {"hud.cockpit_skip", 14848.0f},
    {"hud.fov_match", 1.0f},
    {"hud.fov_origin_max", 32.0f},
    {"hud.marker_unzoom", 0.0f},
    {"hud.nameplate_mode", 0.0f},
    {"hud.widget_arm", 1.0f},
    {"hud.widget_named_only", 0.0f},
    // QUIET BY DEFAULT. The release build writes only failures, refusals,
    // crashes and the run identity. 1 writes everything, at a syscall a line.
    {"diag.verbose", 0.0f},
    {"hud.widget_screenspace_only", 1.0f},
    {"hud.canvas_centre", 0.0f},
    {"hud.widget_skip", 0.0f},
    {"hud.widget_types", 217.0f},
    {"input.force_controller_mode", 1.0f},
    {"input.hold_keyboard_mode", 0.0f},
    {"input.keyboard_menu_nav", 0.0f},
    {"input.synthetic_pad", 1.0f},
    {"input.y_longpress_ms", 350.0f},
    {"marker.gain", 0.55f},
    {"marker.hook", 1.0f},
    {"marker.reframe", 0.0f},
    {"menu.distance_m", 1.5f},
    {"menu.ui_scale", 1.9f},
    {"menu.width_m", 1.7f},
    {"pin.hand", 1.0f},
    {"profile", 2.0f},
    {"render.windowed", 1.0f},
    // THE STEREO KEYS ARE NOT ONLY STEREO, and removing them broke the body.
    //
    // AdvanceSceneReentry() opens with
    //     if (!g_censusWanted && !g_reentryWanted && !RtvCensusWanted()) return;
    // and further down, gated only on a world being ready, it calls
    // InstallVbBindCensus() -- the per-mesh gate that hides the pilot body and
    // leaves the gloves drawing, which is the hands mechanism the wearer
    // actually uses. Strip these keys and all three flags go false, the function
    // returns on its first line, the hook never installs, and body.show = 1 does
    // nothing. A clean run showed it plainly: "VBBIND: not installed yet" every
    // five seconds for 158 seconds.
    //
    // These are the wearer's own tested values. reentry_frames = 0 is what stops
    // same-frame doubling actually happening; reentry = 1 only keeps the module
    // alive so the mechanisms hosted inside it can install.
    //
    // Second time in two days a key was removed on the strength of its name and
    // took a live mechanism with it -- rui.census was the first.
{"stereo.batch2_ipd", 0.0f},
{"stereo.batch2_ipd_mode", 2.0f},
{"stereo.engine_ipd", 0.0f},
{"stereo.own_job_group", 0.0f},
{"stereo.predraw_bind", 0.0f},
{"stereo.predraw_camera", 0.0f},
{"stereo.prewait_job", 0.0f},
{"stereo.prewait_ms", 250.0f},
{"stereo.recompute_blocks", 0.0f},
{"stereo.reentry", 1.0f},
{"stereo.reentry_frames", 0.0f},
{"stereo.reentry_guard", 1.0f},
{"stereo.restore_latch", 0.0f},
{"stereo.substitute", 0.0f},
{"stereo.substitute_reads", 0.0f},
    // THE DIAGNOSTICS ARE BACK, AND REMOVING THEM WAS THE MISTAKE.
    //
    // Three regressions in one day, all the same shape: a key removed because
    // its NAME matched /census|probe/, which was in fact holding a live
    // mechanism open. rui.census arms the HUD widget substitution. The stereo
    // keys keep AdvanceSceneReentry alive and the glove/arms mesh gate installs
    // from inside it. frustum.census feeds the main-scene frustum measurement
    // the projection layer waits on -- without it the game renders to a flat
    // quad instead of stereo, which is what the wearer saw.
    //
    // Three for three. The name of a switch says nothing about what depends on
    // it. They ship at the wearer's own tested values until each is traced to
    // its consumers individually, with a run behind it.
    {"ads.probe", 0.0f},
    {"ads.trace", 1.0f},
    {"anchor.probe", 1.0f},
    {"crash.selftest", 0.0f},
    {"frustum.census", 1.0f},
    {"hud.census", 1.0f},
    {"hud.widget_census", 1.0f},
    {"jt.probe", 1.0f},
    {"natives.dump", 0.0f},
    {"rui.census_early", 0.0f},
    {"rui.dump", 1.0f},
    {"rui.warp_probe", 0.0f},
    {"rui.arm", 0.0f},
    {"rui.block", 1.0f},
    // rui.census IS NOT A DIAGNOSTIC, whatever its name says. The HUD widget
    // substitution happens from the census hook -- hud.widget_arm's own log line
    // says "every type-3 widget THE CENSUS NAMES is substituted... at each
    // widget's FIRST draw FROM THE CENSUS HOOK". Removing it on 2026-09-09
    // because the name matched /census/ silently disarmed every HUD placement
    // the wearer had tuned, and the run that found it was the clean-profile
    // test. Classify by what a thing DOES, not by what it is called -- the rule
    // this project already had, applied to a config key for the first time.
    {"rui.census", 1.0f},
    // DEFAULTED OFF 2026-09-11, and the number is measured, not guessed.
    // The cost run caught it: ENGINE DATA SCAN at t=20551.6 and, 1.2 ms
    // later, HITCH "one frame spent 79.47 ms ... ruiCens+Imm 79.43". One
    // frame of 79 ms on every clean install, for a sweep of 24 million
    // qwords over the whole of engine.dll that then reports itself
    // DISQUALIFIED. Nothing outside this file asks whether it is on.
    // Evidence: docs/STUTTER-ATTRIBUTED-2026-09-11.md.
    {"rui.immediate", 0.0f},
    {"rui.layer_type", 15.0f},
    {"rui.ll_arm", 0.0f},
    {"rui.marker_target", 526768.0f},
    {"rui.suspects", 0.0f},
    {"titan.gate", 1.0f},
    {"use.aim", 1.0f},
    {"use.hook", 1.0f},
    {"viewmodel.correct_hud_pass", 0.0f},
    {"viewmodel.detach_lean", 1.0f},
    {"viewmodel.hidden", 0.0f},
    {"viewmodel.hide_pulse", 0.0f},
    {"vrinput.enabled", 1.0f},
    {"weapon.mono_eye", 2.0f},
    {"weapon.mono_when_zoomed", 1.0f},
    {"xr.declare_rendered_fov", 1.0f},
    {"xr.world_rect_gate", 1.0f},
    {"hud2d.head_anchor", 0.0f},
    {"xr.fit_horizontal", 0.0f},
    {"xr.headlock_quad", 0.0f},
    {"xr.lens_shear", 0.0f},
    {"xr.letterbox_floor", -1.0f},
    {"xr.match_headset_fov", 0.0f},
    {"xr.stale_ms", 250.0f},
    {"xr.viewport_full", 0.0f},
};

void ApplyValue(const char* name, float value);  // defined below; ApplyDefaults applies the registry through it

void ApplyDefaults() {
    for (const auto& entry : kActions) g_bindings[static_cast<size_t>(entry.action)] = entry.defaultChord;
    g_requireForeground = true;
    g_leaderKey = {VK_PAUSE, 0, false};
    g_leaderTimeoutMs = 4000;

    // The ini-only keys first, then the registry. The two sets are disjoint --
    // every name in kIniDefaults is absent from kSettings, which
    // registry-crosscheck.sh now checks -- so the order is not load-bearing.
    for (const auto& entry : kIniDefaults) ApplyValue(entry.name, entry.value);

    const int count = SettingCount();
    for (int i = 0; i < count; ++i) {
        const Setting* setting = SettingAt(i);
        if (setting) ApplyValue(setting->name, setting->defaultValue);
    }
}

void ApplyValue(const char* name, float value) {
    if (std::strcmp(name, "hotkeys.require_foreground") == 0) { g_requireForeground = value != 0.0f; return; }
    if (std::strcmp(name, "hotkeys.leader_timeout_ms") == 0) {
        // Long enough to be found by feel with a headset on, short enough that
        // a forgotten leader press cannot arm something minutes later.
        if (value >= 500.0f && value <= 30000.0f) g_leaderTimeoutMs = static_cast<unsigned>(value);
        return;
    }
    if (std::strcmp(name, "viewmodel.exclude_lower_camera") == 0) { SetViewmodelLowerCameraExcluded(value != 0.0f); return; }
    if (std::strcmp(name, "viewmodel.include_near_origin") == 0) { SetViewmodelIncludeNearOrigin(value != 0.0f); return; }
    if (std::strcmp(name, "viewmodel.frame_locked_angles") == 0) { SetViewmodelFrameLockedAngles(value != 0.0f); return; }
    if (std::strcmp(name, "viewmodel.display_pose_correction") == 0) { SetViewmodelDisplayPoseCorrection(value != 0.0f); return; }
    if (std::strcmp(name, "weapon.mono_eye") == 0) { SetWeaponMonoEye(static_cast<int>(value)); return; }
    if (std::strcmp(name, "weapon.size") == 0) { SetWeaponSize(value); return; }
    if (std::strcmp(name, "weapon.mono_when_zoomed") == 0) { SetWeaponMonoOnlyWhenZoomed(value != 0.0f); return; }
    if (std::strcmp(name, "weapon.zoom_threshold") == 0) { SetZoomThreshold(value); return; }
    if (std::strcmp(name, "viewmodel.pin_mode") == 0) { SetViewmodelPinMode(static_cast<int>(value)); return; }
    if (std::strcmp(name, "viewmodel.detach_lean") == 0) { SetViewmodelDetachLean(value); return; }
    if (std::strcmp(name, "headtracking.neck_model") == 0) { SetNeckModel(value != 0.0f); return; }
    // Held and committed together, so one key does not apply half a pivot.
    if (std::strcmp(name, "headtracking.neck_forward") == 0) { g_neckForward = value; SetNeckOffsets(g_neckForward, g_neckUp); return; }
    if (std::strcmp(name, "headtracking.neck_up") == 0) { g_neckUp = value; SetNeckOffsets(g_neckForward, g_neckUp); return; }
    if (std::strcmp(name, "view.step_guard") == 0) { SetStepGuard(value != 0.0f); return; }
    if (std::strcmp(name, "view.step_guard_threshold") == 0) { SetStepGuardThreshold(value); return; }
    if (std::strcmp(name, "xr.match_headset_fov") == 0) { SetMatchHeadsetFov(value != 0.0f); return; }
    // Both dimensions are needed before anything is applied, so they are held
    // and committed together rather than one key changing the mode twice.
    // render.width / render.height ARE BACK, and they are the wearer's tuned
    // known-good resolution. RESTORED after I retired them and broke the FOV.
    //
    // 5840x3648 is aspect 1.60, which gives the game a 115.77 x 89.73 frustum.
    // I replaced it with the runtime's recommended 4032x3648 -- aspect 1.105 --
    // on the reasoning that the runtime is the authority on its own per-eye size.
    // It is, for the SIZE. It is not for the SHAPE: the game's horizontal field
    // of view follows the buffer aspect, so rendering the runtime's shape
    // narrowed the wearer's view of the world.
    //
    // That was a change to a measured, tuned setting made as a side effect of a
    // menu tidy-up, and it is exactly the kind of thing that should never happen
    // quietly. These win over render.scale, which is unset by default.
    if (std::strcmp(name, "render.width") == 0) { g_renderWidth = static_cast<int>(value); SetRequestedRenderResolution(g_renderWidth, g_renderHeight); return; }
    if (std::strcmp(name, "render.height") == 0) { g_renderHeight = static_cast<int>(value); SetRequestedRenderResolution(g_renderWidth, g_renderHeight); return; }
    if (std::strcmp(name, "render.scale") == 0) { SetRenderScale(value); return; }
    if (std::strcmp(name, "render.windowed") == 0) { SetRequestedRenderWindowed(value != 0.0f); return; }
    if (std::strcmp(name, "pin.fwd") == 0) { g_pinForward.store(value, std::memory_order_release); return; }
    if (std::strcmp(name, "pin.right") == 0) { g_pinRight.store(value, std::memory_order_release); return; }
    if (std::strcmp(name, "pin.up") == 0) { g_pinUp.store(value, std::memory_order_release); return; }
    if (std::strcmp(name, "pin.spin") == 0) { g_pinSpin.store(value, std::memory_order_release); return; }
    if (std::strcmp(name, "pin.hand") == 0) { SetPlacementPinHandDriven(value != 0.0f); return; }
    if (std::strcmp(name, "hand.use_roll") == 0) { g_useRoll.store(value != 0.0f ? 1.0f : 0.0f, std::memory_order_release); return; }
    if (std::strcmp(name, "hand.use_pitch") == 0) { g_usePitch.store(value != 0.0f ? 1.0f : 0.0f, std::memory_order_release); return; }
    if (std::strcmp(name, "hand.grip_fwd") == 0) { g_gripForward.store(value, std::memory_order_release); return; }
    if (std::strcmp(name, "hand.grip_right") == 0) { g_gripRight.store(value, std::memory_order_release); return; }
    if (std::strcmp(name, "hand.grip_up") == 0) { g_gripUp.store(value, std::memory_order_release); return; }
    if (std::strcmp(name, "hand.off_fwd") == 0) { g_handOffForward.store(value, std::memory_order_release); return; }
    if (std::strcmp(name, "hand.off_left") == 0) { g_handOffLeft.store(value, std::memory_order_release); return; }
    if (std::strcmp(name, "hand.off_up") == 0) { g_handOffUp.store(value, std::memory_order_release); return; }
    if (std::strcmp(name, "hand.off_pitch") == 0) { g_handOffPitch.store(value, std::memory_order_release); return; }
    if (std::strcmp(name, "hand.off_yaw") == 0) { g_handOffYaw.store(value, std::memory_order_release); return; }
    if (std::strcmp(name, "hand.off_roll") == 0) { g_handOffRoll.store(value, std::memory_order_release); return; }
    if (std::strcmp(name, "hand.yawcomp") == 0) { g_handYawComp.store(value, std::memory_order_release); return; }
    if (std::strcmp(name, "hand.yaw_rebase") == 0) { SetHandYawRebase(value != 0.0f); return; }
    if (std::strcmp(name, "hand.ref_yaw") == 0) { SetHandReferenceYaw(value != 0.0f); return; }
    // WHICH PROFILE IS LIVE, SAID OUT LOUD. The two profiles are file copies
    // with the same name, so nothing in the log could ever distinguish them and
    // the standing rule -- every report names the active profile or its visual
    // claims are void -- rested on whoever did the copy remembering. It does
    // not any more. 1 = FLAT (monitor), 2 = VR.
    if (std::strcmp(name, "profile") == 0) {
        const int p = static_cast<int>(value);
        // STORED, not only announced. The render target has to know: see the
        // note at SetFlatProfile for the flat run this cost.
        SetFlatProfile(p == 1);
        Tf2VrLog(p == 2 ? "[TF2VR] ======== ACTIVE PROFILE: PROFILE-VR (headset) ========\n"
               : p == 1 ? "[TF2VR] ======== ACTIVE PROFILE: PROFILE-FLAT (monitor) ========\n"
                        : "[TF2VR] ======== ACTIVE PROFILE: UNNAMED -- visual claims from this "
                          "run are unattributable ========\n");
        return;
    }
    if (std::strcmp(name, "autoarm") == 0) { SetAutoArmTarget(static_cast<int>(value)); return; }
    // PLAN-TITAN P0-b. Arms the deliberate-fault key and NOTHING else -- it does
    // not fault by itself, so an ini left with this at 1 costs a log line, not a
    // session. No registry row: a control that ends the game does not belong on
    // a slider a wearer can reach by feel with a headset on.
    // F0's scheduler-precondition instrument. 0 off, 1 JT counters only,
    // 2 counters plus the scene-draw caller census. Read-only at every value:
    // it reads three exported tier0 counters and, at 2, swaps one .rdata vtable
    // slot for a thunk that logs and tail-jumps to the original. No registry
    // row -- a diagnostic that answers a question about the job scheduler is not
    // a control a wearer should meet on a slider, and `crash.selftest` sets the
    // precedent for an ini-only arm.
    // F1. Makes the nested scene-draw call AVAILABLE; it still fires only on
    // the arm key, once per press, and disarms itself immediately. Default 0,
    // so the "nothing arms on load" rule holds: an ini left at 1 costs a log
    // line, not a run. No registry row -- this is a render-path experiment, not
    // a wearer control.
    if (std::strcmp(name, "stereo.reentry") == 0) { SetSceneReentryWanted(value != 0.0f); return; }
    // The guard for the defect three runs measured: the nested pass releasing a
    // job group the engine already released. Suppresses ONLY that repeat call.
    // Default 0, so the measured behaviour is what an unchanged ini reproduces.
    if (std::strcmp(name, "stereo.reentry_guard") == 0) { SetSceneReentryGuard(value != 0.0f); return; }
    if (std::strcmp(name, "stereo.restore_latch") == 0) { SetSceneLatchRestore(value != 0.0f); return; }
    if (std::strcmp(name, "stereo.recompute_blocks") == 0) { SetSceneRecomputeBlocks(value != 0.0f); return; }
    if (std::strcmp(name, "stereo.own_job_group") == 0) { SetSceneOwnJobGroup(value != 0.0f); return; }
    if (std::strcmp(name, "stereo.prewait_job") == 0) { SetSceneJobPreWait(value != 0.0f); return; }
    if (std::strcmp(name, "stereo.prewait_ms") == 0) { SetSceneJobPreWaitMs(value); return; }
    if (std::strcmp(name, "stereo.reentry_frames") == 0) { SetSceneReentryFrames(static_cast<int>(value)); return; }
    // M1. The OMSetRenderTargets census: read-only, ini-only, no registry row,
    // same reasoning as `stereo.reentry` above. Default 0, so an unchanged ini
    // hooks nothing at all.
    if (std::strcmp(name, "stereo.stereo_targets") == 0) { SetRtvCensusWanted(static_cast<int>(value)); return; }
    // M2. The one behavioural change of this rung. Default 0, so an unchanged
    // ini censuses and substitutes nothing. CTRL+F2 is its positive control.
    if (std::strcmp(name, "stereo.substitute") == 0) { SetRtvSubstituteWanted(static_cast<int>(value)); return; }
    // M5. The read side: batch 2's post samples batch 2's own world, not batch 1's.
    if (std::strcmp(name, "stereo.substitute_reads") == 0) { SetRtvReadSubstituteWanted(static_cast<int>(value)); return; }
    // M5. The camera written immediately before batch 2's scene draws.
    if (std::strcmp(name, "stereo.predraw_camera") == 0) { SetPreDrawCameraIpd(value); return; }
    // M5. Bind our own camera buffer for batch 2 instead of rewriting the engine's.
    if (std::strcmp(name, "stereo.predraw_bind") == 0) { SetPreDrawCameraBind(static_cast<int>(value)); return; }
    // M3. The eye translation applied to batch 2 only. 0 is the positive control.
    if (std::strcmp(name, "stereo.batch2_ipd") == 0) { SetBatch2Ipd(value); return; }
    // 0 = batch-2 only; 1 = every qualifying upload, which is what the shipped
    // test.eyetranslation does. Makes the batch-2 gate a single testable variable.
    if (std::strcmp(name, "stereo.batch2_ipd_mode") == 0) { SetBatch2IpdMode(static_cast<int>(value)); return; }
    // N2/C-ENGINE. The eye offset written into the ENGINE's own view matrix
    // before the nested call, not into anything D3D-side. 0 disables it.
    if (std::strcmp(name, "stereo.engine_ipd") == 0) { SetEngineIpd(value); return; }
    // GATE 1 is done and confirmed, so hiding the arms is a shipped feature
    // rather than a probe: having to press F4 every session, and losing it on
    // any restart, makes the body reappear in the middle of calibration.
    // Opt-in from the ini, so "nothing arms on load" still holds for anyone who
    // has not asked for it.
    if (std::strcmp(name, "body.show") == 0) { SetBodyShowMode(static_cast<int>(value)); return; }
    if (std::strcmp(name, "vrinput.enabled") == 0) { SetVrInputEnabled(value != 0.0f); return; }
    // PLAN-CURRENT P0. `aim.cmd` PATCHES CODE, so it defaults to 0 and the
    // "nothing arms on load" rule holds for anyone who has not asked for it.
    // The other two only change what an already-armed drive writes.
    if (std::strcmp(name, "aim.cmd") == 0) { SetAimCmdState(static_cast<int>(value)); return; }
    if (std::strcmp(name, "aim.cmd_source") == 0) { SetAimCmdSource(static_cast<int>(value)); return; }
    if (std::strcmp(name, "aim.cmd_yaw") == 0) { SetAimCmdYawDegrees(value); return; }
    // PLAN-ADS S2. READ-ONLY, so it may arm itself from the ini: it patches no
    // code and writes no field, which is the whole reason the "nothing arms on
    // load" rule does not have to cover it. It needs `aim.cmd = 1` beside it or
    // it has no aim ray to compare against, and it says so in its own log.
    //
    // Deliberately NOT registry rows: these are diagnostics, not settings the
    // wearer tunes, and settings_registry.h is explicit that a registry entry
    // needs a real setting behind it. They join the ~97 other ini-only keys.
    // PLAN-ADS C1. A registry setting, so it is dialled live in the panel during
    // H1 rather than rebuilt between runs. Default 0 = the game's own behaviour,
    // so it arms nothing on load.
    if (std::strcmp(name, "ads.max_magnification") == 0) { SetAdsMaxMagnification(value); return; }
    if (std::strcmp(name, "ads.optic_passthrough") == 0) { SetAdsOpticPassthrough(value); return; }
    // PLAN-ADS C4, the brace. A gain of 1.0 IS "off", so none of these arms
    // anything on load and there is no flag that can disagree with the value.
    if (std::strcmp(name, "ads.brace_gain") == 0) { SetAdsBraceGain(value); return; }
    if (std::strcmp(name, "ads.brace_cone_deg") == 0) { SetAdsBraceConeDegrees(value); return; }
    if (std::strcmp(name, "ads.brace_readopt_deg") == 0) {
        SetAdsBraceReadoptDegrees(value);
        return;
    }
    // Diagnostic, ini-only, and NOT a registry row: it holds the brace engaged
    // without the trigger so the ratio falsifiers can be taken on a monitor,
    // where IsAdsHeld() is false by definition because vr input is off.
    if (std::strcmp(name, "ads.brace_force") == 0) { SetAdsBraceForced(value != 0.0f); return; }
    // The same arm F9 makes, from the ini, because a measuring run should not
    // depend on a keypress somebody has to remember. S2 came back with
    // worldTan 0.0000 on every line for exactly that reason: the frustum is
    // read inside the UpdateSubresource interception, and this is what installs
    // it. The want is deferred until the swapchain is verified -- see
    // present_hook.h.
    if (std::strcmp(name, "viewmodel.compensation") == 0) {
        SetViewmodelCompensationWanted(value != 0.0f);
        return;
    }
    // The upload census, from the ini. It already existed and was reachable only
    // from a keypress, which is why two runs went by without it.
    //
    // WHY IT IS NEEDED. IsMainSceneCamera requires a near-plane element within
    // 0.5 of -7.0, and that constant was measured over two VR captures. The S2B
    // run had the interception installed and working -- 552 entry calls, 240
    // camera-sized -- and still classified NOTHING as the main scene, so the
    // frustum was never read. This census prints every camera-sized upload
    // family it sees, near plane and all, which turns "the classifier rejects
    // everything flat" from a guess into the actual numbers.
    if (std::strcmp(name, "grip.perweapon") == 0) { SetPerWeaponGrip(value != 0.0f); return; }
    if (std::strcmp(name, "grip.ref_fwd") == 0) { SetGripReference(0, value); return; }
    if (std::strcmp(name, "grip.ref_right") == 0) { SetGripReference(1, value); return; }
    if (std::strcmp(name, "grip.ref_up") == 0) { SetGripReference(2, value); return; }
    if (std::strcmp(name, "hud2d.shift_x") == 0 || std::strcmp(name, "hud2d.shift_y") == 0 ||
        std::strcmp(name, "hud2d.zoom") == 0) {
        float x = 0.0f, y = 0.0f, z = 1.0f;
        ReadHud2dPlacement(&x, &y, &z);
        if (std::strcmp(name, "hud2d.shift_x") == 0) x = value;
        else if (std::strcmp(name, "hud2d.shift_y") == 0) y = value;
        else z = value;
        SetHud2dPlacement(x, y, z);
        return;
    }
    // RUNG 3, the RUI layer coordinate space -- the lever that actually moves the
    // HUD. Bigger space = smaller HUD, pulled in from the edges, size and
    // position together. Inert until the key arms it, so nothing arms on load.
    if (std::strcmp(name, "rui.inset") == 0) { SetRuiInset(value); return; }
    if (std::strcmp(name, "rui.layer_type") == 0) { SetRuiLayerType(static_cast<int>(value)); return; }
    // SHIPPED, so it may arm itself -- the same exception arms.collapse already
    // has. It patches a call site, which is why it stays opt-in and off by
    // default: "nothing arms on load" still holds for anyone who has not asked.
    // It fires ONCE, so F6 remains a straight A/B against the original.

    if (std::strcmp(name, "ads.probe") == 0) { SetAdsProbeEnabled(value != 0.0f); return; }
    if (std::strcmp(name, "ads.probe_attachment") == 0) {
        SetAdsProbeAttachment(static_cast<int>(value));
        return;
    }
    if (std::strcmp(name, "ads.trace") == 0) { SetAdsTrace(value != 0.0f); return; }
    if (std::strcmp(name, "anchor.probe") == 0) { SetAnchorProbeEnabled(value != 0.0f); return; }
    if (std::strcmp(name, "camera.upload_census") == 0) {
        SetUploadCensusEnabled(value != 0.0f);
        return;
    }
    if (std::strcmp(name, "crash.selftest") == 0) { SetCrashSelfTestArmed(value != 0.0f); return; }
    if (std::strcmp(name, "frustum.census") == 0) { SetFrustumCensusEnabled(value != 0.0f); return; }
    if (std::strcmp(name, "hud.census") == 0) { SetUploadCensusEnabled(value != 0.0f); return; }
    if (std::strcmp(name, "hud.widget_census") == 0) { SetRuiWidgetCensus(value != 0.0f); return; }
    if (std::strcmp(name, "jt.probe") == 0) { SetJtProbeMode(static_cast<int>(value)); return; }
    if (std::strcmp(name, "lockhud.census") == 0) { SetLockHudCensusEnabled(value != 0.0f); return; }
    if (std::strcmp(name, "log.debug_channel") == 0) { Tf2VrSetLogDebugChannel(value != 0.0f); return; }
    if (std::strcmp(name, "natives.dump") == 0) { SetSquirrelNativeDumpEnabled(value != 0.0f); return; }
    if (std::strcmp(name, "pin.report") == 0) { SetPlacementPinReportEnabled(value != 0.0f); return; }
    if (std::strcmp(name, "rui.census_early") == 0) { SetRuiCensusEarly(value != 0.0f); return; }
    if (std::strcmp(name, "rui.dump") == 0) { SetRuiAssetDump(value != 0.0f); return; }
    if (std::strcmp(name, "rui.warp_probe") == 0) { SetRuiWarpProbe(value != 0.0f); return; }
    if (std::strcmp(name, "xr.cost_report") == 0) { PluginCost::SetEnabled(value != 0.0f); return; }
    if (std::strcmp(name, "rui.arm") == 0) { SetRuiArmOnLoad(value != 0.0f); return; }
    // PHASE 1 RUNG A1. READ-ONLY: it installs the same call-site patch (which
    // is why it is opt-in from the ini rather than automatic) and writes
    // nothing. Leave `rui.arm` at 0 while this is on -- the inset substitutes
    // the layer type byte FC500 branches on, and the identity resolved at
    // 0xFC6B1 is downstream of that branch, so arming both would census our
    // own substitution rather than the game's layers.
    // A2 is answered, so F5 steps A3. This puts it back on A2 for a run that
    // needs to hunt a different element -- the health readout was never on
    // screen during the A2 sweep, so that question is still open.
    if (std::strcmp(name, "rui.a2") == 0) { SetRuiA2LadderSelected(value != 0.0f); return; }
    // PHASE 1 RESULT: the lower-left group. Scaling its coordinate space scales
    // it about the SCREEN CENTRE by the reciprocal -- smaller, off the bottom
    // edge and in from the left, in one number. 1.0 is inert and is the default,
    // so the "nothing arms on load" rule holds for anyone who has not asked.
    if (std::strcmp(name, "rui.ll_scale") == 0) { SetRuiLowerLeftScale(value); return; }
    if (std::strcmp(name, "rui.ll_arm") == 0) { SetRuiLowerLeftArmOnLoad(value != 0.0f); return; }
    if (std::strcmp(name, "rui.a3") == 0) { SetRuiA3LadderSelected(value != 0.0f); return; }
    if (std::strcmp(name, "rui.suspects") == 0) { SetRuiSuspectLadder(value != 0.0f); return; }
    if (std::strcmp(name, "rui.census") == 0) { SetRuiCensusWanted(value != 0.0f); return; }
    if (std::strcmp(name, "rui.block") == 0) { SetRuiBlockCensusSelected(value != 0.0f); return; }
    // Read-only: substitutes one world-marker widget as a pass-through and points
    // the block census at it. See rui_probe.h for the candidate RVAs.
    if (std::strcmp(name, "rui.marker_target") == 0) { SetRuiMarkerTarget(static_cast<unsigned>(value)); return; }
    // THE HUD SIZE CONTROL (rui_asset_dump.h). Named hud.widget_scale, NOT
    // hud.size. The old hud.scale was measured inert and was REMOVED on
    // 2026-09-06 with its mechanism, so the similar-name trap is gone.
    if (std::strcmp(name, "hud.widget_arm") == 0) { SetRuiHudSizeArm(value != 0.0f); return; }
    if (std::strcmp(name, "hud.widget_scale") == 0) { SetHudWidgetScale(value); return; }
    if (std::strcmp(name, "hud.widget_skip") == 0) { SetHudWidgetSkip(static_cast<unsigned>(value)); return; }
    if (std::strcmp(name, "hud.widget_named_only") == 0) { SetHudWidgetNamedOnly(value != 0.0f); return; }
    if (std::strcmp(name, "hud.ll_shift_x") == 0) { SetHudLowerLeftShiftX(value); return; }
    if (std::strcmp(name, "hud.ll_shift_x_titan") == 0) { SetHudLowerLeftShiftXTitan(value); return; }
    if (std::strcmp(name, "diag.verbose") == 0) { Tf2VrSetLogVerbose(value != 0.0f); return; }
    if (std::strcmp(name, "hud.prompt_scale") == 0) { SetHudPromptScale(value); return; }
    if (std::strcmp(name, "hud.prompt_shift_y") == 0) { SetHudPromptShiftY(value); return; }
    if (std::strcmp(name, "hud.instruction_scale") == 0) { SetHudInstructionScale(value); return; }
    if (std::strcmp(name, "hud.instruction_shift_y") == 0) { SetHudInstructionShiftY(value); return; }
    if (std::strcmp(name, "hud.timer_scale") == 0) { SetHudTimerScale(value); return; }
    if (std::strcmp(name, "hud.timer_shift_x") == 0) { SetHudTimerShiftX(value); return; }
    if (std::strcmp(name, "hud.timer_shift_y") == 0) { SetHudTimerShiftY(value); return; }
    if (std::strcmp(name, "hud.highlight_scale") == 0) { SetHudHighlightScale(value); return; }
    if (std::strcmp(name, "hud.widget_screenspace_only") == 0) { SetHudWidgetScreenSpaceOnly(value != 0.0f); return; }
    if (std::strcmp(name, "hud.canvas_centre") == 0) { SetHudCanvasCentre(value != 0.0f); return; }
    if (std::strcmp(name, "hud.cockpit_scale") == 0) { SetHudCockpitScale(value); return; }
    if (std::strcmp(name, "hud.cockpit_skip") == 0) { SetHudCockpitSkip(static_cast<unsigned>(value)); return; }
    if (std::strcmp(name, "hud.bars_scale") == 0) { SetHudBarsScale(value); return; }
    if (std::strcmp(name, "hud.bars_shift_y") == 0) { SetHudBarsShiftY(value); return; }
    if (std::strcmp(name, "hud.nameplate_scale") == 0) { SetHudNameplateScale(value); return; }
    if (std::strcmp(name, "hud.nameplate_mode") == 0) { SetHudNameplateMode(static_cast<int>(value)); return; }
    if (std::strcmp(name, "hud.marker_unzoom") == 0) { SetHudMarkerUnzoom(value != 0.0f); return; }
    if (std::strcmp(name, "hud.widget_types") == 0) { SetRuiHudSizeTypes(static_cast<unsigned>(value)); return; }
    if (std::strcmp(name, "hud.marker_size") == 0) { SetRuiMarkerSize(value); return; }
    // Read-only: the marker widget's VM state dumped on its draws (rui_asset_dump.h).
    // Read-only: the SECOND RUI draw path and the descriptor pool (rui_immediate.h).
    if (std::strcmp(name, "rui.immediate") == 0) { SetRuiImmediateWanted(value != 0.0f); return; }
    // The missile-lock rings: project through the RENDERED view, not the aim (lock_hud.h).
    if (std::strcmp(name, "lockhud.viewfix") == 0) { SetLockHudViewFix(value != 0.0f); return; }
    if (std::strcmp(name, "lockhud.ring_scale") == 0) { SetLockHudRingScale(value); return; }
    // THE TEXT-POSITION FIX (rui_asset_dump.h): where the authored block is pinned.
    if (std::strcmp(name, "hud.marker_text_x") == 0) { SetRuiMarkerTextX(value); return; }
    // USE TARGETING (use_target.h). use.hook 0 leaves the game code untouched;
    // use.aim is the arm the run starts on: 0 off, 1 along the hand, 2 from it.
    if (std::strcmp(name, "use.hook") == 0) { SetUseTargetHookWanted(value != 0.0f); return; }
    if (std::strcmp(name, "use.aim") == 0) { SetUseAimArm(static_cast<int>(value)); return; }
    // WORLD MARKERS (world_marker.h): the vec3 evaluator swaps and the reframe arm.
    if (std::strcmp(name, "marker.hook") == 0) { SetWorldMarkerHookWanted(value != 0.0f); return; }
    if (std::strcmp(name, "marker.reframe") == 0) { SetWorldMarkerArm(static_cast<int>(value)); return; }
    if (std::strcmp(name, "marker.gain") == 0) { SetWorldMarkerGain(value); return; }
    if (std::strcmp(name, "hud.fov_match") == 0) { SetHudFovMode(static_cast<int>(value)); return; }
    // The refit's origin bound in units (rui/camera_update_hook.h). Default 32.
    if (std::strcmp(name, "hud.fov_origin_max") == 0) { SetHudFovOriginMax(value); return; }
    // PLAN-CURRENT F1/F2: the non-reticle HUD's size, on the 64-byte
    // pixel-ortho family R1 identified. 1.0 is inert and is the default, so
    // the "nothing arms on load" rule holds. The anchor is separate because
    // which point the scale is taken about is a MEASURED question that F1
    // answers -- see the note in camera_update_hook.h.
    // THE RETICLE. Patches code when it is off 1.0, so 1.0 is the default and
    // the "nothing arms on load" rule holds for anyone who has not asked.
    if (std::strcmp(name, "crosshair.scale") == 0) { SetCrosshairScale(value); return; }
    // The reticle removed outright, which is a different mechanism from the
    // scale above: one int store to the crosshair state global, no code patch.
    if (std::strcmp(name, "reticle.hidden") == 0) { SetReticleHidden(value != 0.0f); return; }
    if (std::strcmp(name, "ads.keep_reticle") == 0) { SetAdsKeepReticle(value != 0.0f); return; }
    if (std::strcmp(name, "ads.lock") == 0) { SetAdsLock(value != 0.0f); return; }
    if (std::strcmp(name, "ads.turn_scale") == 0) { SetAdsTurnScale(value); return; }
    if (std::strcmp(name, "ads.face_aim") == 0) { SetAdsFaceAim(value != 0.0f); return; }
    if (std::strcmp(name, "ads.face_aim_return") == 0) { SetAdsFaceAimReturn(value != 0.0f); return; }
    if (std::strcmp(name, "ads.face_aim_pitch") == 0) { SetAdsFaceAimPitch(value != 0.0f); return; }
    // THE D-PAD GESTURE. Titan mode uses all four directions plus View, and
    // every button on both controllers was already spoken for.
    // Y's long press = the View button (BACK), which restores faded waypoint
    // markers. 0 disables it and returns Y to a press-edge weapon change.
    if (std::strcmp(name, "input.y_longpress_ms") == 0) { SetYLongPressMs(static_cast<unsigned>(value)); return; }
    if (std::strcmp(name, "dpad.modifier") == 0) { SetDpadModifierSource(static_cast<int>(value)); return; }
    if (std::strcmp(name, "dpad.head_up") == 0) { SetDpadHeadGestureUp(value); return; }
    if (std::strcmp(name, "dpad.head_radius") == 0) { SetDpadHeadGestureRadius(value); return; }
    if (std::strcmp(name, "dpad.dwell_ms") == 0) { SetDpadDwellMs(static_cast<int>(value)); return; }
    if (std::strcmp(name, "dpad.deadzone") == 0) { SetDpadDeadzone(value); return; }
    // The synthetic pose's TILT. Without these the harness recentres level
    // every time, and a composition that subtracts the reference pitch/roll is
    // indistinguishable flat from one that does not -- a null A/B.
    if (std::strcmp(name, "sim.pitch") == 0) { SetSyntheticPosePitch(value); return; }
    if (std::strcmp(name, "sim.roll") == 0) { SetSyntheticPoseRoll(value); return; }
    // Arms the synthetic pose from the INI so a flat test needs no keypress.
    // A toggle that silently failed to take is indistinguishable from a fix
    // that did nothing, and this project has made that mistake three times.
    // Put this AFTER sim.pitch/sim.roll in the file so the confirming log line
    // reports the tilt that is actually armed.
    if (std::strcmp(name, "sim.enable") == 0) { SetSyntheticPoseEnabled(value != 0.0f); return; }
    if (std::strcmp(name, "viewmodel.eye_basis_prerotation") == 0) { SetViewmodelEyeBasisPreRotation(value != 0.0f); return; }
    if (std::strcmp(name, "viewmodel.match_world_fov") == 0) { SetViewmodelMatchWorldFov(value != 0.0f); return; }
    if (std::strcmp(name, "viewmodel.correct_hud_pass") == 0) { SetCorrectHudPass(value != 0.0f); return; }
    if (std::strcmp(name, "viewmodel.correct_world_pass") == 0) { SetCorrectWorldOriginPass(value != 0.0f); return; }
    if (std::strcmp(name, "rui.hud_anchor") == 0) { SetHudAnchorMode(static_cast<int>(value)); return; }
    if (std::strcmp(name, "headtracking.positional") == 0) { SetHeadPositionalTracking(value != 0.0f); return; }
    if (std::strcmp(name, "upload.hook_passthrough") == 0) { SetHookPassThrough(value != 0.0f); return; }
    if (std::strcmp(name, "upload.hook_implementation") == 0) { SetHookImplementationAllowed(value != 0.0f); return; }
    if (std::strcmp(name, "xr.force_zero_layers") == 0) { SetXrForceZeroLayers(value != 0.0f); return; }
    // Defaults ON, and deliberately: it reads and logs and touches nothing in
    // the engine, so there is no state for a player to be surprised by, and the
    // one thing that would make this step useless is finding out afterwards that
    // it was switched off -- which this project has now done three times.
    if (std::strcmp(name, "xr.controllers") == 0) { SetControllerInputEnabled(value != 0.0f); return; }
    if (std::strcmp(name, "input.force_controller_mode") == 0) { SetForceControllerMode(value != 0.0f); return; }
    if (std::strcmp(name, "input.a_sends_space") == 0) { SetASendsSpace(value != 0.0f); return; }
    if (std::strcmp(name, "input.synthetic_pad") == 0) { SetSyntheticPadEnabled(value != 0.0f); return; }
    if (std::strcmp(name, "input.keyboard_menu_nav") == 0) { SetKeyboardMenuNav(value != 0.0f); return; }
    if (std::strcmp(name, "input.kbuttons") == 0) { SetKbuttonRouteEnabled(value != 0.0f); return; }
    if (std::strcmp(name, "input.hold_keyboard_mode") == 0) { SetHoldKeyboardMode(value != 0.0f); return; }
    // Which grip throws the grenade: 0 = left, 1 = right. Default right.
    // menu.open -- the panel without the key, for a scripted flat run that has
    // no keyboard. Same state the chord toggles; not a second store.
    if (std::strcmp(name, "menu.distance_m") == 0) { SetMenuQuadDistanceMetres(value); return; }
    if (std::strcmp(name, "menu.width_m") == 0) { SetMenuQuadWidthMetres(value); return; }
    if (std::strcmp(name, "menu.ui_scale") == 0) { SetMenuUiScale(value); return; }
    if (std::strcmp(name, "menu.open") == 0) { SetMenuOpen(value != 0.0f); return; }
    // Right-stick cross-talk: turn on X vs jump/crouch on Y.
    if (std::strcmp(name, "input.haptic_strength") == 0) { SetHapticStrength(value); return; }
    if (std::strcmp(name, "input.turn_mode") == 0) { SetTurnMode(static_cast<int>(value)); return; }
    if (std::strcmp(name, "input.turn_curve") == 0) { SetTurnCurve(value); return; }
    if (std::strcmp(name, "input.turn_speed") == 0) { SetTurnSpeed(value); return; }
    if (std::strcmp(name, "input.snap_turn") == 0) { SetSnapTurnDegrees(value); return; }
    if (std::strcmp(name, "input.turn_deadzone") == 0) { SetTurnDeadzone(value); return; }
    if (std::strcmp(name, "input.jumpcrouch_deadzone") == 0) { SetJumpCrouchDeadzone(value); return; }
    if (std::strcmp(name, "input.stick_cross_ratio") == 0) { SetStickCrossRatio(value); return; }
    // Crouch: 0 = hold, 1 = the engine's own +toggle_duck (default), 2 = ours.
    if (std::strcmp(name, "input.crouch_mode") == 0) { SetCrouchMode(static_cast<int>(value)); return; }
    if (std::strcmp(name, "autoarm.require_live_view") == 0) { SetAutoArmRequireLiveView(value != 0.0f); return; }
    if (std::strcmp(name, "viewmodel.correction") == 0) { SetViewmodelCorrectionWanted(value != 0.0f); return; }
    if (std::strcmp(name, "xr.headlock_quad") == 0) { SetHeadlockQuad(value != 0.0f); return; }
    if (std::strcmp(name, "xr.stale_ms") == 0) { SetPresentStaleThresholdMs(value); return; }
    if (std::strcmp(name, "xr.viewport_full") == 0) { SetViewportFull(value != 0.0f); return; }
    // A1, PLAN-ASPECT section 4. A one-shot diagnostic sweep of the buffer
    // aspect, which puts the original back and disarms itself. No registry row:
    // it is a measurement, not a setting, and it is FLAT ONLY.
    // The cost half of A1, measured BETWEEN LAUNCHES because live mode changes
    // render black after the first one. Changes nothing; times frames and prints.
    // The engine's own letterbox dial. Below the buffer aspect it never fires,
    // so the world pass fills the buffer with anisotropy still 1.000.
    if (std::strcmp(name, "xr.letterbox_floor") == 0) { SetLetterboxFloor(value); return; }
    if (std::strcmp(name, "game.fov_auto") == 0) { SetFovAuto(value != 0.0f); return; }
    if (std::strcmp(name, "render.min_aspect_floor") == 0) { SetMinAspectFloor(value != 0.0f); return; }
    if (std::strcmp(name, "headtracking.recentre_on_spawn") == 0) { SetRecentreOnSpawn(value != 0.0f); return; }
    // HEIGHT A2: the eye raised at the engine's own getter (player_eye_hook.h).
    // eye.hook installs the interceptor (pass-through); eye.raise is the value
    // F5 applies. No registry rows until the flat proof; ini-only, like the
    // other levers that move the eye.
    if (std::strcmp(name, "eye.hook") == 0) { SetPlayerEyeHookWanted(value != 0.0f); return; }
    if (std::strcmp(name, "eye.raise") == 0) { SetEyeHookRaiseUnits(value); return; }
    if (std::strcmp(name, "eye.lean_follow") == 0) { SetEyeHookLeanFollow(value != 0.0f); return; }
    // TITAN STATE (titan_state.h). 0 = read-only: the IsTitan reading is logged,
    // nothing acts. 1 (default) = the arms collapse follows it.
    if (std::strcmp(name, "titan.gate") == 0) { SetTitanGate(static_cast<int>(value)); return; }
    if (std::strcmp(name, "xr.declare_rendered_fov") == 0) { SetDeclareRenderedFov(value != 0.0f); return; }
    if (std::strcmp(name, "xr.world_rect_gate") == 0) { SetWorldRectGate(value != 0.0f); return; }
    if (std::strcmp(name, "hud2d.head_anchor") == 0) { SetHud2dHeadAnchor(value != 0.0f); return; }
    if (std::strcmp(name, "xr.fit_horizontal") == 0) { SetFitHorizontal(value != 0.0f); return; }
    if (std::strcmp(name, "xr.lens_shear") == 0) { SetLensShearEnabled(value != 0.0f); return; }
    if (std::strcmp(name, "game.fov_scale") == 0) { SetGameFovScale(value); return; }
    if (std::strcmp(name, "xr.decouple") == 0) { SetXrDecoupled(value != 0.0f); return; }
    if (std::strcmp(name, "xr.mt_protect") == 0) { SetMultithreadProtectionWanted(value != 0.0f); return; }
    if (std::strcmp(name, "upload.no_substitute") == 0) { SetUploadNoSubstitute(value != 0.0f); return; }
    if (std::strcmp(name, "upload.identity_copy") == 0) { SetUploadIdentityCopy(value != 0.0f); return; }
    if (std::strcmp(name, "upload.body_level") == 0) { SetHookBodyLevel(static_cast<int>(value)); return; }
    if (std::strcmp(name, "upload.body_sweep") == 0) { SetHookBodyLevelSweep(value != 0.0f); return; }
    if (std::strcmp(name, "weapon.settings_hook") == 0) { SetWeaponSettingsHookAllowed(value != 0.0f); return; }
    // Investigation only: records the script API as it is registered and writes
    // it to a file. It patches client.dll, so it is off unless asked for -- but
    // it CANNOT be hotkey-gated the way the rest are, because every registration
    // happens while the VM is built and there is no later moment to catch them.
    // The INI is where that trade is opted into.
    // Registers six console variables the plugin publishes a hand pose through.
    // Off by default: registering convars changes engine-global state, which
    // nobody should acquire just by launching the game.
    // 0 off, 1 synthetic spin, 2 synthetic fixed offset (flat-checkable), 3 the
    // right controller. The composition path is identical for 2 and 3, which is
    // what lets it be verified without a headset.
    if (std::strcmp(name, "hand.source") == 0) { SetHandPoseSource(static_cast<int>(value)); return; }
    // S5 verification: flips the real viewmodel off and on so the A/B and the
    // restore come from one mechanism. Off by default; it changes what the
    // player can see.
    if (std::strcmp(name, "viewmodel.hide_pulse") == 0) { SetViewmodelHidePulseEnabled(value != 0.0f); return; }
    // H1: hold it hidden for the whole session rather than pulsing.
    if (std::strcmp(name, "viewmodel.hidden") == 0) { SetViewmodelHiddenPersistent(value != 0.0f); return; }
    if (std::strcmp(name, "stereo.half_ipd_units") == 0) {
        SetHalfInterpupillaryUnits(value);
        // Naming it in the file means it wins over the runtime's reported IPD.
        MarkHalfInterpupillaryUnitsExplicit();
        return;
    }
    // THE REGISTRY SELF-CHECK'S INSTRUMENT, and the only line this if-chain
    // gains. The check needs to know that a name fell through to here without
    // parsing the log for it, so the fallthrough is counted as well as printed.
    ++g_unknownSettingCount;
    char line[192]{};
    std::snprintf(line, sizeof(line), "[TF2VR] config: unknown setting '%s' ignored.\n", name);
    Tf2VrLog(line);
}

void ParseConfigFile(const char* path) {
    FILE* file = nullptr;
    if (fopen_s(&file, path, "r") != 0 || !file) {
        // Say so. A silent miss here cost a whole headset run: an ini was placed
        // to disable a hook for an A/B, the file was never opened, the hook
        // stayed on, and the log gave no hint -- so the run looked like a
        // negative result for the hook rather than a test that never happened.
        char line[MAX_PATH + 96]{};
        std::snprintf(line, sizeof(line), "[TF2VR] no config file at %s; built-in defaults are in use.\n", path);
        Tf2VrLog(line);
        return;
    }
    char raw[512]{};
    unsigned lineNumber = 0;
    unsigned applied = 0;
    while (std::fgets(raw, sizeof(raw), file)) {
        ++lineNumber;
        TrimAscii(raw);
        if (!raw[0] || raw[0] == '#' || raw[0] == ';') continue;
        // A CONFIG LINE THAT DOES NOTHING MUST SAY SO.
        //
        // Every directive needs an '=', and a line without one was silently
        // dropped -- no error, not counted, indistinguishable from a line that
        // worked. That cost a run: `bind test.eyetranslation F12` was written
        // the way the ini's own existing example is written, thrown away, and
        // the run it was arming came back "no visible change" from a lever that
        // had never been bound.
        //
        // It also means the live ini's `bind aim.cmd LEADER+BACKSPACE` has
        // never done anything. It is a no-op only by luck: it names the chord
        // that action already defaults to.
        //
        // The applied-vs-present count was the tell and nothing printed it.
        // Now every skipped line names itself.
        char* equals = std::strchr(raw, '=');
        if (!equals) {
            char line[220]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] config line %u IGNORED (no '='): \"%s\". Directives are "
                "`set name = value` and `bind action = CHORD` -- the '=' is required for both.\n",
                lineNumber, raw);
            Tf2VrLog(line);
            continue;
        }
        *equals = '\0';
        char* left = raw;
        char* right = equals + 1;
        TrimAscii(left);
        TrimAscii(right);
        // "leader = PAUSE" takes no subject, so it is handled before the
        // directive/subject split below.
        {
            char directive[16]{};
            std::snprintf(directive, sizeof(directive), "%s", left);
            UpperAscii(directive);
            if (std::strcmp(directive, "LEADER") == 0) {
                KeyChord chord{};
                if (!ParseChord(right, chord) || !chord.vk || chord.leader) {
                    char line[192]{};
                    std::snprintf(line, sizeof(line),
                        "[TF2VR] config line %u: unusable leader key; keeping the previous one.\n", lineNumber);
                    Tf2VrLog(line);
                } else {
                    g_leaderKey = chord;
                    ++applied;
                }
                continue;
            }
        }
        char* space = std::strpbrk(left, " \t");
        if (!space) continue;
        *space = '\0';
        char* subject = space + 1;
        TrimAscii(subject);
        if (std::strcmp(left, "bind") == 0) {
            const int index = ActionIndexByName(subject);
            KeyChord chord{};
            if (index < 0) {
                char line[192]{};
                std::snprintf(line, sizeof(line), "[TF2VR] config line %u: unknown action '%s'.\n", lineNumber, subject);
                Tf2VrLog(line);
            } else if (!ParseChord(right, chord)) {
                char line[192]{};
                std::snprintf(line, sizeof(line), "[TF2VR] config line %u: unparsable key '%s'; default kept.\n", lineNumber, right);
                Tf2VrLog(line);
            } else {
                g_bindings[static_cast<size_t>(index)] = chord;
                ++applied;
            }
        } else if (std::strcmp(left, "set") == 0) {
            ApplyValue(subject, static_cast<float>(std::atof(right)));
            ++applied;
        }
    }
    // THE CONTENT HASH, NOT JUST THE PATH AND THE COUNT, and it is computed
    // BEFORE the close -- rewinding a closed stream reads nothing and would have
    // printed a constant, which is worse than no hash at all.
    //
    // The standing rule is that the agent sets the live config and verifies FROM
    // THE RUN'S OWN LOG which config actually ran, because "a run of an
    // unchanged config looks exactly like a successful fix" -- and that has
    // already cost one headset run to a copy that did not land. The path proves
    // which file was opened and the count proves it was not empty; neither
    // proves it was the file that was meant to be there. FNV-1a over the bytes
    // does, and it costs one pass over a few kilobytes at load.
    std::uint32_t hash = 2166136261u;
    std::rewind(file);
    for (int byte = std::fgetc(file); byte != EOF; byte = std::fgetc(file)) {
        hash = (hash ^ static_cast<std::uint32_t>(byte)) * 16777619u;
    }
    std::fclose(file);
    char line[MAX_PATH + 96]{};
    std::snprintf(line, sizeof(line), "[TF2VR] config: %u directives applied from %s (content %08X).\n",
                  applied, path, hash);
    Tf2VrLog(line);
}

}  // namespace

void LoadPluginConfig(HMODULE self) {
    ApplyDefaults();
    g_configPath[0] = '\0';
    // ALWAYS resolve our own module from a function address. The handle
    // Northstar passes is not this DLL -- it resolved to the game root, so an
    // ini placed beside the plugin was looked for at
    // "Titanfall2\titanfall2vr.ini" and never found. Two test runs were spent
    // on experiments that silently never happened because of it, the first
    // giving no clue at all and the second only naming the path because the
    // miss had been made to log. Trusting a handle nobody had checked was the
    // whole of the bug; the fallback was there but gated on it being null.
    (void)self;
    HMODULE selfModule = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&LoadPluginConfig), &selfModule);
    self = selfModule;
    if (self && GetModuleFileNameA(self, g_configPath, MAX_PATH)) {
        char* leaf = std::strrchr(g_configPath, '\\');
        if (leaf) {
            const size_t used = static_cast<size_t>(leaf - g_configPath) + 1;
            std::snprintf(g_configPath + used, MAX_PATH - used, "titanfall2vr.ini");
        }
    }
    // M2's SELF-CHECK, AND IT RUNS BEFORE THE INI IS PARSED. That ordering is
    // defence in depth, and it was added because the first version of this check
    // was itself dangerous.
    //
    // The check feeds every registry name through ApplyValue at that setting's
    // CURRENT value. If a getter is not a faithful mirror of its setter, that
    // "no-op" is a WRITE of the wrong value -- and two entries were exactly that
    // when it was written: vrinput.enabled read g_enabled (which is 0 for a
    // correctly-armed session still waiting for client.dll) and would have
    // cleared g_wanted, disarming the controllers for the whole session; and
    // the former weapon.sway_auto read IsWeaponSwaySuppressed(), a different question,
    // which would have switched the auto-suppression policy off.
    //
    // Both getters were fixed rather than the check being weakened -- catching
    // that pair is the round-trip test doing its job. But running here, on
    // defaults, means the worst a future mismatch can do is write a default over
    // a default, with the ini applied afterwards on top. It can never disturb a
    // value the wearer set.
    RunSettingsRegistrySelfCheck();
    if (g_configPath[0]) ParseConfigFile(g_configPath);
}

void ReloadPluginConfig() {
    ApplyDefaults();
    if (g_configPath[0]) ParseConfigFile(g_configPath);
    Tf2VrLog("[TF2VR] config reloaded; every action is back to its file/default binding and nothing is armed.\n");
    LogActiveBindings();
}

const KeyChord& BindingFor(Action action) {
    static const KeyChord unbound{};
    const auto index = static_cast<size_t>(action);
    return index < static_cast<size_t>(Action::Count) ? g_bindings[index] : unbound;
}

const char* ActionName(Action action) {
    for (const auto& entry : kActions) {
        if (entry.action == action) return entry.name;
    }
    return "?";
}

const char* DescribeChord(const KeyChord& chord, char* buffer, size_t size) {
    if (!buffer || !size) return "";
    if (!chord.vk) { std::snprintf(buffer, size, "none"); return buffer; }
    char key[16]{};
    if (chord.vk >= VK_F1 && chord.vk <= VK_F24) std::snprintf(key, sizeof(key), "F%u", chord.vk - VK_F1 + 1);
    else if (chord.vk >= VK_NUMPAD0 && chord.vk <= VK_NUMPAD9) std::snprintf(key, sizeof(key), "NUMPAD%u", chord.vk - VK_NUMPAD0);
    else if ((chord.vk >= 'A' && chord.vk <= 'Z') || (chord.vk >= '0' && chord.vk <= '9')) std::snprintf(key, sizeof(key), "%c", static_cast<char>(chord.vk));
    else {
        std::snprintf(key, sizeof(key), "0x%02X", chord.vk);
        for (const auto& entry : kNamedKeys) {
            if (entry.vk == chord.vk) { std::snprintf(key, sizeof(key), "%s", entry.name); break; }
        }
    }
    std::snprintf(buffer, size, "%s%s%s%s%s", chord.leader ? "LEADER then " : "",
                  (chord.mods & kModCtrl) ? "CTRL+" : "",
                  (chord.mods & kModAlt) ? "ALT+" : "", (chord.mods & kModShift) ? "SHIFT+" : "", key);
    return buffer;
}

// ---------------------------------------------------------------------------
// THE BINDING COLLISION CHECK. It exists because of a specific fault, and the
// fault is worth stating rather than generalising away.
//
// `stereo.reentry.fire` was bound to bare F4 for one run. Bare F4 already
// belonged to `arms.collapse` -- the one mechanism this task's do-not-touch
// list names by name, because it corrupts the heap. One press fired BOTH: it
// armed a read-only experiment and it armed a known crash, and the session died
// 1.2 seconds later with the exact recorded signature. Nothing said a word.
//
// The binding table already prints itself at startup, and the collision was
// sitting in that printout as two lines reading F4. A list a human has to
// diff against itself is not a check. This one is: it compares every pair and
// says so.
//
// It runs at startup beside the registry and hook-registry self-checks, and it
// prints a line either way, because a check that is silent when it passes
// cannot be told from one that never ran.
void CheckBindingCollisions() {
    int collisions = 0;
    for (int i = 0; i < static_cast<int>(Action::Count); ++i) {
        const KeyChord& a = g_bindings[i];
        if (!a.vk) continue;
        for (int j = i + 1; j < static_cast<int>(Action::Count); ++j) {
            const KeyChord& b = g_bindings[j];
            if (!b.vk) continue;
            if (a.vk != b.vk || a.mods != b.mods || a.leader != b.leader) continue;
            ++collisions;
            char chordText[48]{};
            char line[320]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] BINDING COLLISION: '%s' and '%s' are BOTH on %s. One press fires both. "
                "This is how a diagnostic key re-armed arms.collapse and crashed a session.\n",
                ActionName(static_cast<Action>(i)), ActionName(static_cast<Action>(j)),
                DescribeChord(a, chordText, sizeof(chordText)));
            Tf2VrLog(line);
        }
    }
    char summary[200]{};
    std::snprintf(summary, sizeof(summary),
        "[TF2VR] binding collision check: %d collision%s across %d actions.%s\n",
        collisions, collisions == 1 ? "" : "s", static_cast<int>(Action::Count),
        collisions ? "" : " None -- every bound chord is owned by exactly one action.");
    Tf2VrLog(summary);
}

void LogActiveBindings() {
    CheckBindingCollisions();
    char leaderText[48]{};
    char header[320]{};
    std::snprintf(header, sizeof(header),
        "[TF2VR] active bindings. LEADER is %s: tap it, then press the key below within %u ms. "
        "Every key used is one the game ignores, because it sees our presses too. "
        "Edit titanfall2vr.ini next to the DLL to change any of this.\n",
        DescribeChord(g_leaderKey, leaderText, sizeof(leaderText)), g_leaderTimeoutMs);
    Tf2VrLog(header);
    // BOUND ONES ONLY. Printing 140 lines of which 100 said "unbound" is how a
    // table nobody reads accumulates twenty-seven chords from closed rungs. The
    // unbound ones are counted instead, and named as reachable from the ini so
    // "retired" cannot be misread as "deleted".
    int bound = 0, unbound = 0;
    for (const auto& entry : kActions) {
        const KeyChord& chord = g_bindings[static_cast<size_t>(entry.action)];
        if (!chord.vk) { ++unbound; continue; }
        ++bound;
        char chordText[48]{};
        char line[160]{};
        std::snprintf(line, sizeof(line), "[TF2VR]   %-24s %s\n", entry.name,
                      DescribeChord(chord, chordText, sizeof(chordText)));
        Tf2VrLog(line);
    }
    char tail[420]{};
    std::snprintf(tail, sizeof(tail),
        "[TF2VR]   (%d bound, %d unbound. The unbound ones are retired diagnostics from closed "
        "rungs, not missing features: each still exists and comes back with one "
        "`bind <name> <chord>` line in the ini, no rebuild. Experiments live in the four fixed "
        "slots F3/F5/F6/F12 and nowhere else.)\n", bound, unbound);
    Tf2VrLog(tail);
}

const KeyChord& LeaderKey() { return g_leaderKey; }
unsigned LeaderTimeoutMs() { return g_leaderTimeoutMs; }
bool HotkeysRequireForeground() { return g_requireForeground; }

void ReadPinOffsets(float* forward, float* right, float* up, float* spinDegreesPerSecond) {
    if (forward) *forward = g_pinForward.load(std::memory_order_acquire);
    if (right) *right = g_pinRight.load(std::memory_order_acquire);
    if (up) *up = g_pinUp.load(std::memory_order_acquire);
    if (spinDegreesPerSecond) *spinDegreesPerSecond = g_pinSpin.load(std::memory_order_acquire);
}

// WHERE THE GUN POINTS, as opposed to where it sits.
//
// hand.off_yaw was wired from the start and reachable only by editing the ini
// and restarting, which means dialling it blind: the wearer cannot see the file
// and cannot judge a degree of yaw from a number. Reported as "the gun points
// too far right and I can't adjust it" -- the control existed, the way to USE
// it did not.
//
// Positive yaw is LEFT in Source's convention (+X forward, +Y left), so the key
// pointing left makes the gun point left.
void NudgeHandAngleOffsets(float pitch, float yaw, float roll) {
    g_handOffPitch.store(g_handOffPitch.load(std::memory_order_acquire) + pitch,
                         std::memory_order_release);
    g_handOffYaw.store(g_handOffYaw.load(std::memory_order_acquire) + yaw,
                       std::memory_order_release);
    g_handOffRoll.store(g_handOffRoll.load(std::memory_order_acquire) + roll,
                        std::memory_order_release);
}

void ReadHandCalibration(float* offPitch, float* offYaw, float* offRoll, float* yawComp) {
    if (offPitch) *offPitch = g_handOffPitch.load(std::memory_order_acquire);
    if (offYaw) *offYaw = g_handOffYaw.load(std::memory_order_acquire);
    if (offRoll) *offRoll = g_handOffRoll.load(std::memory_order_acquire);
    if (yawComp) *yawComp = g_handYawComp.load(std::memory_order_acquire);
}

void ReadHandPositionOffset(float* forward, float* left, float* up) {
    if (forward) *forward = g_handOffForward.load(std::memory_order_acquire);
    if (left) *left = g_handOffLeft.load(std::memory_order_acquire);
    if (up) *up = g_handOffUp.load(std::memory_order_acquire);
}

void ReadGripOffset(float* forward, float* right, float* up) {
    if (forward) *forward = g_gripForward.load(std::memory_order_acquire);
    if (right) *right = g_gripRight.load(std::memory_order_acquire);
    if (up) *up = g_gripUp.load(std::memory_order_acquire);
}

void NudgeGripOffset(float forward, float right, float up) {
    g_gripForward.store(g_gripForward.load(std::memory_order_acquire) + forward, std::memory_order_release);
    g_gripRight.store(g_gripRight.load(std::memory_order_acquire) + right, std::memory_order_release);
    g_gripUp.store(g_gripUp.load(std::memory_order_acquire) + up, std::memory_order_release);
}

void ReadHandAxisGates(float* usePitch, float* useRoll) {
    if (usePitch) *usePitch = g_usePitch.load(std::memory_order_acquire);
    if (useRoll) *useRoll = g_useRoll.load(std::memory_order_acquire);
}

void NudgeHandPositionOffset(float forward, float left, float up) {
    g_handOffForward.store(g_handOffForward.load(std::memory_order_acquire) + forward,
                           std::memory_order_release);
    g_handOffLeft.store(g_handOffLeft.load(std::memory_order_acquire) + left,
                        std::memory_order_release);
    g_handOffUp.store(g_handOffUp.load(std::memory_order_acquire) + up,
                      std::memory_order_release);
}

namespace {
// COARSE by default. The wearer cannot read a log to find out which mode they
// are in, so the one that is unmistakable on the first press is the one to
// start in. F12 drops to fine for the final trim.
//
// NOW THREE TARGETS, NOT TWO, and the arrow cluster is still the only way in.
//
// The keyboard is out of chords -- every leader-prefixed key this keyboard
// physically has is bound, the numpad does not exist, and F13-F24 parse but no
// key sends them. Adding a HUD control therefore could not add keys. It does
// not need to: the arrow cluster is already the thing the wearer can find by
// feel with a headset on, and F12 already exists to say what it drives. A
// third target costs one more press of a key that is already in the hand.
std::atomic<int> g_calibrationTarget = static_cast<int>(CalibrationTarget::Off);
}
void SetCalibrationTarget(CalibrationTarget target) {
    g_calibrationTarget.store(static_cast<int>(target), std::memory_order_release);
}
CalibrationTarget CurrentCalibrationTarget() {
    return static_cast<CalibrationTarget>(g_calibrationTarget.load(std::memory_order_acquire));
}
void SetPivotCalibrationMode(bool pivotMode) {
    SetCalibrationTarget(pivotMode ? CalibrationTarget::GunPosition : CalibrationTarget::Pivot);
}
bool PivotCalibrationMode() {
    return CurrentCalibrationTarget() == CalibrationTarget::GunPosition;
}

// ===========================================================================
// THE SETTINGS REGISTRY. PLAN-VRMENU M2.
//
// It lives HERE, at the end of config.cpp, and that is a decision rather than
// an accident. Two reasons, and both are about there being exactly one store
// per setting:
//
//   1. ApplyValue is internal to this translation unit, and SetSettingValue
//      must go through it. The plan's whole safety argument is that a menu edit
//      and an INI line take LITERALLY the same path, so the registry cannot be
//      allowed to reach the mechanisms any other way.
//   2. Most of these settings are stored in atomics in this file's anonymous
//      namespace. Getters written here read the EXACT atomic the setter writes.
//      A registry in its own file would need its own copies or its own accessor
//      layer, and either is a second store that can drift from the if-chain.
//
// It is ADDITIVE. Not one line of ApplyValue's if-chain was moved, reordered or
// wrapped. The only change to it is a counter increment on the unknown-name
// fallthrough, which is the self-check's instrument.
//
// WHAT IS DELIBERATELY ABSENT. Every entry below names a key ApplyValue really
// handles AND a getter that really reads that key's store. Settings with no
// reachable getter are OMITTED, not stubbed -- the plan is explicit that a row
// with nothing behind it is a second store by another name. The omissions are
// listed at the bottom of this block so the next pass knows what it inherits.
// ===========================================================================

namespace {

const char* const kComboCrouchMode[] = {"Hold", "Toggle (engine)", "Toggle (ours)", nullptr};
const char* const kComboTurnMode[] = {"Smooth", "Snap", nullptr};
const char* const kComboBodyShow[] = {"Weapon only", "Weapon + hands", "Weapon + full body", nullptr};

// Every getter reads the same store its setter writes. None caches.
float GetGripFwd()        { return g_gripForward.load(std::memory_order_acquire); }
float GetGripRight()      { return g_gripRight.load(std::memory_order_acquire); }
float GetGripUp()         { return g_gripUp.load(std::memory_order_acquire); }
float GetOffFwd()         { return g_handOffForward.load(std::memory_order_acquire); }
float GetOffLeft()        { return g_handOffLeft.load(std::memory_order_acquire); }
float GetOffUp()          { return g_handOffUp.load(std::memory_order_acquire); }
float GetOffPitch()       { return g_handOffPitch.load(std::memory_order_acquire); }
float GetOffYaw()         { return g_handOffYaw.load(std::memory_order_acquire); }
float GetOffRoll()        { return g_handOffRoll.load(std::memory_order_acquire); }
float GetYawComp()        { return g_handYawComp.load(std::memory_order_acquire); }
float GetUseRoll()        { return g_useRoll.load(std::memory_order_acquire); }
float GetUsePitch()       { return g_usePitch.load(std::memory_order_acquire); }
float GetPinFwd()         { return g_pinForward.load(std::memory_order_acquire); }
float GetPinRight()       { return g_pinRight.load(std::memory_order_acquire); }
float GetPinUp()          { return g_pinUp.load(std::memory_order_acquire); }
float GetPinSpin()        { return g_pinSpin.load(std::memory_order_acquire); }
float GetEyeRaise()       { return EyeHookRaiseConfigured(); }
float GetNeckForward()    { return g_neckForward; }
float GetNeckUp()         { return g_neckUp; }
float GetRenderWidth()    { return static_cast<float>(g_renderWidth); }
float GetRenderHeight()   { return static_cast<float>(g_renderHeight); }
// THE SLIDER SHOWS AN EFFECTIVE VALUE, never a bare 0.
//
// render.scale stores 0 for "unset", which is right for the ini -- it is how the
// absolute render.width/height keep precedence. But a slider parked at 0 tells
// the wearer nothing and reads as broken, which is exactly how it was reported.
//
// So when unset, this reports the scale the CURRENT resolution already amounts
// to, in the same panel terms the page above uses: delivered width over what the
// headset asked for. The slider then starts where the wearer actually is, and
// moving it means something relative to that.
float GetRenderScale() {
    const float stored = RenderScale();
    if (stored > 0.0f) return stored;
    return EffectiveRenderScale();
}
float GetLeaderTimeout()  { return static_cast<float>(g_leaderTimeoutMs); }
float GetRequireForeground() { return g_requireForeground ? 1.0f : 0.0f; }

// ---- THE HUD ROWS. M3's first task, discharged.
//
// Each of these was on the OMITTED list below for one reason: it had no getter
// reading the store its setter writes. Every one of them DID already have a
// reader published by its owning module -- RuiLowerLeftScale, ReadHud2dPlacement,
// ReadHud2dPlacement, RuiInset, RuiLowerLeftScale -- so these are the thin
// shims the omission note describes, not a second store.
//
// ALL FIVE READERS ARE LOCK-FREE, which is the hard rule for a registry getter
// and was checked before writing this: the camera_update_hook trio are atomic
// loads, and the two rui_probe accessors are plain float reads. Nothing here can
// re-lock a mutex the Present path already owns, which is what crashed the panel
// three times.
//
// The pair getters read all components and return one. That is a redundant load
// or two per panel build, which is free, and it means the getter cannot drift
// from the setter's grouping the way two separate stores would.
float GetHud2dShiftX()    { float x = 0.0f, y = 0.0f, z = 1.0f; ReadHud2dPlacement(&x, &y, &z); return x; }
float GetHud2dShiftY()    { float x = 0.0f, y = 0.0f, z = 1.0f; ReadHud2dPlacement(&x, &y, &z); return y; }
float GetHudLlShiftXV()   { return HudLowerLeftShiftX(); }
float GetHudLlShiftXTitanV() { return HudLowerLeftShiftXTitan(); }
float Get_hud_prompt_scale() { return HudPromptScale(); }
float Get_hud_prompt_shift_y() { return HudPromptShiftY(); }
float Get_hud_instruction_scale() { return HudInstructionScale(); }
float Get_hud_instruction_shift_y() { return HudInstructionShiftY(); }
float Get_hud_timer_scale() { return HudTimerScale(); }
float Get_hud_timer_shift_x() { return HudTimerShiftX(); }
float Get_hud_timer_shift_y() { return HudTimerShiftY(); }
float Get_hud_highlight_scale() { return HudHighlightScale(); }
float GetHud2dZoom()      { float x = 0.0f, y = 0.0f, z = 1.0f; ReadHud2dPlacement(&x, &y, &z); return z; }
float GetMarkerSize()     { return RuiMarkerSize(); }
float GetRuiInsetV()      { return RuiInset(); }
float GetRuiLlScaleV()    { return RuiLowerLeftScale(); }
// THE FOUR THE WEARER TUNED ON 2026-09-06, read from the same stores the
// setters write, never a cached copy.
float GetHudWidgetScaleV() { return HudWidgetScale(); }
float GetLockHudViewFixV() { return LockHudViewFixOn() ? 1.0f : 0.0f; }
float GetLockHudRingScaleV() { return LockHudRingScale(); }
float GetHudCockpitScaleV() { return HudCockpitScale(); }
float GetHudBarsScaleV()   { return HudBarsScale(); }
float GetHudBarsShiftYV()  { return HudBarsShiftY(); }
float GetMarkerTextXV()   { return RuiMarkerTextX(); }
float GetNameplateScaleV() { return HudNameplateScale(); }

// These read their own module's store, through the accessor that module already
// publishes. Still one store: the module owns it and both sides go through it.
float GetCrosshairScale() { return CrosshairScale(); }
float GetReticleHidden()  { return IsReticleHidden() ? 1.0f : 0.0f; }
float GetAdsKeepReticle() { return IsAdsKeepReticle() ? 1.0f : 0.0f; }
float GetAdsLock()        { return IsAdsLock() ? 1.0f : 0.0f; }
float GetAdsFaceAim()     { return IsAdsFaceAim() ? 1.0f : 0.0f; }
float GetAdsFaceAimPitch(){ return IsAdsFaceAimPitch() ? 1.0f : 0.0f; }
float GetCrouchModeV()    { return static_cast<float>(CrouchMode()); }
float GetTurnDeadzoneV()  { return TurnDeadzone(); }
float GetTurnSpeedV()     { return TurnSpeed(); }
float GetSnapTurnV()      { return SnapTurnDegrees(); }
float GetTurnModeV()      { return static_cast<float>(TurnMode()); }
float GetBodyShowV()      { return static_cast<float>(BodyShowMode()); }
float GetHapticStrengthV() { return HapticStrength() * 100.0f; }
float GetTurnCurveV()     { return TurnCurve(); }
float GetJumpCrouchDz()   { return JumpCrouchDeadzone(); }
float GetCrossRatio()     { return StickCrossRatio(); }
float GetMenuOpenV()      { return IsMenuOpen() ? 1.0f : 0.0f; }
float GetVrInputEnabled() { return IsVrInputWanted() ? 1.0f : 0.0f; }
float GetXrDecoupledV()   { return IsXrDecoupled() ? 1.0f : 0.0f; }

constexpr SettingWidget kToggle = SettingWidget::Toggle;
constexpr SettingWidget kSlider = SettingWidget::Slider;
constexpr SettingWidget kCombo = SettingWidget::Combo;
constexpr SettingWidget kNumeric = SettingWidget::Numeric;
constexpr SettingTier kLive = SettingTier::Live;
constexpr SettingTier kRestart = SettingTier::Restart;
// The enum has carried Careful since M2; nothing had used it until the HUD
// anchor pair, which is exactly what the tier is for -- a control that can
// visibly break the HUD and should never be a bare drag.
constexpr SettingTier kCareful = SettingTier::Careful;

const Setting kSettings[] = {
    // ---- Home ----------------------------------------------------------
    // ONE resolution control, and only one.
    //
    // render.width and render.height are deliberately NOT rows here. They are two
    // independent numbers, and two independent numbers can be given a different
    // aspect ratio from the headset -- which does not look like a wrong setting,
    // it looks like a stretched world. They remain ini keys for anyone who wants
    // an absolute override, and they win over the scale when both are set.

    // ---- View ----------------------------------------------------------
    // HEIGHT A2 (2026-09-02). ONE quantity, applied at the engine's own
    // eye-position getter on both the client and the server, so the camera,
    // the gun and the round rise together and the crosshair stays on the
    // impacts at every value. The game puts the eye at 83.3% of the hull where
    // a human eye sits at 93.3%: about 7 units short, the same on every
    // headset. Getter is a bare atomic load, per settings_registry.h.
    {"eye.raise", "Eye height",
     "How far above the game's own eye you stand, in game units (about an inch "
     "each). The game itself sits you at neck level; 7 to 10 puts you at a "
     "character's eyes. Camera, gun and rounds all move together, so aim stays "
     "true at any value.",
     GetEyeRaise, 0.0f, 20.0f, 8.0f, kSlider, kLive, SettingCategory::View, nullptr},
    {"headtracking.neck_forward", "Neck length forward",
     "How far in front of the neck pivot your eyes sit. Larger values make "
     "leaning your head translate the view further.",
     GetNeckForward, 0.0f, 0.4f, 0.10f, kSlider, kLive, SettingCategory::View, nullptr},
    {"headtracking.neck_up", "Neck length up",
     "How far above the neck pivot your eyes sit.",
     GetNeckUp, 0.0f, 0.4f, 0.10f, kSlider, kLive, SettingCategory::View, nullptr},
    // PLAN-ADS C1. The getter is a bare atomic load, per the hard rule at the
    // bottom of settings_registry.h -- a getter that takes a lock crashed the
    // game on summon three times.
    {"ads.max_magnification", "ADS magnification cap",
     "How far aiming down sights is allowed to magnify. 0 lets the weapon zoom as "
     "much as the game intends, which is what a monitor shows you. Lower values hold "
     "it back, which widens what you can see but makes distant things smaller.",
     AdsMaxMagnification, 0.0f, 3.0f, 0.0f, kSlider, kLive, SettingCategory::View, nullptr},
    {"ads.optic_passthrough", "Let scopes zoom",
     "A weapon that zooms at least this much is left alone, because a scope should "
     "magnify -- that is what a scope is. Below it, the cap above applies. Each "
     "weapon tells the game its own zoom, so this is one setting rather than a list.",
     AdsOpticPassthrough, 1.0f, 8.0f, 3.0f, kSlider, kLive, SettingCategory::View, nullptr},

    // ---- Hands & Weapon: C4, the brace ----------------------------------
    // The gun's steadiness is a property of how the weapon is held, so these sit
    // with the grip rather than with the view.
    {"ads.lock", "Sights take over in ADS",
     "Aiming down sights hands the gun to the game, exactly as it works on a monitor: "
     "it centres, lines up with your view, and shoots where you look. Your hand stops "
     "steering it until you let go.",
     GetAdsLock, 0.0f, 1.0f, 1.0f, kToggle, kLive, SettingCategory::HandsWeapon, nullptr},
    {"ads.face_aim", "Turn to the gun in ADS",
     "Aiming down sights turns you to face where the gun was pointing, so what it "
     "was aimed at ends up in the middle of the screen. Off instead leaves the view "
     "alone and the gun shoots where you were looking. Left and right only.",
     GetAdsFaceAim, 0.0f, 1.0f, 1.0f, kToggle, kLive, SettingCategory::HandsWeapon, nullptr},
    {"ads.face_aim_pitch", "Tilt to the gun in ADS",
     "Aiming down sights also tilts your view up or down to the gun, so a scope "
     "pointed high ends up in front of you rather than off the top of the screen. "
     "While it is on, level in your headset is not level in the world.",
     GetAdsFaceAimPitch, 0.0f, 1.0f, 1.0f, kToggle, kLive, SettingCategory::HandsWeapon, nullptr},
    {"ads.turn_scale", "Turn speed in ADS",
     "How much of your normal turn speed you keep while aiming. Lower is steadier and "
     "slower, which is the cost the flat game charges for aiming.",
     AdsTurnScale, 0.05f, 1.0f, 0.20f, kSlider, kLive, SettingCategory::HandsWeapon, nullptr},
    {"ads.brace_gain", "Aim brace strength",
     "While you are aiming down sights the gun moves this fraction of what your "
     "hand moves, in the same frame -- smaller, never later. 1.0 turns the brace "
     "off. 0.25 means a 1 degree wobble becomes a quarter of a degree.",
     AdsBraceGain, 0.05f, 1.0f, 0.35f, kSlider, kLive, SettingCategory::HandsWeapon, nullptr},
    {"ads.brace_cone_deg", "Fine-tune cone",
     "How far you can move inside the brace before it starts letting go. Inside "
     "this angle the gun moves at the brace strength above.",
     AdsBraceConeDegrees, 0.5f, 20.0f, 4.0f, kSlider, kLive, SettingCategory::HandsWeapon,
     nullptr},
    {"ads.brace_readopt_deg", "Brace release angle",
     "Move further than this and the gun follows your hand normally again, so you "
     "can still turn while aiming. Must be larger than the fine-tune cone.",
     AdsBraceReadoptDegrees, 1.0f, 45.0f, 12.0f, kSlider, kLive, SettingCategory::HandsWeapon,
     nullptr},

    // ---- Hands & Weapon -------------------------------------------------
    {"hand.grip_fwd", "Grip forward",
     "Where the gun sits along your hand, front to back. This is the pivot as "
     "well as the position -- see the note in config.h.",
     GetGripFwd, -30.0f, 30.0f, -6.75f, kSlider, kLive, SettingCategory::HandsWeapon, nullptr},
    {"hand.grip_right", "Grip right",
     "Where the gun sits across your hand, left to right.",
     GetGripRight, -30.0f, 30.0f, -3.00f, kSlider, kLive, SettingCategory::HandsWeapon, nullptr},
    {"hand.grip_up", "Grip up",
     "Where the gun sits vertically in your hand.",
     GetGripUp, -30.0f, 30.0f, 4.75f, kSlider, kLive, SettingCategory::HandsWeapon, nullptr},
    {"hand.off_fwd", "Position forward",
     "Moves the whole weapon away from or towards you, in the player's frame "
     "rather than the gun's.",
     GetOffFwd, -30.0f, 30.0f, 1.29f, kSlider, kLive, SettingCategory::HandsWeapon, nullptr},
    {"hand.off_left", "Position left",
     "Moves the whole weapon sideways in the player's frame.",
     GetOffLeft, -30.0f, 30.0f, 0.20f, kSlider, kLive, SettingCategory::HandsWeapon, nullptr},
    {"hand.off_up", "Position up",
     "Moves the whole weapon vertically in the player's frame.",
     GetOffUp, -30.0f, 30.0f, 1.55f, kSlider, kLive, SettingCategory::HandsWeapon, nullptr},
    {"hand.off_pitch", "Angle pitch",
     "Tilts the weapon nose up or down relative to your hand.",
     GetOffPitch, -90.0f, 90.0f, 0.0f, kSlider, kLive, SettingCategory::HandsWeapon, nullptr},
    {"hand.off_yaw", "Angle yaw",
     "Swings the weapon left or right relative to your hand.",
     GetOffYaw, -90.0f, 90.0f, 6.50f, kSlider, kLive, SettingCategory::HandsWeapon, nullptr},
    {"hand.off_roll", "Angle roll",
     "Rolls the weapon about its own barrel relative to your hand.",
     GetOffRoll, -90.0f, 90.0f, 0.0f, kSlider, kLive, SettingCategory::HandsWeapon, nullptr},
    {"hand.use_pitch", "Use hand pitch",
     "Whether tilting your wrist up and down tilts the gun. Off pins the gun "
     "level regardless of your wrist.",
     GetUsePitch, 0.0f, 1.0f, 1.0f, kToggle, kLive, SettingCategory::HandsWeapon, nullptr},
    {"hand.use_roll", "Use hand roll",
     "Whether rolling your wrist rolls the gun. NOT free to toggle once a grip "
     "offset has been dialled in -- the offset is rotated by the gun's own "
     "orientation, so turning roll off moves the gun.",
     GetUseRoll, 0.0f, 1.0f, 1.0f, kToggle, kLive, SettingCategory::HandsWeapon, nullptr},
    {"hand.yawcomp", "Yaw compensation",
     "Confirmed 0 by measurement. Do not sweep this without a reason.",
     GetYawComp, -1.0f, 1.0f, 0.0f, kSlider, kLive, SettingCategory::HandsWeapon, nullptr},
    {"pin.fwd", "Pin forward",
     "Where the pinned viewmodel sits, front to back.",
     GetPinFwd, -200.0f, 200.0f, 25.0f, kSlider, kLive, SettingCategory::HandsWeapon, nullptr},
    {"pin.right", "Pin right",
     "Where the pinned viewmodel sits, left to right.",
     GetPinRight, -200.0f, 200.0f, 12.0f, kSlider, kLive, SettingCategory::HandsWeapon, nullptr},
    {"pin.up", "Pin up",
     "Where the pinned viewmodel sits vertically.",
     GetPinUp, -200.0f, 200.0f, -6.0f, kSlider, kLive, SettingCategory::HandsWeapon, nullptr},
    {"pin.spin", "Pin spin",
     "Degrees per second the pinned viewmodel rotates. A diagnostic, not a "
     "comfort setting.",
     GetPinSpin, -360.0f, 360.0f, 0.0f, kSlider, kLive, SettingCategory::HandsWeapon, nullptr},

    // ---- HUD ------------------------------------------------------------
    // The category existed and was EMPTY, which is why the tab showed nothing.
    // Ranges span what the ini already accepts, so a value typed there and a
    // value dragged here cannot disagree -- the registry's own rule.
    {"rui.ll_scale", "Ammo and ability corner size",
     "The lower-left group -- weapon, ammo, tactical, ordnance. Bigger number "
     "draws it smaller and pulls it further in from the corner, which is what a "
     "headset usually wants: on a monitor it can sit at the very edge, in VR "
     "that edge is outside comfortable vision.",
     GetRuiLlScaleV, 0.5f, 3.0f, 1.0f, kSlider, kLive, SettingCategory::Hud, nullptr},
    {"hud.ll_shift_x", "Ammo and ability corner, across",
     "Moves the lower-left weapon and ammo group sideways WITHOUT resizing it, "
     "as a fraction of screen width. Positive is right. The size control moves "
     "this group by scaling it about the screen centre, so it cannot bring the "
     "group in from the edge without also making it smaller; this separates the "
     "two. 0 leaves it where the size control puts it.",
     GetHudLlShiftXV, -0.40f, 0.40f, 0.060f, kSlider, kLive, SettingCategory::Hud, nullptr},
    {"hud.ll_shift_x_titan", "Ammo corner across, in a Titan",
     "The same sideways move for the Titan's larger loadout bar, which is a "
     "different readout with more in it and usually wants a different amount. "
     "Applies only while you are in a Titan; on foot the setting above applies.",
     GetHudLlShiftXTitanV, -0.40f, 0.40f, 0.120f, kSlider, kLive, SettingCategory::Hud, nullptr},
    {"hud.prompt_scale", "Dialogue response prompt, size",
     "The up/down prompt that appears when you are asked a question. It rides "
     "the same size control as the rest of the HUD, which leaves it too small "
     "to read in a headset; this brings it back on its own. 1.82 is the size "
     "the game draws. Above that it moves DOWN as it grows, because it sits "
     "near the bottom and scales about the screen centre -- use the control "
     "below to lift it back.",
     Get_hud_prompt_scale, 0.05f, 3.00f, 1.8200f, kSlider, kLive,
     SettingCategory::Hud, nullptr},
    {"hud.prompt_shift_y", "Dialogue response prompt, up and down",
     "Moves that prompt vertically, as a fraction of screen height. Negative "
     "is up. Needed only if you raise the size above 1.82, which pushes it "
     "toward the bottom edge.",
     Get_hud_prompt_shift_y, -1.00f, 1.00f, 0.0000f, kSlider, kLive,
     SettingCategory::Hud, nullptr},
    {"hud.instruction_scale", "Gauntlet instruction panel, size",
     "The big instruction panel the training gauntlet puts on screen. The game authors it at about 85% of the screen wide, which is overwhelming in a headset. Size only: changing it does not move the panel.",
     Get_hud_instruction_scale, 0.05f, 3.00f, 0.5800f, kSlider, kLive, SettingCategory::Hud, nullptr},
    {"hud.instruction_shift_y", "Gauntlet instruction panel, up and down",
     "Moves that panel vertically, as a fraction of screen height. Positive is down.",
     Get_hud_instruction_shift_y, -1.00f, 1.00f, 0.1590f, kSlider, kLive, SettingCategory::Hud, nullptr},
    {"hud.timer_scale", "Gauntlet timer, size",
     "The gauntlet's run timer. Size only: changing it does not move the timer.",
     Get_hud_timer_scale, 0.05f, 3.00f, 0.4500f, kSlider, kLive, SettingCategory::Hud, nullptr},
    {"hud.timer_shift_x", "Gauntlet timer, across",
     "Moves the timer sideways, as a fraction of screen width. Negative is left, in from the edge.",
     Get_hud_timer_shift_x, -1.00f, 1.00f, -0.1000f, kSlider, kLive, SettingCategory::Hud, nullptr},
    {"hud.timer_shift_y", "Gauntlet timer, up and down",
     "Moves the timer vertically. Negative is up.",
     Get_hud_timer_shift_y, -1.00f, 1.00f, -0.0300f, kSlider, kLive, SettingCategory::Hud, nullptr},
    {"hud.highlight_scale", "Weapon pickup highlight, size",
     "The prompt over a weapon you can pick up or swap. It rides the same size control as the rest of the HUD, which makes it too small to read in a headset; this brings it back on its own. 1.82 is the size the game draws.",
     Get_hud_highlight_scale, 0.05f, 3.00f, 2.1000f, kSlider, kLive, SettingCategory::Hud, nullptr},
    {"rui.inset", "Top-left cluster inset",
     "Pulls the top-left cluster in from the screen edges, using the engine's "
     "own safe-area branch. A per-edge fraction: 0.10 takes a tenth off each "
     "side. Headset-verified to a 48% shrink.",
     GetRuiInsetV, 0.0f, 0.49f, 0.0f, kSlider, kLive, SettingCategory::Hud, nullptr},
    {"hud2d.shift_y", "Reticle and 2D HUD, up and down",
     "Moves the flat HUD pass -- the reticle included -- in render pixels. "
     "Positive and negative both work; if the aim mark and where your rounds "
     "land disagree vertically, this is the control that moves the mark.",
     GetHud2dShiftY, -1000.0f, 1000.0f, 0.0f, kSlider, kLive, SettingCategory::Hud, nullptr},
    {"hud2d.shift_x", "Reticle and 2D HUD, across",
     "The horizontal half of the shift above, in render pixels.",
     GetHud2dShiftX, -1000.0f, 1000.0f, 0.0f, kSlider, kLive, SettingCategory::Hud, nullptr},
    {"hud2d.zoom", "Reticle and 2D HUD, size",
     "Scales that same flat pass about its centre. 1.00 leaves it alone.",
     GetHud2dZoom, 0.25f, 3.0f, 0.28f, kSlider, kLive, SettingCategory::Hud, nullptr},

    // ---- THE FOUR THE WEARER TUNED, 2026-09-06 --------------------------
    //
    // Grouped by WHO SEES THEM, because that is the split the wearer named:
    // the first is on screen as a pilot AND in a Titan, the other three exist
    // only inside a Titan. Every range spans exactly what its setter clamps
    // to, so a number typed in the ini and a number dragged here cannot
    // disagree. The defaults are the values the wearer settled on by stepping
    // ladders in the headset, not guesses.
    {"hud.widget_scale", "Weapon and ammo group, size",
     "The lower-left weapon and ammo group, and the other flat HUD segments "
     "that sit at the screen edges. Smaller numbers pull them in from the "
     "corners and make them readable in a headset, where the edge of the "
     "screen is outside comfortable vision. This one is on screen both on "
     "foot and in a Titan. 0.55 is the settled value.",
     GetHudWidgetScaleV, 0.30f, 1.50f, 0.55f, kSlider, kLive, SettingCategory::Hud, nullptr},
    {"hud.nameplate_scale", "Name labels, size",
     "The floating name labels over friends and enemies, and the health bar "
     "under an enemy one. Size only: a label stays on the player it names at "
     "every setting, and the bar shrinks with its label as one piece. These sit "
     "on their own draw layer, which nothing was reaching before, so they "
     "stayed at full size while the rest of the HUD came in. On foot and in a "
     "Titan both. 1.00 is the game's own size.",
     GetNameplateScaleV, 0.20f, 2.00f, 0.50f, kSlider, kLive, SettingCategory::Hud, nullptr},
    {"hud.cockpit_scale", "Titan cockpit HUD, size",
     "TITAN ONLY. The cockpit surround -- the faint grey animating border -- "
     "and the health bar across the top, which are one widget and move "
     "together. Smaller numbers draw them smaller and pull them in toward the "
     "middle. 0 leaves the Titan cockpit HUD exactly as the game draws it. "
     "0.60 is the settled value.",
     GetHudCockpitScaleV, 0.0f, 1.50f, 0.60f, kSlider, kLive, SettingCategory::Hud, nullptr},
    {"hud.bars_scale", "Titan dash bars, size",
     "TITAN ONLY. The two-bar indicator along the bottom of the cockpit HUD. "
     "It has its own size because the cockpit control above made it smaller "
     "when the wearer wanted it larger. 1.00 is the game's own size.",
     GetHudBarsScaleV, 0.25f, 3.00f, 1.15f, kSlider, kLive, SettingCategory::Hud, nullptr},
    {"hud.bars_shift_y", "Titan dash bars, height",
     "TITAN ONLY. Slides those same two bars up and down. NEGATIVE MOVES THEM "
     "UP -- this widget is painted onto cockpit geometry whose vertical axis "
     "runs opposite to the rest of the HUD, which is measured, not assumed. "
     "0 is the game's own height and -0.090 is the settled value.",
     GetHudBarsShiftYV, -0.50f, 0.50f, -0.090f, kSlider, kLive, SettingCategory::Hud, nullptr},

    // ---- MISSILE-LOCK RINGS, 2026-09-07 --------------------------------
    //
    // TITAN ONLY. The multi-target missile rings are VGUI panels the game
    // already places correctly; our 2D HUD zoom was compressing them onto the
    // square. The fix undoes that zoom for the ring panels alone, every input
    // read live (docs/HUD-RESULT-LOCKRINGS-FIX-2026-09-07.md).
    {"lockhud.viewfix", "Missile lock rings, stay on targets",
     "TITAN ONLY. Multi-target missiles: hold the trigger, sweep the square "
     "over enemies, each painted one gets a ring. On, the rings stay on their "
     "targets while the square follows your hand. Off, they ride the square, "
     "which is how the game's own placement lands once our HUD zoom is applied.",
     GetLockHudViewFixV, 0.0f, 1.0f, 1.0f, kToggle, kLive, SettingCategory::Hud, nullptr},
    {"lockhud.ring_scale", "Missile lock rings, size",
     "Size of each lock ring. The ring is placed by its centre, so this never "
     "moves it. 1.00 is the game's own size; 0.60 is the settled value.",
     GetLockHudRingScaleV, 0.20f, 2.00f, 0.60f, kSlider, kLive, SettingCategory::Hud, nullptr},

    {"hud.marker_size", "Waypoint marker, size",
     "How large the waypoint marker and its distance text are drawn. This is "
     "size only: the marker stays on its spot in the world at every setting, "
     "because where it sits is decided by the HUD pass's frustum and not by "
     "this. Smaller is a smaller marker.",
     GetMarkerSize, 0.20f, 0.80f, 0.45f, kSlider, kLive, SettingCategory::Hud, nullptr},
    {"hud.marker_text_x", "Waypoint distance text, across",
     "Where the waypoint's distance text and its leader line sit across the "
     "screen, as a fraction. The game puts that block at one of two places and "
     "the wrong one, 0.08, is jammed against the left edge where a headset "
     "cannot read it. This pins it back. The target icon itself never moves and "
     "no height changes -- the fault is purely sideways -- and the leader line "
     "is re-attached so its far end still lands on the target. 0 turns the fix "
     "off and leaves the text where the game puts it.",
     GetMarkerTextXV, 0.0f, 1.0f, 0.32f, kSlider, kLive, SettingCategory::Hud, nullptr},

    // ---- Reticle --------------------------------------------------------
    {"reticle.hidden", "Hide the reticle",
     "Removes the floating aim mark entirely. One int store to the crosshair "
     "state the game's own draw reads.",
     GetReticleHidden, 0.0f, 1.0f, 0.0f, kToggle, kLive, SettingCategory::Reticle, nullptr},
    {"ads.keep_reticle", "Keep the reticle in ADS",
     "Flat hides the aim mark when you aim down sights, because on a monitor the "
     "camera is locked to the sights for you. In VR nothing lines your eye up with "
     "them, so hiding it takes away the only reference you had. The mark sits where "
     "the round goes.",
     GetAdsKeepReticle, 0.0f, 1.0f, 1.0f, kToggle, kLive, SettingCategory::Reticle, nullptr},
    {"crosshair.scale", "Reticle element scale",
     "MEASURED CLOSED -- with scale 0.262 the falsifier counted 5718 scaled "
     "emits and the reticle was identical. The float is not the size. Kept "
     "because it is real and reversible, not because it works.",
     GetCrosshairScale, 0.05f, 20.0f, 1.0f, kSlider, kLive, SettingCategory::Reticle, nullptr},

    // ---- Controls -------------------------------------------------------
    // vrinput.enabled IS DELIBERATELY NOT A ROW HERE, and menu.open is not
    // either. Both remain ini keys; neither belongs in a panel.
    //
    // vrinput.enabled switches off the controller input this panel is driven
    // BY. A wearer who focused it and pressed A would lose the sticks, the
    // buttons and the summon chord in one press, with no way to undo it from
    // inside the headset. A control that can disable its own operator is a trap
    // however well it is labelled.
    //
    // menu.open closes the panel you are reading it on. It exists so a scripted
    // flat run can raise the panel without a keyboard, which is not something a
    // wearer ever needs to click.
    {"render.scale", "Resolution",
     "Sharpness, and the first thing to change if the frame rate is poor. Both "
     "axes move together, so cost falls with the SQUARE of this number and your "
     "field of view never changes at any setting. The default is deliberately "
     "below your headset's full panel height, because full height here is about "
     "twice a 4K frame and nothing checks your GPU first. Raise it if you have "
     "the headroom. Applies as soon as you close this panel -- but only the "
     "FIRST change of a session is safe, see KNOWN-ISSUES 1.",
     GetRenderScale, 0.5f, 2.0f, kDefaultRenderScale, kSlider, kLive,
     SettingCategory::Home, nullptr},
    {"input.crouch_mode", "Crouch",
     "Hold, the engine's own toggle, or ours. The engine's toggle is the game's "
     "#TOGGLE_CROUCH command, so slide and wallrun behaviour stays the game's.",
     GetCrouchModeV, 0.0f, 2.0f, 1.0f, kCombo, kLive, SettingCategory::Controls,
     kComboCrouchMode},
    {"input.turn_mode", "Turning",
     "Smooth sweeps the view; snap jumps it in fixed steps with nothing rendered "
     "in between, which is what makes snap turning help with motion sickness.",
     GetTurnModeV, 0.0f, 1.0f, 0.0f, kCombo, kLive, SettingCategory::Controls,
     kComboTurnMode},
    {"body.show", "Pilot body",
     "How much of the pilot is drawn. Weapon only hides the arms and body; "
     "weapon + hands keeps the gloves on the gun; full body draws everything. "
     "In a titan nothing is ever hidden.",
     GetBodyShowV, 0.0f, 2.0f, 1.0f, kCombo, kLive, SettingCategory::Body,
     kComboBodyShow},
    {"input.turn_speed", "Turn speed",
     "How fast the right stick sweeps the view. 0 is the slowest rate that is "
     "still usable, 70 is the game's own turn rate, and 100 is faster than the "
     "game turns on a pad. Your in-game look sensitivity still applies on top.",
     GetTurnSpeedV, 0.0f, 100.0f, 70.0f, kSlider, kLive, SettingCategory::Controls, nullptr},
    {"input.snap_turn", "Snap turn",
     "How far each flick turns you. The stick must return to centre between "
     "turns, so holding it gives one step rather than a spin.",
     GetSnapTurnV, 10.0f, 90.0f, 30.0f, kSlider, kLive, SettingCategory::Controls, nullptr},
    {"input.haptic_strength", "Rumble strength",
     "How hard the controllers buzz, as a percentage of the game's own rumble. "
     "100 is as strong as OpenXR allows; 0 turns it off.",
     GetHapticStrengthV, 0.0f, 100.0f, 100.0f, kSlider, kLive, SettingCategory::Controls,
     nullptr},
    {"input.turn_curve", "Turn response",
     "How the turn rate follows the stick. 1.0 is straight-line; higher gives "
     "finer control near the centre while keeping the same top speed.",
     GetTurnCurveV, 1.0f, 4.0f, 2.0f, kSlider, kLive, SettingCategory::Controls, nullptr},
    {"input.turn_deadzone", "Turn deadzone",
     "How far the right stick must go sideways before it turns you.",
     GetTurnDeadzoneV, 0.05f, 0.95f, 0.45f, kSlider, kLive, SettingCategory::Controls, nullptr},
    {"input.jumpcrouch_deadzone", "Jump/crouch deadzone",
     "How far the right stick must go up or down before it jumps or crouches.",
     GetJumpCrouchDz, 0.05f, 0.95f, 0.6f, kSlider, kLive, SettingCategory::Controls, nullptr},
    {"input.stick_cross_ratio", "Stick separation",
     "Stops turning from clipping a crouch and vice versa. An axis counts only "
     "if it is pushed at least this much harder than the other one. Higher "
     "separates them more; 0 restores the old behaviour.",
     GetCrossRatio, 0.0f, 0.95f, 0.6f, kSlider, kLive, SettingCategory::Controls, nullptr},

    // ---- Advanced -------------------------------------------------------
    {"xr.decouple", "Decouple the XR frame loop",
     "Hands the headset's frame loop to its own pacing thread so it keeps "
     "getting fresh poses while the game is slow. It does not shorten a level "
     "load.",
     GetXrDecoupledV, 0.0f, 1.0f, 0.0f, kToggle, kLive, SettingCategory::Advanced, nullptr},
    {"hotkeys.require_foreground", "Hotkeys need focus",
     "Whether the plugin's hotkeys only work while the game window has focus.",
     GetRequireForeground, 0.0f, 1.0f, 1.0f, kToggle, kLive, SettingCategory::Advanced, nullptr},
    {"hotkeys.leader_timeout_ms", "Leader timeout",
     "How long the leader key stays armed, in milliseconds.",
     GetLeaderTimeout, 500.0f, 30000.0f, 4000.0f, kNumeric, kLive, SettingCategory::Advanced,
     nullptr},
};

constexpr int kSettingCount = static_cast<int>(sizeof(kSettings) / sizeof(kSettings[0]));


// ---------------------------------------------------------------------------
// OMITTED ON PURPOSE, and this list is the handover to M3.
//
// Each of these is a real key in the if-chain above, and each is missing a
// getter that reads its store. None is stubbed, because a control that shows a
// number nothing owns is worse than a control that is absent:
//
//   stereo.half_ipd_units, headtracking.positional, xr.match_headset_fov,
//   weapon.mono_eye, weapon.size, weapon.mono_when_zoomed, weapon.zoom_threshold,
//   viewmodel.pin_mode, viewmodel.detach_lean, viewmodel.hidden, hand.source,
//   rui.ll_arm, render.windowed,
//   aim.mode, aim.deadzone_degrees, aim.gain_degrees, aim.invert_yaw,
//   aim.pitch, autoarm
//
// THE EIGHT HUD KEYS CAME OFF THIS LIST on 2026-09-02 -- rui.ll_scale,
// rui.inset, hud2d.shift_x/y, hud2d.zoom, hud.widget_scale. Each
// already had a published reader in its owning module, so each needed only the
// shim its entry above now uses. The HUD category had existed with no rows in
// it since M2, which meant the panel showed an empty tab.
//
// Adding one is a two-line job in its owning module -- publish the value it
// already holds -- plus one row here. That is M3's first task, and it is
// deliberately left as explicit work rather than hidden behind a cache.
//
// Also absent and NOT an oversight: render.scale and the desktop-window keys
// (M4b has not run), menu.distance_m and friends (M6 owns the quad and there is
// no panel placement to configure yet), and rui.hud_anchor (Phase 1.6 has not
// landed it -- the plan says omit, not stub).
// ---------------------------------------------------------------------------

}  // namespace

int SettingCount() { return kSettingCount; }

const Setting* SettingAt(int index) {
    if (index < 0 || index >= kSettingCount) return nullptr;
    return &kSettings[index];
}

const Setting* FindSetting(const char* name) {
    if (!name) return nullptr;
    for (const auto& setting : kSettings) {
        if (std::strcmp(setting.name, name) == 0) return &setting;
    }
    return nullptr;
}

const char* SettingCategoryName(SettingCategory category) {
    switch (category) {
        case SettingCategory::Home:        return "Home";
        case SettingCategory::View:        return "View";
        case SettingCategory::HandsWeapon: return "Hands & Weapon";
        case SettingCategory::Hud:         return "HUD";
        case SettingCategory::Reticle:     return "Reticle";
        case SettingCategory::Body:        return "Body";
        case SettingCategory::Controls:    return "Controls";
        case SettingCategory::Advanced:    return "Advanced";
        default:                           return "?";
    }
}

const char* SettingTierLabel(SettingTier tier) {
    switch (tier) {
        case SettingTier::Live:    return "";
        case SettingTier::Restart: return "takes effect on the next launch";
        case SettingTier::Careful: return "can visibly break things -- apply deliberately";
        default:                   return "";
    }
}

void SetSettingValue(const Setting& setting, float value) {
    // THE ONE WRITE PATH. Clamped to the registry's own range first, so the
    // slider and the ini cannot disagree about what is accepted, and then
    // straight into ApplyValue -- the same call an ini line makes.
    if (value < setting.minimum) value = setting.minimum;
    if (value > setting.maximum) value = setting.maximum;
    ApplyValue(setting.name, value);
}

unsigned long long UnknownSettingCount() { return g_unknownSettingCount; }


unsigned RunSettingsRegistrySelfCheck() {
    unsigned problems = 0;

    // THE POSITIVE CONTROL RUNS FIRST, AND IT IS NOT OPTIONAL.
    //
    // It was an ini key for about ten minutes. That was wrong twice over: this
    // check runs BEFORE the ini is parsed (see LoadPluginConfig for why), so an
    // ini-set control could never have been active when it ran -- and a control
    // you can forget to switch on is one you will forget to switch on. A sweep
    // never seen to catch anything cannot be trusted when it reports clean, so
    // the control is now unconditional and costs exactly one log line per load.
    {
        const unsigned long long before = g_unknownSettingCount;
        ApplyValue("registry.selfcheck.deliberately_bogus", 1.0f);
        if (g_unknownSettingCount == before) {
            Tf2VrLog("[TF2VR] registry SELF-CHECK CONTROL FAILED: a deliberately bogus name was NOT "
                     "reported as unknown. The check cannot see a failure, so its clean result "
                     "below means nothing. Treat the whole self-check as void.\n");
            ++problems;
        } else {
            Tf2VrLog("[TF2VR] registry self-check control PASSED: the bogus name was caught, so the "
                     "sweep below can see a failure.\n");
        }
    }

    for (int i = 0; i < kSettingCount; ++i) {
        const Setting& setting = kSettings[i];
        const float before = setting.get();
        const unsigned long long unknownBefore = g_unknownSettingCount;

        // Applied at its CURRENT value: a no-op for the state, not for the
        // question. If the if-chain does not know this name, it falls through
        // and the counter moves.
        ApplyValue(setting.name, before);

        if (g_unknownSettingCount != unknownBefore) {
            char line[300]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] registry SELF-CHECK FAILED: '%s' (%s) is in the registry but ApplyValue "
                "does not handle it. That control would silently do nothing.\n",
                setting.name, setting.label);
            Tf2VrLog(line);
            ++problems;
            continue;
        }

        // AND THE ROUND TRIP. A getter reading a different store from its setter
        // is the other way a control silently lies, and from a headset the two
        // look identical.
        const float after = setting.get();
        const float delta = after - before;
        if (delta > 0.0001f || delta < -0.0001f) {
            char line[320]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] registry SELF-CHECK FAILED: '%s' round-trip mismatch -- applied %.4f, "
                "getter now reads %.4f. The getter and the setter are not looking at the same "
                "store.\n",
                setting.name, static_cast<double>(before), static_cast<double>(after));
            Tf2VrLog(line);
            ++problems;
        }
    }

    char line[280]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] registry self-check: %d settings, %u problems, %llu unknown names seen this "
        "process (one of those is the control's own bogus name, and that is the point).\n",
        kSettingCount, problems, g_unknownSettingCount);
    Tf2VrLog(line);
    return problems;
}

// ---------------------------------------------------------------------------
// THE STAGED-EDIT BUFFER. See settings_registry.h for why this is not a second
// store: only the panel writes it, only the panel reads it, and its only exit is
// through SetSettingValue -> ApplyValue.
// ---------------------------------------------------------------------------
namespace {
bool g_staged[kSettingCount]{};
float g_stagedValue[kSettingCount]{};
}  // namespace

float StagedOrLiveValue(int index) {
    if (index < 0 || index >= kSettingCount) return 0.0f;
    if (g_staged[index]) return g_stagedValue[index];
    return kSettings[index].get();
}

void StageSettingValue(int index, float value) {
    if (index < 0 || index >= kSettingCount) return;
    const Setting& setting = kSettings[index];
    // Clamped HERE as well as in SetSettingValue, so what the panel shows while
    // it is staged is what will actually be applied. A value that silently
    // changed at commit time would make the panel a liar about its own edit.
    if (value < setting.minimum) value = setting.minimum;
    if (value > setting.maximum) value = setting.maximum;
    g_staged[index] = true;
    g_stagedValue[index] = value;
}

int StagedEditCount() {
    int count = 0;
    for (int i = 0; i < kSettingCount; ++i) {
        if (g_staged[i]) ++count;
    }
    return count;
}

int CommitStagedSettings() {
    int applied = 0;
    for (int i = 0; i < kSettingCount; ++i) {
        if (!g_staged[i]) continue;
        g_staged[i] = false;
        // THE ONE WRITE PATH, unchanged. A committed edit is indistinguishable
        // from an ini line: same function, same clamp, same if-chain.
        SetSettingValue(kSettings[i], g_stagedValue[i]);
        // AND WRITE IT TO DISK. Without this a menu edit lives until the game
        // closes and then vanishes -- "when I exit and come back in the NEW
        // PANEL RESOLUTION SHOULD BE HELD".
        PersistSettingToIni(kSettings[i].name, g_stagedValue[i]);
        // AND INTO THE PER-HEADSET CACHE, for the one setting the cache also
        // carries. The ini was not enough and the complaint above came back a
        // second time one layer down: LoadHeadsetCache runs AFTER the config,
        // deliberately, and its stored scale wins -- so a panel edit reached
        // the ini, was overridden on the next launch by a cache nothing had
        // updated, and reverted with the explanation dropped by the quiet
        // filter. Measured 2026-09-11: ini 135%, cache 75%, cache won.
        if (std::strcmp(kSettings[i].name, "render.scale") == 0) {
            NoteRenderScaleChosen(g_stagedValue[i]);
        }
        ++applied;
    }
    if (applied) {
        char line[200]{};
        std::snprintf(line, sizeof(line),
                      "[TF2VR] menu: applied %d staged setting%s on close.\n",
                      applied, applied == 1 ? "" : "s");
        Tf2VrLog(line);
    }
    return applied;
}

void DiscardStagedSettings() {
    for (int i = 0; i < kSettingCount; ++i) g_staged[i] = false;
}

// Sanity note for whoever adds the next row: see the getter rule at the bottom
// of settings_registry.h. Every getter above reads an atomic or a plain value.
// None of them lock, and that is a requirement rather than a coincidence.

// ---------------------------------------------------------------------------
// PERSISTENCE. PLAN-VRMENU M4, and its absence is why a menu edit never
// survived a restart: "when I exit and come back in the NEW PANEL RESOLUTION
// SHOULD BE HELD".
//
// SURGICAL, LINE BY LINE. The ini is read, the ONE line for this key is
// rewritten in place, and everything else -- every comment, every blank line,
// every ordering decision, and this file is mostly comments recording measured
// findings -- is copied through byte for byte. A key that is not present is
// appended under a marked block rather than inserted somewhere clever.
//
// Nothing here is clever on purpose. A config rewriter that reformats is a
// config rewriter that will one day eat the reasoning this project is built on.
// ---------------------------------------------------------------------------
bool PersistSettingToIni(const char* name, float value) {
    if (!name || !g_configPath[0]) return false;

    FILE* file = nullptr;
    if (fopen_s(&file, g_configPath, "rb") != 0 || !file) return false;
    std::fseek(file, 0, SEEK_END);
    long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size < 0 || size > 4 * 1024 * 1024) { std::fclose(file); return false; }
    std::vector<char> text(static_cast<size_t>(size) + 1, '\0');
    const size_t read = std::fread(text.data(), 1, static_cast<size_t>(size), file);
    std::fclose(file);
    text[read] = '\0';

    char replacement[192]{};
    std::snprintf(replacement, sizeof(replacement), "set %s = %g", name, static_cast<double>(value));

    std::string out;
    out.reserve(read + 256);
    bool replaced = false;
    const char* cursor = text.data();
    while (*cursor) {
        const char* lineEnd = std::strchr(cursor, '\n');
        const size_t lineLen = lineEnd ? static_cast<size_t>(lineEnd - cursor) : std::strlen(cursor);
        std::string line(cursor, lineLen);

        // Match "set <name>" with any spacing, and only outside a comment.
        std::string trimmed = line;
        size_t firstNonSpace = trimmed.find_first_not_of(" \t");
        bool isOurs = false;
        if (firstNonSpace != std::string::npos && trimmed[firstNonSpace] != '#') {
            std::string body = trimmed.substr(firstNonSpace);
            if (body.rfind("set", 0) == 0) {
                size_t p = 3;
                while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) ++p;
                const size_t nameLen = std::strlen(name);
                if (body.compare(p, nameLen, name) == 0) {
                    size_t after = p + nameLen;
                    while (after < body.size() && (body[after] == ' ' || body[after] == '\t')) ++after;
                    if (after < body.size() && body[after] == '=') isOurs = true;
                }
            }
        }
        if (isOurs && !replaced) {
            replaced = true;
            out += replacement;
        } else if (!isOurs) {
            out += line;
        } else {
            // A duplicate of a key we already rewrote. Directives are applied in
            // FILE ORDER, so a later duplicate would silently win and undo the
            // edit. Commented out rather than dropped, so the wearer can see
            // what happened to a line they may have typed themselves.
            out += "# superseded by the menu: ";
            out += line;
        }
        if (lineEnd) out += '\n';
        cursor = lineEnd ? lineEnd + 1 : cursor + lineLen;
    }

    if (!replaced) {
        if (!out.empty() && out.back() != '\n') out += '\n';
        out += "\n# --- written by the in-headset config menu ---\n";
        out += replacement;
        out += '\n';
    }

    // Written to a temporary beside the real file and then moved over it, so an
    // interrupted write cannot leave the wearer with a truncated config and no
    // working stack.
    char temporary[MAX_PATH + 8]{};
    std::snprintf(temporary, sizeof(temporary), "%s.tmp", g_configPath);
    FILE* outFile = nullptr;
    if (fopen_s(&outFile, temporary, "wb") != 0 || !outFile) return false;
    const size_t written = std::fwrite(out.data(), 1, out.size(), outFile);
    std::fclose(outFile);
    if (written != out.size()) { std::remove(temporary); return false; }
    std::remove(g_configPath);
    if (std::rename(temporary, g_configPath) != 0) return false;

    char line[300]{};
    std::snprintf(line, sizeof(line), "[TF2VR] config: saved '%s = %g' to %s (%s).\n",
                  name, static_cast<double>(value), g_configPath,
                  replaced ? "line rewritten in place" : "appended");
    Tf2VrLog(line);
    return true;
}
