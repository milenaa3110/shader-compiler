// volume_data.h — the procedural volume behind volume_fs.src, shared by the
// RISC-V render host and the Vulkan one so both backends sample identical data
// and any difference in the output is the backend's, not the input's.
//
// __tex3d_sample maps a wrapped coordinate to texel index u*(N-1), so texel
// N-1 is the image of u == 1.0. Seamless wrapping therefore requires voxel N-1
// to repeat voxel 0, which falls out of sampling the noise at phase i/(N-1)
// with lattice frequencies that are whole numbers of periods.
//
// (Vulkan's REPEAT addressing maps u to u*N - 0.5 instead, so the two samplers
// differ by a half texel. That shifts the cloud by well under one voxel and is
// not visible; it is not an attempt at bit-exact parity.)

#ifndef TEST_VOLUME_DATA_H
#define TEST_VOLUME_DATA_H

#include <cmath>
#include <cstdint>
#include <vector>

namespace voldata {

// Voxels per axis. 64^3 RGBA floats = 4 MB, small enough to stay resident.
constexpr int kN = 64;

inline float latticeHash(int x, int y, int z) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u
               + (uint32_t)z * 1442695041u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFFFFu) / (float)0xFFFFFFu;
}

// Value noise periodic over phase 1.0, with `freq` lattice cells per period.
inline float periodicNoise(float x, float y, float z, int freq) {
    float fx = x * freq, fy = y * freq, fz = z * freq;
    int ix = (int)std::floor(fx), iy = (int)std::floor(fy), iz = (int)std::floor(fz);
    float tx = fx - ix, ty = fy - iy, tz = fz - iz;
    tx = tx * tx * (3.f - 2.f * tx);
    ty = ty * ty * (3.f - 2.f * ty);
    tz = tz * tz * (3.f - 2.f * tz);

    auto wrap = [freq](int v) { return ((v % freq) + freq) % freq; };
    int x0 = wrap(ix), x1 = wrap(ix + 1);
    int y0 = wrap(iy), y1 = wrap(iy + 1);
    int z0 = wrap(iz), z1 = wrap(iz + 1);

    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    float c00 = lerp(latticeHash(x0, y0, z0), latticeHash(x1, y0, z0), tx);
    float c10 = lerp(latticeHash(x0, y1, z0), latticeHash(x1, y1, z0), tx);
    float c01 = lerp(latticeHash(x0, y0, z1), latticeHash(x1, y0, z1), tx);
    float c11 = lerp(latticeHash(x0, y1, z1), latticeHash(x1, y1, z1), tx);
    return lerp(lerp(c00, c10, ty), lerp(c01, c11, ty), tz);
}

// 4-octave FBM; every frequency divides the period, so the sum stays periodic.
inline float fbm(float x, float y, float z) {
    return periodicNoise(x, y, z, 3)  * 0.5f
         + periodicNoise(x, y, z, 6)  * 0.25f
         + periodicNoise(x, y, z, 12) * 0.155f
         + periodicNoise(x, y, z, 24) * 0.095f;
}

// Builds the N^3 RGBA volume: .w is density, .xyz is colour pre-lit by a
// per-voxel shadow ray toward the sun. Baking the lighting keeps the shader's
// per-step cost at one trilinear fetch plus the compositing math.
inline std::vector<float> make(int N = kN) {
    const int P = N - 1;   // period, in voxels

    std::vector<float> dens((size_t)N * N * N);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int z = 0; z < N; ++z)
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                float f = fbm(x / (float)P, y / (float)P, z / (float)P);
                // Threshold into wisps: everything below the cut is empty
                // space, which is what gives the cloud a silhouette instead of
                // a uniform haze.
                float d = (f - 0.42f) * 2.6f;
                dens[((size_t)z * N + y) * N + x] = d > 0.f ? d : 0.f;
            }

    const float sunX = 0.42f, sunY = 0.80f, sunZ = 0.43f;
    const float sLen = std::sqrt(sunX*sunX + sunY*sunY + sunZ*sunZ);
    const float lx = sunX / sLen, ly = sunY / sLen, lz = sunZ / sLen;

    auto densWrap = [&](int x, int y, int z) {
        x = ((x % P) + P) % P; y = ((y % P) + P) % P; z = ((z % P) + P) % P;
        return dens[((size_t)z * N + y) * N + x];
    };

    std::vector<float> vol((size_t)N * N * N * 4);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int z = 0; z < N; ++z)
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                float d = dens[((size_t)z * N + y) * N + x];

                // Optical depth along the sun ray, 12 voxel-sized steps.
                float depth = 0.f;
                for (int s = 1; s <= 12; ++s) {
                    float sx = x + lx * s, sy = y + ly * s, sz = z + lz * s;
                    depth += densWrap((int)(sx + 0.5f), (int)(sy + 0.5f),
                                      (int)(sz + 0.5f));
                }
                float light = std::exp(-depth * 0.16f);

                // Shadowed indigo → lit amber, with extra warmth in the densest
                // cores so the interior does not read flat.
                float warm = light * light;
                float* px = &vol[(((size_t)z * N + y) * N + x) * 4];
                px[0] = 0.10f + 1.05f * warm + 0.10f * d;
                px[1] = 0.13f + 0.72f * warm + 0.04f * d;
                px[2] = 0.34f + 0.38f * warm;
                px[3] = d;
            }
    return vol;
}

}  // namespace voldata

#endif  // TEST_VOLUME_DATA_H
