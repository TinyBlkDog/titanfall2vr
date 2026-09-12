#include "stereo_experiment.h"
#include "diagnostics.h"
#include <atomic>
#include <cstdint>
#include <cstdio>

extern "C" volatile std::uintptr_t g_cameraStructAddress;
extern "C" volatile std::uint8_t g_cameraWriteBlock;
extern "C" volatile std::uint8_t g_stereoExperimentActive = 0;
extern "C" volatile std::uint8_t g_stereoExperimentDone = 0;
extern "C" volatile std::uint64_t g_stereoExperimentWrapperCalls = 0;
extern "C" float g_stereoExperimentBaseX = 0.0f;
extern "C" float g_stereoExperimentAppliedX = 0.0f;
namespace { enum class Phase : std::uint8_t { Idle, BaselinePending, ControlPending, Experiment, TestPending }; std::atomic<Phase> g_phase{Phase::Idle}; }
void RequestStereoSlotExperiment() { Phase e=Phase::Idle; if(g_phase.compare_exchange_strong(e,Phase::BaselinePending)) Tf2VrLog("[TF2VR] F5: baseline requested.\n"); }
StereoCaptureAction ConsumeStereoCaptureAction() { Phase e=Phase::BaselinePending; if(g_phase.compare_exchange_strong(e,Phase::ControlPending)) return StereoCaptureAction::Baseline; e=Phase::ControlPending; if(g_phase.compare_exchange_strong(e,Phase::Experiment)) return StereoCaptureAction::Control; if(g_phase.load()==Phase::Experiment && g_stereoExperimentDone) { e=Phase::Experiment; if(g_phase.compare_exchange_strong(e,Phase::TestPending)) return StereoCaptureAction::Test; } return StereoCaptureAction::None; }
void BeginStereoSlotExperiment() {
    auto* c = reinterpret_cast<const float*>(g_cameraStructAddress);
    if (!c) {
        Tf2VrLog("[TF2VR] F5 aborted: no camera pointer.\n");
        g_phase = Phase::Idle;
        return;
    }
    g_stereoExperimentBaseX = c[0];
    g_stereoExperimentAppliedX = c[0];
    g_stereoExperimentWrapperCalls = 0;
    g_stereoExperimentDone = 0;
    g_cameraWriteBlock = 0;
    g_stereoExperimentActive = 1;
    Tf2VrLog("[TF2VR] F5: one native CViewRender slot-1 preparation under +50 X, before the normal engine render bridge.\n");
}
extern "C" void EndStereoSlotExperiment() { g_stereoExperimentActive=0;g_cameraWriteBlock=0;g_phase=Phase::Idle;char line[192]{};std::snprintf(line,sizeof(line),"[TF2VR] F5 complete: slot-1 preparation camera X %.3f -> %.3f; primary writer restored.\n",g_stereoExperimentBaseX,g_stereoExperimentAppliedX);Tf2VrLog(line); }
