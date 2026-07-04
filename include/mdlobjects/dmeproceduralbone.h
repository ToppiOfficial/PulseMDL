//===== Copyright (c) 1996-2009, Valve Corporation, All rights reserved. =====
//
// DmeProceduralBone
//
//============================================================================

#ifndef DMEPROCEDURALBONE_H
#define DMEPROCEDURALBONE_H

#if defined( _WIN32 )
#pragma once
#endif


// Valve includes
#include "datamodel/dmattributevar.h"
#include "movieobjects/dmejoint.h"


//-----------------------------------------------------------------------------
// DmeProceduralBone
//
// Base class for skeleton joints that are driven procedurally at runtime
// (jigglebones, QUATINTERP driver bones, AIMATBONE look-at bones). The joint
// itself lives in the skeleton; the derived element carries the procedural
// description that studiomdl folds into the mstudioboneprocedural_t tables.
//-----------------------------------------------------------------------------
class CDmeProceduralBone : public CDmeJoint
{
	DEFINE_ELEMENT( CDmeProceduralBone, CDmeJoint );

public:
};


//-----------------------------------------------------------------------------
// DmeQuatInterpBone
//
// DME equivalent of QC $driverbone / .vrd <helper>: a QUATINTERP procedural
// bone. The joint blends toward a target pose whenever the control ("driver")
// bone's local rotation approaches one of the trigger rotations. The four
// trigger arrays are parallel (one entry per trigger); studiomdl copies them
// into s_quatinterpbone_t.
//-----------------------------------------------------------------------------
class CDmeQuatInterpBone : public CDmeProceduralBone
{
	DEFINE_ELEMENT( CDmeQuatInterpBone, CDmeProceduralBone );

public:
	CDmaString				m_sControlBone;			// driver bone whose rotation is sampled
	CDmaVar< Vector >		m_vBasePos;				// base position added to every target position
	CDmaVar< bool >			m_bUnlockBones;			// treat targets as deltas onto the real bind pose

	CDmaArray< float >		m_Tolerances;			// per-trigger angular tolerance (degrees)
	CDmaArray< Quaternion >	m_TriggerRotations;		// per-trigger driver rotation (when to fire)
	CDmaArray< Vector >		m_TargetPositions;		// per-trigger target position
	CDmaArray< Quaternion >	m_TargetRotations;		// per-trigger target rotation (what to snap to)
};


//-----------------------------------------------------------------------------
// DmeAimAtBone
//
// DME equivalent of QC $driverlookat / .vrd <aimconstraint>: an AIMATBONE
// procedural bone. The joint continuously rotates so one of its local axes
// points at a target bone or attachment. studiomdl copies this into
// s_aimatbone_t.
//-----------------------------------------------------------------------------
class CDmeAimAtBone : public CDmeProceduralBone
{
	DEFINE_ELEMENT( CDmeAimAtBone, CDmeProceduralBone );

public:
	CDmaString			m_sAimTarget;	// bone or attachment aimed at (attachment checked first)
	CDmaString			m_sParentBone;	// optional parent override (empty -> skeleton parent)
	CDmaVar< Vector >	m_vAimVector;	// local axis that points at the target (default 0 0 1)
	CDmaVar< Vector >	m_vUpVector;	// local axis used to resolve roll (default 1 0 0)
	CDmaVar< Vector >	m_vBasePos;		// world-space offset applied to the target position
};


#endif // DMEPROCEDURALBONE_H
