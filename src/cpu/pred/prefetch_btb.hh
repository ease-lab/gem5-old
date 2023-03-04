/*
 * Copyright (c) 2022 The University of Edinburgh
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

/** Implementation a pre-fetcher for filling the BTB.
 */

#ifndef __CPU_PRED_PREFETCHER_BTB_HH__
#define __CPU_PRED_PREFETCHER_BTB_HH__

#include <queue>
#include <map>
// #include <vector>

// #include "base/circular_queue.hh"
// #include "mem/cache/prefetch/associative_set.hh"
// #include "mem/cache/prefetch/queued.hh"
#include "cpu/base.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/pred/associative_btb.hh"
#include "cpu/static_inst.hh"
#include "mem/cache/base.hh"
#include "mem/cache/prefetch/mem_interface.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "params/PrefetchBTB.hh"
#include "sim/byteswap.hh"
#include "sim/clocked_object.hh"
#include "sim/probe/probe.hh"
#include "base/statistics.hh"


namespace gem5
{

struct PrefetchBTBParams;

namespace branch_prediction
{

class PrefetchBTB : public AssociativeBTB {

    typedef AssociativeBTB Base;

  public:
    PrefetchBTB(const PrefetchBTBParams &p);
    ~PrefetchBTB();

    /**
     * Register probe points for this object.
     */
    void regProbeListeners() override;

    /** Helper to register the cache */
    void addListeners(BaseCache *_cache, BaseCPU *_cpu) { }

   private:

    void memInvalidate() override;

    /** Override the update function to record a branch mapping. */
    void update(ThreadID tid, Addr instPC,
                        const PCStateBase &target_pc,
                        BranchClass type = BranchClass::NoBranch,
                        StaticInstPtr inst = nullptr,
                        unsigned inst_size = 0) override;

    void hitOnPrefetch(BTBEntry* entry) override;
    void unusedPrefetch(BTBEntry* entry) override;

    /** The block size of the parent cache. */
    const unsigned blkSize;

    /** log_2(block size of the parent cache). */
    const unsigned lBlkSize;

    /** Only predecode on a fill brought into the cache by a prefetch*/
    const bool onlyPrefetchFill;

    /** Fill the BTB with all conditinal branches */
    const bool installConditional;

    /** Bit size for long and short targets */
    const unsigned bitsLongTgt;
    const unsigned bitsShortTgt;
    /** Bits sizes for full PC and delta PC entries*/
    const unsigned bitsFullPC;
    const unsigned bitsDeltaPC;

    /** Threshold to decide of whether a entry can be compressed or not */
    const unsigned deltaPCTgtThreshold;
    const unsigned deltaTgtPCThreshold;


    /** Max percentage of BTB entries that can be used for prefetches */
    const unsigned int throttleFactorPct;
    signed maxPrefetchEntries;
    signed prefetchesInBTB;


    /** Predecoding takes a bit of time.
     * The decode queue adds a delay before entries are available for
     * insertion.
     */
    const bool         queueEnabled;
    const unsigned int queueSize;
    const unsigned int delay;



    Addr blockAddress(Addr a) const { return a & ~((Addr)blkSize - 1); }
    Addr blockIndex(Addr a) const { return a >> lBlkSize; }

    std::string toStr(BranchClass type) const {
        return std::string(enums::BranchClassStrings[type]);
    }

    BranchClass getBranchClass(StaticInstPtr inst) {
        if (inst->isReturn()) {
            return BranchClass::Return;
        }

        if (inst->isCall()) {
            return inst->isDirectCtrl() ? BranchClass::CallDirect
                                        : BranchClass::CallIndirect;
        }

        if (inst->isDirectCtrl()) {
            return inst->isCondCtrl() ? BranchClass::DirectCond
                                      : BranchClass::DirectUncond;
        }

        if (inst->isIndirectCtrl()) {
            return inst->isCondCtrl() ? BranchClass::IndirectCond
                                      : BranchClass::IndirectUncond;
        }
        return BranchClass::NoBranch;
    }

    bool isUncond(BranchClass type) {
        return (type == BranchClass::NoBranch
            || type == BranchClass::DirectCond
            || type == BranchClass::IndirectCond)
            ? false : true;
    }
    bool isCtxChange(BranchClass type) {
        return (type == BranchClass::CallDirect
            || type == BranchClass::CallIndirect
            || type == BranchClass::Return)
            ? true : false;
    }

  private:

    /** Recording functionality */
    void recordBranch(const Addr pc, const PCStateBase& target,
                    StaticInstPtr inst, BranchClass type, bool taken);


    /** We keep all the information to populate the BTB entries in
     * a lookup table. We only write the address plus the additional number
     * of dummy bits that would be required for a real implementation to
     * memory. With this we simulate the correct behaviour with less
     * complexity. */
    struct BranchInfo {
        BranchInfo(Addr _pc, const PCStateBase &_target, ThreadID _tid)
            : pc(_pc), tid(_tid)
        {
            set(target, &_target);
        }

        BranchInfo()
            : pc(MaxAddr), target(nullptr), inst(nullptr), tid(0) {}

        /** The branch PC */
        Addr pc;

        /** The entry's target. */
        std::unique_ptr<PCStateBase> target;

        StaticInstPtr inst;
        ThreadID tid;
    };

    uint32_t getKey(Addr pc) {
        /** We hash the upper 48 bit and keep the lower 16 */
        uint32_t upper = (pc >> 16) ^ (pc >> 32) ^ (pc >> 48);
        return (pc & ((1<<16)-1)) | upper << 16;
    }

    std::map<Addr,BranchInfo> branchMap;
    // Buffer to remember which entries are stored in each cache line
    std::queue<std::pair<unsigned,std::list<Addr>>> records;
    std::list<Addr> curRecords;
    unsigned bits_ready;

    void writeCacheLine();

    // The interface to read and write blocks to memory
    prefetch::RecReplyMemInterface* memIF;

    uint8_t* write_data;
    uint8_t* read_data;

    /** Call back to receive data from memory */
    bool receiveRecord(uint8_t* data);

    // Helper to install branches in the BTB
    void installBranch(BranchInfo& bi);

    bool recording;
    bool replaying;
    bool pause_replay;

    void updateReplayTrottle();

  public:
    /** Record interface function */
    void startRecord();
    void stopRecord();

    /** Replay interface function */
    void startReplay();
    void stopReplay();



  private:
    struct ProcessQueueEntry {
        Addr pc;
        Tick processTick;
        ProcessQueueEntry(Addr x, Tick t) : pc(x), processTick(t) {};
    };

    std::deque<ProcessQueueEntry> processQueue;

    /** Event to handle the delay queue processing */
    void processQueueEventWrapper();
    EventFunctionWrapper processQueueEvent;

    /** Each of the meta data entries require a bit of decoding
     * also we cannot install all branches at once.
     */
    void insertIntoProcessQueue(Addr addr);
    void finishDecoding(Addr vaddr);

  protected:
    struct PrefetchBTBStats : public statistics::Group
    {
        PrefetchBTBStats(PrefetchBTB *parent);

        /** Stat for number of BTB lookups. */

        // statistics::Vector lookupType;
        /** Stat for number of BTB misses. */
        statistics::Scalar btbFillHitOnPf;
        statistics::Scalar btbFillHitOnDemand;
        statistics::Scalar btbMisses;
        statistics::Scalar btbFills;
        statistics::Vector btbFillTypes;

        statistics::Scalar takenPred;
        statistics::Scalar takenMiss;

        statistics::Scalar unused;
        statistics::Scalar useful;

        statistics::Scalar recordedBranches;
        statistics::Scalar recordedUsefulPf;
        statistics::Scalar recordedBranchesUnique;
        statistics::Scalar recordedLongTgt;
        statistics::Scalar recordedShortTgt;
        statistics::Scalar recordedFullPC;
        statistics::Scalar recordedDeltaPC;
        statistics::Scalar recordedDoubleShort;

        statistics::Distribution deltaPCTgt;
        statistics::Distribution deltaTgtPC;
        statistics::Distribution deltaPCPC;


    } prefetchStats;

};

}  // namespace branch_prediction
}  // namespace gem5

#endif  // __CPU_PRED_PREDECODER_BTB_HH__
