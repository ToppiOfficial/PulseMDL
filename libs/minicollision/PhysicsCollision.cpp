// minicollision - IPhysicsCollision implementation
// Provides the 11 methods studiomdl needs for $collisionmodel / $collisionjoints.
// Uses the ConvexHull module for hull computation and ivp_compact.h for PHY serialization.

#include "PhysicsCollisionImpl.h"
#include "ivp_compact.h"
#include "MiniVPhysics.h"
#include "vphysics_interface.h"
#include "mathlib/vector.h"
#include "mathlib/mathlib.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <new>

// ---------------------------------------------------------------------------
// CPhysConvexImpl helpers
// ---------------------------------------------------------------------------

float CPhysConvexImpl::Volume() const
{
    const auto& verts = hull.vertices;
    const auto& idx   = hull.indices;
    float vol = 0.0f;
    for (int i = 0; i < (int)idx.size(); i += 3) {
        const CHVec3& v0 = verts[idx[i+0]];
        const CHVec3& v1 = verts[idx[i+1]];
        const CHVec3& v2 = verts[idx[i+2]];
        // signed volume of tetrahedron from origin
        vol += v0.x*(v1.y*v2.z - v1.z*v2.y)
             + v0.y*(v1.z*v2.x - v1.x*v2.z)
             + v0.z*(v1.x*v2.y - v1.y*v2.x);
    }
    return fabsf(vol) / 6.0f;
}

float CPhysConvexImpl::SurfaceArea() const
{
    const auto& verts = hull.vertices;
    const auto& idx   = hull.indices;
    float area = 0.0f;
    for (int i = 0; i < (int)idx.size(); i += 3) {
        const CHVec3& a = verts[idx[i+0]];
        const CHVec3& b = verts[idx[i+1]];
        const CHVec3& c = verts[idx[i+2]];
        float ex = b.x-a.x, ey = b.y-a.y, ez = b.z-a.z;
        float fx = c.x-a.x, fy = c.y-a.y, fz = c.z-a.z;
        float cx = ey*fz - ez*fy;
        float cy = ez*fx - ex*fz;
        float cz = ex*fy - ey*fx;
        area += sqrtf(cx*cx + cy*cy + cz*cz);
    }
    return area * 0.5f;
}

Vector CPhysConvexImpl::MassCenter() const
{
    const auto& verts = hull.vertices;
    const auto& idx   = hull.indices;
    float cx=0, cy=0, cz=0, totalVol=0;
    for (int i = 0; i < (int)idx.size(); i += 3) {
        const CHVec3& v0 = verts[idx[i+0]];
        const CHVec3& v1 = verts[idx[i+1]];
        const CHVec3& v2 = verts[idx[i+2]];
        float vol = (v0.x*(v1.y*v2.z - v1.z*v2.y)
                   + v0.y*(v1.z*v2.x - v1.x*v2.z)
                   + v0.z*(v1.x*v2.y - v1.y*v2.x)) / 6.0f;
        // centroid of tetrahedron (origin + v0 + v1 + v2) / 4
        cx += vol * (v0.x + v1.x + v2.x) / 4.0f;
        cy += vol * (v0.y + v1.y + v2.y) / 4.0f;
        cz += vol * (v0.z + v1.z + v2.z) / 4.0f;
        totalVol += vol;
    }
    if (fabsf(totalVol) > 1e-10f) { cx/=totalVol; cy/=totalVol; cz/=totalVol; }
    return Vector(cx, cy, cz);
}

void CPhysConvexImpl::GetAABB(Vector& mins, Vector& maxs) const
{
    mins.x = mins.y = mins.z =  1e30f;
    maxs.x = maxs.y = maxs.z = -1e30f;
    for (const CHVec3& v : hull.vertices) {
        if (v.x < mins.x) mins.x = v.x;
        if (v.y < mins.y) mins.y = v.y;
        if (v.z < mins.z) mins.z = v.z;
        if (v.x > maxs.x) maxs.x = v.x;
        if (v.y > maxs.y) maxs.y = v.y;
        if (v.z > maxs.z) maxs.z = v.z;
    }
}

// Inertia tensor diagonal about mass center (unit density, scaled by volume later)
// Using Polyhedral Mass Properties formula (Mirtich 1994 covariance approach)
Vector CPhysConvexImpl::InertiaDiag() const
{
    const auto& verts = hull.vertices;
    const auto& idx   = hull.indices;
    Vector mc = MassCenter();

    float Ixx=0, Iyy=0, Izz=0, totalVol=0;
    for (int i = 0; i < (int)idx.size(); i += 3) {
        // Translate to mass center
        float x0=verts[idx[i+0]].x-mc.x, y0=verts[idx[i+0]].y-mc.y, z0=verts[idx[i+0]].z-mc.z;
        float x1=verts[idx[i+1]].x-mc.x, y1=verts[idx[i+1]].y-mc.y, z1=verts[idx[i+1]].z-mc.z;
        float x2=verts[idx[i+2]].x-mc.x, y2=verts[idx[i+2]].y-mc.y, z2=verts[idx[i+2]].z-mc.z;
        float vol = (x0*(y1*z2-z1*y2) + y0*(z1*x2-x1*z2) + z0*(x1*y2-y1*x2)) / 6.0f;
        totalVol += vol;
        // Inertia contribution per tet (Polyhedral Mass Properties, Eberly)
        // Ixx += integral(y^2+z^2) over tet
        Ixx += vol * (y0*y0+y0*y1+y1*y1+y0*y2+y1*y2+y2*y2 +
                      z0*z0+z0*z1+z1*z1+z0*z2+z1*z2+z2*z2) / 10.0f;
        Iyy += vol * (x0*x0+x0*x1+x1*x1+x0*x2+x1*x2+x2*x2 +
                      z0*z0+z0*z1+z1*z1+z0*z2+z1*z2+z2*z2) / 10.0f;
        Izz += vol * (x0*x0+x0*x1+x1*x1+x0*x2+x1*x2+x2*x2 +
                      y0*y0+y0*y1+y1*y1+y0*y2+y1*y2+y2*y2) / 10.0f;
    }
    if (fabsf(totalVol) > 1e-10f) { Ixx/=totalVol; Iyy/=totalVol; Izz/=totalVol; }
    // Take absolute value for signed volume correction
    return Vector(fabsf(Ixx), fabsf(Iyy), fabsf(Izz));
}

// ---------------------------------------------------------------------------
// CPhysCollideImpl helpers
// ---------------------------------------------------------------------------

void CPhysCollideImpl::ComputeMassCenter()
{
    if (massCenterOverridden) return;
    float totalVol = 0;
    massCenter = vec3_origin;
    for (CPhysConvexImpl* h : hulls) {
        float v = h->Volume();
        Vector mc = h->MassCenter();
        massCenter += mc * v;
        totalVol   += v;
    }
    if (totalVol > 1e-10f) massCenter /= totalVol;
}

void CPhysCollideImpl::GetAABB(Vector* pMins, Vector* pMaxs) const
{
    pMins->x = pMins->y = pMins->z =  1e30f;
    pMaxs->x = pMaxs->y = pMaxs->z = -1e30f;
    for (CPhysConvexImpl* h : hulls) {
        Vector lo, hi;
        h->GetAABB(lo, hi);
        if (lo.x < pMins->x) pMins->x = lo.x;
        if (lo.y < pMins->y) pMins->y = lo.y;
        if (lo.z < pMins->z) pMins->z = lo.z;
        if (hi.x > pMaxs->x) pMaxs->x = hi.x;
        if (hi.y > pMaxs->y) pMaxs->y = hi.y;
        if (hi.z > pMaxs->z) pMaxs->z = hi.z;
    }
}

float CPhysCollideImpl::Volume() const
{
    float v = 0;
    for (CPhysConvexImpl* h : hulls) v += h->Volume();
    return v;
}

// ---------------------------------------------------------------------------
// PHY blob serialization (CollideSize / CollideWrite)
// ---------------------------------------------------------------------------

// IVP/vphysics stores all spatial quantities in meters. Source game units are
// inches. vphysics applies this conversion internally in ConvexFromVerts and
// CollideWrite; we must replicate it so the blob is read correctly at runtime.
static const float INCHES_TO_METERS    = 0.0254f;
static const float INCHES_TO_METERS_SQ = INCHES_TO_METERS * INCHES_TO_METERS;

// PHY compact surfaces are in IVP coordinate space (Y-DOWN, meters).
// vphysics' ConvertPositionToIVP does:
//   IVP[0] =  Source.x * scale
//   IVP[1] = -Source.z * scale   (Source "up" -> IVP "-Y")
//   IVP[2] =  Source.y * scale
// This has det=1 (pure rotation), so triangle winding is preserved.
static inline void SrcToIVP(float sx, float sy, float sz, float& ox, float& oy, float& oz)
{
    ox =  sx * INCHES_TO_METERS;
    oy = -sz * INCHES_TO_METERS;
    oz =  sy * INCHES_TO_METERS;
}

// Compute IVP compact ledge size in bytes for a single hull
static int LedgeSizeBytes(const CPhysConvexImpl* h)
{
    int nTri = (int)h->hull.indices.size() / 3;
    int nPts = (int)h->hull.vertices.size();
    return (1 + nTri + nPts) * 16;
}

// Compute total PHY blob size for a CPhysCollide.
//
// The .phy stores each solid as "int32 size" + blob, and Source's vcollide
// loader requires size to equal exactly phycollide_hdr_t(28) + surfaceSize with
// no slack. So the blob size must NOT be padded - any extra bytes make the
// loader reject the whole vcollide ("missing vcollide data").  wtf? - Toppi
static int CalcCollideSize(const CPhysCollideImpl* col)
{
    int N = (int)col->hulls.size();
    if (N == 0) return 0;
    // blob = header(28) + surface(48) + sum(ledge sizes) + tree_nodes(28*(2N-1))
    int nodeCount = 2 * N - 1;
    int ledgeBytes = 0;
    for (CPhysConvexImpl* h : col->hulls) ledgeBytes += LedgeSizeBytes(h);
    return 28 + 48 + nodeCount * 28 + ledgeBytes;
}

// Validate that a hull is a closed 2-manifold within IVP's bitfield limits.
// IVP's runtime navigates hulls with edge->get_opposite() hops; a single
// unpaired or duplicated directed edge sends the solver into garbage memory
// in-game, so reject such hulls up front.
static bool HullIsValidForIVP(const ConvexHullResult& hull)
{
    int nTri = (int)hull.indices.size() / 3;
    int nPts = (int)hull.vertices.size();
    if (nTri < 4 || nTri > 4095) return false;   // tri_index / pierce_index are 12-bit
    if (nPts < 4 || nPts > 65535) return false;  // start_point_index is 16-bit

    std::unordered_map<uint64_t, int> edges;
    edges.reserve(nTri * 6);
    for (int t = 0; t < nTri; t++) {
        for (int e = 0; e < 3; e++) {
            int v0 = hull.indices[t*3 + e];
            int v1 = hull.indices[t*3 + (e+1)%3];
            if (v0 == v1) return false;
            uint64_t key = ((uint64_t)v0 << 32) | (uint32_t)v1;
            if (edges.count(key)) return false;  // non-manifold: edge used twice
            edges[key] = t*3 + e;
        }
    }
    for (const auto& kv : edges) {
        uint64_t rev = (kv.first << 32) | (kv.first >> 32);
        if (!edges.count(rev)) return false;     // open mesh: no reversed twin
    }
    return true;
}

// Serialize one compact ledge into 'dest'. Returns bytes written.
static int WriteCompactLedge(char* dest, const CPhysConvexImpl* h, int gameData)
{
    const auto& verts = h->hull.vertices;
    const auto& idx   = h->hull.indices;
    int nTri = (int)idx.size() / 3;
    int nPts = (int)verts.size();

    // Build adjacency: directed edge (v0->v1) -> linear edge slot in this ledge.
    // IVP's get_opposite() does pointer arithmetic in 4-byte edge units and a
    // triangle occupies 4 slots (bitfield dword + 3 edges), so slot = t*4 + e.
    std::unordered_map<uint64_t, int> edgeLinear;
    edgeLinear.reserve(nTri * 6);
    for (int t = 0; t < nTri; t++) {
        for (int e = 0; e < 3; e++) {
            int v0 = idx[t*3 + e];
            int v1 = idx[t*3 + (e+1)%3];
            edgeLinear[((uint64_t)v0 << 32) | (uint32_t)v1] = t*4 + e;
        }
    }

    // Triangle centroids and outward normals for pierce_index computation
    std::vector<CHVec3> triCentroid(nTri), triNormal(nTri);
    for (int t = 0; t < nTri; t++) {
        const CHVec3& a = verts[idx[t*3+0]];
        const CHVec3& b = verts[idx[t*3+1]];
        const CHVec3& c = verts[idx[t*3+2]];
        triCentroid[t] = { (a.x+b.x+c.x)/3.0f, (a.y+b.y+c.y)/3.0f, (a.z+b.z+c.z)/3.0f };
        float ex = b.x-a.x, ey = b.y-a.y, ez = b.z-a.z;
        float fx = c.x-a.x, fy = c.y-a.y, fz = c.z-a.z;
        triNormal[t] = { ey*fz - ez*fy, ez*fx - ex*fz, ex*fy - ey*fx };
    }

    // Write ledge header
    ivp_compact_ledge_t hdr;
    ivp_ledge_init(hdr, nTri, nPts);
    hdr.client_data = gameData;
    memcpy(dest, &hdr, sizeof(hdr));
    int off = sizeof(hdr);  // = 16

    // Write triangles
    for (int t = 0; t < nTri; t++) {
        // pierce_index = triangle on the far side of the hull (the one whose
        // centroid lies deepest against this triangle's outward normal); IVP
        // uses it as the start of the pierce walk in minimize_on_other_side().
        int pierce = t;
        float bestDot = 1e30f;
        for (int u = 0; u < nTri; u++) {
            if (u == t) continue;
            float d = triNormal[t].x * triCentroid[u].x
                    + triNormal[t].y * triCentroid[u].y
                    + triNormal[t].z * triCentroid[u].z;
            if (d < bestDot) { bestDot = d; pierce = u; }
        }

        ivp_compact_triangle_t tri;
        ivp_tri_set(tri, t, pierce, 0);
        for (int e = 0; e < 3; e++) {
            int v0 = idx[t*3 + e];
            int v1 = idx[t*3 + (e+1)%3];
            auto it = edgeLinear.find(((uint64_t)v1 << 32) | (uint32_t)v0);
            int16_t opp = 0;
            if (it != edgeLinear.end())
                opp = (int16_t)(it->second - (t*4 + e));
            ivp_edge_set(tri.c_three_edges[e], (uint16_t)v0, opp);
        }
        memcpy(dest + off, &tri, sizeof(tri));
        off += sizeof(tri);
    }

    // Write points: scale to meters (vphysics does its own axis conversion at load time)
    for (const CHVec3& v : verts) {
        ivp_compact_poly_point_t pt;
        SrcToIVP(v.x, v.y, v.z, pt.k[0], pt.k[1], pt.k[2]);
        pt.hesse_val = 0.0f;
        memcpy(dest + off, &pt, sizeof(pt));
        off += sizeof(pt);  // 16 bytes
    }

    return off;
}

// Write full PHY blob for CPhysCollide into 'dest'. Returns bytes written.
static int WriteCollide(char* dest, const CPhysCollideImpl* col)
{
    int N = (int)col->hulls.size();
    if (N == 0) return 0;

    int nodeCount = 2 * N - 1;
    // Pre-compute ledge sizes for offset math
    std::vector<int> ledgeSz(N);
    for (int i = 0; i < N; i++) ledgeSz[i] = LedgeSizeBytes(col->hulls[i]);

    // Compute bounding sphere for the whole collide (in game units first)
    Vector mins, maxs;
    col->GetAABB(&mins, &maxs);
    Vector center = (mins + maxs) * 0.5f;
    // Convert bounding sphere center to IVP coords (Y-up) and meters
    float cx_ivp, cy_ivp, cz_ivp;
    SrcToIVP(center.x, center.y, center.z, cx_ivp, cy_ivp, cz_ivp);
    float radiusM = ((maxs - center).Length()) * INCHES_TO_METERS;

    // Total IVP data size (surface + ledges + tree)
    int ledgeTotal = 0;
    for (int s : ledgeSz) ledgeTotal += s;
    int surfaceDataSize = 48 + ledgeTotal + nodeCount * 28;

    // Compute inertia (weighted average across hulls)
    Vector inertia(0,0,0);
    float totalVol = 0;
    for (CPhysConvexImpl* h : col->hulls) {
        float v = h->Volume();
        inertia += h->InertiaDiag() * v;
        totalVol += v;
    }
    if (totalVol > 1e-10f) inertia /= totalVol;

    float mcx_ivp, mcy_ivp, mcz_ivp;
    SrcToIVP(col->massCenter.x, col->massCenter.y, col->massCenter.z, mcx_ivp, mcy_ivp, mcz_ivp);

    // upper_limit_radius and max_factor_surface_deviation are measured around
    // the mass center (IVP_SurfaceBuilder_Ledge_Soup::insert_radius_in_compact_surface
    // -> IVP_Compact_Ledge_Solver::calc_radius_to_given_center).
    float massRadiusM = 0.0f;
    float massDevM    = 0.0f;
    for (CPhysConvexImpl* h : col->hulls) {
        const auto& verts = h->hull.vertices;
        const auto& idx   = h->hull.indices;
        for (int t = 0; t < (int)idx.size() / 3; t++) {
            float p[3][3]; // triangle verts relative to mass center, IVP space
            for (int k = 0; k < 3; k++) {
                const CHVec3& v = verts[idx[t*3 + k]];
                SrcToIVP(v.x, v.y, v.z, p[k][0], p[k][1], p[k][2]);
                p[k][0] -= mcx_ivp; p[k][1] -= mcy_ivp; p[k][2] -= mcz_ivp;
            }
            float e1x = p[1][0]-p[0][0], e1y = p[1][1]-p[0][1], e1z = p[1][2]-p[0][2];
            float e2x = p[2][0]-p[0][0], e2y = p[2][1]-p[0][1], e2z = p[2][2]-p[0][2];
            float hx = e1y*e2z - e1z*e2y;
            float hy = e1z*e2x - e1x*e2z;
            float hz = e1x*e2y - e1y*e2x;
            float hLenSq = hx*hx + hy*hy + hz*hz;
            for (int k = 0; k < 3; k++) {
                float qSq = p[k][0]*p[k][0] + p[k][1]*p[k][1] + p[k][2]*p[k][2];
                if (qSq > massRadiusM * massRadiusM) massRadiusM = sqrtf(qSq);
                if (hLenSq > 1e-20f) {
                    // distance of vertex from the line through the mass center
                    // along the face normal = |p x h| / |h|
                    float cxp = p[k][1]*hz - p[k][2]*hy;
                    float cyp = p[k][2]*hx - p[k][0]*hz;
                    float czp = p[k][0]*hy - p[k][1]*hx;
                    float devSq = (cxp*cxp + cyp*cyp + czp*czp) / hLenSq;
                    if (devSq > massDevM * massDevM) massDevM = sqrtf(devSq);
                }
            }
        }
    }
    // IVP_COMPACT_SURFACE_DEVIATION_STEP_SIZE = 1/250
    int devFactor = 0;
    if (massRadiusM > 1e-10f)
        devFactor = (int)(1.0f + massDevM / (massRadiusM * (1.0f / 250.0f)));
    if (devFactor > 255) devFactor = 255;

    // ------------------------------------------------------------------
    // 1. phycollide_hdr_t
    // ------------------------------------------------------------------
    int off = 0;
    phycollide_hdr_t hdr;
    hdr.vphysicsID      = (int32_t)MINICOL_VPHY_ID;
    hdr.version         = MINICOL_VERSION_IVP;
    hdr.modelType       = 0;
    hdr.surfaceSize     = surfaceDataSize;
    // Drag areas in m^2. IVP X=Src X, IVP Y=Src Z, IVP Z=Src Y.
    // Area perpendicular to IVP axis = product of the OTHER TWO IVP extents.
    float hx = (maxs.x - mins.x) * 0.5f * INCHES_TO_METERS;  // IVP X extent
    float hy = (maxs.z - mins.z) * 0.5f * INCHES_TO_METERS;  // IVP Y extent = Src Z
    float hz = (maxs.y - mins.y) * 0.5f * INCHES_TO_METERS;  // IVP Z extent = Src Y
    hdr.dragAxisAreas[0] = hy * hz * 3.14159f;
    hdr.dragAxisAreas[1] = hx * hz * 3.14159f;
    hdr.dragAxisAreas[2] = hx * hy * 3.14159f;
    hdr.axisMapSize     = 0;
    memcpy(dest + off, &hdr, sizeof(hdr)); off += sizeof(hdr); // 28

    // ------------------------------------------------------------------
    // 2. ivp_compact_surface_t
    // ------------------------------------------------------------------
    ivp_compact_surface_t surf;
    memset(&surf, 0, sizeof(surf));
    surf.mass_center[0]       = mcx_ivp;
    surf.mass_center[1]       = mcy_ivp;
    surf.mass_center[2]       = mcz_ivp;
    // IVP Y = Src Z, IVP Z = Src Y -> permute inertia diagonal accordingly
    surf.rotation_inertia[0]  = inertia.x * INCHES_TO_METERS_SQ;  // IVP Ixx = Src Ixx
    surf.rotation_inertia[1]  = inertia.z * INCHES_TO_METERS_SQ;  // IVP Iyy = Src Izz
    surf.rotation_inertia[2]  = inertia.y * INCHES_TO_METERS_SQ;  // IVP Izz = Src Iyy
    surf.upper_limit_radius   = massRadiusM;
    surf.bitfield = (uint32_t)devFactor; // max_factor_surface_deviation in bits [0..7]
    ivp_surface_set_byte_size(surf, surfaceDataSize);
    // Valve layout: ledges directly follow the 48-byte surface header (keeps
    // every ledge 16-byte aligned, required by IVP's edge->triangle pointer
    // masking), ledge tree nodes go at the end.
    surf.offset_ledgetree_root = (int32_t)sizeof(ivp_compact_surface_t) + ledgeTotal;
    surf.dummy[0] = 0;
    surf.dummy[1] = 0;
    surf.dummy[2] = (int32_t)MINICOL_IVPS_ID;
    memcpy(dest + off, &surf, sizeof(surf)); off += sizeof(surf); // 48

    // ------------------------------------------------------------------
    // 3. Compact ledges. At runtime vphysics copies the blob starting at the
    //    compact surface into 16-aligned memory, so alignment relative to the
    //    surface start (dest[28]) is what matters: surface header = 48 and
    //    each ledge is a multiple of 16 bytes -> every ledge stays 16-aligned.
    // ------------------------------------------------------------------
    std::vector<int> ledgeAbsOff(N); // offsets from dest[0]
    ledgeAbsOff[0] = off;
    for (int k = 1; k < N; k++) ledgeAbsOff[k] = ledgeAbsOff[k-1] + ledgeSz[k-1];

    for (int k = 0; k < N; k++) {
        int written = WriteCompactLedge(dest + off, col->hulls[k], col->hulls[k]->gameData);
        off += written;
    }

    // ------------------------------------------------------------------
    // 4. Ledge tree nodes: degenerate left-chain layout
    //    Nodes [0 .. N-2] are internal, nodes [N-1 .. 2N-2] are leaves.
    //    Left child of internal node i = next in memory (node i+1).
    //    Right child of internal node i = node N+i.
    // ------------------------------------------------------------------
    int nodeBase = off; // offset of node array start from dest[0]

    // Write all 2N-1 nodes (fill positions first with zeros, fix offsets after)
    int nodeAreaSize = nodeCount * (int)sizeof(ivp_ledgetree_node_t);
    memset(dest + off, 0, nodeAreaSize);

    // Helper: get node pointer by index
    auto NodeAt = [&](int ni) -> ivp_ledgetree_node_t* {
        return reinterpret_cast<ivp_ledgetree_node_t*>(dest + nodeBase + ni * 28);
    };

    // Fill internal nodes (0 .. N-2)
    for (int i = 0; i < N-1; i++) {
        ivp_ledgetree_node_t* nd = NodeAt(i);
        // Left child = node i+1 (automatically next in memory, no offset needed)
        // Right child = node N+i
        int rightNodeAbsOff = nodeBase + (N+i) * 28;
        int thisNodeAbsOff  = nodeBase + i * 28;
        nd->offset_right_node    = rightNodeAbsOff - thisNodeAbsOff;
        nd->offset_compact_ledge = 0; // internal
        nd->center[0] = cx_ivp;
        nd->center[1] = cy_ivp;
        nd->center[2] = cz_ivp;
        nd->radius = radiusM;
    }

    // Fill leaf nodes (N-1 .. 2N-2), leaf k -> hull k
    for (int k = 0; k < N; k++) {
        int leafNodeIdx   = N - 1 + k;
        int leafAbsOff    = nodeBase + leafNodeIdx * 28;
        int ledgeAbsOffK  = ledgeAbsOff[k];

        CPhysConvexImpl* h = col->hulls[k];
        Vector hlo, hhi;
        h->GetAABB(hlo, hhi);
        Vector hcSrc = (hlo + hhi) * 0.5f;
        float  hradius = (hhi - hcSrc).Length() * INCHES_TO_METERS;
        float hcx, hcy, hcz;
        SrcToIVP(hcSrc.x, hcSrc.y, hcSrc.z, hcx, hcy, hcz);

        ivp_ledgetree_node_t* nd = NodeAt(leafNodeIdx);
        nd->offset_right_node    = 0; // leaf
        nd->offset_compact_ledge = ledgeAbsOffK - leafAbsOff;
        nd->center[0] = hcx;
        nd->center[1] = hcy;
        nd->center[2] = hcz;
        nd->radius = hradius;
    }

    off += nodeAreaSize;

    return off;
}

// ---------------------------------------------------------------------------
// IPhysicsCollision implementation
// ---------------------------------------------------------------------------

class CPhysicsCollisionMini : public IPhysicsCollision
{
public:
    virtual ~CPhysicsCollisionMini() {}

    // -- Required by studiomdl --

    CPhysConvex* ConvexFromVerts(Vector** ppVerts, int vertCount) override
    {
        if (!ppVerts || vertCount < 1) return nullptr;
        // Build float array for ConvexHull
        std::vector<float> fpts;
        fpts.reserve(vertCount * 3);
        for (int i = 0; i < vertCount; i++) {
            fpts.push_back((*ppVerts[i]).x);
            fpts.push_back((*ppVerts[i]).y);
            fpts.push_back((*ppVerts[i]).z);
        }
        ConvexHullResult res = BuildConvexHull(fpts.data(), vertCount);
        if (res.degenerate || res.indices.empty()) {
            // Fallback: treat as a point / very thin shape; return null so caller skips it
            return nullptr;
        }
        if (!HullIsValidForIVP(res)) {
            fprintf(stderr, "minicollision: ERROR - convex hull (%d verts, %d tris) is not a closed "
                    "manifold or exceeds IVP limits - aborting collision compile\n",
                    (int)res.vertices.size(), (int)res.indices.size() / 3);
            exit(1);
        }
        CPhysConvexImpl* c = new CPhysConvexImpl();
        c->hull = std::move(res);
        return reinterpret_cast<CPhysConvex*>(c);
    }

    float ConvexVolume(CPhysConvex* pConvex) override
    {
        if (!pConvex) return 0;
        return reinterpret_cast<CPhysConvexImpl*>(pConvex)->Volume();
    }

    float ConvexSurfaceArea(CPhysConvex* pConvex) override
    {
        if (!pConvex) return 0;
        return reinterpret_cast<CPhysConvexImpl*>(pConvex)->SurfaceArea();
    }

    void SetConvexGameData(CPhysConvex* pConvex, unsigned int gameData) override
    {
        if (!pConvex) return;
        reinterpret_cast<CPhysConvexImpl*>(pConvex)->gameData = gameData;
    }

    void ConvexFree(CPhysConvex* pConvex) override
    {
        delete reinterpret_cast<CPhysConvexImpl*>(pConvex);
    }

    CPhysCollide* ConvertConvexToCollide(CPhysConvex** ppConvex, int convexCount) override
    {
        if (!ppConvex || convexCount < 1) return nullptr;
        CPhysCollideImpl* col = new CPhysCollideImpl();
        for (int i = 0; i < convexCount; i++) {
            if (!ppConvex[i]) continue;
            // Transfer ownership
            CPhysConvexImpl* h = reinterpret_cast<CPhysConvexImpl*>(ppConvex[i]);
            col->hulls.push_back(h);
        }
        col->ComputeMassCenter();
        return reinterpret_cast<CPhysCollide*>(col);
    }

    CPhysCollide* ConvertConvexToCollideParams(CPhysConvex** ppConvex, int convexCount,
                                               const convertconvexparams_t& /*params*/) override
    {
        return ConvertConvexToCollide(ppConvex, convexCount);
    }

    void DestroyCollide(CPhysCollide* pCollide) override
    {
        delete reinterpret_cast<CPhysCollideImpl*>(pCollide);
    }

    int CollideSize(CPhysCollide* pCollide) override
    {
        if (!pCollide) return 0;
        return CalcCollideSize(reinterpret_cast<CPhysCollideImpl*>(pCollide));
    }

    int CollideWrite(char* pDest, CPhysCollide* pCollide, bool /*bSwap*/ = false) override
    {
        if (!pDest || !pCollide) return 0;
        return WriteCollide(pDest, reinterpret_cast<CPhysCollideImpl*>(pCollide));
    }

    void CollideGetAABB(Vector* pMins, Vector* pMaxs,
                        const CPhysCollide* pCollide,
                        const Vector& /*origin*/, const QAngle& /*angles*/) override
    {
        if (!pCollide) {
            *pMins = *pMaxs = vec3_origin;
            return;
        }
        reinterpret_cast<const CPhysCollideImpl*>(pCollide)->GetAABB(pMins, pMaxs);
    }

    void CollideGetMassCenter(CPhysCollide* pCollide, Vector* pOutMassCenter) override
    {
        if (!pCollide) { *pOutMassCenter = vec3_origin; return; }
        *pOutMassCenter = reinterpret_cast<CPhysCollideImpl*>(pCollide)->massCenter;
    }

    void CollideSetMassCenter(CPhysCollide* pCollide, const Vector& massCenter) override
    {
        if (!pCollide) return;
        CPhysCollideImpl* col = reinterpret_cast<CPhysCollideImpl*>(pCollide);
        col->massCenter = massCenter;
        col->massCenterOverridden = true;
    }

    // -- Stubs for methods not used by studiomdl --

    CPhysConvex* ConvexFromPlanes(float*, int, float) override { return nullptr; }
    CPhysConvex* BBoxToConvex(const Vector&, const Vector&) override { return nullptr; }
    CPhysConvex* ConvexFromConvexPolyhedron(const CPolyhedron&) override { return nullptr; }
    void ConvexesFromConvexPolygon(const Vector&, const Vector*, int, CPhysConvex**) override {}
    CPhysPolysoup* PolysoupCreate() override { return nullptr; }
    void PolysoupDestroy(CPhysPolysoup*) override {}
    void PolysoupAddTriangle(CPhysPolysoup*, const Vector&, const Vector&, const Vector&, int) override {}
    CPhysCollide* ConvertPolysoupToCollide(CPhysPolysoup*, bool) override { return nullptr; }
    CPhysCollide* UnserializeCollide(char*, int, int) override { return nullptr; }
    float CollideVolume(CPhysCollide*) override { return 0; }
    float CollideSurfaceArea(CPhysCollide*) override { return 0; }
    Vector CollideGetExtent(const CPhysCollide*, const Vector&, const QAngle&, const Vector&) override { return vec3_origin; }
    int CollideIndex(const CPhysCollide*) override { return 0; }
    CPhysCollide* BBoxToCollide(const Vector&, const Vector&) override { return nullptr; }
    int GetConvexesUsedInCollideable(const CPhysCollide*, CPhysConvex**, int) override { return 0; }
    IPhysicsCollision* ThreadContextCreate() override { return this; }
    void ThreadContextDestroy(IPhysicsCollision*) override {}
    CPhysCollide* CreateVirtualMesh(const void*) override { return nullptr; }
    bool SupportsVirtualMesh() override { return false; }
    bool GetBBoxCacheSize(int* a, int* b) override { if(a)*a=0; if(b)*b=0; return false; }
    CPolyhedron* PolyhedronFromConvex(CPhysConvex* const, bool) override { return nullptr; }
    void OutputDebugInfo(const CPhysCollide*) override {}
    unsigned int ReadStat(int) override { return 0; }
    float CollideGetRadius(const CPhysCollide*) override { return 0; }
    void* VCollideAllocUserData(vcollide_t*, size_t) override { return nullptr; }
    void VCollideFreeUserData(vcollide_t*) override {}
    void VCollideCheck(vcollide_t*, const char*) override {}
    Vector CollideGetOrthographicAreas(const CPhysCollide*) override { return Vector(1,1,1); }
    void CollideSetOrthographicAreas(CPhysCollide*, const Vector&) override {}
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

static CPhysicsCollisionMini g_MiniPhysicsCollision;

IPhysicsCollision* CreateMiniPhysicsCollision()
{
    return &g_MiniPhysicsCollision;
}
