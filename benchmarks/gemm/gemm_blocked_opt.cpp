#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <string>
#include <vector>

#if defined(__ARM_FEATURE_SVE)
#include <arm_sve.h>
#define GEMM_BLK_NATIVE 1
#endif

#if defined(GEM5)
#include <gem5/m5ops.h>
#endif

namespace {

// Register-blocked coherent (stock-MESI) GEMM baseline. Same BLIS-style
// 4 rows x 64 cols register-tile microkernel as gemm_spm_opt.cpp -- 16
// independent FP32 accumulators (z0..z15) held across the whole kc
// accumulation -- but the B vectors come from ordinary coherent loads (ld1w)
// instead of spm.ld1w. Keeping the microkernel identical makes this a
// best-vs-best comparison: the only difference between the two binaries is the
// instruction that sources B (coherent cache vs core-private SPM).
//
// nc is fixed at 64 = one 512b-VL register tile (4 SVE vectors). kc is the
// tunable K-block depth; the kc x nc B panel should stay hot in a private
// cache level across all rows a thread owns (kc=128 -> 32 KiB, fits the L0-D).
constexpr int kNc = 64;  // register-tile width in columns (4 * VL)
constexpr int kMr = 4;   // register-tile height in rows

struct Options {
    int threads = 1;
    int m = 512;
    int k = 768;
    int n = 768;
    int kc = 128;
    int nc = kNc;
    int warmup = 1;
    int repeat = 1;
    int verify = 16;
    int pin = 1;
};

struct AlignedFloats {
    float *ptr = nullptr;
    std::size_t count = 0;

    explicit AlignedFloats(std::size_t n = 0) : count(n)
    {
        if (n == 0) {
            return;
        }
        void *raw = nullptr;
        const int rc = posix_memalign(&raw, 64, n * sizeof(float));
        if (rc != 0) {
            std::fprintf(stderr, "posix_memalign failed: %s\n", std::strerror(rc));
            std::exit(1);
        }
        ptr = static_cast<float *>(raw);
    }

    ~AlignedFloats()
    {
        std::free(ptr);
    }

    AlignedFloats(const AlignedFloats &) = delete;
    AlignedFloats &operator=(const AlignedFloats &) = delete;
};

struct Shared {
    Options opt;
    AlignedFloats a;
    AlignedFloats b;
    AlignedFloats c;
    pthread_barrier_t barrier;
    int total_reps = 0;
};

struct WorkerArgs {
    Shared *shared = nullptr;
    int tid = 0;
};

static void usage(const char *argv0)
{
    std::fprintf(
        stderr,
        "usage: %s [options]\n"
        "  --threads N   pthread workers, default 1\n"
        "  --m N         C/A rows, default 512 (W4)\n"
        "  --k N         inner dimension, default 768 (W4); multiple of kc\n"
        "  --n N         C/B columns, default 768 (W4); multiple of 64\n"
        "  --kc N        K block size (B panel rows), default 128; divides k\n"
        "  --nc N        must be 64 (register-tile width); default 64\n"
        "  --warmup N    untimed warmup repetitions, default 1\n"
        "  --repeat N    timed repetitions, default 1\n"
        "  --verify N    sampled output checks, 0 disables, default 16\n"
        "  --pin 0|1     pin worker i to CPU i where supported, default 1\n",
        argv0);
}

static int parse_int(const char *s, const char *name, long min_value)
{
    char *end = nullptr;
    errno = 0;
    long value = std::strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || value < min_value ||
        value > INT32_MAX) {
        std::fprintf(stderr, "invalid %s: %s\n", name, s);
        std::exit(2);
    }
    return static_cast<int>(value);
}

static Options parse_args(int argc, char **argv)
{
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        auto need_value = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--threads") {
            opt.threads = parse_int(need_value("--threads"), "--threads", 1);
        } else if (arg == "--m") {
            opt.m = parse_int(need_value("--m"), "--m", 1);
        } else if (arg == "--k") {
            opt.k = parse_int(need_value("--k"), "--k", 1);
        } else if (arg == "--n") {
            opt.n = parse_int(need_value("--n"), "--n", 1);
        } else if (arg == "--kc") {
            opt.kc = parse_int(need_value("--kc"), "--kc", 1);
        } else if (arg == "--nc") {
            opt.nc = parse_int(need_value("--nc"), "--nc", 1);
        } else if (arg == "--warmup") {
            opt.warmup = parse_int(need_value("--warmup"), "--warmup", 0);
        } else if (arg == "--repeat") {
            opt.repeat = parse_int(need_value("--repeat"), "--repeat", 1);
        } else if (arg == "--verify") {
            opt.verify = parse_int(need_value("--verify"), "--verify", 0);
        } else if (arg == "--pin") {
            opt.pin = parse_int(need_value("--pin"), "--pin", 0) != 0;
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            usage(argv[0]);
            std::exit(2);
        }
    }

    if (opt.threads > opt.m) {
        std::fprintf(stderr, "threads must not exceed m\n");
        std::exit(2);
    }
    if (opt.nc != kNc) {
        std::fprintf(stderr,
                     "optimized blocked kernel fixes nc=%d (register-tile width)\n",
                     kNc);
        std::exit(2);
    }
    if (opt.n % kNc != 0 || opt.k % opt.kc != 0) {
        std::fprintf(stderr,
                     "optimized blocked kernel requires n %% %d == 0 and k %% kc == 0\n",
                     kNc);
        std::exit(2);
    }
    return opt;
}

static void init_tensor(float *data, std::size_t count, float scale)
{
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t x = static_cast<std::uint32_t>((i * 1103515245u + 12345u) >> 8);
        data[i] = scale * static_cast<float>(static_cast<int>(x % 2048) - 1024) / 1024.0f;
    }
}

static void pin_worker(int tid)
{
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(tid, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)tid;
#endif
}

#if defined(GEMM_BLK_NATIVE)

// Accumulate one 4-row x 64-col tile of C over kc K-steps. Identical to the
// SPM kernel except B comes from coherent memory: per K-step the 4 B vectors
// are the 64 contiguous columns of one B row (z16..z19), and the B pointer
// advances one full B row (ldb bytes = N*4) per step. 16 independent fmlas.
static inline void
blk_kernel_4x64(float *c0, float *c1, float *c2, float *c3, const float *a0,
                const float *a1, const float *a2, const float *a3,
                const float *b, std::uint64_t ldb, std::uint64_t kc)
{
    asm volatile(
        "ptrue p7.s\n"
        "ld1w {z0.s},  p7/z, [%[c0], #0, mul vl]\n"
        "ld1w {z1.s},  p7/z, [%[c0], #1, mul vl]\n"
        "ld1w {z2.s},  p7/z, [%[c0], #2, mul vl]\n"
        "ld1w {z3.s},  p7/z, [%[c0], #3, mul vl]\n"
        "ld1w {z4.s},  p7/z, [%[c1], #0, mul vl]\n"
        "ld1w {z5.s},  p7/z, [%[c1], #1, mul vl]\n"
        "ld1w {z6.s},  p7/z, [%[c1], #2, mul vl]\n"
        "ld1w {z7.s},  p7/z, [%[c1], #3, mul vl]\n"
        "ld1w {z8.s},  p7/z, [%[c2], #0, mul vl]\n"
        "ld1w {z9.s},  p7/z, [%[c2], #1, mul vl]\n"
        "ld1w {z10.s}, p7/z, [%[c2], #2, mul vl]\n"
        "ld1w {z11.s}, p7/z, [%[c2], #3, mul vl]\n"
        "ld1w {z12.s}, p7/z, [%[c3], #0, mul vl]\n"
        "ld1w {z13.s}, p7/z, [%[c3], #1, mul vl]\n"
        "ld1w {z14.s}, p7/z, [%[c3], #2, mul vl]\n"
        "ld1w {z15.s}, p7/z, [%[c3], #3, mul vl]\n"
        "1:\n"
        "ld1w {z16.s}, p7/z, [%[b], #0, mul vl]\n"
        "ld1w {z17.s}, p7/z, [%[b], #1, mul vl]\n"
        "ld1w {z18.s}, p7/z, [%[b], #2, mul vl]\n"
        "ld1w {z19.s}, p7/z, [%[b], #3, mul vl]\n"
        "add %[b], %[b], %[ldb]\n"
        "ld1rw {z20.s}, p7/z, [%[a0]]\n"
        "add %[a0], %[a0], #4\n"
        "ld1rw {z21.s}, p7/z, [%[a1]]\n"
        "add %[a1], %[a1], #4\n"
        "ld1rw {z22.s}, p7/z, [%[a2]]\n"
        "add %[a2], %[a2], #4\n"
        "ld1rw {z23.s}, p7/z, [%[a3]]\n"
        "add %[a3], %[a3], #4\n"
        "fmla z0.s,  p7/m, z16.s, z20.s\n"
        "fmla z1.s,  p7/m, z17.s, z20.s\n"
        "fmla z2.s,  p7/m, z18.s, z20.s\n"
        "fmla z3.s,  p7/m, z19.s, z20.s\n"
        "fmla z4.s,  p7/m, z16.s, z21.s\n"
        "fmla z5.s,  p7/m, z17.s, z21.s\n"
        "fmla z6.s,  p7/m, z18.s, z21.s\n"
        "fmla z7.s,  p7/m, z19.s, z21.s\n"
        "fmla z8.s,  p7/m, z16.s, z22.s\n"
        "fmla z9.s,  p7/m, z17.s, z22.s\n"
        "fmla z10.s, p7/m, z18.s, z22.s\n"
        "fmla z11.s, p7/m, z19.s, z22.s\n"
        "fmla z12.s, p7/m, z16.s, z23.s\n"
        "fmla z13.s, p7/m, z17.s, z23.s\n"
        "fmla z14.s, p7/m, z18.s, z23.s\n"
        "fmla z15.s, p7/m, z19.s, z23.s\n"
        "subs %[k], %[k], #1\n"
        "b.ne 1b\n"
        "st1w {z0.s},  p7, [%[c0], #0, mul vl]\n"
        "st1w {z1.s},  p7, [%[c0], #1, mul vl]\n"
        "st1w {z2.s},  p7, [%[c0], #2, mul vl]\n"
        "st1w {z3.s},  p7, [%[c0], #3, mul vl]\n"
        "st1w {z4.s},  p7, [%[c1], #0, mul vl]\n"
        "st1w {z5.s},  p7, [%[c1], #1, mul vl]\n"
        "st1w {z6.s},  p7, [%[c1], #2, mul vl]\n"
        "st1w {z7.s},  p7, [%[c1], #3, mul vl]\n"
        "st1w {z8.s},  p7, [%[c2], #0, mul vl]\n"
        "st1w {z9.s},  p7, [%[c2], #1, mul vl]\n"
        "st1w {z10.s}, p7, [%[c2], #2, mul vl]\n"
        "st1w {z11.s}, p7, [%[c2], #3, mul vl]\n"
        "st1w {z12.s}, p7, [%[c3], #0, mul vl]\n"
        "st1w {z13.s}, p7, [%[c3], #1, mul vl]\n"
        "st1w {z14.s}, p7, [%[c3], #2, mul vl]\n"
        "st1w {z15.s}, p7, [%[c3], #3, mul vl]\n"
        : [b] "+r"(b), [a0] "+r"(a0), [a1] "+r"(a1), [a2] "+r"(a2),
          [a3] "+r"(a3), [k] "+r"(kc)
        : [c0] "r"(c0), [c1] "r"(c1), [c2] "r"(c2), [c3] "r"(c3), [ldb] "r"(ldb)
        : "p7", "z0", "z1", "z2", "z3", "z4", "z5", "z6", "z7", "z8", "z9",
          "z10", "z11", "z12", "z13", "z14", "z15", "z16", "z17", "z18", "z19",
          "z20", "z21", "z22", "z23", "cc", "memory");
}

// Single-row tail for rows that do not fill a 4-row tile.
static inline void blk_kernel_1x64(float *c0, const float *a0, const float *b,
                                   std::uint64_t ldb, std::uint64_t kc)
{
    asm volatile(
        "ptrue p7.s\n"
        "ld1w {z0.s}, p7/z, [%[c0], #0, mul vl]\n"
        "ld1w {z1.s}, p7/z, [%[c0], #1, mul vl]\n"
        "ld1w {z2.s}, p7/z, [%[c0], #2, mul vl]\n"
        "ld1w {z3.s}, p7/z, [%[c0], #3, mul vl]\n"
        "1:\n"
        "ld1w {z16.s}, p7/z, [%[b], #0, mul vl]\n"
        "ld1w {z17.s}, p7/z, [%[b], #1, mul vl]\n"
        "ld1w {z18.s}, p7/z, [%[b], #2, mul vl]\n"
        "ld1w {z19.s}, p7/z, [%[b], #3, mul vl]\n"
        "add %[b], %[b], %[ldb]\n"
        "ld1rw {z20.s}, p7/z, [%[a0]]\n"
        "add %[a0], %[a0], #4\n"
        "fmla z0.s, p7/m, z16.s, z20.s\n"
        "fmla z1.s, p7/m, z17.s, z20.s\n"
        "fmla z2.s, p7/m, z18.s, z20.s\n"
        "fmla z3.s, p7/m, z19.s, z20.s\n"
        "subs %[k], %[k], #1\n"
        "b.ne 1b\n"
        "st1w {z0.s}, p7, [%[c0], #0, mul vl]\n"
        "st1w {z1.s}, p7, [%[c0], #1, mul vl]\n"
        "st1w {z2.s}, p7, [%[c0], #2, mul vl]\n"
        "st1w {z3.s}, p7, [%[c0], #3, mul vl]\n"
        : [b] "+r"(b), [a0] "+r"(a0), [k] "+r"(kc)
        : [c0] "r"(c0), [ldb] "r"(ldb)
        : "p7", "z0", "z1", "z2", "z3", "z16", "z17", "z18", "z19", "z20",
          "cc", "memory");
}

#endif // GEMM_BLK_NATIVE

// C[row0:row1] += A[row0:row1] * B, blocked so one kc x nc panel of B stays hot
// across all rows owned by this thread, processed in 4-row register tiles.
static void run_rows(const Shared &shared, int row0, int row1)
{
    const Options &opt = shared.opt;
    const float *A = shared.a.ptr;
    const float *B = shared.b.ptr;
    float *C = shared.c.ptr;
    const int K = opt.k;
    const int N = opt.n;

    for (int jc = 0; jc < N; jc += kNc) {
        for (int pc = 0; pc < K; pc += opt.kc) {
#if defined(GEMM_BLK_NATIVE)
            const float *bpanel = B + static_cast<std::size_t>(pc) * N + jc;
            const std::uint64_t ldb = static_cast<std::uint64_t>(N) * sizeof(float);
            int i = row0;
            for (; i + kMr <= row1; i += kMr) {
                float *c0 = C + static_cast<std::size_t>(i + 0) * N + jc;
                float *c1 = C + static_cast<std::size_t>(i + 1) * N + jc;
                float *c2 = C + static_cast<std::size_t>(i + 2) * N + jc;
                float *c3 = C + static_cast<std::size_t>(i + 3) * N + jc;
                const float *a0 = A + static_cast<std::size_t>(i + 0) * K + pc;
                const float *a1 = A + static_cast<std::size_t>(i + 1) * K + pc;
                const float *a2 = A + static_cast<std::size_t>(i + 2) * K + pc;
                const float *a3 = A + static_cast<std::size_t>(i + 3) * K + pc;
                blk_kernel_4x64(c0, c1, c2, c3, a0, a1, a2, a3, bpanel, ldb,
                                static_cast<std::uint64_t>(opt.kc));
            }
            for (; i < row1; ++i) {
                float *c0 = C + static_cast<std::size_t>(i) * N + jc;
                const float *a0 = A + static_cast<std::size_t>(i) * K + pc;
                blk_kernel_1x64(c0, a0, bpanel, ldb,
                                static_cast<std::uint64_t>(opt.kc));
            }
#else
            // Host fallback validates the blocking off-target.
            for (int i = row0; i < row1; ++i) {
                const float *a = A + static_cast<std::size_t>(i) * K;
                float *c = C + static_cast<std::size_t>(i) * N;
                for (int j = jc; j < jc + kNc; ++j) {
                    float acc = c[j];
                    for (int p = pc; p < pc + opt.kc; ++p) {
                        acc += a[p] * B[static_cast<std::size_t>(p) * N + j];
                    }
                    c[j] = acc;
                }
            }
#endif
        }
    }
}

static void thread_rows(const Shared &shared, int tid, int *row0, int *row1)
{
    const int rows_per = (shared.opt.m + shared.opt.threads - 1) / shared.opt.threads;
    *row0 = std::min(tid * rows_per, shared.opt.m);
    *row1 = std::min(*row0 + rows_per, shared.opt.m);
}

static void *worker_entry(void *raw)
{
    WorkerArgs *args = static_cast<WorkerArgs *>(raw);
    Shared &shared = *args->shared;
    if (shared.opt.pin) {
        pin_worker(args->tid);
    }
    int row0 = 0;
    int row1 = 0;
    thread_rows(shared, args->tid, &row0, &row1);
    for (int rep = 0; rep < shared.total_reps; ++rep) {
        pthread_barrier_wait(&shared.barrier);
        run_rows(shared, row0, row1);
        pthread_barrier_wait(&shared.barrier);
    }
    return nullptr;
}

static double checksum(const float *data, std::size_t count)
{
    double sum = 0.0;
    for (std::size_t i = 0; i < count; i += 97) {
        sum += data[i];
    }
    return sum;
}

static int verify_samples(const Shared &shared)
{
    const Options &opt = shared.opt;
    int bad = 0;
    for (int s = 0; s < opt.verify; ++s) {
        const int i = (s * 131 + 7) % opt.m;
        const int j = (s * 197 + 3) % opt.n;
        double ref = 0.0;
        for (int p = 0; p < opt.k; ++p) {
            ref += static_cast<double>(shared.a.ptr[static_cast<std::size_t>(i) * opt.k + p]) *
                   static_cast<double>(shared.b.ptr[static_cast<std::size_t>(p) * opt.n + j]);
        }
        ref *= shared.total_reps;
        const double got = shared.c.ptr[static_cast<std::size_t>(i) * opt.n + j];
        const double tol = 1e-3 * std::max(1.0, std::fabs(ref));
        if (std::fabs(got - ref) > tol) {
            std::fprintf(stderr, "verify mismatch at C[%d][%d]: got %f want %f\n",
                         i, j, got, ref);
            ++bad;
        }
    }
    return bad;
}

static void roi_begin()
{
#if defined(GEM5)
    m5_reset_stats(0, 0);
    m5_work_begin(0, 0);
#endif
}

static void roi_end()
{
#if defined(GEM5)
    m5_work_end(0, 0);
    m5_dump_stats(0, 0);
#endif
}

} // namespace

int main(int argc, char **argv)
{
    Options opt = parse_args(argc, argv);

#if defined(GEMM_BLK_NATIVE)
    if (svcntw() != 16) {
        std::fprintf(stderr,
                     "register tile requires VL=512b (16 words/vector); got %u\n",
                     static_cast<unsigned>(svcntw()));
        return 1;
    }
#endif

    Shared shared{opt,
                  AlignedFloats(static_cast<std::size_t>(opt.m) * opt.k),
                  AlignedFloats(static_cast<std::size_t>(opt.k) * opt.n),
                  AlignedFloats(static_cast<std::size_t>(opt.m) * opt.n),
                  {},
                  opt.warmup + opt.repeat};

    init_tensor(shared.a.ptr, shared.a.count, 0.25f);
    init_tensor(shared.b.ptr, shared.b.count, 0.25f);
    std::fill(shared.c.ptr, shared.c.ptr + shared.c.count, 0.0f);

    const int rc = pthread_barrier_init(&shared.barrier, nullptr,
                                        static_cast<unsigned>(opt.threads));
    if (rc != 0) {
        std::fprintf(stderr, "pthread_barrier_init failed: %s\n", std::strerror(rc));
        return 1;
    }

    std::vector<pthread_t> threads(static_cast<std::size_t>(opt.threads - 1));
    std::vector<WorkerArgs> args(static_cast<std::size_t>(opt.threads));
    for (int tid = 1; tid < opt.threads; ++tid) {
        args[static_cast<std::size_t>(tid)] = WorkerArgs{&shared, tid};
        const int prc = pthread_create(&threads[static_cast<std::size_t>(tid - 1)],
                                       nullptr, worker_entry,
                                       &args[static_cast<std::size_t>(tid)]);
        if (prc != 0) {
            std::fprintf(stderr, "pthread_create failed: %s\n", std::strerror(prc));
            return 1;
        }
    }

    if (opt.pin) {
        pin_worker(0);
    }
    int row0 = 0;
    int row1 = 0;
    thread_rows(shared, 0, &row0, &row1);
    for (int rep = 0; rep < shared.total_reps; ++rep) {
        if (rep == opt.warmup) {
            roi_begin();
        }
        pthread_barrier_wait(&shared.barrier);
        run_rows(shared, row0, row1);
        pthread_barrier_wait(&shared.barrier);
    }
    roi_end();

    for (pthread_t thread : threads) {
        pthread_join(thread, nullptr);
    }
    pthread_barrier_destroy(&shared.barrier);

    const int bad = opt.verify > 0 ? verify_samples(shared) : 0;
    const double ck = checksum(shared.c.ptr, shared.c.count);
    std::printf("gemm_blocked_opt threads=%d m=%d k=%d n=%d kc=%d nc=%d "
                "warmup=%d repeat=%d verify=%s checksum=%.9f\n",
                opt.threads, opt.m, opt.k, opt.n, opt.kc, opt.nc,
                opt.warmup, opt.repeat,
                opt.verify == 0 ? "off" : (bad == 0 ? "pass" : "FAIL"), ck);
    return bad == 0 ? 0 : 1;
}
