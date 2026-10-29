// Observational, debug-only per-message/flit timestamps. No modeled resources.
#ifndef __MEM_RUBY_CACHEFLEX_TIMING_HH__
#define __MEM_RUBY_CACHEFLEX_TIMING_HH__

#include <cstdlib>
#include "base/cprintf.hh"
#include "mem/ruby/network/CacheFlexAggregate.hh"
#include "base/trace.hh"
#include "debug/CacheFlexTiming.hh"
#include "mem/ruby/slicc_interface/Message.hh"
#include "mem/ruby/slicc_interface/RubyRequest.hh"
#if __has_include("mem/ruby/protocol/MESI_Three_Level_SPM/RequestMsg.hh")
#include "mem/ruby/protocol/MESI_Three_Level_SPM/RequestMsg.hh"
#include "mem/ruby/protocol/MESI_Three_Level_SPM/ResponseMsg.hh"
#include "mem/ruby/protocol/MESI_Three_Level_SPM/CoherenceMsg.hh"
#endif

namespace gem5::ruby {
inline Addr cfTimingAddress(const Message *msg)
{
    if (auto *m = dynamic_cast<const RubyRequest*>(msg)) return m->m_PhysicalAddress;
#if __has_include("mem/ruby/protocol/MESI_Three_Level_SPM/RequestMsg.hh")
    using namespace MESI_Three_Level_SPM;
    if (auto *m = dynamic_cast<const RequestMsg*>(msg)) return m->getaddr();
    if (auto *m = dynamic_cast<const ResponseMsg*>(msg)) return m->getaddr();
    if (auto *m = dynamic_cast<const CoherenceMsg*>(msg)) return m->getaddr();
#endif
    return MaxAddr;
}
inline bool cfTimingMatch(const Message *msg)
{
    if (!debug::CacheFlexTiming || !msg) return false;
    // Optional physical cache-line filter, evaluated only while debug is on.
    static const char *filter = std::getenv("CACHEFLEX_TRACE_LINE");
    static const Addr line = filter ? std::strtoull(filter, nullptr, 0) : 0;
    return !filter || (cfTimingAddress(msg) & ~Addr(63)) == line;
}
}

// Pointers join queue events to NI clones within one process; packet/flit IDs
// join all network hops. Address alone is never treated as a transaction ID.
#define CFT_MSG(event, msg, fmt, ...) do { \
    const auto *_cfmsg = (msg); \
    if (::gem5::ruby::cfAggregateEnabled() && \
        ::gem5::ruby::cfAggregateObserve(event, _cfmsg)) \
        ::gem5::ruby::cfAggregateMessage(event, name(), _cfmsg, \
            ::gem5::csprintf(fmt, ##__VA_ARGS__)); \
    if (::gem5::ruby::cfTimingMatch(_cfmsg)) { \
        DPRINTF(CacheFlexTiming, "CFT " event " msg=%p addr=%#x " fmt "\n", \
            (const void*)_cfmsg, ::gem5::ruby::cfTimingAddress(_cfmsg), \
            ##__VA_ARGS__); \
    } \
} while (0)

#define CFT_FLIT(event, f, fmt, ...) do { \
    auto *_cfflit = (f); \
    CFT_MSG(event, _cfflit->get_msg_ptr().get(), \
        "packet=%d flit=%d count=%d vc=%d vnet=%d " fmt, \
        _cfflit->getPacketID(), _cfflit->get_id(), _cfflit->get_size(), \
        _cfflit->get_vc(), _cfflit->get_vnet(), ##__VA_ARGS__); \
} while (0)
#endif
