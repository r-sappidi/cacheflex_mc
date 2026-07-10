/* 2D Jacobi 5-point stencil with three halo-exchange strategies.
 *
 * The grid (rows x cols doubles) is split into row bands, one per thread.
 * Each iteration every band needs its neighbors' boundary rows.  One
 * pthread barrier per iteration in every mode; the modes differ only in how
 * the halo rows travel:
 *
 *   --mode=cache  Neighbor rows are read directly from the shared arrays;
 *                 the coherence protocol moves the lines (producer's rows
 *                 are downgraded by the consumer's reads each iteration and
 *                 re-upgraded by the next write).
 *   --mode=pull   After the barrier, each core SPMCP-fetches the neighbor
 *                 boundary rows into its own SPM (remote dirty lines ->
 *                 GETS_SILENT snapshot), then computes.  The fetch is on the
 *                 critical path: it cannot start before the barrier because
 *                 the data does not exist earlier.
 *   --mode=push   Each core computes its OWN boundary rows first, stages
 *                 them into outbound SPM slots (ld1d + spm.st1d), fires one
 *                 spmho per line into the neighbor's posted halo buffer,
 *                 and only then computes the interior rows -- the handoffs
 *                 complete under the interior compute, so by the barrier
 *                 the halos for the next iteration are already resident in
 *                 the consumer's SPM.  Receive buffers ping-pong by
 *                 iteration parity, so a fast producer can never overwrite
 *                 a buffer its consumer is still reading.
 *
 * All three modes execute the identical row kernel in the identical order,
 * so checksums must match bit-exactly.
 *
 * SPM slot map (each core's private SPM, 4 ways x sets):
 *   inbound  up-halo  parity p: way p,     sets [0, L)
 *   inbound  dn-halo  parity p: way 2 + p, sets [0, L)
 *   outbound row lo:            way 0,     sets [512, 512 + L)
 *   outbound row hi-1:          way 1,     sets [512, 512 + L)
 * with L = cols/8 lines per halo row.
 */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdatomic.h>

#include <gem5/m5ops.h>

#define SPM_ADDR_BASE (1ULL << 40)

static inline uint64_t spm_addr(unsigned way, unsigned set, unsigned offset)
{
    return SPM_ADDR_BASE | ((uint64_t)way << 16) | ((uint64_t)set << 6) | offset;
}

static inline uint64_t spm_ho_desc(unsigned core, uint64_t slot)
{
    return slot | ((uint64_t)core << 32);
}

static inline void spmcp64(uint64_t dst_spm, const void *src)
{
    asm volatile("SPMCP_64_IMM %0, [%1, #0]\n" :: "r"(dst_spm), "r"(src)
                 : "memory");
}

static inline void spmho64(uint64_t dst_desc, uint64_t src_spm)
{
    asm volatile("SPMHO_64_IMM %0, [%1, #0]\n" :: "r"(dst_desc), "r"(src_spm)
                 : "memory");
}

static inline void spm_release(uint64_t slot)
{
    asm volatile("SPMREL_8_IMM xzr, [%0, #0]\n" :: "r"(slot) : "memory");
}

static inline void spm_load64_to_mem(void *dst, uint64_t src_spm)
{
    asm volatile(
        "mov x9, #8\n"
        "whilelt p0.d, xzr, x9\n"
        "spm.ld1d z0.d, p0/z, [%1]\n"
        "st1d z0.d, p0, [%0]\n"
        :: "r"(dst), "r"(src_spm) : "x9", "p0", "z0", "memory");
}

static inline void spm_store64_from_mem(uint64_t dst_spm, const void *src)
{
    asm volatile(
        "mov x9, #8\n"
        "whilelt p0.d, xzr, x9\n"
        "ld1d z0.d, p0/z, [%1]\n"
        "spm.st1d z0.d, p0, [%0]\n"
        :: "r"(dst_spm), "r"(src) : "x9", "p0", "z0", "memory");
}

#define DSB asm volatile("dsb sy" ::: "memory")

enum { HALO_IN_BASE_SET = 0, HALO_OUT_BASE_SET = 512 };

static inline uint64_t in_slot(int dir /*0=up,1=dn*/, int parity, int j)
{
    return spm_addr((unsigned)(2 * dir + parity),
                    (unsigned)(HALO_IN_BASE_SET + j), 0);
}

static inline uint64_t out_slot(int side /*0=lo,1=hi*/, int j)
{
    return spm_addr((unsigned)side, (unsigned)(HALO_OUT_BASE_SET + j), 0);
}

/* Point-to-point sync slots (push mode): a producer pushes a FLAG line after
 * its halo data is acked (data-ready), and a consumer pushes a CREDIT line
 * after unloading a parity buffer (buffer-free).  Both are polled with plain
 * local SPM loads -- no NoC traffic while spinning, no global barrier. */
enum { SYNC_BASE_SET = 896 };
static inline uint64_t in_flag(int dir, int parity)
{ return spm_addr((unsigned)dir, (unsigned)(SYNC_BASE_SET + parity), 0); }
static inline uint64_t in_cr(int dir, int parity)
{ return spm_addr((unsigned)(2 + dir), (unsigned)(SYNC_BASE_SET + parity), 0); }
static inline uint64_t out_flag(int dir)
{ return spm_addr((unsigned)dir, (unsigned)(SYNC_BASE_SET + 2), 0); }
static inline uint64_t out_cr(int dir)
{ return spm_addr((unsigned)(2 + dir), (unsigned)(SYNC_BASE_SET + 2), 0); }

static inline double spm_lane0(uint64_t slot, double *tmp)
{
    spm_load64_to_mem(tmp, slot);
    return tmp[0];
}

enum { MODE_CACHE = 0, MODE_PULL = 1, MODE_PUSH = 2 };

/* One Jacobi row: d[c] = 0.25*(up[c] + dn[c] + mid[c-1] + mid[c+1]).
 * Edge columns are Dirichlet: copied through unchanged.  Shared verbatim by
 * every mode so the arithmetic order is identical. */
static void jrow(double *restrict d, const double *restrict up,
                 const double *restrict mid, const double *restrict dn, int n)
{
    d[0] = mid[0];
    for (int c = 1; c < n - 1; ++c) {
        d[c] = 0.25 * (up[c] + dn[c] + mid[c - 1] + mid[c + 1]);
    }
    d[n - 1] = mid[n - 1];
}

/* Sense-reversing central spin barrier: the iteration-loop barrier for the
 * cache/pull modes, so they are not handicapped by the futex-based pthread
 * barrier (setup still uses the pthread one). */
typedef struct {
    atomic_int count;
    atomic_int gen;
    int nthreads;
} spin_barrier_t;

static void spin_barrier_wait(spin_barrier_t *b)
{
    int g = atomic_load_explicit(&b->gen, memory_order_acquire);
    if (atomic_fetch_add_explicit(&b->count, 1, memory_order_acq_rel) ==
        b->nthreads - 1) {
        atomic_store_explicit(&b->count, 0, memory_order_relaxed);
        atomic_store_explicit(&b->gen, g + 1, memory_order_release);
    } else {
        while (atomic_load_explicit(&b->gen, memory_order_acquire) == g) {}
    }
}

typedef struct {
    int tid, threads, rows, cols, iters, mode, pin, profile, spin_bar;
    double *bufa, *bufb;
    double *up_buf, *dn_buf;     /* per-thread coherent halo landing pads */
    const double *scratch;       /* distinct lines for posting inbound slots */
    pthread_barrier_t *barrier;
    spin_barrier_t *sbar;
} warg_t;

static void maybe_pin(int tid, int pin)
{
    if (!pin) return;
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(tid, &s);
    sched_setaffinity(0, sizeof(s), &s);   /* ignored in gem5 SE; tid==cpu by
                                            * creation order (main = worker 0) */
}

__attribute__((noinline))
static void band_init(double *bufa, double *bufb, int lo, int hi, int cols)
{
    for (int r = lo; r < hi; ++r) {
        for (int c = 0; c < cols; ++c) {
            double v = sin(0.7 * r) + cos(1.3 * c) + 0.001 * r * c;
            bufa[(size_t)r * cols + c] = v;
            bufb[(size_t)r * cols + c] = v;
        }
    }
}

static void *worker(void *opaque)
{
    warg_t *a = (warg_t *)opaque;
    const int tid = a->tid, threads = a->threads;
    const int rows = a->rows, cols = a->cols, L = a->cols / 8;
    const int bh = rows / threads;
    const int lo = tid * bh, hi = lo + bh;
    const int has_up = tid > 0, has_dn = tid < threads - 1;
    double *src = a->bufa, *dst = a->bufb;
    const double *my_scratch = a->scratch + (size_t)tid * 4 * L * 8;

    maybe_pin(tid, a->pin);

    /* Grid init is SERIAL (in main, before threads): gem5 SE assigns
     * physical pages in fault order, so parallel first-touch scatters a
     * band's pages across page colors and manufactures L1 set-conflict
     * misses (measured 2.6x on the pure-cache mode).  Real OSes color
     * pages; SE does not.
     * Warm my scratch slice so posting sources are cache-resident. */
    memset((void *)(uintptr_t)my_scratch, 1, (size_t)4 * L * 64);
    pthread_barrier_wait(a->barrier);      /* everyone initialized + pinned */

    if (tid == 0) {
        m5_reset_stats(0, 0);
        m5_work_begin(0, 0);
    }
    pthread_barrier_wait(a->barrier);      /* ---- ROI begins ---- */

    /* ------- mode-specific one-time setup (inside ROI: it is real cost) */
    if (a->mode == MODE_PUSH) {
        /* Post the four inbound halo buffers from distinct scratch lines
         * (concurrent same-line SPMCPs are merged by the sequencer). */
        for (int b = 0; b < 4; ++b) {
            for (int j = 0; j < L; ++j) {
                spmcp64(spm_addr((unsigned)b, (unsigned)(HALO_IN_BASE_SET + j), 0),
                        my_scratch + ((size_t)b * L + j) * 8);
            }
        }
        /* Post the outbound slots from my initial boundary rows: this both
         * installs the slots and stages the iteration-0 halo content. */
        if (has_up) {
            for (int j = 0; j < L; ++j) {
                spmcp64(out_slot(0, j), src + (size_t)lo * cols + 8 * j);
            }
        }
        if (has_dn) {
            for (int j = 0; j < L; ++j) {
                spmcp64(out_slot(1, j), src + (size_t)(hi - 1) * cols + 8 * j);
            }
        }
        /* Post the sync slots (flags + credits, in and out) from distinct
         * warm lines. */
        for (int d = 0; d < 2; ++d) {
            for (int q2 = 0; q2 < 2; ++q2) {
                spmcp64(in_flag(d, q2), a->up_buf + (size_t)(4 * d + q2) * 8);
                spmcp64(in_cr(d, q2),
                        a->up_buf + (size_t)(8 + 4 * d + q2) * 8);
            }
            spmcp64(out_flag(d), a->dn_buf + (size_t)d * 8);
            spmcp64(out_cr(d), a->dn_buf + (size_t)(2 + d) * 8);
        }
        DSB;
        pthread_barrier_wait(a->barrier);  /* all slots posted everywhere */
        /* Seed parity-0 halos with the initial boundary rows, then the
         * data-ready flags (tag 1.0 = "consumable at iteration 0").  From
         * here on the flags/credits carry ALL synchronization. */
        if (has_up) {
            for (int j = 0; j < L; ++j) {
                spmho64(spm_ho_desc((unsigned)(tid - 1), in_slot(1, 0, j)),
                        out_slot(0, j));
            }
        }
        if (has_dn) {
            for (int j = 0; j < L; ++j) {
                spmho64(spm_ho_desc((unsigned)(tid + 1), in_slot(0, 0, j)),
                        out_slot(1, j));
            }
        }
        DSB;                               /* seed data installed */
        {
            double tag1[8] __attribute__((aligned(64))) = {1.0};
            if (has_up) spm_store64_from_mem(out_flag(0), tag1);
            if (has_dn) spm_store64_from_mem(out_flag(1), tag1);
            DSB;
            if (has_up) {
                spmho64(spm_ho_desc((unsigned)(tid - 1), in_flag(1, 0)),
                        out_flag(0));
            }
            if (has_dn) {
                spmho64(spm_ho_desc((unsigned)(tid + 1), in_flag(0, 0)),
                        out_flag(1));
            }
        }
    }

    /* ------------------------------ iterations */
    if (a->mode == MODE_PUSH) {
        /* Barrier-free: flags gate consumption, credits gate production.
         * Neighbors couple with slack 1 (double buffer); skew diffuses
         * through the chain instead of globally synchronizing. */
        double tagbuf[8] __attribute__((aligned(64))) = {0};
        double pollbuf[8] __attribute__((aligned(64)));
        for (int it = 0; it < a->iters; ++it) {
            const int p = it & 1, q = p ^ 1;
            const int prof = a->profile && tid == 0 && it == 2;
            if (prof) m5_dump_stats(0, 0);       /* iter start */

            /* data-ready?  poll my own SPM (zero NoC traffic), unload */
            if (has_up) {
                while (spm_lane0(in_flag(0, p), pollbuf) < (double)(it + 1)) {}
                for (int j = 0; j < L; ++j) {
                    spm_load64_to_mem(a->up_buf + 8 * j, in_slot(0, p, j));
                }
            }
            if (has_dn) {
                while (spm_lane0(in_flag(1, p), pollbuf) < (double)(it + 1)) {}
                for (int j = 0; j < L; ++j) {
                    spm_load64_to_mem(a->dn_buf + 8 * j, in_slot(1, p, j));
                }
            }
            if (prof) m5_dump_stats(0, 0);       /* halos ready + unloaded */

            if (has_up) {
                jrow(dst + (size_t)lo * cols, a->up_buf,
                     src + (size_t)lo * cols, src + (size_t)(lo + 1) * cols,
                     cols);
            }
            if (has_dn) {
                jrow(dst + (size_t)(hi - 1) * cols,
                     src + (size_t)(hi - 2) * cols,
                     src + (size_t)(hi - 1) * cols, a->dn_buf, cols);
            }
            if (prof) m5_dump_stats(0, 0);       /* boundary rows done */

            /* stage credits (parity p is free) + boundary data */
            tagbuf[0] = (double)(it + 1);
            if (has_up) spm_store64_from_mem(out_cr(0), tagbuf);
            if (has_dn) spm_store64_from_mem(out_cr(1), tagbuf);
            if (has_up) {
                for (int j = 0; j < L; ++j) {
                    spm_store64_from_mem(out_slot(0, j),
                                         dst + (size_t)lo * cols + 8 * j);
                }
            }
            if (has_dn) {
                for (int j = 0; j < L; ++j) {
                    spm_store64_from_mem(out_slot(1, j),
                                         dst + (size_t)(hi - 1) * cols + 8 * j);
                }
            }
            /* buffer-free?  neighbor must have consumed parity q at it-1 */
            if (it >= 1) {
                if (has_up) {
                    while (spm_lane0(in_cr(0, q), pollbuf) < (double)it) {}
                }
                if (has_dn) {
                    while (spm_lane0(in_cr(1, q), pollbuf) < (double)it) {}
                }
            }
            DSB;                                 /* stages drained */
            if (has_up) {
                spmho64(spm_ho_desc((unsigned)(tid - 1), in_cr(1, p)),
                        out_cr(0));
                for (int j = 0; j < L; ++j) {
                    spmho64(spm_ho_desc((unsigned)(tid - 1),
                                        in_slot(1, q, j)), out_slot(0, j));
                }
            }
            if (has_dn) {
                spmho64(spm_ho_desc((unsigned)(tid + 1), in_cr(0, p)),
                        out_cr(1));
                for (int j = 0; j < L; ++j) {
                    spmho64(spm_ho_desc((unsigned)(tid + 1),
                                        in_slot(0, q, j)), out_slot(1, j));
                }
            }
            DSB;                                 /* data installed at dst */
            tagbuf[0] = (double)(it + 2);        /* consumable at it+1 */
            if (has_up) spm_store64_from_mem(out_flag(0), tagbuf);
            if (has_dn) spm_store64_from_mem(out_flag(1), tagbuf);
            DSB;                                 /* flag stage drained */
            if (has_up) {
                spmho64(spm_ho_desc((unsigned)(tid - 1), in_flag(1, q)),
                        out_flag(0));
            }
            if (has_dn) {
                spmho64(spm_ho_desc((unsigned)(tid + 1), in_flag(0, q)),
                        out_flag(1));
            }
            if (prof) m5_dump_stats(0, 0);       /* sends issued */

            /* interior; flag-HO acks absorbed here (slot reuse next iter is
             * protected by the XHO stall, so no trailing dsb) */
            {
                const int ir0 = has_up ? lo + 1 : (lo > 1 ? lo : 1);
                const int ir1 = has_dn ? hi - 1
                                       : (hi < rows - 1 ? hi : rows - 1);
                for (int r = ir0; r < ir1; ++r) {
                    jrow(dst + (size_t)r * cols, src + (size_t)(r - 1) * cols,
                         src + (size_t)r * cols, src + (size_t)(r + 1) * cols,
                         cols);
                }
            }
            if (prof) m5_dump_stats(0, 0);       /* interior done */
            double *t = src; src = dst; dst = t;
        }
        DSB;                                     /* last sends acked */
    } else {
    for (int it = 0; it < a->iters; ++it) {
        const int p = it & 1;
        const int prof = a->profile && tid == 0 && it == 2;
#define PMARK() do { if (prof) m5_dump_stats(0, 0); } while (0)
        PMARK();                          /* iter start */

        if (a->mode == MODE_PULL) {
            /* Fetch the halo rows now that the barrier says they exist.
             * This is the pull critical path push avoids. */
            if (has_up) {
                for (int j = 0; j < L; ++j) {
                    spmcp64(in_slot(0, 0, j),
                            src + (size_t)(lo - 1) * cols + 8 * j);
                }
            }
            if (has_dn) {
                for (int j = 0; j < L; ++j) {
                    spmcp64(in_slot(1, 0, j), src + (size_t)hi * cols + 8 * j);
                }
            }
            DSB;
        }
        PMARK();                          /* pull fetch done */

        /* Unload halos from SPM into the per-thread landing pads. */
        if (a->mode == MODE_PULL) {
            if (has_up) {
                for (int j = 0; j < L; ++j) {
                    spm_load64_to_mem(a->up_buf + 8 * j, in_slot(0, 0, j));
                }
            }
            if (has_dn) {
                for (int j = 0; j < L; ++j) {
                    spm_load64_to_mem(a->dn_buf + 8 * j, in_slot(1, 0, j));
                }
            }
        }
        PMARK();                          /* unload done */
        const double *up_row = (a->mode == MODE_CACHE)
            ? src + (size_t)(lo - 1) * cols : a->up_buf;
        const double *dn_row = (a->mode == MODE_CACHE)
            ? src + (size_t)hi * cols : a->dn_buf;

        if (has_up) {
            jrow(dst + (size_t)lo * cols, up_row, src + (size_t)lo * cols,
                 src + (size_t)(lo + 1) * cols, cols);
        }
        if (has_dn) {
            jrow(dst + (size_t)(hi - 1) * cols, src + (size_t)(hi - 2) * cols,
                 src + (size_t)(hi - 1) * cols, dn_row, cols);
        }
        PMARK();                          /* boundary rows done */

        {
            const int ir0 = has_up ? lo + 1 : (lo > 1 ? lo : 1);
            const int ir1 = has_dn ? hi - 1 : (hi < rows - 1 ? hi : rows - 1);
            for (int r = ir0; r < ir1; ++r) {
                jrow(dst + (size_t)r * cols, src + (size_t)(r - 1) * cols,
                     src + (size_t)r * cols, src + (size_t)(r + 1) * cols,
                     cols);
            }
        }
        PMARK();                          /* interior done */
        if (a->spin_bar) {                /* iteration complete everywhere */
            spin_barrier_wait(a->sbar);
        } else {
            pthread_barrier_wait(a->barrier);
        }
        PMARK();                          /* barrier done */
        double *t = src; src = dst; dst = t;
    }
    }

    pthread_barrier_wait(a->barrier);
    if (tid == 0) {
        m5_work_end(0, 0);
        m5_dump_stats(0, 0);              /* ---- ROI ends ---- */
    }

    /* hygiene: release this core's slots (outside the ROI) */
    if (a->mode == MODE_PUSH) {
        for (int b = 0; b < 4; ++b) {
            for (int j = 0; j < L; ++j) {
                spm_release(spm_addr((unsigned)b,
                                     (unsigned)(HALO_IN_BASE_SET + j), 0));
            }
        }
        if (has_up) for (int j = 0; j < L; ++j) spm_release(out_slot(0, j));
        if (has_dn) for (int j = 0; j < L; ++j) spm_release(out_slot(1, j));
        for (int d = 0; d < 2; ++d) {
            for (int q2 = 0; q2 < 2; ++q2) {
                spm_release(in_flag(d, q2));
                spm_release(in_cr(d, q2));
            }
            spm_release(out_flag(d));
            spm_release(out_cr(d));
        }
        DSB;
    } else if (a->mode == MODE_PULL) {
        if (has_up) for (int j = 0; j < L; ++j) spm_release(in_slot(0, 0, j));
        if (has_dn) for (int j = 0; j < L; ++j) spm_release(in_slot(1, 0, j));
        DSB;
    }
    return NULL;
}

static int arg_int(int argc, char **argv, const char *key, int dflt)
{
    size_t klen = strlen(key);
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], key, klen) == 0 && argv[i][klen] == '=') {
            return atoi(argv[i] + klen + 1);
        }
    }
    return dflt;
}

static const char *arg_str(int argc, char **argv, const char *key,
                           const char *dflt)
{
    size_t klen = strlen(key);
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], key, klen) == 0 && argv[i][klen] == '=') {
            return argv[i] + klen + 1;
        }
    }
    return dflt;
}

static double checksum(const double *a, int rows, int cols)
{
    double s = 0.0;
    size_t total = (size_t)rows * cols;
    for (size_t i = 0; i < total; i += 257) {
        s += (i & 1) ? 0.25 * a[i] : 0.5 * a[i];
    }
    return s;
}

int main(int argc, char **argv)
{
    const int threads = arg_int(argc, argv, "--threads", 8);
    const int rows = arg_int(argc, argv, "--rows", 128);
    const int cols = arg_int(argc, argv, "--cols", 2048);
    const int iters = arg_int(argc, argv, "--iters", 24);
    const int pin = arg_int(argc, argv, "--pin", 1);
    const int profile = arg_int(argc, argv, "--profile", 0);
    const int spin_bar = arg_int(argc, argv, "--spin-barrier", 0);
    const char *mode_s = arg_str(argc, argv, "--mode", "cache");
    int mode = MODE_CACHE;
    if (strcmp(mode_s, "pull") == 0) mode = MODE_PULL;
    else if (strcmp(mode_s, "push") == 0) mode = MODE_PUSH;

    if (rows % threads != 0 || cols % 8 != 0 || rows / threads < 2) {
        fprintf(stderr, "need rows %% threads == 0, cols %% 8 == 0, "
                        "rows/threads >= 2\n");
        return 1;
    }
    const int L = cols / 8;
    if (HALO_OUT_BASE_SET + L > 1024) {
        fprintf(stderr, "cols too large for the slot map (L=%d)\n", L);
        return 1;
    }

    double *bufa, *bufb;
    if (posix_memalign((void **)&bufa, 64, (size_t)rows * cols * 8) ||
        posix_memalign((void **)&bufb, 64, (size_t)rows * cols * 8)) {
        return 1;
    }
    band_init(bufa, bufb, 0, rows, cols);
    double *scratch = calloc((size_t)threads * 4 * L * 8, 8);
    if (!scratch) return 1;

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, (unsigned)threads);
    static spin_barrier_t sbar;
    atomic_init(&sbar.count, 0);
    atomic_init(&sbar.gen, 0);
    sbar.nthreads = threads;

    warg_t args[64];
    pthread_t tids[64];
    for (int t = 0; t < threads; ++t) {
        args[t] = (warg_t){t, threads, rows, cols, iters, mode, pin, profile,
                           spin_bar, bufa, bufb, NULL, NULL, scratch, &barrier,
                           &sbar};
        posix_memalign((void **)&args[t].up_buf, 64, (size_t)cols * 8);
        posix_memalign((void **)&args[t].dn_buf, 64, (size_t)cols * 8);
        memset(args[t].up_buf, 0, (size_t)cols * 8);
        memset(args[t].dn_buf, 0, (size_t)cols * 8);
    }
    /* main runs worker 0 inline so worker tid == cpu id */
    for (int t = 1; t < threads; ++t) {
        pthread_create(&tids[t - 1], NULL, worker, &args[t]);
    }
    worker(&args[0]);
    for (int t = 1; t < threads; ++t) {
        pthread_join(tids[t - 1], NULL);
    }

    const double *final = (iters & 1) ? bufb : bufa;
    printf("stencil_halo mode=%s threads=%d rows=%d cols=%d iters=%d "
           "checksum=%.17g\n", mode_s, threads, rows, cols, iters,
           checksum(final, rows, cols));
    return 0;
}
