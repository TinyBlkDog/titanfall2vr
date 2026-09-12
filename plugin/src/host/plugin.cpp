#include "northstar_plugin_abi.h"
#include "aim_probe.h"
#include "viewmodel_bones.h"
#include "placement_watchpoint.h"
#include "placement_pin.h"
#include "arms_collapse.h"
#include "mesh_census.h"
#include "bone_reader_watch.h"
#include "draw_item_census.h"
#include "vb_bind_census.h"
#include "aim_census.h"
#include "aim_cmd.h"
#include "ads_lock.h"
#include "ads_probe.h"
#include "ads_zoom.h"
#include "crosshair_scale.h"
#include "menu_overlay.h"
#include "hudwarp.h"
#include "rui_probe.h"
#include "rui_layer_probe.h"
#include "rui_asset_dump.h"
#include "rui_immediate.h"
#include "rui_hunt.h"
#include "lock_hud.h"
#include "vr_input.h"
#include "xinput_pad.h"
#include "viewmodel_visibility.h"
#include "camera_hook.h"
#include "camera_update_hook.h"
#include "config.h"
#include "crash_recorder.h"
#include "engine_cvars.h"
#include "dpad_gesture.h"
#include "hook_registry.h"
#include "jt_probe.h"
#include "scene_reentry.h"
#include "stereo_targets.h"
#include "view_block_camera.h"
#include "temporal_lever.h"
#include "titan_state.h"
#include "flat_harness.h"
#include "d3d11_trace.h"
#include "d3d11_entry_trace.h"
#include "host.h"
#include "version.h"
#include "diagnostics.h"
#include "plugin_cost.h"
#include "present_hook.h"
#include "render_hook.h"
#include "render_resolution.h"
#include "hand_pose.h"
#include "squirrel_natives.h"
#include "viewmodel_hide.h"
#include "player_eye.h"
#include "player_eye_hook.h"
#include "use_target.h"
#include "world_marker.h"
#include "anchor_probe.h"
#include "frustum_census.h"
#include "stereo_experiment.h"
#include "view_build_hook.h"
#include "weapon_settings.h"
#include "xr_input.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// One edge-triggered slot per action, replacing the sixteen hand-maintained
// `static bool xWasDown` pairs this used to be.
bool g_actionWasDown[static_cast<size_t>(Action::Count)]{};

// Mirrors the correction's armed state so the standalone toggle starts from
// what head tracking last set, rather than from a static that could disagree
// with it after a disarm.
bool g_viewmodelCorrecting = false;
// headtracking.recentre_on_spawn. On by default: a height sampled during a
// loading screen is wrong for the whole session and nothing else corrects it.
bool g_recentreOnSpawn = true;
int g_spawnRecentreTicks = 0;

// ARM THE WHOLE STACK WITHOUT TOUCHING THE KEYBOARD.
//
// Bringing VR up took four keys -- F10, F9, DEL, END -- pressed in order, by
// someone wearing a headset who cannot see the keyboard. That is a bad enough
// interface on its own; it is worse than that, because a key that silently
// fails to take (as DEL's eye offset did) is indistinguishable from a key that
// was mistyped, and both look like "the feature does not work".
//
// Each stage is attempted until the state it controls reports itself armed,
// then the next begins.
//
// "ATTEMPTING EARLY IS HARMLESS" WAS WRONG, AND EXPENSIVELY SO.
//
// The first version armed as soon as the swapchain existed, which is during
// startup. The upload hook installed then took slot[48] = ...2A00 -- d3d11's
// raw implementation -- where an in-map install takes the ...28F0 thunk with
// the implementation detoured behind it. The hook received 615 calls for the
// whole session instead of the usual ninety thousand, applied=0 corrections,
// and the weapon rode the head exactly as it did before any of this was fixed.
// view builds=0 and weaponhook hits=0 in the same line: nothing had been
// parsed or rendered yet.
//
// This is the trap the operating notes already warn about -- arming in a menu
// looks exactly like success -- reached by a new route, because a human
// pressing keys naturally does it while playing and a state machine does it as
// early as it possibly can.
//
// So the stages that touch the engine wait for the engine to be running a
// world: weapons parsed (they are parsed at level load) and the camera hook
// actually seeing view builds. OpenXR itself is safe early and stays ungated.
extern "C" volatile std::uint64_t g_cameraHookCallCount;
extern "C" volatile std::uint32_t g_cameraBaseYawBits;
extern "C" volatile std::uint32_t g_cameraBasePitchBits;
extern "C" volatile std::uint32_t g_cameraBaseRollBits;

// autoarm.require_live_view. 1 = hold the ladder until the engine has committed
// real view angles; 0 = the old behaviour, which armed during the load. One ini
// line, because this gate controls the WHOLE ladder and a wrong one arms
// nothing at all.
bool g_autoArmRequireLiveView = true;
bool g_viewmodelCorrectionWanted = true;   // viewmodel.correction
// THE PLAYER HAS A VIEW OF THEIR OWN, which is not the same as a world being
// rendered -- and that distinction is the whole of 1.6-E.
//
// camera_hook.asm latches the engine's committed view angles on every render
// frame, unconditionally. Through the startup menu and the entire loading
// screen all three read EXACTLY zero; they take real values only once the
// player is in the world. Measured, with the wearer's own button presses as
// anchors (STAGE, 2026-08-22):
//
//   autoArm=1 gate=0 | weaponHits=0  cameraCalls=0  | baseYaw=0.00    cursor=1
//   STAGE anchor: pad A pressed (#1)                        <- menu dismissed
//   autoArm=1 gate=0 | weaponHits=65 cameraCalls=0  | baseYaw=0.00    cursor=1
//   autoArm=1 gate=1 | weaponHits=65 cameraCalls=1  | baseYaw=0.00    cursor=0
//   Head tracking armed; recentring on the current head pose
//   autoArm=2 gate=1 | weaponHits=65 cameraCalls=81 | baseYaw=-106.38 cursor=0
//   STAGE anchor: pad A pressed (#2)                        <- LOADING ENDS
//
// The old gate opened at cameraHookCalls == 1 -- the first frame the hook ever
// ran -- so head tracking, the camera writes and the projection layer all came
// up during the load. Two costs, and the second is the worse one: the wearer
// gets a jerky loading screen, and THE RECENTRE REFERENCE IS TAKEN while the
// view angles are still zero and they may be looking anywhere.
//
// Exactly-zero is the uninitialised state rather than a pose: a real view is a
// float that happens to be zero on all three axes essentially never, and if it
// ever were, the first degree of head or stick movement clears it.
//
// Two candidates were REFUTED offline before this one, against archived logs
// and for no headset run: `main-scene passes per frame` (its first sample is
// the hook installing, 19 lines AFTER arming) and CURSOR_SHOWING (already 0
// before arming -- the cursor hides for the load too).
bool CameraBaseAnglesAreReal() {
    return g_cameraBaseYawBits != 0 || g_cameraBasePitchBits != 0 || g_cameraBaseRollBits != 0;
}

bool WorldIsReady() {
    if (WeaponSettingsHits() == 0 || g_cameraHookCallCount == 0) return false;
    if (!g_autoArmRequireLiveView) return true;
    return CameraBaseAnglesAreReal();
}

// THE PLAYER TAKES CONTROL, AND THEY SAY SO THEMSELVES.
//
// Waiting for real view angles was correct and bought almost nothing: measured
// across two runs, the old gate opened at cameraHookCalls == 1 and the new one
// at cameraHookCalls == 3. The engine commits the player's angles within about
// two frames of the hook starting, so the whole stack still came up well before
// the wearer cleared the loading panel -- the ladder armed at line 713, the
// projection layer at 834, and pad A #2 at 932.
//
// THREE INFERRED PREDICATES HAVE NOW BEEN REFUTED, all offline, none costing a
// headset run:
//
//   main-scene passes per frame  -- first sample is the hook installing.
//   CURSOR_SHOWING               -- already 0 before arming; hides for the load.
//   main-scene pass COUNT        -- 3 and 4 in the load window and in gameplay
//                                   alike (43/42 against 40/53). No signal.
//
// So stop inferring the moment and use the one the wearer already named:
// "when I hit A to continue, and we switch into true VR mode, that is when we
// should really be dialing up full OpenXR."
//
// Any deliberate controller input counts, not A specifically. A is what clears
// the panel today, but a map that never shows one would otherwise leave the
// stack unarmed forever -- and a player who is in control always does
// SOMETHING. The input must come AFTER the world is ready, or the press that
// dismissed the launch menu would satisfy it before the level even loads.
bool PlayerHasTakenControl() {
    static bool taken = false;
    if (taken) return true;
    if (!WorldIsReady()) return false;

    const bool pressed = (g_controllerButtons[0] | g_controllerButtons[1]) != 0 ||
                         g_controllerThumbstickClick[0] || g_controllerThumbstickClick[1] ||
                         g_controllerTrigger[0] > 0.5f || g_controllerTrigger[1] > 0.5f ||
                         g_controllerSqueeze[0] > 0.5f || g_controllerSqueeze[1] > 0.5f ||
                         std::fabs(g_controllerThumbstick[0][0]) > 0.5f ||
                         std::fabs(g_controllerThumbstick[0][1]) > 0.5f ||
                         std::fabs(g_controllerThumbstick[1][0]) > 0.5f ||
                         std::fabs(g_controllerThumbstick[1][1]) > 0.5f;
    if (!pressed) return false;
    taken = true;
    Tf2VrLog("[TF2VR] auto-arm: the world is up AND the wearer has taken control -- arming the "
             "engine-side stages now, which is the moment they asked for rather than one this "
             "code guessed.\n");
    return true;
}

// REVERTED TO WorldIsReady, because the premise that motivated the input gate
// was REFUTED by measurement rather than by taste.
//
// The jerky window is 40 fps against 80 fps in gameplay (2 XR breakdowns over
// 3 s versus 4 over 3 s), and it is the window in which gate=0 and NOTHING OF
// OURS IS ARMED. Deferring arming cannot fix a jerk that happens before
// anything arms, so waiting for the wearer to take control bought nothing and
// carried a real risk: a gate keyed on input that fails to fire arms no stack
// at all.
//
// WorldIsReady stays, on its own smaller merit -- it is what stops the recentre
// reference being taken while the engine's view angles are still exactly zero.
bool GameIsRenderingAWorld() { return WorldIsReady(); }
// The cvar enumeration is read-only, so it does not need arming -- but it does
// need TIMING. Cvars are registered as their owning module loads, so a probe run
// during startup reports a fraction of the table and looks exactly like a
// complete answer. It therefore fires on the same condition the engine-side
// auto-arm stages wait for: the game is actually rendering a world.
bool g_cvarProbeRequested = false;
unsigned g_fovScaleTick = 0;
std::atomic<float> g_gameFovScale{0.0f};

// The ship-first latch. Not part of the auto-arm state machine: that ladder is
// the XR stack and its stages are ordered against each other, whereas this is
// one independent feature that happens to share the "wait for a world" gate.
bool g_ruiAutoArmed = false;
bool g_ruiAutoArmWaitLogged = false;
bool g_ruiCensusArmedOnce = false;
bool g_ruiCensusWaitLogged = false;
bool g_ruiLowerLeftArmed = false;

int g_autoArmStage = 0;
unsigned g_autoArmCountdown = 0;
bool g_autoArmWaitLogged = false;
// 0 = off, 1 = XR, 2 = +head tracking, 3 = +stereo, 4 = +projection layer.
// Off by default: the "nothing arms on load" rule still holds for anyone who
// does not ask for this in the INI. See the note in config.h.
int g_autoArmTarget = 0;


// STAGE -- READ-ONLY. It changes no behaviour; it dates the arming ladder
// against the loading screen so the gate above can be replaced from evidence.
//
// WHY. The wearer reports the loading screen is jerky and diagnosed it as
// entering full VR too early. The log agrees about the timing: the predicate
// above is WeaponSettingsHits() > 0 && cameraHookCallCount > 0, weapons are
// parsed AT LEVEL LOAD, and at the frame the ladder advanced the session showed
// aimcmd seq=2, view builds=6, and -- decisively -- `game yaw 0.0, head yaw
// 0.0` with the arms bone cache empty and the RUI thunk at zero calls. A world
// was rendering; the PLAYER WAS NOT IN IT.
//
// TWO CANDIDATE REPLACEMENTS WERE ALREADY REFUTED OFFLINE, against archived
// logs, for no headset run at all:
//
//   main-scene passes per frame  -- first line lands 19 lines AFTER arming,
//                                   because it is the hook installing rather
//                                   than a later state. Not a discriminator.
//   CURSOR_SHOWING               -- already 0 before arming. The cursor hides
//                                   for the loading screen too.
//
// So this logs the surviving candidates on one line, once a second, FROM
// PLUGIN LOAD -- before the ladder runs, which is the whole point, since a gate
// cannot be judged by an instrument that only starts after it fires.
//
// IT NEEDS NOTHING FROM THE WEARER. Both continue panels are dismissed with A,
// and the synthetic pad sees that press, so the A edges logged beside these
// values ARE the timeline anchors: the load ends at the second one.
void LogStageTimeline() {
    static std::uint64_t nextReport = 0;
    const std::uint64_t now = GetTickCount64();
    if (nextReport == 0) { nextReport = now + 1000; return; }
    if (now < nextReport) return;
    nextReport = now + 1000;

    float baseYaw = 0.0f;
    const std::uint32_t bits = g_cameraBaseYawBits;
    std::memcpy(&baseYaw, &bits, sizeof(baseYaw));

    CURSORINFO cursor{};
    cursor.cbSize = sizeof(cursor);
    const bool showing = GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING) != 0;

    char line[300]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR] STAGE autoArm=%d gate=%d worldReady=%d | weaponSettingsHits=%llu "
                  "cameraHookCalls=%llu | baseYaw=%.2f cursorShowing=%d\n",
                  g_autoArmStage, GameIsRenderingAWorld() ? 1 : 0, WorldIsReady() ? 1 : 0,
                  static_cast<unsigned long long>(WeaponSettingsHits()),
                  static_cast<unsigned long long>(g_cameraHookCallCount),
                  static_cast<double>(baseYaw), showing ? 1 : 0);
    Tf2VrLog(line);
}

bool GameHasForeground() {
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId();
}

bool ModifiersMatch(unsigned mods) {
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    // Exact match in both directions, so a bare F9 does not fire while a
    // modifier is held for something else. No default binding uses modifiers.
    return ctrl == ((mods & kModCtrl) != 0) && alt == ((mods & kModAlt) != 0) &&
           shift == ((mods & kModShift) != 0);
}

// Leader-key state. We cannot suppress input -- the game reads raw input and
// sees every key we do -- so a modifier chord fires the game's binding for the
// modifier too, which is how CTRL+ALT+U ended up crouching. A leader avoids
// modifiers entirely, and every key it opens onto is one the game ignores.
std::uint64_t g_leaderExpiryTick = 0;
bool g_leaderWasDown = false;

bool LeaderWindowOpen() { return g_leaderExpiryTick != 0; }

void CloseLeaderWindow() { g_leaderExpiryTick = 0; }

// The low-order "pressed since last query" bit of GetAsyncKeyState is shared
// across the process and can be consumed by the game before this callback runs,
// so the physical down bit is used and the edge is kept here.
bool ActionPressed(Action action) {
    // MEASURED, NOT ASSUMED. RunFrame regressed from 0.036-0.051 ms/frame
    // (2026-08-24) to 0.40-0.67 (2026-09-08), and this function is called 104
    // times a frame with one GetAsyncKeyState each -- the obvious suspect, and
    // arithmetic says it can only be a fifth of it. So it gets its own bucket
    // rather than a theory: RunFrame minus Hotkeys is what actually grew.
    PluginCost::Scope costScope(PluginCost::kHotkeys);
    const KeyChord& chord = BindingFor(action);
    const auto index = static_cast<size_t>(action);
    if (!chord.vk) { g_actionWasDown[index] = false; return false; }
    const bool down = (GetAsyncKeyState(static_cast<int>(chord.vk)) & 0x8000) != 0 && ModifiersMatch(chord.mods);
    // Track the edge even when the binding cannot fire, so that closing the
    // leader window while a key is still held does not produce a stale press.
    const bool edge = down && !g_actionWasDown[index];
    g_actionWasDown[index] = down;
    // A leader binding fires only inside the window; a bare one only outside
    // it. That is what lets LEADER+F9 and bare F9 mean different things.
    if (chord.leader != LeaderWindowOpen()) return false;
    const bool pressed = edge;
    if (pressed && chord.leader) CloseLeaderWindow();
    // GetAsyncKeyState is process-wide and fires while the game is alt-tabbed,
    // which has already produced one "the game was not listening" ambiguity.
    if (pressed && HotkeysRequireForeground() && !GameHasForeground()) {
        char line[160]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] ignoring '%s': the game does not have foreground.\n", ActionName(action));
        Tf2VrLog(line);
        return false;
    }
    return pressed;
}

// Opens the window on a leader press and closes it on timeout. Announced both
// ways: with a headset on, the log is the only feedback that a leader press
// registered, and a silent expiry is indistinguishable from a dead key.
void PollLeaderKey() {
    const KeyChord& leader = LeaderKey();
    if (!leader.vk) { g_leaderWasDown = false; CloseLeaderWindow(); return; }
    const bool down = (GetAsyncKeyState(static_cast<int>(leader.vk)) & 0x8000) != 0 && ModifiersMatch(leader.mods);
    const bool pressed = down && !g_leaderWasDown;
    g_leaderWasDown = down;
    // COUNTED, NOT SILENT. The action path below logs a press the foreground
    // gate drops; this path did not, so a leader pressed while another window
    // had focus looked exactly like a dead key. 2026-09-02: "LEADER+TAB does
    // nothing", and the run's log carried zero leader lines of any kind.
    if (pressed && HotkeysRequireForeground() && !GameHasForeground()) {
        Tf2VrLog("[TF2VR] leader pressed but IGNORED: the game window does not have foreground "
                 "(hotkeys.require_foreground = 1). Click the game window first, or set it to 0.\n");
        return;
    }
    if (pressed && (!HotkeysRequireForeground() || GameHasForeground())) {
        g_leaderExpiryTick = GetTickCount64() + LeaderTimeoutMs();
        char line[192]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] leader pressed; waiting up to %u ms for a diagnostic key.\n", LeaderTimeoutMs());
        Tf2VrLog(line);
        return;
    }
    if (LeaderWindowOpen() && GetTickCount64() >= g_leaderExpiryTick) {
        CloseLeaderWindow();
        Tf2VrLog("[TF2VR] leader window expired; nothing was run.\n");
    }
}

class Titanfall2VrPlugin final : public IPluginId, public IPluginCallbacks {
public:
    const char* GetString(PluginString prop) override {
        switch (prop) {
        case PluginString::NAME: return TF2VR_PRODUCT_NAME;
        case PluginString::LOG_NAME: return "TF2VR";
        case PluginString::DEPENDENCY_NAME: return "TITANFALL2VR";
        }
        return "";
    }
    std::int64_t GetField(PluginField prop) override {
        return prop == PluginField::CONTEXT ? PluginContext::CLIENT : 0x00C0FFEE;
    }
    // ---- THE NORTHSTAR ADAPTER, and it is only an adapter -------------------
    //
    // Three one-line forwards into host.h. Every body they used to hold now
    // lives below the anonymous namespace as a free function, so a second host
    // is a second adapter of this size rather than a rewrite. See host.h.
    void Init(HMODULE self, const PluginNorthstarData*, bool) override { HostOnInit(self); }
    void Finalize() override {}
    // The watchpoint must come off first: debug registers left set on live
    // threads after this DLL is gone would fault into a handler that no longer
    // exists.
    bool Unload() override { ShutdownMenuOverlay(); RestoreViewmodel(); RemoveSquirrelNativeDump(); RemoveClientViewCallsiteHook(); RemoveClientViewBuildHook(); RemoveRenderHook(); RemoveCameraHook(); RemovePlayerEyeHook(); RemoveUseTargetHook(); RemoveWorldMarkerHook(); RemoveD3D11FunctionEntryTrace(); RemoveCameraUpdateTrace(); RemoveD3D11DrawTraceHooks(); RemoveWeaponSettingsHook(); RemovePresentHook(); return true; }
    void OnSqvmCreated(CSquirrelVM*) override {}
    void OnSqvmDestroying(CSquirrelVM*) override {}
    void OnLibraryLoaded(HMODULE module, const char* name) override { HostOnModuleLoaded(module, name); }
    void RunFrame() override { HostOnMainThreadFrame(); }
};

Titanfall2VrPlugin g_plugin;
}

// ---------------------------------------------------------------------------
// THE HOST SEAM -- the three entry points declared in host.h, and the one
// helper only they use. These bodies were moved out of the Northstar plugin
// class verbatim; the class above is now three one-line forwards. Nothing here
// knows what loaded it.
// ---------------------------------------------------------------------------

// One stage per call, retried on a countdown so a stage that is not ready
// yet does not spin or spam the log.
static void AdvanceAutoArm() {
    if (g_autoArmStage >= g_autoArmTarget) return;
    if (g_autoArmCountdown) { --g_autoArmCountdown; return; }
    g_autoArmCountdown = 30;
    switch (g_autoArmStage) {
        case 0:
            // Never adopt a swapchain that is about to be recreated.
            if (!RenderResolutionSettled()) return;
            if (!IsVerifiedGameSwapchainReady()) return;
            if (!IsXrArmed()) { SetXrArmed(true); Tf2VrLog("[TF2VR] auto-arm: OpenXR armed.\n"); return; }
            ++g_autoArmStage;
            return;
        case 1:
            // Everything from here installs hooks into the engine, and
            // installing them before it is drawing a world puts them on the
            // wrong entries. Wait, however long it takes.
            if (!GameIsRenderingAWorld()) {
                if (!g_autoArmWaitLogged) {
                    g_autoArmWaitLogged = true;
                    Tf2VrLog("[TF2VR] auto-arm: OpenXR is up; holding the engine-side stages "
                             "until a world is being rendered. Load into a map.\n");
                }
                return;
            }
            if (!IsHeadLookArmed()) {
                SetHeadLookArmed(true);
                SetHeadTrackingArmed(true);
                if (g_viewmodelCorrectionWanted) {
                    SetVerifiedGameViewmodelCompensation(true);
                    g_viewmodelCorrecting = true;
                    Tf2VrLog("[TF2VR] auto-arm: head tracking and viewmodel correction armed "
                             "(sway suppression is always on).\n");
                } else {
                    Tf2VrLog("[TF2VR] auto-arm: head tracking armed; viewmodel correction LEFT OFF "
                             "(viewmodel.correction = 0), so the weapon family rides the engine's "
                             "own camera. Sway suppression is always on.\n");
                }
                return;
            }
            ++g_autoArmStage;
            return;
        case 2:
            if (!IsAlternateFrameStereoArmed()) {
                ToggleVerifiedGameAlternateFrameStereo();
                return;
            }
            ++g_autoArmStage;
            return;
        case 3:
            if (!IsProjectionLayerArmed()) { ToggleVerifiedGameProjectionLayer(); return; }
            ++g_autoArmStage;
            return;
        case 4:
            // THE MOTION-CONTROLLED WEAPON, ARMED WITHOUT A KEYPRESS.
            //
            // The wearer had to press F3 at the start of every single
            // session to get the gun onto the controller. That is not a
            // diagnostic any more, it is how the weapon is held, and a
            // shipped behaviour should not need a key found by feel with a
            // headset on.
            //
            // Gated on pin.hand, so this arms ONLY the hand-driven mode.
            // The other pin targets really are diagnostics -- they park the
            // viewmodel at a fixed offset to answer a question -- and
            // auto-arming one of those would leave the gun stuck somewhere
            // nobody asked for.
            //
            // It is also why the hand-driven mode no longer expires: see the
            // note at AdvanceViewmodelPlacementPin.
            if (IsPlacementPinHandDriven() && !IsViewmodelPlacementPinArmed()) {
                ToggleViewmodelPlacementPin();
                Tf2VrLog("[TF2VR] auto-arm: hand-driven weapon pin armed (pin.hand=1). No F3 "
                         "needed, and it does not time out.\n");
                return;
            }
            ++g_autoArmStage;
            return;
        default:
            return;
    }
}

void HostOnInit(HMODULE self) {
    // BEFORE THE CONFIG, so a fault while parsing the ini is still recorded,
    // and before any hook is installed. It reads nothing and arms nothing:
    // it registers two handlers that observe and return CONTINUE_SEARCH.
    //
    // The log rotation this pairs with needs no call at all -- it happens
    // inside the first Tf2VrLog of the process, which is the config parser's,
    // and it writes its own report as line 1 of the file it just created.
    // Before anything else logs: the buffer must never hold more than a
    // quarter second, because a hang stops logging and a hang is what this
    // log exists to explain.
    Tf2VrLogStartFlusher();
    InstallCrashRecorder();
    // Before the config, so the derived-resolution path has the last headset's
    // geometry in hand by the time render.scale is read.

    // Bindings and tunables only. Nothing this reads can arm anything: the
    // two flags it can set change what an already-armed correction does,
    // and every action below still has to be pressed.
    LoadPluginConfig(self);
    // AFTER the config, deliberately. The ini carries ONE render.scale for the
    // whole install; the cache carries one PER HEADSET, and the per-headset
    // value is the answer to "what did this wearer choose for THIS headset".
    // Loaded first, the ini overwrote it and switching headsets restored
    // nothing -- measured: cache 117%, ini 50%, and 50% won.
    LoadHeadsetCache();
    // WHICH BUILD IS ACTUALLY RUNNING.
    //
    // A run was spent testing a binding that was not in the loaded DLL: the
    // game was already open when the new build was deployed, so the process
    // kept the one it started with. The file on disk hashed correctly, the
    // ini was right, and the log looked normal -- there was simply nothing
    // in it that said WHICH build produced it, so "not working" and "not
    // loaded" were indistinguishable.
    //
    // __DATE__ and __TIME__ are fixed at compile time, so this line is a
    // fingerprint of the binary rather than of the run. If it does not
    // match the build that was just deployed, the game was running when it
    // was copied and the test is void.
    //
    // __DATE__/__TIME__ ALONE WAS NOT ENOUGH. They are fixed when THIS FILE
    // is compiled, and an incremental build that touches only another
    // translation unit leaves them unchanged -- two different DLLs then
    // report the same stamp, which is exactly what happened while chasing
    // the post-load panel. The DLL's own file time cannot lie that way.
    {
        char path[MAX_PATH]{};
        char written[64] = "unknown";
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&Tf2VrLog), &self);
        if (self && GetModuleFileNameA(self, path, MAX_PATH)) {
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (GetFileAttributesExA(path, GetFileExInfoStandard, &attributes)) {
                SYSTEMTIME utc{}, local{};
                FileTimeToSystemTime(&attributes.ftLastWriteTime, &utc);
                SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
                std::snprintf(written, sizeof(written), "%04u-%02u-%02u %02u:%02u:%02u",
                              local.wYear, local.wMonth, local.wDay,
                              local.wHour, local.wMinute, local.wSecond);
            }
        }
        char line[320];
        std::snprintf(line, sizeof(line),
                      "[TF2VR] titanfall2vr %s (%s) initialized; nothing is armed. "
                      "BUILD %s %s | DLL written %s\n",
                      TF2VR_VERSION_TAG, TF2VR_GIT_SHA, __DATE__, __TIME__, written);
        // FORCED. This is the identity line, and every document tells a person
        // filing a bug to send it: README, INSTALL and the README-FIRST in the
        // zip all say the version is on the log's first line. It was going
        // through Tf2VrLog, which in quiet mode -- the shipped default -- is
        // filtered by KeepWhenQuiet(), and no substring in that allowlist
        // matches it. So the one line that says WHICH BUILD THIS IS was absent
        // from every log a user could ever send. Checked against a real
        // default-configuration log before changing it.
        Tf2VrLogWrite(line, true);
    }
    // HOOK THE HID CHAIN AT LOAD, NOT ON THE FIRST TICK.
    //
    // The capture came back completely empty -- zero HID opens, zero
    // ReadFile calls -- and the disassembly says why: the pad chain lives
    // in one region (HidD_GetAttributes from fn C6C0, ReadFile from fn C230
    // and CA00) and it runs during STARTUP ENUMERATION. The first game tick
    // is long after that, so the probe was installed after everything it
    // wanted to see had already happened.
    //
    // The plugin loads early enough to matter -- early enough that
    // client.dll is not present yet, which vrinput already reports -- so
    // installing here catches the enumeration if inputsystem.dll is up. The
    // per-tick retry stays for the case where it is not.
    {
        // Whether we are early enough is the whole question, so it is
        // stated rather than inferred from an empty capture later.
        const bool inputSystemUp = GetModuleHandleA("inputsystem.dll") != nullptr;
        char line[200];
        std::snprintf(line, sizeof(line),
                      "[TF2VR] hidcap: at plugin load, inputsystem.dll is %s and client.dll "
                      "is %s.\n",
                      inputSystemUp ? "ALREADY LOADED" : "not loaded yet",
                      GetModuleHandleA("client.dll") ? "loaded" : "not loaded yet");
        Tf2VrLog(line);
    }
    // Before any hook is installed, so the calibration run measures an
    // idle machine rather than competing with the game's own threads.
    PluginCost::CalibrateOverhead();
    // MUST be registered HERE and not from the tick. The swap has to be in
    // place before inputsystem.dll runs InitializeXDevices, and the first
    // tick is already too late by construction -- the same timing that made
    // two hidcap captures come back empty with a PASSING self-test.
    RegisterXInputPadLoadWatch();
    LogActiveBindings();
}

void HostOnModuleLoaded(HMODULE module, const char* name) {
    if (name && _stricmp(name, "engine.dll") == 0) {
        EnsureRenderHookInstalled();
        // Before any script runs: a script reading a convar name that does
        // not exist is a script error, and that kills the thread that
        // would have reported it.
    }
    if (name && _stricmp(name, "client.dll") == 0) {
        // Before anything else on this DLL: the registration hook has to be
        // in place before the client VM is built, and this is the earliest
        // moment the plugin is told client.dll exists.
        EnsureSquirrelNativeDumpInstalled(module);
        EnsureViewmodelHideResolved(module);
        EnsureCameraHookInstalled();
        EnsureWeaponSettingsHookInstalled();
    }
    if (name && _stricmp(name, "materialsystem_dx11.dll") == 0) {
        InstallPresentHook();
    }
}

void HostOnMainThreadFrame() {
    // RunFrame forwards to nothing, so the whole body is ours: ~70
    // unconditional Advance/Ensure/Tick calls and, per the audit, 73
    // GetAsyncKeyState calls that each cross into win32k.
    PluginCost::Scope costScope(PluginCost::kRunFrame);
    // FIRST, and before anything that could fault. Twenty-nine lock-free
    // reads and one store; the crash handler then needs no calls at all.
    // Publishing at the TOP means a fault later in this same frame is
    // attributed to the state that was live when it happened, not to the
    // state of the frame before.
    PluginCost::FrameSegBegin();
    PublishArmStateForCrashRecorder();
    PluginCost::FrameSeg(0);
    // THE HEIGHT. The eye-position interceptor (player_eye_hook.h): installs
    // lazily on both player vtables, heartbeats every 5 s. This replaced the
    // view-offset write and the render-camera raise, both now deleted.
    PluginCost::Scope hudTickScope(PluginCost::kHudTick);
    PlayerEyeHookTick();
    // USE TARGETING: the use-search wrap (use_target.h). Installs lazily once
    // client.dll exists, heartbeats every 5 s with per-stage counters.
    UseTargetTick(GameIsRenderingAWorld());
    // WORLD MARKERS: the client vec3 evaluator swaps (world_marker.h).
    WorldMarkerTick(GameIsRenderingAWorld());
    // WORLD-ANCHORING FUNDAMENTALS (anchor_probe.h): the camera write against
    // the head's true orientation, and the aim against a still controller.
    AnchorProbeTick();
    // THE FRUSTUM CENSUS (frustum_census.h): what projection each pass is
    // uploaded with. Sampling happens at the upload site; this only reports.
    FrustumCensusReport();
    ReportHudFovMatch();
    hudTickScope.Stop();
    PluginCost::FrameSeg(1);
    // THE DISCRIMINATOR, on the FRAME clock rather than the census's 2 s one.
    // A stance transition lasts a fraction of a second, so a slow sampler
    // lands either side of the step and reports two stable states with the
    // interesting moment missing between them. The logger itself only
    // writes on change plus a slow heartbeat, so this costs a pair of float
    // reads a frame and a line when something actually moves.
    {
        // NOT GATED ON PlayerEyeReadOnly SUCCEEDING.
        //
        // It was, and the endpoint scan then never ran while the wearer was
        // crouched: the run produced eight lines, all standing, and then
        // nothing -- so a run in which they crouched twice for five seconds
        // each came back with no scan at all and I read that as "they did
        // not crouch". The camera height was 70.03 in the same log. The
        // instrument was silent, not the wearer.
        //
        // The scan needs the camera height, which is always available, and
        // the entity offset only for the LABEL on its line. So it runs
        // unconditionally and reports the offset it managed to get,
        // including that it got none. A diagnostic must not be hostage to a
        // second diagnostic succeeding.
        float originZ = 0.0f, offsetZ = -1.0f;
        const bool haveOffset =
            PlayerEyeReadOnly(GameCameraHeightUnits(), &originZ, &offsetZ, nullptr);
        if (haveOffset) LogViewHeightRecordVsEntity(offsetZ, GameCameraHeightUnits());
        LogViewHeightEndpointCandidates(haveOffset ? offsetZ : -1.0f,
                                        GameCameraHeightUnits());
    }
    PollLeaderKey();
    PollSquirrelNativeDump();
    PublishHandPoseForFrame();
    AdvanceViewmodelHidePulse();
    AdvancePersistentViewmodelHide();
    // Before auto-arm, which waits on it: the mode change recreates the
    // swapchain, and the XR pipeline is built around an adopted one.
    // BEFORE the resolution: only the first mode change of a session renders,
    // so the buffer and the letterbox floor must be right together.
    // THE LETTERBOX FLOOR MUST BE IN PLACE BEFORE THE LOAD STARTS. Measured
    // 2026-09-10 21:01: the floor written at 14.7 s, once a world was ready, and
    // the world still came up letterboxed at 15.1 s (3528x2205, frustum 1.6) --
    // the engine samples mat_letterbox_aspect_* when the level begins loading,
    // and this tick does not run during a load (no STAGE line from 9.7 s to
    // 13.6 s). The one tick-side moment that PRECEDES a load is the button
    // press that starts it. So: no world, any press -> floor in the same tick;
    // released a quarter second later if nothing stalled the tick (menu
    // navigation); kept if the weapon-settings count moved, which is a load in
    // progress; and kept for as long as a world renders.
    {
        // THE STALL IS THE LOAD. Run at 21:26: floor on the press at 12.4 s, the
        // tick then did not run until 15.65 s, and the weapon-settings count had
        // not moved yet -- it moves near the END of a load -- so the quarter-
        // second rule restored the defaults mid-load and the world sampled them.
        // A tick gap over half a second after a press IS a load in progress;
        // the floor is then kept until a world renders.
        static std::uint8_t lastButtons = 0;
        static bool pressFloor = false;
        static bool loadSeen = false;
        static std::uint64_t pressAt = 0;
        static std::uint64_t lastTickMs = 0;
        const bool world = GameIsRenderingAWorld();
        const std::uint8_t buttons = static_cast<std::uint8_t>(
            g_controllerButtons[0] | g_controllerButtons[1] |
            (g_controllerThumbstickClick[0] || g_controllerThumbstickClick[1] ||
             g_controllerTrigger[0] > 0.5f || g_controllerTrigger[1] > 0.5f ? 0x80 : 0));
        const std::uint64_t now = GetTickCount64();
        const std::uint64_t gap = lastTickMs ? now - lastTickMs : 0;
        lastTickMs = now;
        bool wantFloor = world;
        if (!world) {
            if (buttons && !lastButtons) {
                pressFloor = true;
                loadSeen = false;
                pressAt = now;
            }
            if (pressFloor && gap > 500) {
                if (!loadSeen) {
                    char line[160]{};
                    std::snprintf(line, sizeof(line),
                        "[TF2VR] letterbox: tick stalled %llu ms after a press -- a load is in "
                        "progress; the floor stays until a world renders.\n",
                        static_cast<unsigned long long>(gap));
                    Tf2VrLog(line);
                }
                loadSeen = true;
            }
            if (pressFloor && !loadSeen && now - pressAt > 250) {
                pressFloor = false;   // menu navigation: no load followed the press
            }
            wantFloor = pressFloor;
        } else {
            pressFloor = false;
            loadSeen = false;
        }
        lastButtons = buttons;
        // MEASURED 2026-09-10 22:07 AND WITHDRAWN: the floor was in force from the press
        // that started the load, through the whole load, read back correctly, and
        // the world still came up letterboxed. The engine takes these values when
        // the VIDEO MODE is set and keeps them for the session, menu and world alike.
        // Floor always, as the Quest 3 always had; the menu crop on a squarer
        // headset is KNOWN-ISSUES 12. The press/stall machinery above is kept as a
        // measured instrument and decides nothing.
        (void)wantFloor;
        ApplyLetterboxFloorIfRequested(true);
    }
    ApplyRenderResolutionIfRequested();
    PluginCost::FrameSeg(2);
    // GATED ON THE WORLD AT THE CALL SITE, which is how every other probe
    // here waits, and the first version of this was NOT and it cost a run.
    //
    // The evidence came from the A1 aspect sweep (removed 2026-09-08, its
    // question answered): it installed the camera detours itself, as early
    // as a device existed -- 30.7 s, against a world that came up at 65.6 s.
    // That is precisely the trap the auto-arm note above describes: an early
    // install takes slot[48] = ...2A00, d3d11's raw implementation, where an
    // in-map install takes the ...28F0 thunk with the implementation
    // detoured behind it. The hooks reported installed, every falsifier
    // passed, and RSSetViewports then logged "0 distinct rectangles over 120
    // frames" while a world was rendering.
    //
    // "Attempting early is harmless" was wrong for the arm ladder and it is
    // wrong here, by a new route: a state machine arms as early as it
    // possibly can, and a human pressing a key never would.
    // THE PER-HEADSET RESOLUTION, THE MOMENT THE RUNTIME HAS REPORTED IT.
    //
    // On the game tick and OUTSIDE the world gate, deliberately. The panel size
    // is known at ~6.5 s and OpenXR is presenting before the game's logo; the
    // world does not exist until ~44 s on a new campaign because of the opening
    // cutscene. Waiting for it meant 45 seconds of a 16:9 backbuffer stretched
    // into a 1.43 headset -- the crop the wearer could not see the cutscene
    // through -- followed by a live mode change mid-session, which is
    // KNOWN-ISSUES 1.
    //
    // The hang this call must avoid came from running it INSIDE XR init, on that
    // thread, mid-session-setup. The game tick is outside that by construction.
    // A world rendering was only ever a proxy for it, and an expensive one.
    ApplyDerivedSetupNow();

    if (GameIsRenderingAWorld()) {
        // C1. Read-only, three streams, and gated on a world for the reason
        // every capped diagnostic here is: a first-N sample taken before the
        // level is up measures the loading screen and then stops.
        AimCensusTick();
        // RE-ARM THE HAND-DRIVEN PIN AFTER A RESPAWN OR A LEVEL RELOAD.
        //
        // The pin disarms ITSELF when the viewmodel it cached is freed --
        // "the instance stopped being a C_BaseViewModel -- weapon switch or
        // respawn" -- which is correct. What was missing is anything to put
        // it back: auto-arm reaches stage 4 once and then returns early
        // forever, so after a reload the gun stayed wherever the engine
        // draws it and the wearer met it as "the gun was no longer attached
        // to my hand, it was pinned where it would be in flat mode".
        //
        // A watchdog rather than a one-shot ladder stage, because the
        // condition it repairs can recur any number of times in a session.
        // Gated on pin.hand so it only ever restores the mode the wearer
        // actually configured, and rate-limited so a genuinely unavailable
        // viewmodel costs one line every few seconds rather than a stream.
        // ONLY AFTER THE LADDER HAS FINISHED, and this gate is the whole
        // difference between a repair and a regression. This block runs
        // BEFORE AdvanceAutoArm in the frame, so without the gate it armed
        // the pin the moment a world appeared -- far earlier than stage 4
        // does -- and the pin captured its hand reference before that
        // reference was good. The wearer met it as the gun being pinned in
        // the wrong place and rotating about the wrong axis, on the FIRST
        // load, which is the tell that this ran instead of the ladder rather
        // than after it. A watchdog must never pre-empt the thing it guards.
        // AND ONLY WHEN THE LADDER WAS CONFIGURED TO ARM THE PIN AT ALL.
        // "Finished" alone is not enough: with autoarm = 0 the ladder is
        // finished before it starts (stage 0 >= target 0), and on the
        // 2026-08-29 flat run this watchdog armed the pin on a profile
        // whose whole point was that nothing arms -- pin.hand is kept at 1
        // in PROFILE-FLAT precisely because it is documented as a mode
        // selector, not an armer. The pin's arming stage is case 4, which
        // only runs when the target is 5 or more, so that is the gate: the
        // watchdog restores a state the ladder established, and a ladder
        // that never establishes it has nothing to restore.
        if (g_autoArmTarget >= 5 && g_autoArmStage >= g_autoArmTarget &&
            IsPlacementPinHandDriven() && !IsViewmodelPlacementPinArmed()) {
            static std::uint64_t nextPinRearm = 0;
            const std::uint64_t nowTick = GetTickCount64();
            if (nowTick >= nextPinRearm) {
                nextPinRearm = nowTick + 3000;
                Tf2VrLog("[TF2VR] weapon pin re-arming: it disarmed itself (respawn, weapon "
                         "switch or level reload) and pin.hand is set, so the gun goes back on "
                         "the hand without a keypress.\n");
                ToggleViewmodelPlacementPin();
            }
        }
    }
    AdvanceAutoArm();
    PluginCost::FrameSeg(3);
    // game.fov_scale -- SET THE GAME'S OWN FOV RATHER THAN OVERRIDE ITS
    // PROJECTION.
    //
    // xr.match_headset_fov clamps the frustum to the headset's on every
    // main-scene upload, which also clamps it during ADS -- so the zoom the
    // wearer relies on stops working, and it read as "ADS is very messed up,
    // not sharp, zooming in at an odd angle". Overriding a projection the
    // game is actively animating was always going to fight it.
    //
    // cl_fovScale is the engine's own lever and it composes correctly:
    // the base FOV moves, and ADS still narrows RELATIVE to it.
    //
    // The log already says what it controls. Vertical FOV measured 92.9 deg
    // at scale 1.55 across every buffer aspect tried -- 1.778, 1.600, 1.105
    // -- while horizontal moved with the aspect each time (tangent ratios
    // 1.775 and 1.599 matching their buffers). So the game holds VERTICAL
    // constant and derives horizontal from the aspect, and cl_fovScale sets
    // that vertical. At a 1.4283 buffer, 90 deg vertical gives 110 across,
    // which is the headset exactly.
    //
    // Re-applied if it drifts: a map load can replicate a server value over
    // it, and a silently reverted setting looks identical to one that never
    // worked.
    if (g_gameFovScale.load(std::memory_order_acquire) > 0.01f && ++g_fovScaleTick % 120 == 0) {
        float current = 0.0f;
        const bool readable = TryReadCvarFloat("cl_fovScale", current);
        const float wanted = g_gameFovScale.load(std::memory_order_acquire);
        if (readable && std::fabs(current - wanted) > 0.001f) {
            if (TrySetCvarFloat("cl_fovScale", wanted)) {
                char line[240]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] cl_fovScale %.3f -> %.3f. This moves the game's own vertical FOV, so "
                    "ADS still narrows relative to it -- unlike xr.match_headset_fov, which "
                    "clamps the frustum on every upload and breaks the zoom.\n",
                    current, wanted);
                Tf2VrLog(line);
            }
        }
    }

    // THE CVAR PROBE NO LONGER SELF-ARMS AT THE WORLD GATE, AND THAT GATE
    // WAS THE LEVEL-ENTRY FREEZE.
    //
    // It used to request itself the first time GameIsRenderingAWorld() went
    // true, which is the exact frame a level finishes loading. Measured, on
    // the game thread, at that frame: 5845 ms before SafeName stopped doing
    // a VirtualQuery per character, and 1764 ms after -- against a
    // STALE WINDOW of 1770 ms in the same run, so the probe was essentially
    // the whole of it.
    //
    // It is a diagnostic that dumps the complete cvar table to a file, it
    // has a binding of its own (LEADER then SCROLLLOCK), and nothing in
    // normal play reads what it produces. There is no reason for the wearer
    // to pay for it on every level entry, so they no longer do.
    //
    // What is left of the wait at a level entry is the GAME's own loading,
    // which is why entering the same level a second time is much faster --
    // that is its asset cache, and not ours to shorten.
    (void)g_cvarProbeRequested;
    // NOT auto-requested, and that is the lesson from the crash rather than
    // caution about it. "Passive" described what the hook READS; it patches
    // code either way, and this project's own rule is that anything touching
    // the engine is hotkey-gated and never automatic on load. Auto-installing
    // a detour meant a mistake in it could only ever present as a crash on
    // level entry, with no way to get in and read the log first.
    // 20 degrees, which is unmistakable at a wall and small enough that the
    // player can still see where the rounds went.
    if (ActionPressed(Action::ToggleWeaponPin))
        SetWeaponPinnedToController(!IsWeaponPinnedToController());
    if (ActionPressed(Action::RecentreWeaponPin)) RecentreWeaponPin();
    if (ActionPressed(Action::CycleWeaponPinSlot)) CycleWeaponPinSlot();
    if (ActionPressed(Action::ToggleDeterministicBoneSetup)) ToggleDeterministicBoneSetup();
    if (ActionPressed(Action::ToggleWeaponBonePin))
        SetWeaponBonePinEnabled(!IsWeaponBonePinEnabled());
    if (ActionPressed(Action::CycleWeaponBoneTarget)) CycleWeaponBoneTarget();
    // Passive: counts which of the two sway computations the pilot weapon
    // actually uses. Installed by the flat session, never automatically --
    // it patches code, and this project's rule is that nothing which
    // patches code arms itself on load.
    // Passive: counts which of the two sway computations the pilot weapon uses.
    // Installed on demand with the flat session, never automatically.
    AdvanceBoneProbe();
    AdvanceCvarProbe();
    if (ActionPressed(Action::ToggleAimProbe)) ToggleAimProbe();
    // Passive on install; substitutes nothing until F4.
    PluginCost::FrameSeg(4);
    if (ActionPressed(Action::ArmOpenXr)) {
        const bool armed = !IsXrArmed();
        SetXrArmed(armed);
        Tf2VrLog(armed ? "[TF2VR] OpenXR armed.\n" : "[TF2VR] OpenXR disarmed.\n");
    }
    if (ActionPressed(Action::ToggleHeadTracking)) {
        // One key arms both: the XR side publishes the pose, the camera
        // side consumes it. Splitting them only creates a state where one
        // is on and the other is not.
        const bool armed = !IsHeadLookArmed();
        SetHeadLookArmed(armed);
        SetHeadTrackingArmed(armed);
        // The viewmodel correction only makes sense while the head is
        // driving the view, so it is not a separate thing to remember.
        SetVerifiedGameViewmodelCompensation(armed);
        g_viewmodelCorrecting = armed;
        Tf2VrLog(armed ? "[TF2VR] head tracking armed.\n" : "[TF2VR] head tracking disarmed.\n");
    }
    // Toggles the viewmodel correction on its own, for A/B checking it
    // against head tracking without disarming head tracking itself.
    if (ActionPressed(Action::ToggleViewmodelCorrection)) {
        g_viewmodelCorrecting = !g_viewmodelCorrecting;
        SetVerifiedGameViewmodelCompensation(g_viewmodelCorrecting);
    }
    if (ActionPressed(Action::ToggleAlternateFrameStereo)) ToggleVerifiedGameAlternateFrameStereo();
    if (ActionPressed(Action::ToggleProjectionLayer)) ToggleVerifiedGameProjectionLayer();
    if (ActionPressed(Action::ToggleViewmodelWorldFov))
        SetViewmodelMatchWorldFov(!IsViewmodelMatchWorldFov());
    if (ActionPressed(Action::ToggleMatchHeadsetFov))
        SetMatchHeadsetFov(!IsMatchHeadsetFov());
    if (ActionPressed(Action::ToggleDeclareRenderedFov))
        SetDeclareRenderedFov(!IsDeclareRenderedFovArmed());
    if (ActionPressed(Action::ToggleFitHorizontal))
        SetFitHorizontal(!IsFitHorizontalArmed());
    // The resolution/binding experiment. Both run the same mat_setvideomode
    // path the panel's slider uses; they differ only in whether the size
    // actually moves, which is the one variable the question needs.
    if (ActionPressed(Action::ToggleViewportFull))
        SetViewportFull(!IsViewportFullArmed());
    if (ActionPressed(Action::ToggleDecouple))
        SetXrDecoupled(!IsXrDecoupled());
    // Multiplicative, so a press feels like the same change at either end
    // of the range rather than a big step when the gun is already small.
    if (ActionPressed(Action::WeaponSmaller)) SetWeaponSize(WeaponSize() / 1.1f);
    if (ActionPressed(Action::WeaponLarger)) SetWeaponSize(WeaponSize() * 1.1f);
    if (ActionPressed(Action::TogglePositionalTracking))
        SetHeadPositionalTracking(!IsHeadPositionalTracking());
    if (ActionPressed(Action::ToggleSyntheticPose))
        CycleSyntheticPoseMode();
    // Multiplicative steps, so the same keypress feels like the same change
    // at either end of the range.
    // WORLD SCALE, not the raw half-IPD. The number these land on is the one
    // to type into the panel afterwards, and unlike NudgeHalfInterpupillary
    // this does not set g_halfIpdExplicit -- the runtime keeps supplying the
    // baseline, which is what a default everyone inherits has to be built on.
    // F3 shrinks the world (people look BIGGER); F6 grows it.
    if (ActionPressed(Action::IpdSmaller)) SetWorldScale(WorldScale() / 1.05f);
    if (ActionPressed(Action::IpdLarger)) SetWorldScale(WorldScale() * 1.05f);

    if (ActionPressed(Action::ReloadConfig)) ReloadPluginConfig();
    if (ActionPressed(Action::LogBindings)) LogActiveBindings();
    if (ActionPressed(Action::StepRuiMarkerLadder)) StepRuiMarkerLadder();
    if (ActionPressed(Action::StepUseAim)) StepUseAim();
    if (ActionPressed(Action::StepWorldMarker)) StepWorldMarkerArm();
    if (ActionPressed(Action::ToggleHud2dZoomAB)) ToggleHud2dZoomForMarkerTest();
    if (ActionPressed(Action::ToggleHudFovMatch)) SetHudFovMode((HudFovMode() + 1) % 3);
    if (ActionPressed(Action::StepMarkerSize)) StepRuiMarkerSize();
    if (ActionPressed(Action::StepMarkerTextX)) StepRuiMarkerTextX();
    if (ActionPressed(Action::StepHudWidgetScale)) StepHudWidgetScale();
    if (ActionPressed(Action::StepLowerLeftShiftX)) StepHudLowerLeftShiftX();
    if (ActionPressed(Action::TagVisibleWidgets)) RuiTagVisibleWidgets();
    if (ActionPressed(Action::MarkFrameSpike)) XrReportRecentFrames();
    if (ActionPressed(Action::StepInstructionPanelScale)) StepInstructionPanelScale();
    if (ActionPressed(Action::StepGauntletTimerShift)) StepGauntletTimerShift();
    if (ActionPressed(Action::StepHudWidgetSkip)) StepHudWidgetSkip();
    if (ActionPressed(Action::StepHudWidgetNamedOnly)) StepHudWidgetNamedOnly();
    if (ActionPressed(Action::StepHudWidgetScreenSpaceOnly)) StepHudWidgetScreenSpaceOnly();
    if (ActionPressed(Action::StepHudCockpitScale)) StepHudCockpitScale();
    if (ActionPressed(Action::StepHudBarsShiftY)) StepHudBarsShiftY();
    if (ActionPressed(Action::StepHudNameplateScale)) StepHudNameplateScale();
    if (ActionPressed(Action::StepHudNameplateMode)) StepHudNameplateMode();
    if (ActionPressed(Action::ToggleMarkerUnzoom)) ToggleHudMarkerUnzoom();
    if (ActionPressed(Action::ToggleVguiDraw)) ToggleVguiDraw();
    if (ActionPressed(Action::DumpLabelWidget)) {
        // F5 = "the friendly name vanished at THIS step". The same meaning
        // at every step of the ladder, so there is no mode to keep track of.
        MarkRuiWidgetHunt();
        // And the pin digest: F5 is also "I see the floating rectangle NOW".
        MarkWeaponPinDigest();
    }
    if (ActionPressed(Action::ResetWidgetHunt)) RuiHuntReset();
    if (ActionPressed(Action::ToggleHudAnchorSign)) ToggleHudAnchorSign();
    if (ActionPressed(Action::StepWidgetHunt)) StepRuiWidgetHunt();
    if (ActionPressed(Action::MarkRuiImmediate)) {
        // ONE PRESS, ONE ARM, ONE IMAGE. The shot is requested here rather
        // than left to a separate F6 so there is no gap between blanking a
        // widget and the frame that shows what happened, and so the lock
        // count in the picture belongs to the arm named beside it.
        ToggleLockHudViewFix();
        RequestSameFrameEyePair();
    }
    if (ActionPressed(Action::CaptureLockHuntFrame)) {
        LogRuiLockHuntState();
        RequestSameFrameEyePair();
    }
    if (ActionPressed(Action::ToggleHudScaleAB)) ToggleHudWidgetScaleAB();
    if (ActionPressed(Action::CaptureEyes)) {
        // THE LABEL'S POSITION AT THE INSTANT OF THE SHOT. Three runs of
        // regression could not separate one label instance from another --
        // eleven different widgets reported an "instance 0" of exactly 294
        // samples, so the tracks were splitting on a gap in TIME, not on
        // entity. A burst sidesteps tracking entirely: at one moment the
        // widget draws K layers and all K positions are logged, so two
        // presses at two head yaws can be compared set against set.
        RuiLayerProbeBurst();
        RequestSameFrameEyePair();
        Tf2VrLog("[TF2VR] F6 SCREENSHOT requested. On the next Present the backbuffer is "
                 "written to %TEMP%\\tf2vr-F6-NN.bmp, numbered per press so several shots in "
                 "one run all survive. The agent reads them off disk; nothing has to be sent.\n");
    }
    // PLAN-CURRENT F1. Same key arms and disarms; the disarm is what writes
    // the report, so a run that is never disarmed produces nothing.
    if (ActionPressed(Action::ToggleViewmodelPlacementWatchpoint))
        ToggleViewmodelPlacementWatchpoint();
    // Closes the capture on its own clock. The key above only stops it
    // early, which matters because the watchpoint is the one probe here
    // that can make the game too slow to press a key on.
    AdvanceViewmodelPlacementWatchpoint();
    if (ActionPressed(Action::DumpCameraPassMatrices)) RequestCameraPassMatrixDump();
    // Enumerates what the model divides into, then steps through the
    // combinations. Restores m_nBody itself when it finishes or is stopped.
    if (ActionPressed(Action::ToggleViewmodelVisibilityProbe)) ToggleViewmodelVisibilityProbe();
    // GATE 1 FH1. Read-only: field read-back plus a ShouldDraw counter on the
    // arms class vtable. Closes its own ten-second window in Advance below.
    // D2, the blind height A/B. Three keys, one verdict each, no adjustment.
    if (ActionPressed(Action::ToggleVrInput)) ToggleVrInput();
    // BEFORE the input consumers, so movement suppression and the pad's
    // d-pad bits are one decision made from one sample of the stick. Two
    // consumers each deciding for themselves is two things that can drift.
    AdvanceDpadGesture();
    AdvanceVrInput();
    PluginCost::FrameSeg(5);
    // PLAN-CURRENT P0. One key, four states, and each press names the state
    // it entered. Advance emits the falsifier summary on its own clock, so
    // a run that is never disarmed still leaves a readable instrument.
    if (ActionPressed(Action::CycleAimCmd)) CycleAimCmd();
    AdvanceAimCmd();
    // The engine's own ADS zoom, one tick a frame: two calls and a field read.
    // C1 consumes it on every camera upload, so it must not depend on any
    // probe being armed.
    TickAdsZoom();
    // KICKOFF-TITAN-ARMS-2026-09-03: the titan-state READ. Polls
    // C_Player::IsTitan on the local player every frame (IsPlayer as the
    // control); logs every edge and a 5 s heartbeat with the world state
    // labelled. titan.gate decides whether anything acts on it.
    AdvanceTitanProbe(GameIsRenderingAWorld());
    // P0-c's offline falsifier pair, once, after the world gate has let the
    // ordinary installs happen -- by then the registry holds whatever this
    // configuration installs, so the positive control has something real to
    // hit.
    {
        static bool registrySelfChecked = false;
        if (!registrySelfChecked && GameIsRenderingAWorld()) {
            registrySelfChecked = true;
            HookRegistrySelfCheck();
        }
    }
    // viewmodel.compensation from the ini. Cheap: two atomic loads once the
    // want has been applied, and nothing at all when it was never set.
    TickViewmodelCompensationWanted();
    // PLAN-ADS S2. Read-only, ini-armed, and every second it prints either a
    // measurement or the reason it has none. Placed AFTER AdvanceAimCmd so
    // the aim ray it reads is this frame's rather than the previous one's.
    AdvanceAdsProbe();
    AdvanceCrosshairScale();
    AdvanceReticleHidden();
    // Before every consumer that asks it, so the gun, the bullet and the turn
    // all read one latch computed once this frame.
    AdvanceAdsLock();
    AdvanceAdsKeepReticle();
    AdvanceTurnSpeed();
    AdvanceHaptics();
    // PLAN-VRMENU M1. The chord toggles the flat overlay; Advance emits the
    // falsifier line on its own clock, so a run that never presses the key
    // still leaves a readable instrument.
    if (ActionPressed(Action::ToggleConfigMenu)) ToggleMenu();
    AdvanceMenuOverlay();
    AdvanceUploadCensus();
    PluginCost::FrameSeg(6);
    // PLAN-CURRENT rung 1. Each stage restores itself on its own clock, so
    // the run is bounded whether or not the key is pressed again.
    // ONE PRESS DOES BOTH: arms the HUD size and points the six-key cluster at
    // it. Nothing to cycle, nothing to count, and no log to read -- which is
    // the only thing that works with a headset on.
    // F5 NOW STEPS RUNG A2's CANDIDATE LADDER, not the layer-type sweep.
    //
    // The type sweep is measured closed for the lower-left group -- types
    // 1, 3, 4, 6 and 7 were each isolated and none of them moved it -- and
    // A1 explained why: the type byte lives on the SHARED context, one
    // value for a whole batch, so isolating a type was always hitting ~20
    // different widget classes at once. The finer key is the draw target
    // each layer resolves to, and that is what this steps.
    //
    // Repointed rather than left bound to the old sweep, because a key that
    // still works but tests something already closed is worse than a dead
    // one: it produces results that look current.
    // A2 is answered -- the lower-left group is ui(11)+0x1010 (tactical,
    // ordnance, separator) and ui(11)+0x4320 (weapon and ammo) -- so F5
    // steps A3's transform ladder now. `rui.a2 = 1` puts it back on the
    // suppression ladder, which is kept rather than deleted because the
    // health readout was never on screen during the A2 sweep.
    // F5 IS NOW THE LOWER-LEFT'S OWN CALIBRATION KEY, the mirror of what F6
    // is for the top-left: one press arms the group and points the six-key
    // cluster at it, and the cluster sizes it from there.
    //
    // The ladders it used to step have both done their job -- A2 named the
    // two targets, A3 measured the lever -- and a stepped ladder is the
    // wrong interface for a headset anyway: it makes the wearer COUNT
    // presses and wrap around past the answer. Dialling monotonically until
    // it looks right needs neither. Both ladders stay reachable from the
    // ini (`rui.a2`, `rui.a3`) for a run that needs to hunt again.
    AdvanceRuiSuppression();
    AdvanceRuiWarpBurst();
    AdvanceRuiAutoShots();
    PluginCost::Scope ruiTickScope(PluginCost::kRuiTick);
    AdvanceRuiSuspectLadder();
    AdvanceRuiBlockCensus();
    // The classifier reports on its own clock; read-only, inert when unarmed.
    ReportWidgetClassification(false);
    ruiTickScope.Stop();
    // PHASE 1 RESULT, on the same one-shot latch as the top-left inset and for
    // the same reason: F5 has to stay a real A/B against the game's own
    // layout, so a clock-driven re-arm would undo it a tick later.
    // LATCHES ON SUCCESS, NOT ON THE ATTEMPT. The descriptors are found
    // through the census, which needs a few seconds of a rendered world to
    // populate, so the first attempt can legitimately find nothing.
    if (RuiLowerLeftArmOnLoad() && !g_ruiLowerLeftArmed && GameIsRenderingAWorld()) {
        static unsigned wait = 0;
        if (wait) { --wait; }
        else {
            wait = 60;
            ArmRuiLowerLeft();
            if (RuiLowerLeftArmed()) g_ruiLowerLeftArmed = true;
        }
    }
    AdvanceRuiA3();
    PluginCost::FrameSeg(7);
    // SHIP-FIRST (PLAN-CURRENT Phase 1): the half of the HUD that works
    // arms itself, at the ini's inset, and stops costing a keypress every
    // session.
    //
    // ONCE, AND ONLY ONCE -- the latch is the whole point. F6 is an A/B
    // against the original, so a `wanted` flag re-armed from a clock (the
    // arms.collapse pattern) would restore the inset a tick after the
    // wearer pressed the key to see it removed.
    //
    // Gated on a world being rendered for the same reason every other
    // engine-side stage is: this patches a call site in engine.dll, and
    // arming in the menu looks exactly like success. The wait is announced
    // so a run that never arms says why instead of looking inert.
    // PHASE 1 RUNG A1: the read-only identity census, on the same gate and
    // the same one-shot latch. It installs the call-site patch and changes
    // nothing, so it can share a run with anything that is not the inset.
    if (RuiCensusWanted() && !g_ruiCensusArmedOnce) {
        // rui.census_early -- ARM BEFORE THE WORLD GATE, for the two-screen
        // question and nothing else.
        //
        // The gate below is "weapons parsed + view builds seen", which opens
        // only once a level is loading -- so the census cannot see the FIRST
        // continue panel at all, and snapshot #1 correctly reported itself
        // NOT ARMED rather than printing an empty report that would read
        // like "no screens were drawn".
        //
        // Snapshot #2 already proved there are TWO full-screen surfaces
        // coexisting at the second press -- ui(11)+0x5B1B0 and +0x5B500,
        // both 1920x1080, both drawn on all 294 frames to that point, then
        // retiring 17 frames apart (294 and 311) while the gameplay HUD
        // elements kept climbing. What is missing is whether one of them is
        // the FIRST panel, still being drawn underneath after it was
        // dismissed. That needs the census armed before the level loads.
        //
        // Read-only: the call-site patch is inert until a layer type is
        // selected, and the census only tallies.
        if (RuiCensusEarly() && GetModuleHandleA("engine.dll")) {
            g_ruiCensusArmedOnce = true;
            Tf2VrLog("[TF2VR] RUI identity census armed EARLY (rui.census_early), before the "
                     "world gate, so the first continue panel is visible to it.\n");
            ArmRuiCensus();
        } else if (!GameIsRenderingAWorld()) {
            // SAY SO, ONCE. A census that is waiting on a gate and a census
            // that is broken produce the same thing -- an empty log -- and
            // this project has already spent a run on that difference. The
            // gate is `weapon settings parsed AND the camera hook seeing
            // view builds`, neither of which needs OpenXR, so it should
            // open in the flat profile too; if this line is the last thing
            // in the log, it did not.
            if (!g_ruiCensusWaitLogged) {
                g_ruiCensusWaitLogged = true;
                Tf2VrLog("[TF2VR] RUI identity census: wanted, holding until the game is "
                         "rendering a world (weapons parsed + view builds seen). Load into "
                         "a map.\n");
            }
        } else {
        g_ruiCensusArmedOnce = true;
        ArmRuiCensus();
        // THE A1 BASELINE CAPTURE IS GONE, AND IT WAS COSTING A LEVEL ENTRY.
        //
        // It took a full-frame backbuffer readback here, at the exact frame
        // the world gate opens, as the control image for rung A2's pixel
        // diff. That comparison is finished (PHASE 1 is closed), so nothing
        // reads the file any more -- but the capture kept running on every
        // level entry the wearer plays.
        //
        // A capture is not a cheap thing to leave lying around. DumpBackbuffer
        // creates a staging texture and calls Map(D3D11_MAP_READ), which is a
        // BARRIER: the game thread stops until the GPU has finished
        // everything queued ahead of it. At a level entry that queue is the
        // level load. It is on the game thread, inside Present, so the whole
        // game -- including input -- waits for it.
        //
        // In both instrumented runs on 2026-08-22 the line immediately before
        // the freeze was this capture, flat and VR alike. That is also why
        // halving render.width/height refuted nothing: the cost here is the
        // SYNC, which does not scale with the image, not the bytes, which do.
        //
        // If a future rung needs a baseline, take it on a keypress, away
        // from a load, and read the cost off the capture's own timing line.
        }
    }
    AdvanceRuiCensus();
    AdvanceRuiImmediate(GameIsRenderingAWorld());
    PluginCost::FrameSeg(8);
    AdvanceLockHud(GameIsRenderingAWorld());
    PluginCost::FrameSeg(9);
    if (RuiArmOnLoad() && !g_ruiAutoArmed) {
        if (GameIsRenderingAWorld()) {
            g_ruiAutoArmed = true;
            // Deliberately NOT SetCalibrationTarget: that would take the
            // six-key cluster away from the pivot on every load, and hand
            // calibration is working stack. F6 still repoints it.
            ToggleRuiLayerScale();
        } else if (!g_ruiAutoArmWaitLogged) {
            g_ruiAutoArmWaitLogged = true;
            Tf2VrLog("[TF2VR] HUD size: wanted from the ini, holding until the game is "
                     "rendering a world. Load into a map.\n");
        }
    }
    AdvanceRuiProbe();
    // The 64-byte control is measured closed and stays inert at 1.0; its
    // Advance is kept because the INI can still reach it.
    AdvanceArmsCollapse();
    AdvanceMeshCensus();
    AdvanceVbBindCensus();
    // AdvanceBoneReaderWatch();  // done: runs 2-4 named the copier and the consumer
    AdvanceDrawItemCensus();
    PluginCost::FrameSeg(10);
    // A quarter of an inch a press: fine enough to land on, coarse enough
    // to cross a foot in a reasonable number of presses while held.
    // COARSE BY DEFAULT, AND EVERY PRESS MOVES THE GUN.
    //
    // Two things were wrong here and both made the control feel dead to
    // someone wearing a headset.
    //
    // The step was 0.25 units. grip is the negative of the model-space point
    // that sits in the hand, so the lever arm to remove is tens of units --
    // a hundred presses away, and that is if the direction is right.
    //
    // Worse, the "pivot" mode compensated the position so the gun did NOT
    // move on a press. That was deliberate, to isolate the pivot from the
    // translation -- and it is exactly wrong for someone trying to find out
    // whether the key does anything at all. An invisible correct change and
    // a dead key look identical through a headset.
    //
    // TWO TARGETS, ONE CLUSTER, AND F12 CHOOSES WHICH -- not how.
    //
    // The previous version put the step size behind this key as well, so
    // switching target also switched behaviour and a session that was
    // nearly dialled in came apart on one press. The step is now the same
    // in both, and the key changes only what the six keys move:
    //
    //   PIVOT  -- the point the gun rotates about. The gun does NOT move.
    //   GUN    -- where the gun sits. The pivot stays put ON the gun, so
    //             the whole thing translates and its rotation still feels
    //             the same.
    //
    // They are complements: one holds the gun still and moves the pivot
    // within it, the other holds the pivot within the gun and moves both.
    // Dialling one cannot undo the other.
    constexpr float step = 0.5f;
    // ALWAYS the compensated pivot nudge. This flag used to select the
    // MODE as well as the step, so dropping to fine also silently switched
    // back to plain grip nudging and the gun started sliding again -- two
    // different behaviours behind one key, which is exactly what made the
    // presses feel inconsistent. It now changes the step and nothing else.
    auto nudge = [](float f, float r, float u) {
        switch (CurrentCalibrationTarget()) {
            case CalibrationTarget::GunPosition:
                // GUN: player-frame forward / LEFT / up, so a positive
                // "right" key has to subtract from left.
                NudgeHandPositionOffset(f, -r, u);
                break;
            case CalibrationTarget::Reticle:
                // PGUP/PGDN only -- the "up" argument here. Size is the
                // question; the reticle's position is the engine's and is
                // now preserved by the compensation rather than driven.
                if (u != 0.0f) NudgeHud2dPlacement(0.0f, 0.0f, u > 0.0f ? 1.25f : 1.0f / 1.25f);
                break;
            case CalibrationTarget::HudScale: {
                // SIZE ONLY THIS RUN, AND EVERY KEY DRIVES IT.
                //
                // The inset now goes through the engine's own safe-area
                // branch, which takes a single scalar fraction off all four
                // edges -- so there is no per-edge asymmetry to hang a
                // position control on. Rather than leave the four arrows
                // dead, they drive size too, at a finer step. A key that
                // does nothing is indistinguishable from a broken one, and
                // the wearer cannot read the log to tell the difference.
                //
                //   PGUP / UP     bigger      (0.02 less inset)
                //   PGDN / DOWN   smaller     (0.02 more inset)
                //   RIGHT / LEFT  bigger / smaller, 0.005 -- the fine pass
                constexpr float kCoarse = 0.02f;
                constexpr float kFine = 0.005f;
                float d = 0.0f;
                if (u != 0.0f) d = u > 0.0f ? -kCoarse : kCoarse;
                else if (f != 0.0f) d = f > 0.0f ? -kCoarse : kCoarse;
                else if (r != 0.0f) d = r > 0.0f ? -kFine : kFine;
                if (d != 0.0f) SetRuiInset(RuiInset() + d);
                if (!RuiLayerScaleOn()) ToggleRuiLayerScale();
                char line[280]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] HUD size: inset %.3f (rui_safeAreaFrac %.2f)\n",
                    static_cast<double>(RuiInset()), static_cast<double>(RuiInset() * 20.0f));
                Tf2VrLog(line);
                break;
            }
            case CalibrationTarget::HudLowerLeft: {
                // SIZE ONLY, and every key drives it -- the same reasoning
                // as HudScale above. The lever is a uniform scale about the
                // screen centre, so there is no axis to hang a separate
                // position control on, and leaving four of the six keys
                // dead would be worse than pointing them all at the size.
                //
                //   PGUP / UP     bigger, coarse
                //   PGDN / DOWN   smaller, coarse
                //   RIGHT / LEFT  bigger / smaller, fine
                int steps = 0;
                bool fine = false;
                if (u != 0.0f) steps = u > 0.0f ? 1 : -1;
                else if (f != 0.0f) steps = f > 0.0f ? 1 : -1;
                else if (r != 0.0f) { steps = r > 0.0f ? 1 : -1; fine = true; }
                if (steps) NudgeRuiLowerLeft(steps, fine);
                break;
            }
            case CalibrationTarget::HandAngle: {
                // WHERE THE GUN POINTS. A quarter degree a press: the
                // complaint was a gun a few degrees off, not tens.
                // LEFT/RIGHT is yaw, and positive yaw is LEFT in Source, so
                // the key pointing left points the gun left. UP/DOWN is
                // pitch, PGUP/PGDN roll.
                constexpr float kAngleStep = 0.25f;
                if (r != 0.0f) NudgeHandAngleOffsets(0.0f, r > 0.0f ? -kAngleStep : kAngleStep, 0.0f);
                else if (f != 0.0f) NudgeHandAngleOffsets(f > 0.0f ? -kAngleStep : kAngleStep, 0.0f, 0.0f);
                else if (u != 0.0f) NudgeHandAngleOffsets(0.0f, 0.0f, u > 0.0f ? kAngleStep : -kAngleStep);
                break;
            }
            case CalibrationTarget::WorldScale: {
                // TWO RELATED KNOBS, both answering "I am the wrong size".
                //
                // UP/DOWN move the EYE HEIGHT in game units. That is the one
                // the wearer's evidence asks for: they recentred, which puts
                // the eye exactly at the game's own camera, and still stood
                // at a character's chest. Where a chest sits relative to your
                // eye is geometry, not perceived scale, so the camera really
                // is low and only a height offset moves it.
                //
                // PGUP/PGDN change WORLD SCALE via the stereo separation.
                // That is a different complaint -- everything the wrong size
                // rather than the viewpoint in the wrong place -- and mixing
                // the two would let "make me taller" be answered by
                // falsifying the eye separation, which then breaks depth.
                // Repointed 2026-09-02 at `eye.raise`, which raises the eye
                // at the engine's own getter so the gun and the round come
                // with it. It used to nudge the render camera, which moved
                // the eye and left the shot origin behind -- the aim bug
                // this work removed. Same gesture, correct mechanism.
                if (u > 0.0f) SetEyeHookRaiseUnits(EyeHookRaiseConfigured() + 2.0f);
                else if (u < 0.0f) SetEyeHookRaiseUnits(EyeHookRaiseConfigured() - 2.0f);
                else if (f > 0.0f) NudgeHalfInterpupillary(1.05f);
                else if (f < 0.0f) NudgeHalfInterpupillary(1.0f / 1.05f);
                break;
            }
            case CalibrationTarget::HudAnchor: {
                // A choice, not a scale, so the keys pick rather than
                // nudge: UP/PGUP asks for the aim anchor, DOWN/PGDN for
                // the head. Pressing the same one twice is harmless, which
                // matters for a key found by feel.
                if (u > 0.0f || f > 0.0f) SetHudAnchorMode(1);
                else if (u < 0.0f || f < 0.0f) SetHudAnchorMode(0);
                break;
            }
            // EXPLICIT, and above the default, because Pivot shares that
            // default -- so without this Off would nudge the pivot, which
            // is the exact bug being fixed.
            case CalibrationTarget::Off:
                break;
            case CalibrationTarget::Pivot:
            default:
                NudgePivotOffset(f, r, u);
                break;
        }
    };
    if (ActionPressed(Action::GripForwardMore)) nudge(step, 0.0f, 0.0f);
    if (ActionPressed(Action::GripForwardLess)) nudge(-step, 0.0f, 0.0f);
    if (ActionPressed(Action::GripRightMore)) nudge(0.0f, step, 0.0f);
    if (ActionPressed(Action::GripRightLess)) nudge(0.0f, -step, 0.0f);
    if (ActionPressed(Action::GripUpMore)) nudge(0.0f, 0.0f, step);
    if (ActionPressed(Action::GripUpLess)) nudge(0.0f, 0.0f, -step);
    // THE NUMBER TO MINIMISE, instead of a feeling to chase.
    //
    // grip is the NEGATIVE of the model-space point that sits in the hand,
    // so |grip| is the lever arm the wrist swings the gun on, and zero means
    // the gun turns in place. Logged in BOTH targets, naming the live one,
    // so a glance at the log afterwards says which was being moved.
    {
        static std::uint64_t lastPivotLog = 0;
        const std::uint64_t nowTick = GetTickCount64();
        if (nowTick - lastPivotLog >= 1000 &&
            CurrentCalibrationTarget() == CalibrationTarget::Reticle) {
            lastPivotLog = nowTick;
            // The number to copy back, in the ini's own syntax. The scaled-
            // emit count lives on AdvanceCrosshairScale's own line so the
            // falsifier is beside the value rather than derived from it.
            float ax = 0.0f, ay = 0.0f, dy = 0.0f, dp = 0.0f;
            float hz = 1.0f, hx = 0.0f, hy = 0.0f;
            ReadReticleAnchor(&ax, &ay, &dy, &dp);
            ReadHud2dPlacement(&hx, &hy, &hz);
            char line[360]{};
            std::snprintf(line, sizeof(line),
                          "[TF2VR] CALIBRATING RETICLE: set hud2d.zoom = %.3f -- aim is "
                          "<yaw %+.2f, pitch %+.2f> off the RENDER camera, so the anchor is "
                          "pixel <%.0f, %.0f>; uploads touched=%llu\n",
                          static_cast<double>(hz), dy, dp, ax, ay,
                          Hud2dAdjustedUploadCount());
            Tf2VrLog(line);
        } else if (nowTick - lastPivotLog >= 1000 &&
                   CurrentCalibrationTarget() != CalibrationTarget::Off) {
            lastPivotLog = nowTick;
            // ALL SIX NUMBERS, IN INI SYNTAX, EVERY SECOND.
            //
            // This used to print the grip alone. GUN POSITION mode nudges
            // hand.off_*, which was in NO log line at all -- so a session
            // spent dialling the gun in was unrecoverable once the game
            // closed, and that is exactly what happened: the grip came back
            // out of the log and the offset was simply lost. A calibration
            // that only exists in memory is a calibration that will be done
            // twice.
            //
            // Printed as the ini lines themselves so persisting them is
            // mechanical rather than a translation step, and printed in
            // BOTH modes because a pivot nudge moves the grip and the
            // offset together.
            float gf = 0.0f, gr = 0.0f, gu = 0.0f;
            float of = 0.0f, ol = 0.0f, ou = 0.0f;
            float ap = 0.0f, ay = 0.0f, ar = 0.0f;
            ReadGripOffset(&gf, &gr, &gu);
            ReadHandPositionOffset(&of, &ol, &ou);
            // The three ANGLES too. Same lesson as the position offsets one
            // session earlier: a number that is dialled in but never
            // printed is a number that gets dialled in twice.
            ReadHandCalibration(&ap, &ay, &ar, nullptr);
            const float lever = std::sqrt(gf * gf + gr * gr + gu * gu);
            char line[600]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] CALIBRATING %s: lever arm %.2f units | set hand.grip_fwd = %.2f / "
                "set hand.grip_right = %.2f / set hand.grip_up = %.2f / set hand.off_fwd = "
                "%.2f / set hand.off_left = %.2f / set hand.off_up = %.2f / set "
                "hand.off_pitch = %.2f / set hand.off_yaw = %.2f / set hand.off_roll = %.2f\n",
                PivotCalibrationMode() ? "GUN POSITION" : "PIVOT",
                lever, gf, gr, gu, of, ol, ou, ap, ay, ar);
            Tf2VrLog(line);
            // THE REFERENCE, so a calibration done on ANY weapon is complete:
            // grip + the R_HAND of the weapon it was done on (ads_probe.h).
            int refClass = -1; float ref[3]{};
            if (TryGetLatchedRightHand(&refClass, ref)) {
                std::snprintf(line, sizeof(line),
                    "[TF2VR] CALIBRATING reference (weapon class %d in hand): set grip.ref_fwd = "
                    "%.2f / set grip.ref_right = %.2f / set grip.ref_up = %.2f -- copy these THREE "
                    "beside the six above; they are the R_HAND of the weapon this grip was dialled on.\n",
                    refClass, ref[0], ref[1], ref[2]);
            } else {
                std::snprintf(line, sizeof(line),
                    "[TF2VR] CALIBRATING reference: the weapon in hand has NOT latched its R_HAND yet, "
                    "so grip.ref_* cannot be printed; hold it still a second and press again.\n");
            }
            Tf2VrLog(line);
        }
    }
    AdvanceViewmodelVisibilityProbe();
    PluginCost::FrameSeg(15);
    // PLAN-CURRENT F2. Same key arms and disarms; it also closes itself on
    // its own clock, so a run that is never disarmed still ends and still
    // reports.
    AdvanceViewmodelPlacementPin();
    PluginCost::FrameSeg(16);
    if (ActionPressed(Action::ExperimentStereoSlot)) RequestStereoSlotExperiment();
    // PLAN-TITAN P0-b's falsifier. Does nothing at all unless the ini set
    // crash.selftest = 1, and says so in the log when it does nothing, so a
    // silent key and a disarmed key are not the same result.
    SceneReentryWatchdogTick();
    PluginCost::FrameSeg(17);
    if (ActionPressed(Action::ToggleRtvSubstitution)) ToggleRtvSubstitute();
    if (ActionPressed(Action::ToggleEyeRaise)) ToggleEyeHookRaise();
    if (ActionPressed(Action::TogglePerWeaponGrip)) TogglePerWeaponGrip();
    if (ActionPressed(Action::ToggleLeanDetach)) {
        SetViewmodelDetachLean(ViewmodelDetachLeanStrength() > 0.001f ? 0.0f : 1.0f);
    }
    // SLOT C REASSIGNED. The eye-offset question is settled (mode 2 works), so
    // the slot carries this build's live question instead of accumulating a
    // new chord for it.
    if (ActionPressed(Action::ToggleBatch2EyeOffset)) ToggleRtvReadSubstitute();
    // SLOT C REASSIGNED AGAIN, to T-A. The read-side substitution above keeps
    // its action and its handler and simply has no chord this build.
// SLOT C REASSIGNED to the symmetric pair. The temporal lever it replaces
    // is spent: T-A refuted temporal reuse and the action keeps its handler.
    if (ActionPressed(Action::ToggleTemporalCurrentFrameOnly)) ToggleSceneDrawEpilogue();
    // SLOT D, N2's positive control. Toggling this rather than editing the
    // ini is what lets one session produce an armed burst and a zero-offset
    // burst of the same scene; a control taken in a different session is a
    // control taken of a different scene.
    if (ActionPressed(Action::ToggleEngineCameraWrite)) ToggleEngineCameraWrite();
    if (ActionPressed(Action::CrashRecorderSelfTest)) TriggerCrashRecorderSelfTest();
    // RESET HEIGHT, by hand. See camera_hook.h for why this had to exist.
    if (ActionPressed(Action::RecentreView)) {
        Tf2VrLog("[TF2VR] RESET HEIGHT: sit the way you want to play, then this makes your "
                 "current head pose the game's eye height and forward direction.\n");
        RequestHeadTrackingRecentre();
    }
    // AND ONCE, AUTOMATICALLY, WHEN THE WORLD IS ACTUALLY UP.
    //
    // The reference is otherwise captured when head tracking arms, which with
    // autoarm is mid-LOADING-SCREEN -- so the wearer's height is sampled while
    // they are still settling, adjusting the headset, or sitting up to see the
    // monitor. That single bad sample then IS the eye height for the whole
    // session, and there was no way to correct it.
    //
    // Deliberately a few seconds AFTER the world appears rather than the
    // instant it does: the point is to sample a wearer who is sitting the way
    // they intend to play, and the moment a level finishes loading is the
    // moment they are least likely to be.
    if (g_recentreOnSpawn && GameIsRenderingAWorld() && IsHeadTrackingArmed()) {
        if (++g_spawnRecentreTicks == 300) {
            Tf2VrLog("[TF2VR] RESET HEIGHT (automatic, once): the world is up and settled, so "
                     "your current head pose becomes the game's eye height. This is what stops "
                     "a height sampled during the loading screen from following you all "
                     "session. LEADER then TAB does it again whenever you want.\n");
            RequestHeadTrackingRecentre();
        }
    }

    // Covers a graphics module that loaded before this plugin.
    if (GetModuleHandleA("materialsystem_dx11.dll")) InstallPresentHook();
    PluginCost::Scope ensuresScope(PluginCost::kEnsures);
    EnsureCameraHookInstalled();
    EnsureWeaponSettingsHookInstalled();
    // Weapons are parsed at level load, so an object captured after
    // suppression was armed keeps its sway until something reapplies it.
    // Silent unless a map change actually brought new weapons in.
    TickWeaponSwaySuppression();
    EnsureClientViewBuildHookInstalled();
    EnsureClientViewCallsiteHookInstalled();
    TryLogRequestedCameraSnapshot();
    AdvanceCameraAngleTrace();
    AdvanceBoundedYawWriteTest();
    AdvanceBoundedSourceYawWriteTest();
    // The load watch is registered once at plugin init, not here -- by tick
    // time inputsystem.dll has long since loaded and there is nothing left
    // to be notified about. This retry only covers the case where the watch
    // could not be registered at all.
    ensuresScope.Stop();
    // One status line a second while capturing, so a quiet log still says
    // WHY it is quiet: hooked but never called, called but no HID reports,
    // or never hooked at all are three different answers that look
    // identical without counters.
    // Its OWN gate, not nested inside the raw-input one: those two probes
    // are armed independently, and hanging one status line off the other's
    // flag would make turning raw-input capture off silently stop this
    // reporting too.
    WatchRawControllerButtonEdges();
    WatchControllerModeObject();
    HoldGameControllerFlagDown();
    LogStageTimeline();
    LogXInputPadCounters();
    PluginCost::FrameSeg(18);
    AdvanceSyntheticPose();
    AdvanceHeadTracking();
    PluginCost::FrameSeg(19);
    TryApplyRequestedCameraNudge();
    AdvanceRenderTrace();
    AdvanceClientViewBuildTrace();
    AdvanceJtProbe(GameIsRenderingAWorld());
    AdvanceSceneReentry(GameIsRenderingAWorld());
    AdvanceVerifiedGameDrawFunctionEntryTrace();
    AdvanceCameraUpdateTrace();
    PluginCost::FrameSeg(11);
}

void SetAutoArmTarget(int stage) {
    // THE CLAMP MUST TRACK THE LADDER. It said 4 while the ladder had grown a
    // stage 5, so "autoarm = 5" in the ini was silently clamped back to 4 and
    // the weapon-pin stage never ran -- the wearer still had to press F3, with
    // nothing in the log to say why. A ceiling that outlives the thing it is
    // bounding is worse than no ceiling.
    g_autoArmTarget = stage < 0 ? 0 : (stage > 5 ? 5 : stage);
    static const char* const kNames[] = {
        "[TF2VR] auto-arm OFF; every stage is a keypress.\n",
        "[TF2VR] auto-arm: OpenXR only.\n",
        "[TF2VR] auto-arm: OpenXR + head tracking (with viewmodel correction and sway).\n",
        "[TF2VR] auto-arm: OpenXR + head tracking + alternate-frame stereo.\n",
        "[TF2VR] auto-arm: the full stack, up to and including the projection layer.\n",
        "[TF2VR] auto-arm: the full stack, plus the hand-driven weapon pin (no F3 needed).\n",
    };
    // Bounds-checked against the array rather than against a literal, so the
    // next stage added cannot walk off the end the way the clamp above just did.
    static_assert(sizeof(kNames) / sizeof(kNames[0]) == 6, "one name per auto-arm stage");
    if (g_autoArmTarget >= 0 && g_autoArmTarget < static_cast<int>(sizeof(kNames) / sizeof(kNames[0]))) {
        Tf2VrLog(kNames[g_autoArmTarget]);
    }
}

void SetViewmodelCorrectionWanted(bool wanted) {
    g_viewmodelCorrectionWanted = wanted;
    Tf2VrLog(wanted
        ? "[TF2VR] viewmodel.correction = 1: auto-arm stage 2 arms the viewmodel correction.\n"
        : "[TF2VR] viewmodel.correction = 0: auto-arm stage 2 does NOT arm the viewmodel "
          "correction; the weapon family stays on the engine's own camera (INSERT still "
          "toggles it by hand).\n");
}
void SetAutoArmRequireLiveView(bool required) {
    g_autoArmRequireLiveView = required;
    Tf2VrLog(required
        ? "[TF2VR] auto-arm HOLDS until the engine commits real view angles, so nothing arms "
          "during the loading screen and the recentre reference is taken with the player actually "
          "in the world.\n"
        : "[TF2VR] auto-arm does NOT wait for live view angles; it arms as soon as a level starts "
          "loading, which is the pre-1.6-E behaviour.\n");
}


extern "C" __declspec(dllexport) void* CreateInterface(const char* name, int*) {
    if (!name) return nullptr;
    if (std::strcmp(name, "PluginId001") == 0) return static_cast<IPluginId*>(&g_plugin);
    if (std::strcmp(name, "PluginCallbacks001") == 0) return static_cast<IPluginCallbacks*>(&g_plugin);
    return nullptr;
}
// game.fov_scale. 0 leaves the game's own value alone.
float GameFovScale() { return g_gameFovScale.load(std::memory_order_acquire); }

void SetGameFovScale(float scale) {
    g_gameFovScale.store(scale, std::memory_order_release);
    char line[240]{};
    std::snprintf(line, sizeof(line),
        scale > 0.01f
            ? "[TF2VR] game.fov_scale = %.3f: cl_fovScale will be held here. It sets the game's own "
              "VERTICAL FOV, and horizontal follows the buffer aspect, so 90 deg into a 1.4283 "
              "buffer is 110 across -- the headset exactly.\n"
            : "[TF2VR] game.fov_scale = 0: the game keeps its own cl_fovScale.\n",
        scale);
    // FORCED: this is the number that decides how zoomed the view is, and a
    // report of a view stuck fully zoomed in is unanswerable without it.
    Tf2VrLogWrite(line, true);
}


void SetRecentreOnSpawn(bool enabled) {
    g_recentreOnSpawn = enabled;
    Tf2VrLog(enabled
        ? "[TF2VR] headtracking.recentre_on_spawn = 1: your height is re-sampled once, a few "
          "seconds after the world is up. Without it the height is whatever it was during the "
          "loading screen, for the whole session.\n"
        : "[TF2VR] headtracking.recentre_on_spawn = 0: the height is taken when head tracking arms "
          "and never re-sampled. LEADER then TAB still resets it by hand.\n");
}
