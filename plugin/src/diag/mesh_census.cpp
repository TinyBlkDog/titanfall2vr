#include "mesh_census.h"

#include "diagnostics.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "arms_collapse.h"

// The model handle SetupBones was handed (rdx), published by arms_collapse.cpp.
extern "C" std::uint64_t g_armsBonesModel;

namespace {

// ---------------------------------------------------------------------------
// THE v53 STUDIOHDR, EVERY OFFSET FROM THE DISASSEMBLY (2026-09-05).
//
// The header is stock v48 shifted by +4 from +0x0C on (an sznameindex was
// inserted before the name), which is why numbones sits at +0xA0 and not
// +0x9C. Each pair below is quoted from a reader in the shipped binaries;
// none is inferred from the shift alone.
//
//   engine.dll+0x1AFE20 (model-load material walk, offsets +0x524..+0x6E4):
//     mov  ecx,[r12+0xE0]            numskinref
//     movsxd rax,[r12+0xE8]          skinindex      -> pskinref = hdr+skinindex+skin*numskinref*2
//     cmp  dword [r12+0xEC],0        numbodyparts
//     movsxd rax,[r12+0xF0]          bodypartindex, bodypart stride 0x10 (add rcx,0x10)
//     cmp  dword [rax+0x4],0         bodypart.nummodels;  movsxd rsi,[rax+0xC] bodypart.modelindex
//     cmp  dword [rsi+0x48],0        model.nummeshes;     movsxd rax,[rsi+0x4C] model.meshindex
//     add  rdx,0x94                  model stride;        add r15,0x74  mesh stride
//     movsxd rax,[rax+rsi]           mesh.material (+0)
//     movsx  rax,word [r8+rax*2]     pskinref[material]
//     movsxd r8,[r12+0xD4]           textureindex;  imul rax,rax,0x2C  texture record stride
//     movsxd rdx,[r8]; add rdx,r8    texture.sznameindex (+0) -> the name
//   studiorender.dll+0x164F0 (material loader): the same +0xD4 / 0x2C / +0
//     walk, then movsxd rax,[rbx+0xDC]; movsxd rcx,[rax+rbx] -> cdtexture
//     path (+0xDC cdtextureindex, an int table of string offsets), joined
//     with the texture name into the material path.
//   studiorender.dll+0x167D1: movsxd r8,[rdx+0xD0] as the texture count;
//     +0x168CF: movsxd r14,[r15+0xE4] numskinfamilies.
//   studiorender.dll+0x75B5 (draw loop): movsxd rcx,[r15+0x20] mesh.meshid,
//     the index into the hardware mesh data (16 bytes each).
//   client.dll+0x0A3620: cmp r8d,[rdx+0xEC]; movsxd rax,[rdx+0xF0]; shl 4;
//     mov r8d,[rcx+0x4]; idiv [rcx+0x8] -- the bodygroup accessor, same shape.
//
// The header's own name: +0x0C printed "..." in the 2026-09-04 run (it is the
// v53 sznameindex, an int); the char[64] name is at +0x10 and is controlled
// below by having to start with "models".
// ---------------------------------------------------------------------------
constexpr std::size_t kHdrName = 0x10;
constexpr std::size_t kHdrNumTextures = 0xD0;
constexpr std::size_t kHdrTextureIndex = 0xD4;
constexpr std::size_t kHdrNumCdTextures = 0xD8;
constexpr std::size_t kHdrCdTextureIndex = 0xDC;
constexpr std::size_t kHdrNumSkinRef = 0xE0;
constexpr std::size_t kHdrNumSkinFamilies = 0xE4;
constexpr std::size_t kHdrSkinIndex = 0xE8;
constexpr std::size_t kHdrNumBodyParts = 0xEC;
constexpr std::size_t kHdrBodyPartIndex = 0xF0;
constexpr std::size_t kHdrBytesNeeded = 0xF4;
constexpr std::size_t kTextureStride = 0x2C;
constexpr std::size_t kBodyPartStride = 0x10;
constexpr std::size_t kModelStride = 0x94;
constexpr std::size_t kMeshStride = 0x74;
constexpr std::size_t kModelNumMeshes = 0x48;
constexpr std::size_t kModelMeshIndex = 0x4C;
constexpr std::size_t kMeshMaterial = 0x00;
constexpr std::size_t kMeshNumVertices = 0x08;  // stock v48 field, printed as a hint only
constexpr std::size_t kMeshId = 0x20;

// Caps so a corrupt header cannot walk forever. The pilot arms have 74 bones
// and a handful of meshes; these are generous.
constexpr int kMaxTextures = 64;
constexpr int kMaxCdTextures = 8;
constexpr int kMaxSkinRef = 64;
constexpr int kMaxSkinFamilies = 8;
constexpr int kMaxBodyParts = 16;
constexpr int kMaxModels = 16;
constexpr int kMaxMeshes = 64;
constexpr int kMaxDumpedHdrs = 8;

std::uint64_t g_dumpedHdrs[kMaxDumpedHdrs]{};
int g_dumpedCount = 0;

std::size_t BytesFrom(const void* p) {
    if (!p) return 0;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    const DWORD prot = mbi.Protect & 0xFF;
    const bool readable = prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_READONLY ||
                          prot == PAGE_EXECUTE_READ || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_WRITECOPY;
    if (!readable) return 0;
    return reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize - reinterpret_cast<std::uintptr_t>(p);
}

// Copies a bounded, printable version of a C string. Returns the length; 0
// means nothing readable there.
std::size_t ReadName(const char* name, char* out, std::size_t cap) {
    out[0] = '\0';
    if (!name || cap < 2) return 0;
    std::size_t i = 0;
    for (; i + 1 < cap; ++i) {
        if (BytesFrom(name + i) < 1) break;
        const char c = name[i];
        if (c == '\0') break;
        out[i] = (c >= 0x20 && c < 0x7F) ? c : '.';
    }
    out[i] = '\0';
    return i;
}

bool StartsWith(const char* s, const char* prefix) {
    for (; *prefix; ++s, ++prefix) {
        const char a = (*s >= 'A' && *s <= 'Z') ? static_cast<char>(*s + 32) : *s;
        if (a != *prefix) return false;
    }
    return true;
}

int ReadInt(const std::uint8_t* p) { return *reinterpret_cast<const int*>(p); }

// The whole dump under one SEH frame; a fault is logged and ends the dump.
// ---------------------------------------------------------------------------
// THE EMBEDDED VTX (2026-09-05, draw-item dump runs 2-3). The live renderer
// submits one call per hardware mesh with N instances, and the item's +0x04
// is the mesh's index count. The v53 rmdl embeds the OptimizedModel lump
// (studiorender.dll+0x167B0 walked it: bodypart offset at +0x20, 8-byte
// bodyparts and models, 12-byte lods, 9-byte meshes {numstripgroups,
// stripgroupoffset, flags}, 0x21-byte strip groups with flags at +0x18). Its
// header is self-verifying: FileHeader_t {version 7, vertCacheSize,
// maxBonesPerStrip:16, maxBonesPerTri:16, maxBonesPerVert, checksum, numLODs,
// materialReplacementListOffset, numBodyParts, bodyPartOffset}, and the
// checksum at +0x10 must equal the studiohdr's at +0x8. The control that the
// walk is right: the per-mesh vertex sums must equal the studiohdr's own
// mesh vertex counts. Stock strip-group fields numVerts +0, numIndices +8.
// ---------------------------------------------------------------------------
constexpr std::size_t kHdrChecksum = 0x08;
constexpr std::size_t kHdrLength = 0x50;  // v48 +0x4C shifted +4
constexpr int kVtxMaxMeshes = 16;
constexpr std::size_t kStripGroupStride = 0x21;

}  // namespace

int g_armsMeshIndexCount[16]{};
int g_armsMeshVertexCount[16]{};
int g_armsMeshIsGlove[16]{};
int g_armsMeshCount = 0;
bool g_armsHeaderValid = false;
int g_armsMeshId[16]{};
const void* g_armsStudioHdr = nullptr;
ArmsModelMeshes g_armsModels[kMaxArmsModels]{};
int g_armsModelCount = 0;
bool g_armsMeshControlPass = false;

namespace {

void WalkEmbeddedVtx(const std::uint8_t* studio, const int* meshVerts, int meshCount) {
    char line[400];
    const int checksum = ReadInt(studio + kHdrChecksum);
    const int length = ReadInt(studio + kHdrLength);
    const std::size_t avail = BytesFrom(studio);
    std::size_t span = (length > 0x100 && length < (64 << 20)) ? static_cast<std::size_t>(length) : 0;
    if (span == 0 || span > avail) span = avail;
    const std::uint8_t* vtx = nullptr;
    for (std::size_t off = 0x100; off + 0x24 <= span; off += 4) {
        const std::uint8_t* p = studio + off;
        if (ReadInt(p) != 7) continue;
        if (ReadInt(p + 0x10) != checksum) continue;
        const int numLods = ReadInt(p + 0x14);
        const int numBodyParts = ReadInt(p + 0x1C);
        const int bodyPartOffset = ReadInt(p + 0x20);
        if (numLods < 1 || numLods > 8 || numBodyParts < 1 || numBodyParts > 8 || bodyPartOffset < 0x24 ||
            bodyPartOffset > 0x1000)
            continue;
        vtx = p;
        std::snprintf(line, sizeof(line),
                      "[TF2VR] VTX: header at studiohdr+0x%zX (checksum match %08X, numLODs %d, numBodyParts %d, bodyPartOffset "
                      "0x%X, length field %d, span 0x%zX).\n",
                      off, checksum, numLods, numBodyParts, bodyPartOffset, length, span);
        Tf2VrLog(line);
        break;
    }
    if (!vtx) {
        std::snprintf(line, sizeof(line), "[TF2VR] VTX: no OptimizedModel header with checksum %08X within 0x%zX bytes of the "
                      "studiohdr; trying header offsets and pointers.\n", checksum, span);
        Tf2VrLog(line);
        // (a) every dword of the header as an offset from the studiohdr; (b) every qword of the
        // header, and of the model handle SetupBones was handed, as a pointer.
        for (std::size_t off = 0; off + 4 <= 0x400 && !vtx; off += 4) {
            const int o = ReadInt(studio + off);
            if (o <= 0x100 || o > (256 << 20)) continue;
            const std::uint8_t* p = studio + o;
            if (BytesFrom(p) < 0x24) continue;
            if (ReadInt(p) == 7 && ReadInt(p + 0x10) == checksum) {
                vtx = p;
                std::snprintf(line, sizeof(line), "[TF2VR] VTX: header via studiohdr+0x%zX = offset 0x%X -> %p.\n", off, o,
                              reinterpret_cast<const void*>(p));
                Tf2VrLog(line);
            }
        }
        const std::uint8_t* roots[2] = {studio, reinterpret_cast<const std::uint8_t*>(g_armsBonesModel)};
        const std::size_t rootBytes[2] = {0x400, 0x200};
        for (int r = 0; r < 2 && !vtx; ++r) {
            if (!roots[r] || BytesFrom(roots[r]) < rootBytes[r]) continue;
            for (std::size_t off = 0; off + 8 <= rootBytes[r] && !vtx; off += 8) {
                const auto* p = *reinterpret_cast<const std::uint8_t* const*>(roots[r] + off);
                if (!p || (reinterpret_cast<std::uintptr_t>(p) & 3) != 0 || BytesFrom(p) < 0x24) continue;
                if (ReadInt(p) == 7 && ReadInt(p + 0x10) == checksum) {
                    vtx = p;
                    std::snprintf(line, sizeof(line), "[TF2VR] VTX: header via %s+0x%zX -> %p.\n", r == 0 ? "studiohdr" : "model handle",
                                  off, reinterpret_cast<const void*>(p));
                    Tf2VrLog(line);
                }
            }
        }
        if (!vtx) {
            Tf2VrLog("[TF2VR] VTX: not reachable by any header offset or pointer either; index counts unknown.\n");
            return;
        }
    }
    // bodypart 0, model 0, LOD 0.
    const std::uint8_t* bp = vtx + ReadInt(vtx + 0x20);
    if (BytesFrom(bp) < 8) return;
    const std::uint8_t* model = bp + ReadInt(bp + 4);
    if (BytesFrom(model) < 8) return;
    const std::uint8_t* lod = model + ReadInt(model + 4);
    if (BytesFrom(lod) < 12) return;
    const int numMeshes = ReadInt(lod);
    const std::uint8_t* meshes = lod + ReadInt(lod + 4);
    std::snprintf(line, sizeof(line), "[TF2VR] VTX: bodypart0 models %d, model0 lods %d, lod0 meshes %d (studiohdr says %d).\n",
                  ReadInt(bp), ReadInt(model), numMeshes, meshCount);
    Tf2VrLog(line);
    if (numMeshes <= 0 || numMeshes > kVtxMaxMeshes) return;
    int pass = 0;
    int vtxCount = 0;
    for (int k = 0; k < numMeshes; ++k) {
        const std::uint8_t* mesh = meshes + static_cast<std::size_t>(k) * 9;
        if (BytesFrom(mesh) < 9) break;
        const int numGroups = ReadInt(mesh);
        const std::uint8_t* groups = mesh + ReadInt(mesh + 4);
        long long verts = 0, indices = 0;
        for (int g = 0; g < numGroups && g < 64; ++g) {
            const std::uint8_t* sg = groups + static_cast<std::size_t>(g) * kStripGroupStride;
            if (BytesFrom(sg) < kStripGroupStride) break;
            verts += ReadInt(sg + 0);
            indices += ReadInt(sg + 8);
        }
        const int declared = k < meshCount ? meshVerts[k] : -1;
        const bool ok = declared == verts;
        if (ok) ++pass;
        g_armsMeshIndexCount[vtxCount] = static_cast<int>(indices);
        (void)verts;
        ++vtxCount;
        std::snprintf(line, sizeof(line), "[TF2VR] VTX MESH[%d]: %d strip groups, verts %lld (studiohdr %d %s), INDICES %lld\n", k,
                      numGroups, verts, declared, ok ? "match" : "MISMATCH", indices);
        Tf2VrLog(line);
    }
    g_armsMeshControlPass = pass == numMeshes && numMeshes == meshCount;
    std::snprintf(line, sizeof(line), "[TF2VR] VTX: vertex-count control %s (%d/%d meshes match); the index counts above are "
                  "what the draw items carry at +0x04 if the walk is right.\n",
                  g_armsMeshControlPass ? "PASS" : "FAIL", pass, numMeshes);
    Tf2VrLog(line);
}

void DumpMeshCensusGuarded(const std::uint8_t* studio) {
    int meshVerts[16]{};
    int meshVertCount = 0;
    char line[520];
    char name[128];
    if (BytesFrom(studio) < kHdrBytesNeeded) {
        std::snprintf(line, sizeof(line), "[TF2VR] mesh-census: REFUSED -- studiohdr %p readable for fewer than 0x%zX bytes.\n",
                      reinterpret_cast<const void*>(studio), kHdrBytesNeeded);
        Tf2VrLog(line);
        return;
    }
    ReadName(reinterpret_cast<const char*>(studio + kHdrName), name, 64);
    // The arms model is "weapons/arms/pov_....mdl": the header name has no "models/" prefix.
    const bool nameOk = std::strstr(name, ".mdl") != nullptr;
    const int numTextures = ReadInt(studio + kHdrNumTextures);
    const int textureIndex = ReadInt(studio + kHdrTextureIndex);
    const int numCd = ReadInt(studio + kHdrNumCdTextures);
    const int cdIndex = ReadInt(studio + kHdrCdTextureIndex);
    const int numSkinRef = ReadInt(studio + kHdrNumSkinRef);
    const int numSkinFamilies = ReadInt(studio + kHdrNumSkinFamilies);
    const int skinIndex = ReadInt(studio + kHdrSkinIndex);
    const int numBodyParts = ReadInt(studio + kHdrNumBodyParts);
    const int bodyPartIndex = ReadInt(studio + kHdrBodyPartIndex);
    std::snprintf(line, sizeof(line),
                  "[TF2VR] mesh-census: studiohdr %p name+0x10 \"%s\" (%s) numtextures %d textureindex 0x%X "
                  "numcdtextures %d cdtextureindex 0x%X numskinref %d numskinfamilies %d skinindex 0x%X "
                  "numbodyparts %d bodypartindex 0x%X (engine.dll+0x1AFE20 / studiorender.dll+0x164F0).\n",
                  reinterpret_cast<const void*>(studio), name, nameOk ? "name control PASS" : "name control FAIL",
                  numTextures, textureIndex, numCd, cdIndex, numSkinRef, numSkinFamilies, skinIndex, numBodyParts,
                  bodyPartIndex);
    Tf2VrLog(line);
    if (numTextures <= 0 || numTextures > kMaxTextures || textureIndex <= 0 || textureIndex > (16 << 20) ||
        numSkinRef <= 0 || numSkinRef > kMaxSkinRef || numSkinFamilies <= 0 || numSkinFamilies > kMaxSkinFamilies ||
        skinIndex <= 0 || skinIndex > (16 << 20) || numBodyParts <= 0 || numBodyParts > kMaxBodyParts ||
        bodyPartIndex <= 0 || bodyPartIndex > (16 << 20)) {
        Tf2VrLog("[TF2VR] mesh-census: REFUSED -- a count or index is out of range; tables not walked.\n");
        return;
    }

    // cd texture paths: an int table of string offsets, each relative to the header.
    int cdOk = 0;
    if (numCd > 0 && numCd <= kMaxCdTextures && cdIndex > 0) {
        for (int i = 0; i < numCd; ++i) {
            const std::uint8_t* slot = studio + cdIndex + static_cast<std::size_t>(i) * 4;
            if (BytesFrom(slot) < 4) break;
            const int off = ReadInt(slot);
            if (off <= 0) {
                std::snprintf(line, sizeof(line), "[TF2VR] CDTEXTURE[%d] offset %d (not followed)\n", i, off);
                Tf2VrLog(line);
                break;
            }
            if (ReadName(reinterpret_cast<const char*>(studio) + off, name, sizeof(name)) == 0) break;
            std::snprintf(line, sizeof(line), "[TF2VR] CDTEXTURE[%d] \"%s\"\n", i, name);
            Tf2VrLog(line);
            ++cdOk;
        }
    }

    // The texture table. Names must look like material names for the control.
    int texOk = 0;
    int texPathLike = 0;
    for (int i = 0; i < numTextures; ++i) {
        const std::uint8_t* rec = studio + textureIndex + static_cast<std::size_t>(i) * kTextureStride;
        if (BytesFrom(rec) < kTextureStride) {
            std::snprintf(line, sizeof(line), "[TF2VR] mesh-census: STOPPED at texture %d -- record %p unreadable.\n", i,
                          reinterpret_cast<const void*>(rec));
            Tf2VrLog(line);
            break;
        }
        const int nameIndex = ReadInt(rec);
        const int f1 = ReadInt(rec + 4);
        const int f2 = ReadInt(rec + 8);
        const std::uint64_t q10 = *reinterpret_cast<const std::uint64_t*>(rec + 0x10);
        const std::uint64_t q18 = *reinterpret_cast<const std::uint64_t*>(rec + 0x18);
        if (nameIndex == 0 || ReadName(reinterpret_cast<const char*>(rec) + nameIndex, name, sizeof(name)) == 0) {
            std::snprintf(line, sizeof(line), "[TF2VR] TEXTURE[%d] nameindex %d -> unreadable\n", i, nameIndex);
            Tf2VrLog(line);
            continue;
        }
        bool pathLike = false;
        for (const char* p = name; *p; ++p) if (*p == '/' || *p == '\\' || *p == '_') pathLike = true;
        if (pathLike) ++texPathLike;
        ++texOk;
        std::snprintf(line, sizeof(line), "[TF2VR] TEXTURE[%d] \"%s\"  (+4 %d, +8 %d, +0x10 %016llX, +0x18 %016llX)\n", i, name,
                      f1, f2, static_cast<unsigned long long>(q10), static_cast<unsigned long long>(q18));
        Tf2VrLog(line);
    }

    // Skin table: numskinfamilies rows of numskinref shorts. Row 0 is skin 0.
    for (int fam = 0; fam < numSkinFamilies; ++fam) {
        int n = std::snprintf(line, sizeof(line), "[TF2VR] SKIN[%d] pskinref:", fam);
        bool inRange = true;
        for (int i = 0; i < numSkinRef; ++i) {
            const std::uint8_t* p = studio + skinIndex + (static_cast<std::size_t>(fam) * numSkinRef + i) * 2;
            if (BytesFrom(p) < 2) { inRange = false; break; }
            const int v = *reinterpret_cast<const short*>(p);
            if (v < 0 || v >= numTextures) inRange = false;
            if (n < static_cast<int>(sizeof(line)) - 12) n += std::snprintf(line + n, sizeof(line) - n, " %d", v);
        }
        std::snprintf(line + n, sizeof(line) - n, "  (%s)\n", inRange ? "every entry < numtextures" : "OUT OF RANGE");
        Tf2VrLog(line);
    }

    // bodypart -> model -> mesh, with the mesh's material resolved through skin 0.
    int meshesSeen = 0;
    int meshesResolved = 0;
    for (int bp = 0; bp < numBodyParts; ++bp) {
        const std::uint8_t* part = studio + bodyPartIndex + static_cast<std::size_t>(bp) * kBodyPartStride;
        if (BytesFrom(part) < kBodyPartStride) break;
        const int partNameIndex = ReadInt(part);
        const int numModels = ReadInt(part + 4);
        const int base = ReadInt(part + 8);
        const int modelIndex = ReadInt(part + 0xC);
        char partName[64];
        if (partNameIndex == 0 || ReadName(reinterpret_cast<const char*>(part) + partNameIndex, partName, sizeof(partName)) == 0) {
            std::snprintf(partName, sizeof(partName), "?");
        }
        std::snprintf(line, sizeof(line), "[TF2VR] BODYPART[%d] \"%s\" nummodels %d base %d modelindex 0x%X\n", bp, partName,
                      numModels, base, modelIndex);
        Tf2VrLog(line);
        if (numModels <= 0 || numModels > kMaxModels || modelIndex <= 0) continue;
        for (int m = 0; m < numModels; ++m) {
            const std::uint8_t* model = part + modelIndex + static_cast<std::size_t>(m) * kModelStride;
            if (BytesFrom(model) < kModelStride) break;
            ReadName(reinterpret_cast<const char*>(model), name, 64);
            const int numMeshes = ReadInt(model + kModelNumMeshes);
            const int meshIndex = ReadInt(model + kModelMeshIndex);
            std::snprintf(line, sizeof(line), "[TF2VR]   MODEL[%d.%d] \"%s\" nummeshes %d meshindex 0x%X\n", bp, m, name,
                          numMeshes, meshIndex);
            Tf2VrLog(line);
            if (numMeshes <= 0 || numMeshes > kMaxMeshes || meshIndex <= 0) continue;
            for (int k = 0; k < numMeshes; ++k) {
                const std::uint8_t* mesh = model + meshIndex + static_cast<std::size_t>(k) * kMeshStride;
                if (BytesFrom(mesh) < kMeshStride) break;
                ++meshesSeen;
                const int material = ReadInt(mesh + kMeshMaterial);
                const int numVerts = ReadInt(mesh + kMeshNumVertices);
                const int meshId = ReadInt(mesh + kMeshId);
                if (meshVertCount < 16) meshVerts[meshVertCount++] = numVerts;
                int tex = -1;
                name[0] = '\0';
                if (material >= 0 && material < numSkinRef) {
                    const std::uint8_t* ref = studio + skinIndex + static_cast<std::size_t>(material) * 2;
                    if (BytesFrom(ref) >= 2) tex = *reinterpret_cast<const short*>(ref);
                }
                if (tex >= 0 && tex < numTextures) {
                    const std::uint8_t* rec = studio + textureIndex + static_cast<std::size_t>(tex) * kTextureStride;
                    const int nameIndex = BytesFrom(rec) >= 4 ? ReadInt(rec) : 0;
                    if (nameIndex) ReadName(reinterpret_cast<const char*>(rec) + nameIndex, name, sizeof(name));
                    if (name[0]) ++meshesResolved;
                }
                std::snprintf(line, sizeof(line),
                              "[TF2VR]     MESH[%d.%d.%d] material %d -> skin0 texture %d \"%s\"  meshid %d numverts(stock) %d\n",
                              bp, m, k, material, tex, name[0] ? name : "?", meshId, numVerts);
                Tf2VrLog(line);
                // Publish for the draw-item filter: this mesh's vertex count and whether it is a glove.
                if (meshVertCount - 1 < 16 && meshVertCount > 0) {
                    g_armsMeshVertexCount[meshVertCount - 1] = numVerts;
                    g_armsMeshIsGlove[meshVertCount - 1] = std::strstr(name, "gauntlet") != nullptr ? 1 : 0;
                    g_armsMeshId[meshVertCount - 1] = meshId;
                }
            }
        }
    }
    if (meshesSeen > 0 && meshesResolved == meshesSeen && meshVertCount == meshesSeen) {
        g_armsMeshCount = meshVertCount;
        // The header itself: studiorender's draw context carries it at +0x18
        // (studiorender.dll+0x123DD writes it from the descriptor), so this is
        // how a model draw is recognised as THIS model rather than a grunt
        // that happens to share its materials.
        g_armsStudioHdr = studio;
        g_armsHeaderValid = true;
        // Publish this model into the per-model table too, keyed by its own
        // name, so a filter can look up the model it is actually drawing.
        {
            char mn[64];
            ReadName(reinterpret_cast<const char*>(studio) + 0x10, mn, sizeof(mn));
            int slot = -1;
            for (int k = 0; k < g_armsModelCount; ++k)
                if (std::strcmp(g_armsModels[k].name, mn) == 0) { slot = k; break; }
            if (slot < 0 && g_armsModelCount < kMaxArmsModels) slot = g_armsModelCount++;
            if (slot >= 0) {
                ArmsModelMeshes& a = g_armsModels[slot];
                std::memcpy(a.name, mn, sizeof(mn));
                a.meshCount = meshVertCount;
                for (int k = 0; k < meshVertCount && k < 16; ++k) {
                    a.meshId[k] = g_armsMeshId[k];
                    a.vertexCount[k] = g_armsMeshVertexCount[k];
                    a.isGlove[k] = g_armsMeshIsGlove[k];
                }
                a.valid = true;
                int an = std::snprintf(line, sizeof(line),
                                       "[TF2VR] mesh-census: ARMS MODEL[%d] \"%s\" published, %d meshes (id/verts/glove):",
                                       slot, a.name, a.meshCount);
                for (int k = 0; k < a.meshCount && k < 16; ++k)
                    an += std::snprintf(line + an, sizeof(line) - an, " %d/%d/%d", a.meshId[k], a.vertexCount[k],
                                        a.isGlove[k]);
                std::snprintf(line + an, sizeof(line) - an, "\n");
                Tf2VrLog(line);
            }
        }
        int n = std::snprintf(line, sizeof(line), "[TF2VR] mesh-census: PUBLISHED %d meshes for the filter (verts/glove):", meshVertCount);
        for (int k = 0; k < meshVertCount; ++k)
            n += std::snprintf(line + n, sizeof(line) - n, " %d/%d", g_armsMeshVertexCount[k], g_armsMeshIsGlove[k]);
        std::snprintf(line + n, sizeof(line) - n, "\n");
        Tf2VrLog(line);
    }
    std::snprintf(line, sizeof(line),
                  "[TF2VR] mesh-census: %s -- name %s, %d/%d texture names read (%d path-like), %d/%d cd paths, "
                  "%d meshes seen, %d resolved to a texture name.\n",
                  (nameOk && texOk == numTextures && meshesSeen > 0 && meshesResolved == meshesSeen) ? "CONTROL PASS"
                                                                                                    : "CONTROL FAIL",
                  nameOk ? "ok" : "BAD", texOk, numTextures, texPathLike, cdOk, numCd, meshesSeen, meshesResolved);
    Tf2VrLog(line);
    WalkEmbeddedVtx(studio, meshVerts, meshVertCount);
}

void DumpMeshCensusSeh(const std::uint8_t* studio) {
    __try {
        DumpMeshCensusGuarded(studio);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Tf2VrLog("[TF2VR] mesh-census: FAULTED while walking the tables (caught); dump abandoned for this model.\n");
    }
}

// ---------------------------------------------------------------------------
// THE GATE OBSERVER.
//
// studiorender.dll+0x75A7 (software path, fn 0x6F70) and +0x8947 (hardware
// path, fn 0x8340), per mesh:
//
//     movsxd rax,[r15]                    ; mesh.material
//     movsx  rcx,word [rcx+rax*2]         ; pskinref[material]
//     mov    rcx,[rdx+rcx*8]              ; ppMaterials[...]   (IMaterial*)
//     mov    rax,[rcx]
//     call   [rax+0x238]                  ; IMaterial vtable slot 71
//     test   al,al
//     je     <next mesh>                  ; FALSE = the mesh is not drawn
//
// Slot 71 of the CMaterial vtable (materialsystem_dx11.dll+0x1CF3C8, RTTI
// ".?AVCMaterial@@" via the locator at +0x1CF3C0) is +0x49910:
//
//     sub rsp,28h ; mov rax,[rcx] ; mov edx,8000000h ; call [rax+128h]
//     test al,al ; sete al                ; = !GetMaterialVarFlag(1<<27)
//
// so the engine already hides a mesh by one material flag. The observer only
// watches: the verdict is the original's, unchanged. Slot 0 of the same
// vtable (+0x48210: add rcx,1Ch; jmp CUtlSymbol::String) is GetName.
// ---------------------------------------------------------------------------
constexpr std::uintptr_t kCMaterialVtableRva = 0x1CF3C8;
constexpr int kShouldDrawSlot = 71;
constexpr std::uintptr_t kShouldDrawRva = 0x49910;
constexpr std::uint8_t kShouldDrawPrologue[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x01, 0xBA, 0x00, 0x00, 0x00, 0x08,
                                                0xFF, 0x90, 0x28, 0x01, 0x00, 0x00, 0x84, 0xC0, 0x0F, 0x94, 0xC0};

using GateFn = bool(__fastcall*)(void* self);
using GetNameFn = const char*(__fastcall*)(void* self);

GateFn g_gateOriginal = nullptr;
void** g_gateSlot = nullptr;
bool g_gateTried = false;

constexpr int kMaxSeen = 256;
struct Seen {
    std::atomic<void*> material{nullptr};
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> falses{0};
    char name[96]{};
};
Seen g_seen[kMaxSeen];
std::atomic<int> g_seenCount{0};
std::atomic<std::uint64_t> g_gateCalls{0};
std::atomic<std::uint64_t> g_gateFalse{0};
std::atomic<std::uint64_t> g_gateOverflow{0};
std::atomic<std::uint64_t> g_gateNameFaults{0};
int g_seenPrinted = 0;

// Reads the material's name through its own vtable slot 0, under SEH, into
// a bounded buffer. Runs on the render thread once per distinct material.
bool ReadMaterialName(void* material, char* out, std::size_t cap) {
    __try {
        if (BytesFrom(material) < sizeof(void*)) return false;
        auto* vt = *reinterpret_cast<void***>(material);
        if (BytesFrom(vt) < sizeof(void*)) return false;
        auto getName = reinterpret_cast<GetNameFn>(vt[0]);
        const char* s = getName(material);
        return ReadName(s, out, cap) > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RecordVerdict(void* material, bool drawn) {
    g_gateCalls.fetch_add(1, std::memory_order_relaxed);
    if (!drawn) g_gateFalse.fetch_add(1, std::memory_order_relaxed);
    const int n = g_seenCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        if (g_seen[i].material.load(std::memory_order_relaxed) == material) {
            g_seen[i].calls.fetch_add(1, std::memory_order_relaxed);
            if (!drawn) g_seen[i].falses.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    if (n >= kMaxSeen) {
        g_gateOverflow.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Claim a slot; a concurrent claimer of the same material makes a
    // duplicate row, which the report tolerates.
    int slot = n;
    while (!g_seenCount.compare_exchange_weak(slot, slot + 1, std::memory_order_acq_rel)) {
        if (slot >= kMaxSeen) {
            g_gateOverflow.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    Seen& s = g_seen[slot];
    if (!ReadMaterialName(material, s.name, sizeof(s.name))) {
        g_gateNameFaults.fetch_add(1, std::memory_order_relaxed);
        std::snprintf(s.name, sizeof(s.name), "(name unreadable)");
    }
    s.calls.store(1, std::memory_order_relaxed);
    s.falses.store(drawn ? 0 : 1, std::memory_order_relaxed);
    s.material.store(material, std::memory_order_release);
}

bool __fastcall GateObserver(void* self) {
    const bool drawn = g_gateOriginal(self);
    RecordVerdict(self, drawn);
    return drawn;
}

bool InstallGateObserver() {
    HMODULE ms = GetModuleHandleA("materialsystem_dx11.dll");
    if (!ms) {
        Tf2VrLog("[TF2VR] mesh-census: materialsystem_dx11.dll is not loaded; gate observer not installed.\n");
        return false;
    }
    auto* base = reinterpret_cast<std::uint8_t*>(ms);
    if (std::memcmp(base + kShouldDrawRva, kShouldDrawPrologue, sizeof(kShouldDrawPrologue)) != 0) {
        Tf2VrLog("[TF2VR] mesh-census: CMaterial::ShouldDraw prologue does not match this build; gate observer "
                 "not installed.\n");
        return false;
    }
    auto** slot = reinterpret_cast<void**>(base + kCMaterialVtableRva + kShouldDrawSlot * 8);
    auto* expected = reinterpret_cast<void*>(base + kShouldDrawRva);
    if (*slot != expected) {
        char line[240];
        std::snprintf(line, sizeof(line), "[TF2VR] mesh-census: CMaterial vtable slot %d holds %p, expected %p; gate "
                      "observer not installed.\n", kShouldDrawSlot, *slot, expected);
        Tf2VrLog(line);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        Tf2VrLog("[TF2VR] mesh-census: VirtualProtect failed on the CMaterial vtable; gate observer not installed.\n");
        return false;
    }
    g_gateOriginal = reinterpret_cast<GateFn>(expected);
    *slot = reinterpret_cast<void*>(&GateObserver);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    g_gateSlot = slot;
    Tf2VrLog("[TF2VR] mesh-census: gate observer installed on CMaterial vtable slot 71 "
             "(materialsystem_dx11.dll+0x49910 ShouldDraw = !GetMaterialVarFlag(0x8000000)); read-only, every "
             "verdict is the engine's own.\n");
    return true;
}

}  // namespace

void DumpMeshCensusOnce(const std::uint8_t* studio) {
    if (!studio) return;
    const auto key = reinterpret_cast<std::uint64_t>(studio);
    for (int i = 0; i < g_dumpedCount; ++i) {
        if (g_dumpedHdrs[i] == key) return;
    }
    if (g_dumpedCount >= kMaxDumpedHdrs) return;
    g_dumpedHdrs[g_dumpedCount++] = key;
    DumpMeshCensusSeh(studio);
}

// PARKED 2026-09-05: the studiorender gate observer proved that module dormant
// (zero calls in every run) and has nothing more to say; it does not install.
constexpr bool kGateObserverEnabled = false;

void AdvanceMeshCensus() {
    if (!kGateObserverEnabled) return;
    if (!g_gateSlot) {
        if (g_gateTried || !IsArmsCollapseArmed()) return;  // armed only in a map with the arms entity
        g_gateTried = true;
        InstallGateObserver();
        return;
    }
    static std::uint64_t lastReport = 0;
    const std::uint64_t now = GetTickCount64();
    if (now - lastReport < 1000) return;
    lastReport = now;
    const int n = g_seenCount.load(std::memory_order_acquire);
    if (n > kMaxSeen) return;
    char line[400];
    std::snprintf(line, sizeof(line),
                  "[TF2VR] mesh-census: gate %llu calls, %llu FALSE (mesh skipped by the engine), %d distinct materials, "
                  "%llu past the cap, %llu name faults.%s\n",
                  static_cast<unsigned long long>(g_gateCalls.load()), static_cast<unsigned long long>(g_gateFalse.load()),
                  n, static_cast<unsigned long long>(g_gateOverflow.load()),
                  static_cast<unsigned long long>(g_gateNameFaults.load()),
                  g_gateCalls.load() == 0 ? "  ZERO CALLS -- the gate is not being reached; nothing below is evidence." : "");
    Tf2VrLog(line);
    // New materials since the last report, in discovery order; each printed once.
    for (; g_seenPrinted < n; ++g_seenPrinted) {
        const Seen& s = g_seen[g_seenPrinted];
        void* mat = s.material.load(std::memory_order_acquire);
        if (!mat) break;  // still being filled in on the render thread; retry next second
        std::snprintf(line, sizeof(line), "[TF2VR] GATE[%d] %p calls %llu false %llu \"%s\"\n", g_seenPrinted, mat,
                      static_cast<unsigned long long>(s.calls.load()), static_cast<unsigned long long>(s.falses.load()),
                      s.name);
        Tf2VrLog(line);
    }
}

void RemoveMeshCensus() {
    if (!g_gateSlot) return;
    if (*g_gateSlot == reinterpret_cast<void*>(&GateObserver)) {
        DWORD old = 0;
        if (VirtualProtect(g_gateSlot, sizeof(void*), PAGE_READWRITE, &old)) {
            *g_gateSlot = reinterpret_cast<void*>(g_gateOriginal);
            DWORD ignored = 0;
            VirtualProtect(g_gateSlot, sizeof(void*), old, &ignored);
        }
    }
    g_gateSlot = nullptr;
}
