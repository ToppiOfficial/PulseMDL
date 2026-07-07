//========= Copyright © 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose:
//
// $NoKeywords: $
//
//=============================================================================//

//
// studiomdl.c: generates a studio .mdl file from a .qc script
// sources/<scriptname>.mdl.
//


#pragma warning( disable : 4244 )
#pragma warning( disable : 4237 )
#pragma warning( disable : 4305 )


#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "common/scriplib.h"
#include "studiomdl/studiomdl.h"

extern StudioMdlContext g_StudioMdlContext;

// --------------------------------------------------------------------------
// Vertex deduplication hash map
//
// Key covers all fields compared with exact equality: material, position,
// primary texcoord.  The dot-product normal check and bone-weight checks
// are handled in the linear walk of the per-bucket chain.
// Chain linkage reuses v_unify_t::next (already in the struct).
// --------------------------------------------------------------------------
struct SmdVertKey {
    int      mat;
    uint32_t px, py, pz;
    uint32_t u, v;

    bool operator==(const SmdVertKey& o) const {
        return mat == o.mat
            && px == o.px && py == o.py && pz == o.pz
            && u  == o.u  && v  == o.v;
    }
};

struct SmdVertKeyHash {
    size_t operator()(const SmdVertKey& k) const noexcept {
        // FNV-1a 64-bit
        uint64_t h = 14695981039346656037ULL;
        auto eat = [&](uint32_t x) { h = (h ^ (uint64_t)x) * 1099511628211ULL; };
        eat((uint32_t)k.mat);
        eat(k.px); eat(k.py); eat(k.pz);
        eat(k.u);  eat(k.v);
        return (size_t)h;
    }
};

static std::unordered_map<SmdVertKey, int, SmdVertKeyHash> s_smdVertMap;

// Build a key from exact-match vertex fields
static inline SmdVertKey MakeSmdVertKey(int material, const Vector& vertex, const Vector2D& texcoord) {
    SmdVertKey k;
    k.mat = material;
    memcpy(&k.px, &vertex.x, 4);
    memcpy(&k.py, &vertex.y, 4);
    memcpy(&k.pz, &vertex.z, 4);
    memcpy(&k.u,  &texcoord.x, 4);
    memcpy(&k.v,  &texcoord.y, 4);
    return k;
}

// --------------------------------------------------------------------------

int lookup_index(s_source_t *psource, int material, Vector &vertex, Vector &normal, Vector2D texcoord, int iCount,
                 const int *bones, const float *weights, int iExtras, const float *extras) {

    SmdVertKey key = MakeSmdVertKey(material, vertex, texcoord);

    auto it = s_smdVertMap.find(key);
    if (it != s_smdVertMap.end()) {
        // Walk the per-bucket chain
        int i = it->second;
        while (i >= 0) {
            if (DotProduct(g_StudioMdlContext.normal[i], normal) > normal_blend) {
                if (g_StudioMdlContext.bone[i].numbones == iCount) {
                    int j;
                    for (j = 0; j < iCount; j++) {
                        if (g_StudioMdlContext.bone[i].bone[j] != bones[j] ||
                            g_StudioMdlContext.bone[i].weight[j] != weights[j])
                            break;
                    }
                    if (j == iCount) {
                        int k;
                        for (k = 0; k < (iExtras / 2); k++) {
                            if (v_listdata[i].t[k + 1] == -1) break;
                            if (g_StudioMdlContext.texcoord[k + 1][i][0] != extras[k * 2]) break;
                            if (g_StudioMdlContext.texcoord[k + 1][i][1] != extras[k * 2 + 1]) break;
                        }
                        if (k == (iExtras / 2)) {
                            v_listdata[i].lastref = g_numvlist;
                            return i;
                        }
                    }
                }
            }
            // Advance to next entry in chain
            i = v_listdata[i].next ? (int)(v_listdata[i].next - v_listdata) : -1;
        }
    }

    // New vertex
    int i = g_numvlist;
    if (i >= MAXSTUDIOSRCVERTS) {
        MdlError("too many indices in source: \"%s\"\n", psource->filename);
    }

    VectorCopy(vertex, g_StudioMdlContext.vertex[i]);
    VectorCopy(normal, g_StudioMdlContext.normal[i]);
    Vector2Copy(texcoord, g_StudioMdlContext.texcoord[0][i]);

    g_StudioMdlContext.bone[i].numbones = iCount;
    for (int j = 0; j < iCount; j++) {
        g_StudioMdlContext.bone[i].bone[j] = bones[j];
        g_StudioMdlContext.bone[i].weight[j] = weights[j];
    }

    v_listdata[i].v = i;
    v_listdata[i].m = material;
    v_listdata[i].n = i;
    v_listdata[i].t[0] = i;

    for (int j = 1; j < MAXSTUDIOTEXCOORDS; ++j)
        v_listdata[i].t[j] = -1;

    for (int j = 0; j < (iExtras / 2); j++) {
        g_StudioMdlContext.texcoord[j + 1][i][0] = extras[j * 2];
        g_StudioMdlContext.texcoord[j + 1][i][1] = extras[j * 2 + 1];
        v_listdata[i].t[j + 1] = i;
    }

    v_listdata[i].lastref = g_numvlist;

    // Prepend to hash bucket chain
    if (it != s_smdVertMap.end()) {
        v_listdata[i].next = &v_listdata[it->second];
        it->second = i;
    } else {
        v_listdata[i].next = nullptr;
        s_smdVertMap.emplace(key, i);
    }

    g_numvlist = i + 1;
    return i;
}

// --------------------------------------------------------------------------
// Fast single-pass face-line tokenizer
//
// Replaces sscanf + repeated GetNextFaceItem (strchr) calls with one linear
// scan over the line buffer using strtol/strtof.
// --------------------------------------------------------------------------
static inline const char *SkipSpaces(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static inline float ParseFloat(const char *&p) {
    char *end;
    float v = strtof(p, &end);
    p = end;
    return v;
}

static inline int ParseInt(const char *&p) {
    char *end;
    int v = (int)strtol(p, &end, 10);
    p = end;
    return v;
}

void ParseFaceData(s_source_t *psource, int material, s_face_t *pFace) {
    int   index[3];
    int   iCount, bones[MAXSTUDIOSRCBONES];
    float weights[MAXSTUDIOSRCBONES];
    int   iExtras;
    float extras[(MAXSTUDIOTEXCOORDS - 1) * 2];

    for (int j = 0; j < 3; j++) {
        memset(g_StudioMdlContext.szLine, 0, sizeof(g_StudioMdlContext.szLine));

        if (!GetLineInput()) {
            MdlError("%s: error on line %d: %s",
                     g_StudioMdlContext.szFilename,
                     g_StudioMdlContext.iLinecount,
                     g_StudioMdlContext.szLine);
        }

        iCount  = 0;
        iExtras = 0;

        const char *p = SkipSpaces(g_StudioMdlContext.szLine);

        // bone_index  px py pz  nx ny nz  u v
        int bone = ParseInt(p);
        p = SkipSpaces(p);

        Vector pos, nrm;
        Vector2D t;
        pos[0] = ParseFloat(p); p = SkipSpaces(p);
        pos[1] = ParseFloat(p); p = SkipSpaces(p);
        pos[2] = ParseFloat(p); p = SkipSpaces(p);
        nrm[0] = ParseFloat(p); p = SkipSpaces(p);
        nrm[1] = ParseFloat(p); p = SkipSpaces(p);
        nrm[2] = ParseFloat(p); p = SkipSpaces(p);
        t[0]   = ParseFloat(p); p = SkipSpaces(p);
        t[1]   = ParseFloat(p); p = SkipSpaces(p);

        if (bone < 0 || bone >= psource->numbones) {
            MdlError("bogus bone index\n%d %s :\n%s",
                     g_StudioMdlContext.iLinecount,
                     g_StudioMdlContext.szFilename,
                     g_StudioMdlContext.szLine);
        }

        scale_vertex(pos);

        // Optional bone weight list
        if (*p && *p != '\n' && *p != '\r') {
            iCount = ParseInt(p);
            p = SkipSpaces(p);
            int actualCount = (iCount < MAXSTUDIOSRCBONES) ? iCount : MAXSTUDIOSRCBONES;
            for (int k = 0; k < actualCount; k++) {
                if (!(*p)) {
                    MdlError("Bone ID %d not found\n%d %s :\n%s",
                             k, g_StudioMdlContext.iLinecount,
                             g_StudioMdlContext.szFilename,
                             g_StudioMdlContext.szLine);
                }
                bones[k]   = ParseInt(p);   p = SkipSpaces(p);
                weights[k] = ParseFloat(p); p = SkipSpaces(p);
            }

            // SMD v3 extra texcoords
            if (psource->version >= 3 && *p && *p != '\n' && *p != '\r') {
                iExtras = ParseInt(p);
                p = SkipSpaces(p);
                iExtras = MIN(iExtras, (MAXSTUDIOTEXCOORDS - 1) * 2);
                for (int e = 0; e < iExtras; e++) {
                    if (!(*p)) {
                        MdlError("Extra data item %d not found\n%d %s :\n%s",
                                 e, g_StudioMdlContext.iLinecount,
                                 g_StudioMdlContext.szFilename,
                                 g_StudioMdlContext.szLine);
                    }
                    extras[e] = ParseFloat(p);
                    p = SkipSpaces(p);
                }
            }
        }

        // Invert V
        t[1] = 1.0f - t[1];

        if (iCount == 0) {
            iCount    = 1;
            bones[0]  = bone;
            weights[0] = 1.0f;
        } else {
            // keep up to the source-stage cap; the clip to MAXSTUDIOBONEWEIGHTS
            // happens after bone collapse (BalanceGlobalBoneWeights)
            iCount = SortAndBalanceBones(iCount, MAXSTUDIOSRCBONEWEIGHTS, bones, weights);
        }

        index[j] = lookup_index(psource, material, pos, nrm, t, iCount, bones, weights, iExtras, extras);
    }

    pFace->a = index[0];
    pFace->b = index[2];
    pFace->c = index[1];
    Assert(((pFace->a & 0xF0000000) == 0) && ((pFace->b & 0xF0000000) == 0) &&
           ((pFace->c & 0xF0000000) == 0));
}

void Grab_Triangles(s_source_t *psource) {
    int i;
    Vector vmin, vmax;

    vmin[0] = vmin[1] = vmin[2] = 99999;
    vmax[0] = vmax[1] = vmax[2] = -99999;

    g_StudioMdlContext.numfaces = 0;
    g_numvlist = 0;

    // Reset vertex dedup map for this mesh
    s_smdVertMap.clear();
    s_smdVertMap.reserve(65536);

    //
    // load the base triangles
    //
    int texture;
    int material;
    char texturename[MAX_PATH];

    while (1) {
        if (!GetLineInput())
            break;

        // check for end
        if (IsEnd(g_StudioMdlContext.szLine))
            break;

        // Look for extra junk that we may want to avoid...
        int nLineLength = strlen(g_StudioMdlContext.szLine);
        if (nLineLength >= sizeof(texturename)) {
            MdlWarning("Unexpected data at line %d, (need a texture name) ignoring...\n", g_StudioMdlContext.iLinecount);
            continue;
        }

        // strip off trailing smag
        strncpy(texturename, g_StudioMdlContext.szLine, sizeof(texturename) - 1);
        for (i = strlen(texturename) - 1; i >= 0 && !isgraph(texturename[i]); i--) {
        }
        texturename[i + 1] = '\0';

        // funky texture overrides
        for (i = 0; i < numrep; i++) {
            if (sourcetexture[i][0] == '\0') {
                strcpy(texturename, defaulttexture[i]);
                break;
            }
            if (stricmp(texturename, sourcetexture[i]) == 0) {
                strcpy(texturename, defaulttexture[i]);
                break;
            }
        }

        if (texturename[0] == '\0') {
            // weird source problem, skip them
            GetLineInput();
            GetLineInput();
            GetLineInput();
            continue;
        }

        if (stricmp(texturename, "null.bmp") == 0 || stricmp(texturename, "null.tga") == 0 ||
            stricmp(texturename, "debug/debugempty") == 0) {
            // skip all faces with the null texture on them.
            GetLineInput();
            GetLineInput();
            GetLineInput();
            continue;
        }

        texture = LookupTexture(texturename, (psource->version == 2));
        psource->texmap[texture] = texture;    // hack, make it 1:1
        material = UseTextureAsMaterial(texture);

        s_face_t f;
        ParseFaceData(psource, material, &f);

        // remove degenerate triangles
        if (f.a == f.b || f.b == f.c || f.a == f.c) {
            continue;
        }

        g_StudioMdlContext.src_uface[g_StudioMdlContext.numfaces] = f;
        g_StudioMdlContext.face[g_StudioMdlContext.numfaces].material = material;
        g_StudioMdlContext.numfaces++;
    }

    for (int i = 0; i < MAXSTUDIOTEXCOORDS; ++i) {
        if (g_StudioMdlContext.texcoord[i].Count()) {
            g_StudioMdlContext.numtexcoords[i] = g_numvlist;
        }
    }

    // Release map memory after we're done building this mesh
    s_smdVertMap.clear();
    s_smdVertMap.rehash(0);

    BuildIndividualMeshes(psource);
}


int Load_SMD(s_source_t *psource) {
    char cmd[1024];
    int option;

    // Reset smdVersion
    psource->version = 1;

    if (!OpenGlobalFile(psource->filename))
        return 0;

    if (!g_StudioMdlContext.quiet) {
        printf("SMD MODEL %s\n", psource->filename);
    }

    g_StudioMdlContext.iLinecount = 0;

    while (GetLineInput()) {
        int numRead = sscanf(g_StudioMdlContext.szLine, "%s %d", cmd, &option);

        // Blank line
        if ((numRead == EOF) || (numRead == 0))
            continue;

        if (stricmp(cmd, "version") == 0) {
            if (option < 1 || option > 3) {
                MdlError("bad version\n");
            }
            psource->version = option;
        } else if (stricmp(cmd, "nodes") == 0) {
            psource->numbones = Grab_Nodes(psource->localBone);
        } else if (stricmp(cmd, "skeleton") == 0) {
            Grab_Animation(psource, "BindPose");
        } else if (stricmp(cmd, "triangles") == 0) {
            Grab_Triangles(psource);
        } else if (stricmp(cmd, "vertexanimation") == 0) {
            Grab_Vertexanimation(psource, "BindPose");
        } else if ((strncmp(cmd, "//", 2) == 0) || (strncmp(cmd, ";", 1) == 0) || (strncmp(cmd, "#", 1) == 0)) {
            ProcessSourceComment(psource, cmd);
            continue;
        } else {
            MdlWarning("unknown studio command \"%s\"\n", cmd);
        }
    }
    fclose(g_StudioMdlContext.fpInput);

    return 1;
}
