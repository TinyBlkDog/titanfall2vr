#include "diagnostics.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <cstdio>

// THE LOG WAS COSTING MORE THAN EVERYTHING IT MEASURED.
//
// Every single line used to do, on the calling thread:
//
//     OutputDebugStringA(message);   // system-global lock, kernel transition
//     GetTempPathA(...);             // every call
//     CreateFileA(...);              // OPEN the file
//     WriteFile(...);
//     CloseHandle(...);              // CLOSE it again
//
// An open/close pair per line is expensive on its own. OutputDebugStringA is
// worse than it looks: it takes a SYSTEM-WIDE mutex and does a kernel round
// trip whether or not a debugger is attached, so it serialises against every
// other process on the machine as well as against ourselves.
//
// The wearer's own diagnosis is what pointed here -- "SOMETHING is killing our
// CPU/GPU and it makes the whole thing take a really long time" -- and the
// evidence fits it and nothing else that had been tried:
//
//   - the loading screen runs at 40 fps against 80 in play (measured);
//   - the game holds a frozen frame for 2-4 seconds at level entry;
//   - HALVING THE RENDER RESOLUTION MADE NO DIFFERENCE, which rules out pixel
//     cost and points at the CPU;
//   - level entry is exactly when this plugin logs most -- the cvar dump alone
//     is 143,917 bytes, thousands of lines, each one an open/write/close plus a
//     global-lock round trip, on the game's threads.
//
// Fixed by keeping the handle open and dropping the debug channel. NOT by
// buffering: this project reads its log after crashes and hangs, and a buffer
// loses precisely the tail that matters.
//
// 2026-09-11: IT IS BUFFERED NOW, and the objection above was right, so it is
// ANSWERED rather than ignored. What survived that cleanup was still one
// WriteFile per line -- 108 syscalls a second, arriving in bursts of seventy
// inside a single frame -- which is the frame spike KNOWN-ISSUES 5 describes.
// The tail is protected three ways: a background thread flushes every 250 ms
// whether or not anything is still logging (a hang stops logging, which is
// exactly when the old 250 ms on-write bound would never fire), and BOTH crash
// routes flush before the process dies. The most that can be lost is a quarter
// second.
namespace {

std::atomic<HANDLE> g_file{INVALID_HANDLE_VALUE};

// OutputDebugString, behind a switch and off by default. It is genuinely useful
// when something crashes before the file is flushed, which is why it is kept at
// all rather than deleted.
std::atomic_bool g_debugChannel{false};

// ONE FILE PER LAUNCH. The log used to be append-only, and that is why the
// titan crash has no evidence.
//
// FILE_APPEND_DATA + OPEN_ALWAYS never truncated anything -- the failure was
// the opposite one. Sessions were CONCATENATED: F1-FREEZE-DATED-2026-08-22
// records the file holding 28 launches and 18 MB, of which only the last was
// the run being analysed, and reading the first `ACTIVE PROFILE` banner in it
// reported a months-old launch. The only way to read one run was to delete the
// file before starting the game, by hand, every time -- and the run nobody
// deleted for was the one that mattered. The live log on 2026-08-26 holds
// exactly one session because somebody remembered; the titan session before it
// is gone because somebody remembered then too.
//
// So the launch takes the rename, not the reader. `titanfall2vr.log` is always
// THIS launch; `titanfall2vr.prev-1.log` is the one before it, and so on to
// prev-N. A crash now leaves its tail in a file the next launch cannot touch.
//
// N IS A COMPILE-TIME CONSTANT AND NOT AN INI KEY, deliberately. Rotation has
// to happen before the first line is written, and the first line is written by
// the config parser -- so a `log.keep` read from the ini would arrive after the
// rotation it was meant to control. A knob that cannot reach the thing it names
// is the dead-control failure this project has already paid for twice.
//
// The hand-named archives (`titanfall2vr.prev-1.6G-run9-devicehung.log` and
// friends) are untouched: they do not collide with the numeric names.
constexpr int kKeptSessions = 8;

// Filled in by RotateLogs and written as the log's own first line, because a
// rotation nobody can see is indistinguishable from one that did not happen.
char g_rotationReport[320]{};
std::atomic_bool g_rotationReportPending{false};

void BuildLogPath(char* out, size_t size, const char* tempPath, int previousIndex) {
    if (previousIndex <= 0) std::snprintf(out, size, "%stitanfall2vr.log", tempPath);
    else std::snprintf(out, size, "%stitanfall2vr.prev-%d.log", tempPath, previousIndex);
}

void RotateLogs(const char* tempPath) {
    char from[MAX_PATH]{}, to[MAX_PATH]{};
    // Oldest first, so each rename lands on a name that has just been vacated.
    // prev-N itself is dropped; MoveFileEx's REPLACE_EXISTING does that for us
    // rather than a separate delete that could half-succeed.
    int moved = 0, failed = 0;
    DWORD firstError = 0;
    for (int index = kKeptSessions - 1; index >= 0; --index) {
        BuildLogPath(from, sizeof(from), tempPath, index);
        BuildLogPath(to, sizeof(to), tempPath, index + 1);
        if (GetFileAttributesA(from) == INVALID_FILE_ATTRIBUTES) continue;
        if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING)) {
            ++moved;
        } else {
            ++failed;
            if (!firstError) firstError = GetLastError();
        }
    }
    // THE ONE FAILURE MODE WORTH NAMING. A second copy of the game still
    // holding the handle denies the rename (the share mode below grants read
    // and write but not DELETE), and the new session then APPENDS to the old
    // file -- back to the behaviour this exists to end. It says so rather than
    // looking like a clean rotation.
    if (failed) {
        std::snprintf(g_rotationReport, sizeof(g_rotationReport),
            "[TF2VR] LOG ROTATION INCOMPLETE: %d of %d renames failed (first error %lu). This "
            "session is APPENDED to whatever the previous one left, so timestamps restart mid-file "
            "-- scope any reading of it to the last 'plugin initialized'. The usual cause is a "
            "second copy of the game still holding the file.\n",
            failed, moved + failed, firstError);
    } else {
        std::snprintf(g_rotationReport, sizeof(g_rotationReport),
            "[TF2VR] LOG ROTATED: this file is THIS launch alone. The previous %d sessions are "
            "titanfall2vr.prev-1.log (most recent) through prev-%d.log; %d file(s) shifted. A crash "
            "no longer loses its own evidence to the next launch.\n",
            kKeptSessions, kKeptSessions, moved);
    }
    g_rotationReportPending.store(true, std::memory_order_release);
}

// WHERE THE LOG LIVES: %LOCALAPPDATA%\titanfall2vr\, not %TEMP%.
//
// It was %TEMP% because that is writable without administrator rights, which
// the plugins folder under Program Files is not. That property is the whole
// requirement, and LOCALAPPDATA has it too -- while %TEMP% is swept by Storage
// Sense and Disk Cleanup, so the evidence from a crash can be deleted before
// the person who hit it thinks to send it. A log that a scheduled task can
// throw away is not a record.
//
// Returns a path that ALREADY ENDS IN A BACKSLASH, because BuildLogPath
// concatenates the filename straight on. Falls back to %TEMP% if LOCALAPPDATA
// is missing or its directory cannot be created: a log in the wrong place
// beats no log, and the fallback is silent because the first line has not been
// written yet -- there is nowhere to report it TO.
bool LogDirectory(char* out, size_t size) {
    char base[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        char folder[MAX_PATH]{};
        const int written = std::snprintf(folder, sizeof(folder), "%s\\titanfall2vr", base);
        if (written > 0 && static_cast<size_t>(written) < sizeof(folder)) {
            const BOOL made = CreateDirectoryA(folder, nullptr);
            if (made || GetLastError() == ERROR_ALREADY_EXISTS) {
                const int full = std::snprintf(out, size, "%s\\", folder);
                if (full > 0 && static_cast<size_t>(full) < size) return true;
            }
        }
    }
    char temp[MAX_PATH]{};
    if (!GetTempPathA(MAX_PATH, temp)) return false;
    const int full = std::snprintf(out, size, "%s", temp);
    return full > 0 && static_cast<size_t>(full) < size;
}

HANDLE OpenLog() {
    char tempPath[MAX_PATH]{};
    if (!LogDirectory(tempPath, sizeof(tempPath))) return INVALID_HANDLE_VALUE;
    const size_t length = std::strlen(tempPath);
    constexpr char filename[] = "titanfall2vr.log";
    if (length + sizeof(filename) > sizeof(tempPath)) return INVALID_HANDLE_VALUE;
    // BEFORE the file is created, and exactly once per process -- whoever logs
    // first triggers it, so there is no ordering to get wrong at the call site.
    static std::atomic_flag rotated = ATOMIC_FLAG_INIT;
    if (!rotated.test_and_set(std::memory_order_acq_rel)) RotateLogs(tempPath);
    std::memcpy(tempPath + length, filename, sizeof(filename));
    // FILE_APPEND_DATA keeps every write atomic at the end of the file, which is
    // what lets several game threads share one handle without a lock of ours.
    return CreateFileA(tempPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

HANDLE LogHandle() {
    HANDLE handle = g_file.load(std::memory_order_acquire);
    if (handle != INVALID_HANDLE_VALUE) return handle;
    HANDLE opened = OpenLog();
    if (opened == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE expected = INVALID_HANDLE_VALUE;
    if (g_file.compare_exchange_strong(expected, opened, std::memory_order_acq_rel)) return opened;
    // Lost the race; the other thread's handle is the live one.
    CloseHandle(opened);
    return expected;
}

}  // namespace

// MILLISECONDS SINCE THE FIRST LINE, on every line.
//
// This log had no clock in it at all, and a whole phase of frame-pacing and
// crash work was read without one: "the game stopped presenting" could not be
// placed against "the level finished loading" except by counting log lines and
// hoping they were evenly spaced. The 2026-08-23 load crash was reconstructed
// from a 2-second beat because it was the only thing in the file that knew what
// time it was.
//
// One QPC read and one integer format per line, into the SAME buffer as the
// message, so it stays one WriteFile. Two writes would double the syscall count
// on a file this project already knows it can flood.
double MillisecondsSinceStart() {
    static LARGE_INTEGER frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f; }();
    static LARGE_INTEGER start = [] { LARGE_INTEGER s{}; QueryPerformanceCounter(&s); return s; }();
    if (!frequency.QuadPart) return 0.0;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - start.QuadPart) * 1000.0 /
           static_cast<double>(frequency.QuadPart);
}

// ---- AND THE WRITE ITSELF IS BUFFERED, for the same reason ----------------
//
// The open/close pair and OutputDebugStringA went years ago, per the note
// above, and what survived was one WriteFile per line. That is still a syscall
// per line on the game's own threads, and 2026-09-11 measured what it costs:
// 9,414 lines over 86.8 s of play -- about 108 syscalls a second in the steady
// state -- and they do NOT arrive evenly. 47 separate 16 ms frames wrote fifty
// lines or more, the worst 451, and the sixty-to-seventy line bursts land
// every fifteen seconds on the nose, which is a periodic census report rather
// than anything the player did. Seventy syscalls inside one frame is the
// "extra 4 to 5 ms every 20 to 30 seconds" of KNOWN-ISSUES 5, measured at last
// instead of guessed at.
//
// So lines accumulate and go out in one write. A burst of seventy becomes one
// syscall; the steady state becomes about four a second instead of 108. The
// flush is bounded by TIME as well as by size so nothing sits unwritten while
// something is going wrong, and Tf2VrLogFlush() is exposed for the crash path,
// which must not lose the lines that explain the crash.
struct LogLockHolder {
    CRITICAL_SECTION cs;
    LogLockHolder() { InitializeCriticalSection(&cs); }
};
CRITICAL_SECTION& LogLock() {
    static LogLockHolder holder;   // magic static: initialised once, thread-safe
    return holder.cs;
}
constexpr std::size_t kLogBufferBytes = 32768;
constexpr std::uint64_t kLogFlushMs = 250;
char g_logBuffer[kLogBufferBytes];
std::size_t g_logUsed = 0;
std::uint64_t g_logLastFlush = 0;

// Caller holds the lock.
void FlushLogLocked(HANDLE file) {
    if (!g_logUsed || file == INVALID_HANDLE_VALUE) { g_logUsed = 0; return; }
    DWORD written = 0;
    WriteFile(file, g_logBuffer, static_cast<DWORD>(g_logUsed), &written, nullptr);
    g_logUsed = 0;
    g_logLastFlush = GetTickCount64();
}

void AppendOrWrite(HANDLE file, const char* data, std::size_t length) {
    // A line too big for the buffer is written straight through rather than
    // splitting it: the session banner is over a kilobyte and one oversized
    // line a session is not what this is defending against.
    if (length >= kLogBufferBytes) {
        FlushLogLocked(file);
        DWORD written = 0;
        WriteFile(file, data, static_cast<DWORD>(length), &written, nullptr);
        return;
    }
    if (g_logUsed + length > kLogBufferBytes) FlushLogLocked(file);
    std::memcpy(g_logBuffer + g_logUsed, data, length);
    g_logUsed += length;
    const std::uint64_t now = GetTickCount64();
    if (g_logLastFlush == 0) g_logLastFlush = now;
    if (now - g_logLastFlush >= kLogFlushMs) FlushLogLocked(file);
}

// ---- QUIET BY DEFAULT, VERBOSE ON REQUEST ---------------------------------
//
// 2026-09-11, measured: 9,559 lines in 87 s of play. The volume is NOT a few
// chatty families that could be denylisted -- the eighteen biggest are only
// 56% of it and the rest is a long tail -- so the only thing that works is to
// ship SILENT and let a developer turn it on.
//
// The rule is deliberately an ALLOWLIST of what survives, not a denylist of
// what goes, and it is generous: anything that reads like a failure, a
// refusal, a crash, or the run's own identity is kept. A line nobody thought
// about is dropped in release and present in dev, which is the safe direction
// for a log whose whole job in release is "something went wrong, and here is
// what".
std::atomic_bool g_logVerbose = false;

bool KeepWhenQuiet(const char* message) {
    // TIGHTENED AGAINST A REAL LOG, not chosen by taste. The first list used
    // loose lowercase words and let 6 lines a second through in play: "refus"
    // matched 222 lines of prose, "cannot" 116. This project SHOUTS its real
    // failures -- REFUSED, FAILED, INVALID, DISQUALIFIED -- so the uppercase
    // forms are the signal and the lowercase ones were noise. Re-measured
    // against the same 9,559-line run: 56 lines kept, 11 KB, and ZERO during
    // steady play.
    static const char* const kKeep[] = {
        "CRASH", "crash recorder", "FAILED", "failed", "REFUSED", "REFUSING",
        "refusing", "ERROR", "error", "INVALID", "DISQUALIF", "WARNING",
        "NOT INSTALLED", "not installed", "not loaded", "could not", "Could not",
        "PROFILE", "config:", "unknown setting", "LOG ROTATED",
        "EXCEPTION", "exception", "ACCESS_VIOLATION",
        // THE RUN'S IDENTITY. An alpha tester's log that names neither the
        // OpenXR runtime nor the headset is useless for triage, and the first
        // tightening pass dropped both.
        "OpenXR runtime", "does not meet",
        // A WEAPON CENSUSING ITSELF IS A RESULT, and it happens during ordinary
        // play. Dozens of weapons are not available at the gun range, so a
        // dedicated census session would cover a fraction of them and waste the
        // wearer's time; instead every session records the weapons that were
        // actually held, and the table fills in from normal play. Without this
        // line the quiet filter drops them and that harvest is impossible.
        "GRIP LATCHED", "GRIP REFUSED",
        // Frame spikes are the one PERFORMANCE family that survives quiet mode.
        // A release build that cannot report a stall is no use when someone
        // says "it stutters", which is exactly how this session started.
        "SPIKE",
        // HITCH, AND ONLY HITCH. plugin_cost.cpp times every frame segment in
        // every build and keeps the worst SINGLE frame per segment; HITCH is
        // the line that fires when one frame spends over 4 ms inside the
        // plugin, naming the worst five segments in order. It is triggered by
        // the fault, it is rate-limited to one line per 400 ms, and it fired
        // TWICE in a four-minute run -- so it belongs in a release log for the
        // same reason SPIKE does: someone says "it stutters" and the log has to
        // be able to answer.
        //
        // ITS TWO COMPANIONS DO NOT SHIP. `PLUGIN COST` and `RUNFRAME SEGMENTS`
        // print every 240 frames whether or not anything is wrong, which is
        // exactly what made them the positive control for the 2026-09-11 cost
        // run -- and exactly why they cannot be on in a player's log: they were
        // 176 lines of that run on their own. `diag.verbose = 1` brings them
        // back for a dev session, which is where a positive control is wanted.
        "HITCH",
        // WHAT THE RUNTIME SAID ABOUT THE SESSION, kept because six freezes
        // have now been read without it.
        //
        // Every one ends the same way: the frame loop stops dead from a
        // HEALTHY rate -- 82 fps in the 2026-09-11 Titan run, 1.8% missed in
        // the one before it -- and tens of seconds later the runtime faults
        // inside its own D3D11 path. The question every time is what happened
        // at the moment the loop stopped, and xr_context.cpp already writes the
        // answer if there is one: a transition to STOPPING, LOSS_PENDING or
        // EXITING names the runtime taking the session away. That line went
        // through Tf2VrLog and was dropped here, three sightings running.
        //
        // It is not an instrument and it is not periodic: session state changes
        // about five times in a session, all during startup. This costs a
        // handful of lines and turns the next tester's freeze into a report
        // that says something.
        "XR session state",
    };
    for (const char* needle : kKeep) {
        if (std::strstr(message, needle)) return true;
    }
    return false;
}

void Tf2VrSetLogVerbose(bool enabled) {
    const bool was = g_logVerbose.exchange(enabled, std::memory_order_release);
    if (was == enabled) return;
    Tf2VrLogAlways(enabled
        ? "[TF2VR] diag.verbose = 1: every line is written. This is the dev setting; it costs a "
          "syscall per line on the game's own threads and it is what the release build turns off.\n"
        : "[TF2VR] diag.verbose = 0: quiet. Only failures, refusals, crashes and the run's identity "
          "are written from here on.\n");
}

bool Tf2VrLogVerboseEnabled() { return g_logVerbose.load(std::memory_order_acquire); }

void Tf2VrLogWrite(const char* message, bool force) {
    if (!message) return;
    if (!force && !g_logVerbose.load(std::memory_order_relaxed) && !KeepWhenQuiet(message)) return;
    if (g_debugChannel.load(std::memory_order_relaxed)) OutputDebugStringA(message);
    const HANDLE file = LogHandle();
    if (file == INVALID_HANDLE_VALUE) return;
    // The rotation happened inside LogHandle, before the file existed, so it
    // could not log itself without recursing. It goes out here instead, ahead of
    // whatever line triggered the open, so it is literally line 1 of the file it
    // is describing. One relaxed load per line in the steady state.
    if (g_rotationReportPending.load(std::memory_order_relaxed) &&
        g_rotationReportPending.exchange(false, std::memory_order_acq_rel)) {
        EnterCriticalSection(&LogLock());
        AppendOrWrite(file, g_rotationReport, std::strlen(g_rotationReport));
        LeaveCriticalSection(&LogLock());
    }
    const size_t length = std::strlen(message);
    char stamped[2600];
    const int prefix = std::snprintf(stamped, sizeof(stamped), "[%10.1f] ", MillisecondsSinceStart());
    DWORD written = 0;
    // Long lines (the session banner is over a kilobyte) fall back to writing
    // the message alone rather than truncating it: a stamp is worth less than
    // the line it would cut short.
    (void)written;
    EnterCriticalSection(&LogLock());
    if (prefix > 0 && length + static_cast<size_t>(prefix) < sizeof(stamped)) {
        std::memcpy(stamped + prefix, message, length);
        AppendOrWrite(file, stamped, length + static_cast<size_t>(prefix));
    } else {
        AppendOrWrite(file, message, length);
    }
    LeaveCriticalSection(&LogLock());
}

// FLUSH NOW. For the crash path, which must not lose the lines that explain
// the crash. TryEnter, never Enter: a handler that blocks on a lock held by the
// thread it interrupted deadlocks, and losing a few lines beats hanging the
// game on the way down.
// UNCONDITIONAL. For the crash recorder and anything else whose whole purpose
// is to be in the file when the worst happens.
// THE BYPASS IS A PARAMETER, NOT A GLOBAL. This used to exchange g_logVerbose
// to true, log, and put it back -- which races catastrophically. Two threads
// interleaving as A-exchange(was=false), B-exchange(was=true), A-store(false),
// B-store(true) leave the flag TRUE for the rest of the process, and the 9,559
// lines per 87 s that this session was spent removing come back silently.
// Callers on different threads demonstrably overlap: the submit thread writes
// FRAME SPIKES every ten seconds, the crash recorder writes ~45 lines
// back-to-back, and the main thread writes the tag. Even without the latch,
// every other thread's quiet filter was bypassed for the duration.
void Tf2VrLogAlways(const char* message) { Tf2VrLogWrite(message, true); }

void Tf2VrLog(const char* message) { Tf2VrLogWrite(message, false); }

// A HANG IS THE CASE THIS PROJECT DEBUGS, and a buffer is exactly wrong for it.
// The 250 ms bound only fires on a LATER log call, so a process that stops
// logging -- hung, or shut down -- keeps up to 32 KB unwritten forever. This
// file's own header rejected buffering once for this reason. So a tiny thread
// flushes on a timer, independent of whether anything is still logging, and
// the buffer only ever holds a quarter second of lines.
DWORD WINAPI LogFlushThread(LPVOID) {
    for (;;) {
        Sleep(250);
        Tf2VrLogFlush();
    }
}

void Tf2VrLogStartFlusher() {
    static std::atomic_bool started{false};
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) return;
    const HANDLE thread = CreateThread(nullptr, 0, LogFlushThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
}

void Tf2VrLogFlush() {
    const HANDLE file = LogHandle();
    if (file == INVALID_HANDLE_VALUE) return;
    if (!TryEnterCriticalSection(&LogLock())) return;
    FlushLogLocked(file);
    LeaveCriticalSection(&LogLock());
}

void Tf2VrSetLogDebugChannel(bool enabled) {
    g_debugChannel.store(enabled, std::memory_order_relaxed);
}
