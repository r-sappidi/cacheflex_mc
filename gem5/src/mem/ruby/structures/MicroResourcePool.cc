#include "mem/ruby/structures/MicroResourcePool.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/CacheFlexTiming.hh"

namespace gem5::ruby {
MicroResourcePool::Stats::Stats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(allocations, statistics::units::Count::get(), "Credits acquired"),
    ADD_STAT(releases, statistics::units::Count::get(), "Credits released"),
    ADD_STAT(maxLive, statistics::units::Count::get(), "Maximum live credits"),
    ADD_STAT(fullChecks, statistics::units::Count::get(), "Failed capacity checks, not causal cycles"),
    ADD_STAT(occupancy, statistics::units::Count::get(), "Time-average live credits") {}

MicroResourcePool::MicroResourcePool(const MicroResourcePoolParams &p)
  : SimObject(p), limit(p.capacity), stats(this) {}

bool MicroResourcePool::areNSlotsAvailable(int n, Tick now)
{
    if (!limit || live.size() + n <= limit) return true;
    ++stats.fullChecks;
    DPRINTF(CacheFlexTiming, "CFT POOL_FULL live=%u capacity=%u requested=%d\n",
            live.size(), limit, n);
    return false;
}

bool MicroResourcePool::full() { return !areNSlotsAvailable(1, 0); }

void MicroResourcePool::allocate(Addr address)
{
    if (!limit) return;
    panic_if(live.size() >= limit,
             "%s invalid allocation for %#x", name(), address);
    live.insert(address);
    DPRINTF(CacheFlexTiming, "CFT POOL_ALLOC addr=%#x live=%u capacity=%u\n",
            address, live.size(), limit);
    ++stats.allocations;
    stats.occupancy = live.size();
    if (live.size() > stats.maxLive.value()) stats.maxLive = live.size();
}

void MicroResourcePool::release(Addr address)
{
    // A TBE can be coherence-only, or already have released its data buffer.
    auto entry = live.find(address);
    if (entry != live.end()) {
        live.erase(entry);
        DPRINTF(CacheFlexTiming, "CFT POOL_RELEASE addr=%#x live=%u capacity=%u\n",
                address, live.size(), limit);
        ++stats.releases;
        stats.occupancy = live.size();
    }
}
}
