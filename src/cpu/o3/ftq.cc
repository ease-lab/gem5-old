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

#include "cpu/o3/ftq.hh"

#include <list>

#include "arch/generic/pcstate.hh"
#include "base/logging.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "debug/Fetch.hh"
// #include "debug/FTQ.hh"
#include "debug/BAC.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{

namespace o3
{


/** Fetch Target Methods */
FetchTarget::FetchTarget(ThreadID _tid, const PCStateBase &_start_pc, InstSeqNum seqNum)
    : tid(_tid), ftSeqNum(seqNum),
      bpu_history(nullptr), taken(false)
{
    set(startPC , _start_pc);
    _start_addr = startPC->instAddr();
}

// void
// Fetch::FetchTarget::addTerminalBranch(const DynInstPtr &inst)
// {
//     terminalBranch = inst;
//     endAddress = inst->pcState().instAddr();
// }

void
FetchTarget::addTerminal(const PCStateBase &br_pc,InstSeqNum seq,
                                bool _is_branch, bool pred_taken,
                                const PCStateBase &pred_pc)
{
    set(endPC, br_pc);
    set(predPC, pred_pc);
    _end_addr = endPC->instAddr();
    _pred_addr = predPC->instAddr();
    taken = pred_taken;
    is_branch = _is_branch;
}

// Fetch::FetchTarget::~FetchTarget()
// {

// }

std::string
FetchTarget::print()
{
    std::stringstream ss;
    ss << "FT[" << ftSeqNum << "]: [0x" << std::hex
        << startPC->instAddr() << "->0x" << endPC->instAddr()
        << "|B:" << is_branch
        // << "|T" << (is_branch ? endPC : "-")
        << "]";
    return ss.str();
}




FTQ::FTQ(CPU *_cpu, const BaseO3CPUParams &params)
    :
    // ftqPolicy(params.smtFTQPolicy),
      cpu(_cpu),
      numEntries(params.numFTQEntries),
      squashWidth(params.squashWidth),
      numFetchTargetsInFTQ(0),
      numThreads(params.numThreads),
      stats(_cpu)
{
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        maxEntries[tid] = numEntries;
    }

    for (ThreadID tid = numThreads; tid < MaxThreads; tid++) {
        maxEntries[tid] = 0;
    }

    resetState();
}

void
FTQ::resetState()
{
    for (ThreadID tid = 0; tid  < MaxThreads; tid++) {
        // threadEntries[tid] = 0;
        // squashIt[tid] = instList[tid].end();
        squashedSeqNum[tid] = 0;
        doneSquashing[tid] = true;
        ftq[tid].clear();
        ftqStatus[tid] = Valid;
    }
    numFetchTargetsInFTQ = 0;

    // Initialize the "universal" FTQ head & tail point to invalid
    // pointers

    // head = ftq[0].end();
    // tail = ftq[0].end();
}

std::string
FTQ::name() const
{
    return cpu->name() + ".ftq";
}

void
FTQ::drainSanityCheck() const
{
    // for (ThreadID tid = 0; tid  < numThreads; tid++)
    //     assert(ft[tid].empty());
    assert(isEmpty());
}

bool
FTQ::isDrained() const
{
    /* Make sure the FTQ is empty and the state of all threads is
     * idle.
     */
    for (ThreadID i = 0; i < numThreads; ++i) {
        // Verify FTQs are drained
        if (!ftq[i].empty())
            return false;
    }
    return true;
}


void
FTQ::takeOverFrom()
{
    resetState();
}




////////////////////////////////////////////////////////////////////////


// bool
// FTQ::updateFTQStatus(ThreadID tid)
// {
//     bool ret = false;
//     switch (ftqStatus[tid]) {
//     case FTQActive:
//         // if (ftq[tid].size() > ftqSize) {
//         //     DPRINTF(BAC, "[tid:%i] FTQ got unblocked.\n", tid);
//         //     ftqStatus[tid] = FTQActive;
//         //     ret = true;
//         // }
//         break;

//     case FTQSquash:
//         DPRINTF(BAC, "[tid:%i] Done squashing FTQ -> running.\n", tid);
//         ftqStatus[tid] = FTQActive;
//         ret = true;
//         break;

//     case FTQFull:
//         if (ftq[tid].size() < maxEntries[tid]) {
//             DPRINTF(BAC, "[tid:%i] FTQ got unblocked.\n", tid);
//             ftqStatus[tid] = FTQActive;
//             ret = true;
//         }
//         break;

//     case FTQInactive:
//         break;

//     default:
//         break;
//     }
//     return ret;
// }



void
FTQ::dumpFTQ(ThreadID tid) {
    int i = 0;
    for (auto& ft : ftq[tid]) {
        DPRINTF(BAC, "FTQ[tid:%i][%i]: %s.\n", tid, i, ft->print());
        i++;
    }
}

void
FTQ::insert(ThreadID tid, FetchTargetPtr fetchTarget)
{
    numFetchTargetsInFTQ++;
    ftq[tid].push_back(fetchTarget);

    DPRINTF(BAC, "Insert %s in FTQ[T:%i]. sz:%i\n",
                    fetchTarget->print(), tid, ftq[tid].size());

    stats.writes++;
}


void
FTQ::updateHead(ThreadID tid)
{
    DPRINTF(BAC, "Pop FT:[fn%llu] in FTQ[T:%i]. sz:%i\n",
                    ftq[tid].front()->ftNum(),
                    tid, ftq[tid].size()-1);

    numFetchTargetsInFTQ--;
    ftq[tid].pop_front();
    stats.reads++;
}


FetchTargetPtr
FTQ::readHead(ThreadID tid)
{
    if (ftqStatus[tid] == Invalid) return nullptr;
    if (ftq[tid].empty()) return nullptr;

    return ftq[tid].front();
}

bool
FTQ::isInHead(ThreadID tid, Addr pc) {

    if (ftqStatus[tid] == Invalid) return false;
    if (ftq[tid].empty()) return false;

    if (!ftq[tid].front()->inRange(pc)) {
        // DPRINTF(Fetch, "[tid:%i] PC:%#x not within FT!\n",
        //     tid, pc);
        return false;
    }
    return true;
}

bool
FTQ::isHeadReady(ThreadID tid)
{
    return (ftqStatus[tid] != Invalid) && (ftq[tid].size() > 0);
}

bool
FTQ::isValid(ThreadID tid)
{
    return ftqStatus[tid] != Invalid;
}

unsigned
FTQ::numFreeEntries(ThreadID tid)
{
    return maxEntries[tid] - ftq[tid].size();
}


unsigned
FTQ::size(ThreadID tid)
{
    return ftq[tid].size();
}

bool
FTQ::isFull(ThreadID tid)
{
    return ftq[tid].size() == maxEntries[tid];
}

bool
FTQ::isEmpty() const
{
    return numFetchTargetsInFTQ == 0;
}

bool
FTQ::isEmpty(ThreadID tid) const
{
    return ftq[tid].empty();
}


FTQ::FetchTargetIt
FTQ::tail(ThreadID tid)
{
    auto it = ftq[tid].end();
    if (ftq[tid].size() > 0)
        it--;
    return it;
}

FTQ::FetchTargetIt
FTQ::end(ThreadID tid)
{
    return ftq[tid].end();
}

FTQ::FetchTargetIt
FTQ::head(ThreadID tid)
{
    return ftq[tid].begin();
}

FTQ::FetchTargetIt
FTQ::begin(ThreadID tid)
{
    return ftq[tid].begin();
}


void
FTQ::squash(ThreadID tid)
{
    squashSanityCheck(tid);
    ftq[tid].clear();
    ftqStatus[tid] = Valid;
}

void
FTQ::squashSanityCheck(ThreadID tid)
{
    for (auto ft : ftq[tid]) {
        assert(ft->bpu_history == nullptr);
    }
}

void
FTQ::invalidate(ThreadID tid)
{
    /** Only a full ftq can be invalid*/
    if (!ftq[tid].empty())
        ftqStatus[tid] = Invalid;
}

void
FTQ::doSquash(ThreadID tid)
{

}



FTQ::FTQStats::FTQStats(statistics::Group *parent)
  : statistics::Group(parent, "ftq"),
    ADD_STAT(reads, statistics::units::Count::get(),
        "The number of FTQ reads"),
    ADD_STAT(writes, statistics::units::Count::get(),
        "The number of FTQ writes")
{
}

} // namespace o3
} // namespace gem5
