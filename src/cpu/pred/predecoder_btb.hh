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

/** Implementation a pre-decoder for filling the BTB.
 */

#ifndef __CPU_PRED_PREDECODER_BTB_HH__
#define __CPU_PRED_PREDECODER_BTB_HH__

#include <deque>
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
#include "mem/packet.hh"
#include "mem/request.hh"
#include "params/PredecoderBTB.hh"
#include "sim/byteswap.hh"
#include "sim/clocked_object.hh"
#include "sim/probe/probe.hh"
#include "base/statistics.hh"


namespace gem5
{

struct PredecoderBTBParams;

namespace branch_prediction
{

class PredecoderBTB : public AssociativeBTB {

    typedef AssociativeBTB Base;

  public:
    PredecoderBTB(const PredecoderBTBParams &p);
    virtual ~PredecoderBTB();

    /**
     * Register probe points for this object.
     */
    void regProbeListeners() override;

    /** Helper to register the cache */
    void addListeners(BaseCache *_cache, BaseCPU *_cpu) {
        cache = _cache;
        cpu = _cpu;
    }

   private:

//    const PCStateBase *lookup(ThreadID tid, Addr instPC,
//                            BranchClass type = BranchClass::NoBranch) override;

    /** Override the update function to record a branch mapping. */
    void update(ThreadID tid, Addr instPC,
                        const PCStateBase &target_pc,
                        BranchClass type = BranchClass::NoBranch,
                        StaticInstPtr inst = nullptr,
                        unsigned inst_size = 0) override;


    /** Pointer to the I cache. */
    BaseCache *cache;

    /** Pointer to the CPU to listen for instructions */
    BaseCPU *cpu;

    /** The block size of the parent cache. */
    const unsigned blkSize;

    /** log_2(block size of the parent cache). */
    const unsigned lBlkSize;

    /** Only predecode on a fill brought into the cache by a prefetch*/
    const bool onlyPrefetchFill;

    /** Fill the BTB with all conditinal branches */
    const bool installConditional;

    /** Parameters for the taken predictor */
    const unsigned takenPredSize;

    /** Use the predecoder only to trace misses */
    const bool disablePredecode;

    /** Predecoding takes a bit of time.
     * The decode queue adds a delay before entries are available for
     * insertion.
     */
    const bool         decodeQueueEnabled;
    const unsigned int decodeQueueSize;
    const unsigned int decodeDelayTicks;



    /** Structures to fake the predecoding */

    typedef std::tuple<Addr,PCStateBase&,BranchClass> BTBProbeType;

    struct BranchInfo {
        BranchInfo(Addr _pc, const PCStateBase &_target, ThreadID _tid)
            : pc(_pc), tid(_tid) {
            set(target, &_target);
        }

        BranchInfo()
            : pc(MaxAddr), target(nullptr), inst(nullptr), tid(0),
              conditional(false), taken(false),
              type(BranchClass::NoBranch) {}

        /** The branch PC */
        Addr pc;

        /** The entry's target. */
        std::unique_ptr<PCStateBase> target;

        StaticInstPtr inst;

        /** The entry's thread id. */
        ThreadID tid;

        bool conditional;
        bool taken;

        /** Branch type */
        BranchClass type;

        // std::string print() {
        //   return std::string("["+)
        // }
        /** Use for eventual prefetcher */
        bool prefetched;
    };


    // struct PredecBTBEntry : public Base::BTBEntry
    // {
    //     bool predecoded = false;
    //     bool used = false;
    // };

    // AssociativeSet<PredecBTBEntry> predecEntries;


    /**
     * Notify functions when a branch gets retired.
     * and new cache lines are filled
     */
    // ProbeManager *manager;
    std::vector<ProbeListener *> listeners;

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

    // std::string printBranchClass(StaticInstPtr inst) const {
    //     auto type = getBranchClass(inst);
    //     return std::string(enums::BranchClassStrings[type]);
    // }


    using BranchList = std::vector<BranchInfo>;

    std::map<Addr, std::map<Addr, BranchInfo>> branchMap;
    std::map<Addr, std::map<Addr, BranchInfo>> targetMap;


    std::map<Addr, Addr> backwardTranslationMap;

    std::vector<uint8_t> takenPredictor;

    /** */
    void recordBranch(const Addr pc, const PCStateBase& target,
                    StaticInstPtr inst, BranchClass type, bool taken);

  protected:


    struct DecodeQueueEntry {
        Addr baseAddr;
        Tick processTick;

        DecodeQueueEntry(Addr x, Tick t) : baseAddr(x), processTick(t)
        {}
    };

    std::deque<DecodeQueueEntry> decodeQueue;

    /** Event to handle the delay queue processing */
    void decodeQueueEventWrapper();
    EventFunctionWrapper decodeQueueEvent;

    /** Insert the cache line that is filled in from the cache into the
     *  decode queue. This will trigger an event after the decode delay
     *  cycles pass.
     *  @param addr: address to insert into the delay queue
     */
    void insertIntoDecodeQueue(Addr addr);

    void finishDecoding(Addr vaddr);

    bool predTaken(BranchInfo& branch);
    void updateTaken(const o3::DynInstConstPtr& dynInst);
    unsigned index(Addr a) {
        return (a >> instShiftAmt) % takenPredSize;
    }

    /** Listen to cache acceess to get a tranlation map
     * TODO: Use the TLB for translations */
    void notifyAccess(const PacketPtr &pkt, bool miss);

    /** Listen to cache fills to start the predecoding */
    void notifyFill(const PacketPtr &pkt);

    /** Notify every instruction. Necessary to create an instruction map a
     * and fake the decoding */
    void notifyFetch(const o3::DynInstConstPtr& dynInst);


    /** Translation function */
    Addr translate(const PacketPtr &pkt);



    struct PredecoderBTBStats : public statistics::Group
    {
        PredecoderBTBStats(statistics::Group *parent);

        /** Stat for number of BTB lookups. */
        statistics::Scalar cacheFills;
        statistics::Scalar cacheFillsDemand;
        statistics::Scalar clsDecoded;

        // statistics::Vector lookupType;
        /** Stat for number of BTB misses. */
        statistics::Scalar btbHits;
        statistics::Scalar btbMisses;
        statistics::Scalar btbFills;
        statistics::Vector btbFillTypes;

        statistics::Scalar takenPred;
        statistics::Scalar takenMiss;

        statistics::Scalar unused;
        statistics::Scalar useful;

        statistics::Scalar recordedBranches;
        statistics::Scalar recordedBranchesUnique;



        // statistics::Vector missType;
        // /** Stat for number for the ratio between BTB misses and lookups. */
        // statistics::Formula missRatio;
        // /** Stat for number of BTB updates. */
        // statistics::Scalar updates;
        // statistics::Vector updateType;
        // /** Stat for number of BTB updates. */
        // statistics::Scalar evictions;
        // statistics::Vector evictionType;
        // /** Stat for number BTB mispredictions.
        //  * No target found or target wrong */
        // statistics::Scalar mispredict;
        // statistics::Vector mispredictType;

    } predecStats;

};

}  // namespace branch_prediction
}  // namespace gem5

#endif  // __CPU_PRED_PREDECODER_BTB_HH__
