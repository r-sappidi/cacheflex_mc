// Posted SPM updates remain visible to architectural barriers after early ACK.
// Keys include the System identity: ContextIDs alone are not globally unique.
#ifndef __MEM_CACHEFLEX_REDUCTION_HH__
#define __MEM_CACHEFLEX_REDUCTION_HH__
#include <map>
#include <utility>
#include "base/types.hh"
namespace gem5 {
class System;
namespace cfReduction {
inline std::map<std::pair<const System *, ContextID>, unsigned> pending;
inline bool busy(const System *s, ContextID c) {
    auto it = pending.find({s, c});
    return it != pending.end() && it->second != 0;
}
constexpr Addr Command = Addr(1) << 60;
constexpr Addr HalfContribution = Addr(1) << 58;
constexpr Addr Finalize = Addr(1) << 57;
constexpr Addr Initialize = Addr(1) << 56;
constexpr Addr SlotMask = 0x7ffff;
} // namespace cfReduction
} // namespace gem5
#endif
