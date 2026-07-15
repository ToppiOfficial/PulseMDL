// Plain convex hull entry point backed by VHACD's hull builder.
//
// VHACD.h declares its hull classes only under ENABLE_VHACD_IMPLEMENTATION, which
// exactly one TU may define (vhacd_impl.cpp). This header lets callers reach the
// hull without pulling the 8k-line single-header library in.

#ifndef VHACD_HULL_H
#define VHACD_HULL_H

#include <vector>

namespace VHACDHull
{

struct HullResult
{
	std::vector<float> vertices;  // xyz triples, only points the faces reference
	std::vector<int>   indices;   // triangle list, CCW from outside
};

// Convex hull of a point cloud. rawPts is numPts xyz float triples.
// maxVerts caps the output vertex count (0 = no meaningful cap).
// Returns false if the cloud is degenerate (fewer than 4 non-coplanar points).
bool ComputeHull( const float *rawPts, int numPts, int maxVerts, HullResult &out );

} // namespace VHACDHull

#endif // VHACD_HULL_H
