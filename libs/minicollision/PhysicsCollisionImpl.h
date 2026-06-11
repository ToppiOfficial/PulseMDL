#pragma once
// Internal implementation types for minicollision.
// Not part of the public API.

#include <vector>
#include "ConvexHull.h"
#include "mathlib/vector.h"
#include "mathlib/mathlib.h"

// One convex hull, result of IPhysicsCollision::ConvexFromVerts
struct CPhysConvexImpl {
    ConvexHullResult hull;        // triangulated convex hull
    unsigned int     gameData = 0; // from SetConvexGameData (bone index for ragdolls)

    float Volume()     const;
    float SurfaceArea() const;
    Vector MassCenter() const;
    void   GetAABB(Vector& mins, Vector& maxs) const;

    // Compute inertia tensor diagonal (about hull's mass center)
    Vector InertiaDiag() const;
};

// Compound of one or more convex hulls, result of ConvertConvexToCollide
struct CPhysCollideImpl {
    std::vector<CPhysConvexImpl*> hulls; // owned pointers
    Vector massCenter = vec3_origin;
    bool   massCenterOverridden = false;

    ~CPhysCollideImpl() {
        for (CPhysConvexImpl* h : hulls) delete h;
    }

    // Recomputes massCenter from hull volumes (call after building)
    void ComputeMassCenter();

    void GetAABB(Vector* pMins, Vector* pMaxs) const;

    // Total volume (sum of hull volumes)
    float Volume() const;
};
