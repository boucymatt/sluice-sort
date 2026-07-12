// Fuzz the wide-key MSD radix path (u64/i64/f64) against std::sort, exercising
// both the sequential core and the parallel path (threads still run on a
// single-core box, so the parallel code path and its races are covered; pair
// with ThreadSanitizer). Sizes straddle the MSD base case and reach deep
// recursion; distributions stress equal keys, narrow ranges, and skew. Uses
// finite doubles so std::sort is a valid oracle.
#include "sluice.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <random>
#include <vector>

static int fails = 0, seq_radix = 0, par_runs = 0;

template <class T>
static void check(const char* tag, sluice_dtype dt, std::vector<T> in, bool parallel) {
    std::vector<T> gold(in);
    std::sort(gold.begin(), gold.end());
    sluice_config cfg{}; sluice_stats st;
    const sluice_config* pc = nullptr;
    if (parallel) { cfg.max_threads = 4; cfg.parallel_min = 1000; pc = &cfg; }
    sluice_status rc = sluice_sort(dt, in.data(), in.size(), 0, nullptr, 1, &st, pc);
    if (rc != SLUICE_OK) { std::printf("FAIL %s rc=%d\n", tag, rc); ++fails; return; }
    if (in != gold)      { std::printf("FAIL %s wrong order (n=%zu, %s)\n", tag, gold.size(),
                                       parallel ? "parallel" : "seq"); ++fails; return; }
    if (std::strcmp(st.algorithm, "radix") == 0) { if (st.threads_used > 1) ++par_runs; else ++seq_radix; }
}

int main() {
    std::mt19937_64 rng(20260712);
    const char* dists[] = {"uniform","dup-heavy","narrow","all-equal","sorted","reverse"};
    for (int t = 0; t < 3000; ++t) {
        // size: mostly small (base-case boundary), sometimes large (deep + parallel)
        size_t n = (t % 7 == 0) ? (2000 + rng() % 300000) : (1 + rng() % 200);
        int di = rng() % 6;
        bool parallel = (n >= 1000) && (t % 2 == 0);
        auto val = [&](void) -> uint64_t {
            switch (di) {
                case 0: return rng();                         // uniform
                case 1: return (rng() % 100 < 96) ? (uint64_t)(rng() % 4) : rng(); // dup-heavy
                case 2: return rng() % 1000;                  // narrow range
                case 3: return 0x0123456789ABCDEFull;         // all-equal
                default: return rng();
            }
        };
        int dom = rng() % 3;   // 0=u64, 1=i64, 2=f64
        if (dom == 0) {
            std::vector<uint64_t> a(n); for (auto& x : a) x = val();
            if (di == 4) std::sort(a.begin(), a.end());
            if (di == 5) { std::sort(a.begin(), a.end()); std::reverse(a.begin(), a.end()); }
            check("u64", SLUICE_U64, a, parallel);
        } else if (dom == 1) {
            std::vector<int64_t> a(n); for (auto& x : a) x = (int64_t)val();
            if (di == 4) std::sort(a.begin(), a.end());
            if (di == 5) { std::sort(a.begin(), a.end()); std::reverse(a.begin(), a.end()); }
            check("i64", SLUICE_I64, a, parallel);
        } else {
            std::vector<double> a(n);
            for (auto& x : a) {
                uint64_t b = val(); double d; std::memcpy(&d, &b, 8);
                if (!std::isfinite(d)) d = static_cast<double>(static_cast<int64_t>(b));
                x = d;
            }
            if (di == 4) std::sort(a.begin(), a.end());
            if (di == 5) { std::sort(a.begin(), a.end()); std::reverse(a.begin(), a.end()); }
            check("f64", SLUICE_F64, a, parallel);
        }
        (void)dists;
    }
    std::printf("%s  (wide-key radix: %d sequential, %d parallel runs; %d failures)\n",
                fails ? "FAILED" : "OK", seq_radix, par_runs, fails);
    return fails ? 1 : 0;
}
