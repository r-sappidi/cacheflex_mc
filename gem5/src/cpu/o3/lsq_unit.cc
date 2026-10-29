/*
 * Copyright (c) 2010-2014, 2017-2021 ARM Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * All rights reserved
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
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
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

#include "cpu/o3/lsq_unit.hh"
#include "debug/CacheFlexDeps.hh"
#include "cpu/o3/cacheflex_load_trace.hh"
#include "cpu/o3/spm_forwarding.hh"

#include "arch/generic/debugfaults.hh"
#include "base/str.hh"
#include "cpu/checker/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "cpu/o3/lsq.hh"
#include "debug/Activity.hh"
#include "debug/HtmCpu.hh"
#include "debug/IEW.hh"
#include "debug/LSQUnit.hh"
#include "debug/O3PipeView.hh"
//jq86
#include "debug/SPM_CPU.hh"


#include "mem/packet.hh"
#include "mem/request.hh"

namespace gem5
{

namespace o3
{


LSQUnit::WritebackEvent::WritebackEvent(const DynInstPtr &_inst,
        PacketPtr _pkt, LSQUnit *lsq_ptr)
    : Event(Default_Pri, AutoDelete),
      inst(_inst), pkt(_pkt), lsqPtr(lsq_ptr)
{
    assert(_inst->savedRequest);
    _inst->savedRequest->writebackScheduled();
}

void
LSQUnit::WritebackEvent::process()
{
    assert(!lsqPtr->cpu->switchedOut());

    lsqPtr->writeback(inst, pkt);

    assert(inst->savedRequest);
    inst->savedRequest->writebackDone();
    delete pkt;
}

const char *
LSQUnit::WritebackEvent::description() const
{
    return "Store writeback";
}


//jq86
static inline int spm_get_way(Addr spm_addr) {
    return (spm_addr >> 16) & 0x7;   // 3 bits
}

static inline int spm_get_slot(Addr spm_addr) {
    // Dependency identity includes level and LLC bank. These are NOT extra
    // set bits in the physical array; this helper is used for alias checks.
    return ((spm_addr >> 6) & 0x3FF) |
        ((spm_addr & (Addr(1) << 59)) ?
            (0x4000 | (((spm_addr >> 20) & 7) << 10)) : 0);
}

static inline int spm_get_offset(Addr spm_addr) {
    return spm_addr & 0x3F;          // 6 bits (64B)
}
static inline Addr spm_addr(int way, int slot, int offset) {
    return ((Addr)way << 16) | ((Addr)slot << 6) | offset;
}


bool
LSQUnit::recvTimingResp(PacketPtr pkt)
{
    LSQRequest *request = dynamic_cast<LSQRequest*>(pkt->senderState);
    assert(request != nullptr);
    if (boundedTracking) {
        const auto seq = request->instruction()->seqNum;
        auto it = outstandingLoadPackets.find(seq);
        if (it != outstandingLoadPackets.end() && --it->second == 0) {
            outstandingLoadPackets.erase(it);
            if (detachedLoads.erase(seq)) {
                ++stats.boundedDetachedReleases;
                cpu->wakeCPU();
                iewStage->updateLSQNextCycle = true;
                iewStage->activityThisCycle();
            }
        }
    }
    //jq86

    // Ruby's SPMWB completion is a bridge acknowledgment, not a direct SPM
    // load/store response. Always release its shadow dependency, regardless
    // of the optional resource-accounting policy. Otherwise reusing that
    // SPM slot later waits forever on an already-completed writeback.
    if (pkt->SPMresp || pkt->req->isSPMWBStore())
    {
        DynInstPtr inst = request->instruction();
        assert(inst);

        bool command_complete = true;
        if (auto *batch = dynamic_cast<LSQ::BatchSPMCPRequest *>(request))
            command_complete = batch->noteSPMResponse();
        if (auto *dma = dynamic_cast<LSQ::DmaSPMCPRequest *>(request))
            command_complete = dma->noteSPMResponse();
        if (!command_complete)
            return true;

        auto it = spmShadowTable.find(inst->seqNum);
        if (it != spmShadowTable.end()) {
            auto &e = it->second;
            Addr sAddr = e.spmAddr;
            DPRINTF(SPM_CPU,
                "[SPM] Resp Recv: SN=%llu Addr=%#x [S:%d W:%d O:%d] (Waking %d blocked loads)\n",
                inst->seqNum, sAddr,
                spm_get_slot(sAddr), spm_get_way(sAddr), spm_get_offset(sAddr),
                (int)e.waitingLoads.size());

            e.ready = true;
            for (auto &ld : e.waitingLoads) {
                if (!ld || ld->isSquashed()) continue;
                // A vector load can wait on several overlapping copies.
                // Only the last dependency may enqueue its replay: multiple
                // responses before the next issue cycle otherwise insert the
                // same DynInst into the IQ more than once, re-sending a live
                // memory request. Keep the other wait lists intact until their
                // copies complete. read() still rechecks addresses on replay.
                bool stillWaiting = false;
                for (const auto &[seq, other] : spmShadowTable) {
                    if (other.ready || seq >= ld->seqNum)
                        continue;
                    for (const auto &waiter : other.waitingLoads) {
                        if (waiter == ld) {
                            stillWaiting = true;
                            break;
                        }
                    }
                    if (stillWaiting)
                        break;
                }
                if (stillWaiting) {
                    DPRINTF(SPM_CPU,
                        "[SPM] -> Still waiting: load SN=%llu after copy SN=%llu\n",
                        ld->seqNum, inst->seqNum);
                    continue;
                }
                // Direct addReadyMemInst: immediately re-issue this specific
                // load without draining the entire instsToReplay queue.
                // rescheduleMemInst pushes to instsToReplay but never drains
                // (deadlock for VL=4). replayMemInst drains ALL pending loads
                // (replay storm for VL=2). addReadyMemInst targets only this load.
                ld->translationStarted(false);
                ld->translationCompleted(false);
                ld->clearCanIssue();
                ld->clearIssued();
                ld->effAddrValid(false);
                iewStage->instQueue.addReadyMemInst(ld);
                DPRINTF(SPM_CPU,
                    "[SPM] -> Waking blocked load SN=%llu (via addReadyMemInst)\n", ld->seqNum);
            }
            e.waitingLoads.clear();
            DPRINTF(SPM_CPU,
                "[SPM] Shadow marked ready: SN=%llu Addr=%#x\n",
                inst->seqNum, sAddr);
        }
        if (boundedTracking)
            cleanupRetiredShadow();
        return true;
    }



    
    bool ret = true;
    /* Check that the request is still alive before any further action. */
    if (!request->isReleased()) {
        ret = request->recvTimingResp(pkt);
    }
    // If this is the spmThread receiving an spm.ld1qd response (not a SPMCP
    // response), the SPM read is now complete at L2.  Clear the WAR pending
    // load table entry so queued CPTOSPMReqs can proceed.
    if (isSpmLsq) {
        DynInstPtr inst = request->instruction();
        if (inst && (inst->memReqFlags & Request::SPM_LOAD)) {
            InstSeqNum sn = inst->seqNum;
            auto service = spmLoadServiceStart.find(sn);
            if (service != spmLoadServiceStart.end()) {
                stats.spmLoadServiceCycles +=
                    (curTick() - service->second) / cpu->clockPeriod();
                ++stats.spmLoadResponses;
                spmLoadServiceStart.erase(service);
            }
            for (auto &pl : spmPendingLoadTable) {
                if (pl.valid && pl.seqNum == sn) {
                    pl.valid = false;
                    DPRINTF(SPM_CPU,
                        "[SPM] WAR clear: load SN=%llu SPMLoadResp received\n",
                        sn);
                    break;
                }
            }
        }
    }
    return ret;
}

void
LSQUnit::completeDataAccess(PacketPtr pkt)
{
    LSQRequest *request = dynamic_cast<LSQRequest *>(pkt->senderState);
    DynInstPtr inst = request->instruction();

    // hardware transactional memory
    // sanity check
    if (pkt->isHtmTransactional() && !inst->isSquashed()) {
        assert(inst->getHtmTransactionUid() == pkt->getHtmTransactionUid());
    }

    // if in a HTM transaction, it's possible
    // to abort within the cache hierarchy.
    // This is signalled back to the processor
    // through responses to memory requests.
    if (pkt->htmTransactionFailedInCache()) {
        // cannot do this for write requests because
        // they cannot tolerate faults
        const HtmCacheFailure htm_rc =
            pkt->getHtmTransactionFailedInCacheRC();
        if (pkt->isWrite()) {
            DPRINTF(HtmCpu,
                "store notification (ignored) of HTM transaction failure "
                "in cache - addr=0x%lx - rc=%s - htmUid=%d\n",
                pkt->getAddr(), htmFailureToStr(htm_rc),
                pkt->getHtmTransactionUid());
        } else {
            HtmFailureFaultCause fail_reason =
                HtmFailureFaultCause::INVALID;

            if (htm_rc == HtmCacheFailure::FAIL_SELF) {
                fail_reason = HtmFailureFaultCause::SIZE;
            } else if (htm_rc == HtmCacheFailure::FAIL_REMOTE) {
                fail_reason = HtmFailureFaultCause::MEMORY;
            } else if (htm_rc == HtmCacheFailure::FAIL_OTHER) {
                // these are likely loads that were issued out of order
                // they are faulted here, but it's unlikely that these will
                // ever reach the commit head.
                fail_reason = HtmFailureFaultCause::OTHER;
            } else {
                panic("HTM error - unhandled return code from cache (%s)",
                      htmFailureToStr(htm_rc));
            }

            inst->fault =
            std::make_shared<GenericHtmFailureFault>(
                inst->getHtmTransactionUid(),
                fail_reason);

            DPRINTF(HtmCpu,
                "load notification of HTM transaction failure "
                "in cache - pc=%s - addr=0x%lx - "
                "rc=%u - htmUid=%d\n",
                inst->pcState(), pkt->getAddr(),
                htmFailureToStr(htm_rc), pkt->getHtmTransactionUid());
        }
    }

    cpu->ppDataAccessComplete->notify(std::make_pair(inst, pkt));

    assert(!cpu->switchedOut());
    if (!inst->isSquashed()) {
        if (request->needWBToRegister()) {
            // Only loads, store conditionals and atomics perform the writeback
            // after receving the response from the memory
            assert(inst->isLoad() || inst->isStoreConditional() ||
                   inst->isAtomic());

            // hardware transactional memory
            if (pkt->htmTransactionFailedInCache()) {
                request->mainPacket()->setHtmTransactionFailedInCache(
                    pkt->getHtmTransactionFailedInCacheRC() );
            }

            writeback(inst, request->mainPacket());
            if (inst->isStore() || inst->isAtomic()) {
                request->writebackDone();
                completeStore(request->instruction()->sqIt);
            }
        } else if (inst->isStore()) {
            // This is a regular store (i.e., not store conditionals and
            // atomics), so it can complete without writing back
        
        // inst->setCompleted();

        // if (cpu->checker) {
        //     cpu->checker->verify(storeWBIt->instruction());
        // }
            completeStore(request->instruction()->sqIt);
        }
    }
}

LSQUnit::LSQUnit(uint32_t lqEntries, uint32_t sqEntries)
    : lsqID(-1), storeQueue(sqEntries), loadQueue(lqEntries),
      storesToWB(0),
      htmStarts(0), htmStops(0),
      lastRetiredHtmUid(0),
      cacheBlockMask(0), stalled(false),
      isStoreBlocked(false), storeInFlight(false), stats(nullptr)
{
}

void
LSQUnit::init(CPU *cpu_ptr, IEW *iew_ptr, const BaseO3CPUParams &params,
        LSQ *lsq_ptr, unsigned id)
{
    lsqID = id;

    cpu = cpu_ptr;
    iewStage = iew_ptr;

    lsq = lsq_ptr;

    // LSQUnit is not itself a SimObject, so its statistics must be attached
    // explicitly to the CPU's statistics tree.  Register both units: besides
    // making the SPM diagnostics visible, this restores the pre-existing
    // regular-LSQ statistics which were otherwise left as legacy/orphaned
    // stats in this port.
    cpu->addStatGroup(
        csprintf("%sLsq%i", isSpmLsq ? "spm" : "regular", lsqID).c_str(),
        &stats);

    DPRINTF(LSQUnit, "Creating LSQUnit%i object.\n",lsqID);

    depCheckShift = params.LSQDepCheckShift;
    checkLoads = params.LSQCheckLoads;
    needsTSO = params.needsTSO;
    boundedTracking = params.cacheFlexBoundedTracking;
    spmStoreForwarding = params.cacheFlexSpmStoreForwarding;
    spmEarlyAddress = params.cacheFlexSpmEarlyAddress;
    spmForwardLatency = params.cacheFlexSpmForwardLatency;
    fatal_if(spmStoreForwarding && spmForwardLatency < Cycles(1),
             "SPM forwarding must have a positive modeled latency");

    resetState();
    //jq86
    spmShadowTable.clear();
    spmShadowTable.reserve(512);  // pre-allocate for typical max in-flight SPMCPs
    spmRawWaitStart.clear();
    spmRawWaitStart.reserve(128);
    spmLoadServiceStart.clear();
    spmLoadServiceStart.reserve(128);
    spmPendingLoadTable.resize(64);    // tracks spm.ld1qd loads only; max = LQEntries(32) + margin
}


void
LSQUnit::resetState()
{
    storesToWB = 0;

    // hardware transactional memory
    // nesting depth
    htmStarts = htmStops = 0;

    storeWBIt = storeQueue.begin();

    retryPkt = NULL;
    memDepViolator = NULL;

    stalled = false;

    cacheBlockMask = ~(cpu->cacheLineSize() - 1);
}

std::string
LSQUnit::name() const
{
    if (MaxThreads == 1) {
        return iewStage->name() + ".lsq";
    } else {
        return iewStage->name() + ".lsq.thread" + std::to_string(lsqID);
    }
}

LSQUnit::LSQUnitStats::LSQUnitStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(forwLoads, statistics::units::Count::get(),
               "Number of loads that had data forwarded from stores"),
      ADD_STAT(spmForwardedBytes, statistics::units::Byte::get(),
               "SPM load bytes forwarded from existing SQ payloads"),
      ADD_STAT(spmEarlyAddrIssued, statistics::units::Count::get(),
               "SPM address-only computations issued into existing SQ metadata"),
      ADD_STAT(spmEarlyAddrDisjoint, statistics::units::Count::get(),
               "Load checks bypassing unresolved-data stores with known disjoint ranges"),
      ADD_STAT(spmEarlyAddrOverlap, statistics::units::Count::get(),
               "Load checks still blocked by overlapping early store addresses"),
      ADD_STAT(spmEarlyAddrBusy, statistics::units::Count::get(),
               "Address-only checks awaiting finite unit availability or result"),
      ADD_STAT(squashedLoads, statistics::units::Count::get(),
               "Number of loads squashed"),
      ADD_STAT(ignoredResponses, statistics::units::Count::get(),
               "Number of memory responses ignored because the instruction is "
               "squashed"),
      ADD_STAT(memOrderViolation, statistics::units::Count::get(),
               "Number of memory ordering violations"),
      ADD_STAT(squashedStores, statistics::units::Count::get(),
               "Number of stores squashed"),
      ADD_STAT(rescheduledLoads, statistics::units::Count::get(),
               "Number of loads that were rescheduled"),
      ADD_STAT(blockedByCache, statistics::units::Count::get(),
               "Number of times an access to memory failed due to the cache "
               "being blocked"),
      ADD_STAT(reductionIsolatedRejects, statistics::units::Count::get(),
               "Reduction capture rejections isolated from coherent retries"),
      ADD_STAT(coherentSentWhileReductionBlocked, statistics::units::Count::get(),
               "Accepted regular requests while reduction capture awaits retry"),
      ADD_STAT(spmRawBlockEvents, statistics::units::Count::get(),
               "SPM load attempts blocked by an overlapping SPMCP"),
      ADD_STAT(spmRawBlockedUniqueLoads, statistics::units::Count::get(),
               "Unique SPM loads that experienced an SPMCP RAW wait"),
      ADD_STAT(spmRawWaitCycles, statistics::units::Cycle::get(),
               "Cycles from first SPMCP RAW block until final load issue"),
      ADD_STAT(spmUnresolvedAddrRetries, statistics::units::Count::get(),
               "SPM load retries while an older SPMCP address is unresolved"),
      ADD_STAT(spmPortBlockedEvents, statistics::units::Count::get(),
               "SPM load sends rejected by port or cache backpressure"),
      ADD_STAT(spmLoadResponses, statistics::units::Count::get(),
               "SPM load responses with a recorded successful send"),
      ADD_STAT(boundedShadowAllocations, statistics::units::Count::get(),
               "Bridge allocations charged to the finite SPM SQ"),
      ADD_STAT(boundedShadowReleases, statistics::units::Count::get(),
               "Completed bridge reservations released from the SPM SQ"),
      ADD_STAT(boundedDetachedLoads, statistics::units::Count::get(),
               "Loads retaining LQ credit after squash or early retirement"),
      ADD_STAT(boundedDetachedReleases, statistics::units::Count::get(),
               "Detached load credits released on final response"),
      ADD_STAT(boundedMaxLoadOccupancy, statistics::units::Count::get(),
               "Maximum observed LQ occupancy including detached requests"),
      ADD_STAT(boundedMaxStoreOccupancy, statistics::units::Count::get(),
               "Maximum observed SQ occupancy including bridge reservations"),
      ADD_STAT(spmLoadServiceCycles, statistics::units::Cycle::get(),
               "Successful-send to response cycles summed over SPM loads"),
      ADD_STAT(loadToUse, "Distribution of cycle latency between the "
                "first time a load is issued and its completion")
{
    loadToUse
        .init(0, 299, 10)
        .flags(statistics::nozero);
}

void
LSQUnit::setDcachePort(RequestPort *dcache_port)
{
    dcachePort = dcache_port;
}

void
LSQUnit::drainSanityCheck() const
{
    for (int i = 0; i < loadQueue.capacity(); ++i)
        assert(!loadQueue[i].valid());

    assert(storesToWB == 0);
    assert(!retryPkt);
}

void
LSQUnit::takeOverFrom()
{
    resetState();
}

void
LSQUnit::insert(const DynInstPtr &inst)
{
    assert(inst->isMemRef());

    // assert(inst->isLoad() || inst->isStore() || inst->isAtomic());
    
    //weihang
    assert(inst->isLoad() || inst->isStore() || inst->isAtomic() || inst->memReqFlags & Request::SPM_LOAD || inst->memReqFlags & Request::SPM_WRITEBACK || inst->memReqFlags & Request::CP_TO_SPM || inst->memReqFlags & Request::SPM_STORE);

    if (inst->isLoad()) {
        insertLoad(inst);
    } else {
        insertStore(inst);
    }

    //weihang
    // if (inst->isLoad() || inst->memReqFlags & Request::SPM_LOAD || inst->memReqFlags & Request::CP_TO_SPM) {
    //     insertLoad(inst);
    // } else if (inst->isLoad() || inst->memReqFlags & Request::SPM_STORE || inst->memReqFlags & Request::SPM_WRITEBACK || inst->memReqFlags & Request::CP_TO_SPM) {
    //     insertStore(inst);
    // }

    inst->setInLSQ();
}

void
LSQUnit::insertLoad(const DynInstPtr &load_inst)
{
    assert(!lqFull());
    assert(!loadQueue.full());
    assert(loadQueue.size() < loadQueue.capacity());

    DPRINTF(LSQUnit, "Inserting load PC %s, idx:%i [sn:%lli]\n",
            load_inst->pcState(), loadQueue.tail(), load_inst->seqNum);


    //jq86
    if (load_inst->memReqFlags & Request::SPM_LOAD) {
        Addr addr = load_inst->effAddr; // 注意：此时 effAddr 可能还没计算好，如果是动态计算的话。
        // 如果此时还不知道地址，可以只打印 SN
        DPRINTF(SPM_CPU, "[SPM] Insert Load: SN=%llu PC=%s\n", 
                load_inst->seqNum, load_inst->pcState());
    }

    /* Grow the queue. */
    loadQueue.advance_tail();

    load_inst->sqIt = storeQueue.end();

    assert(!loadQueue.back().valid());
    loadQueue.back().set(load_inst);
    load_inst->lqIdx = loadQueue.tail();
    assert(load_inst->lqIdx > 0);
    load_inst->lqIt = loadQueue.getIterator(load_inst->lqIdx);


    
    // hardware transactional memory
    // transactional state and nesting depth must be tracked
    // in the in-order part of the core.
    if (load_inst->isHtmStart()) {
        htmStarts++;
        DPRINTF(HtmCpu, ">> htmStarts++ (%d) : htmStops (%d)\n",
                htmStarts, htmStops);

        const int htm_depth = htmStarts - htmStops;
        const auto& htm_cpt = cpu->tcBase(lsqID)->getHtmCheckpointPtr();
        auto htm_uid = htm_cpt->getHtmUid();

        // for debugging purposes
        if (!load_inst->inHtmTransactionalState()) {
            htm_uid = htm_cpt->newHtmUid();
            DPRINTF(HtmCpu, "generating new htmUid=%u\n", htm_uid);
            if (htm_depth != 1) {
                DPRINTF(HtmCpu,
                    "unusual HTM transactional depth (%d)"
                    " possibly caused by mispeculation - htmUid=%u\n",
                    htm_depth, htm_uid);
            }
        }
        load_inst->setHtmTransactionalState(htm_uid, htm_depth);
    }

    if (load_inst->isHtmStop()) {
        htmStops++;
        DPRINTF(HtmCpu, ">> htmStarts (%d) : htmStops++ (%d)\n",
                htmStarts, htmStops);

        if (htmStops==1 && htmStarts==0) {
            DPRINTF(HtmCpu,
            "htmStops==1 && htmStarts==0. "
            "This generally shouldn't happen "
            "(unless due to misspeculation)\n");
        }
    }
}

void
LSQUnit::insertStore(const DynInstPtr& store_inst)
{
    assert(!sqFull());
    // Make sure it is not full before inserting an instruction.
    assert(!storeQueue.full());
    assert(storeQueue.size() < storeQueue.capacity());

    DPRINTF(LSQUnit, "Inserting store PC %s, idx:%i [sn:%lli]\n",
            store_inst->pcState(), storeQueue.tail(), store_inst->seqNum);

    if (store_inst->memReqFlags & (Request::CP_TO_SPM | Request::SPM_WRITEBACK | Request::SPM_STORE)) {
        DPRINTF(SPM_CPU, "[SPM] Insert Store/Cmd: SN=%llu PC=%s (Flags: %s%s)\n", 
                store_inst->seqNum, store_inst->pcState(),
                (store_inst->memReqFlags & Request::CP_TO_SPM) ? "CP2SPM " : "",
                (store_inst->memReqFlags & Request::SPM_WRITEBACK) ? "WB " : "");
    }


    storeQueue.advance_tail();

    store_inst->sqIdx = storeQueue.tail();
    store_inst->sqIt = storeQueue.getIterator(store_inst->sqIdx);

    store_inst->lqIdx = loadQueue.tail() + 1;
    assert(store_inst->lqIdx > 0);
    store_inst->lqIt = loadQueue.end();
     
    
    storeQueue.back().set(store_inst);

    // //jq86
    // if (store_inst->memReqFlags & Request::CP_TO_SPM || store_inst->memReqFlags & Request::SPM_WRITEBACK) 
    // {
    //     DPRINTF(LSQUnit,
    //         "SPM pseudo-store inserted [sn:%lli] (CP2SPM/WRITEBACK)\n",
    //         store_inst->seqNum);
    //     Addr spm_addr = 0;
    //     if (store_inst->memData) {
    //         memcpy(&spm_addr, store_inst->memData, sizeof(Addr));
    //     } else {
    //         spm_addr = 0;
    //     }
    //     store_inst->effAddr = spm_addr;
    //     store_inst->effSize = sizeof(Addr);
    //     store_inst->effAddrValid(true);
    //     entry.size() = store_inst->effSize;
    //     entry.canWB()     = false;   
    //     entry.committed() = false;
    //     entry.completed() = false;
    //     store_inst->setExecuted();
    // }

}

DynInstPtr
LSQUnit::getMemDepViolator()
{
    DynInstPtr temp = memDepViolator;

    memDepViolator = NULL;

    return temp;
}

unsigned
LSQUnit::numFreeLoadEntries()
{
        DPRINTF(LSQUnit, "LQ size: %d, #loads occupied: %d\n",
                loadQueue.capacity(), loadQueue.size());
        const auto used = loadQueue.size() +
            (boundedTracking ? detachedLoads.size() : 0);
        assert(used <= loadQueue.capacity());
        if (boundedTracking && used > stats.boundedMaxLoadOccupancy.value())
            stats.boundedMaxLoadOccupancy = used;
        return loadQueue.capacity() - used;
}

unsigned
LSQUnit::numFreeStoreEntries()
{
        DPRINTF(LSQUnit, "SQ size: %d, #stores occupied: %d\n",
                storeQueue.capacity(), storeQueue.size());
        const auto used = storeQueue.size() +
            (boundedTracking && isSpmLsq ? spmShadowTable.size() : 0);
        assert(used <= storeQueue.capacity());
        if (boundedTracking && used > stats.boundedMaxStoreOccupancy.value())
            stats.boundedMaxStoreOccupancy = used;
        return storeQueue.capacity() - used;

 }

void
LSQUnit::checkSnoop(PacketPtr pkt)
{
    // Should only ever get invalidations in here
    assert(pkt->isInvalidate());

    DPRINTF(LSQUnit, "Got snoop for address %#x\n", pkt->getAddr());

    for (int x = 0; x < cpu->numContexts(); x++) {
        gem5::ThreadContext *tc = cpu->getContext(x);
        bool no_squash = cpu->thread[x]->noSquashFromTC;
        cpu->thread[x]->noSquashFromTC = true;
        tc->getIsaPtr()->handleLockedSnoop(pkt, cacheBlockMask);
        cpu->thread[x]->noSquashFromTC = no_squash;
    }

    if (loadQueue.empty())
        return;

    auto iter = loadQueue.begin();

    Addr invalidate_addr = pkt->getAddr() & cacheBlockMask;

    DynInstPtr ld_inst = iter->instruction();
    assert(ld_inst);
    LSQRequest *request = iter->request();

    // Check that this snoop didn't just invalidate our lock flag
    if (ld_inst->effAddrValid() && request &&
        request->isCacheBlockHit(invalidate_addr, cacheBlockMask)
        && ld_inst->memReqFlags & Request::LLSC) {
        ld_inst->tcBase()->getIsaPtr()->handleLockedSnoopHit(ld_inst.get());
    }

    bool force_squash = false;

    while (++iter != loadQueue.end()) {
        ld_inst = iter->instruction();
        assert(ld_inst);
        request = iter->request();
        if (!ld_inst->effAddrValid() || ld_inst->strictlyOrdered() || !request)
            continue;

        DPRINTF(LSQUnit, "-- inst [sn:%lli] to pktAddr:%#x\n",
                    ld_inst->seqNum, invalidate_addr);

        if (force_squash ||
            request->isCacheBlockHit(invalidate_addr, cacheBlockMask)) {
            if (needsTSO) {
                // If we have a TSO system, as all loads must be ordered with
                // all other loads, this load as well as *all* subsequent loads
                // need to be squashed to prevent possible load reordering.
                force_squash = true;
            }
            if (ld_inst->possibleLoadViolation() || force_squash) {
                DPRINTF(LSQUnit, "Conflicting load at addr %#x [sn:%lli]\n",
                        pkt->getAddr(), ld_inst->seqNum);

                // Mark the load for re-execution
                ld_inst->fault = std::make_shared<ReExec>();
                request->setStateToFault();
            } else {
                DPRINTF(LSQUnit, "HitExternal Snoop for addr %#x [sn:%lli]\n",
                        pkt->getAddr(), ld_inst->seqNum);

                // Make sure that we don't lose a snoop hitting a LOCKED
                // address since the LOCK* flags don't get updated until
                // commit.
                if (ld_inst->memReqFlags & Request::LLSC) {
                    ld_inst->tcBase()->getIsaPtr()->
                        handleLockedSnoopHit(ld_inst.get());
                }

                // If a older load checks this and it's true
                // then we might have missed the snoop
                // in which case we need to invalidate to be sure
                ld_inst->hitExternalSnoop(true);
            }
        }
    }
    return;
}

Fault
LSQUnit::checkViolations(typename LoadQueue::iterator& loadIt,
        const DynInstPtr& inst)
{
    Addr inst_eff_addr1 = inst->effAddr >> depCheckShift;
    Addr inst_eff_addr2 = (inst->effAddr + inst->effSize - 1) >> depCheckShift;

    /** @todo in theory you only need to check an instruction that has executed
     * however, there isn't a good way in the pipeline at the moment to check
     * all instructions that will execute before the store writes back. Thus,
     * like the implementation that came before it, we're overly conservative.
     */
    while (loadIt != loadQueue.end()) {
        DynInstPtr ld_inst = loadIt->instruction();
        if (!ld_inst->effAddrValid() || ld_inst->strictlyOrdered()) {
            ++loadIt;
            continue;
        }

        Addr ld_eff_addr1 = ld_inst->effAddr >> depCheckShift;
        Addr ld_eff_addr2 =
            (ld_inst->effAddr + ld_inst->effSize - 1) >> depCheckShift;

        if (inst_eff_addr2 >= ld_eff_addr1 && inst_eff_addr1 <= ld_eff_addr2) {
            if (inst->isLoad()) {
                // If this load is to the same block as an external snoop
                // invalidate that we've observed then the load needs to be
                // squashed as it could have newer data
                if (ld_inst->hitExternalSnoop()) {
                    if (!memDepViolator ||
                            ld_inst->seqNum < memDepViolator->seqNum) {
                        DPRINTF(LSQUnit, "Detected fault with inst [sn:%lli] "
                                "and [sn:%lli] at address %#x\n",
                                inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                        memDepViolator = ld_inst;

                        ++stats.memOrderViolation;

                        return std::make_shared<GenericISA::M5PanicFault>(
                            "Detected fault with inst [sn:%lli] and "
                            "[sn:%lli] at address %#x\n",
                            inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                    }
                }

                // Otherwise, mark the load has a possible load violation and
                // if we see a snoop before it's commited, we need to squash
                ld_inst->possibleLoadViolation(true);
                DPRINTF(LSQUnit, "Found possible load violation at addr: %#x"
                        " between instructions [sn:%lli] and [sn:%lli]\n",
                        inst_eff_addr1, inst->seqNum, ld_inst->seqNum);
            } else {
                // A load/store incorrectly passed this store.
                // Check if we already have a violator, or if it's newer
                // squash and refetch.
                if (memDepViolator && ld_inst->seqNum > memDepViolator->seqNum)
                    break;

                DPRINTF(LSQUnit, "Detected fault with inst [sn:%lli] and "
                        "[sn:%lli] at address %#x\n",
                        inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                memDepViolator = ld_inst;

                ++stats.memOrderViolation;

                return std::make_shared<GenericISA::M5PanicFault>(
                    "Detected fault with "
                    "inst [sn:%lli] and [sn:%lli] at address %#x\n",
                    inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
            }
        }

        ++loadIt;
    }
    return NoFault;
}




Fault
LSQUnit::executeLoad(const DynInstPtr &inst)
{
    CFL_INST("EXECUTE", inst, "");
    // Execute a specific load.
    Fault load_fault = NoFault;

    DPRINTF(LSQUnit, "Executing load PC %s, [sn:%lli]\n",
            inst->pcState(), inst->seqNum);

    assert(!inst->isSquashed());

    load_fault = inst->initiateAcc();

    if (load_fault == NoFault && !inst->readMemAccPredicate()) {
        assert(inst->readPredicate());
        inst->setExecuted();
        inst->completeAcc(nullptr);
        iewStage->instToCommit(inst);
        iewStage->activityThisCycle();
        return NoFault;
    }

    if (inst->isTranslationDelayed() && load_fault == NoFault)
        return load_fault;

    if (load_fault != NoFault && inst->translationCompleted() &&
            inst->savedRequest->isPartialFault()
            && !inst->savedRequest->isComplete()) {
        assert(inst->savedRequest->isSplit());
        // If we have a partial fault where the mem access is not complete yet
        // then the cache must have been blocked. This load will be re-executed
        // when the cache gets unblocked. We will handle the fault when the
        // mem access is complete.
        return NoFault;
    }

    // If the instruction faulted or predicated false, then we need to send it
    // along to commit without the instruction completing.
    if (load_fault != NoFault || !inst->readPredicate()) {
        // Send this instruction to commit, also make sure iew stage
        // realizes there is activity.  Mark it as executed unless it
        // is a strictly ordered load that needs to hit the head of
        // commit.
        if (!inst->readPredicate())
            inst->forwardOldRegs();
        DPRINTF(LSQUnit, "Load [sn:%lli] not executed from %s\n",
                inst->seqNum,
                (load_fault != NoFault ? "fault" : "predication"));
        if (!(inst->hasRequest() && inst->strictlyOrdered()) ||
            inst->isAtCommit()) {
            inst->setExecuted();
        }
        iewStage->instToCommit(inst);
        iewStage->activityThisCycle();
    } else {
        if (inst->effAddrValid()) {
            auto it = inst->lqIt;
            ++it;

            if (checkLoads)
                return checkViolations(it, inst);
        }
    }

    return load_fault;
}

Fault
LSQUnit::executeStore(const DynInstPtr &store_inst)
{
    store_inst->cacheFlexExecuteTick = curTick();
    // Make sure that a store exists.
    assert(storeQueue.size() != 0);

    if (store_inst->seqNum == 3280313)
    {
        int a =1 ;
    }

    ssize_t store_idx = store_inst->sqIdx;

    DPRINTF(LSQUnit, "Executing store PC %s [sn:%lli]\n",
            store_inst->pcState(), store_inst->seqNum);

    assert(!store_inst->isSquashed());

    // Check the recently completed loads to see if any match this store's
    // address.  If so, then we have a memory ordering violation.
    typename LoadQueue::iterator loadIt = store_inst->lqIt;

    Fault store_fault = store_inst->initiateAcc();



    if (store_inst->isTranslationDelayed() &&
        store_fault == NoFault)
        return store_fault;

    if (!store_inst->readPredicate()) {
        DPRINTF(LSQUnit, "Store [sn:%lli] not executed from predication\n",
                store_inst->seqNum);
        store_inst->forwardOldRegs();
        return store_fault;
    }

    

    if (storeQueue[store_idx].size() == 0) {
        DPRINTF(LSQUnit,"Fault on Store PC %s, [sn:%lli], Size = 0\n",
                store_inst->pcState(), store_inst->seqNum);

        if (store_inst->memReqFlags & Request::CP_TO_SPM)
        {
            // Page fault on SPMCP coherence address (demand-paged heap).
            // Let fixupFault handle it at commit: send to commit with fault set.
            if (!(store_inst->hasRequest() && store_inst->strictlyOrdered()) ||
                store_inst->isAtCommit()) {
                store_inst->setExecuted();
            }
            iewStage->instToCommit(store_inst);
            iewStage->activityThisCycle();
            return store_fault;
        }

        if (store_inst->isAtomic()) {
            // If the instruction faulted, then we need to send it along
            // to commit without the instruction completing.
            if (!(store_inst->hasRequest() && store_inst->strictlyOrdered()) ||
                store_inst->isAtCommit()) {
                store_inst->setExecuted();
            }
            iewStage->instToCommit(store_inst);
            iewStage->activityThisCycle();
        }

        return store_fault;
    }

    assert(store_fault == NoFault);

    if (store_inst->isStoreConditional() || store_inst->isAtomic()) {
        // Store conditionals and Atomics need to set themselves as able to
        // writeback if we haven't had a fault by here.
        storeQueue[store_idx].canWB() = true;

        ++storesToWB;
    }

    return checkViolations(loadIt, store_inst);

}

void
LSQUnit::commitLoad()
{
    assert(loadQueue.front().valid());

    DynInstPtr inst = loadQueue.front().instruction();

    DPRINTF(LSQUnit, "Committing head load instruction, PC %s\n",
            inst->pcState());

    // Update histogram with memory latency from load
    // Only take latency from load demand that where issued and did not fault
    if (!inst->isInstPrefetch() && !inst->isDataPrefetch()
            && inst->firstIssue != -1
            && inst->lastWakeDependents != -1) {
        stats.loadToUse.sample(cpu->ticksToCycles(
                    inst->lastWakeDependents - inst->firstIssue));
    }

    if (boundedTracking && outstandingLoadPackets.count(inst->seqNum)) {
        detachedLoads.emplace(inst->seqNum, true);
        ++stats.boundedDetachedLoads;
    }
    loadQueue.front().clear();
    loadQueue.pop_front();
}

void
LSQUnit::commitLoads(InstSeqNum &youngest_inst)
{
    assert(loadQueue.size() == 0 || loadQueue.front().valid());

    while (loadQueue.size() != 0 && loadQueue.front().instruction()->seqNum
            <= youngest_inst) {
        commitLoad();
    }
}

void
LSQUnit::commitStores(InstSeqNum &youngest_inst)
{
    assert(storeQueue.size() == 0 || storeQueue.front().valid());

    if (isSpmLsq) {
        for (auto &[seq, entry] : spmShadowTable)
            if (seq <= youngest_inst)
                entry.retired = true;
    }

    /* Forward iterate the store queue (age order). */
    for (auto& x : storeQueue) {
        assert(x.valid());
        // Mark any stores that are now committed and have not yet
        // been marked as able to write back.
        if (!x.canWB()) {
            if (x.instruction()->seqNum > youngest_inst) {
                break;
            }
            DPRINTF(LSQUnit, "Marking store as able to write back, PC "
                    "%s [sn:%lli]\n",
                    x.instruction()->pcState(),
                    x.instruction()->seqNum);

            x.canWB() = true;
            x.instruction()->cacheFlexEligibleTick = curTick();

            ++storesToWB;
        }
    }
}


//jq86
void
LSQUnit::writebackBlockedStore()
{
    assert(isStoreBlocked);
    auto inst = storeWBIt->instruction();
    auto req  = storeWBIt->request();

    req->sendPacketToCache();
    if (req->isSent()) {
        isStoreBlocked = false;

        // A DMA whose first line was backpressured enters the decoupled
        // engine here rather than through the normal writeback path.
        if (auto *dma = dynamic_cast<LSQ::DmaSPMCPRequest *>(req))
            lsq->registerDma(dma);

        if (inst->memReqFlags & (Request::CP_TO_SPM | Request::SPM_WRITEBACK)) {
            DPRINTF(SPM_CPU,
                    "[SPM] (retry) Force Complete: SN=%llu\n", inst->seqNum);

            bool wasHead = (storeWBIt == storeQueue.begin());
            auto doneIt = storeWBIt;
            completeStore(doneIt);

            if (wasHead) {
                if (!storeQueue.empty())
                    storeWBIt = storeQueue.begin();
                else
                    storeWBIt = storeQueue.end();
            } else {
                if (!storeQueue.empty())
                    storeWBIt++;
                else
                    storeWBIt = storeQueue.end();
            }
        } else {
            storePostSend();
        }
    }
}


void
LSQUnit::writebackStores()
{
    // Bug 16 diag: SQ state for main thread (not spmThread)
    if (!isSpmLsq) {
        if (storesToWB > 0 && storeWBIt.dereferenceable() && storeWBIt->valid()) {
            DynInstPtr hi = storeWBIt->instruction();
        }
    }

    if (isStoreBlocked) {
        DPRINTF(LSQUnit, "Writing back  blocked store\n");
        writebackBlockedStore();
    }

    while (storesToWB > 0 &&
           storeWBIt.dereferenceable() &&
           storeWBIt->valid() &&
           storeWBIt->canWB() &&
           ((!needsTSO) || (!storeInFlight)) &&
           (isSpmLsq ? (lsq->spmPortsAvailable(false) > 0)
                     : lsq->cachePortAvailable(false))) {

        if (isStoreBlocked) {
            DPRINTF(LSQUnit, "Unable to write back any more stores, cache"
                    " is blocked!\n");
            break;
        }

        // Store didn't write any data so no need to write it back to
        // memory.
        if (storeWBIt->size() == 0) {
            /* It is important that the preincrement happens at (or before)
             * the call, as the the code of completeStore checks
             * storeWBIt. */
            completeStore(storeWBIt++);
            continue;
        }

        if (storeWBIt->instruction()->isDataPrefetch()) {
            storeWBIt++;
            continue;
        }

        assert(storeWBIt->hasRequest());
        assert(!storeWBIt->committed());

        DynInstPtr inst = storeWBIt->instruction();
        LSQRequest* request = storeWBIt->request();

        if (!isSpmLsq) {
            bool overlap = false;
            const Addr start = request->mainReq()->getPaddr();
            const Addr end = start + ((inst->memReqFlags & Request::CP_TO_SPM)
                                      ? 64 : storeWBIt->size());
            for (const auto &[seq, entry] : lsq->getSpmUnit(lsqID).spmShadowTable) {
                if (entry.isWriteback && !entry.ready && seq < inst->seqNum &&
                    (!entry.addrValid || (start < entry.regularAddr + 64 &&
                                         end > entry.regularAddr))) {
                    overlap = true;
                    break;
                }
            }
            if (overlap)
                break; // Preserve WAW/RAW until the older coherent install.
        }

        if (!isSpmLsq && (inst->memReqFlags & Request::SPM_WRITEBACK)) {
            // Bridge reads issue from the regular SQ, independently of SPM
            // store draining. In-order retirement does NOT imply that an older
            // SPM store has issued or completed. Keep the source read behind
            // older overlapping stores/copies, using the existing queue and
            // shadow records (no extra buffer or global fence).
            auto &spm = lsq->getSpmUnit(lsqID);
            const Addr source = inst->spmAddr;
            const int slot = spm_get_slot(source), first = spm_get_way(source);
            const int last = first + ((source & (Addr(1) << 57)) ? 1 : 0);
            bool raw = false;
            for (auto it = spm.storeQueue.begin(); it != spm.storeQueue.end(); ++it) {
                if (!it->valid()) continue;
                auto older = it->instruction();
                if (!older || older->isSquashed() || older->seqNum >= inst->seqNum)
                    continue;
                if (!it->hasRequest() || !older->effAddrValid()) { raw = true; break; }
                const Addr a = it->request()->mainReq()->getPaddr();
                const int begin = spm_get_way(a);
                const int end = begin + (spm_get_offset(a) + it->size() - 1) / 64;
                if (spm_get_slot(a) == slot && begin <= last && end >= first) {
                    raw = true; break;
                }
            }
            for (const auto &[seq, e] : spm.spmShadowTable) {
                if (seq >= inst->seqNum || e.ready) continue;
                if (!e.addrValid || (spm_get_slot(e.spmAddr) == slot &&
                    spm_get_way(e.spmAddr) <= last &&
                    spm_get_way(e.spmAddr) + e.wayCount - 1 >= first)) {
                    raw = true; break;
                }
            }
            if (raw) {
                DPRINTF(SPM_CPU, "[SPM] WB RAW defer: SN=%llu source=%#x\n",
                        inst->seqNum, source);
                break;
            }
            // SPMWB is routed by its slot address below the LSQ, so an older
            // access to the target line that has issued but is not yet
            // acknowledged (waiting at L0, the private L2 or the network) is
            // not ordered with it, and an older store could then overwrite
            // the writeback (WAW), or an older copy read its data (WAR). Keep
            // the writeback behind such accesses to its line.
            const Addr line = request->mainReq()->getPaddr() & ~Addr(63);
            bool pending = false;
            for (auto it = storeQueue.begin(); it != storeWBIt; ++it) {
                if (!it->valid() || it->completed() || !it->hasRequest() ||
                    it->size() == 0)
                    continue;
                auto older = it->instruction();
                if (!older || older->isSquashed())
                    continue;
                const Addr a = it->request()->mainReq()->getPaddr();
                const Addr e = a + ((older->memReqFlags &
                                     (Request::CP_TO_SPM |
                                      Request::SPM_WRITEBACK)) ? 64 : it->size());
                if (a < line + 64 && e > line) {
                    pending = true;
                    break;
                }
            }
            if (pending) {
                DPRINTF(SPM_CPU, "[SPM] WB WAW defer: SN=%llu line=%#x\n",
                        inst->seqNum, line);
                break;
            }
            // Posted reductions already captured by Ruby remain ordered by
            // reductionPending until their remote accumulator install ACK.
        }


        //jq86
        if (isSpmLsq) {
            bool tsoHold = storeInFlight;

            bool spmHold = false;
            const InstSeqNum mySeq = inst->seqNum;

            if (inst->memReqFlags & Request::SPM_STORE) {
                // SPM_STORE: only block on older entries to SAME address
                // (not all older entries — spm.st is independent of SPMCP to other addresses)
                // Also skip tsoHold — spm.st to different addresses are independent
                tsoHold = false;
                // A pure store must not overtake a pending copy into any of
                // its slots (WAW), or a writeback reading those slots (WAR).
                const Addr addr = request->mainReq()->getPaddr();
                const int set = spm_get_slot(addr);
                const int first = spm_get_way(addr);
                const int last = first +
                    (spm_get_offset(addr) + storeWBIt->size() - 1) / 64;
                for (const auto &[seq, e] : spmShadowTable) {
                    if (seq >= mySeq || e.ready)
                        continue;
                    if (!e.addrValid ||
                        (spm_get_slot(e.spmAddr) == set &&
                         spm_get_way(e.spmAddr) <= last &&
                         spm_get_way(e.spmAddr) + e.wayCount - 1 >= first)) {
                        spmHold = true;
                        break;
                    }
                }
            } else {
                // Original logic for SPMCP/WB
                for (const auto& [seq, e] : spmShadowTable) {
                    if (e.retired) continue;
                    if (e.seqNum < mySeq) {
                        spmHold = true;
                        break;
                    }
                }
            }

            if (tsoHold || spmHold) {
                DPRINTF(SPM_CPU,
                    "[SPM] Store WB Stalled: SN=%llu (TSO=%d, OlderTxn=%d)\n",
                    inst->seqNum, tsoHold, spmHold);
                break;
            }
        } 

        // Process store conditionals or store release after all previous
        // stores are completed
        if ((request->mainReq()->isLLSC() ||
             request->mainReq()->isRelease()) &&
             (storeWBIt.idx() != storeQueue.head())) {
            DPRINTF(LSQUnit, "Store idx:%i PC:%s to Addr:%#x "
                "[sn:%lli] is %s%s and not head of the queue\n",
                storeWBIt.idx(), inst->pcState(),
                request->mainReq()->getPaddr(), inst->seqNum,
                request->mainReq()->isLLSC() ? "SC" : "",
                request->mainReq()->isRelease() ? "/Release" : "");
            break;
        }

        storeWBIt->committed() = true;

        assert(!inst->memData);
        const unsigned store_data_size =
            request->mainReq()->isSPMCPBatch3() ? sizeof(Addr) :
            request->_size;
        inst->memData = new uint8_t[store_data_size];

        if (storeWBIt->isAllZeros())
            memset(inst->memData, 0, store_data_size);
        else
            memcpy(inst->memData, storeWBIt->data(), store_data_size);

        request->buildPackets();

        DPRINTF(LSQUnit, "D-Cache: Writing back store idx:%i PC:%s "
                "to Addr:%#x, data:%#x [sn:%lli]\n",
                storeWBIt.idx(), inst->pcState(),
                request->mainReq()->getPaddr(), (int)*(inst->memData),
                inst->seqNum);

        // @todo: Remove this SC hack once the memory system handles it.
        if (inst->isStoreConditional()) {
            // Disable recording the result temporarily.  Writing to
            // misc regs normally updates the result, but this is not
            // the desired behavior when handling store conditionals.
            inst->recordResult(false);
            bool success = inst->tcBase()->getIsaPtr()->handleLockedWrite(
                    inst.get(), request->mainReq(), cacheBlockMask);
            inst->recordResult(true);
            request->packetSent();

            if (!success) {
                request->complete();
                // Instantly complete this store.
                DPRINTF(LSQUnit, "Store conditional [sn:%lli] failed.  "
                        "Instantly completing it.\n",
                        inst->seqNum);
                PacketPtr new_pkt = new Packet(*request->packet());
                WritebackEvent *wb = new WritebackEvent(inst,
                        new_pkt, this);
                cpu->schedule(wb, curTick() + 1);
                completeStore(storeWBIt);
                if (!storeQueue.empty())
                    storeWBIt++;
                else
                    storeWBIt = storeQueue.end();
                continue;
            }
        }

        if (request->mainReq()->isLocalAccess()) {
            assert(!inst->isStoreConditional());
            assert(!inst->inHtmTransactionalState());
            gem5::ThreadContext *thread = cpu->tcBase(lsqID);
            PacketPtr main_pkt = new Packet(request->mainReq(),
                                            MemCmd::WriteReq);
            main_pkt->dataStatic(inst->memData);
            request->mainReq()->localAccessor(thread, main_pkt);
            delete main_pkt;
            completeStore(storeWBIt);
            storeWBIt++;
            continue;
        }
        /* Send to cache */
        // WAR hazard check for cp2spm: defer CPTOSPMReq until all earlier
        // spm.ld1qd targeting the same SPM slot have received their response.
        //
        // CP_TO_SPM is inserted into the MAIN thread's store queue (not the
        // spmThread's), so writebackStores runs with isSpmLsq=false here.
        // We must therefore use the spmThread's load queue and pending load
        // table (not this LSQUnit's, which is empty for the main thread).
        //
        // Two sources of pending loads:
        // (A) spmPendingLoadTable: loads that have already called read() and
        //     sent their SPMLoadReq (address is known).
        // (B) Load queue entries not yet in the table: loads dispatched to the
        //     LQ but not yet issued from IQ (read() not called yet, address may
        //     be unknown).  With fast prefetchers CPTOSPMReq can complete before
        //     such loads have issued, so we must block conservatively for them.
        if (inst->memReqFlags & Request::CP_TO_SPM) {
            // Use spmThread's LSQUnit for all SPM-related state
            LSQUnit& spmUnit = isSpmLsq ? *this
                                        : lsq->getSpmUnit(inst->threadNumber);

            Addr spm_target  = inst->spmAddr;
            int  target_slot = spm_get_slot(spm_target);
            int  target_way  = spm_get_way(spm_target);
            int  target_last_way = target_way +
                ((inst->memReqFlags & Request::SPMCP_BATCH3) ? 2 : 0);
            bool war_conflict = false;

            // (A) Issued loads with known addresses
            for (auto &pl : spmUnit.spmPendingLoadTable) {
                if (!pl.valid) continue;
                if (pl.seqNum >= inst->seqNum) continue;
                if (pl.slot != target_slot) continue;
                if (target_way > pl.end_way ||
                    target_last_way < pl.start_way) continue;
                war_conflict = true;
                DPRINTF(SPM_CPU,
                    "[SPM] WAR defer: SPMCP SN=%llu [S:%d W:%d] blocked by issued load SN=%llu\n",
                    inst->seqNum, target_slot, target_way, pl.seqNum);
                break;
            }

            // (B) Unissued loads not yet in spmPendingLoadTable.
            //
            // When the load has executed (IEW address computation done) but
            // read() has not yet been called, effAddrValid()==true and effAddr
            // holds the SPM address.  Use it for a precise overlap check.
            //
            // Only block conservatively when the address is truly unknown
            // (effAddrValid()==false, load not yet issued from IQ).  This
            // eliminates spurious WAR stalls for loads targeting a different
            // SPM buffer (e.g., double-buffered kernels where loads hit
            // Buffer[K&1] while SPMCPs target Buffer[1-(K&1)]).
            if (!war_conflict) {
                auto lq_it = spmUnit.loadQueue.begin();
                while (lq_it != spmUnit.loadQueue.end()) {
                    DynInstPtr ld = lq_it->instruction();
                    ++lq_it;
                    if (!ld || ld->isSquashed()) continue;
                    if (ld->seqNum >= inst->seqNum) continue;
                    if (!(ld->memReqFlags & Request::SPM_LOAD)) continue;
                    if (ld->isExecuted()) continue; // SPMLoadResp received, read done

                    // Check if already tracked by loop (A) above
                    bool tracked = false;
                    for (auto &pl : spmUnit.spmPendingLoadTable)
                        if (pl.valid && pl.seqNum == ld->seqNum) { tracked = true; break; }
                    if (tracked) continue;

                    // Untracked load: try address-based check first.
                    if (ld->effAddrValid()) {
                        // Address known (IEW computed EA before calling read()).
                        // Do precise slot+way overlap check.
                        Addr load_addr     = ld->effAddr;
                        int  load_slot     = spm_get_slot(load_addr);
                        int  load_sw       = spm_get_way(load_addr);
                        int  load_off      = spm_get_offset(load_addr);
                        unsigned load_size = ld->effSize;
                        int  load_ew       = load_sw + ((load_off + (int)load_size - 1) / 64);

                        if (load_slot != target_slot) continue; // different slot — no WAR
                        if (target_way > load_ew ||
                            target_last_way < load_sw) continue; // no way overlap

                        // Real WAR conflict: load targets same slot+way, hasn't read yet
                        war_conflict = true;
                        DPRINTF(SPM_CPU,
                            "[SPM] WAR defer (addr-match): SPMCP SN=%llu [S:%d W:%d] "
                            "blocked by load SN=%llu [S:%d W:%d-%d]\n",
                            inst->seqNum, target_slot, target_way,
                            ld->seqNum, load_slot, load_sw, load_ew);
                    } else {
                        // Address unknown (load not yet issued from IQ); block conservatively.
                        war_conflict = true;
                        DPRINTF(SPM_CPU,
                            "[SPM] WAR defer (addr-unknown): SPMCP SN=%llu [S:%d W:%d] "
                            "blocked by unissued load SN=%llu\n",
                            inst->seqNum, target_slot, target_way, ld->seqNum);
                    }
                    if (war_conflict) break;
                }
            }

            if (war_conflict) {
                // CP2SPM is fundamentally a READ from DRAM to private SPM.
                // It must not block subsequent CP2SPMs (reads don't block reads)
                // or subsequent normal WRs (reads don't block unrelated writes).
                // WR-WR ordering is preserved by the store queue's existing logic.
                // Use continue (not break) so only this CP2SPM is skipped.
                continue;
            }
        }
        request->sendPacketToCache();
        if (auto *dma = dynamic_cast<LSQ::DmaSPMCPRequest *>(request);
            dma && request->isSent())
            lsq->registerDma(dma);
        //jq86
        if (isSpmLsq || (inst->memReqFlags & (Request::CP_TO_SPM | Request::SPM_WRITEBACK))) 
        {
            RequestPtr orig_req = request->mainReq();
            assert(orig_req);
            RequestPtr spm_req = std::make_shared<Request>(*orig_req);
            PacketPtr spm_pkt = new Packet(spm_req, MemCmd::WriteReq);

            Addr paddr = request->mainReq()->getPaddr();
            DPRINTF(SPM_CPU,
                "[SPM] Store/Cmd Sent to Cache: SN=%llu Addr=%#x [S:%d W:%d O:%d] (Type: %s)\n",
                inst->seqNum, paddr, 
                spm_get_slot(paddr), spm_get_way(paddr), spm_get_offset(paddr),
                (inst->memReqFlags & Request::CP_TO_SPM) ? "CP2SPM" : 
                (inst->memReqFlags & Request::SPM_WRITEBACK) ? "SPM_WB" : "Store");
        }


        /* If successful, do the post send */
        if (request->isSent()) 
        {
            if (inst->memReqFlags & Request::CP_TO_SPM || inst->memReqFlags & Request::SPM_WRITEBACK) 
            {
                DPRINTF(SPM_CPU, "[SPM] Force Complete: SN=%llu\n", inst->seqNum);
                bool wasHead = (storeWBIt == storeQueue.begin());


                auto doneIt = storeWBIt;
                completeStore(doneIt); 
                if (wasHead) {

                    if (!storeQueue.empty()) {
                        storeWBIt = storeQueue.begin(); 
                    } else {
                        storeWBIt = storeQueue.end();   
                    }
                } else {

                    if (!storeQueue.empty()) {
                        storeWBIt++;
                    } else {
                        storeWBIt = storeQueue.end();
                    }
                }

                continue;
            }
            
            storePostSend();
        }

        
        else
        {

            if (inst->memReqFlags & Request::CP_TO_SPM || inst->memReqFlags & Request::SPM_WRITEBACK) 
            {
                DPRINTF(SPM_CPU, "[SPM] Store SN=%llu Failed to send (Cache Busy)\n", inst->seqNum);
            }
            DPRINTF(LSQUnit, "D-Cache became blocked when writing [sn:%lli], "
                    "will retry later\n",
                    inst->seqNum);
        }
    }
    assert(storesToWB >= 0);
}

void
LSQUnit::squash(const InstSeqNum &squashed_num)
{
    DPRINTF(LSQUnit, "Squashing until [sn:%lli]!"
            "(Loads:%i Stores:%i)\n", squashed_num, loadQueue.size(),
            storeQueue.size());

    while (loadQueue.size() != 0 &&
            loadQueue.back().instruction()->seqNum > squashed_num) {
        DPRINTF(LSQUnit,"Load Instruction PC %s squashed, "
                "[sn:%lli]\n",
                loadQueue.back().instruction()->pcState(),
                loadQueue.back().instruction()->seqNum);

        if (isStalled() && loadQueue.tail() == stallingLoadIdx) {
            stalled = false;
            stallingStoreIsn = 0;
            stallingLoadIdx = 0;
        }

        // hardware transactional memory
        // Squashing instructions can alter the transaction nesting depth
        // and must be corrected before fetching resumes.
        if (loadQueue.back().instruction()->isHtmStart())
        {
            htmStarts = (--htmStarts < 0) ? 0 : htmStarts;
            DPRINTF(HtmCpu, ">> htmStarts-- (%d) : htmStops (%d)\n",
              htmStarts, htmStops);
        }
        if (loadQueue.back().instruction()->isHtmStop())
        {
            htmStops = (--htmStops < 0) ? 0 : htmStops;
            DPRINTF(HtmCpu, ">> htmStarts (%d) : htmStops-- (%d)\n",
              htmStarts, htmStops);
        }
        // Clear the smart pointer to make sure it is decremented.
        if (boundedTracking) {
            const auto seq = loadQueue.back().instruction()->seqNum;
            if (outstandingLoadPackets.count(seq)) {
                detachedLoads.emplace(seq, true);
                ++stats.boundedDetachedLoads;
            }
        }
        loadQueue.back().instruction()->setSquashed();
        loadQueue.back().clear();

        loadQueue.pop_back();
        ++stats.squashedLoads;
    }

    // hardware transactional memory
    // scan load queue (from oldest to youngest) for most recent valid htmUid
    auto scan_it = loadQueue.begin();
    uint64_t in_flight_uid = 0;
    while (scan_it != loadQueue.end()) {
        if (scan_it->instruction()->isHtmStart() &&
            !scan_it->instruction()->isSquashed()) {
            in_flight_uid = scan_it->instruction()->getHtmTransactionUid();
            DPRINTF(HtmCpu, "loadQueue[%d]: found valid HtmStart htmUid=%u\n",
                scan_it._idx, in_flight_uid);
        }
        scan_it++;
    }
    // If there's a HtmStart in the pipeline then use its htmUid,
    // otherwise use the most recently committed uid
    const auto& htm_cpt = cpu->tcBase(lsqID)->getHtmCheckpointPtr();
    if (htm_cpt) {
        const uint64_t old_local_htm_uid = htm_cpt->getHtmUid();
        uint64_t new_local_htm_uid;
        if (in_flight_uid > 0)
            new_local_htm_uid = in_flight_uid;
        else
            new_local_htm_uid = lastRetiredHtmUid;

        if (old_local_htm_uid != new_local_htm_uid) {
            DPRINTF(HtmCpu, "flush: lastRetiredHtmUid=%u\n",
                lastRetiredHtmUid);
            DPRINTF(HtmCpu, "flush: resetting localHtmUid=%u\n",
                new_local_htm_uid);

            htm_cpt->setHtmUid(new_local_htm_uid);
        }
    }

    if (memDepViolator && squashed_num < memDepViolator->seqNum) {
        memDepViolator = NULL;
    }

    while (storeQueue.size() != 0 &&
           storeQueue.back().instruction()->seqNum > squashed_num) {
        // Instructions marked as can WB are already committed.
        if (storeQueue.back().canWB()) {
            break;
        }

        DPRINTF(LSQUnit,"Store Instruction PC %s squashed, "
                "idx:%i [sn:%lli]\n",
                storeQueue.back().instruction()->pcState(),
                storeQueue.tail(), storeQueue.back().instruction()->seqNum);

        // I don't think this can happen.  It should have been cleared
        // by the stalling load.
        if (isStalled() &&
            storeQueue.back().instruction()->seqNum == stallingStoreIsn) {
            panic("Is stalled should have been cleared by stalling load!\n");
            stalled = false;
            stallingStoreIsn = 0;
        }

        // Clear the smart pointer to make sure it is decremented.
        storeQueue.back().instruction()->setSquashed();

        // Must delete request now that it wasn't handed off to
        // memory.  This is quite ugly.  @todo: Figure out the proper
        // place to really handle request deletes.
        storeQueue.back().clear();

        storeQueue.pop_back();
        ++stats.squashedStores;
    }




    //jq86

     if (isSpmLsq) {
        for (auto it = spmShadowTable.begin(); it != spmShadowTable.end(); ) {
            auto &entry = it->second;
            if (entry.seqNum > squashed_num) {
                for (auto &ld : entry.waitingLoads) {
                    if (ld && !ld->isSquashed())
                        iewStage->replayMemInst(ld);
                }
                it = spmShadowTable.erase(it);
            } else {
                ++it;
            }
        }
        // Also remove squashed loads from the WAR pending table.
        for (auto &pl : spmPendingLoadTable) {
            if (pl.valid && pl.seqNum > squashed_num)
                pl.valid = false;
        }
        for (auto it = spmRawWaitStart.begin();
             it != spmRawWaitStart.end(); ) {
            if (it->first > squashed_num)
                it = spmRawWaitStart.erase(it);
            else
                ++it;
        }
        for (auto it = spmLoadServiceStart.begin();
             it != spmLoadServiceStart.end(); ) {
            if (it->first > squashed_num)
                it = spmLoadServiceStart.erase(it);
            else
                ++it;
        }
    }


}

uint64_t
LSQUnit::getLatestHtmUid() const
{
    const auto& htm_cpt = cpu->tcBase(lsqID)->getHtmCheckpointPtr();
    return htm_cpt->getHtmUid();
}

void
LSQUnit::storePostSend()
{
    if (isStalled() &&
        storeWBIt->instruction()->seqNum == stallingStoreIsn) {
        DPRINTF(LSQUnit, "Unstalling, stalling store [sn:%lli] "
                "load idx:%li\n",
                stallingStoreIsn, stallingLoadIdx);
        stalled = false;
        stallingStoreIsn = 0;
        iewStage->replayMemInst(loadQueue[stallingLoadIdx].instruction());
    }

    if (!storeWBIt->instruction()->isStoreConditional()) {
        // The store is basically completed at this time. This
        // only works so long as the checker doesn't try to
        // verify the value in memory for stores.
    
        // weihang
        storeWBIt->instruction()->setCompleted();

        if (cpu->checker) {
            cpu->checker->verify(storeWBIt->instruction());
        }

    }

    if (needsTSO) {
        storeInFlight = true;
    }

    storeWBIt++;
}

void
LSQUnit::writeback(const DynInstPtr &inst, PacketPtr pkt)
{
    iewStage->wakeCPU();

    // Squashed instructions do not need to complete their access.
    if (inst->isSquashed()) {
        assert (!inst->isStore() || inst->isStoreConditional());
        ++stats.ignoredResponses;
        return;
    }

    if (!inst->isExecuted()) {
        DPRINTF(CacheFlexDeps, "CFD MEM_RESP sn=%llu pc=%#x spm=%d va=%#x\n",
                inst->seqNum, inst->pcState().instAddr(),
                bool(inst->memReqFlags & Request::SPMLD), inst->effAddr);
        inst->setExecuted();

        if (inst->fault == NoFault) {
            // Complete access to copy data to proper place.
            inst->completeAcc(pkt);
        } else {
            // If the instruction has an outstanding fault, we cannot complete
            // the access as this discards the current fault.

            // If we have an outstanding fault, the fault should only be of
            // type ReExec or - in case of a SplitRequest - a partial
            // translation fault

            // Unless it's a hardware transactional memory fault
            auto htm_fault = std::dynamic_pointer_cast<
                GenericHtmFailureFault>(inst->fault);

            if (!htm_fault) {
                assert(dynamic_cast<ReExec*>(inst->fault.get()) != nullptr ||
                       inst->savedRequest->isPartialFault());

            } else if (!pkt->htmTransactionFailedInCache()) {
                // Situation in which the instruction has a hardware
                // transactional memory fault but not the packet itself. This
                // can occur with ldp_uop microops since access is spread over
                // multiple packets.
                DPRINTF(HtmCpu,
                        "%s writeback with HTM failure fault, "
                        "however, completing packet is not aware of "
                        "transaction failure. cause=%s htmUid=%u\n",
                        inst->staticInst->getName(),
                        htmFailureToStr(htm_fault->getHtmFailureFaultCause()),
                        htm_fault->getHtmUid());
            }

            DPRINTF(LSQUnit, "Not completing instruction [sn:%lli] access "
                    "due to pending fault.\n", inst->seqNum);
        }
    }

    // Need to insert instruction into queue to commit
    iewStage->instToCommit(inst);

    iewStage->activityThisCycle();

    // see if this load changed the PC
    iewStage->checkMisprediction(inst);
}

void
LSQUnit::completeStore(typename StoreQueue::iterator store_idx)
{
    assert(store_idx->valid());
    store_idx->completed() = true;
    --storesToWB;
    // A bit conservative because a store completion may not free up entries,
    // but hopefully avoids two store completions in one cycle from making
    // the CPU tick twice.
    cpu->wakeCPU();
    cpu->activityThisCycle();

    /* We 'need' a copy here because we may clear the entry from the
     * store queue. */
    DynInstPtr store_inst = store_idx->instruction();
    if (store_idx == storeQueue.begin()) {
        do {

            storeQueue.front().clear();
            storeQueue.pop_front();
        } while (storeQueue.front().completed() &&
                 !storeQueue.empty());

        iewStage->updateLSQNextCycle = true;
    }

    DPRINTF(LSQUnit, "Completing store [sn:%lli], idx:%i, store head "
            "idx:%i\n",
            store_inst->seqNum, store_idx.idx() - 1, storeQueue.head() - 1);

#if TRACING_ON
    if (debug::O3PipeView) {
        store_inst->storeTick =
            curTick() - store_inst->fetchTick;
    }
#endif

    if (isStalled() &&
        store_inst->seqNum == stallingStoreIsn) {
        DPRINTF(LSQUnit, "Unstalling, stalling store [sn:%lli] "
                "load idx:%li\n",
                stallingStoreIsn, stallingLoadIdx);
        stalled = false;
        stallingStoreIsn = 0;
        iewStage->replayMemInst(loadQueue[stallingLoadIdx].instruction());
    }

    store_inst->setCompleted();

    if (needsTSO) {
        storeInFlight = false;
    }

    // Tell the checker we've completed this instruction.  Some stores
    // may get reported twice to the checker, but the checker can
    // handle that case.
    // Store conditionals cannot be sent to the checker yet, they have
    // to update the misc registers first which should take place
    // when they commit
    if (cpu->checker &&  !store_inst->isStoreConditional()) {
        cpu->checker->verify(store_inst);
    }
}

bool
LSQUnit::trySendPacket(bool isLoad, PacketPtr data_pkt)
{
    bool ret = true;
    bool cache_got_blocked = false;

    LSQRequest *request = dynamic_cast<LSQRequest*>(data_pkt->senderState);
    const bool isolated = lsq->independentBridge();
    const bool reduction = lsq->independentReduction() && isSpmLsq &&
        data_pkt->req->isSPMST() && (data_pkt->getAddr() & (Addr(1) << 60));
    const bool bridge = isolated && !isSpmLsq &&
        (data_pkt->req->isSPMCPFetch() || data_pkt->req->isSPMWBStore());
    const bool traceLoad = isLoad && !isSpmLsq &&
        cfTraceLoad(request->instruction()->pcState().instAddr());
    if (traceLoad) {
        CFL_INST("FRAG_TRY", request->instruction(),
            "id=%p req=%p pa=%#x bytes=%u cache_blocked=%d port_available=%d",
            data_pkt, request, data_pkt->getAddr(), data_pkt->getSize(),
            lsq->cacheBlocked(), lsq->cachePortAvailable(true));
    }

    if (isSpmLsq) {
        // SPM path: consume ports proportional to cachelines.
        // Each 64B cacheline = 1 port slot. Send immediately, consume num_cl.
        // If num_cl > available ports, the overshoot blocks subsequent sends
        // this cycle (budget exhausted), modeling bandwidth correctly.
        int avail = lsq->spmPortsAvailable(isLoad);
        if (avail <= 0 || (reduction ? lsq->reductionBlocked() :
                          (isolated && lsq->spmBlocked()))) {
            ret = false;
        } else {
            RequestPort &port = reduction ? lsq->getReductionPort() : *dcachePort;
            if (!port.sendTimingReq(data_pkt)) {
                ret = false;
                cache_got_blocked = true;
            }
        }
    } else {
        // Normal cache path
        if (!(bridge ? lsq->bridgeBlocked() : lsq->cacheBlocked()) &&
            lsq->cachePortAvailable(isLoad)) {
            RequestPort &port = bridge ? lsq->getBridgePort() : *dcachePort;
            if (!port.sendTimingReq(data_pkt)) {
                ret = false;
                cache_got_blocked = true;
            }
        } else {
            ret = false;
        }
    }

    if (ret) {
        if (!isSpmLsq && lsq->independentReduction() && lsq->reductionBlocked())
            ++stats.coherentSentWhileReductionBlocked;
        if (!isLoad) {
            isStoreBlocked = false;
        }
        if (isSpmLsq) {
            lsq->spmPortBusy(isLoad);
        } else {
            lsq->cachePortBusy(isLoad);
        }
        request->packetSent();
        if (boundedTracking && isLoad && data_pkt->needsResponse())
            ++outstandingLoadPackets[request->instruction()->seqNum];
    } else {
        if (cache_got_blocked) {
            if (reduction) {
                lsq->reductionBlocked(true);
                ++stats.reductionIsolatedRejects;
            } else if (bridge)
                lsq->bridgeBlocked(true);
            else if (isolated && isSpmLsq)
                lsq->spmBlocked(true);
            else
                lsq->cacheBlocked(true);
            ++stats.blockedByCache;
        }
        auto *dma = dynamic_cast<LSQ::DmaSPMCPRequest *>(request);
        if (!isLoad && !(dma && dma->isBackground())) {
            assert(request == storeWBIt->request());
            isStoreBlocked = true;
        }
        request->packetNotSent();
    }
    DPRINTF(LSQUnit, "Memory request (pkt: %s) from inst [sn:%llu] was"
            " %ssent (cache is blocked: %d, cache_got_blocked: %d)\n",
            data_pkt->print(), request->instruction()->seqNum,
            ret ? "": "not ", lsq->cacheBlocked(), cache_got_blocked);
    if (traceLoad) {
        CFL_INST("FRAG_RESULT", request->instruction(),
            "id=%p req=%p pa=%#x bytes=%u accepted=%d rejected_by_ruby=%d",
            data_pkt, request, data_pkt->getAddr(), data_pkt->getSize(),
            ret, cache_got_blocked);
    }
    return ret;
}

void
LSQUnit::startStaleTranslationFlush()
{
    DPRINTF(LSQUnit, "Unit %p marking stale translations %d %d\n", this,
        storeQueue.size(), loadQueue.size());
    for (auto& entry : storeQueue) {
        if (entry.valid() && entry.hasRequest())
            entry.request()->markAsStaleTranslation();
    }
    for (auto& entry : loadQueue) {
        if (entry.valid() && entry.hasRequest())
            entry.request()->markAsStaleTranslation();
    }
}

bool
LSQUnit::checkStaleTranslations() const
{
    DPRINTF(LSQUnit, "Unit %p checking stale translations\n", this);
    for (auto& entry : storeQueue) {
        if (entry.valid() && entry.hasRequest()
            && entry.request()->hasStaleTranslation())
            return true;
    }
    for (auto& entry : loadQueue) {
        if (entry.valid() && entry.hasRequest()
            && entry.request()->hasStaleTranslation())
            return true;
    }
    DPRINTF(LSQUnit, "Unit %p found no stale translations\n", this);
    return false;
}

void
LSQUnit::recvRetry()
{
    if (isStoreBlocked) {
        DPRINTF(LSQUnit, "Receiving retry: blocked store\n");
        writebackBlockedStore();
    }
}

void
LSQUnit::dumpInsts() const
{
    cprintf("Load store queue: Dumping instructions.\n");
    cprintf("Load queue size: %i\n", loadQueue.size());
    cprintf("Load queue: ");

    for (const auto& e: loadQueue) {
        const DynInstPtr &inst(e.instruction());
        cprintf("%s.[sn:%llu] ", inst->pcState(), inst->seqNum);
    }
    cprintf("\n");

    cprintf("Store queue size: %i\n", storeQueue.size());
    cprintf("Store queue: ");

    for (const auto& e: storeQueue) {
        const DynInstPtr &inst(e.instruction());
        cprintf("%s.[sn:%llu] ", inst->pcState(), inst->seqNum);
    }

    cprintf("\n");
}

void LSQUnit::schedule(Event& ev, Tick when) { cpu->schedule(ev, when); }

BaseMMU *LSQUnit::getMMUPtr() { return cpu->mmu; }

unsigned int
LSQUnit::cacheLineSize()
{
    return cpu->cacheLineSize();
}


Fault
LSQUnit::read(LSQRequest *request, ssize_t load_idx)
{
    LQEntry& load_entry = loadQueue[load_idx];
    const DynInstPtr& load_inst = load_entry.instruction();

    CFL_INST("TRANSLATED_READ", load_inst, "req=%p va=%#x bytes=%u",
             request, request->mainReq()->getVaddr(), request->mainReq()->getSize());

    load_entry.setRequest(request);
    assert(load_inst);

    assert(!load_inst->isExecuted());

    // Make sure this isn't a strictly ordered load
    // A bit of a hackish way to get strictly ordered accesses to work
    // only if they're at the head of the LSQ and are ready to commit
    // (at the head of the ROB too).
    
    //jq86
    if (isSpmLsq)
    {
        // Eagerly clean up ready shadow entries before RAW check scan.
        // Without this, entries accumulate when dispatch stalls (DRAM pressure /
        // full store queue), causing O(|table|) scan cost per spm.ld1qd load →
        // O(n²) slowdown for large K_tiles × N_tiles (Bug 15).
        cleanupRetiredShadow();

        {
            int n_valid = 0;
            InstSeqNum min_sn = UINT64_MAX;
            for (auto &[seq, e] : spmShadowTable) {
                ++n_valid; if (e.seqNum < min_sn) min_sn = e.seqNum;
            }
        }
        Addr load_s_raw = request->mainReq()->getVaddr();
        unsigned size   = request->mainReq()->getSize();

        int start_way = spm_get_way(load_s_raw);
        int slot      = spm_get_slot(load_s_raw);
        int off_s     = spm_get_offset(load_s_raw);

        int end_offset_total = off_s + size - 1;
        int end_way = start_way + (end_offset_total / 64);
        int off_e   = end_offset_total % 64;

        // WAR tracking: unconditionally register this load in spmPendingLoadTable
        // so that any future SPMCP targeting the same SPM slot is deferred until
        // this load's SPMLoadReq has been sent (regardless of RAW-block status).
        {
            bool dup_pl = false;
            for (auto &pl : spmPendingLoadTable)
                if (pl.valid && pl.seqNum == load_inst->seqNum) { dup_pl = true; break; }
            if (!dup_pl) {
                bool inserted = false;
                for (auto &pl : spmPendingLoadTable) {
                    if (!pl.valid) {
                        pl.valid     = true;
                        pl.seqNum    = load_inst->seqNum;
                        pl.slot      = slot;
                        pl.start_way = start_way;
                        pl.end_way   = end_way;
                        DPRINTF(SPM_CPU,
                            "[SPM] WAR track (unconditional): load SN=%llu [S:%d W:%d-%d]\n",
                            load_inst->seqNum, slot, start_way, end_way);
                        inserted = true;
                        break;
                    }
                }
                panic_if(!inserted,
                    "spmPendingLoadTable overflow (size=%zu): SN=%llu. "
                    "Increase table size (resize) in lsq_unit.cc:307",
                    spmPendingLoadTable.size(), load_inst->seqNum);
            }
        }

        DPRINTF(SPM_CPU,
            "[SPM] Exec Load: SN=%llu Addr=%#x [S:%d W:%d-%d O:%d] Size=%d\n",
            load_inst->seqNum, load_s_raw, slot, start_way, end_way, off_s, size);


        bool blocked = false;
        InstSeqNum blockingSeq = 0;
        // True if any shadow entry has unresolved address (addrValid=false).
        // In that case the load is rescheduled for retry next cycle rather than
        // blocking conservatively — by then the SPMCP will have executed and we
        // can do a precise address-based check.
        bool deferred_unresolved = false;

        // Address-based RAW: block spm.ld1qd only on pending SPMCPs whose spmAddr
        // overlaps with this load's SPM address range [start_way..end_way, slot].
        //
        // For each matching entry, add this load to its waitingLoads.  When the
        // last registered SPMCP's SPMresp arrives, the load is woken once and
        // replays. Replay rechecks any newly resolved overlapping entries.
        //
        // If spmAddr is not yet known (addrValid=false, SPMCP dispatched but not
        // yet executed), reschedule the load for retry next cycle instead of
        // blocking conservatively — the SPMCP will execute within 1-2 cycles and
        // addrValid will be true, enabling a precise overlap check.
        //
        // seqNum filter: only block on SPMCPs dispatched BEFORE this load.
        // Future-iteration SPMCPs (dispatched after the load) have higher seqNums
        // and must not block the current read (WAR, not RAW).
        for (auto &[_seq, entry] : spmShadowTable)
        {
            if (entry.seqNum >= load_inst->seqNum)
                continue;   // future-iteration SPMCP, skip (WAR handled separately)
            if (entry.ready)
                continue;   // SPMCP already completed (SPMresp received), SPM write done

            if (!entry.addrValid) {
                // Address not yet resolved (SPMCP dispatched but not executed).
                // Don't block conservatively; retry load next cycle for precise check.
                deferred_unresolved = true;
                continue;
            }

            int e_way  = spm_get_way(entry.spmAddr);
            int e_last_way = e_way + entry.wayCount - 1;
            int e_slot = spm_get_slot(entry.spmAddr);

            bool overlaps = (e_slot == slot) &&
                            (e_way <= end_way) &&
                            (e_last_way >= start_way);

            if (overlaps) {
                // Always block: the SPM write for this address range may be
                // incomplete.  Set blocked=true even on replay when the load
                // was already added to waitingLoads (dup=true): without this,
                // the load would proceed incorrectly after the first SPMresp
                // woke it while other overlapping SPMCPs are still in-flight.
                blocked = true;
                blockingSeq = entry.seqNum;

                // Avoid duplicate insertion into waitingLoads
                bool dup = false;
                for (const auto &wl : entry.waitingLoads)
                    if (wl == load_inst) { dup = true; break; }
                if (!dup) {
                    entry.waitingLoads.push_back(load_inst);
                    DPRINTF(SPM_CPU,
                        "[SPM] RAW block: load SN=%llu on SPMCP SN=%llu "
                        "spmAddr=%#x [S:%d W:%d]\n",
                        load_inst->seqNum, entry.seqNum,
                        entry.spmAddr, e_slot, e_way);

                    // WAR tracking is now unconditional (done before this loop)
                }
            }
        }

        // Track shadow table + pending load table high-water marks and dump
        {
            int nShadow = (int)spmShadowTable.size();
            int nPending = 0;
            for (auto &pl : spmPendingLoadTable) if (pl.valid) ++nPending;
        }

        if (blocked) {
            // Load is already registered in waitingLoads of the blocking SPMCP(s).
            // It will be woken by rescheduleMemInst() in recvTimingResp when the
            // SPMCP response arrives.  Do NOT call rescheduleMemInst here — that
            // causes busy-retry every IQ cycle until SPMCP completes (~100 cycles),
            // wasting IQ bandwidth and shadow-table scan cycles.
            // Third state: parked, waiting for SPMCP-resp wake-up.
            load_inst->clearIssued();
            load_inst->clearCanIssue();
            load_inst->effAddrValid(false);
            ++stats.spmRawBlockEvents;
            if (spmRawWaitStart.emplace(load_inst->seqNum, curTick()).second)
            {
                ++stats.spmRawBlockedUniqueLoads;
            }
            ++stats.rescheduledLoads;
            return NoFault;
        }

        if (deferred_unresolved) {
            DPRINTF(SPM_CPU,
                "[SPM] Load SN=%llu deferred: SPMCP addr unresolved, retry next cycle\n",
                load_inst->seqNum);
            iewStage->instQueue.retrySpmAddrLoad(load_inst);
            ++stats.spmUnresolvedAddrRetries;
            return NoFault;
        }

        // Optional finite-SQ forwarding. Bridge RAW checks above always win.
        // Pick the youngest overlapping older store, including completed
        // entries so we never forward stale bytes from an even older store.
        // Unknown addresses and partial/masked stores conservatively fall
        // through to the existing wait path; no merging or extra data buffer.
        if (spmStoreForwarding && !load_inst->inHtmTransactionalState() &&
            !request->isAnyOutstandingRequest()) {
            const auto loadRange = SpmForwardRange::decode(load_s_raw, size);
            SQEntry *candidate = nullptr;
            bool unknown = false;
            for (auto it = storeQueue.begin(); it != storeQueue.end(); ++it) {
                if (!it->valid() || it->instruction()->seqNum >= load_inst->seqNum)
                    continue;
                if (!it->instruction()->effAddrValid() || !it->hasRequest() ||
                    !it->size()) {
                    unknown = true;
                    continue;
                }
                const auto range = SpmForwardRange::decode(
                    it->request()->mainReq()->getPaddr(), it->size());
                if (range.overlaps(loadRange))
                    candidate = &*it;
            }
            if (!unknown && candidate && !candidate->completed() &&
                (candidate->instruction()->memReqFlags & Request::SPM_STORE) &&
                !candidate->instruction()->isAtomic() &&
                !candidate->request()->mainReq()->isMasked() &&
                !request->mainReq()->isLLSC()) {
                const auto range = SpmForwardRange::decode(
                    candidate->request()->mainReq()->getPaddr(), candidate->size());
                if (range.covers(loadRange)) {
                    // Use the same finite byte/port budget as direct SPM loads.
                    if (lsq->spmPortsAvailable(true) <= 0) {
                        ++stats.spmPortBlockedEvents;
                        iewStage->instQueue.retrySpmAddrLoad(load_inst);
                        return NoFault;
                    }
                    lsq->spmPortBusy(true, (off_s + size + 63) / 64);
                    if (!load_inst->memData)
                        load_inst->memData = new uint8_t[size];
                    if (candidate->isAllZeros())
                        memset(load_inst->memData, 0, size);
                    else
                        memcpy(load_inst->memData,
                               candidate->data() + loadRange.begin - range.begin,
                               size);
                    auto *pkt = new Packet(request->mainReq(), MemCmd::ReadReq);
                    pkt->dataStatic(load_inst->memData);
                    // Snapshot already captured: younger copies may overwrite
                    // the array without affecting this load's result.
                    for (auto &pl : spmPendingLoadTable)
                        if (pl.valid && pl.seqNum == load_inst->seqNum)
                            pl.valid = false;
                    auto rawWait = spmRawWaitStart.find(load_inst->seqNum);
                    if (rawWait != spmRawWaitStart.end()) {
                        stats.spmRawWaitCycles +=
                            (curTick() - rawWait->second) / cpu->clockPeriod();
                        spmRawWaitStart.erase(rawWait);
                    }
                    cpu->schedule(new WritebackEvent(load_inst, pkt, this),
                                  cpu->clockEdge(spmForwardLatency));
                    ++stats.forwLoads;
                    stats.spmForwardedBytes += size;
                    DPRINTF(SPM_CPU, "[SPM] SQ_FORWARD load=%llu store=%llu bytes=%u latency=%llu\n",
                            load_inst->seqNum, candidate->instruction()->seqNum,
                            size, uint64_t(spmForwardLatency));
                    return NoFault;
                }
            }
        }

        // Unlike a synchronous array call, the timed Ruby path can still have
        // older SPM stores in the SQ or in transit. Do not let a younger load
        // observe pre-store data. Loads not forwarded above must wait for
        // matching stores; early addresses may prove unrelated stores disjoint.
        bool pendingSpmStore = false;
        for (auto it = storeQueue.begin(); it != storeQueue.end(); ++it) {
            if (!it->valid() || it->completed())
                continue;
            const auto &st = it->instruction();
            if (st->seqNum >= load_inst->seqNum)
                continue;
            if (!st->effAddrValid() || !it->hasRequest()) {
                Addr earlyAddr;
                unsigned earlyBytes;
                if (earlySpmStoreRange(*it, earlyAddr, earlyBytes)) {
                    const auto sr = SpmForwardRange::decode(earlyAddr, earlyBytes);
                    const auto lr = SpmForwardRange::decode(load_s_raw, size);
                    if (!sr.overlaps(lr)) {
                        ++stats.spmEarlyAddrDisjoint;
                        continue;
                    }
                    ++stats.spmEarlyAddrOverlap;
                }
                pendingSpmStore = true;
                break;
            }
            const Addr addr = it->request()->mainReq()->getPaddr();
            const int first = spm_get_way(addr);
            const int last = first +
                (spm_get_offset(addr) + it->size() - 1) / 64;
            if (spm_get_slot(addr) == slot && first <= end_way &&
                last >= start_way) {
                pendingSpmStore = true;
                break;
            }
        }
        if (pendingSpmStore) {
            ++stats.rescheduledLoads;
            iewStage->instQueue.retrySpmAddrLoad(load_inst);
            return NoFault;
        }

        DPRINTF(SPM_CPU,
            "[SPM] Load Issue to Cache: SN=%llu Addr=%#x [S:%d W:%d]\n",
            load_inst->seqNum, load_s_raw, slot, start_way);

        auto rawWait = spmRawWaitStart.find(load_inst->seqNum);
        if (rawWait != spmRawWaitStart.end()) {
            stats.spmRawWaitCycles +=
                (curTick() - rawWait->second) / cpu->clockPeriod();
            spmRawWaitStart.erase(rawWait);
        }

        if (!load_inst->memData) 
        {
            load_inst->memData = new uint8_t[request->mainReq()->getSize()];
            DPRINTF(LSQUnit, "[SPM] Allocated %d bytes for SPM load\n", 
                    request->mainReq()->getSize());
        }

        request->buildPackets();
        request->sendPacketToCache();
        // WAR tracking entry is cleared in recvTimingResp when SPMLoadResp
        // arrives — only then is the SPM read guaranteed complete at L2.
        // (Clearing here at send-time is insufficient: with prefetchers,
        //  CPTOSPMReq can arrive and be serviced by L2 before SPMLoadReq
        //  is processed, causing a WAR violation.)
        if (!request->isSent())
        {
            DPRINTF(SPM_CPU, "[SPM] Load SN=%llu Blocked by Port/Cache Busy\n", load_inst->seqNum);
            ++stats.spmPortBlockedEvents;
            iewStage->blockMemInst(load_inst);
        } else {
            spmLoadServiceStart.emplace(load_inst->seqNum, curTick());
        }

        return NoFault;
    }

    else
    {
        // SPMWB has left the regular SQ, but its coherent destination remains
        // pending in the existing SPM shadow entry. Do not read stale data or
        // forward the encoded SPM-address operand as if it were store data.
        bool bridge_blocked = false;
        auto &shadows = lsq->getSpmUnit(lsqID).spmShadowTable;
        const Addr load_start = request->mainReq()->getPaddr();
        const Addr load_end = load_start + request->mainReq()->getSize();
        for (auto &[seq, entry] : shadows) {
            if (!entry.isWriteback || entry.ready || seq >= load_inst->seqNum)
                continue;
            if (!entry.addrValid || (load_start < entry.regularAddr + 64 &&
                                    load_end > entry.regularAddr)) {
                bridge_blocked = true;
                if (std::find(entry.waitingLoads.begin(), entry.waitingLoads.end(),
                              load_inst) == entry.waitingLoads.end())
                    entry.waitingLoads.push_back(load_inst);
            }
        }
        if (bridge_blocked) {
            CFL_INST("ORDER_WAIT", load_inst, "reason=pending_spmwb");
            load_inst->clearIssued();
            load_inst->clearCanIssue();
            load_inst->effAddrValid(false);
            ++stats.rescheduledLoads;
            return NoFault;
        }
        if (request->mainReq()->isStrictlyOrdered() &&
            (load_idx != loadQueue.head() || !load_inst->isAtCommit())) {
            CFL_INST("ORDER_WAIT", load_inst, "reason=strict_order");
            // Tell IQ/mem dep unit that this instruction will need to be
            // rescheduled eventually
            iewStage->rescheduleMemInst(load_inst);
            load_inst->clearIssued();
            load_inst->effAddrValid(false);
            ++stats.rescheduledLoads;
            DPRINTF(LSQUnit, "Strictly ordered load [sn:%lli] PC %s\n",
                    load_inst->seqNum, load_inst->pcState());

            // Must delete request now that it wasn't handed off to
            // memory.  This is quite ugly.  @todo: Figure out the proper
            // place to really handle request deletes.
            load_entry.setRequest(nullptr);
            request->discard();
            return std::make_shared<GenericISA::M5PanicFault>(
                "Strictly ordered load [sn:%llx] PC %s\n",
                load_inst->seqNum, load_inst->pcState());
        }

        DPRINTF(LSQUnit, "Read called, load idx: %i, store idx: %i, "
                "storeHead: %i addr: %#x%s\n",
                load_idx - 1, load_inst->sqIt._idx, storeQueue.head() - 1,
                request->mainReq()->getPaddr(), request->isSplit() ? " split" :
                "");

        if (request->mainReq()->isLLSC()) {
            // Disable recording the result temporarily.  Writing to misc
            // regs normally updates the result, but this is not the
            // desired behavior when handling store conditionals.
            load_inst->recordResult(false);
            load_inst->tcBase()->getIsaPtr()->handleLockedRead(load_inst.get(),
                    request->mainReq());
            load_inst->recordResult(true);
        }

        if (request->mainReq()->isLocalAccess()) {
            assert(!load_inst->memData);
            load_inst->memData = new uint8_t[MaxDataBytes];

            gem5::ThreadContext *thread = cpu->tcBase(lsqID);
            PacketPtr main_pkt = new Packet(request->mainReq(), MemCmd::ReadReq);

            main_pkt->dataStatic(load_inst->memData);

            Cycles delay = request->mainReq()->localAccessor(thread, main_pkt);

            WritebackEvent *wb = new WritebackEvent(load_inst, main_pkt, this);
            cpu->schedule(wb, cpu->clockEdge(delay));
            return NoFault;
        }

        // Check the SQ for any previous stores that might lead to forwarding
        auto store_it = load_inst->sqIt;
        assert (store_it >= storeWBIt);
        // End once we've reached the top of the LSQ
        while (store_it != storeWBIt && !load_inst->isDataPrefetch()) {
            // Move the index to one younger
            store_it--;
            assert(store_it->valid());
            assert(store_it->instruction()->seqNum < load_inst->seqNum);
            // SPMCP reads the coherent source. Its SQ payload is a slot
            // address, never coherent store data suitable for forwarding.
            if (store_it->instruction()->memReqFlags & Request::CP_TO_SPM)
                continue;
            int store_size = store_it->size();

            // Cache maintenance instructions go down via the store
            // path but they carry no data and they shouldn't be
            // considered for forwarding
            if (store_size != 0 && !store_it->instruction()->strictlyOrdered() &&
                !(store_it->request()->mainReq() &&
                store_it->request()->mainReq()->isCacheMaintenance())) {
                assert(store_it->instruction()->effAddrValid());

                // Check if the store data is within the lower and upper bounds of
                // addresses that the request needs.
                auto req_s = request->mainReq()->getVaddr();
                auto req_e = req_s + request->mainReq()->getSize();
                auto st_s = store_it->instruction()->effAddr;
                auto st_e = st_s + store_size;

                bool store_has_lower_limit = req_s >= st_s;
                bool store_has_upper_limit = req_e <= st_e;
                bool lower_load_has_store_part = req_s < st_e;
                bool upper_load_has_store_part = req_e > st_s;

                auto coverage = AddrRangeCoverage::NoAddrRangeCoverage;

                // If the store entry is not atomic (atomic does not have valid
                // data), the store has all of the data needed, and
                // the load is not LLSC, then
                // we can forward data from the store to the load
                if (!store_it->instruction()->isAtomic() &&
                    store_has_lower_limit && store_has_upper_limit &&
                    !request->mainReq()->isLLSC()) {

                    const auto& store_req = store_it->request()->mainReq();
                    coverage = store_req->isMasked() ?
                        AddrRangeCoverage::PartialAddrRangeCoverage :
                        AddrRangeCoverage::FullAddrRangeCoverage;
                } else if (
                    // This is the partial store-load forwarding case where a store
                    // has only part of the load's data and the load isn't LLSC
                    (!request->mainReq()->isLLSC() &&
                    ((store_has_lower_limit && lower_load_has_store_part) ||
                    (store_has_upper_limit && upper_load_has_store_part) ||
                    (lower_load_has_store_part && upper_load_has_store_part))) ||
                    // The load is LLSC, and the store has all or part of the
                    // load's data
                    (request->mainReq()->isLLSC() &&
                    ((store_has_lower_limit || upper_load_has_store_part) &&
                    (store_has_upper_limit || lower_load_has_store_part))) ||
                    // The store entry is atomic and has all or part of the load's
                    // data
                    (store_it->instruction()->isAtomic() &&
                    ((store_has_lower_limit || upper_load_has_store_part) &&
                    (store_has_upper_limit || lower_load_has_store_part)))) {

                    coverage = AddrRangeCoverage::PartialAddrRangeCoverage;
                }

                if (coverage == AddrRangeCoverage::FullAddrRangeCoverage) {
                    // Get shift amount for offset into the store's data.
                    int shift_amt = request->mainReq()->getVaddr() -
                        store_it->instruction()->effAddr;

                    // Allocate memory if this is the first time a load is issued.
                    if (!load_inst->memData) {
                        load_inst->memData =
                            new uint8_t[request->mainReq()->getSize()];
                    }
                    if (store_it->isAllZeros())
                        memset(load_inst->memData, 0,
                                request->mainReq()->getSize());
                    else
                        memcpy(load_inst->memData,
                            store_it->data() + shift_amt,
                            request->mainReq()->getSize());

                    DPRINTF(LSQUnit, "Forwarding from store idx %i to load to "
                            "addr %#x\n", store_it._idx,
                            request->mainReq()->getVaddr());

                    PacketPtr data_pkt = new Packet(request->mainReq(),
                            MemCmd::ReadReq);
                    data_pkt->dataStatic(load_inst->memData);

                    // hardware transactional memory
                    // Store to load forwarding within a transaction
                    // This should be okay because the store will be sent to
                    // the memory subsystem and subsequently get added to the
                    // write set of the transaction. The write set has a stronger
                    // property than the read set, so the load doesn't necessarily
                    // have to be there.
                    assert(!request->mainReq()->isHTMCmd());
                    if (load_inst->inHtmTransactionalState()) {
                        assert (!storeQueue[store_it._idx].completed());
                        assert (
                            storeQueue[store_it._idx].instruction()->
                            inHtmTransactionalState());
                        assert (
                            load_inst->getHtmTransactionUid() ==
                            storeQueue[store_it._idx].instruction()->
                            getHtmTransactionUid());
                        data_pkt->setHtmTransactional(
                            load_inst->getHtmTransactionUid());
                        DPRINTF(HtmCpu, "HTM LD (ST2LDF) "
                        "pc=0x%lx - vaddr=0x%lx - "
                        "paddr=0x%lx - htmUid=%u\n",
                        load_inst->pcState().instAddr(),
                        data_pkt->req->hasVaddr() ?
                            data_pkt->req->getVaddr() : 0lu,
                        data_pkt->getAddr(),
                        load_inst->getHtmTransactionUid());
                    }

                    if (request->isAnyOutstandingRequest()) {
                        assert(request->_numOutstandingPackets > 0);
                        // There are memory requests packets in flight already.
                        // This may happen if the store was not complete the
                        // first time this load got executed. Signal the senderSate
                        // that response packets should be discarded.
                        request->discard();
                        // Avoid checking snoops on this discarded request.
                        load_entry.setRequest(nullptr);
                    }

                    WritebackEvent *wb = new WritebackEvent(load_inst, data_pkt,
                            this);

                    // We'll say this has a 1 cycle load-store forwarding latency
                    // for now.
                    // @todo: Need to make this a parameter.
                    cpu->schedule(wb, curTick());

                    // Don't need to do anything special for split loads.
                    ++stats.forwLoads;

                    CFL_INST("STORE_FORWARD", load_inst, "store_sn=%llu",
                             store_it->instruction()->seqNum);

                    return NoFault;
                } else if (
                        coverage == AddrRangeCoverage::PartialAddrRangeCoverage) {
                    CFL_INST("ORDER_WAIT", load_inst, "reason=partial_store_overlap store_sn=%llu",
                             store_it->instruction()->seqNum);
                    // If it's already been written back, then don't worry about
                    // stalling on it.
                    if (store_it->completed()) {
                        panic("Should not check one of these");
                        continue;
                    }

                    // Must stall load and force it to retry, so long as it's the
                    // oldest load that needs to do so.
                    if (!stalled ||
                        (stalled &&
                        load_inst->seqNum <
                        loadQueue[stallingLoadIdx].instruction()->seqNum)) {
                        stalled = true;
                        stallingStoreIsn = store_it->instruction()->seqNum;
                        stallingLoadIdx = load_idx;
                    }

                    // Tell IQ/mem dep unit that this instruction will need to be
                    // rescheduled eventually
                    iewStage->rescheduleMemInst(load_inst);
                    load_inst->clearIssued();
                    load_inst->effAddrValid(false);
                    ++stats.rescheduledLoads;

                    // Do not generate a writeback event as this instruction is not
                    // complete.
                    DPRINTF(LSQUnit, "Load-store forwarding mis-match. "
                            "Store idx %i to load addr %#x\n",
                            store_it._idx, request->mainReq()->getVaddr());

                    // Must discard the request.
                    request->discard();
                    load_entry.setRequest(nullptr);
                    return NoFault;
                }
            }
        }

        // If there's no forwarding case, then go access memory
        CFL_INST("ELIGIBLE", load_inst, "req=%p va=%#x bytes=%u",
                 request, request->mainReq()->getVaddr(), request->mainReq()->getSize());
        DPRINTF(LSQUnit, "Doing memory access for inst [sn:%lli] PC %s\n",
                load_inst->seqNum, load_inst->pcState());

        // Allocate memory if this is the first time a load is issued.
        if (!load_inst->memData) {
            load_inst->memData = new uint8_t[request->mainReq()->getSize()];
        }


        // hardware transactional memory
        if (request->mainReq()->isHTMCmd()) {
            // this is a simple sanity check
            // the Ruby cache controller will set
            // memData to 0x0ul if successful.
            *load_inst->memData = (uint64_t) 0x1ull;
        }

        // For now, load throughput is constrained by the number of
        // load FUs only, and loads do not consume a cache port (only
        // stores do).
        // @todo We should account for cache port contention
        // and arbitrate between loads and stores.

        // if we the cache is not blocked, do cache access
        request->buildPackets();
        request->sendPacketToCache();
        if (!request->isSent())
            iewStage->blockMemInst(load_inst);

        return NoFault;
    }


}

bool
LSQUnit::earlySpmStoreRange(SQEntry &entry, Addr &addr, unsigned &bytes)
{
    if (!spmEarlyAddress || !isSpmLsq)
        return false;
    if (entry.spmEarlyValid) {
        if (curTick() < entry.spmEarlyReady) {
            ++stats.spmEarlyAddrBusy;
            return false;
        }
        addr = entry.spmEarlyAddr;
        bytes = entry.spmEarlyBytes;
        return true;
    }
    const auto &inst = entry.instruction();
    const auto &si = inst->staticInst;
    // Only the supported physical-address SVE immediate-store form exposes
    // this hook. Scalar, bridge, unsupported and unknown-base cases still wait.
    if (!si->isSPMMem() || !si->isStore() || si->numSrcRegs() == 0 ||
        si->srcRegIdx(0).classValue() != IntRegClass || !inst->readySrcIdx(0))
        return false;
    if (curTick() < nextSpmAddrIssue) {
        ++stats.spmEarlyAddrBusy;
        return false;
    }
    if (!si->spmStoreAddress(inst->getRegOperand(si.get(), 0),
                              inst->tcBase(), addr, bytes) ||
        !SpmForwardRange::decode(addr, bytes).valid())
        return false;
    // One dedicated address-only unit per core: one issue/cycle, one cycle
    // latency. It adds control/AGU logic, not entries or data storage. Does NOT
    // mark the instruction executed, data-ready, or safe to commit.
    nextSpmAddrIssue = cpu->clockEdge(Cycles(1));
    entry.spmEarlyValid = true;
    entry.spmEarlyAddr = addr;
    entry.spmEarlyBytes = bytes;
    entry.spmEarlyReady = nextSpmAddrIssue;
    ++stats.spmEarlyAddrIssued;
    DPRINTF(SPM_CPU, "[SPM] EARLY_ADDR store=%llu addr=%#x bytes=%u ready=%llu\n",
            inst->seqNum, addr, bytes, entry.spmEarlyReady);
    return false;
}

Fault
LSQUnit::write(LSQRequest *request, uint8_t *data, ssize_t store_idx)
{
    assert(storeQueue[store_idx].valid());

    DPRINTF(LSQUnit, "Doing write to store idx %i, addr %#x | storeHead:%i "
            "[sn:%llu]\n",
            store_idx - 1, request->req()->getPaddr(), storeQueue.head() - 1,
            storeQueue[store_idx].instruction()->seqNum);

    storeQueue[store_idx].setRequest(request);
    unsigned size = request->_size;
    const auto &early = storeQueue[store_idx];
    panic_if(early.spmEarlyValid &&
             (early.spmEarlyAddr != request->mainReq()->getPaddr() ||
              early.spmEarlyBytes != size),
             "SPM early store address/size differs from actual execution");
    storeQueue[store_idx].size() = size;
    bool store_no_data =
        request->mainReq()->getFlags() & Request::STORE_NO_DATA;
    storeQueue[store_idx].isAllZeros() = store_no_data;
    assert(size <= SQEntry::DataSize || store_no_data);

    // copy data into the storeQueue only if the store request has valid data
    if (!(request->req()->getFlags() & Request::CACHE_BLOCK_ZERO) &&
        !request->req()->isCacheMaintenance() &&
        !request->req()->isAtomic())
        memcpy(storeQueue[store_idx].data(), data, size);

    // This function only writes the data to the store queue, so no fault
    // can happen here.
    return NoFault;
}

InstSeqNum
LSQUnit::getLoadHeadSeqNum()
{
    if (loadQueue.front().valid())
        return loadQueue.front().instruction()->seqNum;
    else
        return 0;
}

InstSeqNum
LSQUnit::getStoreHeadSeqNum()
{
    if (storeQueue.front().valid())
        return storeQueue.front().instruction()->seqNum;
    else
        return 0;
}

//jq86
DynInstPtr LSQUnit::findInstBySeqNum(InstSeqNum seqNum) const
{
    // 在 store queue 中查找
    for (const auto& entry : storeQueue) {
        if (entry.valid() && 
            entry.instruction() && 
            entry.instruction()->seqNum == seqNum) {
            return entry.instruction();
        }
    }

    for (const auto& entry : loadQueue) {
        if (entry.valid() && 
            entry.instruction() && 
            entry.instruction()->seqNum == seqNum) {
            return entry.instruction();
        }
    }
    
    return nullptr;
}


void
LSQUnit::reserveSpmStore(const DynInstPtr& inst, bool addr_known, Addr regular_addr)
{
    assert(isSpmLsq);

    // Recycle completed entries; dispatch checks the shared SPM SQ capacity.
    cleanupRetiredShadow();

    const InstSeqNum seq = inst->seqNum;

    if (addr_known) {
        Addr addr = inst->spmAddr;
        DPRINTF(SPM_CPU,
            "[SPM] Reserve Shadow (addr): SN=%llu Addr=%#x [S:%d W:%d O:%d]\n",
            seq, addr, spm_get_slot(addr), spm_get_way(addr), spm_get_offset(addr));
    } else {
        DPRINTF(SPM_CPU,
            "[SPM] Reserve Shadow (dispatch): SN=%llu addr unknown\n", seq);
    }

    // If entry already exists (dispatch-time entry), update with real spmAddr
    auto it = spmShadowTable.find(seq);
    if (it != spmShadowTable.end()) {
        auto &e = it->second;
        if (addr_known) {
            e.spmAddr   = inst->spmAddr;
            e.regularAddr = regular_addr;
            e.addrValid = true;
            e.wayCount =
                (inst->memReqFlags & Request::SPMCP_BATCH3) ? 3 : 1;
            if ((inst->memReqFlags & Request::SPM_WRITEBACK) &&
                (inst->spmAddr & (Addr(1) << 57))) e.wayCount = 2;
        }
        e.ready   = false;
        e.retired = false;
        return;
    }

    // Insert new entry
    if (boundedTracking) {
        assert(numFreeStoreEntries() > 0);
        ++stats.boundedShadowAllocations;
    }
    SpmShadowEntry newEntry;
    newEntry.seqNum      = seq;
    newEntry.spmAddr     = addr_known ? inst->spmAddr : 0;
    newEntry.regularAddr = regular_addr;
    newEntry.addrValid   = addr_known;
    newEntry.ready       = false;
    newEntry.retired     = false;
    newEntry.isCopy      = inst->memReqFlags & Request::CP_TO_SPM;
    newEntry.isWriteback = inst->memReqFlags & Request::SPM_WRITEBACK;
    newEntry.wayCount     =
        (inst->memReqFlags & Request::SPMCP_BATCH3) ? 3 : 1;
    if (addr_known && (inst->memReqFlags & Request::SPM_WRITEBACK) &&
        (inst->spmAddr & (Addr(1) << 57))) newEntry.wayCount = 2;

    DPRINTF(LSQUnit,
        "SPM reserve(new): seq=%llu spmAddr=%#x addrValid=%d copy=%d wb=%d\n",
        seq, newEntry.spmAddr, newEntry.addrValid, newEntry.isCopy, newEntry.isWriteback);
    spmShadowTable.emplace(seq, std::move(newEntry));
}


void
LSQUnit::markShadowReady(InstSeqNum seq)
{
    assert(isSpmLsq);

    auto it = spmShadowTable.find(seq);
    if (it != spmShadowTable.end()) {
        it->second.ready = true;
        DPRINTF(SPM_CPU,
            "[SPM] Shadow Ready: SN=%llu Addr=%#x [S:%d W:%d]\n",
            seq, it->second.spmAddr,
            spm_get_slot(it->second.spmAddr), spm_get_way(it->second.spmAddr));
    }
}

bool
LSQUnit::hasPendingWriteback() const
{
    for (const auto &[seq, entry] : spmShadowTable)
        // Both committed bridge directions must drain before a barrier.
        // The regular SQ can already have released its entry after dispatch.
        if (entry.retired && !entry.ready)
            return true;
    return false;
}


void LSQUnit::retireShadow(InstSeqNum seq)
{
    assert(isSpmLsq);

    auto it = spmShadowTable.find(seq);
    if (it != spmShadowTable.end()) {
        it->second.retired = true;
        DPRINTF(LSQUnit,
            "SPM-LSQ: Shadow retired [sn:%llu spmAddr=%#x]\n",
            it->second.seqNum, it->second.spmAddr);
    } else {
        DPRINTF(LSQUnit,
            "SPM-LSQ: retireShadow(%llu) not found\n", seq);
    }
}



void LSQUnit::cleanupRetiredShadow()
{
    if (!isSpmLsq)
        return;

    size_t cleaned = 0;
    for (auto it = spmShadowTable.begin(); it != spmShadowTable.end(); ) {
        const auto &e = it->second;
        if (e.ready && (e.retired || e.waitingLoads.empty())) {
            it = spmShadowTable.erase(it);
            cleaned++;
        } else {
            ++it;
        }
    }

    if (cleaned > 0) {
        DPRINTF(LSQUnit, "Cleaned %zu retired shadow entries\n", cleaned);
        if (boundedTracking) {
            stats.boundedShadowReleases += cleaned;
            cpu->wakeCPU();
            iewStage->updateLSQNextCycle = true;
            iewStage->activityThisCycle();
        }
    }
}



} // namespace o3
} // namespace gem5
