#pragma once

struct ID3D11Device;

// Passive Draw/DrawIndexed counters, filtered to the verified game immediate
// context and only while the F12 native-mode measurement marks a mode active.
void RemoveD3D11DrawTraceHooks();
void BeginNativeModeDrawTrace(unsigned int mode);
void EndNativeModeDrawTrace(unsigned int mode);
// Read-only function-entry inspection for the distinct inline-detour avenue.
// It currently reports VSSetConstantBuffers, DrawIndexed, and Draw.  It
// neither changes a vtable nor patches/intercepts a function.
void ProbeD3D11DrawFunctionEntries(ID3D11Device* device);
