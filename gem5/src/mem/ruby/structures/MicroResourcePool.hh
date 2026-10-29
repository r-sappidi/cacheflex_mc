#ifndef __MEM_RUBY_MICRO_RESOURCE_POOL_HH__
#define __MEM_RUBY_MICRO_RESOURCE_POOL_HH__

#include <unordered_set>
#include "base/statistics.hh"
#include "params/MicroResourcePool.hh"
#include "sim/sim_object.hh"

namespace gem5::ruby {
// Models transaction storage independently from protocol TBE representation.
// Payloads remain in existing TBEs/queues; this never adds a data SRAM.
class MicroResourcePool : public SimObject
{
  private:
    const unsigned limit;
    std::unordered_multiset<Addr> live;
    struct Stats : statistics::Group {
        Stats(statistics::Group *parent);
        statistics::Scalar allocations, releases, maxLive, fullChecks;
        statistics::Average occupancy;
    } stats;
  public:
    MicroResourcePool(const MicroResourcePoolParams &p);
    bool areNSlotsAvailable(int n, Tick now);
    bool full();
    bool contains(Addr address) const { return live.count(address); }
    void allocate(Addr address);
    void release(Addr address);
};
}
#endif
