// obj_loader.h — Wavefront OBJ + MTL parser.
//
// Handles:
//   v  x y z              — vertex position
//   vn x y z              — vertex normal
//   vt u v                — vertex UV (texture coordinate)
//   f  i  j  k            — triangle (1-based indices)
//   f  i/_/in  j/_/jn ... — pos/uv/normal triples
//   f  i//in  ...         — pos//normal pairs
//   f  i/it   ...         — pos/uv pairs
//   usemtl <name>         — switch material; emits a new MaterialRange
//   mtllib <file.mtl>     — load companion MTL alongside the OBJ
//
// Quads are split into two triangles. Missing normals are computed from face
// geometry. Missing UVs default to (0,0).
//
// Texture paths in the MTL (`map_Kd`) are resolved leniently: first as
// written, then as basename inside the OBJ's directory.

#pragma once

#include "mesh_data.h"
#include "../../error_utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace obj {

// Strip leading directory from a path-like string.
inline std::string basename_of(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return (s == std::string::npos) ? p : p.substr(s + 1);
}

inline std::string dirname_of(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return (s == std::string::npos) ? std::string(".") : p.substr(0, s);
}

inline bool file_exists(const std::string& p) {
    std::ifstream f(p);
    return f.good();
}

// Resolve a texture path mentioned in an MTL file relative to the OBJ dir.
// Tries the written path first, then the basename inside the OBJ dir, so
// either layout (`assets/<sub>/tex.jpg` or `assets/tex.jpg`) works.
inline std::string resolve_texture_path(const std::string& objDir,
                                        const std::string& mtlMapPath) {
    std::string p1 = objDir + "/" + mtlMapPath;
    if (file_exists(p1)) return p1;
    std::string p2 = objDir + "/" + basename_of(mtlMapPath);
    if (file_exists(p2)) return p2;
    return p1;  // return the canonical path; caller handles missing files
}

inline bool load_mtl(const std::string& path, std::vector<Material>& out,
                     std::unordered_map<std::string, int>& nameToIdx,
                     const std::string& objDir) {
    std::ifstream f(path);
    if (!f) {
        logError(std::string("[mtl] cannot open ") + path);
        return false;
    }
    std::string line;
    Material* cur = nullptr;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string tag; ss >> tag;
        if (tag == "newmtl") {
            std::string name; ss >> name;
            Material m;
            m.name = name;
            out.push_back(m);
            nameToIdx[name] = (int)out.size() - 1;
            cur = &out.back();
        } else if (cur && tag == "Kd") {
            ss >> cur->diffuse[0] >> cur->diffuse[1] >> cur->diffuse[2];
        } else if (cur && tag == "map_Kd") {
            std::string p; std::getline(ss, p);
            // Trim leading whitespace.
            size_t i = p.find_first_not_of(" \t\r\n");
            if (i != std::string::npos) p = p.substr(i);
            // Trim trailing whitespace.
            size_t j = p.find_last_not_of(" \t\r\n");
            if (j != std::string::npos) p = p.substr(0, j + 1);
            cur->diffuseMap = resolve_texture_path(objDir, p);
        }
        // ignore Ka, Ks, illum, Ns, etc. — not needed for our renderer
    }
    return true;
}

inline bool load(const char* path, Mesh& out) {
    std::ifstream f(path);
    if (!f) {
        logError(std::string("[obj] cannot open ") + path);
        return false;
    }
    std::string objDir = dirname_of(path);

    std::vector<std::array<float,3>> positions;
    std::vector<std::array<float,3>> normals;
    std::vector<std::array<float,2>> uvs;

    // Combined-index → output-vertex map: encodes (posIdx, uvIdx, normalIdx)
    // into a 96-bit key (we use a string so all three indices are exact).
    std::unordered_map<std::string, uint32_t> dedup;
    auto keyOf = [](int p, int t, int n) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%d/%d/%d", p, t, n);
        return std::string(buf);
    };

    std::unordered_map<std::string, int> mtlNameToIdx;

    auto pushVertex = [&](int pi, int ti, int ni) -> uint32_t {
        std::string k = keyOf(pi, ti, ni);
        auto it = dedup.find(k);
        if (it != dedup.end()) return it->second;
        Vertex v{};
        v.pos[0] = positions[pi][0];
        v.pos[1] = positions[pi][1];
        v.pos[2] = positions[pi][2];
        if (ni >= 0 && ni < (int)normals.size()) {
            v.normal[0] = normals[ni][0];
            v.normal[1] = normals[ni][1];
            v.normal[2] = normals[ni][2];
        }
        if (ti >= 0 && ti < (int)uvs.size()) {
            v.uv[0] = uvs[ti][0];
            // OBJ UV origin is bottom-left; image origin is top-left → flip V.
            v.uv[1] = 1.0f - uvs[ti][1];
        }
        uint32_t id = (uint32_t)out.vertices.size();
        out.vertices.push_back(v);
        dedup[k] = id;
        return id;
    };

    int activeMaterial = -1;
    auto closeRangeIfNeeded = [&]() {
        if (out.ranges.empty()) return;
        MaterialRange& r = out.ranges.back();
        r.indexCount = (uint32_t)out.indices.size() - r.firstIndex;
    };

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string tag; ss >> tag;
        if (tag == "v") {
            std::array<float,3> p; ss >> p[0] >> p[1] >> p[2];
            positions.push_back(p);
        } else if (tag == "vn") {
            std::array<float,3> n; ss >> n[0] >> n[1] >> n[2];
            normals.push_back(n);
        } else if (tag == "vt") {
            std::array<float,2> t; ss >> t[0] >> t[1];
            uvs.push_back(t);
        } else if (tag == "mtllib") {
            // Read the rest of the line so filenames containing spaces
            // (e.g. "Rumba Dancing.mtl") parse correctly. `>>` would only
            // consume the first whitespace-delimited token.
            std::string mlib; std::getline(ss, mlib);
            size_t a = mlib.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) continue;
            size_t b = mlib.find_last_not_of(" \t\r\n");
            mlib = mlib.substr(a, b - a + 1);
            std::string mtlPath = objDir + "/" + mlib;
            load_mtl(mtlPath, out.materials, mtlNameToIdx, objDir);
        } else if (tag == "usemtl") {
            std::string mname; ss >> mname;
            auto it = mtlNameToIdx.find(mname);
            int newMatId = (it == mtlNameToIdx.end()) ? -1 : it->second;
            if (newMatId != activeMaterial) {
                closeRangeIfNeeded();
                MaterialRange r;
                r.firstIndex = (uint32_t)out.indices.size();
                r.indexCount = 0;
                r.materialId = newMatId;
                out.ranges.push_back(r);
                activeMaterial = newMatId;
            }
        } else if (tag == "f") {
            std::vector<std::array<int,3>> faceVerts;  // (posIdx, uvIdx, normIdx)
            std::string token;
            while (ss >> token) {
                int pi = 0, ti = 0, ni = 0;
                if (std::sscanf(token.c_str(), "%d/%d/%d", &pi, &ti, &ni) == 3) {}
                else if (std::sscanf(token.c_str(), "%d//%d", &pi, &ni) == 2) { ti = 0; }
                else if (std::sscanf(token.c_str(), "%d/%d",  &pi, &ti) == 2) { ni = 0; }
                else if (std::sscanf(token.c_str(), "%d",     &pi)      == 1) { ti = 0; ni = 0; }
                else continue;
                int posIdx = pi > 0 ? pi - 1 : (int)positions.size() + pi;
                int uvIdx  = ti > 0 ? ti - 1 : (ti < 0 ? (int)uvs.size()     + ti : -1);
                int nrmIdx = ni > 0 ? ni - 1 : (ni < 0 ? (int)normals.size() + ni : -1);
                faceVerts.push_back({posIdx, uvIdx, nrmIdx});
            }
            if (faceVerts.size() < 3) continue;
            for (size_t i = 1; i + 1 < faceVerts.size(); ++i) {
                uint32_t a = pushVertex(faceVerts[0].at(0), faceVerts[0].at(1), faceVerts[0].at(2));
                uint32_t b = pushVertex(faceVerts[i].at(0), faceVerts[i].at(1), faceVerts[i].at(2));
                uint32_t c = pushVertex(faceVerts[i+1].at(0), faceVerts[i+1].at(1), faceVerts[i+1].at(2));
                out.indices.push_back(a);
                out.indices.push_back(b);
                out.indices.push_back(c);
            }
        }
        // ignore vt, g, s, mtllib, etc.
    }
    closeRangeIfNeeded();

    // If no normals were declared, compute per-vertex normals by averaging
    // face normals across each vertex's incident triangles.
    if (normals.empty()) {
        std::vector<std::array<float,3>> accum(out.vertices.size(), {0,0,0});
        for (size_t i = 0; i + 2 < out.indices.size(); i += 3) {
            uint32_t ia = out.indices[i], ib = out.indices[i+1], ic = out.indices[i+2];
            const float* A = out.vertices[ia].pos;
            const float* B = out.vertices[ib].pos;
            const float* C = out.vertices[ic].pos;
            float ux = B[0]-A[0], uy = B[1]-A[1], uz = B[2]-A[2];
            float vx = C[0]-A[0], vy = C[1]-A[1], vz = C[2]-A[2];
            float nx = uy*vz - uz*vy;
            float ny = uz*vx - ux*vz;
            float nz = ux*vy - uy*vx;
            for (uint32_t k : {ia, ib, ic}) {
                accum[k][0] += nx; accum[k][1] += ny; accum[k][2] += nz;
            }
        }
        for (size_t i = 0; i < out.vertices.size(); ++i) {
            float nx = accum[i][0], ny = accum[i][1], nz = accum[i][2];
            float L = std::sqrt(nx*nx + ny*ny + nz*nz);
            if (L > 0) { nx /= L; ny /= L; nz /= L; }
            out.vertices[i].normal[0] = nx;
            out.vertices[i].normal[1] = ny;
            out.vertices[i].normal[2] = nz;
        }
    }

    std::cerr << "[obj] " << path << ": "
              << out.vertices.size() << " verts, "
              << out.triangleCount() << " tris, "
              << out.materials.size() << " materials, "
              << out.ranges.size() << " ranges\n";
    return true;
}

// Centre + uniform-scale the mesh so it fits in a unit-ish sphere at the origin.
inline void normalize_to_unit(Mesh& m) {
    if (m.vertices.empty()) return;
    float mn[3] = { m.vertices[0].pos[0], m.vertices[0].pos[1], m.vertices[0].pos[2] };
    float mx[3] = { mn[0], mn[1], mn[2] };
    for (auto& v : m.vertices) {
        for (int i = 0; i < 3; ++i) {
            if (v.pos[i] < mn[i]) mn[i] = v.pos[i];
            if (v.pos[i] > mx[i]) mx[i] = v.pos[i];
        }
    }
    float cx = (mn[0] + mx[0]) * 0.5f;
    float cy = (mn[1] + mx[1]) * 0.5f;
    float cz = (mn[2] + mx[2]) * 0.5f;
    float ex = mx[0] - mn[0], ey = mx[1] - mn[1], ez = mx[2] - mn[2];
    float scale = std::max(ex, std::max(ey, ez));
    if (scale <= 0) scale = 1;
    for (auto& v : m.vertices) {
        v.pos[0] = (v.pos[0] - cx) / scale * 1.5f;
        v.pos[1] = (v.pos[1] - cy) / scale * 1.5f;
        v.pos[2] = (v.pos[2] - cz) / scale * 1.5f;
    }
}

}  // namespace obj
