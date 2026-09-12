#include "crosshair_scale.h"

#include "ads_zoom.h"

#include "diagnostics.h"
#include "hook_registry.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Read by the wrapper on every call. 1.0 makes it an exact pass-through, and
// the armed byte makes it not even read the argument.
extern "C" volatile std::uint8_t g_crosshairScaleArmed = 0;
extern "C" volatile float g_crosshairScale = 1.0f;
extern "C" volatile std::uint64_t g_crosshairScaleHits = 0;
extern "C" std::uintptr_t g_crosshairTrampoline = 0;

extern "C" void crosshairScaleInterceptor();

namespace {

// client.dll+0x158EB0 -- the crosshair's per-element emit. See the header for
// how it was identified and why it is preferred to the draw function.
constexpr std::uintptr_t kEmitRva = 0x158EB0;

// mov [rsp+8],rbx / mov [rsp+10h],rbp / mov [rsp+18h],rsi
// 5 + 5 + 5 = 15 bytes, three whole instructions, none rip-relative, and 15 is
// where the next boundary falls -- 14 would land inside the third store. A
// 14-byte absolute jump plus one nop fits exactly.
constexpr std::uint8_t kExpectedEmit[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08,
    0x48, 0x89, 0x6C, 0x24, 0x10,
    0x48, 0x89, 0x74, 0x24, 0x18,
};
constexpr std::size_t kDisplaced = sizeof(kExpectedEmit);
static_assert(kDisplaced == 15, "14-byte jump plus one nop");

std::atomic_bool g_installed = false;
std::uint64_t g_lastLogTick = 0;
std::uint64_t g_lastHits = 0;

bool Install() {
    if (g_installed.load(std::memory_order_acquire)) return true;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) {
        Tf2VrLog("[TF2VR] crosshair: client.dll is not loaded yet; nothing patched.\n");
        return false;
    }
    auto* site = reinterpret_cast<std::uint8_t*>(client) + kEmitRva;
    if (std::memcmp(site, kExpectedEmit, kDisplaced) != 0) {
        char line[420]{};
        int used = std::snprintf(line, sizeof(line),
            "[TF2VR] crosshair: client.dll+0x%llX does not begin with the bytes it was resolved "
            "from, so this is not the build those offsets came from. NOT installed. Found:",
            static_cast<unsigned long long>(kEmitRva));
        for (std::size_t i = 0; i < kDisplaced && used < static_cast<int>(sizeof(line)) - 8; ++i) {
            used += std::snprintf(line + used, sizeof(line) - used, " %02X", site[i]);
        }
        std::snprintf(line + used, sizeof(line) - used, "\n");
        Tf2VrLog(line);
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(site, kDisplaced, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Tf2VrLog("[TF2VR] crosshair: VirtualProtect failed; nothing patched.\n");
        return false;
    }

    // jmp qword ptr [rip+0], then the absolute target. Built from a ZEROED
    // array so bytes 2..5 are the displacement and MUST be zero -- sway_probe
    // records a memset of 0x90 leaving disp32 = 0x90909090 and the first call
    // jumping through a wild address.
    std::uint8_t detour[kDisplaced]{};
    detour[0] = 0xFF;
    detour[1] = 0x25;
    const auto target = reinterpret_cast<std::uintptr_t>(crosshairScaleInterceptor);
    std::memcpy(detour + 6, &target, sizeof(target));
    std::memset(detour + 14, 0x90, kDisplaced - 14);

    std::uint32_t displacement = 0;
    std::memcpy(&displacement, detour + 2, sizeof(displacement));
    if (detour[0] != 0xFF || detour[1] != 0x25 || displacement != 0) {
        Tf2VrLog("[TF2VR] crosshair: refusing to patch -- the constructed detour is malformed. "
                 "Nothing was written.\n");
        DWORD ignored = 0;
        VirtualProtect(site, kDisplaced, oldProtect, &ignored);
        return false;
    }

    auto* trampoline = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        Tf2VrLog("[TF2VR] crosshair: could not allocate a trampoline; nothing patched.\n");
        DWORD ignored = 0;
        VirtualProtect(site, kDisplaced, oldProtect, &ignored);
        return false;
    }
    std::memcpy(trampoline, site, kDisplaced);
    std::uint8_t* jump = trampoline + kDisplaced;
    jump[0] = 0xFF;
    jump[1] = 0x25;
    std::memset(jump + 2, 0, 4);
    const auto resume = reinterpret_cast<std::uintptr_t>(site + kDisplaced);
    std::memcpy(jump + 6, &resume, sizeof(resume));
    FlushInstructionCache(GetCurrentProcess(), trampoline, 64);
    g_crosshairTrampoline = reinterpret_cast<std::uintptr_t>(trampoline);
    RegisterHookSite("crosshair emit trampoline", trampoline, 64);
    RegisterHookSite("crosshair emit patched entry", site, kDisplaced);

    std::memcpy(site, detour, kDisplaced);
    FlushInstructionCache(GetCurrentProcess(), site, kDisplaced);
    DWORD ignored = 0;
    VirtualProtect(site, kDisplaced, oldProtect, &ignored);
    g_installed.store(true, std::memory_order_release);

    char line[360]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] crosshair: wrapped the reticle's per-element emit at client.dll+0x%llX, %zu bytes "
        "displaced, trampoline at %p. It is called only from the crosshair draw, so nothing else "
        "can be affected. Pass-through at scale 1.0.\n",
        static_cast<unsigned long long>(kEmitRva), kDisplaced, static_cast<void*>(trampoline));
    Tf2VrLog(line);
    return true;
}

}  // namespace

void SetCrosshairScale(float scale) {
    // Clamped: this is nudged blind with a headset on, and a scale at zero
    // removes its own target with no key that brings it back.
    if (scale < 0.05f) scale = 0.05f;
    if (scale > 20.0f) scale = 20.0f;
    const bool wantArmed = std::fabs(scale - 1.0f) > 0.0001f;
    if (wantArmed && !Install()) {
        // NOT a refusal. The ini is applied at plugin load, before client.dll
        // is in the process, so the first attempt can only fail -- and treating
        // that as final is how vrinput once spent a whole session doing nothing.
        // Advance retries.
        g_crosshairScale = scale;
        return;
    }
    g_crosshairScale = scale;
    g_crosshairScaleArmed = wantArmed ? 1 : 0;
    char line[220]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR] crosshair: reticle scale %.3f%s\n", scale,
                  wantArmed ? "" : " (1.0 -- exact pass-through, the argument is not touched)");
    Tf2VrLog(line);
}

float CrosshairScale() { return g_crosshairScale; }

void NudgeCrosshairScale(float factor) { SetCrosshairScale(g_crosshairScale * factor); }

void AdvanceCrosshairScale() {
    if (g_crosshairScaleArmed && !g_installed.load(std::memory_order_acquire)) {
        static std::uint64_t lastTry = 0;
        const std::uint64_t now = GetTickCount64();
        if (now - lastTry >= 500) {
            lastTry = now;
            if (Install()) SetCrosshairScale(g_crosshairScale);
        }
    }
    if (!g_crosshairScaleArmed) return;
    const std::uint64_t tick = GetTickCount64();
    if (tick - g_lastLogTick < 1000) return;
    g_lastLogTick = tick;

    // THE FALSIFIER, and it separates the two failures that look identical from
    // the headset. hits at zero means the wrapper is not being reached at all,
    // which is a hook problem and says nothing about the float. hits climbing
    // while the reticle is unchanged means the float is NOT a scale -- a real
    // answer, and the one that sends this to the widget setter at 0x54B160
    // rather than to more guessing here.
    const std::uint64_t hits = g_crosshairScaleHits;
    char line[280]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR] crosshair: scale %.3f | emits scaled=%llu (+%llu since last second)\n",
                  static_cast<double>(g_crosshairScale),
                  static_cast<unsigned long long>(hits),
                  static_cast<unsigned long long>(hits - g_lastHits));
    Tf2VrLog(line);
    g_lastHits = hits;
}

// ---------------------------------------------------------------------------
// THE RETICLE, HIDDEN OUTRIGHT. See the header for the derivation of both
// addresses and for what the falsifier line separates.
// ---------------------------------------------------------------------------

namespace {

// client.dll+0x22AC694 -- the crosshair state global Crosshair_SetState writes.
constexpr std::uintptr_t kCrosshairStateRva = 0x22AC694;
constexpr int kCrosshairStateHideAll = 2;

std::atomic_bool g_reticleHidden = false;
// The engine's own value, captured immediately BEFORE our first store, so
// disarming can put back what was there rather than guessing that 0 is right.
std::atomic_bool g_reticleStateSaved = false;
std::atomic_int g_reticleSavedState = 0;
std::uint64_t g_reticleReasserts = 0;
std::uint64_t g_reticleFrames = 0;
std::uint64_t g_reticleLastLogTick = 0;

int* CrosshairState() {
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return nullptr;
    return reinterpret_cast<int*>(reinterpret_cast<std::uint8_t*>(client) + kCrosshairStateRva);
}

}  // namespace

void SetReticleHidden(bool hidden) {
    const bool was = g_reticleHidden.exchange(hidden, std::memory_order_acq_rel);
    if (was == hidden) return;

    int* state = CrosshairState();
    if (!state) {
        // NOT a refusal, for the same reason SetCrosshairScale's is not: the ini
        // is applied at plugin load, before client.dll is in the process, so the
        // first attempt can only fail. AdvanceReticleHidden picks it up.
        Tf2VrLog(hidden
            ? "[TF2VR] reticle: hide requested before client.dll is loaded; the per-frame "
              "re-assert will apply it as soon as the module is there.\n"
            : "[TF2VR] reticle: show requested before client.dll is loaded; nothing to restore.\n");
        return;
    }

    if (hidden) {
        if (!g_reticleStateSaved.load(std::memory_order_acquire)) {
            g_reticleSavedState.store(*state, std::memory_order_release);
            g_reticleStateSaved.store(true, std::memory_order_release);
        }
        const int before = *state;
        *state = kCrosshairStateHideAll;
        char line[280]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] reticle: HIDDEN. client.dll+0x%llX %d -> %d (2 = HIDE_ALL). Re-asserted every "
            "plugin frame because Crosshair_SetState owns this global.\n",
            static_cast<unsigned long long>(kCrosshairStateRva), before, *state);
        Tf2VrLog(line);
    } else {
        const int restore = g_reticleStateSaved.load(std::memory_order_acquire)
                                ? g_reticleSavedState.load(std::memory_order_acquire)
                                : 0;
        const int before = *state;
        *state = restore;
        char line[280]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] reticle: SHOWN. client.dll+0x%llX %d -> %d (%s). Re-assert stopped after "
            "%llu frames, %llu of which had been overwritten by the engine.\n",
            static_cast<unsigned long long>(kCrosshairStateRva), before, *state,
            g_reticleStateSaved.load(std::memory_order_acquire) ? "the value seen before we armed"
                                                                : "0 -- nothing was captured",
            static_cast<unsigned long long>(g_reticleFrames),
            static_cast<unsigned long long>(g_reticleReasserts));
        Tf2VrLog(line);
        g_reticleFrames = 0;
        g_reticleReasserts = 0;
    }
}

bool IsReticleHidden() { return g_reticleHidden.load(std::memory_order_acquire); }

void AdvanceReticleHidden() {
    if (!g_reticleHidden.load(std::memory_order_acquire)) return;
    int* state = CrosshairState();
    if (!state) return;

    // Capture before the first store even if we armed pre-load, so the restore
    // value is the engine's and not our own 2.
    if (!g_reticleStateSaved.load(std::memory_order_acquire)) {
        g_reticleSavedState.store(*state, std::memory_order_release);
        g_reticleStateSaved.store(true, std::memory_order_release);
    }

    ++g_reticleFrames;
    // THE MEASUREMENT. Only count the frames where the value was NOT already
    // ours -- those are the frames the engine genuinely overwrote us, and that
    // count is the whole reason this loop exists rather than a single store.
    if (*state != kCrosshairStateHideAll) {
        ++g_reticleReasserts;
        *state = kCrosshairStateHideAll;
    }

    const std::uint64_t tick = GetTickCount64();
    if (tick - g_reticleLastLogTick < 5000) return;
    g_reticleLastLogTick = tick;
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] reticle: hidden | frames=%llu reasserts=%llu (engine overwrote us on %.1f%% of "
        "frames). reasserts=0 with the reticle still visible means this address is not the state "
        "this build's draw reads.\n",
        static_cast<unsigned long long>(g_reticleFrames),
        static_cast<unsigned long long>(g_reticleReasserts),
        g_reticleFrames ? 100.0 * static_cast<double>(g_reticleReasserts) /
                              static_cast<double>(g_reticleFrames)
                        : 0.0);
    Tf2VrLog(line);
}

// ---------------------------------------------------------------------------
// ads.keep_reticle -- HOLD THE RETICLE THROUGH ADS.
//
// Flat ADS hides the reticle and that is right, because on a monitor the camera
// is GLUED to the sight line: the game takes your eye to the sights for you, so
// a reticle would be redundant. VR removes exactly that guarantee. The gun is
// wherever your hand is, nothing forces your eye onto the rear sight, and the
// wearer's report is the consequence: "unless you have it lined up EXACTLY to
// your eye, which flat enforces, it is difficult to aim well."
//
// PLAN-ADS section 1 lists reticle hiding as "CORRECT AS-IS, the one piece
// already right". That judgement assumed the sights are usable once it is gone.
// The observation says otherwise, and an observation beats a design assumption.
//
// WE ARE NOT THE ONES HIDING IT. reticle.hidden is a separate setting and it is
// off; the reticle disappears because we trigger the game's own +zoom and flat
// ADS hides it as part of that state. So this does not "stop hiding" anything --
// it re-asserts the VISIBLE value over the engine's, which is the same
// mechanism SetReticleHidden already uses with the opposite value, on the same
// address, with the same falsifier.
//
// THE VISIBLE VALUE IS LATCHED FROM THE ENGINE, NOT ASSUMED. While the ADS
// fraction is zero the reticle is by definition in its normal state, so
// whatever the global holds at that instant IS the value to restore. Same trick
// as the resting fov in ads_zoom: the engine declares "at rest", nothing here
// infers it, and no constant is guessed. If that latched value turns out to be
// HIDE_ALL the setting refuses rather than writing a no-op and reporting
// success.
//
// AND IT REFUSES TO FIGHT reticle.hidden. Two producers writing one global with
// opposite intents is the fault this project has hit four times on inputs; it
// is not going to be introduced here on a render global.
namespace {
std::atomic_bool g_adsKeepReticle = false;
std::atomic_int g_reticleRestState{-1};
std::atomic_bool g_reticleRestLatched = false;
std::uint64_t g_keepFrames = 0;
std::uint64_t g_keepReasserts = 0;
std::uint64_t g_keepLastLogTick = 0;
bool g_keepConflictLogged = false;
}  // namespace

void SetAdsKeepReticle(bool keep) {
    const bool was = g_adsKeepReticle.exchange(keep, std::memory_order_acq_rel);
    if (was == keep) return;
    Tf2VrLog(keep
        ? "[TF2VR] ads.keep_reticle = 1: the reticle is held visible through ADS. Flat hides it "
          "because the monitor camera is glued to the sight line; VR gives no such guarantee, so "
          "hiding it removes the only aiming reference without supplying the one it stood for. "
          "The visible value is latched from the engine while the ADS fraction is zero -- nothing "
          "here guesses a constant.\n"
        : "[TF2VR] ads.keep_reticle = 0: the game's own ADS hiding stands.\n");
}

bool IsAdsKeepReticle() { return g_adsKeepReticle.load(std::memory_order_acquire); }

void AdvanceAdsKeepReticle() {
    if (!g_adsKeepReticle.load(std::memory_order_acquire)) return;
    // Never both. reticle.hidden wants it gone and this wants it shown; letting
    // them alternate on one global would produce a flicker nobody could
    // attribute, so the explicit setting wins and this says why, once.
    if (IsReticleHidden()) {
        if (!g_keepConflictLogged) {
            g_keepConflictLogged = true;
            Tf2VrLog("[TF2VR] ads.keep_reticle is armed but reticle.hidden is ALSO armed. They "
                     "write the same global with opposite intents, so keep_reticle stands down "
                     "and hidden wins. Turn reticle.hidden off if you want this.\n");
        }
        return;
    }
    int* state = CrosshairState();
    if (!state) return;

    AdsZoomState zoom;
    if (!GetAdsZoom(&zoom)) return;

    if (zoom.frac <= 0.001f) {
        // Hip. Whatever the engine holds now IS the visible value.
        if (*state != kCrosshairStateHideAll) {
            g_reticleRestState.store(*state, std::memory_order_release);
            g_reticleRestLatched.store(true, std::memory_order_release);
        }
        return;
    }
    if (!g_reticleRestLatched.load(std::memory_order_acquire)) return;
    const int rest = g_reticleRestState.load(std::memory_order_acquire);

    ++g_keepFrames;
    if (*state != rest) {
        ++g_keepReasserts;
        *state = rest;
    }

    const std::uint64_t tick = GetTickCount64();
    if (tick - g_keepLastLogTick < 5000) return;
    g_keepLastLogTick = tick;
    char line[360]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] ads.keep_reticle: holding state %d through ADS | frames=%llu reasserts=%llu "
        "(the engine hid it on %.1f%% of them, which is the number that says this is doing "
        "anything at all). reasserts=0 with the reticle still gone means the hiding is not this "
        "global on this build.\n",
        rest, static_cast<unsigned long long>(g_keepFrames),
        static_cast<unsigned long long>(g_keepReasserts),
        g_keepFrames ? 100.0 * static_cast<double>(g_keepReasserts) /
                           static_cast<double>(g_keepFrames)
                     : 0.0);
    Tf2VrLog(line);
}
