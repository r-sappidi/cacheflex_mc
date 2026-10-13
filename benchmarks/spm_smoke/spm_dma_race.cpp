#include "common_spm.hpp"

#include <cstring>

// DMA-vs-SPM race test (VER-05 runtime). Each core repeatedly copies its
// target lines into SPM (SPMCP), checks them, writes a new stamp through SPM
// and writes it back (SPMWB). A DMA traffic generator configured by the host
// reads and writes the same physical lines with full-line writes that fill
// every byte with its requestor id. A line must therefore always hold either
// this core's latest stamp or a uniform DMA pattern: an older stamp means a
// lost writeback, and a mixture means a torn line. The eviction sweep forces
// clean LLC replacements that race the directory's DMA invalidations
// (-DTARGETED_EVICT evicts each target line right after it is used).
//
// The target page is in .bss so SE mode allocates it at load time; its physical
// address is then fixed for a given binary and argument lengths.

constexpr int kMaxLines = 64;
alignas(4096) static volatile std::uint64_t target[kMaxLines * 8];
constexpr std::size_t kEvictWords = (512 * 1024) / 8;
alignas(4096) static volatile std::uint64_t evict[kEvictWords];

// Whole-line accesses to target lines are one 64-byte SVE access (VL4, line
// aligned), issued by gem5 as a single cache-line request. Eight scalar
// accesses could legally interleave with a DMA write and appear torn.
static inline void copy_line(volatile std::uint64_t *dst, const volatile std::uint64_t *src)
{
    asm volatile("ptrue p0.d\nld1d z0.d, p0/z, [%1]\nst1d z0.d, p0, [%0]\ndsb sy\n"
                 :: "r"(dst), "r"(src) : "p0", "z0", "memory");
}

static inline std::uint64_t stamp(unsigned core, unsigned line, unsigned iter, unsigned word)
{
    return 0xC0DE000000000000ull ^ (std::uint64_t(core) << 44) ^
           (std::uint64_t(line) << 32) ^ (std::uint64_t(iter) << 8) ^ word;
}

// 0: latest stamp, 1: uniform DMA pattern, -1: anything else.
static int classify(const volatile std::uint64_t *w, unsigned core, unsigned line, unsigned iter)
{
    bool ok = true;
    for (unsigned i = 0; i < 8; ++i) ok &= w[i] == stamp(core, line, iter, i);
    if (ok) return 0;
    unsigned char b[64];
    for (unsigned i = 0; i < 8; ++i) {
        std::uint64_t v = w[i];
        std::memcpy(b + 8 * i, &v, 8);
    }
    for (unsigned i = 1; i < 64; ++i) if (b[i] != b[0]) return -1;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        std::printf("usage: core iters lines evict_kb\n");
        return 2;
    }
    const unsigned core = std::atoi(argv[1]), iters = std::atoi(argv[2]);
    const unsigned lines = std::atoi(argv[3]), evict_words = std::atoi(argv[4]) * 1024 / 8;
    if (lines == 0 || lines > kMaxLines || evict_words > kEvictWords) return 2;
    std::printf("SPM_DMA_RACE core=%u target=%p lines=%u iters=%u\n", core, (void *)target, lines, iters);

    alignas(64) volatile std::uint64_t got[8], next[8];
    for (unsigned l = 0; l < lines; ++l) {
        for (unsigned i = 0; i < 8; ++i) next[i] = stamp(core, l, 0, i);
        copy_line(target + l * 8, next);
    }

    int errors = 0, printed = 0;
    unsigned long dma_copy = 0, dma_load = 0, sink = 0;
    auto report = [&](const char *what, unsigned it, unsigned l, const volatile std::uint64_t *w) {
        ++errors;
        if (printed++ < 8)
            std::printf("ERROR %s iter=%u line=%u w0=%#llx w7=%#llx\n", what, it, l,
                        (unsigned long long)w[0], (unsigned long long)w[7]);
    };
    for (unsigned it = 1; it <= iters; ++it) {
        for (unsigned l = 0; l < lines; ++l) {
            volatile std::uint64_t *line = target + l * 8;
            std::uint64_t slot = spm_addr(0, l);
            asm volatile("dsb sy\nSPMCP_64_IMM %0, [%1, #0]\ndsb sy\n"
                         "ptrue p0.d\nspm.ld1d z0.d, p0/z, [%0]\n"
                         "st1d z0.d, p0, [%2]\ndsb sy\n"
                         :: "r"(slot), "r"(line), "r"(got)
                         : "p0", "z0", "memory");
            int c = classify(got, core, l, it - 1);
            if (c < 0) report("spmcp", it, l, got);
            dma_copy += c == 1;
            for (unsigned i = 0; i < 8; ++i) next[i] = stamp(core, l, it, i);
            asm volatile("ptrue p0.d\nld1d z0.d, p0/z, [%1]\n"
                         "spm.st1d z0.d, p0, [%0]\ndsb sy\n"
                         "SPMWB_64_IMM %0, [%2, #0]\ndsb sy\n"
                         :: "r"(slot), "r"(next), "r"(line)
                         : "p0", "z0", "memory");
            copy_line(got, line);
            c = classify(got, core, l, it);
            if (c < 0) report("load", it, l, got);
            dma_load += c == 1;
            // A DMA write after the writeback replaces the stamp; restore it so
            // the next copy expects this iteration's value, as for any store.
            if (c == 1) copy_line(line, next);
#ifdef TARGETED_EVICT
            // Evict this line from its LLC set now, while DMA traffic to it is
            // active: 8 loads at a 16 KiB stride share its bank and set (two
            // 4-way 128-set banks, physically contiguous .bss assumed).
            for (unsigned k = 0; k < 8; ++k) sink += evict[(k * 16384 + 64 * l) / 8];
#endif
        }
        for (unsigned i = 0; i < evict_words; i += 8) sink += evict[i];
    }
    std::printf("SPM_DMA_RACE %s core=%u errors=%d dma_seen_copy=%lu dma_seen_load=%lu sink=%lu\n",
                errors ? "FAIL" : "PASS", core, errors, dma_copy, dma_load, sink);
    return errors != 0;
}
