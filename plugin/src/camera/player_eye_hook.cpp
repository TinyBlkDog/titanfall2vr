#include "player_eye_hook.h"

#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "diagnostics.h"
#include "hook_registry.h"
#include "player_eye.h"

#include <cmath>

namespace {

// ---- what pescan read out of the shipped binaries --------------------------
//
// Every number here is verified at install: the slot must hold exactly the
// function it held offline, and that function must begin with the bytes it
// began with offline. A different build, or another patcher already in the
// slot, refuses loudly and installs nothing.
struct SideSpec {
    const char* module;
    const char* className;
    std::uintptr_t vtableRva;
    std::uintptr_t slotOffset;
    std::uintptr_t functionRva;
};
// C_Player::EyePosition and CPlayer::EyePosition share one prologue.
constexpr std::uint8_t kEyePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08,  // mov [rsp+8], rbx
                                          0x57,                          // push rdi
                                          0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00};  // sub rsp,80h
constexpr SideSpec kSides[2] = {
    {"client.dll", "C_Player", 0x8C7A58, 0x5A0, 0x2C3F40},
    {"server.dll", "CPlayer", 0x9524F8, 0x428, 0x5CF7D0},
};
constexpr const char* kSideName[2] = {"CLIENT", "SERVER"};

using EyeFn = float*(__fastcall*)(void* self, float* out);

// ---- state -----------------------------------------------------------------
// DEFAULTS ON, AND THE VALUE IS THE MEASURED ONE. Not a violation of "nothing
// unproven ships switched on": this is proven (flat run C582B839, the wearer's
// own report hip and ADS), and it is the only height mechanism left. A user
// with no ini at all, or the wearer on a headset whose profile has never been
// touched, must come up at the right height -- that is what "works on any
// headset" has to mean. 8 rather than 10 because 10 read a touch tall.
// The install still verifies the slot and the prologue and refuses loudly, so
// a game patch degrades to "no height change", never to a wrong write.
// The tracked head offset the camera detour publishes and adds to the camera.
// The server eye reads the SAME value so the shot origin cannot drift from the
// viewpoint the reticle is drawn through.
extern "C" volatile std::uint32_t g_cameraOffsetXBits;
extern "C" volatile std::uint32_t g_cameraOffsetYBits;
extern "C" volatile std::uint32_t g_cameraOffsetZBits;

constexpr float kDefaultRaiseUnits = 8.0f;
// eye.lean_follow -- OFF, REFUTED BY A RUN. 2026-09-03.
//
// The idea was sound and the consumer analysis was not: moving the server eye
// to make the shot origin coincide with the rendered viewpoint also moves it
// for everything ELSE that reads the eye, and ADS is one of those. The wearer:
// "ADS weapon no longer perfectly aligned - it changes angle as I lean or as
// the gun moves left/right\", plus "leaning far now seems to make shots go down
// vertically" -- a worse error than the small horizontal one it was meant to
// remove. Hard-sight alignment is load-bearing; lean parallax is a rounding
// error on an unusual posture. Wrong trade, reverted.
//
// Kept behind the key rather than deleted because the MEASUREMENT stands: the
// census still prints the tracking residual, and a future fix has to move the
// shot origin without moving what ADS reads -- which means finding the origin
// itself, not the eye that everything shares.
std::atomic_bool g_leanFollow{false};
std::atomic_bool g_wanted{true};
std::atomic<float> g_raiseConfigured{kDefaultRaiseUnits};
std::atomic<float> g_raiseInForce{kDefaultRaiseUnits};

struct Side {
    EyeFn original = nullptr;
    void** slot = nullptr;
    std::uintptr_t base = 0;
    std::atomic_bool installed{false};
    std::atomic_bool refused{false};
    std::atomic_uint64_t calls{0};
    std::atomic_uint64_t raisedCalls{0};
    std::atomic_uint64_t nullReturns{0};
    std::atomic<float> lastZ{0.0f};
    std::atomic<float> lastRaisedZ{0.0f};
    std::atomic_uint64_t leanedCalls{0};
    std::atomic_uint64_t leanRejected{0};
    std::atomic<float> lastLean{0.0f};
    // The first distinct callers, so the log NAMES the camera path and the
    // fire path rather than reporting a count. Fixed-size, published once.
    static constexpr int kCallerSlots = 8;
    std::atomic<std::uintptr_t> callers[kCallerSlots]{};
    std::atomic_uint32_t callerCount{0};
    std::atomic<std::uintptr_t> selves[4]{};
    std::atomic_uint32_t selfCount{0};
};
Side g_side[2];

void NoteCaller(Side& side, std::uintptr_t ret) {
    const std::uint32_t count = side.callerCount.load(std::memory_order_acquire);
    for (std::uint32_t i = 0; i < count && i < Side::kCallerSlots; ++i) {
        if (side.callers[i].load(std::memory_order_relaxed) == ret) return;
    }
    if (count >= Side::kCallerSlots) return;
    // Racy by design: two threads may both claim slot `count`; the worst case
    // is one lost caller in a diagnostic list, never a wrong pointer.
    side.callers[count].store(ret, std::memory_order_relaxed);
    side.callerCount.store(count + 1, std::memory_order_release);
}

void NoteSelf(Side& side, std::uintptr_t self) {
    const std::uint32_t count = side.selfCount.load(std::memory_order_acquire);
    for (std::uint32_t i = 0; i < count && i < 4; ++i) {
        if (side.selves[i].load(std::memory_order_relaxed) == self) return;
    }
    if (count >= 4) return;
    side.selves[count].store(self, std::memory_order_relaxed);
    side.selfCount.store(count + 1, std::memory_order_release);
}

template <int N>
float* __fastcall EyeInterceptor(void* self, float* out) {
    Side& side = g_side[N];
    float* result = side.original(self, out);
    side.calls.fetch_add(1, std::memory_order_relaxed);
    NoteCaller(side, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    NoteSelf(side, reinterpret_cast<std::uintptr_t>(self));
    float* vec = result ? result : out;
    if (!vec) {
        side.nullReturns.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    side.lastZ.store(vec[2], std::memory_order_relaxed);
    // THE RAISE APPLIES IN A TITAN TOO. 2026-09-03: the plan was to stand it
    // down in the cockpit, and a gate for that was built; five headset runs
    // with the raise applied inside the titan and the wearer's "height felt
    // right in cockpit" retired it, so the gate was deleted rather than left
    // as a knob with no observation behind it (RESULT-TITAN-ARMS-2026-09-03).
    //
    // Because the raise is a CONSTANT rather than a sample, nothing has to be
    // re-measured across a mech transition in either direction, and a save
    // that starts in a titan needs no special case.
    const float raise = g_raiseInForce.load(std::memory_order_relaxed);
    if (raise != 0.0f) {
        vec[2] += raise;
        side.raisedCalls.fetch_add(1, std::memory_order_relaxed);
        side.lastRaisedZ.store(vec[2], std::memory_order_relaxed);
    }

    // ---- THE LEAN, ON THE SERVER SIDE ONLY ---------------------------------
    //
    // Reported 2026-09-03: "if I lean left, the shots go right of the reticle,
    // but a small amount". That is parallax, and the census had already printed
    // it every second as the TRACKING residual -- "head tracking has the
    // rendered eye N units off the game eye, which the round does NOT follow".
    //
    // The rendered camera is the engine's position PLUS the tracked head
    // offset, added by the camera detour. The round leaves the player's eye,
    // which has no such offset. So the reticle is projected from the leaned
    // viewpoint and the round is fired from the unleaned one, and the two
    // disagree by atan(lean / range) -- to the RIGHT when you lean left,
    // exactly as reported.
    //
    // SERVER ONLY, and that asymmetry is the whole point. The client's eye is
    // what the camera is built from, so adding the offset there would apply the
    // lean twice and move the view. The server's eye is where the round starts
    // and nothing adds the offset to it. Raising only that side makes the shot
    // origin coincide with the point the reticle is drawn from.
    //
    // Uses the SAME published offset the camera detour uses, so the two cannot
    // drift apart: one source, two readers.
    if (N == 1 && g_leanFollow.load(std::memory_order_relaxed)) {
        float lean[3]{};
        std::memcpy(&lean[0], const_cast<const std::uint32_t*>(&g_cameraOffsetXBits), sizeof(float));
        std::memcpy(&lean[1], const_cast<const std::uint32_t*>(&g_cameraOffsetYBits), sizeof(float));
        std::memcpy(&lean[2], const_cast<const std::uint32_t*>(&g_cameraOffsetZBits), sizeof(float));
        // The detour clamps its own offset to 40 units; anything beyond that is
        // a tracking glitch rather than a lean and must not move a shot origin.
        bool sane = true;
        for (float axis : lean) {
            if (!(axis == axis) || axis > 41.0f || axis < -41.0f) sane = false;
        }
        if (sane) {
            vec[0] += lean[0];
            vec[1] += lean[1];
            vec[2] += lean[2];
            side.leanedCalls.fetch_add(1, std::memory_order_relaxed);
            side.lastLean.store(std::sqrt(lean[0] * lean[0] + lean[1] * lean[1] + lean[2] * lean[2]),
                                std::memory_order_relaxed);
        } else {
            side.leanRejected.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return result;
}

bool InstallSide(int n) {
    Side& side = g_side[n];
    const SideSpec& spec = kSides[n];
    if (side.installed.load(std::memory_order_acquire) || side.refused.load(std::memory_order_acquire)) {
        return side.installed.load(std::memory_order_acquire);
    }
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(spec.module));
    if (!base) return false;  // not loaded yet; the tick retries
    auto** slot = reinterpret_cast<void**>(base + spec.vtableRva + spec.slotOffset);
    auto* expected = reinterpret_cast<void*>(base + spec.functionRva);
    char line[360]{};
    if (*slot != expected) {
        side.refused.store(true, std::memory_order_release);
        std::snprintf(line, sizeof(line),
            "[TF2VR] eye hook %s: %s vtable slot +0x%llX holds %p, expected %s+0x%llX = %p. "
            "REFUSED -- different build, or another patcher is already in the slot.\n",
            kSideName[n], spec.className, static_cast<unsigned long long>(spec.slotOffset), *slot,
            spec.module, static_cast<unsigned long long>(spec.functionRva), expected);
        Tf2VrLog(line);
        return false;
    }
    if (std::memcmp(expected, kEyePrologue, sizeof(kEyePrologue)) != 0) {
        side.refused.store(true, std::memory_order_release);
        std::snprintf(line, sizeof(line),
            "[TF2VR] eye hook %s: %s+0x%llX does not start with the EyePosition prologue read "
            "offline. REFUSED.\n",
            kSideName[n], spec.module, static_cast<unsigned long long>(spec.functionRva));
        Tf2VrLog(line);
        return false;
    }
    side.original = reinterpret_cast<EyeFn>(expected);
    side.base = base;
    void* interceptor = (n == 0) ? reinterpret_cast<void*>(&EyeInterceptor<0>)
                                 : reinterpret_cast<void*>(&EyeInterceptor<1>);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        side.refused.store(true, std::memory_order_release);
        std::snprintf(line, sizeof(line),
            "[TF2VR] eye hook %s: VirtualProtect failed on the vtable slot. REFUSED.\n", kSideName[n]);
        Tf2VrLog(line);
        return false;
    }
    InterlockedExchangePointer(slot, interceptor);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    side.slot = slot;
    RegisterHookSite(n == 0 ? "eye hook CLIENT interceptor" : "eye hook SERVER interceptor",
                     interceptor, 256);
    side.installed.store(true, std::memory_order_release);
    std::snprintf(line, sizeof(line),
        "[TF2VR] eye hook %s INSTALLED: %s::EyePosition, %s vtable +0x%llX slot +0x%llX -> "
        "%s+0x%llX, raise in force %+.2f (configured eye.raise %+.2f; F5 drops it to 0 and back).\n",
        kSideName[n], spec.className, spec.module, static_cast<unsigned long long>(spec.vtableRva),
        static_cast<unsigned long long>(spec.slotOffset), spec.module,
        static_cast<unsigned long long>(spec.functionRva),
        static_cast<double>(g_raiseInForce.load(std::memory_order_relaxed)),
        static_cast<double>(g_raiseConfigured.load(std::memory_order_relaxed)));
    Tf2VrLog(line);
    return true;
}

// module+rva for a return address, or the bare pointer when it is in neither.
void DescribeAddress(std::uintptr_t address, char* out, std::size_t size) {
    for (int n = 0; n < 2; ++n) {
        const std::uintptr_t base = g_side[n].base;
        if (base && address >= base && address < base + 0x4000000ull) {
            std::snprintf(out, size, "%s+0x%llX", kSides[n].module,
                          static_cast<unsigned long long>(address - base));
            return;
        }
    }
    std::snprintf(out, size, "%p", reinterpret_cast<void*>(address));
}

void Heartbeat() {
    static std::uint64_t nextMs = 0;
    static std::uint64_t previousCalls[2] = {0, 0};
    const std::uint64_t now = GetTickCount64();
    if (now < nextMs) return;
    nextMs = now + 5000;
    for (int n = 0; n < 2; ++n) {
        Side& side = g_side[n];
        if (!side.installed.load(std::memory_order_acquire)) {
            if (n == 1 && !side.refused.load(std::memory_order_acquire)) {
                Tf2VrLog("[TF2VR] eye hook SERVER: not installed yet (server.dll not loaded -- it "
                         "arrives with the first level).\n");
            }
            continue;
        }
        const std::uint64_t calls = side.calls.load(std::memory_order_relaxed);
        char callers[400]{};
        std::size_t used = 0;
        const std::uint32_t count = side.callerCount.load(std::memory_order_acquire);
        for (std::uint32_t i = 0; i < count && i < Side::kCallerSlots && used + 40 < sizeof(callers); ++i) {
            char one[64]{};
            DescribeAddress(side.callers[i].load(std::memory_order_relaxed), one, sizeof(one));
            used += static_cast<std::size_t>(std::snprintf(callers + used, sizeof(callers) - used,
                                                           "%s%s", i ? " " : "", one));
        }
        char line[900]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] EYEHOOK %s: calls=%llu (+%llu in 5 s) raised=%llu null=%llu | lean-followed=%llu (rejected %llu, last %.2f units) | raise in force "
            "%+.2f (configured %+.2f) | last z returned %.2f, last raised z %.2f | distinct "
            "players=%u | callers(%u): %s%s\n",
            kSideName[n], static_cast<unsigned long long>(calls),
            static_cast<unsigned long long>(calls - previousCalls[n]),
            static_cast<unsigned long long>(side.raisedCalls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(side.nullReturns.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(side.leanedCalls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(side.leanRejected.load(std::memory_order_relaxed)),
            static_cast<double>(side.lastLean.load(std::memory_order_relaxed)),
            static_cast<double>(g_raiseInForce.load(std::memory_order_relaxed)),
            static_cast<double>(g_raiseConfigured.load(std::memory_order_relaxed)),
            static_cast<double>(side.lastZ.load(std::memory_order_relaxed)),
            static_cast<double>(side.lastRaisedZ.load(std::memory_order_relaxed)),
            side.selfCount.load(std::memory_order_relaxed), count, callers,
            (calls == previousCalls[n]) ? " | NO CALLS IN 5 s -- this side is not being asked" : "");
        Tf2VrLog(line);
        previousCalls[n] = calls;
    }
    // POSITIVE CONTROL for the client side: the live local player's vptr must
    // BE the vtable whose slot was swapped, or the swap is on the wrong class.
    if (g_side[0].installed.load(std::memory_order_acquire)) {
        const void* player = PlayerEyeLocalPlayerForReading();
        if (player) {
            const auto vptr = *reinterpret_cast<const std::uintptr_t*>(player);
            const std::uintptr_t wanted = g_side[0].base + kSides[0].vtableRva;
            char line[240]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] EYEHOOK control: local player vptr %s C_Player vtable (%p vs %p).\n",
                vptr == wanted ? "IS the" : "IS NOT the -- WRONG CLASS, disregard every client count",
                reinterpret_cast<void*>(vptr), reinterpret_cast<void*>(wanted));
            Tf2VrLog(line);
        }
    }
}

}  // namespace

void PlayerEyeHookTick() {
    if (!g_wanted.load(std::memory_order_acquire)) return;
    InstallSide(0);
    InstallSide(1);
    Heartbeat();
}

void SetPlayerEyeHookWanted(bool wanted) {
    g_wanted.store(wanted, std::memory_order_release);
    Tf2VrLog(wanted ? "[TF2VR] eye hook WANTED (eye.hook = 1): installs on both player vtables as "
                      "their modules appear; pass-through until F5.\n"
                    : "[TF2VR] eye hook not wanted (eye.hook = 0).\n");
}

void SetEyeHookRaiseUnits(float units) {
    // Range-gated for the same reason the other height levers are: this moves
    // the eye, and a typo would put it above the map with nothing to say why.
    if (!(units > -60.0f && units < 60.0f)) return;
    // IN FORCE FROM THE MOMENT IT IS SET, since the flat proof (2026-09-02:
    // impacts on the crosshair hip and ADS with +10 in force on both sides).
    // F5 is now the A/B: it takes the raise to zero and back. A live slider
    // in the config menu lands here too, which is why it applies at once.
    g_raiseConfigured.store(units, std::memory_order_release);
    g_raiseInForce.store(units, std::memory_order_release);
    char line[200]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] eye.raise = %+.2f units, IN FORCE (F5 drops it to 0 and back for an A/B).\n",
        static_cast<double>(units));
    Tf2VrLog(line);
}

float EyeHookRaiseConfigured() { return g_raiseConfigured.load(std::memory_order_acquire); }

void ToggleEyeHookRaise() {
    const float configured = g_raiseConfigured.load(std::memory_order_acquire);
    const float current = g_raiseInForce.load(std::memory_order_acquire);
    const float next = (current != 0.0f) ? 0.0f : configured;
    g_raiseInForce.store(next, std::memory_order_release);
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] EYEHOOK F5: raise in force %+.2f -> %+.2f units%s (client %s, server %s).\n",
        static_cast<double>(current), static_cast<double>(next),
        (next == 0.0f && configured == 0.0f) ? " -- eye.raise is 0 in the ini, so F5 has nothing to apply" : "",
        g_side[0].installed.load(std::memory_order_acquire) ? "installed" : "NOT installed",
        g_side[1].installed.load(std::memory_order_acquire) ? "installed" : "NOT installed");
    Tf2VrLog(line);
}

float EyeHookRaiseInForce() { return g_raiseInForce.load(std::memory_order_acquire); }

void RemovePlayerEyeHook() {
    g_raiseInForce.store(0.0f, std::memory_order_release);
    for (int n = 0; n < 2; ++n) {
        Side& side = g_side[n];
        if (!side.installed.load(std::memory_order_acquire) || !side.slot) continue;
        void* interceptor = (n == 0) ? reinterpret_cast<void*>(&EyeInterceptor<0>)
                                     : reinterpret_cast<void*>(&EyeInterceptor<1>);
        if (*side.slot == interceptor) {
            DWORD old = 0;
            if (VirtualProtect(side.slot, sizeof(void*), PAGE_READWRITE, &old)) {
                InterlockedExchangePointer(side.slot, reinterpret_cast<void*>(side.original));
                DWORD ignored = 0;
                VirtualProtect(side.slot, sizeof(void*), old, &ignored);
            }
        }
        side.installed.store(false, std::memory_order_release);
    }
}

void SetEyeHookLeanFollow(bool enabled) {
    g_leanFollow.store(enabled, std::memory_order_release);
    Tf2VrLog(enabled
        ? "[TF2VR] eye.lean_follow = 1: the SERVER eye follows the tracked head offset, so a round "
          "fired while leaning leaves the same point the reticle is drawn from.\n"
        : "[TF2VR] eye.lean_follow = 0: the shot origin ignores the lean. Leaning will put impacts "
          "off the mark by atan(lean / range) -- left lean, impacts right.\n");
}
