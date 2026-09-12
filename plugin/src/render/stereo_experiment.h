#pragma once

#include <cstdint>

enum class StereoCaptureAction { None, Baseline, Control, Test };
void RequestStereoSlotExperiment();
StereoCaptureAction ConsumeStereoCaptureAction();
void BeginStereoSlotExperiment();
extern "C" void EndStereoSlotExperiment();
extern "C" volatile std::uint8_t g_stereoExperimentActive;
extern "C" volatile std::uint8_t g_stereoExperimentDone;
extern "C" volatile std::uint64_t g_stereoExperimentWrapperCalls;
extern "C" float g_stereoExperimentBaseX;
extern "C" float g_stereoExperimentAppliedX;
