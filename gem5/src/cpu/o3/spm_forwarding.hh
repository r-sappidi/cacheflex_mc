// Byte geometry for a vector that spans ways at a fixed SPM slot index.
#ifndef __CPU_O3_SPM_FORWARDING_HH__
#define __CPU_O3_SPM_FORWARDING_HH__
#include <cstdint>
namespace gem5::o3 {
struct SpmForwardRange
{
    unsigned slot, begin, end;
    static SpmForwardRange decode(uint64_t addr, unsigned bytes) {
        unsigned begin = ((addr >> 16) & 7) * 64 + (addr & 63);
        return {static_cast<unsigned>((addr >> 6) & 1023), begin, begin + bytes};
    }
    bool valid() const { return begin < end && end <= 8 * 64; }
    bool overlaps(const SpmForwardRange &r) const {
        return slot == r.slot && begin < r.end && r.begin < end;
    }
    bool covers(const SpmForwardRange &r) const {
        return valid() && r.valid() && slot == r.slot && begin <= r.begin && end >= r.end;
    }
};
}
#endif
