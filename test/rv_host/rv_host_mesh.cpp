// rv_host_mesh.cpp — RISC-V CPU mesh renderer.
// Drives the compiled mesh VS+FS through the software rasterizer with an
// indexed VBO+IBO. Same shader source as the Vulkan side, so each frame is
// directly comparable. Runs under QEMU during benchmarks.
//
// CLI args: <name> <nframes> <mesh-spec>
//   <mesh-spec>:
//     icosphere:N   — generated unit icosphere (N subdivision levels)
//     <path.obj>    — Wavefront OBJ (positions + normals; missing normals computed)

#include "../vk_host/icosphere.h"
#include "../vk_host/mesh_data.h"
#include "../vk_host/obj_loader.h"

#include "../../src/runtime/pipeline_runtime.h"
#include "../../src/runtime/pipeline_abi.h"
#include "../../src/common/error_utils.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "../vk_host/stb_image.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifndef WIDTH
#define WIDTH 512
#endif
#ifndef HEIGHT
#define HEIGHT 512
#endif
#ifndef NFRAMES
#define NFRAMES 300
#endif
#ifndef FPS
#define FPS 30
#endif
#ifndef ANIM_NAME
#define ANIM_NAME "mesh"
#endif

// Shader uniforms — defined inside the compiled mesh_rv.o; we just write
// to them before each frame / each draw range.
extern "C" float uTime;
// Per-draw material colour. uKd[3] is unused (pad to vec4 for std140 parity
// with the GPU push-constant layout).
extern "C" float uKd[4];

static Mesh loadMesh(const std::string& spec) {
    if (spec.rfind("icosphere:", 0) == 0) {
        int subs = std::atoi(spec.c_str() + 10);
        if (subs < 0) subs = 0;
        if (subs > 7) subs = 7;
        return icosphere::generate(subs);
    }
    Mesh m;
    if (!obj::load(spec.c_str(), m)) std::exit(1);
    obj::normalize_to_unit(m);
    return m;
}

int main(int argc, char** argv) {
    const char* name      = (argc > 1) ? argv[1] : ANIM_NAME;
    int          nframes  = (argc > 2) ? std::atoi(argv[2]) : NFRAMES;
    std::string  meshSpec = (argc > 3) ? argv[3] : "icosphere:3";

    Mesh mesh = loadMesh(meshSpec);
    if (mesh.vertices.empty()) {
        logError("Mesh has no vertices");
        return 1;
    }

    // Runtime check: shader's per-vertex attribute footprint must match the
    // Vertex layout (3 pos + 3 normal + 2 uv = 8 floats).
    if (vs_input_floats != 8) {
        logError("Shader vs_input_floats != 8 (Vertex layout mismatch)");
        return 1;
    }

    // Flatten Vertex array into a tightly-packed float buffer (pos+normal+uv).
    std::vector<float> vbuf(mesh.vertices.size() * 8);
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        vbuf[i*8 + 0] = mesh.vertices[i].pos[0];
        vbuf[i*8 + 1] = mesh.vertices[i].pos[1];
        vbuf[i*8 + 2] = mesh.vertices[i].pos[2];
        vbuf[i*8 + 3] = mesh.vertices[i].normal[0];
        vbuf[i*8 + 4] = mesh.vertices[i].normal[1];
        vbuf[i*8 + 5] = mesh.vertices[i].normal[2];
        vbuf[i*8 + 6] = mesh.vertices[i].uv[0];
        vbuf[i*8 + 7] = mesh.vertices[i].uv[1];
    }

    // ── Per-material textures ────────────────────────────────────────────────
    // Load every material's diffuse map up-front. Materials without map_Kd
    // get a 1×1 white fallback — uKd then carries their colour. When the
    // mesh has no MTL at all (bunny, sphere) we synthesise a single
    // "default" material with a warm-beige Kd so they keep their previous
    // cream-plastic look.
    struct MatTex {
        std::vector<float> rgba;
        int                w  = 1, h = 1;
    };
    std::vector<MatTex> matTex(mesh.materials.size());

    auto loadMatTex = [](const std::string& path, MatTex& mt) -> bool {
        int n = 0;
        uint8_t* raw = stbi_load(path.c_str(), &mt.w, &mt.h, &n, 4);
        if (!raw) {
            std::cerr << "Failed to load texture " << path << ": "
                      << stbi_failure_reason() << "\n";
            return false;
        }
        mt.rgba.resize((size_t)mt.w * (size_t)mt.h * 4);
        for (size_t i = 0; i < mt.rgba.size(); ++i)
            mt.rgba[i] = raw[i] * (1.0f / 255.0f);
        stbi_image_free(raw);
        std::cerr << "[mesh] texture " << path << ": "
                  << mt.w << "x" << mt.h << " RGBA float\n";
        return true;
    };

    for (size_t m = 0; m < mesh.materials.size(); ++m) {
        if (!mesh.materials[m].diffuseMap.empty()) {
            if (!loadMatTex(mesh.materials[m].diffuseMap, matTex[m])) return 1;
        } else {
            matTex[m].rgba = {1.f, 1.f, 1.f, 1.f};   // 1×1 white
            std::cerr << "[mesh] material \"" << mesh.materials[m].name
                      << "\" has no map_Kd — using 1x1 white fallback\n";
        }
    }
    // Fallback for un-MTL'd meshes: single "default" material slot with a
    // visibly-warm tan Kd. The previous (234,214,184) was too close to white —
    // additive spec+rim contributions saturated highlights and the bunny read
    // as white. (210,170,130) gives the bunny a clearly tinted look without
    // tipping into clay/terracotta territory.
    bool hasMTL = !mesh.materials.empty();
    MatTex defaultTex;
    defaultTex.rgba = {1.f, 1.f, 1.f, 1.f};
    float defaultKd[4] = { 210.f/255, 170.f/255, 130.f/255, 1.f };

    PipelineDesc desc{
        WIDTH, HEIGHT,
        /*vert_count=*/(int)mesh.vertices.size(),
        /*vbuf=*/vbuf.data(),
        /*indices=*/mesh.indices.data(),
        /*index_count=*/(int)mesh.indices.size(),
    };

    mkdir("result", 0755);
    std::vector<unsigned char> img(WIDTH * HEIGHT * 3);

    char ff_cmd[512];
    std::snprintf(ff_cmd, sizeof(ff_cmd),
        "ffmpeg -y -f rawvideo -pixel_format rgb24 -video_size %dx%d "
        "-framerate %d -i pipe:0 "
        "-c:v libx264 -pix_fmt yuv420p -crf 20 result/%s_rv.mp4 2>/dev/null",
        WIDTH, HEIGHT, FPS, name);
    FILE* ffpipe = nullptr;

    double total_ms = 0.0;
    for (int frame = 0; frame < nframes; ++frame) {
        uTime = frame / (float)FPS;
        auto t0 = std::chrono::high_resolution_clock::now();

        // Per-range draw loop. Each iteration sets uKd + binds texture for
        // its material then dispatches the rasterizer over that range.
        // `clear=true` only on the first iteration; subsequent ranges
        // accumulate into the same framebuffer + zbuffer.
        bool firstRange = true;
        auto drawRange = [&](uint32_t fi, uint32_t ic, int matId) {
            const MatTex* tx;
            const float* kd;
            if (!hasMTL) {
                tx = &defaultTex;
                kd = defaultKd;
            } else {
                int m = (matId >= 0 && matId < (int)mesh.materials.size())
                          ? matId : 0;
                tx = &matTex[m];
                kd = mesh.materials[m].diffuse;
            }
            uKd[0] = kd[0]; uKd[1] = kd[1]; uKd[2] = kd[2]; uKd[3] = 1.f;
            bind_texture(0, tx->rgba.data(), tx->w, tx->h);
            desc.first_index  = (int)fi;
            desc.index_count  = (int)ic;
            desc.clear        = firstRange;
            render_pipeline(desc, img.data());
            firstRange = false;
        };

        if (mesh.ranges.empty()) {
            drawRange(0, (uint32_t)mesh.indices.size(), -1);
        } else {
            for (const auto& r : mesh.ranges)
                drawRange(r.firstIndex, r.indexCount, r.materialId);
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        total_ms += ms;

        if (!ffpipe) ffpipe = popen(ff_cmd, "w");
        if (ffpipe) std::fwrite(img.data(), 1, img.size(), ffpipe);

        // Write PPM at a fixed frame index so cross-backend comparisons land
        // on the same rotation regardless of NFRAMES (RV defaults to 60, GPU
        // to 300). Falls back to nframes/2 for very short runs.
        int ppm_frame = (nframes >= 60) ? 30 : (nframes / 2);
        if (frame == ppm_frame) {
            char ppm[256];
            std::snprintf(ppm, sizeof(ppm), "result/%s_rv.ppm", name);
            FILE* f = std::fopen(ppm, "wb");
            if (f) {
                std::fprintf(f, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
                std::fwrite(img.data(), 1, img.size(), f);
                std::fclose(f);
            }
        }
    }
    if (ffpipe) pclose(ffpipe);

    std::cout << "[" << name << "] tris: " << mesh.triangleCount()
              << ", verts: " << mesh.vertices.size()
              << ", RISC-V avg: " << total_ms / nframes << " ms/frame ("
              << 1000.0 * nframes / total_ms << " fps simulated)\n";
    std::cout << "[" << name << "] MP4: result/" << name << "_rv.mp4\n";
    return 0;
}
