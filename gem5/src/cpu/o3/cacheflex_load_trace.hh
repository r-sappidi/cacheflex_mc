// Debug-only selected-PC load lifetime observations. No simulated resources.
#ifndef __CPU_O3_CACHEFLEX_LOAD_TRACE_HH__
#define __CPU_O3_CACHEFLEX_LOAD_TRACE_HH__
#include <cstdlib>
#include <set>
#include <sstream>
#include <string>
#include "base/trace.hh"
#include "base/types.hh"
#include "debug/CacheFlexLoads.hh"
namespace gem5::o3 {
inline bool cfTraceLoad(Addr pc)
{
    if (!debug::CacheFlexLoads) return false;
    static const std::set<Addr> pcs = [] {
        std::set<Addr> out;
        const char *env = std::getenv("CACHEFLEX_LOAD_PCS");
        if (!env) return out;
        std::stringstream input(env); std::string item;
        while (std::getline(input, item, ','))
            out.insert(std::strtoull(item.c_str(), nullptr, 0));
        return out;
    }();
    return pcs.count(pc);
}
}
#define CFL_INST(event, inst, fmt, ...) do { \
    const auto &_cfi = (inst); \
    if (::gem5::o3::cfTraceLoad(_cfi->pcState().instAddr())) \
        DPRINTF(CacheFlexLoads, "CFL " event " sn=%llu pc=%#x " fmt "\n", \
            _cfi->seqNum, _cfi->pcState().instAddr(), ##__VA_ARGS__); \
} while (0)
#define CFL_PKT(event, pkt, fmt, ...) do { \
    const auto *_cfp = (pkt); \
    if (_cfp->req->hasPC() && ::gem5::o3::cfTraceLoad(_cfp->req->getPC())) \
        DPRINTF(CacheFlexLoads, "CFL " event " id=%p pc=%#x pa=%#x bytes=%u " fmt "\n", \
            (const void*)_cfp, _cfp->req->getPC(), _cfp->getAddr(), _cfp->getSize(), ##__VA_ARGS__); \
} while (0)
#endif
