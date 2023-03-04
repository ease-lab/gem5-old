/**
 * Copyright (c) 2019 Metempsy Technology Consulting
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

#include "cpu/pred/predecoder_btb.hh"

#include <utility>
#include "base/intmath.hh"
#include "cpu/o3/dyn_inst.hh"
#include "debug/BTB.hh"
#include "base/trace.hh"

namespace gem5
{


namespace branch_prediction
{

PredecoderBTB::PredecoderBTB(const PredecoderBTBParams &p)
    : AssociativeBTB(p),
      cache(p.cache), cpu(p.cpu),
      blkSize(p.block_size), lBlkSize(floorLog2(blkSize)),
      onlyPrefetchFill(p.only_prefetch_fill),
      installConditional(p.install_conditional),
      takenPredSize(p.taken_pred_size),
      disablePredecode(p.disable_predecode),
      decodeQueueEnabled(p.decode_queue_enable),
      decodeQueueSize(p.decode_queue_size),
      decodeDelayTicks(cyclesToTicks(p.decode_queue_cycles)),
      decodeQueueEvent([this]{ decodeQueueEventWrapper(); }, name()),
      predecStats(this)
{
    fatal_if((takenPredSize > 2) && (!isPowerOf2(takenPredSize)),
        "The number of entries of the taken predictor must be a power of 2");
    takenPredictor.resize(takenPredSize);

    DPRINTF(BTB, "Config delay: %i clks / %i ticks \n",
                    p.decode_queue_cycles, decodeDelayTicks);

}

PredecoderBTB::~PredecoderBTB()
{
    for (auto l = listeners.begin(); l != listeners.end(); ++l) {
        delete (*l);
    }
    listeners.clear();
}



void
PredecoderBTB::regProbeListeners()
{
    // Register cache listeners
    assert(cache);
    typedef ProbeListenerArgFn<PacketPtr> CacheListener;

    listeners.push_back(
            new CacheListener(cache->getProbeManager(), "Miss",
                [this](const PacketPtr& pkt)
                    { notifyAccess(pkt, true); }));
    listeners.push_back(
            new CacheListener(cache->getProbeManager(), "Hit",
                [this](const PacketPtr& pkt)
                    { notifyAccess(pkt, false); }));
    listeners.push_back(
            new CacheListener(cache->getProbeManager(), "Fill",
                [this](const PacketPtr& pkt)
                    { notifyFill(pkt); }));


    // Register CPU listeners
    assert(cpu);
    typedef ProbeListenerArgFn<o3::DynInstConstPtr> FetchListener;
    listeners.push_back(new FetchListener(cpu->getProbeManager(),
                "Fetch", [this](const o3::DynInstConstPtr& dynInst)
                    { notifyFetch(dynInst); }));
}


void
PredecoderBTB::notifyAccess(const PacketPtr &pkt, bool miss)
{
    if (!pkt->req->hasVaddr() || !pkt->req->hasPaddr()) {
        return;
    }

    Addr pa = blockAddress(pkt->req->getPaddr());
    Addr va = blockAddress(pkt->req->getVaddr());

    backwardTranslationMap[pa] = va;

    // if (traceMisses && miss) {
    //     notifyMiss(pkt);
    // }
    return;
}



void
PredecoderBTB::notifyFill(const PacketPtr &pkt)
{
    // DPRINTF(BTB, "Fill %#x VA: %#x in cache.\n", pkt->getAddr(),
    //         pkt->req->hasVaddr() ? pkt->req->getVaddr() : 0);

    predecStats.cacheFills++;

    if (!pkt->req->isPrefetch()) {
        predecStats.cacheFillsDemand++;
        if (onlyPrefetchFill) return;
    }

    if (disablePredecode) {
        return;
    }

    Addr vaddr = translate(pkt);
    if (vaddr == MaxAddr) {
        return;
    }
    DPRINTF(BTB, "Start decoding CL:%#x VA:%#x\n",
            pkt->getAddr(), vaddr);
    insertIntoDecodeQueue(vaddr);

}

void
PredecoderBTB::notifyFetch(const o3::DynInstConstPtr& dynInst)
{
    if (!dynInst->isLastMicroop()) return;
    if (!dynInst->isControl()) return;

    recordBranch(
        dynInst->pcState().instAddr(), *(dynInst->branchTarget()),
        dynInst->staticInst, getBranchClass(dynInst->staticInst),
        dynInst->pcState().branching());
}



Addr
PredecoderBTB::translate(const PacketPtr &pkt)
{
    Addr pa = blockAddress(pkt->req->getPaddr());
    Addr va = blockAddress(pkt->req->hasVaddr() ? pkt->req->getVaddr() : 0);

    // Find translation if no VA available. Find does not change the container.
    if (va) {
        backwardTranslationMap[pa] = va;
    } else {
        va = backwardTranslationMap[pa];
        if (!va) {
            DPRINTF(BTB, "No transl. found for PA: %#x.\n", pa);
            return MaxAddr;
        }
    }
    return va;
}


void
PredecoderBTB::insertIntoDecodeQueue(Addr x)
{
    if (decodeQueue.size() == decodeQueueSize) {
        return;
    }

    // Add the address to the decode queue and schedule an event to process
    // it after the specified decode cycles
    Tick process_tick = curTick() + decodeDelayTicks;

    decodeQueue.push_back(DecodeQueueEntry(x, process_tick));

    if (!decodeQueueEvent.scheduled()) {
        schedule(decodeQueueEvent, process_tick);
    }
}

void
PredecoderBTB::decodeQueueEventWrapper()
{
    while (!decodeQueue.empty() &&
            decodeQueue.front().processTick <= curTick())
    {
        Addr addr_x = decodeQueue.front().baseAddr;
        finishDecoding(addr_x);
        decodeQueue.pop_front();
    }

    // Schedule an event for the next element if there is one
    if (!decodeQueue.empty()) {
        schedule(decodeQueueEvent, decodeQueue.front().processTick);
    }
}


void
PredecoderBTB::finishDecoding(Addr vaddr)
{

    auto brInfos = branchMap.find(vaddr);
    if (brInfos == branchMap.end()) {
        DPRINTF(BTB, "No branches in CL:%#x found\n", vaddr);
        return;
    }

    // Extract all branches previously recorded.
    auto branches = &(brInfos->second);
    DPRINTF(BTB, " %i branches from CL:%#x.\n",
                    branches->size(),vaddr);

    predecStats.clsDecoded++;

    for (auto branch = branches->begin();
             branch != branches->end(); branch++) {

        assert(branch->first == branch->second.pc);

        // Check whether to install the branch or not.
        if (!predTaken(branch->second)) {
            continue;
        }

        if (Base::valid(branch->second.tid, branch->second.pc)) {

            DPRINTF(BTB, "Branch %#x already in BTB.[%s,%s]\n",
                branch->second.pc, toStr(branch->second.type),
                *(branch->second.target));
            predecStats.btbHits++;

        } else {

            DPRINTF(BTB, "Install new branch %#x in BTB.[%s,%s]\n",
                branch->second.pc, toStr(branch->second.type),
                *(branch->second.target));

            // // Record the branch to help the predecoder
            // recordBranch(instPC, target_pc, inst, type, true);
            uint64_t idx = getIndex(branch->second.tid, branch->second.pc);
            BTBEntry * entry = btb->findEntry(idx, /* unused */ false);

            // Do the actual update.
            Base::updateEntry(entry, branch->second.tid, branch->second.pc,
                              *(branch->second.target), branch->second.type,
                               branch->second.inst, 0);

            // Mark the entry as prefetch
            entry->prefetched = true;
            predecStats.btbFills++;
            predecStats.btbFillTypes[branch->second.type]++;
        }
    }
}


bool
PredecoderBTB::predTaken(BranchInfo& branch)
{
    // Install all conditional branches
    if(!branch.conditional || installConditional) {
        return true;
    }

    // If the predictior is disabled do not install any conditional branch.
    if(takenPredictor.size() == 0) {
        return false;
    }

    // Predict if the branch should be inserted or not.
    unsigned idx = index(branch.pc);
    uint8_t taken = takenPredictor[idx];

    predecStats.takenPred++;
    if (taken != branch.taken) {
        predecStats.takenMiss++;
    }

    DPRINTF(BTB, "Predict PC:%#x taken:%i \n", branch.pc, taken);
    return taken;
}

void
PredecoderBTB::updateTaken(const o3::DynInstConstPtr& dynInst)
{
    // Check if the predictor is used at all
    if(takenPredSize == 0) {
        return;
    }

    // Only conditional taken will get predcited
    //
    if(!dynInst->isCondCtrl() ||
       !dynInst->pcState().branching()) {
        return;
    }

    // Predict if the branch should be inserted or not.
    unsigned idx = index(dynInst->pcState().instAddr());
    takenPredictor[idx] = 1;

    DPRINTF(BTB, "Update taken predictor for PC:%#x\n",
                dynInst->pcState().instAddr());
    return;
}



void
PredecoderBTB::update(ThreadID tid, Addr instPC, const PCStateBase &target_pc,
                    BranchClass type, StaticInstPtr inst, unsigned inst_size)
{
    // // Record the branch to help the predecoder
    // recordBranch(instPC, target_pc, inst, type, true);
    uint64_t idx = getIndex(tid, instPC);
    BTBEntry * entry = btb->findEntry(idx, /* unused */ false);

    // Do the actual update.
    Base::updateEntry(entry, tid, instPC, target_pc, type, inst, inst_size);
}


void
PredecoderBTB::recordBranch(const Addr pc, const PCStateBase& target,
                        StaticInstPtr inst, BranchClass type, bool taken)
{


    DPRINTF(BTB, "%s: PC:%#x %s, taken:%i, target: %s.\n", __func__,
                pc, toStr(type), taken, target);

    // The predecoder only knows if its a branch or not. it does not know
    // if it is taken or not so we insert it any way.
    predecStats.recordedBranches++;

    Addr blkPC = blockAddress(pc);

    auto brInfo = &branchMap[blkPC][pc];
    if (brInfo->pc == MaxAddr) {
        // New branch will be added
        predecStats.recordedBranchesUnique++;

        brInfo->pc = pc;
        set(brInfo->target, target);
        brInfo->tid = 0;
        brInfo->conditional = (type == BranchClass::DirectCond);
        brInfo->taken = taken;
        brInfo->type = type;
        brInfo->inst = inst;

        DPRINTF(BTB, "Add new branch to CL: %#x > PC %#x\n",
                        blkPC, branchMap[blkPC][pc].pc);
    }
}






PredecoderBTB::PredecoderBTBStats::PredecoderBTBStats(
                                                statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(cacheFills, statistics::units::Count::get(),
               "Number of cache lines filled by the cache lookups"),
      ADD_STAT(cacheFillsDemand, statistics::units::Count::get(),
               "Number of cache lines filled by the cache lookups"),
      ADD_STAT(clsDecoded, statistics::units::Count::get(),
               "Number of BTB lookups per branch type"),
      ADD_STAT(btbHits, statistics::units::Count::get(),
               "Number of BTB misses"),
      ADD_STAT(btbMisses, statistics::units::Count::get(),
               "Number of BTB misses per branch type"),
    //   ADD_STAT(missRatio, statistics::units::Ratio::get(), "BTB Hit Ratio",
    //            misses / lookups),
      ADD_STAT(btbFills, statistics::units::Count::get(),
               "Number of BTB updates"),
      ADD_STAT(btbFillTypes, statistics::units::Count::get(),
               "Number of BTB updates per branch type"),
      ADD_STAT(takenPred, statistics::units::Count::get(),
               "Num. of predictions made whether a branch was taken or not"),
      ADD_STAT(takenMiss, statistics::units::Count::get(),
               "Number of miss predictions"),
      ADD_STAT(recordedBranches, statistics::units::Count::get(),
               "Number of miss predictions"),
      ADD_STAT(recordedBranchesUnique, statistics::units::Count::get(),
               "Number of miss predictions")



    //   ADD_STAT(updateType, statistics::units::Count::get(),
    //            "Number of BTB updates per branch type"),
    //   ADD_STAT(evictions, statistics::units::Count::get(),
    //            "Number of BTB evictions"),
    //   ADD_STAT(evictionType, statistics::units::Count::get(),
    //            "Number of BTB evictions per branch type"),
    //   ADD_STAT(mispredict, statistics::units::Count::get(),
    //            "Number of BTB mispredicts"),
    //   ADD_STAT(mispredictType, statistics::units::Count::get(),
    //            "Number of BTB mispredicts per branch type")
{

    using namespace statistics;
    // missRatio.precision(6);
    // missesCausedBy
    //     .init(enums::Num_BranchClass)
    //     .flags(total | pdf);
    btbFillTypes
        .init(enums::Num_BranchClass)
        .flags(total | pdf);

    // missType
    //     .init(enums::Num_BranchClass)
    //     .flags(total | pdf);

    // updateType
    //     .init(enums::Num_BranchClass)
    //     .flags(total | pdf);

    // evictionType
    //     .init(enums::Num_BranchClass)
    //     .flags(total | pdf);

    // mispredictType
    //     .init(enums::Num_BranchClass)
    //     .flags(total | pdf);


    for (int i = 0; i < enums::Num_BranchClass; i++) {
        // missesCausedBy.subname(i, enums::BranchClassStrings[i]);
        btbFillTypes.subname(i, enums::BranchClassStrings[i]);
    //     updateType.subname(i, enums::BranchClassStrings[i]);
    //     evictionType.subname(i, enums::BranchClassStrings[i]);
    //     mispredictType.subname(i, enums::BranchClassStrings[i]);
    }
}

} // namespace prefetch
} // namespace gem5
