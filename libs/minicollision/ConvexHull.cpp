// Convex hull, delegated to VHACD's builder (Newton-derived, exact-arithmetic
// predicates).
//
// A hand-rolled incremental hull used to live here. It tore its horizon on
// near-coplanar clouds and emitted hulls that were not closed manifolds, which
// IVP rejects: decompiled physics meshes quantize coordinates to ~1e-6 in, which
// is exactly the noise level that breaks a float plane-side test. VHACD's builder
// handles that cloud shape cleanly, so it does the work and this file only
// adapts the types.

#include "ConvexHull.h"
#include "vhacd_hull.h"

// IVP's start_point_index is 16-bit, and a convex ledge with more points than
// this is far past anything a collision hull needs.
static const int kMaxHullVerts = 1024;

ConvexHullResult BuildConvexHull(const float* rawPts, int numPts)
{
    ConvexHullResult result;

    if (!rawPts || numPts < 4) {
        result.degenerate = true;
        return result;
    }

    VHACDHull::HullResult hull;
    if (!VHACDHull::ComputeHull(rawPts, numPts, kMaxHullVerts, hull)) {
        result.degenerate = true;
        return result;
    }

    result.vertices.reserve(hull.vertices.size() / 3);
    for (size_t i = 0; i + 2 < hull.vertices.size(); i += 3)
        result.vertices.push_back({ hull.vertices[i], hull.vertices[i+1], hull.vertices[i+2] });

    result.indices = hull.indices;

    if (result.indices.empty() || result.vertices.size() < 4)
        result.degenerate = true;
    return result;
}
