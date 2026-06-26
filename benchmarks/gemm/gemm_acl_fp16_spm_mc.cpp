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

#include <arm_sve.h>

#if defined(GEM5)
#include <gem5/m5ops.h>
#endif

#include "kernels_spm.hpp"

namespace {

constexpr std::size_t kSpmWindowBase = 0x40000000ull;
constexpr std::size_t kSpmBaseSet = kSpmWindowBase >> 6;

struct Options {
    int threads = 1, m = 128, k = 4096, n = 11008;
    int mc = 128, kc = 64, warmup = 1, repeat = 1, verify = 16, pin = 1;
    int grid_cols_override = 0;
};

struct Halfs {
    __fp16 *ptr = nullptr;
    std::size_t count = 0;
    explicit Halfs(std::size_t n = 0) : count(n) {
        if (!n) return;
        void *raw = nullptr;
        if (posix_memalign(&raw, 64, n * sizeof(__fp16)) != 0) std::exit(1);
        ptr = static_cast<__fp16 *>(raw);
    }
    ~Halfs() { std::free(ptr); }
    Halfs(const Halfs &) = delete;
    Halfs &operator=(const Halfs &) = delete;
    Halfs(Halfs &&o) noexcept : ptr(o.ptr), count(o.count) { o.ptr = nullptr; o.count = 0; }
    Halfs &operator=(Halfs &&o) noexcept {
        if (this != &o) { std::free(ptr); ptr = o.ptr; count = o.count; o.ptr = nullptr; o.count = 0; }
        return *this;
    }
};

struct Tile {
    int row0 = 0, row1 = 0, col0 = 0, col1 = 0;
    Halfs btail, c, acache, cpanel;
};

struct Shared {
    Options opt;
    Halfs a, b;
    pthread_barrier_t barrier{};
    int total_reps = 0;
    std::vector<Tile> tiles;
};

struct WorkerArgs { Shared *shared = nullptr; int tid = 0; };

static int parse_int(const char *s, const char *nm, long mn) {
    char *e = nullptr; errno = 0; long v = std::strtol(s, &e, 10);
    if (errno || e == s || *e || v < mn || v > INT32_MAX) {
        std::fprintf(stderr, "bad %s\n", nm); std::exit(2);
    }
    return static_cast<int>(v);
}

static Options parse_args(int argc, char **argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        auto nv = [&](const char *nm) -> const char * {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing %s\n", nm); std::exit(2); }
            return argv[++i];
        };
        if (arg == "--threads") opt.threads = parse_int(nv("threads"), "threads", 1);
        else if (arg == "--m") opt.m = parse_int(nv("m"), "m", 1);
        else if (arg == "--k") opt.k = parse_int(nv("k"), "k", 1);
        else if (arg == "--n") opt.n = parse_int(nv("n"), "n", 1);
        else if (arg == "--mc") opt.mc = parse_int(nv("mc"), "mc", 1);
        else if (arg == "--kc") opt.kc = parse_int(nv("kc"), "kc", 1);
        else if (arg == "--warmup") opt.warmup = parse_int(nv("warmup"), "warmup", 0);
        else if (arg == "--repeat") opt.repeat = parse_int(nv("repeat"), "repeat", 1);
        else if (arg == "--verify") opt.verify = parse_int(nv("verify"), "verify", 0);
        else if (arg == "--pin") opt.pin = parse_int(nv("pin"), "pin", 0) != 0;
        else if (arg == "--grid-cols") opt.grid_cols_override = parse_int(nv("grid-cols"), "grid-cols", 0);
        else { std::fprintf(stderr, "unknown %s\n", arg.c_str()); std::exit(2); }
    }
    if (opt.grid_cols_override > 0 && opt.threads % opt.grid_cols_override != 0) std::exit(2);
    return opt;
}

static void init_tensor(__fp16 *d, std::size_t n, float s) {
    for (std::size_t i = 0; i < n; ++i) {
        std::uint32_t x = static_cast<std::uint32_t>((i * 1103515245u + 12345u) >> 8);
        d[i] = static_cast<__fp16>(s * static_cast<float>(static_cast<int>(x % 2048) - 1024) / 1024.0f);
    }
}

static float tensor_value(std::size_t i, float s) {
    std::uint32_t x = static_cast<std::uint32_t>((i * 1103515245u + 12345u) >> 8);
    return s * static_cast<float>(static_cast<int>(x % 2048) - 1024) / 1024.0f;
}

static void pin_worker(int tid) {
#if defined(__linux__)
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(tid, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)tid;
#endif
}

static void split_range(int count, int parts, int idx, int *lo, int *hi) {
    int base = count / parts, rem = count % parts;
    *lo = idx * base + std::min(idx, rem);
    *hi = *lo + base + (idx < rem ? 1 : 0);
}

static void pack_B_tail_panel(const __fp16 *B, std::size_t N,
                              std::size_t source_stride, std::size_t kc,
                              std::size_t KC, __fp16 *Bpack) {
    const std::size_t NT = 3 * svcnth();
    for (std::size_t ki = 0; ki < KC; ++ki) {
        __fp16 *dst = Bpack + ki * NT;
        if (ki < kc) {
            std::memcpy(dst, B + ki * source_stride, N * sizeof(__fp16));
            if (N < NT) std::memset(dst + N, 0, (NT - N) * sizeof(__fp16));
        } else {
            std::memset(dst, 0, NT * sizeof(__fp16));
        }
    }
}

static int grid_cols(int threads, int panels) {
    for (int pc = threads; pc >= 1; --pc)
        if (threads % pc == 0 && panels % pc == 0) return pc;
    return 1;
}

static void assign_tile(const Options &opt, int tid, Tile *t) {
    const int nt = static_cast<int>(3 * svcnth());
    const int panels = (opt.n + nt - 1) / nt;
    int PC = opt.grid_cols_override > 0 ? opt.grid_cols_override : grid_cols(opt.threads, panels);
    int PR = opt.threads / PC, pr = tid / PC, pc = tid % PC;
    int p0, p1;
    split_range(opt.m, PR, pr, &t->row0, &t->row1);
    split_range(panels, PC, pc, &p0, &p1);
    t->col0 = p0 * nt;
    t->col1 = std::min(opt.n, p1 * nt);
}

static void prepare_tile(Shared &sh, int tid) {
    const Options &opt = sh.opt;
    Tile &t = sh.tiles[tid];
    assign_tile(opt, tid, &t);
    const int rows = t.row1 - t.row0;
    const int cols = t.col1 - t.col0;
    const std::size_t nt = 3 * svcnth();
    const std::size_t max_ab = (opt.mc + 7) / 8;
    t.btail = Halfs(static_cast<std::size_t>(opt.kc) * nt);
    t.c = Halfs(static_cast<std::size_t>(rows) * cols);
    t.acache = Halfs(((rows + opt.mc - 1) / opt.mc) * max_ab * 8 * opt.kc);
    t.cpanel = Halfs(max_ab * 8 * nt);
    std::memset(t.c.ptr, 0, t.c.count * sizeof(__fp16));
    std::memset(t.acache.ptr, 0, t.acache.count * sizeof(__fp16));
    std::memset(t.cpanel.ptr, 0, t.cpanel.count * sizeof(__fp16));
}

static void run_tile(const Shared &sh, Tile &t) {
    const Options &opt = sh.opt;
    const std::size_t M = t.row1 - t.row0;
    const std::size_t N = t.col1 - t.col0;
    const std::size_t K = opt.k;
    const std::size_t MC = opt.mc, KC = opt.kc;
    const std::size_t MT = 8, NT = 3 * svcnth();
    const std::size_t max_ab = (MC + MT - 1) / MT;
    const std::size_t ntiles = (N + NT - 1) / NT;
    const __fp16 *A = sh.a.ptr + static_cast<std::size_t>(t.row0) * K;

#if defined(VL_1) || defined(VL_2) || defined(VL_4)
    const std::size_t SPM_KC_FACTOR = 1;
#elif defined(VL_8)
    const std::size_t SPM_KC_FACTOR = 2;
#elif defined(VL_16)
    const std::size_t SPM_KC_FACTOR = 3;
#endif
    const std::size_t NSP = std::max<std::size_t>(1, 1024 / (KC * SPM_KC_FACTOR));

    for (std::size_t k0 = 0, kt = 0; k0 < K; k0 += KC, ++kt) {
        std::size_t kc = std::min(KC, K - k0);
        for (std::size_t m0 = 0; m0 < M; m0 += MC) {
            std::size_t mc = std::min(MC, M - m0);
            pack_A_fp16_8row(A, K, t.acache.ptr + (m0 / MC) * max_ab * MT * KC,
                             M, m0, K, k0, mc, kc);
        }
        for (std::size_t nbatch = 0; nbatch < ntiles; nbatch += NSP) {
            std::size_t batch_tiles = std::min(NSP, ntiles - nbatch);
            for (std::size_t ti = 0; ti < batch_tiles; ++ti) {
                std::size_t nidx = nbatch + ti;
                std::size_t base_set = kSpmBaseSet + ti * KC * SPM_KC_FACTOR;
                std::size_t n0 = nidx * NT;
                std::size_t nc = std::min(NT, N - n0);
                const __fp16 *Bp = sh.b.ptr + (k0 * opt.n) + (t.col0 + n0);
                if (nc == NT) {
                    pack_B_tile_to_spm(Bp, opt.n, kc, KC, base_set);
                } else {
                    pack_B_tail_panel(Bp, nc, opt.n, kc, KC, t.btail.ptr);
                    pack_B_tile_to_spm(t.btail.ptr, NT, kc, KC, base_set);
                }
            }
            for (std::size_t m0 = 0; m0 < M; m0 += MC) {
                std::size_t mc = std::min(MC, M - m0);
                std::size_t ablocks = (mc + MT - 1) / MT;
                const __fp16 *Ap = t.acache.ptr + (m0 / MC) * max_ab * MT * KC;
                for (std::size_t ti = 0; ti < batch_tiles; ++ti) {
                    std::size_t nidx = nbatch + ti;
                    std::size_t n0 = nidx * NT;
                    std::size_t nc = std::min(NT, N - n0);
                    std::size_t base_set = kSpmBaseSet + ti * KC * SPM_KC_FACTOR;
                    spm_kernel_mblocks(Ap, t.cpanel.ptr, static_cast<int>(kc), static_cast<int>(ablocks), KC, base_set);
                    for (std::size_t mb = 0; mb < ablocks; ++mb) {
                        const __fp16 *tp = t.cpanel.ptr + mb * MT * NT;
                        for (std::size_t r = 0; r < MT; ++r) {
                            std::size_t gr = m0 + mb * MT + r;
                            if (gr >= M) break;
                            __fp16 *c_row = t.c.ptr + gr * N + n0;
                            const __fp16 *x = tp + r * NT;
                            std::size_t i = 0; svbool_t pg = svptrue_b16();
                            if (k0 == 0) {
                                for (; i + svcnth() <= nc; i += svcnth()) svst1_f16(pg, c_row + i, svld1_f16(pg, x + i));
                                if (i < nc) { svbool_t pt = svwhilelt_b16_u64(i, nc); svst1_f16(pt, c_row + i, svld1_f16(pt, x + i)); }
                            } else {
                                for (; i + svcnth() <= nc; i += svcnth()) svst1_f16(pg, c_row + i, svadd_f16_x(pg, svld1_f16(pg, c_row + i), svld1_f16(pg, x + i)));
                                if (i < nc) { svbool_t pt = svwhilelt_b16_u64(i, nc); svst1_f16(pt, c_row + i, svadd_f16_x(pt, svld1_f16(pt, c_row + i), svld1_f16(pt, x + i))); }
                            }
                        }
                    }
                }
            }
        }
    }
}

static void roi_begin() {
#if defined(GEM5)
    m5_reset_stats(0, 0); m5_work_begin(0, 0);
#endif
}
static void roi_end() {
#if defined(GEM5)
    m5_work_end(0, 0); m5_dump_stats(0, 0);
#endif
}

static void *worker_entry(void *raw) {
    WorkerArgs *args = static_cast<WorkerArgs *>(raw);
    Shared &sh = *args->shared;
    if (sh.opt.pin) pin_worker(args->tid);
    prepare_tile(sh, args->tid);
    for (int rep = 0; rep < sh.total_reps; ++rep) {
        if (rep == sh.opt.warmup) {
            pthread_barrier_wait(&sh.barrier);
            pthread_barrier_wait(&sh.barrier);
        } else {
            pthread_barrier_wait(&sh.barrier);
        }
        run_tile(sh, sh.tiles[args->tid]);
        pthread_barrier_wait(&sh.barrier);
    }
    return nullptr;
}

static double checksum(const Shared &sh) {
    double s = 0;
    for (const Tile &t : sh.tiles)
        for (std::size_t i = 0; i < t.c.count; i += 97) s += static_cast<float>(t.c.ptr[i]);
    return s;
}

static int verify_samples(const Shared &sh) {
    int bad = 0;
    const Options &opt = sh.opt;
    for (int sample = 0; sample < opt.verify; ++sample) {
        int gi = (sample * 131 + 7) % opt.m;
        int gj = (sample * 197 + 3) % opt.n;
        const Tile *tile = nullptr;
        for (const Tile &t : sh.tiles) {
            if (gi >= t.row0 && gi < t.row1 && gj >= t.col0 && gj < t.col1) { tile = &t; break; }
        }
        if (!tile) { ++bad; continue; }
        double ref = 0;
        for (int p = 0; p < opt.k; ++p)
            ref += tensor_value(static_cast<std::size_t>(gi) * opt.k + p, 0.25f) *
                   tensor_value(static_cast<std::size_t>(p) * opt.n + gj, 0.25f);
        double got = static_cast<float>(tile->c.ptr[static_cast<std::size_t>(gi - tile->row0) * (tile->col1 - tile->col0) + (gj - tile->col0)]);
        double tol = 2e-2 * std::max(1.0, std::fabs(ref));
        if (std::fabs(got - ref) > tol) {
            std::fprintf(stderr, "mismatch %d %d got=%f ref=%f\n", gi, gj, got, ref);
            ++bad;
        }
    }
    return bad;
}

} // namespace

int main(int argc, char **argv) {
    Options opt = parse_args(argc, argv);
    const int runtime_vlh = static_cast<int>(svcnth());
    const int panels = (opt.n + 3 * runtime_vlh - 1) / (3 * runtime_vlh);
#if defined(VL_1)
    constexpr int kExpectedVlh = 8;
    constexpr int kSpmKcFactor = 1;
#elif defined(VL_2)
    constexpr int kExpectedVlh = 16;
    constexpr int kSpmKcFactor = 1;
#elif defined(VL_4)
    constexpr int kExpectedVlh = 32;
    constexpr int kSpmKcFactor = 1;
#elif defined(VL_8)
    constexpr int kExpectedVlh = 64;
    constexpr int kSpmKcFactor = 2;
#elif defined(VL_16)
    constexpr int kExpectedVlh = 128;
    constexpr int kSpmKcFactor = 3;
#else
#error "Unsupported SPM VL"
#endif
    if (runtime_vlh != kExpectedVlh) {
        std::fprintf(stderr, "SPM binary was built for svcnth=%d, got svcnth=%d\n",
                     kExpectedVlh, runtime_vlh);
        return 2;
    }
    if (static_cast<std::size_t>(opt.kc) * kSpmKcFactor > 1024) {
        std::fprintf(stderr, "kc=%d exceeds SPM set capacity for this VL path\n", opt.kc);
        return 2;
    }
    if (opt.grid_cols_override > panels) {
        std::fprintf(stderr, "grid-cols=%d exceeds N-panel count=%d\n", opt.grid_cols_override, panels);
        return 2;
    }
    Shared sh{opt, Halfs(static_cast<std::size_t>(opt.m) * opt.k),
              Halfs(static_cast<std::size_t>(opt.k) * opt.n), {}, opt.warmup + opt.repeat,
              std::vector<Tile>(static_cast<std::size_t>(opt.threads))};
    init_tensor(sh.a.ptr, sh.a.count, 0.25f);
    init_tensor(sh.b.ptr, sh.b.count, 0.25f);
    if (pthread_barrier_init(&sh.barrier, nullptr, static_cast<unsigned>(opt.threads))) return 1;
    std::vector<pthread_t> threads(static_cast<std::size_t>(opt.threads - 1));
    std::vector<WorkerArgs> args(static_cast<std::size_t>(opt.threads));
    for (int tid = 1; tid < opt.threads; ++tid) {
        args[tid] = WorkerArgs{&sh, tid};
        if (pthread_create(&threads[tid - 1], nullptr, worker_entry, &args[tid])) return 1;
    }
    if (opt.pin) pin_worker(0);
    prepare_tile(sh, 0);
    for (int rep = 0; rep < sh.total_reps; ++rep) {
        if (rep == opt.warmup) {
            pthread_barrier_wait(&sh.barrier);
            roi_begin();
            pthread_barrier_wait(&sh.barrier);
        } else {
            pthread_barrier_wait(&sh.barrier);
        }
        run_tile(sh, sh.tiles[0]);
        pthread_barrier_wait(&sh.barrier);
    }
    roi_end();
    for (pthread_t t : threads) pthread_join(t, nullptr);
    pthread_barrier_destroy(&sh.barrier);
    int bad = opt.verify ? verify_samples(sh) : 0;
    int PC = opt.grid_cols_override > 0 ? opt.grid_cols_override : grid_cols(opt.threads, panels);
    std::printf("gemm_acl_fp16_spm_mc threads=%d grid=%dx%d mc=%d kc=%d nt=%zu m=%d k=%d n=%d verify=%s checksum=%.9f\n",
                opt.threads, opt.threads / PC, PC, opt.mc, opt.kc, 3 * svcnth(), opt.m, opt.k, opt.n,
                opt.verify == 0 ? "off" : (bad == 0 ? "pass" : "FAIL"), checksum(sh));
    return bad == 0 ? 0 : 1;
}
