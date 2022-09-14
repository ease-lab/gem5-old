/*
 * Copyright (c) 2022 The University of Edinburgh
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


#ifndef __CPU_PRED_BTB_HH__
#define __CPU_PRED_BTB_HH__


#include "arch/generic/pcstate.hh"
#include "base/statistics.hh"
#include "config/the_isa.hh"
#include "cpu/static_inst.hh"
#include "enums/BranchClass.hh"
#include "params/BranchTargetBuffer.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace branch_prediction
{

class BranchTargetBuffer : public SimObject
{

  public:
    typedef BranchTargetBufferParams Params;
    typedef enums::BranchClass BranchClass;

    BranchTargetBuffer(const Params &params);

    virtual void reset() = 0;

    /** Looks up an address in the BTB. Must call valid() first on the address.
     *  @param inst_PC The address of the branch to look up.
     *  @return Returns the target of the branch.
     */
    virtual const PCStateBase *lookup(ThreadID tid, Addr instPC,
                            BranchClass type = BranchClass::NoBranch) = 0;

    /** Looks up an address in the BTB and return the instruction
     * information if existant. May not be supported in all BTBs.
     *  @param inst_PC The address of the branch to look up.
     *  @return Returns the target of the branch.
     */
    virtual const StaticInstPtr lookupInst(ThreadID tid, Addr instPC);

    /** Checks if a branch is in the BTB.
     *  @param inst_PC The address of the branch to look up.
     *  @param inst Optional passing in the branch type for better statistics.
     *  @return Whether or not the branch exists in the BTB.
     */
    virtual bool valid(ThreadID tid, Addr instPC,
                            BranchClass type = BranchClass::NoBranch) = 0;

    /** Updates the BTB with the target of a branch.
     *  @param inst_pc The address of the branch being updated.
     *  @param target_pc The target address of the branch.
     */
    virtual void update(ThreadID tid, Addr inst_pc,
                          const PCStateBase &target_pc,
                          BranchClass type = BranchClass::NoBranch) = 0;

    /** Update BTB statistics
     */
    virtual void incorrectTarget(BranchClass type = BranchClass::NoBranch)
    {
      stats.mispredict++;
      if (type != BranchClass::NoBranch) {
        stats.mispredictType[type]++;
      }
    }

    /** Number of the threads for which the branch history is maintained. */
    const unsigned numThreads;

    struct BranchTargetBufferStats : public statistics::Group
    {
        BranchTargetBufferStats(statistics::Group *parent);

        /** Stat for number of BTB lookups. */
        statistics::Scalar lookups;
        statistics::Vector lookupType;
        /** Stat for number of BTB misses. */
        statistics::Scalar misses;
        statistics::Vector missType;
        /** Stat for number for the ratio between BTB misses and lookups. */
        statistics::Formula missRatio;
        /** Stat for number of BTB updates. */
        statistics::Scalar updates;
        statistics::Vector updateType;
        /** Stat for number of BTB updates. */
        statistics::Scalar evictions;
        statistics::Vector evictionType;
        /** Stat for number BTB mispredictions.
         * No target found or target wrong */
        statistics::Scalar mispredict;
        statistics::Vector mispredictType;

    } stats;

};

} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_HH__
