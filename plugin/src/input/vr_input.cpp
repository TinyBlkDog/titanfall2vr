#include "vr_input.h"

#include "diagnostics.h"
#include "dpad_gesture.h"
#include "xr_input.h"
#include "render_resolution.h"
#include "aim_cmd.h"
#include "menu_overlay.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// Each entry: the `+command` handler, so its prologue can be checked, and the
// kbutton_t the handler writes. Both were read out of client.dll; see the
// header for how, and GATE1/handoff notes for the technique.
struct Button {
    const char* name;
    std::uintptr_t handlerRva;
    std::uintptr_t kbuttonRva;   // state word is +0x08
};

// Order is the order they are applied; nothing depends on it.
constexpr Button kButtons[] = {
    {"+forward",   0x2526C0, 0x11BE160},
    {"+back",      0x2520E0, 0x11BE170},
    {"+moveleft",  0x252EB0, 0x11BE140},
    {"+moveright", 0x252FA0, 0x11BE150},
    {"+left",      0x252AF0, 0x11C1468},
    {"+right",     0x253930, 0x11C1478},
    {"+jump",      0x252910, 0x11BE190},
    {"+duck",      0x2525D0, 0x11C1418},
    {"+attack",    0x251FF0, 0x11BE1F0},
    {"+reload",    0x253720, 0x11BE1A0},
    // Resolved the same way as the ten above, and none of it guessed: the
    // command STRING was found in client.dll's .rdata, the single rip-relative
    // reference to it is the ConCommand registration, its `lea r8` is the
    // handler, and the handler's last `mov [rip+..], eax` is the kbutton's
    // STATE word -- so the struct base is that minus 8. Every one came out
    // 109 bytes with the KeyDown prologue, the same shape as the ten above,
    // and the runtime prologue check below still has to pass before any of
    // them is written.
    //
    // Which offhand is which is the game's own answer, not a guess:
    // r2\R2Northstar\runtime\compiled\scripts\kb_act.lst maps
    //   +offhand0 -> #ORDNANCE_GRENADE
    //   +offhand1 -> #TACTICAL_ABILITY
    //   +weaponCycle -> #SWITCH_WEAPONS_PILOT   (capital C; the lowercase
    //                   spelling does not exist in the binary)
    {"+melee",       0x252DC0, 0x11C1578},
    {"+speed",       0x253B80, 0x11BE120},
    {"+offhand0",    0x253090, 0x11C1518},
    {"+offhand1",    0x253180, 0x11C1528},
    {"+weaponCycle", 0x254220, 0x11C14D8},
    // AIM DOWN SIGHTS, derived by the same method and POSITIVE-CONTROLLED on
    // an entry already in this table before it was trusted.
    //
    // kb_act.lst: "+zoom" -> #AIM_MODIFIER (hold). "+toggle_zoom" is the toggle
    // variant and is deliberately not used -- the wearer asked for the trigger,
    // and a hold on a trigger is what a trigger is.
    //
    // The control: re-running the method on "+melee" reproduced 0x252DC0 and
    // 0x11C1578, this table's existing values, so the derivation below is not
    // resting on its own say-so. pescan selftest on this client.dll first:
    // 46493 functions, 1738832 instructions, 0.9872% bad.
    //
    // "+zoom" at .rdata 0x905AD4; its single rip-relative reference is the
    // registration at 0x86AE50, whose `lea r8` gives handler 0x254880; the
    // handler's last `mov [rip+..], eax` writes 0x11C1440, so the struct base
    // is 0x11C1438. 109 bytes, one .pdata fragment, KeyDown prologue -- the
    // same shape as all fifteen above, and the runtime prologue check still
    // has to pass before it is written.
    {"+zoom",        0x254880, 0x11C1438},
    // CROUCH AS THE ENGINE'S OWN TOGGLE, not one we hand-roll.
    //
    // kb_act.lst has BOTH "+duck" -> #CROUCH and "+toggle_duck" ->
    // #TOGGLE_CROUCH, so the game already owns a toggle-crouch command and the
    // Hold/Toggle option in its menu is a choice between these two. Driving the
    // engine's command is a rung above re-implementing its behaviour.
    //
    // Derived by the same method as the six above and POSITIVE-CONTROLLED on an
    // entry already in this table. pescan selftest on this client.dll first:
    // 46493 functions, 1738832 instructions, 0.9872% bad.
    //
    // The control: re-running the method end to end on "+duck" reproduced this
    // table's existing 0x2525D0 and 0x11C1418 exactly -- string at .rdata
    // 0x8EBF5C, its registration at 0x86A5A0, whose `lea r8` gives 0x2525D0, and
    // whose last `mov [rip+..], eax` writes 0x11C1420 for a base of 0x11C1418.
    //
    // "+toggle_duck" at .rdata 0x905A08; its single rip-relative reference is
    // the registration at 0x86AD00, whose `lea r8` gives handler 0x254A60; the
    // handler's last `mov [rip+..], eax` writes 0x11C1430, so the struct base is
    // 0x11C1428. extent says root 0x254A60, 109 bytes, ONE .pdata fragment, and
    // it opens `sub rsp,0x28 / cmp dword [rcx],1` -- the KeyDown prologue, read
    // out of the binary rather than assumed, which matters because ONE prologue
    // mismatch refuses every button in this table.
    {"+toggle_duck",  0x254A60, 0x11C1428},
};
constexpr int kButtonCount = static_cast<int>(sizeof(kButtons) / sizeof(kButtons[0]));

enum Index { Forward, Back, MoveLeft, MoveRight, TurnLeft, TurnRight, Jump, Duck, Attack, Reload,
             Melee, Sprint, Ordnance, Tactical, WeaponCycle, Ads, ToggleDuck };

// Every handler is Source's KeyDown and they all open the same way:
// sub rsp,28h / cmp dword [rcx],1. Checked so a build whose addresses moved
// refuses instead of writing into whatever is there now.
constexpr std::uint8_t kKeyDownPrologue[] = {0x48, 0x83, 0xEC, 0x28, 0x83, 0x39, 0x01};

// Source's kbutton_t state bits.
constexpr int kStateHeld = 1;
constexpr int kStatePressed = 2;
constexpr int kStateReleased = 4;

std::uint8_t* g_client = nullptr;
bool g_resolved = false;
bool g_enabled = false;
bool g_wanted = false;
bool g_held[kButtonCount]{};

// Off-centre past this and the stick counts as a held key. Generous, because a
// digital latch driven from an analogue stick wants a clear commit rather than
// a hair trigger that flickers on and off at the edge.
constexpr float kMoveDeadzone = 0.35f;
constexpr float kTurnDeadzone = 0.45f;
constexpr float kJumpCrouchDeadzone = 0.6f;
constexpr float kTriggerThreshold = 0.6f;
// The grips are analogue on Touch too. Same threshold as the trigger: both are
// squeezes and there is no reason for them to commit at different pressures.
constexpr float kGripThreshold = 0.6f;
// Release lower than engage, so a stick resting on the line cannot chatter.
constexpr float kReleaseFactor = 0.6f;


// input.crouch_mode. See the notes in the header.
// Defaults are the wearer's stated mapping: grenade on the RIGHT grip, and
// crouch as a toggle driven by the ENGINE'S OWN +toggle_duck command.
std::atomic_int g_crouchMode{1};

// RIGHT-STICK CROSS-TALK. The right stick carries turn on X and jump/crouch on
// Y, and with a plain per-axis magnitude threshold a diagonal push satisfies
// BOTH -- so turning clips a crouch and a crouch clips a turn. The wearer
// reports both directions.
//
// The gate is AXIS DOMINANCE, not a bigger deadzone. An axis counts only if its
// own deflection is at least kCrossRatio times the OTHER axis. At 0.6 that is a
// roughly +/-31 degree exclusive cone around each axis, with a wide diagonal
// band in between where BOTH still fire -- which is the point. The wearer asked
// for a concerted push, not for simultaneous turn-and-crouch to be taken away.
//
// Tunable from the ini so dialling it in costs an ini edit, not a rebuild and
// another headset run.
std::atomic<float> g_turnDeadzone{0.45f};
std::atomic<float> g_jumpCrouchDeadzone{0.6f};
std::atomic<float> g_stickCrossRatio{0.6f};

// input.turn_speed -- degrees per second for stick turning. 0 leaves the game's
// own cl_yawspeed alone.
//
// THE ENGINE'S OWN LEVER, not a rate of our own. Turning already goes through
// the engine's +left/+right, which is what keeps prediction, aim assist and the
// head-tracking composition agreeing about where the body is pointing. cl_yawspeed
// is the number that path already reads, so setting it changes the speed without
// introducing a second turn mechanism to disagree with the first.
std::atomic<float> g_hapticStrength{100.0f};   // per cent; 100 is the spec's own ceiling
std::atomic<float> g_turnSpeed{70.0f};   // the SETTING, 0..100 -- see SetTurnSpeed

// input.turn_curve -- the response curve on the turn stick.
//
// 1.0 is linear, which is what shipped and what the wearer described as "it ramps
// to 100% very quickly ... impossible to really get any feel to it". A linear
// map after a large deadzone spends most of the remaining travel near the top.
// 2.0 squares it: the first half of the travel covers a quarter of the rate, so
// slow precise turns get most of the stick and the fast end is still reachable.
std::atomic<float> g_turnCurve{2.0f};

// input.turn_mode -- 0 = smooth, 1 = snap. A MODE, not a magic zero.
//
// Snap and smooth are different behaviours with different settings, not two ends
// of one slider. Encoding "snap off" as snap_turn = 0 meant the snap angle and
// the mode shared one control, so you could not set 30 degrees and then switch
// back to smooth without losing it.
std::atomic_int g_turnMode{0};

// input.snap_turn -- degrees per step, used in snap mode.
//
// Built on the SAME +left/+right path rather than by writing the view angle
// directly. A direct write would fight the head-tracking composition, which
// forms the gun and aim against the body yaw; the engine turning its own body is
// the thing that composition is already built around.
//
// So a snap is the turn button held for exactly as long as the engine needs to
// cover the angle: degrees / yawspeed seconds. It is a very fast turn rather than
// an instantaneous teleport, which is also what most comfort implementations
// actually do -- and raising turn_speed makes it harder if that is wanted.
std::atomic<float> g_snapTurnDegrees{30.0f};
float g_effectiveYawSpeed = 210.0f;   // Source default until the engine says otherwise

// CROUCH STATE, AND WHY THERE IS EXACTLY ONE EDGE DETECTOR.
//
// g_crouchStickHeld tracks THE STICK and nothing else, and it is computed once,
// ABOVE the mode branch. It deliberately does not reuse g_held[Duck] or
// g_held[ToggleDuck], because in either toggle mode the stick is centred while
// the player stays crouched, and feeding an output back into Latch's hysteresis
// would compare the release threshold against a stick nobody is pushing.
//
// One detector above the branch rather than one per arm is the whole point: two
// held-flags either side of a mode switch both fire the instant the mode flips,
// which reads exactly like a phantom press.
//
// g_crouchLatched is used by mode 2 ONLY -- the hand-rolled fallback.
bool g_crouchStickHeld = false;
bool g_crouchLatched = false;
// Separate from g_crouchStickHeld so the release edge can be reported too; it
// is only ever read by the log line.
bool g_crouchEdgeWasDown = false;

int* StateWord(int index) {
    return reinterpret_cast<int*>(g_client + kButtons[index].kbuttonRva + 8);
}

// ---------------------------------------------------------------------------
// CONTROLLER MODE.
//
// The wearer's aim is not to work around keyboard prompts one at a time -- it
// is to get the game INTO controller mode, so "press SPACE to continue" becomes
// "press A" and maps to a button we already send.
//
// client.dll registers a script native whose own help text says exactly what is
// wanted: "Returns whether the user is using a controller instead of a
// keyboard". The registration at 0x3AD055 is a TYPED native table --
// {help, "IsControllerModeActive", "Script_IsControllerModeActive", fn} -- so
// the framework marshals the return and the function itself takes NO arguments.
// That resolves what looked like a problem: fn is a three-instruction thunk
//
//   mov rcx, [rip+..]{904FC8}   ; the interface pointer
//   mov rax, [rcx]              ; its vtable
//   jmp [rax+0x90]              ; bool, no arguments
//
// and overwriting rcx is correct for a `bool()` rather than evidence that this
// is not the native.
//
// So the state lives behind vtable slot +0x90 of the interface at
// client.dll+0x904FC8. A SLOT SWAP is the project's preferred technique over an
// entry detour, needs no prologue check, and is undone by putting the pointer
// back.
//
// MEASURED FACT THIS MUST RESPECT: prompt mode is LAST INPUT DEVICE, not device
// presence. With a real pad connected before launch the prompts stayed on
// keyboard until an input arrived. So this may report "controller" only after
// the game has SEEN controller input, and forcing the query true may not be
// sufficient on its own. It is cheap and reversible, which is why it is worth
// trying before building a virtual HID device.
constexpr std::uintptr_t kControllerModeInterfaceRva = 0x904FC8;
constexpr std::uintptr_t kIsControllerModeSlot = 0x90;

using ControllerModeFn = bool(__fastcall*)(void*);
ControllerModeFn g_originalControllerMode = nullptr;
void** g_controllerModeVtable = nullptr;
bool g_forceControllerMode = false;

// input.keyboard_menu_nav -- ONE SWITCH FOR ONE MECHANISM.
//
// Everything this gates is the same idea: drive the game's menus by INJECTING
// KEYSTROKES, because the game did not believe a controller existed. As of
// 1.6-B2 it does -- its own IsControllerModeActive reads 1 with forced=0, it
// polls our synthetic pad 15522 times a session and sends rumble back to it --
// so the game can drive its own menus, which is what every reference mod
// relies on and what a real pad was already observed doing here.
//
// The two are gated together deliberately. The menu branch injects arrows and
// SPACE, and the gameplay branch injects SPACE on A; they are one mechanism
// wearing two names, and retiring half of it would leave a press arriving by
// two routes at once -- the double-fire this project has already debugged
// twice. Default 1 until the pad is proven to drive the menus in the headset.
bool g_keyboardMenuNav = true;
bool g_controllerModeHooked = false;

bool __fastcall ForcedControllerModeActive(void* self) {
    (void)self;
    return true;
}

// THE GAME'S OWN CONTROLLER-MODE FLAG, HELD DOWN.
//
// Read out of the ORIGINAL function at client.dll+0x24ED80, dumped from the
// live vtable slot rather than guessed:
//
//     if (!global->vtable[0x108]()) return false;   // a global gate
//     if (this->byte[0x0B] == 0)    return false;   // THIS flag
//     return true;
//
// Two conditions, ANDed, and `this` is the same interface pointer we already
// hold at client.dll+0x904FC8. Byte +0x0B is the one real pad input sets.
//
// WHY THIS AND NOT THE NATIVE. Three runs bracketed it: forcing the native TRUE
// with the pad idle still gave ONE screen, so the dropship gate does not read
// the native at all. The GLYPHS read our forced slot; the GATE reads the state
// underneath it. Holding that state at 0 is the only arrangement that gives the
// wearer both of their requirements at once -- controller-only input AND one
// press -- and it is exactly the combination they have already seen working,
// just reached without needing the pad to sit idle.
//
// It writes the game's own bool, not input: no event is fabricated and no press
// is sent on the wearer's behalf.
//
// FALSIFIER BUILT IN: `cleared` counts how often the flag was found set and put
// back. If it stays 0 while the pad is being used, +0x0B is NOT the flag pad
// input sets, the theory is wrong, and the other condition -- the global at
// vtable +0x108 -- is next. A zero here means "wrong field", never "worked".
bool g_holdKeyboardMode = false;
std::uint64_t g_controllerFlagCleared = 0;

// WATCH THE WHOLE OBJECT, because holding one byte of it was not enough.
//
// Measured: with the byte at +0x0B held at 0 the native reports keyboard
// (controllerMode=0) and flagCleared climbed to 9 -- so the field is right, the
// write works, and pad input really does set it. THE GATE STILL BLOCKED. So the
// gate reads a different consequence of pad input, and the cheapest place to
// look for it is the rest of this same object: whatever else flips when the
// wearer first touches the pad is the candidate.
//
// Read-only. Dumps the first 64 bytes whenever any of them changes, capped, so
// a press produces a short before/after pair rather than a flood.
// WHERE DID THE OTHER TWO PRESSES GO?
//
// The wearer pressed A three times -- menu, screen one, screen two -- and the
// synthetic pad's edge detector logged exactly ONE, while the pad was live and
// the game polled it 4332 times. So two presses dismissed two screens without
// passing through the pad's button path at all.
//
// That anchor lives inside ComposeGamepad, which only runs when the controllers
// are reporting; this one does not. It watches the raw XR button bits every
// tick, unconditionally, so "the wearer did not press" and "we never saw the
// press" stop looking identical -- which is exactly the confusion that made the
// single anchor unreadable.
void WatchRawControllerButtons() {
    static std::uint8_t previous[2]{};
    static bool have = false;
    static int logged = 0;
    if (logged >= 30) return;
    const std::uint8_t left = g_controllerButtons[0];
    const std::uint8_t right = g_controllerButtons[1];
    if (have && left == previous[0] && right == previous[1]) return;
    previous[0] = left;
    previous[1] = right;
    if (!have) { have = true; return; }   // first sample is a baseline, not an edge
    ++logged;
    char line[220];
    std::snprintf(line, sizeof(line),
                  "[TF2VR] RAWBTN left=0x%02X right=0x%02X (A is bit 0 of right) | seq=%llu\n",
                  left, right, static_cast<unsigned long long>(g_controllerSequence));
    Tf2VrLog(line);
}

void WatchControllerObject() {
    if (!g_client) return;
    static std::uint8_t previous[64]{};
    static bool havePrevious = false;
    static int dumps = 0;
    if (dumps >= 24) return;   // bounded by construction
    __try {
        auto* object = *reinterpret_cast<std::uint8_t**>(g_client + kControllerModeInterfaceRva);
        if (!object) return;
        std::uint8_t now[64]{};
        std::memcpy(now, object, sizeof(now));
        if (havePrevious && std::memcmp(now, previous, sizeof(now)) == 0) return;
        std::memcpy(previous, now, sizeof(now));
        const bool first = !havePrevious;
        havePrevious = true;
        ++dumps;
        char line[420];
        int used = std::snprintf(line, sizeof(line), "[TF2VR] CTRLOBJ %s:",
                                 first ? "initial" : "CHANGED");
        for (int i = 0; i < 64 && used < static_cast<int>(sizeof(line)) - 8; ++i) {
            used += std::snprintf(line + used, sizeof(line) - used, " %02X", now[i]);
        }
        std::snprintf(line + used, sizeof(line) - used, "\n");
        Tf2VrLog(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void HoldControllerFlagImpl() {
    if (!g_holdKeyboardMode || !g_client) return;
    __try {
        auto* object = *reinterpret_cast<std::uint8_t**>(g_client + kControllerModeInterfaceRva);
        if (!object) return;
        if (object[0x0B] != 0) {
            object[0x0B] = 0;
            ++g_controllerFlagCleared;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// THE GAME'S REAL INPUT MODE, READ DIRECTLY. Read-only.
//
// `controllerMode=` on the candidates line reads the NATIVE, and we FORCED the
// native, so it answers 1 whatever the game believes -- our own override hiding
// the one state worth seeing. The real flag is byte +0x0B of the interface:
// non-zero is controller, zero is keyboard, and the game sets it from input.
//
// This exists because of the wearer's report that after changing the resolution
// in the VR panel, a HUD ability reads UNBOUND and its button stops working --
// the signature of the game switching to KEYBOARD bindings for an ability that
// only has a controller one. mat_setvideomode recreates the window, and if the
// game resets this flag while doing so, that IS the bug. -1 means the object
// could not be read at all, which is a third answer and must not be printed as
// a zero.
// Declared in the header; see the note there.
int ReadGameControllerModeFlagImpl() {
    if (!g_client) return -1;
    __try {
        auto* object = *reinterpret_cast<std::uint8_t**>(g_client + kControllerModeInterfaceRva);
        if (!object) return -1;
        return object[0x0B] != 0 ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Reports what the game currently believes, without changing it.
bool QueryControllerModeActive() {
    if (!g_client) return false;
    __try {
        auto* object = *reinterpret_cast<void**>(g_client + kControllerModeInterfaceRva);
        if (!object) return false;
        auto** vtable = *reinterpret_cast<void***>(object);
        auto fn = g_controllerModeHooked
                      ? g_originalControllerMode
                      : reinterpret_cast<ControllerModeFn>(vtable[kIsControllerModeSlot / 8]);
        if (!fn) return false;
        return fn(object);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ApplyControllerModeHook() {
    if (!g_forceControllerMode || g_controllerModeHooked || !g_client) return;
    __try {
        auto* object = *reinterpret_cast<void**>(g_client + kControllerModeInterfaceRva);
        if (!object) return;   // not constructed yet; try again next tick
        auto** vtable = *reinterpret_cast<void***>(object);
        void** slot = &vtable[kIsControllerModeSlot / 8];
        g_originalControllerMode = reinterpret_cast<ControllerModeFn>(*slot);
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return;
        *slot = reinterpret_cast<void*>(&ForcedControllerModeActive);
        VirtualProtect(slot, sizeof(void*), old, &old);
        g_controllerModeHooked = true;
        char line[220];
        std::snprintf(line, sizeof(line),
                      "[TF2VR] controller mode FORCED: IsControllerModeActive slot +0x%llX swapped; "
                      "the game believed %s beforehand.\n",
                      static_cast<unsigned long long>(kIsControllerModeSlot),
                      g_originalControllerMode ? "it was readable" : "nothing");
        Tf2VrLog(line);

        // THE INTERNAL FLAG, DUMPED SO IT CAN BE FOUND WITHOUT A SECOND RUN.
        //
        // Three runs bracketed the gate: forcing this slot TRUE with no pad
        // input still gave ONE screen, so the dropship gate does NOT read this
        // native -- it reads the game's own internal controller-mode state,
        // which only the ORIGINAL function knows how to find. The glyphs read
        // the slot we already own; the gate reads a field we do not.
        //
        // That field is one disassembly away, and the original function pointer
        // is in our hand right now. Logging its RVA plus its opening bytes means
        // the field offset can be read straight out of the log offline, instead
        // of costing a run to discover an address and another to use it.
        if (g_originalControllerMode) {
            const auto* code = reinterpret_cast<const std::uint8_t*>(g_originalControllerMode);
            const auto rva = static_cast<unsigned long long>(
                reinterpret_cast<const std::uint8_t*>(g_originalControllerMode) - g_client);
            char dump[420];
            int used = std::snprintf(dump, sizeof(dump),
                                     "[TF2VR] controller mode ORIGINAL fn at client.dll+0x%llX; "
                                     "first bytes:", rva);
            __try {
                for (int i = 0; i < 48 && used < static_cast<int>(sizeof(dump)) - 8; ++i) {
                    used += std::snprintf(dump + used, sizeof(dump) - used, " %02X", code[i]);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                used += std::snprintf(dump + used, sizeof(dump) - used, " <faulted>");
            }
            std::snprintf(dump + used, sizeof(dump) - used, "\n");
            Tf2VrLog(dump);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Tf2VrLog("[TF2VR] controller mode: faulted swapping the slot; left alone.\n");
        g_forceControllerMode = false;
    }
}

// ---------------------------------------------------------------------------
// THE PAUSE MENU. Not a kbutton, and it does not need to be.
//
// It was briefly written off here as "engine UI on the ESC path, so it needs a
// different mechanism" -- which was an assertion, not a finding, and wrong in
// the way that matters: the game ships full gamepad support, so a controller
// can obviously reach its own menu without a keyboard. Going and looking found
// Source's standard pair, `gameui_activate` and `gameui_hide`, registered in
// engine.dll.
//
// Both handlers are three-instruction thunks onto a singleton:
//
//   mov rax, [rip+..]{7E56D0}    ; the object's vtable
//   lea rcx, [rip+..]{7E56D0}    ; this = the object
//   jmp [rax+0x28]               ; activate   (+0x30 hides)
//
// They OVERWRITE rcx before jumping, so they never read the CCommand argument a
// ConCommand callback is handed. That is what makes them callable directly with
// no arguments and no fabricated command context -- the thing that made the
// console route look unattractive is simply not present here.
//
// Verified by BYTES before being called, exactly as the kbutton handlers are.
// A build whose addresses moved gets a refusal, not a call into whatever now
// occupies that address.
// Defined with the menu-navigation block below; used by the menu button here.
void SendKey(unsigned vk);
bool GameHasFocus();

constexpr std::uintptr_t kGameUiActivateRva = 0x24A410;
constexpr std::uintptr_t kGameUiHideRva = 0x24A470;
// The singleton both thunks load, and the vtable slot that answers "is the
// menu up". See the note on GameUiVisible for how the slot was identified.
constexpr std::uintptr_t kGameUiObjectRva = 0x7E56D0;
constexpr std::uintptr_t kGameUiVisibleSlot = 0x18;
constexpr std::uint8_t kGameUiActivateBytes[] = {
    0x48, 0x8B, 0x05, 0xB9, 0xB2, 0x59, 0x00,
    0x48, 0x8D, 0x0D, 0xB2, 0xB2, 0x59, 0x00,
    0x48, 0xFF, 0x60, 0x28,
};
constexpr std::uint8_t kGameUiHideBytes[] = {
    0x48, 0x8B, 0x05, 0x59, 0xB2, 0x59, 0x00,
    0x48, 0x8D, 0x0D, 0x52, 0xB2, 0x59, 0x00,
    0x48, 0xFF, 0x60, 0x30,
};

std::uint8_t* g_engine = nullptr;
bool g_gameUiResolved = false;
bool g_gameUiUsable = false;
// OUR OWN belief about whether the menu is up. The player can also open and
// close it with ESC, which we never see, so this can desync -- one further
// press resyncs it. Tracking it is still better than only ever activating,
// which would give the wearer a button that opens a menu they cannot close.
bool g_gameUiOpen = false;

bool EnsureGameUiResolved() {
    if (g_gameUiResolved) return g_gameUiUsable;
    HMODULE engine = GetModuleHandleA("engine.dll");
    if (!engine) return false;   // not yet, not a refusal
    g_gameUiResolved = true;
    auto* base = reinterpret_cast<std::uint8_t*>(engine);
    const bool activateOk = std::memcmp(base + kGameUiActivateRva, kGameUiActivateBytes,
                                        sizeof(kGameUiActivateBytes)) == 0;
    const bool hideOk = std::memcmp(base + kGameUiHideRva, kGameUiHideBytes,
                                    sizeof(kGameUiHideBytes)) == 0;
    if (!activateOk || !hideOk) {
        char line[224];
        std::snprintf(line, sizeof(line),
                      "[TF2VR] vrinput: gameui_activate/gameui_hide are not the thunk shape this "
                      "was built against (activate ok=%d hide ok=%d). Menu button disabled; "
                      "nothing is called.\n",
                      activateOk ? 1 : 0, hideOk ? 1 : 0);
        Tf2VrLog(line);
        return false;
    }
    g_engine = base;
    g_gameUiUsable = true;
    Tf2VrLog("[TF2VR] vrinput: menu button armed -- gameui_activate and gameui_hide both matched "
             "their expected thunks.\n");
    return true;
}

// IS THE MENU UP? Vtable slot +0x18 on the same singleton.
//
// Found by elimination rather than by guessing: of the twenty-six references to
// engine+0x7E56D0, every one is a `jmp [rax+slot]` command thunk -- void, no
// result -- except a single site at engine+0x24913F which does
//
//   mov rax,[7E56D0] / lea rcx,[7E56D0] / call [rax+0x18] / test al,al / je ...
//
// A slot that is CALLED, returns a value in al, and gates a branch is a
// predicate, and the only predicate this object exposes. That is the shape of
// Source's IsGameUIVisible.
//
// STRONG INFERENCE, NOT PROOF -- so it proves itself at runtime instead. Until
// this has been observed returning BOTH true and false in one session, the
// query is not trusted and nothing is gated on it. A slot that is really
// something else would sit stuck at one value and never arm; a stuck-true
// predicate is the dangerous failure here, because it would make menu
// navigation fire during gameplay, and "must have changed at least once" rules
// exactly that out.
bool g_uiSeenTrue = false;
bool g_uiSeenFalse = false;
bool g_uiQueryFaulted = false;

bool GameUiVisible() {
    if (g_uiQueryFaulted || !EnsureGameUiResolved()) return false;
    auto* object = reinterpret_cast<std::uint8_t*>(g_engine + kGameUiObjectRva);
    bool visible = false;
    __try {
        auto** vtable = *reinterpret_cast<void***>(object);
        using Query = bool(__fastcall*)(void*);
        visible = reinterpret_cast<Query>(vtable[kGameUiVisibleSlot / 8])(object);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Tf2VrLog("[TF2VR] vrinput: faulted querying the game-UI predicate; it will not be used "
                 "again this session.\n");
        g_uiQueryFaulted = true;
        return false;
    }
    if (visible && !g_uiSeenTrue) {
        g_uiSeenTrue = true;
        Tf2VrLog("[TF2VR] vrinput: game-UI predicate returned TRUE for the first time.\n");
    } else if (!visible && !g_uiSeenFalse) {
        g_uiSeenFalse = true;
        Tf2VrLog("[TF2VR] vrinput: game-UI predicate returned FALSE for the first time.\n");
    }
    return visible;
}

// The predicate is only believed once it has been seen BOTH ways.
bool GameUiQueryTrusted() { return g_uiSeenTrue && g_uiSeenFalse; }

// SLOT +0x18 IS NOT THE UI FLAG. Measured: 68 samples, every one FALSE, during
// a session where the menu was demonstrably opened and closed several times.
// The inference behind it -- "the only slot that is CALLED and tested must be
// the predicate" -- was reasonable and wrong; the slot is some other boolean.
// The trust gate is what stopped that being harmful, by refusing to arm on a
// signal that never proved itself rather than firing keystrokes into gameplay.
//
// So the menu state is TRACKED instead, from the only two things that actually
// toggle it that we can see: the ESC we send ourselves, and a physical ESC on
// the keyboard. It desyncs only if the menu is closed some other way (clicking
// Resume with the mouse, a level transition), and one press of the menu button
// puts it back.
//
// Deliberately NOT another guessed engine address. Two have now been wrong, so
// the candidates below are LOGGED rather than trusted, and whichever of them
// actually tracks the menu can replace this next time -- chosen from a
// measurement instead of from a third inference.
bool g_menuOpen = false;
bool g_physicalEscapeHeld = false;
// OUR OWN injected Escape is a REAL key event -- that is the whole point of
// using scan codes -- so GetAsyncKeyState sees it too, and without this the
// menu button would toggle the tracked state twice and immediately undo
// itself. Suppresses the physical watcher for long enough to cover the
// injection, and no longer.
std::uint64_t g_suppressEscapeUntil = 0;
// Buttons still held when the menu closed. See the note at the mode switch:
// B exits the menu and B is reload, so without this the weapon reloads every
// time the menu is dismissed with it.
bool g_wasInMenu = false;
bool g_consumeRightPrimary = false;
bool g_consumeRightSecondary = false;
// Set on the frame the L3+R3 chord latches, cleared when BOTH clicks are up.
// Never consulted before the chord latches, so it adds no latency to either
// click. The synthetic pad honours it too.
bool g_consumeThumbClicks = false;

void NoteMenuToggled() { g_menuOpen = !g_menuOpen; }

// THE MENU DETECTOR, CHOSEN BY MEASUREMENT.
//
// Three candidates were logged for a session in which the menu was genuinely
// opened, navigated and closed. The result was not close:
//
//   slot +0x18     0 in every sample, across two sessions   -- dead
//   ClipCursor     constant 2560 wide                       -- useless
//   CURSOR_SHOWING contiguous blocks matching the session   -- tracks it
//
// The cursor trace read `111111111111111111 0000....0000 11`: eighteen seconds
// showing while the wearer navigated the startup menu, a long run hidden during
// play, then showing again when they reopened it. Contiguous blocks, not noise.
//
// It is also the only candidate that needs NO engine address, so it cannot rot
// with a game patch -- and it is indifferent to HOW the menu was opened, which
// the tracked flag never could be: that session used the real controller's menu
// button and our tracking saw not one toggle of it.
bool MenuIsOpen() {
    CURSORINFO cursor{};
    cursor.cbSize = sizeof(cursor);
    if (!GetCursorInfo(&cursor)) return g_menuOpen;   // fall back to tracking
    return (cursor.flags & CURSOR_SHOWING) != 0;
}

// Watches for the player's OWN Escape, so pressing it on the keyboard keeps our
// idea of the menu in step with theirs.
void PollPhysicalEscape() {
    const bool down = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    const bool suppressed = GetTickCount64() < g_suppressEscapeUntil;
    if (down && !g_physicalEscapeHeld && !suppressed) NoteMenuToggled();
    g_physicalEscapeHeld = down;
}

// The candidate census that CHOSE the detector above. Kept, because it is what
// makes a later regression legible: if menu gating ever misbehaves, this line
// says whether the cursor signal stopped tracking or something else changed.
// `slot0x18` stays in it as the refuted control -- a column that is always 0 is
// how a future reader can tell this was measured rather than assumed.
void SetHoldKeyboardModeImpl(bool hold) {
    g_holdKeyboardMode = hold;
    Tf2VrLog(hold
        ? "[TF2VR] holding the game's internal controller-mode flag (interface +0x0B) at 0; the "
          "forced native still reports controller, so glyphs stay on pad art.\n"
        : "[TF2VR] game's internal controller-mode flag left alone.\n");
}

void LogMenuCandidates() {
    CURSORINFO cursor{};
    cursor.cbSize = sizeof(cursor);
    const bool showing = GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING) != 0;
    RECT clip{};
    const bool haveClip = GetClipCursor(&clip) != 0;
    const long clipWidth = haveClip ? (clip.right - clip.left) : -1;
    char line[240];
    std::snprintf(line, sizeof(line),
                  "[TF2VR] vrinput menu candidates: tracked=%d slot0x18=%d cursorShowing=%d "
                  "clipWidth=%ld | controllerMode=%d forced=%d flagCleared=%llu | GAME REAL MODE "
                  "+0x0B=%d (1 controller, 0 KEYBOARD -- an ability with only a pad binding reads "
                  "UNBOUND at 0; -1 means unreadable, which is not a zero)\n",
                  g_menuOpen ? 1 : 0, GameUiVisible() ? 1 : 0, showing ? 1 : 0, clipWidth,
                  QueryControllerModeActive() ? 1 : 0, g_controllerModeHooked ? 1 : 0,
                  static_cast<unsigned long long>(g_controllerFlagCleared), ReadGameControllerModeFlagImpl());
    Tf2VrLog(line);
}

std::atomic_bool g_kbuttonsEnabled{false};

void ToggleGameUi() {
    // SEND ESCAPE, DO NOT CALL gameui_activate.
    //
    // The thunks were found, matched their bytes and were called -- the log
    // shows gameui_activate and gameui_hide alternating on every press -- and
    // NO MENU APPEARED. So the command exists and does nothing: it is vestigial
    // Source plumbing in a game whose menus Respawn replaced with their own
    // Squirrel/RUI stack. A command being registered is not evidence that it is
    // still wired to anything, which is the lesson worth keeping.
    //
    // ESC is known to work, because it is what the player already uses, and it
    // TOGGLES -- so this needs no open/closed state of its own and cannot
    // desync the way the tracked flag could. The same SendInput path the menu
    // navigation uses.
    // ...AND NOW IT DOES NOT SEND ESC EITHER, because the pad sends START.
    //
    // Everything above stays true -- gameui_activate really is vestigial, ESC
    // really does toggle -- but a synthetic KEYSTROKE has a cost the note above
    // never accounted for: it tells the game the player just used a KEYBOARD.
    // The game tracks that (our own controller-mode work found the state it
    // really uses is an internal field we do not own), and it is the leading
    // explanation for the wearer's HUD ability reading UNBOUND and its button
    // going dead after a menu -- the game switched to keyboard bindings for an
    // ability that only has a controller one.
    //
    // Cyberpunk's port states the rule the hard way: inject a key only for an
    // action with no free pad button, and only a key the game binds to NOTHING.
    // ESC fails both halves. START is what a pad sends for this, the game binds
    // it already, and it cannot be mistaken for a keyboard.
    if (g_kbuttonsEnabled.load(std::memory_order_relaxed)) {
        SendKey(VK_ESCAPE);   // sets the suppression window itself
    }
    NoteMenuToggled();
    {
        char line[200];
        std::snprintf(line, sizeof(line),
                      "[TF2VR] vrinput: menu button -> %s; focus=%d, menu now believed "
                      "%s.\n",
                      g_kbuttonsEnabled.load(std::memory_order_relaxed)
                          ? "ESC (scan code, legacy route)"
                          : "START on the pad (no keystroke injected)",
                      GameHasFocus() ? 1 : 0, g_menuOpen ? "OPEN" : "CLOSED");
        Tf2VrLog(line);
    }
}

// RE-ASSERT EVERY FRAME, do not write only on the edge.
//
// Source's GetButtonBits clears the pressed/released edge bits each time it
// folds the buttons into the usercmd, and this engine is free to clear the held
// bit too. An edge-only writer cannot recover from that: it believes the key is
// still down, never writes again, and the button silently stops working after
// one frame. Setting bit 0 every frame is idempotent and costs nothing.
// THE KBUTTON ROUTE IS RETIRED. `input.kbuttons` puts it back in one line.
//
// This wrote the engine's own +jump / +duck / +attack state directly, in
// parallel with the synthetic pad, so every button arrived twice and each one
// needed a hand-written rule about which route won. Those rules are what broke
// B in the wearer's d-pad screen, and they needed an "is a game menu up"
// predicate that cannot see every game screen.
//
// Every action this route carried is already on the pad -- movement, jump,
// attack, ADS, reload, weapon switch, sprint and melee were pure duplicates;
// the turn buttons were already dead in both turn modes; snap turn writes view
// angles through the command wrapper and never used a kbutton. The one control
// it uniquely owned, the grips, is now LB/RB in xinput_pad.cpp.
//
// The reference mods settle the architecture: Halo MCC VR, BioShock and
// Cyberpunk all present ONE synthetic pad at the XInput seam and write no
// engine memory ("No engine memory is written" is BioShock's own note). Only
// CoD4 VR writes engine input structures, and it is a full source port that
// owns its engine. We are the plugin case, so we are the pad case.
//
// KEPT, NOT DELETED, FOR ONE RUN: a control that turns out to be missing is
// then an ini line rather than a rebuild. Once a headset run confirms nothing
// is lost, this flag, this branch and every Apply call go together.
void Apply(int index, bool wanted) {
    if (index < 0 || index >= kButtonCount) return;
    if (!g_kbuttonsEnabled.load(std::memory_order_relaxed)) {
        // Still tracked, so a re-arm mid-session starts from the truth and
        // ReleaseAll below stays meaningful.
        g_held[index] = wanted;
        return;
    }
    const bool wasHeld = g_held[index];
    int* state = StateWord(index);
    __try {
        if (wanted) {
            // The PRESSED edge only on the transition; held every frame.
            *state = (*state | kStateHeld) | (wasHeld ? 0 : kStatePressed);
        } else if (wasHeld) {
            // KeyUp: clear held and set the released edge, as the engine's own
            // handler does. Dropping that edge would leave anything watching for
            // a release waiting forever.
            *state = ((*state & ~kStateHeld) | kStateReleased);
        }
        g_held[index] = wanted;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Tf2VrLog("[TF2VR] vrinput: faulted writing a kbutton; disarming.\n");
        g_enabled = false;
    }
}

void ReleaseAll() {
    for (int i = 0; i < kButtonCount; ++i) Apply(i, false);
}

// Hysteresis: engage at the threshold, release at a fraction of it.
bool Latch(float value, float threshold, bool currentlyHeld) {
    const float release = threshold * kReleaseFactor;
    return currentlyHeld ? (value > release) : (value > threshold);
}

// ---------------------------------------------------------------------------
// MENU NAVIGATION.
//
// The menus are the engine's own UI and do not read kbuttons -- kbuttons are
// gameplay, folded into the usercmd. What the UI reads is KEY EVENTS, so that
// is what this sends: real keystrokes, through SendInput, exactly as if the
// arrow cluster had been pressed. No new engine surface, no hooks in the UI.
//
// Gated three ways, because a key injector that fires during gameplay would be
// far worse than no menu navigation at all:
//   1. the game-UI predicate must be TRUSTED (seen both true and false), and
//   2. it must currently say the menu is up, and
//   3. gameplay kbuttons are all released for as long as it is.
//
// Sticks REPEAT rather than firing once, because menu lists are long; first
// press is immediate, then it repeats while held.
constexpr float kMenuStickThreshold = 0.5f;
constexpr std::uint64_t kMenuRepeatFirstMs = 400;
constexpr std::uint64_t kMenuRepeatNextMs = 140;

// SCAN CODES, NOT VIRTUAL KEYS.
//
// The first version set only `wVk`, and the log proved it inert: five
// `menu button -> ESC` lines and no menu. The player's own ESC works, so the
// key path is fine -- what differed is WHAT a real keyboard sends. This engine
// reads raw input and keys off the SCAN CODE, the same reason its controller
// support is raw HID rather than XInput. A virtual-key-only injection has no
// scan code and is simply not seen.
//
// So this sends what the hardware sends: KEYEVENTF_SCANCODE with the scan code
// MapVirtualKey resolves, and KEYEVENTF_EXTENDEDKEY for the arrow cluster,
// which lives on the extended (E0-prefixed) part of the keyboard. Getting the
// extended flag wrong on the arrows would deliver the NUMPAD keys instead.
bool IsExtendedKey(unsigned vk) {
    switch (vk) {
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
        case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_INSERT: case VK_DELETE:
            return true;
        default:
            return false;
    }
}

// A ZERO-LENGTH PRESS IS NOT A PRESS TO EVERY CONSUMER.
//
// This used to send down and up in ONE SendInput call, so the key was down for
// approximately no time at all. That is enough for anything reading key EVENTS
// -- menu selection worked -- and can be missed entirely by anything that
// samples key STATE once a frame. Which is exactly the split that was reported:
// A activated menu items, but the post-load panel would not dismiss, while a
// real keyboard press did both. A human holds a key for 50-100ms.
//
// So the release is now SCHEDULED rather than sent immediately, and the game
// sees the key held across several frames the way it would from hardware.
// Nothing blocks: the release is delivered by a later tick.
constexpr std::uint64_t kKeyHoldMs = 90;

struct PendingRelease {
    WORD scan = 0;
    DWORD extended = 0;
    std::uint64_t dueAt = 0;
    bool active = false;
};
PendingRelease g_pendingReleases[4];

void ReleaseScan(WORD scan, DWORD extended) {
    INPUT up{};
    up.type = INPUT_KEYBOARD;
    up.ki.wScan = scan;
    up.ki.dwFlags = KEYEVENTF_SCANCODE | extended | KEYEVENTF_KEYUP;
    SendInput(1, &up, sizeof(INPUT));
}

// Called every tick. Also flushes on shutdown paths, so a key cannot be left
// stuck down if the plugin stops ticking with one outstanding.
void AdvancePendingReleases(bool flushAll) {
    const std::uint64_t now = GetTickCount64();
    for (auto& pending : g_pendingReleases) {
        if (!pending.active) continue;
        if (!flushAll && now < pending.dueAt) continue;
        ReleaseScan(pending.scan, pending.extended);
        pending.active = false;
    }
}

void SendKey(unsigned vk) {  // forward-declared above
    const UINT scan = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    if (!scan) return;
    const DWORD extended = IsExtendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0u;
    // SUPPRESS HERE, once, covering both the queued and immediate paths. Our
    // injected Escape is a real key event, so the physical-Escape watcher sees
    // it and would count it as the player's. The window must outlast the HOLD
    // below, which it does comfortably.
    if (vk == VK_ESCAPE) g_suppressEscapeUntil = GetTickCount64() + 300 + kKeyHoldMs;
    INPUT down{};
    down.type = INPUT_KEYBOARD;
    down.ki.wVk = 0;                      // scan code only, as hardware does
    down.ki.wScan = static_cast<WORD>(scan);
    down.ki.dwFlags = KEYEVENTF_SCANCODE | extended;
    SendInput(1, &down, sizeof(INPUT));

    // Queue the release. If every slot is busy -- which would mean four keys
    // injected inside 90ms -- release this one immediately rather than leaking
    // a key that is never lifted.
    for (auto& pending : g_pendingReleases) {
        if (pending.active) continue;
        pending.scan = static_cast<WORD>(scan);
        pending.extended = extended;
        pending.dueAt = GetTickCount64() + kKeyHoldMs;
        pending.active = true;
        return;
    }
    ReleaseScan(static_cast<WORD>(scan), extended);
}

// WHOSE WINDOW HAS FOCUS. Logged beside every injected key, because
// "SendInput did nothing" has two very different causes and this separates
// them: input going to ANOTHER window (focus lost to the desktop, the VR
// overlay, or a mirror) versus input reaching the game and being ignored.
// Without this the next run would be another guess.
bool GameHasFocus() {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId();
}

// One direction's worth of repeat state.
struct Repeat {
    bool held = false;
    std::uint64_t nextAt = 0;
};

bool RepeatFires(Repeat& state, bool active, std::uint64_t now) {
    if (!active) { state.held = false; return false; }
    if (!state.held) {
        state.held = true;
        state.nextAt = now + kMenuRepeatFirstMs;
        return true;
    }
    if (now >= state.nextAt) {
        state.nextAt = now + kMenuRepeatNextMs;
        return true;
    }
    return false;
}

}  // namespace


// PLAN-VRMENU 2.4. Exposed, not duplicated: GameMenuIsOpen is the SAME
// predicate the menu-mode branch above uses, so the config panel and the
// suppression path can never disagree about whether a menu is up.
bool GameMenuIsOpen() { return MenuIsOpen(); }

void SendGameMenuToggle() { ToggleGameUi(); }

bool ConsumeThumbClicks() { return g_consumeThumbClicks; }


// The TRACKED menu belief, flipped by the menu button and by a physical Escape.
// Paired with GameMenuIsOpen(): the cursor test that backs the latter was
// measured dead in VR, so anything gated on the cursor alone gates away the
// buttons that menu needs. Neither is reliable alone.
bool GameMenuBelievedOpen() { return g_menuOpen; }
bool EnsureVrInputResolved() {
    if (g_resolved) return g_client != nullptr;
    HMODULE client = GetModuleHandleA("client.dll");
    // NOT a failure to latch. The ini is applied when the plugin loads, which is
    // BEFORE client.dll is in the process -- so the first attempt always finds
    // nothing, and latching it meant the buttons never resolved for the whole
    // session and every press did nothing. Only a PROLOGUE mismatch is a real
    // refusal; a missing module is just "not yet".
    if (!client) return false;
    g_resolved = true;
    auto* base = reinterpret_cast<std::uint8_t*>(client);

    for (int i = 0; i < kButtonCount; ++i) {
        if (std::memcmp(base + kButtons[i].handlerRva, kKeyDownPrologue,
                        sizeof(kKeyDownPrologue)) != 0) {
            char line[224];
            std::snprintf(line, sizeof(line),
                          "[TF2VR] vrinput: %s handler at client.dll+0x%06llX is not the KeyDown "
                          "shape this was built against. Refusing to drive ANY button.\n",
                          kButtons[i].name,
                          static_cast<unsigned long long>(kButtons[i].handlerRva));
            Tf2VrLog(line);
            return false;
        }
    }
    g_client = base;
    {
        char line[160];
        std::snprintf(line, sizeof(line),
                      "[TF2VR] vrinput: all %d kbutton handlers matched; input is drivable.\n",
                      kButtonCount);
        Tf2VrLog(line);
    }
    return true;
}

void SetVrInputEnabled(bool enabled) {
    // Record the INTENT even when the engine is not up yet. The ini is applied
    // at plugin load, before client.dll is in the process, so the first attempt
    // can only fail -- and the first version treated that as a refusal and gave
    // up for the whole session. Every button press then did nothing, with one
    // line in the log an hour earlier as the only sign.
    g_wanted = enabled;
    if (enabled && !EnsureVrInputResolved()) {
        Tf2VrLog("[TF2VR] vrinput: wanted, but client.dll is not loaded yet. It will arm itself "
                 "as soon as it is.\n");
        return;
    }
    if (!enabled && g_enabled) ReleaseAll();
    g_enabled = enabled;
    Tf2VrLog(enabled ? "[TF2VR] vrinput: ARMED. Left stick moves, right stick turns, right stick "
                       "up/down jumps and crouches, right trigger fires, right B reloads.\n"
                     : "[TF2VR] vrinput: disarmed; every button released.\n");
}

bool IsVrInputEnabled() { return g_enabled; }

// The SAME latch that drives +zoom, not a second read of the trigger. A second
// threshold test on the same axis would sit on the other side of the hysteresis
// for a frame or two at every press, and the brace would engage on a different
// frame from the command -- which is a clock split between two things that must
// be simultaneous by definition.
bool IsAdsHeld() { return g_enabled && g_held[Ads]; }

// THE INTENT, not the achieved state, and the registry needs exactly this one.
//
// SetVrInputEnabled records g_wanted and then returns EARLY if client.dll is
// not in the process yet -- which is always true when the ini is applied at
// plugin load. So g_enabled reads 0 for a session that is correctly armed and
// merely waiting for the module. A registry getter reading g_enabled would
// report "off", and anything that then wrote that value back would clear
// g_wanted and disarm the controllers for the whole session.
bool IsVrInputWanted() { return g_wanted; }

void ToggleVrInput() { SetVrInputEnabled(!g_enabled); }

void AdvanceVrInput() {
    // Arm late. The ini asked for this before the engine existed.
    if (g_wanted && !g_enabled) {
        static std::uint64_t lastTry = 0;
        const std::uint64_t tick = GetTickCount64();
        if (tick - lastTry >= 500) {
            lastTry = tick;
            if (EnsureVrInputResolved()) SetVrInputEnabled(true);
        }
    }
    if (!g_enabled || !g_client) return;

    // THE MENU OWNS THE CONTROLLER WHILE IT IS UP.
    //
    // Everything below this drives gameplay, and none of it should reach the
    // game while the player is in a menu -- the sticks would walk the pilot
    // around behind the pause screen. Releasing first also means the release
    // edges are delivered properly rather than the buttons being left stuck
    // down for as long as the menu is open.
    // OBSERVE THE PREDICATE EVERY TICK, act on it only once it is trusted.
    //
    // This was `GameUiQueryTrusted() && GameUiVisible()`, which deadlocked: the
    // trust gate short-circuits, so the query was never CALLED, so it could
    // never be seen both ways, so it could never become trusted. The log gave
    // it away by containing no predicate lines at all -- not a wrong value, no
    // value. Observation and action have to be separate.
    // Installed from the tick rather than at load: the interface does not exist
    // until client.dll has constructed it, and a null there is "not yet", not a
    // failure. Cheap once hooked -- it returns immediately.
    AdvancePendingReleases(false);
    ApplyControllerModeHook();
    PollPhysicalEscape();
    // ABOVE the menu branch on purpose: that branch returns early, so logging
    // below it would never sample while the menu was open -- which is the only
    // state worth sampling.
    {
        static std::uint64_t lastCandidateLog = 0;
        const std::uint64_t nowTick = GetTickCount64();
        if (nowTick - lastCandidateLog >= 1000) {
            lastCandidateLog = nowTick;
            LogMenuCandidates();
        }
    }
    // ONE EDGE DETECTOR FOR THE MENU BUTTON, ABOVE THE BRANCH.
    //
    // There used to be two -- one in the gameplay path, one in the menu path --
    // each with its own `held` state, and that is a double-fire waiting to
    // happen: press it in gameplay, it fires and the menu opens, the cursor
    // appears, and on the NEXT tick the menu branch takes over, sees the button
    // still physically down against its own stale held=false, calls that a
    // fresh press and fires again. The menu flashes open and shut, and the
    // second press works because by then the two states agree.
    //
    // Reported as exactly that, and reproducible by exiting with B and pressing
    // menu again -- reopening while warm makes the cursor appear inside the
    // button hold. A signal that is read on both sides of a mode switch needs
    // ONE piece of state, held outside the switch.
    static bool menuButtonHeld = false;
    const bool menuButtonNow = g_controllerMenu != 0;
    const bool menuButtonPressed = menuButtonNow && !menuButtonHeld;
    menuButtonHeld = menuButtonNow;

    // A BUTTON STILL HELD WHEN THE MENU CLOSES IS NOT A GAMEPLAY PRESS.
    //
    // B exits the menu, and B is RELOAD in gameplay -- so the moment the menu
    // shut, the gameplay branch saw B down on the very next tick and reloaded
    // the weapon. Reported as "using B to exit the menu also reloads the gun".
    //
    // Same class as the menu-button double-fire above: one physical hold being
    // read by both sides of a mode switch. The cure is the same in spirit --
    // whatever is still down at the transition is CONSUMED, and does not count
    // again until it has actually been released.
    // OUR PANEL IS ITS OWN TERM, and it is NOT folded into MenuIsOpen().
    //
    // PLAN-VRMENU 2.4 proposed MenuIsOpen() becoming GameMenuIsOpen() ||
    // VrMenuIsOpen() -- one predicate, one branch. That was written on the
    // assumption that MenuIsOpen() works. The 2026-08-24 VR run showed it does
    // not: it is a cursor-visibility test, the cursor is not shown while the
    // pause menu is up in VR, and every log line from that run read "game menu
    // down" while the wearer was looking at the pause menu.
    //
    // So the two terms are kept SEPARATE and both are logged. Our own panel
    // state is a flag we set and can trust; MenuIsOpen() is a measurement of
    // something else that may or may not be firing. Folding a trustworthy term
    // into an untrustworthy one would have hidden exactly the failure that run
    // found.
    const bool vrPanelOpen = IsMenuOpen();
    // THE CURSOR TERM IS IGNORED WHILE OUR PANEL IS OR HAS BEEN UP.
    //
    // MenuIsOpen() is a cursor-visibility test and it is not trustworthy in
    // either direction: it misses the VR pause menu entirely (measured
    // 2026-08-24), and it FALSELY reports a menu after our panel has been
    // opened, because the ImGui backend had been changing the OS cursor. That
    // false positive suppressed every gameplay input while the panel itself kept
    // working, which is a uniquely confusing failure.
    //
    // NoMouseCursorChange now stops the panel disturbing the cursor at all. This
    // is the belt to that braces: our own flag is authoritative for our own
    // panel, and the cursor is only consulted for the GAME's menus.
    const bool inMenu = MenuIsOpen() || vrPanelOpen;
    {
        static int lastTerms = -1;
        const int terms = (MenuIsOpen() ? 1 : 0) | (vrPanelOpen ? 2 : 0);
        if (terms != lastTerms) {
            lastTerms = terms;
            char line[240]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] input gate: game_menu(cursor)=%d vr_panel=%d -> gameplay input %s\n",
                (terms & 1) ? 1 : 0, (terms & 2) ? 1 : 0,
                terms ? "SUPPRESSED" : "live");
            Tf2VrLog(line);
        }
    }
    const std::uint8_t rightButtons = g_controllerButtons[1];
    if (g_wasInMenu && !inMenu) {
        g_consumeRightPrimary = (rightButtons & kControllerPrimaryButton) != 0;
        g_consumeRightSecondary = (rightButtons & kControllerSecondaryButton) != 0;
    }
    g_wasInMenu = inMenu;
    // Released, so it may count again.
    if (!(rightButtons & kControllerPrimaryButton)) g_consumeRightPrimary = false;
    if (!(rightButtons & kControllerSecondaryButton)) g_consumeRightSecondary = false;

    // THE CHORD IS READ ABOVE THE MENU BRANCH, AND THAT PLACEMENT IS THE WHOLE
    // POINT. Opening our panel opens the game's menu (that is what pauses it),
    // which makes inMenu true -- and the inMenu branch below RETURNS EARLY. A
    // chord detector sitting after that return would be reachable only while the
    // panel is closed: the wearer could summon the panel and then never dismiss
    // it, with no way out but alt-tab. Read it before anything can return.
    // ---------------------------------------------------------------------
    // THE L3+R3 SUMMON CHORD. PLAN-VRMENU 2.4, and Halo's detector.
    //
    // THE HAZARD IS SPECIFIC AND ALREADY COST THIS PROJECT ONCE. CONTROLS-2026-
    // 08-21: L3 is +speed (sprint) and R3 is +melee. So the chord that opens the
    // menu is made of TWO LIVE GAMEPLAY BUTTONS. A naive implementation sprints
    // and melees on the way in, and again on the way out -- which is the same
    // double-fire defect this file has already fixed twice for other buttons.
    //
    // The cure is Halo's, and one struct answers three questions at once:
    //
    //   250 ms window     both clicks must land close together, so a deliberate
    //                     sprint followed later by a melee is not a summon.
    //   latch once        the chord fires on the frame it completes and not
    //                     again while it is held.
    //   consume until
    //   BOTH released     sprint and melee are suppressed for the whole gesture,
    //                     entry and exit, so neither leaks into the game.
    //
    // The consume flag is what makes the dismiss side symmetric for free: the
    // same chord closes the panel and runs the same suppression on the way out.
    // "Never leave a reset half-armed."
    // THE L3+R3 SUMMON CHORD. NO TIMING GATE ANYWHERE ON THIS PATH.
    //
    // An earlier version held the first click for 250 ms to see whether a
    // partner arrived. That was rejected outright and rightly: a quarter second
    // of latency on melee and sprint is unacceptable, and no menu convenience
    // buys it.
    //
    // So the consume is set ONLY on the frame the chord actually latches, which
    // is the frame the SECOND click goes down. It costs nothing, because it is
    // never consulted before that frame:
    //
    //   first click   fires sprint or melee IMMEDIATELY, exactly as it always
    //                 did. There is no way to un-fire it and no attempt is made.
    //   second click  lands on the same tick the chord latches, and this block
    //                 runs ABOVE Apply(Melee)/Apply(Sprint), so it is suppressed
    //                 in full -- press and hold both.
    //   release       stays consumed until BOTH are up, so letting go does not
    //                 fire either.
    //
    // WHICH MEANS THE ORDER MATTERS, and it is worth knowing rather than
    // guessing at: LEAD WITH L3. A leaked L3 is a brief sprint, which is
    // harmless. A leaked R3 is a melee swing, which is not. Leading with L3
    // means the only click that ever reaches the game is the harmless one.
    {
        static std::uint64_t leftClickAt = 0;
        static std::uint64_t rightClickAt = 0;
        static bool leftHeld = false;
        static bool rightHeld = false;
        static bool latched = false;
        static bool consumeClicks = false;
        constexpr std::uint64_t kChordWindowMs = 400;

        const bool leftNow = g_controllerThumbstickClick[0] != 0;
        const bool rightNow = g_controllerThumbstickClick[1] != 0;
        const std::uint64_t nowMs = GetTickCount64();
        if (leftNow && !leftHeld) leftClickAt = nowMs;
        if (rightNow && !rightHeld) rightClickAt = nowMs;
        leftHeld = leftNow;
        rightHeld = rightNow;

        if (leftNow && rightNow && !latched) {
            // 400 ms rather than 250: widening the PAIRING window costs nothing
            // -- it changes only whether the chord forms, never when a click
            // fires -- and the wearer reported the chord itself was fiddly.
            const std::uint64_t gap = leftClickAt > rightClickAt ? leftClickAt - rightClickAt
                                                                 : rightClickAt - leftClickAt;
            if (gap <= kChordWindowMs) {
                latched = true;
                consumeClicks = true;
                ToggleMenu();
                char line[300]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] MENU CHORD: L3+R3 within %llu ms -> panel %s. The SECOND click and "
                    "the release are consumed; the first already fired (lead with L3 and that is "
                    "a sprint, not a melee).\n",
                    static_cast<unsigned long long>(gap), IsMenuOpen() ? "OPEN" : "closed");
                Tf2VrLog(line);
            }
        }
        // Both up: the gesture is over, and only then may these count as
        // gameplay again. Releasing one is not enough -- the other is still
        // part of the chord.
        if (!leftNow && !rightNow) {
            latched = false;
            consumeClicks = false;
        }
        g_consumeThumbClicks = consumeClicks;
    }

    if (inMenu) {
        ReleaseAll();
        // SS 2.4.5. WHILE OUR OWN PANEL IS UP, THE STICK MUST NOT SendKey.
        //
        // This branch synthesises VK_UP/DOWN/LEFT/RIGHT/SPACE/ESCAPE for the
        // GAME's menu. Our config panel is drawn ON TOP of that menu -- opening
        // it is what pauses the game -- so leaving this armed would drive the
        // pause menu underneath while the wearer thinks they are moving a
        // slider. That is the exact accidental navigation this was asked to
        // prevent, and it would be invisible until something had been changed.
        //
        // The stick terminates in ImGui's own nav instead. The edge detectors
        // below still RUN and are discarded, which is the existing discipline.
        const bool navKeysGoToGame = g_keyboardMenuNav && !IsMenuOpen();
        // LOG WHICH TERM DECIDED IT, on change. A predicate that widens can
        // otherwise change meaning silently.
        {
            static int lastState = -1;
            const int state = (g_keyboardMenuNav ? 1 : 0) | (IsMenuOpen() ? 2 : 0);
            if (state != lastState) {
                lastState = state;
                char line[260]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] menu nav: keyboard_menu_nav=%d vr_panel_open=%d -> stick %s\n",
                    g_keyboardMenuNav ? 1 : 0, IsMenuOpen() ? 1 : 0,
                    navKeysGoToGame ? "drives the GAME menu (SendKey)"
                                    : "does NOT SendKey (ImGui nav owns it)");
                Tf2VrLog(line);
            }
        }
        const std::uint64_t now = GetTickCount64();
        // BOTH sticks, as asked: whichever hand the wearer reaches for.
        const float navX = std::fabs(g_controllerThumbstick[0][0]) > std::fabs(g_controllerThumbstick[1][0])
                               ? g_controllerThumbstick[0][0] : g_controllerThumbstick[1][0];
        const float navY = std::fabs(g_controllerThumbstick[0][1]) > std::fabs(g_controllerThumbstick[1][1])
                               ? g_controllerThumbstick[0][1] : g_controllerThumbstick[1][1];
        static Repeat up, down, left, right;
        if (navKeysGoToGame) {
            if (RepeatFires(up, navY > kMenuStickThreshold, now)) SendKey(VK_UP);
            if (RepeatFires(down, navY < -kMenuStickThreshold, now)) SendKey(VK_DOWN);
            if (RepeatFires(right, navX > kMenuStickThreshold, now)) SendKey(VK_RIGHT);
            if (RepeatFires(left, navX < -kMenuStickThreshold, now)) SendKey(VK_LEFT);
        } else {
            // The edge detectors still have to RUN, or the first press after
            // switching back reads as a fresh edge from a stick that has been
            // held the whole time. Stepped, discarded.
            (void)RepeatFires(up, navY > kMenuStickThreshold, now);
            (void)RepeatFires(down, navY < -kMenuStickThreshold, now);
            (void)RepeatFires(right, navX > kMenuStickThreshold, now);
            (void)RepeatFires(left, navX < -kMenuStickThreshold, now);
        }

        // A selects, B goes back. Both on the EDGE -- a held button must not
        // repeat a selection.
        const std::uint8_t rbMenu = g_controllerButtons[1];
        static bool selectHeld = false, backHeld = false;
        const bool selectNow = (rbMenu & kControllerPrimaryButton) != 0;
        const bool backNow = (rbMenu & kControllerSecondaryButton) != 0;
        // A SELECTS -- and it sends SPACE, not ENTER.
        //
        // ENTER was the original choice and it is subtly wrong: on the
        // post-load "press A to continue" screen it ADVANCES the game but does
        // not HIDE the panel, which is exactly the reported symptom -- entering
        // the game with the screen still laid over it. The keyboard SPACE does
        // both, which is why pressing SPACE has always worked.
        //
        // The earlier attempt to fix this put SPACE in the GAMEPLAY section,
        // below the early return this branch takes, so it never ran: the log
        // showed "A -> SPACE" firing zero times while the cursor was showing
        // throughout the menu and load. The binding was in the wrong branch,
        // not the wrong key.
        //
        // SPACE is now the single accept key on A in both branches, so there is
        // one behaviour rather than two that differ by which screen is up. If
        // this turns out not to activate ordinary menu items, the fix is to
        // send BOTH here -- but one key that works everywhere is worth trying
        // before two that need a screen to be told apart.
        if (navKeysGoToGame) {
            if (selectNow && !selectHeld) SendKey(VK_SPACE);
            if (backNow && !backHeld) SendKey(VK_ESCAPE);
        }
        // Tracked either way, for the same reason the repeats above are stepped:
        // a button held across the switch must not read as a new press.
        selectHeld = selectNow;
        backHeld = backNow;

        // The menu button closes the menu, rather than toggling into it again.
        // Shares the single edge above; it must not keep its own.
        if (menuButtonPressed) ToggleGameUi();
        return;
    }

    const float lx = g_controllerThumbstick[0][0];
    const float ly = g_controllerThumbstick[0][1];
    const float rx = g_controllerThumbstick[1][0];
    const float ry = g_controllerThumbstick[1][1];
    const float rt = g_controllerTrigger[1];
    const std::uint8_t rb = g_controllerButtons[1];
    // Beside rb, because both are now read on the face-button paths above and
    // below; one declaration cannot go stale against the other.
    const std::uint8_t lb = g_controllerButtons[0];

    // Left stick: move. Y is forward on OpenXR's thumbstick convention.
    //
    // UNLESS THE STICK IS CURRENTLY A D-PAD. While the gesture is engaged the
    // wearer is answering a prompt, not walking, and every movement command is
    // released rather than frozen -- Apply(false) writes the release, so the
    // pilot stops instead of running on with a latched kbutton for as long as
    // the mode is held. The pad route zeroes the analogue axes for the same
    // window; both halves are needed, because either one alone still moves.
    //
    // Nothing here is withheld when the gesture is NOT engaged: no latency, no
    // threshold change, no sampling difference. The stick reaches movement on
    // the same frame it always did.
    const bool dpadOwnsStick = DpadGestureEngaged();
    Apply(Forward, !dpadOwnsStick && Latch(ly, kMoveDeadzone, g_held[Forward]));
    Apply(Back, !dpadOwnsStick && Latch(-ly, kMoveDeadzone, g_held[Back]));
    Apply(MoveRight, !dpadOwnsStick && Latch(lx, kMoveDeadzone, g_held[MoveRight]));
    Apply(MoveLeft, !dpadOwnsStick && Latch(-lx, kMoveDeadzone, g_held[MoveLeft]));

    // THE RIGHT STICK'S TWO AXES, GATED AGAINST EACH OTHER.
    //
    // Turn is on X, jump/crouch on Y, and a plain per-axis threshold lets one
    // diagonal push satisfy both -- which is the wearer's report from both
    // sides: turning clips a crouch, and crouching clips a turn.
    //
    // The gate is AXIS DOMINANCE and it is computed ONCE, here, above every
    // consumer. An axis counts only if its own deflection is at least
    // stick_cross_ratio times the other's. That leaves a wide diagonal band
    // where BOTH still fire, deliberately: the ask was for a concerted push, not
    // for simultaneous turn-and-crouch to be taken away.
    //
    // Hysteresis on the RATIO as well as on the magnitude. Without it a stick
    // resting near the cone boundary would chatter the gate at 84 Hz while the
    // magnitude latch sat perfectly still, which would look like the stick
    // itself flickering.
    const float turnDeadzone = g_turnDeadzone.load(std::memory_order_acquire);
    const float jumpCrouchDeadzone = g_jumpCrouchDeadzone.load(std::memory_order_acquire);
    const float crossRatio = g_stickCrossRatio.load(std::memory_order_acquire);
    const float absRx = rx < 0.0f ? -rx : rx;
    const float absRy = ry < 0.0f ? -ry : ry;
    const bool turnWasHeld = g_held[TurnLeft] || g_held[TurnRight];
    const bool vertWasHeld = g_held[Jump] || g_crouchStickHeld;
    // A gate already open is judged more leniently than one being opened, the
    // same shape as Latch's release threshold.
    const float horizGate = turnWasHeld ? crossRatio * kReleaseFactor : crossRatio;
    const float vertGate = vertWasHeld ? crossRatio * kReleaseFactor : crossRatio;
    const bool horizDominant = absRx >= horizGate * absRy;
    const bool vertDominant = absRy >= vertGate * absRx;

    // Right stick X: turn, through the engine's own turn-rate path, so it
    // rotates the BODY -- which is what head tracking is already composed
    // against and therefore does not fight it.
    // SMOOTH OR SNAP, and both drive the engine's own +left / +right.
    //
    // Smooth is the stick held, exactly as before. Snap is the same button held
    // for a measured time: degrees / yawspeed seconds covers the angle, because
    // that path turns at cl_yawspeed by definition. The stick must return to
    // centre before another snap, so a held stick gives one turn rather than a
    // spin.
    const float snapDegrees = g_snapTurnDegrees.load(std::memory_order_acquire);
    const bool snapMode = g_turnMode.load(std::memory_order_acquire) != 0;
    bool turnRight = false;
    bool turnLeft = false;
    if (!snapMode) {
        // THE KBUTTON TURN IS OFF IN SMOOTH MODE, and that is what gives the
        // stick back its analogue feel.
        //
        // +left/+right are DIGITAL: on or off, at whatever fixed rate the engine
        // uses. The synthetic pad forwards the stick as an ANALOGUE axis. With
        // both live the digital half swamped the analogue one, so every push past
        // the deadzone turned at the same speed no matter how far it went -- "we've
        // lost the ability to control the turn speed by how far you push the
        // stick".
        //
        // One owner per action, which is the rule this file already states for the
        // offhands. The pad owns smooth turning; it is the only one of the two
        // that can express how far the stick moved.
    } else {
        // A ONE-SHOT VIEW-ANGLE WRITE, not a held turn button.
        //
        // The first version held the engine's +left/+right for degrees/yawspeed
        // seconds. That is a fast SWEEP -- about 17 frames at 80 Hz for 45
        // degrees -- and it is exactly what snap turn exists to avoid. The whole
        // purpose is to remove the optical flow that causes the vestibular
        // mismatch, so a quick smooth turn can make a motion-sick player worse.
        //
        // RequestSnapYaw writes worldViewAngles once and the engine keeps it, so
        // the turn happens between two frames with nothing rendered in between.
        static bool snapArmed = true;
        const bool pushedRight = horizDominant && rx > turnDeadzone;
        const bool pushedLeft = horizDominant && rx < -turnDeadzone;
        if (!pushedRight && !pushedLeft) snapArmed = true;
        if (snapArmed && (pushedRight || pushedLeft)) {
            snapArmed = false;
            if (SnapTurnAvailable()) {
                RequestSnapYaw(pushedRight ? -snapDegrees : snapDegrees);
            } else {
                // Said once per flick rather than never: a snap that silently
                // does nothing is indistinguishable from one that is off.
                Tf2VrLog("[TF2VR] SNAP TURN requested, but the command wrapper is not installed, "
                         "so nothing was written. Snap turning needs it.\n");
            }
        }
        // The engine's turn buttons are NOT used in snap mode at all.
    }
    Apply(TurnRight, turnRight);
    Apply(TurnLeft, turnLeft);

    // RIGHT STICK Y IS NO LONGER JUMP.
    //
    // A already jumps, so the stick was a second way to do something that had a
    // perfectly good button -- and it was spending the only axis left on the
    // controller. The wearer, after a long session: "I'm no longer thinking that
    // UP for jump works well. We already have A for jump".
    //
    // The axis now drives the PAD'S D-PAD instead, which is what the game's
    // two-option prompts read. Up is d-pad up and nothing else; down is d-pad
    // down AND crouch, deliberately double-mapped -- the prompts and crouch never
    // appear at the same moment, and crouch is worth keeping on the stick.
    //
    // The d-pad half is reported by the synthetic pad; see xinput_pad.cpp. Jump
    // is left entirely to A.
    Apply(Jump, false);

    // CROUCH. ONE edge detector, computed here, ABOVE the mode branch -- see the
    // note beside g_crouchStickHeld for why it is not g_held[Duck].
    //
    // Every mode drives EXACTLY ONE of the two commands and explicitly releases
    // the other, so a mode change cannot leave a kbutton asserted by a mode that
    // is no longer running.
    // CROUCH IS ON B NOW, NOT THE STICK. The wearer's remap, and it also removes
    // the last reason the stick's vertical axis touches anything but a prompt.
    //
    // Everything downstream of this boolean is unchanged: the hold/toggle modes,
    // the edge detector and the mode-reporting line all key off it, so moving
    // the SOURCE moves the whole mechanism and cannot leave a kbutton asserted
    // by a path that is no longer running.
    //
    // The consume guard comes with it. Right-secondary is part of a chord, and
    // the chord suppressed reload for exactly the reason it must now suppress
    // crouch: a chord press is not a gameplay press.
    const bool crouchStickNow =
        (rb & kControllerSecondaryButton) != 0 && !g_consumeRightSecondary;
    const bool crouchStickEdge = crouchStickNow && !g_crouchStickHeld;
    g_crouchStickHeld = crouchStickNow;
    // THE CROUCH EDGE, and it exists because nothing else in a run says which
    // mode is live. The mode is a code default unless the ini names it, so
    // SetCrouchMode may never be called and never log -- a run would then be
    // testing a crouch mechanism it cannot name afterwards.
    //
    // Printed on the edge, because a stick push is transient and a once-a-second
    // sample reads 0.00 through all of them. It carries BOTH kbutton state words
    // so the three ways this fails are separable from one run:
    //
    //   mode 1, toggleduck asserts, wearer does not stay crouched
    //       -> +toggle_duck is not the command we think it is; switch to mode 2,
    //          which is one ini digit and no rebuild
    //   mode 1, toggleduck state word stays 0
    //       -> we are not driving it at all; a resolve or Apply problem, and
    //          nothing to do with the engine's toggle
    //   edge never prints
    //       -> the stick is not reaching the deadzone, and crouch is not the
    //          question yet
    if (crouchStickEdge || (!crouchStickNow && g_crouchEdgeWasDown)) {
        const int mode = g_crouchMode.load(std::memory_order_acquire);
        const int* duckState = g_client ? StateWord(Duck) : nullptr;
        const int* toggleState = g_client ? StateWord(ToggleDuck) : nullptr;
        char line[400]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] CROUCH EDGE: stick %s (ry=%.2f rx=%.2f | deadzone %.2f | vertDominant=%d at "
            "ratio %.2f) | mode=%d (%s) | +duck state=%d +toggle_duck state=%d\n",
            crouchStickNow ? "DOWN" : "released", static_cast<double>(-ry),
            static_cast<double>(rx), static_cast<double>(jumpCrouchDeadzone),
            vertDominant ? 1 : 0, static_cast<double>(crossRatio), mode,
            mode == 0 ? "hold" : (mode == 2 ? "our toggle over +duck"
                                            : "engine's own +toggle_duck"),
            duckState ? *duckState : -1, toggleState ? *toggleState : -1);
        Tf2VrLog(line);
    }
    g_crouchEdgeWasDown = crouchStickNow;

    switch (g_crouchMode.load(std::memory_order_acquire)) {
        case 0:
            // HOLD. The original behaviour, and the engine's #CROUCH command.
            g_crouchLatched = false;
            Apply(ToggleDuck, false);
            Apply(Duck, crouchStickNow);
            break;
        default:
            // 1 = THE ENGINE'S OWN TOGGLE, and the default. +toggle_duck is the
            // game's #TOGGLE_CROUCH -- the same command its own Hold/Toggle
            // option selects -- so the toggling, and every interaction with
            // slide, wallrun and titan entry, stays the engine's own.
            //
            // Driven with exactly the shape +duck is driven with: asserted while
            // the stick is down, released when it centres. The engine acts on
            // the edge; we do not toggle anything ourselves, which is why
            // g_crouchLatched is untouched here.
            g_crouchLatched = false;
            Apply(Duck, false);
            Apply(ToggleDuck, crouchStickNow);
            break;
        case 2:
            // 2 = OUR OWN TOGGLE, over the plain +duck command. The fallback, and
            // it exists to save a headset run: if mode 1 turns out not to toggle,
            // this is one INI digit rather than a rebuild.
            if (crouchStickEdge) g_crouchLatched = !g_crouchLatched;
            Apply(ToggleDuck, false);
            Apply(Duck, g_crouchLatched);
            break;
    }

    Apply(Attack, Latch(rt, kTriggerThreshold, g_held[Attack]));

    // AIM DOWN SIGHTS on the LEFT trigger, as asked for. It was the one input
    // on either controller with nothing bound to it, so this displaces nothing.
    //
    // Same hysteresis latch as the right trigger and the grips: an analogue
    // axis resting near the line would otherwise chatter the command at 84 Hz.
    Apply(Ads, Latch(g_controllerTrigger[0], kTriggerThreshold, g_held[Ads]));
    // RELOAD MOVES TO X (left primary), off B, which is crouch now.
    //
    // X already reaches the game through the pad as use/interact and that stays:
    // the two are not in conflict, and the note above the pad's face-button block
    // records why a control is never taken away merely for being double-routed.
    // So X is use AND reload, which is what the wearer asked for and is also the
    // flat game's own pairing on that button.
    Apply(Reload, (lb & kControllerPrimaryButton) != 0);

    // THE REST OF THE PILOT'S KIT, as asked for.
    //
    //   R3 (right stick click)   melee
    //   L3 (left stick click)    sprint
    //   right grip               ordnance / grenade
    //   other grip               tactical ability / shield
    //   Y (left secondary)       switch weapons
    //
    // The grips are analogue on Touch, so they get the same hysteresis latch
    // the trigger uses rather than a bare threshold -- a grip resting near the
    // line would otherwise chatter the command on and off every frame.

    // Suppressed for the whole chord gesture, which is what stops the summon
    // from sprinting and meleeing on its way in and out.
    // The consume is FALSE until the chord latches, so the first click of a
    // gesture fires with no delay whatsoever. Only the second click and the
    // release are suppressed.
    Apply(Melee, g_controllerThumbstickClick[1] != 0 && !g_consumeThumbClicks);
    Apply(Sprint, g_controllerThumbstickClick[0] != 0 && !g_consumeThumbClicks);
    // THE GRIP EDGE, WITH BOTH HANDS' SQUEEZE AT THAT INSTANT.
    //
    // The wearer reports that either grip fires BOTH ordnance and tactical. The
    // once-a-second sample lines cannot answer it -- a press is transient and
    // every one of them read 0.00 -- so the reading is taken on the edge, which
    // is the only moment that means anything.
    //
    // Read it like this: LEFT press with squeeze L=0.9 R=0.0 and both latches
    // going true means the fault is OURS, below this line. L=0.9 R=0.9 means the
    // runtime is reporting one hand's grip on both subaction paths and the fault
    // is above us.
    {
        // WHICH GRIP IS WHICH, fixed. The commands are settled by kb_act.lst --
        // +offhand0 IS the grenade, +offhand1 IS the tactical ability -- and the
        // RIGHT grip feeds the grenade, beside the gun hand. Was selectable via
        // input.grenade_hand; a player who wants it the other way round rebinds
        // the shoulders in the game.
        const float squeezeOrdnance = g_controllerSqueeze[1];
        const float squeezeTactical = g_controllerSqueeze[0];
        const float squeezeL = g_controllerSqueeze[0];
        const float squeezeR = g_controllerSqueeze[1];
        const bool wantOrdnance = Latch(squeezeOrdnance, kGripThreshold, g_held[Ordnance]);
        const bool wantTactical = Latch(squeezeTactical, kGripThreshold, g_held[Tactical]);
        if (wantOrdnance != g_held[Ordnance] || wantTactical != g_held[Tactical]) {
            char line[360]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] GRIP EDGE: squeeze L=%.2f R=%.2f (threshold %.2f) -> ordnance %d->%d, "
                "tactical %d->%d. Ordnance reads the RIGHT grip. If one hand is squeezed and BOTH "
                "values are high, the runtime is reporting the same grip on both subaction "
                "paths.\n",
                static_cast<double>(squeezeL), static_cast<double>(squeezeR),
                static_cast<double>(kGripThreshold),
                g_held[Ordnance] ? 1 : 0, wantOrdnance ? 1 : 0,
                g_held[Tactical] ? 1 : 0, wantTactical ? 1 : 0);
            Tf2VrLog(line);
        }
        Apply(Ordnance, wantOrdnance);
        Apply(Tactical, wantTactical);
    }
    // WHO ASSERTS +weaponCycle? THE WEARER'S ITEM 1, AND THIS IS READ-ONLY.
    //
    // The report is that the RIGHT TRIGGER switches weapons -- now in normal
    // play, not only in ADS. On the kbutton route that is impossible: the right
    // trigger drives Attack, and WeaponCycle is on the LEFT controller's
    // secondary button, one line below. So either the trigger is reaching the
    // game by a SECOND route (the synthetic pad forwards the trigger too, and
    // this project has already had one input travel two routes at once), or
    // something is writing that kbutton behind us.
    //
    // The state word is sampled BEFORE our own Apply re-asserts it, which is the
    // only moment the answer is visible. Read like this:
    //
    //   foreign=1 with wePressed=0   somebody else set +weaponCycle. Not us, and
    //                                the trigger value on the same line says
    //                                whether it correlates with the trigger.
    //   foreign never 1, and the
    //   weapon still switches        +weaponCycle is not the mechanism at all;
    //                                the switch is happening by some other route
    //                                and this whole avenue is the wrong one.
    //   wePressed=1 on a frame the
    //   wearer touched no button     the left secondary is being reported down
    //                                when it is not -- an xr_input latch fault,
    //                                not a mapping fault.
    //
    // Bounded by construction: it logs only on a CHANGE, and it stops after 40
    // lines so a stuck bit cannot flood a session's log.
    {
        static int lastForeign = -1;
        static bool lastPressed = false;
        static int budget = 40;
        const int* cycleState = g_client ? StateWord(WeaponCycle) : nullptr;
        const int seen = cycleState ? *cycleState : -1;
        const bool wePressed = (lb & kControllerSecondaryButton) != 0;
        // Held in the engine's word while WE are not asserting it, and were not
        // asserting it last frame either.
        const int foreign = (seen > 0 && (seen & kStateHeld) != 0 && !wePressed &&
                             !g_held[WeaponCycle]) ? 1 : 0;
        if (budget > 0 && (foreign != lastForeign || wePressed != lastPressed)) {
            --budget;
            char line[360]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] WEAPONCYCLE: foreign=%d wePressed=%d | +weaponCycle state=0x%X | "
                "rtrigger=%.2f ltrigger=%.2f lb=0x%02X rb=0x%02X | attack_held=%d ads_held=%d "
                "(%d lines left)\n",
                foreign, wePressed ? 1 : 0, seen,
                static_cast<double>(g_controllerTrigger[1]),
                static_cast<double>(g_controllerTrigger[0]),
                static_cast<unsigned>(lb), static_cast<unsigned>(rb),
                g_held[Attack] ? 1 : 0, g_held[Ads] ? 1 : 0, budget);
            Tf2VrLog(line);
            lastForeign = foreign;
            lastPressed = wePressed;
        }
    }
    Apply(WeaponCycle, (lb & kControllerSecondaryButton) != 0);

    // THE MENU BUTTON IS THE GAME'S PAUSE MENU, as it always was. The config
    // panel is on L3+R3.
    //
    // Every other binding re-asserts a held state every frame; this one fires a
    // command, so it must run once per press or the menu would flap at 84 Hz
    // while held.
    if (menuButtonPressed) ToggleGameUi();

    // input.a_sends_space IS GONE, and it was retired on a measurement.
    //
    // It existed because forcing controller mode made the post-load panel wait
    // for a real gamepad button while we could only send keyboard keys. The
    // synthetic pad supplies the real button, so the workaround has nothing
    // left to do -- and that is measured, not assumed: with the injected-key
    // path off, the session's log counted ZERO "A -> SPACE" injections while
    // the wearer dismissed BOTH continue panels with A.
    //
    // Removing it was the wearer's own condition for this phase being done.

    // ONE LINE A SECOND, and it separates the three ways this can fail.
    //
    // "No buttons did anything" has to be resolvable without another run, so
    // the line carries the sticks as read, the mask we are asserting, and the
    // state word the engine currently holds for +forward. Sticks at zero means
    // the controllers are not reaching us; a mask with bits set but a state
    // word that keeps coming back clear means the engine is overwriting us;
    // both non-zero and still no movement means the kbuttons are not what this
    // engine folds into its usercmd, and the route is wrong rather than the
    // addresses.
    static std::uint64_t lastLog = 0;
    const std::uint64_t tick = GetTickCount64();
    if (tick - lastLog >= 1000) {
        lastLog = tick;
        int mask = 0;
        for (int i = 0; i < kButtonCount; ++i) mask |= (g_held[i] ? (1 << i) : 0);
        int forwardState = -1;
        __try {
            forwardState = *StateWord(Forward);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        char line[288];
        std::snprintf(line, sizeof(line),
                      "[TF2VR] vrinput: L<%.2f,%.2f> R<%.2f,%.2f> trig=%.2f btn=0x%02X | held "
                      "mask=0x%04X | +forward state=%d | menu tracked=%d\n",
                      lx, ly, rx, ry, rt, rb, mask, forwardState, g_menuOpen ? 1 : 0);
        Tf2VrLog(line);
    }
}

void SetForceControllerMode(bool force) {
    g_forceControllerMode = force;
    Tf2VrLog(force ? "[TF2VR] controller mode: forcing IsControllerModeActive true (armed; the "
                     "swap happens once client.dll's interface exists).\n"
                   : "[TF2VR] controller mode: not forced.\n");
}

bool IsControllerModeBelievedActive() { return QueryControllerModeActive(); }

void SetKeyboardMenuNav(bool enabled) {
    g_keyboardMenuNav = enabled;
    Tf2VrLog(enabled
        ? "[TF2VR] vrinput: menus driven by INJECTED KEYS (arrows, SPACE, ESC) -- the pre-pad path.\n"
        : "[TF2VR] vrinput: injected menu keys OFF. The game drives its own menus from the synthetic "
          "pad; A -> SPACE goes off with them, because they are one mechanism.\n");
}

// RETIRED. Kept only so an old ini with input.a_sends_space in it still loads
// and says why the key no longer does anything, instead of silently ignoring a
// line someone put there on purpose.
void SetASendsSpace(bool) {
    Tf2VrLog("[TF2VR] vrinput: input.a_sends_space is RETIRED and does nothing. The synthetic pad "
             "supplies a real A, and a session with the injected keys off dismissed both continue "
             "panels with zero SPACE injections. The line can be deleted.\n");
}


// Public wrappers. The implementations live in the anonymous namespace with the
// state they touch; these are what config.cpp and the tick call.
void SetHoldKeyboardMode(bool hold) { SetHoldKeyboardModeImpl(hold); }
int ReadGameControllerModeFlag() { return ReadGameControllerModeFlagImpl(); }
void WatchRawControllerButtonEdges() { WatchRawControllerButtons(); }
void WatchControllerModeObject() { WatchControllerObject(); }
void HoldGameControllerFlagDown() { HoldControllerFlagImpl(); }

// ---------------------------------------------------------------------------
// input.crouch_mode is a preference with an immediate, self-evident effect, so
// it needs no probe -- but it names its new state in the log, because a setting
// that changes nothing visible and says nothing is indistinguishable from one
// that never applied.
// ---------------------------------------------------------------------------

void SetCrouchMode(int mode) {
    const int wanted = (mode == 0 || mode == 2) ? mode : 1;
    const int was = g_crouchMode.exchange(wanted, std::memory_order_acq_rel);
    if (was == wanted) return;
    // Leaving a latched crouch behind across a mode change would look exactly
    // like the toggle having stuck, so it is cleared on the transition as well
    // as in the tick. Clearing twice is harmless; missing it once is a bug the
    // wearer has to stand up out of.
    g_crouchLatched = false;
    switch (wanted) {
        case 0:
            Tf2VrLog("[TF2VR] input: crouch is a HOLD -- the right stick must be held down, on the "
                     "engine's +duck. Any latched crouch was cleared on the switch.\n");
            break;
        case 2:
            Tf2VrLog("[TF2VR] input: crouch is OUR OWN TOGGLE over +duck -- the fallback. Use this "
                     "only if mode 1 did not toggle. Any latched crouch was cleared.\n");
            break;
        default:
            Tf2VrLog("[TF2VR] input: crouch is the ENGINE'S OWN TOGGLE -- +toggle_duck, the game's "
                     "#TOGGLE_CROUCH. One push down of the right stick crouches, the next stands "
                     "up, and the engine owns the toggling. Any latched crouch was cleared.\n");
            break;
    }
}

int CrouchMode() { return g_crouchMode.load(std::memory_order_acquire); }

// ---------------------------------------------------------------------------
// RIGHT-STICK CROSS-TALK TUNING. See the note beside g_stickCrossRatio.
//
// All three are clamped to ranges that cannot make the stick unusable: a
// deadzone at 0 would fire on noise, and a cross ratio at or above 1.0 would
// make a perfect 45-degree diagonal the ONLY way to get both axes, which is the
// opposite of what was asked for.
// ---------------------------------------------------------------------------

void SetTurnDeadzone(float value) {
    if (value < 0.05f) value = 0.05f;
    if (value > 0.95f) value = 0.95f;
    g_turnDeadzone.store(value, std::memory_order_release);
}

void SetJumpCrouchDeadzone(float value) {
    if (value < 0.05f) value = 0.05f;
    if (value > 0.95f) value = 0.95f;
    g_jumpCrouchDeadzone.store(value, std::memory_order_release);
}

void SetStickCrossRatio(float value) {
    if (value < 0.0f) value = 0.0f;
    // Strictly below 1.0. At 1.0 the two cones meet with no overlap and a
    // deliberate diagonal becomes impossible to hold; the wearer explicitly
    // asked for simultaneous turn-and-crouch to keep working.
    if (value > 0.95f) value = 0.95f;
    g_stickCrossRatio.store(value, std::memory_order_release);
}

float TurnDeadzone() { return g_turnDeadzone.load(std::memory_order_acquire); }
float JumpCrouchDeadzone() { return g_jumpCrouchDeadzone.load(std::memory_order_acquire); }
float StickCrossRatio() { return g_stickCrossRatio.load(std::memory_order_acquire); }


// ---------------------------------------------------------------------------
// TURN SPEED AND SNAP TURN.
// ---------------------------------------------------------------------------

// TURN SPEED IS A PERCENTAGE, AND IT SCALES BOTH ROUTES.
//
// It was degrees per second driving cl_yawspeed, and the wearer reported "I
// couldn't tell any difference between min and max". The log shows the command
// SUCCEEDING at 10 and at 60, so cl_yawspeed was being set and simply is not
// what turns this game.
//
// Turning is DOUBLE-ROUTED, like the grips and B before it. vr_input writes the
// engine's +left/+right, and the synthetic pad ALSO forwards the right stick's X
// to the game, which turns through Respawn's own gamepad look. That path does not
// read cl_yawspeed -- none of Source's joy_* convars even exist in this binary,
// because Respawn replaced the input system -- so the pad route was doing the
// turning and ignoring the setting.
//
// Rather than guess at Respawn's convar names, this scales what we SEND: the
// stick deflection handed to the pad, and cl_yawspeed for the kbutton half, both
// by the same fraction. Whichever route is actually turning, it scales.
//
// If the minimum still feels fast, the residue is a route that ignores both, and
// that is a real finding rather than a mystery.
// THE SETTING IS NOT THE RATE, and it stopped being the rate deliberately.
//
// It used to be a straight percentage of the game's own turn rate, 10..100.
// The wearer: everything below about 70 was "SO SLOW and UNUSABLE" -- which
// means three fifths of the slider's travel moved through settings nobody
// would ever choose, and the band worth having was squeezed into the last
// third with no way to go past the top of it.
//
// So the slider is re-anchored onto the part that is usable. The number the
// wearer sets runs 0..100 across that band:
//
//     setting   0  ->   70% of the game's rate   the old floor of usability
//     setting  70  ->  100% of the game's rate   the old default, unchanged
//     setting 100  ->  113% of the game's rate   past the old maximum
//
// Every point of the slider now does something someone might want, the old
// default still exists and still feels identical at 70, and the top goes
// somewhere the old slider could not reach. The registry default moves to 70,
// so a fresh install turns at exactly the rate it turned at before.
//
// A saved `input.turn_speed = 100` from an older build now means 113% rather
// than 100%. That is a faster turn, not a broken one, and the slider is right
// there.
namespace {
constexpr float kTurnRateFloorPct = 70.0f;   // what setting 0 sends
// Chosen so that setting 70 sends exactly 100: (100 - 70) * (100 / 70).
constexpr float kTurnRateSpanPct  = (100.0f - kTurnRateFloorPct) * (100.0f / 70.0f);
float TurnRatePercent(float setting) {
    return kTurnRateFloorPct + setting * (kTurnRateSpanPct / 100.0f);
}
}  // namespace

void SetTurnSpeed(float percent) {
    if (percent < 0.0f) percent = 0.0f;
    if (percent > 100.0f) percent = 100.0f;
    g_turnSpeed.store(percent, std::memory_order_release);
    const float ratePct = TurnRatePercent(percent);
    // The kbutton half, scaled from Source's stock 210 deg/s.
    const float yawSpeed = 210.0f * (ratePct / 100.0f);
    g_effectiveYawSpeed = yawSpeed;
    char command[64]{};
    std::snprintf(command, sizeof(command), "cl_yawspeed %.0f\n", static_cast<double>(yawSpeed));
    const bool ran = RunEngineConsoleCommand(command);
    char line[260]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] input: turn speed %.0f -> %.0f%% of the game's rate. Pad stick scaled to "
        "%.2f, cl_yawspeed %.0f (%s).\n",
        static_cast<double>(percent), static_cast<double>(ratePct),
        static_cast<double>(ratePct / 100.0f), static_cast<double>(yawSpeed),
        ran ? "applied" : "engine not up yet; stored");
    Tf2VrLog(line);
}

// The fraction the synthetic pad multiplies the right stick's X by. Reads
// the RATE, not the setting -- the two stopped being the same number above.
float TurnStickScale() {
    return TurnRatePercent(g_turnSpeed.load(std::memory_order_acquire)) / 100.0f;
}

float TurnSpeed() { return g_turnSpeed.load(std::memory_order_acquire); }

void SetSnapTurnDegrees(float degrees) {
    if (degrees < 10.0f) degrees = 10.0f;
    if (degrees > 90.0f) degrees = 90.0f;
    g_snapTurnDegrees.store(degrees, std::memory_order_release);
    char line[200]{};
    std::snprintf(line, sizeof(line), "[TF2VR] input: snap step %.0f degrees.\n",
                  static_cast<double>(degrees));
    Tf2VrLog(line);
}

void SetTurnMode(int mode) {
    const int wanted = mode != 0 ? 1 : 0;
    g_turnMode.store(wanted, std::memory_order_release);
    Tf2VrLog(wanted
        ? "[TF2VR] input: SNAP turning. Each flick is one instant step, and the stick must return "
          "to centre between turns.\n"
        : "[TF2VR] input: SMOOTH turning, at the configured turn speed.\n");
}

int TurnMode() { return g_turnMode.load(std::memory_order_acquire); }

float SnapTurnDegrees() { return g_snapTurnDegrees.load(std::memory_order_acquire); }

// Re-applies the turn speed once the engine is up. Called from the plugin frame;
// silent and free once it has taken.
void AdvanceTurnSpeed() {
    static bool applied = false;
    if (applied) return;
    const float wanted = g_turnSpeed.load(std::memory_order_acquire);
    // NO "<= 0 means unset" GUARD ANY MORE. 0 is a legal setting now -- the
    // slow end of the usable band, not "nothing chosen" -- and skipping the
    // re-apply for it would leave cl_yawspeed wherever the engine started.
    if (!GetModuleHandleA("engine.dll")) return;
    applied = true;
    SetTurnSpeed(wanted);
}

// input.haptic_strength -- a multiplier on the game's own rumble.
//
// The wearer: "There is haptics but it is a bit hard to feel." A pad's weighted
// motors and a Touch controller's linear actuator do not deliver the same force
// for the same nominal amplitude, so passing the game's number through unchanged
// under-drives the controller. Above 100 the amplitude clamps at 1.0, which is
// the runtime's maximum -- so the top of this range makes weak rumbles firmer
// without being able to overdrive anything.
void SetHapticStrength(float percent) {
    // 0-100, not 0-300. XrHapticVibration::amplitude is clamped to 0..1 by the
    // SPECIFICATION, so everything above 100 was discarded by the runtime and the
    // wearer reported the top two thirds of the range as indistinguishable. A
    // range that cannot act is a lie about what the control does.
    if (percent < 0.0f) percent = 0.0f;
    if (percent > 100.0f) percent = 100.0f;
    g_hapticStrength.store(percent, std::memory_order_release);
    char line[200]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR] input: haptic strength %.0f%% (0 turns rumble off).\n",
                  static_cast<double>(percent));
    Tf2VrLog(line);
}

float HapticStrength() { return g_hapticStrength.load(std::memory_order_acquire) / 100.0f; }

void SetTurnCurve(float curve) {
    if (curve < 1.0f) curve = 1.0f;
    if (curve > 4.0f) curve = 4.0f;
    g_turnCurve.store(curve, std::memory_order_release);
    char line[220]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR] input: turn curve %.1f (1.0 linear; higher gives finer control near "
                  "centre).\n", static_cast<double>(curve));
    Tf2VrLog(line);
}

float TurnCurve() { return g_turnCurve.load(std::memory_order_acquire); }

void SetKbuttonRouteEnabled(bool enabled) {
    if (g_kbuttonsEnabled.load(std::memory_order_acquire) == enabled) return;
    // RELEASE BEFORE DISABLING, never after. Apply() early-returns once the
    // route is off, so releasing afterwards writes nothing and would leave
    // every kbutton the route had asserted stuck down in the engine -- the
    // player sprinting or crouching forever with no input causing it.
    if (!enabled) ReleaseAll();
    g_kbuttonsEnabled.store(enabled, std::memory_order_release);
    Tf2VrLog(enabled
        ? "[TF2VR] input.kbuttons = 1: THE LEGACY ENGINE-WRITE ROUTE IS BACK. Buttons arrive twice "
          "-- once as engine kbutton state, once on the synthetic pad -- and the menu button sends "
          "a real ESC keystroke again. This is the pre-2026-08-29 behaviour, kept as a one-line "
          "revert; if nothing was missing without it, delete the flag and its branches.\n"
        : "[TF2VR] input.kbuttons = 0 (the default): the synthetic pad is the ONLY route into the "
          "game, so the game's own controller bindings decide what every button means, in menus "
          "and in gameplay alike. No engine input memory is written and no keystrokes are "
          "injected.\n");
}

bool IsKbuttonRouteEnabled() { return g_kbuttonsEnabled.load(std::memory_order_acquire); }
