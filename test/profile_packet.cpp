// Native generated shader benchmark. Packing/scattering and OpenMP dispatch are timed.
// Full-output validation and checksums are outside the timed region; no video I/O.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <vector>
#include <omp.h>
#ifndef SHADER_PACKET_WIDTH
#define SHADER_PACKET_WIDTH 4
#endif
extern "C" void fs_invoke(float*, float*, float*, double*);
extern "C" void fs_packet(const float*, const float*, float*, int*);
extern "C" const int __shader_packet_width;
extern "C" float uTime;
constexpr int W = SHADER_PACKET_WIDTH;
using Clock = std::chrono::steady_clock;
int main(int argc, char** argv) {
    if (argc != 5) { std::fprintf(stderr, "usage: bench threads runs reps fragments\n"); return 2; }
    const int threads=std::atoi(argv[1]), runs=std::atoi(argv[2]);
    const int reps=std::atoi(argv[3]), n=std::atoi(argv[4]);
    if (threads<1 || runs<1 || reps<1 || n<W || n%W || __shader_packet_width!=W) return 2;
    omp_set_dynamic(0); omp_set_num_threads(threads);
    int actual_threads=0;
    #pragma omp parallel
    {
        #pragma omp single
        actual_threads=omp_get_num_threads();
    }
    if (actual_threads!=threads) { std::fprintf(stderr,"thread team smaller than requested\n"); return 2; }
    std::vector<float> x(n),y(n),scalar(4*n),packet(4*n);
    std::mt19937 rng(123);
    std::uniform_real_distribution<float> random(0.0f,1.0f);
    for(int i=0;i<n;++i) { x[i]=random(rng); y[i]=random(rng); }
    auto render=[&](bool use_packet) {
        if(!use_packet) {
            #pragma omp parallel for schedule(static)
            for(int i=0;i<n;++i) {
                float frag[4]={x[i]*1024,y[i]*1024,0,1},varying[2]={x[i],y[i]};
                double double_out[1]={};
                fs_invoke(frag,varying,scalar.data()+4*i,double_out);
            }
        } else {
            #pragma omp parallel for schedule(static)
            for(int i=0;i<n;i+=W) {
                alignas(64) float varying[2*W],frag[4*W],out[4*W];
                alignas(64) int live[W];
                for(int l=0;l<W;++l) {
                    varying[l]=x[i+l]; varying[W+l]=y[i+l];
                    frag[l]=x[i+l]*1024; frag[W+l]=y[i+l]*1024;
                    frag[2*W+l]=0; frag[3*W+l]=1;
                }
                fs_packet(varying,frag,out,live);
                for(int l=0;l<W;++l) for(int c=0;c<4;++c)
                    packet[4*(i+l)+c]=live[l]?out[c*W+l]:NAN;
            }
        }
    };
    double max_error=0;
    for(float t:{0.0f,1.7f,5.0f}) {
        uTime=t; render(false); render(true);
        for(int i=0;i<4*n;++i) {
            double err=std::abs(double(scalar[i])-packet[i]);
            max_error=std::max(max_error,err);
            if(!std::isfinite(scalar[i]) || !std::isfinite(packet[i]) ||
               err>1e-4+1e-4*std::max(std::abs(scalar[i]),std::abs(packet[i]))) {
                std::fprintf(stderr,"equivalence failed: component=%d t=%g scalar=%g packet=%g\n",i,t,scalar[i],packet[i]);
                return 1;
            }
        }
    }
    std::printf("check,%d,%d,%.9g\n",W,n*4*3,max_error);
    uTime=1.7f;
    for(int i=0;i<2;++i) { render(false); render(true); }
    for(int round=0;round<runs;++round) for(int second=0;second<2;++second) {
        const bool p=((round+second)%2!=0);
        auto begin=Clock::now();
        for(int r=0;r<reps;++r) render(p);
        double ms=std::chrono::duration<double,std::milli>(Clock::now()-begin).count()/reps;
        const auto& result=p?packet:scalar;
        double checksum=std::accumulate(result.begin(),result.end(),0.0);
        std::printf("sample,%s,%d,%d,%.9f,%.12g\n",p?"packet":"scalar",threads,round,ms,checksum);
    }
}
