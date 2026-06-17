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
#endif

#if defined(GEM5)
#include <gem5/m5ops.h>
#endif

namespace {

struct Options {
    int threads = 1;
    int heads = 8;
    int seq_len = 512;
    int head_dim = 64;
    int q_tile = 16;
    int kv_tile = 64;
    int warmup = 1;
    int repeat = 3;
    int pin = 0;
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

struct Benchmark {
    Options opt;
    AlignedFloats q;
    AlignedFloats k;
    AlignedFloats v;
    AlignedFloats out;
};

struct WorkerArgs {
    Benchmark *bench = nullptr;
    int tid = 0;
    int repeat = 1;
};

static void usage(const char *argv0)
{
    std::fprintf(
        stderr,
        "usage: %s [options]\n"
        "  --threads N   pthread workers, default 1\n"
        "  --heads N     attention heads, default 8\n"
        "  --seq-len N   tokens per head, default 512\n"
        "  --head-dim N  features per head, default 64\n"
        "  --q-tile N    query rows per task tile, default 16\n"
        "  --kv-tile N   key/value rows per streaming tile, default 64\n"
        "  --warmup N    untimed warmup repetitions, default 1\n"
        "  --repeat N    timed repetitions, default 3\n"
        "  --pin 0|1     pin worker i to CPU i where supported, default 0\n",
        argv0);
}

static int parse_int(const char *s, const char *name)
{
    char *end = nullptr;
    errno = 0;
    long value = std::strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || value <= 0 || value > INT32_MAX) {
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
            opt.threads = parse_int(need_value("--threads"), "--threads");
        } else if (arg == "--heads") {
            opt.heads = parse_int(need_value("--heads"), "--heads");
        } else if (arg == "--seq-len") {
            opt.seq_len = parse_int(need_value("--seq-len"), "--seq-len");
        } else if (arg == "--head-dim") {
            opt.head_dim = parse_int(need_value("--head-dim"), "--head-dim");
        } else if (arg == "--q-tile") {
            opt.q_tile = parse_int(need_value("--q-tile"), "--q-tile");
        } else if (arg == "--kv-tile") {
            opt.kv_tile = parse_int(need_value("--kv-tile"), "--kv-tile");
        } else if (arg == "--warmup") {
            opt.warmup = parse_int(need_value("--warmup"), "--warmup");
        } else if (arg == "--repeat") {
            opt.repeat = parse_int(need_value("--repeat"), "--repeat");
        } else if (arg == "--pin") {
            opt.pin = parse_int(need_value("--pin"), "--pin") != 0;
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            usage(argv[0]);
            std::exit(2);
        }
    }

    if (opt.q_tile > opt.seq_len || opt.kv_tile > opt.seq_len) {
        std::fprintf(stderr, "tile sizes must not exceed seq-len\n");
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

static inline float dot_product(const float *a, const float *b, int n)
{
#if defined(__ARM_FEATURE_SVE)
    svfloat32_t acc = svdup_f32(0.0f);
    int i = 0;
    while (i < n) {
        svbool_t pg = svwhilelt_b32(i, n);
        svfloat32_t va = svld1(pg, a + i);
        svfloat32_t vb = svld1(pg, b + i);
        acc = svmla_f32_m(pg, acc, va, vb);
        i += svcntw();
    }
    return svaddv_f32(svptrue_b32(), acc);
#else
    float acc = 0.0f;
    for (int i = 0; i < n; ++i) {
        acc += a[i] * b[i];
    }
    return acc;
#endif
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

static void run_query_row(const Benchmark &bench, int head, int qi)
{
    const Options &opt = bench.opt;
    const int s = opt.seq_len;
    const int d = opt.head_dim;
    const float inv_sqrt_d = 1.0f / std::sqrt(static_cast<float>(d));
    const std::size_t head_base = static_cast<std::size_t>(head) * s * d;
    const float *q = bench.q.ptr + head_base + static_cast<std::size_t>(qi) * d;
    float *out = bench.out.ptr + head_base + static_cast<std::size_t>(qi) * d;

    std::vector<float> acc(static_cast<std::size_t>(d), 0.0f);
    float row_max = -INFINITY;
    float row_sum = 0.0f;

    for (int kb = 0; kb < s; kb += opt.kv_tile) {
        const int kend = std::min(kb + opt.kv_tile, s);
        for (int kj = kb; kj < kend; ++kj) {
            const float *k = bench.k.ptr + head_base + static_cast<std::size_t>(kj) * d;
            const float *v = bench.v.ptr + head_base + static_cast<std::size_t>(kj) * d;
            const float score = dot_product(q, k, d) * inv_sqrt_d;
            const float new_max = std::max(row_max, score);
            const float old_scale = row_max == -INFINITY ? 0.0f : std::exp(row_max - new_max);
            const float score_scale = std::exp(score - new_max);

            for (int x = 0; x < d; ++x) {
                acc[static_cast<std::size_t>(x)] =
                    acc[static_cast<std::size_t>(x)] * old_scale + score_scale * v[x];
            }
            row_sum = row_sum * old_scale + score_scale;
            row_max = new_max;
        }
    }

    const float inv_sum = 1.0f / row_sum;
    for (int x = 0; x < d; ++x) {
        out[x] = acc[static_cast<std::size_t>(x)] * inv_sum;
    }
}

static void run_range(Benchmark &bench, int tid, int repeat)
{
    const Options &opt = bench.opt;
    const int q_blocks = (opt.seq_len + opt.q_tile - 1) / opt.q_tile;
    const int tasks = opt.heads * q_blocks;

    for (int rep = 0; rep < repeat; ++rep) {
        for (int task = tid; task < tasks; task += opt.threads) {
            const int head = task / q_blocks;
            const int qb = task % q_blocks;
            const int q0 = qb * opt.q_tile;
            const int q1 = std::min(q0 + opt.q_tile, opt.seq_len);
            for (int qi = q0; qi < q1; ++qi) {
                run_query_row(bench, head, qi);
            }
        }
    }
}

static void *worker_entry(void *raw)
{
    WorkerArgs *args = static_cast<WorkerArgs *>(raw);
    if (args->bench->opt.pin) {
        pin_worker(args->tid);
    }
    run_range(*args->bench, args->tid, args->repeat);
    return nullptr;
}

static void run_parallel(Benchmark &bench, int repeat)
{
    const int spawned = std::max(0, bench.opt.threads - 1);
    std::vector<pthread_t> threads(static_cast<std::size_t>(spawned));
    std::vector<WorkerArgs> args(static_cast<std::size_t>(bench.opt.threads));
    for (int tid = 0; tid < spawned; ++tid) {
        args[static_cast<std::size_t>(tid)] = WorkerArgs{&bench, tid, repeat};
        const int rc = pthread_create(&threads[static_cast<std::size_t>(tid)], nullptr,
                                      worker_entry, &args[static_cast<std::size_t>(tid)]);
        if (rc != 0) {
            std::fprintf(stderr, "pthread_create failed: %s\n", std::strerror(rc));
            std::exit(1);
        }
    }
    args[static_cast<std::size_t>(bench.opt.threads - 1)] =
        WorkerArgs{&bench, bench.opt.threads - 1, repeat};
    worker_entry(&args[static_cast<std::size_t>(bench.opt.threads - 1)]);
    for (pthread_t thread : threads) {
        pthread_join(thread, nullptr);
    }
}

static double checksum(const float *data, std::size_t count)
{
    double sum = 0.0;
    for (std::size_t i = 0; i < count; i += 97) {
        sum += data[i];
    }
    return sum;
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
    const std::size_t elems = static_cast<std::size_t>(opt.heads) *
                              static_cast<std::size_t>(opt.seq_len) *
                              static_cast<std::size_t>(opt.head_dim);

    Benchmark bench{opt, AlignedFloats(elems), AlignedFloats(elems),
                    AlignedFloats(elems), AlignedFloats(elems)};

    init_tensor(bench.q.ptr, elems, 0.25f);
    init_tensor(bench.k.ptr, elems, 0.25f);
    init_tensor(bench.v.ptr, elems, 1.0f);
    std::fill(bench.out.ptr, bench.out.ptr + elems, 0.0f);

    run_parallel(bench, opt.warmup);
    roi_begin();
    run_parallel(bench, opt.repeat);
    roi_end();

    const double ck = checksum(bench.out.ptr, elems);
    std::printf("flash_attention_tiled threads=%d heads=%d seq_len=%d head_dim=%d "
                "q_tile=%d kv_tile=%d warmup=%d repeat=%d checksum=%.9f\n",
                opt.threads, opt.heads, opt.seq_len, opt.head_dim,
                opt.q_tile, opt.kv_tile, opt.warmup, opt.repeat, ck);
    return 0;
}
