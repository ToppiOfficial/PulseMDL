// Convex decomposition bridge implementation.
//
// This is the ONLY translation unit that pulls in the VHACD backend, and the
// only one that defines ENABLE_VHACD_IMPLEMENTATION (the single-header library
// requires its implementation to be compiled in exactly one .cpp).  Keeping it
// isolated here means the rest of the compiler stays backend-agnostic; swapping
// VHACD for another decomposer (e.g. CoACD) only touches this file.

#include "studiomdl/convexdecompose.h"

#include <vector>
#include <cstdint>

#include "tier0/icommandline.h"

// VHACD is multithreaded by default.  Threading here is conditional on the
// -collisionthreads CLI flag (see DecomposeConvex): >=2 enables VHACD's async
// split-plane search across cores, <=1 forces the single-threaded path.  Define
// the implementation exactly once, here.
#define ENABLE_VHACD_IMPLEMENTATION 1
#include "VHACD.h"

namespace
{
	// Swallow VHACD's progress/log chatter so a model compile stays quiet.
	class NullLogger : public VHACD::IVHACD::IUserLogger
	{
	public:
		void Log( const char * /*msg*/ ) override {}
	};
}

bool DecomposeConvex( const CUtlVector<Vector> &verts,
                      const CUtlVector<int> &triIndices,
                      float concavity, int maxHulls, bool forceHulls, int maxVerts,
                      CUtlVector<DecomposedHull> &out )
{
	out.RemoveAll();

	if ( verts.Count() < 4 || triIndices.Count() < 3 )
		return false;

	// Flatten verts to a double array (X1,Y1,Z1, X2,Y2,Z2, ...).
	std::vector<double> points;
	points.reserve( (size_t)verts.Count() * 3 );
	for ( int i = 0; i < verts.Count(); i++ )
	{
		points.push_back( verts[i].x );
		points.push_back( verts[i].y );
		points.push_back( verts[i].z );
	}

	// Triangle indices as uint32_t, validated against vertex range.
	std::vector<uint32_t> tris;
	tris.reserve( triIndices.Count() );
	for ( int i = 0; i + 2 < triIndices.Count(); i += 3 )
	{
		int a = triIndices[i], b = triIndices[i+1], c = triIndices[i+2];
		if ( a < 0 || b < 0 || c < 0 ||
		     a >= verts.Count() || b >= verts.Count() || c >= verts.Count() )
			continue;
		tris.push_back( (uint32_t)a );
		tris.push_back( (uint32_t)b );
		tris.push_back( (uint32_t)c );
	}
	if ( tris.size() < 3 )
		return false;

	NullLogger logger;
	VHACD::IVHACD::Parameters params;
	params.m_logger   = &logger;

	// -collisionthreads >=2 lets VHACD parallelize its split-plane search across
	// cores; <=1 keeps the single-threaded, fully-deterministic path.  Default 4.
	int nThreads = CommandLine()->ParmValue( "-collisionthreads", 4 );
	params.m_asyncACD = ( nThreads >= 2 );

	// The downstream physics packer (minicollision) re-hulls each piece with a
	// small incremental convex-hull builder and rejects anything that isn't a
	// clean closed manifold.  Keeping the per-hull vertex count low produces
	// simpler, well-conditioned hulls that survive that re-hull/validation.
	// (VHACD's default of 64 routinely yields hulls the packer rejects.)
	// maxVerts also doubles as the user's "polygon count / detail" knob.
	if ( maxVerts < 4 )  maxVerts = 4;   // a convex solid needs at least 4 verts
	params.m_maxNumVerticesPerCH = (uint32_t)maxVerts;
	params.m_shrinkWrap          = true;

	// Map the user-facing concavity [0..1] onto VHACD's allowed volume error
	// (a percent).  Lower concavity -> tighter fit -> smaller allowed error ->
	// more pieces; higher concavity -> coarser -> fewer pieces.
	// Clamp to a sane band: ~0.1% (very tight) .. ~10% (very coarse).
	float c = concavity;
	if ( c < 0.0f ) c = 0.0f;
	if ( c > 1.0f ) c = 1.0f;
	params.m_minimumVolumePercentErrorAllowed = 0.1 + (double)c * ( 10.0 - 0.1 );

	if ( maxHulls < 1 ) maxHulls = 1;
	params.m_maxConvexHulls = (uint32_t)maxHulls;

	// VHACD splits by the concavity threshold above, then merges down to
	// m_maxConvexHulls only if it produced more (VHACD.h, PerformConvexDecomposition).
	// It never pads *up* to the cap, so by default maxHulls is a suggestion: a mesh
	// that seals within the concavity tolerance using fewer pieces yields fewer.
	// forceHulls makes it an exact target - drive the volume error toward zero so
	// VHACD keeps splitting past the concavity threshold until it has at least
	// maxHulls pieces, which the merge step then trims back to exactly maxHulls.
	// (A genuinely convex region still can't be split, so the target is a ceiling
	// the geometry may not reach.)
	if ( forceHulls && maxHulls > 1 )
		params.m_minimumVolumePercentErrorAllowed = 0.0;

	// Voxel resolution controls how faithfully VHACD captures the shape and where
	// it places its split planes; it is INDEPENDENT of the per-hull vertex budget
	// (m_maxNumVerticesPerCH above), which decimates each finished hull afterward
	// via ShrinkWrap/QuickHull.  So VHACD already does "decompose high, then
	// decimate to N verts" - the classic Blender collision workflow.  Tying the
	// grid to maxVerts (the old formula) starved that first step: a low maxverts
	// ran the whole decomposition on a coarse grid, yielding blocky, lopsided
	// hulls that spent their vertex budget on the wrong region.  Run the grid at
	// full detail regardless of the vertex cap; scale up with hull count (more
	// pieces need a finer grid to separate cleanly), floored at VHACD's default.
	uint32_t res = 400000u + (uint32_t)( maxHulls > 1 ? maxHulls - 1 : 0 ) * 100000u;
	if ( res > 1000000u ) res = 1000000u;
	params.m_resolution = res;

	VHACD::IVHACD *pVHACD = VHACD::CreateVHACD();
	if ( !pVHACD )
		return false;

	bool bComputed = pVHACD->Compute( points.data(), (uint32_t)( points.size() / 3 ),
	                                  tris.data(),   (uint32_t)( tris.size() / 3 ),
	                                  params );
	if ( bComputed )
	{
		uint32_t nHulls = pVHACD->GetNConvexHulls();
		for ( uint32_t h = 0; h < nHulls; h++ )
		{
			VHACD::IVHACD::ConvexHull ch;
			if ( !pVHACD->GetConvexHull( h, ch ) )
				continue;
			if ( ch.m_points.size() < 4 )
				continue;

			DecomposedHull &hull = out[ out.AddToTail() ];
			hull.verts.EnsureCapacity( (int)ch.m_points.size() );
			for ( size_t v = 0; v < ch.m_points.size(); v++ )
			{
				const VHACD::Vertex &p = ch.m_points[v];
				hull.verts.AddToTail( Vector( (float)p.mX, (float)p.mY, (float)p.mZ ) );
			}
		}
	}

	pVHACD->Clean();
	pVHACD->Release();

	return out.Count() > 0;
}
