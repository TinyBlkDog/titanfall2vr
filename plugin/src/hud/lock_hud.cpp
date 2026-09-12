#include "lock_hud.h"

#include "plugin_cost.h"

#include <windows.h>
#include <intrin.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "camera_update_hook.h"
#include "engine_cvars.h"
#include "diagnostics.h"
#include "hook_registry.h"

// ---------------------------------------------------------------------------
// THE RING PANELS ARE LOOKED UP BY NAME, AND THAT IS A SEAM THAT MUST RUN.
//
// Six placement natives all read zero, which was not a mis-extraction: the
// registration table at client+0x2A8C000 confirms Hud_SetEntity really is
// client+0x5338A0. Script simply never places these. They are driven by C++.
//
// client+0x4DC820 is that driver. It builds a name -- "TargetRingBullet" plus
// an index, formatted through client+0x7334A0 into a 0x80 buffer -- and hands
// it to client+0x52F9B0, which looks the HUD element up by name and returns
// the panel. It runs ONCE per session, enumerating all 90 ring slots.
//
// 2026-09-07 build 2. The first 0x140 bytes of the client panel never moved
// across three sweeps, so this build watches 0x400 of the client panel AND
// 0x100 of the vgui2 VPanel it points at, EVERY FRAME, not only on F3. The
// VPanel is found without calling anything: a pointer in the client panel
// whose target holds a pointer BACK to the client panel (the same offset pair
// on every ring is the control that the link is real). Positions in vgui2 are
// shorts, so the VPanel is diffed at 2-byte granularity; the client panel at 4.
// Every 5 s the log names the offsets that changed most; the first 80 changes
// after each F3 are printed with values. Both vtables are dumped once so the
// slot that writes the discovered offset can be read offline with pescan.
// ---------------------------------------------------------------------------

namespace {

constexpr std::uintptr_t kDriverRva = 0x4DC820;   // formats the name, drives the rings
constexpr std::uintptr_t kLookupRva = 0x52F9B0;   // find HUD element by name

// mov [rsp+0x10],rdx / push rbp / push rbx / push r15 / lea rbp,[rsp-0x90]
constexpr std::uint8_t kDriverBytes[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x41,
                                         0x57, 0x48, 0x8D, 0xAC, 0x24, 0x70, 0xFF, 0xFF, 0xFF};
constexpr std::size_t kDriverLen = sizeof(kDriverBytes);
// push rbx / sub rsp,0x20 / mov rbx,rcx / mov rcx,[rcx] / test rcx,rcx
constexpr std::uint8_t kLookupBytes[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48,
                                         0x8B, 0xD9, 0x48, 0x8B, 0x09, 0x48, 0x85, 0xC9};
constexpr std::size_t kLookupLen = sizeof(kLookupBytes);

constexpr std::size_t kWatchBytes = 0x400;
constexpr std::size_t kVWatch = 0x100;
constexpr std::size_t kVScan = 0x80;     // where in the client panel the VPanel pointer may sit

using DriverFn = void*(__fastcall*)(void*, void*, void*, void*);
using LookupFn = void*(__fastcall*)(void*, const char*);
using CreateInterfaceFn = void*(*)(const char*, int*);

DriverFn g_driverOrig = nullptr;
LookupFn g_lookupOrig = nullptr;
std::uintptr_t g_clientBase = 0;
bool g_installed = false;
bool g_refused = false;
bool g_driverOk = false;
bool g_lookupOk = false;

std::uint64_t g_driverCalls = 0;
std::uint64_t g_lookupCalls = 0;
std::uint64_t g_ringLookups = 0;
std::uint64_t g_faults = 0;
int g_namesLogged = 0;
int g_nullNamesLogged = 0;
std::uint64_t g_lookupNullNames = 0;   // non-ring lookups that returned no node
std::uint64_t g_lookupDropped = 0;     // non-ring nodes lost to a full table
bool g_censusOn = false;   // lockhud.census, default 0
std::uint32_t g_ringGeneration = 0;   // bumped when a known ring name returns a new node
constexpr int kMaxRings = 128;
constexpr int kMaxOthers = 256;       // non-ring script-HUD nodes seen by the lookup
void* g_otherNode[kMaxOthers]{};
char g_otherName[kMaxOthers][48]{};
int g_otherCount = 0;
struct Ring {
    void* panel;
    char name[48];
    std::uint8_t snap[kWatchBytes];   // baseline at the last F3
    std::uint8_t prev[kWatchBytes];   // last frame
    bool haveSnap, havePrev;
    void* vpanel;                     // validated by the pointer cycle, else null
    int vpanelOff, backOff;           // panel+vpanelOff -> vpanel; vpanel+backOff -> panel
    int vpanelMatches;                // >1 means the cycle was ambiguous for this ring
    std::uint8_t vsnap[kVWatch];
    std::uint8_t vprev[kVWatch];
    bool haveVSnap, haveVPrev;
};
Ring g_rings[kMaxRings]{};
int g_ringCount = 0;
void* g_ringPanel = nullptr;
char g_ringName[64]{};

int g_dumps = 0;
std::uint64_t g_lastTick = 0;

// Per-frame change histograms, reset every status tick.
std::uint32_t g_hist[kWatchBytes / 4]{};
std::uint32_t g_vhist[kVWatch / 2]{};
std::uint64_t g_frames = 0;          // frames the per-frame watch ran with >=1 ring held
std::uint64_t g_panelReads = 0, g_panelFaults = 0, g_vReads = 0, g_vFaults = 0;
std::uint64_t g_panelChangedFrames = 0, g_vChangedFrames = 0;
int g_vpFound = 0, g_vpMissed = 0, g_vpUnreadable = 0, g_vpAmbiguous = 0;
int g_changeLines = 0;               // rolling per-change log, capped, reopened by F3
constexpr int kChangeLineCap = 80;
bool g_vtablesLogged = false;

bool ReadableRange(const void* p, std::size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const auto a = reinterpret_cast<std::uintptr_t>(p);
    return a + n <= base + mbi.RegionSize;
}

// AN ADDRESS THAT CANNOT BE A POINTER, REJECTED BY ARITHMETIC.
//
// 2026-09-08, and this one crashed the wearer's game three times before it was
// understood. Removing the ReadableRange (VirtualQuery) guards from the ring
// path was right for speed -- 290 us a call, ~270 calls a tick -- but it left
// SafeCopy's __except as the ONLY thing standing between a dead ring pointer
// and a fault. The crash record:
//
//   ACCESS_VIOLATION at VCRUNTIME140.dll+0x1C177 (memcpy)
//   READ from 0xFFFFFFFFFFFFFFFF
//   stack: memcpy <- titanfall2vr.dll x4 (lock_hud) <- Northstar <- engine
//
// 0xFFFFFFFFFFFFFFFF is a stale ring node, and on x86-64 it is NON-CANONICAL:
// touching it raises a general-protection fault rather than an ordinary page
// fault, and recovering from that through __except is not dependable. "Let it
// fault and catch it" was a bad trade for the two cases that matter.
//
// So the impossible addresses are refused before memcpy sees them, in three
// compares and no syscall: null, the never-mapped low pages, and anything
// outside the 47-bit canonical user half. That is the part of ReadableRange
// that was load-bearing here; the part that cost 290 us is still gone.
bool PlausiblePointer(const void* p, std::size_t n) {
    const auto a = reinterpret_cast<std::uintptr_t>(p);
    if (a < 0x10000) return false;                        // null and the guard pages
    if (a >= 0x0000800000000000ULL) return false;         // non-canonical, or kernel
    if (a + n < a) return false;                          // wraps
    if (a + n >= 0x0000800000000000ULL) return false;     // runs off the canonical half
    return true;
}

// THE MAPPING CHECK IS BACK, CACHED. This is the third attempt and the reason
// for each is worth keeping.
//
//   1. ReadableRange (a VirtualQuery per call) -- CORRECT, and 290 us a call
//      across ~270 calls a tick was the 55 ms interval hitch.
//   2. Arithmetic only -- FAST, and it crashed: 0xFFFFFFFFFFFFFFFF is refused
//      by a canonical-range test, but the next stale pointer read
//      0x300000000004, which IS canonical and simply is not mapped. A plausible
//      address is not a mapped one, and __except did not save us either time.
//   3. This: ask the kernel, but at most once per REGION rather than once per
//      pointer. The ~90 ring nodes live in a handful of heaps, so a small cache
//      of validated regions answers nearly every call from memory. Misses cost
//      one VirtualQuery; hits cost two compares.
//
// The cache is dropped whenever the HUD is re-created, which is the only moment
// these mappings can change under us -- the same generation counter the ring
// table already keys on. Nothing here is a substitute for the SEH guard below;
// it is what stops the guard from being the only line of defence.
struct Region { std::uintptr_t base, end; };
constexpr int kRegionCache = 16;
Region g_okRegion[kRegionCache]{};
int g_okCount = 0;
int g_okNext = 0;

void ForgetValidatedRegions() {
    g_okCount = 0;
    g_okNext = 0;
}

bool MappedForRead(const void* p, std::size_t n) {
    const auto a = reinterpret_cast<std::uintptr_t>(p);
    for (int i = 0; i < g_okCount; ++i) {
        if (a >= g_okRegion[i].base && a + n <= g_okRegion[i].end) return true;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const auto end = base + mbi.RegionSize;
    if (a + n > end) return false;
    // Remember it. Round-robin rather than LRU: the working set here is a few
    // heaps, so which entry is evicted does not matter.
    const int slot = (g_okCount < kRegionCache) ? g_okCount++ : (g_okNext = (g_okNext + 1) % kRegionCache);
    g_okRegion[slot].base = base;
    g_okRegion[slot].end = end;
    return true;
}

// SEH must live in a function with no C++ objects to unwind.
bool SafeCopy(void* dst, const void* src, std::size_t n) {
    if (!PlausiblePointer(src, n)) return false;
    if (!MappedForRead(src, n)) return false;
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// READABLE, WITHOUT THE 290 us. This is the guarantee ReadableRange actually
// provided at its call sites -- "these n bytes can be read" -- and a successful
// copy IS that proof. It costs a bounded memcpy when the read succeeds and one
// SEH dispatch when it does not, instead of a VirtualQuery every time.
//
// IT EXISTS BECAUSE REMOVING THE CHECK BROKE THE HUD. The previous build
// deleted ReadableRange from the ring path for speed and the wearer reported
// Titan HUD elements no longer being moved. The check was never only a fault
// guard: it also FILTERED which pointers became live rings, so without it a
// stale-but-mapped pointer passes, the wrong panel is matched, and the
// placement silently stops. A guard can be a gate; replacing one means keeping
// everything it established, not just the part that was in the way.
bool Readable(const void* p, std::size_t n) {
    if (!p) return false;
    std::uint8_t scratch[0x140];
    if (n > sizeof(scratch)) n = sizeof(scratch);
    return SafeCopy(scratch, p, n);
}

void* __fastcall DriverHook(void* a, void* b, void* c, void* d) {
    ++g_driverCalls;
    return g_driverOrig ? g_driverOrig(a, b, c, d) : nullptr;
}

void* __fastcall LookupHook(void* container, const char* name) {
    ++g_lookupCalls;
    void* panel = g_lookupOrig ? g_lookupOrig(container, name) : nullptr;
    __try {
        if (name && name[0]) {
            const bool isRing = std::strncmp(name, "TargetRing", 10) == 0;
            // 2026-09-07: the 40/90 cap silenced every lookup after HUD creation.
            // Ring names stay capped (90 alike); every OTHER name prints once at
            // first sight, and a table-full drop is counted, never silent.
            if (isRing && g_namesLogged < 40) {
                ++g_namesLogged;
                char line[240]{};
                std::snprintf(line, sizeof(line), "[TF2VR] LOOKUP '%s' -> %p\n", name, panel);
                Tf2VrLog(line);
            }
            if (!isRing && !panel) {
                ++g_lookupNullNames;
                if (g_nullNamesLogged < 40) {
                    ++g_nullNamesLogged;
                    char line[240]{};
                    std::snprintf(line, sizeof(line), "[TF2VR] LOOKUP '%s' -> (no node)\n", name);
                    Tf2VrLog(line);
                }
            }
            if (panel && !isRing) {
                // BUILD 16: keep every other script-HUD node the lookup returns. Their
                // boxes at HUD creation carry the game's own idea of the screen centre,
                // which the ring's box only receives on the first release.
                bool known = false;
                for (int i = 0; i < g_otherCount; ++i) {
                    if (std::strcmp(g_otherName[i], name) == 0) { g_otherNode[i] = panel; known = true; break; }
                }
                if (!known && g_otherCount < kMaxOthers) {
                    g_otherNode[g_otherCount] = panel;
                    std::snprintf(g_otherName[g_otherCount], sizeof(g_otherName[0]), "%s", name);
                    ++g_otherCount;
                    char line[240]{};
                    std::snprintf(line, sizeof(line), "[TF2VR] LOOKUP '%s' -> %p (node %d, first sight)\n", name, panel, g_otherCount);
                    Tf2VrLog(line);
                } else if (!known) {
                    ++g_lookupDropped;
                }
            }
            if (panel && std::strncmp(name, "TargetRing", 10) == 0) {
                ++g_ringLookups;
                g_ringPanel = panel;
                std::snprintf(g_ringName, sizeof(g_ringName), "%s", name);
                // REPLACE BY NAME. The HUD is re-created on a new Titan or level and
                // every ring gets a new node; appending them hit the 128 cap with 90
                // stale entries in front (build 10 run: held=128, vpanels=11).
                bool known = false;
                for (int r = 0; r < g_ringCount; ++r) {
                    if (std::strcmp(g_rings[r].name, name) == 0) {
                        if (g_rings[r].panel != panel) {
                            g_rings[r].panel = panel;
                            g_rings[r].vpanel = nullptr;
                            ++g_ringGeneration;
                        }
                        known = true;
                        break;
                    }
                }
                if (!known && g_ringCount < kMaxRings) {
                    Ring& rg = g_rings[g_ringCount];
                    rg.panel = panel;
                    std::snprintf(rg.name, sizeof(rg.name), "%s", name);
                    rg.haveSnap = rg.havePrev = rg.haveVSnap = rg.haveVPrev = false;
                    rg.vpanel = nullptr;
                    rg.vpanelMatches = 0;
                    ++g_ringCount;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_faults;
    }
    return panel;
}

bool Patch(std::uintptr_t rva, const std::uint8_t* expected, std::size_t len, void* hook,
           void** originalOut, const char* what) {
    auto* site = reinterpret_cast<std::uint8_t*>(g_clientBase + rva);
    if (std::memcmp(site, expected, len) != 0) {
        char line[420]{};
        int used = std::snprintf(line, sizeof(line),
            "[TF2VR] RINGS: REFUSING client+0x%llX (%s) -- wrong bytes. Found:",
            static_cast<unsigned long long>(rva), what);
        for (std::size_t i = 0; i < len && used < static_cast<int>(sizeof(line)) - 8; ++i) {
            used += std::snprintf(line + used, sizeof(line) - used, " %02X", site[i]);
        }
        std::snprintf(line + used, sizeof(line) - used, "\n");
        Tf2VrLog(line);
        return false;
    }
    auto* tramp = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    std::memcpy(tramp, site, len);
    std::uint8_t* jump = tramp + len;
    jump[0] = 0xFF;
    jump[1] = 0x25;
    std::memset(jump + 2, 0, 4);
    const auto resume = reinterpret_cast<std::uintptr_t>(site + len);
    std::memcpy(jump + 6, &resume, sizeof(resume));
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);
    *originalOut = tramp;

    std::uint8_t detour[24]{};
    detour[0] = 0xFF;
    detour[1] = 0x25;
    const auto target = reinterpret_cast<std::uintptr_t>(hook);
    std::memcpy(detour + 6, &target, sizeof(target));
    std::memset(detour + 14, 0x90, len - 14);

    DWORD old = 0;
    if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(site, detour, len);
    FlushInstructionCache(GetCurrentProcess(), site, len);
    DWORD ignored = 0;
    VirtualProtect(site, len, old, &ignored);
    RegisterHookSite(what, site, len);
    return true;
}

bool Install() {
    if (g_installed || g_refused) return g_installed;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return false;
    g_clientBase = reinterpret_cast<std::uintptr_t>(client);
    g_lookupOk = Patch(kLookupRva, kLookupBytes, kLookupLen, reinterpret_cast<void*>(&LookupHook),
                       reinterpret_cast<void**>(&g_lookupOrig), "HUD element lookup by name");
    g_driverOk = Patch(kDriverRva, kDriverBytes, kDriverLen, reinterpret_cast<void*>(&DriverHook),
                       reinterpret_cast<void**>(&g_driverOrig), "lock ring driver");
    g_installed = g_lookupOk || g_driverOk;
    g_refused = !g_installed;
    char line[520]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] RINGS: lookup-by-name %s, ring driver %s. Build 12: hud2d zoom undone for the ring pass, anchor minus the VGUI inset, identity-checked; ring nodes 0x%zX, "
        "panel and 0x%zX of its VPanel every frame.\n",
        g_lookupOk ? "wrapped" : "NOT wrapped", g_driverOk ? "wrapped" : "NOT wrapped",
        kWatchBytes, static_cast<std::size_t>(0x4C));
    Tf2VrLog(line);
    return g_installed;
}

// Find the vgui2 VPanel behind a client panel: a pointer in the panel whose
// target points back at the panel. No calls into either object.
void* ReadPtr(const void* base, std::size_t off) {
    // VirtualQuery IS THE COST IN THIS PROCESS, AND IT WAS MEASURED BY GETTING
    // IT WRONG.
    //
    // The previous version of this function added a ReadableRange (one
    // VirtualQuery) ahead of the read, on the theory that these reads were
    // faulting on dead ring pointers and SEH dispatch was the expense. The next
    // run made the segment WORSE: res.rings went 52.27-56.06 ms to
    // 75.91-80.28 ms. One extra VirtualQuery per ring across ~90 rings cost
    // ~26 ms, which prices a single VirtualQuery at roughly 290 MICROSECONDS.
    //
    // That accidental A/B is the whole answer, and it was never the fault path:
    // ~3 ReadableRange calls per ring x ~90 rings is ~270 VirtualQuery calls,
    // which at that price IS the ~55 ms this loop cost before I made it worse.
    // VirtualQuery walks the process VAD tree under a lock, and this process --
    // the game, D3D, a VR runtime and Northstar -- has an enormous, heavily
    // fragmented address space. "VirtualQuery is cheap" is not true here.
    //
    // So the guard is GONE from the read path. SafeCopy alone is the right
    // tool: __try/__except costs nothing when the read SUCCEEDS, which the
    // status line says is the normal case (identityFails=0), and it still
    // refuses a genuinely dead pointer. ReadableRange survives for cold paths
    // that run once, never per ring per tick.
    if (!base) return nullptr;
    void* p = nullptr;
    if (!SafeCopy(&p, static_cast<const std::uint8_t*>(base) + off, 8)) return nullptr;
    return p;
}

// ---------------------------------------------------------------------------
// BUILD 10 (2026-09-07). Build 9 in the headset: "much better, framerate
// improved, rings stay put through head and hand movement". Two residuals:
// every ring sits the SAME distance left and up of its target, and the rings
// are large. The offset is a units error: the ring node's own box puts the
// VGUI screen centre at (2472,1691), so VGUI space is 4944x3382 while the
// zoom anchor is in pass pixels (5210x3648). Using the anchor unconverted
// shifts every ring by A(1 - 1/s)(1 - 1/z) ~ -340 px in both axes -- constant,
// left and up. So: convert A into VGUI space by s = VGUI/pass per axis.
// Size: lockhud.ring_scale (default 1) multiplies the ring; the panel centre
// is held fixed, so the scale can be tuned without moving the ring.
// ---------------------------------------------------------------------------

constexpr std::size_t kNodePanel = 0x28;
constexpr std::size_t kPanelVPanel = 0x68;
constexpr std::size_t kNodeX = 0x124, kNodeY = 0x128, kNodeW = 0x12C, kNodeH = 0x130;
constexpr std::uintptr_t kVPanelVtableRva = 0xDBB58;
constexpr int kSetPosSlot = 10;
constexpr std::uintptr_t kSetPosRva = 0x344E0;
constexpr int kSetSizeSlot = 12;
constexpr std::uintptr_t kSetSizeRva = 0x34590;

using SetPosFn = void(__fastcall*)(void*, int, int);
SetPosFn g_setPosOrig = nullptr;
SetPosFn g_setSizeOrig = nullptr;
bool g_hooksInstalled = false;
bool g_hooksRefused = false;
std::uintptr_t g_vgui2Base = 0;

void* g_ringV[kMaxRings]{};
volatile int g_ringVCount = 0;
int g_resolvedRings = 0;
std::uint64_t g_lastResolveTick = 0;
int g_gameW[kMaxRings]{}, g_gameH[kMaxRings]{};   // the size the GAME last asked for, per ring
int g_nodeX[kMaxRings]{}, g_nodeY[kMaxRings]{};   // the ring's PARKED top-left from its node box (constant from creation)
volatile float g_centreX = 0.0f, g_centreY = 0.0f;  // VGUI screen centre (root VPanel size / 2)
constexpr std::size_t kVParent = 0x28;              // VPanel parent link (SetParent stores it)
constexpr std::size_t kVSize = 0x50;                // VPanel size, two int16
constexpr std::size_t kVPos = 0x4C;                 // VPanel position, two int16
int g_vguiW = 0, g_vguiH = 0;                       // VGUI root size, from the root VPanel
volatile float g_insetX = 0.0f, g_insetY = 0.0f;    // sum of the ring's ancestors' positions: ring-local -> pass px

std::uint64_t g_setPosCalls = 0, g_setPosRing = 0, g_setSizeCalls = 0, g_setSizeRing = 0;
std::uint64_t g_applied = 0, g_skipped = 0;
std::uint64_t g_hookTicks = 0;
std::uint64_t g_lastStatusHookTicks = 0, g_lastStatusRing = 0;
bool g_on = false;
volatile float g_ringScale = 1.0f;
std::uint64_t g_lastRawTick = 0;
int g_rawThisSecond = 0;

std::uint64_t Qpc() {
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return static_cast<std::uint64_t>(li.QuadPart);
}
std::uint64_t QpcFreq() {
    static std::uint64_t f = 0;
    if (!f) { LARGE_INTEGER li{}; QueryPerformanceFrequency(&li); f = static_cast<std::uint64_t>(li.QuadPart); }
    return f ? f : 1;
}

// Pointer membership is not identity: after a HUD rebuild a stale ring VPanel
// address can belong to another panel (build 11 run: the Titan health bar was
// scaled and moved). So a match is confirmed by walking the chain back --
// node+0x28 -> panel, panel+0x68 == this VPanel, and the node still named
// TargetRingBullet -- before any ring treatment. A failed check drops the entry.
std::uint64_t g_identityFails = 0;
bool RingIdentityHolds(int i, void* vpanel) {
    void* node = g_rings[i].panel;
    if (!node) return false;
    void* panel = ReadPtr(node, kNodePanel);
    if (!panel) return false;
    void* v = ReadPtr(panel, kPanelVPanel);
    if (v != vpanel) return false;
    char nm[17]{};
    if (!SafeCopy(nm, static_cast<std::uint8_t*>(node) + 0x60, 16)) return false;
    return std::memcmp(nm, "TargetRingBullet", 16) == 0;
}

int RingIndexOf(void* vpanel) {
    const int n = g_ringVCount;
    for (int i = 0; i < n; ++i) {
        if (g_ringV[i] != vpanel) continue;
        if (RingIdentityHolds(i, vpanel)) return i;
        ++g_identityFails;
        g_ringV[i] = nullptr;
        return -1;
    }
    return -1;
}

struct Zoom { float avx, avy, z, sx, sy, ax, ay, pw, ph; bool ok; };
Zoom ReadZoom() {
    Zoom o{};
    float hx = 0, hy = 0;
    ReadReticleAnchor(&o.ax, &o.ay, nullptr, nullptr);
    ReadReticleAnchorDetail(&o.pw, &o.ph, nullptr, nullptr, nullptr, nullptr);
    ReadHud2dPlacement(&hx, &hy, &o.z);
    const float ax = o.ax, ay = o.ay, pw = o.pw, ph = o.ph;
    const float cx = 0.5f * pw, cy = 0.5f * ph;   // pass centre; the per-ring offset is applied in the hook
    o.ok = pw > 0 && ph > 0 && o.z > 0.01f && o.z < 10.0f;
    if (o.ok) {
        // Build 12. Three headset runs pin the mapping: anchor unconverted (9, 11)
        // = stable but a constant offset left/up; anchor SCALED by the ring box
        // (10) = no offset at rest but hand-dependent. Only a TRANSLATION fits
        // both: the VGUI screen (2C = 4944x3382) is CENTRED in the pass
        // (5210x3648), so pass px = VGUI px + (pw/2 - Cx, ph/2 - Cy) = (133,133).
        // The anchor in VGUI units is therefore A - inset. In flat the inset is 0.
        // Build 14: the inset is the ancestors' offset sum when the chain has been
        // read (root known), else the node-box estimate (pass centre minus box centre).
        // Build 16: the inset is pass centre minus the game's own HUD centre (see
        // ResolveRingVPanels for where that centre comes from). The VPanel chain
        // sum is always 0 here (build 15 log) and is no longer used.
        o.sx = 0.5f * pw - cx;
        o.sy = 0.5f * ph - cy;
        o.avx = ax - o.sx;
        o.avy = ay - o.sy;
    }
    return o;
}

int RoundI(float r) {
    if (r > 30000.0f) return 30000;
    if (r < -30000.0f) return -30000;
    return static_cast<int>(r + (r >= 0 ? 0.5f : -0.5f));
}

// ---------------------------------------------------------------------------
// MOVE CENSUS (2026-09-07, friendly-label hunt). READ-ONLY. Every VPanel::SetPos
// that is not a ring is tallied per VPanel: calls, calls that CHANGED the
// position, and the span covered. A label tracking a player in world space is
// moved every frame across a wide span; a parked panel is not. Every 5 s the
// top movers print with a name where the by-name lookup seam has seen the node
// (node+0x28 -> panel, panel+0x68 == VPanel, node name re-read so a stale
// pointer cannot alias) and the parent named the same way. Fixed table,
// overflow counted, reset after every print. Same thread as the heartbeat.
// ---------------------------------------------------------------------------
constexpr int kCensusSlots = 512;
struct MoveSlot {
    void* v;
    std::uint32_t calls, changed;
    int lastX, lastY, minX, maxX, minY, maxY;
};
MoveSlot g_census[kCensusSlots]{};
std::uint32_t g_censusOverflow = 0, g_censusDistinct = 0, g_censusCalls = 0;

void CensusNote(void* v, int x, int y) {
    ++g_censusCalls;
    const std::uintptr_t key = reinterpret_cast<std::uintptr_t>(v) >> 4;
    std::size_t i = static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> 55) & (kCensusSlots - 1);
    for (int probe = 0; probe < 16; ++probe, i = (i + 1) & (kCensusSlots - 1)) {
        MoveSlot& s = g_census[i];
        if (s.v == v) {
            ++s.calls;
            if (x != s.lastX || y != s.lastY) { ++s.changed; s.lastX = x; s.lastY = y; }
            if (x < s.minX) s.minX = x;
            if (x > s.maxX) s.maxX = x;
            if (y < s.minY) s.minY = y;
            if (y > s.maxY) s.maxY = y;
            return;
        }
        if (!s.v) {
            s.v = v; s.calls = 1; s.changed = 0;
            s.lastX = s.minX = s.maxX = x;
            s.lastY = s.minY = s.maxY = y;
            ++g_censusDistinct;
            return;
        }
    }
    ++g_censusOverflow;
}

// Name a VPanel through the lookup seam's node table; "" when no node is known.
const char* NameOfVPanel(void* v) {
    if (!v) return "";
    for (int i = 0; i < g_otherCount; ++i) {
        void* nd = g_otherNode[i];
        if (!nd || !ReadableRange(nd, 0x140)) continue;
        void* panel = ReadPtr(nd, kNodePanel);
        if (!panel || !ReadableRange(panel, kPanelVPanel + 8)) continue;
        if (ReadPtr(panel, kPanelVPanel) != v) continue;
        char nm[17]{};
        if (!SafeCopy(nm, static_cast<std::uint8_t*>(nd) + 0x60, 16)) continue;
        if (std::strncmp(nm, g_otherName[i], 16) != 0) continue;
        return g_otherName[i];
    }
    for (int r = 0; r < g_ringVCount; ++r) {
        if (g_ringV[r] == v) return g_rings[r].name;
    }
    return "";
}

// Name a VPanel from ITS OWN memory, no lookup table needed (2026-09-07: the
// by-name lookup seam is called zero times outside the Titan ring driver, so
// a name table built on it is empty for most of the HUD). The client panel is
// found by the pointer cycle the ring code proved (a qword in the VPanel whose
// target holds a qword back to the VPanel), then both objects are scanned for
// pointers to printable strings. Bounded: 16x16 qword reads plus 48 candidates.
int DescribePanel(void* v, char* out, std::size_t cap) {
    int used = 0;
    if (!v || !ReadableRange(v, 0x80)) return std::snprintf(out, cap, " (vpanel unreadable)");
    void* cli = nullptr;
    int cliOff = -1, backOff = -1;
    for (std::size_t off = 0; off < 0x80 && !cli; off += 8) {
        void* p = ReadPtr(v, off);
        if (!p || p == v || !ReadableRange(p, 0x100)) continue;
        for (std::size_t k = 0; k < 0x80; k += 8) {
            if (ReadPtr(p, k) == v) { cli = p; cliOff = static_cast<int>(off); backOff = static_cast<int>(k); break; }
        }
    }
    used += std::snprintf(out + used, cap - used, " cli=%p(v+0x%X,back+0x%X)", cli, cliOff, backOff);
    int found = 0;
    for (int pass = 0; pass < 2 && found < 4; ++pass) {
        void* obj = pass == 0 ? cli : v;
        const std::size_t span = pass == 0 ? 0x100 : 0x80;
        if (!obj) continue;
        for (std::size_t off = 0; off < span && found < 4 && used < static_cast<int>(cap) - 80; off += 8) {
            void* s = ReadPtr(obj, off);
            if (!s || !ReadableRange(s, 48)) continue;
            char buf[49]{};
            if (!SafeCopy(buf, s, 48)) continue;
            int n = 0;
            while (n < 48 && buf[n] >= 32 && buf[n] <= 126) ++n;
            if (n < 4 || (n < 48 && buf[n] != '\0')) continue;
            buf[n] = '\0';
            used += std::snprintf(out + used, cap - used, " %s+0x%X=[%s]", pass == 0 ? "cli" : "v", static_cast<unsigned>(off), buf);
            ++found;
        }
    }
    if (!found) used += std::snprintf(out + used, cap - used, " (no strings)");
    return used;
}

void PrintCensus(double secs) {
    float drawVgui = -1.0f;
    TryReadCvarFloat("r_drawvgui", drawVgui);
    char line[400]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR] MOVES over %.0fs: r_drawvgui=%.0f setpos non-ring=%u distinct=%u overflow=%u | lookups total=%llu null=%llu dropped=%llu nodes named=%d\n",
                  secs, drawVgui, g_censusCalls, g_censusDistinct, g_censusOverflow, static_cast<unsigned long long>(g_lookupCalls),
                  static_cast<unsigned long long>(g_lookupNullNames), static_cast<unsigned long long>(g_lookupDropped), g_otherCount);
    Tf2VrLog(line);
    bool used[kCensusSlots]{};
    for (int rank = 0; rank < 8; ++rank) {
        int best = -1;
        for (int i = 0; i < kCensusSlots; ++i) {
            if (!g_census[i].v || used[i]) continue;
            if (best < 0 || g_census[i].changed > g_census[best].changed ||
                (g_census[i].changed == g_census[best].changed && g_census[i].calls > g_census[best].calls)) {
                best = i;
            }
        }
        if (best < 0) break;
        used[best] = true;
        const MoveSlot& s = g_census[best];
        std::int16_t sz[2]{};
        void* parent = nullptr;
        if (ReadableRange(s.v, 0x60)) {
            SafeCopy(sz, static_cast<std::uint8_t*>(s.v) + kVSize, 4);
            parent = ReadPtr(s.v, kVParent);
        }
        std::snprintf(line, sizeof(line),
                      "[TF2VR]   MOVER #%d %p '%s' calls=%u changed=%u x=%d..%d y=%d..%d size=%dx%d parent=%p '%s'\n",
                      rank + 1, s.v, NameOfVPanel(s.v), s.calls, s.changed, s.minX, s.maxX, s.minY, s.maxY, sz[0], sz[1],
                      parent, NameOfVPanel(parent));
        Tf2VrLog(line);
        char desc[400]{};
        DescribePanel(s.v, desc, sizeof(desc));
        std::snprintf(line, sizeof(line), "[TF2VR]     #%d self:%s\n", rank + 1, desc);
        Tf2VrLog(line);
    }
    std::memset(g_census, 0, sizeof(g_census));
    g_censusOverflow = 0;
    g_censusDistinct = 0;
    g_censusCalls = 0;
}

void __fastcall SetPosHook(void* vpanel, int x, int y) {
    const std::uint64_t t0 = Qpc();
    ++g_setPosCalls;
    const int r = RingIndexOf(vpanel);
    if (r >= 0) {
        ++g_setPosRing;
        const int inX = x, inY = y;
        Zoom zm{};
        int gw = g_gameW[r], gh = g_gameH[r];
        if (g_on) {
            zm = ReadZoom();
            if (zm.ok && gw > 0 && gh > 0 && g_nodeX[r] > 0 && g_nodeY[r] > 0) {
                // BUILD 19. The zoom centre in ring space is the anchor minus the
                // ring's own PARKED offset: pass centre minus (parked x + half the
                // game's size). Every term is the game's -- the parked box never moves
                // from creation (build 18 dumps: x 2415, y 1634, width 378 then 114)
                // and the size comes from the game's first SetSize -- so it is there on
                // the first press and scales with the screen. At 5210x3648 it is 133 px,
                // the number the wearer's screenshot confirmed lands on the target.
                const float k = g_ringScale;
                const float insetX = 0.5f * zm.pw - (static_cast<float>(g_nodeX[r]) + 0.5f * static_cast<float>(gw));
                const float insetY = 0.5f * zm.ph - (static_cast<float>(g_nodeY[r]) + 0.5f * static_cast<float>(gh));
                const float avx = zm.ax - insetX, avy = zm.ay - insetY;
                zm.avx = avx; zm.avy = avy; zm.sx = insetX; zm.sy = insetY;
                const float cxIn = static_cast<float>(inX) + 0.5f * static_cast<float>(gw);
                const float cyIn = static_cast<float>(inY) + 0.5f * static_cast<float>(gh);
                const float cxOut = avx + (cxIn - avx) / zm.z;
                const float cyOut = avy + (cyIn - avy) / zm.z;
                x = RoundI(cxOut - 0.5f * k * static_cast<float>(gw) / zm.z);
                y = RoundI(cyOut - 0.5f * k * static_cast<float>(gh) / zm.z);
                ++g_applied;
            } else {
                ++g_skipped;
            }
        }
        // Build 17: every ring placed within the same second is logged (cap 12), so a
        // screenshot's frame can be matched to the placements the hook made in it.
        const std::uint64_t now = GetTickCount64();
        if (now - g_lastRawTick >= 1000) { g_lastRawTick = now; g_rawThisSecond = 0; }
        if (g_rawThisSecond < 12) {
            ++g_rawThisSecond;
            char line[340]{};
            std::snprintf(line, sizeof(line),
                          "[TF2VR] RAW %s in=(%d,%d) gameWH=(%d,%d) out=(%d,%d) on=%d | C=(%.0f,%.0f) inset=(%.0f,%.0f) Av=(%.0f,%.0f) z=%.3f k=%.2f ok=%d\n",
                          g_rings[r].name, inX, inY, gw, gh, x, y, g_on ? 1 : 0, g_centreX, g_centreY, zm.sx, zm.sy,
                          zm.avx, zm.avy, zm.z, g_ringScale, zm.ok ? 1 : 0);
            Tf2VrLog(line);
        }
    }
    if (r < 0) CensusNote(vpanel, x, y);
    g_hookTicks += Qpc() - t0;
    if (g_setPosOrig) g_setPosOrig(vpanel, x, y);
}

void __fastcall SetSizeHook(void* vpanel, int w, int h) {
    const std::uint64_t t0 = Qpc();
    ++g_setSizeCalls;
    const int r = RingIndexOf(vpanel);
    if (r >= 0) {
        ++g_setSizeRing;
        g_gameW[r] = w;
        g_gameH[r] = h;
        if (g_on) {
            const Zoom zm = ReadZoom();
            if (zm.ok) {
                w = RoundI(g_ringScale * static_cast<float>(w) / zm.z);
                h = RoundI(g_ringScale * static_cast<float>(h) / zm.z);
            }
        }
    }
    g_hookTicks += Qpc() - t0;
    if (g_setSizeOrig) g_setSizeOrig(vpanel, w, h);
}

// VPanels are fixed after HUD init: resolve every 2 s. Also refresh the VGUI
// centre from ring 0's node box and seed the game sizes from the node box.
void ResolveRingVPanels() {
    static std::uint32_t seenGeneration = 0;
    const std::uint64_t now = GetTickCount64();
    const bool regenerated = seenGeneration != g_ringGeneration;
    if (!regenerated && g_ringVCount == g_ringCount && now - g_lastResolveTick < 2000) return;
    g_lastResolveTick = now;
    if (regenerated) {
        seenGeneration = g_ringGeneration;
        // The HUD was re-created, so every mapping this cache validated may have
        // been freed underneath it. This is the only moment that can happen, and
        // it is exactly the Titan-to-pilot level change the wearer reproduces on.
        ForgetValidatedRegions();
        for (int r = 0; r < kMaxRings; ++r) { g_gameW[r] = 0; g_gameH[r] = 0; g_nodeX[r] = 0; g_nodeY[r] = 0; }
        char line[160]{};
        std::snprintf(line, sizeof(line), "[TF2VR] RINGS: HUD re-created (generation %u) -- ring nodes replaced by name, sizes reseeded.\n",
                      g_ringGeneration);
        Tf2VrLog(line);
    }
    int n = 0;
    for (int r = 0; r < g_ringCount && r < kMaxRings; ++r) {
        void* panel = ReadPtr(g_rings[r].panel, kNodePanel);
        // Same checks the ReadableRange version made, at SafeCopy prices. The
        // second one is a VALIDITY FILTER, not a fault guard: dropping it let
        // stale-but-mapped pointers become live rings and the Titan HUD stopped
        // being moved.
        void* v = (panel && Readable(panel, kPanelVPanel + 8)) ? ReadPtr(panel, kPanelVPanel) : nullptr;
        g_ringV[r] = (v && Readable(v, 0x60)) ? v : nullptr;
        if (g_ringV[r]) ++n;
        {
            std::int32_t bx = 0, by = 0, bw = 0, bh = 0;
            auto* nb = static_cast<std::uint8_t*>(g_rings[r].panel);
            // Readable(), not ReadableRange(): same guarantee, no VirtualQuery.
            if (g_ringV[r] && Readable(nb, kNodeH + 4) &&
                SafeCopy(&bx, nb + kNodeX, 4) && SafeCopy(&by, nb + kNodeY, 4) && SafeCopy(&bw, nb + kNodeW, 4) &&
                SafeCopy(&bh, nb + kNodeH, 4) && bx > 0 && by > 0 && bw > 0 && bh > 0 && bw < 2000) {
                g_nodeX[r] = bx;
                g_nodeY[r] = by;
                if (g_gameW[r] <= 0) { g_gameW[r] = bw; g_gameH[r] = bh; }
            }
        }
    }
    PluginCost::FrameSeg(20);
    g_resolvedRings = n;
    g_ringVCount = g_ringCount;
    // BUILD 13: the VGUI screen size comes from the ROOT VPanel (walk +0x28, the
    // parent link SetParent writes, to the top; size is the two shorts at +0x50).
    // The ring node's own box, used before, only takes its final value after the
    // first lock completes -- which is why the FIRST RB press was offset and the
    // rings snapped into place on release. The root is fixed from HUD creation.
    for (int r = 0; r < g_ringCount; ++r) {
        if (!g_ringV[r] || !RingIdentityHolds(r, g_ringV[r])) continue;
        // BUILD 14: the root IS the pass (5210x3648, build 13 log), so the ring's
        // coordinates are RELATIVE to an ancestor that sits at the inset. Sum the
        // ancestors' own positions (+0x4C) up the chain; that sum is the offset
        // between ring-local and pass pixels. Logged per level once, so the 133
        // is read off the log rather than inferred.
        void* v = g_ringV[r];
        int depth = 0;
        float sumX = 0.0f, sumY = 0.0f;
        char chain[600]{};
        int used = 0;
        for (; depth < 16; ++depth) {
            // The second check BOUNDS THE WALK. Dropping it let a 16-deep chain
            // keep walking through pointers that are not ancestors at all.
            void* parent = Readable(v, 0x60) ? ReadPtr(v, kVParent) : nullptr;
            if (!parent || !Readable(parent, 0x60)) break;
            std::int16_t px = 0, py = 0, pw2 = 0, ph2 = 0;
            SafeCopy(&px, static_cast<std::uint8_t*>(parent) + kVPos, 2);
            SafeCopy(&py, static_cast<std::uint8_t*>(parent) + kVPos + 2, 2);
            SafeCopy(&pw2, static_cast<std::uint8_t*>(parent) + kVSize, 2);
            SafeCopy(&ph2, static_cast<std::uint8_t*>(parent) + kVSize + 2, 2);
            sumX += static_cast<float>(px);
            sumY += static_cast<float>(py);
            if (used < 500) {
                used += std::snprintf(chain + used, sizeof(chain) - used, " [%d] pos=(%d,%d) size=(%d,%d)", depth + 1, px, py, pw2, ph2);
            }
            v = parent;
        }
        std::int16_t rw = 0, rh = 0;
        if (depth > 0 && SafeCopy(&rw, static_cast<std::uint8_t*>(v) + kVSize, 2) &&
            SafeCopy(&rh, static_cast<std::uint8_t*>(v) + kVSize + 2, 2) && rw > 0 && rh > 0) {
            if (rw != g_vguiW || rh != g_vguiH || sumX != g_insetX || sumY != g_insetY) {
                g_vguiW = rw;
                g_vguiH = rh;
                g_insetX = sumX;
                g_insetY = sumY;
                char line[760]{};
                std::snprintf(line, sizeof(line), "[TF2VR] RINGS: VGUI CHAIN above '%s':%s -> root %dx%d. Ancestor offset sum = (%.0f,%.0f) = the inset.\n",
                              g_rings[r].name, chain, rw, rh, sumX, sumY);
                Tf2VrLog(line);
            }
            g_centreX = 0.5f * static_cast<float>(rw);
            g_centreY = 0.5f * static_cast<float>(rh);
        }
        break;
    }
    PluginCost::FrameSeg(21);
    // BUILD 16. The VPanel chain is all full-screen (build 15 log) and the game's
    // projection is symmetric, yet the game's own HUD centre -- the ring box after
    // its first release, (2472,1691) at 5210x3648 -- is what makes the correction
    // land (build 12). The ring box only gets it on the first release. So take
    // the game's centre from the AutoLockCorner_0..3 boxes, which exist from HUD
    // creation, else from a ring box that has left the pass centre, else the
    // pass centre. Every other node's box is logged once so the source is on record.
    static bool boxesLogged = false;
    float cornerSumX = 0.0f, cornerSumY = 0.0f;
    int corners = 0;
    char line[240]{};
    for (int i = 0; i < g_otherCount; ++i) {
        void* nd = g_otherNode[i];
        std::int32_t bx = 0, by = 0, bw = 0, bh = 0;
        if (!nd || !ReadableRange(nd, 0x140) || !SafeCopy(&bx, static_cast<std::uint8_t*>(nd) + kNodeX, 4) ||
            !SafeCopy(&by, static_cast<std::uint8_t*>(nd) + kNodeY, 4) || !SafeCopy(&bw, static_cast<std::uint8_t*>(nd) + kNodeW, 4) ||
            !SafeCopy(&bh, static_cast<std::uint8_t*>(nd) + kNodeH, 4)) {
            continue;
        }
        char nm[17]{};
        SafeCopy(nm, static_cast<std::uint8_t*>(nd) + 0x60, 16);
        const bool nameHolds = std::strncmp(nm, g_otherName[i], 16) == 0;
        if (!boxesLogged && i < 60) {
            std::snprintf(line, sizeof(line), "[TF2VR]   BOX '%s' x=%d y=%d w=%d h=%d centre=(%.1f,%.1f)%s\n", g_otherName[i], bx, by, bw, bh,
                          bx + 0.5f * bw, by + 0.5f * bh, nameHolds ? "" : "  (name mismatch, ignored)");
            Tf2VrLog(line);
        }
        if (nameHolds && std::strncmp(g_otherName[i], "AutoLockCorner_", 15) == 0 && bw > 0 && bh > 0 && bw < 4000 && bh < 4000) {
            cornerSumX += static_cast<float>(bx) + 0.5f * static_cast<float>(bw);
            cornerSumY += static_cast<float>(by) + 0.5f * static_cast<float>(bh);
            ++corners;
        }
    }
    PluginCost::FrameSeg(22);
    if (!boxesLogged && g_otherCount > 0) {
        boxesLogged = true;
        std::snprintf(line, sizeof(line), "[TF2VR] RINGS: %d other nodes' boxes above; AutoLockCorner centre from %d corners = (%.1f,%.1f)\n",
                      g_otherCount, corners, corners ? cornerSumX / corners : 0.0f, corners ? cornerSumY / corners : 0.0f);
        Tf2VrLog(line);
    }
    // BUILD 17: the AutoLockCorner centre (2653,1872) is 48 px the OTHER way and
    // the LOCK text sits at the pass centre -- elements park in different places,
    // so no element box is "the HUD centre". Back to the one source that nearly
    // landed (build 12): the ring box once it has left the pass centre. Its raw
    // fields are logged whenever they change so the number is on record.
    (void)corners; (void)cornerSumX; (void)cornerSumY;
    float cx = 0.0f, cy = 0.0f;
    const char* source = "none";
    {
        std::int32_t bx = 0, by = 0, bw = 0, bh = 0;
        auto* n0 = static_cast<std::uint8_t*>(g_rings[0].panel);
        if (RingIdentityHolds(0, g_ringV[0]) && SafeCopy(&bx, n0 + kNodeX, 4) && SafeCopy(&by, n0 + kNodeY, 4) &&
            SafeCopy(&bw, n0 + kNodeW, 4) && SafeCopy(&bh, n0 + kNodeH, 4)) {
            static std::int32_t lastBox[4] = {-1, -1, -1, -1};
            if (bx != lastBox[0] || by != lastBox[1] || bw != lastBox[2] || bh != lastBox[3]) {
                lastBox[0] = bx; lastBox[1] = by; lastBox[2] = bw; lastBox[3] = bh;
                std::snprintf(line, sizeof(line), "[TF2VR] RINGS: ring 0 node box x=%d y=%d w=%d h=%d\n", bx, by, bw, bh);
                Tf2VrLog(line);
                // BUILD 18: the whole node as dwords, at creation and on each box
                // change (cap 3), to find where the parked position lives BEFORE the
                // first release -- the dynamic source the first press needs.
                static int dumps = 0;
                if (dumps < 3) {
                    ++dumps;
                    static std::uint8_t buf[0x240];
                    if (SafeCopy(buf, n0, sizeof(buf))) {
                        std::snprintf(line, sizeof(line), "[TF2VR]   NODE DWORDS of '%s' (dump %d), 8 per line:\n", g_rings[0].name, dumps);
                        Tf2VrLog(line);
                        for (std::size_t off = 0; off < sizeof(buf); off += 32) {
                            int used = std::snprintf(line, sizeof(line), "[TF2VR]     +%03zX:", off);
                            for (std::size_t k = 0; k < 8; ++k) {
                                std::int32_t v = 0;
                                float f = 0.0f;
                                std::memcpy(&v, buf + off + k * 4, 4);
                                std::memcpy(&f, buf + off + k * 4, 4);
                                const float af = f < 0 ? -f : f;
                                if (af > 1e-3f && af < 1e6f && (v < -1000000 || v > 1000000)) {
                                    used += std::snprintf(line + used, sizeof(line) - used, " %.3ff", f);
                                } else {
                                    used += std::snprintf(line + used, sizeof(line) - used, " %d", v);
                                }
                            }
                            std::snprintf(line + used, sizeof(line) - used, "\n");
                            Tf2VrLog(line);
                        }
                    }
                }
            }
            if (bx > 0 && by > 0 && bw > 0 && bh > 0 && bw < 2000) {
                cx = static_cast<float>(bx) + 0.5f * static_cast<float>(bw);
                cy = static_cast<float>(by) + 0.5f * static_cast<float>(bh);
                source = "ring box";
            }
        }
    }
    if (cx > 0 && cy > 0) {
        g_centreX = cx;
        g_centreY = cy;
        static float lastCx = -1.0f, lastCy = -1.0f;
        if (cx != lastCx || cy != lastCy) {
            lastCx = cx;
            lastCy = cy;
            std::snprintf(line, sizeof(line), "[TF2VR] RINGS: HUD centre now (%.1f,%.1f) from %s\n", cx, cy, source);
            Tf2VrLog(line);
        }
    }
    PluginCost::FrameSeg(23);
}

bool SwapSlot(int slot, std::uintptr_t expectedRva, void* hook, SetPosFn* orig, const char* what) {
    auto* p = reinterpret_cast<std::uintptr_t*>(g_vgui2Base + kVPanelVtableRva + static_cast<std::size_t>(slot) * 8);
    char line[300]{};
    if (*p != g_vgui2Base + expectedRva) {
        std::snprintf(line, sizeof(line), "[TF2VR] RINGS: REFUSING %s slot swap -- slot %d holds vgui2+0x%llX, expected +0x%llX\n",
                      what, slot, static_cast<unsigned long long>(*p - g_vgui2Base), static_cast<unsigned long long>(expectedRva));
        Tf2VrLog(line);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(p, 8, PAGE_READWRITE, &old)) return false;
    *orig = reinterpret_cast<SetPosFn>(*p);
    *p = reinterpret_cast<std::uintptr_t>(hook);
    DWORD ignored = 0;
    VirtualProtect(p, 8, old, &ignored);
    RegisterHookSite(what, p, 8);
    return true;
}

void InstallHooks() {
    if (g_hooksInstalled || g_hooksRefused) return;
    HMODULE vgui2 = GetModuleHandleA("vgui2.dll");
    if (!vgui2) return;
    g_vgui2Base = reinterpret_cast<std::uintptr_t>(vgui2);
    const bool a = SwapSlot(kSetPosSlot, kSetPosRva, reinterpret_cast<void*>(&SetPosHook), &g_setPosOrig, "VPanel::SetPos slot 10");
    const bool b = SwapSlot(kSetSizeSlot, kSetSizeRva, reinterpret_cast<void*>(&SetSizeHook), &g_setSizeOrig, "VPanel::SetSize slot 12");
    g_hooksInstalled = a || b;
    g_hooksRefused = !g_hooksInstalled;
    char line[240]{};
    std::snprintf(line, sizeof(line), "[TF2VR] RINGS: VPanel SetPos %s, SetSize %s. Zoom undo %s at start, ring scale %.2f.\n",
                  a ? "SWAPPED" : "NOT swapped", b ? "SWAPPED" : "NOT swapped", g_on ? "ON" : "OFF", g_ringScale);
    Tf2VrLog(line);
}

}  // namespace

void SetLockHudViewFix(bool on) { g_on = on; }
bool LockHudViewFixOn() { return g_on; }
void SetLockHudRingScale(float k) { g_ringScale = (k > 0.05f && k < 5.0f) ? k : 1.0f; }
float LockHudRingScale() { return g_ringScale; }


// F5 (2026-09-07): the VGUI-off test done by the plugin, not the console. The
// cvar is found through the engine's own interface, its value read before and
// after the write, and every outcome prints -- a missing cvar prints as such.
int g_vguiToggles = 0;
void ToggleVguiDraw() {
    ++g_vguiToggles;
    float before = -1.0f, after = -1.0f;
    const bool readOk = TryReadCvarFloat("r_drawvgui", before);
    const float want = (readOk && before != 0.0f) ? 0.0f : 1.0f;
    const bool setOk = readOk && TrySetCvarFloat("r_drawvgui", want);
    const bool readBack = TryReadCvarFloat("r_drawvgui", after);
    char line[240]{};
    std::snprintf(line, sizeof(line),
                  "[TF2VR] VGUI F5 #%d: r_drawvgui found=%d before=%.0f wrote=%.0f ok=%d readback=%.0f (%s)\n",
                  g_vguiToggles, readOk ? 1 : 0, before, want, setOk ? 1 : 0, readBack ? after : -1.0f,
                  !readOk ? "CVAR NOT FOUND -- test void" : (after == 0.0f ? "VGUI OFF: anything still visible is NOT VGUI" : "VGUI ON"));
    Tf2VrLog(line);
}

void ToggleLockHudViewFix() {
    ++g_dumps;
    g_on = !g_on;
    const Zoom zm = ReadZoom();
    char line[320]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] RING F3 #%d: zoom undo -> %s | Av=(%.0f,%.0f) inset=(%.0f,%.0f) z=%.3f k=%.2f ok=%d | setpos ring=%llu setsize ring=%llu applied=%llu skipped=%llu\n",
        g_dumps, g_on ? "ON" : "OFF", zm.avx, zm.avy, zm.sx, zm.sy, zm.z, g_ringScale, zm.ok ? 1 : 0,
        static_cast<unsigned long long>(g_setPosRing), static_cast<unsigned long long>(g_setSizeRing),
        static_cast<unsigned long long>(g_applied), static_cast<unsigned long long>(g_skipped));
    Tf2VrLog(line);
}

// THE ROOT FIX: DO NOT HOLD HUD POINTERS ACROSS A LEVEL CHANGE.
//
// Four crashes, four different bad addresses, one shape. The table holds up to
// 128 ring nodes and up to 256 other HUD nodes and NEVER PRUNES THEM. A level
// change frees every one of those objects and we keep reading them, every two
// seconds, forever. The addresses tell the story of me chasing it the wrong way:
//
//   0xFFFFFFFFFFFFFFFF   obvious garbage -- refused by an arithmetic check
//   0x300000000004       canonical but unmapped -- refused by asking the kernel
//   0x1EAFE583960        a REAL heap address, in a region the cache had
//                        validated, freed by the transition afterwards
//
// The third one ends the guard argument. No validation is sound when the answer
// can go stale between the check and the read, and a cached answer goes stale
// exactly at a level change -- which is the moment being tested. Guarding harder
// was the wrong axis; the bug is that we hold the pointers at all.
//
// So the tables are dropped the moment the world goes away. The lookup hook
// repopulates them when the next HUD is built, which is where they came from in
// the first place. The generation counter cannot do this job: it only bumps when
// a KNOWN NAME returns a NEW node, so between a level unloading and the next one
// building its HUD it never fires, and that gap is the whole window.
void ForgetHudNodes(const char* why) {
    g_ringCount = 0;
    g_ringVCount = 0;
    g_otherCount = 0;
    g_resolvedRings = 0;
    for (int r = 0; r < kMaxRings; ++r) {
        g_rings[r].panel = nullptr;
        g_rings[r].vpanel = nullptr;
        g_ringV[r] = nullptr;
        g_gameW[r] = 0; g_gameH[r] = 0; g_nodeX[r] = 0; g_nodeY[r] = 0;
    }
    for (int i = 0; i < kMaxOthers; ++i) g_otherNode[i] = nullptr;
    ForgetValidatedRegions();
    char line[200]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] RINGS: dropped every cached HUD node (%s). They are re-learned from the "
        "lookup hook when the next HUD is built.\n", why);
    Tf2VrLog(line);
}

void AdvanceLockHud(bool worldReady) {
    // The world→no-world edge is the level change. Everything cached here
    // belongs to the level that is going away.
    static bool hadWorld = false;
    if (hadWorld && !worldReady) ForgetHudNodes("the level unloaded");
    hadWorld = worldReady;

    // SUBDIVIDED 2026-09-08. The enclosing segment measured a WORST FRAME of
    // 60-158 ms against a 12.50 ms budget, alternating between two magnitudes
    // -- periodic, not scene-driven. These three marks split this function into
    // its parts; whatever is left lands in lh.STATUS+CENSUS, the 5 s block.
    Install();
    PluginCost::FrameSeg(12);
    if (!g_installed) return;
    if (g_ringCount > 0) ResolveRingVPanels();
    PluginCost::FrameSeg(13);
    // 2026-09-07: unconditional once vgui2 is loaded. Gating it on the lookup
    // seam left the census at a fake zero for a whole run.
    InstallHooks();
    PluginCost::FrameSeg(14);
    // THE 5 s STATUS BLOCK IS A DIAGNOSTIC AND IT COST 102-158 ms IN ONE FRAME
    // (segment lh.STATUS+CENSUS, 2026-09-08). It is off unless asked for. A
    // diagnostic that ships must be free when off, not merely cheap.
    if (!g_censusOn) return;
    const std::uint64_t now = GetTickCount64();
    if (now - g_lastTick < 5000) return;
    const double secs = g_lastTick ? static_cast<double>(now - g_lastTick) / 1000.0 : 5.0;
    g_lastTick = now;
    const std::uint64_t dTicks = g_hookTicks - g_lastStatusHookTicks;
    const std::uint64_t dRing = g_setPosRing - g_lastStatusRing;
    g_lastStatusHookTicks = g_hookTicks;
    g_lastStatusRing = g_setPosRing;
    const double usPerSec = static_cast<double>(dTicks) * 1e6 / static_cast<double>(QpcFreq()) / secs;
    const Zoom zm = ReadZoom();
    char line[640]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] RINGS: %s | held=%d vpanels=%d | hooks %s | zoom undo %s C=(%.0f,%.0f) inset=(%.0f,%.0f) Av=(%.0f,%.0f) z=%.3f k=%.2f ok=%d | setpos total=%llu ring=%llu (%.0f/s) | setsize total=%llu ring=%llu | applied=%llu skipped=%llu | identityFails=%llu | HOOK COST %.0f us/s | F3=%d\n",
        g_installed ? "wrapped" : (g_refused ? "REFUSED" : "not installed yet"), g_ringCount, g_resolvedRings,
        g_hooksInstalled ? "SWAPPED" : (g_hooksRefused ? "REFUSED" : "not yet"), g_on ? "ON" : "OFF",
        g_centreX, g_centreY, zm.sx, zm.sy, zm.avx, zm.avy, zm.z, g_ringScale, zm.ok ? 1 : 0,
        static_cast<unsigned long long>(g_setPosCalls), static_cast<unsigned long long>(g_setPosRing),
        static_cast<double>(dRing) / secs,
        static_cast<unsigned long long>(g_setSizeCalls), static_cast<unsigned long long>(g_setSizeRing),
        static_cast<unsigned long long>(g_applied), static_cast<unsigned long long>(g_skipped), static_cast<unsigned long long>(g_identityFails), usPerSec, g_dumps);
    Tf2VrLog(line);
    PrintCensus(secs);
}

// lockhud.census: the 5 s ring status line and the mover census. OFF by
// default -- measured at 102-158 ms in the frame it lands on.
void SetLockHudCensusEnabled(bool enabled) { g_censusOn = enabled; }
