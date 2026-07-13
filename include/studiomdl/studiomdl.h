//===== Copyright � 1996-2008, Valve Corporation, All rights reserved. ======//
//
// Purpose: 
//
// $NoKeywords: $
//
//===========================================================================//

#ifndef STUDIOMDL_H
#define STUDIOMDL_H

#ifdef _WIN32
#pragma once
#endif


#include <vector>
#include <deque>
#include <array>
#include <cstdio>
#include "tier0/basetypes.h"
#include "tier1/utlvector.h"
#include "tier1/utlsymbol.h"
#include "tier1/utlstring.h"
#include "mathlib/vector.h"
#include "studio.h"
#include "datamodel/dmelementhandle.h"
#include "worldsize.h"

struct LodScriptData_t;
struct s_flexkey_t;
struct s_flexcontroller_t;
struct s_flexcontrollerremap_t;
struct s_combinationrule_t;
struct s_combinationcontrol_t;
struct s_dmeflexrule_t;

class CDmeVertexDeltaData;

class CDmeCombinationOperator;

#ifdef MDLCOMPILE
#define SRC_FILE_EXT ".mc"
#define MC_CURRENT_VERSION 1
#else
#define SRC_FILE_EXT ".qc"
#define MC_CURRENT_VERSION 0
#endif

#define IDSTUDIOHEADER            (('T'<<24)+('S'<<16)+('D'<<8)+'I')
// little-endian "IDST"
#define IDSTUDIOANIMGROUPHEADER    (('G'<<24)+('A'<<16)+('D'<<8)+'I')
// little-endian "IDAG"


#define STUDIO_QUADRATIC_MOTION 0x00002000

#define MAXSTUDIOANIMFRAMES        5000    // max frames per animation
// [mlowrance] updated total number of animations to give more headroom for new weapons
// bumped up from 2k to 3k
#define MAXSTUDIOANIMS            8192    // total animations
#define MAXSTUDIOSEQUENCES        4096    // total sequences
#define MAXSTUDIOSRCBONES        1024        // bones allowed at source movement
#define MAXSTUDIOTEXCOORDS        8
#define MAXSTUDIOMESHES            256
#define MAXSTUDIOEVENTS            1024
#define MAXSTUDIOFLEXKEYS        (MAXSTUDIOFLEXDESC / 2)    // always half of MAXSTUDIOFLEXDESC
#define MAXSTUDIOFLEXRULES        4096
#define MAXSTUDIOBONECONSTRAINTS  256

//-----------------------------------------------------------------------------
// $modelbudget HARD CAPS (the compile-time ceilings each budget param clamps to)
// These mirror the #defines above/in studio.h. Edit the #define to change a cap;
// $modelbudget only sets the lower runtime *soft* limit (never above the cap).
//
//   budget param      hard cap #define          default soft value
//   ------------      ----------------          ------------------
//   totalverts        MAXSTUDIOVERTS            MAXSTUDIOVERTS
//   bones             MAXSTUDIOBONES (1024)     256
//   materials         MAXSTUDIOSKINS (128)      64
//   flexcontroller    MAXSTUDIOFLEXCTRL (256)   96
//   flexmorph         MAXSTUDIOFLEXDESC (4096)  1024   (also bounds flexkeys = /2)
//   flexmorphverts    MAXSTUDIOFLEXVERTS(65536) 32768
//   flexrules         MAXSTUDIOFLEXRULES (4096) 1024
//   poseparam         MAXSTUDIOPOSEPARAM (64)   24
//   boneconstraints   MAXSTUDIOBONECONSTRAINTS  64
//   sequence          MAXSTUDIOSEQUENCES (4096) 1524
//   animation         MAXSTUDIOANIMS (8192)     3000
//-----------------------------------------------------------------------------
#define MAXSTUDIOBONEWEIGHTS    3
// Source-stage per-vertex influence capacity. Loaders keep up to this many
// weights (culling only <STUDIO_MIN_BONE_WEIGHT noise) so bone collapse can
// merge helper-bone weights into their parents BEFORE the clip to the
// MAXSTUDIOBONEWEIGHTS hardware limit (BalanceGlobalBoneWeights, simplify.cpp).
// Clipping at load time made soon-to-collapse helpers compete with real bones
// for the 3 slots, discarding legitimate influences.
#define MAXSTUDIOSRCBONEWEIGHTS 16
// Minimum significant per-vertex influence: weights below this fraction of the
// normalized total are culled by SortAndBalanceBones (at load and in the final
// post-collapse clip).
#define STUDIO_MIN_BONE_WEIGHT  0.0001f
#define MAXSTUDIOCMDS            128
#define MAXSTUDIOMOVEKEYS        64
#define MAXSTUDIOIKRULES        64
#define MAXSTUDIONAME            128
#define MAXSTUDIOACTIVITYMODIFIERS    128
#define MAXSTUDIOTAGS            1024

#define MAXSTUDIOSRCVERTS        (8*65536*8)

//-----------------------------------------------------------------------------
// PulseMDL tunable feature limits
// (capacities for PulseMDL-added QC features; edit here rather than hunting
//  through the .cpp implementation files)
//-----------------------------------------------------------------------------
// $rendermesh: named filtered views of DMX files (studiomdl_commands.cpp)
#define MAX_RENDERMESH_DEFS             512     // total $rendermesh definitions
#define MAX_RENDERMESH_OVERRIDES        256     // per-def mesh overrides
#define MAX_RENDERMESH_MATERIAL_REMOVES 256     // per-def material removals
#define MAX_RENDERMESH_MATERIAL_WORDS   256     // per-def material substring removals
#define MAX_RENDERMESH_FLEXCTRL_REMOVES 256     // per-def flex controller removals
// Conditional ($if/$switch) stacking limits live in scriplib.h (libs/utils),
// since that parsing lives below the studiomdl layer.

#ifndef EXTERN
#define EXTERN extern
#endif

EXTERN    char g_outname[MAX_PATH];
EXTERN  char g_szInternalName[MAX_PATH];
EXTERN  qboolean cdset;
EXTERN  int numdirs;
EXTERN    char cddir[32][MAX_PATH];
EXTERN  int g_numAddSearchDirs;
EXTERN  char g_addSearchDirs[16][MAX_PATH];
EXTERN    int numcdtextures;
EXTERN    char *cdtextures[16];
EXTERN  char g_fullpath[MAX_PATH];

EXTERN    char rootname[MAXSTUDIONAME];        // name of the root bone
EXTERN    float g_defaultscale;
EXTERN  float g_currentscale;
EXTERN  RadianEuler g_defaultrotation;


EXTERN    char defaulttexture[16][MAX_PATH];
EXTERN    char sourcetexture[16][MAX_PATH];

EXTERN    int numrep;

EXTERN    float normal_blend;
EXTERN    int dump_hboxes;
EXTERN    int ignore_warnings;

EXTERN    Vector eyeposition;
EXTERN    float g_flMaxEyeDeflection;
EXTERN    int g_illumpositionattachment;
EXTERN    Vector illumposition;
EXTERN    int illumpositionset;
EXTERN    int gflags;
EXTERN    Vector bbox[2];
EXTERN    Vector cbox[2];
EXTERN    bool g_wrotebbox;
EXTERN    bool g_wrotecbox;
EXTERN    bool g_bboxonlyverts;

EXTERN    int clip_texcoords;
EXTERN    bool g_staticprop;
EXTERN    bool g_simpleprop;
EXTERN    bool g_centerstaticprop;
EXTERN    bool g_nosequence;
EXTERN    bool g_bLegacyVTX;

EXTERN    bool g_realignbones;
EXTERN    bool g_definebones;

EXTERN  byte g_constdirectionalightdot;

int KeyValueTextSize(std::vector<char> *pKeyValue);

const char *KeyValueText(std::vector<char> *pKeyValue);

extern vec_t Q_rint(vec_t in);

extern void WriteModelFiles(void);

// --------------------------------------------------------------------

template<class T>
class CUtlVectorAuto : public CUtlVector<T> {
    // typedef CUtlVectorAuto< T, CUtlVector<T > > BaseClass;
public:
    T &operator[](int i);
};

template<typename T>
inline T &CUtlVectorAuto<T>::operator[](int i) {
    EnsureCount(i + 1);
    Assert(IsValidIndex(i));
    return Base()[i];
}

//-----------------------------------------------------------------------------
// Assigns a default surface property to the entire model
//-----------------------------------------------------------------------------
struct SurfacePropName_t {
    char m_pJointName[128];
    char m_pSurfaceProp[128];
};

//////////////////////////////////////////////////////////////////////////
// Purpose: contains settings specified in gameinfo.txt
//////////////////////////////////////////////////////////////////////////

struct GameInfo_t {
    bool bSupportsDX8;
};
extern struct GameInfo_t g_gameinfo;

// --------------------------------------------------------------------

struct s_trianglevert_t {
    int vertindex;
    int normindex;        // index into normal array
    int s, t;
    float u, v;
};

struct s_boneweight_t {
    int numbones;
    std::array<int, MAXSTUDIOSRCBONEWEIGHTS> bone;
    std::array<float, MAXSTUDIOSRCBONEWEIGHTS> weight;
};

struct s_tmpface_t {
    int material{};
    uint32_t a, b, c, d;
    uint32_t na, nb, nc, nd;
    std::array<uint32_t, MAXSTUDIOTEXCOORDS> ta{0xFFFFFFFF};
    std::array<uint32_t, MAXSTUDIOTEXCOORDS> tb{0xFFFFFFFF};
    std::array<uint32_t, MAXSTUDIOTEXCOORDS> tc{0xFFFFFFFF};
    std::array<uint32_t, MAXSTUDIOTEXCOORDS> td{0xFFFFFFFF}; // d used by subd quads, otherwise 0xFFFFFFFF

    s_tmpface_t() {
        a = b = c = d = 0xFFFFFFFF;
        na = nb = nc = nd = 0xFFFFFFFF;
        for (int i = 0; i < MAXSTUDIOTEXCOORDS; ++i) { ta[i] = tb[i] = tc[i] = td[i] = 0xFFFFFFFF; }
    }
};

struct s_face_t {
    s_face_t() { a = b = c = d = 0xFFFFFFFF; }

    uint32_t a, b, c, d;        // d used by subd quads
};

struct s_vertexinfo_t {
    int material;
    int mesh;
    Vector position;
    Vector normal;
    Vector4D tangentS;
    int numTexcoord;
    std::array<Vector2D, MAXSTUDIOTEXCOORDS> texcoord;
    s_boneweight_t boneweight;
};


//============================================================================

// dstudiobone_t bone[MAXSTUDIOBONES];
struct s_bonefixup_t {
    matrix3x4_t m;
};

struct s_bonetable_t {
    char name[MAXSTUDIONAME];    // bone name for symbolic links
    int parent;        // parent bone
    bool split;
    int bonecontroller;    // -1 == 0
    Vector pos;        // default pos
    Vector posscale;    // pos values scale
    RadianEuler rot;        // default pos
    Vector rotscale;    // rotation values scale
    int group;        // hitgroup
    Vector bmin, bmax;    // bounding box
    bool bPreDefined;
    matrix3x4_t rawLocalOriginal; // original transform of preDefined bone
    matrix3x4_t rawLocal;
    matrix3x4_t srcRealign;
    bool bPreAligned;
    matrix3x4_t boneToPose;
    int flags;
    int proceduralindex;
    int physicsBoneIndex;
    int surfacePropIndex;
    Quaternion qAlignment;
    bool bDontCollapse;
    bool bIsMeshDag;    // every contributing source flagged this bone as a DmeMesh transform dag
    Vector posrange;
};
EXTERN    std::array<s_bonetable_t, MAXSTUDIOSRCBONES> g_bonetable;

extern int findGlobalBone(const char *name);    // finds a named bone in the global bone table

EXTERN int g_numrenamedbones;
struct s_renamebone_t {
    char from[MAXSTUDIONAME];
    char to[MAXSTUDIONAME];
};
EXTERN std::array<s_renamebone_t, MAXSTUDIOSRCBONES> g_renamedbone;

const char *RenameBone(const char *pName); // returns new name if available, else return pName.

EXTERN char g_szStripBonePrefix[MAXSTUDIOSRCBONES][MAXSTUDIONAME];
EXTERN int g_numStripBonePrefixes;

EXTERN std::array<s_renamebone_t, MAXSTUDIOSRCBONES> g_szRenameBoneSubstr;
EXTERN int g_numRenameBoneSubstr;

EXTERN int g_numimportbones;
struct s_importbone_t {
    char name[MAXSTUDIONAME];
    char parent[MAXSTUDIONAME];
    matrix3x4_t rawLocal;
    bool bPreAligned;
    matrix3x4_t srcRealign;
    bool bUnlocked;
};
EXTERN std::array<s_importbone_t, MAXSTUDIOSRCBONES> g_importbone;


// $transformbindposebone - late bind-pose edits (merged $rotatebone/$movebone).
// Capped to a combined budget.
#define MAX_BONE_TRANSFORM_EDITS 64
enum BoneXformKind { BONEXFORM_ROTATE, BONEXFORM_MOVE };  // per-sub-edit category (angles vs position)
enum BoneXformSpace { BONEXFORM_LOCAL, BONEXFORM_WORLD };
struct s_bonetransformedit_t {
    char name[MAXSTUDIONAME];
    bool hasAngles;                  // 'angles <rx ry rz>' present
    BoneXformSpace angleSpace;       // local (default) or world (worldangles)
    float angles[3];                 // euler degrees
    bool hasPosition;                // 'position <x y z>' present
    BoneXformSpace posSpace;         // local (default) or world (worldposition)
    float pos[3];                    // units
    bool hasMoveWeight;              // 'transformweights <residualbone> [factor]' present
    char residualbone[MAXSTUDIONAME];
    float moveWeightFactor;          // [0,1] ramp smoothness: transition width as a fraction of the old->new span (1 = linear, 0 = hard cut)
    float moveWeightSmoothing;       // [0,1] extra S-curve easing inside the ramp band (0 = pure linear, 1 = full smoothstep)
    bool hasMoveWeightOffset;        // 'offset <x y z>' present (requires transformweights)
    float moveWeightOffset[3];       // model-space offset added to the ramp end point only - the bone itself is not moved further
    bool transformVerts;            // rigidly carry the bone's rigged verts at rest (excludes transformweights)
    bool ignoreAnimation;           // edit does not flow into any $sequence/$animation frame
    bool ignoreHitbox;               // keep this bone's manual hitboxes in place
    int linecount;
};
EXTERN std::array<s_bonetransformedit_t, MAX_BONE_TRANSFORM_EDITS> g_bonetransformedit;
EXTERN int g_numbonetransformedits;
void ApplyBoneTransformEdits();
void ApplyMoveWeightQueue();
bool GetAccumulatedBoneEditDelta(int globalBone, matrix3x4_t &outD);
bool GetBoneHitboxCompensation(int globalBone, matrix3x4_t &outInvDelta);


EXTERN int g_numincludemodels;
struct s_includemodel_t {
    char name[MAXSTUDIONAME];
};
EXTERN s_includemodel_t g_includemodel[128];

struct s_bbox_t {
    char name[MAXSTUDIONAME];        // bone name
    char hitboxname[MAXSTUDIONAME];    // hitbox name
    int bone;
    int group;        // hitgroup
    int model;
    Vector bmin, bmax;    // bounding box
    QAngle angOffsetOrientation;
    float flCapsuleRadius;
};

#define MAXSTUDIOHITBOXSETNAME 64

struct s_hitboxset {
    char hitboxsetname[MAXSTUDIOHITBOXSETNAME];

    int numhitboxes;

    std::array<s_bbox_t, MAXSTUDIOSRCBONES> hitbox;
};

EXTERN int g_numhitgroups;
struct s_hitgroup_t {
    int models;
    int group;
    char name[MAXSTUDIONAME];    // bone name
};
EXTERN std::array<s_hitgroup_t, MAXSTUDIOSRCBONES> g_hitgroup;


struct s_bonecontroller_t {
    char name[MAXSTUDIONAME];
    int bone;
    int type;
    int inputfield;
    float start;
    float end;
};

EXTERN std::array<s_bonecontroller_t, MAXSTUDIOSRCBONES> g_bonecontroller;
EXTERN int g_numbonecontrollers;

struct s_screenalignedbone_t {
    char name[MAXSTUDIONAME];
    int flags;
};

EXTERN std::array<s_screenalignedbone_t, MAXSTUDIOSRCBONES> g_screenalignedbone;
EXTERN int g_numscreenalignedbones;

struct s_worldalignedbone_t {
    char name[MAXSTUDIONAME];
    int flags;
};

EXTERN std::array<s_worldalignedbone_t, MAXSTUDIOSRCBONES> g_worldalignedbone;
EXTERN int g_numworldalignedbones;

struct s_attachment_t {
    char name[MAXSTUDIONAME];
    char bonename[MAXSTUDIONAME];
    int bone;
    int type;
    int flags;
    matrix3x4_t local;

    bool operator==(const s_attachment_t &rhs) const;
};


#define IS_ABSOLUTE        0x0001
#define IS_RIGID        0x0002
// compile-only marker (never written to the .mdl): attachment came from a source DMX/SMD
// rather than an explicit QC command. $staticproppose strips these on skeleton collapse.
#define IS_FROM_SOURCE     0x0004
// compile-only marker (never written to the .mdl): slot reserved by $declareattachment to
// pin its index order. Anchored slots keep their declared position and are exempt from the
// source-attachment reorder even when later filled by a DMX/SMD attachment.
#define IS_DECLARED        0x0008

EXTERN std::array<s_attachment_t, MAXSTUDIOSRCBONES> g_attachment;
EXTERN int g_numattachments;

// $attachmentbyverts: deferred request that auto-generates an attachment from a union
// of vertex selectors. Origin = centroid of all collected verts + offset; forward =
// literal world-space 'forward', or the averaged vertex normals when 'forward' is
// 0,0,0. Selectors (any combination, OR'd together):
//   morph <flex morph>        - verts deformed by a named flex (by flexdesc)
//   flexgroup <group/type>    - verts deformed by flexes driven by a flexcontroller group
//   materials <material>      - verts assigned to a named material
//   boneweight <bone> <min>   - verts whose weight to <bone> is >= min
// Parent bone = explicit 'bone <name>' if given, else the bone with the highest
// accumulated weight across collected verts. Stored absolute; resolved by
// GenerateAttachmentByVertsAttachments (simplify.cpp).
#define MAX_ATTACHMENTBYVERTS 16
#define MAX_ATTACHBV_TARGETS 16
struct s_attachmentbyverts_t {
    char name[MAXSTUDIONAME];                                  // attachment name
    char morphs[MAX_ATTACHBV_TARGETS][MAXSTUDIONAME];          // flex morph names
    int  nummorphs;
    char flexgroups[MAX_ATTACHBV_TARGETS][MAXSTUDIONAME];      // flexgroup (type) names
    int  numflexgroups;
    char materials[MAX_ATTACHBV_TARGETS][MAXSTUDIONAME];       // material names
    int  nummaterials;
    char bwBone[MAX_ATTACHBV_TARGETS][MAXSTUDIONAME];          // boneweight selector bones
    float bwMin[MAX_ATTACHBV_TARGETS];                         // boneweight selector minimums
    int  numboneweights;
    char bone[MAXSTUDIONAME];   // explicit parent bone override; "" = auto by weight
    Vector offset;     // world-aligned, added to the centroid (units); default 0,0,0
    Vector forward;    // literal world-space forward; 0,0,0 = use averaged normals
    int attachIndex;   // slot reserved in g_attachment[] at parse time (preserves order)
    int linecount;
};
EXTERN std::array<s_attachmentbyverts_t, MAX_ATTACHMENTBYVERTS> g_attachmentbyverts;
EXTERN int g_numattachmentbyverts;
void GenerateAttachmentByVertsAttachments();

struct s_bonemerge_t {
    char bonename[MAXSTUDIONAME];
};

EXTERN std::vector<s_bonemerge_t> g_BoneMerge;

struct s_alwayssetup_t {
    char bonename[MAXSTUDIONAME];
};

EXTERN std::vector<s_alwayssetup_t> g_BoneAlwaysSetup;

struct s_mouth_t {
    char bonename[MAXSTUDIONAME];
    int bone;
    Vector forward;
    int flexdesc;
};

EXTERN std::array<s_mouth_t, MAXSTUDIOSRCBONES> g_mouth; // ?? skins?
EXTERN int g_nummouths;

struct s_node_t {
    char name[MAXSTUDIONAME];
    int parent;
    // dag carries a DmeMesh shape (mesh transform node, not a real joint);
    // exempt from the $nocollapsebones force-keep so it still culls/collapses
    bool bIsMeshDag = false;
};

struct s_bone_t {
    Vector pos;
    RadianEuler rot;
};

struct s_linearmove_t {
    int endframe;    // frame when pos, rot is valid.
    int flags;        // type of motion.  Only linear, linear accel, and linear decel is allowed
    float v0;
    float v1;
    Vector vector;        // movement vector
    Vector pos;    // final position
    RadianEuler rot;        // final rotation
};


#define CMD_WEIGHTS    1
#define CMD_SUBTRACT 2
#define CMD_AO        3
#define CMD_MATCH    4
#define CMD_FIXUP    5
#define CMD_ANGLE    6
#define CMD_IKFIXUP    7
#define CMD_IKRULE    8
#define CMD_MOTION    9
#define CMD_REFMOTION    10
#define CMD_DERIVATIVE 11
#define    CMD_NOANIMATION 12
#define CMD_LINEARDELTA 13
#define CMD_SPLINEDELTA 14
#define CMD_COMPRESS 15
#define CMD_NUMFRAMES 16
#define CMD_COUNTERROTATE 17
#define CMD_SETBONE 18
#define CMD_WORLDSPACEBLEND 19
#define CMD_MATCHBLEND 20
#define CMD_LOCALHIERARCHY 21
#define CMD_FORCEBONEPOSROT 22
#define CMD_REVERSE 23
#define CMD_APPENDANIM 24
#define CMD_BONEDRIVER 25
#define CMD_NOANIM_KEEPDURATION 26

struct s_animation_t;
struct s_ikrule_t;


struct s_motion_t {
    int motiontype;
    int iStartFrame;// starting frame to apply motion over
    int iEndFrame;    // end frame to apply motion over
    int iSrcFrame;    // frame that matches the "reference" animation
    s_animation_t *pRefAnim;    // animation to match
    int iRefFrame;    // reference animation's frame to match
};


struct s_animcmd_t {
    int cmd;
    union {
        struct {
            int index;
        } weightlist;

        struct {
            s_animation_t *ref;
            int frame;
            int flags;
        } subtract;

        struct {
            s_animation_t *ref;
            int motiontype;
            int srcframe;
            int destframe;
            char *pBonename;
        } ao;

        struct {
            s_animation_t *ref;
            int srcframe;
            int destframe;
            int destpre;
            int destpost;
        } match;

        struct {
            s_animation_t *ref;
            int startframe;
            int loops;
        } world;

        struct {
            int start;
            int end;
        } fixuploop;

        struct {
            float angle;
        } angle;

        struct {
            s_ikrule_t *pRule;
        } ikfixup;

        struct {
            s_ikrule_t *pRule;
        } ikrule;

        struct {
            float scale;
        } derivative;

        struct {
            int flags;
        } linear;

        struct {
            int frames;
        } compress;

        struct {
            int frames;
        } numframes;

        struct {
            char *pBonename;
            bool bHasTarget;
            float targetAngle[3];
        } counterrotate;

        struct {
            char *pBonename;
            char *pParentname;
            int start;
            int peak;
            int tail;
            int end;
        } localhierarchy;

        struct {
            char *pBonename;
            bool bDoPos;
            float pos[3];
            bool bDoRot;
            float rot[3];
            bool bRotIsLocal;
        } forceboneposrot;

        struct {
            char *pBonename;
            int iAxis;
            float value;
            int start;
            int peak;
            int tail;
            int end;
            bool all;
        } bonedriver;

        struct {
            s_animation_t *ref;
        } appendanim;

        struct s_motion_t motion;
    } u;
};

struct s_streamdata_t {
    Vector pos;
    Quaternion q;
};


struct s_animationstream_t {
    // source animations
    int numerror;
    s_streamdata_t *pError;
    // compressed animations
    float scale[6];
    int numanim[6];
    mstudioanimvalue_t *anim[6];
};

struct s_ikrule_t {
    int chain;

    int index;
    int type;
    int slot;
    char bonename[MAXSTUDIONAME];
    char attachment[MAXSTUDIONAME];
    int bone;
    Vector pos;
    Quaternion q;
    float height;
    float floor;
    float radius;

    int start;
    int peak;
    int tail;
    int end;

    int contact;

    bool usesequence;
    bool usesource;

    int flags;

    s_animationstream_t errorData;
};

struct s_localhierarchy_t {
    int bone;
    int newparent;

    int start;
    int peak;
    int tail;
    int end;

    s_animationstream_t localData;
};


struct s_source_t;
EXTERN    int g_numani;
struct s_compressed_t {
    int num[6];
    mstudioanimvalue_t *data[6];
};

struct s_animation_t {
    bool isImplied;
    bool isOverride;
    bool doesOverride;
    bool nocull;
    bool ignorescale;
    bool ignoreTransformPosition;  // ignoretransformbindpose position: roll back $transformbindposebone position edits for this animation
    bool ignoreTransformAngles;    // ignoretransformbindpose angles: roll back $transformbindposebone angle edits for this animation
    int index;
    char name[MAXSTUDIONAME];
    char filename[MAX_PATH];

    /*
    int				animsubindex;

    // For sharing outside of current .mdl file
    bool			shared_group_checkvalidity;
    bool			shared_group_valid;
    char			shared_animgroup_file[ MAX_PATH ]; // share file name
    char			shared_animgroup_name[ MAXSTUDIONAME ]; // group name in share file
    int				shared_group_subindex;
    studioanimhdr_t *shared_group_header;
    */

    float fps;
    int startframe;
    int endframe;
    int flags;
    // animations processed (time shifted, linearized, and bone adjusted ) from source animations
    CUtlVectorAuto<s_bone_t *> sanim; // [MAXSTUDIOANIMFRAMES]; // [frame][bones];

    int motiontype;

    int fudgeloop;
    int looprestart; // new starting frame for looping animations
    float looprestartpercent;

    // piecewise linear motion
    int numpiecewisekeys;
    s_linearmove_t piecewisemove[MAXSTUDIOMOVEKEYS];

    // default adjustments
    Vector adjust;
    float scale; // ????
    RadianEuler rotation;

    s_source_t *source;
    char animationname[MAX_PATH];

    Vector bmin;
    Vector bmax;

    int numframes;

    // compressed animation data
    int numsections;
    int sectionframes;
    CUtlVectorAuto<CUtlVectorAuto<s_compressed_t> > anim;

    // int				weightlist;
    float weight[MAXSTUDIOSRCBONES];
    float posweight[MAXSTUDIOSRCBONES];

    int numcmds;
    s_animcmd_t cmds[MAXSTUDIOCMDS];

    int numikrules;
    s_ikrule_t ikrule[MAXSTUDIOIKRULES];
    bool noAutoIK;

    int numlocalhierarchy;
    s_localhierarchy_t localhierarchy[MAXSTUDIOIKRULES];

    float motionrollback;

    bool disableAnimblocks;        // no demand loading
    bool isFirstSectionLocal;    // first block of a section isn't demand loaded
    int numNostallFrames;        // number of frames to keep in memory (modulo segement size)

    int rootDriverIndex;
};
EXTERN    s_animation_t *g_panimation[MAXSTUDIOANIMS];


EXTERN  int g_numcmdlists;
struct s_cmdlist_t {
    char name[MAXSTUDIONAME];
    int numcmds;
    s_animcmd_t cmds[MAXSTUDIOCMDS];
};
EXTERN    s_cmdlist_t g_cmdlist[MAXSTUDIOANIMS];


struct s_iklock_t {
    char name[MAXSTUDIONAME];
    int chain;
    float flPosWeight;
    float flLocalQWeight;
};

EXTERN    int g_numikautoplaylocks;
EXTERN    s_iklock_t g_ikautoplaylock[16];

struct s_animtag_t {
    int tag;
    float cycle;
    char tagname[MAXSTUDIONAME];
};

struct s_event_t {
    int event;
    int frame;
    char options[64];
    char eventname[MAXSTUDIONAME];
};

struct s_autolayer_t {
    char name[MAXSTUDIONAME];
    int sequence;
    int flags;
    int pose;
    float start;
    float peak;
    float tail;
    float end;
};

struct s_activitymodifier_t {
    int id;
    char name[64];
};

class s_sequence_t {
public:
    char name[MAXSTUDIONAME];
    char activityname[MAXSTUDIONAME];    // index into the string table, the name of this activity.

    int flags;
    // float			fps;
    // int				numframes;

    int activity;
    int actweight;

    int numanimtags;
    s_animtag_t animtags[MAXSTUDIOTAGS];

    int numevents;
    s_event_t event[MAXSTUDIOEVENTS];

    int numblends;
    int groupsize[2];
    CUtlVectorAuto<CUtlVectorAuto<s_animation_t *> > panim; // [MAXSTUDIOBLENDS][MAXSTUDIOBLENDS];

    int paramindex[2];
    float paramstart[2];
    float paramend[2];
    int paramattachment[2];
    int paramcontrol[2];
    CUtlVectorAuto<float> param0; // [MAXSTUDIOBLENDS];
    CUtlVectorAuto<float> param1; // [MAXSTUDIOBLENDS];
    s_animation_t *paramanim;
    s_animation_t *paramcompanim;
    s_animation_t *paramcenter;

    // Vector			automovepos[MAXSTUDIOANIMATIONS];
    // Vector			automoveangle[MAXSTUDIOANIMATIONS];

    int animindex;

    Vector bmin;
    Vector bmax;

    float fadeintime;
    float fadeouttime;

    int entrynode;
    int exitnode;
    int nodeflags;
    float entryphase;
    float exitphase;

    int numikrules;

    int numautolayers;
    s_autolayer_t autolayer[64];

    float weight[MAXSTUDIOSRCBONES];

    s_iklock_t iklock[64];
    int numiklocks;

    int cycleposeindex;

    std::vector<char> KeyValue;

    int numactivitymodifiers;
    s_activitymodifier_t activitymodifier[MAXSTUDIOACTIVITYMODIFIERS];

    int rootDriverIndex;
    char rootDriverBoneName[MAXSTUDIONAME];
};

EXTERN    CUtlVector<s_sequence_t> g_sequence;
//EXTERN	int g_numseq;


EXTERN int g_numanimblocks;
struct s_animblock_t {
    int iStartAnim;
    int iEndAnim;
    byte *start;
    byte *end;
};
EXTERN s_animblock_t g_animblock[MAXSTUDIOANIMBLOCKS];
EXTERN int g_animblocksize;
EXTERN char g_animblockname[260];
EXTERN int g_animblockmaxframes;


EXTERN int g_numposeparameters;
struct s_poseparameter_t {
    char name[MAXSTUDIONAME];
    float min;
    float max;
    int flags;
    float loop;
};
EXTERN s_poseparameter_t g_pose[32]; // FIXME: this shouldn't be hard coded


EXTERN int g_numxnodes;
EXTERN char *g_xnodename[100];
EXTERN int g_xnode[100][100];
EXTERN int g_numxnodeskips;
EXTERN int g_xnodeskip[10000][2];

struct rgb_t {
    byte r, g, b;
};
struct rgb2_t {
    float r, g, b, a;
};

// FIXME: what about texture overrides inline with loading models
enum TextureFlags_t {
    RELATIVE_TEXTURE_PATH_SPECIFIED = 0x1
};

struct s_texture_t {
    char name[MAX_PATH];
    int flags;
    int parent;
    int material;
    float width;
    float height;
    float dPdu;
    float dPdv;
};
EXTERN    s_texture_t g_texture[MAXSTUDIOSKINS];
EXTERN    int g_numtextures;
EXTERN    int g_material[MAXSTUDIOSKINS]; // link into texture array
EXTERN  int g_nummaterials;

EXTERN  float g_gamma;
EXTERN    int g_numskinref;
EXTERN  int g_numskinfamilies;
EXTERN  int g_skinref[256][MAXSTUDIOSKINS]; // [skin][skinref], returns texture index
EXTERN    int g_numtexturegroups;
EXTERN    int g_numtexturelayers[32];
EXTERN    int g_numtexturereps[32];
EXTERN  int g_texturegroup[32][32][32];

struct s_mesh_t {
    int numvertices;
    int vertexoffset;

    int numfaces;
    int faceoffset;
};


struct s_vertanim_t {
    int vertex;
    float speed;
    float side;
    Vector pos;
    Vector normal;
    float wrinkle;
};

struct s_lodvertexinfo_t : public s_vertexinfo_t {
    int lodFlag;
};

// processed aggregate lod pools
struct s_loddata_t {
    int numvertices;
    s_lodvertexinfo_t *vertex;

    int numfaces;
    s_face_t *face;

    s_mesh_t mesh[MAXSTUDIOSKINS];

    // remaps verts from an lod's source mesh to this all-lod processed aggregate pool
    int *pMeshVertIndexMaps[MAX_NUM_LODS];
};

// Animations stored in raw off-disk source files.  Raw data should be not processed.
class s_sourceanim_t {
public:
    char animationname[MAX_PATH];
    int numframes;
    int startframe;
    int endframe;
    CUtlVectorAuto<s_bone_t *> rawanim;

    // vertex animation
    bool newStyleVertexAnimations;    // new style doesn't store a base pose in vertex anim[0]
    int *vanim_mapcount;    // local verts map to N target verts
    int **vanim_map;        // local vertices to target vertices mapping list
    int *vanim_flag;        // local vert does animate

    int numvanims[MAXSTUDIOANIMFRAMES];
    s_vertanim_t *vanim[MAXSTUDIOANIMFRAMES];    // [frame][vertex]
};

// raw off-disk source files.  Raw data should be not processed.
struct s_source_t {
    char filename[MAX_PATH];
    int version; // Version number from SMD file, otherwise 0
    bool isActiveModel;

    // local skeleton hierarchy
    int numbones;
    std::array<s_node_t, MAXSTUDIOSRCBONES> localBone;
    std::array<matrix3x4_t, MAXSTUDIOSRCBONES> boneToPose;    // converts bone local data into initial pose data

    // bone remapping
    int boneflags[MAXSTUDIOSRCBONES];    // attachment, vertex, etc flags for this bone
    int boneref[MAXSTUDIOSRCBONES];        // flags for this and child bones
    int boneLocalToGlobal[MAXSTUDIOSRCBONES]; // bonemap : local bone to world bone mapping
    int boneGlobalToLocal[MAXSTUDIOSRCBONES]; // boneimap : world bone to local bone mapping

    int texmap[MAXSTUDIOSKINS * 4];        // map local MAX materials to unique textures

    // per material mesh
    int nummeshes;
    int meshindex[MAXSTUDIOSKINS];    // mesh to skin index
    s_mesh_t mesh[MAXSTUDIOSKINS];

    // vertices defined in "local" space (not remapped to global bones)
    int numvertices;
    s_vertexinfo_t *vertex;

    // vertices defined in "global" space (remapped to global bones)
    CUtlVector<s_vertexinfo_t> m_GlobalVertices;

    int numfaces;
    s_face_t *face;                        // vertex indexs per face

    // raw skeletal animation
    CUtlVector<s_sourceanim_t> m_Animations;

    // default adjustments
    Vector adjust;
    float scale; // ????
    RadianEuler rotation;


    // Flex keys stored in the source data
    bool bNoAutoDMXRules;
    CUtlVector<s_flexkey_t> m_FlexKeys;

    // Combination controls stored in the source data
    CUtlVector<s_combinationcontrol_t> m_CombinationControls;

    // Combination rules stored in the source data
    CUtlVector<s_combinationrule_t> m_CombinationRules;

    // DMX flex rules (expression / passthrough / localvar) captured from the source.
    // Only populated for $rendermesh raw loads, where global flex registration is deferred
    // to the per-source path so the (possibly filtered) clone re-emits exactly what it keeps.
    // Replayed by AddBodyFlexRules; empty for normal loads (those register rules directly).
    CUtlVector<s_dmeflexrule_t> m_DmeFlexRules;

    // Flexcontroller remaps
    CUtlVector<s_flexcontrollerremap_t> m_FlexControllerRemaps;

    // Attachment points stored in the SMD/DMX/etc. file
    CUtlVector<s_attachment_t> m_Attachments;

    // Information about how flex controller remaps map into flex rules
    int m_nKeyStartIndex;    // The index at which the flex keys for this model start in the global list
    CUtlVector<int> m_rawIndexToRemapSourceIndex;
    CUtlVector<int> m_rawIndexToRemapLocalIndex;
    CUtlVector<int> m_leftRemapIndexToGlobalFlexControllIndex;
    CUtlVector<int> m_rightRemapIndexToGlobalFlexControllIndex;

    // DmeMesh tracking (DMX sources only): maps face[i] -> index into m_DmeMeshNames, or -1
    CUtlVector<int16_t> m_FaceDmeMeshIdx;
    CUtlVector<CUtlString> m_DmeMeshNames;
};


EXTERN int g_numsources;
EXTERN s_source_t *g_source[MAXSTUDIOSEQUENCES];

EXTERN s_source_t *g_pStaticPropPoseSource;
EXTERN int g_nStaticPropPoseFrame;

// true only while Load_Source() is loading a $staticproppose pose file; suppresses
// non-geometry side effects (jigglebones, constraints) that would otherwise leak into the model
EXTERN bool g_bLoadingStaticPropPose;

// true only while loading the underlying source of a $rendermesh. Per-source flex data
// (m_FlexKeys, m_CombinationControls/Rules, m_FlexControllerRemaps, m_DmeFlexRules) is still
// built, but the *global* flex registration (g_flexcontroller/g_flexdesc/g_flexrule) done by
// AddCombination is skipped. Each $rendermesh clone instead registers its own (filtered) flex
// through PostProcessSource, so a nofacial / mesh-filtered clone contributes exactly the flex
// it keeps and an unfiltered facial clone contributes the full set - the unfiltered raw never
// leaks.
//
// DMX flex rules (CDmeFlexRuleExpression / passthrough / localvar) are captured verbatim into
// m_DmeFlexRules during this raw load and replayed by AddBodyFlexRules, so a $rendermesh clone
// is a faithful copy of all the DMX's flex data (nofacial drops them again like everything else).
EXTERN bool g_bLoadingRenderMeshRaw;

// true only while loading the underlying source of a $rendermesh that requested
// "nojigglebones" / "nohitbox" / "noproceduralbones". They suppress the DMX jigglebone /
// hitbox / procedural-bone parsing so that render-mesh clone contributes none of the
// source's jigglebones / hitboxes / procedural (driverbone/driverlookat) bones.
EXTERN bool g_bRenderMeshSuppressJiggleBones;
EXTERN bool g_bRenderMeshSuppressHitboxes;
EXTERN bool g_bRenderMeshSuppressProceduralBones;

struct s_staticPropPoseFlexOverride_t {
    char name[MAXSTUDIONAME];
    float value;
};
EXTERN CUtlVector<s_staticPropPoseFlexOverride_t> g_staticPropPoseFlexOverrides;

struct s_eyeball_t {
    char name[MAXSTUDIONAME];
    int index;
    int bone;
    Vector org;
    float zoffset;
    float radius;
    Vector up;
    Vector forward;

    int mesh;
    float iris_scale;

    int upperlidflexdesc;
    int upperflexdesc[3];
    float uppertarget[3];

    int lowerlidflexdesc;
    int lowerflexdesc[3];
    float lowertarget[3];
};

struct s_model_t {
    char name[MAXSTUDIONAME];
    char filename[MAX_PATH];
    // set when filename was resolved from a $rendermesh alias; used as the LOD lookup key
    char rendermesh_name[MAX_PATH];

    // needs local scaling and rotation paramaters
    s_source_t *source; // index into source table

    float scale;    // UNUSED

    float boundingradius;

    Vector boundingbox[MAXSTUDIOSRCBONES][2];

    int numattachments;
    s_attachment_t attachment[32];

    int numeyeballs;
    s_eyeball_t eyeball[4];

    // References to sources which are the LODs for this model
    CUtlVector<s_source_t *> m_LodSources;

    // processed aggregate lod data
    s_loddata_t *m_pLodData;
};

EXTERN    int g_nummodels;
EXTERN    int g_nummodelsbeforeLOD;
EXTERN    CUtlVectorAuto<s_model_t *> g_model;


struct s_flexdesc_t {
    char FACS[MAXSTUDIONAME];    // FACS identifier
};
EXTERN int g_numflexdesc;
EXTERN s_flexdesc_t g_flexdesc[MAXSTUDIOFLEXDESC];

int Add_Flexdesc(const char *name);


struct s_flexcontroller_t {
    char name[MAXSTUDIONAME];
    char type[MAXSTUDIONAME];
    float min;
    float max;
};
EXTERN int g_numflexcontrollers;
EXTERN s_flexcontroller_t g_flexcontroller[MAXSTUDIOFLEXCTRL];

struct s_flexcontrollerremap_t {
    CUtlString m_Name;
    CUtlString m_FlexGroup;             // flex controller type/group; empty means use m_Name
    FlexControllerRemapType_t m_RemapType;
    bool m_bIsStereo;
    // DMX-supplied flex controller range (flexMin/flexMax). m_bHasMinMax is true only when the
    // source explicitly specified them; used by AddFlexControllers so a $rendermesh clone keeps
    // the DMX's range instead of the synthesized 0..1 / -1..1 default. See BuildCombinationSourceData.
    bool m_bHasMinMax = false;
    float m_flMin = 0.0f;
    float m_flMax = 1.0f;
    std::vector<CUtlString> m_RawControls;
    int m_Index;        ///< The model relative index of the slider control for value for this if it's not split, -1 otherwise
    int m_LeftIndex;    ///< The model relative index of the left slider control for this if it's split, -1 otherwise
    int m_RightIndex;    ///< The model relative index of the right slider control for this if it's split, -1 otherwise
    int m_MultiIndex;    ///< The model relative index of the value slider control for this if it's multi, -1 otherwise
    CUtlString m_EyesUpDownFlexName;    // The name of the eyes up/down flex controller
    int m_EyesUpDownFlexController;        // The global index of the Eyes Up/Down Flex Controller
    int m_BlinkController;                // The global index of the Blink Up/Down Flex Controller
};

struct s_flexkey_t {
    int flexdesc;
    int flexpair;

    s_source_t *source; // index into source table
    char animationname[MAX_PATH];

    int imodel;
    int frame;

    float target0;
    float target1;
    float target2;
    float target3;

    float split;
    float decay;

    // extracted and remapped vertex animations
    int numvanims;
    s_vertanim_t *vanim;
    int vanimtype;
    int weighttable;
};
EXTERN int g_numflexkeys;
EXTERN std::array<s_flexkey_t, MAXSTUDIOFLEXKEYS> g_flexkey;
EXTERN s_flexkey_t *g_defaultflexkey;

#define MAX_OPS 512

struct s_flexop_t {
    int op;
    union {
        int index;
        float value;
    } d;
};

struct s_flexrule_t {
    int flex;
    int numops;
    s_flexop_t op[MAX_OPS];
};

EXTERN int g_numflexrules;
EXTERN s_flexrule_t g_flexrule[MAXSTUDIOFLEXRULES];

struct s_combinationcontrol_t {
    char name[MAX_PATH];
};

struct s_combinationrule_t {
    // The 'ints' here are indices into the m_Controls array
    CUtlVector<int> m_Combination;
    CUtlVector<CUtlVector<int> > m_Dominators;

    // The index into the flexkeys to put the result in
    // (should affect both left + right if the key is sided)
    int m_nFlex;
};

// A single DMX flex rule captured from a CDmeFlexRules block, prepared for replay through
// the QC flex-rule parser (Option_Flexrule). See s_source_t::m_DmeFlexRules.
struct s_dmeflexrule_t {
    CUtlString m_Name;      // rule (flexdesc) name
    CUtlString m_Script;    // "= <expr>" memory script, $var$ tokens already expanded
    bool m_bEmit;           // true: emit via Option_Flexrule; false: localvar (reserve flexdesc only)
};

EXTERN    Vector g_defaultadjust;

struct s_bodypart_t {
    char name[MAXSTUDIONAME];
    int nummodels;
    int base;
    CUtlVectorAuto<s_model_t *> pmodel;

    s_bodypart_t() {
        memset(this, 0, sizeof(s_bodypart_t));
    }
};


EXTERN    int g_numbodyparts;
EXTERN    CUtlVectorAuto<s_bodypart_t> g_bodypart;

struct s_bodygrouppreset_t {
    char name[MAXSTUDIONAME];
    int iValue;
    int iMask;

    s_bodygrouppreset_t() {
        memset(this, 0, sizeof(s_bodygrouppreset_t));
    }
};

EXTERN int g_numbodygrouppresets;
EXTERN CUtlVectorAuto<s_bodygrouppreset_t> g_bodygrouppresets;

#define MAXWEIGHTLISTS    128
#define MAXWEIGHTSPERLIST    (MAXSTUDIOBONES)

struct s_weightlist_t {
    // weights, indexed by numbones per weightlist
    char name[MAXSTUDIONAME];
    int numbones;
    char *bonename[MAXWEIGHTSPERLIST];
    float boneweight[MAXWEIGHTSPERLIST];
    float boneposweight[MAXWEIGHTSPERLIST];

    // weights, indexed by global bone index
    float weight[MAXSTUDIOBONES];
    float posweight[MAXSTUDIOBONES];
};

EXTERN    int g_numweightlist;
EXTERN    s_weightlist_t g_weightlist[MAXWEIGHTLISTS];

struct s_iklink_t {
    int bone;
    Vector kneeDir;
};

struct s_ikchain_t {
    char name[MAXSTUDIONAME];
    char bonename[MAXSTUDIONAME];
    int axis;
    float value;
    int numlinks;
    s_iklink_t link[10]; // hip, knee, ankle, toes...
    float height;
    float radius;
    float floor;
    Vector center;
};

EXTERN    int g_numikchains;
EXTERN    s_ikchain_t g_ikchain[16];


struct s_jigglebone_t {
    int flags;
    char bonename[MAXSTUDIONAME];
    int bone;

    mstudiojigglebone_t data;    // the actual jiggle properties
};

EXTERN int g_numjigglebones;
EXTERN s_jigglebone_t g_jigglebones[MAXSTUDIOBONES];
EXTERN int g_jigglebonemap[MAXSTUDIOBONES]; // map used jigglebone's to source jigglebonebone's


struct s_axisinterpbone_t {
    int flags;
    char bonename[MAXSTUDIONAME];
    int bone;
    char controlname[MAXSTUDIONAME];
    int control;
    int axis;
    Vector pos[6];
    Quaternion quat[6];
};

EXTERN int g_numaxisinterpbones;
EXTERN s_axisinterpbone_t g_axisinterpbones[MAXSTUDIOBONES];
EXTERN int g_axisinterpbonemap[MAXSTUDIOBONES]; // map used axisinterpbone's to source axisinterpbone's

struct s_quatinterpbone_t {
    int flags;
    char bonename[MAXSTUDIONAME];
    int bone;
    char parentname[MAXSTUDIONAME];
    // int				parent;
    char controlparentname[MAXSTUDIONAME];
    // int				controlparent;
    char controlname[MAXSTUDIONAME];
    int control;
    int numtriggers;
    Vector size;
    Vector basepos;
    float percentage;
    float tolerance[32];
    Quaternion trigger[32];
    Vector pos[32];
    Quaternion quat[32];
    // If true, pos[]/quat[] hold deltas relative to the triggerpose bind pose.
    // simplify.cpp remaps them onto the actual skeleton bind pose from g_bonetable.
    bool unlockbones;
    // QC $driverbone: match the exact skeleton bone name (no XSI dot-suffix strip).
    // .vrd / DMX leave this false, keeping the "Bip01_L_Hand" == "ValveBiped.Bip01_L_Hand" match.
    bool strictname;
};

EXTERN int g_numquatinterpbones;
EXTERN s_quatinterpbone_t g_quatinterpbones[MAXSTUDIOBONES];
EXTERN int g_quatinterpbonemap[MAXSTUDIOBONES]; // map used quatinterpbone's to source axisinterpbone's


struct s_aimatbone_t {
    char bonename[MAXSTUDIONAME];
    int bone;
    char parentname[MAXSTUDIONAME];
    int parent;
    char aimname[MAXSTUDIONAME];
    int aimAttach;
    int aimBone;
    Vector aimvector;
    Vector upvector;
    Vector basepos;
    bool autobasepos; // true: seed basepos from skeleton rest pose in TagProceduralBones
    // QC $driverlookat: match the exact skeleton bone name (no XSI dot-suffix strip).
    bool strictname;
};

EXTERN int g_numaimatbones;
EXTERN s_aimatbone_t g_aimatbones[MAXSTUDIOBONES];
EXTERN int g_aimatbonemap[MAXSTUDIOBONES]; // map used aimatpbone's to source aimatpbone's (may be optimized out)


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
struct s_constraintbonetarget_t {
    char m_szBoneName[MAXSTUDIONAME];
    int m_nBone;
    float m_flWeight;
    Vector m_vOffset;
    Quaternion m_qOffset;

    bool operator==(const s_constraintbonetarget_t &rhs) const;

    bool operator!=(const s_constraintbonetarget_t &rhs) const { return !(*this == rhs); }
};


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
struct s_constraintboneslave_t {
    char m_szBoneName[MAXSTUDIONAME];
    int m_nBone;
    Vector m_vBaseTranslate;
    Quaternion m_qBaseRotation;

    bool operator==(const s_constraintboneslave_t &rhs) const;

    bool operator!=(const s_constraintboneslave_t &rhs) const { return !(*this == rhs); }
};

//-----------------------------------------------------------------------------
// Accumulates faces/verts/animations from one or more sources into a single
// source (used by AddSrcToSrc when merging static-prop bodies).
//-----------------------------------------------------------------------------
class CClampedSource {
public:
    CClampedSource() : m_nummeshes(0) {};

    void Init(int numvertices);;

    // per material mesh
    int m_nummeshes;
    int m_meshindex[MAXSTUDIOSKINS];    // mesh to skin index
    s_mesh_t m_mesh[MAXSTUDIOSKINS];

    // vertices defined in "local" space (not remapped to global bones)
    CUtlVector<int> m_nOrigMap; // maps the original index to the new index
    CUtlVector<s_vertexinfo_t> m_vertex;
    CUtlVector<s_face_t> m_face;
    CUtlVector<s_sourceanim_t> m_Animations;

    int AddNewVert(s_source_t *pOrigSource, int nVert, int nSrcMesh, int nDstMesh, int nPreOffset = 0);

    void AddAnimations(const s_source_t *pOrigSource);

    void DestroyAnimations(s_source_t *pNewSource);

    void Copy(s_source_t *pOrigSource);
};

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
class CTwistBone {
public:
    bool m_bInverse;
    Vector m_vUpVector;
    char m_szParentBoneName[MAXSTUDIONAME];
    int m_nParentBone;
    Quaternion m_qBaseRotation;
    char m_szChildBoneName[MAXSTUDIONAME];
    int m_nChildBone;

    CUtlVector<s_constraintbonetarget_t> m_twistBoneTargets;

    CTwistBone() {
        m_bInverse = false;
        m_vUpVector.Init();
        m_szParentBoneName[0] = '\0';
        m_nParentBone = -1;
        m_qBaseRotation.Init();
        m_szChildBoneName[0] = '\0';
        m_nChildBone = -1;
    }
};

EXTERN CUtlVector<CTwistBone> g_twistbones;


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
class CConstraintBoneBase {
public:
    virtual ~CConstraintBoneBase() {}

    CUtlVector<s_constraintbonetarget_t> m_targets;
    s_constraintboneslave_t m_slave;

    bool operator==(const CConstraintBoneBase &rhs) const;

    bool operator!=(const CConstraintBoneBase &rhs) const { return !(*this == rhs); }
};


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
EXTERN CUtlVector<CConstraintBoneBase *> g_constraintBones;


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
class CPointConstraint : public CConstraintBoneBase {
public:
};


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
class COrientConstraint : public CConstraintBoneBase {
public:
};


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
class CAimConstraint : public CConstraintBoneBase {
public:
    CAimConstraint() {
        m_nUpSpaceTargetBone = -1;
    }

    Quaternion m_qAimOffset;
    Vector m_vUpVector;
    char m_szUpSpaceTargetBone[MAXSTUDIONAME];
    int m_nUpSpaceTargetBone;
    int m_nUpType;                                // CConstraintBones::AimConstraintUpType_t
};


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
class CParentConstraint : public CConstraintBoneBase {
public:
};


struct s_forcedhierarchy_t {
    char parentname[MAXSTUDIONAME];
    char childname[MAXSTUDIONAME];
    char subparentname[MAXSTUDIONAME];
};

EXTERN int g_numforcedhierarchy;
EXTERN s_forcedhierarchy_t g_forcedhierarchy[MAXSTUDIOBONES];

struct s_forcedrealign_t {
    char name[MAXSTUDIONAME];
    RadianEuler rot;
};
EXTERN int g_numforcedrealign;
EXTERN s_forcedrealign_t g_forcedrealign[MAXSTUDIOBONES];

struct s_limitrotation_t {
    char name[MAXSTUDIONAME];
    int numseq;
    char *sequencename[64];
};

EXTERN int g_numlimitrotation;
EXTERN s_limitrotation_t g_limitrotation[MAXSTUDIOBONES];

extern int BuildTris(s_trianglevert_t (*x)[3], s_mesh_t *y, byte **ppdata);


struct s_bonesaveframe_t {
    char name[MAXSTUDIOHITBOXSETNAME];
    bool bSavePos;
    bool bSaveRot;
    bool bSaveRot64;
};

EXTERN CUtlVector<s_bonesaveframe_t> g_bonesaveframe;

int OpenGlobalFile(char *src);

bool GetGlobalFilePath(const char *pSrc, char *pFullPath, int nMaxLen);

s_source_t *Load_Source(const char *filename, const char *ext, bool reverse = false, bool isActiveModel = false,
                        bool bUseCache = true);

// Resolve a $rendermesh to a bone+mesh-only clone for $collisionmodel/$collisionjoints/$generate:
// jigglebones, hitboxes, procedural bones, and flex are all suppressed/stripped. Returns a
// fresh independent clone each call; NULL if no such $rendermesh exists.
s_source_t *GetRenderMeshCollisionSource(const char *name);

// Mark a $rendermesh definition as used (without loading it), so deferred consumers
// like $generate/$generatejoint don't trip the "defined but never used" warning.
void MarkRenderMeshUsed(const char *name);

void ApplyOffsetToSrcVerts(s_source_t *pModel, matrix3x4_t matOffset);

void AddSrcToSrc(s_source_t *pOrigSource, s_source_t *pAppendSource, matrix3x4_t matOffset);

void AddSrcToSrc(s_source_t *pOrigSource, s_source_t *pAppendSource);

int Load_SMD(s_source_t *psource);

int Load_VTA(s_source_t *psource);

int Load_OBJ(s_source_t *psource);

int Load_DMX(s_source_t *psource);

//int Load_FBX( s_source_t *psource );
bool LoadPreprocessedFile(const char *pFileName, float flScale);

int AppendVTAtoOBJ(s_source_t *psource, char *filename, int frame);

void Build_Reference(s_source_t *psource, const char *pAnimName);

int Grab_Nodes(std::array<s_node_t, MAXSTUDIOSRCBONES> &pnodes);

void Grab_Animation(s_source_t *psource, const char *pAnimName);

// Processes source comment line and extracts information about the data file
void ProcessSourceComment(s_source_t *psource, const char *pCommentString);

// Processes original content file "szOriginalContentFile" that was used to generate
// data file "szDataFile"
void ProcessOriginalContentFile(const char *szDataFile, const char *szOriginalContentFile);

//-----------------------------------------------------------------------------
// Utility methods to get or add animation data from sources
//-----------------------------------------------------------------------------
s_sourceanim_t *FindSourceAnim(s_source_t *pSource, const char *pAnimName);

const s_sourceanim_t *FindSourceAnim(const s_source_t *pSource, const char *pAnimName);

s_sourceanim_t *FindOrAddSourceAnim(s_source_t *pSource, const char *pAnimName);

// Adds flexkey data to a particular source
void AddFlexKey(s_source_t *pSource, CDmeCombinationOperator *pComboOp, const char *pFlexKeyName);

// Adds combination data to the source
void AddCombination(s_source_t *pSource, CDmeCombinationOperator *pCombination);

int LookupTexture(const char *pTextureName, bool bRelativePath = false);

int UseTextureAsMaterial(int textureindex);

int MaterialToTexture(int material);

int LookupAttachment(const char *name);

void ClearModel(void);

void SimplifyModel(void);

void CollapseBones(void);

void adjust_vertex(float *org);

void scale_vertex(Vector &org);

void clip_rotations(RadianEuler &rot);

void clip_rotations(Vector &rot);

char *stristr(const char *string, const char *string2);

#define strcpyn(a, b) strncpy( a, b, sizeof( a ) )

void CalcBoneTransforms(s_animation_t *panimation, int frame, matrix3x4_t *pBoneToWorld);

void CalcBoneTransforms(s_animation_t *panimation, s_animation_t *pbaseanimation, int frame, matrix3x4_t *pBoneToWorld);

void CalcBoneTransformsCycle(s_animation_t *panimation, s_animation_t *pbaseanimation, float flCycle,
                             matrix3x4_t *pBoneToWorld);

void
BuildRawTransforms(const s_source_t *psource, const char *pAnimationName, int frame, float scale, Vector const &shift,
                   RadianEuler const &rotate, int flags, matrix3x4_t *boneToWorld);

void BuildRawTransforms(const s_source_t *psource, const char *pAnimationName, int frame, matrix3x4_t *boneToWorld);

void TranslateAnimations(const s_source_t *pSource, const matrix3x4_t *pSrcBoneToWorld, matrix3x4_t *pDestBoneToWorld);

// Returns surface property for a given joint
char *GetSurfaceProp(const char *pJointName);

int GetContents(const char *pJointName);

char *GetDefaultSurfaceProp();

int GetDefaultContents();

// Did we read 'end'
bool IsEnd(const char *pLine);

// Parses an LOD command
void Cmd_LOD(const char *cmdname);

void Cmd_ShadowLOD(void);

// Fixes up the LOD source files
void FixupLODSources();

// Get model LOD source
s_source_t *GetModelLODSource(const char *pModelName,
                              const LodScriptData_t &scriptLOD, bool *pFound);


void LoadLODSources(void);

void ConvertBoneTreeCollapsesToReplaceBones(void);

void FixupReplacedBones(void);

void UnifyLODs(void);

void SpewBoneUsageStats(void);

void MarkParentBoneLODs(void);
//void CheckAutoShareAnimationGroup( const char *animation_name );

/*
=================
=================
*/

extern bool GetLineInput(void);


struct v_unify_t {
    int refcount;
    int lastref;
    int firstref;
    int v;
    int m;
    int n;
    int t[MAXSTUDIOTEXCOORDS];
    v_unify_t *next; // pointer to next entry with same v
};

EXTERN    v_unify_t *v_list[MAXSTUDIOSRCVERTS];
EXTERN    v_unify_t v_listdata[MAXSTUDIOSRCVERTS];
EXTERN    int g_numvlist;

int SortAndBalanceBones(int iCount, int iMaxCount, int bones[], float weights[]);

void Grab_Vertexanimation(s_source_t *psource, const char *pAnimationName);

extern void BuildIndividualMeshes(s_source_t *psource);

//-----------------------------------------------------------------------------
// A little class used to deal with replacement commands
//-----------------------------------------------------------------------------

class CLodScriptReplacement_t {
public:
    void SetSrcName(const char *pSrcName) {
        if (m_pSrcName) {
            delete[] m_pSrcName;
        }
        m_pSrcName = new char[strlen(pSrcName) + 1];
        strcpy(m_pSrcName, pSrcName);
    }

    void SetDstName(const char *pDstName) {
        if (m_pDstName) {
            delete[] m_pDstName;
        }
        m_pDstName = new char[strlen(pDstName) + 1];
        strcpy(m_pDstName, pDstName);
    }

    const char *GetSrcName() const {
        return m_pSrcName;
    }

    const char *GetDstName() const {
        return m_pDstName;
    }

    CLodScriptReplacement_t() {
        m_pSrcName = NULL;
        m_pDstName = NULL;
        m_pSource = 0;
        m_flDecimationFactor = 1.0f;
    }

    ~CLodScriptReplacement_t() {
        delete[] m_pSrcName;
        delete[] m_pDstName;
    }

    s_source_t *m_pSource;
    float m_flDecimationFactor;

private:
    char *m_pSrcName;
    char *m_pDstName;
};


struct LodScriptData_t {
public:
    float switchValue;
    CUtlVector<CLodScriptReplacement_t> modelReplacements;
    CUtlVector<CLodScriptReplacement_t> generateLods;
    CUtlVector<CLodScriptReplacement_t> boneReplacements;
    CUtlVector<CLodScriptReplacement_t> boneTreeCollapses;
    CUtlVector<CLodScriptReplacement_t> materialReplacements;
    CUtlVector<CLodScriptReplacement_t> meshRemovals;
    // removemeshword: remove any mesh whose (path-stripped) material name
    // contains the stored word as a case-insensitive substring
    CUtlVector<CLodScriptReplacement_t> meshWordRemovals;


    void EnableFacialAnimation(bool val) {
        m_bFacialAnimation = val;
    }

    bool GetFacialAnimationEnabled() const {
        return m_bFacialAnimation;
    }

    void StripFromModel(bool val) {
        m_bStrippedFromModel = val;
    }

    bool IsStrippedFromModel() const {
        return m_bStrippedFromModel;
    }

    // decimateallmodel: blanket decimation factor applied to every body-part
    // model source that has no explicit replacemodel/removemodel/decimatemodel
    // entry in this block. <= 0.0f means unset.
    void SetDecimateAllFactor(float val) {
        m_flDecimateAllFactor = val;
    }

    float GetDecimateAllFactor() const {
        return m_flDecimateAllFactor;
    }

    bool HasDecimateAll() const {
        return m_flDecimateAllFactor > 0.0f;
    }

    LodScriptData_t() {
        m_bFacialAnimation = true;
        m_bStrippedFromModel = false;
        m_flDecimateAllFactor = -1.0f;
    }

private:
    bool m_bFacialAnimation;
    bool m_bStrippedFromModel;
    float m_flDecimateAllFactor;
};

EXTERN CUtlVector<LodScriptData_t> g_ScriptLODs;

EXTERN CUtlVector<char *> g_collapse;

EXTERN CUtlVector<char *> g_DoNotCollapse;



// the first time these are called, the name of the model/QC file is printed so that when 
// running in batch mode, no echo, when dumping to a file, it can be determined which file is broke.
void MdlError(const char *pMsg, ...);

void MdlWarning(const char *pMsg, ...);

void CreateMakefile_AddDependency(const char *pFileName);

void EnsureDependencyFileCheckedIn(const char *pFileName);

void AddSurfaceProp(const char *pBoneName, const char *pSurfaceProperty);

char *FindSurfaceProp(const char *pBoneName);

bool ComparePath(const char *a, const char *b);

void SetDefaultSurfaceProp(const char *pSurfaceProperty);

void PostProcessSource(s_source_t *pSource, int imodel);

byte IsByte(int val);

char IsChar(int val);

int IsInt24(int val);

short IsShort(int val);

unsigned short IsUShort(int val);

//-----------------------------------------------------------------------------
// Assigns a default contents to the entire model
//-----------------------------------------------------------------------------
struct ContentsName_t {
    char m_pJointName[128];
    int m_nContents;
};

extern int s_nDefaultContents;                            // in studiomdl.cpp
extern CUtlVector<ContentsName_t> s_JointContents;        // in studiomdl.cpp

#ifdef MDLCOMPILE
void ConvertToCurrentVersion( int nSrcVersion, const char *pFullPath );
extern int g_nMDLCommandCount;
extern MDLCommand_t *g_pMDLCommands;
void ProcessStaticProp();
s_sequence_t *ProcessCmdSequence( const char *pSequenceName );
s_animation_t *ProcessImpliedAnimation( s_sequence_t *psequence, const char *filename );
void ProcessSequence( s_sequence_t *pseq, int numblends, s_animation_t **animations, bool isAppend );
s_animation_t *LookupAnimation( const char *name );
int LookupXNode( const char *name );
int LookupPoseParameter( const char *name );
void AddBodyAttachments( s_source_t *pSource );
#endif


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
enum EyelidType_t {
    kLowerer = 0,
    kNeutral = 1,
    kRaiser = 2,
    kEyelidTypeCount = 3
};


//-----------------------------------------------------------------------------
// Used to point to the current s_model_t when loading QcModelElements from DMX
//-----------------------------------------------------------------------------
extern s_model_t *g_pCurrentModel;


int verify_atoi(const char *token);

float verify_atof(const char *token);

int LookupPoseParameter(const char *name);

void ProcessModelName(const char *pModelName);

struct StudioMdlContext {
    unsigned parseable_completion_output: 1;
    unsigned collapse_bones_message: 1;
    unsigned no_collapse_bones: 1;
    unsigned no_collapse_bones_only_weights: 1;
    unsigned quiet: 1;
    unsigned checkLengths: 1;
    unsigned printBones: 1;
    unsigned perf: 1;
    unsigned dumpGraph: 1;
    unsigned multistageGraph: 1;
    unsigned verbose: 1;
    unsigned createMakefile: 1;
    unsigned ZBrush: 1;
    unsigned verifyOnly: 1;
    unsigned useBoneInBBox: 1;
    unsigned lockBoneLengths: 1;
    unsigned defineBonesLockedByDefault: 1;
    unsigned fastBuild: 1;
    unsigned noDX80: 1;
    unsigned X360: 1;
    unsigned buildPreview: 1;
    unsigned preserveTriangleOrder: 1;
    unsigned centerBonesOnVerts: 1;
    unsigned dumpMaterials: 1;
    unsigned stripLods: 1;
    unsigned noAnimblockStall: 1;
    unsigned animblockHighRes: 1;
    unsigned animblockLowRes: 1;
    unsigned zeroFramesHighres: 1;
    unsigned lCaseAllSequences: 1;
    unsigned errorOnSeqRemapFail: 1;
    unsigned modelIntentionallyHasZeroSequences: 1;
    unsigned bContentRootRelative: 1;
    unsigned bHasModelName: 1;
    unsigned bMakeVsi: 1;
    unsigned bNoWarnings: 1;
    unsigned cullAnims: 1;
    unsigned cullMorphs: 1;
    unsigned bNoAutoDMXRulesGlobal: 1;  // $noautodmxrulesglobal: suppress auto DMX flex on every source
    unsigned bNoProceduralBonesGlobal: 1;  // $noproceduralbones: strip all axisinterp/quatinterp/aimat procedural bones
    unsigned bNoJiggleBonesGlobal: 1;  // $nojigglebones: strip all jigglebones but $donotcollapse their bones
    int g_maxWarnings = -1;
    char g_path[1024];

    int minLod;
    int numAllowedRootLODs;
    float defaultMotionRollback;
    float CollisionPrecision;
    int minSectionFrameLimit;
    int sectionFrames;
    float preloadTime;
    int maxZeroFrames; // clamped from 1..4
    float minZeroFramePosDelta;
    float defaultFadeInTime;
    float defaultFadeOutTime;
    float defaultFPS;
    char szFilename[1024];
    FILE *fpInput;
    char szLine[4096];
    int iLinecount;

    Vector vecMinWorldspace;
    Vector vecMaxWorldspace;
    DmElementHandle_t hDmeBoneFlexDriverList;

    int numtexcoords[MAXSTUDIOTEXCOORDS];
    CUtlVectorAuto<Vector2D> texcoord[MAXSTUDIOTEXCOORDS];

    std::vector<s_hitboxset> hitboxsets;
    bool bForceHitboxSet;                              // $forcehboxset was specified
    char forceHitboxSetName[MAXSTUDIOHITBOXSETNAME];   // the name it forces
    std::vector<char> KeyValueText;
    std::vector<s_flexcontrollerremap_t> FlexControllerRemap;
    std::vector<CUtlSymbol> CreateMakefileDependencies;
    char pDefaultSurfaceProp[128];
    CUtlVector<SurfacePropName_t> JointSurfaceProp;

    char *szInCurrentSeqName;
    std::vector<CUtlString> AllowedActivityNames;

    // $modelbudget soft limits (hard caps = the #defines noted in []).
    // maxBoneLimit keeps its legacy name because existing code
    // (simplify.cpp bone check) reads it.
    int budgetTotalVerts      = MAXSTUDIOVERTS;          // [MAXSTUDIOVERTS]
    int maxBoneLimit          = 256;                     // [MAXSTUDIOBONES = 1024]
    int budgetMaterials       = 64;                      // [MAXSTUDIOSKINS = 128]
    int budgetFlexControllers = 96;                      // [MAXSTUDIOFLEXCTRL = 256]
    int budgetFlexMorph       = 1024;                    // [MAXSTUDIOFLEXDESC = 4096] (also bounds flexkeys = /2)
    int budgetFlexMorphVerts  = 32768;                   // [MAXSTUDIOFLEXVERTS = 65536]
    int budgetFlexRules       = 1024;                    // [MAXSTUDIOFLEXRULES = 4096]
    int budgetPoseParam       = 24;                      // [MAXSTUDIOPOSEPARAM = 64]
    int budgetBoneConstraints = 64;                      // [MAXSTUDIOBONECONSTRAINTS = 256]
    int budgetSequences       = 1524;                    // [MAXSTUDIOSEQUENCES = 4096]
    int budgetAnimations      = 3000;                    // [MAXSTUDIOANIMS = 8192]

    int numverts = 0;
    CUtlVectorAuto<Vector> vertex;
    CUtlVectorAuto<s_boneweight_t> bone;

    int numnormals = 0;
    CUtlVectorAuto<Vector> normal;

    int numfaces = 0;
    CUtlVectorAuto<s_tmpface_t> face;
    CUtlVectorAuto<s_face_t> src_uface;            // max res unified faces
    int numbones=0;

    StudioMdlContext()
            : parseable_completion_output(0),
              collapse_bones_message(0),
              no_collapse_bones(0),
              no_collapse_bones_only_weights(0),
              quiet(0),
              checkLengths(0),
              printBones(0),
              perf(0),
              dumpGraph(0),
              multistageGraph(0),
              verbose(1),
              createMakefile(0),
              ZBrush(0),
              verifyOnly(0),
              useBoneInBBox(1),
              lockBoneLengths(0),
              defineBonesLockedByDefault(1),
              fastBuild(0),
              noDX80(0),
              X360(0),
              buildPreview(0),
              preserveTriangleOrder(0),
              centerBonesOnVerts(0),
              dumpMaterials(0),
              stripLods(0),
              noAnimblockStall(0),
              animblockHighRes(0),
              animblockLowRes(0),
              zeroFramesHighres(0),
              lCaseAllSequences(0),
              errorOnSeqRemapFail(0),
              modelIntentionallyHasZeroSequences(0),
              bContentRootRelative(0),
              bNoAutoDMXRulesGlobal(0),
              bNoProceduralBonesGlobal(0),
              bNoJiggleBonesGlobal(0),
              minLod(0),
              numAllowedRootLODs(0),
              bHasModelName(0),
              bMakeVsi(0),
              bNoWarnings(0),
              defaultMotionRollback(0.3f),
              minSectionFrameLimit(30),
              sectionFrames(30),
              preloadTime(1.0f),
              maxZeroFrames(3),
              minZeroFramePosDelta(2.0f),
              defaultFadeInTime(0.2f),
              defaultFadeOutTime(0.2f),
              defaultFPS(30.0f),
              fpInput(nullptr),
              iLinecount(0),
              vecMinWorldspace(MIN_COORD_INTEGER, MIN_COORD_INTEGER, MIN_COORD_INTEGER),
              vecMaxWorldspace(MAX_COORD_INTEGER, MAX_COORD_INTEGER, MAX_COORD_INTEGER),
              hDmeBoneFlexDriverList(DMELEMENT_HANDLE_INVALID),
              szInCurrentSeqName(nullptr) {
        memset(szFilename, 0, sizeof(szFilename));
        memset(szLine, 0, sizeof(szLine));
        memset(numtexcoords, 0, sizeof(numtexcoords));
        strncpy(pDefaultSurfaceProp, "default", sizeof(pDefaultSurfaceProp) - 1);
    }
};

#endif // STUDIOMDL_H