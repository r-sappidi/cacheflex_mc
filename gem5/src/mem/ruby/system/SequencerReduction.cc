// Experimental posted FP32 scale-and-add into existing SPM slots.
// SRAM accesses deliberately use the existing timed SPM controller path.
// This conservative backend retains local-hop/response overhead; it is NOT
// a zero-latency functional oracle or a calibrated bank-level implementation.
#include "mem/ruby/system/Sequencer.hh"
#include "mem/cacheflex_reduction.hh"
#include "mem/ruby/slicc_interface/AbstractController.hh"
#include "mem/ruby/slicc_interface/AbstractCacheEntry.hh"
#include "softfloat.h"
#include "base/trace.hh"
#include "debug/CacheFlexReductionTrace.hh"
#include <cstring>

namespace gem5 { namespace ruby {
namespace {
bool sharedSlot(Addr a) {
    return (a & (Addr(1)<<59)) || ((a & 0xf8000000ULL) == 0x68000000ULL);
}
Addr slotAddress(Addr a) {
    return sharedSlot(a) ? (0x68000000ULL | (a & 0x77ffff)) :
                          (0x60000000ULL | (a & cfReduction::SlotMask));
}
bool overlap(Addr a, unsigned bytes, Addr b, unsigned otherBytes) {
    a=slotAddress(a); b=slotAddress(b);
    if ((a & 0x08700000ULL) != (b & 0x08700000ULL)) return false;
    if (((a >> 6) & 1023) != ((b >> 6) & 1023)) return false;
    unsigned wa = (a >> 16) & 7, wb = (b >> 16) & 7;
    unsigned na = ((a & 63) + bytes + 63) / 64;
    unsigned nb = ((b & 63) + otherBytes + 63) / 64;
    return wa < wb + nb && wb < wa + na;
}
unsigned touchedBytes(PacketPtr p) {
    if (p->req->isSPMWBStore() &&
        (p->req->getSPMDstAddr() & cfReduction::Finalize)) return 128;
    if (p->req->isSPMST() && (p->req->getSPMSrcAddr() & cfReduction::Command))
        return 256; // A compact contribution still updates four FP32 slots.
    return (p->req->isSPMLD() || p->req->isSPMST()) ? p->getSize() : 64;
}
}

bool Sequencer::reductionConflicts(PacketPtr p) const {
    for (const auto &entry : reductionPending)
        if (overlap(p->req->getSPMDstAddr(), touchedBytes(p), entry.first, 64))
            return true;
    return false;
}

bool Sequencer::reductionOlderAccess(Addr slot) const {
    for (const auto &line : m_RequestTable)
        for (const auto &r : line.second)
            if (r.admissionActive && r.pkt != reductionMemoryPacket && !l3ReductionFlight.count(r.pkt) &&
                r.pkt->req->isSPMRequest() &&
                overlap(slot, 256, r.pkt->req->getSPMDstAddr(), touchedBytes(r.pkt)))
                return true;
    return false;
}

void Sequencer::scheduleReduction() {
    if (!reductionEvent.scheduled())
        schedule(reductionEvent, clockEdge(Cycles(1)));
}

RequestStatus Sequencer::acceptReduction(PacketPtr p) {
    panic_if(p->getSize() != 256 || (p->getAddr() & 63) ||
             ((p->getAddr() >> 16) & 7) > 4 || p->isMaskedWrite() ||
             !p->req->hasContextId(),
             "Reduction ABI requires an unmasked VL16 FP32 row, aligned and within four ways");
    const Addr slot = slotAddress(p->getAddr());
    panic_if(sharedSlot(p->getAddr()) != l3ReductionEnabled,
             "Reduction target domain does not match enabled backend");
    if (reductionSource.size() >= reductionSourceCap || reductionOlderAccess(slot)) {
        if (reductionSource.size() >= reductionSourceCap) ++reductionSourceFull;
        else ++reductionOrderingRetries;
        m_ingress_retry = true;
        scheduleIngress();
        return RequestStatus_BufferFull;
    }
    // No allocation oracle: all four destination slots must already have
    // completed their ordinary SPMCP claim/eviction transactions.
    for (unsigned i = 0; !l3ReductionEnabled && i < 4; ++i) {
        auto *e = reductionCache->lookup(slot + (Addr(i) << 16));
        panic_if(!e || !e->isScratchpad(), "Reduction destination not claimed: %#x", slot);
    }
    ReductionCommand command;
    command.slot = slot;
    command.scale = (p->getAddr() >> (l3ReductionEnabled ? 24 : 20)) & 0xffffffffULL;
    command.context = p->req->contextId();
    command.accepted = clockEdge();
    command.serial = l3ReductionSerial++;
    command.halfContribution = p->getAddr() & cfReduction::HalfContribution;
    command.initialize = p->getAddr() & cfReduction::Initialize;
    panic_if(command.initialize && !l3ReductionEnabled,
             "Initialize reduction is only supported at shared L3");
    panic_if(command.halfContribution && !l3ReductionEnabled,
             "FP16 contribution ABI is only supported by the shared L3 engine");
    command.response = reductionAsync ? nullptr : p;
    std::memcpy(command.contribution.data(), p->getConstPtr<uint8_t>(), 256);
    for (unsigned i = 0; i < 4; ++i) ++reductionPending[slot + (Addr(i) << 16)];
    ++cfReduction::pending[{system, command.context}];
    reductionSource.push_back(command);
    DPRINTF(CacheFlexReductionTrace,
        "CFRED phase=capture serial=%llu slot=%#x source=%u flight=%u\n",
        (unsigned long long)command.serial, slot,
        (unsigned)reductionSource.size(), (unsigned)l3ReductionFlight.size());
    ++reductionAccepted;
    reductionMaxSource = std::max<double>(reductionMaxSource.value(), reductionSource.size());
    if (reductionAsync) {
        // CPU packet/sender state is never retained beyond this callback.
        schedule(new EventFunctionWrapper([this, p] { ruby_hit_callback(p); },
                 name() + ".reductionCaptureAck", true), clockEdge(Cycles(1)));
    }
    scheduleReduction();
    return RequestStatus_Issued;
}

void Sequencer::serviceReduction() {
    if (l3ReductionEnabled) { serviceL3Reduction(); return; }
    if (reductionLinkActive && clockEdge() >= reductionLinkDone) {
        // The sink slot was reserved when transfer started.
        panic_if(reductionSink.size() >= reductionSinkCap, "Lost reduction sink reservation");
        reductionSink.push_back(reductionSource.front());
        reductionSource.pop_front();
        reductionLinkActive = false;
        reductionMaxSink = std::max<double>(reductionMaxSink.value(), reductionSink.size());
        trySendRetries();
    }
    if (!reductionLinkActive && !reductionSource.empty()) {
        if (reductionSink.size() < reductionSinkCap) {
            reductionLinkActive = true;
            reductionTransportBytes += 272;
            reductionLinkDone = clockEdge(reductionLinkLatency +
                Cycles((272 + reductionLinkBytes - 1) / reductionLinkBytes));
        } else ++reductionSinkFullCycles;
    }
    if (!reductionSink.empty() && !reductionMemoryPacket) {
        auto &c = reductionSink.front();
        auto req = std::make_shared<Request>(c.slot, 256,
                    Request::SPMLD | Request::PHYSICAL, m_controller->getRequestorId());
        req->setSPMMetadata(c.slot, c.slot);
        req->setContext(c.context);
        reductionMemoryPacket = new Packet(req, MemCmd::ReadReq);
        reductionMemoryPacket->allocate();
        reductionMemoryIssued = false;
        reductionWriting = false;
    }
    if (reductionMemoryPacket && !reductionMemoryIssued &&
        (!reductionWriting || clockEdge() >= reductionArithmeticDone)) {
        auto status = makeRequest(reductionMemoryPacket);
        if (status == RequestStatus_Issued) reductionMemoryIssued = true;
        else ++reductionMemoryRetries;
    }
    if (!reductionSource.empty() || !reductionSink.empty()) scheduleReduction();
}

void Sequencer::reductionMemoryDone(PacketPtr p) {
    panic_if(reductionSink.empty() || p != reductionMemoryPacket,
             "Reduction completion without owning record");
    auto &c = reductionSink.front();
    if (!reductionWriting) {
        // Two rounded operations, exactly like the non-fused CPU baseline.
        // Preserve global SoftFloat environment, including exception flags.
        const auto rounding = softfloat_roundingMode;
        const auto flags = softfloat_exceptionFlags;
        softfloat_roundingMode = softfloat_round_near_even;
        auto req = std::make_shared<Request>(c.slot, 256,
                    Request::SPMST | Request::PHYSICAL, m_controller->getRequestorId());
        req->setSPMMetadata(c.slot, c.slot);
        req->setContext(c.context);
        auto *write = new Packet(req, MemCmd::WriteReq);
        write->allocate();
        for (unsigned i = 0; i < 64; ++i) {
            float32_t old, contribution;
            std::memcpy(&old.v, p->getConstPtr<uint8_t>() + 4*i, 4);
            std::memcpy(&contribution.v, c.contribution.data() + 4*i, 4);
            auto result = f32_add(f32_mul(old, float32_t{c.scale}), contribution);
            std::memcpy(write->getPtr<uint8_t>() + 4*i, &result.v, 4);
        }
        softfloat_roundingMode = rounding;
        softfloat_exceptionFlags = flags;
        delete p;
        reductionMemoryPacket = write;
        reductionWriting = true;
        reductionMemoryIssued = false;
        reductionArithmeticDone = clockEdge(reductionMulLatency + reductionAddLatency +
                                   Cycles((64 + reductionLanes - 1) / reductionLanes - 1));
    } else {
        delete p;
        reductionMemoryPacket = nullptr;
        for (unsigned i = 0; i < 4; ++i) {
            auto it = reductionPending.find(c.slot + (Addr(i) << 16));
            panic_if(it == reductionPending.end() || !it->second, "Lost destination hazard record");
            if (!--it->second) reductionPending.erase(it);
        }
        auto it = cfReduction::pending.find({system, c.context});
        panic_if(it == cfReduction::pending.end() || !it->second, "Lost barrier credit");
        if (!--it->second) cfReduction::pending.erase(it);
        ++reductionCompleted;
        reductionLifetimeCycles += (clockEdge() - c.accepted) / clockPeriod();
        auto *response = c.response;
        reductionSink.pop_front();
        if (response) ruby_hit_callback(response);
        trySendRetries();
        testDrainComplete();
    }
    scheduleReduction();
}

void Sequencer::serviceL3Reduction() {
    // Passive observation only: no new events, reservations, or timing state.
    // Global --debug-start/--debug-end bound the trace. Sample every 64 core
    // cycles; capture/issue/complete events retain individual command identity.
    if (DTRACE(CacheFlexReductionTrace) &&
        ((clockEdge() / clockPeriod()) % 64 == 0)) {
        Tick oldest = clockEdge();
        for (const auto &c : reductionSource)
            oldest = std::min(oldest, c.accepted);
        for (const auto &[p, f] : l3ReductionFlight)
            oldest = std::min(oldest, f.command.accepted);
        DPRINTF(CacheFlexReductionTrace,
            "CFRED phase=sample source=%u flight=%u oldest_age=%llu\n",
            (unsigned)reductionSource.size(), (unsigned)l3ReductionFlight.size(),
            (unsigned long long)((clockEdge() - oldest) / clockPeriod()));
    }
    // Source capture and the normal timed memory/network path are separate.
    // The finite in-flight records retain completion metadata, not accumulator data.
    if (!reductionSource.empty() && l3ReductionFlight.size() < reductionSinkCap) {
        auto c = reductionSource.front();
        const Addr raw = (Addr(1)<<59) | (c.slot & 0x77ffff);
        const unsigned payload = c.halfContribution ? 128 : 256;
        auto req = std::make_shared<Request>(raw,payload,
                    Request::SPMST | Request::PHYSICAL,m_controller->getRequestorId());
        req->setSPMMetadata(cfReduction::Command | c.scale |
            (c.initialize ? cfReduction::Initialize : 0) |
            (c.halfContribution ? cfReduction::HalfContribution : 0),raw);
        req->setContext(c.context);
        auto *p = new Packet(req,MemCmd::WriteReq);p->allocate();
        std::memcpy(p->getPtr<uint8_t>(),c.contribution.data(),payload);
        l3ReductionFlight.emplace(p,L3ReductionFlight{c,false});
        reductionSource.pop_front();
        reductionMaxSink=std::max<double>(reductionMaxSink.value(),l3ReductionFlight.size());
        trySendRetries();
    }
    // Retry in capture order; never let a later recurrence overtake an older one.
    PacketPtr next=nullptr;
    for (auto &[p,f] : l3ReductionFlight)
        if (!f.issued && (!next || f.command.serial < l3ReductionFlight.at(next).command.serial)) next=p;
    if(next) {
        const auto &c=l3ReductionFlight.at(next).command;
        for(const auto &[p,f]:l3ReductionFlight)
            if(p!=next && f.issued && f.command.serial<c.serial &&
               overlap(c.slot,256,f.command.slot,256)) { next=nullptr;break; }
    }
    if (next) {
        if (makeRequest(next)==RequestStatus_Issued) {
            DPRINTF(CacheFlexReductionTrace,
                "CFRED phase=issue serial=%llu slot=%#x age=%llu\n",
                (unsigned long long)l3ReductionFlight.at(next).command.serial,
                l3ReductionFlight.at(next).command.slot,
                (unsigned long long)((clockEdge() - l3ReductionFlight.at(next).command.accepted) / clockPeriod()));
            l3ReductionFlight.at(next).issued=true;
            reductionTransportBytes+=next->getSize()+16;
        } else ++reductionMemoryRetries;
    }
    if (!reductionSource.empty() || !l3ReductionFlight.empty()) scheduleReduction();
}

void Sequencer::completeL3Reduction(PacketPtr p) {
    const auto c=l3ReductionFlight.at(p).command;
    DPRINTF(CacheFlexReductionTrace,
        "CFRED phase=complete serial=%llu slot=%#x age=%llu\n",
        (unsigned long long)c.serial, c.slot,
        (unsigned long long)((clockEdge() - c.accepted) / clockPeriod()));
    for(unsigned i=0;i<4;++i) {
        auto it=reductionPending.find(c.slot+(Addr(i)<<16));
        panic_if(it==reductionPending.end() || !it->second,"Lost shared reduction hazard");
        if (!--it->second) reductionPending.erase(it);
    }
    auto it=cfReduction::pending.find({system,c.context});
    panic_if(it==cfReduction::pending.end() || !it->second,"Lost shared reduction barrier credit");
    if (!--it->second) cfReduction::pending.erase(it);
    l3ReductionFlight.erase(p);delete p;
    ++reductionCompleted;
    reductionLifetimeCycles+=(clockEdge()-c.accepted)/clockPeriod();
    if(c.response) ruby_hit_callback(c.response);
    trySendRetries();testDrainComplete();scheduleReduction();
}
} } // namespace gem5::ruby
