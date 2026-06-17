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
#define GEMM_SPM_NATIVE 1
#endif

#if defined(GEM5)
#include <gem5/m5ops.h>
#endif

namespace {

// SPM variant of gemm_blocked: identical blocking, but each thread stages its
// current kc x nc panel of B in the core-private L1 SPM (CacheFlex encoded
// slot space, way<<16 | set<<6) and streams it with spm.ld1w instead of
// coherent loads. Defaults are W4 (BERT-Base attention projection).
//
// The encoded slot window is placed at physical 1 GiB: gem5 SE allocates
// program pages upward from PA 0, so a zero-based window would alias the
// program's own physical memory (and stray ifetches into X-state slots
// panic the protocol). The way decode masks to 3 bits, so the base bits
// pass through untouched.
constexpr std::uint64_t kSpmWindowBase = 0x40000000ull;
struct Options {
    int threads = 1;
    int m = 512;
    int k = 768;
    int n = 768;
    int kc = 128;
    int nc = 64;
    int warmup = 1;
    int repeat = 1;
    int verify = 16;
    int pin = 1;
    int spm_way0 = 0;
    int spm_base_set = 0;
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
        "  --threads N    pthread workers, default 1\n"
        "  --m N          C/A rows, default 512 (W4)\n"
        "  --k N          inner dimension, default 768 (W4); multiple of kc\n"
        "  --n N          C/B columns, default 768 (W4); multiple of nc\n"
        "  --kc N         K block size (B panel rows), default 128\n"
        "  --nc N         N block size (B panel cols), default 64; mult of 16\n"
        "  --warmup N     untimed warmup repetitions, default 1\n"
        "  --repeat N     timed repetitions, default 1\n"
        "  --verify N     sampled output checks, 0 disables, default 16\n"
        "  --pin 0|1      pin worker i to CPU i where supported, default 1\n"
        "  --spm-way0 N   first private-L1 way claimed for SPM, default 0\n"
        "  --spm-base-set N  first SPM set claimed in that way, default 0\n",
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
        } else if (arg == "--spm-way0") {
            opt.spm_way0 = parse_int(need_value("--spm-way0"), "--spm-way0", 0);
        } else if (arg == "--spm-base-set") {
            opt.spm_base_set =
                parse_int(need_value("--spm-base-set"), "--spm-base-set", 0);
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
    if (opt.k % opt.kc != 0 || opt.n % opt.nc != 0 || opt.nc % 16 != 0 ||
        opt.n % 16 != 0) {
        std::fprintf(stderr,
                     "SPM variant requires k %% kc == 0, n %% nc == 0, and "
                     "nc, n multiples of 16 (one 64B SPM line per vector)\n");
        std::exit(2);
    }
    // One kc x nc panel claims kc*nc/16 SPM lines, one line per (way, set)
    // slot, 1024 sets per way; set overflow carries linearly into the next
    // way. The software contract leaves >= 2 of the 8 private L1 ways
    // coherent.
    const long lines = static_cast<long>(opt.kc) * (opt.nc / 16);
    if (opt.spm_base_set >= 1024 ||
        opt.spm_way0 * 1024L + opt.spm_base_set + lines > 6 * 1024L) {
        std::fprintf(stderr,
                     "panel of %ld lines at way %d set %d exceeds SPM ways "
                     "0..5 (>= 2 coherent ways required)\n",
                     lines, opt.spm_way0, opt.spm_base_set);
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

#if defined(GEMM_SPM_NATIVE)

static inline void spm_dsb()
{
    asm volatile("dsb sy" ::: "memory");
}

// Copy one 64B line from coherent memory into the encoded SPM slot.
static inline void spmcp64(std::uint64_t dst_spm, const void *src)
{
    asm volatile("SPMCP_64_IMM %0, [%1, #0]\n"
                 :
                 : "r"(dst_spm), "r"(src)
                 : "memory");
}

// acc += sum_p a[p] * spm_line(spm + p*stride); one full 16-float vector
// (exactly one SPM line) per spm.ld1w.
static inline svfloat32_t
spm_fma_column(svfloat32_t acc, const float *a, std::uint64_t spm,
               std::uint64_t stride, std::uint64_t count)
{
    asm volatile(
        "ptrue p7.s\n"
        "1:\n"
        "ld1rw {z30.s}, p7/z, [%[a]]\n"
        "spm.ld1w z31.s, p7/z, [%[spm]]\n"
        "fmla %[acc].s, p7/m, z31.s, z30.s\n"
        "add %[a], %[a], #4\n"
        "add %[spm], %[spm], %[stride]\n"
        "subs %[n], %[n], #1\n"
        "b.ne 1b\n"
        : [acc] "+w"(acc), [a] "+r"(a), [spm] "+r"(spm), [n] "+r"(count)
        : [stride] "r"(stride)
        : "p7", "z30", "z31", "memory", "cc");
    return acc;
}

#endif // GEMM_SPM_NATIVE

// Stage the kc x nc panel B[pc:pc+kc][jc:jc+nc] into SPM (or the host
// emulation buffer), then run all owned rows against it.
static void run_rows(const Shared &shared, int row0, int row1, float *scratch)
{
    const Options &opt = shared.opt;
    const float *A = shared.a.ptr;
    const float *B = shared.b.ptr;
    float *C = shared.c.ptr;
    const int K = opt.k;
    const int N = opt.n;
    const int lines_per_row = opt.nc / 16;
    const std::uint64_t spm_base = kSpmWindowBase +
        (static_cast<std::uint64_t>(opt.spm_way0) << 16) +
        (static_cast<std::uint64_t>(opt.spm_base_set) << 6);

    for (int jc = 0; jc < N; jc += opt.nc) {
        for (int pc = 0; pc < K; pc += opt.kc) {
#if defined(GEMM_SPM_NATIVE)
            (void)scratch;
            spm_dsb();
            for (int p = 0; p < opt.kc; ++p) {
                const float *src = B + static_cast<std::size_t>(pc + p) * N + jc;
                const std::uint64_t dst =
                    spm_base + static_cast<std::uint64_t>(p) * lines_per_row * 64;
                for (int l = 0; l < lines_per_row; ++l) {
                    spmcp64(dst + static_cast<std::uint64_t>(l) * 64, src + l * 16);
                }
            }
            spm_dsb();
            for (int i = row0; i < row1; ++i) {
                const float *a = A + static_cast<std::size_t>(i) * K + pc;
                for (int jv = 0; jv < lines_per_row; ++jv) {
                    float *c = C + static_cast<std::size_t>(i) * N + jc + jv * 16;
                    const svbool_t pg = svptrue_b32();
                    svfloat32_t acc = svld1(pg, c);
                    acc = spm_fma_column(acc, a,
                                         spm_base + static_cast<std::uint64_t>(jv) * 64,
                                         static_cast<std::uint64_t>(opt.nc) * 4,
                                         static_cast<std::uint64_t>(opt.kc));
                    svst1(pg, c, acc);
                }
            }
#else
            // Host build: emulate the SPM panel with a private buffer so the
            // blocking/copy logic can be validated off-target.
            for (int p = 0; p < opt.kc; ++p) {
                std::memcpy(scratch + static_cast<std::size_t>(p) * opt.nc,
                            B + static_cast<std::size_t>(pc + p) * N + jc,
                            static_cast<std::size_t>(opt.nc) * sizeof(float));
            }
            for (int i = row0; i < row1; ++i) {
                const float *a = A + static_cast<std::size_t>(i) * K + pc;
                for (int j = 0; j < opt.nc; ++j) {
                    float acc = C[static_cast<std::size_t>(i) * N + jc + j];
                    for (int p = 0; p < opt.kc; ++p) {
                        acc += a[p] * scratch[static_cast<std::size_t>(p) * opt.nc + j];
                    }
                    C[static_cast<std::size_t>(i) * N + jc + j] = acc;
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
    std::vector<float> scratch(
        static_cast<std::size_t>(shared.opt.kc) * shared.opt.nc);
    for (int rep = 0; rep < shared.total_reps; ++rep) {
        pthread_barrier_wait(&shared.barrier);
        run_rows(shared, row0, row1, scratch.data());
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

#if defined(GEMM_SPM_NATIVE)
    if (svcntw() != 16) {
        std::fprintf(stderr,
                     "SPM layout requires VL=512b (16 words/vector); got %u\n",
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
    std::vector<float> scratch(static_cast<std::size_t>(opt.kc) * opt.nc);
    for (int rep = 0; rep < shared.total_reps; ++rep) {
        if (rep == opt.warmup) {
            roi_begin();
        }
        pthread_barrier_wait(&shared.barrier);
        run_rows(shared, row0, row1, scratch.data());
        pthread_barrier_wait(&shared.barrier);
    }
    roi_end();

    for (pthread_t thread : threads) {
        pthread_join(thread, nullptr);
    }
    pthread_barrier_destroy(&shared.barrier);

    const int bad = opt.verify > 0 ? verify_samples(shared) : 0;
    const double ck = checksum(shared.c.ptr, shared.c.count);
    std::printf("gemm_spm threads=%d m=%d k=%d n=%d kc=%d nc=%d spm_way0=%d "
                "spm_base_set=%d warmup=%d repeat=%d verify=%s checksum=%.9f\n",
                opt.threads, opt.m, opt.k, opt.n, opt.kc, opt.nc, opt.spm_way0,
                opt.spm_base_set, opt.warmup, opt.repeat,
                opt.verify == 0 ? "off" : (bad == 0 ? "pass" : "FAIL"), ck);
    return bad == 0 ? 0 : 1;
}
