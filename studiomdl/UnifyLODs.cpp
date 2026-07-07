//========= Copyright � 1996-2008, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//
//=============================================================================//
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <math.h>
#include <float.h>

#include "common/cmdlib.h"
#include "common/scriplib.h"
#include "mathlib/mathlib.h"
#include "studio.h"
#include "studiomdl/studiomdl.h"
#include "studiomdl/bone_setup.h"
#include "tier1/strtools.h"
#include "mathlib/vmatrix.h"
#include "studiomdl/optimize.h"
#include "meshoptimizer.h"

// debugging only - enabling turns off remapping to create all lod vertexes as unique
// to ensure remapping logic does not introduce collapse anomalies
//#define UNIQUE_VERTEXES_FOR_LOD

extern StudioMdlContext g_StudioMdlContext;

//-----------------------------------------------------------------------------
// Forward declarations local to this file
//-----------------------------------------------------------------------------
class CVertexDictionary;

struct VertexInfo_t;

static void BuildBoneLODMapping(CUtlVector<int> &boneMap, int lodID);


//-----------------------------------------------------------------------------
// Globals
//-----------------------------------------------------------------------------
static int g_NumBonesInLOD[MAX_NUM_LODS];


//-----------------------------------------------------------------------------
// Makes sure all boneweights in a s_boneweight_t are valid
//-----------------------------------------------------------------------------
static void ValidateBoneWeight(const s_boneweight_t &boneWeight) {
#ifdef _DEBUG
    int i;
    if (boneWeight.weight[0] == 1.0f) {
        Assert(boneWeight.numbones == 1);
    }
    for (i = 0; i < boneWeight.numbones; i++) {
        Assert(boneWeight.bone[i] >= 0 && boneWeight.bone[i] < g_StudioMdlContext.numbones);
    }

    float weight = 0.0f;
    for (i = 0; i < boneWeight.numbones; i++) {
        weight += boneWeight.weight[i];
    }
    Assert(fabs(weight - 1.0f) < 1e-3);
#endif
}


//-----------------------------------------------------------------------------
// Swap bones
//-----------------------------------------------------------------------------
static inline void SwapBones(s_boneweight_t &boneWeight, int nBone1, int nBone2) {
    // swap
    int nTmpBone = boneWeight.bone[nBone1];
    float flTmpWeight = boneWeight.weight[nBone1];
    boneWeight.bone[nBone1] = boneWeight.bone[nBone2];
    boneWeight.weight[nBone1] = boneWeight.weight[nBone2];
    boneWeight.bone[nBone2] = nTmpBone;
    boneWeight.weight[nBone2] = flTmpWeight;
}


//-----------------------------------------------------------------------------
// Sort the bone weight structure to be sorted by bone weight
//-----------------------------------------------------------------------------
static void SortBoneWeightByWeight(s_boneweight_t &boneWeight) {
    // bubble sort the bones by weight. . .put the largest weight first.
    for (int j = boneWeight.numbones; j > 1; j--) {
        for (int k = 0; k < j - 1; k++) {
            if (boneWeight.weight[k] >= boneWeight.weight[k + 1])
                continue;

            SwapBones(boneWeight, k, k + 1);
        }
    }
}


//-----------------------------------------------------------------------------
// Sort the bone weight structure to be sorted by bone index
//-----------------------------------------------------------------------------
static void SortBoneWeightByIndex(s_boneweight_t &boneWeight) {
    // bubble sort the bones by index. . .put the smallest index first.
    for (int j = boneWeight.numbones; j > 1; j--) {
        for (int k = 0; k < j - 1; k++) {
            if (boneWeight.bone[k] <= boneWeight.bone[k + 1])
                continue;

            SwapBones(boneWeight, k, k + 1);
        }
    }
}


//-----------------------------------------------------------------------------
// A vertex format
//-----------------------------------------------------------------------------
struct VertexInfo_t {
    Vector m_Position;
    Vector m_Normal;
    int m_numTexCoords;
    std::array<Vector2D, MAXSTUDIOTEXCOORDS> m_TexCoord;
    Vector4D m_TangentS;
    s_boneweight_t m_BoneWeight;
    int m_nLodFlag;
};


//-----------------------------------------------------------------------------
// Stores all vertices in the vertex dictionary
//-----------------------------------------------------------------------------
class CVertexDictionary {
public:
    CVertexDictionary();

    // Adds a vertex to the dictionary
    int AddVertex(const VertexInfo_t &srcVertex);

    int AddVertexFromSource(const s_source_t *pSrc, int nVertexIndex, int nLod);

    // Iteration
    int VertexCount() const;

    VertexInfo_t &Vertex(int i);

    const VertexInfo_t &Vertex(int i) const;

    int RootLODVertexStart() const;

    int RootLODVertexEnd() const;

    // Gets the vertex count for the previous LOD
    int PrevLODVertexCount() const;

    // Marks the dictionary as starting defining vertices for a new LOD
    void StartNewLOD();

    void SetRootVertexRange(int start, int end);

private:
    CUtlVector<VertexInfo_t> m_Verts;
    int m_nPrevLODCount;
    int m_nRootLODStart;
    int m_nRootLODEnd;
};


//-----------------------------------------------------------------------------
// Copies in a particular vertex from the s_source_t
//-----------------------------------------------------------------------------
CVertexDictionary::CVertexDictionary() {
    m_nPrevLODCount = 0;
}


//-----------------------------------------------------------------------------
// Accessor
//-----------------------------------------------------------------------------
inline VertexInfo_t &CVertexDictionary::Vertex(int i) {
    return m_Verts[i];
}

inline const VertexInfo_t &CVertexDictionary::Vertex(int i) const {
    return m_Verts[i];
}


//-----------------------------------------------------------------------------
// Gets the vertex count for the previous LOD
//-----------------------------------------------------------------------------
inline int CVertexDictionary::PrevLODVertexCount() const {
    return m_nPrevLODCount;
}


inline int CVertexDictionary::RootLODVertexStart() const {
    return m_nRootLODStart;
}


inline int CVertexDictionary::RootLODVertexEnd() const {
    return m_nRootLODEnd;
}


//-----------------------------------------------------------------------------
// Marks the dictionary as starting defining vertices for a new LOD
//-----------------------------------------------------------------------------
void CVertexDictionary::StartNewLOD() {
    m_nPrevLODCount = VertexCount();
}


void CVertexDictionary::SetRootVertexRange(int start, int end) {
    m_nRootLODStart = start;
    m_nRootLODEnd = end;
}


//-----------------------------------------------------------------------------
// Adds a vertex to the dictionary
//-----------------------------------------------------------------------------
int CVertexDictionary::AddVertex(const VertexInfo_t &srcVertex) {
    int nDstVertID = m_Verts.AddToTail(srcVertex);
    VertexInfo_t &vertex = m_Verts[nDstVertID];
    ValidateBoneWeight(vertex.m_BoneWeight);
    SortBoneWeightByIndex(vertex.m_BoneWeight);
    ValidateBoneWeight(vertex.m_BoneWeight);

    return nDstVertID;
}


//-----------------------------------------------------------------------------
// Copies in a particular vertex from the s_source_t
//-----------------------------------------------------------------------------
int CVertexDictionary::AddVertexFromSource(const s_source_t *pSrc, int nVertexIndex, int nLod) {
    int nDstVertID = m_Verts.AddToTail();
    VertexInfo_t &vertex = m_Verts[nDstVertID];

    const s_vertexinfo_t &srcVertex = pSrc->m_GlobalVertices[nVertexIndex];
    vertex.m_Position = srcVertex.position;
    vertex.m_Normal = srcVertex.normal;
    vertex.m_TangentS = srcVertex.tangentS;
    vertex.m_BoneWeight = srcVertex.boneweight;
    vertex.m_nLodFlag = 1 << nLod;

    for (int i = 0; i < MAXSTUDIOTEXCOORDS; ++i) {
        vertex.m_TexCoord[i] = srcVertex.texcoord[i];
    }
    vertex.m_numTexCoords = srcVertex.numTexcoord;

    ValidateBoneWeight(vertex.m_BoneWeight);
    SortBoneWeightByIndex(vertex.m_BoneWeight);
    ValidateBoneWeight(vertex.m_BoneWeight);

    return nDstVertID;
}


//-----------------------------------------------------------------------------
// How many vertices in the dictionary?
//-----------------------------------------------------------------------------
int CVertexDictionary::VertexCount() const {
    return m_Verts.Count();
}


s_source_t *GetModelLODSource(const char *pModelName,
                              const LodScriptData_t &scriptLOD, bool *pFound) {
    // When doing LOD replacement, ignore all path + extension information
    char *pTempBuf = (char *) _alloca(Q_strlen(pModelName) + 1);

    // Strip extension
    strcpy(pTempBuf, pModelName);
    char *pDot = strrchr(pTempBuf, '.');
    if (pDot) {
        *pDot = 0;
    }

    // Strip directory from the model's filename so that sources loaded from $pushd
    // subdirectories match bare names written in replacemodel entries.
    char *pSlash = strrchr(pTempBuf, '\\');
    char *pSlash2 = strrchr(pTempBuf, '/');
    if (pSlash2 > pSlash) pSlash = pSlash2;
    const char *pBaseName = pSlash ? pSlash + 1 : pTempBuf;

    for (int i = 0; i < scriptLOD.modelReplacements.Count(); i++) {
        const char *pSrcName = scriptLOD.modelReplacements[i].GetSrcName();
        // Primary: basename match (handles $pushd/$popd context shifts)
        // Fallback: full path match (backward compat for QC files using relative paths)
        if (!Q_stricmp(pBaseName, pSrcName) || !Q_stricmp(pTempBuf, pSrcName)) {
            *pFound = true;
            return scriptLOD.modelReplacements[i].m_pSource;
        }
    }

    *pFound = false;
    return 0;
}


//-----------------------------------------------------------------------------
// Tolerances for all fields of the vertex
//-----------------------------------------------------------------------------
#define POSITION_EPSILON    0.05f
#define TEXCOORD_EPSILON    0.01f
#define NORMAL_EPSILON        10.0f    // in degrees
#define TANGENT_EPSILON        10.0f    // in degrees
#define BONEWEIGHT_EPSILON    0.05f
#define EXTRADATA_EPSILON    0.01f

#define UNMATCHED_BONE_WEIGHT 1.0f

//-----------------------------------------------------------------------------
// Computes error between two positions; returns false if the error is too great
//-----------------------------------------------------------------------------
bool ComparePositionFuzzy(const Vector &p1, const Vector &p2, float &flError) {
    Vector vecDelta;
    VectorSubtract(p1, p2, vecDelta);
    flError = DotProduct(vecDelta, vecDelta);
    return (flError <= (POSITION_EPSILON * POSITION_EPSILON));
}


//-----------------------------------------------------------------------------
// Computes error between two normals; returns false if the error is too great
//-----------------------------------------------------------------------------
bool CompareNormalFuzzy(const Vector &n1, const Vector &n2, float &flError) {
    static float flEpsilon = cos(DEG2RAD(NORMAL_EPSILON));

    Vector v1, v2;
    v1 = n1;
    v2 = n2;
    VectorNormalize(v1);
    VectorNormalize(v2);
    float flDot = DotProduct(v1, v2);
    flError = 1.0F - flDot;
    return (flDot >= flEpsilon);
}

//-----------------------------------------------------------------------------
// Computes error between two tangentS vectors; returns false if the error is too great
//-----------------------------------------------------------------------------
bool CompareTangentSFuzzy(const Vector4D &n1, const Vector4D &n2, float &flError) {
    static float flEpsilon = cos(DEG2RAD(TANGENT_EPSILON));

    Vector4D v1, v2;
    v1 = n1;
    v2 = n2;

    if (v1.w != v2.w) {
        // must match as -1 or 1
        flError = 2;
        return false;
    }

    VectorNormalize(v1.AsVector3D());
    VectorNormalize(v2.AsVector3D());
    float flDot = DotProduct(v1.AsVector3D(), v2.AsVector3D());

    // error ranges from [0..2]
    flError = 1.0F - flDot;

    return (flDot >= flEpsilon);
}

//-----------------------------------------------------------------------------
// Computes error between two texcoords; returns false if the error is too great
//-----------------------------------------------------------------------------
bool CompareTexCoordsFuzzy(const std::array<Vector2D, MAXSTUDIOTEXCOORDS> &t1,
                           const std::array<Vector2D, MAXSTUDIOTEXCOORDS> &t2,
                           float &flError) {
    Vector2D vecError;

    flError = 0.0f;

    for (int i = 0; i < MAXSTUDIOTEXCOORDS; ++i) {
        vecError[0] = fabs(t2[i][0] - t1[i][0]);
        vecError[1] = fabs(t2[i][1] - t1[i][1]);
        flError += vecError.LengthSqr();
    }

    return (flError <= (TEXCOORD_EPSILON * TEXCOORD_EPSILON));
}

//-----------------------------------------------------------------------------
// Computes the error between two bone weights, returns false if they are too far
//-----------------------------------------------------------------------------
bool CompareBoneWeightsFuzzy(const s_boneweight_t &b1, const s_boneweight_t &b2, float &flError) {
    // This is a list of which bones that exist in b1 also exist in b2.
    // Use the index to figure out where in the array for b2 that the corresponding bone in b1 is.
    int nMatchingBones = 0;
    // sized to the source-stage capacity: normally these hold at most
    // MAXSTUDIOBONEWEIGHTS entries by the time UnifyLODs runs, but indexing is
    // bounded by numbones, which can legally reach the larger cap
    int pBoneIndexMap1[MAXSTUDIOSRCBONEWEIGHTS];
    int pBoneIndexMap2[MAXSTUDIOSRCBONEWEIGHTS];

    int i;
    for (i = 0; i < b2.numbones; ++i) {
        pBoneIndexMap2[i] = -1;
    }

    for (i = 0; i < b1.numbones; ++i) {
        pBoneIndexMap1[i] = -1;
        for (int j = 0; j < b2.numbones; ++j) {
            if (b2.bone[j] == b1.bone[i]) {
                pBoneIndexMap1[i] = j;
                pBoneIndexMap2[j] = i;
                ++nMatchingBones;
                break;
            }
        }
    }

    // If no bones match, we're done
    if (!nMatchingBones) {
        flError = FLT_MAX;
        return false;
    }

    // At least one bone matches, so we're going to consider this vertex as a potential match
    // This loop will take care of figuring out the error for all bones that exist in
    // b1 alone, and all bones that exist in b1 and b2
    flError = 0;
    for (i = 0; i < b1.numbones; ++i) {
        // If we didn't find a match for this bone, compute a more expensive weight
        if (pBoneIndexMap1[i] == -1) {
            flError += b1.weight[i] * b1.weight[i] * UNMATCHED_BONE_WEIGHT;
            continue;
        }

        float flDeltaWeight = fabs(b1.weight[i] - b2.weight[pBoneIndexMap1[i]]);
        flError += flDeltaWeight * flDeltaWeight;
    }

    // This loop will take care of figuring out the error for all bones that exist in b2 alone
    for (i = 0; i < b2.numbones; ++i) {
        // If we didn't find a match for this bone, compute a more expensive weight
        if (pBoneIndexMap2[i] == -1) {
            flError += b2.weight[i] * b2.weight[i] * UNMATCHED_BONE_WEIGHT;
        }
    }

    // This renormalizes the error. The error will become greater with the total
    // number of bones in the two vertices.
    flError /= sqrt((float) (b1.numbones + b2.numbones));
    return (flError <= BONEWEIGHT_EPSILON);
}


//-----------------------------------------------------------------------------
// Searches for a material in the texture list
//-----------------------------------------------------------------------------
int FindMaterialByName(const char *pMaterialName) {
    int i;
    int allocLen = strlen(pMaterialName) + 1;
    char *pBaseName = (char *) _alloca(allocLen);
    Q_FileBase((char *) pMaterialName, pBaseName, allocLen);

    for (i = 0; i < g_numtextures; i++) {
        if (stricmp(pMaterialName, g_texture[i].name) == 0) {
            return i;
        }
    }
    return -1;
}

static s_mesh_t *FindMeshByMaterial(s_source_t *pSrc, int nMaterialID) {
    for (int m = 0; m < pSrc->nummeshes; m++) {
        if (pSrc->meshindex[m] == nMaterialID)
            return &pSrc->mesh[pSrc->meshindex[m]];
    }

    // this mesh/material doesn't exist at this lod.
    return nullptr;
}


static s_mesh_t *FindOrCullMesh(int nLodID, s_source_t *pSrc, int nMaterialID) {
    char baseMeshName[MAX_PATH];
    char baseRemovalName[MAX_PATH];

    // possibly marked for removal via $removemesh
    // determine mesh name
    int nTextureID = MaterialToTexture(nMaterialID);
    if (nTextureID == -1) {
        MdlError("Unknown Texture for Material %d\n", nMaterialID);
    }

    Q_FileBase(g_texture[nTextureID].name, baseMeshName, sizeof(baseMeshName) - 1);
    for (int i = 0; i < g_ScriptLODs[nLodID].meshRemovals.Count(); i++) {
        const char *pMeshRemovalName = g_ScriptLODs[nLodID].meshRemovals[i].GetSrcName();
        Q_FileBase(pMeshRemovalName, baseRemovalName, sizeof(baseRemovalName) - 1);

        if (!stricmp(baseRemovalName, baseMeshName)) {
            // mesh has been marked for removal
            return nullptr;
        }
    }

    // removemeshword: case-insensitive substring match against the base name
    // (path already stripped, so DMX material pathing is ignored)
    for (int i = 0; i < g_ScriptLODs[nLodID].meshWordRemovals.Count(); i++) {
        const char *pWord = g_ScriptLODs[nLodID].meshWordRemovals[i].GetSrcName();
        if (pWord && pWord[0] && V_stristr(baseMeshName, pWord)) {
            // mesh has been marked for removal
            return nullptr;
        }
    }

    s_mesh_t *pMesh = FindMeshByMaterial(pSrc, nMaterialID);
    return pMesh;
}


static void CopyVerts(int nLodID, const s_source_t *pSrc, const s_mesh_t *pSrcMesh, CVertexDictionary &vertexDict,
                      s_mesh_t *pDstMesh, int *pMeshVertIndexMap) {
    // populate the dictionary with the verts
    for (int srcVertID = 0; srcVertID < pSrcMesh->numvertices; srcVertID++) {
        int nVertexIndex = pSrcMesh->vertexoffset + srcVertID;
        pMeshVertIndexMap[nVertexIndex] =
                vertexDict.AddVertexFromSource(pSrc, nVertexIndex, nLodID) - pDstMesh->vertexoffset;
    }

    pDstMesh->numvertices = pSrcMesh->numvertices;
}

static void
CopyFaces(const s_source_t *pSrc, const s_mesh_t *pSrcMesh, CUtlVector<s_face_t> &faces, s_mesh_t *pDstMesh) {
    int srcFaceID;
    for (srcFaceID = 0; srcFaceID < pSrcMesh->numfaces; srcFaceID++) {
        int srcID = srcFaceID + pSrcMesh->faceoffset;
        s_face_t *pSrcFace = &pSrc->face[srcID];
        s_face_t *pDstFace = &faces[faces.AddToTail()];
        pDstFace->a = pSrcFace->a;
        pDstFace->b = pSrcFace->b;
        pDstFace->c = pSrcFace->c;
        pDstFace->d = pSrcFace->d;
        pDstMesh->numfaces++;
    }
}

//-----------------------------------------------------------------------------
// Modify the bone weights in all of the vertices....
//-----------------------------------------------------------------------------
static void RemapBoneWeights(const CUtlVector<int> &boneMap, s_boneweight_t &boneWeight) {
    for (int i = 0; i < boneWeight.numbones; i++) {
        Assert(boneWeight.bone[i] >= 0 && boneWeight.bone[i] < boneMap.Count());
        boneWeight.bone[i] = boneMap[boneWeight.bone[i]];
    }
}


//-----------------------------------------------------------------------------
// After the remapping, we may get multiple instances of the same bone
// which we want to collapse into a single bone
//-----------------------------------------------------------------------------
static void CollapseBoneWeights(s_boneweight_t &boneWeight) {
    // We need the bones to be sorted by bone index for the loop right below
    SortBoneWeightByIndex(boneWeight);

    for (int i = 0; i < boneWeight.numbones - 1; i++) {
        if (boneWeight.bone[i] != boneWeight.bone[i + 1])
            continue;

        // add i+1's weight to i since they have the same bone index
        boneWeight.weight[i] += boneWeight.weight[i + 1];

        // remove i+1
        for (int j = i + 1; j < boneWeight.numbones - 1; j++) {
            boneWeight.bone[j] = boneWeight.bone[j + 1];
            boneWeight.weight[j] = boneWeight.weight[j + 1];
        }
        --boneWeight.numbones;

        // Gotta step back one, may have many bones collapsing into one
        --i;
    }

    ValidateBoneWeight(boneWeight);
}


static bool FuzzyFloatCompare(float f1, float f2, float epsilon) {
    if (fabs(f1 - f2) < epsilon) {
        return true;
    } else {
        return false;
    }
}


//-----------------------------------------------------------------------------
// Is this bone weight structure sorted by bone?
//-----------------------------------------------------------------------------
static bool IsBoneWeightSortedByBone(const s_boneweight_t &src) {
    for (int i = 1; i < src.numbones; ++i) {
        Assert(src.bone[i] != -1);
        if (src.bone[i - 1] > src.bone[i])
            return false;
    }

    return true;
}


//-----------------------------------------------------------------------------
// Are two bone-weight structures equal?
//-----------------------------------------------------------------------------
static bool AreBoneWeightsEqual(const s_boneweight_t &b1, const s_boneweight_t &b2) {
    // Have to have the same number of bones
    if (b1.numbones != b2.numbones)
        return false;

    // This is a list of which bones that exist in b1 also exist in b2.
    // Use the index to figure out where in the array for b2 that the corresponding bone in b1 is.
    int nMatchingBones = 0;
    int pBoneIndexMap[MAX_NUM_BONES_PER_VERT];

    int i;
    for (i = 0; i < b1.numbones; ++i) {
        pBoneIndexMap[i] = -1;
        for (int j = 0; j < b2.numbones; ++j) {
            if (b2.bone[j] == b1.bone[i]) {
                pBoneIndexMap[i] = j;
                ++nMatchingBones;
                break;
            }
        }
    }

    // If we aren't using the same bone indices, we're done
    if (nMatchingBones != b1.numbones)
        return false;

    // Check to see if the weights are the same
    for (i = 0; i < b1.numbones; ++i) {
        Assert(pBoneIndexMap[i] != -1);
        if (b1.weight[i] != b2.weight[pBoneIndexMap[i]])
            return false;
    }

    return true;
}

//-----------------------------------------------------------------------------
// Finds an *exact* requested vertex in the dictionary
//-----------------------------------------------------------------------------
static int
FindVertexInDictionaryExact(CVertexDictionary &vertexDict, int nStartVert, int nEndVert, const VertexInfo_t &vertex) {
    for (int nVertID = nStartVert; nVertID < nEndVert; ++nVertID) {
        if (vertexDict.Vertex(nVertID).m_Position != vertex.m_Position)
            continue;

        if (!AreBoneWeightsEqual(vertexDict.Vertex(nVertID).m_BoneWeight, vertex.m_BoneWeight))
            continue;

        bool bMatch = true;
        for (int i = 0; i < MAXSTUDIOTEXCOORDS; ++i) {
            if (vertexDict.Vertex(nVertID).m_TexCoord[i] != vertex.m_TexCoord[i]) {
                bMatch = false;
                break;
            }
        }
        if (!bMatch)
            continue;

        if (vertexDict.Vertex(nVertID).m_Normal != vertex.m_Normal)
            continue;

        if (vertexDict.Vertex(nVertID).m_TangentS != vertex.m_TangentS)
            continue;

        return nVertID;
    }

    return -1;
}


//-----------------------------------------------------------------------------
// Finds the *exact* requested vertex in the dictionary or creates it
//-----------------------------------------------------------------------------
static int FindOrCreateExactVertexInDictionary(CVertexDictionary &vertexDict,
                                               const VertexInfo_t &vertex, s_mesh_t *pDstMesh) {
    int nMeshVertID = FindVertexInDictionaryExact(vertexDict, pDstMesh->vertexoffset,
                                                  pDstMesh->vertexoffset + pDstMesh->numvertices, vertex);
    if (nMeshVertID != -1) {
        // flag vertex for what LoD's are using it
        vertexDict.Vertex(nMeshVertID).m_nLodFlag |= vertex.m_nLodFlag;
        return nMeshVertID - pDstMesh->vertexoffset;
    }

    nMeshVertID = vertexDict.AddVertex(vertex);
    ++pDstMesh->numvertices;
    return nMeshVertID - pDstMesh->vertexoffset;
}

static void PrintBonesUsedInLOD(s_source_t *pSrc) {
    printf("PrintBonesUsedInLOD\n");

    int nVertexCount = pSrc->m_GlobalVertices.Count();
    for (int i = 0; i < nVertexCount; i++) {
        Vector &pos = pSrc->m_GlobalVertices[i].position;
        Vector &norm = pSrc->m_GlobalVertices[i].normal;
        Vector2D &texcoord = pSrc->m_GlobalVertices[i].texcoord[0];
        printf("pos: %f %f %f norm: %f %f %f texcoord: %f %f\n",
               pos[0], pos[1], pos[2], norm[0], norm[1], norm[2], texcoord[0], texcoord[1]);
        s_boneweight_t *pBoneWeight = &pSrc->m_GlobalVertices[i].boneweight;
        int j;
        for (j = 0; j < pBoneWeight->numbones; j++) {
            int globalBoneID = pBoneWeight->bone[j];
            const char *pBoneName = g_bonetable[globalBoneID].name;
            printf("vert: %d bone: %d boneid: %d weight: %f name: \"%s\"\n", i, (int) j, (int) pBoneWeight->bone[j],
                   (float) pBoneWeight->weight[j], pBoneName);
        }
        printf("\n");
        fflush(stdout);
    }
}


//-----------------------------------------------------------------------------
// Indicates a particular set of bones is used by a particular LOD
//-----------------------------------------------------------------------------
static void MarkBonesUsedByLod(const s_boneweight_t &boneWeight, int nLodID) {
    for (int j = 0; j < boneWeight.numbones; ++j) {
        int nGlobalBoneID = boneWeight.bone[j];
        s_bonetable_t *pBone = &g_bonetable[nGlobalBoneID];
        pBone->flags |= (BONE_USED_BY_VERTEX_LOD0 << nLodID);
    }
}


static void PrintSBoneWeight(s_boneweight_t *pBoneWeight, const s_source_t *pSrc) {
    int j;
    for (j = 0; j < pBoneWeight->numbones; j++) {
        int globalBoneID;
        globalBoneID = pBoneWeight->bone[j];
        const char *pBoneName = g_bonetable[globalBoneID].name;
        printf("bone: %d boneid: %d weight: %f name: \"%s\"\n", (int) j, (int) pBoneWeight->bone[j],
               (float) pBoneWeight->weight[j], pBoneName);
    }
}


//-----------------------------------------------------------------------------
// In the non-top LOD, look for vertices that would be appropriate from the
// vertex dictionary, and use them if you find them, or add new vertices to the 
// vertex dictionary if not and use those new vertices.
//-----------------------------------------------------------------------------
static void CreateLODVertsInDictionary(int nLodID, s_source_t *pCurrentLODSrc,
                                       const s_mesh_t *pCurrLODMesh, s_mesh_t *pVertexDictMesh,
                                       CVertexDictionary &vertexDict, int *pMeshVertIndexMap) {
    Assert(nLodID);

    int nNumCurrentVerts = vertexDict.VertexCount();
    vertexDict.StartNewLOD();

    CUtlVector<int> boneMap;
    BuildBoneLODMapping(boneMap, nLodID);

    for (int nSrcVertID = 0; nSrcVertID < pCurrLODMesh->numvertices; ++nSrcVertID) {
        int nSrcID = nSrcVertID + pCurrLODMesh->vertexoffset;
        const s_vertexinfo_t &srcVertex = pCurrentLODSrc->m_GlobalVertices[nSrcID];

        VertexInfo_t vertex;
        vertex.m_Position = srcVertex.position;
        vertex.m_Normal = srcVertex.normal;
        vertex.m_TangentS = srcVertex.tangentS;
        vertex.m_BoneWeight = srcVertex.boneweight;
        vertex.m_nLodFlag = 1 << nLodID;
        for (int i = 0; i < MAXSTUDIOTEXCOORDS; ++i)
            vertex.m_TexCoord[i] = srcVertex.texcoord[i];
        vertex.m_numTexCoords = srcVertex.numTexcoord;

        // Apply per-LOD bone remapping (replacebone / bonetreecollapse commands)
        RemapBoneWeights(boneMap, vertex.m_BoneWeight);
        CollapseBoneWeights(vertex.m_BoneWeight);
        SortBoneWeightByWeight(vertex.m_BoneWeight);

        MarkBonesUsedByLod(vertex.m_BoneWeight, nLodID);

        int nMeshVertID = FindOrCreateExactVertexInDictionary(vertexDict, vertex, pVertexDictMesh);
        pMeshVertIndexMap[nSrcID] = nMeshVertID;
    }

    int nNewVertsCreated = vertexDict.VertexCount() - nNumCurrentVerts;
    if (!g_StudioMdlContext.quiet && nNewVertsCreated) {
        printf("Lod %d: vertexes: %d (%d new)\n", nLodID, vertexDict.VertexCount(), nNewVertsCreated);
    }
}

static void PrintSourceVerts(s_source_t *pSrc) {
    int nVertexCount = pSrc->m_GlobalVertices.Count();
    for (int i = 0; i < nVertexCount; i++) {
        const s_vertexinfo_t &srcVertex = pSrc->m_GlobalVertices[i];
        printf("v %d ", i);
        printf("pos: %f %f %f ", srcVertex.position[0], srcVertex.position[1], srcVertex.position[2]);
        printf("norm: %f %f %f ", srcVertex.normal[0], srcVertex.normal[1], srcVertex.normal[2]);
        printf("texcoord: %f %f\n", srcVertex.texcoord[0].x, srcVertex.texcoord[1].y);
        int j;
        for (j = 0; j < srcVertex.boneweight.numbones; j++) {
            printf("\t%d: %d %f\n", j, (int) srcVertex.boneweight.bone[j],
                   srcVertex.boneweight.weight[j]);
        }
        fflush(stdout);
    }
}


//-----------------------------------------------------------------------------
// Copy the vertex dictionary to the finalized processed data
// Leaves the source data intact, necessary for later processes.
// Routines can then choose which data they operate on
//-----------------------------------------------------------------------------
static void SetProcessedWithDictionary(s_model_t *pSrcModel, CVertexDictionary &vertexDict,
                                       CUtlVector<s_face_t> &faces, CUtlVector<s_mesh_t> &meshes,
                                       int *pMeshVertIndexMaps[MAX_NUM_LODS]) {
    int i;

    s_loddata_t *pLodData = new s_loddata_t;
    memset(pLodData, 0, sizeof(s_loddata_t));

    pSrcModel->m_pLodData = pLodData;

    int nVertexCount = vertexDict.VertexCount();

    pLodData->vertex = (s_lodvertexinfo_t *) calloc(nVertexCount, sizeof(s_lodvertexinfo_t));
    pLodData->numvertices = nVertexCount;
    pLodData->face = (s_face_t *) calloc(faces.Count(), sizeof(s_face_t));
    pLodData->numfaces = faces.Count();

    for (i = 0; i < nVertexCount; ++i) {
        const VertexInfo_t &srcVertex = vertexDict.Vertex(i);
        s_lodvertexinfo_t &dstVertex = pLodData->vertex[i];

        dstVertex.boneweight = srcVertex.m_BoneWeight;
        Assert(dstVertex.boneweight.numbones <= 4);
        dstVertex.position = srcVertex.m_Position;
        dstVertex.normal = srcVertex.m_Normal;
        dstVertex.tangentS = srcVertex.m_TangentS;
        dstVertex.lodFlag = srcVertex.m_nLodFlag;

        for (int j = 0; j < MAXSTUDIOTEXCOORDS; ++j) {
            dstVertex.texcoord[j] = srcVertex.m_TexCoord[j];
        }
        dstVertex.numTexcoord = srcVertex.m_numTexCoords;
    }

    memcpy(pLodData->face, faces.Base(), faces.Count() * sizeof(s_face_t));
    memcpy(pLodData->mesh, meshes.Base(), meshes.Count() * sizeof(s_mesh_t));

    for (i = 0; i < MAX_NUM_LODS; i++) {
        pLodData->pMeshVertIndexMaps[i] = pMeshVertIndexMaps[i];
    }
}


//-----------------------------------------------------------------------------
// This fills out boneMap, which is a mapping from src bone to src bone replacement (or to itself
// if there is no bone replacement.
//-----------------------------------------------------------------------------
static void BuildBoneLODMapping(CUtlVector<int> &boneMap, int lodID) {
    boneMap.AddMultipleToTail(g_StudioMdlContext.numbones);

    Assert(lodID < g_ScriptLODs.Count());
    LodScriptData_t &scriptLOD = g_ScriptLODs[lodID];

    // First, create a direct mapping where no bones are collapsed
    int i;
    for (i = 0; i < g_StudioMdlContext.numbones; i++) {
        boneMap[i] = i;
    }

    for (i = 0; i < scriptLOD.boneReplacements.Count(); i++) {
        const char *src, *dst;
        src = scriptLOD.boneReplacements[i].GetSrcName();
        dst = scriptLOD.boneReplacements[i].GetDstName();
        int j = findGlobalBone(src);
        int k = findGlobalBone(dst);

        if (j != -1 && k != -1) {
            boneMap[j] = k;
        } else if (j == -1) {
            // FIXME: is this really an error?  It could just be  replacement command for bone that doesnt' exist anymore.
            if (g_StudioMdlContext.verbose) {
                MdlWarning("Couldn't replace unknown bone \"%s\" with \"%s\"\n", src, dst);
            }
        } else {
            // FIXME: is this really an error?  It could just be  replacement command for bone that doesnt' exist anymore.
            if (g_StudioMdlContext.verbose) {
                MdlWarning("Couldn't replace bone \"%s\" with unknown \"%s\"\n", src, dst);
            }
        }
    }
}

static void MarkRootLODBones(CVertexDictionary &vertexDictionary) {
    // should result in an identity mapping
    // because their are no bone remaps at the root lod
    CUtlVector<int> boneMap;
    BuildBoneLODMapping(boneMap, 0);

    // iterate and mark bones
    for (int nVertDictID = vertexDictionary.RootLODVertexStart();
         nVertDictID < vertexDictionary.RootLODVertexEnd(); nVertDictID++) {
        s_boneweight_t &boneWeight = vertexDictionary.Vertex(nVertDictID).m_BoneWeight;

        RemapBoneWeights(boneMap, boneWeight);
        CollapseBoneWeights(boneWeight);
        SortBoneWeightByWeight(boneWeight);

        MarkBonesUsedByLod(boneWeight, 0);
    }
}


//-----------------------------------------------------------------------------
// Computes LOD vertices for a model piece.
//-----------------------------------------------------------------------------
static void UnifyModelLODs(s_model_t *pSrcModel) {
    if (!Q_stricmp(pSrcModel->name, "blank"))
        return;

    // each lod has a unique vertex mapping table
    int nNumLODs = pSrcModel->m_LodSources.Count();
    int nLodID;
    int *pMeshVertIndexMaps[MAX_NUM_LODS];
    for (nLodID = 0; nLodID < MAX_NUM_LODS; nLodID++) {
        if (nLodID < nNumLODs && pSrcModel->m_LodSources[nLodID]) {
            int nVertexCount = pSrcModel->m_LodSources[nLodID]->m_GlobalVertices.Count();
            pMeshVertIndexMaps[nLodID] = new int[nVertexCount];
#ifdef _DEBUG
            memset(pMeshVertIndexMaps[nLodID], 0xDD, nVertexCount * sizeof(int));
#endif
        } else {
            pMeshVertIndexMaps[nLodID] = NULL;
        }
    }

    // These hold the aggregate data for the model that grows as lods are processed
    CVertexDictionary vertexDictionary;
    CUtlVector<s_face_t> faces;
    CUtlVector<s_mesh_t> meshes;

    meshes.AddMultipleToTail(MAXSTUDIOSKINS);
    Assert(meshes.Count() == MAXSTUDIOSKINS);
    memset(meshes.Base(), 0, meshes.Count() * sizeof(s_mesh_t));

    int nMeshID;
    for (nMeshID = 0; nMeshID < pSrcModel->source->nummeshes; nMeshID++) {
        s_mesh_t *pVertexDictMesh = &meshes[pSrcModel->source->meshindex[nMeshID]];

        pVertexDictMesh->numvertices = 0;
        pVertexDictMesh->vertexoffset = vertexDictionary.VertexCount();
        pVertexDictMesh->numfaces = 0;
        pVertexDictMesh->faceoffset = faces.Count();

        // First build up information for LOD 0
        if (!pSrcModel->m_LodSources[0])
            continue;

        s_source_t *pLOD0Source = pSrcModel->m_LodSources[0];

        // lookup the material used by this mesh
        int nMaterialID = pLOD0Source->meshindex[nMeshID];
        const char *pName = g_texture[nMaterialID].name;
        if (!g_StudioMdlContext.quiet) {
            printf("Processing LOD for material: %s\n", pName);
        }
        s_mesh_t *pLOD0Mesh = FindMeshByMaterial(pLOD0Source, nMaterialID);
        if (!pLOD0Mesh)
            continue;

        // populate with all vertices from LOD 0
        int nStart = vertexDictionary.VertexCount();
        CopyVerts(0, pLOD0Source, pLOD0Mesh, vertexDictionary, pVertexDictMesh, pMeshVertIndexMaps[0]);
        vertexDictionary.SetRootVertexRange(nStart, vertexDictionary.VertexCount());

        MarkRootLODBones(vertexDictionary);

        // only fix up the faces for the highest lod since the lowest ones are going
        // to be reprocessed later.
        CopyFaces(pLOD0Source, pLOD0Mesh, faces, pVertexDictMesh);

        // Now, for each LOD, try to build meshes using the vertices in LOD 0.
        // Ideally, vertices used in an LOD would be in LOD 0 for the benefit of shared vertices.
        // If we don't find vertices in LOD 0, this code will add vertices into LOD 0's list
        // of vertices for the next LOD to find
        for (nLodID = 1; nLodID < nNumLODs; ++nLodID) {
            s_source_t *pCurrLOD = pSrcModel->m_LodSources[nLodID];
            if (!pCurrLOD)
                continue;

            // Find the mesh that matches the material
            // mesh may not be present or could be culled due to $removemesh commands
            s_mesh_t *pCurrLODMesh = FindOrCullMesh(nLodID, pCurrLOD, nMaterialID);
            if (!pCurrLODMesh)
                continue;

            CreateLODVertsInDictionary(nLodID, pCurrLOD, pCurrLODMesh, pVertexDictMesh, vertexDictionary,
                                       pMeshVertIndexMaps[nLodID]);
        }
    }

#ifdef _DEBUG
    Msg("Total vertex count: %d\n", vertexDictionary.VertexCount());
#endif

    // save the data we just built into the processed data section
    // The processed data has all of the verts that are needed for all LODs.
    SetProcessedWithDictionary(pSrcModel, vertexDictionary, faces, meshes, pMeshVertIndexMaps);
//	PrintSourceVerts( pSrcModel->m_LodModels[0] );
}


//-----------------------------------------------------------------------------
// Force the vertex array for a model to have all of the vertices that are needed
// for all of the LODs of the model.
//-----------------------------------------------------------------------------
void UnifyLODs() {
    // todo: need to fixup the firstref/lastref stuff . . do we really need it anymore?
    for (int modelID = 0; modelID < g_nummodelsbeforeLOD; modelID++) {
        UnifyModelLODs(g_model[modelID]);
    }
}


static void PrintSpaces(int numSpaces) {
    int i;
    for (i = 0; i < numSpaces; i++) {
        printf(" ");
    }
}

static void SpewBoneInfo(int globalBoneID, int depth) {
    s_bonetable_t *pBone = &g_bonetable[globalBoneID];
    if (g_StudioMdlContext.printBones) {
        PrintSpaces(depth * 2);
        printf("%d \"%s\" ", depth, pBone->name);
    }
    int i;
    for (i = 0; i < 8; i++) {
        if (pBone->flags & (BONE_USED_BY_VERTEX_LOD0 << i)) {
            if (g_StudioMdlContext.printBones) {
                printf("lod%d ", i);
            }
            g_NumBonesInLOD[i]++;
        }
    }

    if (g_StudioMdlContext.printBones) {
        if (pBone->flags & BONE_USED_BY_HITBOX)
            printf("hitbox ");

        if (pBone->flags & BONE_USED_BY_ATTACHMENT)
            printf("attachment ");

        if (pBone->flags & BONE_USED_BY_BONE_MERGE)
            printf("merge ");

        printf("\n");
    }

    int j;
    for (j = 0; j < g_StudioMdlContext.numbones; j++) {
        s_bonetable_t *pBone = &g_bonetable[j];
        if (pBone->parent == globalBoneID) {
            SpewBoneInfo(j, depth + 1);
        }
    }
}

void SpewBoneUsageStats() {
    memset(g_NumBonesInLOD, 0, sizeof(int) * MAX_NUM_LODS);
    if (g_StudioMdlContext.numbones == 0) {
        return;
    }
    SpewBoneInfo(0, 0);
    if (g_StudioMdlContext.printBones) {
        int i;
        for (i = 0; i < g_ScriptLODs.Count(); i++) {
            printf("\t%d bones used in lod %d\n", g_NumBonesInLOD[i], i);
        }
    }
}

void MarkParentBoneLODs() {
    int i;
    for (i = 0; i < g_StudioMdlContext.numbones; i++) {
        int flags = g_bonetable[i].flags;
        flags &= BONE_USED_BY_VERTEX_MASK;
        int globalBoneID = g_bonetable[i].parent;
        while (globalBoneID != -1) {
            g_bonetable[globalBoneID].flags |= flags;
            globalBoneID = g_bonetable[globalBoneID].parent;
        }
    }
}


//-----------------------------------------------------------------------------
// Strip path and extension from a model name for comparison, same convention
// as GetModelLODSource. Writes result into buf (size MAX_PATH).
//-----------------------------------------------------------------------------
static const char *GetModelBaseName(const char *pName, char *buf) {
    Q_strncpy(buf, pName, MAX_PATH);
    char *pDot = strrchr(buf, '.');
    if (pDot) *pDot = 0;
    char *pSlash = strrchr(buf, '\\');
    char *pSlash2 = strrchr(buf, '/');
    if (pSlash2 > pSlash) pSlash = pSlash2;
    return pSlash ? pSlash + 1 : buf;
}


//-----------------------------------------------------------------------------
// Decimate pSrc using meshoptimizer and return a new registered s_source_t.
// factor 1.0 = full detail, 0.5 = half triangles. Uses LOD0's vertex data
// so bone weights are already correct - no transfer needed.
//-----------------------------------------------------------------------------
static s_source_t *GenerateDecimatedSource(const s_source_t *pSrc, float factor, int nLodID) {
    if (g_numsources >= MAXSTUDIOSEQUENCES)
        MdlError("GenerateDecimatedSource: ran out of source slots\n");

    s_source_t *pDst = (s_source_t *)calloc(1, sizeof(s_source_t));
    g_source[g_numsources++] = pDst;

    // Shallow copy - shares bone table, animation data, etc.
    memcpy(pDst, pSrc, sizeof(s_source_t));

    // Zero before assign so operator= allocates a fresh buffer instead of aliasing
    // pSrc's (which RemapVerticesToGlobalBones may later realloc, leaving us dangling).
    memset(&pDst->m_GlobalVertices, 0, sizeof(pDst->m_GlobalVertices));
    pDst->m_GlobalVertices = pSrc->m_GlobalVertices;

    // Deep copy vertex[] so the decimated source owns its geometry; otherwise an
    // in-place per-source edit (e.g. ApplyStaticPropPose) would hit pSrc's array once
    // per aliasing source, compounding the transform.
    pDst->vertex = pSrc->numvertices
        ? (s_vertexinfo_t *)malloc(pSrc->numvertices * sizeof(s_vertexinfo_t)) : nullptr;
    if (pDst->vertex)
        memcpy(pDst->vertex, pSrc->vertex, pSrc->numvertices * sizeof(s_vertexinfo_t));

    // Per-mesh decimation: build new faces for each material mesh
    struct MeshFaces { CUtlVector<s_face_t> faces; };
    MeshFaces meshResults[MAXSTUDIOSKINS];

    int nTotalNewFaces = 0;
    float resultError = 0.0f;

    // $rendermesh sources have m_GlobalVertices pre-populated (mesh offsets relative to it);
    // $model sources have it empty here, so fall back to vertex[].
    const s_vertexinfo_t *pVertBase = pSrc->m_GlobalVertices.Count() > 0
        ? pSrc->m_GlobalVertices.Base()
        : pSrc->vertex;
    int nAvailVerts = pSrc->m_GlobalVertices.Count() > 0
        ? pSrc->m_GlobalVertices.Count()
        : pSrc->numvertices;

    unsigned int simplifyOptions = g_staticprop ? meshopt_SimplifyLockBorder : 0;

    for (int mi = 0; mi < pSrc->nummeshes; mi++) {
        int matID = pSrc->meshindex[mi];
        const s_mesh_t &srcMesh = pSrc->mesh[matID];

        if (srcMesh.numfaces == 0 || srcMesh.numvertices == 0 || nAvailVerts == 0)
            continue;

        // Skip meshes that will be removed by removemesh - no point decimating them
        if (FindOrCullMesh(nLodID, const_cast<s_source_t *>(pSrc), matID) == nullptr)
            continue;

        // Face indices are mesh-local (0..numvertices-1), so pass the mesh's own vertex slice.
        // s_vertexinfo_t has int material + int mesh before Vector position, so pass &position
        // with sizeof(s_vertexinfo_t) stride so meshoptimizer advances correctly.
        const float *pMeshPositions = (const float *)&pVertBase[srcMesh.vertexoffset].position;

        int nSrcIndices = srcMesh.numfaces * 3;
        CUtlVector<unsigned int> srcIdx, dstIdx;
        srcIdx.SetCount(nSrcIndices);
        dstIdx.SetCount(nSrcIndices);

        for (int fi = 0; fi < srcMesh.numfaces; fi++) {
            const s_face_t &f = pSrc->face[srcMesh.faceoffset + fi];
            srcIdx[fi * 3 + 0] = f.a;
            srcIdx[fi * 3 + 1] = f.b;
            srcIdx[fi * 3 + 2] = f.c;
        }

        size_t targetIdx = (size_t)((float)nSrcIndices * factor);
        targetIdx = (targetIdx / 3) * 3;
        if (targetIdx < 3) targetIdx = 3;

        size_t newIdxCount = meshopt_simplify(
            dstIdx.Base(), srcIdx.Base(), (size_t)nSrcIndices,
            pMeshPositions, (size_t)srcMesh.numvertices, sizeof(s_vertexinfo_t),
            targetIdx, 1.0f, simplifyOptions, &resultError);

        for (size_t fi = 0; fi < newIdxCount / 3; fi++) {
            s_face_t face;
            face.a = dstIdx[(int)(fi * 3 + 0)];
            face.b = dstIdx[(int)(fi * 3 + 1)];
            face.c = dstIdx[(int)(fi * 3 + 2)];
            face.d = 0xFFFFFFFF;
            meshResults[matID].faces.AddToTail(face);
        }
        nTotalNewFaces += (int)(newIdxCount / 3);
    }

    // Build new flat face array
    pDst->face = nTotalNewFaces > 0
        ? (s_face_t *)malloc(nTotalNewFaces * sizeof(s_face_t))
        : nullptr;
    pDst->numfaces = nTotalNewFaces;

    int faceOffset = 0;
    for (int mi = 0; mi < pSrc->nummeshes; mi++) {
        int matID = pSrc->meshindex[mi];
        s_mesh_t &dstMesh = pDst->mesh[matID];
        dstMesh.faceoffset = faceOffset;
        dstMesh.numfaces = meshResults[matID].faces.Count();
        if (dstMesh.numfaces > 0)
            memcpy(pDst->face + faceOffset, meshResults[matID].faces.Base(),
                   dstMesh.numfaces * sizeof(s_face_t));
        faceOffset += dstMesh.numfaces;
    }

    if (!g_StudioMdlContext.quiet) {
        printf("decimatemodel: %d -> %d faces (factor %.2f)\n",
               pSrc->numfaces, nTotalNewFaces, factor);
    }

    return pDst;
}


//-----------------------------------------------------------------------------
// Returns the sources associated with the various LODs based on the script commands
//-----------------------------------------------------------------------------
static void GetLODSources(CUtlVector<s_source_t *> &lods, const s_model_t *pSrcModel) {
    int nNumLODs = g_ScriptLODs.Count();
    lods.EnsureCount(nNumLODs);

    const char *pLookupName = (pSrcModel->rendermesh_name[0] != '\0')
        ? pSrcModel->rendermesh_name : pSrcModel->filename;

    char baseBuf[MAX_PATH];
    const char *pBaseName = GetModelBaseName(pLookupName, baseBuf);

    for (int lodID = 0; lodID < nNumLODs; lodID++) {
        LodScriptData_t &scriptLOD = g_ScriptLODs[lodID];

        bool bFound;
        s_source_t *pSource = GetModelLODSource(pLookupName, scriptLOD, &bFound);

        if (!pSource && !bFound) {
            // Check decimatemodel entries - decimate the full-detail source for this
            // LOD level. The base is always pSrcModel->source (LOD0 / original geometry),
            // never lods[0]: at lodID 0 lods[0] is not yet assigned (a default-constructed
            // indeterminate pointer), and at lodID > 0 lods[0] would make the factor
            // cumulative instead of "percent of the original triangle count".
            for (int j = 0; j < scriptLOD.generateLods.Count(); j++) {
                char entryBuf[MAX_PATH];
                const char *pEntryBase = GetModelBaseName(scriptLOD.generateLods[j].GetSrcName(), entryBuf);
                if (Q_stricmp(pBaseName, pEntryBase) == 0) {
                    if (pSrcModel->source && !scriptLOD.IsStrippedFromModel()) {
                        pSource = GenerateDecimatedSource(pSrcModel->source, scriptLOD.generateLods[j].m_flDecimationFactor, lodID);
                    }
                    bFound = true;
                    break;
                }
            }
        }

        if (!pSource && !bFound && scriptLOD.HasDecimateAll() && !scriptLOD.IsStrippedFromModel()) {
            // decimateallmodel: blanket-decimate any body-part model that had no
            // explicit replacemodel/removemodel/decimatemodel entry above (those take
            // priority per-mesh). This only ever runs for models in g_model[] (i.e.
            // those used by $body/$bodygroup/$model); rendermeshes used solely by
            // $collisionjoints never become models here, so they are naturally skipped.
            // The factor is applied against the full-detail source directly, so it is
            // consistent across LOD levels regardless of ordering.
            if (pSrcModel->source) {
                pSource = GenerateDecimatedSource(pSrcModel->source, scriptLOD.GetDecimateAllFactor(), lodID);
            }
            bFound = true;
        }

        if (!pSource && !bFound) {
            pSource = pSrcModel->source;
        }

        lods[lodID] = pSource;
    }
}


//-----------------------------------------------------------------------------
// Creates models to store converted data for the various LODs
//-----------------------------------------------------------------------------
void LoadLODSources() {
    g_nummodelsbeforeLOD = g_nummodels;
    for (int modelID = 0; modelID < g_nummodelsbeforeLOD; modelID++) {
        if (!Q_stricmp(g_model[modelID]->name, "blank")) {
            int nNumLODs = g_ScriptLODs.Count();
            g_model[modelID]->m_LodSources.SetCount(nNumLODs);
            for (int i = 0; i < nNumLODs; ++i) {
                g_model[modelID]->m_LodSources[i] = NULL;
            }
            continue;
        }

        GetLODSources(g_model[modelID]->m_LodSources, g_model[modelID]);
    }
}

static void ReplaceBonesRecursive(int globalBoneID, bool replaceThis,
                                  CUtlVector<CLodScriptReplacement_t> &boneReplacements,
                                  const char *replacementName) {
    if (replaceThis) {
        CLodScriptReplacement_t &boneReplacement = boneReplacements[boneReplacements.AddToTail()];
        boneReplacement.SetSrcName(g_bonetable[globalBoneID].name);
        boneReplacement.SetDstName(replacementName);
    }

    // find children and recurse.
    int i;
    for (i = 0; i < g_StudioMdlContext.numbones; i++) {
        if (g_bonetable[i].parent == globalBoneID) {
            ReplaceBonesRecursive(i, true, boneReplacements, replacementName);
        }
    }
}

static void ConvertSingleBoneTreeCollapseToReplaceBones(CLodScriptReplacement_t &boneTreeCollapse,
                                                        CUtlVector<CLodScriptReplacement_t> &boneReplacements) {
    // find the bone that we are starting with.
    int i = findGlobalBone(boneTreeCollapse.GetSrcName());
    if (i != -1) {
        ReplaceBonesRecursive(i, false, boneReplacements, g_bonetable[i].name);
        return;
    }
    MdlWarning("Couldn't find bone %s for bonetreecollapse, skipping\n", boneTreeCollapse.GetSrcName());
}

void ConvertBoneTreeCollapsesToReplaceBones() {
    int i;
    for (i = 0; i < g_ScriptLODs.Count(); i++) {
        LodScriptData_t &lod = g_ScriptLODs[i];
        int j;
        for (j = 0; j < lod.boneTreeCollapses.Count(); j++) {
            ConvertSingleBoneTreeCollapseToReplaceBones(lod.boneTreeCollapses[j],
                                                        lod.boneReplacements);
        }
    }
}

/*
static void PrintReplacedBones( LodScriptData_t &lod )
{
	int i;
	for( i = 0; i < lod.boneReplacements.Count(); i++ )
	{
		printf( "%s -> %s\n", 
			lod.boneReplacements[i].GetSrcName(), 
			lod.boneReplacements[i].GetDstName() );
	}
}
*/

void FixupReplacedBonesForLOD(LodScriptData_t &lod) {
/*
	printf( "before:\n" );
	PrintReplacedBones( lod );
*/
    bool changed;
    int i;
    int j;
    do {
        changed = false;
        for (i = 0; i < lod.boneReplacements.Count(); i++) {
            for (j = 0; j < lod.boneReplacements.Count(); j++) {
                if (i == j) {
                    continue;
                }
                if (Q_stricmp(lod.boneReplacements[i].GetSrcName(), lod.boneReplacements[j].GetDstName()) == 0) {
                    lod.boneReplacements[j].SetDstName(lod.boneReplacements[i].GetDstName());
                    changed = true;
                }
            }
        }
    } while (changed);
/*
	printf( "after:\n" );
	PrintReplacedBones( lod );
*/
}

void FixupReplacedBones() {
    int i;
    for (i = 0; i < g_ScriptLODs.Count(); i++) {
        FixupReplacedBonesForLOD(g_ScriptLODs[i]);
    }
}
