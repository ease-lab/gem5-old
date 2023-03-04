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

#include "cpu/pred/prefetch_btb.hh"

#include <utility>
#include "base/intmath.hh"
#include "cpu/o3/dyn_inst.hh"
#include "debug/BTB.hh"
#include "base/trace.hh"

namespace gem5
{


namespace branch_prediction
{

PrefetchBTB::PrefetchBTB(const PrefetchBTBParams &p)
    : AssociativeBTB(p),
      blkSize(p.block_size), lBlkSize(floorLog2(blkSize)),
      onlyPrefetchFill(p.only_prefetch_fill),
      installConditional(p.install_conditional),
      bitsLongTgt(p.size_long_tgt), bitsShortTgt(p.size_short_tgt),
      bitsFullPC(p.size_full_pc), bitsDeltaPC(p.size_delta_pc),
      deltaPCTgtThreshold(p.delta_pctgt_threshold),
      deltaTgtPCThreshold(p.delta_tgtpc_threshold),
      throttleFactorPct(p.throttle_factor),
      queueEnabled(p.queue_enable), queueSize(p.queue_size),
      delay(cyclesToTicks(p.delay)),
      bits_ready(0),
      memIF(p.mem_if), recording(false), replaying(false),
      processQueueEvent([this]{ processQueueEventWrapper(); }, name()),
      prefetchStats(this)
{
    // DPRINTF(BTB, "Config: delay %i clks, rec size %ib -> %i pre CL\n",
    //              p.delay, recordSizeFull, recordSizeFull/(blkSize*8));

    maxPrefetchEntries = (numEntries * throttleFactorPct) / 100;
    prefetchesInBTB = 0;
    DPRINTF(BTB, "Throttle factor: %i% -> max prefetch entries: %i\n",
                 throttleFactorPct, maxPrefetchEntries);

    assert(memIF != nullptr);

    // Prepare the read/write buffers and register the receive callback
    write_data = new uint8_t[blkSize];
    read_data = new uint8_t[blkSize];

    memIF->registerReceiveCallback([this](uint8_t* d) -> bool
                                    { return receiveRecord(d); });
}

PrefetchBTB::~PrefetchBTB()
{
    // for (auto l = listeners.begin(); l != listeners.end(); ++l) {
    //     delete (*l);
    // }
    // listeners.clear();
    delete[] write_data;
    delete[] read_data;
}



void
PrefetchBTB::regProbeListeners()
{

}

void
PrefetchBTB::memInvalidate()
{
    Base::memInvalidate();
    prefetchesInBTB = 0;
}

void
PrefetchBTB::update(ThreadID tid, Addr instPC, const PCStateBase &target_pc,
                    BranchClass type, StaticInstPtr inst, unsigned inst_size)
{
    // // Record the branch to help the predecoder
    // recordBranch(instPC, target_pc, inst, type, true);
    uint64_t idx = getIndex(tid, instPC);
    BTBEntry * entry = btb->findEntry(idx, /* unused */ false);
    bool hit = (entry != nullptr);

    // Do the actual update.
    Base::updateEntry(entry, tid, instPC, target_pc, type, inst, inst_size);

    if (!hit) {
        recordBranch(instPC, target_pc, inst, type, true);
    }
}

// const PCStateBase *
// PrefetchBTB::lookup(ThreadID tid, Addr instPC, BranchClass type)
// {
//     uint64_t idx = getIndex(tid, instPC);
//     BTBEntry * entry = btb->findEntry(idx, /* unused */ false);

//     if (entry != nullptr && entry->prefetched) {

//     }
// }


void
PrefetchBTB::hitOnPrefetch(BTBEntry* entry)
{
    DPRINTF(BTB, "%s: PC:%#x, N_pf in BTB:%i \n", __func__,
                entry->pc, prefetchesInBTB);
    // assert(prefetchesInBTB > 0);
    prefetchesInBTB--;
    updateReplayTrottle();

    prefetchStats.recordedUsefulPf++;
    recordBranch(entry->pc, *(entry->target), entry->inst,
                 getBranchClass(entry->inst), true);

    Base::hitOnPrefetch(entry);
}

void
PrefetchBTB::unusedPrefetch(BTBEntry* entry)
{
    DPRINTF(BTB, "%s: PC:%#x, N_pf in BTB:%i \n", __func__,
                entry->pc, prefetchesInBTB);
    // assert(prefetchesInBTB > 0);
    prefetchesInBTB--;
    updateReplayTrottle();

    Base::unusedPrefetch(entry);
}


void
PrefetchBTB::recordBranch(const Addr pc, const PCStateBase& target,
                        StaticInstPtr inst, BranchClass type, bool taken)
{
    static Addr lastTgt = MaxAddr;
    static Addr lastPC = MaxAddr;
    int64_t deltaPCTgt = pc - target.instAddr();
    int64_t deltaTgtPC = lastTgt - pc;
    int64_t deltaPCPC = lastPC - pc;

    lastTgt = target.instAddr();
    lastPC = pc;

    auto d_lg2_pctgt = (deltaPCTgt == 0) ? 0 : floorLog2((deltaPCTgt < 0) ?
                                                    -deltaPCTgt : deltaPCTgt);
    prefetchStats.deltaPCTgt.sample(d_lg2_pctgt);

    auto d_lg2_tgtpc = (deltaTgtPC == 0) ? 0 : floorLog2((deltaTgtPC < 0) ?
                                                    -deltaTgtPC : deltaTgtPC);
    prefetchStats.deltaTgtPC.sample(d_lg2_tgtpc);

    auto d_lg2_pcpc = (deltaPCPC == 0) ? 0 : floorLog2((deltaPCPC < 0) ?
                                                    -deltaPCPC : deltaPCPC);
    prefetchStats.deltaPCPC.sample(d_lg2_pcpc);



    DPRINTF(BTB, "%s: PC:%#x %s, taken:%i, target:%s. deltaPCTgt:%#x, deltaTgtPC:%#x\n", __func__,
                pc, toStr(type), taken, target, deltaPCTgt, deltaTgtPC);

    if (!recording) return;

    prefetchStats.recordedBranches++;

    auto brInfo = &branchMap[pc];
    if (brInfo->pc == MaxAddr) {
        // New branch will be added
        prefetchStats.recordedBranchesUnique++;

        brInfo->pc = pc;
        set(brInfo->target, target);
        brInfo->tid = 0;
        brInfo->inst = inst;
    }

    // Check if we need to store the full target offset or
    // if it fits in a compressed entry.
    unsigned _bits = 0;

    if (d_lg2_pctgt < bitsShortTgt) {
        _bits += bitsShortTgt;
        prefetchStats.recordedShortTgt++;
    } else {
        _bits += bitsLongTgt;
        prefetchStats.recordedLongTgt++;
    }

    /** Delta between last target and this PC*/
    if (d_lg2_tgtpc < bitsDeltaPC) {
        _bits += bitsDeltaPC;
        prefetchStats.recordedDeltaPC++;
    } else {
        _bits += bitsFullPC;
        prefetchStats.recordedFullPC++;
    }

    if (_bits == (bitsShortTgt + bitsDeltaPC)) {
        prefetchStats.recordedDoubleShort++;
    }

    // Two bits for branch type and two bits for entry types
    bits_ready += _bits + 2 + 2;

    curRecords.emplace_back(pc);

    /** Check if one cache line is ready to write */
    if ((bits_ready < blkSize * 8)) {
        return;
    }

    writeCacheLine();
    bits_ready -= blkSize * 8;
}

void
PrefetchBTB::writeCacheLine()
{
    // CL number
    static unsigned cl_num = 0;
    cl_num++;

    DPRINTF(BTB, "Write CL:%i with %i entries to memory\n",
            cl_num, curRecords.size());

    // Write only the CL number into the array to send it to memory
    memcpy(write_data, &cl_num, sizeof(cl_num));

    // Attempt to write the CL.
    if (!memIF->write(write_data)) {
        // The write was not successful. Drop the records
        curRecords.clear();
        DPRINTF(BTB, "Write fail: delete record.\n");
        return;
    }

    // Write was successful.
    // Create a new element in the record map and move the elements
    // from the current records to the new list
    // auto& element = records[cl_num] = std::list<Addr>();
    // element.splice(element.begin(), curRecords);
    // records[cl_num].splice(records[cl_num].begin(), curRecords);
    records.push(std::make_pair(cl_num, std::list<Addr>()));
    records.back().second.splice(records.back().second.begin(), curRecords);

    DPRINTF(BTB, "Store rec info sz:%i curRecords:%i back:%i \n",
            records.size(), curRecords.size(), records.back().second.size());
}


bool
PrefetchBTB::receiveRecord(uint8_t* data)
{
    unsigned cln;
    memcpy(&cln, data, sizeof(cln));

    // auto element = records.find(cln);
    // if (element == records.end()) {
    //     DPRINTF(BTB, "Receive CL:%i from memory: no info found!!\n", cln);
    //     return false;
    // }

    auto record = records.front();
    while ((record.first < cln) && records.size()) {
        records.pop();
        record = records.front();
    }
    assert(record.first == cln);


    DPRINTF(BTB, "Receive CL:%i from memory: rec_size:%i element:%i\n",
                    cln, records.size(), record.second.size());


    // Lookup for all addresses stored in this CL the branch
    // info to install it into the BTB
    for (Addr rec : record.second) {

        // auto bri = branchMap.find(rec);
        // if (bri == branchMap.end()) {
        //     DPRINTF(BTB, "No branch info found for %#x\n", rec);
        //     continue;
        // }
        // assert(rec == bri->second.pc);

        // installBranch(bri->second);
        insertIntoProcessQueue(rec);
    }

    // records.erase(element);
    records.pop();

    /** Check throttling */
    updateReplayTrottle();

    return true;
}


void
PrefetchBTB::insertIntoProcessQueue(Addr x)
{
    if (processQueue.size() == queueSize) {
        return;
    }
    // We can only install one branch per cycle.
    // Keep therefore always tick when the last branch was installed
    static Tick last_install_tick=0;

    // Add the address to the decode queue and schedule an event to process
    // it after the specified decode cycles
    Tick process_tick = curTick() + delay;
    if (process_tick < (last_install_tick + clockPeriod())) {
        process_tick = last_install_tick + clockPeriod();
    }
    last_install_tick = process_tick;

    processQueue.push_back(ProcessQueueEntry(x, process_tick));

    if (!processQueueEvent.scheduled()) {
        schedule(processQueueEvent, process_tick);
    }
}

void
PrefetchBTB::processQueueEventWrapper()
{
    while (!processQueue.empty() &&
            processQueue.front().processTick <= curTick())
    {
        Addr pc = processQueue.front().pc;

        auto bri = branchMap.find(pc);
        if (bri == branchMap.end()) {
            DPRINTF(BTB, "No branch info found for %#x\n", pc);
            continue;
        }
        assert(pc == bri->second.pc);

        installBranch(bri->second);
        processQueue.pop_front();
    }

    // Schedule an event for the next element if there is one
    if (!processQueue.empty()) {
        schedule(processQueueEvent, processQueue.front().processTick);
    }
}




void
PrefetchBTB::installBranch(BranchInfo& bi)
{
    // // Check whether to install the branch or not.
    // if (!predTaken(bi)) {
    //     return;
    // }

    uint64_t idx = getIndex(bi.tid, bi.pc);
    BTBEntry * entry = btb->findEntry(idx, /* unused */ false);

    if (entry != nullptr) {
        // Only implemented for single thread.
        assert(entry->tid == 0);

        DPRINTF(BTB, "Branch %#x already in BTB.\n", bi.pc);
        if (entry->prefetched)
            prefetchStats.btbFillHitOnPf++;
        else
            prefetchStats.btbFillHitOnDemand++;

    } else {

        BranchClass type = getBranchClass(bi.inst);
        DPRINTF(BTB, "Install new branch %#x in BTB.[%s,%s] N_pf in BTB:%i\n",
                      bi.pc, toStr(type), *(bi.target), prefetchesInBTB+1);

        // Do the actual update.
        Base::updateEntry(entry, bi.tid, bi.pc, *(bi.target), type,
                            bi.inst, 0);

        // Mark the entry as prefetch
        entry->prefetched = true;
        prefetchStats.btbFills++;
        prefetchStats.btbFillTypes[type]++;
        prefetchesInBTB++;
    }
}

void
PrefetchBTB::updateReplayTrottle()
{
    // Check if we already exceed the threshold
    if (!pause_replay && (prefetchesInBTB > maxPrefetchEntries)) {
        memIF->stopReplay();
        pause_replay=true;
        DPRINTF(BTB, "Prefetch limit reached: %i/%i-> pause replay\n",
                    prefetchesInBTB, numEntries);
        return;
    }

    if (pause_replay && (prefetchesInBTB < maxPrefetchEntries)) {
        pause_replay=false;
        memIF->startReplay();
    }
}

void
PrefetchBTB::startRecord()
{
    memIF->resetRecord();
    memIF->startRecord();
    recording=true;
}

void
PrefetchBTB::stopRecord()
{
    // Write the last entries to memory
    writeCacheLine();
    bits_ready = 0;
    memIF->stopRecord();
    recording=false;
}

void
PrefetchBTB::startReplay()
{
    memIF->startReplay();
    replaying=true;
    pause_replay=false;
    prefetchesInBTB=0;
}

void
PrefetchBTB::stopReplay()
{
    memIF->stopReplay();
    replaying=false;
}





PrefetchBTB::PrefetchBTBStats::PrefetchBTBStats(PrefetchBTB *parent)
    : statistics::Group(parent),

      ADD_STAT(btbFillHitOnPf, statistics::units::Count::get(),
               "Number of records not installed. Hit on existing prefetch."),
      ADD_STAT(btbFillHitOnDemand, statistics::units::Count::get(),
               "Number of records not installed. Hit on exiting entry."),
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
      ADD_STAT(recordedUsefulPf, statistics::units::Count::get(),
               "Number useful prefetches recorded again"),
      ADD_STAT(recordedBranchesUnique, statistics::units::Count::get(),
               "Number of miss predictions"),
      ADD_STAT(recordedLongTgt, statistics::units::Count::get(),
               "Number of miss predictions"),
      ADD_STAT(recordedShortTgt, statistics::units::Count::get(),
               "Number of miss predictions"),
      ADD_STAT(recordedFullPC, statistics::units::Count::get(),
               "Number of miss predictions"),
      ADD_STAT(recordedDeltaPC, statistics::units::Count::get(),
               "Number of miss predictions"),
      ADD_STAT(recordedDoubleShort, statistics::units::Count::get(),
               "Number of miss predictions"),
      ADD_STAT(deltaPCTgt, statistics::units::Count::get(),
               "Log2 distance PC -> Target"),
      ADD_STAT(deltaTgtPC, statistics::units::Count::get(),
               "Log2 distance target -> PC"),
      ADD_STAT(deltaPCPC, statistics::units::Count::get(),
               "Log2 distance target PC -> PC")

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

    deltaPCTgt
        .init(0, 31, 2)
        .flags(total | pdf);
    deltaTgtPC
        .init(0, 31, 2)
        .flags(total | pdf);
    deltaPCPC
        .init(0, 31, 2)
        .flags(total | pdf);
}

} // namespace prefetch
} // namespace gem5
