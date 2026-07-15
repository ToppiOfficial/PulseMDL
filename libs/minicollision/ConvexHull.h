#pragma once
// 3D convex hull builder, backed by VHACD's hull (libs/vhacd).
// Suitable for small inputs (< 1024 verts).

#include <vector>

struct CHVec3 { float x, y, z; };

struct ConvexHullResult {
    std::vector<CHVec3> vertices;  // deduplicated hull vertices
    std::vector<int>    indices;   // triangles: 3 indices per face, CCW from outside
    bool                degenerate = false;
};

// Build the convex hull of 'numPts' points stored as interleaved x,y,z floats.
// Returns empty/degenerate on < 4 non-coplanar points.
ConvexHullResult BuildConvexHull(const float* pts, int numPts);
