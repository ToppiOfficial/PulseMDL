// minicollision - IPhysicsSurfaceProps implementation
// Minimal stub sufficient for studiomdl's surfaceprop usage.
// Parses surfaceprop.txt KeyValues to populate a name->index table.

#include "MiniVPhysics.h"
#include "vphysics_interface.h"

#include <vector>
#include <string>
#include <algorithm>
#include <cctype>

struct SurfacePropEntry {
    std::string name;
};

class CPhysicsSurfacePropsMini : public IPhysicsSurfaceProps
{
    std::vector<SurfacePropEntry> m_props;

    static std::string ToLower(const char* s) {
        std::string r(s);
        for (char& c : r) c = (char)tolower((unsigned char)c);
        return r;
    }

public:
    virtual ~CPhysicsSurfacePropsMini() {}

    // Parse surfaceprop.txt (Valve KeyValues text format).
    // Only extracts top-level block names; ignores all other fields.
    int ParseSurfaceData(const char* pFilename, const char* pTextfile) override
    {
        if (!pTextfile) return 0;
        int added = 0;
        const char* p = pTextfile;

        while (*p) {
            // Skip whitespace and comments
            while (*p && (isspace((unsigned char)*p) || *p == '/')) {
                if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; }
                else p++;
            }
            if (!*p) break;

            // Read block name (quoted or unquoted)
            std::string name;
            if (*p == '"') {
                p++;
                while (*p && *p != '"') name += *p++;
                if (*p == '"') p++;
            } else {
                while (*p && !isspace((unsigned char)*p) && *p != '{') name += *p++;
            }

            if (name.empty()) { p++; continue; }

            // Skip to opening brace
            while (*p && *p != '{' && *p != '"') p++;
            if (*p != '{') continue;

            // Skip entire block (handle nesting)
            int depth = 0;
            while (*p) {
                if (*p == '{') depth++;
                else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
                p++;
            }

            // Add surface prop if not already present
            std::string lname = ToLower(name.c_str());
            bool found = false;
            for (const auto& e : m_props) if (e.name == lname) { found=true; break; }
            if (!found) {
                SurfacePropEntry entry;
                entry.name = lname;
                m_props.push_back(entry);
                added++;
            }
        }

        // Always ensure "default" exists at index 0
        if (m_props.empty() || m_props[0].name != "default") {
            SurfacePropEntry def;
            def.name = "default";
            m_props.insert(m_props.begin(), def);
        }

        return added;
    }

    int SurfacePropCount() const override
    {
        return (int)m_props.size();
    }

    int GetSurfaceIndex(const char* pSurfacePropName) const override
    {
        if (!pSurfacePropName) return 0;
        std::string lname = ToLower(pSurfacePropName);
        for (int i = 0; i < (int)m_props.size(); i++) {
            if (m_props[i].name == lname) return i;
        }
        return 0; // default
    }

    void GetPhysicsProperties(int, float* density, float* thickness,
                              float* friction, float* elasticity) const override
    {
        if (density)   *density   = 1000.0f;
        if (thickness) *thickness = 0.0f;
        if (friction)  *friction  = 0.8f;
        if (elasticity)*elasticity= 0.25f;
    }

    surfacedata_t* GetSurfaceData(int) override { return nullptr; }
    const char* GetString(unsigned short) const override { return ""; }
    const char* GetPropName(int idx) const override {
        if (idx < 0 || idx >= (int)m_props.size()) return "default";
        return m_props[idx].name.c_str();
    }
    void SetWorldMaterialIndexTable(int*, int) override {}
    void GetPhysicsParameters(int, surfacephysicsparams_t*) const override {}
};

static CPhysicsSurfacePropsMini g_MiniSurfaceProps;

IPhysicsSurfaceProps* CreateMiniPhysicsSurfaceProps()
{
    return &g_MiniSurfaceProps;
}
