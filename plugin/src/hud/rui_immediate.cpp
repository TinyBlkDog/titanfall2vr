#include "rui_immediate.h"
#include "rui_hunt.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "diagnostics.h"
#include "hook_registry.h"


// ---------------------------------------------------------------------------
// Addresses. Every one of these was read out of the shipped binaries with
// tools/pescan (client.dll selftest 0.9872% bad, engine.dll 1.4363%,
// ui(11).dll 1.1127%), and each is checked at runtime before it is used.
// ---------------------------------------------------------------------------
namespace {

// The client's copy of the engine RUI interface table. The engine builds the
// master at engine+0x54B40; slot 0x18 is open-context (engine+0xF92B0), slot
// 0x20 is submit (engine+0xFC6E0) and slot 0x28 is the immediate draw
// (engine+0xFC960). The client calls through `call [rip]{C3DA28}` from three
// sites, one of them the HUD driver at client+0x3723B0 by way of 0x309730.
constexpr std::uintptr_t kClientTableRva = 0xC3DA00;
constexpr std::uintptr_t kImmediateSlot = 0x28;
constexpr std::uintptr_t kSubmitSlot = 0x20;
constexpr std::uintptr_t kOpenContextSlot = 0x18;

constexpr std::uintptr_t kFc960Rva = 0xFC960;   // immediate draw, calls descriptor +0x70
constexpr std::uintptr_t kFc6E0Rva = 0xFC6E0;   // submit, the path that ends in FC500
constexpr std::uintptr_t kF92B0Rva = 0xF92B0;   // open context

// The descriptors this project has logged all sit in engine.dll's own .data
// between 0x12A15550 and 0x12A25810. That range is deliberately NOT used as a
// scan window -- see the note above ScanPool. It is recorded here only because
// it is the answer to "where do the ones we already know about live".
//
// The two descriptor slots that hold a widget's draw function.
constexpr std::uintptr_t kDraw68 = 0x68;
constexpr std::uintptr_t kDraw70 = 0x70;
// Coordinate space, where the FC500 census already reads it.
constexpr std::uintptr_t kSpaceW = 0x18;
constexpr std::uintptr_t kSpaceH = 0x1C;

// ---- the watchlist -------------------------------------------------------
//
// Named from ui(11)'s own .rdata. Each asset string is referenced from exactly
// one function, so these are attributions and not guesses.
struct Watch {
    unsigned rva;
    const char* what;
    volatile long seen;
    unsigned long long tick;
    char how[72];
};

// 0x1E600 IS THE PRIME SUSPECT, and the wearer's flat screenshot is why.
// It is compiled from content\r2\ui\hud\smart_core.rui and it draws all three
// of smart_ammo_corner (the rounded-corner square), smart_ammo_lock_image (the
// orange ring that tags each painted target) and smart_ammo_measure. The square
// and the target rings being ELEMENTS OF ONE WIDGET is exactly the shape of the
// defect: the widget is placed at the aim, and the rings are laid out inside
// it, so in VR they follow the controller instead of staying on the targets.
Watch g_watch[] = {
    {0x1E600, "smart_core.rui -- THE SQUARE *AND* THE TARGET RINGS, one widget", 0, 0, {}},
    {0x7E190, "smart_ammo_bracket -- the other lock style", 0, 0, {}},
    {0x908D0, "smart_ammo_skull", 0, 0, {}},
    {0x5B800, "lockon_indicator centre+edge", 0, 0, {}},
};
constexpr int kWatchCount = sizeof(g_watch) / sizeof(g_watch[0]);

// ---- state ---------------------------------------------------------------
bool g_wanted = false;
volatile long g_installed = 0;       // 1 once the slot carries our function
bool g_installTried = false;
bool g_installRefused = false;
char g_installNote[420]{};

using Fc960Fn = void*(__fastcall*)(void*, void*);
Fc960Fn g_original = nullptr;
void** g_slot = nullptr;

std::uintptr_t g_engineBase = 0;
std::uintptr_t g_clientBase = 0;
std::uintptr_t g_uiBase = 0;
std::uintptr_t g_uiSize = 0;

// The FC500 census hands us the first descriptor it sees. The pool scan is
// disqualified out loud if it cannot find that exact address.
volatile long long g_controlDescriptor = 0;

// ---- counters. EVERY early return has one, and all of them print. ---------
volatile long long g_immCalls = 0;        // total calls through the claimed slot
volatile long long g_immNullLayer = 0;
volatile long long g_immNullDescriptor = 0;
volatile long long g_immFaults = 0;
volatile long long g_immDropped = 0;      // identities lost to a full table
volatile long long g_fc500Noted = 0;      // new FC500 identities handed to us
volatile long long g_fc500With70 = 0;     // ... of which carry a +0x70 draw fn
volatile long long g_fc500Differ = 0;     // ... of which +0x70 differs from +0x68

// ---- the immediate-path census -------------------------------------------
struct ImmIdentity {
    std::uintptr_t descriptor;
    std::uintptr_t draw68;
    std::uintptr_t draw70;
    float w;
    float h;
    volatile long long calls;
};
constexpr int kMaxImm = 96;
ImmIdentity g_imm[kMaxImm]{};
volatile long g_immReserved = 0;

// ---- the pool scan -------------------------------------------------------
struct PoolHit {
    unsigned targetRva;        // resolved into ui(11)
    std::uintptr_t firstAt;    // the address of the first qword pointing at it
    long count;
};
constexpr int kMaxPool = 384;
PoolHit g_pool[kMaxPool]{};
int g_poolCount = 0;
long long g_poolScanned = 0;
long long g_poolHits = 0;
long long g_poolDropped = 0;
bool g_poolRan = false;
bool g_poolFaulted = false;
bool g_poolControlFound = false;
bool g_poolControlVisited = false;
bool g_poolControlInRange = false;
bool g_poolCapped = false;
long long g_poolBlocksSkipped = 0;
long long g_lightSweeps = 0;
long long g_lightQwords = 0;
long long g_lightFaults = 0;
std::uintptr_t g_poolStart = 0;
std::uintptr_t g_poolEnd = 0;

unsigned long long g_lastReportTick = 0;
long g_lastRowsPrinted = 0;
int g_markCount = 0;

// ---- helpers -------------------------------------------------------------

void Describe(std::uintptr_t address, char* out, std::size_t cap) {
    if (!address) {
        std::snprintf(out, cap, "null");
        return;
    }
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &mod) &&
        mod) {
        char path[MAX_PATH]{};
        GetModuleFileNameA(mod, path, sizeof(path));
        const char* name = std::strrchr(path, '\\');
        name = name ? name + 1 : path;
        std::snprintf(out, cap, "%s+0x%llX", name,
                      static_cast<unsigned long long>(address -
                                                      reinterpret_cast<std::uintptr_t>(mod)));
        return;
    }
    std::snprintf(out, cap, "0x%llX", static_cast<unsigned long long>(address));
}

bool InUi(std::uintptr_t address) {
    if (!g_uiBase || !g_uiSize) return false;
    if (address <= g_uiBase) return false;
    if (address - g_uiBase >= g_uiSize) return false;
    // Every widget draw function this project has resolved is 16-byte aligned.
    // Requiring it costs nothing and keeps unrelated pointers out of the pool
    // report, which is the difference between a table you can read and noise.
    return (address & 0xF) == 0;
}

void MarkWatch(std::uintptr_t target, const char* how) {
    if (!target || !g_uiBase) return;
    const std::uintptr_t rva = target - g_uiBase;
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_watch[i].rva != rva) continue;
        if (InterlockedCompareExchange(&g_watch[i].seen, 1, 0) != 0) return;
        g_watch[i].tick = GetTickCount64();
        std::snprintf(g_watch[i].how, sizeof(g_watch[i].how), "%s", how);
        char line[520]{};
        std::snprintf(line, sizeof(line),
                      "[TF2VR] *** WATCHLIST HIT: ui(11)+0x%X is %s. Found by %s. This is the "
                      "first time this project has seen it. ***\n",
                      g_watch[i].rva, g_watch[i].what, how);
        Tf2VrLog(line);
        return;
    }
}

// Reads both draw slots of a descriptor and runs the watchlist over them.
// Faults are counted rather than thrown: a descriptor can be torn down between
// the draw that named it and this read.
bool ReadDescriptor(std::uintptr_t descriptor, std::uintptr_t& draw68, std::uintptr_t& draw70,
                    float& w, float& h) {
    draw68 = 0;
    draw70 = 0;
    w = 0.0f;
    h = 0.0f;
    __try {
        draw68 = *reinterpret_cast<const std::uintptr_t*>(descriptor + kDraw68);
        draw70 = *reinterpret_cast<const std::uintptr_t*>(descriptor + kDraw70);
        w = *reinterpret_cast<const float*>(descriptor + kSpaceW);
        h = *reinterpret_cast<const float*>(descriptor + kSpaceH);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_immFaults));
        return false;
    }
}

// ---- the hook ------------------------------------------------------------
//
// FC960(rcx, rdx). rdx is the LAYER: the function's own first act is
// `mov r10,[rdx]` to get the descriptor and `cmp qword [r10+0x70], 0` to test
// the draw slot, which is exactly what is read here. rcx is one dereference
// deeper than FC500's context (FC500 does `mov rcx,[rdi]` before the same
// hashing work), so no layer-type byte is read from it and none is reported.
// Guessing at that byte is how an instrument comes back with a confident wrong
// answer.
//
// The forward is unconditional. Every path through this function ends in the
// engine's own routine with the arguments it was given.
void* __fastcall ImmediateHook(void* a, void* layer) {
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_immCalls));
    if (!layer) {
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_immNullLayer));
        return g_original ? g_original(a, layer) : nullptr;
    }
    std::uintptr_t descriptor = 0;
    __try {
        descriptor = *reinterpret_cast<const std::uintptr_t*>(layer);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_immFaults));
        descriptor = 0;
    }
    if (!descriptor) {
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_immNullDescriptor));
        return g_original ? g_original(a, layer) : nullptr;
    }

    // Before EVERY cached-row return, with identity read from the slot FC960
    // actually calls. FC960's null +70 path returns without invoking a widget;
    // client+3097E7 overwrites RAX, so the caller does not consume its value.
    std::uintptr_t live70 = 0;
    __try {
        live70 = *reinterpret_cast<const std::uintptr_t*>(descriptor + kDraw70);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_immFaults));
    }
    if (RuiHuntObserve(1, live70)) return nullptr;

    // Find or claim. Same shape as the FC500 census: the slot is claimed with
    // an interlocked increment and the descriptor is stored LAST, so a row that
    // is still in flight reads as empty and is skipped rather than printed
    // half-built.
    long taken = g_immReserved;
    if (taken > kMaxImm) taken = kMaxImm;
    for (long i = 0; i < taken; ++i) {
        if (g_imm[i].descriptor == descriptor) {
            InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_imm[i].calls));
            return g_original ? g_original(a, layer) : nullptr;
        }
    }
    const long slot = InterlockedIncrement(&g_immReserved) - 1;
    if (slot >= kMaxImm) {
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_immDropped));
        return g_original ? g_original(a, layer) : nullptr;
    }
    std::uintptr_t draw68 = 0, draw70 = 0;
    float w = 0.0f, h = 0.0f;
    ReadDescriptor(descriptor, draw68, draw70, w, h);
    g_imm[slot].draw68 = draw68;
    g_imm[slot].draw70 = draw70;
    g_imm[slot].w = w;
    g_imm[slot].h = h;
    g_imm[slot].calls = 1;
    _ReadWriteBarrier();
    g_imm[slot].descriptor = descriptor;

    char where68[72]{}, where70[72]{};
    Describe(draw68, where68, sizeof(where68));
    Describe(draw70, where70, sizeof(where70));
    char line[560]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR] IMMEDIATE NEW #%ld t=%llu desc=0x%llX space %.1fx%.1f | +0x68 %s | "
                  "+0x70 %s\n",
                  slot + 1, static_cast<unsigned long long>(GetTickCount64()),
                  static_cast<unsigned long long>(descriptor), w, h, where68, where70);
    Tf2VrLog(line);
    MarkWatch(draw70, "the IMMEDIATE path (engine+0xFC960, descriptor slot +0x70)");
    MarkWatch(draw68, "the IMMEDIATE path (engine+0xFC960, descriptor slot +0x68)");
    return g_original ? g_original(a, layer) : nullptr;
}

// ---- install -------------------------------------------------------------

void ResolveModules() {
    if (!g_engineBase) {
        if (HMODULE m = GetModuleHandleA("engine.dll")) {
            g_engineBase = reinterpret_cast<std::uintptr_t>(m);
        }
    }
    if (!g_clientBase) {
        if (HMODULE m = GetModuleHandleA("client.dll")) {
            g_clientBase = reinterpret_cast<std::uintptr_t>(m);
        }
    }
    if (!g_uiBase) {
        if (HMODULE m = GetModuleHandleA("ui(11).dll")) {
            g_uiBase = reinterpret_cast<std::uintptr_t>(m);
            auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
            auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                reinterpret_cast<const std::uint8_t*>(m) + dos->e_lfanew);
            g_uiSize = nt->OptionalHeader.SizeOfImage;
        }
    }
}

void TryInstall() {
    if (g_installed || g_installRefused) return;
    ResolveModules();
    if (!g_engineBase || !g_clientBase) return;
    // ui(11) FIRST, DELIBERATELY. The watchlist resolves a draw function by
    // subtracting ui(11)'s base, so claiming the slot before that module is up
    // would let the first descriptors through with no name attached and no
    // second chance -- each one is processed exactly once. Waiting costs
    // nothing: ui(11) is loaded long before a world is.
    if (!g_uiBase) return;
    g_installTried = true;

    auto** table = reinterpret_cast<void**>(g_clientBase + kClientTableRva);
    void* immediate = nullptr;
    void* submit = nullptr;
    void* open = nullptr;
    __try {
        immediate = *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(table) + kImmediateSlot);
        submit = *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(table) + kSubmitSlot);
        open = *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(table) + kOpenContextSlot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_installRefused = true;
        std::snprintf(g_installNote, sizeof(g_installNote),
                      "REFUSED: reading client.dll+0x%llX faulted, so the interface table is not "
                      "there. Nothing was written.",
                      static_cast<unsigned long long>(kClientTableRva));
        Tf2VrLog("[TF2VR] IMMEDIATE: REFUSED, the client RUI interface table could not be read. "
                 "Nothing was written.\n");
        return;
    }

    // THE TABLE IS IDENTIFIED BY TWO SLOTS WE ARE NOT TOUCHING, not by the one
    // we are. If open-context and submit are where the engine's own builder put
    // them, this is that table; if they are not, the RVA belongs to a different
    // build and writing to it would corrupt whatever is really there.
    const bool openOk = open == reinterpret_cast<void*>(g_engineBase + kF92B0Rva);
    const bool submitOk = submit == reinterpret_cast<void*>(g_engineBase + kFc6E0Rva);
    const bool immediateOk = immediate == reinterpret_cast<void*>(g_engineBase + kFc960Rva);
    if (!openOk || !submitOk || !immediateOk) {
        char o[72]{}, s[72]{}, i[72]{};
        Describe(reinterpret_cast<std::uintptr_t>(open), o, sizeof(o));
        Describe(reinterpret_cast<std::uintptr_t>(submit), s, sizeof(s));
        Describe(reinterpret_cast<std::uintptr_t>(immediate), i, sizeof(i));
        g_installRefused = true;
        std::snprintf(g_installNote, sizeof(g_installNote),
                      "REFUSED: table slots read open=%s submit=%s immediate=%s", o, s, i);
        char line[900]{};
        std::snprintf(line, sizeof(line),
                      "[TF2VR] IMMEDIATE: REFUSING to claim the slot. client.dll+0x%llX should "
                      "hold open=engine+0xF92B0 submit=engine+0xFC6E0 immediate=engine+0xFC960; it "
                      "reads open=%s submit=%s immediate=%s. Nothing was written.\n",
                      static_cast<unsigned long long>(kClientTableRva), o, s, i);
        Tf2VrLog(line);
        return;
    }

    auto* slot = reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(table) + kImmediateSlot);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        g_installRefused = true;
        std::snprintf(g_installNote, sizeof(g_installNote),
                      "REFUSED: VirtualProtect failed on the slot.");
        Tf2VrLog("[TF2VR] IMMEDIATE: VirtualProtect failed on the interface slot. Nothing was "
                 "written.\n");
        return;
    }
    g_original = reinterpret_cast<Fc960Fn>(immediate);
    g_slot = slot;
    *slot = reinterpret_cast<void*>(&ImmediateHook);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    RegisterHookSite("RUI immediate interface slot", slot, sizeof(void*));
    InterlockedExchange(&g_installed, 1);
    std::snprintf(g_installNote, sizeof(g_installNote), "claimed at client.dll+0x%llX",
                  static_cast<unsigned long long>(kClientTableRva + kImmediateSlot));

    char line[1100]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR] IMMEDIATE PATH CLAIMED. client.dll+0x%llX slot 0x28 held "
                  "engine+0x%llX (the immediate widget draw, which calls the descriptor's +0x70) "
                  "and now holds our forwarder. Identified by two slots we do not touch: "
                  "open-context and submit both read where engine+0x54B40 puts them. READ-ONLY: "
                  "every call is forwarded to the engine unchanged. If this path is what draws the "
                  "missile lock, the widget appears below as an IMMEDIATE NEW line.\n",
                  static_cast<unsigned long long>(kClientTableRva + kImmediateSlot),
                  static_cast<unsigned long long>(kFc960Rva));
    Tf2VrLog(line);
}

// The client's table is a COPY. If anything rebuilds it the claim is silently
// lost, which would look exactly like a path that never fires. Checked on the
// report clock and reclaimed, with a counter.
volatile long long g_reclaims = 0;

void ReclaimIfLost() {
    if (!g_installed || !g_slot) return;
    __try {
        if (*g_slot == reinterpret_cast<void*>(&ImmediateHook)) return;
        // Something put the engine's own pointer back. Take it again.
        void* now = *g_slot;
        if (now != reinterpret_cast<void*>(g_engineBase + kFc960Rva)) return;
        DWORD old = 0;
        if (!VirtualProtect(g_slot, sizeof(void*), PAGE_READWRITE, &old)) return;
        *g_slot = reinterpret_cast<void*>(&ImmediateHook);
        DWORD ignored = 0;
        VirtualProtect(g_slot, sizeof(void*), old, &ignored);
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_reclaims));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ---- the pool scan -------------------------------------------------------
//
// This is the instrument that does not care whether the FC960 theory is right.
// It walks engine.dll's descriptor pool and lists every widget the game holds a
// descriptor for, by reading the two draw slots directly. If a descriptor for
// the bracket exists, it is here whether or not anything ever draws it; if none
// exists, the bracket is not an RUI widget in this build and the whole widget
// layer is closed for it.

void PoolNote(std::uintptr_t target, std::uintptr_t whereFound) {
    const unsigned rva = static_cast<unsigned>(target - g_uiBase);
    for (int i = 0; i < g_poolCount; ++i) {
        if (g_pool[i].targetRva == rva) {
            ++g_pool[i].count;
            return;
        }
    }
    if (g_poolCount >= kMaxPool) {
        ++g_poolDropped;
        return;
    }
    g_pool[g_poolCount].targetRva = rva;
    g_pool[g_poolCount].firstAt = whereFound;
    g_pool[g_poolCount].count = 1;
    ++g_poolCount;
}

// THE WINDOW WAS AN ASSUMPTION, AND ASSUMPTIONS ARE HOW A NULL BECOMES A LIE.
//
// The first version of this scanned engine+0x12A00000..0x12A40000, because
// every descriptor this project has ever logged sits inside it. That is a fact
// about the descriptors we have SEEN, and this instrument exists precisely to
// find one we have not. A widget whose descriptor sat outside that window would
// have come back as a confident absence.
//
// So the window is gone. This walks every readable region of engine.dll's whole
// image and looks at every aligned qword for a pointer into ui(11). A pointer
// to a widget's draw function means the game holds a reference to that widget,
// wherever the structure around it happens to live. The bound is now the
// module, which is not a guess, and the qword cap below is reported out loud if
// it is ever reached.
constexpr long long kMaxQwords = 24000000;   // ~192 MB of engine data per scan

// One region-walked sweep of [start, end), skipping anything inside
// [skipStart, skipEnd) so the priority pass is not counted twice.
void ScanRange(std::uintptr_t start, std::uintptr_t end, std::uintptr_t skipStart,
               std::uintptr_t skipEnd) {
    const std::uintptr_t control = static_cast<std::uintptr_t>(g_controlDescriptor);
    std::uintptr_t block = start;
    while (block < end) {
        if (g_poolScanned >= kMaxQwords) { g_poolCapped = true; return; }
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(block), &mbi, sizeof(mbi)) != sizeof(mbi)) {
            ++g_poolBlocksSkipped;
            block += 0x1000;
            continue;
        }
        std::uintptr_t regionEnd =
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= block) regionEnd = block + 0x1000;   // never fail to advance
        if (regionEnd > end) regionEnd = end;
        const DWORD prot = mbi.Protect & 0xFF;
        const bool readable =
            (mbi.State == MEM_COMMIT) && !(mbi.Protect & PAGE_GUARD) &&
            (prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
             prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
             prot == PAGE_EXECUTE_WRITECOPY);
        if (!readable) {
            ++g_poolBlocksSkipped;
            block = regionEnd;
            continue;
        }
        const std::uintptr_t blockEnd = regionEnd;
        __try {
            for (std::uintptr_t p = block; p + 8 <= blockEnd; p += 8) {
                if (g_poolScanned >= kMaxQwords) { g_poolCapped = true; break; }
                if (p >= skipStart && p < skipEnd) continue;
                ++g_poolScanned;
                if (p == control) g_poolControlVisited = true;
                const auto value = *reinterpret_cast<const std::uintptr_t*>(p);
                if (!InUi(value)) continue;
                ++g_poolHits;
                // THE WATCHLIST IS UNCONDITIONAL AND COMES FIRST. It runs before
                // the row table is consulted, so a full table can never be the
                // reason the bracket goes unreported -- which is the one failure
                // this whole instrument exists to avoid.
                MarkWatch(value, "the ENGINE DATA SCAN (a live pointer to it exists)");
                // The control passes when the descriptor the live census is
                // using is seen to carry a ui(11) pointer at either draw slot.
                if (control && (p == control + kDraw68 || p == control + kDraw70)) {
                    g_poolControlFound = true;
                }
                PoolNote(value, p);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_poolFaulted = true;
        }
        block = regionEnd;
    }
}

// THE LOCK UI ONLY EXISTS WHILE THE TRIGGER IS HELD.
//
// The wearer's own description: the circles appear as targets are painted with
// RB down and vanish the instant it is released. A descriptor created on demand
// for that HUD therefore does not exist when a level loads, so a scan that runs
// once at world entry can miss it completely and report a confident absence.
//
// This is the cheap continuous sweep that closes that hole: the 2 MB where
// every descriptor this project has ever seen lives, walked every five seconds,
// running the watchlist and nothing else. It touches none of the report tables,
// so it cannot disturb the full scan's numbers. About a quarter of a million
// qwords, which is a fraction of a millisecond.
void LightSweep() {
    if (!g_engineBase || !g_uiBase) return;
    const std::uintptr_t start = g_engineBase + 0x12900000;
    const std::uintptr_t end = g_engineBase + 0x12B00000;
    ++g_lightSweeps;
    std::uintptr_t block = start;
    while (block < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(block), &mbi, sizeof(mbi)) != sizeof(mbi)) {
            block += 0x1000;
            continue;
        }
        std::uintptr_t regionEnd =
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= block) regionEnd = block + 0x1000;
        if (regionEnd > end) regionEnd = end;
        const DWORD prot = mbi.Protect & 0xFF;
        const bool readable =
            (mbi.State == MEM_COMMIT) && !(mbi.Protect & PAGE_GUARD) &&
            (prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
             prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
             prot == PAGE_EXECUTE_WRITECOPY);
        if (!readable) {
            block = regionEnd;
            continue;
        }
        __try {
            for (std::uintptr_t p = block; p + 8 <= regionEnd; p += 8) {
                ++g_lightQwords;
                const auto value = *reinterpret_cast<const std::uintptr_t*>(p);
                if (InUi(value)) MarkWatch(value, "the 5-SECOND LIGHT SWEEP of the descriptor pool");
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ++g_lightFaults;
        }
        block = regionEnd;
    }
}

void ScanPool() {
    ResolveModules();
    if (!g_engineBase || !g_uiBase) return;
    g_poolCount = 0;
    g_poolScanned = 0;
    g_poolHits = 0;
    g_poolDropped = 0;
    g_poolFaulted = false;
    g_poolControlFound = false;
    g_poolControlVisited = false;
    g_poolControlInRange = false;
    g_poolBlocksSkipped = 0;
    g_poolCapped = false;

    std::uintptr_t engineSize = 0;
    {
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_engineBase);
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            reinterpret_cast<const std::uint8_t*>(g_engineBase) + dos->e_lfanew);
        engineSize = nt->OptionalHeader.SizeOfImage;
    }
    const std::uintptr_t control = static_cast<std::uintptr_t>(g_controlDescriptor);
    const std::uintptr_t start = g_engineBase;
    const std::uintptr_t end = g_engineBase + engineSize;
    g_poolStart = start;
    g_poolEnd = end;
    if (control >= start && control + 0x80 <= end) g_poolControlInRange = true;

    // THE KNOWN POOL IS SWEPT FIRST, and this is not an optimisation.
    //
    // The qword budget is finite, engine.dll's image is a third of a gigabyte,
    // and the descriptors we already know about sit 312 MB into it. Sweeping
    // strictly front to back could therefore spend the whole budget before
    // reaching them, which would fail the control and waste the run. So the
    // 2 MB around the known descriptors is swept first -- the control passes,
    // and the widgets we can already name are always in the report -- and the
    // rest of the image is swept afterwards with whatever budget is left. Pass
    // two skips the priority window so pointer counts are not doubled.
    const std::uintptr_t prioStart = g_engineBase + 0x12900000;
    const std::uintptr_t prioEnd = g_engineBase + 0x12B00000;
    const bool prioValid = prioEnd <= end;
    if (prioValid) ScanRange(prioStart, prioEnd, 0, 0);
    ScanRange(start, end, prioValid ? prioStart : 0, prioValid ? prioEnd : 0);
    g_poolRan = true;
}

void ReportPool() {
    char line[1200]{};
    if (!g_poolRan) {
        Tf2VrLog("[TF2VR] POOL SCAN: has not run yet. It runs once the game is rendering a world, "
                 "and again on every F3.\n");
        return;
    }
    // THE CONTROL FIRST. A scan that cannot find the descriptor the live FC500
    // census is holding is measuring nothing, and every null below it is void.
    const char* verdict;
    if (!g_controlDescriptor) {
        verdict = "NO CONTROL YET: the FC500 census has not handed over a descriptor, so this "
                  "scan is unverified. Treat every absence below as unproven.";
    } else if (!g_poolControlInRange) {
        verdict = "DISQUALIFIED: the control descriptor lies OUTSIDE engine.dll's image, which "
                  "should be impossible, so this scan is measuring the wrong thing and every "
                  "absence below is void.";
    } else if (!g_poolControlVisited) {
        verdict = "DISQUALIFIED: the control descriptor's address was never even reached (its "
                  "region was skipped or the qword cap cut the scan short), so every absence "
                  "below is void.";
    } else if (!g_poolControlFound) {
        verdict = "DISQUALIFIED: the control descriptor was reached but neither of its draw slots "
                  "read back as a ui(11) pointer, so the scan's test is broken and every absence "
                  "below is void.";
    } else {
        verdict = "CONTROL PASSED: the scan reached the exact descriptor the live census is using "
                  "and read its draw slot correctly, so an absence below is a real absence.";
    }
    std::snprintf(line, sizeof(line),
                  "[TF2VR] ENGINE DATA SCAN over engine.dll 0x%llX..0x%llX (the whole image): "
                  "%lld qwords examined, %lld of them pointing into ui(11), %d distinct widget "
                  "functions, %lld dropped for want of a row (cap %d), %lld regions skipped as "
                  "unreadable, faulted=%d, hit the qword cap=%d. Control descriptor 0x%llX "
                  "inRange=%d visited=%d slotRead=%d. %s\n",
                  static_cast<unsigned long long>(g_poolStart),
                  static_cast<unsigned long long>(g_poolEnd), g_poolScanned, g_poolHits,
                  g_poolCount, g_poolDropped, kMaxPool, g_poolBlocksSkipped,
                  g_poolFaulted ? 1 : 0, g_poolCapped ? 1 : 0,
                  static_cast<unsigned long long>(g_controlDescriptor),
                  g_poolControlInRange ? 1 : 0, g_poolControlVisited ? 1 : 0,
                  g_poolControlFound ? 1 : 0, verdict);
    Tf2VrLog(line);
    for (int i = 0; i < g_poolCount; ++i) {
        std::snprintf(line, sizeof(line),
                      "[TF2VR]   DATA ui(11)+0x%-7X  %ld live pointer(s), first at 0x%llX\n",
                      g_pool[i].targetRva, g_pool[i].count,
                      static_cast<unsigned long long>(g_pool[i].firstAt));
        Tf2VrLog(line);
    }
}

void ReportWatch() {
    char line[640]{};
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_watch[i].seen) {
            std::snprintf(line, sizeof(line), "[TF2VR]   WATCH ui(11)+0x%-7X SEEN at t=%llu by %s | %s\n",
                          g_watch[i].rva, g_watch[i].tick, g_watch[i].how, g_watch[i].what);
        } else {
            std::snprintf(line, sizeof(line),
                          "[TF2VR]   WATCH ui(11)+0x%-7X NOT SEEN by any path or by the pool scan | %s\n",
                          g_watch[i].rva, g_watch[i].what);
        }
        Tf2VrLog(line);
    }
}

void ReportImmediate() {
    char line[1500]{};
    const char* state = g_installed ? "CLAIMED"
                                    : (g_installRefused ? "REFUSED" : "not installed yet");
    std::snprintf(line, sizeof(line),
                  "[TF2VR] IMMEDIATE PATH: %s (%s) | calls=%lld distinct=%ld | DECLINES "
                  "nolayer=%lld nulldesc=%lld dropped=%lld (cap %d) faults=%lld | reclaims=%lld | "
                  "FC500 identities noted=%lld, carrying a +0x70 draw fn=%lld, of which +0x70 "
                  "differs from +0x68=%lld | LIGHT SWEEP sweeps=%lld qwords=%lld faults=%lld\n",
                  state, g_installNote[0] ? g_installNote : "-", g_immCalls,
                  g_immReserved > kMaxImm ? static_cast<long>(kMaxImm) : g_immReserved,
                  g_immNullLayer, g_immNullDescriptor, g_immDropped, kMaxImm, g_immFaults,
                  g_reclaims, g_fc500Noted, g_fc500With70, g_fc500Differ, g_lightSweeps,
                  g_lightQwords, g_lightFaults);
    Tf2VrLog(line);
    // THE NULL IS AN ANSWER, AND IT IS SPELLED OUT so it cannot be read as a
    // broken instrument.
    if (g_installed && g_immCalls == 0) {
        Tf2VrLog("[TF2VR]   INVALID COVERAGE: claimed slot has ZERO calls. No positive control; "
                 "this instrument cannot exclude the immediate path.\n");
    }
    long taken = g_immReserved;
    if (taken > kMaxImm) taken = kMaxImm;
    // THE WATCHLIST RUNS AGAIN OVER EVERY ROW, every report. MarkWatch is
    // idempotent, so this costs a few compares and closes the one window where
    // a hit could be missed: a descriptor recorded before ui(11) resolved.
    for (long i = 0; i < taken; ++i) {
        if (!g_imm[i].descriptor) continue;
        MarkWatch(g_imm[i].draw70, "the IMMEDIATE path (engine+0xFC960, descriptor slot +0x70)");
        MarkWatch(g_imm[i].draw68, "the IMMEDIATE path (engine+0xFC960, descriptor slot +0x68)");
    }
    if (taken != g_lastRowsPrinted) {
        g_lastRowsPrinted = taken;
        for (long i = 0; i < taken; ++i) {
            if (!g_imm[i].descriptor) continue;
            char w68[72]{}, w70[72]{};
            Describe(g_imm[i].draw68, w68, sizeof(w68));
            Describe(g_imm[i].draw70, w70, sizeof(w70));
            std::snprintf(line, sizeof(line),
                          "[TF2VR]   IMM desc=0x%llX space %.1fx%.1f calls=%lld | +0x68 %s | +0x70 %s\n",
                          static_cast<unsigned long long>(g_imm[i].descriptor), g_imm[i].w,
                          g_imm[i].h, g_imm[i].calls, w68, w70);
            Tf2VrLog(line);
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------

void SetRuiImmediateWanted(bool wanted) {
    g_wanted = wanted;
    Tf2VrLog(wanted
                 ? "[TF2VR] rui.immediate = 1: the second RUI draw path (engine+0xFC960, which "
                   "calls a widget through its descriptor's +0x70 slot) is claimed read-only "
                   "through the client's own interface table, and engine.dll's descriptor pool is "
                   "scanned for every widget the game holds a descriptor for. Both are read-only. "
                   "F3 stamps a mark and dumps everything.\n"
                 : "[TF2VR] rui.immediate = 0: the second RUI draw path is left alone.\n");
}

bool RuiImmediateWanted() { return g_wanted; }

void RuiImmediateNoteFc500Descriptor(std::uintptr_t descriptor) {
    if (!g_wanted || !descriptor) return;
    ResolveModules();
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_fc500Noted));
    InterlockedCompareExchange64(&g_controlDescriptor, static_cast<long long>(descriptor), 0);
    std::uintptr_t draw68 = 0, draw70 = 0;
    float w = 0.0f, h = 0.0f;
    if (!ReadDescriptor(descriptor, draw68, draw70, w, h)) return;
    if (draw70) {
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_fc500With70));
        if (draw70 != draw68) {
            InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_fc500Differ));
        }
        MarkWatch(draw70, "an FC500 identity's SECOND draw slot (+0x70)");
    }
    MarkWatch(draw68, "the FC500 path (descriptor slot +0x68)");
}

void RuiImmediateMark() {
    ++g_markCount;
    char line[520]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR] ======== READ-ONLY RUI MARK #%d at t=%llu ======== "
                  "Draw-path census only; no visible element is identified by these tables.\n",
                  g_markCount, static_cast<unsigned long long>(GetTickCount64()));
    Tf2VrLog(line);
    ScanPool();
    ReportPool();
    // FORCED BEFORE THE REPORT, NOT AFTER. ReportImmediate only prints its rows
    // when the row count has changed since last time, so setting this afterwards
    // would have left the mark itself without the table it exists to capture and
    // pushed it into the next periodic line.
    g_lastRowsPrinted = -1;
    ReportImmediate();
    ReportWatch();
    Tf2VrLog("[TF2VR] ======== end of READ-ONLY RUI MARK ========\n");
}

void AdvanceRuiImmediate(bool worldReady) {
    if (!g_wanted) return;
    TryInstall();
    ReclaimIfLost();
    // THE SCAN WAITS FOR A WORLD *AND* FOR ITS CONTROL. Run at load it would
    // sample the menu, which is the mistake this project has made before with
    // capped dumps; run before the FC500 census has handed over a descriptor it
    // would have no control and its verdict would read "unverified", which is
    // the weakest thing this instrument can say. Waiting costs a second or two
    // and makes the automatic scan a verified one. F3 forces a scan regardless,
    // and if the control never arrives the status line says so with
    // `FC500 identities noted=0`.
    if (!g_poolRan && worldReady && g_controlDescriptor) {
        ScanPool();
        ReportPool();
        ReportWatch();
    }
    const unsigned long long now = GetTickCount64();
    if (now - g_lastReportTick < 5000) return;
    g_lastReportTick = now;
    // Before the report, so a widget that only exists while the trigger is held
    // is caught within five seconds of appearing even if F3 is never pressed.
    LightSweep();
    ReportImmediate();
    ReportWatch();
}
