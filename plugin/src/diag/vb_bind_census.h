#pragma once

#include <d3d11.h>

// ---------------------------------------------------------------------------
// VERTEX-BUFFER BIND CENSUS -- read-only (2026-09-05, late).
//
// Three materialsystem submit routines have been hooked (render-context slots
// 170, 171, 172) and none carried the arms: every mesh through them is an
// unskinned stride of 16..28 bytes. The arms are skinned and go through a
// routine not yet named. The one call no draw can avoid is the D3D bind:
// ID3D11DeviceContext::IASetVertexBuffers (context-table slot 18). This swaps
// that slot on the verified game context, the same way and with the same
// checks as the RTV census (slots 33/34), and for every buffer bound asks it
// for its ByteWidth once: a buffer whose ByteWidth / stride equals one of the
// arms model's five vertex counts IS that mesh, and the return address of the
// bind names the routine that draws it, as module+RVA. Nothing is changed.
// ---------------------------------------------------------------------------

void InstallVbBindCensus(ID3D11DeviceContext* context, void** vtable);
void AdvanceVbBindCensus();

// Prints the plugin's recent writes into GAME memory (tick, thread, address,
// old -> new). Called from the crash recorder on an already-faulted thread, so
// it reads only its own small ring and takes no lock. A write microseconds
// before a fault, on another thread, is a race; the last one being minutes old
// exonerates the plugin.
void ReportGameWritesForCrash();
