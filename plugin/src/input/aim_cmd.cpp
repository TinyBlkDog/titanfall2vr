#include "aim_cmd.h"

#include "ads_lock.h"
#include "camera_hook.h"

#include "diagnostics.h"
#include "hand_pose.h"
#include "camera_update_hook.h"
#include "hook_registry.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Read by the wrapper on every call. Zero makes it an exact pass-through.
extern "C" volatile std::uint8_t g_aimCmdArmed = 0;
extern "C" volatile std::uint64_t g_aimCmdHits = 0;
extern "C" std::uintptr_t g_aimCmdTrampoline = 0;

extern "C" void aimCreateMoveInterceptor();

namespace {

// ---- the seam -------------------------------------------------------------

// CInput::CreateMove. See aim_cmd.h for how it was identified and .pdata
// checked; see aim_cmd.asm for why exactly 14 bytes are displaced.
constexpr std::uintptr_t kCreateMoveRva = 0x254D10;
constexpr std::uint8_t kExpectedCreateMove[] = {
    0x48, 0x89, 0x5C, 0x24, 0x18,   // mov [rsp+18h], rbx
    0x55,                           // push rbp
    0x56,                           // push rsi
    0x57,                           // push rdi
    0x41, 0x54,                     // push r12
    0x41, 0x55,                     // push r13
    0x41, 0x56,                     // push r14
};
constexpr std::size_t kDisplaced = sizeof(kExpectedCreateMove);
static_assert(kDisplaced == 14, "the absolute jump is exactly 14 bytes");

// ---- the engine's own integrity calls -------------------------------------
//
// These are CALLED, not re-implemented. CreateMove ends by running exactly
// these three, in this order, and a post-process write is only real if the
// same three are run again over it -- otherwise CInput::VerifyUserCmd restores
// the stored copy before the command is serialised and the drive is silently
// dead. Each is byte-checked like the seam is: calling into the middle of the
// wrong function is worse than not calling at all.

constexpr std::uintptr_t kCmdHashRva = 0x33C580;      // hash(+0x28,+0x0C,..) -> cmd+0x50
constexpr std::uint8_t kExpectedHash[] = {0x40, 0x53, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00};

constexpr std::uintptr_t kCmdCopyRva = 0x33C3A0;      // CUserCmd::operator=(dst, src)
constexpr std::uint8_t kExpectedCopy[] = {0x48, 0x89, 0x7C, 0x24, 0x18, 0x41, 0x56,
                                          0x48, 0x83, 0xEC, 0x20};

constexpr std::uintptr_t kCmdChecksumRva = 0x33C650;  // CUserCmd::GetChecksum
constexpr std::uint8_t kExpectedChecksum[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55,
                                              0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x20};

using CmdHashFn = void(__fastcall*)(void* cmd);
using CmdCopyFn = void*(__fastcall*)(void* dst, const void* src);
using CmdChecksumFn = std::uint32_t(__fastcall*)(const void* cmd);

CmdHashFn g_cmdHash = nullptr;
CmdCopyFn g_cmdCopy = nullptr;
CmdChecksumFn g_cmdChecksum = nullptr;

// ---- CUserCmd and the command ring ----------------------------------------
//
// Every one of these came out of CreateMove's own code, not out of the
// server-side layout: the plan asked for that specifically and it is the only
// reason the offsets can be trusted client-side.

constexpr std::size_t kCmdCommandNumber = 0x00;   // mov [rdi], ebx  (the sequence arg)
constexpr std::size_t kCmdViewAngles = 0x0C;      // movss [rdi+0xC/0x10/0x14]
constexpr std::size_t kCmdAttackAngles = 0x28;    // never written by CreateMove
constexpr std::size_t kCmdButtons = 0x40;         // mov [rdi+0x40], eax, straight from GetButtonBits
constexpr std::size_t kCmdStride = 0x148;
constexpr std::size_t kVerifiedStride = 0x150;
constexpr std::size_t kVerifiedCrc = 0x148;
// CInput+0x18+0xE0 and +0xE8 in CreateMove; CInput+0x100 in VerifyUserCmd.
constexpr std::size_t kInputCommandsPtr = 0xF8;
constexpr std::size_t kInputVerifiedPtr = 0x100;
// The engine divides the sequence by 0x12C and takes the remainder.
constexpr int kRingSize = 300;

// ---- state ----------------------------------------------------------------

std::uint8_t* g_client = nullptr;
std::atomic_bool g_installed = false;
std::atomic<int> g_state = static_cast<int>(AimCmdState::Off);
std::atomic<int> g_wantedState = static_cast<int>(AimCmdState::Off);
std::atomic<int> g_source = 0;         // 0 synthetic yaw, 1 composed controller aim
std::atomic<float> g_yawDegrees = 30.0f;
// One-shot snap-turn delta, consumed by the next command. See the note at the
// write for why one tick is instantaneous and many ticks is a spin.
std::atomic<float> g_pendingSnapYaw{0.0f};
std::atomic_uint64_t g_snapWrites{0};
// Edge latch for the ADS alignment. Command-thread only, so a plain bool.
bool g_adsAligned = false;
// The face-aim turn applied at this ADS entry, undone at release. See the
// release branch.
float g_adsTurnApplied = 0.0f;
// Head delta and pitch latched at ADS entry: the view is steered by the hand's
// changes, never by the head's. See the follow block.
float g_adsHeadYawDeltaAtEntry = 0.0f;
float g_adsHeadPitchAtEntry = 0.0f;
// The camera-latch head delta at entry: engine yaw = hand yaw - this, for the
// whole press. Head yaw then passes through the camera with no engine write.
float g_adsMeasuredDeltaAtEntry = 0.0f;
std::uint64_t g_adsFollowWrites = 0;
// While ADS follows, the rendered view angles from the last frame: the gun is
// drawn there and the round is fired along them.
bool g_adsGunIsView = false;
float g_adsViewAngles[3]{};
std::atomic_bool g_adsFaceAimReturn{true};
// ads.face_aim: on ADS entry, turn to face where the gun was pointing rather
// than adopting where the head was looking. Default ON -- it is what was asked
// for -- and a setting so a turn that reads as too violent can be switched off
// in the panel rather than rebuilt.
std::atomic_bool g_adsFaceAim{true};
// ads.face_aim_pitch: the same turn in the vertical. Separate from face_aim
// because it is the one with a real chance of being sickening -- while it is
// non-zero, level in the headset is not level in the world -- and the yaw turn
// is settled and good, so it must be possible to keep one and drop the other.
std::atomic_bool g_adsFaceAimPitch{true};

float WrapDegrees180(float degrees) {
    while (degrees > 180.0f) degrees -= 360.0f;
    while (degrees < -180.0f) degrees += 360.0f;
    return degrees;
}

// The falsifier's raw material. One writer (the game thread inside the
// wrapper), one reader (the plugin frame); a torn read costs a log line.
volatile float g_lastEngineView[3]{};
volatile float g_lastEngineAttack[3]{};
volatile float g_lastWritten[3]{};
volatile float g_lastVerifiedReadback[3]{};
volatile int g_lastButtons = 0;
std::atomic_uint64_t g_writes = 0;
std::atomic_uint64_t g_reads = 0;
std::atomic_uint64_t g_refused = 0;
std::atomic_uint64_t g_faulted = 0;
std::atomic_uint64_t g_repairMismatch = 0;
// Log every one of the first few after a state change, then settle to 1 Hz.
std::atomic<int> g_verboseLeft = 0;
std::uint64_t g_lastLogTick = 0;

const char* StateName(int state) {
    switch (static_cast<AimCmdState>(state)) {
        case AimCmdState::Off: return "OFF (pass-through)";
        case AimCmdState::Log: return "LOG ONLY (reads, writes nothing)";
        case AimCmdState::WriteAttack: return "WRITE attackangles (+0x28) -- Candidate A";
        case AimCmdState::WriteView: return "WRITE worldViewAngles (+0x0C) -- Candidate B";
    }
    return "?";
}

// RE-RUN THE ENGINE'S OWN THREE, IN THE ENGINE'S OWN ORDER.
//
// CreateMove does exactly this at +0x761/+0x76C/+0x774 as its last act, with
// whatever value we are about to replace. Without it the command reaching the
// wire is the one stored in m_pVerifiedCommands, because CInput::VerifyUserCmd
// copies that back over any command whose CRC no longer matches -- so the write
// reads back perfectly and does nothing at all.
//
// IT IS A FUNCTION NOW BECAUSE THERE ARE THREE EXITS THAT NEED IT. The ADS
// alignment below writes worldViewAngles and then, in two of the three paths
// out of this post-process, the old code returned BEFORE the commit ran. That
// is this file's own "a write that reads back proves nothing", and it would
// have produced an alignment that logged perfectly and moved nothing.
void CommitCommand(std::uint8_t* cmd, std::uint8_t* verified);

bool CheckBytes(const char* what, std::uintptr_t rva, const std::uint8_t* expected,
                std::size_t count) {
    const std::uint8_t* at = g_client + rva;
    if (std::memcmp(at, expected, count) == 0) return true;
    char line[420]{};
    int used = std::snprintf(line, sizeof(line),
        "[TF2VR] aimcmd: %s at client.dll+0x%llX does not begin with the bytes it was resolved "
        "from, so this is not the build those offsets came from. NOT installed. Found:",
        what, static_cast<unsigned long long>(rva));
    for (std::size_t i = 0; i < count && used < static_cast<int>(sizeof(line)) - 8; ++i) {
        used += std::snprintf(line + used, sizeof(line) - used, " %02X", at[i]);
    }
    std::snprintf(line + used, sizeof(line) - used, "\n");
    Tf2VrLog(line);
    return false;
}

// What we want the aim angles to BE this tick. Returns false when the source
// has nothing to say, in which case nothing is written at all -- writing a
// stale or zero pose would throw the aim somewhere arbitrary, and a drive that
// writes garbage is harder to read than one that does not write.
bool TargetAngles(const float engineView[3], float out[3]) {
    if (g_source.load(std::memory_order_relaxed) == 0) {
        // FR2. Relative to what the ENGINE just computed on this same call --
        // never an absolute rebuilt from some global on another clock. That is
        // the anchoring lesson from the pin, and it applies here for the same
        // reason: it makes the write idempotent and it cannot compound.
        out[0] = engineView[0];
        out[1] = engineView[1] + g_yawDegrees.load(std::memory_order_relaxed);
        out[2] = engineView[2];
        return true;
    }
    // FR3/H1. The same composition the pin already uses for the gun's
    // orientation -- aim pose in, Source world angles out. Only the consumer
    // is new.
    float position[3]{};
    float angles[3]{};
    if (!TryGetComposedHandPose(position, angles)) return false;
    out[0] = angles[0];
    out[1] = angles[1];
    out[2] = angles[2];
    return true;
}

void CommitCommand(std::uint8_t* cmd, std::uint8_t* verified) {
    g_cmdHash(cmd);
    g_cmdCopy(verified, cmd);
    *reinterpret_cast<std::uint32_t*>(verified + kVerifiedCrc) = g_cmdChecksum(cmd);
}

void LogLine(const char* tag, int sequence, const float view[3], const float attack[3],
             const float written[3], const float readback[3], int buttons) {
    char line[420]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] aimcmd %s seq=%d buttons=0x%08X | engine view <%.2f,%.2f,%.2f> attack "
        "<%.2f,%.2f,%.2f> | wrote <%.2f,%.2f,%.2f> | verified copy reads <%.2f,%.2f,%.2f>\n",
        tag, sequence, static_cast<unsigned>(buttons), view[0], view[1], view[2], attack[0],
        attack[1], attack[2], written[0], written[1], written[2], readback[0], readback[1],
        readback[2]);
    Tf2VrLog(line);
}

}  // namespace

// The wrapper calls this AFTER CreateMove has returned, with the two values it
// carried across in callee-saved registers.
extern "C" void AimCmdPostProcess(void* input, int sequence) {
    const int state = g_state.load(std::memory_order_relaxed);
    if (state == static_cast<int>(AimCmdState::Off) || !input) return;

    __try {
        auto* self = static_cast<std::uint8_t*>(input);
        // EVERY REFUSAL SAYS WHY, ONCE. A `refused` count with no reason beside
        // it is an unreadable instrument, and the run is over by the time
        // anyone notices.
        const int index = sequence % kRingSize;
        if (index < 0 || index >= kRingSize) {
            if (g_refused.fetch_add(1, std::memory_order_relaxed) == 0) {
                char line[240]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] aimcmd: REFUSING -- sequence %d gives ring slot %d, outside 0..%d. "
                    "Nothing was written.\n", sequence, index, kRingSize - 1);
                Tf2VrLog(line);
            }
            return;
        }
        auto* commands = *reinterpret_cast<std::uint8_t**>(self + kInputCommandsPtr);
        auto* verifieds = *reinterpret_cast<std::uint8_t**>(self + kInputVerifiedPtr);
        if (!commands || !verifieds) {
            if (g_refused.fetch_add(1, std::memory_order_relaxed) == 0) {
                char line[280]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] aimcmd: REFUSING -- the command arrays are null (CInput+0x%zX=%p, "
                    "CInput+0x%zX=%p). Either the seam ran before they were allocated or those "
                    "offsets are wrong for this build. Nothing was written.\n",
                    kInputCommandsPtr, static_cast<void*>(commands), kInputVerifiedPtr,
                    static_cast<void*>(verifieds));
                Tf2VrLog(line);
            }
            return;
        }
        std::uint8_t* cmd = commands + static_cast<std::size_t>(index) * kCmdStride;
        std::uint8_t* verified = verifieds + static_cast<std::size_t>(index) * kVerifiedStride;

        // THE POINTER FALSIFIER, AND IT COSTS ONE COMPARE.
        //
        // CreateMove's third instruction after the ring maths is
        // `mov [rdi], ebx` -- command_number = sequence. If our own arithmetic
        // for the same slot does not land on a command carrying the sequence we
        // were called with, the layout assumption is wrong and every byte after
        // this would be written into the wrong place. Refuse rather than write.
        const int commandNumber = *reinterpret_cast<const int*>(cmd + kCmdCommandNumber);
        if (commandNumber != sequence) {
            if (g_refused.fetch_add(1, std::memory_order_relaxed) == 0) {
                char line[320]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] aimcmd: REFUSING to touch the command. Slot %d of the ring carries "
                    "command_number %d but CreateMove was called with sequence %d, so the ring "
                    "arithmetic (commands=CInput+0x%zX, stride 0x%zX) is wrong for this build. "
                    "Nothing was written.\n",
                    index, commandNumber, sequence, kInputCommandsPtr, kCmdStride);
                Tf2VrLog(line);
            }
            return;
        }

        auto* view = reinterpret_cast<float*>(cmd + kCmdViewAngles);
        auto* attack = reinterpret_cast<float*>(cmd + kCmdAttackAngles);
        const float engineView[3] = {view[0], view[1], view[2]};
        const float engineAttack[3] = {attack[0], attack[1], attack[2]};
        const int buttons = *reinterpret_cast<const int*>(cmd + kCmdButtons);
        for (int i = 0; i < 3; ++i) {
            g_lastEngineView[i] = engineView[i];
            g_lastEngineAttack[i] = engineAttack[i];
        }


        // THE STEP, CAUGHT IN THE ACT.
        //
        // The census samples at 1 Hz and the engine's view pitch jumps +15 to
        // +22 degrees INSIDE a single crouch transition, so a per-second sample
        // can only ever show the aftermath. This runs on the command build --
        // every tick -- and reports the step itself, with what the eye height
        // was doing at that instant.
        //
        // Both of our own pitch terms are already exonerated by measurement
        // (head stays within -3.6..+2.2, the ADS offset returns to exactly 0.00
        // after every press), so what is wanted here is not another look at our
        // writes but the ENGINE's value changing between one tick and the next
        // while we are not writing it at all. `wroteThisTick` says which of
        // those two cases each line is.
        {
            static float previousEnginePitch = 0.0f;
            static bool havePrevious = false;
            static float previousCameraZ = 0.0f;
            static std::uint32_t stepLines = 0;
            static int burst = 0;
            const float cameraZ = GameCameraHeightUnits();
            const float step = havePrevious ? engineView[0] - previousEnginePitch : 0.0f;
            const float heightStep = havePrevious ? cameraZ - previousCameraZ : 0.0f;
            // A crouch transition opens a short burst so the ticks either side
            // of it are all present, not only the one that crossed a threshold.
            if (std::fabs(heightStep) > 1.0f) burst = 12;
            const bool interesting = std::fabs(step) > 3.0f || burst > 0;
            if (havePrevious && interesting && stepLines < 400) {
                ++stepLines;
                if (burst > 0) --burst;
                char line[420]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] PITCHSTEP#%u: engine view pitch %+.2f -> %+.2f (step %+.2f) | camera z "
                    "%.2f -> %.2f (step %+.2f) | attackangles pitch %+.2f | buttons %08X | ads "
                    "aligned=%d\n",
                    stepLines, static_cast<double>(previousEnginePitch),
                    static_cast<double>(engineView[0]), static_cast<double>(step),
                    static_cast<double>(previousCameraZ), static_cast<double>(cameraZ),
                    static_cast<double>(heightStep), static_cast<double>(engineAttack[0]),
                    static_cast<unsigned>(buttons), g_adsAligned ? 1 : 0);
                Tf2VrLog(line);
            }
            previousEnginePitch = engineView[0];
            previousCameraZ = cameraZ;
            havePrevious = true;
        }
        // Set by anything that writes worldViewAngles. The commit at the end of
        // each exit path is conditional on it, because a write without the
        // engine's own three integrity calls re-run over it is reverted by
        // CInput::VerifyUserCmd before it reaches the wire.
        bool viewDirty = false;

        // ---------------------------------------------------------------
        // SNAP TURN. A ONE-SHOT VIEW-ANGLE WRITE, AND IT IS INSTANTANEOUS.
        //
        // This runs regardless of the aim.cmd diagnostic state, because it is
        // not a diagnostic -- it is a comfort feature, and it must work in the
        // shipped Off state.
        //
        // WHY A DIRECT WRITE AND NOT THE TURN BUTTON. The first attempt held the
        // engine's +left/+right for degrees/yawspeed seconds. That is a fast
        // SWEEP, roughly 17 frames at 80 Hz for 45 degrees, and it is exactly
        // what snap turn exists to avoid: the whole point is to remove the
        // optical flow that causes the vestibular mismatch. A quick smooth turn
        // can make a motion-sick player MORE sick, not less. The wearer caught
        // that, and they were right.
        //
        // WHY THIS IS SAFE TO DO ONCE. FR2 measured that a worldViewAngles write
        // COMPOUNDS -- the engine reads it back into its own view state, so +30
        // every tick is a 2400 deg/s spin. Applied for exactly ONE tick, that
        // same property is precisely what is wanted: the view moves by the delta
        // and STAYS there. The hazard and the feature are the same mechanism;
        // the difference is entirely in doing it once.
        //
        // RELATIVE TO WHAT THE ENGINE JUST COMPUTED, never an absolute rebuilt
        // from a global on another clock. Same anchoring rule as the pin.
        {
            const float snap = g_pendingSnapYaw.exchange(0.0f, std::memory_order_acq_rel);
            if (snap != 0.0f) {
                view[1] = engineView[1] + snap;
                viewDirty = true;
                g_snapWrites.fetch_add(1, std::memory_order_relaxed);
                char line[220]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] SNAP TURN: yaw %.1f -> %.1f (%+.0f deg) in ONE tick. No intermediate "
                    "frames.\n",
                    static_cast<double>(engineView[1]), static_cast<double>(view[1]),
                    static_cast<double>(snap));
                Tf2VrLog(line);
            }
        }
        // ---- ADS ALIGNMENT: put the BODY under the RENDERED view ----
        //
        // Reported, with ads.lock on: "it zooms into where I'm looking, not
        // where the reticle is", and "the gun is NOT centered in the screen but
        // on where I was looking."
        //
        // One cause. In VR the rendered view and the game's own view are two
        // different things (camera_hook.h, GetHeadViewDelta):
        //
        //     rendered yaw   = baseYaw + headingDelta
        //     rendered pitch = headPitch          -- ABSOLUTE, the game's own
        //                                            view pitch is not used
        //
        // Everything the engine does for ADS -- where it puts the viewmodel,
        // where the reticle goes, what the zoom is centred on -- is built around
        // the GAME's view. Handing ADS back to the engine therefore lands all of
        // it on the body's axis while the wearer is looking along the head's.
        // Vertically the two are not even related.
        //
        // So while ADS is engaged the body is driven onto the rendered view:
        //
        //   PITCH, every tick, absolutely. The rendered pitch IS headPitch and
        //   nothing feeds back, so writing it is idempotent -- the pin's rule,
        //   "absolute, never a delta", for the same reason.
        //
        //   YAW, once, on the entry edge, by folding the head's delta into the
        //   base and recentring so the delta becomes zero. The sum is unchanged,
        //   so THE RENDERED IMAGE DOES NOT MOVE: this is not a view rotation the
        //   wearer sees, it is the body turning underneath one. It cannot be
        //   done every tick, because a fold without a recentre compounds (FR2
        //   measured exactly that: +30 a tick is a 2400 deg/s spin) and a
        //   recentre every tick would re-capture the POSITION reference too and
        //   break positional tracking.
        if (AdsLockEngaged()) {
            float headYawDelta = 0.0f, headPitch = 0.0f;
            if (GetHeadViewDelta(&headYawDelta, &headPitch)) {
                // THE BODY PITCH WRITE IS GONE. It existed only to carry the
                // round while the aim write was standing down, and the aim write
                // no longer stands down -- attackangles takes BOTH axes from the
                // hand below, which is what the round and the reticle follow.
                // Keeping it would be a second author on worldViewAngles.
                float handPos[3]{}, handAng[3]{};
                const bool haveHand = g_adsFaceAim.load(std::memory_order_relaxed) &&
                                      TryGetComposedHandPose(handPos, handAng);
                // THE VIEW FOLLOWS THE HAND FOR AS LONG AS ADS IS HELD. 2026-09-04.
                //
                // CLOSED ON THE RENDERED VIEW, not predicted from it. The first
                // version steered the body yaw so that engineYaw + headYawDelta
                // equalled the hand's yaw. The camera composes the head's FULL
                // rotation, and a head with pitch and roll turned 39 deg does not
                // add up as a yaw: the run showed the gun 2.3 deg off the
                // rendered view and the view carrying 3.8 deg of roll the gun did
                // not -- exact with the head straight, tilted with it turned.
                //
                // So: read the yaw the camera actually rendered last frame, and
                // turn the body by the difference to where it should be. One
                // correction per rendered frame (the latch generation), so two
                // ticks in one frame cannot double-apply. Pitch is closed the
                // same way through the view pitch offset. The gun is then drawn
                // at the rendered view's own angles and the round is fired along
                // them (placement_pin.cpp, TargetAngles): gun, view and round are
                // one thing, which is the flat game's ADS.
                //
                // THE HEAD IS NOT CANCELLED: the target for the rendered view is
                // the hand plus however far the head has turned since entry, so
                // looking around in ADS still looks around.
                const bool first = !g_adsAligned;
                g_adsAligned = true;
                const bool faceAim = haveHand;
                float camBase[3]{}, camApplied[3]{};
                std::uint32_t camGen = 0;
                const bool haveCam = ReadLatchedCameraAngles(camBase, camApplied, &camGen);
                if (first) {
                    g_adsHeadYawDeltaAtEntry = headYawDelta;
                    g_adsHeadPitchAtEntry = headPitch;
                    g_adsFollowWrites = 0;
                    g_adsGunIsView = false;
                }
                if (faceAim) {
                    // NO INTEGRATION, AND NO ENGINE WRITE FOR THE HEAD. The latch
                    // carries both the engine yaw of a frame and the yaw it
                    // rendered; their difference is the head's real delta, cross
                    // terms included. It is LATCHED AT ENTRY: the engine yaw is
                    // set to the hand's yaw minus that entry delta, and the
                    // head's later yaw motion passes straight through the camera
                    // -- rendered = engine + delta(now) = hand + (delta(now) -
                    // delta(entry)) -- with no write, no frame of lag, exactly as
                    // head PITCH already does through the view pitch offset. Only
                    // the HAND drives engine writes. (2026-09-04: the earlier
                    // version fed the head's motion through the engine write, one
                    // frame late, which made left/right feel damped differently
                    // from up/down.)
                    if (first) {
                        g_adsMeasuredDeltaAtEntry = haveCam ? WrapDegrees180(camApplied[1] - camBase[1])
                                                            : headYawDelta;
                    }
                    const float targetEngine = WrapDegrees180(handAng[1] - g_adsMeasuredDeltaAtEntry);
                    const float turn = WrapDegrees180(targetEngine - engineView[1]);
                    if (first || std::fabs(turn) > 0.02f) {
                        view[1] = targetEngine;
                        viewDirty = true;
                        NoteExternalBodyYawWrite(turn);
                        AddAdsYawCompensation(turn, first);
                        g_adsTurnApplied += turn;
                        if (first) {
                            char line[560]{};
                            std::snprintf(line, sizeof(line),
                                "[TF2VR] ADS ALIGN: turned to face where the GUN was pointing. YAW: hand %.1f, "
                                "measured head delta %+.1f latched (%s), body %.1f -> %.1f, a %+.1f deg turn in one "
                                "tick. PITCH: the gun points %.1f while the head looks %.1f, %+.1f deg apart. The "
                                "body follows the HAND until release; head yaw and pitch pass straight through; the "
                                "gun is drawn at the rendered view's angles and the round fired along them.\n",
                                static_cast<double>(handAng[1]), static_cast<double>(g_adsMeasuredDeltaAtEntry),
                                haveCam ? "camera latch" : "yaw-only fallback",
                                static_cast<double>(engineView[1]), static_cast<double>(view[1]),
                                static_cast<double>(turn),
                                static_cast<double>(handAng[0]), static_cast<double>(headPitch),
                                static_cast<double>(WrapDegrees180(handAng[0] - headPitch)));
                            Tf2VrLog(line);
                        } else {
                            ++g_adsFollowWrites;
                        }
                    }
                    if (g_adsFaceAimPitch.load(std::memory_order_relaxed)) {
                        SetViewPitchOffset(WrapDegrees180(handAng[0] - g_adsHeadPitchAtEntry));
                    }
                    // The angles the gun is drawn at and the round fired along:
                    // this tick's engine yaw plus the CURRENT measured delta (=
                    // the view as it renders, head motion included), the rendered
                    // pitch, the rendered roll.
                    if (haveCam) {
                        const float measuredNow = WrapDegrees180(camApplied[1] - camBase[1]);
                        const float engineNow = viewDirty ? view[1] : engineView[1];
                        g_adsViewAngles[0] = camApplied[0];
                        g_adsViewAngles[1] = WrapDegrees180(engineNow + measuredNow);
                        g_adsViewAngles[2] = camApplied[2];
                        g_adsGunIsView = true;
                    }
                } else if (first) {
                    const float before = WrapDegrees180(engineView[1] + headYawDelta);
                    view[1] = engineView[1] + headYawDelta;
                    viewDirty = true;
                    const float turn = WrapDegrees180(view[1] - engineView[1]);
                    NoteExternalBodyYawWrite(turn);
                    AddAdsYawCompensation(turn, true);
                    g_adsTurnApplied += turn;
                    char line[300]{};
                    std::snprintf(line, sizeof(line),
                        "[TF2VR] ADS ALIGN: folded the head's delta in (ads.face_aim off, or no hand pose). "
                        "YAW: rendered %.1f -> %.1f (body %.1f -> %.1f).\n",
                        static_cast<double>(before),
                        static_cast<double>(WrapDegrees180(view[1] + headYawDelta)),
                        static_cast<double>(engineView[1]), static_cast<double>(view[1]));
                    Tf2VrLog(line);
                }
            }
        } else {
            // RELEASE: the view returns to where the head physically is.
            //
            // The pitch offset is held for exactly as long as ADS is and no
            // longer. Leaving it in place was tried and is what "You did exactly
            // what I said you must not do" was about: the horizon stayed wrong
            // for ordinary play and every later press started from a tilted
            // world.
            //
            // Zero is the right target rather than "whatever it was at entry",
            // because outside ADS the offset is always zero -- so the two are
            // the same thing here, and zero needs nothing remembered across the
            // press. It is also the only reference that cannot go stale: the
            // head's physical tilt is wherever the head physically is.
            // UNDO THE FACE-AIM TURN ON RELEASE. 2026-09-04.
            //
            // Entry turned the body by `turn` so the gun's target sat in the
            // middle of the view, and subtracted the same turn from the hand
            // composition so the gun did not turn with the world. Left in
            // place after release, that subtraction is a permanent yaw offset
            // between the physical hand and the gun, and it ACCUMULATED across
            // every ADS press (-14.5 deg after six presses, -41.6 after a
            // session): the gun sat a foot beside the hand after aiming at the
            // edge of the view, and hip aim drifted the more the wearer played.
            // Turning the body back by exactly the entry turn, and taking the
            // same amount out of the compensation, returns the world, the gun
            // and the hand to where they were before the press. The total can
            // then never grow. ads.face_aim_return = 0 is the old behaviour.
            if (g_adsAligned) {
                SetViewPitchOffset(0.0f);
                if (g_adsTurnApplied != 0.0f && g_adsFaceAimReturn.load(std::memory_order_relaxed)) {
                    const float back = -g_adsTurnApplied;
                    view[1] = WrapDegrees180(engineView[1] + back);
                    viewDirty = true;
                    NoteExternalBodyYawWrite(back);
                    AddAdsYawCompensation(back);
                    char line[260]{};
                    std::snprintf(line, sizeof(line),
                        "[TF2VR] ADS RELEASE: body turned back %+.1f deg in one tick (yaw %.1f -> %.1f), the "
                        "entry turn taken out of the hand compensation; total is now %+.1f.\n",
                        static_cast<double>(back), static_cast<double>(engineView[1]),
                        static_cast<double>(view[1]), static_cast<double>(AdsYawCompensation()),
                        static_cast<unsigned long long>(g_adsFollowWrites));
                    Tf2VrLog(line);
                }
                g_adsTurnApplied = 0.0f;
            }
            g_adsAligned = false;
            g_adsGunIsView = false;
        }

        g_lastButtons = buttons;
        g_reads.fetch_add(1, std::memory_order_relaxed);

        const bool verbose = g_verboseLeft.load(std::memory_order_relaxed) > 0;
        if (verbose) g_verboseLeft.fetch_sub(1, std::memory_order_relaxed);

        if (state == static_cast<int>(AimCmdState::Log)) {
            if (verbose) {
                const float none[3] = {0.0f, 0.0f, 0.0f};
                LogLine("LOG", sequence, engineView, engineAttack, none, none, buttons);
            }
            // Log writes nothing of its own, but the snap turn and the ADS
            // alignment above are not diagnostics and must still reach the wire.
            if (viewDirty) CommitCommand(cmd, verified);
            return;
        }

        // THE AIM WRITE NO LONGER STANDS DOWN IN ADS, AND THAT IS THE FIX FOR
        // "bullets hit in a vertical line".
        //
        // While it stood down, the round followed worldViewAngles -- and only
        // its PITCH was being driven from the hand. Its yaw was written once, at
        // entry, and then frozen. So the arm could move the gun horizontally all
        // it liked and the round would not follow: every shot landed on the
        // entry yaw. A vertical line, exactly as reported, and it was a defect
        // rather than a calibration.
        //
        // attackangles takes BOTH axes from the composed hand pose through the
        // ordinary path below, so the round and the reticle follow the gun in
        // both. The reticle walking off centre as the arm drifts is not a cost
        // to avoid -- it is the reticle correctly showing where the gun is
        // pointing.
        float wanted[3]{};
        if (!TargetAngles(engineView, wanted)) {
            if (g_refused.fetch_add(1, std::memory_order_relaxed) % 240 == 0) {
                Tf2VrLog("[TF2VR] aimcmd: the aim source has no pose yet; wrote nothing this "
                         "tick. (aim.cmd_source = 1 needs the controller composition running.)\n");
            }
            if (viewDirty) CommitCommand(cmd, verified);
            return;
        }
        // IN ADS THE ROUND GOES WHERE THE RENDERED VIEW LOOKS. The follow block
        // above steers the view onto the hand and the pin draws the gun at the
        // view angles; the round takes the same angles so all three agree.
        if (g_adsGunIsView && AdsLockEngaged()) {
            wanted[0] = g_adsViewAngles[0];
            wanted[1] = g_adsViewAngles[1];
        }

        // THE BODY PITCH WE WRITE, WHENEVER IT STEPS.
        //
        // The wearer found a reproducible trigger: crouch, stand, then ADS, and
        // the gun draws way low. The census located the signature -- the
        // engine's BODY pitch freezes at a large downward angle (+20.05 for
        // eight seconds, +41.34 later) while the head-driven applied pitch and
        // the base YAW both keep moving and the latch generation keeps climbing.
        // So the latch is live and the body's pitch is a held value, and in ADS
        // ads.lock hands the gun back to the engine, which draws it relative to
        // that body. A body 20-41 degrees down draws the gun way low.
        //
        // ads.face_aim_pitch is already exonerated: its own log shows +2.9 in
        // and 0.0 out on every press. This is the other writer, and it has never
        // been watched. Logged only when the written pitch STEPS by more than
        // two degrees, with the crouch state beside it, so ordinary aiming is
        // silent and the crouch transition is not.
        {
            static float previousWritten = 0.0f;
            static std::uint32_t pitchLines = 0;
            if (std::fabs(wanted[0] - previousWritten) > 2.0f && pitchLines < 200) {
                ++pitchLines;
                char line[400]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] AIMPITCH#%u: writing body pitch %+.2f (was %+.2f, step %+.2f) into %s "
                    "| engine view pitch %+.2f | camera z %.2f (%s) | ads aligned=%d. A large "
                    "positive pitch left standing is the gun drawn low in ADS.\n",
                    pitchLines, static_cast<double>(wanted[0]),
                    static_cast<double>(previousWritten),
                    static_cast<double>(wanted[0] - previousWritten),
                    state == static_cast<int>(AimCmdState::WriteAttack) ? "attackangles"
                                                                       : "worldViewAngles",
                    static_cast<double>(engineView[0]),
                    static_cast<double>(GameCameraHeightUnits()),
                    GameCameraHeightUnits() < 85.0f ? "CROUCHED" : "standing",
                    g_adsAligned ? 1 : 0);
                Tf2VrLog(line);
            }
            previousWritten = wanted[0];
        }
        float* target = (state == static_cast<int>(AimCmdState::WriteAttack)) ? attack : view;
        target[0] = wanted[0];
        target[1] = wanted[1];
        target[2] = wanted[2];

        CommitCommand(cmd, verified);

        // Read the value back out of the VERIFIED COPY, not out of the command.
        // The command obviously holds what we just wrote; the verified copy is
        // the one the engine will restore from, so it is the only read-back
        // that means anything. Divergence here is the drive being reverted.
        const std::size_t offset =
            (state == static_cast<int>(AimCmdState::WriteAttack)) ? kCmdAttackAngles
                                                                  : kCmdViewAngles;
        auto* copied = reinterpret_cast<const float*>(verified + offset);
        const float readback[3] = {copied[0], copied[1], copied[2]};
        for (int i = 0; i < 3; ++i) {
            g_lastWritten[i] = wanted[i];
            g_lastVerifiedReadback[i] = readback[i];
        }
        bool mismatch = false;
        for (int i = 0; i < 3; ++i) {
            if (std::fabs(readback[i] - wanted[i]) > 0.001f) mismatch = true;
        }
        if (mismatch) g_repairMismatch.fetch_add(1, std::memory_order_relaxed);
        g_writes.fetch_add(1, std::memory_order_relaxed);

        if (verbose || mismatch) {
            LogLine(mismatch ? "WRITE **REVERTED**" : "WRITE", sequence, engineView, engineAttack,
                    wanted, readback, buttons);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faulted.fetch_add(1, std::memory_order_relaxed);
        g_aimCmdArmed = 0;
        g_state.store(static_cast<int>(AimCmdState::Off), std::memory_order_release);
        Tf2VrLog("[TF2VR] aimcmd: faulted inside the post-process; DISARMED. The wrapper stays "
                 "installed and is now a pass-through.\n");
    }
}

namespace {

bool Install() {
    if (g_installed.load(std::memory_order_acquire)) return true;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) {
        Tf2VrLog("[TF2VR] aimcmd: client.dll is not loaded yet; nothing patched.\n");
        return false;
    }
    g_client = reinterpret_cast<std::uint8_t*>(client);

    // All four byte checks before ANY of them is used. A partial install --
    // seam patched, helpers unverified -- would arm a drive that cannot repair
    // what it breaks, which is worse than no drive.
    if (!CheckBytes("CInput::CreateMove", kCreateMoveRva, kExpectedCreateMove, kDisplaced)) return false;
    if (!CheckBytes("the command hash", kCmdHashRva, kExpectedHash, sizeof(kExpectedHash))) return false;
    if (!CheckBytes("CUserCmd::operator=", kCmdCopyRva, kExpectedCopy, sizeof(kExpectedCopy))) return false;
    if (!CheckBytes("CUserCmd::GetChecksum", kCmdChecksumRva, kExpectedChecksum,
                    sizeof(kExpectedChecksum))) return false;

    g_cmdHash = reinterpret_cast<CmdHashFn>(g_client + kCmdHashRva);
    g_cmdCopy = reinterpret_cast<CmdCopyFn>(g_client + kCmdCopyRva);
    g_cmdChecksum = reinterpret_cast<CmdChecksumFn>(g_client + kCmdChecksumRva);

    auto* site = g_client + kCreateMoveRva;
    DWORD oldProtect = 0;
    if (!VirtualProtect(site, kDisplaced, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Tf2VrLog("[TF2VR] aimcmd: VirtualProtect failed; nothing patched.\n");
        return false;
    }

    // jmp qword ptr [rip+0], then the absolute target immediately after.
    // Built from a ZEROED array: bytes 2..5 are the displacement and MUST be
    // zero. sway_probe.cpp records what happens otherwise -- a memset of 0x90
    // first left disp32 = 0x90909090 and the first call took the process down.
    std::uint8_t detour[kDisplaced]{};
    detour[0] = 0xFF;
    detour[1] = 0x25;
    const auto target = reinterpret_cast<std::uintptr_t>(aimCreateMoveInterceptor);
    std::memcpy(detour + 6, &target, sizeof(target));

    std::uint32_t displacement = 0;
    std::memcpy(&displacement, detour + 2, sizeof(displacement));
    std::uintptr_t encoded = 0;
    std::memcpy(&encoded, detour + 6, sizeof(encoded));
    if (detour[0] != 0xFF || detour[1] != 0x25 || displacement != 0 || encoded != target) {
        char line[360]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] aimcmd: refusing to patch -- the constructed detour is malformed (op %02X "
            "%02X, disp32 0x%08X, target 0x%llX). Nothing was written.\n",
            detour[0], detour[1], displacement, static_cast<unsigned long long>(encoded));
        Tf2VrLog(line);
        DWORD ignoredProtect = 0;
        VirtualProtect(site, kDisplaced, oldProtect, &ignoredProtect);
        return false;
    }

    auto* trampoline = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        Tf2VrLog("[TF2VR] aimcmd: could not allocate a trampoline; nothing patched.\n");
        DWORD ignoredProtect = 0;
        VirtualProtect(site, kDisplaced, oldProtect, &ignoredProtect);
        return false;
    }
    std::memcpy(trampoline, site, kDisplaced);
    std::uint8_t* jump = trampoline + kDisplaced;
    jump[0] = 0xFF;
    jump[1] = 0x25;
    std::memset(jump + 2, 0, 4);
    const auto resume = reinterpret_cast<std::uintptr_t>(site + kDisplaced);
    std::memcpy(jump + 6, &resume, sizeof(resume));
    FlushInstructionCache(GetCurrentProcess(), trampoline, 64);
    g_aimCmdTrampoline = reinterpret_cast<std::uintptr_t>(trampoline);
    RegisterHookSite("aimcmd CreateMove trampoline", trampoline, 64);
    RegisterHookSite("aimcmd CreateMove patched entry", site, kDisplaced);

    std::memcpy(site, detour, kDisplaced);
    FlushInstructionCache(GetCurrentProcess(), site, kDisplaced);
    DWORD ignored = 0;
    VirtualProtect(site, kDisplaced, oldProtect, &ignored);
    g_installed.store(true, std::memory_order_release);

    char line[400]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] aimcmd: wrapped CInput::CreateMove at client.dll+0x%llX, %zu bytes displaced, "
        "trampoline at %p resuming at client.dll+0x%llX. Pass-through until armed.\n",
        static_cast<unsigned long long>(kCreateMoveRva), kDisplaced,
        static_cast<void*>(trampoline),
        static_cast<unsigned long long>(kCreateMoveRva + kDisplaced));
    Tf2VrLog(line);
    return true;
}

void EnterState(int state) {
    g_state.store(state, std::memory_order_release);
    g_aimCmdArmed = (state == static_cast<int>(AimCmdState::Off)) ? 0 : 1;
    g_writes.store(0, std::memory_order_relaxed);
    g_reads.store(0, std::memory_order_relaxed);
    g_refused.store(0, std::memory_order_relaxed);
    g_repairMismatch.store(0, std::memory_order_relaxed);
    // EVERY PRESS MUST PRODUCE SOMETHING VISIBLE IMMEDIATELY. A control whose
    // effect only shows up in a summary a second later is indistinguishable
    // from a dead key, and this project has already lost runs to that.
    g_verboseLeft.store(20, std::memory_order_relaxed);
    g_lastLogTick = 0;

    char line[420]{};
    if (state == static_cast<int>(AimCmdState::Off)) {
        std::snprintf(line, sizeof(line),
            "[TF2VR] aimcmd: ===== %s ===== the command build is untouched again.\n",
            StateName(state));
    } else {
        std::snprintf(line, sizeof(line),
            "[TF2VR] aimcmd: ===== %s ===== source=%s yaw=%.1f deg. The next %d commands are "
            "logged in full, then once a second.\n",
            StateName(state),
            g_source.load(std::memory_order_relaxed) == 0 ? "synthetic offset from the engine's "
                                                            "own view angles"
                                                          : "composed controller aim pose",
            g_yawDegrees.load(std::memory_order_relaxed), 20);
    }
    Tf2VrLog(line);
}

}  // namespace

void CycleAimCmd() {
    if (!Install()) {
        Tf2VrLog("[TF2VR] aimcmd: not installed, so the key did nothing. Nothing is patched.\n");
        return;
    }
    // THE KEY CYCLE SKIPS WriteView, DELIBERATELY: Off -> Log -> WriteAttack -> Off.
    //
    // FR2 measured what a worldViewAngles write does. It COMPOUNDS: the engine
    // reads our written value back into its own view state, so +30 degrees a
    // tick at 80 ticks a second span the whole circle in under two seconds.
    // At a desk that is a clean positive control and it did its job. In a
    // headset it is a 2400 deg/s spin strapped to someone's face, and it would
    // have been ONE keypress away from the state H1 runs in.
    //
    // Candidate A is live and B is not needed, so B leaves the cycle rather
    // than staying as a hazard. It is still reachable deliberately with
    // `set aim.cmd = 3` in the ini for anyone re-testing it at a desk.
    int state = g_state.load(std::memory_order_relaxed);
    int next;
    switch (static_cast<AimCmdState>(state)) {
        case AimCmdState::Off: next = static_cast<int>(AimCmdState::Log); break;
        case AimCmdState::Log: next = static_cast<int>(AimCmdState::WriteAttack); break;
        default: next = static_cast<int>(AimCmdState::Off); break;
    }
    EnterState(next);
}

AimCmdState CurrentAimCmdState() {
    return static_cast<AimCmdState>(g_state.load(std::memory_order_relaxed));
}

void SetAimCmdState(int state) {
    if (state < 0 || state > 3) return;
    g_wantedState.store(state, std::memory_order_release);
    if (state == static_cast<int>(AimCmdState::Off)) {
        if (g_installed.load(std::memory_order_acquire)) {
            EnterState(state);
        } else {
            // Counted, not silent: an ini that says 0 before client.dll exists
            // leaves the wrapper uninstalled for the whole run, and the log has
            // to say so or a run with the aim feed off reads like one with it on.
            Tf2VrLog("[TF2VR] aimcmd: OFF from the ini before client.dll is loaded; the wrapper "
                     "stays uninstalled and nothing writes the command this run.\n");
        }
        return;
    }
    if (!Install()) {
        // NOT a refusal. The ini is applied at plugin load, before client.dll
        // is in the process, so the first attempt can only fail -- and treating
        // that as final is exactly how vrinput spent a whole session doing
        // nothing. AdvanceAimCmd retries.
        Tf2VrLog("[TF2VR] aimcmd: wanted from the ini, but client.dll is not loaded yet. It will "
                 "arm itself as soon as it is.\n");
        return;
    }
    EnterState(state);
}

void SetAimCmdSource(int source) { g_source.store(source != 0 ? 1 : 0, std::memory_order_release); }

void SetAimCmdYawDegrees(float degrees) { g_yawDegrees.store(degrees, std::memory_order_release); }


void AdvanceAimCmd() {
    // Arm late, for the reason SetAimCmdState explains.
    const int wanted = g_wantedState.load(std::memory_order_relaxed);
    if (wanted != static_cast<int>(AimCmdState::Off) &&
        g_state.load(std::memory_order_relaxed) == static_cast<int>(AimCmdState::Off) &&
        !g_installed.load(std::memory_order_acquire)) {
        static std::uint64_t lastTry = 0;
        const std::uint64_t now = GetTickCount64();
        if (now - lastTry >= 500) {
            lastTry = now;
            if (Install()) EnterState(wanted);
        }
    }

    if (g_state.load(std::memory_order_relaxed) == static_cast<int>(AimCmdState::Off)) return;

    const std::uint64_t tick = GetTickCount64();
    if (tick - g_lastLogTick < 1000) return;
    g_lastLogTick = tick;

    // THE SUMMARY IS THE FALSIFIER, AND IT IS BUILT SO A NULL CANNOT BE READ AS
    // A REFUTATION.
    //
    // hits counts every call through the wrapper; reads counts the commands we
    // actually resolved; writes counts the ones we changed. hits climbing with
    // reads at zero is bad ring arithmetic, not a dead field. reads climbing
    // with writes at zero is a source with no pose. Everything at zero is a
    // seam that is not on the live path -- which is a reason to re-find the
    // seam, never a reason to conclude the field does nothing.
    char line[560]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] aimcmd %s | hits=%llu reads=%llu writes=%llu refused=%llu reverted=%llu "
        "faults=%llu | engine view <%.2f,%.2f,%.2f> attack <%.2f,%.2f,%.2f> | last wrote "
        "<%.2f,%.2f,%.2f> verified copy <%.2f,%.2f,%.2f> | buttons=0x%08X\n",
        StateName(g_state.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_aimCmdHits),
        static_cast<unsigned long long>(g_reads.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_writes.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_refused.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_repairMismatch.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_faulted.load(std::memory_order_relaxed)),
        g_lastEngineView[0], g_lastEngineView[1], g_lastEngineView[2], g_lastEngineAttack[0],
        g_lastEngineAttack[1], g_lastEngineAttack[2], g_lastWritten[0], g_lastWritten[1],
        g_lastWritten[2], g_lastVerifiedReadback[0], g_lastVerifiedReadback[1],
        g_lastVerifiedReadback[2], static_cast<unsigned>(g_lastButtons));
    Tf2VrLog(line);
}

bool TryGetAimOffsetDegrees(float* yawOffset, float* pitchOffset) {
    if (g_reads.load(std::memory_order_relaxed) == 0) return false;
    // What the aim actually IS this tick: the value we wrote when armed, and
    // the engine's own attack angles when not. Reading g_lastWritten
    // unconditionally would report a stale pulse after disarming.
    const bool driving = g_state.load(std::memory_order_relaxed) ==
                             static_cast<int>(AimCmdState::WriteAttack) &&
                         g_writes.load(std::memory_order_relaxed) > 0;
    const float aimPitch = driving ? g_lastWritten[0] : g_lastEngineAttack[0];
    const float aimYaw = driving ? g_lastWritten[1] : g_lastEngineAttack[1];
    float dYaw = aimYaw - g_lastEngineView[1];
    // Wrap to [-180,180]. Aim and view straddle the +/-180 seam constantly --
    // the H1 log has view yaw at 166.81 on one line and -158.86 a moment later
    // -- and an unwrapped difference there is 325 degrees instead of 35, which
    // would throw the reticle right off the screen exactly when the wearer
    // turns past south.
    while (dYaw > 180.0f) dYaw -= 360.0f;
    while (dYaw < -180.0f) dYaw += 360.0f;
    float dPitch = aimPitch - g_lastEngineView[0];
    while (dPitch > 180.0f) dPitch -= 360.0f;
    while (dPitch < -180.0f) dPitch += 360.0f;
    if (yawOffset) *yawOffset = dYaw;
    if (pitchOffset) *pitchOffset = dPitch;
    return true;
}

bool TryGetAimAnglesDegrees(float angles[3]) {
    if (g_reads.load(std::memory_order_relaxed) == 0) return false;
    const bool driving = g_state.load(std::memory_order_relaxed) ==
                             static_cast<int>(AimCmdState::WriteAttack) &&
                         g_writes.load(std::memory_order_relaxed) > 0;
    for (int i = 0; i < 3; ++i) {
        angles[i] = driving ? g_lastWritten[i] : g_lastEngineAttack[i];
    }
    return true;
}

void RequestSnapYaw(float degrees) {
    if (degrees == 0.0f) return;
    // Replaces rather than accumulates. A second flick before the first has been
    // consumed should turn once, not twice -- the command hook runs at the
    // engine's tick rate and a queue would let a fast double-flick spin.
    g_pendingSnapYaw.store(degrees, std::memory_order_release);
}

bool SnapTurnAvailable() {
    // The wrapper has to be INSTALLED for the write to land. It is installed
    // independently of the aim.cmd state -- Off makes it a pass-through, not
    // absent -- but if it never installed, a snap request would vanish silently
    // and the wearer would report "snap turn does nothing".
    return g_aimCmdArmed != 0;
}

unsigned long long SnapTurnWrites() { return g_snapWrites.load(std::memory_order_relaxed); }

// ads.face_aim. See the note at the ADS alignment for why yaw can follow the
// gun and pitch cannot.
void SetAdsFaceAim(bool faceAim) {
    const bool was = g_adsFaceAim.exchange(faceAim, std::memory_order_release);
    if (was == faceAim) return;
    Tf2VrLog(faceAim
        ? "[TF2VR] ads.face_aim = 1: aiming down sights TURNS YOU to face where the gun was "
          "pointing, in one instant write with no intermediate frames. What it was aimed at ends "
          "up in the middle of the screen. Yaw only -- rendered pitch is the headset's own and has "
          "no base term to write, so the head still covers the vertical.\n"
        : "[TF2VR] ads.face_aim = 0: aiming down sights keeps the view where it is and the gun "
          "adopts it, so the gun shoots at whatever you were looking at.\n");
}

bool IsAdsFaceAim() { return g_adsFaceAim.load(std::memory_order_acquire); }

bool TryGetEngineViewAngles(float angles[3]) {
    if (g_reads.load(std::memory_order_relaxed) == 0) return false;
    for (int i = 0; i < 3; ++i) angles[i] = g_lastEngineView[i];
    return true;
}

void SetAdsFaceAimPitch(bool enabled) {
    const bool was = g_adsFaceAimPitch.exchange(enabled, std::memory_order_release);
    if (was == enabled) return;
    if (!enabled) SetViewPitchOffset(0.0f);
    Tf2VrLog(enabled
        ? "[TF2VR] ads.face_aim_pitch = 1: aiming down sights also tilts the view to the gun's "
          "elevation, so a scope pointed high is in front of you rather than off the top of the "
          "screen. Held only while ADS is held: on release the view returns to where your head "
          "physically is. While it is held, level in the headset is not level in the world -- that "
          "is the cost, and it is why this is separate from the yaw turn.\n"
        : "[TF2VR] ads.face_aim_pitch = 0: the view keeps the head's elevation, and any standing "
          "offset is cleared.\n");
}

bool IsAdsFaceAimPitch() { return g_adsFaceAimPitch.load(std::memory_order_acquire); }

void SetAdsFaceAimReturn(bool enabled) {
    const bool was = g_adsFaceAimReturn.exchange(enabled, std::memory_order_release);
    if (was == enabled) return;
    Tf2VrLog(enabled
        ? "[TF2VR] ads.face_aim_return = 1: releasing ADS turns the body back by the entry turn and clears it from the hand compensation.\n"
        : "[TF2VR] ads.face_aim_return = 0: OLD behaviour -- the face-aim turn and its compensation persist after release and accumulate.\n");
}
bool IsAdsFaceAimReturn() { return g_adsFaceAimReturn.load(std::memory_order_acquire); }

bool TryGetAdsViewAngles(float out[3]) {
    if (!g_adsGunIsView || !AdsLockEngaged()) return false;
    out[0] = g_adsViewAngles[0];
    out[1] = g_adsViewAngles[1];
    out[2] = g_adsViewAngles[2];
    return true;
}
