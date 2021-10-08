/*
 * Copyright (c) 2021 Huawei Technologies Switzerland
 * All rights reserved
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

#ifndef __CPU_PERFECT_BP_HH__
#define __CPU_PERFECT_BP_HH__

#include <zlib.h>

#include <iterator>
#include <list>

#include "base/named.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/bpred_unit.hh"
#include "cpu/static_inst.hh"
#include "params/PerfectBP.hh"

namespace gem5
{
namespace branch_prediction
{
/**
 * PerfectBP implements a branch predictor oracle.
 * It is done by reading a trace from an earlier execution. This trace
 * contains the pc, upc and npc from all the taken branches, in order.
 * The trace is read from a compressed or uncompressed file into a list.
 *
 * The list contains either:
 *  - E: executed branch entry
 *  - N: not yet executed branch entry
 * EEEEEEEEEEEEEENNNNNNNNNNNN
 * |             |          |
 * begin     currBranch    end
 *            iterator
 *
 * The currBranch iterator points to the next executed branch entry.
 * On predict: if the fetched branch is taken, increment the iterator.
 * On update:  all committed instructions popped from the front.
 * On squash:  the iterator is rewinded to the last squashed branch.
 *
 */
class PerfectBP : public BPredUnit
{
  public:
    PerfectBP(const PerfectBPParams &params);

    /**
     * Returns whether or not the instructions is a taken branch, and set the
     * npc if it is taken. This is done based on a trace from an
     * earlier execution.
     * @param inst The branch instruction.
     * @param PC The predicted PC is passed back through this parameter.
     * @return Returns if the branch is taken or not.
     */
    bool predict(const StaticInstPtr &inst, const InstSeqNum &seq_num,
            PCStateBase &pc, ThreadID tid) override;

    /**
     * Rewind the trace until a given sequence number.
     * @param squashedSn The sequence number used. The currBranch iterator
     * will point to the last rewinded entry.
     */
    void squash(const InstSeqNum &squashed_sn, ThreadID tid) override;

    /**
     * Release committed entries in the trace list.
     * @param done_sn The sequence number from the latest committed instruction
     */
    void update(const InstSeqNum &done_sn, ThreadID tid) override;

    void drainSanityCheck() const override{};

    void
    squash(const InstSeqNum &squashed_sn, const PCStateBase &corr_target,
            bool actually_taken, ThreadID tid) override
    {
        squash(squashed_sn, tid);
    }

  private:
    /** File from which the trace is read. */
    gzFile traceBpFd;

    /** Set when the trace file is fully read. */
    bool traceFileDone = false;

    /**
     * Number of entries to fetch from the trace file. This fetch is done each
     * time the PerfectBP has consumed all previously fetched entries.
     */
    unsigned numberEntriesToFetch;

    /**
     * @struct BPTraceEntry
     * @brief Entry containing the trace information from taken branches.
     * @var seqNum The seqNum field is set when the entry is used. In case of a
     * rewind due to a squash, the seqNum entry will be cleared
     */
    struct BPTraceEntry
    {
        uint64_t pc;
        uint16_t upc;
        uint64_t npc;
        uint64_t nupc;
        uint64_t seqNum;
    };

    /** List with the fetched trace entries */
    std::list<BPTraceEntry> takenBranchesTrace;

    std::list<BPTraceEntry>::iterator currBranch;

    /** Fetch new entries. */
    void fetchNewEntries();

    /**
     * Tries to read one entry from the trace file.
     * Return false if the file is fully read. Otherwise return true.
     */
    bool fetchOneEntry();

    /**
     * Need to instantiate all pure virtual function from BPredUnit
     * None are used in this bpred.
     */
    void
    uncondBranch(ThreadID tid, Addr pc, void *&bp_history) override
    {}
    void
    squash(ThreadID tid, void *bp_history) override
    {}
    bool
    lookup(ThreadID tid, Addr instPC, void *&bp_history) override
    {
        return false;
    }
    void
    btbUpdate(ThreadID tid, Addr instPC, void *&bp_history) override
    {}
    void
    update(ThreadID tid, Addr instPC, bool taken, void *bp_history,
            bool squashed, const StaticInstPtr &inst, Addr corrTarget) override
    {}
};

} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PERFECT_BP_HH__
