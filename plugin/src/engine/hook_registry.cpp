#include "hook_registry.h"

#include <atomic>
#include <cstdint>
#include <cstdio>

#include "diagnostics.h"

namespace {

// Fixed storage, no allocation, because the reader is a crash handler. 128 is
// several times the project's current site count (about 30 across 13 modules
// with every diagnostic armed); the registration path says so out loud if it
// ever fills rather than silently dropping -- a dropped entry here would
// recreate the exact `unknown+0x0` blindness this exists to end.
constexpr int kMaxEntries = 128;

struct Entry {
    const char* name;
    std::uintptr_t base;
    std::size_t length;
    // Published with release AFTER the fields above are written; the handler
    // reads it with acquire before trusting them. A slot claimed but not yet
    // ready is simply skipped.
    std::atomic_bool ready;
};

Entry g_entries[kMaxEntries];
std::atomic<int> g_claimed{0};
std::atomic_bool g_overflowReported{false};

}  // namespace

void RegisterHookSite(const char* name, const void* address, std::size_t length) {
    if (!name || !address || !length) return;
    const auto base = reinterpret_cast<std::uintptr_t>(address);
    // Idempotence scan: a re-install (aim_cmd, weapon_settings and the D3D
    // detours all install lazily and can be asked twice) must not add a second
    // row for the same bytes.
    const int published = g_claimed.load(std::memory_order_acquire);
    for (int i = 0; i < published && i < kMaxEntries; ++i) {
        if (g_entries[i].ready.load(std::memory_order_acquire) && g_entries[i].base == base) return;
    }
    const int slot = g_claimed.fetch_add(1, std::memory_order_acq_rel);
    if (slot >= kMaxEntries) {
        if (!g_overflowReported.exchange(true, std::memory_order_acq_rel)) {
            Tf2VrLog("[TF2VR] hook registry FULL: a site went unregistered, so the next "
                     "`unknown+0x0` crash line may be blind to its owner. Raise kMaxEntries.\n");
        }
        return;
    }
    g_entries[slot].name = name;
    g_entries[slot].base = base;
    g_entries[slot].length = length;
    g_entries[slot].ready.store(true, std::memory_order_release);
    char line[280]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] hook registry [%d]: '%s' %p +0x%zX. The crash recorder resolves faulting PCs "
        "against this table.\n",
        slot, name, address, length);
    Tf2VrLog(line);
}

const char* LookupHookSite(const void* pc, const void** baseOut, std::size_t* lengthOut) {
    const auto at = reinterpret_cast<std::uintptr_t>(pc);
    const int published = g_claimed.load(std::memory_order_acquire);
    const int limit = published < kMaxEntries ? published : kMaxEntries;
    for (int i = 0; i < limit; ++i) {
        if (!g_entries[i].ready.load(std::memory_order_acquire)) continue;
        if (at >= g_entries[i].base && at < g_entries[i].base + g_entries[i].length) {
            if (baseOut) *baseOut = reinterpret_cast<const void*>(g_entries[i].base);
            if (lengthOut) *lengthOut = g_entries[i].length;
            return g_entries[i].name;
        }
    }
    return nullptr;
}

int HookRegistryCount() {
    const int published = g_claimed.load(std::memory_order_acquire);
    return published < kMaxEntries ? published : kMaxEntries;
}

void HookRegistrySelfCheck() {
    // NEGATIVE control: an address inside this DLL. crash.selftest faults with
    // its PC in titanfall2vr.dll, and that fault must keep resolving to the
    // module -- a registry that claimed it would be attributing our own code to
    // a hook and every attribution it made would be suspect.
    const void* ownAddress = reinterpret_cast<const void*>(&HookRegistrySelfCheck);
    const char* negative = LookupHookSite(ownAddress, nullptr, nullptr);

    // POSITIVE control: the first published entry must resolve to itself. An
    // instrument that has never been seen to hit proves nothing by staying
    // silent, so if there are no entries yet the line says the positive half
    // is VOID rather than implying it passed.
    const char* positive = nullptr;
    const int count = HookRegistryCount();
    if (count > 0 && g_entries[0].ready.load(std::memory_order_acquire)) {
        positive = LookupHookSite(reinterpret_cast<const void*>(g_entries[0].base), nullptr, nullptr);
    }

    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] hook registry self-check: %d entries. NEGATIVE (own DLL address must not match): "
        "%s. POSITIVE (a registered address must match): %s. The live half -- a real trampoline "
        "fault naming its owner -- only a real crash can prove.\n",
        count,
        negative ? "FAILED -- the registry claims our own code and its attributions are void"
                 : "pass",
        count == 0 ? "VOID -- no entries published yet at check time"
                   : (positive ? "pass" : "FAILED -- a published entry does not resolve to itself"));
    Tf2VrLog(line);
}
