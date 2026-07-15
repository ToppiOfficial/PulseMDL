// The ONE translation unit that compiles the VHACD single-header library.
//
// It exists so more than one consumer can use VHACD without each defining
// ENABLE_VHACD_IMPLEMENTATION (which would duplicate every symbol at link time):
// studiomdl/convexdecompose.cpp wants the IVHACD decomposition API, minicollision
// wants the hull builder. Both get declarations from headers and their definitions
// from here.

#include "vhacd_hull.h"

#include <cstdint>
#include <vector>

#define ENABLE_VHACD_IMPLEMENTATION 1
#include "VHACD.h"

namespace VHACDHull
{

bool ComputeHull( const float *rawPts, int numPts, int maxVerts, HullResult &out )
{
	out.vertices.clear();
	out.indices.clear();

	if ( !rawPts || numPts < 4 )
		return false;

	std::vector<VHACD::Vertex> cloud;
	cloud.reserve( numPts );
	for ( int i = 0; i < numPts; i++ )
		cloud.emplace_back( rawPts[i*3], rawPts[i*3+1], rawPts[i*3+2] );

	VHACD::QuickHull qh;
	uint32_t nTris = qh.ComputeConvexHull( cloud, maxVerts > 0 ? (uint32_t)maxVerts : 0xFFFFFFFFu );
	if ( nTris < 4 )
		return false;

	// VHACD hands back its working vertex pool, which can hold points no face
	// references. Emit only the referenced ones, so the caller's point count
	// matches the hull (IVP sizes its ledge from it).
	const std::vector<VHACD::Vertex>   &verts = qh.GetVertices();
	const std::vector<VHACD::Triangle> &tris  = qh.GetIndices();

	std::vector<int> remap( verts.size(), -1 );
	auto Emit = [&]( uint32_t vi ) -> int
	{
		if ( vi >= remap.size() )
			return -1;
		if ( remap[vi] < 0 )
		{
			remap[vi] = (int)( out.vertices.size() / 3 );
			out.vertices.push_back( (float)verts[vi].mX );
			out.vertices.push_back( (float)verts[vi].mY );
			out.vertices.push_back( (float)verts[vi].mZ );
		}
		return remap[vi];
	};

	out.indices.reserve( tris.size() * 3 );
	for ( const VHACD::Triangle &t : tris )
	{
		int a = Emit( t.mI0 ), b = Emit( t.mI1 ), c = Emit( t.mI2 );
		if ( a < 0 || b < 0 || c < 0 )
			return false;
		out.indices.push_back( a );
		out.indices.push_back( b );
		out.indices.push_back( c );
	}

	return out.vertices.size() >= 4 * 3 && out.indices.size() >= 4 * 3;
}

} // namespace VHACDHull
