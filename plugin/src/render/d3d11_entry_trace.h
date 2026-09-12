#pragma once

struct ID3D11Device;

// A bounded function-entry trace for DrawIndexed/Draw. This is deliberately
// separate from the closed context-vtable experiment: it leaves every COM
// object's vptr unchanged and patches only verified d3d11.dll thunks.
void BeginD3D11FunctionEntryTrace(ID3D11Device* device);
void AdvanceD3D11FunctionEntryTrace();
void RemoveD3D11FunctionEntryTrace();
