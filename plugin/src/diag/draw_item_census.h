#pragma once

// ---------------------------------------------------------------------------
// DRAW ITEM CENSUS -- read-only, the hands-by-mesh front (2026-09-05).
//
// THE LIVE DRAW PATH, from the two bone-reader runs: the client copies the
// arms bones into frame scratch, a render-thread job uploads them, and
// materialsystem_dx11.dll+0x1D800 (reached only through render-context vtable
// slots 170/171, +0x1D59E8/+0x1D59F0 -> +0x72AC0/+0x72B60) submits a list of
// 0x60-byte DRAW ITEMS, batching consecutive items by three identity fields
// ([item+0x18], [[item+0x30]+0x10], [item+0x38]) and reading per-item
// transform (+0x10), colour (+0x40..) and two 16-bit indices (+0x58, +0x5A).
// One item is one mesh draw of one instance. Whether the material and the
// per-instance key sit where the layout suggests is what this census
// measures: for every item it tests the candidate fields for a pointer whose
// vptr is the CMaterial vtable, names the material once, and keeps a few
// samples of the other fields for the arms materials. The verdicts are
// untouched: the wrapper calls the original with the same arguments.
// ---------------------------------------------------------------------------

void AdvanceDrawItemCensus();
void RemoveDrawItemCensus();
