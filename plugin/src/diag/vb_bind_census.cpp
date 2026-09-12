#include "vb_bind_census.h"

#include "arms_collapse.h"
#include "diagnostics.h"
#include "mesh_census.h"
#include "titan_state.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

// The thunk in studio_draw.asm and the three words it talks to. It records the
// draw descriptor (r8) and tail-jumps to the original: no frame, no argument
// forwarding, so its transparency does not depend on getting the arity right.
extern "C" {
void StudioDrawThunk();
void* g_studioDrawOrig = nullptr;
std::uint64_t g_studioDrawCalls = 0;
void* g_studioDrawDesc = nullptr;
// 64 slots of {thread id, descriptor}, keyed on (tid >> 2) & 63 -- Windows
// thread ids are multiples of 4, so masking the raw id gave only four live
// slots and every reading collided. The tid is stored alongside so that a
// collision is detected rather than believed. (TlsAlloc was tried first and
// returned index 87, past the TEB.s 64 inline slots, voiding another run.)
std::uint64_t g_studioDescTable[128] = {};
}

namespace {

bool ReadableAddress(std::uintptr_t a, std::size_t n);

constexpr std::size_t kIASetVertexBuffersSlot = 18;
using IASetVertexBuffersFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*,
                                                    const UINT*);
IASetVertexBuffersFn g_orig = nullptr;
void** g_slot = nullptr;
void* g_slotOriginal = nullptr;
std::atomic<int> g_active{0};

// Buffer -> vertex count cache (one GetDesc per distinct buffer/stride pair).
constexpr int kCacheSize = 4096;
struct CacheEntry {
    std::atomic<std::uintptr_t> key{0};  // buffer pointer ^ (stride << 48)
    std::uint32_t verts;
    std::uint32_t byteWidth;
};
CacheEntry g_cache[kCacheSize];
std::atomic<int> g_cacheUsed{0};
std::atomic<std::uint64_t> g_cacheFull{0};

// Sites that bound an arms buffer: return address, mesh, stride, count.
//
// PROVENANCE (2026-09-06). The 09-05 run named the binder -- every arms mesh
// binds from materialsystem_dx11.dll+0x1C59A, inside the leaf helper +0x1C410.
// Offline that helper has THIRTEEN callers, and the only one big enough to
// enumerate meshes (+0x1C970, 1083 bytes) is itself entered from two different
// render-context vtable slots: 90 (via the arg-shifting thunk +0x729D0) and
// 102 (+0x72BC0), plus two non-vtable thunks. Reading arguments to tell them
// apart is what crashed the game on slot 172, so this build reads NO
// arguments: at the bind it records the return-address chain and, separately,
// a bounded raw scan of the stack. Both are read-only and both are capped to
// the first few sites, so the cost is a few microseconds ONCE per run.
//
// Two instruments, because they fail differently: the unwind walk is exact but
// truncates at the .pdata-less thunks we found (+0x729D0/+0x729E0/+0x60830),
// and a tail-jmp thunk leaves NO frame at all. The raw scan cannot be fooled
// that way but includes stale slots. Where they agree, the chain is real.
constexpr int kMaxSites = 48;
constexpr int kMaxFrames = 16;
struct Site {
    std::atomic<std::uintptr_t> ret{0};
    int mesh;
    std::uint32_t stride;
    std::uint32_t slotIndex;
    std::atomic<std::uint64_t> count{0};
    std::uintptr_t frames[kMaxFrames];
    int frameCount;
    std::uintptr_t raw[kMaxFrames];
    int rawCount;
    // Which studiorender draw descriptor this mesh was bound under, and the
    // shape of its candidate mesh array ([desc+8] -> {+4 count, +8 array}).
    std::uintptr_t studioDesc;
    std::uintptr_t studioArray;
    std::uint32_t studioCount;
    int studioSeen;
    int studioIsArms;
    std::uintptr_t studioGate;
    std::uint32_t gateValue[16];
    int gateRead;
    int studioLod;
    int studioFromTls;
    std::uintptr_t studioGlobalDesc;
    std::uintptr_t studioHdr;
    char studioName[64];
    std::uint32_t readerTid;
    std::uint32_t slotTid;
    std::uint32_t batchCount[16];
    std::uintptr_t batchArray[16];
};
Site g_sites[kMaxSites];
std::atomic<int> g_siteCount{0};
int g_sitesPrinted = 0;

std::atomic<std::uint64_t> g_calls{0}, g_buffers{0}, g_armsBinds{0}, g_faults{0};
std::atomic<std::uint64_t> g_meshBinds[16]{};

std::uint32_t VertsOf(ID3D11Buffer* buffer, UINT stride, std::uint32_t* byteWidthOut) {
    *byteWidthOut = 0;
    if (!buffer || stride == 0) return 0;
    const std::uintptr_t key = reinterpret_cast<std::uintptr_t>(buffer) ^ (static_cast<std::uintptr_t>(stride) << 48);
    std::size_t h = (reinterpret_cast<std::uintptr_t>(buffer) >> 4) & (kCacheSize - 1);
    for (int probe = 0; probe < 16; ++probe) {
        CacheEntry& e = g_cache[(h + probe) & (kCacheSize - 1)];
        const std::uintptr_t k = e.key.load(std::memory_order_acquire);
        if (k == key) { *byteWidthOut = e.byteWidth; return e.verts; }
        if (k == 0) {
            D3D11_BUFFER_DESC desc{};
            buffer->GetDesc(&desc);
            e.verts = desc.ByteWidth / stride;
            e.byteWidth = desc.ByteWidth;
            std::uintptr_t expected = 0;
            if (e.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel)) {
                g_cacheUsed.fetch_add(1, std::memory_order_relaxed);
            }
            *byteWidthOut = desc.ByteWidth;
            return desc.ByteWidth / stride;
        }
    }
    g_cacheFull.fetch_add(1, std::memory_order_relaxed);
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    *byteWidthOut = desc.ByteWidth;
    return desc.ByteWidth / stride;
}

// --- studiorender model draw (2026-09-06) --------------------------------
//
// The 09-06 provenance run put the arms bind under this chain, every frame
// verified against the binary (each return address lands inside the named
// function's .pdata extent):
//
//   studiorender.dll+0x15D10  (virtual; .rdata vtable entry at +0x65A10)
//     -> +0x12380 -> +0xDE10 -> +0xDCC0 -> +0xD810
//       -> materialsystem_dx11+0x72F30 -> +0x1E480 -> +0x1C410 -> IASetVB
//
// Everything below +0x15D10 is single-caller and non-virtual, so +0x15D10 is
// the one hookable point covering the whole subtree. It is reached by ZERO
// direct calls -- only through the vtable -- so swapping the .rdata entry
// catches every arms draw.
//
// ARGUMENTS. rcx = this, rdx = an out-param (zeroed at entry via
// `mov [rdx],rbp`), r8 = the draw descriptor (validated by the routine
// itself: [r8] and [r8+8] non-null, [[r8+8]+4] non-zero, [[r8+8]+8]
// non-null), r9 = a fourth arg. It ALSO reads `mov ebp,[rsp+0x118]` on both
// branches, which with the frame 0xE8 below entry rsp is [entry+0x30] -- a
// SIXTH argument, arriving on the stack, that steers the draw.
//
// That sixth argument cost the wearer a run on 2026-09-06: a C++ wrapper
// forwarded four arguments, the routine read garbage for the sixth, and the
// model renderer was entered 33237 times drawing NOTHING -- gun, body and
// arms all gone, only the RUI ammo counter left. The check that missed it
// listed the [rsp+...] offsets and sorted them as TEXT, so "[rsp+0x118]"
// sorted before "[rsp+0x30]" and fell off the end of the listing. The
// evidence was there; the sort hid it.
//
// The wrapper is now the tail-jump thunk in studio_draw.asm, which rebuilds
// no call frame at all, so no argument -- register or stack -- can be lost
// however many there turn out to be.
//
// This build only WATCHES: it records the descriptor so the bind census can
// say which descriptor the arms are drawn under, and how many entries the
// candidate mesh array holds. It changes nothing.
// RETIRED 2026-09-06. The arms never came through this entry -- they are
// replayed from a queued command record that calls +0x12380 directly (see
// RecoverDescriptorByUnwind). Six runs were spent here. The descriptor now
// comes off the stack, so this build swaps NO vtable and modifies nothing.
constexpr bool kStudioDrawEnabled = false;
constexpr std::uintptr_t kStudioDrawRva = 0x15D10;
constexpr std::uintptr_t kStudioVtableEntryRva = 0x65A10;
std::atomic<int> g_studioState{0};  // 0 = not tried, 1 = installed, 2 = refused

// Retried from the tick until studiorender.dll is loaded. Refuses unless the
// slot still holds the exact function we read offline -- an identity match,
// not a displacement match.
void InstallStudioDrawHook() {
    if (!kStudioDrawEnabled) return;
    if (g_studioState.load(std::memory_order_acquire)) return;
    HMODULE sr = GetModuleHandleA("studiorender.dll");
    if (!sr) return;  // not loaded yet; try again next tick
    const auto base = reinterpret_cast<std::uintptr_t>(sr);
    void** slot = reinterpret_cast<void**>(base + kStudioVtableEntryRva);
    void* const want = reinterpret_cast<void*>(base + kStudioDrawRva);
    char line[260];
    if (*slot != want) {
        std::snprintf(line, sizeof(line),
                      "[TF2VR] STUDIODRAW: vtable +0x%llX holds %p, expected base+0x%llX (%p); NOT installed -- the "
                      "offline slot is wrong for this build, so a zero below is instrument failure.\n",
                      static_cast<unsigned long long>(kStudioVtableEntryRva), *slot,
                      static_cast<unsigned long long>(kStudioDrawRva), want);
        Tf2VrLog(line);
        g_studioState.store(2, std::memory_order_release);
        return;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        Tf2VrLog("[TF2VR] STUDIODRAW: vtable slot would not go writable; NOT installed.\n");
        g_studioState.store(2, std::memory_order_release);
        return;
    }
    g_studioDrawOrig = *slot;
    *slot = reinterpret_cast<void*>(&StudioDrawThunk);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    g_studioState.store(1, std::memory_order_release);
    Tf2VrLog("[TF2VR] STUDIODRAW: installed on studiorender.dll's class vtable (+0x65A10 -> +0x15D10) as the assembly tail-jump thunk, which "
             "rebuilds no call frame and so forwards every argument, stack ones included.\n");
}

bool ReadableAddressFwd(std::uintptr_t a, std::size_t n);

// The queued draw path (2026-09-06, established by the full 15-frame unwind).
//
// The arms are NOT drawn through studiorender's +0x15D10 vtable entry. They
// are drawn from a QUEUED COMMAND RECORD replayed on the render thread:
//
//   materialsystem_dx11+0x87F20  (command loop)
//     -> studiorender+0x13AF0    (63-byte replay dispatcher)
//          r10 = [rcx+8] + [rcx+0x14]      the 0xB8-byte record
//          rdx = r10+0x10                  the draw descriptor, IN the record
//          call [r10]                      the record's own function pointer
//     -> studiorender+0x12380 -> +0xDE10 -> ... -> IASetVertexBuffers
//
// The record's function pointer is +0x12380 itself, so +0x15D10 never appears
// and no vtable slot on this path can be swapped. Six runs were spent hooking
// +0x15D10, which only ever ran on a DIFFERENT thread (16552 against the
// binding thread 7536) -- the per-thread descriptor slot was empty, not
// collided, and said so every time.
//
// Nothing needs to be hooked. +0x12380 keeps the descriptor in r15, which is
// non-volatile, so the unwinder that already walks this stack reliably can
// simply be asked for it: unwind out to the +0x12380 frame and read R15 from
// the reconstructed context. This works on whichever path drew the arms,
// queued or immediate, and modifies nothing in the game.
std::uintptr_t RecoverDescriptorByUnwind(int* framesWalked, int* foundFrame) {
    *framesWalked = 0;
    *foundFrame = -1;
    HMODULE sr = GetModuleHandleA("studiorender.dll");
    if (!sr) return 0;
    const auto srBase = reinterpret_cast<DWORD64>(sr);
    const DWORD64 lo = srBase + 0x12380, hi = srBase + 0x124DA;
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    for (int i = 0; i < 40 && ctx.Rip; ++i) {
        *framesWalked = i + 1;
        if (ctx.Rip >= lo && ctx.Rip < hi) {
            *foundFrame = i;
            return static_cast<std::uintptr_t>(ctx.R15);
        }
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!rf) {
            // Leaf frame: no unwind data, so the return address is at [rsp].
            if (!ReadableAddressFwd(static_cast<std::uintptr_t>(ctx.Rsp), 8)) return 0;
            ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
        }
        PVOID handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, rf, &ctx, &handlerData, &establisher, nullptr);
    }
    return 0;
}

// THE FILTER (2026-09-06). Proven reachable by the run that read all five
// gates live off the arms model's own array.
//
// studiorender.dll+0xE06A skips a mesh outright when the first dword of its
// gate entry is zero:
//     r14 = [ctx+0x28] + meshid*16 ; cmp dword [r14],0 ; je (next mesh)
// so hiding a mesh is one dword, and the engine does the skipping itself --
// no bind, no draw, and nothing shared with the militia grunts is touched
// (which is what sank the per-material lever).
//
// Driven from the GLOVE's bind, deliberately: the glove is never hidden, so
// the write keeps re-applying every model draw and cannot switch itself off
// the way keying on a hidden mesh would. The array is re-derived from the
// live descriptor each time, so a reallocation cannot leave us writing into
// a stale one.
// EVERY WRITE WE MAKE INTO GAME MEMORY, so a crash can be told apart from a
// coincidence (2026-09-06). The 09-06 titan-exit crash faulted in
// materialsystem's DrawItems on a NULL item descriptor, 1.4 s after a titan
// exit, on the first run with the job system back to six workers. The obvious
// question -- did one of OUR writes land in that window, and from which thread
// -- could not be answered: the periodic counters said only that the totals had
// not moved in the last second.
//
// This ring answers it exactly. Gate writes are rare (four in a whole run), so
// recording tick, thread, address and old->new costs nothing, and the crash
// recorder prints the ring. A write microseconds before the fault on another
// thread is a race; the last write being minutes old exonerates us.
struct GameWrite {
    std::uint64_t tick;
    std::uint32_t thread;
    std::uintptr_t address;
    std::uint32_t before;
    std::uint32_t after;
};
constexpr int kGameWriteRing = 16;
GameWrite g_gameWrites[kGameWriteRing]{};
std::atomic<std::uint32_t> g_gameWriteSeq{0};

void NoteGameWrite(std::uintptr_t address, std::uint32_t before, std::uint32_t after) {
    const std::uint32_t n = g_gameWriteSeq.fetch_add(1, std::memory_order_relaxed);
    GameWrite& w = g_gameWrites[n % kGameWriteRing];
    w.tick = GetTickCount64();
    w.thread = GetCurrentThreadId();
    w.address = address;
    w.before = before;
    w.after = after;
}

constexpr bool kGateFilterEnabled = true;
std::atomic<std::uint64_t> g_gateWrites{0}, g_gateRestores{0}, g_gateRefused{0};
std::uintptr_t GateArrayFromStack(int* modelSlot);
std::uint32_t g_gateOriginal[kMaxArmsModels][16]{};
int g_gateOriginalKnown[kMaxArmsModels]{};
int g_gateLastWanted = -1;

// The same derivation the reading run proved, in one place:
//   desc (r15 at the +0x12380 frame) -> [desc+8] LOD data -> its +8 per-LOD
//   table -> + LOD*16, with LOD the clamped byte the routine stores at
//   [desc+0x2A]. Refuses unless the model is THIS one, by name.
std::uintptr_t GateArrayFromStack(int* modelSlot) {
    *modelSlot = -1;
    int walked = 0, foundAt = -1;
    const std::uintptr_t desc = RecoverDescriptorByUnwind(&walked, &foundAt);
    if (!desc || foundAt < 0 || !ReadableAddress(desc, 0x30)) return 0;
    const auto hdr = *reinterpret_cast<const std::uintptr_t*>(desc);
    if (!hdr || !ReadableAddress(hdr, 0x50)) return 0;
    const char* nm = reinterpret_cast<const char*>(hdr + 0x10);
    char name[64];
    int i = 0;
    for (; i < 63 && nm[i] >= 0x20 && nm[i] < 0x7F; ++i) name[i] = nm[i];
    name[i] = '\0';
    // Match ANY published arms model by its own name -- there are two, and the
    // one without the _rifleman suffix is what draws after a titan exit.
    for (int k = 0; k < g_armsModelCount; ++k) {
        if (g_armsModels[k].valid && std::strcmp(g_armsModels[k].name, name) == 0) { *modelSlot = k; break; }
    }
    if (*modelSlot < 0) return 0;
    const auto inner = *reinterpret_cast<const std::uintptr_t*>(desc + 8);
    if (!inner || !ReadableAddress(inner, 0x10)) return 0;
    const auto table = *reinterpret_cast<const std::uintptr_t*>(inner + 8);
    const auto lod = *reinterpret_cast<const std::uint8_t*>(desc + 0x2A);
    if (!table || lod >= 64 || !ReadableAddress(table + static_cast<std::size_t>(lod) * 16, 8)) return 0;
    return *reinterpret_cast<const std::uintptr_t*>(table + static_cast<std::size_t>(lod) * 16);
}

// WHICH MODELS ACTUALLY DRAW (2026-09-06). After a titan exit the wearer saw a
// full body while the filter's three meshes bound ZERO times all run -- so the
// body cannot be this model, and nothing told us what it was: the bone dump
// walks one model and then stands down.
//
// So sample the draw path itself. Every 4096th bound buffer, unwind to the
// +0x12380 frame and read the model's name from its own studiohdr. That is
// ~2400 unwinds across a long run, a few microseconds each, and it enumerates
// every model reaching this path -- the post-titan body included. Read-only.
constexpr int kMaxModelsSeen = 24;
struct ModelSeen {
    char name[64];
    std::uint32_t verts;
    std::uint64_t hits;
};
ModelSeen g_modelsSeen[kMaxModelsSeen]{};
int g_modelsSeenCount = 0;
std::uint64_t g_modelSampleTick = 0;
int g_modelsPrinted = 0;

void SampleModelName(std::uint32_t verts) {
    int walked = 0, foundAt = -1;
    const std::uintptr_t desc = RecoverDescriptorByUnwind(&walked, &foundAt);
    if (!desc || foundAt < 0 || !ReadableAddress(desc, 0x30)) return;
    const auto hdr = *reinterpret_cast<const std::uintptr_t*>(desc);
    if (!hdr || !ReadableAddress(hdr, 0x50)) return;
    const char* nm = reinterpret_cast<const char*>(hdr + 0x10);
    char name[64];
    int i = 0;
    for (; i < 63 && nm[i] >= 0x20 && nm[i] < 0x7F; ++i) name[i] = nm[i];
    name[i] = '\0';
    if (!name[0]) return;
    for (int k = 0; k < g_modelsSeenCount; ++k) {
        if (std::strcmp(g_modelsSeen[k].name, name) == 0) {
            ++g_modelsSeen[k].hits;
            return;
        }
    }
    if (g_modelsSeenCount >= kMaxModelsSeen) return;
    ModelSeen& e = g_modelsSeen[g_modelsSeenCount];
    std::memcpy(e.name, name, sizeof(name));
    e.verts = verts;
    e.hits = 1;
    ++g_modelsSeenCount;
}

void ApplyGateFilter(std::uintptr_t gate, int modelSlot) {
    if (!kGateFilterEnabled || !gate) return;
    if (modelSlot < 0 || modelSlot >= kMaxArmsModels || !g_armsModels[modelSlot].valid) return;
    const ArmsModelMeshes& am = g_armsModels[modelSlot];
    // NEVER HIDE A WHOLE MODEL. This filter keeps the gloves and hides the
    // rest, so a model with no glove mesh has nothing to keep and would be
    // erased entirely. Weapons/arms/buddypov.mdl is published into this table
    // (2 meshes, neither a gauntlet) and cannot trigger the filter today --
    // its vertex counts match no glove -- but "cannot today" is not a reason
    // to leave the erase reachable.
    int gloves = 0;
    for (int m = 0; m < am.meshCount && m < 16; ++m) gloves += am.isGlove[m] ? 1 : 0;
    if (gloves == 0) { g_gateRefused.fetch_add(1, std::memory_order_relaxed); return; }
    // Mode 1 = weapon + gloves, on foot only; full body inside a titan.
    //
    // TitanStateSettled() matters for the SAVE-LOAD case and is not decorative:
    // IsInTitanNow() fails safe to "on foot", so a save that begins inside a
    // titan would read false for its first frames and this would hide the
    // pilot's meshes in the cockpit. The bone-collapse path has always waited
    // for the settle (arms_collapse.cpp, the `!inTitan && TitanStateSettled()`
    // arm); the mesh filter now waits with it. Not settled = hide nothing,
    // which is the safe direction: too much body, never too little.
    const bool wanted = (BodyShowMode() == 1) && TitanStateSettled() && !IsInTitanNow();
    for (int m = 0; m < am.meshCount && m < 16; ++m) {
        const auto entry = gate + static_cast<std::size_t>(am.meshId[m]) * 16;
        if (!ReadableAddress(entry, 4)) { g_gateRefused.fetch_add(1, std::memory_order_relaxed); return; }
        auto* p = reinterpret_cast<std::uint32_t*>(entry);
        if (!g_gateOriginalKnown[modelSlot]) g_gateOriginal[modelSlot][m] = *p;  // before the first write
        if (am.isGlove[m]) continue;                                            // gloves always draw
        const std::uint32_t want = wanted ? 0u : g_gateOriginal[modelSlot][m];
        if (*p == want) continue;
        NoteGameWrite(entry, *p, want);
        *p = want;
        (wanted ? g_gateWrites : g_gateRestores).fetch_add(1, std::memory_order_relaxed);
    }
    g_gateOriginalKnown[modelSlot] = 1;
    g_gateLastWanted = wanted ? 1 : 0;
}

// A committed, readable range -- used to REJECT a stale descriptor instead of
// faulting on it. That matters: HookIASetVertexBuffers stops censusing once
// g_faults is non-zero, so one swallowed fault would silently retire the
// instrument for the rest of the run.
bool ReadableAddress(std::uintptr_t a, std::size_t n) {
    if (a < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const auto end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return a + n <= end;
}

bool ReadableAddressFwd(std::uintptr_t a, std::size_t n) { return ReadableAddress(a, n); }

// True only for a committed, executable address -- keeps data words out of the
// raw stack scan. Capped calls only (see CaptureProvenance).
bool IsCodeAddress(std::uintptr_t a) {
    if (a < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD p = mbi.Protect;
    return (p & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

using CaptureFn = USHORT(WINAPI*)(ULONG, ULONG, PVOID*, PULONG);
CaptureFn g_capture = nullptr;
bool g_captureResolved = false;

// Fills a site's two provenance records. Called ONCE per site (at most
// kMaxSites times in a run), before the site is published.
void CaptureProvenance(Site& s) {
    s.frameCount = 0;
    s.rawCount = 0;
    s.studioDesc = 0;
    s.studioArray = 0;
    s.studioCount = 0;
    s.studioSeen = 0;
    s.studioIsArms = 0;
    s.studioGate = 0;
    s.gateRead = 0;
    s.studioLod = 0;
    s.studioFromTls = 0;
    s.studioGlobalDesc = 0;
    s.studioHdr = 0;
    s.studioName[0] = 0;
    s.readerTid = 0;
    s.slotTid = 0;
    for (int m = 0; m < 16; ++m) { s.batchCount[m] = 0; s.batchArray[m] = 0; }
    for (int m = 0; m < 16; ++m) s.gateValue[m] = 0;
    __try {
        // The descriptor of the studiorender draw we are nested inside, if the
        // hook took. Same thread, synchronous call, so this is exact.
        // Straight off this stack: unwind out to the +0x12380 frame and take
        // its r15. No hook, no thread bookkeeping, no shared state -- and it
        // works on the queued path as well as the immediate one.
        int walked = 0, foundAt = -1;
        const std::uintptr_t unwound = RecoverDescriptorByUnwind(&walked, &foundAt);
        s.readerTid = static_cast<std::uint32_t>(GetCurrentThreadId());
        s.slotTid = static_cast<std::uint32_t>(walked);
        s.studioFromTls = (foundAt >= 0) ? 1 : 0;
        s.studioGlobalDesc = static_cast<std::uintptr_t>(foundAt);
        if (void* d = reinterpret_cast<void*>(unwound)) {
            s.studioDesc = reinterpret_cast<std::uintptr_t>(d);
            s.studioSeen = 1;
            // [desc+0] is the studiohdr (studiorender.dll+0x123D7/+0x123DD copies
            // it into the draw context at +0x18). Matching it against the model
            // mesh_census walked is this reading's gate: g_studioDrawDesc is a
            // plain global the thunk writes on every model draw, so a value that
            // is NOT the arms header means we raced another draw and the gate
            // numbers below describe some other model.
            if (!ReadableAddress(s.studioDesc, 0x30)) { s.studioSeen = 2; return; }
            const auto hdr = *reinterpret_cast<const std::uintptr_t*>(s.studioDesc + 0);
            s.studioHdr = hdr;
            // Identify by the model's OWN NAME (studiohdr +0x10 in v53, the
            // offset arms_collapse.cpp already uses), not by pointer equality:
            // the header mesh_census walked comes from the client-side bone
            // table, which need not be the same object studiorender draws
            // from, so a pointer mismatch proves nothing either way.
            if (hdr && ReadableAddress(hdr, 0x50)) {
                const char* nm = reinterpret_cast<const char*>(hdr + 0x10);
                int i = 0;
                for (; i < 63 && nm[i] >= 0x20 && nm[i] < 0x7F; ++i) s.studioName[i] = nm[i];
                s.studioName[i] = '\0';
            }
            s.studioIsArms = (s.studioName[0] && std::strstr(s.studioName, "pov_mlt_hero_jack_rifleman") != nullptr) ? 1 : 0;
            const auto inner = *reinterpret_cast<const std::uintptr_t*>(s.studioDesc + 8);
            if (inner && ReadableAddress(inner, 0x10)) {
                s.studioCount = *reinterpret_cast<const std::uint32_t*>(inner + 4);  // LOD count
                s.studioArray = *reinterpret_cast<const std::uintptr_t*>(inner + 8); // per-LOD table
                // The gate array studiorender.dll+0xE066 indexes:
                //   gate = [[[desc+8]+8] + LOD*16], entry = gate + meshid*16,
                //   and +0xE06A skips the mesh when *(dword*)entry == 0.
                const auto lod = *reinterpret_cast<const std::uint8_t*>(s.studioDesc + 0x2A);
                s.studioLod = lod;
                if (s.studioArray && lod < 64 && ReadableAddress(s.studioArray + static_cast<std::size_t>(lod) * 16, 8)) {
                    s.studioGate = *reinterpret_cast<const std::uintptr_t*>(s.studioArray +
                                                                           static_cast<std::size_t>(lod) * 16);
                    if (s.studioGate && ReadableAddress(s.studioGate, 16 * 64)) {
                        for (int m = 0; m < g_armsMeshCount && m < 16; ++m) {
                            const auto e = s.studioGate + static_cast<std::size_t>(g_armsMeshId[m]) * 16;
                            s.gateValue[m] = *reinterpret_cast<const std::uint32_t*>(e);
                            // SUB-MESH DUE DILIGENCE. The entry is {count, items@+8};
                            // the item carries ANOTHER count at +0x28 with its array at
                            // +0x30, and studiorender+0xD810 calls the vertex-buffer bind
                            // (render-context slot 96) inside THAT loop -- +0xD91A, the
                            // return address in every one of our unwind chains, is that
                            // call returning. If the gauntlet is more than one batch, a
                            // batch may be the forearm. If it is one, this avenue is shut.
                            const auto item = *reinterpret_cast<const std::uintptr_t*>(e + 8);
                            if (item && ReadableAddress(item, 0x40)) {
                                s.batchCount[m] = *reinterpret_cast<const std::uint32_t*>(item + 0x28);
                                s.batchArray[m] = *reinterpret_cast<const std::uintptr_t*>(item + 0x30);
                            }
                        }
                        s.gateRead = 1;
                    }
                }
            }
        }
        if (!g_captureResolved) {
            g_captureResolved = true;
            if (HMODULE nt = GetModuleHandleA("ntdll.dll")) {
                g_capture = reinterpret_cast<CaptureFn>(
                    reinterpret_cast<void*>(GetProcAddress(nt, "RtlCaptureStackBackTrace")));
            }
        }
        if (g_capture) {
            PVOID frames[kMaxFrames]{};
            const USHORT got = g_capture(1, kMaxFrames, frames, nullptr);
            for (USHORT i = 0; i < got && i < kMaxFrames; ++i) {
                s.frames[s.frameCount++] = reinterpret_cast<std::uintptr_t>(frames[i]);
            }
        }
        // Bounded raw scan: at most 384 qwords up from this frame, never past
        // the thread's own stack base, keeping executable addresses only.
        const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
        const auto base = reinterpret_cast<std::uintptr_t>(tib->StackBase);
        auto p = reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
        for (int i = 0; i < 384 && s.rawCount < kMaxFrames; ++i, p += sizeof(void*)) {
            if (p + sizeof(void*) > base) break;
            const std::uintptr_t v = *reinterpret_cast<const std::uintptr_t*>(p);
            if (!IsCodeAddress(v)) continue;
            bool dup = false;
            for (int k = 0; k < s.rawCount; ++k) {
                if (s.raw[k] == v) { dup = true; break; }
            }
            if (!dup) s.raw[s.rawCount++] = v;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteSite(std::uintptr_t ret, int mesh, UINT stride, UINT slotIndex) {
    const int n = g_siteCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        if (g_sites[i].ret.load(std::memory_order_relaxed) == ret && g_sites[i].mesh == mesh) {
            g_sites[i].count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    if (n >= kMaxSites) return;
    int slot = n;
    if (!g_siteCount.compare_exchange_strong(slot, slot + 1)) return;
    g_sites[slot].mesh = mesh;
    g_sites[slot].stride = stride;
    g_sites[slot].slotIndex = slotIndex;
    g_sites[slot].count.store(1);
    // Everything the printer reads must be written BEFORE the release-store of
    // ret, which is what publishes the site to the main thread.
    CaptureProvenance(g_sites[slot]);
    g_sites[slot].ret.store(ret, std::memory_order_release);
}

void Census(UINT startSlot, UINT numBuffers, ID3D11Buffer* const* buffers, const UINT* strides, std::uintptr_t ret) {
    __try {
        if (!buffers || !strides || numBuffers == 0 || numBuffers > 32) return;
        for (UINT i = 0; i < numBuffers; ++i) {
            g_buffers.fetch_add(1, std::memory_order_relaxed);
            std::uint32_t byteWidth = 0;
            const std::uint32_t verts = VertsOf(buffers[i], strides[i], &byteWidth);
            // Sampled model-name census: one unwind every 4096 buffers.
            if (verts > 200 && (++g_modelSampleTick & 4095) == 0) SampleModelName(verts);
            if (!verts || !g_armsHeaderValid) continue;
            for (int m = 0; m < g_armsMeshCount && m < 16; ++m) {
                if (g_armsMeshVertexCount[m] == static_cast<int>(verts) && byteWidth == verts * strides[i]) {
                    g_armsBinds.fetch_add(1, std::memory_order_relaxed);
                    g_meshBinds[m].fetch_add(1, std::memory_order_relaxed);
                    NoteSite(ret, m, strides[i], startSlot + i);
                    break;
                }
            }
            // THE FILTER TRIGGER, ACROSS EVERY PUBLISHED ARMS MODEL. Keyed on a
            // GLOVE bind, so it never keys on a mesh it has itself hidden. The
            // loop above only knows the first model walked; there are two, and
            // matching only that one is what left pov_mlt_hero_jack.mdl drawing
            // a full body after a titan exit.
            if (!kGateFilterEnabled) continue;
            for (int k = 0; k < g_armsModelCount; ++k) {
                const ArmsModelMeshes& am = g_armsModels[k];
                if (!am.valid) continue;
                bool glove = false;
                for (int m = 0; m < am.meshCount && m < 16; ++m) {
                    if (am.isGlove[m] && am.vertexCount[m] == static_cast<int>(verts) &&
                        byteWidth == verts * strides[i] && am.vertexCount[m] > 1000) {
                        glove = true;
                        break;
                    }
                }
                if (!glove) continue;
                int slot = -1;
                if (const std::uintptr_t g = GateArrayFromStack(&slot)) ApplyGateFilter(g, slot);
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
    }
}

void STDMETHODCALLTYPE HookIASetVertexBuffers(ID3D11DeviceContext* context, UINT startSlot, UINT numBuffers,
                                              ID3D11Buffer* const* buffers, const UINT* strides, const UINT* offsets) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_faults.load(std::memory_order_relaxed) == 0) {
        Census(startSlot, numBuffers, buffers, strides, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    }
    g_orig(context, startSlot, numBuffers, buffers, strides, offsets);
}

void Describe(std::uintptr_t rip, char* out, std::size_t cap) {
    struct Known { const char* name; HMODULE handle; };
    const Known known[] = {
        {"materialsystem_dx11.dll", GetModuleHandleA("materialsystem_dx11.dll")},
        {"engine.dll", GetModuleHandleA("engine.dll")},
        {"client.dll", GetModuleHandleA("client.dll")},
        {"studiorender.dll", GetModuleHandleA("studiorender.dll")},
        {"rtech_game.dll", GetModuleHandleA("rtech_game.dll")},
        {"d3d11.dll", GetModuleHandleA("d3d11.dll")},
        {"titanfall2vr.dll", GetModuleHandleA("titanfall2vr.dll")},
    };
    for (const Known& e : known) {
        if (!e.handle) continue;
        const auto b = reinterpret_cast<std::uintptr_t>(e.handle);
        if (rip > b && rip - b < 0x2000000) {
            std::snprintf(out, cap, "%s+0x%llX", e.name, static_cast<unsigned long long>(rip - b));
            return;
        }
    }
    std::snprintf(out, cap, "%016llX", static_cast<unsigned long long>(rip));
}

}  // namespace

void InstallVbBindCensus(ID3D11DeviceContext* context, void** vtable) {
    if (g_active.load(std::memory_order_acquire) || !context || !vtable) return;
    void** slot = &vtable[kIASetVertexBuffersSlot];
    if (*slot == reinterpret_cast<void*>(&HookIASetVertexBuffers)) {
        Tf2VrLog("[TF2VR] VBBIND: slot 18 already holds our hook; NOT installed.\n");
        g_active.store(1);
        return;
    }
    HMODULE d3d11 = GetModuleHandleA("d3d11.dll");
    MEMORY_BASIC_INFORMATION mbi{};
    if (!d3d11 || !VirtualQuery(*slot, &mbi, sizeof(mbi)) || mbi.AllocationBase != d3d11) {
        char line[240]{};
        std::snprintf(line, sizeof(line), "[TF2VR] VBBIND: slot 18 holds %p, which is not inside d3d11.dll; NOT installed.\n", *slot);
        Tf2VrLog(line);
        g_active.store(1);
        return;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        Tf2VrLog("[TF2VR] VBBIND: slot 18 would not go writable; NOT installed.\n");
        g_active.store(1);
        return;
    }
    g_orig = reinterpret_cast<IASetVertexBuffersFn>(*slot);
    g_slotOriginal = *slot;
    *slot = reinterpret_cast<void*>(&HookIASetVertexBuffers);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    g_slot = slot;
    g_active.store(1, std::memory_order_release);
    Tf2VrLog("[TF2VR] VBBIND: installed on the game context's IASetVertexBuffers (context-table slot 18), read-only; "
             "every bound buffer is sized once, and a buffer with an arms vertex count records its caller.\n");
}

void AdvanceVbBindCensus() {
    static std::uint64_t lastReport = 0;
    const std::uint64_t now = GetTickCount64();
    // Independent of the D3D census: this one needs no verified context, only
    // studiorender.dll to be loaded.
    InstallStudioDrawHook();
    if (!g_slot) {
        static std::uint64_t lastWait = 0;
        if (now - lastWait >= 5000) {
            lastWait = now;
            Tf2VrLog("[TF2VR] VBBIND: not installed yet -- no verified game context for a slot swap (the camera detours "
                     "must be installed first).\n");
        }
        return;
    }
    if (now - lastReport < 1000) return;
    lastReport = now;
    char line[520];
    int n = std::snprintf(line, sizeof(line),
                          "[TF2VR] VBBIND: %llu calls, %llu buffers, %d distinct sized, %llu arms binds, faults %llu, cache-full %llu; per mesh:",
                          static_cast<unsigned long long>(g_calls.load()), static_cast<unsigned long long>(g_buffers.load()),
                          g_cacheUsed.load(), static_cast<unsigned long long>(g_armsBinds.load()),
                          static_cast<unsigned long long>(g_faults.load()), static_cast<unsigned long long>(g_cacheFull.load()));
    for (int m = 0; m < g_armsMeshCount && m < 16; ++m) {
        n += std::snprintf(line + n, sizeof(line) - n, " %d=%llu", g_armsMeshVertexCount[m],
                           static_cast<unsigned long long>(g_meshBinds[m].load()));
    }
    n += std::snprintf(line + n, sizeof(line) - n, "; filter %s writes %llu restores %llu refused %llu; studiodraw %s x%llu",
                       g_gateLastWanted < 0 ? "NEVER APPLIED" : (g_gateLastWanted ? "hiding 3 meshes" : "passing all"),
                       static_cast<unsigned long long>(g_gateWrites.load()),
                       static_cast<unsigned long long>(g_gateRestores.load()),
                       static_cast<unsigned long long>(g_gateRefused.load()),
                       g_studioState.load() == 1 ? "installed" : (g_studioState.load() == 2 ? "REFUSED" : "not yet"),
                       static_cast<unsigned long long>(g_studioDrawCalls));
    std::snprintf(line + n, sizeof(line) - n, "%s\n",
                  (g_studioState.load() == 1 && g_studioDrawCalls == 0)
                      ? "  STUDIODRAW INSTALLED BUT NEVER CALLED -- that is instrument failure, not a null."
                  : g_calls.load() == 0 ? "  ZERO CALLS -- the slot is not reached."
                  : (!g_armsHeaderValid) ? "  (arms header not yet walked)"
                  : (g_buffers.load() > 100000 && g_armsBinds.load() == 0) ? "  NO ARMS BUFFER BOUND YET." : "");
    Tf2VrLog(line);
    // Which threads actually enter the hooked +0x15D10, printed once. The
    // 09-06 runs put the arms bind and this hook on DIFFERENT threads while
    // the unwind showed studiorender frames on the bind's own stack -- those
    // cannot both be true, so list the callers rather than reason about them.
    static bool tidsPrinted = false;
    if (!tidsPrinted && g_studioDrawCalls > 1000) {
        tidsPrinted = true;
        char t[520];
        int tn = std::snprintf(t, sizeof(t), "[TF2VR] STUDIODRAW threads entering +0x15D10 (of %llu calls):",
                               static_cast<unsigned long long>(g_studioDrawCalls));
        int found = 0;
        for (int i = 0; i < 64; ++i) {
            const std::uint64_t stored = g_studioDescTable[i * 2];
            if (!stored) continue;
            ++found;
            tn += std::snprintf(t + tn, sizeof(t) - tn, " %llu", static_cast<unsigned long long>(stored));
        }
        std::snprintf(t + tn, sizeof(t) - tn, "%s\n", found ? "" : "  NONE RECORDED -- the thunk never wrote the table.");
        Tf2VrLog(t);
    }
    // Every model the sampler has named, printed as they appear. The one that
    // draws a body after a titan exit is in here, and it is NOT the arms model
    // (whose three body meshes bound zero times that run).
    for (; g_modelsPrinted < g_modelsSeenCount && g_modelsPrinted < kMaxModelsSeen; ++g_modelsPrinted) {
        const ModelSeen& e = g_modelsSeen[g_modelsPrinted];
        char t[300];
        std::snprintf(t, sizeof(t), "[TF2VR] MODELSEEN[%d] \"%s\" first seen on a %u-vertex bind, samples %llu%s\n",
                      g_modelsPrinted, e.name, e.verts, static_cast<unsigned long long>(e.hits),
                      std::strstr(e.name, "pov_mlt_hero_jack_rifleman") ? "  (the filtered arms model)" : "");
        Tf2VrLog(t);
    }
    const int sn = g_siteCount.load(std::memory_order_acquire);
    for (; g_sitesPrinted < sn && g_sitesPrinted < kMaxSites; ++g_sitesPrinted) {
        const Site& s = g_sites[g_sitesPrinted];
        const std::uintptr_t ret = s.ret.load(std::memory_order_acquire);
        if (!ret) break;
        char where[96];
        Describe(ret, where, sizeof(where));
        std::snprintf(line, sizeof(line), "[TF2VR] VBBIND SITE[%d] %s binds arms mesh %d (%d verts%s) stride %u at input slot %u, x%llu\n",
                      g_sitesPrinted, where, s.mesh, g_armsMeshVertexCount[s.mesh], g_armsMeshIsGlove[s.mesh] ? ", glove" : "",
                      s.stride, s.slotIndex, static_cast<unsigned long long>(s.count.load()));
        Tf2VrLog(line);
        // Provenance, printed for the first two sites only -- the five arms
        // meshes share one binder, so their chains are the same answer.
        if (g_sitesPrinted >= 2) continue;
        char prov[2048];
        int pn = std::snprintf(prov, sizeof(prov), "[TF2VR] VBBIND CHAIN[%d] unwind %d frames:", g_sitesPrinted, s.frameCount);
        for (int f = 0; f < s.frameCount && f < kMaxFrames; ++f) {
            char w[96];
            Describe(s.frames[f], w, sizeof(w));
            pn += std::snprintf(prov + pn, sizeof(prov) - pn, " %s", w);
        }
        std::snprintf(prov + pn, sizeof(prov) - pn, "%s\n",
                      s.frameCount < 2 ? "  UNWIND TRUNCATED -- read the RAW line instead, not a null result." : "");
        Tf2VrLog(prov);
        pn = std::snprintf(prov, sizeof(prov), "[TF2VR] VBBIND RAW[%d] %d code words on the stack:", g_sitesPrinted, s.rawCount);
        for (int f = 0; f < s.rawCount && f < kMaxFrames; ++f) {
            char w[96];
            Describe(s.raw[f], w, sizeof(w));
            pn += std::snprintf(prov + pn, sizeof(prov) - pn, " %s", w);
        }
        std::snprintf(prov + pn, sizeof(prov) - pn, "%s\n",
                      s.rawCount == 0 ? "  SCAN FOUND NOTHING -- instrument failure, not an answer." : "");
        Tf2VrLog(prov);
        std::snprintf(prov, sizeof(prov),
                      "[TF2VR] VBBIND STUDIO[%d] %s desc %p, mesh array %p, count %u%s\n",
                      g_sitesPrinted, s.studioSeen ? "drawn under studiorender +0x15D10:" : "NOT under +0x15D10:",
                      reinterpret_cast<void*>(s.studioDesc), reinterpret_cast<void*>(s.studioArray), s.studioCount,
                      !s.studioSeen ? "  -- either the hook did not take (see STUDIODRAW above) or the arms are drawn "
                                      "outside it; check the call counter before reading this as an answer."
                                    : "");
        Tf2VrLog(prov);
        // The per-mesh skip gate at studiorender.dll+0xE06A. A mesh whose entry
        // reads 0 is skipped by the engine itself, so these values are the
        // filter: the three non-glove meshes are hidden by zeroing theirs.
        pn = std::snprintf(prov, sizeof(prov),
                           "[TF2VR] VBBIND GATE[%d] model \"%s\" (%s), desc %s %p, hdr %p, lod %d, "
                           "gate array %p, unwind %s at frame %d of %u:",
                           g_sitesPrinted, s.studioName[0] ? s.studioName : "?",
                           s.studioIsArms ? "IS the arms model" : "NOT the arms model",
                           s.studioFromTls ? "from the stack" : "NOT RECOVERED", reinterpret_cast<void*>(s.studioDesc),
                           reinterpret_cast<void*>(s.studioHdr),
                           s.studioLod, reinterpret_cast<void*>(s.studioGate),
                           s.studioFromTls ? "FOUND +0x12380" : "DID NOT REACH +0x12380",
                           static_cast<int>(s.studioGlobalDesc), s.slotTid);
        for (int m = 0; m < g_armsMeshCount && m < 16; ++m) {
            pn += std::snprintf(prov + pn, sizeof(prov) - pn, " mesh%d(id%d,%dv%s)=%u/batches=%u", m, g_armsMeshId[m],
                                g_armsMeshVertexCount[m], g_armsMeshIsGlove[m] ? ",glove" : "", s.gateValue[m],
                                s.batchCount[m]);
        }
        std::snprintf(prov + pn, sizeof(prov) - pn, "%s\n",
                      !s.gateRead      ? "  GATE NOT READ -- instrument failure, not a null."
                      : !s.studioIsArms ? "  VOID: raced another model's draw, these numbers are not the arms'."
                                        : "  All five non-zero = the gate is live and this is the filter.");
        Tf2VrLog(prov);
    }
}

// Called from the crash recorder, on a thread that has already faulted: reads
// only its own small ring, takes no lock and calls nothing.
void ReportGameWritesForCrash() {
    const std::uint32_t seq = g_gameWriteSeq.load(std::memory_order_relaxed);
    if (seq == 0) {
        Tf2VrLog("[TF2VR] CRASH game-writes: NONE this run -- the plugin had written nothing into game "
                 "memory through this path when it faulted.\n");
        return;
    }
    const std::uint64_t now = GetTickCount64();
    char line[300];
    std::snprintf(line, sizeof(line), "[TF2VR] CRASH game-writes: %llu total, most recent first (now=%llu ms):\n",
                  static_cast<unsigned long long>(seq), static_cast<unsigned long long>(now));
    Tf2VrLog(line);
    const int shown = (seq < kGameWriteRing) ? static_cast<int>(seq) : kGameWriteRing;
    for (int i = 1; i <= shown; ++i) {
        const GameWrite& w = g_gameWrites[(seq - i) % kGameWriteRing];
        std::snprintf(line, sizeof(line),
                      "[TF2VR] CRASH  write[-%d] %llu ms ago, thread %u, %p: %u -> %u\n", i,
                      static_cast<unsigned long long>(now - w.tick), w.thread,
                      reinterpret_cast<void*>(w.address), w.before, w.after);
        Tf2VrLog(line);
    }
}
