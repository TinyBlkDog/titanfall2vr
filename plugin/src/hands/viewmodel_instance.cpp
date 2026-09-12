#include "viewmodel_instance.h"

#include "viewmodel_bones.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {

// The bone array the scan hands back sits at this offset inside the object, so
// walking back from the array to the object needs no new scan.
constexpr std::size_t kViewmodelBoneOffset = 0x1870;
constexpr std::uintptr_t kViewmodelVtableRva = 0x8A86C8;
constexpr std::size_t kAttachmentBoneOffset = 0x1260;
constexpr std::uintptr_t kAttachmentVtableRva = 0x8B8158;

bool ReadableFrom(const void* address, std::size_t bytes) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    return start + info.RegionSize - at >= bytes;
}

bool VtableMatches(const unsigned char* instance, std::uintptr_t rva) {
    if (!instance || !ReadableFrom(instance, 8)) return false;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return false;
    const auto expected = reinterpret_cast<std::uintptr_t>(client) + rva;
    __try {
        return *reinterpret_cast<const std::uintptr_t*>(instance) == expected;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// ASK THE ENGINE, DO NOT WALK THE HEAP.
//
// The 4.7 GB scan blocks the main thread for about three seconds. Flat that is
// merely unpleasant; with the VR runtime pumping it killed four armed sessions
// in a row, and the SEH guards never fired -- because the fault was never a
// read fault in our thread at all. Stalling the main thread for seconds while
// an XR session is live is the problem, and no amount of guarding the reads
// addresses it.
//
// The engine already knows where its viewmodel is:
//
//   GetLocalPlayer      client.dll+0x14EF00   no args; reads a global index
//   GetViewModelEntity  client.dll+0x14FFA0   rcx = player; reads the EHANDLE
//                                             at [player+0x2638] and resolves it
//
// GetLocalPlayer is recorded as working in the 08-18 handoff, having already
// been called from a plugin frame. GetViewModelEntity resolves through the
// native wrapper at 0x145480 and a jmp thunk at 0x2C7FB0; the destination is a
// real .pdata FUNCTION ENTRY, checked rather than assumed.
//
// The RESULT IS VALIDATED by vtable before anything uses it, so a wrong or
// stale pointer is refused rather than driven. That is what makes calling into
// the engine here safe: we do not have to trust the call, only check what it
// hands back.
constexpr std::uintptr_t kGetLocalPlayerRva = 0x14EF00;
constexpr std::uintptr_t kGetViewModelEntityRva = 0x14FFA0;
constexpr std::uint8_t kGetLocalPlayerPrologue[] = {0x8B, 0x05};
constexpr std::uint8_t kGetViewModelPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10,
                                                  0x57, 0x48, 0x83, 0xEC, 0x20};

const unsigned char* FindViewmodelViaEngine() {
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return nullptr;
    auto* base = reinterpret_cast<std::uint8_t*>(client);
    if (std::memcmp(base + kGetLocalPlayerRva, kGetLocalPlayerPrologue,
                    sizeof(kGetLocalPlayerPrologue)) != 0) {
        return nullptr;
    }
    if (std::memcmp(base + kGetViewModelEntityRva, kGetViewModelPrologue,
                    sizeof(kGetViewModelPrologue)) != 0) {
        return nullptr;
    }
    using GetLocalPlayerFn = void*(__fastcall*)();
    using GetViewModelFn = void*(__fastcall*)(void*);
    auto getLocalPlayer = reinterpret_cast<GetLocalPlayerFn>(base + kGetLocalPlayerRva);
    auto getViewModel = reinterpret_cast<GetViewModelFn>(base + kGetViewModelEntityRva);
    __try {
        void* player = getLocalPlayer();
        if (!player) return nullptr;
        void* viewmodel = getViewModel(player);
        if (!viewmodel) return nullptr;
        return static_cast<const unsigned char*>(viewmodel);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// THE ARMS, WITHOUT THE BONE SCAN.
//
// FindByClass below reads the bone scan's CACHE. If the scan never ran this
// session the cache is empty and the lookup returns null -- which reads
// exactly like "the arms do not exist" and would have made the FH1 probe look
// broken rather than unarmed. The scan itself is not an option: it walks 4.7 GB
// and blocks the main thread about three seconds, which killed four headset
// runs.
//
// The engine has no native that hands back the attachment ENTITY
// (LookupViewModelAttachment returns an attachment INDEX on a model, not this),
// and the handoff's pointer-link scan found no parent/child pointer in the
// first 0x2000 bytes of either object -- the link is by handle.
//
// So go at the handle table instead. The engine's own EHANDLE resolve, read out
// of the code at client.dll+0x374D29:
//
//   list   = *(void**)(client + 0xB0F030)     ; array of 32-byte entries
//   index  = handle & 0xFFFF                  ; refused at >= 0x4000
//   entity = *(void**)(list + index*32 + 8)
//   serial = *(uint32*)(list + index*32 + 16)
//
// Walking all 0x4000 slots is 16k pointer reads out of one 512 KB array --
// microseconds, and bounded by a constant the engine itself enforces. It is
// not a heap walk and must not be confused with one. Every candidate is
// accepted only on a vtable match, so a wrong answer is refused rather than
// driven, exactly as with the engine call above.
constexpr std::uintptr_t kEntityListRva = 0xB0F030;
constexpr std::size_t kEntityListStride = 32;
constexpr std::size_t kEntityListEntityOffset = 8;
constexpr int kEntityListMax = 0x4000;

// Every entity in the handle table whose vtable matches, not just the first.
// FH1 resolved "the arms" to whichever instance came first and found EF_NODRAW
// already set on it -- which is exactly what an inactive or pooled attachment
// looks like, so "we picked the wrong one" had to be ruled out before anything
// was concluded about the lever.
int ForEachInEntityList(std::uintptr_t vtableRva, const unsigned char** out, int max);

const unsigned char* FindInEntityList(std::uintptr_t vtableRva) {
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return nullptr;
    auto* base = reinterpret_cast<std::uint8_t*>(client);
    if (!ReadableFrom(base + kEntityListRva, 8)) return nullptr;

    const std::uint8_t* list = nullptr;
    __try {
        list = *reinterpret_cast<const std::uint8_t* const*>(base + kEntityListRva);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (!list) return nullptr;
    if (!ReadableFrom(list, kEntityListStride * kEntityListMax)) return nullptr;

    for (int index = 0; index < kEntityListMax; ++index) {
        const std::uint8_t* entity = nullptr;
        __try {
            entity = *reinterpret_cast<const std::uint8_t* const*>(
                list + static_cast<std::size_t>(index) * kEntityListStride +
                kEntityListEntityOffset);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (!entity) continue;
        if (VtableMatches(entity, vtableRva)) return entity;
    }
    return nullptr;
}

int ForEachInEntityList(std::uintptr_t vtableRva, const unsigned char** out, int max) {
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client || !out || max <= 0) return 0;
    auto* base = reinterpret_cast<std::uint8_t*>(client);
    if (!ReadableFrom(base + kEntityListRva, 8)) return 0;

    const std::uint8_t* list = nullptr;
    __try {
        list = *reinterpret_cast<const std::uint8_t* const*>(base + kEntityListRva);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (!list) return 0;
    if (!ReadableFrom(list, kEntityListStride * kEntityListMax)) return 0;

    int found = 0;
    for (int index = 0; index < kEntityListMax && found < max; ++index) {
        const std::uint8_t* entity = nullptr;
        __try {
            entity = *reinterpret_cast<const std::uint8_t* const*>(
                list + static_cast<std::size_t>(index) * kEntityListStride +
                kEntityListEntityOffset);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (!entity) continue;
        if (VtableMatches(entity, vtableRva)) out[found++] = entity;
    }
    return found;
}

const unsigned char* FindByClass(const char* wanted, std::size_t boneOffset, std::uintptr_t rva) {
    const void* array = nullptr;
    int boneCount = 0;
    const char* className = nullptr;
    for (int slot = 0; slot < 8; ++slot) {
        if (!GetWeaponBoneTargetArray(slot, &array, &boneCount, &className)) continue;
        if (!className || std::strcmp(className, wanted) != 0) continue;
        const auto* instance = static_cast<const unsigned char*>(array) - boneOffset;
        if (VtableMatches(instance, rva)) return instance;
    }
    return nullptr;
}

}  // namespace

bool ViewmodelInstanceStillValid(const unsigned char* instance) {
    return VtableMatches(instance, kViewmodelVtableRva);
}

bool AttachmentInstanceStillValid(const unsigned char* instance) {
    return VtableMatches(instance, kAttachmentVtableRva);
}

const unsigned char* FindLiveViewmodelInstance() {
    // The engine first, because it is instant and cannot stall a live XR
    // session. Validated by vtable, so a wrong answer is refused rather than
    // driven. The cached bone-scan result stays as a fallback for anything that
    // already ran the scan.
    if (const unsigned char* viaEngine = FindViewmodelViaEngine()) {
        if (ViewmodelInstanceStillValid(viaEngine)) return viaEngine;
    }
    return FindByClass("viewmodel", kViewmodelBoneOffset, kViewmodelVtableRva);
}

const unsigned char* FindLiveAttachmentInstance() {
    // The bone scan's cache first, because when it IS populated it is the
    // answer this project has already driven; the handle table second, because
    // it works with no scan at all. Both are vtable-validated.
    if (const unsigned char* cached =
            FindByClass("attachment", kAttachmentBoneOffset, kAttachmentVtableRva)) {
        return cached;
    }
    return FindInEntityList(kAttachmentVtableRva);
}

int EnumerateAttachmentInstances(const unsigned char** out, int max) {
    return ForEachInEntityList(kAttachmentVtableRva, out, max);
}

int EnumerateViewmodelInstances(const unsigned char** out, int max) {
    return ForEachInEntityList(kViewmodelVtableRva, out, max);
}
