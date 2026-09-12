#include "titan_state.h"

#include "diagnostics.h"
#include "player_eye.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// ---- what was read offline, verified at first use ---------------------------
constexpr std::uintptr_t kPlayerVtableRva = 0x8C7A58;  // C_Player, primary vtable
constexpr std::uintptr_t kIsPlayerSlot = 0x4A8;
constexpr std::uintptr_t kIsTitanSlot = 0x4B0;
constexpr std::uintptr_t kIsPlayerRva = 0x1576D0;
constexpr std::uintptr_t kIsTitanRva = 0x2C92F0;
constexpr std::uint8_t kIsPlayerBytes[] = {0xB0, 0x01, 0xC3};  // mov al,1 ; ret
constexpr std::uint8_t kIsTitanBytes[] = {0x48, 0x83, 0xEC, 0x28,               // sub rsp,28h
                                          0x8B, 0x89, 0x60, 0x1E, 0x00, 0x00};  // mov ecx,[rcx+1E60h]
// The class index IsTitan reads. Printed on every heartbeat and edge as a
// second witness: it changes when the wearer's player class does.
constexpr std::uintptr_t kClassIndexField = 0x1E60;

using BoolMethod = bool(__fastcall*)(void* self);

enum class Reading : int { Unknown = 0, OnFoot = 1, InTitan = 2 };
const char* Name(Reading r) {
    switch (r) {
        case Reading::OnFoot: return "ON FOOT";
        case Reading::InTitan: return "IN TITAN";
        default: return "UNKNOWN";
    }
}

// Consecutive identical raw readings before the committed state moves. The
// state is polled, so one frame of debounce is cheap and rules out a
// single-frame glitch (an entity mid-swap on a map change) reading as an edge.
constexpr unsigned kDebounceFrames = 3;
// The legacy settle used while the gate is off, so the arms collapse arms on
// the same schedule it always has.
constexpr std::uint64_t kLegacySettleMs = 1500;

// ---- resolution ------------------------------------------------------------
bool g_resolved = false;
bool g_refused = false;
std::uintptr_t g_clientBase = 0;
std::uintptr_t g_clientEnd = 0;
std::uintptr_t g_playerVtable = 0;

// ---- per-frame counters (the freshness of the instrument) -------------------
std::uint64_t g_polls = 0;
std::uint64_t g_nullPlayer = 0;
std::uint64_t g_foreignVptrRefused = 0;
std::uint64_t g_isPlayerTrue = 0;
std::uint64_t g_isPlayerFalse = 0;
std::uint64_t g_titanTrue = 0;
std::uint64_t g_titanFalse = 0;
std::uint64_t g_faults = 0;
std::uintptr_t g_lastVptr = 0;
std::uintptr_t g_lastForeignVptrLogged = 0;
std::int32_t g_lastClassIndex = -1;

// ---- the state -------------------------------------------------------------
Reading g_raw = Reading::Unknown;
unsigned g_rawRun = 0;
// The raw reading published for consumers whose safe direction is "act at
// once": the arms collapse stands down on this, so the debounce never buys
// three frames of bone writes on a titan's model. Re-arming waits for the
// debounced state.
std::atomic<int> g_rawPublished{static_cast<int>(Reading::Unknown)};
std::atomic<int> g_committed{static_cast<int>(Reading::Unknown)};
std::uint32_t g_transitions = 0;
// titan.gate: 1 the arms follow the state (DEFAULT -- proven 2026-09-03, so a
// user with no ini gets it, as with eye.hook); 0 read-only, the control arm.
std::atomic<int> g_gate{1};

const char* GateText(int level) {
    return level ? "ACT on this: the arms collapse follows it" : "IGNORE this (read-only run)";
}

// ---- the report clock ------------------------------------------------------
std::uint64_t g_lastReportTick = 0;
std::uint64_t g_sampleIndex = 0;
std::uint64_t g_prevPolls = 0, g_prevNull = 0, g_prevIsPlayerTrue = 0, g_prevIsPlayerFalse = 0,
              g_prevTitanTrue = 0, g_prevTitanFalse = 0;
std::uint64_t g_firstWorldTick = 0;
std::uint64_t g_lastControlFailLogTick = 0;

bool WithinClient(std::uintptr_t p) { return p >= g_clientBase && p < g_clientEnd; }

void Resolve() {
    if (g_resolved || g_refused) return;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return;
    const auto base = reinterpret_cast<std::uintptr_t>(client);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    g_clientBase = base;
    g_clientEnd = base + nt->OptionalHeader.SizeOfImage;
    g_playerVtable = base + kPlayerVtableRva;

    const auto slotIsPlayer = *reinterpret_cast<const std::uintptr_t*>(g_playerVtable + kIsPlayerSlot);
    const auto slotIsTitan = *reinterpret_cast<const std::uintptr_t*>(g_playerVtable + kIsTitanSlot);
    const bool slotsMatch = slotIsPlayer == base + kIsPlayerRva && slotIsTitan == base + kIsTitanRva;
    const bool bytesMatch =
        slotsMatch &&
        std::memcmp(reinterpret_cast<const void*>(slotIsPlayer), kIsPlayerBytes, sizeof(kIsPlayerBytes)) == 0 &&
        std::memcmp(reinterpret_cast<const void*>(slotIsTitan), kIsTitanBytes, sizeof(kIsTitanBytes)) == 0;
    if (!slotsMatch || !bytesMatch) {
        g_refused = true;
        char line[400]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] titan state: C_Player vtable +0x4A8/+0x4B0 hold client+0x%llX/+0x%llX, expected "
            "+0x1576D0/+0x2C92F0 (%s). REFUSED, latched: no titan reading this run, every consumer "
            "reads on-foot, and nothing here can be believed.\n",
            static_cast<unsigned long long>(slotIsPlayer - base),
            static_cast<unsigned long long>(slotIsTitan - base),
            slotsMatch ? "slots match but the function bytes do not" : "slots differ");
        Tf2VrLog(line);
        return;
    }
    g_resolved = true;
    Tf2VrLog("[TF2VR] titan state: RESOLVED. C_Player::IsPlayer (+0x4A8 -> client+0x1576D0) and "
             "C_Player::IsTitan (+0x4B0 -> client+0x2C92F0) verified by slot and by bytes. Polling "
             "the local player every frame; IsPlayer is the control and must read true. Edges log "
             "uncapped as 'TITANSTATE ->'; heartbeat every 5 s.\n");
}

// One virtual call, guarded. Returns -1 on a fault (and latches a refusal --
// a faulting read is not one to keep taking), else 0/1.
int CallGuarded(void* player, std::uintptr_t slot) {
    const auto vptr = *reinterpret_cast<std::uintptr_t*>(player);
    const auto fn = reinterpret_cast<BoolMethod>(*reinterpret_cast<std::uintptr_t*>(vptr + slot));
    __try {
        return fn(player) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_faults;
        g_refused = true;
        return -1;
    }
}

Reading PollOnce() {
    ++g_polls;
    void* player = const_cast<void*>(PlayerEyeLocalPlayerForReading());
    if (!player) {
        ++g_nullPlayer;
        return Reading::Unknown;
    }
    const auto vptr = *reinterpret_cast<std::uintptr_t*>(player);
    g_lastVptr = vptr;
    if (vptr != g_playerVtable) {
        // A player whose class is not C_Player. Its slots must at least point
        // into client.dll before we jump through them; and it is logged once
        // per distinct vptr, because "the local player changed class in a
        // titan" would itself be a finding.
        const bool slotsSane =
            WithinClient(vptr) && WithinClient(*reinterpret_cast<std::uintptr_t*>(vptr + kIsPlayerSlot)) &&
            WithinClient(*reinterpret_cast<std::uintptr_t*>(vptr + kIsTitanSlot));
        if (vptr != g_lastForeignVptrLogged) {
            g_lastForeignVptrLogged = vptr;
            char line[300]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] titan state: local player vptr %p is NOT the C_Player vtable (%p) -- %s.\n",
                reinterpret_cast<void*>(vptr), reinterpret_cast<void*>(g_playerVtable),
                slotsSane ? "its +0x4A8/+0x4B0 slots point into client.dll, so the calls proceed through it"
                          : "slots do not point into client.dll; REFUSING to call, reading UNKNOWN");
            Tf2VrLog(line);
        }
        if (!slotsSane) {
            ++g_foreignVptrRefused;
            return Reading::Unknown;
        }
    }
    g_lastClassIndex = *reinterpret_cast<const std::int32_t*>(reinterpret_cast<std::uint8_t*>(player) + kClassIndexField);

    const int isPlayer = CallGuarded(player, kIsPlayerSlot);
    if (isPlayer < 0) return Reading::Unknown;
    if (isPlayer == 0) {
        // THE CONTROL FAILED. The entity we hold does not answer as a player,
        // so whatever IsTitan says beside it is not about the wearer.
        ++g_isPlayerFalse;
        const std::uint64_t now = GetTickCount64();
        if (now - g_lastControlFailLogTick >= 5000) {
            g_lastControlFailLogTick = now;
            Tf2VrLog("[TF2VR] titan state: CONTROL FAILED -- IsPlayer read FALSE on the local player. "
                     "The IsTitan reading is VOID while this holds; state reads UNKNOWN.\n");
        }
        return Reading::Unknown;
    }
    ++g_isPlayerTrue;

    const int isTitan = CallGuarded(player, kIsTitanSlot);
    if (isTitan < 0) return Reading::Unknown;
    if (isTitan) ++g_titanTrue; else ++g_titanFalse;
    return isTitan ? Reading::InTitan : Reading::OnFoot;
}

}  // namespace

void AdvanceTitanProbe(bool worldIsRendering) {
    const std::uint64_t now = GetTickCount64();
    if (worldIsRendering && g_firstWorldTick == 0) g_firstWorldTick = now;
    if (!worldIsRendering) g_firstWorldTick = 0;

    Resolve();
    if (!g_resolved || g_refused) {
        // A refusal after resolution (a fault) must move the state to unknown
        // and say so once; consumers then read on-foot through the gate logic.
        if (g_refused && g_committed.load(std::memory_order_relaxed) != static_cast<int>(Reading::Unknown)) {
            g_committed.store(static_cast<int>(Reading::Unknown), std::memory_order_release);
            char line[200]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] TITANSTATE -> UNKNOWN: the read FAULTED (faults=%llu); latched off for the run.\n",
                static_cast<unsigned long long>(g_faults));
            Tf2VrLog(line);
        }
        return;
    }

    // ---- poll and debounce --------------------------------------------------
    const Reading raw = PollOnce();
    g_rawPublished.store(static_cast<int>(raw), std::memory_order_release);
    if (raw == g_raw) {
        if (g_rawRun < 1000000) ++g_rawRun;
    } else {
        g_raw = raw;
        g_rawRun = 1;
    }
    const auto committed = static_cast<Reading>(g_committed.load(std::memory_order_relaxed));
    if (g_rawRun >= kDebounceFrames && raw != committed) {
        g_committed.store(static_cast<int>(raw), std::memory_order_release);
        ++g_transitions;
        // EVERY edge, uncapped: these are the lines the wearer's own "I got in"
        // and "I got out" are matched against.
        char line[420]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] TITANSTATE -> %s (was %s; transition #%u; %s; class index %d; vptr %p; "
            "IsPlayer true=%llu false=%llu; gate=%d so consumers %s).\n",
            Name(raw), Name(committed), g_transitions, worldIsRendering ? "in-world" : "no-world",
            g_lastClassIndex, reinterpret_cast<void*>(g_lastVptr),
            static_cast<unsigned long long>(g_isPlayerTrue), static_cast<unsigned long long>(g_isPlayerFalse),
            g_gate.load(std::memory_order_relaxed), GateText(g_gate.load(std::memory_order_relaxed)));
        Tf2VrLog(line);
    }

    // ---- the heartbeat, every 5 s -----------------------------------------
    if (g_lastReportTick != 0 && now - g_lastReportTick < 5000) return;
    g_lastReportTick = now;
    ++g_sampleIndex;
    const std::uint64_t dPolls = g_polls - g_prevPolls, dNull = g_nullPlayer - g_prevNull,
                        dPT = g_isPlayerTrue - g_prevIsPlayerTrue, dPF = g_isPlayerFalse - g_prevIsPlayerFalse,
                        dTT = g_titanTrue - g_prevTitanTrue, dTF = g_titanFalse - g_prevTitanFalse;
    g_prevPolls = g_polls; g_prevNull = g_nullPlayer; g_prevIsPlayerTrue = g_isPlayerTrue;
    g_prevIsPlayerFalse = g_isPlayerFalse; g_prevTitanTrue = g_titanTrue; g_prevTitanFalse = g_titanFalse;
    const auto state = static_cast<Reading>(g_committed.load(std::memory_order_relaxed));
    const char* verdict =
        dPolls == 0 ? "NOT POLLING -- the tick is not reaching this; nothing here is fresh"
        : (dPT == 0 && dNull == dPolls) ? "no local player this window (menu/loading), reading is an absence not a state"
        : dPF ? "CONTROL FAILING -- IsPlayer read false; the IsTitan column is void"
        : (dTT && dTF) ? "IsTitan flipped inside this window -- the edge line above is the record"
        : "control passing (IsPlayer true on every poll that had a player)";
    char line[560]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] TITANSTATE #%llu %s: state=%s | this window: polls=%llu null-player=%llu "
        "IsPlayer true=%llu FALSE=%llu IsTitan true=%llu false=%llu | class index %d, faults=%llu, "
        "foreign-vptr refused=%llu, transitions=%u, gate=%d | %s.\n",
        static_cast<unsigned long long>(g_sampleIndex), worldIsRendering ? "in-world" : "no-world",
        Name(state), static_cast<unsigned long long>(dPolls), static_cast<unsigned long long>(dNull),
        static_cast<unsigned long long>(dPT), static_cast<unsigned long long>(dPF),
        static_cast<unsigned long long>(dTT), static_cast<unsigned long long>(dTF), g_lastClassIndex,
        static_cast<unsigned long long>(g_faults), static_cast<unsigned long long>(g_foreignVptrRefused),
        g_transitions, g_gate.load(std::memory_order_relaxed), verdict);
    Tf2VrLog(line);
}

bool IsInTitanNow() {
    if (g_gate.load(std::memory_order_acquire) < 1) return false;
    // Either reading says titan -> titan. The raw one stands the arms down the
    // very frame the class flips; the committed one holds it there through the
    // debounce. Fails safe towards "do not write bones".
    return g_rawPublished.load(std::memory_order_acquire) == static_cast<int>(Reading::InTitan) ||
           g_committed.load(std::memory_order_acquire) == static_cast<int>(Reading::InTitan);
}

bool TitanStateSettled() {
    if (g_gate.load(std::memory_order_acquire) < 1) {
        // Legacy timing, so the gate-off build behaves exactly as before.
        if (g_firstWorldTick == 0) return false;
        return (GetTickCount64() - g_firstWorldTick) >= kLegacySettleMs;
    }
    return g_committed.load(std::memory_order_acquire) != static_cast<int>(Reading::Unknown);
}

void SetTitanGate(int level) {
    level = level ? 1 : 0;
    g_gate.store(level, std::memory_order_release);
    char line[300]{};
    std::snprintf(line, sizeof(line), "[TF2VR] titan state: titan.gate = %d -- consumers %s.\n", level,
                  GateText(level));
    Tf2VrLog(line);
}
