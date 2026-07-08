#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <string>
#include <vector>

#if defined(GEM5)
#include <gem5/m5ops.h>
#endif

namespace {

constexpr std::uint64_t kSpmBase = 1ull << 40;

// Walker-completion counter for the shared-table updater thread.
std::atomic<int> g_walkers_done{0};

struct Options {
    int threads = 4;
    int src_lines = 256;  // 8 double features per source line.
    int edges = 65536;
    int steps = 65536;    // walk mode: steps per walk group (4 walks/core)
    int walk = 0;         // 0 = independent-edge SpMM, 1 = random-walk chase
    int cold_lines = 0;   // walk mode: >0 adds a cold table; ~1/8 of steps
                          // chase into it (power-law hub/spoke pattern)
    int shared = 0;       // walk mode: table shared by all walkers; last
                          // thread becomes a feature updater (dynamic graph)
    int update_gap = 64;  // updater: idle iterations between line rewrites
    int snapshot = 0;     // shared mode, cache variant: walkers memcpy the
                          // hub to private memory per epoch (best software
                          // baseline; dodges invalidations without SPM)
    int repeat = 1;
    int warmup = 0;
    int prefetch = 0;
    int pin = 1;
    int base_set = 64;
    int spm_ways_used = 4; // power of two, <= configured l1d_num_spm_ways
    int feat_lines = 1;    // GE-SpMM CWM analog: 64B feature lines per node.
                           // 1 = original narrow kernel; 8 = wide (index/weight
                           // and SPM address computed once, reused across 8
                           // feature sub-lines -> amortized index+address).
};

struct ThreadArg {
    Options opt;
    int tid = 0;
    pthread_barrier_t *barrier = nullptr;
    double *features = nullptr;
    std::uint32_t *idx = nullptr;
    double *weights = nullptr;
    double *cold = nullptr;
    double *snap = nullptr;
    double result = 0.0;
};

static int parse_int(const char *s, const char *name, int minv)
{
    char *end = nullptr;
    errno = 0;
    long v = std::strtol(s, &end, 10);
    if (errno || end == s || *end || v < minv || v > INT32_MAX) {
        std::fprintf(stderr, "bad %s: %s\n", name, s);
        std::exit(2);
    }
    return static_cast<int>(v);
}

static Options parse_args(int argc, char **argv)
{
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        auto need = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--threads") opt.threads = parse_int(need("threads"), "threads", 1);
        else if (a == "--src-lines") opt.src_lines = parse_int(need("src-lines"), "src-lines", 1);
        else if (a == "--edges") opt.edges = parse_int(need("edges"), "edges", 1);
        else if (a == "--steps") opt.steps = parse_int(need("steps"), "steps", 1);
        else if (a == "--walk") opt.walk = parse_int(need("walk"), "walk", 0);
        else if (a == "--cold-lines") opt.cold_lines = parse_int(need("cold-lines"), "cold-lines", 0);
        else if (a == "--shared") opt.shared = parse_int(need("shared"), "shared", 0);
        else if (a == "--update-gap") opt.update_gap = parse_int(need("update-gap"), "update-gap", 0);
        else if (a == "--snapshot") opt.snapshot = parse_int(need("snapshot"), "snapshot", 0);
        else if (a == "--repeat") opt.repeat = parse_int(need("repeat"), "repeat", 1);
        else if (a == "--warmup") opt.warmup = parse_int(need("warmup"), "warmup", 0);
        else if (a == "--prefetch") opt.prefetch = parse_int(need("prefetch"), "prefetch", 0);
        else if (a == "--pin") opt.pin = parse_int(need("pin"), "pin", 0);
        else if (a == "--base-set") opt.base_set = parse_int(need("base-set"), "base-set", 0);
        else if (a == "--spm-ways-used") opt.spm_ways_used = parse_int(need("spm-ways-used"), "spm-ways-used", 1);
        else if (a == "--feat-lines") opt.feat_lines = parse_int(need("feat-lines"), "feat-lines", 1);
        else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            std::exit(2);
        }
    }
    if (opt.spm_ways_used != 1 && opt.spm_ways_used != 2 && opt.spm_ways_used != 4) {
        std::fprintf(stderr, "spm-ways-used must be 1, 2, or 4\n");
        std::exit(2);
    }
    if (opt.feat_lines != 1 && opt.feat_lines != 8) {
        std::fprintf(stderr, "feat-lines must be 1 or 8\n");
        std::exit(2);
    }
    if (opt.feat_lines == 8 && opt.walk) {
        std::fprintf(stderr, "feat-lines 8 (wide CWM) is SpMM mode only (no walk)\n");
        std::exit(2);
    }
    if (opt.edges % 4 != 0) {
        std::fprintf(stderr, "edges must be a multiple of 4 (unrolled kernel)\n");
        std::exit(2);
    }
    if (opt.walk && (opt.src_lines & (opt.src_lines - 1)) != 0) {
        std::fprintf(stderr, "walk mode requires power-of-two src-lines\n");
        std::exit(2);
    }
    if (opt.walk && opt.spm_ways_used != 4) {
        std::fprintf(stderr, "walk mode assumes spm-ways-used=4\n");
        std::exit(2);
    }
    if (opt.cold_lines && (opt.cold_lines & (opt.cold_lines - 1)) != 0) {
        std::fprintf(stderr, "cold-lines must be a power of two\n");
        std::exit(2);
    }
    if (opt.shared && (!opt.walk || opt.threads < 3)) {
        std::fprintf(stderr, "shared mode requires walk mode and >= 3 threads (last thread is the updater)\n");
        std::exit(2);
    }
    // The table is striped across the ways of this core's private SPM:
    // line l -> way (l % ways), set base_set + l / ways.
    const int sets_needed = ((opt.src_lines + opt.spm_ways_used - 1) / opt.spm_ways_used)
                            * opt.feat_lines;
    if (opt.base_set + sets_needed > 1024) {
        std::fprintf(stderr, "src-lines too large: base_set + feat_lines*ceil(src_lines/ways) must be <= 1024\n");
        std::exit(2);
    }
    return opt;
}

static void *aligned_bytes(std::size_t bytes)
{
    void *p = nullptr;
    bytes = (bytes + 63) & ~std::size_t(63);
    if (posix_memalign(&p, 64, bytes) != 0 || !p) {
        std::perror("posix_memalign");
        std::exit(1);
    }
    return p;
}

static inline std::uint64_t lcg(std::uint64_t &x)
{
    x = x * 2862933555777941757ull + 3037000493ull;
    return x;
}

static void init_features(double *p, std::size_t n)
{
    // Full-entropy mantissas: the walk kernels derive successor indices from
    // stored feature bits, so every mantissa bit must be well mixed.
    for (std::size_t i = 0; i < n; ++i) {
        std::uint64_t x = i * 11400714819323198485ull + 0x9e3779b97f4a7c15ull;
        x ^= x >> 29;
        x *= 0xbf58476d1ce4e5b9ull;
        x ^= x >> 32;
        const std::uint64_t bits = (0x3FFull << 52) | (x & ((1ull << 52) - 1));
        double d;
        std::memcpy(&d, &bits, sizeof(d));
        p[i] = d - 1.5; // [-0.5, 0.5)
    }
}

static void init_edges(std::uint32_t *idx, double *w, int n, int src_lines, int tid)
{
    std::uint64_t s = 0x123456789abcdef0ull ^ std::uint64_t(tid + 1) * 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < n; ++i) {
        idx[i] = std::uint32_t(lcg(s) % std::uint64_t(src_lines));
        w[i] = double(int((lcg(s) >> 50) & 0x7f) - 64) / 128.0;
    }
}

static void pin_thread(int tid)
{
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(tid, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)tid;
#endif
}

static inline std::uint64_t spm_addr(unsigned way, unsigned set, unsigned off = 0)
{
    return kSpmBase | (std::uint64_t(way) << 16) | (std::uint64_t(set) << 6) | off;
}

static inline void spmcp64(std::uint64_t dst_spm, const void *src)
{
    asm volatile("SPMCP_64_IMM %0, [%1, #0]\n"
                 :
                 : "r"(dst_spm), "r"(src)
                 : "memory");
}

static inline void spm_release(std::uint64_t dst_spm)
{
    asm volatile("SPMREL_8_IMM xzr, [%0, #0]\n"
                 :
                 : "r"(dst_spm)
                 : "memory");
}

/* Weighted feature aggregation over the edge list:
 *   acc[0..7] += w[e] * features[idx[e]][0..7]
 * returning the horizontal sum (same value the old reduce-then-scale loop
 * produced, so checksums are comparable between variants).
 *
 * The loop is 4-way unrolled with four independent vector accumulators so the
 * random 64B feature loads limit throughput, not a serial FP dependency
 * chain -- otherwise the OoO core hides all memory-system differences. */
static double spmm_loop_cache(const std::uint32_t *idx, const double *w,
                              const double *features, long edges, int prefetch)
{
    double out;
    const std::uint32_t *idx_p = idx;
    const double *w_p = w;
    if (prefetch) {
        /* Best-effort baseline: hash-ahead style indirect prefetch of the
         * feature line 32 edges ahead (idx is padded past the end). */
        asm volatile(
            "mov x9, #8\n"
            "whilelt p0.d, xzr, x9\n"
            "dup z24.d, #0\n"
            "dup z25.d, #0\n"
            "dup z26.d, #0\n"
            "dup z27.d, #0\n"
            "mov x8, xzr\n"
            "1:\n"
            "ldr w10, [%[idx]]\n"
            "add x11, %[tab], x10, lsl #6\n"
            "ld1d z0.d, p0/z, [x11]\n"
            "ld1rd z1.d, p0/z, [%[w]]\n"
            "fmla z24.d, p0/m, z0.d, z1.d\n"
            "ldr w10, [%[idx], #4]\n"
            "add x11, %[tab], x10, lsl #6\n"
            "ld1d z2.d, p0/z, [x11]\n"
            "ld1rd z3.d, p0/z, [%[w], #8]\n"
            "fmla z25.d, p0/m, z2.d, z3.d\n"
            "ldr w10, [%[idx], #8]\n"
            "add x11, %[tab], x10, lsl #6\n"
            "ld1d z4.d, p0/z, [x11]\n"
            "ld1rd z5.d, p0/z, [%[w], #16]\n"
            "fmla z26.d, p0/m, z4.d, z5.d\n"
            "ldr w10, [%[idx], #12]\n"
            "add x11, %[tab], x10, lsl #6\n"
            "ld1d z6.d, p0/z, [x11]\n"
            "ld1rd z7.d, p0/z, [%[w], #24]\n"
            "fmla z27.d, p0/m, z6.d, z7.d\n"
            "ldr w10, [%[idx], #128]\n"
            "add x11, %[tab], x10, lsl #6\n"
            "prfm pldl1keep, [x11]\n"
            "ldr w10, [%[idx], #132]\n"
            "add x11, %[tab], x10, lsl #6\n"
            "prfm pldl1keep, [x11]\n"
            "ldr w10, [%[idx], #136]\n"
            "add x11, %[tab], x10, lsl #6\n"
            "prfm pldl1keep, [x11]\n"
            "ldr w10, [%[idx], #140]\n"
            "add x11, %[tab], x10, lsl #6\n"
            "prfm pldl1keep, [x11]\n"
            "add %[idx], %[idx], #16\n"
            "add %[w], %[w], #32\n"
            "add x8, x8, #4\n"
            "cmp x8, %[n]\n"
            "b.lt 1b\n"
            "fadd z24.d, z24.d, z25.d\n"
            "fadd z26.d, z26.d, z27.d\n"
            "fadd z24.d, z24.d, z26.d\n"
            "faddv d24, p0, z24.d\n"
            "fmov %d[out], d24\n"
            : [out] "=w"(out), [idx] "+r"(idx_p), [w] "+r"(w_p)
            : [tab] "r"(features), [n] "r"(edges)
            : "x8", "x9", "x10", "x11", "p0",
              "z0", "z1", "z2", "z3", "z4", "z5", "z6", "z7",
              "z24", "z25", "z26", "z27", "cc", "memory");
    } else {
        asm volatile(
            "mov x9, #8\n"
            "whilelt p0.d, xzr, x9\n"
            "dup z24.d, #0\n"
            "dup z25.d, #0\n"
            "dup z26.d, #0\n"
            "dup z27.d, #0\n"
            "mov x8, xzr\n"
            "1:\n"
            "ldr w10, [%[idx]]\n"
            "add x11, %[tab], x10, lsl #6\n"
            "ld1d z0.d, p0/z, [x11]\n"
            "ld1rd z1.d, p0/z, [%[w]]\n"
            "fmla z24.d, p0/m, z0.d, z1.d\n"
            "ldr w10, [%[idx], #4]\n"
            "add x11, %[tab], x10, lsl #6\n"
            "ld1d z2.d, p0/z, [x11]\n"
            "ld1rd z3.d, p0/z, [%[w], #8]\n"
            "fmla z25.d, p0/m, z2.d, z3.d\n"
            "ldr w10, [%[idx], #8]\n"
            "add x11, %[tab], x10, lsl #6\n"
            "ld1d z4.d, p0/z, [x11]\n"
            "ld1rd z5.d, p0/z, [%[w], #16]\n"
            "fmla z26.d, p0/m, z4.d, z5.d\n"
            "ldr w10, [%[idx], #12]\n"
            "add x11, %[tab], x10, lsl #6\n"
            "ld1d z6.d, p0/z, [x11]\n"
            "ld1rd z7.d, p0/z, [%[w], #24]\n"
            "fmla z27.d, p0/m, z6.d, z7.d\n"
            "add %[idx], %[idx], #16\n"
            "add %[w], %[w], #32\n"
            "add x8, x8, #4\n"
            "cmp x8, %[n]\n"
            "b.lt 1b\n"
            "fadd z24.d, z24.d, z25.d\n"
            "fadd z26.d, z26.d, z27.d\n"
            "fadd z24.d, z24.d, z26.d\n"
            "faddv d24, p0, z24.d\n"
            "fmov %d[out], d24\n"
            : [out] "=w"(out), [idx] "+r"(idx_p), [w] "+r"(w_p)
            : [tab] "r"(features), [n] "r"(edges)
            : "x8", "x9", "x10", "x11", "p0",
              "z0", "z1", "z2", "z3", "z4", "z5", "z6", "z7",
              "z24", "z25", "z26", "z27", "cc", "memory");
    }
    return out;
}

static double spmm_loop_spm(const std::uint32_t *idx, const double *w,
                            std::uint64_t spm_base, long edges,
                            std::uint64_t way_mask, std::uint64_t way_shift)
{
    double out;
    const std::uint32_t *idx_p = idx;
    const double *w_p = w;
    /* spm_base already folds in kSpmBase | (base_set << 6); a table line l
     * lives at spm_base + ((l & way_mask) << 16) + ((l >> way_shift) << 6). */
    asm volatile(
        "mov x9, #8\n"
        "whilelt p0.d, xzr, x9\n"
        "dup z24.d, #0\n"
        "dup z25.d, #0\n"
        "dup z26.d, #0\n"
        "dup z27.d, #0\n"
        "mov x8, xzr\n"
        "1:\n"
        "ldr w10, [%[idx]]\n"
        "and x12, x10, %[mask]\n"
        "add x11, %[base], x12, lsl #16\n"
        "lsr x12, x10, %[shift]\n"
        "add x11, x11, x12, lsl #6\n"
        "spm.ld1d z0.d, p0/z, [x11]\n"
        "ld1rd z1.d, p0/z, [%[w]]\n"
        "fmla z24.d, p0/m, z0.d, z1.d\n"
        "ldr w10, [%[idx], #4]\n"
        "and x12, x10, %[mask]\n"
        "add x11, %[base], x12, lsl #16\n"
        "lsr x12, x10, %[shift]\n"
        "add x11, x11, x12, lsl #6\n"
        "spm.ld1d z2.d, p0/z, [x11]\n"
        "ld1rd z3.d, p0/z, [%[w], #8]\n"
        "fmla z25.d, p0/m, z2.d, z3.d\n"
        "ldr w10, [%[idx], #8]\n"
        "and x12, x10, %[mask]\n"
        "add x11, %[base], x12, lsl #16\n"
        "lsr x12, x10, %[shift]\n"
        "add x11, x11, x12, lsl #6\n"
        "spm.ld1d z4.d, p0/z, [x11]\n"
        "ld1rd z5.d, p0/z, [%[w], #16]\n"
        "fmla z26.d, p0/m, z4.d, z5.d\n"
        "ldr w10, [%[idx], #12]\n"
        "and x12, x10, %[mask]\n"
        "add x11, %[base], x12, lsl #16\n"
        "lsr x12, x10, %[shift]\n"
        "add x11, x11, x12, lsl #6\n"
        "spm.ld1d z6.d, p0/z, [x11]\n"
        "ld1rd z7.d, p0/z, [%[w], #24]\n"
        "fmla z27.d, p0/m, z6.d, z7.d\n"
        "add %[idx], %[idx], #16\n"
        "add %[w], %[w], #32\n"
        "add x8, x8, #4\n"
        "cmp x8, %[n]\n"
        "b.lt 1b\n"
        "fadd z24.d, z24.d, z25.d\n"
        "fadd z26.d, z26.d, z27.d\n"
        "fadd z24.d, z24.d, z26.d\n"
        "faddv d24, p0, z24.d\n"
        "fmov %d[out], d24\n"
        : [out] "=w"(out), [idx] "+r"(idx_p), [w] "+r"(w_p)
        : [base] "r"(spm_base), [n] "r"(edges),
          [mask] "r"(way_mask), [shift] "r"(way_shift)
        : "x8", "x9", "x10", "x11", "x12", "p0",
          "z0", "z1", "z2", "z3", "z4", "z5", "z6", "z7",
          "z24", "z25", "z26", "z27", "cc", "memory");
    return out;
}

/* GE-SpMM "coarse-grained warp merging" analog: F=8 feature lines per node.
 * Each edge loads idx/weight and computes the node base address ONCE, then
 * issues 8 independent 64B loads (one per feature sub-line) + 8 FMAs into 8
 * accumulators.  The index load and the (SPM or cache) address arithmetic are
 * amortized across the 8 feature lines, and the 8 independent loads supply the
 * memory-level parallelism (replacing the narrow kernel's 4-edge unroll).
 * acc[f] += w[e] * features[idx[e]][f][0..7]; checksum is the sum of the 8
 * accumulators' horizontal sums, identical math in cache and SPM variants. */
static double spmm_wide8_loop_cache(const std::uint32_t *idx, const double *w,
                                    const double *features, long edges)
{
    double out;
    const std::uint32_t *idx_p = idx;
    const double *w_p = w;
    asm volatile(
        "mov x9, #8\n"
        "whilelt p0.d, xzr, x9\n"
        "dup z16.d, #0\n" "dup z17.d, #0\n" "dup z18.d, #0\n" "dup z19.d, #0\n"
        "dup z20.d, #0\n" "dup z21.d, #0\n" "dup z22.d, #0\n" "dup z23.d, #0\n"
        "mov x8, xzr\n"
        "1:\n"
        "ldr w10, [%[idx]]\n"
        "ld1rd z30.d, p0/z, [%[w]]\n"
        "add x11, %[tab], x10, lsl #9\n"     // base = tab + n*(8 lines*64B)
        // explicit +64B line addresses (NOT mul vl -- the SVE VL here is not
        // 64B, so mul-vl offsets would miss the feature lines)
        "ld1d z0.d, p0/z, [x11]\n"
        "add x13, x11, #64\n"
        "ld1d z1.d, p0/z, [x13]\n"
        "add x14, x11, #128\n"
        "ld1d z2.d, p0/z, [x14]\n"
        "add x15, x11, #192\n"
        "ld1d z3.d, p0/z, [x15]\n"
        "add x16, x11, #256\n"
        "ld1d z4.d, p0/z, [x16]\n"
        "add x17, x11, #320\n"
        "ld1d z5.d, p0/z, [x17]\n"
        "add x18, x11, #384\n"
        "ld1d z6.d, p0/z, [x18]\n"
        "add x19, x11, #448\n"
        "ld1d z7.d, p0/z, [x19]\n"
        "fmla z16.d, p0/m, z0.d, z30.d\n"
        "fmla z17.d, p0/m, z1.d, z30.d\n"
        "fmla z18.d, p0/m, z2.d, z30.d\n"
        "fmla z19.d, p0/m, z3.d, z30.d\n"
        "fmla z20.d, p0/m, z4.d, z30.d\n"
        "fmla z21.d, p0/m, z5.d, z30.d\n"
        "fmla z22.d, p0/m, z6.d, z30.d\n"
        "fmla z23.d, p0/m, z7.d, z30.d\n"
        "add %[idx], %[idx], #4\n"
        "add %[w], %[w], #8\n"
        "add x8, x8, #1\n"
        "cmp x8, %[n]\n"
        "b.lt 1b\n"
        "fadd z16.d, z16.d, z17.d\n"
        "fadd z18.d, z18.d, z19.d\n"
        "fadd z20.d, z20.d, z21.d\n"
        "fadd z22.d, z22.d, z23.d\n"
        "fadd z16.d, z16.d, z18.d\n"
        "fadd z20.d, z20.d, z22.d\n"
        "fadd z16.d, z16.d, z20.d\n"
        "faddv d16, p0, z16.d\n"
        "fmov %d[out], d16\n"
        : [out] "=w"(out), [idx] "+r"(idx_p), [w] "+r"(w_p)
        : [tab] "r"(features), [n] "r"(edges)
        : "x8", "x9", "x10", "x11", "x13", "x14", "x15", "x16",
          "x17", "x18", "x19", "p0",
          "z0", "z1", "z2", "z3", "z4", "z5", "z6", "z7",
          "z16", "z17", "z18", "z19", "z20", "z21", "z22", "z23", "z30",
          "cc", "memory");
    return out;
}

/* SPM wide-8: node n's 8 lines share way (n & mask) at consecutive sets, so the
 * node base is computed once and each line is base + f*64.  Custom spm.ld1d
 * takes a base register (no mul-vl offset), so the 8 line addresses are formed
 * with cheap immediate adds -- the index load and way/set arithmetic are still
 * amortized across the 8 lines. */
static double spmm_wide8_loop_spm(const std::uint32_t *idx, const double *w,
                                  std::uint64_t spm_base, long edges,
                                  std::uint64_t way_mask, std::uint64_t way_shift)
{
    double out;
    const std::uint32_t *idx_p = idx;
    const double *w_p = w;
    asm volatile(
        "mov x9, #8\n"
        "whilelt p0.d, xzr, x9\n"
        "dup z16.d, #0\n" "dup z17.d, #0\n" "dup z18.d, #0\n" "dup z19.d, #0\n"
        "dup z20.d, #0\n" "dup z21.d, #0\n" "dup z22.d, #0\n" "dup z23.d, #0\n"
        "mov x8, xzr\n"
        "1:\n"
        "ldr w10, [%[idx]]\n"
        "ld1rd z30.d, p0/z, [%[w]]\n"
        "and x12, x10, %[mask]\n"
        "lsl x12, x12, #16\n"
        "add x11, %[base], x12\n"
        "lsr x12, x10, %[shift]\n"
        "add x11, x11, x12, lsl #9\n"        // + (n>>shift) << (6+3)  (8 lines)
        "spm.ld1d z0.d, p0/z, [x11]\n"
        "add x13, x11, #64\n"
        "spm.ld1d z1.d, p0/z, [x13]\n"
        "add x14, x11, #128\n"
        "spm.ld1d z2.d, p0/z, [x14]\n"
        "add x15, x11, #192\n"
        "spm.ld1d z3.d, p0/z, [x15]\n"
        "add x16, x11, #256\n"
        "spm.ld1d z4.d, p0/z, [x16]\n"
        "add x17, x11, #320\n"
        "spm.ld1d z5.d, p0/z, [x17]\n"
        "add x18, x11, #384\n"
        "spm.ld1d z6.d, p0/z, [x18]\n"
        "add x19, x11, #448\n"
        "spm.ld1d z7.d, p0/z, [x19]\n"
        "fmla z16.d, p0/m, z0.d, z30.d\n"
        "fmla z17.d, p0/m, z1.d, z30.d\n"
        "fmla z18.d, p0/m, z2.d, z30.d\n"
        "fmla z19.d, p0/m, z3.d, z30.d\n"
        "fmla z20.d, p0/m, z4.d, z30.d\n"
        "fmla z21.d, p0/m, z5.d, z30.d\n"
        "fmla z22.d, p0/m, z6.d, z30.d\n"
        "fmla z23.d, p0/m, z7.d, z30.d\n"
        "add %[idx], %[idx], #4\n"
        "add %[w], %[w], #8\n"
        "add x8, x8, #1\n"
        "cmp x8, %[n]\n"
        "b.lt 1b\n"
        "fadd z16.d, z16.d, z17.d\n"
        "fadd z18.d, z18.d, z19.d\n"
        "fadd z20.d, z20.d, z21.d\n"
        "fadd z22.d, z22.d, z23.d\n"
        "fadd z16.d, z16.d, z18.d\n"
        "fadd z20.d, z20.d, z22.d\n"
        "fadd z16.d, z16.d, z20.d\n"
        "faddv d16, p0, z16.d\n"
        "fmov %d[out], d16\n"
        : [out] "=w"(out), [idx] "+r"(idx_p), [w] "+r"(w_p)
        : [base] "r"(spm_base), [n] "r"(edges),
          [mask] "r"(way_mask), [shift] "r"(way_shift)
        : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16",
          "x17", "x18", "x19", "p0",
          "z0", "z1", "z2", "z3", "z4", "z5", "z6", "z7",
          "z16", "z17", "z18", "z19", "z20", "z21", "z22", "z23", "z30",
          "cc", "memory");
    return out;
}

/* GraphSAGE/node2vec-style random-walk feature aggregation: four concurrent
 * walks per core; each step loads the current node's 64B feature line,
 * aggregates it, and derives the next node from the loaded data (mantissa
 * bits XOR a shifted step counter, so the walk never collapses into a short
 * cycle).  The next address depends on the loaded value, so memory latency
 * is exposed serially per walk -- the batched-walk width (4) is the only
 * memory-level parallelism, exactly as in real sampling kernels.  Indirect
 * prefetch is impossible for a data-dependent chase, so the baseline is not
 * handicapped by omitting it. */
static double walk_loop_cache(const double *features, long steps,
                              std::uint64_t mask_hi, const double *wp,
                              const double *s0, const double *s1,
                              const double *s2, const double *s3)
{
    double out;
    asm volatile(
        "mov x9, #8\n"
        "whilelt p0.d, xzr, x9\n"
        "ld1rd z30.d, p0/z, [%[wp]]\n"
        "dup z24.d, #0\n"
        "dup z25.d, #0\n"
        "dup z26.d, #0\n"
        "dup z27.d, #0\n"
        "mov x20, %[s0]\n"
        "mov x21, %[s1]\n"
        "mov x22, %[s2]\n"
        "mov x23, %[s3]\n"
        "mov x8, xzr\n"
        "1:\n"
        "ld1d z0.d, p0/z, [x20]\n"
        "fmov x10, d0\n"
        "eor x10, x10, x8, lsl #38\n"
        "and x11, x10, %[mskA]\n"
        "add x20, %[tab], x11, lsr #34\n"
        "fmla z24.d, p0/m, z0.d, z30.d\n"
        "ld1d z1.d, p0/z, [x21]\n"
        "fmov x10, d1\n"
        "eor x10, x10, x8, lsl #38\n"
        "and x11, x10, %[mskA]\n"
        "add x21, %[tab], x11, lsr #34\n"
        "fmla z25.d, p0/m, z1.d, z30.d\n"
        "ld1d z2.d, p0/z, [x22]\n"
        "fmov x10, d2\n"
        "eor x10, x10, x8, lsl #38\n"
        "and x11, x10, %[mskA]\n"
        "add x22, %[tab], x11, lsr #34\n"
        "fmla z26.d, p0/m, z2.d, z30.d\n"
        "ld1d z3.d, p0/z, [x23]\n"
        "fmov x10, d3\n"
        "eor x10, x10, x8, lsl #38\n"
        "and x11, x10, %[mskA]\n"
        "add x23, %[tab], x11, lsr #34\n"
        "fmla z27.d, p0/m, z3.d, z30.d\n"
        "add x8, x8, #1\n"
        "cmp x8, %[n]\n"
        "b.lt 1b\n"
        "fadd z24.d, z24.d, z25.d\n"
        "fadd z26.d, z26.d, z27.d\n"
        "fadd z24.d, z24.d, z26.d\n"
        "faddv d24, p0, z24.d\n"
        "fmov %d[out], d24\n"
        : [out] "=w"(out)
        : [tab] "r"(features), [n] "r"(steps), [mskA] "r"(mask_hi),
          [wp] "r"(wp), [s0] "r"(s0), [s1] "r"(s1), [s2] "r"(s2),
          [s3] "r"(s3)
        : "x8", "x9", "x10", "x11", "x20", "x21", "x22", "x23", "p0",
          "z0", "z1", "z2", "z3", "z24", "z25", "z26", "z27", "z30",
          "cc", "memory");
    return out;
}

/* SPM walk: identical chase, but the line index maps onto the private SPM
 * (way = idx & 3, set = base_set + idx >> 2), matching the staging layout. */
static double walk_loop_spm(std::uint64_t spm_base, long steps,
                            std::uint64_t mask_hi, const double *wp,
                            std::uint64_t s0, std::uint64_t s1,
                            std::uint64_t s2, std::uint64_t s3)
{
    double out;
    asm volatile(
        "mov x9, #8\n"
        "whilelt p0.d, xzr, x9\n"
        "ld1rd z30.d, p0/z, [%[wp]]\n"
        "dup z24.d, #0\n"
        "dup z25.d, #0\n"
        "dup z26.d, #0\n"
        "dup z27.d, #0\n"
        "mov x20, %[s0]\n"
        "mov x21, %[s1]\n"
        "mov x22, %[s2]\n"
        "mov x23, %[s3]\n"
        "mov x8, xzr\n"
        "1:\n"
        "spm.ld1d z0.d, p0/z, [x20]\n"
        "fmov x10, d0\n"
        "eor x10, x10, x8, lsl #38\n"
        "and x11, x10, %[mskA]\n"
        "ubfx x12, x11, #40, #2\n"
        "add x13, %[base], x12, lsl #16\n"
        "lsr x12, x11, #42\n"
        "add x20, x13, x12, lsl #6\n"
        "fmla z24.d, p0/m, z0.d, z30.d\n"
        "spm.ld1d z1.d, p0/z, [x21]\n"
        "fmov x10, d1\n"
        "eor x10, x10, x8, lsl #38\n"
        "and x11, x10, %[mskA]\n"
        "ubfx x12, x11, #40, #2\n"
        "add x13, %[base], x12, lsl #16\n"
        "lsr x12, x11, #42\n"
        "add x21, x13, x12, lsl #6\n"
        "fmla z25.d, p0/m, z1.d, z30.d\n"
        "spm.ld1d z2.d, p0/z, [x22]\n"
        "fmov x10, d2\n"
        "eor x10, x10, x8, lsl #38\n"
        "and x11, x10, %[mskA]\n"
        "ubfx x12, x11, #40, #2\n"
        "add x13, %[base], x12, lsl #16\n"
        "lsr x12, x11, #42\n"
        "add x22, x13, x12, lsl #6\n"
        "fmla z26.d, p0/m, z2.d, z30.d\n"
        "spm.ld1d z3.d, p0/z, [x23]\n"
        "fmov x10, d3\n"
        "eor x10, x10, x8, lsl #38\n"
        "and x11, x10, %[mskA]\n"
        "ubfx x12, x11, #40, #2\n"
        "add x13, %[base], x12, lsl #16\n"
        "lsr x12, x11, #42\n"
        "add x23, x13, x12, lsl #6\n"
        "fmla z27.d, p0/m, z3.d, z30.d\n"
        "add x8, x8, #1\n"
        "cmp x8, %[n]\n"
        "b.lt 1b\n"
        "fadd z24.d, z24.d, z25.d\n"
        "fadd z26.d, z26.d, z27.d\n"
        "fadd z24.d, z24.d, z26.d\n"
        "faddv d24, p0, z24.d\n"
        "fmov %d[out], d24\n"
        : [out] "=w"(out)
        : [base] "r"(spm_base), [n] "r"(steps), [mskA] "r"(mask_hi),
          [wp] "r"(wp), [s0] "r"(s0), [s1] "r"(s1), [s2] "r"(s2),
          [s3] "r"(s3)
        : "x8", "x9", "x10", "x11", "x12", "x13", "x20", "x21", "x22",
          "x23", "p0", "z0", "z1", "z2", "z3", "z24", "z25", "z26", "z27",
          "z30", "cc", "memory");
    return out;
}

/* Hub/spoke walk (power-law graph): ~7/8 of steps land in the hot hub table
 * (src_lines, SPM-pinnable), ~1/8 chase into a large cold table that both
 * variants read from ordinary memory.  Cold traffic thrashes the baseline's
 * caches so its hub accesses lose residency; the SPM variant's hub accesses
 * are immune.  Hot/cold choice, both indices, and the step mixer come from
 * the loaded feature bits, so each step's address depends on the previous
 * load (latency-exposing chase). */
static double walk_hc_loop_cache(const double *hub, const double *cold,
                                 long steps, std::uint64_t hmask_hi,
                                 std::uint64_t cmask_lo, const double *wp,
                                 const double *s0, const double *s1,
                                 const double *s2, const double *s3)
{
    double out;
    asm volatile(
        "mov x9, #8\n"
        "whilelt p0.d, xzr, x9\n"
        "ld1rd z30.d, p0/z, [%[wp]]\n"
        "mov x15, #0x7c15\n"
        "movk x15, #0x7f4a, lsl #16\n"
        "movk x15, #0x79b9, lsl #32\n"
        "movk x15, #0x9e37, lsl #48\n"
        "dup z24.d, #0\n"
        "dup z25.d, #0\n"
        "dup z26.d, #0\n"
        "dup z27.d, #0\n"
        "mov x20, %[s0]\n"
        "mov x21, %[s1]\n"
        "mov x22, %[s2]\n"
        "mov x23, %[s3]\n"
        "mov x8, xzr\n"
        "1:\n"
        "mul x14, x8, x15\n"
        "ld1d z0.d, p0/z, [x20]\n"
        "fmov x10, d0\n"
        "eor x10, x10, x14\n"
        "and x11, x10, %[hmA]\n"
        "and x12, x10, %[cmA]\n"
        "add x11, %[hub], x11, lsr #34\n"
        "add x12, %[cold], x12, lsr #2\n"
        "tst x10, #7\n"
        "csel x20, x12, x11, eq\n"
        "fmla z24.d, p0/m, z0.d, z30.d\n"
        "ld1d z1.d, p0/z, [x21]\n"
        "fmov x10, d1\n"
        "eor x10, x10, x14\n"
        "and x11, x10, %[hmA]\n"
        "and x12, x10, %[cmA]\n"
        "add x11, %[hub], x11, lsr #34\n"
        "add x12, %[cold], x12, lsr #2\n"
        "tst x10, #7\n"
        "csel x21, x12, x11, eq\n"
        "fmla z25.d, p0/m, z1.d, z30.d\n"
        "ld1d z2.d, p0/z, [x22]\n"
        "fmov x10, d2\n"
        "eor x10, x10, x14\n"
        "and x11, x10, %[hmA]\n"
        "and x12, x10, %[cmA]\n"
        "add x11, %[hub], x11, lsr #34\n"
        "add x12, %[cold], x12, lsr #2\n"
        "tst x10, #7\n"
        "csel x22, x12, x11, eq\n"
        "fmla z26.d, p0/m, z2.d, z30.d\n"
        "ld1d z3.d, p0/z, [x23]\n"
        "fmov x10, d3\n"
        "eor x10, x10, x14\n"
        "and x11, x10, %[hmA]\n"
        "and x12, x10, %[cmA]\n"
        "add x11, %[hub], x11, lsr #34\n"
        "add x12, %[cold], x12, lsr #2\n"
        "tst x10, #7\n"
        "csel x23, x12, x11, eq\n"
        "fmla z27.d, p0/m, z3.d, z30.d\n"
        "add x8, x8, #1\n"
        "cmp x8, %[n]\n"
        "b.lt 1b\n"
        "fadd z24.d, z24.d, z25.d\n"
        "fadd z26.d, z26.d, z27.d\n"
        "fadd z24.d, z24.d, z26.d\n"
        "faddv d24, p0, z24.d\n"
        "fmov %d[out], d24\n"
        : [out] "=w"(out)
        : [hub] "r"(hub), [cold] "r"(cold), [n] "r"(steps),
          [hmA] "r"(hmask_hi), [cmA] "r"(cmask_lo), [wp] "r"(wp),
          [s0] "r"(s0), [s1] "r"(s1), [s2] "r"(s2), [s3] "r"(s3)
        : "x8", "x9", "x10", "x11", "x12", "x14", "x15",
          "x20", "x21", "x22", "x23", "p0",
          "z0", "z1", "z2", "z3", "z24", "z25", "z26", "z27", "z30",
          "cc", "memory");
    return out;
}

/* SPM hub/spoke walk: hub accesses hit the private SPM (addresses carry
 * kSpmBase bit 40, steering each load to spm.ld1d via a predictable branch);
 * cold accesses use ordinary loads like the baseline. */
static double walk_hc_loop_spm(std::uint64_t spm_base, const double *cold,
                               long steps, std::uint64_t hmask_hi,
                               std::uint64_t cmask_lo, const double *wp,
                               std::uint64_t s0, std::uint64_t s1,
                               std::uint64_t s2, std::uint64_t s3)
{
    double out;
    asm volatile(
        "mov x9, #8\n"
        "whilelt p0.d, xzr, x9\n"
        "ld1rd z30.d, p0/z, [%[wp]]\n"
        "mov x15, #0x7c15\n"
        "movk x15, #0x7f4a, lsl #16\n"
        "movk x15, #0x79b9, lsl #32\n"
        "movk x15, #0x9e37, lsl #48\n"
        "dup z24.d, #0\n"
        "dup z25.d, #0\n"
        "dup z26.d, #0\n"
        "dup z27.d, #0\n"
        "mov x20, %[s0]\n"
        "mov x21, %[s1]\n"
        "mov x22, %[s2]\n"
        "mov x23, %[s3]\n"
        "mov x8, xzr\n"
        "1:\n"
        "mul x14, x8, x15\n"
        "tbz x20, #40, 20f\n"
        "spm.ld1d z0.d, p0/z, [x20]\n"
        "b 21f\n"
        "20:\n"
        "ld1d z0.d, p0/z, [x20]\n"
        "21:\n"
        "fmov x10, d0\n"
        "eor x10, x10, x14\n"
        "and x11, x10, %[hmA]\n"
        "and x12, x10, %[cmA]\n"
        "ubfx x13, x11, #40, #2\n"
        "add x16, %[base], x13, lsl #16\n"
        "lsr x13, x11, #42\n"
        "add x11, x16, x13, lsl #6\n"
        "add x12, %[cold], x12, lsr #2\n"
        "tst x10, #7\n"
        "csel x20, x12, x11, eq\n"
        "fmla z24.d, p0/m, z0.d, z30.d\n"
        "tbz x21, #40, 22f\n"
        "spm.ld1d z1.d, p0/z, [x21]\n"
        "b 23f\n"
        "22:\n"
        "ld1d z1.d, p0/z, [x21]\n"
        "23:\n"
        "fmov x10, d1\n"
        "eor x10, x10, x14\n"
        "and x11, x10, %[hmA]\n"
        "and x12, x10, %[cmA]\n"
        "ubfx x13, x11, #40, #2\n"
        "add x16, %[base], x13, lsl #16\n"
        "lsr x13, x11, #42\n"
        "add x11, x16, x13, lsl #6\n"
        "add x12, %[cold], x12, lsr #2\n"
        "tst x10, #7\n"
        "csel x21, x12, x11, eq\n"
        "fmla z25.d, p0/m, z1.d, z30.d\n"
        "tbz x22, #40, 24f\n"
        "spm.ld1d z2.d, p0/z, [x22]\n"
        "b 25f\n"
        "24:\n"
        "ld1d z2.d, p0/z, [x22]\n"
        "25:\n"
        "fmov x10, d2\n"
        "eor x10, x10, x14\n"
        "and x11, x10, %[hmA]\n"
        "and x12, x10, %[cmA]\n"
        "ubfx x13, x11, #40, #2\n"
        "add x16, %[base], x13, lsl #16\n"
        "lsr x13, x11, #42\n"
        "add x11, x16, x13, lsl #6\n"
        "add x12, %[cold], x12, lsr #2\n"
        "tst x10, #7\n"
        "csel x22, x12, x11, eq\n"
        "fmla z26.d, p0/m, z2.d, z30.d\n"
        "tbz x23, #40, 26f\n"
        "spm.ld1d z3.d, p0/z, [x23]\n"
        "b 27f\n"
        "26:\n"
        "ld1d z3.d, p0/z, [x23]\n"
        "27:\n"
        "fmov x10, d3\n"
        "eor x10, x10, x14\n"
        "and x11, x10, %[hmA]\n"
        "and x12, x10, %[cmA]\n"
        "ubfx x13, x11, #40, #2\n"
        "add x16, %[base], x13, lsl #16\n"
        "lsr x13, x11, #42\n"
        "add x11, x16, x13, lsl #6\n"
        "add x12, %[cold], x12, lsr #2\n"
        "tst x10, #7\n"
        "csel x23, x12, x11, eq\n"
        "fmla z27.d, p0/m, z3.d, z30.d\n"
        "add x8, x8, #1\n"
        "cmp x8, %[n]\n"
        "b.lt 1b\n"
        "fadd z24.d, z24.d, z25.d\n"
        "fadd z26.d, z26.d, z27.d\n"
        "fadd z24.d, z24.d, z26.d\n"
        "faddv d24, p0, z24.d\n"
        "fmov %d[out], d24\n"
        : [out] "=w"(out)
        : [base] "r"(spm_base), [cold] "r"(cold), [n] "r"(steps),
          [hmA] "r"(hmask_hi), [cmA] "r"(cmask_lo), [wp] "r"(wp),
          [s0] "r"(s0), [s1] "r"(s1), [s2] "r"(s2), [s3] "r"(s3)
        : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16",
          "x20", "x21", "x22", "x23", "p0",
          "z0", "z1", "z2", "z3", "z24", "z25", "z26", "z27", "z30",
          "cc", "memory");
    return out;
}

/* Dynamic-graph updater: continuously re-acquires random hub lines for
 * writing while the walkers read them.  The stored value never changes (the
 * line is rewritten with its own contents, via asm so it cannot be elided),
 * so cache/SPM checksums stay bit-identical while the baseline pays the full
 * invalidate + 3-hop refetch coherence cost.  SPM snapshots are immune by
 * construction (the CacheFlex staleness contract; a no-op update is the
 * degenerate staleness-tolerant case). */
static void updater_loop(double *features, int src_lines, int gap,
                         const std::atomic<int> *done, int target)
{
    std::uint64_t s = 0xfeedfacecafef00dull;
    while (done->load(std::memory_order_acquire) < target) {
        const std::uint64_t line = lcg(s) % std::uint64_t(src_lines);
        double *p = features + line * 8;
        asm volatile(
            "ldr x9, [%0]\n"
            "str x9, [%0]\n"
            :
            : "r"(p)
            : "x9", "memory");
        for (int i = 0; i < gap; ++i) {
            asm volatile("" ::: "memory");
        }
    }
}

static inline void roi_begin()
{
#if defined(GEM5)
    m5_reset_stats(0, 0);
    m5_work_begin(0, 0);
#endif
}

static inline void roi_end()
{
#if defined(GEM5)
    m5_work_end(0, 0);
    m5_dump_stats(0, 0);
#endif
}

static void *worker(void *raw)
{
    ThreadArg *arg = static_cast<ThreadArg *>(raw);
    const Options &opt = arg->opt;
    if (opt.pin) pin_thread(arg->tid);

    // Shared mode: all walkers read one table (slice 0) and the last thread
    // becomes the feature updater instead of walking.
    const bool is_writer = opt.shared && arg->tid == opt.threads - 1;
    const int n_walkers = opt.shared ? opt.threads - 1 : opt.threads;
    double *features = opt.shared
        ? arg->features
        : arg->features + std::size_t(arg->tid) * opt.src_lines * opt.feat_lines * 8;
    // Software epoch-snapshot baseline: walkers copy the shared hub into
    // private memory once (same point as SPM staging) and chase the copy.
    // The updater's invalidations then only hit the copy source, not the
    // walk -- the strongest software-only counterpart to SPM snapshotting.
    const double *walk_tab = features;
    if (opt.shared && opt.snapshot && !is_writer) {
        double *snap = arg->snap + std::size_t(arg->tid) * opt.src_lines * 8;
        std::memcpy(snap, features,
                    std::size_t(opt.src_lines) * 64);
        walk_tab = snap;
    }
    std::uint32_t *idx = arg->idx + std::size_t(arg->tid) * opt.edges;
    double *weights = arg->weights + std::size_t(arg->tid) * opt.edges;
    // Each thread is pinned to its own core, whose SPM is private, so every
    // thread stripes its table across ways [0, spm_ways_used) of its own SPM
    // rather than a single tid-selected way.  ways is a power of two.
    const unsigned way_mask = unsigned(opt.spm_ways_used - 1);
    const unsigned way_shift = unsigned(__builtin_ctz(unsigned(opt.spm_ways_used)));
    const unsigned set0 = unsigned(opt.base_set);
    const auto slot = [&](unsigned line) {
        return spm_addr(line & way_mask, set0 + (line >> way_shift));
    };
    // Wide (F=8) layout: node n's F lines share way (n & mask) at consecutive
    // sets set0 + (n>>shift)*F + f, so the kernel forms the node base once.
    const unsigned F = unsigned(opt.feat_lines);
    const auto slotF = [&](unsigned node, unsigned f) {
        return spm_addr(node & way_mask, set0 + (node >> way_shift) * F + f);
    };

#if defined(USE_SPM)
    if (!is_writer) {
        if (opt.feat_lines == 1) {
            for (int l = 0; l < opt.src_lines; ++l)
                spmcp64(slot(unsigned(l)), features + std::size_t(l) * 8);
        } else {
            for (int n = 0; n < opt.src_lines; ++n)
                for (unsigned f = 0; f < F; ++f)
                    spmcp64(slotF(unsigned(n), f),
                            features + (std::size_t(n) * F + f) * 8);
        }
        asm volatile("dsb sy" ::: "memory");
    }
#endif

    double sum = 0.0;
    for (int rep = 0; rep < opt.warmup + opt.repeat; ++rep) {
        if (rep == opt.warmup) {
            pthread_barrier_wait(arg->barrier);
            if (arg->tid == 0) roi_begin();
            pthread_barrier_wait(arg->barrier);
        } else {
            pthread_barrier_wait(arg->barrier);
        }

        if (is_writer) {
            updater_loop(features, opt.src_lines, opt.update_gap,
                         &g_walkers_done, n_walkers * (rep + 1));
        } else if (opt.walk && opt.cold_lines > 0) {
            static const double kWalkW = 0.001953125; // 2^-9, exact
            const std::uint64_t hmask_hi =
                std::uint64_t(opt.src_lines - 1) << 40;
            const std::uint64_t cmask_lo =
                std::uint64_t(opt.cold_lines - 1) << 8;
            const unsigned q = opt.shared
                ? unsigned(opt.src_lines) / 16
                : unsigned(opt.src_lines) / 4;
            const unsigned b0 = opt.shared ? unsigned(4 * arg->tid) : 0;
            const double *cold =
                arg->cold + std::size_t(arg->tid) * opt.cold_lines * 8;
#if defined(USE_SPM)
            sum += walk_hc_loop_spm(spm_addr(0, set0), cold, opt.steps,
                                    hmask_hi, cmask_lo, &kWalkW,
                                    slot((b0 + 0) * q), slot((b0 + 1) * q),
                                    slot((b0 + 2) * q), slot((b0 + 3) * q));
#else
            sum += walk_hc_loop_cache(walk_tab, cold, opt.steps,
                                      hmask_hi, cmask_lo, &kWalkW,
                                      walk_tab + std::size_t((b0 + 0) * q) * 8,
                                      walk_tab + std::size_t((b0 + 1) * q) * 8,
                                      walk_tab + std::size_t((b0 + 2) * q) * 8,
                                      walk_tab + std::size_t((b0 + 3) * q) * 8);
#endif
        } else if (opt.walk) {
            static const double kWalkW = 0.001953125; // 2^-9, exact
            const std::uint64_t mask_hi =
                std::uint64_t(opt.src_lines - 1) << 40;
            // Shared mode: decorrelate walker start nodes across threads so
            // walks over the shared table do not run in lockstep.
            const unsigned q = opt.shared
                ? unsigned(opt.src_lines) / 16
                : unsigned(opt.src_lines) / 4;
            const unsigned b0 = opt.shared ? unsigned(4 * arg->tid) : 0;
#if defined(USE_SPM)
            sum += walk_loop_spm(spm_addr(0, set0), opt.steps, mask_hi,
                                 &kWalkW,
                                 slot((b0 + 0) * q), slot((b0 + 1) * q),
                                 slot((b0 + 2) * q), slot((b0 + 3) * q));
#else
            sum += walk_loop_cache(walk_tab, opt.steps, mask_hi, &kWalkW,
                                   walk_tab + std::size_t((b0 + 0) * q) * 8,
                                   walk_tab + std::size_t((b0 + 1) * q) * 8,
                                   walk_tab + std::size_t((b0 + 2) * q) * 8,
                                   walk_tab + std::size_t((b0 + 3) * q) * 8);
#endif
        } else {
#if defined(USE_SPM)
            if (opt.feat_lines == 8)
                sum += spmm_wide8_loop_spm(idx, weights, spm_addr(0, set0),
                                           opt.edges, way_mask, way_shift);
            else
                sum += spmm_loop_spm(idx, weights, spm_addr(0, set0),
                                     opt.edges, way_mask, way_shift);
#else
            if (opt.feat_lines == 8)
                sum += spmm_wide8_loop_cache(idx, weights, features, opt.edges);
            else
                sum += spmm_loop_cache(idx, weights, features, opt.edges,
                                       opt.prefetch);
#endif
        }
        if (!is_writer && opt.shared) {
            g_walkers_done.fetch_add(1, std::memory_order_release);
        }
        pthread_barrier_wait(arg->barrier);
    }

    pthread_barrier_wait(arg->barrier);
    if (arg->tid == 0) roi_end();
    pthread_barrier_wait(arg->barrier);

#if defined(USE_SPM)
    if (!is_writer) {
        if (opt.feat_lines == 1) {
            for (int l = 0; l < opt.src_lines; ++l)
                spm_release(slot(unsigned(l)));
        } else {
            for (int n = 0; n < opt.src_lines; ++n)
                for (unsigned f = 0; f < F; ++f)
                    spm_release(slotF(unsigned(n), f));
        }
        asm volatile("dsb sy" ::: "memory");
    }
#endif
    arg->result = sum;
    return nullptr;
}

} // namespace

int main(int argc, char **argv)
{
    Options opt = parse_args(argc, argv);
    const std::size_t feature_count =
        std::size_t(opt.threads) * opt.src_lines * opt.feat_lines * 8;
    const std::size_t edge_count = std::size_t(opt.threads) * opt.edges;
    double *features = static_cast<double *>(aligned_bytes(feature_count * sizeof(double)));
    // Padded and zeroed past the end: the prefetch-ahead kernel reads idx up
    // to 36 entries beyond the last edge of the final thread's slice.
    const std::size_t idx_alloc = (edge_count + 64) * sizeof(std::uint32_t);
    std::uint32_t *idx = static_cast<std::uint32_t *>(aligned_bytes(idx_alloc));
    std::memset(idx, 0, idx_alloc);
    double *weights = static_cast<double *>(aligned_bytes(edge_count * sizeof(double)));
    init_features(features, feature_count);
    for (int t = 0; t < opt.threads; ++t) {
        init_edges(idx + std::size_t(t) * opt.edges,
                   weights + std::size_t(t) * opt.edges,
                   opt.edges, opt.src_lines, t);
    }
    double *cold = nullptr;
    if (opt.cold_lines > 0) {
        const std::size_t cold_count =
            std::size_t(opt.threads) * opt.cold_lines * 8;
        cold = static_cast<double *>(aligned_bytes(cold_count * sizeof(double)));
        init_features(cold, cold_count);
    }
    double *snap = nullptr;
    if (opt.shared && opt.snapshot) {
        snap = static_cast<double *>(
            aligned_bytes(feature_count * sizeof(double)));
    }

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, nullptr, opt.threads);
    std::vector<pthread_t> tids(opt.threads);
    std::vector<ThreadArg> args(opt.threads);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    for (int t = 0; t < opt.threads; ++t) {
        args[t].opt = opt;
        args[t].tid = t;
        args[t].barrier = &barrier;
        args[t].features = features;
        args[t].idx = idx;
        args[t].weights = weights;
        args[t].cold = cold;
        args[t].snap = snap;
        if (pthread_create(&tids[t], &attr, worker, &args[t]) != 0) {
            std::perror("pthread_create");
            return 1;
        }
    }
    pthread_attr_destroy(&attr);
    double sum = 0.0;
    for (int t = 0; t < opt.threads; ++t) {
        pthread_join(tids[t], nullptr);
        sum += args[t].result;
    }
    pthread_barrier_destroy(&barrier);

    const char *variant =
#if defined(USE_SPM)
        "spm";
#else
        "cache";
#endif
    const double edge_ops = opt.walk
        ? double(opt.repeat) * opt.threads * opt.steps * 4.0
        : double(opt.repeat) * opt.threads * opt.edges;
    const double flops = edge_ops * 16.0 * opt.feat_lines; // per 64B feature line.
    const double bytes = edge_ops * (opt.walk ? 64.0
                                              : 64.0 * opt.feat_lines + 8.0 + 4.0);
    std::printf("graph_spmm_tile variant=%s mode=%s threads=%d src_lines=%d feat_lines=%d cold_lines=%d shared=%d snapshot=%d update_gap=%d edges=%d steps=%d repeat=%d prefetch=%d flops=%.0f bytes=%.0f checksum=%.17g\n",
                variant,
                opt.walk ? (opt.cold_lines ? "walk_hc" : "walk") : "spmm",
                opt.threads, opt.src_lines, opt.feat_lines, opt.cold_lines,
                opt.shared, opt.snapshot, opt.update_gap, opt.edges, opt.steps,
                opt.repeat, opt.prefetch, flops, bytes, sum);

    std::free(features);
    std::free(idx);
    std::free(weights);
    std::free(cold);
    std::free(snap);
    return 0;
}
