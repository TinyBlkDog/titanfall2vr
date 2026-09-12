#include "stall_watchdog.h"

#include "diagnostics.h"
#include "jobpool_watch.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

// ~4000x a healthy pass 1 and ~95x a healthy pass 2, so a merely slow frame
// cannot trip it. The wall is a permanent block, not a long frame.
constexpr unsigned long long kStallMs = 2000;
constexpr int kPollMs = 250;
// More than one dump is worth having -- the second says whether the thread is
// parked on one instruction or spinning through several -- but not so many that
// the log fills with the same stack.
constexpr int kMaxDumps = 3;
constexpr int kStackWordsScanned = 256;
constexpr int kStackHitsShown = 16;

std::atomic_bool g_started{false};
HANDLE g_thread = nullptr;

// The section currently open on the hooked thread.
std::atomic<unsigned long long> g_openTick{0};
std::atomic<unsigned long long> g_openFrame{0};
std::atomic_int g_openPass{0};
std::atomic<HANDLE> g_hookThread{nullptr};
std::atomic_int g_dumps{0};
std::atomic<DWORD> g_stalledThreadId{0};

void DumpAllThreads();

bool DescribeAddress(std::uintptr_t address, char* out, std::size_t outSize) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!address || !VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) ||
        !mbi.AllocationBase) {
        return false;
    }
    if (mbi.Type != MEM_IMAGE) return false;
    char full[MAX_PATH]{};
    if (!GetModuleFileNameA(static_cast<HMODULE>(mbi.AllocationBase), full, MAX_PATH)) return false;
    const char* leaf = std::strrchr(full, '\\');
    const auto rva = address - reinterpret_cast<std::uintptr_t>(mbi.AllocationBase);
    std::snprintf(out, outSize, "%s+0x%llX", leaf ? leaf + 1 : full,
                  static_cast<unsigned long long>(rva));
    return true;
}

bool Readable(const void* address, std::size_t bytes) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!address || !VirtualQuery(address, &mbi, sizeof(mbi))) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    const DWORD protect = mbi.Protect & 0xFF;
    return mbi.State == MEM_COMMIT && begin + bytes <= regionEnd &&
        protect != PAGE_NOACCESS && protect != PAGE_GUARD;
}

void DumpStalledThread(int pass, unsigned long long frame, unsigned long long heldMs) {
    HANDLE thread = g_hookThread.load(std::memory_order_acquire);
    if (!thread) {
        Tf2VrLog("[TF2VR] R3 WATCHDOG: a section is stalled but no thread handle was captured. "
                 "VOID -- the dump cannot be taken, and this is NOT evidence of anything.\n");
        return;
    }

    // The process is already hung; suspending costs nothing and is the only way
    // to read a coherent context.
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
        Tf2VrLog("[TF2VR] R3 WATCHDOG: SuspendThread FAILED. No dump. VOID.\n");
        return;
    }

    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    const bool got = GetThreadContext(thread, &ctx) != 0;

    char lines[9000]{};
    int used = 0;
    if (got) {
        char rip[160]{};
        if (!DescribeAddress(static_cast<std::uintptr_t>(ctx.Rip), rip, sizeof(rip))) {
            std::snprintf(rip, sizeof(rip), "0x%llX (not in any module)",
                          static_cast<unsigned long long>(ctx.Rip));
        }
        used += std::snprintf(lines + used, sizeof(lines) - used,
            "[TF2VR] R3 WATCHDOG: pass %d has been inside the scene draw for %llu ms on doubled "
            "frame %llu. THE STALLED THREAD IS AT %s (rsp=0x%llX). Conservative stack scan "
            "follows -- these are stack words that resolve inside a loaded module, so they are "
            "CANDIDATE return addresses, not a verified unwind.\n",
            pass, heldMs, frame, rip, static_cast<unsigned long long>(ctx.Rsp));

        const auto* sp = reinterpret_cast<const std::uintptr_t*>(ctx.Rsp);
        int shown = 0;
        for (int i = 0; i < kStackWordsScanned && shown < kStackHitsShown; ++i) {
            if (!Readable(sp + i, sizeof(std::uintptr_t))) break;
            const std::uintptr_t word = sp[i];
            char where[160]{};
            if (!DescribeAddress(word, where, sizeof(where))) continue;
            // Only code-looking addresses; data pointers into a module's .data
            // would swamp the list.
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<void*>(word), &mbi, sizeof(mbi))) continue;
            const DWORD p = mbi.Protect & 0xFF;
            if (p != PAGE_EXECUTE_READ && p != PAGE_EXECUTE_READWRITE && p != PAGE_EXECUTE) continue;
            ++shown;
            used += std::snprintf(lines + used, sizeof(lines) - used,
                "[TF2VR]   R3 stack[%02d] +0x%03X  %s\n", shown, i * 8, where);
            if (used > static_cast<int>(sizeof(lines)) - 300) break;
        }
        if (shown == 0) {
            used += std::snprintf(lines + used, sizeof(lines) - used,
                "[TF2VR]   R3 stack: NO executable words found in %d qwords from rsp. The RIP "
                "above still stands on its own.\n", kStackWordsScanned);
        }
    } else {
        std::snprintf(lines, sizeof(lines),
            "[TF2VR] R3 WATCHDOG: GetThreadContext FAILED for the stalled thread. VOID.\n");
    }

    ResumeThread(thread);
    Tf2VrLog(lines);

    // EVERY OTHER THREAD, because a wait that never completes has two sides and
    // we have only ever looked at one.
    //
    // R5 established that entering this wait is NORMAL: 35 of 158 sampled frames
    // took it and returned fine, and only the last one did not. So the question
    // is not "why did it wait" but "who was supposed to finish the job and what
    // are they doing instead". With JT_NumWorkerThreads=1 there is exactly one
    // thread that can run it, and this says where that thread is parked.
    //
    //   * worker also inside a JT wait  -> a two-party deadlock, both sides named
    //   * worker idle in its scheduler  -> the job was never queued to it
    //   * worker inside D3D/driver      -> the block is downstream of the job system
    //
    // Those are three different bugs and no counter yet built can tell them
    // apart. Read-only: suspend, read RIP, resume. The process is already dead.
    //
    // AND the job itself, decoded LIVE. Every per-frame sample is taken BEFORE
    // the wait; this one is taken DURING it, which is the only way to see
    // whether the job is still outstanding at the moment it fails to complete.
    {
        // R11 CORRECTION. This used to decode JobPoolWatchLastWaitedJob(), i.e.
        // the CLIENT id in this+0xF1C80 -- and R9 proved that is NOT the job the
        // thread is parked on. The stall is inside engine+0xB6C30 waiting on
        // engine+0x12208E94, so R7's "the job completed and the wake was lost"
        // was measured on the wrong job and does not stand. Decode BOTH now: the
        // engine fence (the one that actually blocks) and the client id (kept
        // only so the retraction stays visible in the log rather than in a doc).
        char fence[260]{};
        JobPoolWatchDescribeEngineFence(fence, sizeof(fence));
        char engineJob[240]{};
        {
            HMODULE eng = GetModuleHandleA("engine.dll");
            unsigned id = 0;
            if (eng) {
                const auto* p = reinterpret_cast<const volatile unsigned*>(
                    reinterpret_cast<const unsigned char*>(eng) + 0x12208E94);
                if (Readable(const_cast<const void*>(static_cast<const volatile void*>(p)),
                             sizeof(unsigned))) {
                    id = *p;
                }
            }
            JobPoolWatchDescribeJob(id, engineJob, sizeof(engineJob));
        }
        char client[240]{};
        JobPoolWatchDescribeJob(JobPoolWatchLastWaitedJob(), client, sizeof(client));
        char jl[900]{};
        std::snprintf(jl, sizeof(jl),
            "[TF2VR] R11 DURING THE STALL:\n"
            "[TF2VR]   ENGINE FENCE %s\n"
            "[TF2VR]   ENGINE JOB (engine+0x12208E94, THE ONE BLOCKED ON): %s\n"
            "[TF2VR]   client this+0xF1C80 (NOT the blocking job, kept for contrast): %s\n",
            fence, engineJob, client);
        Tf2VrLog(jl);
    }
    DumpAllThreads();
}

void DumpAllThreads() {
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        Tf2VrLog("[TF2VR] R3 THREAD CENSUS: CreateToolhelp32Snapshot FAILED. VOID.\n");
        return;
    }
    const DWORD stalledId = g_stalledThreadId.load(std::memory_order_acquire);
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    char lines[9000]{};
    int used = std::snprintf(lines, sizeof(lines),
        "[TF2VR] R3 THREAD CENSUS at the stall (RIP per thread; STALLED marks the one inside the "
        "scene draw, WD is this watchdog):\n");
    int seen = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            ++seen;
            if (te.th32ThreadID == self) {
                used += std::snprintf(lines + used, sizeof(lines) - used,
                    "[TF2VR]   thread %5lu  WD (this watchdog)\n", te.th32ThreadID);
                continue;
            }
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE,
                                  te.th32ThreadID);
            if (!h) {
                used += std::snprintf(lines + used, sizeof(lines) - used,
                    "[TF2VR]   thread %5lu  (OpenThread refused)\n", te.th32ThreadID);
            } else {
                char where[400] = "(context unavailable)";
                if (SuspendThread(h) != static_cast<DWORD>(-1)) {
                    CONTEXT c{};
                    c.ContextFlags = CONTEXT_CONTROL;
                    if (GetThreadContext(h, &c)) {
                        int w = 0;
                        if (!DescribeAddress(static_cast<std::uintptr_t>(c.Rip), where,
                                             sizeof(where))) {
                            w = std::snprintf(where, sizeof(where), "0x%llX",
                                              static_cast<unsigned long long>(c.Rip));
                        } else {
                            w = static_cast<int>(std::strlen(where));
                        }
                        // EVERY THREAD IN THIS PROCESS IS PARKED IN ntdll, so the
                        // RIP alone says nothing -- it cannot tell a JT worker
                        // idling in its scheduler from one blocked mid-job, and
                        // those are two different bugs. This walks each stack for
                        // the first few GAME frames (tier0 / client / engine),
                        // which is what actually names the thread's role.
                        int hits = 0;
                        const auto* sp = reinterpret_cast<const std::uintptr_t*>(c.Rsp);
                        for (int i = 0; i < 512 && hits < 3; ++i) {
                            if (!Readable(sp + i, sizeof(std::uintptr_t))) break;
                            char f[160]{};
                            if (!DescribeAddress(sp[i], f, sizeof(f))) continue;
                            if (!std::strstr(f, "tier0") && !std::strstr(f, "client") &&
                                !std::strstr(f, "engine")) {
                                continue;
                            }
                            MEMORY_BASIC_INFORMATION m{};
                            if (!VirtualQuery(reinterpret_cast<void*>(sp[i]), &m, sizeof(m))) continue;
                            const DWORD p = m.Protect & 0xFF;
                            if (p != PAGE_EXECUTE_READ && p != PAGE_EXECUTE_READWRITE &&
                                p != PAGE_EXECUTE) {
                                continue;
                            }
                            ++hits;
                            w += std::snprintf(where + w, sizeof(where) - w, " < %s", f);
                        }
                        if (hits == 0) {
                            std::snprintf(where + w, sizeof(where) - w,
                                          " < (no game frames -- not a game thread)");
                        }
                    }
                    ResumeThread(h);
                }
                used += std::snprintf(lines + used, sizeof(lines) - used,
                    "[TF2VR]   thread %5lu  %s%s\n", te.th32ThreadID,
                    te.th32ThreadID == stalledId ? "STALLED " : "", where);
                CloseHandle(h);
            }
            if (used > static_cast<int>(sizeof(lines)) - 220) break;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    std::snprintf(lines + used, sizeof(lines) - used,
                  "[TF2VR]   (%d threads in the process)\n", seen);
    Tf2VrLog(lines);
}

DWORD WINAPI WatchdogMain(LPVOID) {
    for (;;) {
        Sleep(kPollMs);
        const unsigned long long opened = g_openTick.load(std::memory_order_acquire);
        if (opened == 0) continue;
        const unsigned long long now = GetTickCount64();
        if (now < opened || now - opened < kStallMs) continue;
        if (g_dumps.load(std::memory_order_acquire) >= kMaxDumps) continue;
        g_dumps.fetch_add(1, std::memory_order_acq_rel);
        DumpStalledThread(g_openPass.load(std::memory_order_acquire),
                          g_openFrame.load(std::memory_order_acquire), now - opened);
        // Re-dump only after another full stall window, so a permanent block
        // yields a few spaced samples rather than a wall of identical stacks.
        Sleep(static_cast<DWORD>(kStallMs));
    }
}

}  // namespace

void StartStallWatchdog() {
    if (g_started.exchange(true, std::memory_order_acq_rel)) return;
    DWORD id = 0;
    g_thread = CreateThread(nullptr, 0, WatchdogMain, nullptr, 0, &id);
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] R3 WATCHDOG started (thread %lu, poll %d ms, stall threshold %llu ms). It dumps "
        "the RIP of whichever pass is stuck. %s\n",
        id, kPollMs, kStallMs,
        g_thread ? "Armed." : "CreateThread FAILED -- there will be no dump and its silence is VOID.");
    Tf2VrLog(line);
}

void StallSectionOpen(int pass, unsigned long long frame) {
    if (!g_hookThread.load(std::memory_order_acquire)) {
        // Captured once, from the thread that actually runs the draw. A pseudo
        // handle would be useless to the watchdog thread, hence the duplicate.
        HANDLE dup = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &dup,
                            0, FALSE, DUPLICATE_SAME_ACCESS)) {
            HANDLE expected = nullptr;
            if (!g_hookThread.compare_exchange_strong(expected, dup)) CloseHandle(dup);
        }
    }
    g_openPass.store(pass, std::memory_order_relaxed);
    g_openFrame.store(frame, std::memory_order_relaxed);
    g_stalledThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_openTick.store(GetTickCount64(), std::memory_order_release);
}

void StallSectionClose() { g_openTick.store(0, std::memory_order_release); }

void LogStallWatchdogStatus() {
    char line[260]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] R3 watchdog: %s, thread handle %s, dumps so far %d.\n",
        g_started.load(std::memory_order_acquire) ? "running" : "NOT STARTED",
        g_hookThread.load(std::memory_order_acquire) ? "captured" : "NOT CAPTURED",
        g_dumps.load(std::memory_order_acquire));
    Tf2VrLog(line);
}
