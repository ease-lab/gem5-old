/*
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
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

#ifndef __CPU_PRED_ASSOCIATIVE_BTB_HH__
#define __CPU_PRED_ASSOCIATIVE_BTB_HH__

#include "base/logging.hh"
#include "base/types.hh"
#include "config/the_isa.hh"
#include "cpu/pred/btb.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include "params/AssociativeBTB.hh"

namespace gem5
{

namespace branch_prediction
{

class AssociativeBTB : public BranchTargetBuffer
{
  public:
    AssociativeBTB(const AssociativeBTBParams &params);

    void reset() override;
    const PCStateBase *lookup(ThreadID tid, Addr instPC,
                           BranchClass type = BranchClass::NoBranch) override;
    bool valid(ThreadID tid, Addr instPC,
                           BranchClass type = BranchClass::NoBranch) override;
    void update(ThreadID tid, Addr instPC, const PCStateBase &target_pc,
                           BranchClass type = BranchClass::NoBranch,
                           StaticInstPtr inst = nullptr) override;
    const StaticInstPtr lookupInst(ThreadID tid, Addr instPC) override;

  private:

    struct BTBEntry : public TaggedEntry
    {
        /** The entry's tag. */
        Addr tag = 0;

        /** The entry's target. */
        PCStateBase * target;

        /** The entry's thread id. */
        ThreadID tid;

        /** Whether or not the entry is valid. */
        bool valid = false;

        StaticInstPtr inst;
    };


    /** Returns the index into the BTB, based on the branch's PC.
     *  @param inst_PC The branch to look up.
     *  @return Returns the index into the BTB.
     */
    inline uint64_t getIndex(ThreadID tid, Addr instPC);


    /** The actual BTB. */
    AssociativeSet<BTBEntry> btb;

    /** The number of entries in the BTB. */
    const unsigned numEntries;

    /** The associativity of the BTB */
    const unsigned assoc;

    /** The number of tag bits per entry. */
    const unsigned tagBits;

    /** Number of bits to shift PC when calculating index. */
    uint64_t instShiftAmt;

    /** The number of BTB index bits and mask. */
    uint64_t idxBits;
    uint64_t idxMask;
};

} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_ASSOCIATIVE_BTB_HH__
