#include "crash_recorder.h"

#include "xr_breadcrumb.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ads_lock.h"
#include "ads_probe.h"
#include "aim_cmd.h"
#include "aim_probe.h"
#include "arms_collapse.h"
#include "viewmodel_bones.h"
#include "camera_hook.h"
#include "camera_update_hook.h"
#include "crosshair_scale.h"
#include "diagnostics.h"
#include "hook_registry.h"
#include "menu_overlay.h"
#include "placement_pin.h"
#include "present_hook.h"
#include "rui_probe.h"
#include "vb_bind_census.h"
#include "vr_input.h"
#include "viewmodel_hide.h"
#include "weapon_settings.h"
#include "xinput_pad.h"
#include "xr_input.h"

namespace {

// ---------------------------------------------------------------------------
// THE ARM STATE, as one word.
//
// WHY A BITMASK AND NOT TWENTY CALLS FROM THE HANDLER. The handler runs on a
// thread that has just faulted, in a process that is about to die, and possibly
// while another of our threads holds a lock. Calling twenty getters there is a
// way to fault inside the fault and lose the line entirely. The tick asks the
// questions; the handler decodes a number against a table it does not have to
// call anything to read.
//
// EVERY GETTER IN THIS TABLE IS LOCK-FREE, and that was checked rather than
// assumed. `IsProjectionLayerArmed()` and `IsXrDecoupled()` are DELIBERATELY
// ABSENT: both take g_xrMutex, which the Present path holds, and neither a
// per-frame publish nor a crash handler may take a lock. Mirroring them into an
// atomic was considered and rejected -- settings_registry.h's rule that a second
// store is a second source of truth applies here too, and nothing in the titan
// crash suspects list needs them. The auto-arm ladder bits below say which stage
// the stack reached.
struct ArmBit {
    const char* name;
    bool (*read)();
};

const ArmBit kArmBits[] = {
    // The ladder.
    {"xr",                 [] { return IsXrArmed(); }},
    {"headlook",           [] { return IsHeadLookArmed(); }},
    {"headtracking",       [] { return IsHeadTrackingArmed(); }},
    {"headtracking.pos",   [] { return IsHeadPositionalTracking(); }},
    {"stereo",             [] { return IsAlternateFrameStereoArmed(); }},
    // PLAN-TITAN section 3's suspects, in the order it bisects them. These are
    // the reason this exists at all, so they are never the ones dropped when the
    // mask runs out of room.
    {"body.collapse",      [] { return IsArmsCollapseArmed(); }},
    {"placement.pin",      [] { return IsViewmodelPlacementPinArmed(); }},
    {"placement.pin.hand", [] { return IsPlacementPinHandDriven(); }},
    {"ads.probe",          [] { return IsAdsProbeEnabled(); }},
    {"ads.lock",           [] { return IsAdsLock(); }},
    {"weapon.sway.supp",   [] { return IsWeaponSwaySuppressed(); }},
    // The rest of the stack, alphabetically within its group.
    {"viewmodel.correct",  [] { return IsViewmodelCompensationEnabled(); }},
    {"viewmodel.hidden",   [] { return IsViewmodelHidden(); }},
    {"weapon.pin",         [] { return IsWeaponPinnedToController(); }},
    {"weapon.bonepin",     [] { return IsWeaponBonePinEnabled(); }},
    {"xr.declare_fov",     [] { return IsDeclareRenderedFovArmed(); }},
    {"xr.fit_horizontal",  [] { return IsFitHorizontalArmed(); }},
    {"xr.viewport_full",   [] { return IsViewportFullArmed(); }},
    {"vrinput",            [] { return IsVrInputEnabled(); }},
    {"xrinput",            [] { return IsControllerInputEnabled(); }},
    {"aim.probe",          [] { return IsAimProbeArmed(); }},
    {"pad.synthetic",      [] { return IsSyntheticPadEnabled(); }},
    {"reticle.hidden",     [] { return IsReticleHidden(); }},
    {"rui.census",         [] { return RuiCensusArmed(); }},
    {"rui.lowerleft",      [] { return RuiLowerLeftArmed(); }},
    {"menu.open",          [] { return IsMenuOpen(); }},
};
constexpr size_t kArmBitCount = sizeof(kArmBits) / sizeof(kArmBits[0]);
static_assert(kArmBitCount <= 32, "the published mask is 32 bits wide");

std::atomic<std::uint32_t> g_armMask{0};
// AimCmdState is a small enum rather than a flag, and it PATCHES CODE, so it is
// carried alongside the mask rather than squeezed into one bit of it.
std::atomic<int> g_aimCmdState{0};
// How many frames have published. A zero here in a crash line means the tick
// never ran -- the crash is during load or startup -- which is itself the
// answer to a different question, so it is printed rather than inferred.
std::atomic<std::uint64_t> g_publishCount{0};

std::atomic_bool g_selfTestArmed{false};
std::atomic_bool g_installed{false};
// At most this many records. NOT one: a first-chance handler also sees
// exceptions the game catches and carries on from, and if one of those spent the
// only slot, the fault that actually killed the process would go unrecorded --
// the exact evidence loss this module exists to end. Four is enough to keep the
// last one and cheap enough that a storm cannot flood the log.
constexpr int kMaxRecords = 4;
std::atomic<int> g_records{0};

LPTOP_LEVEL_EXCEPTION_FILTER g_previousUnhandledFilter = nullptr;

// Appends the arm-state list. Table walk and integer work only; nothing here
// allocates, locks, or calls into the plugin.
int FormatArmState(char* out, size_t size, int offset) {
    const std::uint32_t mask = g_armMask.load(std::memory_order_acquire);
    int written = offset;
    if (!mask) {
        written += std::snprintf(out + written, size - static_cast<size_t>(written),
                                 "NOTHING ARMED");
        return written;
    }
    bool first = true;
    for (size_t index = 0; index < kArmBitCount; ++index) {
        if (!(mask & (1u << index))) continue;
        if (static_cast<size_t>(written) + 32 >= size) break;
        written += std::snprintf(out + written, size - static_cast<size_t>(written), "%s%s",
                                 first ? "" : " ", kArmBits[index].name);
        first = false;
    }
    return written;
}

// Reads a mapped image's own name out of its export directory. Handler-safe:
// every dereference is inside one SEH guard, every offset is bounds-checked
// against the headers, and it copies at most `cap` bytes of ASCII. A module
// that has been unloaded still has its headers mapped, which is exactly the
// case this exists for.
void ReadImageExportName(const void* base, char* out, std::size_t cap) {
    if (!base || !out || cap == 0) return;
    out[0] = 0;
    __try {
        const auto* bytes = static_cast<const std::uint8_t*>(base);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x10000) return;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;
        if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return;
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) return;
        const auto* exports =
            reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(bytes + dir.VirtualAddress);
        if (!exports->Name) return;
        const char* name = reinterpret_cast<const char*>(bytes + exports->Name);
        std::size_t i = 0;
        for (; i + 1 < cap && name[i] >= 0x20 && name[i] < 0x7F; ++i) out[i] = name[i];
        out[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
    }
}

const char* ExceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE";
    default: return "fatal";
    }
}

// Only the codes that end a process. A first-chance handler sees everything --
// C++ throws (0xE06D7363), the debugger's thread-name notification (0x406D1388),
// the CLR's own traffic -- and recording those would bury the one line that
// matters under noise from code that was working correctly.
bool IsFatalCode(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        return true;
    default:
        return false;
    }
}

// MSVC C++ throw (0xE06D7363): name the exception's CLASS, because the address
// is useless -- it is always KERNELBASE!RaiseException. The 2026-08-29 VR run
// died as "fatal code 0xE06D7363 at KERNELBASE.dll+0xC187A" and the line could
// not say WHAT was thrown -- __non_rtti_object (the earlier heap corruption
// resurfacing), bad_alloc, or an engine error class are three different
// investigations.
//
// x64 layout, all documented by the CRT's own throw path: params are
// {0x19930520, object, ThrowInfo*, module base}; ThrowInfo holds an RVA to a
// CatchableTypeArray; its first entry holds an RVA to a CatchableType whose
// +0x4 is an RVA to the TypeDescriptor; the mangled name (".?AV...") starts at
// TypeDescriptor+0x10. Every pointer is VirtualQuery-checked before the read
// -- this runs inside a crash handler, and faulting inside the fault loses the
// whole line.
bool ReadableForRecord(const void* address, size_t bytes) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info)) return false;
    if (info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    return start + info.RegionSize - at >= bytes;
}

const char* CppExceptionTypeName(const EXCEPTION_RECORD* record) {
    if (!record || record->ExceptionCode != 0xE06D7363 || record->NumberParameters < 4) return nullptr;
    const auto base = static_cast<std::uintptr_t>(record->ExceptionInformation[3]);
    const auto throwInfo = reinterpret_cast<const std::uint32_t*>(record->ExceptionInformation[2]);
    if (!base || !ReadableForRecord(throwInfo, 16)) return nullptr;
    const std::uint32_t catchableArrayRva = throwInfo[3];
    if (!catchableArrayRva) return nullptr;
    const auto* catchableArray = reinterpret_cast<const std::uint32_t*>(base + catchableArrayRva);
    if (!ReadableForRecord(catchableArray, 8) || catchableArray[0] == 0) return nullptr;
    const auto* catchableType = reinterpret_cast<const std::uint32_t*>(base + catchableArray[1]);
    if (!ReadableForRecord(catchableType, 8)) return nullptr;
    const auto* descriptor = reinterpret_cast<const char*>(base + catchableType[1]);
    const char* name = descriptor + 0x10;
    if (!ReadableForRecord(name, 8)) return nullptr;
    // The name must look like a mangled type and terminate within bounds.
    if (name[0] != '.' && name[0] != '?') return nullptr;
    for (int i = 0; i < 120; ++i) {
        if (!ReadableForRecord(name + i, 1)) return nullptr;
        if (name[i] == '\0') return i > 0 ? name : nullptr;
    }
    return nullptr;
}

// ONE LINE. Faulting address, the module it lands in, the RVA inside it, and
// what was armed.
//
// THE RVA IS THE WHOLE POINT of resolving the module. An absolute address is
// useless across launches because client.dll is relocated; `client.dll+0x2A31C0`
// is the same place every time and can be fed straight to pescan.
// THE REGISTERS, AND WHAT THEY POINT AT (2026-09-06).
//
// The handler has always received EXCEPTION_POINTERS and passed on only the
// ExceptionRecord, throwing the ContextRecord away. That cost a diagnosis: the
// 09-06 titan-exit crash faulted at materialsystem_dx11+0x1DA66,
//
//     mov rax,[r9+r15+0x30]   ; rax = item+0x30, from the item array
//     mov rcx,[rdx]
//     mov rax,[rax+0x10]      ; <-- rax NULL, "READ from 0x10"
//
// so r9 held the item array, r15 the offset of the offending item and rdx its
// comparison partner. All three were in the context, and all three were
// discarded. The address alone said "a null item descriptor"; the registers
// would have said WHICH item, and let us read it.
//
// Everything here runs ONLY inside the exception handler, so it costs nothing
// until something has already gone wrong. Every read is bounded by VirtualQuery
// -- a crash handler that faults tells you nothing.
// Names an address as module+RVA, the same way the fault line already does.
void DescribeAddress(std::uintptr_t a, char* out, std::size_t cap) {
    HMODULE mod = nullptr;
    if (a && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(a), &mod) && mod) {
        char full[MAX_PATH] = "";
        GetModuleFileNameA(mod, full, sizeof(full));
        const char* leaf = std::strrchr(full, 92);
        std::snprintf(out, cap, "%s+0x%llX", leaf ? leaf + 1 : full,
                      static_cast<unsigned long long>(a - reinterpret_cast<std::uintptr_t>(mod)));
        return;
    }
    std::snprintf(out, cap, "%016llX (no module)", static_cast<unsigned long long>(a));
}
bool Readable(std::uintptr_t a, std::size_t n) { return ReadableForRecord(reinterpret_cast<const void*>(a), n); }

void WriteContext(const CONTEXT* ctx) {
    if (!ctx) {
        Tf2VrLogAlways("[TF2VR] CRASH context: NOT CAPTURED -- no ContextRecord on this route.\n");
        return;
    }
    char line[900];
    std::snprintf(line, sizeof(line),
                  "[TF2VR] CRASH regs: rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX rsi=%016llX rdi=%016llX\n"
                  "[TF2VR] CRASH regs: r8=%016llX r9=%016llX r10=%016llX r11=%016llX r12=%016llX r13=%016llX\n"
                  "[TF2VR] CRASH regs: r14=%016llX r15=%016llX rsp=%016llX rbp=%016llX rip=%016llX\n",
                  ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx, ctx->Rsi, ctx->Rdi, ctx->R8, ctx->R9, ctx->R10, ctx->R11,
                  ctx->R12, ctx->R13, ctx->R14, ctx->R15, ctx->Rsp, ctx->Rbp, ctx->Rip);
    Tf2VrLogAlways(line);
    // Peek at whatever the registers point to. A null field inside an otherwise
    // intact record reads very differently from a wholly bogus pointer, and
    // that difference is the whole question in a suspected race.
    const struct { const char* name; DWORD64 v; } regs[] = {
        {"rax", ctx->Rax}, {"rbx", ctx->Rbx}, {"rcx", ctx->Rcx}, {"rdx", ctx->Rdx},
        {"rsi", ctx->Rsi}, {"rdi", ctx->Rdi}, {"r8", ctx->R8},   {"r9", ctx->R9},
        {"r14", ctx->R14}, {"r15", ctx->R15},
    };
    for (const auto& r : regs) {
        const auto a = static_cast<std::uintptr_t>(r.v);
        if (!Readable(a, 32)) continue;
        const auto* q = reinterpret_cast<const std::uint64_t*>(a);
        std::snprintf(line, sizeof(line),
                      "[TF2VR] CRASH  [%s] = %016llX %016llX %016llX %016llX\n", r.name,
                      static_cast<unsigned long long>(q[0]), static_cast<unsigned long long>(q[1]),
                      static_cast<unsigned long long>(q[2]), static_cast<unsigned long long>(q[3]));
        Tf2VrLogAlways(line);
    }
    // The stack, symbolised. One address names a function; a chain names the
    // path that reached it, which is what tells a game-side fault apart from
    // one of ours.
    CONTEXT walk = *ctx;
    for (int i = 0; i < 24 && walk.Rip; ++i) {
        char where[96];
        DescribeAddress(static_cast<std::uintptr_t>(walk.Rip), where, sizeof(where));
        std::snprintf(line, sizeof(line), "[TF2VR] CRASH stack[%d] %s\n", i, where);
        Tf2VrLogAlways(line);
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(walk.Rip, &imageBase, nullptr);
        if (!fn) {
            if (!Readable(static_cast<std::uintptr_t>(walk.Rsp), 8)) break;
            walk.Rip = *reinterpret_cast<const DWORD64*>(walk.Rsp);
            walk.Rsp += 8;
            continue;
        }
        PVOID handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, walk.Rip, fn, &walk, &handlerData, &establisher, nullptr);
    }
    ReportGameWritesForCrash();
}

void WriteRecord(const char* route, const EXCEPTION_RECORD* record, const CONTEXT* ctx);

void WriteRecord(const char* route, const EXCEPTION_RECORD* record) { WriteRecord(route, record, nullptr); }

void WriteRecord(const char* route, const EXCEPTION_RECORD* record, const CONTEXT* ctx) {
    char module[MAX_PATH] = "unknown";
    std::uintptr_t rva = 0;
    void* address = record ? record->ExceptionAddress : nullptr;
    HMODULE owner = nullptr;
    if (address && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      static_cast<LPCSTR>(address), &owner) && owner) {
        char full[MAX_PATH]{};
        if (GetModuleFileNameA(owner, full, MAX_PATH)) {
            const char* leaf = std::strrchr(full, '\\');
            std::snprintf(module, sizeof(module), "%s", leaf ? leaf + 1 : full);
        }
        rva = reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(owner);
    }

    // P0-c: THE REGISTRY, CONSULTED BEFORE `unknown` IS PRINTED. Four sessions
    // ended at `unknown+0x0` because a trampoline's owner was logged at install
    // and aggregated nowhere. LookupHookSite is handler-safe by construction
    // (fixed array, no locks, no allocation).
    //
    //   PC in no module + registry hit -> the line names the hook instead of
    //     `unknown`, with the offset into ITS range.
    //   PC in a module + registry hit -> module+RVA stands (pescan needs it)
    //     and the patched-site ownership is appended after the arm state.
    const void* hookBase = nullptr;
    std::size_t hookLength = 0;
    const char* hookName = LookupHookSite(address, &hookBase, &hookLength);
    if (hookName && !owner) {
        std::snprintf(module, sizeof(module), "OUR HOOK '%s'", hookName);
        rva = reinterpret_cast<std::uintptr_t>(address) -
              reinterpret_cast<std::uintptr_t>(hookBase);
    }

    // WHEN THE MODULE LOOKUP FAILS, SAY WHAT THE MEMORY IS INSTEAD OF "unknown".
    //
    // Four sessions have now ended at 00007FFC86E260C0 with `unknown+0x0`, and
    // that string is why none of them named a cause. GetModuleHandleEx failing
    // is not a missing detail -- it is the FINDING: the faulting address is not
    // inside any loaded image, and the things that are executable and not an
    // image are detour trampolines, JIT and manually-mapped code. This plugin
    // installs a lot of the first kind, so "not in a module" points at us, and
    // "check our own history before blaming the game" says to look there first.
    //
    // VirtualQuery separates them. MEM_IMAGE means a module the handle lookup
    // somehow missed; MEM_PRIVATE with an executable protection is a trampoline
    // or JIT block. AllocationBase is also the thing to compare ACROSS runs --
    // an identical faulting address four times over is either a stable module
    // base or a trampoline allocated near a stable target, and this tells us
    // which without another session.
    //
    // Safe inside a handler: VirtualQuery takes no lock we could be holding and
    // allocates nothing.
    char region[220]{};
    if (address && !owner) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(address, &info, sizeof(info)) == sizeof(info)) {
            const char* type = info.Type == MEM_IMAGE   ? "MEM_IMAGE"
                             : info.Type == MEM_MAPPED  ? "MEM_MAPPED"
                             : info.Type == MEM_PRIVATE ? "MEM_PRIVATE" : "type?";
            const char* state = info.State == MEM_COMMIT  ? "COMMIT"
                              : info.State == MEM_RESERVE ? "RESERVE"
                              : info.State == MEM_FREE    ? "FREE" : "state?";
            const bool executable =
                (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                 PAGE_EXECUTE_WRITECOPY)) != 0;
            // NAME THE IMAGE FROM ITS OWN PE HEADER when the module list will
            // not. 2026-09-08: two crashes in a row landed at the same address
            // in MEM_IMAGE memory that GetModuleHandle-style enumeration could
            // not name -- which is what an UNLOADED-but-still-mapped DLL looks
            // like, and also what a driver DLL the enumeration skipped looks
            // like. Those are very different answers and the record has to
            // distinguish them, so read the export directory's own name string
            // at the allocation base. Everything is bounds-checked and SEH-
            // guarded: this runs inside a fault handler and must not fault.
            char pename[96] = "";
            if (info.Type == MEM_IMAGE && info.AllocationBase) {
                ReadImageExportName(info.AllocationBase, pename, sizeof(pename));
            }
            std::snprintf(region, sizeof(region),
                " | NOT IN ANY MODULE: %s %s protect 0x%lX%s, alloc base %p, region 0x%llX.%s%s%s "
                "MEM_IMAGE means a mapped image file, NOT the private memory a detour trampoline "
                "would live in -- so this is not one of ours unless it is named as one.",
                type, state, static_cast<unsigned long>(info.Protect),
                executable ? " (EXECUTABLE)" : " (not executable -- a jump through a bad pointer)",
                info.AllocationBase, static_cast<unsigned long long>(info.RegionSize),
                pename[0] ? " PE says the image is '" : "",
                pename[0] ? pename : "",
                pename[0] ? "' (unloaded, or missed by the module walk)." : "");
        } else {
            std::snprintf(region, sizeof(region),
                " | NOT IN ANY MODULE and VirtualQuery refused it: the address is not even "
                "reserved, so control was transferred through a corrupt pointer rather than "
                "faulting inside real code.");
        }
    }

    // Widened for the not-in-any-module detail below: the arm list alone can run
    // past a kilobyte, and a record that dropped the one new field would leave
    // the next reader exactly where the last four left them.
    char line[1800]{};
    int written = std::snprintf(line, sizeof(line),
        "[TF2VR] ***** CRASH (%s): %s code 0x%08lX at %p = %s+0x%llX | thread %lu | frames "
        "published %llu | aim.cmd %d | ARMED: ",
        route, ExceptionName(record ? record->ExceptionCode : 0),
        record ? record->ExceptionCode : 0, address, module,
        static_cast<unsigned long long>(rva), GetCurrentThreadId(),
        static_cast<unsigned long long>(g_publishCount.load(std::memory_order_relaxed)),
        g_aimCmdState.load(std::memory_order_relaxed));
    if (written < 0) return;
    written = FormatArmState(line, sizeof(line) - 2, written);
    // An access violation carries which address it touched and whether it was a
    // read or a write, and that pair is what separates "our write ran off an
    // entity" from "we read a field that is not there on a titan".
    if (record && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        record->NumberParameters >= 2 && static_cast<size_t>(written) + 80 < sizeof(line)) {
        const ULONG_PTR operation = record->ExceptionInformation[0];
        written += std::snprintf(line + written, sizeof(line) - static_cast<size_t>(written),
            " | %s 0x%llX",
            operation == 0 ? "READ from" : operation == 1 ? "WROTE to" : "EXECUTED",
            static_cast<unsigned long long>(record->ExceptionInformation[1]));
    }
    // WHAT WE WERE DOING IN THE XR SUBMIT PATH, and which textures the runtime
    // is holding. Added 2026-09-08 after a crash whose stack was entirely
    // d3d11 + Virtual Desktop with none of our frames on it -- which is what a
    // stale resource we handed over looks like, so "not on the stack" had to
    // stop being read as "not us". See diag/xr_breadcrumb.h.
    {
        char crumb[400]{};
        XrBreadcrumb::Describe(crumb, sizeof(crumb));
        if (crumb[0] && static_cast<size_t>(written) + std::strlen(crumb) + 12 < sizeof(line)) {
            written += std::snprintf(line + written, sizeof(line) - static_cast<size_t>(written),
                " | XR: %s", crumb);
        }
    }
    // The thrown class, when this is a C++ throw. "__non_rtti_object" here
    // means the RTTI walk faulted on a corrupt object and the CRT converted it
    // -- the same heap corruption as an earlier AV record, resurfacing.
    if (const char* thrownType = CppExceptionTypeName(record)) {
        if (static_cast<size_t>(written) + std::strlen(thrownType) + 32 < sizeof(line)) {
            written += std::snprintf(line + written, sizeof(line) - static_cast<size_t>(written),
                " | C++ EXCEPTION of type '%s'", thrownType);
        }
    }
    // A fault at a patched site is OURS even though the module is the game's:
    // the bytes at that RVA are the detour this plugin wrote.
    if (hookName && owner && static_cast<size_t>(written) + 96 < sizeof(line)) {
        written += std::snprintf(line + written, sizeof(line) - static_cast<size_t>(written),
            " | PC IS IN BYTES WE PATCHED: '%s'", hookName);
    }
    // Appended last so a truncated line still carries the address, the arm state
    // and the faulting operation, which are the parts every past record has had.
    if (region[0] && static_cast<size_t>(written) + std::strlen(region) + 2 < sizeof(line)) {
        written += std::snprintf(line + written, sizeof(line) - static_cast<size_t>(written),
                                 "%s", region);
    }
    if (static_cast<size_t>(written) + 2 < sizeof(line)) {
        line[written] = '\n';
        line[written + 1] = '\0';
    }
    Tf2VrLogAlways(line);
    WriteContext(ctx);
}

// Registered LAST, so it sits at the FRONT of the vectored chain -- ahead of
// Northstar's, which writes a minidump and can end the process before any
// unhandled-exception filter is reached.
//
// It ALWAYS returns EXCEPTION_CONTINUE_SEARCH. Nothing about how this fault is
// handled changes; we only write it down on the way past.
LONG CALLBACK VectoredRecorder(EXCEPTION_POINTERS* pointers) {
    if (!pointers || !pointers->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (!IsFatalCode(pointers->ExceptionRecord->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    if (g_records.fetch_add(1, std::memory_order_acq_rel) >= kMaxRecords) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    WriteRecord("first-chance", pointers->ExceptionRecord, pointers->ContextRecord);
    // The buffer must not outlive the process with the record still in it.
    Tf2VrLogFlush();
    return EXCEPTION_CONTINUE_SEARCH;
}

// The plan's own route, kept as the second one and chaining exactly as asked.
// If this ever fires, the line says so and we learn that the vectored route was
// not the live one after all.
LONG WINAPI UnhandledRecorder(EXCEPTION_POINTERS* pointers) {
    if (pointers && pointers->ExceptionRecord &&
        g_records.fetch_add(1, std::memory_order_acq_rel) < kMaxRecords) {
        WriteRecord("unhandled-filter", pointers->ExceptionRecord, pointers->ContextRecord);
        // THE SECOND CRASH ROUTE MUST FLUSH TOO. Only the vectored handler did,
        // so a fault that reached this filter instead left its own record
        // sitting in the log buffer and died with the process.
        Tf2VrLogFlush();
    }
    if (g_previousUnhandledFilter) return g_previousUnhandledFilter(pointers);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallCrashRecorder() {
    if (g_installed.exchange(true, std::memory_order_acq_rel)) return;
    const PVOID vectored = AddVectoredExceptionHandler(1, VectoredRecorder);
    g_previousUnhandledFilter = SetUnhandledExceptionFilter(UnhandledRecorder);
    char line[640]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] crash recorder installed: vectored handler %s (front of the chain, ahead of "
        "Northstar's), unhandled-exception filter chaining to %s. It records %zu arm bits and "
        "returns CONTINUE_SEARCH, so nothing about how a fault is handled changes. Up to %d "
        "records per session. It installs BEFORE the ini is parsed, so whether the deliberate-fault "
        "key is armed is decided by the crash.selftest line that follows this one; no line means "
        "off.\n",
        vectored ? "REGISTERED" : "FAILED TO REGISTER",
        g_previousUnhandledFilter ? "the previous filter" : "the system default",
        kArmBitCount, kMaxRecords);
    Tf2VrLogAlways(line);
}

void PublishArmStateForCrashRecorder() {
    std::uint32_t mask = 0;
    for (size_t index = 0; index < kArmBitCount; ++index) {
        if (kArmBits[index].read()) mask |= (1u << index);
    }
    g_armMask.store(mask, std::memory_order_release);
    g_aimCmdState.store(static_cast<int>(CurrentAimCmdState()), std::memory_order_release);
    g_publishCount.fetch_add(1, std::memory_order_relaxed);
}

void SetCrashSelfTestArmed(bool armed) {
    g_selfTestArmed.store(armed, std::memory_order_release);
    Tf2VrLogAlways(armed
        ? "[TF2VR] crash.selftest = 1: PRINT SCREEN will DELIBERATELY CRASH THE GAME to prove the "
          "recorder works. Expect one CRASH line naming this module, and titanfall2vr.prev-1.log "
          "to hold the previous session. Set it back to 0 before any real run.\n"
        : "[TF2VR] crash.selftest = 0: the deliberate-fault key is inert.\n");
}

bool TriggerCrashRecorderSelfTest() {
    if (!g_selfTestArmed.load(std::memory_order_acquire)) {
        Tf2VrLogAlways("[TF2VR] crash self-test IGNORED: crash.selftest is 0. This key ends the process, "
                 "so it stays inert until the ini asks for it.\n");
        return false;
    }
    // Said BEFORE the fault, so the log distinguishes "the recorder caught a
    // deliberate fault" from "the recorder caught a real one". The log is
    // unbuffered, so this line is on disk before the next statement runs.
    Tf2VrLogAlways("[TF2VR] crash self-test: faulting ON PURPOSE now. The CRASH line that follows is the "
             "falsifier for P0-b; this module is the expected faulting module.\n");
    // A null write rather than __debugbreak or a division: it produces
    // EXCEPTION_ACCESS_VIOLATION with the write flag and a target address of 0,
    // which is the same shape as the fault class we are actually hunting, so the
    // self-test exercises the parameter decoding too and not just the plumbing.
    volatile int* deliberateNull = nullptr;
    *deliberateNull = 1;
    return true;
}
