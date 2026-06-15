#ifndef CONVEX_DECOMPOSE_H
#define CONVEX_DECOMPOSE_H
#pragma once

// Backend-isolated convex decomposition bridge.
//
// Wraps a convex-decomposition backend (currently VHACD 4.x) so the rest of the
// compiler never sees backend types.  Given a triangle mesh, returns a set of
// convex hulls expressed only as vertex clouds; callers re-hull each cloud via
// physcollision->ConvexFromVerts (the existing minicollision entry point), so
// the backend's own triangle output is intentionally discarded.

#include "tier1/utlvector.h"
#include "mathlib/vector.h"

// One decomposed convex piece, as a cloud of vertices (model/bone space matches
// whatever space the caller passed its input verts in).
struct DecomposedHull
{
	CUtlVector<Vector> verts;
};

// Decompose a triangle mesh into convex pieces.
//   verts       : mesh vertices
//   triIndices  : triangle list, 3 indices per triangle, indexing into verts
//   concavity   : quality knob in [0..1].  Lower = closer fit / more pieces,
//                 higher = coarser / fewer pieces.  Maps to VHACD's volume
//                 error tolerance.
//   maxHulls    : hard cap on the number of output convex pieces (>=1).
//   maxVerts    : max vertices per output convex hull (detail / polygon count).
//                 Lower = simpler hulls, easier for the physics packer to seal.
//   out         : receives one DecomposedHull per produced convex piece.
// Returns true if at least one hull was produced.
bool DecomposeConvex( const CUtlVector<Vector> &verts,
                      const CUtlVector<int> &triIndices,
                      float concavity, int maxHulls, int maxVerts,
                      CUtlVector<DecomposedHull> &out );

#endif // CONVEX_DECOMPOSE_H
