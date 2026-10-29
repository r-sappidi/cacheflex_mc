// Observational host-side bookkeeping only. No modeled queue or timing changes.
#ifndef __MEM_RUBY_CACHEFLEX_AGGREGATE_HH__
#define __MEM_RUBY_CACHEFLEX_AGGREGATE_HH__
#include <cstdlib>
#include <string>
#include "base/types.hh"
namespace gem5::ruby {
class Message;
inline bool cfAggregateEnabled()
{
    static const bool enabled = std::getenv("CACHEFLEX_AGGREGATE_PATH");
    return enabled;
}
void cfAggregateBegin(int core, Addr addr, Tick issue, Tick execute, Tick eligible,
    const void *packet);
void cfAggregateDone(int core, Addr addr);
void cfAggregateTransition(const char *ctrl, int core, Addr addr,
    const std::string &event, const std::string &state,
    const std::string &next, int result);
void cfAggregateResource(const char *ctrl, int core, Addr addr,
    const char *resource, const std::string &event);
void cfAggregateMessage(const char *event, const std::string &object,
    const Message *message, const std::string &fields);
bool cfAggregateObserve(const char *event, const Message *message);
}
#endif
