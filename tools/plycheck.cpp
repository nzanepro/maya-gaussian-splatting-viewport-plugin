#include "PlyLoader.h"
#include <cstdio>
#include <string>
int main(int argc, char** argv) {
    SplatData d; std::string err;
    if (!d.load(argv[1], err)) { printf("LOAD FAILED: %s\n", err.c_str()); return 1; }
    printf("splats            : %d\n", d.splatCount);
    printf("SH degree         : %d\n", d.shDegree);
    printf("restFloatsPerSplat: %d\n", d.restFloatsPerSplat);
    printf("positions floats  : %zu (expect %d)\n", d.positions.size(), d.splatCount*4);
    printf("rotations floats  : %zu\n", d.rotations.size());
    printf("scales floats     : %zu\n", d.scales.size());
    printf("sh_dc floats      : %zu\n", d.sh_dc.size());
    printf("sh_rest floats    : %zu (expect %lld)\n", d.sh_rest.size(),
           (long long)d.splatCount * d.restFloatsPerSplat);
    printf("\nfirst splat: pos(%.3f %.3f %.3f) rot(%.3f %.3f %.3f %.3f)\n",
           d.positions[0],d.positions[1],d.positions[2],
           d.rotations[0],d.rotations[1],d.rotations[2],d.rotations[3]);
    printf("            scale(%.4f %.4f %.4f) opacity %.4f  dc(%.3f %.3f %.3f)\n",
           d.scales[0],d.scales[1],d.scales[2],d.scales[3],
           d.sh_dc[0],d.sh_dc[1],d.sh_dc[2]);
    bool sane = d.splatCount>0 && d.positions.size()==(size_t)d.splatCount*4
             && d.scales.size()==(size_t)d.splatCount*4
             && d.sh_rest.size()==(size_t)d.splatCount*d.restFloatsPerSplat
             && d.scales[3]>=0.f && d.scales[3]<=1.f;
    printf("\n%s\n", sane?"RESULT: PASS":"RESULT: FAILED");
    return sane?0:1;
}
