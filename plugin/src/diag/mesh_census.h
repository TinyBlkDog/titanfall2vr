#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// MESH CENSUS -- read-only, the hands-by-mesh front (2026-09-05).
//
// Two instruments, both offline-proven before any run:
//
// 1. DumpMeshCensusOnce(studiohdr): the v53 studiohdr's texture table, cd
//    texture paths, skin table and the bodypart -> model -> mesh walk, so the
//    log says which mesh carries which material. Every offset is quoted from
//    the disassembly in the .cpp, none is guessed. Called once per studiohdr
//    from the arms SetupBones post-process (the bone-table dump already has
//    the pointer).
//
// 2. The GATE OBSERVER. studiorender.dll skips a mesh when its material's
//    vtable slot 71 (CMaterial::ShouldDraw, materialsystem_dx11.dll+0x49910,
//    "!GetMaterialVarFlag(0x8000000)") returns false. The observer swaps that
//    one CMaterial vtable slot for a wrapper that calls the original, counts
//    the verdict per material and reads each new material's name once. It
//    changes no verdict. Positive control: the gun's and the world's materials
//    appear; the arms materials from instrument 1 appear among them.
// ---------------------------------------------------------------------------

void DumpMeshCensusOnce(const std::uint8_t* studio);

// The arms model's per-mesh index counts from its embedded VTX, in mesh order
// (0 gear, 1 gauntlet, 2 gauntlet_jack, 3 lowerbody, 4 upperbody), valid when
// the vertex-count control passed. The draw items carry the index count at
// +0x04; this is how an item is recognised as one of the arms' meshes.
extern int g_armsMeshIndexCount[16];
extern int g_armsMeshVertexCount[16];
extern int g_armsMeshCount;
extern bool g_armsMeshControlPass;
extern int g_armsMeshIsGlove[16];   // 1 = the material name contains "gauntlet"
extern bool g_armsHeaderValid;      // the studiohdr walk resolved every mesh

// The mesh records' own meshid (mstudiomesh_t +0x20) in the same order, and the
// arms studiohdr itself. studiorender.dll+0xDE10 gates each mesh on
// [[ctx+0x28] + meshid*16] != 0, so meshid -- not mesh order -- is the index
// into that gate array, and the studiohdr is how a draw is recognised as this
// model's.
extern int g_armsMeshId[16];
extern const void* g_armsStudioHdr;

// EVERY arms model, not just the first walked. There are two pilot arms models
// -- pov_mlt_hero_jack_rifleman.mdl and pov_mlt_hero_jack.mdl -- and the second
// draws after a titan exit. Mesh IDs are per model, so a filter must look up
// the model it is actually drawing rather than reuse another one.s numbers.
struct ArmsModelMeshes {
    char name[64];
    int meshCount;
    int meshId[16];
    int vertexCount[16];
    int isGlove[16];
    bool valid;
};
constexpr int kMaxArmsModels = 4;
extern ArmsModelMeshes g_armsModels[kMaxArmsModels];
extern int g_armsModelCount;

// Installs the gate observer once the world renders; reports once a second.
void AdvanceMeshCensus();

// Restores the vtable slot. Safe when nothing is installed.
void RemoveMeshCensus();
