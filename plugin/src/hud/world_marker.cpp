#include "world_marker.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "camera_hook.h"
#include "camera_update_hook.h"
#include "diagnostics.h"
#include "hook_registry.h"

// The engine camera's own angles at the seam, before the head is composed in
// (camera_hook.cpp: basePitch/baseYaw/baseRoll, written every camera pass).
extern "C" volatile float g_headBaseAngles[3];

namespace {

// ---- what pescan read out of client.dll (2026-09-05) ------------------------
constexpr std::uintptr_t kTableRva = 0xB21290;  // .data: absolute function pointers
struct TableSlot {
    int index;
    std::uintptr_t originalRva;
    int kind;
};
constexpr TableSlot kSlots[4] = {
    {1, 0x27A500, 1},
    {2, 0x27A500, 1},
    {10, 0x27A530, 3},
    {13, 0x27A5A0, 4},
};
// kind 2: `call 0x27CA80` at 0x309AFD with rcx = &arg[+0x40], writes in place.
constexpr std::uintptr_t kAttachRva = 0x27CA80;
constexpr std::size_t kAttachDisplaced = 15;
constexpr std::uint8_t kAttachPrologue[kAttachDisplaced] = {
    0x48, 0x89, 0x5C, 0x24, 0x08,   // mov [rsp+8], rbx
    0x48, 0x89, 0x6C, 0x24, 0x10,   // mov [rsp+10h], rbp
    0x48, 0x89, 0x74, 0x24, 0x18};  // mov [rsp+18h], rsi
constexpr std::uintptr_t kAttachCallSiteRva = 0x309AFD;
constexpr std::uint8_t kAttachCallSite[5] = {0xE8, 0x7E, 0x2F, 0xF7, 0xFF};
// The draw loop itself, .pdata root 0x309820 (965 bytes, 3 fragments); its
// first bytes are checked as the build-identity control for the table.
constexpr std::uintptr_t kDrawLoopRva = 0x309820;

using TableFn = float*(__fastcall*)(float* out, void* binding, void* r8, void* r9);
using AttachFn = void*(__fastcall*)(float* out, void* entity, int a, int b);

// ---- state ------------------------------------------------------------------
std::atomic_bool g_wanted{true};
std::atomic_bool g_installed{false};
std::atomic_bool g_refused{false};
std::atomic<int> g_arm{0};
// The fraction of the head's yaw delta the reframe applies. 1.0 is the exact
// value when the engine projects in the pure body frame; run 1 says it does
// not quite. See Reframe. F6 steps this ladder, arm 0 is the OFF rung.
std::atomic<float> g_gain{1.0f};
constexpr float kGainLadder[4] = {1.0f, 0.85f, 0.70f, 0.55f};
std::uint8_t* g_client = nullptr;
TableFn g_originalTable[4] = {};
void** g_slotAddress[4] = {};
AttachFn g_originalAttach = nullptr;
std::uint8_t* g_attachSite = nullptr;
std::uint8_t* g_attachTrampoline = nullptr;

// Per-kind counters (index = kind 1..4; 0 unused).
std::atomic_uint64_t g_calls[5]{};
std::atomic_uint64_t g_reframed[5]{};
std::atomic_uint64_t g_noView{0};     // wanted to reframe, no origin or no head published
std::atomic_uint64_t g_off{0};        // arm 0
std::atomic_uint64_t g_faulted{0};
std::atomic_uint64_t g_unchanged{0};  // kind 2 wrote nothing (attachment not found)
std::atomic<float> g_maxShift{0.0f};
// Last values for the instrument (racy floats, diagnostic only).
volatile float g_lastP[3] = {0, 0, 0};
volatile float g_lastPPrime[3] = {0, 0, 0};
volatile float g_lastE[3] = {0, 0, 0};
volatile float g_lastBody[2] = {0, 0};
volatile float g_lastHead[2] = {0, 0};
volatile int g_lastKind = 0;
std::atomic_uint64_t g_lineBudgetMs{0};
std::atomic_uint64_t g_lines{0};

float WrapDegrees(float d) {
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

// YAW ONLY, AND THE PITCH TERM IS DELETED RATHER THAN TUNED. Run 1 shipped
// P' = E + R_body R_head^T (P - E), a full basis change, and its pitch half is
// the "flattened U" the wearer saw: the two frames' PITCHES differ (logged
// body p5.91 against head p12.0, and body p-85.00 later in the same run), so
// the composition tilted the point about the body's left axis even with the
// head yaw-centred. Measured from the run's own line WMARK#331: P z 437.3 ->
// P' z 622.7, a 185.4-unit lift at 2357 units of range = 4.50 deg of vertical
// error, swept into a shallow parabola as the head turns. The engine's own
// projection evidently handles pitch already; only the YAW frames disagree.
//
// So the correction is a rotation about world Z through the view origin:
//     P' = E + Rz(-gain * headYawDelta) * (P - E)
// At gain 1 and a body-frame projection it is exact -- checked by hand against
// WMARK#331: marker azimuth 80.63, theta 5.75, body-frame screen angle after
// the rotation +0.62 deg against a head-frame target of +0.62 deg.
//
// THE GAIN EXISTS BECAUSE A RUN DEMANDED IT, not as a spare knob. At gain 1
// the wearer reported the residual REVERSED (marker now drifts against the
// head) and SHRANK, which is what an engine projection frame that already
// carries a fraction a of the head rotation produces: error before = (1-a)*t,
// error at gain 1 = -a*t, and the exact gain is 1-a. Reversed-and-smaller puts
// a below 0.5, so the gain lives in 0.5..1.0 and F6 steps it.
bool Reframe(float p[3]) {
    float e[3]{};
    if (!TryGetMainSceneOrigin(e)) GameCameraBasePosition(e);
    float yawDelta = 0.0f, headPitch = 0.0f;
    if (!GetHeadViewDelta(&yawDelta, &headPitch)) return false;
    const float bodyYaw = g_headBaseAngles[1];
    const float headYaw = WrapDegrees(bodyYaw + yawDelta);
    const float turn = -yawDelta * g_gain.load(std::memory_order_relaxed);
    const float k = 3.14159265358979f / 180.0f;
    const float s = std::sin(turn * k), c = std::cos(turn * k);
    const float v[3] = {p[0] - e[0], p[1] - e[1], p[2] - e[2]};
    // Rz: x' = x c - y s, y' = x s + y c, z untouched.
    const float q[3] = {e[0] + v[0] * c - v[1] * s, e[1] + v[0] * s + v[1] * c, e[2] + v[2]};
    const float shift = std::sqrt((q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) +
                                  (q[2] - p[2]) * (q[2] - p[2]));
    if (shift > g_maxShift.load(std::memory_order_relaxed)) g_maxShift.store(shift, std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i) { g_lastP[i] = p[i]; g_lastPPrime[i] = q[i]; g_lastE[i] = e[i]; }
    g_lastBody[0] = g_headBaseAngles[0]; g_lastBody[1] = bodyYaw;
    g_lastHead[0] = headPitch; g_lastHead[1] = headYaw;
    p[0] = q[0]; p[1] = q[1]; p[2] = q[2];
    return true;
}

void MaybeLine(int kind, bool applied, const float* original) {
    const std::uint64_t now = GetTickCount64();
    if (now < g_lineBudgetMs.load(std::memory_order_relaxed)) return;
    g_lineBudgetMs.store(now + 250, std::memory_order_relaxed);
    // THE DERIVED ANGLES, NOT THE WIDGET'S BLOCK. Run 1 printed the watched
    // widget's produced screen position on this line and it read one of two
    // clamp constants on every single sample (265 x <746.5,36.3>, 27 x
    // <1920,1080>, 86 absent) -- the field saturates and cannot fit anything.
    // What IS decidable from here is the geometry: where the marker sits
    // relative to the body's forward and relative to the head's. The reframe's
    // whole claim is that the second is what the wearer should see, so the two
    // numbers and their difference are the model, and the wearer's eyes are
    // the measurement of which one the engine actually drew.
    const float deg = 57.2957795f;
    const float ox = original[0] - g_lastE[0], oy = original[1] - g_lastE[1];
    const float azimuth = std::atan2(oy, ox) * deg;
    const float fromBody = WrapDegrees(azimuth - g_lastBody[1]);
    const float fromHead = WrapDegrees(azimuth - g_lastHead[1]);
    const float px = g_lastPPrime[0] - g_lastE[0], py = g_lastPPrime[1] - g_lastE[1];
    const float writtenFromBody = WrapDegrees(std::atan2(py, px) * deg - g_lastBody[1]);
    char line[620]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] WMARK#%llu kind %d arm %d gain %.2f %s | P <%.1f,%.1f,%.1f> -> P' <%.1f,%.1f,%.1f> | "
        "E <%.1f,%.1f,%.1f> | body <p%.2f y%.2f> head <p%.2f y%.2f> | marker azimuth %.2f = %+.2f deg off "
        "BODY forward, %+.2f deg off HEAD forward | after the write it is %+.2f deg off body forward "
        "(target %+.2f) | dz %+.1f units | max shift %.1f\n",
        static_cast<unsigned long long>(g_lines.fetch_add(1, std::memory_order_relaxed) + 1), kind,
        g_arm.load(std::memory_order_relaxed), g_gain.load(std::memory_order_relaxed),
        applied ? "REFRAMED" : "as evaluated",
        original[0], original[1], original[2], g_lastPPrime[0], g_lastPPrime[1], g_lastPPrime[2],
        g_lastE[0], g_lastE[1], g_lastE[2], g_lastBody[0], g_lastBody[1], g_lastHead[0], g_lastHead[1],
        azimuth, fromBody, fromHead, writtenFromBody, fromHead, g_lastPPrime[2] - original[2],
        g_maxShift.load(std::memory_order_relaxed));
    Tf2VrLog(line);
}

// SEH-only: PODs, no C++ objects. Applies the arm to one evaluated vec3.
void Apply(int kind, float* p) {
    __try {
        if (!p) return;
        if (!(p[0] == p[0]) || !(p[1] == p[1]) || !(p[2] == p[2])) return;  // NaN guard
        const float original[3] = {p[0], p[1], p[2]};
        const int arm = g_arm.load(std::memory_order_acquire);
        bool applied = false;
        if (arm <= 0) {
            g_off.fetch_add(1, std::memory_order_relaxed);
            // Measure only: run the arithmetic on a copy so the line still says
            // where the reframe WOULD have put it.
            float copy[3] = {p[0], p[1], p[2]};
            if (!Reframe(copy)) g_noView.fetch_add(1, std::memory_order_relaxed);
        } else if (Reframe(p)) {
            g_reframed[kind].fetch_add(1, std::memory_order_relaxed);
            applied = true;
        } else {
            g_noView.fetch_add(1, std::memory_order_relaxed);
        }
        g_lastKind = kind;
        MaybeLine(kind, applied, original);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faulted.fetch_add(1, std::memory_order_relaxed);
        g_arm.store(0, std::memory_order_release);
    }
}

// ---- KIND 4: THE LOCAL PLAYER'S EYE ANGLES, HEAD-COMPOSED (2026-09-07) ------
// The friendly name labels move WITH the head (wearer, three runs). No counted
// position kind carries them (kind 3 = 0 all run), our widget transform leaves
// their widget untouched (classifier), and they are not VGUI (F5). Kind 4 is
// the one seam that fired with them on screen: two evaluations a frame, each
// copying an entity's EyeAngles into the argument buffer (0x27A5A0 copies 12
// bytes into rcx and returns it, so the write below touches the copy, never
// the entity). If a label's placement is computed from the LOCAL player's eye
// angles, that is a body-frame input in a head-frame draw. Armed: when the
// evaluated angles equal the camera's base angles (the game's own view before
// the head is composed in), the copy gets the head-composed pitch and yaw.
// Roll and every other entity's angles are untouched. The match count is the
// control: zero matches with labels on screen means kind 4 is not the
// player's angles and the arm is VOID, which the heartbeat says in words.
std::atomic_uint64_t g_eyeLocal{0}, g_eyeOther{0}, g_eyeNoHead{0};
std::atomic<std::uint64_t> g_eyeLineMs{0};

void EyeAngles(float* a) {
    __try {
        if (!a) return;
        const float basePitch = g_headBaseAngles[0], baseYaw = g_headBaseAngles[1];
        const float inP = a[0], inY = a[1];
        const float dp = WrapDegrees(inP - basePitch), dy = WrapDegrees(inY - baseYaw);
        const bool local = std::fabs(dp) < 0.5f && std::fabs(dy) < 0.5f;
        if (local) g_eyeLocal.fetch_add(1, std::memory_order_relaxed);
        else g_eyeOther.fetch_add(1, std::memory_order_relaxed);
        float yawDelta = 0.0f, headPitch = 0.0f;
        const bool haveHead = GetHeadViewDelta(&yawDelta, &headPitch);
        const float headYaw = WrapDegrees(baseYaw + yawDelta);
        // READ-ONLY. `marker.eyes` was ELIMINATED 2026-09-08: 3429 evaluations were
        // rewritten to head angles with the labels on screen and the wearer saw no
        // change, so kind 4 is not the placement input. The arm and its ini key are
        // deleted; the census stays because it is the only seam that fires with
        // name labels up, and it now measures without ever writing.
        if (!haveHead) g_eyeNoHead.fetch_add(1, std::memory_order_relaxed);
        const std::uint64_t now = GetTickCount64();
        if (now >= g_eyeLineMs.load(std::memory_order_relaxed)) {
            g_eyeLineMs.store(now + 1000, std::memory_order_relaxed);
            char line[360]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] EYES kind4 in <p%.2f y%.2f> base <p%.2f y%.2f> head <p%.2f y%.2f> %s | READ-ONLY | "
                "local %llu other %llu nohead %llu\n",
                inP, inY, basePitch, baseYaw, headPitch, headYaw, local ? "LOCAL PLAYER" : "other entity",
                static_cast<unsigned long long>(g_eyeLocal.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_eyeOther.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_eyeNoHead.load(std::memory_order_relaxed)));
            Tf2VrLog(line);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faulted.fetch_add(1, std::memory_order_relaxed);
    }
}

void EyesHeartbeat(std::uint64_t kind4) {
    const auto local = g_eyeLocal.load(std::memory_order_relaxed);
    const char* verdict = kind4 == 0 ? "kind 4 never evaluated: nothing tracks eye angles right now"
                        : local == 0 ? "NO evaluation matched the camera base: kind 4 is not the player's angles here"
                                     : "matches found; READ-ONLY census (the rewrite arm was eliminated 2026-09-08)";
    char line[320]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] EYES heartbeat: kind4 %llu local %llu other %llu nohead %llu | %s\n",
        static_cast<unsigned long long>(kind4), static_cast<unsigned long long>(local),
        static_cast<unsigned long long>(g_eyeOther.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_eyeNoHead.load(std::memory_order_relaxed)), verdict);
    Tf2VrLog(line);
}

template <int Slot>
float* __fastcall TableInterceptor(float* out, void* binding, void* r8, void* r9) {
    float* result = g_originalTable[Slot](out, binding, r8, r9);
    const int kind = kSlots[Slot].kind;
    g_calls[kind].fetch_add(1, std::memory_order_relaxed);
    if (kind == 4) {  // eye angles: the copy in the argument buffer, see EyeAngles
        EyeAngles(result ? result : out);
        return result;
    }
    Apply(kind, result ? result : out);
    return result;
}

void* __fastcall AttachInterceptor(float* out, void* entity, int a, int b) {
    float before[3] = {0, 0, 0};
    if (out) { before[0] = out[0]; before[1] = out[1]; before[2] = out[2]; }
    void* result = g_originalAttach(out, entity, a, b);
    g_calls[2].fetch_add(1, std::memory_order_relaxed);
    if (!out || (out[0] == before[0] && out[1] == before[1] && out[2] == before[2])) {
        g_unchanged.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    Apply(2, out);
    return result;
}

void Refuse(const char* why) {
    char line[300]{};
    std::snprintf(line, sizeof(line), "[TF2VR] marker: %s. REFUSED, nothing patched.\n", why);
    Tf2VrLog(line);
    g_refused.store(true, std::memory_order_release);
}

bool Install() {
    if (g_installed.load(std::memory_order_acquire) || g_refused.load(std::memory_order_acquire)) return false;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return false;
    g_client = reinterpret_cast<std::uint8_t*>(client);

    // Build identity: the draw loop's own first bytes, and the direct call site.
    static constexpr std::uint8_t kDrawLoopHead[8] = {0x88, 0x4C, 0x24, 0x08, 0x53, 0x55, 0x56, 0x57};
    if (std::memcmp(g_client + kDrawLoopRva, kDrawLoopHead, sizeof(kDrawLoopHead)) != 0) {
        Refuse("the RUI draw loop at client.dll+0x309820 does not begin with the bytes it was resolved from");
        return false;
    }
    if (std::memcmp(g_client + kAttachCallSiteRva, kAttachCallSite, sizeof(kAttachCallSite)) != 0) {
        Refuse("the kind-2 call site at client.dll+0x309AFD is not `call 0x27CA80`");
        return false;
    }
    if (std::memcmp(g_client + kAttachRva, kAttachPrologue, kAttachDisplaced) != 0) {
        Refuse("the kind-2 evaluator at client.dll+0x27CA80 does not begin with its expected prologue");
        return false;
    }
    // Every table slot must still hold its original before anything is swapped.
    for (int i = 0; i < 4; ++i) {
        auto** slot = reinterpret_cast<void**>(g_client + kTableRva + 8 * kSlots[i].index);
        const void* expected = g_client + kSlots[i].originalRva;
        if (*slot != expected) {
            char line[300]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] marker: vec3 table[%d] holds %p, expected client.dll+0x%llX = %p. REFUSED -- "
                "different build, or another patcher is in the table.\n",
                kSlots[i].index, *slot, static_cast<unsigned long long>(kSlots[i].originalRva), expected);
            Tf2VrLog(line);
            g_refused.store(true, std::memory_order_release);
            return false;
        }
        g_slotAddress[i] = slot;
        g_originalTable[i] = reinterpret_cast<TableFn>(*slot);
    }

    // Kind 2: entry detour, same construction as use_target.cpp / aim_cmd.cpp.
    auto* site = g_client + kAttachRva;
    std::uint8_t detour[kAttachDisplaced]{};
    detour[0] = 0xFF;
    detour[1] = 0x25;
    const auto target = reinterpret_cast<std::uintptr_t>(&AttachInterceptor);
    std::memcpy(detour + 6, &target, sizeof(target));
    detour[14] = 0xCC;
    std::uint32_t disp = 0;
    std::memcpy(&disp, detour + 2, sizeof(disp));
    if (disp != 0) { Refuse("constructed detour is malformed"); return false; }
    auto* trampoline = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) { Refuse("could not allocate a trampoline"); return false; }
    std::memcpy(trampoline, site, kAttachDisplaced);
    std::uint8_t* jump = trampoline + kAttachDisplaced;
    jump[0] = 0xFF;
    jump[1] = 0x25;
    std::memset(jump + 2, 0, 4);
    const auto resume = reinterpret_cast<std::uintptr_t>(site + kAttachDisplaced);
    std::memcpy(jump + 6, &resume, sizeof(resume));
    FlushInstructionCache(GetCurrentProcess(), trampoline, 64);
    g_originalAttach = reinterpret_cast<AttachFn>(trampoline);
    g_attachTrampoline = trampoline;
    RegisterHookSite("marker kind-2 evaluator trampoline", trampoline, 64);
    RegisterHookSite("marker kind-2 evaluator patched entry", site, kAttachDisplaced);
    DWORD oldProtect = 0;
    if (!VirtualProtect(site, kAttachDisplaced, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Refuse("VirtualProtect failed on the kind-2 evaluator");
        return false;
    }
    std::memcpy(site, detour, kAttachDisplaced);
    FlushInstructionCache(GetCurrentProcess(), site, kAttachDisplaced);
    DWORD ignored = 0;
    VirtualProtect(site, kAttachDisplaced, oldProtect, &ignored);
    g_attachSite = site;

    // Table swaps (pointer data; .data is writable, protected anyway).
    void* interceptors[4] = {
        reinterpret_cast<void*>(&TableInterceptor<0>), reinterpret_cast<void*>(&TableInterceptor<1>),
        reinterpret_cast<void*>(&TableInterceptor<2>), reinterpret_cast<void*>(&TableInterceptor<3>)};
    for (int i = 0; i < 4; ++i) {
        DWORD old = 0;
        if (!VirtualProtect(g_slotAddress[i], sizeof(void*), PAGE_READWRITE, &old)) {
            Refuse("VirtualProtect failed on the vec3 table");
            return false;
        }
        InterlockedExchangePointer(g_slotAddress[i], interceptors[i]);
        DWORD ign = 0;
        VirtualProtect(g_slotAddress[i], sizeof(void*), old, &ign);
    }
    g_installed.store(true, std::memory_order_release);
    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] marker INSTALLED: vec3 table client.dll+0x%llX slots 1,2 (kind 1 origin), 10 (kind 3 overhead), "
        "13 (kind 4 angles, counted only) swapped; kind-2 attachment evaluator 0x%llX detoured (%zu bytes, "
        "trampoline %p). Arm %d. F6 toggles the reframe.\n",
        static_cast<unsigned long long>(kTableRva), static_cast<unsigned long long>(kAttachRva),
        kAttachDisplaced, static_cast<void*>(trampoline), g_arm.load(std::memory_order_acquire));
    Tf2VrLog(line);
    return true;
}

void Heartbeat(bool inMap) {
    static std::uint64_t nextMs = 0;
    static std::uint64_t previousTotal = 0;
    static bool wasInMap = false;
    const std::uint64_t now = GetTickCount64();
    if (now < nextMs) return;
    nextMs = now + 5000;
    if (!g_installed.load(std::memory_order_acquire)) {
        if (!g_refused.load(std::memory_order_acquire) && inMap) {
            Tf2VrLog("[TF2VR] marker heartbeat: NOT INSTALLED while in a map -- client.dll absent?\n");
        }
        return;
    }
    std::uint64_t c[5]{}, r[5]{}, total = 0, reframed = 0;
    for (int k = 1; k <= 4; ++k) {
        c[k] = g_calls[k].load(std::memory_order_relaxed);
        r[k] = g_reframed[k].load(std::memory_order_relaxed);
        total += c[k];
        reframed += r[k];
    }
    const std::uint64_t delta = total - previousTotal;
    previousTotal = total;
    const int arm = g_arm.load(std::memory_order_acquire);
    const char* verdict = "";
    if (inMap && wasInMap && delta == 0) {
        verdict = " | no vec3 kind evaluated in 5 s: no world-tracked RUI exists right now (no marker on screen; Y "
                  "long press restores them) or the swaps are dead";
    } else if (delta > 0 && arm >= 1 && reframed == 0) {
        verdict = " | VERDICT: kinds fire but NOTHING is reframed -- no view origin / head delta published "
                  "(camera hook not running?), so the arm is a no-op";
    } else if (delta > 0 && arm >= 1) {
        verdict = " | reframing";
    } else if (delta > 0) {
        verdict = " | measuring only (arm 0)";
    }
    wasInMap = inMap;
    char line[600]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] marker heartbeat: arm %d | kind1 %llu (reframed %llu) kind2 %llu (reframed %llu, unchanged %llu) "
        "kind3 %llu (reframed %llu) kind4 %llu (angles, untouched) | +%llu/5s | off %llu no-view %llu faulted %llu | "
        "max shift %.1f units | head delta y%+.2f | %s%s\n",
        arm, static_cast<unsigned long long>(c[1]), static_cast<unsigned long long>(r[1]),
        static_cast<unsigned long long>(c[2]), static_cast<unsigned long long>(r[2]),
        static_cast<unsigned long long>(g_unchanged.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(c[3]), static_cast<unsigned long long>(r[3]),
        static_cast<unsigned long long>(c[4]), static_cast<unsigned long long>(delta),
        static_cast<unsigned long long>(g_off.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_noView.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_faulted.load(std::memory_order_relaxed)),
        g_maxShift.load(std::memory_order_relaxed), WrapDegrees(g_lastHead[1] - g_lastBody[1]),
        inMap ? "in a map" : "not in a map", verdict);
    Tf2VrLog(line);
    EyesHeartbeat(c[4]);
}

const char* ArmName(int arm) {
    return arm >= 1 ? "REFRAME ON (world point rotated about Z by -gain x head yaw delta before projection)"
                    : "OFF (measuring only; the line still prints where the reframe would put it)";
}

// The F6 ladder: four gains, then OFF, then wrap. One press per rung, each
// naming the rung it entered, so the wearer bisects the gain inside ONE run
// by looking at the marker rather than by a number anyone had to guess.
int g_rung = 0;
constexpr int kRungs = 5;  // 0..3 = kGainLadder, 4 = OFF

}  // namespace

void WorldMarkerTick(bool inMap) {
    if (!g_wanted.load(std::memory_order_acquire)) return;
    Install();
    Heartbeat(inMap);
}

void SetWorldMarkerHookWanted(bool wanted) {
    g_wanted.store(wanted, std::memory_order_release);
    Tf2VrLog(wanted ? "[TF2VR] marker.hook = 1: the vec3 evaluator swaps install once client.dll exists.\n"
                    : "[TF2VR] marker.hook = 0: the RUI vec3 evaluators are left untouched.\n");
}


void SetWorldMarkerArm(int arm) {
    arm = arm >= 1 ? 1 : 0;
    g_arm.store(arm, std::memory_order_release);
    char line[300]{};
    std::snprintf(line, sizeof(line), "[TF2VR] marker.reframe = %d: %s.%s\n", arm, ArmName(arm),
                  g_installed.load(std::memory_order_acquire) ? "" : " (swaps not installed yet; applies when they are)");
    Tf2VrLog(line);
}

int WorldMarkerArm() { return g_arm.load(std::memory_order_acquire); }

void SetWorldMarkerGain(float gain) {
    if (!(gain >= 0.0f && gain <= 2.0f)) return;
    g_gain.store(gain, std::memory_order_release);
    // Keep the ladder in step with the ini, so the first F6 press moves to the
    // NEXT rung instead of repeating the value already in force.
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(kGainLadder[i] - gain) < 0.001f) { g_rung = i; break; }
    }
    char line[320]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] marker.gain = %.2f: the reframe applies this fraction of the head yaw delta. 1.00 is exact "
        "if the engine projects in the pure body frame; run 1 says it does not quite. F6 steps the ladder.\n",
        gain);
    Tf2VrLog(line);
}

float WorldMarkerGain() { return g_gain.load(std::memory_order_acquire); }

void StepWorldMarkerArm() {
    g_rung = (g_rung + 1) % kRungs;
    const bool off = (g_rung == kRungs - 1);
    if (off) {
        g_arm.store(0, std::memory_order_release);
    } else {
        g_gain.store(kGainLadder[g_rung], std::memory_order_release);
        g_arm.store(1, std::memory_order_release);
    }
    std::uint64_t total = 0;
    for (int k = 1; k <= 4; ++k) total += g_calls[k].load(std::memory_order_relaxed);
    char line[440]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F6 rung %d of %d: marker reframe %s. vec3 evaluations so far %llu, max shift %.1f.%s\n",
        g_rung + 1, kRungs,
        off ? "OFF (the A/B rung: the game's own placement)" : "ON",
        static_cast<unsigned long long>(total), g_maxShift.load(std::memory_order_relaxed),
        g_installed.load(std::memory_order_acquire) ? "" : " SWAPS NOT INSTALLED: this press changes nothing.");
    Tf2VrLog(line);
    if (!off) {
        char g[160]{};
        std::snprintf(g, sizeof(g), "[TF2VR]   gain now %.2f (rungs: 1.00, 0.85, 0.70, 0.55, OFF).\n",
                      kGainLadder[g_rung]);
        Tf2VrLog(g);
    }
}

void RemoveWorldMarkerHook() {
    if (!g_installed.load(std::memory_order_acquire)) return;
    for (int i = 0; i < 4; ++i) {
        if (!g_slotAddress[i]) continue;
        DWORD old = 0;
        if (VirtualProtect(g_slotAddress[i], sizeof(void*), PAGE_READWRITE, &old)) {
            InterlockedExchangePointer(g_slotAddress[i], reinterpret_cast<void*>(g_originalTable[i]));
            DWORD ign = 0;
            VirtualProtect(g_slotAddress[i], sizeof(void*), old, &ign);
        }
    }
    if (g_attachSite) {
        DWORD old = 0;
        if (VirtualProtect(g_attachSite, kAttachDisplaced, PAGE_EXECUTE_READWRITE, &old)) {
            std::memcpy(g_attachSite, kAttachPrologue, kAttachDisplaced);
            FlushInstructionCache(GetCurrentProcess(), g_attachSite, kAttachDisplaced);
            DWORD ign = 0;
            VirtualProtect(g_attachSite, kAttachDisplaced, old, &ign);
        }
    }
    g_installed.store(false, std::memory_order_release);
}

// ---- the lane census -------------------------------------------------------
namespace {

constexpr int kLanes = 8;             // block +0x40 .. +0x5C
constexpr int kLaneBase = 0x40 / 4;   // index into the block's float view
struct LaneStats {
    double n = 0;
    double sx = 0, sxx = 0;           // the lane
    double syY = 0, syyY = 0, sxyY = 0;  // against head yaw
    double syP = 0, syyP = 0, sxyP = 0;  // against head pitch
    float lo = 0, hi = 0;
    bool seen = false;
};
LaneStats g_lane[kLanes];
std::atomic_uint64_t g_laneSamples{0};
std::atomic_uint64_t g_laneNoView{0};
std::uint64_t g_laneNextReport = 0;
// A few raw rows for the offline fit: the world point actually handed over,
// the view at that instant, and every lane.
struct Row { float p[3], e[3], bodyYaw, headYaw, headPitch, lanes[kLanes]; };
constexpr int kRows = 6;
Row g_row[kRows];
int g_rowCount = 0;
int g_rowSkip = 0;

double Pearson(double n, double sx, double sxx, double sy, double syy, double sxy) {
    if (n < 8) return 0.0;
    const double cov = n * sxy - sx * sy;
    const double vx = n * sxx - sx * sx;
    const double vy = n * syy - sy * sy;
    if (vx <= 1e-9 || vy <= 1e-9) return 0.0;
    return cov / std::sqrt(vx * vy);
}

}  // namespace

void WorldMarkerRecordWidgetDraw(const float* blockFloats) {
    if (!blockFloats) return;
    __try {
        // The view at THIS draw. Without it a sample cannot be correlated, so
        // it is counted and dropped rather than folded in with a stale angle.
        float yawDelta = 0.0f, headPitch = 0.0f;
        if (!GetHeadViewDelta(&yawDelta, &headPitch)) {
            g_laneNoView.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const float bodyYaw = g_headBaseAngles[1];
        const float headYaw = WrapDegrees(bodyYaw + yawDelta);
        for (int i = 0; i < kLanes; ++i) {
            const float v = blockFloats[kLaneBase + i];
            if (!(v == v)) continue;  // NaN
            LaneStats& s = g_lane[i];
            if (!s.seen) { s.lo = v; s.hi = v; s.seen = true; }
            if (v < s.lo) s.lo = v;
            if (v > s.hi) s.hi = v;
            s.n += 1;
            s.sx += v; s.sxx += double(v) * v;
            s.syY += headYaw; s.syyY += double(headYaw) * headYaw; s.sxyY += double(v) * headYaw;
            s.syP += headPitch; s.syyP += double(headPitch) * headPitch; s.sxyP += double(v) * headPitch;
        }
        if (++g_rowSkip >= 37 && g_rowCount < kRows) {
            g_rowSkip = 0;
            Row& r = g_row[g_rowCount++];
            for (int i = 0; i < 3; ++i) { r.p[i] = g_lastPPrime[i]; r.e[i] = g_lastE[i]; }
            r.bodyYaw = bodyYaw; r.headYaw = headYaw; r.headPitch = headPitch;
            for (int i = 0; i < kLanes; ++i) r.lanes[i] = blockFloats[kLaneBase + i];
        }
        g_laneSamples.fetch_add(1, std::memory_order_relaxed);
        const std::uint64_t now = GetTickCount64();
        if (now < g_laneNextReport) return;
        g_laneNextReport = now + 5000;
        char line[520]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] LANES n=%llu (no-view %llu). Correlation of each block lane with HEAD YAW / HEAD "
            "PITCH -- the screen position is the lane that MOVES with the head:\n",
            static_cast<unsigned long long>(g_laneSamples.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_laneNoView.load(std::memory_order_relaxed)));
        Tf2VrLog(line);
        for (int i = 0; i < kLanes; ++i) {
            const LaneStats& s = g_lane[i];
            const double rY = Pearson(s.n, s.sx, s.sxx, s.syY, s.syyY, s.sxyY);
            const double rP = Pearson(s.n, s.sx, s.sxx, s.syP, s.syyP, s.sxyP);
            std::snprintf(line, sizeof(line),
                "[TF2VR]   lane blk+0x%02X  r(yaw) %+.3f  r(pitch) %+.3f  range %.2f .. %.2f  span %.2f%s\n",
                0x40 + 4 * i, rY, rP, s.lo, s.hi, s.hi - s.lo,
                (rY > 0.7 || rY < -0.7) ? "   <== TRACKS THE HEAD" : "");
            Tf2VrLog(line);
        }
        for (int i = 0; i < g_rowCount; ++i) {
            const Row& r = g_row[i];
            std::snprintf(line, sizeof(line),
                "[TF2VR]   ROW%d P <%.1f,%.1f,%.1f> E <%.1f,%.1f,%.1f> bodyYaw %.2f headYaw %.2f headPitch "
                "%.2f | lanes %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f\n",
                i, r.p[0], r.p[1], r.p[2], r.e[0], r.e[1], r.e[2], r.bodyYaw, r.headYaw, r.headPitch,
                r.lanes[0], r.lanes[1], r.lanes[2], r.lanes[3], r.lanes[4], r.lanes[5], r.lanes[6], r.lanes[7]);
            Tf2VrLog(line);
        }
        g_rowCount = 0;  // a fresh set of rows each window, spread over the turn
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faulted.fetch_add(1, std::memory_order_relaxed);
    }
}

// ---- the hud2d A/B ---------------------------------------------------------
// The two values, named, because the run's baseline is now the STOCK one.
//
// The 2026-09-05 "stock HUD" baseline reset `rui.arm`, `rui.ll_arm` and
// `rui.inset` and deliberately left this one armed, recorded in STATE as "the
// reticle is deliberately untouched". That classification was wrong: it is not
// the reticle's size, it is a scale on the whole screen-space HUD pass about
// the gun's anchor. The ini now ships the stock value for this front, and this
// key puts our shipped value back on demand.
constexpr float kStockZoom = 1.0f;
constexpr float kShippedZoom = 0.262f;  // mirrors `set hud2d.zoom` before this run

void ToggleHud2dZoomForMarkerTest() {
    float x = 0.0f, y = 0.0f, zoom = 1.0f;
    ReadHud2dPlacement(&x, &y, &zoom);
    // 2026-09-07: return to the LIVE ini value, not a constant. The value in force
    // at the first press is the one restored; kShippedZoom is only the fallback.
    static float remembered = 0.0f;
    const bool atStock = std::fabs(zoom - kStockZoom) < 0.001f;
    if (!atStock) remembered = zoom;
    const float next = atStock ? (remembered > 0.0f ? remembered : kShippedZoom) : kStockZoom;
    SetHud2dPlacement(x, y, next);
    char line[560]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F3: hud2d.zoom %.3f -> %.3f (%s). This scales the whole screen-space HUD pass about "
        "the GUN's aim anchor, so at %.3f every marker motion is shown at %.3fx. If the waypoint's "
        "head-drift changes size across this toggle, the marker is in OUR scaled pass and that "
        "attenuation is part of the defect; if it does not, the pass is innocent and the vertices "
        "are next.\n",
        zoom, next, atStock ? "our shipped value, HUD and reticle SMALL" : "STOCK, HUD and reticle FULL SIZE",
        next, next);
    Tf2VrLog(line);
}
