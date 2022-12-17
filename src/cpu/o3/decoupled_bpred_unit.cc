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

#include "cpu/o3/decoupled_bpred_unit.hh"

#include <algorithm>

#include "base/logging.hh"
#include "base/named.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/inst_seq.hh"
#include "cpu/static_inst.hh"
#include "debug/Branch.hh"
#include "params/BaseO3CPU.hh"

using namespace gem5::branch_prediction;

namespace gem5
{

namespace o3
{

// BPredict::BPredict(const BPredictParams &params)
//     :
//     //   bpu(params.branchPred)
//     bpu(nullptr),
//     decoupedStats(this)
// {
// }
BPredict::BPredict(CPU *_cpu, const BaseO3CPUParams &params)
    :
    //   bpu(params.branchPred)
    cpu(_cpu),
    bpu(nullptr),
    preFetchPredHist(params.numThreads),
    ftqSize(params.ftq_size),
    globalFTSeqNum(0),
    decoupedStats(_cpu,this)
{
    for (int i = 0; i < MaxThreads; i++) {
        // pc[i].reset(params.isa[0]->newPCState());
        bpuPC[i].reset(params.isa[0]->newPCState());
        stalls[i] = {false, false};
    }
}

std::string
BPredict::name() const {
    return cpu->name() + ".decBpu";
}

// void
// BPredict::init() {
//     if (!bpu)
//         fatal("Branch predictor needs to be set!\n");
// }

// void
// BPredict::regProbePoints()
// {
//     ppFTQInsert = new ProbePointArg<DynInstPtr>(cpu->getProbeManager(),
//                                                         "FTQInsert");
// }

// void
// BPredict::setActiveThreads(std::list<ThreadID> *at_ptr)
// {
//     activeThreads = at_ptr;
// }


bool
BPredict::predict(const StaticInstPtr &inst, const InstSeqNum &seqNum,
                   PCStateBase &pc, ThreadID tid)
{
    /** Perform the prediction. */
    BPredUnit::PredictorHistory* bpu_history = nullptr;
    bool taken  = bpu->predict(inst, seqNum, pc, tid, bpu_history);

    assert(bpu_history!=nullptr);

    /** Push the record into the history buffer */
    preFetchPredHist[tid].push_front(bpu_history);

    DPRINTF(Branch,
            "[tid:%i] [sn:%llu] History entry added. "
            "preFetchPredHist.size(): %i\n",
            tid, seqNum, preFetchPredHist[tid].size());

    return taken;
}



bool
BPredict::updatePostFetch(PCStateBase &pc, const InstSeqNum &ftNum,
                            const InstSeqNum &seqNum,
                            const StaticInstPtr &inst, ThreadID tid)
{
    DPRINTF(Branch, "[tid:%i] Update after pre-decode [ftn:%llu] => [sn:%llu]\n"
                    , tid, ftNum, seqNum);
    BPredUnit::BranchClass brType = getBranchClass(inst);

    // Iterate from the back.
    auto hist = preFetchPredHist[tid].end();
    while (hist != preFetchPredHist[tid].begin()) {
        --hist;

    // for (auto& hist : predHist[tid]) {
        if ((*hist)->seqNum == ftNum) {

            DPRINTF(Branch, "[tid:%i] [ft:%llu] hit sn: %s ? %s\n"
                    , tid, seqNum,
                    toStr((*hist)->type),
                    toStr(brType));
            break;
        }
    }

    if ((hist != preFetchPredHist[tid].begin())
        && ((*hist)->type != brType)) {

        DPRINTF(Branch, "[tid:%i] [fn:%llu] Update static instr "
                        "fail: types dont match!\n", tid, ftNum);
        assert(false);
    }

    /** After pre-decode we know if an instruction actually
     * was a branch and the target of direct branches.
     * Hence, we can resteer unconditional direct branches
     * as well as returns.
     */
    bool resteer = false;
    bool hist_valid = false;
    if (hist == preFetchPredHist[tid].begin()) {
        DPRINTF(Branch, "[tid:%i] [fn:%llu] No history for PC:%#x, %s\n",
                tid, ftNum, pc, toStr(brType));

        // Direct unconditional branches can be resteered at this point if
        if ((inst->isDirectCtrl() && inst->isUncondCtrl())
            || (bpu->ras && inst->isReturn())) {
            resteer = true;
        }
        hist_valid = false;

    } else {
        if (!(*hist)->predTaken &&
            ((inst->isDirectCtrl() && inst->isUncondCtrl())
            || (bpu->ras && inst->isReturn()))) {
            resteer = true;
        }

        // Move the history to the real prediction history
        bpu->predHist[tid].push_front(*hist);
        preFetchPredHist[tid].erase(hist);
        hist = bpu->predHist[tid].begin();
        hist_valid = true;
        DPRINTF(Branch, "[tid:%i] [sn:%llu] Move history to main buffer after pre-decode"
                "\n", tid, seqNum);
        (*hist)->inst = inst;
        (*hist)->seqNum = seqNum;
    }


    if (resteer) {
        // First resteer then fix history
        DPRINTF(Branch, "[tid:%i] [sn:%llu] Squash FTQ\n", tid, seqNum);
        // TODO: check the indirect predictor and make it properly
        bpu->squash(ftNum, tid);
    }



    // We will mess up the indirect predictor if the seq number is
    // smaller than the fetch target number
    // assert(ftNum > seqNum);


    if (!hist_valid) {

        DPRINTF(Branch,
                "[tid:%i] [sn:%llu] No branch history. "
                "Create pred. history PC %s\n",
                tid, seqNum, pc);
        // Create a dummy PC with the current one
        // we dont care what the BP is predicting as
        // we fix it anyway in a moment.

        void *bp_history = NULL;
        void *indirect_history = NULL;

        if (bpu->iPred) {
            bpu->iPred->genIndirectInfo(tid, indirect_history);
        }


    // void *ras_history = NULL;

        if (inst->isUncondCtrl()) {
            DPRINTF(Branch, "[tid:%i] [sn:%llu] Unconditional control\n",
                tid,seqNum);
            // Tell the BP there was an unconditional branch.
            bpu->uncondBranch(tid, pc.instAddr(), bp_history);
        } else {
            // ++stats.condPredicted;
            // We can only resteer unconditional branches.
            // but we make a dummy prediction and delete change it to not
            // taken right away.
            bpu->lookup(tid, pc.instAddr(), bp_history);
            bpu->btbUpdate(tid, pc.instAddr(), bp_history);
        }

        // Create the history and insert in into the buffer.
        bpu->predHist[tid].emplace_front(
                    new BPredUnit::PredictorHistory(seqNum, pc.instAddr(),
                                        true, bp_history, indirect_history,
                                        nullptr, tid, brType, inst));
        hist = bpu->predHist[tid].begin();
        hist_valid = true;
        DPRINTF(Branch, "[tid:%i] [sn:%llu] Move history to main buffer after pre-decode"
                "\n", tid, seqNum);
        (*hist)->inst = inst;
        (*hist)->seqNum = seqNum;
        (*hist)->predTaken = false;
    }

    if (!resteer) {
        return (*hist)->predTaken;
    }

    std::unique_ptr<PCStateBase> target(pc.clone());


    // We can fix fix the RAS
    if (bpu->ras && inst->isReturn()) {
        (*hist)->wasReturn = true;

        // If it's a function return call, then look up the address
        // in the RAS.
        const PCStateBase *ras_top = bpu->ras->pop(tid, (*hist)->rasHistory);
        if (ras_top) {
            set(target, inst->buildRetPC(pc, *ras_top));
            // inst->buildPC()
            (*hist)->usedRAS = true;

            DPRINTF(Branch, "[tid:%i] [sn:%llu] Instruction %s is a "
                "return, RAS predicted target: %s\n",
                tid, seqNum, pc, *target);
        }
    }

    if (bpu->ras && inst->isCall()) {

        bpu->ras->push(tid, pc, (*hist)->rasHistory);

        // Record that it was a call so that the top RAS entry can
        // be popped off if the speculation is incorrect.
        (*hist)->wasCall = true;

        DPRINTF(Branch, "[tid:%i] [sn:%llu] Instruction %s was "
                "a call, adding to the RAS\n",
                tid, seqNum, pc);
    }

    // For returns we have the target.
    // Now we need to get it for the direct branches.
    if (inst->isDirectCtrl()) {
        set(target, *(inst->branchTarget(pc)));
    }

    DPRINTF(Branch, "[tid:%i] [sn:%llu] Post fetch correction: "
            "Resteered PC:%s %s target:%s\n",
                tid, seqNum, pc, toStr(brType), *target);

    (*hist)->target = target->instAddr();
    (*hist)->predTaken = true;
    set(pc, *target);


    // predHist[tid].push_front(hist);

    // DPRINTF(Branch,
    //         "[tid:%i] [sn:%llu] History entry added. "
    //         "predHist.size(): %i\n",
    //         tid, seqNum, predHist[tid].size());
    return (*hist)->predTaken;
}


BPredict::BPredictStats::BPredictStats(
                                    o3::CPU *cpu, BPredict *decBpu)
                                        // BPredict *decBpu)

    : statistics::Group(cpu, "decBpu"),
      ADD_STAT(fetchTargetsCreated, statistics::units::Count::get(),
              "Number of fetch targets created ")
{
    using namespace statistics;
}

} // namespace branch_prediction
} // namespace gem5
