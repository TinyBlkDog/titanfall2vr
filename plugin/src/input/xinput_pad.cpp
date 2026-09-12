#include "xinput_pad.h"

#include "ads_lock.h"
#include "dpad_gesture.h"

#include "diagnostics.h"
#include "xr_input.h"
#include "vr_input.h"
#include "xr_input.h"
#include "menu_overlay.h"
#include "rui_probe.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// ---------------------------------------------------------------------------
// XInput types, declared here rather than included.
//
// The plugin links nothing new: these are the documented layouts, and the only
// one whose exact field offsets matter is XINPUT_CAPABILITIES, because
// InitializeXDevice reads caps.SubType at byte +1 (`cmp byte [rsp+0x21], 1`
// against a buffer at [rsp+0x20]). Type at +0, SubType at +1 is what that
// instruction pair means, and it is the whole reason the struct is written out
// instead of trusted.
// ---------------------------------------------------------------------------
struct XInputGamepad {
    WORD buttons;
    BYTE leftTrigger;
    BYTE rightTrigger;
    SHORT thumbLX;
    SHORT thumbLY;
    SHORT thumbRX;
    SHORT thumbRY;
};
struct XInputState {
    DWORD packetNumber;
    XInputGamepad gamepad;
};
struct XInputVibration {
    WORD leftMotorSpeed;
    WORD rightMotorSpeed;
};
struct XInputCapabilities {
    BYTE type;
    BYTE subType;
    WORD flags;
    XInputGamepad gamepad;
    XInputVibration vibration;
};
static_assert(sizeof(XInputState) == 16, "PollXDevices strides this by 16");
static_assert(offsetof(XInputCapabilities, subType) == 1, "InitializeXDevice reads caps+1");

constexpr BYTE kDevTypeGamepad = 0x01;
constexpr BYTE kDevSubTypeGamepad = 0x01;

// Y's long press. The threshold is live from the ini so it can be dialled by
// feel without a rebuild; 0 disables the mechanism and returns Y to the press
// edge. The tap hold is how long the synthetic Y is asserted after release, and
// it exists because the game samples the pad on its own clock -- a single-frame
// pulse can land between two polls and be missed entirely.
constexpr unsigned kYTapHoldMs = 60;
std::atomic<unsigned> g_yLongPressMs{350};
// FRESHNESS, so a run that reports "it did nothing" says WHICH half failed:
// no long presses at all means the hold never reached the threshold; long
// presses with no visible objectives means BACK is not the button after all.
std::atomic_uint64_t g_yLongPresses{0};
std::atomic_uint64_t g_yTaps{0};
unsigned YLongPressMs() { return g_yLongPressMs.load(std::memory_order_relaxed); }

constexpr WORD kPadDpadUp = 0x0001;
constexpr WORD kPadDpadDown = 0x0002;
constexpr WORD kPadDpadLeft = 0x0004;
constexpr WORD kPadDpadRight = 0x0008;
constexpr WORD kPadStart = 0x0010;
constexpr WORD kPadBack = 0x0020;
constexpr WORD kPadLeftThumb = 0x0040;
constexpr WORD kPadRightThumb = 0x0080;
constexpr WORD kPadLeftShoulder = 0x0100;
constexpr WORD kPadRightShoulder = 0x0200;
constexpr WORD kPadA = 0x1000;
constexpr WORD kPadB = 0x2000;
constexpr WORD kPadX = 0x4000;
constexpr WORD kPadY = 0x8000;

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XInputState*);
using XInputSetStateFn = DWORD(WINAPI*)(DWORD, XInputVibration*);
using XInputGetCapabilitiesFn = DWORD(WINAPI*)(DWORD, DWORD, XInputCapabilities*);
using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);

constexpr DWORD kErrorSuccess = 0;
constexpr DWORD kErrorDeviceNotConnected = 1167;   // 0x48F, the value at 0xDEEA

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
std::atomic_bool g_enabled = false;
bool g_installed = false;
bool g_notificationRegistered = false;
PVOID g_notificationCookie = nullptr;

void** g_getProcSlot = nullptr;
GetProcAddressFn g_originalGetProcAddress = nullptr;
XInputGetStateFn g_realGetState = nullptr;
XInputSetStateFn g_realSetState = nullptr;
// Bounded read-only census of the motor pair; see HookedSetState.
int g_hapticLogBudget = 30;
XInputGetCapabilitiesFn g_realGetCapabilities = nullptr;

std::atomic_uint64_t g_capsCalls{0};
std::atomic_uint64_t g_capsSynthesised{0};
std::atomic_uint64_t g_capsRealPad{0};
std::atomic_uint64_t g_stateCalls{0};
std::atomic_uint64_t g_stateSynthesised{0};
std::atomic_uint64_t g_stateRealPad{0};
std::atomic_uint64_t g_setStateCalls{0};
std::atomic_uint32_t g_packet{0};
std::atomic_int g_servedNames{0};

// ---------------------------------------------------------------------------
// The IAT walker and slot swap, the same shape hid_capture.cpp already uses on
// this module. Searched across ALL descriptors rather than one named DLL: the
// same function can arrive through an api-set alias, and matching on the DLL
// name is a false absence waiting to happen.
// ---------------------------------------------------------------------------
void** FindImportSlot(HMODULE module, const char* functionName) {
    auto* base = reinterpret_cast<std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return nullptr;
    const auto* descriptor =
        reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) continue;
        const auto* nameThunk =
            reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk);
        auto* addressThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        for (; nameThunk->u1.AddressOfData; ++nameThunk, ++addressThunk) {
            if (IMAGE_SNAP_BY_ORDINAL64(nameThunk->u1.Ordinal)) continue;
            const auto* import =
                reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + nameThunk->u1.AddressOfData);
            if (std::strcmp(import->Name, functionName) == 0) {
                return reinterpret_cast<void**>(&addressThunk->u1.Function);
            }
        }
    }
    return nullptr;
}

bool SwapSlot(void** slot, void* replacement, void** originalOut) {
    if (!slot) return false;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *originalOut = *slot;
    *slot = replacement;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// ---------------------------------------------------------------------------
// Composing the VR controllers into a pad
// ---------------------------------------------------------------------------

// THE WHOLE POINT OF THIS ROUTE, IN ONE FUNCTION.
//
// The kbutton_t path this stands beside converts each stick to a digital latch
// through a fixed threshold, so anything past the threshold is full speed BY
// CONSTRUCTION -- the wearer's "all or nothing like a joypad, not an analog
// stick". A kbutton carries no magnitude and no tuning of the threshold can
// give it one. Here the deflection survives as a number, and the game applies
// its own deadzones, curves and turn rates to it, which is what makes this the
// root fix rather than another approximation of one.
SHORT ToAxis(float value) {
    if (value > 1.0f) value = 1.0f;
    if (value < -1.0f) value = -1.0f;
    // 32767, not 32768: the positive and negative extremes must be symmetric or
    // a full push one way reads slightly harder than the other.
    const float scaled = value * 32767.0f;
    return static_cast<SHORT>(scaled < 0.0f ? scaled - 0.5f : scaled + 0.5f);
}

BYTE ToTrigger(float value) {
    if (value > 1.0f) value = 1.0f;
    if (value < 0.0f) value = 0.0f;
    return static_cast<BYTE>(value * 255.0f + 0.5f);
}

// Mapped to match the kbutton bindings already in vr_input.cpp, so the two
// cannot disagree about what a button means while both exist. The grips carry
// the offhands, which on a pad are the shoulders.
void ComposeGamepad(XInputGamepad& pad) {
    // WHILE OUR CONFIG PANEL IS OPEN THE PAD REPORTS NOTHING AT ALL, and this
    // returns BEFORE the sticks are written rather than after.
    //
    // It is the wearer's own requirement -- "STOP capturing any controller input
    // anywhere outside of the config menu" -- and the 2026-08-24 run showed why
    // gating only the face buttons was not enough: the sticks were still
    // forwarded, so they drove the game's menu underneath the panel while the
    // wearer was trying to move the panel's own focus.
    //
    // Returning leaves `pad` exactly as the real XInput call left it, which for
    // a wearer with no physical pad is centred and empty.
    if (IsMenuOpen()) return;
    const float lx = g_controllerThumbstick[0][0];
    const float ly = g_controllerThumbstick[0][1];
    const float rx = g_controllerThumbstick[1][0];
    const float ry = g_controllerThumbstick[1][1];

    // Deflection is taken as the LARGER of ours and a real pad's, per axis, so
    // a real stick already pushed is never overridden by our centred one.
    if (std::abs(lx) > std::abs(static_cast<float>(pad.thumbLX) / 32767.0f)) pad.thumbLX = ToAxis(lx);
    if (std::abs(ly) > std::abs(static_cast<float>(pad.thumbLY) / 32767.0f)) pad.thumbLY = ToAxis(ly);
    // SMOOTH TURNING LIVES HERE, AND IT IS ANALOGUE.
    //
    // The pad is the only one of the two routes that can express HOW FAR the
    // stick moved; +left/+right are digital. vr_input therefore leaves the turn
    // kbuttons alone in smooth mode and this owns it outright.
    //
    // The deadzone is subtracted and the remainder RESCALED to the full range, so
    // a push just past the deadzone gives a slow turn and a full push gives the
    // configured maximum. Passing the raw deflection through instead would waste
    // the first third of the stick's travel and then jump.
    //
    // In SNAP mode this sends nothing. A snap is one instant write, and a pad
    // stick turning smoothly underneath it would make it a sweep again -- which
    // is the whole thing snap turning exists to avoid.
    float turnScaledRx = 0.0f;
    if (TurnMode() == 0) {
        const float deadzone = TurnDeadzone();
        const float magnitude = rx < 0.0f ? -rx : rx;
        if (magnitude > deadzone) {
            const float span = 1.0f - deadzone;
            float past = span > 0.01f ? (magnitude - deadzone) / span : 1.0f;
            if (past > 1.0f) past = 1.0f;
            // The curve is applied to the NORMALISED travel, so the maximum is
            // unchanged and only the shape below it moves.
            past = std::pow(past, TurnCurve());
            turnScaledRx = (rx < 0.0f ? -past : past) * TurnStickScale();
            // ads.lock: the flat game's muted look sensitivity, on the stick.
            //
            // "We need to HEAVILY dampen movement at that point." In VR the
            // stick turns the body and the head turns the view; damping the
            // head is neither possible nor wanted, so the stick is the whole of
            // it -- and it is exactly what flat mutes. One multiply, applied
            // AFTER the curve and the scale so it composes with the turning
            // work rather than replacing any of it.
            if (AdsLockEngaged()) {
                turnScaledRx *= AdsTurnScale();
                NoteAdsLockTurnDamped();
            }
        }
    }
    if (std::abs(turnScaledRx) > std::abs(static_cast<float>(pad.thumbRX) / 32767.0f)) pad.thumbRX = ToAxis(turnScaledRx);
    // THE RIGHT STICK'S VERTICAL AXIS NEVER DRIVES LOOK. Not conditionally, not
    // outside a deadzone -- never.
    //
    // The wearer's rule, and it is the correct one: "in no scenario should
    // moving the right stick up/down alter the direction we are facing." In VR
    // the headset owns pitch outright -- the rendered camera's pitch is
    // overwritten every frame with the head's own absolute pitch -- so a stick
    // that pitches the view cannot move the picture at all. All it can do is tip
    // the BODY invisibly, which is exactly how it went unnoticed while it
    // ratcheted to the +/-89 clamp and broke ADS: crouch WAS this axis pushed
    // down, and the same deflection was passed through raw as look.
    //
    // A first version of this gated the axis on the crouch gesture. That was too
    // narrow -- it left the axis live wherever the predicate happened not to
    // fire, and the rule has no exceptions in it. This is the rule as stated.
    //
    // The axis still answers the game's two-option prompts through the d-pad
    // below. That is a discrete press, not a look, and it is the one job the
    // wearer wants it to keep.
    //
    // A REAL pad's own stick is untouched. The rule is about the right
    // CONTROLLER; someone playing on a physical gamepad has deliberately pushed
    // that stick and is owed the look they asked for.
    const float absRxGesture = rx < 0.0f ? -rx : rx;
    const float absRyGesture = ry < 0.0f ? -ry : ry;
    const bool verticalGesture =
        absRyGesture >= StickCrossRatio() * absRxGesture && absRyGesture > JumpCrouchDeadzone();

    const BYTE lt = ToTrigger(g_controllerTrigger[0]);
    const BYTE rt = ToTrigger(g_controllerTrigger[1]);
    if (lt > pad.leftTrigger) pad.leftTrigger = lt;
    if (rt > pad.rightTrigger) pad.rightTrigger = rt;

    WORD buttons = 0;
    const std::uint8_t lb = g_controllerButtons[0];
    const std::uint8_t rb = g_controllerButtons[1];
    if (rb & kControllerPrimaryButton) buttons |= kPadA;

    // A's EDGES ARE THE TIMELINE ANCHORS for the STAGE probe in plugin.cpp.
    //
    // Both continue panels are dismissed with A, so logging the edge here dates
    // "the wearer is through the loading screen" without asking them to press
    // anything extra or to remember when it happened. Edge only: a held button
    // would write this line 84 times a second.
    {
        static bool held = false;
        const bool down = (rb & kControllerPrimaryButton) != 0;
        if (down && !held) {
            static int presses = 0;
            char line[140];
            std::snprintf(line, sizeof(line),
                          "[TF2VR] STAGE anchor: pad A pressed (#%d).\n", ++presses);
            Tf2VrLog(line);
            // AND DUMP WHAT IS ON SCREEN AT THIS INSTANT.
            //
            // The wearer has to press A twice to enter a level and asks why --
            // "maybe there are two screens being rendered" -- and refuses a
            // workaround that sends a second event. Fair, and the census is
            // the instrument that can answer it without one.
            //
            // On its own five-second timer the census mixes the panel in with
            // the gameplay that follows, which is exactly what the last
            // session's dump did. Dumping ON THE PRESS gives one labelled
            // snapshot per press, so the two can be COMPARED: the same
            // descriptor set twice means one screen drawn twice, and different
            // sets mean two different surfaces. Read-only either way.
            RequestRuiCensusSnapshot(presses);
        }
        held = down;
    }
    // THE MENU/GAMEPLAY SPLIT, which the note below has been asking for.
    //
    // That note says these are "not visibly broken because both routes happen to
    // land on the same action -- but that is luck, not design." The luck has now
    // run out twice in one run:
    //
    //   B  vr_input writes +reload for it; this reports kPadB, and Titanfall 2's
    //      own pad layout binds B to CROUCH. One press, two routes, two
    //      different actions -- the wearer reloads the pistol and crouches. Not
    //      a coincidence and not intermittent: "This is consistent."
    //
    //   L3/R3  vr_input suppresses sprint and melee for the whole summon chord,
    //      which is useless while this reports the same clicks independently.
    //      That is why the chord "clicked one or the other stick" about half the
    //      time -- the kbutton route was correctly suppressed and this one fired
    //      anyway.
    //
    // So the face buttons and thumb clicks are reported to the pad ONLY when a
    // menu is actually up, which is the one case the note says they are needed
    // for. During gameplay the kbutton path owns them outright, and it is the
    // route with the derived, checked assignment.

    // EVERY FACE BUTTON, ALWAYS. B IS B.
    //
    // This used to withhold B during gameplay and forward it only when a menu
    // was believed open, because the kbutton route wrote crouch for the same
    // press and one press did two things. That referee rule -- and the
    // "is a menu up" predicate it needed -- existed ONLY because two routes
    // delivered the same button. With the kbutton route retired there is one
    // owner, so the game's own controller bindings decide what B means, and
    // they already know it is crouch in gameplay and BACK in a menu.
    //
    // The bug that predicate caused is the reason it is gone: the wearer's
    // d-pad-right screen is a game screen the predicate could not see, so B
    // was withheld there and the screen could not be backed out of while A
    // still selected. Every mod we checked (Halo MCC, BioShock, Cyberpunk)
    // presents ONE synthetic pad and lets the game's own binding and menu code
    // do the rest; this is that.
    // X IS WITHHELD WHILE THE VIEW GESTURE IS TRUE, and this is the one place
    // that can do it.
    //
    // The View gesture IS the left primary held under the d-pad modifier. With
    // this line unconditional, the frame the gesture finally fires would send
    // BACK *and* X -- the objectives display plus a spurious use/reload. That
    // has never been observed because the modifier has never once been
    // recognised in a logged session (`head gesture seen=0`, `thumbrest
    // seen=0`, 20,038 frames), which is exactly why it survived: a defect on a
    // path nothing reaches looks identical to no defect at all.
    //
    // Found by reading, recorded in KNOWN-ISSUES, and fixed here rather than
    // shipped -- CONTROLS.md documents this gesture as sending the View button,
    // and it must not also reload.
    if ((lb & kControllerPrimaryButton) && !DpadGestureViewButton()) buttons |= kPadX;
    if (rb & kControllerSecondaryButton) buttons |= kPadB;

    // ---- Y: TAP CHANGES WEAPON, HOLD IS THE VIEW BUTTON --------------------
    //
    // The waypoint markers fade a few seconds after a level loads, and the
    // control that brings them back is the VIEW button. Settled from the
    // wearer's own settings.cfg rather than guessed:
    //
    //     bind "BACK"  "+showscores"          <- this, XINPUT_GAMEPAD_BACK
    //     bind "START" "ingamemenu_activate"  <- the pause menu, already sent
    //
    // Every button on both controllers is already spoken for, and the existing
    // route to BACK is the d-pad modifier gesture -- which needs either a
    // capacitive thumbrest or the left controller raised beside the head. THE
    // WEARER'S HEADSET IS PLAY FOR DREAM MR, WHOSE RIGHT CONTROLLER HAS NO
    // THUMBREST AT ALL, so on their hardware that route reduces to the head
    // gesture, and the H3 log confirms the modifier was never once recognised
    // in 12,332 frames. So BACK has never been reachable in play.
    //
    // THE WEARER'S OWN INSTRUCTION, 2026-09-05, and its hard constraint:
    // put BACK on a long press of Y -- "If that is going to fire a Y normal
    // press as well, DON'T DO IT."
    //
    // Y is weapon change, so honouring that constraint means the tap cannot be
    // emitted on the PRESS edge: nothing yet knows whether the press will
    // become a hold. So the tap is emitted on RELEASE, and the two outcomes are
    // mutually exclusive by construction rather than by ordering:
    //
    //   held < kYLongPressMs, then released -> a Y pulse, no BACK
    //   held >= kYLongPressMs               -> BACK while held, NEVER a Y
    //
    // THE COST, STATED PLAINLY: a weapon change now lands when Y is RELEASED
    // instead of when it is pressed. For a quick tap that delay is the length
    // of the tap itself, not the threshold -- press and let go and it fires at
    // once. This is the project's "never degrade a gameplay control for a UI
    // gesture" rule being deliberately spent, by the wearer's explicit choice,
    // on the smallest degradation that satisfies their constraint. If weapon
    // switching ever feels late, `input.y_longpress_ms = 0` disables the whole
    // mechanism and returns Y to firing on the press edge.
    //
    // ONE CONSUMER, CHECKED. `vr_input.cpp` also maps the left secondary to
    // WeaponCycle, but only through the kbutton route, and `Apply()` returns
    // early while `g_kbuttonsEnabled` is false -- which is its default and is
    // not overridden in the ini. Suppressing Y here therefore suppresses it
    // everywhere. Had that route been live, this change would have sent BACK
    // and still changed weapon, which is exactly what the wearer forbade.
    {
        const unsigned longMs = YLongPressMs();
        const bool yDown = (lb & kControllerSecondaryButton) != 0;
        static bool wasDown = false;
        static ULONGLONG downAt = 0;
        static bool longFired = false;
        static ULONGLONG tapUntil = 0;
        const ULONGLONG now = GetTickCount64();

        if (longMs == 0) {
            // Disabled: the original press-edge behaviour, nothing withheld.
            if (yDown) buttons |= kPadY;
            wasDown = yDown;
            longFired = false;
            tapUntil = 0;
        } else {
            if (yDown && !wasDown) { downAt = now; longFired = false; }
            if (yDown && !longFired && now - downAt >= longMs) {
                longFired = true;
                ++g_yLongPresses;
            }
            // Released without reaching the threshold: it was a tap, so send Y
            // now. A short HOLD rather than a single frame, because the game
            // samples the pad on its own clock and a one-frame blip can fall
            // between two polls.
            if (!yDown && wasDown && !longFired) {
                tapUntil = now + kYTapHoldMs;
                ++g_yTaps;
            }
            wasDown = yDown;

            if (longFired && yDown) buttons |= kPadBack;
            if (now < tapUntil) buttons |= kPadY;
        }
    }

    // THE GRIPS ARE THE SHOULDERS, and this is the one control the kbutton
    // route uniquely owned. It wrote +offhand0 / +offhand1 directly; the pad
    // equivalent is LB / RB, which is where the game's own controller layout
    // puts the same two abilities.
    //
    // The grenade is the RIGHT grip on both routes, so the hands map to the
    // shoulders the same way here as they did there.
    {
        // HYSTERESIS, not a bare threshold. The grips are analogue and a hand
        // resting near the line would chatter the ability at the poll rate --
        // the same reason the retired kbutton route latched them, with the same
        // release fraction, so the feel of a squeeze does not change with the
        // route.
        constexpr float kGripOn = 0.6f;
        constexpr float kGripOff = 0.45f;
        static bool gripHeld[kHandCount] = {false, false};
        for (int hand = 0; hand < kHandCount; ++hand) {
            const float squeeze = g_controllerSqueeze[hand];
            gripHeld[hand] = gripHeld[hand] ? squeeze > kGripOff : squeeze >= kGripOn;
        }
        // The commands are fixed -- ordnance IS the grenade, tactical IS the
        // ability -- and so is the hand: the RIGHT grip is the right shoulder,
        // which is the grenade on the game's own pad layout. Was selectable via
        // input.grenade_hand, removed because the game already lets a player
        // rebind its own shoulders and this mod feeds them through unchanged.
        if (gripHeld[1]) buttons |= kPadRightShoulder;
        if (gripHeld[0]) buttons |= kPadLeftShoulder;
    }

    // START, replacing the injected ESC KEYSTROKE.
    //
    // The menu button used to open the game's menu by sending a real ESC
    // through SendInput. That is the one thing the reference mods treat as a
    // last resort -- Cyberpunk's own note says to use a key the game binds to
    // NOTHING, and ESC is a key the game very much binds. A synthetic
    // keystroke also tells the game the player just used a keyboard, which is
    // the leading explanation for the wearer's HUD ability reading UNBOUND and
    // its button going dead after a menu: the game switches to keyboard
    // bindings for an ability that only has a controller one.
    //
    // START is what a pad sends for that, the game already binds it, and it
    // cannot be mistaken for a keyboard.
    if (g_controllerMenu) buttons |= kPadStart;
    // THE RIGHT STICK'S VERTICAL AXIS DRIVES THE D-PAD.
    //
    // The game's two-option prompts are answered with d-pad up and down, and
    // there was no controller input left to give them -- every button is spoken
    // for. The right stick's vertical axis was jump, which A already does, so it
    // is spent here instead.
    //
    // DOWN IS DOUBLE-MAPPED on purpose: d-pad down here AND crouch on the
    // kbutton route. A prompt and a crouch never want the stick at the same
    // moment, and crouch is worth keeping where the thumb already expects it.
    //
    // Gated on the same vertical-dominance rule and the same deadzone setting
    // the kbutton route uses, so a diagonal meant as a turn cannot answer a
    // prompt, and the two halves cannot disagree about what counts as pushed.
    // THE SAME PREDICATE THE LOOK AXIS USED ABOVE, not a second copy of it.
    // Two spellings of one rule are two things that can drift, and this one
    // decides both whether the stick crouches and whether it looks.
    {
        if (verticalGesture) {
            buttons |= (ry > 0.0f) ? kPadDpadUp : kPadDpadDown;
        }
    }

    // THE FULL D-PAD AND THE VIEW BUTTON, from the left stick under a modifier.
    //
    // Titan mode uses all four directions and the View button shows the next
    // objective; the right stick's vertical above reaches only two of the five,
    // and it stays exactly as it is -- it is a working control the wearer knows,
    // and this adds to it rather than replacing it. Up and down are therefore
    // reachable two ways, which is the harmless kind of double-routing: both
    // land on the same d-pad bit.
    //
    // The left stick's own axes are ZEROED while the gesture is engaged. The
    // kbutton route suppresses movement for the same window, and this is the
    // other half of that: leaving the analogue axes live here would walk the
    // pilot in whatever direction the wearer was answering a prompt with.
    if (DpadGestureEngaged()) {
        buttons |= DpadGestureButtons();
        pad.thumbLX = 0;
        pad.thumbLY = 0;
    }
    if (DpadGestureViewButton()) buttons |= kPadBack;

    // Withheld for the L3+R3 gesture, and while the panel is up. This is the
    // half of the chord suppression that vr_input cannot reach: it suppressing
    // sprint and melee on the kbutton route is worth nothing if this reports the
    // same clicks independently. No timing gate -- ConsumeThumbClicks is false
    // until the chord latches.
    if (!ConsumeThumbClicks() && !IsMenuOpen()) {
        if (g_controllerThumbstickClick[0]) buttons |= kPadLeftThumb;
        if (g_controllerThumbstickClick[1]) buttons |= kPadRightThumb;
    }
    // THE GRIPS ARE NOT REPORTED HERE, and it is the same rule START obeys
    // above: one owner per action.
    //
    // They were, and it is what the wearer reported as "both the grenade and
    // the shield" firing from either grip. One squeeze travelled two routes:
    // vr_input writes the kbutton for +offhand0 or +offhand1, correctly and
    // separately -- the GRIP EDGE line proves it, squeeze L=1.00 R=0.00 giving
    // ordnance 0->1 and tactical 0->0 -- and then this reported LEFT SHOULDER
    // as well, which the game's own gamepad layout binds to the OTHER ability.
    // Two routes, two different actions, both fire.
    //
    // The kbutton path keeps them because its assignment is the derived one:
    // kb_act.lst maps +offhand0 to #ORDNANCE_GRENADE and +offhand1 to
    // #TACTICAL_ABILITY, which is the separation the wearer asked for. The pad
    // reports the sticks and face buttons that MENUS need and nothing the
    // kbutton path already owns.
    //
    // THE DOUBLE-ROUTING NOTED HERE IS NOW FIXED, and the fix is the one this
    // note asked for: the menu/gameplay split above, not a deletion. The note
    // said these were "not visibly broken because both routes happen to land on
    // the same action -- but that is luck, not design." The luck ran out on
    // 2026-08-24: B reloaded AND crouched, because the pad layout binds B to
    // crouch while vr_input writes +reload for it. Kept as the record of why
    // the split exists.

    // START IS DELIBERATELY NOT MAPPED WHILE THE KEYBOARD PATH IS LIVE.
    //
    // The menu button already sends a real ESC through SendInput. Adding START
    // here would make one press arrive twice, by two routes, and a menu that
    // opens and instantly closes is exactly the double-fire this project has
    // already debugged once -- two edge detectors either side of the mode
    // switch. One owner per action until the kbutton path is retired in B3,
    // and then START comes back with ESC going away in the same change.

    // OR, never assign: a real pad's held buttons must survive.
    pad.buttons |= buttons;
}

// XR IS UP, judged by the controllers actually reporting rather than by our own
// arming flag. A flag says what we asked for; the sequence says what arrived.
bool ControllersAreLive() {
    static std::uint64_t lastSequence = 0;
    static std::uint64_t lastChangeTick = 0;
    const std::uint64_t sequence = g_controllerSequence;
    const std::uint64_t now = GetTickCount64();
    if (sequence != lastSequence) {
        lastSequence = sequence;
        lastChangeTick = now;
    }
    // Two seconds of no new controller sample means the runtime has gone away,
    // and a pad that keeps claiming to be connected on stale data is worse than
    // no pad: the game would keep polling a device that cannot answer.
    return lastChangeTick != 0 && (now - lastChangeTick) < 2000;
}

// There is deliberately no single "armed" predicate any more. The two questions
// the game asks have different answers: whether a pad EXISTS is config alone and
// must hold from startup, and whether it is being MOVED is liveness. Collapsing
// them into one flag is what made the first version answer neither.

// ---------------------------------------------------------------------------
// The three thunks. Real pad first, always.
// ---------------------------------------------------------------------------
DWORD WINAPI HookedGetCapabilities(DWORD userIndex, DWORD flags, XInputCapabilities* caps) {
    g_capsCalls.fetch_add(1, std::memory_order_relaxed);
    DWORD result = kErrorDeviceNotConnected;
    if (g_realGetCapabilities) result = g_realGetCapabilities(userIndex, flags, caps);
    if (result == kErrorSuccess) {
        // A REAL PAD ALWAYS WINS. Returning its own capabilities untouched is
        // what keeps this additive: the game sees the device it would have seen.
        g_capsRealPad.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    // NOT GATED ON THE CONTROLLERS BEING LIVE, and that was the first version's
    // whole failure. Measured 2026-08-21:
    //
    //   PAD armed=1 live=1 served=3 | caps calls=1 real=0 synth=0 | state calls=12
    //
    // The hook installed in time and all three names came through it, the game
    // asked the capabilities question exactly ONCE -- which is what 0xCE10
    // predicts, since InitializeXDevice runs four times and returns immediately
    // for every index but 0 -- and we declined to answer, because `Armed()`
    // requires the VR controllers to be reporting and this call happens during
    // game startup, long before OpenXR is up. The slot never attached and the
    // poll gave up after twelve calls.
    //
    // Whether a pad EXISTS is a static claim and must be answered from the
    // config alone. Whether it is being MOVED is the state call's business.
    // Slot 0 only: it is the only slot InitializeXDevice ever opens, and
    // claiming the others would attach three phantom pads for nothing.
    if (userIndex != 0 || !caps || !g_enabled.load(std::memory_order_acquire)) return result;

    std::memset(caps, 0, sizeof(*caps));
    caps->type = kDevTypeGamepad;
    caps->subType = kDevSubTypeGamepad;   // InitializeXDevice requires exactly this
    g_capsSynthesised.fetch_add(1, std::memory_order_relaxed);
    return kErrorSuccess;
}

DWORD WINAPI HookedGetState(DWORD userIndex, XInputState* state) {
    g_stateCalls.fetch_add(1, std::memory_order_relaxed);
    DWORD result = kErrorDeviceNotConnected;
    if (g_realGetState) result = g_realGetState(userIndex, state);
    const bool realPad = (result == kErrorSuccess);
    if (realPad) g_stateRealPad.fetch_add(1, std::memory_order_relaxed);

    if (userIndex != 0 || !state || !g_enabled.load(std::memory_order_acquire)) return result;

    if (!realPad) std::memset(state, 0, sizeof(*state));

    // ONCE THE PAD IS CLAIMED, IT MUST NEVER REPORT ITSELF DISCONNECTED.
    //
    // PollXDevices at 0xDE60 treats 0x48F (ERROR_DEVICE_NOT_CONNECTED) as a
    // teardown: it takes the branch at 0xDEEA, clears the slot's attached byte,
    // decrements the attached count and runs the disconnect path. There is no
    // reconnect anywhere in the poll, so a single disconnected answer retires
    // the slot for the rest of the session.
    //
    // So failing soft here means "connected and centred", NOT "gone". With the
    // headset off or the runtime asleep the gamepad stays zeroed -- the game
    // sees a pad resting at neutral, which is exactly true -- and the moment
    // the controllers start reporting again the same slot carries them.
    if (ControllersAreLive()) ComposeGamepad(state->gamepad);

    // The packet number must CHANGE when the state changes and HOLD when it does
    // not: a number that ticks every call makes a motionless stick look like
    // continuous input, which is the opposite of the problem this exists to fix.
    //
    // Compared against the last state WE RETURNED, not against what the real
    // pad said this call. With no real pad those are never equal -- the real
    // read fails and the buffer is zeroed every time -- so comparing to it
    // would tick the packet on every single call while the sticks sat still.
    static XInputGamepad lastReturned{};
    static bool haveLast = false;
    if (!haveLast || std::memcmp(&lastReturned, &state->gamepad, sizeof(lastReturned)) != 0) {
        lastReturned = state->gamepad;
        haveLast = true;
        state->packetNumber = g_packet.fetch_add(1, std::memory_order_relaxed) + 1;
    } else {
        state->packetNumber = g_packet.load(std::memory_order_relaxed);
    }
    g_stateSynthesised.fetch_add(1, std::memory_order_relaxed);
    return kErrorSuccess;
}

// The motor state the game last asked for. Held rather than consumed, because a

// sustained rumble is set ONCE and left -- see AdvanceHaptics.
std::atomic<unsigned> g_motorHeavy{0};
std::atomic<unsigned> g_motorLight{0};

std::atomic<std::uint64_t> g_lastHapticApply{0};

// Sends the current motor state to both controllers. Shared by the immediate
// path and the sustain tick so the two cannot drift apart.
void NoteHapticsApplied() { g_lastHapticApply.store(GetTickCount64(), std::memory_order_release); }

void ApplyMotorsToHaptics() {
    const unsigned heavy = g_motorHeavy.load(std::memory_order_acquire);
    const unsigned light = g_motorLight.load(std::memory_order_acquire);
    const unsigned strongest = heavy > light ? heavy : light;
    if (strongest == 0) {
        TriggerHaptic(0, 0.0f, 0.0f, 0);
        TriggerHaptic(1, 0.0f, 0.0f, 0);
        return;
    }
    float amplitude = (static_cast<float>(strongest) / 65535.0f) * HapticStrength();
    if (amplitude > 1.0f) amplitude = 1.0f;   // the spec's own ceiling
    const float total = static_cast<float>(heavy + light);
    const float lightShare = total > 0.5f ? static_cast<float>(light) / total : 0.5f;
    // 160 Hz when the heavy motor carries it, 260 Hz when the light one does --
    // and the census proves this is not decoration: the game sends heavy-only
    // and light-only events, so the two really do mean different things.
    const float frequency = 160.0f + lightShare * 100.0f;
    // 60 ms, longer than the sustain interval, so consecutive pulses overlap
    // rather than leaving gaps.
    TriggerHaptic(0, amplitude, frequency, 60000000);
    TriggerHaptic(1, amplitude, frequency, 60000000);
}

DWORD WINAPI HookedSetState(DWORD userIndex, XInputVibration* vibration) {
    g_setStateCalls.fetch_add(1, std::memory_order_relaxed);
    if (vibration) {
        const WORD heavy = vibration->leftMotorSpeed;
        const WORD light = vibration->rightMotorSpeed;
        // READ-ONLY census, bounded, of whether this game drives the two motors
        // differently at all. If every line shows an equal pair the frequency
        // term is decoration and can go.
        static WORD lastHeavy = 0xFFFF;
        static WORD lastLight = 0xFFFF;
        if ((heavy != lastHeavy || light != lastLight) && g_hapticLogBudget > 0) {
            --g_hapticLogBudget;
            char line[220]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] rumble: heavy=%u light=%u (%d lines left). Equal pairs mean the game does "
                "not use the two motors differently.\n",
                heavy, light, g_hapticLogBudget);
            Tf2VrLog(line);
        }
        lastHeavy = heavy;
        lastLight = light;
        g_motorHeavy.store(heavy, std::memory_order_release);
        g_motorLight.store(light, std::memory_order_release);
        // APPLIED IMMEDIATELY ON CHANGE, not left for the 40 ms tick.
        //
        // The census settled this. The game does not set a level and leave it --
        // it ANIMATES its own envelopes by re-sending a descending series
        // (65535/29490 -> 28418 -> 27321 -> 26233 -> 25140, and a low tail at
        // 894 -> 868 -> 836 -> 77). Those steps arrive faster than 40 ms, so a
        // timer-only path both DELAYED each change and sampled the game's own
        // envelope coarsely, flattening the shape it was drawing.
        //
        // So changes go straight through and the tick only SUSTAINS: it re-fires
        // when the game has gone quiet without dropping to zero.
        if (heavy != lastHeavy || light != lastLight) {
            ApplyMotorsToHaptics();
            NoteHapticsApplied();
        }
    }
    if (g_realSetState) return g_realSetState(userIndex, vibration);
    return 0;
}

// THE ONE HOOK. It answers three names and then gets out of the way.
FARPROC WINAPI HookedGetProcAddress(HMODULE module, LPCSTR name) {
    FARPROC real = g_originalGetProcAddress ? g_originalGetProcAddress(module, name)
                                            : GetProcAddress(module, name);
    // HIWORD(name) == 0 means an ordinal, not a string; dereferencing it as a
    // string is a fault, and inputsystem resolves plenty by ordinal elsewhere.
    if (!name || HIWORD(name) == 0) return real;

    if (std::strcmp(name, "XInputGetState") == 0) {
        g_realGetState = reinterpret_cast<XInputGetStateFn>(real);
        g_servedNames.fetch_add(1, std::memory_order_relaxed);
        return reinterpret_cast<FARPROC>(&HookedGetState);
    }
    if (std::strcmp(name, "XInputSetState") == 0) {
        g_realSetState = reinterpret_cast<XInputSetStateFn>(real);
        g_servedNames.fetch_add(1, std::memory_order_relaxed);
        return reinterpret_cast<FARPROC>(&HookedSetState);
    }
    if (std::strcmp(name, "XInputGetCapabilities") == 0) {
        g_realGetCapabilities = reinterpret_cast<XInputGetCapabilitiesFn>(real);
        g_servedNames.fetch_add(1, std::memory_order_relaxed);
        return reinterpret_cast<FARPROC>(&HookedGetCapabilities);
    }
    return real;
}

// ---------------------------------------------------------------------------
// Module-load watch. Same mechanism hid_capture.cpp proved -- it logged
// "installing now, before its enumeration runs" -- with one difference: NOTHING
// IS LOGGED FROM THE CALLBACK. It runs under the loader lock, and the
// process-wide sweep that hung this game hung because its callback wrote a file
// while that lock was held. A VirtualProtect and a pointer write are all that
// happen here; everything to say is said from the tick.
// ---------------------------------------------------------------------------
struct LdrUnicodeString {
    USHORT length;
    USHORT maximumLength;
    PWSTR buffer;
};
struct LdrNotificationData {
    ULONG flags;
    const LdrUnicodeString* fullDllName;
    const LdrUnicodeString* baseDllName;
    PVOID dllBase;
    ULONG sizeOfImage;
};
using LdrNotificationFn = VOID(CALLBACK*)(ULONG, const LdrNotificationData*, PVOID);
using LdrRegisterFn = LONG(NTAPI*)(ULONG, LdrNotificationFn, PVOID, PVOID*);
constexpr ULONG kDllLoaded = 1;

// Set by the callback, read and reported by the tick.
std::atomic_int g_pendingReport{0};   // 0 none, 1 installed, 2 slot not found

bool InstallOn(HMODULE module) {
    if (g_installed || !module) return false;
    void** slot = FindImportSlot(module, "GetProcAddress");
    if (!slot) {
        g_pendingReport.store(2, std::memory_order_release);
        return false;
    }
    g_getProcSlot = slot;
    void* original = nullptr;
    if (!SwapSlot(slot, reinterpret_cast<void*>(&HookedGetProcAddress), &original)) {
        g_pendingReport.store(2, std::memory_order_release);
        return false;
    }
    g_originalGetProcAddress = reinterpret_cast<GetProcAddressFn>(original);
    g_installed = true;
    g_pendingReport.store(1, std::memory_order_release);
    return true;
}

VOID CALLBACK DllLoadNotification(ULONG reason, const LdrNotificationData* data, PVOID) {
    if (reason != kDllLoaded || !data || !data->baseDllName || !data->baseDllName->buffer) return;
    // Compared without touching the loader or the CRT locale machinery: the
    // base name is short and ASCII, and this runs under the loader lock.
    const LdrUnicodeString* base = data->baseDllName;
    const int chars = base->length / static_cast<int>(sizeof(wchar_t));
    static const char kWanted[] = "inputsystem.dll";
    constexpr int kWantedLen = static_cast<int>(sizeof(kWanted) - 1);
    if (chars != kWantedLen) return;
    for (int i = 0; i < kWantedLen; ++i) {
        wchar_t c = base->buffer[i];
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        if (c != static_cast<wchar_t>(kWanted[i])) return;
    }
    InstallOn(static_cast<HMODULE>(data->dllBase));
}

void DrainPendingReport() {
    const int pending = g_pendingReport.exchange(0, std::memory_order_acq_rel);
    if (pending == 1) {
        Tf2VrLog("[TF2VR] synthetic pad: GetProcAddress slot swapped on inputsystem.dll at module "
                 "load, before InitializeXDevices runs. Nothing is synthesised until the game "
                 "resolves the three XInput names.\n");
    } else if (pending == 2) {
        Tf2VrLog("[TF2VR] synthetic pad: inputsystem.dll loaded but its GetProcAddress import slot "
                 "was NOT found -- nothing is hooked, and every counter below will read zero for "
                 "that reason rather than for any result.\n");
    }
}

}  // namespace

void RegisterXInputPadLoadWatch() {
    if (g_notificationRegistered) return;
    g_notificationRegistered = true;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) return;
    auto registerFn =
        reinterpret_cast<LdrRegisterFn>(GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    if (!registerFn) {
        Tf2VrLog("[TF2VR] synthetic pad: LdrRegisterDllNotification unavailable; falling back to "
                 "the per-tick retry, which is too late for InitializeXDevices by construction.\n");
        return;
    }
    const LONG status = registerFn(0, &DllLoadNotification, nullptr, &g_notificationCookie);
    char line[220];
    std::snprintf(line, sizeof(line),
                  "[TF2VR] synthetic pad: module-load watch %s (status 0x%08lX). It fires the "
                  "instant inputsystem.dll appears, which is the only moment between \"not there\" "
                  "and \"already enumerated\".\n",
                  status == 0 ? "registered" : "FAILED", static_cast<unsigned long>(status));
    Tf2VrLog(line);
    // Already loaded when we arrived? Then the watch will never fire for it.
    // Installing now cannot rescue a device init that has already run, and the
    // counters will say so.
    InstallOn(GetModuleHandleA("inputsystem.dll"));
}

void EnsureXInputPadInstalled() { InstallOn(GetModuleHandleA("inputsystem.dll")); }

void SetSyntheticPadEnabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    Tf2VrLog(enabled
        ? "[TF2VR] synthetic pad ARMED: the VR controllers answer the game's own XInput calls, "
          "composed OVER any real pad. Sticks carry real deflection, so movement and turn go "
          "through the game's own curves instead of the digital latches.\n"
        : "[TF2VR] synthetic pad off: the game's XInput calls are passed straight through.\n");
}

bool IsSyntheticPadEnabled() { return g_enabled.load(std::memory_order_acquire); }

// EVERY COUNTER THIS NEEDS TO BE READ BY, ON ONE LINE.
//
// The falsifier is `state`: InitializeXDevice attaches the slot only if the
// capabilities call is answered, and PollXDevices skips an unattached slot
// entirely, so `state=0` means the game never began reading and NOTHING else on
// the line means anything. `served` is the step before that -- if the three
// names were never resolved through our thunk, the hook went in too late.
// `realPad` counts separately from `synth`, so passthrough is visible as its own
// number rather than inferred.
void SetYLongPressMs(unsigned ms) {
    // Clamped, not trusted. Below ~120 ms a normal weapon-change tap would
    // start turning into a View press, which is the one failure the wearer
    // would experience as the control being broken. 0 stays 0: that is the
    // documented off switch, not a small threshold.
    const unsigned clamped = (ms == 0) ? 0u : (ms < 120u ? 120u : (ms > 2000u ? 2000u : ms));
    g_yLongPressMs.store(clamped, std::memory_order_relaxed);
    char line[300];
    std::snprintf(line, sizeof(line),
        clamped == 0
            ? "[TF2VR] input.y_longpress_ms = 0: Y is a plain weapon change on the press edge. "
              "The View button (BACK) is NOT reachable from Y.\n"
            : "[TF2VR] input.y_longpress_ms = %u: tap Y to change weapon (emitted on release), "
              "hold Y past %u ms for the View button, which restores faded waypoint markers. "
              "A long press never emits a weapon change.\n",
        clamped, clamped);
    Tf2VrLog(line);
}

void LogXInputPadCounters() {
    DrainPendingReport();
    if (!g_installed) return;
    static std::uint64_t nextReport = 0;
    const std::uint64_t now = GetTickCount64();
    if (nextReport == 0) { nextReport = now + 1000; return; }
    if (now < nextReport) return;
    nextReport = now + 1000;

    char line[400];
    std::snprintf(line, sizeof(line),
                  "[TF2VR] PAD armed=%d live=%d served=%d | caps calls=%llu real=%llu synth=%llu | "
                  "state calls=%llu real=%llu synth=%llu | setState=%llu\n",
                  g_enabled.load(std::memory_order_relaxed) ? 1 : 0, ControllersAreLive() ? 1 : 0,
                  g_servedNames.load(std::memory_order_relaxed),
                  static_cast<unsigned long long>(g_capsCalls.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_capsRealPad.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_capsSynthesised.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_stateCalls.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_stateRealPad.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(
                      g_stateSynthesised.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_setStateCalls.load(std::memory_order_relaxed)));
    Tf2VrLog(line);

    // Y's long press, on the same clock. Printed unconditionally so a run that
    // reports "nothing happened" says which half failed rather than leaving it
    // to be guessed: zero long presses means the hold never reached the
    // threshold, and long presses with no objectives on screen means BACK is
    // reaching the game and is not the control we want.
    char yline[240];
    std::snprintf(yline, sizeof(yline),
                  "[TF2VR] PAD Y: threshold %ums | taps (weapon change) %llu | LONG PRESSES "
                  "(View/BACK) %llu. A long press never emits Y.\n",
                  YLongPressMs(),
                  static_cast<unsigned long long>(g_yTaps.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_yLongPresses.load(std::memory_order_relaxed)));
    Tf2VrLog(yline);
}

// ---------------------------------------------------------------------------
// SUSTAINED HAPTICS. Per plugin frame.
//
// THE BUG THIS FIXES: rumble was sent once, on CHANGE, as an
// XR_MIN_HAPTIC_DURATION pulse. A sustained rumble -- the game sets the motors
// and leaves them there for a second or more -- therefore produced ONE micro
// pulse and then silence. That is most of "there is haptics but it is a bit hard
// to feel", and no amount of amplitude was going to fix it.
//
// WHY NOT JUST TURN IT UP: XrHapticVibration::amplitude is clamped to 0..1 by
// the specification, so anything over 100% was discarded by the runtime. That is
// exactly what the wearer observed -- "over 100% are indistinguishable". The
// levers that remain are DURATION and FREQUENCY, and this uses both.
//
// So: while the motors are non-zero, re-apply on a fixed interval with a
// duration slightly LONGER than the interval. The overlap means the actuator is
// driven continuously rather than in gaps, and each call replaces the last.
//
// FREQUENCY sits in the band a linear actuator is actually efficient at. The
// first version asked for 80 Hz when the heavy motor dominated, which is well
// below where a controller's actuator delivers force -- a request it technically
// honoured while producing almost nothing. 160-260 Hz is the useful band.
// ---------------------------------------------------------------------------
void AdvanceHaptics() {
    // SUSTAIN ONLY. Changes are applied the instant they arrive, above; this
    // exists solely for the case where the game sets a level and then says
    // nothing for a while, which a change-triggered pulse would let fall silent.
    //
    // It deliberately does NOT re-fire while the game is actively animating: if
    // a change arrived within the interval, the pulse it sent is still playing
    // and re-firing would restart it mid-envelope.
    constexpr std::uint64_t kSustainMs = 40;
    const std::uint64_t now = GetTickCount64();
    const unsigned heavy = g_motorHeavy.load(std::memory_order_acquire);
    const unsigned light = g_motorLight.load(std::memory_order_acquire);
    if (heavy == 0 && light == 0) return;
    if (now - g_lastHapticApply.load(std::memory_order_acquire) < kSustainMs) return;
    ApplyMotorsToHaptics();
    NoteHapticsApplied();
}
