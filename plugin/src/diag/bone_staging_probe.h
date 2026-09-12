#pragma once

// THE STAGED BONE-TO-WORLD BUFFER, READ AND REPORTED. WRITES NOTHING.
//
// Located by static analysis of client.dll on 2026-08-18, with no game running
// and no headset -- see BONE-ROUTE-OPEN-THREADS-2026-08-18.md §3 for the full
// derivation. The short version:
//
//   "CModelRenderSystem::m_BoneToWorld" (RVA 0x908460) has exactly ONE
//   RIP-relative reference in .text, inside client.dll+0x26A410, which RTTI
//   confirms is CModelRenderSystem::CModelRenderSystem. A static initialiser at
//   client.dll+0x86BCE4 passes it the singleton, so:
//
//       singleton      client.dll + 0x11C2FB0
//       m_BoneToWorld  client.dll + 0x11C2FD0   (singleton + 0x20)
//
//   The pool is a reserve-and-bump linear allocator: VirtualAlloc(MEM_RESERVE)
//   of 1 MiB, 32-byte aligned, committed in 0x8000 chunks. Its base is stored at
//   pool+0x18 and VirtualFree'd from that same field at teardown, which is what
//   confirms the offset rather than one reading of one function.
//
// WHY THIS IS WORTH A PROBE OF ITS OWN. The heap-wide position search was a
// lottery across 4.7 GB: its results differed between two runs of identical code
// in the same scene, and driving one of its hits crashed the process. This
// region is at most 1 MiB, it is by NAME the bone-to-world staging area, and
// every hit inside it is a bone matrix by construction rather than by
// silhouette. Walking it exhaustively costs about 65000 predicate evaluations.
//
// The probe answers three questions and writes nothing:
//   1. Is the pool live -- base non-null, bump pointer inside the reservation?
//   2. How many bone-shaped runs are in [base, current), and where?
//   3. Do the KNOWN arrays' matrices appear inside it? A byte-identical match
//      proves this region stages the very arrays we have been driving, which is
//      what would make it the buffer the gun's renderer actually reads.
//
// Read-only and SEH-guarded throughout. Nothing here can retire a target, move
// a bone, or take the game down.
void ProbeBoneStagingBuffer();

// SAMPLED FROM INSIDE THE FRAME, WHICH IS THE ONLY PLACE IT IS POPULATED.
//
// The first live read of this pool found base == current and highWater == 0 --
// empty. The fields were right (alignment 0x20, commit granularity 0x8000 and
// size 0x100000 all matched the constructor arguments exactly), but the sample
// was taken from the arming path, which runs on the plugin frame, outside the
// render window. This is a per-frame bump allocator: filled while the frame is
// drawn, rewound before the next one. Read outside that window it is always
// empty, and an empty read says nothing about what it holds when it matters.
//
// So the sample is now taken at the viewmodel camera upload -- mid-frame,
// immediately before the viewmodel draws, which is exactly when its bones must
// already be staged. It waits for the first frame where the pool is non-empty,
// reports once in full, and stops.
void RequestBoneStagingSample();
void AdvanceBoneStagingSampleAtRender();
// Prints the catalogue and stops sampling. Called when the flat session ends.
void ReportBoneStagingCollection();
