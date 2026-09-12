#include "view_block_camera.h"

#include "camera_update_hook.h"   // HalfInterpupillaryUnits: AER's IPD, reused not reinvented
#include "diagnostics.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// --- The block, from the build at client+0x359250. See the header. ---------
constexpr std::size_t kBlockBytes = 0x200;
constexpr std::size_t kOffOrigin = 0x00;    // vec3, written by 0x358690
constexpr std::size_t kOffView = 0x40;      // 4x4 row-major, translation in column 3
constexpr std::size_t kOffProj = 0x80;
constexpr std::size_t kOffViewProj = 0xC0;
constexpr std::size_t kOffFov = 0x180;
constexpr std::size_t kOffViewport = 0x190;  // int x, y, w, h
constexpr std::size_t kOffCustomFlag = 0x1B8;

// The three blocks the caller finalises with 0x36A060 immediately before the
// scene draw (client+0x35AAA5, +0x35AAB8, +0x35AAC4).
constexpr std::uintptr_t kViewBlockRvas[3] = {0x12ECC0, 0xA13C0, 0xB55C0};

// THE TWO THAT MUST MOVE TOGETHER, AND THE RUN THAT PROVED IT.
//
// The first build of this rung moved only the block the scene draw is HANDED
// (arg2, this+0x12ECC0). The write was clean -- unit right axis, residual
// 0.0001, and batch 2's uploaded world camera came back at exactly the moved
// position while batch 1's stayed put -- and the private eye came back BLACK,
// with only the Titan's outline glow and the crosshair surviving.
//
// The scene draw reads the two blocks for two different halves of one camera:
//
//     client+0x3725DE   lea r12, [r15+0xA13C0]        r15 is `this`
//     client+0x3725F1   mov eax, [r12]      -> [rbx+0x30]      origin.x
//     client+0x3725FA   mov eax, [r12+0x4]  -> [rbx+0x34]      origin.y
//     client+0x372602   mov eax, [r12+0x8]  -> [rbx+0x38]      origin.z
//     client+0x37260A   the same triple again, into [rbx+0x3C..0x44]
//
// and it never reads +0x00..+0x08 of its arg2 block at all. So the MATRICES
// come from block 0 and the ORIGIN comes from block 1. The world renders
// camera-relative -- vertices arrive as (world - origin) and are transformed by
// a clip matrix with no translation of its own -- so moving one and not the
// other does not move the camera, it tears the pair apart, and every world
// pixel lands somewhere invalid. Things drawn later or with different depth
// handling survive, which is exactly the black eye that was captured.
//
// This is the same shape as eliminated #1 and #2, which patched c_cameraOrigin
// alone and c_cameraRelativeToClip alone: each moved one half of the same pair.
// The difference is that the halves are now named, addressed and checked
// against each other before either is touched.
// The sky camera (index 2) is deliberately NOT written: it is a separate block
// the write never reaches, so the far field takes no offset rather than the 16x
// over-parallax the shipped upload-side guard exists to prevent.
constexpr int kWrittenBlocks = 2;

// The engine's own builders. Called, never reimplemented.
constexpr std::uintptr_t kMatrixMultiplyRva = 0x635370;   // Mul(A, B, Out), row-major 4x4
constexpr std::uintptr_t kRebuildOriginRva = 0x358690;    // origin/basis <- view matrix

// FIRST BYTES OF EACH, FROM THE BINARY. The resolve refuses when they differ,
// for the same reason every other site in this plugin does: a patched or
// relocated target would be called blind, and a wrong call here runs inside the
// frame the wearer is looking at.
constexpr std::uint8_t kMatrixMultiplyPrologue[] = {0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00};
constexpr std::uint8_t kRebuildOriginPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57,
                                                   0x48, 0x83, 0xEC, 0x40};

using MatrixMultiplyFn = void(*)(const void* a, const void* b, void* out);
using RebuildOriginFn = void(*)(void* block);

MatrixMultiplyFn g_matrixMultiply = nullptr;
RebuildOriginFn g_rebuildOrigin = nullptr;
bool g_resolved = false;
bool g_resolveRefused = false;

std::atomic<float> g_engineIpd{0.0f};
std::atomic_bool g_wanted{false};    // the ini put a value here at all
std::atomic_bool g_poisoned{false};

// THE MAGNITUDE LADDER, and why this replaced a plain armed/disarmed toggle.
//
// Two runs have now moved batch 2's camera correctly -- the engine's own
// recovered origin agrees to 0.001, and the uploaded world camera diverged from
// batch 1's by exactly the offset asked for -- and both came back with a blank
// eye. The census says the draws are still ISSUED: 1283 DrawIndexed on the
// armed intervals against 1288 on the control, target for target. Geometry that
// is submitted and produces nothing is geometry the GPU rejected, and the
// captured backbuffer shows why that is worth testing before another mechanism
// is invented: the camera sits in a corridor with concrete about a metre to
// either side, and 120 world units is roughly 2.3 metres SIDEWAYS. A camera
// inside solid geometry gives exactly this picture -- everything backface-
// culled or behind the near plane, a flat fog fill, and the overlays that
// ignore depth surviving.
//
// So the next variable is the MAGNITUDE, not the mechanism. One press steps the
// ladder, one burst photographs each rung, and the magnitude is in the filename
// and in the log at the moment of capture rather than inferred afterwards. A
// run that shows the world at 6 units and blank at 120 has proved the lever and
// bounded it in one sitting; a run that is blank at every rung including 6 has
// refuted the corridor explanation just as cheaply.
constexpr int kLadderRungs = 4;
const float kLadderFractions[kLadderRungs] = {1.0f, 0.2f, 0.05f, 0.0f};
std::atomic_int g_ladderRung{0};
int g_loggedForRung[kLadderRungs]{};

// THE LADDER TOP, AND WHERE ITS MAGNITUDE COMES FROM.
//
// `stereo.engine_ipd` was a hand-picked number whose only job was to make the
// shift unmistakable while the mechanism was in doubt. 24 units is ~0.46 m,
// about seven times a human IPD. The mechanism is no longer in doubt -- the
// wearer confirmed the parallax from the frame -- so the magnitude should come
// from the same place AER's has always come from.
//
// A NEGATIVE `stereo.engine_ipd` MEANS "DERIVE IT". The source is
// `HalfInterpupillaryUnits()`, which AER already maintains: it is seeded from a
// sane default, overridden by `stereo.half_ipd_units` if the ini names one, and
// -- on a headset run -- replaced by the value the RUNTIME reports for the
// wearer's actual eyes. Nothing about IPD is reinvented here.
//
// DOUBLED, and that is the one piece of arithmetic worth stating. AER moves
// each eye by HALF an IPD either side of centre. This path moves ONLY batch 2
// and leaves batch 1 on the engine's own camera, so the separation BETWEEN the
// two batches is the whole IPD in one write. Handing it a half would render a
// pair half as far apart as the wearer's eyes.
//
// It is read LIVE rather than latched at config time, because the runtime's
// value does not exist until the XR session is up -- a value cached during
// startup would be the default forever, on exactly the runs where the real one
// matters.
float LadderTopIpd() {
    const float configured = g_engineIpd.load(std::memory_order_acquire);
    if (configured >= 0.0f) return configured;
    return 2.0f * HalfInterpupillaryUnits();
}

float ActiveIpd() {
    const int rung = g_ladderRung.load(std::memory_order_acquire);
    if (rung < 0 || rung >= kLadderRungs) return 0.0f;
    return LadderTopIpd() * kLadderFractions[rung];
}

// Counters. Every early return is one of these; a silent decline reads exactly
// like a hook that stopped running.
std::atomic_uint64_t g_writes{0};
std::atomic_uint64_t g_declineNotWanted{0};
std::atomic_uint64_t g_declineZeroIpd{0};
std::atomic_uint64_t g_declineUnresolved{0};
std::atomic_uint64_t g_declinePoisoned{0};
std::atomic_uint64_t g_declineBadBasis{0};
std::atomic_uint64_t g_declineBadBottomRow{0};
std::atomic_uint64_t g_declineUnreadable{0};
// The two blocks are supposed to hold ONE camera. If they do not, moving both
// by the same offset is not obviously the right thing and the run says so
// instead of guessing -- this is the gate the first build did not have.
std::atomic_uint64_t g_declineSiblingMismatch{0};
std::atomic_uint64_t g_declineBlockAddress{0};
std::atomic_uint64_t g_residualFailures{0};
std::atomic_uint64_t g_restores{0};
std::atomic_uint64_t g_restoreDeclinedRebuilt{0};
std::atomic_uint64_t g_faults{0};

// The snapshot pair. `before` is what the block held when we arrived; `after`
// is what we left in it. The restore compares the live block against `after`
// and only puts `before` back when they are identical -- so a block the engine
// has already rebuilt is never clobbered with a stale camera.
alignas(16) std::uint8_t g_blockBefore[kWrittenBlocks][kBlockBytes]{};
alignas(16) std::uint8_t g_blockAfter[kWrittenBlocks][kBlockBytes]{};
std::uint8_t* g_writtenBlock[kWrittenBlocks]{};
bool g_holdOutstanding = false;

bool IsReadable(const void* address, std::size_t bytes) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!address || !VirtualQuery(address, &mbi, sizeof(mbi))) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto end = begin + bytes;
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    const DWORD protect = mbi.Protect & 0xFF;
    return mbi.State == MEM_COMMIT && end >= begin && end <= regionEnd &&
           protect != PAGE_NOACCESS && protect != PAGE_GUARD;
}

bool Finite3(const float* v) {
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(v[i])) return false;
    }
    return true;
}

void Resolve() {
    if (g_resolved || g_resolveRefused) return;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return;
    const auto base = reinterpret_cast<std::uintptr_t>(client);
    auto* mul = reinterpret_cast<std::uint8_t*>(base + kMatrixMultiplyRva);
    auto* reb = reinterpret_cast<std::uint8_t*>(base + kRebuildOriginRva);
    if (!IsReadable(mul, sizeof(kMatrixMultiplyPrologue)) ||
        !IsReadable(reb, sizeof(kRebuildOriginPrologue))) {
        return;
    }
    const bool mulOk = std::memcmp(mul, kMatrixMultiplyPrologue,
                                   sizeof(kMatrixMultiplyPrologue)) == 0;
    const bool rebOk = std::memcmp(reb, kRebuildOriginPrologue,
                                   sizeof(kRebuildOriginPrologue)) == 0;
    if (!mulOk || !rebOk) {
        char line[400]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] N2: REFUSING to resolve the engine's view builders. "
            "client+0x%llX prologue %s, client+0x%llX prologue %s. The eye write needs both and "
            "will not fire; nothing is patched.\n",
            static_cast<unsigned long long>(kMatrixMultiplyRva), mulOk ? "matches" : "DIFFERS",
            static_cast<unsigned long long>(kRebuildOriginRva), rebOk ? "matches" : "DIFFERS");
        Tf2VrLog(line);
        g_resolveRefused = true;
        return;
    }
    g_matrixMultiply = reinterpret_cast<MatrixMultiplyFn>(mul);
    g_rebuildOrigin = reinterpret_cast<RebuildOriginFn>(reb);
    g_resolved = true;
    Tf2VrLog("[TF2VR] N2: engine view builders resolved and prologue-verified -- "
             "client+0x635370 (viewproj = proj * view) and client+0x358690 (origin/basis <- view "
             "matrix). The eye offset is one float in the view matrix; both derived blocks are "
             "rebuilt by the engine, not by us.\n");
}

// The two engine calls, behind SEH. They are the only foreign code this rung
// executes and they run inside the frame being looked at.
bool RebuildGuarded(std::uint8_t* blk, DWORD* codeOut) {
    __try {
        g_matrixMultiply(blk + kOffProj, blk + kOffView, blk + kOffViewProj);
        g_rebuildOrigin(blk);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *codeOut = GetExceptionCode();
        return false;
    }
}

}  // namespace

void SetEngineIpd(float units) {
    g_engineIpd.store(units, std::memory_order_release);
    g_wanted.store(true, std::memory_order_release);
    char line[640]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] stereo.engine_ipd = %.3f world units. N2/C-ENGINE: between pass 1 and the "
        "nested pass, the engine's OWN view matrix (this+0x12ECC0 +0x40) has its row-0 column-3 "
        "term moved by that much and the engine's own builders rebuild the viewproj and the "
        "camera origin. Nothing D3D-side is touched. %s\n",
        units,
        units < 0.0f
            ? "NEGATIVE MEANS DERIVE IT FROM AER: the magnitude is taken live from "
              "HalfInterpupillaryUnits() and DOUBLED, because only batch 2 moves and the "
              "separation between the batches is therefore the whole IPD. On a headset run that "
              "is the wearer's own IPD as the runtime reports it; flat, it is the default or "
              "stereo.half_ipd_units. Read live, not latched -- the runtime's value does not "
              "exist until the session is up."
        : units == 0.0f
            ? "ZERO IS THE POSITIVE CONTROL: the block is left alone and the two batches must "
              "upload BYTE-IDENTICAL cameras."
            : "The write reports its own result before any GPU work: the engine's recovered "
              "origin must come back moved by exactly this along camera right.");
    Tf2VrLog(line);
    if (units < 0.0f) {
        char derived[300]{};
        std::snprintf(derived, sizeof(derived),
            "[TF2VR] N2 ladder top RESOLVES to %.3f world units right now (half-IPD %.3f x 2). "
            "This is re-read at every write, so a runtime IPD arriving later moves it.\n",
            LadderTopIpd(), HalfInterpupillaryUnits());
        Tf2VrLog(derived);
    }
}

float EngineIpd() { return LadderTopIpd(); }

float EngineIpdActive() {
    if (!g_wanted.load(std::memory_order_acquire) ||
        g_poisoned.load(std::memory_order_acquire)) {
        return 0.0f;
    }
    return ActiveIpd();
}

bool EngineCameraWriteArmed() { return EngineIpdActive() != 0.0f; }

void ToggleEngineCameraWrite() {
    if (!g_wanted.load(std::memory_order_acquire)) {
        Tf2VrLog("[TF2VR] N2: key pressed, but stereo.engine_ipd is not in the ini. Nothing to "
                 "step.\n");
        return;
    }
    if (g_poisoned.load(std::memory_order_acquire)) {
        Tf2VrLog("[TF2VR] N2: key pressed, but the engine-side write is POISONED after a fault "
                 "and stays off for this session.\n");
        return;
    }
    const int rung = (g_ladderRung.load(std::memory_order_acquire) + 1) % kLadderRungs;
    g_ladderRung.store(rung, std::memory_order_release);
    const float active = ActiveIpd();
    char line[520]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] N2 MAGNITUDE LADDER -> rung %d of %d: %.2f world units. %s\n",
        rung + 1, kLadderRungs, active,
        active == 0.0f
            ? "THE POSITIVE CONTROL: the write is off, so the next burst's pair must show the "
              "SAME viewpoint and bmpshift must read dx=0 dy=0 in every band."
            : "The next burst moves batch 2's camera this far along camera right. The magnitude "
              "is in the capture's filename, so no two rungs of this run can overwrite each "
              "other.");
    Tf2VrLog(line);
}

std::atomic_bool g_symmetric{false};
bool SymmetricEyePairArmed() { return g_symmetric.load(std::memory_order_acquire); }

void ToggleSymmetricEyePair() {
    const bool on = !g_symmetric.load(std::memory_order_acquire);
    g_symmetric.store(on, std::memory_order_release);
    // REOPEN THE WRITE LOG. The cap is per ladder rung, so without this every
    // write line in a session is from before the toggle and the run cannot say
    // what the toggle did -- which is exactly how the last run came back
    // inconclusive.
    for (int i = 0; i < kLadderRungs; ++i) g_loggedForRung[i] = 0;
    char line[620]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] SYMMETRIC EYE PAIR %s (F3). %s\n", on ? "ARMED" : "OFF",
        on ? "Batch 1 is written to -half an IPD before pass 1 and batch 2's existing RELATIVE "
             "write then carries it to +half, so the pair straddles the head. The N2 WRITE lines "
             "report BEFORE and AFTER per write, so the log says whether the two writes actually "
             "accumulated or whether the engine rebuilt the block between them."
           : "THE POSITIVE CONTROL: only batch 2 moves, so the pair keeps its separation but sits "
             "half an IPD to one side and both eyes should slide sideways when a burst arms.");
    Tf2VrLog(line);
}

bool WriteBatch2Camera(void* self, void* viewBlock, float scale) {
    Resolve();
    if (!g_wanted.load(std::memory_order_acquire)) {
        g_declineNotWanted.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (g_poisoned.load(std::memory_order_acquire)) {
        g_declinePoisoned.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const float ipd = ActiveIpd() * scale;
    if (ipd == 0.0f) {
        g_declineZeroIpd.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!g_resolved) {
        g_declineUnresolved.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // THE PAIR. Block 0 is the one the draw is handed and supplies the
    // MATRICES; block 1 is this+0xA13C0 and supplies the ORIGIN the draw
    // actually copies out (client+0x3725F1). Both or neither.
    std::uint8_t* blk[kWrittenBlocks] = {
        static_cast<std::uint8_t*>(viewBlock),
        reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uintptr_t>(self) +
                                        kViewBlockRvas[1]),
    };
    for (int b = 0; b < kWrittenBlocks; ++b) {
        if (!blk[b] || !IsReadable(blk[b], kBlockBytes)) {
            g_declineUnreadable.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    // The arg2 block is supposed to BE this+0x12ECC0. If the engine ever hands
    // the draw a different one, block 1 is not its sibling and pairing them is
    // an assumption rather than a fact.
    if (blk[0] != reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uintptr_t>(self) +
                                                  kViewBlockRvas[0])) {
        if (g_declineBlockAddress.fetch_add(1, std::memory_order_relaxed) == 0) {
            char line[300]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] N2 DECLINED: arg2 block is %p but this+0x12ECC0 is %p. The sibling at "
                "this+0xA13C0 cannot be assumed to pair with it. Nothing written.\n",
                blk[0], reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(self) +
                                                kViewBlockRvas[0]));
            Tf2VrLog(line);
        }
        return false;
    }

    // --- THE GATE, and it names what it checked. ---------------------------
    // Row 0 of a matrix built by 0x636420 out of sines and cosines is the unit
    // camera-right axis. If it is not unit here, this is not that matrix, and
    // the one-float identity the whole write rests on does not hold. Checked on
    // BOTH blocks before EITHER is touched, because a half-write is precisely
    // the defect the black eye of the first run was.
    float view[kWrittenBlocks][16]{};
    float originBefore[kWrittenBlocks][3]{};
    float rightLen[kWrittenBlocks]{};
    for (int b = 0; b < kWrittenBlocks; ++b) {
        std::memcpy(view[b], blk[b] + kOffView, sizeof(view[b]));
        const float* v = view[b];
        rightLen[b] = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (!std::isfinite(rightLen[b]) || rightLen[b] < 0.99f || rightLen[b] > 1.01f) {
            if (g_declineBadBasis.fetch_add(1, std::memory_order_relaxed) == 0) {
                char line[400]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] N2 DECLINED: block %d view-matrix row 0 is (%.5f, %.5f, %.5f), "
                    "length %.5f -- not the unit right axis this write requires. The block at "
                    "+0x40 is not the matrix 0x636420 builds. NEITHER block written.\n",
                    b, v[0], v[1], v[2], rightLen[b]);
                Tf2VrLog(line);
            }
            return false;
        }
        if (v[12] != 0.0f || v[13] != 0.0f || v[14] != 0.0f || v[15] != 1.0f) {
            if (g_declineBadBottomRow.fetch_add(1, std::memory_order_relaxed) == 0) {
                char line[360]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] N2 DECLINED: block %d view-matrix bottom row is (%.4f, %.4f, %.4f, "
                    "%.4f), not (0,0,0,1) -- the translation is not in column 3 where this write "
                    "puts it. NEITHER block written.\n", b, v[12], v[13], v[14], v[15]);
                Tf2VrLog(line);
            }
            return false;
        }
        std::memcpy(originBefore[b], blk[b] + kOffOrigin, sizeof(originBefore[b]));
    }

    // THE SIBLING GATE. The two blocks are two halves of ONE camera, so they
    // have to agree before either is moved. If they ever hold different cameras
    // then this offset does not apply equally to both and the run says so
    // rather than producing another unreadable picture.
    {
        float gap = 0.0f;
        for (int i = 0; i < 3; ++i) {
            const float d = originBefore[0][i] - originBefore[1][i];
            gap += d * d;
        }
        gap = std::sqrt(gap);
        const float rightGap = std::fabs(view[0][0] - view[1][0]) +
                               std::fabs(view[0][1] - view[1][1]) +
                               std::fabs(view[0][2] - view[1][2]);
        if (!std::isfinite(gap) || gap > 0.5f || rightGap > 0.01f) {
            if (g_declineSiblingMismatch.fetch_add(1, std::memory_order_relaxed) == 0) {
                char line[520]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] N2 DECLINED: the two blocks do not hold the same camera. "
                    "this+0x12ECC0 origin (%.2f, %.2f, %.2f), this+0xA13C0 origin "
                    "(%.2f, %.2f, %.2f) -- %.2f units apart, right-axis difference %.5f. The "
                    "draw takes its MATRICES from the first and its ORIGIN from the second, so "
                    "one offset cannot serve both when they disagree. NEITHER block written.\n",
                    originBefore[0][0], originBefore[0][1], originBefore[0][2],
                    originBefore[1][0], originBefore[1][1], originBefore[1][2], gap, rightGap);
                Tf2VrLog(line);
            }
            return false;
        }
    }

    for (int b = 0; b < kWrittenBlocks; ++b) std::memcpy(g_blockBefore[b], blk[b], kBlockBytes);

    // THE WHOLE EYE OFFSET, ON BOTH BLOCKS. Row 0, column 3 of a view matrix
    // with orthonormal rows is -dot(right, eye); moving the eye along right by
    // `ipd` subtracts exactly `ipd` from it and leaves every other element
    // alone. The engine's own 0x358690 then recomputes each block's origin from
    // its own matrix, so the matrix half and the origin half stay one camera.
    for (int b = 0; b < kWrittenBlocks; ++b) {
        const float moved = view[b][3] - ipd;
        std::memcpy(blk[b] + kOffView + 3 * sizeof(float), &moved, sizeof(moved));
        DWORD faultCode = 0;
        if (!RebuildGuarded(blk[b], &faultCode)) {
            // Put EVERY block back byte for byte, including the ones already
            // rebuilt, before anything else looks at any of them.
            for (int r = 0; r <= b; ++r) std::memcpy(blk[r], g_blockBefore[r], kBlockBytes);
            g_faults.fetch_add(1, std::memory_order_relaxed);
            g_poisoned.store(true, std::memory_order_release);
            char line[360]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] N2: the engine's view rebuild FAULTED on block %d, code 0x%08X. All "
                "blocks are restored byte for byte and the engine-side write is POISONED for "
                "this session. Pass 1 is untouched.\n", b, static_cast<unsigned>(faultCode));
            Tf2VrLog(line);
            return false;
        }
    }

    // --- THE WRITE'S OWN FALSIFIER, BEFORE ANY GPU WORK. ------------------
    // 0x358690 recovers the camera origin by inverting the view rotation, so
    // the engine now says where IT thinks each camera is. Both have to be the
    // old origin plus ipd along right; if either is not, nothing downstream is
    // worth judging.
    float originAfter[kWrittenBlocks][3]{};
    float residual[kWrittenBlocks]{};
    bool residualOk = true;
    for (int b = 0; b < kWrittenBlocks; ++b) {
        std::memcpy(originAfter[b], blk[b] + kOffOrigin, sizeof(originAfter[b]));
        float sum = 0.0f;
        for (int i = 0; i < 3; ++i) {
            const float d = originAfter[b][i] - (originBefore[b][i] + view[b][i] * ipd);
            sum += d * d;
        }
        residual[b] = std::sqrt(sum);
        if (!std::isfinite(residual[b]) || residual[b] >= 0.05f || !Finite3(originAfter[b])) {
            residualOk = false;
        }
    }
    if (!residualOk) g_residualFailures.fetch_add(1, std::memory_order_relaxed);

    for (int b = 0; b < kWrittenBlocks; ++b) {
        std::memcpy(g_blockAfter[b], blk[b], kBlockBytes);
        g_writtenBlock[b] = blk[b];
    }
    g_holdOutstanding = true;
    const unsigned long long n = g_writes.fetch_add(1, std::memory_order_relaxed);

    // PER RUNG, NOT PER SESSION, and the first ladder run is why. This was
    // `n < 3` -- the first three writes of the whole session -- so on a run that
    // steps four magnitudes only the TOP rung ever printed its numbers, and the
    // two rungs that actually rendered a world wrote completely unlogged. The
    // question that then could not be answered from the log was whether the
    // offset ACCUMULATES across a burst: eight writes at 6 units with no
    // intervening engine rebuild would put the eye at 48, and the BEFORE origin
    // printed on each of a rung's first writes is exactly what settles that.
    // Namespace-scope so the SYMMETRY TOGGLE can clear it. This cap is per
    // ladder rung, so every write line in the last run was logged BEFORE F3 was
    // pressed and the run could not say whether the symmetric write ever fired.
    // A cap that silences the only frames anyone cares about is worse than no
    // log at all, because it looks like a complete record.
    // (declaration moved to g_loggedForRung)
    const int rung = g_ladderRung.load(std::memory_order_relaxed);
    const bool logThis = rung >= 0 && rung < kLadderRungs && g_loggedForRung[rung] < 3;
    if (logThis) ++g_loggedForRung[rung];
    if (logThis) {
        char line[900]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] N2 WRITE %llu (rung %d, %.2f units, scale %+.2f, symmetry %s): self=%p ipd=%.2f right=(%.4f, %.4f, %.4f) len=%.4f | "
            "MATRIX block this+0x12ECC0 %p: (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f) residual "
            "%.4f | ORIGIN block this+0xA13C0 %p: (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f) "
            "residual %.4f | %s. Both halves of one camera-relative camera, moved together; "
            "these are the ENGINE'S own recovered origins out of client+0x358690.\n",
            n, rung + 1, ipd, scale, SymmetricEyePairArmed() ? "ARMED" : "off", self, ipd, view[0][0], view[0][1], view[0][2], rightLen[0],
            blk[0], originBefore[0][0], originBefore[0][1], originBefore[0][2],
            originAfter[0][0], originAfter[0][1], originAfter[0][2], residual[0],
            blk[1], originBefore[1][0], originBefore[1][1], originBefore[1][2],
            originAfter[1][0], originAfter[1][1], originAfter[1][2], residual[1],
            residualOk ? "PASS: the engine agrees both moved"
                       : "FAIL: the engine does not agree, so nothing downstream on this frame "
                         "is evidence");
        Tf2VrLog(line);
    }
    return true;
}

void RestoreBatch2CameraIfUntouched() {
    if (!g_holdOutstanding) return;
    g_holdOutstanding = false;
    for (int b = 0; b < kWrittenBlocks; ++b) {
        std::uint8_t* blk = g_writtenBlock[b];
        g_writtenBlock[b] = nullptr;
        if (!blk || !IsReadable(blk, kBlockBytes)) continue;
        if (std::memcmp(blk, g_blockAfter[b], kBlockBytes) != 0) {
            // The expected path: client+0x35A9BB rebuilds the block from its
            // source before every scene draw, so by the time we are back here
            // the engine has already undone us. Counted rather than assumed.
            g_restoreDeclinedRebuilt.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        std::memcpy(blk, g_blockBefore[b], kBlockBytes);
        g_restores.fetch_add(1, std::memory_order_relaxed);
    }
}

void CensusViewBlocks(void* self) {
    if (!self) return;
    const auto base = reinterpret_cast<std::uintptr_t>(self);
    float mainOrigin[3]{};
    bool haveMain = false;
    for (int i = 0; i < 3; ++i) {
        auto* blk = reinterpret_cast<std::uint8_t*>(base + kViewBlockRvas[i]);
        if (!IsReadable(blk, kBlockBytes)) {
            char line[200]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] N2 VIEW-BLOCK[%d] this+0x%llX: UNREADABLE.\n", i,
                static_cast<unsigned long long>(kViewBlockRvas[i]));
            Tf2VrLog(line);
            continue;
        }
        float origin[3]{};
        std::memcpy(origin, blk + kOffOrigin, sizeof(origin));
        float right[3]{};
        std::memcpy(right, blk + kOffView, sizeof(right));
        float fov = 0.0f;
        std::memcpy(&fov, blk + kOffFov, sizeof(fov));
        int viewport[4]{};
        std::memcpy(viewport, blk + kOffViewport, sizeof(viewport));
        std::uint32_t custom = 0;
        std::memcpy(&custom, blk + kOffCustomFlag, sizeof(custom));
        float distance = -1.0f;
        if (i == 0) {
            std::memcpy(mainOrigin, origin, sizeof(mainOrigin));
            haveMain = Finite3(origin);
        } else if (haveMain && Finite3(origin)) {
            const float dx = origin[0] - mainOrigin[0];
            const float dy = origin[1] - mainOrigin[1];
            const float dz = origin[2] - mainOrigin[2];
            distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        char line[720]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] N2 VIEW-BLOCK[%d] this+0x%llX%s: origin (%.2f, %.2f, %.2f) right "
            "(%.4f, %.4f, %.4f) fov %.2f viewport %d,%d %dx%d custom-matrix %u | distance from "
            "block 0 %.1f. %s\n",
            i, static_cast<unsigned long long>(kViewBlockRvas[i]),
            i == 0 ? " (THE ONE THE SCENE DRAW IS GIVEN)" : "",
            origin[0], origin[1], origin[2], right[0], right[1], right[2], fov,
            viewport[0], viewport[1], viewport[2], viewport[3], custom, distance,
            distance < 0.0f
                ? "Block 0 is the reference for the distances below."
                : (distance > 1000.0f
                       ? "OVER 1000 UNITS AWAY -- this is the 3D SKY CAMERA. It is a SEPARATE "
                         "block, so the eye write never reaches it and the far field takes no "
                         "offset at all: a far-field disparity of ZERO, not the 16x "
                         "over-parallax the shipped upload-side guard exists to prevent."
                       : "Within 1000 units of the main camera -- not the sky camera."));
        Tf2VrLog(line);
    }
}

void TickEngineCameraWrite() {
    const bool wasResolved = g_resolved;
    Resolve();
    if (!g_resolved || wasResolved) return;
    // Resolve() already logged that both prologues matched. This second line
    // exists because the first one can only be read as "the module loaded"; this
    // one says the rung is WIRED -- the ini value arrived, the key is bound, and
    // the only thing left is a burst.
    char line[640]{};
    const float top = g_engineIpd.load(std::memory_order_relaxed);
    std::snprintf(line, sizeof(line),
        "[TF2VR] N2 READY: stereo.engine_ipd = %.2f is the ladder TOP. F12 steps the magnitude "
        "%.0f -> %.0f -> %.0f -> 0, and each burst (F6) photographs the rung it is on with that "
        "magnitude in the capture's filename. Starting rung: %.2f world units. If the top reads "
        "0.00 here, the ini this run loaded does not carry the value and no burst will measure "
        "anything.\n",
        top, top * kLadderFractions[0], top * kLadderFractions[1], top * kLadderFractions[2],
        ActiveIpd());
    Tf2VrLog(line);
}

void ReportEngineCameraWrite() {
    char line[980]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] N2 STATUS: wanted=%d ladder-rung=%d poisoned=%d ipd-top=%.2f active=%.2f resolved=%d | writes %llu, "
        "residual FAILURES %llu, faults %llu | restores: performed %llu, declined-because-engine-"
        "already-rebuilt %llu | declines: not-wanted %llu, zero-ipd %llu, "
        "unresolved %llu, poisoned %llu, bad-basis %llu, bad-bottom-row %llu, unreadable %llu, "
        "sibling-mismatch %llu, block-address %llu. "
        "Writes of zero with every decline also zero means this was never reached at all, which "
        "is a different failure from every decline listed here.\n",
        g_wanted.load(std::memory_order_relaxed) ? 1 : 0,
        g_ladderRung.load(std::memory_order_relaxed) + 1,
        g_poisoned.load(std::memory_order_relaxed) ? 1 : 0,
        g_engineIpd.load(std::memory_order_relaxed), ActiveIpd(), g_resolved ? 1 : 0,
        g_writes.load(std::memory_order_relaxed),
        g_residualFailures.load(std::memory_order_relaxed),
        g_faults.load(std::memory_order_relaxed),
        g_restores.load(std::memory_order_relaxed),
        g_restoreDeclinedRebuilt.load(std::memory_order_relaxed),
        g_declineNotWanted.load(std::memory_order_relaxed),
        g_declineZeroIpd.load(std::memory_order_relaxed),
        g_declineUnresolved.load(std::memory_order_relaxed),
        g_declinePoisoned.load(std::memory_order_relaxed),
        g_declineBadBasis.load(std::memory_order_relaxed),
        g_declineBadBottomRow.load(std::memory_order_relaxed),
        g_declineUnreadable.load(std::memory_order_relaxed),
        g_declineSiblingMismatch.load(std::memory_order_relaxed),
        g_declineBlockAddress.load(std::memory_order_relaxed));
    Tf2VrLog(line);
}
