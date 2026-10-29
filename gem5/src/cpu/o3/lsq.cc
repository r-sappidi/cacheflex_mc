/*
 * Copyright (c) 2011-2012, 2014, 2017-2019, 2021 ARM Limited
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
 * Copyright (c) 2005-2006 The Regents of The University of Michigan
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

#include "cpu/o3/lsq.hh"
#include "mem/cacheflex_reduction.hh"
#include "cpu/o3/cacheflex_load_trace.hh"

#include <algorithm>
#include <list>
#include <string>

#include "base/compiler.hh"
#include "base/logging.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/iew.hh"
#include "cpu/o3/limits.hh"
#include "debug/Drain.hh"
#include "debug/Fetch.hh"
#include "debug/HtmCpu.hh"
#include "debug/LSQ.hh"
#include "debug/Writeback.hh"
#include "params/BaseO3CPU.hh"
#include "sim/process.hh"

namespace gem5
{

namespace o3
{

LSQ::DcachePort::DcachePort(LSQ *_lsq, CPU *_cpu,
                          const std::string &suffix) :
    RequestPort(_cpu->name() + suffix), lsq(_lsq), cpu(_cpu)
{}

// weihang
LSQ::SpmPort::SpmPort(LSQ *_lsq, CPU *_cpu, const std::string &suffix)
  : RequestPort(_cpu->name() + suffix), lsq(_lsq), cpu(_cpu)
{}


void
LSQ::SpmPort::recvFunctionalSnoop(PacketPtr pkt)
{
    DPRINTF(LSQ, "SPMPort %s ignoring functional snoop addr=%#x\n",
            name(), pkt->getAddr());
}

void
LSQ::SpmPort::recvTimingSnoopReq(PacketPtr pkt)
{
    DPRINTF(LSQ, "SPMPort %s ignoring timing snoop addr=%#x\n",
            name(), pkt->getAddr());
}



LSQ::LSQ(CPU *cpu_ptr, IEW *iew_ptr, const BaseO3CPUParams &params)
    : cpu(cpu_ptr), iewStage(iew_ptr),
      _cacheBlocked(false),
      independentBridgeRetries(params.cacheFlexIndependentBridge),
      independentReductionRetries(params.cacheFlexIndependentReduction),
      cacheStorePorts(params.cacheStorePorts), usedStorePorts(0),
      cacheLoadPorts(params.cacheLoadPorts), usedLoadPorts(0),
      spmLoadPorts(params.spmLoadPorts), usedSpmLoadPorts(0),
      spmStorePorts(params.spmStorePorts), usedSpmStorePorts(0),
      waitingForStaleTranslation(false),
      staleTranslationWaitTxnId(0),
      lsqPolicy(params.smtLSQPolicy),
      LQEntries(params.LQEntries),
      SQEntries(params.SQEntries),
      maxLQEntries(maxLSQAllocation(lsqPolicy, LQEntries, params.numThreads,
                  params.smtLSQThreshold)),
      maxSQEntries(maxLSQAllocation(lsqPolicy, SQEntries, params.numThreads,
                  params.smtLSQThreshold)),
      dcachePort(this, cpu_ptr),
      spmPort(this, cpu_ptr), // weihang
      bridgePort(this, cpu_ptr),
      reductionPort(this, cpu_ptr),
      numThreads(params.numThreads)
{
    assert(numThreads > 0 && numThreads <= MaxThreads);

    //**********************************************
    //************ Handle SMT Parameters ***********
    //**********************************************

    /* Run SMT olicy checks. */
        if (lsqPolicy == SMTQueuePolicy::Dynamic) {
        DPRINTF(LSQ, "LSQ sharing policy set to Dynamic\n");
    } else if (lsqPolicy == SMTQueuePolicy::Partitioned) {
        DPRINTF(Fetch, "LSQ sharing policy set to Partitioned: "
                "%i entries per LQ | %i entries per SQ\n",
                maxLQEntries,maxSQEntries);
    } else if (lsqPolicy == SMTQueuePolicy::Threshold) {

        assert(params.smtLSQThreshold > params.LQEntries);
        assert(params.smtLSQThreshold > params.SQEntries);

        DPRINTF(LSQ, "LSQ sharing policy set to Threshold: "
                "%i entries per LQ | %i entries per SQ\n",
                maxLQEntries,maxSQEntries);
    } else {
        panic("Invalid LSQ sharing policy. Options are: Dynamic, "
                    "Partitioned, Threshold");
    }

    thread.reserve(numThreads);

    //weihang
    spmThread.reserve(numThreads);
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        thread.emplace_back(maxLQEntries, maxSQEntries);
        thread[tid].init(cpu, iew_ptr, params, this, tid);
        thread[tid].setDcachePort(&dcachePort);

        spmThread.emplace_back(maxLQEntries, maxSQEntries); // can be modified not to be that large
        spmThread[tid].isSpmLsq = true;
        spmThread[tid].init(cpu, iew_ptr, params, this, tid);
        spmThread[tid].setDcachePort(&spmPort);
    }
    
}

//weihang
// static inline bool isSpmFlags(const Request::Flags& f) {
//     return (f.isSet(Request::SPM_LOAD) || f.isSet(Request::SPM_STORE) || f.isSet(Request::CP_TO_SPM) || f.isSet(Request::SPM_WRITEBACK));
// }

std::string
LSQ::name() const
{
    return iewStage->name() + ".lsq";
}

void
LSQ::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
    assert(activeThreads != 0);
}

void
LSQ::drainSanityCheck() const
{
    assert(isDrained());

    for (ThreadID tid = 0; tid < numThreads; tid++)
        thread[tid].drainSanityCheck();
}

bool
LSQ::isDrained() const
{
    bool drained(true);
    for (ThreadID tid = 0; tid < thread.size(); ++tid)
        if (cfReduction::busy(cpu->system, cpu->tcBase(tid)->contextId()))
            drained = false;

    if (!lqEmpty()) {
        DPRINTF(Drain, "Not drained, LQ not empty.\n");
        drained = false;
    }

    if (!sqEmpty()) {
        DPRINTF(Drain, "Not drained, SQ not empty.\n");
        drained = false;
    }

    if (!activeDmas.empty()) {
        DPRINTF(Drain, "Not drained, %zu SPM DMA command(s) active.\n",
                activeDmas.size());
        drained = false;
    }

    return drained;
}

void
LSQ::takeOverFrom()
{
    assert(activeDmas.empty());
    usedStorePorts = 0;
    _cacheBlocked = false;

    for (ThreadID tid = 0; tid < numThreads; tid++) {
        thread[tid].takeOverFrom();
    }
}

void
LSQ::tick()
{
    // Re-issue loads which got blocked on the per-cycle port limit.
    if ((usedLoadPorts >= cacheLoadPorts || usedSpmLoadPorts >= spmLoadPorts)
        && !_cacheBlocked)
        iewStage->cacheUnblocked();

    usedLoadPorts = 0;
    usedStorePorts = 0;
    usedSpmLoadPorts = 0;
    usedSpmStorePorts = 0;

    // DMA descriptors retire after their first accepted line.  The engine
    // continues feeding one line per active descriptor per cycle, independent
    // of the SQ/ROB entry that launched it.
    for (auto it = activeDmas.begin(); it != activeDmas.end(); ) {
        DmaSPMCPRequest *dma = *it;
        if (!dma->allIssued() && !_cacheBlocked)
            dma->sendPacketToCache();
        if (dma->allIssued())
            it = activeDmas.erase(it);
        else
            ++it;
    }
}

void
LSQ::registerDma(DmaSPMCPRequest *request)
{
    request->startBackground();
    if (std::find(activeDmas.begin(), activeDmas.end(), request) ==
        activeDmas.end())
        activeDmas.push_back(request);
}

bool
LSQ::cacheBlocked() const
{
    return _cacheBlocked;
}

void
LSQ::cacheBlocked(bool v)
{
    _cacheBlocked = v;
}

bool
LSQ::cachePortAvailable(bool is_load) const
{
    bool ret;
    if (is_load) {
        ret  = usedLoadPorts < cacheLoadPorts;
    } else {
        ret  = usedStorePorts < cacheStorePorts;
    }
    return ret;
}

void
LSQ::cachePortBusy(bool is_load)
{
    assert(cachePortAvailable(is_load));
    if (is_load) {
        usedLoadPorts++;
    } else {
        usedStorePorts++;
    }
}

int
LSQ::spmPortsAvailable(bool is_load) const
{
    if (is_load) {
        return std::max(0, spmLoadPorts - usedSpmLoadPorts);
    } else {
        return std::max(0, spmStorePorts - usedSpmStorePorts);
    }
}

void
LSQ::spmPortBusy(bool is_load, int count)
{
    if (is_load) {
        usedSpmLoadPorts += count;
    } else {
        usedSpmStorePorts += count;
    }
}

void
LSQ::insertLoad(const DynInstPtr &load_inst)
{
    ThreadID tid = load_inst->threadNumber;
    
    //weihang 
    if (load_inst->memReqFlags & Request::SPM_LOAD)
    {
        spmThread[tid].insertLoad(load_inst);
    }
    else 
    {
        thread[tid].insertLoad(load_inst);
    }
   
    // thread[tid].insertLoad(load_inst);
}

void
LSQ::insertStore(const DynInstPtr &store_inst)
{
    ThreadID tid = store_inst->threadNumber;


    //jq86
    if (store_inst->memReqFlags & Request::SPM_STORE)
    {
        spmThread[tid].insertStore(store_inst);
    }

    else if (store_inst->memReqFlags & Request::CP_TO_SPM || store_inst->memReqFlags & Request::SPM_WRITEBACK)
    {
        // store to both LSQs
        thread[tid].insertStore(store_inst);
        //jq86 — dispatch-time reservation: seqNum known, spmAddr not yet resolved
        spmThread[tid].reserveSpmStore(store_inst, false);
        DPRINTF(LSQ, "insert cp2spm seq=%llu src=%#x spm=%#x\n",
        store_inst->seqNum, store_inst->effAddr, store_inst->spmAddr);
    } 
    else
    {
        thread[tid].insertStore(store_inst);
    }
}

Fault
LSQ::executeLoad(const DynInstPtr &inst)
{
    ThreadID tid = inst->threadNumber;
    
    //weihang

    if (inst->memReqFlags & Request::SPM_LOAD)
    {
        return spmThread[tid].executeLoad(inst);
    }
    else
    {
        return thread[tid].executeLoad(inst);
    }
    
}

Fault
LSQ::executeStore(const DynInstPtr &inst)
{
    ThreadID tid = inst->threadNumber;
   
    //weihang
    if (inst->memReqFlags & Request::SPM_STORE) 
    {   
        return spmThread[tid].executeStore(inst);
    }
    else
    {
        return thread[tid].executeStore(inst);
    }

}

void
LSQ::commitLoads(InstSeqNum &youngest_inst, ThreadID tid)
{
    thread.at(tid).commitLoads(youngest_inst);
    //jq86
    spmThread.at(tid).commitLoads(youngest_inst);

}

void
LSQ::commitStores(InstSeqNum &youngest_inst, ThreadID tid)
{
    thread.at(tid).commitStores(youngest_inst);
    spmThread.at(tid).commitStores(youngest_inst);
}

void
LSQ::writebackStores()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (numStoresToWB(tid) > 0) {
            DPRINTF(Writeback,"[tid:%i] Writing back stores. %i stores "
                "available for Writeback.\n", tid, numStoresToWB(tid));
        }
        //jq86
        spmThread[tid].writebackStores();
        thread[tid].writebackStores();
    }
}

void
LSQ::squash(const InstSeqNum &squashed_num, ThreadID tid)
{
    thread.at(tid).squash(squashed_num);
    spmThread[tid].squash(squashed_num);
}

bool
LSQ::violation()
{
    /* Answers: Does Anybody Have a Violation?*/
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (thread[tid].violation())
            return true;
    }

    return false;
}

bool LSQ::violation(ThreadID tid) { return thread.at(tid).violation(); }

DynInstPtr
LSQ::getMemDepViolator(ThreadID tid)
{
    return thread.at(tid).getMemDepViolator();
}

int
LSQ::getLoadHead(ThreadID tid)
{
    return thread.at(tid).getLoadHead();
}

InstSeqNum
LSQ::getLoadHeadSeqNum(ThreadID tid)
{
    return thread.at(tid).getLoadHeadSeqNum();
}

int
LSQ::getStoreHead(ThreadID tid)
{
    return thread.at(tid).getStoreHead();
}

InstSeqNum
LSQ::getStoreHeadSeqNum(ThreadID tid)
{
    return thread.at(tid).getStoreHeadSeqNum();
}

int LSQ::getCount(ThreadID tid) { return thread.at(tid).getCount(); }

int LSQ::numLoads(ThreadID tid) { return thread.at(tid).numLoads(); }

int LSQ::numStores(ThreadID tid) { return thread.at(tid).numStores(); }

int
LSQ::numHtmStarts(ThreadID tid) const
{
    if (tid == InvalidThreadID)
        return 0;
    else
        return thread[tid].numHtmStarts();
}
int
LSQ::numHtmStops(ThreadID tid) const
{
    if (tid == InvalidThreadID)
        return 0;
    else
        return thread[tid].numHtmStops();
}

void
LSQ::resetHtmStartsStops(ThreadID tid)
{
    if (tid != InvalidThreadID)
        thread[tid].resetHtmStartsStops();
}

uint64_t
LSQ::getLatestHtmUid(ThreadID tid) const
{
    if (tid == InvalidThreadID)
        return 0;
    else
        return thread[tid].getLatestHtmUid();
}

void
LSQ::setLastRetiredHtmUid(ThreadID tid, uint64_t htmUid)
{
    if (tid != InvalidThreadID)
        thread[tid].setLastRetiredHtmUid(htmUid);
}

void
LSQ::recvReqRetry()
{
    iewStage->cacheUnblocked();
    cacheBlocked(false);

    for (ThreadID tid : *activeThreads) {
        thread[tid].recvRetry();

        //weihang
        spmThread[tid].recvRetry();
    }
}

LSQUnit&
LSQ::getSpmUnit(ThreadID tid)
{
    return spmThread[tid];
}

void
LSQ::completeDataAccess(PacketPtr pkt)
{
    LSQRequest *request = dynamic_cast<LSQRequest*>(pkt->senderState);
    // thread[cpu->contextToThread(request->contextId())]
    //     .completeDataAccess(pkt);
    //jq86
    ThreadID tid = cpu->contextToThread(request->contextId());

    if (pkt->SPMresp || pkt->SPM_LDST_resp) 
        spmThread[tid].completeDataAccess(pkt);
    else
        thread[tid].completeDataAccess(pkt);
}

bool
LSQ::recvTimingResp(PacketPtr pkt)
{
    if (pkt->isError())
        DPRINTF(LSQ, "Got error packet back for address: %#X\n",
                pkt->getAddr());

    LSQRequest *request = dynamic_cast<LSQRequest*>(pkt->senderState);
    panic_if(!request, "Got packet back with unknown sender state\n");
    CFL_INST("FRAG_RESPONSE", request->instruction(),
             "id=%p req=%p pa=%#x bytes=%u squashed=%d",
             pkt, request, pkt->getAddr(), pkt->getSize(),
             request->instruction()->isSquashed());

    // Ruby may return both CPU ports through the shared sequencer response
    // path, bypassing SpmPort::recvTimingResp().  Recover the micro O3's SPM
    // response classification from the request flags before selecting an LSQ.
    if (pkt->req->isSPMCPFetch()) {
        pkt->SPMresp = true;
    } else if (pkt->req->isSPMLD() || pkt->req->isSPMST() ||
               pkt->req->isSPMWBRead() || pkt->req->isSPMWBStore()) {
        pkt->SPM_LDST_resp = true;
    }

    if (pkt->SPMresp || pkt->SPM_LDST_resp) 
    {
        spmThread[cpu->contextToThread(request->contextId())].recvTimingResp(pkt);
    }
    else
    {
        thread[cpu->contextToThread(request->contextId())].recvTimingResp(pkt);
    }

    if (pkt->isInvalidate()) {
        // This response also contains an invalidate; e.g. this can be the case
        // if cmd is ReadRespWithInvalidate.
        //
        // The calling order between completeDataAccess and checkSnoop matters.
        // By calling checkSnoop after completeDataAccess, we ensure that the
        // fault set by checkSnoop is not lost. Calling writeback (more
        // specifically inst->completeAcc) in completeDataAccess overwrites
        // fault, and in case this instruction requires squashing (as
        // determined by checkSnoop), the ReExec fault set by checkSnoop would
        // be lost otherwise.

        DPRINTF(LSQ, "received invalidation with response for addr:%#x\n",
                pkt->getAddr());

        for (ThreadID tid = 0; tid < numThreads; tid++) {
            thread[tid].checkSnoop(pkt);
        }
    }
    // Update the LSQRequest state (this may delete the request)
    request->packetReplied();

    if (waitingForStaleTranslation) {
        checkStaleTranslations();
    }


    //jq86
    // DynInstPtr inst = request->instruction();
    // if (inst->memReqFlags & Request::CP_TO_SPM) {
    //     spmThread[tid].markShadowReady(inst->seqNum);
    // }

    return true;
}

void
LSQ::recvTimingSnoopReq(PacketPtr pkt)
{
    DPRINTF(LSQ, "received pkt for addr:%#x %s\n", pkt->getAddr(),
            pkt->cmdString());

    // must be a snoop
    if (pkt->isInvalidate()) {
        DPRINTF(LSQ, "received invalidation for addr:%#x\n",
                pkt->getAddr());
        for (ThreadID tid = 0; tid < numThreads; tid++) {
            thread[tid].checkSnoop(pkt);
        }
    } else if (pkt->req && pkt->req->isTlbiExtSync()) {
        DPRINTF(LSQ, "received TLBI Ext Sync\n");
        assert(!waitingForStaleTranslation);

        waitingForStaleTranslation = true;
        staleTranslationWaitTxnId = pkt->req->getExtraData();

        for (auto& unit : thread) {
            unit.startStaleTranslationFlush();
        }

        // In case no units have pending ops, just go ahead
        checkStaleTranslations();
    }
}

int
LSQ::getCount()
{
    unsigned total = 0;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        total += getCount(tid);
    }

    return total;
}

int
LSQ::numLoads()
{
    unsigned total = 0;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        total += numLoads(tid);
    }

    return total;
}

int
LSQ::numStores()
{
    unsigned total = 0;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        total += thread[tid].numStores();
    }

    return total;
}

unsigned
LSQ::numFreeLoadEntries()
{
    unsigned total = 0;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        total += thread[tid].numFreeLoadEntries();
    }

    return total;
}

unsigned
LSQ::numFreeStoreEntries()
{
    unsigned total = 0;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        total += thread[tid].numFreeStoreEntries();
    }

    return total;
}

unsigned
LSQ::numFreeLoadEntries(ThreadID tid)
{
        return thread[tid].numFreeLoadEntries();
}

unsigned
LSQ::numFreeStoreEntries(ThreadID tid)
{
        return thread[tid].numFreeStoreEntries();
}

bool
LSQ::isFull()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!(thread[tid].lqFull() || thread[tid].sqFull()))
            return false;
    }

    return true;
}

bool
LSQ::isFull(ThreadID tid)
{
    //@todo: Change to Calculate All Entries for
    //Dynamic Policy
    if (lsqPolicy == SMTQueuePolicy::Dynamic)
        return isFull();
    else
        return thread[tid].lqFull() || thread[tid].sqFull();
}

bool
LSQ::isEmpty() const
{
    return lqEmpty() && sqEmpty();
}

bool
LSQ::lqEmpty() const
{
    std::list<ThreadID>::const_iterator threads = activeThreads->begin();
    std::list<ThreadID>::const_iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!thread[tid].lqEmpty())
            return false;
    }

    return true;
}

bool
LSQ::sqEmpty() const
{
    std::list<ThreadID>::const_iterator threads = activeThreads->begin();
    std::list<ThreadID>::const_iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!thread[tid].sqEmpty())
            return false;
    }

    return true;
}



//jq86

bool
LSQ::spmLqFull()
{
    for (ThreadID tid : *activeThreads) {
        if (!spmThread[tid].lqFull())
            return false;
    }
    return true;
}
bool
LSQ::spmSqFull()
{
    for (ThreadID tid : *activeThreads) {
        if (!spmThread[tid].sqFull())
            return false;
    }
    return true;
}
bool
LSQ::spmLqFull(ThreadID tid)
{
    if (lsqPolicy == SMTQueuePolicy::Dynamic)
        return spmLqFull();
    else
        return spmThread[tid].lqFull();
}

bool
LSQ::spmSqFull(ThreadID tid)
{
    if (lsqPolicy == SMTQueuePolicy::Dynamic)
        return spmSqFull();
    else
        return spmThread[tid].sqFull();
}

bool
LSQ::isSpmFull()
{
    for (ThreadID tid : *activeThreads) {
        if (!(spmThread[tid].lqFull() || spmThread[tid].sqFull()))
            return false;
    }
    return true;
}

bool
LSQ::isSpmFull(ThreadID tid)
{
    if (lsqPolicy == SMTQueuePolicy::Dynamic)
        return isSpmFull();
    else
        return spmThread[tid].lqFull() || spmThread[tid].sqFull();
}







bool
LSQ::lqFull()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!thread[tid].lqFull())
            return false;
    }

    return true;
}

bool
LSQ::lqFull(ThreadID tid)
{
    //@todo: Change to Calculate All Entries for
    //Dynamic Policy
    if (lsqPolicy == SMTQueuePolicy::Dynamic)
        return lqFull();
    else
        return thread[tid].lqFull();
}

bool
LSQ::sqFull()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!sqFull(tid))
            return false;
    }

    return true;
}

bool
LSQ::sqFull(ThreadID tid)
{
     //@todo: Change to Calculate All Entries for
    //Dynamic Policy
    if (lsqPolicy == SMTQueuePolicy::Dynamic)
        return sqFull();
    else
        return thread[tid].sqFull();
}

bool
LSQ::isStalled()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!thread[tid].isStalled())
            return false;
    }

    return true;
}

bool
LSQ::isStalled(ThreadID tid)
{
    if (lsqPolicy == SMTQueuePolicy::Dynamic)
        return isStalled();
    else
        return thread[tid].isStalled();
}

bool
LSQ::hasStoresToWB()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (hasStoresToWB(tid))
            return true;
    }

    return false;
}

bool
LSQ::hasStoresToWB(ThreadID tid)
{
    return cfReduction::busy(cpu->system, cpu->tcBase(tid)->contextId()) ||
           thread.at(tid).hasStoresToWB() ||
           spmThread.at(tid).hasStoresToWB() ||
           spmThread.at(tid).hasPendingWriteback();
}

int
LSQ::numStoresToWB(ThreadID tid)
{
    return thread.at(tid).numStoresToWB();
}

bool
LSQ::willWB()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (willWB(tid))
            return true;
    }

    return false;
}

bool
LSQ::willWB(ThreadID tid)
{
    return thread.at(tid).willWB();
}

void
LSQ::dumpInsts() const
{
    std::list<ThreadID>::const_iterator threads = activeThreads->begin();
    std::list<ThreadID>::const_iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        thread[tid].dumpInsts();
    }
}

void
LSQ::dumpInsts(ThreadID tid) const
{
    thread.at(tid).dumpInsts();
}

Fault
LSQ::pushRequest(const DynInstPtr& inst, bool isLoad, uint8_t *data,
        unsigned int size, Addr addr, Request::Flags flags, uint64_t *res,
        AtomicOpFunctorPtr amo_op, const std::vector<bool>& byte_enable)
{
    // This comming request can be either load, store or atomic.
    // Atomic request has a corresponding pointer to its atomic memory
    // operation
    [[maybe_unused]] bool isAtomic = !isLoad && amo_op;

    ThreadID tid = cpu->contextToThread(inst->contextId());
    auto cacheLineSize = cpu->cacheLineSize();
    bool needs_burst = transferNeedsBurst(addr, size, cacheLineSize);
    LSQRequest* request = nullptr;

    // Atomic requests that access data across cache line boundary are
    // currently not allowed since the cache does not guarantee corresponding
    // atomic memory operations to be executed atomically across a cache line.
    // For ISAs such as x86 that supports cross-cache-line atomic instructions,
    // the cache needs to be modified to perform atomic update to both cache
    // lines. For now, such cross-line update is not supported.
    assert(!isAtomic || (isAtomic && !needs_burst));

    const bool htm_cmd = isLoad && (flags & Request::HTM_CMD);
    const bool tlbi_cmd = isLoad && (flags & Request::TLBI_CMD);
     
    // weihang
    LSQUnit* mux_port = &thread[tid]; 
    if (flags.isSet(Request::SPM_LOAD) ||flags.isSet(Request::SPM_STORE) ) 
    { 
        mux_port = &spmThread[tid];
        needs_burst = false;
    }

    if (inst->translationStarted()) {
        request = inst->savedRequest;
        assert(request);
    } else {

        CFL_INST("ADDRESS", inst, "va=%#x bytes=%u", addr, size);

        // if (htm_cmd || tlbi_cmd) {
        //     assert(addr == 0x0lu);
        //     assert(size == 8);
        //     request = new UnsquashableDirectRequest(&thread[tid], inst, flags);
        // } else if (needs_burst) {
        //     request = new SplitDataRequest(&thread[tid], inst, isLoad, addr,
        //             size, flags, data, res);
        // } else {
        //     request = new SingleDataRequest(&thread[tid], inst, isLoad, addr,
        //             size, flags, data, res, std::move(amo_op));
        // }


        //weihang
        if (htm_cmd || tlbi_cmd) {
            assert(addr == 0x0lu);
            assert(size == 8);
            request = new UnsquashableDirectRequest(mux_port, inst, flags);
        } else if (flags.isSet(Request::SPMCP_BATCH3)) {
            request = new BatchSPMCPRequest(mux_port, inst, addr, flags,
                                            data, res);
        } else if (flags.isSet(Request::SPMDMA_2D)) {
            request = new DmaSPMCPRequest(mux_port, inst, addr, flags,
                                          data, res);
        } else if (needs_burst) {
            request = new SplitDataRequest(mux_port, inst, isLoad, addr,
                    size, flags, data, res);
        } else {
            request = new SingleDataRequest(mux_port, inst, isLoad, addr,
                    size, flags, data, res, std::move(amo_op));
        }

        assert(request);
        request->_byteEnable = flags.isSet(Request::SPMCP_BATCH3) ?
            std::vector<bool>(3 * cacheLineSize, true) : byte_enable;
        inst->setRequest();
        request->taskId(cpu->taskId());

        // There might be fault from a previous execution attempt if this is
        // a strictly ordered load
        inst->getFault() = NoFault;

        request->initiateTranslation();
    }

    /* This is the place were instructions get the effAddr. */
    if (request->isTranslationComplete()) {
        if (request->isMemAccessRequired()) {
            inst->effAddr = request->getVaddr();
            inst->effSize = size;
            inst->effAddrValid(true);

            if (cpu->checker) {
                inst->reqToVerify = std::make_shared<Request>(*request->req());
            }
            Fault fault;
            if (isLoad)
                fault = read(request, inst->lqIdx);
            else
                fault = write(request, data, inst->sqIdx);
            // inst->getFault() may have the first-fault of a
            // multi-access split request at this point.
            // Overwrite that only if we got another type of fault
            // (e.g. re-exec).
            if (fault != NoFault)
                inst->getFault() = fault;
        } else if (isLoad) {
            inst->setMemAccPredicate(false);
            // Commit will have to clean up whatever happened.  Set this
            // instruction as executed.
            inst->setExecuted();
        }
    }

    if (inst->traceData)
        inst->traceData->setMem(addr, size, flags);

    return inst->getFault();
}

void
LSQ::SingleDataRequest::finish(const Fault &fault, const RequestPtr &request,
        gem5::ThreadContext* tc, BaseMMU::Mode mode)
{
    _fault.push_back(fault);
    numInTranslationFragments = 0;
    numTranslatedFragments = 1;
    /* If the instruction has been squahsed, let the request know
     * as it may have to self-destruct. */
    if (_inst->isSquashed()) {
        squashTranslation();
    } else {
        _inst->strictlyOrdered(request->isStrictlyOrdered());

        flags.set(Flag::TranslationFinished);
        if (fault == NoFault) {
            _inst->physEffAddr = request->getPaddr();
            _inst->memReqFlags = request->getFlags();
            if (request->isCondSwap()) {
                assert(_res);
                request->setExtraData(*_res);
            }
            setState(State::Request);
        } else {
            setState(State::Fault);
        }

        LSQRequest::_inst->fault = fault;
        LSQRequest::_inst->translationCompleted(true);
    }
}

void
LSQ::SplitDataRequest::finish(const Fault &fault, const RequestPtr &req,
        gem5::ThreadContext* tc, BaseMMU::Mode mode)
{
    int i;
    for (i = 0; i < _reqs.size() && _reqs[i] != req; i++);
    assert(i < _reqs.size());
    _fault[i] = fault;

    numInTranslationFragments--;
    numTranslatedFragments++;

    if (fault == NoFault)
        _mainReq->setFlags(req->getFlags());

    if (numTranslatedFragments == _reqs.size()) {
        if (_inst->isSquashed()) {
            squashTranslation();
        } else {
            _inst->strictlyOrdered(_mainReq->isStrictlyOrdered());
            flags.set(Flag::TranslationFinished);
            _inst->translationCompleted(true);

            for (i = 0; i < _fault.size() && _fault[i] == NoFault; i++);
            if (i > 0) {
                _inst->physEffAddr = LSQRequest::req()->getPaddr();
                _inst->memReqFlags = _mainReq->getFlags();
                if (_mainReq->isCondSwap()) {
                    assert (i == _fault.size());
                    assert(_res);
                    _mainReq->setExtraData(*_res);
                }
                if (i == _fault.size()) {
                    _inst->fault = NoFault;
                    setState(State::Request);
                } else {
                  _inst->fault = _fault[i];
                  setState(State::PartialFault);
                }
            } else {
                _inst->fault = _fault[0];
                setState(State::Fault);
            }
        }

    }
}

void
LSQ::SingleDataRequest::initiateTranslation()
{
    assert(_reqs.size() == 0);

    addReq(_addr, _size, _byteEnable);
    //jq86
    if (_reqs.size() > 0 && ( _flags.isSet(Request::SPM_LOAD) || _flags.isSet(Request::SPM_STORE))) {

        // _inst->physEffAddr = _addr;
        // _inst->memReqFlags = _reqs.back()->getFlags();
        // _inst->strictlyOrdered(_reqs.back()->isStrictlyOrdered());  

        _reqs.back()->setReqInstSeqNum(_inst->seqNum);
        _reqs.back()->taskId(_taskId);
        _reqs.back()->setPaddr(_addr);
        _reqs.back()->setInstCount(_inst->getCpuPtr()->totalInsts());

        _inst->strictlyOrdered(_reqs.back()->isStrictlyOrdered());
        _inst->fault = NoFault;
        _inst->physEffAddr = _reqs.back()->getPaddr();
        _inst->memReqFlags = _reqs.back()->getFlags();
        _inst->savedRequest = this;


        flags.set(Flag::TranslationStarted);
        flags.set(Flag::TranslationFinished);
        _inst->translationStarted(true);
        _inst->translationCompleted(true);
        // numInTranslationFragments = 0;
        // numTranslatedFragments = 1;
        setState(State::Request);
        return;
    }
    if (_reqs.size() > 0) {
        _reqs.back()->setReqInstSeqNum(_inst->seqNum);
        _reqs.back()->taskId(_taskId);
        _inst->translationStarted(true);
        setState(State::Translation);
        flags.set(Flag::TranslationStarted);

        _inst->savedRequest = this;
        sendFragmentToTranslation(0);
    } else {
        _inst->setMemAccPredicate(false);
    }
}

PacketPtr
LSQ::SplitDataRequest::mainPacket()
{
    return _mainPacket;
}

RequestPtr
LSQ::SplitDataRequest::mainReq()
{
    return _mainReq;
}

void
LSQ::SplitDataRequest::initiateTranslation()
{
    auto cacheLineSize = _port.cacheLineSize();
    Addr base_addr = _addr;
    Addr next_addr = addrBlockAlign(_addr + cacheLineSize, cacheLineSize);
    Addr final_addr = addrBlockAlign(_addr + _size, cacheLineSize);
    uint32_t size_so_far = 0;

    _mainReq = std::make_shared<Request>(base_addr,
                _size, _flags, _inst->requestorId(),
                _inst->pcState().instAddr(), _inst->contextId());
    _mainReq->setByteEnable(_byteEnable);

    // Paddr is not used in _mainReq. However, we will accumulate the flags
    // from the sub requests into _mainReq by calling setFlags() in finish().
    // setFlags() assumes that paddr is set so flip the paddr valid bit here to
    // avoid a potential assert in setFlags() when we call it from  finish().
    _mainReq->setPaddr(0);

    /* Get the pre-fix, possibly unaligned. */
    auto it_start = _byteEnable.begin();
    auto it_end = _byteEnable.begin() + (next_addr - base_addr);
    addReq(base_addr, next_addr - base_addr,
                     std::vector<bool>(it_start, it_end));
    size_so_far = next_addr - base_addr;

    /* We are block aligned now, reading whole blocks. */
    base_addr = next_addr;
    while (base_addr != final_addr) {
        auto it_start = _byteEnable.begin() + size_so_far;
        auto it_end = _byteEnable.begin() + size_so_far + cacheLineSize;
        addReq(base_addr, cacheLineSize,
                         std::vector<bool>(it_start, it_end));
        size_so_far += cacheLineSize;
        base_addr += cacheLineSize;
    }

    /* Deal with the tail. */
    if (size_so_far < _size) {
        auto it_start = _byteEnable.begin() + size_so_far;
        auto it_end = _byteEnable.end();
        addReq(base_addr, _size - size_so_far,
                         std::vector<bool>(it_start, it_end));
    }

    if (_reqs.size() > 0) {
        /* Setup the requests and send them to translation. */
        for (auto& r: _reqs) {
            r->setReqInstSeqNum(_inst->seqNum);
            r->taskId(_taskId);
        }

        // SPM loads/stores in SplitDataRequest: skip TLB, use VA as PA
        if (_flags.isSet(Request::SPM_LOAD) ||
            _flags.isSet(Request::SPM_STORE)) {
            for (auto& r : _reqs) {
                r->setPaddr(r->getVaddr());
                r->setInstCount(_inst->getCpuPtr()->totalInsts());
            }
            _inst->strictlyOrdered(_reqs.back()->isStrictlyOrdered());
            _inst->fault = NoFault;
            _inst->physEffAddr = _reqs.front()->getPaddr();
            _inst->memReqFlags = _reqs.front()->getFlags();
            _inst->savedRequest = this;

            flags.set(Flag::TranslationStarted);
            flags.set(Flag::TranslationFinished);
            _inst->translationStarted(true);
            _inst->translationCompleted(true);
            setState(State::Request);
        } else {
            _inst->translationStarted(true);
            setState(State::Translation);
            flags.set(Flag::TranslationStarted);
            _inst->savedRequest = this;
            numInTranslationFragments = 0;
            numTranslatedFragments = 0;
            _fault.resize(_reqs.size());

            for (uint32_t i = 0; i < _reqs.size(); i++) {
                sendFragmentToTranslation(i);
            }
        }
    } else {
        _inst->setMemAccPredicate(false);
    }
}

LSQ::LSQRequest::LSQRequest(
        LSQUnit *port, const DynInstPtr& inst, bool isLoad) :
    _state(State::NotIssued),
    _port(*port), _inst(inst), _data(nullptr),
    _res(nullptr), _addr(0), _size(0), _flags(0),
    _numOutstandingPackets(0), _amo_op(nullptr)
{
    flags.set(Flag::IsLoad, isLoad);
    flags.set(Flag::WriteBackToRegister,
              _inst->isStoreConditional() || _inst->isAtomic() ||
              _inst->isLoad());
    flags.set(Flag::IsAtomic, _inst->isAtomic());
    install();
}

LSQ::LSQRequest::LSQRequest(
        LSQUnit *port, const DynInstPtr& inst, bool isLoad,
        const Addr& addr, const uint32_t& size, const Request::Flags& flags_,
        PacketDataPtr data, uint64_t* res, AtomicOpFunctorPtr amo_op,
        bool stale_translation)
    : _state(State::NotIssued),
    numTranslatedFragments(0),
    numInTranslationFragments(0),
    _port(*port), _inst(inst), _data(data),
    _res(res), _addr(addr), _size(size),
    _flags(flags_),
    _numOutstandingPackets(0),
    _amo_op(std::move(amo_op)),
    _hasStaleTranslation(stale_translation)
{
    flags.set(Flag::IsLoad, isLoad);
    flags.set(Flag::WriteBackToRegister,
              _inst->isStoreConditional() || _inst->isAtomic() ||
              _inst->isLoad());
    flags.set(Flag::IsAtomic, _inst->isAtomic());
    install();
}

void
LSQ::LSQRequest::install()
{
    if (isLoad()) {
        _port.loadQueue[_inst->lqIdx].setRequest(this);
    } else {
        // Store, StoreConditional, and Atomic requests are pushed
        // to this storeQueue
        _port.storeQueue[_inst->sqIdx].setRequest(this);
    }
}

bool LSQ::LSQRequest::squashed() const { return _inst->isSquashed(); }

void
LSQ::LSQRequest::addReq(Addr addr, unsigned size,
           const std::vector<bool>& byte_enable)
{
    if (isAnyActiveElement(byte_enable.begin(), byte_enable.end())) {
        auto req = std::make_shared<Request>(
                addr, size, _flags, _inst->requestorId(),
                _inst->pcState().instAddr(), _inst->contextId(),
                std::move(_amo_op));
        req->setByteEnable(byte_enable);

        /* If the request is marked as NO_ACCESS, setup a local access */
        if (_flags.isSet(Request::NO_ACCESS)) {
            req->setLocalAccessor(
                [this, req](gem5::ThreadContext *tc, PacketPtr pkt) -> Cycles
                {
                    if ((req->isHTMStart() || req->isHTMCommit())) {
                        auto& inst = this->instruction();
                        assert(inst->inHtmTransactionalState());
                        pkt->setHtmTransactional(
                            inst->getHtmTransactionUid());
                    }
                    return Cycles(1);
                }
            );
        }

        _reqs.push_back(req);
    }
}

LSQ::LSQRequest::~LSQRequest()
{
    assert(!isAnyOutstandingRequest());
    _inst->savedRequest = nullptr;

    for (auto r: _packets)
        delete r;
};

ContextID
LSQ::LSQRequest::contextId() const
{
    return _inst->contextId();
}

void
LSQ::LSQRequest::sendFragmentToTranslation(int i)
{
    numInTranslationFragments++;
    _port.getMMUPtr()->translateTiming(req(i), _inst->thread->getTC(),
            this, isLoad() ? BaseMMU::Read : BaseMMU::Write);
}

void
LSQ::SingleDataRequest::markAsStaleTranslation()
{
    // If this element has been translated and is currently being requested,
    // then it may be stale
    if ((!flags.isSet(Flag::Complete)) &&
        (!flags.isSet(Flag::Discarded)) &&
        (flags.isSet(Flag::TranslationStarted))) {
        _hasStaleTranslation = true;
    }

    DPRINTF(LSQ, "SingleDataRequest %d 0x%08x isBlocking:%d\n",
        (int)_state, (uint32_t)flags, _hasStaleTranslation);
}

void
LSQ::SplitDataRequest::markAsStaleTranslation()
{
    // If this element has been translated and is currently being requested,
    // then it may be stale
    if ((!flags.isSet(Flag::Complete)) &&
        (!flags.isSet(Flag::Discarded)) &&
        (flags.isSet(Flag::TranslationStarted))) {
        _hasStaleTranslation = true;
    }

    DPRINTF(LSQ, "SplitDataRequest %d 0x%08x isBlocking:%d\n",
        (int)_state, (uint32_t)flags, _hasStaleTranslation);
}

bool
LSQ::SingleDataRequest::recvTimingResp(PacketPtr pkt)
{
    assert(_numOutstandingPackets == 1);
    flags.set(Flag::Complete);
    assert(pkt == _packets.front());
    _port.completeDataAccess(pkt);
    _hasStaleTranslation = false;
    return true;
}

bool
LSQ::SplitDataRequest::recvTimingResp(PacketPtr pkt)
{
    uint32_t pktIdx = 0;
    while (pktIdx < _packets.size() && pkt != _packets[pktIdx])
        pktIdx++;
    assert(pktIdx < _packets.size());
    numReceivedPackets++;
    if (numReceivedPackets == _packets.size()) {
        flags.set(Flag::Complete);
        /* Assemble packets. */
        PacketPtr resp = isLoad()
            ? Packet::createRead(_mainReq)
            : Packet::createWrite(_mainReq);
        if (isLoad())
            resp->dataStatic(_inst->memData);
        else
            resp->dataStatic(_data);
        resp->senderState = this;
        _port.completeDataAccess(resp);
        delete resp;
    }
    _hasStaleTranslation = false;
    return true;
}

void
LSQ::SingleDataRequest::buildPackets()
{
    /* Retries do not create new packets. */
    if (_packets.size() == 0) {
        _packets.push_back(
                isLoad()
                    ?  Packet::createRead(req())
                    :  Packet::createWrite(req()));
        _packets.back()->dataStatic(_inst->memData);
        _packets.back()->senderState = this;

        // hardware transactional memory
        // If request originates in a transaction (not necessarily a HtmCmd),
        // then the packet should be marked as such.
        if (_inst->inHtmTransactionalState()) {
            _packets.back()->setHtmTransactional(
                _inst->getHtmTransactionUid());

            DPRINTF(HtmCpu,
              "HTM %s pc=0x%lx - vaddr=0x%lx - paddr=0x%lx - htmUid=%u\n",
              isLoad() ? "LD" : "ST",
              _inst->pcState().instAddr(),
              _packets.back()->req->hasVaddr() ?
                  _packets.back()->req->getVaddr() : 0lu,
              _packets.back()->getAddr(),
              _inst->getHtmTransactionUid());
        }
    }
    assert(_packets.size() == 1);
}

void
LSQ::SplitDataRequest::buildPackets()
{
    /* Extra data?? */
    Addr base_address = _addr;

    if (_packets.size() == 0) {
        /* New stuff */
        if (isLoad()) {
            _mainPacket = Packet::createRead(_mainReq);
            _mainPacket->dataStatic(_inst->memData);

            // hardware transactional memory
            // If request originates in a transaction,
            // packet should be marked as such
            if (_inst->inHtmTransactionalState()) {
                _mainPacket->setHtmTransactional(
                    _inst->getHtmTransactionUid());
                DPRINTF(HtmCpu,
                  "HTM LD.0 pc=0x%lx-vaddr=0x%lx-paddr=0x%lx-htmUid=%u\n",
                  _inst->pcState().instAddr(),
                  _mainPacket->req->hasVaddr() ?
                      _mainPacket->req->getVaddr() : 0lu,
                  _mainPacket->getAddr(),
                  _inst->getHtmTransactionUid());
            }
        }
        for (int i = 0; i < _reqs.size() && _fault[i] == NoFault; i++) {
            RequestPtr req = _reqs[i];
            PacketPtr pkt = isLoad() ? Packet::createRead(req)
                                     : Packet::createWrite(req);
            ptrdiff_t offset = req->getVaddr() - base_address;
            if (isLoad()) {
                pkt->dataStatic(_inst->memData + offset);
            } else {
                uint8_t* req_data = new uint8_t[req->getSize()];
                std::memcpy(req_data,
                        _inst->memData + offset,
                        req->getSize());
                pkt->dataDynamic(req_data);
            }
            pkt->senderState = this;
            _packets.push_back(pkt);

            // hardware transactional memory
            // If request originates in a transaction,
            // packet should be marked as such
            if (_inst->inHtmTransactionalState()) {
                _packets.back()->setHtmTransactional(
                    _inst->getHtmTransactionUid());
                DPRINTF(HtmCpu,
                  "HTM %s.%d pc=0x%lx-vaddr=0x%lx-paddr=0x%lx-htmUid=%u\n",
                  isLoad() ? "LD" : "ST",
                  i+1,
                  _inst->pcState().instAddr(),
                  _packets.back()->req->hasVaddr() ?
                      _packets.back()->req->getVaddr() : 0lu,
                  _packets.back()->getAddr(),
                  _inst->getHtmTransactionUid());
            }
        }
    }
    assert(_packets.size() > 0);
}

void
LSQ::BatchSPMCPRequest::configureDestinations(Addr encoded_dst)
{
    assert(_reqs.size() == 3);
    // Preserve multicast/claim bits while advancing only the physical way.
    const Addr control = encoded_dst & ~Addr(0x7ffff);
    const Addr slot = encoded_dst & Addr(0x7ffff);
    const Addr base_way = (slot >> 16) & 0x7;
    panic_if(base_way + 2 >= 8,
             "SPMCPB3 destination way range exceeds private SPM ways: %#x",
             encoded_dst);

    for (unsigned i = 0; i < 3; ++i) {
        const Addr dst = control | (slot & ~Addr(0x70000)) |
                         ((base_way + i) << 16);
        _reqs[i]->setSPMMetadata(_reqs[i]->getPaddr(), dst);
    }
    _mainReq->setPaddr(_reqs.front()->getPaddr());
    _mainReq->setSPMMetadata(_reqs.front()->getPaddr(), encoded_dst);
}

void
LSQ::BatchSPMCPRequest::buildPackets()
{
    if (_packets.empty()) {
        assert(_reqs.size() == 3);
        for (const auto &req : _reqs) {
            PacketPtr pkt = Packet::createWrite(req);
            auto *dst = new uint8_t[sizeof(Addr)];
            const Addr encoded = req->getSPMDstAddr() |
                (Addr(req->getSPMMulticastMask()) << 24) |
                (req->isSPMMulticastClaim() ? (Addr(1) << 23) : 0);
            std::memcpy(dst, &encoded, sizeof(encoded));
            pkt->dataDynamic(dst);
            pkt->senderState = this;
            _packets.push_back(pkt);
        }
    }
    assert(_packets.size() == 3);
}

void
LSQ::DmaSPMCPRequest::configure(Addr desc)
{
    if (configured)
        return;
    const unsigned dst_set = desc & 0x3ff;
    const unsigned dst_way = (desc >> 10) & 0x7;
    const unsigned rows = ((desc >> 13) & 0x3ff) + 1;
    const unsigned lines = ((desc >> 23) & 0x7) + 1;
    const unsigned src_stride = (desc >> 26) & 0xffff;
    const uint8_t mask = (desc >> 42) & 0xff;
    panic_if(dst_way + lines > 8, "SPMDMA destination ways overflow");
    panic_if(dst_set + rows > 1024, "SPMDMA destination sets overflow");
    panic_if(src_stride < lines * 64, "SPMDMA source stride too small");

    const RequestPtr base = req();
    const Addr src_base = base->getPaddr();
    const Addr src_vbase = base->getVaddr();
    Process *process = _inst->tcBase()->getProcessPtr();
    _packets.reserve(rows * lines);
    for (unsigned row = 0; row < rows; ++row) {
        for (unsigned line = 0; line < lines; ++line) {
            RequestPtr fragment = std::make_shared<Request>(*base);
            const Addr src_offset = Addr(row) * src_stride + line * 64;
            Addr src = src_base + src_offset;
            // A 2-D transfer commonly crosses guest pages. The original
            // command translates only its base operand, so SE mode must map
            // every generated line through the process page table rather
            // than assuming physically contiguous guest pages.
            if (process) {
                const bool translated =
                    process->pTable->translate(src_vbase + src_offset, src);
                panic_if(!translated,
                    "SPMDMA source address %#x is unmapped",
                    src_vbase + src_offset);
            }
            const Addr dst = (Addr(dst_way + line) << 16) |
                             (Addr(dst_set + row) << 6) |
                             (Addr(mask) << 24);
            fragment->setPaddr(src);
            fragment->setSPMMetadata(src, dst);
            PacketPtr pkt = Packet::createWrite(fragment);
            auto *payload = new uint8_t[sizeof(Addr)];
            std::memcpy(payload, &dst, sizeof(dst));
            pkt->dataDynamic(payload);
            pkt->senderState = this;
            _packets.push_back(pkt);
        }
    }
    const Addr first_dst = (Addr(dst_way) << 16) | (Addr(dst_set) << 6) |
                           (Addr(mask) << 24);
    base->setSPMMetadata(src_base, first_dst);
    configured = true;
}

void
LSQ::DmaSPMCPRequest::buildPackets()
{
    panic_if(!configured || _packets.empty(),
             "SPMDMA packet build before descriptor configuration");
}

void
LSQ::DmaSPMCPRequest::sendPacketToCache()
{
    // The configured SPM store-port count is the modeled DMA injection
    // bandwidth. Keep feeding independent lines until that bandwidth or
    // Ruby backpressure is reached; the paper-matched default remains one.
    unsigned issued = 0;
    while (nextToIssue < _packets.size() && issued < 8 &&
           lsqUnit()->trySendPacket(false, _packets[nextToIssue])) {
        ++_numOutstandingPackets;
        ++nextToIssue;
        ++issued;
    }
}

void
LSQ::SingleDataRequest::sendPacketToCache()
{
    assert(_numOutstandingPackets == 0);
    if (lsqUnit()->trySendPacket(isLoad(), _packets.at(0)))
        _numOutstandingPackets = 1;
}

void
LSQ::SplitDataRequest::sendPacketToCache()
{
    /* Try to send the packets. */
    while (numReceivedPackets + _numOutstandingPackets < _packets.size() &&
            lsqUnit()->trySendPacket(isLoad(),
                _packets.at(numReceivedPackets + _numOutstandingPackets))) {
        _numOutstandingPackets++;
    }
}

Cycles
LSQ::SingleDataRequest::handleLocalAccess(
        gem5::ThreadContext *thread, PacketPtr pkt)
{
    return pkt->req->localAccessor(thread, pkt);
}

Cycles
LSQ::SplitDataRequest::handleLocalAccess(
        gem5::ThreadContext *thread, PacketPtr mainPkt)
{
    Cycles delay(0);
    unsigned offset = 0;

    for (auto r: _reqs) {
        PacketPtr pkt =
            new Packet(r, isLoad() ? MemCmd::ReadReq : MemCmd::WriteReq);
        pkt->dataStatic(mainPkt->getPtr<uint8_t>() + offset);
        Cycles d = r->localAccessor(thread, pkt);
        if (d > delay)
            delay = d;
        offset += r->getSize();
        delete pkt;
    }
    return delay;
}

bool
LSQ::SingleDataRequest::isCacheBlockHit(Addr blockAddr, Addr blockMask)
{
    return ( (LSQRequest::_reqs[0]->getPaddr() & blockMask) == blockAddr);
}

/**
 * Caches may probe into the load-store queue to enforce memory ordering
 * guarantees. This method supports probes by providing a mechanism to compare
 * snoop messages with requests tracked by the load-store queue.
 *
 * Consistency models must enforce ordering constraints. TSO, for instance,
 * must prevent memory reorderings except stores which are reordered after
 * loads. The reordering restrictions negatively impact performance by
 * cutting down on memory level parallelism. However, the core can regain
 * performance by generating speculative loads. Speculative loads may issue
 * without affecting correctness if precautions are taken to handle invalid
 * memory orders. The load queue must squash under memory model violations.
 * Memory model violations may occur when block ownership is granted to
 * another core or the block cannot be accurately monitored by the load queue.
 */
bool
LSQ::SplitDataRequest::isCacheBlockHit(Addr blockAddr, Addr blockMask)
{
    bool is_hit = false;
    for (auto &r: _reqs) {
       /**
        * The load-store queue handles partial faults which complicates this
        * method. Physical addresses must be compared between requests and
        * snoops. Some requests will not have a valid physical address, since
        * partial faults may have outstanding translations. Therefore, the
        * existence of a valid request address must be checked before
        * comparing block hits. We assume no pipeline squash is needed if a
        * valid request address does not exist.
        */
        if (r->hasPaddr() && (r->getPaddr() & blockMask) == blockAddr) {
            is_hit = true;
            break;
        }
    }
    return is_hit;
}


// weihang
bool LSQ::SpmPort::recvTimingResp(PacketPtr pkt) 
{
    if (pkt->req->isSPMCPFetch())
        pkt->SPMresp = true;
    else if (pkt->req->isSPMLD() || pkt->req->isSPMST() ||
             pkt->req->isSPMWBRead() || pkt->req->isSPMWBStore())
        pkt->SPM_LDST_resp = true;
    return lsq->recvTimingResp(pkt); 
}
void LSQ::SpmPort::recvReqRetry()                
{ 
    if (lsq->independentBridge())
        lsq->recvSpmRetry();
    else
        lsq->recvReqRetry();
}

void
LSQ::BridgePort::recvReqRetry()
{
    if (lsq->independentBridge())
        lsq->recvBridgeRetry();
    else
        lsq->recvReqRetry();
}

void
LSQ::recvBridgeRetry()
{
    bridgeBlocked(false);
    // IQ retry lists remain shared; actual sends still check their own port.
    iewStage->cacheUnblocked();
    for (ThreadID tid : *activeThreads)
        thread[tid].recvRetry();
}

void
LSQ::recvSpmRetry()
{
    spmBlocked(false);
    iewStage->cacheUnblocked();
    for (ThreadID tid : *activeThreads)
        spmThread[tid].recvRetry();
}

void
LSQ::ReductionPort::recvReqRetry()
{
    lsq->recvReductionRetry();
}

void
LSQ::recvReductionRetry()
{
    // This notification releases capture admission only. It must not clear
    // a coherent/SPM/bridge rejection on another interface.
    reductionBlocked(false);
    iewStage->cacheUnblocked();
    for (ThreadID tid : *activeThreads)
        spmThread[tid].recvRetry();
}

// void LSQ::SpmPort::recvTimingSnoopReq(PacketPtr pkt) 
// { 
//     lsq->recvTimingSnoopReq(pkt); 
// }

bool
LSQ::DcachePort::recvTimingResp(PacketPtr pkt)
{
    return lsq->recvTimingResp(pkt);
}

void
LSQ::DcachePort::recvTimingSnoopReq(PacketPtr pkt)
{
    for (ThreadID tid = 0; tid < cpu->numThreads; tid++) {
        if (cpu->getCpuAddrMonitor(tid)->doMonitor(pkt)) {
            cpu->wakeup(tid);
        }
    }
    lsq->recvTimingSnoopReq(pkt);
}

void
LSQ::DcachePort::recvReqRetry()
{
    lsq->recvReqRetry();
}

LSQ::UnsquashableDirectRequest::UnsquashableDirectRequest(
    LSQUnit* port,
    const DynInstPtr& inst,
    const Request::Flags& flags_) :
    SingleDataRequest(port, inst, true, 0x0lu, 8, flags_,
        nullptr, nullptr, nullptr)
{
}

void
LSQ::UnsquashableDirectRequest::initiateTranslation()
{
    // Special commands are implemented as loads to avoid significant
    // changes to the cpu and memory interfaces
    // The virtual and physical address uses a dummy value of 0x00
    // Address translation does not really occur thus the code below

    assert(_reqs.size() == 0);

    addReq(_addr, _size, _byteEnable);

    if (_reqs.size() > 0) {
        _reqs.back()->setReqInstSeqNum(_inst->seqNum);
        _reqs.back()->taskId(_taskId);
        _reqs.back()->setPaddr(_addr);
        _reqs.back()->setInstCount(_inst->getCpuPtr()->totalInsts());

        _inst->strictlyOrdered(_reqs.back()->isStrictlyOrdered());
        _inst->fault = NoFault;
        _inst->physEffAddr = _reqs.back()->getPaddr();
        _inst->memReqFlags = _reqs.back()->getFlags();
        _inst->savedRequest = this;

        flags.set(Flag::TranslationStarted);
        flags.set(Flag::TranslationFinished);

        _inst->translationStarted(true);
        _inst->translationCompleted(true);

        setState(State::Request);
    } else {
        panic("unexpected behaviour in initiateTranslation()");
    }
}

void
LSQ::UnsquashableDirectRequest::markAsStaleTranslation()
{
    // HTM/TLBI operations do not translate,
    // so cannot have stale translations
    _hasStaleTranslation = false;
}

void
LSQ::UnsquashableDirectRequest::finish(const Fault &fault,
        const RequestPtr &req, gem5::ThreadContext* tc,
        BaseMMU::Mode mode)
{
    panic("unexpected behaviour - finish()");
}

void
LSQ::checkStaleTranslations()
{
    assert(waitingForStaleTranslation);

    DPRINTF(LSQ, "Checking pending TLBI sync\n");
    // Check if all thread queues are complete
    for (const auto& unit : thread) {
        if (unit.checkStaleTranslations())
            return;
    }
    DPRINTF(LSQ, "No threads have blocking TLBI sync\n");

    // All thread queues have committed their sync operations
    // => send a RubyRequest to the sequencer
    auto req = Request::createMemManagement(
        Request::TLBI_EXT_SYNC_COMP,
        cpu->dataRequestorId());
    req->setExtraData(staleTranslationWaitTxnId);
    PacketPtr pkt = Packet::createRead(req);

    // TODO - reserve some credit for these responses?
    if (!dcachePort.sendTimingReq(pkt)) {
        panic("Couldn't send TLBI_EXT_SYNC_COMP message");
    }

    waitingForStaleTranslation = false;
    staleTranslationWaitTxnId = 0;
}

Fault
LSQ::read(LSQRequest* request, ssize_t load_idx)
{
    assert(request->req()->contextId() == request->contextId());
    ThreadID tid = cpu->contextToThread(request->req()->contextId());
    //jq86
    DynInstPtr inst = request->instruction();
    if ((inst->memReqFlags & Request::SPM_LOAD))
    {
        return spmThread.at(tid).read(request, load_idx);
    }
    else
    {
        return thread.at(tid).read(request, load_idx);
    }



    //weihang
    // auto& u = isSpmFlags(request->req()->getFlags()) ? spmThread.at(tid)
    // : thread.at(tid);

    // return u.read(request, load_idx);

}

Fault
LSQ::write(LSQRequest* request, uint8_t *data, ssize_t store_idx)
{
    ThreadID tid = cpu->contextToThread(request->req()->contextId());

    //weihang
    // auto& u = isSpmFlags(request->req()->getFlags()) ? spmThread.at(tid)
    // : thread.at(tid);

    // return u.write(request, data, store_idx);
    //jq86
    DynInstPtr inst = request->instruction();

    if ((inst->memReqFlags & Request::CP_TO_SPM) ||
        (inst->memReqFlags & Request::SPM_WRITEBACK))
    {
        Addr spmAddr = 0;
        memcpy(&spmAddr, data, inst->effSize);
        inst->spmAddr = spmAddr;

        // Preserve the encoded SPM destination separately from the coherent
        // source address. Translation updates the Request paddr to the source;
        // without this metadata Ruby would install the SPM line at that source
        // address and alias ordinary coherent accesses.
        RequestPtr main_req = request->mainReq();
        assert(main_req);
        main_req->setSPMMetadata(main_req->getPaddr(), spmAddr);
        if (auto *batch = dynamic_cast<BatchSPMCPRequest *>(request))
            batch->configureDestinations(spmAddr);
        if (auto *dma = dynamic_cast<DmaSPMCPRequest *>(request)) {
            dma->configure(spmAddr);
        }
        // Keep the shadow-table address in the canonical local-slot domain;
        // bits 31:24 carry only the multicast core mask.
        inst->spmAddr = main_req->getSPMDstAddr();

        spmThread[tid].reserveSpmStore(inst, true, main_req->getPaddr());
        DPRINTF(LSQ, "write(): captured spmAddr=%#x from data ptr=%p\n",
                spmAddr, data);
    }

    if ((inst->memReqFlags & Request::SPM_STORE))
    {
        return spmThread.at(tid).write(request, data, store_idx);
    }
    else
    {
        return thread.at(tid).write(request, data, store_idx);
    }


    // return thread.at(tid).write(request, data, store_idx);
}

} // namespace o3
} // namespace gem5
