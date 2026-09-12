#include "use_target.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "aim_cmd.h"
#include "diagnostics.h"
#include "hand_pose.h"
#include "hook_registry.h"

namespace {

// ---- what pescan read out of the shipped binaries (2026-09-05) --------------
//
// TWO SIDES, ONE SHAPE. The prompt (what is highlighted) is chosen on the
// CLIENT; the action (what X actually picks up) is chosen on the SERVER, in
// the same process, by a twin of the same search. The first headset run
// proved it: with only the client wrapped, the highlight followed the gun and
// the swap picked a different item. Both selectors call EyePosition, then
// EyeAngles, then search(player, &origin, &angles, range, flag). Both twins
// convert the angles with AngleVectors as their first call, read the flag as
// a byte at [rax+0x28] of the entry rsp, and have exactly one caller and no
// vtable reference.
//
//   CLIENT search client.dll+0x2C44B0  (extent: 2483 bytes, 9 fragments)
//     caller 0x2C51EB `call 2C44B0`; EyeAngles `call [rax+0x5B0]` at 0x2C51D0
//     16 displaced bytes: mov rax,rsp / push rbp,rdi,r12,r14 / lea rbp,[rax-0x2B8]
//   SERVER search server.dll+0x5CFD00  (extent: 1705 bytes, 9 fragments)
//     caller 0x5D0777 `call 5CFD00`; EyeAngles `call [rax+0x438]` at 0x5D075C
//     15 displaced bytes: mov rax,rsp / push rbp,r12,r15 / lea rbp,[rax-0x278]
//
// Every displaced byte is a whole instruction with no rip-relative operand, so
// the trampoline replays them unchanged and resumes at the `sub rsp` that
// follows.
struct SideSpec {
    const char* module;
    std::uintptr_t searchRva;
    std::size_t displaced;
    std::uint8_t expectedEntry[16];
    std::uintptr_t callSiteRva;
    std::uint8_t expectedCallSite[5];
    std::uintptr_t eyeAnglesCallRva;
    std::uint8_t expectedEyeAnglesCall[6];
};
constexpr SideSpec kSides[2] = {
    {"client.dll", 0x2C44B0, 16,
     {0x48, 0x8B, 0xC4, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56, 0x48, 0x8D, 0xA8, 0x48, 0xFD, 0xFF, 0xFF},
     0x2C51EB, {0xE8, 0xC0, 0xF2, 0xFF, 0xFF},
     0x2C51D0, {0xFF, 0x90, 0xB0, 0x05, 0x00, 0x00}},
    {"server.dll", 0x5CFD00, 15,
     {0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0x88, 0xFD, 0xFF, 0xFF, 0x00},
     0x5D0777, {0xE8, 0x84, 0xF5, 0xFF, 0xFF},
     0x5D075C, {0xFF, 0x90, 0x38, 0x04, 0x00, 0x00}},
};
constexpr const char* kSideName[2] = {"CLIENT", "SERVER"};
constexpr std::size_t kJumpBytes = 14;  // FF 25 00000000 + abs64

// search(player, &origin, &angles, range, flag) -> entity or null. rcx, rdx,
// r8, xmm3, then one byte at [rsp+0x28] on entry (the callers write `mov
// [rsp+0x20], dil/sil`).
using SearchFn = void*(__fastcall*)(void* player, float* origin, float* angles, float range,
                                    std::uint8_t flag);

// ---- state ------------------------------------------------------------------
std::atomic_bool g_wanted{true};
std::atomic<int> g_arm{0};
constexpr int kSamplesPerArm = 3;

struct Side {
    SearchFn original = nullptr;
    std::uint8_t* base = nullptr;
    std::uint8_t* site = nullptr;
    std::uint8_t* trampoline = nullptr;
    std::atomic_bool installed{false};
    std::atomic_bool refused{false};
    // Per-stage counters. Every early return in the interceptor bumps one.
    std::atomic_uint64_t calls{0};
    std::atomic_uint64_t noPose{0};
    std::atomic_uint64_t off{0};
    std::atomic_uint64_t alongHand{0};
    std::atomic_uint64_t fromHand{0};
    std::atomic_uint64_t hits{0};
    std::atomic_uint64_t faulted{0};
    std::atomic_uint64_t nullArgs{0};
    // The measurement: what the game passed versus the hand, last seen.
    std::atomic<float> lastGameYaw{0.0f}, lastGamePitch{0.0f};
    std::atomic<float> lastHandYaw{0.0f}, lastHandPitch{0.0f};
    std::atomic<float> lastDeltaYaw{0.0f}, lastDeltaPitch{0.0f};
    std::atomic<float> maxAbsDeltaYaw{0.0f};
    std::atomic<int> samplesLeft{kSamplesPerArm};
    // Heartbeat bookkeeping (plugin thread only).
    std::uint64_t previousCalls = 0;
};
Side g_side[2];

float WrapDegrees(float d) {
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

const char* ArmName(int arm) {
    switch (arm) {
        case 1: return "1 = ALONG THE HAND (eye origin, hand angles: the reticle ray)";
        case 2: return "2 = FROM THE HAND (hand origin, hand angles)";
        default: return "0 = OFF (pass-through, measuring only)";
    }
}

// SEH-only helper: no C++ objects, so __try is legal here. Returns false when
// the hand pose is not valid or a read faulted (then the caller passes through
// untouched). PODs only.
bool ReadHandAndMeasure(Side& side, const float* gameAngles, float handPos[3], float handAngles[3]) {
    __try {
        if (!TryGetComposedHandPose(handPos, handAngles)) return false;
        const float dYaw = WrapDegrees(handAngles[1] - gameAngles[1]);
        const float dPitch = WrapDegrees(handAngles[0] - gameAngles[0]);
        side.lastGamePitch.store(gameAngles[0], std::memory_order_relaxed);
        side.lastGameYaw.store(gameAngles[1], std::memory_order_relaxed);
        side.lastHandPitch.store(handAngles[0], std::memory_order_relaxed);
        side.lastHandYaw.store(handAngles[1], std::memory_order_relaxed);
        side.lastDeltaYaw.store(dYaw, std::memory_order_relaxed);
        side.lastDeltaPitch.store(dPitch, std::memory_order_relaxed);
        const float a = std::fabs(dYaw);
        if (a > side.maxAbsDeltaYaw.load(std::memory_order_relaxed)) {
            side.maxAbsDeltaYaw.store(a, std::memory_order_relaxed);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        side.faulted.fetch_add(1, std::memory_order_relaxed);
        g_arm.store(0, std::memory_order_release);
        return false;
    }
}

void LogSample(int n, int arm, const float* game, const float* hand, const float* handPos,
               const float* origin, float range, std::uint8_t flag, void* result) {
    float engine[3]{};
    const bool haveEngine = TryGetEngineViewAngles(engine);
    char line[580]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] use.aim SAMPLE %s arm %d: game passed angles <%.1f,%.1f,%.1f> | hand <%.1f,%.1f,%.1f> "
        "(delta yaw %+.1f pitch %+.1f) | engine view %s<%.1f,%.1f,%.1f> | origin <%.0f,%.0f,%.0f> "
        "hand pos <%.0f,%.0f,%.0f> | range %.0f flag %u | searched %s | result %s\n",
        kSideName[n], arm, game[0], game[1], game[2], hand[0], hand[1], hand[2],
        WrapDegrees(hand[1] - game[1]), WrapDegrees(hand[0] - game[0]),
        haveEngine ? "" : "(none) ", engine[0], engine[1], engine[2],
        origin[0], origin[1], origin[2], handPos[0], handPos[1], handPos[2], range,
        static_cast<unsigned>(flag),
        arm >= 2 ? "FROM the hand along the hand"
                 : arm == 1 ? "from the eye ALONG THE HAND" : "as the game asked (OFF)",
        result ? "an entity" : "nothing");
    Tf2VrLog(line);
}

template <int N>
void* __fastcall SearchInterceptor(void* player, float* origin, float* angles, float range,
                                   std::uint8_t flag) {
    Side& side = g_side[N];
    side.calls.fetch_add(1, std::memory_order_relaxed);
    if (!angles || !origin) {
        side.nullArgs.fetch_add(1, std::memory_order_relaxed);
        return side.original(player, origin, angles, range, flag);
    }
    float handPos[3]{};
    float hand[3]{};
    if (!ReadHandAndMeasure(side, angles, handPos, hand)) {
        side.noPose.fetch_add(1, std::memory_order_relaxed);
        return side.original(player, origin, angles, range, flag);
    }
    const int arm = g_arm.load(std::memory_order_acquire);
    float* useOrigin = origin;
    float* useAngles = angles;
    float dir[3] = {hand[0], hand[1], hand[2]};
    float org[3] = {handPos[0], handPos[1], handPos[2]};
    if (arm <= 0) {
        side.off.fetch_add(1, std::memory_order_relaxed);
    } else {
        useAngles = dir;
        side.alongHand.fetch_add(1, std::memory_order_relaxed);
        if (arm >= 2) {
            useOrigin = org;
            side.fromHand.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void* result = side.original(player, useOrigin, useAngles, range, flag);
    if (result) side.hits.fetch_add(1, std::memory_order_relaxed);
    if (side.samplesLeft.load(std::memory_order_relaxed) > 0) {
        side.samplesLeft.fetch_sub(1, std::memory_order_relaxed);
        LogSample(N, arm, angles, hand, handPos, origin, range, flag, result);
    }
    return result;
}

bool CheckBytes(int n, const char* what, std::uintptr_t rva, const std::uint8_t* expected, std::size_t count) {
    Side& side = g_side[n];
    const std::uint8_t* at = side.base + rva;
    if (std::memcmp(at, expected, count) == 0) return true;
    char line[420]{};
    int used = std::snprintf(line, sizeof(line),
        "[TF2VR] use.aim %s: %s at %s+0x%llX does not begin with the bytes it was resolved "
        "from -- not the build those offsets came from. REFUSED, nothing patched. Found:",
        kSideName[n], what, kSides[n].module, static_cast<unsigned long long>(rva));
    for (std::size_t i = 0; i < count && used < static_cast<int>(sizeof(line)) - 8; ++i) {
        used += std::snprintf(line + used, sizeof(line) - used, " %02X", at[i]);
    }
    std::snprintf(line + used, sizeof(line) - used, "\n");
    Tf2VrLog(line);
    side.refused.store(true, std::memory_order_release);
    return false;
}

void Refuse(int n, const char* why) {
    char line[240]{};
    std::snprintf(line, sizeof(line), "[TF2VR] use.aim %s: %s; nothing patched. REFUSED.\n", kSideName[n], why);
    Tf2VrLog(line);
    g_side[n].refused.store(true, std::memory_order_release);
}

bool InstallSide(int n) {
    Side& side = g_side[n];
    const SideSpec& spec = kSides[n];
    if (side.installed.load(std::memory_order_acquire) || side.refused.load(std::memory_order_acquire)) return false;
    HMODULE module = GetModuleHandleA(spec.module);
    if (!module) return false;
    side.base = reinterpret_cast<std::uint8_t*>(module);

    if (!CheckBytes(n, "the use search entry", spec.searchRva, spec.expectedEntry, spec.displaced)) return false;
    if (!CheckBytes(n, "its one call site", spec.callSiteRva, spec.expectedCallSite, sizeof(spec.expectedCallSite))) return false;
    if (!CheckBytes(n, "the EyeAngles call feeding it", spec.eyeAnglesCallRva, spec.expectedEyeAnglesCall,
                    sizeof(spec.expectedEyeAnglesCall))) return false;

    auto* site = side.base + spec.searchRva;
    // jmp qword ptr [rip+0] + absolute target, built from a ZEROED array so the
    // disp32 is 0 (aim_cmd.cpp records the crash a 0x90 fill caused). Any
    // displaced byte past the 14-byte jmp is never executed; int3 so a stray
    // jump into it traps instead of running.
    std::uint8_t detour[16]{};
    detour[0] = 0xFF;
    detour[1] = 0x25;
    const auto target = reinterpret_cast<std::uintptr_t>(n == 0 ? &SearchInterceptor<0> : &SearchInterceptor<1>);
    std::memcpy(detour + 6, &target, sizeof(target));
    for (std::size_t i = kJumpBytes; i < spec.displaced; ++i) detour[i] = 0xCC;
    std::uint32_t disp = 0;
    std::memcpy(&disp, detour + 2, sizeof(disp));
    std::uintptr_t encoded = 0;
    std::memcpy(&encoded, detour + 6, sizeof(encoded));
    if (disp != 0 || encoded != target || spec.displaced < kJumpBytes || spec.displaced > sizeof(detour)) {
        Refuse(n, "constructed detour is malformed");
        return false;
    }

    auto* trampoline = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        Refuse(n, "could not allocate a trampoline");
        return false;
    }
    std::memcpy(trampoline, site, spec.displaced);
    std::uint8_t* jump = trampoline + spec.displaced;
    jump[0] = 0xFF;
    jump[1] = 0x25;
    std::memset(jump + 2, 0, 4);
    const auto resume = reinterpret_cast<std::uintptr_t>(site + spec.displaced);
    std::memcpy(jump + 6, &resume, sizeof(resume));
    FlushInstructionCache(GetCurrentProcess(), trampoline, 64);
    side.original = reinterpret_cast<SearchFn>(trampoline);
    side.trampoline = trampoline;
    RegisterHookSite(n == 0 ? "use.aim CLIENT search trampoline" : "use.aim SERVER search trampoline", trampoline, 64);
    RegisterHookSite(n == 0 ? "use.aim CLIENT search patched entry" : "use.aim SERVER search patched entry", site,
                     spec.displaced);

    DWORD oldProtect = 0;
    if (!VirtualProtect(site, spec.displaced, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Refuse(n, "VirtualProtect failed on the search entry");
        return false;
    }
    std::memcpy(site, detour, spec.displaced);
    FlushInstructionCache(GetCurrentProcess(), site, spec.displaced);
    DWORD ignored = 0;
    VirtualProtect(site, spec.displaced, oldProtect, &ignored);
    side.site = site;
    side.installed.store(true, std::memory_order_release);

    char line[480]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] use.aim %s INSTALLED: wrapped the use search at %s+0x%llX (%zu bytes displaced, "
        "trampoline %p, resume +0x%llX). Call site 0x%llX and the EyeAngles call at 0x%llX verified. "
        "Arm now %s. F6 steps 0 -> 1 -> 2 -> 0.\n",
        kSideName[n], spec.module, static_cast<unsigned long long>(spec.searchRva), spec.displaced,
        static_cast<void*>(trampoline), static_cast<unsigned long long>(spec.searchRva + spec.displaced),
        static_cast<unsigned long long>(spec.callSiteRva), static_cast<unsigned long long>(spec.eyeAnglesCallRva),
        ArmName(g_arm.load(std::memory_order_acquire)));
    Tf2VrLog(line);
    return true;
}

void Heartbeat(bool inMap) {
    static std::uint64_t nextMs = 0;
    static bool wasInMap = false;
    const std::uint64_t now = GetTickCount64();
    if (now < nextMs) return;
    nextMs = now + 5000;
    const int arm = g_arm.load(std::memory_order_acquire);
    for (int n = 0; n < 2; ++n) {
        Side& side = g_side[n];
        if (!side.installed.load(std::memory_order_acquire)) {
            if (side.refused.load(std::memory_order_acquire)) continue;  // said why, once
            if (inMap) {
                char line[200]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] use.aim %s heartbeat: NOT INSTALLED while in a map -- %s absent?\n",
                    kSideName[n], kSides[n].module);
                Tf2VrLog(line);
            }
            continue;
        }
        const std::uint64_t calls = side.calls.load(std::memory_order_relaxed);
        const std::uint64_t delta = calls - side.previousCalls;
        side.previousCalls = calls;
        const std::uint64_t noPose = side.noPose.load(std::memory_order_relaxed);
        const char* verdict = "";
        if (n == 1 && delta == 0) {
            // The server side only searches while X is held: silence is the
            // normal state, not a dead wrap.
            verdict = " | idle (the server searches only while USE is held)";
        } else if (inMap && wasInMap && delta == 0) {
            verdict = " | VERDICT: wrap NOT FIRING in a map -- the selector's own gates rejected every "
                      "frame (dead, in a Titan, or a menu) or the patch is dead";
        } else if (delta > 0 && noPose == calls) {
            verdict = " | VERDICT: fires, but the hand pose was NEVER valid -- every call passed through. "
                      "That is hand.source / the headset, not this wrap";
        } else if (delta > 0 && arm >= 1 && side.maxAbsDeltaYaw.load(std::memory_order_relaxed) < 0.5f) {
            verdict = " | VERDICT: the game's angles already EQUAL the hand's (max |delta yaw| < 0.5 deg) -- "
                      "arm 1 is a no-op here";
        } else if (delta > 0 && arm >= 1) {
            verdict = " | substituting";
        }
        char line[660]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] use.aim %s heartbeat: arm %d | calls %llu (+%llu/5s) | along-hand %llu | from-hand %llu | "
            "no-pose %llu | off %llu | hits %llu | null-args %llu | faulted %llu | last game <p%.1f y%.1f> "
            "hand <p%.1f y%.1f> delta yaw %+.1f pitch %+.1f | max |delta yaw| %.1f | %s%s\n",
            kSideName[n], arm, static_cast<unsigned long long>(calls), static_cast<unsigned long long>(delta),
            static_cast<unsigned long long>(side.alongHand.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(side.fromHand.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(noPose),
            static_cast<unsigned long long>(side.off.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(side.hits.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(side.nullArgs.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(side.faulted.load(std::memory_order_relaxed)),
            side.lastGamePitch.load(std::memory_order_relaxed), side.lastGameYaw.load(std::memory_order_relaxed),
            side.lastHandPitch.load(std::memory_order_relaxed), side.lastHandYaw.load(std::memory_order_relaxed),
            side.lastDeltaYaw.load(std::memory_order_relaxed), side.lastDeltaPitch.load(std::memory_order_relaxed),
            side.maxAbsDeltaYaw.load(std::memory_order_relaxed), inMap ? "in a map" : "not in a map", verdict);
        Tf2VrLog(line);
    }
    wasInMap = inMap;
}

void ResetSamples() {
    for (auto& side : g_side) side.samplesLeft.store(kSamplesPerArm, std::memory_order_release);
}

}  // namespace

void UseTargetTick(bool inMap) {
    if (!g_wanted.load(std::memory_order_acquire)) return;
    InstallSide(0);
    InstallSide(1);
    Heartbeat(inMap);
}

void SetUseTargetHookWanted(bool wanted) {
    g_wanted.store(wanted, std::memory_order_release);
    Tf2VrLog(wanted ? "[TF2VR] use.hook = 1: the use-search wrap installs on client.dll and server.dll as they appear.\n"
                    : "[TF2VR] use.hook = 0: the use search is left untouched on both sides (nothing patched).\n");
}

void SetUseAimArm(int arm) {
    if (arm < 0) arm = 0;
    if (arm > 2) arm = 2;
    g_arm.store(arm, std::memory_order_release);
    ResetSamples();
    char line[300]{};
    std::snprintf(line, sizeof(line), "[TF2VR] use.aim = %d: the use search runs %s.%s%s\n", arm, ArmName(arm),
                  g_side[0].installed.load(std::memory_order_acquire) ? "" : " (client wrap not installed yet; applies when it is)",
                  g_side[1].installed.load(std::memory_order_acquire) ? "" : " (server wrap not installed yet; server.dll arrives with the first level)");
    Tf2VrLog(line);
}

int UseAimArm() { return g_arm.load(std::memory_order_acquire); }

void StepUseAim() {
    const int next = (g_arm.load(std::memory_order_acquire) + 1) % 3;
    g_arm.store(next, std::memory_order_release);
    ResetSamples();
    char line[360]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F6: use.aim -> %s. CLIENT calls %llu along-hand %llu no-pose %llu | SERVER calls %llu "
        "along-hand %llu no-pose %llu.%s%s\n", ArmName(next),
        static_cast<unsigned long long>(g_side[0].calls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_side[0].alongHand.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_side[0].noPose.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_side[1].calls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_side[1].alongHand.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_side[1].noPose.load(std::memory_order_relaxed)),
        g_side[0].installed.load(std::memory_order_acquire) ? "" : " CLIENT WRAP NOT INSTALLED.",
        g_side[1].installed.load(std::memory_order_acquire) ? "" : " SERVER WRAP NOT INSTALLED.");
    Tf2VrLog(line);
}

void RemoveUseTargetHook() {
    for (int n = 0; n < 2; ++n) {
        Side& side = g_side[n];
        if (!side.installed.load(std::memory_order_acquire) || !side.site) continue;
        DWORD old = 0;
        if (VirtualProtect(side.site, kSides[n].displaced, PAGE_EXECUTE_READWRITE, &old)) {
            std::memcpy(side.site, kSides[n].expectedEntry, kSides[n].displaced);
            FlushInstructionCache(GetCurrentProcess(), side.site, kSides[n].displaced);
            DWORD ignored = 0;
            VirtualProtect(side.site, kSides[n].displaced, old, &ignored);
        }
        side.installed.store(false, std::memory_order_release);
        // The trampoline is left allocated: a thread mid-search may still be in it.
    }
}
