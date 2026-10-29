/*
 * Copyright (c) 2019-2021,2023 ARM Limited
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 1999-2008 Mark D. Hill and David A. Wood
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mem/ruby/system/Sequencer.hh"
#include "mem/cacheflex_reduction.hh"
#include "cpu/o3/cacheflex_load_trace.hh"

#include "base/logging.hh"
#include "cpu/testers/rubytest/RubyTester.hh"
#include "debug/LLSC.hh"
#include "debug/ProtocolTrace.hh"
#include "mem/ruby/network/CacheFlexAggregate.hh"
#include "cpu/o3/lsq.hh"
#include "cpu/o3/dyn_inst.hh"
#include "debug/RubyHitMiss.hh"
#include "debug/RubySequencer.hh"
#include "debug/CacheFlexResidency.hh"
#include "debug/RubyStats.hh"
#include "mem/packet.hh"
#include "mem/ruby/profiler/Profiler.hh"
#include "mem/ruby/protocol/PrefetchBit.hh"
#include "mem/ruby/protocol/RubyAccessMode.hh"
#include "mem/ruby/slicc_interface/RubyRequest.hh"
#include "mem/ruby/slicc_interface/RubySlicc_Util.hh"
#include "mem/ruby/system/RubySystem.hh"

namespace gem5
{

namespace ruby
{

namespace
{

// SPM addresses are an ISA-local {way, set, offset} encoding, not coherent
// physical addresses.  In SE mode low encoded slots (for example 0x1d80)
// can equal the translated physical line of ordinary code/data (the virtual
// PC 0x401d80 maps to PA 0x1d80).  Keying Ruby's request table and cache tags
// with the raw encoding can then coalesce an SPMLD with an instruction fetch.
// Put all SPM slots in a disjoint Ruby-only namespace while retaining the low
// 19 encoded bits: offset[5:0], set[15:6], way[18:16].
constexpr Addr SpmRubyNamespaceBase = 0x60000000ULL;
constexpr Addr SpmEncodedAddressMask = 0x7ffffULL;

Addr
canonicalSPMAddress(Addr encoded_addr)
{
    if (encoded_addr & (Addr(1) << 59))
        return 0x68000000ULL | (encoded_addr & 0x77ffffULL);
    return SpmRubyNamespaceBase | (encoded_addr & SpmEncodedAddressMask);
}

Addr
rubyRequestAddress(PacketPtr pkt, RubyRequestType type)
{
    if (!pkt->req->isSPMRequest()) {
        return pkt->getAddr();
    }

    if (type == RubyRequestType_SPMCP_fetch) {
        // A multicast claim has no coherent source access.  Its architectural
        // source operand is deliberately xzr, so keying claims by the packet
        // address would coalesce every outstanding slot claim onto line zero.
        // Use the claimed destination slot as the Ruby transaction identity.
        if (pkt->req->isSPMMulticastClaim()) {
            return canonicalSPMAddress(pkt->req->getSPMDstAddr());
        }
        // The packet address is the translated physical source address.
        // getSPMSrcAddr() holds the virtual address captured when the LSQ
        // created the request, before translation, and must not be used to
        // key coherence.
        return pkt->getAddr();
    }

    if (type == RubyRequestType_SPMWB_store) {
        // The O3 bridge metadata stores {regular address, encoded SPM slot}
        // for BOTH copy directions. "Dst" names the slot field, not the
        // semantic direction of a writeback.
        return canonicalSPMAddress(pkt->req->getSPMDstAddr());
    }

    if (type == RubyRequestType_SPMCP_install ||
        type == RubyRequestType_SPMLD ||
        type == RubyRequestType_SPMST ||
        type == RubyRequestType_SPMWB_read ||
        type == RubyRequestType_SPM_release) {
        return canonicalSPMAddress(pkt->req->getSPMDstAddr());
    }

    return pkt->getAddr();
}

} // anonymous namespace

Sequencer::Sequencer(const Params &p)
    : RubyPort(p),
      reductionEnabled(p.cacheflex_reduction),
      reductionAsync(p.cacheflex_reduction_async),
      l3ReductionEnabled(p.cacheflex_l3_reduction),
      reductionCache(p.cacheflex_reduction_cache),
      reductionSourceCap(p.cacheflex_reduction_source_entries),
      reductionSinkCap(p.cacheflex_reduction_sink_entries),
      reductionLinkBytes(p.cacheflex_reduction_link_bytes),
      reductionLanes(p.cacheflex_reduction_lanes),
      reductionLinkLatency(p.cacheflex_reduction_link_latency),
      reductionMulLatency(p.cacheflex_reduction_mul_latency),
      reductionAddLatency(p.cacheflex_reduction_add_latency),
      reductionEvent([this]{ serviceReduction(); }, "Bounded SPM reduction"),
      ADD_STAT(reductionAccepted, statistics::units::Count::get(), "Captured FP32 scale/add commands"),
      ADD_STAT(reductionCompleted, statistics::units::Count::get(), "Installed FP32 scale/add commands"),
      ADD_STAT(reductionSourceFull, statistics::units::Count::get(), "Reduction source-full rejected attempts"),
      ADD_STAT(reductionOrderingRetries, statistics::units::Count::get(), "Conflicting accesses held for posted updates"),
      ADD_STAT(reductionTransportBytes, statistics::units::Byte::get(), "Contribution plus 16B header traffic"),
      ADD_STAT(reductionMaxSource, statistics::units::Count::get(), "Maximum source queue occupancy"),
      ADD_STAT(reductionMaxSink, statistics::units::Count::get(), "Maximum sink queue occupancy including active RMW"),
      ADD_STAT(reductionLifetimeCycles, statistics::units::Cycle::get(), "Sum capture to installation lifetimes; not removable runtime"),
      ADD_STAT(reductionMemoryRetries, statistics::units::Count::get(), "Engine SRAM-path admission retries"),
      ADD_STAT(reductionSinkFullCycles, statistics::units::Cycle::get(), "Cycles waiting for destination queue space"),
      ADD_STAT(m_spmcpRequestsIssued, statistics::units::Count::get(),
               "Number of SPMCP Ruby transactions issued"),
      ADD_STAT(m_spmcpRequestsCompleted, statistics::units::Count::get(),
               "Number of SPMCP Ruby transactions completed"),
      ADD_STAT(m_spmcpClaimRequestsIssued, statistics::units::Count::get(),
               "Number of multicast claim transactions issued"),
      ADD_STAT(m_spmcpClaimRequestsCompleted, statistics::units::Count::get(),
               "Number of multicast claim transactions completed"),
      ADD_STAT(m_spmcpClaimAliasedRequests, statistics::units::Count::get(),
               "Number of multicast claims aliased in the request table"),
      ADD_STAT(m_spmcpDataRequestsIssued, statistics::units::Count::get(),
               "Number of data-fetch SPMCP transactions issued"),
      ADD_STAT(m_spmcpDataRequestsCompleted, statistics::units::Count::get(),
               "Number of data-fetch SPMCP transactions completed"),
      ADD_STAT(m_spmcpDataAliasedRequests, statistics::units::Count::get(),
               "Number of data-fetch SPMCPs aliased in the request table"),
      ADD_STAT(m_spmcpBusyCycles, statistics::units::Cycle::get(),
               "Union of cycles with one or more SPMCPs outstanding"),
      ADD_STAT(m_spmcpOutstandingCycleArea, statistics::units::Cycle::get(),
               "Integral of outstanding SPMCP count over cycles"),
      ADD_STAT(m_spmcpMaxOutstanding, statistics::units::Count::get(),
               "Maximum concurrent outstanding SPMCP transactions"),
      ADD_STAT(m_spmcpAverageParallelism, statistics::units::Ratio::get(),
               "Average SPMCP parallelism while at least one copy is active"),
      ADD_STAT(m_spmcpRequestAttempts, statistics::units::Count::get(),
               "Number of SPMCP admission attempts, including retries"),
      ADD_STAT(m_spmcpBufferFullRejects, statistics::units::Count::get(),
               "SPMCP attempts rejected because the sequencer was full"),
      ADD_STAT(m_ingressFullRejects, statistics::units::Count::get(),
               "Requests rejected because the finite controller ingress was full"),
      ADD_STAT(m_ingressDeferredReplays, statistics::units::Count::get(),
               "Already tracked alias replays waiting for controller ingress"),
      ADD_STAT(m_lineTargetFullRejects, statistics::units::Count::get(),
               "Requests rejected because per-line coalescing records are full"),
      ADD_STAT(m_globalAdmissionFullRejects, statistics::units::Count::get(),
               "Requests rejected by global sequencer guard (must be zero in bounded profile)"),
      ADD_STAT(m_spmcpBlockedLineRejects, statistics::units::Count::get(),
               "SPMCP attempts rejected because the source line was blocked"),
      ADD_STAT(m_spmcpAliasedRequests, statistics::units::Count::get(),
               "SPMCP requests combined with an outstanding same-line request"),
      ADD_STAT(m_idealStagingCopies, statistics::units::Count::get(),
               "Diagnostic oracle copies into already claimed SPM slots"),
      ADD_STAT(m_earlyReadEligible, statistics::units::Count::get(), "PC-selected accumulator read attempts"),
      ADD_STAT(m_earlyReadIssued, statistics::units::Count::get(), "Oracle read responses; real requests still issued"),
      ADD_STAT(m_earlyReadVerified, statistics::units::Count::get(), "Oracle values verified against eventual real responses"),
      ADD_STAT(m_earlyReadFallbackAlias, statistics::units::Count::get(), "Oracle reads kept normal due to in-flight same-line request"),
      ADD_STAT(m_earlyReadFallbackFunctional, statistics::units::Count::get(), "Oracle reads kept normal when functional data unavailable"),
      ADD_STAT(m_idealStagingFallbackSlot, statistics::units::Count::get(),
               "Oracle attempts falling back for absent or busy SPM slot"),
      ADD_STAT(m_idealStagingFallbackAlias, statistics::units::Count::get(),
               "Oracle attempts falling back for outstanding source/destination access"),
      ADD_STAT(m_idealStagingFallbackRead, statistics::units::Count::get(),
               "Oracle attempts falling back because Ruby functional read failed"),
      ADD_STAT(m_idealStagingOrderingRetries, statistics::units::Count::get(),
               "Requests held until a conflicting oracle response completes"),
      m_IncompleteTimes(MachineType_NUM),
      deadlockCheckEvent([this]{ wakeup(); }, "Sequencer deadlock check"),
      ingressEvent([this]{ serviceIngress(); }, "Finite controller ingress retry")
{
    fatal_if(reductionEnabled && (!reductionCache || !reductionSourceCap || !reductionSinkCap ||
             !reductionLinkBytes || !reductionLanes || reductionLanes > 64 ||
             !uint64_t(reductionLinkLatency) || !uint64_t(reductionMulLatency) ||
             !uint64_t(reductionAddLatency)), "Reduction resources must be finite and positive");
    m_spmcpAverageParallelism =
        m_spmcpOutstandingCycleArea / m_spmcpBusyCycles;
    m_spmcpLastAccountCycle = curCycle();
    m_outstanding_count = 0;

    m_ruby_system = p.ruby_system;
    m_earlyReadPcs.insert(p.cacheflex_early_read_pcs.begin(), p.cacheflex_early_read_pcs.end());

    m_dataCache_ptr = p.dcache;
    m_idealStagingCache = p.cacheflex_ideal_staging_cache;
    m_idealStagingController = p.cacheflex_ideal_staging_controller;
    fatal_if(bool(m_idealStagingCache) != bool(m_idealStagingController),
             "Ideal staging requires both the existing cache and its controller");
    fatal_if(m_idealStagingCache && cfAggregateEnabled(),
             "Normal path aggregate cannot describe oracle copies; use oracle counters");
    m_max_outstanding_requests = p.max_outstanding_requests;
    m_cacheflex_fetch_credits = p.cacheflex_fetch_credits;
    m_cacheflex_bridge_credits = p.cacheflex_bridge_credits;
    m_cacheflex_max_bridge_requests = p.cacheflex_max_bridge_requests;
    m_micro_spm_ingress = p.cacheflex_spm_ingress;
    m_micro_data_misses = p.cacheflex_micro_data_misses;
    m_micro_inst_misses = p.cacheflex_micro_inst_misses;
    m_micro_writebacks = p.cacheflex_micro_writebacks;
    m_micro_targets = p.cacheflex_micro_targets;
    m_finite_ingress = p.cacheflex_finite_ingress;
    m_max_line_targets = p.cacheflex_max_line_targets;
    fatal_if(m_cacheflex_max_bridge_requests < 0 ||
             m_cacheflex_max_bridge_requests > m_max_outstanding_requests,
             "Bridge cap must be within total sequencer capacity");
    fatal_if(m_cacheflex_fetch_credits < 0 || m_cacheflex_bridge_credits < 0 ||
             m_cacheflex_fetch_credits + m_cacheflex_bridge_credits >=
                 m_max_outstanding_requests,
             "Dedicated admission credits must leave positive shared capacity");
    m_deadlock_threshold = p.deadlock_threshold;

    m_coreId = p.coreid; // for tracking the two CorePair sequencers
    assert(m_max_outstanding_requests > 0);
    assert(m_deadlock_threshold > 0);

    m_unaddressedTransactionCnt = 0;

    m_runningGarnetStandalone = p.garnet_standalone;

    m_num_pending_invs = 0;
    m_cache_inv_pkt = nullptr;

    // These statistical variables are not for display.
    // The profiler will collate these across different
    // sequencers and display those collated statistics.
    m_outstandReqHist.init(10);
    m_latencyHist.init(10);
    m_hitLatencyHist.init(10);
    m_missLatencyHist.init(10);

    for (int i = 0; i < RubyRequestType_NUM; i++) {
        m_typeLatencyHist.push_back(new statistics::Histogram());
        m_typeLatencyHist[i]->init(10);

        m_hitTypeLatencyHist.push_back(new statistics::Histogram());
        m_hitTypeLatencyHist[i]->init(10);

        m_missTypeLatencyHist.push_back(new statistics::Histogram());
        m_missTypeLatencyHist[i]->init(10);
    }

    for (int i = 0; i < MachineType_NUM; i++) {
        m_hitMachLatencyHist.push_back(new statistics::Histogram());
        m_hitMachLatencyHist[i]->init(10);

        m_missMachLatencyHist.push_back(new statistics::Histogram());
        m_missMachLatencyHist[i]->init(10);

        m_IssueToInitialDelayHist.push_back(new statistics::Histogram());
        m_IssueToInitialDelayHist[i]->init(10);

        m_InitialToForwardDelayHist.push_back(new statistics::Histogram());
        m_InitialToForwardDelayHist[i]->init(10);

        m_ForwardToFirstResponseDelayHist.push_back(
            new statistics::Histogram());
        m_ForwardToFirstResponseDelayHist[i]->init(10);

        m_FirstResponseToCompletionDelayHist.push_back(
            new statistics::Histogram());
        m_FirstResponseToCompletionDelayHist[i]->init(10);
    }

    for (int i = 0; i < RubyRequestType_NUM; i++) {
        m_hitTypeMachLatencyHist.push_back(
            std::vector<statistics::Histogram *>());
        m_missTypeMachLatencyHist.push_back(
            std::vector<statistics::Histogram *>());

        for (int j = 0; j < MachineType_NUM; j++) {
            m_hitTypeMachLatencyHist[i].push_back(new statistics::Histogram());
            m_hitTypeMachLatencyHist[i][j]->init(10);

            m_missTypeMachLatencyHist[i].push_back(
                new statistics::Histogram());
            m_missTypeMachLatencyHist[i][j]->init(10);
        }
    }

}

Sequencer::~Sequencer()
{
}

void
Sequencer::llscLoadLinked(const Addr claddr)
{
    fatal_if(m_dataCache_ptr == NULL,
        "%s must have a dcache object to support LLSC requests.", name());
    AbstractCacheEntry *line = m_dataCache_ptr->lookup(claddr);
    if (line) {
        line->setLocked(m_version);
        DPRINTF(LLSC, "LLSC Monitor - inserting load linked - "
                      "addr=0x%lx - cpu=%u\n", claddr, m_version);
    }
}

void
Sequencer::llscClearMonitor(const Addr claddr)
{
    // clear monitor is called for all stores and evictions
    if (m_dataCache_ptr == NULL)
        return;
    AbstractCacheEntry *line = m_dataCache_ptr->lookup(claddr);
    if (line && line->isLocked(m_version)) {
        line->clearLocked();
        DPRINTF(LLSC, "LLSC Monitor - clearing due to store - "
                      "addr=0x%lx - cpu=%u\n", claddr, m_version);
    }
}

bool
Sequencer::llscStoreConditional(const Addr claddr)
{
    fatal_if(m_dataCache_ptr == NULL,
        "%s must have a dcache object to support LLSC requests.", name());
    AbstractCacheEntry *line = m_dataCache_ptr->lookup(claddr);
    if (!line)
        return false;

    DPRINTF(LLSC, "LLSC Monitor - clearing due to "
                  "store conditional - "
                  "addr=0x%lx - cpu=%u\n",
                  claddr, m_version);

    if (line->isLocked(m_version)) {
        line->clearLocked();
        return true;
    } else {
        line->clearLocked();
        return false;
    }
}

bool
Sequencer::llscCheckMonitor(const Addr address)
{
    assert(m_dataCache_ptr != NULL);
    const Addr claddr = makeLineAddress(address);
    AbstractCacheEntry *line = m_dataCache_ptr->lookup(claddr);
    if (!line)
        return false;

    if (line->isLocked(m_version)) {
        return true;
    } else {
        return false;
    }
}

void
Sequencer::llscClearLocalMonitor()
{
    m_dataCache_ptr->clearLockedAll(m_version);
}

void
Sequencer::wakeup()
{
    assert(drainState() != DrainState::Draining);

    // Check for deadlock of any of the requests
    Cycles current_time = curCycle();

    // Check across all outstanding requests
    [[maybe_unused]] int total_outstanding = 0;

    for (const auto &table_entry : m_RequestTable) {
        for (const auto &seq_req : table_entry.second) {
            if (current_time - seq_req.issue_time < m_deadlock_threshold)
                continue;

            panic("Possible Deadlock detected. Aborting!\n version: %d "
                  "request.paddr: 0x%x m_readRequestTable: %d current time: "
                  "%u issue_time: %d difference: %d\n", m_version,
                  seq_req.pkt->getAddr(), table_entry.second.size(),
                  current_time * clockPeriod(), seq_req.issue_time
                  * clockPeriod(), (current_time * clockPeriod())
                  - (seq_req.issue_time * clockPeriod()));
        }
        total_outstanding += table_entry.second.size();
    }

    assert(m_outstanding_count == total_outstanding);

    if (m_outstanding_count > 0) {
        // If there are still outstanding requests, keep checking
        schedule(deadlockCheckEvent, clockEdge(m_deadlock_threshold));
    }
}

int
Sequencer::functionalWrite(Packet *func_pkt)
{
    int num_written = RubyPort::functionalWrite(func_pkt);

    for (const auto &table_entry : m_RequestTable) {
        for (const auto& seq_req : table_entry.second) {
            if (seq_req.functionalWrite(func_pkt))
                ++num_written;
        }
    }
    // Functional writes to addresses being monitored
    // will fail (remove) the monitor entry.
    llscClearMonitor(makeLineAddress(func_pkt->getAddr()));

    return num_written;
}

void Sequencer::resetStats()
{
    accountSPMCPOutstanding(curCycle());
    m_spmcpRequestsIssued.reset();
    m_spmcpRequestsCompleted.reset();
    m_spmcpClaimRequestsIssued.reset();
    m_spmcpClaimRequestsCompleted.reset();
    m_spmcpClaimAliasedRequests.reset();
    m_spmcpDataRequestsIssued.reset();
    m_spmcpDataRequestsCompleted.reset();
    m_spmcpDataAliasedRequests.reset();
    m_spmcpBusyCycles.reset();
    m_spmcpOutstandingCycleArea.reset();
    m_spmcpMaxOutstanding.reset();
    m_spmcpRequestAttempts.reset();
    m_spmcpBufferFullRejects.reset();
    m_spmcpBlockedLineRejects.reset();
    m_spmcpAliasedRequests.reset();
    m_spmcpLastAccountCycle = curCycle();
    m_outstandReqHist.reset();
    m_latencyHist.reset();
    m_hitLatencyHist.reset();
    m_missLatencyHist.reset();
    for (int i = 0; i < RubyRequestType_NUM; i++) {
        m_typeLatencyHist[i]->reset();
        m_hitTypeLatencyHist[i]->reset();
        m_missTypeLatencyHist[i]->reset();
        for (int j = 0; j < MachineType_NUM; j++) {
            m_hitTypeMachLatencyHist[i][j]->reset();
            m_missTypeMachLatencyHist[i][j]->reset();
        }
    }

    for (int i = 0; i < MachineType_NUM; i++) {
        m_missMachLatencyHist[i]->reset();
        m_hitMachLatencyHist[i]->reset();

        m_IssueToInitialDelayHist[i]->reset();
        m_InitialToForwardDelayHist[i]->reset();
        m_ForwardToFirstResponseDelayHist[i]->reset();
        m_FirstResponseToCompletionDelayHist[i]->reset();

        m_IncompleteTimes[i] = 0;
    }
}

void
Sequencer::accountSPMCPOutstanding(Cycles now)
{
    assert(now >= m_spmcpLastAccountCycle);
    const Cycles elapsed = now - m_spmcpLastAccountCycle;
    if (m_spmcpOutstanding != 0) {
        m_spmcpBusyCycles += elapsed;
        m_spmcpOutstandingCycleArea += elapsed * m_spmcpOutstanding;
    }
    m_spmcpLastAccountCycle = now;
}

// Insert the request in the request table. Return RequestStatus_Aliased
// if the entry was already present.
RequestStatus
Sequencer::insertRequest(PacketPtr pkt, RubyRequestType primary_type,
                         RubyRequestType secondary_type)
{
    // See if we should schedule a deadlock check
    if (!deadlockCheckEvent.scheduled() &&
        drainState() != DrainState::Draining) {
        schedule(deadlockCheckEvent, clockEdge(m_deadlock_threshold));
    }

    if (isTlbiCmdRequest(primary_type)) {
        assert(primary_type == secondary_type);

        switch (primary_type) {
        case RubyRequestType_TLBI_EXT_SYNC_COMP:
            // Don't have to store any data on this
            break;
        case RubyRequestType_TLBI:
        case RubyRequestType_TLBI_SYNC:
            {
                incrementUnaddressedTransactionCnt();

                // returns pair<inserted element, was inserted>
                [[maybe_unused]] auto insert_data = \
                    m_UnaddressedRequestTable.emplace(
                        getCurrentUnaddressedTransactionID(),
                        SequencerRequest(
                            pkt, primary_type, secondary_type, curCycle()));

                // if insert_data.second is false, wasn't inserted
                assert(insert_data.second &&
                       "Another TLBI request with the same ID exists");

                DPRINTF(RubySequencer, "Inserting TLBI request %016x\n",
                        getCurrentUnaddressedTransactionID());

                break;
            }

        default:
            panic("Unexpected TLBI RubyRequestType");
        }

        return RequestStatus_Ready;
    }

    // If command is MemSyncReq, it is used to invalidate the cache.
    // As the cache invalidation requests are already issued in invL1(),
    // there is no need to create a new request for the same here.
    // Instead, return RequestStatus_Aliased, and make the sequencer skip
    // an extra issueRequest
    if (pkt->cmd == MemCmd::MemSyncReq) {
        return RequestStatus_Aliased;
    }

    Addr line_addr = makeLineAddress(rubyRequestAddress(pkt, primary_type));
    // Check if there is any outstanding request for the same cache line.
    auto &seq_req_list = m_RequestTable[line_addr];

    // For software prefetches, if the same cache line is already requested we
    // can return aliased and skip emplacing it on the request table, this way
    // we won't need to handle any response at all
    if (pkt->cmd.isSWPrefetch() && seq_req_list.size() > 1) {
        return RequestStatus_Aliased;
    }

    const bool live_miss =
        (m_micro_data_misses && m_micro_data_misses->contains(line_addr)) ||
        (m_micro_inst_misses && m_micro_inst_misses->contains(line_addr));
    if (m_micro_targets && live_miss && seq_req_list.size() >= m_micro_targets) {
        ++m_lineTargetFullRejects;
        return RequestStatus_BufferFull;
    }
    // Create a default entry
    if (m_max_line_targets && seq_req_list.size() >= m_max_line_targets) {
        ++m_lineTargetFullRejects;
        return RequestStatus_BufferFull;
    }
    seq_req_list.emplace_back(pkt, primary_type,
        secondary_type, curCycle());
    m_outstanding_count++;

    if (seq_req_list.size() > 1) {
        return RequestStatus_Aliased;
    }

    m_outstandReqHist.sample(m_outstanding_count);

    return RequestStatus_Ready;
}

void
Sequencer::markRemoved(SequencerRequest &request)
{
    assert(request.admissionActive);
    request.admissionActive = false;
    m_outstanding_count--;
}

void
Sequencer::recordMissLatency(SequencerRequest* srequest, bool llscSuccess,
                             const MachineType respondingMach,
                             bool isExternalHit, Cycles initialRequestTime,
                             Cycles forwardRequestTime,
                             Cycles firstResponseTime)
{
    RubyRequestType type = srequest->m_type;
    Cycles issued_time = srequest->issue_time;
    Cycles completion_time = curCycle();

    if (type == RubyRequestType_SPMCP_fetch) {
        if (cfAggregateEnabled() && !srequest->pkt->req->isSPMMulticastClaim())
            cfAggregateDone(m_version, srequest->pkt->getAddr());
        accountSPMCPOutstanding(completion_time);
        assert(m_spmcpOutstanding > 0);
        --m_spmcpOutstanding;
        ++m_spmcpRequestsCompleted;
        if (srequest->pkt->req->isSPMMulticastClaim())
            ++m_spmcpClaimRequestsCompleted;
        else
            ++m_spmcpDataRequestsCompleted;
    }

    assert(curCycle() >= issued_time);
    Cycles total_lat = completion_time - issued_time;

    if ((initialRequestTime != 0) && (initialRequestTime < issued_time)) {
        // if the request was combined in the protocol with an earlier request
        // for the same address, it is possible that it will return an
        // initialRequestTime corresponding the earlier request.  Since Cycles
        // is unsigned, we can't let this request get profiled below.

        total_lat = Cycles(0);
    }

    DPRINTFR(ProtocolTrace, "%15s %3s %10s%20s %6s>%-6s %s %d cycles\n",
             curTick(), m_version, "Seq", llscSuccess ? "Done" : "SC_Failed",
             "", "", printAddress(srequest->pkt->getAddr()), total_lat);

    m_latencyHist.sample(total_lat);
    m_typeLatencyHist[type]->sample(total_lat);

    if (isExternalHit) {
        m_missLatencyHist.sample(total_lat);
        m_missTypeLatencyHist[type]->sample(total_lat);

        if (respondingMach != MachineType_NUM) {
            m_missMachLatencyHist[respondingMach]->sample(total_lat);
            m_missTypeMachLatencyHist[type][respondingMach]->sample(total_lat);

            if ((issued_time <= initialRequestTime) &&
                (initialRequestTime <= forwardRequestTime) &&
                (forwardRequestTime <= firstResponseTime) &&
                (firstResponseTime <= completion_time)) {

                m_IssueToInitialDelayHist[respondingMach]->sample(
                    initialRequestTime - issued_time);
                m_InitialToForwardDelayHist[respondingMach]->sample(
                    forwardRequestTime - initialRequestTime);
                m_ForwardToFirstResponseDelayHist[respondingMach]->sample(
                    firstResponseTime - forwardRequestTime);
                m_FirstResponseToCompletionDelayHist[respondingMach]->sample(
                    completion_time - firstResponseTime);
            } else {
                m_IncompleteTimes[respondingMach]++;
            }
        }
    } else {
        m_hitLatencyHist.sample(total_lat);
        m_hitTypeLatencyHist[type]->sample(total_lat);

        if (respondingMach != MachineType_NUM) {
            m_hitMachLatencyHist[respondingMach]->sample(total_lat);
            m_hitTypeMachLatencyHist[type][respondingMach]->sample(total_lat);
        }
    }
}

void
Sequencer::writeCallbackScFail(Addr address, DataBlock& data)
{
    llscClearMonitor(address);
    writeCallback(address, data);
}

void
Sequencer::writeCallback(Addr address, DataBlock& data,
                         const bool externalHit, const MachineType mach,
                         const Cycles initialRequestTime,
                         const Cycles forwardRequestTime,
                         const Cycles firstResponseTime,
                         const bool noCoales)
{
    //
    // Free the whole list as we assume we have had the exclusive access
    // to this cache line when response for the write comes back
    //
    assert(address == makeLineAddress(address));
    if (m_RequestTable.find(address) == m_RequestTable.end()) {
        panic("Ruby write response for %#x has no Sequencer request "
              "(outstanding=%d, spmcpOutstanding=%u)",
              address, m_outstanding_count, m_spmcpOutstanding);
    }
    auto &seq_req_list = m_RequestTable[address];

    // Perform hitCallback on every cpu request made to this cache block while
    // ruby request was outstanding. Since only 1 ruby request was made,
    // profile the ruby latency once.
    // An SPM store has already updated the private L2 array. Its response is
    // an acknowledgment, not a writable L0 cache line. Followers must visit
    // the controller themselves (including loads after the store).
    const bool spm_store_ack =
        seq_req_list.front().pkt->req->isSPMRequest();
    bool ruby_request = true;
    while (!seq_req_list.empty()) {
        SequencerRequest &seq_req = seq_req_list.front();
        // Atomic Request may be executed remotly in the cache hierarchy
        bool atomic_req =
           ((seq_req.m_type == RubyRequestType_ATOMIC_RETURN) ||
            (seq_req.m_type == RubyRequestType_ATOMIC_NO_RETURN));

        // A bridge queued behind an ordinary store still needs its own
        // controller transaction: a coherent-line response has not installed
        // its SPM destination. Never acknowledge that side effect by aliasing.
        const bool spm_follower = seq_req.pkt->req->isSPMRequest();
        if ((noCoales || atomic_req || spm_store_ack || spm_follower) &&
            !ruby_request) {
            // Do not process follow-up requests
            // (e.g. if full line no present)
            // Reissue to the cache hierarchy
            issueRequest(seq_req.pkt, seq_req.m_second_type);
            break;
        }

        if (ruby_request) {
            assert(seq_req.m_type != RubyRequestType_LD);
            assert(seq_req.m_type != RubyRequestType_Load_Linked);
            assert(seq_req.m_type != RubyRequestType_IFETCH);
            assert(seq_req.m_type != RubyRequestType_ATOMIC_RETURN);
            assert(seq_req.m_type != RubyRequestType_ATOMIC_NO_RETURN);
        }

        // handle write request
        if ((seq_req.m_type != RubyRequestType_LD) &&
            (seq_req.m_type != RubyRequestType_Load_Linked) &&
            (seq_req.m_type != RubyRequestType_IFETCH)) {
            // LL/SC support (tested with ARMv8)
            bool success = true;

            if (seq_req.m_type != RubyRequestType_Store_Conditional) {
                // Regular stores to addresses being monitored
                // will fail (remove) the monitor entry.
                llscClearMonitor(address);
            } else {
                // Store conditionals must first check the monitor
                // if they will succeed or not
                success = llscStoreConditional(address);
                seq_req.pkt->req->setExtraData(success ? 1 : 0);
            }

            // Handle SLICC block_on behavior for Locked_RMW accesses. NOTE: the
            // address variable here is assumed to be a line address, so when
            // blocking buffers, must check line addresses.
            if (seq_req.m_type == RubyRequestType_Locked_RMW_Read) {
                // blockOnQueue blocks all first-level cache controller queues
                // waiting on memory accesses for the specified address that go
                // to the specified queue. In this case, a Locked_RMW_Write must
                // go to the mandatory_q before unblocking the first-level
                // controller. This will block standard loads, stores, ifetches,
                // etc.
                m_controller->blockOnQueue(address, m_mandatory_q_ptr);
            } else if (seq_req.m_type == RubyRequestType_Locked_RMW_Write) {
                m_controller->unblock(address);
            }

            if (ruby_request) {
                recordMissLatency(&seq_req, success, mach, externalHit,
                                  initialRequestTime, forwardRequestTime,
                                  firstResponseTime);
            }

            markRemoved(seq_req);
            hitCallback(&seq_req, data, success, mach, externalHit,
                        initialRequestTime, forwardRequestTime,
                        firstResponseTime, !ruby_request);
            ruby_request = false;
        } else {
            // handle read request
            assert(!ruby_request);
            markRemoved(seq_req);
            hitCallback(&seq_req, data, true, mach, externalHit,
                        initialRequestTime, forwardRequestTime,
                        firstResponseTime, !ruby_request);
        }
        seq_req_list.pop_front();
    }

    // free all outstanding requests corresponding to this address
    if (seq_req_list.empty()) {
        m_RequestTable.erase(address);
    }
}

bool
Sequencer::processReadCallback(SequencerRequest &seq_req,
                               DataBlock& data,
                               const bool ruby_request,
                               bool externalHit,
                               const MachineType mach,
                               Cycles initialRequestTime,
                               Cycles forwardRequestTime,
                               Cycles firstResponseTime)
{
    if (ruby_request) {
        assert(isReadRequest(seq_req.m_type)); // C6
    }
    if ((seq_req.m_type != RubyRequestType_LD) &&
        (seq_req.m_type != RubyRequestType_Load_Linked) &&
        (seq_req.m_type != RubyRequestType_IFETCH) &&
        (seq_req.m_type != RubyRequestType_SPMLD) &&       // C6
        (seq_req.m_type != RubyRequestType_SPMWB_read) &&  // C6
        (seq_req.m_type != RubyRequestType_REPLACEMENT)) {
        // Write request: reissue request to the cache hierarchy
        issueRequest(seq_req.pkt, seq_req.m_second_type);
        return true;
    }
    return false;

}

void
Sequencer::readCallback(Addr address, DataBlock& data,
                        bool externalHit, const MachineType mach,
                        Cycles initialRequestTime,
                        Cycles forwardRequestTime,
                        Cycles firstResponseTime)
{
    //
    // Free up read requests until we hit the first Write request
    // or end of the corresponding list.
    //
    assert(address == makeLineAddress(address));
    assert(m_RequestTable.find(address) != m_RequestTable.end());
    auto &seq_req_list = m_RequestTable[address];

    // Perform hitCallback on every cpu request made to this cache block while
    // ruby request was outstanding. Since only 1 ruby request was made,
    // profile the ruby latency once.
    bool ruby_request = true;
    while (!seq_req_list.empty()) {
        SequencerRequest &seq_req = seq_req_list.front();
        if (processReadCallback(seq_req, data, ruby_request, externalHit, mach,
                                initialRequestTime, forwardRequestTime,
                                firstResponseTime)) {
            break;
        }
        if (ruby_request) {
            recordMissLatency(&seq_req, true, mach, externalHit,
                              initialRequestTime, forwardRequestTime,
                              firstResponseTime);
        }
        markRemoved(seq_req);
        hitCallback(&seq_req, data, true, mach, externalHit,
                    initialRequestTime, forwardRequestTime,
                    firstResponseTime, !ruby_request);
        ruby_request = false;
        seq_req_list.pop_front();
    }

    // free all outstanding requests corresponding to this address
    if (seq_req_list.empty()) {
        m_RequestTable.erase(address);
    }
}

void
Sequencer::atomicCallback(Addr address, DataBlock& data,
                         const bool externalHit, const MachineType mach,
                         const Cycles initialRequestTime,
                         const Cycles forwardRequestTime,
                         const Cycles firstResponseTime)
{
    //
    // Free the first request (an atomic operation) from the list.
    // Then issue the next request to ruby system as we cannot
    // assume the cache line is present in the cache
    // (the opperation could be performed remotly)
    //
    assert(address == makeLineAddress(address));
    assert(m_RequestTable.find(address) != m_RequestTable.end());
    auto &seq_req_list = m_RequestTable[address];

    // Perform hitCallback only on the first cpu request that
    // issued the ruby request
    bool ruby_request = true;
    while (!seq_req_list.empty()) {
        SequencerRequest &seq_req = seq_req_list.front();

        if (ruby_request) {
            // Check that the request was an atomic memory operation
            // and record the latency
            assert((seq_req.m_type == RubyRequestType_ATOMIC_RETURN) ||
                   (seq_req.m_type == RubyRequestType_ATOMIC_NO_RETURN));
            recordMissLatency(&seq_req, true, mach, externalHit,
                              initialRequestTime, forwardRequestTime,
                              firstResponseTime);
        } else {
            // Read, Write or Atomic request:
            // reissue request to the cache hierarchy
            // (we don't know if op was performed remotly)
            issueRequest(seq_req.pkt, seq_req.m_second_type);
            break;
        }

        // Atomics clean the monitor entry
        llscClearMonitor(address);

        markRemoved(seq_req);
        ruby_request = false;
        hitCallback(&seq_req, data, true, mach, externalHit,
                    initialRequestTime, forwardRequestTime,
                    firstResponseTime, false);
        seq_req_list.pop_front();
    }

    // free all outstanding requests corresponding to this address
    if (seq_req_list.empty()) {
        m_RequestTable.erase(address);
    }
}

void
Sequencer::hitCallback(SequencerRequest* srequest, DataBlock& data,
                       bool llscSuccess,
                       const MachineType mach, const bool externalHit,
                       const Cycles initialRequestTime,
                       const Cycles forwardRequestTime,
                       const Cycles firstResponseTime,
                       const bool was_coalesced)
{
    warn_once("Replacement policy updates recently became the responsibility "
              "of SLICC state machines. Make sure to setMRU() near callbacks "
              "in .sm files!");

    PacketPtr pkt = srequest->pkt;
    auto oracle = m_earlyReadSnapshots.find(pkt);
    if (oracle != m_earlyReadSnapshots.end()) {
        // The CPU already received a private load value. Its real cache
        // transaction retained normal admission, miss/coalescing resources,
        // fill/replacement and traffic. Verify before discarding the late reply.
        const auto &expected = oracle->second;
        panic_if(std::memcmp(expected.data(),
                 data.getData(pkt->getAddr() & 63, pkt->getSize()),
                 pkt->getSize()) != 0,
                 "Early accumulator read differed from coherent response pc=%#x addr=%#x",
                 pkt->req->getPC(), pkt->getAddr());
        ++m_earlyReadVerified;
        m_earlyReadSnapshots.erase(oracle);
        delete pkt;
        trySendRetries();
        testDrainComplete();
        return;
    }
    Addr request_address(pkt->getAddr());
    CFL_PKT("SEQ_DONE", pkt, "source=%s external=%d coalesced=%d",
            MachineType_to_string(mach), externalHit, was_coalesced);
    DPRINTF(CacheFlexResidency,
            "CFRES DONE id=%p addr=%#x va=%#x pc=%#x bytes=%u type=%s source=%s external=%d\n",
            pkt, request_address, pkt->req->hasVaddr() ? pkt->req->getVaddr() : 0,
            pkt->req->hasPC() ? pkt->req->getPC() : 0, pkt->getSize(),
            RubyRequestType_to_string(srequest->m_type),
            MachineType_to_string(mach), externalHit);
    RubyRequestType type = srequest->m_type;

    if (was_coalesced) {
        // Notify the controller about a coalesced request so it can properly
        // account for it in its hit/miss stats and/or train prefetchers
        // (this is protocol-dependent)
        m_controller->notifyCoalesced(request_address, type, pkt->req,
                                      data, externalHit);
    }

    // Load-linked handling
    if (type == RubyRequestType_Load_Linked) {
        Addr line_addr = makeLineAddress(request_address);
        llscLoadLinked(line_addr);
    }

    DPRINTF(RubyHitMiss, "Cache %s at %#x\n",
                         externalHit ? "miss" : "hit",
                         printAddress(request_address));

    // update the data unless it is a non-data-carrying flush
    if (m_ruby_system->getWarmupEnabled()) {
        data.setData(pkt);
    } else if (!pkt->isFlush() && !pkt->isCleanInvalidateRequest()) {
        if ((type == RubyRequestType_LD) ||
            (type == RubyRequestType_IFETCH) ||
            (type == RubyRequestType_RMW_Read) ||
            (type == RubyRequestType_Locked_RMW_Read) ||
            (type == RubyRequestType_Load_Linked) ||
            (type == RubyRequestType_ATOMIC_RETURN) ||
            (type == RubyRequestType_SPMLD) ||       // C6
            (type == RubyRequestType_SPMWB_read)) {  // C6

            // For software prefetches, since we've already sent an early
            // response back to the core, we can just ignore this
            if (pkt->cmd.isSWPrefetch()) {
                // markRemoved() has already released this request's credit.
                // Unlike a demand response, the early-ack prefetch path does
                // not enter ruby_hit_callback(), which normally wakes retry
                // ports. If the last outstanding work consists of prefetches,
                // omitting this wakeup can strand both demand and fetch ports
                // after BufferFull, even with an empty sequencer.
                auto *sender = safe_cast<RubyPort::SenderState *>(
                    pkt->popSenderState());
                delete sender;
                delete pkt;
                trySendRetries();
                return;
            }

            pkt->setData(
                data.getData(getOffset(request_address), pkt->getSize()));

           if (type == RubyRequestType_ATOMIC_RETURN) {
               DPRINTF(RubySequencer, "ATOMIC RETURN data %s\n", data);
           } else {
               DPRINTF(RubySequencer, "read data %s\n", data);
           }
        } else if (type == RubyRequestType_SPMST) {
            // SLICC writeSPMData() performed the write before this ack.
            // In particular a vector store can exceed the 64B ack DataBlock.
            // Do not apply it again to that temporary response buffer.
        } else if (pkt->req->isSwap()) {
            assert(!pkt->isMaskedWrite());
            std::vector<uint8_t> overwrite_val(pkt->getSize());
            pkt->writeData(&overwrite_val[0]);
            pkt->setData(
                data.getData(getOffset(request_address), pkt->getSize()));
            data.setData(&overwrite_val[0],
                         getOffset(request_address), pkt->getSize());
            DPRINTF(RubySequencer, "swap data %s\n", data);
        } else if (pkt->isAtomicOp()) {
            // Set the data in the packet to the old value in the cache
            pkt->setData(
                data.getData(getOffset(request_address), pkt->getSize()));
            DPRINTF(RubySequencer, "AMO original data %s\n", data);
            // execute AMO operation
            (*(pkt->getAtomicOp()))(
                data.getDataMod(getOffset(request_address)));
            DPRINTF(RubySequencer, "AMO new data %s\n", data);
        } else if (type != RubyRequestType_Store_Conditional || llscSuccess) {
            // Types of stores set the actual data here, apart from
            // failed Store Conditional requests
            data.setData(pkt);
            DPRINTF(RubySequencer, "set data %s\n", data);
        }
    }

    if (pkt == reductionMemoryPacket) {
        reductionMemoryDone(pkt);
        return;
    }
    if (l3ReductionFlight.count(pkt)) {
        completeL3Reduction(pkt);
        return;
    }

    // If using the RubyTester, update the RubyTester sender state's
    // subBlock with the recieved data.  The tester will later access
    // this state.
    if (m_usingRubyTester) {
        DPRINTF(RubySequencer, "hitCallback %s 0x%x using RubyTester\n",
                pkt->cmdString(), pkt->getAddr());
        RubyTester::SenderState* testerSenderState =
            pkt->findNextSenderState<RubyTester::SenderState>();
        assert(testerSenderState);
        testerSenderState->subBlock.mergeFrom(data);
    }

    RubySystem *rs = m_ruby_system;
    if (m_ruby_system->getWarmupEnabled()) {
        assert(pkt->req);
        delete pkt;
        rs->m_cache_recorder->enqueueNextFetchRequest();
    } else if (m_ruby_system->getCooldownEnabled()) {
        delete pkt;
        rs->m_cache_recorder->enqueueNextFlushRequest();
    } else {
        ruby_hit_callback(pkt);
        testDrainComplete();
    }
}

void
Sequencer::unaddressedCallback(Addr unaddressedReqId,
                               RubyRequestType reqType,
                               const MachineType mach,
                               const Cycles initialRequestTime,
                               const Cycles forwardRequestTime,
                               const Cycles firstResponseTime)
{
    DPRINTF(RubySequencer, "unaddressedCallback ID:%08x type:%d\n",
            unaddressedReqId, reqType);

    switch (reqType) {
      case RubyRequestType_TLBI_EXT_SYNC:
      {
        // This should trigger the CPU to wait for stale translations
        // and send an EXT_SYNC_COMP once complete.

        // Don't look for the ID in our requestTable.
        // It won't be there because we didn't request this Sync
        ruby_stale_translation_callback(unaddressedReqId);
        break;
      }
      case RubyRequestType_TLBI:
      case RubyRequestType_TLBI_SYNC:
      {
        // These signal that a TLBI operation that this core initiated
        // of the respective type (TLBI or Sync) has finished.

        assert(m_UnaddressedRequestTable.find(unaddressedReqId)
               != m_UnaddressedRequestTable.end());

        {
            SequencerRequest &seq_req =
                m_UnaddressedRequestTable.at(unaddressedReqId);
            assert(seq_req.m_type == reqType);

            PacketPtr pkt = seq_req.pkt;

            ruby_unaddressed_callback(pkt);
            testDrainComplete();
        }

        m_UnaddressedRequestTable.erase(unaddressedReqId);
        break;
      }
      default:
        panic("Unexpected TLBI RubyRequestType");
    }
}

void
Sequencer::completeHitCallback(std::vector<PacketPtr> & mylist)
{
    for (auto& pkt : mylist) {
        // When Ruby is in warmup or cooldown phase, the requests come
        // from the cache recorder. They do not track which port to use
        // and do not need to send the response back
        if (!m_ruby_system->getWarmupEnabled()
                && !m_ruby_system->getCooldownEnabled()) {
            RubyPort::SenderState *ss =
                safe_cast<RubyPort::SenderState *>(pkt->senderState);
            MemResponsePort *port = ss->port;
            assert(port != NULL);

            pkt->senderState = ss->predecessor;

            if (pkt->cmd != MemCmd::WriteReq) {
                // for WriteReq, we keep the original senderState until
                // writeCompleteCallback
                delete ss;
            }

            port->hitCallback(pkt);
            trySendRetries();
        }
    }

    RubySystem *rs = m_ruby_system;
    if (m_ruby_system->getWarmupEnabled()) {
        rs->m_cache_recorder->enqueueNextFetchRequest();
    } else if (m_ruby_system->getCooldownEnabled()) {
        rs->m_cache_recorder->enqueueNextFlushRequest();
    } else {
        testDrainComplete();
    }
}

void
Sequencer::invL1Callback()
{
    // Since L1 invalidate is currently done with paddr = 0
    assert(m_cache_inv_pkt && m_num_pending_invs > 0);

    m_num_pending_invs--;

    if (m_num_pending_invs == 0) {
        std::vector<PacketPtr> pkt_list { m_cache_inv_pkt };
        m_cache_inv_pkt = nullptr;
        completeHitCallback(pkt_list);
    }
}

void
Sequencer::invL1()
{
    int size = m_dataCache_ptr->getNumBlocks();
    DPRINTF(RubySequencer,
            "There are %d Invalidations outstanding before Cache Walk\n",
            m_num_pending_invs);
    // Walk the cache
    for (int i = 0; i < size; i++) {
        Addr addr = m_dataCache_ptr->getAddressAtIdx(i);
        // Evict Read-only data
        RubyRequestType request_type = RubyRequestType_REPLACEMENT;
        std::shared_ptr<RubyRequest> msg = std::make_shared<RubyRequest>(
            clockEdge(), m_ruby_system->getBlockSizeBytes(), m_ruby_system,
            addr, 0, 0, request_type, RubyAccessMode_Supervisor,
            nullptr);
        DPRINTF(RubySequencer, "Evicting addr 0x%x\n", addr);
        assert(m_mandatory_q_ptr != NULL);
        Tick latency = cyclesToTicks(
            m_controller->mandatoryQueueLatency(request_type));
        m_mandatory_q_ptr->enqueue(msg, clockEdge(), latency,
                                   m_ruby_system->getRandomization(),
                                   m_ruby_system->getWarmupEnabled());
        m_num_pending_invs++;
    }
    DPRINTF(RubySequencer,
            "There are %d Invalidations outstanding after Cache Walk\n",
            m_num_pending_invs);
}

bool
Sequencer::empty() const
{
    return reductionSource.empty() && reductionSink.empty() && l3ReductionFlight.empty() && m_RequestTable.empty() &&
           m_UnaddressedRequestTable.empty();
}

MessageBuffer *
Sequencer::ingressQueue(PacketPtr pkt) const
{
    const bool pure = pkt->req->isSPMLD() || pkt->req->isSPMST() ||
                      pkt->req->isSPMWBRead();
    return pure && m_micro_spm_ingress ? m_micro_spm_ingress : m_mandatory_q_ptr;
}

RequestStatus
Sequencer::makeRequest(PacketPtr pkt)
{
    if (pkt != reductionMemoryPacket && !l3ReductionFlight.count(pkt) && pkt->req->isSPMRequest()) {
        if (pkt->req->isSPMST() && (pkt->getAddr() & cfReduction::Command)) {
            panic_if(!reductionEnabled, "Reduction command sent to disabled engine");
            return acceptReduction(pkt);
        }
        if (reductionEnabled && reductionConflicts(pkt)) {
            ++reductionOrderingRetries;
            m_ingress_retry = true;
            scheduleIngress();
            return RequestStatus_BufferFull;
        }
    }
    if (m_idealStagingCache) {
        // Complete through the normal CPU response path on the next clock
        // edge, never re-entrantly during sendTimingReq. Until then keep
        // conflicting operations out of the ordinary protocol queues.
        bool conflict = m_idealStagingSources.count(makeLineAddress(pkt->getAddr()));
        if (pkt->req->isSPMRequest()) {
            const Addr slot = makeLineAddress(canonicalSPMAddress(pkt->req->getSPMDstAddr()));
            const int lines = (pkt->req->isSPMLD() || pkt->req->isSPMST()) ?
                (pkt->getSize() + 63) / 64 : 1;
            for (int i = 0; i < lines; ++i)
                conflict |= m_idealStagingSlots.count(slot + (Addr(i) << 16));
        }
        if (conflict) {
            ++m_idealStagingOrderingRetries;
            m_ingress_retry = true;
            scheduleIngress();
            return RequestStatus_BufferFull;
        }
        if (pkt->req->isSPMCPFetch() && tryIdealStaging(pkt))
            return RequestStatus_Issued;
    }
    // Classic's coherent CPU port blocks when its actual miss/writeback
    // structures fill. Its separate SPM port does not share that blocked bit.
    const bool pure = pkt->req->isSPMLD() || pkt->req->isSPMST() ||
                      pkt->req->isSPMWBRead();
    auto *misses = pkt->req->isInstFetch() ? m_micro_inst_misses : m_micro_data_misses;
    // Retain the original short-circuit checks: full() also updates statistics,
    // so observation must reuse the result rather than checking a second time.
    const bool check_micro_resources = !pure && !pkt->req->isHTMAbort();
    const bool micro_misses_full = check_micro_resources && misses && misses->full();
    const bool micro_wb_full = check_micro_resources && !micro_misses_full &&
        !pkt->req->isInstFetch() && m_micro_writebacks && m_micro_writebacks->full();
    if (micro_misses_full || micro_wb_full) {
        CFL_PKT("SEQ_REJECT", pkt, "reason=miss_or_writeback_pool misses_full=%d wb_full=%d",
                micro_misses_full, micro_wb_full);
        ++m_ingressFullRejects;
        m_ingress_retry = true;
        scheduleIngress();
        return RequestStatus_BufferFull;
    }
    if (m_finite_ingress && !pkt->req->isHTMAbort() &&
        (!m_ingress_replays.empty() ||
         !ingressQueue(pkt)->areNSlotsAvailable(1, clockEdge()))) {
        CFL_PKT("SEQ_REJECT", pkt, "reason=ingress_full_or_replay");
        ++m_ingressFullRejects;
        m_ingress_retry = true;
        scheduleIngress();
        return RequestStatus_BufferFull;
    }
    const bool is_spmcp = pkt->req->isSPMCPFetch();
    if (is_spmcp)
        ++m_spmcpRequestAttempts;

    // HTM abort signals must be allowed to reach the Sequencer
    // the same cycle they are issued. They cannot be retried.
    bool admission_full = m_outstanding_count >= m_max_outstanding_requests;
    if (admission_full && !pkt->req->isHTMAbort())
        ++m_globalAdmissionFullRejects;
    if (m_cacheflex_fetch_credits || m_cacheflex_bridge_credits ||
        m_cacheflex_max_bridge_requests) {
        // Count live request records, not just distinct cache lines: aliased
        // transactions occupy credits too. Unaddressed requests remain in the
        // shared domain via the residual count. Diagnostic policy only; no
        // requests bypass ordering, coherence or downstream resource limits.
        int fetch = 0, bridge = 0;
        for (const auto &entry : m_RequestTable) {
            for (const auto &req : entry.second) {
                if (!req.admissionActive)
                    continue;
                if (req.pkt->req->isInstFetch())
                    ++fetch;
                else if (req.pkt->req->isSPMCPFetch() ||
                         req.pkt->req->isSPMWBStore())
                    ++bridge;
            }
        }
        const int shared = m_outstanding_count -
            (m_cacheflex_fetch_credits ? fetch : 0) -
            (m_cacheflex_bridge_credits ? bridge : 0);
        if (m_cacheflex_max_bridge_requests &&
            (is_spmcp || pkt->req->isSPMWBStore())) {
            admission_full |= bridge >= m_cacheflex_max_bridge_requests;
        }
        if (m_cacheflex_fetch_credits && pkt->req->isInstFetch())
            admission_full |= fetch >= m_cacheflex_fetch_credits;
        else if (m_cacheflex_bridge_credits &&
                 (is_spmcp || pkt->req->isSPMWBStore()))
            admission_full |= bridge >= m_cacheflex_bridge_credits;
        else
            admission_full |= shared >= m_max_outstanding_requests -
                m_cacheflex_fetch_credits - m_cacheflex_bridge_credits;
    }
    if (admission_full &&
        !pkt->req->isHTMAbort()) {
        CFL_PKT("SEQ_REJECT", pkt, "reason=global_or_partition_cap");
        if (is_spmcp)
            ++m_spmcpBufferFullRejects;
        return RequestStatus_BufferFull;
    }

    RubyRequestType primary_type = RubyRequestType_NULL;
    RubyRequestType secondary_type = RubyRequestType_NULL;

    if (pkt->isLLSC()) {
        // LL/SC instructions need to be handled carefully by the cache
        // coherence protocol to ensure they follow the proper semantics. In
        // particular, by identifying the operations as atomic, the protocol
        // should understand that migratory sharing optimizations should not
        // be performed (i.e. a load between the LL and SC should not steal
        // away exclusive permission).
        //
        // The following logic works correctly with the semantics
        // of armV8 LDEX/STEX instructions.
        const ProtocolInfo &protocol_info = m_ruby_system->getProtocolInfo();

        if (pkt->isWrite()) {
            DPRINTF(RubySequencer, "Issuing SC\n");
            primary_type = RubyRequestType_Store_Conditional;
            if (protocol_info.getUseSecondaryStoreConditional()) {
                secondary_type = RubyRequestType_Store_Conditional;
            } else {
                secondary_type = RubyRequestType_ST;
            }
        } else {
            DPRINTF(RubySequencer, "Issuing LL\n");
            assert(pkt->isRead());
            primary_type = RubyRequestType_Load_Linked;
            if (protocol_info.getUseSecondaryLoadLinked()) {
                secondary_type = RubyRequestType_Load_Linked;
            } else {
                secondary_type = RubyRequestType_LD;
            }
        }
    } else if (pkt->req->isLockedRMW()) {
        //
        // x86 locked instructions are translated to store cache coherence
        // requests because these requests should always be treated as read
        // exclusive operations and should leverage any migratory sharing
        // optimization built into the protocol.
        //
        if (pkt->isWrite()) {
            DPRINTF(RubySequencer, "Issuing Locked RMW Write\n");
            primary_type = RubyRequestType_Locked_RMW_Write;
        } else {
            DPRINTF(RubySequencer, "Issuing Locked RMW Read\n");
            assert(pkt->isRead());
            primary_type = RubyRequestType_Locked_RMW_Read;
        }
        secondary_type = RubyRequestType_ST;
    } else if (pkt->req->isTlbiCmd()) {
        primary_type = secondary_type = tlbiCmdToRubyRequestType(pkt);
        DPRINTF(RubySequencer, "Issuing TLBI\n");
#if defined (PROTOCOL_CHI)
    } else if (pkt->isAtomicOp()) {
        if (pkt->req->isAtomicReturn()){
            DPRINTF(RubySequencer, "Issuing ATOMIC RETURN \n");
            primary_type = secondary_type =
                           RubyRequestType_ATOMIC_RETURN;
        } else {
            DPRINTF(RubySequencer, "Issuing ATOMIC NO RETURN\n");
            primary_type = secondary_type =
                           RubyRequestType_ATOMIC_NO_RETURN;

        }
#endif
    } else if (pkt->req->hasNoAddr()) {
        primary_type = secondary_type = RubyRequestType_hasNoAddr;
    } else if (pkt->req->isSPMCPFetch()) { // C6
        primary_type = secondary_type = RubyRequestType_SPMCP_fetch; // C6
    } else if (pkt->req->isSPMCPInstall()) { // C6
        primary_type = secondary_type = RubyRequestType_SPMCP_install; // C6
    } else if (pkt->req->isSPMLD()) { // C6
        primary_type = secondary_type = RubyRequestType_SPMLD; // C6
    } else if (pkt->req->isSPMST()) { // C6
        primary_type = secondary_type = RubyRequestType_SPMST; // C6
    } else if (pkt->req->isSPMWBRead()) { // C6
        primary_type = secondary_type = RubyRequestType_SPMWB_read; // C6
    } else if (pkt->req->isSPMWBStore()) { // C6
        primary_type = secondary_type = RubyRequestType_SPMWB_store; // C6
    } else if (pkt->req->isSPMRelease()) { // C6
        primary_type = secondary_type = RubyRequestType_SPM_release; // C6
    } else {
        //
        // To support SwapReq, we need to check isWrite() first: a SwapReq
        // should always be treated like a write, but since a SwapReq implies
        // both isWrite() and isRead() are true, check isWrite() first here.
        //
        if (pkt->isWrite()) {
            //
            // Note: M5 packets do not differentiate ST from RMW_Write
            //
            primary_type = secondary_type = RubyRequestType_ST;
        } else if (pkt->isRead()) {
            // hardware transactional memory commands
            if (pkt->req->isHTMCmd()) {
                primary_type = secondary_type = htmCmdToRubyRequestType(pkt);
            } else if (pkt->req->isInstFetch()) {
                primary_type = secondary_type = RubyRequestType_IFETCH;
            } else {
                if (pkt->req->isReadModifyWrite()) {
                    primary_type = RubyRequestType_RMW_Read;
                    secondary_type = RubyRequestType_ST;
                } else {
                    primary_type = secondary_type = RubyRequestType_LD;
                }
            }
        } else if (pkt->isFlush() || pkt->isCleanInvalidateRequest()) {
            primary_type = secondary_type = RubyRequestType_FLUSH;
        } else if (pkt->cmd == MemCmd::MemSyncReq) {
            primary_type = secondary_type = RubyRequestType_REPLACEMENT;
            assert(!m_cache_inv_pkt);
            m_cache_inv_pkt = pkt;
            invL1();
        } else {
            panic("Cannot convert packet [%s] to ruby request\n",
                  pkt->print());
        }
    }

    // Check if the line is blocked for a Locked_RMW
    if (!pkt->req->isMemMgmt() &&
        m_controller->isBlocked(makeLineAddress(pkt->getAddr())) &&
        (primary_type != RubyRequestType_Locked_RMW_Write)) {
        // Return that this request's cache line address aliases with
        // a prior request that locked the cache line. The request cannot
        // proceed until the cache line is unlocked by a Locked_RMW_Write
        CFL_PKT("SEQ_REJECT", pkt, "reason=locked_line");
        if (is_spmcp)
            ++m_spmcpBlockedLineRejects;
        return RequestStatus_Aliased;
    }

    // Only after normal global/ingress/ordering admission has succeeded.
    if (!m_earlyReadPcs.empty() && primary_type == RubyRequestType_LD &&
        secondary_type == RubyRequestType_LD && pkt->req->hasPC() &&
        m_earlyReadPcs.count(pkt->req->getPC()) && tryEarlyRead(pkt))
        return RequestStatus_Issued;

    RequestStatus status = insertRequest(pkt, primary_type, secondary_type);
    CFL_PKT("SEQ_STATUS", pkt, "status=%s", RequestStatus_to_string(status));

    if (is_spmcp && status == RequestStatus_Aliased) {
        ++m_spmcpAliasedRequests;
        if (pkt->req->isSPMMulticastClaim())
            ++m_spmcpClaimAliasedRequests;
        else
            ++m_spmcpDataAliasedRequests;
    }

    // It is OK to receive RequestStatus_Aliased, it can be considered Issued
    if (status != RequestStatus_Ready && status != RequestStatus_Aliased)
        return status;
    // non-aliased with any existing request in the request table, just issue
    // to the cache
    if (status != RequestStatus_Aliased)
        issueRequest(pkt, secondary_type);

    // TODO: issue hardware prefetches here
    return RequestStatus_Issued;
}

bool
Sequencer::tryEarlyRead(PacketPtr pkt)
{
    ++m_earlyReadEligible;
    const Addr line = makeLineAddress(pkt->getAddr());
    // This oracle is restricted to ordinary, unmasked private data reads.
    if (pkt->cmd != MemCmd::ReadReq || pkt->req->isMasked() ||
        pkt->req->isSPMRequest() || pkt->req->isInstFetch() ||
        (pkt->getAddr() & 63) + pkt->getSize() > 64)
        return false;
    if (m_RequestTable.count(line)) {
        ++m_earlyReadFallbackAlias;
        return false;
    }
    // A separate owning packet: never retain CPU sender state/data after its
    // early response has released the LSQ request. Request metadata is shared.
    auto *background = new Packet(pkt, true, false);
    background->senderState = nullptr;
    background->allocate();
    auto req = std::make_shared<Request>(pkt->getAddr(), pkt->getSize(), 0,
                                        m_controller->getRequestorId());
    Packet probe(req, MemCmd::ReadReq);
    probe.allocate();
    if (!m_ruby_system->functionalRead(&probe)) {
        ++m_earlyReadFallbackFunctional;
        delete background;
        return false;
    }
    auto status = insertRequest(background, RubyRequestType_LD, RubyRequestType_LD);
    if (status != RequestStatus_Ready) {
        panic_if(status == RequestStatus_Aliased, "Oracle unexpectedly coalesced");
        delete background;
        return false;
    }
    std::vector<uint8_t> snapshot(probe.getConstPtr<uint8_t>(),
                                 probe.getConstPtr<uint8_t>() + pkt->getSize());
    pkt->setData(snapshot.data());
    m_earlyReadSnapshots.emplace(background, std::move(snapshot));
    ++m_earlyReadIssued;
    issueRequest(background, RubyRequestType_LD);
    schedule(new EventFunctionWrapper([this, pkt] {
        ruby_hit_callback(pkt);
    }, name() + ".accumulatorEarlyRead", true), clockEdge(Cycles(1)));
    return true;
}

bool
Sequencer::tryIdealStaging(PacketPtr pkt)
{
    // A functional oracle, NOT a coherent DMA implementation. Source-side
    // residency is left unchanged. Only plain full-line copies are supported;
    // all first claims/evictions and exceptional operations retain the real
    // protocol. CPU issue/commit, translation and SPM hazards remain intact.
    if (pkt->req->isSPMMulticastClaim() || pkt->req->getSPMMulticastMask() ||
        pkt->req->isSPMDma2D() || pkt->req->isSPMCPBatch3() ||
        // Scalar bridge packets carry an eight-byte SLOT operand, not
        // the data payload. SLICC also copies the entire 64-byte DataBlk.
        pkt->getSize() != sizeof(Addr) || (pkt->getAddr() & 63) ||
        (pkt->req->getSPMDstAddr() & 63))
        return false;
    const Addr src = makeLineAddress(pkt->getAddr());
    const Addr dst = canonicalSPMAddress(pkt->req->getSPMDstAddr());
    auto *entry = m_idealStagingCache->lookup(dst);
    if (!entry || !entry->isScratchpad() ||
        m_idealStagingController->getAccessPermission(dst) != AccessPermission_Read_Write) {
        ++m_idealStagingFallbackSlot;
        return false;
    }
    // In-flight ordinary/bridge accesses may own a newer source value or
    // an older destination value. Never bypass them with functional access.
    bool alias = m_RequestTable.count(src) || m_RequestTable.count(dst) ||
                 m_controller->isBlocked(src);
    for (const auto &line : m_RequestTable) {
        for (const auto &r : line.second) {
            if (r.admissionActive && r.pkt->req->isSPMCPFetch() &&
                canonicalSPMAddress(r.pkt->req->getSPMDstAddr()) == dst)
                alias = true;
        }
    }
    if (alias || m_outstanding_count >= m_max_outstanding_requests) {
        ++m_idealStagingFallbackAlias;
        return false;
    }
    auto req = std::make_shared<Request>(src, 64, 0, m_controller->getRequestorId());
    Packet read(req, MemCmd::ReadReq);
    read.allocate();
    // Never substitute physical backing memory: dirty coherent owners and
    // buffered responses must be visible through Ruby's functional lookup.
    if (!m_ruby_system->functionalRead(&read)) {
        ++m_idealStagingFallbackRead;
        return false;
    }
    const auto status = insertRequest(pkt, RubyRequestType_SPMCP_fetch,
                                     RubyRequestType_SPMCP_fetch);
    panic_if(status != RequestStatus_Ready, "Oracle copy unexpectedly aliased");
    entry->getDataBlk().setData(read.getConstPtr<uint8_t>(), 0, 64);
    m_idealStagingSources.insert(src);
    m_idealStagingSlots.insert(dst);
    ++m_idealStagingCopies;
    ++m_spmcpRequestAttempts;
    accountSPMCPOutstanding(curCycle());
    ++m_spmcpOutstanding;
    ++m_spmcpRequestsIssued;
    ++m_spmcpDataRequestsIssued;
    if (m_spmcpOutstanding > m_spmcpMaxOutstanding.value())
        m_spmcpMaxOutstanding = m_spmcpOutstanding;
    schedule(new EventFunctionWrapper([this, pkt, dst] {
        finishIdealStaging(pkt, dst);
    }, name() + ".idealStagingDone", true), clockEdge(Cycles(1)));
    return true;
}

void
Sequencer::finishIdealStaging(PacketPtr pkt, Addr slot)
{
    const Addr src = makeLineAddress(pkt->getAddr());
    auto it = m_RequestTable.find(src);
    panic_if(it == m_RequestTable.end() || it->second.size() != 1 ||
             it->second.front().pkt != pkt, "Oracle copy lost unique tracking");
    auto &r = it->second.front();
    recordMissLatency(&r, true, MachineType_NUM, false, Cycles(0), Cycles(0), Cycles(0));
    markRemoved(r);
    m_RequestTable.erase(it);
    m_idealStagingSources.erase(src);
    m_idealStagingSlots.erase(slot);
    ruby_hit_callback(pkt);
}

void
Sequencer::scheduleIngress()
{
    if (!ingressEvent.scheduled())
        schedule(ingressEvent, clockEdge(Cycles(1)));
}

void
Sequencer::serviceIngress()
{
    while (!m_ingress_replays.empty() &&
           ingressQueue(m_ingress_replays.front().first)->areNSlotsAvailable(1, clockEdge())) {
        auto entry = m_ingress_replays.front();
        m_ingress_replays.pop_front();
        issueRequest(entry.first, entry.second);
    }
    if (m_ingress_retry) {
        m_ingress_retry = false;
        trySendRetries();
    }
    if (!m_ingress_replays.empty() || m_ingress_retry)
        scheduleIngress();
}

void
Sequencer::issueRequest(PacketPtr pkt, RubyRequestType secondary_type)
{
    assert(pkt != NULL);
    if (m_finite_ingress &&
        !ingressQueue(pkt)->areNSlotsAvailable(1, clockEdge())) {
        m_ingress_replays.emplace_back(pkt, secondary_type);
        ++m_ingressDeferredReplays;
        scheduleIngress();
        return;
    }
    if (secondary_type == RubyRequestType_SPMCP_fetch) {
        if (cfAggregateEnabled() && !pkt->req->isSPMMulticastClaim()) {
            auto *request = pkt->findNextSenderState<o3::LSQ::LSQRequest>();
            panic_if(!request, "SPMCP aggregate expects an O3 LSQ request");
            const auto &inst = request->instruction();
            cfAggregateBegin(m_version, pkt->getAddr(), inst->firstIssue,
                inst->cacheFlexExecuteTick, inst->cacheFlexEligibleTick, pkt);
        }
        accountSPMCPOutstanding(curCycle());
        ++m_spmcpOutstanding;
        ++m_spmcpRequestsIssued;
        if (pkt->req->isSPMMulticastClaim())
            ++m_spmcpClaimRequestsIssued;
        else
            ++m_spmcpDataRequestsIssued;
        if (m_spmcpOutstanding > m_spmcpMaxOutstanding.value())
            m_spmcpMaxOutstanding = m_spmcpOutstanding;
    }
    ContextID proc_id = pkt->req->hasContextId() ?
        pkt->req->contextId() : InvalidContextID;

    ContextID core_id = coreId();

    // If valid, copy the pc to the ruby request
    Addr pc = 0;
    if (pkt->req->hasPC()) {
        pc = pkt->req->getPC();
    }
    if (secondary_type == RubyRequestType_ST) {
        DPRINTF(RubyStats,
                "CacheFlexStoreMap paddr=%#x vaddr=%#x pc=%#x size=%u\n",
                pkt->getAddr(),
                pkt->req->hasVaddr() ? pkt->req->getVaddr() : 0,
                pc, pkt->getSize());
    }

    DPRINTF(CacheFlexResidency,
            "CFRES ISSUE id=%p addr=%#x va=%#x pc=%#x bytes=%u type=%s core=%d\n",
            pkt, pkt->getAddr(), pkt->req->hasVaddr() ? pkt->req->getVaddr() : 0,
            pc, pkt->getSize(), RubyRequestType_to_string(secondary_type),
            core_id);
    int blk_size = m_ruby_system->getBlockSizeBytes();

    // check if the packet has data as for example prefetch and flush
    // requests do not
    std::shared_ptr<RubyRequest> msg;
    if (pkt->req->isMemMgmt()) {
        msg = std::make_shared<RubyRequest>(clockEdge(), blk_size,
                                            m_ruby_system,
                                            pc, secondary_type,
                                            RubyAccessMode_Supervisor, pkt,
                                            proc_id, core_id);

        DPRINTFR(ProtocolTrace, "%15s %3s %10s%20s %6s>%-6s %s\n",
                curTick(), m_version, "Seq", "Begin", "", "",
                RubyRequestType_to_string(secondary_type));

        if (pkt->req->isTlbiCmd()) {
            msg->m_isTlbi = true;
            switch (secondary_type) {
              case RubyRequestType_TLBI_EXT_SYNC_COMP:
                msg->m_tlbiTransactionUid = pkt->req->getExtraData();
                break;
              case RubyRequestType_TLBI:
              case RubyRequestType_TLBI_SYNC:
                msg->m_tlbiTransactionUid = \
                    getCurrentUnaddressedTransactionID();
                break;
              default:
                panic("Unexpected TLBI RubyRequestType");
            }
            DPRINTF(RubySequencer, "Issuing TLBI %016x\n",
                    msg->m_tlbiTransactionUid);
        }
    } else {
        Addr request_addr = rubyRequestAddress(pkt, secondary_type);
        msg = std::make_shared<RubyRequest>(clockEdge(), blk_size,
                                            m_ruby_system,
                                            request_addr, pkt->getSize(),
                                            pc, secondary_type,
                                            RubyAccessMode_Supervisor, pkt,
                                            PrefetchBit_No, proc_id, core_id);

        if (pkt->isAtomicOp() &&
            ((secondary_type == RubyRequestType_ATOMIC_RETURN) ||
             (secondary_type == RubyRequestType_ATOMIC_NO_RETURN))){
            // Create the blocksize, access mask and atomicops
            uint32_t offset = getOffset(pkt->getAddr());
            std::vector<std::pair<int,AtomicOpFunctor*>> atomicOps;
            atomicOps.push_back(std::make_pair<int,AtomicOpFunctor*>
                                (offset, pkt->getAtomicOp()));

            msg->setWriteMask(offset, pkt->getSize(), atomicOps);
        }

        if (pkt->req->isSPMRequest()) { // C6
            if (pkt->req->isSPMWBStore()) {
                const Addr spm_slot = pkt->req->getSPMDstAddr();
                if (spm_slot & cfReduction::Finalize) {
                    // SPMWB carries an 8B slot descriptor at the CPU interface;
                    // its controller transaction writes one full 64B line.
                    panic_if(!(spm_slot & (Addr(1) << 59)) ||
                             pkt->getSize()!=sizeof(Addr) || (pkt->getAddr() & 63),
                             "Finalize requires an 8B shared-slot descriptor and a line-aligned coherent destination");
                    msg->m_L3Finalize = cfReduction::Finalize |
                        ((spm_slot >> 24) & 0xffffffffULL);
                }
                msg->m_SrcAddr = pkt->getAddr();
                msg->m_DstSPMAddr = canonicalSPMAddress(spm_slot);
                msg->m_SPMSet = static_cast<int>((spm_slot >> 6) & 0x3ff);
                msg->m_SPMWay = static_cast<int>((spm_slot >> 16) & 0x7);
            } else {
                // For SPMCP_fetch the coherent source is the packet's own
                // translated physical address; the metadata copy is virtual.
                msg->m_SrcAddr = pkt->req->isSPMCPFetch() ?
                    pkt->getAddr() : pkt->req->getSPMSrcAddr(); // C6
                msg->m_DstSPMAddr = canonicalSPMAddress(
                    pkt->req->getSPMDstAddr()); // C6
                msg->m_SPMSet = pkt->req->getSPMSet(); // C6
                // Mask to the 8-way guide geometry so software can place the
                // encoded slot window at a physical base above the SE image
                // pages (which are allocated upward from PA 0 and would
                // otherwise alias SPM slots).
                msg->m_SPMWay = pkt->req->getSPMWay() & 0x7; // C6
                msg->m_SPMMulticastMask =
                    static_cast<int>(pkt->req->getSPMMulticastMask());
                msg->m_SPMMulticastClaim = pkt->req->isSPMMulticastClaim();
            }
            if (secondary_type == RubyRequestType_SPMST && pkt->hasData()) {
                // SPM store payload travels in WTData at the slot's in-line
                // offset (writeSPMData reads it from there); the generic
                // RubyRequest constructor leaves WTData zeroed.
                const int offset = getOffset(msg->m_DstSPMAddr);
                const int bytes = offset + pkt->getSize();
                if (bytes > msg->m_WTData.getBlockSize()) {
                    // Pure SPM vectors are not split into 64B Ruby requests.
                    // This is the existing request payload, not cache capacity.
                    msg->m_WTData = DataBlock(bytes);
                    msg->m_WTData.clear();
                }
                msg->m_WTData.setData(pkt->getConstPtr<uint8_t>(), offset,
                                      pkt->getSize());
                if (pkt->req->isMasked()) {
                    const auto &enabled = pkt->req->getByteEnable();
                    assert(enabled.size() == pkt->getSize());
                    msg->m_SPMMasked = true;
                    msg->m_writeMask = WriteMask(bytes);
                    for (unsigned i = 0; i < enabled.size(); ++i)
                        if (enabled[i])
                            msg->m_writeMask.setMask(offset + i, 1);
                }
            }
        }

        DPRINTFR(ProtocolTrace, "%15s %3s %10s%20s %6s>%-6s %#x %s\n",
                curTick(), m_version, "Seq", "Begin", "", "",
                printAddress(msg->getPhysicalAddress()),
                RubyRequestType_to_string(secondary_type));
    }

    // hardware transactional memory
    // If the request originates in a transaction,
    // then mark the Ruby message as such.
    if (pkt->isHtmTransactional()) {
        msg->m_htmFromTransaction = true;
        msg->m_htmTransactionUid = pkt->getHtmTransactionUid();
    }

    Tick latency = cyclesToTicks(
                        m_controller->mandatoryQueueLatency(secondary_type));
    assert(latency > 0);

    assert(m_mandatory_q_ptr != NULL);
    CFL_PKT("SEQ_ENQUEUE", pkt, "msg=%p ready=%llu", msg.get(), clockEdge()+latency);
    ingressQueue(pkt)->enqueue(msg, clockEdge(), latency,
                               m_ruby_system->getRandomization(),
                               m_ruby_system->getWarmupEnabled());
}

template <class KEY, class VALUE>
std::ostream &
operator<<(std::ostream &out, const std::unordered_map<KEY, VALUE> &map)
{
    for (const auto &[key, values] : map) {
        out << "[ " << key << " =";
        for (const auto &seq_req : values) {
            out << " " << RubyRequestType_to_string(seq_req.m_second_type);
        }
        out << " ]";
    }
    return out;
}

void
Sequencer::print(std::ostream& out) const
{
    out << "[Sequencer: " << m_version
        << ", outstanding requests: " << m_outstanding_count
        << ", request table: " << m_RequestTable
        << "]";
}

void
Sequencer::recordRequestType(SequencerRequestType requestType) {
    DPRINTF(RubyStats, "Recorded statistic: %s\n",
            SequencerRequestType_to_string(requestType));
}

void
Sequencer::evictionCallback(Addr address)
{
    llscClearMonitor(address);
    ruby_eviction_callback(address);
}

void
Sequencer::incrementUnaddressedTransactionCnt()
{
    m_unaddressedTransactionCnt++;
    // Limit m_unaddressedTransactionCnt to 32 bits,
    // top 32 bits should always be zeroed out
    uint64_t aligned_txid = \
        m_unaddressedTransactionCnt << m_ruby_system->getBlockSizeBits();

    if (aligned_txid > 0xFFFFFFFFull) {
        m_unaddressedTransactionCnt = 0;
    }
}

uint64_t
Sequencer::getCurrentUnaddressedTransactionID() const
{
    return (
        uint64_t(m_version & 0xFFFFFFFF) << 32) |
        (m_unaddressedTransactionCnt << m_ruby_system->getBlockSizeBits()
    );
}

} // namespace ruby
} // namespace gem5
