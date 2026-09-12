#include "frustum_census.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "diagnostics.h"

namespace {

// The layout constants camera_update_hook.cpp uses for a camera upload. The
// call site is gated on `desc.ByteWidth == 576`, so reading the 16-float
// matrix at byte 16 (bytes 16..79) is in bounds by construction.
constexpr std::size_t kOriginOffset = 4;
constexpr std::size_t kClipOffset = 16;

// SHIPS OFF. A diagnostic that a player can meet is a support burden, and one
// armed by default costs log volume and frame time on every machine that is not
// the developer's. Turn it on from the ini to use it.
std::atomic_bool g_enabled{true};

struct Family {
    float m[16];       // the WHOLE matrix, so nothing has to be re-run to answer
                       // an arithmetic question about it later
    float origin[3];
    float originMag;
    float len0, len1;  // |row0| and |row1|: the rotation-invariant scale terms
    bool perspective;
    std::atomic_uint64_t count;
};
// Generous on purpose: a family that does not fit is counted as overflow and
// SAID SO, but 24 is well past the handful of shapes this engine uploads.
constexpr int kMaxFamilies = 24;
Family g_family[kMaxFamilies];
// Written by the RENDER thread, read by the game thread. Published with a
// release store AFTER the entry is fully written, so the reader never walks a
// half-built row.
std::atomic<int> g_familyCount{0};
std::atomic_uint64_t g_samples{0};
std::atomic_uint64_t g_overflow{0};
std::atomic_uint64_t g_nan{0};

bool CloseEnough(float a, float b) {
    return std::fabs(a - b) < 0.002f * (1.0f + std::fabs(a));
}

}  // namespace

void SetFrustumCensusEnabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    Tf2VrLog(enabled ? "[TF2VR] frustum.census = 1: every 576-byte camera upload is bucketed by its own "
                       "projection matrix, and each family's FULL matrix is printed. Read-only.\n"
                     : "[TF2VR] frustum.census = 0.\n");
}

void FrustumCensusSample(const std::uint8_t* cameraBytes) {
    if (!g_enabled.load(std::memory_order_relaxed) || !cameraBytes) return;
    float m[16]{};
    float origin[3]{};
    std::memcpy(origin, cameraBytes + kOriginOffset, sizeof(origin));
    std::memcpy(m, cameraBytes + kClipOffset, sizeof(m));
    for (int i = 0; i < 16; ++i) {
        if (!(m[i] == m[i])) {
            g_nan.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    g_samples.fetch_add(1, std::memory_order_relaxed);

    // PERSPECTIVE vs ORTHO by the SAME test this codebase already uses:
    // IsScreenSpaceHudPass treats element 15 non-zero as the ortho pixel pass.
    // The raw elements are printed either way, so if this convention is wrong
    // it is visible in the numbers rather than hidden behind a label.
    const bool perspective = std::fabs(m[15]) < 0.001f;

    // THE ROW LENGTH, NOT ELEMENT ZERO. The camera-relative-to-clip matrix
    // carries the view ROTATION as well as the projection, so its first row is
    // the scale times the camera's right vector: m[0] alone swings as the head
    // turns, while |row0| is the invariant. Using m[0] made the first version of
    // this census fragment into 24 families with 49,650 overflows and print
    // half-tangents in the thousands. The upload census three thousand lines
    // away has always used |r0| and |r1|, which is why its numbers were stable.
    const float len0 = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    const float len1 = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);

    const int published = g_familyCount.load(std::memory_order_acquire);
    for (int i = 0; i < published; ++i) {
        Family& f = g_family[i];
        if (f.perspective == perspective && CloseEnough(f.len0, len0) && CloseEnough(f.len1, len1) &&
            ((f.originMag < 1.0f) == (std::sqrt(origin[0] * origin[0] + origin[1] * origin[1] +
                                                origin[2] * origin[2]) < 1.0f))) {
            f.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    if (published >= kMaxFamilies) {
        g_overflow.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Family& f = g_family[published];
    std::memcpy(f.m, m, sizeof(m));
    std::memcpy(f.origin, origin, sizeof(origin));
    f.originMag =
        std::sqrt(origin[0] * origin[0] + origin[1] * origin[1] + origin[2] * origin[2]);
    f.len0 = len0;
    f.len1 = len1;
    f.perspective = perspective;
    f.count.store(1, std::memory_order_relaxed);
    g_familyCount.store(published + 1, std::memory_order_release);
}

void FrustumCensusReport() {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    static std::uint64_t nextMs = 0;
    const std::uint64_t now = GetTickCount64();
    if (now < nextMs) return;
    nextMs = now + 20000;

    const std::uint64_t samples = g_samples.load(std::memory_order_relaxed);
    const int count = g_familyCount.load(std::memory_order_acquire);
    char line[700]{};
    // SAY IT ONCE. The empty branch reached quiet mode only because its own
    // text contains "is not installed", which KeepWhenQuiet allowlists -- so a
    // census with nothing to report printed every five seconds while the branch
    // carrying the actual frustums was filtered away. Exactly backwards.
    if (samples == 0) {
        static bool saidOnce = false;
        if (!saidOnce) {
            saidOnce = true;
            Tf2VrLog("[TF2VR] FRUSTUM census: NO 576-BYTE CAMERA UPLOADS SEEN AT ALL. The camera upload hook "
                     "is not installed yet (flat profile, or autoarm off), so this says NOTHING about the "
                     "frustums -- it is not a negative result. Said once; the census keeps looking.\n");
        }
        return;
    }
    std::snprintf(line, sizeof(line),
        "[TF2VR] FRUSTUM census: %llu uploads, %d families (overflow %llu, NaN %llu). The headset's own "
        "half-tangents are 1.42815 x 1.00000; any PERSPECTIVE family whose tangents differ is projecting "
        "through a frustum other than the one displayed, and that ratio is the scale we are hunting.\n",
        static_cast<unsigned long long>(samples), count,
        static_cast<unsigned long long>(g_overflow.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_nan.load(std::memory_order_relaxed)));
    // FORCED. This and the family lines below are the zoom evidence: the ratio
    // between a family's tangents and the headset's own IS the magnification.
    Tf2VrLogWrite(line, true);
    for (int i = 0; i < count; ++i) {
        const Family& f = g_family[i];
        const float tanX = (f.perspective && f.len0 > 1e-6f) ? 1.0f / f.len0 : 0.0f;
        const float tanY = (f.perspective && f.len1 > 1e-6f) ? 1.0f / f.len1 : 0.0f;
        std::snprintf(line, sizeof(line),
            "[TF2VR]   F%d %s halfTan %.5f x %.5f (aspect %.4f) | origin %s |%.1f,%.1f,%.1f| | uploads %llu\n",
            i, f.perspective ? "PERSP" : "ORTHO", tanX, tanY, tanY > 1e-6f ? tanX / tanY : 0.0f,
            f.originMag < 1.0f ? "AT ZERO" : "in world", f.origin[0], f.origin[1], f.origin[2],
            static_cast<unsigned long long>(f.count.load(std::memory_order_relaxed)));
        Tf2VrLogWrite(line, true);
        // THE WHOLE MATRIX, EVERY FAMILY, EVERY REPORT. If a later question
        // needs an element this summary did not think to derive, it is already
        // in the log and does not cost another run.
        std::snprintf(line, sizeof(line),
            "[TF2VR]   F%d m: %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f | "
            "%.5f %.5f %.5f %.5f\n",
            i, f.m[0], f.m[1], f.m[2], f.m[3], f.m[4], f.m[5], f.m[6], f.m[7], f.m[8], f.m[9],
            f.m[10], f.m[11], f.m[12], f.m[13], f.m[14], f.m[15]);
        Tf2VrLog(line);
    }
}
