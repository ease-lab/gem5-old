/*
 * Copyright (c) 2011-2012, 2014 ARM Limited
 * Copyright (c) 2010 The University of Edinburgh
 * Copyright (c) 2012 Mark D. Hill and David A. Wood
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

#include "cpu/pred/bpred_unit.hh"

#include <algorithm>

#include "arch/generic/pcstate.hh"
#include "base/compiler.hh"
#include "base/trace.hh"
#include "debug/Branch.hh"

namespace gem5
{

namespace branch_prediction
{

BPredUnit::BPredUnit(const Params &params)
    : SimObject(params),
      numThreads(params.numThreads),
      predHist(numThreads),
      btb(params.BTB),
      ras(params.RAS),
      iPred(params.indirectBranchPred),
      stats(this,this),
      instShiftAmt(params.instShiftAmt),
      resetBTB(params.resetBTB),
      resetStart(params.resetStart),
      resetEnd(params.resetEnd)
{
}


probing::PMUUPtr
BPredUnit::pmuProbePoint(const char *name)
{
    probing::PMUUPtr ptr;
    ptr.reset(new probing::PMU(getProbeManager(), name));

    return ptr;
}

void
BPredUnit::regProbePoints()
{
    ppBranches = pmuProbePoint("Branches");
    ppMisses = pmuProbePoint("Misses");
}

void
BPredUnit::drainSanityCheck() const
{
    // We shouldn't have any outstanding requests when we resume from
    // a drained system.
    for ([[maybe_unused]] const auto& ph : predHist)
        assert(ph.empty());
}

bool
BPredUnit::predict(const StaticInstPtr &inst, const InstSeqNum &seqNum,
                   PCStateBase &pc, ThreadID tid)
{

    BranchClass brType = getBranchClass(inst);

    // Check the BTB if an entry was found. The BP can only detect branches
    // if they have an entry in the BTB
    stats.BTBLookups++;

    bool btb_hit = false;
    const PCStateBase * btb_target = btb->lookup(tid, pc.instAddr(), brType);
    if (btb_target) {
        btb_hit = true;
        ++stats.BTBHits;
    }


    // auto conditional = inst->isCondCtrl();
    // auto direct = inst->isDirectCtrl();
    // std::string type = "";

    // if (inst->isCall()) {
    //     type = "call";
    // } else if (inst->isReturn()) {
    //     type = "return";
    // } else if (inst->isUncondCtrl() || inst->isCondCtrl()) {
    //     type = "jump";
    // } else {
    //     type = "other";
    // }




    DPRINTF(Branch, "[tid:%i] [sn:%llu] BTB detect %s PC: %s\n",
            tid, seqNum, toStr(brType), pc);



    // See if branch predictor predicts taken.
    // If so, get its target addr either from the BTB or the RAS.
    // Save off record of branch stuff so the RAS can be fixed
    // up once it's done.

    bool pred_taken = false;

    std::unique_ptr<PCStateBase> target(pc.clone());

    stats.lookups[tid]++;
    stats.lookupType[tid][brType]++;

    ppBranches->notify(1);

    void *bp_history = NULL;
    void *indirect_history = NULL;
    void *ras_history = NULL;

    if (inst->isUncondCtrl()) {
        DPRINTF(Branch, "[tid:%i] [sn:%llu] Unconditional control\n",
            tid,seqNum);
        pred_taken = true;
        // Tell the BP there was an unconditional branch.
        uncondBranch(tid, pc.instAddr(), bp_history);
    } else {
        ++stats.condPredicted;
        pred_taken = lookup(tid, pc.instAddr(), bp_history);

        if (pred_taken) {
            ++stats.condPredictedTaken;
        }

        DPRINTF(Branch, "[tid:%i] [sn:%llu] "
                "Branch predictor predicted %i for PC %s\n",
                tid, seqNum,  pred_taken, pc);
    }

    const bool orig_pred_taken = pred_taken;
    if (iPred) {
        iPred->genIndirectInfo(tid, indirect_history);
    }

    DPRINTF(Branch,
            "[tid:%i] [sn:%llu] Creating prediction history for PC %s\n",
            tid, seqNum, pc);

    PredictorHistory predict_record(seqNum, pc.instAddr(), pred_taken,
                                    bp_history, indirect_history,
                                    nullptr, tid, brType, inst);

    if (inst->isUncondCtrl()) {
        predict_record.wasUncond = true;
    }

    if (orig_pred_taken) {
        stats.dirPredTakenType[tid][brType]++;
    } else {
        stats.dirPredNotTakenType[tid][brType]++;
    }


    // Now all the predictors did their job.
    // Get the target address.

    // Now lookup in the BTB or RAS.
    if (pred_taken) {
        // Use RAS for returns if available
        if (ras && inst->isReturn()) {
            predict_record.wasReturn = true;
            // If it's a function return call, then look up the address
            // in the RAS.
            const PCStateBase *ras_top = ras->pop(tid,
                                                  predict_record.rasHistory);
            if (ras_top) {
                set(target, inst->buildRetPC(pc, *ras_top));
                // inst->buildPC()
            predict_record.usedRAS = true;

                DPRINTF(Branch, "[tid:%i] [sn:%llu] Instruction %s is a "
                    "return, RAS predicted target: %s\n",
                    tid, seqNum, pc, *target);
            }
        }

        if (!predict_record.usedRAS) {
            if (inst->isCall()) {
                if (inst->isDirectCtrl()) {
                    ++stats.directCall;
                } else {
                    ++stats.indirectCall;
                }

                if (ras) {
                    ras->push(tid, pc, predict_record.rasHistory);

            // Record that it was a call so that the top RAS entry can
            // be popped off if the speculation is incorrect.
            predict_record.wasCall = true;

                    DPRINTF(Branch, "[tid:%i] [sn:%llu] Instruction %s was "
                            "a call, adding to the RAS\n",
                            tid, seqNum, pc);
                }
            }

            // Use indirect predictor for indirect branche/calls if available
            // not for returns.
            if (iPred && inst->isIndirectCtrl() && !inst->isReturn()) {
                predict_record.wasIndirect = true;
                ++stats.indirectLookups;
                //Consult indirect predictor on indirect control
                if (iPred->lookup(pc.instAddr(), *target, tid)) {
                    // Indirect predictor hit
                    ++stats.indirectHits;
                    DPRINTF(Branch,
                            "[tid:%i] [sn:%llu] Instruction %s predicted "
                            "indirect target is %s\n",
                            tid, seqNum, pc, *target);
                } else {
                    ++stats.indirectMisses;
                    pred_taken = false;
                    predict_record.predTaken = pred_taken;
                    DPRINTF(Branch,
                            "[tid:%i] [sn:%llu] Instruction %s no indirect "
                            "target\n",
                            tid, seqNum, pc);
                    // if (!inst->isCall() && !inst->isReturn()) {

                    // } else if (inst->isCall()
                    //            && !inst->isUncondCtrl()
                    //            && ras) {
                    //     ras->pop(tid);
                    //     predict_record.pushedRAS = false;
                    // }
                    inst->advancePC(*target);
                }
                iPred->recordIndirect(pc.instAddr(), target->instAddr(),
                        seqNum, tid);
            } else {

                // ++stats.BTBLookups;
                // Check BTB on direct branches
                if (btb_target) {
                    // ++stats.BTBHits;
                    predict_record.wasPredTakenBTBHit = true;
                    // If it's not a return, use the BTB to get target addr.
                    set(target, btb_target);
                    DPRINTF(Branch,
                            "[tid:%i] [sn:%llu] Instruction %s predicted "
                            "target is %s\n",
                            tid, seqNum, pc, *target);
                } else {
                    predict_record.wasPredTakenBTBMiss = true;
                    if (inst->isUncondCtrl()) {
                        ++stats.uncondBTBMiss;
                    }

                    DPRINTF(Branch, "[tid:%i] [sn:%llu] BTB doesn't have a "
                            "valid entry\n", tid, seqNum);
                    pred_taken = false;
                    predict_record.predTaken = pred_taken;
                    // The Direction of the branch predictor is altered
                    // because the BTB did not have an entry
                    // The predictor needs to be updated accordingly
                    if (!inst->isCall() && !inst->isReturn()) {
                        btbUpdate(tid, pc.instAddr(), bp_history);
                        DPRINTF(Branch,
                                "[tid:%i] [sn:%llu] btbUpdate "
                                "called for %s\n",
                                tid, seqNum, pc);
                    }
                    inst->advancePC(*target);
                }
            }

        }
    } else {
        if (inst->isReturn()) {
           predict_record.wasReturn = true;
        }
        inst->advancePC(*target);
    }
    predict_record.target = target->instAddr();

    set(pc, *target);

    if (iPred) {
        // Update the indirect predictor with the direction prediction
        // Note that this happens after indirect lookup, so it does not use
        // the new information
        // Note also that we use orig_pred_taken instead of pred_taken in
        // as this is the actual outcome of the direction prediction
        iPred->updateDirectionInfo(tid, orig_pred_taken);
    }

    predHist[tid].push_front(predict_record);

    DPRINTF(Branch,
            "[tid:%i] [sn:%llu] History entry added. "
            "predHist.size(): %i\n",
            tid, seqNum, predHist[tid].size());





    auto pc_addr = predict_record.pc;
    auto distance = pc_addr - predict_record.target;

    DPRINTF(Branch, "Branch | 0x%08x, "
        "%s, taken:%i, dist:%i, target: 0x%08x "
        "| %s.\n",
        pc_addr, toStr(brType), pred_taken,
        distance, predict_record.target,
        inst->disassemble(pc_addr));


    if (pred_taken) {
        stats.predTakenType[tid][brType]++;
    } else {
        stats.predNotTakenType[tid][brType]++;
    }


    return pred_taken;
}

void
BPredUnit::update(const InstSeqNum &done_sn, ThreadID tid)
{
    DPRINTF(Branch, "[tid:%i] Committing branches until "
            "sn:%llu]\n", tid, done_sn);

    while (!predHist[tid].empty() &&
           predHist[tid].back().seqNum <= done_sn) {

        auto hist_it = predHist[tid].rbegin();

        stats.commitType[tid][hist_it->type]++;
        DPRINTF(Branch, "[tid:%i] [sn:%llu] Commit: %s\n",
                    tid, done_sn,
                    toStr(hist_it->type));

        // Update the branch predictor with the correct results.
        update(tid, hist_it->pc,
                    hist_it->predTaken,
                    hist_it->bpHistory, false,
                    hist_it->inst,
                    hist_it->target);

        if (iPred) {
            iPred->commit(done_sn, tid, hist_it->indirectHistory);
        }

        if (ras) {
            ras->commit(tid, hist_it->predTaken,
                             hist_it->target,
                             hist_it->inst,
                             hist_it->rasHistory);
        }
        // btb->update(tid, hist_it->pc,
        //                  hist_it->target,
        //                  hist_it->type);

        predHist[tid].pop_back();
    }
}

void
BPredUnit::squash(const InstSeqNum &squashed_sn, ThreadID tid)
{
    History &pred_hist = predHist[tid];

    if (iPred) {
        iPred->squash(squashed_sn, tid);
    }

    while (!pred_hist.empty() &&
           pred_hist.front().seqNum > squashed_sn) {

        stats.squashType[tid][pred_hist.front().type]++;
        DPRINTF(Branch, "[tid:%i] [squash sn:%llu] Incorrect: %s\n",
                    tid, squashed_sn,
                    toStr(pred_hist.front().type));


        if (pred_hist.front().rasHistory) {
            assert(ras);

            DPRINTF(Branch, "[tid:%i] [squash sn:%llu] Incorrect call/return "
                    "PC %#x. Fix RAS.\n", tid, squashed_sn,
                    pred_hist.front().pc);

            ras->squash(tid, pred_hist.front().rasHistory);
        }

        // This call should delete the bpHistory.
        squash(tid, pred_hist.front().bpHistory);
        if (iPred) {
            iPred->deleteIndirectInfo(tid, pred_hist.front().indirectHistory);
        }

        DPRINTF(Branch, "[tid:%i] [squash sn:%llu] "
                "Removing history for [sn:%llu] "
                "PC %#x\n", tid, squashed_sn, pred_hist.front().seqNum,
                pred_hist.front().pc);


        pred_hist.pop_front();

        DPRINTF(Branch, "[tid:%i] [squash sn:%llu] predHist.size(): %i\n",
                tid, squashed_sn, predHist[tid].size());
    }
}



void
BPredUnit::squash(const InstSeqNum &squashed_sn,
                  const PCStateBase &corr_target,
                  bool actually_taken, ThreadID tid,
                  StaticInstPtr inst, const PCStateBase &pc)
{
    History &pred_hist = predHist[tid];

    DPRINTF(Branch, "[tid:%i] Squashing and dummy prediction for sequence "
            "number %i, setting target to %s\n",
            tid, squashed_sn, corr_target);

    // dump();

    // Squash All Branches AFTER this mispredicted branch
    squash(squashed_sn, tid);

    auto hist_it = pred_hist.begin();

    // If there's no history of this branch the BPU did not detect it.
    // we need to make a dummy prediction so that it created the history.
    if (hist_it == pred_hist.end() || hist_it->seqNum != squashed_sn) {
        DPRINTF(Branch, "No Branch history found for for sn %i. "
                    "Make dummy prediction to create one.\n",
                    pred_hist.front().seqNum);


        // Create a dummy PC with the current one
        // we dont care what the BP is predicting as
        // we fix it anyway in a moment.
        std::unique_ptr<PCStateBase> next_pc(pc.clone());
        predict(inst, squashed_sn, *next_pc, tid);
    }


    // Now do the actual squash with the function below
    squash(squashed_sn, corr_target, actually_taken, tid);

}


void
BPredUnit::squash(const InstSeqNum &squashed_sn,
                  const PCStateBase &corr_target,
                  bool actually_taken, ThreadID tid)
{
    // Now that we know that a branch was mispredicted, we need to undo
    // all the branches that have been seen up until this branch and
    // fix up everything.
    // NOTE: This should be call conceivably in 2 scenarios:
    // (1) After an branch is executed, it updates its status in the ROB
    //     The commit stage then checks the ROB update and sends a signal to
    //     the fetch stage to squash history after the mispredict
    // (2) In the decode stage, you can find out early if a unconditional
    //     PC-relative, branch was predicted incorrectly. If so, a signal
    //     to the fetch stage is sent to squash history after the mispredict

    History &pred_hist = predHist[tid];

    ++stats.condIncorrect;
    ppMisses->notify(1);




    DPRINTF(Branch, "[tid:%i] Squashing from sequence number %i, "
            "setting target to %s\n", tid, squashed_sn, corr_target);

    // Squash All Branches AFTER this mispredicted branch
    squash(squashed_sn, tid);


    auto hist_it = pred_hist.begin();

    // // If there's no history of this branch the BPU did not detect it.
    // // we need to make a dummy prediction so that it created the history.
    // if (hist_it == pred_hist.end() || hist_it->seqNum != squashed_sn) {
    //     DPRINTF(Branch, "No Branch history found for for sn %i. "
    //                 "Make dummy prediction to create one.\n",
    //                 pred_hist.front().seqNum);

    //     Addr pc =

    //     predict(inst, squashed_sn, inst->p)
    // }


    // If there's a squash due to a syscall, there may not be an entry
    // corresponding to the squash.  In that case, don't bother trying to
    // fix up the entry.
    if (!pred_hist.empty()) {



        //HistoryIt hist_it = find(pred_hist.begin(), pred_hist.end(),
        //                       squashed_sn);

        //assert(hist_it != pred_hist.end());
        if (pred_hist.front().seqNum != squashed_sn) {
            DPRINTF(Branch, "Front sn %i != Squash sn %i\n",
                    pred_hist.front().seqNum, squashed_sn);

            assert(pred_hist.front().seqNum == squashed_sn);
        }

        stats.mispredictType[tid][hist_it->type]++;
        DPRINTF(Branch, "[tid:%i] [squash sn:%llu] Incorrect: %s\n",
                    tid, squashed_sn, toStr(hist_it->type));


        if (hist_it->usedRAS) {
            ras->incorrect(hist_it->rasHistory);
            DPRINTF(Branch,
                    "[tid:%i] [squash sn:%llu] Incorrect RAS [sn:%llu]\n",
                    tid, squashed_sn, hist_it->seqNum);
        }

        if (hist_it->wasCall) {
            ++stats.mispredictCall;
            DPRINTF(Branch,
                    "[tid:%i] [squash sn:%llu] Incorrect Call [sn:%llu]\n",
                    tid, squashed_sn, hist_it->seqNum);
        }

        if (hist_it->wasUncond) {
            ++stats.mispredictUncond;
            DPRINTF(Branch,
                    "[tid:%i] [squash sn:%llu] Incorrect Unconditional "
                    "[sn:%llu]\n",
                    tid, squashed_sn, hist_it->seqNum);
        } else {
            ++stats.mispredictCond;
            DPRINTF(Branch,
                    "[tid:%i] [squash sn:%llu] Incorrect Conditional "
                    "[sn:%llu]\n",
                    tid, squashed_sn, hist_it->seqNum);
        }


        // There are separate functions for in-order and out-of-order
        // branch prediction, but not for update. Therefore, this
        // call should take into account that the mispredicted branch may
        // be on the wrong path (i.e., OoO execution), and that the counter
        // counter table(s) should not be updated. Thus, this call should
        // restore the state of the underlying predictor, for instance the
        // local/global histories. The counter tables will be updated when
        // the branch actually commits.

        // Remember the correct direction for the update at commit.
        pred_hist.front().predTaken = actually_taken;
        pred_hist.front().target = corr_target.instAddr();

        update(tid, (*hist_it).pc, actually_taken,
               pred_hist.front().bpHistory, true, pred_hist.front().inst,
               corr_target.instAddr());

        if (iPred) {
            iPred->changeDirectionPrediction(tid,
                pred_hist.front().indirectHistory, actually_taken);
        }


        // Handle RAS ------------------
        if (ras) {
        if (actually_taken) {
                // The branch was taken but the RAS was not updated
                // accordingly. Needs to be fixed.
                if (hist_it->wasCall && (hist_it->rasHistory == nullptr)) {
                    DPRINTF(Branch, "[tid:%i] [squash sn:%llu] "
                            "Incorrectly predicted call. Push call"
                            " RAS. [sn:%llu] PC: %#x\n", tid, squashed_sn,
                            hist_it->seqNum,
                            hist_it->pc);
                    ras->push(tid, corr_target, hist_it->rasHistory);
            }

                if (hist_it->wasReturn && (hist_it->rasHistory == nullptr)) {
                 DPRINTF(Branch, "[tid:%i] [squash sn:%llu] "
                        "Incorrectly predicted "
                        "return [sn:%llu] PC: %#x\n", tid, squashed_sn,
                        hist_it->seqNum,
                        hist_it->pc);
                    ras->pop(tid, hist_it->rasHistory);
                 hist_it->usedRAS = true;
                }

            } else if (hist_it->rasHistory != nullptr) {
                // The branch was not taken but the RAS was modified.
                // Needs to be fixed.
                ras->squash(tid, hist_it->rasHistory);
            }
        }


        if (actually_taken) {
            ++stats.NotTakenMispredicted;

            if (hist_it->wasPredTakenBTBMiss) {
                ++stats.predTakenBTBMiss;
            }

            if (hist_it->wasIndirect) {
                ++stats.indirectMispredicted;
                if (iPred) {
                    iPred->recordTarget(
                        hist_it->seqNum, pred_hist.front().indirectHistory,
                        corr_target, tid);
                }
            } else {
                btb->incorrectTarget(hist_it->pc, hist_it->type);
                ++stats.BTBMispredicted;
            }




                DPRINTF(Branch,"[tid:%i] [squash sn:%llu] "
                        "BTB Update called for [sn:%llu] "
                        "PC %#x\n", tid, squashed_sn,
                        hist_it->seqNum, hist_it->pc);

            btb->update(tid, hist_it->pc, corr_target,
                             hist_it->type, hist_it->inst);












        } else {
           //Actually not Taken
           ++stats.TakenMispredicted;
        //    if (hist_it->usedRAS) {
        //         assert(ras);
        //         DPRINTF(Branch,
        //                 "[tid:%i] [squash sn:%llu] Incorrectly predicted "
        //                 "return [sn:%llu] PC: %#x Restoring RAS\n", tid,
        //                 squashed_sn,
        //                 hist_it->seqNum, hist_it->pc);
        //         DPRINTF(Branch,
        //                 "[tid:%i] [squash sn:%llu] Restoring top of RAS "
        //                 "to: %i, target: %s\n", tid, squashed_sn,
        //                 hist_it->RASIndex, *hist_it->RASTarget);
        //         ras->restore(hist_it->RASIndex,
        //                      hist_it->RASTarget.get(), tid);
        //         hist_it->usedRAS = false;
        //    } else if (hist_it->wasCall && hist_it->pushedRAS) {
        //         assert(ras);
        //         //Was a Call but predicated false. Pop RAS here
        //         DPRINTF(Branch,
        //             "[tid:%i] [squash sn:%llu] "
        //             "Incorrectly predicted "
        //             "Call [sn:%llu] PC: %s Popping RAS\n",
        //             tid, squashed_sn,
        //             hist_it->seqNum, hist_it->pc);
        //         ras->pop(tid);
        //         hist_it->pushedRAS = false;
        //    }
        }
    } else {
        DPRINTF(Branch, "[tid:%i] [sn:%llu] pred_hist empty, can't "
                "update\n", tid, squashed_sn);
    }
}


enums::BranchClass
BPredUnit::getBranchClass(StaticInstPtr inst)
{
    if (inst->isReturn()) {
        return BranchClass::Return;
    }

    if (inst->isCall()) {
        return inst->isDirectCtrl()
                    ? BranchClass::CallDirect
                    : BranchClass::CallIndirect;
    }

    if (inst->isDirectCtrl()) {
        return inst->isCondCtrl()
                    ? BranchClass::DirectCond
                    : BranchClass::DirectUncond;
    }

    if (inst->isIndirectCtrl()) {
        return inst->isCondCtrl()
                    ? BranchClass::IndirectCond
                    : BranchClass::IndirectUncond;
    }
    return BranchClass::NoBranch;
}

void
BPredUnit::dump()
{
    int i = 0;
    for (const auto& ph : predHist) {
        if (!ph.empty()) {
            auto pred_hist_it = ph.begin();

            cprintf("predHist[%i].size(): %i\n", i++, ph.size());

            while (pred_hist_it != ph.end()) {
                cprintf("sn:%llu], PC:%#x, tid:%i, predTaken:%i, "
                        "bpHistory:%#x, rasHistory:%#x\n",
                        pred_hist_it->seqNum, pred_hist_it->pc,
                        pred_hist_it->tid, pred_hist_it->predTaken,
                        pred_hist_it->bpHistory, pred_hist_it->rasHistory);
                pred_hist_it++;
            }

            cprintf("\n");
        }
    }
}


BPredUnit::BPredUnitStats::BPredUnitStats(
                                statistics::Group *parent, BPredUnit *bp)
    : statistics::Group(parent),
      ADD_STAT(lookups, statistics::units::Count::get(),
              "Number of BP lookups"),
      ADD_STAT(lookupType, statistics::units::Count::get(),
              "Number of BP lookups per branch type"),
      ADD_STAT(predTakenType, statistics::units::Count::get(),
              "Number of final pred taken branches per branch type"),
      ADD_STAT(predNotTakenType, statistics::units::Count::get(),
              "Number of final pred not taken branches per branch type"),
      ADD_STAT(dirPredTakenType, statistics::units::Count::get(),
              "Number of direction pred as taken branches per br type"),
      ADD_STAT(dirPredNotTakenType, statistics::units::Count::get(),
              "Number of direction pred as not taken branches per br type"),

      ADD_STAT(squashType, statistics::units::Count::get(),
              "Number of branches squashed per branch type"),
      ADD_STAT(mispredictType, statistics::units::Count::get(),
              "Number of branches mispredicted per branch type"),
      ADD_STAT(commitType, statistics::units::Count::get(),
              "Number of branches commited per branch type"),

      ADD_STAT(condPredicted, statistics::units::Count::get(),
               "Number of conditional branches predicted"),
      ADD_STAT(condPredictedTaken, statistics::units::Count::get(),
               "Number of conditional branches predicted as taken"),
      ADD_STAT(condIncorrect, statistics::units::Count::get(),
               "Number of conditional branches incorrect"),
      ADD_STAT(BTBLookups, statistics::units::Count::get(),
               "Number of BTB lookups"),
      ADD_STAT(BTBHits, statistics::units::Count::get(),
               "Number of BTB hits"),
      ADD_STAT(BTBHitRatio, statistics::units::Ratio::get(), "BTB Hit Ratio",
               BTBHits / BTBLookups),
      ADD_STAT(BTBMispredicted, statistics::units::Count::get(),
               "Number BTB misspredictions. No target found or target wrong"),
      ADD_STAT(indirectLookups, statistics::units::Count::get(),
               "Number of indirect predictor lookups."),
      ADD_STAT(indirectHits, statistics::units::Count::get(),
               "Number of indirect target hits."),
      ADD_STAT(indirectMisses, statistics::units::Count::get(),
               "Number of indirect misses."),
      ADD_STAT(indirectMispredicted, statistics::units::Count::get(),
               "Number of mispredicted indirect branches."),
      ADD_STAT(indirectCall, statistics::units::Count::get(),
               "Number of conditional calls"),
      ADD_STAT(directCall, statistics::units::Count::get(),
               "Number of unconditional calls"),
      ADD_STAT(mispredictCall, statistics::units::Count::get(),
               "Number of calls mispredicted"),

      ADD_STAT(mispredictCond, statistics::units::Count::get(),
               "Number of conditional branches mispredicted"),
      ADD_STAT(mispredictUncond, statistics::units::Count::get(),
               "Number of unconditional branches mispredicted"),
      ADD_STAT(predTakenBTBMiss, statistics::units::Count::get(),
               "Number of branches predicted taken but miss in BTB"),
      ADD_STAT(uncondBTBMiss, statistics::units::Count::get(),
               "Number of unconditional branches miss in BTB"),

      ADD_STAT(NotTakenMispredicted, statistics::units::Count::get(),
               "Number branches predicted 'not taken' but turn out "
               "to be taken"),
      ADD_STAT(TakenMispredicted, statistics::units::Count::get(),
               "Number branches predicted taken but turn out to be not taken")
{
    using namespace statistics;
    BTBHitRatio.precision(6);

    lookups
        .init(bp->numThreads)
        .flags(total);

    lookupType
        .init(bp->numThreads, enums::Num_BranchClass)
        .flags(total | pdf);
    lookupType.ysubnames(enums::BranchClassStrings);

    predTakenType
        .init(bp->numThreads, enums::Num_BranchClass)
        .flags(total | pdf);
    predTakenType.ysubnames(enums::BranchClassStrings);

    predNotTakenType
        .init(bp->numThreads, enums::Num_BranchClass)
        .flags(total | pdf);
    predNotTakenType.ysubnames(enums::BranchClassStrings);

    dirPredTakenType
        .init(bp->numThreads, enums::Num_BranchClass)
        .flags(total | pdf);
    dirPredTakenType.ysubnames(enums::BranchClassStrings);

    dirPredNotTakenType
        .init(bp->numThreads, enums::Num_BranchClass)
        .flags(total | pdf);
    dirPredNotTakenType.ysubnames(enums::BranchClassStrings);

    squashType
        .init(bp->numThreads, enums::Num_BranchClass)
        .flags(total | pdf);
    squashType.ysubnames(enums::BranchClassStrings);

    mispredictType
        .init(bp->numThreads, enums::Num_BranchClass)
        .flags(total | pdf);
    mispredictType.ysubnames(enums::BranchClassStrings);

    commitType
        .init(bp->numThreads, enums::Num_BranchClass)
        .flags(total | pdf);
    commitType.ysubnames(enums::BranchClassStrings);
}

} // namespace branch_prediction
} // namespace gem5
