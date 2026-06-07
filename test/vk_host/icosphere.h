// icosphere.h — Generate a unit-sphere mesh via icosahedron subdivision.
// `subdivisions = 0` returns the 20-triangle base icosahedron.
// Each subdivision step roughly quadruples the triangle count:
//   0 → 20 tris      3 → 1280 tris    5 → 20480 tris
//   1 → 80 tris      4 → 5120 tris    6 → 81920 tris
//   2 → 320 tris

#pragma once

#include "mesh_data.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace icosphere {

inline void normalize3(float v[3]) {
    float L = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (L > 0) { v[0] /= L; v[1] /= L; v[2] /= L; }
}

inline Mesh generate(int subdivisions) {
    Mesh m;

    // Icosahedron base — 12 vertices, 20 triangles.
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;  // golden ratio
    float verts[12][3] = {
        {-1,  t,  0}, { 1,  t,  0}, {-1, -t,  0}, { 1, -t,  0},
        { 0, -1,  t}, { 0,  1,  t}, { 0, -1, -t}, { 0,  1, -t},
        { t,  0, -1}, { t,  0,  1}, {-t,  0, -1}, {-t,  0,  1},
    };
    for (auto& v : verts) normalize3(v);

    std::vector<std::array<uint32_t,3>> tris = {
        {{ 0,11, 5}}, {{ 0, 5, 1}}, {{ 0, 1, 7}}, {{ 0, 7,10}}, {{ 0,10,11}},
        {{ 1, 5, 9}}, {{ 5,11, 4}}, {{11,10, 2}}, {{10, 7, 6}}, {{ 7, 1, 8}},
        {{ 3, 9, 4}}, {{ 3, 4, 2}}, {{ 3, 2, 6}}, {{ 3, 6, 8}}, {{ 3, 8, 9}},
        {{ 4, 9, 5}}, {{ 2, 4,11}}, {{ 6, 2,10}}, {{ 8, 6, 7}}, {{ 9, 8, 1}},
    };

    std::vector<std::array<float,3>> vs;
    vs.reserve(12);
    for (auto& v : verts) vs.push_back({{v[0], v[1], v[2]}});

    // Subdivide: split every edge at its midpoint, project to unit sphere,
    // replace each triangle with four. Cache midpoint vertex IDs per edge so
    // adjacent triangles share the new vertex (no cracks).
    auto edgeKey = [](uint32_t a, uint32_t b) -> uint64_t {
        if (a > b) std::swap(a, b);
        return ((uint64_t)a << 32) | (uint64_t)b;
    };

    for (int s = 0; s < subdivisions; ++s) {
        std::unordered_map<uint64_t, uint32_t> mid;
        std::vector<std::array<uint32_t,3>> next;
        next.reserve(tris.size() * 4);

        auto getMid = [&](uint32_t a, uint32_t b) {
            uint64_t k = edgeKey(a, b);
            auto it = mid.find(k);
            if (it != mid.end()) return it->second;
            float mv[3] = {
                (vs[a][0] + vs[b][0]) * 0.5f,
                (vs[a][1] + vs[b][1]) * 0.5f,
                (vs[a][2] + vs[b][2]) * 0.5f,
            };
            normalize3(mv);
            uint32_t id = (uint32_t)vs.size();
            vs.push_back({{mv[0], mv[1], mv[2]}});
            mid[k] = id;
            return id;
        };

        for (auto& tr : tris) {
            uint32_t a = tr[0], b = tr[1], c = tr[2];
            uint32_t ab = getMid(a, b);
            uint32_t bc = getMid(b, c);
            uint32_t ca = getMid(c, a);
            next.push_back({{a, ab, ca}});
            next.push_back({{b, bc, ab}});
            next.push_back({{c, ca, bc}});
            next.push_back({{ab, bc, ca}});
        }
        tris.swap(next);
    }

    // Emit. Normal == position for a unit sphere; UVs from spherical mapping.
    m.vertices.reserve(vs.size());
    for (auto& v : vs) {
        Vertex outV;
        outV.pos[0] = v[0]; outV.pos[1] = v[1]; outV.pos[2] = v[2];
        outV.normal[0] = v[0]; outV.normal[1] = v[1]; outV.normal[2] = v[2];
        // Equirectangular projection: u from atan2(z,x), v from y.
        outV.uv[0] = 0.5f + std::atan2(v[2], v[0]) * (0.5f / 3.14159265f);
        outV.uv[1] = 0.5f - std::asin(v[1]) * (1.0f / 3.14159265f);
        m.vertices.push_back(outV);
    }
    m.indices.reserve(tris.size() * 3);
    for (auto& t : tris) {
        m.indices.push_back(t[0]);
        m.indices.push_back(t[1]);
        m.indices.push_back(t[2]);
    }
    return m;
}

}  // namespace icosphere
