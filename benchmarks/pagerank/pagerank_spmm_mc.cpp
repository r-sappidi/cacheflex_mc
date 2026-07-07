// PageRank-style SpMM power iteration over a shared feature table.
//
// All cores own an m/T-node chunk of a shared table X (m nodes x 8 double
// features).  Per iteration every core gathers K random in-edges per owned
// node from the *whole* table and rewrites its chunk into the other double
// buffer after a barrier:  X'[i] = c0 + sum_e w_e * X[src_e].
//
// This is the writer-side coherence benchmark: in the naive baseline every
// core ends each epoch holding shared copies of most of X, so each core's
// next-epoch chunk writes pay GETX + invalidation of up to T-1 sharers + ack
// collection per line -- the synchronization-boundary storm.  The
// software-snapshot baseline (per-iteration memcpy to private memory) avoids
// gather-side refetches but its coherent copy-reads still register sharers,
// so the write storm remains.  The SPM variant re-snapshots X into private
// SPM each epoch via GETS_SILENT (no sharers registered, owners keep M), so
// chunk writes are pure local hits with zero coherence messages.
//
// All per-epoch acquisition costs (SPMCP restaging / memcpy) are inside the
// ROI.  Checksums must match bit-exactly across all three variants.

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

struct Options {
    int threads = 8;
    int nodes = 4096;    // m; 8 double features per node = one 64B line each
    int degree = 64;     // K in-edges per node, multiple of 4
    int iters = 6;
    int repeat = 1;      // outer repetitions of the whole power iteration
    int pin = 1;
    int base_set = 0;
    int snapshot = 0;    // cache variant: per-iteration memcpy baseline
};

struct ThreadArg {
    Options opt;
    int tid = 0;
    pthread_barrier_t *barrier = nullptr;
    double *xa = nullptr;
    double *xb = nullptr;
    double *snap = nullptr;      // per-thread private snapshot buffers
    std::uint32_t *idx = nullptr;
    double *weights = nullptr;
    double *bias = nullptr;      // per-node c0: keeps the fixed point
                                 // node-dependent so checksums stay
                                 // sensitive to every gather forever
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
        else if (a == "--nodes") opt.nodes = parse_int(need("nodes"), "nodes", 8);
        else if (a == "--degree") opt.degree = parse_int(need("degree"), "degree", 4);
        else if (a == "--iters") opt.iters = parse_int(need("iters"), "iters", 1);
        else if (a == "--repeat") opt.repeat = parse_int(need("repeat"), "repeat", 1);
        else if (a == "--pin") opt.pin = parse_int(need("pin"), "pin", 0);
        else if (a == "--base-set") opt.base_set = parse_int(need("base-set"), "base-set", 0);
        else if (a == "--snapshot") opt.snapshot = parse_int(need("snapshot"), "snapshot", 0);
        else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            std::exit(2);
        }
    }
    if (opt.degree % 4 != 0) {
        std::fprintf(stderr, "degree must be a multiple of 4 (unrolled kernel)\n");
        std::exit(2);
    }
    if (opt.nodes % opt.threads != 0) {
        std::fprintf(stderr, "nodes must be divisible by threads\n");
        std::exit(2);
    }
    // Table is striped over 4 SPM ways: line l -> way l&3, set base + l>>2.
    if (opt.base_set + (opt.nodes + 3) / 4 > 1024) {
        std::fprintf(stderr, "nodes too large: base_set + nodes/4 must be <= 1024\n");
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

/* One output row: dst[0..7] = c0 + sum_{e<k} w[e] * table[idx[e]][0..7].
 * Unroll-4 with four independent vector accumulators so the random 64B
 * gathers, not an FP chain, limit throughput. */
static inline void row_gather_cache(const std::uint32_t *idx, const double *w,
                                    const double *table, long k,
                                    const double *c0, double *dst)
{
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
        "ld1rd z30.d, p0/z, [%[c0]]\n"
        "fadd z24.d, z24.d, z30.d\n"
        "st1d z24.d, p0, [%[dst]]\n"
        : [idx] "+r"(idx), [w] "+r"(w)
        : [tab] "r"(table), [n] "r"(k), [c0] "r"(c0), [dst] "r"(dst)
        : "x8", "x9", "x10", "x11", "p0",
          "z0", "z1", "z2", "z3", "z4", "z5", "z6", "z7",
          "z24", "z25", "z26", "z27", "z30", "cc", "memory");
}

/* Identical gather, but table lines come from this core's private SPM
 * (way = line & 3, set = base_set + line >> 2). */
static inline void row_gather_spm(const std::uint32_t *idx, const double *w,
                                  std::uint64_t spm_base, long k,
                                  const double *c0, double *dst)
{
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
        "and x12, x10, #3\n"
        "add x11, %[base], x12, lsl #16\n"
        "lsr x12, x10, #2\n"
        "add x11, x11, x12, lsl #6\n"
        "spm.ld1d z0.d, p0/z, [x11]\n"
        "ld1rd z1.d, p0/z, [%[w]]\n"
        "fmla z24.d, p0/m, z0.d, z1.d\n"
        "ldr w10, [%[idx], #4]\n"
        "and x12, x10, #3\n"
        "add x11, %[base], x12, lsl #16\n"
        "lsr x12, x10, #2\n"
        "add x11, x11, x12, lsl #6\n"
        "spm.ld1d z2.d, p0/z, [x11]\n"
        "ld1rd z3.d, p0/z, [%[w], #8]\n"
        "fmla z25.d, p0/m, z2.d, z3.d\n"
        "ldr w10, [%[idx], #8]\n"
        "and x12, x10, #3\n"
        "add x11, %[base], x12, lsl #16\n"
        "lsr x12, x10, #2\n"
        "add x11, x11, x12, lsl #6\n"
        "spm.ld1d z4.d, p0/z, [x11]\n"
        "ld1rd z5.d, p0/z, [%[w], #16]\n"
        "fmla z26.d, p0/m, z4.d, z5.d\n"
        "ldr w10, [%[idx], #12]\n"
        "and x12, x10, #3\n"
        "add x11, %[base], x12, lsl #16\n"
        "lsr x12, x10, #2\n"
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
        "ld1rd z30.d, p0/z, [%[c0]]\n"
        "fadd z24.d, z24.d, z30.d\n"
        "st1d z24.d, p0, [%[dst]]\n"
        : [idx] "+r"(idx), [w] "+r"(w)
        : [base] "r"(spm_base), [n] "r"(k), [c0] "r"(c0), [dst] "r"(dst)
        : "x8", "x9", "x10", "x11", "x12", "p0",
          "z0", "z1", "z2", "z3", "z4", "z5", "z6", "z7",
          "z24", "z25", "z26", "z27", "z30", "cc", "memory");
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

    const int rows = opt.nodes / opt.threads;
    const int row0 = arg->tid * rows;
    const unsigned set0 = unsigned(opt.base_set);
    const auto slot = [&](unsigned line) {
        return spm_addr(line & 3u, set0 + (line >> 2));
    };
    const std::uint32_t *idx = arg->idx + std::size_t(row0) * opt.degree;
    const double *weights = arg->weights + std::size_t(row0) * opt.degree;
    double *snap = arg->snap
        ? arg->snap + std::size_t(arg->tid) * opt.nodes * 8
        : nullptr;

    double checksum = 0.0;
    for (int rep = 0; rep < opt.repeat; ++rep) {
        double *src = arg->xa;
        double *dst = arg->xb;

        pthread_barrier_wait(arg->barrier);
        if (arg->tid == 0 && rep == 0) roi_begin();
        pthread_barrier_wait(arg->barrier);

        for (int it = 0; it < opt.iters; ++it) {
            // Per-epoch acquisition (measured, per variant):
#if defined(USE_SPM)
            // Re-snapshot the whole table into private SPM.  GETS_SILENT
            // registers no sharers and leaves remote owners in M.  Each core
            // starts at its own chunk and wraps, so the cores do not stampede
            // the same line simultaneously (own-chunk lines are also the
            // local-M fast path).
            for (int i = 0; i < opt.nodes; ++i) {
                const unsigned l =
                    unsigned((i + row0) % opt.nodes);
                spm_release(slot(l));
                spmcp64(slot(l), src + std::size_t(l) * 8);
            }
            // No dsb: the slot-aware LSQ bridge tracker defers a gather only
            // until its own slot's SPMCP completes, so staging overlaps the
            // gather phase instead of serializing in front of it.
            // (release -> reinstall same-slot order comes from in-order
            // store-queue drain.)
#else
            if (opt.snapshot) {
                std::memcpy(snap, src, std::size_t(opt.nodes) * 64);
            }
#endif

            for (int r = 0; r < rows; ++r) {
                const std::uint32_t *ri = idx + std::size_t(r) * opt.degree;
                const double *rw = weights + std::size_t(r) * opt.degree;
                double *out = dst + std::size_t(row0 + r) * 8;
                const double *c0 = arg->bias + row0 + r;
#if defined(USE_SPM)
                row_gather_spm(ri, rw, spm_addr(0, set0), opt.degree,
                               c0, out);
#else
                row_gather_cache(ri, rw, opt.snapshot ? snap : src,
                                 opt.degree, c0, out);
#endif
            }

            pthread_barrier_wait(arg->barrier);
            std::swap(src, dst);
        }

        if (arg->tid == 0 && rep == opt.repeat - 1) {
            // After the final swap, src holds the result.
            for (std::size_t i = 0; i < std::size_t(opt.nodes) * 8; i += 7) {
                checksum += src[i];
            }
        }
        pthread_barrier_wait(arg->barrier);
    }

    pthread_barrier_wait(arg->barrier);
    if (arg->tid == 0) roi_end();
    pthread_barrier_wait(arg->barrier);

#if defined(USE_SPM)
    for (int l = 0; l < opt.nodes; ++l) {
        spm_release(slot(unsigned(l)));
    }
    asm volatile("dsb sy" ::: "memory");
#endif
    arg->result = checksum;
    return nullptr;
}

} // namespace

int main(int argc, char **argv)
{
    Options opt = parse_args(argc, argv);
    const std::size_t table_count = std::size_t(opt.nodes) * 8;
    const std::size_t edge_count = std::size_t(opt.nodes) * opt.degree;

    double *xa = static_cast<double *>(aligned_bytes(table_count * sizeof(double)));
    double *xb = static_cast<double *>(aligned_bytes(table_count * sizeof(double)));
    std::uint32_t *idx = static_cast<std::uint32_t *>(aligned_bytes(edge_count * sizeof(std::uint32_t)));
    double *weights = static_cast<double *>(aligned_bytes(edge_count * sizeof(double)));
    double *snap = nullptr;
    if (opt.snapshot) {
        snap = static_cast<double *>(
            aligned_bytes(std::size_t(opt.threads) * table_count * sizeof(double)));
    }

    init_features(xa, table_count);
    std::memset(xb, 0, table_count * sizeof(double));
    std::uint64_t s = 0x123456789abcdef0ull;
    const double w0 = 0.85 / double(opt.degree);
    for (std::size_t e = 0; e < edge_count; ++e) {
        idx[e] = std::uint32_t(lcg(s) % std::uint64_t(opt.nodes));
        weights[e] = w0;
    }
    double *bias = static_cast<double *>(
        aligned_bytes(std::size_t(opt.nodes) * sizeof(double)));
    for (int i = 0; i < opt.nodes; ++i) {
        bias[i] = 0.15 * (1.0 + double((i * 2654435761u) >> 26) / 64.0);
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
        args[t].xa = xa;
        args[t].xb = xb;
        args[t].snap = snap;
        args[t].idx = idx;
        args[t].weights = weights;
        args[t].bias = bias;
    }
    // Worker 0 runs on the main thread: SE mode has exactly `threads`
    // hardware contexts, so only threads-1 pthreads are spawned.
    for (int t = 1; t < opt.threads; ++t) {
        if (pthread_create(&tids[t], &attr, worker, &args[t]) != 0) {
            std::perror("pthread_create");
            return 1;
        }
    }
    pthread_attr_destroy(&attr);
    worker(&args[0]);
    double sum = args[0].result;
    for (int t = 1; t < opt.threads; ++t) {
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
    const double gathers = double(opt.repeat) * opt.iters * opt.nodes * opt.degree;
    const double flops = gathers * 16.0;
    std::printf("pagerank_spmm variant=%s snapshot=%d threads=%d nodes=%d degree=%d iters=%d repeat=%d flops=%.0f checksum=%.17g\n",
                variant, opt.snapshot, opt.threads, opt.nodes, opt.degree,
                opt.iters, opt.repeat, flops, sum);

    std::free(xa);
    std::free(xb);
    std::free(idx);
    std::free(weights);
    std::free(bias);
    std::free(snap);
    return 0;
}
