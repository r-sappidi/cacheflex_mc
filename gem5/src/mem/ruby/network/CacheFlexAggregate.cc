// Online SPMCP latency accounting. Retain only live copies and their transport
// spans; completed requests become sums and fixed log2 histograms, never a log.
#include "mem/ruby/network/CacheFlexAggregate.hh"
#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>
#include "base/logging.hh"
#include "sim/cur_tick.hh"
#include "mem/ruby/network/CacheFlexTiming.hh"

namespace gem5::ruby {
namespace {
using Key = std::pair<int, Addr>;
struct Span { Tick a, b; std::string category; int priority; };
struct Copy {
    Tick issue=0, execute=0, eligible=0, accepted=0, mandatoryReady=0;
    Tick l0First=0, l0Done=0, localReady=0, l1First=0, l1Done=0, install=0;
    Tick blockedAt=0;
    Tick llcConsumed=0;
    std::string blockedResource, path="unclassified";
    std::vector<Span> spans;
};
struct MessageTiming { Key key; Tick queued=0, ready=0; };
struct PacketTiming {
    Key key;
    Tick cursor=0, routerReady=0;
    bool sent=false;
};
struct Aggregate {
    uint64_t count=0, ticks=0, min=~uint64_t(0), max=0;
    uint64_t ackTicks=0;
    std::array<uint64_t,64> hist{};
    std::map<std::string,uint64_t> categories;
};
struct Collector {
    std::map<Key,Copy> copies;
    std::map<const Message*,MessageTiming> messages;
    std::map<uint64_t,PacketTiming> packets;
    std::map<const void*,Key> cpuPackets;
    std::map<std::string,Aggregate> totals;
    uint64_t begun=0, completed=0, maxCopies=0, maxMessages=0, maxPackets=0;
    uint64_t missingInstall=0, overlapTicks=0, networkPackets=0;
    uint64_t llcMatchFailures=0;
    Key lastLLC{-1,0}; Tick lastLLCTick=0;
    Tick lo=0, hi=~Tick(0);
    std::string file;
    Collector() : file(std::getenv("CACHEFLEX_AGGREGATE_PATH")) {
        if (auto *v=std::getenv("CACHEFLEX_AGGREGATE_START")) lo=std::strtoull(v,nullptr,0);
        if (auto *v=std::getenv("CACHEFLEX_AGGREGATE_END")) hi=std::strtoull(v,nullptr,0);
    }
    ~Collector() { dump(); }
    void dump() {
        // JSON keys here originate from controlled C++/SLICC identifiers.
        std::ofstream o(file+".tmp");
        o << "{\"schema\":1,\"window_start\":" << lo << ",\"window_end\":" << hi
          << ",\"begun\":" << begun << ",\"completed\":" << completed
          << ",\"inflight\":" << copies.size() << ",\"max_inflight\":" << maxCopies
          << ",\"live_messages\":" << messages.size() << ",\"max_messages\":" << maxMessages
          << ",\"live_packets\":" << packets.size() << ",\"max_packets\":" << maxPackets
          << ",\"network_packets\":" << networkPackets
          << ",\"missing_install\":" << missingInstall << ",\"overlap_ticks\":" << overlapTicks
          << ",\"llc_match_failures\":" << llcMatchFailures << ",\"groups\":{";
        bool first=true;
        for (auto &[name,a]:totals) {
            if (!first) o << ','; first=false;
            o << '"' << name << "\":{\"count\":" << a.count << ",\"total_ticks\":" << a.ticks
              << ",\"min_ticks\":" << a.min << ",\"max_ticks\":" << a.max
              << ",\"ack_after_install_ticks\":" << a.ackTicks << ",\"log2_tick_histogram\":[";
            for (unsigned i=0;i<64;i++) { if(i) o<<','; o<<a.hist[i]; }
            o << "],\"category_ticks\":{"; bool f=true;
            for (auto &[n,v]:a.categories) { if(!f)o<<',';f=false;o<<'"'<<n<<"\":"<<v; }
            o << "}}";
        }
        o << "}}\n"; o.close();
        std::rename((file+".tmp").c_str(),file.c_str());
    }
};
Collector &collector() { static Collector c; return c; }
void add(Copy &r, Tick a, Tick b, const std::string &n, int priority=1) {
    panic_if(b<a,"CacheFlex aggregate backwards interval %s: %llu > %llu",n,a,b);
    if (b>a) {
        if(!r.spans.empty() && r.spans.back().b==a &&
           r.spans.back().category==n && r.spans.back().priority==priority)
            r.spans.back().b=b;
        else r.spans.push_back({a,b,n,priority});
    }
}
std::string field(const std::string &s,const char *key) {
    std::string k=std::string(key)+"=";
    auto p=s.find(k); if(p==std::string::npos)return "";
    p+=k.size();return s.substr(p,s.find(' ',p)-p);
}
uint64_t number(const std::string &s,const char *key) {
    return std::strtoull(field(s,key).c_str(),nullptr,0);
}
Key messageKey(const Message *msg) {
#if __has_include("mem/ruby/protocol/MESI_Three_Level_SPM/RequestMsg.hh")
    using namespace MESI_Three_Level_SPM;
    if(auto *m=dynamic_cast<const RubyRequest*>(msg)) {
        if(m->m_Type==RubyRequestType_SPMCP_fetch) {
            auto &p=collector().cpuPackets;auto it=p.find(m->m_pkt);
            if(it!=p.end())return it->second;
        }
    }
    if(auto *m=dynamic_cast<const CoherenceMsg*>(msg)) {
        if(m->getClass()==CoherenceClass_SPMCP_FETCH)
            return {m->getSender().num,m->getaddr() & ~Addr(63)};
    }
    if(auto *m=dynamic_cast<const RequestMsg*>(msg)) {
        if(m->getRequestor().type==MachineType_L1Cache &&
          (m->getType()==CoherenceRequestType_GETS_SILENT ||
           m->getType()==CoherenceRequestType_PUTS ||
           m->getType()==CoherenceRequestType_PUTM ||
           m->getType()==CoherenceRequestType_PUTE))
            return {m->getRequestor().num,m->getaddr() & ~Addr(63)};
    }
    if(auto *m=dynamic_cast<const ResponseMsg*>(msg)) {
        if(m->getType()==CoherenceResponseType_DATA ||
           m->getType()==CoherenceResponseType_DATA_EXCLUSIVE ||
           m->getType()==CoherenceResponseType_PUT_ACK) {
            Key found{-1,0};
            for(auto &[key,r]:collector().copies) {
                if(key.second != (m->getaddr() & ~Addr(63)) || r.install)continue;
                if(m->getDestination().isElement(MachineID{MachineType_L1Cache,NodeID(key.first)})) {
                    panic_if(found.first!=-1,"Ambiguous SPMCP response destinations");
                    found=key;
                }
            }
            return found;
        }
    }
#endif
    return {-1,0};
}
}

void cfAggregateBegin(int core, Addr addr, Tick issue, Tick execute, Tick eligible,
    const void *packet) {
    auto &c=collector(); if(curTick()<c.lo || curTick()>c.hi)return;
    Key key{core,addr & ~Addr(63)};
    panic_if(c.copies.count(key),"Overlapping issued SPMCPs on same core/source");
    Copy r; r.issue=issue;r.execute=execute;r.eligible=eligible;r.accepted=curTick();
    panic_if(!issue || issue>execute || execute>eligible || eligible>curTick(),
        "Missing/invalid CPU timestamps for SPMCP core %d",core);
    c.copies.emplace(key,std::move(r));++c.begun;
    c.cpuPackets[packet]=key;
    c.maxCopies=std::max<uint64_t>(c.maxCopies,c.copies.size());
}

void cfAggregateResource(const char *ctrl,int core,Addr addr,const char *resource,
    const std::string &event) {
    auto &c=collector(); Key key{core,addr & ~Addr(63)};
    if(std::string(ctrl)!="L0Cache" || event!="SPMCP_fetch")return;
    auto it=c.copies.find(key);if(it==c.copies.end())return;
    auto &r=it->second;
    if(r.l0Done)return;
    if(r.blockedAt) add(r,r.blockedAt,curTick(),r.blockedResource,2);
    r.blockedAt=curTick();r.blockedResource=std::string("L1_forward_block:")+resource;
    // No charge to FIFO followers here: rejection counts are not durations.
    // Followers retain an explicit FIFO-wait category instead of double-counting.
}

void cfAggregateTransition(const char *ctrl,int core,Addr addr,
    const std::string &event,const std::string &state,const std::string &next,int result) {
    auto &c=collector(); Key key{core,addr & ~Addr(63)};
    if(c.copies.empty())return;
    std::string machine(ctrl);
    if(machine=="L2Cache" && result==0 && event=="SPM_GETS_SILENT") {
        if(c.lastLLCTick==curTick() && c.lastLLC.second==key.second && c.copies.count(c.lastLLC)) {
            auto &r=c.copies.at(c.lastLLC);
            if(state=="M" || state=="SS") r.path="llc_hit";
            else if(state=="NP")r.path="llc_miss";
            else if(state=="MT")r.path="owner_forward";
            else r.path="llc_state_"+state;
        } else ++c.llcMatchFailures;
        return;
    }
    auto it=c.copies.find(key);if(it==c.copies.end())return;auto &r=it->second;
    if(event=="SPMCP_fetch") {
        if(machine=="L0Cache") {
            if(!r.l0First)r.l0First=curTick();
            if(result==0) {
                r.l0Done=curTick();
                if(r.blockedAt) {add(r,r.blockedAt,curTick(),r.blockedResource,2);r.blockedAt=0;}
            }
        } else if(machine=="L1Cache") {
            if(!r.l1First)r.l1First=curTick();
            if(result==0) {r.l1Done=curTick();r.path=state=="I"?"private_miss":"private_hit_release";}
        }
    }
    if(machine=="L1Cache" && result==0 && next=="X" &&
       (state=="IX_D" || state=="SX_A" || state=="MX_A" || state=="EX_A" || state=="IX_A"))
        r.install=curTick();
}

bool cfAggregateObserve(const char *event,const Message *msg) {
    auto &c=collector();if(c.copies.empty())return false;
    if(c.messages.count(msg))return true;
    if(std::string(event)=="QUEUE_ENQUEUE" || std::string(event)=="NI_PACKET")
        return c.copies.count(messageKey(msg));
    return false;
}

void cfAggregateMessage(const char *ev,const std::string &object,
    const Message *msg,const std::string &fields) {
    auto &c=collector();if(c.copies.empty())return;
    const std::string e(ev);
    if(e=="QUEUE_ENQUEUE") {
        Key key=messageKey(msg);auto it=c.copies.find(key);if(it==c.copies.end())return;
        Tick ready=number(fields,"ready");
        c.messages[msg]={key,curTick(),ready};
        auto &r=it->second;
        if(object.find("mandatoryQueue")!=std::string::npos)r.mandatoryReady=ready;
        if(object.find("bufferToL1")!=std::string::npos && !r.localReady)r.localReady=ready;
        if(object.find("L1RequestToL2Cache")!=std::string::npos ||
           (object.find("responseFromL2")!=std::string::npos &&
            object.find("responseFromL2Cache")==std::string::npos))
            add(r,curTick(),ready,"network_destination_enqueue_delay",3);
        if(object.find("responseFromL2Cache")!=std::string::npos && r.llcConsumed)
            add(r,r.llcConsumed,curTick(),"LLC_service_memory_or_owner_wait",3);
        c.maxMessages=std::max<uint64_t>(c.maxMessages,c.messages.size());
    } else if(e=="QUEUE_DEQUEUE") {
        auto it=c.messages.find(msg);if(it==c.messages.end())return;
        auto &r=c.copies.at(it->second.key);
#if __has_include("mem/ruby/protocol/MESI_Three_Level_SPM/RequestMsg.hh")
        using namespace MESI_Three_Level_SPM;
        if(auto *m=dynamic_cast<const RequestMsg*>(msg); m &&
           m->getType()==CoherenceRequestType_GETS_SILENT &&
           object.find("L1RequestToL2Cache")!=std::string::npos) {
            c.lastLLC=it->second.key;c.lastLLCTick=curTick();
            if(number(fields,"final"))r.llcConsumed=curTick();
        }
#endif
        if(number(fields,"final")) {
            if(object.find("L1RequestToL2Cache")!=std::string::npos)
                add(r,it->second.ready,curTick(),"LLC_request_queue_controller_wait",3);
            if(object.find("responseFromL2")!=std::string::npos &&
               object.find("responseFromL2Cache")==std::string::npos)
                add(r,it->second.ready,curTick(),"private_L2_response_queue_install_wait",3);
            c.messages.erase(it);
        }
    } else if(e=="NI_PACKET") {
        auto *parent=reinterpret_cast<const Message*>(number(fields,"parent"));
        auto it=c.messages.find(parent);if(it==c.messages.end())return;
        auto mt=it->second;auto ci=c.copies.find(mt.key);if(ci==c.copies.end())return;
        auto &r=ci->second;
        add(r,mt.queued,mt.ready,"network_fixed_enqueue",3);
        add(r,mt.ready,curTick(),"network_message_wait_before_NI",3);
        auto id=number(fields,"packet");
        c.packets[id]={mt.key,curTick(),0,false};
        c.messages[msg]=mt;
        c.maxPackets=std::max<uint64_t>(c.maxPackets,c.packets.size());++c.networkPackets;
    } else {
        // Follow only the tail flit's elapsed path. Head/body activity overlaps.
        if(field(fields,"flit").empty() || number(fields,"flit")+1!=number(fields,"count"))return;
        auto pi=c.packets.find(number(fields,"packet"));if(pi==c.packets.end())return;
        auto ci=c.copies.find(pi->second.key);if(ci==c.copies.end())return;
        auto &p=pi->second;auto &r=ci->second;
        if(e=="NI_SEND") {
            add(r,p.cursor,curTick(),"network_NI_serialization_flow_control",3);
            p.cursor=curTick();p.sent=true;
        } else if(e=="ROUTER_READY") {
            Tick ready=number(fields,"ready");
            add(r,p.cursor,ready,"network_pipeline_links",3);
            p.cursor=ready;p.routerReady=ready;
        } else if(e=="SA_GRANT") {
            add(r,p.cursor,curTick(),"network_router_buffer_arbitration",3);p.cursor=curTick();
        } else if(e=="NI_ARRIVE") {
            add(r,p.cursor,curTick(),"network_pipeline_links",3);p.cursor=curTick();
        } else if(e=="NI_COMPLETE") {
            add(r,p.cursor,curTick(),"network_destination_backpressure",3);
            c.packets.erase(pi);
        }
    }
}

void cfAggregateDone(int core,Addr addr) {
    auto &c=collector();Key key{core,addr & ~Addr(63)};
    auto it=c.copies.find(key);if(it==c.copies.end())return;auto &r=it->second;
    if(!r.install) { ++c.missingInstall; c.dump(); panic("SPMCP completed without observed installation"); }
    panic_if(!r.l0First || !r.l0Done || !r.mandatoryReady || !r.localReady || !r.l1Done,
        "SPMCP missing stage anchor core=%d addr=%#x mandatory=%llu l0first=%llu l0done=%llu local=%llu l1done=%llu",
        core,addr,r.mandatoryReady,r.l0First,r.l0Done,r.localReady,r.l1Done);
    add(r,r.issue,r.execute,"CPU_issue_to_execute");
    add(r,r.execute,r.eligible,"CPU_execute_to_store_eligible");
    add(r,r.eligible,r.accepted,"CPU_store_eligible_to_Ruby_acceptance");
    add(r,r.accepted,r.mandatoryReady,"L1_fixed_ingress_delay");
    add(r,r.mandatoryReady,r.l0First,"L1_ingress_FIFO_wait");
    add(r,r.l0First,r.l0Done,"L1_forward_other_wait");
    add(r,r.l0Done,r.localReady,"private_L2_fixed_local_delay");
    add(r,r.localReady,r.l1Done,"private_L2_input_wait");
    add(r,r.l1Done,r.install,"source_service_and_destination_wait");
    std::set<Tick> boundaries{r.issue,r.install};
    for(auto &s:r.spans) {if(s.a>=r.issue && s.a<=r.install)boundaries.insert(s.a);
                         if(s.b>=r.issue && s.b<=r.install)boundaries.insert(s.b);}
    std::map<std::string,uint64_t> categories;
    auto a=boundaries.begin();auto b=a;++b;
    for(;b!=boundaries.end();++a,++b) {
        const Span *chosen=nullptr;
        for(auto &s:r.spans) if(s.a<=*a && s.b>=*b) {
            if(!chosen || s.priority>chosen->priority)chosen=&s;
            else if(s.priority==chosen->priority) c.overlapTicks+=*b-*a;
        }
        panic_if(!chosen,"Uncovered SPMCP latency interval");
        categories[chosen->category]+=*b-*a;
    }
    Tick duration=r.install-r.issue;
    for(auto group:{std::string("all"),"core_"+std::to_string(core),"path_"+r.path}) {
        auto &a=c.totals[group];++a.count;a.ticks+=duration;
        a.min=std::min(a.min,duration);a.max=std::max(a.max,duration);
        a.ackTicks+=curTick()-r.install;
        unsigned bin=0;for(Tick n=duration;n>1;n>>=1)++bin;++a.hist[bin];
        for(auto &[name,t]:categories)a.categories[name]+=t;
    }
    c.copies.erase(it);++c.completed;
    for(auto i=c.cpuPackets.begin();i!=c.cpuPackets.end();) {
        if(i->second==key)i=c.cpuPackets.erase(i);else ++i;
    }
    // Erase sidecars even for a message stalled/recycled earlier. No stale
    // pointer is retained across completed requests or allocator pointer reuse.
    for(auto i=c.messages.begin();i!=c.messages.end();) {
        if(i->second.key==key)i=c.messages.erase(i);else ++i;
    }
    for(auto i=c.packets.begin();i!=c.packets.end();) {
        if(i->second.key==key)i=c.packets.erase(i);else ++i;
    }
    if(c.completed%4096==0)c.dump();
}
}
