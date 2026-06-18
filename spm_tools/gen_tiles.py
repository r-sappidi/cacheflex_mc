#!/usr/bin/env python3
"""Generate VL=16 GEMM microkernels for arbitrary register-tile shapes (mr x nB)
so tile size can be swept. Emits a complete .cpp per (variant, mr, nB):

    variant = "spm"  -> B sourced via spm.ld1w (CacheFlex scratchpad)
    variant = "blk"  -> B packed into a contiguous per-thread panel, then
                        sourced via coherent ld1w

Register map (VL=16, one vector = 64 fp32, nc = nB*64 columns):
    C accumulators z[0 .. mr*nB-1]     (row r, col-half c -> z[r*nB+c])
    B vectors      z[mr*nB .. +nB-1]
    A broadcasts   z[mr*nB+nB .. +mr-1]
Requires mr*nB + nB + mr <= 32.

Parallelization, per-panel staging/packing, 2D PRxPC grid, and harness flags
match gemm_spm_opt_mc2.cpp; only the microkernel tile shape changes.
"""
import sys


def acc(r, c, nb):
    return r * nb + c


def bvec(c, mr, nb):
    return mr * nb + c


def areg(r, mr, nb):
    return mr * nb + nb + r


def gen_kernel(name, mr, nb, variant):
    """Return the C++ for one mr x nb inline-asm microkernel."""
    cargs = ", ".join(f"float *c{r}" for r in range(mr))
    aargs = ", ".join(f"const float *a{r}" for r in range(mr))
    if variant == "spm":
        extra_params = "std::uint64_t spm, std::uint64_t kc"
        bptr = "spm"
    else:
        extra_params = "const float *b, std::uint64_t ldb, std::uint64_t kc"
        bptr = "b"

    L = ['    asm volatile(', '        "ptrue p7.s\\n"']
    # C load
    for r in range(mr):
        for c in range(nb):
            z = acc(r, c, nb)
            off = "" if c == 0 else f", #{c}, mul vl"
            L.append(f'        "ld1w {{z{z}.s}}, p7/z, [%[c{r}]{off}]\\n"')
    L.append('        "1:\\n"')
    # B loads
    if variant == "spm":
        for c in range(nb):
            L.append(f'        "spm.ld1w z{bvec(c,mr,nb)}.s, p7/z, [%[b]]\\n"')
            L.append('        "add %[b], %[b], #256\\n"')
    else:
        for c in range(nb):
            off = "" if c == 0 else f", #{c}, mul vl"
            L.append(f'        "ld1w {{z{bvec(c,mr,nb)}.s}}, p7/z, [%[b]{off}]\\n"')
        L.append('        "add %[b], %[b], %[ldb]\\n"')
    # A broadcasts
    for r in range(mr):
        L.append(f'        "ld1rw {{z{areg(r,mr,nb)}.s}}, p7/z, [%[a{r}]]\\n"')
        L.append(f'        "add %[a{r}], %[a{r}], #4\\n"')
    # FMAs
    for r in range(mr):
        for c in range(nb):
            L.append(f'        "fmla z{acc(r,c,nb)}.s, p7/m, '
                     f'z{bvec(c,mr,nb)}.s, z{areg(r,mr,nb)}.s\\n"')
    L.append('        "subs %[k], %[k], #1\\n"')
    L.append('        "b.ne 1b\\n"')
    # C store
    for r in range(mr):
        for c in range(nb):
            z = acc(r, c, nb)
            off = "" if c == 0 else f", #{c}, mul vl"
            L.append(f'        "st1w {{z{z}.s}}, p7, [%[c{r}]{off}]\\n"')
    # operands
    aout = ", ".join(f'[a{r}] "+r"(a{r})' for r in range(mr))
    outs = f'[b] "+r"({bptr}), {aout}, [k] "+r"(kc)'
    cin = ", ".join(f'[c{r}] "r"(c{r})' for r in range(mr))
    ins = cin + ('' if variant == "spm" else ', [ldb] "r"(ldb)')
    maxz = areg(mr - 1, mr, nb)
    zclob = ", ".join(f'"z{i}"' for i in range(maxz + 1))
    L.append(f'        : {outs}')
    L.append(f'        : {ins}')
    L.append(f'        : "p7", {zclob}, "cc", "memory");')

    body = "\n".join(L)
    return (f"static inline void\n{name}({cargs}, {aargs}, {extra_params})\n{{\n"
            f"{body}\n}}\n")


CARGS_DECL = lambda mr: ", ".join(f"cp[{r}]" for r in range(mr))
AARGS_DECL = lambda mr: ", ".join(f"ap[{r}]" for r in range(mr))


def gen_cpp(variant, mr, nb):
    nc = nb * 64
    native = "GEMM_SPM_NATIVE" if variant == "spm" else "GEMM_BLK_NATIVE"
    tag = f"gemm_{variant}_t{mr}x{nb}"
    full = gen_kernel("kern_full", mr, nb, variant)
    tail = gen_kernel("kern_tail", 1, nb, variant)

    if variant == "spm":
        kern_call = (f"kern_full({CARGS_DECL(mr)}, {AARGS_DECL(mr)}, spm_base, "
                     f"static_cast<std::uint64_t>(opt.kc));")
        kern_tail_call = ("kern_tail(c0, a0, spm_base, "
                          "static_cast<std::uint64_t>(opt.kc));")
        stage = """            spm_dsb();
            for (int p = 0; p < opt.kc; ++p) {
                const float *src = B + static_cast<std::size_t>(pc + p) * N + jc;
                const std::uint64_t dst =
                    spm_base + static_cast<std::uint64_t>(p) * kVecsPerRow * 64;
                for (int l = 0; l < kVecsPerRow; ++l) {
                    spmcp64(dst + static_cast<std::uint64_t>(l) * 64, src + l * 16);
                }
            }
            spm_dsb();"""
        bsetup = ("const std::uint64_t spm_base = kSpmWindowBase +\n"
                  "        (static_cast<std::uint64_t>(opt.spm_way0) << 16) +\n"
                  "        (static_cast<std::uint64_t>(opt.spm_base_set) << 6);")
        bpanel = ""
    else:
        kern_call = (f"kern_full({CARGS_DECL(mr)}, {AARGS_DECL(mr)}, bpanel, ldb, "
                     f"static_cast<std::uint64_t>(opt.kc));")
        kern_tail_call = ("kern_tail(c0, a0, bpanel, ldb, "
                          "static_cast<std::uint64_t>(opt.kc));")
        stage = """            float *pack = ensure_bpack(static_cast<std::size_t>(opt.kc) * kNc);
            for (int p = 0; p < opt.kc; ++p) {
                const float *src = B + static_cast<std::size_t>(pc + p) * N + jc;
                std::copy(src, src + kNc, pack + static_cast<std::size_t>(p) * kNc);
            }
            const float *bpanel = pack;
            const std::uint64_t ldb = static_cast<std::uint64_t>(kNc) * sizeof(float);"""
        bsetup = ""
        bpanel = ""

    spm_fields = ("    int spm_way0 = 0;\n    int spm_base_set = 0;\n"
                  if variant == "spm" else "")
    spm_parse = ('''        else if (arg == "--spm-way0") opt.spm_way0 = parse_int(nv("s"), "spm-way0", 0);
        else if (arg == "--spm-base-set") opt.spm_base_set = parse_int(nv("s"), "spm-base-set", 0);'''
                 if variant == "spm" else "")
    spm_dsb_fn = ('static inline void spm_dsb() { asm volatile("dsb sy" ::: "memory"); }\n'
                  'static inline void spmcp64(std::uint64_t d, const void *s) {\n'
                  '    asm volatile("SPMCP_64_IMM %0, [%1, #0]\\n" : : "r"(d), "r"(s) : "memory");\n'
                  '}\n' if variant == "spm" else "")
    bpack_fn = ('''static float *ensure_bpack(std::size_t count) {
    static thread_local float *buf = nullptr;
    static thread_local std::size_t cap = 0;
    if (cap < count) {
        std::free(buf);
        void *raw = nullptr;
        if (posix_memalign(&raw, 64, count * sizeof(float)) != 0) std::exit(1);
        buf = static_cast<float *>(raw);
        cap = count;
    }
    return buf;
}
''' if variant == "blk" else "")

    return TEMPLATE.format(
        native=native, tag=tag, nc=nc, mr=mr, nb=nb, maxz=areg(mr - 1, mr, nb),
        full=full, tail=tail, kern_call=kern_call, kern_tail_call=kern_tail_call,
        stage=stage, bsetup=bsetup, bpanel=bpanel, spm_fields=spm_fields,
        spm_parse=spm_parse, spm_dsb_fn=spm_dsb_fn, bpack_fn=bpack_fn,
        cap_check=("    const long lines = static_cast<long>(opt.kc) * kVecsPerRow;\n"
                   "    if (opt.spm_way0 * 1024L + opt.spm_base_set + lines > 6 * 1024L) {\n"
                   '        std::fprintf(stderr, "panel exceeds SPM ways 0..5\\n"); std::exit(2);\n'
                   "    }\n" if variant == "spm" else ""))


TEMPLATE = r'''#include <algorithm>
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
#define {native} 1
#endif
#if defined(GEM5)
#include <gem5/m5ops.h>
#endif
namespace {{
constexpr std::uint64_t kSpmWindowBase = 0x40000000ull;
constexpr int kNc = {nc};
constexpr int kMr = {mr};
constexpr int kVecsPerRow = kNc / 16;
constexpr int kSpmWords = 64;
struct Options {{
    int threads = 1, m = 512, k = 768, n = 768, kc = 128, nc = kNc;
    int warmup = 1, repeat = 1, verify = 16, pin = 1;
{spm_fields}    int grid_cols_override = 0;
}};
struct AlignedFloats {{
    float *ptr = nullptr; std::size_t count = 0;
    explicit AlignedFloats(std::size_t n = 0) : count(n) {{
        if (n == 0) return;
        void *raw = nullptr;
        if (posix_memalign(&raw, 64, n * sizeof(float)) != 0) std::exit(1);
        ptr = static_cast<float *>(raw);
    }}
    ~AlignedFloats() {{ std::free(ptr); }}
    AlignedFloats(const AlignedFloats &) = delete;
    AlignedFloats &operator=(const AlignedFloats &) = delete;
}};
struct Shared {{ Options opt; AlignedFloats a, b, c; pthread_barrier_t barrier; int total_reps = 0; }};
struct WorkerArgs {{ Shared *shared = nullptr; int tid = 0; }};
static int parse_int(const char *s, const char *nm, long mn) {{
    char *e = nullptr; errno = 0; long v = std::strtol(s, &e, 10);
    if (errno || e == s || *e || v < mn || v > INT32_MAX) {{ std::fprintf(stderr, "bad %s\n", nm); std::exit(2); }}
    return (int)v;
}}
static Options parse_args(int argc, char **argv) {{
    Options opt;
    for (int i = 1; i < argc; ++i) {{
        const std::string arg(argv[i]);
        auto nv = [&](const char *nm) -> const char * {{ if (i + 1 >= argc) std::exit(2); return argv[++i]; }};
        if (arg == "--threads") opt.threads = parse_int(nv("t"), "threads", 1);
        else if (arg == "--m") opt.m = parse_int(nv("m"), "m", 1);
        else if (arg == "--k") opt.k = parse_int(nv("k"), "k", 1);
        else if (arg == "--n") opt.n = parse_int(nv("n"), "n", 1);
        else if (arg == "--kc") opt.kc = parse_int(nv("kc"), "kc", 1);
        else if (arg == "--nc") opt.nc = parse_int(nv("nc"), "nc", 1);
        else if (arg == "--warmup") opt.warmup = parse_int(nv("w"), "warmup", 0);
        else if (arg == "--repeat") opt.repeat = parse_int(nv("r"), "repeat", 1);
        else if (arg == "--verify") opt.verify = parse_int(nv("v"), "verify", 0);
        else if (arg == "--pin") opt.pin = parse_int(nv("p"), "pin", 0) != 0;
        else if (arg == "--grid-cols") opt.grid_cols_override = parse_int(nv("g"), "grid-cols", 0);
{spm_parse}
        else if (arg == "--help") std::exit(0);
        else {{ std::fprintf(stderr, "unknown %s\n", arg.c_str()); std::exit(2); }}
    }}
    if (opt.nc != kNc) {{ std::fprintf(stderr, "nc must be %d\n", kNc); std::exit(2); }}
    if (opt.n % kNc != 0 || opt.k % opt.kc != 0) {{ std::fprintf(stderr, "divisibility\n"); std::exit(2); }}
    if (opt.grid_cols_override > 0 && opt.threads % opt.grid_cols_override != 0) std::exit(2);
{cap_check}    return opt;
}}
static void init_tensor(float *d, std::size_t n, float s) {{
    for (std::size_t i = 0; i < n; ++i) {{
        std::uint32_t x = (std::uint32_t)((i * 1103515245u + 12345u) >> 8);
        d[i] = s * (float)((int)(x % 2048) - 1024) / 1024.0f;
    }}
}}
static void pin_worker(int tid) {{
#if defined(__linux__)
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(tid, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)tid;
#endif
}}
static void split_range(int count, int parts, int idx, int *lo, int *hi) {{
    int base = count / parts, rem = count % parts;
    *lo = idx * base + std::min(idx, rem); *hi = *lo + base + (idx < rem ? 1 : 0);
}}
static int grid_cols(int threads, int panels) {{
    for (int pc = threads; pc >= 1; --pc) if (threads % pc == 0 && panels % pc == 0) return pc;
    return 1;
}}
static void thread_tile(const Shared &sh, int tid, int *r0, int *r1, int *c0, int *c1) {{
    int panels = sh.opt.n / kNc;
    int PC = sh.opt.grid_cols_override > 0 ? sh.opt.grid_cols_override : grid_cols(sh.opt.threads, panels);
    int PR = sh.opt.threads / PC, pr = tid / PC, pc = tid % PC;
    split_range(sh.opt.m, PR, pr, r0, r1);
    int p0, p1; split_range(panels, PC, pc, &p0, &p1);
    *c0 = p0 * kNc; *c1 = p1 * kNc;
}}
#if defined({native})
{spm_dsb_fn}{bpack_fn}{full}
{tail}
#endif
static void run_rows(const Shared &sh, int row0, int row1, int col0, int col1) {{
    const Options &opt = sh.opt;
    const float *A = sh.a.ptr, *B = sh.b.ptr; float *C = sh.c.ptr;
    const int K = opt.k, N = opt.n;
    {bsetup}
    for (int jc = col0; jc < col1; jc += kNc) {{
        for (int pc = 0; pc < K; pc += opt.kc) {{
#if defined({native})
            {bpanel}
{stage}
            int i = row0;
            for (; i + kMr <= row1; i += kMr) {{
                float *cp[kMr]; const float *ap[kMr];
                for (int r = 0; r < kMr; ++r) {{
                    cp[r] = C + (std::size_t)(i + r) * N + jc;
                    ap[r] = A + (std::size_t)(i + r) * K + pc;
                }}
                {kern_call}
            }}
            for (; i < row1; ++i) {{
                float *c0 = C + (std::size_t)i * N + jc;
                const float *a0 = A + (std::size_t)i * K + pc;
                {kern_tail_call}
            }}
#else
            for (int i = row0; i < row1; ++i)
                for (int j = jc; j < jc + kNc; ++j) {{
                    float acc = C[(std::size_t)i * N + j];
                    for (int p = pc; p < pc + opt.kc; ++p)
                        acc += A[(std::size_t)i * K + p] * B[(std::size_t)p * N + j];
                    C[(std::size_t)i * N + j] = acc;
                }}
#endif
        }}
    }}
}}
static void *worker_entry(void *raw) {{
    WorkerArgs *args = (WorkerArgs *)raw; Shared &sh = *args->shared;
    if (sh.opt.pin) pin_worker(args->tid);
    int r0, r1, c0, c1; thread_tile(sh, args->tid, &r0, &r1, &c0, &c1);
    for (int rep = 0; rep < sh.total_reps; ++rep) {{
        pthread_barrier_wait(&sh.barrier); run_rows(sh, r0, r1, c0, c1);
        pthread_barrier_wait(&sh.barrier);
    }}
    return nullptr;
}}
static double checksum(const float *d, std::size_t n) {{ double s = 0; for (std::size_t i = 0; i < n; i += 97) s += d[i]; return s; }}
static int verify_samples(const Shared &sh) {{
    const Options &opt = sh.opt; int bad = 0;
    for (int s = 0; s < opt.verify; ++s) {{
        int i = (s * 131 + 7) % opt.m, j = (s * 197 + 3) % opt.n; double ref = 0;
        for (int p = 0; p < opt.k; ++p)
            ref += (double)sh.a.ptr[(std::size_t)i * opt.k + p] * (double)sh.b.ptr[(std::size_t)p * opt.n + j];
        ref *= sh.total_reps;
        double got = sh.c.ptr[(std::size_t)i * opt.n + j], tol = 1e-3 * std::max(1.0, std::fabs(ref));
        if (std::fabs(got - ref) > tol) {{ std::fprintf(stderr, "mismatch %d %d %f %f\n", i, j, got, ref); ++bad; }}
    }}
    return bad;
}}
static void roi_begin() {{
#if defined(GEM5)
    m5_reset_stats(0, 0); m5_work_begin(0, 0);
#endif
}}
static void roi_end() {{
#if defined(GEM5)
    m5_work_end(0, 0); m5_dump_stats(0, 0);
#endif
}}
}}
int main(int argc, char **argv) {{
    Options opt = parse_args(argc, argv);
#if defined({native})
    if (svcntw() != kSpmWords) {{ std::fprintf(stderr, "need VL=2048b\n"); return 1; }}
#endif
    Shared sh{{opt, AlignedFloats((std::size_t)opt.m * opt.k), AlignedFloats((std::size_t)opt.k * opt.n),
              AlignedFloats((std::size_t)opt.m * opt.n), {{}}, opt.warmup + opt.repeat}};
    init_tensor(sh.a.ptr, sh.a.count, 0.25f); init_tensor(sh.b.ptr, sh.b.count, 0.25f);
    std::fill(sh.c.ptr, sh.c.ptr + sh.c.count, 0.0f);
    if (pthread_barrier_init(&sh.barrier, nullptr, (unsigned)opt.threads)) return 1;
    std::vector<pthread_t> threads((std::size_t)opt.threads - 1);
    std::vector<WorkerArgs> args((std::size_t)opt.threads);
    for (int tid = 1; tid < opt.threads; ++tid) {{
        args[tid] = WorkerArgs{{&sh, tid}};
        if (pthread_create(&threads[tid - 1], nullptr, worker_entry, &args[tid])) return 1;
    }}
    if (opt.pin) pin_worker(0);
    int r0, r1, c0, c1; thread_tile(sh, 0, &r0, &r1, &c0, &c1);
    for (int rep = 0; rep < sh.total_reps; ++rep) {{
        if (rep == opt.warmup) roi_begin();
        pthread_barrier_wait(&sh.barrier); run_rows(sh, r0, r1, c0, c1);
        pthread_barrier_wait(&sh.barrier);
    }}
    roi_end();
    for (pthread_t t : threads) pthread_join(t, nullptr);
    pthread_barrier_destroy(&sh.barrier);
    int bad = opt.verify > 0 ? verify_samples(sh) : 0;
    double ck = checksum(sh.c.ptr, sh.c.count);
    int panels = opt.n / kNc;
    int PC = opt.grid_cols_override > 0 ? opt.grid_cols_override : grid_cols(opt.threads, panels);
    std::printf("{tag} threads=%d grid=%dx%d mr=%d nc=%d kc=%d m=%d k=%d n=%d verify=%s checksum=%.9f\n",
                opt.threads, opt.threads / PC, PC, kMr, opt.nc, opt.kc, opt.m, opt.k, opt.n,
                opt.verify == 0 ? "off" : (bad == 0 ? "pass" : "FAIL"), ck);
    return bad == 0 ? 0 : 1;
}}
'''

if __name__ == "__main__":
    variant, mr, nb = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    assert mr * nb + nb + mr <= 32, "tile exceeds 32 z-registers"
    sys.stdout.write(gen_cpp(variant, mr, nb))
