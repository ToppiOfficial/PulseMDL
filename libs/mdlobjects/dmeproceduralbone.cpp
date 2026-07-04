//====== Copyright � 1996-2009, Valve Corporation, All rights reserved. =======
//
// DmeProceduralBone
//
//=============================================================================


// Valve includes
#include "datamodel/dmelementfactoryhelper.h"
#include "mdlobjects/dmeproceduralbone.h"


// memdbgon must be the last include file in a .cpp file!!!
// DISABLED #include "tier0/memdbgon.h"


//-----------------------------------------------------------------------------
// Expose these classes to the scene database
//-----------------------------------------------------------------------------
IMPLEMENT_ELEMENT_FACTORY( DmeProceduralBone, CDmeProceduralBone );
IMPLEMENT_ELEMENT_FACTORY( DmeQuatInterpBone, CDmeQuatInterpBone );
IMPLEMENT_ELEMENT_FACTORY( DmeAimAtBone, CDmeAimAtBone );


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void CDmeProceduralBone::OnConstruction()
{
}


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void CDmeProceduralBone::OnDestruction()
{
}


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void CDmeQuatInterpBone::OnConstruction()
{
	m_sControlBone.Init( this, "controlBone" );
	m_vBasePos.InitAndSet( this, "basePos", Vector( 0.0f, 0.0f, 0.0f ) );
	m_bUnlockBones.InitAndSet( this, "unlockBones", false );

	m_Tolerances.Init( this, "tolerances" );
	m_TriggerRotations.Init( this, "triggerRotations" );
	m_TargetPositions.Init( this, "targetPositions" );
	m_TargetRotations.Init( this, "targetRotations" );
}


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void CDmeQuatInterpBone::OnDestruction()
{
}


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void CDmeAimAtBone::OnConstruction()
{
	m_sAimTarget.Init( this, "aimTarget" );
	m_sParentBone.Init( this, "parentBone" );
	m_vAimVector.InitAndSet( this, "aimVector", Vector( 0.0f, 0.0f, 1.0f ) );
	m_vUpVector.InitAndSet( this, "upVector", Vector( 1.0f, 0.0f, 0.0f ) );
	m_vBasePos.InitAndSet( this, "basePos", Vector( 0.0f, 0.0f, 0.0f ) );
}


//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
void CDmeAimAtBone::OnDestruction()
{
}
