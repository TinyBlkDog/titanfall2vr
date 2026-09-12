#include "engine_cvars.h"

#include "diagnostics.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// Layouts and vtable indices from NorthstarLauncher's tier1 (the read-only
// reference clone in this repo, built against this same game build):
//   primedev/tier1/cvar.h    -- CCvar::FindVar is vtable 16,
//                               CCvar::FactoryInternalIterator is vtable 41
//   primedev/tier1/cmd.h     -- ConCommandBase: name 0x18, flags 0x28
//   primedev/tier1/convar.h  -- ConVar: value string 0x48, float 0x58, int 0x5C
//
// Every one of them is still VERIFIED before use. An index that has moved would
// otherwise call an unrelated virtual with a string argument, and an offset that
// has moved would read a pointer out of the middle of some other field and then
// dereference it.
constexpr int kFindVarVtableIndex = 16;
constexpr int kFactoryInternalIteratorVtableIndex = 41;
constexpr std::uintptr_t kConCommandBaseName = 0x18;
constexpr std::uintptr_t kConCommandBaseFlags = 0x28;
constexpr std::uintptr_t kConVarValueString = 0x48;
constexpr std::uintptr_t kConVarValueFloat = 0x58;
constexpr std::uintptr_t kConVarValueInt = 0x5C;

// Iterator vtable, from CCVarIteratorInternal in the same header.
constexpr int kIteratorSetFirst = 0;
constexpr int kIteratorNext = 1;
constexpr int kIteratorIsValid = 2;
constexpr int kIteratorGet = 3;

using CreateInterfaceFn = void* (*)(const char* name, int* returnCode);
using FindVarFn = void*(__fastcall*)(void* self, const char* name);
using FactoryIteratorFn = void*(__fastcall*)(void* self);
using IteratorVoidFn = void(__fastcall*)(void* self);
using IteratorIsValidFn = bool(__fastcall*)(void* self);
using IteratorGetFn = void*(__fastcall*)(void* self);

void* g_cvarInterface = nullptr;
bool g_interfaceResolved = false;
bool g_requested = false;
bool g_done = false;

bool IsReadable(const void* address, size_t bytes) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    const DWORD protection = info.Protect & 0xFF;
    const bool readable = protection == PAGE_READONLY || protection == PAGE_READWRITE ||
                          protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
                          protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    if (!readable) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto end = start + info.RegionSize;
    const auto wanted = reinterpret_cast<std::uintptr_t>(address) + bytes;
    return wanted <= end;
}

bool IsExecutable(const void* address) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    const DWORD protection = info.Protect & 0xFF;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

// How many bytes are readable from `address` before its region ends. One
// VirtualQuery, and the answer bounds a scan without needing another.
size_t ReadableBytesFrom(const void* address) {
    if (!address) return 0;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info))) return 0;
    if (info.State != MEM_COMMIT) return 0;
    const DWORD protection = info.Protect & 0xFF;
    const bool readable = protection == PAGE_READONLY || protection == PAGE_READWRITE ||
                          protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
                          protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    if (!readable) return 0;
    const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto end = start + info.RegionSize;
    const auto here = reinterpret_cast<std::uintptr_t>(address);
    return here < end ? static_cast<size_t>(end - here) : 0;
}

// A cvar name is a short printable C string in the owning module's rdata. This
// bounds the read and rejects anything that is not one, so a wrong offset
// produces a refusal rather than a crash inside strlen.
//
// ONE VirtualQuery, NOT ONE PER BYTE, and that is the whole difference between
// a diagnostic and a six-second freeze.
//
// This used to call IsReadable(name + index, 1) inside the scan loop -- a kernel
// transition per character, up to 130 of them per name. Harmless at the ~266
// entries the probe used to inspect. Commit 33341e8 started running it over all
// 3247 entries to dump the whole table, which is ~400,000 syscalls on the game
// thread, and the probe self-arms at the world gate. That is the level-entry
// freeze: measured 5845 ms, flat and VR alike, once per entry, indifferent to
// render resolution -- which is exactly why halving the resolution and fixing
// the log both refuted nothing.
//
// The safety semantics are unchanged: the scan still stops at the end of the
// committed readable region and still refuses anything non-printable or
// unterminated. It just asks where that end is once instead of per character.
const char* SafeName(const void* object, std::uintptr_t offset) {
    if (!IsReadable(object, offset + sizeof(void*))) return nullptr;
    const char* name = *reinterpret_cast<const char* const*>(
        reinterpret_cast<const std::uint8_t*>(object) + offset);
    const size_t available = ReadableBytesFrom(name);
    if (!available) return nullptr;
    const size_t limit = available < 128 ? available : 128;
    for (size_t index = 0; index < limit; ++index) {
        const unsigned char c = static_cast<unsigned char>(name[index]);
        if (c == 0) return index > 0 ? name : nullptr;
        if (c < 0x20 || c > 0x7E) return nullptr;
    }
    return nullptr;   // unterminated within a sane length, or within its region
}

void* VirtualAt(void* object, int index) {
    if (!IsReadable(object, sizeof(void*))) return nullptr;
    void** vtable = *reinterpret_cast<void***>(object);
    if (!IsReadable(vtable, sizeof(void*) * (static_cast<size_t>(index) + 1))) return nullptr;
    void* function = vtable[index];
    return IsExecutable(function) ? function : nullptr;
}

// Resolved once and cached. vstdlib exports CreateInterface, and the interface
// name is the one NorthstarLauncher uses against this build.
// ONLY SUCCESS IS CACHED, AND THAT IS A BUG FIX, NOT A STYLE CHOICE.
//
// This used to set the resolved flag BEFORE trying, so one early failure
// poisoned the interface for the entire session. That is not hypothetical: the
// persistent viewmodel hide calls in from LoadPluginConfig, which runs before
// vstdlib.dll is loaded, and every later retry then got the cached null. The
// hide reported "r_drawviewmodel was not found as a ConVar" forever, while the
// identical lookup had worked the run before -- because that time the first
// call came 560 frames in, long after vstdlib was up.
//
// Failure is logged once rather than every retry, so a per-frame caller cannot
// flood the log.
void* CvarInterface() {
    if (g_cvarInterface) return g_cvarInterface;
    const bool logFailure = !g_interfaceResolved;
    g_interfaceResolved = true;
    HMODULE vstdlib = GetModuleHandleA("vstdlib.dll");
    if (!vstdlib) {
        if (logFailure) Tf2VrLog("[TF2VR] cvar: vstdlib.dll is not loaded yet; will retry.\n");
        return nullptr;
    }
    auto createInterface = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(vstdlib, "CreateInterface"));
    if (!createInterface) {
        Tf2VrLog("[TF2VR] cvar: vstdlib.dll does not export CreateInterface.\n");
        return nullptr;
    }
    int returnCode = 0;
    void* interfacePointer = createInterface("VEngineCvar007", &returnCode);
    if (!interfacePointer) {
        char line[200]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] cvar: VEngineCvar007 not offered by vstdlib.dll (return code %d).\n", returnCode);
        Tf2VrLog(line);
        return nullptr;
    }
    // The two slots this file actually calls, verified as code before either is
    // called. If the interface version ever changes shape, this refuses rather
    // than calling an unrelated virtual with a string argument.
    if (!VirtualAt(interfacePointer, kFindVarVtableIndex) ||
        !VirtualAt(interfacePointer, kFactoryInternalIteratorVtableIndex)) {
        Tf2VrLog("[TF2VR] cvar: VEngineCvar007 resolved, but its vtable slots 16/41 do not both point "
                 "at code. This is not the interface shape the reference clone documents, so nothing "
                 "was called.\n");
        return nullptr;
    }
    g_cvarInterface = interfacePointer;
    char line[200]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] cvar: VEngineCvar007 resolved at %p; cvars are reachable by name.\n", interfacePointer);
    Tf2VrLog(line);
    return g_cvarInterface;
}

void* FindVar(const char* name) {
    void* self = CvarInterface();
    if (!self || !name) return nullptr;
    auto findVar = reinterpret_cast<FindVarFn>(VirtualAt(self, kFindVarVtableIndex));
    if (!findVar) return nullptr;
    return findVar(self, name);
}

bool LooksLikeConVar(void* object) {
    return object && IsReadable(object, kConVarValueInt + sizeof(int));
}

// Case-insensitive substring.
bool NameContains(const char* name, const char* fragment) {
    for (const char* start = name; *start; ++start) {
        const char* a = start;
        const char* b = fragment;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
            if (ca != cb) break;
            ++a; ++b;
        }
        if (!*b) return true;
    }
    return false;
}

// WHAT WE ARE LOOKING FOR, AND WHY EACH GROUP IS HERE.
//
// The step 2 question is "what does the engine use to decide where a bullet
// goes". The zoom group is here for a second, already-open question: ADS
// detection, where the world frustum and the weapon frustum have both been
// measured and are both dead, and the remaining lead is the game's own zoom
// state. The two searches cost the same single enumeration.
const char* const kInterestingFragments[] = {
    "aim", "assist", "autoaim",
    "zoom", "ads", "iron", "scope", "sight",
    "crosshair", "reticle", "cross",
    "viewangle", "viewmodel", "vm_", "view_",
    "spread", "bullet", "projectile", "trace", "hitscan",
    "weapon", "wpn", "fire", "shoot", "recoil",
    "sensitivity", "yaw", "pitch",
    "sv_cheats",
};

const char* FlagsSummary(int flags, char* buffer, size_t size) {
    // Only the flags that change what can be DONE with a cvar. CHEAT and
    // DEVELOPMENTONLY decide whether setting it will be accepted at all, which
    // is the difference between a usable handle and one that silently refuses.
    std::snprintf(buffer, size, "%s%s%s%s%s",
                  (flags & (1 << 14)) ? "CHEAT " : "",
                  (flags & (1 << 1)) ? "DEVONLY " : "",
                  (flags & (1 << 4)) ? "HIDDEN " : "",
                  (flags & (1 << 13)) ? "REPLICATED " : "",
                  (flags & (1 << 19)) ? "RELEASE " : "");
    return buffer;
}

// THE WHOLE TABLE, TO ITS OWN FILE.
//
// The first enumeration logged only the entries matching a keyword list, which
// meant that answering "does cvar X exist" for any X not in that list needed
// another headset run. It cost one immediately: the fire-path instrument turned
// out to need checking against names -- impact, overlay, debug print -- that
// were not in the list, and there was no way to look them up.
//
// 3237 entries is nothing. Writing all of them once means no question about
// which cvars this build has ever needs the game launched again.
FILE* OpenDumpFile() {
    char path[MAX_PATH]{};
    if (!GetTempPathA(MAX_PATH, path)) return nullptr;
    std::strncat(path, "titanfall2vr-cvars.txt", sizeof(path) - std::strlen(path) - 1);
    FILE* file = nullptr;
    if (fopen_s(&file, path, "w") != 0 || !file) {
        char line[MAX_PATH + 96]{};
        std::snprintf(line, sizeof(line), "[TF2VR] cvar probe: could not open %s for the full dump.\n", path);
        Tf2VrLog(line);
        return nullptr;
    }
    char line[MAX_PATH + 96]{};
    std::snprintf(line, sizeof(line), "[TF2VR] cvar probe: writing the COMPLETE table to %s.\n", path);
    Tf2VrLog(line);
    return file;
}

void RunProbe() {
    // THE PROBE REPORTS ITS OWN COST. It self-arms at the world gate, so its
    // cost is paid at every level entry the wearer plays -- and for two days it
    // was 5.8 seconds of VirtualQuery syscalls that nothing in the log named.
    LARGE_INTEGER probeFreq{}, probeStart{}, probeEnd{};
    QueryPerformanceFrequency(&probeFreq);
    QueryPerformanceCounter(&probeStart);
    void* self = CvarInterface();
    if (!self) {
        Tf2VrLog("[TF2VR] cvar probe: no interface, nothing enumerated. This is a NULL RESULT ABOUT "
                 "THE INTERFACE, not about the engine's cvars.\n");
        return;
    }
    auto factory = reinterpret_cast<FactoryIteratorFn>(VirtualAt(self, kFactoryInternalIteratorVtableIndex));
    if (!factory) { Tf2VrLog("[TF2VR] cvar probe: iterator factory slot did not verify.\n"); return; }
    // Allocated by the engine and deliberately not freed: the reference
    // implementation leaks it too, there is no documented destructor slot, and
    // this runs a handful of times per session at most.
    void* iterator = factory(self);
    if (!iterator) { Tf2VrLog("[TF2VR] cvar probe: the iterator factory returned null.\n"); return; }
    auto setFirst = reinterpret_cast<IteratorVoidFn>(VirtualAt(iterator, kIteratorSetFirst));
    auto next = reinterpret_cast<IteratorVoidFn>(VirtualAt(iterator, kIteratorNext));
    auto isValid = reinterpret_cast<IteratorIsValidFn>(VirtualAt(iterator, kIteratorIsValid));
    auto get = reinterpret_cast<IteratorGetFn>(VirtualAt(iterator, kIteratorGet));
    if (!setFirst || !next || !isValid || !get) {
        Tf2VrLog("[TF2VR] cvar probe: the iterator's vtable slots 0-3 did not verify; nothing was "
                 "enumerated.\n");
        return;
    }

    FILE* dump = OpenDumpFile();
    unsigned total = 0, vars = 0, commands = 0, matched = 0, unreadable = 0;
    // Bounded so a corrupt or circular list cannot spin the frame forever.
    constexpr unsigned kMaxEntries = 20000;
    Tf2VrLog("[TF2VR] cvar probe: enumerating. Matches below are name substrings relevant to aim, "
             "zoom and the fire path; the totals at the end say whether the enumeration itself "
             "worked.\n");
    for (setFirst(iterator); isValid(iterator) && total < kMaxEntries; next(iterator)) {
        ++total;
        void* command = get(iterator);
        const char* name = SafeName(command, kConCommandBaseName);
        if (!name) { ++unreadable; continue; }

        // FindVar returns non-null only for ConVars, so it separates variables
        // from commands without depending on a guessed virtual index for
        // IsCommand -- which is the one thing in this layout the reference clone
        // does not pin down for this build.
        void* asVar = FindVar(name);
        if (asVar) ++vars; else ++commands;

        int entryFlags = 0;
        if (IsReadable(command, kConCommandBaseFlags + sizeof(int))) {
            entryFlags = *reinterpret_cast<const int*>(
                reinterpret_cast<const std::uint8_t*>(command) + kConCommandBaseFlags);
        }
        if (dump) {
            char entryFlagText[64]{};
            FlagsSummary(entryFlags, entryFlagText, sizeof(entryFlagText));
            if (asVar && LooksLikeConVar(asVar)) {
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(asVar);
                const char* stringValue = SafeName(asVar, kConVarValueString);
                std::fprintf(dump, "cvar\t%s\t%.6f\t%d\t%s\t%s\n", name,
                             static_cast<double>(*reinterpret_cast<const float*>(bytes + kConVarValueFloat)),
                             *reinterpret_cast<const int*>(bytes + kConVarValueInt),
                             stringValue ? stringValue : "", entryFlagText);
            } else {
                std::fprintf(dump, "cmd\t%s\t\t\t\t%s\n", name, entryFlagText);
            }
        }

        bool interesting = false;
        for (const char* fragment : kInterestingFragments) {
            if (NameContains(name, fragment)) { interesting = true; break; }
        }
        if (!interesting) continue;
        ++matched;

        char flagText[64]{};
        FlagsSummary(entryFlags, flagText, sizeof(flagText));
        char line[420]{};
        if (asVar && LooksLikeConVar(asVar)) {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(asVar);
            const float floatValue = *reinterpret_cast<const float*>(bytes + kConVarValueFloat);
            const int intValue = *reinterpret_cast<const int*>(bytes + kConVarValueInt);
            const char* stringValue = SafeName(asVar, kConVarValueString);
            std::snprintf(line, sizeof(line), "[TF2VR] cvar   %-42s = %-14.4f (int %d, string '%s') %s\n",
                          name, static_cast<double>(floatValue), intValue,
                          stringValue ? stringValue : "<unreadable>", flagText);
        } else {
            std::snprintf(line, sizeof(line), "[TF2VR] cmd    %-42s %s\n", name, flagText);
        }
        Tf2VrLog(line);
    }

    if (dump) std::fclose(dump);
    char summary[520]{};
    std::snprintf(summary, sizeof(summary),
        "[TF2VR] cvar probe: %u entries enumerated (%u variables, %u commands, %u with an unreadable "
        "name), %u matched the aim/zoom/fire fragments. A total in the thousands means the "
        "enumeration worked and any empty category below is a real absence; a total of 0 or a few "
        "means it did not, and nothing below is evidence.%s\n",
        total, vars, commands, unreadable, matched,
        total >= kMaxEntries ? " NOTE: hit the iteration cap, so this is a partial list." : "");
    Tf2VrLog(summary);
    QueryPerformanceCounter(&probeEnd);
    if (probeFreq.QuadPart) {
        char cost[320]{};
        std::snprintf(cost, sizeof(cost),
            "[TF2VR] cvar probe COST %.0f ms, on the game thread, at the world gate -- it self-arms "
            "there, so this is paid at EVERY level entry. Anything above a few tens of ms here is "
            "not a diagnostic, it is the level-entry freeze.\n",
            static_cast<double>(probeEnd.QuadPart - probeStart.QuadPart) * 1000.0 /
                static_cast<double>(probeFreq.QuadPart));
        Tf2VrLog(cost);
    }
}

}  // namespace

void RequestCvarProbe() {
    g_requested = true;
    g_done = false;
}

void AdvanceCvarProbe() {
    if (!g_requested || g_done) return;
    // Cvars are registered as their owning module loads. Enumerating during
    // startup would report a fraction of the table and look exactly like a
    // complete answer -- the trap that put the auto-arm's hook on the wrong
    // entry -- so this waits for the module that owns the gameplay cvars.
    if (!GetModuleHandleA("client.dll") || !GetModuleHandleA("engine.dll")) return;
    g_done = true;
    RunProbe();
}

bool TryReadCvarFloat(const char* name, float& value) {
    void* var = FindVar(name);
    if (!LooksLikeConVar(var)) return false;
    value = *reinterpret_cast<const float*>(
        reinterpret_cast<const std::uint8_t*>(var) + kConVarValueFloat);
    return true;
}

bool TryReadCvarString(const char* name, char* buffer, unsigned size) {
    if (!buffer || !size) return false;
    void* var = FindVar(name);
    if (!LooksLikeConVar(var)) return false;
    const char* text = SafeName(var, kConVarValueString);
    if (!text) return false;
    std::snprintf(buffer, size, "%s", text);
    return true;
}

// WRITING A CVAR BY NAME.
//
// Writes the value slots directly rather than going through ConVar::SetValue's
// vtable entry. That is deliberate and it has a cost worth stating: SetValue
// also updates the string form and fires any registered change callback, and
// this does neither. For a render flag read once a frame as an int that is
// invisible; for a cvar whose owner reacts to changes it would not be, so this
// is not a general-purpose setter and should not become one without adding the
// vtable call.
//
// Both offsets are the ones this file's own enumeration already relies on, and
// they are independently asserted in hand_cvars.cpp against the layout the
// engine's convar registration produces.
bool TrySetCvarFloat(const char* name, float value) {
    void* var = FindVar(name);
    if (!LooksLikeConVar(var)) return false;
    auto* bytes = reinterpret_cast<std::uint8_t*>(var);
    *reinterpret_cast<float*>(bytes + kConVarValueFloat) = value;
    *reinterpret_cast<int*>(bytes + kConVarValueInt) = static_cast<int>(value);
    return true;
}
