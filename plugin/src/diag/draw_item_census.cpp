#include "draw_item_census.h"

#include "arms_collapse.h"
#include "mesh_census.h"
#include "diagnostics.h"

#include "titan_state.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" std::uint64_t g_armsBonesTargetInstance;

namespace {

// materialsystem_dx11.dll, this build. CMatRenderContext vtable +0x1D5498
// (locator +0x1F2D68); slots 170/171 = +0x1D59E8/+0x1D59F0 -> +0x72AC0/+0x72B60,
// wrappers around +0x1D800 DrawItems(count, items) over 0x60-byte items.
//
// WHAT AN ITEM IS (runs 5 and 6, 2026-09-05). Slot 170 is the live submit
// (~10 calls, ~900 items a frame; slot 171 idle). No qword of an item, nor any
// qword one level below, is a CMaterial or a CMaterialGlue. What IS there:
//   [item+0x30] -> a struct whose +0x10, +0x20, +0x70 are D3D COM objects
//                  (vtable in a system DLL): the material's compiled shader state
//   [item+0x38] -> a struct whose +0x8, +0x58, +0x68 are D3D COM objects: the
//                  HARDWARE MESH (index buffer, vertex buffers)
// and +0x1D800 batches consecutive items by [item+0x18], [[item+0x30]+0x10]
// and [item+0x38]. The hardware mesh is per model per mesh -- the arms model's
// five are its own objects however many grunts share its materials -- so it is
// the per-model key a filter needs. This census collects the distinct mesh and
// state pointers the items carry, then walks OUT from the validated arms entity
// (the entity, its model handle at +0x1208, the studiohdr at [handle+8]) two
// levels deep looking for fields that hold those pointers: the model -> meshes
// link, found empirically instead of by a layout guess.
constexpr std::uintptr_t kSlot170Rva = 0x1D59E8;
constexpr std::uintptr_t kSlot171Rva = 0x1D59F0;
constexpr std::uintptr_t kWrap170Rva = 0x72AC0;
constexpr std::uintptr_t kWrap171Rva = 0x72B60;
constexpr std::uint8_t kWrapPrologue[] = {0x48, 0x83, 0xEC, 0x38, 0x44, 0x8B, 0x0D};
constexpr std::size_t kItemBytes = 0x60;
constexpr int kMaxItemsPerCall = 8192;
constexpr std::size_t kItemMesh = 0x38;
constexpr std::size_t kItemState = 0x30;

using DrawFn = std::uint64_t(__fastcall*)(void* self, int count, void* items, void* r9);
DrawFn g_orig170 = nullptr;
DrawFn g_orig171 = nullptr;
void** g_slot170 = nullptr;
void** g_slot171 = nullptr;
bool g_tried = false;

// Distinct pointer sets, lock-free open addressing keyed by value. Written on
// the render thread, read on the main thread; a slot is published by its key.
constexpr int kSetSize = 4096;
struct SetEntry {
    std::atomic<std::uintptr_t> key{0};
    std::atomic<std::uint64_t> count{0};
};
struct PtrSet {
    SetEntry e[kSetSize];
    std::atomic<int> used{0};
    std::atomic<std::uint64_t> overflow{0};
    void Add(std::uintptr_t p) {
        if (!p) return;
        std::size_t h = (p >> 4) & (kSetSize - 1);
        for (int probe = 0; probe < 32; ++probe) {
            SetEntry& s = e[(h + probe) & (kSetSize - 1)];
            std::uintptr_t k = s.key.load(std::memory_order_acquire);
            if (k == p) { s.count.fetch_add(1, std::memory_order_relaxed); return; }
            if (k == 0) {
                std::uintptr_t expected = 0;
                if (s.key.compare_exchange_strong(expected, p, std::memory_order_acq_rel)) {
                    s.count.fetch_add(1, std::memory_order_relaxed);
                    used.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (expected == p) { s.count.fetch_add(1, std::memory_order_relaxed); return; }
            }
        }
        overflow.fetch_add(1, std::memory_order_relaxed);
    }
    bool Has(std::uintptr_t p, std::uint64_t* countOut) const {
        if (!p) return false;
        std::size_t h = (p >> 4) & (kSetSize - 1);
        for (int probe = 0; probe < 32; ++probe) {
            const SetEntry& s = e[(h + probe) & (kSetSize - 1)];
            const std::uintptr_t k = s.key.load(std::memory_order_acquire);
            if (k == p) { *countOut = s.count.load(); return true; }
            if (k == 0) return false;
        }
        return false;
    }
};
PtrSet g_meshes;
PtrSet g_states;

std::atomic<std::uint64_t> g_calls170{0}, g_calls171{0}, g_itemsSeen{0}, g_faults{0};

std::size_t BytesFrom(const void* p) {
    if (!p) return 0;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    const DWORD prot = mbi.Protect & 0xFF;
    const bool readable = prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_READONLY ||
                          prot == PAGE_EXECUTE_READ || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_WRITECOPY;
    if (!readable) return 0;
    return reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize - reinterpret_cast<std::uintptr_t>(p);
}

// ---- ONE-SHOT GROUPED DUMP ------------------------------------------------------
// Run 8 (2026-09-05) stuttered: the sampler read every item's transform, and
// those matrices live in per-frame scratch (mapped memory, the known trap). It
// still showed the layout: +0x00/+0x04 first index / index count inside the
// pooled buffer at +0x38, +0x30 the shader state, and the 16-bit value at
// +0x58 a per-instance bone base that every mesh of one model shares in a
// frame. So: read NOTHING outside the item array, and only once. After a
// warm-up, kDumpCalls consecutive slot-170 calls are grouped by that bone base
// and stored; the main thread prints them. The arms are the five-item group
// whose index counts follow 8565:2738:160:4542:1869.
constexpr int kDumpCalls = 24;
constexpr int kMaxGroups = 256;
constexpr int kGroupSamples = 8;
constexpr std::uint64_t kWarmupMs = 20000;  // spawn-in is over; the first-N-calls trap
struct Group {
    std::uint16_t w58;
    std::uint16_t w5A;
    std::uint32_t n;
    std::uint32_t count4[kGroupSamples];
    std::uint32_t first0[kGroupSamples];
    std::uint32_t d5C[kGroupSamples];
    std::uintptr_t state[kGroupSamples];
    std::uintptr_t pool[kGroupSamples];
};
struct CallDump {
    int itemCount;
    int groupCount;
    int overflow;
    Group g[kMaxGroups];
    std::atomic<bool> ready{false};
};
CallDump g_dumps[kDumpCalls];
std::atomic<int> g_dumpCount{0};
int g_dumpsPrinted = 0;
std::atomic<bool> g_dumpArmed{false};

void DumpCall(const std::uint8_t* base, int count) {
    const int d = g_dumpCount.load(std::memory_order_acquire);
    if (d >= kDumpCalls) return;
    CallDump& cd = g_dumps[d];
    cd.itemCount = count;
    cd.groupCount = 0;
    cd.overflow = 0;
    for (int i = 0; i < count; ++i) {
        const std::uint8_t* it = base + i * kItemBytes;
        const std::uint16_t w58 = *reinterpret_cast<const std::uint16_t*>(it + 0x58);
        const std::uint16_t w5A = *reinterpret_cast<const std::uint16_t*>(it + 0x5A);
        Group* g = nullptr;
        for (int k = 0; k < cd.groupCount; ++k) {
            if (cd.g[k].w58 == w58 && cd.g[k].w5A == w5A) { g = &cd.g[k]; break; }
        }
        if (!g) {
            if (cd.groupCount >= kMaxGroups) { ++cd.overflow; continue; }
            g = &cd.g[cd.groupCount++];
            std::memset(g, 0, sizeof(*g));
            g->w58 = w58;
            g->w5A = w5A;
        }
        if (g->n < kGroupSamples) {
            g->first0[g->n] = *reinterpret_cast<const std::uint32_t*>(it + 0x00);
            g->count4[g->n] = *reinterpret_cast<const std::uint32_t*>(it + 0x04);
            g->d5C[g->n] = *reinterpret_cast<const std::uint32_t*>(it + 0x5C);
            g->state[g->n] = *reinterpret_cast<const std::uintptr_t*>(it + 0x30);
            g->pool[g->n] = *reinterpret_cast<const std::uintptr_t*>(it + 0x38);
        }
        ++g->n;
    }
    cd.ready.store(true, std::memory_order_release);
    g_dumpCount.store(d + 1, std::memory_order_release);
}

// ---- ARMS MATCH TALLY -----------------------------------------------------------
// Every item whose +0x04 equals one of the arms model's VTX index counts is
// tallied under its +0x38 (the hardware mesh) with its +0x30, the instance
// pattern (+0x5C) and how many items the call had. Eight integer compares per
// item, nothing read outside the item array. The arms' five meshes should each
// resolve to ONE +0x38 that persists; a grunt mesh with the same count would
// show as a second +0x38 under that count.
// ---- THE MESH KEY (offline read of +0x1D800, 2026-09-05 evening) ----------------
// Per item the routine binds and draws, on the D3D context [+0x14E8DD8]:
//     IASetVertexBuffers(slot 0, 1, &[[item+0x30]+0x10], &stride [[item+0x30]+0xC], &offset item+0x2C)
//     IASetIndexBuffer([[item+0x38]+8], fmt 0x39, 0)
//     DrawIndexed(count = item+0x04, startIndex = item+0x00, baseVertex 0)      ; ctx slot 12
// +0x30/+0x38 are a ring of 48 per-draw descriptors reused every frame; the
// persistent identity of a mesh is (vertex buffer, vertex offset) with its
// index buffer and count. The arms model's five keys share its vertex buffer,
// created at model load and referenced from its model handle -- the walk
// below hunts those buffer pointers from the entity.
constexpr int kMaxKeys = 512;
struct Key {
    std::atomic<std::uintptr_t> vb{0};
    std::uint32_t off, count, start, d5CSeen;
    std::uintptr_t ib;
    std::uint16_t stride;
    std::atomic<std::uint64_t> hits{0};
    std::uint64_t hitsAtMark;  // snapshot when the wearer pressed F6 (main thread)
    std::uint32_t verts;       // ID3D11Buffer::GetDesc(vb).ByteWidth / stride
    std::uint32_t indices;     // ID3D11Buffer::GetDesc(ib).ByteWidth / 2 (format 0x39 = R16_UINT)
    std::atomic<int> armsMesh{-1};  // index into the arms model's meshes when verts matches exactly
};
Key g_keys[kMaxKeys];
// The filter: mode 1 drops the arms' non-glove meshes at the submit. Armed only
// once every arms mesh has been identified by vertex count.
std::atomic<int> g_armsIdentified{0};
std::atomic<std::uint64_t> g_filteredItems{0}, g_filteredCalls{0}, g_descFaults{0};
alignas(16) std::uint8_t g_filterScratch[kMaxItemsPerCall * kItemBytes];

// Asks the D3D buffer for its own size. Same thread and same objects the engine
// binds a few instructions later; under the caller's SEH.
std::uint32_t ByteWidthOf(std::uintptr_t buffer) {
    if (!buffer || (buffer & 7) != 0) return 0;
    auto* b = reinterpret_cast<ID3D11Buffer*>(buffer);
    D3D11_BUFFER_DESC desc{};
    b->GetDesc(&desc);
    return desc.ByteWidth;
}
std::atomic<int> g_keyCount{0};
std::atomic<std::uint64_t> g_keyOverflow{0}, g_keyFaults{0};

// Render thread, under the caller's SEH. Reads the two ring descriptors the
// engine is about to read itself; nothing in frame scratch.
// Returns the key index (>= 0) for a (vb, off, count), registering and
// identifying it on first sight; -1 when the table is full.
int TallyKeyRaw(std::uintptr_t vb, std::uint16_t stride, std::uintptr_t ib, std::uint32_t off, std::uint32_t count,
                std::uint32_t start, std::uint32_t d5C);

void TallyKey(const std::uint8_t* it) {
    const auto* d30 = *reinterpret_cast<const std::uint8_t* const*>(it + 0x30);
    const auto* d38 = *reinterpret_cast<const std::uint8_t* const*>(it + 0x38);
    if (!d30 || !d38 || (reinterpret_cast<std::uintptr_t>(d30) & 7) || (reinterpret_cast<std::uintptr_t>(d38) & 7)) return;
    const std::uintptr_t vb = *reinterpret_cast<const std::uintptr_t*>(d30 + 0x10);
    const std::uint16_t stride = *reinterpret_cast<const std::uint16_t*>(d30 + 0xC);
    const std::uintptr_t ib = *reinterpret_cast<const std::uintptr_t*>(d38 + 0x8);
    const std::uint32_t off = *reinterpret_cast<const std::uint32_t*>(it + 0x2C);
    const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(it + 0x04);
    const std::uint32_t start = *reinterpret_cast<const std::uint32_t*>(it + 0x00);
    const std::uint32_t d5C = *reinterpret_cast<const std::uint32_t*>(it + 0x5C);
    TallyKeyRaw(vb, stride, ib, off, count, start, d5C);
}

int TallyKeyRaw(std::uintptr_t vb, std::uint16_t stride, std::uintptr_t ib, std::uint32_t off, std::uint32_t count,
                std::uint32_t start, std::uint32_t d5C) {
    if (!vb) return -1;
    g_meshes.Add(vb);
    g_states.Add(ib);
    const int n = g_keyCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        Key& k = g_keys[i];
        if (k.vb.load(std::memory_order_relaxed) == vb && k.off == off && k.count == count) {
            k.hits.fetch_add(1, std::memory_order_relaxed);
            k.d5CSeen |= d5C;
            return i;
        }
    }
    if (n >= kMaxKeys) { g_keyOverflow.fetch_add(1); return -1; }
    int slot = n;
    if (!g_keyCount.compare_exchange_strong(slot, slot + 1)) return -1;
    Key& k = g_keys[slot];
    k.off = off; k.count = count; k.start = start; k.d5CSeen = d5C; k.ib = ib; k.stride = stride;
    k.hits.store(1);
    // Identify by the buffers' own sizes.
    k.verts = stride ? ByteWidthOf(vb) / stride : 0;
    k.indices = ByteWidthOf(ib) / 2;
    int which = -1;
    if (g_armsHeaderValid && k.verts) {
        for (int m = 0; m < g_armsMeshCount; ++m) {
            if (g_armsMeshVertexCount[m] == static_cast<int>(k.verts)) { which = m; break; }
        }
    }
    k.armsMesh.store(which);
    if (which >= 0) g_armsIdentified.fetch_add(1);
    k.vb.store(vb, std::memory_order_release);
    return slot;
}

// ---- SLOT 172: THE SKINNED / INSTANCED PATH -------------------------------------
// materialsystem_dx11.dll+0x1CFE0 via the slot-172 wrapper +0x72B20 (six
// arguments: rcx, count in edx, items in r8, r9d, two stack qwords). One call
// is ONE mesh drawn for `count` instances (capped at 256): the vertex-buffer
// descriptor is [items+0x10] (buffer at +0x10, stride at +0xC), the index
// descriptor [items+0x18] (index count at +0, buffer at +8); it binds a vertex
// shader resource (the bones) and calls DrawIndexedInstanced. The arms, being
// skinned, go here -- slot 170 carried only stride-28 scenery. Mode 1 passes
// a non-glove arms mesh through with ZERO instances.
constexpr std::uintptr_t kSlot172Rva = 0x1D59F8;
constexpr std::uintptr_t kWrap172Rva = 0x72B20;
constexpr std::uint8_t kWrap172Prologue[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x8B, 0x44, 0x24, 0x68};
using Draw172Fn = std::uint64_t(__fastcall*)(void* self, int count, void* items, void* a4, void* a5, void* a6);
Draw172Fn g_orig172 = nullptr;
void** g_slot172 = nullptr;
std::atomic<std::uint64_t> g_calls172{0}, g_filteredCalls172{0}, g_filteredInstances172{0};

// PROVEN LAYOUT of +0x1CFE0 (offline read, late 2026-09-05), through its slot-172
// wrapper +0x72B20 (six caller arguments: rcx, edx, r8, r9d, stack a5, stack a6):
//     1CFE0.ecx  = edx      entry count      -> [rbp+0xD0], the outer loop bound
//     1CFE0.rdx  = r8       table B, 32-byte per-instance records ([rbp+0xD8], read at +8)
//     1CFE0.r8d  = r9d      an int ([rbp+0xE0])
//     1CFE0.r9   = a5       TABLE A (r15): 32-byte entries, index i*32 (shl rsi,5):
//                             +0x00 object (vtable call [+0x48] unless it equals a global)
//                             +0x10 -> vertex descriptor: buffer +0x10, stride +0xC
//                             +0x18 -> index descriptor:  index count +0, buffer +8
//     stack      = a6
// The crashed wrapper read the entries from r8. The entries are at a5.
//
// HIDING an entry without touching either table: copy table A, point the
// hidden entry's +0x18 at a private descriptor {0, same buffer}, pass the copy.
// The routine then binds the same buffers and draws zero indices.
constexpr bool kFilter172Enabled = false;  // this run is LOG-ONLY; flip after the arms' five keys are seen here
constexpr int kMaxEntries172 = 4096;
constexpr std::size_t kEntryBytes172 = 32;
alignas(16) std::uint8_t g_entryScratch[kMaxEntries172 * kEntryBytes172];
struct ZeroIndexDesc { std::uint32_t count; std::uint32_t pad; std::uintptr_t ib; std::uint64_t tail[6]; };
ZeroIndexDesc g_zeroDescs[64];  // one per hidden entry in a call

// Tallies every entry of the call; returns the table to pass (the original, or
// a patched copy) -- pass-through unless the filter is enabled and armed.
void* Tally172(const std::uint8_t* entries, int count) {
    if (count <= 0 || count > kMaxEntries172) return const_cast<std::uint8_t*>(entries);
    if (BytesFrom(entries) < static_cast<std::size_t>(count) * kEntryBytes172) return const_cast<std::uint8_t*>(entries);
    const bool filtering = kFilter172Enabled && BodyShowMode() == 1 && !IsInTitanNow() && g_armsHeaderValid &&
                           g_armsIdentified.load(std::memory_order_acquire) >= g_armsMeshCount;
    bool copied = false;
    int zeroUsed = 0;
    for (int i = 0; i < count; ++i) {
        const std::uint8_t* e = entries + static_cast<std::size_t>(i) * kEntryBytes172;
        const auto* vd = *reinterpret_cast<const std::uint8_t* const*>(e + 0x10);
        const auto* id = *reinterpret_cast<const std::uint8_t* const*>(e + 0x18);
        if (!vd || !id || (reinterpret_cast<std::uintptr_t>(vd) & 7) || (reinterpret_cast<std::uintptr_t>(id) & 7)) continue;
        const std::uintptr_t vb = *reinterpret_cast<const std::uintptr_t*>(vd + 0x10);
        const std::uint16_t stride = *reinterpret_cast<const std::uint16_t*>(vd + 0xC);
        const std::uint32_t indexCount = *reinterpret_cast<const std::uint32_t*>(id + 0x0);
        const std::uintptr_t ib = *reinterpret_cast<const std::uintptr_t*>(id + 0x8);
        const int key = TallyKeyRaw(vb, stride, ib, 0x172, indexCount, static_cast<std::uint32_t>(count), 0);
        if (!filtering || key < 0) continue;
        const int m = g_keys[key].armsMesh.load(std::memory_order_relaxed);
        if (m < 0 || g_armsMeshIsGlove[m] || zeroUsed >= 64) continue;
        if (!copied) {
            std::memcpy(g_entryScratch, entries, static_cast<std::size_t>(count) * kEntryBytes172);
            copied = true;
        }
        ZeroIndexDesc& z = g_zeroDescs[zeroUsed++];
        z.count = 0;
        z.pad = 0;
        z.ib = ib;
        *reinterpret_cast<const void**>(g_entryScratch + static_cast<std::size_t>(i) * kEntryBytes172 + 0x18) = &z;
        g_filteredInstances172.fetch_add(1, std::memory_order_relaxed);
    }
    if (copied) g_filteredCalls172.fetch_add(1, std::memory_order_relaxed);
    return copied ? static_cast<void*>(g_entryScratch) : const_cast<std::uint8_t*>(entries);
}

void* Tally172Guarded(void* entries, int count) {
    __try {
        if (!entries) return entries;
        return Tally172(static_cast<const std::uint8_t*>(entries), count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        return entries;
    }
}

std::uint64_t __fastcall Wrap172(void* self, int count, void* tableB, void* a4, void* tableA, void* a6) {
    g_calls172.fetch_add(1, std::memory_order_relaxed);
    if (g_faults.load(std::memory_order_relaxed) == 0) {
        void* passed = Tally172Guarded(tableA, count);
        return g_orig172(self, count, tableB, a4, passed, a6);
    }
    return g_orig172(self, count, tableB, a4, tableA, a6);
}

// Mode 1, on foot, every arms mesh identified: drop the non-glove arms items.
// Returns the array to submit and rewrites count; pass-through otherwise.
void* FilterItems(void* items, int* count) {
    if (BodyShowMode() != 1 || IsInTitanNow()) return items;
    if (!g_armsHeaderValid || g_armsIdentified.load(std::memory_order_acquire) < g_armsMeshCount) return items;
    if (*count <= 0 || *count > kMaxItemsPerCall) return items;
    const auto* base = static_cast<const std::uint8_t*>(items);
    int kept = 0;
    bool dropped = false;
    const int kn = g_keyCount.load(std::memory_order_acquire);
    for (int i = 0; i < *count; ++i) {
        const std::uint8_t* it = base + i * kItemBytes;
        const auto* d30 = *reinterpret_cast<const std::uint8_t* const*>(it + 0x30);
        bool drop = false;
        if (d30 && (reinterpret_cast<std::uintptr_t>(d30) & 7) == 0) {
            const std::uintptr_t vb = *reinterpret_cast<const std::uintptr_t*>(d30 + 0x10);
            for (int j = 0; j < kn && j < kMaxKeys; ++j) {
                if (g_keys[j].vb.load(std::memory_order_relaxed) != vb) continue;
                const int m = g_keys[j].armsMesh.load(std::memory_order_relaxed);
                drop = m >= 0 && !g_armsMeshIsGlove[m];
                break;
            }
        }
        if (drop) { dropped = true; continue; }
        std::memcpy(g_filterScratch + static_cast<std::size_t>(kept) * kItemBytes, it, kItemBytes);
        ++kept;
    }
    if (!dropped) return items;
    g_filteredItems.fetch_add(static_cast<std::uint64_t>(*count - kept), std::memory_order_relaxed);
    g_filteredCalls.fetch_add(1, std::memory_order_relaxed);
    *count = kept;
    return g_filterScratch;
}

void WalkGuarded(void* items, int count, bool dump) {
    __try {
        if (count <= 0 || count > kMaxItemsPerCall) return;
        if (BytesFrom(items) < static_cast<std::size_t>(count) * kItemBytes) return;
        g_itemsSeen.fetch_add(static_cast<std::uint64_t>(count), std::memory_order_relaxed);
        const auto* base = static_cast<const std::uint8_t*>(items);
        if (dump) DumpCall(base, count);
        for (int i = 0; i < count; ++i) {
            const std::uint8_t* it = base + i * kItemBytes;
            TallyKey(it);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
    }
}

void* FilterGuarded(void* items, int* count) {
    __try {
        return FilterItems(items, count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        return items;
    }
}

std::uint64_t __fastcall Wrap170(void* self, int count, void* items, void* r9) {
    g_calls170.fetch_add(1, std::memory_order_relaxed);
    if (g_faults.load(std::memory_order_relaxed) == 0) {
        const bool dump = g_dumpArmed.load(std::memory_order_acquire) && g_dumpCount.load(std::memory_order_relaxed) < kDumpCalls;
        WalkGuarded(items, count, dump);
        int newCount = count;
        void* submitted = FilterGuarded(items, &newCount);
        if (submitted != items) return g_orig170(self, newCount, submitted, r9);
    }
    return g_orig170(self, count, items, r9);
}

// ---- THE MODEL WALK (main thread, once, after gameplay) --------------------------
// Run 7's walk searched during spawn-in, when the mesh set held 37 pool values
// and none of the arms'. Now the set is the gameplay set and the match is by
// RANGE at the first two levels: an item's +0x38 may point INTO an array of
// mesh structs rather than at a separately allocated object.
constexpr std::size_t kRootBytesEntity = 0x1400;
constexpr std::size_t kRootBytesOther = 0x800;
constexpr std::size_t kInnerBytes = 0x1000;
constexpr std::uintptr_t kRangeBytes = 8;  // buffers are COM objects: equality only
constexpr int kMaxFindings = 80;
constexpr std::uint64_t kWalkAfterMs = 30000;
bool g_walkDone = false;
int g_findings = 0;

// Equality via the hash set, then a range scan over the set: any mesh pointer in [p, p+range).
bool NearMesh(std::uintptr_t p, std::uintptr_t* hitOut, std::uint64_t* countOut) {
    if (!p || (p & 7) != 0) return false;
    if (g_meshes.Has(p, countOut)) { *hitOut = p; return true; }
    for (int i = 0; i < kSetSize; ++i) {
        const std::uintptr_t k = g_meshes.e[i].key.load(std::memory_order_acquire);
        if (k && k >= p && k - p < kRangeBytes) { *hitOut = k; *countOut = g_meshes.e[i].count.load(); return true; }
    }
    return false;
}

void ReportFinding(const char* root, std::size_t off, int inner, int deep, std::uintptr_t held, std::uintptr_t mesh,
                   std::uint64_t count) {
    if (g_findings >= kMaxFindings) return;
    ++g_findings;
    char line[320];
    int n = std::snprintf(line, sizeof(line), "[TF2VR] MODELWALK %s+0x%zX", root, off);
    if (inner >= 0) n += std::snprintf(line + n, sizeof(line) - n, " -> [+0x%X]", inner);
    if (deep >= 0) n += std::snprintf(line + n, sizeof(line) - n, " -> [+0x%X]", deep);
    std::snprintf(line + n, sizeof(line) - n, " = %p  covers item mesh %p (+0x%llX, seen in %llu items)\n",
                  reinterpret_cast<void*>(held), reinterpret_cast<void*>(mesh), static_cast<unsigned long long>(mesh - held),
                  static_cast<unsigned long long>(count));
    Tf2VrLog(line);
}

void ScanRoot(const char* name, const std::uint8_t* root, std::size_t bytes, bool threeLevels) {
    const std::size_t avail = BytesFrom(root);
    if (avail < 16) {
        char line[200];
        std::snprintf(line, sizeof(line), "[TF2VR] MODELWALK root %s at %p is not readable.\n", name, reinterpret_cast<const void*>(root));
        Tf2VrLog(line);
        return;
    }
    if (bytes > avail) bytes = avail;
    for (std::size_t off = 0; off + 8 <= bytes; off += 8) {
        const std::uintptr_t q = *reinterpret_cast<const std::uintptr_t*>(root + off);
        std::uintptr_t hit = 0;
        std::uint64_t c = 0;
        if (NearMesh(q, &hit, &c)) ReportFinding(name, off, -1, -1, q, hit, c);
        if (!q || (q & 7) != 0) continue;
        const std::size_t innerAvail = BytesFrom(reinterpret_cast<const void*>(q));
        if (innerAvail < 16) continue;
        const std::size_t innerBytes = innerAvail < kInnerBytes ? innerAvail : kInnerBytes;
        int hitsHere = 0;
        for (std::size_t j = 0; j + 8 <= innerBytes && hitsHere < 12; j += 8) {
            const std::uintptr_t r = *reinterpret_cast<const std::uintptr_t*>(q + j);
            if (NearMesh(r, &hit, &c)) { ReportFinding(name, off, static_cast<int>(j), -1, r, hit, c); ++hitsHere; }
            if (!threeLevels || !r || (r & 7) != 0 || j >= 0x200) continue;
            const std::size_t deepAvail = BytesFrom(reinterpret_cast<const void*>(r));
            if (deepAvail < 16) continue;
            const std::size_t deepBytes = deepAvail < 0x400 ? deepAvail : 0x400;
            int deepHits = 0;
            for (std::size_t k = 0; k + 8 <= deepBytes && deepHits < 8; k += 8) {
                const std::uintptr_t t = *reinterpret_cast<const std::uintptr_t*>(r + k);
                if (g_meshes.Has(t, &c)) { ReportFinding(name, off, static_cast<int>(j), static_cast<int>(k), t, t, c); ++deepHits; }
            }
        }
    }
}

void ModelWalk() {
    __try {
        const auto* entity = reinterpret_cast<const std::uint8_t*>(g_armsBonesTargetInstance);
        char line[300];
        std::snprintf(line, sizeof(line),
                      "[TF2VR] MODELWALK start: arms entity %p, %d distinct vertex buffers / %d index buffers from %llu items (overflow %llu/%llu); "
                      "range match %llX at levels 1-2.\n",
                      reinterpret_cast<const void*>(entity), g_meshes.used.load(), g_states.used.load(),
                      static_cast<unsigned long long>(g_itemsSeen.load()), static_cast<unsigned long long>(g_meshes.overflow.load()),
                      static_cast<unsigned long long>(g_states.overflow.load()), static_cast<unsigned long long>(kRangeBytes));
        Tf2VrLog(line);
        ScanRoot("entity", entity, kRootBytesEntity, false);
        if (BytesFrom(entity + 0x1208) >= 8) {
            const auto* handle = *reinterpret_cast<const std::uint8_t* const*>(entity + 0x1208);
            std::snprintf(line, sizeof(line), "[TF2VR] MODELWALK model handle [entity+0x1208] = %p\n", reinterpret_cast<const void*>(handle));
            Tf2VrLog(line);
            ScanRoot("handle", handle, kRootBytesOther, true);
            if (handle && BytesFrom(handle + 8) >= 8) {
                const auto* studio = *reinterpret_cast<const std::uint8_t* const*>(handle + 8);
                std::snprintf(line, sizeof(line), "[TF2VR] MODELWALK studiohdr [handle+8] = %p\n", reinterpret_cast<const void*>(studio));
                Tf2VrLog(line);
                ScanRoot("studiohdr", studio, kRootBytesOther, true);
            }
        }
        std::snprintf(line, sizeof(line), "[TF2VR] MODELWALK done: %d findings (cap %d).%s\n", g_findings, kMaxFindings,
                      g_findings == 0 ? "  NOTHING within three levels of the entity, its handle or its header reaches an item mesh." : "");
        Tf2VrLog(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Tf2VrLog("[TF2VR] MODELWALK: FAULTED (caught); walk abandoned.\n");
    }
}

std::uint64_t __fastcall Wrap171(void* self, int count, void* items, void* r9) {
    g_calls171.fetch_add(1, std::memory_order_relaxed);
    return g_orig171(self, count, items, r9);
}

bool SwapSlot(std::uint8_t* base, std::uintptr_t slotRva, std::uintptr_t expectedRva, void* replacement, void*** slotOut,
              DrawFn* origOut, const char* label) {
    auto** slot = reinterpret_cast<void**>(base + slotRva);
    auto* expected = reinterpret_cast<void*>(base + expectedRva);
    char line[240];
    if (*slot != expected) {
        std::snprintf(line, sizeof(line), "[TF2VR] draw-census: %s holds %p, expected %p; not installed.\n", label, *slot,
                      expected);
        Tf2VrLog(line);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        std::snprintf(line, sizeof(line), "[TF2VR] draw-census: VirtualProtect failed on %s; not installed.\n", label);
        Tf2VrLog(line);
        return false;
    }
    *origOut = reinterpret_cast<DrawFn>(expected);
    *slot = replacement;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    *slotOut = slot;
    return true;
}

void Install() {
    HMODULE ms = GetModuleHandleA("materialsystem_dx11.dll");
    if (!ms) {
        Tf2VrLog("[TF2VR] draw-census: materialsystem_dx11.dll not loaded; not installed.\n");
        return;
    }
    auto* base = reinterpret_cast<std::uint8_t*>(ms);
    if (std::memcmp(base + kWrap170Rva, kWrapPrologue, sizeof(kWrapPrologue)) != 0 ||
        std::memcmp(base + kWrap171Rva, kWrapPrologue, sizeof(kWrapPrologue)) != 0) {
        Tf2VrLog("[TF2VR] draw-census: the slot 170/171 wrappers do not match this build; not installed.\n");
        return;
    }
    const bool a = SwapSlot(base, kSlot170Rva, kWrap170Rva, reinterpret_cast<void*>(&Wrap170), &g_slot170, &g_orig170,
                            "render-context slot 170");
    const bool b = SwapSlot(base, kSlot171Rva, kWrap171Rva, reinterpret_cast<void*>(&Wrap171), &g_slot171, &g_orig171,
                            "render-context slot 171");
    bool c = false;
    if (std::memcmp(base + kWrap172Rva, kWrap172Prologue, sizeof(kWrap172Prologue)) == 0) {
        DrawFn orig = nullptr;
        c = SwapSlot(base, kSlot172Rva, kWrap172Rva, reinterpret_cast<void*>(&Wrap172), &g_slot172, &orig,
                     "render-context slot 172 (skinned)");
        if (c) g_orig172 = reinterpret_cast<Draw172Fn>(orig);
    } else {
        Tf2VrLog("[TF2VR] draw-census: the slot 172 wrapper does not match this build; the skinned path is NOT hooked.\n");
    }
    char line172[200];
    std::snprintf(line172, sizeof(line172), "[TF2VR] draw-census: slot 172 (skinned, +0x1CFE0) %s.\n", c ? "hooked" : "NOT hooked");
    Tf2VrLog(line172);
    char line[300];
    std::snprintf(line, sizeof(line),
                  "[TF2VR] draw-census: installed on CMatRenderContext vtable slots 170 (%s) and 171 (%s); read-only. "
                  "Collecting the distinct hardware-mesh (+0x38) and shader-state (+0x30) pointers of every item.\n",
                  a ? "ok" : "NO", b ? "ok" : "NO");
    Tf2VrLog(line);
}

}  // namespace

// PARKED 2026-09-05 (docs/RESULT-HANDS-MESH-2026-09-05.md): the instruments
// stay in the tree but do not install. Flip to true to resume the front.
// RETIRED 2026-09-06. This census answered its question and the gloves front
// shipped through a DIFFERENT mechanism entirely (studiorender.s per-mesh gate,
// vb_bind_census.cpp) -- these three wrappers contribute nothing now.
//
// They are retired as the first suspect in the 09-06 titan-exit crash
// (ACCESS_VIOLATION at materialsystem_dx11+0x1DA66, inside +0x1D800, READ from
// 0x10 = a null item descriptor at [item+0x30]). Not because they are proven
// guilty -- their arity checks out, +0x72AC0 and +0x72B60 are 41-byte thunks
// whose highest stack access is the 0x20 shadow space -- but because they are
// OUR three hooks inside the module that faulted, they ran 431k times a run,
// and until the -numworkerthreads 1 arm came out on 09-06 they had NEVER run
// against a parallel renderer. Dead instrumentation on the crashing module
// makes every attribution after it murkier.
constexpr bool kDrawCensusEnabled = false;
constexpr bool kDumpEnabled = false;         // the grouped dump has said its piece

void AdvanceDrawItemCensus() {
    if (!kDrawCensusEnabled) return;
    if (!g_slot170 && !g_slot171) {
        if (g_tried || !IsArmsCollapseArmed()) return;
        g_tried = true;
        Install();
        return;
    }
    static std::uint64_t lastReport = 0;
    static std::uint64_t installedAt = 0;
    const std::uint64_t now = GetTickCount64();
    if (!installedAt) installedAt = now;
    if (kDumpEnabled && !g_dumpArmed.load() && now - installedAt >= kWarmupMs) g_dumpArmed.store(true, std::memory_order_release);
    if (!g_walkDone && g_armsBonesTargetInstance && now - installedAt >= kWalkAfterMs && g_meshes.used.load() > 0) {
        g_walkDone = true;
        ModelWalk();
    }
    if (now - lastReport < 1000) return;
    lastReport = now;
    char line[600];
    std::snprintf(line, sizeof(line),
                  "[TF2VR] draw-census: slot170 %llu calls, slot171 %llu, dump %s, %d/%d calls captured (%llu items in them), "
                  "faults %llu.%s\n",
                  static_cast<unsigned long long>(g_calls170.load()), static_cast<unsigned long long>(g_calls171.load()),
                  g_dumpArmed.load() ? "armed" : "warming up", g_dumpCount.load(), kDumpCalls,
                  static_cast<unsigned long long>(g_itemsSeen.load()), static_cast<unsigned long long>(g_faults.load()),
                  (g_calls170.load() + g_calls171.load()) == 0 ? "  ZERO CALLS -- the slots are not the live path." : "");
    Tf2VrLog(line);
    // Identification and filter status, once a second; every new key once.
    static int keysPrinted = 0;
    std::snprintf(line, sizeof(line),
                  "[TF2VR] draw-census: FILTER body.show=%d titan=%d header %s, arms meshes identified %d/%d; slot170 %llu calls "
                  "filtered / %llu items dropped; slot172 %llu calls, %llu zeroed (%llu instances); faults %llu.%s\n",
                  BodyShowMode(), IsInTitanNow() ? 1 : 0, g_armsHeaderValid ? "valid" : "NOT VALID", g_armsIdentified.load(),
                  g_armsMeshCount, static_cast<unsigned long long>(g_filteredCalls.load()),
                  static_cast<unsigned long long>(g_filteredItems.load()), static_cast<unsigned long long>(g_calls172.load()),
                  static_cast<unsigned long long>(g_filteredCalls172.load()),
                  static_cast<unsigned long long>(g_filteredInstances172.load()), static_cast<unsigned long long>(g_faults.load()),
                  (BodyShowMode() == 1 && g_armsHeaderValid && g_armsIdentified.load() < g_armsMeshCount)
                      ? "  NOT FILTERING: not every arms mesh has been seen yet."
                      : "");
    Tf2VrLog(line);
    const int knNow = g_keyCount.load(std::memory_order_acquire);
    for (; keysPrinted < knNow && keysPrinted < kMaxKeys; ++keysPrinted) {
        const Key& k = g_keys[keysPrinted];
        const std::uintptr_t vb = k.vb.load(std::memory_order_acquire);
        if (!vb) break;
        const int m = k.armsMesh.load();
        std::snprintf(line, sizeof(line),
                      "[TF2VR] MESHKEY[%d] vb %p verts %u indices %u (item count %u %s) stride %u%s%s\n", keysPrinted,
                      reinterpret_cast<void*>(vb), k.verts, k.indices, k.count, k.indices == k.count ? "match" : "DIFFER",
                      k.stride, m >= 0 ? "  ARMS MESH " : "", m >= 0 ? (g_armsMeshIsGlove[m] ? "(glove, kept)" : "(hidden in mode 1)") : "");
        Tf2VrLog(line);
    }
    // THE F6 MARK. The wearer cannot time a headset run; the plugin can. F6 is
    // the read-only capture key (GetAsyncKeyState, focus-independent): its
    // first press snapshots every key's hits and the call count, and 10 s and
    // 25 s later the report prints each key's rate before and after the mark,
    // in draws per 1000 slot-170 calls. A weapon switch at the press shows as
    // GONE (rate before, none after) for the old gun's meshes and NEW for the
    // new gun's; the arms and the scene are STEADY.
    static bool f6Was = false;
    static std::uint64_t markCalls = 0, markAt = 0;
    static int markReports = 0;
    const bool f6Now = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    if (f6Now && !f6Was && markAt == 0) {
        markAt = now;
        markCalls = g_calls170.load();
        const int kn = g_keyCount.load(std::memory_order_acquire);
        for (int i = 0; i < kn && i < kMaxKeys; ++i) g_keys[i].hitsAtMark = g_keys[i].hits.load();
        std::snprintf(line, sizeof(line), "[TF2VR] MESHKEYS MARK (F6) at %llu slot-170 calls, %d keys snapshotted.\n",
                      static_cast<unsigned long long>(markCalls), kn);
        Tf2VrLog(line);
    }
    f6Was = f6Now;
    if (markAt && ((markReports == 0 && now - markAt >= 10000) || (markReports == 1 && now - markAt >= 25000))) {
        ++markReports;
        const std::uint64_t callsNow = g_calls170.load();
        const std::uint64_t after = callsNow > markCalls ? callsNow - markCalls : 1;
        const int kn = g_keyCount.load(std::memory_order_acquire);
        std::snprintf(line, sizeof(line), "[TF2VR] MESHKEYS MARK report %d: %llu calls before the mark, %llu after; rates in draws per 1000 calls\n",
                      markReports, static_cast<unsigned long long>(markCalls), static_cast<unsigned long long>(after));
        Tf2VrLog(line);
        for (int i = 0; i < kn && i < kMaxKeys; ++i) {
            const Key& k = g_keys[i];
            if (!k.vb.load(std::memory_order_acquire)) continue;
            const std::uint64_t h = k.hits.load();
            const std::uint64_t hb = k.hitsAtMark <= h ? k.hitsAtMark : h;  // keys born after the mark have 0
            const double before = markCalls ? 1000.0 * static_cast<double>(hb) / static_cast<double>(markCalls) : 0.0;
            const double post = 1000.0 * static_cast<double>(h - hb) / static_cast<double>(after);
            const char* verdict = (before > 0.5 && post < 0.05) ? "GONE" : (before < 0.05 && post > 0.5) ? "NEW"
                                  : (before > 0.5 && post > 0.5 && post < before * 0.5) ? "HALVED"
                                  : (before > 0.5 && post > 0.5) ? "steady" : "rare";
            std::snprintf(line, sizeof(line), "[TF2VR]   MARK cnt %6u vb %p: before %8.2f  after %8.2f  %s\n", k.count,
                          reinterpret_cast<void*>(k.vb.load()), before, post, verdict);
            Tf2VrLog(line);
        }
    }
    // The key table, grouped by vertex buffer, at 25 s and 45 s after install.
    static int tablesPrinted = 0;
    const std::uint64_t since = now - installedAt;
    if ((tablesPrinted == 0 && since >= 25000) || (tablesPrinted == 1 && since >= 45000) || (tablesPrinted == 2 && since >= 65000) || (tablesPrinted == 3 && since >= 85000)) {
        ++tablesPrinted;
        const int kn = g_keyCount.load(std::memory_order_acquire);
        std::snprintf(line, sizeof(line), "[TF2VR] MESHKEYS table %d at %llu slot-170 calls: %d keys (vb, offset, count), %llu past the cap, %llu vertex buffers, "
                      "%llu index buffers\n", tablesPrinted, static_cast<unsigned long long>(g_calls170.load()), kn,
                      static_cast<unsigned long long>(g_keyOverflow.load()),
                      static_cast<unsigned long long>(g_meshes.used.load()), static_cast<unsigned long long>(g_states.used.load()));
        Tf2VrLog(line);
        // One line per vertex buffer: its keys ascending by offset (up to 12), so a model reads as one row.
        bool printed[kMaxKeys]{};
        int rows = 0;
        for (int i = 0; i < kn && i < kMaxKeys && rows < 96; ++i) {
            if (printed[i]) continue;
            const std::uintptr_t vb = g_keys[i].vb.load(std::memory_order_acquire);
            if (!vb) continue;
            int n = std::snprintf(line, sizeof(line), "[TF2VR]   VB %p ib %llX stride %u:", reinterpret_cast<void*>(vb),
                                  static_cast<unsigned long long>(g_keys[i].ib & 0xFFFFFFFFFull), g_keys[i].stride);
            int shown = 0;
            for (int pass = 0; pass < 12; ++pass) {
                int best = -1;
                for (int j = 0; j < kn && j < kMaxKeys; ++j) {
                    if (printed[j] || g_keys[j].vb.load(std::memory_order_relaxed) != vb) continue;
                    if (best < 0 || g_keys[j].off < g_keys[best].off) best = j;
                }
                if (best < 0) break;
                printed[best] = true;
                ++shown;
                if (n < static_cast<int>(sizeof(line)) - 48) {
                    n += std::snprintf(line + n, sizeof(line) - n, " [off %u cnt %u st %u x%llu %s]", g_keys[best].off,
                                       g_keys[best].count, g_keys[best].start, static_cast<unsigned long long>(g_keys[best].hits.load()),
                                       (g_keys[best].d5CSeen & 0x7F00) && (g_keys[best].d5CSeen & 0x200) ? "LR" : "");
                }
            }
            int more = 0;
            for (int j = 0; j < kn && j < kMaxKeys; ++j) if (!printed[j] && g_keys[j].vb.load(std::memory_order_relaxed) == vb) { printed[j] = true; ++more; }
            std::snprintf(line + n, sizeof(line) - n, "%s%d keys\n", more ? " ..." : "  ", shown + more);
            Tf2VrLog(line);
            ++rows;
        }
    }
    const int dumps = g_dumpCount.load(std::memory_order_acquire);
    for (; g_dumpsPrinted < dumps; ++g_dumpsPrinted) {
        const CallDump& cd = g_dumps[g_dumpsPrinted];
        if (!cd.ready.load(std::memory_order_acquire)) break;
        std::snprintf(line, sizeof(line), "[TF2VR] DUMPCALL %d: %d items, %d groups by (+0x58,+0x5A), %d past the group cap\n",
                      g_dumpsPrinted, cd.itemCount, cd.groupCount, cd.overflow);
        Tf2VrLog(line);
        // Size histogram first: how many groups of 1, 2, 3-4, 5, 6-8, 9-16, 17+ items.
        int hist[7]{};
        for (int k = 0; k < cd.groupCount; ++k) {
            const std::uint32_t n = cd.g[k].n;
            hist[n <= 1 ? 0 : n == 2 ? 1 : n <= 4 ? 2 : n == 5 ? 3 : n <= 8 ? 4 : n <= 16 ? 5 : 6]++;
        }
        std::snprintf(line, sizeof(line), "[TF2VR]   groups by size: 1:%d 2:%d 3-4:%d 5:%d 6-8:%d 9-16:%d 17+:%d\n", hist[0],
                      hist[1], hist[2], hist[3], hist[4], hist[5], hist[6]);
        Tf2VrLog(line);
        for (int k = 0; k < cd.groupCount; ++k) {
            const Group& g = cd.g[k];
            if (cd.itemCount > 40 && (g.n < 3 || g.n > 16)) continue;  // small calls (the viewmodel pass?) print whole
            int n = std::snprintf(line, sizeof(line), "[TF2VR]   GRP w58=%u w5A=%u n=%u  counts:", g.w58, g.w5A, g.n);
            const int shown = g.n < kGroupSamples ? static_cast<int>(g.n) : kGroupSamples;
            for (int s = 0; s < shown; ++s) n += std::snprintf(line + n, sizeof(line) - n, " %u", g.count4[s]);
            n += std::snprintf(line + n, sizeof(line) - n, "  first:");
            for (int s = 0; s < shown; ++s) n += std::snprintf(line + n, sizeof(line) - n, " %u", g.first0[s]);
            n += std::snprintf(line + n, sizeof(line) - n, "  d5C:");
            for (int s = 0; s < shown; ++s) n += std::snprintf(line + n, sizeof(line) - n, " %X", g.d5C[s]);
            n += std::snprintf(line + n, sizeof(line) - n, "  state:");
            for (int s = 0; s < shown && n < static_cast<int>(sizeof(line)) - 40; ++s)
                n += std::snprintf(line + n, sizeof(line) - n, " %llX", static_cast<unsigned long long>(g.state[s] & 0xFFFFFF));
            n += std::snprintf(line + n, sizeof(line) - n, "  pool:");
            for (int s = 0; s < shown && n < static_cast<int>(sizeof(line)) - 24; ++s)
                n += std::snprintf(line + n, sizeof(line) - n, " %llX", static_cast<unsigned long long>(g.pool[s] & 0xFFFFFF));
            std::snprintf(line + n, sizeof(line) - n, "\n");
            Tf2VrLog(line);
        }
    }
}

void RemoveDrawItemCensus() {
    if (g_slot170 && *g_slot170 == reinterpret_cast<void*>(&Wrap170)) {
        DWORD old = 0;
        if (VirtualProtect(g_slot170, sizeof(void*), PAGE_READWRITE, &old)) {
            *g_slot170 = reinterpret_cast<void*>(g_orig170);
            DWORD ignored = 0;
            VirtualProtect(g_slot170, sizeof(void*), old, &ignored);
        }
    }
    if (g_slot171 && *g_slot171 == reinterpret_cast<void*>(&Wrap171)) {
        DWORD old = 0;
        if (VirtualProtect(g_slot171, sizeof(void*), PAGE_READWRITE, &old)) {
            *g_slot171 = reinterpret_cast<void*>(g_orig171);
            DWORD ignored = 0;
            VirtualProtect(g_slot171, sizeof(void*), old, &ignored);
        }
    }
    if (g_slot172 && *g_slot172 == reinterpret_cast<void*>(&Wrap172)) {
        DWORD old = 0;
        if (VirtualProtect(g_slot172, sizeof(void*), PAGE_READWRITE, &old)) {
            *g_slot172 = reinterpret_cast<void*>(g_orig172);
            DWORD ignored = 0;
            VirtualProtect(g_slot172, sizeof(void*), old, &ignored);
        }
    }
    g_slot170 = g_slot171 = g_slot172 = nullptr;
}
