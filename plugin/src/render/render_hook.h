#pragma once

#include <cstdint>

// Task 3: passive trace of engine.dll+0xDD150 and its vtable +0xB8 dispatch.
void EnsureRenderHookInstalled();
void RemoveRenderHook();
void BeginRenderTrace();
void AdvanceRenderTrace();
// F12 arms one passive, native slot-1/slot-2 output capture.  The engine's
// existing dispatches still execute exactly once; this never calls into the
// render chain or changes camera/render state.
extern "C" void CaptureNativeSlotPreOutput(std::uint32_t slot);
extern "C" void CaptureNativeSlotOutput(std::uint32_t slot);
