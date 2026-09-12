#include "squirrel_natives.h"

#include "diagnostics.h"
#include "hook_registry.h"
#include "scan_outcome.h"

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Filled by the interceptor's install, read by the interceptor.
extern "C" std::uintptr_t g_sqRegisterContinue = 0;
extern "C" void sqRegisterInterceptor();

namespace {

// client.dll+0x13790, the inner registration function. See squirrel_natives.asm
// for why this rather than the 0x108E0 entry Northstar documents and hooks.
constexpr std::uintptr_t kRegisterRva = 0x13790;

// mov [rsp+20h],rbx ; push rbp ; push rdi ; push r14 ; sub rsp,80h
// Read out of the shipped client.dll offline, not guessed. If these bytes are
// not there, this is not the build the offset came from -- or something else
// has already patched it -- and either way nothing gets written.
constexpr std::uint8_t kExpected[] = {
    0x48, 0x89, 0x5C, 0x24, 0x20,
    0x55,
    0x57,
    0x41, 0x56,
    0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00,
};
constexpr std::size_t kDisplaced = sizeof(kExpected);
static_assert(kDisplaced >= 14, "an absolute jump needs 14 bytes");

// The layout is NorthstarLauncher's, from
// primedev/vscript/languages/squirrel_re/include/squirrel.h. Only the fields
// this dump reports are named; the rest are held as raw storage so the stride
// stays right.
struct SQFuncRegistrationView {
    const char* squirrelFuncName;   // 0x00
    const char* cppFuncName;        // 0x08
    const char* helpText;           // 0x10
    const char* returnTypeString;   // 0x18
    const char* argTypes;           // 0x20
    std::uint32_t unknown1;         // 0x28
    std::uint32_t devLevel;         // 0x2C
    const char* shortNameMaybe;     // 0x30
    std::uint32_t unknown2;         // 0x38
    std::uint32_t returnType;       // 0x3C
    std::uint32_t* externalBuffer;  // 0x40
    std::uint64_t externalBufferSize;  // 0x48
    std::uint64_t unknown3;         // 0x50
    std::uint64_t unknown4;         // 0x58
    void* funcPtr;                  // 0x60
};
static_assert(sizeof(SQFuncRegistrationView) == 0x68, "SQFuncRegistration is 0x68 bytes");

// The strings a registration points at are string literals in client.dll's
// .rdata and outlive the process, so holding the pointers is safe. The
// registration struct ITSELF is a temporary -- 0x108E0 builds it on the stack --
// so the fields are copied out here and the struct pointer is never kept.
struct Entry {
    const char* name;
    const char* cppName;
    const char* returnTypeString;
    const char* argTypes;
    const void* funcPtr;
    std::uint32_t devLevel;
    std::uint32_t returnType;
    // Set by the dedup pass at write time, never in the recorder.
    bool unique;
};

// THE FIRST RUN OVERFLOWED 4096 BY 243, AND SAID SO.
//
// The offline call count (958 reaching the documented entry point) was a bad
// estimate of the live total: registrations arrive once per VM, more than one
// VM is built, and Northstar registers its own natives through the same path.
// The measured figure was 4339 for a single map load.
//
// 32768 carries roughly twenty VM constructions. It is deliberately far more
// than one run needs, because the cost of overshooting is a megabyte of static
// storage and the cost of undershooting is a file that is missing exactly the
// native somebody later goes looking for. If it ever fills anyway, the
// ScanOutcome in the report says so IN the header rather than letting a
// truncated dump read as a complete one -- which is how the 243 were caught.
constexpr std::size_t kCapacity = 32768;
Entry g_entries[kCapacity]{};
std::atomic<std::size_t> g_count{0};
std::atomic<std::uint64_t> g_dropped{0};

std::uint8_t* g_site = nullptr;
std::uint8_t g_original[kDisplaced]{};
DWORD g_originalProtect = 0;
bool g_enabled = false;
bool g_installAttempted = false;

// The dump is rewritten whenever the registration count grows, so a capture
// that arrives in more than one burst -- the UI VM at startup, the client VM at
// map load -- ends up complete rather than frozen at whichever burst landed
// first. Waiting on a stable count rather than on a frame number means it does
// not matter which order they come in.
std::size_t g_dumpedCount = 0;
std::size_t g_lastSeenCount = 0;
unsigned g_stableFrames = 0;

const char* SafeString(const char* s) {
    if (!s) return "-";
    // A registration field could be anything if the struct layout is wrong;
    // refuse to walk a pointer that is not readable rather than fault inside
    // the dump.
    if (IsBadReadPtr(s, 1)) return "<unreadable>";
    return s;
}

// Flags the first occurrence of each (name, funcPtr) pair and returns how many
// there were.
//
// The same native is registered once per VM, so the raw capture holds each one
// several times over -- the first run had GetViewModelEntity twice and SetOrigin
// four times. Deduping happens HERE, at write time, and never in the recorder:
// the recorder runs a thousand times in a row while the VM is built, and a
// linear scan per registration would turn that burst into a hitch at map load.
//
// The pair is compared by POINTER, not by string. Both come from client.dll's
// .rdata, so identical names share an address; two natives that genuinely differ
// in implementation keep both rows, which is what makes the two distinct
// SetOrigin registrations visible rather than collapsed.
std::size_t MarkUnique(std::size_t count) {
    // Open addressing over indices, sized to the next power of two above twice
    // the count so the table stays under half full and probes stay short.
    static std::uint32_t table[kCapacity * 2];
    const std::size_t mask = (kCapacity * 2) - 1;
    std::memset(table, 0xFF, sizeof(table));

    std::size_t unique = 0;
    for (std::size_t i = 0; i < count && i < kCapacity; ++i) {
        Entry& e = g_entries[i];
        e.unique = false;
        const auto a = reinterpret_cast<std::uintptr_t>(e.name);
        const auto b = reinterpret_cast<std::uintptr_t>(e.funcPtr);
        std::uint64_t h = a * 0x9E3779B97F4A7C15ull ^ (b + 0x165667B19E3779F9ull);
        h ^= h >> 29;
        std::size_t slot = static_cast<std::size_t>(h) & mask;
        bool duplicate = false;
        for (std::size_t probe = 0; probe <= mask; ++probe) {
            const std::uint32_t held = table[slot];
            if (held == 0xFFFFFFFFu) break;  // empty: first time seen
            const Entry& other = g_entries[held];
            if (other.name == e.name && other.funcPtr == e.funcPtr) {
                duplicate = true;
                break;
            }
            slot = (slot + 1) & mask;
        }
        if (duplicate) continue;
        table[slot] = static_cast<std::uint32_t>(i);
        e.unique = true;
        ++unique;
    }
    return unique;
}

void WriteDump() {
    char path[MAX_PATH]{};
    if (!GetTempPathA(sizeof(path), path)) {
        Tf2VrLog("[TF2VR] natives dump: GetTempPath failed; nothing written.\n");
        return;
    }
    std::strncat(path, "tf2vr-natives.txt", sizeof(path) - std::strlen(path) - 1);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) {
        char line[MAX_PATH + 96]{};
        std::snprintf(line, sizeof(line), "[TF2VR] natives dump: could not open %s for writing.\n", path);
        Tf2VrLog(line);
        return;
    }

    const std::size_t count = g_count.load(std::memory_order_acquire);
    const std::uint64_t dropped = g_dropped.load(std::memory_order_acquire);

    // Every bounded loop in this project reports which bound ended it, in the
    // same line as the counts. A dump that silently lost entries would send
    // Phase C looking for a native that was registered and simply not recorded.
    ScanOutcome outcome;
    outcome.Finish(dropped ? ScanOutcome::End::InstanceCap : ScanOutcome::End::Complete);

    // AN RVA IS ONLY AN RVA IF THE POINTER IS ACTUALLY IN THE MODULE.
    //
    // The first run printed rva=0x7E59E41DE790 for NSFetchVerifiedModsManifesto:
    // a Northstar native, whose implementation lives in Northstar.dll, minus
    // client.dll's base. The subtraction succeeded and the number was garbage.
    // Phase C looks native names up in this file and calls the address it finds,
    // so an offset into the wrong module is not cosmetic -- it is a call into
    // nowhere. The bound is checked, and anything outside it is labelled with
    // the module that really owns it instead of being given a fake offset.
    std::uintptr_t clientBase = 0;
    std::size_t clientSize = 0;
    if (HMODULE client = GetModuleHandleA("client.dll")) {
        clientBase = reinterpret_cast<std::uintptr_t>(client);
        MODULEINFO mi{};
        if (GetModuleInformation(GetCurrentProcess(), client, &mi, sizeof(mi))) {
            clientSize = mi.SizeOfImage;
        }
    }

    const std::size_t unique = MarkUnique(count);

    std::fprintf(f, "# Squirrel native registrations, captured as each VM was built.\n");
    std::fprintf(f, "# hook: client.dll+0x%llX (inner registration function)\n",
                 static_cast<unsigned long long>(kRegisterRva));
    std::fprintf(f, "# client.dll base 0x%llX size 0x%llX\n",
                 static_cast<unsigned long long>(clientBase),
                 static_cast<unsigned long long>(clientSize));
    std::fprintf(f, "# registrations: %zu  unique: %zu  dropped-for-capacity: %llu\n", count, unique,
                 static_cast<unsigned long long>(dropped));
    std::fprintf(f, "# %s\n", outcome.Describe());
    std::fprintf(f, "#\n# Duplicates are removed: the same native is registered once per VM.\n");
    std::fprintf(f, "# rva= is a client.dll offset and is what survives a restart. Entries marked\n");
    std::fprintf(f, "# module=<name> are NOT in client.dll and have no client.dll RVA at all.\n");
    std::fprintf(f, "#\n# name  rva  args  returns  cpp-name  dev-level\n\n");

    for (std::size_t i = 0; i < count && i < kCapacity; ++i) {
        const Entry& e = g_entries[i];
        if (!e.unique) continue;
        const auto fn = reinterpret_cast<std::uintptr_t>(e.funcPtr);
        char where[96]{};
        if (clientBase && clientSize && fn >= clientBase && fn < clientBase + clientSize) {
            std::snprintf(where, sizeof(where), "rva=0x%08llX",
                          static_cast<unsigned long long>(fn - clientBase));
        } else {
            char owner[MAX_PATH]{};
            HMODULE mod = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(fn), &mod) &&
                mod && GetModuleBaseNameA(GetCurrentProcess(), mod, owner, sizeof(owner))) {
                std::snprintf(where, sizeof(where), "module=%s+0x%llX", owner,
                              static_cast<unsigned long long>(fn - reinterpret_cast<std::uintptr_t>(mod)));
            } else {
                std::snprintf(where, sizeof(where), "module=UNKNOWN");
            }
        }
        std::fprintf(f, "%-46s %-30s abs=0x%llX args=%-34s ret=%-14s cpp=%-46s dev=%u ret_t=0x%X\n",
                     SafeString(e.name), where, static_cast<unsigned long long>(fn),
                     SafeString(e.argTypes), SafeString(e.returnTypeString), SafeString(e.cppName),
                     e.devLevel, e.returnType);
    }
    fclose(f);

    char line[MAX_PATH + 220]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR] natives dump: %zu registrations written to %s (dropped %llu). %s\n",
                  count, path, static_cast<unsigned long long>(dropped), outcome.Describe());
    Tf2VrLog(line);
}

}  // namespace

// Called from the interceptor once per registration, on whatever thread is
// building the VM. It must not allocate, log, or block: this runs about a
// thousand times in a row during VM construction, and anything slow here would
// show up as a hitch at map load rather than as a bug.
extern "C" void RecordSquirrelRegistration(const void* registration) {
    if (!registration) return;
    const auto* reg = static_cast<const SQFuncRegistrationView*>(registration);
    const std::size_t slot = g_count.fetch_add(1, std::memory_order_acq_rel);
    if (slot >= kCapacity) {
        g_count.store(kCapacity, std::memory_order_release);
        g_dropped.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    Entry& e = g_entries[slot];
    e.name = reg->squirrelFuncName;
    e.cppName = reg->cppFuncName;
    e.returnTypeString = reg->returnTypeString;
    e.argTypes = reg->argTypes;
    e.funcPtr = reg->funcPtr;
    e.devLevel = reg->devLevel;
    e.returnType = reg->returnType;
}

void SetSquirrelNativeDumpEnabled(bool enabled) { g_enabled = enabled; }
bool IsSquirrelNativeDumpEnabled() { return g_enabled; }
unsigned long long SquirrelNativeRegistrationCount() {
    return static_cast<unsigned long long>(g_count.load(std::memory_order_acquire));
}

void EnsureSquirrelNativeDumpInstalled(void* clientModule) {
    if (g_site || g_installAttempted) return;
    if (!g_enabled) {
        Tf2VrLog("[TF2VR] natives dump: not requested (set natives.dump = 1 in the INI); "
                 "no registration hook installed.\n");
        g_installAttempted = true;
        return;
    }
    if (!clientModule) return;
    g_installAttempted = true;

    auto* site = reinterpret_cast<std::uint8_t*>(clientModule) + kRegisterRva;
    if (std::memcmp(site, kExpected, kDisplaced) != 0) {
        char line[420]{};
        int used = std::snprintf(line, sizeof(line),
            "[TF2VR] natives dump: client.dll+0x%llX does not begin with the bytes it was resolved "
            "from, so this is not that build -- or something patched it first. NOT installed. Found:",
            static_cast<unsigned long long>(kRegisterRva));
        for (std::size_t i = 0; i < kDisplaced && used < static_cast<int>(sizeof(line)) - 8; ++i) {
            used += std::snprintf(line + used, sizeof(line) - used, " %02X", site[i]);
        }
        std::snprintf(line + used, sizeof(line) - used, "\n");
        Tf2VrLog(line);
        return;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(site, kDisplaced, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Tf2VrLog("[TF2VR] natives dump: VirtualProtect failed; nothing patched.\n");
        return;
    }
    std::memcpy(g_original, site, kDisplaced);

    // jmp qword ptr [rip+0], then the absolute target immediately after it.
    // Built from a ZEROED array: bytes 2..5 are the disp32 and must stay zero.
    // A previous hook in this project memset the patch to 0x90 first and left
    // disp32 = 0x90909090, which jumped through a wild address and took the
    // process down on the first call.
    std::uint8_t detour[kDisplaced]{};
    detour[0] = 0xFF;
    detour[1] = 0x25;
    const auto target = reinterpret_cast<std::uintptr_t>(&sqRegisterInterceptor);
    std::memcpy(detour + 6, &target, sizeof(target));
    if (kDisplaced > 14) std::memset(detour + 14, 0x90, kDisplaced - 14);

    std::uint32_t displacement = 0;
    std::memcpy(&displacement, detour + 2, sizeof(displacement));
    std::uintptr_t encoded = 0;
    std::memcpy(&encoded, detour + 6, sizeof(encoded));
    if (detour[0] != 0xFF || detour[1] != 0x25 || displacement != 0 || encoded != target) {
        char line[360]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] natives dump: refusing to patch -- the constructed detour is malformed "
            "(op %02X %02X, disp32 0x%08X, target 0x%llX). Nothing was written.\n",
            detour[0], detour[1], displacement, static_cast<unsigned long long>(encoded));
        Tf2VrLog(line);
        DWORD ignoredProtect = 0;
        VirtualProtect(site, kDisplaced, oldProtect, &ignoredProtect);
        return;
    }

    g_sqRegisterContinue = reinterpret_cast<std::uintptr_t>(site + kDisplaced);
    std::memcpy(site, detour, kDisplaced);
    FlushInstructionCache(GetCurrentProcess(), site, kDisplaced);
    DWORD ignored = 0;
    VirtualProtect(site, kDisplaced, oldProtect, &ignored);
    g_site = site;
    g_originalProtect = oldProtect;
    RegisterHookSite("squirrel-natives registration patched entry", site, kDisplaced);

    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] natives dump: registration hook installed at client.dll+0x%llX, resuming at +0x%llX.\n",
        static_cast<unsigned long long>(kRegisterRva),
        static_cast<unsigned long long>(kRegisterRva + kDisplaced));
    Tf2VrLog(line);
}

void RemoveSquirrelNativeDump() {
    if (!g_site) return;
    DWORD oldProtect = 0;
    if (VirtualProtect(g_site, kDisplaced, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::memcpy(g_site, g_original, kDisplaced);
        FlushInstructionCache(GetCurrentProcess(), g_site, kDisplaced);
        DWORD ignored = 0;
        VirtualProtect(g_site, kDisplaced, oldProtect, &ignored);
    }
    g_site = nullptr;
    g_sqRegisterContinue = 0;
}

void PollSquirrelNativeDump() {
    if (!g_site) return;
    const std::size_t count = g_count.load(std::memory_order_acquire);
    if (count == 0) return;
    if (count != g_lastSeenCount) {
        g_lastSeenCount = count;
        g_stableFrames = 0;
        return;
    }
    // Registrations arrive in one burst per VM. Half a second of no new ones
    // means that burst is over; writing earlier would produce a dump that is
    // short for a reason nothing in the file would record.
    if (g_stableFrames < 30 && ++g_stableFrames < 30) return;

    // RE-DUMPED ON GROWTH, NOT WRITTEN ONCE.
    //
    // There is more than one VM. The UI VM is built at startup and the CLIENT
    // VM at map load, and both register through this function -- so a one-shot
    // dump fires on whichever burst lands first and produces a file that looks
    // complete while holding half the table. Rewriting whenever the count has
    // grown means the file always reflects the fullest capture so far, and the
    // header's own count says which that was.
    if (count == g_dumpedCount) return;
    g_dumpedCount = count;
    WriteDump();
}
