// Adapted from Valve's CS:GO studiomdl collisionmodel.cpp
// Builds vphysics collision (.phy) files from QC $collisionmodel / $collisionjoints

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "vphysics/constraints.h"
#include "studiomdl/collisionmodelsource.h"
#include "studiomdl/collisionmodel.h"
#include "studiomdl/convexdecompose.h"
#include "common/cmdlib.h"
#include "common/scriplib.h"
#include "common/physdll.h"
#include "common/filesystem_tools.h"
#include "mathlib/mathlib.h"
#include "studiomdl/studiomdl.h"
#include "phyfile.h"
#include "MiniVPhysics.h"
#include "tier1/strtools.h"
#include "tier1/keyvalues.h"
#include "tier2/tier2.h"

extern StudioMdlContext g_StudioMdlContext;
extern int FindLocalBoneNamed( const s_source_t *pSource, const char *pName );

IPhysicsCollision  *physcollision = NULL;
IPhysicsSurfaceProps *physprops   = NULL;

float g_WeldVertEpsilon   = 0.0f;
float g_WeldNormalEpsilon = 0.999f;
bool  g_ConvexHullCountOverride = false;

//-----------------------------------------------------------------------------
// Per-bone collision model data
//-----------------------------------------------------------------------------
class CPhysCollisionModel
{
public:
	CPhysCollisionModel()
	{
		memset( this, 0, sizeof(*this) );
	}

	const char *m_parent;
	const char *m_name;

	float m_mass;
	float m_volume;
	float m_surfaceArea;
	float m_damping;
	float m_rotdamping;
	float m_inertia;
	float m_dragCoefficient;
	float m_massBias;

	CPhysCollide        *m_pCollisionData;
	CPhysCollisionModel *m_pNext;
};

enum jointlimit_t
{
	JOINT_FREE  = 0,
	JOINT_FIXED = 1,
	JOINT_LIMIT = 2,
};

class CJointConstraint
{
public:
	CJointConstraint()
	{
		m_pJointName = NULL;
		m_pNext      = NULL;
	}

	CJointConstraint( const char *pName, int axis, jointlimit_t type, float min, float max, float friction )
		: m_axis(axis), m_jointType(type), m_limitMin(min), m_limitMax(max), m_friction(friction), m_pNext(NULL)
	{
		m_pJointName = pName;
	}

	const char   *m_pJointName;
	int           m_axis;
	jointlimit_t  m_jointType;
	float         m_limitMin;
	float         m_limitMax;
	float         m_friction;
	CJointConstraint *m_pNext;
};

struct mergelist_t
{
	char *pParent;
	char *pChild;
};

struct collisionpair_t
{
	int   obj0;
	int   obj1;
	const char *pName0;
	const char *pName1;
	collisionpair_t *pNext;
};

//-----------------------------------------------------------------------------
// Returns the physicsBoneIndex entry for a named bone in the global bone table
//-----------------------------------------------------------------------------
int FindBoneInTable( const char *pName )
{
	return findGlobalBone( pName );
}

//-----------------------------------------------------------------------------
// The jointed collision model – one global instance, reset per model
//-----------------------------------------------------------------------------
class CJointedModel : public CCollisionModelSource
{
public:
	int                  m_collisionCount;
	CPhysCollisionModel *m_pCollisionList;
	collisionpair_t     *m_pCollisionPairs;
	float                m_totalMass;
	CJointConstraint    *m_pConstraintList;
	int                  m_constraintCount;
	int                  m_totalVerts;
	bool                 m_isMassCenterForced;
	bool                 m_noSelfCollisions;
	bool                 m_remove2d;
	Vector               m_massCenterForced;

	float                m_defaultDamping;
	float                m_defaultRotdamping;
	float                m_defaultInertia;
	float                m_defaultDrag;
	CUtlVector<char>     m_textCommands;
	CUtlVector<mergelist_t> m_mergeList;

	// Per-joint override values stored during QC parsing (before bones are built)
	struct joint_override_t {
		char  name[128];
		float massBias;
		float damping;
		float rotdamping;
		float inertia;
	};
	CUtlVector<joint_override_t> m_pendingJointOverrides;

	// Auto-generation requests parsed from $generate / $generatejoint sub-tokens.
	// Each names a $rendermesh to convex-decompose into collision.  bone[0]=='\0'
	// means single-body ($generate); otherwise the hulls attach to that joint.
	// A child bone whose weighted geometry is welded into a $generatejoint's hull
	// (from $addgeneratechild).  All child geometry is emitted in the parent joint
	// bone's local space, so the combined hull still tracks the parent at runtime.
	struct child_weld_t {
		char  bone[128];
		float weightThreshold;  // own cull threshold, or <0 to inherit the request's
	};
	struct generate_request_t {
		char  bone[128];
		char  rendermesh[128];
		float concavity;
		int   maxHulls;
		int   maxVerts;         // max vertices per generated convex hull (detail/poly count)
		float weightThreshold;  // ($generatejoint) min bone weight a vertex must have
		                        // to be included; 0 = include any vertex touching the bone
		CUtlVector<child_weld_t> children;  // from $addgeneratechild
	};
	CUtlVector<generate_request_t> m_generateRequests;

	// $addgeneratechild pairings parsed before their $generatejoint may have been
	// seen; resolved against m_generateRequests at the top of ProcessGenerateRequests.
	struct pending_child_t {
		char  jointBone[128];
		char  childBone[128];
		float weightThreshold;  // <0 = inherit the request's
	};
	CUtlVector<pending_child_t> m_pendingChildren;

	float m_flFrictionTimeIn;
	float m_flFrictionTimeOut;
	float m_flFrictionTimeHold;
	int   m_iMinAnimatedFriction;
	int   m_iMaxAnimatedFriction;
	bool  m_bHasAnimatedFriction;

	CJointedModel();

	void SetSource( s_source_t *pmodel );
	void SetOverrideName( const char *pName );

	void AddMergeCommand( char const *pParent, char const *pChild );
	int  BoneIndex( const char *pName );
	void AppendCollisionModel( CPhysCollisionModel *pCollide );
	void UnlinkCollisionModel( CPhysCollisionModel *pCollide );
	CPhysCollisionModel *GetCollisionModel( const char *pName );
	void AppendCollisionPair( const char *pName0, const char *pName1 );
	void RemoveCollisionPair( const char *pName0, const char *pName1 );
	void AddConstraint( const char *pJointName, int axis, jointlimit_t jointType, float limitMin, float limitMax, float friction );
	int  CollisionIndex( const char *pName );
	void SortCollisionList();
	void ForceMassCenter( const Vector &centerOfMass );
	void AllowConcave()       { m_allowConcave = true; }
	void AllowConcaveJoints() { m_allowConcaveJoints = true; }
	void Remove2DConvex()     { m_remove2d = true; }
	void SetMaxConvex( int n) { m_maxConvex = n; }
	void DefaultDamping( float d );
	void DefaultRotdamping( float d );
	void DefaultInertia( float i );
	void DefaultDrag( float d );
	void SetTotalMass( float mass );
	void SetAutoMass();
	void SetNoSelfCollisions();
	void SetCollisionModelDefaults( CPhysCollisionModel *pModel );
	CPhysCollisionModel *InitCollisionModel( const char *pJointName );

	void JointDamping( const char *pJointName, float damping );
	void JointRotdamping( const char *pJointName, float rotdamping );
	void JointInertia( const char *pJointName, float inertia );
	void JointMassBias( const char *pJointName, float massBias );
	void ApplyPendingJointOverrides();

	void FixBoneList();
	const char *FixParent( const char *pParentName );
	void FixCollisionHierarchy();
	int  ProcessSingleBody();
	int  ProcessJointedModel();
	void ProcessGenerateRequests();

	void AddConvexSrc( const char *szFileName );

	void AddText( const char *pText )
	{
		int len = strlen(pText);
		int count = m_textCommands.Count();
		m_textCommands.AddMultipleToTail( len );
		memcpy( m_textCommands.Base() + count, pText, len );
	}

	void ComputeMass();
};

// The single global instance
CJointedModel g_JointedModel;

//-----------------------------------------------------------------------------
// Constructor
//-----------------------------------------------------------------------------
CJointedModel::CJointedModel()
{
	m_pModel = NULL;
	for ( int i = 0; i <= MAX_EXTRA_COLLISION_MODELS; i++ )
	{
		m_ExtraModels[i].m_pSrc    = NULL;
		m_ExtraModels[i].m_bConcave = false;
	}
	m_bRootCollisionIsEmpty = false;
	m_collisionCount   = 0;
	m_pCollisionList   = NULL;
	m_pCollisionPairs  = NULL;
	m_totalMass        = 1.0f;
	m_bonemap.SetSize(0);
	m_pConstraintList  = NULL;
	m_constraintCount  = 0;
	m_totalVerts       = 0;
	m_defaultDamping   = 0;
	m_defaultRotdamping= 0;
	m_defaultInertia   = 1.0f;
	m_defaultDrag      = -1;
	m_allowConcave     = false;
	m_allowConcaveJoints = false;
	m_remove2d         = false;
	m_maxConvex        = 40;
	m_isMassCenterForced = false;
	m_noSelfCollisions = false;
	m_massCenterForced.Init();
	m_flFrictionTimeIn  = 0.0f;
	m_flFrictionTimeOut = 0.0f;
	m_flFrictionTimeHold= 0.0f;
	m_iMinAnimatedFriction = 0;
	m_iMaxAnimatedFriction = 0;
	m_bHasAnimatedFriction = false;
	m_pOverrideName    = NULL;
	m_isJointed        = false;
	m_bAssumeWorldspace= false;
	m_rootName[0]      = '\0';
}

void CJointedModel::SetSource( s_source_t *pmodel )
{
	m_pModel = pmodel;
	InitBoneMap();
	m_totalVerts = pmodel->numvertices;
}

void CJointedModel::SetOverrideName( const char *pName )
{
	delete[] m_pOverrideName;
	m_pOverrideName = NULL;
	if ( pName && *pName )
	{
		int len = strlen(pName) + 1;
		m_pOverrideName = new char[len];
		V_strncpy( m_pOverrideName, pName, len );
	}
}

void CJointedModel::AddMergeCommand( char const *pParent, char const *pChild )
{
	int i = m_mergeList.AddToTail();
	m_mergeList[i].pParent = strdup(pParent);
	m_mergeList[i].pChild  = strdup(pChild);
}

int CJointedModel::BoneIndex( const char *pName )
{
	return FindLocalBoneNamed( pName );
}

void CJointedModel::AppendCollisionModel( CPhysCollisionModel *pCollide )
{
	if ( m_isMassCenterForced )
		physcollision->CollideSetMassCenter( pCollide->m_pCollisionData, m_massCenterForced );
	pCollide->m_pNext = m_pCollisionList;
	m_pCollisionList  = pCollide;
	m_collisionCount++;
}

void CJointedModel::UnlinkCollisionModel( CPhysCollisionModel *pCollide )
{
	CPhysCollisionModel **pList = &m_pCollisionList;
	if ( !pCollide ) return;
	while ( *pList )
	{
		CPhysCollisionModel *pNode = *pList;
		if ( pNode == pCollide )
		{
			*pList = pCollide->m_pNext;
			m_collisionCount--;
			pCollide->m_pNext = NULL;
			return;
		}
		pList = &pNode->m_pNext;
	}
}

int CJointedModel::CollisionIndex( const char *pName )
{
	if ( !pName ) return -1;
	CPhysCollisionModel *pList = m_pCollisionList;
	int index = 0;
	while ( pList )
	{
		if ( !stricmp( pName, pList->m_name ) )
			return index;
		pList = pList->m_pNext;
		index++;
	}
	return -1;
}

CPhysCollisionModel *CJointedModel::GetCollisionModel( const char *pName )
{
	if ( !pName ) return NULL;
	CPhysCollisionModel *pList = m_pCollisionList;
	while ( pList )
	{
		if ( !stricmp( pName, pList->m_name ) )
			return pList;
		pList = pList->m_pNext;
	}
	return NULL;
}

void CJointedModel::AppendCollisionPair( const char *pName0, const char *pName1 )
{
	collisionpair_t *pPair = new collisionpair_t;
	pPair->obj0 = -1;
	pPair->obj1 = -1;
	int idx0 = FindLocalBoneNamed( pName0 );
	pPair->pName0 = (idx0 >= 0) ? m_pModel->localBone[idx0].name : NULL;
	int idx1 = FindLocalBoneNamed( pName1 );
	pPair->pName1 = (idx1 >= 0) ? m_pModel->localBone[idx1].name : NULL;
	pPair->pNext  = m_pCollisionPairs;
	m_pCollisionPairs = pPair;
}

void CJointedModel::RemoveCollisionPair( const char *pName0, const char *pName1 )
{
	int idx0 = FindLocalBoneNamed( pName0 );
	int idx1 = FindLocalBoneNamed( pName1 );
	if ( idx0 < 0 || idx1 < 0 ) return;
	const char *szName0 = m_pModel->localBone[idx0].name;
	const char *szName1 = m_pModel->localBone[idx1].name;

	collisionpair_t *pPrev = NULL;
	collisionpair_t *pPair = m_pCollisionPairs;
	while ( pPair )
	{
		if ( pPair->pName0 && pPair->pName1 &&
		     !strcmp( pPair->pName0, szName0 ) && !strcmp( pPair->pName1, szName1 ) )
		{
			if ( pPrev ) pPrev->pNext = pPair->pNext;
			else         m_pCollisionPairs = pPair->pNext;
			delete pPair;
			return;
		}
		pPrev = pPair;
		pPair = pPair->pNext;
	}
}

void CJointedModel::AddConstraint( const char *pJointName, int axis, jointlimit_t jointType, float limitMin, float limitMax, float friction )
{
	CJointConstraint *pConstraint = new CJointConstraint( pJointName, axis, jointType, limitMin, limitMax, friction );
	pConstraint->m_pNext = m_pConstraintList;
	m_pConstraintList    = pConstraint;
	m_constraintCount++;
}

void CJointedModel::SortCollisionList()
{
	if ( !m_collisionCount ) return;

	CPhysCollisionModel **pArray = new CPhysCollisionModel *[m_collisionCount];
	CPhysCollisionModel *pList = m_pCollisionList;
	int i = 0;
	while ( pList ) { pArray[i++] = pList; pList = pList->m_pNext; }

	bool swapped = true;
	int sortPasses = 0;
	while ( swapped )
	{
		if ( ++sortPasses > m_collisionCount * m_collisionCount + 10 )
		{
			MdlWarning( "SortCollisionList: cycle detected, stopping after %d passes\n", sortPasses );
			break;
		}
		swapped = false;
		for ( i = 0; i < m_collisionCount; i++ )
		{
			CPhysCollisionModel *pPhys = pArray[i];
			if ( !pPhys->m_parent ) continue;
			if ( !Q_stricmp( pPhys->m_name, pPhys->m_parent ) ) continue;
			int j;
			for ( j = 0; j < m_collisionCount; j++ )
			{
				if ( j == i ) continue;
				if ( !stricmp( pPhys->m_parent, pArray[j]->m_name ) ) break;
			}
			if ( j > i && j < m_collisionCount )
			{
				swapped = true;
				pArray[i] = pArray[j];
				pArray[j] = pPhys;
			}
		}
	}

	for ( i = 0; i < m_collisionCount - 1; i++ )
		pArray[i]->m_pNext = pArray[i+1];
	pArray[i]->m_pNext = NULL;
	m_pCollisionList   = pArray[0];
	delete[] pArray;
}

void CJointedModel::ForceMassCenter( const Vector &centerOfMass )
{
	m_isMassCenterForced = true;
	m_massCenterForced   = centerOfMass;
}

void CJointedModel::DefaultDamping( float d )
{
	m_defaultDamping = d;
	CPhysCollisionModel *pList = m_pCollisionList;
	while ( pList ) { pList->m_damping = d; pList = pList->m_pNext; }
}

void CJointedModel::DefaultRotdamping( float d )
{
	m_defaultRotdamping = d;
	CPhysCollisionModel *pList = m_pCollisionList;
	while ( pList ) { pList->m_rotdamping = d; pList = pList->m_pNext; }
}

void CJointedModel::DefaultInertia( float i )
{
	m_defaultInertia = i;
	CPhysCollisionModel *pList = m_pCollisionList;
	while ( pList ) { pList->m_inertia = i; pList = pList->m_pNext; }
}

void CJointedModel::DefaultDrag( float d )
{
	m_defaultDrag = d;
	CPhysCollisionModel *pList = m_pCollisionList;
	while ( pList ) { pList->m_dragCoefficient = d; pList = pList->m_pNext; }
}

void CJointedModel::SetTotalMass( float mass )
{
	m_totalMass = mass;
}

void CJointedModel::SetAutoMass()
{
	m_totalMass = -1;
}

void CJointedModel::SetNoSelfCollisions()
{
	m_noSelfCollisions = true;
}

void CJointedModel::SetCollisionModelDefaults( CPhysCollisionModel *pModel )
{
	pModel->m_damping        = m_defaultDamping;
	pModel->m_rotdamping     = m_defaultRotdamping;
	pModel->m_inertia        = m_defaultInertia;
	pModel->m_dragCoefficient= m_defaultDrag;
	pModel->m_massBias       = 1.0f;
}

CPhysCollisionModel *CJointedModel::InitCollisionModel( const char *pJointName )
{
	CPhysCollisionModel *pModel = new CPhysCollisionModel;
	SetCollisionModelDefaults( pModel );
	return pModel;
}

// Helper: find or create a pending-override entry for a named joint
static CJointedModel::joint_override_t *FindOrAddOverride( CUtlVector<CJointedModel::joint_override_t> &list, const char *pName )
{
	for ( int i = 0; i < list.Count(); i++ )
		if ( !stricmp( list[i].name, pName ) ) return &list[i];
	CJointedModel::joint_override_t o;
	V_strncpy( o.name, pName, sizeof(o.name) );
	o.massBias = -1.0f; o.damping = -1.0f; o.rotdamping = -1.0f; o.inertia = -1.0f;
	list.AddToTail( o );
	return &list[ list.Count() - 1 ];
}

void CJointedModel::JointDamping( const char *pJointName, float damping )
{
	CPhysCollisionModel *pModel = GetCollisionModel( pJointName );
	if ( pModel ) { pModel->m_damping = damping; return; }
	FindOrAddOverride( m_pendingJointOverrides, pJointName )->damping = damping;
}

void CJointedModel::JointRotdamping( const char *pJointName, float rotdamping )
{
	CPhysCollisionModel *pModel = GetCollisionModel( pJointName );
	if ( pModel ) { pModel->m_rotdamping = rotdamping; return; }
	FindOrAddOverride( m_pendingJointOverrides, pJointName )->rotdamping = rotdamping;
}

void CJointedModel::JointInertia( const char *pJointName, float inertia )
{
	CPhysCollisionModel *pModel = GetCollisionModel( pJointName );
	if ( pModel ) { pModel->m_inertia = inertia; return; }
	FindOrAddOverride( m_pendingJointOverrides, pJointName )->inertia = inertia;
}

void CJointedModel::JointMassBias( const char *pJointName, float massBias )
{
	CPhysCollisionModel *pModel = GetCollisionModel( pJointName );
	if ( pModel ) { pModel->m_massBias = massBias; return; }
	FindOrAddOverride( m_pendingJointOverrides, pJointName )->massBias = massBias;
}

void CJointedModel::ApplyPendingJointOverrides()
{
	for ( int i = 0; i < m_pendingJointOverrides.Count(); i++ )
	{
		const joint_override_t &o = m_pendingJointOverrides[i];
		CPhysCollisionModel *pModel = GetCollisionModel( o.name );
		if ( !pModel ) continue;
		if ( o.massBias   >= 0.0f ) pModel->m_massBias   = o.massBias;
		if ( o.damping    >= 0.0f ) pModel->m_damping    = o.damping;
		if ( o.rotdamping >= 0.0f ) pModel->m_rotdamping = o.rotdamping;
		if ( o.inertia    >= 0.0f ) pModel->m_inertia    = o.inertia;
	}
	m_pendingJointOverrides.RemoveAll();
}

void CJointedModel::AddConvexSrc( const char *szFileName )
{
	for ( int i = 0; i < MAX_EXTRA_COLLISION_MODELS; i++ )
	{
		if ( m_ExtraModels[i].m_pSrc == NULL )
		{
			int nummaterials = g_nummaterials;
			int numtextures  = g_numtextures;
			s_source_t *pmodel = Load_Source( szFileName, "", false, false, false );
			if ( !pmodel ) return;
			if ( nummaterials && numtextures && (numtextures != g_numtextures || nummaterials != g_nummaterials) )
			{
				g_numtextures  = numtextures;
				g_nummaterials = nummaterials;
				pmodel->texmap[0] = 0;
			}
			m_ExtraModels[i].m_pSrc = pmodel;
			m_ExtraModels[i].m_matOffset.SetToIdentity();
			return;
		}
	}
	MdlWarning( "Cannot add more than %d extra collision models.  Ignoring $addconvexsrc \"%s\".\n", MAX_EXTRA_COLLISION_MODELS, szFileName );
}

void CJointedModel::ComputeMass()
{
	if ( m_totalMass < 0 ) return;

	float totalVol = 0;
	CPhysCollisionModel *pList = m_pCollisionList;
	while ( pList ) { totalVol += pList->m_volume * pList->m_massBias; pList = pList->m_pNext; }
	if ( totalVol <= 0 ) totalVol = 1;

	pList = m_pCollisionList;
	while ( pList )
	{
		pList->m_mass = ( pList->m_volume * pList->m_massBias / totalVol ) * m_totalMass;
		if ( pList->m_mass < 1.0f ) pList->m_mass = 1.0f;
		pList = pList->m_pNext;
	}
}

//=============================================================================
// Geometry helpers
//=============================================================================

static CPhysCollisionModel *FindObjectInList( CPhysCollisionModel *pHead, const char *pName )
{
	while ( pHead )
	{
		if ( !stricmp( pName, pHead->m_name ) ) break;
		pHead = pHead->m_pNext;
	}
	return pHead;
}

void CJointedModel::FixBoneList()
{
	if ( !m_isJointed ) return;

	CPhysCollisionModel *pmodel = m_pCollisionList;
	while ( pmodel )
	{
		int nodeIndex = FindLocalBoneNamed( pmodel->m_name );
		if ( nodeIndex < 0 )
		{
			MdlWarning( "Physics for unknown bone %s\n", pmodel->m_name );
		}
		else
		{
			int count = 0;
			while ( m_pModel->boneLocalToGlobal[nodeIndex] < 0 )
			{
				if ( count++ > MAXSTUDIOSRCBONES ) break;
				nodeIndex = m_pModel->localBone[nodeIndex].parent;
			}
			if ( nodeIndex >= 0 )
			{
				pmodel->m_name   = g_bonetable[ m_pModel->boneLocalToGlobal[nodeIndex] ].name;
				pmodel->m_parent = NULL;
				int parentIndex  = m_pModel->localBone[nodeIndex].parent;
				if ( parentIndex >= 0 && parentIndex != nodeIndex )
				{
					parentIndex = m_bonemap[parentIndex];
					if ( m_pModel->boneLocalToGlobal[parentIndex] < 0 )
						pmodel->m_parent = m_pModel->localBone[parentIndex].name;
					else
						pmodel->m_parent = g_bonetable[ m_pModel->boneLocalToGlobal[parentIndex] ].name;
				}
			}
			else
			{
				MdlWarning( "Physics for unknown bone %s\n", pmodel->m_name );
			}
		}
		pmodel = pmodel->m_pNext;
	}
}

const char *CJointedModel::FixParent( const char *pParentName )
{
	while ( pParentName )
	{
		if ( FindObjectInList( m_pCollisionList, pParentName ) )
			return pParentName;
		int nodeIndex = FindLocalBoneNamed( pParentName );
		if ( nodeIndex < 0 ) return NULL;
		int parentIndex = m_pModel->localBone[nodeIndex].parent;
		if ( parentIndex < 0 ) break;
		pParentName = m_pModel->localBone[parentIndex].name;
	}
	return NULL;
}

struct boundingvolume_t { Vector mins, maxs; };

static void CreateCollide( CPhysCollisionModel *pBase, CPhysConvex **pElements, int elementCount, const boundingvolume_t &bv )
{
	if ( !pBase ) return;

	pBase->m_volume = 0;
	pBase->m_surfaceArea = 0;
	for ( int i = 0; i < elementCount; i++ )
	{
		pBase->m_volume      += physcollision->ConvexVolume( pElements[i] );
		pBase->m_surfaceArea += physcollision->ConvexSurfaceArea( pElements[i] );
	}

	Vector size = bv.maxs - bv.mins;
	int largest = 0;
	for ( int i = 0; i < 3; i++ )
		if ( size[i] > size[largest] ) largest = i;

	Vector tmp = size;
	tmp[largest] = 0;
	float len = tmp.Length();
	if ( len > 0 && (size[largest] / len) > 9 )
		pBase->m_rotdamping = 1.0f;

	pBase->m_pCollisionData = physcollision->ConvertConvexToCollide( pElements, elementCount );
}

static bool IsApproximatelyPlanar( Vector **verts, int vertCount, float epsilon )
{
	if ( vertCount < 4 ) return true;

	int v0 = 1, v1 = 2;
	Vector normal;
	while ( v0 < vertCount && v1 < vertCount )
	{
		Vector edge0 = *verts[v0] - *verts[0];
		Vector edge1 = *verts[v1] - *verts[0];
		normal = CrossProduct( edge0, edge1 );
		float len = VectorNormalize( normal );
		if ( len > 0.001f ) break;
		if ( edge0.Length() < 0.001f ) { v0++; v1++; }
		else                             v1++;
	}

	float minDist = DotProduct( normal, *verts[0] );
	float maxDist = minDist;
	for ( int i = 0; i < vertCount; i++ )
	{
		float d = DotProduct( *verts[i], normal );
		if ( d < minDist ) minDist = d;
		else if ( d > maxDist ) maxDist = d;
		if ( fabsf(maxDist - minDist) > epsilon ) return false;
	}
	return true;
}

static void BuildVertWeldTable( int *weldTable, s_source_t *pmodel )
{
	for ( int i = 0; i < pmodel->numvertices; i++ )
	{
		bool found = false;
		for ( int j = 0; j < i; j++ )
		{
			float dist       = (pmodel->vertex[j].position - pmodel->vertex[i].position).Length();
			float normalDist = DotProduct( pmodel->vertex[j].normal, pmodel->vertex[i].normal );
			if ( dist <= g_WeldVertEpsilon && normalDist > g_WeldNormalEpsilon )
			{
				found = true;
				weldTable[i] = j;
				break;
			}
		}
		if ( !found ) weldTable[i] = i;
	}
}

static void BuildConvexListByVertID( s_source_t *pmodel, CUtlVector<convexlist_t> &convexList, CUtlVector<int> &vertList, CUtlVector<int> &vertID )
{
	convexlist_t current;
	for ( int i = 0; i < pmodel->numvertices; i++ )
	{
		if ( vertID[i] < 0 || vertID[i] > pmodel->numfaces ) continue;
		current.firstVertIndex = vertList.Count();
		current.numVertIndex   = 0;
		int id = vertID[i];
		for ( int j = i; j < pmodel->numvertices; j++ )
		{
			if ( vertID[j] == id )
			{
				vertList.AddToTail(j);
				current.numVertIndex++;
				vertID[j] = -1;
			}
		}
		convexList.AddToTail( current );
	}
}

static void BuildSingleConvexForFaceList( s_source_t *pmodel, CUtlVector<convexlist_t> &convexList, CUtlVector<int> &vertList, const CUtlVector<s_face_t> &faceList )
{
	CUtlVector<int> vertID;
	vertID.SetCount( pmodel->numvertices );
	for ( int i = 0; i < pmodel->numvertices; i++ ) vertID[i] = -1;
	for ( int i = 0; i < faceList.Count(); i++ )
	{
		vertID[faceList[i].a] = 1;
		vertID[faceList[i].b] = 1;
		vertID[faceList[i].c] = 1;
	}
	BuildConvexListByVertID( pmodel, convexList, vertList, vertID );
}

static void BuildConvexListForFaceList( s_source_t *pmodel, CUtlVector<convexlist_t> &convexList, CUtlVector<int> &vertList, const CUtlVector<s_face_t> &faceList )
{
	CUtlVector<int> weldTable;
	weldTable.SetCount( pmodel->numvertices );
	BuildVertWeldTable( weldTable.Base(), pmodel );

	CUtlVector<int> vertID;
	vertID.SetCount( pmodel->numvertices );
	for ( int i = 0; i < pmodel->numvertices; i++ )
		vertID[i] = (weldTable[i] != i) ? -1 : pmodel->numfaces + 1;

	int marked = 0;
	do
	{
		marked = 0;
		for ( int i = 0; i < faceList.Count(); i++ )
		{
			s_face_t f = faceList[i];
			f.a = weldTable[f.a]; f.b = weldTable[f.b]; f.c = weldTable[f.c];
			int newid = MIN( i, vertID[f.a] );
			newid = MIN( newid, vertID[f.b] );
			newid = MIN( newid, vertID[f.c] );
			if ( vertID[f.a] != newid ) { vertID[f.a] = newid; marked++; }
			if ( vertID[f.b] != newid ) { vertID[f.b] = newid; marked++; }
			if ( vertID[f.c] != newid ) { vertID[f.c] = newid; marked++; }
		}
	} while ( marked != 0 );

	BuildConvexListByVertID( pmodel, convexList, vertList, vertID );
}

static bool BuildConvexesForLists( CUtlVector<CPhysConvex *> &convexOut, const CUtlVector<convexlist_t> &convexList, const CUtlVector<int> &vertList, const CUtlVector<Vector> &worldspaceVerts, bool bRemove2d )
{
	bool bValid = true;
	CUtlVector<Vector *> vertsThisConvex;
	for ( int i = 0; i < convexList.Count(); i++ )
	{
		const convexlist_t &elem = convexList[i];
		vertsThisConvex.RemoveAll();
		for ( int j = 0; j < elem.numVertIndex; j++ )
		{
			Vector *pVert = const_cast<Vector *>(&worldspaceVerts[vertList[j + elem.firstVertIndex]]);
			vertsThisConvex.AddToTail( pVert );
		}
		if ( vertsThisConvex.Count() > 2 )
		{
			const float g_epsilon_2d = 0.5f;
			if ( IsApproximatelyPlanar( vertsThisConvex.Base(), vertsThisConvex.Count(), g_epsilon_2d ) )
			{
				if ( bRemove2d ) continue;
				MdlWarning( "Model has 2-dimensional geometry (less than %.3f inches thick)!!!\n", g_epsilon_2d );
				bValid = false;
			}
			CPhysConvex *pConvex = physcollision->ConvexFromVerts( vertsThisConvex.Base(), vertsThisConvex.Count() );
			if ( pConvex )
			{
				physcollision->SetConvexGameData( pConvex, 0 );
				convexOut.AddToTail( pConvex );
			}
		}
	}
	return bValid;
}

//-----------------------------------------------------------------------------
// ProcessJointedModel: builds per-bone collision shapes
//-----------------------------------------------------------------------------
int CJointedModel::ProcessJointedModel()
{
	if ( !g_StudioMdlContext.quiet )
		printf( "Processing jointed collision model\n" );

	for ( int boneIndex = 0; boneIndex < m_pModel->numbones; boneIndex++ )
	{
		if ( !ShouldProcessBone( boneIndex ) ) continue;

		CUtlVector<Vector> bonespaceVerts;
		bonespaceVerts.SetCount( m_pModel->numvertices );
		ConvertToBoneSpace( boneIndex, bonespaceVerts );

		// If $transformbindposebone edited this bone's bind pose, the hull is placed
		// at runtime by the edited g_bonetable transform (boneToPose_orig * D_total).
		// Pre-multiply by Inverse(D_total) so the hull lands at its pre-edit position
		// and still tracks the bone in ragdoll. No-op (identity) for unedited bones.
		matrix3x4_t boneEditDelta;
		if ( GetAccumulatedBoneEditDelta( m_pModel->boneLocalToGlobal[boneIndex], boneEditDelta ) )
		{
			matrix3x4_t boneEditDeltaInv;
			MatrixInvert( boneEditDelta, boneEditDeltaInv );
			for ( int vi = 0; vi < bonespaceVerts.Count(); vi++ )
			{
				Vector corrected;
				VectorTransform( bonespaceVerts[vi], boneEditDeltaInv, corrected );
				bonespaceVerts[vi] = corrected;
			}
		}

		CUtlVector<s_face_t>   faceList;
		CUtlVector<convexlist_t> convexList;
		CUtlVector<int>        vertList;
		CUtlVector<CPhysConvex *> convexOut;
		bool bValid = false;

		for ( int i = 0; i < m_pModel->nummeshes; i++ )
		{
			s_mesh_t *pmesh = m_pModel->mesh + m_pModel->meshindex[i];
			for ( int j = 0; j < pmesh->numfaces; j++ )
			{
				s_face_t *face = m_pModel->face + pmesh->faceoffset + j;
				s_face_t globalFace;
				GlobalFace( &globalFace, pmesh, face );
				if ( FaceHasVertOnBone( globalFace, boneIndex ) )
					faceList.AddToTail( globalFace );
			}

			if ( m_allowConcaveJoints )
				BuildConvexListForFaceList( m_pModel, convexList, vertList, faceList );
			else
				BuildSingleConvexForFaceList( m_pModel, convexList, vertList, faceList );

			bValid = BuildConvexesForLists( convexOut, convexList, vertList, bonespaceVerts, m_remove2d );
		}

		if ( convexOut.Count() > m_maxConvex )
		{
			MdlWarning( "COSTLY COLLISION MODEL!!!! (%d parts - %d allowed)\n", convexOut.Count(), m_maxConvex );
			bValid = false;
		}

		if ( !bValid && convexOut.Count() )
		{
			MdlWarning( "Error with convex elements of %s, building single convex!!!!\n", m_pModel->filename );
			for ( int i = 0; i < convexOut.Count(); i++ )
				physcollision->ConvexFree( convexOut[i] );
			convexOut.Purge();
		}

		if ( convexOut.Count() )
		{
			CPhysCollisionModel *pPhys = InitCollisionModel( m_pModel->localBone[boneIndex].name );
			pPhys->m_mass   = 1.0f;
			pPhys->m_name   = m_pModel->localBone[boneIndex].name;
			pPhys->m_parent = (m_pModel->localBone[boneIndex].parent >= 0)
				? m_pModel->localBone[ m_pModel->localBone[boneIndex].parent ].name
				: NULL;

			boundingvolume_t bv;
			ClearBounds( bv.mins, bv.maxs );
			int vertCount = 0;
			for ( int i = 0; i < convexList.Count(); i++ )
			{
				const convexlist_t &elem = convexList[i];
				for ( int j = 0; j < elem.numVertIndex; j++ )
				{
					AddPointToBounds( bonespaceVerts[vertList[elem.firstVertIndex+j]], bv.mins, bv.maxs );
					vertCount++;
				}
			}
			for ( int i = 0; i < convexOut.Count(); i++ )
			{
				int globalBoneIndex = m_pModel->boneLocalToGlobal[boneIndex];
				physcollision->SetConvexGameData( convexOut[i], globalBoneIndex + 1 );
			}

			CreateCollide( pPhys, convexOut.Base(), convexOut.Count(), bv );
			if ( !g_StudioMdlContext.quiet )
				printf( "%-24s (%3d verts, %d convex elements) volume: %4.2f\n", pPhys->m_name, vertCount, convexOut.Count(), pPhys->m_volume );
			UnlinkCollisionModel( pPhys );
			AppendCollisionModel( pPhys );
		}
	}

	// Remove any bones that ended up with no geometry
	CPhysCollisionModel *pPhys = m_pCollisionList;
	while ( pPhys )
	{
		CPhysCollisionModel *pNext = pPhys->m_pNext;
		if ( !pPhys->m_pCollisionData )
		{
			UnlinkCollisionModel( pPhys );
			delete pPhys;
		}
		pPhys = pNext;
	}
	return 1;
}

//-----------------------------------------------------------------------------
// ProcessSingleBody: builds a single convex prop collision shape
//-----------------------------------------------------------------------------
int CJointedModel::ProcessSingleBody()
{
	static const int nMaxModels = MAX_EXTRA_COLLISION_MODELS + 1;

	if ( !m_bRootCollisionIsEmpty )
	{
		m_ExtraModels[MAX_EXTRA_COLLISION_MODELS].m_pSrc    = m_pModel;
		m_ExtraModels[MAX_EXTRA_COLLISION_MODELS].m_bConcave = m_allowConcave;
		m_ExtraModels[MAX_EXTRA_COLLISION_MODELS].m_matOffset.SetToIdentity();
	}

	for ( int i = 0; i < nMaxModels; i++ )
	{
		if ( m_ExtraModels[i].m_pSrc )
		{
			if ( !m_allowConcave ) m_ExtraModels[i].m_bConcave = false;
			ApplyOffsetToSrcVerts( m_ExtraModels[i].m_pSrc, m_ExtraModels[i].m_matOffset );
		}
	}

	s_source_t *pConcaveSrc  = NULL;
	s_source_t *pFallbackSrc = NULL;
	for ( int i = 0; i < nMaxModels; i++ )
	{
		if ( m_ExtraModels[i].m_pSrc )
		{
			if ( !pFallbackSrc ) pFallbackSrc = m_ExtraModels[i].m_pSrc;
			if ( m_ExtraModels[i].m_bConcave )
			{
				if ( !pConcaveSrc ) pConcaveSrc = m_ExtraModels[i].m_pSrc;
				else                AddSrcToSrc( pConcaveSrc, m_ExtraModels[i].m_pSrc );
			}
		}
	}

	if ( !m_pModel )
	{
		if      ( pConcaveSrc )  m_pModel = pConcaveSrc;
		else if ( pFallbackSrc ) m_pModel = pFallbackSrc;
		else                     Error( "No valid physics source mesh!\n" );
	}

	CUtlVector<CPhysConvex *> convexOut;
	CUtlVector<convexlist_t>  convexList;
	CUtlVector<Vector>        allworldspaceVerts;
	bool bValid = true;


	if ( pConcaveSrc && m_allowConcave )
	{
		CUtlVector<Vector> worldspaceVerts;
		ConvertToWorldSpace( worldspaceVerts, pConcaveSrc );
		allworldspaceVerts.AddVectorToTail( worldspaceVerts );

		CUtlVector<s_face_t> faceList;
		CUtlVector<int>      vertList;
		for ( int i = 0; i < pConcaveSrc->nummeshes; i++ )
		{
			s_mesh_t *pmesh = pConcaveSrc->mesh + pConcaveSrc->meshindex[i];
			for ( int j = 0; j < pmesh->numfaces; j++ )
			{
				s_face_t *face = pConcaveSrc->face + pmesh->faceoffset + j;
				s_face_t globalFace; GlobalFace( &globalFace, pmesh, face );
				faceList.AddToTail( globalFace );
			}
		}
		BuildConvexListForFaceList( pConcaveSrc, convexList, vertList, faceList );
		bValid = BuildConvexesForLists( convexOut, convexList, vertList, worldspaceVerts, m_remove2d );
	}

	for ( int i = 0; i < nMaxModels; i++ )
	{
		if ( m_ExtraModels[i].m_pSrc && !m_ExtraModels[i].m_bConcave )
		{
			s_source_t *pmodel = m_ExtraModels[i].m_pSrc;
			CUtlVector<Vector> worldspaceVertsExtra;
			ConvertToWorldSpace( worldspaceVertsExtra, pmodel );
			allworldspaceVerts.AddVectorToTail( worldspaceVertsExtra );

			CUtlVector<Vector *> vertsThisConvex;
			FOR_EACH_VEC( worldspaceVertsExtra, j )
			{
				vertsThisConvex.AddToTail( const_cast<Vector *>(&worldspaceVertsExtra[j]) );
			}
			CPhysConvex *pConvex = physcollision->ConvexFromVerts( vertsThisConvex.Base(), vertsThisConvex.Count() );
			if ( pConvex )
			{
				physcollision->SetConvexGameData( pConvex, 0 );
				convexOut.AddToTail( pConvex );
			}
			else
			{
				MdlWarning( "Error with convex elements of %s!\n", pmodel->filename );
				bValid = false;
			}
		}
	}

	if ( convexOut.Count() > m_maxConvex )
	{
		if ( g_ConvexHullCountOverride )
			MdlWarning( "Allowing costly collision model. (%d parts - %d normally allowed)\n", convexOut.Count(), m_maxConvex );
		else
		{
			MdlWarning( "COSTLY COLLISION MODEL!!!! (%d parts - %d allowed)\n", convexOut.Count(), m_maxConvex );
			bValid = false;
		}
	}

	if ( !bValid )
	{
		for ( int i = 0; i < convexOut.Count(); i++ ) physcollision->ConvexFree( convexOut[i] );
		convexOut.Purge();
	}

	if ( !convexOut.Count() || !m_allowConcave )
	{
		convexOut.Purge();
		if ( allworldspaceVerts.Count() == 0 )
			ConvertToWorldSpace( allworldspaceVerts, m_pModel );

		CUtlVector<Vector *> vertsThisConvex;
		FOR_EACH_VEC( allworldspaceVerts, j )
			vertsThisConvex.AddToTail( const_cast<Vector *>(&allworldspaceVerts[j]) );

		CPhysConvex *pConvex = physcollision->ConvexFromVerts( vertsThisConvex.Base(), vertsThisConvex.Count() );
		if ( pConvex )
		{
			physcollision->SetConvexGameData( pConvex, 0 );
			convexOut.AddToTail( pConvex );
		}
		else
		{
			Error( "Error building fallback convex hull!\n" );
		}
	}

	if ( convexOut.Count() )
	{
		if ( !g_StudioMdlContext.quiet )
			printf( "Model has %d convex sub-parts\n", convexOut.Count() );

		CPhysCollisionModel *pPhys = new CPhysCollisionModel;
		SetCollisionModelDefaults( pPhys );

		boundingvolume_t bv;
		ClearBounds( bv.mins, bv.maxs );
		for ( int i = allworldspaceVerts.Count() - 1; --i >= 0; )
			AddPointToBounds( allworldspaceVerts[i], bv.mins, bv.maxs );

		CreateCollide( pPhys, convexOut.Base(), convexOut.Count(), bv );
		pPhys->m_mass = 1.0f;

		char tmp[512];
		Q_FileBase( m_pModel->filename, tmp, sizeof(tmp) );
		char *out = new char[strlen(tmp)+1];
		strcpy( out, tmp );
		pPhys->m_name   = out;
		pPhys->m_parent = NULL;
		AppendCollisionModel( pPhys );
	}
	return 1;
}

//-----------------------------------------------------------------------------
// Build a flat triangle mesh from a source for convex decomposition.
//
// keepBone < 0  : keep all geometry (single-body case).
// keepBone >= 0 : keep only vertices whose weight to keepBone is >= cullWeight,
//                 and only triangles whose three vertices all pass.  This keeps
//                 the resulting hull tight to the bone and stops neighbouring
//                 joints' hulls from overlapping in the skin blend region.
//                 cullWeight 0 keeps any vertex that touches the bone at all.
// Output positions are produced by posFn(globalVertIndex); re-indexed to a
// compact vertex list.
//-----------------------------------------------------------------------------
// One (bone, threshold) entry in the keep-list passed to ExtractTriangleMesh.  A
// vertex passes if it meets the threshold for ANY listed bone (parent or child).
struct bonecull_t {
	int   localBone;
	float threshold;  // cullWeight; 0 -> any vertex touching the bone passes
};

// bKeepAll true ($generate) keeps every face regardless of weighting; otherwise a
// face is kept only when all three of its verts pass some bone in keepBones.
template <typename PosFn>
static void ExtractTriangleMesh( s_source_t *pSrc, bool bKeepAll,
                                 const CUtlVector<bonecull_t> &keepBones,
                                 CUtlVector<Vector> &outVerts,
                                 CUtlVector<int> &outTris,
                                 PosFn posFn )
{
	outVerts.RemoveAll();
	outTris.RemoveAll();

	// Map global source vertex index -> compact output index (lazily assigned).
	CUtlVector<int> remap;
	remap.SetCount( pSrc->numvertices );
	for ( int i = 0; i < pSrc->numvertices; i++ )
		remap[i] = -1;

	auto vertPasses = [&]( uint32_t vi ) -> bool
	{
		if ( bKeepAll ) return true;
		if ( vi >= (uint32_t)pSrc->numvertices ) return false;
		const s_boneweight_t &bw = pSrc->vertex[vi].boneweight;
		for ( int c = 0; c < keepBones.Count(); c++ )
		{
			for ( int k = 0; k < bw.numbones; k++ )
				if ( bw.bone[k] == keepBones[c].localBone )
				{
					if ( bw.weight[k] >= keepBones[c].threshold )
						return true;  // threshold 0 -> any touch passes
					break;  // found this bone, didn't meet its threshold; try next bone
				}
		}
		return false;
	};

	auto emit = [&]( uint32_t vi ) -> int
	{
		if ( vi >= (uint32_t)pSrc->numvertices ) return -1;
		if ( remap[vi] < 0 )
		{
			remap[vi] = outVerts.AddToTail( posFn( vi ) );
		}
		return remap[vi];
	};

	for ( int m = 0; m < pSrc->nummeshes; m++ )
	{
		s_mesh_t *pmesh = pSrc->mesh + pSrc->meshindex[m];
		for ( int f = 0; f < pmesh->numfaces; f++ )
		{
			s_face_t *face = pSrc->face + pmesh->faceoffset + f;
			s_face_t gf; GlobalFace( &gf, pmesh, face );

			// All three verts must pass so the hull does not extend back out to
			// off-bone (below-threshold) geometry.
			if ( !vertPasses( gf.a ) || !vertPasses( gf.b ) || !vertPasses( gf.c ) )
				continue;

			int a = emit( gf.a ), b = emit( gf.b ), c = emit( gf.c );
			if ( a < 0 || b < 0 || c < 0 ) continue;
			outTris.AddToTail( a );
			outTris.AddToTail( b );
			outTris.AddToTail( c );
		}
	}
}

//-----------------------------------------------------------------------------
// Turn a set of decomposed vertex clouds into convex hulls and attach them to a
// collision model.  Mirrors BuildConvexesForLists + CreateCollide.  gameData is
// the bone index+1 for joints, or 0 for a single rigid body.
//-----------------------------------------------------------------------------
// Returns the number of convex pieces that were accepted by minicollision and
// attached.  0 means the solid has NO collision data (caller must not keep it).
static int AttachDecomposedHulls( CPhysCollisionModel *pPhys,
                                  const CUtlVector<DecomposedHull> &hulls,
                                  unsigned int gameData )
{
	CUtlVector<CPhysConvex *> convexOut;
	boundingvolume_t bv;
	ClearBounds( bv.mins, bv.maxs );

	int nSkipped = 0;
	for ( int i = 0; i < hulls.Count(); i++ )
	{
		const DecomposedHull &h = hulls[i];
		if ( h.verts.Count() < 4 ) { nSkipped++; continue; }

		// Feed the hull's vertex cloud to the packer.  The packer re-hulls and
		// rejects clouds it can't turn into a clean closed manifold (typically
		// caused by near-coplanar points).  If rejected, retry with progressively
		// decimated subsets (keep every Nth vertex) - a sparser cloud of the same
		// shape sidesteps the coplanar-edge degeneracy while preserving extents.
		CPhysConvex *pConvex = NULL;
		for ( int stride = 1; stride <= 4 && !pConvex; stride++ )
		{
			CUtlVector<Vector *> vertsThisConvex;
			for ( int j = 0; j < h.verts.Count(); j += stride )
				vertsThisConvex.AddToTail( const_cast<Vector *>( &h.verts[j] ) );
			if ( vertsThisConvex.Count() < 4 )
				break;
			pConvex = physcollision->ConvexFromVerts( vertsThisConvex.Base(), vertsThisConvex.Count() );
		}

		if ( pConvex )
		{
			physcollision->SetConvexGameData( pConvex, gameData );
			convexOut.AddToTail( pConvex );
			for ( int j = 0; j < h.verts.Count(); j++ )
				AddPointToBounds( h.verts[j], bv.mins, bv.maxs );
		}
		else
		{
			nSkipped++;
		}
	}

	if ( nSkipped )
		MdlWarning( "collision autogen: %d of %d generated convex piece(s) rejected by the physics packer\n",
			nSkipped, hulls.Count() );

	if ( convexOut.Count() )
		CreateCollide( pPhys, convexOut.Base(), convexOut.Count(), bv );

	return convexOut.Count();
}

//=============================================================================
// ProcessGenerateRequests: auto-generate convex collision from $rendermesh
// geometry for $generate (single body) and $generatejoint (per joint) tokens.
//=============================================================================
void CJointedModel::ProcessGenerateRequests()
{
	// Fold $addgeneratechild pairings into their matching $generatejoint request.
	// Done here (not at parse time) so child lines may precede their joint in the QC.
	for ( int p = 0; p < m_pendingChildren.Count(); p++ )
	{
		const pending_child_t &pc = m_pendingChildren[p];
		generate_request_t *pReq = NULL;
		for ( int r = 0; r < m_generateRequests.Count(); r++ )
			if ( m_generateRequests[r].bone[0] && !stricmp( m_generateRequests[r].bone, pc.jointBone ) )
			{
				pReq = &m_generateRequests[r];
				break;
			}
		if ( !pReq )
		{
			MdlWarning( "$addgeneratechild: no $generatejoint for bone '%s' (child '%s' ignored).\n",
				pc.jointBone, pc.childBone );
			continue;
		}
		child_weld_t cw;
		V_strncpy( cw.bone, pc.childBone, sizeof(cw.bone) );
		cw.weightThreshold = pc.weightThreshold;
		pReq->children.AddToTail( cw );
	}

	for ( int r = 0; r < m_generateRequests.Count(); r++ )
	{
		const generate_request_t &req = m_generateRequests[r];

		s_source_t *pSrc = GetRenderMeshSource( req.rendermesh );
		if ( !pSrc )
		{
			MdlError( "$generate%s: unknown $rendermesh '%s' (define it with $rendermesh first).\n",
				req.bone[0] ? "joint" : "", req.rendermesh );
			return;
		}

		CUtlVector<Vector> verts;
		CUtlVector<int>    tris;
		bool bJoint = ( req.bone[0] != '\0' );

		if ( bJoint )
		{
			// Per-joint: extract faces weighted to the named bone, in that bone's
			// local space, so the generated hull tracks the bone at runtime.
			int localBone = ::FindLocalBoneNamed( pSrc, req.bone );
			if ( localBone < 0 )
			{
				MdlError( "$generatejoint: bone '%s' not found in $rendermesh '%s'.\n",
					req.bone, req.rendermesh );
				return;
			}
			int globalBone = findGlobalBone( req.bone );
			if ( globalBone < 0 )
			{
				MdlError( "$generatejoint: bone '%s' not present in the compiled model.\n", req.bone );
				return;
			}

			matrix3x4_t poseFromBone = pSrc->boneToPose[localBone];
			matrix3x4_t boneFromPose;
			MatrixInvert( poseFromBone, boneFromPose );

			// Keep-list: the joint bone plus any $addgeneratechild bones.  All of
			// their geometry is emitted in the parent bone's local space below, so
			// the welded hull tracks the parent joint at runtime.
			CUtlVector<bonecull_t> keepBones;
			{
				bonecull_t bc; bc.localBone = localBone; bc.threshold = req.weightThreshold;
				keepBones.AddToTail( bc );
			}
			for ( int c = 0; c < req.children.Count(); c++ )
			{
				int childLocal = ::FindLocalBoneNamed( pSrc, req.children[c].bone );
				if ( childLocal < 0 )
				{
					MdlWarning( "$addgeneratechild: child bone '%s' not found in $rendermesh '%s' (skipped).\n",
						req.children[c].bone, req.rendermesh );
					continue;
				}
				bonecull_t bc;
				bc.localBone = childLocal;
				bc.threshold = ( req.children[c].weightThreshold < 0.0f )
					? req.weightThreshold : req.children[c].weightThreshold;
				keepBones.AddToTail( bc );
			}

			ExtractTriangleMesh( pSrc, false, keepBones, verts, tris,
				[&]( uint32_t vi ) -> Vector {
					Vector out;
					VectorTransform( pSrc->vertex[vi].position, boneFromPose, out );
					return out;
				} );

			// Compensate $transformbindposebone bind-pose edits on the joint bone, as
			// ProcessJointedModel does; no-op for unedited bones.  Child welds ride the
			// parent's frame, so this parent-keyed correction covers the whole hull.
			matrix3x4_t boneEditDelta;
			if ( GetAccumulatedBoneEditDelta( globalBone, boneEditDelta ) )
			{
				matrix3x4_t boneEditDeltaInv;
				MatrixInvert( boneEditDelta, boneEditDeltaInv );
				for ( int vi = 0; vi < verts.Count(); vi++ )
				{
					Vector corrected;
					VectorTransform( verts[vi], boneEditDeltaInv, corrected );
					verts[vi] = corrected;
				}
			}

			if ( verts.Count() < 4 || tris.Count() < 3 )
			{
				MdlError( "$generatejoint: no geometry on bone '%s' in '%s' passes cullweight %.2f.\n"
					"Lower cullweight, or check that '%s' has geometry weighted to '%s'.\n",
					req.bone, req.rendermesh, req.weightThreshold, req.rendermesh, req.bone );
				return;
			}

			CUtlVector<DecomposedHull> hulls;
			if ( !DecomposeConvex( verts, tris, req.concavity, req.maxHulls, req.maxVerts, hulls ) )
			{
				MdlError( "$generatejoint: convex decomposition failed for bone '%s' (mesh '%s').\n"
					"Try a higher concavity or simpler/thicker source geometry.\n",
					req.bone, req.rendermesh );
				return;
			}
			if ( hulls.Count() > m_maxConvex )
				MdlWarning( "COSTLY GENERATED COLLISION!!!! ('%s': %d parts - %d allowed)\n",
					req.bone, hulls.Count(), m_maxConvex );

			// Attach to the joint's existing collision model, or create one.
			const char *pBoneName = pSrc->localBone[localBone].name;
			CPhysCollisionModel *pPhys = GetCollisionModel( pBoneName );
			bool bNew = ( pPhys == NULL );
			if ( bNew )
			{
				pPhys = InitCollisionModel( pBoneName );
				pPhys->m_mass   = 1.0f;
				pPhys->m_name   = pBoneName;
				int parent = pSrc->localBone[localBone].parent;
				pPhys->m_parent = ( parent >= 0 ) ? pSrc->localBone[parent].name : NULL;
			}
			else if ( pPhys->m_pCollisionData )
			{
				// The bone already has collision from the source mesh.  Generating
				// for it replaces that collision (handy for a shared base collision
				// SMD with per-character deviations).  Free the old collide first.
				physcollision->DestroyCollide( pPhys->m_pCollisionData );
				pPhys->m_pCollisionData = NULL;
				if ( !g_StudioMdlContext.quiet )
					printf( "$generatejoint: bone '%s' - replacing source collision with generated\n", pBoneName );
			}

			int nAttached = AttachDecomposedHulls( pPhys, hulls, (unsigned int)( globalBone + 1 ) );

			// A solid with no collision data produces a malformed .phy that crashes
			// the engine on load.  If nothing survived, halt so the user can fix the
			// source mesh rather than ship a broken ragdoll.
			if ( nAttached == 0 )
			{
				if ( bNew )
					delete pPhys;
				MdlError( "$generatejoint: bone '%s' produced no valid collision (all pieces rejected by the physics packer).\n"
					"Simplify/thicken the source mesh, raise concavity, or lower maxverts.\n",
					pBoneName );
				return;
			}

			if ( bNew )
				AppendCollisionModel( pPhys );

			if ( !g_StudioMdlContext.quiet )
			{
				if ( req.children.Count() )
					printf( "$generatejoint %-20s (%d convex elements from '%s', +%d child bone(s))\n",
						pBoneName, nAttached, req.rendermesh, req.children.Count() );
				else
					printf( "$generatejoint %-20s (%d convex elements from '%s')\n",
						pBoneName, nAttached, req.rendermesh );
			}
		}
		else
		{
			// Single body: all faces in model/world space, one rigid solid.
			CUtlVector<bonecull_t> keepAll;  // unused when bKeepAll is true
			ExtractTriangleMesh( pSrc, true, keepAll, verts, tris,
				[&]( uint32_t vi ) -> Vector {
					Vector raw = pSrc->vertex[vi].position;
					Vector shifted; VectorSubtract( raw, pSrc->adjust, shifted );
					matrix3x4_t originXform; AngleMatrix( pSrc->rotation, originXform );
					Vector out; VectorRotate( shifted, originXform, out );
					return out;
				} );

			if ( verts.Count() < 4 || tris.Count() < 3 )
			{
				MdlError( "$generate: no usable geometry in $rendermesh '%s'.\n", req.rendermesh );
				return;
			}

			CUtlVector<DecomposedHull> hulls;
			if ( !DecomposeConvex( verts, tris, req.concavity, req.maxHulls, req.maxVerts, hulls ) )
			{
				MdlError( "$generate: convex decomposition failed for '%s'.\n"
					"Try a higher concavity or simpler/thicker source geometry.\n", req.rendermesh );
				return;
			}
			if ( hulls.Count() > m_maxConvex )
				MdlWarning( "COSTLY GENERATED COLLISION!!!! ('%s': %d parts - %d allowed)\n",
					req.rendermesh, hulls.Count(), m_maxConvex );

			CPhysCollisionModel *pPhys = new CPhysCollisionModel;
			SetCollisionModelDefaults( pPhys );
			int nAttached = AttachDecomposedHulls( pPhys, hulls, 0 );
			if ( nAttached == 0 || !pPhys->m_pCollisionData )
			{
				delete pPhys;
				MdlError( "$generate: '%s' produced no valid collision (all pieces rejected by the physics packer).\n"
					"Simplify/thicken the source mesh, raise concavity, or lower maxverts.\n", req.rendermesh );
				return;
			}
			pPhys->m_mass = 1.0f;

			char tmp[512];
			Q_FileBase( pSrc->filename, tmp, sizeof(tmp) );
			char *out = new char[strlen(tmp)+1];
			strcpy( out, tmp );
			pPhys->m_name   = out;
			pPhys->m_parent = NULL;
			AppendCollisionModel( pPhys );

			if ( !g_StudioMdlContext.quiet )
				printf( "$generate: '%s' -> %d convex sub-parts\n", req.rendermesh, nAttached );
		}
	}
}

//=============================================================================
// QC command parsing helpers
//=============================================================================

#define MAX_ARGS 16
#define ARG_SIZE 256

static int ReadArgs( char pArgs[][ARG_SIZE], int maxCount )
{
	int argCount = 0;
	while ( argCount < maxCount && TokenAvailable() )
	{
		GetToken( false );
		strncpy( pArgs[argCount], token, ARG_SIZE - 1 );
		pArgs[argCount][ARG_SIZE-1] = '\0';
		argCount++;
	}
	return argCount;
}

static float Safe_atof( const char *p ) { return p ? (float)atof(p) : 0.0f; }
static int   Safe_atoi( const char *p ) { return p ? atoi(p)        : 0;    }

static void CCmd_JointConstrain( CJointedModel &joints, const char *pJointName, const char *pJointAxis, const char *pJointType, const char *pLimitMin, const char *pLimitMax, const char *pFriction )
{
	float limitMin  = Safe_atof(pLimitMin);
	float limitMax  = Safe_atof(pLimitMax);
	float friction  = Safe_atof(pFriction);
	int   axis      = -1;
	int   jointIndex = joints.FindLocalBoneNamed( pJointName );
	if ( !g_StudioMdlContext.createMakefile && jointIndex < 0 )
	{
		MdlWarning( "Can't find joint %s\n", pJointName );
		return;
	}
	pJointName = joints.m_pModel->localBone[jointIndex].name;
	if ( pJointAxis ) axis = tolower(pJointAxis[0]) - 'x';
	if ( axis < 0 || axis > 2 || limitMin > limitMax )
	{
		MdlError( "Invalid joint constraint for %s\nCan't build ragdoll!\n", pJointName );
		return;
	}

	jointlimit_t jointType = JOINT_FREE;
	if      ( !stricmp( pJointType, "free"  ) ) jointType = JOINT_FREE;
	else if ( !stricmp( pJointType, "fixed" ) ) jointType = JOINT_FIXED;
	else if ( !stricmp( pJointType, "limit" ) ) jointType = JOINT_LIMIT;
	else { MdlWarning( "Unknown joint type %s (must be free, fixed, or limit)\n", pJointType ); return; }

	joints.AddConstraint( pJointName, axis, jointType, limitMin, limitMax, friction );
}

static void CCmd_JointSkip( CJointedModel &joints, const char *pName )
{
	int boneIndex = joints.FindLocalBoneNamed( pName );
	if ( boneIndex < 0 ) MdlWarning( "Can't skip joint %s, not found\n", pName );
	else                 joints.SkipBone( boneIndex );
}

static void CCmd_TotalMass( CJointedModel &joints, const char *pMass )       { joints.SetTotalMass( Safe_atof(pMass) ); }
static void CCmd_JointRoot( CJointedModel &joints, const char *pBone )       { strcpy( joints.m_rootName, RenameBone( pBone ) ); }
static void CCmd_JointMerge( CJointedModel &joints, const char *pParent, const char *pChild )
{
	joints.AddMergeCommand( pParent, pChild );
	joints.MergeBones( pParent, pChild );
}

static void CCmd_JoinAnimatedFriction( CJointedModel &joints, const char *pMin, const char *pMax, const char *pTimeIn, const char *pTimeHold, const char *pTimeOut )
{
	joints.m_flFrictionTimeIn    = Safe_atof( pTimeIn );
	joints.m_flFrictionTimeOut   = Safe_atof( pTimeOut );
	joints.m_flFrictionTimeHold  = Safe_atof( pTimeHold );
	joints.m_iMinAnimatedFriction = Safe_atoi( pMin );
	joints.m_iMaxAnimatedFriction = Safe_atoi( pMax );
	joints.m_bHasAnimatedFriction = true;
}

//-----------------------------------------------------------------------------
// Parse a $generate / $generatejoint request.
//
//   $generate       <rendermesh> [concavity <f>] [hull <i>]
//   $generatejoint  <bone> <rendermesh> [concavity <f>] [hull <i>] [cullweight <f>]
//
// All options are keyword/value pairs, optional, in any order, with defaults.
// Returns true if the required positional args were present.
//-----------------------------------------------------------------------------
static bool ParseGenerateRequest( CJointedModel::generate_request_t &req, bool bJoint )
{
	// Defaults.
	req.bone[0]          = '\0';
	req.rendermesh[0]    = '\0';
	req.concavity        = 0.04f;
	req.maxHulls         = 1;
	req.maxVerts         = 16;
	req.weightThreshold  = 0.42f;

	const char *pCmd = bJoint ? "$generatejoint" : "$generate";

	// Required positional args.
	if ( bJoint )
	{
		if ( !GetToken( false ) ) { MdlWarning( "%s: expected <bone>\n", pCmd ); return false; }
		V_strncpy( req.bone, token, sizeof(req.bone) );
	}
	if ( !GetToken( false ) ) { MdlWarning( "%s: expected <rendermesh>\n", pCmd ); return false; }
	V_strncpy( req.rendermesh, token, sizeof(req.rendermesh) );

	// Mark the render mesh as used now, at parse time.  Actual resolution happens later
	// in ProcessGenerateRequests(), after ReportUnusedRenderMeshDefs() has already run,
	// so without this the mesh would be falsely reported as defined-but-never-used.
	MarkRenderMeshUsed( req.rendermesh );

	// Optional keyword/value pairs.  Stop at the next token that is not one of our
	// keywords (it belongs to the enclosing block) and put it back.
	while ( TokenAvailable() )
	{
		if ( !GetToken( false ) ) break;

		if ( !stricmp( token, "concavity" ) )
		{
			if ( GetToken( false ) ) req.concavity = (float)atof( token );
		}
		else if ( !stricmp( token, "hull" ) || !stricmp( token, "hulls" ) )
		{
			if ( GetToken( false ) ) req.maxHulls = atoi( token );
		}
		else if ( !stricmp( token, "maxverts" ) || !stricmp( token, "verts" ) )
		{
			if ( GetToken( false ) ) req.maxVerts = atoi( token );
		}
		else if ( bJoint && ( !stricmp( token, "cullweight" ) || !stricmp( token, "cull" ) ) )
		{
			if ( GetToken( false ) ) req.weightThreshold = (float)atof( token );
		}
		else
		{
			// Not ours - hand it back to the block parser.
			UnGetToken();
			break;
		}
	}

	if ( req.maxHulls < 1 ) req.maxHulls = 1;
	if ( req.maxHulls > 8 ) req.maxHulls = 8;
	if ( req.maxVerts < 4 )   req.maxVerts = 4;
	if ( req.maxVerts > 128 ) req.maxVerts = 128;
	return true;
}

static void ParseCollisionCommands( CJointedModel &joints )
{
	char command[512];
	char args[MAX_ARGS][ARG_SIZE];
	int  argCount;

	g_ConvexHullCountOverride = false;

	while ( GetToken( true ) )
	{
		if ( !strcmp( token, "}" ) ) return;
		strcpy( command, token );

		if ( !stricmp( command, "$mass" ) )
		{
			argCount = ReadArgs(args, 1);
			CCmd_TotalMass( joints, args[0] );
		}
		else if ( !stricmp( command, "$automass" ) || !stricmp( command, "$calculatemass" ) )
		{
			joints.SetAutoMass();
		}
		else if ( !stricmp( command, "$inertia" ) )
		{
			argCount = ReadArgs(args, 1);
			joints.DefaultInertia( Safe_atof(args[0]) );
		}
		else if ( !stricmp( command, "$damping" ) )
		{
			argCount = ReadArgs(args, 1);
			joints.DefaultDamping( Safe_atof(args[0]) );
		}
		else if ( !stricmp( command, "$rotdamping" ) )
		{
			argCount = ReadArgs(args, 1);
			joints.DefaultRotdamping( Safe_atof(args[0]) );
		}
		else if ( !stricmp( command, "$drag" ) )
		{
			argCount = ReadArgs(args, 1);
			joints.DefaultDrag( Safe_atof(args[0]) );
		}
		else if ( !stricmp( command, "$maxconvexpieces" ) )
		{
			argCount = ReadArgs(args, 1);
			joints.SetMaxConvex( Safe_atoi(args[0]) );
		}
		else if ( !stricmp( command, "$remove2d" ) )
		{
			joints.Remove2DConvex();
		}
		else if ( !stricmp( command, "$concaveperjoint" ) )
		{
			joints.AllowConcaveJoints();
		}
		else if ( !stricmp( command, "$weldposition" ) )
		{
			argCount = ReadArgs(args, 1);
			g_WeldVertEpsilon = Safe_atof( args[0] );
		}
		else if ( !stricmp( command, "$weldnormal" ) )
		{
			argCount = ReadArgs(args, 1);
			g_WeldNormalEpsilon = Safe_atof( args[0] );
		}
		else if ( !stricmp( command, "$concave" ) )
		{
			joints.AllowConcave();
		}
		else if ( !stricmp( command, "$convexhullcountoverride" ) )
		{
			ReadArgs(args, 1);
			g_ConvexHullCountOverride = true;
		}
		else if ( !stricmp( command, "$masscenter" ) )
		{
			argCount = ReadArgs(args, 3);
			Vector center;
			center.Init( Safe_atof(args[0]), Safe_atof(args[1]), Safe_atof(args[2]) );
			joints.ForceMassCenter( center );
		}
		else if ( !stricmp( command, "$jointskip" ) )
		{
			argCount = ReadArgs(args, 1);
			CCmd_JointSkip( joints, args[0] );
		}
		else if ( !stricmp( command, "$jointmerge" ) )
		{
			argCount = ReadArgs(args, 2);
			CCmd_JointMerge( joints, args[0], args[1] );
		}
		else if ( !stricmp( command, "$rootbone" ) )
		{
			argCount = ReadArgs(args, 1);
			CCmd_JointRoot( joints, args[0] );
		}
		else if ( !stricmp( command, "$jointconstrain" ) )
		{
			argCount = ReadArgs(args, 6);
			CCmd_JointConstrain( joints, args[0], args[1], args[2], args[3], args[4], (argCount < 6) ? "1.0" : args[5] );
		}
		else if ( !stricmp( command, "$jointinertia" ) )
		{
			argCount = ReadArgs(args, 2);
			joints.JointInertia( args[0], Safe_atof(args[1]) );
		}
		else if ( !stricmp( command, "$jointdamping" ) )
		{
			argCount = ReadArgs(args, 2);
			joints.JointDamping( args[0], Safe_atof(args[1]) );
		}
		else if ( !stricmp( command, "$jointrotdamping" ) )
		{
			argCount = ReadArgs(args, 2);
			joints.JointRotdamping( args[0], Safe_atof(args[1]) );
		}
		else if ( !stricmp( command, "$jointmassbias" ) )
		{
			argCount = ReadArgs(args, 2);
			joints.JointMassBias( args[0], Safe_atof(args[1]) );
		}
		else if ( !stricmp( command, "$noselfcollisions" ) )
		{
			joints.SetNoSelfCollisions();
		}
		else if ( !stricmp( command, "$jointcollide" ) )
		{
			argCount = ReadArgs(args, 2);
			joints.AppendCollisionPair( args[0], args[1] );
		}
		else if ( !stricmp( command, "$animatedfriction" ) )
		{
			argCount = ReadArgs(args, 5);
			if ( argCount == 5 )
				CCmd_JoinAnimatedFriction( joints, args[0], args[1], args[2], args[3], args[4] );
		}
		else if ( !stricmp( command, "$assumeworldspace" ) )
		{
			joints.m_bAssumeWorldspace = true;
		}
		else if ( !stricmp( command, "$addconvexsrc" ) )
		{
			argCount = ReadArgs(args, 1);
			joints.AddConvexSrc( args[0] );
		}
		else if ( !stricmp( command, "$jointcollidealltoall" ) )
		{
			char szTempNames[32][256];
			int nNumEntries = 0;
			GetToken( true );
			if ( token[0] == '{' )
			{
				while ( GetToken(true) && nNumEntries < 32 && strcmp(token, "}") )
				{
					V_strcpy_safe( szTempNames[nNumEntries], token );
					nNumEntries++;
				}
			}
			for ( int i = 0; i < nNumEntries; i++ )
				for ( int j = 0; j < nNumEntries; j++ )
					if ( i != j ) joints.AppendCollisionPair( szTempNames[i], szTempNames[j] );
		}
		else if ( !stricmp( command, "$jointnocollide" ) )
		{
			argCount = ReadArgs(args, 2);
			joints.RemoveCollisionPair( args[0], args[1] );
			joints.RemoveCollisionPair( args[1], args[0] );
		}
		else if ( !stricmp( command, "$generate" ) )
		{
			// $generate <rendermesh> [concavity <f>] [hull <i>]
			// Build in place: generate_request_t holds a CUtlVector and is not copyable.
			int idx = joints.m_generateRequests.AddToTail();
			if ( !ParseGenerateRequest( joints.m_generateRequests[idx], false ) )
				joints.m_generateRequests.Remove( idx );
		}
		else if ( !stricmp( command, "$generatejoint" ) )
		{
			// $generatejoint <bone> <rendermesh> [concavity <f>] [hull <i>] [cullweight <f>]
			int idx = joints.m_generateRequests.AddToTail();
			if ( !ParseGenerateRequest( joints.m_generateRequests[idx], true ) )
				joints.m_generateRequests.Remove( idx );
		}
		else if ( !stricmp( command, "$addgeneratechild" ) )
		{
			// $addgeneratechild <generatejoint bone> <child bone> [cullweight <f>]
			// Welds the child bone's weighted geometry into the named joint's hull.
			CJointedModel::pending_child_t pc;
			pc.weightThreshold = -1.0f;  // inherit the joint's by default
			if ( !GetToken( false ) )
			{
				MdlWarning( "$addgeneratechild: expected <generatejoint bone>\n" );
			}
			else
			{
				V_strncpy( pc.jointBone, token, sizeof(pc.jointBone) );
				if ( !GetToken( false ) )
				{
					MdlWarning( "$addgeneratechild: expected <child bone>\n" );
				}
				else
				{
					V_strncpy( pc.childBone, token, sizeof(pc.childBone) );
					while ( TokenAvailable() && GetToken( false ) )
					{
						if ( !stricmp( token, "cullweight" ) || !stricmp( token, "cull" ) )
						{
							if ( GetToken( false ) ) pc.weightThreshold = (float)atof( token );
						}
						else { UnGetToken(); break; }
					}
					joints.m_pendingChildren.AddToTail( pc );
				}
			}
		}
		else
		{
			MdlWarning( "Unknown command %s in collision series\n", command );
		}
	}
}

void Cmd_CollisionText()
{
	int level = 1;
	if ( !GetToken(true) ) return;
	if ( token[0] != '{' ) return;
	while ( GetToken(true) )
	{
		if ( !strcmp( token, "}" ) )
		{
			level--;
			if ( level <= 0 ) break;
			g_JointedModel.AddText( " }\n" );
		}
		else if ( !strcmp( token, "{" ) )
		{
			g_JointedModel.AddText( "{" );
			level++;
		}
		else
		{
			if ( level > 1 )
			{
				g_JointedModel.AddText( "\"" );
				g_JointedModel.AddText( token );
				g_JointedModel.AddText( "\" " );
			}
			else
			{
				g_JointedModel.AddText( token );
				g_JointedModel.AddText( " " );
			}
		}
	}
}

//=============================================================================
// Surface properties
//=============================================================================

static bool LoadSurfaceProps( const char *pMaterialFilename )
{
	if ( !physprops ) return false;

	FileHandle_t fp = g_pFileSystem->Open( pMaterialFilename, "rb", TOOLS_READ_PATH_ID );
	if ( fp == FILESYSTEM_INVALID_HANDLE ) return false;

	int len = g_pFileSystem->Size( fp );
	char *pText = new char[len+1];
	g_pFileSystem->Read( pText, len, fp );
	g_pFileSystem->Close( fp );
	pText[len] = 0;

	physprops->ParseSurfaceData( pMaterialFilename, pText );
	delete[] pText;
	return true;
}

static void LoadSurfacePropsAll()
{
	static bool bIsLoaded = false;
	if ( bIsLoaded ) return;

	const char *SURFACEPROP_MANIFEST_FILE = "scripts/surfaceproperties_manifest.txt";
	KeyValues *manifest = new KeyValues( SURFACEPROP_MANIFEST_FILE );
	if ( manifest->LoadFromFile( g_pFileSystem, SURFACEPROP_MANIFEST_FILE, "GAME" ) )
	{
		bIsLoaded = true;
		for ( KeyValues *sub = manifest->GetFirstSubKey(); sub; sub = sub->GetNextKey() )
		{
			if ( !Q_stricmp( sub->GetName(), "file" ) )
				LoadSurfaceProps( sub->GetString() );
		}
	}
	manifest->deleteThis();
}

//=============================================================================
// DoCollisionModel: entry point called from Cmd_CollisionModel / Cmd_CollisionJoints
//=============================================================================
int DoCollisionModel( bool separateJoints )
{
	char name[512];

	if ( !GetToken(false) ) return 0;
	strcpyn( name, token );

	// Use built-in collision implementation - no external vphysics.dll required.
	if ( !physcollision )
	{
		physcollision = CreateMiniPhysicsCollision();
		physprops     = CreateMiniPhysicsSurfaceProps();
		LoadSurfacePropsAll();
	}

	int nummaterials = g_nummaterials;
	int numtextures  = g_numtextures;

	if ( !V_strcmp( name, "blank" ) )
	{
		g_JointedModel.m_bRootCollisionIsEmpty = true;
	}
	else
	{
		s_source_t *pmodel = Load_Source( name, "", false, false, false );
		if ( !pmodel ) return 0;

		if ( nummaterials && numtextures && (numtextures != g_numtextures || nummaterials != g_nummaterials) )
		{
			g_numtextures  = numtextures;
			g_nummaterials = nummaterials;
			pmodel->texmap[0] = 0;
		}
		g_JointedModel.SetSource( pmodel );
	}

	bool parseCommands = false;
	if ( GetToken(true) )
	{
		if ( !strcmp( token, "{" ) ) parseCommands = true;
		else                         UnGetToken();
	}

	if ( parseCommands )
		ParseCollisionCommands( g_JointedModel );

	g_JointedModel.m_isJointed = separateJoints;
	return 1;
}

//=============================================================================
// CollisionModel_Build: called after SimplifyModel(), before WriteModelFiles()
//=============================================================================
void CollisionModel_Build()
{
	bool bHasGenerate = g_JointedModel.m_generateRequests.Count() > 0;
	if ( !g_JointedModel.m_pModel && !g_JointedModel.m_bRootCollisionIsEmpty && !bHasGenerate )
		return;

	g_JointedModel.Simplify();

	// Determine whether there is any real source geometry to build from (a loaded
	// model or an $addconvexsrc extra model).  A pure-generation block
	// ($collisionmodel "blank" { $generate ... }) has neither and skips the normal
	// source build, going straight to ProcessGenerateRequests.
	bool bHasSource = g_JointedModel.m_pModel != NULL;
	for ( int i = 0; !bHasSource && i <= MAX_EXTRA_COLLISION_MODELS; i++ )
		if ( g_JointedModel.m_ExtraModels[i].m_pSrc ) bHasSource = true;

	if ( bHasSource )
	{
		if ( g_JointedModel.m_isJointed )
			g_JointedModel.ProcessJointedModel();
		else
			g_JointedModel.ProcessSingleBody();
	}

	g_JointedModel.ProcessGenerateRequests();

	g_JointedModel.ApplyPendingJointOverrides();
	g_JointedModel.FixCollisionHierarchy();
	g_JointedModel.ComputeMass();
}

//=============================================================================
// FixCollisionHierarchy: fix parent links after bone collapse
//=============================================================================
void CJointedModel::FixCollisionHierarchy()
{
	if ( !m_pCollisionList ) return;

	FixBoneList();

	CPhysCollisionModel *pPhys = m_pCollisionList;
	for ( ; pPhys; pPhys = pPhys->m_pNext )
		pPhys->m_parent = FixParent( pPhys->m_parent );

	SortCollisionList();

	CJointConstraint *pList = m_pConstraintList;
	while ( pList )
	{
		pList->m_pJointName = FixParent( pList->m_pJointName );
		pList = pList->m_pNext;
	}

	pPhys = m_pCollisionList;
	for ( int i = 0; i < g_StudioMdlContext.numbones; i++ )
		g_bonetable[i].physicsBoneIndex = -1;

	int index = 0;
	while ( pPhys )
	{
		int boneIndex = FindBoneInTable( pPhys->m_name );
		if ( boneIndex >= 0 )
			g_bonetable[boneIndex].physicsBoneIndex = index;
		pPhys = pPhys->m_pNext;
		index++;
	}

	for ( int i = 0; i < g_StudioMdlContext.numbones; i++ )
	{
		if ( g_bonetable[i].physicsBoneIndex < 0 )
		{
			int idx = g_bonetable[i].parent;
			int bone = -1;
			while ( idx >= 0 )
			{
				bone = g_bonetable[idx].physicsBoneIndex;
				if ( bone >= 0 ) break;
				idx = g_bonetable[idx].parent;
			}
			g_bonetable[i].physicsBoneIndex = (bone >= 0) ? bone : 0;
		}
	}
}

//=============================================================================
// Ragdoll constraint builder
//=============================================================================
static void BuildRagdollConstraint( CPhysCollisionModel *pPhys, constraint_ragdollparams_t &ragdoll )
{
	memset( &ragdoll, 0, sizeof(ragdoll) );
	ragdoll.parentIndex = g_JointedModel.CollisionIndex( pPhys->m_parent );
	ragdoll.childIndex  = g_JointedModel.CollisionIndex( pPhys->m_name );

	if ( ragdoll.parentIndex < 0 || ragdoll.childIndex < 0 )
	{
		if ( ragdoll.childIndex  < 0 ) MdlWarning( "\"%s\" does not appear in collision model!!!\n", pPhys->m_name );
		if ( ragdoll.parentIndex < 0 ) MdlWarning( "\"%s\" does not appear in collision model!!!\n", pPhys->m_parent );
		MdlError( "Bad constraint in ragdoll\n" );
	}

	CJointConstraint *pList = g_JointedModel.m_pConstraintList;
	while ( pList )
	{
		int idx = g_JointedModel.CollisionIndex( pList->m_pJointName );
		CPhysCollisionModel *pListModel = g_JointedModel.GetCollisionModel( pList->m_pJointName );
		if ( idx < 0 )
		{
			MdlError( "Rotation constraint on bone \"%s\" which does not appear in collision model!!!\n", pList->m_pJointName );
		}
		else if ( (!pListModel->m_parent || g_JointedModel.CollisionIndex(pListModel->m_parent) < 0) && stricmp( pList->m_pJointName, g_JointedModel.m_rootName ) )
		{
			MdlError( "Rotation constraint on bone \"%s\" which has no parent!!!\n", pList->m_pJointName );
		}
		else if ( idx == ragdoll.childIndex )
		{
			switch ( pList->m_jointType )
			{
			case JOINT_LIMIT:  ragdoll.axes[pList->m_axis].SetAxisFriction( pList->m_limitMin, pList->m_limitMax, pList->m_friction ); break;
			case JOINT_FIXED:  ragdoll.axes[pList->m_axis].SetAxisFriction( 0, 0, 0 ); break;
			case JOINT_FREE:   ragdoll.axes[pList->m_axis].SetAxisFriction( -360, 360, pList->m_friction ); break;
			}
		}
		pList = pList->m_pNext;
	}
}

//=============================================================================
// CollisionModel_ExpandBBox / CollisionModel_SetName / GetCollisionModelMass
//=============================================================================

void CollisionModel_ExpandBBox( Vector &mins, Vector &maxs )
{
	if ( g_JointedModel.m_isJointed ) return;
	if ( g_JointedModel.m_pCollisionList )
	{
		Vector collideMins, collideMaxs;
		physcollision->CollideGetAABB( &collideMins, &collideMaxs, g_JointedModel.m_pCollisionList->m_pCollisionData, vec3_origin, vec3_angle );
		const float radius = 0.25f;
		collideMins -= Vector(radius,radius,radius);
		collideMaxs += Vector(radius,radius,radius);
		AddPointToBounds( collideMins, mins, maxs );
		AddPointToBounds( collideMaxs, mins, maxs );
	}
}

void CollisionModel_SetName( const char *pName )
{
	g_JointedModel.SetOverrideName( pName );
}

float GetCollisionModelMass()
{
	return g_JointedModel.m_totalMass;
}

//=============================================================================
// KeyValue write helpers
//=============================================================================
static void KeyWriteInt( FILE *fp, const char *pKey, int v )                   { fprintf( fp, "\"%s\" \"%d\"\n",          pKey, v );           }
static void KeyWriteIntPair( FILE *fp, const char *pKey, int v0, int v1 )      { fprintf( fp, "\"%s\" \"%d,%d\"\n",       pKey, v0, v1 );      }
static void KeyWriteString( FILE *fp, const char *pKey, const char *v )        { fprintf( fp, "\"%s\" \"%s\"\n",          pKey, v );           }
static void KeyWriteFloat( FILE *fp, const char *pKey, float v )               { fprintf( fp, "\"%s\" \"%f\"\n",          pKey, v );           }

static float TotalVolume( CPhysCollisionModel *pList )
{
	float vol = 0;
	while ( pList ) { vol += pList->m_volume * pList->m_massBias; pList = pList->m_pNext; }
	return vol;
}

//=============================================================================
// CollisionModel_Write: called from WriteModelFiles() with the MDL checksum
//=============================================================================
void CollisionModel_Write( long checkSum )
{
	if ( !g_JointedModel.m_pCollisionList ) return;

	CPhysCollisionModel *pPhys = g_JointedModel.m_pCollisionList;

	char filename[512];
	strcpy( filename, GetModelOutputDir() );
	strcat( filename, "models/" );
	strcat( filename, g_JointedModel.m_pOverrideName ? g_JointedModel.m_pOverrideName : g_outname );

	float volume = TotalVolume( pPhys );
	if ( volume <= 0 ) volume = 1;
	if ( !g_StudioMdlContext.quiet )
		printf( "Collision model volume %.2f in^3\n", volume );

	Q_SetExtension( filename, ".phy", sizeof(filename) );

	FILE *fp = fopen( filename, "wb" );
	if ( !fp )
	{
		MdlWarning( "Error writing %s!!!\n", filename );
		return;
	}

	// Write header
	phyheader_t header;
	header.size      = sizeof(header);
	header.id        = 0;
	header.checkSum  = checkSum;
	header.solidCount = 0;
	pPhys = g_JointedModel.m_pCollisionList;
	while ( pPhys ) { header.solidCount++; pPhys = pPhys->m_pNext; }
	fwrite( &header, sizeof(header), 1, fp );

	// Write binary collision blobs
	pPhys = g_JointedModel.m_pCollisionList;
	while ( pPhys )
	{
		int size = physcollision->CollideSize( pPhys->m_pCollisionData );
		fwrite( &size, sizeof(int), 1, fp );
		char *buf = (char *)malloc(size);
		physcollision->CollideWrite( buf, pPhys->m_pCollisionData );
		fwrite( buf, size, 1, fp );
		free( buf );
		pPhys = pPhys->m_pNext;
	}

	// Write solid KeyValue sections
	int solidIndex = 0;
	pPhys = g_JointedModel.m_pCollisionList;
	while ( pPhys )
	{
		pPhys->m_mass = ((pPhys->m_volume * pPhys->m_massBias) / volume) * g_JointedModel.m_totalMass;
		if ( pPhys->m_mass < 1.0f ) pPhys->m_mass = 1.0f;

		fprintf( fp, "solid {\n" );
		KeyWriteInt( fp, "index", solidIndex );
		KeyWriteString( fp, "name", pPhys->m_name );
		if ( pPhys->m_parent )
			KeyWriteString( fp, "parent", pPhys->m_parent );
		KeyWriteFloat( fp, "mass", pPhys->m_mass );

		char *pSurfaceProps = GetSurfaceProp( pPhys->m_name );
		KeyWriteString( fp, "surfaceprop", pSurfaceProps );
		KeyWriteFloat( fp, "damping",    pPhys->m_damping );
		KeyWriteFloat( fp, "rotdamping", pPhys->m_rotdamping );
		if ( pPhys->m_dragCoefficient != -1 )
			KeyWriteFloat( fp, "drag", pPhys->m_dragCoefficient );
		KeyWriteFloat( fp, "inertia", pPhys->m_inertia );
		KeyWriteFloat( fp, "volume",  pPhys->m_volume );
		if ( pPhys->m_massBias != 1.0f )
			KeyWriteFloat( fp, "massbias", pPhys->m_massBias );
		fprintf( fp, "}\n" );

		pPhys = pPhys->m_pNext;
		solidIndex++;
	}

	// Write ragdoll constraints
	pPhys = g_JointedModel.m_pCollisionList;
	while ( pPhys )
	{
		if ( pPhys->m_parent )
		{
			constraint_ragdollparams_t ragdoll;
			BuildRagdollConstraint( pPhys, ragdoll );
			if ( ragdoll.parentIndex != ragdoll.childIndex )
			{
				fprintf( fp, "ragdollconstraint {\n" );
				KeyWriteInt(   fp, "parent",    ragdoll.parentIndex );
				KeyWriteInt(   fp, "child",     ragdoll.childIndex  );
				KeyWriteFloat( fp, "xmin",      ragdoll.axes[0].minRotation );
				KeyWriteFloat( fp, "xmax",      ragdoll.axes[0].maxRotation );
				KeyWriteFloat( fp, "xfriction", ragdoll.axes[0].torque );
				KeyWriteFloat( fp, "ymin",      ragdoll.axes[1].minRotation );
				KeyWriteFloat( fp, "ymax",      ragdoll.axes[1].maxRotation );
				KeyWriteFloat( fp, "yfriction", ragdoll.axes[1].torque );
				KeyWriteFloat( fp, "zmin",      ragdoll.axes[2].minRotation );
				KeyWriteFloat( fp, "zmax",      ragdoll.axes[2].maxRotation );
				KeyWriteFloat( fp, "zfriction", ragdoll.axes[2].torque );
				fprintf( fp, "}\n" );
			}
		}
		pPhys = pPhys->m_pNext;
	}

	// Collision rules
	if ( g_JointedModel.m_noSelfCollisions )
	{
		fprintf( fp, "collisionrules {\n" );
		KeyWriteInt( fp, "selfcollisions", 0 );
		fprintf( fp, "}\n" );
	}
	else if ( g_JointedModel.m_pCollisionPairs )
	{
		fprintf( fp, "collisionrules {\n" );
		collisionpair_t *pPair = g_JointedModel.m_pCollisionPairs;
		while ( pPair )
		{
			pPair->obj0 = g_JointedModel.CollisionIndex( pPair->pName0 );
			pPair->obj1 = g_JointedModel.CollisionIndex( pPair->pName1 );
			if ( pPair->obj0 >= 0 && pPair->obj1 >= 0 && pPair->obj0 != pPair->obj1 )
				KeyWriteIntPair( fp, "collisionpair", pPair->obj0, pPair->obj1 );
			else
				MdlWarning( "Invalid collision pair (%s, %s)\n", pPair->pName0, pPair->pName1 );
			pPair = pPair->pNext;
		}
		fprintf( fp, "}\n" );
	}

	// Animated friction
	if ( g_JointedModel.m_bHasAnimatedFriction )
	{
		fprintf( fp, "animatedfriction {\n" );
		KeyWriteFloat( fp, "animfrictionmin",      (float)g_JointedModel.m_iMinAnimatedFriction );
		KeyWriteFloat( fp, "animfrictionmax",      (float)g_JointedModel.m_iMaxAnimatedFriction );
		KeyWriteFloat( fp, "animfrictiontimein",   g_JointedModel.m_flFrictionTimeIn  );
		KeyWriteFloat( fp, "animfrictiontimeout",  g_JointedModel.m_flFrictionTimeOut );
		KeyWriteFloat( fp, "animfrictiontimehold", g_JointedModel.m_flFrictionTimeHold );
		fprintf( fp, "}\n" );
	}

	// Editor params
	fprintf( fp, "editparams {\n" );
	KeyWriteString( fp, "rootname",  g_JointedModel.m_rootName );
	KeyWriteFloat(  fp, "totalmass", g_JointedModel.m_totalMass );
	if ( g_JointedModel.m_allowConcave )
		KeyWriteInt( fp, "concave", 1 );
	for ( int k = 0; k < g_JointedModel.m_mergeList.Count(); k++ )
	{
		char buf[512];
		Q_snprintf( buf, sizeof(buf), "%s,%s", RenameBone( g_JointedModel.m_mergeList[k].pParent ), RenameBone( g_JointedModel.m_mergeList[k].pChild ) );
		KeyWriteString( fp, "jointmerge", buf );
	}
	fprintf( fp, "}\n" );

	// Inline text commands ($collisiontext)
	if ( g_JointedModel.m_textCommands.Count() )
		fwrite( g_JointedModel.m_textCommands.Base(), g_JointedModel.m_textCommands.Count(), 1, fp );

	char terminator = 0;
	fwrite( &terminator, sizeof(terminator), 1, fp );
	fclose( fp );

	if ( !g_StudioMdlContext.quiet )
		printf( "Writing %s\n", filename );
}
