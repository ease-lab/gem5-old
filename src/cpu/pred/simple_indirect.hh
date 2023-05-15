/*
 * Copyright (c) 2014 ARM Limited
 * Copyright (c) 2023 The University of Edinburgh
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

#ifndef __CPU_PRED_INDIRECT_HH__
#define __CPU_PRED_INDIRECT_HH__

#include <deque>

#include "base/circular_queue.hh"
#include "base/statistics.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/indirect.hh"
#include "params/SimpleIndirectPredictor.hh"

namespace gem5
{

namespace branch_prediction
{

class SimpleIndirectPredictor : public IndirectPredictor
{
  public:
    SimpleIndirectPredictor(const SimpleIndirectPredictorParams &params);

    /** Indirect predictor interface */
    void reset() override;

    const PCStateBase * lookup(ThreadID tid, InstSeqNum sn,
                                Addr pc, void * &iHistory) override;
    void update(ThreadID tid, InstSeqNum sn, Addr pc, bool squash,
                bool taken, const PCStateBase& target,
                BranchClass brType, void * &iHistory) override;
    void squash(ThreadID tid, InstSeqNum sn, void * &iHistory) override;
    void commit(ThreadID tid, InstSeqNum sn, void * &iHistory) override;



    /** ------------------
     * The actual predictor
     * -------------------
     * */
  private:
    const bool hashGHR;
    const bool hashTargets;
    const bool takenHistory;
    const unsigned numSets;
    const unsigned numWays;
    const unsigned tagBits;
    const unsigned pathLength;
    const unsigned speculativePathLength;
    const unsigned instShift;
    const unsigned ghrNumBits;
    const unsigned ghrMask;

    struct IPredEntry
    {
        Addr tag = 0;
        std::unique_ptr<PCStateBase> target;
    };

    std::vector<std::vector<IPredEntry> > targetCache;



    struct HistoryEntry
    {
        HistoryEntry(Addr br_addr, Addr tgt_addr, InstSeqNum seq_num)
            : pcAddr(br_addr), targetAddr(tgt_addr), seqNum(seq_num) { }
        HistoryEntry() : pcAddr(0), targetAddr(0), seqNum(0) { }
        Addr pcAddr;
        Addr targetAddr;
        InstSeqNum seqNum;
    };

    using HistoryBuffer = CircularQueue<HistoryEntry>;

    /** Branch history information */
    struct IndirectHistory
    {
        /* data */
        Addr pcAddr;
        Addr targetAddr;
        InstSeqNum seqNum;

        Addr set_index;
        Addr tag;
        bool hit;
        unsigned ghr;
        uint64_t pathHist;
        HistoryBuffer::iterator histTail;

        bool dir_taken;
        bool was_indirect;

        IndirectHistory()
            : pcAddr(MaxAddr), targetAddr(MaxAddr),
              dir_taken(false), was_indirect(false)
        {}
    };

    struct PathHistoryRegister
    {
        unsigned numTgtBits;
        unsigned pathLength;
        unsigned instShift;
        uint64_t reg;
        void update(Addr target) {
            uint64_t v = (target >> instShift) & (numTgtBits-1);
            reg = (reg << numTgtBits) | v;
        }
    };

    struct ThreadInfo
    {
        std::deque<HistoryEntry> pathHist;
        HistoryBuffer indirectHist;
        HistoryBuffer::iterator histTail;
        unsigned headHistEntry = 0;
        // Global direction history register
        unsigned ghr = 0;
        // Path history register
        uint64_t phr = 0;
        ThreadInfo(size_t buffer_size)
          : indirectHist(buffer_size) { }
    };

    std::vector<ThreadInfo> threadInfo;
    std::vector<PathHistoryRegister> pathReg;


    // ---- Internal functions ----- //
    bool lookup(ThreadID tid, Addr br_addr,
                PCStateBase * &target, IndirectHistory * &history);
    void recordTarget(ThreadID tid, InstSeqNum sn,
                      const PCStateBase& target, IndirectHistory * &history);

    // Helper functions to generate and modify the
    // direction info
    void genIndirectInfo(ThreadID tid, void* &iHistory);
    void updateDirectionInfo(ThreadID tid, bool taken, Addr pc, Addr target);

    // Helper to compute set and tag
    inline Addr getSetIndex(Addr br_addr, ThreadID tid);
    inline Addr getTag(Addr br_addr);

    inline bool isIndirectNoReturn(BranchClass type) {
        return (type == BranchClass::CallIndirect) ||
               (type == BranchClass::IndirectUncond);
    }

  protected:
    struct IndirectStats : public statistics::Group
    {
        IndirectStats(statistics::Group *parent);
        // STATS
        statistics::Scalar lookups;
        statistics::Scalar hits;
        statistics::Scalar misses;
        statistics::Scalar targetRecords;
        statistics::Scalar indirectRecords;
        statistics::Scalar speculativeOverflows;

    } stats;
};

} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_INDIRECT_HH__
