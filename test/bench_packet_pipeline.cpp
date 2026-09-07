// Scalar/packet A/B using the production rasterizer and its actual FS batches.
// Build pipeline_runtime.cpp with SHADER_PACKET_BENCH; normal builds have no hooks.
#include "../src/runtime/pipeline_runtime.h"
#include "../src/runtime/pipeline_abi.h"
#include "../src/common/packet_width.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <vector>
#include <omp.h>
#ifdef PACKET_BENCH_MESH
#include "vk_host/icosphere.h"
extern "C" float uKd[4];
#endif
extern "C" float uTime;
extern "C" const int __shader_packet_width;
extern "C" void fs_packet(const float*, const float*, float*, int*) __attribute__((weak));
bool packet_bench_enabled = false;
bool packet_bench_capture = false;
constexpr int PW = SHADER_PACKET_WIDTH;
constexpr int NV = PIPELINE_MAX_VARYINGS, NO = PIPELINE_MAX_FS_OUT;
using Clock = std::chrono::steady_clock;
struct Fragment { float frag[4], vary[NV]; };
struct Batch { int count; Fragment lane[PW]; };
static std::vector<std::vector<Batch>> captured;

void packet_bench_record(int count, int nvar, const float* vary, const float* frag) {
    Batch b{};
    b.count = count;
    for (int l = 0; l < PW; ++l) {
        for (int c = 0; c < 4; ++c) b.lane[l].frag[c] = frag[c * PW + l];
        for (int c = 0; c < nvar; ++c) b.lane[l].vary[c] = vary[c * PW + l];
    }
    captured[omp_get_thread_num()].push_back(b);
}

struct Output { std::array<float, PW * NO> rgba; std::array<int, PW> live; };
static void replay(std::vector<Batch>& batches, std::vector<Output>& result, bool packet) {
    #pragma omp parallel for schedule(static)
    for (size_t i = 0; i < batches.size(); ++i) {
        auto& b = batches[i];
        auto& dst = result[i];
        if (packet) {
            alignas(64) float v[NV * PW], f[4 * PW], out[NO * PW];
            int live[PW];
            for (int l = 0; l < PW; ++l) {
                for (int c = 0; c < vs_varying_floats; ++c) v[c * PW + l] = b.lane[l].vary[c];
                for (int c = 0; c < 4; ++c) f[c * PW + l] = b.lane[l].frag[c];
            }
            fs_packet(v, f, out, live);
            for (int l = 0; l < b.count; ++l) {
                dst.live[l] = !!live[l];
                for (int c = 0; c < fs_output_floats; ++c)
                    dst.rgba[l * NO + c] = out[c * PW + l];
            }
        } else {
            for (int l = 0; l < b.count; ++l) {
                double d[NO]{};
                fs_invoke(b.lane[l].frag, b.lane[l].vary, dst.rgba.data() + l * NO, d);
                // Current scalar ABI has no discard flag. Runner excludes discard shaders.
                dst.live[l] = 1;
            }
        }
    }
}

static int positive(const char* s) {
    char* e = nullptr; long n = std::strtol(s, &e, 10);
    if (!*s || *e || n < 1 || n > 16384) { std::fprintf(stderr, "invalid positive argument: %s\n", s); std::exit(2); }
    return int(n);
}
int main(int argc, char** argv) {
    int width = 512, height = 512, frames = 60, rounds = 5, threads = 1;
    std::string phase = "all", mode = "both";
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc) return 2;
        std::string k = argv[i]; const char* v = argv[i + 1];
        if (k == "--width") width = positive(v);
        else if (k == "--height") height = positive(v);
        else if (k == "--frames") frames = positive(v);
        else if (k == "--rounds") rounds = positive(v);
        else if (k == "--threads") threads = positive(v);
        else if (k == "--phase") phase = v;
        else if (k == "--mode") mode = v;
        else return 2;
    }
    if ((phase != "all" && phase != "check" && phase != "replay" && phase != "pipeline") ||
        (mode != "both" && mode != "scalar" && mode != "packet")) return 2;
    if (__shader_packet_width != PW || vs_varying_floats > NV || fs_output_floats > NO ||
        fs_output_floats < 3 || fs_output_doubles != 0) {
        std::fprintf(stderr, "unsupported ABI or packet width mismatch\n"); return 2;
    }
    // The experiment changes only FS dispatch; keep VS scalar in every phase.
    unsetenv("SHADER_PACKET");
    omp_set_dynamic(0); omp_set_num_threads(threads);
    int actual = 0;
    #pragma omp parallel
    {
        #pragma omp single
        actual = omp_get_num_threads();
    }
    if (actual != threads) return 2;
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::printf("{\"kind\":\"configuration\",\"width\":%d,\"height\":%d,\"frames\":%d,\"fps\":30,\"rounds\":%d,\"threads\":%d,\"packet_width\":%d,\"vlen_bits\":%u,\"fs_packet\":%s,\"vs_mode\":\"scalar\",\"phase\":\"%s\",\"mode\":\"%s\"}\n",
        width,height,frames,rounds,threads,PW,get_vlen_bits(),fs_packet?"true":"false",phase.c_str(),mode.c_str());
    PipelineDesc desc{width,height,6,nullptr,nullptr,0};
#ifdef PACKET_BENCH_MESH
    auto mesh = icosphere::generate(3);
    std::vector<float> vertices;
    for (const auto& v : mesh.vertices) {
        vertices.insert(vertices.end(),v.pos,v.pos+3);
        vertices.insert(vertices.end(),v.normal,v.normal+3);
        vertices.insert(vertices.end(),v.uv,v.uv+2);
    }
    if (vs_input_floats != 8) return 2;
    desc = {width,height,int(mesh.vertices.size()),vertices.data(),mesh.indices.data(),int(mesh.indices.size())};
    // Same default mesh and material as rv_host_mesh.cpp (icosphere:3).
    const float white[4] = {1,1,1,1}; bind_texture(0,white,1,1);
    const float kd[4] = {210.f/255,170.f/255,130.f/255,1}; std::copy(kd,kd+4,uKd);
#else
    if (vs_input_floats != 0) { std::fprintf(stderr,"shader requires a scene adapter\n"); return 2; }
#endif
    std::vector<unsigned char> img(size_t(width)*height*3), reference(img.size());
    auto render = [&](bool packet, float t) {
        packet_bench_enabled = packet; uTime = t; render_pipeline(desc,img.data());
    };
    bool failed = false;
    double checksum = 0;
    std::mt19937 rng(12345);
    if (!fs_packet) {
        std::puts("{\"kind\":\"skip\",\"reason\":\"no fs_packet in linked object; scalar fallback only\"}");
        if (mode == "packet") return 3;
    }
    const bool check = phase == "all" || phase == "check";
    const bool timeReplay = phase == "all" || phase == "replay";
    if (fs_packet && (check || timeReplay)) {
        captured.resize(threads);
        // Validate the complete animation sequence plus t=5, beyond the default clip.
        for (int frame = 0; frame < frames + int(check); ++frame) {
            float t = frame == frames ? 5.f : frame/30.f;
            render(false,t); reference = img;
            for (auto& v : captured) v.clear();
            packet_bench_capture = true; render(true,t); packet_bench_capture = false;
            std::vector<Batch> batches;
            for (auto& v : captured) batches.insert(batches.end(),v.begin(),v.end());
            if (batches.empty()) { std::fprintf(stderr,"capture produced no fragments\n"); return 2; }
            std::vector<Output> output[2] = {std::vector<Output>(batches.size()),std::vector<Output>(batches.size())};
            replay(batches,output[0],false); replay(batches,output[1],true);
            if (check) {
                size_t mismatches=0, masks=0, nonfinite=0, components=0, pixels=0, lanes=0;
                double maxAbs=0,maxRel=0; int maxRgb=0;
                for (size_t i=0;i<img.size();++i) {
                    int e=std::abs(int(img[i])-reference[i]); maxRgb=std::max(maxRgb,e); pixels += e>1;
                }
                for (size_t i=0;i<batches.size();++i) for (int l=0;l<batches[i].count;++l) {
                    ++lanes; masks += output[0][i].live[l] != output[1][i].live[l];
                    for (int c=0;c<fs_output_floats;++c) {
                        double a=output[0][i].rgba[l*NO+c], b=output[1][i].rgba[l*NO+c];
                        ++components;
                        if (!std::isfinite(a) || !std::isfinite(b)) { ++nonfinite; ++mismatches; continue; }
                        double e=std::abs(a-b), scale=std::max(std::abs(a),std::abs(b));
                        maxAbs=std::max(maxAbs,e); maxRel=std::max(maxRel,e/std::max(scale,1e-12));
                        mismatches += e > 1e-4 + 1e-4*scale;
                    }
                }
                failed |= mismatches || masks || pixels;
                std::printf("{\"kind\":\"check\",\"time\":%.9g,\"fragments\":%zu,\"components\":%zu,\"mismatches\":%zu,\"mask_mismatches\":%zu,\"nonfinite\":%zu,\"max_abs\":%.9g,\"max_rel\":%.9g,\"rgb_components_over_1\":%zu,\"max_rgb_error\":%d}\n",t,lanes,components,mismatches,masks,nonfinite,maxAbs,maxRel,pixels,maxRgb);
            }
            if (timeReplay && frame < frames) {
                // Warm both modes on this captured frame; packing/scattering is timed.
                replay(batches,output[0],false); replay(batches,output[1],true);
                for (int r=0;r<rounds;++r) {
                    bool first = rng() & 1;
                    for (int second=0;second<2;++second) {
                        bool p=first ^ bool(second);
                        if (mode != "both" && (p != (mode == "packet"))) continue;
                        auto start=Clock::now(); replay(batches,output[p],p);
                        double ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
                        for (size_t i=0;i<batches.size();++i) for (int l=0;l<batches[i].count;++l) {
                            float value = output[p][i].rgba[l*NO];
                            if (std::isfinite(value)) checksum += value;
                        }
                        std::printf("{\"kind\":\"sample\",\"phase\":\"replay\",\"mode\":\"%s\",\"round\":%d,\"frame\":%d,\"ms\":%.9g}\n",p?"packet":"scalar",r,frame,ms);
                    }
                }
            }
        }
        captured.clear(); captured.shrink_to_fit();
    }
    if (phase == "all" || phase == "pipeline") {
        // Warm every frame in each measured mode once before repeated sequences.
        for (bool p : {false,true}) {
            if ((p && !fs_packet) || (mode != "both" && p != (mode == "packet"))) continue;
            for (int f=0;f<frames;++f) render(p,f/30.f);
        }
        for (int r=0;r<rounds;++r) {
            bool first=rng()&1;
            for (int second=0;second<2;++second) {
                bool p=first ^ bool(second);
                if ((p && !fs_packet) || (mode != "both" && p != (mode == "packet"))) continue;
                double total=0;
                for (int f=0;f<frames;++f) {
                    packet_bench_enabled=p; uTime=f/30.f;
                    auto start=Clock::now(); render_pipeline(desc,img.data());
                    total += std::chrono::duration<double,std::milli>(Clock::now()-start).count();
                    checksum += std::accumulate(img.begin(),img.end(),0.0);
                }
                std::printf("{\"kind\":\"sample\",\"phase\":\"pipeline\",\"mode\":\"%s\",\"round\":%d,\"ms\":%.9g}\n",p?"packet":"scalar",r,total/frames);
            }
        }
    }
    std::printf("{\"kind\":\"complete\",\"validation\":\"%s\",\"checksum\":%.12g}\n",check&&fs_packet?(failed?"failed":"passed"):"not_run",checksum);
    return failed ? 1 : 0;
}
