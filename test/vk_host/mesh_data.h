// mesh_data.h — Shared mesh data structures for the indexed-mesh demo.
// Vertex layout matches mesh_vs.src:
//   aPos    at Location 0  (vec3)
//   aNormal at Location 1  (vec3)
//   aUV     at Location 2  (vec2) — zero-filled when the OBJ has no UVs.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Vertex {
    float pos[3];
    float normal[3];
    float uv[2];
};

// One contiguous index range that should be drawn with a specific material.
// All ranges are sub-ranges of Mesh::indices in declaration order.
struct MaterialRange {
    uint32_t firstIndex;   // offset into Mesh::indices
    uint32_t indexCount;   // number of indices in the range (multiple of 3)
    int      materialId;   // index into Mesh::materials (-1 = default)
};

struct Material {
    std::string name;
    float       diffuse[3] = {1, 1, 1};   // Kd
    std::string diffuseMap;               // map_Kd basename (or empty)
};

struct Mesh {
    std::vector<Vertex>        vertices;
    std::vector<uint32_t>      indices;
    std::vector<MaterialRange> ranges;     // empty → single draw with no material switch
    std::vector<Material>      materials;

    size_t triangleCount() const { return indices.size() / 3; }
};
