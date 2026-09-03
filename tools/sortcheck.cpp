// Confirms the CPU radix sort added for the GL 4.1 path produces exactly the
// ordering the GPU bitonic sort produces, by running a faithful C++ port of
// sort.comp / depth.comp against it.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <random>
#include <limits>
#include <algorithm>

// ---- copied verbatim from GaussianRenderer.cpp ----
static inline uint32_t depthToRadixKey(float z) {
    uint32_t bits; std::memcpy(&bits, &z, sizeof(bits));
    return (bits & 0x80000000u) ? ~bits : (bits | 0x80000000u);
}
static void radixSortByKey(std::vector<uint32_t>& keys, std::vector<uint32_t>& idx,
                           std::vector<uint32_t>& keysTmp, std::vector<uint32_t>& idxTmp) {
    const size_t n = keys.size();
    if (n < 2) return;
    keysTmp.resize(n); idxTmp.resize(n);
    uint32_t hist[4][256] = {};
    for (size_t i = 0; i < n; ++i) {
        const uint32_t k = keys[i];
        ++hist[0][ k & 0xFFu]; ++hist[1][(k >> 8) & 0xFFu];
        ++hist[2][(k >> 16) & 0xFFu]; ++hist[3][(k >> 24) & 0xFFu];
    }
    for (int pass = 0; pass < 4; ++pass) {
        const int shift = pass * 8;
        if (hist[pass][(keys[0] >> shift) & 0xFFu] == n) continue;
        uint32_t offset[256]; uint32_t sum = 0;
        for (int b = 0; b < 256; ++b) { offset[b] = sum; sum += hist[pass][b]; }
        for (size_t i = 0; i < n; ++i) {
            const uint32_t d = offset[(keys[i] >> shift) & 0xFFu]++;
            keysTmp[d] = keys[i]; idxTmp[d] = idx[i];
        }
        keys.swap(keysTmp); idx.swap(idxTmp);
    }
}

// ---- faithful port of sort.comp, driven like GaussianRenderer::sort() ----
static std::vector<uint32_t> bitonicReference(const std::vector<float>& depthsIn) {
    uint32_t n = (uint32_t)depthsIn.size();
    uint32_t sortN = 1; while (sortN < n) sortN <<= 1;
    std::vector<float> d(sortN, std::numeric_limits<float>::infinity());
    std::vector<uint32_t> idx(sortN, 0);
    for (uint32_t i = 0; i < n; ++i) { d[i] = depthsIn[i]; idx[i] = i; }

    for (uint32_t p = 1; p < sortN; p <<= 1) {
        for (uint32_t q = p; q >= 1; q >>= 1) {
            std::vector<float> d0 = d; std::vector<uint32_t> i0 = idx;
            for (uint32_t i = 0; i < sortN; ++i) {
                uint32_t j = i ^ q;
                if (j > i && i < sortN && j < sortN) {
                    float d1 = d0[i], d2 = d0[j];
                    uint32_t idx1 = i0[i], idx2 = i0[j];
                    bool dist = (i & (p * 2)) == 0;
                    if ((d1 > d2) == dist) {
                        d[i] = d2; d[j] = d1; idx[i] = idx2; idx[j] = idx1;
                    }
                }
            }
        }
    }
    idx.resize(n);
    return idx;
}

int main() {
    std::mt19937 rng(12345);
    int failures = 0;

    for (int trial = 0; trial < 200; ++trial) {
        uint32_t n = 1 + (rng() % 600);
        std::uniform_real_distribution<float> dist(-500.0f, 500.0f);
        std::vector<float> depths(n);
        for (auto& z : depths) z = dist(rng);
        // Exercise ties and exact zero too.
        if (trial % 7 == 0) for (uint32_t i = 0; i < n; i += 3) depths[i] = 0.0f;

        std::vector<uint32_t> keys(n), idx(n), kt, it;
        for (uint32_t i = 0; i < n; ++i) { keys[i] = depthToRadixKey(depths[i]); idx[i] = i; }
        radixSortByKey(keys, idx, kt, it);

        std::vector<uint32_t> ref = bitonicReference(depths);

        // Compare by resulting depth sequence: both must be non-decreasing and
        // identical value-for-value (index order may differ only among ties).
        bool ok = true;
        for (uint32_t i = 0; i < n; ++i)
            if (depths[idx[i]] != depths[ref[i]]) { ok = false; break; }
        for (uint32_t i = 1; i < n; ++i)
            if (depths[idx[i]] < depths[idx[i-1]]) { ok = false; break; }
        std::vector<uint32_t> perm = idx; std::sort(perm.begin(), perm.end());
        for (uint32_t i = 0; i < n; ++i) if (perm[i] != i) { ok = false; break; }

        if (!ok) { printf("FAIL trial %d (n=%u)\n", trial, n); if (++failures > 3) break; }
    }

    // Direction check against depth.comp semantics: Maya looks down -Z, so a
    // more negative view-space z is farther away. Ascending z therefore means
    // farthest first = back-to-front, which is what alpha blending needs.
    std::vector<float> demo = { -1.0f, -100.0f, -50.0f, -10.0f };
    std::vector<uint32_t> k(4), ix(4), kt, it;
    for (int i = 0; i < 4; ++i) { k[i] = depthToRadixKey(demo[i]); ix[i] = i; }
    radixSortByKey(k, ix, kt, it);
    printf("depth order (should be farthest first): ");
    for (int i = 0; i < 4; ++i) printf("%.0f ", demo[ix[i]]);
    printf("\nbitonic reference                     : ");
    std::vector<uint32_t> r = bitonicReference(demo);
    for (int i = 0; i < 4; ++i) printf("%.0f ", demo[r[i]]);
    printf("\n\n%s\n", failures ? "RESULT: FAILED" : "RESULT: PASS (200 randomized trials)");
    return failures ? 1 : 0;
}
