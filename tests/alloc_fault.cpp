// Allocation-failure verification.
//
// Overrides global operator new so that, while "armed", every heap allocation
// larger than a threshold throws std::bad_alloc. This exercises the engine's
// documented contract: on allocation failure it must degrade to an in-place
// std::sort (by the order-preserving key), never crash or leak, always leave
// the array fully sorted, and — for the status-returning API — still report
// SLUICE_OK. Run both natively and under ASan to catch any leak on the throw
// path. Uses only finite values so std::sort is a valid oracle.
#include "sluice.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <vector>

static bool   g_arm    = false;
static size_t g_thresh = 0;               // while armed, allocations > this fail
static long   g_fails  = 0;               // how many injected failures fired

void* operator new(std::size_t s) {
    if (g_arm && s > g_thresh) { ++g_fails; throw std::bad_alloc(); }
    void* p = std::malloc(s ? s : 1); if (!p) throw std::bad_alloc(); return p;
}
void* operator new[](std::size_t s) { return ::operator new(s); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

static int g_bad = 0, g_stdsort = 0;
template <class T>
static void check(const char* tag, sluice_dtype dt, std::vector<T> in, size_t thresh) {
    std::vector<T> gold(in); std::sort(gold.begin(), gold.end());
    // 1) status API: must return SLUICE_OK and sort correctly under failure.
    {
        std::vector<T> a(in); sluice_stats st; g_thresh = thresh; g_arm = true;
        sluice_status rc = sluice_sort(dt, a.data(), a.size(), 0, nullptr, 1, &st, nullptr);
        g_arm = false;
        if (rc != SLUICE_OK) { std::printf("  FAIL %s: rc=%d\n", tag, rc); ++g_bad; return; }
        if (a != gold)       { std::printf("  FAIL %s: wrong order (API)\n", tag); ++g_bad; return; }
        if (std::strcmp(st.algorithm, "std::sort") == 0) ++g_stdsort;
    }
    // 2) typed in-place API (no key buffer): exercises the core-level fallback.
    {
        std::vector<T> a(in); g_thresh = thresh; g_arm = true;
        if      (dt == SLUICE_U32) sluice_sort_u32(reinterpret_cast<uint32_t*>(a.data()), a.size());
        else if (dt == SLUICE_I32) sluice_sort_i32(reinterpret_cast<int32_t*> (a.data()), a.size());
        else if (dt == SLUICE_U64) sluice_sort_u64(reinterpret_cast<uint64_t*>(a.data()), a.size());
        else if (dt == SLUICE_I64) sluice_sort_i64(reinterpret_cast<int64_t*> (a.data()), a.size());
        else if (dt == SLUICE_F64) sluice_sort_f64(reinterpret_cast<double*>  (a.data()), a.size());
        g_arm = false;
        if (a != gold) { std::printf("  FAIL %s: wrong order (typed)\n", tag); ++g_bad; }
    }
}

int main() {
    std::mt19937_64 rng(99);
    auto u32 = [&](size_t n, uint32_t mod){ std::vector<uint32_t> v(n);
        for (auto& x : v) x = mod ? (uint32_t)(rng() % mod) : (uint32_t)rng(); return v; };
    // thresh=0 -> every heap allocation fails (keys buffer fails first);
    // thresh=1<<15 -> small allocations pass, large buffers fail (fall-through).
    for (size_t thresh : {size_t(0), size_t(1u << 15)}) {
        // large uniform (radix/keys path) across all integer types + double
        { auto v = u32(200000, 0); check("u32 uniform", SLUICE_U32, v, thresh); }
        { auto v = u32(200000, 0); std::vector<int32_t> s(v.begin(), v.end());
          check("i32 uniform", SLUICE_I32, s, thresh); }
        { std::vector<uint64_t> v(200000); for (auto& x : v) x = rng();
          check("u64 uniform", SLUICE_U64, v, thresh); }
        { std::vector<int64_t> v(200000); for (auto& x : v) x = (int64_t)rng();
          check("i64 uniform", SLUICE_I64, v, thresh); }
        { std::vector<double> v(200000); std::uniform_real_distribution<double> d(-1e9,1e9);
          for (auto& x : v) x = d(rng); check("f64 uniform", SLUICE_F64, v, thresh); }
        // duplicate-heavy (heavy-hitter path) and bounded (counting path)
        { std::vector<uint32_t> v(200000); uint32_t h = (uint32_t)rng();
          for (auto& x : v) x = (rng()%100<97)? h : (uint32_t)rng();
          check("u32 heavy-hitter", SLUICE_U32, v, thresh); }
        { auto v = u32(200000, 1000); check("u32 bounded/counting", SLUICE_U32, v, thresh); }
        // small inputs (insertion path — no heap; must also stay correct)
        { auto v = u32(64, 0); check("u32 small", SLUICE_U32, v, thresh); }
    }
    std::printf("%s  (std::sort fallback taken %d times, %ld injected failures)\n",
                g_bad ? "FAILED" : "OK", g_stdsort, g_fails);
    return g_bad ? 1 : 0;
}
