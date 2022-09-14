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

#include "cpu/pred/btb.hh"

namespace gem5
{

namespace branch_prediction
{

BranchTargetBuffer::BranchTargetBuffer(const Params &params)
    : SimObject(params),
      numThreads(params.numThreads),
      stats(this)
{
}

const StaticInstPtr
BranchTargetBuffer::lookupInst(ThreadID tid, Addr instPC)
{
    panic("Not implemented for this BTB");
    return nullptr;
}

BranchTargetBuffer::BranchTargetBufferStats::BranchTargetBufferStats(
                                                statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(lookups, statistics::units::Count::get(),
               "Number of BTB lookups"),
      ADD_STAT(lookupType, statistics::units::Count::get(),
               "Number of BTB lookups per branch type"),
      ADD_STAT(misses, statistics::units::Count::get(),
               "Number of BTB misses"),
      ADD_STAT(missType, statistics::units::Count::get(),
               "Number of BTB misses per branch type"),
      ADD_STAT(missRatio, statistics::units::Ratio::get(), "BTB Hit Ratio",
               misses / lookups),
      ADD_STAT(updates, statistics::units::Count::get(),
               "Number of BTB updates"),
      ADD_STAT(updateType, statistics::units::Count::get(),
               "Number of BTB updates per branch type"),
      ADD_STAT(evictions, statistics::units::Count::get(),
               "Number of BTB evictions"),
      ADD_STAT(evictionType, statistics::units::Count::get(),
               "Number of BTB evictions per branch type"),
      ADD_STAT(mispredict, statistics::units::Count::get(),
               "Number of BTB mispredicts"),
      ADD_STAT(mispredictType, statistics::units::Count::get(),
               "Number of BTB mispredicts per branch type")
{
    using namespace statistics;
    missRatio.precision(6);
    lookupType
        .init(enums::Num_BranchClass)
        .flags(total | pdf);

    missType
        .init(enums::Num_BranchClass)
        .flags(total | pdf);

    updateType
        .init(enums::Num_BranchClass)
        .flags(total | pdf);

    evictionType
        .init(enums::Num_BranchClass)
        .flags(total | pdf);

    mispredictType
        .init(enums::Num_BranchClass)
        .flags(total | pdf);

    for (int i = 0; i < enums::Num_BranchClass; i++) {
        lookupType.subname(i, enums::BranchClassStrings[i]);
        missType.subname(i, enums::BranchClassStrings[i]);
        updateType.subname(i, enums::BranchClassStrings[i]);
        evictionType.subname(i, enums::BranchClassStrings[i]);
        mispredictType.subname(i, enums::BranchClassStrings[i]);
    }
}

} // namespace branch_prediction
} // namespace gem5
