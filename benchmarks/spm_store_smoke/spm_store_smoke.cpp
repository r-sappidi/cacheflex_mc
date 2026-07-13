#include <cstdint>
#include <cstdio>
#include <cstring>
static inline uint64_t spm_addr(unsigned way, unsigned set, unsigned off)
{ return (1ULL << 40) | ((uint64_t)way << 16) | ((uint64_t)set << 6) | off; }
static inline void spmcp64(uint64_t dst, const void *src)
{ asm volatile("SPMCP_64_IMM %0, [%1, #0]\n" :: "r"(dst), "r"(src) : "memory"); }
static inline void spm_store64_from_mem(uint64_t dst, const void *src)
{ asm volatile("mov x9, #8\nwhilelt p0.d, xzr, x9\nld1d z0.d, p0/z, [%1]\nspm.st1d z0.d, p0, [%0]\n" :: "r"(dst), "r"(src) : "x9","p0","z0","memory"); }
static inline void spm_load64_to_mem(void *dst, uint64_t src)
{ asm volatile("mov x9, #8\nwhilelt p0.d, xzr, x9\nspm.ld1d z0.d, p0/z, [%1]\nst1d z0.d, p0, [%0]\n" :: "r"(dst), "r"(src) : "x9","p0","z0","memory"); }
static inline void spm_release(uint64_t slot)
{ asm volatile("SPMREL_8_IMM xzr, [%0, #0]\n" :: "r"(slot) : "memory"); }
int main()
{
    alignas(64) uint64_t a[8], b[8], out[8];
    for (int i = 0; i < 8; ++i) { a[i] = 0x1111000000ULL + i; b[i] = 0x2222000000ULL + i; }
    uint64_t slot = spm_addr(0, 0, 0);
    spmcp64(slot, a);
    asm volatile("dsb sy" ::: "memory");
    spm_store64_from_mem(slot, b);
    asm volatile("dsb sy" ::: "memory");
    spm_load64_to_mem(out, slot);
    asm volatile("dsb sy" ::: "memory");
    int ok = memcmp(out, b, sizeof b) == 0;
    int stale = memcmp(out, a, sizeof a) == 0;
    printf("spmst_test %s%s out[0]=%llx expect=%llx\n", ok ? "PASS" : "FAIL",
           stale ? " (STALE: spm.st1d dropped)" : "",
           (unsigned long long)out[0], (unsigned long long)b[0]);
    spm_release(slot);
    return ok ? 0 : 1;
}
