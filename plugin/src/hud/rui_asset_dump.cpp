#include "rui_asset_dump.h"

#include "diagnostics.h"
#include "rui_probe.h"
#include "camera_hook.h"
#include "camera_update_hook.h"
#include "weapon_settings.h"
#include "rui_layer_probe.h"
#include "rui_text_filter.h"
#include "rui_transform_scan.h"
#include "titan_state.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

// The engine view angles the marker census also reads; the absolute view yaw is
// body + head and is the only correct correlate for a world-anchored element.
extern "C" volatile float g_headBaseAngles[3];
// Half of plugin.cpp's world gate; see RuiCensusWorldIsUp below for why it is
// rebuilt here rather than called.
extern "C" volatile std::uint64_t g_cameraHookCallCount;

// ---- WHAT THE OFFLINE READ ESTABLISHED (pescan, engine.dll, selftest 1.4363%
// bad; ui(11).dll 1.1127% bad). 2026-09-06.
//
// The scratch the caller hands FC500 (layer+0x18, `lea rax,[rsp+0x1820]` at
// FC868) is the RUI VIRTUAL MACHINE's state, and interface slot +0x30
// (engine+0xF56B0, 93 bytes) is its interpreter:
//
//     movzx edx, byte [pc]; inc pc; call [5F42A0 + rdx*8]   until pc == end
//
// Fourteen opcode handlers, all register maths, none of them draws. The layout
// every handler agrees on:
//
//   scratch+0x0000   the program counter        (F56B0 reads and writes it)
//   scratch+0x0008   the register count         (F4160: mov eax,[rdx+8]; inc)
//   scratch+0x05BC   a 0x200C-byte string arena (slots +0x38/+0x40 sprintf into it)
//   scratch+0x2DD0   ARGS: one vec4 per entry, index i at 0x2DD0 + 16*i. This
//                    is "the block" the census reads; A00 is the space.
//   scratch+0x3A80   REGISTERS: 32 bytes per entry, index i at 0x3A80 + 32*i.
//
// So the three pipelines rui_hook.asm decodes are just registers 0, 1 and 2.
// F4210 shows what a register IS, and the first run's dump confirmed every
// part of it:
//
//   lo = [b0x, b0y, b1x, b1y]   the element's two basis vectors
//   hi = [x, y, x, y]           its position, duplicated
//
// both normalised -- x lanes by the space WIDTH (A00 lane 0), y lanes by the
// space HEIGHT (A00 lane 2). Axis-aligned elements carry lo = [w, 0, 0, h].
//
// ---- WHAT THE 2026-09-06 RUN MEASURED -------------------------------------
//
// Eleven registers. Multiplying one register's hi x by 2 and asking the wearer
// what moved, plus eight dumps, named every part of the waypoint by its own
// basis, in pixels of the space:
//
//   R03  10 x 10        R07  36 x 36
//   R04  239 x 32   the DISTANCE TEXT box
//   R05  746 x 36
//   R06  49 x 28        R09  8 x 8
//   R08  64 x 64    the TARGET ICON -- the only register that tracks the world
//   R10  rotated    the LEADER LINE
//
// R10 is the line and this is proven, not assumed: its two basis vectors are
// exactly perpendicular in PIXEL space (dot 6058 - 6058 = 0 in dump #1, 1542.5
// - 1542.8 in dump #3, which also proves the per-axis normalisation above),
// its long axis is 2165 px in the broken frame and 242 px in a healthy one,
// and its far end -- hi + b0 -- lands on the target icon's CENTRE to within
// 0.004 of the screen. It is anchored at the TEXT end and reaches the target.
//
// THE DEFECT, as a number. Every authored register (3..10 except 8) sits at one
// of exactly two x positions: 0.48, where the text sits beside the target, or
// 0.08. The difference is 0.400 in all four dumps that show it and in all 59
// F6 presses, and **y is identical in both states** (0.39125 either way). So it
// is a pure horizontal displacement of the whole authored block, not a scale
// and not a drift: at 0.08 the text is 8% across a 110-degree field, which is
// the wearer's "WAAAY off to the left side where it can't be read". The target
// icon is untouched by it, which is why they report the target as correct.
//
// ---- THE FIX ---------------------------------------------------------------
//
// Pin the authored block's x and let the engine keep everything else. Every
// write is a delta against what the program just computed:
//
//   dx = pin - R04.hi.x          measured this draw, from this widget
//   every authored register's hi x lanes += dx      (y untouched)
//   the leader line's b0.x -= dx                    (its far end stays on the
//                                                    target it already reached)
//   the line's thickness vector is rotated to stay perpendicular to the new
//   long axis, at its original length, in pixel space
//
// The target icon is excluded, and which register that is has a NAMED GATE:
// index 8 AND a basis measuring 64 x 64 px. If the gate fails the whole
// correction is skipped for that draw and counted, because a widget whose
// target register we cannot identify is one where shifting "everything else"
// could move the target.
//
// A healthy draw needs no special case: it already sits at 0.48, so dx is
// within the dead zone and nothing is written.

namespace {

// ---- the pin ---------------------------------------------------------------
// The game's own healthy value, measured across four widget instances and
// thirty seconds: the authored block sits at exactly 0.48 when it is right.
// 0 = off.
constexpr float kGameAuthoredX = 0.48f;
volatile float g_pinX = kGameAuthoredX;
// Below this, the block is already where the pin wants it and nothing is
// written. A healthy draw lands here, which is what keeps the fix inert on the
// draws that were never broken.
constexpr float kDeadZone = 0.005f;

// The F6 ladder, recentred on the wearer's pick. They stepped the whole ladder
// and came back to 0.32, so 0.32 leads and ships; 0.40 was the previous pick
// and stays as the way back.
//
// NO "OFF" RUNG. It had one while the mechanism was unproven; it is proven now
// (14,895 corrected draws, gate clean, wearer: "leader line stays with it
// well"), and this project has already paid once for leaving a confirmed fix
// one keypress from being switched off. `hud.marker_text_x = 0` in the ini
// still turns it off for anyone who wants the game's own layout back.
constexpr float kPinLadder[5] = {0.32f, 0.28f, 0.36f, 0.24f, 0.40f};
int g_pinRung = 0;

// Register roles. Indices 0..2 are the engine's own mask, inset and aspect fit
// and are never touched; 8 is the target icon and must not move.
constexpr unsigned kFirstAuthoredReg = 3;
constexpr unsigned kTargetReg = 8;
constexpr float kTargetIconPx = 64.0f;

// ---- counters. Every early return is counted: a silent one made an earlier
// probe in this project run 7135 times and report nothing.
volatile std::uint64_t g_draws = 0;
volatile std::uint64_t g_corrected = 0;
volatile std::uint64_t g_skipDeadZone = 0;
volatile std::uint64_t g_skipOff = 0;
volatile std::uint64_t g_skipNoProgram = 0;
volatile std::uint64_t g_skipGate = 0;
volatile std::uint64_t g_lineFixed = 0;
volatile std::uint64_t g_faults = 0;
bool g_gateLogged = false;

// The last sample, for the rolling report: all PRE-correction.
float g_lastTextX = 0.0f, g_lastTargetX = 0.0f, g_lastDx = 0.0f;
float g_lastTargetW = 0.0f, g_lastTargetH = 0.0f;
unsigned g_lastRegs = 0;
std::uint64_t g_nextReport = 0;

// ---- the dump --------------------------------------------------------------
bool g_dumpSelected = false;
int g_dumpCount = 0;
constexpr int kDumpCap = 6;
bool g_firstDumpDone = false;

// ---- THE APPEARANCE TRANSIENT ---------------------------------------------
// Wearer, 2026-09-06: "when I first bring up the waypoint indicator it is HUGE
// and after a couple of seconds settles in on the smaller size."
//
// Read-only, and it costs no run of its own because it rides the draws the fix
// is already walking. A window opens whenever the widget has not laid anything
// out for half a second and then does -- which is exactly "the indicator came
// up" -- and each window logs the SIZES the engine computed, in pixels, across
// the settling period. That says what is animating and what it settles to,
// which is what a clamp would need; guessing at it would cost a run.
//
// The widget draws about 180 times a second, so every twelfth draw over the
// first 600 samples the whole settle in about 50 lines, and the session's very
// first laid-out draw opens a window too rather than being spent.
// WEARER, before the run: "the settling only happens the FIRST time the
// waypoint HUD appears. Every time after that is already settled."
//
// That kills the gap-triggered window as the primary instrument. A trigger
// that fires on every appearance spends itself on settled ones, and its first
// firing can be a menu or loading draw the wearer never saw -- this project has
// already had a capped dump sample the menu instead of the gameplay it was
// aimed at. One window is kept as a cheap backstop; the real instrument is the
// SIZE-CHANGE DETECTOR below, which fires on the animation itself wherever it
// happens and cannot be spent on anything else.
constexpr std::uint64_t kAppearanceGapMs = 500;
constexpr int kAppearanceSamples = 600;
constexpr int kAppearanceStride = 12;
constexpr int kAppearanceWindows = 1;

// ---- THE SIZE-CHANGE DETECTOR ---------------------------------------------
// Three quantities are constant in steady state and can only move if something
// is animating: the text box's HEIGHT (its width changes with the distance
// string, so width is useless here), the target icon's two sides, and the
// space itself. Any of them moving by half a percent between consecutive draws
// IS the animation, and each such draw is logged with every size in pixels.
// The first few also take a full dump, so the register that carries it is
// named rather than guessed.
constexpr float kChangeFrac = 0.005f;
constexpr int kChangeLineCap = 240;
constexpr int kChangeDumpCap = 3;
float g_prevTextH = 0.0f, g_prevIconW = 0.0f, g_prevIconH = 0.0f, g_prevSpaceW = 0.0f;
bool SizeMoved(float a, float b) {
    const float ref = a > b ? a : b;
    return ref > 0.0001f && std::fabs(a - b) > ref * kChangeFrac;
}
bool g_havePrev = false;
int g_changeLines = 0;
int g_changeDumps = 0;
bool g_changeNow = false;
std::uint64_t g_firstLaidOutTick = 0;
std::uint64_t g_lastLaidOutTick = 0;
std::uint64_t g_appearanceStartTick = 0;
int g_appearanceIndex = -1;      // -1 = no window open
int g_appearanceWindow = 0;
bool g_appearanceDumpTaken = false;

bool ReadableRange(const void* p, std::size_t len) {
    if (!p || len == 0) return false;
    const auto* b = static_cast<const std::uint8_t*>(p);
    const auto* end = b + len;
    while (b < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(b, &info, sizeof(info)) != sizeof(info)) return false;
        if (info.State != MEM_COMMIT) return false;
        if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        b = static_cast<const std::uint8_t*>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

void DumpHex(const char* tag, const std::uint8_t* p, std::size_t len) {
    char line[240]{};
    for (std::size_t off = 0; off < len; off += 32) {
        int n = std::snprintf(line, sizeof(line), "[TF2VR]   %s+%04zX ", tag, off);
        for (std::size_t i = 0; i < 32 && off + i < len; ++i) {
            n += std::snprintf(line + n, sizeof(line) - n, "%02X%s", p[off + i],
                               ((i & 3) == 3) ? " " : "");
            if (n >= static_cast<int>(sizeof(line)) - 4) break;
        }
        std::snprintf(line + n, sizeof(line) - n, "\n");
        Tf2VrLog(line);
    }
}

// Plain C inside: a function holding __try may not own objects with
// destructors, so everything here is arrays and pointers.
void DumpMarkerAsset(void* layer, std::uint8_t* scratch, const char* reason) {
    __try {
        const auto* L = static_cast<const std::uint8_t*>(layer);
        char line[1200]{};
        if (!ReadableRange(L, 0x40) || !ReadableRange(scratch, 0x3A80 + 32)) {
            Tf2VrLog("[TF2VR] RUI DUMP: layer or scratch not readable; nothing dumped.\n");
            return;
        }
        const auto* desc = *reinterpret_cast<const std::uint8_t* const*>(L);
        const unsigned regs = *reinterpret_cast<const std::uint32_t*>(scratch + 8);
        const auto* pc = *reinterpret_cast<const std::uint8_t* const*>(scratch);
        const std::uint8_t* progBase = nullptr;
        if (ReadableRange(desc, 0x80)) progBase = *reinterpret_cast<const std::uint8_t* const*>(desc + 0x10);
        std::snprintf(line, sizeof(line),
            "[TF2VR] ======== RUI DUMP #%d (%s) ======== marker widget ui(11)+0x%X | hud.marker_size %.2f "
            "| pin %.3f | registers used %u | pc %p base %p (%lld bytes) | layer %p scratch %p | "
            "draws %llu corrected %llu faults %llu. VALUES BELOW ARE POST-CORRECTION.\n",
            g_dumpCount, reason, RuiMarkerTarget(), RuiMarkerSize(), g_pinX, regs,
            static_cast<const void*>(pc), static_cast<const void*>(progBase),
            (progBase && pc) ? static_cast<long long>(pc - progBase) : -1LL,
            layer, static_cast<void*>(scratch),
            static_cast<unsigned long long>(g_draws), static_cast<unsigned long long>(g_corrected),
            static_cast<unsigned long long>(g_faults));
        Tf2VrLog(line);
        DumpHex("layer", L, 0x40);
        if (ReadableRange(desc, 0x80)) DumpHex("desc", desc, 0x80);
        if (progBase && pc > progBase && (pc - progBase) <= 4096 && ReadableRange(progBase, pc - progBase)) {
            DumpHex("prog", progBase, static_cast<std::size_t>(pc - progBase));
        }
        // Registers, in pixels as well as normalised: the pixel basis is what
        // named every element last run and is the readable form.
        const auto* space = reinterpret_cast<const float*>(scratch + 0x2DD0);
        const float sx = space[0], sy = space[2];
        const unsigned show = regs > 48 ? 48 : regs;
        for (unsigned i = 0; i < show; ++i) {
            const auto* r = reinterpret_cast<const float*>(scratch + 0x3A80 + 32 * i);
            std::snprintf(line, sizeof(line),
                "[TF2VR]   R%02u lo %.5g %.5g %.5g %.5g | hi %.5g %.5g | px basis %.1f,%.1f and %.1f,%.1f "
                "| px pos %.1f,%.1f%s\n",
                i, r[0], r[1], r[2], r[3], r[4], r[5],
                r[0] * sx, r[1] * sy, r[2] * sx, r[3] * sy, r[4] * sx, r[5] * sy,
                (i == kTargetReg) ? "   <== TARGET ICON, never moved" : "");
            Tf2VrLog(line);
        }
        for (unsigned i = 0; i < 24; ++i) {
            const auto* a = reinterpret_cast<const float*>(scratch + 0x2DD0 + 16 * i);
            std::snprintf(line, sizeof(line), "[TF2VR]   A%02u (blk+0x%03X) %.5g %.5g %.5g %.5g\n",
                          i, 16 * i, a[0], a[1], a[2], a[3]);
            Tf2VrLog(line);
        }
        if (!ReadableRange(desc, 0x80)) return;
        const unsigned ecount = *reinterpret_cast<const std::uint16_t*>(desc + 0x48);
        const unsigned vcount = *reinterpret_cast<const std::uint16_t*>(desc + 0x4C);
        const auto* etab = *reinterpret_cast<const std::uint8_t* const*>(desc + 0x50);
        const auto* vtab = *reinterpret_cast<const std::uint8_t* const*>(desc + 0x58);
        std::snprintf(line, sizeof(line),
            "[TF2VR]   descriptor: authored %g x %g | elements %u at %p | evaluators %u at %p\n",
            *reinterpret_cast<const float*>(desc + 0x18), *reinterpret_cast<const float*>(desc + 0x1C),
            ecount, static_cast<const void*>(etab), vcount, static_cast<const void*>(vtab));
        Tf2VrLog(line);
        const unsigned eshow = ecount > 40 ? 40 : ecount;
        if (etab && ReadableRange(etab, 0x34 * eshow)) {
            for (unsigned e = 0; e < eshow; ++e) {
                char tag[16]{};
                std::snprintf(tag, sizeof(tag), "E%02u", e);
                DumpHex(tag, etab + 0x34 * e, 0x34);
            }
        }
        std::uint32_t sizes[3] = {0x12, 0x2A, 0x1A};
        if (HMODULE eng = GetModuleHandleA("engine.dll")) {
            const auto* t = reinterpret_cast<const std::uint32_t*>(
                reinterpret_cast<const std::uint8_t*>(eng) + 0x5F43C8);
            if (ReadableRange(t, 12) && t[0] >= 4 && t[0] <= 0x100 && t[1] >= 4 && t[1] <= 0x100 &&
                t[2] >= 4 && t[2] <= 0x100) {
                sizes[0] = t[0]; sizes[1] = t[1]; sizes[2] = t[2];
            }
        }
        if (vtab) {
            const std::uint8_t* p = vtab;
            const unsigned vshow = vcount > 40 ? 40 : vcount;
            for (unsigned v = 0; v < vshow; ++v) {
                if (!ReadableRange(p, 2)) break;
                const unsigned type = *reinterpret_cast<const std::uint16_t*>(p);
                if (type > 2 || !ReadableRange(p, sizes[type])) break;
                char tag[16]{};
                std::snprintf(tag, sizeof(tag), "V%02u", v);
                DumpHex(tag, p, sizes[type]);
                p += sizes[type];
            }
        }
        Tf2VrLog("[TF2VR] ======== RUI DUMP END ========\n");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_faults;
        Tf2VrLog("[TF2VR] RUI DUMP: faulted part-way; whatever printed above is what was readable.\n");
    }
}

void ReportRolling() {
    char line[700]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] MARKER TEXT: pin %.3f | last draw PRE-correction: text block x %.4f, target icon x %.4f, "
        "gap %.4f, shift applied %+.4f | target icon basis %.1f x %.1f px (gate wants %.0f x %.0f) | "
        "registers %u | draws %llu corrected %llu line-fixed %llu | SIZE-CHANGE lines %d of %d, dumps %d "
        "of %d | skipped: pin-off %llu, already-there %llu, no-program %llu, GATE FAILED %llu | "
        "faults %llu\n",
        g_pinX, g_lastTextX, g_lastTargetX, g_lastTargetX - g_lastTextX, g_lastDx,
        g_lastTargetW, g_lastTargetH, kTargetIconPx, kTargetIconPx, g_lastRegs,
        static_cast<unsigned long long>(g_draws), static_cast<unsigned long long>(g_corrected),
        static_cast<unsigned long long>(g_lineFixed),
        g_changeLines, kChangeLineCap, g_changeDumps, kChangeDumpCap,
        static_cast<unsigned long long>(g_skipOff),
        static_cast<unsigned long long>(g_skipDeadZone), static_cast<unsigned long long>(g_skipNoProgram),
        static_cast<unsigned long long>(g_skipGate), static_cast<unsigned long long>(g_faults));
    Tf2VrLog(line);
}

}  // namespace

void RuiMarkerAfterDrawInstrument(void* layer, std::uint8_t* scratch) {
    if (!layer || !scratch) return;
    __try {
        ++g_draws;
        const unsigned regs = *reinterpret_cast<const std::uint32_t*>(scratch + 8);
        g_lastRegs = regs;
        const std::uint64_t now = GetTickCount64();
        // The program did not run this draw (three registers, nothing laid
        // out): there is no text to move and no dump worth taking.
        if (regs <= kTargetReg || regs > 256) {
            ++g_skipNoProgram;
            if (now >= g_nextReport) { g_nextReport = now + 5000; ReportRolling(); }
            return;
        }
        auto* space = reinterpret_cast<float*>(scratch + 0x2DD0);
        const float sx = space[0], sy = space[2];
        auto* regBase = reinterpret_cast<float*>(scratch + 0x3A80);
        auto* text = regBase + 8 * 4;                   // R04, the distance text box
        auto* target = regBase + 8 * kTargetReg;        // R08, the target icon

        const float textX = text[4];
        const float targetX = target[4];
        const float targetW = target[0] * sx, targetH = target[3] * sy;
        g_lastTextX = textX;
        g_lastTargetX = targetX;
        g_lastTargetW = targetW;
        g_lastTargetH = targetH;

        // PRE-CORRECTION SIZES, captured before anything below writes, because
        // the correction shortens the leader line's long axis by dx and a
        // transient measured after that would be reading our own edit back.
        const float preTextW = text[0] * sx, preTextH = text[3] * sy;
        float preLineLen = 0.0f;
        for (unsigned i = kFirstAuthoredReg; i < regs; ++i) {
            const float* r = regBase + 8 * i;
            if (r[1] == 0.0f && r[2] == 0.0f) continue;
            const float lx = r[0] * sx, ly = r[1] * sy;
            preLineLen = std::sqrt(lx * lx + ly * ly);
            break;
        }

        // THE GATE, NAMED: the register we refuse to move must actually be the
        // 64 x 64 target icon. If it is not, this is a marker variant this fix
        // has never seen, and shifting "everything except register 8" could
        // move the target the wearer says is correct. Skip, count, say so once.
        const bool gateOk = std::fabs(targetW - kTargetIconPx) < 1.0f &&
                            std::fabs(targetH - kTargetIconPx) < 1.0f;
        float dx = 0.0f;
        if (!(g_pinX > 0.0f)) {
            ++g_skipOff;
        } else if (!gateOk) {
            ++g_skipGate;
            if (!g_gateLogged) {
                g_gateLogged = true;
                char line[400]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] MARKER TEXT: GATE FAILED and the correction is OFF for these draws. Register %u "
                    "measures %.1f x %.1f px, not the %.0f x %.0f target icon this fix identifies. Nothing "
                    "was moved.\n",
                    kTargetReg, targetW, targetH, kTargetIconPx, kTargetIconPx);
                Tf2VrLog(line);
            }
        } else if (std::fabs(g_pinX - textX) <= kDeadZone) {
            // Already where the pin wants it -- the healthy layout lands here.
            ++g_skipDeadZone;
        } else {
            dx = g_pinX - textX;
            for (unsigned i = kFirstAuthoredReg; i < regs; ++i) {
                if (i == kTargetReg) continue;
                float* r = regBase + 8 * i;
                r[4] += dx;
                r[6] += dx;
            }
            // THE LEADER LINE, kept attached. Its far end (hi + b0) sits on the
            // target icon's centre -- measured to 0.004 last run -- so moving
            // its anchor by dx and shortening b0 by dx leaves that end exactly
            // where the engine put it. The thickness vector is rebuilt
            // perpendicular to the new long axis at its original length, in
            // PIXEL space, because x and y are normalised by different numbers.
            for (unsigned i = kFirstAuthoredReg; i < regs; ++i) {
                if (i == kTargetReg) continue;
                float* r = regBase + 8 * i;
                if (r[1] == 0.0f && r[2] == 0.0f) continue;   // axis-aligned: not the line
                const float b0x = r[0] * sx - dx * sx, b0y = r[1] * sy;
                const float b1x = r[2] * sx, b1y = r[3] * sy;
                const float len0 = std::sqrt(b0x * b0x + b0y * b0y);
                const float len1 = std::sqrt(b1x * b1x + b1y * b1y);
                r[0] = b0x / sx;
                if (len0 > 0.0001f && len1 > 0.0001f) {
                    const float sign = (b0x * b1y - b0y * b1x) >= 0.0f ? 1.0f : -1.0f;
                    r[2] = (-b0y / len0 * len1 * sign) / sx;
                    r[3] = (b0x / len0 * len1 * sign) / sy;
                }
                ++g_lineFixed;
            }
            ++g_corrected;
        }
        g_lastDx = dx;

        // ---- THE SIZE-CHANGE DETECTOR, read-only ---------------------------
        // Fires on the animation itself, so a once-per-session intro cannot be
        // missed by having watched the wrong moment.
        if (!g_firstLaidOutTick) g_firstLaidOutTick = now;
        g_changeNow = !g_havePrev || SizeMoved(preTextH, g_prevTextH) || SizeMoved(targetW, g_prevIconW) ||
                      SizeMoved(targetH, g_prevIconH) || SizeMoved(sx, g_prevSpaceW);
        if (g_changeNow && g_changeLines < kChangeLineCap) {
            ++g_changeLines;
            char line[460]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR]   SIZECHG %03d t+%5llu ms | space %.1f x %.1f | text box %.1f x %.1f px | target "
                "icon %.1f x %.1f px | leader line %.1f px | text x %.4f | registers %u | draw %llu%s\n",
                g_changeLines, static_cast<unsigned long long>(now - g_firstLaidOutTick), sx, sy,
                preTextW, preTextH, targetW, targetH, preLineLen, textX, regs,
                static_cast<unsigned long long>(g_draws),
                g_havePrev ? "" : "   <== FIRST LAID-OUT DRAW, nothing to compare against");
            Tf2VrLog(line);
        }
        g_prevTextH = preTextH;
        g_prevIconW = targetW;
        g_prevIconH = targetH;
        g_prevSpaceW = sx;
        g_havePrev = true;

        // ---- THE APPEARANCE TRANSIENT, read-only ---------------------------
        // A gap in laid-out draws then a laid-out draw IS the indicator coming
        // up. Everything logged is the engine's own value before our edit.
        if ((g_lastLaidOutTick == 0 || now - g_lastLaidOutTick > kAppearanceGapMs) &&
            g_appearanceWindow < kAppearanceWindows) {
            ++g_appearanceWindow;
            g_appearanceIndex = 0;
            g_appearanceStartTick = now;
            g_appearanceDumpTaken = false;
            char line[420]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] ======== MARKER APPEARED (window %d of %d) ======== every %dth draw for %d draws, "
                "sizes in pixels of the widget's own space, all PRE-correction. The wearer reports it comes "
                "up HUGE and settles over about two seconds; these lines are that curve.\n",
                g_appearanceWindow, kAppearanceWindows, kAppearanceStride, kAppearanceSamples);
            Tf2VrLog(line);
        }
        g_lastLaidOutTick = now;
        if (g_appearanceIndex >= 0) {
            if (g_appearanceIndex % kAppearanceStride == 0) {
                char line[420]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR]   APPEAR w%d n%03d t+%4llu ms | space %.0f x %.0f | text box %.1f x %.1f px | "
                    "target icon %.1f x %.1f px | leader line %.1f px | text x %.4f | registers %u\n",
                    g_appearanceWindow, g_appearanceIndex,
                    static_cast<unsigned long long>(now - g_appearanceStartTick),
                    sx, sy, preTextW, preTextH, targetW, targetH, preLineLen, textX, regs);
                Tf2VrLog(line);
            }
            if (++g_appearanceIndex >= kAppearanceSamples) g_appearanceIndex = -1;
        }

        if (now >= g_nextReport) { g_nextReport = now + 5000; ReportRolling(); }

        // THE DUMP, RE-AIMED AT THE DEFECT. Last run its whole budget went on
        // the first draws, which were all healthy, and the broken regime
        // arrived forty seconds later with nothing left to spend. It now fires
        // on the defect itself -- the text block far from the target icon,
        // measured BEFORE the correction -- plus one first draw for reference.
        if (!g_dumpSelected || g_dumpCount >= kDumpCap) return;
        const char* reason = nullptr;
        if (!g_firstDumpDone) { g_firstDumpDone = true; reason = "first laid-out draw, for reference"; }
        // RE-AIMED AGAIN, at the transient. The defect trigger spent five of
        // six dumps in the first forty seconds last run, and the 0.400
        // displacement it was hunting is now measured, corrected and closed.
        // The open question is the appearance animation, so the budget goes
        // there: the first draw of a window is the HUGE state and the one
        // about a second and a half later is the settled state, and the pair
        // names the field that animates.
        // THE ANIMATION ITSELF gets the budget, because the wearer says it
        // happens exactly once per session: a trigger that waits for the right
        // appearance can be spent before they ever see one, a trigger on the
        // sizes moving cannot.
        else if (g_changeNow && g_changeDumps < kChangeDumpCap) {
            ++g_changeDumps;
            reason = "SIZES ARE MOVING -- the intro animation, caught mid-flight";
        }
        else if (!g_appearanceDumpTaken && g_appearanceIndex > 0 &&
                 now - g_appearanceStartTick > 1500) {
            g_appearanceDumpTaken = true;
            reason = "1.5 s after the first appearance -- the settled state, to difference against";
        }
        if (!reason) return;
        ++g_dumpCount;
        DumpMarkerAsset(layer, scratch, reason);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_faults;
    }
}

// ---- THE CLASSIFIER --------------------------------------------------------
//
// The wearer, before letting a blanket scale ship: "there are SO many
// components to the HUD ... Do we fully understand how these all work and
// interrelate? I don't have time to spend a week tuning all this."
//
// We did not, and the blanket scale would have been wrong for about half their
// list. Their nine items are TWO classes and the class decides the fix:
//
//   SCREEN-ANCHORED  ammo block, Titan health, slide/jump bars, tutorial
//                    prompts. Fixed places on screen. Scaling them about the
//                    screen centre is right and is what brings the top and
//                    bottom segments back into view.
//   WORLD-ANCHORED   enemy and friendly names, pickup indicators, the waypoint,
//                    missile lock. Their position is computed from a world
//                    point, so scaling that position about the centre walks
//                    them off the thing they label -- the exact bug that cost
//                    this project a day on the waypoint.
//
// Hand-classifying 43 widgets is the week the wearer does not have. The class
// is MEASURABLE instead: a world-anchored register's POSITION moves with the
// head while its SIZE does not, which is how the waypoint's target icon was
// identified. So correlate every register's position against the head, per
// widget, and let the run sort them.
//
// THE POSITIVE CONTROL IS BUILT IN. The marker widget is already known from
// three runs -- R08 (64x64) tracks the world, R04 (239x32, the distance text)
// does not. The classifier is pointed at it too, and if it does not reproduce
// that split it is broken and every other row it prints is worthless.
//
// THE GATE IS THE HEAD. A correlation against a head that did not move is
// meaningless, so a widget is only classified once the head's yaw has swept a
// real range while that widget was drawing, and it says so plainly when it has
// not. This is why a busy Titan fight is the right scene: many widgets drawing
// at once, and a head that moves because the fight makes it move.
namespace {

// THE GAMEPLAY GATE FOR THE TEXT CENSUS.
//
// plugin.cpp's GameIsRenderingAWorld() lives in an anonymous namespace and is
// not linkable from here, so this is its first clause built from the two
// symbols that ARE exported. Deliberately the LOOSER half: WorldIsReady()
// additionally waits for real view angles when `autoarm.require_live_view` is
// on, and opening a read-only census slightly early costs nothing while
// opening it late would lose the first seconds of play. Verified against the
// 2026-09-07 flat run, where the stage-1 gate opened at 14.550 s -- after the
// 14.302-14.398 s window that swallowed the whole old budget.
bool RuiCensusWorldIsUp() {
    return WeaponSettingsHits() != 0 && g_cameraHookCallCount != 0;
}

// 96, not 48. The 2026-09-06 census named 47 distinct widgets with only types 3
// and 4 substituted; adding type 7 for the name labels pushes past 48, and
// FindWidget drops a widget SILENTLY once it is full -- this project has already
// been burned by a cap hit exactly ("held=12 (cap 12)"), and a silently dropped
// widget reads exactly like one that never drew. The refusal is counted and
// reported loudly below as well.
constexpr int kMaxClassWidgets = 96;
std::uint64_t g_classFull = 0;
// EVERY early return in RuiWidgetClassify now has one of these. The project
// rule is written down and I broke it: an uncounted early return is how a widget
// that draws a thousand times looks identical to one that never drew.
std::uint64_t g_classTooFewRegs = 0;
unsigned g_classTooFewLastRva = 0;
unsigned g_classTooFewLastRegs = 0;
std::uint64_t g_classNotArmed = 0;
std::uint64_t g_classNoScratch = 0;
std::uint64_t g_stringsCapped = 0;
// The marker correction, declared here because the classifier report prints its
// declines and that report comes first in the file. Every early return in
// RuiUnzoomWorldMarkers has one, because the first version of that function
// never ran once and said nothing about it.
volatile std::uint64_t g_unzoomDraws = 0;
volatile std::uint64_t g_unzoomSkipped = 0;
volatile std::uint64_t g_unzoomNoScratch = 0;
volatile std::uint64_t g_unzoomOff = 0;
volatile std::uint64_t g_unzoomCrosshair = 0;
volatile std::uint64_t g_unzoomNoZoom = 0;
volatile std::uint64_t g_unzoomNoAnchor = 0;
constexpr int kMaxClassRegs = 24;
// Below this the head has not swept enough for a correlation to mean anything.
constexpr float kHeadSweepDeg = 25.0f;
constexpr int kMinClassSamples = 60;
// The same threshold the marker's lane census used, kept deliberately: two
// instruments answering the same question should not use two bars.
constexpr double kTracksHead = 0.7;

struct RegStat {
    double n;
    double sx, sxx;      // the register's position lane
    double sh, shh, sxh; // the head lane, and the cross term
    float lo, hi;
    float sizeW, sizeH;
};

struct WidgetStat {
    unsigned rva;
    unsigned regs;
    float spaceW, spaceH;
    float headMin, headMax;
    std::uint64_t draws;
    bool reported;
    // Set the moment ANY register in this widget is judged world-anchored, and
    // never cleared. The size control reads it and switches that widget to size
    // only, which is what the verdict line has always instructed. Seeded for the
    // labels below so the first seconds are right too, before any sweep.
    bool hasWorld;
    // Which register index classification started at: 3 for a normal widget,
    // 0 for one small enough that registers 0..2 ARE its content.
    unsigned base;
    // Which transform path this widget actually took, so "is this drift ours"
    // is answered per widget instead of argued about.
    int path;
    RegStat x[kMaxClassRegs];   // hi.x against head YAW
    RegStat y[kMaxClassRegs];   // hi.y against head PITCH
};

WidgetStat g_class[kMaxClassWidgets]{};
int g_classCount = 0;
bool g_classArmed = false;
std::uint64_t g_classNextReport = 0;
volatile std::uint64_t g_classNoHead = 0;
volatile std::uint64_t g_classDraws = 0;

WidgetStat* FindWidget(unsigned rva) {
    for (int i = 0; i < g_classCount; ++i) {
        if (g_class[i].rva == rva) return &g_class[i];
    }
    if (g_classCount >= kMaxClassWidgets) { ++g_classFull; return nullptr; }
    WidgetStat* w = &g_class[g_classCount++];
    w->rva = rva;
    w->headMin = 1.0e9f;
    w->headMax = -1.0e9f;
    return w;
}

void Accumulate(RegStat& s, float value, float head) {
    if (s.n == 0.0) { s.lo = value; s.hi = value; }
    if (value < s.lo) s.lo = value;
    if (value > s.hi) s.hi = value;
    s.n += 1.0;
    s.sx += value; s.sxx += double(value) * value;
    s.sh += head;  s.shh += double(head) * head;
    s.sxh += double(value) * head;
}

double Correlation(const RegStat& s) {
    if (s.n < 2.0) return 0.0;
    const double num = s.n * s.sxh - s.sx * s.sh;
    const double d1 = s.n * s.sxx - s.sx * s.sx;
    const double d2 = s.n * s.shh - s.sh * s.sh;
    if (d1 <= 1e-9 || d2 <= 1e-9) return 0.0;
    return num / std::sqrt(d1 * d2);
}

}  // namespace

// ---- WHICH TRANSFORM DID THIS WIDGET ACTUALLY GET ------------------------
//
// Every drift argument on this front has turned on one question -- is this
// element being moved by us, or is it the game's own projection -- and it has
// been settled three times by reasoning and got the wrong answer three times.
// So each widget records the path it took and the CLASS header prints it. No
// deduction, and no run spent on the question again.
enum WidgetPath {
    kPathUntouched = 0,   // reached the size control and was not transformed
    kPathScreenScale = 1, // moved AND resized about the screen centre -- ours
    kPathSizeOnly = 2,    // resized about itself, never moved -- cannot drift
    kPathDenylist = 3,    // world-anchored denylist: skipped entirely
    kPathModelSpace = 4,  // cockpit group, held back or on its own scale
    kPathIdentifyOnly = 5 // type 0: substituted to be read, never transformed
};
void NoteWidgetPath(unsigned rva, int path) {
    for (int i = 0; i < g_classCount; ++i) {
        if (g_class[i].rva == rva) { g_class[i].path = path; return; }
    }
}
const char* WidgetPathName(int path) {
    switch (path) {
        case kPathScreenScale:  return "MOVED+RESIZED about the screen centre (ours: this CAN drift)";
        case kPathSizeOnly:     return "RESIZED ONLY, never moved (ours, but cannot drift)";
        case kPathDenylist:     return "SKIPPED, world-anchored denylist (we do not touch it)";
        case kPathModelSpace:   return "cockpit group (its own scale, or held back)";
        case kPathIdentifyOnly: return "type 0, substituted to be READ only (we do not touch it)";
        default:                return "untouched";
    }
}

void RuiWidgetClassify(unsigned rva, std::uint8_t* scratch) {
    if (!g_classArmed) { ++g_classNotArmed; return; }
    if (!scratch) { ++g_classNoScratch; return; }
    __try {
        ++g_classDraws;
        const unsigned regs = *reinterpret_cast<const std::uint32_t*>(scratch + 8);
        if (regs == 0 || regs > 256) {
            // COUNTED, and this is the return that lost the missile lock. 0x5B800
            // drew 1201 times in one run and produced no verdict at all because
            // it fell out here in silence, which reads in the log exactly like a
            // widget that never drew. The RVA is kept so the log names it.
            ++g_classTooFewRegs;
            g_classTooFewLastRva = rva;
            g_classTooFewLastRegs = regs;
            return;
        }
        // A SMALL WIDGET IS STILL MEASURED. Registers 0..2 are the engine's own
        // mask, inset and aspect fit for a big widget, so classification starts
        // at 3 -- but a widget with only one or two registers has its content
        // THERE, and skipping it is how the lock produced nothing for weeks.
        // Learning WHY a widget declined is not the same as measuring it, and
        // this run has to measure it. The header says which base was used.
        const unsigned base = (regs > kFirstAuthoredReg) ? kFirstAuthoredReg : 0u;
        float yawDelta = 0.0f, pitch = 0.0f;
        if (!GetHeadViewDelta(&yawDelta, &pitch)) {
            ++g_classNoHead;   // counted, not silently skipped
            return;
        }
        // THE CORRELATE IS THE ABSOLUTE VIEW YAW, NOT THE HEAD DELTA. Run 1 of
        // the classifier used the delta and this project's own history says why
        // that is wrong: "a right-stick BODY turn holds the head delta CONSTANT,
        // yet the marker still drifts with the turn." A world-anchored element
        // moves with where the VIEW points, however the view got there, so a
        // Titan fight driven mostly by stick turns would show a world-anchored
        // element as uncorrelated and it would be filed as safe to reposition --
        // the one mistake this classifier exists to prevent.
        const float viewYaw = g_headBaseAngles[1] + yawDelta;
        WidgetStat* w = FindWidget(rva);
        if (!w) return;
        // Every fourth draw: the correlation needs spread, not volume, and this
        // runs for up to 48 widgets at a few hundred draws a second each.
        if ((++w->draws & 3u) != 0u) return;
        const auto* space = reinterpret_cast<const float*>(scratch + 0x2DD0);
        w->spaceW = space[0];
        w->spaceH = space[2];
        w->regs = regs;
        w->base = base;
        if (viewYaw < w->headMin) w->headMin = viewYaw;
        if (viewYaw > w->headMax) w->headMax = viewYaw;
        const auto* regBase = reinterpret_cast<const float*>(scratch + 0x3A80);
        const unsigned count = regs > kMaxClassRegs ? kMaxClassRegs : regs;
        for (unsigned i = base; i < count; ++i) {
            const float* r = regBase + 8 * i;
            if (!(r[4] == r[4]) || !(r[5] == r[5])) continue;   // NaN
            Accumulate(w->x[i], r[4], viewYaw);
            Accumulate(w->y[i], r[5], pitch);
            w->x[i].sizeW = r[0] * w->spaceW;
            w->x[i].sizeH = r[3] * w->spaceH;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_faults;
    }
}

// Defined below, beside the state it reports. It rides on this report so the
// text census cannot be silently absent from a log that otherwise looks whole.
static void ReportWidgetTextCensus();

void ReportWidgetClassification(bool force) {
    if (!g_classArmed) return;
    const std::uint64_t now = GetTickCount64();
    if (!force && now < g_classNextReport) return;
    g_classNextReport = now + 15000;
    char line[700]{};
    if (g_classFull) {
        char full[300]{};
        std::snprintf(full, sizeof(full),
            "[TF2VR] CLASSIFIER TABLE FULL: %llu draws were dropped because more than %d distinct "
            "widgets drew. Everything below is a SUBSET and must not be read as complete.\n",
            static_cast<unsigned long long>(g_classFull), kMaxClassWidgets);
        Tf2VrLog(full);
    }
    // ---- WHAT THIS REPORT DID NOT SEE, PRINTED EVERY TIME --------------
    //
    // A verdict list is only trustworthy beside the count of what never
    // reached it. The missile lock drew 1201 times in the 2026-09-06 run and
    // appeared nowhere, because it fell out of an uncounted early return; that
    // is indistinguishable in a log from a widget that never drew, and it cost
    // a run to notice. Every decline is counted now and printed here whether
    // or not it is zero, so absence of a widget is always explained.
    {
        {
            char uz[400]{};
            std::snprintf(uz, sizeof(uz),
                "[TF2VR] UNZOOM DECLINES: applied %llu, off %llu, crosshair %llu, zoom-is-1 %llu, "
                "no-anchor %llu, bad-regs %llu, no-scratch %llu. Applied 0 with the setting on means "
                "the correction never reached a marker.\n",
                static_cast<unsigned long long>(g_unzoomDraws),
                static_cast<unsigned long long>(g_unzoomOff),
                static_cast<unsigned long long>(g_unzoomCrosshair),
                static_cast<unsigned long long>(g_unzoomNoZoom),
                static_cast<unsigned long long>(g_unzoomNoAnchor),
                static_cast<unsigned long long>(g_unzoomSkipped),
                static_cast<unsigned long long>(g_unzoomNoScratch));
            Tf2VrLog(uz);
        }
        char skips[420]{};
        std::snprintf(skips, sizeof(skips),
            "[TF2VR] CLASSIFIER DECLINES: too-few-registers %llu (last ui(11)+0x%X with %u), "
            "no-head-pose %llu, not-armed %llu, no-scratch %llu, table-full %llu. A widget missing "
            "from the list below is explained by one of these or it did not draw.\n",
            static_cast<unsigned long long>(g_classTooFewRegs), g_classTooFewLastRva,
            g_classTooFewLastRegs,
            static_cast<unsigned long long>(g_classNoHead),
            static_cast<unsigned long long>(g_classNotArmed),
            static_cast<unsigned long long>(g_classNoScratch),
            static_cast<unsigned long long>(g_classFull));
        Tf2VrLog(skips);
    }
    ReportWidgetTextCensus();
    ReportRuiLayerProbe();
    int reportedNow = 0;
    for (int i = 0; i < g_classCount; ++i) {
        WidgetStat& w = g_class[i];
        if (w.reported && !force) continue;
        const float sweep = w.headMax - w.headMin;
        // ---- THE TRANSIENT TIER, BUILT FOR THE MISSILE LOCK ---------------
        //
        // The lock indicator (0x5B800) has been substituted and drawing for
        // weeks and has NEVER produced a single classifier row, because the
        // full bar is 60 samples across a 25 degree sweep and a lock is on
        // screen for a couple of seconds. A widget that cannot clear the bar is
        // indistinguishable in the log from one that never drew, which is how
        // this front has already lost runs.
        //
        // So there is a second tier: half the sweep and a third of the samples,
        // reported and LABELLED as the weaker evidence it is. It is enough to
        // separate "tracks the view" from "does not", which is the only
        // question being asked of the lock, and it cannot be mistaken for the
        // full verdict because the header says which tier produced it.
        constexpr float kTransientSweepDeg = 12.0f;
        constexpr int kTransientSamples = 20;
        const float samples = w.x[w.base].n;
        const bool enough = sweep >= kHeadSweepDeg && samples >= kMinClassSamples;
        const bool transient = !enough && sweep >= kTransientSweepDeg && samples >= kTransientSamples;
        if (!enough && !transient && !force) continue;
        w.reported = true;
        ++reportedNow;
        std::snprintf(line, sizeof(line),
            "[TF2VR] CLASS ui(11)+0x%X: space %.0fx%.0f, %u registers from index %u | TRANSFORM: %s "
            "| head swept %.1f deg over %.0f samples.%s\n",
            w.rva, w.spaceW, w.spaceH, w.regs, w.base, WidgetPathName(w.path), sweep, samples,
            enough ? ""
            : transient ? "  TRANSIENT TIER: this widget is only on screen briefly, so it is judged "
                          "on a shorter sweep and fewer samples. Good enough to say whether it tracks "
                          "the view, not good enough to trust a number."
                        : "  NOT ENOUGH HEAD MOVEMENT WHILE IT DREW -- every row below is "
                          "unclassified, not screen-anchored.");
        Tf2VrLog(line);
        const unsigned count = w.regs > kMaxClassRegs ? kMaxClassRegs : w.regs;
        for (unsigned r = w.base; r < count; ++r) {
            if (w.x[r].n < 2.0) continue;
            const double rx = Correlation(w.x[r]);
            const double ry = Correlation(w.y[r]);
            const bool world = enough && (rx > kTracksHead || rx < -kTracksHead ||
                                          ry > kTracksHead || ry < -kTracksHead);
            // THREE VERDICTS, NOT TWO. Run 1 called 101 registers screen-anchored
            // and none world-anchored, and every one of those had a position range
            // of exactly zero -- they never moved at all. "Never moved" and "moved
            // but not with the view" are different facts and only the first is
            // safe to reposition; collapsing them into one verdict is how a
            // world-anchored element gets filed as safe on a quiet run.
            const float spanX = w.x[r].hi - w.x[r].lo;
            const float spanY = w.y[r].hi - w.y[r].lo;
            const bool moved = spanX > 0.002f || spanY > 0.002f;
            // The transient tier counts for the verdict AND for hasWorld. A lock
            // that tracks the view must take the size-only path on the weaker
            // evidence too: the cost of believing it wrongly is a widget that
            // is resized and not moved, and the cost of NOT believing it is a
            // marker walked off its target, which is the defect being chased.
            const bool judged = enough || transient;
            if (world && judged) w.hasWorld = true;
            const char* verdict =
                !judged ? "UNCLASSIFIED -- the view did not sweep while this drew"
                        : (world ? (enough ? "WORLD-ANCHORED -- size only, never reposition"
                                           : "WORLD-ANCHORED (transient tier) -- size only, never reposition")
                                 : (moved ? "MOVES, but not with the view -- animated or data-driven, "
                                            "NOT yet safe to reposition"
                                          : "static -- safe to scale about centre"));
            std::snprintf(line, sizeof(line),
                "[TF2VR]     R%02u size %.0fx%.0f px | x %.3f..%.3f (span %.3f) r(yaw) %+.2f | y "
                "%.3f..%.3f (span %.3f) r(pitch) %+.2f | %s\n",
                r, w.x[r].sizeW, w.x[r].sizeH, w.x[r].lo, w.x[r].hi, spanX, rx, w.y[r].lo, w.y[r].hi,
                spanY, ry, verdict);
            Tf2VrLog(line);
        }
    }
    if (reportedNow == 0 && force) {
        std::snprintf(line, sizeof(line),
            "[TF2VR] CLASS: NOTHING CLASSIFIED. %d widgets seen, %llu draws, %llu draws with no head "
            "pose. If widgets is 0 the substitution never took (hud.widget_arm) and this says nothing "
            "about the HUD.\n",
            g_classCount, static_cast<unsigned long long>(g_classDraws),
            static_cast<unsigned long long>(g_classNoHead));
        Tf2VrLog(line);
    }
}

void SetRuiWidgetCensus(bool armed) {
    g_classArmed = armed;
    Tf2VrLog(armed
        ? "[TF2VR] hud.widget_census = 1: every substituted widget's registers are correlated against "
          "head yaw and pitch, and each is called WORLD-ANCHORED (position tracks the head, so it may "
          "only be resized about its own anchor) or screen-anchored (safe to scale about the centre). "
          "Read-only. The marker widget is the POSITIVE CONTROL: it must come back with R08 (64x64) "
          "world-anchored and R04 (239x32) not. Play a busy fight and move your head -- a widget that "
          "draws while the head is still cannot be classified and will say so.\n"
        : "[TF2VR] hud.widget_census = 0.\n");
}

// ---- THE HUD SIZE CONTROL --------------------------------------------------
//
// Wearer, 2026-09-06: "the HUD is overall quite large with the top and bottom
// segments pushed quite up and down ... I cannot even see the LB RB Y
// indicators I saw before we started the HUD overhaul."
//
// That is the known cost of the frustum fix, now reaching the wearer. Restoring
// the frustum the game uploads for the HUD pass magnifies everything drawn in
// it by 1.8952x -- currently 2.1992x, since the refit tracks the declared FOV --
// and only the waypoint ever got a size knob to pay for it.
//
// IT CANNOT BE FIXED AT THE PASS. The pass's frustum is exactly what makes the
// waypoint track its world point; scaling it is the thing F6 used to do and the
// thing that was unbound for leaving the wearer in the broken state. So the
// size is corrected PER WIDGET, in the register file, as a true similarity
// transform about the screen centre:
//
//     lo *= s                       both basis vectors, so no aspect distortion
//     hi  = 0.5 + (hi - 0.5) * s    position scaled about the centre
//
// Every element of a widget moves together and keeps its shape, which is why
// this cannot reproduce the shrink-toward-the-corner that scaling the
// coordinate space caused: that changed the denominator positions were measured
// against, this moves the positions themselves.
//
// 1.0 is inert and is the default, so an armed build with no key pressed is
// identical to an unarmed one.
namespace {
volatile float g_hudWidgetScale = 1.0f;
constexpr float kHudScaleLadder[5] = {1.0f, 0.85f, 0.75f, 0.65f, 0.55f};
int g_hudScaleRung = 0;
volatile std::uint64_t g_hudScaleWidgets = 0;
volatile std::uint64_t g_hudScaleDraws = 0;
volatile std::uint64_t g_hudScaleSkippedRegs = 0;
volatile std::uint64_t g_hudScaleSkippedSpace = 0;
// `hud.widget_screenspace_only`. 1 = scale only widgets whose space IS the
// 1920x1080 screen. Model-space widgets are painted on cockpit geometry, where
// 0.5 is the middle of a surface rather than the middle of the screen.
volatile bool g_screenSpaceOnly = true;
// The box witness: one line per widget per scale setting, so the widget that
// moves DOWN names itself instead of being deduced at the wearer's expense.
constexpr int kBoxCap = 200;
unsigned g_boxRva[kBoxCap]{};
float g_boxScale[kBoxCap]{};
int g_boxCount = 0;
int g_boxReported = 0;
// `hud.widget_skip`: one widget RVA (DECIMAL, the parser is atof) left unscaled.
// For the one element the scale is wrong for, once the box witness names it.
volatile unsigned g_skipRva = 0;
// `hud.cockpit_scale` and `hud.cockpit_skip`. The Titan cockpit group gets its
// own number because 0.55 took the surround and the top health bar in further
// than the wearer wanted, and one widget in the group -- the two-bar indicator --
// must not move at all.
volatile float g_cockpitScale = 0.0f;
volatile unsigned g_cockpitSkipRva = 0;
volatile std::uint64_t g_cockpitSkipped = 0;
// `hud.bars_scale` and `hud.bars_shift_y`. The two-bar indicator gets its own
// size and height, because the wearer wants it BIGGER and slid UP rather than
// merely left alone. Both inert by default, so an unset ini leaves it untouched.
volatile float g_barsScale = 1.0f;
volatile float g_barsShiftY = 0.0f;
// `hud.nameplate_scale`. The friend and enemy labels: type-7 widgets, world-
// anchored, scaled about their OWN centre so they stay on the entity.
volatile float g_nameplateScale = 1.0f;
volatile std::uint64_t g_nameplateDraws = 0;
volatile std::uint64_t g_nameplateSkippedRegs = 0;
// 0 = size only, which cannot move a label off its entity. 1 = the group scaled
// about the MEDIAN of its positions, robust to the static registers that dragged
// the bounding-box centre around and caused the drift the wearer reported.
volatile int g_nameplateMode = 0;
// DEFAULT OFF, 2026-09-07. Switched on for one run and it made things worse:
// the square grew, the reticle became huge once missiles were active, and the
// markers stayed stuck inside the square exactly as before. See the note in
// RuiUnzoomWorldMarkers for what that proves.
volatile bool g_markerUnzoom = false;
// SAMPLES ARE SPENT ON TEXT, DURING PLAY, AND NEVER ON THE LOADING SCREEN.
//
// The first version dumped once at a widget's first draw, which is why the
// friendly label never printed the name the wearer could read on screen: at its
// first draw it had no name in it yet. A label's text arrives later than the
// widget does, so sampling only the first draw asks the question at the one
// moment it cannot be answered.
//
// The second version took four samples per widget per session, keyed on the
// arena CONTENT changing. Measured against the 2026-09-07 run (config content
// D5FA26FD): 92 of its 124 samples landed between 14.302 s and 14.398 s -- a
// 96 ms window on the spawn screen -- and BOTH type-7 label candidates,
// ui(11)+0x84070 and +0x8A330, spent all four there. GameIsRenderingAWorld()
// did not open until 14.550 s. Every one of those samples was float bytes read
// as ASCII ("[1U_?]", "[#F0?]"), and all four were byte-identical between the
// two supposedly different widgets. So "160 samples and none printed a rank and
// surname" was never a test of anything: the budget was gone before a friendly
// name existed on screen, and the samples that consumed it contained no text.
//
// Two changes, and the first is the one that matters:
//
//  1. ONLY A SAMPLE THAT CONTAINS TEXT COUNTS AGAINST THE BUDGET. A changing
//     block of float garbage can no longer spend it, whatever the game is
//     showing. This holds even if the gameplay gate below is ever wrong.
//  2. Sampling is gated on the world being up, so the loading screen is not
//     sampled at all. Its declines are counted and printed, so a silent census
//     is distinguishable from a census that was never allowed to look.
//
// The scratch POINTER is recorded with every sample, because the identical
// output from two different RVAs above is either two widgets sharing a scratch
// or a reader that resolved the wrong one, and nothing in the old line said
// which.
struct StringSample {
    unsigned rva;
    int times;                       // samples PRINTED (text-bearing only)
    unsigned hash;                   // hash of the last printed text
    unsigned long long draws;        // draws that reached the reader at all
    unsigned long long preWorld;     // declined: the world was not up yet
    unsigned long long noText;       // reached it, arena held nothing readable
    const void* lastScratch;         // the block this widget was last read from
    int bestStreak;                  // longest letter run its arena ever held
    unsigned long long windowStart;  // when this widget's current budget opened
    int printed;                     // session total, across all windows
};
StringSample g_stringsSeen[64]{};
int g_stringsDumpedCount = 0;
// A BUDGET THAT REOPENS, because "only text spends it" was not enough.
//
// 2026-09-07 second run: ui(11)+0x8A330 spent all twelve of its samples between
// 14.588 s and 14.865 s -- 277 ms, at draws 1 to 59 of the 7835 it went on to
// make. Nothing was wrong with the filter this time: the widget really was
// drawing text, and the text was the respawn screen ("%[X_BUTTON|SPACE]%
// RESPAWN", "SPAWN AS TITAN", Cooper's logbook). A session budget spent on real
// text at second fourteen is just as gone as one spent on noise, and the world
// gate had already opened, so the gate could not save it either.
//
// So the cap is per WINDOW. Every widget gets kTextSampleCap fresh samples each
// window, all session, bounded overall by kTextSessionCap. A widget saying the
// same thing forever costs one line per window; a widget that starts saying a
// player's name at second sixty gets read at second sixty.
constexpr int kTextSampleCap = 8;
constexpr int kTextSessionCap = 64;
constexpr unsigned long long kTextWindowMs = 20000;
// `hud.widget_named_only`. 1 = only the three POSITIVELY NAMED widgets are moved
// (loadout block x2, earn meter); every other widget stays exactly where the game
// puts it. This is the wearer.s own criterion for the two bars: do not touch them.
// F6 toggles it so both arms can be seen in one run.
volatile bool g_namedOnly = false;
// hud.ll_shift_x -- MOVE THE LOWER-LEFT GROUP ACROSS WITHOUT RESIZING IT.
//
// hud.widget_scale positions by scaling about the screen centre, so size and
// position are one knob: the only way to bring the loadout block further in
// from the left edge is to make it smaller, and the wearer's requirement is
// explicit -- shifted right, no change in size. A translation applied AFTER
// the scale, to the position lanes only, is that control. Normalised screen
// width, so +0.02 is two per cent of the screen to the right whatever the
// buffer is. Named-movable widgets only: this is the loadout block and the
// earn meter, never the whole HUD.
volatile float g_llShiftX = 0.0f;
// AND THE TITAN'S OWN VALUE. The wearer: "there is a larger bar with more
// content in the Titan", so the two are different objects wanting different
// amounts. Same guard the per-weapon grip uses -- IsInTitanNow fails safe to
// on-foot, and an unsettled titan state is treated as on foot -- so a save
// that LOADS inside a titan cannot spend its first frames using the pilot
// value on the titan bar.
volatile float g_llShiftXTitan = 0.0f;

// ---- PER-WIDGET OVERRIDES, for the two the wearer named and the tag found --
//
// hud.widget_scale is ONE number over a heterogeneous population, which is why
// the wearer's three launch defects pull in different directions: the value
// that suits the loadout block leaves the gauntlet's instruction panel huge.
// These are multipliers ON TOP of the global scale, plus a translation, for
// named widgets only. Identified 2026-09-11 by the wearer's own RIGHT-ARROW
// tag, by diffing what drew in the gauntlet against what drew at the range:
//
//   0x80F80  the gauntlet instruction panel. Authored 0.855 x 0.749 of the
//            screen -- at the global 0.55 it is still about 47% x 41%, which
//            is "VERY LARGE and in your face" verbatim.
//   0x4E030  the gauntlet timer, authored at x 0.766..0.940, upper right. The
//            global scale pulls it to 0.646..0.742 and the wearer still reads
//            it as way too far to the side, so it wants a translation of its
//            own rather than more shrinking.
struct WidgetOverride {
    unsigned rva;
    float scale;     // multiplies the global scale for this widget alone
    float shiftX;    // added after, position lanes only, so size is unchanged
    float shiftY;
};

WidgetOverride g_overrides[] = {
    // Dialled by the wearer in the gauntlet 2026-09-11. The scale figures are
    // settled; the shifts are what they are dialling next, because SHRINKING
    // MOVES THINGS -- the transform scales about the screen centre, so the
    // panel rose as it got smaller and needed bringing back down. That
    // coupling is the same one the lower-left group hit, and it is why every
    // one of these wants both a size and a position.
    // THE INSTRUCTION PANEL IS SETTLED. x0.58 and +0.130 down, both dialled by
    // the wearer in the gauntlet: "Instructions now look good."
    {0x80F80u, 0.58f, 0.0167f, 0.1590f},
    // THE TIMER, and its shift is COMPUTED rather than dialled a third time.
    //
    // The wearer settled its position at -0.130 across while it was full size,
    // then settled its size at x0.45 -- and shrinking moved it, because this
    // transform scales about the SCREEN CENTRE. Its authored box is
    // x 0.766..0.940, y 0.338..0.505, so its centre is (0.853, 0.4215), and
    // the drift is exactly:
    //
    //   before  x = 0.5 + (0.853-0.5)*0.55        - 0.130 = 0.5642
    //   after   x = 0.5 + (0.853-0.5)*0.55*0.45   - 0.130 = 0.4574   (-0.107)
    //   before  y = 0.5 + (0.4215-0.5)*0.55               = 0.4568
    //   after   y = 0.5 + (0.4215-0.5)*0.55*0.45          = 0.4806   (+0.024)
    //
    // which is "too left and too far down" to the decimal. Adding the drift
    // back restores the centre to (0.5642, 0.4568) -- the position the wearer
    // had already approved -- at the new size. LEFT and DOWN remain as fine
    // trims in case the box centre and the eye disagree.
    // Trimmed by the wearer from the computed value: LEFT rung 3 (+0.030 on
    // the x trim) and DOWN rung 3 (-0.030 on the y trim). "I dialed it in
    // pretty well."
    {0x4E030u, 0.45f, -0.1000f, -0.0300f},
    // ui(11)+0x84070 WAS TRIED HERE AND IS ELIMINATED. It is type 7 and takes
    // the nameplate path at hud.nameplate_scale 0.50, so it looked like the
    // weapon highlight. Dialled to x2.00 -- which makes the effective scale
    // exactly 1.0, the size the game authored, a doubling nobody could miss --
    // and the wearer saw NO CHANGE. So it is some other type-7 readout, and
    // the highlight is found by hiding widgets rather than by resizing a
    // guess. The multiply in RuiNameplateScale stays: it is inert with no
    // entry here, and it is what will apply the fix once the hunt names the
    // widget.
    //
    // THE WEAPON HIGHLIGHT IS ui(11)+0x27600, named by the wearer on the
    // hiding ladder: 257 draws suppressed while armed and the highlight went
    // with them. It is TYPE 4, which hud.widget_types = 217 admits, so it has
    // been taking the global hud.widget_scale = 0.55 all along -- the value
    // settled on 2026-09-06, which is exactly when the wearer says it stopped
    // being readable.
    //
    // The earlier tag census had already isolated it as the only widget unique
    // to the three range tags, and it was discarded for having a full-screen
    // box. That box is the point: it is a container whose contents are placed
    // from a world position, so it spans the screen because the highlight can
    // appear anywhere in it.
    //
    // And that makes this one free of the coupling everything else fought. Its
    // box is x 0.000..1.000, y 0.050..0.950, so its centre is exactly
    // (0.5, 0.5) -- the screen centre this transform scales about. Scaling it
    // moves it nowhere. Size only, by construction, with no translation to
    // dial afterwards.
    // x2.10, dialled by the wearer at the range: 1.15 of the size the game
    // itself draws, chosen over the authored size after eleven seconds on that
    // rung. Larger than authored is right for a headset, where this text sits
    // out where the panel is least sharp.
    {0x27600u, 2.10f, 0.0f, 0.0f},
    // THE UP/DOWN DIALOGUE PROMPT -- ui(11)+0x151D0, named by the TAG on
    // 2026-09-11 and not by deduction.
    //
    // Three tags in one run: one control with nothing on screen (16 widgets)
    // and two with the prompt up (17 each). Exactly ONE widget is in both
    // prompt tags and absent from the control, and nothing in the control is
    // missing from the prompts. Its geometry agrees: box x 0.351..0.649,
    // y 0.906..0.976 -- centred on x 0.500 to three places and the LOWEST
    // real box on screen, which is where a response prompt lives. Type 0x10,
    // ADMITTED, so it has been taking the global hud.widget_scale = 0.55 all
    // along, which is the same cause as the weapon highlight above.
    //
    // 1.82 = 1/0.55, SO THE PRODUCT IS 1.00 AND THIS IS THE SIZE AND THE
    // PLACE THE GAME AUTHORS. That is not timidity, it is geometry: unlike
    // the highlight, this widget's centre is NOT the screen centre. It sits
    // at y 0.941, and the transform scales about 0.5, so the drawn centre is
    // 0.5 + 0.441 x s. At s = 1.10 that is 0.985 with a half-height of 0.039,
    // i.e. the bottom edge is at 1.024 and the prompt is off the screen. Any
    // setting above authored size needs hud.prompt_shift_y to pull it back up,
    // which is why that slider exists and starts at 0.
    {0x151D0u, 1.82f, 0.0f, 0.0f},
    // THE TITAN "CORE +N%" MESSAGE -- A CANDIDATE, AND SAY SO.
    //
    // The text hunt found the message: the arena formats it "2.CORE +2`3%",
    // "9.CORE +9`3%", "12.CORE +12`3%", which is the wearer's "+6%" exactly.
    // But TWENTY widgets reported seeing it, including 0x5090, which the
    // nomenclature says draws NO AMMO / LOW AMMO / RELOAD. The arena is SHARED:
    // a widget reports whatever was last formatted into it, not what it drew.
    // So the hunt proved the message exists and what it says, and isolated
    // nothing. That limit is the method's, not the data's.
    //
    // Geometry is the only discriminator left. Of the five that saw "CORE +N",
    // this is the one with a real text-line box -- zero width, 0.026 tall,
    // anchored at (0.500, 0.567..0.593), dead centre -- which is what a short
    // floating message looks like. The others are 0x0000 boxes or a
    // pixel-space full-screen container.
    //
    // 0.5 is chosen to be unmistakable rather than final: if the message halves
    // the candidate is right and the number can be tuned; if something else
    // halves, the wearer names it and this entry goes.
    // 0.55: the wearer confirmed the candidate by halving it -- "you nailed it"
    // -- and asked for about ten per cent back. Identified by geometry after
    // the text hunt could not isolate it, and confirmed by a visible change,
    // which is the only thing that has identified a widget all session.
    {0x15DE0u, 0.55f, 0.0f, 0.0f},
};

WidgetOverride* FindOverride(unsigned rva) {
    for (auto& o : g_overrides) {
        if (o.rva == rva) return &o;
    }
    return nullptr;
}

bool LowerLeftInTitan() { return TitanStateSettled() && IsInTitanNow(); }

float ActiveLowerLeftShiftX() {
    return LowerLeftInTitan() ? g_llShiftXTitan : g_llShiftX;
}
volatile std::uint64_t g_namedOnlySkips = 0;
unsigned g_hudScaleLastRva = 0;
// THE DUPLICATION CHECK, kept as a standing counter. The 2026-09-06 run
// dumped 80 registers across 7 widgets and NONE had hi lanes 6,7 differing
// from 4,5, so `hi = [x, y, x, y]` holds beyond the marker widget. This stays
// so a widget that breaks it announces itself instead of being assumed.
volatile std::uint64_t g_hiLanesDiffer = 0;
// THE WIDGETS THIS SESSION HAS ACTUALLY SCALED, in first-scaled order. F6 steps
// `hud.widget_skip` through them so the wearer can restore one widget at a time
// to the game.s own layout and SEE which one holds the jump/slide bars -- the
// press that puts the bars right is simultaneously the diagnosis and the fix.
constexpr int kScaledCap = 32;
unsigned g_scaledRva[kScaledCap]{};
volatile int g_scaledCount = 0;
// The register dump prints once per widget for the whole session, whatever
// the mode; the BOX line prints once per widget per (scale, mode).
unsigned g_regDumped[kBoxCap]{};
int g_regDumpedCount = 0;
}  // namespace

// THE VERDICT LINE, AND IT IS THE POINT OF THE WHOLE CENSUS.
//
// A list of widgets that said nothing is worthless without a widget that said
// something in the same run: that is the positive control, and it is structural
// rather than a hardcoded RVA. If NOT ONE widget produced readable text while
// the world was up, the reader is not reading text, and no absence anywhere in
// this log excludes anything -- which is exactly the mistake the 2026-09-07
// "160 samples, no rank and surname" claim made.
static void ReportWidgetTextCensus() {
    int withText = 0, silent = 0;
    unsigned long long preWorld = 0, noText = 0, draws = 0;
    for (int i = 0; i < g_stringsDumpedCount; ++i) {
        const StringSample& s = g_stringsSeen[i];
        draws += s.draws;
        preWorld += s.preWorld;
        noText += s.noText;
        if (s.printed > 0) ++withText; else ++silent;
    }
    char head[520]{};
    std::snprintf(head, sizeof(head),
        "[TF2VR] RUI TEXT CENSUS: gate %s | widgets %d (with text %d, silent %d, table-full %llu) | "
        "draws seen %llu, declined pre-world %llu, reached-but-no-text %llu, faults %llu.\n",
        RuiCensusWorldIsUp() ? "OPEN (world up)" : "CLOSED (no world yet)",
        g_stringsDumpedCount, withText, silent,
        static_cast<unsigned long long>(g_stringsCapped),
        static_cast<unsigned long long>(draws),
        static_cast<unsigned long long>(preWorld),
        static_cast<unsigned long long>(noText),
        static_cast<unsigned long long>(g_faults));
    Tf2VrLog(head);
    if (withText == 0) {
        Tf2VrLog("[TF2VR]   VERDICT: BROKEN OR UNTESTED -- not one widget has produced readable "
                 "text while the world was up. The arena reader is not reading text (or nothing "
                 "has drawn yet), so NO absence in this log is an exclusion of any widget.\n");
        return;
    }
    // Name the silent ones explicitly. "Not in the list" is the failure mode
    // this project keeps paying for; a widget that drew and never said anything
    // has to appear BY NAME beside the ones that did.
    char line[520]{};
    int used = std::snprintf(line, sizeof(line),
        "[TF2VR]   VERDICT: READING -- %d widget(s) said something readable during play, so a "
        "widget below drew without text rather than going unread. Silent:", withText);
    int listed = 0;
    for (int i = 0; i < g_stringsDumpedCount && used < 430; ++i) {
        const StringSample& s = g_stringsSeen[i];
        if (s.printed > 0) continue;
        // The best letter run comes with it: 1 means the arena genuinely holds
        // no words, 3 means the filter is one notch from printing it and should
        // be loosened rather than believed.
        used += std::snprintf(line + used, sizeof(line) - used, " 0x%X(%llu draws, best run %d)",
                              s.rva, static_cast<unsigned long long>(s.draws), s.bestStreak);
        ++listed;
    }
    if (!listed) used += std::snprintf(line + used, sizeof(line) - used, " none");
    else if (listed < silent) std::snprintf(line + used, sizeof(line) - used, " +%d more", silent - listed);
    std::strncat(line, "\n", sizeof(line) - std::strlen(line) - 1);
    Tf2VrLog(line);
}

// THE DENYLIST, and it is not a guess. STATE-HUD's world-anchored class was
// named by the 2026-09-05 census from the assets each widget draws, before any
// of this work: the waypoint marker, the distance readouts, the overhead titan
// arrow, the battery capture marker and the Titan lock-on. Those compute their
// position from a world point, so scaling that position about the screen centre
// walks them off the thing they label. Everything else substituted is chrome.
//
// This is a DENYLIST of the known-dangerous rather than an allowlist of the
// known-safe, which is the weaker of the two and is stated plainly: a
// world-anchored widget nobody has named yet would be scaled. The classifier
// stays armed to catch exactly that, and the wearer's eyes catch it faster --
// a mislabelled element walks off its target visibly.
constexpr unsigned kWorldAnchored[] = {
    0x809B0,   // the waypoint marker (its own hud.marker_size)
    0xBF60,    // #HUD_DISTANCE_METERS / KILOMETERS with flyout borders
    0x69570,   // overhead_icon_titan_arrow_you
    0xC4D0,    // battery capture, friendly and enemy
    0x5B800,   // the Titan missile lock-on, centre and edge
};

bool IsWorldAnchoredWidget(unsigned rva) {
    for (unsigned d : kWorldAnchored) {
        if (d == rva) return true;
    }
    return false;
}

// ---- THE ALLOWLIST, and it is the wearer's own acceptance criterion --------
//
// "The original location of the two bars was fine. If we could just not touch
// them when we move everything else, that would also work."
//
// The denylist above is the weaker of the two lists and its own comment says
// so: a widget nobody has named yet gets scaled by default. That default is
// what is moving the two bars. This inverts it -- only widgets POSITIVELY
// NAMED from the assets they draw are moved, and everything else stays exactly
// where the game puts it.
//
// The three below are the ones the wearer asked to have pulled inward and has
// confirmed by eye, named in docs/HUD-NOMENCLATURE-2026-09-05.md from their
// RUI assets:
//
//   0x1010, 0x4320  the loadout block  (hud\weapon_status.rui) -- weapon, ammo
//                   count, tactical, ordnance, cooldown bars, lower left
//   0x2B190         the earn meter     (rui/hud/earn_meter/*) -- titan-build
//                   meter and #CORE_ACTIVE, lower left
//
// The 2026-09-06 register dump found four more widgets being scaled that the
// nomenclature table does NOT name, all of them bar-shaped and none of them
// something the wearer asked to move: 0x93BE0 (twelve stepping cells plus two
// registers with a 16-unit y basis), 0x76420 (two registers symmetric about
// x 0.5), and the matched pair 0x5F6B0 / 0x15DE0 (one zero-width register each
// at dead centre, so their width arrives from a data binding -- what a meter
// that fills looks like). Those four are exactly the population the two bars
// must be in, and this list drops all four.
constexpr unsigned kNamedMovable[] = {
    0x1010,    // loadout block, part 1
    0x4320,    // loadout block, part 2
    0x55D00,   // loadout block, part 3 -- MISSED on the first pass. The
               // nomenclature table lists three functions for this asset and I
               // copied two, so a third of the block the wearer had asked to
               // move stopped moving.
    0x2B190,   // earn meter
};

bool IsNamedMovableWidget(unsigned rva) {
    for (unsigned a : kNamedMovable) {
        if (a == rva) return true;
    }
    return false;
}

// ---- THE GENERAL RULE, LEARNED THE EXPENSIVE WAY --------------------------
//
// The classifier has been printing "WORLD-ANCHORED -- size only, never
// reposition" against individual registers since it was built, and nothing
// ever read it: the size control repositioned every widget it was allowed to
// touch regardless. That is why the enemy labels drifted once they were
// reachable, and it is why the FRIENDLY labels still drifted after the enemy
// ones were fixed -- ui(11)+0x920D0 is layer type 4, not 7, so it never took
// the label path at all and kept getting scaled about the screen centre.
//
// So the verdict is wired to the control. A widget with any world-anchored
// register is scaled in SIZE ONLY, wherever it lives and whatever its type.
// Two are seeded here so the rule holds from the first frame, before any head
// sweep has happened to classify them:
//
//   0x8A330  type 7, 32 registers, 19 of them world-anchored (measured)
//   0x920D0  type 4, 35 registers, the friendly counterpart
//   0x84070  type 7, per-entity (6-14 draws a frame), the FRIENDLY label -- seeded 2026-09-07
//   0x86220  type 7, per-entity sibling, no asset strings -- seeded 2026-09-07
// ---- WHAT DOES THIS WIDGET SAY -------------------------------------------
//
// The wearer named the friendly label by its text: "Lt Shaver". The scratch
// block carries a 0x200C-byte string arena at +0x05BC (HANDOFF section 2), so
// a widget can simply be asked what it says instead of being guessed at from
// register geometry, which is how the last three identifications went wrong.
//
// READ ONLY, once per widget per session, and it scales nothing. Printable
// ASCII runs of four characters or more, up to six of them, so a nameplate
// announces the name it is drawing and the search stops being a guess.
// ---- THE STRUCTURAL READ OF THE IDENTIFIED WIDGET (2026-09-07) ---------------
//
// It is pointed at ui(11)+0x8A330 now, which is the widget the ladder named by
// disappearance, and it fires ITSELF. Both of those are corrections.
//
// It used to target 0x84070 -- a suspect from a table, never identified -- and
// it used to need a keypress, which meant it sampled whenever the wearer
// happened to press rather than at a spread of head yaws.
//
// WHAT IT IS FOR. The per-layer regression built on top of this widget failed
// because nothing in the scratch identified WHICH layer a draw belonged to:
// every layer reports 32 registers and the string arena holds float noise, not
// entity names, so 1569 samples of different entities averaged into a body
// slope of -0.20 px/deg over 169 degrees of body rotation. Before any more
// algebra, read the block and find out what a layer is actually handed.
//
// An archived dump of the sibling 0x84070 (prev-7, 2026-09-07) shows args A04-A06
// and A07-A09 holding TWO 3x4 transforms with near-identical translations and
// different rotations. If each layer carries its own transform, the frame a
// label was placed in is readable directly and no per-entity key is needed at
// all. That is a reading of a DIFFERENT widget in an OLDER run, so it is a
// question for this dump to answer, not a premise to build on.
//
// Self-firing rules, so the dumps land where they are worth something: only
// once the world is up AND the head pose is published (a dump with no head is
// half a measurement), at most one every kLabelDumpGapMs so the sample spans
// several different head yaws, capped per session. Every decline is counted.
constexpr unsigned kLabelDumpCap = 8;
constexpr unsigned long long kLabelDumpGapMs = 2000;
int g_labelDumps = 0;
unsigned long long g_labelDumpLast = 0;
std::uint64_t g_labelDumpDeclinedCap = 0, g_labelDumpDeclinedScratch = 0, g_labelDumpFaults = 0;
std::uint64_t g_labelDumpDeclinedGate = 0, g_labelDumpDeclinedGap = 0, g_labelDumpDeclinedHead = 0;

void RuiLabelDump(unsigned rva, const std::uint8_t* scratch) {
    if (rva != kLabelWidgetRva) return;
    if (!scratch) { ++g_labelDumpDeclinedScratch; return; }
    if (!RuiCensusWorldIsUp()) { ++g_labelDumpDeclinedGate; return; }
    {
        float probeYaw = 0.0f, probePitch = 0.0f;
        if (!GetHeadViewDelta(&probeYaw, &probePitch)) { ++g_labelDumpDeclinedHead; return; }
    }
    if (g_labelDumps >= static_cast<int>(kLabelDumpCap)) {
        ++g_labelDumpDeclinedCap;
        return;
    }
    const unsigned long long nowMs = GetTickCount64();
    if (g_labelDumpLast && nowMs - g_labelDumpLast < kLabelDumpGapMs) {
        ++g_labelDumpDeclinedGap;
        return;
    }
    g_labelDumpLast = nowMs;
    ++g_labelDumps;
    __try {
        const unsigned regs = *reinterpret_cast<const std::uint32_t*>(scratch + 8);
        const auto* space = reinterpret_cast<const float*>(scratch + 0x2DD0);
        const float sx = space[0], sy = space[2];
        float yawDelta = 0.0f, headPitch = 0.0f;
        const bool haveHead = GetHeadViewDelta(&yawDelta, &headPitch);
        char line[400]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] ======== LABEL DUMP #%d ui(11)+0x%X | regs %u | space %.0fx%.0f | body <p%.2f y%.2f> head <p%.2f y%.2f> delta y%+.2f (head %s) ========\n",
            g_labelDumps, rva, regs, sx, sy, g_headBaseAngles[0], g_headBaseAngles[1], headPitch,
            g_headBaseAngles[1] + yawDelta, yawDelta, haveHead ? "published" : "NOT published");
        Tf2VrLog(line);
        const unsigned show = regs > 16 ? 16 : regs;
        for (unsigned i = 0; i < show; ++i) {
            const auto* r = reinterpret_cast<const float*>(scratch + 0x3A80 + 32 * i);
            std::snprintf(line, sizeof(line),
                "[TF2VR]   R%02u lo %.5g %.5g %.5g %.5g | hi %.5g %.5g | px basis %.1f,%.1f and %.1f,%.1f | px pos %.1f,%.1f\n",
                i, r[0], r[1], r[2], r[3], r[4], r[5], r[0] * sx, r[1] * sy, r[2] * sx, r[3] * sy, r[4] * sx, r[5] * sy);
            Tf2VrLog(line);
        }
        for (unsigned i = 0; i < 24; ++i) {
            const auto* a = reinterpret_cast<const float*>(scratch + 0x2DD0 + 16 * i);
            std::snprintf(line, sizeof(line), "[TF2VR]   A%02u (blk+0x%03X) %.6g %.6g %.6g %.6g\n", i, 16 * i, a[0], a[1], a[2], a[3]);
            Tf2VrLog(line);
        }
        // ---- ANY 3x4 TRANSFORM SITTING IN THE ARGS, FOUND STRUCTURALLY ------
        //
        // Not "A04-A06 is a matrix because the sibling's was": three
        // consecutive args whose xyz parts are all unit length is a basis, and
        // that test does not care which widget or which build. Each one's row
        // yaws are printed against the BODY yaw and against the HEAD-COMPOSED
        // yaw, which is the whole question in one number -- a layer placed in
        // the body frame sits near the body figure and a tracking one sits near
        // the head figure.
        const auto* args = reinterpret_cast<const float*>(scratch + 0x2DD0);
        const float bodyY = g_headBaseAngles[1];
        const float headY = g_headBaseAngles[1] + yawDelta;
        int at[8]{};
        const int triples = ScanArgTransforms(args, 24, at, 8);
        for (int t = 0; t < triples && t < 8; ++t) {
            const float* r0 = args + 4 * at[t];
            const float* r1 = args + 4 * (at[t] + 1);
            const float* r2 = args + 4 * (at[t] + 2);
            const float y0 = ArgRowYawDeg(r0);
            std::snprintf(line, sizeof(line),
                "[TF2VR]   TRANSFORM A%02d-A%02d | row yaws %.2f / %.2f / %.2f | row0 vs BODY "
                "%+.2f, vs HEAD %+.2f | translation (%.1f %.1f %.1f)\n",
                at[t], at[t] + 2, y0, ArgRowYawDeg(r1), ArgRowYawDeg(r2),
                YawDeltaDeg(y0, bodyY), YawDeltaDeg(y0, headY),
                r0[3], r1[3], r2[3]);
            Tf2VrLog(line);
        }
        std::snprintf(line, sizeof(line),
            "[TF2VR]   %d transform(s) found in the args | declined: cap %llu no-scratch %llu "
            "world-not-up %llu no-head %llu too-soon %llu faults %llu\n",
            triples,
            static_cast<unsigned long long>(g_labelDumpDeclinedCap),
            static_cast<unsigned long long>(g_labelDumpDeclinedScratch),
            static_cast<unsigned long long>(g_labelDumpDeclinedGate),
            static_cast<unsigned long long>(g_labelDumpDeclinedHead),
            static_cast<unsigned long long>(g_labelDumpDeclinedGap),
            static_cast<unsigned long long>(g_labelDumpFaults));
        Tf2VrLog(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_labelDumpFaults;
    }
}


// (The note below describes a DELETED function. RuiWidgetStrings, immediately
// after it, is a different and still-live per-draw arena reader -- do not read
// this as describing that.)
//
// THE CORE-MESSAGE TEXT HUNT LIVED HERE AND IS REMOVED, 2026-09-11.
//
// It did its job and then cost more than it was worth. It scanned the 0x200C
// string arena of every substituted widget every 250 ms, and once the message
// was found that is pure overhead on a build whose whole point this week was
// getting frame time down.
//
// What it established, so nobody rebuilds it: the arena is SHARED. Twenty
// widgets reported drawing the word CORE, including 0x5090, which draws
// NO AMMO / LOW AMMO / RELOAD -- each one reports whatever was last formatted
// into the arena, not what it drew. The hunt can prove a string EXISTS and
// read its text ("2.CORE +2`3%"), and it cannot attribute that string to a
// widget. Geometry did the attributing in the end, and a visible change
// confirmed it.
void RuiWidgetStrings(unsigned rva, const std::uint8_t* scratch) {
    if (!scratch) return;
    StringSample* slot = nullptr;
    for (int i = 0; i < g_stringsDumpedCount; ++i) {
        if (g_stringsSeen[i].rva == rva) { slot = &g_stringsSeen[i]; break; }
    }
    if (!slot) {
        if (g_stringsDumpedCount >= 64) { ++g_stringsCapped; return; }
        slot = &g_stringsSeen[g_stringsDumpedCount++];
        slot->rva = rva;
    }
    ++slot->draws;
    slot->lastScratch = scratch;
    // THE GATE, AND IT IS COUNTED. Before the world is up the arena holds the
    // frontend's leftovers and the widget cannot be showing a player's name,
    // so a sample taken there answers nothing and used to consume the budget.
    if (!RuiCensusWorldIsUp()) { ++slot->preWorld; return; }
    // The window reopens the budget. See kTextSampleCap: 0x8A330 spent a whole
    // session budget in 277 ms on the respawn screen, on text that was real.
    const unsigned long long now = GetTickCount64();
    if (!slot->windowStart || now - slot->windowStart >= kTextWindowMs) {
        slot->windowStart = now;
        slot->times = 0;
    }
    if (slot->times >= kTextSampleCap || slot->printed >= kTextSessionCap) return;
    __try {
        const char* arena = reinterpret_cast<const char*>(scratch + 0x05BC);
        char text[360]{};
        int best = 0;
        const int found = ExtractArenaText(arena, text, sizeof(text), &best);
        if (best > slot->bestStreak) slot->bestStreak = best;
        // NO TEXT SPENDS NO BUDGET. This is the property that makes the census
        // survive a wrong gate: whatever the game is drawing, only something
        // readable can use a sample up.
        if (!found) { ++slot->noText; return; }
        // Re-log only when what it SAYS has changed, so the budget buys twelve
        // different utterances rather than twelve frames of the same one. The
        // hash is over the extracted text, not the raw arena: the raw block
        // changes every frame from the float state around the strings.
        unsigned h = 2166136261u;
        for (const char* p = text; *p; ++p) {
            h = (h ^ static_cast<unsigned char>(*p)) * 16777619u;
        }
        if (slot->times > 0 && h == slot->hash) return;
        slot->hash = h;
        ++slot->times;
        ++slot->printed;
        char out[520]{};
        std::snprintf(out, sizeof(out),
                      "[TF2VR]   SAYS ui(11)+0x%X (text %d of %d this window, %d of %d overall, "
                      "draw %llu, scratch %p):%s\n",
                      rva, slot->times, kTextSampleCap, slot->printed, kTextSessionCap,
                      static_cast<unsigned long long>(slot->draws),
                      static_cast<const void*>(scratch), text);
        Tf2VrLog(out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_faults;
    }
}

bool WidgetHasWorldAnchored(unsigned rva) {
    // 2026-09-07: 0x84070 and 0x86220 are the other two per-entity type-7 widgets
    // (the nameplate comment above; 0x84070 draws once per entity in view). The
    // VR ini scales type 7 about the screen centre (hud.widget_scale, named_only 0),
    // which is the drift the ENEMY label had before 0x8A330 was seeded. Same fix.
    if (rva == 0x8A330 || rva == 0x920D0 || rva == 0x84070 || rva == 0x86220) return true;
    // THE CLASSIFIER NO LONGER ROUTES A WRITE. 2026-09-08, and this regression
    // had already cost the wearer their top health bar three times.
    //
    // This used to return g_class[i].hasWorld -- a RUNTIME CORRELATION, set
    // whenever a register's position tracked head yaw or pitch above the
    // threshold, and set on the TRANSIENT TIER too, which the classifier itself
    // describes as "good enough to say whether it tracks the view, not good
    // enough to trust a number".
    //
    // So an animated widget -- the Titan cockpit background graphics, the dash
    // bars -- could correlate with head motion by coincidence inside one short
    // window, be filed world-anchored, and take the size-only path. That path
    // multiplies the basis lanes by hud.nameplate_scale and leaves the anchor
    // alone, so the element shrinks TOWARD ITS OWN ANCHOR: smaller, and dragged
    // to the top left. Exactly what the wearer reports, every time.
    //
    // And it is not reproducible from the source alone: whether it fires depends
    // on what the census happened to observe that run, which is why it has come
    // back repeatedly with no code change to explain it.
    //
    // The four seeds above are the label widgets, identified by disappearance
    // and named in the nomenclature. They are a fixed list. Everything else
    // keeps the path it had, whatever a measurement thinks -- widen observation
    // freely, never widen what you WRITE to.
    //
    // The classifier still runs and still prints its per-register verdicts; it
    // simply no longer decides where anything is written.
    return false;
}

// ELIMINATED 2026-09-07, AND THE EVIDENCE IS IN WHAT IT DID.
//
// Switched on for one run: the SQUARE grew, the main reticle became huge once
// missiles were active, and the target markers stayed stuck inside the square
// exactly as before. Three things follow, and together they close this avenue.
//
// 1. ui(11)+0x1D560 IS THE SQUARE, not the markers. Its eight registers in four
//    symmetric pairs are the square's edge marks. Moving each of them outward
//    from the anchor by 1/0.28 is what made the square 3.6x bigger -- positions
//    alone were touched and the square still grew, which only happens if those
//    registers ARE its corners. The earlier reading of this widget as the
//    target bracket was wrong.
// 2. The markers scaled WITH the square and kept their arrangement inside it.
//    A uniform scale about a shared anchor preserves relative position, so the
//    markers were already pinned to the square before this ran.
// 3. Therefore the markers are NOT compressed world positions. Their placement
//    is wrong at source, computed relative to the aim rather than the world,
//    and no scale about any anchor can recover information that was never in
//    the position. hud2d.zoom is eliminated as the cause of the marker defect.
//
// Kept, default off, because it is a proven lever on this pass and its failure
// is the measurement that closed the avenue.
//
// ---- UNDO THE PASS SCALE FOR THE WORLD-ANCHORED MARKERS ONLY -------------
//
// `hud2d.zoom` scales the WHOLE pixel-ortho pass about the gun's aim anchor,
// which is screen centre. That is wanted for the reticle and the menus -- the
// wearer set it deliberately and full size is not acceptable -- and it is
// exactly wrong for the missile-lock target markers, which are world-anchored
// and must land on the things they mark. At 0.28 the pass compresses seven
// targets spread across the view into one clump, which is the defect: measured
// as 3.0x too little motion by the classifier, predicted as 3.6x by the zoom,
// and visible as a clump in the 2026-09-07 capture.
//
// So instead of turning the zoom off, this pre-multiplies the markers by its
// INVERSE about the same anchor. The pass then scales them back down to
// exactly where they belong, while everything else in the pass stays small.
// The reticle and the menus are untouched.
//
// WHICH WIDGETS: only ones the classifier has judged world-anchored. The
// crosshair shares this layer and is NOT world-anchored, so it self-excludes
// and cannot be moved by this. No RVA is hardcoded.
//
// SIZE IS NOT TOUCHED. Only the position lanes are corrected, so the markers
// stay the size the pass draws them and only their placement is fixed.
void RuiUnzoomWorldMarkers(unsigned rva, std::uint8_t* scratch) {
    if (!scratch) { ++g_unzoomNoScratch; return; }
    if (!g_markerUnzoom) { ++g_unzoomOff; return; }
    // THE CROSSHAIR IS THE ONLY THING ON THIS LAYER THAT MUST NOT MOVE, and it
    // is named in HUD-NOMENCLATURE as ui(11)+0x205D0, marked OUT OF SCOPE.
    // Everything else on the pixel-ortho layer is lock furniture.
    //
    // The first version gated on the classifier's world-anchored verdict
    // instead, and it never ran once: that verdict FLIPPED between two runs on
    // the same widget -- 0x1D560 read WORLD-ANCHORED with r(pitch) -0.97 and a
    // 0.132 span on 2026-09-06, then "MOVES, but not with the view" with a
    // 0.041 span on 2026-09-07 -- because the classifier correlates against the
    // HEAD and these markers follow the GUN. A correction cannot depend on a
    // predicate that changes with how the wearer happened to move.
    if (rva == 0x205D0) { ++g_unzoomCrosshair; return; }
    float zx = 0.0f, zy = 0.0f, zoom = 1.0f;
    ReadHud2dPlacement(&zx, &zy, &zoom);
    if (!(zoom > 0.01f) || std::fabs(zoom - 1.0f) < 0.001f) { ++g_unzoomNoZoom; return; }
    const float inv = 1.0f / zoom;
    // ---- ABOUT THE GUN'S AIM ANCHOR, NOT THE SCREEN CENTRE ---------------
    //
    // This is the correction the first attempt got wrong twice over. The pass
    // scale is taken about the aim anchor, and in VR that anchor is wherever
    // the motion controller points -- it is the square the wearer paints with.
    // Compressing about it is exactly why every locked target collapses INSIDE
    // the square instead of landing in the world. Undoing it about the screen
    // centre would be undoing a different transform than the one applied.
    float anchorPx = 0.0f, anchorPy = 0.0f, yawOff = 0.0f, pitchOff = 0.0f;
    ReadReticleAnchor(&anchorPx, &anchorPy, &yawOff, &pitchOff);
    float passW = 0.0f, passH = 0.0f, divX = 0.0f, divY = 0.0f;
    int hadAim = 0;
    unsigned long long updates = 0;
    ReadReticleAnchorDetail(&passW, &passH, &divX, &divY, &hadAim, &updates);
    float ax = 0.5f, ay = 0.5f;
    if (passW > 1.0f && passH > 1.0f) {
        ax = anchorPx / passW;
        ay = anchorPy / passH;
    } else {
        ++g_unzoomNoAnchor;
    }
    if (!(ax == ax) || !(ay == ay) || ax < -2.0f || ax > 3.0f || ay < -2.0f || ay > 3.0f) {
        ax = 0.5f;
        ay = 0.5f;
    }
    __try {
        const unsigned regs = *reinterpret_cast<const std::uint32_t*>(scratch + 8);
        if (regs == 0 || regs > 256) { ++g_unzoomSkipped; return; }
        const unsigned base = (regs > kFirstAuthoredReg) ? kFirstAuthoredReg : 0u;
        auto* regBase = reinterpret_cast<float*>(scratch + 0x3A80);
        for (unsigned i = base; i < regs; ++i) {
            float* r = regBase + 8 * i;
            r[4] = ax + (r[4] - ax) * inv;
            r[5] = ay + (r[5] - ay) * inv;
            r[6] = ax + (r[6] - ax) * inv;
            r[7] = ay + (r[7] - ay) * inv;
        }
        ++g_unzoomDraws;
        static unsigned told[8]{};
        static int toldCount = 0;
        bool seen = false;
        for (int i = 0; i < toldCount; ++i) {
            if (told[i] == rva) { seen = true; break; }
        }
        if (!seen && toldCount < 8) {
            told[toldCount++] = rva;
            char line[380]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] UNZOOM ui(11)+0x%X: positions pre-multiplied by %.3f about the GUN AIM "
                "ANCHOR (%.3f, %.3f of the pass) to cancel hud2d.zoom %.3f. That anchor is the "
                "square the wearer paints with, which is why the targets collapsed inside it. "
                "Size untouched; the reticle and menus keep their shrink.\n",
                rva, inv, ax, ay, zoom);
            Tf2VrLog(line);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_faults;
    }
}

void SetHudMarkerUnzoom(bool on) {
    g_markerUnzoom = on;
    Tf2VrLog(on
        ? "[TF2VR] hud.marker_unzoom = 1: the missile-lock target markers are pre-multiplied by the "
          "inverse of hud2d.zoom about screen centre, so the pass scales them back to where they "
          "belong. The reticle and the menus keep their shrink -- only the world-anchored markers "
          "on that pass are corrected.\n"
        : "[TF2VR] hud.marker_unzoom = 0: the markers take the pass scale like everything else, "
          "which compresses them into a clump instead of landing them on their targets.\n");
}

bool HudMarkerUnzoom() { return g_markerUnzoom; }

// F12 THIS BUILD: the fix against no fix, with the reticle small on BOTH sides.
void ToggleHudMarkerUnzoom() {
    const bool next = !g_markerUnzoom;
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F12: hud.marker_unzoom %d -> %d. Watch the lock markers only: do they spread onto "
        "their targets. %llu marker draws corrected so far.\n",
        g_markerUnzoom ? 1 : 0, next ? 1 : 0,
        static_cast<unsigned long long>(g_unzoomDraws));
    Tf2VrLog(line);
    SetHudMarkerUnzoom(next);
}

// ---------------------------------------------------------------------------
// hud.canvas_centre -- A FULL-SCREEN ELEMENT BELONGS AT THE FULL SCREEN.
//
// The fade, the loading screen's "Press A" overlay and the reticle are one
// surface to the wearer: the flat HUD pass, which the engine places where the
// ATTACK angles point -- the right hand, with aim.cmd = 2 (runs 4-6,
// 2026-09-10). Shifting that pass moved the reticle too, and the reticle must
// JUST follow the hand. So this is per widget, in the register file, where
// the uniform scale already works:
//
// A screen-space widget (space 1920x1080) that carries an authored register
// whose basis is the whole space -- lo = [1, 0, 0, 1] within 2% -- is a
// CANVAS. Its elements are meant to cover the screen. Wherever the engine put
// that register, every authored register of the widget is translated by the
// same delta so the canvas element's centre lands at (0.5, 0.5): hi is the
// element's anchor end (the marker analysis above: hi + b0 lands on the
// target), so the centre is hi + (b0 + b1) / 2. Scale is left alone.
//
// The reticle widget has no full-space element and is never touched. A HUD
// widget with a full-screen background (damage flash, vignette) is centred
// too, which is where it belongs. No aim maths, no constants: the delta is
// measured on the widget's own register this draw.
//
// If the engine's hand offset is NOT in the register -- if it lives in the
// vertex transform instead -- the delta reads ~0 every draw ("home") and the
// CANVAS line says so; that is the falsifier.
// ---------------------------------------------------------------------------
volatile bool g_hudCanvasCentre = false;
volatile unsigned long long g_canvasDraws = 0, g_canvasMoved = 0, g_canvasHome = 0;
volatile unsigned long long g_canvasNoCanvas = 0, g_canvasSpaceSkip = 0, g_canvasFaults = 0;

void RuiCanvasCentre(unsigned rva, std::uint8_t* scratch) {
    if (!g_hudCanvasCentre || !scratch) return;
    __try {
        ++g_canvasDraws;
        const unsigned regs = *reinterpret_cast<const std::uint32_t*>(scratch + 8);
        if (regs <= kFirstAuthoredReg || regs > 256) { ++g_canvasNoCanvas; return; }
        const auto* spaceArg = reinterpret_cast<const float*>(scratch + 0x2DD0);
        const float spaceW = spaceArg[0], spaceH = spaceArg[2];
        if (std::fabs(spaceW - 1920.0f) > 1.0f || std::fabs(spaceH - 1080.0f) > 1.0f) {
            ++g_canvasSpaceSkip;
            return;
        }
        auto* rb = reinterpret_cast<float*>(scratch + 0x3A80);
        int canvasReg = -1;
        for (unsigned i = kFirstAuthoredReg; i < regs; ++i) {
            const float* r = rb + 8 * i;
            if (std::fabs(r[0] - 1.0f) <= 0.02f && std::fabs(r[3] - 1.0f) <= 0.02f &&
                std::fabs(r[1]) <= 0.02f && std::fabs(r[2]) <= 0.02f) {
                canvasReg = static_cast<int>(i);
                break;
            }
        }
        if (canvasReg < 0) { ++g_canvasNoCanvas; return; }
        const float* c = rb + 8 * canvasReg;
        const float centreX = c[4] + 0.5f * (c[0] + c[2]);
        const float centreY = c[5] + 0.5f * (c[1] + c[3]);
        const float dx = 0.5f - centreX;
        const float dy = 0.5f - centreY;
        const bool home = std::fabs(dx) < 1e-4f && std::fabs(dy) < 1e-4f;
        // Once a second, from the draw itself: the raw register and the delta.
        static std::uint64_t lastTick = 0;
        const std::uint64_t now = GetTickCount64();
        if (now - lastTick >= 1000) {
            lastTick = now;
            char line[420]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] CANVAS ui(11)+0x%X r%02u of %u: lo[%.3f %.3f %.3f %.3f] hi[%.4f %.4f] centre "
                "(%.4f %.4f) -> delta (%+.4f %+.4f)%s | draws=%llu moved=%llu home=%llu no-canvas=%llu "
                "space-skip=%llu faults=%llu\n",
                rva, canvasReg, regs, c[0], c[1], c[2], c[3], c[4], c[5], centreX, centreY, dx, dy,
                home ? " (home, nothing written)" : "",
                g_canvasDraws, g_canvasMoved, g_canvasHome, g_canvasNoCanvas, g_canvasSpaceSkip,
                g_canvasFaults);
            Tf2VrLog(line);
        }
        if (home) { ++g_canvasHome; return; }
        for (unsigned i = kFirstAuthoredReg; i < regs; ++i) {
            float* r = rb + 8 * i;
            r[4] += dx; r[5] += dy; r[6] += dx; r[7] += dy;
        }
        ++g_canvasMoved;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_canvasFaults;
    }
}

// THE LIVE WIDGET CENSUS, AND IT SITS UPSTREAM OF EVERY GATE.
//
// The first version of this sat inside RuiWidgetUniformScale, which is
// downstream of the hud.widget_types mask -- so it could only ever report
// widgets that already passed the mask, which is the exact population it was
// built to hunt. 23 widgets, and the three hint RVAs the nomenclature names
// were not among them, because hud.widget_types = 217 admits types 0,3,4,6,7
// and nothing else. A discovery filter that inherits the gate of the thing it
// is surveying cannot find what the gate excludes.
//
// So it is called from the substitution loop instead, for EVERY substituted
// draw whatever its type, and it records the type bits and whether the mask
// would have admitted it. Read-only. Two things come out of it: a widget that
// nobody has named identifies itself by when it appears, and a widget that IS
// named identifies which type bit would be needed to reach it.
struct SeenWidget {
    unsigned rva;
    long typeBits;
    float left, right, top, bot;
    unsigned regs;
    std::uint64_t lastTick;
};
SeenWidget g_seen[128]{};
int g_seenCount = 0;

bool WidgetBox(const std::uint8_t* scratch, float& left, float& right, float& top, float& bot,
               unsigned& regs) {
    left = 1.0e9f; right = -1.0e9f; top = 1.0e9f; bot = -1.0e9f; regs = 0;
    __try {
        regs = *reinterpret_cast<const std::uint32_t*>(scratch + 8);
        if (regs == 0 || regs > 256) return false;
        unsigned lim = regs > 64 ? 64 : regs;
        const float* regBase = reinterpret_cast<const float*>(scratch + 0x3A80);
        bool any = false;
        for (unsigned i = kFirstAuthoredReg; i < lim; ++i) {
            const float* r = regBase + 8 * i;
            const float xs[2] = {r[4], r[4] + r[0]};
            const float ys[2] = {r[5], r[5] + r[3]};
            for (int k = 0; k < 2; ++k) {
                if (xs[k] < left) left = xs[k];
                if (xs[k] > right) right = xs[k];
                if (ys[k] < top) top = ys[k];
                if (ys[k] > bot) bot = ys[k];
            }
            any = true;
        }
        // A widget with no register past the header has no box to report, and
        // the old census printed the 1e9 sentinel for eleven of twenty-three.
        if (!any) { left = right = top = bot = 0.0f; return false; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void RuiNoteWidgetDraw(unsigned rva, long typeBits, const std::uint8_t* scratch) {
    if (!scratch) return;
    // COSTS NOTHING IN A RELEASE BUILD. This runs for every substituted
    // draw and walks a table of up to 128 entries to do it. Worth paying
    // while it was identifying widgets; not worth paying to a player who
    // will never read the log. One relaxed atomic load and out -- and
    // verbose is also exactly when the tag that reads this table is of
    // any use.
    if (!Tf2VrLogVerboseEnabled()) return;
    const std::uint64_t now = GetTickCount64();
    for (int i = 0; i < g_seenCount; ++i) {
        if (g_seen[i].rva == rva) { g_seen[i].lastTick = now; return; }
    }
    if (g_seenCount >= 128) return;
    SeenWidget& w = g_seen[g_seenCount++];
    w.rva = rva; w.typeBits = typeBits; w.lastTick = now;
    const bool boxed = WidgetBox(scratch, w.left, w.right, w.top, w.bot, w.regs);
    const bool admitted = (typeBits & static_cast<long>(RuiHudSizeTypes())) != 0;
    char line[360]{};
    if (boxed) {
        std::snprintf(line, sizeof(line),
            "[TF2VR] WIDGET SEEN ui(11)+0x%X type bits 0x%lX (%s by hud.widget_types): box x "
            "%.3f..%.3f y %.3f..%.3f, %u registers. Widget %d.\n",
            rva, typeBits, admitted ? "ADMITTED" : "EXCLUDED",
            static_cast<double>(w.left), static_cast<double>(w.right),
            static_cast<double>(w.top), static_cast<double>(w.bot), w.regs, g_seenCount);
    } else {
        std::snprintf(line, sizeof(line),
            "[TF2VR] WIDGET SEEN ui(11)+0x%X type bits 0x%lX (%s by hud.widget_types): no authored "
            "register to measure, %u registers. Widget %d.\n",
            rva, typeBits, admitted ? "ADMITTED" : "EXCLUDED", w.regs, g_seenCount);
    }
    Tf2VrLog(line);
}

// F6 THIS BUILD: the wearer's own tag. Pressed the moment something is on
// screen, it names every widget that has drawn in the last second and a half,
// so "the hint is up NOW" becomes a list of candidates instead of a timestamp
// to correlate against.
// UNCONDITIONAL OUTPUT. Logging ships quiet, so a tag whose lines went through
// the normal path would be filtered out and the wearer would press the key and
// get nothing -- the one failure mode a manual instrument must not have.
void RuiTagVisibleWidgets() {
    // THE TABLE IT READS IS VERBOSE-GATED, so in a release build this would
    // print a header, zero rows and a footer -- precisely the "press the key
    // and get nothing" failure its own comment forbids. Say so instead.
    if (!Tf2VrLogVerboseEnabled()) {
        Tf2VrLogAlways("[TF2VR] TAG: the widget census only runs with diag.verbose = 1, so there is "
                       "nothing to report. Set it in the ini and relaunch.\n");
        return;
    }
    const std::uint64_t now = GetTickCount64();
    char head[200]{};
    std::snprintf(head, sizeof(head),
        "[TF2VR] ==== TAG: what is on screen right now ====\n");
    Tf2VrLogAlways(head);
    int listed = 0;
    for (int i = 0; i < g_seenCount; ++i) {
        const SeenWidget& w = g_seen[i];
        if (now - w.lastTick > 1500) continue;
        ++listed;
        char line[340]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] TAG   ui(11)+0x%X type bits 0x%lX (%s): box x %.3f..%.3f y %.3f..%.3f "
            "(%.3f wide, %.3f tall), %u registers.\n",
            w.rva, w.typeBits,
            (w.typeBits & static_cast<long>(RuiHudSizeTypes())) != 0 ? "ADMITTED" : "EXCLUDED",
            static_cast<double>(w.left), static_cast<double>(w.right),
            static_cast<double>(w.top), static_cast<double>(w.bot),
            static_cast<double>(w.right - w.left), static_cast<double>(w.bot - w.top), w.regs);
        Tf2VrLogAlways(line);
    }
    char tail[200]{};
    std::snprintf(tail, sizeof(tail),
        "[TF2VR] ==== TAG END: %d widgets drawing, %d seen this session ====\n",
        listed, g_seenCount);
    Tf2VrLogAlways(tail);
}

void RuiWidgetUniformScale(unsigned rva, std::uint8_t* scratch) {
    if (!scratch) return;
    // A widget carrying a world-anchored register is a label on a thing in the
    // world. It may be resized but never moved, so it goes to the size-only
    // path instead of the screen-centre one, whatever its layer type.
    if (WidgetHasWorldAnchored(rva)) {
        static unsigned told[16]{};
        static int toldCount = 0;
        bool seen = false;
        for (int i = 0; i < toldCount; ++i) {
            if (told[i] == rva) { seen = true; break; }
        }
        if (!seen && toldCount < 16) {
            told[toldCount++] = rva;
            char line[320]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] ui(11)+0x%X carries a WORLD-ANCHORED register, so it takes the size-only "
                "path and its position is never touched. It labels something in the world.\n", rva);
            Tf2VrLog(line);
        }
        NoteWidgetPath(rva, 2);
        RuiNameplateScale(rva, scratch);
        return;
    }
    // THE ALLOWLIST GATE, before every other test, so an unnamed widget is
    // never reached by any part of this transform.
    if (g_namedOnly && !IsNamedMovableWidget(rva)) {
        ++g_namedOnlySkips;
        static unsigned told[24]{};
        static int toldCount = 0;
        bool seen = false;
        for (int i = 0; i < toldCount; ++i) {
            if (told[i] == rva) { seen = true; break; }
        }
        if (!seen && toldCount < 24) {
            told[toldCount++] = rva;
            char line[340]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] hud.widget_named_only = 1 LEAVES ui(11)+0x%X exactly where the game puts it: "
                "the nomenclature table does not name it, so it is not one of the three the wearer "
                "asked to move. This is the population the two bars are in.\n", rva);
            Tf2VrLog(line);
        }
        NoteWidgetPath(rva, 3);
        return;
    }
    if (IsWorldAnchoredWidget(rva)) {
        static unsigned announced[8]{};
        static int announcedCount = 0;
        bool seen = false;
        for (int i = 0; i < announcedCount; ++i) {
            if (announced[i] == rva) { seen = true; break; }
        }
        if (!seen && announcedCount < 8) {
            announced[announcedCount++] = rva;
            char line[300]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] hud.widget_scale SKIPS ui(11)+0x%X: world-anchored, named by the 2026-09-05 "
                "census from the assets it draws. Scaling its position would walk it off its target.\n",
                rva);
            Tf2VrLog(line);
        }
        NoteWidgetPath(rva, 3);
        return;
    }
    float s = g_hudWidgetScale;
    __try {
        ++g_hudScaleDraws;
        const unsigned regs = *reinterpret_cast<const std::uint32_t*>(scratch + 8);
        // Registers 0..2 are the engine's own mask, inset and aspect fit and are
        // not this widget's content; below three there is no content at all.
        if (regs <= kFirstAuthoredReg || regs > 256) {
            ++g_hudScaleSkippedRegs;
            return;
        }
        if (rva == g_skipRva) return;
        // ---- SCREEN SPACE ONLY, and this is why the bars went down ---------
        //
        // Every position here is normalised against the WIDGET'S OWN SPACE, and
        // for most widgets that space is 1920x1080, the screen -- so 0.5 is the
        // screen centre and scaling about it pulls things inward, which is what
        // the wearer liked. But several widgets have model-sized spaces:
        // 2048x1218, 512x672, 256x256, 925x28. STATE records why -- the Titan
        // cockpit HUD is PAINTED ONTO COCKPIT GEOMETRY rather than drawn on a
        // screen plane. For those, 0.5 is the middle of a model surface and
        // lands wherever that surface happens to be on screen.
        //
        // So scaling them about 0.5 moves their content toward the middle of a
        // piece of geometry, in whatever direction that is on screen -- down,
        // in the wearer's case. That is why every measurement I took said
        // "moved up" while they watched it move down: my numbers were in the
        // widget's space and their eyes were on the screen. The wearer was
        // right every time and the instrument was answering a different
        // question.
        //
        // A model-space widget cannot be scaled about the screen centre without
        // knowing where its surface sits, so it is left alone.
        const auto* spaceArg = reinterpret_cast<const float*>(scratch + 0x2DD0);
        const float spaceW = spaceArg[0], spaceH = spaceArg[2];
        const bool modelSpace =
            (std::fabs(spaceW - 1920.0f) > 1.0f || std::fabs(spaceH - 1080.0f) > 1.0f);
        // ---- THE TITAN COCKPIT GROUP, NAMED BY THE WEARER 2026-09-06 -------
        //
        // Releasing this group on F6 moved exactly three things, and the
        // COCKPIT-REG dump matches all three to a widget:
        //
        //   0x2110  2048x1218  the base plate. r04 is 1833x1169 px, nearly the
        //                      whole space -- the faint grey animating surround
        //                      -- and r19..r22 are stacked 674x49 / 674x38 /
        //                      642x36 bars at y 230..250, the TOP HEALTH BAR.
        //                      Both of the two the wearer wants kept, and they
        //                      are one widget, so one number serves both.
        //   0x36A0  256x256    three ~200x200 quads with rotated bases: a dial,
        //                      not bars.
        //   0x3A00  512x672    r03 is a 506x60 bar at the BOTTOM (y 612 of 672)
        //                      with r04 a ZERO-WIDTH 0x50 fill at the same y and
        //                      centre, plus twin 123x13 bars at r07/r08. This is
        //                      the two-bar indicator, and the wearer wants it
        //                      left exactly where the game puts it.
        //
        // `hud.cockpit_skip` drops that one widget; `hud.cockpit_scale` gives
        // the rest of the group its own number, because 0.55 took them in too
        // far and the wearer asked for 80%.
        if (modelSpace && g_cockpitSkipRva && rva == g_cockpitSkipRva) {
            ++g_cockpitSkipped;
            NoteWidgetPath(rva, 4);
            // ---- THE TWO BARS GET THEIR OWN SIZE AND HEIGHT ---------------
            //
            // This widget started as a plain exclusion. The wearer now wants it
            // a bit BIGGER and slid UP, so it gets its own two numbers instead:
            // `hud.bars_scale` on the basis vectors and `hud.bars_shift_y` on
            // the position. Both default to inert, so an unset ini leaves this
            // widget exactly as untouched as the plain exclusion did.
            //
            // WHICH SIGN IS UP IS NOT ASSUMED. These widgets are painted on
            // cockpit meshes and the two in this group disagree about it: under
            // the same pull toward their space centre, 0x2110's health bar had
            // its y INCREASE and 0x3A00's r03 had its y DECREASE, yet the wearer
            // saw both move DOWN the screen. So the mesh orientation differs
            // between them and a predicted sign would be a coin toss. The F6
            // ladder steps both directions instead, and the wearer's eye picks.
            const float bs = g_barsScale;
            const float by = g_barsShiftY;
            if (bs != 1.0f || by != 0.0f) {
                auto* rb = reinterpret_cast<float*>(scratch + 0x3A80);
                for (unsigned i = kFirstAuthoredReg; i < regs; ++i) {
                    float* r = rb + 8 * i;
                    if (bs != 1.0f) {
                        r[0] *= bs; r[1] *= bs; r[2] *= bs; r[3] *= bs;
                    }
                    if (by != 0.0f) {
                        r[5] += by; r[7] += by;
                    }
                }
            }
            static bool saidSkip = false;
            if (!saidSkip) {
                saidSkip = true;
                char sl[380]{};
                std::snprintf(sl, sizeof(sl),
                    "[TF2VR] the two-bar indicator ui(11)+0x%X is handled on its own: size x%.2f, "
                    "height offset %+.3f of its space. Both inert means untouched, which is what "
                    "the plain exclusion did. %llu draws so far.\n",
                    g_cockpitSkipRva, bs, by,
                    static_cast<unsigned long long>(g_cockpitSkipped));
                Tf2VrLog(sl);
            }
            return;
        }
        if (modelSpace && g_cockpitScale > 0.0f) {
            s = g_cockpitScale;
        } else if (g_screenSpaceOnly && modelSpace) {
            ++g_hudScaleSkippedSpace;
            NoteWidgetPath(rva, 4);
            static unsigned told[16]{};
            static int toldCount = 0;
            bool seen = false;
            for (int i = 0; i < toldCount; ++i) {
                if (told[i] == rva) { seen = true; break; }
            }
            if (!seen && toldCount < 16) {
                told[toldCount++] = rva;
                char line[380]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] hud.widget_scale SKIPS ui(11)+0x%X: its space is %.0fx%.0f, not the "
                    "1920x1080 screen. Model-space widgets are painted on cockpit geometry, so 0.5 is "
                    "the middle of that surface and scaling about it moves content in an arbitrary "
                    "screen direction. Set hud.widget_screenspace_only = 0 to scale them anyway.\n",
                    rva, spaceW, spaceH);
                Tf2VrLog(line);
            }
            // READ-ONLY, AND IT RUNS WHETHER OR NOT THE GATE IS RELEASED.
            // This is the group the wearer has never been able to reach, so a
            // run must not depend on them pressing anything for it to tell me
            // what is inside. Once per widget per session, every register with
            // its size and position converted to PIXELS of its own space -- a
            // bar is wide and thin and names itself on sight.
            {
                bool dumped = false;
                for (int i = 0; i < g_regDumpedCount; ++i) {
                    if (g_regDumped[i] == rva) { dumped = true; break; }
                }
                if (!dumped && g_regDumpedCount < kBoxCap) {
                    g_regDumped[g_regDumpedCount++] = rva;
                    const float* rb = reinterpret_cast<const float*>(scratch + 0x3A80);
                    for (unsigned i = kFirstAuthoredReg; i < regs && i < kFirstAuthoredReg + 40; ++i) {
                        const float* r = rb + 8 * i;
                        char rl[300]{};
                        std::snprintf(rl, sizeof(rl),
                            "[TF2VR]     COCKPIT-REG ui(11)+0x%X r%02u space %.0fx%.0f "
                            "lo[%.4f %.4f %.4f %.4f] hi[%.4f %.4f] px %.0fx%.0f at (%.0f,%.0f)\n",
                            rva, i, spaceW, spaceH, r[0], r[1], r[2], r[3], r[4], r[5],
                            r[0] * spaceW, r[3] * spaceH, r[4] * spaceW, r[5] * spaceH);
                        Tf2VrLog(rl);
                    }
                }
            }
            return;
        }
        // THE PER-WIDGET SCALE, folded into s before anything is written, so a
        // named widget takes one transform rather than two and cannot end up
        // scaled about a point it was already moved away from.
        WidgetOverride* ov = FindOverride(rva);
        const float gScale = s;          // the global, before any override
        if (ov) s *= ov->scale;          // sizes still take the product
        if (!(s > 0.0f)) return;
        // The lower-left translation has to survive this early-out too. At
        // hud.widget_scale = 1.0 with no override, the old test returned before
        // the dx below was ever reached, so a player who raised the size control
        // to 1.0 found the across-slider silently dead.
        const bool haveShift = ActiveLowerLeftShiftX() != 0.0f && IsNamedMovableWidget(rva);
        if (s == 1.0f && !haveShift &&
            (!ov || (ov->shiftX == 0.0f && ov->shiftY == 0.0f))) {
            return;
        }
        auto* regBase = reinterpret_cast<float*>(scratch + 0x3A80);
        // ---- THE BOX WITNESS ----------------------------------------------
        // Wearer: everything moved toward the centre and became visible EXCEPT
        // one two-bar jump/slide indicator, which moved DOWN almost off screen.
        // Scaling about 0.5 cannot move anything away from 0.5, so my model of
        // that element is wrong -- and I could not find it in the classifier
        // data by deduction. So stop deducing: every widget reports the screen
        // box it occupied BEFORE and AFTER, once per widget per scale setting,
        // and the one whose box moves DOWN names itself. The box spans the
        // element's anchor AND its far corner, so an element with a negative
        // basis (drawn upward or leftward from its anchor, which several of
        // these have) is measured where it actually appears rather than where
        // its anchor sits.
        float preTop = 1.0e9f, preBot = -1.0e9f, preLeft = 1.0e9f, preRight = -1.0e9f;
        for (unsigned i = kFirstAuthoredReg; i < regs; ++i) {
            const float* r = regBase + 8 * i;
            const float ys[2] = {r[5], r[5] + r[3]};
            const float xs[2] = {r[4], r[4] + r[0]};
            for (int k = 0; k < 2; ++k) {
                if (ys[k] < preTop) preTop = ys[k];
                if (ys[k] > preBot) preBot = ys[k];
                if (xs[k] < preLeft) preLeft = xs[k];
                if (xs[k] > preRight) preRight = xs[k];
            }
        }
        // The widget's own centre, from the box measured BEFORE anything is
        // written -- the same box the census and the BOX line report.
        const float pcx = (preLeft + preRight) * 0.5f;
        const float pcy = (preTop + preBot) * 0.5f;
        // Once per widget per scale setting: the BOX line below.
        bool reportBox = false;
        if (g_boxReported < kBoxCap) {
            bool seen = false;
            for (int i = 0; i < g_boxCount; ++i) {
                if (g_boxRva[i] == rva && g_boxScale[i] == s) { seen = true; break; }
            }
            if (!seen && g_boxCount < kBoxCap) {
                g_boxRva[g_boxCount] = rva;
                g_boxScale[g_boxCount] = s;
                ++g_boxCount;
                ++g_boxReported;
                reportBox = true;
            }
        }
        // Once per widget per session: every register's eight lanes, BEFORE the
        // scale touches them. Answers offline, on a run the wearer is doing
        // anyway: (1) are lanes 6,7 a copy of 4,5 on EVERY register or only the
        // marker's; (2) which widget and register are the two jump/slide bars
        // -- two small registers near the bottom of a 1920x1080 space.
        bool dumpRegs = false;
        if (reportBox) {
            bool seen = false;
            for (int i = 0; i < g_regDumpedCount; ++i) {
                if (g_regDumped[i] == rva) { seen = true; break; }
            }
            if (!seen && g_regDumpedCount < kBoxCap) {
                g_regDumped[g_regDumpedCount++] = rva;
                dumpRegs = true;
            }
        }
        unsigned differ = 0;
        // ---- THE ONLY REGISTERS THAT CAN MOVE DOWN ------------------------
        //
        // `hi' = 0.5 + (hi - 0.5) * s` with s < 1 pulls every position toward
        // 0.5, so a register can only move DOWN the screen if it starts ABOVE
        // centre. In the 2026-09-06 dump the only registers that do are the
        // full-space roots at hi (0,0): those go to (0.225, 0.225) at s = 0.55
        // and shrink to 55%, which is "moved down and got smaller" exactly.
        //
        // A root is not an element, it is the widget's container quad, and its
        // contents ride on it. For a panel anchored at the top of the screen
        // that container moving down IS moving inward, which is why the top
        // health bar looked right while it was being scaled. For the two bars
        // it is the whole defect.
        //
        // So the widgets that can hold the bars are exactly the widgets with a
        // register above centre, and this flags them for the F6 ladder. It is
        // derived per draw rather than hardcoded, so a widget that only appears
        // in some scene still gets found.
        bool anyDown = false;
        // HOISTED OUT OF THE PER-REGISTER LOOP. Both are constant for the
        // widget, and both were being evaluated once per register: the shift
        // costs up to five acquire loads and the membership test a linear walk.
        const float dx = ActiveLowerLeftShiftX();
        const bool namedMovable = IsNamedMovableWidget(rva);
        for (unsigned i = kFirstAuthoredReg; i < regs; ++i) {
            float* r = regBase + 8 * i;
            const bool dup = (r[6] == r[4]) && (r[7] == r[5]);
            if (r[5] < 0.5f) anyDown = true;
            if (!dup) ++differ;
            if (dumpRegs && i < kFirstAuthoredReg + 40) {
                char line[300]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR]     REG ui(11)+0x%X r%02u lo[%.4f %.4f %.4f %.4f] hi[%.4f %.4f %.4f %.4f]%s\n",
                    rva, i, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
                    dup ? "" : "  <== hi lanes 6,7 are NOT a copy of 4,5");
                Tf2VrLog(line);
            }
            // ALL FOUR LANES, ALWAYS. The 2026-09-06 ladder settled this: lanes
            // 6,7 were a copy of 4,5 on all 80 registers dumped, and the arm
            // that left them alone was indistinguishable on screen. Leaving a
            // full-space root register unscaled was the other arm, and it made
            // the loadout block VANISH -- roots are load-bearing and must move
            // with their siblings.
            r[0] *= s; r[1] *= s; r[2] *= s; r[3] *= s;
            if (ov) {
                // A PER-WIDGET SCALE IS SIZE ONLY, ABOUT THE WIDGET'S OWN
                // CENTRE. The global scale keeps scaling about the screen
                // centre -- that is what pulls the HUD in from the edges and
                // it is wanted. But an OVERRIDE is somebody saying "this one
                // element is the wrong size", and doing that about the screen
                // centre drags it toward the middle as it shrinks. That is the
                // coupling that cost four rungs on the gauntlet timer, and it
                // would make a size slider move things, which nobody expects.
                //
                // Global first, about the screen centre; then the override
                // about the widget's own box centre, so the element stays put:
                //   p' = 0.5 + (pc-0.5)*g + (p-pc)*g*k
                const float k = ov->scale;
                r[4] = 0.5f + (pcx - 0.5f) * gScale + (r[4] - pcx) * gScale * k;
                r[5] = 0.5f + (pcy - 0.5f) * gScale + (r[5] - pcy) * gScale * k;
                r[6] = 0.5f + (pcx - 0.5f) * gScale + (r[6] - pcx) * gScale * k;
                r[7] = 0.5f + (pcy - 0.5f) * gScale + (r[7] - pcy) * gScale * k;
            } else {
                r[4] = 0.5f + (r[4] - 0.5f) * s;
                r[5] = 0.5f + (r[5] - 0.5f) * s;
                r[6] = 0.5f + (r[6] - 0.5f) * s;
                r[7] = 0.5f + (r[7] - 0.5f) * s;
            }
            // THE TRANSLATION, AFTER THE SCALE AND ON THE X LANES ONLY, so the
            // group moves across at exactly the size it already had. Lanes 4
            // and 6 are x (they multiply spaceW in the register dump); 0..3 are
            // the basis and are deliberately untouched -- touching them is what
            // would resize it.
            if (dx != 0.0f && namedMovable) {
                r[4] += dx;
                r[6] += dx;
            }
            // The per-widget translation, same lanes and same reason: a widget
            // that is in the wrong PLACE does not want to be made smaller.
            if (ov) {
                r[4] += ov->shiftX; r[6] += ov->shiftX;
                r[5] += ov->shiftY; r[7] += ov->shiftY;
            }
        }
        g_hiLanesDiffer += differ;
        // THE LADDER MENU: only widgets that can physically produce the symptom.
        if (anyDown) {
            bool listed = false;
            for (int i = 0; i < g_scaledCount; ++i) {
                if (g_scaledRva[i] == rva) { listed = true; break; }
            }
            if (!listed && g_scaledCount < kScaledCap) {
                g_scaledRva[g_scaledCount] = rva;
                g_scaledCount = g_scaledCount + 1;
            }
        }
        if (reportBox) {
            float postTop = 1.0e9f, postBot = -1.0e9f;
            for (unsigned i = kFirstAuthoredReg; i < regs; ++i) {
                const float* r = regBase + 8 * i;
                const float ys[2] = {r[5], r[5] + r[3]};
                for (int k = 0; k < 2; ++k) {
                    if (ys[k] < postTop) postTop = ys[k];
                    if (ys[k] > postBot) postBot = ys[k];
                }
            }
            char line[460]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR]   BOX ui(11)+0x%X at scale %.2f: top %.3f -> %.3f, bottom %.3f -> %.3f "
                "(x was %.3f..%.3f) | %u regs, %u with hi lanes 6,7 differing | F6 rung %d\n",
                rva, s, preTop, postTop, preBot, postBot, preLeft, preRight,
                regs - kFirstAuthoredReg, differ, g_scaledCount);
            Tf2VrLog(line);
        }
        NoteWidgetPath(rva, 1);
        ++g_hudScaleWidgets;
        g_hudScaleLastRva = rva;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_faults;
    }
}

// F6 THIS BUILD. The ONE group left, and the only one never put in front of
// the wearer.
//
// Three runs were spent stepping widgets inside the screen-space set. Every
// rung only ever toggled parts of the lower-left loadout block, and the two
// bars never moved once -- which is not a failure to find them, it is proof
// they are not in that set at all. The census says what the remaining set is:
// ui(11)+0x2110 (2048x1218), +0x3A00 (512x672) and +0x36A0 (256x256), the
// Titan cockpit frame, whose assets are named in HUD-NOMENCLATURE as the base
// plate, CG meter and DOOMED BARS. `hud.widget_screenspace_only = 1` has been
// holding all three back since it shipped, which is exactly why nothing the
// wearer presses reaches the bars.
//
// One press moves that whole group and nothing else. If the two bars move, the
// population is settled in a single press. If they do not move, the RUI widget
// system does not draw them at all and the search goes to the pass-level
// transforms instead. Both outcomes are decisive and neither needs a third
// hypothesis.
void StepHudWidgetScreenSpaceOnly() {
    const bool next = !g_screenSpaceOnly;
    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F6: hud.widget_screenspace_only %d -> %d. %s %llu draws have been held back by this "
        "gate so far.\n",
        g_screenSpaceOnly ? 1 : 0, next ? 1 : 0,
        next ? "The Titan cockpit group is held back again."
             : "The Titan cockpit group (0x2110, 0x3A00, 0x36A0) is now MOVED. If the two bars "
               "change at all -- any direction, any size -- they are in that group.",
        static_cast<unsigned long long>(g_hudScaleSkippedSpace));
    Tf2VrLog(line);
    SetHudWidgetScreenSpaceOnly(next);
}

void SetHudCanvasCentre(bool on) {
    g_hudCanvasCentre = on;
    Tf2VrLog(on
        ? "[TF2VR] hud.canvas_centre = 1: every screen-space widget with a full-space element is "
          "moved so that element is centred -- fades and full-screen overlays in front of the "
          "head, whatever the attack angles say. The reticle has no such element and is untouched.\n"
        : "[TF2VR] hud.canvas_centre = 0: full-screen canvases stay where the engine puts them "
          "(at the attack angles, i.e. the right hand with aim.cmd = 2).\n");
}

void SetHudWidgetScreenSpaceOnly(bool only) {
    g_screenSpaceOnly = only;
    Tf2VrLog(only
        ? "[TF2VR] hud.widget_screenspace_only = 1: only widgets whose space is the 1920x1080 screen "
          "are scaled. The Titan cockpit ones are painted on model geometry, where 0.5 is the middle "
          "of a surface, not of the screen -- scaling those moved an indicator DOWN on screen while "
          "every number I had said it moved up.\n"
        : "[TF2VR] hud.widget_screenspace_only = 0: model-space widgets are scaled too. Expect content "
          "to move in arbitrary screen directions.\n");
}

void SetHudWidgetNamedOnly(bool only) {
    g_namedOnly = only;
    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] hud.widget_named_only = %d: %s\n", only ? 1 : 0,
        only
            ? "ONLY the loadout block (0x1010, 0x4320) and the earn meter (0x2B190) are moved "
              "inward. Every other widget, including whichever one holds the two bars, is left "
              "exactly where the game puts it."
            : "every substituted widget that is not world-anchored is moved, which is the state "
              "that pushes the two bars DOWN and shrinks them.");
    Tf2VrLog(line);
}

bool HudWidgetNamedOnly() { return g_namedOnly; }

// F6 THIS BUILD. One press flips the whole question the wearer asked: are the
// two bars left alone, and does anything they wanted moved stop moving?
void StepHudWidgetNamedOnly() {
    const bool next = !g_namedOnly;
    char line[400]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F6: hud.widget_named_only %d -> %d. %s Applies from the next HUD draw; %llu draws "
        "have been left alone so far.\n",
        g_namedOnly ? 1 : 0, next ? 1 : 0,
        next ? "The bars should now STOP moving and stop shrinking."
             : "The bars should now go back DOWN and small.",
        static_cast<unsigned long long>(g_namedOnlySkips));
    Tf2VrLog(line);
    SetHudWidgetNamedOnly(next);
}

void SetHudBarsScale(float scale) {
    if (!(scale == scale) || scale <= 0.0f) scale = 1.0f;
    if (scale > 3.0f) scale = 3.0f;
    g_barsScale = scale;
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] hud.bars_scale = %.2f: the two-bar indicator is drawn at this multiple of the "
        "game's own size. 1.00 is untouched.\n", scale);
    Tf2VrLog(line);
}

void SetHudBarsShiftY(float shift) {
    if (!(shift == shift)) shift = 0.0f;
    if (shift > 0.5f) shift = 0.5f;
    if (shift < -0.5f) shift = -0.5f;
    g_barsShiftY = shift;
    char line[340]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] hud.bars_shift_y = %+.3f of the widget's own space. Which sign is UP on screen is "
        "not predicted -- this widget is painted on a cockpit mesh and the group disagrees about "
        "orientation. The F6 ladder covers both directions.\n", shift);
    Tf2VrLog(line);
}

float HudBarsScale() { return g_barsScale; }
float HudBarsShiftY() { return g_barsShiftY; }

// F6 THIS BUILD: the two bars' height. Five rungs covering BOTH directions,
// because the sign of "up" for this mesh is not something to guess at the
// wearer's expense. Rung 1 is where the game puts it, so the ladder always has
// a way back to the state they already accepted.
void StepHudBarsShiftY() {
    // FINE LADDER, 2026-09-06 pass 2. The wearer stepped the coarse ladder
    // twice round and left it on -0.060, then asked for halfway to the next
    // rung, which is -0.120. So -0.090 ships and these are 0.015 steps around
    // it, all on the side they chose.
    constexpr float kLadder[5] = {-0.090f, -0.105f, -0.120f, -0.075f, -0.060f};
    const float now = g_barsShiftY;
    int rung = 0;
    for (int i = 0; i < 5; ++i) {
        if (std::fabs(now - kLadder[i]) < 0.001f) { rung = (i + 1) % 5; break; }
    }
    char line[340]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F6 rung %d of 5: hud.bars_shift_y %+.3f -> %+.3f (fine ladder -0.090, -0.105, "
        "-0.120, -0.075, -0.060, in steps of 0.015). Rung 1 is the shipped value. Size stays at "
        "x%.2f.\n",
        rung + 1, now, kLadder[rung], g_barsScale);
    Tf2VrLog(line);
    SetHudBarsShiftY(kLadder[rung]);
}

// ---- THE FRIEND AND ENEMY NAME LABELS -------------------------------------
//
// `hud.nameplate_scale`. These are type-7 widgets -- 0x84070, 0x86220 and
// 0x8A330 in the census, each carrying thousands of simultaneous layers, which
// is one per labelled entity -- and nothing was substituting type 7 at all, so
// every earlier size control missed them entirely.
//
// SIZE ONLY, AND THE GROUP MOVES AS ONE. A nameplate is WORLD-ANCHORED: it
// floats over a player, so pulling its position toward the screen centre would
// walk it off the thing it names, which is the exact waypoint defect this
// project already paid for. So the screen-centre transform is wrong here.
//
// Nor is scaling each register's basis vectors on its own enough. That shrinks
// every element toward its OWN anchor and leaves the gaps between them at full
// size, so the label and the health bar under it would drift apart instead of
// shrinking together. The wearer said they expect it to scale as one item, and
// that is the correct behaviour.
//
// So: take the group's own bounding box, and scale both the basis vectors AND
// every position about that box's CENTRE. The assembly stays rigid, gaps
// included, and its centre stays exactly where the game put it -- still on the
// entity, just smaller.
void RuiNameplateScale(unsigned rva, std::uint8_t* scratch) {
    if (!scratch) return;
    float k = g_nameplateScale;
    // A named widget multiplies the global nameplate scale, so one element can
    // be brought back up without dragging every name label with it.
    if (const WidgetOverride* ov = FindOverride(rva)) k *= ov->scale;
    if (k == 1.0f || !(k > 0.0f)) return;
    __try {
        const unsigned regs = *reinterpret_cast<const std::uint32_t*>(scratch + 8);
        if (regs <= kFirstAuthoredReg || regs > 256) {
            ++g_nameplateSkippedRegs;
            return;
        }
        auto* regBase = reinterpret_cast<float*>(scratch + 0x3A80);
        // The group's own box, spanning each element's anchor AND its far
        // corner, so an element drawn upward or leftward from its anchor is
        // measured where it actually appears.
        float left = 1.0e9f, right = -1.0e9f, top = 1.0e9f, bottom = -1.0e9f;
        for (unsigned i = kFirstAuthoredReg; i < regs; ++i) {
            const float* r = regBase + 8 * i;
            const float xs[2] = {r[4], r[4] + r[0] + r[2]};
            const float ys[2] = {r[5], r[5] + r[1] + r[3]};
            for (int j = 0; j < 2; ++j) {
                if (xs[j] < left) left = xs[j];
                if (xs[j] > right) right = xs[j];
                if (ys[j] < top) top = ys[j];
                if (ys[j] > bottom) bottom = ys[j];
            }
        }
        if (!(right > left) || !(bottom > top)) {
            ++g_nameplateSkippedRegs;
            return;
        }
        // ---- WHY THE BOUNDING-BOX CENTRE WAS WRONG, AND IT WAS MY BUG -----
        //
        // The 2026-09-06 classifier ran on 0x8A330 for the first time and its
        // verdict is unambiguous: 19 of 32 registers are WORLD-ANCHORED with
        // r(yaw) -0.92, x sweeping -4.669..5.728 as the view turns, and the
        // verdict line on every one of them reads "size only, never
        // reposition". Two are NOT: R03 is a full-space root pinned at (0,0)
        // and R04 is a fixed 107x28 element at (0.530, 0.444).
        //
        // Mixing those two into one bounding box is the defect the wearer saw.
        // The world-anchored majority sweeps five space-widths while the two
        // static ones stay put, so the box CENTRE moves at some fraction of the
        // entity's speed. Scaling about a reference that moves differently from
        // the thing being scaled displaces it by (1-k) times a distance that
        // changes as you turn -- which is a label that slides off its player
        // exactly when the head moves. The classifier told me not to do this
        // and I had already done it.
        //
        // MODE 0, THE DEFAULT: size only. The basis vectors shrink and no
        // position is touched at all, so a label CANNOT leave its entity. This
        // is what the verdict prescribes.
        //
        // MODE 1: the group scaled about the MEDIAN position rather than the
        // box centre. A median over 32 registers is decided by the 19 that move
        // together and is immune to the two that do not, so it tracks the
        // entity and the assembly shrinks as one piece with its gaps. Offered
        // as the alternative because the wearer asked for the label and its
        // health bar to scale as one item, and mode 0 keeps their spacing.
        float cx = 0.0f, cy = 0.0f;
        const bool rigid = (g_nameplateMode != 0);
        if (rigid) {
            float xs[64]{}, ys[64]{};
            int n = 0;
            for (unsigned i = kFirstAuthoredReg; i < regs && n < 64; ++i) {
                const float* r = regBase + 8 * i;
                xs[n] = r[4];
                ys[n] = r[5];
                ++n;
            }
            for (int a = 1; a < n; ++a) {
                const float vx = xs[a], vy = ys[a];
                int b = a - 1;
                while (b >= 0 && xs[b] > vx) { xs[b + 1] = xs[b]; --b; }
                xs[b + 1] = vx;
                b = a - 1;
                while (b >= 0 && ys[b] > vy) { ys[b + 1] = ys[b]; --b; }
                ys[b + 1] = vy;
            }
            cx = xs[n / 2];
            cy = ys[n / 2];
        }
        for (unsigned i = kFirstAuthoredReg; i < regs; ++i) {
            float* r = regBase + 8 * i;
            r[0] *= k; r[1] *= k; r[2] *= k; r[3] *= k;
            if (rigid) {
                r[4] = cx + (r[4] - cx) * k;
                r[5] = cy + (r[5] - cy) * k;
                r[6] = cx + (r[6] - cx) * k;
                r[7] = cy + (r[7] - cy) * k;
            }
        }
        NoteWidgetPath(rva, 2);
        ++g_nameplateDraws;
        // Once per widget: the box it occupied, so a label that is NOT one of
        // these can be told apart from one that is, without a run to ask.
        static unsigned told[8]{};
        static int toldCount = 0;
        bool seen = false;
        for (int i = 0; i < toldCount; ++i) {
            if (told[i] == rva) { seen = true; break; }
        }
        if (!seen && toldCount < 8) {
            told[toldCount++] = rva;
            char line[360]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] NAMEPLATE ui(11)+0x%X at x%.2f: %u registers, box x %.3f..%.3f y "
                "%.3f..%.3f, scaled about its own centre (%.3f, %.3f) so it stays on the entity.\n",
                rva, k, regs - kFirstAuthoredReg, left, right, top, bottom, cx, cy);
            Tf2VrLog(line);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_faults;
    }
}

void SetHudNameplateMode(int mode) {
    g_nameplateMode = (mode != 0) ? 1 : 0;
    Tf2VrLog(g_nameplateMode
        ? "[TF2VR] hud.nameplate_mode = 1: the label and its health bar shrink as ONE PIECE, gaps "
          "included, about the median of the group's positions. The median is decided by the 19 "
          "registers that move with the entity and ignores the 2 that do not, so it tracks the "
          "player. If a label slides off its player when you turn, this arm is the cause.\n"
        : "[TF2VR] hud.nameplate_mode = 0: SIZE ONLY. No position is touched at all, so a label "
          "cannot leave its entity. The spacing between the label and its health bar stays at the "
          "game's, so the group may look a little loose at small sizes.\n");
}

int HudNameplateMode() { return g_nameplateMode; }

// F6 THIS BUILD: the one behavioural variable. Rung 0 cannot drift by
// construction; rung 1 is the group-rigid version done properly.
void StepHudNameplateMode() {
    const int next = g_nameplateMode ? 0 : 1;
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F6: hud.nameplate_mode %d -> %d. Watch ONE label and turn your head: does it stay "
        "on its player. %llu label draws scaled so far.\n",
        static_cast<int>(g_nameplateMode), next,
        static_cast<unsigned long long>(g_nameplateDraws));
    Tf2VrLog(line);
    SetHudNameplateMode(next);
}

void SetHudNameplateScale(float scale) {
    if (!(scale == scale) || scale <= 0.0f) scale = 1.0f;
    if (scale > 2.0f) scale = 2.0f;
    g_nameplateScale = scale;
    char line[340]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] hud.nameplate_scale = %.2f: the friend and enemy name labels are drawn at this "
        "multiple of the game's size. Size only -- they stay on the entity they name, and the "
        "health bar under an enemy label scales with it as one piece. 1.00 is untouched.\n", scale);
    Tf2VrLog(line);
}

float HudNameplateScale() { return g_nameplateScale; }

// F6 THIS BUILD: the label size ladder. The wearer says they are "really big",
// so every rung is smaller than the game's own and rung 1 is the shipped value.
void StepHudNameplateScale() {
    constexpr float kLadder[5] = {0.60f, 0.50f, 0.40f, 0.80f, 1.00f};
    const float now = g_nameplateScale;
    int rung = 0;
    for (int i = 0; i < 5; ++i) {
        if (std::fabs(now - kLadder[i]) < 0.001f) { rung = (i + 1) % 5; break; }
    }
    char line[340]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F6 rung %d of 5: hud.nameplate_scale %.2f -> %.2f (ladder 0.60, 0.50, 0.40, 0.80, "
        "1.00). Rung 5 is the game's own size, so there is always a way back. %llu label draws "
        "scaled so far.\n",
        rung + 1, now, kLadder[rung],
        static_cast<unsigned long long>(g_nameplateDraws));
    Tf2VrLog(line);
    SetHudNameplateScale(kLadder[rung]);
}

void SetHudCockpitScale(float scale) {
    if (!(scale == scale) || scale < 0.0f) scale = 0.0f;
    if (scale > 1.5f) scale = 1.5f;
    g_cockpitScale = scale;
    char line[400]{};
    if (scale <= 0.0f) {
        std::snprintf(line, sizeof(line),
            "[TF2VR] hud.cockpit_scale = 0: the Titan cockpit group is held back entirely, which is "
            "where this front started.\n");
    } else {
        std::snprintf(line, sizeof(line),
            "[TF2VR] hud.cockpit_scale = %.2f: the Titan cockpit base plate (0x2110, the grey "
            "surround and the top health bar) and the dial (0x36A0) are moved in by this much. The "
            "two-bar indicator is excluded separately by hud.cockpit_skip.\n", scale);
    }
    Tf2VrLog(line);
}

float HudCockpitScale() { return g_cockpitScale; }

void SetHudCockpitSkip(unsigned rva) {
    g_cockpitSkipRva = rva;
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] hud.cockpit_skip = %u (ui(11)+0x%X): that cockpit widget is left entirely alone. "
        "0 means none.\n", rva, rva);
    Tf2VrLog(line);
}

// F6 THIS BUILD: the cockpit size ladder. The wearer asked for 80% after seeing
// 55% take the surround and the health bar in too far, so 0.80 is rung 1 and
// ships; the rest are there so the number can be settled by eye in one run
// without another build. The two bars are excluded at every rung.
void StepHudCockpitScale() {
    // FINE LADDER, 2026-09-06 pass 2. The wearer stepped the whole coarse ladder
    // and stopped on its smallest rung, 0.70, asking for a touch smaller still.
    // So every rung here is below 0.70 and the steps are 0.02 rather than 0.05.
    constexpr float kLadder[5] = {0.68f, 0.66f, 0.64f, 0.62f, 0.60f};
    const float now = g_cockpitScale;
    int rung = 0;
    for (int i = 0; i < 5; ++i) {
        if (std::fabs(now - kLadder[i]) < 0.001f) { rung = (i + 1) % 5; break; }
    }
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F6 rung %d of 5: hud.cockpit_scale %.2f -> %.2f (fine ladder 0.68, 0.66, 0.64, "
        "0.62, 0.60). Bigger number = less movement inward. The two bars stay out of it.\n",
        rung + 1, now, kLadder[rung]);
    Tf2VrLog(line);
    SetHudCockpitScale(kLadder[rung]);
}

void SetHudWidgetSkip(unsigned rva) {
    g_skipRva = rva;
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] hud.widget_skip = %u (ui(11)+0x%X): this widget is left entirely alone by "
        "hud.widget_scale. 0 means none.\n", rva, rva);
    Tf2VrLog(line);
}

// F6 THIS BUILD: step `hud.widget_skip` through the ONLY widgets that can move
// something down -- the ones carrying a register above the screen centre, which
// in practice is the full-space root container. Everything else in the HUD moves
// up under this transform and cannot be what the wearer is describing.
//
// Three or so rungs, not eight. Each press hands ONE of them back to the game
// entirely; the rest keep the wearer.s 0.55. The press where the two bars jump
// back up names the widget AND is the fix: the RVA goes into `hud.widget_skip`
// in the ini and ships. The same press also stops that widget.s other content
// moving inward, so the wearer judges the trade directly.
void StepHudWidgetSkip() {
    const int count = g_scaledCount;
    if (count <= 0) {
        Tf2VrLog("[TF2VR] F6: no widget has been scaled yet, so there is nothing to hand back. Get "
                 "into a map with the HUD up first -- hud.widget_scale must not be 1.00.\n");
        return;
    }
    int next = 0;
    if (g_skipRva == 0) {
        next = 0;
    } else {
        for (int i = 0; i < count; ++i) {
            if (g_scaledRva[i] == g_skipRva) { next = i + 1; break; }
        }
    }
    char line[440]{};
    if (next >= count) {
        g_skipRva = 0;
        std::snprintf(line, sizeof(line),
            "[TF2VR] F6 rung %d of %d: hud.widget_skip = 0 -- every widget is scaled again. This is "
            "the shipped state. Next press restarts at rung 1.\n", count + 1, count + 1);
    } else {
        g_skipRva = g_scaledRva[next];
        std::snprintf(line, sizeof(line),
            "[TF2VR] F6 rung %d of %d: hud.widget_skip = %u (ui(11)+0x%X) -- THAT ONE WIDGET is back "
            "at the game's own size and place; the rest stay at %.2f. If the two bars just snapped "
            "back, this RVA is the answer and it goes in the ini.\n",
            next + 1, count + 1, g_scaledRva[next], g_scaledRva[next], HudWidgetScale());
    }
    Tf2VrLog(line);
}

void SetHudWidgetScale(float scale) {
    if (!(scale == scale) || scale <= 0.0f) scale = 1.0f;
    if (scale < 0.30f) scale = 0.30f;
    if (scale > 1.50f) scale = 1.50f;
    g_hudWidgetScale = scale;
    for (int i = 0; i < 5; ++i) {
        if (kHudScaleLadder[i] == scale) { g_hudScaleRung = i; break; }
    }
    char line[560]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] hud.widget_scale = %.2f: every type-3 HUD widget EXCEPT the waypoint marker is scaled by this "
        "about the SCREEN CENTRE -- sizes and positions together, so nothing distorts and the top and "
        "bottom segments come IN rather than just getting smaller where they are. 1.00 is inert. The "
        "marker is excluded because its target is re-projected from a world point and would walk off "
        "it; it keeps hud.marker_size. Needs hud.widget_arm = 1 to have any widgets to act on.\n",
        scale);
    Tf2VrLog(line);
}

float HudWidgetScale() { return g_hudWidgetScale; }

// ---- THE OVERRIDE VALUES AS SETTINGS ---------------------------------------
//
// Three widgets were dialled in by eye and their numbers were constants no
// player could reach -- the same objection the wearer raised about the grip
// seed table, and they were right about that too. Every one is a live slider
// now. The shifts stay meaningful because an override scale is size-only:
// moving a size slider no longer drags the element across the screen.
namespace {
void SetOverrideField(unsigned rva, int field, float value) {
    WidgetOverride* ov = FindOverride(rva);
    if (!ov) return;
    if (value < -3.0f) value = -3.0f;
    if (value > 3.0f) value = 3.0f;
    if (field == 0) { if (value < 0.05f) value = 0.05f; ov->scale = value; }
    else if (field == 1) ov->shiftX = value;
    else ov->shiftY = value;
}
float OverrideField(unsigned rva, int field) {
    const WidgetOverride* ov = FindOverride(rva);
    if (!ov) return 0.0f;
    return field == 0 ? ov->scale : (field == 1 ? ov->shiftX : ov->shiftY);
}
}  // namespace

void  SetHudInstructionScale(float v)  { SetOverrideField(0x80F80u, 0, v); }
float HudInstructionScale()            { return OverrideField(0x80F80u, 0); }
void  SetHudInstructionShiftY(float v) { SetOverrideField(0x80F80u, 2, v); }
float HudInstructionShiftY()           { return OverrideField(0x80F80u, 2); }
void  SetHudTimerScale(float v)        { SetOverrideField(0x4E030u, 0, v); }
float HudTimerScale()                  { return OverrideField(0x4E030u, 0); }
void  SetHudTimerShiftX(float v)       { SetOverrideField(0x4E030u, 1, v); }
float HudTimerShiftX()                 { return OverrideField(0x4E030u, 1); }
void  SetHudTimerShiftY(float v)       { SetOverrideField(0x4E030u, 2, v); }
float HudTimerShiftY()                 { return OverrideField(0x4E030u, 2); }
void  SetHudHighlightScale(float v)    { SetOverrideField(0x27600u, 0, v); }
float HudHighlightScale()              { return OverrideField(0x27600u, 0); }
void  SetHudPromptScale(float v)       { SetOverrideField(0x151D0u, 0, v); }
float HudPromptScale()                 { return OverrideField(0x151D0u, 0); }
void  SetHudPromptShiftY(float v)      { SetOverrideField(0x151D0u, 2, v); }
float HudPromptShiftY()                { return OverrideField(0x151D0u, 2); }

void SetHudLowerLeftShiftX(float dx) {
    if (dx < -0.40f) dx = -0.40f;
    if (dx > 0.40f) dx = 0.40f;
    g_llShiftX = dx;
    char line[320]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] hud.ll_shift_x = %+.3f (ON FOOT): the loadout block and earn meter move this "
        "fraction of the screen width ACROSS, after hud.widget_scale and on the position lanes only, "
        "so their size is unchanged. Positive is right. 0 is inert. The titan's larger bar has its "
        "own value in hud.ll_shift_x_titan.\n", static_cast<double>(dx));
    Tf2VrLog(line);
}

void SetHudLowerLeftShiftXTitan(float dx) {
    if (dx < -0.40f) dx = -0.40f;
    if (dx > 0.40f) dx = 0.40f;
    g_llShiftXTitan = dx;
    char line[300]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] hud.ll_shift_x_titan = %+.3f: the same translation, applied only while the titan "
        "state has settled AND the player is in a titan. On foot the value above applies.\n",
        static_cast<double>(dx));
    Tf2VrLog(line);
}

float HudLowerLeftShiftX() { return g_llShiftX; }
float HudLowerLeftShiftXTitan() { return g_llShiftXTitan; }

// F6 THIS BUILD: the ladder for the shift above, so it can be dialled in one
// run instead of one value per run.
// LEFT ARROW, SECOND DIMENSION: the panel's HEIGHT on screen. Its size is
// settled at x0.58; what is left is that scaling about the screen centre
// carried it upward as it shrank. Positive y is DOWN -- the 2026-09-06 box
// witness established that, with full-space roots at y 0 landing at 0.225 and
// the wearer reading that as downward.
void StepInstructionPanelScale() {
    // THE WEAPON HIGHLIGHT'S SIZE, on the widget path this time. Multiplies
    // hud.widget_scale = 0.55 for this widget alone, so the rest of the HUD
    // keeps the size the wearer settled. Rung 1 is what they have been
    // seeing; rung 5 is 1/0.55, the size the game authored.
    static const float kLadder[] = {1.00f, 1.20f, 1.40f, 1.60f, 1.82f, 2.10f};
    static int rung = 0;
    const int count = static_cast<int>(sizeof(kLadder) / sizeof(kLadder[0]));
    rung = (rung + 1) % count;
    WidgetOverride* ov = FindOverride(0x27600u);
    if (ov) ov->scale = kLadder[rung];
    char line[340]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] LEFT RUNG %d of %d: weapon highlight ui(11)+0x27600 scale x%.2f, so its effective "
        "size is %.2f of what the game draws (hud.widget_scale %.2f times this). Rung 1 is what you "
        "have been seeing; rung 5 is the authored size. Nothing else on the HUD moves.\n",
        rung + 1, count, static_cast<double>(kLadder[rung]),
        static_cast<double>(kLadder[rung] * g_hudWidgetScale),
        static_cast<double>(g_hudWidgetScale));
    Tf2VrLog(line);
}

// DOWN ARROW, SECOND DIMENSION: the timer's SIZE. Its position is settled at
// -0.130 across. Shrinking it will also pull it toward the centre, on top of
// the translation already applied -- so if it ends up too far in, the shift
// wants a rung back, and the log carries both numbers to work that out from.
void StepGauntletTimerShift() {
    // The timer is settled. Kept as a no-op stub rather than deleted, because
    // its action and its ini key still exist and a dead branch that silently
    // does nothing is worse than one that says so.
    static const float kLadder[] = {0.0f};
    static int rung = 0;
    const int count = static_cast<int>(sizeof(kLadder) / sizeof(kLadder[0]));
    rung = (rung + 1) % count;
    (void)rung; (void)count; (void)kLadder;
    Tf2VrLog("[TF2VR] DOWN: the gauntlet timer is settled at x0.45, shiftX -0.1000, shiftY "
             "-0.0300. This key does nothing this build.\n");
}

void StepHudLowerLeftShiftX() {
    static const float kLadder[] = {0.00f, 0.02f, 0.04f, 0.06f, 0.09f, 0.12f};
    // ONE LADDER PER CONTEXT, so stepping as a pilot and stepping in a titan do
    // not fight each other: walking into a titan resumes that ladder where it
    // was rather than wherever the pilot one had got to.
    static int rungFoot = 0;
    static int rungTitan = 0;
    const int count = static_cast<int>(sizeof(kLadder) / sizeof(kLadder[0]));
    const bool titan = LowerLeftInTitan();
    int& rung = titan ? rungTitan : rungFoot;
    rung = (rung + 1) % count;
    char line[280]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F6 RUNG %d of %d, %s: shift -> %+.3f. Say the rung number that looks right, and "
        "which of the two you were in.\n",
        rung + 1, count, titan ? "IN A TITAN" : "ON FOOT", static_cast<double>(kLadder[rung]));
    Tf2VrLog(line);
    if (titan) SetHudLowerLeftShiftXTitan(kLadder[rung]);
    else SetHudLowerLeftShiftX(kLadder[rung]);
}

void StepHudWidgetScale() {
    g_hudScaleRung = (g_hudScaleRung + 1) % 5;
    g_hudWidgetScale = kHudScaleLadder[g_hudScaleRung];
    char line[620]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F6 rung %d of 5: hud.widget_scale = %.2f (ladder 1.00, 0.85, 0.75, 0.65, 0.55). Rung 1 is the "
        "CONTROL and is inert. Widget draws seen %llu, scaled %llu, skipped for space %llu, skipped for no content %llu, last "
        "widget ui(11)+0x%X. If scaled is ZERO the substitution never took and this says nothing about "
        "the size.\n",
        g_hudScaleRung + 1, g_hudWidgetScale,
        static_cast<unsigned long long>(g_hudScaleDraws),
        static_cast<unsigned long long>(g_hudScaleWidgets),
        static_cast<unsigned long long>(g_hudScaleSkippedSpace),
        static_cast<unsigned long long>(g_hudScaleSkippedRegs), g_hudScaleLastRva);
    Tf2VrLog(line);
}

void SetRuiAssetDump(bool selected) {
    g_dumpSelected = selected;
    Tf2VrLog(selected
        ? "[TF2VR] rui.dump = 1: the marker widget's VM state is dumped once for reference and then only "
          "when the DEFECT is present (the text block more than 0.25 from the target icon), capped at 6. "
          "Read-only. Read: tools/logdig f 'RUI DUMP'\n"
        : "[TF2VR] rui.dump = 0: no VM dump.\n");
}

void SetRuiMarkerTextX(float pin) {
    if (!(pin == pin) || pin < 0.0f || pin > 1.0f) pin = 0.0f;
    g_pinX = pin;
    for (int i = 0; i < 5; ++i) {
        if (kPinLadder[i] == pin) { g_pinRung = i; break; }
    }
    char line[520]{};
    if (pin > 0.0f) {
        std::snprintf(line, sizeof(line),
            "[TF2VR] hud.marker_text_x = %.3f: the waypoint's distance text and leader line are pinned to "
            "this fraction across the screen. %.3f is the game's own healthy value, measured. The TARGET "
            "ICON is never moved and neither is anything's height -- the defect is a pure %+.2f horizontal "
            "displacement of the authored block. The leader line is re-attached so its far end stays on the "
            "target. 0 turns it off.\n",
            pin, kGameAuthoredX, kGameAuthoredX - 0.08f);
    } else {
        std::snprintf(line, sizeof(line),
            "[TF2VR] hud.marker_text_x = 0: the text-position fix is OFF. The text sits wherever the game "
            "puts it, which the 2026-09-06 run measured at 0.08 across -- unreadable in a headset.\n");
    }
    Tf2VrLog(line);
}

float RuiMarkerTextX() { return g_pinX; }

void StepRuiMarkerTextX() {
    g_pinRung = (g_pinRung + 1) % 5;
    g_pinX = kPinLadder[g_pinRung];
    char line[520]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] F6 rung %d of 5: hud.marker_text_x = %.3f (ladder 0.32, 0.28, 0.36, 0.24, 0.40). %s "
        "Draws so far %llu, corrected %llu.\n",
        g_pinRung + 1, g_pinX,
        g_pinX > 0.0f ? "The text block moves to this fraction across the screen; the target icon does not move. Every rung is a working state -- there is no OFF rung."
                      : "OFF -- the control. This is the broken layout the wearer reported.",
        static_cast<unsigned long long>(g_draws), static_cast<unsigned long long>(g_corrected));
    Tf2VrLog(line);
    ReportRolling();
}
