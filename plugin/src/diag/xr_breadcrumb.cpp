#include "xr_breadcrumb.h"

#include <windows.h>

#include <atomic>
#include <cstdio>

namespace XrBreadcrumb {
namespace {

const char* const kStageName[] = {
    "Idle", "WaitFrame", "BeginFrame", "AcquireImage", "WaitImage",
    "CopyIntoImage", "ReleaseImage", "EndFrame", "SwapchainCreate", "SwapchainDestroy",
};

std::atomic<int> g_stage{0};
std::atomic<unsigned long long> g_stageEnteredMs{0};

// Per eye: the pointer submitted, and the swapchain epoch that was current when
// it was submitted. If those two epochs disagree at crash time, we handed the
// runtime a resource from before the last swapchain rebuild.
std::atomic<const void*> g_tex[2]{};
std::atomic<unsigned int> g_texEpoch[2]{};

std::atomic<unsigned int> g_epoch{0};
std::atomic<unsigned long long> g_lastChangeMs{0};
std::atomic<unsigned int> g_creates{0};
std::atomic<unsigned int> g_destroys{0};

}  // namespace

void SetStage(Stage stage) {
    g_stage.store(static_cast<int>(stage), std::memory_order_relaxed);
    g_stageEnteredMs.store(GetTickCount64(), std::memory_order_relaxed);
}

void NoteSubmittedTexture(int eye, const void* texture) {
    if (eye < 0 || eye > 1) return;
    g_tex[eye].store(texture, std::memory_order_relaxed);
    g_texEpoch[eye].store(g_epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void NoteSwapchainChanged(bool created) {
    if (created) {
        g_creates.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_destroys.fetch_add(1, std::memory_order_relaxed);
    }
    g_epoch.fetch_add(1, std::memory_order_relaxed);
    g_lastChangeMs.store(GetTickCount64(), std::memory_order_relaxed);
}

void Describe(char* out, unsigned int cap) {
    if (!out || cap < 64) return;
    const int stage = g_stage.load(std::memory_order_relaxed);
    const char* name = (stage >= 0 && stage < static_cast<int>(sizeof(kStageName) / sizeof(kStageName[0])))
                           ? kStageName[stage] : "?";
    const unsigned long long now = GetTickCount64();
    const unsigned long long enter = g_stageEnteredMs.load(std::memory_order_relaxed);
    const unsigned long long change = g_lastChangeMs.load(std::memory_order_relaxed);
    const unsigned int epoch = g_epoch.load(std::memory_order_relaxed);
    const unsigned int e0 = g_texEpoch[0].load(std::memory_order_relaxed);
    const unsigned int e1 = g_texEpoch[1].load(std::memory_order_relaxed);

    // THE VERDICT IS SPELLED OUT, not left to be inferred at 3am from two
    // numbers. A submitted texture whose epoch predates the current one is a
    // resource we kept across a swapchain rebuild.
    const bool stale = (g_tex[0].load(std::memory_order_relaxed) && e0 != epoch) ||
                       (g_tex[1].load(std::memory_order_relaxed) && e1 != epoch);

    std::snprintf(out, cap,
        "stage=%s (entered %llu ms ago) | swapchain epoch=%u (%u created, %u destroyed, last "
        "change %llu ms ago) | submitted eye0=%p@epoch%u eye1=%p@epoch%u | %s",
        name, enter ? now - enter : 0ULL, epoch,
        g_creates.load(std::memory_order_relaxed), g_destroys.load(std::memory_order_relaxed),
        change ? now - change : 0ULL,
        g_tex[0].load(std::memory_order_relaxed), e0,
        g_tex[1].load(std::memory_order_relaxed), e1,
        stale ? "*** STALE SUBMIT: a texture was handed over from BEFORE the last swapchain "
                "change. If the runtime faulted, this is why and it is OURS ***"
              : "submitted textures are from the current swapchain epoch");
}

}  // namespace XrBreadcrumb
