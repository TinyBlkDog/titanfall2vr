#include "bone_reader_watch.h"

#include "diagnostics.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

// The validated arms target, published by arms_collapse.cpp (extern "C" for
// the asm interceptor). Zero until an instance has validated.
extern "C" std::uint64_t g_armsBonesTarget;
extern "C" std::uint64_t g_armsBonesTargetInstance;
extern "C" int g_armsBonesTargetCount;

namespace {

constexpr std::size_t kMaxSites = 64;
constexpr std::uint32_t kMaxHits = 20000;
constexpr std::uint64_t kWallClockMs = 8000;

struct Site {
    std::uintptr_t rip;
    std::uintptr_t stack[6];  // [rsp+0..+40] at the FIRST hit: return addresses when RIP is in a leaf or after pushes
    std::uintptr_t rsi, rdi, rcx;  // memcpy: source, destination, remaining bytes at the first hit
    std::uint32_t count;
    std::uint32_t threadId;
    std::uint32_t threads;  // distinct thread ids seen (approximate: counts changes)
};

Site g_sites[kMaxSites]{};
std::atomic_uint32_t g_siteCount = 0;
std::atomic_uint32_t g_hitCount = 0;
std::atomic_bool g_armed = false;
bool g_done = false;
bool g_waitSaid = false;
PVOID g_handler = nullptr;
std::uintptr_t g_watchAddress = 0;
std::uint64_t g_armedAt = 0;
std::uint64_t g_watchedInstance = 0;

// DR7: L0 (bit 0); RW0 = 11 read-or-write (bits 16-17); LEN0 = 10 eight
// bytes (bits 18-19). The mask clears exactly those bits plus G0.
constexpr DWORD64 kDr7Mask = 0xF0003ull;
constexpr DWORD64 kDr7Bits = 0xB0001ull;
static_assert((kDr7Bits & ~kDr7Mask) == 0, "every armed bit is a bit the disarm clears");

bool ReadableSpan(const void* address, std::size_t bytes) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    return start + info.RegionSize - at >= bytes;
}

// Exception-handler context: no allocation, no logging, no locks.
void RecordSite(std::uintptr_t rip, const std::uintptr_t* stack, const CONTEXT* c, std::uint32_t tid) {
    const std::uint32_t count = g_siteCount.load(std::memory_order_acquire);
    for (std::uint32_t i = 0; i < count && i < kMaxSites; ++i) {
        if (g_sites[i].rip == rip) {
            ++g_sites[i].count;
            if (g_sites[i].threadId != tid) { g_sites[i].threadId = tid; ++g_sites[i].threads; }
            return;
        }
    }
    if (count >= kMaxSites) return;
    const std::uint32_t slot = g_siteCount.fetch_add(1, std::memory_order_acq_rel);
    if (slot >= kMaxSites) return;
    g_sites[slot].rip = rip;
    for (int k = 0; k < 6; ++k) g_sites[slot].stack[k] = stack[k];
    g_sites[slot].rsi = static_cast<std::uintptr_t>(c->Rsi);
    g_sites[slot].rdi = static_cast<std::uintptr_t>(c->Rdi);
    g_sites[slot].rcx = static_cast<std::uintptr_t>(c->Rcx);
    g_sites[slot].count = 1;
    g_sites[slot].threadId = tid;
    g_sites[slot].threads = 1;
}

LONG CALLBACK BoneReaderHandler(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if (!(info->ContextRecord->Dr6 & 0x1)) return EXCEPTION_CONTINUE_SEARCH;  // not DR0: not ours
    std::uintptr_t stack[6]{};
    const auto* rsp = reinterpret_cast<const std::uintptr_t*>(info->ContextRecord->Rsp);
    if (ReadableSpan(rsp, sizeof(stack))) {
        for (int k = 0; k < 6; ++k) stack[k] = rsp[k];
    }
    RecordSite(static_cast<std::uintptr_t>(info->ContextRecord->Rip), stack, info->ContextRecord, GetCurrentThreadId());
    info->ContextRecord->Dr6 = 0;
    if (g_hitCount.fetch_add(1, std::memory_order_acq_rel) + 1 >= kMaxHits) {
        info->ContextRecord->Dr7 &= ~static_cast<DWORD64>(0x3);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

void ApplyRegisters(CONTEXT& context, bool arm) {
    if (arm) {
        context.Dr0 = g_watchAddress;
        context.Dr7 = (context.Dr7 & ~kDr7Mask) | kDr7Bits;
    } else {
        context.Dr0 = 0;
        context.Dr7 &= ~kDr7Mask;
    }
}

void ApplyToCurrentThread(bool arm, std::uint32_t& touched) {
    CONTEXT context{};
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(GetCurrentThread(), &context)) return;
    ApplyRegisters(context, arm);
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (SetThreadContext(GetCurrentThread(), &context)) ++touched;
}

void ForEachOtherThread(bool arm, std::uint32_t& touched) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
            HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE,
                                       entry.th32ThreadID);
            if (!thread) continue;
            if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(thread, &context)) {
                    ApplyRegisters(context, arm);
                    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (SetThreadContext(thread, &context)) ++touched;
                }
                ResumeThread(thread);
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

void DescribeRip(std::uintptr_t rip, const char** module, std::uintptr_t* rva) {
    struct Known { const char* name; HMODULE handle; };
    const Known known[] = {
        {"client.dll", GetModuleHandleA("client.dll")},
        {"engine.dll", GetModuleHandleA("engine.dll")},
        {"materialsystem_dx11.dll", GetModuleHandleA("materialsystem_dx11.dll")},
        {"studiorender.dll", GetModuleHandleA("studiorender.dll")},
        {"rtech_game.dll", GetModuleHandleA("rtech_game.dll")},
        {"datacache.dll", GetModuleHandleA("datacache.dll")},
        {"tier0.dll", GetModuleHandleA("tier0.dll")},
        {"vstdlib.dll", GetModuleHandleA("vstdlib.dll")},
        {"vphysics.dll", GetModuleHandleA("vphysics.dll")},
        {"server.dll", GetModuleHandleA("server.dll")},
        {"d3d11.dll", GetModuleHandleA("d3d11.dll")},
        {"ntdll.dll", GetModuleHandleA("ntdll.dll")},
        {"vcruntime140.dll", GetModuleHandleA("vcruntime140.dll")},
        {"titanfall2vr.dll", GetModuleHandleA("titanfall2vr.dll")},
    };
    *module = "unknown";
    *rva = rip;
    for (const Known& entry : known) {
        if (!entry.handle) continue;
        const auto base = reinterpret_cast<std::uintptr_t>(entry.handle);
        if (rip <= base || rip - base >= 0x2000000) continue;
        *module = entry.name;
        *rva = rip - base;
        return;
    }
}

// STAGE 1 watches the entity's own array. Run 2 (2026-09-05) showed its only
// reader is the CRT memcpy at the end of the SetupBones driver (client.dll
// +0xFEDB8), copying all 74 matrices into a caller-supplied heap buffer. STAGE
// 2 re-arms on THAT buffer: whoever reads the copy is the next hop toward the
// draw. The memcpy's own write is stage 2's positive control.
int g_stage = 1;
std::uintptr_t g_stage2Address = 0;
constexpr std::uintptr_t kArrayBytes = 74 * 48;

void Arm() {
    const std::uint64_t target = g_stage == 1 ? g_armsBonesTarget : g_stage2Address;
    if (!target || (target & 7) != 0 || !ReadableSpan(reinterpret_cast<const void*>(target), 48)) {
        char line[200];
        std::snprintf(line, sizeof(line), "[TF2VR] bone-reader: stage %d target %p is not an aligned readable matrix; not arming.\n",
                      g_stage, reinterpret_cast<void*>(target));
        Tf2VrLog(line);
        g_done = true;
        return;
    }
    g_siteCount.store(0, std::memory_order_release);
    g_hitCount.store(0, std::memory_order_release);
    g_watchAddress = static_cast<std::uintptr_t>(target);
    g_watchedInstance = g_armsBonesTargetInstance;
    g_handler = AddVectoredExceptionHandler(1, BoneReaderHandler);
    if (!g_handler) {
        Tf2VrLog("[TF2VR] bone-reader: AddVectoredExceptionHandler failed; not arming.\n");
        g_done = true;
        return;
    }
    std::uint32_t touched = 0;
    g_armed.store(true, std::memory_order_release);
    ForEachOtherThread(true, touched);
    ApplyToCurrentThread(true, touched);
    g_armedAt = GetTickCount64();
    char line[300];
    std::snprintf(line, sizeof(line),
                  "[TF2VR] bone-reader: STAGE %d ARMED DR0 read-or-write x8 on %s %p (instance %p, %d bones) "
                  "across %u threads; closes after %llu ms or %u hits. Every RIP that touches matrix 0 is recorded.\n",
                  g_stage, g_stage == 1 ? "the arms bone array" : "the memcpy DESTINATION buffer",
                  reinterpret_cast<void*>(target), reinterpret_cast<void*>(g_watchedInstance), g_armsBonesTargetCount,
                  touched, static_cast<unsigned long long>(kWallClockMs), kMaxHits);
    Tf2VrLog(line);
}

void Disarm(const char* why) {
    std::uint32_t touched = 0;
    ForEachOtherThread(false, touched);
    ApplyToCurrentThread(false, touched);
    g_armed.store(false, std::memory_order_release);
    if (g_handler) { RemoveVectoredExceptionHandler(g_handler); g_handler = nullptr; }
    g_done = true;
    const std::uint32_t hits = g_hitCount.load(std::memory_order_acquire);
    const std::uint32_t sites = g_siteCount.load(std::memory_order_acquire);
    const std::uint64_t ms = GetTickCount64() - g_armedAt;
    char line[400];
    std::snprintf(line, sizeof(line),
                  "[TF2VR] bone-reader: DISARMED (%s) after %llu ms, %u hits at %u distinct sites (cap %zu), debug registers "
                  "cleared on %u threads.%s\n",
                  why, static_cast<unsigned long long>(ms), hits, sites < kMaxSites ? sites : static_cast<std::uint32_t>(kMaxSites),
                  kMaxSites, touched,
                  hits == 0 ? "  ZERO HITS -- not even SetupBones' own write landed, so the watchpoint did not take; "
                              "this run says nothing about readers."
                            : "");
    Tf2VrLog(line);
    bool clientWriterSeen = false;
    for (std::uint32_t i = 0; i < sites && i < kMaxSites; ++i) {
        const Site& s = g_sites[i];
        const char* mod = nullptr;
        std::uintptr_t rva = 0;
        DescribeRip(s.rip, &mod, &rva);
        if (mod[0] == 'c') clientWriterSeen = true;
        const char* role = "";
        if (s.rsi >= g_watchAddress - 4096 && s.rsi <= g_watchAddress) role = "  READS the array (rsi in it)";
        else if (s.rdi >= g_watchAddress - 4096 && s.rdi <= g_watchAddress) role = "  WRITES the array (rdi in it)";
        std::snprintf(line, sizeof(line),
                      "[TF2VR] BONE-READER[%u] %s+0x%llX  hits %u  thread %u (%u thread changes)  rsi %p rdi %p rcx %llu%s\n",
                      i, mod, static_cast<unsigned long long>(rva), s.count, s.threadId, s.threads - 1,
                      reinterpret_cast<void*>(s.rsi), reinterpret_cast<void*>(s.rdi),
                      static_cast<unsigned long long>(s.rcx), role);
        Tf2VrLog(line);
        int n = std::snprintf(line, sizeof(line), "[TF2VR]   stack at first hit:");
        for (int k = 0; k < 6; ++k) {
            const char* sm = nullptr;
            std::uintptr_t sr = 0;
            DescribeRip(s.stack[k], &sm, &sr);
            if (sm[0] == 'u') n += std::snprintf(line + n, sizeof(line) - n, " [+%d]=%016llX", k * 8,
                                                 static_cast<unsigned long long>(s.stack[k]));
            else n += std::snprintf(line + n, sizeof(line) - n, " [+%d]=%s+0x%llX", k * 8, sm,
                                    static_cast<unsigned long long>(sr));
        }
        std::snprintf(line + n, sizeof(line) - n, "\n");
        Tf2VrLog(line);
    }
    std::snprintf(line, sizeof(line),
                  "[TF2VR] bone-reader: stage %d positive control (a client.dll site: %s) %s.\n", g_stage,
                  g_stage == 1 ? "SetupBones' own write" : "the memcpy that filled this buffer",
                  clientWriterSeen ? "PASS" : "FAIL -- the site list is not to be trusted");
    Tf2VrLog(line);
    if (g_stage != 1) return;
    // Stage 2 target: the destination of the memcpy that READ the array.
    for (std::uint32_t i = 0; i < sites && i < kMaxSites; ++i) {
        const Site& s = g_sites[i];
        if (s.rsi < g_watchAddress || s.rsi > g_watchAddress + kArrayBytes || !s.rdi) continue;
        const std::uintptr_t dst = s.rdi - (s.rsi - g_watchAddress);
        if ((dst & 7) != 0 || !ReadableSpan(reinterpret_cast<const void*>(dst), 48)) continue;
        g_stage2Address = dst;
        g_stage = 2;
        g_done = false;
        std::snprintf(line, sizeof(line),
                      "[TF2VR] bone-reader: stage 2 PENDING on the memcpy destination %p (from site %u: rsi %p rdi %p); "
                      "arms next frame.\n",
                      reinterpret_cast<void*>(dst), i, reinterpret_cast<void*>(s.rsi), reinterpret_cast<void*>(s.rdi));
        Tf2VrLog(line);
        return;
    }
    Tf2VrLog("[TF2VR] bone-reader: no memcpy READ of the array was recorded, so there is no stage 2 target.\n");
}

}  // namespace

void AdvanceBoneReaderWatch() {
    if (g_done) return;
    if (!g_armed.load(std::memory_order_acquire)) {
        if (g_stage == 1 && !g_armsBonesTarget) {
            if (!g_waitSaid) {
                g_waitSaid = true;
                Tf2VrLog("[TF2VR] bone-reader: waiting for the arms hook to validate a bone array.\n");
            }
            return;
        }
        Arm();
        return;
    }
    const bool capped = g_hitCount.load(std::memory_order_acquire) >= kMaxHits;
    const bool timedOut = GetTickCount64() - g_armedAt >= kWallClockMs;
    if (capped || timedOut) Disarm(capped ? "hit cap" : "wall clock");
}
