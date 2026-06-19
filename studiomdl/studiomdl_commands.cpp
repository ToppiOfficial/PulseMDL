//
// Created by RED on 15.05.2024.
//

#include <minmax.h>
#include "studiomdl_commands.h"
#include "datamodel/idatamodel.h"
#include "mdlobjects/dmeboneflexdriver.h"
#include "common/scriplib.h"
#include "studiomdl/collisionmodel.h"
#include "studiomdl_errors.h"
#include "movieobjects/movieobjects.h"
#include "movieobjects/dmemdlmakefile.h"
#include "tier1/fmtstr.h"
#include "bspflags.h"

#ifdef WIN32
    #undef strdup
    #define strdup _strdup
    #define strlwr _strlwr
#endif


extern StudioMdlContext g_StudioMdlContext;

//-----------------------------------------------------------------------------
// $rendermesh: named filtered views of DMX files
//-----------------------------------------------------------------------------

#define MAX_RENDERMESH_DEFS             512
#define MAX_RENDERMESH_OVERRIDES        256
#define MAX_RENDERMESH_MATERIAL_REMOVES 256

struct s_rendermesh_override_t {
    char meshName[MAXSTUDIONAME];
};

struct s_rendermesh_def_t {
    char name[MAXSTUDIONAME];
    char filename[MAX_PATH];
    bool defaultState;
    s_rendermesh_override_t overrides[MAX_RENDERMESH_OVERRIDES];
    int numOverrides;
    char removeMaterials[MAX_RENDERMESH_MATERIAL_REMOVES][MAXSTUDIONAME];
    int numRemoveMaterials;
    bool noFacial;          // strip all flex controllers, flex rules and deltas from this clone
    bool used;
    s_source_t *pOwnedSource;
};

static s_rendermesh_def_t g_rendermeshDefs[MAX_RENDERMESH_DEFS];
static int g_numRendermeshDefs = 0;

static s_rendermesh_def_t *FindRenderMeshDef(const char *name) {
    for (int i = 0; i < g_numRendermeshDefs; i++)
        if (!Q_strcmp(g_rendermeshDefs[i].name, name))
            return &g_rendermeshDefs[i];
    return nullptr;
}

static s_source_t *CloneSourceGeometry(s_source_t *pOrig);
static void RebuildGeometryFromKeepMask(s_source_t *pSource, const CUtlVector<bool> &keepFace);
static void ApplyDmeMeshFilter(s_source_t *pSource, const CUtlVector<CUtlString> &meshNames, bool isIsolate);
static void ApplyMaterialRemoveFilter(s_source_t *pSource, s_rendermesh_def_t *pDef);
static void ApplyNoFacialFilter(s_source_t *pSource);
static void ApplyRenderMeshFilter(s_source_t *pSource, s_rendermesh_def_t *pDef);

//-----------------------------------------------------------------------------
// Parse contents flags
//-----------------------------------------------------------------------------
static void ParseContents(int *pAddFlags, int *pRemoveFlags) {
    *pAddFlags = 0;
    *pRemoveFlags = 0;
    do {
        GetToken(false);

        if (!stricmp(token, "grate")) {
            *pAddFlags |= CONTENTS_GRATE;
            *pRemoveFlags |= CONTENTS_SOLID;
        } else if (!stricmp(token, "ladder")) {
            *pAddFlags |= CONTENTS_LADDER;
        } else if (!stricmp(token, "solid")) {
            *pAddFlags |= CONTENTS_SOLID;
        } else if (!stricmp(token, "monster")) {
            *pAddFlags |= CONTENTS_MONSTER;
        } else if (!stricmp(token, "debris")) {
            *pAddFlags |= CONTENTS_DEBRIS;
        } else if (!stricmp(token, "notsolid")) {
            *pRemoveFlags |= CONTENTS_SOLID;
        } else {
            // Numeric input: arbitrary combinations of Contents flags may be set
            // via a raw integer (decimal or 0x-prefixed hex). See:
            // https://developer.valvesoftware.com/wiki/BSP_flags_(Source)#Contents_flags
            char *pEnd = nullptr;
            long nValue = strtol(token, &pEnd, 0);
            if (pEnd != token && *pEnd == '\0') {
                *pAddFlags |= (int)nValue;
            } else {
                MdlError("Unknown $contents value \"%s\"\n", token);
            }
        }
    } while (TokenAvailable());
}

void ProcessModelName(const char *pModelName) {
    // Abort early if modelname is too big

    const int nModelNameLen = Q_strlen(pModelName);
    char *pTmpBuf = reinterpret_cast< char * >( _alloca((nModelNameLen + 1) * sizeof(char)));
    Q_StripExtension(pModelName, pTmpBuf, nModelNameLen + 1);
    // write.cpp strips extension then adds .mdl and writes that into studiohdr_t::name

    // Need one for sizeof operation to work...
    studiohdr_t shdr;
    if (Q_strlen(pTmpBuf) + 4 >= (sizeof(g_outname) / sizeof(g_outname[0]))) {
        MdlError("Model Name \"%s.mdl\" Too Big, %d Characters, Max %d Characters\n",
                 pTmpBuf,
                 Q_strlen(pTmpBuf) + 4,
                 (sizeof(g_outname) / sizeof(g_outname)) - 1);
    }

    g_StudioMdlContext.bHasModelName = true;
    Q_strncpy(g_outname, pModelName, sizeof(g_outname));
}

//-----------------------------------------------------------------------------
// Parse the studio options from a .qc file
//-----------------------------------------------------------------------------
bool ParseOptionStudio(CDmeSourceSkin *pSkin) {
    if (!GetToken(false))
        return false;

    pSkin->SetRelativeFileName(token);
    while (TokenAvailable()) {
        // Stop at the '{' that opens a following model-option block WITHOUT consuming it.
        // The source load that happens after this (ProcessOptionStudio) re-enters the
        // tokenizer for DMX flex rules, which would clobber an UnGetToken'd '{'. Leaving
        // the brace physically in the stream lets Option_Model find it reliably.
        {
            char peek[MAXTOKEN];
            if (PeekToken(peek, sizeof(peek)) && !Q_stricmp("{", peek))
                break;
        }
        GetToken(false);
        if (!Q_stricmp("reverse", token)) {
            pSkin->m_bFlipTriangles = true;
            continue;
        }

        if (!Q_stricmp("scale", token)) {
            GetToken(false);
            pSkin->m_flScale = verify_atof(token);
            continue;
        }

        if (!Q_stricmp("faces", token)) {
            GetToken(false);
            GetToken(false);
            continue;
        }

        if (!Q_stricmp("bias", token)) {
            GetToken(false);
            continue;
        }

        if (!Q_stricmp("subd", token)) {
            pSkin->m_bQuadSubd = true;
            continue;
        }

        MdlError("unknown command \"%s\"\n", token);
        return false;
    }
    return true;
}

//-----------------------------------------------------------------------------
// Parse the body command from a .qc file
//-----------------------------------------------------------------------------
void ProcessOptionStudio(s_model_t *pmodel, const char *pFullPath, float flScale, bool bFlipTriangles, bool bQuadSubd) {
    // If the name matches a $rendermesh definition, resolve it to the underlying DMX file.
    s_rendermesh_def_t *pRenderMeshDef = FindRenderMeshDef(pFullPath);
    Q_strncpy(pmodel->filename, pRenderMeshDef ? pRenderMeshDef->filename : pFullPath, sizeof(pmodel->filename));
    if (pRenderMeshDef) {
        Q_strncpy(pmodel->rendermesh_name, pFullPath, sizeof(pmodel->rendermesh_name));
    }

    if (flScale != 0.0f) {
        pmodel->scale = g_currentscale = flScale;
    } else {
        pmodel->scale = g_currentscale = g_defaultscale;
    }

    g_pCurrentModel = pmodel;

    if (!pRenderMeshDef) {
        pmodel->source = Load_Source(pmodel->filename, "", bFlipTriangles, true);
    } else {
        if (!pRenderMeshDef->pOwnedSource) {
            // First use of this def: load raw from cache (read-only), clone it immediately
            // so the cache is never mutated, then process and filter on the private copy.
            s_source_t *pRaw = Load_Source(pmodel->filename, "", bFlipTriangles, true);
            if (pRaw) {
                s_source_t *pOwned = CloneSourceGeometry(pRaw);
                ApplyRenderMeshFilter(pOwned, pRenderMeshDef);
                pRenderMeshDef->pOwnedSource = pOwned;
            }
        }
        // Each model use gets its own clone of the pre-processed owned source.
        pmodel->source = pRenderMeshDef->pOwnedSource
            ? CloneSourceGeometry(pRenderMeshDef->pOwnedSource)
            : nullptr;
    }

    g_pCurrentModel = NULL;
    g_currentscale = g_defaultscale;
}

//-----------------------------------------------------------------------------
// Process a body command
//-----------------------------------------------------------------------------
void ProcessCmdBody(const char *pFullPath, const char *pBodyPartName, float flScale, bool bFlipTriangles, bool bQuadSubd) {
    g_bodypart[g_numbodyparts].base = 1;
    if (g_numbodyparts != 0) {
        g_bodypart[g_numbodyparts].base =
                g_bodypart[g_numbodyparts - 1].base * g_bodypart[g_numbodyparts - 1].nummodels;
    }
    Q_strncpy(g_bodypart[g_numbodyparts].name, pBodyPartName, sizeof(g_bodypart[g_numbodyparts].name));

    g_model[g_nummodels] = (s_model_t *) calloc(1, sizeof(s_model_t));
    g_bodypart[g_numbodyparts].pmodel[0] = g_model[g_nummodels];
    g_bodypart[g_numbodyparts].nummodels = 1;

    ProcessOptionStudio(g_model[g_nummodels], pFullPath, flScale, bFlipTriangles, bQuadSubd);

    // Body command should add any flex commands in the source loaded
    PostProcessSource(g_model[g_nummodels]->source, g_nummodels);

    g_nummodels++;
    g_numbodyparts++;
}


//-----------------------------------------------------------------------------
// Purpose: Handle the $boneflexdriver command
// QC: $boneflexdriver <bone name> <tx|ty|tz> <flex controller name> <min> <max>
//-----------------------------------------------------------------------------
void Cmd_BoneFlexDriver() {
    CDisableUndoScopeGuard undoDisable;    // Turn of Dme undo

    // Find or create the DmeBoneFlexDriverList
    CDmeBoneFlexDriverList *pDmeBoneFlexDriverList = GetElement<CDmeBoneFlexDriverList>(g_StudioMdlContext.hDmeBoneFlexDriverList);
    if (!pDmeBoneFlexDriverList) {
        pDmeBoneFlexDriverList = CreateElement<CDmeBoneFlexDriverList>("boneDriverFlexList", DMFILEID_INVALID);
        if (pDmeBoneFlexDriverList) {
            g_StudioMdlContext.hDmeBoneFlexDriverList = pDmeBoneFlexDriverList->GetHandle();
        }
    }

    if (!pDmeBoneFlexDriverList) {
        MdlError("%s: Couldn't find or create DmeBoneDriverFlexList\n", "$boneflexdriver");
        return;
    }

    // <bone name>
    GetToken(false);
    CDmeBoneFlexDriver *pDmeBoneFlexDriver = pDmeBoneFlexDriverList->FindOrCreateBoneFlexDriver(token);
    if (!pDmeBoneFlexDriver) {
        MdlError("%s: Couldn't find or create DmeBoneFlexDriver for bone \"%s\"\n", "$boneflexdriver", token);
        return;
    }

    // <tx|ty|tz|rx|ry|rz>
    GetToken(false);
    const char *ppszComponentTypeList[] = {"tx", "ty", "tz"};
    int nBoneComponent = -1;
    for (int i = 0; i < ARRAYSIZE(ppszComponentTypeList); ++i) {
        if (StringHasPrefix(token, ppszComponentTypeList[i])) {
            nBoneComponent = i;
            break;
        }
    }

    if (nBoneComponent < STUDIO_BONE_FLEX_TX || nBoneComponent > STUDIO_BONE_FLEX_TZ) {
        TokenError("%s: Invalid bone component, must be one of <tx|ty|tz>\n", "$boneflexdriver");
        return;
    }

    // <flex controller name>
    GetToken(false);
    CDmeBoneFlexDriverControl *pDmeBoneFlexDriverControl = pDmeBoneFlexDriver->FindOrCreateControl(token);
    if (!pDmeBoneFlexDriverControl) {
        MdlError("%s: Couldn't find or create DmeBoneFlexDriverControl for bone \"%s\"\n", "$boneflexdriver", token);
        return;
    }

    pDmeBoneFlexDriverControl->m_nBoneComponent = nBoneComponent;

    // <min>
    GetToken(false);
    pDmeBoneFlexDriverControl->m_flMin = verify_atof(token);

    // <max>
    GetToken(false);
    pDmeBoneFlexDriverControl->m_flMax = verify_atof(token);
}

void Cmd_PoseParameter() {
    if (g_numposeparameters >= MAXSTUDIOPOSEPARAM) {
        TokenError("too many pose parameters (max %d)\n", MAXSTUDIOPOSEPARAM);
    }

    GetToken(false); //[wills] unless you want a pose parameter named "poseparameter", should probably GetToken here

    int i = LookupPoseParameter(token);
    strcpyn(g_pose[i].name, token);

    if (TokenAvailable()) {
        // min
        GetToken(false);
        g_pose[i].min = verify_atof(token);
    }

    if (TokenAvailable()) {
        // max
        GetToken(false);
        g_pose[i].max = verify_atof(token);
    }

    while (TokenAvailable()) {
        GetToken(false);

        if (!Q_stricmp(token, "wrap")) {
            g_pose[i].flags |= STUDIO_LOOPING;
            g_pose[i].loop = g_pose[i].max - g_pose[i].min;
        } else if (!Q_stricmp(token, "loop")) {
            g_pose[i].flags |= STUDIO_LOOPING;
            GetToken(false);
            g_pose[i].loop = verify_atof(token);
        }
    }
}

void Cmd_OverrideMaterial() {
    char to[256];

    GetToken(false);
    strcpy(to, token);

    Msg("$overridematerial is replacing ALL material references with %s.\n", to);

    int i;
    for (i = 0; i < g_numtextures; i++) {
        strcpy(g_texture[i].name, to);
    }
}

void Cmd_RenameMaterial() {
    char from[256];
    char to[256];

    GetToken(false);
    strcpy(from, token);

    GetToken(false);
    strcpy(to, token);

    int i;
    for (i = 0; i < g_numtextures; i++) {
        if (stricmp(g_texture[i].name, from) == 0) {
            strcpy(g_texture[i].name, to);
            return;
        }
    }

    for (i = 0; i < g_numtextures; i++) {
        if (V_stristr(g_texture[i].name, from)) {
            Msg("$renamematerial fell back to partial match: Replacing %s with %s.\n", g_texture[i].name, to);
            strcpy(g_texture[i].name, to);
            return;
        }
    }

    MdlError("unknown material \"%s\" in rename\n", from);
}

void Cmd_Eyeposition() {
// rotate points into frame of reference so g_model points down the positive x
// axis
    //	FIXME: these coords are bogus
    GetToken(false);
    eyeposition[1] = verify_atof(token);

    GetToken(false);
    eyeposition[0] = -verify_atof(token);

    GetToken(false);
    eyeposition[2] = verify_atof(token);
}

void Cmd_MaxEyeDeflection() {
    GetToken(false);
    g_flMaxEyeDeflection = cosf(verify_atof(token) * M_PI / 180.0f);
}

//-----------------------------------------------------------------------------
// Cmd_AddSearchDir: add the custom defined path to an array that we will add to the search paths
//-----------------------------------------------------------------------------
void Cmd_AddSearchDir() {
    GetToken(false);
    if (!g_StudioMdlContext.quiet) {
        printf("New search path: %s\n", token);
    }
    CmdLib_AddNewSearchPath(token);

    if (g_numAddSearchDirs >= ARRAYSIZE(g_addSearchDirs)) {
        MdlWarning("$addsearchdir: max search dirs (%d) reached, ignoring for source loader\n", (int)ARRAYSIZE(g_addSearchDirs));
        return;
    }

    char path[MAX_PATH];
    if (Q_IsAbsolutePath(token)) {
        Q_strncpy(path, token, sizeof(path));
    } else {
        Q_snprintf(path, sizeof(path), "%s%s", cddir[0], token);
    }

    int len = strlen(path);
    if (len > 0 && path[len - 1] != '/' && path[len - 1] != '\\') {
        Q_strncat(path, "/", sizeof(path), COPY_ALL_CHARACTERS);
    }
    Q_FixSlashes(path);

    Q_strncpy(g_addSearchDirs[g_numAddSearchDirs++], path, MAX_PATH);
}

//-----------------------------------------------------------------------------
// Cmd_Illumposition
//-----------------------------------------------------------------------------
void Cmd_Illumposition() {
    GetToken(false);
    illumposition[0] = verify_atof(token);

    GetToken(false);
    illumposition[1] = verify_atof(token);

    GetToken(false);
    illumposition[2] = verify_atof(token);

    if (TokenAvailable()) {
        GetToken(false);

        strncpy(g_attachment[g_numattachments].name, "__illumPosition", sizeof(g_attachment[g_numattachments].name));
        strncpy(g_attachment[g_numattachments].bonename, token, sizeof(g_attachment[g_numattachments].bonename));
        AngleMatrix(QAngle(0, 0, 0), illumposition, g_attachment[g_numattachments].local);
        g_attachment[g_numattachments].type |= IS_RIGID;

        g_illumpositionattachment = g_numattachments + 1;
        ++g_numattachments;
    } else {
        g_illumpositionattachment = 0;

        // rotate points into frame of reference so
        // g_model points down the positive x axis
        // FIXME: these coords are bogus
        float flTemp = illumposition[0];
        illumposition[0] = -illumposition[1];
        illumposition[1] = flTemp;
    }

    illumpositionset = true;
}

//-----------------------------------------------------------------------------
// Parse Cmd_Modelname
//-----------------------------------------------------------------------------
void Cmd_Modelname() {
    GetToken(false);
    if (token[0] == '/' || token[0] == '\\') {
        MdlWarning("$modelname key has slash as first character. Removing.\n");
        ProcessModelName(&token[1]);
    } else {
        ProcessModelName(token);
    }
}

void Cmd_InternalName() {
    GetToken(false);
    Q_strncpy(g_szInternalName, token, sizeof(g_szInternalName));
}


void Cmd_PreserveTriangleOrder() {
    g_StudioMdlContext.preserveTriangleOrder = true;
}

void Cmd_Autocenter() {
    g_centerstaticprop = true;
}


//-----------------------------------------------------------------------------
// Parse + process the studio options from a .qc file
//-----------------------------------------------------------------------------
void Option_Studio(s_model_t *pmodel) {
    CDmeSourceSkin *pSourceSkin = CreateElement<CDmeSourceSkin>("", DMFILEID_INVALID);

    // Set defaults
    pSourceSkin->m_flScale = g_defaultscale;
    pSourceSkin->m_bQuadSubd = false;
    pSourceSkin->m_bFlipTriangles = false;

    if (ParseOptionStudio(pSourceSkin)) {
        ProcessOptionStudio(pmodel, pSourceSkin->GetRelativeFileName(), pSourceSkin->m_flScale,
                            pSourceSkin->m_bFlipTriangles, pSourceSkin->m_bQuadSubd);
    }
    DestroyElement(pSourceSkin);
}

int Option_Blank() {
    g_model[g_nummodels] = (s_model_t *) calloc(1, sizeof(s_model_t));

    g_source[g_numsources] = (s_source_t *) calloc(1, sizeof(s_source_t));
    g_model[g_nummodels]->source = g_source[g_numsources];
    g_numsources++;

    g_bodypart[g_numbodyparts].pmodel[g_bodypart[g_numbodyparts].nummodels] = g_model[g_nummodels];

    strcpyn(g_model[g_nummodels]->name, "blank");

    g_bodypart[g_numbodyparts].nummodels++;
    g_nummodels++;
    return 0;
}

void Option_Model(int imodel); // forward declaration - defined near Cmd_Model

void Cmd_Bodygroup() {
    int is_started = 0;

    if (!GetToken(false))
        return;

    g_bodypart[g_numbodyparts].base = 1;
    if (g_numbodyparts != 0) {
        g_bodypart[g_numbodyparts].base =
                g_bodypart[g_numbodyparts - 1].base * g_bodypart[g_numbodyparts - 1].nummodels;
    }
    strcpyn(g_bodypart[g_numbodyparts].name, token);

    do {
        GetToken(true);
        if (endofscript)
            return;
        else if (token[0] == '{') {
            is_started = 1;
        } else if (token[0] == '}') {
            break;
        } else if (stricmp("studio", token) == 0) {
            g_model[g_nummodels] = (s_model_t *) calloc(1, sizeof(s_model_t));
            g_bodypart[g_numbodyparts].pmodel[g_bodypart[g_numbodyparts].nummodels] = g_model[g_nummodels];
            g_bodypart[g_numbodyparts].nummodels++;

            Option_Studio(g_model[g_nummodels]);

            // Body command should add any flex commands in the source loaded
            PostProcessSource(g_model[g_nummodels]->source, g_nummodels);

            g_nummodels++;
        } else if (stricmp("blank", token) == 0) {
            Option_Blank();
        } else if (stricmp("$model", token) == 0) {
            if (!GetToken(false))
                MdlError("$model inside $bodygroup: expected model name\n");
            g_model[g_nummodels] = (s_model_t *) calloc(1, sizeof(s_model_t));
            strcpyn(g_model[g_nummodels]->name, token);

            g_bodypart[g_numbodyparts].pmodel[g_bodypart[g_numbodyparts].nummodels] = g_model[g_nummodels];
            g_bodypart[g_numbodyparts].nummodels++;

            Option_Studio(g_model[g_nummodels]);

            if (g_model[g_nummodels]->source) {
                AddBodyFlexData(g_model[g_nummodels]->source, g_nummodels);
                AddBodyAttachments(g_model[g_nummodels]->source);
            }

            Option_Model(g_nummodels);

            g_nummodels++;
        } else {
            MdlError("unknown bodygroup option: \"%s\"\n", token);
        }
    } while (1);

    g_numbodyparts++;
    return;
}

void Cmd_AppendBlankBodygroup() {
    // stick a blank bodygroup on the end of the current part
    g_numbodyparts--;
    Option_Blank();
    g_numbodyparts++;
    return;
}

void Cmd_BodygroupPreset() {
    if (!GetToken(false))
        return;

    // make sure this name is unused
    for (int i = 0; i < g_numbodygrouppresets; i++) {
        if (!V_strcmp(token, g_bodygrouppresets[i].name)) {
            MdlError("Error: bodygroup preset \"%s\" already exists.\n", token);
            return;
        }
    }

    s_bodygrouppreset_t newpreset;
    V_strcpy_safe(newpreset.name, token);

    int nAccumValue = 0;
    int nAccumMask = 0;

    do {
        GetToken(true);

        if (endofscript) {
            return;
        } else if (token[0] == '{') {

        } else if (token[0] == '}') {
            break;
        } else {
            //gather up name:value pairs into a baked bodygroup value and mask

            bool bFoundPart = false;
            for (int i = 0; i < g_numbodyparts; i++) {
                if (!V_strcmp(g_bodypart[i].name, token)) {
                    GetToken(true);

                    int iValue = atoi(token);
                    if (iValue >= 0 && iValue < g_bodypart[i].nummodels) {

                        int iCurrentVal = (nAccumValue / g_bodypart[i].base) % g_bodypart[i].nummodels;
                        nAccumValue = (nAccumValue - (iCurrentVal * g_bodypart[i].base) +
                                       (iValue * g_bodypart[i].base));

                        int iCurrentMask = (nAccumMask / g_bodypart[i].base) % g_bodypart[i].nummodels;
                        nAccumMask = (nAccumMask - (iCurrentMask * g_bodypart[i].base) + (1 * g_bodypart[i].base));

                    } else {
                        MdlError(
                                "Error: can't assign value \"%i\" to bodygroup preset \"%s\" (out of available range).\n",
                                iValue, newpreset.name);
                        return;
                    }

                    bFoundPart = true;
                }
            }

            if (!bFoundPart) {
                MdlError("Error: can't find any bodygroups named \"%s\".\n", token);
                return;
            }

        }
    } while (1);

    newpreset.iValue = nAccumValue;
    newpreset.iMask = nAccumMask;

    //Msg( "Built bodygroup preset: %s, value: %i, mask:%i\n", newpreset.name, newpreset.iValue, newpreset.iMask );

    g_bodygrouppresets.AddToTail(newpreset);
    g_numbodygrouppresets++;
}

//-----------------------------------------------------------------------------
// Parse the body command from a .qc file
//-----------------------------------------------------------------------------
void Cmd_Body() {
    if (!GetToken(false))
        return;

    CDmeSourceSkin *pSourceSkin = CreateElement<CDmeSourceSkin>("", DMFILEID_INVALID);

    // Set defaults
    pSourceSkin->m_flScale = g_defaultscale;

    pSourceSkin->m_SkinName = token;
    if (ParseOptionStudio(pSourceSkin)) {
        ProcessCmdBody(pSourceSkin->GetRelativeFileName(), pSourceSkin->m_SkinName.Get(),
                       pSourceSkin->m_flScale, pSourceSkin->m_bFlipTriangles, pSourceSkin->m_bQuadSubd);
    }
    DestroyElement(pSourceSkin);
}

#ifdef MDLCOMPILE
//-----------------------------------------------------------------------------
// Add attachments from the s_source_t that aren't already present in the
// global attachment list.  At this point, the attachments aren't linked
// to the bone, but since that is done by string matching on the bone name
// the test for an attachment being a duplicate is still valid this early.
// Only doing it this way for mdlcompile though.
//-----------------------------------------------------------------------------
void AddBodyAttachments( s_source_t *pSource )
{
    for ( int i = 0; i < pSource->m_Attachments.Count(); ++i )
    {
        const s_attachment_t &sourceAtt = pSource->m_Attachments[i];

        bool bDuplicate = false;

        for ( int j = 0; j < g_numattachments; ++j )
        {
            if ( sourceAtt == g_attachment[j] )
            {
                bDuplicate = true;
                break;
            }
        }

        if ( bDuplicate )
            continue;

        if ( g_numattachments >= ARRAYSIZE( g_attachment ) )
        {
            MdlWarning( "Too Many Attachments (Max %d), Ignoring Attachment %s:%s\n",
                ARRAYSIZE( g_attachment ), pSource->filename, pSource->m_Attachments[i].name );
            continue;;
        }

        memcpy( &g_attachment[g_numattachments], &( pSource->m_Attachments[i] ), sizeof( s_attachment_t ) );
        ++g_numattachments;
    }
}
#else

//-----------------------------------------------------------------------------
// Add all attachments from the source to the global attachment list
// stopping when the g_attachment array is full.  Duplicate attachments
// will be purged later after they are linked to bones
//-----------------------------------------------------------------------------
void AddBodyAttachments(s_source_t *pSource) {
    for (int i = 0; i < pSource->m_Attachments.Count(); ++i) {
        if (g_numattachments >= g_attachment.size()) {
            MdlWarning("Too Many Attachments (Max %d), Ignoring Attachment %s:%s\n",
                       g_attachment.size(), pSource->filename, pSource->m_Attachments[i].name);
            continue;;
        }

        memcpy(&g_attachment[g_numattachments], &(pSource->m_Attachments[i]), sizeof(s_attachment_t));
        ++g_numattachments;
    }
}

#endif

static int FindFlexControllerByName(const char *name) {
    for (int i = 0; i < g_numflexcontrollers; ++i) {
        if (!Q_stricmp(g_flexcontroller[i].name, name))
            return i;
    }
    return -1;
}

//-----------------------------------------------------------------------------
// Adds flex controller data to a particular source
//-----------------------------------------------------------------------------
void AddFlexControllers(
        s_source_t *pSource) {
    CUtlVector<int> &r2s = pSource->m_rawIndexToRemapSourceIndex;
    CUtlVector<int> &r2l = pSource->m_rawIndexToRemapLocalIndex;
    CUtlVector<int> &l2i = pSource->m_leftRemapIndexToGlobalFlexControllIndex;
    CUtlVector<int> &r2i = pSource->m_rightRemapIndexToGlobalFlexControllIndex;

    // Number of Raw controls in this source
    const int nRawControlCount = pSource->m_CombinationControls.Count();
    // Initialize rawToRemapIndices
    r2s.SetSize(nRawControlCount);
    r2l.SetSize(nRawControlCount);
    for (int i = 0; i < nRawControlCount; ++i) {
        r2s[i] = -1;
        r2l[i] = -1;
    }

    // Number of Remapped Controls in this source
    const int nRemappedControlCount = pSource->m_FlexControllerRemaps.Count();
    l2i.SetSize(nRemappedControlCount);
    r2i.SetSize(nRemappedControlCount);

    for (int i = 0; i < nRemappedControlCount; ++i) {
        s_flexcontrollerremap_t &remapControl = pSource->m_FlexControllerRemaps[i];

        // Number of Raw Controls In This Remapped Control
        const int nRemappedRawControlCount = remapControl.m_RawControls.size();

        // Figure out the mapping from raw to remapped
        for (int j = 0; j < nRemappedRawControlCount; ++j) {
            for (int k = 0; k < nRawControlCount; ++k) {
                if (remapControl.m_RawControls[j] == pSource->m_CombinationControls[k].name) {
                    Assert(r2s[k] == -1);
                    Assert(r2l[k] == -1);
                    r2s[k] = i;    // The index of the remapped control
                    r2l[k] = j;    // The index of which control this is in the remap
                    break;
                }
            }
        }

        if (remapControl.m_bIsStereo) {
            // The controls have to be named 'right_' and 'left_' and right has to be first for
            // hlfaceposer to recognize them

            // See if we can add two more flex controllers
            if ((g_numflexcontrollers + 1) >= MAXSTUDIOFLEXCTRL)
                MdlError("Line %d: Too many flex controllers, max %d, cannot add split control %s from source %s",
                         g_StudioMdlContext.iLinecount, MAXSTUDIOFLEXCTRL, remapControl.m_Name.Get(), pSource->filename);

            s_flexcontroller_t *pController;

            int nLen = remapControl.m_Name.Length();
            char *pTemp = (char *) _alloca(nLen + 7);    // 'left_' && 'right_'

            memcpy(pTemp + 6, remapControl.m_Name.Get(), nLen + 1);
            memcpy(pTemp, "right_", 6);
            pTemp[nLen + 6] = '\0';

            {
                int existing = FindFlexControllerByName(pTemp);
                if (existing >= 0) {
                    remapControl.m_RightIndex = existing;
                    r2i[i] = existing;
                } else {
                    remapControl.m_RightIndex = g_numflexcontrollers;
                    r2i[i] = g_numflexcontrollers;
                    pController = &g_flexcontroller[g_numflexcontrollers++];
                    Q_strncpy(pController->name, pTemp, sizeof(pController->name));
                    Q_strncpy(pController->type, remapControl.m_FlexGroup.IsEmpty() ? pTemp : remapControl.m_FlexGroup.Get(), sizeof(pController->type));
                    if (remapControl.m_RemapType == FLEXCONTROLLER_REMAP_2WAY ||
                        remapControl.m_RemapType == FLEXCONTROLLER_REMAP_EYELID) {
                        pController->min = -1.0f;
                        pController->max = 1.0f;
                    } else {
                        pController->min = 0.0f;
                        pController->max = 1.0f;
                    }
                    if (!g_StudioMdlContext.quiet) {
                        printf("flex controller  %-32s [%s] %.3f..%.3f\n",
                               pController->name, pController->type, pController->min, pController->max);
                    }
                }
            }

            memcpy(pTemp + 5, remapControl.m_Name.Get(), nLen + 1);
            memcpy(pTemp, "left_", 5);
            pTemp[nLen + 5] = '\0';

            {
                int existing = FindFlexControllerByName(pTemp);
                if (existing >= 0) {
                    remapControl.m_LeftIndex = existing;
                    l2i[i] = existing;
                } else {
                    remapControl.m_LeftIndex = g_numflexcontrollers;
                    l2i[i] = g_numflexcontrollers;
                    pController = &g_flexcontroller[g_numflexcontrollers++];
                    Q_strncpy(pController->name, pTemp, sizeof(pController->name));
                    Q_strncpy(pController->type, remapControl.m_FlexGroup.IsEmpty() ? pTemp : remapControl.m_FlexGroup.Get(), sizeof(pController->type));
                    if (remapControl.m_RemapType == FLEXCONTROLLER_REMAP_2WAY ||
                        remapControl.m_RemapType == FLEXCONTROLLER_REMAP_EYELID) {
                        pController->min = -1.0f;
                        pController->max = 1.0f;
                    } else {
                        pController->min = 0.0f;
                        pController->max = 1.0f;
                    }
                    if (!g_StudioMdlContext.quiet) {
                        printf("flex controller  %-32s [%s] %.3f..%.3f\n",
                               pController->name, pController->type, pController->min, pController->max);
                    }
                }
            }
        } else {
            {
                int existing = FindFlexControllerByName(remapControl.m_Name.Get());
                if (existing >= 0) {
                    remapControl.m_Index = existing;
                    r2i[i] = existing;
                    l2i[i] = existing;
                } else {
                    // See if we can add one more flex controller
                    if (g_numflexcontrollers >= MAXSTUDIOFLEXCTRL)
                        MdlError("Line %d: Too many flex controllers, max %d, cannot add control %s from source %s",
                                 g_StudioMdlContext.iLinecount, MAXSTUDIOFLEXCTRL, remapControl.m_Name.Get(), pSource->filename);

                    remapControl.m_Index = g_numflexcontrollers;
                    r2i[i] = g_numflexcontrollers;
                    l2i[i] = g_numflexcontrollers;
                    s_flexcontroller_t *pController = &g_flexcontroller[g_numflexcontrollers++];
                    Q_strncpy(pController->name, remapControl.m_Name.Get(), sizeof(pController->name));
                    Q_strncpy(pController->type, remapControl.m_FlexGroup.IsEmpty() ? remapControl.m_Name.Get() : remapControl.m_FlexGroup.Get(), sizeof(pController->type));

                    if (remapControl.m_RemapType == FLEXCONTROLLER_REMAP_2WAY ||
                        remapControl.m_RemapType == FLEXCONTROLLER_REMAP_EYELID) {
                        pController->min = -1.0f;
                        pController->max = 1.0f;
                    } else {
                        pController->min = 0.0f;
                        pController->max = 1.0f;
                    }
                    if (!g_StudioMdlContext.quiet) {
                        printf("flex controller  %-32s [%s] %.3f..%.3f\n",
                               pController->name, pController->type, pController->min, pController->max);
                    }
                }
            }
        }

        if (remapControl.m_RemapType == FLEXCONTROLLER_REMAP_NWAY ||
            remapControl.m_RemapType == FLEXCONTROLLER_REMAP_EYELID) {
            const int nLen = remapControl.m_Name.Length();
            char *pTemp = (char *) _alloca(nLen + 6 + 1); // 'multi_' + 1 for the NULL
            memcpy(pTemp, "multi_", 6);
            memcpy(pTemp + 6, remapControl.m_Name.Get(), nLen + 1);
            pTemp[nLen + 6] = '\0';

            int existing = FindFlexControllerByName(pTemp);
            if (existing >= 0) {
                remapControl.m_MultiIndex = existing;
            } else {
                if (g_numflexcontrollers >= MAXSTUDIOFLEXCTRL)
                    MdlError(
                            "Line %d: Too many flex controllers, max %d, cannot add value control for nWay %s from source %s",
                            g_StudioMdlContext.iLinecount, MAXSTUDIOFLEXCTRL, remapControl.m_Name.Get(), pSource->filename);

                remapControl.m_MultiIndex = g_numflexcontrollers;
                s_flexcontroller_t *pController = &g_flexcontroller[g_numflexcontrollers++];
                Q_strncpy(pController->name, pTemp, sizeof(pController->name));
                Q_strncpy(pController->type, pTemp, sizeof(pController->type));
                pController->min = -1.0f;
                pController->max = 1.0f;
                if (!g_StudioMdlContext.quiet) {
                    printf("flex controller  %-32s [%s] %.3f..%.3f\n",
                           pController->name, pController->type, pController->min, pController->max);
                }
            }
        }

        if (remapControl.m_RemapType == FLEXCONTROLLER_REMAP_EYELID) {
            // Make a blink controller
            int existing = FindFlexControllerByName("blink");
            if (existing >= 0) {
                remapControl.m_BlinkController = existing;
            } else {
                if (g_numflexcontrollers >= MAXSTUDIOFLEXCTRL)
                    MdlError(
                            "Line %d: Too many flex controllers, max %d, cannot add value control for nWay %s from source %s",
                            g_StudioMdlContext.iLinecount, MAXSTUDIOFLEXCTRL, remapControl.m_Name.Get(), pSource->filename);

                remapControl.m_BlinkController = g_numflexcontrollers;
                s_flexcontroller_t *pController = &g_flexcontroller[g_numflexcontrollers++];
                Q_strncpy(pController->name, "blink", sizeof(pController->name));
                Q_strncpy(pController->type, "blink", sizeof(pController->type));
                pController->min = 0.0f;
                pController->max = 1.0f;
                if (!g_StudioMdlContext.quiet) {
                    printf("flex controller  %-32s [%s] %.3f..%.3f\n",
                           pController->name, pController->type, pController->min, pController->max);
                }
            }
        }
    }

#ifdef _DEBUG
    for (int j = 0; j != nRawControlCount; ++j) {
        Assert(r2s[j] != -1);
        Assert(r2l[j] != -1);
    }
#endif // def _DEBUG
}


//-----------------------------------------------------------------------------
// Adds flex controller remappers
//-----------------------------------------------------------------------------
void AddBodyFlexRemaps(s_source_t *pSource) {
    int nCount = pSource->m_FlexControllerRemaps.Count();
    for (int i = 0; i < nCount; ++i) {
        g_StudioMdlContext.FlexControllerRemap.emplace_back();
        s_flexcontrollerremap_t &remap = g_StudioMdlContext.FlexControllerRemap.back();
        remap = pSource->m_FlexControllerRemaps[i];
    }
}

//-----------------------------------------------------------------------------
// Process a body command
//-----------------------------------------------------------------------------
void AddBodyFlexData(s_source_t *pSource, int imodel) {
    pSource->m_nKeyStartIndex = g_numflexkeys;

    // Add flex keys
    int nCount = pSource->m_FlexKeys.Count();
    for (int i = 0; i < nCount; ++i) {
        s_flexkey_t &key = pSource->m_FlexKeys[i];

        if (g_numflexkeys >= MAXSTUDIOFLEXKEYS)
            MdlError("Line %d: Too many flex keys, max %d, cannot add flexKey %s from source %s",
                     g_StudioMdlContext.iLinecount, MAXSTUDIOFLEXKEYS, key.animationname, pSource->filename);

        memcpy(&g_flexkey[g_numflexkeys], &key, sizeof(s_flexkey_t));
        g_flexkey[g_numflexkeys].imodel = imodel;

        // flexpair was set up in AddFlexKey
        if (key.flexpair) {
            char mod[512];
            Q_snprintf(mod, sizeof(mod), "%sL", key.animationname);
            g_flexkey[g_numflexkeys].flexdesc = Add_Flexdesc(mod);
            Q_snprintf(mod, sizeof(mod), "%sR", key.animationname);
            g_flexkey[g_numflexkeys].flexpair = Add_Flexdesc(mod);
        } else {
            g_flexkey[g_numflexkeys].flexdesc = Add_Flexdesc(key.animationname);
            g_flexkey[g_numflexkeys].flexpair = 0;
        }

        ++g_numflexkeys;
    }

    AddFlexControllers(pSource);

    AddBodyFlexRemaps(pSource);
}


void Option_Eyeball(s_model_t *pmodel) {
    Vector tmp;
    int i, j;
    int mesh_material;
    char szMeshMaterial[256];

    s_eyeball_t *eyeball = &(pmodel->eyeball[pmodel->numeyeballs++]);

    // name
    GetToken(false);
    strcpyn(eyeball->name, token);

    // bone name
    GetToken(false);
    for (i = 0; i < pmodel->source->numbones; i++) {
        if (!Q_stricmp(pmodel->source->localBone[i].name, token)) {
            eyeball->bone = i;
            break;
        }
    }
    if (!g_StudioMdlContext.createMakefile && i >= pmodel->source->numbones) {
        TokenError("unknown eyeball bone \"%s\"\n", token);
    }

    // X
    GetToken(false);
    tmp[0] = verify_atof(token) * g_currentscale;

    // Y
    GetToken(false);
    tmp[1] = verify_atof(token) * g_currentscale;

    // Z
    GetToken(false);
    tmp[2] = verify_atof(token) * g_currentscale;

    // mesh material
    GetToken(false);
    Q_strncpy(szMeshMaterial, token, sizeof(szMeshMaterial));
    mesh_material = UseTextureAsMaterial(LookupTexture(token));

    // diameter
    GetToken(false);
    eyeball->radius = verify_atof(token) / 2.0 * g_currentscale;

    // Z angle offset
    GetToken(false);
    eyeball->zoffset = tan(DEG2RAD(verify_atof(token)));

    // iris material (no longer used, but we need to remove the token)
    GetToken(false);

    // pupil scale
    GetToken(false);
    eyeball->iris_scale = 1.0 / (verify_atof(token) * g_currentscale);

    VectorCopy(tmp, eyeball->org);

    for (i = 0; i < pmodel->source->nummeshes; i++) {
        j = pmodel->source->meshindex[i]; // meshes are internally stored by material index

        if (j == mesh_material) {
            eyeball->mesh = i; // FIXME: should this be pre-adjusted?
            break;
        }
    }

    if (!g_StudioMdlContext.createMakefile && i >= pmodel->source->nummeshes) {
        TokenError("can't find eyeball texture \"%s\" on model\n", szMeshMaterial);
    }

    // translate eyeball into bone space
    VectorITransform(tmp, pmodel->source->boneToPose[eyeball->bone], eyeball->org);

    matrix3x4_t vtmp;
    AngleMatrix(g_defaultrotation, vtmp);

    VectorIRotate(Vector(0, 0, 1), vtmp, tmp);
    VectorIRotate(tmp, pmodel->source->boneToPose[eyeball->bone], eyeball->up);

    VectorIRotate(Vector(1, 0, 0), vtmp, tmp);
    VectorIRotate(tmp, pmodel->source->boneToPose[eyeball->bone], eyeball->forward);

    // these get overwritten by "eyelid" data
    eyeball->upperlidflexdesc = -1;
    eyeball->lowerlidflexdesc = -1;
}


//-----------------------------------------------------------------------------
//
// A vertex cache animation file is a special case of a VTA file
// Same format
// Frame 0 is the defaultflex frame
// All other frames will get a flexdesc of "f#" where # [0,frameCount-1]
// Then an NWAY controller will be defined to play back the flex data
// as an animation as the controller goes from [0,1]
//-----------------------------------------------------------------------------
void Option_VertexCacheAnimationFile(char *pszVtaFile, int nModelIndex) {
    if (g_numflexkeys > 0) {
        MdlError("%s: Flexes already defined.  vcafile can be only flex option in $model block\n", __FUNCTION__);
        return;
    }

    s_source_t *pSource = g_model[nModelIndex]->source;
    s_source_t *pVtaSource = Load_Source(pszVtaFile, "vta");

    if (pVtaSource->m_Animations.Count() <= 0) {
        MdlError("%s: No animations in VertexCacheAnimationFile \"%s\"\n", __FUNCTION__, pszVtaFile);
        return;
    }

    {
        s_flexkey_t &flexKey = g_flexkey[g_numflexkeys++];

        flexKey.flexdesc = Add_Flexdesc("default");
        flexKey.flexpair = 0;
        flexKey.source = pVtaSource;
        flexKey.imodel = nModelIndex;
        flexKey.frame = 0;
        flexKey.target0 = 0.0;
        flexKey.target1 = 1.0;
        flexKey.target2 = 10;
        flexKey.target3 = 11;
        flexKey.split = 0;
        flexKey.decay = 0.0;
        V_strncpy(flexKey.animationname, pVtaSource->m_Animations[0].animationname, ARRAYSIZE(flexKey.animationname));
    }

    CFmtStr sTmp;

    const int nActualFrameCount = pVtaSource->m_Animations.Head().numframes - 1;

    for (int i = 0; i < nActualFrameCount; ++i) {
        sTmp.sprintf("f%d", i);

        {
            s_flexkey_t &flexKey = g_flexkey[g_numflexkeys++];

            flexKey.flexdesc = Add_Flexdesc(sTmp.Access());
            flexKey.flexpair = 0;
            flexKey.source = pVtaSource;
            flexKey.imodel = nModelIndex;
            flexKey.frame = (i + 1);
            flexKey.target0 = 0.0;
            flexKey.target1 = 1.0;
            flexKey.target2 = 10;
            flexKey.target3 = 11;
            flexKey.split = 0;
            flexKey.decay = 0.0;
            V_strncpy(flexKey.animationname, pVtaSource->m_Animations[0].animationname,
                      ARRAYSIZE(flexKey.animationname));
        }
    }

    s_flexcontrollerremap_t &flexRemap = pSource->m_FlexControllerRemaps[pSource->m_FlexControllerRemaps.AddToTail()];

    flexRemap.m_RemapType = FLEXCONTROLLER_REMAP_NWAY;
    flexRemap.m_bIsStereo = false;
    flexRemap.m_Index = -1;            // Don't know this right now
    flexRemap.m_LeftIndex = -1;        // Don't know this right now
    flexRemap.m_RightIndex = -1;    // Don't know this right now
    flexRemap.m_MultiIndex = -1;    // Don't know this right now
    flexRemap.m_EyesUpDownFlexController = -1;
    flexRemap.m_BlinkController = -1;

    char szBuf[MAX_PATH];
    V_FileBase(pVtaSource->filename, szBuf, ARRAYSIZE(szBuf));
    flexRemap.m_Name = szBuf;

    for (int i = 0; i < nActualFrameCount; ++i) {
        sTmp.sprintf("f%d", i);
        flexRemap.m_RawControls.emplace_back(sTmp.Access());
    }

    for (int i = 0; i < flexRemap.m_RawControls.size(); ++i) {
        int nFlexKey = -1;
        for (int j = 0; j < g_numflexkeys; ++j) {
            if (!V_stricmp(g_flexdesc[g_flexkey[j].flexdesc].FACS, flexRemap.m_RawControls[i].Get())) {
                nFlexKey = j;
                break;
            }
        }

        if (nFlexKey < 0) {
            MdlError("%s Cannot find flex to group \"%s\"\n", __FUNCTION__, flexRemap.m_RawControls[i].Get());
            pSource->m_FlexControllerRemaps.RemoveMultipleFromTail(1);
            return;
        }

        s_combinationcontrol_t &combinationControl = pSource->m_CombinationControls[pSource->m_CombinationControls.AddToTail()];
        V_strncpy(combinationControl.name, flexRemap.m_RawControls[i].Get(), ARRAYSIZE(combinationControl.name));

        s_combinationrule_t &combinationRule = pSource->m_CombinationRules[pSource->m_CombinationRules.AddToTail()];
        combinationRule.m_nFlex = nFlexKey;
        combinationRule.m_Combination.AddToTail(nFlexKey - 1);
    }

    AddFlexControllers(pSource);

    AddBodyFlexRemaps(pSource);
}


void Option_Spherenormals(s_source_t *psource) {
    Vector pos;
    int i, j;
    int mesh_material;
    char szMeshMaterial[256];

    // mesh material
    GetToken(false);
    strcpyn(szMeshMaterial, token);
    mesh_material = UseTextureAsMaterial(LookupTexture(token));

    // X
    GetToken(false);
    pos[0] = verify_atof(token);

    // Y
    GetToken(false);
    pos[1] = verify_atof(token);

    // Z
    GetToken(false);
    pos[2] = verify_atof(token);

    for (i = 0; i < psource->nummeshes; i++) {
        j = psource->meshindex[i]; // meshes are internally stored by material index

        if (j == mesh_material) {
            s_vertexinfo_t *vertex = &psource->vertex[psource->mesh[i].vertexoffset];

            for (int k = 0; k < psource->mesh[i].numvertices; k++) {
                Vector n = vertex[k].position - pos;
                VectorNormalize(n);
                if (DotProduct(n, vertex[k].normal) < 0.0) {
                    vertex[k].normal = -1 * n;
                } else {
                    vertex[k].normal = n;
                }
#if 0
                vertex[k].normal[0] += 0.5f * ( 2.0f * ( ( float )rand() ) / ( float )VALVE_RAND_MAX ) - 1.0f;
                vertex[k].normal[1] += 0.5f * ( 2.0f * ( ( float )rand() ) / ( float )VALVE_RAND_MAX ) - 1.0f;
                vertex[k].normal[2] += 0.5f * ( 2.0f * ( ( float )rand() ) / ( float )VALVE_RAND_MAX ) - 1.0f;
                VectorNormalize( vertex[k].normal );
#endif
            }
            break;
        }
    }

    if (i >= psource->nummeshes) {
        TokenError("can't find spherenormal texture \"%s\" on model\n", szMeshMaterial);
    }
}

//-----------------------------------------------------------------------------
// Purpose: Returns an s_sourceanim_t * from the specified s_source_t *
//          that matches the specified animation name (case insensitive)
//          and is also a new style (i.e. DMX) vertex animation
//-----------------------------------------------------------------------------
const s_sourceanim_t *GetNewStyleSourceVertexAnim(s_source_t *pSource, const char *pszVertexAnimName) {
    for (int i = 0; i < pSource->m_Animations.Count(); ++i) {
        const s_sourceanim_t *pSourceAnim = &(pSource->m_Animations[i]);
        if (!pSourceAnim || !pSourceAnim->newStyleVertexAnimations)
            continue;

        if (!Q_stricmp(pszVertexAnimName, pSourceAnim->animationname))
            return pSourceAnim;
    }

    return nullptr;
}


//-----------------------------------------------------------------------------
// Purpose:   Handle the eyelid option using a DMX instead of a VTA source
// QC Syntax: dmxeyelid <upper|lower> <source> lowerer <delta> <-0.20 neutral F00 0.19 raiser F02 0.28 righteyeball righteye lefteyeball lefteye
// e.g: dmxeyelid upper "coach_model_merged.dmx" lowerer F01 -0.20 neutral F00 0.19 raiser F02 0.28 righteyeball righteye lefteyeball lefteye
//-----------------------------------------------------------------------------
void Option_DmxEyelid(int imodel) {
    // upper | lower
    const char *pszType = NULL;
    GetToken(false);
    if (!Q_stricmp("upper", token)) {
        pszType = "upper";
    } else if (!Q_stricmp("lower", token)) {
        pszType = "lower";
    } else {
        TokenError("$model dmxeyelid, expected one of \"upper\", \"lower\"");
        return;
    }

    // (exr)
    CUtlString sSourceFile;
    GetToken(false);
    sSourceFile = token;

    s_source_t *pSource = Load_Source(sSourceFile.Get(), "dmx");
    if (!pSource) {
        MdlError("(%d) : %s:  Cannot load source file \"%s\"\n", g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine, sSourceFile.Get());
        return;
    }

    enum RightLeftType_t {
        kLeft = 0,
        kRight = 1,
        kRightLeftTypeCount = 2
    };

    struct EyelidData_t {
        int m_nFlexDesc[kRightLeftTypeCount];
        const s_sourceanim_t *m_pSourceAnim;
        float m_flTarget;
        const char *m_pszSuffix;
    };

    EyelidData_t eyelidData[3] =
            {
                    {{-1, -1}, nullptr, 0.0f, "lowerer"},
                    {{-1, -1}, nullptr, 0.0f, "neutral"},
                    {{-1, -1}, nullptr, 0.0f, "raiser"}
            };

    CUtlString sRightEyeball;
    CUtlString sLeftEyeball;

    while (TokenAvailable()) {
        GetToken(false);
        bool bTokenHandled = false;

        for (int i = 0; i < kEyelidTypeCount; ++i) {
            if (!Q_stricmp(token, eyelidData[i].m_pszSuffix)) {
                bTokenHandled = true;

                GetToken(false);
                eyelidData[i].m_pSourceAnim = GetNewStyleSourceVertexAnim(pSource, token);
                if (eyelidData[i].m_pSourceAnim == NULL) {
                    MdlError("(%d) : %s:  No DMX vertex animation named \"%s\" in source \"%s\"\n", g_StudioMdlContext.iLinecount,
                             g_StudioMdlContext.szLine, token, sSourceFile.Get());
                    return;
                }

                // target
                GetToken(false);
                eyelidData[i].m_flTarget = verify_atof(token) * g_currentscale;

                break;
            }
        }

        if (bTokenHandled)
            continue;

        else if (!Q_stricmp(token, "righteyeball")) {
            GetToken(false);
            sRightEyeball = token;
        } else if (!Q_stricmp(token, "lefteyeball")) {
            GetToken(false);
            sLeftEyeball = token;
        }
    }

    // Add a flexdesc for <type>_right & <type>_left
    // Where <type> is "upper" or "lower"
    int nRightLeftBaseDesc[kRightLeftTypeCount] = {-1, -1};

    CUtlString sRightBaseDesc = pszType;
    sRightBaseDesc += "_right";
    nRightLeftBaseDesc[kRight] = Add_Flexdesc(sRightBaseDesc.Get());

    for (int i = 0; i < kEyelidTypeCount; ++i) {
        CUtlString sRightLocalDesc = sRightBaseDesc;
        sRightLocalDesc += "_";
        sRightLocalDesc += eyelidData[i].m_pszSuffix;
        eyelidData[i].m_nFlexDesc[kRight] = Add_Flexdesc(sRightLocalDesc.Get());
    }

    CUtlString sLeftBaseDesc = pszType;
    sLeftBaseDesc += "_left";
    nRightLeftBaseDesc[kLeft] = Add_Flexdesc(sLeftBaseDesc.Get());

    for (int i = 0; i < kEyelidTypeCount; ++i) {
        CUtlString sLeftLocalDesc = sLeftBaseDesc;
        sLeftLocalDesc += "_";
        sLeftLocalDesc += eyelidData[i].m_pszSuffix;
        eyelidData[i].m_nFlexDesc[kLeft] = Add_Flexdesc(sLeftLocalDesc.Get());
    }

    for (int i = 0; i < kEyelidTypeCount; ++i) {
        s_flexkey_t *pFlexKey = &g_flexkey[g_numflexkeys];
        pFlexKey->source = pSource;
        Q_strncpy(pFlexKey->animationname, eyelidData[i].m_pSourceAnim->animationname, sizeof(pFlexKey->animationname));
        pFlexKey->frame = 0;            // Currently always 0 for DMX
        pFlexKey->imodel = imodel;
        pFlexKey->flexdesc = nRightLeftBaseDesc[kLeft];
        pFlexKey->flexpair = nRightLeftBaseDesc[kRight];
        pFlexKey->split = 0.0f;
        pFlexKey->decay = 1.0;
        switch (i) {
            case kLowerer:
                pFlexKey->target0 = -11;
                pFlexKey->target1 = -10;
                pFlexKey->target2 = eyelidData[kLowerer].m_flTarget;
                pFlexKey->target3 = eyelidData[kNeutral].m_flTarget;
                break;
            case kNeutral:
                pFlexKey->target0 = eyelidData[kLowerer].m_flTarget;
                pFlexKey->target1 = eyelidData[kNeutral].m_flTarget;
                pFlexKey->target2 = eyelidData[kNeutral].m_flTarget;
                pFlexKey->target3 = eyelidData[kRaiser].m_flTarget;
                break;
            case kRaiser:
                pFlexKey->target0 = eyelidData[kNeutral].m_flTarget;
                pFlexKey->target1 = eyelidData[kRaiser].m_flTarget;
                pFlexKey->target2 = 10;
                pFlexKey->target3 = 11;
                break;
        }
        ++g_numflexkeys;
    }

    bool bRightOk = false;
    bool bLeftOk = false;

    s_model_t *pModel = g_model[imodel];
    for (int i = 0; i < pModel->numeyeballs; ++i) {
        s_eyeball_t *pEyeball = &(pModel->eyeball[i]);
        if (!pEyeball)
            continue;

        RightLeftType_t nRightLeftIndex = kRight;
        if (!Q_stricmp(sRightEyeball, pEyeball->name)) {
            nRightLeftIndex = kRight;
            bRightOk = true;
        } else if (!Q_stricmp(sLeftEyeball, pEyeball->name)) {
            nRightLeftIndex = kLeft;
            bLeftOk = true;
        } else {
            MdlWarning("Unknown Eyeball: %s\n", pEyeball->name);
            continue;
        }

        for (int j = 0; j < kEyelidTypeCount; ++j) {
            if (fabs(eyelidData[j].m_flTarget) > pEyeball->radius) {
                TokenError("Eyelid \"%s\" %s %.1f out of range (+-%.1f)\n", pszType, eyelidData[j].m_pszSuffix,
                           eyelidData[j].m_flTarget, pEyeball->radius);
            }
        }

        switch (*pszType) {
            case 'u':    // upper
                pEyeball->upperlidflexdesc = nRightLeftBaseDesc[nRightLeftIndex];
                for (int j = 0; j < kEyelidTypeCount; ++j) {
                    pEyeball->upperflexdesc[j] = eyelidData[j].m_nFlexDesc[nRightLeftIndex];
                    pEyeball->uppertarget[j] = eyelidData[j].m_flTarget;
                }
                break;
            case 'l':    // lower
                pEyeball->lowerlidflexdesc = nRightLeftBaseDesc[nRightLeftIndex];
                for (int j = 0; j < kEyelidTypeCount; ++j) {
                    pEyeball->lowerflexdesc[j] = eyelidData[j].m_nFlexDesc[nRightLeftIndex];
                    pEyeball->lowertarget[j] = eyelidData[j].m_flTarget;
                }
                break;
            default:
                Assert(0);
                break;
        }
    }

    if (!bRightOk) {
        TokenError("Could not find right eye \"%s\"\n", sRightEyeball.Get());
    }

    if (!bLeftOk) {
        TokenError("Could not find left eye \"%s\"\n", sRightEyeball.Get());
    }
}

int Option_Mouth(s_model_t *pmodel) {
    // index
    GetToken(false);
    int index = verify_atoi(token);
    if (index >= g_nummouths)
        g_nummouths = index + 1;

    // flex controller name
    GetToken(false);
    g_mouth[index].flexdesc = Add_Flexdesc(token);

    // bone name
    GetToken(false);
    strcpyn(g_mouth[index].bonename, token);

    // vector
    GetToken(false);
    g_mouth[index].forward[0] = verify_atof(token);
    GetToken(false);
    g_mouth[index].forward[1] = verify_atof(token);
    GetToken(false);
    g_mouth[index].forward[2] = verify_atof(token);
    return 0;
}

void Option_Flexcontroller(s_model_t *pmodel) {
    char type[256];
    float range_min = 0.0f;
    float range_max = 1.0f;

    // g_flex
    GetToken(false);
    strcpy(type, token);

    while (TokenAvailable()) {
        GetToken(false);

        if (stricmp(token, "range") == 0) {
            GetToken(false);
            range_min = verify_atof(token);

            GetToken(false);
            range_max = verify_atof(token);
        } else {
            int existing = FindFlexControllerByName(token);
            if (existing >= 0) {
                MdlWarning("Flex controller '%s' already defined, reusing existing entry\n", token);
            } else {
                if (g_numflexcontrollers >= MAXSTUDIOFLEXCTRL)
                    TokenError("Too many flex controllers, max %d\n", MAXSTUDIOFLEXCTRL);
                strcpyn(g_flexcontroller[g_numflexcontrollers].name, token);
                strcpyn(g_flexcontroller[g_numflexcontrollers].type, type);
                g_flexcontroller[g_numflexcontrollers].min = range_min;
                g_flexcontroller[g_numflexcontrollers].max = range_max;
                if (!g_StudioMdlContext.quiet) {
                    printf("flex controller  %-32s [%s] %.3f..%.3f\n",
                           token, type, range_min, range_max);
                }
                g_numflexcontrollers++;
            }
        }
    }

    // this needs to be per model.
}

void Option_NoAutoDMXRules(s_source_t *pSource) {
    // zero out the automatic flex controllers
    g_numflexcontrollers = 0;

    pSource->bNoAutoDMXRules = true;
}

//-----------------------------------------------------------------------------
// Purpose: throw away all the options for a specific sequence or animation
//-----------------------------------------------------------------------------

int ParseEmpty() {
    int depth = 0;

    while (1) {
        if (depth > 0) {
            if (!GetToken(true)) {
                break;
            }
        } else {
            if (!TokenAvailable()) {
                break;
            } else {
                GetToken(false);
            }
        }

        if (endofscript) {
            if (depth != 0) {
                TokenError("missing }\n");
            }
            return 1;
        }
        if (stricmp("{", token) == 0) {
            depth++;
        } else if (stricmp("}", token) == 0) {
            depth--;
        }

        if (depth < 0) {
            TokenError("missing {\n");
        }
    }

    return 0;
}

void Option_Flex(char *name, char *vtafile, int imodel, float pairsplit) {
    if (g_numflexkeys >= MAXSTUDIOFLEXKEYS) {
        TokenError("Too many flexes, max %d\n", MAXSTUDIOFLEXKEYS);
    }

    int flexdesc, flexpair;

    if (pairsplit != 0) {
        char mod[256];
        sprintf(mod, "%sR", name);
        flexdesc = Add_Flexdesc(mod);

        sprintf(mod, "%sL", name);
        flexpair = Add_Flexdesc(mod);
    } else {
        flexdesc = Add_Flexdesc(name);
        flexpair = 0;
    }

    // initialize
    g_flexkey[g_numflexkeys].imodel = imodel;
    g_flexkey[g_numflexkeys].flexdesc = flexdesc;
    g_flexkey[g_numflexkeys].target0 = 0.0;
    g_flexkey[g_numflexkeys].target1 = 1.0;
    g_flexkey[g_numflexkeys].target2 = 10;
    g_flexkey[g_numflexkeys].target3 = 11;
    g_flexkey[g_numflexkeys].split = pairsplit;
    g_flexkey[g_numflexkeys].flexpair = flexpair;
    g_flexkey[g_numflexkeys].decay = 1.0;

    while (TokenAvailable()) {
        GetToken(false);

        if (stricmp(token, "frame") == 0) {
            GetToken(false);

            g_flexkey[g_numflexkeys].frame = verify_atoi(token);
        } else if (stricmp(token, "position") == 0) {
            GetToken(false);
            g_flexkey[g_numflexkeys].target1 = verify_atof(token);
        } else if (stricmp(token, "split") == 0) {
            GetToken(false);
            g_flexkey[g_numflexkeys].split = verify_atof(token);
        } else if (stricmp(token, "decay") == 0) {
            GetToken(false);
            g_flexkey[g_numflexkeys].decay = verify_atof(token);
        } else {
            TokenError("unknown option: %s", token);
        }

    }

    if (g_numflexkeys > 1) {
        if (g_flexkey[g_numflexkeys - 1].flexdesc == g_flexkey[g_numflexkeys].flexdesc) {
            g_flexkey[g_numflexkeys - 1].target2 = g_flexkey[g_numflexkeys - 1].target1;
            g_flexkey[g_numflexkeys - 1].target3 = g_flexkey[g_numflexkeys].target1;
            g_flexkey[g_numflexkeys].target0 = g_flexkey[g_numflexkeys - 1].target1;
        }
    }

    // link to source
    s_source_t *pSource = Load_Source(vtafile, "vta");
    g_flexkey[g_numflexkeys].source = pSource;
    if (pSource->m_Animations.Count()) {
        Q_strncpy(g_flexkey[g_numflexkeys].animationname, pSource->m_Animations[0].animationname,
                  sizeof(g_flexkey[g_numflexkeys].animationname));
    } else {
        g_flexkey[g_numflexkeys].animationname[0] = 0;
    }
    g_numflexkeys++;
    // this needs to be per model.
}


void Option_Flexrule(s_model_t * /* pmodel */, const char *name) {
    int precedence[32];
    precedence[STUDIO_CONST] = 0;
    precedence[STUDIO_FETCH1] = 0;
    precedence[STUDIO_FETCH2] = 0;
    precedence[STUDIO_ADD] = 1;
    precedence[STUDIO_SUB] = 1;
    precedence[STUDIO_MUL] = 2;
    precedence[STUDIO_DIV] = 2;
    precedence[STUDIO_NEG] = 4;
    precedence[STUDIO_EXP] = 3;
    precedence[STUDIO_OPEN] = 0;    // only used in token parsing
    precedence[STUDIO_CLOSE] = 0;
    precedence[STUDIO_COMMA] = 0;
    precedence[STUDIO_MAX] = 5;
    precedence[STUDIO_MIN] = 5;

    s_flexop_t stream[MAX_OPS];
    int i = 0;
    s_flexop_t stack[MAX_OPS];
    int j = 0;
    int k = 0;

    int flexdesc;
    for (flexdesc = 0; flexdesc < g_numflexdesc; flexdesc++) {
        if (stricmp(name, g_flexdesc[flexdesc].FACS) == 0) {
            break;
        }
    }

    if (flexdesc >= g_numflexdesc) {
        TokenError("Rule for unknown flex %s\n", name);
    }

    // A rule for this flex descriptor may already exist - the same DMX
    // combination operator is re-processed once per LOD source load (and per
    // $body/$model that references the same DMX), so without this guard the
    // identical flex rules accumulate (e.g. 4 copies for a 2-LOD model). Mirror
    // the dedup already done in AddBodyFlexRule(): skip the duplicate, draining
    // the rest of the expression line so the tokenizer stays in sync.
    for (j = 0; j < g_numflexrules; j++) {
        if (g_flexrule[j].flex == flexdesc) {
            // =
            GetToken(false);
            bool linecontinueSkip = false;
            while (linecontinueSkip || TokenAvailable()) {
                GetExprToken(linecontinueSkip);
                linecontinueSkip = false;
                if (token[0] == '\\') {
                    if (!GetToken(false) || token[0] != '\\') {
                        TokenError("unknown expression token '\\%s\n", token);
                    }
                    linecontinueSkip = true;
                } else if (token[0] == '%') {
                    GetExprToken(false);
                }
            }
            return;
        }
    }

    if (g_numflexrules >= MAXSTUDIOFLEXRULES) {
        TokenError("Too many flex rules (max %d)\n", MAXSTUDIOFLEXRULES);
    }

    s_flexrule_t *pRule = &g_flexrule[g_numflexrules++];

    pRule->flex = flexdesc;
    pRule->numops = 0;

    // =
    GetToken(false);

    // parse all the tokens
    bool linecontinue = false;
    while (linecontinue || TokenAvailable()) {
        GetExprToken(linecontinue);

        linecontinue = false;

        if (token[0] == '\\') {
            if (!GetToken(false) || token[0] != '\\') {
                TokenError("unknown expression token '\\%s\n", token);
            }
            linecontinue = true;
        } else if (token[0] == '(') {
            stream[i++].op = STUDIO_OPEN;
        } else if (token[0] == ')') {
            stream[i++].op = STUDIO_CLOSE;
        } else if (token[0] == '+') {
            stream[i++].op = STUDIO_ADD;
        } else if (token[0] == '-') {
            stream[i].op = STUDIO_SUB;
            if (i > 0) {
                switch (stream[i - 1].op) {
                    case STUDIO_OPEN:
                    case STUDIO_ADD:
                    case STUDIO_SUB:
                    case STUDIO_MUL:
                    case STUDIO_DIV:
                    case STUDIO_COMMA:
                        // it's a unary if it's preceded by a "(+-*/,"?
                        stream[i].op = STUDIO_NEG;
                        break;
                }
            }
            i++;
        } else if (token[0] == '*') {
            stream[i++].op = STUDIO_MUL;
        } else if (token[0] == '/') {
            stream[i++].op = STUDIO_DIV;
        } else if (V_isdigit(token[0])) {
            stream[i].op = STUDIO_CONST;
            stream[i++].d.value = verify_atof(token);
        } else if (token[0] == ',') {
            stream[i++].op = STUDIO_COMMA;
        } else if (stricmp(token, "max") == 0) {
            stream[i++].op = STUDIO_MAX;
        } else if (stricmp(token, "min") == 0) {
            stream[i++].op = STUDIO_MIN;
        } else {
            if (token[0] == '%') {
                GetExprToken(false);

                for (k = 0; k < g_numflexdesc; k++) {
                    if (stricmp(token, g_flexdesc[k].FACS) == 0) {
                        stream[i].op = STUDIO_FETCH2;
                        stream[i++].d.index = k;
                        break;
                    }
                }
                if (k >= g_numflexdesc) {
                    TokenError("unknown flex %s\n", token);
                }
            } else {
                for (k = 0; k < g_numflexcontrollers; k++) {
                    if (stricmp(token, g_flexcontroller[k].name) == 0) {
                        stream[i].op = STUDIO_FETCH1;
                        stream[i++].d.index = k;
                        break;
                    }
                }
                if (k >= g_numflexcontrollers) {
                    TokenError("unknown controller %s\n", token);
                }
            }
        }
    }

    if (i > MAX_OPS) {
        TokenError("expression %s too complicated\n", g_flexdesc[pRule->flex].FACS);
    }

    j = 0;
    for (k = 0; k < i; k++) {
        if (j >= MAX_OPS) {
            TokenError("expression %s too complicated\n", g_flexdesc[pRule->flex].FACS);
        }
        switch (stream[k].op) {
            case STUDIO_CONST:
            case STUDIO_FETCH1:
            case STUDIO_FETCH2:
                pRule->op[pRule->numops++] = stream[k];
                break;
            case STUDIO_OPEN:
                stack[j++] = stream[k];
                break;
            case STUDIO_CLOSE:
                // pop all operators off of the stack until an open paren
                while (j > 0 && stack[j - 1].op != STUDIO_OPEN) {
                    pRule->op[pRule->numops++] = stack[j - 1];
                    j--;
                }
                if (j == 0) {
                    TokenError("unmatched closed parentheses\n");
                }
                if (j > 0)
                    j--;
                break;
            case STUDIO_COMMA:
                // pop all operators off of the stack until an open paren
                while (j > 0 && stack[j - 1].op != STUDIO_OPEN) {
                    pRule->op[pRule->numops++] = stack[j - 1];
                    j--;
                }
                // push operator onto the stack
                stack[j++] = stream[k];
                break;
            case STUDIO_ADD:
            case STUDIO_SUB:
            case STUDIO_MUL:
            case STUDIO_DIV:
                // pop all operators off of the stack that have equal or higher precedence
                while (j > 0 && precedence[stream[k].op] <= precedence[stack[j - 1].op]) {
                    pRule->op[pRule->numops++] = stack[j - 1];
                    j--;
                }
                // push operator onto the stack
                stack[j++] = stream[k];
                break;
            case STUDIO_NEG:
                if (stream[k + 1].op == STUDIO_CONST) {
                    // change sign of constant, skip op
                    stream[k + 1].d.value = -stream[k + 1].d.value;
                } else {
                    // push operator onto the stack
                    stack[j++] = stream[k];
                }
                break;
            case STUDIO_MAX:
            case STUDIO_MIN:
                // push operator onto the stack
                stack[j++] = stream[k];
                break;
        }
        if (pRule->numops >= MAX_OPS)
            TokenError("expression for \"%s\" too complicated\n", g_flexdesc[pRule->flex].FACS);
    }
    // pop all operators off of the stack
    while (j > 0) {
        pRule->op[pRule->numops++] = stack[j - 1];
        j--;
        if (pRule->numops >= MAX_OPS)
            TokenError("expression for \"%s\" too complicated\n", g_flexdesc[pRule->flex].FACS);
    }

    // reprocess the operands, eating commas for all functions
    int numCommas = 0;
    j = 0;
    for (k = 0; k < pRule->numops; k++) {
        switch (pRule->op[k].op) {
            case STUDIO_MAX:
            case STUDIO_MIN:
                if (pRule->op[j - 1].op != STUDIO_COMMA) {
                    TokenError("missing comma\n");
                }
                // eat the comma operator
                numCommas--;
                pRule->op[j - 1] = pRule->op[k];
                break;
            case STUDIO_COMMA:
                numCommas++;
                pRule->op[j++] = pRule->op[k];
                break;
            default:
                pRule->op[j++] = pRule->op[k];
                break;
        }
    }
    pRule->numops = j;
    if (numCommas != 0) {
        TokenError("too many comma's\n");
    }

    if (pRule->numops > MAX_OPS) {
        TokenError("expression %s too complicated\n", g_flexdesc[pRule->flex].FACS);
    }
}

//-----------------------------------------------------------------------------
// Add A Body Flex Rule
//-----------------------------------------------------------------------------
void AddBodyFlexFetchRule(
        s_source_t *pSource,
        s_flexrule_t *pRule,
        int rawIndex,
        const CUtlVector<int> &pRawIndexToRemapSourceIndex,
        const CUtlVector<int> &pRawIndexToRemapLocalIndex,
        const CUtlVector<int> &pRemapSourceIndexToGlobalFlexControllerIndex) {
    // Lookup the various indices of the requested input to fetch
    // Relative to the remapped controls in the current s_source_t
    const int remapSourceIndex = pRawIndexToRemapSourceIndex[rawIndex];
    // Relative to the specific remapped control
    const int remapLocalIndex = pRawIndexToRemapLocalIndex[rawIndex];
    // The global flex controller index that the user ultimately twiddles
    const int globalFlexControllerIndex = pRemapSourceIndexToGlobalFlexControllerIndex[remapSourceIndex];

    // Get the Remap record
    s_flexcontrollerremap_t &remap = pSource->m_FlexControllerRemaps[remapSourceIndex];
    switch (remap.m_RemapType) {
        case FLEXCONTROLLER_REMAP_PASSTHRU:
            // Easy As!
            pRule->op[pRule->numops].op = STUDIO_FETCH1;
            pRule->op[pRule->numops].d.index = globalFlexControllerIndex;
            pRule->numops++;
            break;

        case FLEXCONTROLLER_REMAP_EYELID:
            if (remapLocalIndex == 0) {
                pRule->op[pRule->numops].op = STUDIO_CONST;
                pRule->op[pRule->numops].d.value =
                        remap.m_EyesUpDownFlexController >= 0 ? remap.m_EyesUpDownFlexController : -1;
                pRule->numops++;

                pRule->op[pRule->numops].op = STUDIO_CONST;
                pRule->op[pRule->numops].d.value = remap.m_BlinkController >= 0 ? remap.m_BlinkController : -1;
                pRule->numops++;

                pRule->op[pRule->numops].op = STUDIO_CONST;
                pRule->op[pRule->numops].d.value = globalFlexControllerIndex;    // CloseLid
                pRule->numops++;

                pRule->op[pRule->numops].op = STUDIO_DME_LOWER_EYELID;
                pRule->op[pRule->numops].d.index = remap.m_MultiIndex;    // CloseLidV
                pRule->numops++;
            } else {
                pRule->op[pRule->numops].op = STUDIO_CONST;
                pRule->op[pRule->numops].d.value =
                        remap.m_EyesUpDownFlexController >= 0 ? remap.m_EyesUpDownFlexController : -1;
                pRule->numops++;

                pRule->op[pRule->numops].op = STUDIO_CONST;
                pRule->op[pRule->numops].d.value = remap.m_BlinkController >= 0 ? remap.m_BlinkController : -1;
                pRule->numops++;

                pRule->op[pRule->numops].op = STUDIO_CONST;
                pRule->op[pRule->numops].d.value = globalFlexControllerIndex;    // CloseLid
                pRule->numops++;

                pRule->op[pRule->numops].op = STUDIO_DME_UPPER_EYELID;
                pRule->op[pRule->numops].d.index = remap.m_MultiIndex;    // CloseLidV
                pRule->numops++;
            }
            break;

        case FLEXCONTROLLER_REMAP_2WAY:
            // A little trickier... local index 0 is on the left, local index 1 is on the right
            // Left Equivalent RemapVal( -1.0, 0.0, 0.0, 1.0 )
            // Right Equivalent RemapVal( 0.0, 1.0, 0.0, 1.0 )
            if (remapLocalIndex == 0) {
                pRule->op[pRule->numops].op = STUDIO_2WAY_0;
                pRule->op[pRule->numops].d.index = globalFlexControllerIndex;
                pRule->numops++;
            } else {
                pRule->op[pRule->numops].op = STUDIO_2WAY_1;
                pRule->op[pRule->numops].d.index = globalFlexControllerIndex;
                pRule->numops++;
            }
            break;

        case FLEXCONTROLLER_REMAP_NWAY: {
            int nRemapCount = remap.m_RawControls.size();
            float flStep = (nRemapCount > 2) ? 2.0f / (nRemapCount - 1) : 0.0f;

            if (remapLocalIndex == 0) {
                pRule->op[pRule->numops].op = STUDIO_CONST;
                pRule->op[pRule->numops].d.value = -11.0f;
                pRule->numops++;

                pRule->op[pRule->numops].op = STUDIO_CONST;
                pRule->op[pRule->numops].d.value = -10.0f;
                pRule->numops++;

                pRule->op[pRule->numops].op = STUDIO_CONST;
                pRule->op[pRule->numops].d.value = -1.0f;
                pRule->numops++;

                pRule->op[pRule->numops].op = STUDIO_CONST;
                pRule->op[pRule->numops].d.value = -1.0f + flStep;
                pRule->numops++;
            } else if (remapLocalIndex == nRemapCount - 1) {
                pRule->op[pRule->numops].op = STUDIO_CONST;
                pRule->op[pRule->numops].d.value = 1.0f - flStep;
                pRule->numops++;

                pRule->op[pRule->numops].op = STUDIO_CONST;
                pRule->op[pRule->numops].d.value = 1.0f;
                pRule->numops++;

                pRule->op[pRule->numops].op = STUDIO_CONST;
                pRule->op[pRule->numops].d.value = 10.0f;
                pRule->numops++;

                pRule->op[pRule->numops].op = STUDIO_CONST;
                pRule->op[pRule->numops].d.value = 11.0f;
                pRule->numops++;
            } else {
                float flPeak = remapLocalIndex * flStep - 1.0f;

                pRule->op[pRule->numops].op = STUDIO_CONST;
                pRule->op[pRule->numops].d.value = flPeak - flStep;
                pRule->numops++;

                pRule->op[pRule->numops].op = STUDIO_CONST;
                pRule->op[pRule->numops].d.value = flPeak;
                pRule->numops++;

                pRule->op[pRule->numops].op = STUDIO_CONST;
                pRule->op[pRule->numops].d.value = flPeak;
                pRule->numops++;

                pRule->op[pRule->numops].op = STUDIO_CONST;
                pRule->op[pRule->numops].d.value = flPeak + flStep;
                pRule->numops++;
            }

            pRule->op[pRule->numops].op = STUDIO_CONST;
            pRule->op[pRule->numops].d.value = remap.m_MultiIndex;
            pRule->numops++;

            pRule->op[pRule->numops].op = STUDIO_NWAY;
            pRule->op[pRule->numops].d.index = globalFlexControllerIndex;
            pRule->numops++;
        }
            break;
        default:
            Assert(0);
            // This is an error condition
            pRule->op[pRule->numops].op = STUDIO_CONST;
            pRule->op[pRule->numops].d.value = 1.0f;
            pRule->numops++;
            break;
    }
}


//-----------------------------------------------------------------------------
// Add A Body Flex Rule
//-----------------------------------------------------------------------------
void AddBodyFlexRule(
        s_source_t *pSource,
        s_combinationrule_t &rule,
        int nFlexDesc,
        const CUtlVector<int> &pRawIndexToRemapSourceIndex,
        const CUtlVector<int> &pRawIndexToRemapLocalIndex,
        const CUtlVector<int> &pRemapSourceIndexToGlobalFlexControllerIndex) {
    for (int j = 0; j < g_numflexrules; ++j) {
        if (g_flexrule[j].flex == nFlexDesc)
            return;
    }

    if (g_numflexrules >= MAXSTUDIOFLEXRULES)
        MdlError("Line %d: Too many flex rules, max %d",
                 g_StudioMdlContext.iLinecount, MAXSTUDIOFLEXRULES);

    s_flexrule_t *pRule = &g_flexrule[g_numflexrules++];
    pRule->flex = nFlexDesc;

    // This will multiply the combination together
    const int nCombinationCount = rule.m_Combination.Count();
    if (nCombinationCount) {
        for (int j = 0; j < nCombinationCount; ++j) {
            // Handle any controller remapping
            AddBodyFlexFetchRule(pSource, pRule, rule.m_Combination[j],
                                 pRawIndexToRemapSourceIndex, pRawIndexToRemapLocalIndex,
                                 pRemapSourceIndexToGlobalFlexControllerIndex);
        }

        if (nCombinationCount > 1) {
            pRule->op[pRule->numops].op = STUDIO_COMBO;
            pRule->op[pRule->numops].d.index = nCombinationCount;
            pRule->numops++;
        }
    }

    // This will multiply in the suppressors
    int nDominators = rule.m_Dominators.Count();
    for (int j = 0; j < nDominators; ++j) {
        const int nFactorCount = rule.m_Dominators[j].Count();
        if (nFactorCount) {
            for (int k = 0; k < nFactorCount; ++k) {
                AddBodyFlexFetchRule(pSource, pRule, rule.m_Dominators[j][k],
                                     pRawIndexToRemapSourceIndex, pRawIndexToRemapLocalIndex,
                                     pRemapSourceIndexToGlobalFlexControllerIndex);
            }

            pRule->op[pRule->numops].op = STUDIO_DOMINATE;
            pRule->op[pRule->numops].d.index = nFactorCount;
            pRule->numops++;
        }
    }
}


void AddBodyFlexRules(s_source_t *pSource) {
    const int nRemapCount = pSource->m_FlexControllerRemaps.Count();
    for (int i = 0; i < nRemapCount; ++i) {
        s_flexcontrollerremap_t &remap = pSource->m_FlexControllerRemaps[i];
        if (remap.m_RemapType == FLEXCONTROLLER_REMAP_EYELID && !remap.m_EyesUpDownFlexName.IsEmpty()) {
            for (int j = 0; j < g_numflexcontrollers; ++j) {
                if (!Q_strcmp(g_flexcontroller[j].name, remap.m_EyesUpDownFlexName.Get())) {
                    Assert(remap.m_EyesUpDownFlexController == -1);
                    remap.m_EyesUpDownFlexController = j;
                    break;
                }
            }
        }
    }

    const int nCount = pSource->m_CombinationRules.Count();
    for (int i = 0; i < nCount; ++i) {
        s_combinationrule_t &rule = pSource->m_CombinationRules[i];
        s_flexkey_t &flexKey = g_flexkey[pSource->m_nKeyStartIndex + rule.m_nFlex];
        AddBodyFlexRule(pSource, rule, flexKey.flexdesc,
                        pSource->m_rawIndexToRemapSourceIndex, pSource->m_rawIndexToRemapLocalIndex,
                        pSource->m_leftRemapIndexToGlobalFlexControllIndex);
        if (flexKey.flexpair != 0) {
            AddBodyFlexRule(pSource, rule, flexKey.flexpair,
                            pSource->m_rawIndexToRemapSourceIndex, pSource->m_rawIndexToRemapLocalIndex,
                            pSource->m_rightRemapIndexToGlobalFlexControllIndex);
        }
    }
}

void Option_Eyelid(int imodel) {
    char type[256];
    char vtafile[256];

    // type
    GetToken(false);
    strcpyn(type, token);

    // source
    GetToken(false);
    strcpyn(vtafile, token);

    int lowererframe = 0;
    int neutralframe = 0;
    int raiserframe = 0;
    float lowerertarget = 0.0f;
    float neutraltarget = 0.0f;
    float raisertarget = 0.0f;
    int lowererdesc = 0;
    int neutraldesc = 0;
    int raiserdesc = 0;
    int basedesc;
    float split = 0;
    char szEyeball[64] = {""};

    basedesc = g_numflexdesc;
    strcpyn(g_flexdesc[g_numflexdesc++].FACS, type);

    while (TokenAvailable()) {
        GetToken(false);

        char localdesc[256];
        strcpy(localdesc, type);
        strcat(localdesc, "_");
        strcat(localdesc, token);

        if (stricmp(token, "lowerer") == 0) {
            GetToken(false);
            lowererframe = verify_atoi(token);
            GetToken(false);
            lowerertarget = verify_atof(token) * g_currentscale;
            lowererdesc = g_numflexdesc;
            strcpyn(g_flexdesc[g_numflexdesc++].FACS, localdesc);
        } else if (stricmp(token, "neutral") == 0) {
            GetToken(false);
            neutralframe = verify_atoi(token);
            GetToken(false);
            neutraltarget = verify_atof(token) * g_currentscale;
            neutraldesc = g_numflexdesc;
            strcpyn(g_flexdesc[g_numflexdesc++].FACS, localdesc);
        } else if (stricmp(token, "raiser") == 0) {
            GetToken(false);
            raiserframe = verify_atoi(token);
            GetToken(false);
            raisertarget = verify_atof(token) * g_currentscale;
            raiserdesc = g_numflexdesc;
            strcpyn(g_flexdesc[g_numflexdesc++].FACS, localdesc);
        } else if (stricmp(token, "split") == 0) {
            GetToken(false);
            split = verify_atof(token);
        } else if (stricmp(token, "eyeball") == 0) {
            GetToken(false);
            strcpy(szEyeball, token);
        } else {
            TokenError("unknown option: %s", token);
        }
    }

    s_source_t *pSource = Load_Source(vtafile, "vta");
    g_flexkey[g_numflexkeys + 0].source = pSource;
    g_flexkey[g_numflexkeys + 0].frame = lowererframe;
    g_flexkey[g_numflexkeys + 0].flexdesc = basedesc;
    g_flexkey[g_numflexkeys + 0].imodel = imodel;
    g_flexkey[g_numflexkeys + 0].split = split;
    g_flexkey[g_numflexkeys + 0].target0 = -11;
    g_flexkey[g_numflexkeys + 0].target1 = -10;
    g_flexkey[g_numflexkeys + 0].target2 = lowerertarget;
    g_flexkey[g_numflexkeys + 0].target3 = neutraltarget;
    g_flexkey[g_numflexkeys + 0].decay = 0.0;
    if (pSource->m_Animations.Count() > 0) {
        Q_strncpy(g_flexkey[g_numflexkeys + 0].animationname, pSource->m_Animations[0].animationname,
                  sizeof(g_flexkey[g_numflexkeys + 0].animationname));
    } else {
        g_flexkey[g_numflexkeys + 0].animationname[0] = 0;
    }

    g_flexkey[g_numflexkeys + 1].source = g_flexkey[g_numflexkeys + 0].source;
    Q_strncpy(g_flexkey[g_numflexkeys + 1].animationname, g_flexkey[g_numflexkeys + 0].animationname,
              sizeof(g_flexkey[g_numflexkeys + 1].animationname));
    g_flexkey[g_numflexkeys + 1].frame = neutralframe;
    g_flexkey[g_numflexkeys + 1].flexdesc = basedesc;
    g_flexkey[g_numflexkeys + 1].imodel = imodel;
    g_flexkey[g_numflexkeys + 1].split = split;
    g_flexkey[g_numflexkeys + 1].target0 = lowerertarget;
    g_flexkey[g_numflexkeys + 1].target1 = neutraltarget;
    g_flexkey[g_numflexkeys + 1].target2 = neutraltarget;
    g_flexkey[g_numflexkeys + 1].target3 = raisertarget;
    g_flexkey[g_numflexkeys + 1].decay = 0.0;

    g_flexkey[g_numflexkeys + 2].source = g_flexkey[g_numflexkeys + 0].source;
    Q_strncpy(g_flexkey[g_numflexkeys + 2].animationname, g_flexkey[g_numflexkeys + 0].animationname,
              sizeof(g_flexkey[g_numflexkeys + 2].animationname));
    g_flexkey[g_numflexkeys + 2].frame = raiserframe;
    g_flexkey[g_numflexkeys + 2].flexdesc = basedesc;
    g_flexkey[g_numflexkeys + 2].imodel = imodel;
    g_flexkey[g_numflexkeys + 2].split = split;
    g_flexkey[g_numflexkeys + 2].target0 = neutraltarget;
    g_flexkey[g_numflexkeys + 2].target1 = raisertarget;
    g_flexkey[g_numflexkeys + 2].target2 = 10;
    g_flexkey[g_numflexkeys + 2].target3 = 11;
    g_flexkey[g_numflexkeys + 2].decay = 0.0;
    g_numflexkeys += 3;

    s_model_t *pmodel = g_model[imodel];
    for (int i = 0; i < pmodel->numeyeballs; i++) {
        s_eyeball_t *peyeball = &(pmodel->eyeball[i]);

        if (szEyeball[0] != '\0') {
            if (stricmp(peyeball->name, szEyeball) != 0)
                continue;
        }

        if (fabs(lowerertarget) > peyeball->radius) {
            TokenError("Eyelid \"%s\" lowerer out of range (+-%.1f)\n", type, peyeball->radius);
        }
        if (fabs(neutraltarget) > peyeball->radius) {
            TokenError("Eyelid \"%s\" neutral out of range (+-%.1f)\n", type, peyeball->radius);
        }
        if (fabs(raisertarget) > peyeball->radius) {
            TokenError("Eyelid \"%s\" raiser  out of range (+-%.1f)\n", type, peyeball->radius);
        }

        switch (type[0]) {
            case 'u':
                peyeball->upperlidflexdesc = basedesc;
                peyeball->upperflexdesc[0] = lowererdesc;
                peyeball->uppertarget[0] = lowerertarget;
                peyeball->upperflexdesc[1] = neutraldesc;
                peyeball->uppertarget[1] = neutraltarget;
                peyeball->upperflexdesc[2] = raiserdesc;
                peyeball->uppertarget[2] = raisertarget;
                break;
            case 'l':
                peyeball->lowerlidflexdesc = basedesc;
                peyeball->lowerflexdesc[0] = lowererdesc;
                peyeball->lowertarget[0] = lowerertarget;
                peyeball->lowerflexdesc[1] = neutraldesc;
                peyeball->lowertarget[1] = neutraltarget;
                peyeball->lowerflexdesc[2] = raiserdesc;
                peyeball->lowertarget[2] = raisertarget;
                break;
        }
    }
}

void Option_Model(int imodel) {
    int depth = 0;
    while (1) {
        char FAC[256], vtafile[256];
        if (depth > 0) {
            if (!GetToken(true))
                break;
        } else {
            if (!TokenAvailable())
                break;
            else
                GetToken(false);
        }

        if (endofscript) {
            if (depth != 0)
                TokenError("missing }\n");
            return;
        }
        if (!Q_stricmp("{", token)) {
            depth++;
        } else if (!Q_stricmp("}", token)) {
            depth--;
        } else if (!Q_stricmp("eyeball", token)) {
            Option_Eyeball(g_model[imodel]);
        } else if (!Q_stricmp("eyelid", token)) {
            Option_Eyelid(imodel);
        } else if (!Q_stricmp("dmxeyelid", token)) {
            Option_DmxEyelid(imodel);
        } else if (!V_stricmp("vcafile", token)) {
            GetToken(false);
            Option_VertexCacheAnimationFile(token, imodel);
        } else if (!Q_stricmp("flex", token)) {
            GetToken(false);
            strcpy(FAC, token);
            if (depth == 0) {
                GetToken(false);
                strcpy(vtafile, token);
            }
            Option_Flex(FAC, vtafile, imodel, 0.0);
        } else if (!Q_stricmp("flexpair", token)) {
            GetToken(false);
            strcpy(FAC, token);
            GetToken(false);
            float split = atof(token);
            if (depth == 0) {
                GetToken(false);
                strcpy(vtafile, token);
            }
            Option_Flex(FAC, vtafile, imodel, split);
        } else if (!Q_stricmp("defaultflex", token)) {
            if (depth == 0) {
                GetToken(false);
                strcpy(vtafile, token);
            }
            Option_Flex("default", vtafile, imodel, 0.0);
            g_defaultflexkey = &g_flexkey[g_numflexkeys - 1];
        } else if (!Q_stricmp("flexfile", token)) {
            GetToken(false);
            strcpy(vtafile, token);
        } else if (!Q_stricmp("localvar", token)) {
            while (TokenAvailable()) {
                GetToken(false);
                Add_Flexdesc(token);
            }
        } else if (!Q_stricmp("mouth", token)) {
            Option_Mouth(g_model[imodel]);
        } else if (!Q_stricmp("flexcontroller", token)) {
            Option_Flexcontroller(g_model[imodel]);
        } else if (token[0] == '%') {
            Option_Flexrule(g_model[imodel], &token[1]);
        } else if (!Q_stricmp("attachment", token)) {
            // Option_Attachment( g_model[imodel] );
        } else if (!Q_stricmp(token, "spherenormals")) {
            Option_Spherenormals(g_model[imodel]->source);
        } else if (!Q_stricmp(token, "noautodmxrules")) {
            Option_NoAutoDMXRules(g_model[imodel]->source);
        } else {
            TokenError("unknown model option \"%s\"\n", token);
        }

        if (depth < 0)
            TokenError("missing {\n");
    }

    if (g_model[imodel]->source && !g_model[imodel]->source->bNoAutoDMXRules)
        AddBodyFlexRules(g_model[imodel]->source);
}

void Cmd_Model() {
    g_model[g_nummodels] = (s_model_t *) calloc(1, sizeof(s_model_t));

    // name
    if (!GetToken(false))
        return;
    strcpyn(g_model[g_nummodels]->name, token);

    // fake g_bodypart stuff
    g_bodypart[g_numbodyparts].base = 1;
    if (g_numbodyparts != 0) {
        g_bodypart[g_numbodyparts].base =
                g_bodypart[g_numbodyparts - 1].base * g_bodypart[g_numbodyparts - 1].nummodels;
    }
    strcpyn(g_bodypart[g_numbodyparts].name, token);

    g_bodypart[g_numbodyparts].pmodel[g_bodypart[g_numbodyparts].nummodels] = g_model[g_nummodels];
    g_bodypart[g_numbodyparts].nummodels = 1;
    g_numbodyparts++;

    Option_Studio(g_model[g_nummodels]);

    if (g_model[g_nummodels]->source) {
        // Body command should add any flex commands in the source loaded
        AddBodyFlexData(g_model[g_nummodels]->source, g_nummodels);
        AddBodyAttachments(g_model[g_nummodels]->source);
    }

    Option_Model(g_nummodels);

    g_nummodels++;
}

void Cmd_FakeVTA() {
    int depth = 0;

    GetToken(false);

    s_source_t *psource = (s_source_t *) calloc(1, sizeof(s_source_t));
    g_source[g_numsources] = psource;
    strcpyn(g_source[g_numsources]->filename, token);
    g_numsources++;

    while (1) {
        if (depth > 0) {
            if (!GetToken(true)) {
                break;
            }
        } else {
            if (!TokenAvailable()) {
                break;
            } else {
                GetToken(false);
            }
        }

        if (endofscript) {
            if (depth != 0) {
                TokenError("missing }\n");
            }
            return;
        }
        if (stricmp("{", token) == 0) {
            depth++;
        } else if (stricmp("}", token) == 0) {
            depth--;
        } else if (stricmp("appendvta", token) == 0) {
            char filename[256];
            // file
            GetToken(false);
            strcpy(filename, token);

            GetToken(false);
            int frame = verify_atoi(token);

            AppendVTAtoOBJ(psource, filename, frame);
        }
    }
}


int lookupControl(char *string) {
    if (stricmp(string, "X") == 0) return STUDIO_X;
    if (stricmp(string, "Y") == 0) return STUDIO_Y;
    if (stricmp(string, "Z") == 0) return STUDIO_Z;
    if (stricmp(string, "XR") == 0) return STUDIO_XR;
    if (stricmp(string, "YR") == 0) return STUDIO_YR;
    if (stricmp(string, "ZR") == 0) return STUDIO_ZR;

    if (stricmp(string, "LX") == 0) return STUDIO_LX;
    if (stricmp(string, "LY") == 0) return STUDIO_LY;
    if (stricmp(string, "LZ") == 0) return STUDIO_LZ;
    if (stricmp(string, "LXR") == 0) return STUDIO_LXR;
    if (stricmp(string, "LYR") == 0) return STUDIO_LYR;
    if (stricmp(string, "LZR") == 0) return STUDIO_LZR;

    if (stricmp(string, "LM") == 0) return STUDIO_LINEAR;
    if (stricmp(string, "LQ") == 0) return STUDIO_QUADRATIC_MOTION;

    return -1;
}


void Cmd_IKChain() {
    if (!GetToken(false))
        return;

    int i;
    for (i = 0; i < g_numikchains; i++) {
        if (stricmp(token, g_ikchain[i].name) == 0) {
            break;
        }
    }
    if (i < g_numikchains) {
        if (!g_StudioMdlContext.quiet) {
            printf("duplicate ikchain \"%s\" ignored\n", token);
        }
        while (TokenAvailable()) {
            GetToken(false);
        }
        return;
    }

    strcpyn(g_ikchain[g_numikchains].name, token);

    GetToken(false);
    strcpyn(g_ikchain[g_numikchains].bonename, token);

    g_ikchain[g_numikchains].axis = STUDIO_Z;
    g_ikchain[g_numikchains].value = 0.0;
    g_ikchain[g_numikchains].height = 18.0;
    g_ikchain[g_numikchains].floor = 0.0;
    g_ikchain[g_numikchains].radius = 0.0;

    while (TokenAvailable()) {
        GetToken(false);

        if (lookupControl(token) != -1) {
            g_ikchain[g_numikchains].axis = lookupControl(token);
            GetToken(false);
            g_ikchain[g_numikchains].value = verify_atof(token);
        } else if (stricmp("height", token) == 0) {
            GetToken(false);
            g_ikchain[g_numikchains].height = verify_atof(token);
        } else if (stricmp("pad", token) == 0) {
            GetToken(false);
            g_ikchain[g_numikchains].radius = verify_atof(token) / 2.0;
        } else if (stricmp("floor", token) == 0) {
            GetToken(false);
            g_ikchain[g_numikchains].floor = verify_atof(token);
        } else if (stricmp("knee", token) == 0) {
            GetToken(false);
            g_ikchain[g_numikchains].link[0].kneeDir.x = verify_atof(token);
            GetToken(false);
            g_ikchain[g_numikchains].link[0].kneeDir.y = verify_atof(token);
            GetToken(false);
            g_ikchain[g_numikchains].link[0].kneeDir.z = verify_atof(token);
        } else if (stricmp("center", token) == 0) {
            GetToken(false);
            g_ikchain[g_numikchains].center.x = verify_atof(token);
            GetToken(false);
            g_ikchain[g_numikchains].center.y = verify_atof(token);
            GetToken(false);
            g_ikchain[g_numikchains].center.z = verify_atof(token);
        }
    }
    g_numikchains++;
}

void Cmd_IKAutoplayLock() {
    GetToken(false);
    strcpyn(g_ikautoplaylock[g_numikautoplaylocks].name, token);

    GetToken(false);
    g_ikautoplaylock[g_numikautoplaylocks].flPosWeight = verify_atof(token);

    GetToken(false);
    g_ikautoplaylock[g_numikautoplaylocks].flLocalQWeight = verify_atof(token);

    g_numikautoplaylocks++;
}

void Cmd_Root() {
    if (GetToken(false)) {
        strcpyn(rootname, token);
    }
}

void Cmd_Controller() {
    if (GetToken(false)) {
        if (!stricmp("mouth", token)) {
            g_bonecontroller[g_numbonecontrollers].inputfield = 4;
        } else {
            g_bonecontroller[g_numbonecontrollers].inputfield = verify_atoi(token);
        }
        if (GetToken(false)) {
            strcpyn(g_bonecontroller[g_numbonecontrollers].name, token);
            GetToken(false);
            if ((g_bonecontroller[g_numbonecontrollers].type = lookupControl(token)) == -1) {
                MdlWarning("unknown g_bonecontroller type '%s'\n", token);
                return;
            }
            GetToken(false);
            g_bonecontroller[g_numbonecontrollers].start = verify_atof(token);
            GetToken(false);
            g_bonecontroller[g_numbonecontrollers].end = verify_atof(token);

            if (g_bonecontroller[g_numbonecontrollers].type & (STUDIO_XR | STUDIO_YR | STUDIO_ZR)) {
                if (((int) (g_bonecontroller[g_numbonecontrollers].start + 360) % 360) ==
                    ((int) (g_bonecontroller[g_numbonecontrollers].end + 360) % 360)) {
                    g_bonecontroller[g_numbonecontrollers].type |= STUDIO_RLOOP;
                }
            }
            g_numbonecontrollers++;
        }
    }
}

void Cmd_ScreenAlign() {
    if (GetToken(false)) {

        Assert(g_numscreenalignedbones < MAXSTUDIOSRCBONES);

        strcpyn(g_screenalignedbone[g_numscreenalignedbones].name, token);
        g_screenalignedbone[g_numscreenalignedbones].flags = BONE_SCREEN_ALIGN_SPHERE;

        if (GetToken(false)) {
            if (!stricmp("sphere", token)) {
                g_screenalignedbone[g_numscreenalignedbones].flags = BONE_SCREEN_ALIGN_SPHERE;
            } else if (!stricmp("cylinder", token)) {
                g_screenalignedbone[g_numscreenalignedbones].flags = BONE_SCREEN_ALIGN_CYLINDER;
            }
        }

        g_numscreenalignedbones++;

    } else {
        TokenError("$screenalign: expected bone name\n");
    }
}

void Cmd_WorldAlign() {
    if (GetToken(false)) {
        Assert(g_numworldalignedbones < MAXSTUDIOSRCBONES);

        strcpyn(g_worldalignedbone[g_numworldalignedbones].name, token);
        g_worldalignedbone[g_numworldalignedbones].flags = BONE_WORLD_ALIGN;

        g_numworldalignedbones++;

    } else {
        TokenError("$worldalign: expected bone name\n");
    }
}

void Cmd_BBox() {
    GetToken(false);
    bbox[0][0] = verify_atof(token);

    GetToken(false);
    bbox[0][1] = verify_atof(token);

    GetToken(false);
    bbox[0][2] = verify_atof(token);

    GetToken(false);
    bbox[1][0] = verify_atof(token);

    GetToken(false);
    bbox[1][1] = verify_atof(token);

    GetToken(false);
    bbox[1][2] = verify_atof(token);

    g_wrotebbox = true;
}

void Cmd_BBoxOnlyVerts() {
    g_bboxonlyverts = true;
}

void Cmd_CBox() {
    GetToken(false);
    cbox[0][0] = verify_atof(token);

    GetToken(false);
    cbox[0][1] = verify_atof(token);

    GetToken(false);
    cbox[0][2] = verify_atof(token);

    GetToken(false);
    cbox[1][0] = verify_atof(token);

    GetToken(false);
    cbox[1][1] = verify_atof(token);

    GetToken(false);
    cbox[1][2] = verify_atof(token);

    g_wrotecbox = true;
}

void Cmd_Gamma() {
    GetToken(false);
    g_gamma = verify_atof(token);
}

void Cmd_TextureGroup() {
    if (g_StudioMdlContext.createMakefile) {
        return;
    }
    int i;
    int depth = 0;
    int index = 0;
    int group = 0;


    if (!GetToken(false))
        return;

    if (g_numskinref == 0)
        g_numskinref = g_numtextures;

    while (1) {
        if (!GetToken(true)) {
            break;
        }

        if (endofscript) {
            if (depth != 0) {
                TokenError("missing }\n");
            }
            return;
        }
        if (token[0] == '{') {
            depth++;
        } else if (token[0] == '}') {
            depth--;
            if (depth == 0)
                break;
            group++;
            index = 0;
        } else if (depth == 2) {
            i = UseTextureAsMaterial(LookupTexture(token));
            g_texturegroup[g_numtexturegroups][group][index] = i;
            if (group != 0)
                g_texture[i].parent = g_texturegroup[g_numtexturegroups][0][index];
            index++;
            g_numtexturereps[g_numtexturegroups] = index;
            g_numtexturelayers[g_numtexturegroups] = group + 1;
        }
    }

    g_numtexturegroups++;
}

void Cmd_Hitgroup() {
    GetToken(false);
    g_hitgroup[g_numhitgroups].group = verify_atoi(token);
    GetToken(false);
    strcpyn(g_hitgroup[g_numhitgroups].name, token);
    g_numhitgroups++;
}

void Cmd_Hitbox() {
    bool autogenerated = false;
    if (g_StudioMdlContext.hitboxsets.size() == 0) {
        g_StudioMdlContext.hitboxsets.emplace_back();
        autogenerated = true;
    }

    // Last one
    s_hitboxset *set = &g_StudioMdlContext.hitboxsets[g_StudioMdlContext.hitboxsets.size() - 1];
    if (autogenerated) {
        memset(set, 0, sizeof(*set));

        // fill in name if it wasn't specified in the .qc
        strcpy(set->hitboxsetname, "default");
    }

    GetToken(false);
    set->hitbox[set->numhitboxes].group = verify_atoi(token);

    // Grab the bone name:
    GetToken(false);
    strcpyn(set->hitbox[set->numhitboxes].name, token);

    GetToken(false);
    set->hitbox[set->numhitboxes].bmin[0] = verify_atof(token);
    GetToken(false);
    set->hitbox[set->numhitboxes].bmin[1] = verify_atof(token);
    GetToken(false);
    set->hitbox[set->numhitboxes].bmin[2] = verify_atof(token);
    GetToken(false);
    set->hitbox[set->numhitboxes].bmax[0] = verify_atof(token);
    GetToken(false);
    set->hitbox[set->numhitboxes].bmax[1] = verify_atof(token);
    GetToken(false);
    set->hitbox[set->numhitboxes].bmax[2] = verify_atof(token);

    if (TokenAvailable()) {
        GetToken(false);
        set->hitbox[set->numhitboxes].angOffsetOrientation[0] = verify_atof(token);
        GetToken(false);
        set->hitbox[set->numhitboxes].angOffsetOrientation[1] = verify_atof(token);
        GetToken(false);
        set->hitbox[set->numhitboxes].angOffsetOrientation[2] = verify_atof(token);
    } else {
        set->hitbox[set->numhitboxes].angOffsetOrientation = QAngle(0, 0, 0);
    }

    if (TokenAvailable()) {
        GetToken(false);
        set->hitbox[set->numhitboxes].flCapsuleRadius = verify_atof(token);
    } else {
        set->hitbox[set->numhitboxes].flCapsuleRadius = -1;
    }

    //Scale hitboxes
    scale_vertex(set->hitbox[set->numhitboxes].bmin);
    scale_vertex(set->hitbox[set->numhitboxes].bmax);
    // clear out the hitboxname:
    memset(set->hitbox[set->numhitboxes].hitboxname, 0, sizeof(set->hitbox[set->numhitboxes].hitboxname));

    // Grab the hit box name if present:
    if (TokenAvailable()) {
        GetToken(false);
        strcpyn(set->hitbox[set->numhitboxes].hitboxname, token);
    }


    set->numhitboxes++;
}

void Cmd_HitboxSet() {
    // Add a new hitboxset
    g_StudioMdlContext.hitboxsets.emplace_back();
    s_hitboxset *set = &g_StudioMdlContext.hitboxsets.back();
    GetToken(false);
    memset(set, 0, sizeof(*set));
    strcpy(set->hitboxsetname, token);
}

//-----------------------------------------------------------------------------
// Assigns a default surface property to the entire model
//-----------------------------------------------------------------------------
void SetDefaultSurfaceProp(const char *pSurfaceProperty) {
    Q_strncpy(g_StudioMdlContext.pDefaultSurfaceProp, pSurfaceProperty, sizeof(g_StudioMdlContext.pDefaultSurfaceProp));
}

void Cmd_SurfaceProp() {
    GetToken(false);
    SetDefaultSurfaceProp(token);
}

//-----------------------------------------------------------------------------
// Assigns a surface property to a particular joint
//-----------------------------------------------------------------------------
void Cmd_JointSurfaceProp() {
    // Get joint name...
    GetToken(false);

    char pJointName[MAX_PATH];
    Q_strncpy(pJointName, token, sizeof(pJointName));

    // surface property name
    GetToken(false);
    AddSurfaceProp(pJointName, token);
}

//-----------------------------------------------------------------------------
// Assigns a default contents to the entire model
//-----------------------------------------------------------------------------
void Cmd_Contents() {
    int nAddFlags, nRemoveFlags;
    ParseContents(&nAddFlags, &nRemoveFlags);
    s_nDefaultContents |= nAddFlags;
    s_nDefaultContents &= ~nRemoveFlags;
}

//-----------------------------------------------------------------------------
// Assigns contents to a particular joint
//-----------------------------------------------------------------------------
void Cmd_JointContents() {
    // Get joint name...
    GetToken(false);

    // Search for the name in our list
    int i;
    for (i = s_JointContents.Count(); --i >= 0;) {
        if (!stricmp(s_JointContents[i].m_pJointName, token)) {
            break;
        }
    }

    // Add new entry if we haven't seen this name before
    if (i < 0) {
        i = s_JointContents.AddToTail();
        strcpyn(s_JointContents[i].m_pJointName, token);
    }

    int nAddFlags, nRemoveFlags;
    ParseContents(&nAddFlags, &nRemoveFlags);
    s_JointContents[i].m_nContents = CONTENTS_SOLID;
    s_JointContents[i].m_nContents |= nAddFlags;
    s_JointContents[i].m_nContents &= ~nRemoveFlags;
}

void Cmd_BoneAlwaysSetup() {
    if (g_StudioMdlContext.createMakefile)
        return;

    g_BoneAlwaysSetup.emplace_back();

    // bone name
    GetToken(false);
    strcpyn(g_BoneAlwaysSetup.back().bonename, token);
}

void Internal_Cmd_Attachment(int nAttachmentTarget = g_numattachments) {
    if (g_StudioMdlContext.createMakefile)
        return;

    // name
    GetToken(false);
    strcpyn(g_attachment[nAttachmentTarget].name, token);

    // bone name
    GetToken(false);
    strcpyn(g_attachment[nAttachmentTarget].bonename, token);

    Vector tmp;

    // position
    GetToken(false);
    tmp.x = verify_atof(token);
    GetToken(false);
    tmp.y = verify_atof(token);
    GetToken(false);
    tmp.z = verify_atof(token);

    scale_vertex(tmp);
    // identity matrix
    AngleMatrix(QAngle(0, 0, 0), g_attachment[nAttachmentTarget].local);

    while (TokenAvailable()) {
        GetToken(false);

        if (stricmp(token, "absolute") == 0) {
            g_attachment[nAttachmentTarget].type |= IS_ABSOLUTE;
            AngleIMatrix(g_defaultrotation, g_attachment[nAttachmentTarget].local);
            // AngleIMatrix( Vector( 0, 0, 0 ), g_attachment[nAttachmentTarget].local );
        } else if (stricmp(token, "rigid") == 0) {
            g_attachment[nAttachmentTarget].type |= IS_RIGID;
        } else if (stricmp(token, "world_align") == 0) {
            g_attachment[nAttachmentTarget].flags |= ATTACHMENT_FLAG_WORLD_ALIGN;
        } else if (stricmp(token, "rotate") == 0) {
            QAngle angles;
            for (int i = 0; i < 3; ++i) {
                if (!TokenAvailable())
                    break;

                GetToken(false);
                angles[i] = verify_atof(token);
            }
            AngleMatrix(angles, g_attachment[nAttachmentTarget].local);
        } else if (stricmp(token, "x_and_z_axes") == 0) {
            int i;
            Vector xaxis, yaxis, zaxis;
            for (i = 0; i < 3; ++i) {
                if (!TokenAvailable())
                    break;

                GetToken(false);
                xaxis[i] = verify_atof(token);
            }
            for (i = 0; i < 3; ++i) {
                if (!TokenAvailable())
                    break;

                GetToken(false);
                zaxis[i] = verify_atof(token);
            }
            VectorNormalize(xaxis);
            VectorMA(zaxis, -DotProduct(zaxis, xaxis), xaxis, zaxis);
            VectorNormalize(zaxis);
            CrossProduct(zaxis, xaxis, yaxis);
            MatrixSetColumn(xaxis, 0, g_attachment[nAttachmentTarget].local);
            MatrixSetColumn(yaxis, 1, g_attachment[nAttachmentTarget].local);
            MatrixSetColumn(zaxis, 2, g_attachment[nAttachmentTarget].local);
            MatrixSetColumn(vec3_origin, 3, g_attachment[nAttachmentTarget].local);
        } else {
            TokenError("unknown attachment (%s) option: ", g_attachment[nAttachmentTarget].name, token);
        }
    }

    g_attachment[nAttachmentTarget].local[0][3] = tmp.x;
    g_attachment[nAttachmentTarget].local[1][3] = tmp.y;
    g_attachment[nAttachmentTarget].local[2][3] = tmp.z;

    if (nAttachmentTarget == g_numattachments)
        g_numattachments++;
}

void Cmd_RedefineAttachment() {
    // find a pre-existing attachment of the given name and re-populate its values

    if (g_StudioMdlContext.createMakefile)
        return;

    // name
    GetToken(false);

    UnGetToken();

    for (int n = 0; n < g_numattachments; n++) {
        if (!stricmp(token, g_attachment[n].name)) {
            Msg("Found pre-existing attachment matching name: %s\n", token);
            printf("Found pre-existing attachment matching name: %s\n", token);
            Internal_Cmd_Attachment(n);
            return;
        }
    }

    MdlError("Can't redefine attachment \"%s\" because it wasn't found.\n", token);

}

void Cmd_Attachment() {
    Internal_Cmd_Attachment(g_numattachments);
}

void Cmd_Renamebone() {
    // from
    GetToken(false);
    strcpyn(g_renamedbone[g_numrenamedbones].from, token);

    // to
    GetToken(false);
    strcpyn(g_renamedbone[g_numrenamedbones].to, token);

    g_numrenamedbones++;
}

void Cmd_StripBonePrefix() {
    if (g_numStripBonePrefixes < MAXSTUDIOSRCBONES) {
        GetToken(false);

        // make sure it's not a duplicate
        for (int k = 0; k < g_numStripBonePrefixes; k++) {
            if (!Q_strcmp(token, g_szStripBonePrefix[k])) {
                MdlWarning("Ignoring duplicate $bonestripprefix for token %s\n", token);
                return;
            }
        }

        strcpyn(g_szStripBonePrefix[g_numStripBonePrefixes], token);

        g_numStripBonePrefixes++;
    } else {
        MdlError("Too many bone strip prefixes!\n");
    }
}

void Cmd_RenameBoneSubstr() {
    // from
    GetToken(false);
    strcpyn(g_szRenameBoneSubstr[g_numRenameBoneSubstr].from, token);

    // to
    GetToken(false);
    strcpyn(g_szRenameBoneSubstr[g_numRenameBoneSubstr].to, token);

    g_numRenameBoneSubstr++;
}

int LookupXNode(const char *name) {
    int i;
    for (i = 1; i <= g_numxnodes; i++) {
        if (stricmp(name, g_xnodename[i]) == 0) {
            return i;
        }
    }
    g_xnodename[i] = strdup(name);
    g_numxnodes = i;
    return i;
}

void Cmd_Skiptransition() {
    int nskips = 0;
    int list[10];

    while (TokenAvailable()) {
        GetToken(false);
        list[nskips++] = LookupXNode(token);
    }

    for (int i = 0; i < nskips; i++) {
        for (int j = 0; j < nskips; j++) {
            if (list[i] != list[j]) {
                g_xnodeskip[g_numxnodeskips][0] = list[i];
                g_xnodeskip[g_numxnodeskips][1] = list[j];
                g_numxnodeskips++;
            }
        }
    }
}


//-----------------------------------------------------------------------------
//
// The following code is all related to LODs
//
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Checks to see if the model source was already loaded
//-----------------------------------------------------------------------------
static s_source_t *FindCachedSourceInDir(const char *dir, const char *name, const char *xext) {
    int i;
    if (xext[0]) {
        Q_snprintf(g_StudioMdlContext.szFilename, sizeof(g_StudioMdlContext.szFilename), "%s%s.%s", dir, name, xext);
        for (i = 0; i < g_numsources; i++) {
            if (!Q_stricmp(g_StudioMdlContext.szFilename, g_source[i]->filename))
                return g_source[i];
        }
    } else {
        static const char *s_exts[] = { "vrm", "dmx", "smd", "xml", "obj" };
        for (int e = 0; e < ARRAYSIZE(s_exts); e++) {
            Q_snprintf(g_StudioMdlContext.szFilename, sizeof(g_StudioMdlContext.szFilename), "%s%s.%s", dir, name, s_exts[e]);
            for (i = 0; i < g_numsources; i++) {
                if (!Q_stricmp(g_StudioMdlContext.szFilename, g_source[i]->filename))
                    return g_source[i];
            }
        }
    }
    return nullptr;
}

s_source_t *FindCachedSource(const char *name, const char *xext) {
    // primary: current pushd/popd directory
    s_source_t *pFound = FindCachedSourceInDir(cddir[numdirs], name, xext);
    if (pFound)
        return pFound;

    // fallback: $addsearchdir directories (last resort)
    for (int d = 0; d < g_numAddSearchDirs; d++) {
        pFound = FindCachedSourceInDir(g_addSearchDirs[d], name, xext);
        if (pFound)
            return pFound;
    }

    return nullptr;
}


//-----------------------------------------------------------------------------
// Parse replacemodel command, causes an LOD to use a new model
//-----------------------------------------------------------------------------

static void Cmd_ReplaceModel(LodScriptData_t &lodData) {
    int i = lodData.modelReplacements.AddToTail();
    CLodScriptReplacement_t &newReplacement = lodData.modelReplacements[i];

    // from
    GetToken(false);

    // Strip off extensions for the source...
    char *pDot = strrchr(token, '.');
    if (pDot) {
        *pDot = 0;
    }

    // If the token is a $rendermesh alias, resolve it to the underlying DMX filename for the
    // cache lookup, but keep the alias as the src name so GetLODSources can match it.
    s_rendermesh_def_t *pRmDef = FindRenderMeshDef(token);
    if (pRmDef) {
        char szResolved[MAX_PATH];
        Q_strncpy(szResolved, pRmDef->filename, sizeof(szResolved));
        char *pDot2 = strrchr(szResolved, '.');
        if (pDot2) *pDot2 = 0;
        if (!FindCachedSource(szResolved, "")) {
            TokenError("Unknown replace model '%s' (resolved from $rendermesh to '%s')\n", token, szResolved);
        }
    } else if (!FindCachedSource(token, "")) {
        // Fallback: search by basename for sources loaded under a $pushd context that
        // has since been popped. GetModelLODSource strips paths for comparison, so
        // the bare name in replacemodel will match correctly at build time.
        bool bFoundByBasename = false;
        for (int si = 0; si < g_numsources && !bFoundByBasename; si++) {
            if (!g_source[si]) continue;
            const char *pBase = strrchr(g_source[si]->filename, '/');
            const char *pBase2 = strrchr(g_source[si]->filename, '\\');
            if (pBase2 > pBase) pBase = pBase2;
            pBase = pBase ? pBase + 1 : g_source[si]->filename;
            char szBuf[MAX_PATH];
            Q_strncpy(szBuf, pBase, sizeof(szBuf));
            char *pDot2 = strrchr(szBuf, '.');
            if (pDot2) *pDot2 = 0;
            if (!Q_stricmp(szBuf, token))
                bFoundByBasename = true;
        }
        if (!bFoundByBasename) {
            TokenError("Unknown replace model '%s'\n", token);
        }
    }

    newReplacement.SetSrcName(token);

    // to
    GetToken(false);
    newReplacement.SetDstName(token);

    // check for "reverse"
    bool reverse = false;
    if (TokenAvailable() && GetToken(false)) {
        if (stricmp("reverse", token) == 0) {
            reverse = true;
        } else {
            TokenError("\"%s\" unexpected\n", token);
        }
    }

    // If the LOD system tells us to replace "blank", let's forget
    // we ever read this. Have to do it here so parsing works
    if (!stricmp(newReplacement.GetSrcName(), "blank")) {
        lodData.modelReplacements.FastRemove(i);
        return;
    }

    // Load the source right here baby! That way its bones will get converted
    if (!lodData.IsStrippedFromModel()) {
        s_rendermesh_def_t *pDstRmDef = FindRenderMeshDef(newReplacement.GetDstName());
        if (pDstRmDef) {
            if (!pDstRmDef->pOwnedSource) {
                s_source_t *pRaw = Load_Source(pDstRmDef->filename, "", reverse, false);
                if (pRaw) {
                    s_source_t *pOwned = CloneSourceGeometry(pRaw);
                    ApplyRenderMeshFilter(pOwned, pDstRmDef);
                    pDstRmDef->pOwnedSource = pOwned;
                }
            }
            newReplacement.m_pSource = pDstRmDef->pOwnedSource
                ? CloneSourceGeometry(pDstRmDef->pOwnedSource)
                : nullptr;
        } else {
            newReplacement.m_pSource = Load_Source(newReplacement.GetDstName(), "", reverse, false);
        }

        // replacemodel now requires a fully rigged mesh - weight transfer has been removed.
        // For static props there are no bone weights, so skip the check.
        if (newReplacement.m_pSource && !g_staticprop) {
            const s_source_t *pCheck = newReplacement.m_pSource;
            for (int v = 0; v < pCheck->m_GlobalVertices.Count(); v++) {
                if (pCheck->m_GlobalVertices[v].boneweight.numbones <= 0) {
                    MdlError("replacemodel \"%s\": vertex %d has no bone weights. "
                             "LOD meshes must be fully rigged (weight transfer has been removed).\n",
                             newReplacement.GetDstName(), v);
                }
            }
        }
    } else if (!g_StudioMdlContext.quiet) {
        printf("Stripped lod \"%s\" @ %.1f\n", newReplacement.GetDstName(), lodData.switchValue);
    }
}

//-----------------------------------------------------------------------------
// Parse removemodel command, causes an LOD to stop using a model
//-----------------------------------------------------------------------------

static void Cmd_RemoveModel(LodScriptData_t &lodData) {
    int i = lodData.modelReplacements.AddToTail();
    CLodScriptReplacement_t &newReplacement = lodData.modelReplacements[i];

    // from
    GetToken(false);

    // Strip off extensions...
    char *pDot = strrchr(token, '.');
    if (pDot)
        *pDot = 0;

    newReplacement.SetSrcName(token);

    // to
    newReplacement.SetDstName("");

    // If the LOD system tells us to replace "blank", let's forget
    // we ever read this. Have to do it here so parsing works
    if (!stricmp(newReplacement.GetSrcName(), "blank")) {
        lodData.modelReplacements.FastRemove(i);
    }
}

//-----------------------------------------------------------------------------
// Parse replacebone command, causes a part of an LOD model to use a different bone
//-----------------------------------------------------------------------------

static void Cmd_ReplaceBone(LodScriptData_t &lodData) {
    int i = lodData.boneReplacements.AddToTail();
    CLodScriptReplacement_t &newReplacement = lodData.boneReplacements[i];

    // from
    GetToken(false);
    newReplacement.SetSrcName(token);

    // to
    GetToken(false);
    newReplacement.SetDstName(token);
}

//-----------------------------------------------------------------------------
// Parse bonetreecollapse command, causes the entire subtree to use the same bone as the node
//-----------------------------------------------------------------------------

static void Cmd_BoneTreeCollapse(LodScriptData_t &lodData) {
    int i = lodData.boneTreeCollapses.AddToTail();
    CLodScriptReplacement_t &newCollapse = lodData.boneTreeCollapses[i];

    // from
    GetToken(false);
    newCollapse.SetSrcName(token);
}

//-----------------------------------------------------------------------------
// Parse replacematerial command, causes a material to be used in place of another
//-----------------------------------------------------------------------------

static void Cmd_ReplaceMaterial(LodScriptData_t &lodData) {
    int i = lodData.materialReplacements.AddToTail();
    CLodScriptReplacement_t &newReplacement = lodData.materialReplacements[i];

    // from
    GetToken(false);
    newReplacement.SetSrcName(token);

    // to
    GetToken(false);
    newReplacement.SetDstName(token);

    if (!lodData.IsStrippedFromModel()) {
        // make sure it goes into the master list
        UseTextureAsMaterial(LookupTexture(token));
    }
}

//-----------------------------------------------------------------------------
// Parse removemesh command, causes a mesh to not be used anymore
//-----------------------------------------------------------------------------

static void Cmd_RemoveMesh(LodScriptData_t &lodData) {
    int i = lodData.meshRemovals.AddToTail();
    CLodScriptReplacement_t &newReplacement = lodData.meshRemovals[i];

    // from
    GetToken(false);
    Q_FixSlashes(token);
    newReplacement.SetSrcName(token);
}

//-----------------------------------------------------------------------------
// Parse decimatemodel command - auto-decimates the named model via meshoptimizer
//-----------------------------------------------------------------------------

static void Cmd_GenerateLod(LodScriptData_t &lodData) {
    int i = lodData.generateLods.AddToTail();
    CLodScriptReplacement_t &newEntry = lodData.generateLods[i];

    // model name (matches body-part model, same convention as replacemodel src)
    GetToken(false);
    char *pDot = strrchr(token, '.');
    if (pDot) *pDot = 0;
    newEntry.SetSrcName(token);

    // decimation factor: 1.0 = full detail, 0.5 = 50% triangles
    if (!TokenAvailable()) {
        MdlError("decimatemodel: expected decimation factor after model name (%d) : %s\n",
                 g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
    }
    GetToken(false);
    float factor = verify_atof(token);
    if (factor <= 0.0f || factor > 1.0f) {
        MdlError("decimatemodel: decimation factor must be in range (0, 1.0] (%d) : %s\n",
                 g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
    }
    newEntry.m_flDecimationFactor = factor;

    if (lodData.IsStrippedFromModel() && !g_StudioMdlContext.quiet) {
        printf("Stripped decimatemodel \"%s\" @ %.1f\n", newEntry.GetSrcName(), lodData.switchValue);
    }
}

void Cmd_LOD(const char *cmdname) {
    if (gflags & STUDIOHDR_FLAGS_HASSHADOWLOD) {
        MdlError("Model can only have one $shadowlod and it must be the last lod in the " SRC_FILE_EXT " (%d) : %s\n",
                 g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
    }

    int i = g_ScriptLODs.AddToTail();
    LodScriptData_t &newLOD = g_ScriptLODs[i];

    if (g_ScriptLODs.Count() > MAX_NUM_LODS) {
        MdlError("Too many LODs (MAX_NUM_LODS==%d)\n", (int) MAX_NUM_LODS);
    }

    // Shadow lod reserves -1 as switch value
    // which uniquely identifies a shadow lod
    newLOD.switchValue = -1.0f;

    bool isShadowCall = (!stricmp(cmdname, "$shadowlod")) ? true : false;

    if (isShadowCall) {
        if (TokenAvailable()) {
            GetToken(false);
            MdlWarning("(%d) : %s:  Ignoring switch value on %s command line\n", cmdname, g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
        }

        // Disable facial animation by default
        newLOD.EnableFacialAnimation(false);
    } else {
        if (TokenAvailable()) {
            GetToken(false);
            newLOD.switchValue = verify_atof(token);
            if (newLOD.switchValue < 0.0f) {
                MdlError("Negative switch value reserved for $shadowlod (%d) : %s\n", g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
            }
        } else {
            MdlError("Expected LOD switch value (%d) : %s\n", g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
        }
    }

    GetToken(true);
    if (stricmp("{", token) != 0) {
        MdlError("\"{\" expected while processing %s (%d) : %s", cmdname, g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
    }

    // In case we are stripping all lods and it's not Lod0, strip it
    if (i && g_StudioMdlContext.stripLods)
        newLOD.StripFromModel(true);

    while (1) {
        GetToken(true);
        if (stricmp("replacemodel", token) == 0) {
            Cmd_ReplaceModel(newLOD);
        } else if (stricmp("decimatemodel", token) == 0) {
            Cmd_GenerateLod(newLOD);
        } else if (stricmp("removemodel", token) == 0) {
            Cmd_RemoveModel(newLOD);
        } else if (stricmp("replacebone", token) == 0) {
            Cmd_ReplaceBone(newLOD);
        } else if (stricmp("bonetreecollapse", token) == 0) {
            Cmd_BoneTreeCollapse(newLOD);
        } else if (stricmp("replacematerial", token) == 0) {
            Cmd_ReplaceMaterial(newLOD);
        } else if (stricmp("removemesh", token) == 0) {
            Cmd_RemoveMesh(newLOD);
        } else if (stricmp("nofacial", token) == 0) {
            newLOD.EnableFacialAnimation(false);
        } else if (stricmp("facial", token) == 0) {
            if (isShadowCall) {
                // facial animation has no reasonable purpose on a shadow lod
                TokenError("Facial animation is not allowed for $shadowlod\n");
            }

            newLOD.EnableFacialAnimation(true);
        } else if (stricmp("use_shadowlod_materials", token) == 0) {
            if (isShadowCall) {
                gflags |= STUDIOHDR_FLAGS_USE_SHADOWLOD_MATERIALS;
            }
        } else if (stricmp("}", token) == 0) {
            break;
        } else {
            MdlError("invalid input while processing %s (%d) : %s", cmdname, g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
        }
    }

    // If the LOD is stripped, then forget we saw it
    if (newLOD.IsStrippedFromModel()) {
        g_ScriptLODs.FastRemove(i);
    }
}

void Cmd_ShadowLOD() {
    if (!g_StudioMdlContext.quiet) {
        printf("Processing $shadowlod\n");
    }

    // Act like it's a regular lod entry
    Cmd_LOD("$shadowlod");

    // Mark .mdl as having shadow lod (we also check above that we have only one of these
    // and that it's the last entered lod )
    gflags |= STUDIOHDR_FLAGS_HASSHADOWLOD;
}


//-----------------------------------------------------------------------------
// A couple commands related to translucency sorting
//-----------------------------------------------------------------------------
void Cmd_Opaque() {
    // Force Opaque has precedence
    gflags |= STUDIOHDR_FLAGS_FORCE_OPAQUE;
    gflags &= ~STUDIOHDR_FLAGS_TRANSLUCENT_TWOPASS;
}

void Cmd_TranslucentTwoPass() {
    // Force Opaque has precedence
    if ((gflags & STUDIOHDR_FLAGS_FORCE_OPAQUE) == 0) {
        gflags |= STUDIOHDR_FLAGS_TRANSLUCENT_TWOPASS;
    }
}

//-----------------------------------------------------------------------------
// Indicates the model be rendered with ambient boost heuristic (first used on Alyx in Episode 1)
//-----------------------------------------------------------------------------
void Cmd_AmbientBoost() {
    gflags |= STUDIOHDR_FLAGS_AMBIENT_BOOST;
}

//-----------------------------------------------------------------------------
// Indicates the model contains a quad-only Catmull-Clark subd mesh
//-----------------------------------------------------------------------------
void Cmd_SubdivisionSurface() {
    gflags |= STUDIOHDR_FLAGS_SUBDIVISION_SURFACE;
}


//-----------------------------------------------------------------------------
// Indicates the model should not cast shadows (useful for first-person models as used in L4D)
//-----------------------------------------------------------------------------
void Cmd_DoNotCastShadows() {
    gflags |= STUDIOHDR_FLAGS_DO_NOT_CAST_SHADOWS;
}

//-----------------------------------------------------------------------------
// Indicates the model should cast texture-based shadows in vrad (NOTE: only applicable to prop_static)
//-----------------------------------------------------------------------------
void Cmd_CastTextureShadows() {
    gflags |= STUDIOHDR_FLAGS_CAST_TEXTURE_SHADOWS;
}


//-----------------------------------------------------------------------------
// Indicates the model should not fade out even if the level or fallback settings say to
//-----------------------------------------------------------------------------
void Cmd_NoForcedFade() {
    gflags |= STUDIOHDR_FLAGS_NO_FORCED_FADE;
}


//-----------------------------------------------------------------------------
// Indicates the model should not use the bone origin when calculating bboxes, sequence boxes, etc.
//-----------------------------------------------------------------------------
void Cmd_SkipBoneInBBox() {
    g_StudioMdlContext.useBoneInBBox = false;
}


//-----------------------------------------------------------------------------
// Indicates the model will lengthen the viseme check to always include two phonemes
//-----------------------------------------------------------------------------
void Cmd_ForcePhonemeCrossfade() {
    gflags |= STUDIOHDR_FLAGS_FORCE_PHONEME_CROSSFADE;
}

//-----------------------------------------------------------------------------
// Indicates the model should keep pre-defined bone lengths regardless of animation changes
//-----------------------------------------------------------------------------
void Cmd_LockBoneLengths() {
    g_StudioMdlContext.lockBoneLengths = true;
}

//-----------------------------------------------------------------------------
// Indicates the model should replace pre-defined bone bind poses
//-----------------------------------------------------------------------------
void Cmd_UnlockDefineBones() {
    g_StudioMdlContext.defineBonesLockedByDefault = false;
}

//-----------------------------------------------------------------------------
// Mark this model as obsolete so that it'll show the obsolete material in game.
//-----------------------------------------------------------------------------
void Cmd_Obsolete() {
    // Force Opaque has precedence
    gflags |= STUDIOHDR_FLAGS_OBSOLETE;
}

//-----------------------------------------------------------------------------
// The bones should be moved so that they center themselves on the verts they own.
//-----------------------------------------------------------------------------
void Cmd_CenterBonesOnVerts() {
    // force centering on bones
    g_StudioMdlContext.centerBonesOnVerts = true;
}

//-----------------------------------------------------------------------------
// How far back should simple motion extract pull back from the last frame
//-----------------------------------------------------------------------------
void Cmd_MotionExtractionRollBack() {
    GetToken(false);
    g_StudioMdlContext.defaultMotionRollback = atof(token);
}

//-----------------------------------------------------------------------------
// rules for breaking up long animations into multiple sub anims
//-----------------------------------------------------------------------------
void Cmd_SectionFrames() {
    GetToken(false);
    g_StudioMdlContext.sectionFrames = atof(token);
    GetToken(false);
    g_StudioMdlContext.minSectionFrameLimit = atoi(token);
}


//-----------------------------------------------------------------------------
// world space clamping boundaries for animations
//-----------------------------------------------------------------------------
void Cmd_ClampWorldspace() {
    GetToken(false);
    g_StudioMdlContext.vecMinWorldspace[0] = verify_atof(token);

    GetToken(false);
    g_StudioMdlContext.vecMinWorldspace[1] = verify_atof(token);

    GetToken(false);
    g_StudioMdlContext.vecMinWorldspace[2] = verify_atof(token);

    GetToken(false);
    g_StudioMdlContext.vecMaxWorldspace[0] = verify_atof(token);

    GetToken(false);
    g_StudioMdlContext.vecMaxWorldspace[1] = verify_atof(token);

    GetToken(false);
    g_StudioMdlContext.vecMaxWorldspace[2] = verify_atof(token);
}

//-----------------------------------------------------------------------------
// Purpose: force a specific parent child relationship
//-----------------------------------------------------------------------------

void Cmd_ForcedHierarchy() {
    // child name
    GetToken(false);
    strcpyn(g_forcedhierarchy[g_numforcedhierarchy].childname, token);

    // parent name
    GetToken(false);
    strcpyn(g_forcedhierarchy[g_numforcedhierarchy].parentname, token);

    g_numforcedhierarchy++;
}


//-----------------------------------------------------------------------------
// Purpose: insert a virtual bone between a child and parent (currently unsupported)
//-----------------------------------------------------------------------------

void Cmd_InsertHierarchy() {
    // child name
    GetToken(false);
    strcpyn(g_forcedhierarchy[g_numforcedhierarchy].childname, token);

    // subparent name
    GetToken(false);
    strcpyn(g_forcedhierarchy[g_numforcedhierarchy].subparentname, token);

    // parent name
    GetToken(false);
    strcpyn(g_forcedhierarchy[g_numforcedhierarchy].parentname, token);

    g_numforcedhierarchy++;
}


//-----------------------------------------------------------------------------
// Purpose: rotate a specific bone
//-----------------------------------------------------------------------------

void Cmd_ForceRealign() {
    // bone name
    GetToken(false);
    strcpyn(g_forcedrealign[g_numforcedrealign].name, token);

    // skip
    GetToken(false);

    // X axis
    GetToken(false);
    g_forcedrealign[g_numforcedrealign].rot.x = DEG2RAD(verify_atof(token));

    // Y axis
    GetToken(false);
    g_forcedrealign[g_numforcedrealign].rot.y = DEG2RAD(verify_atof(token));

    // Z axis
    GetToken(false);
    g_forcedrealign[g_numforcedrealign].rot.z = DEG2RAD(verify_atof(token));

    g_numforcedrealign++;
}


//-----------------------------------------------------------------------------
// Purpose: specify a bone to allow > 180 but < 360 rotation (forces a calculated "mid point" to rotation)
//-----------------------------------------------------------------------------

void Cmd_LimitRotation() {
    // bone name
    GetToken(false);
    strcpyn(g_limitrotation[g_numlimitrotation].name, token);

    while (TokenAvailable()) {
        // sequence name
        GetToken(false);
        strcpyn(g_limitrotation[g_numlimitrotation].sequencename[g_limitrotation[g_numlimitrotation].numseq++], token);
    }

    g_numlimitrotation++;
}

//-----------------------------------------------------------------------------
// Purpose: artist controlled sanity check for expected state of the model.
// The idea is to allow artists to anticipate and prevent content errors by adding 'qc asserts'
// into commonly iterated models. This could be anything from bones that are expected (or not)
// to polycounts, material references, etc.- It's just an "Assert" for content.
//-----------------------------------------------------------------------------
void Cmd_QCAssert() {
    //get the assert type
    GetToken(false);

    //Msg( "Validating QC Assert '%s'\n", token );

    //start building assert description line
    char szAssertLine[1024] = "QC Assert: ";
    strcat(szAssertLine, token);

    bool bQueryValue = false;

    if (!Q_stricmp(token, "boneexists")) {
        // bone name
        GetToken(false);
        char szBoneName[MAXSTUDIONAME];
        strcpyn(szBoneName, token);

        strcat(szAssertLine, " ");
        strcat(szAssertLine, szBoneName);

        // src name
        GetToken(false);
        s_source_t *pSrc = Load_Source(token, "");

        strcat(szAssertLine, " ");
        strcat(szAssertLine, token);
        for (int n = 0; n < pSrc->numbones; n++) {
            if (!Q_stricmp(szBoneName, pSrc->localBone[n].name)) {
                bQueryValue = true;
                break;
            }
        }
    } else if (!Q_stricmp(token, "importboneexists")) {
        // bone name
        GetToken(false);
        char szBoneName[MAXSTUDIONAME];
        strcpyn(szBoneName, token);

        strcat(szAssertLine, " ");
        strcat(szAssertLine, szBoneName);

        for (int n = 0; n < g_numimportbones; n++) {
            if (!Q_stricmp(szBoneName, g_importbone[n].name)) {
                bQueryValue = true;
                break;
            }
        }
    }

    // add more possible qc asserts here...


    // is the assert value positive or negative
    GetToken(false);
    strcat(szAssertLine, " ");
    strcat(szAssertLine, token);

    bool bAssertValue = !Q_stricmp(token, "true");

    // print the result
    strcat(szAssertLine, " RESULT: ");
    if (bQueryValue != bAssertValue) {
        strcat(szAssertLine, "[Fail]\n");

        //// show helpful message, if one exists
        //if ( TokenAvailable() )
        //{
        //	GetToken (false);
        //	strcat( szAssertLine, token );
        //}

        MdlError(szAssertLine);
    } else {
        strcat(szAssertLine, "[Success]\n");
    }
    printf(szAssertLine);
}

//-----------------------------------------------------------------------------
// Purpose: specify bones to store, even if nothing references them
//-----------------------------------------------------------------------------

void Cmd_DefineBone() {
    // bone name
    GetToken(false);
    strcpyn(g_importbone[g_numimportbones].name, token);

    // parent name
    GetToken(false);
    strcpyn(g_importbone[g_numimportbones].parent, token);

    g_importbone[g_numimportbones].bUnlocked = !g_StudioMdlContext.defineBonesLockedByDefault;

    GetToken(false);
    if (!V_strcmp(token, "unlocked")) {
        g_importbone[g_numimportbones].bUnlocked = true;
    } else if (!V_strcmp(token, "locked")) {
        g_importbone[g_numimportbones].bUnlocked = false;
    } else {
        UnGetToken();
    }

    Vector pos;
    QAngle angles;

    // default pos
    GetToken(false);
    pos.x = verify_atof(token);
    GetToken(false);
    pos.y = verify_atof(token);
    GetToken(false);
    pos.z = verify_atof(token);
    GetToken(false);
    angles.x = verify_atof(token);
    GetToken(false);
    angles.y = verify_atof(token);
    GetToken(false);
    angles.z = verify_atof(token);
    AngleMatrix(angles, pos, g_importbone[g_numimportbones].rawLocal);

    if (TokenAvailable()) {
        g_importbone[g_numimportbones].bPreAligned = true;
        // realign pos
        GetToken(false);
        pos.x = verify_atof(token);
        GetToken(false);
        pos.y = verify_atof(token);
        GetToken(false);
        pos.z = verify_atof(token);
        GetToken(false);
        angles.x = verify_atof(token);
        GetToken(false);
        angles.y = verify_atof(token);
        GetToken(false);
        angles.z = verify_atof(token);

        AngleMatrix(angles, pos, g_importbone[g_numimportbones].srcRealign);
    } else {
        SetIdentityMatrix(g_importbone[g_numimportbones].srcRealign);
    }

    g_numimportbones++;
}

//-----------------------------------------------------------------------------
// $rotatebone <bonename> <rx ry rz> [local|world]
//   Rotates a bone's bind pose. local (DEFAULT) = about the bone's local axes;
//   world = about the bone's own origin using world-aligned axes. Mesh stays
//   undeformed at rest and animations remain visually identical (see
//   ApplyBoneTransformEdits in simplify.cpp). Bone is resolved later, since the
//   global bone table does not exist yet at parse time.
//-----------------------------------------------------------------------------
void Cmd_RotateBone() {
    if (g_numbonetransformedits >= MAX_BONE_TRANSFORM_EDITS)
        TokenError("$rotatebone/$movebone: exceeded combined limit of %d\n", MAX_BONE_TRANSFORM_EDITS);

    s_bonetransformedit_t &e = g_bonetransformedit[g_numbonetransformedits];
    memset(&e, 0, sizeof(e));
    e.kind = BONEXFORM_ROTATE;
    e.space = BONEXFORM_LOCAL;        // rotate default = local

    GetToken(false);
    strcpyn(e.name, token);
    GetToken(false);
    e.v[0] = verify_atof(token);
    GetToken(false);
    e.v[1] = verify_atof(token);
    GetToken(false);
    e.v[2] = verify_atof(token);

    while (TokenAvailable()) {
        GetToken(false);
        if (!V_stricmp(token, "world"))
            e.space = BONEXFORM_WORLD;
        else if (!V_stricmp(token, "local"))
            e.space = BONEXFORM_LOCAL;
        else if (!V_stricmp(token, "ignorehitbox"))
            e.ignoreHitbox = true;
        else {
            UnGetToken();
            break;
        }
    }

    e.linecount = g_StudioMdlContext.iLinecount;
    g_numbonetransformedits++;
}

//-----------------------------------------------------------------------------
// $movebone <bonename> <x y z> [local|world] [moveweight <residualbone> [factor]]
//   Translates a bone's bind pose. world (DEFAULT) = along world/model axes;
//   local = along the bone's local axes. Optional moveweight transfers [factor]
//   ([0,1], default 0.55) of the moved bone's vertex weight to <residualbone>
//   (see ApplyMoveWeightQueue).
//-----------------------------------------------------------------------------
void Cmd_MoveBone() {
    if (g_numbonetransformedits >= MAX_BONE_TRANSFORM_EDITS)
        TokenError("$rotatebone/$movebone: exceeded combined limit of %d\n", MAX_BONE_TRANSFORM_EDITS);

    s_bonetransformedit_t &e = g_bonetransformedit[g_numbonetransformedits];
    memset(&e, 0, sizeof(e));
    e.kind = BONEXFORM_MOVE;
    e.space = BONEXFORM_WORLD;        // move default = world

    GetToken(false);
    strcpyn(e.name, token);
    GetToken(false);
    e.v[0] = verify_atof(token);
    GetToken(false);
    e.v[1] = verify_atof(token);
    GetToken(false);
    e.v[2] = verify_atof(token);

    while (TokenAvailable()) {
        GetToken(false);
        if (!V_stricmp(token, "world")) {
            e.space = BONEXFORM_WORLD;
        } else if (!V_stricmp(token, "local")) {
            e.space = BONEXFORM_LOCAL;
        } else if (!V_stricmp(token, "moveweight")) {
            e.hasMoveWeight = true;
            GetToken(false);
            strcpyn(e.residualbone, token);
            // optional factor: fraction of the moved bone's weight handed to the
            // residual bone (1 = full transfer, 0 = none). Omitted -> 0.55 default,
            // so legacy QC without the float keeps working.
            e.moveWeightFactor = 0.55f;
            if (TokenAvailable()) {
                GetToken(false);
                // only consume the token if it parses as a number; otherwise it is
                // a following keyword (ignorehitbox) - put it back
                char *endp = nullptr;
                float f = (float)strtod(token, &endp);
                if (endp != token && *endp == '\0') {
                    e.moveWeightFactor = f;
                    if (e.moveWeightFactor < 0.0f)
                        e.moveWeightFactor = 0.0f;
                    else if (e.moveWeightFactor > 1.0f)
                        e.moveWeightFactor = 1.0f;
                } else {
                    UnGetToken();
                }
            }
        } else if (!V_stricmp(token, "ignorehitbox")) {
            e.ignoreHitbox = true;
        } else {
            UnGetToken();
            break;
        }
    }

    e.linecount = g_StudioMdlContext.iLinecount;
    g_numbonetransformedits++;
}

//-----------------------------------------------------------------------------
// $aligneyes <name> { eyeball <name> [...] [offset <x y z>] [forward <x y z>] }
//   Auto-generates an attachment centered on the listed eyeballs. 'forward' is a
//   literal world-space direction (1 0 0 = world +X); if omitted/0 0 0 the averaged
//   eyeball mesh normals are used. All eyeballs must share one bone. Filled in later
//   by GenerateAlignEyesAttachments (simplify.cpp).
//-----------------------------------------------------------------------------
void Cmd_AlignEyes() {
    if (g_StudioMdlContext.createMakefile)
        return;

    if (g_numaligneyes >= MAX_ALIGNEYES)
        TokenError("$aligneyes: exceeded limit of %d\n", MAX_ALIGNEYES);

    s_aligneyes_t &e = g_aligneyes[g_numaligneyes];
    memset(&e, 0, sizeof(e));
    e.forward.Init(0, 0, 0);     // 0,0,0 = auto (use averaged eyeball normals)
    e.offset.Init(0, 0, 0);

    // attachment name
    GetToken(false);
    strcpyn(e.name, token);

    GetToken(true);
    if (stricmp(token, "{")) {
        MdlError("$aligneyes: expected '{', got '%s'\n", token);
        return;
    }

    while (GetToken(true)) {
        if (!stricmp(token, "}"))
            break;

        if (!stricmp(token, "eyeball")) {
            if (e.numeyeballs >= MAX_ALIGNEYES_EYEBALLS)
                TokenError("$aligneyes: too many eyeballs (max %d)\n", MAX_ALIGNEYES_EYEBALLS);
            GetToken(false);
            strcpyn(e.eyeballs[e.numeyeballs], token);
            e.numeyeballs++;
        } else if (!stricmp(token, "offset")) {
            GetToken(false); e.offset.x = verify_atof(token);
            GetToken(false); e.offset.y = verify_atof(token);
            GetToken(false); e.offset.z = verify_atof(token);
        } else if (!stricmp(token, "forward")) {
            GetToken(false); e.forward.x = verify_atof(token);
            GetToken(false); e.forward.y = verify_atof(token);
            GetToken(false); e.forward.z = verify_atof(token);
        } else {
            MdlError("$aligneyes: unknown option '%s'\n", token);
        }
    }

    if (e.numeyeballs == 0)
        MdlError("$aligneyes \"%s\": no eyeballs specified\n", e.name);

    // Reserve the attachment slot now to keep declaration order vs. $attachment;
    // GenerateAlignEyesAttachments fills it once bones/sources exist.
    if (g_numattachments >= (int)g_attachment.size())
        TokenError("$aligneyes: too many attachments\n");
    e.attachIndex = g_numattachments;
    s_attachment_t &att = g_attachment[g_numattachments];
    memset(&att, 0, sizeof(att));
    strcpyn(att.name, e.name);
    g_numattachments++;

    e.linecount = g_StudioMdlContext.iLinecount;
    g_numaligneyes++;
}

//-----------------------------------------------------------------------------
// $alignmouth <name> { flexgroup <name> | flexcontroller <name> [...] [offset ...] [forward ...] }
//   Like $aligneyes, but centers the attachment on the vertices deformed by the
//   listed flexes. 'flexgroup' matches a flexcontroller 'type' (group); 'flexcontroller'
//   names a single controller. Filled in by GenerateAlignMouthAttachments
//   (simplify.cpp).
//-----------------------------------------------------------------------------
void Cmd_AlignMouth() {
    if (g_StudioMdlContext.createMakefile)
        return;

    if (g_numalignmouth >= MAX_ALIGNMOUTH)
        TokenError("$alignmouth: exceeded limit of %d\n", MAX_ALIGNMOUTH);

    s_alignmouth_t &e = g_alignmouth[g_numalignmouth];
    memset(&e, 0, sizeof(e));
    e.forward.Init(0, 0, 0);     // 0,0,0 = auto (use averaged flex-vertex normals)
    e.offset.Init(0, 0, 0);

    // attachment name
    GetToken(false);
    strcpyn(e.name, token);

    GetToken(true);
    if (stricmp(token, "{")) {
        MdlError("$alignmouth: expected '{', got '%s'\n", token);
        return;
    }

    while (GetToken(true)) {
        if (!stricmp(token, "}"))
            break;

        if (!stricmp(token, "flexgroup")) {
            if (e.numgroups >= MAX_ALIGNMOUTH_TARGETS)
                TokenError("$alignmouth: too many flexgroups (max %d)\n", MAX_ALIGNMOUTH_TARGETS);
            GetToken(false);
            strcpyn(e.groups[e.numgroups], token);
            e.numgroups++;
        } else if (!stricmp(token, "flexcontroller")) {
            if (e.numcontrollers >= MAX_ALIGNMOUTH_TARGETS)
                TokenError("$alignmouth: too many flexcontrollers (max %d)\n", MAX_ALIGNMOUTH_TARGETS);
            GetToken(false);
            strcpyn(e.controllers[e.numcontrollers], token);
            e.numcontrollers++;
        } else if (!stricmp(token, "offset")) {
            GetToken(false); e.offset.x = verify_atof(token);
            GetToken(false); e.offset.y = verify_atof(token);
            GetToken(false); e.offset.z = verify_atof(token);
        } else if (!stricmp(token, "forward")) {
            GetToken(false); e.forward.x = verify_atof(token);
            GetToken(false); e.forward.y = verify_atof(token);
            GetToken(false); e.forward.z = verify_atof(token);
        } else {
            MdlError("$alignmouth: unknown option '%s'\n", token);
        }
    }

    if (e.numgroups == 0 && e.numcontrollers == 0)
        MdlError("$alignmouth \"%s\": no flexgroup/flexcontroller specified\n", e.name);

    // Reserve the attachment slot now to keep declaration order vs. $attachment;
    // GenerateAlignMouthAttachments fills it once flexes are resolved.
    if (g_numattachments >= (int)g_attachment.size())
        TokenError("$alignmouth: too many attachments\n");
    e.attachIndex = g_numattachments;
    s_attachment_t &att = g_attachment[g_numattachments];
    memset(&att, 0, sizeof(att));
    strcpyn(att.name, e.name);
    g_numattachments++;

    e.linecount = g_StudioMdlContext.iLinecount;
    g_numalignmouth++;
}

bool ParseJiggleAngleConstraint(s_jigglebone_t *jiggleInfo) {
    jiggleInfo->data.flags |= JIGGLE_HAS_ANGLE_CONSTRAINT;

    if (!GetToken(false)) {
        MdlError("$jigglebone: expecting angle value\n", g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
        return false;
    }

    jiggleInfo->data.angleLimit = verify_atof(token) * M_PI / 180.0f;

    return true;
}

bool ParseJiggleYawConstraint(s_jigglebone_t *jiggleInfo) {
    jiggleInfo->data.flags |= JIGGLE_HAS_YAW_CONSTRAINT;

    if (!GetToken(false)) {
        MdlError("$jigglebone: expecting minimum yaw value\n", g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
        return false;
    }

    jiggleInfo->data.minYaw = verify_atof(token) * M_PI / 180.0f;

    if (!GetToken(false)) {
        MdlError("$jigglebone: expecting maximum yaw value\n", g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
        return false;
    }

    jiggleInfo->data.maxYaw = verify_atof(token) * M_PI / 180.0f;

    return true;
}

bool ParseJigglePitchConstraint(s_jigglebone_t *jiggleInfo) {
    jiggleInfo->data.flags |= JIGGLE_HAS_PITCH_CONSTRAINT;

    if (!GetToken(false)) {
        MdlError("$jigglebone: expecting minimum pitch value\n", g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
        return false;
    }

    jiggleInfo->data.minPitch = verify_atof(token) * M_PI / 180.0f;

    if (!GetToken(false)) {
        MdlError("$jigglebone: expecting maximum pitch value\n", g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
        return false;
    }

    jiggleInfo->data.maxPitch = verify_atof(token) * M_PI / 180.0f;

    return true;
}

void Grab_AxisInterpBones() {
    char cmd[1024], tmp[1025];
    Vector basepos;
    s_axisinterpbone_t *pAxis = NULL;
    s_axisinterpbone_t *pBone = &g_axisinterpbones[g_numaxisinterpbones];

    while (GetLineInput()) {
        if (IsEnd(g_StudioMdlContext.szLine)) {
            return;
        }
        int i = sscanf(g_StudioMdlContext.szLine, "%1023s \"%[^\"]\" \"%[^\"]\" \"%[^\"]\" \"%[^\"]\" %d", cmd, pBone->bonename, tmp,
                       pBone->controlname, tmp, &pBone->axis);
        if (i == 6 && stricmp(cmd, "bone") == 0) {
            // printf( "\"%s\" \"%s\" \"%s\" \"%s\"\n", cmd, pBone->bonename, tmp, pBone->controlname );
            pAxis = pBone;
            pBone->axis = pBone->axis - 1;    // MAX uses 1..3, engine 0..2
            g_numaxisinterpbones++;
            pBone = &g_axisinterpbones[g_numaxisinterpbones];
        } else if (stricmp(cmd, "display") == 0) {
            // skip all display info
        } else if (stricmp(cmd, "type") == 0) {
            // skip all type info
        } else if (stricmp(cmd, "basepos") == 0) {
            i = sscanf(g_StudioMdlContext.szLine, "basepos %f %f %f", &basepos.x, &basepos.y, &basepos.z);
            // skip all type info
        } else if (stricmp(cmd, "axis") == 0) {
            Vector pos;
            QAngle rot;
            int j;
            i = sscanf(g_StudioMdlContext.szLine, "axis %d %f %f %f %f %f %f", &j, &pos[0], &pos[1], &pos[2], &rot[2], &rot[0], &rot[1]);
            if (i == 7) {
                pAxis->pos[j] = (basepos + pos) * g_currentscale;
                AngleQuaternion(rot, pAxis->quat[j]);
            }
        }
    }
}

//
// Parse common parameters.
// This assumes a token has already been read, and returns true if
// the token is recognized and parsed.
//
bool ParseCommonJiggle(s_jigglebone_t *jiggleInfo) {
    if (!stricmp(token, "tip_mass")) {
        if (!GetToken(false)) {
            return false;
        }

        jiggleInfo->data.tipMass = verify_atof(token);
    } else if (!stricmp(token, "length")) {
        if (!GetToken(false)) {
            return false;
        }

        jiggleInfo->data.length = verify_atof(token);
    } else if (!stricmp(token, "angle_constraint")) {
        if (ParseJiggleAngleConstraint(jiggleInfo) == false) {
            return false;
        }
    } else if (!stricmp(token, "yaw_constraint")) {
        if (ParseJiggleYawConstraint(jiggleInfo) == false) {
            return false;
        }
    } else if (!stricmp(token, "yaw_friction")) {
        if (!GetToken(false)) {
            return false;
        }

        jiggleInfo->data.yawFriction = verify_atof(token);
    } else if (!stricmp(token, "yaw_bounce")) {
        if (!GetToken(false)) {
            return false;
        }

        jiggleInfo->data.yawBounce = verify_atof(token);
    } else if (!stricmp(token, "pitch_constraint")) {
        if (ParseJigglePitchConstraint(jiggleInfo) == false) {
            return false;
        }
    } else if (!stricmp(token, "pitch_friction")) {
        if (!GetToken(false)) {
            return false;
        }

        jiggleInfo->data.pitchFriction = verify_atof(token);
    } else if (!stricmp(token, "pitch_bounce")) {
        if (!GetToken(false)) {
            return false;
        }

        jiggleInfo->data.pitchBounce = verify_atof(token);
    } else {
        // unknown token
        MdlError("$jigglebone: invalid syntax '%s'\n", token);
        return false;
    }

    return true;
}

//
// Parse parameters for is_rigid subsection
//
bool ParseRigidJiggle(s_jigglebone_t *jiggleInfo) {
    jiggleInfo->data.flags |= (JIGGLE_IS_RIGID | JIGGLE_HAS_LENGTH_CONSTRAINT);

    bool gotOpenBracket = false;
    while (true) {
        if (GetToken(true) == false) {
            MdlError("$jigglebone:is_rigid: parse error\n", g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
            return false;
        }

        if (!stricmp(token, "{")) {
            gotOpenBracket = true;
        } else if (!gotOpenBracket) {
            MdlError("$jigglebone:is_rigid: missing '{'\n", g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
            return false;
        } else if (!stricmp(token, "}")) {
            // definition complete
            break;
        } else if (ParseCommonJiggle(jiggleInfo) == false) {
            MdlError("$jigglebone:is_rigid: invalid syntax '%s'\n", token);
            return false;
        }
    }

    return true;
}


float ParseJiggleStiffness() {
    if (!GetToken(false)) {
        MdlError("$jigglebone: expecting stiffness value\n", g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
        return 0.0f;
    }

    float stiffness = verify_atof(token);

    const float minStiffness = 0.0f;
    const float maxStiffness = 1000.0f;

    return clamp(stiffness, minStiffness, maxStiffness);
}

float ParseJiggleDamping() {
    if (!GetToken(false)) {
        MdlError("$jigglebone: expecting damping value\n", g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
        return 0.0f;
    }

    float damping = verify_atof(token);

    const float minDamping = 0.0f;
    const float maxDamping = 10.0f;

    return clamp(damping, minDamping, maxDamping);
}

//
// Parse parameters for has_base_spring subsection
//
bool ParseBaseSpringJiggle(s_jigglebone_t *jiggleInfo) {
    jiggleInfo->data.flags |= JIGGLE_HAS_BASE_SPRING;

    bool gotOpenBracket = false;
    while (true) {
        if (GetToken(true) == false) {
            MdlError("$jigglebone:is_rigid: parse error\n", g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
            return false;
        }

        if (!stricmp(token, "{")) {
            gotOpenBracket = true;
        } else if (!gotOpenBracket) {
            MdlError("$jigglebone:is_rigid: missing '{'\n", g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
            return false;
        } else if (!stricmp(token, "}")) {
            // definition complete
            break;
        } else if (!stricmp(token, "stiffness")) {
            jiggleInfo->data.baseStiffness = ParseJiggleStiffness();
        } else if (!stricmp(token, "damping")) {
            jiggleInfo->data.baseDamping = ParseJiggleStiffness();
        } else if (!stricmp(token, "left_constraint")) {
            if (!GetToken(false)) {
                return false;
            }

            jiggleInfo->data.baseMinLeft = verify_atof(token);

            if (!GetToken(false)) {
                return false;
            }

            jiggleInfo->data.baseMaxLeft = verify_atof(token);
        } else if (!stricmp(token, "left_friction")) {
            if (!GetToken(false)) {
                return false;
            }

            jiggleInfo->data.baseLeftFriction = verify_atof(token);
        } else if (!stricmp(token, "up_constraint")) {
            if (!GetToken(false)) {
                return false;
            }

            jiggleInfo->data.baseMinUp = verify_atof(token);

            if (!GetToken(false)) {
                return false;
            }

            jiggleInfo->data.baseMaxUp = verify_atof(token);
        } else if (!stricmp(token, "up_friction")) {
            if (!GetToken(false)) {
                return false;
            }

            jiggleInfo->data.baseUpFriction = verify_atof(token);
        } else if (!stricmp(token, "forward_constraint")) {
            if (!GetToken(false)) {
                return false;
            }

            jiggleInfo->data.baseMinForward = verify_atof(token);

            if (!GetToken(false)) {
                return false;
            }

            jiggleInfo->data.baseMaxForward = verify_atof(token);
        } else if (!stricmp(token, "forward_friction")) {
            if (!GetToken(false)) {
                return false;
            }

            jiggleInfo->data.baseForwardFriction = verify_atof(token);
        } else if (!stricmp(token, "base_mass")) {
            if (!GetToken(false)) {
                return false;
            }

            jiggleInfo->data.baseMass = verify_atof(token);
        } else if (ParseCommonJiggle(jiggleInfo) == false) {
            MdlError("$jigglebone:has_base_spring: invalid syntax '%s'\n", token);
            return false;
        }
    }

    return true;
}


//
// Parse parameters for is_flexible subsection
//
bool ParseFlexibleJiggle(s_jigglebone_t *jiggleInfo) {
    jiggleInfo->data.flags |= (JIGGLE_IS_FLEXIBLE | JIGGLE_HAS_LENGTH_CONSTRAINT);

    bool gotOpenBracket = false;
    while (true) {
        if (GetToken(true) == false) {
            MdlError("$jigglebone:is_flexible: parse error\n", g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
            return false;
        }

        if (!stricmp(token, "{")) {
            gotOpenBracket = true;
        } else if (!gotOpenBracket) {
            MdlError("$jigglebone:is_flexible: missing '{'\n", g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
            return false;
        } else if (!stricmp(token, "}")) {
            // definition complete
            break;
        } else if (!stricmp(token, "yaw_stiffness")) {
            jiggleInfo->data.yawStiffness = ParseJiggleStiffness();
        } else if (!stricmp(token, "yaw_damping")) {
            jiggleInfo->data.yawDamping = ParseJiggleStiffness();
        } else if (!stricmp(token, "pitch_stiffness")) {
            jiggleInfo->data.pitchStiffness = ParseJiggleStiffness();
        } else if (!stricmp(token, "pitch_damping")) {
            jiggleInfo->data.pitchDamping = ParseJiggleStiffness();
        } else if (!stricmp(token, "along_stiffness")) {
            jiggleInfo->data.alongStiffness = ParseJiggleStiffness();
        } else if (!stricmp(token, "along_damping")) {
            jiggleInfo->data.alongDamping = ParseJiggleStiffness();
        } else if (!stricmp(token, "allow_length_flex")) {
            jiggleInfo->data.flags &= ~JIGGLE_HAS_LENGTH_CONSTRAINT;
        } else if (ParseCommonJiggle(jiggleInfo) == false) {
            MdlError("$jigglebone:is_flexible: invalid syntax '%s'\n", token);
            return false;
        }
    }

    return true;
}

//
// Parse $jigglebone parameters
//
void Cmd_JiggleBone() {
    // bone name
    GetToken(false);

    // If this bone already has a jigglebone definition, overwrite it in place
    // (last definition wins) instead of appending an orphaned duplicate entry.
    struct s_jigglebone_t *jiggleInfo = NULL;
    bool isDuplicate = false;
    for (int i = 0; i < g_numjigglebones; ++i) {
        if (!stricmp(token, g_jigglebones[i].bonename)) {
            MdlWarning("$jigglebone: '%s' already defined, overwriting previous definition\n", token);
            jiggleInfo = &g_jigglebones[i];
            isDuplicate = true;
            break;
        }
    }

    if (!isDuplicate) {
        jiggleInfo = &g_jigglebones[g_numjigglebones];
    }

    strcpyn(jiggleInfo->bonename, token);

    // default values
    memset(&jiggleInfo->data, 0, sizeof(mstudiojigglebone_t));
    jiggleInfo->data.length = 10.0f;
    jiggleInfo->data.yawStiffness = 100.0f;
    jiggleInfo->data.pitchStiffness = 100.0f;
    jiggleInfo->data.alongStiffness = 100.0f;
    jiggleInfo->data.baseStiffness = 100.0f;
    jiggleInfo->data.baseMinUp = -100.0f;
    jiggleInfo->data.baseMaxUp = 100.0f;
    jiggleInfo->data.baseMinLeft = -100.0f;
    jiggleInfo->data.baseMaxLeft = 100.0f;
    jiggleInfo->data.baseMinForward = -100.0f;
    jiggleInfo->data.baseMaxForward = 100.0f;

    bool gotOpenBracket = false;
    while (true) {
        if (GetToken(true) == false) {
            MdlError("$jigglebone: parse error\n", g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
            return;
        }

        if (!stricmp(token, "{")) {
            gotOpenBracket = true;
        } else if (!gotOpenBracket) {
            MdlError("$jigglebone: missing '{'\n", g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
            return;
        } else if (!stricmp(token, "}")) {
            // definition complete
            break;
        } else if (!stricmp(token, "is_flexible")) {
            if (ParseFlexibleJiggle(jiggleInfo) == false) {
                return;
            }
        } else if (!stricmp(token, "is_rigid")) {
            if (ParseRigidJiggle(jiggleInfo) == false) {
                return;
            }
        } else if (!stricmp(token, "has_base_spring")) {
            if (ParseBaseSpringJiggle(jiggleInfo) == false) {
                return;
            }
        } else {
            MdlError("$jigglebone: invalid syntax '%s'\n", token);
            return;
        }
    }

    if (!g_StudioMdlContext.quiet)
        Msg("Marking bone %s as a jiggle bone\n", jiggleInfo->bonename);

    if (!isDuplicate)
        g_numjigglebones++;
}


//-----------------------------------------------------------------------------
// Purpose: specify bones to store, even if nothing references them
//-----------------------------------------------------------------------------

void Cmd_IncludeModel() {
    GetToken(false);

    // Must reference a compiled .mdl; anything else (e.g. a source .dmx/.smd or a
    // bare name) cannot be included and would silently produce a broken reference.
    const int nLen = Q_strlen(token);
    if (nLen < 4 || Q_stricmp(token + nLen - 4, ".mdl") != 0) {
        MdlError("$includemodel: \"%s\" must end in .mdl\n", token);
    }

    strcpyn(g_includemodel[g_numincludemodels].name, "models/");
    strcat(g_includemodel[g_numincludemodels].name, token);
    g_numincludemodels++;
}

void Cmd_CD() {
    if (cdset)
        MdlError("Two $cd in one model");
    cdset = true;
    GetToken(false);
    strcpy(cddir[0], token);
    strcat(cddir[0], "/");
    numdirs = 0;
}

void Cmd_ContentRootRelative() {
    g_StudioMdlContext.bContentRootRelative = true;
}

void Cmd_CDMaterials() {
    while (TokenAvailable()) {
        GetToken(false);

        char szPath[512];
        Q_strncpy(szPath, token, sizeof(szPath));

        int len = strlen(szPath);
        if (len > 0 && szPath[len - 1] != '/' && szPath[len - 1] != '\\') {
            Q_strncat(szPath, "/", sizeof(szPath), COPY_ALL_CHARACTERS);
        }

        Q_FixSlashes(szPath);
        cdtextures[numcdtextures] = strdup(szPath);
        numcdtextures++;
    }
}

void Cmd_Pushd() {
    GetToken(false);

    strcpy(cddir[numdirs + 1], cddir[numdirs]);
    strcat(cddir[numdirs + 1], token);
    strcat(cddir[numdirs + 1], "/");
    numdirs++;
}

void Cmd_Popd() {
    if (numdirs > 0)
        numdirs--;
}


void Cmd_ExternalTextures() {
    MdlWarning("ignoring $externaltextures, obsolete...");
}

void Cmd_ClipToTextures() {
    clip_texcoords = 1;
}

void Cmd_CollapseBones() {
    // Bone collapse is now default-on. This command is kept as a no-op so
    // existing QC files that contain $collapsebones do not produce errors.
}

void Cmd_NoCollapseBones() {
    g_StudioMdlContext.no_collapse_bones = true;
}

void Cmd_SkinnedLODs() {
    g_bSkinnedLODs = true;
}

void Cmd_AlwaysCollapse() {
    GetToken(false);
    g_collapse.AddToTail(strdup(token));
}

void Cmd_DoNotCollapse() {
    GetToken(false);
    g_DoNotCollapse.AddToTail(strdup(token));
}

void Cmd_CalcTransitions() {
    g_StudioMdlContext.multistageGraph = true;
}

void ProcessStaticProp() {
    g_staticprop = true;
    gflags |= STUDIOHDR_FLAGS_STATIC_PROP;
}

void Cmd_StaticProp() {
    if (g_pStaticPropPoseSource) {
        MdlError("$staticprop cannot be used together with $staticproppose\n");
        return;
    }
    ProcessStaticProp();
}

void Cmd_StaticPropPose() {
    if (g_staticprop) {
        MdlError("$staticproppose cannot be used together with $staticprop\n");
        return;
    }

    GetToken(false);

    if (token[0] == '{') {
        // block form: $staticproppose { pose <file> <frame>  flex <name> [<value>] ... }
        while (GetToken(true)) {
            if (token[0] == '}')
                break;

            if (!Q_stricmp(token, "pose")) {
                GetToken(false);
                s_source_t *pSource = Load_Source(token, "", false, false);
                if (!pSource) {
                    MdlError("$staticproppose: failed to load pose source '%s'\n", token);
                    return;
                }
                GetToken(false);
                g_pStaticPropPoseSource = pSource;
                g_nStaticPropPoseFrame = atoi(token);
            } else if (!Q_stricmp(token, "flex")) {
                s_staticPropPoseFlexOverride_t ov;
                GetToken(false);
                Q_strncpy(ov.name, token, sizeof(ov.name));
                ov.value = 1.0f;
                if (TokenAvailable()) {
                    GetToken(false);
                    ov.value = (float) verify_atof(token);
                }
                g_staticPropPoseFlexOverrides.AddToTail(ov);
            } else {
                MdlWarning("$staticproppose: unknown token '%s', ignoring\n", token);
            }
        }

        if (!g_pStaticPropPoseSource) {
            MdlError("$staticproppose: block form requires a 'pose' line\n");
            return;
        }
    } else {
        // inline form: $staticproppose <file> <frame>
        s_source_t *pSource = Load_Source(token, "", false, false);
        if (!pSource) {
            MdlError("$staticproppose: failed to load pose source '%s'\n", token);
            return;
        }
        GetToken(false);
        g_pStaticPropPoseSource = pSource;
        g_nStaticPropPoseFrame = atoi(token);
    }

    ProcessStaticProp();
}

void Cmd_NoSequence() {
    g_nosequence = true;
}

void Cmd_ZBrush() {
    g_StudioMdlContext.ZBrush = true;
}

void Cmd_RealignBones() {
    g_realignbones = true;
}

void Cmd_BaseLOD() {
    Cmd_LOD("$lod");
}

//-----------------------------------------------------------------------------
// Key value block
//-----------------------------------------------------------------------------
static void AppendKeyValueText(std::vector<char> *pKeyValue, const char *pString) {
    int nLen = strlen(pString);
    for (int i = 0; i < nLen; ++i) { pKeyValue->emplace_back(pString[i]); }
}

int KeyValueTextSize(std::vector<char> *pKeyValue) {
    return pKeyValue->size();
}

const char *KeyValueText(std::vector<char> *pKeyValue) {
    return pKeyValue->data();
}

void Option_KeyValues(std::vector<char> *pKeyValue);

//-----------------------------------------------------------------------------
// Key value block!
//-----------------------------------------------------------------------------
void Option_KeyValues(std::vector<char> *pKeyValue) {
    // Simply read in the block between { }s as text
    // and plop it out unchanged into the .mdl file.
    // Make sure to respect the fact that we may have nested {}s
    int nLevel = 1;

    if (!GetToken(true))
        return;

    if (token[0] != '{')
        return;


    while (GetToken(true)) {
        if (!stricmp(token, "}")) {
            nLevel--;
            if (nLevel <= 0)
                break;
            AppendKeyValueText(pKeyValue, " }\n");
        } else if (!stricmp(token, "{")) {
            AppendKeyValueText(pKeyValue, "{\n");
            nLevel++;
        } else {
            // tokens inside braces are quoted
            if (nLevel > 1) {
                AppendKeyValueText(pKeyValue, "\"");
                AppendKeyValueText(pKeyValue, token);
                AppendKeyValueText(pKeyValue, "\" ");
            } else {
                AppendKeyValueText(pKeyValue, token);
                AppendKeyValueText(pKeyValue, " ");
            }
        }
    }

    if (nLevel >= 1) {
        TokenError("Keyvalue block missing matching braces.\n");
    }

}

void Cmd_KeyValues() {
    Option_KeyValues(&g_StudioMdlContext.KeyValueText);
}

void Cmd_ConstDirectionalLight() {
    gflags |= STUDIOHDR_FLAGS_CONSTANT_DIRECTIONAL_LIGHT_DOT;

    GetToken(false);
    g_constdirectionalightdot = (byte) (verify_atof(token) * 255.0f);
}

void Cmd_MinLOD() {
    GetToken(false);
    g_StudioMdlContext.minLod = atoi(token);

    // "minlod" rules over "allowrootlods"
    if (g_StudioMdlContext.numAllowedRootLODs > 0 && g_StudioMdlContext.numAllowedRootLODs < g_StudioMdlContext.minLod) {
        MdlWarning("$minlod %d overrides $allowrootlods %d, proceeding with $allowrootlods %d.\n", g_StudioMdlContext.minLod,
                   g_StudioMdlContext.numAllowedRootLODs, g_StudioMdlContext.minLod);
        g_StudioMdlContext.numAllowedRootLODs = g_StudioMdlContext.minLod;
    }
}

void Cmd_AllowRootLODs() {
    GetToken(false);
    g_StudioMdlContext.numAllowedRootLODs = atoi(token);

    // Root LOD restriction has to obey "minlod" request
    if (g_StudioMdlContext.numAllowedRootLODs > 0 && g_StudioMdlContext.numAllowedRootLODs < g_StudioMdlContext.minLod) {
        MdlWarning("$allowrootlods %d is conflicting with $minlod %d, proceeding with $allowrootlods %d.\n",
                   g_StudioMdlContext.numAllowedRootLODs, g_StudioMdlContext.minLod, g_StudioMdlContext.minLod);
        g_StudioMdlContext.numAllowedRootLODs = g_StudioMdlContext.minLod;
    }
}

void Cmd_BoneSaveFrame() {
    s_bonesaveframe_t tmp;

    // bone name
    GetToken(false);
    strcpyn(tmp.name, token);

    tmp.bSavePos = false;
    tmp.bSaveRot = false;
    tmp.bSaveRot64 = false;
    while (TokenAvailable()) {
        GetToken(false);
        if (stricmp("position", token) == 0) {
            tmp.bSavePos = true;
        } else if (stricmp("rotation", token) == 0) {
            tmp.bSaveRot = true;
        } else if (stricmp("rotation64", token) == 0) {
            tmp.bSaveRot64 = true;
        } else {
            MdlError("unknown option \"%s\" on $bonesaveframe : %s\n", token, tmp.name);
        }
    }

    g_bonesaveframe.AddToTail(tmp);
}


static bool s_bFlexClothBorderJoints = false;
QAngle s_angClothPrerotate(0, 0, 0);


void Cmd_SetDefaultFadeInTime() {
    if (!GetToken(false))
        return;

    g_StudioMdlContext.defaultFadeInTime = verify_atof(token);
}

void Cmd_SetDefaultFadeOutTime() {
    if (!GetToken(false))
        return;

    g_StudioMdlContext.defaultFadeOutTime = verify_atof(token);
}

void Cmd_LCaseAllSequences() {
    g_StudioMdlContext.lCaseAllSequences = true;
}

void Cmd_AllowActivityName() {
    if (!GetToken(false))
        return;

    g_StudioMdlContext.AllowedActivityNames.emplace_back(token);
}

void Cmd_CollisionPrecision() {
    if (!GetToken(false))
        return;

    g_StudioMdlContext.CollisionPrecision = verify_atof(token);
}


void Cmd_CollisionModel()  { DoCollisionModel(false); }
void Cmd_CollisionJoints() { DoCollisionModel(true);  }

void Cmd_ErrorOnSeqRemapFail() {
    g_StudioMdlContext.errorOnSeqRemapFail = true;
}


void Cmd_SetModelIntentionallyHasZeroSequences() {
    g_StudioMdlContext.modelIntentionallyHasZeroSequences = true;
}


void Cmd_BoneMerge() {
    if (g_StudioMdlContext.createMakefile)
        return;

    g_BoneMerge.emplace_back();

    // bone name
    GetToken(false);
    strcpyn(g_BoneMerge.back().bonename, token);
}

//-----------------------------------------------------------------------------
// Purpose: create named list of boneweights
//-----------------------------------------------------------------------------
void Option_Weightlist(s_weightlist_t *pweightlist) {
    int depth = 0;
    int i;

    pweightlist->numbones = 0;

    while (1) {
        if (depth > 0) {
            if (!GetToken(true)) {
                break;
            }
        } else {
            if (!TokenAvailable()) {
                break;
            } else {
                GetToken(false);
            }
        }

        if (endofscript) {
            if (depth != 0) {
                TokenError("missing }\n");
            }
            return;
        }
        if (stricmp("{", token) == 0) {
            depth++;
        } else if (stricmp("}", token) == 0) {
            depth--;
        } else if (stricmp("posweight", token) == 0) {
            i = pweightlist->numbones - 1;
            if (i < 0) {
                MdlError("Error with specifing bone Position weight \'%s:%s\'\n", pweightlist->name,
                         pweightlist->bonename[i]);
            }
            GetToken(false);
            pweightlist->boneposweight[i] = verify_atof(token);
            if (pweightlist->boneweight[i] == 0 && pweightlist->boneposweight[i] > 0) {
                MdlError("Non-zero Position weight with zero Rotation weight not allowed \'%s:%s %f %f\'\n",
                         pweightlist->name, pweightlist->bonename[i], pweightlist->boneweight[i],
                         pweightlist->boneposweight[i]);
            }
        } else {
            i = pweightlist->numbones++;
            if (i >= MAXWEIGHTSPERLIST) {
                TokenError("Too many bones (%d) in weightlist '%s'\n", i, pweightlist->name);
            }
            pweightlist->bonename[i] = strdup(token);
            GetToken(false);
            pweightlist->boneweight[i] = verify_atof(token);
            pweightlist->boneposweight[i] = pweightlist->boneweight[i];
        }

        if (depth < 0) {
            TokenError("missing {\n");
        }
    };
}

void Cmd_Weightlist() {
    int i;

    if (!GetToken(false))
        return;

    if (g_numweightlist >= MAXWEIGHTLISTS) {
        TokenError("Too many weightlist commands (%d)\n", MAXWEIGHTLISTS);
    }

    for (i = 1; i < g_numweightlist; i++) {
        if (stricmp(g_weightlist[i].name, token) == 0) {
            TokenError("Duplicate weightlist '%s'\n", token);
        }
    }

    strcpyn(g_weightlist[i].name, token);

    Option_Weightlist(&g_weightlist[g_numweightlist]);

    g_numweightlist++;
}

void Cmd_DefaultWeightlist() {
    Option_Weightlist(&g_weightlist[0]);
}

/*
=================
Cmd_Origin
=================
*/
void Cmd_Origin() {
    GetToken(false);
    g_defaultadjust.x = verify_atof(token);

    GetToken(false);
    g_defaultadjust.y = verify_atof(token);

    GetToken(false);
    g_defaultadjust.z = verify_atof(token);

    if (TokenAvailable()) {
        GetToken(false);
        g_defaultrotation.z = DEG2RAD(verify_atof(token) + 90);
    }
}

//-----------------------------------------------------------------------------
// Purpose: Set the default root rotation so that the Y axis is up instead of the Z axis (for Maya)
//-----------------------------------------------------------------------------
void ProcessUpAxis(const RadianEuler &angles) {
    g_defaultrotation = angles;
}


//-----------------------------------------------------------------------------
// Purpose: Set the default root rotation so that the Y axis is up instead of the Z axis (for Maya)
//-----------------------------------------------------------------------------
void Cmd_UpAxis() {
    // We want to create a rotation that rotates from the art space
    // (specified by the up direction) to a z up space
    // Note: x, -x, -y are untested
    RadianEuler angles(0.0f, 0.0f, M_PI / 2.0f);
    GetToken(false);
    if (!Q_stricmp(token, "x")) {
        // rotate 90 degrees around y to move x into z
        angles.x = 0.0f;
        angles.y = M_PI / 2.0f;
    } else if (!Q_stricmp(token, "-x")) {
        // untested
        angles.x = 0.0f;
        angles.y = -M_PI / 2.0f;
    } else if (!Q_stricmp(token, "y")) {
        // rotate 90 degrees around x to move y into z
        angles.x = M_PI / 2.0f;
        angles.y = 0.0f;
    } else if (!Q_stricmp(token, "-y")) {
        // untested
        angles.x = -M_PI / 2.0f;
        angles.y = 0.0f;
    } else if (!Q_stricmp(token, "z")) {
        // there's still a built in 90 degree Z rotation :(
        angles.x = 0.0f;
        angles.y = 0.0f;
    } else if (!Q_stricmp(token, "-z")) {
        // there's still a built in 90 degree Z rotation :(
        angles.x = 0.0f;
        angles.y = 0.0f;
    } else {
        TokenError("unknown $upaxis option: \"%s\"\n", token);
        return;
    }

    ProcessUpAxis(angles);
}


void Cmd_ScaleUp() {
    GetToken(false);
    g_defaultscale = verify_atof(token);

    g_currentscale = g_defaultscale;
}

//-----------------------------------------------------------------------------
// Purpose: Sets how what size chunks to cut the animations into
//-----------------------------------------------------------------------------
void Cmd_AnimBlockSize() {
    GetToken(false);
    g_animblocksize = verify_atoi(token);
    if (g_animblocksize < 1024) {
        g_animblocksize *= 1024;
    }
    while (TokenAvailable()) {
        GetToken(false);
        if (!Q_stricmp(token, "nostall")) {
            g_StudioMdlContext.noAnimblockStall = true;
        } else if (!Q_stricmp(token, "highres")) {
            g_StudioMdlContext.animblockHighRes = true;
            g_StudioMdlContext.animblockLowRes = false;
        } else if (!Q_stricmp(token, "lowres")) {
            g_StudioMdlContext.animblockLowRes = true;
            g_StudioMdlContext.animblockHighRes = false;
        } else if (!Q_stricmp(token, "numframes")) {
            GetToken(false);
            g_StudioMdlContext.maxZeroFrames = clamp(atoi(token), 1, 4);
        } else if (!Q_stricmp(token, "cachehighres")) {
            g_StudioMdlContext.zeroFramesHighres = true;
        } else if (!Q_stricmp(token, "posdelta")) {
            GetToken(false);
            g_StudioMdlContext.minZeroFramePosDelta = atof(token);
        } else {
            MdlError("unknown option \"%s\" on $animblocksize command\n");
        }
    }
}

void Cmd_AppendSource() {
    if (!GetToken(false))
        return;

    s_source_t *pOrigSource = g_model[0]->source;
    s_source_t *pAppendSource = Load_Source(token, "", false, false,
                                            false /* don't use cached lookup, since this might be a dup of the starting src */ );

    matrix3x4_t matTemp;
    matTemp.SetToIdentity();
    matTemp.ScaleUpper3x3Matrix(g_currentscale);

    if (TokenAvailable()) {
        GetToken(false);

        if (!V_strncmp(token, "offset", 6)) {

            Vector vecOffsetPosition;
            vecOffsetPosition.Init();
            QAngle angOffsetAngle;
            angOffsetAngle.Init();
            float flScale = 1;

            int nCount = sscanf(token, "offset pos[ %f %f %f ] angle[ %f %f %f ] scale[ %f ]",
                                &vecOffsetPosition.x, &vecOffsetPosition.y, &vecOffsetPosition.z,
                                &angOffsetAngle.x, &angOffsetAngle.y, &angOffsetAngle.z,
                                &flScale);

            if (nCount == 7) {
                AngleMatrix(angOffsetAngle, vecOffsetPosition, matTemp);
                matTemp.ScaleUpper3x3Matrix(flScale * (1.0f / g_currentscale));
            } else {
                MdlError("Malformed offset parameters to $appendsource.");
                return;
            }

        } else {
            UnGetToken();
        }
    }

    AddSrcToSrc(pOrigSource, pAppendSource, matTemp);
}

// $modelbudget { ... } -- sets user-facing *soft* resource limits. Each value is
// clamped to [1, hard cap], where the hard cap is the matching compile-time #define.
// Replaces the legacy $maxverts / $maxbones commands.
static int ReadBudgetValue(int hardCap) {
    GetToken(false);
    return clamp(atoi(token), 1, hardCap);
}

void Cmd_ModelBudget() {
    int is_started = 0;

    do {
        GetToken(true);
        if (endofscript)
            return;
        else if (token[0] == '{') {
            is_started = 1;
        } else if (token[0] == '}') {
            break;
        } else if (stricmp("totalverts", token) == 0) {
            g_StudioMdlContext.budgetTotalVerts = ReadBudgetValue(MAXSTUDIOVERTS);
        } else if (stricmp("bodyverts", token) == 0) {
            // first value: split limit; clamped to [1, MAXSTUDIOVERTS/2]
            g_StudioMdlContext.maxVertexLimit = ReadBudgetValue(MAXSTUDIOVERTS / 2);
            g_StudioMdlContext.maxVertexClamp = MIN(g_StudioMdlContext.maxVertexLimit, MAXSTUDIOVERTS / 2);
            if (TokenAvailable()) {
                // optional second value: explicit clamp
                g_StudioMdlContext.maxVertexClamp = ReadBudgetValue(MAXSTUDIOVERTS / 2);
            }
        } else if (stricmp("bones", token) == 0) {
            g_StudioMdlContext.maxBoneLimit = ReadBudgetValue(MAXSTUDIOBONES);
        } else if (stricmp("materials", token) == 0) {
            g_StudioMdlContext.budgetMaterials = ReadBudgetValue(MAXSTUDIOSKINS);
        } else if (stricmp("flexcontroller", token) == 0) {
            g_StudioMdlContext.budgetFlexControllers = ReadBudgetValue(MAXSTUDIOFLEXCTRL);
        } else if (stricmp("flexmorph", token) == 0) {
            g_StudioMdlContext.budgetFlexMorph = ReadBudgetValue(MAXSTUDIOFLEXDESC);
        } else if (stricmp("flexmorphverts", token) == 0) {
            g_StudioMdlContext.budgetFlexMorphVerts = ReadBudgetValue(MAXSTUDIOFLEXVERTS);
        } else if (stricmp("flexrules", token) == 0) {
            g_StudioMdlContext.budgetFlexRules = ReadBudgetValue(MAXSTUDIOFLEXRULES);
        } else if (stricmp("poseparam", token) == 0) {
            g_StudioMdlContext.budgetPoseParam = ReadBudgetValue(MAXSTUDIOPOSEPARAM);
        } else if (stricmp("boneconstraints", token) == 0) {
            g_StudioMdlContext.budgetBoneConstraints = ReadBudgetValue(MAXSTUDIOBONECONSTRAINTS);
        } else if (stricmp("sequence", token) == 0) {
            g_StudioMdlContext.budgetSequences = ReadBudgetValue(MAXSTUDIOSEQUENCES);
        } else if (stricmp("animation", token) == 0) {
            g_StudioMdlContext.budgetAnimations = ReadBudgetValue(MAXSTUDIOANIMS);
        } else {
            MdlError("unknown $modelbudget parameter: \"%s\"\n", token);
        }
    } while (1);

    (void) is_started;
}

s_sequence_t *LookupSequence(const char *name) {
    int i;
    for (i = 0; i < g_sequence.Count(); ++i) {
        if (!Q_stricmp(g_sequence[i].name, name))
            return &g_sequence[i];
    }
    return nullptr;
}


s_animation_t *LookupAnimation(const char *name, int nFallbackRecursionDepth) {
    int i;
    for (i = 0; i < g_numani; i++) {
        if (!Q_stricmp(g_panimation[i]->name, name))
            return g_panimation[i];
    }

    s_sequence_t *pseq = LookupSequence(name);

    // Used to just return pseq->panim[0][0] but pseq->panim is
    // a CUtlVectorAuto which expands the array on access as necessary
    // but seems to fill it with random data on expansion, so prevent
    // that here because we're doing a lookup to see if something
    // already exists
    if (pseq && pseq->panim.Count() > 0) {
        CUtlVectorAuto<s_animation_t *> &animList = pseq->panim[0];
        if (animList.Count() > 0)
            return animList[0];
    }

    // check optional fallbacks or reserved name syntax

    if (nFallbackRecursionDepth == 0 && !V_strcmp(name, "this")) {
        return LookupAnimation(g_StudioMdlContext.szInCurrentSeqName, 1);
    }

    return nullptr;
}

s_animation_t *LookupAnimation(const char *name) {
    return LookupAnimation(name, 0);
}


int Option_AnimTag(s_sequence_t *psequence) {
    if (psequence->numanimtags + 1 >= MAXSTUDIOTAGS) {
        TokenError("too many animtags\n");
    }

    GetToken(false);

    strcpy(psequence->animtags[psequence->numanimtags].tagname, token);

    GetToken(false);
    psequence->animtags[psequence->numanimtags].cycle = verify_atof(token);

    psequence->numanimtags++;

    return 0;
}

int Option_Event(s_sequence_t *psequence) {
    if (psequence->numevents + 1 >= MAXSTUDIOEVENTS) {
        TokenError("too many events\n");
    }

    GetToken(false);

    strcpy(psequence->event[psequence->numevents].eventname, token);

    GetToken(false);
    psequence->event[psequence->numevents].frame = verify_atoi(token);

    psequence->numevents++;

    // option token
    if (TokenAvailable()) {
        GetToken(false);
        if (token[0] == '}') // opps, hit the end
            return 1;
        // found an option
        strcpyn(psequence->event[psequence->numevents - 1].options, token);
    }

    return 0;
}


void Option_IKRule(s_ikrule_t *pRule) {
    // chain
    GetToken(false);

    int i;
    for (i = 0; i < g_numikchains; i++) {
        if (stricmp(token, g_ikchain[i].name) == 0) {
            break;
        }
    }
    if (i >= g_numikchains) {
        TokenError("unknown chain \"%s\" in ikrule\n", token);
    }
    pRule->chain = i;
    // default slot
    pRule->slot = i;

    // type
    GetToken(false);

    if (stricmp(token, "autosteps") == 0) {
        GetToken(false);
        pRule->end = verify_atoi(token);

        GetToken(false);
        strcpyn(pRule->bonename, token);

        pRule->type = IK_GROUND;

        pRule->height = g_ikchain[pRule->chain].height;
        pRule->floor = g_ikchain[pRule->chain].floor;
        pRule->radius = g_ikchain[pRule->chain].radius;

        pRule->start = -2;
        pRule->peak = -1;
        pRule->tail = -1;
    } else if (stricmp(token, "touch") == 0) {
        pRule->type = IK_SELF;

        // bone
        GetToken(false);
        strcpyn(pRule->bonename, token);
    } else if (stricmp(token, "footstep") == 0) {
        pRule->type = IK_GROUND;

        pRule->height = g_ikchain[pRule->chain].height;
        pRule->floor = g_ikchain[pRule->chain].floor;
        pRule->radius = g_ikchain[pRule->chain].radius;
    } else if (stricmp(token, "attachment") == 0) {
        pRule->type = IK_ATTACHMENT;

        // name of attachment
        GetToken(false);
        strcpyn(pRule->attachment, token);
    } else if (stricmp(token, "release") == 0) {
        pRule->type = IK_RELEASE;
    } else if (stricmp(token, "unlatch") == 0) {
        pRule->type = IK_UNLATCH;
    }

    pRule->contact = -1;

    while (TokenAvailable()) {
        GetToken(false);
        if (stricmp(token, "height") == 0) {
            GetToken(false);
            pRule->height = verify_atof(token);
        } else if (stricmp(token, "target") == 0) {
            // slot
            GetToken(false);
            pRule->slot = verify_atoi(token);
        } else if (stricmp(token, "range") == 0) {
            // ramp
            GetToken(false);
            if (token[0] == '.')
                pRule->start = -1;
            else
                pRule->start = verify_atoi(token);

            GetToken(false);
            if (token[0] == '.')
                pRule->peak = -1;
            else
                pRule->peak = verify_atoi(token);

            GetToken(false);
            if (token[0] == '.')
                pRule->tail = -1;
            else
                pRule->tail = verify_atoi(token);

            GetToken(false);
            if (token[0] == '.')
                pRule->end = -1;
            else
                pRule->end = verify_atoi(token);
        } else if (stricmp(token, "floor") == 0) {
            GetToken(false);
            pRule->floor = verify_atof(token);
        } else if (stricmp(token, "pad") == 0) {
            GetToken(false);
            pRule->radius = verify_atof(token) / 2.0f;
        } else if (stricmp(token, "radius") == 0) {
            GetToken(false);
            pRule->radius = verify_atof(token);
        } else if (stricmp(token, "contact") == 0) {
            GetToken(false);
            pRule->contact = verify_atoi(token);
        } else if (stricmp(token, "usesequence") == 0) {
            pRule->usesequence = true;
            pRule->usesource = false;
        } else if (stricmp(token, "usesource") == 0) {
            pRule->usesequence = false;
            pRule->usesource = true;
        } else if (stricmp(token, "fakeorigin") == 0) {
            GetToken(false);
            pRule->pos.x = verify_atof(token);
            GetToken(false);
            pRule->pos.y = verify_atof(token);
            GetToken(false);
            pRule->pos.z = verify_atof(token);

            pRule->bone = -1;
        } else if (stricmp(token, "fakerotate") == 0) {
            QAngle ang;

            GetToken(false);
            ang.x = verify_atof(token);
            GetToken(false);
            ang.y = verify_atof(token);
            GetToken(false);
            ang.z = verify_atof(token);

            AngleQuaternion(ang, pRule->q);

            pRule->bone = -1;
        } else if (stricmp(token, "bone") == 0) {
            strcpy(pRule->bonename, token);
        } else {
            UnGetToken();
            return;
        }
    }
}


float verify_atof_with_null(const char *token) {
    if (strcmp(token, "..") == 0)
        return -1;

    if (token[0] != '-' && token[0] != '.' && (token[0] < '0' || token[0] > '9')) {
        TokenError("expecting float, got \"%s\"\n", token);
    }
    return atof(token);
}


//-----------------------------------------------------------------------------
// Purpose: parse order dependant s_animcmd_t token for $animations
//-----------------------------------------------------------------------------
int ParseCmdlistToken(int &numcmds, s_animcmd_t *cmds) {
    if (numcmds >= MAXSTUDIOCMDS) {
        return false;
    }
    s_animcmd_t *pcmd = &cmds[numcmds];
    if (stricmp("fixuploop", token) == 0) {
        pcmd->cmd = CMD_FIXUP;

        GetToken(false);
        pcmd->u.fixuploop.start = verify_atoi(token);
        GetToken(false);
        pcmd->u.fixuploop.end = verify_atoi(token);
    } else if (strnicmp("weightlist", token, 6) == 0) {
        GetToken(false);

        int i;
        for (i = 1; i < g_numweightlist; i++) {
            if (stricmp(g_weightlist[i].name, token) == 0) {
                break;
            }
        }
        if (i == g_numweightlist) {
            TokenError("unknown weightlist '%s\'\n", token);
        }
        pcmd->cmd = CMD_WEIGHTS;
        pcmd->u.weightlist.index = i;
    } else if (stricmp("subtract", token) == 0) {
        pcmd->cmd = CMD_SUBTRACT;

        GetToken(false);

        s_animation_t *extanim = LookupAnimation(token);
        if (extanim == NULL) {
            TokenError("unknown subtract animation '%s\'\n", token);
        }

        pcmd->u.subtract.ref = extanim;

        GetToken(false);
        pcmd->u.subtract.frame = verify_atoi(token);

        pcmd->u.subtract.flags |= STUDIO_POST;
    } else if (stricmp("presubtract", token) == 0) // FIXME: rename this to something better
    {
        pcmd->cmd = CMD_SUBTRACT;

        GetToken(false);

        s_animation_t *extanim = LookupAnimation(token);
        if (extanim == NULL) {
            TokenError("unknown presubtract animation '%s\'\n", token);
        }

        pcmd->u.subtract.ref = extanim;

        GetToken(false);
        pcmd->u.subtract.frame = verify_atoi(token);
    } else if (stricmp("alignto", token) == 0) {
        pcmd->cmd = CMD_AO;

        pcmd->u.ao.pBonename = NULL;

        GetToken(false);
        s_animation_t *extanim = LookupAnimation(token);
        if (extanim == NULL) {
            TokenError("unknown alignto animation '%s\'\n", token);
        }

        pcmd->u.ao.ref = extanim;
        pcmd->u.ao.motiontype = STUDIO_X | STUDIO_Y;
        pcmd->u.ao.srcframe = 0;
        pcmd->u.ao.destframe = 0;
    } else if (stricmp("align", token) == 0) {
        pcmd->cmd = CMD_AO;

        pcmd->u.ao.pBonename = NULL;

        GetToken(false);
        s_animation_t *extanim = LookupAnimation(token);
        if (extanim == NULL) {
            TokenError("unknown align animation '%s\'\n", token);
        }

        pcmd->u.ao.ref = extanim;

        // motion type to match
        pcmd->u.ao.motiontype = 0;
        GetToken(false);
        int ctrl;
        while ((ctrl = lookupControl(token)) != -1) {
            pcmd->u.ao.motiontype |= ctrl;
            GetToken(false);
        }
        if (pcmd->u.ao.motiontype == 0) {
            TokenError("missing controls on align\n");
        }

        // frame of reference animation to match
        pcmd->u.ao.srcframe = verify_atoi(token);

        // against what frame of the current animation
        GetToken(false);
        pcmd->u.ao.destframe = verify_atoi(token);
    } else if (stricmp("alignboneto", token) == 0) {
        pcmd->cmd = CMD_AO;

        GetToken(false);
        pcmd->u.ao.pBonename = strdup(token);

        GetToken(false);
        s_animation_t *extanim = LookupAnimation(token);
        if (extanim == NULL) {
            TokenError("unknown alignboneto animation '%s\'\n", token);
        }

        pcmd->u.ao.ref = extanim;
        pcmd->u.ao.motiontype = STUDIO_X | STUDIO_Y;
        pcmd->u.ao.srcframe = 0;
        pcmd->u.ao.destframe = 0;
    } else if (stricmp("alignbone", token) == 0) {
        pcmd->cmd = CMD_AO;

        GetToken(false);
        pcmd->u.ao.pBonename = strdup(token);

        GetToken(false);
        s_animation_t *extanim = LookupAnimation(token);
        if (extanim == NULL) {
            TokenError("unknown alignboneto animation '%s\'\n", token);
        }

        pcmd->u.ao.ref = extanim;

        // motion type to match
        pcmd->u.ao.motiontype = 0;
        GetToken(false);
        int ctrl;
        while ((ctrl = lookupControl(token)) != -1) {
            pcmd->u.ao.motiontype |= ctrl;
            GetToken(false);
        }
        if (pcmd->u.ao.motiontype == 0) {
            TokenError("missing controls on align\n");
        }

        // frame of reference animation to match
        pcmd->u.ao.srcframe = verify_atoi(token);

        // against what frame of the current animation
        GetToken(false);
        pcmd->u.ao.destframe = verify_atoi(token);
    } else if (stricmp("match", token) == 0) {
        pcmd->cmd = CMD_MATCH;

        GetToken(false);

        s_animation_t *extanim = LookupAnimation(token);
        if (extanim == NULL) {
            TokenError("unknown match animation '%s\'\n", token);
        }

        pcmd->u.match.ref = extanim;
    } else if (stricmp("matchblend", token) == 0) {
        pcmd->cmd = CMD_MATCHBLEND;

        GetToken(false);

        s_animation_t *extanim = LookupAnimation(token);
        if (extanim == NULL) {
            MdlError("unknown match animation '%s\'\n", token);
        }

        pcmd->u.match.ref = extanim;

        // frame of reference animation to match
        GetToken(false);
        pcmd->u.match.srcframe = verify_atoi(token);

        // against what frame of the current animation
        GetToken(false);
        pcmd->u.match.destframe = verify_atoi(token);

        // backup and starting match in here
        GetToken(false);
        pcmd->u.match.destpre = verify_atoi(token);

        // continue blending match till here
        GetToken(false);
        pcmd->u.match.destpost = verify_atoi(token);

    } else if (stricmp("worldspaceblend", token) == 0) {
        pcmd->cmd = CMD_WORLDSPACEBLEND;

        GetToken(false);

        s_animation_t *extanim = LookupAnimation(token);
        if (extanim == NULL) {
            TokenError("unknown worldspaceblend animation '%s\'\n", token);
        }

        pcmd->u.world.ref = extanim;
        pcmd->u.world.startframe = 0;
        pcmd->u.world.loops = false;
    } else if (stricmp("worldspaceblendloop", token) == 0) {
        pcmd->cmd = CMD_WORLDSPACEBLEND;

        GetToken(false);

        s_animation_t *extanim = LookupAnimation(token);
        if (extanim == NULL) {
            TokenError("unknown worldspaceblend animation '%s\'\n", token);
        }

        pcmd->u.world.ref = extanim;

        GetToken(false);
        pcmd->u.world.startframe = atoi(token);

        pcmd->u.world.loops = true;
    } else if (stricmp("rotateto", token) == 0) {
        pcmd->cmd = CMD_ANGLE;

        GetToken(false);
        pcmd->u.angle.angle = verify_atof(token);
    } else if (stricmp("ikrule", token) == 0) {
        pcmd->cmd = CMD_IKRULE;

        pcmd->u.ikrule.pRule = (s_ikrule_t *) calloc(1, sizeof(s_ikrule_t));

        Option_IKRule(pcmd->u.ikrule.pRule);
    } else if (stricmp("ikfixup", token) == 0) {
        pcmd->cmd = CMD_IKFIXUP;

        pcmd->u.ikfixup.pRule = (s_ikrule_t *) calloc(1, sizeof(s_ikrule_t));

        Option_IKRule(pcmd->u.ikrule.pRule);
    } else if (stricmp("walkframe", token) == 0) {
        pcmd->cmd = CMD_MOTION;

        // frame
        GetToken(false);
        pcmd->u.motion.iEndFrame = verify_atoi(token);

        // motion type to match
        pcmd->u.motion.motiontype = 0;
        while (TokenAvailable()) {
            GetToken(false);
            int ctrl = lookupControl(token);
            if (ctrl != -1) {
                pcmd->u.motion.motiontype |= ctrl;
            } else {
                UnGetToken();
                break;
            }
        }

        /*
		GetToken( false ); // X
		pcmd->u.motion.x = verify_atof( token );

		GetToken( false ); // Y
		pcmd->u.motion.y = verify_atof( token );

		GetToken( false ); // A
		pcmd->u.motion.zr = verify_atof( token );
		*/
    } else if (stricmp("walkalignto", token) == 0) {
        pcmd->cmd = CMD_REFMOTION;

        GetToken(false);
        pcmd->u.motion.iEndFrame = verify_atoi(token);

        pcmd->u.motion.iSrcFrame = pcmd->u.motion.iEndFrame;

        GetToken(false); // reference animation
        s_animation_t *extanim = LookupAnimation(token);
        if (extanim == NULL) {
            TokenError("unknown alignto animation '%s\'\n", token);
        }
        pcmd->u.motion.pRefAnim = extanim;

        pcmd->u.motion.iRefFrame = 0;

        // motion type to match
        pcmd->u.motion.motiontype = 0;
        while (TokenAvailable()) {
            GetToken(false);
            int ctrl = lookupControl(token);
            if (ctrl != -1) {
                pcmd->u.motion.motiontype |= ctrl;
            } else {
                UnGetToken();
                break;
            }
        }


        /*
		GetToken( false ); // X
		pcmd->u.motion.x = verify_atof( token );

		GetToken( false ); // Y
		pcmd->u.motion.y = verify_atof( token );

		GetToken( false ); // A
		pcmd->u.motion.zr = verify_atof( token );
		*/
    } else if (stricmp("walkalign", token) == 0) {
        pcmd->cmd = CMD_REFMOTION;

        // end frame to apply motion over
        GetToken(false);
        pcmd->u.motion.iEndFrame = verify_atoi(token);

        // reference animation
        GetToken(false);
        s_animation_t *extanim = LookupAnimation(token);
        if (extanim == NULL) {
            TokenError("unknown alignto animation '%s\'\n", token);
        }
        pcmd->u.motion.pRefAnim = extanim;

        // motion type to match
        pcmd->u.motion.motiontype = 0;
        while (TokenAvailable()) {
            GetToken(false);
            int ctrl = lookupControl(token);
            if (ctrl != -1) {
                pcmd->u.motion.motiontype |= ctrl;
            } else {
                break;
            }
        }
        if (pcmd->u.motion.motiontype == 0) {
            TokenError("missing controls on walkalign\n");
        }

        // frame of reference animation to match
        pcmd->u.motion.iRefFrame = verify_atoi(token);

        // against what frame of the current animation
        GetToken(false);
        pcmd->u.motion.iSrcFrame = verify_atoi(token);
    } else if (stricmp("derivative", token) == 0) {
        pcmd->cmd = CMD_DERIVATIVE;

        // get scale
        GetToken(false);
        pcmd->u.derivative.scale = verify_atof(token);
    } else if (stricmp("noanimation", token) == 0) {
        pcmd->cmd = CMD_NOANIMATION;
    } else if (stricmp("noanim_keepduration", token) == 0) {
        pcmd->cmd = CMD_NOANIM_KEEPDURATION;
    } else if (stricmp("lineardelta", token) == 0) {
        pcmd->cmd = CMD_LINEARDELTA;
        pcmd->u.linear.flags |= STUDIO_AL_POST;
    } else if (stricmp("splinedelta", token) == 0) {
        pcmd->cmd = CMD_LINEARDELTA;
        pcmd->u.linear.flags |= STUDIO_AL_POST;
        pcmd->u.linear.flags |= STUDIO_AL_SPLINE;
    } else if (stricmp("compress", token) == 0) {
        pcmd->cmd = CMD_COMPRESS;

        // get frames to skip
        GetToken(false);
        pcmd->u.compress.frames = verify_atoi(token);
    } else if (stricmp("numframes", token) == 0) {
        pcmd->cmd = CMD_NUMFRAMES;

        // get frames to force
        GetToken(false);
        pcmd->u.compress.frames = verify_atoi(token);
    } else if (stricmp("counterrotate", token) == 0) {
        pcmd->cmd = CMD_COUNTERROTATE;

        // get bone name
        GetToken(false);
        pcmd->u.counterrotate.pBonename = strdup(token);
    } else if (stricmp("counterrotateto", token) == 0) {
        pcmd->cmd = CMD_COUNTERROTATE;

        pcmd->u.counterrotate.bHasTarget = true;

        // get pitch
        GetToken(false);
        pcmd->u.counterrotate.targetAngle[0] = verify_atof(token);

        // get yaw
        GetToken(false);
        pcmd->u.counterrotate.targetAngle[1] = verify_atof(token);

        // get roll
        GetToken(false);
        pcmd->u.counterrotate.targetAngle[2] = verify_atof(token);

        // get bone name
        GetToken(false);
        pcmd->u.counterrotate.pBonename = strdup(token);
    } else if (stricmp("localhierarchy", token) == 0) {
        pcmd->cmd = CMD_LOCALHIERARCHY;

        // get bone name
        GetToken(false);
        pcmd->u.localhierarchy.pBonename = strdup(token);

        // get parent name
        GetToken(false);
        pcmd->u.localhierarchy.pParentname = strdup(token);

        pcmd->u.localhierarchy.start = -1;
        pcmd->u.localhierarchy.peak = -1;
        pcmd->u.localhierarchy.tail = -1;
        pcmd->u.localhierarchy.end = -1;

        if (TokenAvailable()) {
            GetToken(false);
            if (stricmp(token, "range") == 0) {
                //
                GetToken(false);
                pcmd->u.localhierarchy.start = verify_atof_with_null(token);

                //
                GetToken(false);
                pcmd->u.localhierarchy.peak = verify_atof_with_null(token);

                //
                GetToken(false);
                pcmd->u.localhierarchy.tail = verify_atof_with_null(token);

                //
                GetToken(false);
                pcmd->u.localhierarchy.end = verify_atof_with_null(token);
            } else {
                UnGetToken();
            }
        }
    } else if (stricmp("forceboneposrot", token) == 0) {
        pcmd->cmd = CMD_FORCEBONEPOSROT;

        // get bone name
        GetToken(false);
        pcmd->u.forceboneposrot.pBonename = strdup(token);

        pcmd->u.forceboneposrot.bDoPos = false;
        pcmd->u.forceboneposrot.bDoRot = false;

        pcmd->u.forceboneposrot.pos[0] = 0;
        pcmd->u.forceboneposrot.pos[1] = 0;
        pcmd->u.forceboneposrot.pos[2] = 0;

        pcmd->u.forceboneposrot.rot[0] = 0;
        pcmd->u.forceboneposrot.rot[1] = 0;
        pcmd->u.forceboneposrot.rot[2] = 0;

        if (TokenAvailable()) {
            GetToken(false);
            if (stricmp(token, "pos") == 0) {
                pcmd->u.forceboneposrot.bDoPos = true;

                GetToken(false);
                pcmd->u.forceboneposrot.pos[0] = verify_atof_with_null(token);

                GetToken(false);
                pcmd->u.forceboneposrot.pos[1] = verify_atof_with_null(token);

                GetToken(false);
                pcmd->u.forceboneposrot.pos[2] = verify_atof_with_null(token);
            } else {
                UnGetToken();
            }

            if (TokenAvailable()) {
                GetToken(false);
                if (stricmp(token, "rot") == 0) {
                    pcmd->u.forceboneposrot.bDoRot = true;

                    GetToken(false);
                    pcmd->u.forceboneposrot.rot[0] = verify_atof_with_null(token);

                    GetToken(false);
                    pcmd->u.forceboneposrot.rot[1] = verify_atof_with_null(token);

                    GetToken(false);
                    pcmd->u.forceboneposrot.rot[2] = verify_atof_with_null(token);

                    pcmd->u.forceboneposrot.bRotIsLocal = false;
                    if (TokenAvailable()) {
                        GetToken(false);
                        if (stricmp(token, "local") == 0) {
                            pcmd->u.forceboneposrot.bRotIsLocal = true;
                        } else {
                            UnGetToken();
                        }
                    }
                } else {
                    UnGetToken();
                }
            }
        }

    } else if (stricmp("bonedriver", token) == 0) {
        pcmd->cmd = CMD_BONEDRIVER;

        pcmd->u.bonedriver.iAxis = 0;
        pcmd->u.bonedriver.value = 1.0f;
        pcmd->u.bonedriver.all = true;

        // get bone name
        GetToken(false);
        pcmd->u.bonedriver.pBonename = strdup(token);

        if (TokenAvailable()) {
            GetToken(false);
            if (stricmp(token, "axis") == 0) {
                GetToken(false);
                if (stricmp(token, "x") == 0) {
                    pcmd->u.bonedriver.iAxis = 0;
                } else if (stricmp(token, "y") == 0) {
                    pcmd->u.bonedriver.iAxis = 1;
                } else if (stricmp(token, "z") == 0) {
                    pcmd->u.bonedriver.iAxis = 2;
                } else {
                    TokenError("Unknown bonedriver axis.\n");
                }
            } else {
                UnGetToken();
            }
        }

        if (TokenAvailable()) {
            GetToken(false);
            if (stricmp(token, "value") == 0) {
                GetToken(false);
                pcmd->u.bonedriver.value = verify_atof_with_null(token);
            } else {
                UnGetToken();
            }
        }

        if (TokenAvailable()) {
            GetToken(false);
            if (stricmp(token, "range") == 0) {
                pcmd->u.bonedriver.all = false;

                GetToken(false);
                pcmd->u.bonedriver.start = verify_atoi(token);

                GetToken(false);
                pcmd->u.bonedriver.peak = verify_atoi(token);

                GetToken(false);
                pcmd->u.bonedriver.tail = verify_atoi(token);

                GetToken(false);
                pcmd->u.bonedriver.end = verify_atoi(token);
            } else {
                UnGetToken();
            }
        }
    } else if (stricmp("reverse", token) == 0) {
        pcmd->cmd = CMD_REVERSE;
    } else if (stricmp("appendanim", token) == 0) {
        pcmd->cmd = CMD_APPENDANIM;

        GetToken(false); // reference animation
        s_animation_t *extanim = LookupAnimation(token);
        if (extanim == NULL) {
            TokenError("unknown appendanim '%s\'\n", token);
        }
        pcmd->u.appendanim.ref = extanim;

    } else {
        return false;
    }
    numcmds++;
    return true;
}

void Cmd_Cmdlist() {
    int depth = 0;

    // name
    GetToken(false);
    strcpyn(g_cmdlist[g_numcmdlists].name, token);

    while (1) {
        if (depth > 0) {
            if (!GetToken(true)) {
                break;
            }
        } else {
            if (!TokenAvailable()) {
                break;
            } else {
                GetToken(false);
            }
        }

        if (endofscript) {
            if (depth != 0) {
                TokenError("missing }\n");
            }
            return;
        }
        if (stricmp("{", token) == 0) {
            depth++;
        } else if (stricmp("}", token) == 0) {
            depth--;
        } else if (ParseCmdlistToken(g_cmdlist[g_numcmdlists].numcmds, g_cmdlist[g_numcmdlists].cmds)) {

        } else {
            TokenError("unknown command: %s\n", token);
        }

        if (depth < 0) {
            TokenError("missing {\n");
        }
    };

    g_numcmdlists++;
}

//-----------------------------------------------------------------------------
// Purpose: parse order independant s_animation_t token for $animations
//-----------------------------------------------------------------------------
bool ParseAnimationToken(s_animation_t *panim) {
    if (!Q_stricmp("nocull", token)) {
        panim->nocull = true;
        return true;
    }

    if (!Q_stricmp("ignorescale", token)) {
        panim->ignorescale = true;
        return true;
    }

    if (!Q_stricmp("ignorebonemove", token)) {
        panim->ignoreBoneMove = true;
        return true;
    }

    if (!Q_stricmp("ignorebonerotate", token)) {
        panim->ignoreBoneRotate = true;
        return true;
    }

    if (!Q_stricmp("if", token)) {
        // fixme: add expression evaluation
        GetToken(false);
        if (atoi(token) == 0 && stricmp(token, "true") != 0) {
            GetToken(true);
            if (token[0] == '{') {
                int depth = 1;
                while (TokenAvailable() && depth > 0) {
                    GetToken(true);
                    if (stricmp("{", token) == 0) {
                        depth++;
                    } else if (stricmp("}", token) == 0) {
                        depth--;
                    }
                }
            }
        }
        return true;
    }

    if (!Q_stricmp("fps", token)) {
        GetToken(false);
        panim->fps = verify_atof(token);
        if (panim->fps <= 0.0f) {
            TokenError("ParseAnimationToken:  fps (%f from '%s') <= 0.0\n", panim->fps, token);
        }
        return true;
    }

    if (!Q_stricmp("origin", token)) {
        GetToken(false);
        panim->adjust.x = verify_atof(token);

        GetToken(false);
        panim->adjust.y = verify_atof(token);

        GetToken(false);
        panim->adjust.z = verify_atof(token);
        return true;
    }

    if (!Q_stricmp("rotate", token)) {
        GetToken(false);
        // FIXME: broken for Maya
        panim->rotation.z = DEG2RAD(verify_atof(token) + 90);
        return true;
    }

    if (!Q_stricmp("angles", token)) {
        GetToken(false);
        panim->rotation.x = DEG2RAD(verify_atof(token));
        GetToken(false);
        panim->rotation.y = DEG2RAD(verify_atof(token));
        GetToken(false);
        panim->rotation.z = DEG2RAD(verify_atof(token) + 90.0f);
        return true;
    }

    if (!Q_stricmp("scale", token)) {
        GetToken(false);
        panim->scale = verify_atof(token);
        return true;
    }

    if (!Q_strnicmp("loop", token, 4)) {
        panim->flags |= STUDIO_LOOPING;
        return true;
    }

    if (!Q_stricmp("noforceloop", token)) {
        panim->flags |= STUDIO_NOFORCELOOP;
        return true;
    }

    if (!Q_strcmp("startloop", token)) {
        GetToken(false);
        panim->looprestart = verify_atoi(token);
        if (panim->looprestartpercent != 0) {
            MdlError("Can't specify startloop for animation %s, percentstartloop already specified.", panim->name);
        }
        panim->flags |= STUDIO_LOOPING;
        return true;
    }

    if (!Q_strcmp("percentstartloop", token)) {
        GetToken(false);
        panim->looprestartpercent = verify_atof(token);
        if (panim->looprestart != 0) {
            MdlError("Can't specify percentstartloop for animation %s, looprestart already specified.", panim->name);
        }
        panim->flags |= STUDIO_LOOPING;
        return true;
    }

    if (!Q_stricmp("fudgeloop", token)) {
        panim->fudgeloop = true;
        panim->flags |= STUDIO_LOOPING;
        return true;
    }

    if (!Q_strnicmp("snap", token, 4)) {
        panim->flags |= STUDIO_SNAP;
        return true;
    }

    if (!Q_strnicmp("frame", token, 5) || !Q_strnicmp("framestart", token, 10)) {

        // framestart assumes the animation's end frame is ok to use no matter what it is. This is better than finding 'frame 9 10000' in qc script
        bool bUseDefaultEndFrame = (!Q_strnicmp("framestart", token, 10));

        GetToken(false);
        panim->startframe = verify_atoi(token);

        if (!bUseDefaultEndFrame) {
            GetToken(false);
            panim->endframe = verify_atoi(token);
        }

        // NOTE: This always affects the first source anim read in
        s_sourceanim_t *pSourceAnim = FindSourceAnim(panim->source, panim->animationname);
        if (pSourceAnim) {
            if (panim->startframe < pSourceAnim->startframe) {
                panim->startframe = pSourceAnim->startframe;
            }

            if (panim->endframe > pSourceAnim->endframe || bUseDefaultEndFrame) {
                panim->endframe = pSourceAnim->endframe;
            }
        }

        if (!g_StudioMdlContext.createMakefile && panim->endframe < panim->startframe) {
            TokenError("end frame before start frame in %s", panim->name);
        }

        panim->numframes = panim->endframe - panim->startframe + 1;

        return true;
    }

    if (!Q_stricmp("blockname", token)) {
        GetToken(false);
        s_sourceanim_t *pSourceAnim = FindSourceAnim(panim->source, token);

        // NOTE: This always affects the first source anim read in
        if (pSourceAnim) {
            panim->startframe = pSourceAnim->startframe;
            panim->endframe = pSourceAnim->endframe;

            if (!g_StudioMdlContext.createMakefile && panim->endframe < panim->startframe) {
                TokenError("end frame before start frame in %s", panim->name);
            }

            panim->numframes = panim->endframe - panim->startframe + 1;
            Q_strncpy(panim->animationname, token, sizeof(panim->animationname));
        } else {
            MdlError("Requested unknown animation block name %s\n", token);
        }
        return true;
    }

    if (!Q_stricmp("post", token)) {
        panim->flags |= STUDIO_POST;
        return true;
    }

    if (!Q_stricmp("noautoik", token)) {
        panim->noAutoIK = true;
        return true;
    }

    if (!Q_stricmp("autoik", token)) {
        panim->noAutoIK = false;
        return true;
    }

    if (ParseCmdlistToken(panim->numcmds, panim->cmds))
        return true;

    if (!Q_stricmp("cmdlist", token)) {
        GetToken(false); // A

        int i;
        for (i = 0; i < g_numcmdlists; i++) {
            if (stricmp(g_cmdlist[i].name, token) == 0) {
                break;
            }
        }
        if (i == g_numcmdlists)
            TokenError("unknown cmdlist %s\n", token);

        for (int j = 0; j < g_cmdlist[i].numcmds; j++) {
            if (panim->numcmds >= MAXSTUDIOCMDS) {
                TokenError("Too many cmds in %s\n", panim->name);
            }
            panim->cmds[panim->numcmds++] = g_cmdlist[i].cmds[j];
        }
        return true;
    }

    if (!Q_stricmp("motionrollback", token)) {
        GetToken(false);
        panim->motionrollback = atof(token);
        return true;
    }

    if (!Q_stricmp("noanimblock", token)) {
        panim->disableAnimblocks = true;
        return true;
    }

    if (!Q_stricmp("noanimblockstall", token)) {
        panim->isFirstSectionLocal = true;
        return true;
    }

    if (!Q_stricmp("nostallframes", token)) {
        GetToken(false);
        panim->numNostallFrames = atof(token);
        return true;
    }

    if (lookupControl(token) != -1) {
        panim->motiontype |= lookupControl(token);
        return true;
    }

    return false;
}


//-----------------------------------------------------------------------------
// Purpose: wrapper for parsing $animation tokens
//-----------------------------------------------------------------------------

int ParseAnimation(s_animation_t *panim, bool isAppend) {
    int depth = 0;

    while (1) {
        if (depth > 0) {
            if (!GetToken(true)) {
                break;
            }
        } else {
            if (!TokenAvailable()) {
                break;
            } else {
                GetToken(false);
            }
        }

        if (endofscript) {
            if (depth != 0) {
                TokenError("missing }\n");
            }
            return 1;
        }
        if (stricmp("{", token) == 0) {
            depth++;
        } else if (stricmp("}", token) == 0) {
            depth--;
        } else if (ParseAnimationToken(panim)) {

        } else {
            TokenError("Unknown animation option\'%s\'\n", token);
        }

        if (depth < 0) {
            TokenError("missing {\n");
        }
    };

    return 0;
}


//-----------------------------------------------------------------------------
// Purpose: mark panim and all animations it transitively references as used
//-----------------------------------------------------------------------------
static void MarkAnimReferenced( s_animation_t *panim, bool *referenced )
{
    if ( !panim || referenced[panim->index] )
        return;
    referenced[panim->index] = true;

    for ( int i = 0; i < panim->numcmds; i++ )
    {
        const s_animcmd_t &cmd = panim->cmds[i];
        s_animation_t *ref = nullptr;
        switch ( cmd.cmd )
        {
        case CMD_SUBTRACT:       ref = cmd.u.subtract.ref;   break;
        case CMD_AO:             ref = cmd.u.ao.ref;          break;
        case CMD_MATCH:          ref = cmd.u.match.ref;       break;
        case CMD_MATCHBLEND:     ref = cmd.u.match.ref;       break;
        case CMD_WORLDSPACEBLEND:ref = cmd.u.world.ref;       break;
        case CMD_REFMOTION:      ref = cmd.u.motion.pRefAnim; break;
        case CMD_APPENDANIM:     ref = cmd.u.appendanim.ref;  break;
        default: break;
        }
        MarkAnimReferenced( ref, referenced );
    }
}

//-----------------------------------------------------------------------------
// Purpose: remove $animations not referenced by any $sequence (unless nocull)
//-----------------------------------------------------------------------------
void CullUnreferencedAnimations()
{
    if ( !g_StudioMdlContext.cullAnims || g_numani == 0 )
        return;

    bool *referenced = (bool *) calloc( g_numani, sizeof(bool) );

    for ( int i = 0; i < g_sequence.Count(); i++ )
    {
        s_sequence_t &seq = g_sequence[i];
        for ( int j = 0; j < seq.groupsize[0]; j++ )
            for ( int k = 0; k < seq.groupsize[1]; k++ )
                if ( seq.panim.Count() > j && seq.panim[j].Count() > k )
                    MarkAnimReferenced( seq.panim[j][k], referenced );
        MarkAnimReferenced( seq.paramanim,    referenced );
        MarkAnimReferenced( seq.paramcompanim, referenced );
        MarkAnimReferenced( seq.paramcenter,  referenced );
    }

    int numCulled = 0;
    int newCount  = 0;
    for ( int i = 0; i < g_numani; i++ )
    {
        s_animation_t *panim = g_panimation[i];
        if ( !referenced[i] && !panim->nocull )
        {
            if ( !g_StudioMdlContext.quiet )
                printf( "Culling unreferenced animation \"%s\"\n", panim->name );
            free( panim );
            numCulled++;
        }
        else
        {
            panim->index = newCount;
            g_panimation[newCount++] = panim;
        }
    }
    g_numani = newCount;

    free( referenced );

    if ( !g_StudioMdlContext.quiet && numCulled > 0 )
        printf( "Culled %d unreferenced animation(s).\n", numCulled );
}

//-----------------------------------------------------------------------------
// Purpose: remove flex morphs (flexkeys) whose target flexdesc is not driven by
//          any flexrule, read by any flexrule, or referenced by an eyeball or
//          mouth. Such morphs carry vertex-animation data that is never reachable
//          at runtime and only inflate the .vvd/.mdl. Must run before
//          SimplifyModel() so the dead vanim data is never built.
//-----------------------------------------------------------------------------
void CullUnreferencedFlexes()
{
    if ( !g_StudioMdlContext.cullMorphs || g_numflexkeys == 0 )
        return;

    bool usedDesc[MAXSTUDIOFLEXDESC] = {false};

    auto MarkDesc = [&]( int desc ) {
        if ( desc >= 0 && desc < MAXSTUDIOFLEXDESC )
            usedDesc[desc] = true;
    };

    // flexrules: their output flexdesc and any flexdesc they FETCH2 (read) are live
    for ( int r = 0; r < g_numflexrules; r++ )
    {
        const s_flexrule_t &rule = g_flexrule[r];
        MarkDesc( rule.flex );
        for ( int o = 0; o < rule.numops; o++ )
            if ( rule.op[o].op == STUDIO_FETCH2 )
                MarkDesc( rule.op[o].d.index );
    }

    // eyeballs reference flexdescs directly (lid + raiser/neutral/lowerer)
    for ( int i = 0; i < g_nummodels; i++ )
    {
        s_model_t *pmodel = g_model[i];
        if ( !pmodel )
            continue;
        for ( int j = 0; j < pmodel->numeyeballs; j++ )
        {
            const s_eyeball_t &eye = pmodel->eyeball[j];
            MarkDesc( eye.upperlidflexdesc );
            MarkDesc( eye.lowerlidflexdesc );
            for ( int k = 0; k < 3; k++ )
            {
                MarkDesc( eye.upperflexdesc[k] );
                MarkDesc( eye.lowerflexdesc[k] );
            }
        }
    }

    // mouths reference a flexdesc for movement
    for ( int i = 0; i < g_nummouths; i++ )
        MarkDesc( g_mouth[i].flexdesc );

    int newCount = 0;
    int numCulled = 0;
    for ( int i = 0; i < g_numflexkeys; i++ )
    {
        s_flexkey_t &fk = g_flexkey[i];

        bool bKeep = ( &fk == g_defaultflexkey )
                     || ( fk.flexdesc >= 0 && fk.flexdesc < MAXSTUDIOFLEXDESC && usedDesc[fk.flexdesc] )
                     || ( fk.flexpair >  0 && fk.flexpair < MAXSTUDIOFLEXDESC && usedDesc[fk.flexpair] );

        if ( !bKeep )
        {
            if ( !g_StudioMdlContext.quiet )
            {
                const char *facs = ( fk.flexdesc >= 0 && fk.flexdesc < g_numflexdesc )
                                       ? g_flexdesc[fk.flexdesc].FACS : "?";
                printf( "Culling unused flex morph \"%s\"\n", facs );
            }
            numCulled++;
            continue;
        }

        if ( newCount != i )
        {
            // keep g_defaultflexkey pointing at the morph after compaction
            if ( g_defaultflexkey == &g_flexkey[i] )
                g_defaultflexkey = &g_flexkey[newCount];
            g_flexkey[newCount] = g_flexkey[i];
        }
        newCount++;
    }
    g_numflexkeys = newCount;

    if ( !g_StudioMdlContext.quiet && numCulled > 0 )
        printf( "Culled %d unused flex morph(s).\n", numCulled );
}

//-----------------------------------------------------------------------------
// Purpose: allocate an entry for $animation
//-----------------------------------------------------------------------------
void Cmd_Animation() {
    // name
    GetToken(false);

    s_animation_t *panim = LookupAnimation(token);

    if (panim != NULL) {
        if (!panim->isOverride) {
            TokenError("Duplicate animation name \"%s\"\n", token);
        } else {
            panim->doesOverride = true;
            ParseEmpty();
            return;
        }
    }

    // allocate animation entry
    g_panimation[g_numani] = (s_animation_t *) calloc(1, sizeof(s_animation_t));
    g_panimation[g_numani]->index = g_numani;
    panim = g_panimation[g_numani];
    strcpyn(panim->name, token);
    g_numani++;

    // filename
    GetToken(false);
    strcpyn(panim->filename, token);

    panim->source = Load_Source(panim->filename, "");
    if (panim->source->m_Animations.Count()) {
        s_sourceanim_t *pSourceAnim = &panim->source->m_Animations[0];
        panim->startframe = pSourceAnim->startframe;
        panim->endframe = pSourceAnim->endframe;
        Q_strncpy(panim->animationname, pSourceAnim->animationname, sizeof(panim->animationname));
    } else {
        panim->startframe = 0;
        panim->endframe = 0;
        Q_strncpy(panim->animationname, "", sizeof(panim->animationname));
    }
    VectorCopy(g_defaultadjust, panim->adjust);
    panim->rotation = g_defaultrotation;
    panim->scale = 1.0f;
    panim->fps = 30.0;
    panim->motionrollback = g_StudioMdlContext.defaultMotionRollback;

    ParseAnimation(panim, false);

    panim->numframes = panim->endframe - panim->startframe + 1;

    //CheckAutoShareAnimationGroup( panim->name );
}

//-----------------------------------------------------------------------------
// Purpose: allocate an entry for $sequence
//-----------------------------------------------------------------------------
s_sequence_t *ProcessCmdSequence(const char *pSequenceName) {
    s_animation_t *panim = LookupAnimation(pSequenceName);

    // allocate sequence
    if (panim != NULL) {
        if (!panim->isOverride) {
            TokenError("Duplicate sequence name \"%s\"\n", pSequenceName);
        } else {
            panim->doesOverride = true;
            return nullptr;
        }
    }

    if (g_sequence.Count() >= MAXSTUDIOSEQUENCES) {
        TokenError("Too many sequences (%d max)\n", MAXSTUDIOSEQUENCES);
    }

    s_sequence_t *pseq = &g_sequence[g_sequence.AddToTail()];
    memset(pseq, 0, sizeof(s_sequence_t));

    // initialize sequence
    Q_strncpy(pseq->name, pSequenceName, sizeof(pseq->name));

    pseq->actweight = 0;
    pseq->activityname[0] = '\0';
    pseq->activity = -1; // -1 is the default for 'no activity'

    pseq->paramindex[0] = -1;
    pseq->paramindex[1] = -1;

    pseq->groupsize[0] = 0;
    pseq->groupsize[1] = 0;

    pseq->fadeintime = g_StudioMdlContext.defaultFadeInTime;
    pseq->fadeouttime = g_StudioMdlContext.defaultFadeOutTime;
    return pseq;
}


int Option_Activity(s_sequence_t *psequence) {
    qboolean found;

    found = false;

    GetToken(false);
    strcpy(psequence->activityname, token);

    if (g_StudioMdlContext.AllowedActivityNames.size()) {
        if (std::find(g_StudioMdlContext.AllowedActivityNames.begin(), g_StudioMdlContext.AllowedActivityNames.end(), token) ==
            g_StudioMdlContext.AllowedActivityNames.end()) {
            MdlError("Unknown sequence activity \"%s\" in \"%s\".", token, psequence->name);
        }
    }

    GetToken(false);
    psequence->actweight = verify_atoi(token);

    if (psequence->actweight == 0) {
        TokenError("Activity %s has a zero weight (weights must be integers > 0)\n", psequence->activityname);
    }

    return 0;
}


int Option_ActivityModifier(s_sequence_t *psequence) {
    GetToken(false);

    if (token[0] == '{') {
        while (TokenAvailable()) {
            GetToken(true);
            if (stricmp("}", token) == 0)
                break;

            strlwr(token);
            strcpyn(psequence->activitymodifier[psequence->numactivitymodifiers++].name, token);
        }
    } else {
        strlwr(token);
        strcpyn(psequence->activitymodifier[psequence->numactivitymodifiers++].name, token);
    }

    return 0;
}

//-----------------------------------------------------------------------------
// Purpose: create a virtual $animation command from a $sequence reference
//-----------------------------------------------------------------------------
s_animation_t *ProcessImpliedAnimation(s_sequence_t *psequence, const char *filename) {
    // allocate animation entry
    g_panimation[g_numani] = (s_animation_t *) calloc(1, sizeof(s_animation_t));
    g_panimation[g_numani]->index = g_numani;
    s_animation_t *panim = g_panimation[g_numani];
    g_numani++;

    panim->isImplied = true;

    panim->startframe = 0;
    panim->endframe = MAXSTUDIOANIMFRAMES - 1;

    strcpy(panim->name, "@");
    strcat(panim->name, psequence->name);
    strcpyn(panim->filename, filename);

    VectorCopy(g_defaultadjust, panim->adjust);
    panim->scale = 1.0f;
    panim->rotation = g_defaultrotation;
    panim->fps = 30;
    panim->motionrollback = g_StudioMdlContext.defaultMotionRollback;

    //panim->source = Load_Source( panim->filename, "smd" );
    panim->source = Load_Source(panim->filename, "");
    if (panim->source->m_Animations.Count()) {
        s_sourceanim_t *pSourceAnim = &panim->source->m_Animations[0];
        Q_strncpy(panim->animationname, panim->source->m_Animations[0].animationname, sizeof(panim->animationname));
        if (panim->startframe < pSourceAnim->startframe) {
            panim->startframe = pSourceAnim->startframe;
        }

        if (panim->endframe > pSourceAnim->endframe) {
            panim->endframe = pSourceAnim->endframe;
        }
    } else {
        Q_strncpy(panim->animationname, "", sizeof(panim->animationname));
    }

    if (!g_StudioMdlContext.createMakefile && panim->endframe < panim->startframe) {
        TokenError("end frame before start frame in %s", panim->name);
    }

    panim->numframes = panim->endframe - panim->startframe + 1;

    //CheckAutoShareAnimationGroup( panim->name );

    return panim;
}

//-----------------------------------------------------------------------------
// Purpose: copy globally reavent $animation options from one $animation to another
//-----------------------------------------------------------------------------

void CopyAnimationSettings(s_animation_t *pdest, s_animation_t *psrc) {
    pdest->fps = psrc->fps;

    VectorCopy(psrc->adjust, pdest->adjust);
    pdest->scale = psrc->scale;
    pdest->rotation = psrc->rotation;

    pdest->motiontype = psrc->motiontype;

    //Adrian - Hey! Revisit me later.
    /*if (pdest->startframe < psrc->startframe)
		pdest->startframe = psrc->startframe;

	if (pdest->endframe > psrc->endframe)
		pdest->endframe = psrc->endframe;

	if (pdest->endframe < pdest->startframe)
		TokenError( "fixedup end frame before start frame in %s", pdest->name );

	pdest->numframes = pdest->endframe - pdest->startframe + 1;*/

    for (int i = 0; i < psrc->numcmds; i++) {
        if (pdest->numcmds >= MAXSTUDIOCMDS) {
            TokenError("Too many cmds in %s\n", pdest->name);
        }
        pdest->cmds[pdest->numcmds++] = psrc->cmds[i];
    }
}


//-----------------------------------------------------------------------------
// Performs processing on a sequence
//-----------------------------------------------------------------------------
void ProcessSequence(s_sequence_t *pseq, int numblends, s_animation_t **animations, bool isAppend) {
    if (isAppend)
        return;

    if (numblends == 0) {
        TokenError("no animations found\n");
    }

    if (pseq->groupsize[0] == 0) {
        if (numblends < 4) {
            pseq->groupsize[0] = numblends;
            pseq->groupsize[1] = 1;
        } else {
            int i = sqrt((float) numblends);
            if (i * i == numblends) {
                pseq->groupsize[0] = i;
                pseq->groupsize[1] = i;
            } else {
                TokenError("non-square (%d) number of blends without \"blendwidth\" set\n", numblends);
            }
        }
    } else {
        pseq->groupsize[1] = numblends / pseq->groupsize[0];

        if (pseq->groupsize[0] * pseq->groupsize[1] != numblends) {
            TokenError("missing animation blends. Expected %d, found %d\n",
                       pseq->groupsize[0] * pseq->groupsize[1], numblends);
        }
    }

    for (int i = 0; i < numblends; i++) {
        int j = i % pseq->groupsize[0];
        int k = i / pseq->groupsize[0];

        pseq->panim[j][k] = animations[i];

        if (i > 0 && animations[i]->isImplied) {
            CopyAnimationSettings(animations[i], animations[0]);
        }
        animations[i]->isImplied = false; // don't copy any more commands
        pseq->flags |= animations[i]->flags;
    }

    pseq->numblends = numblends;
}


//-----------------------------------------------------------------------------
// Purpose: parse options unique to $sequence
//-----------------------------------------------------------------------------
int ParseSequence(s_sequence_t *pseq, bool isAppend) {

    g_StudioMdlContext.szInCurrentSeqName = pseq->name;

    int depth = 0;
    s_animation_t *animations[64];
    int i, j, n;
    int numblends = 0;
    bool seqIgnoreScale = false;

    if (isAppend) {
        animations[0] = pseq->panim[0][0];
    }

    while (1) {
        if (depth > 0) {
            if (!GetToken(true)) {
                break;
            }
        } else {
            if (!TokenAvailable()) {
                break;
            } else {
                GetToken(false);
            }
        }

        if (endofscript) {
            if (depth != 0) {
                TokenError("missing }\n");
            }
            return 1;
        }
        if (stricmp("{", token) == 0) {
            depth++;
        } else if (stricmp("}", token) == 0) {
            depth--;
        }
            /*
		else if (stricmp("deform", token ) == 0)
		{
			Option_Deform( pseq );
		}
		*/
        else if (stricmp("animtag", token) == 0) {
            depth -= Option_AnimTag(pseq);
        } else if (stricmp("event", token) == 0) {
            depth -= Option_Event(pseq);
        } else if (stricmp("activity", token) == 0) {
            Option_Activity(pseq);
        } else if ((stricmp("activitymodifier", token) == 0) || (stricmp("actmod", token) == 0)) {
            Option_ActivityModifier(pseq);
        } else if (strnicmp(token, "ACT_", 4) == 0) {
            UnGetToken();
            Option_Activity(pseq);
        } else if (stricmp("snap", token) == 0) {
            pseq->flags |= STUDIO_SNAP;
        } else if (stricmp("ignorescale", token) == 0) {
            seqIgnoreScale = true;
        } else if (stricmp("blendwidth", token) == 0) {
            GetToken(false);
            pseq->groupsize[0] = verify_atoi(token);
        } else if (stricmp("blend", token) == 0) {
            i = 0;
            if (pseq->paramindex[0] != -1) {
                i = 1;
            }

            GetToken(false);
            j = LookupPoseParameter(token);
            pseq->paramindex[i] = j;
            pseq->paramattachment[i] = -1;
            GetToken(false);
            pseq->paramstart[i] = verify_atof(token);
            GetToken(false);
            pseq->paramend[i] = verify_atof(token);

            g_pose[j].min = min(g_pose[j].min, pseq->paramstart[i]);
            g_pose[j].min = min(g_pose[j].min, pseq->paramend[i]);
            g_pose[j].max = max(g_pose[j].max, pseq->paramstart[i]);
            g_pose[j].max = max(g_pose[j].max, pseq->paramend[i]);
        } else if (stricmp("calcblend", token) == 0) {
            i = 0;
            if (pseq->paramindex[0] != -1) {
                i = 1;
            }

            GetToken(false);
            j = LookupPoseParameter(token);
            pseq->paramindex[i] = j;

            GetToken(false);
            pseq->paramattachment[i] = LookupAttachment(token);
            if (pseq->paramattachment[i] == -1) {
                TokenError("Unknown calcblend attachment \"%s\"\n", token);
            }

            GetToken(false);
            pseq->paramcontrol[i] = lookupControl(token);
        } else if (stricmp("blendref", token) == 0) {
            GetToken(false);
            pseq->paramanim = LookupAnimation(token);
            if (pseq->paramanim == NULL) {
                TokenError("Unknown blendref animation \"%s\"\n", token);
            }
        } else if (stricmp("blendcomp", token) == 0) {
            GetToken(false);
            pseq->paramcompanim = LookupAnimation(token);
            if (pseq->paramcompanim == NULL) {
                TokenError("Unknown blendcomp animation \"%s\"\n", token);
            }
        } else if (stricmp("blendcenter", token) == 0) {
            GetToken(false);
            pseq->paramcenter = LookupAnimation(token);
            if (pseq->paramcenter == NULL) {
                TokenError("Unknown blendcenter animation \"%s\"\n", token);
            }
        } else if (stricmp("node", token) == 0) {
            GetToken(false);
            pseq->entrynode = pseq->exitnode = LookupXNode(token);
        } else if (stricmp("transition", token) == 0) {
            GetToken(false);
            pseq->entrynode = LookupXNode(token);
            GetToken(false);
            pseq->exitnode = LookupXNode(token);
        } else if (stricmp("rtransition", token) == 0) {
            GetToken(false);
            pseq->entrynode = LookupXNode(token);
            GetToken(false);
            pseq->exitnode = LookupXNode(token);
            pseq->nodeflags |= 1;
        } else if (stricmp("exitphase", token) == 0) {
            GetToken(false);
            pseq->exitphase = verify_atof(token);
        } else if (stricmp("delta", token) == 0) {
            pseq->flags |= STUDIO_DELTA;
            pseq->flags |= STUDIO_POST;
        } else if (stricmp("worldspace", token) == 0) {
            pseq->flags |= STUDIO_WORLD;
            pseq->flags |= STUDIO_POST;
        } else if (stricmp("worldrelative", token) == 0) {
            pseq->flags |= STUDIO_WORLD_AND_RELATIVE;
            pseq->flags |= STUDIO_POST;
        } else if (stricmp("rootdriver", token) == 0) {
            pseq->flags |= STUDIO_ROOTXFORM;

            // get bone name
            GetToken(false);

            strcpyn(pseq->rootDriverBoneName, token);
        } else if (stricmp("post", token) == 0) // remove
        {
            pseq->flags |= STUDIO_POST;
        } else if (stricmp("predelta", token) == 0) {
            pseq->flags |= STUDIO_DELTA;
        } else if (stricmp("autoplay", token) == 0) {
            pseq->flags |= STUDIO_AUTOPLAY;
        } else if (stricmp("fadein", token) == 0) {
            GetToken(false);
            pseq->fadeintime = verify_atof(token);
        } else if (stricmp("fadeout", token) == 0) {
            GetToken(false);
            pseq->fadeouttime = verify_atof(token);
        } else if (stricmp("realtime", token) == 0) {
            pseq->flags |= STUDIO_REALTIME;
        } else if (stricmp("posecycle", token) == 0) {
            pseq->flags |= STUDIO_CYCLEPOSE;

            GetToken(false);
            pseq->cycleposeindex = LookupPoseParameter(token);
        } else if (stricmp("hidden", token) == 0) {
            pseq->flags |= STUDIO_HIDDEN;
        } else if (stricmp("addlayer", token) == 0) {
            GetToken(false);
            strcpyn(pseq->autolayer[pseq->numautolayers].name, token);

            while (TokenAvailable()) {
                GetToken(false);
                if (stricmp("local", token) == 0) {
                    pseq->autolayer[pseq->numautolayers].flags |= STUDIO_AL_LOCAL;
                    pseq->flags |= STUDIO_LOCAL;
                } else {
                    UnGetToken();
                    break;
                }
            }

            pseq->numautolayers++;
        } else if (stricmp("iklock", token) == 0) {
            GetToken(false);
            strcpyn(pseq->iklock[pseq->numiklocks].name, token);

            GetToken(false);
            pseq->iklock[pseq->numiklocks].flPosWeight = verify_atof(token);

            GetToken(false);
            pseq->iklock[pseq->numiklocks].flLocalQWeight = verify_atof(token);

            pseq->numiklocks++;
        } else if (stricmp("keyvalues", token) == 0) {
            Option_KeyValues(&pseq->KeyValue);
        } else if (stricmp("blendlayer", token) == 0) {
            pseq->autolayer[pseq->numautolayers].flags = 0;

            GetToken(false);
            strcpyn(pseq->autolayer[pseq->numautolayers].name, token);

            GetToken(false);
            pseq->autolayer[pseq->numautolayers].start = verify_atof(token);

            GetToken(false);
            pseq->autolayer[pseq->numautolayers].peak = verify_atof(token);

            GetToken(false);
            pseq->autolayer[pseq->numautolayers].tail = verify_atof(token);

            GetToken(false);
            pseq->autolayer[pseq->numautolayers].end = verify_atof(token);

            while (TokenAvailable()) {
                GetToken(false);
                if (stricmp("xfade", token) == 0) {
                    pseq->autolayer[pseq->numautolayers].flags |= STUDIO_AL_XFADE;
                } else if (stricmp("spline", token) == 0) {
                    pseq->autolayer[pseq->numautolayers].flags |= STUDIO_AL_SPLINE;
                } else if (stricmp("noblend", token) == 0) {
                    pseq->autolayer[pseq->numautolayers].flags |= STUDIO_AL_NOBLEND;
                } else if (stricmp("poseparameter", token) == 0) {
                    pseq->autolayer[pseq->numautolayers].flags |= STUDIO_AL_POSE;
                    GetToken(false);
                    pseq->autolayer[pseq->numautolayers].pose = LookupPoseParameter(token);
                } else if (stricmp("local", token) == 0) {
                    pseq->autolayer[pseq->numautolayers].flags |= STUDIO_AL_LOCAL;
                    pseq->flags |= STUDIO_LOCAL;
                } else {
                    UnGetToken();
                    break;
                }
            }

            pseq->numautolayers++;
        } else if ((numblends || isAppend) && ParseAnimationToken(animations[0])) {

        } else if (!isAppend) {
            // assume it's an animation reference
            // first look up an existing animation
            for (n = 0; n < g_numani; n++) {
                if (stricmp(token, g_panimation[n]->name) == 0) {
                    animations[numblends++] = g_panimation[n];
                    break;
                }
            }

            if (n >= g_numani) {
                // assume it's an implied animation
                animations[numblends++] = ProcessImpliedAnimation(pseq, token);
            }
            // hack to allow animation commands to refer to same sequence
            if (numblends == 1) {
                pseq->panim[0][0] = animations[0];
            }

        } else {
            TokenError("unknown command \"%s\"\n", token);
        }

        if (depth < 0) {
            TokenError("missing {\n");
        }
    }

    ProcessSequence(pseq, numblends, animations, isAppend);

    if (seqIgnoreScale) {
        for (int idx = 0; idx < numblends; idx++) {
            if (animations[idx]) animations[idx]->ignorescale = true;
        }
        if (pseq->paramanim)     pseq->paramanim->ignorescale     = true;
        if (pseq->paramcompanim) pseq->paramcompanim->ignorescale = true;
        if (pseq->paramcenter)   pseq->paramcenter->ignorescale   = true;
    } else {
        bool bHasIgnore = false, bHasNormal = false;
        for (int idx = 0; idx < numblends; idx++) {
            if (animations[idx]) {
                if (animations[idx]->ignorescale) bHasIgnore = true;
                else bHasNormal = true;
            }
        }
        if (bHasIgnore && bHasNormal)
            MdlWarning("sequence \"%s\" blends animations with mixed ignorescale settings - scale mismatch at runtime\n",
                       pseq->name);
    }

    return 0;
}

//-----------------------------------------------------------------------------
// Process the sequence command
//-----------------------------------------------------------------------------
void Cmd_Sequence() {
    if (!GetToken(false))
        return;

    if (g_StudioMdlContext.lCaseAllSequences)
        strlwr(token);

    // Find existing sequences
    const char *pSequenceName = token;
    s_animation_t *panim = LookupAnimation(pSequenceName);
    if (panim != NULL && panim->isOverride) {
        ParseEmpty();
    }

    s_sequence_t *pseq = ProcessCmdSequence(pSequenceName);
    if (pseq) {
        ParseSequence(pseq, false);
    }
}

//-----------------------------------------------------------------------------
// Purpose: append commands to either a sequence or an animation
//-----------------------------------------------------------------------------
void Cmd_Append() {
    GetToken(false);


    s_sequence_t *pseq = LookupSequence(token);

    if (pseq) {
        ParseSequence(pseq, true);
        return;
    } else {
        s_animation_t *panim = LookupAnimation(token);

        if (panim) {
            ParseAnimation(panim, true);
            return;
        }
    }
    TokenError("unknown append animation %s\n", token);
}

void Cmd_Prepend() {
    GetToken(false);

    s_sequence_t *pseq = LookupSequence(token);
    int count = 0;
    s_animation_t *panim = NULL;
    int iRet = false;

    if (pseq) {
        panim = pseq->panim[0][0];
        count = panim->numcmds;
        iRet = ParseSequence(pseq, true);
    } else {
        panim = LookupAnimation(token);
        if (panim) {
            count = panim->numcmds;
            iRet = ParseAnimation(panim, true);
        }
    }
    if (panim && count != panim->numcmds) {
        s_animcmd_t tmp;
        tmp = panim->cmds[panim->numcmds - 1];
        int i;
        for (i = panim->numcmds - 1; i > 0; i--) {
            panim->cmds[i] = panim->cmds[i - 1];
        }
        panim->cmds[0] = tmp;
        return;
    }
    TokenError("unknown prepend animation \"%s\"\n", token);
}

void Cmd_Continue() {
    GetToken(false);

    s_sequence_t *pseq = LookupSequence(token);

    if (pseq) {
        GetToken(true);
        UnGetToken();
        if (token[0] != '$')
            ParseSequence(pseq, true);
        return;
    } else {
        s_animation_t *panim = LookupAnimation(token);

        if (panim) {
            GetToken(true);
            UnGetToken();
            if (token[0] != '$')
                ParseAnimation(panim, true);
            return;
        }
    }
    TokenError("unknown continue animation %s\n", token);
}

//-----------------------------------------------------------------------------
// Purpose: foward declare an empty sequence
//-----------------------------------------------------------------------------
void Cmd_DeclareSequence() {
    if (g_sequence.Count() >= MAXSTUDIOSEQUENCES) {
        TokenError("Too many sequences (%d max)\n", MAXSTUDIOSEQUENCES);
    }

    s_sequence_t *pseq = &g_sequence[g_sequence.AddToTail()];
    memset(pseq, 0, sizeof(s_sequence_t));
    pseq->flags = STUDIO_OVERRIDE;

    // initialize sequence
    GetToken(false);
    strcpyn(pseq->name, token);
}


//-----------------------------------------------------------------------------
// Purpose: foward declare an empty sequence
//-----------------------------------------------------------------------------
void Cmd_DeclareAnimation() {
    if (g_numani >= MAXSTUDIOANIMS) {
        TokenError("Too many animations (%d max)\n", MAXSTUDIOANIMS);
    }

    // allocate animation entry
    s_animation_t *panim = (s_animation_t *) calloc(1, sizeof(s_animation_t));
    g_panimation[g_numani] = panim;
    panim->index = g_numani;
    panim->flags = STUDIO_OVERRIDE;
    g_numani++;

    // initialize animation
    GetToken(false);
    strcpyn(panim->name, token);
}

bool Grab_AimAtBones() {
    s_aimatbone_t *pAimAtBone(&g_aimatbones[g_numaimatbones]);

    // Already know it's <aimconstraint> in the first string, otherwise wouldn't be here
    if (sscanf(g_StudioMdlContext.szLine, "%*s %127s %127s %127s", pAimAtBone->bonename, pAimAtBone->parentname, pAimAtBone->aimname) ==
        3) {
        g_numaimatbones++;

        char cmd[1024];
        Vector vector;

        while (GetLineInput()) {
            g_StudioMdlContext.iLinecount++;

            if (IsEnd(g_StudioMdlContext.szLine)) {
                return false;
            }

            if (sscanf(g_StudioMdlContext.szLine, "%1024s %f %f %f", cmd, &vector[0], &vector[1], &vector[2]) != 4) {
                // Allow blank lines to be skipped without error
                bool allSpace(true);
                for (const char *pC(g_StudioMdlContext.szLine); *pC != '\0' && pC < (g_StudioMdlContext.szLine + 4096); ++pC) {
                    if (!V_isspace(*pC)) {
                        allSpace = false;
                        break;
                    }
                }

                if (allSpace) {
                    continue;
                }

                return true;
            }

            if (stricmp(cmd, "<aimvector>") == 0) {
                // Make sure these are unit length on read
                VectorNormalize(vector);
                pAimAtBone->aimvector = vector;
            } else if (stricmp(cmd, "<upvector>") == 0) {
                // Make sure these are unit length on read
                VectorNormalize(vector);
                pAimAtBone->upvector = vector;
            } else if (stricmp(cmd, "<basepos>") == 0) {
                pAimAtBone->basepos = vector;
            } else {
                return true;
            }
        }
    }

    // If we get here, we're at EOF
    return false;
}

// Quote-aware token scanner for .vrd lines. Reads up to 5 whitespace-delimited
// tokens; each token may be wrapped in double-quotes to allow spaces in bone names.
static int ScanVrdLine(const char *line,
    char *t0, int s0, char *t1, int s1, char *t2, int s2,
    char *t3, int s3, char *t4, int s4)
{
    char *outs[5] = { t0, t1, t2, t3, t4 };
    int   szs[5]  = { s0, s1, s2, s3, s4 };
    const char *p = line;
    int count = 0;
    for (int tok = 0; tok < 5; tok++) {
        while (*p && V_isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char *out = outs[tok];
        int   sz  = szs[tok];
        int   n   = 0;
        if (*p == '"') {
            p++;
            while (*p && *p != '"' && n < sz - 1) out[n++] = *p++;
            if (*p == '"') p++;
        } else {
            while (*p && !V_isspace((unsigned char)*p) && n < sz - 1) out[n++] = *p++;
        }
        out[n] = '\0';
        count++;
    }
    return count;
}

void Grab_QuatInterpBones() {
    char cmd[1024];
    Vector basepos;
    RadianEuler rotateaxis(0.0f, 0.0f, 0.0f);
    RadianEuler jointorient(0.0f, 0.0f, 0.0f);
    s_quatinterpbone_t *pAxis = NULL;
    s_quatinterpbone_t *pBone = &g_quatinterpbones[g_numquatinterpbones];

    while (GetLineInput()) {
        g_StudioMdlContext.iLinecount++;
        if (IsEnd(g_StudioMdlContext.szLine)) {
            return;
        }

        int i = ScanVrdLine(g_StudioMdlContext.szLine,
                            cmd, (int)sizeof(cmd),
                            pBone->bonename, MAXSTUDIONAME,
                            pBone->parentname, MAXSTUDIONAME,
                            pBone->controlparentname, MAXSTUDIONAME,
                            pBone->controlname, MAXSTUDIONAME);

        while (i == 4 && stricmp(cmd, "<aimconstraint>") == 0) {
            // If Grab_AimAtBones() returns false, there file is at EOF
            if (!Grab_AimAtBones()) {
                return;
            }

            // Grab_AimAtBones will read input into g_StudioMdlContext.szLine same as here until it gets a line it doesn't understand, at which point
            // it will exit leaving that line in g_StudioMdlContext.szLine, so check for the end and scan the current buffer again and continue on with
            // the normal QuatInterpBones process

            i = sscanf(g_StudioMdlContext.szLine, "%s %s %s %s %s", cmd, pBone->bonename, pBone->parentname, pBone->controlparentname,
                       pBone->controlname);
        }

        if (i == 5 && stricmp(cmd, "<helper>") == 0) {
            // printf( "\"%s\" \"%s\" \"%s\" \"%s\"\n", cmd, pBone->bonename, tmp, pBone->controlname );
            pAxis = pBone;
            g_numquatinterpbones++;
            pBone = &g_quatinterpbones[g_numquatinterpbones];
        } else if (i > 0) {
            // There was a bug before which could cause the same command to be parsed twice
            // because if the sscanf above completely fails, it will return 0 and not
            // change the contents of cmd, so i should be greater than 0 in order for
            // any of these checks to be valid... Still kind of buggy as these checks
            // do case insensitive stricmp but then the sscanf does case sensitive
            // matching afterwards... Should probably change those to
            // sscanf( g_StudioMdlContext.szLine, "%*s %f ... ) etc...

            if (stricmp(cmd, "<display>") == 0) {
                // skip all display info
                Vector size;
                float distance;

                i = sscanf(g_StudioMdlContext.szLine, "<display> %f %f %f %f",
                           &size[0], &size[1], &size[2],
                           &distance);

                if (i == 4) {
                    pAxis->percentage = distance / 100.0;
                    pAxis->size = size;
                } else {
                    MdlError("Line %d: Unable to parse procedual <display> bone: %s", g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
                }
            } else if (stricmp(cmd, "<basepos>") == 0) {
                i = sscanf(g_StudioMdlContext.szLine, "<basepos> %f %f %f", &basepos.x, &basepos.y, &basepos.z);
                // skip all type info
            } else if (stricmp(cmd, "<rotateaxis>") == 0) {
                i = sscanf(g_StudioMdlContext.szLine, "%*s %f %f %f", &rotateaxis.x, &rotateaxis.y, &rotateaxis.z);
                rotateaxis.x = DEG2RAD(rotateaxis.x);
                rotateaxis.y = DEG2RAD(rotateaxis.y);
                rotateaxis.z = DEG2RAD(rotateaxis.z);
            } else if (stricmp(cmd, "<jointorient>") == 0) {
                i = sscanf(g_StudioMdlContext.szLine, "%*s %f %f %f", &jointorient.x, &jointorient.y, &jointorient.z);
                jointorient.x = DEG2RAD(jointorient.x);
                jointorient.y = DEG2RAD(jointorient.y);
                jointorient.z = DEG2RAD(jointorient.z);
            } else if (stricmp(cmd, "<trigger>") == 0) {
                float tolerance;
                RadianEuler trigger;
                Vector pos;
                RadianEuler ang;

                QAngle rot;
                int j;
                i = sscanf(g_StudioMdlContext.szLine, "<trigger> %f %f %f %f %f %f %f %f %f %f",
                           &tolerance,
                           &trigger.x, &trigger.y, &trigger.z,
                           &ang.x, &ang.y, &ang.z,
                           &pos.x, &pos.y, &pos.z);

                if (i == 10) {
                    trigger.x = DEG2RAD(trigger.x);
                    trigger.y = DEG2RAD(trigger.y);
                    trigger.z = DEG2RAD(trigger.z);
                    ang.x = DEG2RAD(ang.x);
                    ang.y = DEG2RAD(ang.y);
                    ang.z = DEG2RAD(ang.z);

                    Quaternion q;
                    AngleQuaternion(ang, q);

                    if (rotateaxis.x != 0.0 || rotateaxis.y != 0.0 || rotateaxis.z != 0.0) {
                        Quaternion q1;
                        Quaternion q2;
                        AngleQuaternion(rotateaxis, q1);
                        QuaternionMult(q1, q, q2);
                        q = q2;
                    }

                    if (jointorient.x != 0.0 || jointorient.y != 0.0 || jointorient.z != 0.0) {
                        Quaternion q1;
                        Quaternion q2;
                        AngleQuaternion(jointorient, q1);
                        QuaternionMult(q, q1, q2);
                        q = q2;
                    }

                    j = pAxis->numtriggers++;
                    pAxis->tolerance[j] = DEG2RAD(tolerance);
                    AngleQuaternion(trigger, pAxis->trigger[j]);
                    pAxis->pos[j] = (basepos + pos) * g_currentscale;
                    pAxis->quat[j] = q;
                } else {
                    MdlError("Line %d: Unable to parse procedual <trigger> bone: %s", g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
                }
            } else {
                MdlError("Line %d: Unable to parse procedual bone data: %s", g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
            }
        } else {
            // Allow blank lines to be skipped without error
            bool allSpace(true);
            for (const char *pC(g_StudioMdlContext.szLine); *pC != '\0' && pC < (g_StudioMdlContext.szLine + 4096); ++pC) {
                if (!V_isspace(*pC)) {
                    allSpace = false;
                    break;
                }
            }

            if (!allSpace) {
                MdlError("Line %d: Unable to parse procedual bone data: %s", g_StudioMdlContext.iLinecount, g_StudioMdlContext.szLine);
            }
        }
    }
}

void Load_ProceduralBones() {
    char filename[256];
    char cmd[1024];
    int option;

    GetToken(false);
    strcpy(filename, token);

    if (!OpenGlobalFile(filename)) {
        Error("unknown $procedural file \"%s\"\n", filename);
        return;
    }

    g_StudioMdlContext.iLinecount = 0;

    char ext[32];
    Q_ExtractFileExtension(filename, ext, sizeof(ext));

    if (stricmp(ext, "vrd") == 0) {
        Grab_QuatInterpBones();
    } else {
        while (GetLineInput()) {
            g_StudioMdlContext.iLinecount++;
            const int numRead = sscanf(g_StudioMdlContext.szLine, "%s", cmd, &option);

            // No Command Was Parsed, Blank Line Usually
            if ((numRead == EOF) || (numRead == 0))
                continue;

            if (stricmp(cmd, "version") == 0) {
                if (option != 1) {
                    MdlError("bad version\n");
                }
            } else if (stricmp(cmd, "proceduralbones") == 0) {
                Grab_AxisInterpBones();
            }
        }
    }
    fclose(g_StudioMdlContext.fpInput);
}

//-----------------------------------------------------------------------------
// $rendermesh - filter helpers
//-----------------------------------------------------------------------------

static s_source_t *CloneSourceGeometry(s_source_t *pOrig) {
    if (g_numsources >= MAXSTUDIOSEQUENCES)
        MdlError("CloneSourceGeometry: ran out of source slots\n");
    s_source_t *pClone = (s_source_t *)calloc(1, sizeof(s_source_t));
    g_source[g_numsources++] = pClone;

    // Shallow copy all fields; CUtlVector internals are shared with pOrig for now.
    memcpy(pClone, pOrig, sizeof(s_source_t));

    // Deep copy vertex[] and face[]; the filter will free and replace them.
    pClone->vertex = pOrig->numvertices
        ? (s_vertexinfo_t *)malloc(pOrig->numvertices * sizeof(s_vertexinfo_t)) : nullptr;
    if (pClone->vertex)
        memcpy(pClone->vertex, pOrig->vertex, pOrig->numvertices * sizeof(s_vertexinfo_t));
    pClone->face = pOrig->numfaces
        ? (s_face_t *)malloc(pOrig->numfaces * sizeof(s_face_t)) : nullptr;
    if (pClone->face)
        memcpy(pClone->face, pOrig->face, pOrig->numfaces * sizeof(s_face_t));

    // Re-init tracking vectors with independent copies so filters don't interfere.
    // m_GlobalVertices is copied faithfully (drop the aliased ref without freeing pOrig's
    // buffer, then deep-copy). When the filter changes geometry, RebuildGeometryFromKeepMask
    // RemoveAll()s and rebuilds it; for a no-op filter this copy is the correct final state.
    memset(&pClone->m_GlobalVertices, 0, sizeof(pClone->m_GlobalVertices));
    pClone->m_GlobalVertices.AddVectorToTail(pOrig->m_GlobalVertices);
    memset(&pClone->m_FaceDmeMeshIdx, 0, sizeof(pClone->m_FaceDmeMeshIdx));
    pClone->m_FaceDmeMeshIdx.AddVectorToTail(pOrig->m_FaceDmeMeshIdx);
    memset(&pClone->m_DmeMeshNames, 0, sizeof(pClone->m_DmeMeshNames));
    for (int i = 0; i < pOrig->m_DmeMeshNames.Count(); i++)
        pClone->m_DmeMeshNames.AddToTail(pOrig->m_DmeMeshNames[i]);

    // Re-init m_Animations: rawanim is shared (read-only after load),
    // vanim arrays are deep-copied because the filter modifies them in-place.
    memset(&pClone->m_Animations, 0, sizeof(pClone->m_Animations));
    for (int i = 0; i < pOrig->m_Animations.Count(); i++) {
        const s_sourceanim_t &origAnim = pOrig->m_Animations[i];
        int idx = pClone->m_Animations.AddToTail();
        s_sourceanim_t &ca = pClone->m_Animations[idx];
        memcpy(&ca, &origAnim, sizeof(s_sourceanim_t));
        for (int fr = 0; fr < MAXSTUDIOANIMFRAMES; fr++) {
            if (origAnim.vanim[fr] && origAnim.numvanims[fr] > 0) {
                int n = origAnim.numvanims[fr];
                ca.vanim[fr] = (s_vertanim_t *)malloc(n * sizeof(s_vertanim_t));
                memcpy(ca.vanim[fr], origAnim.vanim[fr], n * sizeof(s_vertanim_t));
            }
        }
    }

    // Deep-copy flex metadata. The initial memcpy aliased these CUtlVectors to pOrig's
    // buffers. m_FlexKeys also needs source pointer fixup: without it, BuildVAnimFlags
    // in simplify.cpp skips every flex key because g_flexkey[n].source != pClone.
    // Toppi: this is confusing af.
    memset(&pClone->m_FlexKeys, 0, sizeof(pClone->m_FlexKeys));
    pClone->m_FlexKeys.SetCount(pOrig->m_FlexKeys.Count());
    for (int i = 0; i < pOrig->m_FlexKeys.Count(); i++) {
        pClone->m_FlexKeys[i] = pOrig->m_FlexKeys[i];
        if (pClone->m_FlexKeys[i].source == pOrig)
            pClone->m_FlexKeys[i].source = pClone;
    }
    memset(&pClone->m_CombinationControls, 0, sizeof(pClone->m_CombinationControls));
    pClone->m_CombinationControls.SetCount(pOrig->m_CombinationControls.Count());
    for (int i = 0; i < pOrig->m_CombinationControls.Count(); i++)
        pClone->m_CombinationControls[i] = pOrig->m_CombinationControls[i];
    memset(&pClone->m_CombinationRules, 0, sizeof(pClone->m_CombinationRules));
    pClone->m_CombinationRules.SetCount(pOrig->m_CombinationRules.Count());
    for (int i = 0; i < pOrig->m_CombinationRules.Count(); i++)
        pClone->m_CombinationRules[i] = pOrig->m_CombinationRules[i];
    memset(&pClone->m_FlexControllerRemaps, 0, sizeof(pClone->m_FlexControllerRemaps));
    pClone->m_FlexControllerRemaps.SetCount(pOrig->m_FlexControllerRemaps.Count());
    for (int i = 0; i < pOrig->m_FlexControllerRemaps.Count(); i++)
        pClone->m_FlexControllerRemaps[i] = pOrig->m_FlexControllerRemaps[i];

    // Deep-copy attachments. Without this they stay aliased to pOrig's buffer and
    // AddBodyAttachments() (via PostProcessSource) reads from the shared buffer.
    memset(&pClone->m_Attachments, 0, sizeof(pClone->m_Attachments));
    pClone->m_Attachments.AddVectorToTail(pOrig->m_Attachments);

    // Reset the post-process-derived fields so the clone is treated as a virgin,
    // never-post-processed source. These are written by AddBodyFlexData/AddBodyFlexRules
    // (m_nKeyStartIndex, the raw->remap index tables, and the L/R global flex controller
    // index tables). Leaving them aliased to pOrig (which may already have been
    // post-processed as a plain $body) would carry stale indices and double-register.
    pClone->m_nKeyStartIndex = 0;
    memset(&pClone->m_rawIndexToRemapSourceIndex, 0, sizeof(pClone->m_rawIndexToRemapSourceIndex));
    memset(&pClone->m_rawIndexToRemapLocalIndex, 0, sizeof(pClone->m_rawIndexToRemapLocalIndex));
    memset(&pClone->m_leftRemapIndexToGlobalFlexControllIndex, 0, sizeof(pClone->m_leftRemapIndexToGlobalFlexControllIndex));
    memset(&pClone->m_rightRemapIndexToGlobalFlexControllIndex, 0, sizeof(pClone->m_rightRemapIndexToGlobalFlexControllIndex));

    return pClone;
}

// Rebuilds vertex/face/mesh arrays from a keepFace boolean mask.
// Also remaps vertex animations and rebuilds m_GlobalVertices.
static void RebuildGeometryFromKeepMask(s_source_t *pSource, const CUtlVector<bool> &keepFace) {
    CUtlVector<s_vertexinfo_t> newVerts;
    CUtlVector<s_face_t> newFaces;
    CUtlVector<int16_t> newFaceDmeMeshIdx;
    s_mesh_t newMesh[MAXSTUDIOSKINS];
    int newMeshIndex[MAXSTUDIOSKINS];
    int newNummeshes = 0;
    memset(newMesh, 0, sizeof(newMesh));
    memset(newMeshIndex, 0, sizeof(newMeshIndex));

    // Global vertex remap: oldAbsIdx -> newAbsIdx (-1 if removed)
    CUtlVector<int> globalVertRemap;
    globalVertRemap.SetSize(pSource->numvertices);
    for (int i = 0; i < pSource->numvertices; i++) globalVertRemap[i] = -1;

    for (int m = 0; m < MAXSTUDIOSKINS; m++) {
        if (pSource->mesh[m].numfaces == 0 && pSource->mesh[m].numvertices == 0)
            continue;

        int vOffset = pSource->mesh[m].vertexoffset;
        int fOffset = pSource->mesh[m].faceoffset;
        int numV    = pSource->mesh[m].numvertices;
        int numF    = pSource->mesh[m].numfaces;

        // mesh-relative vert remap: -1 = unused, -2 = needed (unassigned), >=0 = new mesh-rel index
        CUtlVector<int> vertRemap;
        vertRemap.SetSize(numV);
        for (int i = 0; i < numV; i++) vertRemap[i] = -1;

        for (int fi = fOffset; fi < fOffset + numF; fi++) {
            if (!keepFace[fi]) continue;
            auto mark = [&](uint32_t v) {
                if (v != 0xFFFFFFFFu && (int)v < numV && vertRemap[v] == -1)
                    vertRemap[v] = -2;
            };
            mark(pSource->face[fi].a);
            mark(pSource->face[fi].b);
            mark(pSource->face[fi].c);
            mark(pSource->face[fi].d);
        }

        int newVStart = newVerts.Count();
        for (int i = 0; i < numV; i++) {
            if (vertRemap[i] == -2) {
                int newRelIdx = newVerts.Count() - newVStart;
                globalVertRemap[vOffset + i] = newVStart + newRelIdx;
                vertRemap[i] = newRelIdx;
                newVerts.AddToTail(pSource->vertex[vOffset + i]);
            }
        }
        int newVCount = newVerts.Count() - newVStart;

        int newFStart = newFaces.Count();
        for (int fi = fOffset; fi < fOffset + numF; fi++) {
            if (!keepFace[fi]) continue;
            auto remap = [&](uint32_t v) -> uint32_t {
                return (v != 0xFFFFFFFFu && (int)v < numV) ? (uint32_t)vertRemap[v] : 0xFFFFFFFFu;
            };
            s_face_t nf;
            nf.a = remap(pSource->face[fi].a);
            nf.b = remap(pSource->face[fi].b);
            nf.c = remap(pSource->face[fi].c);
            nf.d = remap(pSource->face[fi].d);
            newFaces.AddToTail(nf);
            if (!pSource->m_FaceDmeMeshIdx.IsEmpty())
                newFaceDmeMeshIdx.AddToTail(pSource->m_FaceDmeMeshIdx[fi]);
        }
        int newFCount = newFaces.Count() - newFStart;

        if (newFCount > 0 || newVCount > 0) {
            newMesh[m].vertexoffset = newVStart;
            newMesh[m].numvertices  = newVCount;
            newMesh[m].faceoffset   = newFStart;
            newMesh[m].numfaces     = newFCount;
            newMeshIndex[newNummeshes++] = m;
        }
    }

    // Replace geometry
    free(pSource->vertex);
    pSource->numvertices = newVerts.Count();
    pSource->vertex = pSource->numvertices
        ? (s_vertexinfo_t *)malloc(pSource->numvertices * sizeof(s_vertexinfo_t)) : nullptr;
    if (pSource->vertex)
        memcpy(pSource->vertex, newVerts.Base(), pSource->numvertices * sizeof(s_vertexinfo_t));

    free(pSource->face);
    pSource->numfaces = newFaces.Count();
    pSource->face = pSource->numfaces
        ? (s_face_t *)malloc(pSource->numfaces * sizeof(s_face_t)) : nullptr;
    if (pSource->face)
        memcpy(pSource->face, newFaces.Base(), pSource->numfaces * sizeof(s_face_t));

    memcpy(pSource->mesh, newMesh, sizeof(newMesh));
    memcpy(pSource->meshindex, newMeshIndex, sizeof(newMeshIndex));
    pSource->nummeshes = newNummeshes;

    pSource->m_FaceDmeMeshIdx.RemoveAll();
    pSource->m_FaceDmeMeshIdx.AddVectorToTail(newFaceDmeMeshIdx);

    // Rebuild m_GlobalVertices to match the compacted vertex[] layout so that
    // UnifyLODs index math (vertexoffset + i -> m_GlobalVertices[i]) is correct.
    pSource->m_GlobalVertices.RemoveAll();
    pSource->m_GlobalVertices.AddVectorToTail(newVerts);

    // Remap new-style vertex animation indices
    for (int animIdx = 0; animIdx < pSource->m_Animations.Count(); animIdx++) {
        s_sourceanim_t &anim = pSource->m_Animations[animIdx];
        if (!anim.newStyleVertexAnimations) continue;
        for (int frame = 0; frame < MAXSTUDIOANIMFRAMES; frame++) {
            if (!anim.numvanims[frame] || !anim.vanim[frame]) continue;
            int newCount = 0;
            for (int k = 0; k < anim.numvanims[frame]; k++) {
                int oldVtx = anim.vanim[frame][k].vertex;
                int newVtx = (oldVtx >= 0 && oldVtx < globalVertRemap.Count()) ? globalVertRemap[oldVtx] : -1;
                if (newVtx >= 0) {
                    anim.vanim[frame][newCount] = anim.vanim[frame][k];
                    anim.vanim[frame][newCount].vertex = newVtx;
                    newCount++;
                }
            }
            anim.numvanims[frame] = newCount;
        }
    }

    if (pSource->numfaces == 0)
        MdlWarning("$rendermesh: filter produced empty mesh for '%s'\n", pSource->filename);
}

static void ApplyDmeMeshFilter(s_source_t *pSource, const CUtlVector<CUtlString> &meshNames, bool isIsolate) {
    if (pSource->m_FaceDmeMeshIdx.IsEmpty() || pSource->numfaces == 0)
        return;

    // Build filter mask: which DmeMesh indices match the provided names
    int numDmeMeshes = pSource->m_DmeMeshNames.Count();
    CUtlVector<bool> inFilterSet;
    inFilterSet.SetSize(numDmeMeshes);
    for (int i = 0; i < numDmeMeshes; i++) inFilterSet[i] = false;
    for (int k = 0; k < meshNames.Count(); k++) {
        for (int j = 0; j < numDmeMeshes; j++) {
            if (!Q_strcmp(meshNames[k], pSource->m_DmeMeshNames[j])) { inFilterSet[j] = true; break; }
        }
    }

    // Decide which faces to keep
    CUtlVector<bool> keepFace;
    keepFace.SetSize(pSource->numfaces);
    for (int i = 0; i < pSource->numfaces; i++) {
        int16_t dmeMeshIdx = pSource->m_FaceDmeMeshIdx[i];
        bool inSet = (dmeMeshIdx >= 0 && dmeMeshIdx < numDmeMeshes) && inFilterSet[dmeMeshIdx];
        keepFace[i] = isIsolate ? inSet : !inSet;
    }

    RebuildGeometryFromKeepMask(pSource, keepFace);
}

static void ApplyMaterialRemoveFilter(s_source_t *pSource, s_rendermesh_def_t *pDef) {
    if (pSource->numfaces == 0)
        return;

    // Warn for any removematerial entry that doesn't match a mesh slot in this source
    for (int k = 0; k < pDef->numRemoveMaterials; k++) {
        bool found = false;
        for (int m = 0; m < MAXSTUDIOSKINS && !found; m++) {
            if (pSource->mesh[m].numfaces == 0) continue;
            if (m >= g_nummaterials) continue;
            if (Q_stricmp(g_texture[g_material[m]].name, pDef->removeMaterials[k]) == 0)
                found = true;
        }
        if (!found)
            MdlWarning("$rendermesh '%s': removematerial '%s' not found in source '%s'\n",
                       pDef->name, pDef->removeMaterials[k], pSource->filename);
    }

    // Build keepFace mask: false for any face in a mesh slot whose material matches
    CUtlVector<bool> keepFace;
    keepFace.SetSize(pSource->numfaces);
    for (int i = 0; i < pSource->numfaces; i++) keepFace[i] = true;

    bool anyRemoved = false;
    for (int m = 0; m < MAXSTUDIOSKINS; m++) {
        if (pSource->mesh[m].numfaces == 0) continue;
        if (m >= g_nummaterials) continue;
        const char *matName = g_texture[g_material[m]].name;

        bool remove = false;
        for (int k = 0; k < pDef->numRemoveMaterials && !remove; k++) {
            if (Q_stricmp(matName, pDef->removeMaterials[k]) == 0)
                remove = true;
        }
        if (!remove) continue;

        Msg("$rendermesh '%s': removing faces with material '%s'\n", pDef->name, matName);
        int fOffset = pSource->mesh[m].faceoffset;
        int numF    = pSource->mesh[m].numfaces;
        for (int fi = fOffset; fi < fOffset + numF; fi++)
            keepFace[fi] = false;
        anyRemoved = true;
    }

    if (anyRemoved)
        RebuildGeometryFromKeepMask(pSource, keepFace);
}

// Strips all facial data from a (cloned) source: flex keys, combination controls/rules,
// flex controller remaps, and the vertex-animation deltas. With these cleared,
// AddBodyFlexData / AddBodyFlexRules (run later via PostProcessSource) register no flex
// controllers, flex rules, or flex keys for this source, so the result has no facial flexing.
static void ApplyNoFacialFilter(s_source_t *pSource) {
    int nFlexKeys = pSource->m_FlexKeys.Count();

    pSource->m_FlexKeys.RemoveAll();
    pSource->m_CombinationControls.RemoveAll();
    pSource->m_CombinationRules.RemoveAll();
    pSource->m_FlexControllerRemaps.RemoveAll();
    pSource->bNoAutoDMXRules = true;

    // Free and drop the vertex-animation deltas. The clone owns its vanim[] buffers
    // (CloneSourceGeometry malloc'd independent copies), so freeing them is safe; the
    // remaining vanim_map/vanim_flag pointers are still aliased to the original and are
    // left untouched. rawanim (skeletal/bind pose) is preserved.
    for (int a = 0; a < pSource->m_Animations.Count(); a++) {
        s_sourceanim_t &anim = pSource->m_Animations[a];
        for (int fr = 0; fr < MAXSTUDIOANIMFRAMES; fr++) {
            if (anim.vanim[fr]) {
                free(anim.vanim[fr]);
                anim.vanim[fr] = nullptr;
            }
            anim.numvanims[fr] = 0;
        }
        anim.newStyleVertexAnimations = false;
    }

    Msg("$rendermesh: nofacial - stripped %d flex key%s and all flex rules/deltas from '%s'\n",
        nFlexKeys, nFlexKeys == 1 ? "" : "s", pSource->filename);
}

static void ApplyRenderMeshFilter(s_source_t *pSource, s_rendermesh_def_t *pDef) {
    pDef->used = true;
    Msg("$rendermesh '%s': applying to '%s' (%d mesh override%s, %d removematerial%s)\n",
        pDef->name, pSource->filename,
        pDef->numOverrides, pDef->numOverrides == 1 ? "" : "s",
        pDef->numRemoveMaterials, pDef->numRemoveMaterials == 1 ? "" : "s");

    // Apply DmeMesh filter (requires DmeMesh tracking in the source)
    if (pDef->numOverrides > 0) {
        if (pSource->m_DmeMeshNames.IsEmpty()) {
            MdlWarning("$rendermesh '%s': source '%s' has no DmeMesh tracking data (not a DMX?), mesh overrides ignored\n",
                       pDef->name, pSource->filename);
        } else {
            // Validate override mesh names exist in the source
            for (int j = 0; j < pDef->numOverrides; j++) {
                bool found = false;
                for (int k = 0; k < pSource->m_DmeMeshNames.Count(); k++) {
                    if (!Q_strcmp(pDef->overrides[j].meshName, pSource->m_DmeMeshNames[k])) { found = true; break; }
                }
                if (!found) {
                    char available[1024] = "";
                    for (int k = 0; k < pSource->m_DmeMeshNames.Count(); k++) {
                        Q_strncat(available, pSource->m_DmeMeshNames[k].Get(), sizeof(available));
                        if (k < pSource->m_DmeMeshNames.Count() - 1) Q_strncat(available, ", ", sizeof(available));
                    }
                    MdlError("$rendermesh '%s': DmeMesh '%s' not found in '%s'\n  Available: %s\n",
                             pDef->name, pDef->overrides[j].meshName, pSource->filename, available);
                }
            }

            // defaultState=0: listed meshes are kept (isolate). defaultState=1: listed meshes are dropped (exclude).
            CUtlVector<CUtlString> filterMeshes;
            bool isIsolate = !pDef->defaultState;
            for (int j = 0; j < pDef->numOverrides; j++)
                filterMeshes.AddToTail(pDef->overrides[j].meshName);

            if (filterMeshes.IsEmpty()) {
                // defaultState=0 with no listed meshes produces nothing; defaultState=1 keeps all (no-op)
                if (!pDef->defaultState)
                    MdlWarning("$rendermesh '%s': defaultState=0 with no listed meshes produces an empty model\n", pDef->name);
            } else {
                ApplyDmeMeshFilter(pSource, filterMeshes, isIsolate);
            }
        }
    }

    // Apply material remove filter (works independently of DmeMesh tracking)
    if (pDef->numRemoveMaterials > 0)
        ApplyMaterialRemoveFilter(pSource, pDef);

    // Strip facial data (flex controllers/rules/deltas) if requested
    if (pDef->noFacial)
        ApplyNoFacialFilter(pSource);

    if (pSource->numfaces == 0)
        MdlError("$rendermesh '%s': filter produced a 0-polygon mesh from '%s' - check mesh names and defaultState\n",
                 pDef->name, pSource->filename);
}

void Cmd_RenderMesh() {
    if (g_numRendermeshDefs >= MAX_RENDERMESH_DEFS)
        MdlError("$rendermesh: too many definitions (max %d)\n", MAX_RENDERMESH_DEFS);

    s_rendermesh_def_t &def = g_rendermeshDefs[g_numRendermeshDefs];
    memset(&def, 0, sizeof(def));

    GetToken(false);
    Q_strncpy(def.name, token, sizeof(def.name));

    for (int i = 0; i < g_numRendermeshDefs; i++) {
        if (!Q_strcmp(g_rendermeshDefs[i].name, def.name))
            MdlError("$rendermesh: name '%s' is already defined\n", def.name);
    }

    GetToken(false);
    Q_strncpy(def.filename, token, sizeof(def.filename));

    GetToken(false);
    {
        char *endptr;
        long val = strtol(token, &endptr, 10);
        if (*endptr != '\0' || endptr == token || (val != 0 && val != 1))
            TokenError("$rendermesh '%s': expected 0 or 1 for defaultState, got '%s'\n", def.name, token);
        def.defaultState = (val != 0);
    }

    GetToken(true);
    if (token[0] != '{')
        TokenError("$rendermesh '%s': expected '{', got '%s'\n", def.name, token);

    while (true) {
        GetToken(true);
        if (endofscript) TokenError("$rendermesh '%s': unexpected end of file\n", def.name);
        if (token[0] == '}') break;

        if (Q_stricmp(token, "removematerial") == 0) {
            if (def.numRemoveMaterials >= MAX_RENDERMESH_MATERIAL_REMOVES)
                MdlError("$rendermesh '%s': too many removematerial entries (max %d)\n", def.name, MAX_RENDERMESH_MATERIAL_REMOVES);
            GetToken(false);
            Q_strncpy(def.removeMaterials[def.numRemoveMaterials], token, MAXSTUDIONAME);
            def.numRemoveMaterials++;
        } else if (Q_stricmp(token, "nofacial") == 0) {
            def.noFacial = true;
        } else {
            if (def.numOverrides >= MAX_RENDERMESH_OVERRIDES)
                MdlError("$rendermesh '%s': too many mesh overrides (max %d)\n", def.name, MAX_RENDERMESH_OVERRIDES);
            Q_strncpy(def.overrides[def.numOverrides].meshName, token,
                      sizeof(def.overrides[def.numOverrides].meshName));
            def.numOverrides++;
        }
    }

    Msg("$rendermesh: defined '%s' -> '%s' (defaultState=%d, %d mesh override%s, %d removematerial%s%s)\n",
        def.name, def.filename, (int)def.defaultState,
        def.numOverrides, def.numOverrides == 1 ? "" : "s",
        def.numRemoveMaterials, def.numRemoveMaterials == 1 ? "" : "s",
        def.noFacial ? ", nofacial" : "");
    g_numRendermeshDefs++;
}

void ReportUnusedRenderMeshDefs() {
    for (int i = 0; i < g_numRendermeshDefs; i++) {
        if (!g_rendermeshDefs[i].used)
            MdlWarning("$rendermesh '%s' was defined but never used\n", g_rendermeshDefs[i].name);
    }
}

//-----------------------------------------------------------------------------
// Resolve a $rendermesh by name to its loaded, filtered geometry.
//
// Used by the collision auto-generation path ($generate / $generatejoint) so it
// can decompose an already-filtered render mesh (DmeMesh isolate/exclude,
// removematerial, nofacial) into convex collision.  Lazily loads and filters the
// owned source on first use, exactly like ProcessOptionStudio does for $body.
// Returns the shared owned source (do NOT free it); NULL if the name is unknown.
//-----------------------------------------------------------------------------
s_source_t *GetRenderMeshSource(const char *name) {
    s_rendermesh_def_t *pDef = FindRenderMeshDef(name);
    if (!pDef)
        return nullptr;

    if (!pDef->pOwnedSource) {
        s_source_t *pRaw = Load_Source(pDef->filename, "", false, true);
        if (pRaw) {
            s_source_t *pOwned = CloneSourceGeometry(pRaw);
            ApplyRenderMeshFilter(pOwned, pDef);
            pDef->pOwnedSource = pOwned;
        }
    }

    pDef->used = true;
    return pDef->pOwnedSource;
}


//-----------------------------------------------------------------------------
// $driverbone - inline quatinterp procedural bone driven by triggerpose frames
// Based on $nekodriverbone from NekoMDL
//
// Syntax:
//   $driverbone <driver_bone> {
//       triggerpose <file>
//       [unlockbones]
//       [noculltriggers]
//       [autotrigger <angle_deg> [<min_frame> <max_frame>]]
//       trigger <tolerance_deg> <frame>   // as many as needed (max 32 total)
//       ...
//       <helper_bone_name> ...
//   }
//
// For each helper bone, one STUDIO_PROC_QUATINTERP entry is emitted.
// The driver bone's local rotation at 'frame' becomes the trigger quaternion.
// The helper bone's local pos/quat at 'frame' becomes the target pose.
// With 'unlockbones', helper bone position is locked to the rest pose (frame 0)
// so rotation-only driving works across skeletons with different proportions.
// 'autotrigger' auto-generates one trigger per frame in [min,max]; any explicit
// 'trigger' entry for the same frame takes priority.
// By default, near-duplicate triggers (driver rotation within 1 deg of an earlier
// trigger) are culled and reported. 'noculltriggers' disables this cleanup.
//-----------------------------------------------------------------------------

void Cmd_DriverBone() {
    // Toppi: I feel like this could've been shorter
    char driverBoneName[MAXSTUDIONAME];
    GetToken(false);
    Q_strncpy(driverBoneName, token, sizeof(driverBoneName));

    char triggerPoseFile[MAX_PATH] = {};
    bool unlockBones      = false;
    bool noCullTriggers   = false;
    bool hasAutoTrigger   = false;
    float autoTriggerAngle = 5.0f;
    int   autoTriggerMin  = -1;
    int   autoTriggerMax  = -1;

    struct TriggerEntry { float toleranceDeg; int frame; };
    CUtlVector<TriggerEntry> explicitTriggers;

    int  numHelperBones = 0;
    char helperBoneNames[MAXSTUDIOBONES][MAXSTUDIONAME];

    GetToken(true);
    if (stricmp(token, "{")) {
        MdlError("$driverbone: expected '{', got '%s'\n", token);
        return;
    }

    while (GetToken(true)) {
        if (!stricmp(token, "}"))
            break;

        if (!stricmp(token, "triggerpose")) {
            GetToken(false);
            Q_strncpy(triggerPoseFile, token, sizeof(triggerPoseFile));
        } else if (!stricmp(token, "unlockbones")) {
            unlockBones = true;
        } else if (!stricmp(token, "noculltriggers")) {
            noCullTriggers = true;
        } else if (!stricmp(token, "trigger")) {
            TriggerEntry e;
            GetToken(false); e.toleranceDeg = (float)atof(token);
            GetToken(false); e.frame        = atoi(token);
            explicitTriggers.AddToTail(e);
        } else if (!stricmp(token, "autotrigger")) {
            hasAutoTrigger = true;
            GetToken(false);
            autoTriggerAngle = (float)atof(token);
            if (TokenAvailable()) {
                GetToken(false);
                autoTriggerMin = atoi(token);
                if (TokenAvailable()) {
                    GetToken(false);
                    autoTriggerMax = atoi(token);
                }
            }
        } else {
            if (numHelperBones < MAXSTUDIOBONES) {
                Q_strncpy(helperBoneNames[numHelperBones++], token, MAXSTUDIONAME);
            } else {
                MdlWarning("$driverbone '%s': too many helper bones; ignoring '%s'\n",
                           driverBoneName, token);
            }
        }
    }

    if (!triggerPoseFile[0]) {
        MdlError("$driverbone '%s': missing 'triggerpose <file>'\n", driverBoneName);
        return;
    }
    if (!numHelperBones) {
        MdlWarning("$driverbone '%s': no helper bones specified\n", driverBoneName);
        return;
    }

    s_source_t *pSrc = Load_Source(triggerPoseFile, "", false, false, true);
    if (!pSrc || pSrc->m_Animations.Count() == 0) {
        MdlError("$driverbone '%s': cannot load triggerpose '%s'\n",
                 driverBoneName, triggerPoseFile);
        return;
    }
    s_sourceanim_t *pAnim = &pSrc->m_Animations[0];

    int driverLocalIdx = -1;
    for (int i = 0; i < pSrc->numbones; i++) {
        if (!stricmp(pSrc->localBone[i].name, driverBoneName)) {
            driverLocalIdx = i;
            break;
        }
    }
    if (driverLocalIdx < 0) {
        MdlError("$driverbone: driver bone '%s' not found in triggerpose '%s'\n",
                 driverBoneName, triggerPoseFile);
        return;
    }

    // Build full trigger list: explicit entries first, autotrigger fills the rest
    CUtlVector<TriggerEntry> allTriggers;
    for (int i = 0; i < explicitTriggers.Count(); i++)
        allTriggers.AddToTail(explicitTriggers[i]);

    if (hasAutoTrigger) {
        int fMin = (autoTriggerMin >= 0) ? autoTriggerMin : 0;
        int fMax = (autoTriggerMax >= 0) ? autoTriggerMax : (pAnim->numframes - 1);
        fMin = max(fMin, 0);
        fMax = min(fMax, pAnim->numframes - 1);
        for (int f = fMin; f <= fMax; f++) {
            bool covered = false;
            for (int e = 0; e < explicitTriggers.Count(); e++) {
                if (explicitTriggers[e].frame == f) { covered = true; break; }
            }
            if (!covered) {
                TriggerEntry e; e.toleranceDeg = autoTriggerAngle; e.frame = f;
                allTriggers.AddToTail(e);
            }
        }
    }

    if (allTriggers.Count() == 0) {
        MdlWarning("$driverbone '%s': no triggers defined\n", driverBoneName);
        return;
    }

    if (!noCullTriggers && allTriggers.Count() > 1) {
        // Pre-compute driver bone quaternions so we can compare trigger poses.
        CUtlVector<Quaternion> triggerQuats;
        triggerQuats.SetCount(allTriggers.Count());
        for (int i = 0; i < allTriggers.Count(); i++) {
            int f = max(0, min(allTriggers[i].frame, pAnim->numframes - 1));
            s_bone_t *pFrameBones = (f < pAnim->rawanim.Count()) ? pAnim->rawanim[f] : nullptr;
            if (pFrameBones)
                AngleQuaternion(pFrameBones[driverLocalIdx].rot, triggerQuats[i]);
            else
                triggerQuats[i].Init(0.0f, 0.0f, 0.0f, 1.0f);
        }

        // Mark entries whose driver rotation falls within 1 deg of an earlier kept entry.
        CUtlVector<bool> keep;
        keep.SetCount(allTriggers.Count());
        for (int i = 0; i < allTriggers.Count(); i++) keep[i] = true;

        int numCulled = 0;
        for (int i = 0; i < allTriggers.Count(); i++) {
            if (!keep[i]) continue;
            for (int j = i + 1; j < allTriggers.Count(); j++) {
                if (!keep[j]) continue;
                float angleDiff = RAD2DEG(QuaternionAngleDiff(triggerQuats[i], triggerQuats[j]));
                if (angleDiff < 1.0f) {
                    keep[j] = false;
                    numCulled++;
                    if (!g_StudioMdlContext.quiet)
                        Msg("$driverbone '%s': culling near-duplicate trigger frame %d"
                            " (%.2f deg from frame %d)\n",
                            driverBoneName, allTriggers[j].frame, angleDiff, allTriggers[i].frame);
                }
            }
        }

        if (numCulled > 0) {
            CUtlVector<TriggerEntry> filtered;
            for (int i = 0; i < allTriggers.Count(); i++) {
                if (keep[i]) filtered.AddToTail(allTriggers[i]);
            }
            allTriggers.RemoveAll();
            for (int i = 0; i < filtered.Count(); i++)
                allTriggers.AddToTail(filtered[i]);
            Msg("$driverbone '%s': culled %d near-duplicate trigger(s), %d remaining\n",
                driverBoneName, numCulled, allTriggers.Count());
        }
    }

    if (allTriggers.Count() > 32) {
        MdlWarning("$driverbone '%s': %d triggers exceed limit of 32; truncating\n",
                   driverBoneName, allTriggers.Count());
        allTriggers.SetCount(32);
    }

    for (int h = 0; h < numHelperBones; h++) {
        if (g_numquatinterpbones >= MAXSTUDIOBONES) {
            MdlError("$driverbone: quatinterp bone limit (%d) reached\n", MAXSTUDIOBONES);
            return;
        }

        s_quatinterpbone_t *pBone = &g_quatinterpbones[g_numquatinterpbones];
        memset(pBone, 0, sizeof(*pBone));
        Q_strncpy(pBone->bonename,    helperBoneNames[h], MAXSTUDIONAME);
        Q_strncpy(pBone->controlname, driverBoneName,     MAXSTUDIONAME);

        int helperLocalIdx = -1;
        for (int i = 0; i < pSrc->numbones; i++) {
            if (!stricmp(pSrc->localBone[i].name, helperBoneNames[h])) {
                helperLocalIdx = i;
                break;
            }
        }
        if (helperLocalIdx < 0) {
            MdlWarning("$driverbone: helper bone '%s' not found in '%s'; skipping\n",
                       helperBoneNames[h], triggerPoseFile);
            continue;
        }

        pBone->unlockbones  = unlockBones;
        pBone->numtriggers  = allTriggers.Count();

        // For unlockbones: store delta relative to the triggerpose bind pose (frame 0).
        // simplify.cpp will add the actual skeleton bind pose on top.
        // For normal mode: store absolute pos/quat from the triggerpose frame.
        Quaternion bindQuatTP;
        Vector     bindPosTP(0.0f, 0.0f, 0.0f);
        if (unlockBones) {
            s_bone_t *pBindFrame = (pAnim->rawanim.Count() > 0) ? pAnim->rawanim[0] : nullptr;
            if (pBindFrame) {
                AngleQuaternion(pBindFrame[helperLocalIdx].rot, bindQuatTP);
                bindPosTP = pBindFrame[helperLocalIdx].pos;
            } else {
                bindQuatTP.Init(0.0f, 0.0f, 0.0f, 1.0f);
            }
        }

        for (int t = 0; t < allTriggers.Count(); t++) {
            int frame = max(0, min(allTriggers[t].frame, pAnim->numframes - 1));
            pBone->tolerance[t] = DEG2RAD(allTriggers[t].toleranceDeg);

            s_bone_t *pFrameBones = pAnim->rawanim[frame];
            if (!pFrameBones) {
                pBone->trigger[t].Init(0.0f, 0.0f, 0.0f, 1.0f);
                pBone->quat[t].Init(0.0f, 0.0f, 0.0f, 1.0f);
                pBone->pos[t].Init(0.0f, 0.0f, 0.0f);
                continue;
            }

            AngleQuaternion(pFrameBones[driverLocalIdx].rot, pBone->trigger[t]);

            if (unlockBones) {
                // delta_quat = inv(bind) * frame  - rotation offset from triggerpose rest
                Quaternion frameQuat, invBind;
                AngleQuaternion(pFrameBones[helperLocalIdx].rot, frameQuat);
                QuaternionInvert(bindQuatTP, invBind);
                QuaternionMult(invBind, frameQuat, pBone->quat[t]);
                // rawanim positions are already scaled; store delta from triggerpose bind pose
                pBone->pos[t] = pFrameBones[helperLocalIdx].pos - bindPosTP;
            } else {
                AngleQuaternion(pFrameBones[helperLocalIdx].rot, pBone->quat[t]);
                pBone->pos[t] = pFrameBones[helperLocalIdx].pos;
            }
        }

        g_numquatinterpbones++;
    }

    if (!g_StudioMdlContext.quiet)
        Msg("$driverbone '%s': %d helper bone(s), %d trigger(s)\n",
            driverBoneName, numHelperBones, allTriggers.Count());
}


//-----------------------------------------------------------------------------
// $driverlookat - inline aimatbone procedural look-at
//
// Syntax:
//   $driverlookat <aim_target_bone_or_attachment> {
//       [upvector  <x> <y> <z>]   // default: 1 0 0
//       [aimvector <x> <y> <z>]   // default: 0 0 1
//       [origin    <x> <y> <z>]   // offset the aim target (basepos)
//       <helper_bone_name> ...
//   }
//
// For each helper bone, one STUDIO_PROC_AIMATBONE (or AIMATATTACH) entry is
// emitted. The parent bone is auto-derived from the skeleton hierarchy in
// simplify.cpp, so it does not need to be specified manually.
//-----------------------------------------------------------------------------

void Cmd_DriverLookAt() {
    char aimTargetName[MAXSTUDIONAME];
    GetToken(false);
    Q_strncpy(aimTargetName, token, sizeof(aimTargetName));

    Vector upVector (1.0f, 0.0f, 0.0f);
    Vector aimVector(0.0f, 0.0f, 1.0f);
    Vector origin   (0.0f, 0.0f, 0.0f);

    int  numHelperBones = 0;
    char helperBoneNames[MAXSTUDIOBONES][MAXSTUDIONAME];

    GetToken(true);
    if (stricmp(token, "{")) {
        MdlError("$driverlookat: expected '{', got '%s'\n", token);
        return;
    }

    while (GetToken(true)) {
        if (!stricmp(token, "}"))
            break;

        if (!stricmp(token, "upvector")) {
            GetToken(false); upVector.x = (float)atof(token);
            GetToken(false); upVector.y = (float)atof(token);
            GetToken(false); upVector.z = (float)atof(token);
            VectorNormalize(upVector);
        } else if (!stricmp(token, "aimvector")) {
            GetToken(false); aimVector.x = (float)atof(token);
            GetToken(false); aimVector.y = (float)atof(token);
            GetToken(false); aimVector.z = (float)atof(token);
            VectorNormalize(aimVector);
        } else if (!stricmp(token, "origin")) {
            GetToken(false); origin.x = (float)atof(token);
            GetToken(false); origin.y = (float)atof(token);
            GetToken(false); origin.z = (float)atof(token);
        } else {
            if (numHelperBones < MAXSTUDIOBONES) {
                Q_strncpy(helperBoneNames[numHelperBones++], token, MAXSTUDIONAME);
            } else {
                MdlWarning("$driverlookat '%s': too many helper bones; ignoring '%s'\n",
                           aimTargetName, token);
            }
        }
    }

    if (!numHelperBones) {
        MdlWarning("$driverlookat '%s': no helper bones specified\n", aimTargetName);
        return;
    }

    for (int h = 0; h < numHelperBones; h++) {
        if (g_numaimatbones >= MAXSTUDIOBONES) {
            MdlError("$driverlookat: aimatbone limit (%d) reached\n", MAXSTUDIOBONES);
            return;
        }

        s_aimatbone_t *pAimAt = &g_aimatbones[g_numaimatbones];
        memset(pAimAt, 0, sizeof(*pAimAt));

        Q_strncpy(pAimAt->bonename,  helperBoneNames[h], MAXSTUDIONAME);
        pAimAt->parentname[0] = '\0';  // auto-resolved from skeleton in simplify.cpp
        Q_strncpy(pAimAt->aimname,   aimTargetName,      MAXSTUDIONAME);
        pAimAt->aimvector = aimVector;
        pAimAt->upvector  = upVector;
        pAimAt->basepos     = origin * g_currentscale;
        pAimAt->autobasepos = true;
        pAimAt->aimAttach = -1;
        pAimAt->aimBone   = -1;
        pAimAt->bone      = -1;
        pAimAt->parent    = -1;

        g_numaimatbones++;
    }

    if (!g_StudioMdlContext.quiet)
        Msg("$driverlookat '%s': %d bone(s) registered\n", aimTargetName, numHelperBones);
}

static void Cmd_Elif()    { Error("$elif must immediately follow the closing '}' of a $if or $elif block\n"); }
static void Cmd_Else()    { Error("$else must immediately follow the closing '}' of a $if or $elif block\n"); }
static void Cmd_Case()    { Error("$case is only valid directly inside a $switch block\n"); }
static void Cmd_Default() { Error("$default is only valid directly inside a $switch block\n"); }

//-----------------------------------------------------------------------------
// $deltaproportions
//-----------------------------------------------------------------------------

struct DP_AbsTransform {
    Vector     pos;
    Quaternion rot;
};

// Compute world-space transforms from local bone data.
static void DP_BuildAbsTransforms(const s_source_t *src, const s_bone_t *bones,
                                   CUtlVector<DP_AbsTransform> &out) {
    int n = src->numbones;
    out.SetCount(n);
    for (int i = 0; i < n; i++) {
        int par = src->localBone[i].parent;
        Quaternion localRot;
        AngleQuaternion(bones[i].rot, localRot);
        if (par < 0) {
            out[i].pos = bones[i].pos;
            out[i].rot = localRot;
        } else {
            Vector rotated;
            QuaternionMultiply(out[par].rot, bones[i].pos, rotated);
            out[i].pos = out[par].pos + rotated;
            QuaternionMult(out[par].rot, localRot, out[i].rot);
        }
    }
}

static CUtlString DP_CollectAnimBlock() {
    GetToken(true);
    if (token[0] != '{')
        TokenError("$deltaproportions: expected '{' for qc block\n");

    CUtlString buf;
    int depth = 1;
    while (GetToken(true)) {
        if (token[0] == '{') {
            depth++;
            buf += "{ ";
        } else if (token[0] == '}') {
            depth--;
            if (depth == 0) {
                break;
            }
            buf += "} ";
        } else {
            // re-quote tokens that contain spaces (e.g. bone names with spaces)
            if (strchr(token, ' ') || strchr(token, '\t')) {
                buf += "\"";
                buf += token;
                buf += "\" ";
            } else {
                buf += token;
                buf += " ";
            }
        }
    }
    return buf;
}

void PushMemoryScript(char *pszBuffer, const int nSize);
bool PopMemoryScript();

static void DP_ApplyAnimBlock(const char *animName, const CUtlString &block) {
    if (!block.Length()) return;
    s_animation_t *pa = LookupAnimation(animName);
    if (!pa) return;

    // PushMemoryScript requires a mutable buffer; make a copy
    CUtlString mutableBuf = block;
    PushMemoryScript(mutableBuf.Get(), mutableBuf.Length());
    ParseAnimation(pa, false);
    PopMemoryScript();
}


static void DP_RegisterInMemoryAnim(const char *animName,
                                    const s_source_t *refSrc,
                                    const CUtlVector<Vector> &lpos,
                                    const CUtlVector<RadianEuler> &lrot,
                                    const bool *pSkip = nullptr) {
    if (g_numani >= MAXSTUDIOANIMS) {
        MdlError("$deltaproportions: too many animations (max %d)\n", MAXSTUDIOANIMS);
        return;
    }
    if (LookupAnimation(animName)) {
        MdlWarning("$deltaproportions: animation '%s' already defined, skipping\n", animName);
        return;
    }

    int nBones = refSrc->numbones;

    // allocate source and register it so the pipeline can remap bones
    s_source_t *pSrc = (s_source_t *)calloc(1, sizeof(s_source_t));
    g_source[g_numsources++] = pSrc;
    Q_snprintf(pSrc->filename, sizeof(pSrc->filename), "<deltaproportions:%s>", animName);
    pSrc->numbones = nBones;
    for (int i = 0; i < nBones; i++)
        pSrc->localBone[i] = refSrc->localBone[i];

    // single animation with one frame
    int animIdx = pSrc->m_Animations.AddToTail();
    s_sourceanim_t &sa = pSrc->m_Animations[animIdx];
    Q_strncpy(sa.animationname, animName, sizeof(sa.animationname));
    sa.numframes   = 1;
    sa.startframe  = 0;
    sa.endframe    = 0;

    s_bone_t *frameBones = new s_bone_t[nBones]();  // zero-init; skipped bones stay at zero
    for (int i = 0; i < nBones; i++) {
        if (pSkip && pSkip[i]) continue;
        frameBones[i].pos = lpos[i];
        frameBones[i].rot = lrot[i];
    }
    sa.rawanim.AddToTail(frameBones);

    // build boneToPose from frame 0 so the pipeline can resolve world poses
    Build_Reference(pSrc, animName);

    // register animation entry
    g_panimation[g_numani] = (s_animation_t *)calloc(1, sizeof(s_animation_t));
    s_animation_t *pa = g_panimation[g_numani];
    pa->index      = g_numani++;
    pa->source     = pSrc;
    pa->startframe = 0;
    pa->endframe   = 0;
    pa->numframes  = 1;
    Q_strncpy(pa->name,          animName, sizeof(pa->name));
    Q_strncpy(pa->filename,      pSrc->filename, sizeof(pa->filename));
    Q_strncpy(pa->animationname, animName, sizeof(pa->animationname));
    VectorCopy(g_defaultadjust, pa->adjust);
    pa->rotation       = g_defaultrotation;
    pa->scale          = 1.0f;
    pa->fps            = 30.0f;
    pa->motionrollback = g_StudioMdlContext.defaultMotionRollback;
}

struct DP_BoneParams {
    bool   ignore        = false;
    bool   hasOffsetPos  = false;
    bool   hasOffsetAngle = false;
    bool   hasIgnorePos  = false;
    bool   hasIgnoreAngle = false;
    Vector offsetPos;
    Vector offsetAngle; 
    Vector ignorePos;
    Vector ignoreAngle;
};

void Cmd_DeltaProportions() {
    char refPath[MAX_PATH]  = {};
    int  refFrame           = 0;
    char propPath[MAX_PATH] = {};
    int  propFrame          = 0;
    bool hasProp            = false;
    CUtlString qcRefBlock, qcPropBlock;
    CUtlDict<DP_BoneParams, int> boneCfg;  // bone name -> per-bone overrides

    // Toppi: This function is so long! TODO: Comeback and do revision!!

    GetToken(true);
    if (token[0] != '{')
        TokenError("$deltaproportions: expected '{'\n");

    while (GetToken(true)) {
        if (token[0] == '}') break;
        if (!Q_stricmp(token, "referencepose")) {
            GetToken(false); Q_strncpy(refPath,  token, sizeof(refPath));
            GetToken(false); refFrame  = atoi(token);
        } else if (!Q_stricmp(token, "proportionpose")) {
            GetToken(false); Q_strncpy(propPath, token, sizeof(propPath));
            GetToken(false); propFrame = atoi(token);
            hasProp = true;
        } else if (!Q_stricmp(token, "appendreference")) {
            qcRefBlock  = DP_CollectAnimBlock();
        } else if (!Q_stricmp(token, "appendproportions")) {
            qcPropBlock = DP_CollectAnimBlock();
        } else {
            char boneName[MAXSTUDIONAME];
            Q_strncpy(boneName, token, sizeof(boneName));

            DP_BoneParams bp;
            while (TokenAvailable()) {
                GetToken(false);
                if (!Q_stricmp(token, "ignore")) {
                    bp.ignore = true;
                } else if (!Q_stricmp(token, "offsetpos")) {
                    GetToken(false); bp.offsetPos.x = (float)atof(token);
                    GetToken(false); bp.offsetPos.y = (float)atof(token);
                    GetToken(false); bp.offsetPos.z = (float)atof(token);
                    bp.hasOffsetPos = true;
                } else if (!Q_stricmp(token, "offsetangle")) {
                    GetToken(false); bp.offsetAngle.x = (float)atof(token);
                    GetToken(false); bp.offsetAngle.y = (float)atof(token);
                    GetToken(false); bp.offsetAngle.z = (float)atof(token);
                    bp.hasOffsetAngle = true;
                } else if (!Q_stricmp(token, "ignorepos")) {
                    GetToken(false); bp.ignorePos.x = (float)atof(token);
                    GetToken(false); bp.ignorePos.y = (float)atof(token);
                    GetToken(false); bp.ignorePos.z = (float)atof(token);
                    bp.hasIgnorePos = true;
                } else if (!Q_stricmp(token, "ignoreangle")) {
                    GetToken(false); bp.ignoreAngle.x = (float)atof(token);
                    GetToken(false); bp.ignoreAngle.y = (float)atof(token);
                    GetToken(false); bp.ignoreAngle.z = (float)atof(token);
                    bp.hasIgnoreAngle = true;
                } else {
                    TokenError("$deltaproportions: unknown bone param '%s' for bone '%s'\n",
                               token, boneName);
                }
            }
            boneCfg.Insert(boneName, bp);
        }
    }

    if (!refPath[0])
        TokenError("$deltaproportions: 'referencepose' is required\n");

    // snapshot material/texture state - these sources are skeleton-only,
    // any mesh materials they register must not end up in the final mdl
    const int savedNumTextures  = g_numtextures;
    const int savedNumMaterials = g_nummaterials;

    // load reference source
    s_source_t *pRef = Load_Source(refPath, "");
    if (!pRef || !pRef->numbones || !pRef->m_Animations.Count())
        MdlError("$deltaproportions: could not load referencepose '%s'\n", refPath);
    s_sourceanim_t &refAnim = pRef->m_Animations[0];
    if (refFrame >= refAnim.numframes)
        MdlError("$deltaproportions: referencepose frame %d out of range (has %d)\n",
                 refFrame, refAnim.numframes);
    s_bone_t *refBones = refAnim.rawanim[refFrame];

    // load or derive proportion source
    s_source_t *pProp     = nullptr;
    s_bone_t   *propBones = nullptr;
    if (hasProp) {
        pProp = Load_Source(propPath, "");
        if (!pProp || !pProp->numbones || !pProp->m_Animations.Count())
            MdlError("$deltaproportions: could not load proportionpose '%s'\n", propPath);
        s_sourceanim_t &propAnim = pProp->m_Animations[0];
        if (propFrame >= propAnim.numframes)
            MdlError("$deltaproportions: proportionpose frame %d out of range (has %d)\n",
                     propFrame, propAnim.numframes);
        propBones = propAnim.rawanim[propFrame];
    } else {
        for (int si = 0; si < g_numsources; si++) {
            if (g_source[si] && g_source[si]->isActiveModel &&
                g_source[si]->numbones > 0 && g_source[si]->m_Animations.Count()) {
                pProp     = g_source[si];
                propBones = pProp->m_Animations[0].rawanim[0];
                break;
            }
        }
        if (!pProp)
            MdlError("$deltaproportions: 'proportionpose' omitted but no $body/$model loaded yet\n");
    }

    // roll back any textures/materials the skeleton-only loads registered
    for (int i = 0; i < g_numtextures; i++) {
        if (g_texture[i].material >= savedNumMaterials)
            g_texture[i].material = -1;
    }
    for (int i = savedNumTextures; i < g_numtextures; i++)
        g_texture[i].name[0] = '\0';
    g_numtextures  = savedNumTextures;
    g_nummaterials = savedNumMaterials;

    int nBones = pRef->numbones;

    CUtlDict<int, int> propBoneIdx;
    for (int i = 0; i < pProp->numbones; i++)
        propBoneIdx.Insert(pProp->localBone[i].name, i);

    CUtlVector<DP_AbsTransform> propAbs;
    DP_BuildAbsTransforms(pProp, propBones, propAbs);

    auto getBoneCfg = [&](const char *name) -> const DP_BoneParams * {
        int bi = boneCfg.Find(name);
        return (bi != boneCfg.InvalidIndex()) ? &boneCfg[bi] : nullptr;
    };

    // world rotation from proportion pose; position derived from reference hierarchy
    CUtlVector<DP_AbsTransform> refNew;
    refNew.SetCount(nBones);
    for (int i = 0; i < nBones; i++) {
        int par = pRef->localBone[i].parent;

        Vector absPos;
        if (par < 0) {
            absPos = refBones[i].pos;
        } else {
            Vector rotated;
            QuaternionMultiply(refNew[par].rot, refBones[i].pos, rotated);
            absPos = refNew[par].pos + rotated;
        }

        const DP_BoneParams *bp = getBoneCfg(pRef->localBone[i].name);
        int pi = propBoneIdx.Find(pRef->localBone[i].name);
        bool hasMatch = (pi != propBoneIdx.InvalidIndex()) && !(bp && bp->ignore);

        Quaternion absRot;
        if (hasMatch) {
            if (bp && bp->hasIgnoreAngle) {
                // compute reference world rotation to blend with proportion on masked axes
                Quaternion localRot;
                AngleQuaternion(refBones[i].rot, localRot);
                Quaternion refWorldRot;
                if (par >= 0)
                    QuaternionMult(refNew[par].rot, localRot, refWorldRot);
                else
                    refWorldRot = localRot;
                RadianEuler refE, propE;
                QuaternionAngles(refWorldRot, refE);
                QuaternionAngles(propAbs[pi].rot, propE);
                RadianEuler masked(
                    bp->ignoreAngle.x ? refE.x : propE.x,
                    bp->ignoreAngle.y ? refE.y : propE.y,
                    bp->ignoreAngle.z ? refE.z : propE.z);
                AngleQuaternion(masked, absRot);
            } else {
                absRot = propAbs[pi].rot;
            }
        } else {
            Quaternion localRot;
            AngleQuaternion(refBones[i].rot, localRot);
            if (par >= 0)
                QuaternionMult(refNew[par].rot, localRot, absRot);
            else
                absRot = localRot;
        }
        refNew[i] = { absPos, absRot };
    }

    // world position from proportion pose; rotation unchanged
    CUtlVector<DP_AbsTransform> propNew;
    propNew.SetCount(nBones);
    for (int i = 0; i < nBones; i++) {
        int par = pRef->localBone[i].parent;

        Vector derivedPos;
        if (par < 0) {
            derivedPos = refBones[i].pos;
        } else {
            Vector rotated;
            QuaternionMultiply(propNew[par].rot, refBones[i].pos, rotated);
            derivedPos = propNew[par].pos + rotated;
        }

        const DP_BoneParams *bp = getBoneCfg(pRef->localBone[i].name);
        int pi = propBoneIdx.Find(pRef->localBone[i].name);
        bool hasMatch = (pi != propBoneIdx.InvalidIndex()) && !(bp && bp->ignore);

        Vector absPos;
        if (hasMatch) {
            if (bp && bp->hasIgnorePos) {
                const Vector &pp = propAbs[pi].pos;
                absPos.x = bp->ignorePos.x ? derivedPos.x : pp.x;
                absPos.y = bp->ignorePos.y ? derivedPos.y : pp.y;
                absPos.z = bp->ignorePos.z ? derivedPos.z : pp.z;
            } else {
                absPos = propAbs[pi].pos;
            }
        } else {
            absPos = derivedPos;
        }
        propNew[i] = { absPos, refNew[i].rot };
    }

    // convert world transforms back to local space; apply per-bone offsets to a_proportions
    CUtlVector<Vector>      refLP, propLP;
    CUtlVector<RadianEuler> refLR, propLR;
    refLP.SetCount(nBones);  propLP.SetCount(nBones);
    refLR.SetCount(nBones);  propLR.SetCount(nBones);

    for (int i = 0; i < nBones; i++) {
        int par = pRef->localBone[i].parent;
        const DP_BoneParams *bp = getBoneCfg(pRef->localBone[i].name);

        // ignored bones are excluded from animation data entirely via the skip mask
        if (bp && bp->ignore)
            continue;

        // reference local (no per-bone offsets)
        if (par < 0) {
            refLP[i] = refNew[i].pos;
            QuaternionAngles(refNew[i].rot, refLR[i]);
        } else {
            const DP_AbsTransform &parAbs = refNew[par];
            Quaternion parInv = { -parAbs.rot.x, -parAbs.rot.y, -parAbs.rot.z, parAbs.rot.w };
            Quaternion localRot;
            QuaternionMult(parInv, refNew[i].rot, localRot);
            QuaternionAngles(localRot, refLR[i]);
            Vector diff = refNew[i].pos - parAbs.pos;
            QuaternionMultiply(parInv, diff, refLP[i]);
        }

        // proportions local - keep quaternion so offsets can be applied before euler conversion
        Quaternion propLocalRot;
        if (par < 0) {
            propLP[i]    = propNew[i].pos;
            propLocalRot = propNew[i].rot;
        } else {
            const DP_AbsTransform &parAbs = propNew[par];
            Quaternion parInv = { -parAbs.rot.x, -parAbs.rot.y, -parAbs.rot.z, parAbs.rot.w };
            QuaternionMult(parInv, propNew[i].rot, propLocalRot);
            Vector diff = propNew[i].pos - parAbs.pos;
            QuaternionMultiply(parInv, diff, propLP[i]);
        }

        // apply per-bone offsets to a_proportions (local space)
        if (bp) {
            if (bp->hasOffsetPos) {
                Vector rotated;
                QuaternionMultiply(propLocalRot, bp->offsetPos, rotated);
                propLP[i] += rotated;
            }
            if (bp->hasOffsetAngle) {
                RadianEuler offE(DEG2RAD(bp->offsetAngle.x),
                                 DEG2RAD(bp->offsetAngle.y),
                                 DEG2RAD(bp->offsetAngle.z));
                Quaternion offRot, newRot;
                AngleQuaternion(offE, offRot);
                QuaternionMult(propLocalRot, offRot, newRot);
                propLocalRot = newRot;
            }
        }
        QuaternionAngles(propLocalRot, propLR[i]);
    }

    // build skip mask so ignored bones are absent from both animations
    // which can conflict with $unlockdefinebones
    bool skipBones[MAXSTUDIOSRCBONES] = {};
    for (int i = 0; i < nBones; i++) {
        const DP_BoneParams *bp = getBoneCfg(pRef->localBone[i].name);
        skipBones[i] = bp && bp->ignore;
    }

    DP_RegisterInMemoryAnim("a_reference",   pRef, refLP,  refLR,  skipBones);
    DP_RegisterInMemoryAnim("a_proportions", pRef, propLP, propLR, skipBones);

    DP_ApplyAnimBlock("a_reference",   qcRefBlock);
    DP_ApplyAnimBlock("a_proportions", qcPropBlock);

    Msg("Delta Proportions: registered 'a_reference' and 'a_proportions'\n");
}

//-----------------------------------------------------------------------------
// Prints a message to the console during compile.
//-----------------------------------------------------------------------------
void Cmd_Print() {
    if (TokenAvailable()) {
        GetToken(false);
        Msg("Print: %s\n", token);
    }
}

//-----------------------------------------------------------------------------
// Stops the compile immediately with an optional message.
//-----------------------------------------------------------------------------
void Cmd_Return() {
    if (TokenAvailable()) {
        GetToken(false);
        Msg("$return: %s\n", token);
    } else {
        Msg("$return: stopping compile.\n");
    }
    exit(0);
}

//
// This is the master list of the commands a QC file supports.
// To add a new command to the QC files, add it here.
//
MDLCommand_t g_Commands[] =
        {
                {"$cd",                              Cmd_CD,},
                {"$modelname",                       Cmd_Modelname,},
                {"$internalname",                    Cmd_InternalName,},
                {"$cdmaterials",                     Cmd_CDMaterials,},
                {"$pushd",                           Cmd_Pushd,},
                {"$popd",                            Cmd_Popd,},
                {"$scale",                           Cmd_ScaleUp,},
                {"$root",                            Cmd_Root,},
                {"$controller",                      Cmd_Controller,},
                {"$screenalign",                     Cmd_ScreenAlign,},
                {"$worldalign",                      Cmd_WorldAlign,},
                {"$model",                           Cmd_Model,},
                {"$appendsource",                    Cmd_AppendSource,},
                {"$body",                            Cmd_Body,},
                {"$bodygroup",                       Cmd_Bodygroup,},
                {"$appendblankbodygroup",            Cmd_AppendBlankBodygroup,},
                {"$bodygrouppreset",                 Cmd_BodygroupPreset,},
                {"$animation",                       Cmd_Animation,},
                {"$autocenter",                      Cmd_Autocenter,},
                {"$sequence",                        Cmd_Sequence,},
                {"$append",                          Cmd_Append,},
                {"$prepend",                         Cmd_Prepend,},
                {"$continue",                        Cmd_Continue,},
                {"$declaresequence",                 Cmd_DeclareSequence,},
                {"$declareanimation",                Cmd_DeclareAnimation,},
                {"$cmdlist",                         Cmd_Cmdlist,},
                {"$animblocksize",                   Cmd_AnimBlockSize,},
                {"$weightlist",                      Cmd_Weightlist,},
                {"$defaultweightlist",               Cmd_DefaultWeightlist,},
                {"$ikchain",                         Cmd_IKChain,},
                {"$ikautoplaylock",                  Cmd_IKAutoplayLock,},
                {"$eyeposition",                     Cmd_Eyeposition,},
                {"$illumposition",                   Cmd_Illumposition,},
                {"$origin",                          Cmd_Origin,},
                {"$upaxis",                          Cmd_UpAxis,},
                {"$bbox",                            Cmd_BBox,},
                {"$bboxonlyverts",                   Cmd_BBoxOnlyVerts,},
                {"$cbox",                            Cmd_CBox,},
                {"$gamma",                           Cmd_Gamma,},
                {"$texturegroup",                    Cmd_TextureGroup,},
                {"$hgroup",                          Cmd_Hitgroup,},
                {"$hbox",                            Cmd_Hitbox,},
                {"$hboxset",                         Cmd_HitboxSet,},
                {"$surfaceprop",                     Cmd_SurfaceProp,},
                {"$jointsurfaceprop",                Cmd_JointSurfaceProp,},
                {"$contents",                        Cmd_Contents,},
                {"$jointcontents",                   Cmd_JointContents,},
                {"$attachment",                      Cmd_Attachment,},
                {"$aligneyes",                       Cmd_AlignEyes,},
                {"$alignmouth",                      Cmd_AlignMouth,},
                {"$redefineattachment",              Cmd_RedefineAttachment,},
                {"$bonemerge",                       Cmd_BoneMerge,},
                {"$bonealwayssetup",                 Cmd_BoneAlwaysSetup,},
                {"$externaltextures",                Cmd_ExternalTextures,},
                {"$cliptotextures",                  Cmd_ClipToTextures,},
                {"$skinnedLODs",                     Cmd_SkinnedLODs,},
                {"$renamebone",                      Cmd_Renamebone,},
                {"$stripboneprefix",                 Cmd_StripBonePrefix,},
                {"$renamebonesubstr",                Cmd_RenameBoneSubstr,},
                {"$collapsebones",                   Cmd_CollapseBones,},
                {"$nocollapsebones",                 Cmd_NoCollapseBones,},
                {"$alwayscollapse",                  Cmd_AlwaysCollapse,},
                {"$donotcollapse",                   Cmd_DoNotCollapse,},
                {"$proceduralbones",                 Load_ProceduralBones,},
                {"$skiptransition",                  Cmd_Skiptransition,},
                {"$calctransitions",                 Cmd_CalcTransitions,},
                {"$staticprop",                      Cmd_StaticProp,},
                {"$staticproppose",                  Cmd_StaticPropPose,},
                {"$nosequence",                      Cmd_NoSequence,},
                {"$zbrush",                          Cmd_ZBrush,},
                {"$realignbones",                    Cmd_RealignBones,},
                {"$forcerealign",                    Cmd_ForceRealign,},
                {"$lod",                             Cmd_BaseLOD,},
                {"$shadowlod",                       Cmd_ShadowLOD,},
                {"$poseparameter",                   Cmd_PoseParameter,},
                {"$heirarchy",                       Cmd_ForcedHierarchy,},
                {"$hierarchy",                       Cmd_ForcedHierarchy,},
                {"$insertbone",                      Cmd_InsertHierarchy,},
                {"$limitrotation",                   Cmd_LimitRotation,},
                {"$definebone",                      Cmd_DefineBone,},
                {"$rotatebone",                      Cmd_RotateBone,},
                {"$movebone",                        Cmd_MoveBone,},
                {"$jigglebone",                      Cmd_JiggleBone,},
                {"$includemodel",                    Cmd_IncludeModel,},
                {"$opaque",                          Cmd_Opaque,},
                {"$mostlyopaque",                    Cmd_TranslucentTwoPass,},
                {"$keyvalues",                       Cmd_KeyValues,},
                {"$obsolete",                        Cmd_Obsolete,},
                {"$renamematerial",                  Cmd_RenameMaterial,},
                {"$overridematerial",                Cmd_OverrideMaterial,},
                {"$fakevta",                         Cmd_FakeVTA,},
                {"$noforcedfade",                    Cmd_NoForcedFade,},
                {"$skipboneinbbox",                  Cmd_SkipBoneInBBox,},
                {"$forcephonemecrossfade",           Cmd_ForcePhonemeCrossfade,},
                {"$lockbonelengths",                 Cmd_LockBoneLengths,},
                {"$unlockdefinebones",               Cmd_UnlockDefineBones,},
                {"$constantdirectionallight",        Cmd_ConstDirectionalLight,},
                {"$minlod",                          Cmd_MinLOD,},
                {"$allowrootlods",                   Cmd_AllowRootLODs,},
                {"$bonesaveframe",                   Cmd_BoneSaveFrame,},
                {"$ambientboost",                    Cmd_AmbientBoost,},
                {"$centerbonesonverts",              Cmd_CenterBonesOnVerts,},
                {"$donotcastshadows",                Cmd_DoNotCastShadows,},
                {"$casttextureshadows",              Cmd_CastTextureShadows,},
                {"$motionrollback",                  Cmd_MotionExtractionRollBack,},
                {"$sectionframes",                   Cmd_SectionFrames,},
                {"$clampworldspace",                 Cmd_ClampWorldspace,},
                {"$maxeyedeflection",                Cmd_MaxEyeDeflection,},
                {"$addsearchdir",                    Cmd_AddSearchDir,},
                {"$subd",                            Cmd_SubdivisionSurface,},
                {"$boneflexdriver",                  Cmd_BoneFlexDriver,},
                {"$modelbudget",                     Cmd_ModelBudget,},
                {"$preservetriangleorder",           Cmd_PreserveTriangleOrder,},
                {"$qcassert",                        Cmd_QCAssert,},
                {"$lcaseallsequences",               Cmd_LCaseAllSequences,},
                {"$defaultfadein",                   Cmd_SetDefaultFadeInTime,},
                {"$defaultfadeout",                  Cmd_SetDefaultFadeOutTime,},
                {"$allowactivityname",               Cmd_AllowActivityName,},
                {"$collisionprecision",              Cmd_CollisionPrecision,},
                {"$collisionmodel",                  Cmd_CollisionModel,},
                {"$collisionjoints",                 Cmd_CollisionJoints,},
                {"$erroronsequenceremappingfailure", Cmd_ErrorOnSeqRemapFail,},
                {"$modelhasnosequences",             Cmd_SetModelIntentionallyHasZeroSequences,},
                {"$contentrootrelative",             Cmd_ContentRootRelative,},
                {"$rendermesh",                       Cmd_RenderMesh,},
                {"$driverbone",                       Cmd_DriverBone,},
                {"$driverlookat",                     Cmd_DriverLookAt,},
                {"$elif",                             Cmd_Elif,},
                {"$else",                             Cmd_Else,},
                {"$case",                             Cmd_Case,},
                {"$default",                          Cmd_Default,},
                {"$deltaproportions",                 Cmd_DeltaProportions,},
                {"$return",                           Cmd_Return,},
                {"$print",                            Cmd_Print,},
        };

int g_nMDLCommandCount = ARRAYSIZE(g_Commands);

