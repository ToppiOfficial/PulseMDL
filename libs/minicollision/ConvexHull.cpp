// Incremental 3D convex hull.
// Builds the hull of a small point cloud by starting with an initial tetrahedron
// and expanding face-by-face. O(n^2) in the worst case but fine for collision meshes.

#include "ConvexHull.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <unordered_map>

static const float CH_EPS = 1e-6f;

// ---------------------------------------------------------------------------
// Internal math helpers
// ---------------------------------------------------------------------------

struct V3 {
    float x, y, z;
    V3() : x(0), y(0), z(0) {}
    V3(float x, float y, float z) : x(x), y(y), z(z) {}
    V3 operator-(const V3& o) const { return { x-o.x, y-o.y, z-o.z }; }
    V3 operator+(const V3& o) const { return { x+o.x, y+o.y, z+o.z }; }
    V3 operator*(float s)     const { return { x*s,   y*s,   z*s   }; }
    float dot  (const V3& o) const { return x*o.x + y*o.y + z*o.z; }
    V3    cross(const V3& o) const {
        return { y*o.z - z*o.y, z*o.x - x*o.z, x*o.y - y*o.x };
    }
    float lenSq() const { return x*x + y*y + z*z; }
    float len()   const { return sqrtf(lenSq()); }
    V3 normalized() const {
        float l = len();
        return l > CH_EPS ? *this * (1.0f / l) : V3{};
    }
};

// ---------------------------------------------------------------------------
// Face
// ---------------------------------------------------------------------------

struct CHFace {
    int   v[3];   // CCW from outside
    V3    normal;
    float d;      // plane: normal.dot(p) = d for p on plane
    bool  alive;
};

static CHFace MakeFace(const std::vector<V3>& pts, int a, int b, int c, const V3& insidePt)
{
    CHFace f;
    f.v[0] = a; f.v[1] = b; f.v[2] = c;
    f.alive = true;
    V3 e1 = pts[b] - pts[a];
    V3 e2 = pts[c] - pts[a];
    f.normal = e1.cross(e2).normalized();
    if (f.normal.lenSq() < CH_EPS * CH_EPS) {
        f.alive = false;
        f.d = 0;
        return f;
    }
    f.d = f.normal.dot(pts[a]);
    // Outward means normal points AWAY from insidePt
    if (f.normal.dot(insidePt) > f.d + CH_EPS) {
        std::swap(f.v[1], f.v[2]);
        f.normal = f.normal * -1.0f;
        f.d      = -f.d;
    }
    return f;
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

ConvexHullResult BuildConvexHull(const float* rawPts, int numPts)
{
    ConvexHullResult result;

    if (numPts < 4) {
        result.degenerate = true;
        return result;
    }

    // Deduplicate input vertices
    std::vector<V3> pts;
    pts.reserve(numPts);
    for (int i = 0; i < numPts; i++) {
        V3 p{ rawPts[i*3], rawPts[i*3+1], rawPts[i*3+2] };
        bool dup = false;
        for (const V3& q : pts) {
            if ((p - q).lenSq() < CH_EPS * CH_EPS) { dup = true; break; }
        }
        if (!dup) pts.push_back(p);
    }

    int n = (int)pts.size();
    if (n < 4) { result.degenerate = true; return result; }

    // ------------------------------------------------------------------
    // Find initial tetrahedron
    // ------------------------------------------------------------------

    // (1) Extremal point in +x
    int ia = 0;
    for (int i = 1; i < n; i++) if (pts[i].x < pts[ia].x) ia = i;

    // (2) Farthest from ia
    int ib = -1; float best = -1;
    for (int i = 0; i < n; i++) {
        if (i == ia) continue;
        float d = (pts[i] - pts[ia]).lenSq();
        if (d > best) { best = d; ib = i; }
    }
    if (best < CH_EPS * CH_EPS) { result.degenerate = true; return result; }

    // (3) Farthest from line ia-ib
    V3 ab = (pts[ib] - pts[ia]).normalized();
    int ic = -1; best = -1;
    for (int i = 0; i < n; i++) {
        if (i == ia || i == ib) continue;
        V3 v = pts[i] - pts[ia];
        V3 perp = v - ab * ab.dot(v);
        float d = perp.lenSq();
        if (d > best) { best = d; ic = i; }
    }
    if (ic < 0 || best < CH_EPS * CH_EPS) { result.degenerate = true; return result; }

    // (4) Farthest from plane ia-ib-ic
    V3 pn = (pts[ib] - pts[ia]).cross(pts[ic] - pts[ia]).normalized();
    float pd = pn.dot(pts[ia]);
    int id = -1; best = -1;
    for (int i = 0; i < n; i++) {
        if (i == ia || i == ib || i == ic) continue;
        float d = fabsf(pn.dot(pts[i]) - pd);
        if (d > best) { best = d; id = i; }
    }
    if (id < 0 || best < CH_EPS) { result.degenerate = true; return result; }

    V3 centroid = (pts[ia] + pts[ib] + pts[ic] + pts[id]) * 0.25f;

    // Build initial 4 faces of the tetrahedron
    std::vector<CHFace> faces;
    faces.reserve(32);
    faces.push_back(MakeFace(pts, ia, ib, ic, centroid));
    faces.push_back(MakeFace(pts, ia, ib, id, centroid));
    faces.push_back(MakeFace(pts, ia, ic, id, centroid));
    faces.push_back(MakeFace(pts, ib, ic, id, centroid));

    // Directed edge -> face index map: key = (uint64_t)v0 << 32 | v1
    std::unordered_map<uint64_t, int> edgeToFace;
    edgeToFace.reserve(64);

    auto RegFace = [&](int fi) {
        const CHFace& f = faces[fi];
        if (!f.alive) return;
        for (int e = 0; e < 3; e++) {
            int v0 = f.v[e], v1 = f.v[(e+1)%3];
            edgeToFace[((uint64_t)v0 << 32) | (uint32_t)v1] = fi;
        }
    };
    auto UnregFace = [&](int fi) {
        const CHFace& f = faces[fi];
        if (!f.alive) return;
        for (int e = 0; e < 3; e++) {
            int v0 = f.v[e], v1 = f.v[(e+1)%3];
            edgeToFace.erase(((uint64_t)v0 << 32) | (uint32_t)v1);
        }
    };

    for (int fi = 0; fi < (int)faces.size(); fi++) RegFace(fi);

    // ------------------------------------------------------------------
    // Incrementally add remaining points
    // ------------------------------------------------------------------

    for (int i = 0; i < n; i++) {
        if (i == ia || i == ib || i == ic || i == id) continue;
        const V3& p = pts[i];

        // Find visible faces (faces the new point is above)
        std::vector<int> visible;
        for (int fi = 0; fi < (int)faces.size(); fi++) {
            const CHFace& f = faces[fi];
            if (f.alive && f.normal.dot(p) > f.d + CH_EPS)
                visible.push_back(fi);
        }
        if (visible.empty()) continue; // point is inside current hull

        // Build a quick lookup for visible set
        std::unordered_map<int,bool> visSet;
        visSet.reserve(visible.size() * 2);
        for (int fi : visible) visSet[fi] = true;

        // Find horizon edges: edges of visible faces whose reverse is in a non-visible face
        std::vector<std::pair<int,int>> horizon;
        for (int fi : visible) {
            const CHFace& f = faces[fi];
            for (int e = 0; e < 3; e++) {
                int v0 = f.v[e], v1 = f.v[(e+1)%3];
                auto it = edgeToFace.find(((uint64_t)v1 << 32) | (uint32_t)v0);
                if (it != edgeToFace.end() && !visSet.count(it->second))
                    horizon.push_back({ v0, v1 });
            }
        }

        if (horizon.empty()) continue; // can happen with degenerate inputs

        // Remove visible faces
        for (int fi : visible) {
            UnregFace(fi);
            faces[fi].alive = false;
        }

        // Add new faces from horizon edges to point i
        for (const auto& he : horizon) {
            CHFace nf = MakeFace(pts, he.first, he.second, i, centroid);
            int nfi = (int)faces.size();
            faces.push_back(nf);
            RegFace(nfi);
        }
    }

    // ------------------------------------------------------------------
    // Collect output
    // ------------------------------------------------------------------

    // Only output vertices that appear in living faces
    std::vector<int> used(n, -1);
    for (const CHFace& f : faces) {
        if (!f.alive) continue;
        for (int k = 0; k < 3; k++) {
            int vi = f.v[k];
            if (used[vi] < 0) {
                used[vi] = (int)result.vertices.size();
                result.vertices.push_back({ pts[vi].x, pts[vi].y, pts[vi].z });
            }
        }
    }

    result.indices.reserve(faces.size() * 3);
    for (const CHFace& f : faces) {
        if (!f.alive) continue;
        result.indices.push_back(used[f.v[0]]);
        result.indices.push_back(used[f.v[1]]);
        result.indices.push_back(used[f.v[2]]);
    }

    if (result.indices.empty()) result.degenerate = true;
    return result;
}
