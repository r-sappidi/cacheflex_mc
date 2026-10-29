// Conservative dependency rule shared by the Ruby selector and standalone tests.
#ifndef __MEM_RUBY_INGRESS_BYPASS_HH__
#define __MEM_RUBY_INGRESS_BYPASS_HH__
#include <cstdint>
namespace gem5::ruby {
struct IngressRequestKey {
    bool reorderable = false; // Ordinary cacheable LD/IFETCH or plain SPMCP only.
    bool copy = false;
    uint64_t coherentLine = 0;
    uint64_t spmLine = 0;
};
inline bool ingressCopyHead(const IngressRequestKey &key)
{
    return key.reorderable && key.copy;
}
inline bool ingressDemandCandidate(const IngressRequestKey &key)
{
    return key.reorderable && !key.copy;
}
inline bool ingressIndependent(const IngressRequestKey &younger,
                               const IngressRequestKey &older)
{
    // Stores, atomics, maintenance, transactions and unsupported SPM operations
    // are full boundaries, even where a weaker architecture could reorder them.
    return younger.reorderable && older.reorderable &&
        younger.coherentLine != older.coherentLine &&
        !(younger.copy && older.copy && younger.spmLine == older.spmLine);
}
}
#endif
